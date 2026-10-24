#pragma once

#include "midipad/Preset.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midipad {

// The presets of one folder: <folder>/<name>.json.
class PresetLibrary {
public:
    explicit PresetLibrary(std::filesystem::path directory);


    std::vector<std::string> names() const;  // sorted, case-insensitive
    bool contains(std::string_view name) const;
    std::filesystem::path pathFor(std::string_view name) const;

    Preset load(std::string_view name) const;  // throws PresetError
    void save(const Preset& preset) const;     // to pathFor(preset.name); creates the folder
    // Deletes the file for good and returns its path; throws if there is none.
    std::filesystem::path remove(std::string_view name) const;

    // "Base", "Base 2", "Base 3"... whichever is free.
    std::string uniqueName(std::string_view base) const;

    // Why `name` cannot be a preset name, or nullopt if it can.
    static std::optional<std::string> nameError(std::string_view name);

private:
    std::filesystem::path directory_;
};

}  // namespace midipad
