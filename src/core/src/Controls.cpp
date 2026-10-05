#include "midipad/Controls.hpp"

#include <array>

namespace midipad {

namespace {

constexpr std::array<std::string_view, kButtonCount> kButtonNames = {
    "south",         "east",          "west",          "north",        "back",          "guide",
    "start",         "left_stick",    "right_stick",   "left_shoulder", "right_shoulder", "dpad_up",
    "dpad_down",     "dpad_left",     "dpad_right",    "misc1",        "right_paddle1", "left_paddle1",
    "right_paddle2", "left_paddle2",  "touchpad",      "misc2",        "misc3",         "misc4",
    "misc5",         "misc6",         "left_trigger",  "right_trigger",
};

constexpr std::array<std::string_view, kAxisCount> kAxisNames = {
    "left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger",
};

constexpr std::array<std::string_view, kHatDirCount> kHatDirNames = {
    "up", "up_right", "right", "down_right", "down", "down_left", "left", "up_left",
};

constexpr std::array<std::string_view, kZoneCount> kZoneNames = {
    "top_left", "top_right", "bottom_left", "bottom_right",
};

template <typename Enum, std::size_t N>
std::optional<Enum> lookup(const std::array<std::string_view, N>& names, std::string_view s) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == s)
            return static_cast<Enum>(i);
    return std::nullopt;
}

template <std::size_t N, typename Enum>
std::string_view nameOf(const std::array<std::string_view, N>& names, Enum e) {
    auto i = static_cast<std::size_t>(e);
    return i < N ? names[i] : std::string_view{};
}

}  // namespace

std::string_view toString(Button b) { return nameOf(kButtonNames, b); }
std::string_view toString(Axis a) { return nameOf(kAxisNames, a); }
std::string_view toString(HatDir d) { return nameOf(kHatDirNames, d); }
std::string_view toString(Zone z) { return nameOf(kZoneNames, z); }

std::optional<Button> parseButton(std::string_view s) { return lookup<Button>(kButtonNames, s); }
std::optional<Axis> parseAxis(std::string_view s) { return lookup<Axis>(kAxisNames, s); }
std::optional<HatDir> parseHatDir(std::string_view s) { return lookup<HatDir>(kHatDirNames, s); }
std::optional<Zone> parseZone(std::string_view s) { return lookup<Zone>(kZoneNames, s); }

}  // namespace midipad
