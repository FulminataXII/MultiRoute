#pragma once
//==============================================================================
// Logging.h - Asynchronous, real-time-safe diagnostic logging.
//
// DESIGN CONSTRAINT: audio render/capture threads run under MMCSS "Pro Audio"
// scheduling. They must never allocate, lock a mutex, or perform I/O. So the
// hot path here does NOT format strings: it pushes a fixed-size POD record
// (event id + two integer args) into a lock-free bounded queue. A dedicated
// drain thread converts records to text and hands them to the sink.
//
// Consequence: a slow sink (a GUI repainting, a file flush) can never stall
// the audio path. Worst case the queue fills and records are dropped, which
// is counted and reported rather than silently lost.
//==============================================================================

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace mux {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5
};

inline const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

//------------------------------------------------------------------------------
// Stable event ids. Every diagnosable condition gets one, so failures can be
// traced by grepping a numeric code rather than matching prose.
//------------------------------------------------------------------------------
enum class Event : uint16_t {
    None = 0,

    // Lifecycle
    EngineStarting          = 100,
    EngineRunning           = 101,
    EngineStopping          = 102,
    EngineStopped           = 103,
    EngineFaulted           = 104,
    ThreadStarted           = 110,
    ThreadExiting           = 111,
    ComInitFailed           = 112,

    // Device setup
    DeviceActivated         = 200,
    DeviceActivateFailed    = 201,
    DeviceInitFailed        = 202,
    DeviceSkipped           = 203,
    ClockServiceFailed      = 204,
    BufferAllocFailed       = 205,
    MixFormatQueried        = 206,

    // Capture path
    CaptureStarted          = 300,
    CaptureStopped          = 301,
    CaptureEventTimeout     = 302,  // silence-starvation self-heal poll fired
    CaptureGetBufferFailed  = 303,
    CaptureReleaseFailed    = 304,
    CapturePacketSizeFailed = 305,
    CaptureWaitFailed       = 306,

    // Render path
    RenderStarted           = 400,
    RenderStopped           = 401,
    RenderPaddingFailed     = 402,
    RenderGetBufferFailed   = 403,
    RenderReleaseFailed     = 404,
    RenderWaitFailed        = 405,
    RenderPaddingImplausible= 406,
    PrebufferBegin          = 410,
    PrebufferComplete       = 411,
    PrebufferRestart        = 412,

    // Data-flow faults
    RingOverflow            = 500,
    PartialUnderflow        = 501,
    CatastrophicUnderflow   = 502,

    // Clock control
    RateApplied             = 600,
    RateApplyFailed         = 601,
    RateClampedAtLimit      = 602,

    // Volume control
    VolumeServiceFailed     = 700,  // IAudioStreamVolume unavailable; device still routes audio, just without volume control
    VolumeSetFailed         = 701,

    // Live delay adjustment
    DelayAdjustRequested    = 800,
    DelayAdjustSmoothed     = 801,  // small change: retarget only, controller absorbs it inaudibly
    DelayAdjustSpliced      = 802,  // large change: muted, buffer spliced, unmuted
    DelayAdjustTimeout      = 803,
    DelayAdjustNoMute       = 804,  // splice needed but this device has no volume control; expect a click
    DelaySpliceShortfall    = 805,  // ring held less than we needed to discard

    // Generic text carrier (message lives in LogRecord::text)
    Message                 = 900
};

