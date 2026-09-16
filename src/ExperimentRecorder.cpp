#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ExperimentRecorder.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace mux {

std::string ExperimentRecorder::narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::string ExperimentRecorder::sanitizeFileNamePart(const std::wstring& name) {
    std::string s = narrow(name);
    if (s.empty()) s = "experiment";
    static const std::string invalid = "\\/:*?\"<>| ";
    for (char& c : s)
        if (invalid.find(c) != std::string::npos) c = '_';
    return s;
}

int64_t ExperimentRecorder::msSince(std::chrono::steady_clock::time_point t) const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t)
        .count();
}

bool ExperimentRecorder::start(const std::wstring& name, std::string& error) {
    finalize();  // guard against a stray double-start

    if (!CreateDirectoryA(kOutputDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "failed to create experiments directory";
        return false;
    }

    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    std::ostringstream stamp;
    stamp << std::put_time(&tmv, "%Y%m%d-%H%M%S");

    const std::string path = std::string(kOutputDir) + "\\" + sanitizeFileNamePart(name) + "_" +
                             stamp.str() + ".log";

    file_.open(path, std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        error = "failed to open experiment log file: " + path;
        return false;
    }

    experimentStart_ = std::chrono::steady_clock::now();
    stepIndex_ = 0;
    stepOpen_ = false;

    file_ << "EXPERIMENT: " << narrow(name) << "\n";
    file_ << "STARTED: " << stamp.str() << "\n";
    file_.flush();
    return true;
}

void ExperimentRecorder::beginStep(const EngineConfig& cfg,
                                   const std::vector<std::wstring>& deviceNames) {
    if (!file_.is_open()) return;
    endStep();

    ++stepIndex_;
    stepStart_ = std::chrono::steady_clock::now();
    stepOpen_ = true;

    file_ << "\n=== STEP " << stepIndex_ << " @ " << msSince(experimentStart_) << "ms ===\n";
    file_ << "CONFIG:\n";
    file_ << "  cushion_ms=" << cfg.cushion_ms << " control_range_ms=" << cfg.control_range_ms
          << " control_period_ms=" << cfg.control_period_ms
          << " capture_poll_ms=" << cfg.capture_poll_ms
          << " starvation_reset_ms=" << cfg.starvation_reset_ms << "\n";
    file_ << "  drift: deadband=" << cfg.drift.deadband << " ema_alpha=" << cfg.drift.ema_alpha
          << " max_drift_ratio=" << cfg.drift.max_drift_ratio
          << " rate_hysteresis_hz=" << cfg.drift.rate_hysteresis_hz << "\n";
    file_ << "  outputs:\n";
    for (size_t i = 0; i < cfg.outputs.size(); ++i) {
        const std::string dn = i < deviceNames.size() ? narrow(deviceNames[i]) : "(unknown)";
        file_ << "    - " << dn << "  delay_ms=" << cfg.outputs[i].relative_delay_ms << "\n";
    }
    file_ << "TELEMETRY:\n";
    file_ << "t_ms,device,fill,target,clock_hz,ema,overflow,partial,drops,api\n";
    file_.flush();
}

void ExperimentRecorder::recordSample(const EngineStats& stats) {
    if (!file_.is_open() || !stepOpen_) return;

    const int64_t t = msSince(stepStart_);
    for (const auto& d : stats.devices) {
        file_ << t << "," << narrow(d.name) << "," << d.fill_bytes << "," << d.target_bytes
              << "," << d.sample_rate_hz << "," << d.ema_error << "," << d.overflow_count << ","
              << d.partial_underflow_count << "," << d.catastrophic_underflow_count << ","
              << d.api_error_count << "\n";
    }
    file_.flush();
}

void ExperimentRecorder::endStep() {
    if (!file_.is_open() || !stepOpen_) return;
    file_ << "=== STEP " << stepIndex_ << " END @ " << msSince(experimentStart_) << "ms ===\n";
    file_.flush();
    stepOpen_ = false;
}

void ExperimentRecorder::finalize() {
    if (!file_.is_open()) return;
    endStep();
    file_ << "\nEXPERIMENT END @ " << msSince(experimentStart_) << "ms\n";
    file_.close();
}

}  // namespace mux
