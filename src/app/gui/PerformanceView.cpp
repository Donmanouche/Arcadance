#include "gui/PerformanceView.hpp"

#include "midipad/Engine.hpp"
#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace midipad;
using T = PerformanceView::Theme;

namespace {

constexpr float kPi = 3.14159265358979f;

// PlayStation colors for the face buttons.
constexpr ImU32 kPsCross = IM_COL32(110, 145, 255, 255);
constexpr ImU32 kPsCircle = IM_COL32(255, 90, 90, 255);
constexpr ImU32 kPsSquare = IM_COL32(255, 110, 200, 255);
constexpr ImU32 kPsTriangle = IM_COL32(64, 205, 160, 255);
constexpr ImU32 kCubeRed = IM_COL32(225, 45, 60, 255);
constexpr ImU32 kIce = IM_COL32(170, 215, 255, 255);

ImU32 alpha(ImU32 c, float a) {
    const auto base = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    const auto value = static_cast<ImU32>(std::clamp(a * base, 0.0f, 1.0f) * 255.0f);
    return (c & ~IM_COL32_A_MASK) | (value << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto channel = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | channel(IM_COL32_A_SHIFT);
}

ImU32 shade(ImU32 c, float factor) { return mix(IM_COL32(0, 0, 0, (c >> IM_COL32_A_SHIFT) & 0xFF), c, factor); }

float em() { return ImGui::GetFontSize(); }

void text(ImDrawList* dl, ImVec2 pos, ImU32 col, const std::string& s, float size, bool centered = false) {
    ImFont* font = ImGui::GetFont();
    if (centered) {
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str());
        pos.x -= sz.x * 0.5f;
    }
    dl->AddText(font, size, pos, col, s.c_str());
}

// Text clipped to `width`, ending with "…" when cut.
std::string fit(const std::string& s, float size, float width) {
    ImFont* font = ImGui::GetFont();
    if (font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= width)
        return s;
    std::string cut = s;
    while (!cut.empty() && font->CalcTextSizeA(size, FLT_MAX, 0, (cut + "…").c_str()).x > width) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)  // UTF-8 continuation
            cut.pop_back();
    }
    return cut + "…";
}

// Text reading bottom to top, centered on `center`, like the section labels
// of hardware-style plugins.
void verticalText(ImDrawList* dl, ImVec2 center, ImU32 col, const std::string& s, float size) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str());
    const int first = dl->VtxBuffer.Size;
    // AddText drops glyphs outside the clip rect before we rotate them: lay
    // the text out unclipped, the rotated result lands inside the panel.
    dl->PushClipRectFullScreen();
    dl->AddText(font, size, ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, s.c_str());
    dl->PopClipRect();
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        const float dx = p.x - center.x, dy = p.y - center.y;
        p = ImVec2(center.x + dy, center.y - dx);
    }
}

// What a binding plays, in a few characters: "Ebmaj11", "C3", "CC 52".
std::string compact(const TriggerBinding& b, bool flats) {
    switch (b.type) {
    case ActionType::None: return "-";
    case ActionType::Note: return noteName(b.number, flats);
    case ActionType::ControlChange: return "CC " + std::to_string(b.number);
    case ActionType::Chord: {
        if (!b.chordName.empty())
            return b.chordName;
        std::string notes;
        for (auto n : b.chord.view())
            notes += (notes.empty() ? "" : " ") + noteName(n, flats);
        return notes;
    }
    case ActionType::Transport:
        return b.transport == TransportCommand::Start ? "Lecture" : b.transport == TransportCommand::Stop ? "Arrêt" : "Reprise";
    case ActionType::ChannelStep: return std::string("Canal ") + (b.channelStep > 0 ? "+" : "") + std::to_string(b.channelStep);
    case ActionType::Freeze: return "Freeze";
    }
    return {};
}

// A small loop arrow marking toggle (bascule) bindings.
void toggleMark(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->PathArcTo(c, r, 0.2f * kPi, 1.8f * kPi, 16);
    dl->PathStroke(col, 1.5f);
    const ImVec2 tip(c.x + std::cos(1.8f * kPi) * r, c.y + std::sin(1.8f * kPi) * r);
    dl->AddTriangleFilled(ImVec2(tip.x + r * 0.55f, tip.y), ImVec2(tip.x - r * 0.25f, tip.y - r * 0.45f),
                          ImVec2(tip.x - r * 0.25f, tip.y + r * 0.45f), col);
}

void led(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool on) {
    if (on)
        dl->AddCircleFilled(c, r * 2.2f, alpha(col, 0.25f));
    dl->AddCircleFilled(c, r, on ? col : shade(col, 0.25f));
    dl->AddCircle(c, r, IM_COL32(0, 0, 0, 120), 0, 1.0f);
}

