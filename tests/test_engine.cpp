#include "midipad/Engine.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <utility>
#include <vector>

using namespace midipad;

namespace {

struct RecordingSink : MidiSink {
    std::vector<MidiMessage> sent;
    void send(const MidiMessage& m) override { sent.push_back(m); }

    std::vector<MidiMessage> take() { return std::exchange(sent, {}); }
};

using Messages = std::vector<MidiMessage>;
constexpr auto on = MidiMessage::noteOn;
constexpr auto off = MidiMessage::noteOff;
constexpr auto cc = MidiMessage::controlChange;

TriggerBinding note(int number, Mode mode = Mode::Momentary) {
    TriggerBinding b;
    b.type = ActionType::Note;
    b.number = static_cast<std::uint8_t>(number);
    b.mode = mode;
    return b;
}

TriggerBinding chord(std::string_view name, Mode mode = Mode::Momentary) {
    TriggerBinding b;
    b.type = ActionType::Chord;
    b.chord = *parseChord(name);
    b.chordName = name;
    b.mode = mode;
    return b;
}

TriggerBinding control(int number, Mode mode = Mode::Momentary, int onValue = 127) {
    TriggerBinding b;
    b.type = ActionType::ControlChange;
    b.number = static_cast<std::uint8_t>(number);
    b.onValue = static_cast<std::uint8_t>(onValue);
    b.mode = mode;
    return b;
}

TriggerBinding channelStep(int step) {
    TriggerBinding b;
    b.type = ActionType::ChannelStep;
    b.channelStep = static_cast<std::int8_t>(step);
    return b;
}

TriggerBinding freeze(std::initializer_list<Axis> axes) {
    TriggerBinding b;
    b.type = ActionType::Freeze;
    for (Axis a : axes)
        b.freezeAxes = static_cast<std::uint8_t>(b.freezeAxes | (1u << static_cast<unsigned>(a)));
    return b;
}

AxisBinding axisCc(int number, bool invert = false, int min = 0, int max = 127) {
    return {true, static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(min), static_cast<std::uint8_t>(max),
            invert};
}

TriggerBinding& button(Preset& p, Button b) { return p.base.buttons[static_cast<std::size_t>(b)]; }
TriggerBinding& dpad(Preset& p, HatDir d) { return p.base.dpad[static_cast<std::size_t>(d)]; }

struct Fixture {
    RecordingSink sink;
    Engine engine{sink};

    void load(Preset p) { engine.setPreset(std::make_shared<const Preset>(std::move(p))); }
    void tap(Button b) {
        engine.buttonChanged(b, true);
        engine.buttonChanged(b, false);
    }
};

}  // namespace

TEST_CASE_FIXTURE(Fixture, "momentary note sounds while held") {
    Preset p;
    button(p, Button::South) = note(48);
    load(p);

    engine.buttonChanged(Button::South, true);
    CHECK(sink.take() == Messages{on(0, 48, 100)});
    engine.buttonChanged(Button::South, true);  // repeated press: ignored
    CHECK(sink.take().empty());
    engine.buttonChanged(Button::South, false);
    CHECK(sink.take() == Messages{off(0, 48)});
}

TEST_CASE_FIXTURE(Fixture, "toggled notes behave like radio buttons within the buttons") {
    Preset p;
    button(p, Button::South) = note(48, Mode::Toggle);
    button(p, Button::East) = note(51, Mode::Toggle);
    load(p);

    tap(Button::South);
    CHECK(sink.take() == Messages{on(0, 48, 100)});
    tap(Button::East);
    CHECK(sink.take() == Messages{off(0, 48), on(0, 51, 100)});
    tap(Button::East);
    CHECK(sink.take() == Messages{off(0, 51)});
}

TEST_CASE_FIXTURE(Fixture, "a toggled chord replaces a toggled note of the same family") {
    Preset p;
    button(p, Button::South) = note(48, Mode::Toggle);
    button(p, Button::North) = chord("Cm", Mode::Toggle);
    load(p);

    tap(Button::South);
    sink.take();
    tap(Button::North);
    CHECK(sink.take() == Messages{off(0, 48), on(0, 60, 100), on(0, 63, 100), on(0, 67, 100)});
}

