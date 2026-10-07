// Converts the presets of the Python version (configs/*.json, with their
// <name>-<channel>.json files) to the MidiPad 2 preset format.

#include "midipad/LegacyImport.hpp"
#include "midipad/PresetIO.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: midipad-import <configs folder of the Python version> <output folder>\n";
        return 2;
    }
    const fs::path source = argv[1];
    const fs::path target = argv[2];
    if (!fs::is_directory(source)) {
        std::cerr << "Folder not found: " << source << '\n';
        return 1;
    }
    fs::create_directories(target);

    const auto imported = midipad::importLegacyDirectory(source);
    for (const auto& error : imported.errors)
        std::cerr << "ERROR " << error << '\n';

    int failures = static_cast<int>(imported.errors.size());
    for (const auto& result : imported.presets) {
        const fs::path out = target / (result.preset.name + ".json");
        int channels = 0;
        for (const auto& c : result.preset.channels)
            channels += c.has_value();
        try {
            midipad::savePreset(result.preset, out);
            std::cout << "OK  " << result.preset.name;
            if (channels)
                std::cout << " (+" << channels << (channels > 1 ? " channel-specific mappings)" : " channel-specific mapping)");
            std::cout << " -> " << out.string() << '\n';
        } catch (const std::exception& e) {
            std::cerr << "ERROR " << result.preset.name << ": " << e.what() << '\n';
            ++failures;
        }
        for (const auto& warning : result.warnings)
            std::cout << "    ! " << warning << '\n';
    }
    return failures ? 1 : 0;
}
