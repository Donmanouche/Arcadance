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

};

// Missing or unreadable values keep the ones of `defaults`.
AppSettings loadSettings(const std::filesystem::path& file, AppSettings defaults);
void saveSettings(const AppSettings& settings, const std::filesystem::path& file);  // throws on I/O error

}  // namespace midipad
