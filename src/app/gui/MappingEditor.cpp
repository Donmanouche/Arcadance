#include "gui/MappingEditor.hpp"

#include "midipad/Engine.hpp"
#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <cstdio>

using namespace midipad;

namespace {

constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImVec4 kHintColor{0.65f, 0.65f, 0.7f, 1.0f};

float axisPosition(const LiveState& live, Axis a) {
    const float raw = static_cast<float>(live.axes[static_cast<std::size_t>(a)]) / 32767.0f;
    return std::clamp(isTrigger(a) ? raw : (raw + 1.0f) * 0.5f, 0.0f, 1.0f);
}

bool inputU7(const char* label, std::uint8_t& value, int min = 0, int max = 127) {
    int v = value;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (!ImGui::InputInt(label, &v))
        return false;
    v = std::clamp(v, min, max);
    if (v == value)
        return false;
    value = static_cast<std::uint8_t>(v);
    return true;
}

// A program number, shown 1-128 as on synths, kept 0-127.
bool inputProgram(const char* label, std::uint8_t& value) {
    int v = value + 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (!ImGui::InputInt(label, &v))
        return false;
    v = std::clamp(v, 1, 128);
    if (v - 1 == value)
        return false;
    value = static_cast<std::uint8_t>(v - 1);
    return true;
}

bool sliderU7(const char* label, std::uint8_t& value, int min = 0, int max = 127) {
    int v = value;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (!ImGui::SliderInt(label, &v, min, max))
        return false;
    value = static_cast<std::uint8_t>(std::clamp(v, min, max));
    return true;
}

bool modeRadio(Mode& mode) {
    bool changed = false;
    ImGui::TextUnformatted("Mode");
    ImGui::SameLine();
    if (ImGui::RadioButton("Momentary", mode == Mode::Momentary)) {
        changed = mode != Mode::Momentary;
        mode = Mode::Momentary;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Toggle", mode == Mode::Toggle)) {
        changed = mode != Mode::Toggle;
        mode = Mode::Toggle;
    }
    ImGui::SetItemTooltip("Momentary: on while the control is held.\n"
                          "Toggle: one press turns it on, the next turns it off.\n"
                          "Toggled notes and chords of the same family replace each other.");
    return changed;
}

// Live value bar plus the CC it currently produces.
void axisValueBar(const AxisBinding& a, float position, float width) {
    const float p = a.invert ? 1.0f - position : position;
    char overlay[32] = "";
    if (a.enabled) {
        const int value = a.min + static_cast<int>(std::lround(p * static_cast<float>(a.max - a.min)));
        std::snprintf(overlay, sizeof overlay, "%d", value);
    }
    ImGui::ProgressBar(p, ImVec2(width, 0), overlay);
}

std::vector<std::string> splitSpec(const std::string& spec) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= spec.size(); ++i) {
        if (i == spec.size() || spec[i] == ':' || spec[i] == ',') {
            parts.push_back(spec.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// Bank Select before a Program Change: MSB (CC 0) and LSB (CC 32), each -1 when not sent.
bool inputBankSelect(std::int16_t& msb, std::int16_t& lsb) {
    bool changed = false;
    bool bank = msb >= 0 || lsb >= 0;
    if (ImGui::Checkbox("Bank select", &bank)) {
        msb = static_cast<std::int16_t>(bank ? 0 : -1);
        lsb = -1;
        changed = true;
    }
    if (!bank)
        return changed;
    auto bankField = [&](const char* label, std::int16_t& v) {
        bool on = v >= 0;
        ImGui::SameLine();
        if (ImGui::Checkbox(label, &on)) {
            v = static_cast<std::int16_t>(on ? 0 : -1);
            changed = true;
        }
        if (on) {
            ImGui::SameLine();
            std::uint8_t value = static_cast<std::uint8_t>(v);
            ImGui::PushID(label);
            if (inputU7("##value", value)) {
                v = value;
                changed = true;
            }
            ImGui::PopID();
        }
    };
    bankField("MSB (CC 0)", msb);
    bankField("LSB (CC 32)", lsb);
    return changed;
}

}  // namespace

std::string MappingEditor::controlTitle(const std::string& spec) {
    const auto p = splitSpec(spec);
    if (p.size() == 2 && p[0] == "button")
        if (auto b = parseButton(p[1]))
            return label(*b);
    if (p.size() == 2 && p[0] == "dpad")
        if (auto d = parseHatDir(p[1]))
            return "D-pad: " + label(*d);
    if (p.size() == 3 && p[0] == "zone")
        if (auto z = parseZone(p[2]))
            return "Touchpad finger " + p[1] + ": " + label(*z);
    if (p.size() == 2 && p[0] == "axis")
        if (auto a = parseAxis(p[1]))
            return label(*a);
    if (p.size() == 3 && p[0] == "axes") {
        if (p[1] == "left_x")
            return "Left stick";
        if (p[1] == "right_x")
            return "Right stick";
        return "Triggers";
    }
    if (p.size() == 2 && p[0] == "finger")
        return "Touchpad finger " + p[1];
    if (p.size() == 1 && p[0] == "fingers")
        return "Touchpad";
    if (p.size() == 1 && p[0] == "notes")
        return "Notes";
    return spec;
}

bool MappingEditor::drawAxisForm(const char* id, AxisBinding& a, float position, bool trigger) {
    bool changed = false;
    ImGui::PushID(id);
    changed |= ImGui::Checkbox("Enabled", &a.enabled);
    ImGui::BeginDisabled(!a.enabled);
    // What the axis does.
    auto modeRadio = [&](const char* label, AxisMode mode) {
        ImGui::SameLine();
        if (ImGui::RadioButton(label, a.mode == mode) && a.mode != mode) {
            if (mode == AxisMode::Program) {
                a.min = 0;
                a.max = 15;
            } else if (a.mode == AxisMode::Program) {
                a.min = 0;
                a.max = 127;
            }
            a.mode = mode;
            changed = true;
        }
    };
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::Dummy(ImVec2(0, 0));
    modeRadio("CC", AxisMode::Cc);
    modeRadio("Program change", AxisMode::Program);
    modeRadio("Velocity", AxisMode::Velocity);
    modeRadio("Transpose", AxisMode::Transpose);
    auto hint = [](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };
    switch (a.mode) {
    case AxisMode::Program:
        changed |= inputProgram("From", a.min);
        ImGui::SameLine();
        changed |= inputProgram("To", a.max);
        changed |= ImGui::Checkbox("Invert", &a.invert);
        changed |= inputBankSelect(a.bankMsb, a.bankLsb);
        hint("The travel picks a program in this range, in the bank set above; it is sent once the control stays on "
             "it a moment, never when it springs back to rest.");
        break;
    case AxisMode::Velocity:
        changed |= ImGui::Checkbox("Invert", &a.invert);
        hint(trigger ? "Pressed, the trigger raises the velocity of the notes then played, from their mapping's "
                       "velocity up to 127 at the end of its travel."
                     : "Away from the middle, the velocity of the notes then played rises from their mapping's "
                       "velocity up to 127 one way, and falls down to 1 the other way. A stick at rest changes "
                       "nothing; a finger keeps its last value once lifted. A loop of this axis does the same.");
        break;
    case AxisMode::Transpose: {
        if (!trigger) {
            changed |= inputU7("Down", a.transposeDown, 0, kMaxTranspose);
            ImGui::SameLine();
        }
        changed |= inputU7("Up (semitones)", a.transposeUp, 0, kMaxTranspose);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Transposes");
        ImGui::SameLine();
        if (ImGui::RadioButton("the notes played", a.transposeTarget == TransposeTarget::Notes) &&
            a.transposeTarget != TransposeTarget::Notes) {
            a.transposeTarget = TransposeTarget::Notes;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("the note loop", a.transposeTarget == TransposeTarget::Loop) &&
            a.transposeTarget != TransposeTarget::Loop) {
            a.transposeTarget = TransposeTarget::Loop;
            changed = true;
        }
        changed |= ImGui::Checkbox("Invert", &a.invert);
        hint(trigger ? "Pressed, the trigger transposes up to this many semitones, by degrees of the scale when a "
                       "tonality is chosen. Notes take it when they start."
                     : "Away from the middle, it transposes down or up to these many semitones, by degrees of the "
                       "scale when a tonality is chosen. Notes take it when they start; the note loop is the active "
                       "channel's. A stick at rest changes nothing; a finger keeps its last value once lifted.");
        break;
    }
    case AxisMode::Cc:
        changed |= inputU7("CC", a.cc);
        changed |= inputU7("Min", a.min);
        ImGui::SameLine();
        changed |= inputU7("Max", a.max);
        changed |= ImGui::Checkbox("Invert", &a.invert);
        break;
    }
    ImGui::EndDisabled();
    if (a.mode == AxisMode::Cc)
        axisValueBar(a, position, ImGui::GetFontSize() * 14);
    ImGui::PopID();
    return changed;
}

bool MappingEditor::drawFingerForm(int finger, FingerBinding& f, const LiveState& live) {
    bool changed = false;
    static const char* kModes[] = {"Off", "X/Y position -> CC", "4 zones -> pads"};
    ImGui::PushID(finger);
    int mode = static_cast<int>(f.mode);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::Combo("Mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
        f.mode = static_cast<TouchMode>(mode);
        changed = true;
    }
    const auto& live1 = live.fingers[static_cast<std::size_t>(finger)];
    if (f.mode == TouchMode::Axis) {
        ImGui::SeparatorText("X (left -> right)");
        changed |= drawAxisForm("x", f.x, live1.down ? live1.x : 0.0f);
        ImGui::SeparatorText("Y (bottom -> top)");
        changed |= drawAxisForm("y", f.y, live1.down ? 1.0f - live1.y : 0.0f);
    } else if (f.mode == TouchMode::Zones) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("Click a zone in the touchpad module to choose what it plays.");
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return changed;
}

bool MappingEditor::drawControl(const std::string& spec, Mapping& m, const LiveState& live,
                                const std::optional<Key>& key) {
    key_ = key;
    const auto p = splitSpec(spec);
    ImGui::PushID(spec.c_str());
    bool changed = false;
    TriggerBinding* trigger = nullptr;
    if (p.size() == 2 && p[0] == "button") {
        if (auto b = parseButton(p[1]); b && !isDpad(*b)) {
            selection_ = {Selection::Kind::Button, static_cast<int>(*b)};
            trigger = &m.buttons[static_cast<std::size_t>(*b)];
        }
    } else if (p.size() == 2 && p[0] == "dpad") {
        if (auto d = parseHatDir(p[1])) {
            selection_ = {Selection::Kind::Dpad, static_cast<int>(*d)};
            trigger = &m.dpad[static_cast<std::size_t>(*d)];
        }
    } else if (p.size() == 3 && p[0] == "zone" && (p[1] == "1" || p[1] == "2")) {
        if (auto z = parseZone(p[2])) {
            const int finger = p[1][0] - '1';
            selection_ = {Selection::Kind::Zone, static_cast<int>(*z), finger};
            trigger = &m.fingers[static_cast<std::size_t>(finger)].zones[static_cast<std::size_t>(*z)];
        }
    } else if ((p.size() == 2 && p[0] == "axis") || (p.size() == 3 && p[0] == "axes")) {
        for (std::size_t i = 1; i < p.size(); ++i) {
            const auto a = parseAxis(p[i]);
            if (!a)
                continue;
            if (p.size() == 3)
                ImGui::SeparatorText(label(*a).c_str());
            changed |= drawAxisForm(p[i].c_str(), m.axes[static_cast<std::size_t>(*a)], axisPosition(live, *a),
                                    *a == Axis::LeftTrigger || *a == Axis::RightTrigger);
        }
        // A whole stick, or both triggers: their loop too.
        if (p.size() == 3) {
            if (const auto a = parseAxis(p[1]); a && (*a == Axis::LeftX || *a == Axis::RightX))
                changed |= drawLoopForm(*a == Axis::LeftX ? 0 : 1, m);
            else if (a && *a == Axis::LeftTrigger)
                changed |= drawLoopForm(static_cast<int>(LoopSource::Triggers), m);
        }
    } else if (p.size() == 2 && p[0] == "finger" && (p[1] == "1" || p[1] == "2")) {
        const int finger = p[1][0] - '1';
        changed |= drawFingerForm(finger, m.fingers[static_cast<std::size_t>(finger)], live);
        changed |= drawLoopForm(2 + finger, m);
    } else if (p.size() == 1 && p[0] == "notes") {
        changed |= drawLoopForm(static_cast<int>(LoopSource::Notes), m);
    } else if (p.size() == 1 && p[0] == "fingers") {
        for (int f = 0; f < kFingerCount; ++f) {
            ImGui::SeparatorText(f == 0 ? "Finger 1" : "Finger 2");
            changed |= drawFingerForm(f, m.fingers[static_cast<std::size_t>(f)], live);
            changed |= drawLoopForm(2 + f, m);
        }
    }
    if (trigger)
        changed |= drawTriggerEditor(*trigger);
    ImGui::PopID();
    return changed;
}

// The loop a Loop record button captures from a stick or finger (a
// LoopSource): how it plays, and on which CCs, beside the source's own.
bool MappingEditor::drawLoopForm(int source, Mapping& m) {
    static constexpr const char* kTitles[] = {"Left stick loop (recorded movement)", "Right stick loop (recorded movement)",
                                              "Finger 1 loop (recorded movement)", "Finger 2 loop (recorded movement)",
                                              "Triggers loop (recorded travel)", "Note loop (recorded notes and chords)"};
    bool changed = false;
    LoopBinding& loop = m.loops[static_cast<std::size_t>(source)];
    ImGui::PushID(source);
    ImGui::SeparatorText(kTitles[source]);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Playback");
    const std::pair<LoopPlayback, const char*> playbacks[] = {
        {LoopPlayback::Forward, "Forward"}, {LoopPlayback::Reverse, "Reverse"}, {LoopPlayback::BackAndForth, "Back & forth"}};
    for (auto [playback, text] : playbacks) {
        ImGui::SameLine();
        if (ImGui::RadioButton(text, loop.playback == playback) && loop.playback != playback) {
            loop.playback = playback;
            changed = true;
        }
    }
    if (source == static_cast<int>(LoopSource::Notes)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("Hold Record and play notes or chords (buttons, D-pad, triggers as buttons): they loop on "
                           "the channel where they were played. What still sounds is cut when the loop starts over. "
                           "The touchpad pads have their own loops.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Clear the loop"))
            clearLoopRequest_ = source;
        ImGui::PopID();
        return changed;
    }
    // What the source itself sends.
    const bool finger = source == 2 || source == 3;
    const bool triggers = source == static_cast<int>(LoopSource::Triggers);
    const FingerBinding* f = finger ? &m.fingers[static_cast<std::size_t>(source - 2)] : nullptr;
    if (finger && f->mode != TouchMode::Axis) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped(f->mode == TouchMode::Zones
                               ? "In 4-pad mode the pads play notes: Record puts them in the note loop, with the "
                                 "buttons' notes (see the piano roll); a tap over it adds more in overdub."
                               : "The finger is off: nothing to record.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Clear the loop"))
            clearLoopRequest_ = source;
        ImGui::PopID();
        return changed;
    }
    changed |= inputU7(triggers ? "Loop CC for L2" : "Loop CC for X", loop.ccX);
    changed |= inputU7(triggers ? "Loop CC for R2" : "Loop CC for Y", loop.ccY);
    const Axis axisX = triggers ? Axis::LeftTrigger : source == 0 ? Axis::LeftX : Axis::RightX;
    const Axis axisY = triggers ? Axis::RightTrigger : source == 0 ? Axis::LeftY : Axis::RightY;
    const AxisBinding& own = finger ? f->x : m.axes[static_cast<std::size_t>(axisX)];
    const AxisBinding& ownY = finger ? f->y : m.axes[static_cast<std::size_t>(axisY)];

    // The loop plays beside the sticks and fingers: a CC they send too would fight.
    std::string clash;
    auto check = [&](const AxisBinding& b, const std::string& name) {
        if (b.enabled && b.sendsCc() && ((own.sendsCc() && b.cc == loop.ccX) || (ownY.sendsCc() && b.cc == loop.ccY)))
            clash += (clash.empty() ? "" : ", ") + name + " (CC " + std::to_string(b.cc) + ")";
    };
    for (int a = 0; a < kAxisCount; ++a)
        check(m.axes[static_cast<std::size_t>(a)], label(static_cast<Axis>(a)));
    for (int k = 0; k < kFingerCount; ++k) {
        const FingerBinding& other = m.fingers[static_cast<std::size_t>(k)];
        if (other.mode == TouchMode::Axis) {
            check(other.x, "finger " + std::to_string(k + 1) + " X");
            check(other.y, "finger " + std::to_string(k + 1) + " Y");
        }
    }
    if (!clash.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("Also sent by %s: both would fight. Choose other CCs.", clash.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        const char* what = finger ? "finger" : triggers ? "triggers" : "stick";
        // Each axis of the loop: on its loop CC, or as the Velocity / Transpose its axis is set to.
        auto plays = [](const AxisBinding& axis, int loopCc) {
            if (axis.enabled && axis.mode == AxisMode::Velocity)
                return std::string("as Velocity");
            if (axis.enabled && axis.mode == AxisMode::Transpose)
                return std::string("as Transpose");
            return "on CC " + std::to_string(loopCc);
        };
        const std::string text = std::string("The loop plays beside the ") + what + ": " + (triggers ? "L2 " : "X ") +
                                 plays(own, loop.ccX) + ", " + (triggers ? "R2 " : "Y ") + plays(ownY, loop.ccY) + ".";
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Clear the loop"))
        clearLoopRequest_ = source;
    ImGui::PopID();
    return changed;
}

bool MappingEditor::drawTriggerEditor(TriggerBinding& b) {
    bool changed = false;
    static const char* kTypes[] = {"No action", "Note", "CC", "Chord", "Transport", "Channel change", "Axis freeze",
                                   "Loop record", "Program change", "Loop stretch"};
    int type = static_cast<int>(b.type);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Action", &type, kTypes, IM_ARRAYSIZE(kTypes)) && type != static_cast<int>(b.type)) {
        TriggerBinding fresh;
        fresh.type = static_cast<ActionType>(type);
        fresh.mode = b.mode;
        if (fresh.type == ActionType::Note)
            fresh.number = 60;
        if (fresh.type == ActionType::Chord) {
            fresh.chordName = "C";
            fresh.chord = *parseChord("C");
        }
        if (fresh.type == ActionType::Freeze)
            fresh.freezeAxes = 0b11;
        b = fresh;
        chordTextFor_ = {};
        changed = true;
    }
    ImGui::Spacing();

    switch (b.type) {
    case ActionType::None:
        break;
    case ActionType::Note:
        changed |= drawNoteField(b.number);
        changed |= sliderU7("Velocity", b.velocity, 1, 127);
        changed |= modeRadio(b.mode);
        changed |= drawHumanizeCheckbox(b);
        break;
    case ActionType::ControlChange:
        changed |= inputU7("CC number", b.number);
        changed |= sliderU7("Value on press", b.onValue);
        changed |= sliderU7("Value on release", b.offValue);
        changed |= modeRadio(b.mode);
        break;
    case ActionType::Chord:
        changed |= drawChordFields(b);
        changed |= sliderU7("Velocity", b.velocity, 1, 127);
        changed |= modeRadio(b.mode);
        changed |= drawHumanizeCheckbox(b);
        break;
    case ActionType::Transport: {
        const std::pair<TransportCommand, const char*> commands[] = {
            {TransportCommand::Start, "Start"},
            {TransportCommand::Stop, "Stop"},
            {TransportCommand::Continue, "Continue"},
        };
        for (auto [command, text] : commands) {
            if (ImGui::RadioButton(text, b.transport == command) && b.transport != command) {
                b.transport = command;
                changed = true;
            }
        }
        break;
    }
    case ActionType::ChannelStep: {
        if (ImGui::RadioButton("Previous channel (-1)", b.channelStep == -1) && b.channelStep != -1) {
            b.channelStep = -1;
            changed = true;
        }
        if (ImGui::RadioButton("Next channel (+1)", b.channelStep == 1) && b.channelStep != 1) {
            b.channelStep = 1;
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("The channel wraps from 16 to 1. If the preset has a mapping for the new channel, "
                           "it applies at once.");
        ImGui::PopStyleColor();
        break;
    }
    case ActionType::Freeze: {
        ImGui::TextUnformatted("Axes held at their current value:");
        for (int a = 0; a < kAxisCount; ++a) {
            bool on = b.freezeAxes & (1u << a);
            if (ImGui::Checkbox(label(static_cast<Axis>(a)).c_str(), &on)) {
                const auto bit = static_cast<std::uint8_t>(1u << a);
                const auto next = static_cast<std::uint8_t>(on ? b.freezeAxes | bit : b.freezeAxes & ~bit);
                if (next != 0) {  // a freeze needs at least one axis
                    b.freezeAxes = next;
                    changed = true;
                }
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("One press freezes, the next one releases.");
        ImGui::PopStyleColor();
        break;
    }
    case ActionType::ProgramChange: {
        const std::pair<int, const char*> kinds[] = {{0, "Program"}, {1, "Next (+1)"}, {-1, "Previous (-1)"}};
        for (auto [step, text] : kinds) {
            if (step != 0)
                ImGui::SameLine();
            if (ImGui::RadioButton(text, b.programStep == step) && b.programStep != step) {
                b.programStep = static_cast<std::int8_t>(step);
                changed = true;
            }
        }
        if (b.programStep == 0)
            changed |= inputProgram("Program", b.number);
        else
            changed |= ImGui::Checkbox("Wrap around (128 -> 1)", &b.programWrap);
        changed |= inputBankSelect(b.bankMsb, b.bankLsb);
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("Sent on the active channel. Next and Previous step from the last program sent there.");
        ImGui::PopStyleColor();
        break;
    }
    case ActionType::LoopStretch:
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("Hold, then move a stick or a finger on the touchpad left or right to stretch or compress "
                           "its loop, as Shift + mouse wheel over its LOOP panel does. A stick pushed further turns "
                           "faster; on the touchpad, each slide of the finger counts. Meanwhile they send no CC. The "
                           "stretch stays when you release.");
        ImGui::PopStyleColor();
        break;
    case ActionType::LoopRecord:
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("Hold to record the stick that moves (left or right, or both); release to play it in a "
                           "loop, beside the stick, on the loop CCs set in the stick's panel. A press without moving "
                           "any stick stops the loops.");
        ImGui::PopStyleColor();
        break;
    }
    return changed;
}

const std::vector<KeyChord>& MappingEditor::keyChords() {
    if (key_ != chordsFor_) {
        chordsFor_ = key_;
        chords_ = key_ ? chordsInKey(*key_) : std::vector<KeyChord>{};
    }
    return chords_;
}

bool MappingEditor::drawNoteField(std::uint8_t& note) {
    if (!key_) {
        const bool changed = inputU7("Note", note);
        ImGui::SameLine();
        ImGui::Text("%s", noteName(note).c_str());
        return changed;
    }

    // With a tonality: only the notes of the scale, on every octave. A note
    // outside it (from an older preset) stays listed so it is not lost.
    bool changed = false;
    const Key& key = *key_;
    auto label = [&](int n) {
        return noteName(n, flats()) + "  (" + std::to_string(n) + ")" + (inKey(key, n) ? "" : "  out of key");
    };
    auto step = [&](int direction) {
        for (int n = note + direction; n >= 0 && n <= 127; n += direction) {
            if (inKey(key, n)) {
                note = static_cast<std::uint8_t>(n);
                changed = true;
                return;
            }
        }
    };
    if (ImGui::ArrowButton("##notedown", ImGuiDir_Left))
        step(-1);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::BeginCombo("##note", label(note).c_str(), ImGuiComboFlags_HeightLarge)) {
        if (!inKey(key, note)) {
            ImGui::Selectable(label(note).c_str(), true);
            ImGui::Separator();
        }
        for (int n = 0; n <= 127; ++n) {
            if (!inKey(key, n))
                continue;
            const bool selected = n == note;
            if (ImGui::Selectable(label(n).c_str(), selected) && !selected) {
                note = static_cast<std::uint8_t>(n);
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::ArrowButton("##noteup", ImGuiDir_Right))
        step(+1);
    ImGui::SameLine();
    ImGui::TextUnformatted("Note");
    ImGui::SetItemTooltip("Notes of %s; the arrows step to the neighboring note of the scale.", keyName(key).c_str());
    return changed;
}

bool MappingEditor::drawHumanizeCheckbox(TriggerBinding& b) {
    const bool changed = ImGui::Checkbox("Humanize", &b.humanize);
    ImGui::SetItemTooltip("Random velocity and timing on the channels chosen in Options; "
                          "each note of a chord varies on its own.");
    if (b.humanize && humanizeChannels_ == 0) {
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "off on every channel (Options)");
    }
    return changed;
}

bool MappingEditor::drawChordFields(TriggerBinding& b) {
    bool changed = false;
    // The text field keeps what is typed even while it is not a valid chord;
    // the binding only takes valid names.
    if (!(chordTextFor_ == selection_)) {
        chordTextFor_ = selection_;
        chordText_ = b.chordName;
    }

    auto apply = [&](const std::string& text) {
        if (auto chord = parseChord(text, b.chordOctave, b.chordRootBelow)) {
            b.chordName = text;
            b.chord = *chord;
            chordText_ = text;
            changed = true;
        }
    };

    // With a tonality: every chord of the scale, by degree.
    if (key_) {
        static constexpr const char* kDegrees[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
        const auto& chords = keyChords();
        const auto scale = scalePitchClasses(*key_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        const std::string preview = b.chordName.empty() ? std::string("-") : b.chordName;
        if (ImGui::BeginCombo("Chord of the key", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
            int degree = -1;
            for (const auto& chord : chords) {
                if (chord.degree != degree) {
                    degree = chord.degree;
                    const std::string title = std::string(degree < 7 ? kDegrees[degree] : "") + "  " +
                                              pitchName(scale[static_cast<std::size_t>(degree)], flats());
                    ImGui::SeparatorText(title.c_str());
                }
                std::string notes;
                for (auto n : chord.notes.view())
                    notes += pitchName(n, flats()) + " ";
                const std::string text = chord.name + "##" + chord.name;
                const bool selected = chord.name == b.chordName;
                if (ImGui::Selectable(text.c_str(), selected))
                    apply(chord.name);
                if (selected)
                    ImGui::SetItemDefaultFocus();
                ImGui::SameLine(ImGui::GetFontSize() * 7);
                ImGui::TextDisabled("%s", notes.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%zu chords whose notes all belong to %s", chords.size(),
                              keyName(*key_).c_str());
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::InputText(key_ ? "Free entry" : "Chord", &chordText_)) {
        if (auto chord = parseChord(chordText_, b.chordOctave, b.chordRootBelow)) {
            b.chordName = chordText_;
            b.chord = *chord;
            changed = true;
        }
    }

    const auto typed = parseChord(chordText_, b.chordOctave, b.chordRootBelow);
    if (!typed && !chordText_.empty()) {
        ImGui::TextColored(kErrorColor, "Unknown chord (the previous one stays active)");
    } else if (b.chord.count) {
        std::string notes;
        bool outside = false;
        for (auto n : b.chord.view()) {
            notes += noteName(n, flats()) + " ";
            outside |= key_ && !inKey(*key_, n);
        }
        ImGui::Text("Notes: %s", notes.c_str());
        if (outside)
            ImGui::TextColored(kErrorColor, "Contains notes outside %s", keyName(*key_).c_str());
    }

    // The chord's root, for the octave and the bass below.
    const auto rootPitch = parsePitch(b.chordName);

    // Octave: the same chord higher or lower.
    if (rootPitch) {
        int octave = b.chordOctave;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::InputInt("Octave", &octave)) {
            octave = std::clamp(octave, kMinChordOctave, kMaxChordOctave);
            if (auto chord = parseChord(b.chordName, octave, b.chordRootBelow)) {
                b.chordOctave = static_cast<std::int8_t>(octave);
                b.chord = *chord;
                changed = true;
            }
        }
        ImGui::SetItemTooltip("Octave of the root note: 4 places C on C4 (MIDI 60)");
        ImGui::SameLine();
        ImGui::TextDisabled("root %s", noteName((b.chordOctave + 1) * 12 + *rootPitch, flats()).c_str());

        // The root doubled lower, to anchor the bass.
        static constexpr const char* kBelow[] = {"None", "1 octave below", "2 octaves below"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 11, 0), ImVec2(FLT_MAX, FLT_MAX));  // no scrollbar
        if (ImGui::BeginCombo("Bass root", kBelow[std::min<int>(b.chordRootBelow, 2)])) {
            for (int below = 0; below <= 2; ++below) {
                const auto chord = parseChord(b.chordName, b.chordOctave, below);
                std::string label = kBelow[below];
                if (below > 0)
                    label += "  (" + noteName((b.chordOctave + 1 - below) * 12 + *rootPitch, flats()) + ")";
                ImGui::BeginDisabled(!chord);
                if (ImGui::Selectable(label.c_str(), below == b.chordRootBelow) && chord) {
                    b.chordRootBelow = static_cast<std::uint8_t>(below);
                    b.chord = *chord;
                    changed = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Also plays the root one or two octaves lower, to anchor the bass of the chord");
    }
    return changed;
}