// Format strings receive exactly two long long args; extra args to snprintf
// are ignored per the C standard, so formats may use fewer placeholders.
inline const char* eventFormat(Event e) {
    switch (e) {
        case Event::EngineStarting:           return "engine starting";
        case Event::EngineRunning:            return "engine running (%lld outputs)";
        case Event::EngineStopping:           return "engine stopping";
        case Event::EngineStopped:            return "engine stopped";
        case Event::EngineFaulted:            return "engine faulted";
        case Event::ThreadStarted:            return "thread started";
        case Event::ThreadExiting:            return "thread exiting";
        case Event::ComInitFailed:            return "CoInitializeEx failed on worker thread (hr=0x%llx)";
        case Event::DeviceActivated:          return "device activated";
        case Event::DeviceActivateFailed:     return "IMMDevice::Activate failed (hr=0x%llx)";
        case Event::DeviceInitFailed:         return "IAudioClient::Initialize failed (hr=0x%llx)";
        case Event::DeviceSkipped:            return "device skipped";
        case Event::ClockServiceFailed:       return "IAudioClockAdjustment unavailable (hr=0x%llx)";
        case Event::BufferAllocFailed:        return "ring buffer allocation failed";
        case Event::MixFormatQueried:         return "mix format: %lld Hz, block align %lld bytes";
        case Event::CaptureStarted:           return "loopback capture started";
        case Event::CaptureStopped:           return "loopback capture stopped";
        case Event::CaptureEventTimeout:      return "capture event timed out, polling (count=%lld)";
        case Event::CaptureGetBufferFailed:   return "capture GetBuffer failed (hr=0x%llx)";
        case Event::CaptureReleaseFailed:     return "capture ReleaseBuffer failed (hr=0x%llx)";
        case Event::CapturePacketSizeFailed:  return "capture GetNextPacketSize failed (hr=0x%llx)";
        case Event::CaptureWaitFailed:        return "capture wait failed (result=%lld)";
        case Event::RenderStarted:            return "render started";
        case Event::RenderStopped:            return "render stopped";
        case Event::RenderPaddingFailed:      return "GetCurrentPadding failed (hr=0x%llx)";
        case Event::RenderGetBufferFailed:    return "render GetBuffer failed (hr=0x%llx)";
        case Event::RenderReleaseFailed:      return "render ReleaseBuffer failed (hr=0x%llx)";
        case Event::RenderWaitFailed:         return "render wait failed (result=%lld)";
        case Event::RenderPaddingImplausible: return "padding %lld exceeds buffer %lld frames";
        case Event::PrebufferBegin:           return "prebuffering (target %lld bytes)";
        case Event::PrebufferComplete:        return "prebuffer complete (fill %lld bytes)";
        case Event::PrebufferRestart:         return "starvation exceeded %lld ms, rebuilding prebuffer";
        case Event::RingOverflow:             return "ring overflow, dropped %lld bytes (total %lld)";
        case Event::PartialUnderflow:         return "partial underflow, %lld of %lld bytes short";
        case Event::CatastrophicUnderflow:    return "underflow, %lld bytes of silence emitted";
        case Event::RateApplied:              return "sample rate -> %lld.%03lld Hz";
        case Event::RateApplyFailed:          return "SetSampleRate failed (hr=0x%llx)";
        case Event::RateClampedAtLimit:       return "rate correction clamped at limit";
        case Event::VolumeServiceFailed:      return "IAudioStreamVolume unavailable (hr=0x%llx); volume control disabled for this device";
        case Event::VolumeSetFailed:          return "SetAllVolumes failed (hr=0x%llx)";
        case Event::DelayAdjustRequested:     return "delay change requested: %lld ms -> %lld ms";
        case Event::DelayAdjustSmoothed:      return "delay retargeted to %lld ms; drift controller will absorb it (no interruption)";
        case Event::DelayAdjustSpliced:       return "delay spliced to %lld ms (%lld bytes moved) with brief mute";
        case Event::DelayAdjustTimeout:       return "render thread did not complete splice within timeout (%lld bytes outstanding)";
        case Event::DelayAdjustNoMute:        return "splicing without mute (device has no volume control); a click is expected";
        case Event::DelaySpliceShortfall:     return "wanted to discard %lld bytes, buffer only held %lld";
        case Event::Message:                  return "%s";
        case Event::None:                     return "(none)";
    }
    return "unknown event";
}

//------------------------------------------------------------------------------
// LogRecord: POD, trivially copyable. Sized to stay cache-friendly.
//------------------------------------------------------------------------------
struct LogRecord {
    int64_t   timestamp_ns;
    int64_t   a;
    int64_t   b;
    Event     event;
    LogLevel  level;
    int16_t   device;      // -1 == engine-wide
    bool      has_text;
    char      text[144];   // only populated by non-RT callers
};

