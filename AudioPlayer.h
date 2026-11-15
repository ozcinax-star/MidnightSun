// AudioPlayer.h
//
// Client-side audio playback for the game streaming pipeline. Consumes the
// Opus packets that AudioCapture sends (delivered by
// StreamReceiver::onAudioPacket), decodes them to 48 kHz stereo float32,
// runs them through a small jitter ring, and renders through WASAPI shared
// mode.
//
// Wiring -- onPacket() is thread-safe, the callback fires on GSMR-Recv:
//
//   AudioPlayer player;
//   player.start();
//   receiver.onAudioPacket = [&](std::vector<uint8_t> opus, uint64_t tsUs) {
//       player.onPacket(opus, tsUs);
//   };
//
// Threading model:
//   - start()/stop() from the control thread. start() blocks until WASAPI
//     render and the Opus decoder are up (or failed).
//   - onPacket() (GSMR-Recv thread): Opus decode + packet-loss concealment
//     under the decoder mutex, then pushes the decoded frame into the ring.
//     A timestamp gap > 25 ms means packets were lost on the wire: up to 4
//     PLC frames (opus_decode_float with null input) fill the hole before
//     the current packet is decoded.
//   - "Audio-Render" thread: owns ALL COM objects (CoInitializeEx
//     MULTITHREADED), event-driven IAudioRenderClient loop boosted via
//     MMCSS "Pro Audio". Renders directly in 48 kHz stereo float32 by
//     initializing with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
//     SRC_DEFAULT_QUALITY -- the audio engine converts to whatever the
//     device mix format is, so no render-side resampler is needed.
//
// Jitter ring: 8 slots of one decoded 20 ms frame each. Playback waits for
// 2 buffered frames before starting (and re-primes after a full drain) so
// one late packet doesn't stutter. Ring full -> oldest frame dropped with
// a throttled warning. Ring empty -> silence; PLC is deliberately NOT used
// here -- concealment is for packets lost on the network (handled in
// onPacket), silence is for buffer underrun.
//
// Device changes: an IMMNotificationClient watches the default render
// endpoint; when it changes (headset plugged in) or the device dies, the
// render thread rebuilds the WASAPI objects on the new default, retrying
// every 500 ms while none is available. The decoder and jitter ring
// survive the swap.
//
// Clock sync: sender and receiver audio clocks drift apart by design
// (independent crystals). The render thread tracks total buffered audio --
// ring content plus what WASAPI holds, measured against IAudioClock's
// actual render position -- as an EMA; when it exceeds the prime target by
// more than 5 ms, one 20 ms frame is dropped (logged, at most every
// 500 ms). The slow direction needs no duplication: a drained ring already
// inserts silence, which stretches time the same way.
//
// Requires: MSVC 2022, C++20, Opus SDK at extern/opus/ (headers under
// include/, x64 opus.lib under lib/x64/ -- linked together with ole32 /
// oleaut32 / avrt via #pragma comment in the .cpp).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mmdeviceapi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// From opus.h ("typedef struct OpusDecoder OpusDecoder;"); forward-declared
// so this header does not drag the SDK into every includer.
struct OpusDecoder;

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer(); // calls stop() if still running

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Creates the Opus decoder, spins up the render thread, and blocks
    // until WASAPI is initialized. Returns false with lastError() set on
    // failure (nothing left running in that case).
    bool start();

    // Joins the render thread, destroys the decoder, releases everything.
    // Safe to call when not running.
    void stop();

    // Decodes one Opus packet (plus PLC for any detected gap) and queues
    // it for playback. Thread-safe; call from the StreamReceiver callback.
    // Cheap no-op when the player is not running.
    void onPacket(const std::vector<uint8_t>& opusData, uint64_t timestampUs);

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // Most recent failure description; read it after start() reports false.
    std::string lastError() const;

private:
    static constexpr uint32_t kSampleRate      = 48'000;
    static constexpr uint32_t kChannels        = 2;
    static constexpr uint32_t kFrameSamples    = 960; // 20 ms per channel
    static constexpr uint32_t kFrameFloats     = kFrameSamples * kChannels;
    static constexpr uint64_t kFrameDurationUs = 20'000;
    static constexpr uint64_t kGapThresholdUs  = 25'000; // loss detector
    static constexpr uint32_t kMaxPlcFrames    = 4;
    static constexpr size_t   kRingSlots       = 8;
    static constexpr size_t   kPrimeFrames     = 2;
    static constexpr uint32_t kDriftSamples    = 240;     // 5 ms at 48 kHz
    static constexpr uint64_t kDriftCooldownUs = 500'000; // between corrections

    struct JitterSlot {
        uint64_t timestampUs = 0;
        std::array<float, kFrameFloats> pcm{};
    };

    // Everything the render thread owns (COM interfaces, consumer-side
    // frame staging, drift state). Defined in the .cpp.
    struct RenderContext;

    void renderThreadProc(std::promise<bool> ready);
    bool initRender(RenderContext& ctx, IMMDeviceEnumerator* enumerator);

    // Copies `frames` stereo samples into `dst` from the ring (silence on
    // underrun); updates the consumer staging in ctx.
    void fillBuffer(RenderContext& ctx, float* dst, uint32_t frames);

    // Producer side. Drops the oldest slot (throttled warning) when full.
    void pushFrame(const float* pcm, uint64_t timestampUs);

    // Consumer side. Returns false while priming or after a full drain.
    bool popFrame(float* pcm);

    // Stereo samples currently buffered in the ring (consumer staging not
    // included). Takes the ring mutex.
    uint32_t ringSamples() const;

    void setError(std::string message);

    // --- decoder (guarded by m_decoderMutex; used on the recv thread) ---
    std::mutex   m_decoderMutex;
    OpusDecoder* m_decoder = nullptr;
    std::vector<float> m_decodeBuf; // kFrameFloats, allocated in start()
    uint64_t m_lastPacketTs    = 0;
    bool     m_lastPacketValid = false;

    // --- jitter ring (producer: recv thread, consumer: render thread) ---
    mutable std::mutex m_ringMutex;
    std::array<JitterSlot, kRingSlots> m_ring;
    size_t m_ringHead  = 0;
    size_t m_ringCount = 0;
    bool   m_primed    = false;

    // --- counters (throttled logging) ---
    std::atomic<uint32_t> m_ringDrops{0};
    std::atomic<uint32_t> m_plcFrames{0};
    std::atomic<uint32_t> m_underruns{0};
    std::atomic<uint32_t> m_decodeFails{0};

    HANDLE m_event = nullptr; // created in start(), closed in stop()

    // Set by the IMMNotificationClient when the default render endpoint
    // changes; the render thread reinitializes and clears it.
    std::atomic<bool> m_deviceChanged{false};

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};
