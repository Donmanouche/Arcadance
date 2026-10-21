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

    // A loop the user asked to clear, once: a LoopSource and an axis (-1: both).
    struct ClearLoop {
        int source = 0, axis = -1;
    };
    std::optional<ClearLoop> takeClearLoopRequest() { return std::exchange(clearLoopRequest_, std::nullopt); }
    // A loop's modulation set in the window, once (Engine::setLoopModulation, on the active channel).
    struct Modulation {
        int source = 0, axis = 0;
        midipad::LoopModulation modulation;
    };
    std::optional<Modulation> takeModulationRequest() { return std::exchange(modulationRequest_, std::nullopt); }
    // An axis loop's end dragged in the control's window: its new length, once
    // (Engine::setAxisLoopLength, on the active channel).
    struct AxisLength {
        int source = 0, axis = 0, length = 0;
    };
    std::optional<AxisLength> takeAxisLengthRequest() { return std::exchange(axisLength_, std::nullopt); }
    // Escape, during the last drawControl(), cancelled a drag: the panel should stay open.
    bool tookEscape() const { return escapeTaken_; }

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
    bool drawLoopForm(int source, midipad::Mapping& m, const LiveState& live);
    void drawAxisLanes(int source, const midipad::Mapping& m, const LiveState& live);
    bool drawStepsForm(int source, midipad::Mapping& m);
    bool drawAxisForm(const char* id, midipad::AxisBinding& a, float position, bool trigger = false);
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
    std::optional<ClearLoop> clearLoopRequest_;
    std::optional<Modulation> modulationRequest_;
    void drawModulationForm(int source, const LiveState& live);
    std::optional<AxisLength> axisLength_;
    std::string layerSpec_;    // the control whose layers were shown last
    bool shiftLayer_ = false;  // its Shift layer was
    // An axis loop's end being dragged: which, where the mouse took it, the
    // samples a pixel then, its length then and so far.
    struct LaneDrag {
        int source = 0, axis = 0;
        float grabX = 0, perPx = 1;
        int length = 0, newLength = 0;
    };
    std::optional<LaneDrag> laneDrag_;
    bool escapeTaken_ = false;
};
