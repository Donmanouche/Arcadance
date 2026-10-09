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

constexpr ImU32 kLiveColor = IM_COL32(40, 120, 70, 160);
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImVec4 kHintColor{0.65f, 0.65f, 0.7f, 1.0f};

// Buttons in the order a player thinks of them; the D-pad lives in its own tab.
constexpr std::array kButtonOrder = {
    Button::South,      Button::East,       Button::West,  Button::North, Button::LeftShoulder,
    Button::RightShoulder, Button::LeftTrigger, Button::RightTrigger, Button::LeftStick, Button::RightStick,
    Button::Back,       Button::Start,      Button::Guide, Button::Touchpad, Button::Misc1,
};

constexpr std::array<std::array<std::optional<HatDir>, 3>, 3> kDpadGrid = {{
    {HatDir::UpLeft, HatDir::Up, HatDir::UpRight},
    {HatDir::Left, std::nullopt, HatDir::Right},
    {HatDir::DownLeft, HatDir::Down, HatDir::DownRight},
}};

constexpr std::array<const char*, 12> kRoots = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

float axisPosition(const LiveState& live, Axis a) {
    const float raw = static_cast<float>(live.axes[static_cast<std::size_t>(a)]) / 32767.0f;
    return std::clamp(isTrigger(a) ? raw : (raw + 1.0f) * 0.5f, 0.0f, 1.0f);
}

bool isLive(const LiveState& live, Button b) {
    if (b == Button::LeftTrigger)
        return axisPosition(live, Axis::LeftTrigger) >= Engine::kTriggerPress;
    if (b == Button::RightTrigger)
        return axisPosition(live, Axis::RightTrigger) >= Engine::kTriggerPress;
    return live.isDown(b);
}

std::optional<HatDir> liveHat(const LiveState& live) {
    const int y = int(live.isDown(Button::DpadUp)) - int(live.isDown(Button::DpadDown));
    const int x = int(live.isDown(Button::DpadRight)) - int(live.isDown(Button::DpadLeft));
    static constexpr std::array<std::array<std::optional<HatDir>, 3>, 3> kFromXY = {{
        {HatDir::DownLeft, HatDir::Down, HatDir::DownRight},
        {HatDir::Left, std::nullopt, HatDir::Right},
        {HatDir::UpLeft, HatDir::Up, HatDir::UpRight},
    }};
    return kFromXY[static_cast<std::size_t>(y + 1)][static_cast<std::size_t>(x + 1)];
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

// One axis binding as table cells: enabled, CC, min, max, invert, live value.
bool axisCells(const char* id, AxisBinding& a, float position) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TableNextColumn();
    changed |= ImGui::Checkbox("##on", &a.enabled);
    ImGui::BeginDisabled(!a.enabled);
    ImGui::TableNextColumn();
    changed |= inputU7("##cc", a.cc);
    ImGui::TableNextColumn();
    changed |= inputU7("##min", a.min);
    ImGui::TableNextColumn();
    changed |= inputU7("##max", a.max);
    ImGui::TableNextColumn();
    changed |= ImGui::Checkbox("##inv", &a.invert);
    ImGui::EndDisabled();
    ImGui::TableNextColumn();
    axisValueBar(a, position, -1.0f);
    ImGui::PopID();
    return changed;
}

void setupAxisColumns(const char* first) {
    ImGui::TableSetupColumn(first, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("CC", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Invert", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Value");
    ImGui::TableHeadersRow();
}

// A clickable cell for a grid (D-pad, touchpad zones).
bool gridCell(const std::string& title, const TriggerBinding& b, bool selected, bool live, ImVec2 size, bool flats) {
    if (live)
        ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(kLiveColor));
    const std::string text = title + "\n" + describe(b, flats);
    const bool clicked = ImGui::Selectable(text.c_str(), selected || live, 0, size);
    if (live)
        ImGui::PopStyleColor();
    return clicked;
}

}  // namespace

