// InputCapture.cpp -- see InputCapture.h for the design overview.

#include "InputCapture.h"

#include <Xinput.h>
#include <timeapi.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "winmm.lib") // timeBeginPeriod/timeEndPeriod

namespace {

// ---------------------------------------------------------------------------
// Logging. Same shape as the other modules: debugger + stderr, stack buffer
// only, called on errors and state changes -- never per packet.
// ---------------------------------------------------------------------------
void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), sizeof(buf) - 3, fmt, args);
    va_end(args);
    if (n < 0) n = static_cast<int>(strlen(buf));
    if (n == 0 || buf[n - 1] != '\n') {
        buf[n] = '\n';
        buf[n + 1] = '\0';
    }
    OutputDebugStringA(buf);
    fputs(buf, stderr);
}

std::string Win32ErrorString(DWORD err)
{
    char sysMsg[512] = {};
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        sysMsg, static_cast<DWORD>(sizeof(sysMsg)), nullptr);
    while (len > 0 && (sysMsg[len - 1] == '\r' || sysMsg[len - 1] == '\n'))
        sysMsg[--len] = '\0';

    char buf[600];
    if (len > 0)
        sprintf_s(buf, "%lu (%s)", err, sysMsg);
    else
        sprintf_s(buf, "%lu", err);
    return buf;
}

uint64_t SteadyMicros()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Zeroes a thumbstick that sits inside the circular deadzone. int64 math:
// two squared SHORTs can overflow int32.
void ApplyRadialDeadzone(SHORT& x, SHORT& y, int deadzone)
{
    const int64_t magSq = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y;
    if (magSq < static_cast<int64_t>(deadzone) * deadzone) {
        x = 0;
        y = 0;
    }
}

// Raw input reports the generic VK_SHIFT/VK_CONTROL/VK_MENU; the wire
// format carries the specific left/right key so the server can replay it
// faithfully. Shift is disambiguated by scancode, ctrl/alt by the E0 flag.
uint8_t ResolveVk(const RAWKEYBOARD& kb)
{
    const bool e0 = (kb.Flags & RI_KEY_E0) != 0;
    switch (kb.VKey) {
    case VK_SHIFT:
        return static_cast<uint8_t>(
            MapVirtualKeyW(kb.MakeCode, MAPVK_VSC_TO_VK_EX)); // VK_LSHIFT/VK_RSHIFT
    case VK_CONTROL:
        return e0 ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return e0 ? VK_RMENU : VK_LMENU;
    default:
        return static_cast<uint8_t>(kb.VKey);
    }
}

constexpr wchar_t kWndClassName[] = L"GSMR-InputCaptureWnd";

constexpr uint64_t kHeartbeatUs = 120'000;   // null-packet keepalive interval
constexpr uint64_t kPadRetryUs  = 1'000'000; // disconnected-pad reprobe interval

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

InputCapture::InputCapture(StreamReceiver& receiver)
    : m_receiver(receiver)
{
}

InputCapture::~InputCapture()
{
    stop();
}

bool InputCapture::start()
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    m_stopRequested.store(false, std::memory_order_release);
    for (auto& word : m_keys)
        word.store(0, std::memory_order_relaxed);
    m_mouseDx.store(0, std::memory_order_relaxed);
    m_mouseDy.store(0, std::memory_order_relaxed);
    m_mouseButtons.store(0, std::memory_order_relaxed);
    m_wheel.store(0, std::memory_order_relaxed);
    m_wheelH.store(0, std::memory_order_relaxed);
    m_absPointerValid.store(false, std::memory_order_relaxed);
    m_absPrimed = false;

    std::promise<bool> ready;
    std::future<bool> readyResult = ready.get_future();
    m_rawThread = std::thread(&InputCapture::rawInputThreadProc, this, std::move(ready));

    if (!readyResult.get()) {
        m_rawThread.join();
        LogF("[InputCapture] %s", lastError().c_str());
        return false;
    }

    m_sendThread = std::thread(&InputCapture::sendThreadProc, this);
    m_running.store(true, std::memory_order_release);
    LogF("[InputCapture] started (raw input + XInput, 120 Hz, %s mouse)",
         absoluteMouse() ? "absolute" : "relative");
    return true;
}

