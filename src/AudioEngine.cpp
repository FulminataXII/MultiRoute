//==============================================================================
// AudioEngine.cpp - WASAPI implementation of the router core.
//
// Ownership model (this is the part that keeps WASAPI happy):
//   supervisor thread : IMMDeviceEnumerator, IAudioClient (capture + each
//                       render), IAudioClockAdjustment. Acquired and released
//                       on this one thread, satisfying WASAPI's same-thread
//                       release affinity for service objects.
//   capture thread    : IAudioCaptureClient  (acquired + released in-thread)
//   render thread     : IAudioRenderClient   (acquired + released in-thread)
//   control thread    : owns no COM object at all; it only calls SetSampleRate
//                       on an interface the supervisor owns, which is the
//                       documented use of AUDCLNT_STREAMFLAGS_RATEADJUST and
//                       keeps that call off the real-time render thread.
//==============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "AudioEngine.h"
#include "AudioRingBuffer.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

// Ole32 supplies CoInitializeEx / CoCreateInstance / CoTaskMemFree /
// PropVariantClear. A Visual Studio project links it by default but a bare
// `cl` command line does not, which is exactly the LNK2019 wall you hit.
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Avrt.lib")

using Microsoft::WRL::ComPtr;

namespace mux {

const char* engineStateName(EngineState s) {
    switch (s) {
        case EngineState::Idle:     return "Idle";
        case EngineState::Starting: return "Starting";
        case EngineState::Running:  return "Running";
        case EngineState::Stopping: return "Stopping";
        case EngineState::Faulted:  return "Faulted";
    }
    return "Unknown";
}

namespace {

//------------------------------------------------------------------------------
// RAII helpers
//------------------------------------------------------------------------------
struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { CoTaskMemFree(p); }
};
using CoTaskMemString = std::unique_ptr<WCHAR, CoTaskMemDeleter>;

// CoInitializeEx returns S_FALSE when COM is already initialised on this
// thread; that case still requires a matching CoUninitialize. Only
// RPC_E_CHANGED_MODE means "we did not initialise it, do not tear it down".
struct ScopedCom {
    HRESULT hr = E_FAIL;
    bool owns = false;

    ScopedCom() {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns = SUCCEEDED(hr);
    }
    ~ScopedCom() {
        if (owns) CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }

    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;
};

// COM + optional MMCSS "Pro Audio" scheduling for one worker thread.
struct ThreadEnvironment {
    ScopedCom com;
    HANDLE mmcssTask = nullptr;

    explicit ThreadEnvironment(bool realtime) {
        if (realtime && com.usable()) {
            DWORD taskIndex = 0;
            mmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        }
    }
    ~ThreadEnvironment() {
        if (mmcssTask) AvRevertMmThreadCharacteristics(mmcssTask);
    }
    bool comUsable() const { return com.usable(); }
    HRESULT comResult() const { return com.hr; }
};

struct ScopedPropVariant {
    PROPVARIANT v;
    ScopedPropVariant() { PropVariantInit(&v); }
    ~ScopedPropVariant() { PropVariantClear(&v); }
    ScopedPropVariant(const ScopedPropVariant&) = delete;
    ScopedPropVariant& operator=(const ScopedPropVariant&) = delete;
};

// IAudioStreamVolume::SetAllVolumes takes a pointer to one gain value per
// channel, not a single scalar — this project only ever wants the same gain
// on every channel, so this wraps that array-building detail in one place.
HRESULT setUniformStreamVolume(IAudioStreamVolume* vol, UINT32 channelCount, float level) {
    std::vector<float> levels(channelCount, level);
    return vol->SetAllVolumes(channelCount, levels.data());
}

std::wstring deviceFriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"Unknown Device";
    ScopedPropVariant name;
    if (FAILED(props->GetValue(PKEY_Device_FriendlyName, &name.v))) return L"Unknown Device";
    if (name.v.vt != VT_LPWSTR || name.v.pwszVal == nullptr) return L"Unknown Device";
    return std::wstring(name.v.pwszVal);
}

//------------------------------------------------------------------------------
// Pure calculations — deliberately free of WASAPI/COM so they can be unit
// tested standalone (given a test harness) without mocking any interface.
//------------------------------------------------------------------------------

// Shifts a set of user-requested delays so the smallest becomes zero — the
// engine only cares about offsets relative to each other, not their absolute
// values, and normalizing here keeps every later calculation non-negative.
void normalizeDelays(std::vector<int64_t>& delays) {
    if (delays.empty()) return;
    const int64_t minDelay = *std::min_element(delays.begin(), delays.end());
    for (auto& d : delays) d -= minDelay;
}

struct DeviceBufferSizing {
    size_t padding_bytes;
    size_t target_fill_bytes;
    size_t required_capacity_bytes;
};

// Converts one device's requested delay into concrete byte counts. Throws
// std::overflow_error for a pathological delay rather than silently wrapping
// — the caller (startLocked) already treats that as "skip this device", not
// a fatal error for the whole engine.
DeviceBufferSizing computeBufferSizing(int64_t delay_ms, double bytes_per_ms, size_t block_align,
                                       size_t cushion_bytes, size_t bytes_per_second) {
    size_t paddingBytes = static_cast<size_t>(static_cast<double>(delay_ms) * bytes_per_ms);
    if (block_align > 0) paddingBytes -= (paddingBytes % block_align);

    const size_t targetFill = cushion_bytes + paddingBytes;
    if (targetFill > SIZE_MAX / 4) throw std::overflow_error("requested delay too large");

    const size_t requiredCapacity = std::max(targetFill * 4, bytes_per_second);
    return {paddingBytes, targetFill, requiredCapacity};
}

}  // namespace

//------------------------------------------------------------------------------
// Per-device runtime state
//------------------------------------------------------------------------------
struct DeviceRuntime {
    int index = 0;
    std::wstring endpoint_id;
    std::wstring name;
    int64_t delay_ms = 0;  // startup value only; live value lives in current_delay_ms below

    // Owned by supervisor thread.
    ComPtr<IAudioClient> client;
    ComPtr<IAudioClockAdjustment> clockAdjuster;
    ComPtr<IAudioStreamVolume> streamVolume;  // optional: absent devices still route audio, just without volume control
    UINT32 volumeChannelCount = 0;
    UniqueHandle renderEvent;
    UniqueHandle stopEvent;  // set only when isolate_device_faults removes this device alone

    std::atomic<bool> faulted{false};

    std::unique_ptr<AudioRingBuffer> ring;
    std::unique_ptr<ClockDriftController> controller;

    // target_bytes is mutated live by setDeviceDelay and read concurrently by
    // the render thread (prebuffer check) and control thread (drift target),
    // so it must be atomic. capacity_bytes/control_range_bytes are fixed at
    // start() and never change while threads run, so they stay plain.
    std::atomic<size_t>   target_bytes{0};
    size_t capacity_bytes = 0;
    size_t control_range_bytes = 0;

    // Live delay state. current_delay_ms is what telemetry reports;
    // max_delay_bytes is the ceiling this session's ring can hold.
    std::atomic<int64_t>  current_delay_ms{0};
    size_t max_delay_bytes = 0;

