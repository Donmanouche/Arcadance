#include "gui/PerformanceView.hpp"

#include "midipad/Engine.hpp"
#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace midipad;
using T = PerformanceView::Theme;

namespace {

constexpr float kPi = 3.14159265358979f;

// PlayStation colors for the face buttons.
constexpr ImU32 kPsCross = IM_COL32(0x7C, 0x9B, 0xFF, 255);
constexpr ImU32 kPsCircle = IM_COL32(0xFF, 0x64, 0x64, 255);
constexpr ImU32 kPsSquare = IM_COL32(0xFF, 0x7A, 0xD0, 255);
constexpr ImU32 kPsTriangle = IM_COL32(0x4A, 0xD9, 0xAE, 255);
constexpr ImU32 kPadGrey = IM_COL32(0xD6, 0xDA, 0xE0, 255);
constexpr ImU32 kTrack = IM_COL32(0x15, 0x17, 0x1A, 255);
constexpr ImU32 kKnob = IM_COL32(0x43, 0x47, 0x4E, 255);
constexpr ImU32 kGraph = IM_COL32(0x20, 0x22, 0x26, 255);
constexpr ImU32 kBadge = IM_COL32(0x45, 0x4A, 0x52, 255);

// Fonts of the current frame (set by draw()).
ImFont* gMedium = nullptr;
ImFont* gBold = nullptr;
ImFont* gCondensed = nullptr;

// PerformanceView::Look: 0 classic, 1 green phosphor, 2 blue phosphor.
int gLook = 0;

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

// Sizes are designed for a 16 px base font; px() follows the UI scale.
float px(float v) { return v * ImGui::GetFontSize() / 16.0f; }

enum class H { Left, Center, Right };
enum class V { Top, Middle };

float textWidth(ImFont* f, float size, const std::string& s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x; }

// The right edge of a note's bar in the piano roll: a hair short of where it
// ends, so a note played again at once (a retrigger) reads as a new one.
float noteBarEnd(float start, float end, bool ended) { return std::max(ended ? end - px(1.5f) : end, start + px(2)); }

void text(ImDrawList* dl, ImFont* f, float size, float x, float y, ImU32 col, const std::string& s, H h = H::Left,
          V v = V::Top) {
    const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str());
    if (h == H::Center)
        x -= sz.x * 0.5f;
    else if (h == H::Right)
        x -= sz.x;
    if (v == V::Middle)
        y -= sz.y * 0.5f;
    dl->AddText(f, size, ImVec2(x, y), col, s.c_str());
}

// Text cut to `width`, ending with "…" when cut.
std::string fit(ImFont* f, float size, const std::string& s, float width) {
    if (textWidth(f, size, s) <= width)
        return s;
    std::string cut = s;
    while (!cut.empty() && textWidth(f, size, cut + "…") > width) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)  // UTF-8 continuation
            cut.pop_back();
    }
    return cut + "…";
}

// A rounded badge with centered text; returns its width.
float badge(ImDrawList* dl, ImFont* f, float size, float x, float y, float h, const std::string& s, ImU32 fg, ImU32 bg,
            ImU32 border, H align = H::Center, float pad = 7) {
    const float w = textWidth(f, size, s) + px(pad) * 2;
    if (align == H::Center)
        x -= w * 0.5f;
    else if (align == H::Right)
        x -= w;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, h * 0.5f);
    if (border)
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), border, h * 0.5f, px(1.3f));
    text(dl, f, size, x + w * 0.5f, y + h * 0.5f, fg, s, H::Center, V::Middle);
    return w;
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
        return b.transport == TransportCommand::Start ? "Start" : b.transport == TransportCommand::Stop ? "Stop" : "Continue";
    case ActionType::ChannelStep: return std::string("Ch ") + (b.channelStep > 0 ? "+" : "") + std::to_string(b.channelStep);
    case ActionType::Freeze: return "Freeze";
    case ActionType::LoopRecord: return "Rec";
    case ActionType::Shift: return "Shift";
    case ActionType::TapTempo: return "Tap";
    case ActionType::SessionScreen: return "Session";
    case ActionType::RecenterTilt: return "Center";
    case ActionType::Retrigger: return "Retrig \xC3\x97" + std::to_string(b.retriggers);
    case ActionType::ProgramChange:
        return b.programStep != 0 ? std::string("PC ") + (b.programStep > 0 ? "+1" : "-1") : "PC " + std::to_string(b.number + 1);
    }
    return {};
}

// What an axis sends, for its chip: "CC 40", "PC 1-16", "VEL +40%", "TR -2", "-".
std::string axisTarget(const AxisBinding& b, const LiveState& live) {
    if (!b.enabled)
        return "-";
    char buf[24];
    switch (b.mode) {
    case AxisMode::Program:
        return "PC " + std::to_string(b.min + 1) + "-" + std::to_string(b.max + 1);
    case AxisMode::Velocity: {
        const float v = b.velocityTarget == VelocityTarget::Loop ? live.velocityLoop : live.velocityShaping;
        if (std::abs(v) < 0.005f)
            return b.velocityTarget == VelocityTarget::Loop ? "VEL LOOP" : "VEL";
        std::snprintf(buf, sizeof buf, "VEL %+d%%", static_cast<int>(std::lround(v * 100.0f)));
        return buf;
    }
    case AxisMode::Transpose: {
        const int steps = b.transposeTarget == TransposeTarget::Loop ? live.transposeLoop : live.transposeNotes;
        if (steps == 0)
            return b.transposeTarget == TransposeTarget::Loop ? "TR LOOP" : "TR";
        std::snprintf(buf, sizeof buf, "TR %+d", steps);
        return buf;
    }
    case AxisMode::Cc:
        break;
    }
    return "CC " + std::to_string(b.cc);
}

// A small loop arrow marking toggle bindings.
void toggleMark(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->PathArcTo(c, r, 0.22f * kPi, 1.83f * kPi, 16);
    dl->PathStroke(col, px(1.6f));
    const ImVec2 tip(c.x + std::cos(1.83f * kPi) * r, c.y + std::sin(1.83f * kPi) * r);
    dl->AddTriangleFilled(ImVec2(tip.x + r * 0.6f, tip.y), ImVec2(tip.x - r * 0.25f, tip.y - r * 0.5f),
                          ImVec2(tip.x - r * 0.25f, tip.y + r * 0.5f), col);
}

void led(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool on) {
    if (on)
        dl->AddCircleFilled(c, r * 1.9f, alpha(col, 0.22f));
    dl->AddCircleFilled(c, r, on ? col : shade(col, 0.35f));
}

// Each channel's colour, on the session and song screens (by channel % 8).
constexpr ImU32 kChannelColors[] = {T::kBlue, T::kPink, T::kGreen, T::kOrange, T::kViolet,
                                    T::kCyan, IM_COL32(0xFF, 0xD6, 0x40, 255), IM_COL32(0xFF, 0x78, 0x5A, 255)};

// A movement track's colour (its module's) and short name, for the badges of a modulation.
ImU32 trackColor(int track) {
    static constexpr ImU32 kColors[] = {T::kBlue, T::kOrange, T::kCyan, T::kOrange, T::kPink, T::kViolet};
    return kColors[std::clamp(track / 2, 0, 5)];
}
std::string trackShortName(int track) {
    static constexpr const char* kNames[] = {"LEFT X", "LEFT Y", "RIGHT X", "RIGHT Y", "F1 X", "F1 Y", "F2 X", "F2 Y"};
    if (track == 8 || track == 9)
        return label(track == 8 ? Button::LeftTrigger : Button::RightTrigger);
    if (track == 10 || track == 11)
        return track == 10 ? "TILT X" : "TILT Y";
    return track >= 0 && track < 8 ? kNames[track] : "";
}

// The phosphor looks' neon, before GuiApp tints it.
constexpr ImU32 kNeon = IM_COL32(0x3C, 0xD8, 0xFF, 255);

// A block of a phosphor look: lines on black, a rule under the title (header 0: none).
void frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float header, ImU32 accent) {
    const ImU32 line = IM_COL32(235, 240, 245, 245), dim = IM_COL32(175, 182, 190, 190);
    const float t = px(1.4f);
    if (gLook == 1) {  // square, a thin outline
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 120), px(2));
        dl->AddRect(a, b, line, px(2), t);
        if (header > 0)
            dl->AddLine(ImVec2(a.x, a.y + header), ImVec2(b.x, a.y + header), dim, t);
        return;
    }
    const float c = px(10);  // chamfered corners
    ImVec2 pts[] = {ImVec2(a.x + c, a.y), ImVec2(b.x - c, a.y), ImVec2(b.x, a.y + c), ImVec2(b.x, b.y - c),
                    ImVec2(b.x - c, b.y), ImVec2(a.x + c, b.y), ImVec2(a.x, b.y - c), ImVec2(a.x, a.y + c)};
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(0, 0, 0, 120));
    dl->AddPolyline(pts, 8, line, t, ImDrawFlags_Closed);
    if (header > 0) {
        dl->AddLine(ImVec2(a.x + px(10), a.y + header), ImVec2(b.x - px(10), a.y + header), dim, t);
        dl->AddLine(ImVec2(a.x + px(10), a.y + header), ImVec2(a.x + px(46), a.y + header), accent, px(2));
    }
}

// A module's colour, from its title: the rule under it in a phosphor look.
ImU32 accentFor(const std::string& title) {
    if (gLook == 1)
        return kNeon;
    return title == "LEFT STICK" ? T::kBlue : title == "RIGHT STICK" ? T::kOrange : title == "TRIGGERS" ? T::kPink
         : title == "TOUCHPAD" ? T::kCyan : title == "TILT" ? T::kViolet : T::kLabel;
}

// A CRT-like screen: dark glass, content, scanlines and a vignette.
template <typename Content>
void screen(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 tint, Content&& content) {
    if (gLook == 0)  // a bezel; a phosphor look outlines the glass instead
        dl->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), IM_COL32(0x12, 0x13, 0x15, 255), px(7));
    else if (gLook == 1)
        dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), alpha(mix(tint, kNeon, 0.5f), 0.45f), px(4), px(1));
    else
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0x3A, 0x3E, 0x45, 255), px(5), px(1));
    dl->AddRectFilledMultiColor(a, b, shade(tint, 0.10f), shade(tint, 0.10f), shade(tint, 0.24f), shade(tint, 0.24f));
    dl->PushClipRect(a, b, true);
    content();
    for (float y = a.y; y < b.y; y += 3.0f)  // one pixel each: a line would spread over three
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y + 1.0f), IM_COL32(0, 0, 0, 55));
    const float v = std::min(b.x - a.x, b.y - a.y) * 0.18f;
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + v), IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 110), 0, 0);
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - v), b, 0, 0, IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 110));
    dl->PopClipRect();
    if (gLook == 0)
        dl->AddRect(a, b, IM_COL32(0, 0, 0, 220), px(4), 1.5f);
}

// A screen off (an empty clip slot): dark grey glass.
constexpr ImU32 kScreenOff = IM_COL32(0x60, 0x66, 0x6E, 255);
constexpr ImU32 kSongScreen = IM_COL32(0x6E, 0x96, 0xC8, 255);  // the song's screens: a steel blue

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
    shape(1.45f, alpha(col, 0.16f), ImVec2(0, 0));
    shape(1.2f, alpha(col, 0.27f), ImVec2(0, 0));
    shape(1.0f, col, ImVec2(0, 0));
    shape(0.55f, alpha(mix(col, IM_COL32_WHITE, 0.6f), 0.86f), ImVec2(-r * 0.15f, -r * 0.18f));
}

// Where a stick was frozen: a still, translucent silhouette.
void ghost(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float stretchX, float stretchY) {
    constexpr int kPoints = 64;
    auto shape = [&](float scale, ImU32 color) {
        ImVec2 pts[kPoints];
        for (int i = 0; i < kPoints; ++i) {
            const float th = static_cast<float>(i) / kPoints * 2.0f * kPi;
            const float wobble = 1.0f + 0.10f * std::sin(3 * th + 1.2f) + 0.06f * std::sin(5 * th - 1.6f) +
                                 0.04f * std::sin(7 * th + 0.6f);
            const float rr = r * scale * wobble;
            pts[i] = ImVec2(c.x + std::cos(th) * rr * stretchX, c.y + std::sin(th) * rr * stretchY);
        }
        dl->AddConcavePolyFilled(pts, kPoints, color);
    };
    shape(1.25f, alpha(col, 0.14f));
    shape(1.08f, alpha(col, 0.34f));
}

float stickPosition(const LiveState& live, Axis a) {
    return std::clamp(static_cast<float>(live.axes[static_cast<std::size_t>(a)]) / 32767.0f, -1.0f, 1.0f);
}

float axisPosition01(const LiveState& live, Axis a) {
    const float raw = static_cast<float>(live.axes[static_cast<std::size_t>(a)]) / 32767.0f;
    return std::clamp(isTrigger(a) ? raw : (raw + 1.0f) * 0.5f, 0.0f, 1.0f);
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

// A module: rounded panel with a header (title and activity LED). Returns the
// y where its content starts.
float panel(ImDrawList* dl, float x, float y, float w, float h, const char* title, bool active) {
    const float header = px(36);
    const float r = px(6);
    if (gLook == 0) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, r);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + header), T::kPanelHeader, r, ImDrawFlags_RoundCornersTop);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, r, px(1));
        text(dl, gBold, px(14), x + px(12), y + header * 0.5f, T::kText, title, H::Left, V::Middle);
    } else {
        frame(dl, ImVec2(x, y), ImVec2(x + w, y + h), header, accentFor(title));
        text(dl, gBold, px(gLook == 1 ? 13.5f : 12.5f), x + px(12), y + header * 0.5f, gLook == 1 ? IM_COL32(0xB8, 0xEC, 0xFF, 255) : T::kLabel, title, H::Left, V::Middle);
    }
    led(dl, ImVec2(x + w - px(16), y + header * 0.5f), px(5), T::kGreen, active);
    if (!active)
        dl->AddCircleFilled(ImVec2(x + w - px(16), y + header * 0.5f), px(5), T::kDim);
    return y + header;
}

// A section title with a rule, e.g. "VALUES ————".
void section(ImDrawList* dl, float x, float w, float y, const std::string& title) {
    const float size = px(11);
    text(dl, gBold, size, x + px(12), y, T::kDim, title, H::Left, V::Middle);
    const float tw = textWidth(gBold, size, title);
    dl->AddLine(ImVec2(x + px(16) + tw, y), ImVec2(x + w - px(12), y), IM_COL32(0x40, 0x44, 0x4B, 255), px(1));
}

// A full-width rounded row: a toggle, a status line...
void row(ImDrawList* dl, float x, float y, float w, float h, ImU32 fill, ImU32 border) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), fill, px(6));
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), border, px(6), px(1.3f));
}

// Edit mode, during a draw: the items the cursor can go to, the one it is
// on, and whether it clicks it this frame. gNoFocus: the next button is not one.
struct FocusEntry {
    std::string id;
    ImVec2 a, b;
};
std::vector<FocusEntry>* gFocusItems = nullptr;
const std::string* gFocusId = nullptr;
bool gFocusClick = false;
bool gNoFocus = false;

void focusFrame(ImVec2 a, ImVec2 b) {
    ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - px(2), a.y - px(2)), ImVec2(b.x + px(2), b.y + px(2)), IM_COL32(0x78, 0xDC, 0xFF, 255), px(7), px(2.5f));
}

// Registers a focus item; true when the edit cursor is on it and clicks it now.
bool focusItem(const std::string& id, ImVec2 a, ImVec2 b) {
    if (!gFocusItems || gNoFocus)
        return false;
    gFocusItems->push_back({id, a, b});
    if (!gFocusId || *gFocusId != id)
        return false;
    focusFrame(a, b);
    return std::exchange(gFocusClick, false);
}

bool hoverButton(const char* id, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton(id, ImVec2(std::max(b.x - a.x, 1.0f), std::max(b.y - a.y, 1.0f)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(255, 255, 255, 70), px(6), px(1));
    }
    const bool focusClicked = focusItem(id, a, b);
    return ImGui::IsItemClicked() || focusClicked;
}

// A padlock `h` high, its left edge at `x`, centred on `y`: the sign of a locked loop.
void drawLock(ImDrawList* dl, float x, float y, float h, ImU32 col) {
    const float w = h * 0.82f, top = y - h * 0.5f, bodyTop = top + h * 0.48f;
    const float stroke = std::max(h * 0.15f, 1.2f), r = w * 0.5f - stroke * 1.1f, cx = x + w * 0.5f;
    const float arcY = top + stroke * 0.5f + r;
    dl->PathLineTo(ImVec2(cx - r, bodyTop + stroke));  // the shackle: up, over, down
    dl->PathArcTo(ImVec2(cx, arcY), r, 3.14159265f, 6.2831853f, 12);
    dl->PathLineTo(ImVec2(cx + r, bodyTop + stroke));
    dl->PathStroke(col, stroke);
    dl->AddRectFilled(ImVec2(x, bodyTop), ImVec2(x + w, top + h), col, h * 0.15f);
    dl->AddCircleFilled(ImVec2(cx, bodyTop + (top + h - bodyTop) * 0.45f), h * 0.1f, IM_COL32(0x20, 0x22, 0x26, 255));
}

// The border of a locked loop's box: solid, in its colour, with a soft halo.
void drawLockedBorder(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float rounding) {
    const float halo = px(2);
    dl->AddRect(ImVec2(a.x - halo, a.y - halo), ImVec2(b.x + halo, b.y + halo), (col & 0x00FFFFFFu) | (60u << 24), rounding + halo, px(3));
    dl->AddRect(a, b, col, rounding, px(2));
}

// --- Idle animations ------------------------------------------------------
// When a LOOP box has nothing to show, a screen in grey plays a little 80s
// arcade game, drawn smooth and glowing rather than in big pixels.
// kIdleArtCount of them; PerformanceView picks one per box, and which boxes play.

constexpr int kIdleArtCount = 10;
constexpr ImU32 kIdleTint = IM_COL32(0x9A, 0xA0, 0xA8, 255), kIdleInk = IM_COL32(0xE6, 0xE8, 0xEC, 255);

// A soft glow under a shape: the shape drawn wider and fainter.
void glowLine(ImDrawList* dl, ImVec2 p1, ImVec2 p2, ImU32 col, float th) {
    dl->AddLine(p1, p2, alpha(col, 0.18f), th * 3.5f);
    dl->AddLine(p1, p2, col, th);
}
void glowCircle(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(c, r * 1.9f, alpha(col, 0.12f));
    dl->AddCircleFilled(c, r, col);
}
void glowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float round) {
    const float g = std::max(std::min(b.x - a.x, b.y - a.y) * 0.45f, 1.5f);
    dl->AddRectFilled(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), alpha(col, 0.12f), round + g);
    dl->AddRectFilled(a, b, col, round);
}
// A sprite of soft dots, like a LED matrix: 'X' lit.
void dots(ImDrawList* dl, std::initializer_list<const char*> rows, float x, float y, float q, ImU32 col) {
    float yy = y;
    for (const char* row : rows) {
        float xx = x;
        for (const char* c = row; *c; ++c, xx += q)
            if (*c == 'X')
                dl->AddRectFilled(ImVec2(xx + q * 0.08f, yy + q * 0.08f), ImVec2(xx + q * 0.92f, yy + q * 0.92f), col, q * 0.3f);
        yy += q;
    }
}
float tri01(float v) { return std::abs(std::fmod(v, 2.0f) - 1.0f); }  // a triangle wave in [0, 1]
float hash01(int k) { return static_cast<float>((k * 2654435761u) % 1000u) / 1000.0f; }

// One game in the box a-b, its shapes in `col`.
void arcade(ImDrawList* dl, int game, ImVec2 a, ImVec2 b, double time, ImU32 col) {
    const auto t = static_cast<float>(time);
    const float w = b.x - a.x, h = b.y - a.y, u = h / 40.0f;  // a unit: a fortieth of the height
    const ImU32 dim = alpha(col, 0.35f), faint = alpha(col, 0.15f);
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    dl->PushClipRect(a, b, true);
    switch (game) {
    case 0: {  // Pac-Man: eating the pellets of a corridor, a ghost behind
        const float y = c.y, span = w + u * 30;
        const float x = a.x - u * 12 + std::fmod(t * u * 9.0f, span);
        dl->AddLine(ImVec2(a.x, y - u * 7), ImVec2(b.x, y - u * 7), faint, u * 0.6f);
        dl->AddLine(ImVec2(a.x, y + u * 7), ImVec2(b.x, y + u * 7), faint, u * 0.6f);
        for (float px_ = a.x + u * 3; px_ < b.x; px_ += u * 5)
            if (px_ > x + u * 2)
                glowCircle(dl, ImVec2(px_, y), u * (std::fmod(px_ - a.x, u * 25) < u * 4 ? 1.3f : 0.6f), col);
        const float open = 0.15f + 0.6f * tri01(t * 6.0f);
        dl->PathLineTo(ImVec2(x, y));
        dl->PathArcTo(ImVec2(x, y), u * 4.5f, open, 6.2832f - open, 24);
        dl->PathFillConvex(col);
        // The ghost: a dome, a waving hem, eyes looking ahead.
        const float gx = x - u * 16;
        dl->PathArcTo(ImVec2(gx, y - u * 0.5f), u * 4.2f, 3.1416f, 6.2832f, 16);
        for (int k = 0; k <= 6; ++k)
            dl->PathLineTo(ImVec2(gx + u * 4.2f - static_cast<float>(k) * u * 1.4f, y + u * 4.2f - (k % 2 ? u * 1.2f : 0) + std::sin(t * 12) * u * 0.3f));
        dl->PathFillConvex(dim);
        for (int e = -1; e <= 1; e += 2) {
            dl->AddCircleFilled(ImVec2(gx + static_cast<float>(e) * u * 1.6f, y - u * 0.8f), u * 1.1f, IM_COL32(255, 255, 255, 230));
            dl->AddCircleFilled(ImVec2(gx + static_cast<float>(e) * u * 1.6f + u * 0.5f, y - u * 0.7f), u * 0.5f, IM_COL32(20, 20, 30, 255));
        }
        break;
    }
    case 1: {  // Space Invaders: a LED formation marching, shields, the cannon
        const bool frame = std::fmod(t, 1.0f) < 0.5f;
        const float q = u * 0.62f, formation = q * (13 * 4 + 11), room = std::max(0.0f, w - formation - u * 2);
        const float left = a.x + u + room * tri01(t * 0.12f), top = a.y + u * 3 + std::fmod(std::floor(t * 0.12f), 4.0f) * u;
        for (int row = 0; row < 3; ++row)
            for (int k = 0; k < 5; ++k) {
                const float ix = left + static_cast<float>(k) * q * 13, iy = top + static_cast<float>(row) * q * 11;
                const ImU32 ic = row == 0 ? col : alpha(col, 0.75f);
                if (frame)
                    dots(dl, {"..X.....X..", "...X...X...", "..XXXXXXX..", ".XX.XXX.XX.", "XXXXXXXXXXX", "X.XXXXXXX.X", "X.X.....X.X", "...XX.XX..."}, ix, iy, q, ic);
                else
                    dots(dl, {"..X.....X..", "X..X...X..X", "X.XXXXXXX.X", "XXX.XXX.XXX", "XXXXXXXXXXX", ".XXXXXXXXX.", "..X.....X..", ".X.......X."}, ix, iy, q, ic);
            }
        for (int s = 0; s < 3; ++s) {  // shields
            const float sx = a.x + w * (0.2f + 0.3f * static_cast<float>(s)), sy = b.y - u * 9;
            dl->AddRectFilled(ImVec2(sx - u * 3, sy), ImVec2(sx + u * 3, sy + u * 3), dim, u * 1.5f, ImDrawFlags_RoundCornersTop);
        }
        const float cx = a.x + u * 3 + (w - u * 6) * tri01(t * 0.2f);
        glowRect(dl, ImVec2(cx - u * 2.5f, b.y - u * 3), ImVec2(cx + u * 2.5f, b.y - u * 1.5f), col, u * 0.5f);
        dl->AddRectFilled(ImVec2(cx - u * 0.6f, b.y - u * 4.5f), ImVec2(cx + u * 0.6f, b.y - u * 3), col, u * 0.3f);
        const float shot = std::fmod(t * 1.3f, 1.0f);
        glowLine(dl, ImVec2(cx, b.y - u * 5 - shot * h * 0.8f), ImVec2(cx, b.y - u * 7 - shot * h * 0.8f), col, u * 0.5f);
        break;
    }
    case 2: {  // Asteroids, in vector lines
        for (int k = 0; k < 4; ++k) {
            const float r = u * (k == 0 ? 6.0f : 3.5f), sp = 0.15f + 0.1f * static_cast<float>(k);
            const ImVec2 rc(a.x + std::fmod(hash01(k * 7) * w + t * u * 3 * (1 + sp), w + r * 2) - r, a.y + std::fmod(hash01(k * 13) * h + t * u * 2 * sp * 4, h + r * 2) - r);
            ImVec2 pts[9];
            for (int i = 0; i < 9; ++i) {
                const float ang = static_cast<float>(i) / 9 * 6.2832f + t * sp;
                const float rr = r * (0.75f + 0.25f * hash01(k * 31 + i));
                pts[i] = ImVec2(rc.x + std::cos(ang) * rr, rc.y + std::sin(ang) * rr);
            }
            dl->AddPolyline(pts, 9, alpha(col, 0.18f), u * 1.6f, ImDrawFlags_Closed);
            dl->AddPolyline(pts, 9, col, u * 0.45f, ImDrawFlags_Closed);
        }
        const float ang = t * 0.9f;
        auto rot = [&](float x, float y) { return ImVec2(c.x + x * std::cos(ang) - y * std::sin(ang), c.y + x * std::sin(ang) + y * std::cos(ang)); };
        const ImVec2 ship[4] = {rot(u * 3.5f, 0), rot(-u * 2.5f, u * 2.2f), rot(-u * 1.5f, 0), rot(-u * 2.5f, -u * 2.2f)};
        dl->AddPolyline(ship, 4, alpha(col, 0.2f), u * 1.8f, ImDrawFlags_Closed);
        dl->AddPolyline(ship, 4, col, u * 0.55f, ImDrawFlags_Closed);
        for (int k = 0; k < 3; ++k) {
            const float d = std::fmod(t * u * 40 + static_cast<float>(k) * u * 12, u * 36) + u * 5;
            glowCircle(dl, rot(d, 0), u * 0.45f, col);
        }
        break;
    }
    case 3: {  // Pong: soft paddles, a ball with its trail, the scores
        for (float y = a.y + u; y < b.y; y += u * 3)
            dl->AddRectFilled(ImVec2(c.x - u * 0.3f, y), ImVec2(c.x + u * 0.3f, y + u * 1.5f), faint, u * 0.3f);
        auto ball = [&](float tt) { return ImVec2(a.x + u * 4 + (w - u * 8) * tri01(tt * 0.45f), a.y + u * 2 + (h - u * 4) * tri01(tt * 0.62f + 0.3f)); };
        for (int k = 8; k >= 1; --k)
            dl->AddCircleFilled(ball(t - static_cast<float>(k) * 0.03f), u * 1.1f * (1 - static_cast<float>(k) * 0.08f), alpha(col, 0.08f * static_cast<float>(9 - k)));
        const ImVec2 bp = ball(t);
        glowCircle(dl, bp, u * 1.2f, col);
        const float ph = u * 9, ly = std::clamp(bp.y - ph * 0.5f, a.y, b.y - ph), ry = std::clamp(a.y + (h - ph) * tri01(t * 0.5f + 0.7f), a.y, b.y - ph);
        glowRect(dl, ImVec2(a.x + u * 1.5f, ly), ImVec2(a.x + u * 2.7f, ly + ph), col, u * 0.6f);
        glowRect(dl, ImVec2(b.x - u * 2.7f, ry), ImVec2(b.x - u * 1.5f, ry + ph), col, u * 0.6f);
        text(dl, gBold, u * 6, c.x - u * 5, a.y + u * 2, dim, "3", H::Right);
        text(dl, gBold, u * 6, c.x + u * 5, a.y + u * 2, dim, "2", H::Left);
        break;
    }
    case 4: {  // Breakout: rounded bricks, the ball's trail, the paddle
        const float bw = (w - u * 4) / 7.0f;
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 7; ++k)
                if ((row * 3 + k + static_cast<int>(t * 0.6f)) % 9 != 4)
                    dl->AddRectFilled(ImVec2(a.x + u * 2 + static_cast<float>(k) * bw + u * 0.4f, a.y + u * 2 + static_cast<float>(row) * u * 3),
                                      ImVec2(a.x + u * 2 + static_cast<float>(k + 1) * bw - u * 0.4f, a.y + u * 4.2f + static_cast<float>(row) * u * 3),
                                      alpha(col, 1.0f - 0.18f * static_cast<float>(row)), u * 0.7f);
        auto ball = [&](float tt) { return ImVec2(a.x + u * 2 + (w - u * 4) * tri01(tt * 0.5f), a.y + u * 15 + (h - u * 20) * tri01(tt * 0.7f)); };
        for (int k = 6; k >= 1; --k)
            dl->AddCircleFilled(ball(t - static_cast<float>(k) * 0.03f), u, alpha(col, 0.07f * static_cast<float>(7 - k)));
        const ImVec2 bp = ball(t);
        glowCircle(dl, bp, u * 1.1f, col);
        const float pw = u * 10, pxl = std::clamp(bp.x - pw * 0.5f, a.x, b.x - pw);
        glowRect(dl, ImVec2(pxl, b.y - u * 2.5f), ImVec2(pxl + pw, b.y - u * 1.2f), col, u * 0.65f);
        break;
    }
    case 5: {  // Missile Command: trails falling on the cities, interceptions blooming
        dl->AddLine(ImVec2(a.x, b.y - u * 2), ImVec2(b.x, b.y - u * 2), dim, u * 0.6f);
        for (int k = 0; k < 5; ++k)
            dl->AddRectFilled(ImVec2(a.x + w * (0.1f + 0.2f * static_cast<float>(k)) - u * 2, b.y - u * 4), ImVec2(a.x + w * (0.1f + 0.2f * static_cast<float>(k)) + u * 2, b.y - u * 2),
                              k == 2 ? col : dim, u * 0.6f, ImDrawFlags_RoundCornersTop);
        for (int k = 0; k < 3; ++k) {
            const float life = std::fmod(t * 0.3f + static_cast<float>(k) * 0.33f, 1.0f);
            const ImVec2 from(a.x + w * hash01(k * 11 + static_cast<int>(t * 0.3f + static_cast<float>(k) * 0.33f)), a.y), to(a.x + w * (0.1f + 0.4f * static_cast<float>(k)), b.y - u * 4);
            const ImVec2 head(from.x + (to.x - from.x) * life * 0.8f, from.y + (to.y - from.y) * life * 0.8f);
            glowLine(dl, from, head, alpha(col, 0.7f), u * 0.35f);
            glowCircle(dl, head, u * 0.5f, IM_COL32(255, 255, 255, 220));
            if (life > 0.55f) {  // the interception: a bloom, growing then fading
                const float k2 = (life - 0.55f) / 0.45f;
                const ImVec2 at(head.x + u * 4, head.y - u * 3);
                dl->AddCircleFilled(at, u * 5 * std::sin(k2 * 3.1416f), alpha(col, 0.22f * (1 - k2)));
                dl->AddCircle(at, u * 5 * std::sin(k2 * 3.1416f), alpha(col, 0.8f * (1 - k2)), 24, u * 0.4f);
            }
        }
        break;
    }
    case 6: {  // Tetris: the well, the stack, a piece falling and turning
        const float cell = u * 3.2f, wx = c.x - cell * 5, wy = b.y - u;
        dl->AddLine(ImVec2(wx - u, a.y), ImVec2(wx - u, wy), dim, u * 0.5f);
        dl->AddLine(ImVec2(wx + cell * 10 + u, a.y), ImVec2(wx + cell * 10 + u, wy), dim, u * 0.5f);
        static constexpr int kStack[4][10] = {{1, 1, 0, 1, 1, 1, 1, 0, 1, 1}, {1, 1, 1, 1, 0, 1, 1, 1, 1, 1}, {0, 1, 1, 0, 0, 0, 1, 1, 0, 1}, {0, 0, 1, 0, 0, 0, 0, 1, 0, 0}};
        auto block = [&](float x, float y, ImU32 bc) { dl->AddRectFilled(ImVec2(x + u * 0.25f, y + u * 0.25f), ImVec2(x + cell - u * 0.25f, y + cell - u * 0.25f), bc, u * 0.6f); };
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 10; ++k)
                if (kStack[r][k])
                    block(wx + static_cast<float>(k) * cell, wy - static_cast<float>(r + 1) * cell, alpha(col, 0.55f + 0.1f * static_cast<float>(r)));
        const float fall = std::fmod(t * 0.35f, 1.0f);
        const int turn = static_cast<int>(t * 0.9f) % 4;
        static constexpr int kT[4][4][2] = {{{0, 0}, {1, 0}, {2, 0}, {1, 1}}, {{1, -1}, {1, 0}, {1, 1}, {0, 0}}, {{0, 1}, {1, 1}, {2, 1}, {1, 0}}, {{1, -1}, {1, 0}, {1, 1}, {2, 0}}};
        const float px0 = wx + cell * 4, py0 = a.y + u + fall * (h - cell * 7);
        for (const auto& bl : kT[turn]) {
            const float bx = px0 + static_cast<float>(bl[0]) * cell, by = py0 + static_cast<float>(bl[1]) * cell;
            dl->AddRectFilled(ImVec2(bx - u * 0.4f, by - u * 0.4f), ImVec2(bx + cell + u * 0.4f, by + cell + u * 0.4f), alpha(col, 0.15f), u);
            block(bx, by, col);
        }
        break;
    }
    case 7: {  // Galaga: a formation, one diving in a loop, the fighter firing
        const float sway = std::sin(t * 0.8f) * u * 3;
        auto bug = [&](ImVec2 p, ImU32 bc, float flap) {
            dl->AddEllipseFilled(ImVec2(p.x - u * 1.6f, p.y), ImVec2(u * 1.4f, u * (0.7f + 0.5f * flap)), alpha(bc, 0.6f));
            dl->AddEllipseFilled(ImVec2(p.x + u * 1.6f, p.y), ImVec2(u * 1.4f, u * (0.7f + 0.5f * flap)), alpha(bc, 0.6f));
            glowCircle(dl, p, u * 1.1f, bc);
        };
        const float flap = tri01(t * 4);
        for (int row = 0; row < 2; ++row)
            for (int k = 0; k < 6; ++k)
                if (!(row == 0 && k == 3))
                    bug(ImVec2(a.x + w * (0.14f + 0.145f * static_cast<float>(k)) + sway, a.y + u * 5 + static_cast<float>(row) * u * 6), row == 0 ? col : dim, flap);
        const float dive = std::fmod(t * 0.4f, 1.0f) * 6.2832f;
        bug(ImVec2(c.x + std::sin(dive) * w * 0.32f, a.y + u * 5 + (1 - std::cos(dive)) * h * 0.3f), col, flap);
        const float fx = c.x + std::sin(t * 0.7f) * w * 0.3f, fy = b.y - u * 4;
        dl->AddTriangleFilled(ImVec2(fx, fy - u * 3), ImVec2(fx - u * 2.5f, fy + u * 1.5f), ImVec2(fx + u * 2.5f, fy + u * 1.5f), col);
        dl->AddCircleFilled(ImVec2(fx, fy), u * 3.5f, alpha(col, 0.1f));
        for (int k = 0; k < 2; ++k) {
            const float sy = fy - u * 4 - std::fmod(t * h * 1.2f + static_cast<float>(k) * h * 0.5f, h);
            glowLine(dl, ImVec2(fx, sy), ImVec2(fx, sy - u * 2), IM_COL32(255, 255, 255, 220), u * 0.4f);
        }
        break;
    }
    case 8: {  // Donkey Kong: slanted girders, ladders, barrels rolling down
        const int floors = 3;
        auto girderY = [&](int f, float x) {
            const float y0 = a.y + u * 9 + static_cast<float>(f) * (h - u * 12) / static_cast<float>(floors - 1);
            const float tilt = (f % 2 ? 1.0f : -1.0f) * u * 2;
            return y0 + tilt * ((x - a.x) / w - 0.5f);
        };
        for (int f = 0; f < floors; ++f) {
            const ImVec2 l(a.x + (f % 2 ? u * 4 : 0), girderY(f, a.x)), r(b.x - (f % 2 ? 0 : u * 4), girderY(f, b.x));
            glowLine(dl, l, r, dim, u * 1.2f);
            for (float x = l.x + u * 2; x < r.x; x += u * 4)
                dl->AddCircleFilled(ImVec2(x, girderY(f, x)), u * 0.35f, col);
            if (f < floors - 1) {
                const float lx = f % 2 ? a.x + w * 0.2f : a.x + w * 0.75f;
                for (float yy = girderY(f, lx) + u; yy < girderY(f + 1, lx) - u; yy += u * 1.5f)
                    dl->AddLine(ImVec2(lx - u * 1.2f, yy), ImVec2(lx + u * 1.2f, yy), faint, u * 0.4f);
            }
        }
        for (int k = 0; k < 3; ++k) {  // a barrel per floor, rolling the floor's way
            const int f = k;
            const float dir = f % 2 ? -1.0f : 1.0f;
            const float prog = std::fmod(t * 0.25f + static_cast<float>(k) * 0.33f, 1.0f);
            const float x = dir > 0 ? a.x + u * 3 + prog * (w - u * 6) : b.x - u * 3 - prog * (w - u * 6);
            const ImVec2 bc(x, girderY(f, x) - u * 2.2f);
            glowCircle(dl, bc, u * 1.8f, col);
            const float ang = prog * 25.0f * dir;
            dl->AddLine(ImVec2(bc.x - std::cos(ang) * u * 1.4f, bc.y - std::sin(ang) * u * 1.4f), ImVec2(bc.x + std::cos(ang) * u * 1.4f, bc.y + std::sin(ang) * u * 1.4f),
                        IM_COL32(20, 20, 30, 200), u * 0.5f);
        }
        dl->AddRectFilled(ImVec2(a.x + u * 2, girderY(0, a.x) - u * 7), ImVec2(a.x + u * 7, girderY(0, a.x) - u * 1.5f), dim, u * 1.2f);  // Kong, at the top
        break;
    }
    default: {  // Q*bert: a pyramid of cubes, a ball hopping, the tops lighting up
        const float s = u * 3.6f;  // half a cube's width
        const ImVec2 top(c.x, a.y + u * 6);
        auto cubeAt = [&](int r, int k) { return ImVec2(top.x + (static_cast<float>(k) - static_cast<float>(r) * 0.5f) * s * 2, top.y + static_cast<float>(r) * s * 1.5f); };
        const int path[6][2] = {{0, 0}, {1, 1}, {2, 1}, {3, 2}, {2, 2}, {1, 0}};
        const float hop = t * 1.4f;
        const int at = static_cast<int>(hop) % 6, next = (at + 1) % 6;
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k <= r; ++k) {
                const ImVec2 p = cubeAt(r, k);
                bool lit = false;
                for (int q = 0; q <= at; ++q)
                    lit = lit || (path[q][0] == r && path[q][1] == k);
                const ImVec2 t0(p.x, p.y - s * 0.5f), t1(p.x + s, p.y), t2(p.x, p.y + s * 0.5f), t3(p.x - s, p.y);
                dl->AddQuadFilled(t0, t1, t2, t3, lit ? col : alpha(col, 0.3f));
                dl->AddQuadFilled(t3, t2, ImVec2(p.x, p.y + s * 1.6f), ImVec2(p.x - s, p.y + s * 1.1f), alpha(col, 0.18f));
                dl->AddQuadFilled(t2, t1, ImVec2(p.x + s, p.y + s * 1.1f), ImVec2(p.x, p.y + s * 1.6f), alpha(col, 0.1f));
            }
        const ImVec2 p0 = cubeAt(path[at][0], path[at][1]), p1 = cubeAt(path[next][0], path[next][1]);
        const float f = hop - std::floor(hop);
        const ImVec2 ball(p0.x + (p1.x - p0.x) * f, p0.y + (p1.y - p0.y) * f - s * 0.9f - std::sin(f * 3.1416f) * u * 4);
        glowCircle(dl, ball, u * 1.6f, IM_COL32(255, 255, 255, 235));
        break;
    }
    }
    dl->PopClipRect();
}

