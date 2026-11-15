// NetworkProtocol.h
//
// Shared wire-protocol definitions for the game streaming UDP transport.
// Included by both StreamSender (server) and StreamReceiver (client).
//
// Wire format -- every datagram is one PacketHeader (18 bytes, all
// multi-byte fields little-endian) followed immediately by the payload:
//
//   [4B] magic        = 0x47534D52 ("GSMR")
//   [1B] type         = VIDEO(0x01) | AUDIO(0x02) | INPUT(0x03) | KEEPALIVE(0x04)
//   [4B] sequence     -- monotonic counter, independent per stream type;
//                        increments per logical unit (frame), NOT per packet
//   [2B] fragmentId   -- 0-based index of this fragment
//   [2B] totalFrags   -- total fragments for this logical unit
//   [4B] timestampUs  -- capture timestamp, low 32 bits (wraps at ~71 min;
//                        receivers unwrap with signed-delta accumulation)
//   [1B] flags        = KEYFRAME(0x01) | LAST_FRAG(0x02)
//
// A logical unit (one encoded video frame, one audio packet, one input
// packet) larger than kMaxPayloadSize is split into fragments. All
// fragments of a unit carry the same sequence number; every fragment
// except the last carries exactly kMaxPayloadSize bytes, and the last one
// carries the remainder plus the LAST_FRAG flag. Reassembly is complete
// when all totalFrags fragments have arrived.
//
// Keepalive convention (not part of the byte layout, but both sides
// implement it): the fragmentId of a KEEPALIVE selects a control message
// (KeepaliveKind below):
//   0 = ping; the peer echoes it back with fragmentId == 1 and the
//       timestampUs unchanged, which lets the original sender compute RTT
//       from its own clock.
//   1 = pong (ping echo).
//   2 = keyframe request (client -> server): the client detected loss it
//       cannot conceal (evicted reassembly, delivered past a gap, decoder
//       not yet primed) and asks for a fresh IDR. No payload.
//   3 = receiver report (client -> server): 8-byte payload, two u32 LE --
//       frames expected and frames completed over the client's last stats
//       window (~1 s). Drives the server's adaptive bitrate controller.
//
// Encryption (optional, PacketCrypto in Crypto.h): when both configs set
// the same "psk", every datagram becomes
//   [18B header, plaintext, authenticated] [12B nonce] [ciphertext] [16B tag]
// AES-256-GCM with per-direction keys derived from the PSK. The receiver
// of a datagram that fails authentication drops it before ANY state is
// touched -- including client discovery, which is what closes the
// "first datagram wins" hijack hole. kMaxPayloadSize already reserves the
// 28-byte crypto overhead so fragment layout is identical in both modes.
//
// This header is OS-free on purpose (no winsock includes) so protocol
// logic stays unit-testable; serialization is explicit byte-by-byte and
// endian-safe.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Sizes and constants
// ---------------------------------------------------------------------------

constexpr uint32_t kPacketMagic      = 0x47534D52; // "GSMR"
constexpr size_t   kPacketHeaderSize = 18;
constexpr size_t   kMaxDatagramSize  = 1400;
// 12-byte GCM nonce + 16-byte tag, appended when encryption is enabled.
// Reserved unconditionally so fragment sizes (and therefore reassembly
// offsets) are identical with and without a PSK.
constexpr size_t   kCryptoOverhead   = 28;
constexpr size_t   kMaxPayloadSize   =
    kMaxDatagramSize - kPacketHeaderSize - kCryptoOverhead; // 1354

enum class PacketType : uint8_t {
    Video     = 0x01,
    Audio     = 0x02,
    Input     = 0x03,
    Keepalive = 0x04,
};

namespace PacketFlags {
constexpr uint8_t kKeyframe = 0x01; // unit is an IDR frame (set on every fragment)
constexpr uint8_t kLastFrag = 0x02; // this fragment is the unit's last
} // namespace PacketFlags

// Control messages carried in a KEEPALIVE's fragmentId (see file comment).
namespace KeepaliveKind {
constexpr uint16_t kPing            = 0;
constexpr uint16_t kPong            = 1;
constexpr uint16_t kKeyframeRequest = 2; // client -> server, no payload
constexpr uint16_t kReceiverReport  = 3; // client -> server, 8B payload
} // namespace KeepaliveKind

constexpr size_t kReceiverReportSize = 8; // u32 expected + u32 completed, LE

// ---------------------------------------------------------------------------
// PacketHeader
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t   magic       = kPacketMagic;
    PacketType type        = PacketType::Keepalive;
    uint32_t   sequence    = 0;
    uint16_t   fragmentId  = 0;
    uint16_t   totalFrags  = 1;
    uint32_t   timestampUs = 0;
    uint8_t    flags       = 0;
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == kPacketHeaderSize,
              "PacketHeader must match the 18-byte wire layout");

// Explicit little-endian (de)serialization. The struct above is packed and
// would memcpy correctly on x86/x64, but the byte-by-byte form is the wire
// contract: it stays correct on any endianness and survives future struct
// reordering.

inline void writeU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void writeU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// `out` must have room for kPacketHeaderSize bytes.
inline void serialize(const PacketHeader& h, uint8_t* out) {
    writeU32LE(out + 0, h.magic);
    out[4] = static_cast<uint8_t>(h.type);
    writeU32LE(out + 5, h.sequence);
    writeU16LE(out + 9, h.fragmentId);
    writeU16LE(out + 11, h.totalFrags);
    writeU32LE(out + 13, h.timestampUs);
    out[17] = h.flags;
}

