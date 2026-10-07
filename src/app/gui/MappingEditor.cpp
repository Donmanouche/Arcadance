#include "gui/MappingEditor.hpp"

#include "midipad/Engine.hpp"
#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
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
    if (ImGui::RadioButton("Momentané", mode == Mode::Momentary)) {
        changed = mode != Mode::Momentary;
        mode = Mode::Momentary;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Bascule (toggle)", mode == Mode::Toggle)) {
        changed = mode != Mode::Toggle;
        mode = Mode::Toggle;
    }
    ImGui::SetItemTooltip("Momentané : actif tant que le contrôle est tenu.\n"
                          "Bascule : un appui active, l'appui suivant désactive.\n"
                          "Les notes et accords en bascule d'une même famille s'excluent.");
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
    ImGui::TableSetupColumn("Actif", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("CC", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Inverser", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Valeur");
    ImGui::TableHeadersRow();
}

// A clickable cell for a grid (D-pad, touchpad zones).
bool gridCell(const std::string& title, const TriggerBinding& b, bool selected, bool live, ImVec2 size) {
    if (live)
        ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(kLiveColor));
    const std::string text = title + "\n" + describe(b);
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
    } else if (kind == "zone" && rest.size() > 2 && (rest[0] == '1' || rest[0] == '2') && rest[1] == ':') {
        if (auto z = parseZone(rest.substr(2))) {
            selection_ = {Selection::Kind::Zone, static_cast<int>(*z), rest[0] - '1'};
            showTab(Tab::Touchpad);
            return true;
        }
    }
    return false;
}

