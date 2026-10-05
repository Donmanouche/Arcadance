#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace midipad {

// Same order as SDL_GamepadButton, so the input layer converts with a cast.
enum class Button : std::uint8_t {
    South, East, West, North,  // PlayStation: Cross, Circle, Square, Triangle
    Back, Guide, Start,        // Create, PS, Options
    LeftStick, RightStick,     // L3, R3
    LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Misc1,  // DualSense: mute
    RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2,
    Touchpad,  // touchpad click
    Misc2, Misc3, Misc4, Misc5, Misc6,
    // Not SDL buttons: the engine derives these from the analog triggers.
    LeftTrigger, RightTrigger,
    Count
};

// Same order as SDL_GamepadAxis.
enum class Axis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

// The D-pad read as an 8-way hat; "centered" is std::nullopt.
enum class HatDir : std::uint8_t { Up, UpRight, Right, DownRight, Down, DownLeft, Left, UpLeft, Count };

// Quadrants of the touchpad.
enum class Zone : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight, Count };

inline constexpr int kButtonCount = static_cast<int>(Button::Count);
inline constexpr int kAxisCount = static_cast<int>(Axis::Count);
inline constexpr int kHatDirCount = static_cast<int>(HatDir::Count);
inline constexpr int kZoneCount = static_cast<int>(Zone::Count);
inline constexpr int kFingerCount = 2;
inline constexpr int kChannelCount = 16;

constexpr bool isDpad(Button b) { return b >= Button::DpadUp && b <= Button::DpadRight; }
constexpr bool isTrigger(Axis a) { return a == Axis::LeftTrigger || a == Axis::RightTrigger; }

// Stable snake_case identifiers used in preset files ("south", "left_x", "up_right", "top_left").
std::string_view toString(Button b);
std::string_view toString(Axis a);
std::string_view toString(HatDir d);
std::string_view toString(Zone z);

std::optional<Button> parseButton(std::string_view s);
std::optional<Axis> parseAxis(std::string_view s);
std::optional<HatDir> parseHatDir(std::string_view s);
std::optional<Zone> parseZone(std::string_view s);

}  // namespace midipad
