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
        log_("Manette déconnectée : toutes les notes sont coupées.");
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

void GamepadInput::tryOpen() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count && !pad_; ++i)
        if (!accept_ || accept_(ids[i]))
            pad_ = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (!pad_)
        return;

    hasTouchpad_ = SDL_GetNumGamepadTouchpads(pad_) > 0;
    const char* name = SDL_GetGamepadName(pad_);
    std::string message = std::string("Manette connectée : ") + (name ? name : "sans nom");
    message += hasTouchpad_ ? " (touchpad actif)" : " (pas de touchpad)";
    log_(message);
    if (!hasTouchpad_ && SDL_GetGamepadType(pad_) == SDL_GAMEPAD_TYPE_PS5)
        log_("  Le touchpad de la DualSense demande l'accès à /dev/hidraw* : voir la règle udev du README.");

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
