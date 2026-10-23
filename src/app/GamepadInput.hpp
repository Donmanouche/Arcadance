#pragma once

#include "midipad/Engine.hpp"
#include "midipad/Labels.hpp"

#include <SDL3/SDL_gamepad.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <string>

// Reads the first connected gamepad by polling its state and feeds every
// change to the engine.
//
// Polling instead of SDL events keeps the controller off the main thread's
// event loop (which will also render the UI), so input latency stays at one
// poll period. SDL gamepad events must therefore be disabled.
class GamepadInput {
public:
    using Log = std::function<void(const std::string&)>;
    using Filter = std::function<bool(SDL_JoystickID)>;

    // `accept` picks which gamepads may be opened (default: any).
    GamepadInput(midipad::Engine& engine, Log log, Filter accept = {});
    ~GamepadInput();
    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

    // Called from poll() for every button change, before the engine sees it.
    // Must not block: it runs on the real-time thread.
    void setButtonTap(std::function<void(midipad::Button, bool)> tap) { buttonTap_ = std::move(tap); }

    // Call at a steady high rate from one thread. Allocation-free while a
    // gamepad stays connected.
    void poll(std::uint64_t nowNs);

    struct Finger {
        bool down = false;
        float x = 0, y = 0;
    };

    // State as of the last poll(), for display.
    bool connected() const { return pad_ != nullptr; }
    bool hasTouchpad() const { return hasTouchpad_; }
    // What the controller is and has, found when it connects: the family its
    // buttons are named after, a bit per midipad::Button it has (the triggers
    // as buttons when it has their axes) and per axis.
    midipad::ControllerFamily family() const { return family_; }
    std::uint32_t presentButtons() const { return presentButtons_; }
    std::uint8_t presentAxes() const { return presentAxes_; }
    bool touchpadExpected() const { return touchpadExpected_; }  // a Sony DualShock 4 or DualSense, touchpad read or not
    std::string name() const;  // empty when no gamepad is open
    const std::bitset<SDL_GAMEPAD_BUTTON_COUNT>& buttons() const { return buttons_; }
    const std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT>& axes() const { return axes_; }
    const std::array<Finger, midipad::kFingerCount>& fingers() const { return fingers_; }

private:

    void tryOpen();
    void close();
    void captureBaseline();

    midipad::Engine& engine_;
    Log log_;
    Filter accept_;
    std::function<void(midipad::Button, bool)> buttonTap_;
    SDL_Gamepad* pad_ = nullptr;
    std::uint64_t nextScanNs_ = 0;

    std::bitset<SDL_GAMEPAD_BUTTON_COUNT> buttons_;
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axes_{};
    std::array<Finger, midipad::kFingerCount> fingers_{};
    bool hasTouchpad_ = false;
    midipad::ControllerFamily family_ = midipad::ControllerFamily::PlayStation;
    std::uint32_t presentButtons_ = 0;
    std::uint8_t presentAxes_ = 0;
    bool touchpadExpected_ = false;
};