bool MappingEditor::draw(Mapping& mapping, const LiveState& live, bool readOnly) {
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
        if (tab("Boutons", Tab::Buttons)) {
            changed |= drawButtons(mapping, live);
            ImGui::EndTabItem();
        }
        if (tab("Croix directionnelle", Tab::Dpad)) {
            changed |= drawDpad(mapping, live);
            ImGui::EndTabItem();
        }
        if (tab("Sticks et gâchettes", Tab::Axes)) {
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
    ImGui::TableSetupColumn("Bouton", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8);
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
        ImGui::TextUnformatted(describe(m.buttons[static_cast<std::size_t>(index)]).c_str());
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
            if (gridCell(label(dir), m.dpad[static_cast<std::size_t>(dir)], selection_ == me, current == dir, size))
                selection_ = me;
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
    ImGui::TextWrapped("Les diagonales se déclenchent quand deux directions voisines sont tenues ensemble.");
    ImGui::PopStyleColor();
    return false;
}

bool MappingEditor::drawAxes(Mapping& m, const LiveState& live) {
    bool changed = false;
    if (ImGui::BeginTable("axes", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        setupAxisColumns("Axe");
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
    ImGui::TextWrapped("Chaque axe envoie un CC, de Min (stick à gauche ou en bas, gâchette relâchée) à Max. "
                       "Inverser retourne le sens. Les gâchettes servent aussi de boutons L2 et R2 (onglet Boutons), "
                       "déclenchés à %d %% de la course.",
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
        ImGui::TextWrapped("Touchpad non détecté : vérifiez la règle udev (voir le README).");
        ImGui::PopStyleColor();
    }

    static const char* kModes[] = {"Désactivé", "Position X/Y -> CC", "4 zones -> pads"};
    for (int f = 0; f < kFingerCount; ++f) {
        FingerBinding& finger = m.fingers[static_cast<std::size_t>(f)];
        ImGui::PushID(f);
        ImGui::SeparatorText(f == 0 ? "Doigt 1" : "Doigt 2");
        int mode = static_cast<int>(finger.mode);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        if (ImGui::Combo("Mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
            finger.mode = static_cast<TouchMode>(mode);
            changed = true;
        }
        const auto& liveFinger = live.fingers[static_cast<std::size_t>(f)];
        if (finger.mode == TouchMode::Axis) {
            if (ImGui::BeginTable("xy", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                setupAxisColumns("Axe");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("X (gauche -> droite)");
                changed |= axisCells("x", finger.x, liveFinger.down ? liveFinger.x : 0.0f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Y (bas -> haut)");
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
                             cell))
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
    case Selection::Kind::Dpad: return "Croix : " + label(static_cast<HatDir>(selection_.index));
    case Selection::Kind::Zone:
        return std::string("Touchpad, doigt ") + (selection_.finger == 0 ? "1" : "2") + " : " +
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
            ImGui::TextWrapped("Les axes se règlent directement dans le tableau.");
        else if (tab_ == Tab::Touchpad)
            ImGui::TextWrapped("En mode 4 zones, cliquez sur une zone pour choisir son action. "
                               "En mode X/Y, réglez les CC directement dans le tableau.");
        else
            ImGui::TextWrapped("Cliquez sur un contrôle pour choisir son action.\n\n"
                               "Astuce : les contrôles que vous touchez sur la manette s'allument en vert.");
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
    static const char* kTypes[] = {"Aucune action", "Note", "CC", "Accord", "Transport", "Changement de canal",
                                   "Freeze d'axes"};
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
        changed |= inputU7("Note", b.number);
        ImGui::SameLine();
        ImGui::Text("%s", noteName(b.number).c_str());
        changed |= sliderU7("Vélocité", b.velocity, 1, 127);
        changed |= modeRadio(b.mode);
        break;
    case ActionType::ControlChange:
        changed |= inputU7("Numéro de CC", b.number);
        changed |= sliderU7("Valeur à l'appui", b.onValue);
        changed |= sliderU7("Valeur au relâchement", b.offValue);
        changed |= modeRadio(b.mode);
        break;
    case ActionType::Chord:
        changed |= drawChordFields(b);
        changed |= sliderU7("Vélocité", b.velocity, 1, 127);
        changed |= modeRadio(b.mode);
        break;
    case ActionType::Transport: {
        const std::pair<TransportCommand, const char*> commands[] = {
            {TransportCommand::Start, "Lecture (Start)"},
            {TransportCommand::Stop, "Arrêt (Stop)"},
            {TransportCommand::Continue, "Reprise (Continue)"},
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
        if (ImGui::RadioButton("Canal précédent (-1)", b.channelStep == -1) && b.channelStep != -1) {
            b.channelStep = -1;
            changed = true;
        }
        if (ImGui::RadioButton("Canal suivant (+1)", b.channelStep == 1) && b.channelStep != 1) {
            b.channelStep = 1;
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("Le canal tourne de 16 à 1. Si le preset définit un mapping pour le nouveau canal, "
                           "il s'applique aussitôt.");
        ImGui::PopStyleColor();
        break;
    }
    case ActionType::Freeze: {
        ImGui::TextUnformatted("Axes gelés à leur valeur actuelle :");
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
        ImGui::TextWrapped("Un appui gèle, l'appui suivant dégèle.");
        ImGui::PopStyleColor();
        break;
    }
    }
    return changed;
}

bool MappingEditor::drawChordFields(TriggerBinding& b) {
    bool changed = false;
    // The text field keeps what is typed even while it is not a valid chord;
    // the binding only takes valid names.
    if (!(chordTextFor_ == selection_)) {
        chordTextFor_ = selection_;
        if (!b.chordName.empty()) {
            chordText_ = b.chordName;
        } else {
            chordText_.clear();
        }
    }

    auto apply = [&](const std::string& text) {
        if (auto chord = parseChord(text)) {
            b.chordName = text;
            b.chord = *chord;
            changed = true;
        }
    };

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::InputText("Accord", &chordText_))
        apply(chordText_);

    const auto typed = parseChord(chordText_);
    if (!typed && !chordText_.empty()) {
        ImGui::TextColored(kErrorColor, "Accord inconnu (l'accord précédent reste actif)");
    } else if (b.chord.count) {
        std::string notes;
        for (auto n : b.chord.view())
            notes += noteName(n) + " ";
        ImGui::Text("Notes : %s", notes.c_str());
    }

    // Builder: root + quality.
    std::size_t rootLength = 0;
    const auto rootPitch = parsePitch(b.chordName, &rootLength);
    int root = rootPitch ? ((*rootPitch % 12) + 12) % 12 : 0;
    const std::string quality = rootPitch ? b.chordName.substr(rootLength) : std::string();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::Combo("Fondamentale", &root, kRoots.data(), static_cast<int>(kRoots.size()))) {
        chordText_ = kRoots[static_cast<std::size_t>(root)] + quality;
        apply(chordText_);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    const std::string preview = quality.empty() ? "majeur" : quality;
    if (ImGui::BeginCombo("Type", preview.c_str())) {
        for (std::string_view q : chordQualities()) {
            const std::string name = q.empty() ? "majeur" : std::string(q);
            if (ImGui::Selectable(name.c_str(), q == quality)) {
                chordText_ = kRoots[static_cast<std::size_t>(root)] + std::string(q);
                apply(chordText_);
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}