    // Splice channel, control thread -> render thread. Positive: render owes
    // this many bytes of inserted silence (grows the buffer, increasing
    // delay). Negative: render must discard this many bytes (shrinking it).
    // The render thread — the ring's only legal consumer — is what actually
    // performs the work and drives this back to zero, which is why a delay
    // change never has a third thread touching the ring's indices.
    std::atomic<int64_t>  spliceRemaining{0};

    // Request channel, caller -> control thread. delayAdjustInFlight also
    // serves as the mutual-exclusion guard rejecting overlapping requests.
    std::atomic<bool>     delayAdjustInFlight{false};
    std::atomic<int64_t>  requestedDelayMs{0};
    std::atomic<size_t>   requestedTargetBytes{0};
    UniqueHandle          delayRequestEvent;  // wakes the control thread immediately instead of waiting out its tick

    std::atomic<size_t>   fill{0};
    std::atomic<float>    sample_rate{0.0f};
    std::atomic<float>    ema_error{0.0f};
    std::atomic<float>    volume{1.0f};
    std::atomic<bool>     prebuffering{true};
    std::atomic<bool>     running{false};
    std::atomic<uint64_t> overflow{0};
    std::atomic<uint64_t> partial_underflow{0};
    std::atomic<uint64_t> catastrophic_underflow{0};
    std::atomic<uint64_t> api_error{0};

    std::thread renderThread;
    std::thread controlThread;
};

//------------------------------------------------------------------------------
// Command plumbing (the "async user input" channel)
//------------------------------------------------------------------------------
struct Command {
    enum class Type { Start, Stop, Quit };
    Type type = Type::Stop;
    EngineConfig config;
    std::shared_ptr<std::promise<std::string>> reply;  // "" == success
};

//------------------------------------------------------------------------------
// Impl
//------------------------------------------------------------------------------
struct AudioEngine::Impl {
    Logger logger{4096};
    std::function<void(EngineState)> stateCallback;
    std::atomic<EngineState> state{EngineState::Idle};

    // Command queue
    std::mutex cmdMutex;
    std::condition_variable cmdCv;
    std::deque<Command> commands;
    std::thread supervisor;
    std::atomic<bool> supervisorAlive{false};

    // Fault signalling from worker threads to supervisor
    std::atomic<bool> faultFlag{false};

    // Runtime, touched by supervisor thread only (except atomics)
    UniqueHandle shutdownEvent;
    UniqueHandle captureEvent;
    ComPtr<IAudioClient> captureClient;
    std::vector<uint8_t> mixFormatBlob;
    uint32_t sampleRate = 0;
    uint16_t blockAlign = 0;
    // Derived sizing constants for the current session. Written once by
    // startLocked, read afterward by publishStats and setDeviceDelay, both of
    // which need them to convert between milliseconds and bytes.
    double bytesPerMs = 0.0;
    size_t bytesPerSecond = 0;
    size_t cushionBytes = 0;
    std::thread captureThread;
    std::atomic<uint64_t> capturePollTimeouts{0};

    EngineConfig activeConfig;

    mutable std::mutex devicesMutex;
    std::vector<std::unique_ptr<DeviceRuntime>> devices;

    // Read path for stats(): the GUI (or any frontend) can poll this every
    // frame without ever taking devicesMutex, which the supervisor also
    // holds during device teardown/setup — a UI thread blocking on a lock
    // held mid-restart is a textbook case of a low-priority reader stalling
    // behind a busy writer. Instead the supervisor periodically builds a
    // plain-data snapshot (no COM handles, just values already copied out of
    // atomics) and publishes it with a single pointer swap; stats() just
    // reads whatever was last published. Same idea as RCU or double-
    // buffering: the writer never blocks readers, and readers never see a
    // half-updated structure. std::atomic_load/store on shared_ptr (not the
    // C++20 std::atomic<shared_ptr<T>> specialization) because this project
    // targets C++17.
    std::shared_ptr<const EngineStats> publishedStats = std::make_shared<const EngineStats>();

    // Builds a fresh snapshot from the current device list and publishes it.
    // Called only from the supervisor thread (the sole owner/writer of
    // `devices`), so the brief devicesMutex lock here never contends with a
    // reader — only ever with another supervisor-thread operation, which by
    // definition can't be running concurrently with this one.
    void publishStats() {
        auto snapshot = std::make_shared<EngineStats>();
        snapshot->running = (state.load(std::memory_order_acquire) == EngineState::Running);
        snapshot->mix_sample_rate = sampleRate;
        snapshot->mix_block_align = blockAlign;
        snapshot->capture_poll_timeouts = capturePollTimeouts.load(std::memory_order_relaxed);
        snapshot->log_records_dropped = logger.droppedCount();

        std::lock_guard<std::mutex> lock(devicesMutex);
        snapshot->devices.reserve(devices.size());
        for (const auto& d : devices) {
            DeviceStats ds;
            ds.name = d->name;
            ds.endpoint_id = d->endpoint_id;
            ds.fill_bytes = d->fill.load(std::memory_order_relaxed);
            ds.target_bytes = d->target_bytes.load(std::memory_order_relaxed);
            ds.capacity_bytes = d->capacity_bytes;
            ds.configured_delay_ms = d->current_delay_ms.load(std::memory_order_relaxed);
            ds.max_delay_ms_for_device =
                bytesPerMs > 0.0
                    ? static_cast<int64_t>(static_cast<double>(d->max_delay_bytes) / bytesPerMs)
                    : 0;
            ds.delay_adjust_in_flight = d->delayAdjustInFlight.load(std::memory_order_relaxed);
            ds.sample_rate_hz = d->sample_rate.load(std::memory_order_relaxed);
            ds.ema_error = d->ema_error.load(std::memory_order_relaxed);
            ds.volume = d->volume.load(std::memory_order_relaxed);
            ds.prebuffering = d->prebuffering.load(std::memory_order_relaxed);
            ds.running = d->running.load(std::memory_order_relaxed);
            ds.faulted = d->faulted.load(std::memory_order_relaxed);
            ds.overflow_count = d->overflow.load(std::memory_order_relaxed);
            ds.partial_underflow_count = d->partial_underflow.load(std::memory_order_relaxed);
            ds.catastrophic_underflow_count =
                d->catastrophic_underflow.load(std::memory_order_relaxed);
            ds.api_error_count = d->api_error.load(std::memory_order_relaxed);
            snapshot->devices.push_back(std::move(ds));
        }
        std::atomic_store(&publishedStats, std::shared_ptr<const EngineStats>(std::move(snapshot)));
    }

    WAVEFORMATEX* mixFormat() {
        return mixFormatBlob.empty()
                   ? nullptr
                   : reinterpret_cast<WAVEFORMATEX*>(mixFormatBlob.data());
    }

    void setState(EngineState s) {
        state.store(s, std::memory_order_release);
        if (stateCallback) stateCallback(s);
    }

    void signalFault() {
        faultFlag.store(true, std::memory_order_release);
        if (shutdownEvent) SetEvent(shutdownEvent.get());
    }

