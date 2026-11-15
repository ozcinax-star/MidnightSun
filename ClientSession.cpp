// ClientSession.cpp -- see ClientSession.h for the contract.
//
// Pipeline (identical to the former client_main session):
//
//   StreamReceiver (own GSMR-Recv thread: reassembly, reorder, jitter)
//        |  onVideoFrame                              [GSMR-Recv thread]
//   VideoDecoder::decode()  ->  onDecodedFrame (NV12 GPU texture)
//        |  frame mailbox (mutex + condvar, latest-frame-wins)
//   RenderWindow::presentFrame()                      [caller's thread]
//
//   StreamReceiver::onAudioPacket  ->  AudioPlayer::onPacket()
//   InputCapture (own 120 Hz thread)  ->  StreamReceiver::sendInput()
//
// The decoder callback fires on the receiver thread; the render window is
// strictly single-threaded (Win32 message queue), so decoded frames cross
// threads through a one-slot mailbox -- newest frame wins. The
// decoder-primed gate keeps requesting an IDR (throttled) until the first
// keyframe arrives, which is also what makes mid-stream joins and
// reconnects converge quickly.

#include "StreamReceiver.h" // pulls winsock2.h before windows.h
#include "AudioPlayer.h"
#include "ClientSession.h"
#include "InputCapture.h"
#include "LatencyTracker.h"
#include "RenderWindow.h"
#include "VideoDecoder.h"

#include <windows.h>
#include <d3d11_4.h> // ID3D11Multithread

#pragma comment(lib, "d3d11.lib")

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

// One-slot frame mailbox between the decode callback (GSMR-Recv thread)
// and the render loop (caller's thread). Newest frame wins; the ComPtr
// keeps the decoder's ring texture alive while it crosses threads.
struct FrameMailbox {
    std::mutex                              mutex;
    std::condition_variable                 cv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    uint64_t                                timestampUs = 0;
    bool                                    fresh       = false;
};

} // namespace

