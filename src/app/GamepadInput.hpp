#pragma once

#include "midipad/Engine.hpp"

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

private:
    struct Finger {
        bool down = false;
        float x = 0, y = 0;
    };

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
};