    // Single choke point for every render/control-thread fatal error. Whether
    // a faulted output takes the whole engine down with it is controlled by
    // EngineConfig::isolate_device_faults — flip that one field (e.g. from a
    // GUI settings checkbox) and the behavior changes with no other code
    // touched. Capture-thread failures are NOT routed through here: the
    // shared loopback source dying affects every output equally, so it is
    // always engine-fatal regardless of this setting.
    void reportDeviceFault(DeviceRuntime* dev) {
        dev->faulted.store(true, std::memory_order_release);
        dev->api_error.fetch_add(1, std::memory_order_relaxed);
        if (activeConfig.isolate_device_faults) {
            // This device's thread exits via its own stopEvent; capture keeps
            // writing into its ring buffer, which just harmlessly overflows
            // (counted, visible in telemetry) until pruneFaultedDevices()
            // removes it on the supervisor thread.
            if (dev->stopEvent) SetEvent(dev->stopEvent.get());
        } else {
            signalFault();
        }
    }

    // Removes and joins any device whose thread already exited after a
    // reportDeviceFault() call. Runs on the supervisor thread, so releasing
    // its COM objects here satisfies the same-thread affinity they were
    // acquired under. Returns devices remaining afterward.
    size_t pruneFaultedDevices() {
        std::lock_guard<std::mutex> lock(devicesMutex);
        devices.erase(
            std::remove_if(devices.begin(), devices.end(),
                           [this](std::unique_ptr<DeviceRuntime>& d) {
                               if (!d->faulted.load(std::memory_order_acquire)) return false;
                               if (d->renderThread.joinable()) d->renderThread.join();
                               if (d->controlThread.joinable()) d->controlThread.join();
                               logger.message(LogLevel::Warn, d->index,
                                              "device isolated and removed after fault");
                               return true;
                           }),
            devices.end());
        return devices.size();
    }

    void signalShutdown() {
        if (shutdownEvent) SetEvent(shutdownEvent.get());
    }

    // --- supervisor-thread operations ---
    std::string startLocked(const EngineConfig& cfg);
    void stopLocked();
    void supervisorLoop();

    // --- worker loops ---
    void captureLoop();
    void renderLoop(DeviceRuntime* dev);
    void controlLoop(DeviceRuntime* dev);
    void applyDelayAdjustment(DeviceRuntime* dev);
};

//==============================================================================
// Capture thread
//==============================================================================
void AudioEngine::Impl::captureLoop() {
    ThreadEnvironment env(true);
    logger.event(LogLevel::Debug, Event::ThreadStarted, -1);

    if (!env.comUsable()) {
        logger.event(LogLevel::Fatal, Event::ComInitFailed, -1,
                     static_cast<int64_t>(env.comResult()));
        signalFault();
        return;
    }

    ComPtr<IAudioCaptureClient> captureService;
    HRESULT hr = captureClient->GetService(IID_PPV_ARGS(&captureService));
    if (FAILED(hr)) {
        logger.event(LogLevel::Fatal, Event::CaptureGetBufferFailed, -1,
                     static_cast<int64_t>(hr));
        signalFault();
        return;
    }

    hr = captureClient->Start();
    if (FAILED(hr)) {
        logger.event(LogLevel::Fatal, Event::CaptureGetBufferFailed, -1,
                     static_cast<int64_t>(hr));
        signalFault();
        return;
    }
    logger.event(LogLevel::Info, Event::CaptureStarted, -1);

    // Snapshot device pointers once; the device list is immutable while running.
    std::vector<DeviceRuntime*> targets;
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        targets.reserve(devices.size());
        for (auto& d : devices) targets.push_back(d.get());
    }

    HANDLE waitArray[2] = {shutdownEvent.get(), captureEvent.get()};
    const DWORD pollMs = activeConfig.capture_poll_ms;
    const size_t align = blockAlign;

    bool stop = false;
    while (!stop) {
        const DWORD waitResult = WaitForMultipleObjects(2, waitArray, FALSE, pollMs);

        if (waitResult == WAIT_OBJECT_0) break;  // shutdown

        if (waitResult == WAIT_TIMEOUT) {
            // Event-driven WASAPI loopback stops signalling when the render
            // endpoint has no active playback (reproduced across NAudio,
            // PortAudio and miniaudio). The bounded wait converts that from a
            // permanent hang into a poll, so we fall through and ask the
            // service directly whether anything is queued.
            capturePollTimeouts.fetch_add(1, std::memory_order_relaxed);
            logger.event(LogLevel::Trace, Event::CaptureEventTimeout, -1,
                         static_cast<int64_t>(capturePollTimeouts.load(std::memory_order_relaxed)));
        } else if (waitResult != WAIT_OBJECT_0 + 1) {
            logger.event(LogLevel::Fatal, Event::CaptureWaitFailed, -1,
                         static_cast<int64_t>(waitResult));
            signalFault();
            break;
        }

        UINT32 packetLength = 0;
        hr = captureService->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            logger.event(LogLevel::Fatal, Event::CapturePacketSizeFailed, -1,
                         static_cast<int64_t>(hr));
            signalFault();
            break;
        }

        while (packetLength != 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = captureService->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                logger.event(LogLevel::Fatal, Event::CaptureGetBufferFailed, -1,
                             static_cast<int64_t>(hr));
                signalFault();
                stop = true;
                break;
            }

            const size_t bytes = static_cast<size_t>(frames) * align;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            for (DeviceRuntime* dev : targets) {
                const bool ok = silent ? dev->ring->write_silence(bytes)
                                       : dev->ring->write(data, bytes);
                if (!ok) {
                    const uint64_t total =
                        dev->overflow.fetch_add(1, std::memory_order_relaxed) + 1;
                    logger.event(LogLevel::Warn, Event::RingOverflow, dev->index,
                                 static_cast<int64_t>(bytes),
                                 static_cast<int64_t>(total));
                }
            }

            // GetBuffer/ReleaseBuffer must alternate on this thread. A failed
            // release leaves the stream in an indeterminate state, so it is
            // fatal rather than merely counted.
            hr = captureService->ReleaseBuffer(frames);
            if (FAILED(hr)) {
                logger.event(LogLevel::Fatal, Event::CaptureReleaseFailed, -1,
                             static_cast<int64_t>(hr));
                signalFault();
                stop = true;
                break;
            }

            hr = captureService->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                logger.event(LogLevel::Fatal, Event::CapturePacketSizeFailed, -1,
                             static_cast<int64_t>(hr));
                signalFault();
                stop = true;
                break;
            }
        }
    }

    captureClient->Stop();
    logger.event(LogLevel::Info, Event::CaptureStopped, -1);
    logger.event(LogLevel::Debug, Event::ThreadExiting, -1);
}

