// InputInjector.cpp -- see InputInjector.h for the design overview.

#include "InputInjector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "extern/vigemclient/include/ViGEm/Client.h"

#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "extern/vigemclient/lib/x64/ViGEmClient.lib")
#pragma comment(lib, "setupapi.lib") // ViGEmClient.lib dependency

namespace {

// ---------------------------------------------------------------------------
// Logging. Same shape as the other modules: debugger + stderr, stack buffer
// only, called on errors and state changes -- never per packet (the two
// per-packet failure paths are counter-throttled).
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

std::string VigemErrorString(VIGEM_ERROR err)
{
    char buf[32];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(err));
    return buf;
}

// Keys whose scancode lives in the extended (E0) range: without
// KEYEVENTF_EXTENDEDKEY the navigation-cluster scancodes alias onto the
// numpad, right-hand modifiers onto the left-hand ones, and so on.
bool IsExtendedKey(uint8_t vk)
{
    switch (vk) {
    case 0x21: // VK_PRIOR (PgUp)
    case 0x22: // VK_NEXT  (PgDn)
    case 0x23: // VK_END
    case 0x24: // VK_HOME
    case 0x25: // VK_LEFT
    case 0x26: // VK_UP
    case 0x27: // VK_RIGHT
    case 0x28: // VK_DOWN
    case 0x2C: // VK_SNAPSHOT (PrintScreen)
    case 0x2D: // VK_INSERT
    case 0x2E: // VK_DELETE
    case 0x5B: // VK_LWIN
    case 0x5C: // VK_RWIN
    case 0x5D: // VK_APPS
    case 0x6F: // VK_DIVIDE (numpad /)
    case 0x90: // VK_NUMLOCK
    case 0xA3: // VK_RCONTROL
    case 0xA5: // VK_RMENU
        return true;
    default:
        return false;
    }
}

void FillKeyEvent(INPUT& in, uint8_t vk, bool down)
{
    in = {};
    in.type     = INPUT_KEYBOARD;
    in.ki.wVk   = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (in.ki.wScan != 0)
        in.ki.dwFlags |= KEYEVENTF_SCANCODE;
    if (IsExtendedKey(vk))
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
}

struct MouseButtonMap {
    uint8_t bit;
    DWORD   down;
    DWORD   up;
    DWORD   mouseData; // XBUTTON1/2 for the X buttons, else 0
};
constexpr MouseButtonMap kMouseButtonMap[] = {
    {MouseButtons::kLeft,   MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP,   0},
    {MouseButtons::kRight,  MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP,  0},
    {MouseButtons::kMiddle, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0},
    {MouseButtons::kX1,     MOUSEEVENTF_XDOWN,      MOUSEEVENTF_XUP,      XBUTTON1},
    {MouseButtons::kX2,     MOUSEEVENTF_XDOWN,      MOUSEEVENTF_XUP,      XBUTTON2},
};

// Worst case per packet: 256 key transitions + 1 mouse move + 5 button
// transitions + 2 wheel events. Stack array keeps the hot path
// allocation-free (~10 KB of stack, fine for the GSMR-Send thread).
constexpr UINT kMaxInjectEvents = 264;

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

InputInjector::~InputInjector()
{
    stop();
}

