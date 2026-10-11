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

enum class ActionType : std::uint8_t { None, Note, ControlChange, Chord, Transport, ChannelStep, Freeze, LoopRecord, ProgramChange, LoopStretch };

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
    // ProgramChange: `number` when programStep is 0, else one up (+1) or down
    // (-1) from the last program sent on the channel, wrapping or stopping at
    // the ends; preceded by a Bank Select (CC 0, CC 32) when set.
    std::int8_t programStep = 0;
    bool programWrap = true;
    std::int16_t bankMsb = -1, bankLsb = -1;  // -1: none

    bool isSet() const { return type != ActionType::None; }
};

// What a continuous control does:
// - Cc: sends a CC, scaled to [min, max] (min > max reverses it);
// - Program: chooses a Program Change among min..max, sent once the control has
//   stayed on it a moment, never on the way back to rest, and preceded by a
//   Bank Select (CC 0, CC 32) when set;
// - Velocity: raises the velocity of the notes then played, up to 127 at the
//   end of its travel, or lowers it, down to 1 (a trigger only raises it);
// - Transpose: transposes the notes then played, or the note loop's, by up to
//   transposeDown / transposeUp semitones at the ends of its travel (a trigger
//   only up); by degrees of the scale when a tonality is chosen.
// A stick at rest changes nothing; a finger keeps its last value once lifted.
enum class AxisMode : std::uint8_t { Cc, Program, Velocity, Transpose };
enum class TransposeTarget : std::uint8_t { Notes, Loop };
inline constexpr int kMaxTranspose = 24;  // semitones each way

struct AxisBinding {
    bool enabled = false;
    std::uint8_t cc = 0;
    std::uint8_t min = 0;
    std::uint8_t max = 127;
    bool invert = false;
    AxisMode mode = AxisMode::Cc;
    std::int16_t bankMsb = -1, bankLsb = -1;  // Program: -1 none
    std::uint8_t transposeDown = 12, transposeUp = 12;
    TransposeTarget transposeTarget = TransposeTarget::Notes;

    bool sendsCc() const { return mode == AxisMode::Cc; }
};

// A stick movement recorded by a LoopRecord button and played back in a loop,
// beside the stick, on CCs of its own.
enum class LoopPlayback : std::uint8_t { Forward, Reverse, BackAndForth };

struct LoopBinding {
    LoopPlayback playback = LoopPlayback::Forward;
    std::uint8_t ccX = 70, ccY = 71;
    friend bool operator==(const LoopBinding&, const LoopBinding&) = default;
};

// What a loop records: a stick (X, Y), a touchpad finger (X, Y or its pads),
// the triggers' travel (L2 as X, R2 as Y), or the notes and chords played by
// the buttons, the D-pad and the triggers as buttons.
enum class LoopSource : std::uint8_t { LeftStick, RightStick, Finger1, Finger2, Triggers, Notes };
inline constexpr int kLoopSourceCount = 6;

// By default on CC 70/71, 72/73 (sticks), 74/75, 76/77 (fingers) and 78/79
// (triggers); a note loop sends the notes it recorded, its CCs are unused.
inline constexpr std::array<LoopBinding, kLoopSourceCount> kDefaultLoops{{{LoopPlayback::Forward, 70, 71},
                                                                          {LoopPlayback::Forward, 72, 73},
                                                                          {LoopPlayback::Forward, 74, 75},
                                                                          {LoopPlayback::Forward, 76, 77},
                                                                          {LoopPlayback::Forward, 78, 79},
                                                                          {LoopPlayback::Forward, 0, 0}}};

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
    std::array<LoopBinding, kLoopSourceCount> loops = kDefaultLoops;  // by LoopSource
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
