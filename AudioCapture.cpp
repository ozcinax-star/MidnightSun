// AudioCapture.cpp -- see AudioCapture.h for the design overview.

#include "AudioCapture.h"

#include <windows.h> // winsock2.h already pulled by our header
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
// only, called on errors and state changes -- never per packet (the one
// per-frame failure path is counter-throttled).
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

// Scoped COM apartment + MMCSS boost for the capture thread.
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

// Subformat GUIDs, spelled out so we don't depend on ks.h/ksmedia.h
// include order (data layout is the standard KSDATAFORMAT_SUBTYPE_*).
constexpr GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// Standard downmix weights for WAVEFORMATEXTENSIBLE channel order
// (FL FR FC LFE BL BR SL SR): center/back/side folded at -3 dB, LFE dropped.
constexpr float kDownmixL[8] = {1.0f, 0.0f, 0.7071f, 0.0f, 0.7071f, 0.0f, 0.7071f, 0.0f};
constexpr float kDownmixR[8] = {0.0f, 1.0f, 0.7071f, 0.0f, 0.0f, 0.7071f, 0.0f, 0.7071f};

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
        LogF("[AudioCapture] loopback endpoint: %s", name);
    }
    PropVariantClear(&value);
}

// Watches the default render endpoint; on a change it sets `flag` and
// signals `event` so the capture thread wakes and reinitializes. Lives on
// the capture thread's stack -- refcounting is a formality (never deleted
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
// CaptureContext -- everything the capture thread owns
// ---------------------------------------------------------------------------

struct AudioCapture::CaptureContext {
    ComPtr<IAudioClient>        client;
    ComPtr<IAudioCaptureClient> capture;
    uint32_t bufferFrames = 0;

    // Source (mix) format, parsed in initCapture().
    uint32_t srcRate     = 0;
    uint16_t srcChannels = 0;
    bool     srcIsFloat  = false; // false = int16

    // Linear resampler state (continuous across packets).
    double resPhase  = 0.0;
    float  resPrevL  = 0.0f;
    float  resPrevR  = 0.0f;
    bool   resPrimed = false;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AudioCapture::AudioCapture(StreamSender& sender)
    : m_sender(sender)
{
}

AudioCapture::~AudioCapture()
{
    stop();
}

bool AudioCapture::start()
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    m_stopRequested.store(false, std::memory_order_release);

    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_event) {
        setError("CreateEvent failed: " + HrErrorString(HRESULT_FROM_WIN32(GetLastError())));
        LogF("[AudioCapture] %s", lastError().c_str());
        return false;
    }

    std::promise<bool> ready;
    std::future<bool> readyResult = ready.get_future();
    m_thread = std::thread(&AudioCapture::captureThreadProc, this, std::move(ready));

    if (!readyResult.get()) {
        m_thread.join();
        CloseHandle(m_event);
        m_event = nullptr;
        LogF("[AudioCapture] %s", lastError().c_str());
        return false;
    }

    m_running.store(true, std::memory_order_release);
    LogF("[AudioCapture] started (WASAPI loopback -> Opus %d kbps, 20 ms frames)",
         kOpusBitrate / 1000);
    return true;
}

void AudioCapture::stop()
{
    if (!m_thread.joinable())
        return;

    m_stopRequested.store(true, std::memory_order_release);
    SetEvent(m_event); // wake the capture wait immediately
    m_thread.join();

    CloseHandle(m_event);
    m_event = nullptr;
    m_running.store(false, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
    LogF("[AudioCapture] stopped");
}

std::string AudioCapture::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void AudioCapture::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(message);
}

// ---------------------------------------------------------------------------
// WASAPI initialization (capture thread)
// ---------------------------------------------------------------------------

