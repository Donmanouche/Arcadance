#pragma once

#include "Runtime.hpp"

#include "midipad/Preset.hpp"

#include <optional>
#include <string>

// Tabs listing every control of one Mapping, and a side panel editing the
// selected one. Draws into the current ImGui window.
class MappingEditor {
public:
    enum class Tab { Buttons, Dpad, Axes, Touchpad };

    void showTab(Tab tab) { requestedTab_ = tab; }
    // Preselects a control, e.g. "button:south", "dpad:up", "zone:1:top_left".
    // For screenshots; returns false if `spec` names no control.
    bool select(const std::string& spec);

    // Returns true when `mapping` was changed. `live` lights up the controls in use.
    bool draw(midipad::Mapping& mapping, const LiveState& live, bool readOnly);

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

    midipad::TriggerBinding* selected(midipad::Mapping& m);
    std::string selectedTitle() const;

    Tab tab_ = Tab::Buttons;
    std::optional<Tab> requestedTab_;
    Selection selection_;
    Selection chordTextFor_;
    std::string chordText_;
};
