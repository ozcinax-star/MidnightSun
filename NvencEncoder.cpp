// NvencEncoder.cpp -- see NvencEncoder.h for the API contract.

#include "NvencEncoder.h"

#include <chrono>
#include <cstdio>

namespace {

// nvEncodeAPI64.dll exports these two entry points; everything else is
// reached through the function list filled in by NvEncodeAPICreateInstance.
typedef NVENCSTATUS(NVENCAPI* PFN_NvEncodeAPIGetMaxSupportedVersion)(uint32_t*);
typedef NVENCSTATUS(NVENCAPI* PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);

// P4 + ultra-low-latency tuning on every SDK: the legacy LOW_LATENCY_HQ
// GUID is deprecated since SDK 10 and modern drivers only emulate it --
// the emulation path measured an order of magnitude slower than the
// native P-preset path, so the legacy GUID is intentionally not used
// even where the headers still provide it.
const GUID kPreferredPreset = NV_ENC_PRESET_P4_GUID;
const GUID kFallbackPreset  = NV_ENC_PRESET_P1_GUID; // fastest, if P4 is rejected

bool dxgiToNvFormat(DXGI_FORMAT dxgi, NV_ENC_BUFFER_FORMAT& outNv) {
    switch (dxgi) {
    // NVENC's "ARGB" is the packed little-endian dword, i.e. B,G,R,A in
    // memory -- exactly DXGI B8G8R8A8 (what desktop duplication delivers).
    case DXGI_FORMAT_B8G8R8A8_UNORM: outNv = NV_ENC_BUFFER_FORMAT_ARGB; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: outNv = NV_ENC_BUFFER_FORMAT_ABGR; return true;
    default: return false;
    }
}

} // namespace

NvencEncoder::~NvencEncoder() {
    shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool NvencEncoder::initialize(ID3D11Device* device, const Config& config) {
    shutdown();

    if (!device)
        return setError("initialize", "device is null");
    if (config.bitrateBps == 0 || config.frameRateNum == 0 || config.frameRateDen == 0)
        return setError("initialize", "bitrate and frame rate must be non-zero");

    m_device = device;
    m_config = config;

    if (!loadApi() || !openSession()) {
        shutdown(); // unwind partial state, but keep m_lastError
        return false;
    }
    return true;
}

void NvencEncoder::shutdown() {
    destroyEncoderSession();
    m_nvenc = {};
    if (m_nvencLib) {
        FreeLibrary(m_nvencLib);
        m_nvencLib = nullptr;
    }
    m_device.Reset();
    m_frameIndex = 0;
    m_forceIdr.store(false, std::memory_order_release);
    m_pendingBitrate.store(0, std::memory_order_release);
    m_currentBitrate.store(0, std::memory_order_release);
}

bool NvencEncoder::loadApi() {
    m_nvencLib = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!m_nvencLib)
        return setError("LoadLibrary(nvEncodeAPI64.dll)",
                        "not found -- NVIDIA driver not installed?");

    auto getMaxVersion = reinterpret_cast<PFN_NvEncodeAPIGetMaxSupportedVersion>(
        GetProcAddress(m_nvencLib, "NvEncodeAPIGetMaxSupportedVersion"));
    auto createInstance = reinterpret_cast<PFN_NvEncodeAPICreateInstance>(
        GetProcAddress(m_nvencLib, "NvEncodeAPICreateInstance"));
    if (!getMaxVersion || !createInstance)
        return setError("GetProcAddress", "nvEncodeAPI64.dll is missing entry points");

    // The driver must support at least the API version these headers
    // describe, or every versioned struct we pass would be rejected.
    uint32_t driverVersion = 0;
    NVENCSTATUS status = getMaxVersion(&driverVersion);
    if (status != NV_ENC_SUCCESS)
        return setError("NvEncodeAPIGetMaxSupportedVersion", status);
    const uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (driverVersion < headerVersion)
        return setError("NvEncodeAPIGetMaxSupportedVersion",
                        "installed NVIDIA driver is older than the NVENC SDK headers "
                        "-- update the driver");

    m_nvenc = {};
    m_nvenc.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    status = createInstance(&m_nvenc);
    if (status != NV_ENC_SUCCESS)
        return setError("NvEncodeAPICreateInstance", status);
    return true;
}

