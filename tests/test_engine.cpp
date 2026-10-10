#include "midipad/Engine.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <string>
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

TEST_CASE_FIXTURE(Fixture, "releasing a D-pad diagonal does not play the direction in between") {
    Preset p;
    dpad(p, HatDir::Left) = note(60);
    dpad(p, HatDir::DownLeft) = note(61);
    dpad(p, HatDir::Down) = note(62);
    load(p);
    engine.setDpadSettle(15, 15);
    constexpr std::uint64_t ms = 1'000'000;

    engine.tick(0);
    engine.buttonChanged(Button::DpadDown, true);
    engine.tick(5 * ms);
    engine.buttonChanged(Button::DpadLeft, true);  // Down never settled
    engine.tick(19 * ms);
    CHECK(sink.take().empty());
    engine.tick(20 * ms);
    CHECK(sink.take() == Messages{on(0, 61, 100)});

    engine.tick(100 * ms);
    engine.buttonChanged(Button::DpadDown, false);
    engine.tick(110 * ms);
    engine.buttonChanged(Button::DpadLeft, false);  // Left never settled: released at once
    engine.tick(200 * ms);
    CHECK(sink.take() == Messages{off(0, 61)});
}

TEST_CASE_FIXTURE(Fixture, "a D-pad direction held past the settle time plays") {
    Preset p;
    dpad(p, HatDir::Left) = note(60);
    dpad(p, HatDir::DownLeft) = note(61);
    load(p);
    engine.setDpadSettle(15, 30);
    constexpr std::uint64_t ms = 1'000'000;

    engine.tick(0);
    engine.buttonChanged(Button::DpadDown, true);
    engine.buttonChanged(Button::DpadLeft, true);
    engine.tick(15 * ms);
    engine.buttonChanged(Button::DpadDown, false);
    engine.tick(44 * ms);
    CHECK(sink.take() == Messages{on(0, 61, 100)});
    engine.tick(45 * ms);
    CHECK(sink.take() == Messages{off(0, 61), on(0, 60, 100)});
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

namespace {

Humanize humanizeChannel1(int velocityMin, int velocityMax, int delayMin, int delayMax) {
    Humanize h;
    h.channels = 1;  // channel 1 only
    h.velocityMinPercent = velocityMin;
    h.velocityMaxPercent = velocityMax;
    h.delayMinMs = delayMin;
    h.delayMaxMs = delayMax;
    return h;
}

constexpr std::uint64_t kMs = 1'000'000;

}  // namespace

TEST_CASE_FIXTURE(Fixture, "humanization scales the velocity on the chosen channels only") {
    Preset p;
    button(p, Button::South) = note(60);
    load(p);
    engine.setHumanize(humanizeChannel1(50, 50, 0, 0));

    tap(Button::South);
    CHECK(sink.take() == Messages{on(0, 60, 50), off(0, 60)});

    engine.setChannel(1);  // channel 2: not humanized
    tap(Button::South);
    CHECK(sink.take() == Messages{on(1, 60, 100), off(1, 60)});
}

TEST_CASE_FIXTURE(Fixture, "a mapping can opt out of humanization") {
    Preset p;
    button(p, Button::South) = note(60);
    button(p, Button::South).humanize = false;
    load(p);
    engine.setHumanize(humanizeChannel1(10, 10, 200, 200));

    tap(Button::South);
    CHECK(sink.take() == Messages{on(0, 60, 100), off(0, 60)});
}

TEST_CASE_FIXTURE(Fixture, "a humanized note starts late and keeps its length") {
    Preset p;
    button(p, Button::North) = chord("C");
    load(p);
    engine.setHumanize(humanizeChannel1(100, 100, 100, 100));

    engine.tick(0);
    engine.buttonChanged(Button::North, true);
    engine.tick(99 * kMs);
    CHECK(sink.take().empty());
    engine.tick(100 * kMs);
    CHECK(sink.take() == Messages{on(0, 60, 100), on(0, 64, 100), on(0, 67, 100)});

    engine.tick(150 * kMs);
    engine.buttonChanged(Button::North, false);
    CHECK(sink.take().empty());
    engine.tick(250 * kMs);
    CHECK(sink.take() == Messages{off(0, 60), off(0, 64), off(0, 67)});
}

TEST_CASE_FIXTURE(Fixture, "each note of a humanized chord gets its own velocity and delay, within the bounds") {
    Preset p;
    button(p, Button::North) = chord("Cmaj9");
    load(p);
    engine.setHumanize(humanizeChannel1(40, 80, 0, 300));

    bool velocitiesDiffer = false;
    std::uint64_t t = 0;
    for (int round = 0; round < 20; ++round) {
        engine.tick(t);
        engine.buttonChanged(Button::North, true);
        std::vector<int> velocities;
        for (int ms = 0; ms <= 300; ++ms) {
            engine.tick(t + static_cast<std::uint64_t>(ms) * kMs);
            for (const auto& m : sink.take()) {
                velocities.push_back(m.bytes[2]);
                CHECK(m.bytes[2] >= 40);
                CHECK(m.bytes[2] <= 80);
            }
        }
        CHECK(velocities.size() == 5);
        velocitiesDiffer |= std::adjacent_find(velocities.begin(), velocities.end(), std::not_equal_to<>()) !=
                            velocities.end();
        engine.buttonChanged(Button::North, false);
        t += 1000 * kMs;
        engine.tick(t);
        CHECK(sink.take().size() == 5);
    }
    CHECK(velocitiesDiffer);
}

TEST_CASE_FIXTURE(Fixture, "panic drops the humanized notes not started yet") {
    Preset p;
    button(p, Button::North) = note(60);
    button(p, Button::South) = note(62);
    load(p);
    engine.setHumanize(humanizeChannel1(100, 100, 100, 100));

    engine.tick(0);
    engine.buttonChanged(Button::South, true);
    engine.tick(100 * kMs);
    CHECK(sink.take() == Messages{on(0, 62, 100)});
    engine.buttonChanged(Button::North, true);  // due at 200 ms
    engine.releaseAll();
    CHECK(sink.take() == Messages{off(0, 62), off(0, 60)});  // slot order; 60 never started
    engine.tick(300 * kMs);
    CHECK(sink.take().empty());
}

TEST_CASE_FIXTURE(Fixture, "a note played again ends its previous instance first") {
    Preset p;
    button(p, Button::South) = note(60);
    load(p);
    Humanize h = humanizeChannel1(100, 100, 100, 100);
    engine.setHumanize(h);

    engine.tick(0);
    engine.buttonChanged(Button::South, true);
    engine.tick(100 * kMs);
    engine.buttonChanged(Button::South, false);  // off due at 200 ms
    h.delayMinMs = h.delayMaxMs = 0;
    engine.setHumanize(h);
    engine.buttonChanged(Button::South, true);  // starts now
    CHECK(sink.take() == Messages{on(0, 60, 100), off(0, 60), on(0, 60, 100)});
    engine.tick(300 * kMs);
    CHECK(sink.take().empty());
}

namespace {

struct LoopFixture : Fixture {
    std::uint64_t t = 0;

    LoopFixture() {
        Preset p;
        button(p, Button::South).type = ActionType::LoopRecord;
        p.base.axes[static_cast<std::size_t>(Axis::LeftX)] = axisCc(40);
        p.base.axes[static_cast<std::size_t>(Axis::RightX)] = axisCc(42);
        load(p);
    }
    void setPlayback(LoopPlayback playback) {
        Preset p = engine.preset();
        p.base.loops[0].playback = playback;
        load(p);
    }
    void step() {
        t += kMs * 5;
        engine.tick(t);
    }
    // Records the left stick going to the right, back to the center, then left.
    void recordLeft(Axis axis = Axis::LeftX) {
        engine.tick(t);
        engine.buttonChanged(Button::South, true);
        for (float v : {1.0f, 0.0f, -1.0f}) {
            engine.axisMoved(axis, v);
            step();
        }
        engine.buttonChanged(Button::South, false);
        sink.take();
    }
    // The values sent on `cc` over the next `steps` loop steps.
    std::vector<int> played(int controller, int steps) {
        std::vector<int> values;
        for (int i = 0; i < steps; ++i) {
            step();
            for (const auto& m : sink.take())
                if ((m.bytes[0] & 0xF0) == 0xB0 && m.bytes[1] == controller)
                    values.push_back(m.bytes[2]);
        }
        return values;
    }
};

}  // namespace

TEST_CASE_FIXTURE(LoopFixture, "Record captures the stick that moves and loops it on the loop CCs") {
    recordLeft();
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(0, 0).length == 3);
    CHECK(engine.loopStatus(0, 1).state == LoopStatus::State::Empty);  // the right stick did not move
    const auto values = played(70, 6);
    REQUIRE(values.size() >= 6);
    CHECK(std::vector(values.begin(), values.begin() + 6) == std::vector<int>{127, 64, 0, 127, 64, 0});
}

