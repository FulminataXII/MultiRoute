#pragma once
//==============================================================================
// HelpText.h - Plain-English glossary shown in the GUI's Help window.
//
// EDIT THIS FILE FREELY: it is pure data, included only by main_gui.cpp's
// help window. Adding, rewording, or removing an entry never touches any
// rendering or engine logic. Keep descriptions to 1-3 short sentences —
// this is a glossary, not a manual (see TEST_MANUAL.md for depth).
//==============================================================================

#include <utility>
#include <vector>

namespace mux::help {

// Shown at the top of the Help window, before the glossary.
inline const char* kHowToUse =
    "1. Check the devices you want audio sent to, set a delay (ms) for each "
    "if you want them offset from each other, then press Confirm.\n"
    "2. The Live screen shows what's currently playing. Use Edit Devices to "
    "change the selection later (this restarts playback briefly).\n"
    "3. Debug reveals advanced tuning knobs - only touch these if something "
    "sounds wrong and you know what you're changing.\n"
    "4. Experiment Mode records a named session: your device/parameter choices "
    "plus live telemetry, step by step, to a log file for later comparison.";

// {term, description} pairs, shown in this order.
inline const std::vector<std::pair<const char*, const char*>> kEntries = {
    {"Loopback source",
     "The device Windows is currently set to play audio through by default. "
     "This app listens to that device's audio and copies it to the outputs "
     "you select. It can't also be selected as an output - it's already "
     "playing that same audio natively."},
    {"Relative delay",
     "An extra software wait time (in milliseconds) applied to one output "
     "before it plays, so multiple speakers/devices with different natural "
     "latencies can be brought into sync with each other. It is not the "
     "device's total end-to-end latency, just the extra offset you're adding."},
    {"Changing delay while playing",
     "You can edit a device's delay on the Live screen without restarting. "
     "Small changes ease in silently. Larger ones briefly mute that one "
     "device while its buffer is adjusted - the other devices keep playing "
     "throughout. There's a ceiling per session (shown in the tooltip): the "
     "buffer is sized when playback starts, so going beyond it needs a "
     "restart. Rejected changes always say why, just under the table."},
    {"Cushion",
     "How much audio (in milliseconds) is buffered ahead of time before "
     "playback starts. A bigger cushion tolerates more system hiccups "
     "without glitching, at the cost of a longer delay before sound starts."},
    {"Prebuffering",
     "The device is filling its cushion before playing; you'll hear silence "
     "briefly during this state. Normal right after starting or after a bad "
     "enough dropout that playback had to restart."},
    {"Fill %",
     "How full a device's audio buffer is compared to its target cushion. "
     "100% means exactly at target. Above 100% is fine (there's spare "
     "headroom above the target); near 0% for a while means it may glitch."},
    {"Clock Hz",
     "The playback sample rate this app is currently telling that device to "
     "use. It nudges slightly above or below the normal rate to correct for "
     "tiny differences between devices' hardware clocks, keeping them in "
     "sync over time."},
    {"Volume",
     "Per-device output level, adjustable while audio is playing. This "
     "doesn't restart anything or touch timing/sync - it's a separate, "
     "instant control built into Windows' own audio pipeline for exactly "
     "this purpose, which is why it can't cause a glitch the way changing a "
     "delay or buffer setting would."},
    {"EMA error",
     "A smoothed measure of how far a device's buffer fill is from its "
     "target, used internally to decide clock corrections. Near zero and "
     "stable is good; large or constantly growing suggests the sync "
     "parameters need tuning."},
    {"Overflow",
     "Audio arrived faster than a device could store it and some was "
     "dropped. Occasional overflows are usually harmless; frequent ones "
     "suggest that device's buffer is undersized for its delay setting."},
    {"Partial / Drops (underflow)",
     "The device ran out of buffered audio to play. 'Partial' means only "
     "some of what was needed was missing (minor); 'Drops' means none was "
     "available and silence had to be played instead (audible glitch)."},
    {"Capture polls",
     "Windows can stop notifying this app when the source device goes "
     "completely silent. This app checks in periodically on its own during "
     "silence so it doesn't get stuck - a rising count during silence is "
     "expected and fine; a rising count during active playback is not."},
    {"Isolate device faults",
     "If one output device has a technical failure, this decides what "
     "happens: OFF (default) stops everything and reports an error, which "
     "is easiest to notice and diagnose. ON drops just that device and "
     "keeps the others playing."},
    {"Debug parameters",
     "Low-level tuning values (buffer sizes, timing, sync sensitivity). "
     "Changing these can affect audio quality or cause glitches if set "
     "poorly - the confirmation prompt exists so you don't reveal or "
     "change them by accident."},
    {"Experiment",
     "A named recording session. Each time you confirm a device/parameter "
     "setup during an experiment, that's a 'step' - its settings and its "
     "live telemetry over time are saved together. Useful for comparing "
     "how different settings behave, side by side, after the fact."},
};

}  // namespace mux::help