void InputCapture::stop()
{
    if (!m_rawThread.joinable() && !m_sendThread.joinable())
        return;

    m_stopRequested.store(true, std::memory_order_release);

    // The send thread pushes the all-released packet on its way out, so
    // join it while the receiver may still be running.
    if (m_sendThread.joinable())
        m_sendThread.join();

    if (m_rawThread.joinable()) {
        PostThreadMessageW(m_rawThreadId, WM_QUIT, 0, 0);
        m_rawThread.join();
    }

    m_running.store(false, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
    LogF("[InputCapture] stopped");
}

void InputCapture::addWheel(int vertical, int horizontal)
{
    if (vertical != 0)
        m_wheel.fetch_add(vertical, std::memory_order_relaxed);
    if (horizontal != 0)
        m_wheelH.fetch_add(horizontal, std::memory_order_relaxed);
}

void InputCapture::setAbsolutePointer(uint16_t nx, uint16_t ny)
{
    m_absPointer.store((static_cast<uint32_t>(ny) << 16) | nx,
                       std::memory_order_relaxed);
    m_absPointerValid.store(true, std::memory_order_release);
}

std::string InputCapture::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void InputCapture::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(message);
}

// ---------------------------------------------------------------------------
// Raw input thread: hidden message-only window + WM_INPUT pump
// ---------------------------------------------------------------------------

LRESULT CALLBACK InputCapture::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    } else if (msg == WM_INPUT) {
        auto* self = reinterpret_cast<InputCapture*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self)
            self->handleRawInput(lParam);
        // fall through: DefWindowProc performs the WM_INPUT buffer cleanup
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void InputCapture::rawInputThreadProc(std::promise<bool> ready)
{
    SetThreadDescription(GetCurrentThread(), L"Input-RawMsg");
    m_rawThreadId = GetCurrentThreadId();

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &InputCapture::wndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        setError("RegisterClassEx failed: " + Win32ErrorString(GetLastError()));
        ready.set_value(false);
        return;
    }

    m_hwnd = CreateWindowExW(0, kWndClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (!m_hwnd) {
        setError("CreateWindowEx(HWND_MESSAGE) failed: " +
                 Win32ErrorString(GetLastError()));
        UnregisterClassW(kWndClassName, wc.hInstance);
        ready.set_value(false);
        return;
    }

    // INPUTSINK delivers input regardless of focus -- the streaming client
    // window does not need to be foreground for capture to work.
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01; // generic desktop
    rid[0].usUsage     = 0x06; // keyboard
    rid[0].dwFlags     = RIDEV_INPUTSINK;
    rid[0].hwndTarget  = m_hwnd;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage     = 0x02; // mouse
    rid[1].dwFlags     = RIDEV_INPUTSINK;
    rid[1].hwndTarget  = m_hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        setError("RegisterRawInputDevices failed: " +
                 Win32ErrorString(GetLastError()));
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        UnregisterClassW(kWndClassName, wc.hInstance);
        ready.set_value(false);
        return;
    }

    ready.set_value(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        DispatchMessageW(&msg);

    rid[0].dwFlags    = RIDEV_REMOVE;
    rid[0].hwndTarget = nullptr;
    rid[1].dwFlags    = RIDEV_REMOVE;
    rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    UnregisterClassW(kWndClassName, wc.hInstance);
}