TEST_CASE_FIXTURE(LoopFixture, "the stick keeps sending its own CC while its loop plays") {
    recordLeft();
    engine.axisMoved(Axis::LeftX, 0.5f);
    bool stickSent = false;
    for (const auto& m : sink.take())
        stickSent |= m.bytes[1] == 40;
    CHECK(stickSent);
    CHECK_FALSE(played(70, 3).empty());
}

TEST_CASE_FIXTURE(LoopFixture, "loops play in reverse or back and forth") {
    setPlayback(LoopPlayback::Reverse);
    recordLeft();
    auto values = played(70, 6);
    REQUIRE(values.size() >= 6);
    CHECK(std::vector(values.begin(), values.begin() + 6) == std::vector<int>{0, 64, 127, 0, 64, 127});

    setPlayback(LoopPlayback::BackAndForth);
    recordLeft();
    values = played(70, 7);
    REQUIRE(values.size() >= 7);
    CHECK(std::vector(values.begin(), values.begin() + 7) == std::vector<int>{127, 64, 0, 64, 127, 64, 0});
}

TEST_CASE_FIXTURE(LoopFixture, "recording the other stick keeps the first loop") {
    recordLeft();
    recordLeft(Axis::RightX);
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(0, 1).state == LoopStatus::State::Playing);
    CHECK_FALSE(played(72, 3).empty());
}

