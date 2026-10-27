#include "gui/MappingEditor.hpp"

#include "midipad/Engine.hpp"
#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"
#include "gui/PerformanceView.hpp"

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

std::vector<std::string> MappingEditor::splitSpec(const std::string& spec) {
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
    if (p.size() == 1 && p[0] == "tilt")
        return "Tilt";
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
    // "##mode": the same words label fields below (the CC number...); ImGui needs distinct IDs.
    modeRadio("CC##mode", AxisMode::Cc);
    modeRadio("Program change##mode", AxisMode::Program);
    modeRadio("Velocity##mode", AxisMode::Velocity);
    modeRadio("Transpose##mode", AxisMode::Transpose);
    switch (a.mode) {
    case AxisMode::Program:
        changed |= inputProgram("From", a.min);
        ImGui::SameLine();
        changed |= inputProgram("To", a.max);
        changed |= ImGui::Checkbox("Invert", &a.invert);
        changed |= inputBankSelect(a.bankMsb, a.bankLsb);
        break;
    case AxisMode::Velocity: {
        changed |= ImGui::Checkbox("Invert", &a.invert);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Shapes");
        static constexpr std::pair<VelocityTarget, const char*> kTargets[] = {
            {VelocityTarget::Notes, "the notes played"}, {VelocityTarget::Loop, "the note loop"}, {VelocityTarget::Both, "both"}};
        for (const auto& [target, name] : kTargets) {
            ImGui::SameLine();
            if (ImGui::RadioButton(name, a.velocityTarget == target) && a.velocityTarget != target) {
                a.velocityTarget = target;
                changed = true;
            }
        }
        break;
    }
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
    }
    ImGui::PopID();
    return changed;
}

