// InputInjector.h
//
// Server-side input injection for the game streaming pipeline. Consumes
// the 55-byte InputPacket v2 payloads that InputCapture sends (delivered
// by StreamSender::onInputPacket), replays keyboard and mouse through
// SendInput(), and mirrors the gamepad onto a virtual Xbox 360 controller
// through the ViGEm bus.
//
// Wiring -- inject() is thread-safe, the callback fires on GSMR-Send:
//
//   InputInjector injector;
//   injector.start();
//   sender.onInputPacket = [&](std::vector<uint8_t> payload, uint64_t) {
//       injector.inject(payload);
//   };
//
// Injection model: key and mouse-button state is diffed against the
// previous packet, so only transitions become SendInput events -- a held
// key generates exactly one keydown. The key state is the full 256-bit VK
// bitmask, so every keyboard key replays, including left/right modifiers.
// Keyboard events carry scancodes (KEYEVENTF_SCANCODE, plus
// KEYEVENTF_EXTENDEDKEY where required) so games that read DirectInput/raw
// input see them, not just GetAsyncKeyState() callers. Mouse movement is
// MOUSEEVENTF_MOVE relative deltas in game mode, or MOUSEEVENTF_ABSOLUTE
// normalized coordinates on the primary (captured) monitor when the packet
// carries InputFlags::kAbsoluteMouse (desktop mode). stop() releases
// everything still held, so a client that vanishes mid-keypress cannot
// leave the host with a stuck key.
//
// ViGEm is optional at runtime: if the ViGEmBus driver is not installed,
// start() logs a warning, disables gamepad injection, and still returns
// true -- keyboard/mouse forwarding works on a bare machine (lastError()
// holds the ViGEm failure description). The virtual pad is updated only
// when the packet's gamepad fields actually changed.
//
// Requires: MSVC 2022, C++20, ViGEmClient SDK at extern/vigemclient/
// (headers under include/, x64 ViGEmClient.lib under lib/x64/ -- linked
// together with setupapi.lib via #pragma comment in the .cpp).

#pragma once

#include "InputPacket.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// ViGEm handle types (ViGEm/Client.h typedefs pointers to these tags);
// forward-declared so this header does not drag the SDK into every includer.
struct _VIGEM_CLIENT_T;
struct _VIGEM_TARGET_T;

class InputInjector {
public:
    InputInjector() = default;
    ~InputInjector(); // calls stop() if still running

    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // Connects to the ViGEm bus and plugs in a virtual Xbox 360 pad.
    // Gamepad failures degrade to keyboard/mouse-only as described above,
    // so this returns true even then; the bool is kept for symmetry with
    // the other modules' start().
    bool start();

    // Releases every key/button still held, neutralizes + unplugs the
    // virtual pad, disconnects from the bus. Safe to call when not running.
    void stop();

    // Deserializes one wire packet and injects it. Thread-safe (single
    // internal mutex serializes SendInput + ViGEm). Returns false on a
    // malformed packet or when not running. No exceptions and no heap
    // allocation on the success path.
    bool inject(const std::vector<uint8_t>& rawPacket);

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // Most recent failure description (ViGEm setup, malformed packets).
    std::string lastError() const;

private:
    struct PadState {
        uint16_t buttons      = 0;
        uint8_t  leftTrigger  = 0;
        uint8_t  rightTrigger = 0;
        int16_t  lx = 0, ly = 0, rx = 0, ry = 0;

        bool operator==(const PadState&) const = default;
    };

    void setError(std::string message);

    // Serializes inject()/stop(): SendInput ordering and the ViGEm calls.
    std::mutex m_mutex;

    // --- previous packet state, guarded by m_mutex ---
    uint64_t m_prevKeys[4]      = {}; // 256-bit VK bitmask of held keys
    uint8_t  m_prevMouseButtons = 0;
    bool     m_prevAbsValid     = false; // last absolute position known?
    uint16_t m_prevAbsX         = 0;
    uint16_t m_prevAbsY         = 0;
    PadState m_prevPad;
    uint32_t m_sendInputFails   = 0; // throttles SendInput failure logging

    // --- ViGEm handles, guarded by m_mutex; null when gamepad disabled ---
    _VIGEM_CLIENT_T* m_client = nullptr;
    _VIGEM_TARGET_T* m_target = nullptr;

    std::atomic<bool>     m_running{false};
    std::atomic<uint32_t> m_badPackets{0}; // throttles malformed-packet logging

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;
};