TEST_CASE_FIXTURE(LoopFixture, "pressing Record without moving a stick stops the loops") {
    recordLeft();
    engine.buttonChanged(Button::South, true);
    step();
    engine.buttonChanged(Button::South, false);
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Empty);
    sink.take();
    CHECK(played(70, 4).empty());
}

TEST_CASE_FIXTURE(LoopFixture, "a small wobble of a resting stick is not a movement") {
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.axisMoved(Axis::LeftX, 0.08f);  // 4 % of the travel
    step();
    CHECK(engine.loopStatus(0, 0).waiting);
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Empty);
    engine.buttonChanged(Button::South, false);
}

TEST_CASE_FIXTURE(LoopFixture, "each channel has its own loops, and they all play") {
    recordLeft();                // channel 1
    engine.setChannel(1);
    recordLeft();                // channel 2: a second left loop, the first keeps playing
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(1, 0).state == LoopStatus::State::Playing);
    bool channel1 = false, channel2 = false;
    for (int i = 0; i < 4; ++i) {
        step();
        for (const auto& m : sink.take()) {
            channel1 |= m.bytes[0] == 0xB0 && m.bytes[1] == 70;
            channel2 |= m.bytes[0] == 0xB1 && m.bytes[1] == 70;
        }
    }
    CHECK(channel1);
    CHECK(channel2);

    // A press without movement stops the active channel's loops only.
    engine.buttonChanged(Button::South, true);
    step();
    engine.buttonChanged(Button::South, false);
    CHECK(engine.loopStatus(1, 0).state == LoopStatus::State::Empty);
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Playing);

    engine.stopLoops();  // Panic: every channel
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Empty);
}