bool MappingEditor::select(const std::string& spec) {
    const auto colon = spec.find(':');
    const std::string kind = spec.substr(0, colon);
    const std::string rest = colon == std::string::npos ? std::string() : spec.substr(colon + 1);
    if (kind == "button") {
        if (auto b = parseButton(rest); b && !isDpad(*b)) {
            selection_ = {Selection::Kind::Button, static_cast<int>(*b)};
            showTab(Tab::Buttons);
            return true;
        }
    } else if (kind == "dpad") {
        if (auto d = parseHatDir(rest)) {
            selection_ = {Selection::Kind::Dpad, static_cast<int>(*d)};
            showTab(Tab::Dpad);
            return true;
        }
    } else if (kind == "tab") {
        if (rest == "buttons")
            showTab(Tab::Buttons);
        else if (rest == "dpad")
            showTab(Tab::Dpad);
        else if (rest == "axes")
            showTab(Tab::Axes);
        else if (rest == "touchpad")
            showTab(Tab::Touchpad);
        else
            return false;
        return true;
    } else if (kind == "zone" && rest.size() > 2 && (rest[0] == '1' || rest[0] == '2') && rest[1] == ':') {
        if (auto z = parseZone(rest.substr(2))) {
            selection_ = {Selection::Kind::Zone, static_cast<int>(*z), rest[0] - '1'};
            showTab(Tab::Touchpad);
            return true;
        }
    }
    return false;
}

namespace {

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
    return spec;
}

bool MappingEditor::drawAxisForm(const char* id, AxisBinding& a, float position) {
    bool changed = false;
    ImGui::PushID(id);
    changed |= ImGui::Checkbox("Enabled", &a.enabled);
    ImGui::BeginDisabled(!a.enabled);
    changed |= inputU7("CC", a.cc);
    changed |= inputU7("Min", a.min);
    ImGui::SameLine();
    changed |= inputU7("Max", a.max);
    changed |= ImGui::Checkbox("Invert", &a.invert);
    ImGui::EndDisabled();
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
            changed |= drawAxisForm(p[i].c_str(), m.axes[static_cast<std::size_t>(*a)], axisPosition(live, *a));
        }
    } else if (p.size() == 2 && p[0] == "finger" && (p[1] == "1" || p[1] == "2")) {
        const int finger = p[1][0] - '1';
        changed |= drawFingerForm(finger, m.fingers[static_cast<std::size_t>(finger)], live);
    } else if (p.size() == 1 && p[0] == "fingers") {
        for (int f = 0; f < kFingerCount; ++f) {
            ImGui::SeparatorText(f == 0 ? "Finger 1" : "Finger 2");
            changed |= drawFingerForm(f, m.fingers[static_cast<std::size_t>(f)], live);
        }
    }
    if (trigger)
        changed |= drawTriggerEditor(*trigger);
    ImGui::PopID();
    return changed;
}