// The idle animation of an empty LOOP box: its game on a grey screen.
// The idle screen of an empty LOOP box: its game on a grey screen, or the
// screen still (art -1).
void idleArt(ImDrawList* dl, int art, ImVec2 a, ImVec2 b, double time) {
    screen(dl, a, b, kIdleTint, [&] {
        if (art >= 0)
            arcade(dl, art % kIdleArtCount, ImVec2(a.x + px(4), a.y + px(4)), ImVec2(b.x - px(4), b.y - px(4)), time, kIdleInk);
    });
}

// Screen geometry shared by the modules.
struct ScreenBox {
    ImVec2 a, b;
};

// Where the LOOP panels start below their module's screen, the same in every
// module so their boxes line up.
float loopOffset() { return px(108); }

// The height the modules' screens are sized from: the whole top row, so every
// module has the same screen whatever its own height.
float gScreenRef = 0;

ScreenBox screenBox(float x, float top, float w, float h) {
    if (gScreenRef > 0)
        h = gScreenRef;
    const float pad = px(10);
    const float sh = std::clamp(h * 0.30f, px(100), (w - 2 * pad) * 0.85f);
    return {ImVec2(x + pad, top + px(10)), ImVec2(x + w - pad, top + px(10) + sh)};
}

// A loop's stretch as shown: "+2.5x" lasts 2.5 times as long, "-2.5x" 2.5 times less.
std::string stretchText(int stretch) {
    if (stretch == 0)
        return "1x";
    const int halves = 2 + std::abs(stretch);  // the factor, in halves
    char buf[16];
    if (halves % 2 == 0)
        std::snprintf(buf, sizeof buf, "%c%dx", stretch > 0 ? '+' : '-', halves / 2);
    else
        std::snprintf(buf, sizeof buf, "%c%d.5x", stretch > 0 ? '+' : '-', halves / 2);
    return buf;
}

// A synced loop's length: "1 BEAT", "3 BEATS", "1 BAR", "2.5 BARS".
std::string beatsText(double beats, int beatsPerBar) {
    char buf[24];
    const double bars = beats / beatsPerBar;
    const bool wholeBeats = std::abs(beats - std::round(beats)) < 1e-6;
    const bool halfBars = std::abs(bars * 2 - std::round(bars * 2)) < 1e-6;
    if (halfBars && bars >= 1.0) {
        if (std::abs(bars - std::round(bars)) < 1e-6)
            std::snprintf(buf, sizeof buf, "%d %s", static_cast<int>(std::round(bars)), std::round(bars) == 1 ? "BAR" : "BARS");
        else
            std::snprintf(buf, sizeof buf, "%.1f BARS", bars);
    } else if (wholeBeats) {
        std::snprintf(buf, sizeof buf, "%d %s", static_cast<int>(std::round(beats)), std::round(beats) == 1 ? "BEAT" : "BEATS");
    } else {
        std::snprintf(buf, sizeof buf, "%.1f BEATS", beats);
    }
    return buf;
}

// The beat grid of a synced note loop's clip over [x0, x1]: a line per beat,
// stronger on bars, and the bars' numbers along the top. Where beats would crowd (a loop
// stretched far), a line per bar, then every 2, 4, 8... bars.
void beatGrid(ImDrawList* dl, ImFont* font, float x0, float x1, float y0, float y1, double beats, int beatsPerBar) {
    if (beats <= 0 || x1 <= x0)
        return;
    const float perBeat = (x1 - x0) / static_cast<float>(beats);
    int step = 1;  // beats between lines
    if (perBeat < px(14)) {
        step = beatsPerBar;
        while (static_cast<float>(step) * perBeat < px(14))
            step *= 2;
    }
    const bool numbers = static_cast<float>(std::max(step, beatsPerBar)) * perBeat > px(22);
    for (int k = 0; k < beats - 1e-6; k += step) {
        const float x = x0 + (x1 - x0) * static_cast<float>(k / beats);
        const bool bar = k % beatsPerBar == 0;
        if (k > 0)
            dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), bar ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 20), px(bar ? 1.2f : 1.0f));
        if (bar && numbers)
            text(dl, font, px(9), x + px(3), y0 + px(6), T::kDim, std::to_string(k / beatsPerBar + 1), H::Left, V::Middle);
    }
}

// A loop's length as shown, stretched: "4.0 s", "12:30 min".
std::string durationText(double seconds) {
    char buf[24];
    if (seconds < 60.0)
        std::snprintf(buf, sizeof buf, "%.1f s", seconds);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d min", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
    return buf;
}

// While Shift is on, top right of a stick's or the touchpad's screen: what
// moving it changes in its loops, their lengths or their amplitudes (Shift +
// its click switches).
void shiftModeBadge(ImDrawList* dl, ImVec2 a, ImVec2 b, Engine::ShapeMode mode, ImU32 color) {
    const char* label = mode == Engine::ShapeMode::Offset ? "OFS" : mode == Engine::ShapeMode::Amplitude ? "AMP" : "LEN";
    const float w = textWidth(gBold, px(11), label) + px(14);
    badge(dl, gBold, px(11), b.x - px(8) - w * 0.5f, a.y + px(8), px(20), label, T::kText, alpha(color, 0.3f), color);
}

}  // namespace

std::string loopBeatsText(double beats, int beatsPerBar) { return beatsText(beats, beatsPerBar); }
std::string loopDurationText(double seconds) { return durationText(seconds); }

void PerformanceView::setLook(Look look) { gLook = static_cast<int>(look); }

PerformanceView::PerformanceView() {
    std::mt19937 random(std::random_device{}());
    std::array<std::uint8_t, kIdleArtCount> arts{};
    for (std::size_t i = 0; i < arts.size(); ++i)
        arts[i] = static_cast<std::uint8_t>(i);
    // Each channel on its own: the games of its boxes (all different), and the
    // two boxes of four that play theirs, never the same pair as the channel before.
    for (std::size_t c = 0; c < idleArt_.size(); ++c) {
        std::shuffle(arts.begin(), arts.end(), random);
        std::copy_n(arts.begin(), idleArt_[c].size(), idleArt_[c].begin());
        std::uint8_t plays = 0;
        do {
            std::array<int, 4> boxes{0, 1, 2, 3};
            std::shuffle(boxes.begin(), boxes.end(), random);
            plays = static_cast<std::uint8_t>((1u << boxes[0]) | (1u << boxes[1]));
        } while (c > 0 && plays == idlePlays_[c - 1]);
        idlePlays_[c] = plays;
    }
}

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

void PerformanceView::menuOnRightClick(const MenuTarget& target, bool recorded, bool locked, const ImVec2* a, const ImVec2* b) {
    if (!ImGui::IsItemHovered() || (a && !ImGui::IsMouseHoveringRect(*a, *b)))
        return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        menuRequest_ = MenuRequest{target, ImGui::GetIO().MousePos};
    if ((recorded || locked) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && ImGui::BeginTooltip()) {
        if (locked)
            ImGui::TextUnformatted("Locked: Record keeps it");
        ImGui::EndTooltip();
    }
}

bool PerformanceView::focusable(const std::string& id, ImVec2 a, ImVec2 b, const MenuTarget& target) {
    if (!gFocusItems)
        return false;
    focusTargets_.push_back({id, target});
    return focusItem(id, a, b);
}

std::optional<PerformanceView::MenuTarget> PerformanceView::focusTarget() const {
    for (const auto& [id, target] : focusTargets_)
        if (id == focus_)
            return target;
    for (const FocusItem& item : focusItems_)
        if (item.id == focus_)
            return MenuTarget{MenuTarget::Kind::Control};  // its window
    return std::nullopt;
}

void PerformanceView::moveFocus(int dx, int dy) {
    const FocusItem* now = nullptr;
    for (const FocusItem& item : focusItems_)
        if (item.id == focus_)
            now = &item;
    if (!now) {
        if (!focusItems_.empty())
            focus_ = focusItems_.front().id;
        return;
    }
    const ImVec2 c((now->a.x + now->b.x) * 0.5f, (now->a.y + now->b.y) * 0.5f);
    const FocusItem* best = nullptr;
    float bestScore = 1e9f;
    for (const FocusItem& item : focusItems_) {
        if (&item == now)
            continue;
        const ImVec2 o((item.a.x + item.b.x) * 0.5f, (item.a.y + item.b.y) * 0.5f);
        const float along = dx != 0 ? (o.x - c.x) * static_cast<float>(dx) : (o.y - c.y) * static_cast<float>(dy);
        if (along <= px(2))
            continue;
        const float across = dx != 0 ? std::abs(o.y - c.y) : std::abs(o.x - c.x);
        const float score = along + across * 2.5f;
        if (score < bestScore) {
            bestScore = score;
            best = &item;
        }
    }
    if (best)
        focus_ = best->id;
}

void PerformanceView::stretchWheel(std::uint16_t tracks, ImVec2 a, ImVec2 b) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool amplitude = io.KeyCtrl && !io.KeyShift;            // Ctrl + wheel: the amplitude
    const bool offset = io.KeyAlt && !io.KeyShift && !io.KeyCtrl;  // Alt + wheel: the offset
    if ((!io.KeyShift && !amplitude && !offset) || !ImGui::IsWindowHovered() || !ImGui::IsMouseHoveringRect(a, b))
        return;
    // Some systems turn Shift + wheel into a horizontal scroll.
    const float wheel = io.MouseWheel != 0.0f ? io.MouseWheel : io.MouseWheelH;
    if (wheel == 0.0f)
        return;
    wheelRest_ += wheel;
    const int notches = static_cast<int>(wheelRest_);
    if (notches == 0)
        return;
    wheelRest_ -= static_cast<float>(notches);
    // Turned quickly, each notch goes further: 100x is 198 steps away.
    wheelStreak_ = time_ - lastWheel_ < 0.12 ? wheelStreak_ + 1 : 0;
    lastWheel_ = time_;
    const int accel = std::min(1 + wheelStreak_ / 2, amplitude || offset ? 4 : 10);
    using K = StretchRequest::Kind;
    stretch_ = StretchRequest{offset ? K::Offset : amplitude ? K::Amplitude : K::Stretch, tracks, notches * accel};
    auto& shown = offset ? offsetShown_ : amplitude ? amplitudeShown_ : stretchShown_;
    for (std::size_t i = 0; i < shown.size(); ++i)
        if (tracks & (1u << i))
            shown[i] = time_;
}

float PerformanceView::drawStretchChip(ImDrawList* dl, float right, float y, int stretch, ImU32 color) {
    return stretch == 0 ? right : drawChip(dl, right, y, stretchText(stretch), color);
}

// While the offset changes (and a moment after), a gauge across the bottom of
// the lane: -100 % to +100 %, 0 marked.
void PerformanceView::drawOffsetGauge(ImDrawList* dl, ImVec2 ga, ImVec2 gb, std::uint16_t tracks, int offset, ImU32 color) {
    double shownAt = -1e9;
    for (std::size_t i = 0; i < offsetShown_.size(); ++i)
        if (tracks & (1u << i))
            shownAt = std::max(shownAt, offsetShown_[i]);
    const double age = time_ - shownAt;
    if (age > 1.6)
        return;
    const float a = age < 1.2 ? 1.0f : static_cast<float>(1.0 - (age - 1.2) / 0.4);
    const ImVec2 ba(ga.x + px(6), gb.y - px(34)), bb(gb.x - px(6), gb.y - px(6));
    dl->AddRectFilled(ba, bb, IM_COL32(0x14, 0x16, 0x19, static_cast<int>(240 * a)), px(5));
    dl->AddRect(ba, bb, alpha(IM_COL32(0x2E, 0x32, 0x38, 255), a), px(5), px(1));
    const float x0 = ba.x + px(12), x1 = bb.x - px(12), ty = bb.y - px(9), ly = ba.y + px(9);
    auto xAt = [&](int pct) { return x0 + (x1 - x0) * static_cast<float>(pct + Engine::kMaxOffset) / static_cast<float>(2 * Engine::kMaxOffset); };
    const float zero = xAt(0), at = xAt(std::clamp(offset, -Engine::kMaxOffset, Engine::kMaxOffset));
    dl->AddLine(ImVec2(x0, ty), ImVec2(x1, ty), alpha(IM_COL32(0x4A, 0x4F, 0x57, 255), a), px(2));
    dl->AddLine(ImVec2(zero, ty - px(4)), ImVec2(zero, ty + px(4)), alpha(T::kDim, a), px(1.5f));
    dl->AddLine(ImVec2(std::min(zero, at), ty), ImVec2(std::max(zero, at), ty), alpha(color, a), px(3));
    dl->AddCircleFilled(ImVec2(at, ty), px(4.5f), alpha(color, a));
    const std::string value = std::string("OFS ") + (offset > 0 ? "+" : "") + std::to_string(offset) + "%";
    const float vw = textWidth(gBold, px(10), value);
    const float vx = std::clamp(at, x0 + vw * 0.5f, x1 - vw * 0.5f);
    auto fixed = [&](float x, const char* label, H align) {
        const float w = textWidth(gMedium, px(10), label);
        const float l = align == H::Left ? x : align == H::Right ? x - w : x - w * 0.5f;
        if (l + w < vx - vw * 0.5f - px(4) || l > vx + vw * 0.5f + px(4))
            text(dl, gMedium, px(10), x, ly, alpha(T::kDim, a), label, align, V::Middle);
    };
    fixed(x0 - px(6), "-100%", H::Left);
    if (offset != 0)
        fixed(zero, "0", H::Center);
    fixed(x1 + px(6), "+100%", H::Right);
    text(dl, gBold, px(10), vx, ly, alpha(T::kText, a), value, H::Center, V::Middle);
}

float PerformanceView::drawAmplitudeChip(ImDrawList* dl, float right, float y, int amplitude, ImU32 color) {
    return amplitude == 100 ? right : drawChip(dl, right, y, "AMP " + std::to_string(amplitude) + "%", color);
}

float PerformanceView::drawChip(ImDrawList* dl, float right, float y, const std::string& label, ImU32 color) {
    const float w = textWidth(gBold, px(10), label) + px(10);
    const ImVec2 a(right - w, y - px(7)), b(right, y + px(7));
    dl->AddRectFilled(a, b, alpha(color, 0.2f), px(7));
    dl->AddRect(a, b, alpha(color, 0.8f), px(7), px(1));
    text(dl, gBold, px(10), (a.x + b.x) * 0.5f, y, T::kText, label, H::Center, V::Middle);
    return a.x;
}

// While the amplitude changes (and a moment after), a gauge across the bottom
// of the LOOP box, like the stretch's: 0 % on the left, 100 % (as recorded)
// marked, up to 800 % on the right.
void PerformanceView::drawAmplitudeGauge(ImDrawList* dl, ImVec2 ga, ImVec2 gb, std::uint16_t tracks, int amplitude, ImU32 color) {
    double shownAt = -1e9;
    for (std::size_t i = 0; i < amplitudeShown_.size(); ++i)
        if (tracks & (1u << i))
            shownAt = std::max(shownAt, amplitudeShown_[i]);
    const double age = time_ - shownAt;
    if (age > 1.6)
        return;
    const float a = age < 1.2 ? 1.0f : static_cast<float>(1.0 - (age - 1.2) / 0.4);
    const ImVec2 ba(ga.x + px(6), gb.y - px(34)), bb(gb.x - px(6), gb.y - px(6));
    dl->AddRectFilled(ba, bb, IM_COL32(0x14, 0x16, 0x19, static_cast<int>(240 * a)), px(5));
    dl->AddRect(ba, bb, alpha(IM_COL32(0x2E, 0x32, 0x38, 255), a), px(5), px(1));
    const float x0 = ba.x + px(12), x1 = bb.x - px(12), ty = bb.y - px(9), ly = ba.y + px(9);
    // 0-100 % on the first third, 100-800 % on the rest.
    auto xAt = [&](int pct) {
        const float f = pct <= 100 ? 0.35f * static_cast<float>(pct) / 100.0f
                                   : 0.35f + 0.65f * static_cast<float>(pct - 100) / static_cast<float>(Engine::kMaxAmplitude - 100);
        return x0 + (x1 - x0) * f;
    };
    const float one = xAt(100), at = xAt(std::clamp(amplitude, 0, Engine::kMaxAmplitude));
    dl->AddLine(ImVec2(x0, ty), ImVec2(x1, ty), alpha(IM_COL32(0x4A, 0x4F, 0x57, 255), a), px(2));
    dl->AddLine(ImVec2(one, ty - px(4)), ImVec2(one, ty + px(4)), alpha(T::kDim, a), px(1.5f));
    dl->AddLine(ImVec2(std::min(one, at), ty), ImVec2(std::max(one, at), ty), alpha(color, a), px(3));  // away from as recorded
    dl->AddCircleFilled(ImVec2(at, ty), px(4.5f), alpha(color, a));
    const std::string value = "AMP " + std::to_string(amplitude) + "%";
    const float vw = textWidth(gBold, px(10), value);
    const float vx = std::clamp(at, x0 + vw * 0.5f, x1 - vw * 0.5f);
    auto fixed = [&](float x, const char* label, H align) {
        const float w = textWidth(gMedium, px(10), label);
        const float l = align == H::Left ? x : align == H::Right ? x - w : x - w * 0.5f;
        if (l + w < vx - vw * 0.5f - px(4) || l > vx + vw * 0.5f + px(4))
            text(dl, gMedium, px(10), x, ly, alpha(T::kDim, a), label, align, V::Middle);
    };
    fixed(x0 - px(6), "0%", H::Left);
    if (amplitude != 100)
        fixed(one, "100%", H::Center);
    fixed(x1 + px(6), "800%", H::Right);
    text(dl, gBold, px(10), vx, ly, alpha(T::kText, a), value, H::Center, V::Middle);
}

// While the wheel turns (and a moment after), a gauge across the bottom of the
// LOOP box: -20x on the left, 1x, +100x on the right, on a log scale.
void PerformanceView::drawStretchGauge(ImDrawList* dl, ImVec2 ga, ImVec2 gb, std::uint16_t tracks, int stretch, ImU32 color) {
    double shownAt = -1e9;
    for (std::size_t i = 0; i < stretchShown_.size(); ++i)
        if (tracks & (1u << i))
            shownAt = std::max(shownAt, stretchShown_[i]);
    const double age = time_ - shownAt;
    if (age > 1.6)
        return;
    const float a = age < 1.2 ? 1.0f : static_cast<float>(1.0 - (age - 1.2) / 0.4);
    const ImVec2 ba(ga.x + px(6), gb.y - px(34)), bb(gb.x - px(6), gb.y - px(6));
    dl->AddRectFilled(ba, bb, IM_COL32(0x14, 0x16, 0x19, static_cast<int>(240 * a)), px(5));
    dl->AddRect(ba, bb, alpha(IM_COL32(0x2E, 0x32, 0x38, 255), a), px(5), px(1));
    const float x0 = ba.x + px(12), x1 = bb.x - px(12), ty = bb.y - px(9), ly = ba.y + px(9);
    const double kLeft = std::log(20.0), kSpan = std::log(20.0) + std::log(100.0);
    auto xAt = [&](double factor) { return x0 + (x1 - x0) * static_cast<float>((std::log(factor) + kLeft) / kSpan); };
    const float one = xAt(1.0), at = xAt(Engine::stretchFactor(stretch));
    dl->AddLine(ImVec2(x0, ty), ImVec2(x1, ty), alpha(IM_COL32(0x4A, 0x4F, 0x57, 255), a), px(2));
    dl->AddLine(ImVec2(one, ty - px(4)), ImVec2(one, ty + px(4)), alpha(T::kDim, a), px(1.5f));
    dl->AddLine(ImVec2(std::min(one, at), ty), ImVec2(std::max(one, at), ty), alpha(color, a), px(3));
    dl->AddCircleFilled(ImVec2(at, ty), px(4.5f), alpha(color, a));
    // The value above the marker; the fixed labels where it leaves room.
    const std::string value = stretchText(stretch);
    const float vw = textWidth(gBold, px(10), value);
    const float vx = std::clamp(at, x0 + vw * 0.5f, x1 - vw * 0.5f);
    auto fixed = [&](float x, const char* label, H align) {
        const float w = textWidth(gMedium, px(10), label);
        const float l = align == H::Left ? x : align == H::Right ? x - w : x - w * 0.5f;
        if (l + w < vx - vw * 0.5f - px(4) || l > vx + vw * 0.5f + px(4))
            text(dl, gMedium, px(10), x, ly, alpha(T::kDim, a), label, align, V::Middle);
    };
    fixed(x0 - px(6), "-20x", H::Left);
    if (stretch != 0)
        fixed(one, "1x", H::Center);
    fixed(x1 + px(6), "+100x", H::Right);
    text(dl, gBold, px(10), vx, ly, alpha(T::kText, a), value, H::Center, V::Middle);
}

std::optional<std::string> PerformanceView::draw(const Mapping& normalMapping, const LiveState& live,
                                                 const std::optional<Key>& key) {
    // While Shift is on, the pads, D-pad and touchpad pads show their Shift layer.
    Mapping shifted;
    if (live.stretchHeld) {
        shifted = normalMapping;
        auto overlay = [](TriggerBinding& shown, const TriggerBinding& layer) {
            if (shown.type != ActionType::Shift && layer.isSet() && layer.type != ActionType::Shift)
                shown = layer;
        };
        for (std::size_t i = 0; i < shifted.buttons.size(); ++i)
            overlay(shifted.buttons[i], normalMapping.shift.buttons[i]);
        for (std::size_t i = 0; i < shifted.dpad.size(); ++i)
            overlay(shifted.dpad[i], normalMapping.shift.dpad[i]);
        for (std::size_t f = 0; f < shifted.fingers.size(); ++f)
            for (std::size_t z = 0; z < shifted.fingers[f].zones.size(); ++z)
                overlay(shifted.fingers[f].zones[z], normalMapping.shift.zones[f][z]);
    }
    const Mapping& mapping = live.stretchHeld ? shifted : normalMapping;
    gMedium = fonts_.medium ? fonts_.medium : ImGui::GetFont();
    gBold = fonts_.bold ? fonts_.bold : gMedium;
    gCondensed = fonts_.condensed ? fonts_.condensed : gBold;
    time_ = ImGui::GetTime();
    dt_ = ImGui::GetIO().DeltaTime;
    flats_ = key && prefersFlats(*key);
    clicked_.reset();
    stretch_.reset();
    key_ = key;
    // Edit mode: gather the items the cursor goes to; it clicks the one it is on.
    focusItems_.clear();
    focusTargets_.clear();
    std::vector<FocusEntry> gathered;
    if (editMode_) {
        gFocusItems = &gathered;
        gFocusId = &focus_;
        gFocusClick = std::exchange(focusClick_, false);
    }
    beatsPerBar_ = std::max(live.beatsPerBar, 1);
    trackFrozenPositions(live);
    // A stretch that changed, from the wheel or the controller, shows its gauge.
    for (int track = 0; track < Engine::kNoteTrack; ++track) {
        const auto t = static_cast<std::size_t>(track);
        const LoopStatus& ls = live.loops[static_cast<std::size_t>(Engine::sourceOfTrack(track))].lanes[static_cast<std::size_t>(Engine::axisOfTrack(track))].status;
        if (ls.stretch != lastStretch_[t] && live.channel == lastStretchChannel_)
            stretchShown_[t] = time_;
        lastStretch_[t] = ls.stretch;
        if (ls.amplitude != lastAmplitude_[t] && live.channel == lastStretchChannel_)
            amplitudeShown_[t] = time_;
        lastAmplitude_[t] = ls.amplitude;
        if (ls.offset != lastOffset_[t] && live.channel == lastStretchChannel_)
            offsetShown_[t] = time_;
        lastOffset_[t] = ls.offset;
    }
    lastStretchChannel_ = live.channel;

    // Flash the buttons pressed since the last frame.
    const std::uint32_t pressed = live.buttons & ~previousButtons_;
    for (int b = 0; b < 32; ++b)
        if ((pressed >> b) & 1u)
            lastPress_[static_cast<std::size_t>(b)] = time_;
    previousButtons_ = live.buttons;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = px(8);
    const float stripH = px(114);
    const float topH = avail.y - stripH - gap;
    gScreenRef = topH;
    const float colW = (avail.x - 4 * gap) / 5.0f;

    // The modules, all as tall; below them the piano roll (under the sticks
    // and triggers), the scale and the channel.
    const float lowH = std::clamp(topH * 0.23f, px(120), px(150));
    const float moduleH = topH - lowH - gap;
    const float lowY = origin.y + moduleH + gap;
    float x = origin.x;
    // A module the controller has nothing for is off: dimmed under its title, its settings kept.
    auto off = [&](float mx, bool absent) {
        if (absent)
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mx + px(1), origin.y + px(37)), ImVec2(mx + colW - px(1), origin.y + moduleH - px(1)),
                                                      gLook ? IM_COL32(0, 0, 0, 205) : IM_COL32(0x2E, 0x31, 0x36, 225), px(6));
    };
    drawStick(0, mapping, live, x, origin.y, colW, moduleH);
    off(x, live.connected && !live.has(Axis::LeftX) && !live.has(Axis::LeftY));
    x += colW + gap;
    drawStick(1, mapping, live, x, origin.y, colW, moduleH);
    off(x, live.connected && !live.has(Axis::RightX) && !live.has(Axis::RightY));
    x += colW + gap;
    drawTriggers(mapping, live, x, origin.y, colW, moduleH);
    off(x, live.connected && !live.has(Axis::LeftTrigger) && !live.has(Axis::RightTrigger));
    drawPianoRoll(mapping, live, origin.x, lowY, colW * 3 + gap * 2, lowH);
    x += colW + gap;
    drawDpad(mapping, live, x, origin.y, colW, moduleH);
    drawScale(key, x, lowY, colW, lowH);
    x += colW + gap;
    // The tilt, with motion sensors: in place of a touchpad the controller has
    // not, else beside it, under a tab.
    const bool touchpad = !live.connected || live.hasTouchpad || live.touchpadExpected;
    const bool tilt = live.connected && live.hasMotion;
    tabbed_ = tilt && touchpad;
    if (tilt && (!touchpad || tiltTab_)) {
        drawTilt(mapping, live, x, origin.y, colW, moduleH);
    } else {
        drawTouchpad(mapping, live, x, origin.y, colW, moduleH);
        off(x, live.connected && !live.hasTouchpad && !live.touchpadExpected);  // a DualSense's may only lack the access
    }
    drawChannel(live, x, lowY, colW, lowH);
    drawButtons(mapping, live, origin.x, origin.y + topH + gap, avail.x, stripH);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y));
    ImGui::Dummy(ImVec2(0, 0));
    if (editMode_) {
        gFocusItems = nullptr;
        gFocusId = nullptr;
        gFocusClick = false;
        for (const FocusEntry& e : gathered)
            focusItems_.push_back({e.id, e.a, e.b});
        const bool known = std::any_of(focusItems_.begin(), focusItems_.end(), [&](const FocusItem& i) { return i.id == focus_; });
        if (!known && !focusItems_.empty())  // a cursor to start with: the top-left item
            focus_ = std::min_element(focusItems_.begin(), focusItems_.end(), [](const FocusItem& l, const FocusItem& r) {
                         return l.a.y + l.a.x * 0.2f < r.a.y + r.a.x * 0.2f;
                     })->id;
    } else {
        focusTargets_.clear();
    }
    return clicked_;
}