//==============================================================================
// Render thread (real-time)
//==============================================================================
void AudioEngine::Impl::renderLoop(DeviceRuntime* dev) {
    ThreadEnvironment env(true);
    logger.event(LogLevel::Debug, Event::ThreadStarted, dev->index);

    if (!env.comUsable()) {
        logger.event(LogLevel::Fatal, Event::ComInitFailed, dev->index,
                     static_cast<int64_t>(env.comResult()));
        reportDeviceFault(dev);
        return;
    }

    ComPtr<IAudioRenderClient> renderService;
    HRESULT hr = dev->client->GetService(IID_PPV_ARGS(&renderService));
    if (FAILED(hr)) {
        logger.event(LogLevel::Fatal, Event::RenderGetBufferFailed, dev->index,
                     static_cast<int64_t>(hr));
        reportDeviceFault(dev);
        return;
    }

    UINT32 bufferFrameCount = 0;
    hr = dev->client->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr) || bufferFrameCount == 0) {
        logger.event(LogLevel::Fatal, Event::RenderPaddingFailed, dev->index,
                     static_cast<int64_t>(hr));
        reportDeviceFault(dev);
        return;
    }

    const size_t align = blockAlign;

    // WASAPI requires the endpoint buffer to be primed before Start().
    {
        BYTE* initial = nullptr;
        hr = renderService->GetBuffer(bufferFrameCount, &initial);
        if (SUCCEEDED(hr)) {
            memset(initial, 0, static_cast<size_t>(bufferFrameCount) * align);
            hr = renderService->ReleaseBuffer(bufferFrameCount, 0);
            if (FAILED(hr)) {
                logger.event(LogLevel::Fatal, Event::RenderReleaseFailed, dev->index,
                             static_cast<int64_t>(hr));
                reportDeviceFault(dev);
                return;
            }
        }
    }

    hr = dev->client->Start();
    if (FAILED(hr)) {
        logger.event(LogLevel::Fatal, Event::RenderGetBufferFailed, dev->index,
                     static_cast<int64_t>(hr));
        reportDeviceFault(dev);
        return;
    }

    dev->running.store(true, std::memory_order_relaxed);
    logger.event(LogLevel::Info, Event::RenderStarted, dev->index);
    logger.event(LogLevel::Debug, Event::PrebufferBegin, dev->index,
                 static_cast<int64_t>(dev->target_bytes.load(std::memory_order_relaxed)));

    // Index 2 (stopEvent) only ever gets signaled by reportDeviceFault() in
    // isolate_device_faults mode, letting this thread exit on its own without
    // taking the shared shutdownEvent (and every other device) down with it.
    HANDLE waitArray[3] = {shutdownEvent.get(), dev->renderEvent.get(), dev->stopEvent.get()};
    bool prebuffering = true;
    bool starving = false;
    auto starvationStart = std::chrono::steady_clock::now();
    const auto starvationLimit =
        std::chrono::milliseconds(activeConfig.starvation_reset_ms);

    for (;;) {
        const DWORD waitResult = WaitForMultipleObjects(3, waitArray, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) break;                  // global shutdown
        if (waitResult == WAIT_OBJECT_0 + 2) break;               // this device isolated out
        if (waitResult != WAIT_OBJECT_0 + 1) {
            logger.event(LogLevel::Fatal, Event::RenderWaitFailed, dev->index,
                         static_cast<int64_t>(waitResult));
            reportDeviceFault(dev);
            break;
        }

        UINT32 padding = 0;
        hr = dev->client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            logger.event(LogLevel::Fatal, Event::RenderPaddingFailed, dev->index,
                         static_cast<int64_t>(hr));
            reportDeviceFault(dev);
            break;
        }
        if (padding > bufferFrameCount) {
            logger.event(LogLevel::Fatal, Event::RenderPaddingImplausible, dev->index,
                         static_cast<int64_t>(padding),
                         static_cast<int64_t>(bufferFrameCount));
            reportDeviceFault(dev);
            break;
        }

        const UINT32 framesAvailable = bufferFrameCount - padding;
        if (framesAvailable == 0) continue;

        // Delay-shrink splice. Done here, on the ring's one legal consumer
        // thread, rather than by whichever thread requested the change —
        // that's what keeps the SPSC invariant intact. Discarding buffered
        // audio is instantaneous; the control thread has already muted this
        // device (and only this device) around it if the jump was large
        // enough to be audible.
        int64_t splice = dev->spliceRemaining.load(std::memory_order_acquire);
        if (splice < 0) {
            const size_t want = static_cast<size_t>(-splice);
            const size_t dropped = dev->ring->skip(want);
            if (dropped < want)
                logger.event(LogLevel::Warn, Event::DelaySpliceShortfall, dev->index,
                             static_cast<int64_t>(want), static_cast<int64_t>(dropped));
            dev->spliceRemaining.store(0, std::memory_order_release);
            splice = 0;
        }

        BYTE* out = nullptr;
        hr = renderService->GetBuffer(framesAvailable, &out);
        if (FAILED(hr)) {
            logger.event(LogLevel::Fatal, Event::RenderGetBufferFailed, dev->index,
                         static_cast<int64_t>(hr));
            reportDeviceFault(dev);
            break;
        }

        const size_t requested = static_cast<size_t>(framesAvailable) * align;
        const size_t currentFill = dev->ring->get_available_read();

        if (prebuffering && currentFill >= dev->target_bytes.load(std::memory_order_relaxed)) {
            prebuffering = false;
            starving = false;
            dev->prebuffering.store(false, std::memory_order_relaxed);
            logger.event(LogLevel::Info, Event::PrebufferComplete, dev->index,
                         static_cast<int64_t>(currentFill));
        }

        if (prebuffering) {
            memset(out, 0, requested);
        } else if (splice > 0) {
            // Delay-grow splice: emit silence WITHOUT draining the ring, so
            // capture's continued writes raise the fill level by exactly the
            // requested amount. Underflow counters are deliberately not
            // touched here — this silence is intentional, and counting it as
            // a fault would corrupt the very telemetry used to judge whether
            // the device is healthy.
            const size_t emit = std::min(requested, static_cast<size_t>(splice));
            memset(out, 0, emit);
            if (emit < requested) {
                const size_t rest = requested - emit;
                const size_t got = dev->ring->read(out + emit, rest);
                if (got < rest) memset(out + emit + got, 0, rest - got);
            }
            dev->spliceRemaining.fetch_sub(static_cast<int64_t>(emit), std::memory_order_release);
        } else {
            const size_t got = dev->ring->read(out, requested);
            if (got == 0) {
                memset(out, 0, requested);
                dev->catastrophic_underflow.fetch_add(1, std::memory_order_relaxed);
                logger.event(LogLevel::Warn, Event::CatastrophicUnderflow, dev->index,
                             static_cast<int64_t>(requested));
                if (!starving) {
                    starving = true;
                    starvationStart = std::chrono::steady_clock::now();
                } else if (std::chrono::steady_clock::now() - starvationStart >
                           starvationLimit) {
                    prebuffering = true;
                    starving = false;
                    dev->prebuffering.store(true, std::memory_order_relaxed);
                    logger.event(LogLevel::Warn, Event::PrebufferRestart, dev->index,
                                 static_cast<int64_t>(activeConfig.starvation_reset_ms));
                }
            } else if (got < requested) {
                memset(out + got, 0, requested - got);
                dev->partial_underflow.fetch_add(1, std::memory_order_relaxed);
                logger.event(LogLevel::Debug, Event::PartialUnderflow, dev->index,
                             static_cast<int64_t>(requested - got),
                             static_cast<int64_t>(requested));
                starving = false;
            } else {
                starving = false;
            }
        }

        dev->fill.store(dev->ring->get_available_read(), std::memory_order_relaxed);

        hr = renderService->ReleaseBuffer(framesAvailable, 0);
        if (FAILED(hr)) {
            logger.event(LogLevel::Fatal, Event::RenderReleaseFailed, dev->index,
                         static_cast<int64_t>(hr));
            reportDeviceFault(dev);
            break;
        }
    }

    dev->client->Stop();
    dev->running.store(false, std::memory_order_relaxed);
    logger.event(LogLevel::Info, Event::RenderStopped, dev->index);
    logger.event(LogLevel::Debug, Event::ThreadExiting, dev->index);
}