// A knob showing `value` in [0, 1]: dark body, grey track, colored arc.
void knob(ImDrawList* dl, ImVec2 c, float r, float value, ImU32 accent, bool enabled, const std::string& label,
          const std::string& valueText) {
    const float a0 = 0.75f * kPi, a1 = 2.25f * kPi;
    const float a = a0 + (a1 - a0) * std::clamp(value, 0.0f, 1.0f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.08f), r * 1.02f, IM_COL32(0, 0, 0, 90));
    dl->PathArcTo(c, r * 1.18f, a0, a1, 40);
    dl->PathStroke(IM_COL32(28, 28, 30, 255), r * 0.16f);
    if (enabled) {
        dl->PathArcTo(c, r * 1.18f, a0, a, 40);
        dl->PathStroke(accent, r * 0.16f);
    }
    dl->AddCircleFilled(c, r * 0.92f, IM_COL32(58, 60, 64, 255));
    dl->AddCircleFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.15f), r * 0.66f, IM_COL32(72, 74, 79, 255));
    dl->AddCircle(c, r * 0.92f, IM_COL32(20, 20, 22, 255), 0, 1.5f);
    const ImVec2 tip(c.x + std::cos(a) * r * 0.68f, c.y + std::sin(a) * r * 0.68f);
    dl->AddCircleFilled(tip, r * 0.11f, enabled ? accent : IM_COL32(110, 110, 110, 255));
    const float small = em() * 0.72f;
    text(dl, ImVec2(c.x, c.y - r * 1.25f - small * 1.15f), T::kDim, label, small, true);
    text(dl, ImVec2(c.x, c.y + r * 1.25f), enabled ? T::kText : T::kDim, valueText, small, true);
}

// A CRT-like screen: dark glass, content, scanlines and a vignette.
template <typename Content>
void screen(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 tint, Content&& content) {
    dl->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), IM_COL32(22, 23, 25, 255), 6.0f);
    dl->AddRectFilledMultiColor(a, b, shade(tint, 0.10f), shade(tint, 0.10f), shade(tint, 0.22f), shade(tint, 0.22f));
    dl->PushClipRect(a, b, true);
    content();
    for (float y = a.y; y < b.y; y += 3.0f)
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(0, 0, 0, 55), 1.0f);
    const float v = std::min(b.x - a.x, b.y - a.y) * 0.18f;
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + v), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), 0, 0);
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - v), b, 0, 0, IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120));
    dl->PopClipRect();
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 200), 3.0f, 1.5f);
}

// An organic shape, like the blobs of hardware-synth displays.
void blob(ImDrawList* dl, ImVec2 c, float r, ImU32 col, double t, float stretchX, float stretchY) {
    constexpr int kPoints = 64;
    auto shape = [&](float scale, ImU32 color, ImVec2 offset) {
        ImVec2 pts[kPoints];
        for (int i = 0; i < kPoints; ++i) {
            const float th = static_cast<float>(i) / kPoints * 2.0f * kPi;
            const auto tt = static_cast<float>(t);
            const float wobble = 1.0f + 0.10f * std::sin(3 * th + 1.7f * tt) + 0.06f * std::sin(5 * th - 2.3f * tt) +
                                 0.04f * std::sin(7 * th + 0.9f * tt);
            const float rr = r * scale * wobble;
            pts[i] = ImVec2(c.x + offset.x + std::cos(th) * rr * stretchX, c.y + offset.y + std::sin(th) * rr * stretchY);
        }
        dl->AddConcavePolyFilled(pts, kPoints, color);
    };
    shape(1.35f, alpha(col, 0.18f), ImVec2(0, 0));
    shape(1.0f, col, ImVec2(0, 0));
    shape(0.55f, alpha(mix(col, IM_COL32_WHITE, 0.55f), 0.8f), ImVec2(-r * 0.15f, -r * 0.18f));
}

float stickPosition(const LiveState& live, Axis a) {
    return std::clamp(static_cast<float>(live.axes[static_cast<std::size_t>(a)]) / 32767.0f, -1.0f, 1.0f);
}

float axisPosition01(const LiveState& live, Axis a) {
    const float raw = static_cast<float>(live.axes[static_cast<std::size_t>(a)]) / 32767.0f;
    return std::clamp(isTrigger(a) ? raw : (raw + 1.0f) * 0.5f, 0.0f, 1.0f);
}

// The CC an axis binding sends for a [0, 1] position.
int ccValue(const AxisBinding& b, float position) {
    if (b.invert)
        position = 1.0f - position;
    return b.min + static_cast<int>(std::lround(position * static_cast<float>(b.max - b.min)));
}

std::optional<HatDir> liveHat(const LiveState& live) {
    const int y = int(live.isDown(Button::DpadUp)) - int(live.isDown(Button::DpadDown));
    const int x = int(live.isDown(Button::DpadRight)) - int(live.isDown(Button::DpadLeft));
    static constexpr std::optional<HatDir> kFromXY[3][3] = {
        {HatDir::DownLeft, HatDir::Down, HatDir::DownRight},
        {HatDir::Left, std::nullopt, HatDir::Right},
        {HatDir::UpLeft, HatDir::Up, HatDir::UpRight},
    };
    return kFromXY[y + 1][x + 1];
}

// The button whose freeze action covers `axes`, if any.
std::optional<Button> freezeButton(const Mapping& m, std::uint8_t axes) {
    for (int b = 0; b < kButtonCount; ++b) {
        const auto& t = m.buttons[static_cast<std::size_t>(b)];
        if (t.type == ActionType::Freeze && (t.freezeAxes & axes))
            return static_cast<Button>(b);
    }
    return std::nullopt;
}

struct Panel {
    float x, y, w, h;
    float bodyTop;  // below the header
};

Panel panel(ImDrawList* dl, float x, float y, float w, float h, const char* title, bool active) {
    const float header = em() * 1.9f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, 4.0f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + header), T::kPanelHeader, 4.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, 4.0f, 1.0f);
    // Grip, like the drag handles of the reference design.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            dl->AddCircleFilled(ImVec2(x + em() * (0.7f + 0.3f * static_cast<float>(j)),
                                       y + header * 0.5f + em() * 0.3f * static_cast<float>(i - 1)),
                                1.3f, T::kDim);
    text(dl, ImVec2(x + em() * 1.6f, y + (header - em()) * 0.5f), T::kText, title, em());
    led(dl, ImVec2(x + w - em() * 0.9f, y + header * 0.5f), em() * 0.28f, T::kGreen, active);
    return {x, y, w, h, y + header};
}