namespace {

struct FingerLoopFixture : LoopFixture {
    explicit FingerLoopFixture(TouchMode mode = TouchMode::Axis) {
        Preset p = engine.preset();
        FingerBinding& f = p.base.fingers[0];
        f.mode = mode;
        f.x = axisCc(60);
        f.y = axisCc(61);
        load(p);
    }
};

struct ZonesFixture : FingerLoopFixture {
    ZonesFixture() : FingerLoopFixture(TouchMode::Zones) {}
};

}  // namespace

TEST_CASE_FIXTURE(FingerLoopFixture, "a finger is recorded with the moments it was lifted, which play nothing") {
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.touchMoved(0, 1.0f, 0.0f);  // lands: that is a movement
    step();
    engine.touchReleased(0);
    step();
    engine.touchMoved(0, 0.0f, 1.0f);
    step();
    engine.buttonChanged(Button::South, false);
    sink.take();
    CHECK(engine.loopStatus(0, 2).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(0, 2).length == 3);
    CHECK(engine.loopSample(0, 2, 0, 1) < 0.0f);  // lifted
    // Finger loop CCs 74 (X) and 75 (Y, upwards): right/top, nothing, left/bottom.
    const auto x = played(74, 6);
    REQUIRE(x.size() >= 4);
    CHECK(std::vector(x.begin(), x.begin() + 4) == std::vector<int>{127, 0, 127, 0});
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Empty);  // the sticks did not move
}

TEST_CASE_FIXTURE(FingerLoopFixture, "a stick and a finger moved together get a loop each") {
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.axisMoved(Axis::LeftX, 1.0f);
    engine.touchMoved(0, 0.2f, 0.2f);
    step();
    engine.axisMoved(Axis::LeftX, -1.0f);
    engine.touchMoved(0, 0.8f, 0.8f);
    step();
    engine.buttonChanged(Button::South, false);
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(0, 2).state == LoopStatus::State::Playing);
}

namespace {

// The notes sent over `steps` loop steps, as "+60" (on) and "-60" (off).
std::vector<std::string> notesOver(LoopFixture& fx, int steps) {
    std::vector<std::string> out;
    for (int i = 0; i < steps; ++i) {
        fx.step();
        for (const auto& m : fx.sink.take()) {
            const int kind = m.bytes[0] & 0xF0;
            if (kind == 0x90 || kind == 0x80)
                out.push_back((kind == 0x90 ? "+" : "-") + std::to_string(m.bytes[1]));
        }
    }
    return out;
}

}  // namespace

TEST_CASE_FIXTURE(ZonesFixture, "the pads a finger plays in 4-pad mode go into the note loop") {
    Preset p = engine.preset();
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::TopLeft)] = note(60);
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::BottomRight)] = note(64);
    load(p);

    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.touchMoved(0, 0.2f, 0.2f);  // top left
    step();
    engine.touchMoved(0, 0.8f, 0.8f);  // bottom right
    step();
    engine.touchReleased(0);
    step();
    engine.buttonChanged(Button::South, false);
    sink.take();
    constexpr int kNoteLoop = static_cast<int>(LoopSource::Notes);
    REQUIRE(engine.loopStatus(0, kNoteLoop).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(0, 2).state == LoopStatus::State::Empty);  // no X/Y loop for the finger
    NoteInterval intervals[8];
    CHECK(engine.noteLoopIntervals(0, intervals, 8) == 2);
    const auto notes = notesOver(*this, 6);
    REQUIRE(notes.size() >= 6);
    CHECK(std::vector(notes.begin(), notes.begin() + 6) == std::vector<std::string>{"+60", "-60", "+64", "-64", "+60", "-60"});
}

