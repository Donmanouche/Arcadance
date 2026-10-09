#pragma once

#include "midipad/Chords.hpp"
#include "midipad/Controls.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace midipad {

enum class Mode : std::uint8_t {
    Momentary,  // sounds while held
    Toggle,     // press to latch, press again to release
};

enum class ActionType : std::uint8_t { None, Note, ControlChange, Chord, Transport, ChannelStep, Freeze, LoopRecord };

enum class TransportCommand : std::uint8_t { Start, Stop, Continue };

// Octaves a named chord's root can be placed in (4: C4 = 60, as parseChord).
inline constexpr int kMinChordOctave = 0;
inline constexpr int kMaxChordOctave = 8;
inline constexpr int kDefaultChordOctave = 4;

// What a discrete control (button, D-pad direction, touchpad zone) does.
struct TriggerBinding {
    ActionType type = ActionType::None;
    Mode mode = Mode::Momentary;
    std::uint8_t number = 0;     // note or controller number
    std::uint8_t velocity = 100; // Note, Chord
    std::uint8_t onValue = 127;  // ControlChange
    std::uint8_t offValue = 0;   // ControlChange
    ChordNotes chord;
    std::string chordName;       // empty when the chord was given as raw notes
    std::int8_t chordOctave = kDefaultChordOctave;  // octave of a named chord's root
    std::uint8_t chordRootBelow = 0;  // 1 or 2: the root doubled that many octaves lower
    bool humanize = true;        // Note, Chord: follows the channel's humanization
    TransportCommand transport = TransportCommand::Start;
    std::int8_t channelStep = 1;
    std::uint8_t freezeAxes = 0; // one bit per Axis

    bool isSet() const { return type != ActionType::None; }
};

// A continuous control sent as a CC, scaled to [min, max] (min > max reverses it).
struct AxisBinding {
    bool enabled = false;
    std::uint8_t cc = 0;
    std::uint8_t min = 0;
    std::uint8_t max = 127;
    bool invert = false;
};

// A stick movement recorded by a LoopRecord button and played back in a loop,
// beside the stick, on CCs of its own.
enum class LoopPlayback : std::uint8_t { Forward, Reverse, BackAndForth };

struct LoopBinding {
    LoopPlayback playback = LoopPlayback::Forward;
    std::uint8_t ccX = 70, ccY = 71;
    friend bool operator==(const LoopBinding&, const LoopBinding&) = default;
};

// Loops of the left and right sticks, by default on CC 70/71 and 72/73.
inline constexpr std::array<LoopBinding, 2> kDefaultLoops{{{LoopPlayback::Forward, 70, 71},
                                                           {LoopPlayback::Forward, 72, 73}}};

enum class TouchMode : std::uint8_t { Off, Axis, Zones };

struct FingerBinding {
    TouchMode mode = TouchMode::Off;
    AxisBinding x, y;  // TouchMode::Axis; y grows upwards
    std::array<TriggerBinding, kZoneCount> zones;  // TouchMode::Zones
};

struct Mapping {
    std::array<TriggerBinding, kButtonCount> buttons;  // D-pad entries unused: see dpad
    std::array<TriggerBinding, kHatDirCount> dpad;
    std::array<AxisBinding, kAxisCount> axes;
    std::array<FingerBinding, kFingerCount> fingers;
    std::array<LoopBinding, 2> loops = kDefaultLoops;  // left stick, right stick
};

struct Preset {
    std::string name;
    Mapping base;
    // Replaces `base` entirely while that output channel is active (index 0 = MIDI channel 1).
    std::array<std::optional<Mapping>, kChannelCount> channels;

    const Mapping& forChannel(int channel) const {
        if (channel < 0 || channel >= kChannelCount || !channels[static_cast<std::size_t>(channel)])
            return base;
        return *channels[static_cast<std::size_t>(channel)];
    }
};

}  // namespace midipad
