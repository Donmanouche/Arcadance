#pragma once

#include "midipad/Preset.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace midipad {

// Conversion of presets written by the Python version (configs/*.json).
//
// Those files number controls the way the Linux joystick driver enumerates a
// DualSense: buttons 0-12 = Cross, Circle, Triangle, Square, L1, R1, L2, R2,
// Create, Options, PS, L3, R3; axes 0-5 = LX, LY, L2, RX, RY, R2.

struct LegacyImportResult {
    Preset preset;
    std::vector<std::string> warnings;  // entries that were dropped or reinterpreted
};

// `channelFiles` maps a 1-based MIDI channel to the content of <name>-<channel>.json.
LegacyImportResult importLegacyPreset(std::string name, const nlohmann::ordered_json& baseFile,
                                      const std::map<int, nlohmann::ordered_json>& channelFiles);

struct LegacyDirectoryImport {
    std::vector<LegacyImportResult> presets;
    std::vector<std::string> errors;  // files that could not be read at all
};

// Groups <name>.json with its <name>-1.json ... <name>-16.json channel files.
LegacyDirectoryImport importLegacyDirectory(const std::filesystem::path& configsDir);

}  // namespace midipad