// The tilt: on or off, what makes it move, how far it goes, its axes.
bool MappingEditor::drawTiltForm(Mapping& m, const LiveState& live) {
    TiltBinding& t = m.tilt;
    bool changed = false;
    ImGui::PushID("tilt");
    // The buttons that may make it move: L1, R1, L2, R2 and the D-pad's four arrows.
    static constexpr Button kHolds[] = {Button::LeftShoulder, Button::RightShoulder, Button::LeftTrigger, Button::RightTrigger,
                                        Button::DpadUp, Button::DpadDown, Button::DpadLeft, Button::DpadRight};
    auto binding = [&](Button b) -> const TriggerBinding& {  // a D-pad arrow's is under its direction
        switch (b) {
        case Button::DpadUp: return m.dpad[static_cast<std::size_t>(HatDir::Up)];
        case Button::DpadDown: return m.dpad[static_cast<std::size_t>(HatDir::Down)];
        case Button::DpadLeft: return m.dpad[static_cast<std::size_t>(HatDir::Left)];
        case Button::DpadRight: return m.dpad[static_cast<std::size_t>(HatDir::Right)];
        default: return m.buttons[static_cast<std::size_t>(b)];
        }
    };
    // A button that is Shift is named so: held, it would be Shift too.
    auto isShift = [&](Button b) { return binding(b).type == ActionType::Shift; };
    auto holdName = [&](Button b) { return label(b) + (isShift(b) ? " (Shift)" : ""); };
    if (ImGui::Checkbox("Enabled", &t.enabled)) {
        changed = true;
        // Turned on with Shift as its button: the first free one instead.
        if (t.enabled && t.whileHeld && isShift(t.hold)) {
            const Button* free = std::find_if(std::begin(kHolds), std::end(kHolds), [&](Button b) {
                return live.has(b) && !binding(b).isSet();
            });
            if (free == std::end(kHolds))
                free = std::find_if(std::begin(kHolds), std::end(kHolds), [&](Button b) { return live.has(b) && !isShift(b); });
            if (free != std::end(kHolds))
                t.hold = *free;
        }
    }
    ImGui::BeginDisabled(!t.enabled);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Active");
    ImGui::SameLine();
    if (ImGui::RadioButton("Always", !t.whileHeld) && t.whileHeld) {
        t.whileHeld = false;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("While held", t.whileHeld) && !t.whileHeld) {
        t.whileHeld = true;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!t.whileHeld);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::BeginCombo("##hold", holdName(t.hold).c_str())) {
        for (const Button button : kHolds) {
            if (!live.has(button))
                continue;
            if (ImGui::Selectable(holdName(button).c_str(), t.hold == button) && t.hold != button) {
                t.hold = button;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    changed |= ImGui::SliderInt("Angle", &t.angle, kMinTiltAngle, kMaxTiltAngle, "%d\xC2\xB0 each way", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    if (ImGui::Button("Center"))
        recenterTiltRequest_ = true;
    ImGui::SeparatorText("X (left side down -> right side down)");
    changed |= drawAxisForm("x", t.x, live.tiltActive ? live.tiltX : 0.0f);
    ImGui::SeparatorText("Y (towards you -> forward)");
    changed |= drawAxisForm("y", t.y, live.tiltActive ? live.tiltY : 0.0f);
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

bool MappingEditor::drawControl(const std::string& spec, Mapping& m, const LiveState& live,
                                const std::optional<Key>& key) {
    key_ = key;
    escapeTaken_ = false;
    const auto p = splitSpec(spec);
    ImGui::PushID(spec.c_str());
    bool changed = false;
    TriggerBinding* trigger = nullptr;
    TriggerBinding* shifted = nullptr;  // its entry in the Shift layer
    if (p.size() == 2 && p[0] == "button") {
        if (auto b = parseButton(p[1]); b && !isDpad(*b)) {
            selection_ = {Selection::Kind::Button, static_cast<int>(*b)};
            trigger = &m.buttons[static_cast<std::size_t>(*b)];
            shifted = &m.shift.buttons[static_cast<std::size_t>(*b)];
        }
    } else if (p.size() == 2 && p[0] == "dpad") {
        if (auto d = parseHatDir(p[1])) {
            selection_ = {Selection::Kind::Dpad, static_cast<int>(*d)};
            trigger = &m.dpad[static_cast<std::size_t>(*d)];
            shifted = &m.shift.dpad[static_cast<std::size_t>(*d)];
        }
    } else if (p.size() == 3 && p[0] == "zone" && (p[1] == "1" || p[1] == "2")) {
        if (auto z = parseZone(p[2])) {
            const int finger = p[1][0] - '1';
            selection_ = {Selection::Kind::Zone, static_cast<int>(*z), finger};
            trigger = &m.fingers[static_cast<std::size_t>(finger)].zones[static_cast<std::size_t>(*z)];
            shifted = &m.shift.zones[static_cast<std::size_t>(finger)][static_cast<std::size_t>(*z)];
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
                changed |= drawLoopForm(*a == Axis::LeftX ? 0 : 1, m, live);
            else if (a && *a == Axis::LeftTrigger)
                changed |= drawLoopForm(static_cast<int>(LoopSource::Triggers), m, live);
        }
    } else if (p.size() == 2 && p[0] == "finger" && (p[1] == "1" || p[1] == "2")) {
        const int finger = p[1][0] - '1';
        changed |= drawFingerForm(finger, m.fingers[static_cast<std::size_t>(finger)], live);
        changed |= drawLoopForm(2 + finger, m, live);
    } else if (p.size() == 1 && p[0] == "notes") {
        changed |= drawLoopForm(static_cast<int>(LoopSource::Notes), m, live);
    } else if (p.size() == 1 && p[0] == "tilt") {
        changed |= drawTiltForm(m, live);
        changed |= drawLoopForm(static_cast<int>(LoopSource::Tilt), m, live);
    } else if (p.size() == 1 && p[0] == "fingers") {
        for (int f = 0; f < kFingerCount; ++f) {
            ImGui::SeparatorText(f == 0 ? "Finger 1" : "Finger 2");
            changed |= drawFingerForm(f, m.fingers[static_cast<std::size_t>(f)], live);
            changed |= drawLoopForm(2 + f, m, live);
        }
    }
    if (trigger && shifted && trigger->type != ActionType::Shift) {
        // Two layers: what it plays normally, and while Shift is on. Another
        // control opens on the layer shown last, on Shift's while Shift is on.
        const bool newControl = spec != layerSpec_;
        if (newControl) {
            layerSpec_ = spec;
            shiftLayer_ = shiftLayer_ || live.stretchHeld;
        }
        if (ImGui::BeginTabBar("##layer")) {
            if (ImGui::BeginTabItem("Normal", nullptr, newControl && !shiftLayer_ ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!newControl)
                    shiftLayer_ = false;
                ImGui::Spacing();
                changed |= drawTriggerEditor(*trigger);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(shifted->isSet() ? "Shift *###shift" : "Shift###shift", nullptr,
                                    newControl && shiftLayer_ ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!newControl)
                    shiftLayer_ = true;
                ImGui::Spacing();
                changed |= drawTriggerEditor(*shifted);
                if (shifted->type == ActionType::Shift) {  // no Shift inside the Shift layer
                    *shifted = TriggerBinding{};
                    changed = true;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else if (trigger) {
        changed |= drawTriggerEditor(*trigger);
    }
    ImGui::PopID();
    return changed;
}

// Each axis loop of the source playing: the loop whose amplitude it
// modulates (Engine::setLoopModulation), how deep, which way, and whether it
// still sends its own CC. Kept in the clip, not in the preset.
void MappingEditor::drawModulationForm(int source, const LiveState& live) {
    auto statusOf = [&](int track) -> const LoopStatus& {
        return live.loops[static_cast<std::size_t>(Engine::sourceOfTrack(track))].lanes[static_cast<std::size_t>(Engine::axisOfTrack(track))].status;
    };
    bool any = false;
    for (int axis = 0; axis < 2; ++axis)
        any = any || statusOf(Engine::trackOf(source, axis)).state == LoopStatus::State::Playing;
    if (!any)
        return;
    ImGui::SeparatorText("Modulation");
    const bool triggers = source == static_cast<int>(LoopSource::Triggers);
    const float column = ImGui::GetFontSize() * 7.5f, comboW = ImGui::GetFontSize() * 9;
    for (int axis = 0; axis < 2; ++axis) {
        const int self = Engine::trackOf(source, axis);
        const LoopStatus& st = statusOf(self);
        if (st.state != LoopStatus::State::Playing)
            continue;
        ImGui::PushID(axis);
        const std::string name = triggers ? label(axis == 0 ? Button::LeftTrigger : Button::RightTrigger) : (axis == 0 ? "X" : "Y");
        // The loops it may modulate: playing, never one it is modulated by (even through others).
        auto allowed = [&](int track) {
            if (track == self || statusOf(track).state != LoopStatus::State::Playing)
                return false;
            for (int at = self, hops = 0; at >= 0 && hops < Engine::kNoteTrack; ++hops) {
                at = statusOf(at).modSource;
                if (at == track)
                    return false;
            }
            return true;
        };
        auto choose = [&](const char* id, int current) {  // a combo of the loops; the one chosen, or -1
            int picked = -1;
            ImGui::SetNextItemWidth(comboW);
            if (ImGui::BeginCombo(id, current >= 0 ? PerformanceView::trackName(current).c_str() : (current == -1 ? "\xE2\x80\x94" : "+"))) {
                for (int track = 0; track < Engine::kNoteTrack; ++track) {
                    if (track == self || (statusOf(track).modSource == self && track != current))
                        continue;  // itself, or one it already modulates
                    if (ImGui::Selectable(PerformanceView::trackName(track).c_str(), track == current, allowed(track) ? 0 : ImGuiSelectableFlags_Disabled) && track != current)
                        picked = track;
                }
                ImGui::EndCombo();
            }
            return picked;
        };
        bool first = true;
        auto label = [&] {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(first ? (std::string(name) + " modulates").c_str() : "");
            ImGui::SameLine(column);
            first = false;
        };
        // A row per loop it modulates: which, how deep, which way, x to stop.
        for (int track = 0; track < Engine::kNoteTrack; ++track) {
            const LoopStatus& t = statusOf(track);
            if (t.modSource != self)
                continue;
            ImGui::PushID(track);
            label();
            LoopModulation m{self, t.modDepth, t.modInverted};
            if (const int other = choose("##target", track); other >= 0)  // moved to another loop
                modulationRequest_ = Modulation{other, m, std::nullopt, track};
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            bool changed = ImGui::SliderInt("##depth", &m.depth, 0, 100, "depth %d %%", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("inverted", &m.inverted);
            ImGui::SameLine();
            if (ImGui::SmallButton("\xC3\x97"))
                modulationRequest_ = Modulation{track, {}, std::nullopt};
            else if (changed)
                modulationRequest_ = Modulation{track, m, std::nullopt};
            ImGui::PopID();
        }
        const bool modulates = st.modTargets != 0;
        label();
        if (const int added = choose("##add", modulates ? -2 : -1); added >= 0)
            modulationRequest_ = Modulation{added, {self, 100, false}, std::nullopt};
        if (modulates) {
            ImGui::SetCursorPosX(column);
            bool sends = st.modSendsCc;
            if (ImGui::Checkbox((std::string(name) + " sends its CC").c_str(), &sends))
                modulationRequest_ = Modulation{self, {}, sends};
        }
        ImGui::PopID();
    }
}

// The loop a Loop record button captures from a stick or finger (a
// LoopSource): how it plays, and on which CCs, beside the source's own.
bool MappingEditor::drawLoopForm(int source, Mapping& m, const LiveState& live) {
    static constexpr const char* kTitles[] = {"Left stick loop (recorded movement)", "Right stick loop (recorded movement)",
                                              "Finger 1 loop (recorded movement)", "Finger 2 loop (recorded movement)",
                                              "Triggers loop (recorded travel)", "Tilt loop (recorded movement)",
                                              "Note loop (recorded notes and chords)"};
    bool changed = false;
    LoopBinding& loop = m.loops[static_cast<std::size_t>(source)];
    ImGui::PushID(source);
    ImGui::SeparatorText(kTitles[source]);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Playback");
    // A movement loop may also be an envelope: once from its start at each note of the channel.
    const bool notes = source == static_cast<int>(LoopSource::Notes);
    const std::pair<LoopPlayback, const char*> playbacks[] = {{LoopPlayback::Forward, "Forward"},
                                                              {LoopPlayback::Reverse, "Reverse"},
                                                              {LoopPlayback::BackAndForth, "Back & forth"},
                                                              {LoopPlayback::Envelope, "Envelope"}};
    for (auto [playback, text] : playbacks) {
        if (notes && playback == LoopPlayback::Envelope)
            continue;
        ImGui::SameLine();
        if (ImGui::RadioButton(text, loop.playback == playback) && loop.playback != playback) {
            loop.playback = playback;
            changed = true;
        }
    }
    if (!notes && loop.playback == LoopPlayback::Envelope) {
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Legato", &loop.legato);
        ImGui::SetItemTooltip("A note played while another sounds does not start the envelope again");
    }
    if (source == static_cast<int>(LoopSource::Notes)) {
        if (ImGui::Button("Clear the loop"))
            clearLoopRequest_ = ClearLoop{source, -1};
        ImGui::PopID();
        return changed;
    }
    // What the source itself sends.
    const bool finger = source == 2 || source == 3;
    const bool triggers = source == static_cast<int>(LoopSource::Triggers);
    const FingerBinding* f = finger ? &m.fingers[static_cast<std::size_t>(source - 2)] : nullptr;
    if (finger && f->mode != TouchMode::Axis) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        if (f->mode != TouchMode::Zones)
            ImGui::TextWrapped("The finger is off: nothing to record.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Clear the loop"))
            clearLoopRequest_ = ClearLoop{source, -1};
        ImGui::PopID();
        return changed;
    }
    const Axis axisX = triggers ? Axis::LeftTrigger : source == 0 ? Axis::LeftX : Axis::RightX;
    const Axis axisY = triggers ? Axis::RightTrigger : source == 0 ? Axis::LeftY : Axis::RightY;
    const bool tilt = source == static_cast<int>(LoopSource::Tilt);
    const AxisBinding& own = finger ? f->x : tilt ? m.tilt.x : m.axes[static_cast<std::size_t>(axisX)];
    const AxisBinding& ownY = finger ? f->y : tilt ? m.tilt.y : m.axes[static_cast<std::size_t>(axisY)];
    // Each axis of the loop: on its loop CC, or as the Velocity / Transpose its axis is set to.
    auto shapesAs = [](const AxisBinding& axis) -> const char* {
        if (axis.enabled && axis.mode == AxisMode::Velocity)
            return "Velocity";
        if (axis.enabled && axis.mode == AxisMode::Transpose)
            return "Transpose";
        return nullptr;
    };
    // On the axis' own CC (the control taking over while it moves), or on CCs of its own.
    const char* what = finger ? "finger" : triggers ? "triggers" : tilt ? "tilt" : "stick";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Plays on");
    ImGui::SameLine();
    if (ImGui::RadioButton("The axis' CC", !loop.ownCcs) && loop.ownCcs) {
        loop.ownCcs = false;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Its own CCs", loop.ownCcs) && !loop.ownCcs) {
        loop.ownCcs = true;
        changed = true;
    }
    auto loopCcField = [&](const AxisBinding& axis, const char* name, std::uint8_t& cc) {
        if (const char* as = shapesAs(axis)) {  // no CC to choose: the loop plays as its axis
            ImGui::TextDisabled("%s: plays as %s", name, as);
            return false;
        }
        if (!loop.ownCcs && axis.enabled && axis.sendsCc()) {
            ImGui::TextDisabled("%s: on CC %d", name, axis.cc);
            return false;
        }
        return inputU7((std::string("Loop CC for ") + name).c_str(), cc);
    };
    const std::string leftName = label(Button::LeftTrigger), rightName = label(Button::RightTrigger);
    changed |= loopCcField(own, triggers ? leftName.c_str() : "X", loop.ccX);
    changed |= loopCcField(ownY, triggers ? rightName.c_str() : "Y", loop.ccY);

    // The loop plays beside the sticks and fingers: a CC they send too would fight.
    std::string clash;
    auto check = [&](const AxisBinding& b, const std::string& name) {
        const bool xOwn = own.sendsCc() && loopCc(loop, own, 0) == loop.ccX, yOwn = ownY.sendsCc() && loopCc(loop, ownY, 1) == loop.ccY;
        if (b.enabled && b.sendsCc() && ((xOwn && b.cc == loop.ccX) || (yOwn && b.cc == loop.ccY)))
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
    if (m.tilt.enabled) {
        check(m.tilt.x, "tilt X");
        check(m.tilt.y, "tilt Y");
    }
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        auto plays = [&](const AxisBinding& axis, int which) {
            const char* as = shapesAs(axis);
            return as ? std::string("as ") + as : "on CC " + std::to_string(loopCc(loop, axis, which));
        };
        const std::string text = std::string(loop.ownCcs ? "The loop plays beside the " : "The loop plays on the CCs of the ") + what + ": " +
                                 (triggers ? leftName + " " : std::string("X ")) + plays(own, 0) + ", " + (triggers ? rightName + " " : std::string("Y ")) +
                                 plays(ownY, 1) + (loop.ownCcs ? "." : std::string("; the ") + what + " takes over while it moves.");
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }
    if (!clash.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("Also sent by %s: both would fight.", clash.c_str());
        ImGui::PopStyleColor();
    }
    drawAxisLanes(source, m, live);
    changed |= drawStepsForm(source, m);
    drawModulationForm(source, live);
    // Clear one axis' loop, or both.
    const char* names[2] = {triggers ? "Clear L2" : "Clear X", triggers ? "Clear R2" : "Clear Y"};
    for (int axis = 0; axis < 2; ++axis) {
        if (ImGui::Button(names[axis]))
            clearLoopRequest_ = ClearLoop{source, axis};
        ImGui::SameLine();
    }
    if (ImGui::Button("Clear both"))
        clearLoopRequest_ = ClearLoop{source, -1};
    ImGui::PopID();
    return changed;
}

// Each axis loop of a movement source in a lane, over its own length, its end
// at the right to drag as in the note editor: shorter, the part cut is
// hatched; longer, the time added is a straight line from its end back to its
// start. A synced loop keeps whole beats; one in steps, whole steps.
void MappingEditor::drawAxisLanes(int source, const Mapping& m, const LiveState& live) {
    using T = PerformanceView::Theme;
    const LiveState::Loop& loop = live.loops[static_cast<std::size_t>(source)];
    if (loop.status.state != LoopStatus::State::Playing)
        return;
    const bool finger = source == 2 || source == 3;
    const bool triggers = source == static_cast<int>(LoopSource::Triggers);
    const bool tilt = source == static_cast<int>(LoopSource::Tilt);
    const ImU32 base = source == 0 ? T::kBlue : source == 1 ? T::kOrange : triggers ? T::kPink : source == 2 ? T::kCyan
                     : tilt ? T::kViolet : T::kOrange;
    const ImU32 kViolet = T::kViolet;
    auto alpha = [](ImU32 c, float a) {
        return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * a) << IM_COL32_A_SHIFT);
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const float font = ImGui::GetFontSize();
    const float width = ImGui::GetContentRegionAvail().x, laneH = font * 4.8f;
    const LoopBinding& lb = m.loops[static_cast<std::size_t>(source)];
    ImGui::Spacing();
    for (int axis = 0; axis < 2; ++axis) {
        const LiveState::Lane& lane = loop.lanes[static_cast<std::size_t>(axis)];
        const LoopStatus& st = lane.status;
        const ImVec4 c = ImGui::ColorConvertU32ToFloat4(base);  // Y: lighter
        const ImU32 color = axis == 0 ? base : ImGui::ColorConvertFloat4ToU32(ImVec4((c.x + 1) * 0.5f, (c.y + 1) * 0.5f, (c.z + 1) * 0.5f, 1));
        const std::string nameText = triggers ? label(axis == 0 ? Button::LeftTrigger : Button::RightTrigger) : axis == 0 ? "X" : "Y";
        const char* name = nameText.c_str();
        const AxisBinding& binding = finger ? (axis == 0 ? m.fingers[static_cast<std::size_t>(source - 2)].x
                                                         : m.fingers[static_cast<std::size_t>(source - 2)].y)
                                   : tilt     ? (axis == 0 ? m.tilt.x : m.tilt.y)
                                   : triggers ? m.axes[static_cast<std::size_t>(axis == 0 ? Axis::LeftTrigger : Axis::RightTrigger)]
                                              : m.axes[static_cast<std::size_t>(source * 2 + axis)];  // a stick
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + laneH);
        ImGui::PushID(axis);
        ImGui::InvisibleButton("##lane", ImVec2(width, laneH));
        // (Pressed, the lane is the active item: the window no longer counts as hovered.)
        const bool laneHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool lanePressed = ImGui::IsItemActivated();
        ImGui::PopID();
        dl->AddRectFilled(a, b, IM_COL32(0x20, 0x22, 0x26, 255), 4.0f);
        dl->AddText(ImVec2(a.x + 6, a.y + 4), color, name);
        if (st.state == LoopStatus::State::Playing && !st.locked) {  // an x clears this axis alone
            const float cx = a.x + 6 + ImGui::CalcTextSize(name).x + 12, cy = a.y + 4 + font * 0.5f, r = font * 0.42f;
            const ImVec2 mouse = io.MousePos;
            const bool over = (laneHovered || lanePressed) && !laneDrag_ && std::hypot(mouse.x - cx, mouse.y - cy) <= r + 2;
            dl->AddCircleFilled(ImVec2(cx, cy), r, over ? IM_COL32(0x8A, 0x2A, 0x32, 255) : IM_COL32(0x46, 0x1E, 0x22, 255));
            dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(0xFF, 0x3B, 0x4E, 255), 0, 1.0f);
            const float q = r * 0.45f;
            dl->AddLine(ImVec2(cx - q, cy - q), ImVec2(cx + q, cy + q), T::kText, 1.5f);
            dl->AddLine(ImVec2(cx + q, cy - q), ImVec2(cx - q, cy + q), T::kText, 1.5f);
            if (over) {
                ImGui::SetTooltip("Clear %s's loop", name);
                if (lanePressed) {
                    clearLoopRequest_ = ClearLoop{source, axis};
                    ImGui::Dummy(ImVec2(0, 2));
                    continue;
                }
            }
        }
        if (st.state != LoopStatus::State::Playing || lane.points < 2 || st.length < 2) {
            const char* why = !binding.enabled ? "not enabled: not recorded" : "no loop";
            const ImVec2 size = ImGui::CalcTextSize(why);
            dl->AddText(ImVec2((a.x + b.x - size.x) * 0.5f, (a.y + b.y - size.y) * 0.5f), T::kDim, why);
            ImGui::Dummy(ImVec2(0, 2));
            continue;
        }
        const bool dragging = laneDrag_ && laneDrag_->source == source && laneDrag_->axis == axis;
        const int length = st.length;
        const int shown = dragging ? laneDrag_->newLength : length;
        const int span = std::max(length, shown);
        const float x0 = a.x + 6, x1 = b.x - 10, y0 = a.y + font * 1.3f, y1 = b.y - 6;
        auto xOf = [&](double sample) { return x0 + (x1 - x0) * static_cast<float>(sample / std::max(span - 1, 1)); };
        auto yOf = [&](float v) {
            if (finger && axis == 1)
                v = 1.0f - v;  // as sent
            if (binding.invert)
                v = 1.0f - v;
            return y1 - v * (y1 - y0);
        };
        auto sampleOf = [&](int i) { return static_cast<double>(i) * (length - 1) / (lane.points - 1); };
        auto value = [&](int i) { return static_cast<float>(lane.v[static_cast<std::size_t>(i)]) / 255.0f; };
        // Its steps, where they leave room.
        if (lane.stepSamples > 0 && xOf(lane.stepSamples) - x0 > 4)
            for (int k = lane.stepSamples; k < span; k += lane.stepSamples)
                dl->AddLine(ImVec2(xOf(k), a.y + 2), ImVec2(xOf(k), b.y - 2), IM_COL32(255, 255, 255, 18));
        for (int i = 1; i < lane.points; ++i)
            if (lane.on[static_cast<std::size_t>(i)] && lane.on[static_cast<std::size_t>(i - 1)])
                dl->AddLine(ImVec2(xOf(sampleOf(i - 1)), yOf(value(i - 1))), ImVec2(xOf(sampleOf(i)), yOf(value(i))), color, 1.8f);
        const float endX = xOf(shown - 1);
        if (shown < length) {  // the part cut: hatched
            dl->AddRectFilled(ImVec2(endX, a.y + 1), ImVec2(xOf(length - 1), b.y - 1), IM_COL32(0x14, 0x16, 0x19, 170));
            dl->PushClipRect(ImVec2(endX, a.y + 1), ImVec2(xOf(length - 1), b.y - 1), true);
            for (float hx = endX - laneH; hx < xOf(length - 1); hx += 9)
                dl->AddLine(ImVec2(hx, b.y), ImVec2(hx + laneH, a.y), IM_COL32(255, 255, 255, 26));
            dl->PopClipRect();
        } else if (shown > length) {  // the time added: back to its start
            const float from = xOf(length - 1);
            dl->AddRectFilled(ImVec2(from, a.y + 1), ImVec2(endX, b.y - 1), alpha(kViolet, 0.10f));
            const bool lifted = !lane.on[static_cast<std::size_t>(lane.points - 1)] || !lane.on[0];
            if (!lifted) {
                const ImVec2 p0(from, yOf(value(lane.points - 1))), p1(endX, yOf(value(0)));
                const float len = std::hypot(p1.x - p0.x, p1.y - p0.y);
                for (float d = 0; d < len; d += 10) {
                    const float t0 = d / len, t1 = std::min(d + 6, len) / len;
                    dl->AddLine(ImVec2(p0.x + (p1.x - p0.x) * t0, p0.y + (p1.y - p0.y) * t0),
                                ImVec2(p0.x + (p1.x - p0.x) * t1, p0.y + (p1.y - p0.y) * t1), color, 1.6f);
                }
            }
            const char* back = "back to its start";
            const ImVec2 size = ImGui::CalcTextSize(back);
            if (endX - from > size.x + 12)
                dl->AddText(ImVec2((from + endX - size.x) * 0.5f, b.y - size.y - 4), alpha(kViolet, 0.8f), back);
        }
        if (!dragging) {  // the playhead
            const float hx = xOf(std::clamp(st.position, 0, length - 1));
            dl->AddLine(ImVec2(hx, a.y + 2), ImVec2(hx, b.y - 2), IM_COL32(255, 255, 255, 150), 1.3f);
        }
        // Its length, top right: "4.0 s", or while dragged "4.0 s  to  5.2 s".
        const double factor = Engine::stretchFactor(st.stretch);
        const double perBeat = st.beats > 0 ? length * factor / st.beats : 0;  // samples a beat, as recorded
        auto lengthText = [&](int samples) {
            return perBeat > 0 ? loopBeatsText(std::round(samples / perBeat) * factor, std::max(live.beatsPerBar, 1))
                               : loopDurationText(samples * factor / Engine::kLoopRateHz);
        };
        const std::string label = dragging && shown != length ? lengthText(length) + "  to  " + lengthText(shown) : lengthText(length);
        const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
        const ImVec2 la(b.x - 16 - labelSize.x - 8, a.y + 3), lb2(b.x - 16, a.y + 5 + labelSize.y);
        dl->AddRectFilled(la, lb2, IM_COL32(0x14, 0x16, 0x19, 220), 6);
        dl->AddRect(la, lb2, dragging ? kViolet : alpha(color, 0.8f), 6);
        dl->AddText(ImVec2(la.x + 4, la.y + 1), dragging ? kViolet : T::kText, label.c_str());
        const bool shapes = binding.enabled && (binding.mode == AxisMode::Velocity || binding.mode == AxisMode::Transpose);
        const std::string cc = shapes ? (binding.mode == AxisMode::Velocity ? "VEL" : "TR") : "CC " + std::to_string(loopCc(lb, binding, axis));
        const ImVec2 ccSize = ImGui::CalcTextSize(cc.c_str());
        dl->AddText(ImVec2(b.x - 16 - ccSize.x, b.y - ccSize.y - 4), T::kDim, cc.c_str());
        // The end: hovered, dragged.
        const ImVec2 mouse = io.MousePos;
        const bool hovered = !laneDrag_ && (laneHovered || lanePressed) && mouse.x >= endX - 8 && mouse.x <= endX + 6 && mouse.y >= a.y &&
                             mouse.y <= b.y;
        const bool active = dragging || hovered;
        if (active) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            dl->AddLine(ImVec2(endX, a.y), ImVec2(endX, b.y), alpha(kViolet, 0.25f), 6);
        }
        dl->AddLine(ImVec2(endX, a.y), ImVec2(endX, b.y), alpha(kViolet, active ? 1.0f : 0.65f), 2);
        const float gy = (a.y + b.y) * 0.5f;
        dl->AddRectFilled(ImVec2(endX - 6, gy - 12), ImVec2(endX + 1, gy + 12), alpha(kViolet, active ? 1.0f : 0.8f), 3);
        if (hovered && lanePressed)
            laneDrag_ = LaneDrag{source, axis, mouse.x, static_cast<float>(length - 1) / std::max(x1 - x0, 1.0f), length, length};
        if (dragging) {
            LaneDrag& d = *laneDrag_;
            double wanted = d.length + static_cast<double>(mouse.x - d.grabX) * d.perPx;
            if (perBeat > 0) {
                wanted = std::clamp(std::round(wanted / perBeat), 1.0, std::max(1.0, std::floor(Engine::kMaxLoopSamples / perBeat))) * perBeat;
            } else if (lane.stepSamples > 0) {
                const double step = lane.stepSamples;
                wanted = std::max(1.0, std::round(wanted / step)) * step;
            }
            d.newLength = std::clamp(static_cast<int>(std::lround(wanted)), Engine::kLoopRateHz / 10, Engine::kMaxLoopSamples);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                laneDrag_.reset();
                escapeTaken_ = true;
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (d.newLength != d.length)
                    axisLength_ = AxisLength{source, axis, d.newLength};
                laneDrag_.reset();
            }
        }
        ImGui::Dummy(ImVec2(0, 2));
    }
}

// Steps, per axis: the loop plays holding each value for a step of the beat
// grid, on a number of levels; what it recorded stays as it was.
bool MappingEditor::drawStepsForm(int source, Mapping& m) {
    const bool triggers = source == static_cast<int>(LoopSource::Triggers);
    bool changed = false;
    LoopBinding& loop = m.loops[static_cast<std::size_t>(source)];
    ImGui::Spacing();
    for (int axis = 0; axis < 2; ++axis) {
        StepGrid& grid = loop.steps[static_cast<std::size_t>(axis)];
        ImGui::PushID(100 + axis);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", triggers ? label(axis == 0 ? Button::LeftTrigger : Button::RightTrigger).c_str() : axis == 0 ? "X " : "Y ");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Steps", &grid.on);
        ImGui::BeginDisabled(!grid.on);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.5f);
        if (ImGui::InputInt("Levels", &grid.levels)) {
            grid.levels = std::clamp(grid.levels, 2, 128);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        int step = std::clamp(grid.step, 0, kStepChoices - 1);
        if (ImGui::Combo("Step", &step, kStepNames.data(), kStepChoices) && step != grid.step) {
            grid.step = step;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    return changed;
}

bool MappingEditor::drawTriggerEditor(TriggerBinding& b) {
    bool changed = false;
    static const char* kTypes[] = {"No action", "Note", "CC", "Chord", "Transport", "Channel change", "Axis freeze",
                                   "Loop record", "Program change", "Shift", "Tap tempo", "Mode (session, edit)", "Recenter tilt",
                                   "MIDI effect"};
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
        break;
    }
    case ActionType::ProgramChange: {
        // "##kind": the Program field below has the same label.
        const std::pair<int, const char*> kinds[] = {{0, "Program##kind"}, {1, "Next (+1)##kind"}, {-1, "Previous (-1)##kind"}};
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
        break;
    }
    case ActionType::Shift:
        changed |= modeRadio(b.mode);
        break;
    case ActionType::SessionScreen:
    case ActionType::RecenterTilt:
        break;
    case ActionType::Effect: {
        static constexpr const char* kEffects[kEffectKinds] = {"Retrigger", "Delay"};
        int effect = static_cast<int>(b.effect);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::Combo("Effect", &effect, kEffects, kEffectKinds) && effect != static_cast<int>(b.effect)) {
            b.effect = static_cast<EffectKind>(effect);
            changed = true;
        }
        const bool delay = b.effect == EffectKind::Delay;
        int count = b.effectRepeats;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (!delay && ImGui::InputInt("Retriggers", &count)) {
            const int before = b.effectRepeats;
            b.effectRepeats = static_cast<std::uint8_t>(std::clamp(count, 1, kMaxEffectRepeats));
            for (int k = before; k < b.effectRepeats; ++k)  // the new ones as the last
                b.effectVelocity[static_cast<std::size_t>(k)] = b.effectVelocity[static_cast<std::size_t>(std::max(before - 1, 0))];
            changed = true;
        }
        static constexpr const char* kEvery[kEffectEveryChoices] = {"1/8", "1/8 triplet", "1/16", "1/16 triplet", "1/32", "1/32 triplet", "1/64"};
        int every = std::clamp<int>(b.effectEvery, 0, kEffectEveryChoices - 1);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::Combo("Every", &every, kEvery, kEffectEveryChoices) && every != b.effectEvery) {
            b.effectEvery = static_cast<std::uint8_t>(every);
            changed = true;
        }
        if (delay) {
            int feedback = b.effectFeedback;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::SliderInt("Feedback", &feedback, kMinEffectFeedback, kMaxEffectFeedback, "%d%%")) {
                b.effectFeedback = static_cast<std::uint8_t>(feedback);
                changed = true;
            }
            ImGui::SetItemTooltip("Each echo at this share of the velocity of the one before");
            int transpose = b.effectTranspose;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::SliderInt("Transpose each echo", &transpose, -kMaxEffectTranspose, kMaxEffectTranspose, transpose == 0 ? "0" : "%+d semitones")) {
                b.effectTranspose = static_cast<std::int8_t>(transpose);
                changed = true;
            }
            changed |= modeRadio(b.mode);
            drawEchoes(b);
        } else {
            changed |= drawEffectVelocity(b);
        }
        break;
    }
    case ActionType::TapTempo:
        break;
    case ActionType::LoopRecord:
        break;
    }
    return changed;
}