bool MappingEditor::draw(Mapping& mapping, const LiveState& live, bool readOnly, const std::optional<Key>& key) {
    key_ = key;
    bool changed = false;
    const float sideWidth = std::max(ImGui::GetFontSize() * 26, ImGui::GetContentRegionAvail().x * 0.38f);

    ImGui::BeginChild("controls", ImVec2(-sideWidth, 0));
    ImGui::BeginDisabled(readOnly);
    if (ImGui::BeginTabBar("tabs")) {
        auto tab = [&](const char* name, Tab t) {
            ImGuiTabItemFlags flags = requestedTab_ == t ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(name, nullptr, flags))
                return false;
            tab_ = t;
            return true;
        };
        if (tab("Buttons", Tab::Buttons)) {
            changed |= drawButtons(mapping, live);
            ImGui::EndTabItem();
        }
        if (tab("D-pad", Tab::Dpad)) {
            changed |= drawDpad(mapping, live);
            ImGui::EndTabItem();
        }
        if (tab("Sticks and triggers", Tab::Axes)) {
            changed |= drawAxes(mapping, live);
            ImGui::EndTabItem();
        }
        if (tab("Touchpad", Tab::Touchpad)) {
            changed |= drawTouchpad(mapping, live);
            ImGui::EndTabItem();
        }
        requestedTab_.reset();
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("side", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(readOnly);
    changed |= drawSidePanel(mapping);
    ImGui::EndDisabled();
    ImGui::EndChild();
    return changed;
}

bool MappingEditor::drawButtons(Mapping& m, const LiveState& live) {
    if (!ImGui::BeginTable("buttons", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY))
        return false;
    ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8);
    ImGui::TableSetupColumn("Action");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (Button b : kButtonOrder) {
        const int index = static_cast<int>(b);
        const Selection me{Selection::Kind::Button, index};
        ImGui::TableNextRow();
        if (isLive(live, b))
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, kLiveColor);
        ImGui::TableNextColumn();
        ImGui::PushID(index);
        if (ImGui::Selectable(label(b).c_str(), selection_ == me, ImGuiSelectableFlags_SpanAllColumns))
            selection_ = me;
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(describe(m.buttons[static_cast<std::size_t>(index)], flats()).c_str());
    }
    ImGui::EndTable();
    return false;
}