//==============================================================================
// Clock control thread (non-real-time by design)
//==============================================================================
void AudioEngine::Impl::controlLoop(DeviceRuntime* dev) {
    ThreadEnvironment env(false);
    logger.event(LogLevel::Debug, Event::ThreadStarted, dev->index);

    if (!env.comUsable()) {
        logger.event(LogLevel::Error, Event::ComInitFailed, dev->index,
                     static_cast<int64_t>(env.comResult()));
        reportDeviceFault(dev);
        return;
    }

    const DWORD period = activeConfig.control_period_ms;
    HANDLE waitArray[3] = {shutdownEvent.get(), dev->stopEvent.get(),
                           dev->delayRequestEvent.get()};

    for (;;) {
        const DWORD waitResult = WaitForMultipleObjects(3, waitArray, FALSE, period);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_OBJECT_0 + 1) break;

        // A delay request wakes this thread immediately rather than making
        // the user wait out a control tick. Handling it here, on this
        // device's own non-realtime control thread, is what keeps every
        // OTHER device playing untouched throughout: nothing below is shared
        // with them.
        if (waitResult == WAIT_OBJECT_0 + 2) {
            applyDelayAdjustment(dev);
            continue;
        }
        const size_t fill = dev->fill.load(std::memory_order_relaxed);

        // One call, one immutable result — no separate query needed
        // afterward (see ClockDriftController.h's design note).
        const DriftUpdateResult step =
            dev->controller->update(fill, dev->target_bytes.load(std::memory_order_relaxed),
                                    dev->control_range_bytes);

        dev->ema_error.store(step.ema_error, std::memory_order_relaxed);
        if (step.clamped) logger.event(LogLevel::Trace, Event::RateClampedAtLimit, dev->index);

        if (step.rate_changed) {
            const HRESULT hr = dev->clockAdjuster->SetSampleRate(step.applied_rate_hz);
            if (SUCCEEDED(hr)) {
                dev->sample_rate.store(step.applied_rate_hz, std::memory_order_relaxed);
                const int64_t whole = static_cast<int64_t>(step.applied_rate_hz);
                const int64_t milli = static_cast<int64_t>(
                    std::llround((step.applied_rate_hz - whole) * 1000.0f));
                logger.event(LogLevel::Debug, Event::RateApplied, dev->index, whole, milli);
            } else {
                dev->api_error.fetch_add(1, std::memory_order_relaxed);
                logger.event(LogLevel::Error, Event::RateApplyFailed, dev->index,
                             static_cast<int64_t>(hr));
            }
        }
    }

    logger.event(LogLevel::Debug, Event::ThreadExiting, dev->index);
}

//==============================================================================
// Live delay adjustment (runs on one device's control thread)
//==============================================================================
void AudioEngine::Impl::applyDelayAdjustment(DeviceRuntime* dev) {
    const int64_t newDelayMs = dev->requestedDelayMs.load(std::memory_order_acquire);
    const size_t newTarget = dev->requestedTargetBytes.load(std::memory_order_acquire);
    const size_t oldTarget = dev->target_bytes.load(std::memory_order_relaxed);
    const int64_t oldDelayMs = dev->current_delay_ms.load(std::memory_order_relaxed);

    logger.event(LogLevel::Info, Event::DelayAdjustRequested, dev->index, oldDelayMs, newDelayMs);

    const int64_t deltaBytes =
        static_cast<int64_t>(newTarget) - static_cast<int64_t>(oldTarget);
    const int64_t deltaMs = newDelayMs - oldDelayMs;
    const int64_t smoothLimitMs = std::max<int64_t>(0, activeConfig.smooth_delay_step_ms);

    // Small change: no splice, no mute, no interruption whatsoever. Moving
    // the target is enough — the drift controller sees the new error and
    // walks the buffer there over a few seconds, which is inaudible because
    // that is exactly the mechanism it already uses to correct clock drift.
    if (std::llabs(deltaMs) <= smoothLimitMs) {
        dev->target_bytes.store(newTarget, std::memory_order_relaxed);
        dev->current_delay_ms.store(newDelayMs, std::memory_order_relaxed);
        logger.event(LogLevel::Info, Event::DelayAdjustSmoothed, dev->index, newDelayMs);
        dev->delayAdjustInFlight.store(false, std::memory_order_release);
        return;
    }

    // Large change: mute this device, splice its buffer, unmute. Muting is
    // what makes the discontinuity inaudible; with it in place there is no
    // reason to converge gradually, so the splice is instantaneous rather
    // than spending tens of seconds draining via clock rate.
    const float restoreVolume = dev->volume.load(std::memory_order_relaxed);
    const bool canMute = static_cast<bool>(dev->streamVolume);
    if (canMute) {
        setUniformStreamVolume(dev->streamVolume.Get(), dev->volumeChannelCount, 0.0f);
        // Let the muted level actually reach the endpoint before splicing.
        WaitForSingleObject(shutdownEvent.get(), 30);
    } else {
        logger.event(LogLevel::Warn, Event::DelayAdjustNoMute, dev->index);
    }

    dev->spliceRemaining.store(deltaBytes, std::memory_order_release);

    // Wait for the render thread to carry it out. A shrink completes on its
    // next callback; a grow takes as many callbacks as the inserted silence
    // spans. Bounded so a wedged render thread can't strand this device
    // muted forever.
    constexpr int kMaxWaitMs = 1500;
    constexpr int kPollMs = 5;
    int waited = 0;
    while (dev->spliceRemaining.load(std::memory_order_acquire) != 0 && waited < kMaxWaitMs) {
        if (WaitForSingleObject(shutdownEvent.get(), kPollMs) != WAIT_TIMEOUT) break;
        waited += kPollMs;
    }

    const int64_t outstanding = dev->spliceRemaining.load(std::memory_order_acquire);
    if (outstanding != 0) {
        // Abandon the remainder rather than leaving the render thread owing
        // silence indefinitely, and report the real end state instead of the
        // requested one — telemetry must never claim a delay we didn't reach.
        dev->spliceRemaining.store(0, std::memory_order_release);
        logger.event(LogLevel::Error, Event::DelayAdjustTimeout, dev->index, outstanding);
    }

    const int64_t achievedBytes = deltaBytes - outstanding;
    const size_t achievedTarget =
        static_cast<size_t>(static_cast<int64_t>(oldTarget) + achievedBytes);
    const int64_t achievedDelayMs =
        bytesPerMs > 0.0
            ? oldDelayMs + static_cast<int64_t>(static_cast<double>(achievedBytes) / bytesPerMs)
            : newDelayMs;

    dev->target_bytes.store(achievedTarget, std::memory_order_relaxed);
    dev->current_delay_ms.store(achievedDelayMs, std::memory_order_relaxed);

    if (canMute)
        setUniformStreamVolume(dev->streamVolume.Get(), dev->volumeChannelCount, restoreVolume);

    logger.event(LogLevel::Info, Event::DelayAdjustSpliced, dev->index, achievedDelayMs,
                 achievedBytes);
    dev->delayAdjustInFlight.store(false, std::memory_order_release);
}

