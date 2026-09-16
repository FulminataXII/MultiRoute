#pragma once
//==============================================================================
// AudioEngine.h - Public API for the multi-output audio router core.
//
// This header intentionally exposes NO Windows or COM types. A CLI, a Win32
// GUI, a Qt GUI, or a test harness can all include it without inheriting
// windows.h. All platform detail lives behind the Impl pointer in the .cpp.
//
// Threading contract:
//   * All public methods are safe to call from any single frontend thread.
//   * stats() is safe to call concurrently with a running engine.
//   * Logs arrive asynchronously on the logger's drain thread, never on the
//     caller's thread and never on an audio thread.
//   * COM object lifetime is confined to one internal supervisor thread, so
//     WASAPI's same-thread acquire/release affinity holds regardless of which
//     frontend thread calls start()/stop().
//==============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ClockDriftController.h"
#include "Logging.h"

namespace mux {

//------------------------------------------------------------------------------
// Discovery
//------------------------------------------------------------------------------
struct EndpointInfo {
    std::wstring id;
    std::wstring name;
    bool is_default_render = false;  // this one is the loopback source
};

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------
struct OutputRequest {
    std::wstring endpoint_id;
    // Additional software delay for this output, relative to the others.
    // The minimum across all outputs is normalised to zero at start().
    // NOTE: this is a relative software FIFO offset, not absolute end-to-end
    // device latency; engine buffering adds a common baseline on top.
    int64_t relative_delay_ms = 0;

    // Starting volume for this output, 0.0 (silent) to 1.0 (full). Applied
    // via WASAPI's native per-stream volume control (IAudioStreamVolume) at
    // startup; adjustable afterward without a restart via
    // AudioEngine::setDeviceVolume() — see that method's comment for why.
    float initial_volume = 1.0f;
};

struct EngineConfig {
    std::vector<OutputRequest> outputs;

    double   cushion_ms          = 50.0;   // baseline FIFO depth before playback starts
    double   control_range_ms    = 50.0;   // normalisation range for the drift controller
    uint32_t control_period_ms   = 250;    // drift controller tick
    uint32_t capture_poll_ms     = 20;     // bounded capture wait (silence self-heal)
    uint32_t starvation_reset_ms = 500;    // continuous starvation before re-prebuffering
    int64_t  max_delay_ms        = 10000;  // rejects absurd user input early

    // A live delay change smaller than this is applied by simply moving the
    // controller's target: the drift controller absorbs it over a few
    // seconds with no interruption at all. Anything larger is applied by
    // muting that one device, splicing its buffer, and unmuting — sub-second
    // but briefly silent. Raising this trades "silent gap" for "slow, and
    // slightly pitch-shifted while converging".
    int64_t  smooth_delay_step_ms = 10;

    // When false (default): any output's fatal WASAPI error stops the whole
    // engine and reports EngineState::Faulted (loud-by-default, easiest to
    // debug — this is the tested behavior). When true: that one output is
    // marked faulted and dropped, the rest keep playing, engine stays
    // Running. A shared capture-source failure is always engine-fatal
    // regardless of this flag, since nothing routes without it. See
    // AudioEngine.cpp reportDeviceFault() for the single choke point this
    // controls — a GUI settings toggle needs no other engine change.
    bool isolate_device_faults = false;