bool MappingEditor::drawDpad(Mapping& m, const LiveState& live) {
    const auto current = liveHat(live);
    const float cellWidth = std::min(ImGui::GetContentRegionAvail().x / 3.0f - ImGui::GetStyle().ItemSpacing.x,
                                     ImGui::GetFontSize() * 14);
    const ImVec2 size(cellWidth, ImGui::GetTextLineHeightWithSpacing() * 2.6f);
    for (const auto& row : kDpadGrid) {
        for (std::size_t col = 0; col < row.size(); ++col) {
            if (col > 0)
                ImGui::SameLine();
            if (!row[col]) {
                ImGui::Dummy(size);
                continue;
            }
            const HatDir dir = *row[col];
            const Selection me{Selection::Kind::Dpad, static_cast<int>(dir)};
            ImGui::PushID(me.index);
            if (gridCell(label(dir), m.dpad[static_cast<std::size_t>(dir)], selection_ == me, current == dir, size, flats()))
                selection_ = me;
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
    ImGui::TextWrapped("Diagonals play when two neighboring directions are held together.");
    ImGui::PopStyleColor();
    return false;
}

bool MappingEditor::drawAxes(Mapping& m, const LiveState& live) {
    bool changed = false;
    if (ImGui::BeginTable("axes", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        setupAxisColumns("Axis");
        for (int a = 0; a < kAxisCount; ++a) {
            const Axis axis = static_cast<Axis>(a);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label(axis).c_str());
            changed |= axisCells(label(axis).c_str(), m.axes[static_cast<std::size_t>(a)], axisPosition(live, axis));
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
    ImGui::TextWrapped("Each axis sends a CC, from Min (stick left or down, trigger released) to Max. "
                       "Invert reverses it. The triggers also act as the L2 and R2 buttons (Buttons tab), "
                       "pressed at %d %% of their travel.",
                       static_cast<int>(Engine::kTriggerPress * 100));
    ImGui::PopStyleColor();
    return changed;
}

bool MappingEditor::drawTouchpad(Mapping& m, const LiveState& live) {
    bool changed = false;

    // Live view of the touchpad with both fingers.
    const float width = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 24);
    const ImVec2 size(width, width * 0.45f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(35, 38, 48, 255), 6.0f);
    draw->AddLine(ImVec2(origin.x + size.x / 2, origin.y), ImVec2(origin.x + size.x / 2, origin.y + size.y),
                  IM_COL32(70, 75, 90, 255));
    draw->AddLine(ImVec2(origin.x, origin.y + size.y / 2), ImVec2(origin.x + size.x, origin.y + size.y / 2),
                  IM_COL32(70, 75, 90, 255));
    const ImU32 fingerColors[] = {IM_COL32(90, 200, 255, 255), IM_COL32(255, 170, 80, 255)};
    for (int f = 0; f < kFingerCount; ++f) {
        const auto& finger = live.fingers[static_cast<std::size_t>(f)];
        if (!finger.down)
            continue;
        const ImVec2 p(origin.x + finger.x * size.x, origin.y + finger.y * size.y);
        draw->AddCircleFilled(p, ImGui::GetFontSize() * 0.5f, fingerColors[f]);
        draw->AddText(ImVec2(p.x + 10, p.y - 8), IM_COL32_WHITE, f == 0 ? "1" : "2");
    }
    ImGui::Dummy(size);
    if (!live.hasTouchpad && live.connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("Touchpad not detected: check the udev rule (see the README).");
        ImGui::PopStyleColor();
    }

    static const char* kModes[] = {"Off", "X/Y position -> CC", "4 zones -> pads"};
    for (int f = 0; f < kFingerCount; ++f) {
        FingerBinding& finger = m.fingers[static_cast<std::size_t>(f)];
        ImGui::PushID(f);
        ImGui::SeparatorText(f == 0 ? "Finger 1" : "Finger 2");
        int mode = static_cast<int>(finger.mode);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        if (ImGui::Combo("Mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
            finger.mode = static_cast<TouchMode>(mode);
            changed = true;
        }
        const auto& liveFinger = live.fingers[static_cast<std::size_t>(f)];
        if (finger.mode == TouchMode::Axis) {
            if (ImGui::BeginTable("xy", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                setupAxisColumns("Axis");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("X (left -> right)");
                changed |= axisCells("x", finger.x, liveFinger.down ? liveFinger.x : 0.0f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Y (bottom -> top)");
                changed |= axisCells("y", finger.y, liveFinger.down ? 1.0f - liveFinger.y : 0.0f);
                ImGui::EndTable();
            }
        } else if (finger.mode == TouchMode::Zones) {
            const float cellWidth = std::min(ImGui::GetContentRegionAvail().x / 2.0f - ImGui::GetStyle().ItemSpacing.x,
                                             ImGui::GetFontSize() * 14);
            const ImVec2 cell(cellWidth, ImGui::GetTextLineHeightWithSpacing() * 2.6f);
            std::optional<Zone> current;
            if (liveFinger.down)
                current = liveFinger.y < 0.5f ? (liveFinger.x < 0.5f ? Zone::TopLeft : Zone::TopRight)
                                              : (liveFinger.x < 0.5f ? Zone::BottomLeft : Zone::BottomRight);
            for (int z = 0; z < kZoneCount; ++z) {
                if (z % 2)
                    ImGui::SameLine();
                const Zone zone = static_cast<Zone>(z);
                const Selection me{Selection::Kind::Zone, z, f};
                ImGui::PushID(z);
                if (gridCell(label(zone), finger.zones[static_cast<std::size_t>(z)], selection_ == me, current == zone,
                             cell, flats()))
                    selection_ = me;
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
    return changed;
}

TriggerBinding* MappingEditor::selected(Mapping& m) {
    switch (selection_.kind) {
    case Selection::Kind::Button:
        return tab_ == Tab::Buttons ? &m.buttons[static_cast<std::size_t>(selection_.index)] : nullptr;
    case Selection::Kind::Dpad:
        return tab_ == Tab::Dpad ? &m.dpad[static_cast<std::size_t>(selection_.index)] : nullptr;
    case Selection::Kind::Zone: {
        FingerBinding& f = m.fingers[static_cast<std::size_t>(selection_.finger)];
        if (tab_ != Tab::Touchpad || f.mode != TouchMode::Zones)
            return nullptr;
        return &f.zones[static_cast<std::size_t>(selection_.index)];
    }
    case Selection::Kind::None:
        break;
    }
    return nullptr;
}

std::string MappingEditor::selectedTitle() const {
    switch (selection_.kind) {
    case Selection::Kind::Button: return label(static_cast<Button>(selection_.index));
    case Selection::Kind::Dpad: return "D-pad: " + label(static_cast<HatDir>(selection_.index));
    case Selection::Kind::Zone:
        return std::string("Touchpad, finger ") + (selection_.finger == 0 ? "1" : "2") + ": " +
               label(static_cast<Zone>(selection_.index));
    case Selection::Kind::None: break;
    }
    return {};
}

bool MappingEditor::drawSidePanel(Mapping& m) {
    TriggerBinding* b = selected(m);
    if (!b) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        if (tab_ == Tab::Axes)
            ImGui::TextWrapped("Axes are set directly in the table.");
        else if (tab_ == Tab::Touchpad)
            ImGui::TextWrapped("In 4-zone mode, click a zone to choose its action. "
                               "In X/Y mode, set the CCs directly in the table.");
        else
            ImGui::TextWrapped("Click a control to choose its action.\n\n"
                               "Tip: the controls you touch on the gamepad light up in green.");
        ImGui::PopStyleColor();
        return false;
    }
    ImGui::SeparatorText(selectedTitle().c_str());
    ImGui::PushID(static_cast<int>(selection_.kind) * 1000 + selection_.finger * 100 + selection_.index);
    const bool changed = drawTriggerEditor(*b);
    ImGui::PopID();
    return changed;
}

bool MappingEditor::drawTriggerEditor(TriggerBinding& b) {
    bool changed = false;
    static const char* kTypes[] = {"No action", "Note", "CC", "Chord", "Transport", "Channel change", "Axis freeze"};
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

    // Builder: root + quality, narrowed to the tonality when there is one.
    std::size_t rootLength = 0;
    const auto rootPitch = parsePitch(b.chordName, &rootLength);
    const int root = rootPitch ? ((*rootPitch % 12) + 12) % 12 : 0;
    const std::string quality = rootPitch ? b.chordName.substr(rootLength) : std::string();
    std::vector<int> roots;
    if (key_)
        roots = scalePitchClasses(*key_);
    else
        for (int pc = 0; pc < 12; ++pc)
            roots.push_back(pc);
    const bool rootInKey = std::find(roots.begin(), roots.end(), root) != roots.end();
    auto rootName = [&](int pc) { return key_ ? pitchName(pc, flats()) : std::string(kRoots[static_cast<std::size_t>(pc)]); };

    // A root outside the scale keeps the spelling of the chord name.
    const std::string rootPreview = rootInKey || !rootPitch ? rootName(root) : b.chordName.substr(0, rootLength);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::BeginCombo("Root", rootPreview.c_str())) {
        for (int pc : roots) {
            if (!ImGui::Selectable(rootName(pc).c_str(), pc == root))
                continue;
            std::string candidate = rootName(pc) + quality;
            if (key_) {  // keep the chord inside the scale: same quality if it fits, else the first that does
                const auto& chords = keyChords();
                const auto same = std::find_if(chords.begin(), chords.end(),
                                               [&](const KeyChord& c) { return c.name == candidate; });
                const auto first = std::find_if(chords.begin(), chords.end(), [&](const KeyChord& c) {
                    return parsePitch(c.name) && ((*parsePitch(c.name) % 12) + 12) % 12 == pc;
                });
                if (same == chords.end() && first != chords.end())
                    candidate = first->name;
            }
            apply(candidate);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    const std::string preview = quality.empty() ? "major" : quality;
    if (ImGui::BeginCombo("Type", preview.c_str())) {
        for (std::string_view q : chordQualities()) {
            const std::string candidate = rootPreview + std::string(q);
            if (key_ && rootInKey) {
                const auto& chords = keyChords();
                const bool fits = std::any_of(chords.begin(), chords.end(),
                                              [&](const KeyChord& c) { return c.name == candidate; });
                if (!fits)
                    continue;
            }
            const std::string name = q.empty() ? "major" : std::string(q);
            if (ImGui::Selectable(name.c_str(), q == quality))
                apply(candidate);
        }
        ImGui::EndCombo();
    }

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