//==============================================================================
// Supervisor: start
//==============================================================================
std::string AudioEngine::Impl::startLocked(const EngineConfig& cfg) {
    activeConfig = cfg;
    faultFlag.store(false, std::memory_order_release);
    capturePollTimeouts.store(0, std::memory_order_relaxed);

    if (cfg.outputs.empty()) return "no output devices requested";

    logger.event(LogLevel::Info, Event::EngineStarting, -1);

    shutdownEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!shutdownEvent) return "failed to create shutdown event";

    captureEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!captureEvent) return "failed to create capture event";

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return "failed to create device enumerator";

    // ---- loopback source = current default render endpoint ----
    ComPtr<IMMDevice> captureDevice;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &captureDevice);
    if (FAILED(hr)) return "no default render endpoint available";

    LPWSTR rawCaptureId = nullptr;
    if (FAILED(captureDevice->GetId(&rawCaptureId))) return "failed to read capture endpoint id";
    CoTaskMemString captureId(rawCaptureId);

    hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(captureClient.GetAddressOf()));
    if (FAILED(hr)) return "failed to activate capture client";

    WAVEFORMATEX* rawFormat = nullptr;
    hr = captureClient->GetMixFormat(&rawFormat);
    if (FAILED(hr)) return "failed to query mix format";
    std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> formatGuard(rawFormat);

    const size_t formatBytes = sizeof(WAVEFORMATEX) + rawFormat->cbSize;
    mixFormatBlob.assign(reinterpret_cast<uint8_t*>(rawFormat),
                         reinterpret_cast<uint8_t*>(rawFormat) + formatBytes);
    sampleRate = rawFormat->nSamplesPerSec;
    blockAlign = rawFormat->nBlockAlign;
    if (blockAlign == 0) return "device reported zero block alignment";

    logger.event(LogLevel::Info, Event::MixFormatQueried, -1, sampleRate, blockAlign);

    const DWORD captureFlags =
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, captureFlags, 10000000, 0,
                                   mixFormat(), nullptr);
    if (FAILED(hr)) return "failed to initialize loopback capture client";
    if (FAILED(captureClient->SetEventHandle(captureEvent.get())))
        return "failed to set capture event handle";

    // ---- normalise requested delays ----
    std::vector<int64_t> delays;
    delays.reserve(cfg.outputs.size());
    for (const auto& o : cfg.outputs) {
        if (o.relative_delay_ms < -cfg.max_delay_ms || o.relative_delay_ms > cfg.max_delay_ms)
            return "requested delay out of range";
        delays.push_back(o.relative_delay_ms);
    }
    normalizeDelays(delays);

    bytesPerSecond = static_cast<size_t>(sampleRate) * static_cast<size_t>(blockAlign);
    bytesPerMs = static_cast<double>(bytesPerSecond) / 1000.0;
    cushionBytes = static_cast<size_t>(cfg.cushion_ms * bytesPerMs);
    const size_t controlRangeBytes =
        std::max<size_t>(1, static_cast<size_t>(cfg.control_range_ms * bytesPerMs));

    // ---- build each output ----
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return "failed to enumerate render endpoints";

    UINT total = 0;
    collection->GetCount(&total);

    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        devices.clear();

        for (size_t i = 0; i < cfg.outputs.size(); ++i) {
            const std::wstring& wantId = cfg.outputs[i].endpoint_id;

            if (wantId == captureId.get()) {
                logger.message(LogLevel::Warn, static_cast<int>(i),
                               "output %zu is the loopback source itself; skipped", i);
                continue;
            }

            ComPtr<IMMDevice> device;
            for (UINT j = 0; j < total; ++j) {
                ComPtr<IMMDevice> candidate;
                if (FAILED(collection->Item(j, &candidate))) continue;
                LPWSTR rawId = nullptr;
                if (FAILED(candidate->GetId(&rawId))) continue;
                CoTaskMemString id(rawId);
                if (wantId == id.get()) {
                    device = candidate;
                    break;
                }
            }
            if (!device) {
                logger.message(LogLevel::Warn, static_cast<int>(i),
                               "requested endpoint not found; skipped");
                continue;
            }

            auto dev = std::make_unique<DeviceRuntime>();
            dev->index = static_cast<int>(devices.size());
            dev->endpoint_id = wantId;
            dev->name = deviceFriendlyName(device.Get());
            dev->delay_ms = delays[i];

            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(dev->client.GetAddressOf()));
            if (FAILED(hr)) {
                logger.event(LogLevel::Warn, Event::DeviceActivateFailed, dev->index,
                             static_cast<int64_t>(hr));
                continue;
            }

            const DWORD streamFlags =
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST;

            hr = dev->client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0,
                                         mixFormat(), nullptr);
            if (FAILED(hr)) {
                logger.event(LogLevel::Warn, Event::DeviceInitFailed, dev->index,
                             static_cast<int64_t>(hr));
                continue;
            }

            dev->renderEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (!dev->renderEvent ||
                FAILED(dev->client->SetEventHandle(dev->renderEvent.get()))) {
                logger.event(LogLevel::Warn, Event::DeviceSkipped, dev->index);
                continue;
            }

            // Manual-reset: only ever set once (by reportDeviceFault) and only
            // ever cleared by this DeviceRuntime being destroyed, so there is
            // no reset-race to worry about the way there would be with the
            // auto-reset renderEvent above.
            dev->stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!dev->stopEvent) {
                logger.event(LogLevel::Warn, Event::DeviceSkipped, dev->index);
                continue;
            }

            // Acquired here, on the supervisor thread, and released here when
            // DeviceRuntime is destroyed on this same thread.
            hr = dev->client->GetService(IID_PPV_ARGS(&dev->clockAdjuster));
            if (FAILED(hr)) {
                logger.event(LogLevel::Warn, Event::ClockServiceFailed, dev->index,
                             static_cast<int64_t>(hr));
                continue;
            }

            // Volume control is an enhancement layered on top of already-
            // working audio, not a requirement for it — unlike the clock
            // adjuster above, failure here does not skip the device. Some
            // endpoints (virtual devices in particular) may not support it.
            hr = dev->client->GetService(IID_PPV_ARGS(&dev->streamVolume));
            if (SUCCEEDED(hr)) {
                hr = dev->streamVolume->GetChannelCount(&dev->volumeChannelCount);
                if (SUCCEEDED(hr)) {
                    const float initialVolume = std::clamp(cfg.outputs[i].initial_volume, 0.0f, 1.0f);
                    hr = setUniformStreamVolume(dev->streamVolume.Get(), dev->volumeChannelCount,
                                                initialVolume);
                    if (SUCCEEDED(hr)) {
                        dev->volume.store(initialVolume, std::memory_order_relaxed);
                    } else {
                        logger.event(LogLevel::Warn, Event::VolumeSetFailed, dev->index,
                                     static_cast<int64_t>(hr));
                        dev->streamVolume.Reset();
                    }
                } else {
                    logger.event(LogLevel::Warn, Event::VolumeServiceFailed, dev->index,
                                 static_cast<int64_t>(hr));
                    dev->streamVolume.Reset();
                }
            } else {
                logger.event(LogLevel::Warn, Event::VolumeServiceFailed, dev->index,
                             static_cast<int64_t>(hr));
            }

            try {
                const DeviceBufferSizing sizing = computeBufferSizing(
                    dev->delay_ms, bytesPerMs, blockAlign, cushionBytes, bytesPerSecond);

                dev->ring = std::make_unique<AudioRingBuffer>(sizing.required_capacity_bytes);
                dev->controller =
                    std::make_unique<ClockDriftController>(sampleRate, cfg.drift);

                dev->target_bytes.store(sizing.target_fill_bytes, std::memory_order_relaxed);
                dev->capacity_bytes = dev->ring->get_capacity();
                dev->control_range_bytes = controlRangeBytes;
                dev->current_delay_ms.store(dev->delay_ms, std::memory_order_relaxed);

                // Ceiling for live delay changes. computeBufferSizing sized
                // this ring as target*4, so holding any future target to
                // capacity/4 preserves exactly the same headroom the startup
                // path guarantees — no live change can push the buffer into a
                // regime the original sizing wouldn't have allowed.
                const size_t maxTarget = dev->capacity_bytes / 4;
                dev->max_delay_bytes = (maxTarget > cushionBytes) ? (maxTarget - cushionBytes) : 0;

                dev->delayRequestEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
                if (!dev->delayRequestEvent) {
                    logger.event(LogLevel::Warn, Event::DeviceSkipped, dev->index);
                    continue;
                }

                dev->sample_rate.store(static_cast<float>(sampleRate),
                                       std::memory_order_relaxed);

                if (sizing.padding_bytes > 0) dev->ring->write_silence(sizing.padding_bytes);
                dev->fill.store(dev->ring->get_available_read(), std::memory_order_relaxed);
            } catch (const std::exception& e) {
                logger.message(LogLevel::Warn, dev->index,
                               "buffer allocation failed (%s); device skipped", e.what());
                continue;
            }

            logger.event(LogLevel::Info, Event::DeviceActivated, dev->index);
            devices.push_back(std::move(dev));
        }
    }

    if (devices.empty()) return "no output device could be initialized";

    // ---- launch threads ----
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        for (auto& dev : devices) {
            DeviceRuntime* raw = dev.get();
            raw->renderThread = std::thread([this, raw] { renderLoop(raw); });
            raw->controlThread = std::thread([this, raw] { controlLoop(raw); });
        }
    }
    captureThread = std::thread([this] { captureLoop(); });

    logger.event(LogLevel::Info, Event::EngineRunning, -1,
                 static_cast<int64_t>(devices.size()));
    return {};
}

