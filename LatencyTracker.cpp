// LatencyTracker.cpp -- see LatencyTracker.h for the design overview.

#include "LatencyTracker.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Logging. Same shape as the other modules: debugger + stderr, stack buffer
// only, called on errors and state changes -- never per frame.
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

uint64_t QpcNow()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

// Duration of [a, b] in milliseconds, or -1 when either endpoint was
// never stamped (or they came from different clock domains and ended up
// inverted -- never report garbage).
float PairMs(uint64_t a, uint64_t b, double msPerTick)
{
    if (a == 0 || b == 0 || b < a)
        return -1.0f;
    return static_cast<float>(static_cast<double>(b - a) * msPerTick);
}

// Appends ",<value>" with three decimals, or just "," for unavailable.
void AppendCsvCell(std::string& line, float ms)
{
    if (ms < 0.0f) {
        line += ',';
        return;
    }
    char buf[32];
    sprintf_s(buf, ",%.3f", static_cast<double>(ms));
    line += buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

LatencyTracker& LatencyTracker::get()
{
    static LatencyTracker instance;
    return instance;
}

LatencyTracker::LatencyTracker()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency); // cannot fail on XP+
    m_msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
}

LatencyTracker::~LatencyTracker()
{
    // Stop accepting rows, then let the CSV thread drain what is queued.
    m_csvEnabled.store(false, std::memory_order_release);
    if (m_csvThread.joinable()) {
        m_csvStop.store(true, std::memory_order_release);
        m_csvThread.join(); // performs the final drain + flush
    }
    if (m_csvFile) {
        fflush(m_csvFile);
        fclose(m_csvFile);
        m_csvFile = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Hot path
// ---------------------------------------------------------------------------

void LatencyTracker::mark(uint64_t frameId, Stage stage)
{
    if (frameId == kEmptyId || stage >= Stage::COUNT)
        return;

    const uint64_t now = QpcNow();
    Slot& slot = m_slots[frameId % kRingSize];

    uint64_t current = slot.frameId.load(std::memory_order_acquire);
    if (current != frameId) {
        // Claim the slot. frameIds are capture timestamps (monotonic), so
        // only a newer frame may evict; a mark older than the occupant is
        // stale -- its trace was already evicted -- and is dropped.
        for (;;) {
            if (current > frameId)
                return;
            if (slot.frameId.compare_exchange_weak(current, frameId,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                // We own the transition. Publish the evicted trace (unless
                // RENDER_END already did), then reset the slot. The new
                // frame's other stages causally follow this first mark, so
                // nobody is writing stamps we could wipe.
                if (current != kEmptyId &&
                    !slot.finalized.exchange(true, std::memory_order_acq_rel))
                    finalizeSlot(slot, current);
                for (auto& ts : slot.stageTs)
                    ts.store(0, std::memory_order_relaxed);
                slot.finalized.store(false, std::memory_order_release);
                break;
            }
            if (current == frameId)
                break; // another thread claimed it for the same frame
        }
    }

    slot.stageTs[stageIndex(stage)].store(now, std::memory_order_release);

    // RENDER_END is the pipeline's last checkpoint: nothing later can
    // arrive, so publish immediately instead of waiting for eviction
    // (which can lag ~2 s behind at 60 fps with a 128-slot ring).
    if (stage == Stage::RENDER_END &&
        !slot.finalized.exchange(true, std::memory_order_acq_rel))
        finalizeSlot(slot, frameId);
}

// ---------------------------------------------------------------------------
// Finalization + rolling statistics
// ---------------------------------------------------------------------------

LatencyTracker::Record LatencyTracker::makeRecord(const FrameTrace& trace) const
{
    const uint64_t* ts = trace.stageTs;
    Record record;
    record.frameId   = trace.frameId;
    record.captureMs = PairMs(ts[stageIndex(Stage::CAPTURE_START)],
                              ts[stageIndex(Stage::ENCODE_START)], m_msPerTick);
    record.encodeMs  = PairMs(ts[stageIndex(Stage::ENCODE_START)],
                              ts[stageIndex(Stage::ENCODE_END)], m_msPerTick);
    record.networkMs = PairMs(ts[stageIndex(Stage::SEND_START)],
                              ts[stageIndex(Stage::RECEIVE_END)], m_msPerTick);
    record.decodeMs  = PairMs(ts[stageIndex(Stage::DECODE_START)],
                              ts[stageIndex(Stage::DECODE_END)], m_msPerTick);
    record.renderMs  = PairMs(ts[stageIndex(Stage::RENDER_START)],
                              ts[stageIndex(Stage::RENDER_END)], m_msPerTick);
    record.totalMs   = PairMs(ts[stageIndex(Stage::CAPTURE_START)],
                              ts[stageIndex(Stage::RENDER_END)], m_msPerTick);
    return record;
}

void LatencyTracker::finalizeSlot(Slot& slot, uint64_t frameId)
{
    FrameTrace trace;
    trace.frameId = frameId;
    trace.complete = true;
    for (size_t i = 0; i < kStageCount; ++i) {
        trace.stageTs[i] = slot.stageTs[i].load(std::memory_order_relaxed);
        if (trace.stageTs[i] == 0)
            trace.complete = false;
    }

    const Record record = makeRecord(trace);

    std::unique_lock<std::shared_mutex> lock(m_statsMutex);
    m_window[m_finalizedCount % kPercentileWindow] = record;
    ++m_finalizedCount;

    if (m_csvEnabled.load(std::memory_order_acquire)) {
        // SPSC producer side; serialized by m_statsMutex. Drop-newest on
        // overflow -- the profiler must never stall the pipeline.
        const uint64_t tail = m_csvTail.load(std::memory_order_relaxed);
        if (tail - m_csvHead.load(std::memory_order_acquire) >= kCsvQueueSize) {
            m_csvDropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_csvQueue[tail % kCsvQueueSize] = record;
            m_csvTail.store(tail + 1, std::memory_order_release);
        }
    }
}

LatencyReport LatencyTracker::report() const
{
    LatencyReport result;

    std::shared_lock<std::shared_mutex> lock(m_statsMutex);

    const uint64_t total = m_finalizedCount;
    result.trackedFrames = static_cast<uint32_t>(
        std::min<uint64_t>(total, UINT32_MAX));
    if (total == 0)
        return result; // all fields stay -1 / 0

    // Averages over the newest kReportWindow records; each metric only
    // counts records where it was measurable.
    struct Avg {
        double sum = 0.0;
        uint32_t n = 0;
        void add(float v) { if (v >= 0.0f) { sum += v; ++n; } }
        float value() const { return n ? static_cast<float>(sum / n) : -1.0f; }
    };
    Avg capture, encode, network, decode, render, totalAvg;

    const uint64_t avgCount = std::min<uint64_t>(total, kReportWindow);
    for (uint64_t i = total - avgCount; i < total; ++i) {
        const Record& r = m_window[i % kPercentileWindow];
        capture.add(r.captureMs);
        encode.add(r.encodeMs);
        network.add(r.networkMs);
        decode.add(r.decodeMs);
        render.add(r.renderMs);
        totalAvg.add(r.totalMs);
    }
    result.captureMs = capture.value();
    result.encodeMs  = encode.value();
    result.networkMs = network.value();
    result.decodeMs  = decode.value();
    result.renderMs  = render.value();
    result.totalMs   = totalAvg.value();

    // p99 of total over the newest kPercentileWindow records.
    float totals[kPercentileWindow];
    size_t totalsCount = 0;
    const uint64_t pctCount = std::min<uint64_t>(total, kPercentileWindow);
    for (uint64_t i = total - pctCount; i < total; ++i) {
        const float t = m_window[i % kPercentileWindow].totalMs;
        if (t >= 0.0f)
            totals[totalsCount++] = t;
    }
    if (totalsCount > 0) {
        const size_t rank = (totalsCount - 1) * 99 / 100;
        std::nth_element(totals, totals + rank, totals + totalsCount);
        result.p99TotalMs = totals[rank];
    }

    return result;
}

void LatencyTracker::reset()
{
    std::unique_lock<std::shared_mutex> lock(m_statsMutex);
    for (auto& record : m_window)
        record = Record{};
    m_finalizedCount = 0;
}

// ---------------------------------------------------------------------------
// CSV logging
// ---------------------------------------------------------------------------

void LatencyTracker::enableCsvLog(const std::wstring& path)
{
    std::unique_lock<std::shared_mutex> lock(m_statsMutex);
    if (m_csvThread.joinable()) {
        LogF("[LatencyTracker] enableCsvLog called twice; keeping the first log");
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        LogF("[LatencyTracker] cannot open CSV log '%ls'; logging disabled",
             path.c_str());
        return;
    }
    fputs("frameId,capture_ms,encode_ms,network_ms,decode_ms,render_ms,total_ms\n",
          file);

    m_csvFile = file;
    m_csvStop.store(false, std::memory_order_release);
    m_csvThread = std::thread(&LatencyTracker::csvThreadProc, this);
    m_csvEnabled.store(true, std::memory_order_release);
    LogF("[LatencyTracker] CSV latency log -> '%ls'", path.c_str());
}

size_t LatencyTracker::csvDrain(size_t& sinceFlush)
{
    size_t written = 0;
    uint64_t head = m_csvHead.load(std::memory_order_relaxed);
    while (head != m_csvTail.load(std::memory_order_acquire)) {
        const Record record = m_csvQueue[head % kCsvQueueSize];
        ++head;
        m_csvHead.store(head, std::memory_order_release);

        std::string line;
        line.reserve(96);
        char idBuf[32];
        sprintf_s(idBuf, "%llu", static_cast<unsigned long long>(record.frameId));
        line += idBuf;
        AppendCsvCell(line, record.captureMs);
        AppendCsvCell(line, record.encodeMs);
        AppendCsvCell(line, record.networkMs);
        AppendCsvCell(line, record.decodeMs);
        AppendCsvCell(line, record.renderMs);
        AppendCsvCell(line, record.totalMs);
        line += '\n';
        fputs(line.c_str(), m_csvFile);

        ++written;
        if (++sinceFlush >= kCsvFlushInterval) {
            fflush(m_csvFile);
            sinceFlush = 0;
        }
    }
    return written;
}

void LatencyTracker::csvThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"Latency-CsvLog");
    // The log must never compete with capture/encode/render for a core.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    size_t sinceFlush = 0;
    uint32_t reportedDrops = 0;
    for (;;) {
        const size_t written = csvDrain(sinceFlush);

        const uint32_t drops = m_csvDropped.load(std::memory_order_relaxed);
        if (drops != reportedDrops) {
            LogF("[LatencyTracker] CSV queue overflow: %u rows dropped so far",
                 drops);
            reportedDrops = drops;
        }

        if (m_csvStop.load(std::memory_order_acquire)) {
            csvDrain(sinceFlush); // final drain after producers stopped
            break;
        }
        if (written == 0)
            Sleep(50);
    }
    fflush(m_csvFile);
}
