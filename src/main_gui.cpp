//==============================================================================
// main_gui.cpp - Dear ImGui (v1.92.x) frontend over mux::AudioEngine.
//
// Platform harness (CreateDeviceD3D/WndProc/main loop) is adapted directly
// from imgui/examples/example_win32_directx11/main.cpp — see
// https://github.com/ocornut/imgui/wiki/Getting-Started for the canonical
// version. Everything below the "Application" marker is this project's own.
//
// Threading: every AudioEngine call here (start/stop/stats/enumerateOutputs)
// is already safe to call from an arbitrary frontend thread per AudioEngine.h
// contract. stats() takes a brief internal lock but never blocks on the
// audio hot path (see AudioEngine.cpp). Engine restarts run on a background
// thread so a slow stop()/start() never freezes the UI thread; navigation
// buttons are disabled while a restart is in flight specifically to avoid a
// second restart being queued against a config that hasn't landed yet.
//==============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>

#include "AudioEngine.h"
#include "ExperimentRecorder.h"
#include "HelpText.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using mux::AudioEngine;
using mux::DriftControlParams;
using mux::EndpointInfo;
using mux::EngineConfig;
using mux::EngineState;
using mux::LogLevel;
using mux::LogMessage;
using mux::OutputRequest;

//==============================================================================
// Platform harness (Win32 + DX11) — unmodified pattern from the ImGui example
//==============================================================================
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

//==============================================================================
// Application
//==============================================================================
namespace {

// Proper UTF-8 conversion for display, rather than truncating each wchar_t
// to a char via an iterator-range std::string constructor — that silently
// mangles any non-ASCII character in a device name instead of encoding it.
std::string narrowUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

enum class Screen { DeviceSetup, Live };

struct DeviceRow {
    EndpointInfo info;
    bool selected = false;
    int delayMs = 0;
    float volume = 1.0f;  // starting volume, applied at Confirm; live-adjustable afterward on the Live screen
};

// Plain-old-data mirror of the tunables in EngineConfig, bound directly to
// ImGui widgets. Kept separate from EngineConfig itself so the widgets never
// touch a config that might be mid-use by a running engine.
struct TunableStaging {
    double cushion_ms = 50.0;
    double control_range_ms = 50.0;
    int control_period_ms = 250;
    int capture_poll_ms = 20;
    int starvation_reset_ms = 500;
    int max_delay_ms = 10000;
    float deadband = 0.05f;
    float ema_alpha = 0.05f;
    float max_drift_ratio = 0.0025f;
    float rate_hysteresis_hz = 0.5f;
    bool isolate_device_faults = false;
};

struct GuiState {
    Screen screen = Screen::DeviceSetup;
    std::vector<DeviceRow> deviceRows;
    std::string enumerateError;

    bool debugEnabled = false;
    bool showDebugConfirmPopup = false;
    TunableStaging tunables;

    bool showLogs = false;
    bool showHelp = false;
    std::string liveDelayError;  // last rejection from setDeviceDelay, shown under the live table
    int logLevelIndex = 1;  // index into kLogLevels below; default Debug

    // Experiment mode
    bool experimentActive = false;
    bool experimentSetupPending = false;  // DeviceSetup screen is configuring a step
    bool showExperimentNamePopup = false;
    char experimentNameBuf[128] = {};
    bool debugStateBeforeExperiment = false;
    mux::ExperimentRecorder recorder;
    std::chrono::steady_clock::time_point lastSampleTime;

    // Async restart plumbing
    std::thread restartThread;
    std::atomic<bool> restarting{false};
    std::mutex restartResultMutex;
    bool haveRestartResult = false;
    std::string restartError;
    EngineConfig pendingConfigForStep;  // snapshot used to open the experiment step once restart succeeds
    std::vector<std::wstring> pendingDeviceNamesForStep;

    // Log ring buffer, fed by the engine's log sink (runs on its drain
    // thread — never an audio thread, never this GUI thread).
    std::mutex logMutex;
    std::deque<LogMessage> logLines;
    static constexpr size_t kMaxLogLines = 500;

