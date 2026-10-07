#pragma once

#include "midipad/Preset.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midipad {

// The presets of one folder: <folder>/<name>.json. Nothing is ever deleted:
// removed presets go to <folder>/trash/.
class PresetLibrary {
public:
    explicit PresetLibrary(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return directory_; }

    std::vector<std::string> names() const;  // sorted, case-insensitive
    bool contains(std::string_view name) const;
    std::filesystem::path pathFor(std::string_view name) const;

    Preset load(std::string_view name) const;  // throws PresetError
    void save(const Preset& preset) const;     // to pathFor(preset.name); creates the folder
    // Returns where the file went.
    std::filesystem::path moveToTrash(std::string_view name) const;

    // "Base", "Base 2", "Base 3"... whichever is free.
    std::string uniqueName(std::string_view base) const;

    // Why `name` cannot be a preset name, or nullopt if it can.
    static std::optional<std::string> nameError(std::string_view name);

private:
    std::filesystem::path directory_;
};

}  // namespace midipad