bool InputInjector::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running.load(std::memory_order_relaxed))
        return true;

    m_client = vigem_alloc();
    if (m_client == nullptr) {
        setError("vigem_alloc failed (out of memory)");
        LogF("[InputInjector] %s -- gamepad injection disabled",
             lastError().c_str());
    } else {
        const VIGEM_ERROR connectErr = vigem_connect(m_client);
        if (connectErr != VIGEM_ERROR_NONE) {
            setError("vigem_connect failed: " + VigemErrorString(connectErr) +
                     " (is the ViGEmBus driver installed?)");
            LogF("[InputInjector] %s -- gamepad injection disabled",
                 lastError().c_str());
            vigem_free(m_client);
            m_client = nullptr;
        } else {
            m_target = vigem_target_x360_alloc();
            const VIGEM_ERROR addErr =
                m_target ? vigem_target_add(m_client, m_target)
                         : VIGEM_ERROR_NONE;
            if (m_target == nullptr || addErr != VIGEM_ERROR_NONE) {
                setError(m_target == nullptr
                             ? "vigem_target_x360_alloc failed"
                             : "vigem_target_add failed: " + VigemErrorString(addErr));
                LogF("[InputInjector] %s -- gamepad injection disabled",
                     lastError().c_str());
                if (m_target) {
                    vigem_target_free(m_target);
                    m_target = nullptr;
                }
                vigem_disconnect(m_client);
                vigem_free(m_client);
                m_client = nullptr;
            } else {
                LogF("[InputInjector] virtual Xbox 360 pad plugged in");
            }
        }
    }

    for (uint64_t& word : m_prevKeys)
        word = 0;
    m_prevMouseButtons = 0;
    m_prevAbsValid     = false;
    m_prevPad          = {};
    m_sendInputFails   = 0;
    m_badPackets.store(0, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_release);
    LogF("[InputInjector] started (keyboard/mouse via SendInput%s)",
         m_target ? ", gamepad via ViGEm" : "; gamepad DISABLED");
    return true;
}

void InputInjector::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;

    // Release whatever the last packet left held down -- a client that
    // vanished mid-keypress must not leave the host with a stuck key.
    INPUT events[kMaxInjectEvents];
    UINT  count = 0;
    for (int w = 0; w < 4; ++w)
        for (uint64_t held = m_prevKeys[w]; held != 0; held &= held - 1)
            FillKeyEvent(events[count++],
                         static_cast<uint8_t>(w * 64 + std::countr_zero(held)), false);
    for (const auto& button : kMouseButtonMap) {
        if (!(m_prevMouseButtons & button.bit))
            continue;
        INPUT& in = events[count++];
        in = {};
        in.type         = INPUT_MOUSE;
        in.mi.dwFlags   = button.up;
        in.mi.mouseData = button.mouseData;
    }
    if (count > 0)
        SendInput(count, events, sizeof(INPUT));
    for (uint64_t& word : m_prevKeys)
        word = 0;
    m_prevMouseButtons = 0;
    m_prevAbsValid     = false;
    m_prevPad          = {};

    if (m_target != nullptr) {
        XUSB_REPORT neutral = {};
        vigem_target_x360_update(m_client, m_target, neutral); // best effort
        vigem_target_remove(m_client, m_target);
        vigem_target_free(m_target);
        m_target = nullptr;
    }
    if (m_client != nullptr) {
        vigem_disconnect(m_client);
        vigem_free(m_client);
        m_client = nullptr;
    }
    LogF("[InputInjector] stopped");
}

std::string InputInjector::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void InputInjector::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(message);
}

// ---------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------