TEST_CASE_FIXTURE(ZonesFixture, "a toggled pad of the note loop is cut when it starts over") {
    Preset p = engine.preset();
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::TopLeft)] = note(60, Mode::Toggle);
    load(p);

    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.touchMoved(0, 0.2f, 0.2f);
    step();
    step();
    engine.touchReleased(0);
    step();
    engine.buttonChanged(Button::South, false);
    sink.take();
    // Latched on, then cut as the loop starts over, then on again.
    const auto notes = notesOver(*this, 5);
    REQUIRE(notes.size() >= 3);
    CHECK(std::vector(notes.begin(), notes.begin() + 3) == std::vector<std::string>{"+60", "-60", "+60"});

    // Panic while it is on turns it off.
    for (int i = 0; i < 10 && notesOver(*this, 1) != std::vector<std::string>{"+60"}; ++i) {
    }
    engine.stopLoops();
    CHECK(sink.take() == Messages{off(0, 60)});
}

TEST_CASE_FIXTURE(LoopFixture, "the triggers' travel is recorded and loops on its own CCs, without the button notes") {
    Preset p = engine.preset();
    button(p, Button::LeftTrigger) = note(60);  // the digital L2 plays a note: the loop must not
    load(p);
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    for (float v : {1.0f, 0.5f, 0.0f}) {
        engine.axisMoved(Axis::LeftTrigger, v);
        step();
    }
    engine.buttonChanged(Button::South, false);
    engine.clearLoop(0, static_cast<int>(LoopSource::Notes));  // L2's button note went into the note loop
    sink.take();
    CHECK(engine.loopStatus(0, static_cast<int>(LoopSource::Triggers)).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Empty);
    bool notes = false;
    std::vector<int> l2;
    for (int i = 0; i < 6; ++i) {
        step();
        for (const auto& m : sink.take()) {
            notes |= (m.bytes[0] & 0xF0) == 0x90;
            if ((m.bytes[0] & 0xF0) == 0xB0 && m.bytes[1] == 78)
                l2.push_back(m.bytes[2]);
        }
    }
    CHECK_FALSE(notes);
    REQUIRE(l2.size() >= 3);
    CHECK(std::vector(l2.begin(), l2.begin() + 3) == std::vector<int>{127, 64, 0});
}

namespace {

constexpr int kNotes = static_cast<int>(LoopSource::Notes);

struct NoteLoopFixture : LoopFixture {
    NoteLoopFixture() {
        Preset p = engine.preset();
        button(p, Button::North) = note(60);
        button(p, Button::East) = chord("C");
        button(p, Button::West) = note(62, Mode::Toggle);
        load(p);
    }
    void hold(Button b, int steps) {
        engine.buttonChanged(b, true);
        for (int i = 0; i < steps; ++i)
            step();
        engine.buttonChanged(b, false);
    }
};

}  // namespace

TEST_CASE_FIXTURE(NoteLoopFixture, "notes played while Record is held loop on their channel") {
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    hold(Button::North, 2);  // starts the recording
    step();
    hold(Button::East, 2);   // a chord
    step();
    engine.buttonChanged(Button::South, false);
    sink.take();
    REQUIRE(engine.loopStatus(0, kNotes).state == LoopStatus::State::Playing);
    NoteInterval notes[16];
    CHECK(engine.noteLoopIntervals(0, notes, 16) == 4);  // 60, then C E G
    const auto played = notesOver(*this, 7);
    REQUIRE(played.size() >= 8);
    CHECK(std::vector(played.begin(), played.begin() + 8) ==
          std::vector<std::string>{"+60", "-60", "+60", "+64", "+67", "-60", "-64", "-67"});
}