    DriftControlParams drift;
};

//------------------------------------------------------------------------------
// Observability
//------------------------------------------------------------------------------
struct DeviceStats {
    std::wstring name;
    std::wstring endpoint_id;
    size_t   fill_bytes = 0;
    size_t   target_bytes = 0;
    size_t   capacity_bytes = 0;
    int64_t  configured_delay_ms = 0;
    int64_t  max_delay_ms_for_device = 0;  // largest delay this session's ring buffer can hold; see setDeviceDelay
    bool     delay_adjust_in_flight = false;
    float    sample_rate_hz = 0.0f;
    float    ema_error = 0.0f;
    float    volume = 1.0f;
    bool     prebuffering = true;
    bool     running = false;
    bool     faulted = false;
    uint64_t overflow_count = 0;
    uint64_t partial_underflow_count = 0;
    uint64_t catastrophic_underflow_count = 0;
    uint64_t api_error_count = 0;
};

struct EngineStats {
    bool running = false;
    uint32_t mix_sample_rate = 0;
    uint16_t mix_block_align = 0;
    uint64_t capture_poll_timeouts = 0;
    uint64_t log_records_dropped = 0;
    std::vector<DeviceStats> devices;
};

enum class EngineState { Idle, Starting, Running, Stopping, Faulted };

const char* engineStateName(EngineState s);

//------------------------------------------------------------------------------
// Engine
//------------------------------------------------------------------------------
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Attach a log sink before start(). Invoked on the drain thread.
    void setLogSink(LogSink sink);
    void setLogLevel(LogLevel level);

    // Invoked on the supervisor thread whenever the engine state changes.
    void setStateCallback(std::function<void(EngineState)> cb);

    // Enumerate active render endpoints and identify the loopback source.
    // Does not require the engine to be running.
    static bool enumerateOutputs(std::vector<EndpointInfo>& out, std::string& error);

    // Blocks until the engine is running or has failed. On failure `error`
    // explains why and the engine returns to Idle.
    bool start(const EngineConfig& config, std::string& error);

    // Asynchronous: requests shutdown and returns immediately.
    void requestStop();

    // Blocks until fully stopped and all threads joined.
    void stop();

    EngineState state() const;
    bool isRunning() const;

    // Live volume control, no restart required. deviceIndex is the output's
    // position in the current session's device list (same ordering as
    // EngineStats::devices — index i here matches stats().devices[i]), not a
    // value that survives a restart. Uses WASAPI's IAudioStreamVolume, which
    // applies the gain natively inside the audio engine's own mixing stage —
    // this never touches the ring buffer, the render thread, or buffer
    // sizing, so it cannot introduce a glitch or shift latency the way
    // changing a structural parameter (cushion, delay) would. That's also
    // why, unlike cushion/deadband/delay, this does NOT go through
    // start()/stop(): restarting to change a volume would itself cause the
    // audible interruption this method exists to avoid.
    // Returns false (and logs a warning) if the index is out of range or the
    // device didn't support volume control at startup; the device keeps
    // playing normally either way. linearVolume is clamped to [0, 1].
    bool setDeviceVolume(int deviceIndex, float linearVolume);

    // Live per-device delay change, no restart. deviceIndex follows the same
    // convention as setDeviceVolume (position in stats().devices).
    //
    // Validation is synchronous and strict — this returns false with a
    // specific reason in `error` for: engine not running, index out of
    // range, negative delay, delay above EngineConfig::max_delay_ms, delay
    // beyond what this session's ring buffer can hold (the buffer was sized
    // at start() and is not reallocated live — DeviceStats reports the
    // per-device ceiling as max_delay_ms_for_device), or an adjustment
    // already in flight for that device. It never accepts a request it
    // cannot carry out.
    //
    // On acceptance the work happens asynchronously on that ONE device's
    // control thread; every other output keeps playing without interruption,
    // since each device owns its own ring buffer, render thread and control
    // thread. Two application paths, picked by size (see
    // EngineConfig::smooth_delay_step_ms):
    //   small change -> retarget only; the drift controller converges over a
    //                   few seconds with no audible interruption at all.
    //   large change -> that device is muted, its buffer spliced, then
    //                   unmuted. Sub-second, but briefly silent, because
    //                   buffered audio cannot be un-played and draining it
    //                   via clock rate alone would take tens of seconds.
    // Returns true once the request is accepted and queued, not once it has
    // finished; poll DeviceStats::delay_adjust_in_flight to observe it land.
    bool setDeviceDelay(int deviceIndex, int64_t delayMs, std::string& error);

    // Lock-free: reads an immutable snapshot published by the supervisor
    // thread (see AudioEngine.cpp), so this never blocks on whatever the
    // supervisor is doing (including a slow device teardown/restart). Safe
    // to call every frame from a UI without risking a stall.
    EngineStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mux