void RunClientSession(const AppConfig& config, const std::string& serverIp,
                      uint16_t serverPort, const std::atomic<bool>& interrupted)
{
    printf("[client] connecting to %s:%u\n", serverIp.c_str(), serverPort);

    // --- D3D11 device shared by decoder and render window ---
    // VIDEO_SUPPORT for D3D11VA + the video processor, BGRA_SUPPORT for
    // the Direct2D overlay.
    Microsoft::WRL::ComPtr<ID3D11Device>        device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const UINT deviceFlags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   deviceFlags, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &featureLevel, &context);
    if (FAILED(hr)) {
        fprintf(stderr, "[client] D3D11CreateDevice failed: 0x%08lX\n",
                static_cast<unsigned long>(hr));
        return;
    }

    // Decode (recv thread) and render (this thread) share the immediate
    // context; protected mode serializes them. Also required by Media
    // Foundation for D3D11 decoding.
    {
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);
    }

    // --- decoder + window ---
    VideoDecoder decoder;
    if (!decoder.initialize(device.Get())) {
        fprintf(stderr, "[client] video decoder failed to start: %s\n",
                decoder.lastError().c_str());
        return;
    }

    RenderWindow window;
    if (!window.initialize(device.Get(), 1920, 1080)) {
        fprintf(stderr, "[client] render window failed to start: %s\n",
                window.lastError().c_str());
        decoder.shutdown();
        return;
    }

    FrameMailbox mailbox;
    decoder.onDecodedFrame = [&mailbox](ID3D11Texture2D* texture, uint64_t tsUs) {
        LatencyTracker::get().mark(tsUs, Stage::DECODE_END);
        {
            std::lock_guard<std::mutex> lock(mailbox.mutex);
            mailbox.texture     = texture;
            mailbox.timestampUs = tsUs;
            mailbox.fresh       = true;
        }
        mailbox.cv.notify_one();
    };

    // --- audio (optional: a session without audio still shows video) ---
    AudioPlayer player;
    const bool audioUp = player.start();
    if (!audioUp)
        fprintf(stderr, "[client] audio playback unavailable: %s "
                        "(continuing without audio)\n",
                player.lastError().c_str());

    // --- transport ---
    StreamReceiver receiver(serverIp, serverPort);
    std::atomic<bool> disconnected{false};
    // The decoder cannot start on a P-frame: until the first keyframe
    // arrives (mid-stream join, or after a reconnect), keep asking the
    // host for an IDR -- requestKeyframe() self-throttles to 4/s.
    std::atomic<bool> decoderPrimed{false};
    receiver.onVideoFrame = [&decoder, &receiver, &decoderPrimed](EncodedFrame frame) {
        if (frame.isKeyframe)
            decoderPrimed.store(true, std::memory_order_relaxed);
        else if (!decoderPrimed.load(std::memory_order_relaxed)) {
            receiver.requestKeyframe();
            return; // undecodable without a reference; don't feed the decoder
        }
        LatencyTracker::get().mark(frame.timestampUs, Stage::RECEIVE_END);
        LatencyTracker::get().mark(frame.timestampUs, Stage::DECODE_START);
        decoder.decode(frame);
    };
    if (audioUp) {
        receiver.onAudioPacket = [&player](std::vector<uint8_t> opus, uint64_t tsUs) {
            player.onPacket(opus, tsUs);
        };
    }
    receiver.onDisconnect = [&disconnected]() {
        fprintf(stderr, "[client] connection lost (no traffic for 3 s); "
                        "will retry every 3 s\n");
        disconnected.store(true, std::memory_order_release);
    };
    if (!receiver.setPsk(config.psk)) {
        // A configured PSK that cannot be applied must not fall back to
        // plaintext silently.
        fprintf(stderr, "[client] transport encryption setup failed: %s\n",
                receiver.lastError().c_str());
        player.stop();
        window.shutdown();
        decoder.shutdown();
        return;
    }
    if (!receiver.start()) {
        fprintf(stderr, "[client] transport failed to start: %s\n",
                receiver.lastError().c_str());
        player.stop();
        window.shutdown();
        decoder.shutdown();
        return;
    }

    // --- input forwarding (keyboard/mouse/gamepad at 120 Hz) ---
    InputCapture input(receiver);
    if (!input.start())
        fprintf(stderr, "[client] input capture unavailable: %s "
                        "(view-only session)\n",
                input.lastError().c_str());

    // Wheel (the only path precision-touchpad scrolling takes) and the
    // absolute pointer come from the render window's message loop.
    window.onWheel = [&input](int vertical, int horizontal) {
        input.addWheel(vertical, horizontal);
    };
    window.onPointerAbs = [&input](uint16_t nx, uint16_t ny) {
        input.setAbsolutePointer(nx, ny);
    };
    input.setAbsoluteMouse(window.absoluteMouse());

    printf("[client] session up -- Shift+ESC ends the session, F1 stats, "
           "F2 mouse mode (abs/rel)\n");

    // --- render loop ---
    using Clock = std::chrono::steady_clock;
    constexpr auto kStatsInterval     = std::chrono::milliseconds(500);
    constexpr auto kIdleRepaint       = std::chrono::milliseconds(16);
    constexpr auto kReconnectInterval = std::chrono::seconds(3);

    auto lastStatsPush = Clock::now();
    auto lastPresent   = Clock::now();
    auto lastReconnect = Clock::now();

    while (!window.shouldClose() &&
           !interrupted.load(std::memory_order_acquire)) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> frame;
        uint64_t frameTs = 0;
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lock(mailbox.mutex);
            mailbox.cv.wait_for(lock, std::chrono::milliseconds(8),
                                [&mailbox] { return mailbox.fresh; });
            if (mailbox.fresh) {
                frame         = mailbox.texture;
                frameTs       = mailbox.timestampUs;
                mailbox.fresh = false;
                haveFrame     = true;
            }
        }

        // F2 in the render window flips the mode; mirror it into the
        // input pipeline (cheap atomic write).
        input.setAbsoluteMouse(window.absoluteMouse());

        const auto now = Clock::now();
        if (haveFrame) {
            window.presentFrame(frame.Get(), frameTs);
            lastPresent = now;
        } else if (now - lastPresent >= kIdleRepaint) {
            // No new frame: repaint the last one so the message loop
            // keeps pumping (Shift+ESC stays responsive during outages).
            window.presentFrame(nullptr, 0);
            lastPresent = now;
        }

        if (now - lastStatsPush >= kStatsInterval) {
            StreamStats stats = receiver.getStats();
            stats.latency = LatencyTracker::get().report();
            window.setStatsOverlay(stats);
            lastStatsPush = now;
        }

        if (disconnected.load(std::memory_order_acquire) &&
            now - lastReconnect >= kReconnectInterval) {
            lastReconnect = now;
            fprintf(stderr, "[client] reconnecting to %s:%u ...\n",
                    serverIp.c_str(), serverPort);
            receiver.stop(); // also resets reassembly/jitter state
            if (receiver.start()) {
                // Discovery ping sent; the decoder-primed gate requests a
                // fresh IDR as soon as frames flow again.
                decoderPrimed.store(false, std::memory_order_relaxed);
                disconnected.store(false, std::memory_order_release);
            } else {
                fprintf(stderr, "[client] reconnect failed: %s\n",
                        receiver.lastError().c_str());
            }
        }
    }

    // --- clean shutdown, reverse start order ---
    printf("[client] session ending...\n");
    input.stop();    // final all-released packet first, while the socket lives
    receiver.stop(); // no more decode()/onPacket() callbacks after this
    player.stop();
    {
        // Drop the mailbox's texture reference before the decoder
        // releases its ring.
        std::lock_guard<std::mutex> lock(mailbox.mutex);
        mailbox.texture.Reset();
        mailbox.fresh = false;
    }
    window.shutdown();
    decoder.shutdown();
}