// Returns false if the buffer is too short, the magic doesn't match, or the
// packet type is unknown -- callers can drop the datagram without further
// validation.
inline bool deserialize(const uint8_t* data, size_t len, PacketHeader& out) {
    if (!data || len < kPacketHeaderSize)
        return false;
    out.magic = readU32LE(data + 0);
    if (out.magic != kPacketMagic)
        return false;
    const uint8_t type = data[4];
    if (type < static_cast<uint8_t>(PacketType::Video) ||
        type > static_cast<uint8_t>(PacketType::Keepalive))
        return false;
    out.type        = static_cast<PacketType>(type);
    out.sequence    = readU32LE(data + 5);
    out.fragmentId  = readU16LE(data + 9);
    out.totalFrags  = readU16LE(data + 11);
    out.timestampUs = readU32LE(data + 13);
    out.flags       = data[17];
    return true;
}

// ---------------------------------------------------------------------------
// Stream payload types
// ---------------------------------------------------------------------------

// One encoded video frame as produced by the NVENC module (H.264 Annex B).
// NvencEncoder::encode() returns the raw byte vector; the streaming app
// wraps it in this struct (timestamp from CaptureFrame::timestamp_us,
// isKeyframe true for the first frame and after requestKeyframe()).
struct EncodedFrame {
    std::vector<uint8_t> nalUnits;        // H.264 Annex B
    uint64_t             timestampUs = 0; // capture timestamp
    bool                 isKeyframe  = false;
};

// Per-stage pipeline latency snapshot, produced by LatencyTracker::report()
// (LatencyTracker.h) and carried inside StreamStats so the render overlay
// can show it. Not a wire structure -- it never leaves the process. All
// values are rolling averages in milliseconds; -1 means "not measurable
// here" (each process can only time the stage pairs it stamped itself:
// the server gets capture/encode, the client gets decode/render, and the
// cross-machine pairs resolve only when both ends share a clock -- see
// the clock-domain note in LatencyTracker.h).
struct LatencyReport {
    float    captureMs  = -1.0f; // CAPTURE_START -> ENCODE_START
    float    encodeMs   = -1.0f; // ENCODE_START  -> ENCODE_END
    float    networkMs  = -1.0f; // SEND_START    -> RECEIVE_END
    float    decodeMs   = -1.0f; // DECODE_START  -> DECODE_END
    float    renderMs   = -1.0f; // RENDER_START  -> RENDER_END
    float    totalMs    = -1.0f; // CAPTURE_START -> RENDER_END
    float    p99TotalMs = -1.0f; // 99th percentile of total, last 300 frames
    uint32_t trackedFrames = 0;  // finalized traces since start (0 = no data yet)
};

// Snapshot of transport health, refreshed once per second by each side's
// stats thread and read via getStats().
struct StreamStats {
    float    rttMs           = 0.0f; // round-trip time from keepalive echoes
    float    packetLossPct   = 0.0f; // lost/sent * 100, rolling 1-second window
                                     // (receiver-side only: the sender has no
                                     // feedback channel and reports 0)
    float    bitrateKbps     = 0.0f; // bytes sent/received in the last second * 8
    uint32_t droppedFrames   = 0;    // cumulative dropped video frames
    uint32_t outOfOrderCount = 0;    // cumulative out-of-order packets

    // Filled by the application (not the transport) from
    // LatencyTracker::get().report() before handing the snapshot to the
    // overlay; the transport's stats threads leave it default.
    LatencyReport latency;
};

// ---------------------------------------------------------------------------
// ReassemblySlot -- in-progress reassembly of one fragmented unit
// ---------------------------------------------------------------------------
//
// `data` is laid out as totalFrags * kMaxPayloadSize so fragment i lands at
// offset i * kMaxPayloadSize regardless of arrival order. Because every
// fragment except the last is exactly kMaxPayloadSize bytes, the assembled
// unit is the contiguous prefix of length assembledSize() once complete.

struct ReassemblySlot {
    std::vector<uint8_t>  data;
    std::vector<uint64_t> receivedMask;     // 1 bit per fragment
    uint64_t firstArrivalUs = 0;            // local steady clock, for eviction
    uint32_t timestampUs    = 0;            // wire timestamp of the unit
    uint32_t lastFragSize   = 0;            // valid once the LAST_FRAG arrived
    uint16_t totalFrags     = 0;
    uint16_t receivedCount  = 0;
    uint8_t  flags          = 0;            // OR of all fragments' flags
    bool     sawLastFrag    = false;

    void init(uint16_t frags, uint32_t tsUs, uint64_t arrivalUs) {
        totalFrags     = frags;
        timestampUs    = tsUs;
        firstArrivalUs = arrivalUs;
        data.resize(static_cast<size_t>(frags) * kMaxPayloadSize);
        receivedMask.assign((frags + 63u) / 64u, 0);
        receivedCount = 0;
        lastFragSize  = 0;
        flags         = 0;
        sawLastFrag   = false;
    }

    // Marks fragment `id` received; returns false if it was a duplicate.
    bool markReceived(uint16_t id) {
        uint64_t& word = receivedMask[id / 64u];
        const uint64_t bit = 1ull << (id % 64u);
        if (word & bit)
            return false;
        word |= bit;
        ++receivedCount;
        return true;
    }

    bool complete() const {
        return totalFrags != 0 && receivedCount == totalFrags && sawLastFrag;
    }

    size_t assembledSize() const {
        return static_cast<size_t>(totalFrags - 1) * kMaxPayloadSize + lastFragSize;
    }
};