    // Names by device index, refreshed each time stats() is polled, so log
    // lines can show a name instead of a bare index.
    std::vector<std::wstring> deviceNamesByIndex;
};

constexpr LogLevel kLogLevels[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                                   LogLevel::Warn,  LogLevel::Error, LogLevel::Fatal};
constexpr const char* kLogLevelNames[] = {"Trace", "Debug", "Info", "Warn", "Error", "Fatal"};

void refreshDeviceList(GuiState& gui) {
    std::vector<EndpointInfo> endpoints;
    std::string error;
    if (!AudioEngine::enumerateOutputs(endpoints, error)) {
        gui.enumerateError = error;
        gui.deviceRows.clear();
        return;
    }
    gui.enumerateError.clear();

    // Fresh enumeration every call (never cached), but preserve the user's
    // in-progress selection/delay for endpoints that are still present.
    std::vector<DeviceRow> rows;
    rows.reserve(endpoints.size());
    for (auto& ep : endpoints) {
        DeviceRow row;
        row.info = ep;
        for (const auto& old : gui.deviceRows) {
            if (old.info.id == ep.id) {
                row.selected = old.selected;
                row.delayMs = old.delayMs;
                row.volume = old.volume;
                break;
            }
        }
        rows.push_back(std::move(row));
    }
    gui.deviceRows = std::move(rows);
}

EngineConfig buildConfig(const GuiState& gui) {
    EngineConfig cfg;
    for (const auto& row : gui.deviceRows) {
        if (!row.selected || row.info.is_default_render) continue;
        OutputRequest req;
        req.endpoint_id = row.info.id;
        req.relative_delay_ms = row.delayMs;
        req.initial_volume = row.volume;
        cfg.outputs.push_back(std::move(req));
    }

    if (gui.debugEnabled) {
        const TunableStaging& t = gui.tunables;
        cfg.cushion_ms = t.cushion_ms;
        cfg.control_range_ms = t.control_range_ms;
        cfg.control_period_ms = static_cast<uint32_t>(std::max(1, t.control_period_ms));
        cfg.capture_poll_ms = static_cast<uint32_t>(std::max(1, t.capture_poll_ms));
        cfg.starvation_reset_ms = static_cast<uint32_t>(std::max(0, t.starvation_reset_ms));
        cfg.max_delay_ms = std::max<int64_t>(0, t.max_delay_ms);
        cfg.drift.deadband = t.deadband;
        cfg.drift.ema_alpha = t.ema_alpha;
        cfg.drift.max_drift_ratio = t.max_drift_ratio;
        cfg.drift.rate_hysteresis_hz = t.rate_hysteresis_hz;
        cfg.isolate_device_faults = t.isolate_device_faults;
    }
    // else: EngineConfig's own defaults apply, matching a non-debug session.
    return cfg;
}

// Pulls the engine's CURRENT live state back into the GUI's staging rows.
//
// deviceRows holds what the user set up at Confirm time; delays and volumes
// tuned afterward on the Live screen go straight to the engine and never
// touch these rows. Any path that returns to the setup screen, or rebuilds a
// config from the rows, must therefore re-sync first — otherwise it silently
// reapplies the stale setup-time values and throws away everything the user
// tuned live, which is exactly the surprise this function exists to prevent.
void syncRowsFromEngine(GuiState& gui, const AudioEngine& engine) {
    if (!engine.isRunning()) return;
    const mux::EngineStats stats = engine.stats();

    for (auto& row : gui.deviceRows) {
        const auto it = std::find_if(
            stats.devices.begin(), stats.devices.end(),
            [&row](const mux::DeviceStats& d) { return d.endpoint_id == row.info.id; });
        if (it == stats.devices.end()) {
            // Not currently playing: either never selected, or dropped after
            // a fault. Either way it is not part of the live set.
            row.selected = false;
            continue;
        }
        row.selected = true;
        row.delayMs = static_cast<int>(it->configured_delay_ms);
        row.volume = it->volume;
    }
}

std::vector<std::wstring> selectedDeviceNames(const GuiState& gui) {
    std::vector<std::wstring> names;
    for (const auto& row : gui.deviceRows)
        if (row.selected && !row.info.is_default_render) names.push_back(row.info.name);
    return names;
}

// Restart is stop()+start() on a background thread so the UI thread never
// blocks on WASAPI teardown/setup. Callers must not allow a second restart
// to be queued while gui.restarting is true — every button that can trigger
// one is disabled for exactly that reason (see renderLiveScreen).
void startAsyncRestart(GuiState& gui, AudioEngine& engine, EngineConfig cfg) {
    if (gui.restartThread.joinable()) gui.restartThread.join();
    gui.restarting.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(gui.restartResultMutex);
        gui.haveRestartResult = false;
    }
    gui.restartThread = std::thread([&gui, &engine, cfg]() {
        engine.stop();
        std::string err;
        engine.start(cfg, err);
        std::lock_guard<std::mutex> lock(gui.restartResultMutex);
        gui.restartError = err;
        gui.haveRestartResult = true;
        gui.restarting.store(false, std::memory_order_release);
    });
}

