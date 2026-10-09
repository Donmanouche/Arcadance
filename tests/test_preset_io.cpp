#include "midipad/PresetIO.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <filesystem>

using namespace midipad;
using Json = nlohmann::ordered_json;

namespace {

Json minimal(Json base) {
    return Json{{"format", "midipad-preset"}, {"version", 2}, {"name", "test"}, {"base", std::move(base)}};
}

std::string errorFor(const Json& j) {
    try {
        presetFromJson(j);
    } catch (const PresetError& e) {
        return e.what();
    }
    return {};
}

}  // namespace

TEST_CASE("every kind of binding survives a save/load round trip") {
    const Json original = minimal(Json::parse(R"({
        "buttons": {
            "south": {"type": "note", "note": 48, "mode": "toggle"},
            "east": {"type": "note", "note": 50, "velocity": 90, "mode": "momentary"},
            "north": {"type": "chord", "chord": "Cm9", "mode": "toggle"},
            "west": {"type": "chord", "notes": [60, 64, 67], "mode": "momentary"},
            "back": {"type": "channel", "step": -1},
            "start": {"type": "channel", "step": 1},
            "left_stick": {"type": "freeze", "axes": ["left_x", "left_y"]},
            "left_shoulder": {"type": "cc", "cc": 52, "value": 100, "off_value": 10, "mode": "toggle"},
            "guide": {"type": "transport", "command": "stop"},
            "left_trigger": {"type": "note", "note": 63, "mode": "momentary"}
        },
        "dpad": {
            "up": {"type": "chord", "chord": "Ebmaj11", "mode": "toggle"},
            "down_right": {"type": "cc", "cc": 47, "value": 64, "mode": "momentary"}
        },
        "axes": {
            "left_x": {"cc": 40},
            "left_y": {"cc": 41, "invert": true},
            "right_trigger": {"cc": 51, "min": 10, "max": 90}
        },
        "touchpad": {
            "finger1": {"mode": "zones", "zones": {"top_left": {"type": "note", "note": 48, "mode": "momentary"}}},
            "finger2": {"mode": "axis", "x": {"cc": 62}, "y": {"cc": 63}}
        }
    })"));
    Json withChannel = original;
    withChannel["channels"] = Json{{"2", Json{{"buttons", {{"south", {{"type", "note"}, {"note", 72}, {"mode", "momentary"}}}}}}}};

    const Preset preset = presetFromJson(withChannel);
    // Compare ignoring key order.
    CHECK(nlohmann::json::parse(presetToJson(preset).dump()) == nlohmann::json::parse(withChannel.dump()));

    CHECK(preset.base.buttons[static_cast<std::size_t>(Button::North)].chord.count == 5);
    CHECK(preset.base.axes[static_cast<std::size_t>(Axis::LeftY)].invert);
    CHECK(preset.channels[1].has_value());
    CHECK(preset.forChannel(1).buttons[0].number == 72);
    CHECK(preset.forChannel(2).buttons[0].number == 48);
}

TEST_CASE("notes may be written as names") {
    const Preset p = presetFromJson(minimal({{"buttons", {{"south", {{"type", "note"}, {"note", "E4"}}}}}}));
    CHECK(p.base.buttons[0].number == 64);
    CHECK(p.base.buttons[0].mode == Mode::Momentary);
}