TEST_CASE_FIXTURE(NoteLoopFixture, "a note loop plays backwards in reverse") {
    Preset p = engine.preset();
    p.base.loops[kNotes].playback = LoopPlayback::Reverse;
    load(p);
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    hold(Button::North, 1);
    step();
    engine.buttonChanged(Button::West, true);  // toggled on: it rings to the end of the loop
    step();
    engine.buttonChanged(Button::West, false);
    step();
    engine.buttonChanged(Button::South, false);
    sink.take();
    const auto played = notesOver(*this, 6);
    REQUIRE(played.size() >= 2);
    CHECK(played[0] == "+62");  // the end comes first, the held note sounding from there
}

TEST_CASE_FIXTURE(NoteLoopFixture, "a note still on when the note loop starts over is cut") {
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.buttonChanged(Button::West, true);  // toggled on, never off during the recording
    step();
    engine.buttonChanged(Button::West, false);
    step();
    step();
    engine.buttonChanged(Button::South, false);
    sink.take();
    const auto played = notesOver(*this, 4);
    REQUIRE(played.size() >= 3);
    CHECK(std::vector(played.begin(), played.begin() + 3) == std::vector<std::string>{"+62", "-62", "+62"});
}

TEST_CASE_FIXTURE(ZonesFixture, "pads are recorded into the note loop, with the buttons' notes") {
    Preset p = engine.preset();
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::TopLeft)] = note(60);
    load(p);
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.touchMoved(0, 0.2f, 0.2f);
    step();
    engine.touchReleased(0);
    step();
    engine.buttonChanged(Button::South, false);
    CHECK(engine.loopStatus(0, kNotes).state == LoopStatus::State::Playing);
    CHECK(engine.loopStatus(0, 2).state == LoopStatus::State::Empty);
}

namespace {

// A note loop of `steps` steps on channel 1: North held for its first step.
void recordShortLoop(NoteLoopFixture& fx, int steps) {
    fx.engine.tick(fx.t);
    fx.engine.buttonChanged(Button::South, true);
    fx.hold(Button::North, 1);
    for (int i = 1; i < steps; ++i)
        fx.step();
    fx.engine.buttonChanged(Button::South, false);
    fx.sink.take();
}

void tapRecord(NoteLoopFixture& fx) {
    fx.engine.buttonChanged(Button::South, true);
    fx.step();
    fx.engine.buttonChanged(Button::South, false);
}

}  // namespace

TEST_CASE_FIXTURE(NoteLoopFixture, "a tap of Record over a playing note loop turns overdub on and off") {
    recordShortLoop(*this, 8);
    REQUIRE(engine.loopStatus(0, kNotes).state == LoopStatus::State::Playing);
    tapRecord(*this);
    CHECK(engine.loopStatus(0, kNotes).overdub);
    CHECK(engine.loopStatus(0, kNotes).state == LoopStatus::State::Playing);  // not stopped
    tapRecord(*this);
    CHECK_FALSE(engine.loopStatus(0, kNotes).overdub);
}

TEST_CASE_FIXTURE(NoteLoopFixture, "notes played while overdubbing join the note loop") {
    recordShortLoop(*this, 8);
    tapRecord(*this);
    NoteInterval notes[16];
    REQUIRE(engine.noteLoopIntervals(0, notes, 16) == 1);
    hold(Button::East, 1);  // a C chord, added where the loop plays
    CHECK(engine.noteLoopIntervals(0, notes, 16) == 4);
    // It plays again on the next turn, from the loop.
    bool replayed = false;
    for (int i = 0; i < 10; ++i) {
        step();
        for (const auto& m : sink.take())
            replayed |= (m.bytes[0] & 0xF0) == 0x90 && m.bytes[1] == 67;
    }
    CHECK(replayed);
}

TEST_CASE_FIXTURE(NoteLoopFixture, "a long press without playing stops the loops, overdub included") {
    recordShortLoop(*this, 8);
    tapRecord(*this);
    engine.buttonChanged(Button::South, true);
    for (int i = 0; i < 110; ++i)  // 550 ms
        step();
    engine.buttonChanged(Button::South, false);
    CHECK(engine.loopStatus(0, kNotes).state == LoopStatus::State::Empty);
    CHECK_FALSE(engine.loopStatus(0, kNotes).overdub);
}

