// GamepadInput against an SDL virtual gamepad: checks the SDL-to-engine
// conversion (button order, axis ranges, touchpad, hotplug) without hardware.

#include "GamepadInput.hpp"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

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

TriggerBinding note(int number) {
    TriggerBinding b;
    b.type = ActionType::Note;
    b.number = static_cast<std::uint8_t>(number);
    return b;
}

AxisBinding axisCc(int number) { return {true, static_cast<std::uint8_t>(number), 0, 127, false}; }

struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;

    VirtualPad() {
        static const SDL_VirtualJoystickTouchpadDesc touchpad{2, {}};
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.ntouchpads = 1;
        desc.touchpads = &touchpad;
        desc.name = "MidiPad test pad";
        id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE_MESSAGE(id != 0, SDL_GetError());
        joystick = SDL_OpenJoystick(id);
        REQUIRE(joystick);
    }
    ~VirtualPad() { detach(); }

    void detach() {
        if (joystick)
            SDL_CloseJoystick(joystick);
        if (id)
            SDL_DetachVirtualJoystick(id);
        joystick = nullptr;
        id = 0;
    }
    void button(SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(joystick, b, down); }
    void axis(SDL_GamepadAxis a, Sint16 v) { SDL_SetJoystickVirtualAxis(joystick, a, v); }
    void finger(int f, bool down, float x, float y) { SDL_SetJoystickVirtualTouchpad(joystick, 0, f, down, x, y, 1.0f); }
};

struct SdlSession {
    SdlSession() {
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
        SDL_SetGamepadEventsEnabled(false);
        SDL_SetJoystickEventsEnabled(false);
    }
    ~SdlSession() { SDL_Quit(); }
};

}  // namespace

TEST_CASE("a virtual gamepad drives the engine through GamepadInput") {
    SdlSession sdl;
    VirtualPad pad;

    Preset p;
    p.base.buttons[static_cast<std::size_t>(Button::South)] = note(48);
    p.base.buttons[static_cast<std::size_t>(Button::RightShoulder)] = note(49);
    p.base.dpad[static_cast<std::size_t>(HatDir::Up)] = note(60);
    p.base.axes[static_cast<std::size_t>(Axis::LeftX)] = axisCc(40);
    p.base.axes[static_cast<std::size_t>(Axis::RightTrigger)] = axisCc(51);
    p.base.fingers[0].mode = TouchMode::Zones;
    p.base.fingers[0].zones[static_cast<std::size_t>(Zone::TopLeft)] = note(50);

    RecordingSink sink;
    Engine engine(sink);
    engine.setPreset(std::make_shared<const Preset>(p));
    std::vector<std::string> log;
    GamepadInput input(engine, [&](const std::string& line) { log.push_back(line); },
                       [](SDL_JoystickID id) { return SDL_IsJoystickVirtual(id); });

    std::uint64_t now = 0;
    auto poll = [&] { input.poll(now += 1'000'000); };

    poll();
    REQUIRE(!log.empty());
    CHECK(log.front().find("MidiPad test pad") != std::string::npos);
    CHECK(log.front().find("touchpad actif") != std::string::npos);
    CHECK(sink.take().empty());  // connecting sends nothing

    SUBCASE("buttons keep their SDL meaning") {
        pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
        poll();
        pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
        pad.button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
        poll();
        CHECK(sink.take() == Messages{on(0, 48, 100), off(0, 48), on(0, 49, 100)});
    }

    SUBCASE("the D-pad reaches the engine as a hat") {
        pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        poll();
        pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, false);
        poll();
        CHECK(sink.take() == Messages{on(0, 60, 100), off(0, 60)});
    }

    SUBCASE("stick and trigger ranges are normalized") {
        pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 32767);
        poll();
        pad.axis(SDL_GAMEPAD_AXIS_LEFTX, -32768);
        poll();
        pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);  // virtual axis full range = trigger fully pressed
        poll();
        CHECK(sink.take() == Messages{cc(0, 40, 127), cc(0, 40, 0), cc(0, 51, 127)});
    }

    SUBCASE("touchpad fingers reach the engine") {
        pad.finger(0, true, 0.1f, 0.1f);
        poll();
        pad.finger(0, false, 0.1f, 0.1f);
        poll();
        CHECK(sink.take() == Messages{on(0, 50, 100), off(0, 50)});
    }

    SUBCASE("unplugging silences held notes") {
        pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
        poll();
        sink.take();
        pad.detach();
        poll();
        CHECK(sink.take() == Messages{off(0, 48)});
        CHECK(log.back().find("déconnectée") != std::string::npos);
    }
}
