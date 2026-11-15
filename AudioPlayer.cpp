// AudioPlayer.cpp -- see AudioPlayer.h for the design overview.

#include "AudioPlayer.h"

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h> // needs mmdeviceapi.h's propsys deps first
#include <audioclient.h>
#include <avrt.h>
#include <wrl/client.h>

#include "extern/opus/include/opus.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "extern/opus/lib/x64/opus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "avrt.lib")

namespace {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ---------------------------------------------------------------------------
// Logging. Same shape as the other modules: debugger + stderr, stack buffer
// only, called on errors and state changes -- per-packet failure paths are
// counter-throttled.
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

std::string HrErrorString(HRESULT hr)
{
    char sysMsg[512] = {};
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        sysMsg, static_cast<DWORD>(sizeof(sysMsg)), nullptr);
    while (len > 0 && (sysMsg[len - 1] == '\r' || sysMsg[len - 1] == '\n'))
        sysMsg[--len] = '\0';

    char buf[600];
    if (len > 0)
        sprintf_s(buf, "0x%08X (%s)", static_cast<unsigned>(hr), sysMsg);
    else
        sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

uint64_t SteadyMicros()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

struct ComApartment {
    HRESULT hr;
    ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

struct MmcssBoost {
    HANDLE task = nullptr;
    MmcssBoost()
    {
        DWORD index = 0;
        task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    }
    ~MmcssBoost() { if (task) AvRevertMmThreadCharacteristics(task); }
};

void LogEndpointName(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.GetAddressOf())))
        return;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, name, sizeof(name) - 1,
                            nullptr, nullptr);
        LogF("[AudioPlayer] render endpoint: %s", name);
    }
    PropVariantClear(&value);
}

// Watches the default render endpoint; on a change it sets `flag` and
// signals `event` so the render thread wakes and reinitializes. Lives on
// the render thread's stack -- refcounting is a formality (never deleted
// through Release()).
class DeviceChangeNotifier : public IMMNotificationClient {
public:
    DeviceChangeNotifier(std::atomic<bool>& flag, HANDLE event)
        : m_flag(flag), m_event(event) {}

    // IUnknown -- stack lifetime, no self-delete.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR) override
    {
        if (flow == eRender && role == eConsole) {
            m_flag.store(true, std::memory_order_release);
            if (m_event)
                SetEvent(m_event);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    std::atomic<bool>& m_flag;
    HANDLE             m_event;
};

} // namespace

// ---------------------------------------------------------------------------
// RenderContext -- everything the render thread owns
// ---------------------------------------------------------------------------

struct AudioPlayer::RenderContext {
    ComPtr<IAudioClient>       client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock>        clock; // null = drift correction disabled
    UINT32 bufferFrames = 0;
    UINT64 clockFreq    = 0;

    // Consumer-side staging: the frame currently being drained into WASAPI
    // buffers (render chunk sizes don't align to 960-sample frames).
    std::array<float, kFrameFloats> cur{};
    size_t curPos = kFrameFloats; // == kFrameFloats means empty

    // Drift state.
    uint64_t framesSubmitted  = 0;
    double   emaBuffered      = -1.0; // stereo samples; <0 = unprimed
    uint64_t lastCorrectionUs = 0;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AudioPlayer::~AudioPlayer()
{
    stop();
}

bool AudioPlayer::start()
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    m_stopRequested.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        int err = 0;
        m_decoder = opus_decoder_create(kSampleRate, kChannels, &err);
        if (!m_decoder || err != OPUS_OK) {
            char msg[96];
            sprintf_s(msg, "opus_decoder_create failed: %d", err);
            setError(msg);
            m_decoder = nullptr;
            LogF("[AudioPlayer] %s", lastError().c_str());
            return false;
        }
        m_decodeBuf.assign(kFrameFloats, 0.0f);
        m_lastPacketValid = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        m_ringHead  = 0;
        m_ringCount = 0;
        m_primed    = false;
    }

    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_event) {
        setError("CreateEvent failed: " +
                 HrErrorString(HRESULT_FROM_WIN32(GetLastError())));
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        opus_decoder_destroy(m_decoder);
        m_decoder = nullptr;
        LogF("[AudioPlayer] %s", lastError().c_str());
        return false;
    }

    std::promise<bool> ready;
    std::future<bool> readyResult = ready.get_future();
    m_thread = std::thread(&AudioPlayer::renderThreadProc, this, std::move(ready));

    if (!readyResult.get()) {
        m_thread.join();
        CloseHandle(m_event);
        m_event = nullptr;
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        opus_decoder_destroy(m_decoder);
        m_decoder = nullptr;
        LogF("[AudioPlayer] %s", lastError().c_str());
        return false;
    }

    m_running.store(true, std::memory_order_release);
    LogF("[AudioPlayer] started (Opus -> WASAPI shared, %zu-slot jitter ring)",
         kRingSlots);
    return true;
}