void renderDebugConfirmPopup(GuiState& gui) {
    if (gui.showDebugConfirmPopup) ImGui::OpenPopup("Enable Debug Parameters?");
    if (ImGui::BeginPopupModal("Enable Debug Parameters?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("This exposes low-level tuning parameters.");
        ImGui::Text("Incorrect values can cause audio glitches or crashes.");
        ImGui::Separator();
        if (ImGui::Button("Yes, show them", ImVec2(150, 0))) {
            gui.debugEnabled = true;
            gui.showDebugConfirmPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(150, 0))) {
            gui.showDebugConfirmPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void renderExperimentNamePopup(GuiState& gui, const AudioEngine& engine) {
    if (gui.showExperimentNamePopup) ImGui::OpenPopup("Start Experiment");
    if (ImGui::BeginPopupModal("Start Experiment", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Experiment name:");
        ImGui::InputText("##expname", gui.experimentNameBuf, sizeof(gui.experimentNameBuf));
        ImGui::TextDisabled("Saved to experiments/<name>_<timestamp>.log");
        ImGui::TextDisabled("Debug parameters will be shown for the duration of the experiment.");
        ImGui::Separator();
        const bool haveName = gui.experimentNameBuf[0] != '\0';
        ImGui::BeginDisabled(!haveName);
        if (ImGui::Button("Start", ImVec2(120, 0))) {
            const std::string narrow(gui.experimentNameBuf);
            const std::wstring wname(narrow.begin(), narrow.end());
            std::string err;
            if (gui.recorder.start(wname, err)) {
                gui.experimentActive = true;
                gui.experimentSetupPending = true;
                // Debug params must be visible/tunable for the whole
                // experiment; remember whatever the user had before so it
                // can be restored exactly when the experiment ends.
                gui.debugStateBeforeExperiment = gui.debugEnabled;
                gui.debugEnabled = true;
                // Adopt whatever is playing right now as the experiment's
                // starting point — a user who tuned delays live and then
                // started an experiment meant to record THAT state, not the
                // values they happened to type at setup time.
                refreshDeviceList(gui);
                syncRowsFromEngine(gui, engine);
                gui.screen = Screen::DeviceSetup;
            } else {
                gui.enumerateError = err;  // reuse the same error line, simplest surface for this
            }
            gui.showExperimentNamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            gui.showExperimentNamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void renderTunables(TunableStaging& t) {
    ImGui::SeparatorText("Tunable Parameters");
    ImGui::InputDouble("Cushion (ms)", &t.cushion_ms, 5.0, 20.0, "%.1f");
    ImGui::InputDouble("Control range (ms)", &t.control_range_ms, 5.0, 20.0, "%.1f");
    ImGui::SliderInt("Control period (ms)", &t.control_period_ms, 20, 1000);
    ImGui::SliderInt("Capture poll (ms)", &t.capture_poll_ms, 5, 200);
    ImGui::SliderInt("Starvation reset (ms)", &t.starvation_reset_ms, 100, 5000);
    ImGui::InputInt("Max delay (ms)", &t.max_delay_ms, 100, 1000);
    ImGui::SliderFloat("Deadband", &t.deadband, 0.0f, 0.5f, "%.3f");
    ImGui::SliderFloat("EMA alpha", &t.ema_alpha, 0.005f, 0.5f, "%.3f");
    ImGui::SliderFloat("Max drift ratio", &t.max_drift_ratio, 0.0001f, 0.02f, "%.4f");
    ImGui::SliderFloat("Rate hysteresis (Hz)", &t.rate_hysteresis_hz, 0.05f, 5.0f, "%.2f");
    ImGui::Checkbox("Isolate device faults (keep other outputs alive)",
                    &t.isolate_device_faults);
    ImGui::TextDisabled("Off: one device fault stops the whole engine (default, tested).");
    ImGui::TextDisabled("On: the faulted device is dropped; the rest keep playing.");
}

void renderDeviceTable(GuiState& gui) {
    if (ImGui::BeginTable("devices", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Play", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Delay (ms)", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Starting Volume", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < gui.deviceRows.size(); ++i) {
            DeviceRow& row = gui.deviceRows[i];
            const bool isSource = row.info.is_default_render;
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(isSource);
            ImGui::Checkbox("##sel", &row.selected);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);
            std::string label = narrowUtf8(row.info.name);
            if (isSource) label += "  (loopback source - see Help)";
            ImGui::TextUnformatted(label.c_str());
            if (isSource && ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "This is the device you're capturing audio FROM. It already "
                    "plays that audio natively and can't also be a target.");

            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(!row.selected || isSource);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##delay", &row.delayMs, 10, 100);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(3);
            ImGui::BeginDisabled(!row.selected || isSource);
            ImGui::SetNextItemWidth(-1);
            float volPercent = row.volume * 100.0f;
            if (ImGui::SliderFloat("##vol", &volPercent, 0.0f, 100.0f, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                row.volume = volPercent / 100.0f;
            }
            if (isSource && ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "This is your system's default output - its volume is controlled "
                    "by your PC/laptop's normal volume buttons or slider, not from here.");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (gui.deviceRows.size() == 1 && gui.deviceRows[0].info.is_default_render) {
        ImGui::TextColored(
            ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
            "Only one audio device exists on this system, and it's the loopback "
            "source, so there's nothing to route audio to.");
        ImGui::TextDisabled(
            "A second physical output, or a virtual device like VB-Cable set as "
            "default, is needed for this app to do anything.");
    }
}

void renderDeviceSetupScreen(GuiState& gui, AudioEngine& engine) {
    if (gui.experimentSetupPending) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "Experiment step configuration");
        ImGui::Separator();
    }

    if (gui.debugEnabled) {
        renderTunables(gui.tunables);
        ImGui::Separator();
    }

    ImGui::SeparatorText("Output Devices");
    if (ImGui::Button("Refresh")) refreshDeviceList(gui);
    ImGui::SameLine();
    if (ImGui::Button("Help")) gui.showHelp = true;
    if (!gui.enumerateError.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", gui.enumerateError.c_str());

    renderDeviceTable(gui);

    if (gui.deviceRows.empty() && gui.enumerateError.empty())
        ImGui::TextDisabled("No active render endpoints found.");

    const size_t selectedCount = std::count_if(
        gui.deviceRows.begin(), gui.deviceRows.end(),
        [](const DeviceRow& r) { return r.selected && !r.info.is_default_render; });

    ImGui::Separator();
    ImGui::BeginDisabled(selectedCount == 0 || gui.restarting.load());
    const char* confirmLabel = gui.experimentSetupPending ? "Confirm Step" : "Confirm && Play";
    if (ImGui::Button(confirmLabel, ImVec2(180, 0))) {
        EngineConfig cfg = buildConfig(gui);
        if (gui.experimentSetupPending) {
            gui.pendingConfigForStep = cfg;
            gui.pendingDeviceNamesForStep = selectedDeviceNames(gui);
        }
        startAsyncRestart(gui, engine, cfg);
        gui.screen = Screen::Live;
    }
    ImGui::EndDisabled();
    if (gui.restarting.load()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "Restarting, please wait...");
    }
    if (selectedCount == 0) ImGui::TextDisabled("Select at least one output device.");
}

void renderLiveTelemetryTable(const mux::EngineStats& stats, AudioEngine& engine,
                              std::string& delayError) {
    // ImGuiChildFlags_ResizeY gives a native drag handle on the bottom edge
    // (ImGui persists the chosen height itself across frames/sessions, the
    // same way it remembers a resized column), so the amount of vertical
    // space this table takes up is the user's call, not a guessed constant.
    ImGui::BeginChild("live_devices_region", ImVec2(0, 260),
                      ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders);

    if (ImGui::BeginTable("live_devices", 9,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 200.0f);
        ImGui::TableSetupColumn("Delay (ms)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Fill", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Clock Hz", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("EMA err", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Ovf / Partial", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Drops / API", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        // Indexed on purpose: a device's position in this list is exactly
        // the deviceIndex AudioEngine::setDeviceVolume() expects (see that
        // method's doc comment) — position-based addressing stays correct
        // even after isolate_device_faults prunes an earlier device, unlike
        // keying off the device's own fixed creation-time index.
        for (size_t i = 0; i < stats.devices.size(); ++i) {
            const auto& d = stats.devices[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            std::string name = narrowUtf8(d.name);
            ImGui::TextUnformatted(name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            int delayMs = static_cast<int>(d.configured_delay_ms);
            // Live delay editing. Committed on deactivate-after-edit (Enter
            // or focus loss) rather than every keystroke, since each accepted
            // change may mute this device briefly — firing one per digit
            // typed would be user-hostile.
            ImGui::InputInt("##livedelay", &delayMs, 0, 0);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string err;
                if (!engine.setDeviceDelay(static_cast<int>(i), delayMs, err))
                    delayError = err;  // surfaced under the table; never silently dropped
                else
                    delayError.clear();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Extra delay for this device, in milliseconds. Max for this session: "
                    "%lld ms.\nSmall changes apply smoothly; larger ones briefly mute THIS "
                    "device only - the others keep playing.",
                    static_cast<long long>(d.max_delay_ms_for_device));

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            float volPercent = d.volume * 100.0f;
            // Fresh copy of the authoritative value every frame — the usual
            // way to bind an ImGui widget to external state; ImGui's own
            // internal drag state takes over while the slider is actively
            // held, so this doesn't fight the user mid-drag. No restart, no
            // ring buffer or timing involved: see setDeviceVolume()'s doc
            // comment for why this is safe to call every frame it changes.
            if (ImGui::SliderFloat("##vol", &volPercent, 0.0f, 100.0f, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                engine.setDeviceVolume(static_cast<int>(i), volPercent / 100.0f);
            }

            ImGui::TableSetColumnIndex(3);
            const float pctF = d.target_bytes ? static_cast<float>(d.fill_bytes) /
                                                    static_cast<float>(d.target_bytes)
                                              : 0.0f;
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%.0f%%", pctF * 100.0f);
            // Bar is visually clamped at 100% (there's headroom above target
            // by design - see Help - so "full" doesn't mean "about to fail"),
            // but the overlay always shows the true percentage even past it.
            ImGui::ProgressBar(std::clamp(pctF, 0.0f, 1.0f), ImVec2(-1, 0), overlay);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", d.sample_rate_hz);

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.3f", d.ema_error);

            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%llu / %llu", static_cast<unsigned long long>(d.overflow_count),
                       static_cast<unsigned long long>(d.partial_underflow_count));

            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%llu / %llu",
                       static_cast<unsigned long long>(d.catastrophic_underflow_count),
                       static_cast<unsigned long long>(d.api_error_count));

            ImGui::TableSetColumnIndex(8);
            if (d.delay_adjust_in_flight)
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "adjusting");
            else
                ImGui::TextUnformatted(d.prebuffering ? "prebuffering" : "playing");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (!delayError.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Delay change rejected: %s",
                           delayError.c_str());
}

void renderLiveScreen(GuiState& gui, AudioEngine& engine) {
    const EngineState state = engine.state();
    const bool busy = gui.restarting.load();

    ImGui::Text("State: %s", mux::engineStateName(state));
    if (busy) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "(restarting)");
    }
    if (!gui.restartError.empty() && !busy) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Last restart error: %s",
                           gui.restartError.c_str());
    }

    ImGui::Spacing();

    if (engine.isRunning()) {
        const mux::EngineStats stats = engine.stats();

        gui.deviceNamesByIndex.clear();
        for (const auto& d : stats.devices) gui.deviceNamesByIndex.push_back(d.name);

        ImGui::Text("Mix: %u Hz, block %u B | capture polls %llu | logs dropped %llu",
                   stats.mix_sample_rate, stats.mix_block_align,
                   static_cast<unsigned long long>(stats.capture_poll_timeouts),
                   static_cast<unsigned long long>(stats.log_records_dropped));

        renderLiveTelemetryTable(stats, engine, gui.liveDelayError);

        // Experiment sampling: throttled, off the audio path entirely (this
        // just reads the same stats() snapshot already being displayed).
        if (gui.experimentActive && !gui.experimentSetupPending) {
            const auto now = std::chrono::steady_clock::now();
            if (now - gui.lastSampleTime > std::chrono::milliseconds(500)) {
                gui.recorder.recordSample(stats);
                gui.lastSampleTime = now;
            }
        }
    } else {
        ImGui::TextDisabled("Not running.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Every button here can trigger or interact with a restart; disabling
    // all of them while one is in flight prevents queuing a second restart
    // against a config that hasn't landed yet.
    ImGui::BeginDisabled(busy);

    if (ImGui::Button("Edit Devices")) {
        refreshDeviceList(gui);
        syncRowsFromEngine(gui, engine);
        gui.screen = Screen::DeviceSetup;
    }
    ImGui::SameLine();
    if (ImGui::Button(gui.showLogs ? "Hide Logs" : "Show Logs")) gui.showLogs = !gui.showLogs;
    ImGui::SameLine();
    if (ImGui::Button("Help")) gui.showHelp = true;
    ImGui::SameLine();
    if (ImGui::Button(gui.debugEnabled ? "Hide Debug" : "Debug")) {
        if (gui.debugEnabled)
            gui.debugEnabled = false;
        else
            gui.showDebugConfirmPopup = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(gui.experimentActive);
    if (ImGui::Button("Start Experiment")) {
        gui.experimentNameBuf[0] = '\0';
        gui.showExperimentNamePopup = true;
    }
    ImGui::EndDisabled();

    if (gui.experimentActive) {
        ImGui::SameLine();
        if (ImGui::Button("New Step")) {
            refreshDeviceList(gui);
            syncRowsFromEngine(gui, engine);
            gui.experimentSetupPending = true;
            gui.screen = Screen::DeviceSetup;
        }
        ImGui::SameLine();
        if (ImGui::Button("End Experiment")) {
            gui.recorder.finalize();
            gui.experimentActive = false;
            gui.experimentSetupPending = false;
            gui.debugEnabled = gui.debugStateBeforeExperiment;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Recording...");
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop Playback")) {
        // Sync first: once the engine stops, stats() no longer reports the
        // live delays/volumes, and they would be lost.
        syncRowsFromEngine(gui, engine);
        engine.requestStop();
        gui.screen = Screen::DeviceSetup;
        refreshDeviceList(gui);
    }

    ImGui::EndDisabled();

    // Debug parameters are also available here, at the bottom of the Live
    // screen, with their own Apply button that restarts using the CURRENT
    // device selection (no need to go back through device setup just to
    // change a tuning value).
    if (gui.debugEnabled) {
        ImGui::Spacing();
        ImGui::Separator();
        renderTunables(gui.tunables);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Apply Parameters", ImVec2(180, 0))) {
            // Changing a tuning value must not silently revert live-tuned
            // delays/volumes, so adopt them into the rows before rebuilding.
            syncRowsFromEngine(gui, engine);
            startAsyncRestart(gui, engine, buildConfig(gui));
        }
        ImGui::EndDisabled();
        if (busy) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "Restarting, please wait...");
        }
    }
}

void renderLogsWindow(GuiState& gui, AudioEngine& engine) {
    if (!gui.showLogs) return;
    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Logs", &gui.showLogs)) {
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("Level", &gui.logLevelIndex, kLogLevelNames,
                         IM_ARRAYSIZE(kLogLevelNames))) {
            engine.setLogLevel(kLogLevels[gui.logLevelIndex]);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Trace/Debug show detail the manual references (e.g. silence self-heal, rate changes); Info+ is quieter for normal use.");

        ImGui::BeginChild("scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        std::lock_guard<std::mutex> lock(gui.logMutex);
        const LogLevel activeFilter = kLogLevels[gui.logLevelIndex];
        for (const auto& m : gui.logLines) {
            // Display-side filter: engine.setLogLevel() only stops NEW
            // records below the chosen level from being captured — it can't
            // retroactively hide lines already sitting in this buffer from
            // before the dropdown changed. Re-applying the same threshold
            // here is what makes changing the dropdown actually do anything
            // to what's already on screen.
            if (m.level < activeFilter) continue;

            ImVec4 color(0.8f, 0.8f, 0.8f, 1.0f);
            if (m.level == LogLevel::Warn) color = ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
            else if (m.level == LogLevel::Error || m.level == LogLevel::Fatal)
                color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            else if (m.level == LogLevel::Debug || m.level == LogLevel::Trace)
                color = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);

            std::string devTag;
            if (m.device >= 0) {
                if (static_cast<size_t>(m.device) < gui.deviceNamesByIndex.size()) {
                    std::string n(gui.deviceNamesByIndex[m.device].begin(),
                                 gui.deviceNamesByIndex[m.device].end());
                    devTag = " [" + n + "]";
                } else {
                    devTag = " [dev " + std::to_string(m.device) + "]";
                }
            }
            ImGui::TextColored(color, "[%s]%s %s", mux::levelName(m.level), devTag.c_str(),
                               m.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

void renderHelpWindow(GuiState& gui) {
    if (!gui.showHelp) return;
    ImGui::SetNextWindowSize(ImVec2(640, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Help", &gui.showHelp)) {
        ImGui::TextWrapped("%s", mux::help::kHowToUse);
        ImGui::Separator();
        ImGui::BeginChild("help_scroll", ImVec2(0, 0));
        for (const auto& entry : mux::help::kEntries) {
            ImGui::SeparatorText(entry.first);
            ImGui::TextWrapped("%s", entry.second);
            ImGui::Spacing();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

}  // namespace

//==============================================================================
// Main
//==============================================================================
int main(int, char**) {
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale =
        ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = {sizeof(wc),      CS_CLASSDC, WndProc, 0L,   0L,
                      GetModuleHandle(nullptr), nullptr,   nullptr, nullptr, nullptr,
                      L"MultiMuxGui",  nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"MultiMux", WS_OVERLAPPEDWINDOW, 100, 100,
                               (int)(1000 * main_scale), (int)(700 * main_scale), nullptr,
                               nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();  // dark mode; no theme switch requested
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    GuiState gui;
    AudioEngine engine;
    engine.setLogLevel(kLogLevels[gui.logLevelIndex]);
    engine.setLogSink([&gui](const LogMessage& m) {
        std::lock_guard<std::mutex> lock(gui.logMutex);
        gui.logLines.push_back(m);
        if (gui.logLines.size() > GuiState::kMaxLogLines) gui.logLines.pop_front();
    });

    refreshDeviceList(gui);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Pick up a finished async restart before drawing this frame.
        {
            std::lock_guard<std::mutex> lock(gui.restartResultMutex);
            if (gui.haveRestartResult) {
                gui.haveRestartResult = false;
                if (gui.restartError.empty() && gui.experimentSetupPending) {
                    gui.recorder.beginStep(gui.pendingConfigForStep, gui.pendingDeviceNamesForStep);
                    gui.experimentSetupPending = false;
                }
                if (!gui.restartError.empty()) {
                    // Restart failed: stay on/return to setup so the user can retry.
                    gui.screen = Screen::DeviceSetup;
                }
            }
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("MultiMux", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (gui.screen == Screen::DeviceSetup)
            renderDeviceSetupScreen(gui, engine);
        else
            renderLiveScreen(gui, engine);

        ImGui::End();

        renderLogsWindow(gui, engine);
        renderHelpWindow(gui);
        renderDebugConfirmPopup(gui);
        renderExperimentNamePopup(gui, engine);

        ImGui::Render();
        const float clear_color[4] = {0.06f, 0.06f, 0.07f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    if (gui.restartThread.joinable()) gui.restartThread.join();
    engine.requestStop();
    engine.stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

//==============================================================================
// Platform harness implementation (unmodified from the ImGui example)
//==============================================================================
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                            createDeviceFlags, featureLevelArray, 2,
                                            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
                                            &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