// What a delay plays: the note, then each echo at the feedback of the one
// before, until too soft to hear (a note at velocity 100).
void MappingEditor::drawEchoes(const TriggerBinding& b) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float w = em * 22, h = em * 3.5f, y1 = o.y + h;
    dl->AddRectFilled(o, ImVec2(o.x + w, y1), IM_COL32(0x20, 0x22, 0x26, 255), 4);
    std::array<int, kMaxEffectRepeats + 1> velocity{100};
    int n = 1;
    for (; n <= kMaxEffectRepeats; ++n) {
        velocity[static_cast<std::size_t>(n)] = (velocity[static_cast<std::size_t>(n - 1)] * b.effectFeedback + 50) / 100;
        if (velocity[static_cast<std::size_t>(n)] < kMinEchoVelocity)
            break;
    }
    const float colW = w / static_cast<float>(kMaxEffectRepeats + 1);
    for (int k = 0; k < n; ++k) {
        const float x0 = o.x + colW * static_cast<float>(k) + colW * 0.2f, x1 = o.x + colW * static_cast<float>(k + 1) - colW * 0.2f;
        dl->AddRectFilled(ImVec2(x0, y1 - (h - 4) * static_cast<float>(velocity[static_cast<std::size_t>(k)]) / 127.0f), ImVec2(x1, y1), 
                          k == 0 ? IM_COL32(0x78, 0x7E, 0x8A, 255) : IM_COL32(0x9B, 0x7B, 0xE0, 255), 2, ImDrawFlags_RoundCornersTop);
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("%d echoes", n - 1);
}