TEST_CASE("a chord can be placed in another octave") {
    const Json j = minimal({{"buttons", {{"north", {{"type", "chord"}, {"chord", "Cm"}, {"octave", 2}}},
                                         {"south", {{"type", "chord"}, {"chord", "Cm"}}}}}});
    const Preset p = presetFromJson(j);
    const TriggerBinding& low = p.base.buttons[static_cast<std::size_t>(Button::North)];
    CHECK(low.chordOctave == 2);
    CHECK(low.chord.notes[0] == 36);  // C2
    CHECK(p.base.buttons[static_cast<std::size_t>(Button::South)].chord.notes[0] == 60);

    const Json saved = presetToJson(p);
    CHECK(saved["base"]["buttons"]["north"]["octave"] == 2);
    CHECK_FALSE(saved["base"]["buttons"]["south"].contains("octave"));  // the default stays implicit

    const Json doubled = minimal({{"buttons", {{"north", {{"type", "chord"}, {"chord", "Cm"}, {"root_below", 2}}}}}});
    const TriggerBinding bass = presetFromJson(doubled).base.buttons[static_cast<std::size_t>(Button::North)];
    CHECK(bass.chordRootBelow == 2);
    CHECK(std::vector(bass.chord.view().begin(), bass.chord.view().end()) == std::vector<std::uint8_t>{36, 60, 63, 67});
    CHECK(presetToJson(presetFromJson(doubled))["base"]["buttons"]["north"]["root_below"] == 2);
    CHECK(errorFor(minimal({{"buttons", {{"north", {{"type", "chord"}, {"chord", "Cm"}, {"root_below", 3}}}}}})) ==
          "base.buttons.north.root_below: expected 0, 1 or 2 (octaves below the root)");

    CHECK(errorFor(minimal({{"buttons", {{"north", {{"type", "chord"}, {"chord", "Cm"}, {"octave", 9}}}}}})) ==
          "base.buttons.north.octave: expected an integer from 0 to 8");
    CHECK(errorFor(minimal({{"buttons", {{"north", {{"type", "chord"}, {"chord", "B13"}, {"octave", 8}}}}}})) ==
          "base.buttons.north.octave: \"B13\" goes beyond the MIDI notes in this octave");
}

TEST_CASE("a note or chord can opt out of humanization") {
    const Json j = minimal({{"buttons", {{"north", {{"type", "chord"}, {"chord", "Cm"}, {"humanize", false}}},
                                         {"south", {{"type", "note"}, {"note", 60}}}}}});
    const Preset p = presetFromJson(j);
    CHECK_FALSE(p.base.buttons[static_cast<std::size_t>(Button::North)].humanize);
    CHECK(p.base.buttons[static_cast<std::size_t>(Button::South)].humanize);
    const Json saved = presetToJson(p);
    CHECK(saved["base"]["buttons"]["north"]["humanize"] == false);
    CHECK_FALSE(saved["base"]["buttons"]["south"].contains("humanize"));
}

TEST_CASE("errors name the faulty entry") {
    CHECK(errorFor(minimal({{"buttons", {{"south", {{"type", "notes"}}}}}})) ==
          "base.buttons.south.type: unknown type \"notes\"");
    CHECK(errorFor(minimal({{"buttons", {{"cross", {{"type", "note"}, {"note", 1}}}}}})) ==
          "base.buttons.cross: unknown button");
    CHECK(errorFor(minimal({{"buttons", {{"dpad_up", {{"type", "note"}, {"note", 1}}}}}})) ==
          "base.buttons.dpad_up: map D-pad directions under \"dpad\"");
    CHECK(errorFor(minimal({{"buttons", {{"south", {{"type", "note"}, {"note", 128}}}}}})) ==
          "base.buttons.south.note: expected a MIDI note number or a name like \"C4\"");
    CHECK(errorFor(minimal({{"dpad", {{"up", {{"type", "chord"}, {"chord", "Hmaj"}}}}}})) ==
          "base.dpad.up.chord: unknown chord \"Hmaj\"");
    CHECK(errorFor(minimal({{"axes", {{"left_x", {{"cc", 200}}}}}})) ==
          "base.axes.left_x.cc: expected an integer from 0 to 127");

    Json badChannel = minimal(Json::object());
    badChannel["channels"] = {{"17", Json::object()}};
    CHECK(errorFor(badChannel) == "channels.17: expected a channel number from 1 to 16");
}

TEST_CASE("files from another program or a newer version are refused") {
    CHECK(errorFor(Json{{"mappings", Json::object()}}).starts_with("format:"));
    Json newer = minimal(Json::object());
    newer["version"] = kPresetFormatVersion + 1;
    CHECK(errorFor(newer).starts_with("version:"));
}

TEST_CASE("presets are written to and read from disk") {
    const auto dir = std::filesystem::temp_directory_path() / "midipad-preset-io-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "roundtrip.json";

    Preset p;
    p.name = "roundtrip";
    p.base.buttons[0].type = ActionType::Note;
    p.base.buttons[0].number = 61;
    savePreset(p, file);

    const Preset loaded = loadPreset(file);
    CHECK(loaded.name == "roundtrip");
    CHECK(loaded.base.buttons[0].number == 61);
    CHECK_FALSE(std::filesystem::exists(dir / "roundtrip.json.tmp"));
    std::filesystem::remove_all(dir);

    CHECK_THROWS_AS(loadPreset(dir / "missing.json"), PresetError);
}