TEST_CASE_FIXTURE(Fixture, "D-pad toggles do not cancel button toggles") {
    Preset p;
    button(p, Button::South) = note(48, Mode::Toggle);
    dpad(p, HatDir::Left) = chord("Cm", Mode::Toggle);
    load(p);

    tap(Button::South);
    tap(Button::DpadLeft);
    CHECK(sink.take() == Messages{on(0, 48, 100), on(0, 60, 100), on(0, 63, 100), on(0, 67, 100)});
}

TEST_CASE_FIXTURE(Fixture, "toggled CCs are independent") {
    Preset p;
    button(p, Button::LeftShoulder) = control(52, Mode::Toggle);
    button(p, Button::RightShoulder) = control(53, Mode::Toggle);
    load(p);

    tap(Button::LeftShoulder);
    tap(Button::RightShoulder);
    CHECK(sink.take() == Messages{cc(0, 52, 127), cc(0, 53, 127)});
    tap(Button::LeftShoulder);
    CHECK(sink.take() == Messages{cc(0, 52, 0)});
}

TEST_CASE_FIXTURE(Fixture, "a held note is released on the channel it started on") {
    Preset p;
    button(p, Button::South) = note(48);
    button(p, Button::Start) = channelStep(+1);
    load(p);

    engine.buttonChanged(Button::South, true);
    tap(Button::Start);
    CHECK(engine.channel() == 1);
    engine.buttonChanged(Button::South, false);
    CHECK(sink.take() == Messages{on(0, 48, 100), off(0, 48)});

    tap(Button::South);
    CHECK(sink.take() == Messages{on(1, 48, 100), off(1, 48)});
}

TEST_CASE_FIXTURE(Fixture, "channel steps wrap around") {
    Preset p;
    button(p, Button::Back) = channelStep(-1);
    load(p);
    tap(Button::Back);
    CHECK(engine.channel() == 15);
}

TEST_CASE_FIXTURE(Fixture, "each channel has its own toggle state and may have its own mapping") {
    Preset p;
    button(p, Button::South) = note(48, Mode::Toggle);
    button(p, Button::Start) = channelStep(+1);
    Mapping second = p.base;
    second.buttons[static_cast<std::size_t>(Button::South)] = chord("Cm");
    p.channels[1] = second;
    load(p);

    tap(Button::South);  // latched on channel 1
    tap(Button::Start);
    tap(Button::South);  // channel 2 mapping: a momentary chord
    CHECK(sink.take() == Messages{on(0, 48, 100), on(1, 60, 100), on(1, 63, 100), on(1, 67, 100), off(1, 60),
                                  off(1, 63), off(1, 67)});
}

TEST_CASE_FIXTURE(Fixture, "a held note survives a preset change and still stops") {
    Preset p;
    button(p, Button::South) = note(48);
    load(p);
    engine.buttonChanged(Button::South, true);

    Preset other;
    button(other, Button::South) = note(72);
    load(other);
    engine.buttonChanged(Button::South, false);
    CHECK(sink.take() == Messages{on(0, 48, 100), off(0, 48)});
}

TEST_CASE_FIXTURE(Fixture, "sticks send scaled CCs without repeating values") {
    Preset p;
    p.base.axes[static_cast<std::size_t>(Axis::LeftX)] = axisCc(40);
    p.base.axes[static_cast<std::size_t>(Axis::LeftY)] = axisCc(41, /*invert=*/true);
    p.base.axes[static_cast<std::size_t>(Axis::RightX)] = axisCc(42, false, 20, 100);
    load(p);

    engine.axisMoved(Axis::LeftX, 1.0f);
    engine.axisMoved(Axis::LeftX, 0.999f);  // same CC value
    engine.axisMoved(Axis::LeftX, -1.0f);
    engine.axisMoved(Axis::LeftY, -1.0f);  // stick pushed up, inverted: high
    engine.axisMoved(Axis::RightX, 0.0f);
    CHECK(sink.take() == Messages{cc(0, 40, 127), cc(0, 40, 0), cc(0, 41, 127), cc(0, 42, 60)});
}

