// InputCapture.h
//
// Client-side input capture for the game streaming pipeline. Grabs
// keyboard and mouse through the Raw Input API and gamepad 0 through
// XInput, folds everything into the fixed 55-byte InputPacket v2 wire
// format (InputPacket.h), and ships it to the server through
// StreamReceiver::sendInput() at up to 120 Hz.
//
// Input sources and how they get here:
//   - Keyboard: Raw Input WM_INPUT, full 256-bit VK bitmask. Generic
//     VK_SHIFT/VK_CONTROL/VK_MENU are resolved to their left/right VKs
//     (scancode for shift, E0 flag for ctrl/alt) so the server can
//     distinguish modifiers.
//   - Mouse movement + buttons (incl. X1/X2): Raw Input. Absolute sources
//     (RDP, some KVMs) are differenced into relative deltas.
//   - Mouse WHEEL: NOT Raw Input. Windows Precision Touchpad two-finger
//     scrolling is synthesized as WM_MOUSEWHEEL/WM_MOUSEHWHEEL window
//     messages and never reaches the raw mouse stream, so the render
//     window forwards its wheel messages here via addWheel(). Real mice
//     generate the same window messages while the client window is
//     foreground, so nothing is lost -- and wheel events can no longer be
//     captured while the client is in the background (which is the
//     behaviour a user expects anyway).
//   - Absolute pointer (desktop mode): the render window maps the local
//     cursor position onto the video letterbox rect and pushes normalized
//     0..65535 coordinates via setAbsolutePointer(); setAbsoluteMouse()
//     switches the packet between relative deltas and absolute position
//     (InputFlags::kAbsoluteMouse). In absolute mode the raw-input deltas
//     are still accumulated but discarded at send time.
//
// Threading model:
//   - start()/stop() from the control thread. start() blocks until the
//     raw-input window is up (or failed); stop() pushes one final
//     all-released packet so the server never keeps a key held down.
//   - "Input-RawMsg" thread: owns a hidden message-only window registered
//     for WM_INPUT with RIDEV_INPUTSINK (keyboard/mouse state arrives
//     regardless of which window has focus). All state lands in atomics
//     shared with the send thread.
//   - "Input-Send" thread: unified 120 Hz (8.33 ms) timer, sharpened with
//     timeBeginPeriod(1). Each tick it polls XInput pad 0 (sending only
//     when dwPacketNumber changed; a disconnected pad is reprobed just
//     once a second because XInputGetState is slow for absent
//     controllers), applies the radial thumbstick deadzones, snapshots
//     the keyboard/mouse atomics (mouse deltas reset to 0 on every send),
//     and transmits when anything changed -- plus an unconditional
//     heartbeat packet every 120 ms. GetAsyncKeyState backs up the raw
//     key mask: keyups swallowed by a secure desktop (UAC, Ctrl+Alt+Del)
//     would otherwise stick keys forever.
//   - addWheel()/setAbsolutePointer()/setAbsoluteMouse() may be called
//     from any thread (the render thread, in this project).
//
// Requires: MSVC 2022, C++20. Links xinput.lib and winmm.lib via #pragma
// comment in the .cpp.

#pragma once

#include "InputPacket.h"
#include "StreamReceiver.h" // pulls winsock2.h before windows.h

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>

class InputCapture {
public:
    explicit InputCapture(StreamReceiver& receiver);
    ~InputCapture(); // calls stop() if still running

    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;

    // Creates the raw-input window and message thread, then the 120 Hz
    // send thread. Returns false with lastError() set if raw-input setup
    // failed (no threads left running in that case).
    bool start();

    // Sends a final all-released packet, then joins both threads. Safe to
    // call when not running.
    void stop();

    // Accumulates wheel movement (WHEEL_DELTA units, signed). Called by
    // the render window from WM_MOUSEWHEEL/WM_MOUSEHWHEEL -- the only
    // path that also carries precision-touchpad scrolling. Any thread.
    void addWheel(int vertical, int horizontal);

    // Publishes the local cursor position mapped onto the remote monitor
    // (0..65535 on both axes). Only used while absolute mode is on. Any
    // thread.
    void setAbsolutePointer(uint16_t nx, uint16_t ny);

    // Switches between relative deltas (game mode) and absolute pointer
    // (desktop mode). Any thread.
    void setAbsoluteMouse(bool absolute) {
        m_absoluteMouse.store(absolute, std::memory_order_relaxed);
    }
    bool absoluteMouse() const {
        return m_absoluteMouse.load(std::memory_order_relaxed);
    }

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // Most recent failure description; read it after start() reports false.
    std::string lastError() const;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void rawInputThreadProc(std::promise<bool> ready);
    void sendThreadProc();
    void handleRawInput(LPARAM lParam); // raw thread, from WM_INPUT
    void setError(std::string message);

    StreamReceiver& m_receiver;

    // --- shared input state (raw thread writes, send thread consumes) ---
    std::atomic<uint64_t> m_keys[4] = {};    // bit v = VK code v down
    std::atomic<int32_t>  m_mouseDx{0};      // accumulated since last send
    std::atomic<int32_t>  m_mouseDy{0};
    std::atomic<uint8_t>  m_mouseButtons{0}; // MouseButtons:: bits
    std::atomic<int32_t>  m_wheel{0};        // accumulated WHEEL_DELTA units
    std::atomic<int32_t>  m_wheelH{0};       // horizontal wheel, same units

    // --- absolute pointer (render thread writes, send thread consumes) ---
    std::atomic<uint32_t> m_absPointer{0};   // (ny << 16) | nx, both 0..65535
    std::atomic<bool>     m_absPointerValid{false};
    std::atomic<bool>     m_absoluteMouse{true};

    // --- absolute-source-mouse differencing (raw thread only) ---
    int  m_absX = 0;
    int  m_absY = 0;
    bool m_absPrimed = false;

    // --- raw-input window (raw thread only) ---
    HWND  m_hwnd = nullptr;
    DWORD m_rawThreadId = 0; // set before start() returns; stop() posts WM_QUIT to it

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;

    // --- thread control ---
    std::thread       m_rawThread;
    std::thread       m_sendThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};