bool NvencEncoder::openSession() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
    sessionParams.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device     = m_device.Get();
    sessionParams.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS status = m_nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &m_encoder);
    if (status != NV_ENC_SUCCESS) {
        m_encoder = nullptr; // the API may write garbage on failure
        return setError("NvEncOpenEncodeSessionEx", status);
    }
    return true;
}

bool NvencEncoder::initializeEncoder(uint32_t width, uint32_t height) {
    // Start from the driver's own preset + tuning configuration and only
    // override what this module's contract pins down. This keeps all the
    // fields NVIDIA tunes per-architecture (multipass, AQ, ...) at their
    // recommended ultra-low-latency values.
    NV_ENC_PRESET_CONFIG presetConfig = {};
    presetConfig.version           = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    GUID presetGuid = kPreferredPreset;
    NVENCSTATUS status = m_nvenc.nvEncGetEncodePresetConfigEx(
        m_encoder, NV_ENC_CODEC_H264_GUID, presetGuid,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig);
    if (status != NV_ENC_SUCCESS) {
        presetGuid = kFallbackPreset;
        status = m_nvenc.nvEncGetEncodePresetConfigEx(
            m_encoder, NV_ENC_CODEC_H264_GUID, presetGuid,
            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig);
    }
    if (status != NV_ENC_SUCCESS)
        return setError("NvEncGetEncodePresetConfigEx", status);

    m_encodeConfig = presetConfig.presetCfg;
    NV_ENC_CONFIG& encodeConfig = m_encodeConfig;

    // Infinite GOP, no B-frames: every frame references only the previous
    // one, and IDRs happen solely on request (plus the very first frame).
    encodeConfig.gopLength      = NVENC_INFINITE_GOPLENGTH;
    encodeConfig.frameIntervalP = 1;

    // CBR with a one-frame VBV: the encoder can never owe the channel more
    // than one frame's worth of bits, which is what bounds worst-case
    // send latency after a scene cut or a forced IDR.
    NV_ENC_RC_PARAMS& rc = encodeConfig.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.averageBitRate  = m_config.bitrateBps;
    rc.maxBitRate      = m_config.bitrateBps;
    rc.vbvBufferSize   = static_cast<uint32_t>(
        static_cast<uint64_t>(m_config.bitrateBps) * m_config.frameRateDen / m_config.frameRateNum);
    rc.vbvInitialDelay = rc.vbvBufferSize;

    // Speed over the last few percent of quality: single-pass encoding and
    // no lookahead. Lookahead in particular would buffer frames inside the
    // encoder, which the synchronous one-in-one-out encode() cannot afford.
    rc.multiPass       = NV_ENC_MULTI_PASS_DISABLED;
    rc.enableLookahead = 0;
    rc.lookaheadDepth  = 0;

    // Spatial AQ: shifts bits toward flat/low-detail regions, which is
    // exactly where tight CBR shows blocking/"grain" first. Negligible
    // encode-time cost on NVENC. aqStrength 0 = driver default.
    rc.enableAQ   = 1;
    rc.aqStrength = 0;

    NV_ENC_CONFIG_H264& h264 = encodeConfig.encodeCodecConfig.h264Config;
    h264.idrPeriod    = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1; // every IDR is independently decodable mid-stream

    // CABAC instead of CAVLC: ~10% fewer bits for the same quality; all
    // NVENC-capable GPUs decode-side peers handle it (H.264 High profile).
    h264.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;

    // Periodic intra refresh: a rolling wave of intra-coded macroblock
    // rows continuously repairs undetected corruption WITHOUT the
    // one-frame bit spike (and visible quality pulse) of a periodic IDR
    // under the single-frame VBV. The wave spans intraRefreshCnt frames
    // and restarts every intraRefreshPeriod frames.
    const uint32_t fps = m_config.frameRateNum / (m_config.frameRateDen ? m_config.frameRateDen : 1);
    h264.enableIntraRefresh = 1;
    h264.intraRefreshPeriod = fps > 0 ? fps * 4 : 240; // one wave every ~4 s
    h264.intraRefreshCnt    = fps > 12 ? fps / 6 : 5;  // wave lasts ~1/6 s

    NV_ENC_INITIALIZE_PARAMS& initParams = m_initParams;
    initParams = {};
    initParams.version           = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID        = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID        = presetGuid;
    initParams.tuningInfo        = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initParams.encodeWidth       = width;
    initParams.encodeHeight      = height;
    initParams.darWidth          = width;
    initParams.darHeight         = height;
    initParams.frameRateNum      = m_config.frameRateNum;
    initParams.frameRateDen      = m_config.frameRateDen;
    initParams.enablePTD         = 1; // encoder decides picture types (all P here)
    initParams.enableEncodeAsync = 0; // synchronous: one frame in, one bitstream out
    initParams.encodeConfig      = &encodeConfig;

    status = m_nvenc.nvEncInitializeEncoder(m_encoder, &initParams);
    if (status != NV_ENC_SUCCESS) {
        // Some driver/GPU combinations reject intra refresh; retry once
        // without it rather than failing the whole session.
        h264.enableIntraRefresh = 0;
        status = m_nvenc.nvEncInitializeEncoder(m_encoder, &initParams);
        if (status == NV_ENC_SUCCESS)
            fprintf(stderr, "[NvencEncoder] intra refresh rejected by driver; "
                            "continuing without it\n");
    }
    if (status != NV_ENC_SUCCESS)
        return setError("NvEncInitializeEncoder", status);
    m_encoderInitialized = true;
    m_currentBitrate.store(m_config.bitrateBps, std::memory_order_relaxed);

    fprintf(stderr, "[NvencEncoder] session: %ux%u, preset %s + ULL tuning, "
                    "single-pass CBR %u kbps, 1-frame VBV, AQ+CABAC%s\n",
            width, height,
            presetGuid == kPreferredPreset ? "P4" : "P1(fallback)",
            m_config.bitrateBps / 1000,
            h264.enableIntraRefresh ? ", intra-refresh" : "");

    NV_ENC_CREATE_BITSTREAM_BUFFER createBitstream = {};
    createBitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    status = m_nvenc.nvEncCreateBitstreamBuffer(m_encoder, &createBitstream);
    if (status != NV_ENC_SUCCESS)
        return setError("NvEncCreateBitstreamBuffer", status);
    m_bitstream = createBitstream.bitstreamBuffer;

    m_width  = width;
    m_height = height;
    return true;
}