void sectionLabel(ImDrawList* dl, float x, float top, float bottom, const char* label) {
    verticalText(dl, ImVec2(x + em() * 0.65f, (top + bottom) * 0.5f), T::kDim, label, em() * 0.68f);
    dl->AddLine(ImVec2(x + em() * 1.25f, top + 2), ImVec2(x + em() * 1.25f, bottom - 2), IM_COL32(0, 0, 0, 70));
}

bool hoverButton(const char* id, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton(id, ImVec2(std::max(b.x - a.x, 1.0f), std::max(b.y - a.y, 1.0f)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(255, 255, 255, 60), 4.0f, 1.0f);
    }
    return ImGui::IsItemClicked();
}

}  // namespace

float PerformanceView::smooth(int id, float target, float speed) {
    auto& v = smoothed_[static_cast<std::size_t>(id)];
    if (!smoothedInit_[static_cast<std::size_t>(id)]) {
        smoothedInit_[static_cast<std::size_t>(id)] = true;
        v = target;
    }
    v += (target - v) * std::min(1.0f, dt_ * speed);
    return v;
}

void PerformanceView::click(const std::string& spec) { clicked_ = spec; }

std::optional<std::string> PerformanceView::draw(const Mapping& mapping, const LiveState& live,
                                                 const std::optional<Key>& key) {
    time_ = ImGui::GetTime();
    dt_ = ImGui::GetIO().DeltaTime;
    flats_ = key && prefersFlats(*key);
    clicked_.reset();

    // Flash the buttons pressed since the last frame.
    const std::uint32_t pressed = live.buttons & ~previousButtons_;
    for (int b = 0; b < 32; ++b)
        if ((pressed >> b) & 1u)
            lastPress_[static_cast<std::size_t>(b)] = time_;
    previousButtons_ = live.buttons;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = em() * 0.6f;
    const float stripH = std::max(avail.y * 0.30f, em() * 9.5f);
    const float topH = avail.y - stripH - gap;
    const float colW = (avail.x - 4 * gap) / 5.0f;

    float x = origin.x;
    drawStick(0, mapping, live, x, origin.y, colW, topH);
    x += colW + gap;
    drawStick(1, mapping, live, x, origin.y, colW, topH);
    x += colW + gap;
    drawTriggers(mapping, live, x, origin.y, colW, topH);
    x += colW + gap;
    drawDpad(mapping, live, x, origin.y, colW, topH);
    x += colW + gap;
    drawTouchpad(mapping, live, x, origin.y, colW, topH);
    drawButtons(mapping, live, origin.x, origin.y + topH + gap, avail.x, stripH);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y));
    ImGui::Dummy(ImVec2(0, 0));
    return clicked_;
}