void PerformanceView::addNote(int channel, int note, int velocity, bool fromLoop, double time) {
    if (velocity > 0) {
        rollNotes_.push_back({time, time, static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(note),
                              static_cast<std::uint8_t>(velocity), fromLoop, true});
    } else {
        for (auto it = rollNotes_.rbegin(); it != rollNotes_.rend(); ++it) {
            if (it->sounding && it->channel == channel && it->note == note) {
                it->end = time;
                it->sounding = false;
                break;
            }
        }
    }
    // Keep what the roll can still show.
    while (rollNotes_.size() > 1024 || (!rollNotes_.empty() && !rollNotes_.front().sounding && rollNotes_.front().end < time - 8.0))
        rollNotes_.pop_front();
}

// The notes and chords sent on the active channel. Without a note loop, the
// last seconds scroll to a "now" line at the right edge. With one, the roll
// is the loop itself, still, a playhead sweeping it: its notes light up as it
// crosses them, and a note added in overdub sits where it lands.
void PerformanceView::drawPianoRoll(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr auto kNotes = static_cast<std::size_t>(LoopSource::Notes);
    const LoopStatus& st = live.loops[kNotes].status;
    const LoopBinding& lb = m.loops[kNotes];
    const bool recording = st.state == LoopStatus::State::Recording;
    const bool clip = st.state == LoopStatus::State::Playing && st.length > 1;
    const ImU32 kViolet = T::kViolet;
    const ImU32 kMauve = IM_COL32(0xC4, 0x7C, 0xFF, 255);  // selected notes, in the editor
    const ImU32 kLive = IM_COL32(0xEB, 0xEE, 0xF2, 255);
    // No title bar: the roll fills its block, its state shows top right.
    if (gLook) {
        frame(dl, ImVec2(x, y), ImVec2(x + w, y + h), 0, kNeon);
    } else {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, px(8));
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, px(8), px(1));
    }

    char seconds[16];
    std::snprintf(seconds, sizeof seconds, "%.1f s", static_cast<double>(st.length) / Engine::kLoopRateHz);
    std::string status;
    ImU32 statusColor = T::kDim;
    if (recording) {
        status = std::string("REC  ") + seconds;
        statusColor = T::kRed;
    } else if (st.waiting) {
        status = "REC";
        statusColor = T::kRed;
    } else if (clip) {
        status = std::string(st.overdub ? "OVERDUB  " : "LOOP  ") +
                 (lb.playback == LoopPlayback::Forward ? "FWD" : lb.playback == LoopPlayback::Reverse ? "REV" : "B&F") +
                 "  \xC2\xB7  " + (st.beats > 0 ? beatsText(st.beats, beatsPerBar_) : std::string(seconds));
        statusColor = st.overdub ? T::kOrange : kViolet;
    }

    // Geometry: keyboard, then the roll.
    const float ax0 = x + px(12), ay0 = y + px(10), ax1 = x + w - px(12), ay1 = y + h - px(10);
    if (ay1 - ay0 < px(30))
        return;
    const float keyW = px(20), gx0 = ax0 + keyW + px(16);
    constexpr double kWindow = 6.0;  // seconds shown while scrolling
    const float pxPerSec = (ax1 - gx0) / static_cast<float>(kWindow);
    auto xAtTime = [&](double t) { return ax1 + static_cast<float>(t - time_) * pxPerSec; };  // scrolling
    // The loop's end dragged in the editor: its new length so far, whole beats
    // for a synced loop. Longer, the roll shows it all, the loop narrowing.
    // (The main view draws its roll too, every frame: it leaves the editor's drag alone.)
    if (editing_ && !clip)
        lengthDrag_.reset();
    int newLength = st.length;
    double newBeats = st.beats;
    if (lengthDrag_ && editing_) {
        LengthDrag& d = *lengthDrag_;
        const double perPx = static_cast<double>(d.length) / std::max((ax1 - gx0) * viewZoom_, 1.0f);
        double wanted = d.length + (ImGui::GetIO().MousePos.x - d.grabX) * perPx;
        if (d.beats > 0) {
            const double perBeat = d.length / d.beats;
            newBeats = std::clamp(std::round(wanted / perBeat), 1.0, std::max(1.0, std::floor(Engine::kMaxLoopSamples / perBeat)));
            wanted = newBeats * perBeat;
        }
        d.newLength = newLength = std::clamp(static_cast<int>(std::lround(wanted)), Engine::kLoopRateHz / 10, Engine::kMaxLoopSamples);
        std::snprintf(seconds, sizeof seconds, "%.1f s", static_cast<double>(newLength) / Engine::kLoopRateHz);
        status = std::string("LOOP  ") + (lb.playback == LoopPlayback::Forward ? "FWD" : lb.playback == LoopPlayback::Reverse ? "REV" : "B&F") +
                 "  \xC2\xB7  " + (newBeats > 0 ? beatsText(newBeats, beatsPerBar_) : std::string(seconds));
    }
    const float shownLength = static_cast<float>(std::max(newLength, st.length));
    // The editor's view: zoomed `viewZoom_` times, from `viewStart_` (a fraction of the loop).
    if (!editing_ || !clip) {
        if (editing_) {
            viewZoom_ = 1;
            viewStart_ = 0;
        }
    }
    const float zoom = editing_ ? viewZoom_ : 1.0f, viewFrom = editing_ ? viewStart_ : 0.0f;
    auto xAtSample = [&](float sample) { return gx0 + (ax1 - gx0) * (sample / shownLength - viewFrom) * zoom; };  // clip
    auto sampleAtX = [&](float sx) { return ((sx - gx0) / std::max(ax1 - gx0, 1.0f) / zoom + viewFrom) * shownLength; };
    if (editing_) {
        rollGx0_ = gx0;
        rollAx1_ = ax1;
        editorClip_ = clip;
    }

    // The range: the notes in view, two octaves at least, eased.
    int lo = 127, hi = 0;
    for (const RollNote& n : rollNotes_)
        if (n.channel == live.channel && (n.sounding || n.end > time_ - kWindow)) {
            lo = std::min<int>(lo, n.note);
            hi = std::max<int>(hi, n.note);
        }
    if (clip || recording)
        for (int i = 0; i < live.noteLoopCount; ++i) {
            lo = std::min<int>(lo, live.noteLoop[static_cast<std::size_t>(i)].note);
            hi = std::max<int>(hi, live.noteLoop[static_cast<std::size_t>(i)].note);
        }
    if (lo > hi) {
        lo = 48;
        hi = 72;
    }
    if (hi - lo < 24) {
        const int c = (lo + hi) / 2;
        lo = c - 12;
        hi = c + 12;
    }
    const float low = smooth(62, static_cast<float>(lo - 1), 6.0f), high = smooth(63, static_cast<float>(hi + 1), 6.0f);
    const float rowH = (ay1 - ay0) / std::max(high - low, 1.0f);
    auto yOf = [&](int note) { return ay1 - (static_cast<float>(note) - low) * rowH; };  // bottom of its row

    dl->PushClipRect(ImVec2(ax0, ay0), ImVec2(ax1, ay1), true);
    dl->AddRectFilled(ImVec2(gx0 - px(2), ay0), ImVec2(ax1, ay1), kGraph, px(3));
    for (int n = static_cast<int>(std::floor(low)); n <= static_cast<int>(std::ceil(high)); ++n) {
        const float yb = yOf(n), yt = yb - rowH;
        const bool black = n % 12 == 1 || n % 12 == 3 || n % 12 == 6 || n % 12 == 8 || n % 12 == 10;
        dl->AddRectFilled(ImVec2(ax0, yt), ImVec2(ax0 + keyW, yb - 0.5f), black ? IM_COL32(0x2A, 0x2C, 0x30, 255) : IM_COL32(0xCD, 0xD0, 0xD6, 255));
        if (black)
            dl->AddRectFilled(ImVec2(gx0, yt), ImVec2(ax1, yb), IM_COL32(0, 0, 0, 40));
        if (editing_ && !scaleMoves_ && rowH >= px(7))  // chromatic: each row named
            text(dl, gMedium, std::min(px(8), rowH * 0.9f), ax0 + keyW - px(2), yb - rowH * 0.5f, black ? T::kLabel : IM_COL32(0x30, 0x33, 0x38, 255),
                 noteName(n, false), H::Right, V::Middle);
        if (editing_ && scaleMoves_ && key_) {  // where notes may go: the scale's rows, its roots tinted
            if (!inKey(*key_, n))
                dl->AddRectFilled(ImVec2(gx0, yt), ImVec2(ax1, yb), IM_COL32(0, 0, 0, 95));
            else if (((n - key_->root) % 12 + 12) % 12 == 0)
                dl->AddRectFilled(ImVec2(gx0, yt), ImVec2(ax1, yb), IM_COL32(120, 170, 255, 22));
        }
        if (n % 12 == 0) {
            dl->AddLine(ImVec2(gx0, yb), ImVec2(ax1, yb), IM_COL32(255, 255, 255, 30));
            text(dl, gBold, px(8), ax0 + keyW + px(2), yb - rowH * 0.5f, T::kDim, "C" + std::to_string(n / 12 - 1), H::Left, V::Middle);
        }
    }

    bool any = false;
    int noteHovered = -1;  // the loop note under the mouse
    bool noteEndHovered = false;  // on the end of a selected note: its length
    if (clip) {
        // Synced: the beats and bars of the loop. Free: half-second lines. From the loop's start, which is marked.
        dl->PushClipRect(ImVec2(gx0 - px(2), ay0), ImVec2(ax1, ay1), true);  // zoomed, nothing over the keyboard
        if (st.beats > 0) {
            beatGrid(dl, gMedium, xAtSample(0), xAtSample(shownLength), ay0, ay1, newLength > st.length ? newBeats : st.beats, beatsPerBar_);
        } else {
            const float halfSecond = static_cast<float>(Engine::kLoopRateHz) * 0.5f;
            for (float s2 = halfSecond; s2 < shownLength; s2 += halfSecond)
                dl->AddLine(ImVec2(xAtSample(s2), ay0), ImVec2(xAtSample(s2), ay1), IM_COL32(255, 255, 255, 18));
        }
        dl->AddLine(ImVec2(xAtSample(0), ay0), ImVec2(xAtSample(0), ay1), IM_COL32(255, 255, 255, 90), px(1.2f));
        text(dl, gMedium, px(8), xAtSample(0) + px(3), ay1 - px(6), T::kDim, "start", H::Left, V::Middle);
        // Beneath the loop, what is played live scrolls by as without a loop, from
        // "now" at the right edge: faint cyan, so the loop stays in front.
        bool liveShown = false;
        for (const RollNote& n : rollNotes_) {
            if (n.channel != live.channel || n.fromLoop)
                continue;
            const double end = n.sounding ? time_ : n.end;
            if (end < time_ - kWindow)
                continue;
            liveShown = true;
            const float yb = yOf(n.note);
            const float a = 0.14f + 0.08f * static_cast<float>(n.velocity) / 127.0f;
            dl->AddRectFilled(ImVec2(std::max(xAtTime(n.start), gx0), yb - rowH + 0.5f),
                              ImVec2(noteBarEnd(xAtTime(n.start), xAtTime(end), !n.sounding), yb - 0.5f), alpha(T::kCyan, a), px(1));
        }
        if (liveShown)
            dl->AddLine(ImVec2(ax1 - px(1), ay0), ImVec2(ax1 - px(1), ay1), alpha(T::kCyan, 0.3f), px(1.2f));
        // Notes being moved: where they go so far, the group as a block inside
        // the loop and the MIDI range, its first note on the quantize grid.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float sampleAt = sampleAtX(mouse.x);
        const bool byDegrees = scaleMoves_ && key_.has_value();
        if (drag_ && editing_ && drag_->resize) {
            // Lengths: every selected note alike, the grabbed end onto the
            // quantize grid; each keeps a sample, and ends inside the loop.
            double moved = static_cast<double>(sampleAt - drag_->grabSample);
            if (quantize_ > 0 && st.beats > 0) {
                const double step = static_cast<double>(st.length) / (st.beats * quantize_);
                moved = std::max(step, std::round((drag_->grabEnd + moved) / step) * step) - drag_->grabEnd;
            }
            int shortest = -(1 << 30), longest = 1 << 30;
            for (const NoteInterval& n : drag_->notes) {
                shortest = std::max(shortest, 1 - std::max(static_cast<int>(n.end - n.start), 1));
                longest = std::min(longest, st.length - 1 - static_cast<int>(n.end));
            }
            drag_->samples = std::clamp(static_cast<int>(std::lround(moved)), shortest, std::max(longest, shortest));
        } else if (drag_ && editing_) {
            int first = st.length, last = 0, lowest = 127, highest = 0;
            for (const NoteInterval& n : drag_->notes) {
                first = std::min(first, static_cast<int>(n.start));
                last = std::max(last, std::max(static_cast<int>(n.end), static_cast<int>(n.start) + 1));
                lowest = std::min<int>(lowest, n.note);
                highest = std::max<int>(highest, n.note);
            }
            double moved = static_cast<double>(sampleAt - drag_->grabSample);
            if (quantize_ > 0 && st.beats > 0) {
                const double step = static_cast<double>(st.length) / (st.beats * quantize_);
                moved = std::round((first + moved) / step) * step - first;
            }
            drag_->samples = std::clamp(static_cast<int>(std::lround(moved)), -first, std::max(st.length - 1 - last, -first));
            const int rows = static_cast<int>(std::lround((drag_->grabY - mouse.y) / rowH));
            drag_->semitones = byDegrees ? rows : std::clamp(rows, -lowest, 127 - highest);  // degrees, or semitones
        }
        auto movedNote = [&](int note) {
            return byDegrees ? Engine::transposedIn(key_, note, drag_->semitones) : std::clamp(note + drag_->semitones, 0, 127);
        };
        // The loop's notes: white, as opaque as they are loud; a halo while
        // the playhead crosses them; orange while being added in overdub (from
        // their start to the playhead). In the editor, selected notes are
        // mauve, as opaque as they are loud, outlined in cyan, and the halo is
        // white; notes being moved are faint.
        const auto pos = static_cast<float>(st.position);
        int hovered = -1;
        auto moving = [&](const NoteInterval& n) {
            if (!drag_ || !editing_)
                return false;
            for (const NoteInterval& other : drag_->notes)
                if (other.start == n.start && other.note == n.note)
                    return true;
            return false;
        };
        for (int i = 0; i < live.noteLoopCount; ++i) {
            const NoteInterval& n = live.noteLoop[static_cast<std::size_t>(i)];
            const float yb = yOf(n.note);
            if (n.open && st.overdub) {
                const float xa = xAtSample(static_cast<float>(n.start));
                const float xb = pos >= static_cast<float>(n.start) ? xAtSample(pos) : ax1;  // wraps past the end
                dl->AddRectFilled(ImVec2(xa, yb - rowH + 0.5f), ImVec2(std::max(xb, xa + px(2)), yb - 0.5f), T::kOrange, px(1));
                continue;
            }
            const bool dragged = moving(n) && !drag_->resize;
            const bool lit = pos >= static_cast<float>(n.start) && pos < static_cast<float>(n.end);
            const ImVec2 na(xAtSample(static_cast<float>(n.start)), yb - rowH + 0.5f);
            const ImVec2 nb(std::max(xAtSample(static_cast<float>(n.end)), na.x + px(2)), yb - 0.5f);
            if (mouse.x >= na.x - px(2) && mouse.x <= std::max(nb.x, na.x + px(4)) + px(2) && mouse.y >= na.y - 1 && mouse.y <= nb.y + 1)
                hovered = i;
            const bool chosen = editing_ && selected(n);
            if (chosen && hovered == i && mouse.x >= std::max(nb.x, na.x + px(4)) - px(6))
                noteEndHovered = true;
            if (lit && !dragged)
                dl->AddRectFilled(ImVec2(na.x - px(2), na.y - px(2)), ImVec2(nb.x + px(2), nb.y + px(2)),
                                  editing_ ? IM_COL32(255, 255, 255, 70) : alpha(kViolet, 0.6f), px(3));
            const bool cut = n.start >= newLength;  // past the loop's new end: it goes
            const ImU32 fill = alpha(cut ? IM_COL32(0xFF, 0x5C, 0x5C, 255) : chosen ? kMauve : IM_COL32_WHITE,
                                     (cut ? 0.75f : 0.1f) + (cut ? 0.25f : 0.9f) * static_cast<float>(n.velocity) / 127.0f);
            dl->AddRectFilled(na, ImVec2(noteBarEnd(na.x, nb.x, true), nb.y), dragged ? alpha(IM_COL32_WHITE, 0.12f) : fill, px(1));
            if (!dragged && chosen)
                dl->AddRect(ImVec2(na.x - 1, na.y - 1), ImVec2(nb.x + 1, nb.y + 1), T::kCyan, px(1), px(1.5f));
        }
        if (drag_ && editing_ && drag_->resize) {  // their new ends
            for (const NoteInterval& n : drag_->notes) {
                const float yb = yOf(n.note);
                const int end = static_cast<int>(n.end) + drag_->samples;
                const ImVec2 na(xAtSample(static_cast<float>(n.start)), yb - rowH + 0.5f);
                const float oldX = xAtSample(static_cast<float>(n.end)), newX = std::max(xAtSample(static_cast<float>(end)), na.x + px(2));
                if (newX > oldX)
                    dl->AddRectFilled(ImVec2(oldX, na.y), ImVec2(newX, yb - 0.5f), alpha(T::kCyan, 0.55f), px(1));
                else
                    dl->AddRectFilled(ImVec2(newX, na.y), ImVec2(oldX, yb - 0.5f), IM_COL32(0, 0, 0, 150), px(1));
                dl->AddRect(ImVec2(na.x - 1, na.y - 1), ImVec2(newX + 1, yb + 0.5f), T::kCyan, px(1), px(1.5f));
                dl->AddRectFilled(ImVec2(newX - px(3), na.y - px(1)), ImVec2(newX + px(1), yb + px(0.5f)), IM_COL32_WHITE, px(1));
            }
                } else if (drag_ && editing_) {  // where they go
            for (const NoteInterval& n : drag_->notes) {
                const float yb = yOf(movedNote(n.note));
                const int start = n.start + drag_->samples, end = std::max(static_cast<int>(n.end), static_cast<int>(n.start) + 1) + drag_->samples;
                const ImVec2 na(xAtSample(static_cast<float>(start)), yb - rowH + 0.5f);
                const ImVec2 nb(std::max(xAtSample(static_cast<float>(end)), na.x + px(2)), yb - 0.5f);
                dl->AddRectFilled(na, nb, alpha(T::kCyan, 0.85f), px(1));
                dl->AddRect(ImVec2(na.x - 1, na.y - 1), ImVec2(nb.x + 1, nb.y + 1), IM_COL32_WHITE, px(1), px(1));
            }
        } else if (editing_ && hovered >= 0 && ImGui::IsWindowHovered()) {
            const NoteInterval& n = live.noteLoop[static_cast<std::size_t>(hovered)];
            const float yb = yOf(n.note);
            dl->AddRect(ImVec2(xAtSample(static_cast<float>(n.start)) - 2, yb - rowH - 1.5f),
                        ImVec2(std::max(xAtSample(static_cast<float>(n.end)), xAtSample(static_cast<float>(n.start)) + px(2)) + 2, yb + 1.5f),
                        IM_COL32(255, 255, 255, 200), px(1), px(1));
        }
        if (bandFrom_ && editing_) {  // the selection rectangle
            const ImVec2 ra(std::min(bandFrom_->x, mouse.x), std::min(bandFrom_->y, mouse.y));
            const ImVec2 rb(std::max(bandFrom_->x, mouse.x), std::max(bandFrom_->y, mouse.y));
            dl->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 25));
            dl->AddRect(ra, rb, IM_COL32(255, 255, 255, 160), 0.0f, px(1));
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                for (int i = 0; i < live.noteLoopCount; ++i) {
                    const NoteInterval& n = live.noteLoop[static_cast<std::size_t>(i)];
                    const float yb = yOf(n.note);
                    const float xa = xAtSample(static_cast<float>(n.start));
                    const float xb = std::max(xAtSample(static_cast<float>(n.end)), xa + px(2));
                    if (xb >= ra.x && xa <= rb.x && yb >= ra.y && yb - rowH <= rb.y && !selected(n))
                        selection_.push_back({n.start, n.note});
                }
                bandFrom_.reset();
            }
        }
        if (editing_) {
            // The loop's end: what goes (dimmed, hatched) or the silence added,
            // and the handle, at the right edge at rest.
            const float endX = xAtSample(static_cast<float>(newLength)), oldX = xAtSample(static_cast<float>(st.length));
            if (newLength < st.length) {
                const float cutTo = std::min(oldX, ax1);
                dl->AddRectFilled(ImVec2(endX, ay0), ImVec2(cutTo, ay1), IM_COL32(10, 10, 12, 130));
                dl->PushClipRect(ImVec2(endX, ay0), ImVec2(cutTo, ay1), true);
                for (float hx = endX - (ay1 - ay0); hx < cutTo; hx += px(10))
                    dl->AddLine(ImVec2(hx, ay1), ImVec2(hx + (ay1 - ay0), ay0), IM_COL32(255, 255, 255, 16), px(1));
                dl->PopClipRect();
                dl->AddLine(ImVec2(oldX - px(1), ay0), ImVec2(oldX - px(1), ay1), alpha(kViolet, 0.3f), px(1));
            } else if (newLength > st.length) {
                dl->AddRectFilled(ImVec2(oldX, ay0), ImVec2(ax1, ay1), alpha(kViolet, 0.1f));
                dl->AddLine(ImVec2(oldX, ay0), ImVec2(oldX, ay1), alpha(kViolet, 0.35f), px(1));
                if (ax1 - oldX > px(80))
                    text(dl, gMedium, px(10), (oldX + ax1) * 0.5f, ay1 - px(10), alpha(kViolet, 0.7f), "silence added", H::Center, V::Middle);
            }
            const ImVec2 at = ImGui::GetIO().MousePos;
            const float restX = std::min(xAtSample(shownLength), ax1);  // the end, at the right edge unzoomed
            const bool endShown = xAtSample(shownLength) <= ax1 + px(1);
            lengthHovered_ = endShown && !drag_ && !bandFrom_ && ImGui::IsWindowHovered() && at.x >= restX - px(8) &&
                             at.x <= restX + px(3) && at.y >= ay0 && at.y <= ay1;
            const bool active = lengthDrag_ || lengthHovered_;
            const float hx = (newLength < st.length ? endX : restX) - px(1);
            if (endShown || lengthDrag_) {
                if (active)
                    dl->AddLine(ImVec2(hx, ay0), ImVec2(hx, ay1), alpha(kViolet, 0.25f), px(6));
                dl->AddLine(ImVec2(hx, ay0), ImVec2(hx, ay1), alpha(kViolet, active ? 1.0f : 0.65f), px(2));
                const float gy = (ay0 + ay1) * 0.5f;
                // Its grip, inside the roll.
                dl->AddRectFilled(ImVec2(hx - px(6), gy - px(14)), ImVec2(hx + px(1), gy + px(14)), alpha(kViolet, active ? 1.0f : 0.8f), px(3));
                for (int k = -1; k <= 1; ++k)
                    dl->AddLine(ImVec2(hx - px(4), gy + static_cast<float>(k) * px(5)), ImVec2(hx - px(1.5f), gy + static_cast<float>(k) * px(5)),
                                IM_COL32(0x28, 0x1E, 0x3C, 255), px(1));
            }
        }
        dl->PopClipRect();
        noteHovered = hovered;
        any = true;
    } else {
        // Half-second lines, scrolling with time.
        for (double t = std::floor(time_ * 2.0) / 2.0; xAtTime(t) > gx0; t -= 0.5)
            dl->AddLine(ImVec2(xAtTime(t), ay0), ImVec2(xAtTime(t), ay1), IM_COL32(255, 255, 255, 18));
        // What was played: white, or violet from a loop; brighter when louder.
        for (const RollNote& n : rollNotes_) {
            if (n.channel != live.channel)
                continue;
            const double end = n.sounding ? time_ : n.end;
            if (end < time_ - kWindow)
                continue;
            any = true;
            const float yb = yOf(n.note);
            const float a = 0.55f + 0.45f * static_cast<float>(n.velocity) / 127.0f;
            dl->AddRectFilled(ImVec2(std::max(xAtTime(n.start), gx0), yb - rowH + 0.5f),
                              ImVec2(noteBarEnd(xAtTime(n.start), xAtTime(end), !n.sounding), yb - 0.5f), alpha(n.fromLoop ? kViolet : kLive, a), px(1));
        }
    }
    dl->PopClipRect();

    // The playhead (clip) or "now" (scrolling, at the right edge).
    if (clip) {
        const float hx = xAtSample(static_cast<float>(st.position));
        const ImU32 col = st.overdub ? T::kOrange : IM_COL32(255, 255, 255, 200);
        dl->AddLine(ImVec2(hx, ay0), ImVec2(hx, ay1), col, px(st.overdub ? 2.0f : 1.4f));
        const float s2 = px(4), dir = static_cast<float>(st.direction);  // which way it sweeps
        dl->AddTriangleFilled(ImVec2(hx + dir * s2, ay0 + s2), ImVec2(hx - dir * s2, ay0), ImVec2(hx - dir * s2, ay0 + s2 * 2), col);
    } else if (any || recording) {
        dl->AddLine(ImVec2(ax1 - px(1), ay0), ImVec2(ax1 - px(1), ay1), recording ? alpha(T::kRed, 0.9f) : IM_COL32(255, 255, 255, 120), px(1.5f));
    }
    if (!status.empty()) {
        const float tw = textWidth(gBold, px(10), status);
        const float chipX = ax1 - tw - px(12) - (st.locked && clip ? px(16) : 0.0f) - (editing_ && clip ? px(8) : 0.0f);
        const float chipR = ax1 - px(3) - (editing_ && clip ? px(8) : 0.0f);  // clear of the loop's end handle
        dl->AddRectFilled(ImVec2(chipX, ay0 + px(3)), ImVec2(chipR, ay0 + px(17)), IM_COL32(0x20, 0x22, 0x26, 230), px(4));
        if (lengthDrag_ && editing_)
            dl->AddRect(ImVec2(chipX, ay0 + px(3)), ImVec2(chipR, ay0 + px(17)), kViolet, px(4), px(1));
        text(dl, gBold, px(10), chipR - px(4), ay0 + px(10), lengthDrag_ && editing_ ? kViolet : statusColor, status, H::Right, V::Middle);
        if (st.locked && clip)
            drawLock(dl, chipX + px(5), ay0 + px(10), px(10), kViolet);
        if (clip && std::abs(live.velocityLoop) >= 0.005f) {  // a Velocity axis on the loop: by how much
            char vel[24];
            std::snprintf(vel, sizeof vel, "VEL %+d%%", static_cast<int>(std::lround(live.velocityLoop * 100.0f)));
            drawChip(dl, chipX - px(6), ay0 + px(10), vel, T::kBlue);
        }
    }
    if (st.locked && clip)
        drawLockedBorder(dl, ImVec2(ax0, ay0), ImVec2(ax1, ay1), kViolet, px(3));
    const bool pressed = hoverButton("##pianoroll", ImVec2(ax0, ay0), ImVec2(ax1, ay1));
    const bool hovering = ImGui::IsItemHovered();
    if (!editing_) {  // in the editor, a right-click deletes notes
        const MenuTarget target{MenuTarget::Kind::NoteLoop, 0, 0, static_cast<int>(kNotes), -1, "notes"};
        menuOnRightClick(target, clip, st.locked);
        if (gFocusItems)
            focusTargets_.push_back({"##pianoroll", target});
    }
    if (!clip || live.channel != selectionChannel_) {
        drag_.reset();
        if (editing_ || !clip)
            lengthDrag_.reset();
        if (editing_) {
            viewZoom_ = 1;
            viewStart_ = 0;
        }
        bandFrom_.reset();
        selection_.clear();
        selectionChannel_ = live.channel;
    }
    if (!editing_ || !clip) {  // the view's piano roll: a click opens the editor
        if (pressed && !editing_)
            click("notes");
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    // Ctrl + wheel zooms around the mouse, from the whole loop to a second (or
    // a beat) across; the wheel alone or a sideways scroll moves along. The
    // window does not scroll.
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (hovering && !lengthDrag_ && !drag_ && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        const float most = std::max(1.0f, st.beats > 0 ? static_cast<float>(st.beats) : static_cast<float>(st.length) / Engine::kLoopRateHz);
        if (io.MouseWheel != 0.0f && io.KeyCtrl) {  // Ctrl + wheel: zoom, as everywhere
            const float f = std::clamp((mouse.x - gx0) / std::max(ax1 - gx0, 1.0f), 0.0f, 1.0f);
            const float under = viewStart_ + f / viewZoom_;  // the place under the mouse stays there
            viewZoom_ = std::clamp(viewZoom_ * std::pow(1.25f, io.MouseWheel), 1.0f, most);
            viewStart_ = under - f / viewZoom_;
        } else if (io.MouseWheel != 0.0f) {  // the wheel alone moves along
            viewStart_ -= io.MouseWheel * 0.1f / viewZoom_;
        }
        viewStart_ -= io.MouseWheelH * 0.1f / viewZoom_;
        viewStart_ = std::clamp(viewStart_, 0.0f, 1.0f - 1.0f / viewZoom_);
    }
    const float sampleAt = sampleAtX(mouse.x);
    const bool byDegrees = scaleMoves_ && key_.has_value();
    // The selection keeps only notes still there (once an edit had time to land).
    if (time_ - lastNoteEdit_ > 0.5)
        std::erase_if(selection_, [&](const LoopNoteRef& r) {
            for (int i = 0; i < live.noteLoopCount; ++i)
                if (live.noteLoop[static_cast<std::size_t>(i)].start == r.start && live.noteLoop[static_cast<std::size_t>(i)].note == r.note)
                    return false;
            return true;
        });
    auto noteAt = [&](int i) -> const NoteInterval& { return live.noteLoop[static_cast<std::size_t>(i)]; };
    // The loop's end: dragged, its new length on release; Escape keeps it as it was.
    if (lengthDrag_ || lengthHovered_)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (lengthDrag_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            lengthDrag_.reset();
            escapeTaken_ = true;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (lengthDrag_->newLength != lengthDrag_->length) {
                NoteEdit edit;
                edit.loopLength = lengthDrag_->newLength;
                noteEdit_ = std::move(edit);
                lastNoteEdit_ = time_;
            }
            lengthDrag_.reset();
        }
        return;
    }
    if (pressed && lengthHovered_) {
        lengthDrag_ = LengthDrag{mouse.x, st.length, st.beats, st.length};
        return;
    }
    if ((hovering && noteEndHovered) || (drag_ && drag_->resize))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (hovering && noteHovered >= 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (pressed && noteHovered >= 0 && noteEndHovered && !io.KeyCtrl && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // The end of a selected note: every selected note's length.
        NoteDrag d;
        for (int i = 0; i < live.noteLoopCount; ++i)
            if (selected(noteAt(i)))
                d.notes.push_back(noteAt(i));
        d.grabSample = sampleAt;
        d.grabY = mouse.y;
        d.resize = true;
        d.grabEnd = noteAt(noteHovered).end;
        drag_ = std::move(d);
    } else if (pressed && noteHovered >= 0) {
        const NoteInterval& n = noteAt(noteHovered);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // Its chord: the notes of the same press (or, unknown, starting with it).
            drag_.reset();
            selection_.clear();
            for (int i = 0; i < live.noteLoopCount; ++i) {
                const NoteInterval& other = noteAt(i);
                if (n.press != 0 ? other.press == n.press : std::abs(other.start - n.start) <= Engine::kLoopRateHz / 10)
                    selection_.push_back({other.start, other.note});
            }
        } else if (io.KeyCtrl) {
            if (selected(n))
                std::erase_if(selection_, [&](const LoopNoteRef& r) { return r.start == n.start && r.note == n.note; });
            else
                selection_.push_back({n.start, n.note});
        } else {
            if (!selected(n))
                selection_ = {{n.start, n.note}};
            NoteDrag d;
            for (int i = 0; i < live.noteLoopCount; ++i)
                if (selected(noteAt(i)))
                    d.notes.push_back(noteAt(i));
            d.grabSample = sampleAt;
            d.grabY = mouse.y;
            drag_ = std::move(d);
        }
    } else if (pressed) {  // empty space: a selection rectangle (with Ctrl, added to the selection)
        bandFrom_ = mouse;
        if (!io.KeyCtrl)
            selection_.clear();
    }
    auto removeNotes = [&](std::vector<LoopNoteRef> notes) {
        NoteEdit edit;
        edit.remove = true;
        edit.notes = std::move(notes);
        noteEdit_ = std::move(edit);
        lastNoteEdit_ = time_;
    };
    if (hovering && noteHovered >= 0 && !drag_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const NoteInterval& n = noteAt(noteHovered);
        removeNotes(selected(n) ? std::exchange(selection_, {}) : std::vector<LoopNoteRef>{{n.start, n.note}});
    }
    const bool keysFree = !ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!selection_.empty() && !drag_ && keysFree && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        removeNotes(std::exchange(selection_, {}));
    if (!selection_.empty() && !drag_ && keysFree && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        selection_.clear();
        escapeTaken_ = true;
    }
    if (drag_ && drag_->resize && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (drag_->samples != 0) {
            NoteEdit edit;
            for (const NoteInterval& n : drag_->notes)
                edit.notes.push_back({n.start, n.note});
            edit.samples = drag_->samples;
            edit.resize = true;
            noteEdit_ = std::move(edit);
            lastNoteEdit_ = time_;
        }
        drag_.reset();
    }
    if (drag_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (drag_->samples != 0 || drag_->semitones != 0) {
            NoteEdit edit;
            for (const NoteInterval& n : drag_->notes)
                edit.notes.push_back({n.start, n.note});
            edit.samples = drag_->samples;
            edit.semitones = drag_->semitones;
            edit.byDegrees = byDegrees;
            for (LoopNoteRef& r : selection_) {  // the selection moves with them
                r.start += drag_->samples;
                r.note = static_cast<std::uint8_t>(byDegrees ? Engine::transposedIn(key_, r.note, drag_->semitones)
                                                             : std::clamp(r.note + drag_->semitones, 0, 127));
            }
            noteEdit_ = std::move(edit);
            lastNoteEdit_ = time_;
        }
        drag_.reset();
    }
}