bool InputInjector::inject(const std::vector<uint8_t>& rawPacket)
{
    InputPacket pkt;
    if (!deserialize(rawPacket.data(), rawPacket.size(), pkt)) {
        const uint32_t bad =
            m_badPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        if (bad == 1 || bad % 1000 == 0)
            LogF("[InputInjector] dropped malformed input packet "
                 "(%u so far, %zu bytes)",
                 bad, rawPacket.size());
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load(std::memory_order_relaxed))
        return false;

    // --- keyboard + mouse: one SendInput batch carrying only transitions.
    INPUT events[kMaxInjectEvents];
    UINT  count = 0;

    for (int w = 0; w < 4; ++w) {
        for (uint64_t diff = pkt.keys[w] ^ m_prevKeys[w]; diff != 0; diff &= diff - 1) {
            const int bit = std::countr_zero(diff);
            FillKeyEvent(events[count++], static_cast<uint8_t>(w * 64 + bit),
                         ((pkt.keys[w] >> bit) & 1ull) != 0);
        }
    }

    if (pkt.flags & InputFlags::kAbsoluteMouse) {
        // Desktop mode: normalized position on the primary monitor -- the
        // same monitor ScreenCapture duplicates, so client letterbox
        // coordinates land exactly where the user pointed.
        const uint16_t nx = static_cast<uint16_t>(pkt.mouseX);
        const uint16_t ny = static_cast<uint16_t>(pkt.mouseY);
        if (!m_prevAbsValid || nx != m_prevAbsX || ny != m_prevAbsY) {
            INPUT& in = events[count++];
            in = {};
            in.type       = INPUT_MOUSE;
            in.mi.dx      = nx;
            in.mi.dy      = ny;
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            m_prevAbsX     = nx;
            m_prevAbsY     = ny;
            m_prevAbsValid = true;
        }
    } else {
        m_prevAbsValid = false;
        if (pkt.mouseX != 0 || pkt.mouseY != 0) {
            INPUT& in = events[count++];
            in = {};
            in.type       = INPUT_MOUSE;
            in.mi.dx      = pkt.mouseX;
            in.mi.dy      = pkt.mouseY;
            in.mi.dwFlags = MOUSEEVENTF_MOVE; // relative delta
        }
    }

    if (pkt.wheelDelta != 0) {
        INPUT& in = events[count++];
        in = {};
        in.type         = INPUT_MOUSE;
        in.mi.mouseData = static_cast<DWORD>(pkt.wheelDelta);
        in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    }
    if (pkt.wheelDeltaH != 0) {
        INPUT& in = events[count++];
        in = {};
        in.type         = INPUT_MOUSE;
        in.mi.mouseData = static_cast<DWORD>(pkt.wheelDeltaH);
        in.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
    }

    const uint8_t buttonDiff = pkt.mouseButtons ^ m_prevMouseButtons;
    for (const auto& button : kMouseButtonMap) {
        if (!(buttonDiff & button.bit))
            continue;
        INPUT& in = events[count++];
        in = {};
        in.type         = INPUT_MOUSE;
        in.mi.dwFlags   = (pkt.mouseButtons & button.bit) ? button.down : button.up;
        in.mi.mouseData = button.mouseData;
    }

    if (count > 0) {
        const UINT sent = SendInput(count, events, sizeof(INPUT));
        if (sent != count) {
            // Typically UIPI: the foreground window is more privileged than
            // this process. Log throttled -- this can repeat at 120 Hz.
            ++m_sendInputFails;
            if (m_sendInputFails == 1 || m_sendInputFails % 1000 == 0)
                LogF("[InputInjector] SendInput injected %u of %u events "
                     "(%u failures so far): %s",
                     sent, count, m_sendInputFails,
                     Win32ErrorString(GetLastError()).c_str());
        }
    }

    for (int w = 0; w < 4; ++w)
        m_prevKeys[w] = pkt.keys[w];
    m_prevMouseButtons = pkt.mouseButtons;

    // --- gamepad: only touch the bus when the pad state actually changed
    // (input packets repeat as 120 ms heartbeats even when idle).
    if (m_target != nullptr) {
        const PadState pad{pkt.gamepadButtons, pkt.leftTrigger, pkt.rightTrigger,
                           pkt.thumbLX, pkt.thumbLY, pkt.thumbRX, pkt.thumbRY};
        if (!(pad == m_prevPad)) {
            XUSB_REPORT report = {};
            report.wButtons      = pad.buttons;
            report.bLeftTrigger  = pad.leftTrigger;
            report.bRightTrigger = pad.rightTrigger;
            report.sThumbLX      = pad.lx;
            report.sThumbLY      = pad.ly;
            report.sThumbRX      = pad.rx;
            report.sThumbRY      = pad.ry;
            const VIGEM_ERROR err =
                vigem_target_x360_update(m_client, m_target, report);
            if (err != VIGEM_ERROR_NONE) {
                ++m_sendInputFails;
                if (m_sendInputFails == 1 || m_sendInputFails % 1000 == 0)
                    LogF("[InputInjector] vigem_target_x360_update failed: %s",
                         VigemErrorString(err).c_str());
            }
            m_prevPad = pad;
        }
    }

    return true;
}