void PerformanceView::drawStick(int index, const Mapping& m, const LiveState& live, float x, float y, float w,
                                float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Axis ax = index == 0 ? Axis::LeftX : Axis::RightX;
    const Axis ay = index == 0 ? Axis::LeftY : Axis::RightY;
    const ImU32 accent = index == 0 ? T::kBlue : T::kOrange;
    const auto& bx = m.axes[static_cast<std::size_t>(ax)];
    const auto& by = m.axes[static_cast<std::size_t>(ay)];
    const auto axisBits = static_cast<std::uint8_t>((1u << static_cast<unsigned>(ax)) | (1u << static_cast<unsigned>(ay)));
    const bool frozen = live.frozenAxes & axisBits;

    const Panel p = panel(dl, x, y, w, h, index == 0 ? "STICK GAUCHE" : "STICK DROIT",
                          live.connected && (bx.enabled || by.enabled));
    const float pad = em() * 0.6f;
    const ImVec2 sa(x + pad, p.bodyTop + pad), sb(x + w - pad, p.bodyTop + pad + std::min(w * 0.78f, h * 0.42f));

    const float sx = smooth(index * 4 + 0, stickPosition(live, ax), 18.0f);
    const float sy = smooth(index * 4 + 1, stickPosition(live, ay), 18.0f);
    screen(dl, sa, sb, accent, [&] {
        const ImVec2 c((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f);
        const float rw = (sb.x - sa.x) * 0.5f, rh = (sb.y - sa.y) * 0.5f;
        dl->AddLine(ImVec2(sa.x, c.y), ImVec2(sb.x, c.y), alpha(accent, 0.12f));
        dl->AddLine(ImVec2(c.x, sa.y), ImVec2(c.x, sb.y), alpha(accent, 0.12f));
        dl->AddCircle(c, rh * 0.8f, alpha(accent, 0.10f), 48);
        const float deflection = std::min(1.0f, std::sqrt(sx * sx + sy * sy));
        const float r = rh * (0.20f + 0.12f * deflection);
        const ImVec2 pos(c.x + sx * (rw - r * 1.4f), c.y + sy * (rh - r * 1.4f));
        blob(dl, pos, r, frozen ? kIce : accent, time_ * (frozen ? 0.2 : 1.0), 1.0f + std::abs(sx) * 0.35f,
             1.0f + std::abs(sy) * 0.35f);
        if (frozen)
            text(dl, ImVec2(c.x, sb.y - em() * 1.3f), kIce, "FREEZE", em() * 0.8f, true);
    });
    if (hoverButton(index == 0 ? "##stickL" : "##stickR", sa, sb))
        click("tab:axes");

    // Knobs: the CC each axis sends.
    const float kTop = sb.y + em() * 2.2f;
    const float r = std::min(w * 0.135f, em() * 1.8f);
    const float kc = kTop + r + em() * 0.4f;
    sectionLabel(dl, x, sb.y + em() * 0.8f, kc + r + em() * 1.6f, "VALEURS");
    auto axisKnob = [&](float cx, const AxisBinding& b, Axis a, const char* name, int smoothId) {
        const float pos = axisPosition01(live, a);
        const int value = ccValue(b, pos);
        const float shown = smooth(smoothId, static_cast<float>(value) / 127.0f, 20.0f);
        char label[32], val[16];
        std::snprintf(label, sizeof label, b.enabled ? "%s  CC %d" : "%s", name, b.cc);
        std::snprintf(val, sizeof val, b.enabled ? (frozen ? "gelé" : "%d") : "-", value);
        knob(dl, ImVec2(cx, kc), r, shown, frozen ? kIce : accent, b.enabled, label, val);
    };
    const float left = x + em() * 1.4f, right = x + w;
    axisKnob(left + (right - left) * 0.28f, bx, ax, "X", index * 4 + 2);
    axisKnob(left + (right - left) * 0.72f, by, ay, "Y", index * 4 + 3);

    // Freeze status.
    const float fTop = kc + r + em() * 2.2f;
    if (fTop + em() * 2 < y + h) {
        sectionLabel(dl, x, fTop, std::min(fTop + em() * 3.0f, y + h - 4), "FREEZE");
        const auto button = freezeButton(m, axisBits);
        led(dl, ImVec2(left + em() * 0.8f, fTop + em() * 1.0f), em() * 0.32f, kIce, frozen);
        const std::string state = button ? label(*button) + (frozen ? " : gelé" : " : libre") : "aucun bouton";
        text(dl, ImVec2(left + em() * 1.6f, fTop + em() * 0.5f), frozen ? kIce : T::kDim, state, em() * 0.85f);
    }
}

void PerformanceView::drawTriggers(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& bl = m.axes[static_cast<std::size_t>(Axis::LeftTrigger)];
    const auto& br = m.axes[static_cast<std::size_t>(Axis::RightTrigger)];
    const auto& dl2 = m.buttons[static_cast<std::size_t>(Button::LeftTrigger)];
    const auto& dr2 = m.buttons[static_cast<std::size_t>(Button::RightTrigger)];
    const Panel p = panel(dl, x, y, w, h, "GÂCHETTES",
                          live.connected && (bl.enabled || br.enabled || dl2.isSet() || dr2.isSet()));
    const float pad = em() * 0.6f;
    const ImVec2 sa(x + pad, p.bodyTop + pad), sb(x + w - pad, p.bodyTop + pad + std::min(w * 0.78f, h * 0.42f));
    const float vl = smooth(8, axisPosition01(live, Axis::LeftTrigger), 20.0f);
    const float vr = smooth(9, axisPosition01(live, Axis::RightTrigger), 20.0f);
    const bool soundL = live.isSounding(Engine::slotOf(Button::LeftTrigger));
    const bool soundR = live.isSounding(Engine::slotOf(Button::RightTrigger));

    screen(dl, sa, sb, T::kPink, [&] {
        const float W = sb.x - sa.x, H = sb.y - sa.y;
        for (int i = 1; i < 4; ++i) {
            const float gy = sa.y + H * static_cast<float>(i) / 4.0f;
            dl->AddLine(ImVec2(sa.x, gy), ImVec2(sb.x, gy), IM_COL32(255, 255, 255, 14));
        }
        // Digital threshold.
        const float ty = sb.y - H * 0.12f - (H * 0.76f) * Engine::kTriggerPress;
        for (float dx = sa.x; dx < sb.x; dx += 8)
            dl->AddLine(ImVec2(dx, ty), ImVec2(dx + 4, ty), alpha(T::kPink, 0.35f));
        // One envelope per trigger: attack up to the pressure, then release.
        auto envelope = [&](float x0, float x1, float v, bool sounding, const char* name) {
            const float base = sb.y - H * 0.12f;
            const float peak = base - (H * 0.76f) * v;
            const ImVec2 pts[] = {{x0, base}, {x0 + (x1 - x0) * 0.25f, peak}, {x0 + (x1 - x0) * 0.6f, peak},
                                  {x1, base}};
            const ImU32 col = sounding ? T::kPink : mix(T::kPink, IM_COL32(180, 180, 180, 255), 0.4f);
            const ImVec2 fill[] = {pts[0], pts[1], pts[2], pts[3]};
            dl->AddConvexPolyFilled(fill, 4, alpha(col, 0.18f + 0.25f * v));
            dl->AddPolyline(pts, 4, col, 2.0f);
            const float hs = em() * 0.28f;
            dl->AddRectFilled(ImVec2(pts[1].x - hs, peak - hs), ImVec2(pts[1].x + hs, peak + hs), col);
            text(dl, ImVec2((x0 + x1) * 0.5f, sa.y + em() * 0.3f), sounding ? T::kText : T::kDim, name, em() * 0.8f,
                 true);
        };
        envelope(sa.x + W * 0.04f, sa.x + W * 0.48f, vl, soundL, "L2");
        envelope(sa.x + W * 0.52f, sa.x + W * 0.96f, vr, soundR, "R2");
    });
    if (hoverButton("##triggers", sa, sb))
        click("tab:axes");

    const float r = std::min(w * 0.135f, em() * 1.8f);
    const float kc = sb.y + em() * 2.2f + r + em() * 0.4f;
    sectionLabel(dl, x, sb.y + em() * 0.8f, kc + r + em() * 1.6f, "CC");
    const float left = x + em() * 1.4f, right = x + w;
    auto trigKnob = [&](float cx, const AxisBinding& b, Axis a, const char* name, int id) {
        const int value = ccValue(b, axisPosition01(live, a));
        char label[32], val[16];
        std::snprintf(label, sizeof label, b.enabled ? "%s  CC %d" : "%s", name, b.cc);
        std::snprintf(val, sizeof val, b.enabled ? "%d" : "-", value);
        knob(dl, ImVec2(cx, kc), r, smooth(id, static_cast<float>(value) / 127.0f, 20.0f), T::kPink, b.enabled, label,
             val);
    };
    trigKnob(left + (right - left) * 0.28f, bl, Axis::LeftTrigger, "L2", 10);
    trigKnob(left + (right - left) * 0.72f, br, Axis::RightTrigger, "R2", 11);

    // The digital L2 / R2 buttons.
    const float bTop = kc + r + em() * 2.2f;
    if (bTop + em() * 3 < y + h) {
        sectionLabel(dl, x, bTop, std::min(bTop + em() * 3.6f, y + h - 4), "BOUTONS");
        auto row = [&](float ry, Button b, const TriggerBinding& t, bool sounding) {
            led(dl, ImVec2(left + em() * 0.8f, ry + em() * 0.55f), em() * 0.3f, T::kPink, sounding);
            text(dl, ImVec2(left + em() * 1.6f, ry), sounding ? T::kText : T::kDim,
                 fit(label(b) + "   " + compact(t, flats_) + (t.isSet() && t.mode == Mode::Toggle ? ", bascule" : ""),
                     em() * 0.82f, right - left - em() * 2.2f),
                 em() * 0.82f);
            if (hoverButton(b == Button::LeftTrigger ? "##l2" : "##r2", ImVec2(left, ry - 2),
                            ImVec2(right - em() * 0.4f, ry + em() * 1.3f)))
                click(b == Button::LeftTrigger ? "button:left_trigger" : "button:right_trigger");
        };
        row(bTop + em() * 0.3f, Button::LeftTrigger, dl2, soundL);
        row(bTop + em() * 1.8f, Button::RightTrigger, dr2, soundR);
    }
}

void PerformanceView::drawDpad(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool any = false;
    for (const auto& t : m.dpad)
        any |= t.isSet();
    const Panel p = panel(dl, x, y, w, h, "CROIX", live.connected && any);
    const float pad = em() * 0.6f;
    const ImVec2 sa(x + pad, p.bodyTop + pad), sb(x + w - pad, p.bodyTop + pad + std::min(w * 0.78f, h * 0.42f));
    const auto current = liveHat(live);
    static constexpr std::optional<HatDir> kGrid[3][3] = {
        {HatDir::UpLeft, HatDir::Up, HatDir::UpRight},
        {HatDir::Left, std::nullopt, HatDir::Right},
        {HatDir::DownLeft, HatDir::Down, HatDir::DownRight},
    };

    screen(dl, sa, sb, IM_COL32(150, 150, 160, 255), [&] {
        // Isometric cubes; the direction played rises and turns red.
        const float W = sb.x - sa.x, H = sb.y - sa.y;
        const float s = std::min(W / 7.0f, H / 5.4f);
        const ImVec2 c((sa.x + sb.x) * 0.5f, sa.y + H * 0.30f);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const auto dir = kGrid[row][col];
                const int id = 20 + row * 3 + col;
                const bool active = dir && current == dir;
                const bool sounding = dir && live.isSounding(Engine::slotOf(*dir));
                const float lift = smooth(id, active ? 1.0f : sounding ? 0.45f : 0.0f, 16.0f) * s * 0.9f;
                const ImVec2 base(c.x + static_cast<float>(col - row) * s, c.y + static_cast<float>(col + row) * s * 0.5f);
                const float depth = s * 0.55f + lift;
                const ImVec2 top(base.x, base.y - lift);
                ImU32 face = dir ? IM_COL32(225, 225, 228, 255) : IM_COL32(110, 110, 115, 255);
                if (dir && !m.dpad[static_cast<std::size_t>(*dir)].isSet())
                    face = IM_COL32(150, 150, 155, 255);
                face = mix(face, kCubeRed, smooth(id + 10, active || sounding ? 1.0f : 0.0f, 12.0f));
                const ImVec2 t0(top.x, top.y - s * 0.5f), t1(top.x + s, top.y), t2(top.x, top.y + s * 0.5f),
                    t3(top.x - s, top.y);
                const ImVec2 left[] = {t3, t2, ImVec2(t2.x, t2.y + depth), ImVec2(t3.x, t3.y + depth)};
                const ImVec2 right[] = {t2, t1, ImVec2(t1.x, t1.y + depth), ImVec2(t2.x, t2.y + depth)};
                const ImVec2 topFace[] = {t0, t1, t2, t3};
                dl->AddConvexPolyFilled(left, 4, shade(face, 0.62f));
                dl->AddConvexPolyFilled(right, 4, shade(face, 0.42f));
                dl->AddConvexPolyFilled(topFace, 4, face);
            }
        }
        if (current) {
            const auto& t = m.dpad[static_cast<std::size_t>(*current)];
            dl->AddRectFilled(ImVec2(sa.x, sb.y - em() * 1.5f), sb, IM_COL32(0, 0, 0, 150));
            text(dl, ImVec2((sa.x + sb.x) * 0.5f, sb.y - em() * 1.3f), T::kText,
                 fit(label(*current) + " : " + compact(t, flats_), em() * 0.8f, W - em()), em() * 0.8f, true);
        }
    });
    if (hoverButton("##dpadscreen", sa, sb))
        click(current ? "dpad:" + std::string(toString(*current)) : "tab:dpad");

    // The eight directions and what they play.
    const float left = x + em() * 1.4f, right = x + w - em() * 0.4f;
    const float top = sb.y + em() * 0.9f;
    const float rowH = std::min(em() * 1.45f, (y + h - top - em() * 0.4f) / 8.0f);
    sectionLabel(dl, x, top, top + rowH * 8, "DIRECTIONS");
    static constexpr HatDir kOrder[] = {HatDir::Up,   HatDir::UpRight,  HatDir::Right, HatDir::DownRight,
                                        HatDir::Down, HatDir::DownLeft, HatDir::Left,  HatDir::UpLeft};
    for (int i = 0; i < 8; ++i) {
        const HatDir d = kOrder[i];
        const float ry = top + rowH * static_cast<float>(i);
        const bool active = current == d;
        const bool sounding = live.isSounding(Engine::slotOf(d));
        if (active)
            dl->AddRectFilled(ImVec2(left, ry), ImVec2(right, ry + rowH - 1), alpha(kCubeRed, 0.25f), 3.0f);
        led(dl, ImVec2(left + em() * 0.5f, ry + rowH * 0.5f), em() * 0.26f, kCubeRed, active || sounding);
        const float size = em() * 0.8f;
        text(dl, ImVec2(left + em() * 1.1f, ry + (rowH - size) * 0.5f), T::kDim, label(d), size);
        const float descX = left + em() * 5.6f;
        const auto& t = m.dpad[static_cast<std::size_t>(d)];
        const bool toggle = t.isSet() && t.mode == Mode::Toggle;
        const float markR = em() * 0.28f;
        text(dl, ImVec2(descX, ry + (rowH - size) * 0.5f), active || sounding ? T::kText : mix(T::kDim, T::kText, 0.4f),
             fit(compact(t, flats_), size, right - descX - (toggle ? markR * 3 : 0)), size);
        if (toggle)
            toggleMark(dl, ImVec2(right - markR * 1.6f, ry + rowH * 0.5f), markR, sounding ? kCubeRed : T::kDim);
        if (hoverButton(("##dir" + std::to_string(i)).c_str(), ImVec2(left, ry), ImVec2(right, ry + rowH - 1)))
            click("dpad:" + std::string(toString(d)));
    }
}

