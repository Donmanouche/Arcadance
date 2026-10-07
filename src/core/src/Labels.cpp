#include "midipad/Labels.hpp"

#include "midipad/Notes.hpp"

namespace midipad {

std::string label(Button b) {
    switch (b) {
    case Button::South: return "Croix";
    case Button::East: return "Rond";
    case Button::West: return "Carré";
    case Button::North: return "Triangle";
    case Button::Back: return "Create";
    case Button::Guide: return "PS";
    case Button::Start: return "Options";
    case Button::LeftStick: return "L3";
    case Button::RightStick: return "R3";
    case Button::LeftShoulder: return "L1";
    case Button::RightShoulder: return "R1";
    case Button::DpadUp: return "Croix haut";
    case Button::DpadDown: return "Croix bas";
    case Button::DpadLeft: return "Croix gauche";
    case Button::DpadRight: return "Croix droite";
    case Button::Misc1: return "Micro";
    case Button::Touchpad: return "Clic touchpad";
    case Button::LeftTrigger: return "L2";
    case Button::RightTrigger: return "R2";
    default: return std::string(toString(b));
    }
}

std::string label(Axis a) {
    switch (a) {
    case Axis::LeftX: return "Stick gauche X";
    case Axis::LeftY: return "Stick gauche Y";
    case Axis::RightX: return "Stick droit X";
    case Axis::RightY: return "Stick droit Y";
    case Axis::LeftTrigger: return "Gâchette L2";
    case Axis::RightTrigger: return "Gâchette R2";
    case Axis::Count: break;
    }
    return {};
}

std::string label(HatDir d) {
    switch (d) {
    case HatDir::Up: return "Haut";
    case HatDir::UpRight: return "Haut-droite";
    case HatDir::Right: return "Droite";
    case HatDir::DownRight: return "Bas-droite";
    case HatDir::Down: return "Bas";
    case HatDir::DownLeft: return "Bas-gauche";
    case HatDir::Left: return "Gauche";
    case HatDir::UpLeft: return "Haut-gauche";
    case HatDir::Count: break;
    }
    return {};
}

std::string label(Zone z) {
    switch (z) {
    case Zone::TopLeft: return "Haut gauche";
    case Zone::TopRight: return "Haut droite";
    case Zone::BottomLeft: return "Bas gauche";
    case Zone::BottomRight: return "Bas droite";
    case Zone::Count: break;
    }
    return {};
}

namespace {

std::string withMode(std::string text, Mode mode) {
    if (mode == Mode::Toggle)
        text += ", bascule";
    return text;
}

}  // namespace

std::string describe(const TriggerBinding& b) {
    switch (b.type) {
    case ActionType::None:
        return "-";
    case ActionType::Note: {
        std::string s = "Note " + noteName(b.number) + " (" + std::to_string(b.number) + ")";
        if (b.velocity != 100)
            s += ", vél. " + std::to_string(b.velocity);
        return withMode(s, b.mode);
    }
    case ActionType::ControlChange:
        return withMode("CC " + std::to_string(b.number) + " : " + std::to_string(b.onValue) + " / " +
                            std::to_string(b.offValue),
                        b.mode);
    case ActionType::Chord: {
        std::string s = "Accord ";
        if (!b.chordName.empty()) {
            s += b.chordName;
        } else {
            for (std::uint8_t i = 0; i < b.chord.count; ++i)
                s += (i ? " " : "") + noteName(b.chord.notes[i]);
        }
        if (b.velocity != 100)
            s += ", vél. " + std::to_string(b.velocity);
        return withMode(s, b.mode);
    }
    case ActionType::Transport:
        return b.transport == TransportCommand::Start  ? "Transport : lecture"
               : b.transport == TransportCommand::Stop ? "Transport : arrêt"
                                                       : "Transport : reprise";
    case ActionType::ChannelStep:
        return std::string("Canal ") + (b.channelStep > 0 ? "+" : "") + std::to_string(b.channelStep);
    case ActionType::Freeze: {
        std::string s = "Freeze :";
        bool first = true;
        for (int a = 0; a < kAxisCount; ++a) {
            if (b.freezeAxes & (1u << a)) {
                s += (first ? " " : ", ") + label(static_cast<Axis>(a));
                first = false;
            }
        }
        return s;
    }
    }
    return {};
}

std::string describe(const AxisBinding& a) {
    if (!a.enabled)
        return "-";
    std::string s = "CC " + std::to_string(a.cc);
    if (a.min != 0 || a.max != 127)
        s += " (" + std::to_string(a.min) + "-" + std::to_string(a.max) + ")";
    if (a.invert)
        s += ", inversé";
    return s;
}

}  // namespace midipad