// The session screen: a column per channel, a row per clip slot, a stop at
// the bottom of each column; the clips with what they hold and their state.
std::optional<PerformanceView::SessionAction> PerformanceView::drawSession(const LiveState& live, int cursorSlot, int chainCursor, int button) {
    gMedium = fonts_.medium ? fonts_.medium : ImGui::GetFont();
    gBold = fonts_.bold ? fonts_.bold : gMedium;
    gCondensed = fonts_.condensed ? fonts_.condensed : gBold;
    time_ = ImGui::GetTime();
    std::optional<SessionAction> action;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    constexpr int kColumns = 8, kSlots = Engine::kClipSlots;
    const float gap = px(6), colW = (width - gap * (kColumns - 1)) / kColumns;
    const float stripH = px(104);  // the chain, under the grid
    const float headH = px(34), stopH = px(28);
    const float cellH = std::clamp((ImGui::GetContentRegionAvail().y - headH - stopH - px(10) - px(8) - stripH - px(4)) / kSlots, px(44), px(70));
    const float gridH = headH + cellH * kSlots + stopH + px(10);
    const bool blink = std::fmod(time_, 0.8) < 0.45;
    if (live.channel != sessionChannelSeen_) {  // the active channel changed: show its eight
        sessionFirst_ = live.channel / kColumns * kColumns;
        sessionChannelSeen_ = live.channel;
    }
    for (int col = 0; col < kColumns; ++col) {
        const int ch = sessionFirst_ + col;
        const auto c = static_cast<std::size_t>(ch);
        const ImU32 chColor = kChannelColors[ch % 8];
        const float x = origin.x + static_cast<float>(col) * (colW + gap);
        const bool playing = (live.clipsPlaying >> ch) & 1u;
        if (gLook) {
            frame(dl, ImVec2(x, origin.y), ImVec2(x + colW, origin.y + gridH), headH, gLook == 1 ? kNeon : chColor);
        } else {
            dl->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + colW, origin.y + gridH), T::kPanel, px(8));
            dl->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + colW, origin.y + headH), T::kPanelHeader, px(8), ImDrawFlags_RoundCornersTop);
            dl->AddRect(ImVec2(x, origin.y), ImVec2(x + colW, origin.y + gridH), T::kBorder, px(8), px(1));
        }
        // Header, as a module's: the channel, a LED while it plays; the active one underlined. A click chooses it.
        const bool active = ch == live.channel;
        text(dl, gBold, px(12.5f), x + px(12), origin.y + px(17), active ? T::kText : T::kLabel, fit(gBold, px(12.5f), channelTitle(ch), colW - px(64)), H::Left,
             V::Middle);
        led(dl, ImVec2(x + colW - px(15), origin.y + px(17)), px(5), T::kGreen, playing);
        if (!playing)
            dl->AddCircleFilled(ImVec2(x + colW - px(15), origin.y + px(17)), px(5), T::kDim);
        {  // a chain sign when it has one: green with its chain and step while one runs
            const LiveState::Chains& chains = live.chains[c];
            const bool runs = chains.at >= 0 && chains.playing >= 0;
            const LiveState::Chain& chain = chains.list[static_cast<std::size_t>(runs ? chains.playing : 0)];
            int used = 0, index = 0;
            for (int k = 0; k < Engine::kChainSteps; ++k)
                if (chain.clip[static_cast<std::size_t>(k)] >= 0) {
                    ++used;
                    if (k <= chains.at)
                        index = used;
                }
            bool any = runs;
            for (const LiveState::Chain& other : chains.list)
                any = any || !other.empty();
            if (any) {
                const std::string label = runs ? chainLetter(chains.playing) + " " + std::to_string(index) + "/" + std::to_string(used) : "";
                const float right = x + colW - px(26);
                const float w = px(24) + (label.empty() ? 0.0f : textWidth(gBold, px(8), label) + px(3));
                const ImU32 cc = runs ? T::kGreen : T::kDim;
                const ImVec2 ca(right - w, origin.y + px(10)), cb(right, origin.y + px(24));
                dl->AddRectFilled(ca, cb, alpha(cc, 0.18f), px(6));
                dl->AddRect(ca, cb, alpha(cc, 0.8f), px(6), px(1));
                const ImU32 link = runs ? T::kText : T::kDim;
                const float ly = origin.y + px(17);
                dl->AddRect(ImVec2(ca.x + px(5), ly - px(3)), ImVec2(ca.x + px(13), ly + px(3)), link, px(3), px(1.3f));
                dl->AddRect(ImVec2(ca.x + px(10), ly - px(3)), ImVec2(ca.x + px(18), ly + px(3)), link, px(3), px(1.3f));
                if (!label.empty())
                    text(dl, gBold, px(8), ca.x + px(22), ly, T::kText, label, H::Left, V::Middle);
            }
        }
        if (active)
            dl->AddLine(ImVec2(x + px(12), origin.y + px(28)), ImVec2(x + px(12) + textWidth(gBold, px(11), "CHANNEL 16"), origin.y + px(28)), chColor, px(2));
        ImGui::PushID(ch);
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        if (ImGui::InvisibleButton("##head", ImVec2(colW, headH)))
            action = SessionAction{SessionAction::Kind::Channel, ch, 0};
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            renameRequest_ = ch;
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::Channel, ch}, ImGui::GetIO().MousePos};
        for (int slot = 0; slot < kSlots; ++slot) {
            const LiveState::Clip& clip = live.clips[c * kSlots + static_cast<std::size_t>(slot)];
            const float cy = origin.y + headH + cellH * static_cast<float>(slot);
            const ImVec2 a(x + px(6), cy + px(4)), b(x + colW - px(6), cy + cellH - px(4));
            const bool current = live.currentClip[c] == slot;
            const bool clipPlays = current && playing;
            const bool queued = live.queuedClip[c] == slot;
            ImGui::PushID(slot);
            ImGui::SetCursorScreenPos(a);
            const bool clicked = ImGui::InvisibleButton("##clip", ImVec2(b.x - a.x, b.y - a.y));
            const bool hovered = ImGui::IsItemHovered();
            if (clip.sources && ImGui::BeginDragDropSource()) {  // onto a step of the chain
                const int payload[2] = {ch, slot};
                ImGui::SetDragDropPayload("MIDIPAD_CLIP", payload, sizeof payload);
                ImGui::Text("Clip %d", slot + 1);
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
            if (clicked)
                action = SessionAction{SessionAction::Kind::Launch, ch, slot};
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::Clip, ch, slot}, ImGui::GetIO().MousePos};
            if (!clip.sources) {  // empty: a screen off
                screen(dl, a, b, kScreenOff, [] {});
                if (hovered)
                    dl->AddRect(a, b, T::kControlBorder, px(4), px(1));
            } else {  // a screen in the channel's colour, its state around it
                const ImU32 outline = clip.recording ? T::kRed : clipPlays ? T::kGreen : queued ? alpha(T::kGreen, blink ? 0.9f : 0.3f)
                                    : hovered      ? T::kControlBorder
                                                   : 0;
                screen(dl, a, b, chColor, [&] {
                    // Its name and a green LED (lit playing or recording, blinking
                    // waiting to start), its notes below, else one of its curves.
                    dl->AddRectFilled(ImVec2(a.x + px(2), a.y + px(4)), ImVec2(a.x + px(5), b.y - px(4)), chColor, px(1));
                    text(dl, gBold, px(12.5f), a.x + px(11), a.y + px(13), T::kText, "Clip " + std::to_string(slot + 1), H::Left, V::Middle);
                    led(dl, ImVec2(b.x - px(12), a.y + px(13)), px(4.5f), T::kGreen, clipPlays || clip.recording || (queued && blink));
                    const float x0 = a.x + px(11), x1 = b.x - px(9), y0 = a.y + px(26), y1 = b.y - px(8), th = px(3);
                    if ((clip.sources & (1u << static_cast<int>(LoopSource::Notes))) && clip.noteCount > 0) {
                        int lo = 127, hi = 0;
                        for (int i = 0; i < clip.noteCount; ++i) {
                            lo = std::min<int>(lo, clip.notes[static_cast<std::size_t>(i)][2]);
                            hi = std::max<int>(hi, clip.notes[static_cast<std::size_t>(i)][2]);
                        }
                        for (int i = 0; i < clip.noteCount; ++i) {
                            const auto& n = clip.notes[static_cast<std::size_t>(i)];
                            const float nx0 = x0 + (x1 - x0) * static_cast<float>(n[0]) / 255.0f;
                            const float nx1 = std::max(x0 + (x1 - x0) * static_cast<float>(n[1]) / 255.0f, nx0 + px(3));
                            const float t = hi > lo ? static_cast<float>(n[2] - lo) / static_cast<float>(hi - lo) : 0.5f;
                            const float ny = y1 - th - t * (y1 - y0 - th);
                            dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(nx1, ny + th), IM_COL32(0xF0, 0xF2, 0xF5, 242), th * 0.4f);
                        }
                    } else {
                        for (const auto& curve : clip.curves) {
                            if (std::all_of(curve.begin(), curve.end(), [](std::uint8_t v) { return v == 0; }))
                                continue;
                            ImVec2 pts[LiveState::Clip::kCurvePoints];
                            for (int i = 0; i < LiveState::Clip::kCurvePoints; ++i) {
                                const float t = static_cast<float>(i) / static_cast<float>(LiveState::Clip::kCurvePoints - 1);
                                pts[i] = ImVec2(x0 + t * (x1 - x0), y1 - static_cast<float>(curve[static_cast<std::size_t>(i)]) / 255.0f * (y1 - y0));
                            }
                            dl->AddPolyline(pts, LiveState::Clip::kCurvePoints, mix(chColor, IM_COL32(255, 255, 255, 255), 0.35f), px(2));
                            break;
                        }
                    }
                    if (clipPlays) {  // how far in its turn
                        const float p = std::clamp(live.clipProgress[c], 0.0f, 1.0f);
                        const ImVec2 ta(a.x + px(9), b.y - px(7)), tb(b.x - px(9), b.y - px(3.5f));  // a dark track under it
                        dl->AddRectFilled(ta, tb, IM_COL32(0, 0, 0, 170), px(2));
                        dl->AddRectFilled(ta, ImVec2(ta.x + (tb.x - ta.x) * p, tb.y), T::kGreen, px(2));
                    }
                });
                if (outline)
                    dl->AddRect(a, b, outline, px(4), px(clip.recording || clipPlays || queued ? 2.0f : 1.0f));
            }
            if (active && slot == cursorSlot) {  // the cursor
                sessionCursorBox_ = {a, b};
                dl->AddRect(ImVec2(a.x - px(3), a.y - px(3)), ImVec2(b.x + px(3), b.y + px(3)), T::kCyan, px(7), px(2.5f));
                if (!clip.sources) {
                    text(dl, gBold, px(18), (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, T::kCyan, "+", H::Center, V::Middle);
                }
            }
        }
        // A channel muted, or out of the solo, sends nothing from its clips: they dim.
        const float sy = origin.y + headH + cellH * kSlots + px(2);
        const bool muted = ((live.muted >> ch) & 1u) != 0, soloed = ((live.soloed >> ch) & 1u) != 0;
        if (muted || (live.soloed != 0 && !soloed))
            dl->AddRectFilled(ImVec2(x + px(3), origin.y + headH), ImVec2(x + colW - px(3), sy - px(2)), IM_COL32(0, 0, 0, muted ? 150 : 95), px(4));
        // At the bottom: the stop (blinking while waiting for the end of the turn), then M and S.
        const float msW = px(34), gapB = px(4);
        const float stopW = colW - px(12) - 2 * (msW + gapB);
        for (int k = 0; k < 2; ++k) {
            const bool on = k == 0 ? muted : soloed;
            const ImU32 tone = k == 0 ? IM_COL32(0xD9, 0xA0, 0x48, 255) : IM_COL32(0x6F, 0xB8, 0xD6, 255);  // amber, blue: muted tones
            const float bx = x + px(6) + stopW + gapB + static_cast<float>(k) * (msW + gapB);
            const ImVec2 ba(bx, sy), bb(bx + msW, sy + stopH - px(2));
            ImGui::SetCursorScreenPos(ba);
            if (ImGui::InvisibleButton(k == 0 ? "##mute" : "##solo", ImVec2(msW, bb.y - ba.y)))
                action = SessionAction{k == 0 ? SessionAction::Kind::Mute : SessionAction::Kind::Solo, ch, on ? 0 : 1};
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetItemTooltip(k == 0 ? "Mute: its clips, chains and song play on, silent" : "Solo: only the channels in solo sound");
            dl->AddRectFilled(ba, bb, on ? alpha(tone, 0.32f) : IM_COL32(0x28, 0x2A, 0x2E, 255), px(5));
            dl->AddRect(ba, bb, on ? alpha(tone, 0.8f) : hovered ? T::kControlBorder : IM_COL32(0x3A, 0x3E, 0x45, 255), px(5), px(1));
            text(dl, gBold, px(12), (ba.x + bb.x) * 0.5f, (ba.y + bb.y) * 0.5f, on ? mix(tone, T::kText, 0.45f) : T::kLabel, k == 0 ? "M" : "S", H::Center, V::Middle);
            if (active && button == k + 1) {
                dl->AddRect(ImVec2(ba.x - px(3), ba.y - px(3)), ImVec2(bb.x + px(3), bb.y + px(3)), T::kCyan, px(7), px(2.5f));
                sessionCursorBox_ = {ba, bb};
            }
        }
        const ImVec2 sa(x + px(6), sy), sb(x + px(6) + stopW, sy + stopH - px(2));
        if (active && button == 0) {
            dl->AddRect(ImVec2(sa.x - px(3), sa.y - px(3)), ImVec2(sb.x + px(3), sb.y + px(3)), T::kCyan, px(7), px(2.5f));
            sessionCursorBox_ = {sa, sb};
        }
        ImGui::SetCursorScreenPos(sa);
        const bool stopClicked = ImGui::InvisibleButton("##stop", ImVec2(sb.x - sa.x, sb.y - sa.y));
        const bool stopHovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("Stops this channel at the end of its clip's turn");
        if (stopClicked)
            action = SessionAction{SessionAction::Kind::Stop, ch, 0};
        const bool stopping = live.queuedClip[c] == kSlots;
        dl->AddRectFilled(sa, sb, IM_COL32(0x28, 0x2A, 0x2E, 255), px(5));
        dl->AddRect(sa, sb, stopHovered ? T::kControlBorder : IM_COL32(0x3A, 0x3E, 0x45, 255), px(5), px(1));
        const float mx = (sa.x + sb.x) * 0.5f, my = (sa.y + sb.y) * 0.5f;
        dl->AddRectFilled(ImVec2(mx - px(5), my - px(5)), ImVec2(mx + px(5), my + px(5)),
                          stopping ? alpha(T::kRed, blink ? 1.0f : 0.35f) : playing ? T::kLabel : IM_COL32(0x50, 0x55, 0x5C, 255), px(1));
        ImGui::PopID();
    }
    // The chain of the active channel, then the other eight channels' link in its header.
    const float by = origin.y + gridH + px(8);
    if (auto chainAction = drawChainStrip(dl, live, ImVec2(origin.x, by), width, stripH, cursorSlot == kSlots ? chainCursor : -1))
        action = chainAction;
    const char* other = sessionFirst_ == 0 ? "Channels 9\xE2\x80\x93" "16  \xE2\x80\xBA" : "\xE2\x80\xB9  Channels 1\xE2\x80\x93" "8";
    const float ow = textWidth(gMedium, px(10), other) + px(16);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - ow - px(4), by + px(5)));
    if (ImGui::InvisibleButton("##otherchannels", ImVec2(ow, px(20))))
        sessionFirst_ = sessionFirst_ == 0 ? 8 : 0;
    text(dl, gMedium, px(10), origin.x + width - px(12), by + px(15), ImGui::IsItemHovered() ? T::kText : T::kDim, other, H::Right, V::Middle);
    return action;
}

std::pair<ImVec2, ImVec2> PerformanceView::cursorBox() const {
    if (editMode_)
        for (const FocusItem& item : focusItems_)
            if (item.id == focus_)
                return {item.a, item.b};
    return sessionCursorBox_;
}

// A controller button as a small sign: the face buttons' symbols, a pill with the name for the others.
float buttonSign(ImDrawList* dl, float x, float y, const std::string& name) {
    const float r = px(12);
    // Xbox and Nintendo face buttons: a letter in a ring (Xbox's in their colours).
    const ControllerFamily family = labelFamily();
    if ((family == ControllerFamily::Xbox || family == ControllerFamily::Nintendo) && name.size() == 1 && std::string("ABXY").find(name) != std::string::npos) {
        const ImU32 ring = family == ControllerFamily::Nintendo ? kPadGrey
                           : name == "A" ? IM_COL32(0x5C, 0xC8, 0x5C, 255) : name == "B" ? IM_COL32(0xF0, 0x5A, 0x5A, 255)
                           : name == "X" ? IM_COL32(0x4D, 0x9C, 0xFF, 255) : IM_COL32(0xF2, 0xC0, 0x3C, 255);
        dl->AddCircleFilled(ImVec2(x + r, y), r, IM_COL32(0x1E, 0x20, 0x24, 255));
        dl->AddCircle(ImVec2(x + r, y), r, ring, 0, px(1.6f));
        text(dl, gBold, px(12), x + r, y, ring, name, H::Center, V::Middle);
        return x + 2 * r;
    }
    ImU32 col = 0;
    if (name == "Cross") col = IM_COL32(0x6E, 0x8C, 0xFF, 255);
    if (name == "Circle") col = IM_COL32(0xFF, 0x5A, 0x5A, 255);
    if (name == "Square") col = IM_COL32(0xFF, 0x78, 0xC8, 255);
    if (name == "Triangle") col = IM_COL32(0x40, 0xD2, 0xAA, 255);
    if (col) {
        dl->AddCircleFilled(ImVec2(x + r, y), r, IM_COL32(0x1E, 0x20, 0x24, 255));
        dl->AddCircle(ImVec2(x + r, y), r, col, 0, px(1.6f));
        const float q = px(5.5f);
        if (name == "Cross") {
            dl->AddLine(ImVec2(x + r - q, y - q), ImVec2(x + r + q, y + q), col, px(1.8f));
            dl->AddLine(ImVec2(x + r + q, y - q), ImVec2(x + r - q, y + q), col, px(1.8f));
        } else if (name == "Circle") {
            dl->AddCircle(ImVec2(x + r, y), q, col, 0, px(1.6f));
        } else if (name == "Square") {
            dl->AddRect(ImVec2(x + r - q, y - q), ImVec2(x + r + q, y + q), col, 0, px(1.6f));
        } else {
            dl->AddTriangle(ImVec2(x + r, y - q - px(1)), ImVec2(x + r + q + px(1), y + q), ImVec2(x + r - q - px(1), y + q), col, px(1.6f));
        }
        return x + 2 * r;
    }
    const float w = textWidth(gBold, px(12), name) + px(16);
    dl->AddRectFilled(ImVec2(x, y - r + px(1)), ImVec2(x + w, y + r - px(1)), IM_COL32(0x1E, 0x20, 0x24, 255), r);
    dl->AddRect(ImVec2(x, y - r + px(1)), ImVec2(x + w, y + r - px(1)), T::kLabel, r, px(1));
    text(dl, gBold, px(12), x + w * 0.5f, y, T::kText, name, H::Center, V::Middle);
    return x + w;
}

void PerformanceView::drawHelp(const std::string& title, ImU32 color, const std::string& closeHint,
                               const std::vector<std::pair<std::vector<std::string>, std::string>>& rows) {
    gMedium = fonts_.medium ? fonts_.medium : ImGui::GetFont();
    gBold = fonts_.bold ? fonts_.bold : gMedium;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 vs = ImGui::GetMainViewport()->Size, vp = ImGui::GetMainViewport()->Pos;
    dl->AddRectFilled(vp, ImVec2(vp.x + vs.x, vp.y + vs.y), IM_COL32(8, 9, 11, 170));
    const float w = std::min(px(680), vs.x - px(40)), rowH = px(46);
    const float h = px(70) + rowH * static_cast<float>(rows.size()) + px(8);
    const ImVec2 a(vp.x + (vs.x - w) * 0.5f, vp.y + (vs.y - h) * 0.5f), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, IM_COL32(0x1E, 0x20, 0x24, 248), px(10));
    dl->AddRect(a, b, T::kBorder, px(10), px(1));
    text(dl, gBold, px(18), a.x + px(22), a.y + px(30), color, title, H::Left, V::Middle);
    text(dl, gMedium, px(12), b.x - px(22), a.y + px(30), T::kDim, closeHint, H::Right, V::Middle);
    float y = a.y + px(70);
    for (const auto& [keys, what] : rows) {
        float x = a.x + px(24);
        for (const std::string& k : keys)
            x = buttonSign(dl, x, y + rowH * 0.3f, k) + px(8);
        text(dl, gMedium, px(14), a.x + px(200), y + rowH * 0.3f, T::kText, what, H::Left, V::Middle);
        y += rowH;
    }
}

int PerformanceView::nextRepeats(int repeats, int steps) {
    const auto at = std::find(kChainRepeats.begin(), kChainRepeats.end(), repeats);
    const int i = at == kChainRepeats.end() ? 0 : static_cast<int>(at - kChainRepeats.begin());
    return kChainRepeats[static_cast<std::size_t>(std::clamp(i + steps, 0, static_cast<int>(kChainRepeats.size()) - 1))];
}

