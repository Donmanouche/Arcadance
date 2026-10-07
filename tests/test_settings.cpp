#include "midipad/AppSettings.hpp"

#include <doctest/doctest.h>

#include <fstream>

using namespace midipad;
namespace fs = std::filesystem;

TEST_CASE("settings round-trip and fall back to defaults") {
    const fs::path dir = fs::temp_directory_path() / "midipad-settings-test";
    fs::remove_all(dir);
    const fs::path file = dir / "settings.json";

    AppSettings defaults;
    defaults.presetsDir = "/default/presets";
    CHECK(loadSettings(file, defaults).presetsDir == "/default/presets");  // no file yet

    AppSettings s = defaults;
    s.presetsDir = "/home/me/presets";
    s.lastPreset = "PS5_Test";
    s.output = AppSettings::Output::ExistingPort;
    s.existingPortName = "OXI ONE";
    s.outputChosen = true;
    s.monitorMidi = true;
    s.key = Key{9, *findScale("harmonic_minor")};
    s.performanceView = true;
    saveSettings(s, file);

    const AppSettings loaded = loadSettings(file, defaults);
    CHECK(loaded.presetsDir == "/home/me/presets");
    CHECK(loaded.lastPreset == "PS5_Test");
    CHECK(loaded.output == AppSettings::Output::ExistingPort);
    CHECK(loaded.existingPortName == "OXI ONE");
    CHECK(loaded.outputChosen);
    CHECK(loaded.monitorMidi);
    CHECK(loaded.performanceView);
    REQUIRE(loaded.key);
    CHECK(loaded.key->root == 9);
    CHECK(loaded.key->type().id == "harmonic_minor");
    CHECK(loaded.virtualPortName == "MidiPad");

    std::ofstream(file) << R"({"last_preset": 42, "presets_dir": "/x"})";
    const AppSettings partial = loadSettings(file, defaults);
    CHECK(partial.presetsDir == "/x");
    CHECK(partial.lastPreset.empty());  // wrong type: default kept
    CHECK_FALSE(partial.key);           // no tonality unless one was chosen

    CHECK_FALSE(loadSettings(file, defaults).outputChosen);
    std::ofstream(file) << R"({"output": "port", "existing_port_name": "VirMIDI 3-0"})";
    CHECK(loadSettings(file, defaults).outputChosen);  // older settings: a remembered port was chosen

    std::ofstream(file) << "{ broken";
    CHECK(loadSettings(file, defaults).presetsDir == "/default/presets");
    fs::remove_all(dir);
}