void AudioPlayer::stop()
{
    if (!m_thread.joinable() && !m_decoder)
        return;

    m_running.store(false, std::memory_order_release); // onPacket no-ops now
    m_stopRequested.store(true, std::memory_order_release);

    if (m_thread.joinable()) {
        SetEvent(m_event);
        m_thread.join();
    }
    if (m_event) {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        if (m_decoder) {
            opus_decoder_destroy(m_decoder);
            m_decoder = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        m_ringHead  = 0;
        m_ringCount = 0;
        m_primed    = false;
    }

    m_stopRequested.store(false, std::memory_order_release);
    LogF("[AudioPlayer] stopped");
}

std::string AudioPlayer::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void AudioPlayer::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(message);
}

// ---------------------------------------------------------------------------
// Producer side: decode + PLC on the receiver thread
// ---------------------------------------------------------------------------

void AudioPlayer::onPacket(const std::vector<uint8_t>& opusData, uint64_t timestampUs)
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(m_decoderMutex);
    if (!m_decoder)
        return;

    // A timestamp gap beyond one frame + 5 ms slack means packets were
    // lost on the wire (the capture side stamps exactly 20 ms apart) --
    // conceal the hole before decoding the packet that revealed it.
    if (m_lastPacketValid && timestampUs > m_lastPacketTs &&
        timestampUs - m_lastPacketTs > kGapThresholdUs) {
        uint32_t missing = static_cast<uint32_t>(
            (timestampUs - m_lastPacketTs + kFrameDurationUs / 2) / kFrameDurationUs);
        missing = missing > 0 ? missing - 1 : 0;
        if (missing > kMaxPlcFrames)
            missing = kMaxPlcFrames;
        for (uint32_t k = 0; k < missing; ++k) {
            const int n = opus_decode_float(m_decoder, nullptr, 0,
                                            m_decodeBuf.data(), kFrameSamples, 0);
            if (n <= 0)
                break;
            pushFrame(m_decodeBuf.data(),
                      m_lastPacketTs + (k + 1) * kFrameDurationUs);
            const uint32_t total = m_plcFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            if (total == 1 || total % 500 == 0)
                LogF("[AudioPlayer] packet loss concealed (%u PLC frames so far)",
                     total);
        }
    }

    const int n = opus_decode_float(m_decoder, opusData.data(),
                                    static_cast<int>(opusData.size()),
                                    m_decodeBuf.data(), kFrameSamples, 0);
    if (n <= 0) {
        const uint32_t fails = m_decodeFails.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fails == 1 || fails % 500 == 0)
            LogF("[AudioPlayer] opus_decode_float failed: %d (%u so far)", n, fails);
        return;
    }
    if (n < static_cast<int>(kFrameSamples)) {
        // Shorter frame than this pipeline ever produces; zero-pad so the
        // ring keeps its fixed 20 ms cadence.
        memset(m_decodeBuf.data() + static_cast<size_t>(n) * kChannels, 0,
               (kFrameFloats - static_cast<size_t>(n) * kChannels) * sizeof(float));
    }

    pushFrame(m_decodeBuf.data(), timestampUs);
    m_lastPacketTs    = timestampUs;
    m_lastPacketValid = true;
}