bool AudioCapture::initCapture(CaptureContext& ctx, IMMDeviceEnumerator* enumerator)
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

    // Shared-mode loopback must use the device mix format.
    WAVEFORMATEX* mix = nullptr;
    hr = ctx.client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        setError("GetMixFormat failed: " + HrErrorString(hr));
        return false;
    }

    bool formatOk = false;
    if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix->wBitsPerSample == 32) {
        ctx.srcIsFloat = true;
        formatOk = true;
    } else if (mix->wFormatTag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 16) {
        ctx.srcIsFloat = false;
        formatOk = true;
    } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        if (IsEqualGUID(ext->SubFormat, kSubtypeFloat) && mix->wBitsPerSample == 32) {
            ctx.srcIsFloat = true;
            formatOk = true;
        } else if (IsEqualGUID(ext->SubFormat, kSubtypePcm) && mix->wBitsPerSample == 16) {
            ctx.srcIsFloat = false;
            formatOk = true;
        }
    }
    ctx.srcRate     = mix->nSamplesPerSec;
    ctx.srcChannels = mix->nChannels;

    if (!formatOk || ctx.srcChannels == 0 || ctx.srcChannels > 8 || ctx.srcRate == 0) {
        char msg[160];
        sprintf_s(msg, "unsupported mix format: tag=%u bits=%u ch=%u rate=%lu",
                  mix->wFormatTag, mix->wBitsPerSample, mix->nChannels,
                  mix->nSamplesPerSec);
        setError(msg);
        CoTaskMemFree(mix);
        return false;
    }

    // 50 ms buffer: small enough to never matter for latency (we drain on
    // every event), big enough to ride out scheduling hiccups.
    constexpr REFERENCE_TIME kBufferDuration = 500'000; // 100 ns units
    hr = ctx.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK |
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                kBufferDuration, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        setError("IAudioClient::Initialize(loopback) failed: " + HrErrorString(hr));
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

    hr = ctx.client->GetService(IID_PPV_ARGS(ctx.capture.GetAddressOf()));
    if (FAILED(hr)) {
        setError("GetService(IAudioCaptureClient) failed: " + HrErrorString(hr));
        return false;
    }

    hr = ctx.client->Start();
    if (FAILED(hr)) {
        setError("IAudioClient::Start failed: " + HrErrorString(hr));
        return false;
    }

    LogF("[AudioCapture] mix format: %s %u ch @ %u Hz, device buffer %u frames",
         ctx.srcIsFloat ? "float32" : "int16", ctx.srcChannels, ctx.srcRate,
         ctx.bufferFrames);
    return true;
}

// ---------------------------------------------------------------------------
// Format adaptation: source format -> 48 kHz stereo float32
// ---------------------------------------------------------------------------

void AudioCapture::convertAppend(CaptureContext& ctx, const uint8_t* data,
                                 uint32_t frames, bool silent,
                                 std::vector<float>& scratch,
                                 std::vector<float>& stage)
{
    if (frames == 0)
        return;

    // Step 1: source format -> stereo float at the source rate. Silent
    // packets become zeros (run through the same path so the resampler
    // phase stays continuous).
    scratch.clear();
    if (silent) {
        scratch.resize(static_cast<size_t>(frames) * 2, 0.0f);
    } else {
        const uint16_t ch = ctx.srcChannels;
        for (uint32_t f = 0; f < frames; ++f) {
            float l = 0.0f, r = 0.0f;
            if (ch == 1) {
                float s;
                if (ctx.srcIsFloat)
                    s = reinterpret_cast<const float*>(data)[f];
                else
                    s = reinterpret_cast<const int16_t*>(data)[f] / 32768.0f;
                l = r = s;
            } else {
                for (uint16_t c = 0; c < ch && c < 8; ++c) {
                    float s;
                    if (ctx.srcIsFloat)
                        s = reinterpret_cast<const float*>(data)[f * ch + c];
                    else
                        s = reinterpret_cast<const int16_t*>(data)[f * ch + c] / 32768.0f;
                    l += s * kDownmixL[c];
                    r += s * kDownmixR[c];
                }
                if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            }
            scratch.push_back(l);
            scratch.push_back(r);
        }
    }

    // Step 2: resample to 48 kHz onto the staging buffer.
    const size_t inFrames = scratch.size() / 2;
    if (ctx.srcRate == kSampleRate) {
        stage.insert(stage.end(), scratch.begin(), scratch.end());
        return;
    }

    // Linear interpolation over the virtual stream v[0]=prev, v[1..n]=in.
    // Phase carries the fractional read position across packet boundaries.
    if (!ctx.resPrimed) {
        ctx.resPrevL  = scratch[0];
        ctx.resPrevR  = scratch[1];
        ctx.resPrimed = true;
        ctx.resPhase  = 0.0;
    }
    const double step = static_cast<double>(ctx.srcRate) / kSampleRate;
    double pos = ctx.resPhase;
    auto sampleL = [&](size_t k) { return k == 0 ? ctx.resPrevL : scratch[(k - 1) * 2]; };
    auto sampleR = [&](size_t k) { return k == 0 ? ctx.resPrevR : scratch[(k - 1) * 2 + 1]; };
    while (pos < static_cast<double>(inFrames)) {
        const size_t i   = static_cast<size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i));
        stage.push_back(sampleL(i) + (sampleL(i + 1) - sampleL(i)) * frac);
        stage.push_back(sampleR(i) + (sampleR(i + 1) - sampleR(i)) * frac);
        pos += step;
    }
    ctx.resPhase = pos - static_cast<double>(inFrames);
    ctx.resPrevL = scratch[(inFrames - 1) * 2];
    ctx.resPrevR = scratch[(inFrames - 1) * 2 + 1];
}

