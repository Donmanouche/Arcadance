#pragma once

#include "Runtime.hpp"

#include "midipad/Preset.hpp"
#include "midipad/Scales.hpp"

#include <optional>
#include <utility>
#include <vector>
#include <string>

// The forms editing one control of a Mapping, for the panel shown over the
// animated view. Draws into the current ImGui window.
class MappingEditor {
public:
    // Edits one control. `spec` names it: "button:south", "dpad:up",
    // "zone:1:top_left", "axis:left_x", "axes:left_x,left_y", "finger:1",
    // "fingers". Returns true when `mapping` was changed.
    bool drawControl(const std::string& spec, midipad::Mapping& mapping, const LiveState& live,
                     const std::optional<midipad::Key>& key);
    // Channels where humanization is on (Options), for the hint next to the
    // Humanize checkbox of notes and chords.
    void setHumanizeChannels(std::uint16_t channels) { humanizeChannels_ = channels; }

    // A stick whose loop the user asked to clear (0: left, 1: right), once.
    std::optional<int> takeClearLoopRequest() { return std::exchange(clearLoopRequest_, std::nullopt); }

    // A title for that control: "Cross", "D-pad: Up", "Left stick X"...
    static std::string controlTitle(const std::string& spec);

private:
    struct Selection {
        enum class Kind { None, Button, Dpad, Zone } kind = Kind::None;
        int index = 0;   // Button, HatDir or Zone
        int finger = 0;  // Zone
        friend bool operator==(const Selection&, const Selection&) = default;
    };

    bool drawTriggerEditor(midipad::TriggerBinding& b);
    bool drawChordFields(midipad::TriggerBinding& b);
    bool drawNoteField(std::uint8_t& note);
    bool drawHumanizeCheckbox(midipad::TriggerBinding& b);
    bool drawLoopForm(int stick, midipad::Mapping& m);
    bool drawAxisForm(const char* id, midipad::AxisBinding& a, float position);
    bool drawFingerForm(int finger, midipad::FingerBinding& f, const LiveState& live);
    const std::vector<midipad::KeyChord>& keyChords();
    bool flats() const { return key_ && midipad::prefersFlats(*key_); }

    Selection selection_;
    Selection chordTextFor_;
    std::string chordText_;
    std::optional<midipad::Key> key_;
    std::optional<midipad::Key> chordsFor_;
    std::vector<midipad::KeyChord> chords_;
    std::uint16_t humanizeChannels_ = 0;
    std::optional<int> clearLoopRequest_;
};
