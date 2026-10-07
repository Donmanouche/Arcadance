#include "midipad/PresetIO.hpp"
#include "midipad/PresetLibrary.hpp"

#include <doctest/doctest.h>

#include <fstream>

using namespace midipad;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() / "midipad-library-test";
    TempDir() {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

Preset named(std::string name, int note = 48) {
    Preset p;
    p.name = std::move(name);
    p.base.buttons[0].type = ActionType::Note;
    p.base.buttons[0].number = static_cast<std::uint8_t>(note);
    return p;
}

}  // namespace

TEST_CASE("a library lists, saves and loads the presets of its folder") {
    TempDir dir;
    PresetLibrary library(dir.path / "presets");  // created on first save
    CHECK(library.names().empty());

    library.save(named("zeta"));
    library.save(named("Alpha", 60));
    library.save(named("beta"));
    std::ofstream(dir.path / "presets" / "notes.txt") << "not a preset";
    CHECK(library.names() == std::vector<std::string>{"Alpha", "beta", "zeta"});

    CHECK(library.contains("Alpha"));
    CHECK_FALSE(library.contains("alpha2"));
    CHECK(library.load("Alpha").base.buttons[0].number == 60);
}

TEST_CASE("the file name is the preset name") {
    TempDir dir;
    PresetLibrary library(dir.path);
    savePreset(named("old name"), library.pathFor("renamed"));
    CHECK(library.load("renamed").name == "renamed");
}

TEST_CASE("removed presets go to the trash folder, never deleted") {
    TempDir dir;
    PresetLibrary library(dir.path);
    library.save(named("live"));
    const auto first = library.moveToTrash("live");
    CHECK(first == dir.path / "trash" / "live.json");
    CHECK(fs::exists(first));
    CHECK(library.names().empty());

    library.save(named("live"));
    CHECK(library.moveToTrash("live") == dir.path / "trash" / "live (2).json");
    CHECK(fs::exists(first));
}

TEST_CASE("unique names count up") {
    TempDir dir;
    PresetLibrary library(dir.path);
    CHECK(library.uniqueName("Base") == "Base");
    library.save(named("Base"));
    CHECK(library.uniqueName("Base") == "Base 2");
    library.save(named("Base 2"));
    CHECK(library.uniqueName("Base") == "Base 3");
}

TEST_CASE("names that would be unsafe file names are refused") {
    CHECK_FALSE(PresetLibrary::nameError("PS5 Live set"));
    CHECK_FALSE(PresetLibrary::nameError("Accords-é"));
    CHECK(PresetLibrary::nameError(""));
    CHECK(PresetLibrary::nameError("a/b"));
    CHECK(PresetLibrary::nameError("a\\b"));
    CHECK(PresetLibrary::nameError("what?"));
    CHECK(PresetLibrary::nameError(".hidden"));
    CHECK(PresetLibrary::nameError("trailing "));
    CHECK(PresetLibrary::nameError(std::string(101, 'a')));

    TempDir dir;
    CHECK_THROWS_AS(PresetLibrary(dir.path).save(named("a/b")), PresetError);
}