// The chain strip of the session screen (see drawSession), at `at`, `w` by `h`;
// `cursor` the step under the controller's cursor (kChainSteps: the end), -1 none.
std::optional<PerformanceView::SessionAction> PerformanceView::drawChainStrip(ImDrawList* dl, const LiveState& live, ImVec2 at, float w, float h,
                                                                                int cursor) {
    std::optional<SessionAction> action;
    const int ch = live.channel;
    const auto c = static_cast<std::size_t>(ch);
    const LiveState::Chains& chains = live.chains[c];
    const int shownChain = std::clamp(shownChains_[c], 0, Engine::kChains - 1);
    const LiveState::Chain& chain = chains.list[static_cast<std::size_t>(shownChain)];
    const ImU32 chColor = kChannelColors[ch % 8];
    const bool anyRuns = chains.at >= 0 && chains.playing >= 0;
    const bool runs = anyRuns && chains.playing == shownChain;  // the chain shown plays
    const bool nextHere = chains.next >= 0 && (chains.nextChain >= 0 ? chains.nextChain == shownChain : runs);
    const bool blink = std::fmod(time_, 0.8) < 0.45;
    if (gLook) {
        frame(dl, at, ImVec2(at.x + w, at.y + h), px(29), gLook == 1 ? kNeon : chColor);
    } else {
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), T::kPanel, px(8));
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + px(29)), T::kPanelHeader, px(8), ImDrawFlags_RoundCornersTop);
        dl->AddRect(at, ImVec2(at.x + w, at.y + h), T::kBorder, px(8), px(1));
    }
    const float hy = at.y + px(15);
    text(dl, gBold, px(11), at.x + px(12), hy, T::kText, "CHAIN", H::Left, V::Middle);
    const float titleX = at.x + px(16) + textWidth(gBold, px(11), "CHAIN");
    text(dl, gBold, px(11), titleX, hy, chColor, channelTitle(ch), H::Left, V::Middle);
    {  // the chain shown: its letter and name
        const std::string& name = chainNames_[c][static_cast<std::size_t>(shownChain)];
        const std::string which = "\xC2\xB7  " + chainLetter(shownChain) + (name.empty() ? "" : "  " + name);
        text(dl, gBold, px(11), titleX + textWidth(gBold, px(11), channelTitle(ch)) + px(8), hy, T::kCyan, which, H::Left, V::Middle);
    }
    // CHAIN ON / OFF, and what plays.
    const float bw = px(84), bx = at.x + w - px(150) - bw;
    const ImVec2 ba(bx, at.y + px(5)), bb(bx + bw, at.y + px(25));
    ImGui::SetCursorScreenPos(ba);
    ImGui::PushID("chain");
    if (ImGui::InvisibleButton("##onoff", ImVec2(bw, bb.y - ba.y)))
        action = runs ? SessionAction{SessionAction::Kind::ChainStop, ch} : SessionAction{SessionAction::Kind::ChainGo, ch, cursor >= 0 && cursor < Engine::kChainSteps ? cursor : 0};
    const bool onHovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip(runs      ? "Stops the chain (the clip plays on)"
                          : anyRuns ? "Plays this chain instead, at the end of the turn"
                                    : "Plays the chain from its first step (or the step under the cursor)");
    dl->AddRectFilled(ba, bb, runs ? alpha(T::kGreen, 0.22f) : T::kControl, px(5));
    dl->AddRect(ba, bb, runs ? T::kGreen : onHovered ? T::kControlBorder : T::kBorder, px(5), px(1));
    if (runs)
        dl->AddRectFilled(ImVec2(ba.x + px(9), at.y + px(11)), ImVec2(ba.x + px(17), at.y + px(19)), T::kText, px(1));
    else
        dl->AddTriangleFilled(ImVec2(ba.x + px(9), at.y + px(10)), ImVec2(ba.x + px(9), at.y + px(20)), ImVec2(ba.x + px(17), at.y + px(15)), T::kGreen);
    text(dl, gBold, px(9.5f), ba.x + px(23), hy, T::kText, runs ? "CHAIN ON" : "CHAIN OFF", H::Left, V::Middle);
    if (anyRuns) {  // what plays: this chain's step, or another chain's
        const int repeats = chains.list[static_cast<std::size_t>(chains.playing)].repeats[static_cast<std::size_t>(chains.at)];
        std::string state = (runs ? "" : "chain " + chainLetter(chains.playing) + "  \xC2\xB7  ") + "step " + std::to_string(chains.at + 1) +
                            "  \xC2\xB7  turn " + std::to_string(chains.turns + 1);
        if (repeats > 0)
            state += " of " + std::to_string(repeats);
        if (chains.next >= 0)
            state += "  \xC2\xB7  next: " +
                     (chains.nextChain >= 0 && chains.nextChain != chains.playing ? "chain " + chainLetter(chains.nextChain) + " " : std::string()) +
                     "step " + std::to_string(chains.next + 1);
        text(dl, gMedium, px(10.5f), bx - px(10), hy, T::kGreen, state, H::Right, V::Middle);
    }
    // The steps, an arrow between them, then the end.
    const float gap = px(22), endW = px(84);
    const float stepW = (w - px(24) - gap * static_cast<float>(Engine::kChainSteps) - endW) / static_cast<float>(Engine::kChainSteps);
    const float y0 = at.y + px(30), y1 = at.y + h - px(8);
    const ImGuiIO& io = ImGui::GetIO();
    // An empty step before the last that plays (or is silent) is a bar of silence.
    int lastFilled = -1;
    for (int q = 0; q < Engine::kChainSteps; ++q) {
        const int cq = chain.clip[static_cast<std::size_t>(q)];
        bool anyClip = false;
        for (int slot = 0; slot < Engine::kClipSlots && cq == Engine::kChainRandom; ++slot)
            anyClip = anyClip || live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(slot)].sources != 0;
        if (cq == Engine::kChainSilence || anyClip ||
            (cq >= 0 && cq < Engine::kClipSlots && live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(cq)].sources != 0))
            lastFilled = q;
    }
    for (int k = 0; k < Engine::kChainSteps; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        const float x0 = at.x + px(12) + static_cast<float>(k) * (stepW + gap), x1 = x0 + stepW;
        const ImVec2 a(x0, y0), b(x1, y1);
        const int clipSlot = chain.clip[kk];
        const int repeats = chain.repeats[kk];
        ImGui::PushID(k);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##step", ImVec2(stepW, y1 - y0)))
            action = SessionAction{SessionAction::Kind::ChainCursor, ch, k};
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::BeginDragDropTarget()) {  // a clip dragged from the grid
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MIDIPAD_CLIP")) {
                const int* v = static_cast<const int*>(p->Data);
                if (v[0] == ch)
                    action = SessionAction{SessionAction::Kind::ChainSet, ch, k, v[1], clipSlot >= 0 ? repeats : 1};
            }
            ImGui::EndDragDropTarget();
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::Step, ch, k}, ImGui::GetIO().MousePos};
        if (hovered && io.MouseWheel != 0.0f) {  // over its number: the clip; over its chip: the repeats
            const int notch = io.MouseWheel > 0 ? 1 : -1;
            if (io.MousePos.x < x0 + stepW * 0.45f || clipSlot < 0)
                action = SessionAction{SessionAction::Kind::ChainSet, ch, k, std::clamp(clipSlot + notch, -1, Engine::kChainSilence),
                                       clipSlot >= 0 ? repeats : 1};
            else
                action = SessionAction{SessionAction::Kind::ChainSet, ch, k, clipSlot, nextRepeats(repeats, notch)};
        }
        ImGui::PopID();
        const bool playsNow = runs && chains.at == k;
        const bool isNext = nextHere && chains.next == k;
        if (clipSlot == Engine::kChainSilence) {  // a silence step: a rest, its bars
            constexpr ImU32 kRest = IM_COL32(0x78, 0x82, 0x96, 255);
            screen(dl, a, b, kRest, [&] {
                const ImU32 ink = playsNow ? T::kText : T::kLabel;
                const float rx = x0 + px(24), ry = y0 + px(14), u = px(4);  // a quarter rest
                const ImVec2 rest[] = {{rx, ry}, {rx + u * 1.6f, ry + u * 2.0f}, {rx + u * 0.2f, ry + u * 3.6f}, {rx + u * 1.8f, ry + u * 5.8f},
                                       {rx + u * 0.4f, ry + u * 5.4f}, {rx + u * 1.2f, ry + u * 7.4f}};
                dl->AddPolyline(rest, 6, ink, px(2.2f));
                text(dl, gBold, px(7.5f), x0 + px(12), y1 - px(10), T::kDim, "SILENCE", H::Left, V::Middle);
                const std::string label = playsNow && repeats > 0 ? std::to_string(chains.turns + 1) + "/" + std::to_string(repeats)
                                          : repeats == 0          ? "\xE2\x88\x9E"
                                                                  : "\xC3\x97" + std::to_string(repeats);
                const float size = repeats == 0 && !playsNow ? px(17) : px(13);
                const float tw = std::max(textWidth(gBold, size, label) + px(18), px(34));
                const ImVec2 ca(x1 - px(5) - tw, y0 + px(4)), cb(x1 - px(5), y0 + px(27));
                const ImU32 chip = playsNow ? T::kGreen : kRest;
                dl->AddRectFilled(ca, cb, alpha(chip, 0.22f), px(11));
                dl->AddRect(ca, cb, alpha(chip, 0.85f), px(11), px(1.2f));
                text(dl, gBold, size, (ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f, T::kText, label, H::Center, V::Middle);
                if (repeats > 0)
                    text(dl, gMedium, px(10), x1 - px(10), y1 - px(12), T::kDim, repeats == 1 ? "1 bar" : std::to_string(repeats) + " bars", H::Right,
                         V::Middle);
                if (playsNow) {  // how far in its bar
                    const float p = std::clamp(live.clipProgress[c], 0.0f, 1.0f);
                    const ImVec2 ta(x0 + px(6), y1 - px(6.5f)), tb(x0 + stepW - px(6), y1 - px(3));
                    dl->AddRectFilled(ta, tb, IM_COL32(0, 0, 0, 170), px(2));
                    dl->AddRectFilled(ta, ImVec2(ta.x + (tb.x - ta.x) * p, tb.y), T::kGreen, px(2));
                }
            });
            const ImU32 outline = playsNow ? T::kGreen : isNext ? alpha(T::kGreen, blink ? 0.9f : 0.3f) : hovered ? T::kControlBorder : 0;
            if (outline)
                dl->AddRect(a, b, outline, px(4), px(playsNow || isNext ? 2.0f : 1.0f));
        } else if (clipSlot < 0) {
            // Empty: a dashed outline and a +; before the last step that plays, a bar of silence.
            const ImU32 dash = hovered ? T::kControlBorder : IM_COL32(0x4A, 0x4F, 0x57, 255);
            for (float t = 0; t < stepW; t += px(8)) {
                dl->AddLine(ImVec2(x0 + t, y0), ImVec2(std::min(x0 + t + px(4), x1), y0), dash, px(1));
                dl->AddLine(ImVec2(x0 + t, y1), ImVec2(std::min(x0 + t + px(4), x1), y1), dash, px(1));
            }
            for (float t = 0; t < y1 - y0; t += px(8)) {
                dl->AddLine(ImVec2(x0, y0 + t), ImVec2(x0, std::min(y0 + t + px(4), y1)), dash, px(1));
                dl->AddLine(ImVec2(x1, y0 + t), ImVec2(x1, std::min(y0 + t + px(4), y1)), dash, px(1));
            }
            const bool bar = k < lastFilled;
            text(dl, gBold, px(16), (x0 + x1) * 0.5f, (y0 + y1) * 0.5f - (bar ? px(6) : 0.0f), IM_COL32(0x5A, 0x5F, 0x67, 255), "+", H::Center,
                 V::Middle);
            if (bar)
                text(dl, gMedium, px(10), (x0 + x1) * 0.5f, y1 - px(14), playsNow ? T::kGreen : IM_COL32(0x6A, 0x70, 0x78, 255), "1 bar", H::Center,
                     V::Middle);
            if (playsNow) {  // its bar of silence, playing
                const float p = std::clamp(live.clipProgress[c], 0.0f, 1.0f);
                const ImVec2 ta(x0 + px(6), y1 - px(6.5f)), tb(x1 - px(6), y1 - px(3));
                dl->AddRectFilled(ta, tb, IM_COL32(0, 0, 0, 170), px(2));
                dl->AddRectFilled(ta, ImVec2(ta.x + (tb.x - ta.x) * p, tb.y), T::kGreen, px(2));
                dl->AddRect(a, b, T::kGreen, px(4), px(2.0f));
            }
        } else {
            // A random step: R, and while it plays the clip it picked.
            const bool random = clipSlot == Engine::kChainRandom;
            const int shown = random ? (playsNow ? chains.clipNow : -1) : clipSlot;
            static const LiveState::Clip kNone{};
            const LiveState::Clip& clip = shown >= 0 ? live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(shown)] : kNone;
            const ImU32 outline = playsNow ? T::kGreen : isNext ? alpha(T::kGreen, blink ? 0.9f : 0.3f) : hovered ? T::kControlBorder : 0;
            screen(dl, a, b, random ? T::kViolet : chColor, [&] {  // a screen, its state around it
            dl->AddRectFilled(ImVec2(x0 + px(2), y0 + px(6)), ImVec2(x0 + px(5), y1 - px(6)), chColor, px(1));
            const ImU32 kViolet = T::kViolet;
            if (random) {
                text(dl, gBold, px(26), x0 + px(12), y0 + px(19), kViolet, "R", H::Left, V::Middle);
                text(dl, gBold, px(7.5f), x0 + px(12), y1 - px(10), T::kDim,
                     shown >= 0 ? "CLIP " + std::to_string(shown + 1) : std::string("RANDOM"), H::Left, V::Middle);
                if (shown < 0) {  // two dice, for any clip
                    const float dx = x0 + px(44), dy = y0 + px(40), ds = px(16);
                    for (int d = 0; d < 2; ++d) {
                        const float ox = dx + static_cast<float>(d) * px(22), oy = dy + static_cast<float>(d) * px(6);
                        dl->AddRect(ImVec2(ox, oy), ImVec2(ox + ds, oy + ds), alpha(kViolet, 0.8f), px(3), px(1.3f));
                        const int pips = d == 0 ? 3 : 5;
                        static constexpr float kPip[6][2] = {{0.27f, 0.27f}, {0.73f, 0.73f}, {0.5f, 0.5f}, {0.73f, 0.27f}, {0.27f, 0.73f}, {0, 0}};
                        static constexpr int kThree[3] = {0, 2, 1}, kFive[5] = {0, 1, 2, 3, 4};
                        for (int q = 0; q < pips; ++q) {
                            const auto* pp = kPip[pips == 3 ? kThree[q] : kFive[q]];
                            dl->AddCircleFilled(ImVec2(ox + ds * pp[0], oy + ds * pp[1]), px(1.4f), kViolet);
                        }
                    }
                }
            } else {
                text(dl, gBold, px(26), x0 + px(12), y0 + px(19), clip.sources ? T::kText : T::kDim, std::to_string(clipSlot + 1), H::Left, V::Middle);
                text(dl, gBold, px(7.5f), x0 + px(12), y1 - px(10), T::kDim, clip.sources ? "CLIP" : "EMPTY", H::Left, V::Middle);
            }
            // A sketch of the clip: its notes, its liveliest curve.
            const float sx0 = x0 + px(38), sx1 = x1 - px(8), sy0 = y0 + px(31), sy1 = y1 - px(10);
            if (clip.noteCount > 0 && sx1 > sx0) {
                int lo = 127, hi = 0;
                for (int i = 0; i < clip.noteCount; ++i) {
                    lo = std::min<int>(lo, clip.notes[static_cast<std::size_t>(i)][2]);
                    hi = std::max<int>(hi, clip.notes[static_cast<std::size_t>(i)][2]);
                }
                for (int i = 0; i < clip.noteCount; ++i) {
                    const auto& n = clip.notes[static_cast<std::size_t>(i)];
                    const float nx0 = sx0 + (sx1 - sx0) * static_cast<float>(n[0]) / 255.0f;
                    const float nx1 = std::max(sx0 + (sx1 - sx0) * static_cast<float>(n[1]) / 255.0f, nx0 + px(2));
                    const float t = hi > lo ? static_cast<float>(n[2] - lo) / static_cast<float>(hi - lo) : 0.5f;
                    const float ny = sy1 - px(2) - t * (sy1 - sy0 - px(2));
                    dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(nx1, ny + px(1.8f)), IM_COL32(0xDC, 0xDE, 0xE2, 200));
                }
            }
            for (int m = 0; m < 4 && sx1 > sx0; ++m) {
                if (!(clip.sources & ~(1u << static_cast<int>(LoopSource::Notes))))
                    break;
                bool any = false;
                for (auto v : clip.curves[static_cast<std::size_t>(m)])
                    any = any || v != 0;
                if (!any)
                    continue;
                ImVec2 pts[LiveState::Clip::kCurvePoints];
                for (int i = 0; i < LiveState::Clip::kCurvePoints; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(LiveState::Clip::kCurvePoints - 1);
                    const float v = static_cast<float>(clip.curves[static_cast<std::size_t>(m)][static_cast<std::size_t>(i)]) / 255.0f;
                    pts[i] = ImVec2(sx0 + t * (sx1 - sx0), sy1 - v * (sy1 - sy0));
                }
                dl->AddPolyline(pts, LiveState::Clip::kCurvePoints, alpha(chColor, 0.85f), px(1.4f));
                break;
            }
            // Its repeats (playing: the turn), top right.
            const std::string label = playsNow && repeats > 0 ? std::to_string(chains.turns + 1) + "/" + std::to_string(repeats)
                                      : repeats == 0          ? "\xE2\x88\x9E"
                                                              : "\xC3\x97" + std::to_string(repeats);
            const ImU32 chip = playsNow ? T::kGreen : chColor;
            const float size = repeats == 0 && !playsNow ? px(17) : px(13);  // the infinity sign reads small
            const float tw = std::max(textWidth(gBold, size, label) + px(18), px(34));
            const ImVec2 ca(x1 - px(5) - tw, y0 + px(4)), cb(x1 - px(5), y0 + px(27));
            dl->AddRectFilled(ca, cb, alpha(chip, 0.22f), px(11));
            dl->AddRect(ca, cb, alpha(chip, 0.85f), px(11), px(1.2f));
            text(dl, gBold, size, (ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f, T::kText, label, H::Center, V::Middle);
            if (playsNow) {  // how far in its turn
                const float p = std::clamp(live.clipProgress[c], 0.0f, 1.0f);
                const ImVec2 ta(x0 + px(6), y1 - px(6.5f)), tb(x0 + stepW - px(6), y1 - px(3));  // a dark track under it
                dl->AddRectFilled(ta, tb, IM_COL32(0, 0, 0, 170), px(2));
                dl->AddRectFilled(ta, ImVec2(ta.x + (tb.x - ta.x) * p, tb.y), T::kGreen, px(2));
                dl->AddTriangleFilled(ImVec2(x1 - px(16), y1 - px(18)), ImVec2(x1 - px(16), y1 - px(8)), ImVec2(x1 - px(8), y1 - px(13)), T::kGreen);
            }
            });
            if (outline)
                dl->AddRect(a, b, outline, px(4), px(playsNow || isNext ? 2.0f : 1.0f));
        }
        if (cursor == k)
            sessionCursorBox_ = {a, b};
        if (cursor == k)
            dl->AddRect(ImVec2(x0 - px(3), y0 - px(3)), ImVec2(x1 + px(3), y1 + px(3)), T::kCyan, px(8), px(2.5f));
        // The arrow to the next step (or the end).
        const float ax0 = x1 + px(4), ax1 = x1 + gap - px(4), ay = (y0 + y1) * 0.5f;
        dl->AddLine(ImVec2(ax0, ay), ImVec2(ax1 - px(3), ay), T::kLabel, px(1.6f));
        dl->AddTriangleFilled(ImVec2(ax1, ay), ImVec2(ax1 - px(6), ay - px(4)), ImVec2(ax1 - px(6), ay + px(4)), T::kLabel);
    }
    // The CHAIN cell: the chain shown, its letter between two arrows (or the
    // wheel) that show another; a dot per chain (green: playing, cyan: shown,
    // grey: has steps); what comes after its last step. A click plays or
    // stops it; a right-click opens its menu.
    const float ex0 = at.x + px(12) + static_cast<float>(Engine::kChainSteps) * (stepW + gap), ex1 = ex0 + endW;
    const float cx = (ex0 + ex1) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(ex0, y0));
    const bool cellClicked = ImGui::InvisibleButton("##chaincell", ImVec2(endW, y1 - y0));
    const bool endHovered = ImGui::IsItemHovered();
    // The letter left, the arrows stacked right of it, the dots and the then under them.
    const float letterX = ex0 + (endW - px(22)) * 0.5f, letterY = y0 + (y1 - y0) * 0.42f, arrowX = ex1 - px(14);
    if (cellClicked) {
        if (io.MousePos.x > arrowX - px(10))
            action = SessionAction{SessionAction::Kind::ChainShow, ch,
                                   (shownChain + (io.MousePos.y < letterY ? Engine::kChains - 1 : 1)) % Engine::kChains};
        else
            action = runs ? SessionAction{SessionAction::Kind::ChainStop, ch} : SessionAction{SessionAction::Kind::ChainGo, ch, 0};
    }
    if (endHovered && io.MouseWheel != 0.0f)
        action = SessionAction{SessionAction::Kind::ChainShow, ch, (shownChain + (io.MouseWheel > 0 ? Engine::kChains - 1 : 1)) % Engine::kChains};
    if (endHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::Chain, ch, shownChain}, io.MousePos};
    ImGui::PopID();
    screen(dl, ImVec2(ex0, y0), ImVec2(ex1, y1), runs ? T::kGreen : kScreenOff, [] {});
    if (runs || endHovered)
        dl->AddRect(ImVec2(ex0, y0), ImVec2(ex1, y1), runs ? T::kGreen : T::kControlBorder, px(4), px(runs ? 2.0f : 1.0f));
    text(dl, gBold, px(7.5f), letterX, y0 + px(8), T::kDim, "CHAIN", H::Center, V::Middle);
    const ImU32 arrowColor = endHovered ? T::kCyan : alpha(T::kCyan, 0.6f);
    dl->AddTriangleFilled(ImVec2(arrowX - px(5), letterY - px(4)), ImVec2(arrowX + px(5), letterY - px(4)), ImVec2(arrowX, letterY - px(10)), arrowColor);
    dl->AddTriangleFilled(ImVec2(arrowX - px(5), letterY + px(4)), ImVec2(arrowX + px(5), letterY + px(4)), ImVec2(arrowX, letterY + px(10)), arrowColor);
    text(dl, gBold, px(22), letterX, letterY, chain.empty() ? T::kDim : T::kText, chainLetter(shownChain), H::Center, V::Middle);
    for (int n = 0; n < Engine::kChains; ++n) {
        const float dx = cx + (static_cast<float>(n) - 3.5f) * px(8);
        const bool plays = anyRuns && chains.playing == n;
        const ImU32 dot = plays ? T::kGreen : n == shownChain ? T::kCyan : !chains.list[static_cast<std::size_t>(n)].empty() ? T::kLabel
                                                                                                                         : IM_COL32(0x3C, 0x40, 0x46, 255);
        dl->AddCircleFilled(ImVec2(dx, y1 - px(17)), px(2.5f), dot);
    }
    {  // then: a round arrow (back to step 1), a square (stop) or another chain's letter
        const int then = chain.then;
        const float ty = y1 - px(7);
        const float tw = textWidth(gMedium, px(8.5f), "then"), tx = cx - (tw + px(13)) * 0.5f;  // the word and its icon, centred
        const float ix = tx + tw + px(8);
        text(dl, gMedium, px(8.5f), tx, ty, T::kDim, "then", H::Left, V::Middle);
        if (then == Engine::kThenLoop) {
            dl->PathArcTo(ImVec2(ix, ty), px(4.5f), 0.7f, 5.8f, 16);
            dl->PathStroke(T::kLabel, px(1.5f));
            const ImVec2 tip(ix + px(4.5f) * std::cos(0.7f), ty + px(4.5f) * std::sin(0.7f));
            dl->AddTriangleFilled(ImVec2(tip.x - px(3), tip.y - px(1)), ImVec2(tip.x + px(2.5f), tip.y - px(2)), ImVec2(tip.x, tip.y + px(3)), T::kLabel);
        } else if (then == Engine::kThenStop) {
            dl->AddRectFilled(ImVec2(ix - px(3.5f), ty - px(3.5f)), ImVec2(ix + px(3.5f), ty + px(3.5f)), T::kLabel, px(1));
        } else {
            text(dl, gBold, px(9), ix, ty, T::kCyan, chainLetter(then), H::Center, V::Middle);
        }
    }
    if (cursor == Engine::kChainSteps)
        sessionCursorBox_ = {ImVec2(ex0, y0), ImVec2(ex1, y1)};
    if (cursor == Engine::kChainSteps)
        dl->AddRect(ImVec2(ex0 - px(3), y0 - px(3)), ImVec2(ex1 + px(3), y1 + px(3)), T::kCyan, px(8), px(2.5f));
    return action;
}

// --- The song screen ---------------------------------------------------------

double PerformanceView::songItemBeats(const LiveState& live, int channel, const midipad::SongItem& item, bool* open) {
    const auto c = static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1));
    const auto i = static_cast<std::size_t>(std::clamp(item.index, 0, Engine::kChains - 1));
    const bool chain = item.kind == midipad::SongItem::Kind::Chain;
    if (open)
        *open = chain && ((live.chainOpen[c] >> i) & 1u);
    if (item.kind == midipad::SongItem::Kind::Rest)
        return std::clamp(item.beats, 1, midipad::kMaxRestBeats);
    const double one = chain ? live.chainBeats[c][i] : live.clipBeats[c][std::min<std::size_t>(i, Engine::kClipSlots - 1)];
    return one * std::clamp(item.repeats, 1, midipad::kMaxSongRepeats);
}

// Where each item of a channel's cell starts, in bars into the section.
std::vector<double> PerformanceView::songItemBars(const LiveState& live, int channel, const std::vector<midipad::SongItem>& items) {
    auto starts = midipad::songItemStarts(items, live.beatsPerBar, [&](const midipad::SongItem& it) { return songItemBeats(live, channel, it); });
    for (double& v : starts)
        v /= std::max(live.beatsPerBar, 1);
    return starts;
}

std::string PerformanceView::restText(int beats, int beatsPerBar) {
    beatsPerBar = std::max(beatsPerBar, 1);
    const int bars = beats / beatsPerBar, rest = beats % beatsPerBar;
    std::string s;
    if (bars > 0)
        s = std::to_string(bars) + (bars == 1 ? " bar" : " bars");
    if (rest > 0)
        s += (s.empty() ? "" : " ") + std::to_string(rest) + (rest == 1 ? " beat" : " beats");
    return s;
}

int PerformanceView::songSectionBars(const LiveState& live, const midipad::SongSection& section) {
    return midipad::sectionBarsOf(section, live.beatsPerBar,
                                  [&live](int channel, const midipad::SongItem& item) { return songItemBeats(live, channel, item); });
}

std::vector<int> PerformanceView::songChannels(const LiveState& live, const Song& song) {
    std::vector<int> out;
    for (int ch = 0; ch < kChannelCount; ++ch) {
        const auto c = static_cast<std::size_t>(ch);
        bool used = std::any_of(live.clipBeats[c].begin(), live.clipBeats[c].end(), [](double b) { return b > 0; }) ||
                    std::any_of(live.chains[c].list.begin(), live.chains[c].list.end(), [](const LiveState::Chain& k) { return !k.empty(); });
        for (const SongSection& sec : song.sections)
            used = used || !sec.cells[c].empty();
        if (used)
            out.push_back(ch);
    }
    return out;
}

