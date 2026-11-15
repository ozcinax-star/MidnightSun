// LatencyTracker.h
//
// Lock-free per-frame latency profiler for the game streaming pipeline.
// Both executables link it: the server stamps CAPTURE_START..SEND_START,
// the client stamps RECEIVE_END..RENDER_END. Stages are keyed by frameId
// -- the frame's capture timestamp in microseconds, which already rides
// the wire in EncodedFrame::timestampUs, so both ends tag the same frame
// without any protocol change.
//
// Clock domains -- read this before trusting a number:
//   All stamps are QueryPerformanceCounter ticks of the LOCAL machine.
//   QPC is consistent across threads and processes on one machine, but
//   NOT across machines. Consequently each process can only compute the
//   stage pairs whose two endpoints it stamped itself: the server gets
//   captureMs/encodeMs, the client gets decodeMs/renderMs. networkMs and
//   totalMs span both ends and come out as "unavailable" (-1) in a real
//   two-machine deployment -- there is no synchronized clock to measure
//   them against. They DO resolve when server and client run on the same
//   PC (loopback testing): merge the two CSV logs by frameId offline --
//   same QPC time base -- or read them live if both halves ever share a
//   process. The report never lies by mixing clock domains.
//   (CSV merge note: the wire carries the low 32 bits of the timestamp
//   and the receiver unwraps from its first packet, so the two logs'
//   frameIds match in their low 32 bits.)
//
// mark() hot path: one QueryPerformanceCounter call plus two atomic
// stores on the slot -- no locks, no allocation, ~20-30 ns. Traces live
// in a fixed 128-slot ring indexed by frameId % 128; when a new frame
// hashes onto an occupied slot, the marker that claims it finalizes the
// old trace first (computes durations, folds them into the rolling stats,
// queues the CSV row). A trace whose RENDER_END lands is finalized
// immediately -- nothing later can arrive -- so the client overlay is
// fresh to the last frame. Finalization happens once per frame and takes
// one brief uncontended unique_lock on the stats mutex; the per-stage
// stamping itself never locks.
//
// Claim rule: frameIds are capture timestamps and therefore monotonic, so
// a slot is only ever claimed by a NEWER frame; marks belonging to a
// frame older than the slot's occupant are stale and dropped. The first
// stage a process stamps for a given frame causally precedes all of its
// later stages (the pipeline hands the frame from stage to stage), so
// the claim/reset is never racing the same frame's other markers.
//
// report(): rolling averages over the last 60 finalized traces plus the
// 99th-percentile total over the last 300, under a shared_mutex (readers
// concurrent, writers are the once-per-frame finalize). A metric with no
// valid samples in the window reports -1.
//
// CSV logging (opt-in via enableCsvLog): every finalized trace becomes
// one row -- frameId, capture_ms, encode_ms, network_ms, decode_ms,
// render_ms, total_ms; unavailable cells are left empty. Rows travel
// through a fixed 256-slot SPSC ring (producer side is the finalize
// path, already serialized by the stats mutex; drop-newest when full,
// counted) to a dedicated lowest-priority "Latency-CsvLog" thread that
// owns the FILE* and flushes every 100 rows. The pipeline threads never
// touch the file. The destructor stops the thread, drains the queue,
// flushes, and closes.
//
// Requires: MSVC 2022, C++20. No third-party libraries. windows.h stays
// out of this header (the .cpp owns QPC and the thread plumbing).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "NetworkProtocol.h" // LatencyReport (shared with StreamStats)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>
#include <string>
#include <thread>

// One pipeline checkpoint per frame. The comments name the exact event
// each stamp represents; the integration points mirror them 1:1.
enum class Stage : uint8_t {
    CAPTURE_START, // DXGI getLatestFrame() returns           (server)
    ENCODE_START,  // NvencEncoder::encode() called           (server)
    ENCODE_END,    // NvencEncoder::encode() returns          (server)
    SEND_START,    // StreamSender::sendFrame() called        (server)
    RECEIVE_END,   // StreamReceiver::onVideoFrame fires      (client)
    DECODE_START,  // VideoDecoder::decode() called           (client)
    DECODE_END,    // VideoDecoder::onDecodedFrame fires      (client)
    RENDER_START,  // RenderWindow::presentFrame() called     (client)
    RENDER_END,    // IDXGISwapChain::Present() returns       (client)
    COUNT
};

