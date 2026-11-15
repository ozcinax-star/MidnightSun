// ScreenCapture.h
//
// DXGI Desktop Duplication screen capture for game streaming.
//
// Captures the primary monitor on a dedicated thread and publishes frames
// through a triple-buffered ring of GPU textures (no CPU readback, no
// staging copies). The consumer polls getLatestFrame() at its own rate;
// the capture thread is never blocked by the consumer and vice versa.
//
// Threading model:
//   - startCapture()/stopCapture() must be called from the same thread
//     (typically the app's main/control thread).
//   - getLatestFrame() may be called from any single consumer thread.
//   - The capture thread owns the D3D11 immediate context for its copies.
//     ID3D11Multithread protection is enabled on the device so a consumer
//     that submits GPU work on the same device from another thread
//     (e.g. handing the texture to an NVENC/AMF/QSV encoder) is safe.
//
// Frame lifetime contract:
//   The texture returned by getLatestFrame() remains valid until the NEXT
//   call to getLatestFrame() or until stopCapture(), whichever comes first.
//   Do not hold the pointer across those calls; if you need the pixels
//   longer, CopyResource them into your own texture.
//
// Requires: MSVC 2022, C++20, Windows SDK 10.0, D3D11. Link d3d11.lib and
// dxgi.lib (done via #pragma comment in the .cpp).

#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// One captured frame. `texture` lives in GPU memory on the device returned
// by ScreenCapture::device(); format is the desktop format (in practice
// DXGI_FORMAT_B8G8R8A8_UNORM, i.e. raw BGRA).
struct CaptureFrame {
    ID3D11Texture2D* texture;      // borrowed pointer -- see lifetime contract above
    UINT64           timestamp_us; // QPC-derived microseconds of the desktop present
    int              width;
    int              height;
};

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    // Non-copyable, non-movable: owns a thread and D3D objects whose
    // addresses are captured by that thread.
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Creates the D3D11 device on the adapter that owns the primary monitor
    // and starts the capture thread. Returns false only on hard failures
    // (no D3D11-capable adapter, no primary output). Transient conditions
    // (monitor asleep, secure desktop, mode change in progress) do NOT fail
    // startCapture(); the capture thread retries them with backoff.
    bool startCapture();

    // Stops and joins the capture thread and releases all GPU resources.
    // Any CaptureFrame previously returned becomes invalid. Safe to call
    // when not capturing.
    void stopCapture();

    // Fetches the most recent completed frame. Returns false if no frame
    // has been captured yet (or capture is restarting after a mode change).
    // If no NEW frame arrived since the last call, returns true with the
    // same frame again -- streaming encoders typically want to re-send the
    // last frame rather than stall.
    bool getLatestFrame(CaptureFrame& outFrame);

    // Blocks the consumer until a NEW frame is published or `timeoutMs`
    // elapses. Returns true if a new frame is waiting (getLatestFrame will
    // return it). Lets the encode loop run frame-driven instead of polling
    // on a fixed tick -- roughly half a frame interval less latency.
    bool waitForNewFrame(uint32_t timeoutMs);

    // The device the frame textures live on. Valid between startCapture()
    // and stopCapture(). The consumer needs this to do anything useful
    // with the textures (create SRVs, feed a hardware encoder, etc.).
    ID3D11Device* device() const { return m_device.Get(); }

    bool isCapturing() const { return m_running.load(std::memory_order_acquire); }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // Triple buffering: the capture thread copies into the "write" slot,
    // then swaps write<->latest under the lock. The consumer swaps
    // read<->latest when a new frame is available. Each slot is therefore
    // only ever touched by one side at a time, with no allocation and no
    // frame-sized copies on either swap.
    static constexpr int kNumBuffers = 3;

    struct BufferSlot {
        ComPtr<ID3D11Texture2D> texture;
        UINT64 timestampUs = 0;
    };

    // --- capture thread ---
    void captureThreadProc();
    bool initializeDevice();          // once per device lifetime
    bool initializeDuplication();     // re-run after ACCESS_LOST / mode change
    bool createFrameBuffers(UINT width, UINT height, DXGI_FORMAT format);
    void releaseDuplication();
    void releaseDevice();
    void publishFrame(UINT64 timestampUs);
    UINT64 qpcToMicroseconds(LONGLONG qpc) const;

    // --- cursor compositing (capture thread only) ---
    // The duplication API never composites the pointer into the desktop
    // image; updateCursor() caches shape/position from the frame info and
    // compositeCursor() CPU-blends the cached shape into the ring slot just
    // copied (small staging round-trip over the cursor rectangle only).
    void updateCursor(const DXGI_OUTDUPL_FRAME_INFO& frameInfo);
    void compositeCursor(ID3D11Texture2D* target);

    // --- device / duplication (capture thread owns these after start) ---
    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGIOutput1>           m_output;       // primary monitor
    ComPtr<IDXGIOutputDuplication> m_duplication;
    LONGLONG                       m_qpcFrequency = 0;

    // --- shared state (guarded by m_frameMutex) ---
    std::mutex m_frameMutex;
    std::condition_variable m_frameCv; // signaled by publishFrame()
    BufferSlot m_slots[kNumBuffers];
    int        m_writeIndex  = 0;
    int        m_latestIndex = 1;
    int        m_readIndex   = 2;
    bool       m_hasFrame    = false; // at least one frame in the read/latest chain
    bool       m_hasNewFrame = false; // latest slot is newer than read slot
    int        m_frameWidth  = 0;
    int        m_frameHeight = 0;

    // Keeps the texture last handed to the consumer alive even if the ring
    // is rebuilt under it (resolution change). Written only inside
    // getLatestFrame() under m_frameMutex.
    ComPtr<ID3D11Texture2D> m_consumerHeldTexture;

    // --- cursor state (capture thread only) ---
    std::vector<uint8_t>            m_cursorShape;      // raw DXGI shape buffer
    DXGI_OUTDUPL_POINTER_SHAPE_INFO m_cursorShapeInfo{};
    ComPtr<ID3D11Texture2D>         m_cursorStaging;    // CPU r/w patch texture
    UINT m_cursorStagingW = 0;
    UINT m_cursorStagingH = 0;
    int  m_cursorX        = 0;
    int  m_cursorY        = 0;
    bool m_cursorVisible  = false;

    // --- thread control ---
    std::thread       m_captureThread;
    std::atomic<bool> m_running{false};
};
