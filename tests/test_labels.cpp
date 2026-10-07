#include "midipad/Labels.hpp"

#include <doctest/doctest.h>

using namespace midipad;

TEST_CASE("controls have PlayStation and French names") {
    CHECK(label(Button::South) == "Croix");
    CHECK(label(Button::LeftTrigger) == "L2");
    CHECK(label(Axis::LeftY) == "Stick gauche Y");
    CHECK(label(HatDir::UpRight) == "Haut-droite");
    CHECK(label(Zone::BottomLeft) == "Bas gauche");
    for (int b = 0; b < kButtonCount; ++b)
        CHECK_FALSE(label(static_cast<Button>(b)).empty());
}

TEST_CASE("bindings are summarized in one line") {
    TriggerBinding b;
    CHECK(describe(b) == "-");

    b.type = ActionType::Note;
    b.number = 48;
    b.mode = Mode::Toggle;
    CHECK(describe(b) == "Note C3 (48), bascule");
    b.velocity = 90;
    b.mode = Mode::Momentary;
    CHECK(describe(b) == "Note C3 (48), vél. 90");

    b = {};
    b.type = ActionType::ControlChange;
    b.number = 45;
    b.onValue = 64;
    CHECK(describe(b) == "CC 45 : 64 / 0");

    b = {};
    b.type = ActionType::Chord;
    b.chordName = "Cm9";
    CHECK(describe(b) == "Accord Cm9");
    b.chordName.clear();
    b.chord = *parseChord("C");
    CHECK(describe(b) == "Accord C4 E4 G4");

    b = {};
    b.type = ActionType::ChannelStep;
    b.channelStep = -1;
    CHECK(describe(b) == "Canal -1");
    b.channelStep = 1;
    CHECK(describe(b) == "Canal +1");

    b = {};
    b.type = ActionType::Transport;
    b.transport = TransportCommand::Stop;
    CHECK(describe(b) == "Transport : arrêt");

    b = {};
    b.type = ActionType::Freeze;
    b.freezeAxes = 0b11;
    CHECK(describe(b) == "Freeze : Stick gauche X, Stick gauche Y");
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
    CHECK(describe(a) == "CC 40 (10-90), inversé");
}