// Snapshot of one frame's checkpoints, taken when the trace is finalized.
// stageTs entries are QPC ticks; 0 = the stage was never stamped (e.g.
// the other machine's stages, or a frame dropped mid-pipeline).
struct FrameTrace {
    uint64_t frameId = 0;
    uint64_t stageTs[static_cast<int>(Stage::COUNT)] = {};
    bool     complete = false; // all stages stamped (single-process runs only)
};

class LatencyTracker {
public:
    // Process-wide singleton. Construction is thread-safe (C++ magic
    // static); destruction at exit flushes and closes the CSV log.
    static LatencyTracker& get();

    LatencyTracker(const LatencyTracker&) = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;

    // Stamps `stage` of `frameId` with the current QPC time. Thread-safe,
    // lock-free, wait-free in the common case; safe to call from the
    // capture, encode, network, decode, and render threads concurrently.
    // frameId 0 is ignored (RenderWindow's repaint-last-frame path).
    void mark(uint64_t frameId, Stage stage);

    // Rolling averages over the last 60 finalized traces, p99 of total
    // over the last 300. Fields with no measurable samples are -1 (see
    // the clock-domain note above). Thread-safe (shared lock).
    LatencyReport report() const;

    // Clears the rolling statistics window (in-flight traces and the CSV
    // file are unaffected).
    void reset();

    // Starts appending one CSV row per finalized trace to `path`
    // (truncates an existing file, writes a header row). Call at most
    // once, before or during streaming; failures are logged and logging
    // stays disabled. Never blocks the pipeline threads.
    void enableCsvLog(const std::wstring& path);

private:
    LatencyTracker();
    ~LatencyTracker();

    static constexpr size_t   kRingSize         = 128; // in-flight traces
    static constexpr size_t   kReportWindow     = 60;  // frames averaged
    static constexpr size_t   kPercentileWindow = 300; // frames for p99
    static constexpr size_t   kCsvQueueSize     = 256;
    static constexpr size_t   kCsvFlushInterval = 100; // rows between fflush
    static constexpr uint64_t kEmptyId          = 0;

    // (A constexpr member function cannot be used in a constant expression
    // while its class is still incomplete, hence the direct cast here.)
    static constexpr size_t kStageCount = static_cast<size_t>(Stage::COUNT);
    static constexpr size_t stageIndex(Stage s) { return static_cast<size_t>(s); }

    // One in-flight trace. frameId is the claim word; stageTs are written
    // by whichever thread runs that stage. `finalized` arbitrates between
    // finalize-on-RENDER_END and finalize-on-eviction so a trace is
    // published exactly once.
    struct Slot {
        std::atomic<uint64_t> frameId{kEmptyId};
        std::atomic<uint64_t> stageTs[kStageCount] = {};
        std::atomic<bool>     finalized{false};
    };

    // Durations of one finalized trace, milliseconds; -1 = the stage pair
    // was not measurable in this process.
    struct Record {
        uint64_t frameId   = 0;
        float    captureMs = -1.0f;
        float    encodeMs  = -1.0f;
        float    networkMs = -1.0f;
        float    decodeMs  = -1.0f;
        float    renderMs  = -1.0f;
        float    totalMs   = -1.0f;
    };

    // Snapshots the slot's stamps for `frameId`, computes the durations,
    // folds them into the rolling window, and queues the CSV row. Called
    // by the marker that evicts the slot or stamps RENDER_END.
    void finalizeSlot(Slot& slot, uint64_t frameId);
    Record makeRecord(const FrameTrace& trace) const;

    void csvThreadProc();
    // Drains the queue to the file; returns rows written.
    size_t csvDrain(size_t& sinceFlush);

    // --- timestamping ---
    double m_msPerTick = 0.0; // 1000 / QueryPerformanceFrequency

    // --- in-flight traces (lock-free) ---
    Slot m_slots[kRingSize];

    // --- rolling statistics (m_statsMutex) ---
    mutable std::shared_mutex m_statsMutex;
    Record m_window[kPercentileWindow]; // circular, newest at (count-1) % N
    uint64_t m_finalizedCount = 0;      // monotonic; doubles as write index

    // --- CSV pipeline ---
    // SPSC ring: producer is the finalize path (serialized by
    // m_statsMutex), consumer is the CSV thread.
    Record                m_csvQueue[kCsvQueueSize];
    std::atomic<uint64_t> m_csvHead{0}; // consumer position
    std::atomic<uint64_t> m_csvTail{0}; // producer position
    std::atomic<uint32_t> m_csvDropped{0};
    std::atomic<bool>     m_csvEnabled{false};
    std::atomic<bool>     m_csvStop{false};
    FILE*                 m_csvFile = nullptr; // owned by the CSV thread after start
    std::thread           m_csvThread;
};
