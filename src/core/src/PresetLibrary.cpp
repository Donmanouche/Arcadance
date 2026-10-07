#include "midipad/PresetLibrary.hpp"

#include "midipad/PresetIO.hpp"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace midipad {

namespace {

constexpr const char* kTrashFolder = "trash";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

PresetLibrary::PresetLibrary(fs::path directory) : directory_(std::move(directory)) {}

std::vector<std::string> PresetLibrary::names() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory_, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            names.push_back(entry.path().stem().string());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return lower(a) < lower(b) || (lower(a) == lower(b) && a < b);
    });
    return names;
}

bool PresetLibrary::contains(std::string_view name) const {
    std::error_code ec;
    return fs::is_regular_file(pathFor(name), ec);
}

fs::path PresetLibrary::pathFor(std::string_view name) const {
    return directory_ / (std::string(name) + ".json");
}

Preset PresetLibrary::load(std::string_view name) const {
    Preset p = loadPreset(pathFor(name));
    p.name = std::string(name);  // the file name wins over the stored name
    return p;
}

void PresetLibrary::save(const Preset& preset) const {
    if (auto error = nameError(preset.name))
        throw PresetError(*error);
    fs::create_directories(directory_);
    savePreset(preset, pathFor(preset.name));
}

fs::path PresetLibrary::moveToTrash(std::string_view name) const {
    const fs::path trash = directory_ / kTrashFolder;
    fs::create_directories(trash);
    fs::path target = trash / (std::string(name) + ".json");
    for (int i = 2; fs::exists(target); ++i)
        target = trash / (std::string(name) + " (" + std::to_string(i) + ").json");
    fs::rename(pathFor(name), target);
    return target;
}

std::string PresetLibrary::uniqueName(std::string_view base) const {
    std::string name(base);
    for (int i = 2; contains(name); ++i)
        name = std::string(base) + " " + std::to_string(i);
    return name;
}

std::optional<std::string> PresetLibrary::nameError(std::string_view name) {
    if (name.empty())
        return "The name is empty.";
    if (name.size() > 100)
        return "The name is too long (100 characters at most).";
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos)
            return "The name cannot contain / \\ : * ? \" < > |";
    }
    if (name.front() == '.')
        return "The name cannot start with a dot.";
    if (name.back() == '.' || name.back() == ' ')
        return "The name cannot end with a dot or a space.";
    return std::nullopt;
}

}  // namespace midipad