void AudioPlayer::pushFrame(const float* pcm, uint64_t timestampUs)
{
    std::lock_guard<std::mutex> lock(m_ringMutex);
    if (m_ringCount == kRingSlots) {
        // Consumer is behind (or the stream outruns the device) -- drop
        // the oldest frame to stay near live.
        m_ringHead = (m_ringHead + 1) % kRingSlots;
        --m_ringCount;
        const uint32_t drops = m_ringDrops.fetch_add(1, std::memory_order_relaxed) + 1;
        if (drops == 1 || drops % 100 == 0)
            LogF("[AudioPlayer] jitter ring full -- dropped oldest frame "
                 "(%u so far)",
                 drops);
    }
    JitterSlot& slot = m_ring[(m_ringHead + m_ringCount) % kRingSlots];
    slot.timestampUs = timestampUs;
    memcpy(slot.pcm.data(), pcm, kFrameFloats * sizeof(float));
    ++m_ringCount;
}

// ---------------------------------------------------------------------------
// Consumer side: render thread
// ---------------------------------------------------------------------------

bool AudioPlayer::popFrame(float* pcm)
{
    std::lock_guard<std::mutex> lock(m_ringMutex);
    if (!m_primed) {
        if (m_ringCount < kPrimeFrames)
            return false; // keep playing silence until the ring fills
        m_primed = true;
    }
    if (m_ringCount == 0) {
        // Full drain: re-prime so one packet doesn't restart a stutter
        // cycle. Silence here on purpose -- PLC is for network loss only.
        m_primed = false;
        const uint32_t underruns = m_underruns.fetch_add(1, std::memory_order_relaxed) + 1;
        if (underruns == 1 || underruns % 100 == 0)
            LogF("[AudioPlayer] jitter ring underrun -- silence (%u so far)",
                 underruns);
        return false;
    }
    memcpy(pcm, m_ring[m_ringHead].pcm.data(), kFrameFloats * sizeof(float));
    m_ringHead = (m_ringHead + 1) % kRingSlots;
    --m_ringCount;
    return true;
}

uint32_t AudioPlayer::ringSamples() const
{
    std::lock_guard<std::mutex> lock(m_ringMutex);
    return static_cast<uint32_t>(m_ringCount) * kFrameSamples;
}

void AudioPlayer::fillBuffer(RenderContext& ctx, float* dst, uint32_t frames)
{
    uint32_t written = 0;
    while (written < frames) {
        if (ctx.curPos >= kFrameFloats) {
            if (!popFrame(ctx.cur.data()))
                break;
            ctx.curPos = 0;
        }
        const size_t want = static_cast<size_t>(frames - written) * kChannels;
        const size_t have = kFrameFloats - ctx.curPos;
        const size_t copyFloats = want < have ? want : have;
        memcpy(dst + static_cast<size_t>(written) * kChannels,
               ctx.cur.data() + ctx.curPos, copyFloats * sizeof(float));
        ctx.curPos += copyFloats;
        written += static_cast<uint32_t>(copyFloats / kChannels);
    }
    if (written < frames)
        memset(dst + static_cast<size_t>(written) * kChannels, 0,
               static_cast<size_t>(frames - written) * kChannels * sizeof(float));
}

