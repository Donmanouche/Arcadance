#include "GamepadInput.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

using namespace midipad;

static_assert(static_cast<int>(Button::LeftTrigger) == SDL_GAMEPAD_BUTTON_COUNT,
              "midipad::Button must start with the SDL gamepad buttons, in SDL order");
static_assert(kAxisCount == SDL_GAMEPAD_AXIS_COUNT, "midipad::Axis must match SDL_GamepadAxis");

namespace {

constexpr std::uint64_t kScanIntervalNs = 500'000'000;  // look for a gamepad twice a second

float normalizeAxis(int axis, Sint16 raw) {
    // Sticks go from -32768 to 32767, triggers from 0 to 32767.
    const float v = static_cast<float>(raw) / 32767.0f;
    return axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? std::clamp(v, 0.0f, 1.0f) : std::clamp(v, -1.0f, 1.0f);
}

}  // namespace

GamepadInput::GamepadInput(Engine& engine, Log log, Filter accept)
    : engine_(engine), log_(std::move(log)), accept_(std::move(accept)) {}

GamepadInput::~GamepadInput() { close(); }

void GamepadInput::poll(std::uint64_t nowNs) {
    SDL_UpdateGamepads();

    if (!pad_) {
        if (nowNs < nextScanNs_)
            return;
        nextScanNs_ = nowNs + kScanIntervalNs;
        tryOpen();
        return;
    }
    if (!SDL_GamepadConnected(pad_)) {
        log_("Gamepad disconnected: every note was turned off.");
        close();
        engine_.reset();
        return;
    }

    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const bool down = SDL_GetGamepadButton(pad_, static_cast<SDL_GamepadButton>(b));
        if (down != buttons_.test(static_cast<std::size_t>(b))) {
            buttons_.set(static_cast<std::size_t>(b), down);
            if (buttonTap_)
                buttonTap_(static_cast<Button>(b), down);
            engine_.buttonChanged(static_cast<Button>(b), down);
        }
    }

    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const Sint16 raw = SDL_GetGamepadAxis(pad_, static_cast<SDL_GamepadAxis>(a));
        if (raw != axes_[static_cast<std::size_t>(a)]) {
            axes_[static_cast<std::size_t>(a)] = raw;
            engine_.axisMoved(static_cast<Axis>(a), normalizeAxis(a, raw));
        }
    }

    if (!hasTouchpad_)
        return;
    for (int f = 0; f < kFingerCount; ++f) {
        Finger now;
        float pressure = 0;
        if (!SDL_GetGamepadTouchpadFinger(pad_, 0, f, &now.down, &now.x, &now.y, &pressure))
            continue;
        Finger& before = fingers_[static_cast<std::size_t>(f)];
        if (now.down && (!before.down || now.x != before.x || now.y != before.y))
            engine_.touchMoved(f, now.x, now.y);
        else if (!now.down && before.down)
            engine_.touchReleased(f);
        before = now;
    }
}

std::string GamepadInput::name() const {
    const char* n = pad_ ? SDL_GetGamepadName(pad_) : nullptr;
    return n ? n : (pad_ ? "Unnamed gamepad" : "");
}

// Controllers SDL maps wrongly on its own, mapped right (by place: a is the
// bottom face button, b the right one). The HORIPAD S (wired, for Switch)
// sends its buttons in the Switch's order (Y, B, A, X, L, R, ZL, ZR, -, +,
// LS, RS, Home, Capture), its right stick on Z / RZ, and has no analog
// triggers: SDL's guess swapped the face buttons and gave the triggers the
// right stick's axes.
constexpr const char* kMappingFixes[] = {
    "0300910d0d0f0000c100000011010000,HORIPAD S,a:b1,b:b2,x:b0,y:b3,back:b8,guide:b12,start:b9,leftstick:b10,rightstick:b11,"
    "leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,misc1:b13,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,",
};

constexpr Uint16 kSonyVendor = 0x054c, kNintendoVendor = 0x057e, kValveVendor = 0x28de;

void GamepadInput::tryOpen() {
    static const bool fixed = [] {
        for (const char* mapping : kMappingFixes)
            SDL_AddGamepadMapping(mapping);
        return true;
    }();
    (void)fixed;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count && !pad_; ++i)
        if (!accept_ || accept_(ids[i]))
            pad_ = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (!pad_)
        return;

    hasTouchpad_ = SDL_GetNumGamepadTouchpads(pad_) > 0;
    // Its family: from its type; else Nintendo's and Valve's own (Xbox-like)
    // by their vendor; else a PlayStation or Nintendo labelling; else generic,
    // named by place (SDL calls an unknown pad's face buttons A, B, X, Y even
    // when they read 1-4).
    using F = midipad::ControllerFamily;
    const Uint16 vendor = SDL_GetGamepadVendor(pad_);
    switch (SDL_GetGamepadType(pad_)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: family_ = F::PlayStation; break;
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: family_ = F::Xbox; break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE: family_ = F::Nintendo; break;
    default:
        if (vendor == kNintendoVendor)
            family_ = F::Nintendo;
        else if (vendor == kValveVendor)
            family_ = F::Xbox;
        else
            switch (SDL_GetGamepadButtonLabel(pad_, SDL_GAMEPAD_BUTTON_SOUTH)) {
            case SDL_GAMEPAD_BUTTON_LABEL_B: family_ = F::Nintendo; break;
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: family_ = F::PlayStation; break;
            default: family_ = F::Generic; break;
            }
    }
    // A touchpad only Sony's own DualShock 4 and DualSense are expected to have
    // (without the access to it, they have none to SDL).
    const SDL_GamepadType type = SDL_GetGamepadType(pad_);
    touchpadExpected_ = vendor == kSonyVendor && (type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5);
    presentButtons_ = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b)
        if (SDL_GamepadHasButton(pad_, static_cast<SDL_GamepadButton>(b)))
            presentButtons_ |= 1u << b;
    presentAxes_ = 0;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
        if (SDL_GamepadHasAxis(pad_, static_cast<SDL_GamepadAxis>(a)))
            presentAxes_ = static_cast<std::uint8_t>(presentAxes_ | (1u << a));
    if (SDL_GamepadHasAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))
        presentButtons_ |= 1u << static_cast<int>(midipad::Button::LeftTrigger);
    if (SDL_GamepadHasAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
        presentButtons_ |= 1u << static_cast<int>(midipad::Button::RightTrigger);
    const char* name = SDL_GetGamepadName(pad_);
    std::string message = std::string("Gamepad connected: ") + (name ? name : "unnamed");
    message += hasTouchpad_ ? " (touchpad active)" : " (no touchpad)";
    log_(message);
    if (!hasTouchpad_ && touchpadExpected_)
        log_("  The DualSense touchpad needs access to /dev/hidraw*: see the udev rule in the README.");

    captureBaseline();
}

void GamepadInput::close() {
    if (pad_)
        SDL_CloseGamepad(pad_);
    pad_ = nullptr;
    buttons_.reset();
    axes_.fill(0);
    fingers_ = {};
    hasTouchpad_ = false;
}

void GamepadInput::captureBaseline() {
    // Start from the controller's current state, so connecting it does not send
    // a burst of CCs for sticks at rest; only later movements count.
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        buttons_.set(static_cast<std::size_t>(b), SDL_GetGamepadButton(pad_, static_cast<SDL_GamepadButton>(b)));
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
        axes_[static_cast<std::size_t>(a)] = SDL_GetGamepadAxis(pad_, static_cast<SDL_GamepadAxis>(a));
    fingers_ = {};
}
