#pragma once

#include "Runtime.hpp"

#include "midipad/Preset.hpp"
#include "midipad/Scales.hpp"

#include <optional>
#include <vector>
#include <string>

// Tabs listing every control of one Mapping, and a side panel editing the
// selected one. Draws into the current ImGui window.
class MappingEditor {
public:
    enum class Tab { Buttons, Dpad, Axes, Touchpad };

    void showTab(Tab tab) { requestedTab_ = tab; }
    // Preselects a control, e.g. "button:south", "dpad:up", "zone:1:top_left",
    // or just a tab: "tab:axes".
    // For screenshots; returns false if `spec` names no control.
    bool select(const std::string& spec);

    // Edits one control in a compact form, for the panel shown over the
    // animated view. `spec` is a select() spec ("button:south", "dpad:up",
    // "zone:1:top_left") or "axis:left_x", "axes:left_x,left_y", "finger:1",
    // "fingers". Returns true when `mapping` was changed.
    bool drawControl(const std::string& spec, midipad::Mapping& mapping, const LiveState& live,
                     const std::optional<midipad::Key>& key);
    // A title for that control: "Cross", "D-pad: Up", "Left stick X"...
    static std::string controlTitle(const std::string& spec);

    // Returns true when `mapping` was changed. `live` lights up the controls in
    // use; `key`, if any, narrows the notes and chords offered.
    bool draw(midipad::Mapping& mapping, const LiveState& live, bool readOnly, const std::optional<midipad::Key>& key);

private:
    struct Selection {
        enum class Kind { None, Button, Dpad, Zone } kind = Kind::None;
        int index = 0;   // Button, HatDir or Zone
        int finger = 0;  // Zone
        friend bool operator==(const Selection&, const Selection&) = default;
    };

    bool drawButtons(midipad::Mapping& m, const LiveState& live);
    bool drawDpad(midipad::Mapping& m, const LiveState& live);
    bool drawAxes(midipad::Mapping& m, const LiveState& live);
    bool drawTouchpad(midipad::Mapping& m, const LiveState& live);
    bool drawSidePanel(midipad::Mapping& m);
    bool drawTriggerEditor(midipad::TriggerBinding& b);
    bool drawChordFields(midipad::TriggerBinding& b);
    bool drawNoteField(std::uint8_t& note);
    bool drawAxisForm(const char* id, midipad::AxisBinding& a, float position);
    bool drawFingerForm(int finger, midipad::FingerBinding& f, const LiveState& live);
    const std::vector<midipad::KeyChord>& keyChords();
    bool flats() const { return key_ && midipad::prefersFlats(*key_); }

    midipad::TriggerBinding* selected(midipad::Mapping& m);
    std::string selectedTitle() const;

    Tab tab_ = Tab::Buttons;
    std::optional<Tab> requestedTab_;
    Selection selection_;
    Selection chordTextFor_;
    std::string chordText_;
    std::optional<midipad::Key> key_;
    std::optional<midipad::Key> chordsFor_;
    std::vector<midipad::KeyChord> chords_;
};
