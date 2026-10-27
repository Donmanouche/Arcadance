#include "midipad/Labels.hpp"

#include "midipad/Notes.hpp"

#include <algorithm>

namespace midipad {

namespace {
ControllerFamily gFamily = ControllerFamily::PlayStation;
}  // namespace

void setLabelFamily(ControllerFamily family) { gFamily = family; }
ControllerFamily labelFamily() { return gFamily; }

std::string label(Button b) {
    if (gFamily == ControllerFamily::Xbox) {
        switch (b) {
        case Button::South: return "A";
        case Button::East: return "B";
        case Button::West: return "X";
        case Button::North: return "Y";
        case Button::Back: return "View";
        case Button::Guide: return "Xbox";
        case Button::Start: return "Menu";
        case Button::LeftStick: return "LS";
        case Button::RightStick: return "RS";
        case Button::LeftShoulder: return "LB";
        case Button::RightShoulder: return "RB";
        case Button::LeftTrigger: return "LT";
        case Button::RightTrigger: return "RT";
        case Button::Misc1: return "Share";
        default: break;
        }
    } else if (gFamily == ControllerFamily::Nintendo) {
        switch (b) {
        case Button::South: return "B";
        case Button::East: return "A";
        case Button::West: return "Y";
        case Button::North: return "X";
        case Button::Back: return "Minus";
        case Button::Guide: return "Home";
        case Button::Start: return "Plus";
        case Button::LeftStick: return "LS";
        case Button::RightStick: return "RS";
        case Button::LeftShoulder: return "L";
        case Button::RightShoulder: return "R";
        case Button::LeftTrigger: return "ZL";
        case Button::RightTrigger: return "ZR";
        case Button::Misc1: return "Capture";
        default: break;
        }
    } else if (gFamily == ControllerFamily::Generic) {
        switch (b) {
        case Button::South: return "Bottom";
        case Button::East: return "Right";
        case Button::West: return "Left";
        case Button::North: return "Top";
        case Button::Back: return "Select";
        case Button::Guide: return "Home";
        case Button::Start: return "Start";
        default: break;
        }
    }
    switch (b) {
    case Button::South: return "Cross";
    case Button::East: return "Circle";
    case Button::West: return "Square";
    case Button::North: return "Triangle";
    case Button::Back: return "Create";
    case Button::Guide: return "PS";
    case Button::Start: return "Options";
    case Button::LeftStick: return "L3";
    case Button::RightStick: return "R3";
    case Button::LeftShoulder: return "L1";
    case Button::RightShoulder: return "R1";
    case Button::DpadUp: return "D-pad up";
    case Button::DpadDown: return "D-pad down";
    case Button::DpadLeft: return "D-pad left";
    case Button::DpadRight: return "D-pad right";
    case Button::Misc1: return "Mute";
    case Button::Touchpad: return "Touchpad click";
    case Button::LeftTrigger: return "L2";
    case Button::RightTrigger: return "R2";
    case Button::RightPaddle1: return "P1";
    case Button::RightPaddle2: return "P2";
    case Button::LeftPaddle1: return "P3";
    case Button::LeftPaddle2: return "P4";
    default: return std::string(toString(b));
    }
}

std::string label(Axis a) {
    switch (a) {
    case Axis::LeftX: return "Left stick X";
    case Axis::LeftY: return "Left stick Y";
    case Axis::RightX: return "Right stick X";
    case Axis::RightY: return "Right stick Y";
    case Axis::LeftTrigger: return label(Button::LeftTrigger) + " trigger";
    case Axis::RightTrigger: return label(Button::RightTrigger) + " trigger";
    case Axis::Count: break;
    }
    return {};
}

std::string label(HatDir d) {
    switch (d) {
    case HatDir::Up: return "Up";
    case HatDir::UpRight: return "Up-right";
    case HatDir::Right: return "Right";
    case HatDir::DownRight: return "Down-right";
    case HatDir::Down: return "Down";
    case HatDir::DownLeft: return "Down-left";
    case HatDir::Left: return "Left";
    case HatDir::UpLeft: return "Up-left";
    case HatDir::Count: break;
    }
    return {};
}

std::string label(Zone z) {
    switch (z) {
    case Zone::TopLeft: return "Top left";
    case Zone::TopRight: return "Top right";
    case Zone::BottomLeft: return "Bottom left";
    case Zone::BottomRight: return "Bottom right";
    case Zone::Count: break;
    }
    return {};
}

namespace {

std::string withMode(std::string text, Mode mode) {
    if (mode == Mode::Toggle)
        text += ", toggle";
    return text;
}

}  // namespace

std::string describe(const TriggerBinding& b, bool flats) {
    switch (b.type) {
    case ActionType::None:
        return "-";
    case ActionType::Note: {
        std::string s = "Note " + noteName(b.number, flats) + " (" + std::to_string(b.number) + ")";
        if (b.velocity != 100)
            s += ", vel. " + std::to_string(b.velocity);
        return withMode(s, b.mode);
    }
    case ActionType::ControlChange:
        return withMode("CC " + std::to_string(b.number) + ": " + std::to_string(b.onValue) + " / " +
                            std::to_string(b.offValue),
                        b.mode);
    case ActionType::Chord: {
        std::string s = "Chord ";
        if (!b.chordName.empty()) {
            s += b.chordName;
        } else {
            for (std::uint8_t i = 0; i < b.chord.count; ++i)
                s += (i ? " " : "") + noteName(b.chord.notes[i], flats);
        }
        if (b.velocity != 100)
            s += ", vel. " + std::to_string(b.velocity);
        return withMode(s, b.mode);
    }
    case ActionType::Transport:
        return b.transport == TransportCommand::Start  ? "Transport: start"
               : b.transport == TransportCommand::Stop ? "Transport: stop"
                                                       : "Transport: continue";
    case ActionType::ChannelStep:
        return std::string("Channel ") + (b.channelStep > 0 ? "+" : "") + std::to_string(b.channelStep);
    case ActionType::LoopRecord:
        return "Loop record";
    case ActionType::Shift:
        return b.mode == Mode::Toggle ? "Shift (toggle)" : "Shift";
    case ActionType::TapTempo:
        return "Tap tempo";
    case ActionType::SessionScreen:
        return "Mode";
    case ActionType::RecenterTilt:
        return "Recenter tilt";
    case ActionType::Effect:
        return (b.effect == EffectKind::Delay ? "Delay " + std::to_string(b.effectFeedback) + "%" : "Retrigger \xC3\x97" + std::to_string(b.effectRepeats)) +
               ", " + std::string(kEffectEveryNames[static_cast<std::size_t>(std::clamp<int>(b.effectEvery, 0, kEffectEveryChoices - 1))]);
    case ActionType::ProgramChange:
        if (b.programStep != 0)
            return std::string("Program ") + (b.programStep > 0 ? "+1" : "-1");
        return "Program " + std::to_string(b.number + 1);
    case ActionType::Freeze: {
        std::string s = "Freeze:";
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
        s += ", inverted";
    return s;
}

}  // namespace midipad