// The velocity of each repeat of an effect, in percent of the note's: a bar
// each after the note's, drawn with the mouse or set with the controller (a
// vertical slider each), or one of four shapes.
bool MappingEditor::drawEffectVelocity(TriggerBinding& b) {
    bool changed = false;
    const int n = std::clamp<int>(b.effectRepeats, 1, kMaxEffectRepeats);
    auto shape = [&](auto percent) {
        for (int k = 0; k < n; ++k)
            b.effectVelocity[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::clamp((percent(k) + 2) / 5 * 5, 5, kMaxEffectVelocity));
        changed = true;
    };
    ImGui::TextUnformatted("Velocity");
    ImGui::SameLine();
    if (ImGui::SmallButton("Flat"))
        shape([](int) { return 100; });
    ImGui::SameLine();
    if (ImGui::SmallButton("Fade out"))
        shape([&](int k) { return 100 * (n - k) / (n + 1); });
    ImGui::SameLine();
    if (ImGui::SmallButton("Fade in"))
        shape([&](int k) { return 100 * (k + 1) / n; });
    ImGui::SameLine();
    if (ImGui::SmallButton("Accent"))
        shape([](int k) { return k % 2 ? 100 : 60; });
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float w = em * 22, h = em * 4.5f, colW = w / static_cast<float>(n + 1), y1 = o.y + h;
    dl->AddRectFilled(o, ImVec2(o.x + w, y1), IM_COL32(0x20, 0x22, 0x26, 255), 4);
    const float full = y1 - h * 0.5f;  // 100%
    for (float x = o.x + 2; x < o.x + w - 2; x += 6)
        dl->AddLine(ImVec2(x, full), ImVec2(std::min(x + 3, o.x + w - 2), full), IM_COL32(255, 255, 255, 40));
    auto bar = [&](int column, int percent, ImU32 color) {
        const float x0 = o.x + colW * static_cast<float>(column) + colW * 0.18f, x1 = o.x + colW * static_cast<float>(column + 1) - colW * 0.18f;
        dl->AddRectFilled(ImVec2(x0, y1 - h * static_cast<float>(percent) / kMaxEffectVelocity), ImVec2(x1, y1), color, 2, ImDrawFlags_RoundCornersTop);
    };
    bar(0, 100, IM_COL32(0x78, 0x7E, 0x8A, 255));  // the note
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 14));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 24));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const ImU32 color = b.effect == EffectKind::Delay ? IM_COL32(0x9B, 0x7B, 0xE0, 255) : IM_COL32(0xFF, 0xB0, 0x20, 255);
    for (int k = 0; k < n; ++k) {
        auto& v = b.effectVelocity[static_cast<std::size_t>(k)];
        int percent = v;
        ImGui::SetCursorScreenPos(ImVec2(o.x + colW * static_cast<float>(k + 1), o.y));
        ImGui::PushID(k);
        if (ImGui::VSliderInt("##velocity", ImVec2(colW, h), &percent, 5, kMaxEffectVelocity, "")) {
            v = static_cast<std::uint8_t>(std::clamp((percent + 2) / 5 * 5, 5, kMaxEffectVelocity));
            changed = true;
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetTooltip("%d%%", v);
        ImGui::PopID();
        bar(k + 1, v, color);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    // Under each bar, its percentage.
    const float ty = y1 + em * 0.15f;
    auto label = [&](int column, const char* text) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        if (size.x * 0.8f < colW)
            dl->AddText(ImGui::GetFont(), em * 0.8f, ImVec2(o.x + colW * (static_cast<float>(column) + 0.5f) - size.x * 0.4f, ty), IM_COL32(0x8D, 0x93, 0x9C, 255), text);
    };
    label(0, "note");
    for (int k = 0; k < n; ++k)
        label(k + 1, std::to_string(b.effectVelocity[static_cast<std::size_t>(k)]).c_str());
    ImGui::SetCursorScreenPos(ImVec2(o.x, y1));
    ImGui::Dummy(ImVec2(w, em * 1.2f));
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
    ImGui::SetItemTooltip("Random velocity and timing on the channels chosen in Settings; "
                          "each note of a chord varies on its own.");
    if (b.humanize && humanizeChannels_ == 0) {
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "off on every channel (Settings)");
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

    // A new chord keeps the inversion, if it has that many notes.
    auto take = [&](const ChordNotes& chord) {
        b.chord = chord;
        b.chordInversion = static_cast<std::uint8_t>(std::min<int>(b.chordInversion, std::max(chord.count - chord.bassCount - 1, 0)));
    };
    auto apply = [&](const std::string& text) {
        if (auto chord = parseChord(text, b.chordOctave, b.chordRootBelow, b.chordInversion)) {
            b.chordName = text;
            take(*chord);
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
        if (auto chord = parseChord(chordText_, b.chordOctave, b.chordRootBelow, b.chordInversion)) {
            b.chordName = chordText_;
            take(*chord);
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
            if (auto chord = parseChord(b.chordName, octave, b.chordRootBelow, b.chordInversion)) {
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
                const auto chord = parseChord(b.chordName, b.chordOctave, below, b.chordInversion);
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

        // Inversion: the lowest note of the chord moved up an octave, once per step.
        static constexpr const char* kInversions[] = {"Root", "1st", "2nd", "3rd", "4th", "5th", "6th"};
        const int tones = std::min<int>(b.chord.count - b.chord.bassCount, 7);
        if (tones > 1) {
            for (int k = 0; k < tones; ++k) {
                if (k > 0)
                    ImGui::SameLine(0, 4);
                const bool on = k == b.chordInversion;
                if (on)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
                if (ImGui::Button(kInversions[k], ImVec2(ImGui::GetFontSize() * (k == 0 ? 3.2f : 2.6f), 0)) && !on) {
                    if (auto chord = parseChord(b.chordName, b.chordOctave, b.chordRootBelow, k)) {
                        b.chordInversion = static_cast<std::uint8_t>(k);
                        b.chord = *chord;
                        changed = true;
                    }
                }
                if (on)
                    ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Inversion");
            ImGui::SetItemTooltip("The lowest note of the chord goes up an octave at each step; the bass root stays");
        }
    }
    if (b.chord.count - b.chord.bassCount > 1) {
        changed |= ImGui::Checkbox("Closest to the previous chord", &b.chordVoiceLeading);
        ImGui::SetItemTooltip("Plays the inversion whose notes are nearest to the chord played before on the channel, "
                              "at this octave or the one below: the voices move as little as possible");
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "(voice leading)");
    }
    return changed;
}