void PerformanceView::drawTouchpad(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool any = m.fingers[0].mode != TouchMode::Off || m.fingers[1].mode != TouchMode::Off;
    const Panel p = panel(dl, x, y, w, h, "TOUCHPAD", live.connected && live.hasTouchpad && any);
    const float pad = em() * 0.6f;
    const ImVec2 sa(x + pad, p.bodyTop + pad), sb(x + w - pad, p.bodyTop + pad + std::min(w * 0.78f, h * 0.42f));
    const ImU32 fingerColors[] = {IM_COL32(120, 220, 255, 255), T::kOrange};

    for (int f = 0; f < kFingerCount; ++f) {
        auto& trail = trails_[static_cast<std::size_t>(f)];
        const auto& finger = live.fingers[static_cast<std::size_t>(f)];
        if (finger.down) {
            trail.points.emplace_back(finger.x, finger.y);
            trail.times.push_back(time_);
        }
        while (!trail.times.empty() && time_ - trail.times.front() > 0.45) {
            trail.times.pop_front();
            trail.points.pop_front();
        }
    }

    screen(dl, sa, sb, T::kPink, [&] {
        const float W = sb.x - sa.x, H = sb.y - sa.y;
        for (float gx = sa.x + 6; gx < sb.x; gx += 12)
            for (float gy = sa.y + 6; gy < sb.y; gy += 12)
                dl->AddCircleFilled(ImVec2(gx, gy), 1.0f, IM_COL32(255, 255, 255, 22));
        dl->AddLine(ImVec2(sa.x + W * 0.5f, sa.y), ImVec2(sa.x + W * 0.5f, sb.y), IM_COL32(255, 255, 255, 25));
        dl->AddLine(ImVec2(sa.x, sa.y + H * 0.5f), ImVec2(sb.x, sa.y + H * 0.5f), IM_COL32(255, 255, 255, 25));
        // Zones that are playing.
        for (int f = 0; f < kFingerCount; ++f) {
            if (m.fingers[static_cast<std::size_t>(f)].mode != TouchMode::Zones)
                continue;
            for (int z = 0; z < kZoneCount; ++z) {
                const float on = smooth(40 + f * 4 + z, live.isSounding(Engine::slotOf(f, static_cast<Zone>(z))) ? 1.0f : 0.0f, 10.0f);
                if (on < 0.01f)
                    continue;
                const float zx = sa.x + static_cast<float>(z % 2) * W * 0.5f, zy = sa.y + static_cast<float>(z / 2) * H * 0.5f;
                dl->AddRectFilled(ImVec2(zx + 2, zy + 2), ImVec2(zx + W * 0.5f - 2, zy + H * 0.5f - 2),
                                  alpha(fingerColors[f], 0.30f * on), 3.0f);
            }
        }
        for (int f = 0; f < kFingerCount; ++f) {
            const auto& trail = trails_[static_cast<std::size_t>(f)];
            for (std::size_t i = 1; i < trail.points.size(); ++i) {
                const float age = static_cast<float>((time_ - trail.times[i]) / 0.45);
                const ImVec2 a(sa.x + trail.points[i - 1].first * W, sa.y + trail.points[i - 1].second * H);
                const ImVec2 b(sa.x + trail.points[i].first * W, sa.y + trail.points[i].second * H);
                dl->AddLine(a, b, alpha(fingerColors[f], 0.8f * (1.0f - age)), 3.0f * (1.0f - age) + 1.0f);
            }
            const auto& finger = live.fingers[static_cast<std::size_t>(f)];
            if (finger.down) {
                const ImVec2 c(sa.x + finger.x * W, sa.y + finger.y * H);
                const float pulse = 1.0f + 0.15f * std::sin(static_cast<float>(time_) * 9.0f);
                dl->AddCircleFilled(c, em() * 1.0f * pulse, alpha(fingerColors[f], 0.18f));
                dl->AddCircleFilled(c, em() * 0.45f, fingerColors[f]);
                text(dl, ImVec2(c.x + em() * 0.6f, c.y - em() * 1.2f), IM_COL32_WHITE, f == 0 ? "1" : "2", em() * 0.8f);
            }
        }
        if (!live.hasTouchpad && live.connected)
            text(dl, ImVec2((sa.x + sb.x) * 0.5f, sa.y + H * 0.42f), T::kDim, "touchpad non détecté", em() * 0.8f, true);
    });
    if (hoverButton("##touchscreen", sa, sb))
        click("tab:touchpad");

    // Per finger: X/Y knobs or the four zones.
    const float left = x + em() * 1.4f, right = x + w - em() * 0.4f;
    float top = sb.y + em() * 0.9f;
    const float block = (y + h - top - em() * 0.4f) / 2.0f;
    for (int f = 0; f < kFingerCount; ++f) {
        const FingerBinding& fb = m.fingers[static_cast<std::size_t>(f)];
        const auto& finger = live.fingers[static_cast<std::size_t>(f)];
        const float bTop = top + block * static_cast<float>(f);
        sectionLabel(dl, x, bTop, bTop + block - em() * 0.3f, f == 0 ? "DOIGT 1" : "DOIGT 2");
        if (fb.mode == TouchMode::Axis) {
            const float r = std::min({w * 0.11f, em() * 1.25f, block * 0.22f});
            const float kc = bTop + block * 0.5f;
            auto touchKnob = [&](float cx, const AxisBinding& b, float pos, const char* name, int id) {
                const int value = ccValue(b, pos);
                char lbl[32], val[16];
                std::snprintf(lbl, sizeof lbl, b.enabled ? "%s  CC %d" : "%s", name, b.cc);
                std::snprintf(val, sizeof val, b.enabled && finger.down ? "%d" : "-", value);
                knob(dl, ImVec2(cx, kc), r, smooth(id, finger.down ? static_cast<float>(value) / 127.0f : 0.0f, 16.0f),
                     fingerColors[f], b.enabled, lbl, val);
            };
            touchKnob(left + (right - left) * 0.28f, fb.x, finger.x, "X", 50 + f * 2);
            touchKnob(left + (right - left) * 0.72f, fb.y, 1.0f - finger.y, "Y", 51 + f * 2);
            if (hoverButton(f == 0 ? "##f1" : "##f2", ImVec2(left, bTop), ImVec2(right, bTop + block - em() * 0.3f)))
                click("tab:touchpad");
        } else if (fb.mode == TouchMode::Zones) {
            const float cw = (right - left - em() * 0.4f) / 2.0f, ch = (block - em() * 0.8f) / 2.0f;
            for (int z = 0; z < kZoneCount; ++z) {
                const ImVec2 a(left + static_cast<float>(z % 2) * (cw + em() * 0.4f),
                               bTop + static_cast<float>(z / 2) * (ch + em() * 0.3f));
                const ImVec2 b(a.x + cw, a.y + ch);
                const bool on = live.isSounding(Engine::slotOf(f, static_cast<Zone>(z)));
                dl->AddRectFilled(a, b, on ? alpha(fingerColors[f], 0.45f) : IM_COL32(45, 47, 51, 255), 4.0f);
                dl->AddRect(a, b, on ? fingerColors[f] : T::kBorder, 4.0f);
                text(dl, ImVec2(a.x + em() * 0.3f, a.y + (ch - em() * 0.75f) * 0.5f), on ? T::kText : T::kDim,
                     fit(compact(fb.zones[static_cast<std::size_t>(z)], flats_), em() * 0.75f, cw - em() * 0.5f),
                     em() * 0.75f);
                const std::string spec = "zone:" + std::to_string(f + 1) + ":" + std::string(toString(static_cast<Zone>(z)));
                if (hoverButton(("##" + spec).c_str(), a, b))
                    click(spec);
            }
        } else {
            text(dl, ImVec2(left + em() * 0.4f, bTop + block * 0.4f), T::kDim, "désactivé", em() * 0.85f);
            if (hoverButton(f == 0 ? "##f1off" : "##f2off", ImVec2(left, bTop), ImVec2(right, bTop + block - em() * 0.3f)))
                click("tab:touchpad");
        }
    }
}