TEST_CASE_FIXTURE(Fixture, "freeze holds the current value of its axes") {
    struct FreezeLog : EngineObserver {
        std::vector<int> masks;
        void freezeChanged(int, std::uint8_t axes) override { masks.push_back(axes); }
    } observer;
    engine.setObserver(&observer);
    Preset p;
    p.base.axes[static_cast<std::size_t>(Axis::LeftX)] = axisCc(40);
    button(p, Button::LeftStick) = freeze({Axis::LeftX, Axis::LeftY});
    load(p);

    engine.axisMoved(Axis::LeftX, 1.0f);
    tap(Button::LeftStick);
    CHECK(engine.isFrozen(Axis::LeftX));
    engine.axisMoved(Axis::LeftX, 0.0f);
    CHECK(sink.take() == Messages{cc(0, 40, 127)});

    tap(Button::LeftStick);
    CHECK_FALSE(engine.isFrozen(Axis::LeftX));
    engine.axisMoved(Axis::LeftX, 0.0f);
    CHECK(sink.take() == Messages{cc(0, 40, 64)});
    CHECK(observer.masks == std::vector{0b11, 0});
}

TEST_CASE_FIXTURE(Fixture, "triggers are both an axis and a digital button") {
    Preset p;
    p.base.axes[static_cast<std::size_t>(Axis::LeftTrigger)] = axisCc(50);
    button(p, Button::LeftTrigger) = note(63);
    load(p);

    engine.axisMoved(Axis::LeftTrigger, 0.3f);
    engine.axisMoved(Axis::LeftTrigger, 0.2f);  // within the hysteresis band
    engine.axisMoved(Axis::LeftTrigger, 0.1f);
    CHECK(sink.take() == Messages{on(0, 63, 100), cc(0, 50, 38), cc(0, 50, 25), off(0, 63), cc(0, 50, 13)});

    engine.buttonChanged(Button::LeftTrigger, true);  // only derived from the axis
    CHECK(sink.take().empty());
}

TEST_CASE_FIXTURE(Fixture, "the D-pad is read as an 8-way hat") {
    Preset p;
    dpad(p, HatDir::Up) = note(60);
    dpad(p, HatDir::UpRight) = note(61);
    dpad(p, HatDir::Right) = note(62);
    load(p);

    engine.buttonChanged(Button::DpadUp, true);
    engine.buttonChanged(Button::DpadRight, true);
    engine.buttonChanged(Button::DpadUp, false);
    engine.buttonChanged(Button::DpadRight, false);
    CHECK(sink.take() == Messages{on(0, 60, 100), off(0, 60), on(0, 61, 100), off(0, 61), on(0, 62, 100),
                                  off(0, 62)});
}

TEST_CASE_FIXTURE(Fixture, "a toggled D-pad direction stays on when the D-pad is released") {
    Preset p;
    dpad(p, HatDir::Up) = chord("Cm", Mode::Toggle);
    dpad(p, HatDir::Down) = control(45, Mode::Momentary, 64);
    load(p);

    tap(Button::DpadUp);
    tap(Button::DpadDown);
    CHECK(sink.take() == Messages{on(0, 60, 100), on(0, 63, 100), on(0, 67, 100), cc(0, 45, 64), cc(0, 45, 0)});
}

TEST_CASE_FIXTURE(Fixture, "touchpad zones act as four pads per finger") {
    Preset p;
    FingerBinding& f = p.base.fingers[0];
    f.mode = TouchMode::Zones;
    f.zones[static_cast<std::size_t>(Zone::TopLeft)] = note(48);
    f.zones[static_cast<std::size_t>(Zone::TopRight)] = note(49);
    load(p);

    engine.touchMoved(0, 0.2f, 0.2f);
    engine.touchMoved(0, 0.3f, 0.3f);  // same zone
    engine.touchMoved(0, 0.8f, 0.2f);
    engine.touchReleased(0);
    CHECK(sink.take() == Messages{on(0, 48, 100), off(0, 48), on(0, 49, 100), off(0, 49)});
}

