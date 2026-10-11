#pragma once

#include "midipad/Humanize.hpp"
#include "midipad/Scales.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace midipad {

// What the application remembers between sessions.
struct AppSettings {
    std::filesystem::path presetsDir;
    std::string lastPreset;

    enum class Output { VirtualPort, ExistingPort };
    Output output = Output::VirtualPort;
    std::string virtualPortName = "MidiPad";
    std::string existingPortName;  // matched as a substring, like --connect
    bool outputChosen = false;     // false: ask which output to use at startup

    // Narrows the notes and chords offered by the editor; none: every note.
    std::optional<Key> key;

    // Engine::setDpadSettle, in milliseconds.
    int dpadSettlePressMs = 15;
    int dpadSettleReleaseMs = 15;

    Humanize humanize;  // off on every channel by default

    // Sync: Ableton Link shares tempo and beat with the DAW. With it, loops
    // snap to `loopSnap` (their length rounded to it, their start on it); without
    // it, MidiPad runs free.
    bool linkEnabled = false;
    enum class LoopSnap { Free, Beat, Bar, TwoBars, FourBars };
    LoopSnap loopSnap = LoopSnap::Beat;
    int beatsPerBar = 4;
    bool startStopSync = true;  // Play / Stop follow, and lead, the Link session's
    // The snap in beats: 0 for Free.
    int snapBeats() const {
        switch (loopSnap) {
        case LoopSnap::Free: return 0;
        case LoopSnap::Beat: return 1;
        case LoopSnap::Bar: return beatsPerBar;
        case LoopSnap::TwoBars: return 2 * beatsPerBar;
        case LoopSnap::FourBars: return 4 * beatsPerBar;
        }
        return 0;
    }
};

// Missing or unreadable values keep the ones of `defaults`.
AppSettings loadSettings(const std::filesystem::path& file, AppSettings defaults);
void saveSettings(const AppSettings& settings, const std::filesystem::path& file);  // throws on I/O error

}  // namespace midipad
