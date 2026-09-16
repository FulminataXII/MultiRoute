#pragma once
//==============================================================================
// ExperimentRecorder.h - Records a sequence of (config, telemetry-over-time)
// steps to a plain-text log file. Deliberately has no dependency on the GUI
// toolkit or on AudioEngine internals beyond the public types in
// AudioEngine.h, so it can be unit-tested or reused by a future CLI
// "experiment" mode without pulling in ImGui.
//
// File format is intentionally simple (not JSON): a human can read it
// directly, and the TELEMETRY block of each step is CSV, so it drops straight
// into a spreadsheet. See writeStepHeader()/writeSample() for the exact
// layout.
//==============================================================================

#include "AudioEngine.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace mux {

class ExperimentRecorder {
public:
    // Fixed, non-configurable output directory, per requirement: always
    // "experiments" relative to the process's current working directory.
    static constexpr const char* kOutputDir = "experiments";

    ~ExperimentRecorder() { finalize(); }

    bool active() const { return file_.is_open(); }

    // Opens "experiments/<sanitized name>_<timestamp>.log" and writes a
    // header. Fails (returns false, sets error) if the directory can't be
    // created or the file can't be opened - both surfaced to the GUI rather
    // than silently losing experiment data.
    bool start(const std::wstring& name, std::string& error);

    // Call once per configuration change (initial start, edit, or a new
    // experiment step). Flushes any in-progress step first.
    void beginStep(const EngineConfig& cfg, const std::vector<std::wstring>& deviceNames);

    // Call periodically (a few times a second is plenty) while a step is
    // live. No-op if no step is open.
    void recordSample(const EngineStats& stats);

    // Closes the current step's section without starting a new one.
    void endStep();

    // Writes the closing footer and closes the file. Safe to call multiple
    // times; idempotent after the first call.
    void finalize();

private:
    std::ofstream file_;
    std::chrono::steady_clock::time_point experimentStart_;
    std::chrono::steady_clock::time_point stepStart_;
    int stepIndex_ = 0;
    bool stepOpen_ = false;

    static std::string sanitizeFileNamePart(const std::wstring& name);
    static std::string narrow(const std::wstring& s);
    int64_t msSince(std::chrono::steady_clock::time_point t) const;
};

}  // namespace mux