bool AudioPlayer::initRender(RenderContext& ctx, IMMDeviceEnumerator* enumerator)
{
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                     device.GetAddressOf());
    if (FAILED(hr)) {
        setError("GetDefaultAudioEndpoint(eRender) failed: " + HrErrorString(hr));
        return false;
    }
    LogEndpointName(device.Get());

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(ctx.client.GetAddressOf()));
    if (FAILED(hr)) {
        setError("IMMDevice::Activate(IAudioClient) failed: " + HrErrorString(hr));
        return false;
    }

    // We always hand the engine 48 kHz stereo float32; AUTOCONVERTPCM +
    // SRC lets the engine convert to whatever the device mix format is,
    // so no render-side resampler is needed.
    WAVEFORMATEX wf      = {};
    wf.wFormatTag        = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels         = static_cast<WORD>(kChannels);
    wf.nSamplesPerSec    = kSampleRate;
    wf.wBitsPerSample    = 32;
    wf.nBlockAlign       = static_cast<WORD>(kChannels * sizeof(float));
    wf.nAvgBytesPerSec   = kSampleRate * wf.nBlockAlign;

    constexpr REFERENCE_TIME kBufferDuration = 200'000; // 20 ms in 100 ns units
    hr = ctx.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                kBufferDuration, 0, &wf, nullptr);
    if (FAILED(hr)) {
        setError("IAudioClient::Initialize(render) failed: " + HrErrorString(hr));
        return false;
    }

    hr = ctx.client->SetEventHandle(m_event);
    if (FAILED(hr)) {
        setError("SetEventHandle failed: " + HrErrorString(hr));
        return false;
    }

    hr = ctx.client->GetBufferSize(&ctx.bufferFrames);
    if (FAILED(hr)) {
        setError("GetBufferSize failed: " + HrErrorString(hr));
        return false;
    }

    hr = ctx.client->GetService(IID_PPV_ARGS(ctx.render.GetAddressOf()));
    if (FAILED(hr)) {
        setError("GetService(IAudioRenderClient) failed: " + HrErrorString(hr));
        return false;
    }

    // IAudioClock is best effort: without it we just skip drift correction.
    if (SUCCEEDED(ctx.client->GetService(IID_PPV_ARGS(ctx.clock.GetAddressOf())))) {
        if (FAILED(ctx.clock->GetFrequency(&ctx.clockFreq)) || ctx.clockFreq == 0)
            ctx.clock.Reset();
    }
    if (!ctx.clock)
        LogF("[AudioPlayer] IAudioClock unavailable -- drift correction disabled");

    // Pre-fill the device buffer with silence so Start() has data.
    BYTE* prefill = nullptr;
    hr = ctx.render->GetBuffer(ctx.bufferFrames, &prefill);
    if (FAILED(hr)) {
        setError("prefill GetBuffer failed: " + HrErrorString(hr));
        return false;
    }
    ctx.render->ReleaseBuffer(ctx.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    ctx.framesSubmitted = ctx.bufferFrames;

    hr = ctx.client->Start();
    if (FAILED(hr)) {
        setError("IAudioClient::Start failed: " + HrErrorString(hr));
        return false;
    }

    LogF("[AudioPlayer] render buffer %u frames (%0.1f ms)", ctx.bufferFrames,
         ctx.bufferFrames * 1000.0 / kSampleRate);
    return true;
}

