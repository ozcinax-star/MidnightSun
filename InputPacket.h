// InputPacket.h
//
// Wire format for the GSMR INPUT stream (PacketType::Input), protocol v2.
// One packet is a fixed 55-byte little-endian snapshot of the client's
// complete input state, sent by InputCapture (client) at up to 120 Hz and
// replayed by InputInjector (server). Always single-fragment: 55 bytes is
// far below kMaxPayloadSize, which matters because StreamSender only
// accepts single-fragment input packets.
//
// Layout (offsets in bytes):
//
//   [ 0, 1B]  type            = 0x03 (PacketType::Input)
//   [ 1, 1B]  flags           -- InputFlags:: bits (bit0 = absolute mouse)
//   [ 2,32B]  keys            -- 256-bit bitmask, bit == raw VK code.
//                                Serialized as four u64 LE words; bit v of
//                                the mask = key with virtual-key code v is
//                                down. Covers EVERY key a keyboard can
//                                report (letters, numpad, OEM punctuation,
//                                Home/End/PgUp/PgDn, Win, media keys, and
//                                distinct left/right modifiers VK_LSHIFT..
//                                VK_RMENU 0xA0..0xA5).
//   [34, 2B]  mouseX          -- relative mode: accumulated delta (i16).
//   [36, 2B]  mouseY             absolute mode (flags bit0): normalized
//                                0..65535 position on the captured monitor
//                                (u16), same convention as MOUSEEVENTF_ABSOLUTE.
//   [38, 1B]  mouseButtons    -- MouseButtons:: bits (incl. X1/X2)
//   [39, 2B]  gamepadButtons  -- XINPUT_GAMEPAD_* bitmask (same bit layout
//                                as the XUSB report ViGEm consumes)
//   [41, 2B]  thumbLX         -- thumbstick axes, full SHORT range,
//   [43, 2B]  thumbLY            deadzone already applied by the client
//   [45, 2B]  thumbRX
//   [47, 2B]  thumbRY
//   [49, 1B]  leftTrigger     -- 0..255
//   [50, 1B]  rightTrigger
//   [51, 2B]  wheelDelta      -- vertical mouse wheel, accumulated WHEEL_DELTA
//                                units since the previous packet (touchpad
//                                two-finger scroll arrives the same way)
//   [53, 2B]  wheelDeltaH     -- horizontal wheel, same convention
//
// v1 -> v2 changes (both ends MUST be rebuilt together):
//   - 64-entry kInputKeyTable replaced by a full 256-bit VK bitmask; the
//     bit index IS the VK code, no translation table.
//   - flags byte added; bit0 selects absolute-mouse interpretation of
//     mouseX/mouseY (desktop mode: client cursor position mapped onto the
//     stream, injected with MOUSEEVENTF_ABSOLUTE on the server).
//   - extended mouse buttons X1/X2 added to mouseButtons.
//   - packet grows 30 -> 55 bytes; deserialize() rejects any other size,
//     so a stale v1 peer is dropped loudly rather than misparsed.
//
// Like NetworkProtocol.h this header is OS-free on purpose (no windows.h);
// serialization is explicit byte-by-byte and endian-safe.

#pragma once

#include "NetworkProtocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr size_t  kInputPacketSize = 55;
constexpr uint8_t kInputPacketType = static_cast<uint8_t>(PacketType::Input);

namespace InputFlags {
constexpr uint8_t kAbsoluteMouse = 0x01; // mouseX/Y are 0..65535 normalized
} // namespace InputFlags

namespace MouseButtons {
constexpr uint8_t kLeft   = 0x01;
constexpr uint8_t kRight  = 0x02;
constexpr uint8_t kMiddle = 0x04;
constexpr uint8_t kX1     = 0x08; // XBUTTON1 ("back")
constexpr uint8_t kX2     = 0x10; // XBUTTON2 ("forward")
} // namespace MouseButtons

// ---------------------------------------------------------------------------
// InputPacket
// ---------------------------------------------------------------------------