//------------------------------------------------------------------------------
// Bounded lock-free MPMC queue (Vyukov). Multiple producers = every audio
// thread; single consumer = the drain thread. try_push never blocks and never
// allocates, which is what makes it safe from the RT path.
//------------------------------------------------------------------------------
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t requested) {
        size_t cap = 1;
        while (cap < requested) cap <<= 1;
        capacity_ = cap;
        mask_ = cap - 1;
        cells_.reset(new Cell[cap]);
        for (size_t i = 0; i < cap; ++i)
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    bool try_push(const T& value) noexcept {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = value;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;  // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = cell->data;
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    size_t capacity() const noexcept { return capacity_; }

private:
    struct alignas(64) Cell {
        std::atomic<size_t> sequence;
        T data{};
    };

    std::unique_ptr<Cell[]> cells_;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    alignas(64) std::atomic<size_t> dequeue_pos_{0};
};

//------------------------------------------------------------------------------
// Formatted output handed to sinks.
//------------------------------------------------------------------------------
struct LogMessage {
    int64_t     timestamp_ns;
    LogLevel    level;
    int         device;
    Event       event;
    std::string text;
};

using LogSink = std::function<void(const LogMessage&)>;

//------------------------------------------------------------------------------
// Logger
//------------------------------------------------------------------------------
class Logger {
public:
    explicit Logger(size_t queueCapacity = 2048)
        : queue_(queueCapacity) {}

    ~Logger() { stop(); }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Sink is invoked on the drain thread, never on an audio thread.
    // Set before start() to avoid racing the drain loop.
    void setSink(LogSink sink) { sink_ = std::move(sink); }

    void setMinLevel(LogLevel l) { minLevel_.store(l, std::memory_order_relaxed); }
    LogLevel minLevel() const { return minLevel_.load(std::memory_order_relaxed); }

    void start() {
        if (running_.exchange(true)) return;
        drain_ = std::thread([this] { drainLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (drain_.joinable()) drain_.join();
        flushPending();  // emit anything queued after the loop's last pass
    }

    // ---- REAL-TIME SAFE: no allocation, no lock, no formatting ----
    void event(LogLevel level, Event ev, int device = -1,
               int64_t a = 0, int64_t b = 0) noexcept {
        if (level < minLevel_.load(std::memory_order_relaxed)) return;
        LogRecord r{};
        r.timestamp_ns = nowNs();
        r.a = a;
        r.b = b;
        r.event = ev;
        r.level = level;
        r.device = static_cast<int16_t>(device);
        r.has_text = false;
        if (!queue_.try_push(r))
            dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- NOT RT-safe (uses vsnprintf): setup/teardown/control threads only ----
    void message(LogLevel level, int device, const char* fmt, ...) {
        if (level < minLevel_.load(std::memory_order_relaxed)) return;
        LogRecord r{};
        r.timestamp_ns = nowNs();
        r.event = Event::Message;
        r.level = level;
        r.device = static_cast<int16_t>(device);
        r.has_text = true;
        va_list args;
        va_start(args, fmt);
        vsnprintf(r.text, sizeof(r.text), fmt, args);
        va_end(args);
        if (!queue_.try_push(r))
            dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

    static int64_t nowNs() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

private:
    void drainLoop() {
        LogRecord r;
        while (running_.load(std::memory_order_relaxed)) {
            bool any = false;
            while (queue_.try_pop(r)) {
                emit(r);
                any = true;
            }
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void flushPending() {
        LogRecord r;
        while (queue_.try_pop(r)) emit(r);
        uint64_t d = dropped_.load(std::memory_order_relaxed);
        if (d > 0 && sink_) {
            LogMessage m;
            m.timestamp_ns = nowNs();
            m.level = LogLevel::Warn;
            m.device = -1;
            m.event = Event::Message;
            m.text = "log queue overflowed; " + std::to_string(d) + " record(s) dropped";
            sink_(m);
        }
    }

    void emit(const LogRecord& r) {
        if (!sink_) return;
        char buf[256];
        if (r.has_text) {
            snprintf(buf, sizeof(buf), "%s", r.text);
        } else {
            snprintf(buf, sizeof(buf), eventFormat(r.event),
                     static_cast<long long>(r.a), static_cast<long long>(r.b));
        }
        LogMessage m;
        m.timestamp_ns = r.timestamp_ns;
        m.level = r.level;
        m.device = r.device;
        m.event = r.event;
        m.text = buf;
        sink_(m);
    }

    BoundedQueue<LogRecord> queue_;
    LogSink sink_;
    std::thread drain_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<LogLevel> minLevel_{LogLevel::Info};
};

}  // namespace mux