void InputCapture::handleRawInput(LPARAM lParam)
{
    // Keyboard and mouse RAWINPUTs are small and fixed-size; anything that
    // doesn't fit (HID gadgets we never registered for) is ignored.
    alignas(8) uint8_t buffer[sizeof(RAWINPUT) + 32];
    UINT size = sizeof(buffer);
    const UINT copied = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                                        RID_INPUT, buffer, &size,
                                        sizeof(RAWINPUTHEADER));
    if (copied == static_cast<UINT>(-1) || copied < sizeof(RAWINPUTHEADER))
        return;

    const RAWINPUT& ri = *reinterpret_cast<const RAWINPUT*>(buffer);

    if (ri.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = ri.data.keyboard;
        if (kb.VKey == 0 || kb.VKey >= 0xFF)
            return; // 0xFF marks fake/overrun keys
        const uint8_t vk = ResolveVk(kb);
        if (vk == 0)
            return;
        // F1/F2 istemci-yerel kısayollardır (istatistik / fare modu);
        // sunucuya iletilirse hem yerelde hem uzakta etki eder.
        if (vk == VK_F1 || vk == VK_F2)
            return;
        std::atomic<uint64_t>& word = m_keys[vk >> 6];
        const uint64_t mask = 1ull << (vk & 63);
        if (kb.Flags & RI_KEY_BREAK)
            word.fetch_and(~mask, std::memory_order_relaxed);
        else
            word.fetch_or(mask, std::memory_order_relaxed);
        return;
    }

    if (ri.header.dwType != RIM_TYPEMOUSE)
        return;

    const RAWMOUSE& mouse = ri.data.mouse;

    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Absolute sources (RDP, touch, some KVMs) report normalized
        // 0..65535 coordinates; difference successive samples into the
        // same relative-delta stream the wire format carries.
        const bool virtualDesktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int width  = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        const int x = MulDiv(mouse.lLastX, width, 65535);
        const int y = MulDiv(mouse.lLastY, height, 65535);
        if (m_absPrimed) {
            if (x != m_absX)
                m_mouseDx.fetch_add(x - m_absX, std::memory_order_relaxed);
            if (y != m_absY)
                m_mouseDy.fetch_add(y - m_absY, std::memory_order_relaxed);
        }
        m_absX = x;
        m_absY = y;
        m_absPrimed = true;
    } else if (mouse.lLastX != 0 || mouse.lLastY != 0) {
        m_mouseDx.fetch_add(mouse.lLastX, std::memory_order_relaxed);
        m_mouseDy.fetch_add(mouse.lLastY, std::memory_order_relaxed);
    }

    const USHORT flags = mouse.usButtonFlags;
    struct ButtonFlagMap {
        USHORT  down, up;
        uint8_t bit;
    };
    static constexpr ButtonFlagMap kButtonFlags[] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN,   RI_MOUSE_LEFT_BUTTON_UP,   MouseButtons::kLeft},
        {RI_MOUSE_RIGHT_BUTTON_DOWN,  RI_MOUSE_RIGHT_BUTTON_UP,  MouseButtons::kRight},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, MouseButtons::kMiddle},
        {RI_MOUSE_BUTTON_4_DOWN,      RI_MOUSE_BUTTON_4_UP,      MouseButtons::kX1},
        {RI_MOUSE_BUTTON_5_DOWN,      RI_MOUSE_BUTTON_5_UP,      MouseButtons::kX2},
    };
    for (const auto& map : kButtonFlags) {
        if (flags & map.down)
            m_mouseButtons.fetch_or(map.bit, std::memory_order_relaxed);
        if (flags & map.up)
            m_mouseButtons.fetch_and(static_cast<uint8_t>(~map.bit),
                                     std::memory_order_relaxed);
    }

    // Wheel deliberately NOT handled here: precision-touchpad scrolling
    // never reaches the raw mouse stream, so ALL wheel input (mouse and
    // touchpad alike) arrives through the render window's WM_MOUSEWHEEL /
    // WM_MOUSEHWHEEL messages and addWheel(). Handling both paths would
    // double-scroll for real mice.
}

// ---------------------------------------------------------------------------
// Send thread: unified 120 Hz timer -- XInput poll + packet assembly
// ---------------------------------------------------------------------------