TEST_CASE_FIXTURE(NoteLoopFixture, "recording notes again replaces them and ends the overdub") {
    recordShortLoop(*this, 8);
    tapRecord(*this);
    engine.buttonChanged(Button::South, true);
    hold(Button::East, 2);
    step();
    engine.buttonChanged(Button::South, false);
    NoteInterval notes[16];
    CHECK(engine.noteLoopIntervals(0, notes, 16) == 3);  // only the chord
    CHECK_FALSE(engine.loopStatus(0, kNotes).overdub);
}

TEST_CASE_FIXTURE(NoteLoopFixture, "a tap with no note loop still stops the other loops") {
    recordLeft();
    tapRecord(*this);
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Empty);
}

TEST_CASE_FIXTURE(ZonesFixture, "pads played while overdubbing join the note loop") {
    Preset p = engine.preset();
    button(p, Button::North) = note(60);
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::TopLeft)] = note(72);
    load(p);
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.buttonChanged(Button::North, true);
    step();
    engine.buttonChanged(Button::North, false);
    for (int i = 0; i < 6; ++i)
        step();
    engine.buttonChanged(Button::South, false);
    engine.buttonChanged(Button::South, true);  // tap: overdub
    step();
    engine.buttonChanged(Button::South, false);
    REQUIRE(engine.loopStatus(0, kNotes).overdub);
    engine.touchMoved(0, 0.2f, 0.2f);
    step();
    engine.touchReleased(0);
    NoteInterval notes[16];
    REQUIRE(engine.noteLoopIntervals(0, notes, 16) == 2);
    CHECK((notes[0].note == 72 || notes[1].note == 72));
}

TEST_CASE_FIXTURE(ZonesFixture, "after recording only pads, a tap of Record turns overdub on") {
    Preset p = engine.preset();
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::TopLeft)] = note(48);
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::BottomRight)] = note(51);
    load(p);
    engine.tick(t);
    engine.buttonChanged(Button::South, true);
    engine.touchMoved(0, 0.2f, 0.2f);
    step();
    engine.touchReleased(0);
    for (int i = 0; i < 5; ++i)
        step();
    engine.buttonChanged(Button::South, false);
    engine.buttonChanged(Button::South, true);  // tap
    step();
    engine.buttonChanged(Button::South, false);
    REQUIRE(engine.loopStatus(0, kNotes).overdub);
    engine.touchMoved(0, 0.8f, 0.8f);  // another pad, added
    step();
    engine.touchReleased(0);
    NoteInterval notes[8];
    CHECK(engine.noteLoopIntervals(0, notes, 8) == 2);
}

TEST_CASE_FIXTURE(NoteLoopFixture, "Stop pauses every loop without clearing it, Play starts them again from the start") {
    recordLeft();  // a stick loop on CC 70
    recordShortLoop(*this, 4);  // and a note loop
    engine.pauseLoops();
    CHECK(engine.loopsPaused());
    sink.take();
    CHECK(played(70, 6).empty());
    CHECK(engine.loopStatus(0, 0).state == LoopStatus::State::Playing);  // kept
    engine.playLoops();
    CHECK_FALSE(engine.loopsPaused());
    const auto values = played(70, 3);
    REQUIRE(values.size() >= 1);
    CHECK(values[0] == 127);  // from the start
}

TEST_CASE_FIXTURE(NoteLoopFixture, "recording while the loops are stopped plays them all again") {
    recordLeft();
    engine.pauseLoops();
    engine.setChannel(1);
    recordLeft(Axis::RightX);  // a new loop, on another channel
    CHECK_FALSE(engine.loopsPaused());
    CHECK_FALSE(played(70, 4).empty());  // channel 1's loop plays again
}