//==============================================================================
// Supervisor: stop
//==============================================================================
void AudioEngine::Impl::stopLocked() {
    logger.event(LogLevel::Info, Event::EngineStopping, -1);
    signalShutdown();

    if (captureThread.joinable()) captureThread.join();

    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        for (auto& dev : devices) {
            if (dev->renderThread.joinable()) dev->renderThread.join();
            if (dev->controlThread.joinable()) dev->controlThread.join();
        }
        // Destroying DeviceRuntime here releases IAudioClockAdjustment and
        // IAudioClient on the supervisor thread that acquired them.
        devices.clear();
    }

    captureClient.Reset();
    captureEvent.reset();
    shutdownEvent.reset();
    mixFormatBlob.clear();
    sampleRate = 0;
    blockAlign = 0;

    logger.event(LogLevel::Info, Event::EngineStopped, -1);
}

//==============================================================================
// Supervisor loop
//==============================================================================
void AudioEngine::Impl::supervisorLoop() {
    ScopedCom com;
    supervisorAlive.store(true, std::memory_order_release);

    bool quit = false;
    while (!quit) {
        Command cmd;
        bool haveCmd = false;
        {
            std::unique_lock<std::mutex> lock(cmdMutex);
            // 100ms: fast enough that a UI polling stats() every frame sees
            // fresh numbers without perceptible lag, cheap enough (a handful
            // of atomic loads plus one small allocation) that tightening it
            // further wouldn't be worth the wakeups.
            cmdCv.wait_for(lock, std::chrono::milliseconds(100),
                           [this] { return !commands.empty(); });
            if (!commands.empty()) {
                cmd = std::move(commands.front());
                commands.pop_front();
                haveCmd = true;
            }
        }

        // Watchdog: a worker thread hit a fatal condition and signalled.
        if (!haveCmd) {
            if (state.load(std::memory_order_acquire) == EngineState::Running) {
                // Isolate mode: quietly remove any device whose thread has
                // already exited. If that empties the device list, there is
                // nothing left to play, so escalate to the same full-fault
                // path a global signalFault() would have taken.
                if (activeConfig.isolate_device_faults) {
                    const bool hadDevices = !devices.empty();
                    if (pruneFaultedDevices() == 0 && hadDevices) {
                        faultFlag.store(true, std::memory_order_release);
                    }
                }
                if (faultFlag.load(std::memory_order_acquire)) {
                    setState(EngineState::Stopping);
                    stopLocked();
                    logger.event(LogLevel::Fatal, Event::EngineFaulted, -1);
                    setState(EngineState::Faulted);
                }
            }
            publishStats();
            continue;
        }

        switch (cmd.type) {
            case Command::Type::Start: {
                std::string err;
                if (state.load(std::memory_order_acquire) == EngineState::Running) {
                    err = "engine already running";
                } else {
                    setState(EngineState::Starting);
                    err = startLocked(cmd.config);
                    if (err.empty()) {
                        setState(EngineState::Running);
                    } else {
                        stopLocked();
                        setState(EngineState::Idle);
                    }
                }
                publishStats();
                if (cmd.reply) cmd.reply->set_value(err);
                break;
            }
            case Command::Type::Stop: {
                const EngineState s = state.load(std::memory_order_acquire);
                if (s == EngineState::Running || s == EngineState::Faulted) {
                    setState(EngineState::Stopping);
                    stopLocked();
                    setState(EngineState::Idle);
                }
                publishStats();
                if (cmd.reply) cmd.reply->set_value(std::string{});
                break;
            }
            case Command::Type::Quit: {
                const EngineState s = state.load(std::memory_order_acquire);
                if (s == EngineState::Running || s == EngineState::Faulted) {
                    setState(EngineState::Stopping);
                    stopLocked();
                    setState(EngineState::Idle);
                }
                if (cmd.reply) cmd.reply->set_value(std::string{});
                quit = true;
                break;
            }
        }
    }

    supervisorAlive.store(false, std::memory_order_release);
}

//==============================================================================
// Public API
//==============================================================================
AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {
    impl_->logger.start();
    impl_->supervisor = std::thread([this] { impl_->supervisorLoop(); });
}

AudioEngine::~AudioEngine() {
    if (impl_->supervisor.joinable()) {
        auto reply = std::make_shared<std::promise<std::string>>();
        auto fut = reply->get_future();
        {
            std::lock_guard<std::mutex> lock(impl_->cmdMutex);
            Command c;
            c.type = Command::Type::Quit;
            c.reply = reply;
            impl_->commands.push_back(std::move(c));
        }
        impl_->cmdCv.notify_one();
        fut.wait();
        impl_->supervisor.join();
    }
    impl_->logger.stop();
}