TEST_CASE_FIXTURE(Fixture, "touchpad axis mode sends X and Y with Y growing upwards") {
    Preset p;
    FingerBinding& f = p.base.fingers[1];
    f.mode = TouchMode::Axis;
    f.x = axisCc(62);
    f.y = axisCc(63);
    load(p);

    engine.touchMoved(1, 0.0f, 0.0f);
    engine.touchMoved(1, 1.0f, 1.0f);
    CHECK(sink.take() == Messages{cc(0, 62, 0), cc(0, 63, 127), cc(0, 62, 127), cc(0, 63, 0)});
}

TEST_CASE_FIXTURE(Fixture, "transport sends realtime messages") {
    Preset p;
    button(p, Button::Start).type = ActionType::Transport;
    button(p, Button::Back).type = ActionType::Transport;
    button(p, Button::Back).transport = TransportCommand::Stop;
    load(p);

    tap(Button::Start);
    tap(Button::Back);
    CHECK(sink.take() == Messages{MidiMessage::realtime(0xFA), MidiMessage::realtime(0xFC)});
}

TEST_CASE_FIXTURE(Fixture, "releaseAll turns off everything still sounding") {
    Preset p;
    button(p, Button::South) = note(48, Mode::Toggle);
    button(p, Button::East) = note(50);
    button(p, Button::LeftShoulder) = control(52, Mode::Toggle);
    load(p);

    tap(Button::South);
    tap(Button::LeftShoulder);
    engine.buttonChanged(Button::East, true);
    sink.take();
    engine.releaseAll();
    auto sent = sink.take();
    CHECK(sent.size() == 3);
    CHECK(std::find(sent.begin(), sent.end(), off(0, 48)) != sent.end());
    CHECK(std::find(sent.begin(), sent.end(), off(0, 50)) != sent.end());
    CHECK(std::find(sent.begin(), sent.end(), cc(0, 52, 0)) != sent.end());
}

TEST_CASE_FIXTURE(Fixture, "reset forgets held controls so a reconnected controller starts clean") {
    Preset p;
    button(p, Button::South) = note(48);
    dpad(p, HatDir::Up) = note(60);
    load(p);

    engine.buttonChanged(Button::South, true);
    engine.buttonChanged(Button::DpadUp, true);
    sink.take();
    engine.reset();
    CHECK(sink.take() == Messages{off(0, 48), off(0, 60)});

    engine.buttonChanged(Button::South, false);  // stale release: nothing left to stop
    CHECK(sink.take().empty());
    engine.buttonChanged(Button::South, true);
    engine.buttonChanged(Button::DpadUp, true);
    CHECK(sink.take() == Messages{on(0, 48, 100), on(0, 60, 100)});
}

TEST_CASE_FIXTURE(Fixture, "sounding slots show held and latched controls of the active channel") {
    Preset p;
    button(p, Button::South) = note(48, Mode::Toggle);
    button(p, Button::East) = note(50);
    dpad(p, HatDir::Up) = chord("Cm");
    button(p, Button::Start) = channelStep(+1);
    load(p);
    auto bit = [](int slot) { return std::uint64_t{1} << slot; };

    tap(Button::South);  // latched
    engine.buttonChanged(Button::East, true);
    engine.buttonChanged(Button::DpadUp, true);
    CHECK(engine.soundingSlots() ==
          (bit(Engine::slotOf(Button::South)) | bit(Engine::slotOf(Button::East)) | bit(Engine::slotOf(HatDir::Up))));

    engine.buttonChanged(Button::East, false);
    engine.buttonChanged(Button::DpadUp, false);
    CHECK(engine.soundingSlots() == bit(Engine::slotOf(Button::South)));

    tap(Button::Start);  // channel 2: the latch belongs to channel 1
    CHECK(engine.soundingSlots() == 0);
}