void PerformanceView::drawButtons(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool any = false;
    for (const auto& t : m.buttons)
        any |= t.isSet();
    const Panel p = panel(dl, x, y, w, h, "BOUTONS", live.connected && any);

    struct Pad {
        Button button;
        ImU32 color;
        bool big;
    };
    static constexpr Pad kPads[] = {
        {Button::LeftShoulder, 0, false}, {Button::LeftTrigger, 0, false}, {Button::LeftStick, 0, false},
        {Button::Back, 0, false},         {Button::South, kPsCross, true}, {Button::East, kPsCircle, true},
        {Button::West, kPsSquare, true},  {Button::North, kPsTriangle, true}, {Button::Start, 0, false},
        {Button::Guide, 0, false},        {Button::Touchpad, 0, false},   {Button::Misc1, 0, false},
        {Button::RightStick, 0, false},   {Button::RightTrigger, 0, false}, {Button::RightShoulder, 0, false},
    };
    const float pad = em() * 0.8f;
    const float slot = (w - 2 * pad) / static_cast<float>(std::size(kPads));
    const float bodyH = y + h - p.bodyTop;
    const float cy = p.bodyTop + bodyH * 0.40f;
    for (std::size_t i = 0; i < std::size(kPads); ++i) {
        const Pad& pd = kPads[i];
        const float cx = x + pad + slot * (static_cast<float>(i) + 0.5f);
        const float r = std::min(slot * (pd.big ? 0.40f : 0.30f), bodyH * (pd.big ? 0.27f : 0.20f));
        const ImU32 color = pd.color ? pd.color : IM_COL32(200, 200, 205, 255);
        const int sdl = static_cast<int>(pd.button);
        bool down = false;
        if (pd.button == Button::LeftTrigger)
            down = axisPosition01(live, Axis::LeftTrigger) >= Engine::kTriggerPress;
        else if (pd.button == Button::RightTrigger)
            down = axisPosition01(live, Axis::RightTrigger) >= Engine::kTriggerPress;
        else
            down = live.isDown(pd.button);
        const bool sounding = live.isSounding(Engine::slotOf(pd.button));
        const float on = sounding ? 1.0f : down ? 0.7f : 0.0f;

        // Body, glow while sounding, ring, symbol.
        dl->AddCircleFilled(ImVec2(cx, cy + r * 0.08f), r * 1.05f, IM_COL32(0, 0, 0, 100));
        if (on > 0)
            dl->AddCircleFilled(ImVec2(cx, cy), r * 1.45f, alpha(color, 0.22f * on));
        dl->AddCircleFilled(ImVec2(cx, cy), r, down ? IM_COL32(78, 80, 86, 255) : IM_COL32(52, 54, 58, 255));
        dl->AddCircle(ImVec2(cx, cy), r, sounding ? color : alpha(color, 0.45f), 0, sounding ? 3.0f : 1.5f);
        const ImU32 sym = on > 0 ? color : alpha(color, 0.75f);
        const float s = r * 0.45f;
        switch (pd.button) {
        case Button::South:
            dl->AddLine(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), sym, r * 0.14f);
            dl->AddLine(ImVec2(cx - s, cy + s), ImVec2(cx + s, cy - s), sym, r * 0.14f);
            break;
        case Button::East:
            dl->AddCircle(ImVec2(cx, cy), s * 1.05f, sym, 0, r * 0.14f);
            break;
        case Button::West:
            dl->AddRect(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), sym, 0.0f, r * 0.14f);
            break;
        case Button::North:
            dl->AddTriangle(ImVec2(cx, cy - s * 1.15f), ImVec2(cx + s * 1.1f, cy + s * 0.75f),
                            ImVec2(cx - s * 1.1f, cy + s * 0.75f), sym, r * 0.14f);
            break;
        default: {
            const char* code = pd.button == Button::Back       ? "CRÉ"
                               : pd.button == Button::Start    ? "OPT"
                               : pd.button == Button::Touchpad ? "TP"
                               : pd.button == Button::Misc1    ? "MIC"
                                                                : nullptr;
            const std::string name = code ? code : label(pd.button);
            const float size = std::min(em() * 0.85f, r * 0.62f);
            text(dl, ImVec2(cx, cy - size * 0.55f), sym, name, size, true);
        }
        }
        // Flash on each press.
        const double since = time_ - lastPress_[static_cast<std::size_t>(sdl)];
        if (pd.button != Button::LeftTrigger && pd.button != Button::RightTrigger && since < 0.4) {
            const auto k = static_cast<float>(since / 0.4);
            dl->AddCircle(ImVec2(cx, cy), r * (1.0f + 0.7f * k), alpha(color, 1.0f - k), 0, 3.0f * (1.0f - k) + 1.0f);
        }

        // Name and action below.
        const float small = em() * 0.72f;
        float ty = cy + std::max(r, slot * 0.40f) * 1.2f + em() * 0.2f;
        text(dl, ImVec2(cx, ty), T::kDim, fit(label(pd.button), small, slot - 4), small, true);
        ty += small * 1.3f;
        const auto& binding = m.buttons[static_cast<std::size_t>(pd.button)];
        text(dl, ImVec2(cx, ty), sounding ? T::kText : mix(T::kDim, T::kText, 0.5f),
             fit(compact(binding, flats_), small, slot - 4), small, true);
        if (binding.mode == Mode::Toggle && binding.isSet())
            toggleMark(dl, ImVec2(cx, ty + small * 2.0f), small * 0.42f, sounding ? color : T::kDim);

        if (hoverButton(("##pad" + std::to_string(i)).c_str(), ImVec2(cx - slot * 0.5f + 2, p.bodyTop + 2),
                        ImVec2(cx + slot * 0.5f - 2, y + h - 2)))
            click("button:" + std::string(toString(pd.button)));
    }
}
