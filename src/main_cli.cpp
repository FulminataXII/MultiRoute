//==============================================================================
// main_cli.cpp - Console frontend. Contains no audio logic whatsoever; it only
// collects configuration, starts the engine, renders its log stream and its
// telemetry. A GUI would replace this file and nothing else.
//==============================================================================

#include "AudioEngine.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex g_consoleMutex;

void printLine(const std::wstring& line) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    std::wcout << line << std::endl;
}

std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// Validated integer input. A failed extraction leaves cin in a fail state and
// the target untouched, which would silently corrupt every later prompt, so
// the stream is cleared and the user is re-prompted.
int64_t readInt64(const std::wstring& prompt) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_consoleMutex);
            std::wcout << prompt;
        }
        int64_t value = 0;
        if (std::cin >> value) {
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return value;
        }
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        printLine(L"  Not a valid number, try again.");
    }
}

void renderLog(const mux::LogMessage& m) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    std::wostringstream os;
    os << L"[" << widen(mux::levelName(m.level)) << L"]";
    if (m.device >= 0) os << L"[dev " << m.device << L"]";
    os << L" (" << static_cast<int>(m.event) << L") " << widen(m.text);
    std::wcout << os.str() << std::endl;
}

void printStats(const mux::EngineStats& s) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    std::wcout << L"\n--- Telemetry ---------------------------------\n";
    std::wcout << L"  mix " << s.mix_sample_rate << L" Hz, block " << s.mix_block_align
               << L" B | capture polls " << s.capture_poll_timeouts
               << L" | logs dropped " << s.log_records_dropped << L"\n";
    for (const auto& d : s.devices) {
        const double fillPct =
            d.target_bytes ? (100.0 * static_cast<double>(d.fill_bytes) /
                              static_cast<double>(d.target_bytes))
                           : 0.0;
        std::wcout << L"  " << d.name << L"  (+" << d.configured_delay_ms << L" ms)\n"
                   << L"    fill   " << d.fill_bytes << L" / " << d.target_bytes
                   << L" B (" << std::fixed << std::setprecision(1) << fillPct << L"%)"
                   << (d.prebuffering ? L"  [prebuffering]" : L"") << L"\n"
                   << L"    clock  " << std::setprecision(2) << d.sample_rate_hz
                   << L" Hz   ema " << std::setprecision(3) << d.ema_error << L"\n"
                   << L"    faults " << d.overflow_count << L" ovf | "
                   << d.partial_underflow_count << L" partial | "
                   << d.catastrophic_underflow_count << L" drops | "
                   << d.api_error_count << L" api\n";
    }
    std::wcout << std::endl;
}

}  // namespace

int wmain() {
    std::vector<mux::EndpointInfo> endpoints;
    std::string error;
    if (!mux::AudioEngine::enumerateOutputs(endpoints, error)) {
        std::wcerr << L"Fatal: " << widen(error) << std::endl;
        return 1;
    }

    std::wcout << L"--- Available Audio Outputs ---" << std::endl;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        std::wcout << L"[" << i << L"] " << endpoints[i].name
                   << (endpoints[i].is_default_render ? L"   (loopback source)" : L"")
                   << std::endl;
    }

    std::wcout << L"\nEnter device numbers separated by space: ";
    std::string selectionLine;
    std::getline(std::cin, selectionLine);

    std::vector<size_t> selected;
    {
        std::istringstream ss(selectionLine);
        long long idx;
        while (ss >> idx) {
            if (idx >= 0 && static_cast<size_t>(idx) < endpoints.size())
                selected.push_back(static_cast<size_t>(idx));
        }
    }
    if (selected.empty()) {
        std::wcout << L"No devices selected." << std::endl;
        return 0;
    }

    mux::EngineConfig config;
    for (size_t idx : selected) {
        mux::OutputRequest req;
        req.endpoint_id = endpoints[idx].id;
        req.relative_delay_ms =
            readInt64(L"Relative software delay for [" + endpoints[idx].name + L"] (ms): ");
        config.outputs.push_back(std::move(req));
    }

    mux::AudioEngine engine;
    engine.setLogLevel(mux::LogLevel::Info);
    engine.setLogSink(renderLog);
    engine.setStateCallback([](mux::EngineState s) {
        printLine(L"[STATE] " + widen(mux::engineStateName(s)));
    });

    if (!engine.start(config, error)) {
        std::wcerr << L"Fatal: " << widen(error) << std::endl;
        return 1;
    }

    std::atomic<bool> polling{true};
    std::thread statsThread([&] {
        while (polling.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!polling.load(std::memory_order_relaxed)) break;
            if (engine.isRunning()) printStats(engine.stats());
        }
    });

    printLine(L"\n[System Live] Type 'x' and press ENTER to stop.");

    std::string cmd;
    while (std::getline(std::cin, cmd)) {
        if (cmd == "x" || cmd == "X") break;
        if (cmd == "s" || cmd == "S") printStats(engine.stats());
        if (!engine.isRunning() && engine.state() == mux::EngineState::Faulted) {
            printLine(L"Engine faulted; exiting.");
            break;
        }
    }

    polling.store(false, std::memory_order_relaxed);
    if (statsThread.joinable()) statsThread.join();

    engine.stop();
    return 0;
}
