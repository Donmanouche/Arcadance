#include "midipad/Labels.hpp"

#include <doctest/doctest.h>

using namespace midipad;

TEST_CASE("controls have PlayStation names") {
    CHECK(label(Button::South) == "Cross");
    CHECK(label(Button::LeftTrigger) == "L2");
    CHECK(label(Axis::LeftY) == "Left stick Y");
    CHECK(label(HatDir::UpRight) == "Up-right");
    CHECK(label(Zone::BottomLeft) == "Bottom left");
    for (int b = 0; b < kButtonCount; ++b)
        CHECK_FALSE(label(static_cast<Button>(b)).empty());
}

TEST_CASE("bindings are summarized in one line") {
    TriggerBinding b;
    CHECK(describe(b) == "-");

    b.type = ActionType::Note;
    b.number = 48;
    b.mode = Mode::Toggle;
    CHECK(describe(b) == "Note C3 (48), toggle");
    b.velocity = 90;
    b.mode = Mode::Momentary;
    CHECK(describe(b) == "Note C3 (48), vel. 90");

    b = {};
    b.type = ActionType::ControlChange;
    b.number = 45;
    b.onValue = 64;
    CHECK(describe(b) == "CC 45: 64 / 0");

    b = {};
    b.type = ActionType::Chord;
    b.chordName = "Cm9";
    CHECK(describe(b) == "Chord Cm9");
    b.chordName.clear();
    b.chord = *parseChord("C");
    CHECK(describe(b) == "Chord C4 E4 G4");

    b = {};
    b.type = ActionType::ChannelStep;
    b.channelStep = -1;
    CHECK(describe(b) == "Channel -1");
    b.channelStep = 1;
    CHECK(describe(b) == "Channel +1");

    b = {};
    b.type = ActionType::Transport;
    b.transport = TransportCommand::Stop;
    CHECK(describe(b) == "Transport: stop");

    b = {};
    b.type = ActionType::Freeze;
    b.freezeAxes = 0b11;
    CHECK(describe(b) == "Freeze: Left stick X, Left stick Y");
}

TEST_CASE("axis bindings are summarized in one line") {
    AxisBinding a;
    CHECK(describe(a) == "-");
    a.enabled = true;
    a.cc = 40;
    CHECK(describe(a) == "CC 40");
    a.min = 10;
    a.max = 90;
    a.invert = true;
    CHECK(describe(a) == "CC 40 (10-90), inverted");
}