void NvencEncoder::setBitrate(uint32_t bitrateBps) {
    if (bitrateBps < 1'000'000)
        bitrateBps = 1'000'000;
    if (bitrateBps > 500'000'000)
        bitrateBps = 500'000'000;
    m_pendingBitrate.store(bitrateBps, std::memory_order_release);
}

void NvencEncoder::applyPendingBitrate() {
    const uint32_t target = m_pendingBitrate.exchange(0, std::memory_order_acq_rel);
    if (target == 0 || !m_encoderInitialized)
        return;
    if (target == m_currentBitrate.load(std::memory_order_relaxed))
        return;

    NV_ENC_RC_PARAMS& rc = m_encodeConfig.rcParams;
    rc.averageBitRate  = target;
    rc.maxBitRate      = target;
    rc.vbvBufferSize   = static_cast<uint32_t>(
        static_cast<uint64_t>(target) * m_config.frameRateDen / m_config.frameRateNum);
    rc.vbvInitialDelay = rc.vbvBufferSize;

    NV_ENC_RECONFIGURE_PARAMS reconfig = {};
    reconfig.version            = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfig.reInitEncodeParams = m_initParams; // struct copy; keeps &m_encodeConfig
    reconfig.resetEncoder       = 0;            // keep reference frames
    reconfig.forceIDR           = 0;            // no keyframe spike on a rate change

    const NVENCSTATUS status = m_nvenc.nvEncReconfigureEncoder(m_encoder, &reconfig);
    if (status != NV_ENC_SUCCESS) {
        setError("NvEncReconfigureEncoder", status);
        fprintf(stderr, "[NvencEncoder] bitrate reconfigure to %u kbps failed: %s\n",
                target / 1000, m_lastError.c_str());
        return;
    }
    m_currentBitrate.store(target, std::memory_order_relaxed);
    m_config.bitrateBps = target; // survive a resolution-change session rebuild
    fprintf(stderr, "[NvencEncoder] bitrate -> %u kbps (live reconfigure)\n",
            target / 1000);
}

