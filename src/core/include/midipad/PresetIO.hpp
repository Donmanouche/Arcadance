#pragma once

#include "midipad/Preset.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace midipad {

// Raised for unreadable or invalid preset files; the message names the
// offending entry, e.g. "base.buttons.south: unknown type 'notes'".
class PresetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr int kPresetFormatVersion = 2;

Preset presetFromJson(const nlohmann::ordered_json& json);
nlohmann::ordered_json presetToJson(const Preset& preset);

Preset loadPreset(const std::filesystem::path& file);
void savePreset(const Preset& preset, const std::filesystem::path& file);

// Reads a note given as a MIDI number or a name like "C4".
std::optional<std::uint8_t> noteFromJson(const nlohmann::ordered_json& value);

// The longest start of `text` that fits in `maxBytes`, never cutting a UTF-8
// character in two (a name cut in the middle of an "é" could not be saved).
std::string truncateUtf8(std::string_view text, std::size_t maxBytes);

}  // namespace midipad