// ---------------------------------------------------------------------------
// Capture thread
// ---------------------------------------------------------------------------

void AudioCapture::captureThreadProc(std::promise<bool> ready)
{
    SetThreadDescription(GetCurrentThread(), L"Audio-Capture");

    ComApartment com;
    if (FAILED(com.hr)) {
        setError("CoInitializeEx failed: " + HrErrorString(com.hr));
        ready.set_value(false);
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        setError("CoCreateInstance(MMDeviceEnumerator) failed: " + HrErrorString(hr));
        ready.set_value(false);
        return;
    }

    // Watch for default-endpoint switches (headset plugged in, Bluetooth
    // connected, ...). Best effort: capture works without it, it just
    // stays glued to the old device.
    m_deviceChanged.store(false, std::memory_order_relaxed);
    DeviceChangeNotifier notifier(m_deviceChanged, m_event);
    const bool notifierRegistered =
        SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(&notifier));
    if (!notifierRegistered)
        LogF("[AudioCapture] endpoint notifications unavailable -- device "
             "changes will not be followed");

    CaptureContext ctx;
    if (!initCapture(ctx, enumerator.Get())) {
        if (notifierRegistered)
            enumerator->UnregisterEndpointNotificationCallback(&notifier);
        ready.set_value(false);
        return; // ComPtrs in ctx release before CoUninitialize
    }

    int opusErr = 0;
    OpusEncoder* encoder = opus_encoder_create(kSampleRate, kChannels,
                                               OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                                               &opusErr);
    if (!encoder || opusErr != OPUS_OK) {
        char msg[96];
        sprintf_s(msg, "opus_encoder_create failed: %d", opusErr);
        setError(msg);
        ctx.client->Stop();
        if (notifierRegistered)
            enumerator->UnregisterEndpointNotificationCallback(&notifier);
        ready.set_value(false);
        return;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kOpusBitrate));

    MmcssBoost boost; // best effort; capture still works without it

    ready.set_value(true);

    std::vector<float>   scratch;
    std::vector<float>   stage;
    std::vector<uint8_t> opusBuf(kMaxOpusPacket);
    scratch.reserve(static_cast<size_t>(ctx.bufferFrames) * 2 + 16);
    stage.reserve(kFrameFloats * 8);

    const uint64_t anchorUs   = SteadyMicros();
    uint64_t samplesEncoded   = 0; // 48 kHz frames handed to the encoder
    uint32_t encodeFailures   = 0;
    bool     needReinit       = false;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        if (needReinit) {
            // Default endpoint changed or the device died: drop the WASAPI
            // objects (format may be completely different) and rebuild on
            // the current default. Encoder + timestamp counter survive.
            if (ctx.client)
                ctx.client->Stop();
            ctx = CaptureContext{};
            stage.clear();
            m_deviceChanged.store(false, std::memory_order_relaxed);
            LogF("[AudioCapture] audio device changed; reinitializing loopback");
            while (!m_stopRequested.load(std::memory_order_acquire) &&
                   !initCapture(ctx, enumerator.Get())) {
                ctx = CaptureContext{};
                Sleep(500); // no usable device right now; keep retrying
            }
            if (m_stopRequested.load(std::memory_order_acquire))
                break;
            needReinit = false;
        }

        const DWORD wait = WaitForSingleObject(m_event, 200);
        if (m_stopRequested.load(std::memory_order_acquire))
            break;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            LogF("[AudioCapture] event wait failed (%lu) -- capture thread exiting",
                 wait);
            break;
        }
        // WAIT_TIMEOUT is normal when nothing is rendering: loopback only
        // produces data while some session plays audio.

        if (m_deviceChanged.load(std::memory_order_acquire)) {
            needReinit = true;
            continue;
        }

        UINT32 packetFrames = 0;
        hr = ctx.capture->GetNextPacketSize(&packetFrames);
        while (SUCCEEDED(hr) && packetFrames > 0) {
            BYTE*  data  = nullptr;
            UINT32 got   = 0;
            DWORD  flags = 0;
            hr = ctx.capture->GetBuffer(&data, &got, &flags, nullptr, nullptr);
            if (FAILED(hr))
                break;
            convertAppend(ctx, data, got,
                          (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, scratch, stage);
            ctx.capture->ReleaseBuffer(got);
            hr = ctx.capture->GetNextPacketSize(&packetFrames);
        }
        if (FAILED(hr)) {
            // AUDCLNT_E_DEVICE_INVALIDATED (unplugged) lands here too --
            // treat every capture failure as "rebuild on current default".
            setError("capture loop failed: " + HrErrorString(hr));
            LogF("[AudioCapture] %s -- reinitializing", lastError().c_str());
            needReinit = true;
            continue;
        }

        // Encode every complete 20 ms frame in the staging buffer.
        size_t consumed = 0;
        while (stage.size() - consumed >= kFrameFloats) {
            const int bytes = opus_encode_float(encoder, stage.data() + consumed,
                                                kFrameSamples, opusBuf.data(),
                                                static_cast<int>(opusBuf.size()));
            if (bytes > 0) {
                const uint64_t tsUs =
                    anchorUs + samplesEncoded * 1'000'000ull / kSampleRate;
                m_sender.sendAudio(opusBuf.data(), static_cast<size_t>(bytes), tsUs);
            } else {
                ++encodeFailures;
                if (encodeFailures == 1 || encodeFailures % 1000 == 0)
                    LogF("[AudioCapture] opus_encode_float failed: %d (%u so far)",
                         bytes, encodeFailures);
            }
            samplesEncoded += kFrameSamples;
            consumed += kFrameFloats;
        }
        if (consumed > 0)
            stage.erase(stage.begin(),
                        stage.begin() + static_cast<ptrdiff_t>(consumed));
    }

    // Flush: zero-pad the partial frame so the tail of the stream is heard.
    if (!stage.empty()) {
        stage.resize(kFrameFloats, 0.0f);
        const int bytes = opus_encode_float(encoder, stage.data(), kFrameSamples,
                                            opusBuf.data(),
                                            static_cast<int>(opusBuf.size()));
        if (bytes > 0) {
            const uint64_t tsUs = anchorUs + samplesEncoded * 1'000'000ull / kSampleRate;
            m_sender.sendAudio(opusBuf.data(), static_cast<size_t>(bytes), tsUs);
        }
    }

    if (ctx.client)
        ctx.client->Stop();
    opus_encoder_destroy(encoder);
    if (notifierRegistered)
        enumerator->UnregisterEndpointNotificationCallback(&notifier);
}
