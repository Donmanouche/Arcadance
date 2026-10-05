#include "midipad/Engine.hpp"
#include "midipad/LegacyImport.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>

using namespace midipad;
using Json = nlohmann::ordered_json;

namespace {

const std::filesystem::path kLegacyDir = std::filesystem::path(MIDIPAD_FIXTURES) / "legacy";

LegacyImportResult find(const LegacyDirectoryImport& imported, const std::string& name) {
    auto it = std::find_if(imported.presets.begin(), imported.presets.end(),
                           [&](const auto& r) { return r.preset.name == name; });
    REQUIRE(it != imported.presets.end());
    return *it;
}

const TriggerBinding& button(const Mapping& m, Button b) { return m.buttons[static_cast<std::size_t>(b)]; }
const AxisBinding& axis(const Mapping& m, Axis a) { return m.axes[static_cast<std::size_t>(a)]; }

}  // namespace

TEST_CASE("channel files are grouped with their base preset") {
    const auto imported = importLegacyDirectory(kLegacyDir);
    CHECK(imported.errors.empty());
    REQUIRE(imported.presets.size() == 3);

    const Preset p = find(imported, "PS5_Test").preset;
    for (int c = 0; c < kChannelCount; ++c) {
        CAPTURE(c);
        CHECK(p.channels[static_cast<std::size_t>(c)].has_value() == (c == 1 || c == 15));
    }
}

TEST_CASE("PS5_Test converts without loss") {
    const auto imported = importLegacyDirectory(kLegacyDir);
    const auto result = find(imported, "PS5_Test");
    CHECK(result.warnings.empty());
    const Mapping& m = result.preset.base;

    CHECK(button(m, Button::South).number == 48);
    CHECK(button(m, Button::South).mode == Mode::Toggle);
    CHECK(button(m, Button::East).number == 51);
    CHECK(button(m, Button::North).number == 55);
    CHECK(button(m, Button::West).number == 58);
    CHECK(button(m, Button::LeftShoulder).type == ActionType::ControlChange);
    CHECK(button(m, Button::LeftShoulder).number == 52);
    CHECK(button(m, Button::RightShoulder).number == 53);
    CHECK(button(m, Button::LeftTrigger).number == 63);
    CHECK(button(m, Button::LeftTrigger).mode == Mode::Momentary);
    CHECK(button(m, Button::RightTrigger).number == 50);
    CHECK(button(m, Button::Back).channelStep == -1);
    CHECK(button(m, Button::Start).channelStep == 1);
    CHECK(button(m, Button::LeftStick).freezeAxes == 0b000011);   // left_x, left_y
    CHECK(button(m, Button::RightStick).freezeAxes == 0b001100);  // right_x, right_y

    CHECK(axis(m, Axis::LeftX).cc == 40);
    CHECK(axis(m, Axis::LeftY).cc == 41);
    CHECK(axis(m, Axis::LeftY).invert);
    CHECK(axis(m, Axis::RightX).cc == 42);
    CHECK(axis(m, Axis::RightY).cc == 43);
    CHECK(axis(m, Axis::RightY).invert);
    CHECK_FALSE(axis(m, Axis::RightX).invert);
    CHECK(axis(m, Axis::LeftTrigger).cc == 50);
    CHECK(axis(m, Axis::RightTrigger).cc == 51);

    const auto& up = m.dpad[static_cast<std::size_t>(HatDir::Up)];
    CHECK(up.type == ActionType::Chord);
    CHECK(up.chordName == "Ebmaj11");
    CHECK(up.mode == Mode::Toggle);
    CHECK(m.dpad[static_cast<std::size_t>(HatDir::DownRight)].onValue == 64);
    CHECK(m.dpad[static_cast<std::size_t>(HatDir::DownLeft)].number == 62);

    CHECK(m.fingers[0].mode == TouchMode::Axis);
    CHECK(m.fingers[0].x.cc == 60);
    CHECK(m.fingers[0].y.cc == 61);
    CHECK(m.fingers[1].x.cc == 62);

    const Mapping& ch2 = *result.preset.channels[1];
    CHECK(button(ch2, Button::South).chordName == "Cm9");
    CHECK(ch2.dpad[static_cast<std::size_t>(HatDir::Left)].chordName == "Cm");
    CHECK(ch2.dpad[static_cast<std::size_t>(HatDir::Right)].chordName == "Cm9");

    const Mapping& ch16 = *result.preset.channels[15];
    CHECK(ch16.fingers[0].mode == TouchMode::Zones);
    CHECK(ch16.fingers[0].zones[static_cast<std::size_t>(Zone::TopRight)].number == 49);
}

TEST_CASE("an imported preset plays: channel 2 switches the face buttons to chords") {
    struct Sink : MidiSink {
        std::vector<MidiMessage> sent;
        void send(const MidiMessage& m) override { sent.push_back(m); }
    } sink;
    Engine engine(sink);
    const auto imported = importLegacyDirectory(kLegacyDir);
    engine.setPreset(std::make_shared<const Preset>(find(imported, "PS5_Test").preset));

    engine.buttonChanged(Button::Start, true);  // Options: next channel
    engine.buttonChanged(Button::Start, false);
    engine.buttonChanged(Button::South, true);  // Cross: Cm9
    CHECK(engine.channel() == 1);
    CHECK(sink.sent.size() == 5);
    CHECK(sink.sent.front() == MidiMessage::noteOn(1, 60, 100));
}

TEST_CASE("unknown entries are reported, not silently dropped") {
    const Json legacy = Json::parse(R"({
        "mappings": {
            "button": {"20": {"midi_type": "note", "midi_id": 40}, "0": {"midi_type": "note", "midi_id": "E4"}},
            "axis": {"0": {"midi_type": "note", "midi_id": 40}},
            "hat": {"1": {"up": {"midi_type": "note", "midi_id": 40}}},
            "button_extra": {}
        }
    })");
    const auto result = importLegacyPreset("odd", legacy, {});
    CHECK(result.preset.base.buttons[0].number == 64);
    CHECK(result.warnings.size() == 5);
}