void NvencEncoder::destroyEncoderSession() {
    if (!m_encoder)
        return;

    // Drain the pipeline before tearing anything down. With sync mode and
    // no B-frames nothing is ever queued, but EOS is the documented
    // teardown handshake and is harmless here. Best effort: we are
    // destroying the session regardless of what these calls return.
    if (m_encoderInitialized) {
        NV_ENC_PIC_PARAMS eos = {};
        eos.version        = NV_ENC_PIC_PARAMS_VER;
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        m_nvenc.nvEncEncodePicture(m_encoder, &eos);
    }

    for (Registration& reg : m_registrations)
        m_nvenc.nvEncUnregisterResource(m_encoder, reg.handle);
    m_registrations.clear();

    if (m_bitstream) {
        m_nvenc.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstream);
        m_bitstream = nullptr;
    }

    m_nvenc.nvEncDestroyEncoder(m_encoder);
    m_encoder = nullptr;
    m_encoderInitialized = false;
    m_width  = 0;
    m_height = 0;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

std::vector<uint8_t> NvencEncoder::encode(ID3D11Texture2D* frame) {
    std::vector<uint8_t> nalUnits;

    if (!frame) {
        setError("encode", "frame is null");
        return nalUnits;
    }
    if (!m_encoder) {
        setError("encode", "encoder not initialized -- call initialize() first");
        return nalUnits;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    frame->GetDesc(&desc);

    // Resolution change: rebuild the whole session. NvEncDestroyEncoder
    // tears the session down with the encoder state, so reopen both. The
    // next bitstream out is a fresh IDR + SPS/PPS, so a downstream decoder
    // can follow the switch without out-of-band signalling.
    if (m_encoderInitialized && (desc.Width != m_width || desc.Height != m_height)) {
        destroyEncoderSession();
        if (!openSession())
            return nalUnits;
    }
    if (!m_encoderInitialized && !initializeEncoder(desc.Width, desc.Height))
        return nalUnits;

    applyPendingBitrate(); // adaptive-bitrate hook, cheap no-op when idle

    NV_ENC_REGISTERED_PTR registered = registerOrLookup(frame, desc);
    if (!registered)
        return nalUnits;

    using EncClock = std::chrono::steady_clock;
    const auto msBetween = [](EncClock::time_point a, EncClock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto t0 = EncClock::now();

    // Zero-copy hand-off: map the registered D3D11 texture as the encode
    // input. NVENC reads the texture directly from VRAM.
    NV_ENC_MAP_INPUT_RESOURCE mapParams = {};
    mapParams.version            = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapParams.registeredResource = registered;
    NVENCSTATUS status = m_nvenc.nvEncMapInputResource(m_encoder, &mapParams);
    if (status != NV_ENC_SUCCESS) {
        setError("NvEncMapInputResource", status);
        return nalUnits;
    }
    const auto tMapped = EncClock::now();

    const bool forceIdr = m_forceIdr.exchange(false, std::memory_order_acq_rel);

    NV_ENC_PIC_PARAMS picParams = {};
    picParams.version         = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer     = mapParams.mappedResource;
    picParams.bufferFmt       = mapParams.mappedBufferFmt;
    picParams.inputWidth      = m_width;
    picParams.inputHeight     = m_height;
    picParams.outputBitstream = m_bitstream;
    picParams.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
    picParams.inputTimeStamp  = m_frameIndex;
    if (forceIdr)
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    status = m_nvenc.nvEncEncodePicture(m_encoder, &picParams);
    const auto tSubmitted = EncClock::now();
    auto tWaited = tSubmitted;
    if (status == NV_ENC_SUCCESS) {
        // Synchronous mode: this lock blocks until the frame's bitstream
        // is complete, then hands us a pointer into NVENC's buffer.
        NV_ENC_LOCK_BITSTREAM lockParams = {};
        lockParams.version         = NV_ENC_LOCK_BITSTREAM_VER;
        lockParams.outputBitstream = m_bitstream;
        lockParams.doNotWait       = 0;
        status = m_nvenc.nvEncLockBitstream(m_encoder, &lockParams);
        tWaited = EncClock::now();
        if (status == NV_ENC_SUCCESS) {
            const auto* data = static_cast<const uint8_t*>(lockParams.bitstreamBufferPtr);
            nalUnits.assign(data, data + lockParams.bitstreamSizeInBytes);
            m_nvenc.nvEncUnlockBitstream(m_encoder, m_bitstream);
            ++m_frameIndex;
        } else {
            setError("NvEncLockBitstream", status);
        }
    } else {
        setError("NvEncEncodePicture", status);
    }

    m_nvenc.nvEncUnmapInputResource(m_encoder, mapParams.mappedResource);

    // Stage breakdown for slow frames, at most one line per second: where
    // do the milliseconds go -- mapping, submission, hardware wait, or the
    // copy-out/unmap tail?
    const auto tDone = EncClock::now();
    const double totalMs = msBetween(t0, tDone);
    static auto s_lastSlowLog = EncClock::now() - std::chrono::seconds(10);
    if (totalMs > 25.0 && tDone - s_lastSlowLog >= std::chrono::seconds(1)) {
        s_lastSlowLog = tDone;
        fprintf(stderr, "[NvencEncoder] slow encode %.1f ms: map %.1f, submit %.1f, "
                        "hw-wait %.1f, copy/unmap %.1f (%zu bytes%s)\n",
                totalMs,
                msBetween(t0, tMapped),
                msBetween(tMapped, tSubmitted),
                msBetween(tSubmitted, tWaited),
                msBetween(tWaited, tDone),
                nalUnits.size(),
                forceIdr ? ", IDR" : "");
    }

    // Don't silently drop a keyframe request because this frame failed.
    if (forceIdr && nalUnits.empty())
        m_forceIdr.store(true, std::memory_order_release);

    return nalUnits;
}

NV_ENC_REGISTERED_PTR NvencEncoder::registerOrLookup(ID3D11Texture2D* texture,
                                                     const D3D11_TEXTURE2D_DESC& desc) {
    for (const Registration& reg : m_registrations)
        if (reg.texture.Get() == texture)
            return reg.handle;

    // New texture. Validate it before handing it to the driver: NVENC
    // gives an unhelpful generic error for both of these mistakes.
    NV_ENC_BUFFER_FORMAT nvFormat;
    if (!dxgiToNvFormat(desc.Format, nvFormat)) {
        setError("registerOrLookup",
                 "unsupported texture format -- expected B8G8R8A8_UNORM or R8G8B8A8_UNORM");
        return nullptr;
    }
    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (textureDevice.Get() != m_device.Get()) {
        setError("registerOrLookup",
                 "texture was created on a different D3D11 device than the encoder");
        return nullptr;
    }

    // A steady producer (ScreenCapture's ring) has exactly 3 textures, so
    // the cache saturates immediately and registration cost disappears.
    // If the producer churns textures (ring rebuilds at the same
    // resolution), flush everything rather than growing without bound --
    // the cached ComPtrs would otherwise keep dead ring textures alive.
    if (m_registrations.size() >= kMaxRegistrations) {
        for (Registration& reg : m_registrations)
            m_nvenc.nvEncUnregisterResource(m_encoder, reg.handle);
        m_registrations.clear();
    }

    NV_ENC_REGISTER_RESOURCE registerParams = {};
    registerParams.version            = NV_ENC_REGISTER_RESOURCE_VER;
    registerParams.resourceType       = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    registerParams.resourceToRegister = texture;
    registerParams.width              = desc.Width;
    registerParams.height             = desc.Height;
    registerParams.pitch              = 0;
    registerParams.bufferFormat       = nvFormat;
    registerParams.bufferUsage        = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS status = m_nvenc.nvEncRegisterResource(m_encoder, &registerParams);
    if (status != NV_ENC_SUCCESS) {
        setError("NvEncRegisterResource", status);
        return nullptr;
    }

    m_registrations.push_back({texture, registerParams.registeredResource});
    return registerParams.registeredResource;
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

bool NvencEncoder::setError(const char* where, NVENCSTATUS status) {
    m_lastError = where;
    m_lastError += " failed: ";
    m_lastError += statusToString(status);
    // The driver often has a far more specific message (which cap was
    // violated, which field was bad) than the status code alone.
    if (m_encoder && m_nvenc.nvEncGetLastErrorString) {
        const char* detail = m_nvenc.nvEncGetLastErrorString(m_encoder);
        if (detail && *detail) {
            m_lastError += ": ";
            m_lastError += detail;
        }
    }
    return false;
}

bool NvencEncoder::setError(const char* where, const char* detail) {
    m_lastError = where;
    m_lastError += " failed: ";
    m_lastError += detail;
    return false;
}

const char* NvencEncoder::statusToString(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_SUCCESS:
        return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
        return "NV_ENC_ERR_NO_ENCODE_DEVICE (no NVENC-capable GPU found)";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
        return "NV_ENC_ERR_UNSUPPORTED_DEVICE (GPU does not support NVENC)";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:
        return "NV_ENC_ERR_INVALID_ENCODERDEVICE (invalid encoder device)";
    case NV_ENC_ERR_INVALID_DEVICE:
        return "NV_ENC_ERR_INVALID_DEVICE (invalid D3D/CUDA device)";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:
        return "NV_ENC_ERR_DEVICE_NOT_EXIST (device no longer exists -- removed or TDR?)";
    case NV_ENC_ERR_INVALID_PTR:
        return "NV_ENC_ERR_INVALID_PTR (invalid pointer argument)";
    case NV_ENC_ERR_INVALID_EVENT:
        return "NV_ENC_ERR_INVALID_EVENT (invalid completion event)";
    case NV_ENC_ERR_INVALID_PARAM:
        return "NV_ENC_ERR_INVALID_PARAM (invalid parameter)";
    case NV_ENC_ERR_INVALID_CALL:
        return "NV_ENC_ERR_INVALID_CALL (API call out of sequence)";
    case NV_ENC_ERR_OUT_OF_MEMORY:
        return "NV_ENC_ERR_OUT_OF_MEMORY (allocation failed)";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
        return "NV_ENC_ERR_ENCODER_NOT_INITIALIZED (encoder not initialized)";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
        return "NV_ENC_ERR_UNSUPPORTED_PARAM (parameter not supported on this GPU/driver)";
    case NV_ENC_ERR_LOCK_BUSY:
        return "NV_ENC_ERR_LOCK_BUSY (lock busy, try again)";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:
        return "NV_ENC_ERR_NOT_ENOUGH_BUFFER (output buffer too small)";
    case NV_ENC_ERR_INVALID_VERSION:
        return "NV_ENC_ERR_INVALID_VERSION (struct version mismatch -- SDK/driver skew)";
    case NV_ENC_ERR_MAP_FAILED:
        return "NV_ENC_ERR_MAP_FAILED (mapping registered resource failed)";
    case NV_ENC_ERR_NEED_MORE_INPUT:
        return "NV_ENC_ERR_NEED_MORE_INPUT (bitstream pending, submit more frames)";
    case NV_ENC_ERR_ENCODER_BUSY:
        return "NV_ENC_ERR_ENCODER_BUSY (encoder busy, retry)";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD:
        return "NV_ENC_ERR_EVENT_NOT_REGISTERD (completion event not registered)";
    case NV_ENC_ERR_GENERIC:
        return "NV_ENC_ERR_GENERIC (unknown driver error)";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
        return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY (incompatible client key)";
    case NV_ENC_ERR_UNIMPLEMENTED:
        return "NV_ENC_ERR_UNIMPLEMENTED (feature not implemented)";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:
        return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED (resource registration failed)";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:
        return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED (resource was not registered)";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED:
        return "NV_ENC_ERR_RESOURCE_NOT_MAPPED (resource not mapped)";
    default:
        return "unknown NVENCSTATUS";
    }
}
