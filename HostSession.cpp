// HostSession.cpp -- see HostSession.h for the contract.
//
// Pipeline (identical to the former server_main):
//
//   ScreenCapture (own capture thread, triple-buffer GPU ring)
//        |  getLatestFrame()                      [Pipeline-Encode thread]
//   NvencEncoder::encode()  ->  EncodedFrame
//        |  sendFrame()
//   StreamSender (own GSMR-Send thread: fragmentation, discovery, keepalive)
//
//   AudioCapture (own Audio-Capture thread)  ->  sendAudio()
//   StreamSender::onInputPacket (GSMR-Send thread)  ->  InputInjector::inject()
//
// The capture/encode consumer is ONE thread by contract: NvencEncoder
// requires initialize()/encode() on the thread that calls
// ScreenCapture::getLatestFrame(), so "Pipeline-Encode" owns both ends.
//
// Loss recovery: the client sends keyframe requests (KeepaliveKind::
// kKeyframeRequest) whenever it detects unconcealable loss or joins
// mid-stream; the encode thread turns those into IDRs. Between IDRs the
// stream self-heals via NVENC intra refresh (no periodic IDR pulse). The
// client's 1 Hz receiver report drives the adaptive bitrate controller
// (back off 30% on >2% loss, ramp 15% after 5 clean seconds; the config's
// bitrate_kbps is the MAXIMUM).
//
// Stop conditions: status window closed, or `interrupted` set. Session
// state lives in file-scope statics (one host session at a time; they are
// reset on every entry).

#include "StreamSender.h"   // pulls winsock2.h before windows.h
#include "AudioCapture.h"
#include "Discovery.h"
#include "HostSession.h"
#include "InputInjector.h"
#include "LatencyTracker.h"
#include "NvencEncoder.h"
#include "ScreenCapture.h"
#include "StatusWindow.h"
#include "VideoScaler.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

// One host session at a time; reset at RunHostSession entry.
std::atomic<bool>     s_stopRequested{false};
std::atomic<bool>     s_keyframeRequested{false};
std::atomic<uint32_t> s_reportExpected{0};
std::atomic<uint32_t> s_reportCompleted{0};
std::atomic<bool>     s_reportFresh{false};

// "--" for stages the tracker could not measure (see LatencyTracker.h).
const char* FormatMs(float ms, char (&buf)[16])
{
    if (ms < 0.0f)
        return "  --";
    sprintf_s(buf, "%4.1f", static_cast<double>(ms));
    return buf;
}

// ---------------------------------------------------------------------------
// IDR detection. The transport needs the keyframe flag (keyframes are never
// dropped from the outbound ring) but NvencEncoder returns raw Annex B, so
// scan the start codes for a NAL of type 5 (IDR slice).
// ---------------------------------------------------------------------------