void AudioPlayer::renderThreadProc(std::promise<bool> ready)
{
    SetThreadDescription(GetCurrentThread(), L"Audio-Render");

    ComApartment com;
    if (FAILED(com.hr)) {
        setError("CoInitializeEx failed: " + HrErrorString(com.hr));
        ready.set_value(false);
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT ehr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(ehr)) {
        setError("CoCreateInstance(MMDeviceEnumerator) failed: " + HrErrorString(ehr));
        ready.set_value(false);
        return;
    }

    // Watch for default-endpoint switches. Best effort: playback works
    // without it, it just stays glued to the old device.
    m_deviceChanged.store(false, std::memory_order_relaxed);
    DeviceChangeNotifier notifier(m_deviceChanged, m_event);
    const bool notifierRegistered =
        SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(&notifier));
    if (!notifierRegistered)
        LogF("[AudioPlayer] endpoint notifications unavailable -- device "
             "changes will not be followed");

    RenderContext ctx;
    if (!initRender(ctx, enumerator.Get())) {
        if (notifierRegistered)
            enumerator->UnregisterEndpointNotificationCallback(&notifier);
        ready.set_value(false);
        return; // ComPtrs in ctx release before CoUninitialize
    }

    MmcssBoost boost; // best effort

    ready.set_value(true);

    // Total buffered audio the drift logic aims for: the priming level
    // plus whatever WASAPI itself holds. Recomputed after a device swap
    // (the new device's buffer size may differ).
    double targetSamples =
        static_cast<double>(kPrimeFrames) * kFrameSamples + ctx.bufferFrames;

    bool needReinit = false;
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        if (needReinit) {
            // Default endpoint changed or the device died: rebuild WASAPI
            // on the current default. Decoder + jitter ring survive.
            if (ctx.client)
                ctx.client->Stop();
            ctx = RenderContext{};
            m_deviceChanged.store(false, std::memory_order_relaxed);
            LogF("[AudioPlayer] audio device changed; reinitializing render");
            while (!m_stopRequested.load(std::memory_order_acquire) &&
                   !initRender(ctx, enumerator.Get())) {
                ctx = RenderContext{};
                Sleep(500); // no usable device right now; keep retrying
            }
            if (m_stopRequested.load(std::memory_order_acquire))
                break;
            targetSamples =
                static_cast<double>(kPrimeFrames) * kFrameSamples + ctx.bufferFrames;
            needReinit = false;
        }

        const DWORD wait = WaitForSingleObject(m_event, 2000);
        if (m_stopRequested.load(std::memory_order_acquire))
            break;
        if (m_deviceChanged.load(std::memory_order_acquire)) {
            needReinit = true;
            continue;
        }
        if (wait == WAIT_TIMEOUT)
            continue;
        if (wait != WAIT_OBJECT_0) {
            LogF("[AudioPlayer] event wait failed (%lu) -- render thread exiting",
                 wait);
            break;
        }

        UINT32 padding = 0;
        HRESULT hr = ctx.client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            // AUDCLNT_E_DEVICE_INVALIDATED (unplugged) lands here -- treat
            // every render failure as "rebuild on current default".
            setError("GetCurrentPadding failed: " + HrErrorString(hr));
            LogF("[AudioPlayer] %s -- reinitializing", lastError().c_str());
            needReinit = true;
            continue;
        }
        const UINT32 avail = ctx.bufferFrames - padding;
        if (avail == 0)
            continue;

        BYTE* raw = nullptr;
        hr = ctx.render->GetBuffer(avail, &raw);
        if (FAILED(hr)) {
            setError("render GetBuffer failed: " + HrErrorString(hr));
            LogF("[AudioPlayer] %s -- reinitializing", lastError().c_str());
            needReinit = true;
            continue;
        }
        fillBuffer(ctx, reinterpret_cast<float*>(raw), avail);
        hr = ctx.render->ReleaseBuffer(avail, 0);
        if (FAILED(hr)) {
            setError("ReleaseBuffer failed: " + HrErrorString(hr));
            LogF("[AudioPlayer] %s -- reinitializing", lastError().c_str());
            needReinit = true;
            continue;
        }
        ctx.framesSubmitted += avail;

        // Drift correction: sender and device clocks differ by design, so
        // buffered audio slowly creeps up (sender fast) -- measure actual
        // playback via IAudioClock and shed one frame when the smoothed
        // excess passes 5 ms. The slow direction self-corrects through
        // underrun silence.
        if (ctx.clock) {
            UINT64 pos = 0;
            if (SUCCEEDED(ctx.clock->GetPosition(&pos, nullptr))) {
                const double played =
                    static_cast<double>(pos) / static_cast<double>(ctx.clockFreq) *
                    kSampleRate;
                const double inDevice =
                    static_cast<double>(ctx.framesSubmitted) - played;
                const double staged =
                    static_cast<double>(kFrameFloats - ctx.curPos) / kChannels;
                const double total = inDevice + ringSamples() + staged;
                ctx.emaBuffered = ctx.emaBuffered < 0.0
                                      ? total
                                      : ctx.emaBuffered * 0.9 + total * 0.1;

                const uint64_t nowUs = SteadyMicros();
                if (ctx.emaBuffered > targetSamples + kDriftSamples &&
                    nowUs - ctx.lastCorrectionUs > kDriftCooldownUs) {
                    bool dropped = false;
                    {
                        std::lock_guard<std::mutex> lock(m_ringMutex);
                        if (m_ringCount > 1) {
                            m_ringHead = (m_ringHead + 1) % kRingSlots;
                            --m_ringCount;
                            dropped = true;
                        }
                    }
                    if (dropped) {
                        ctx.lastCorrectionUs = nowUs;
                        ctx.emaBuffered -= kFrameSamples;
                        LogF("[AudioPlayer] drift correction: dropped one 20 ms "
                             "frame (buffered %.1f ms)",
                             ctx.emaBuffered * 1000.0 / kSampleRate);
                    }
                }
            }
        }
    }

    if (ctx.client)
        ctx.client->Stop();
    if (notifierRegistered)
        enumerator->UnregisterEndpointNotificationCallback(&notifier);
}
