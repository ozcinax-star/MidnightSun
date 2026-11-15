// AudioCapture.h
//
// Server-side audio capture for the game streaming pipeline. Captures
// whatever the system is playing through WASAPI loopback, adapts it to
// 48 kHz stereo float32, encodes 20 ms Opus frames, and hands them to
// StreamSender::sendAudio() -- this is what finally feeds the transport's
// audio path.
//
// Threading model:
//   - start()/stop() from the control thread. start() blocks until WASAPI
//     and the Opus encoder are up (or failed); stop() wakes the thread,
//     lets it flush the partial staging frame (zero-padded), and joins.
//   - "Audio-Capture" thread: owns ALL COM objects (CoInitializeEx
//     MULTITHREADED, released before CoUninitialize) and the Opus encoder.
//     Event-driven IAudioCaptureClient loop (Windows 10+ signals events
//     for loopback clients; the 200 ms wait timeout keeps the loop
//     responsive to stop() either way), boosted via MMCSS "Pro Audio".
//
// Format adaptation: shared-mode loopback must use the device mix format,
// which is float32 in practice but can be any rate and channel count.
// Non-48 kHz input goes through a stateful linear resampler; >2 channels
// are folded with standard downmix weights (C/back/side at -3 dB, LFE
// dropped); int16 sources are scaled. Anything else fails start() with a
// clear error.
//
// Opus: 960-sample (20 ms) frames, 128 kbps CBR-ish VBR, application
// OPUS_APPLICATION_RESTRICTED_LOWDELAY -- the spec sheet said VOIP "for
// latency", but VOIP is a speech-tuning mode with the same lookahead as
// AUDIO; RESTRICTED_LOWDELAY is the mode that actually cuts algorithmic
// delay (2.5 ms lookahead, CELT-only), and CELT is the right codec for
// game audio at this bitrate. OPUS_SET_SIGNAL is a no-op in this mode and
// is therefore not set.
//
// Timestamps are synthesized from the encoded-sample counter anchored to
// the steady clock at start: exactly 20'000 us apart, so the receiver's
// packet-loss detection (gap > 25 ms) only fires on real loss, never on
// capture jitter.
//
// Loopback caveat: WASAPI loopback produces data only while some session
// is rendering. When the machine goes silent, no packets are sent and the
// player underruns to silence by design.
//
// Device changes: an IMMNotificationClient watches the default render
// endpoint; when it changes (or the device is invalidated / unplugged),
// the capture thread tears down the WASAPI objects and re-initializes on
// the new default -- retrying every 500 ms while no device is available.
// The Opus encoder and the timestamp counter survive the swap, so the
// receiver just sees a short gap.
//
// Requires: MSVC 2022, C++20, Opus SDK at extern/opus/ (headers under
// include/, x64 opus.lib under lib/x64/ -- linked together with ole32 /
// oleaut32 / avrt via #pragma comment in the .cpp).

#pragma once

#include "StreamSender.h" // pulls winsock2.h before windows.h

#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// From opus.h ("typedef struct OpusEncoder OpusEncoder;"); forward-declared
// so this header does not drag the SDK into every includer.
struct OpusEncoder;

class AudioCapture {
public:
    explicit AudioCapture(StreamSender& sender);
    ~AudioCapture(); // calls stop() if still running

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Spins up the capture thread and blocks until WASAPI loopback and the
    // Opus encoder are initialized. Returns false with lastError() set on
    // failure (no thread left running in that case).
    bool start();

    // Wakes the thread, flushes the partial staging frame, joins, releases
    // everything. Safe to call when not running.
    void stop();

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // Most recent failure description; read it after start() reports false.
    std::string lastError() const;

private:
    static constexpr uint32_t kSampleRate      = 48'000;
    static constexpr uint32_t kChannels        = 2;
    static constexpr uint32_t kFrameSamples    = 960; // 20 ms per channel
    static constexpr uint32_t kFrameFloats     = kFrameSamples * kChannels;
    static constexpr uint64_t kFrameDurationUs = 20'000;
    static constexpr int32_t  kOpusBitrate     = 128'000;
    static constexpr size_t   kMaxOpusPacket   = 4000; // > worst case for one frame

    // Everything the capture thread owns (COM interfaces, source format,
    // resampler state). Defined in the .cpp; lives on the thread's stack.
    struct CaptureContext;

    void captureThreadProc(std::promise<bool> ready);
    bool initCapture(CaptureContext& ctx, IMMDeviceEnumerator* enumerator);

    // Converts one WASAPI packet (source format) to 48 kHz stereo float32
    // appended onto `stage`. `scratch` is reused to avoid hot-path
    // allocation; `silent` short-circuits to zeros at the right length.
    void convertAppend(CaptureContext& ctx, const uint8_t* data, uint32_t frames,
                       bool silent, std::vector<float>& scratch,
                       std::vector<float>& stage);

    void setError(std::string message);

    StreamSender& m_sender;

    HANDLE m_event = nullptr; // created in start(), closed in stop()

    // Set by the IMMNotificationClient when the default render endpoint
    // changes; the capture thread reinitializes and clears it.
    std::atomic<bool> m_deviceChanged{false};

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};