bool ContainsIdrNal(const std::vector<uint8_t>& annexB)
{
    const size_t n = annexB.size();
    for (size_t i = 0; i + 3 < n;) {
        if (annexB[i] == 0 && annexB[i + 1] == 0) {
            if (annexB[i + 2] == 1) {
                if ((annexB[i + 3] & 0x1F) == 5)
                    return true;
                i += 3;
                continue;
            }
            if (annexB[i + 2] == 0 && i + 4 < n && annexB[i + 3] == 1) {
                if ((annexB[i + 4] & 0x1F) == 5)
                    return true;
                i += 4;
                continue;
            }
        }
        ++i;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Pipeline-Encode thread: capture consumer + NVENC producer.
// ---------------------------------------------------------------------------

void EncodeThreadProc(ScreenCapture& capture, StreamSender& sender,
                      const AppConfig& config, std::promise<bool> ready)
{
    SetThreadDescription(GetCurrentThread(), L"Pipeline-Encode");

    NvencEncoder encoder;
    NvencEncoder::Config encoderConfig;
    encoderConfig.bitrateBps   = config.bitrateKbps * 1000;
    encoderConfig.frameRateNum = config.fpsTarget;
    encoderConfig.frameRateDen = 1;

    if (!encoder.initialize(capture.device(), encoderConfig)) {
        fprintf(stderr, "[host] NVENC initialization failed: %s\n",
                encoder.lastError().c_str());
        ready.set_value(false);
        return;
    }
    ready.set_value(true);

    // Optional GPU downscale before NVENC (encode_height in the config).
    // Failure is not fatal: fall back to encoding at native resolution.
    VideoScaler scaler;
    bool useScaler = false;
    if (config.encodeHeight > 0) {
        if (scaler.initialize(capture.device(), config.encodeHeight))
            useScaler = true;
        else
            fprintf(stderr, "[host] scaler unavailable (%s); "
                            "encoding at native resolution\n",
                    scaler.lastError().c_str());
    }

    using Clock = std::chrono::steady_clock;
    const auto frameInterval =
        std::chrono::microseconds(1'000'000 / config.fpsTarget);
    // waitForNewFrame budget: one frame interval, floored at 1 ms so a
    // 240 fps target still yields to the OS between polls.
    const uint32_t frameWaitMs =
        1000 / config.fpsTarget > 0 ? 1000 / config.fpsTarget : 1;

    // Adaptive bitrate controller state. The encoder does the actual
    // reconfiguration (setBitrate -> NvEncReconfigureEncoder, no IDR);
    // this loop just decides the target from the client's 1 Hz receiver
    // reports.
    const uint32_t maxBitrateBps = config.bitrateKbps * 1000;
    const uint32_t minBitrateBps =
        maxBitrateBps / 8 > 2'000'000 ? maxBitrateBps / 8 : 2'000'000;
    uint32_t currentBitrateBps = maxBitrateBps;
    auto     lastRateChange    = Clock::now() - std::chrono::seconds(10);
    int      cleanReports      = 0;

    auto nextDeadline    = Clock::now();
    uint32_t encodeFails = 0;

    while (!s_stopRequested.load(std::memory_order_acquire)) {
        // Frame-driven pacing: first honor the fps cap (never encode two
        // frames closer than frameInterval), then wait until a NEW capture
        // is published -- encoding starts the moment the frame lands
        // instead of at the next fixed tick, which removes up to one frame
        // interval of avoidable latency. On timeout (static desktop) the
        // previous frame is re-encoded, exactly like the old fixed tick.
        if (const auto now = Clock::now(); nextDeadline > now)
            std::this_thread::sleep_until(nextDeadline);
        capture.waitForNewFrame(frameWaitMs);
        nextDeadline = Clock::now() + frameInterval;

        // Client asked for an IDR (join, packet loss). Periodic IDRs are
        // gone: intra refresh heals silent corruption, and the client
        // re-requests until a keyframe actually arrives.
        if (s_keyframeRequested.exchange(false, std::memory_order_acq_rel))
            encoder.requestKeyframe();

        // Fold the latest receiver report into the bitrate controller.
        if (s_reportFresh.exchange(false, std::memory_order_acq_rel)) {
            const uint32_t expected  = s_reportExpected.load(std::memory_order_relaxed);
            const uint32_t completed = s_reportCompleted.load(std::memory_order_relaxed);
            const bool lossy = expected > 0 &&
                               (expected - completed) * 100 > expected * 2; // >2%
            if (lossy) {
                cleanReports = 0;
                if (Clock::now() - lastRateChange >= std::chrono::seconds(2)) {
                    const uint32_t next =
                        currentBitrateBps * 7 / 10 > minBitrateBps
                            ? currentBitrateBps * 7 / 10 : minBitrateBps;
                    if (next != currentBitrateBps) {
                        currentBitrateBps = next;
                        encoder.setBitrate(currentBitrateBps);
                        lastRateChange = Clock::now();
                        fprintf(stderr, "[host] loss reported; bitrate -> %u kbps\n",
                                currentBitrateBps / 1000);
                    }
                }
            } else if (currentBitrateBps < maxBitrateBps && ++cleanReports >= 5) {
                cleanReports = 0;
                const uint64_t ramped =
                    static_cast<uint64_t>(currentBitrateBps) * 115 / 100;
                currentBitrateBps = ramped < maxBitrateBps
                    ? static_cast<uint32_t>(ramped) : maxBitrateBps;
                encoder.setBitrate(currentBitrateBps);
                lastRateChange = Clock::now();
            }
        }

        CaptureFrame frame{};
        if (!capture.getLatestFrame(frame))
            continue; // nothing captured yet (mode change, monitor asleep)
        LatencyTracker::get().mark(frame.timestamp_us, Stage::CAPTURE_START);

        // Downscale on the GPU first when configured; on a scaler failure
        // (driver hiccup) encode the native frame rather than dropping it.
        ID3D11Texture2D* encodeTexture = frame.texture;
        if (useScaler) {
            if (ID3D11Texture2D* scaled = scaler.scale(frame.texture))
                encodeTexture = scaled;
        }

        LatencyTracker::get().mark(frame.timestamp_us, Stage::ENCODE_START);
        std::vector<uint8_t> bitstream = encoder.encode(encodeTexture);
        LatencyTracker::get().mark(frame.timestamp_us, Stage::ENCODE_END);
        if (bitstream.empty()) {
            ++encodeFails;
            if (encodeFails <= 3 || encodeFails % 300 == 0)
                fprintf(stderr, "[host] encode failed: %s\n",
                        encoder.lastError().c_str());
            continue;
        }
        encodeFails = 0;

        EncodedFrame encoded;
        encoded.isKeyframe  = ContainsIdrNal(bitstream);
        encoded.nalUnits    = std::move(bitstream);
        encoded.timestampUs = frame.timestamp_us;
        LatencyTracker::get().mark(encoded.timestampUs, Stage::SEND_START);
        sender.sendFrame(std::move(encoded));
    }

    encoder.shutdown(); // same thread as initialize()/encode(), per contract
}

} // namespace

// ---------------------------------------------------------------------------
// RunHostSession
// ---------------------------------------------------------------------------

int RunHostSession(const AppConfig& config, const std::atomic<bool>& interrupted)
{
    s_stopRequested.store(false, std::memory_order_release);
    s_keyframeRequested.store(false, std::memory_order_release);
    s_reportFresh.store(false, std::memory_order_release);

    printf("[host] hosting -- port %u, up to %u kbps, %u fps target\n",
           config.port, config.bitrateKbps, config.fpsTarget);

    // --- IP-free pairing: LAN discovery responder + pairing code ---
    // Both are conveniences; failures are logged and hosting continues
    // (a manually typed ip:port still works).
    const std::string localIp     = GetLocalIpv4();
    const std::string pairingCode = EncodePairingCode(localIp, config.port);
    DiscoveryResponder responder;
    if (!responder.start(config.port, !config.psk.empty()))
        fprintf(stderr, "[host] LAN discovery unavailable: %s\n",
                responder.lastError().c_str());
    if (!pairingCode.empty())
        printf("[host] pairing code: %s  (local %s:%u)\n",
               pairingCode.c_str(), localIp.c_str(), config.port);
    else
        fprintf(stderr, "[host] no local IPv4 found; pairing code unavailable\n");

    // --- status window (pairing code + live state; closing it ends the
    // session and returns to the launcher) ---
    StatusWindow statusWindow;
    if (!statusWindow.start(pairingCode, localIp, config.port))
        fprintf(stderr, "[host] status window unavailable; continuing headless\n");

    // --- capture (device owner) ---
    ScreenCapture capture;
    if (!capture.startCapture()) {
        fprintf(stderr, "[host] screen capture failed to start\n");
        statusWindow.stop();
        responder.stop();
        return 1;
    }

    // --- input injection (degrades to keyboard/mouse-only without ViGEm) ---
    InputInjector injector;
    injector.start();

    // --- transport ---
    StreamSender sender(config.port);
    sender.onInputPacket = [&injector](std::vector<uint8_t> payload, uint64_t) {
        injector.inject(payload);
    };
    sender.onKeyframeRequest = []() {
        s_keyframeRequested.store(true, std::memory_order_release);
    };
    sender.onReceiverReport = [](uint32_t expected, uint32_t completed) {
        s_reportExpected.store(expected, std::memory_order_relaxed);
        s_reportCompleted.store(completed, std::memory_order_relaxed);
        s_reportFresh.store(true, std::memory_order_release);
    };
    // Bağlantı onayı: yeni uç nokta durum penceresinde "İzin ver / Reddet"
    // istemi açar; karar aşağıdaki izleme döngüsünde toplanıp sender'a
    // iletilir. (Onay kapalıyken eski davranış: ilk geçerli datagram kazanır.)
    sender.setRequireApproval(config.requireApproval);
    sender.onClientRequest = [&statusWindow](const std::string& addr) {
        statusWindow.requestApproval(addr);
    };
    if (!sender.setPsk(config.psk)) {
        // A configured PSK that cannot be applied must not fall back to
        // plaintext silently.
        fprintf(stderr, "[host] transport encryption setup failed: %s\n",
                sender.lastError().c_str());
        injector.stop();
        capture.stopCapture();
        statusWindow.stop();
        responder.stop();
        return 1;
    }
    if (!sender.start()) {
        fprintf(stderr, "[host] transport failed to start: %s\n",
                sender.lastError().c_str());
        injector.stop();
        capture.stopCapture();
        statusWindow.stop();
        responder.stop();
        return 1;
    }

    // --- audio (optional: hosting without audio still streams video) ---
    AudioCapture audio(sender);
    if (!audio.start())
        fprintf(stderr, "[host] audio capture unavailable: %s "
                        "(continuing without audio)\n",
                audio.lastError().c_str());

    // --- encode pipeline ---
    std::promise<bool> encoderReady;
    std::future<bool> encoderReadyFuture = encoderReady.get_future();
    std::thread encodeThread(EncodeThreadProc, std::ref(capture), std::ref(sender),
                             std::cref(config), std::move(encoderReady));
    if (!encoderReadyFuture.get()) {
        s_stopRequested.store(true, std::memory_order_release);
        encodeThread.join();
        audio.stop();
        sender.stop();
        injector.stop();
        capture.stopCapture();
        statusWindow.stop();
        responder.stop();
        return 1;
    }

    printf("[host] pipeline up; clients find this machine via LAN discovery "
           "or the pairing code. Close the status window to stop.\n");

    // --- monitor loop: console + status window refresh every 2 seconds ---
    while (!s_stopRequested.load(std::memory_order_acquire)) {
        for (int i = 0; i < 20; ++i) {
            Sleep(100);
            // Onay istemi yanıtlandıysa kararı transporta ilet (100 ms
            // tanecikli; kullanıcı etkileşimi için fazlasıyla hızlı).
            if (const int decision = statusWindow.takeApprovalDecision())
                sender.resolvePending(decision > 0);
            if (statusWindow.closeRequested() ||
                interrupted.load(std::memory_order_acquire)) {
                s_stopRequested.store(true, std::memory_order_release);
                break;
            }
        }
        if (s_stopRequested.load(std::memory_order_acquire))
            break;

        const StreamStats stats = sender.getStats();
        const LatencyReport latency = LatencyTracker::get().report();
        const std::string clientAddr = sender.clientAddress();
        const bool streaming = stats.bitrateKbps > 0.0f;
        statusWindow.setStreaming(streaming, clientAddr);
        statusWindow.setStats(stats.bitrateKbps, stats.rttMs, stats.packetLossPct);
        char captureBuf[16], encodeBuf[16];
        printf("[host] %-18s rtt %6.1f ms | %8.0f kbps | loss %5.2f%% | "
               "dropped %u | cap %s ms | enc %s ms\n",
               streaming ? "streaming" : "waiting for client",
               static_cast<double>(stats.rttMs),
               static_cast<double>(stats.bitrateKbps),
               static_cast<double>(stats.packetLossPct),
               stats.droppedFrames,
               FormatMs(latency.captureMs, captureBuf),
               FormatMs(latency.encodeMs, encodeBuf));
        fflush(stdout);
    }

    printf("[host] session ending...\n");
    encodeThread.join(); // encoder torn down on its own thread
    audio.stop();
    sender.stop();
    injector.stop();
    capture.stopCapture();
    statusWindow.stop();
    responder.stop();
    printf("[host] session closed\n");
    return 0;
}