std::optional<PerformanceView::SongAction> PerformanceView::drawSong(const LiveState& live, const midipad::Song& song, const SongCursor& cursor) {
    using midipad::SongItem;
    using midipad::SongSection;
    gMedium = fonts_.medium ? fonts_.medium : ImGui::GetFont();
    gBold = fonts_.bold ? fonts_.bold : gMedium;
    gCondensed = fonts_.condensed ? fonts_.condensed : gBold;
    time_ = ImGui::GetTime();
    std::optional<SongAction> action;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetContentRegionAvail().y;
    constexpr int kColumns = 8;
    static constexpr ImU32 kSectionColors[] = {IM_COL32(0x78, 0x82, 0x96, 255), T::kBlue, T::kOrange, T::kPink, T::kViolet, T::kGreen, T::kCyan};
    constexpr ImU32 kViolet = T::kViolet;
    const int sections = static_cast<int>(song.sections.size());
    const double bar = std::max(live.beatsPerBar, 1);
    // The channels in use, eight at a time, each column and lane its usual size.
    const std::vector<int> shown = songChannels(live, song);
    const int shownCount = static_cast<int>(shown.size());
    if (cursor.col >= 0)
        if (const auto it = std::find(shown.begin(), shown.end(), cursor.col); it != shown.end())
            songFirst_ = static_cast<int>(it - shown.begin()) / kColumns * kColumns;
    songFirst_ = std::clamp(songFirst_, 0, std::max(0, (shownCount - 1) / kColumns * kColumns));
    const int count = std::min(kColumns, shownCount - songFirst_);
    auto channelAt = [&](int k) { return shown[static_cast<std::size_t>(songFirst_ + k)]; };
    // Where each section starts, in bars, and the whole song's length.
    std::vector<int> barsOf(static_cast<std::size_t>(sections)), startOf(static_cast<std::size_t>(sections) + 1, 0);
    for (int s = 0; s < sections; ++s) {
        barsOf[static_cast<std::size_t>(s)] = songSectionBars(live, song.sections[static_cast<std::size_t>(s)]);
        startOf[static_cast<std::size_t>(s) + 1] = startOf[static_cast<std::size_t>(s)] + barsOf[static_cast<std::size_t>(s)];
    }
    const int totalBars = std::max(startOf.back(), 1);
    const double secondsPerBar = bar * 60.0 / std::max(live.tempo, 1.0);
    auto clock = [](double seconds) {
        const int s = static_cast<int>(std::lround(std::max(seconds, 0.0)));
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
        return std::string(buf);
    };
    const bool playing = live.song.playing && live.song.section >= 0 && live.song.section < sections;
    const double playBars = playing ? startOf[static_cast<std::size_t>(live.song.section)] + std::max(live.song.beat, 0.0) / bar : 0.0;
    auto itemLabel = [&](int ch, const SongItem& it) {
        if (it.kind == SongItem::Kind::Rest)
            return "Silence  " + restText(it.beats, live.beatsPerBar);
        if (it.kind == SongItem::Kind::Clip)
            return "Clip " + std::to_string(it.index + 1);
        const std::string& name = chainNames_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(it.index)];
        return "Chain " + chainLetter(it.index) + (name.empty() ? "" : " " + name);
    };

    // --- The whole song, at scale ---
    const float topH = px(196);
    if (gLook) {
        frame(dl, origin, ImVec2(origin.x + width, origin.y + topH), px(38), gLook == 1 ? kNeon : kSongScreen);
    } else {
        dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + topH), T::kPanel, px(8));
        dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + px(38)), T::kPanelHeader, px(8), ImDrawFlags_RoundCornersTop);
        dl->AddRect(origin, ImVec2(origin.x + width, origin.y + topH), T::kBorder, px(8), px(1));
    }
    // The song at scale on a screen, as the modules show what they do.
    screen(dl, ImVec2(origin.x + px(8), origin.y + px(38)), ImVec2(origin.x + width - px(8), origin.y + topH - px(6)), kSongScreen, [] {});
    const float hy = origin.y + px(19);
    text(dl, gBold, px(16.5f), origin.x + px(12), hy, T::kText, "SONG", H::Left, V::Middle);
    std::string summary = std::to_string(sections) + (sections == 1 ? " section" : " sections") + "  \xC2\xB7  " + std::to_string(startOf.back()) +
                          " bars  \xC2\xB7  " + clock(startOf.back() * secondsPerBar);
    text(dl, gMedium, px(15.75f), origin.x + px(72), hy, T::kDim, summary, H::Left, V::Middle);
    // Loop the section (Play and Stop are the top bar's).
    const float lw = px(160), by0 = origin.y + px(5), by1 = origin.y + px(33);
    const float loopX = origin.x + width - px(10) - lw;
    ImGui::PushID("song");
    ImGui::SetCursorScreenPos(ImVec2(loopX, by0));
    if (ImGui::InvisibleButton("##loop", ImVec2(lw, by1 - by0)))
        action = SongAction{SongAction::Kind::Loop, 0, -1, 0, live.song.loop ? 0 : 1};
    ImGui::SetItemTooltip("The section playing plays again and again");
    dl->AddRectFilled(ImVec2(loopX, by0), ImVec2(loopX + lw, by1), live.song.loop ? alpha(T::kCyan, 0.2f) : T::kControl, px(5));
    dl->AddRect(ImVec2(loopX, by0), ImVec2(loopX + lw, by1), live.song.loop ? T::kCyan : T::kBorder, px(5), px(1));
    text(dl, gBold, px(14.25f), loopX + lw * 0.5f, hy, live.song.loop ? T::kText : T::kLabel, "LOOP SECTION", H::Center, V::Middle);
    // Loop the song: after its last section, its first again.
    const float songX = loopX - px(8) - lw;
    ImGui::SetCursorScreenPos(ImVec2(songX, by0));
    if (ImGui::InvisibleButton("##loopsong", ImVec2(lw, by1 - by0)))
        action = SongAction{SongAction::Kind::LoopSong, 0, -1, 0, song.loop ? 0 : 1};
    ImGui::SetItemTooltip("After its last section, the song plays again from its first");
    dl->AddRectFilled(ImVec2(songX, by0), ImVec2(songX + lw, by1), song.loop ? alpha(T::kCyan, 0.2f) : T::kControl, px(5));
    dl->AddRect(ImVec2(songX, by0), ImVec2(songX + lw, by1), song.loop ? T::kCyan : T::kBorder, px(5), px(1));
    text(dl, gBold, px(14.25f), songX + lw * 0.5f, hy, song.loop ? T::kText : T::kLabel, "LOOP SONG", H::Center, V::Middle);
    if (playing) {
        const SongSection& sec = song.sections[static_cast<std::size_t>(live.song.section)];
        std::string state = (sec.name.empty() ? "SECTION " + std::to_string(live.song.section + 1) : sec.name);
        if (live.song.beat < 0)
            state += "  \xC2\xB7  on the next bar";
        else
            state += "  \xC2\xB7  bar " + std::to_string(static_cast<int>(live.song.beat / bar) + 1) + " of " +
                     std::to_string(barsOf[static_cast<std::size_t>(live.song.section)]);
        state += "  \xC2\xB7  " + clock(playBars * secondsPerBar) + " / " + clock(startOf.back() * secondsPerBar);
        text(dl, gMedium, px(15.75f), songX - px(10), hy, T::kGreen, state, H::Right, V::Middle);
    }
    // The sections, at scale; a lane per channel shown, where it plays.
    const float sx0 = origin.x + px(100), sx1 = origin.x + width - px(12), sy0 = origin.y + px(45), sy1 = sy0 + px(28);
    auto xOf = [&](double bars) { return sx0 + static_cast<float>(bars / totalBars) * (sx1 - sx0); };
    for (int s = 0; s < sections; ++s) {
        const SongSection& sec = song.sections[static_cast<std::size_t>(s)];
        const ImU32 col = kSectionColors[s % 7];
        const ImVec2 a(xOf(startOf[static_cast<std::size_t>(s)]) + px(1), sy0), b(xOf(startOf[static_cast<std::size_t>(s) + 1]) - px(1), sy1);
        const bool now = playing && live.song.section == s;
        dl->AddRectFilled(a, b, alpha(col, 0.28f), px(4));
        dl->AddRect(a, b, now ? T::kGreen : s == cursor.row ? T::kCyan : alpha(col, 0.9f), px(4), px(now || s == cursor.row ? 2.0f : 1.0f));
        const std::string label = (sec.name.empty() ? "SECTION " + std::to_string(s + 1) : sec.name) + "  " + std::to_string(barsOf[static_cast<std::size_t>(s)]);
        if (b.x - a.x > px(24))
            text(dl, gBold, px(13.5f), a.x + px(5), (sy0 + sy1) * 0.5f, T::kText, fit(gBold, px(13.5f), label, b.x - a.x - px(8)), H::Left, V::Middle);
        ImGui::PushID(s);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##sec", ImVec2(std::max(b.x - a.x, 1.0f), sy1 - sy0)))
            action = SongAction{SongAction::Kind::Cursor, s, -1};
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            action = SongAction{SongAction::Kind::Play, s};
        ImGui::PopID();
    }
    const float laneH = px(11.5f), ly0 = sy1 + px(7);
    for (int k = 0; k < count; ++k) {
        const int ch = channelAt(k);
        const auto c = static_cast<std::size_t>(ch);
        const float y = ly0 + static_cast<float>(k) * (laneH + px(2));
        const ImU32 chColor = kChannelColors[ch % 8];
        text(dl, gBold, px(12), origin.x + px(12), y + laneH * 0.5f, chColor, fit(gBold, px(12), channelTitle(ch), px(82)), H::Left, V::Middle);
        dl->AddRectFilled(ImVec2(sx0, y), ImVec2(sx1, y + laneH), IM_COL32(0, 0, 0, 70), px(2));
        for (int s = 0; s < sections; ++s) {
            const auto& items = song.sections[static_cast<std::size_t>(s)].cells[c];
            const double end = startOf[static_cast<std::size_t>(s) + 1];
            const auto st = songItemBars(live, ch, items);
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].kind == SongItem::Kind::Rest)
                    continue;  // a silence: nothing plays
                bool open = false;
                const double at = startOf[static_cast<std::size_t>(s)] + st[i];
                const double next = i + 1 < items.size() ? startOf[static_cast<std::size_t>(s)] + st[i + 1] : end;
                const double played = std::min(at + songItemBeats(live, ch, items[i], &open) / bar, std::min(next, end));
                const ImU32 col = items[i].kind == SongItem::Kind::Chain ? kViolet : chColor;
                if (played > at)
                    dl->AddRectFilled(ImVec2(xOf(at) + px(0.5f), y), ImVec2(std::max(xOf(played) - px(0.5f), xOf(at) + px(2)), y + laneH), alpha(col, 0.85f), px(2));
                if ((items[i].loop || open) && next > played)  // on until the next item: hollow
                    dl->AddRect(ImVec2(xOf(played), y + px(0.5f)), ImVec2(xOf(std::min(next, end)) - px(0.5f), y + laneH - px(0.5f)), alpha(col, 0.6f), px(2), px(1));
            }
        }
    }
    if (playing) {
        const float x = xOf(playBars);
        dl->AddLine(ImVec2(x, sy0 - px(3)), ImVec2(x, ly0 + static_cast<float>(std::max(count, 1)) * (laneH + px(2))), T::kGreen, px(1.6f));
        dl->AddTriangleFilled(ImVec2(x - px(5), sy0 - px(6)), ImVec2(x + px(5), sy0 - px(6)), ImVec2(x, sy0), T::kGreen);
    }

    // --- The sections, a row each; a column per channel ---
    const float editorH = px(205), gy0 = origin.y + topH + px(6), gy1 = origin.y + height - editorH - px(6);
    const float nameW = px(160), headH = px(35), rowH = px(70), gap = px(6);
    if (gLook) {
        frame(dl, ImVec2(origin.x, gy0), ImVec2(origin.x + width, gy1), headH, gLook == 1 ? kNeon : kSongScreen);
    } else {
        dl->AddRectFilled(ImVec2(origin.x, gy0), ImVec2(origin.x + width, gy1), T::kPanel, px(8));
        dl->AddRectFilled(ImVec2(origin.x, gy0), ImVec2(origin.x + width, gy0 + headH), T::kPanelHeader, px(8), ImDrawFlags_RoundCornersTop);
        dl->AddRect(ImVec2(origin.x, gy0), ImVec2(origin.x + width, gy1), T::kBorder, px(8), px(1));
    }
    const float cx0 = origin.x + px(8) + nameW + gap, colW = (origin.x + width - px(8) - cx0 - gap * (kColumns - 1)) / kColumns;
    for (int k = 0; k < count; ++k) {
        const int ch = channelAt(k);
        const float x = cx0 + static_cast<float>(k) * (colW + gap);
        text(dl, gBold, px(15), x + px(4), gy0 + px(18), kChannelColors[ch % 8], fit(gBold, px(15), channelTitle(ch), colW - px(8)), H::Left, V::Middle);
        // Its header: a double-click renames the channel, a right-click opens its menu.
        ImGui::PushID(ch);
        ImGui::SetCursorScreenPos(ImVec2(x, gy0));
        ImGui::InvisibleButton("##songhead", ImVec2(colW, headH));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            renameRequest_ = ch;
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::Channel, ch, 1}, ImGui::GetIO().MousePos};
        ImGui::PopID();
    }
    if (shownCount > kColumns) {  // more than eight: the other page's channels
        const int otherFirst = songFirst_ == 0 ? kColumns : 0, otherLast = songFirst_ == 0 ? shownCount - 1 : kColumns - 1;
        const std::string range = std::to_string(shown[static_cast<std::size_t>(otherFirst)] + 1) + "\xE2\x80\x93" +
                                  std::to_string(shown[static_cast<std::size_t>(otherLast)] + 1);
        const std::string other = songFirst_ == 0 ? "Channels " + range + " \xE2\x80\xBA" : "\xE2\x80\xB9 Channels " + range;
        const float tw = textWidth(gMedium, px(14.25f), other);
        const ImVec2 a(origin.x + px(10), gy0 + px(7));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##page", ImVec2(tw + px(6), px(22))))
            action = SongAction{SongAction::Kind::Cursor, std::max(cursor.row, 0), shown[static_cast<std::size_t>(otherFirst)]};
        text(dl, gMedium, px(14.25f), a.x, gy0 + px(18), ImGui::IsItemHovered() ? T::kText : T::kDim, other, H::Left, V::Middle);
    } else if (shownCount > 0) {
        text(dl, gMedium, px(14.25f), origin.x + px(10), gy0 + px(18), T::kDim, std::to_string(shownCount) + (shownCount == 1 ? " channel" : " channels"),
             H::Left, V::Middle);
    }
    const int visible = std::max(static_cast<int>((gy1 - gy0 - headH - px(11)) / (rowH + px(4))), 1);
    if (cursor.row < songScroll_)
        songScroll_ = cursor.row;
    if (cursor.row >= songScroll_ + visible)
        songScroll_ = cursor.row - visible + 1;
    songScroll_ = std::clamp(songScroll_, 0, std::max(sections + 1 - visible, 0));
    const ImVec2 clipA(origin.x, gy0 + headH), clipB(origin.x + width, gy1 - px(4));
    if (ImGui::IsMouseHoveringRect(clipA, clipB) && io.MouseWheel != 0.0f && !io.KeyCtrl)
        songScroll_ = std::clamp(songScroll_ - (io.MouseWheel > 0 ? 1 : -1), 0, std::max(sections + 1 - visible, 0));
    dl->PushClipRect(clipA, clipB, true);
    for (int r = songScroll_; r <= sections && r < songScroll_ + visible; ++r) {
        const float y = gy0 + headH + px(5) + static_cast<float>(r - songScroll_) * (rowH + px(4));  // room for the cursor's frame
        const ImVec2 na(origin.x + px(8), y), nb(origin.x + px(8) + nameW, y + rowH);
        ImGui::PushID(r);
        if (r == sections) {  // + section
            const ImU32 dash = IM_COL32(0x5A, 0x5F, 0x67, 255);
            for (float t = 0; t < nameW; t += px(8))
                dl->AddLine(ImVec2(na.x + t, na.y), ImVec2(std::min(na.x + t + px(4), nb.x), na.y), dash, px(1));
            for (float t = 0; t < nameW; t += px(8))
                dl->AddLine(ImVec2(na.x + t, na.y + px(46)), ImVec2(std::min(na.x + t + px(4), nb.x), na.y + px(46)), dash, px(1));
            text(dl, gMedium, px(15.75f), (na.x + nb.x) * 0.5f, na.y + px(23), T::kLabel, "+ section", H::Center, V::Middle);
            ImGui::SetCursorScreenPos(na);
            if (ImGui::InvisibleButton("##add", ImVec2(nameW, px(46))))
                action = SongAction{SongAction::Kind::AddSection, r};
            if (cursor.row == r) {
                dl->AddRect(ImVec2(na.x - px(3), na.y - px(3)), ImVec2(nb.x + px(3), na.y + px(49)), T::kCyan, px(8), px(2.5f));
                sessionCursorBox_ = {na, ImVec2(nb.x, na.y + px(46))};
            }
            ImGui::PopID();
            break;
        }
        const SongSection& sec = song.sections[static_cast<std::size_t>(r)];
        const int bars = barsOf[static_cast<std::size_t>(r)];
        const bool now = playing && live.song.section == r;
        // Its name, length, and a play sign.
        screen(dl, na, nb, kSectionColors[r % 7], [] {});  // a screen in the section's colour
        if (now)
            dl->AddRect(na, nb, T::kGreen, px(4), px(2));
        dl->AddRectFilled(ImVec2(na.x + px(2), na.y + px(7)), ImVec2(na.x + px(5), nb.y - px(7)), kSectionColors[r % 7], px(1));
        text(dl, gBold, px(16.5f), na.x + px(12), na.y + px(17), T::kText, fit(gBold, px(16.5f), sec.name.empty() ? "SECTION " + std::to_string(r + 1) : sec.name, nameW - px(36)),
             H::Left, V::Middle);
        text(dl, gMedium, px(15), na.x + px(12), na.y + px(38), T::kLabel, std::to_string(bars) + (bars == 1 ? " bar" : " bars"), H::Left, V::Middle);
        text(dl, gMedium, px(12.75f), na.x + px(12), na.y + px(55), T::kDim, sec.fit ? "= longest" : "fixed", H::Left, V::Middle);
        const ImVec2 pc(nb.x - px(18), na.y + px(17));
        ImGui::SetCursorScreenPos(ImVec2(na.x, na.y));
        const bool nameClicked = ImGui::InvisibleButton("##name", ImVec2(nameW, rowH));
        const bool nameHovered = ImGui::IsItemHovered();
        if (nameClicked)
            action = io.MousePos.x > pc.x - px(12) && io.MousePos.y < pc.y + px(12)
                         ? (now ? SongAction{SongAction::Kind::Stop} : SongAction{SongAction::Kind::Play, r})
                         : SongAction{SongAction::Kind::Cursor, r, -1};
        if (nameHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::Section, 0, r}, io.MousePos};
        if (nameHovered && io.MouseWheel != 0.0f && io.KeyCtrl)
            action = SongAction{SongAction::Kind::Bars, r, -1, 0, io.MouseWheel > 0 ? 1 : -1};
        if (now)
            dl->AddRectFilled(ImVec2(pc.x - px(5), pc.y - px(5)), ImVec2(pc.x + px(5), pc.y + px(5)), T::kGreen, px(1));
        else
            dl->AddTriangleFilled(ImVec2(pc.x - px(4), pc.y - px(6)), ImVec2(pc.x - px(4), pc.y + px(6)), ImVec2(pc.x + px(6), pc.y),
                                  nameHovered ? T::kGreen : alpha(T::kGreen, 0.55f));
        if (cursor.row == r && cursor.col < 0) {
            dl->AddRect(ImVec2(na.x - px(3), na.y - px(3)), ImVec2(nb.x + px(3), nb.y + px(3)), T::kCyan, px(8), px(2.5f));
            sessionCursorBox_ = {na, nb};
        }
        // Its cells: each item at scale in the section, hollow on to the next while it loops; red where the section cuts it.
        for (int k = 0; k < count; ++k) {
            const int ch = channelAt(k);
            const auto c = static_cast<std::size_t>(ch);
            const float x = cx0 + static_cast<float>(k) * (colW + gap);
            const ImVec2 a(x, y), b(x + colW, y + rowH);
            const auto& items = sec.cells[c];
            const bool here = cursor.row == r && cursor.col == ch;
            if (items.empty()) {
                dl->AddRectFilled(a, b, IM_COL32(0x26, 0x29, 0x2E, 255), px(6));
            } else {  // a screen in the channel's colour
                screen(dl, a, b, kChannelColors[ch % 8], [] {});
                if (now)
                    dl->AddRect(a, b, alpha(T::kGreen, 0.8f), px(4), px(1));
            }
            const float ix0 = a.x + px(6), ix1 = b.x - px(6), barW = (ix1 - ix0) / static_cast<float>(bars);
            const ImU32 chColor = kChannelColors[ch % 8];
            const auto st = songItemBars(live, ch, items);
            for (std::size_t i = 0; i < items.size(); ++i) {
                const SongItem& it = items[i];
                bool open = false;
                const double len = songItemBeats(live, ch, it, &open) / bar;
                const double next = i + 1 < items.size() ? st[i + 1] : bars;
                const double end = std::min(st[i] + len, std::min(next, static_cast<double>(bars)));
                const ImU32 col = it.kind == SongItem::Kind::Chain ? kViolet : chColor;
                const float x0 = ix0 + static_cast<float>(st[i]) * barW, x1 = std::max(ix0 + static_cast<float>(end) * barW, x0 + px(3));
                if (st[i] >= bars)
                    continue;
                if (it.kind == SongItem::Kind::Rest) {  // a silence: a faint outline
                    dl->AddRect(ImVec2(x0 + px(0.5f), a.y + px(7)), ImVec2(x1 - px(0.5f), b.y - px(7)), IM_COL32(0x5A, 0x5F, 0x67, 255), px(3), px(1));
                    continue;
                }
                dl->AddRectFilled(ImVec2(x0 + px(0.5f), a.y + px(7)), ImVec2(x1 - px(0.5f), b.y - px(7)), alpha(col, 0.3f), px(3));
                dl->AddRect(ImVec2(x0 + px(0.5f), a.y + px(7)), ImVec2(x1 - px(0.5f), b.y - px(7)), col, px(3), px(1));
                if ((it.loop || open) && next > end)
                    dl->AddLine(ImVec2(x1, (a.y + b.y) * 0.5f), ImVec2(ix0 + static_cast<float>(std::min(next, static_cast<double>(bars))) * barW - px(1), (a.y + b.y) * 0.5f),
                                alpha(col, 0.7f), px(1.5f));
                if (st[i] + len > bars + 1e-6 && next >= bars)
                    dl->AddLine(ImVec2(ix1 + px(2), a.y + px(4)), ImVec2(ix1 + px(2), b.y - px(4)), T::kRed, px(2));
                const std::string short_ = (it.kind == SongItem::Kind::Clip ? "Clip " + std::to_string(it.index + 1) : "Chain " + chainLetter(it.index)) +
                                           (it.repeats > 1 ? " \xC3\x97" + std::to_string(it.repeats) : std::string());
                if (x1 - x0 > px(14))
                    text(dl, gBold, px(13.5f), x0 + px(5), a.y + px(21), T::kText, fit(gBold, px(13.5f), short_, x1 - x0 - px(6)), H::Left, V::Middle);
            }
            if (now && !items.empty()) {  // the playhead in the section
                const float px_ = ix0 + static_cast<float>(std::clamp(live.song.beat / bar, 0.0, static_cast<double>(bars))) * barW;
                dl->AddLine(ImVec2(px_, a.y + px(3)), ImVec2(px_, b.y - px(3)), T::kGreen, px(1.3f));
            }
            ImGui::PushID(ch);
            ImGui::SetCursorScreenPos(a);
            if (ImGui::InvisibleButton("##cell", ImVec2(colW, rowH)))
                action = SongAction{SongAction::Kind::EditCell, r, ch};
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::SongCell, ch, r}, io.MousePos};
            ImGui::PopID();
            if (here) {
                dl->AddRect(ImVec2(a.x - px(3), a.y - px(3)), ImVec2(b.x + px(3), b.y + px(3)), cursor.editing ? alpha(T::kCyan, 0.5f) : T::kCyan, px(8), px(2.5f));
                if (!cursor.editing)
                    sessionCursorBox_ = {a, b};
            }
        }
        ImGui::PopID();
    }
    dl->PopClipRect();

    // --- The editor of the cell under the cursor ---
    const float ey0 = origin.y + height - editorH, ey1 = origin.y + height;
    if (gLook) {
        // Its frame alone (no rule under a title while no cell is chosen); the cyan one while editing.
        const bool chosen = cursor.col >= 0 && cursor.row < sections;
        frame(dl, ImVec2(origin.x, ey0), ImVec2(origin.x + width, ey1), chosen ? px(37) : 0.0f, gLook == 1 ? kNeon : kSongScreen);
        if (cursor.editing)
            dl->AddRect(ImVec2(origin.x, ey0), ImVec2(origin.x + width, ey1), T::kCyan, px(8), px(2.0f));
    } else {
        dl->AddRectFilled(ImVec2(origin.x, ey0), ImVec2(origin.x + width, ey1), T::kPanel, px(8));
        dl->AddRectFilled(ImVec2(origin.x, ey0), ImVec2(origin.x + width, ey0 + px(37)), T::kPanelHeader, px(8), ImDrawFlags_RoundCornersTop);
        dl->AddRect(ImVec2(origin.x, ey0), ImVec2(origin.x + width, ey1), cursor.editing ? T::kCyan : T::kBorder, px(8), px(cursor.editing ? 2.0f : 1.0f));
    }
    if (cursor.col < 0 || cursor.row >= sections) {  // no cell chosen: an empty editor
        ImGui::PopID();
        return action;
    }
    const int r = cursor.row, ch = cursor.col;
    const SongSection& sec = song.sections[static_cast<std::size_t>(r)];
    const auto& items = sec.cells[static_cast<std::size_t>(ch)];
    const int bars = barsOf[static_cast<std::size_t>(r)];
    const ImU32 chColor = kChannelColors[ch % 8];
    {
        const std::string title = (sec.name.empty() ? "SECTION " + std::to_string(r + 1) : sec.name);
        text(dl, gBold, px(16.5f), origin.x + px(12), ey0 + px(19), T::kText, title, H::Left, V::Middle);
        text(dl, gBold, px(16.5f), origin.x + px(20) + textWidth(gBold, px(16.5f), title), ey0 + px(19), chColor,
             "\xC2\xB7  " + channelTitle(ch) + "  \xC2\xB7  " + std::to_string(bars) + " bars", H::Left, V::Middle);
        double used = 0;
        const auto st = songItemBars(live, ch, items);
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == SongItem::Kind::Rest)
                continue;  // silence
            const double next = i + 1 < items.size() ? st[i + 1] : bars;
            used += std::max(std::min(songItemBeats(live, ch, items[i]) / bar, next - st[i]), 0.0);
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1f of %d bars played", std::min(used, static_cast<double>(bars)), bars);
        text(dl, gMedium, px(15), origin.x + width - px(12), ey0 + px(19), T::kDim, buf, H::Right, V::Middle);
    }
    screen(dl, ImVec2(origin.x + px(8), ey0 + px(37)), ImVec2(origin.x + width - px(8), ey1 - px(8)), kSongScreen, [] {});  // the cell's timeline, on a screen
    const float ex0 = origin.x + px(14), ex1 = origin.x + width - px(60), barW = (ex1 - ex0) / static_cast<float>(bars);
    const float ry = ey0 + px(42);
    const int every = bars <= 16 ? 1 : bars <= 32 ? 2 : bars <= 64 ? 4 : 8;
    for (int k = 0; k <= bars; ++k) {
        const float x = ex0 + static_cast<float>(k) * barW;
        dl->AddLine(ImVec2(x, ry), ImVec2(x, ry + px(k % every == 0 ? 7 : 4)), T::kDim, px(1));
        if (k < bars && k % every == 0)
            text(dl, gMedium, px(12.75f), x + px(3), ry + px(14), T::kDim, std::to_string(k + 1), H::Left, V::Middle);
    }
    const float iy0 = ry + px(27), iy1 = ey1 - px(14);
    float lastEnd = ex0;
    const auto starts = songItemBars(live, ch, items);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const SongItem& it = items[i];
        bool open = false;
        const double len = songItemBeats(live, ch, it, &open) / bar;
        const double at = starts[i];
        const double next = i + 1 < items.size() ? std::max(starts[i + 1], at) : bars;
        const double end = std::min(at + std::max(len, 0.25), std::min(next, static_cast<double>(bars)));
        const bool selected = cursor.editing && cursor.item == static_cast<int>(i);
        const bool rest = it.kind == SongItem::Kind::Rest;
        const ImU32 col = rest ? IM_COL32(0x7E, 0x84, 0x8C, 255) : it.kind == SongItem::Kind::Chain ? kViolet : chColor;
        const float x0 = ex0 + static_cast<float>(std::min(at, static_cast<double>(bars))) * barW, x1 = std::max(ex0 + static_cast<float>(end) * barW, x0 + px(10));
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(ImVec2(x0, iy0));
        if (ImGui::InvisibleButton("##item", ImVec2(x1 - x0, iy1 - iy0)))
            action = SongAction{SongAction::Kind::ItemSelect, r, ch, static_cast<int>(i)};
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            songDragBars_ = static_cast<int>(std::lround(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x / barW));
        if (ImGui::IsItemDeactivated() && songDragBars_ != 0) {
            action = SongAction{SongAction::Kind::ItemMove, r, ch, static_cast<int>(i), static_cast<int>(std::floor(at + 1e-6)) + songDragBars_};
            songDragBars_ = 0;
        }
        const float shift = ImGui::IsItemActive() ? static_cast<float>(songDragBars_) * barW : 0.0f;
        if (hovered && io.MouseWheel != 0.0f) {
            const int notch = io.MouseWheel > 0 ? 1 : -1;
            action = io.MousePos.x < x0 + (x1 - x0) * 0.5f ? SongAction{SongAction::Kind::ItemContent, r, ch, static_cast<int>(i), notch}
                                                          : SongAction{SongAction::Kind::ItemRepeats, r, ch, static_cast<int>(i), notch};
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            menuRequest_ = MenuRequest{MenuTarget{MenuTarget::Kind::SongItem, ch, r, 0, static_cast<int>(i)}, io.MousePos};
        ImGui::PopID();
        const ImVec2 a(x0 + shift + px(1), iy0), b(x1 + shift - px(1), iy1);
        if (rest) {  // a silence: dashed
            for (float t = a.x; t < b.x; t += px(8)) {
                dl->AddLine(ImVec2(t, a.y), ImVec2(std::min(t + px(4), b.x), a.y), col, px(1));
                dl->AddLine(ImVec2(t, b.y), ImVec2(std::min(t + px(4), b.x), b.y), col, px(1));
            }
            for (float t = a.y; t < b.y; t += px(8)) {
                dl->AddLine(ImVec2(a.x, t), ImVec2(a.x, std::min(t + px(4), b.y)), col, px(1));
                dl->AddLine(ImVec2(b.x, t), ImVec2(b.x, std::min(t + px(4), b.y)), col, px(1));
            }
            if (selected)
                dl->AddRect(a, b, T::kCyan, px(5), px(2.5f));
        } else {
            dl->AddRectFilled(a, b, alpha(col, 0.22f), px(5));
            dl->AddRect(a, b, selected ? T::kCyan : col, px(5), px(selected ? 2.5f : 1.2f));
        }
        if (selected)
            sessionCursorBox_ = {a, b};
        text(dl, gBold, px(15.75f), a.x + px(8), a.y + px(19), rest ? T::kLabel : T::kText,
             fit(gBold, px(15.75f), itemLabel(ch, it) + (it.repeats > 1 && !rest ? "  \xC3\x97" + std::to_string(it.repeats) : ""), b.x - a.x - px(12)),
             H::Left, V::Middle);
        char sub[64];
        if (rest)
            std::snprintf(sub, sizeof sub, "%s", it.after && i > 0 ? "right after" : ("bar " + std::to_string(it.bar + 1)).c_str());
        else if (it.after && i > 0)
            std::snprintf(sub, sizeof sub, "right after  \xC2\xB7  %.1f bars%s", len, open ? " \xC2\xB7 \xE2\x88\x9E" : "");
        else
            std::snprintf(sub, sizeof sub, "bar %d  \xC2\xB7  %.1f bars%s", it.bar + 1, len, open ? " \xC2\xB7 \xE2\x88\x9E" : "");
        text(dl, gMedium, px(13.5f), a.x + px(8), a.y + px(42), T::kLabel, fit(gMedium, px(13.5f), sub, b.x - a.x - px(12)), H::Left, V::Middle);
        if (len <= 0)
            text(dl, gMedium, px(13.5f), a.x + px(8), a.y + px(62), T::kRed, "empty", H::Left, V::Middle);
        if ((it.loop || open) && next > end) {  // on to the next item
            const float lx1 = ex0 + static_cast<float>(std::min(next, static_cast<double>(bars))) * barW - px(2);
            dl->AddLine(ImVec2(b.x, (a.y + b.y) * 0.5f), ImVec2(lx1 + shift, (a.y + b.y) * 0.5f), alpha(col, 0.8f), px(2));
            text(dl, gMedium, px(12.75f), b.x + px(5), (a.y + b.y) * 0.5f - px(12), alpha(col, 0.9f), "loop", H::Left, V::Middle);
        }
        lastEnd = std::max(lastEnd, x1);
    }
    {  // + : a new item after the last
        const bool selected = cursor.editing && cursor.item == static_cast<int>(items.size());
        const float ax0 = std::min(lastEnd + px(6), ex1 + px(4)), ax1 = ax0 + px(42);
        const ImU32 dash = selected ? T::kCyan : IM_COL32(0x5A, 0x5F, 0x67, 255);
        dl->AddRect(ImVec2(ax0, iy0), ImVec2(ax1, iy1), dash, px(5), px(selected ? 2.5f : 1.0f));
        if (selected)
            sessionCursorBox_ = {ImVec2(ax0, iy0), ImVec2(ax1, iy1)};
        text(dl, gBold, px(24), (ax0 + ax1) * 0.5f, (iy0 + iy1) * 0.5f, selected ? T::kCyan : T::kDim, "+", H::Center, V::Middle);
        ImGui::SetCursorScreenPos(ImVec2(ax0, iy0));
        if (ImGui::InvisibleButton("##additem", ImVec2(ax1 - ax0, iy1 - iy0)))
            action = SongAction{SongAction::Kind::AddItem, r, ch};
        ImGui::SetItemTooltip("Add a clip, a chain of the channel or a silence after the last item");
    }
    if (playing && live.song.section == r && live.song.beat >= 0) {
        const float x = ex0 + static_cast<float>(std::min(live.song.beat / bar, static_cast<double>(bars))) * barW;
        dl->AddLine(ImVec2(x, ry), ImVec2(x, iy1 + px(2)), T::kGreen, px(1.6f));
    }
    ImGui::PopID();
    return action;
}

void PerformanceView::drawNoteEditor(const Mapping& mapping, const LiveState& live, float w, float h) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    editing_ = true;
    escapeTaken_ = false;
    editorClip_ = false;
    drawPianoRoll(mapping, live, at.x, at.y, w, h);
    editing_ = false;
    // Under the roll, a scrollbar: its thumb the part of the loop shown. Drag
    // it, or click beside it to go there.
    const float barY = at.y + h + px(4), barH = px(8);
    if (editorClip_) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = rollGx0_, bw = std::max(rollAx1_ - rollGx0_, 1.0f), span = 1.0f / viewZoom_;
        ImGui::SetCursorScreenPos(ImVec2(x0, barY - px(2)));
        ImGui::InvisibleButton("##rollscroll", ImVec2(bw, barH + px(4)));
        const float mouseX = ImGui::GetIO().MousePos.x;
        if (ImGui::IsItemActivated()) {
            const float tx0 = x0 + viewStart_ * bw;
            scrollGrab_ = mouseX >= tx0 && mouseX <= tx0 + span * bw ? mouseX - tx0 : span * bw * 0.5f;
        }
        if (ImGui::IsItemActive())
            viewStart_ = std::clamp((mouseX - scrollGrab_ - x0) / bw, 0.0f, 1.0f - span);
        const float tx0 = x0 + viewStart_ * bw, tx1 = tx0 + span * bw;
        dl->AddRectFilled(ImVec2(x0, barY), ImVec2(x0 + bw, barY + barH), IM_COL32(0x1E, 0x20, 0x24, 255), px(4));
        dl->AddRect(ImVec2(x0, barY), ImVec2(x0 + bw, barY + barH), IM_COL32(0x3A, 0x3E, 0x45, 255), px(4), px(1));
        const ImU32 thumb = viewZoom_ <= 1.0f ? IM_COL32(0x4E, 0x53, 0x5C, 255)
                            : ImGui::IsItemActive() || ImGui::IsItemHovered() ? IM_COL32(0x9A, 0xA0, 0xAC, 255)
                                                                              : IM_COL32(0x78, 0x7E, 0x8A, 255);
        dl->AddRectFilled(ImVec2(tx0 + 1, barY + 1), ImVec2(tx1 - 1, barY + barH - 1), thumb, px(3));
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, barY + barH + px(2)));
    ImGui::Dummy(ImVec2(w, 0));
}

bool PerformanceView::selected(const NoteInterval& n) const {
    for (const LoopNoteRef& r : selection_)
        if (r.start == n.start && r.note == n.note)
            return true;
    return false;
}

// A block without a title bar, like the piano roll's.
void plainBlock(ImDrawList* dl, float x, float y, float w, float h) {
    if (gLook) {
        frame(dl, ImVec2(x, y), ImVec2(x + w, y + h), 0, kNeon);
        return;
    }
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, px(8));
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, px(8), px(1));
}

// The tonality, for every channel: its name over an octave of keys, the
// scale's notes lit, the root brighter. A click opens the menu choosing it.
void PerformanceView::drawScale(const std::optional<Key>& key, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    plainBlock(dl, x, y, w, h);
    const ImVec2 a(x + px(10), y + px(10)), b(x + w - px(10), y + h - px(10));
    if (b.y - a.y < px(30))
        return;
    std::array<bool, 12> inScale{};
    if (key)
        for (int pc : scalePitchClasses(*key))
            inScale[static_cast<std::size_t>(((pc % 12) + 12) % 12)] = true;
    const int root = key ? key->root : -1;
    screen(dl, a, b, T::kBlue, [&] {
        text(dl, gBold, px(13), (a.x + b.x) * 0.5f, a.y + px(13), key ? T::kIce : T::kDim,
             key ? keyName(*key) : "no scale", H::Center, V::Middle);
        const float kx0 = a.x + px(8), kx1 = b.x - px(8), ky0 = a.y + px(26), ky1 = b.y - px(7);
        const float ww = (kx1 - kx0) / 7.0f;
        static constexpr int kWhites[] = {0, 2, 4, 5, 7, 9, 11};
        static constexpr const char* kNames[] = {"C", "D", "E", "F", "G", "A", "B"};
        for (int i = 0; i < 7; ++i) {
            const int pc = kWhites[i];
            const bool on = inScale[static_cast<std::size_t>(pc)];
            const ImU32 fill = on ? (pc == root ? T::kIce : T::kBlue) : IM_COL32(0x3C, 0x42, 0x4C, 255);
            dl->AddRectFilled(ImVec2(kx0 + static_cast<float>(i) * ww + 1, ky0), ImVec2(kx0 + static_cast<float>(i + 1) * ww - 1, ky1), fill, px(2));
            text(dl, gBold, px(8.5f), kx0 + (static_cast<float>(i) + 0.5f) * ww, ky1 - px(7), on ? IM_COL32(0x10, 0x12, 0x16, 255) : T::kDim,
                 kNames[i], H::Center, V::Middle);
        }
        static constexpr std::pair<int, int> kBlacks[] = {{1, 0}, {3, 1}, {6, 3}, {8, 4}, {10, 5}};  // pitch, white key before it
        for (auto [pc, before] : kBlacks) {
            const bool on = inScale[static_cast<std::size_t>(pc)];
            const float bx = kx0 + static_cast<float>(before + 1) * ww;
            const ImVec2 ba(bx - ww * 0.32f, ky0), bb(bx + ww * 0.32f, ky0 + (ky1 - ky0) * 0.58f);
            dl->AddRectFilled(ba, bb, on ? (pc == root ? T::kIce : T::kBlue) : IM_COL32(0x18, 0x1A, 0x1E, 255), px(2));
            dl->AddRect(ba, bb, IM_COL32(0x0A, 0x0A, 0x0C, 255), px(2), px(1));
        }
    });
    if (hoverButton("##scale", a, b))
        click("scale");
}

// What a lane's loop plays on: its CC, or the Velocity / Transpose its axis is set to.
constexpr int kLaneVelocity = -1, kLaneTranspose = -2;
static int laneTarget(const AxisBinding& axis, int cc) {
    if (axis.enabled && axis.mode == AxisMode::Velocity)
        return kLaneVelocity;
    if (axis.enabled && axis.mode == AxisMode::Transpose)
        return kLaneTranspose;
    return cc;
}

// A channel's title: its number and name, or CHANNEL and its number.
std::string PerformanceView::channelTitle(int channel) const {
    const std::string& name = channelNames_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))];
    return name.empty() ? "CHANNEL " + std::to_string(channel + 1) : std::to_string(channel + 1) + "  " + name;
}

// The active channel, large, and its name. Its arrows (or the wheel over it)
// go to the previous or next channel; a click in the middle or a right-click
// opens its menu (previous, next, rename).
void PerformanceView::drawChannel(const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    plainBlock(dl, x, y, w, h);
    const ImVec2 a(x + px(10), y + px(10)), b(x + w - px(10), y + h - px(10));
    if (b.y - a.y < px(30))
        return;
    const std::string& name = channelNames_[static_cast<std::size_t>(std::clamp(live.channel, 0, kChannelCount - 1))];
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b);
    screen(dl, a, b, T::kGreen, [&] {
        text(dl, gBold, px(9), a.x + px(9), a.y + px(10), alpha(T::kGreen, 0.7f), "CHANNEL", H::Left, V::Middle);
        const float cx = (a.x + b.x) * 0.5f, midY = (a.y + b.y) * 0.5f + (name.empty() ? px(3) : -px(5));
        const std::string number = std::to_string(live.channel + 1);
        const float size = std::min(px(64), (b.y - a.y) * (name.empty() ? 0.72f : 0.6f));
        for (float r : {px(3), px(1.5f)})  // glow
            for (auto [dx, dy] : {std::pair{-r, 0.0f}, {r, 0.0f}, {0.0f, -r}, {0.0f, r}})
                text(dl, gBold, size, cx + dx, midY + dy, alpha(T::kGreen, 0.18f), number, H::Center, V::Middle);
        text(dl, gBold, size, cx, midY, T::kGreen, number, H::Center, V::Middle);
        if (!name.empty())
            text(dl, gBold, px(16), cx, b.y - px(15), mix(T::kGreen, T::kText, 0.3f), fit(gBold, px(16), name, b.x - a.x - px(50)), H::Center, V::Middle);
        // The arrows: faint, lit while the mouse is over the block.
        const float ay = (a.y + b.y) * 0.5f + px(3), s = px(7);
        const ImU32 col = alpha(T::kGreen, hovered ? 1.0f : 0.3f);
        dl->AddTriangleFilled(ImVec2(a.x + px(10), ay), ImVec2(a.x + px(10) + s, ay - s * 1.3f), ImVec2(a.x + px(10) + s, ay + s * 1.3f), col);
        dl->AddTriangleFilled(ImVec2(b.x - px(10), ay), ImVec2(b.x - px(10) - s, ay - s * 1.3f), ImVec2(b.x - px(10) - s, ay + s * 1.3f), col);
    });
    const MenuTarget target{MenuTarget::Kind::ChannelBlock, live.channel};
    if (gFocusItems)
        focusTargets_.push_back({"##channelblock", target});
    if (hoverButton("##channelblock", a, b)) {
        const float mx = ImGui::GetIO().MousePos.x, third = (b.x - a.x) / 3.0f;
        if (mx < a.x + third)
            channelRequest_ = (live.channel + kChannelCount - 1) % kChannelCount;
        else if (mx > b.x - third)
            channelRequest_ = (live.channel + 1) % kChannelCount;
        else
            menuRequest_ = MenuRequest{target, ImGui::GetIO().MousePos};
    }
    if (ImGui::IsItemHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            menuRequest_ = MenuRequest{target, ImGui::GetIO().MousePos};
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            channelRequest_ = (live.channel + (wheel > 0 ? 1 : kChannelCount - 1)) % kChannelCount;
    }
}

void PerformanceView::trackFrozenPositions(const LiveState& live) {
    const auto channel = static_cast<std::size_t>(std::clamp(live.channel, 0, 15));
    const bool sameChannel = live.channel == lastChannel_;
    std::uint8_t& known = frozenKnown_[channel];
    for (int a = 0; a < 4; ++a) {  // the stick axes
        const auto bit = static_cast<std::uint8_t>(1u << a);
        if (!(live.frozenAxes & bit)) {
            known = static_cast<std::uint8_t>(known & ~bit);
            continue;
        }
        // Just frozen, or frozen before this view saw it: the stick is there now.
        if (!(known & bit) || (sameChannel && !(lastFrozen_ & bit))) {
            frozenAt_[channel][static_cast<std::size_t>(a)] = stickPosition(live, static_cast<Axis>(a));
            known = static_cast<std::uint8_t>(known | bit);
        }
    }
    lastChannel_ = live.channel;
    lastFrozen_ = live.frozenAxes;
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
    const ImU32 color = frozen ? T::kIce : accent;

    const float top = panel(dl, x, y, w, h, index == 0 ? "LEFT STICK" : "RIGHT STICK", live.connected && (bx.enabled || by.enabled));
    const ScreenBox s = screenBox(x, top, w, h);

    const float sx = smooth(index * 4 + 0, stickPosition(live, ax), 18.0f);
    const float sy = smooth(index * 4 + 1, stickPosition(live, ay), 18.0f);
    screen(dl, s.a, s.b, accent, [&] {
        const ImVec2 c((s.a.x + s.b.x) * 0.5f, (s.a.y + s.b.y) * 0.5f);
        const float rw = (s.b.x - s.a.x) * 0.5f, rh = (s.b.y - s.a.y) * 0.5f;
        dl->AddLine(ImVec2(s.a.x, c.y), ImVec2(s.b.x, c.y), alpha(accent, 0.12f));
        dl->AddLine(ImVec2(c.x, s.a.y), ImVec2(c.x, s.b.y), alpha(accent, 0.12f));
        dl->AddCircle(c, rh * 0.8f, alpha(accent, 0.10f), 48);
        auto place = [&](float px_, float py_, float& radius) {
            const float deflection = std::min(1.0f, std::sqrt(px_ * px_ + py_ * py_));
            radius = rh * (0.20f + 0.12f * deflection);
            return ImVec2(c.x + px_ * (rw - radius * 1.4f), c.y + py_ * (rh - radius * 1.4f));
        };
        float r = 0;
        const ImVec2 pos = place(sx, sy, r);
        bool away = false;
        if (frozen) {
            // The spot where it froze stays marked, in the stick's color, until it thaws.
            const auto& at = frozenAt_[static_cast<std::size_t>(std::clamp(live.channel, 0, 15))];
            const bool fx = live.frozenAxes & (1u << static_cast<unsigned>(ax));
            const bool fy = live.frozenAxes & (1u << static_cast<unsigned>(ay));
            const float gx = fx ? at[static_cast<std::size_t>(ax)] : sx;
            const float gy = fy ? at[static_cast<std::size_t>(ay)] : sy;
            float gr = 0;
            const ImVec2 g = place(gx, gy, gr);
            if (fx)
                dl->AddLine(ImVec2(g.x, s.a.y), ImVec2(g.x, s.b.y), alpha(accent, 0.6f), px(1.2f));
            if (fy)
                dl->AddLine(ImVec2(s.a.x, g.y), ImVec2(s.b.x, g.y), alpha(accent, 0.6f), px(1.2f));
            ghost(dl, g, gr, accent, 1.0f + std::abs(gx) * 0.35f, 1.0f + std::abs(gy) * 0.35f);
            away = std::abs(gx - sx) > 0.06f || std::abs(gy - sy) > 0.06f;
        }
        blob(dl, pos, r, color, time_ * (frozen ? 0.2 : 1.0), 1.0f + std::abs(sx) * 0.35f, 1.0f + std::abs(sy) * 0.35f);
        if (frozen)
            badge(dl, gBold, px(12), c.x, s.b.y - px(30), px(22), away ? "FROZEN  \xC2\xB7  find the spot" : "FROZEN",
                  T::kIce, alpha(T::kIce, 0.16f), T::kIce);
    });
    if (live.stretchHeld)
        shiftModeBadge(dl, s.a, s.b, live.stickShapes[static_cast<std::size_t>(index)], accent);
    if (hoverButton(index == 0 ? "##stickL" : "##stickR", s.a, s.b))
        click(index == 0 ? "axes:left_x,left_y" : "axes:right_x,right_y");

    // What the axes send: their CCs, as chips (ice blue while frozen).
    const float midiY = s.b.y + px(18);
    section(dl, x, w, midiY, "MIDI");
    const float chipTop = midiY + px(12), chipH = px(20);
    const float cw = (w - px(30)) * 0.5f;
    auto axisChip = [&](float cx0, const AxisBinding& b, Axis a, const char* name) {
        const std::string label = std::string(name) + "  " + axisTarget(b, live) + (b.invert ? "  inv" : "");
        badge(dl, gCondensed, px(13), cx0 + cw * 0.5f, chipTop, chipH, label, T::kText, alpha(frozen ? T::kIce : accent, 0.22f),
              b.enabled ? (frozen ? T::kIce : accent) : T::kDim);
        const std::string spec = "axis:" + std::string(toString(a));
        if (hoverButton(("##" + spec).c_str(), ImVec2(cx0, chipTop - px(2)), ImVec2(cx0 + cw, chipTop + chipH + px(2))))
            click(spec);
    };
    axisChip(x + px(12), bx, ax, "X");
    axisChip(x + px(18) + cw, by, ay, "Y");

    // Freeze: which button freezes these axes, and whether they are frozen.
    const auto button = freezeButton(m, axisBits);
    const float fy = chipTop + chipH + px(8), fh = px(28);
    if (fy + fh > y + h - px(10))
        return;
    row(dl, x + px(12), fy, w - px(24), fh, frozen ? alpha(T::kIce, 0.18f) : T::kRow, frozen ? T::kIce : T::kRowBorder);
    const ImVec2 dot(x + px(28), fy + fh * 0.5f);
    if (frozen)
        dl->AddCircleFilled(dot, px(5.5f), T::kIce);
    else
        dl->AddCircle(dot, px(5.5f), T::kIce, 0, px(2));
    text(dl, gBold, px(13), x + px(42), fy + fh * 0.5f, T::kText, button ? label(*button) : "no freeze button", H::Left, V::Middle);
    text(dl, frozen ? gBold : gMedium, px(12), x + w - px(22), fy + fh * 0.5f, frozen ? T::kIce : T::kLabel,
         frozen ? "frozen" : "free", H::Right, V::Middle);
    if (button && hoverButton(index == 0 ? "##freezeL" : "##freezeR", ImVec2(x + px(12), fy), ImVec2(x + w - px(12), fy + fh)))
        click("button:" + std::string(toString(*button)));

    // Loop: the movement a Loop record button captured from this stick.
    const float loopY = s.b.y + loopOffset();
    if (y + h - px(10) - (loopY + px(10)) < px(28))
        return;
    const LiveState::Loop& loop = live.loops[static_cast<std::size_t>(index)];
    const LoopBinding& lb = m.loops[static_cast<std::size_t>(index)];
    const LaneLook lanes[2] = {
        {&loop.lanes[0], Engine::trackOf(index, 0), accent, "X", bx.enabled, bx.invert, false, laneTarget(bx, lb.ccX), lb.steps[0]},
        {&loop.lanes[1], Engine::trackOf(index, 1), mix(accent, T::kText, 0.5f), "Y", by.enabled, by.invert, false, laneTarget(by, lb.ccY), lb.steps[1]}};
    drawLoopPanel(dl, x, w, loopY, y + h - px(10), loop.status, lb.playback, lanes, 2, accent, artFor(live.channel, index),
                  "REC", index == 0 ? "##loopL" : "##loopR", index == 0 ? "axes:left_x,left_y" : "axes:right_x,right_y",
                  index);
}