struct InputPacket {
    uint8_t  type           = kInputPacketType;
    uint8_t  flags          = 0;
    uint64_t keys[4]        = {}; // bit v (v = 0..255) = VK code v is down
    int16_t  mouseX         = 0;  // delta, or abs 0..65535 (see flags)
    int16_t  mouseY         = 0;
    uint8_t  mouseButtons   = 0;
    uint16_t gamepadButtons = 0;
    int16_t  thumbLX        = 0;
    int16_t  thumbLY        = 0;
    int16_t  thumbRX        = 0;
    int16_t  thumbRY        = 0;
    uint8_t  leftTrigger    = 0;
    uint8_t  rightTrigger   = 0;
    int16_t  wheelDelta     = 0;
    int16_t  wheelDeltaH    = 0;

    bool keyDown(uint8_t vk) const {
        return (keys[vk >> 6] >> (vk & 63)) & 1ull;
    }
    void setKey(uint8_t vk, bool down) {
        const uint64_t bit = 1ull << (vk & 63);
        if (down)
            keys[vk >> 6] |= bit;
        else
            keys[vk >> 6] &= ~bit;
    }
};

// ---------------------------------------------------------------------------
// Serialization (explicit little-endian, like PacketHeader's)
// ---------------------------------------------------------------------------

inline void writeU64LE(uint8_t* p, uint64_t v) {
    writeU32LE(p, static_cast<uint32_t>(v));
    writeU32LE(p + 4, static_cast<uint32_t>(v >> 32));
}

inline uint64_t readU64LE(const uint8_t* p) {
    return static_cast<uint64_t>(readU32LE(p)) |
           (static_cast<uint64_t>(readU32LE(p + 4)) << 32);
}

// `out` must have room for kInputPacketSize bytes.
inline void serialize(const InputPacket& p, uint8_t* out) {
    out[0] = p.type;
    out[1] = p.flags;
    for (int w = 0; w < 4; ++w)
        writeU64LE(out + 2 + w * 8, p.keys[w]);
    writeU16LE(out + 34, static_cast<uint16_t>(p.mouseX));
    writeU16LE(out + 36, static_cast<uint16_t>(p.mouseY));
    out[38] = p.mouseButtons;
    writeU16LE(out + 39, p.gamepadButtons);
    writeU16LE(out + 41, static_cast<uint16_t>(p.thumbLX));
    writeU16LE(out + 43, static_cast<uint16_t>(p.thumbLY));
    writeU16LE(out + 45, static_cast<uint16_t>(p.thumbRX));
    writeU16LE(out + 47, static_cast<uint16_t>(p.thumbRY));
    out[49] = p.leftTrigger;
    out[50] = p.rightTrigger;
    writeU16LE(out + 51, static_cast<uint16_t>(p.wheelDelta));
    writeU16LE(out + 53, static_cast<uint16_t>(p.wheelDeltaH));
}

inline std::vector<uint8_t> serializeInputPacket(const InputPacket& p) {
    std::vector<uint8_t> out(kInputPacketSize);
    serialize(p, out.data());
    return out;
}

// Strict: an input packet is exactly kInputPacketSize bytes with the INPUT
// type byte; anything else (including old 30-byte v1 packets) is dropped
// by the caller.
inline bool deserialize(const uint8_t* data, size_t len, InputPacket& out) {
    if (!data || len != kInputPacketSize || data[0] != kInputPacketType)
        return false;
    out.type  = data[0];
    out.flags = data[1];
    for (int w = 0; w < 4; ++w)
        out.keys[w] = readU64LE(data + 2 + w * 8);
    out.mouseX         = static_cast<int16_t>(readU16LE(data + 34));
    out.mouseY         = static_cast<int16_t>(readU16LE(data + 36));
    out.mouseButtons   = data[38];
    out.gamepadButtons = readU16LE(data + 39);
    out.thumbLX        = static_cast<int16_t>(readU16LE(data + 41));
    out.thumbLY        = static_cast<int16_t>(readU16LE(data + 43));
    out.thumbRX        = static_cast<int16_t>(readU16LE(data + 45));
    out.thumbRY        = static_cast<int16_t>(readU16LE(data + 47));
    out.leftTrigger    = data[49];
    out.rightTrigger   = data[50];
    out.wheelDelta     = static_cast<int16_t>(readU16LE(data + 51));
    out.wheelDeltaH    = static_cast<int16_t>(readU16LE(data + 53));
    return true;
}