void AudioEngine::setLogSink(LogSink sink) { impl_->logger.setSink(std::move(sink)); }
void AudioEngine::setLogLevel(LogLevel level) { impl_->logger.setMinLevel(level); }

void AudioEngine::setStateCallback(std::function<void(EngineState)> cb) {
    impl_->stateCallback = std::move(cb);
}

bool AudioEngine::enumerateOutputs(std::vector<EndpointInfo>& out, std::string& error) {
    out.clear();
    error.clear();

    ScopedCom com;
    if (!com.usable()) {
        error = "COM initialization failed";
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        error = "failed to create device enumerator";
        return false;
    }

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&raw))) {
            CoTaskMemString id(raw);
            defaultId = id.get();
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        error = "failed to enumerate render endpoints";
        return false;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        LPWSTR raw = nullptr;
        if (FAILED(device->GetId(&raw))) continue;
        CoTaskMemString id(raw);

        EndpointInfo info;
        info.id = id.get();
        info.name = deviceFriendlyName(device.Get());
        info.is_default_render = (!defaultId.empty() && info.id == defaultId);
        out.push_back(std::move(info));
    }
    return true;
}

bool AudioEngine::start(const EngineConfig& config, std::string& error) {
    auto reply = std::make_shared<std::promise<std::string>>();
    auto fut = reply->get_future();
    {
        std::lock_guard<std::mutex> lock(impl_->cmdMutex);
        Command c;
        c.type = Command::Type::Start;
        c.config = config;
        c.reply = reply;
        impl_->commands.push_back(std::move(c));
    }
    impl_->cmdCv.notify_one();

    error = fut.get();
    return error.empty();
}

void AudioEngine::requestStop() {
    {
        std::lock_guard<std::mutex> lock(impl_->cmdMutex);
        Command c;
        c.type = Command::Type::Stop;
        impl_->commands.push_back(std::move(c));
    }
    impl_->cmdCv.notify_one();
}

void AudioEngine::stop() {
    auto reply = std::make_shared<std::promise<std::string>>();
    auto fut = reply->get_future();
    {
        std::lock_guard<std::mutex> lock(impl_->cmdMutex);
        Command c;
        c.type = Command::Type::Stop;
        c.reply = reply;
        impl_->commands.push_back(std::move(c));
    }
    impl_->cmdCv.notify_one();
    fut.wait();
}

EngineState AudioEngine::state() const {
    return impl_->state.load(std::memory_order_acquire);
}

bool AudioEngine::isRunning() const { return state() == EngineState::Running; }

bool AudioEngine::setDeviceVolume(int deviceIndex, float linearVolume) {
    linearVolume = std::clamp(linearVolume, 0.0f, 1.0f);

    // Deliberately NOT routed through the command queue: that channel exists
    // to serialize structural changes (start/stop) that must not interleave.
    // A volume change touches nothing structural — it's a direct call into a
    // COM object WASAPI already guarantees is safe to adjust while the
    // stream runs (that's the entire point of IAudioStreamVolume) — so
    // queuing it would only add latency a slider drag would feel immediately.
    //
    // devicesMutex is used here, unlike in stats(), because this is a
    // discrete user action (at most tens of calls/second while dragging),
    // not a per-frame poll: an uncontended lock costs tens of nanoseconds,
    // which is not a responsiveness concern at that call rate. stats() is
    // lock-free because it MUST tolerate being called every render frame
    // without risk of blocking on a restart in progress — two different
    // access patterns, deliberately two different coordination strategies.
    std::lock_guard<std::mutex> lock(impl_->devicesMutex);
    if (deviceIndex < 0 || static_cast<size_t>(deviceIndex) >= impl_->devices.size())
        return false;

    DeviceRuntime* dev = impl_->devices[static_cast<size_t>(deviceIndex)].get();
    if (!dev->streamVolume) return false;  // this endpoint didn't support volume control at startup

    const HRESULT hr = setUniformStreamVolume(dev->streamVolume.Get(), dev->volumeChannelCount,
                                              linearVolume);
    if (FAILED(hr)) {
        impl_->logger.event(LogLevel::Warn, Event::VolumeSetFailed, dev->index,
                            static_cast<int64_t>(hr));
        return false;
    }
    dev->volume.store(linearVolume, std::memory_order_relaxed);
    return true;
}

bool AudioEngine::setDeviceDelay(int deviceIndex, int64_t delayMs, std::string& error) {
    error.clear();

    // All validation is synchronous and happens before anything is queued, so
    // a rejected request changes no state at all and the caller always learns
    // why. Nothing here can fail silently later.
    if (state() != EngineState::Running) {
        error = "engine is not running";
        return false;
    }
    if (delayMs < 0) {
        error = "delay must be zero or positive";
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->devicesMutex);

    if (deviceIndex < 0 || static_cast<size_t>(deviceIndex) >= impl_->devices.size()) {
        error = "device index out of range";
        return false;
    }
    if (delayMs > impl_->activeConfig.max_delay_ms) {
        error = "delay exceeds the configured maximum of " +
                std::to_string(impl_->activeConfig.max_delay_ms) + " ms";
        return false;
    }
    if (impl_->bytesPerMs <= 0.0) {
        error = "session audio format is not initialized";
        return false;
    }

    DeviceRuntime* dev = impl_->devices[static_cast<size_t>(deviceIndex)].get();

    if (dev->faulted.load(std::memory_order_acquire)) {
        error = "device has faulted";
        return false;
    }

    // The ring buffer is sized once at start() and never reallocated while
    // threads are running, so a live delay change has a hard ceiling. Report
    // it rather than accepting a request that would overrun the buffer.
    const int64_t maxDelayForDevice =
        static_cast<int64_t>(static_cast<double>(dev->max_delay_bytes) / impl_->bytesPerMs);
    if (delayMs > maxDelayForDevice) {
        error = "delay exceeds what this session's buffer can hold (max " +
                std::to_string(maxDelayForDevice) +
                " ms for this device); restart with a larger delay to size a bigger buffer";
        return false;
    }

    // One adjustment per device at a time. compare_exchange makes the guard
    // itself the mutual exclusion, so two concurrent callers can't both pass.
    bool expected = false;
    if (!dev->delayAdjustInFlight.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
        error = "a delay adjustment is already in progress for this device";
        return false;
    }

    size_t newTarget = 0;
    try {
        const DeviceBufferSizing sizing =
            computeBufferSizing(delayMs, impl_->bytesPerMs, impl_->blockAlign,
                                impl_->cushionBytes, impl_->bytesPerSecond);
        newTarget = sizing.target_fill_bytes;
    } catch (const std::exception& e) {
        dev->delayAdjustInFlight.store(false, std::memory_order_release);
        error = std::string("failed to compute buffer sizing: ") + e.what();
        return false;
    }

    dev->requestedDelayMs.store(delayMs, std::memory_order_release);
    dev->requestedTargetBytes.store(newTarget, std::memory_order_release);
    SetEvent(dev->delayRequestEvent.get());
    return true;
}

EngineStats AudioEngine::stats() const {
    // Lock-free read: see Impl::publishStats() for why this never takes
    // devicesMutex. atomic_load pairs with the supervisor's atomic_store.
    return *std::atomic_load(&impl_->publishedStats);
}

}  // namespace mux