void InputCapture::sendThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"Input-Send");

    // 8.33 ms ticks need better than the default 15.6 ms scheduler quantum.
    timeBeginPeriod(1);

    using clock = std::chrono::steady_clock;
    constexpr auto kTick = std::chrono::microseconds(1'000'000 / 120);

    std::vector<uint8_t> wire(kInputPacketSize);

    // Gamepad state (send thread only).
    bool     padConnected = false;
    DWORD    padPacketNo  = 0;
    uint64_t padRetryAtUs = 0;
    uint16_t padButtons   = 0;
    uint8_t  padLT = 0, padRT = 0;
    int16_t  padLX = 0, padLY = 0, padRX = 0, padRY = 0;

    uint64_t lastSendUs  = 0;
    uint64_t prevKeys[4] = {};
    uint8_t  prevButtons = 0;
    uint32_t prevAbs     = 0;
    bool     prevAbsSent = false;

    auto next = clock::now() + kTick;
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_until(next);
        next += kTick;
        if (const auto now = clock::now(); next < now)
            next = now + kTick; // fell behind (suspend/debugger); don't burst

        const uint64_t nowUs = SteadyMicros();

        // --- gamepad. XInputGetState is expensive for absent controllers,
        // so a disconnected pad is reprobed only once a second.
        bool padChanged = false;
        if (padConnected || nowUs >= padRetryAtUs) {
            XINPUT_STATE state = {};
            if (XInputGetState(0, &state) == ERROR_SUCCESS) {
                if (!padConnected) {
                    padConnected = true;
                    padChanged   = true;
                    LogF("[InputCapture] gamepad 0 connected");
                }
                if (state.dwPacketNumber != padPacketNo) {
                    padPacketNo = state.dwPacketNumber;
                    padChanged  = true;
                }
                if (padChanged) {
                    SHORT lx = state.Gamepad.sThumbLX, ly = state.Gamepad.sThumbLY;
                    SHORT rx = state.Gamepad.sThumbRX, ry = state.Gamepad.sThumbRY;
                    ApplyRadialDeadzone(lx, ly, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                    ApplyRadialDeadzone(rx, ry, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                    padButtons = state.Gamepad.wButtons;
                    padLT      = state.Gamepad.bLeftTrigger;
                    padRT      = state.Gamepad.bRightTrigger;
                    padLX = lx; padLY = ly; padRX = rx; padRY = ry;
                }
            } else {
                if (padConnected) {
                    padConnected = false;
                    padChanged   = true; // push the neutral state once
                    padButtons   = 0;
                    padLT = padRT = 0;
                    padLX = padLY = padRX = padRY = 0;
                    LogF("[InputCapture] gamepad 0 disconnected");
                }
                padRetryAtUs = nowUs + kPadRetryUs;
            }
        }

        // --- keyboard. Raw input is the source of truth; GetAsyncKeyState
        // backs it up for keyups swallowed while input went to a secure
        // desktop (UAC, Ctrl+Alt+Del), which would otherwise stick keys.
        uint64_t keys[4];
        bool keysChanged = false;
        for (int w = 0; w < 4; ++w) {
            keys[w] = m_keys[w].load(std::memory_order_relaxed);
            uint64_t stuck = 0;
            for (uint64_t scan = keys[w]; scan != 0; scan &= scan - 1) {
                const int vk = w * 64 + std::countr_zero(scan);
                if (!(GetAsyncKeyState(vk) & 0x8000))
                    stuck |= 1ull << (vk & 63);
            }
            if (stuck) {
                m_keys[w].fetch_and(~stuck, std::memory_order_relaxed);
                keys[w] &= ~stuck;
            }
            keysChanged |= keys[w] != prevKeys[w];
        }

        // --- mouse: consume the accumulated deltas.
        const int32_t dx32    = m_mouseDx.exchange(0, std::memory_order_relaxed);
        const int32_t dy32    = m_mouseDy.exchange(0, std::memory_order_relaxed);
        const int32_t wheel   = m_wheel.exchange(0, std::memory_order_relaxed);
        const int32_t wheelH  = m_wheelH.exchange(0, std::memory_order_relaxed);
        const uint8_t buttons = m_mouseButtons.load(std::memory_order_relaxed);

        const bool absolute = m_absoluteMouse.load(std::memory_order_relaxed) &&
                              m_absPointerValid.load(std::memory_order_acquire);
        const uint32_t abs  = m_absPointer.load(std::memory_order_relaxed);

        const bool mouseMoved = absolute ? (!prevAbsSent || abs != prevAbs)
                                         : (dx32 != 0 || dy32 != 0);
        const bool changed = padChanged || mouseMoved ||
                             wheel != 0 || wheelH != 0 ||
                             keysChanged || buttons != prevButtons;
        if (!changed && nowUs - lastSendUs < kHeartbeatUs)
            continue;

        InputPacket pkt;
        for (int w = 0; w < 4; ++w)
            pkt.keys[w] = keys[w];
        if (absolute) {
            pkt.flags  = InputFlags::kAbsoluteMouse;
            pkt.mouseX = static_cast<int16_t>(abs & 0xFFFF);         // u16 payload
            pkt.mouseY = static_cast<int16_t>((abs >> 16) & 0xFFFF); // u16 payload
        } else {
            pkt.mouseX = static_cast<int16_t>(std::clamp(dx32, -32768, 32767));
            pkt.mouseY = static_cast<int16_t>(std::clamp(dy32, -32768, 32767));
        }
        pkt.wheelDelta     = static_cast<int16_t>(std::clamp(wheel, -32768, 32767));
        pkt.wheelDeltaH    = static_cast<int16_t>(std::clamp(wheelH, -32768, 32767));
        pkt.mouseButtons   = buttons;
        pkt.gamepadButtons = padButtons;
        pkt.thumbLX        = padLX;
        pkt.thumbLY        = padLY;
        pkt.thumbRX        = padRX;
        pkt.thumbRY        = padRY;
        pkt.leftTrigger    = padLT;
        pkt.rightTrigger   = padRT;

        serialize(pkt, wire.data());
        m_receiver.sendInput(wire);

        lastSendUs  = nowUs;
        for (int w = 0; w < 4; ++w)
            prevKeys[w] = keys[w];
        prevButtons = buttons;
        if (absolute) {
            prevAbs     = abs;
            prevAbsSent = true;
        } else {
            prevAbsSent = false; // re-send position after a mode round-trip
        }
    }

    // Tell the server everything is released, so quitting the client can
    // never leave a key or button held down in the game.
    const InputPacket neutral;
    serialize(neutral, wire.data());
    m_receiver.sendInput(wire);

    timeEndPeriod(1);
}