// A LOOP panel: its header (state, playback), and a lane per axis loop, each
// across the whole box on its own time scale.
void PerformanceView::drawLoopPanel(ImDrawList* dl, float x, float w, float loopY, float bottom, const LoopStatus& st,
                                    LoopPlayback playback, const LaneLook* lanes, int laneCount, ImU32 color, int art,
                                    const char* waitStatus, const char* id, const std::string& spec, int lockSource) {
    const bool recording = st.state == LoopStatus::State::Recording;
    const bool playing = st.state == LoopStatus::State::Playing;
    std::uint16_t tracks = 0;
    for (int k = 0; k < laneCount; ++k)
        tracks = static_cast<std::uint16_t>(tracks | (1u << lanes[k].track));

    // Header: LOOP ——— status. Each lane shows its own length.
    const float size = px(11);
    text(dl, gBold, size, x + px(12), loopY, T::kDim, "LOOP", H::Left, V::Middle);
    std::string status;
    ImU32 statusColor = T::kDim;
    if (recording) {
        int recorded = 0;  // the longest lane recording, not one already playing
        for (int k = 0; k < laneCount; ++k)
            if (lanes[k].lane->status.state == LoopStatus::State::Recording)
                recorded = std::max(recorded, lanes[k].lane->status.length);
        char secondsText[24];
        std::snprintf(secondsText, sizeof secondsText, "REC  %.1f s", static_cast<double>(recorded) / Engine::kLoopRateHz);
        status = secondsText;
        statusColor = T::kRed;
    } else if (st.waiting) {
        status = waitStatus;
        statusColor = T::kRed;
    } else if (playing) {
        status = playback == LoopPlayback::Forward    ? "FWD"
                 : playback == LoopPlayback::Reverse  ? "REV"
                 : playback == LoopPlayback::Envelope ? "ENV"
                                                      : "B&F";
        statusColor = color;
    }
    const float statusX = x + w - px(12) - textWidth(gBold, px(10), status);
    if (!status.empty())
        text(dl, gBold, px(10), statusX, loopY, statusColor, status, H::Left, V::Middle);
    const float iconX = statusX - px(9);
    if (recording || st.waiting) {
        dl->AddCircleFilled(ImVec2(iconX, loopY), px(3.5f), alpha(T::kRed, recording ? 1.0f : 0.6f));
    } else if (playing) {
        auto arrow = [&](float cx, bool left) {
            const float s2 = px(3.5f);
            if (left)
                dl->AddTriangleFilled(ImVec2(cx - s2, loopY), ImVec2(cx + s2, loopY - s2 * 1.2f), ImVec2(cx + s2, loopY + s2 * 1.2f), color);
            else
                dl->AddTriangleFilled(ImVec2(cx + s2, loopY), ImVec2(cx - s2, loopY - s2 * 1.2f), ImVec2(cx - s2, loopY + s2 * 1.2f), color);
        };
        if (playback == LoopPlayback::BackAndForth) {
            arrow(iconX - px(4), true);
            arrow(iconX + px(3), false);
        } else {
            arrow(iconX, playback == LoopPlayback::Reverse);
        }
    }
    const float ruleEnd = status.empty() ? x + w - px(12) : iconX - px(playback == LoopPlayback::BackAndForth && playing ? 14 : 10);
    float ruleStart = x + px(16) + textWidth(gBold, size, "LOOP");
    if (st.locked) {
        drawLock(dl, ruleStart - px(1), loopY - px(0.5f), px(11), color);
        ruleStart += px(14);
    }
    if (ruleEnd > ruleStart)
        dl->AddLine(ImVec2(ruleStart, loopY), ImVec2(ruleEnd, loopY), IM_COL32(0x40, 0x44, 0x4B, 255), px(1));

    // Empty, one box with its idle animation; otherwise a box per lane, its
    // border telling its state.
    const ImVec2 ga(x + px(12), loopY + px(10)), gb(x + w - px(12), bottom);
    if (!recording && !playing && !st.waiting) {
        idleArt(dl, art, ga, gb, time_);
    }
    std::array<std::pair<ImVec2, ImVec2>, 4> laneBoxes{};
    laneClearHovered_.reset();
    if (recording || playing || st.waiting) {
        const float gap = px(4);
        const float laneH = (gb.y - ga.y - gap * static_cast<float>(laneCount - 1)) / static_cast<float>(std::max(laneCount, 1));
        for (int k = 0; k < laneCount; ++k) {
            const float top = ga.y + (laneH + gap) * static_cast<float>(k);
            const ImVec2 la(ga.x, top), lb(gb.x, top + laneH);
            laneBoxes[static_cast<std::size_t>(k)] = {la, lb};
            const LoopStatus& ls = lanes[k].lane->status;
            dl->AddRectFilled(la, lb, kGraph, px(4));
            const bool lanePlays = ls.state == LoopStatus::State::Playing;
            if (ls.locked && lanePlays)
                drawLockedBorder(dl, la, lb, lanes[k].color, px(4));
            else
                dl->AddRect(la, lb,
                            ls.state == LoopStatus::State::Recording ? T::kRed
                            : ls.waiting && lanes[k].enabled         ? alpha(T::kRed, 0.5f)
                            : lanePlays                              ? alpha(lanes[k].color, 0.45f)
                                                                     : IM_COL32(0x2A, 0x2D, 0x32, 255),
                            px(4), px(1.3f));
            drawLane(dl, la, lb, lanes[k]);
        }
    }
    // Shift / Ctrl + wheel over the header: every lane.
    stretchWheel(tracks, ImVec2(ga.x, loopY - px(6)), ImVec2(gb.x, ga.y));
    const bool hasLanes = recording || playing || st.waiting;
    gNoFocus = hasLanes;  // the edit cursor goes to its lanes instead
    const bool panelClicked = hoverButton(id, ImVec2(ga.x, loopY - px(6)), gb);
    gNoFocus = false;
    if (panelClicked) {
        if (laneClearHovered_)  // a lane's x
            clearRequest_ = laneClearHovered_;
        else
            click(spec);
    }
    // A right-click: the menu of a lane's axis, on the header of all of them.
    const ImVec2 ha(ga.x, loopY - px(6)), hb(gb.x, hasLanes ? ga.y : gb.y);  // empty: the whole panel
    menuOnRightClick(MenuTarget{MenuTarget::Kind::Panel, 0, 0, lockSource, -1, spec}, playing, st.locked, &ha, &hb);
    if (hasLanes)
        for (int k = 0; k < laneCount; ++k) {
            const LoopStatus& ls = lanes[k].lane->status;
            const auto& [la, lb] = laneBoxes[static_cast<std::size_t>(k)];
            const MenuTarget target{MenuTarget::Kind::Lane, 0, 0, Engine::sourceOfTrack(lanes[k].track), Engine::axisOfTrack(lanes[k].track), spec};
            menuOnRightClick(target, ls.state == LoopStatus::State::Playing, ls.locked, &la, &lb);
            if (focusable("##lane" + std::to_string(lanes[k].track), la, lb, target))
                click(spec);
        }
}

// One axis loop in its lane: its curve over its whole length (in steps, the
// staircase it plays, the recorded curve faint behind), the playhead, its
// name and CC, its length, stretch and amplitude. Shift / Ctrl + wheel over it
// stretches or amplifies it alone.
void PerformanceView::drawLane(ImDrawList* dl, ImVec2 a, ImVec2 b, const LaneLook& look) {
    const LiveState::Lane& lane = *look.lane;
    const LoopStatus& st = lane.status;
    const bool recording = st.state == LoopStatus::State::Recording;
    const bool playing = st.state == LoopStatus::State::Playing;
    const auto bit = static_cast<std::uint16_t>(1u << look.track);
    const float midY = (a.y + b.y) * 0.5f;
    const float small = std::min(px(10), (b.y - a.y) * 0.32f);
    const float nameY = a.y + std::min(px(10), (b.y - a.y) * 0.3f);
    if (st.state == LoopStatus::State::Empty || lane.points < 2) {
        // Its name, and why it holds nothing.
        text(dl, gBold, small, a.x + px(7), nameY, look.enabled ? alpha(look.color, 0.6f) : T::kDim, look.name, H::Left, V::Middle);
        const char* why = !look.enabled ? "not enabled  \xC2\xB7  not recorded" : st.waiting ? "ready" : "no loop";
        text(dl, gMedium, small, (a.x + b.x) * 0.5f, midY, st.waiting && look.enabled ? alpha(T::kRed, 0.7f) : T::kDim, why, H::Center,
             V::Middle);
        return;
    }
    const float seconds = static_cast<float>(st.length) / Engine::kLoopRateHz;
    const float lx0 = a.x + px(6);
    // Over the whole lane whatever its length, even while it records (the newest at the right).
    const float lx1 = b.x - px(6);
    const float ly0 = a.y + px(4), ly1 = b.y - px(4);
    // Around its middle, with another amplitude; on the grid's levels, in steps.
    float lo = 1, hi = 0;
    for (int i = 0; i < lane.points; ++i)
        if (lane.on[static_cast<std::size_t>(i)]) {
            lo = std::min(lo, static_cast<float>(lane.v[static_cast<std::size_t>(i)]) / 255.0f);
            hi = std::max(hi, static_cast<float>(lane.v[static_cast<std::size_t>(i)]) / 255.0f);
        }
    const float centre = lo <= hi ? (lo + hi) * 0.5f : 0.5f;
    // Its amplitude, times what the loop modulating it gives now.
    const bool modulated = playing && st.modSource >= 0;
    const float amp = playing ? static_cast<float>(st.amplitude) / 100.0f * (modulated ? st.modFactor : 1.0f) : 1.0f;
    const bool steps = playing && lane.stepSamples > 0;
    const float levels = static_cast<float>(std::clamp(look.steps.levels, 2, 128) - 1);
    const float offset = playing ? static_cast<float>(st.offset) / 100.0f : 0.0f;
    // `shifted`: with its offset (not the faint curve as recorded).
    auto value = [&](int i, float scale, bool stepped, bool shifted = true) {
        float v = Engine::shapeAmplitude(static_cast<float>(lane.v[static_cast<std::size_t>(i)]) / 255.0f, centre, scale);
        if (shifted)
            v = Engine::shapeOffset(v, offset);
        if (stepped)
            v = std::round(v * levels) / levels;
        // What the loop sends: a finger's Y the other way up, an inverted axis upside down.
        if (look.flip)
            v = 1.0f - v;
        return look.invert ? 1.0f - v : v;
    };
    auto yOf = [&](float v) { return ly1 - v * (ly1 - ly0); };
    auto xOfPoint = [&](int i) { return lx0 + (lx1 - lx0) * static_cast<float>(i) / static_cast<float>(lane.points - 1); };
    auto polyline = [&](float scale, ImU32 col, float thickness, bool shifted = true) {  // broken where a finger was lifted
        for (int i = 1; i < lane.points; ++i)
            if (lane.on[static_cast<std::size_t>(i)] && lane.on[static_cast<std::size_t>(i - 1)])
                dl->AddLine(ImVec2(xOfPoint(i - 1), yOf(value(i - 1, scale, false, shifted))), ImVec2(xOfPoint(i), yOf(value(i, scale, false, shifted))),
                            col, thickness);
    };
    const int lastSample = std::max(st.length - 1, 1);
    auto pointOf = [&](int sample) { return std::clamp(static_cast<int>(std::lround(static_cast<float>(sample) / static_cast<float>(lastSample) *
                                                                                    static_cast<float>(lane.points - 1))),
                                                       0, lane.points - 1); };
    auto xOfSample = [&](int sample) { return lx0 + (lx1 - lx0) * static_cast<float>(sample) / static_cast<float>(lastSample); };
    if (steps) {
        // The grid, where its steps leave room; the curve as recorded; the staircase played.
        const int step = lane.stepSamples;
        if (xOfSample(step) - lx0 >= px(5))
            for (int s = step; s < st.length; s += step)
                dl->AddLine(ImVec2(xOfSample(s), a.y + px(2)), ImVec2(xOfSample(s), b.y - px(2)), IM_COL32(255, 255, 255, 18), px(1));
        polyline(1.0f, alpha(look.color, 0.22f), px(1.3f), false);
        float previous = -1;
        for (int s = 0; s < st.length; s += step) {
            const int i = pointOf(s);
            if (!lane.on[static_cast<std::size_t>(i)]) {
                previous = -1;
                continue;
            }
            const float y = yOf(value(i, amp, true));
            const float x0 = xOfSample(s), x1 = xOfSample(std::min(s + step, lastSample));
            if (previous >= 0)
                dl->AddLine(ImVec2(x0, previous), ImVec2(x0, y), look.color, px(1.8f));
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), look.color, px(1.8f));
            previous = y;
        }
    } else {
        if (amp != 1.0f || offset != 0.0f)
            polyline(1.0f, alpha(look.color, 0.22f), px(1.3f), false);
        polyline(amp, look.color, px(1.8f));
    }
    if (modulated) {  // the band the loop modulating it leaves it now, dashed in that loop's colour
        const ImU32 tone = alpha(trackColor(st.modSource), 0.8f);
        for (float v : {lo, hi}) {
            float e = Engine::shapeOffset(Engine::shapeAmplitude(v, centre, amp), offset);
            if (look.flip)
                e = 1.0f - e;
            const float y = yOf(look.invert ? 1.0f - e : e);
            for (float x = lx0; x < lx1; x += px(7))
                dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + px(4), lx1), y), tone, px(1.2f));
        }
    }
    if (recording) {
        dl->AddLine(ImVec2(lx1, a.y + px(2)), ImVec2(lx1, b.y - px(2)), alpha(T::kRed, 0.8f), px(1.3f));
    } else if (playing) {
        const float hx = xOfSample(std::clamp(st.position, 0, lastSample));
        dl->AddLine(ImVec2(hx, a.y + px(2)), ImVec2(hx, b.y - px(2)), IM_COL32(255, 255, 255, 170), px(1.3f));
        int i = pointOf(st.position);
        if (steps)
            i = pointOf(st.position / lane.stepSamples * lane.stepSamples);
        if (lane.on[static_cast<std::size_t>(i)]) {
            const ImVec2 p(hx, yOf(value(i, amp, steps)));
            dl->AddCircleFilled(p, px(3), look.color);
            dl->AddCircle(p, px(3), IM_COL32_WHITE, 0, px(1));
        }
    }
    // Its name, top left; its length (and stretch, amplitude), top right; its CC, bottom right.
    text(dl, gBold, small, a.x + px(7), nameY, look.color, look.name, H::Left, V::Middle);
    if (st.locked) {
        drawLock(dl, a.x + px(11) + textWidth(gBold, small, look.name), nameY - px(0.5f), small, look.color);
    } else if (playing && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(a, b)) {
        // The lane under the mouse: an x by its name clears it alone.
        const float r = std::max(small * 0.55f, px(5));
        const ImVec2 xc(a.x + px(14) + textWidth(gBold, small, look.name) + r, nameY);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool over = std::hypot(mouse.x - xc.x, mouse.y - xc.y) <= r + px(2);
        dl->AddCircleFilled(xc, r, over ? IM_COL32(0x8A, 0x2A, 0x32, 255) : IM_COL32(0x46, 0x1E, 0x22, 255));
        dl->AddCircle(xc, r, T::kRed, 0, px(1));
        const float q = r * 0.45f;
        dl->AddLine(ImVec2(xc.x - q, xc.y - q), ImVec2(xc.x + q, xc.y + q), T::kText, px(1.4f));
        dl->AddLine(ImVec2(xc.x + q, xc.y - q), ImVec2(xc.x - q, xc.y + q), T::kText, px(1.4f));
        if (over) {
            laneClearHovered_ = LockRequest{Engine::sourceOfTrack(look.track), Engine::axisOfTrack(look.track)};
            ImGui::SetTooltip("Clear %s's loop", look.name);
        }
    }
    // A modulation: bottom left, the loop modulating it (and how deep), else the ones it modulates.
    if (playing && b.y - a.y >= px(30) && (st.modSource >= 0 || st.modTargets != 0)) {
        const bool target = st.modSource >= 0;
        std::string badge;
        int first = -1, count = 0;
        if (target) {
            badge = std::string(trackShortName(st.modSource)) + " \xE2\x80\xBA AMP " + std::to_string(st.modDepth) + "%";
        } else {
            for (int t = 0; t < Engine::kNoteTrack; ++t)
                if ((st.modTargets >> t) & 1u) {
                    badge += (count++ ? " \xC2\xB7 " : "\xE2\x80\xBA ") + std::string(trackShortName(t));
                    first = first < 0 ? t : first;
                }
            if (textWidth(gBold, small, badge) > (b.x - a.x) * 0.55f)  // no room: how many
                badge = "\xE2\x80\xBA " + std::to_string(count) + " loops";
        }
        const ImU32 tone = trackColor(target ? st.modSource : first);
        const float bw = textWidth(gBold, small, badge) + px(10);
        const ImVec2 pa(a.x + px(4), b.y - small - px(7)), pb(a.x + px(4) + bw, b.y - px(3));
        dl->AddRectFilled(pa, pb, IM_COL32(0x14, 0x16, 0x19, 220), px(6));
        dl->AddRect(pa, pb, alpha(tone, 0.8f), px(6), px(1));
        text(dl, gBold, small, (pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f, mix(tone, T::kText, 0.4f), badge, H::Center, V::Middle);
    }
    if (b.y - a.y >= px(30)) {
        std::string cc = look.cc == kLaneVelocity ? "VEL" : look.cc == kLaneTranspose ? "TR" : "CC " + std::to_string(look.cc);
        if (playing && st.modTargets != 0 && !st.modSendsCc)
            cc = "MOD";  // it only modulates
        const float ccW = textWidth(gCondensed, small, cc) + px(10);
        const ImVec2 pa(b.x - px(4) - ccW, b.y - small - px(7)), pb(b.x - px(4), b.y - px(3));
        dl->AddRectFilled(pa, pb, IM_COL32(0x14, 0x16, 0x19, 200), px(6));
        dl->AddRect(pa, pb, alpha(look.color, 0.7f), px(6), px(1));
        text(dl, gCondensed, small, (pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f, T::kText, cc, H::Center, V::Middle);
    }
    if (playing) {
        const std::string length = st.beats > 0 ? beatsText(st.beats, beatsPerBar_) : durationText(seconds * Engine::stretchFactor(st.stretch));
        const float chipY = nameY;
        float right = drawChip(dl, b.x - px(4), chipY, length, look.color) - px(4);
        // The stretch and the amplitude too, where they fit right of its name (and its lock or x).
        const float limit = a.x + px(7) + textWidth(gBold, small, look.name) + px(24);
        auto fits = [&](const std::string& label) { return right - (textWidth(gBold, px(10), label) + px(10)) >= limit; };
        if (b.y - a.y >= px(34)) {
            if (st.stretch != 0 && fits(stretchText(st.stretch)))
                right = drawStretchChip(dl, right, chipY, st.stretch, look.color) - px(4);
            if (st.amplitude != 100 && fits("AMP " + std::to_string(st.amplitude) + "%"))
                drawAmplitudeChip(dl, right, chipY, st.amplitude, look.color);
        }
        drawStretchGauge(dl, a, b, bit, st.stretch, look.color);
        drawAmplitudeGauge(dl, a, b, bit, st.amplitude, look.color);
        drawOffsetGauge(dl, a, b, bit, st.offset, look.color);
    } else if (recording) {  // how long it is so far
        drawChip(dl, b.x - px(4), nameY, durationText(seconds), T::kRed);
    }
    stretchWheel(bit, a, b);
}

void PerformanceView::drawTriggers(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& bl = m.axes[static_cast<std::size_t>(Axis::LeftTrigger)];
    const auto& br = m.axes[static_cast<std::size_t>(Axis::RightTrigger)];
    const auto& dl2 = m.buttons[static_cast<std::size_t>(Button::LeftTrigger)];
    const auto& dr2 = m.buttons[static_cast<std::size_t>(Button::RightTrigger)];
    const float top = panel(dl, x, y, w, h, "TRIGGERS", live.connected && (bl.enabled || br.enabled || dl2.isSet() || dr2.isSet()));
    const ScreenBox s = screenBox(x, top, w, h);
    const float vl = smooth(8, axisPosition01(live, Axis::LeftTrigger), 20.0f);
    const float vr = smooth(9, axisPosition01(live, Axis::RightTrigger), 20.0f);
    const bool soundL = live.isSounding(Engine::slotOf(Button::LeftTrigger));
    const bool soundR = live.isSounding(Engine::slotOf(Button::RightTrigger));

    screen(dl, s.a, s.b, T::kPink, [&] {
        const float W = s.b.x - s.a.x, Hh = s.b.y - s.a.y;
        const float base = s.b.y - Hh * 0.12f, travel = Hh * 0.72f;
        const float ty = base - travel * Engine::kTriggerPress;
        for (float dx = s.a.x; dx < s.b.x; dx += px(9))
            dl->AddLine(ImVec2(dx, ty), ImVec2(dx + px(5), ty), alpha(T::kPink, 0.55f), px(1.2f));
        auto envelope = [&](float x0, float x1, float v, bool on, const char* name) {
            const float peak = base - travel * v;
            const ImVec2 pts[] = {{x0, base}, {x0 + (x1 - x0) * 0.25f, peak}, {x0 + (x1 - x0) * 0.6f, peak}, {x1, base}};
            const ImU32 col = on ? T::kPink : IM_COL32(0xA9, 0xA1, 0xA6, 255);
            dl->AddConvexPolyFilled(pts, 4, alpha(col, 0.20f + 0.25f * v));
            dl->AddPolyline(pts, 4, col, px(2.2f));
            text(dl, gBold, px(13), (x0 + x1) * 0.5f, s.a.y + px(8), on ? T::kText : T::kLabel, name, H::Center);
        };
        envelope(s.a.x + W * 0.04f, s.a.x + W * 0.48f, vl, soundL, label(Button::LeftTrigger).c_str());
        envelope(s.a.x + W * 0.52f, s.a.x + W * 0.96f, vr, soundR, label(Button::RightTrigger).c_str());
    });
    if (hoverButton("##triggers", s.a, s.b))
        click("axes:left_trigger,right_trigger");

    // Per trigger: the CC it sends, and what it plays as a button (lit while it sounds).
    float rowY = s.b.y + px(18);
    section(dl, x, w, rowY, "MIDI");
    rowY += px(12);
    const ImU32 pinkLight = mix(T::kPink, T::kText, 0.5f);
    auto triggerRow = [&](float ry, Axis a, Button b, const AxisBinding& ab, const TriggerBinding& t, bool on) {
        const float rh = px(28), midY = ry + rh * 0.5f;
        text(dl, gBold, px(12), x + px(12), midY, T::kLabel, label(a == Axis::LeftTrigger ? Button::LeftTrigger : Button::RightTrigger), H::Left, V::Middle);
        const float ccX0 = x + px(38), ccW = px(52);
        badge(dl, gCondensed, px(13), ccX0 + ccW * 0.5f, midY - px(10), px(20), axisTarget(ab, live),
              T::kText, alpha(T::kPink, 0.22f), ab.enabled ? T::kPink : T::kDim);
        const std::string axisSpec = "axes:left_trigger,right_trigger";
        if (hoverButton(a == Axis::LeftTrigger ? "##l2cc" : "##r2cc", ImVec2(ccX0, ry), ImVec2(ccX0 + ccW, ry + rh)))
            click(axisSpec);
        const float bx0 = ccX0 + ccW + px(6), bx1 = x + w - px(12);
        row(dl, bx0, midY - px(10), bx1 - bx0, px(20), on ? alpha(T::kPink, 0.35f) : T::kRow, on ? T::kPink : T::kRowBorder);
        const std::string action = t.isSet() ? "button: " + compact(t, flats_) : "button: -";
        text(dl, gCondensed, px(13), (bx0 + bx1) * 0.5f, midY, on ? T::kText : T::kLabel,
             fit(gCondensed, px(13), action, bx1 - bx0 - px(8)), H::Center, V::Middle);
        if (hoverButton(b == Button::LeftTrigger ? "##l2" : "##r2", ImVec2(bx0, ry), ImVec2(bx1, ry + rh)))
            click(b == Button::LeftTrigger ? "button:left_trigger" : "button:right_trigger");
    };
    triggerRow(rowY, Axis::LeftTrigger, Button::LeftTrigger, bl, dl2, soundL);
    triggerRow(rowY + px(30), Axis::RightTrigger, Button::RightTrigger, br, dr2, soundR);

    // Loop: the triggers' travel a Loop record button captured, L2 and R2.
    const float loopY = s.b.y + loopOffset();
    if (y + h - px(10) - (loopY + px(10)) < px(28))
        return;
    constexpr int kTriggers = static_cast<int>(LoopSource::Triggers);
    const LiveState::Loop& loop = live.loops[static_cast<std::size_t>(kTriggers)];
    const LoopBinding& lb = m.loops[static_cast<std::size_t>(kTriggers)];
    const std::string leftName = label(Button::LeftTrigger), rightName = label(Button::RightTrigger);
    const LaneLook lanes[2] = {{&loop.lanes[0], Engine::trackOf(kTriggers, 0), T::kPink, leftName.c_str(), bl.enabled, bl.invert, false, laneTarget(bl, lb.ccX), lb.steps[0]},
                               {&loop.lanes[1], Engine::trackOf(kTriggers, 1), pinkLight, rightName.c_str(), br.enabled, br.invert, false, laneTarget(br, lb.ccY), lb.steps[1]}};
    drawLoopPanel(dl, x, w, loopY, y + h - px(10), loop.status, lb.playback, lanes, 2, T::kPink, artFor(live.channel, 2),
                  "REC", "##looptriggers", "axes:left_trigger,right_trigger", kTriggers);
}

void PerformanceView::drawDpad(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool any = false;
    for (const auto& t : m.dpad)
        any |= t.isSet();
    const float top = panel(dl, x, y, w, h, "D-PAD", live.connected && any);
    const ScreenBox s = screenBox(x, top, w, h);
    const auto current = liveHat(live);
    static constexpr std::optional<HatDir> kGrid[3][3] = {
        {HatDir::UpLeft, HatDir::Up, HatDir::UpRight},
        {HatDir::Left, std::nullopt, HatDir::Right},
        {HatDir::DownLeft, HatDir::Down, HatDir::DownRight},
    };

    screen(dl, s.a, s.b, IM_COL32(0x9A, 0x9A, 0xA4, 255), [&] {
        // Isometric cubes; the direction played rises and turns red.
        const float W = s.b.x - s.a.x, Hh = s.b.y - s.a.y;
        const float sz = std::min(W / 7.0f, Hh / 5.6f);
        const ImVec2 c((s.a.x + s.b.x) * 0.5f, s.a.y + Hh * 0.30f);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const auto dir = kGrid[row][col];
                const int id = 20 + row * 3 + col;
                const bool active = dir && current == dir;
                const bool sounding = dir && live.isSounding(Engine::slotOf(*dir));
                const float lift = smooth(id, active ? 1.0f : sounding ? 0.45f : 0.0f, 16.0f) * sz * 0.9f;
                const ImVec2 base(c.x + static_cast<float>(col - row) * sz, c.y + static_cast<float>(col + row) * sz * 0.5f);
                const float depth = sz * 0.55f + lift;
                const ImVec2 tp(base.x, base.y - lift);
                ImU32 face = dir ? IM_COL32(0xE6, 0xE6, 0xEA, 255) : IM_COL32(0x6E, 0x6E, 0x74, 255);
                if (dir && !m.dpad[static_cast<std::size_t>(*dir)].isSet())
                    face = IM_COL32(0x96, 0x96, 0x9B, 255);
                face = mix(face, T::kRed, smooth(id + 10, active ? 1.0f : sounding ? 0.6f : 0.0f, 12.0f));
                const ImVec2 t0(tp.x, tp.y - sz * 0.5f), t1(tp.x + sz, tp.y), t2(tp.x, tp.y + sz * 0.5f), t3(tp.x - sz, tp.y);
                const ImVec2 left[] = {t3, t2, ImVec2(t2.x, t2.y + depth), ImVec2(t3.x, t3.y + depth)};
                const ImVec2 right[] = {t2, t1, ImVec2(t1.x, t1.y + depth), ImVec2(t2.x, t2.y + depth)};
                const ImVec2 topFace[] = {t0, t1, t2, t3};
                dl->AddConvexPolyFilled(left, 4, shade(face, 0.62f));
                dl->AddConvexPolyFilled(right, 4, shade(face, 0.42f));
                dl->AddConvexPolyFilled(topFace, 4, face);
            }
        }
        if (current) {
            const float band = px(28);
            dl->AddRectFilled(ImVec2(s.a.x, s.b.y - band), s.b, IM_COL32(0, 0, 0, 170));
            const std::string caption = label(*current) + "  ·  " + compact(m.dpad[static_cast<std::size_t>(*current)], flats_);
            text(dl, gBold, px(14), (s.a.x + s.b.x) * 0.5f, s.b.y - band * 0.5f, T::kText,
                 fit(gBold, px(14), caption, W - px(12)), H::Center, V::Middle);
        }
    });
    if (hoverButton("##dpadscreen", s.a, s.b) && current)
        click("dpad:" + std::string(toString(*current)));

    // The eight directions and what they play.
    static constexpr HatDir kOrder[] = {HatDir::Up,   HatDir::UpRight,  HatDir::Right, HatDir::DownRight,
                                        HatDir::Down, HatDir::DownLeft, HatDir::Left,  HatDir::UpLeft};
    const float listTop = s.b.y + px(14);
    const float rowH = std::min(px(35), (y + h - px(8) - listTop) / 8.0f);
    for (int i = 0; i < 8; ++i) {
        const HatDir d = kOrder[i];
        const float ry = listTop + rowH * static_cast<float>(i);
        const float rh = rowH - px(3);
        const bool active = current == d;
        const bool sounding = live.isSounding(Engine::slotOf(d));
        const auto& t = m.dpad[static_cast<std::size_t>(d)];
        const bool toggle = t.isSet() && t.mode == Mode::Toggle;
        if (active || sounding)
            row(dl, x + px(8), ry, w - px(16), rh, alpha(T::kRed, active ? 0.30f : 0.16f), alpha(T::kRed, active ? 1.0f : 0.6f));
        led(dl, ImVec2(x + px(20), ry + rh * 0.5f), px(4.5f), T::kRed, active || sounding);
        text(dl, gMedium, px(13), x + px(32), ry + rh * 0.5f, active || sounding ? T::kText : T::kLabel, label(d), H::Left,
             V::Middle);
        const float right = x + w - px(toggle ? 36 : 14);
        const float bh = std::min(px(22), rh - px(4));
        badge(dl, gCondensed, px(14), right, ry + (rh - bh) * 0.5f, bh,
              fit(gCondensed, px(14), compact(t, flats_), w * 0.45f), t.isSet() ? T::kText : T::kDim, kBadge, 0, H::Right);
        if (toggle)
            toggleMark(dl, ImVec2(x + w - px(22), ry + rh * 0.5f), px(6), active || sounding ? T::kRed : T::kLabel);
        if (hoverButton(("##dir" + std::to_string(i)).c_str(), ImVec2(x + px(8), ry), ImVec2(x + w - px(8), ry + rh)))
            click("dpad:" + std::string(toString(d)));
    }
}

void PerformanceView::drawTouchpad(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool any = m.fingers[0].mode != TouchMode::Off || m.fingers[1].mode != TouchMode::Off;
    const float top = panel(dl, x, y, w, h, tabbed_ ? "" : "TOUCHPAD", live.connected && live.hasTouchpad && any);
    if (tabbed_)
        drawModuleTabs(dl, x, y);
    const ScreenBox s = screenBox(x, top, w, h);
    const ImU32 fingerColors[] = {T::kCyan, T::kOrange};

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

    screen(dl, s.a, s.b, T::kPink, [&] {
        const float W = s.b.x - s.a.x, Hh = s.b.y - s.a.y;
        for (float gx = s.a.x + px(6); gx < s.b.x; gx += px(12))
            for (float gy = s.a.y + px(6); gy < s.b.y; gy += px(12))
                dl->AddCircleFilled(ImVec2(gx, gy), 1.0f, IM_COL32(255, 255, 255, 26));
        dl->AddLine(ImVec2(s.a.x + W * 0.5f, s.a.y), ImVec2(s.a.x + W * 0.5f, s.b.y), IM_COL32(255, 255, 255, 40));
        dl->AddLine(ImVec2(s.a.x, s.a.y + Hh * 0.5f), ImVec2(s.b.x, s.a.y + Hh * 0.5f), IM_COL32(255, 255, 255, 40));
        for (int f = 0; f < kFingerCount; ++f) {
            if (m.fingers[static_cast<std::size_t>(f)].mode != TouchMode::Zones)
                continue;
            for (int z = 0; z < kZoneCount; ++z) {
                const float on = smooth(40 + f * 4 + z, live.isSounding(Engine::slotOf(f, static_cast<Zone>(z))) ? 1.0f : 0.0f, 10.0f);
                if (on < 0.01f)
                    continue;
                const float zx = s.a.x + static_cast<float>(z % 2) * W * 0.5f, zy = s.a.y + static_cast<float>(z / 2) * Hh * 0.5f;
                dl->AddRectFilled(ImVec2(zx + 2, zy + 2), ImVec2(zx + W * 0.5f - 2, zy + Hh * 0.5f - 2),
                                  alpha(fingerColors[f], 0.30f * on), px(3));
            }
        }
        for (int f = 0; f < kFingerCount; ++f) {
            const auto& trail = trails_[static_cast<std::size_t>(f)];
            for (std::size_t i = 1; i < trail.points.size(); ++i) {
                const float age = static_cast<float>((time_ - trail.times[i]) / 0.45);
                const ImVec2 a(s.a.x + trail.points[i - 1].first * W, s.a.y + trail.points[i - 1].second * Hh);
                const ImVec2 b(s.a.x + trail.points[i].first * W, s.a.y + trail.points[i].second * Hh);
                dl->AddLine(a, b, alpha(fingerColors[f], 0.8f * (1.0f - age)), px(3) * (1.0f - age) + 1.0f);
            }
            const auto& finger = live.fingers[static_cast<std::size_t>(f)];
            if (finger.down) {
                const ImVec2 c(s.a.x + finger.x * W, s.a.y + finger.y * Hh);
                const float pulse = 1.0f + 0.15f * std::sin(static_cast<float>(time_) * 9.0f);
                dl->AddCircleFilled(c, px(16) * pulse, alpha(fingerColors[f], 0.20f));
                dl->AddCircleFilled(c, px(7), fingerColors[f]);
                text(dl, gBold, px(13), c.x + px(10), c.y - px(22), T::kText, f == 0 ? "1" : "2");
            }
        }
        if (!live.hasTouchpad && live.connected)
            text(dl, gMedium, px(13), (s.a.x + s.b.x) * 0.5f, s.a.y + Hh * 0.45f, T::kLabel, "touchpad not detected",
                 H::Center, V::Middle);
    });
    if (live.stretchHeld && (m.fingers[0].mode == TouchMode::Axis || m.fingers[1].mode == TouchMode::Axis))
        shiftModeBadge(dl, s.a, s.b, live.fingerShape, fingerColors[0]);
    if (hoverButton("##touchscreen", s.a, s.b))
        click("fingers");

    // Per finger, what it sends: its CCs in axis mode, its four zones.
    float rowY = s.b.y + px(18);
    section(dl, x, w, rowY, "MIDI");
    rowY += px(12);
    for (int f = 0; f < kFingerCount; ++f) {
        const FingerBinding& fb = m.fingers[static_cast<std::size_t>(f)];
        const auto& finger = live.fingers[static_cast<std::size_t>(f)];
        const float rowH = px(28);
        const float midY = rowY + rowH * 0.5f;
        const ImU32 col = fingerColors[f];
        text(dl, gBold, px(11), x + px(12), midY, fb.mode != TouchMode::Off && finger.down ? T::kLabel : T::kDim,
             f == 0 ? "FINGER 1" : "FINGER 2", H::Left, V::Middle);
        const float cx0 = x + px(80), cx1 = x + w - px(12);
        if (fb.mode == TouchMode::Axis) {
            const float cw = (cx1 - cx0 - px(6)) * 0.5f;
            const AxisBinding* axes[] = {&fb.x, &fb.y};
            for (int k = 0; k < 2; ++k) {
                const AxisBinding& b = *axes[k];
                const std::string label = std::string(k == 0 ? "X  " : "Y  ") + axisTarget(b, live);
                badge(dl, gCondensed, px(13), cx0 + cw * 0.5f + static_cast<float>(k) * (cw + px(6)), midY - px(10), px(20), label,
                      T::kText, alpha(col, finger.down ? 0.30f : 0.16f), b.enabled ? col : T::kDim);
            }
        } else if (fb.mode == TouchMode::Zones) {  // the four pads in a row: TL, TR, BL, BR
            const float cw = (cx1 - cx0 - px(9)) / 4.0f, ch = px(20);
            for (int z = 0; z < kZoneCount; ++z) {
                const ImVec2 za(cx0 + static_cast<float>(z) * (cw + px(3)), midY - ch * 0.5f);
                const ImVec2 zb(za.x + cw, za.y + ch);
                const bool on = live.isSounding(Engine::slotOf(f, static_cast<Zone>(z)));
                row(dl, za.x, za.y, cw, ch, on ? alpha(col, 0.35f) : T::kRow, on ? col : T::kRowBorder);
                text(dl, gCondensed, px(12), (za.x + zb.x) * 0.5f, (za.y + zb.y) * 0.5f, on ? T::kText : T::kLabel,
                     fit(gCondensed, px(12), compact(fb.zones[static_cast<std::size_t>(z)], flats_), cw - px(6)), H::Center,
                     V::Middle);
                const std::string spec = "zone:" + std::to_string(f + 1) + ":" + std::string(toString(static_cast<Zone>(z)));
                if (hoverButton(("##" + spec).c_str(), za, zb))
                    click(spec);
            }
        } else {
            text(dl, gMedium, px(12), cx0, midY, T::kDim, "off", H::Left, V::Middle);
        }
        if (fb.mode != TouchMode::Zones && hoverButton(f == 0 ? "##f1" : "##f2", ImVec2(x + px(8), rowY), ImVec2(x + w - px(8), rowY + rowH)))
            click(f == 0 ? "finger:1" : "finger:2");
        rowY += rowH + px(4);
    }

    // Loop: the finger movements a Loop record button captured, drawn as the
    // paths they took on the touchpad, with a timeline of when each touched.
    const float loopY = s.b.y + loopOffset();
    const float timelineH = px(20);  // inside the box, at its bottom
    const float boxTop = loopY + px(10), boxBottom = y + h - px(10);
    if (boxBottom - boxTop < px(40))
        return;
    // In 4-pad mode the pads play notes, which the note loop records: the box
    // shows them from it, one lane per pad.
    if (m.fingers[0].mode == TouchMode::Zones || m.fingers[1].mode == TouchMode::Zones) {
        drawPadLanes(dl, m, live, x, w, loopY, boxTop, boxBottom, timelineH);
        return;
    }
    // A lane per axis of each finger in axis mode; finger 2's only once it holds a loop.
    const LiveState::Loop* loops[] = {&live.loops[2], &live.loops[3]};
    LaneLook lanes[4];
    int laneCount = 0;
    LoopStatus st;  // both fingers together
    for (int f = 0; f < kFingerCount; ++f) {
        const FingerBinding& fb = m.fingers[static_cast<std::size_t>(f)];
        const LoopStatus& ls = loops[f]->status;
        if (fb.mode != TouchMode::Axis && ls.state == LoopStatus::State::Empty)
            continue;
        if (f == 1 && laneCount > 0 && ls.state == LoopStatus::State::Empty && !ls.waiting)
            continue;
        const int source = static_cast<int>(LoopSource::Finger1) + f;
        const LoopBinding& lb = m.loops[static_cast<std::size_t>(source)];
        const bool axisMode = fb.mode == TouchMode::Axis;
        lanes[laneCount++] = {&loops[f]->lanes[0], Engine::trackOf(source, 0), fingerColors[f], f == 0 ? "1 X" : "2 X",
                              axisMode && fb.x.enabled, fb.x.invert, false, laneTarget(fb.x, lb.ccX), lb.steps[0]};
        lanes[laneCount++] = {&loops[f]->lanes[1], Engine::trackOf(source, 1), mix(fingerColors[f], T::kText, 0.45f), f == 0 ? "1 Y" : "2 Y",
                              axisMode && fb.y.enabled, fb.y.invert, true, laneTarget(fb.y, lb.ccY), lb.steps[1]};
        if (ls.state == LoopStatus::State::Recording || (ls.state == LoopStatus::State::Playing && st.state != LoopStatus::State::Recording))
            st.state = ls.state;
        st.waiting = st.waiting || ls.waiting;
        st.locked = st.locked || ls.locked;
        st.length = std::max(st.length, ls.length);
    }
    if (laneCount == 0) {  // both fingers off: the screen all the same, and why it waits
        drawLoopPanel(dl, x, w, loopY, boxBottom, LoopStatus{}, m.loops[2].playback, lanes, 0, fingerColors[0], artFor(live.channel, 3), "", "##touchloop",
                      "fingers", static_cast<int>(LoopSource::Finger1));
        text(dl, gMedium, px(10), x + w * 0.5f, boxBottom - px(14), alpha(T::kLabel, 0.8f), "touchpad off", H::Center, V::Middle);
        return;
    }
    const int shown = loops[0]->status.state != LoopStatus::State::Empty ? 0 : 1;
    drawLoopPanel(dl, x, w, loopY, boxBottom, st, m.loops[static_cast<std::size_t>(2 + shown)].playback, lanes, laneCount,
                  fingerColors[shown], artFor(live.channel, 3), "REC", "##touchloop", "fingers",
                  static_cast<int>(LoopSource::Finger1) + shown);
}

void PerformanceView::drawModuleTabs(ImDrawList* dl, float x, float y) {
    const float size = px(gLook == 2 ? 12.5f : 13.5f), midY = y + px(18);
    const ImU32 lit = gLook == 1 ? IM_COL32(0xB8, 0xEC, 0xFF, 255) : T::kText;
    const float padW = textWidth(gBold, size, "TOUCHPAD"), tiltX = x + px(24) + padW;
    text(dl, gBold, size, x + px(12), midY, tiltTab_ ? T::kDim : lit, "TOUCHPAD", H::Left, V::Middle);
    text(dl, gBold, size, tiltX - px(6), midY, T::kDim, "\xC2\xB7", H::Center, V::Middle);
    text(dl, gBold, size, tiltX, midY, tiltTab_ ? lit : T::kDim, "TILT", H::Left, V::Middle);
    const float tabX = tiltTab_ ? tiltX : x + px(12), tabW = tiltTab_ ? textWidth(gBold, size, "TILT") : padW;
    dl->AddLine(ImVec2(tabX, y + px(29)), ImVec2(tabX + tabW, y + px(29)), tiltTab_ ? T::kViolet : T::kCyan, px(2));
    if (hoverButton("##tabTouchpad", ImVec2(x + px(6), y + px(4)), ImVec2(tiltX - px(10), y + px(32))))
        tiltTab_ = false;
    if (hoverButton("##tabTilt", ImVec2(tiltX - px(4), y + px(4)), ImVec2(tiltX + textWidth(gBold, size, "TILT") + px(6), y + px(32))))
        tiltTab_ = true;
}

// The tilt: a ball rolling in a dish towards the side that dips (dimmed
// while the tilt waits for its button, or is off), its CCs, what makes it
// move, and the loops of its movements.
void PerformanceView::drawTilt(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent = T::kViolet;
    const TiltBinding& t = m.tilt;
    const bool on = t.enabled, active = on && live.tiltActive;
    const float top = panel(dl, x, y, w, h, tabbed_ ? "" : "TILT", live.connected && on);
    if (tabbed_)
        drawModuleTabs(dl, x, y);
    const ScreenBox s = screenBox(x, top, w, h);
    const float tx = smooth(60, (live.tiltX - 0.5f) * 2.0f, 14.0f), ty = smooth(61, (live.tiltY - 0.5f) * 2.0f, 14.0f);
    screen(dl, s.a, s.b, accent, [&] {
        const float rw = (s.b.x - s.a.x) * 0.5f, rh = (s.b.y - s.a.y) * 0.5f;
        const float lit = active ? 1.0f : on ? 0.55f : 0.3f;
        const ImVec2 bc((s.a.x + s.b.x) * 0.5f, (s.a.y + s.b.y) * 0.5f + rh * 0.12f);
        for (int k = 5; k >= 1; --k) {  // the dish
            const float f = static_cast<float>(k) / 5.0f;
            dl->AddEllipseFilled(bc, ImVec2(rw * 0.9f * f, rh * 0.42f * f), alpha(accent, (0.05f + 0.05f * (1 - f)) * lit));
            dl->AddEllipse(bc, ImVec2(rw * 0.9f * f, rh * 0.42f * f), alpha(accent, 0.35f * lit), 0, 48, px(1));
        }
        // The ball, its shadow under it: forward rolls it away, up the screen,
        // smaller; towards the player, larger.
        const float br = rh * 0.2f * (1.0f - 0.28f * ty);
        const ImVec2 at(bc.x + tx * rw * 0.62f, bc.y - ty * rh * 0.3f - br * 0.6f);
        dl->AddEllipseFilled(ImVec2(at.x, at.y + br * 0.95f), ImVec2(br * 0.9f, br * 0.28f), IM_COL32(0, 0, 0, static_cast<int>(110 * lit)));
        for (int k = 8; k >= 1; --k) {
            const float f = static_cast<float>(k) / 8.0f;
            dl->AddCircleFilled(ImVec2(at.x - br * 0.3f * (1 - f), at.y - br * 0.3f * (1 - f)), br * f, alpha(mix(accent, T::kText, 1 - f), lit));
        }
        if (!on)
            badge(dl, gBold, px(12), bc.x, s.b.y - px(30), px(22), "OFF", T::kLabel, IM_COL32(0x2A, 0x2D, 0x32, 255), T::kDim);
    });
    if (hoverButton("##tiltscreen", s.a, s.b))
        click("tilt");

    // What it sends.
    const float midiY = s.b.y + px(18);
    section(dl, x, w, midiY, "MIDI");
    const float chipTop = midiY + px(12), chipH = px(20);
    const float cw = (w - px(30)) * 0.5f;
    auto chip = [&](float cx0, const AxisBinding& b, const char* name, const char* id) {
        const std::string text = std::string(name) + "  " + axisTarget(b, live) + (b.invert ? "  inv" : "");
        const bool lit = on && b.enabled;
        badge(dl, gCondensed, px(13), cx0 + cw * 0.5f, chipTop, chipH, text, lit ? T::kText : T::kDim, alpha(accent, lit ? 0.22f : 0.06f),
              lit ? accent : T::kDim);
        if (hoverButton(id, ImVec2(cx0, chipTop - px(2)), ImVec2(cx0 + cw, chipTop + chipH + px(2))))
            click("tilt");
    };
    chip(x + px(12), t.x, "X", "##tiltX");
    chip(x + px(18) + cw, t.y, "Y", "##tiltY");

    // What makes it move: always, or while its button is held.
    const float fy = chipTop + chipH + px(8), fh = px(28);
    if (fy + fh > y + h - px(10))
        return;
    row(dl, x + px(12), fy, w - px(24), fh, active ? alpha(accent, 0.18f) : T::kRow, active ? accent : T::kRowBorder);
    const ImVec2 dot(x + px(28), fy + fh * 0.5f);
    if (active)
        dl->AddCircleFilled(dot, px(5.5f), accent);
    else
        dl->AddCircle(dot, px(5.5f), on ? accent : T::kDim, 0, px(2));
    const std::string how = !on ? "tilt off" : t.whileHeld ? "while " + label(t.hold) + " held" : "always";
    text(dl, gBold, px(13), x + px(42), fy + fh * 0.5f, on ? T::kText : T::kDim, how, H::Left, V::Middle);
    if (on)
        text(dl, active ? gBold : gMedium, px(12), x + w - px(22), fy + fh * 0.5f, active ? accent : T::kLabel, active ? "active" : "idle",
             H::Right, V::Middle);
    if (hoverButton("##tilthow", ImVec2(x + px(12), fy), ImVec2(x + w - px(12), fy + fh)))
        click("tilt");

    // Loop: the tilt's movements a Loop record button captured.
    const float loopY = s.b.y + loopOffset();
    if (y + h - px(10) - (loopY + px(10)) < px(28))
        return;
    constexpr int kSource = static_cast<int>(LoopSource::Tilt);
    const LiveState::Loop& loop = live.loops[static_cast<std::size_t>(kSource)];
    const LoopBinding& lb = m.loops[static_cast<std::size_t>(kSource)];
    const LaneLook lanes[2] = {
        {&loop.lanes[0], Engine::trackOf(kSource, 0), accent, "X", on && t.x.enabled, t.x.invert, false, laneTarget(t.x, lb.ccX), lb.steps[0]},
        {&loop.lanes[1], Engine::trackOf(kSource, 1), mix(accent, T::kText, 0.5f), "Y", on && t.y.enabled, t.y.invert, false, laneTarget(t.y, lb.ccY), lb.steps[1]}};
    drawLoopPanel(dl, x, w, loopY, y + h - px(10), loop.status, lb.playback, lanes, 2, accent, artFor(live.channel, 3),
                  "REC", "##looptilt", "tilt", kSource);
}

// The touchpad's LOOP panel in 4-pad mode: the pads' notes in the note loop,
// one lane per pad (TL, TR, BL, BR), with a playhead, and below when each
// finger's pads played.
void PerformanceView::drawPadLanes(ImDrawList* dl, const Mapping& m, const LiveState& live, float x, float w, float loopY,
                                   float boxTop, float boxBottom, float timelineH) {
    const ImU32 fingerColors[] = {T::kCyan, T::kOrange};
    const ImU32 kViolet = T::kViolet;
    const LoopStatus& st = live.loops[static_cast<std::size_t>(LoopSource::Notes)].status;
    const bool recording = st.state == LoopStatus::State::Recording;
    const bool playing = st.state == LoopStatus::State::Playing;

    // Which lane, and which finger, plays each note.
    std::array<std::int8_t, 128> laneOf, fingerOf;
    laneOf.fill(-1);
    fingerOf.fill(-1);
    for (int f = kFingerCount - 1; f >= 0; --f) {  // finger 1 wins a note both play
        const FingerBinding& fb = m.fingers[static_cast<std::size_t>(f)];
        if (fb.mode != TouchMode::Zones)
            continue;
        for (int z = 0; z < kZoneCount; ++z) {
            const TriggerBinding& t = fb.zones[static_cast<std::size_t>(z)];
            auto mark = [&](int note) {
                laneOf[static_cast<std::size_t>(note & 0x7F)] = static_cast<std::int8_t>(z);
                fingerOf[static_cast<std::size_t>(note & 0x7F)] = static_cast<std::int8_t>(f);
            };
            if (t.type == ActionType::Note)
                mark(t.number);
            else if (t.type == ActionType::Chord)
                for (auto n : t.chord.view())
                    mark(n);
        }
    }

    // Header: the note loop's state.
    char seconds[16];
    std::snprintf(seconds, sizeof seconds, "%.1f s", static_cast<double>(st.length) / Engine::kLoopRateHz);
    std::string status;
    ImU32 statusColor = T::kDim;
    if (recording) {
        status = std::string("REC  ") + seconds;
        statusColor = T::kRed;
    } else if (st.waiting) {
        status = "REC";
        statusColor = T::kRed;
    } else if (playing) {
        const LoopPlayback pb = m.loops[static_cast<std::size_t>(LoopSource::Notes)].playback;
        status = std::string(st.overdub ? "OVERDUB  " : "") +
                 (pb == LoopPlayback::Forward ? "FWD" : pb == LoopPlayback::Reverse ? "REV" : "B&F") + "  \xC2\xB7  " + seconds;
        statusColor = st.overdub ? T::kOrange : kViolet;
    }
    text(dl, gBold, px(11), x + px(12), loopY, T::kDim, "LOOP", H::Left, V::Middle);
    const float statusX = x + w - px(12) - textWidth(gBold, px(10), status);
    if (!status.empty())
        text(dl, gBold, px(10), statusX, loopY, statusColor, status, H::Left, V::Middle);
    const float ruleEnd = status.empty() ? x + w - px(12) : statusX - px(8);
    float ruleStart = x + px(16) + textWidth(gBold, px(11), "LOOP");
    if (st.locked) {
        drawLock(dl, ruleStart - px(1), loopY - px(0.5f), px(11), kViolet);
        ruleStart += px(14);
    }
    if (ruleEnd > ruleStart)
        dl->AddLine(ImVec2(ruleStart, loopY), ImVec2(ruleEnd, loopY), IM_COL32(0x40, 0x44, 0x4B, 255), px(1));

    const ImVec2 ga(x + px(12), boxTop), gb(x + w - px(12), boxBottom);
    dl->AddRectFilled(ga, gb, kGraph, px(4));
    if (st.locked && playing)
        drawLockedBorder(dl, ga, gb, kViolet, px(4));
    else
        dl->AddRect(ga, gb, recording ? T::kRed : st.waiting ? alpha(T::kRed, 0.5f) : playing ? alpha(kViolet, 0.45f) : kGraph, px(4),
                    px(1.3f));
    const ImVec2 pa(ga.x + px(6), ga.y + px(6)), pb(gb.x - px(6), gb.y - px(6) - timelineH);
    static constexpr const char* kPadNames[] = {"TL", "TR", "BL", "BR"};
    const float laneH = (pb.y - pa.y) / kZoneCount;
    const float lx0 = pa.x + px(18), lx1 = pb.x;
    for (int z = 0; z < kZoneCount; ++z) {
        const float ly = pa.y + laneH * static_cast<float>(z);
        text(dl, gBold, px(9), pa.x, ly + laneH * 0.5f, T::kDim, kPadNames[z], H::Left, V::Middle);
        if (z > 0)
            dl->AddLine(ImVec2(lx0, ly), ImVec2(lx1, ly), IM_COL32(255, 255, 255, 18));
    }

    bool any = false;
    if ((recording || playing) && st.length > 1) {
        // While recording the loop grows over a 4-second window, then fills the box.
        const float span = recording ? std::min(1.0f, static_cast<float>(st.length) / (4.0f * Engine::kLoopRateHz)) : 1.0f;
        auto xAt = [&](float sample) { return lx0 + (lx1 - lx0) * span * sample / static_cast<float>(st.length); };
        const float tx0 = ga.x + px(2), tx1 = gb.x - px(2);
        auto tAt = [&](float sample) { return tx0 + (tx1 - tx0) * span * sample / static_cast<float>(st.length); };
        for (int i = 0; i < live.noteLoopCount; ++i) {
            const NoteInterval& n = live.noteLoop[static_cast<std::size_t>(i)];
            const int lane = laneOf[n.note], f = fingerOf[n.note];
            if (lane < 0)
                continue;  // a button's note: the piano roll shows it
            any = true;
            const float end = n.open ? static_cast<float>(recording || st.position < n.start ? st.length : st.position)
                                     : static_cast<float>(n.end);
            const ImU32 col = n.open && (st.overdub || recording) ? T::kOrange : fingerColors[f];
            const float top = pa.y + laneH * static_cast<float>(lane) + px(2), bottom = top + laneH - px(4);
            dl->AddRectFilled(ImVec2(xAt(static_cast<float>(n.start)), top), ImVec2(std::max(xAt(end), xAt(static_cast<float>(n.start)) + px(2)), bottom),
                              alpha(col, 0.8f), px(2));
            // The timeline: when each finger's pads played.
            const float ty = gb.y - timelineH + px(5) + static_cast<float>(f) * px(8);
            dl->AddRectFilled(ImVec2(tAt(static_cast<float>(n.start)), ty - px(2.5f)),
                              ImVec2(std::max(tAt(end), tAt(static_cast<float>(n.start)) + px(2)), ty + px(2.5f)), fingerColors[f], px(2));
        }
        const float hx = recording ? xAt(static_cast<float>(st.length)) : xAt(static_cast<float>(st.position));
        dl->AddLine(ImVec2(hx, pa.y), ImVec2(hx, pb.y), recording ? alpha(T::kRed, 0.8f) : st.overdub ? T::kOrange : IM_COL32(255, 255, 255, 190),
                    px(st.overdub ? 2.0f : 1.3f));
    }
    if (!any)
        idleArt(dl, artFor(live.channel, 3), ga, gb, time_);
    if (hoverButton("##touchloop", ImVec2(ga.x, loopY - px(6)), gb))
        click("notes");
    menuOnRightClick(MenuTarget{MenuTarget::Kind::NoteLoop, 0, 0, static_cast<int>(LoopSource::Notes), -1, "notes"}, playing, st.locked);
}

void PerformanceView::drawButtons(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool any = false;
    for (const auto& t : m.buttons)
        any |= t.isSet();
    // No title bar either: the pads, their names and what they play.
    (void)any;
    const float top = y;
    if (gLook) {
        frame(dl, ImVec2(x, y), ImVec2(x + w, y + h), 0, kNeon);
    } else {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, px(8));
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, px(8), px(1));
    }

    struct Pad {
        Button button;
        ImU32 color;  // face buttons only
    };
    static constexpr Pad kPads[] = {
        {Button::LeftPaddle1, 0},  {Button::LeftPaddle2, 0},  {Button::LeftShoulder, 0}, {Button::LeftTrigger, 0}, {Button::LeftStick, 0},
        {Button::Back, 0},         {Button::South, kPsCross}, {Button::East, kPsCircle}, {Button::West, kPsSquare}, {Button::North, kPsTriangle},
        {Button::Start, 0},        {Button::Guide, 0},        {Button::Touchpad, 0},     {Button::Misc1, 0},        {Button::RightStick, 0},
        {Button::RightTrigger, 0}, {Button::RightShoulder, 0}, {Button::RightPaddle2, 0}, {Button::RightPaddle1, 0},
    };
    // The controller's buttons (the paddles only when it has them), its face buttons in its family's colours.
    const ControllerFamily family = labelFamily();
    std::vector<Pad> pads;
    for (Pad p : kPads) {
        const bool paddle = p.button == Button::LeftPaddle1 || p.button == Button::LeftPaddle2 || p.button == Button::RightPaddle1 ||
                            p.button == Button::RightPaddle2;
        if (!live.has(p.button) || (paddle && !live.connected))
            continue;
        if (p.color && family == ControllerFamily::Xbox)
            p.color = p.button == Button::South ? IM_COL32(0x5C, 0xC8, 0x5C, 255) : p.button == Button::East ? IM_COL32(0xF0, 0x5A, 0x5A, 255)
                    : p.button == Button::West  ? IM_COL32(0x4D, 0x9C, 0xFF, 255) : IM_COL32(0xF2, 0xC0, 0x3C, 255);
        else if (p.color && family != ControllerFamily::PlayStation)
            p.color = kPadGrey;
        pads.push_back(p);
    }
    const float pad = px(12);
    const float slot = (w - 2 * pad) / static_cast<float>(std::max<std::size_t>(pads.size(), 1));
    const float body = y + h - top;
    const float cy = top + body * 0.30f;
    for (std::size_t i = 0; i < pads.size(); ++i) {
        const Pad& pd = pads[i];
        const bool face = pd.color != 0;
        const float cx = x + pad + slot * (static_cast<float>(i) + 0.5f);
        const float r = std::min({slot * (face ? 0.42f : 0.33f), px(face ? 30 : 23), body * (face ? 0.19f : 0.155f)});
        const ImU32 color = face ? pd.color : kPadGrey;
        bool down = false;
        if (pd.button == Button::LeftTrigger)
            down = axisPosition01(live, Axis::LeftTrigger) >= Engine::kTriggerPress;
        else if (pd.button == Button::RightTrigger)
            down = axisPosition01(live, Axis::RightTrigger) >= Engine::kTriggerPress;
        else
            down = live.isDown(pd.button);
        const bool sounding = live.isSounding(Engine::slotOf(pd.button));
        const ImVec2 c(cx, cy);

        // Body: filled with its color while sounding, tinted otherwise.
        if (sounding) {
            dl->AddCircleFilled(c, r * 1.5f, alpha(color, 0.18f));
            dl->AddCircleFilled(c, r, color);
        } else {
            dl->AddCircleFilled(c, r, face ? alpha(color, down ? 0.35f : 0.15f) : (down ? IM_COL32(0x55, 0x5A, 0x63, 255) : IM_COL32(0x3B, 0x40, 0x47, 255)));
        }
        dl->AddCircle(c, r, face || sounding ? color : IM_COL32(0x8C, 0x92, 0x9A, 255), 0, px(2.2f));
        const ImU32 symbol = sounding ? IM_COL32(0x10, 0x12, 0x15, 255) : face ? color : T::kText;
        const float s = r * 0.42f, th = r * 0.11f;
        if (face && (family == ControllerFamily::Xbox || family == ControllerFamily::Nintendo)) {  // its letter
            text(dl, gBold, r * 0.95f, cx, cy, symbol, label(pd.button), H::Center, V::Middle);
        } else if (face && family == ControllerFamily::Generic) {  // four dots, the one of its place filled
            const float d = r * 0.42f, dr = r * 0.16f;
            const ImVec2 at[4] = {ImVec2(cx, cy + d), ImVec2(cx + d, cy), ImVec2(cx - d, cy), ImVec2(cx, cy - d)};
            const int mine = pd.button == Button::South ? 0 : pd.button == Button::East ? 1 : pd.button == Button::West ? 2 : 3;
            for (int k = 0; k < 4; ++k)
                if (k == mine)
                    dl->AddCircleFilled(at[k], dr * 1.25f, symbol);
                else
                    dl->AddCircle(at[k], dr, alpha(symbol, 0.6f), 0, th * 0.6f);
        } else
        switch (pd.button) {
        case Button::South:
            dl->AddLine(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), symbol, th);
            dl->AddLine(ImVec2(cx - s, cy + s), ImVec2(cx + s, cy - s), symbol, th);
            break;
        case Button::East:
            dl->AddCircle(c, s * 1.05f, symbol, 0, th);
            break;
        case Button::West:
            dl->AddRect(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), symbol, 0.0f, th);
            break;
        case Button::North:
            dl->AddTriangle(ImVec2(cx, cy - s * 1.15f), ImVec2(cx + s * 1.1f, cy + s * 0.75f), ImVec2(cx - s * 1.1f, cy + s * 0.75f),
                            symbol, th);
            break;
        default: {
            // Short codes for the long names; the paddles P1-P4.
            const bool ps = family == ControllerFamily::PlayStation, xbox = family == ControllerFamily::Xbox, nin = family == ControllerFamily::Nintendo;
            const char* code = pd.button == Button::Back       ? (ps ? "CRE" : xbox ? "VIEW" : nin ? "\xE2\x80\x93" : "SEL")
                               : pd.button == Button::Start    ? (ps ? "OPT" : xbox ? "MENU" : nin ? "+" : "STA")
                               : pd.button == Button::Guide    ? (ps ? "PS" : xbox ? "XBOX" : "HOME")
                               : pd.button == Button::Touchpad ? "TP"
                               : pd.button == Button::Misc1    ? (ps ? "MUTE" : xbox ? "SHR" : nin ? "CAP" : "MISC")
                               : pd.button == Button::RightPaddle1 ? "P1"
                               : pd.button == Button::LeftPaddle1  ? "P3"
                               : pd.button == Button::RightPaddle2 ? "P2"
                               : pd.button == Button::LeftPaddle2  ? "P4"
                                                                : nullptr;
            const std::string name = code ? code : label(pd.button);
            text(dl, gBold, std::min(px(name.size() > 3 ? 11 : 13), r * 0.62f), cx, cy, symbol, name, H::Center, V::Middle);
        }
        }
        // Flash on each press.
        const double since = time_ - lastPress_[static_cast<std::size_t>(pd.button)];
        if (pd.button != Button::LeftTrigger && pd.button != Button::RightTrigger && since < 0.4) {
            const auto k = static_cast<float>(since / 0.4);
            dl->AddCircle(c, r * (1.0f + 0.7f * k), alpha(color, 1.0f - k), 0, px(3) * (1.0f - k) + 1.0f);
        }

        // Name, then what it plays, then the toggle mark.
        const auto& binding = m.buttons[static_cast<std::size_t>(pd.button)];
        if (binding.type == ActionType::LoopRecord && live.loops[static_cast<std::size_t>(LoopSource::Notes)].status.overdub)
            dl->AddCircle(c, r + px(4), T::kOrange, 0, px(2.5f));  // overdubbing
        float ty = cy + r + px(11);
        // "Touchpad click" is too long under its pad: the strip says "Touchpad".
        const std::string name = pd.button == Button::Touchpad ? std::string("Touchpad") : label(pd.button);
        text(dl, gMedium, px(12), cx, ty, T::kLabel, fit(gMedium, px(12), name, slot - 2), H::Center, V::Middle);
        ty += px(14);
        const std::string action = fit(gCondensed, px(14), compact(binding, flats_), slot - px(16));
        badge(dl, gCondensed, px(14), cx, ty, px(22), action, binding.isSet() ? T::kText : T::kDim,
              sounding ? alpha(color, 0.45f) : IM_COL32(0x40, 0x45, 0x4D, 255), 0, H::Center, 6);
        if (binding.isSet() && binding.mode == Mode::Toggle)  // top right of the pad
            toggleMark(dl, ImVec2(cx + r + px(7), cy - r + px(4)), px(4.5f), sounding ? color : T::kLabel);

        if (hoverButton(("##pad" + std::to_string(i)).c_str(), ImVec2(cx - slot * 0.5f + 2, top + px(4)),
                        ImVec2(cx + slot * 0.5f - 2, y + h - px(4))))
            click("button:" + std::string(toString(pd.button)));
    }
}
