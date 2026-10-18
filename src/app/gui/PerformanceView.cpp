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
    case AxisMode::Velocity:
        if (std::abs(live.velocityShaping) < 0.005f)
            return "VEL";
        std::snprintf(buf, sizeof buf, "VEL %+d%%", static_cast<int>(std::lround(live.velocityShaping * 100.0f)));
        return buf;
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

// A CRT-like screen: dark glass, content, scanlines and a vignette.
template <typename Content>
void screen(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 tint, Content&& content) {
    dl->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), IM_COL32(0x12, 0x13, 0x15, 255), px(7));
    dl->AddRectFilledMultiColor(a, b, shade(tint, 0.10f), shade(tint, 0.10f), shade(tint, 0.24f), shade(tint, 0.24f));
    dl->PushClipRect(a, b, true);
    content();
    for (float y = a.y; y < b.y; y += 3.0f)  // one pixel each: a line would spread over three
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y + 1.0f), IM_COL32(0, 0, 0, 55));
    const float v = std::min(b.x - a.x, b.y - a.y) * 0.18f;
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + v), IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 110), 0, 0);
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - v), b, 0, 0, IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 110));
    dl->PopClipRect();
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 220), px(4), 1.5f);
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
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, r);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + header), T::kPanelHeader, r, ImDrawFlags_RoundCornersTop);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, r, px(1));
    text(dl, gBold, px(14), x + px(12), y + header * 0.5f, T::kText, title, H::Left, V::Middle);
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

bool hoverButton(const char* id, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton(id, ImVec2(std::max(b.x - a.x, 1.0f), std::max(b.y - a.y, 1.0f)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(255, 255, 255, 70), px(6), px(1));
    }
    return ImGui::IsItemClicked();
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
// When a LOOP box has nothing to show, a small 80s-90s style animation in
// grey pixels, at half opacity: curves imitating the controls, arcade sprites,
// or a mini-game. kIdleArtCount of them; PerformanceView picks one per box.

constexpr int kIdleArtCount = 12;
constexpr ImU32 kArt = IM_COL32(215, 215, 215, 128);
constexpr ImU32 kArtDim = IM_COL32(215, 215, 215, 64);

// A pixel sprite: 'X' is lit.
void sprite(ImDrawList* dl, std::initializer_list<const char*> rows, float x, float y, float p, ImU32 col) {
    float yy = y;
    for (const char* row : rows) {
        float xx = x;
        for (const char* c = row; *c; ++c, xx += p)
            if (*c == 'X')
                dl->AddRectFilled(ImVec2(xx, yy), ImVec2(xx + p, yy + p), col);
        yy += p;
    }
}

void dot(ImDrawList* dl, float x, float y, float p, ImU32 col) {
    dl->AddRectFilled(ImVec2(std::floor(x / p) * p, std::floor(y / p) * p), ImVec2(std::floor(x / p) * p + p, std::floor(y / p) * p + p), col);
}

// A curve snapped to the pixel grid, `f` giving its height in [0, 1].
template <typename Fn>
void pixelCurve(ImDrawList* dl, ImVec2 a, ImVec2 b, float p, ImU32 col, Fn f) {
    float prev = -1;
    for (float x = a.x; x < b.x - p; x += p) {
        const float y = std::floor((a.y + (b.y - a.y) * std::clamp(f((x - a.x) / (b.x - a.x)), 0.0f, 1.0f)) / p) * p;
        if (prev >= 0)
            for (float yy = std::min(prev, y); yy <= std::max(prev, y); yy += p)
                dl->AddRectFilled(ImVec2(x, yy), ImVec2(x + p, yy + p), col);
        prev = y;
    }
}

float wave01(float v) { return std::abs(std::fmod(v, 2.0f) - 1.0f); }  // a triangle wave in [0, 1]

void idleArt(ImDrawList* dl, int art, ImVec2 a, ImVec2 b, double time) {
    const float p = std::max(2.0f, std::floor(px(3)));
    const auto t = static_cast<float>(time);
    const float w = b.x - a.x, h = b.y - a.y;
    a = ImVec2(a.x + p * 2, a.y + p * 2);
    b = ImVec2(b.x - p * 2, b.y - p * 2);
    dl->PushClipRect(a, b, true);
    switch (art) {
    case 0:  // stick curves, X and Y, scrolling
        pixelCurve(dl, a, b, p, kArt, [&](float u) { return 0.5f + 0.35f * std::sin(6.283f * (u * 1.4f + t * 0.25f)); });
        pixelCurve(dl, a, b, p, kArtDim, [&](float u) { return 0.5f + 0.30f * std::cos(6.283f * (u * 0.9f + t * 0.18f + 0.3f)); });
        break;
    case 1: {  // a stick's trail, fading
        for (int k = 60; k >= 0; --k) {
            const float tau = t * 1.6f - static_cast<float>(k) * 0.05f;
            const float u = 0.5f + 0.4f * std::sin(1.3f * tau) * std::cos(0.37f * tau), v = 0.5f + 0.4f * std::cos(1.1f * tau);
            const auto alphaK = static_cast<int>(128.0f * (1.0f - static_cast<float>(k) / 61.0f));
            dot(dl, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * v, k == 0 ? p * 2 : p, IM_COL32(215, 215, 215, alphaK));
        }
        break;
    }
    case 2:  // trigger presses, scrolling
        pixelCurve(dl, a, b, p, kArt, [&](float u) { return 1.0f - std::pow(std::max(0.0f, std::sin(6.283f * (u * 2.0f + t * 0.3f))), 1.5f) * 0.85f; });
        pixelCurve(dl, a, b, p, kArtDim, [&](float u) { return 1.0f - std::pow(std::max(0.0f, std::sin(6.283f * (u + t * 0.22f + 0.55f))), 3.0f) * 0.7f; });
        break;
    case 3: {  // a finger's spiral trail
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        for (int k = 0; k < 60; ++k) {
            const float ang = static_cast<float>(k) * 0.28f + t * 1.5f, r = p * 2 + static_cast<float>(k) * h / 140.0f;
            dot(dl, c.x + std::cos(ang) * r, c.y + std::sin(ang) * r * 0.75f, p, IM_COL32(215, 215, 215, 30 + k * 2));
        }
        break;
    }
    case 4: {  // shoot'em up: stars, a ship, its shots
        for (int k = 0; k < 40; ++k) {
            const float speed = 20.0f + static_cast<float>(k % 3) * 25.0f;
            const float sx = b.x - std::fmod(static_cast<float>(k * 53) + t * speed, w);
            dot(dl, sx, a.y + std::fmod(static_cast<float>(k * 37), h - p * 4), p * 0.7f, k % 3 == 2 ? kArt : kArtDim);
        }
        const float shipY = a.y + h * 0.42f + std::sin(t * 1.3f) * h * 0.2f;
        sprite(dl, {"XX..............", "XXXX............", ".XXXXXXXXXX.....", "XXXXXXXXXXXXXX..", ".XXXXXXXXXX.....",
                    "XXXX............", "XX.............."},
               a.x + p * 4, shipY, p, kArt);
        for (int k = 0; k < 3; ++k)
            dl->AddRectFilled(ImVec2(a.x + p * 18 + std::fmod(t * 120.0f + static_cast<float>(k) * w / 3, w), shipY + p * 3),
                              ImVec2(a.x + p * 20 + std::fmod(t * 120.0f + static_cast<float>(k) * w / 3, w), shipY + p * 4), kArt);
        break;
    }
    case 5: {  // invaders marching, the cannon below
        const bool frame = std::fmod(t, 1.0f) < 0.5f;
        const float q = std::max(1.0f, std::floor(p * 0.7f));
        const float formation = q * (14 * 4 + 11), room = std::max(0.0f, (b.x - a.x) - formation);
        const float left = a.x + room * wave01(t * 0.15f);  // marching from side to side, inside the box
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 5; ++col) {
                const float ix = left + static_cast<float>(col) * q * 14, iy = a.y + q * 4 + static_cast<float>(row) * q * 12;
                if (frame)
                    sprite(dl, {"..X.....X..", "...X...X...", "..XXXXXXX..", ".XX.XXX.XX.", "XXXXXXXXXXX", "X.XXXXXXX.X", "X.X.....X.X",
                                "...XX.XX..."},
                           ix, iy, q, kArt);
                else
                    sprite(dl, {"..X.....X..", "X..X...X..X", "X.XXXXXXX.X", "XXX.XXX.XXX", "XXXXXXXXXXX", ".XXXXXXXXX.", "..X.....X..",
                                ".X.......X."},
                           ix, iy, q, kArt);
            }
        sprite(dl, {"....X....", "...XXX...", "XXXXXXXXX", "XXXXXXXXX"}, a.x + (b.x - a.x - q * 9) * wave01(t * 0.21f), b.y - q * 6, q, kArt);
        break;
    }
    case 6: {  // pong
        for (float y = a.y; y < b.y; y += p * 3)
            dl->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - p * 0.5f, y), ImVec2((a.x + b.x) * 0.5f + p * 0.5f, y + p * 1.5f), kArtDim);
        const float bx = a.x + p * 4 + (w - p * 14) * wave01(t * 0.45f), by = a.y + (h - p * 6) * wave01(t * 0.62f + 0.3f);
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + p * 2, by + p * 2), kArt);
        const float padH = p * 10;
        const float ly = std::clamp(by - padH * 0.5f, a.y, b.y - padH), ry = std::clamp(a.y + (h - padH) * wave01(t * 0.5f + 0.7f), a.y, b.y - padH);
        dl->AddRectFilled(ImVec2(a.x + p, ly), ImVec2(a.x + p * 2, ly + padH), kArt);
        dl->AddRectFilled(ImVec2(b.x - p * 2, ry), ImVec2(b.x - p, ry + padH), kArt);
        break;
    }
    case 7: {  // a cat chasing a mouse
        const float x = a.x - p * 30 + std::fmod(t * 35.0f, w + p * 50);
        const float y = b.y - p * 12;
        const bool step = std::fmod(t, 0.5f) < 0.25f;
        sprite(dl, {"...........X.X", "X..........XXX", "X.........XXXX", ".XXXXXXXXXXXX.", "..XXXXXXXXXX..", "..XXXXXXXXXX..",
                    step ? "..X.X....X.X.." : "...XX.....XX..", step ? "..X..X..X..X.." : "...X.X...X.X.."},
               x, y - p * 8, p, kArt);
        sprite(dl, {"..XX.", ".XXXX", "XXXXX", "X...."}, x + p * 26, y - p * 4, p, kArt);
        break;
    }
    case 8: {  // snake, along a rounded loop
        const int cols = static_cast<int>(w / (p * 2)) - 2, rows = static_cast<int>(h / (p * 2)) - 2;
        const int perimeter = 2 * (cols + rows);
        auto cell = [&](int k) {
            k = ((k % perimeter) + perimeter) % perimeter;
            if (k < cols)
                return ImVec2(static_cast<float>(k), 0);
            k -= cols;
            if (k < rows)
                return ImVec2(static_cast<float>(cols), static_cast<float>(k));
            k -= rows;
            if (k < cols)
                return ImVec2(static_cast<float>(cols - k), static_cast<float>(rows));
            k -= cols;
            return ImVec2(0, static_cast<float>(rows - k));
        };
        const int head = static_cast<int>(t * 8.0f);
        for (int k = 0; k < 14; ++k) {
            const ImVec2 c = cell(head - k);
            const float cx = a.x + p * 2 + c.x * p * 2, cy = a.y + p * 2 + c.y * p * 2;
            dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + p * 2 - 1, cy + p * 2 - 1), k == 0 ? kArt : kArtDim);
        }
        if (std::fmod(t, 0.8f) < 0.6f)
            dl->AddRectFilled(ImVec2(a.x + w * 0.45f, a.y + h * 0.45f), ImVec2(a.x + w * 0.45f + p * 2, a.y + h * 0.45f + p * 2), kArt);
        break;
    }
    case 9: {  // asteroids: drifting rocks, a ship firing
        for (int k = 0; k < 3; ++k) {
            const float q = k == 0 ? p : std::max(1.0f, p * 0.7f);
            const float rx = a.x + std::fmod(static_cast<float>(k) * w * 0.37f + t * (12.0f + static_cast<float>(k) * 7.0f), w);
            const float ry = a.y + std::fmod(static_cast<float>(k) * h * 0.53f + t * (8.0f + static_cast<float>(k) * 5.0f), h);
            sprite(dl, {"..XXXX..", ".X....X.", "X......X", "X.....X.", ".X....X.", "..XXXX.."}, rx, ry, q, kArt);
        }
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        sprite(dl, {"...X...", "...X...", "..X.X..", "..X.X..", ".X...X.", ".XXXXX.", "X.....X"}, c.x - p * 3.5f, c.y - p * 3.5f, p, kArt);
        for (int k = 0; k < 3; ++k)
            dl->AddRectFilled(ImVec2(c.x - p * 0.5f, c.y - p * 6 - std::fmod(t * 90.0f + static_cast<float>(k) * h / 3, h * 0.5f)),
                              ImVec2(c.x + p * 0.5f, c.y - p * 5 - std::fmod(t * 90.0f + static_cast<float>(k) * h / 3, h * 0.5f)), kArt);
        break;
    }
    case 10: {  // breakout
        const float bw = (w - p * 4) / 8.0f;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 8; ++col)
                if ((row * 3 + col + static_cast<int>(t * 0.5f)) % 7 != 2)
                    dl->AddRectFilled(ImVec2(a.x + static_cast<float>(col) * bw + 1, a.y + static_cast<float>(row) * p * 3),
                                      ImVec2(a.x + static_cast<float>(col + 1) * bw - 1, a.y + static_cast<float>(row) * p * 3 + p * 2), kArt);
        const float bx = a.x + (w - p * 6) * wave01(t * 0.5f), by = a.y + p * 10 + (h - p * 16) * wave01(t * 0.7f);
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + p * 2, by + p * 2), kArt);
        const float padX = std::clamp(bx - p * 6, a.x, b.x - p * 12);
        dl->AddRectFilled(ImVec2(padX, b.y - p * 2), ImVec2(padX + p * 12, b.y - p), kArt);
        break;
    }
    default: {  // a pac-man eating dots, a ghost behind
        const float x = a.x - p * 20 + std::fmod(t * 30.0f, w + p * 40), y = (a.y + b.y) * 0.5f - p * 3.5f;
        const bool open = std::fmod(t, 0.3f) < 0.15f;
        sprite(dl, {"..XXXX..", ".XXXXXX.", open ? "XXXXX..." : "XXXXXXXX", open ? "XXXX...." : "XXXXXXXX", open ? "XXXXX..." : "XXXXXXXX",
                    ".XXXXXX.", "..XXXX.."},
               x, y, p, kArt);
        for (float dx = std::ceil((x + p * 10 - a.x) / (p * 4)) * p * 4 + a.x; dx < b.x; dx += p * 4)
            dl->AddRectFilled(ImVec2(dx, y + p * 3), ImVec2(dx + p, y + p * 4), kArt);
        sprite(dl, {"..XXXX..", ".XXXXXX.", "XX.XX.XX", "XXXXXXXX", "XXXXXXXX", std::fmod(t, 0.4f) < 0.2f ? "X.X..X.X" : ".X.XX.X."},
               x - p * 16, y + p, p, kArt);
        break;
    }
    }
    dl->PopClipRect();
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
void shiftModeBadge(ImDrawList* dl, ImVec2 a, ImVec2 b, bool amplitudes, ImU32 color) {
    const char* label = amplitudes ? "AMP" : "LEN";
    const float w = textWidth(gBold, px(11), label) + px(14);
    badge(dl, gBold, px(11), b.x - px(8) - w * 0.5f, a.y + px(8), px(20), label, T::kText, alpha(color, 0.3f), color);
}

}  // namespace

std::string loopBeatsText(double beats, int beatsPerBar) { return beatsText(beats, beatsPerBar); }
std::string loopDurationText(double seconds) { return durationText(seconds); }

PerformanceView::PerformanceView() {
    std::mt19937 random(std::random_device{}());
    std::array<std::uint8_t, kIdleArtCount> arts{};
    for (std::size_t i = 0; i < arts.size(); ++i)
        arts[i] = static_cast<std::uint8_t>(i);
    for (auto& channel : idleArt_) {
        std::shuffle(arts.begin(), arts.end(), random);
        std::copy_n(arts.begin(), channel.size(), channel.begin());
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

void PerformanceView::lockOnRightClick(int source, bool recorded, bool locked, int axis, const ImVec2* a, const ImVec2* b) {
    if (!ImGui::IsItemHovered() || (!recorded && !locked) || (a && !ImGui::IsMouseHoveringRect(*a, *b)))
        return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        lockRequest_ = LockRequest{source, axis};
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(locked ? "Locked: Record keeps it"
                               : a && axis >= 0 ? "Right-click: lock this axis' loop"
                               : a              ? "Right-click: lock these loops"
                                                : "Right-click: lock this loop");
        ImGui::TextDisabled(locked ? "Right-click: unlock" : "Record will not erase it");
        ImGui::EndTooltip();
    }
}

void PerformanceView::stretchWheel(std::uint16_t tracks, ImVec2 a, ImVec2 b) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool amplitude = io.KeyCtrl && !io.KeyShift;  // Ctrl + wheel: the amplitude
    if ((!io.KeyShift && !amplitude) || !ImGui::IsWindowHovered() || !ImGui::IsMouseHoveringRect(a, b))
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
    const int accel = std::min(1 + wheelStreak_ / 2, amplitude ? 4 : 10);
    stretch_ = StretchRequest{tracks, notches * accel, amplitude};
    auto& shown = amplitude ? amplitudeShown_ : stretchShown_;
    for (std::size_t i = 0; i < shown.size(); ++i)
        if (tracks & (1u << i))
            shown[i] = time_;
}

float PerformanceView::drawStretchChip(ImDrawList* dl, float right, float y, int stretch, ImU32 color) {
    return stretch == 0 ? right : drawChip(dl, right, y, stretchText(stretch), color);
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
    drawStick(0, mapping, live, x, origin.y, colW, moduleH);
    x += colW + gap;
    drawStick(1, mapping, live, x, origin.y, colW, moduleH);
    x += colW + gap;
    drawTriggers(mapping, live, x, origin.y, colW, moduleH);
    drawPianoRoll(mapping, live, origin.x, lowY, colW * 3 + gap * 2, lowH);
    x += colW + gap;
    drawDpad(mapping, live, x, origin.y, colW, moduleH);
    drawScale(key, x, lowY, colW, lowH);
    x += colW + gap;
    drawTouchpad(mapping, live, x, origin.y, colW, moduleH);
    drawChannel(live, x, lowY, colW, lowH);
    drawButtons(mapping, live, origin.x, origin.y + topH + gap, avail.x, stripH);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y));
    ImGui::Dummy(ImVec2(0, 0));
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
    const ImU32 kViolet = IM_COL32(0xB0, 0x8C, 0xFF, 255);
    const ImU32 kMauve = IM_COL32(0xC4, 0x7C, 0xFF, 255);  // selected notes, in the editor
    const ImU32 kLive = IM_COL32(0xEB, 0xEE, 0xF2, 255);
    // No title bar: the roll fills its block, its state shows top right.
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, px(8));
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, px(8), px(1));

    char seconds[16];
    std::snprintf(seconds, sizeof seconds, "%.1f s", static_cast<double>(st.length) / Engine::kLoopRateHz);
    std::string status;
    ImU32 statusColor = T::kDim;
    if (recording) {
        status = std::string("REC  ") + seconds;
        statusColor = T::kRed;
    } else if (st.waiting) {
        status = "REC  \xC2\xB7  play notes";
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
                              ImVec2(std::max(xAtTime(end), xAtTime(n.start) + px(2)), yb - 0.5f), alpha(T::kCyan, a), px(1));
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
            dl->AddRectFilled(na, nb, dragged ? alpha(IM_COL32_WHITE, 0.12f) : fill, px(1));
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
                              ImVec2(std::max(xAtTime(end), xAtTime(n.start) + px(2)), yb - 0.5f), alpha(n.fromLoop ? kViolet : kLive, a), px(1));
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
    }
    if (st.locked && clip)
        drawLockedBorder(dl, ImVec2(ax0, ay0), ImVec2(ax1, ay1), kViolet, px(3));
    const bool pressed = hoverButton("##pianoroll", ImVec2(ax0, ay0), ImVec2(ax1, ay1));
    const bool hovering = ImGui::IsItemHovered();
    if (!editing_)  // in the editor, a right-click deletes notes
        lockOnRightClick(static_cast<int>(kNotes), clip, st.locked);
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
    // The wheel zooms around the mouse, from the whole loop to a second (or a
    // beat) across; a sideways scroll moves along. The window does not scroll.
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (hovering && !lengthDrag_ && !drag_ && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        const float most = std::max(1.0f, st.beats > 0 ? static_cast<float>(st.beats) : static_cast<float>(st.length) / Engine::kLoopRateHz);
        if (io.MouseWheel != 0.0f) {
            const float f = std::clamp((mouse.x - gx0) / std::max(ax1 - gx0, 1.0f), 0.0f, 1.0f);
            const float under = viewStart_ + f / viewZoom_;  // the place under the mouse stays there
            viewZoom_ = std::clamp(viewZoom_ * std::pow(1.25f, io.MouseWheel), 1.0f, most);
            viewStart_ = under - f / viewZoom_;
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
std::optional<PerformanceView::SessionAction> PerformanceView::drawSession(const LiveState& live, int cursorSlot) {
    gMedium = fonts_.medium ? fonts_.medium : ImGui::GetFont();
    gBold = fonts_.bold ? fonts_.bold : gMedium;
    gCondensed = fonts_.condensed ? fonts_.condensed : gBold;
    time_ = ImGui::GetTime();
    std::optional<SessionAction> action;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    constexpr int kColumns = 8, kSlots = Engine::kClipSlots;
    static constexpr ImU32 kChannelColors[] = {T::kBlue, T::kPink, T::kGreen, T::kOrange, IM_COL32(0xB0, 0x8C, 0xFF, 255),
                                               T::kCyan, IM_COL32(0xFF, 0xD6, 0x40, 255), IM_COL32(0xFF, 0x78, 0x5A, 255)};
    // The modules' colours, for what a clip holds: notes, left stick, right stick, triggers, touchpad.
    static constexpr ImU32 kModuleColors[] = {IM_COL32(0xEC, 0xEE, 0xF2, 255), T::kBlue, T::kOrange, T::kPink, T::kCyan};
    const float gap = px(6), colW = (width - gap * (kColumns - 1)) / kColumns;
    const float headH = px(34), cellH = px(70), stopH = px(28);
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
        dl->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + colW, origin.y + gridH), T::kPanel, px(8));
        dl->AddRect(ImVec2(x, origin.y), ImVec2(x + colW, origin.y + gridH), T::kBorder, px(8), px(1));
        // Header: the channel, a dot while it plays; the active one underlined. A click chooses it.
        const bool active = ch == live.channel;
        text(dl, gBold, px(11), x + px(12), origin.y + px(17), active ? T::kText : T::kDim, "CHANNEL " + std::to_string(ch + 1), H::Left, V::Middle);
        dl->AddCircleFilled(ImVec2(x + colW - px(15), origin.y + px(17)), px(5), playing ? T::kGreen : IM_COL32(0x3C, 0x40, 0x46, 255));
        if (active)
            dl->AddLine(ImVec2(x + px(12), origin.y + px(28)), ImVec2(x + px(12) + textWidth(gBold, px(11), "CHANNEL 16"), origin.y + px(28)), chColor, px(2));
        ImGui::PushID(ch);
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        if (ImGui::InvisibleButton("##head", ImVec2(colW, headH)))
            action = SessionAction{SessionAction::Kind::Channel, ch, 0};
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
            ImGui::PopID();
            if (clicked)
                action = SessionAction{SessionAction::Kind::Launch, ch, slot};
            if (!clip.sources) {
                dl->AddRectFilled(a, b, IM_COL32(0x24, 0x26, 0x2A, 255), px(5));
                dl->AddRect(a, b, hovered ? T::kControlBorder : IM_COL32(0x34, 0x38, 0x3E, 255), px(5), px(1));
            } else {
                const ImU32 outline = clip.recording ? T::kRed : clipPlays ? T::kGreen : queued ? alpha(T::kGreen, blink ? 0.9f : 0.3f)
                                    : hovered      ? T::kControlBorder
                                                   : IM_COL32(0x46, 0x4B, 0x53, 255);
                dl->AddRectFilled(a, b, IM_COL32(0x28, 0x2B, 0x30, 255), px(5));
                dl->AddRect(a, b, outline, px(5), px(clip.recording || clipPlays || queued ? 2.0f : 1.0f));
                dl->AddRectFilled(ImVec2(a.x + px(2), a.y + px(4)), ImVec2(a.x + px(5), b.y - px(4)), chColor, px(1));
                const bool hasNotes = clip.sources & (1u << static_cast<int>(LoopSource::Notes));
                const bool hasCurves = clip.sources & ~(1u << static_cast<int>(LoopSource::Notes));
                const float px0 = a.x + px(10), px1 = b.x - px(6);
                const float ny0 = a.y + px(21), ny1 = hasCurves ? a.y + px(38) : b.y - px(8);
                const float cy0 = hasNotes ? a.y + px(38) : a.y + px(22), cy1 = b.y - px(6);
                if (hasNotes && clip.noteCount > 0) {
                    int lo = 127, hi = 0;
                    for (int i = 0; i < clip.noteCount; ++i) {
                        lo = std::min<int>(lo, clip.notes[static_cast<std::size_t>(i)][2]);
                        hi = std::max<int>(hi, clip.notes[static_cast<std::size_t>(i)][2]);
                    }
                    for (int i = 0; i < clip.noteCount; ++i) {
                        const auto& n = clip.notes[static_cast<std::size_t>(i)];
                        const float nx0 = px0 + (px1 - px0) * static_cast<float>(n[0]) / 255.0f;
                        const float nx1 = std::max(px0 + (px1 - px0) * static_cast<float>(n[1]) / 255.0f, nx0 + px(2));
                        const float t = hi > lo ? static_cast<float>(n[2] - lo) / static_cast<float>(hi - lo) : 0.5f;
                        const float ny = ny1 - px(2) - t * (ny1 - ny0 - px(2));
                        dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(nx1, ny + px(2)), alpha(kModuleColors[0], 0.85f));
                    }
                }
                static constexpr int kCurveBits[4] = {1 << 0, 1 << 1, 1 << 4, (1 << 2) | (1 << 3)};
                for (int m = 0; m < 4; ++m) {
                    if (!(clip.sources & kCurveBits[m]))
                        continue;
                    ImVec2 pts[LiveState::Clip::kCurvePoints];
                    for (int i = 0; i < LiveState::Clip::kCurvePoints; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(LiveState::Clip::kCurvePoints - 1);
                        const float v = static_cast<float>(clip.curves[static_cast<std::size_t>(m)][static_cast<std::size_t>(i)]) / 255.0f;
                        pts[i] = ImVec2(px0 + t * (px1 - px0), cy1 - v * (cy1 - cy0));
                    }
                    dl->AddPolyline(pts, LiveState::Clip::kCurvePoints, kModuleColors[1 + m], px(1.4f));
                }
                // Its name, and what it holds.
                text(dl, gBold, px(10), a.x + px(10), a.y + px(11), T::kText, "Clip " + std::to_string(slot + 1), H::Left, V::Middle);
                float bx = a.x + px(50);
                static constexpr const char* kLetters[] = {"", "L", "R", "T", "P"};
                for (int m = 0; m < 5; ++m) {
                    const bool has = m == 0 ? hasNotes : (clip.sources & (m == 4 ? kCurveBits[3] : kCurveBits[m - 1])) != 0;
                    if (!has || bx > b.x - px(30))
                        continue;
                    const ImU32 mc = kModuleColors[m];
                    const ImVec2 ba(bx, a.y + px(5)), bb(bx + px(11), a.y + px(17));
                    dl->AddRectFilled(ba, bb, alpha(mc, 0.18f), px(3));
                    dl->AddRect(ba, bb, alpha(mc, 0.8f), px(3), px(1));
                    if (m == 0) {  // a note sign
                        dl->AddCircleFilled(ImVec2(bx + px(4.5f), a.y + px(13.2f)), px(2), mc);
                        dl->AddLine(ImVec2(bx + px(6.3f), a.y + px(7)), ImVec2(bx + px(6.3f), a.y + px(13.2f)), mc, px(1));
                        dl->AddLine(ImVec2(bx + px(6.3f), a.y + px(7)), ImVec2(bx + px(8.8f), a.y + px(8.5f)), mc, px(1));
                    } else {
                        text(dl, gBold, px(7.5f), bx + px(5.5f), a.y + px(11), mc, kLetters[m], H::Center, V::Middle);
                    }
                    bx += px(13);
                }
                // Its state: playing (and how far in its turn), next, recording.
                const ImVec2 tri0(b.x - px(16), a.y + px(6)), tri1(b.x - px(16), a.y + px(16)), tri2(b.x - px(7), a.y + px(11));
                if (clip.recording) {
                    dl->AddCircleFilled(ImVec2(b.x - px(11), a.y + px(11)), px(5), T::kRed);
                } else if (clipPlays) {
                    dl->AddTriangleFilled(tri0, tri1, tri2, T::kGreen);
                    const float p = std::clamp(live.clipProgress[c], 0.0f, 1.0f);
                    dl->AddRectFilled(ImVec2(a.x + px(2), b.y - px(4)), ImVec2(a.x + px(2) + (b.x - a.x - px(4)) * p, b.y - px(2)), T::kGreen);
                } else if (queued) {
                    dl->AddTriangle(tri0, tri1, tri2, alpha(T::kGreen, blink ? 1.0f : 0.35f), px(1.2f));
                }
            }
            if (active && slot == cursorSlot) {  // the cursor
                dl->AddRect(ImVec2(a.x - px(3), a.y - px(3)), ImVec2(b.x + px(3), b.y + px(3)), T::kCyan, px(7), px(2.5f));
                if (!clip.sources) {
                    text(dl, gBold, px(16), (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - px(7), T::kCyan, "+", H::Center, V::Middle);
                    text(dl, gMedium, px(9), (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + px(12), T::kCyan, "Record: new clip", H::Center, V::Middle);
                }
            }
        }
        // The stop at the bottom: blinking while waiting for the end of the turn.
        const float sy = origin.y + headH + cellH * kSlots + px(2);
        const ImVec2 sa(x + px(6), sy), sb(x + colW - px(6), sy + stopH - px(2));
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
    // The other eight channels.
    const float by = origin.y + gridH + px(8);
    const char* other = sessionFirst_ == 0 ? "Channels 9\xE2\x80\x93" "16  \xE2\x80\xBA" : "\xE2\x80\xB9  Channels 1\xE2\x80\x93" "8";
    const float ow = textWidth(gMedium, px(11), other) + px(16);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - ow, by));
    if (ImGui::InvisibleButton("##otherchannels", ImVec2(ow, px(20))))
        sessionFirst_ = sessionFirst_ == 0 ? 8 : 0;
    text(dl, gMedium, px(11), origin.x + width - px(8), by + px(10), ImGui::IsItemHovered() ? T::kText : T::kDim, other, H::Right, V::Middle);
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
             key ? keyName(*key) : "no scale  \xC2\xB7  click to choose", H::Center, V::Middle);
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

// The active MIDI channel, large, and the sixteen channels: a violet dot on
// those where a loop plays.
void PerformanceView::drawChannel(const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    plainBlock(dl, x, y, w, h);
    const ImVec2 a(x + px(10), y + px(10)), b(x + w - px(10), y + h - px(10));
    if (b.y - a.y < px(30))
        return;
    const ImU32 kViolet = IM_COL32(0xB0, 0x8C, 0xFF, 255);
    screen(dl, a, b, T::kGreen, [&] {
        const float numX = a.x + (b.x - a.x) * 0.24f, midY = (a.y + b.y) * 0.5f;
        const std::string number = std::to_string(live.channel + 1);
        const float size = std::min(px(64), (b.y - a.y) * 0.8f);
        for (float r : {px(3), px(1.5f)})  // glow
            for (auto [dx, dy] : {std::pair{-r, 0.0f}, {r, 0.0f}, {0.0f, -r}, {0.0f, r}})
                text(dl, gBold, size, numX + dx, midY + dy, alpha(T::kGreen, 0.18f), number, H::Center, V::Middle);
        text(dl, gBold, size, numX, midY, T::kGreen, number, H::Center, V::Middle);
        // The sixteen channels, four by four.
        const float gx0 = a.x + (b.x - a.x) * 0.48f, gx1 = b.x - px(6), gy0 = a.y + px(5), gy1 = b.y - px(5);
        const float cw = (gx1 - gx0) / 4.0f, ch = (gy1 - gy0) / 4.0f;
        for (int c = 0; c < 16; ++c) {
            const ImVec2 ca(gx0 + static_cast<float>(c % 4) * cw + px(2), gy0 + static_cast<float>(c / 4) * ch + px(2));
            const ImVec2 cb(ca.x + cw - px(4), ca.y + ch - px(4));
            const bool active = c == live.channel;
            dl->AddRectFilled(ca, cb, active ? alpha(T::kGreen, 0.85f) : alpha(T::kGreen, 0.10f), px(3));
            dl->AddRect(ca, cb, alpha(T::kGreen, active ? 1.0f : 0.35f), px(3), px(1));
            text(dl, gCondensed, px(9), (ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f,
                 active ? IM_COL32(0x0A, 0x1E, 0x12, 255) : mix(T::kGreen, IM_COL32(0, 0, 0, 255), 0.2f), std::to_string(c + 1),
                 H::Center, V::Middle);
            if ((live.loopChannels >> c) & 1u)
                dl->AddCircleFilled(ImVec2(cb.x - px(1), ca.y + px(1)), px(2.5f), kViolet);
        }
    });
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
        shiftModeBadge(dl, s.a, s.b, live.stickAmplitudes[static_cast<std::size_t>(index)], accent);
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
        {&loop.lanes[0], Engine::trackOf(index, 0), accent, "X", bx.enabled, bx.invert, false, lb.ccX, lb.steps[0]},
        {&loop.lanes[1], Engine::trackOf(index, 1), mix(accent, T::kText, 0.5f), "Y", by.enabled, by.invert, false, lb.ccY, lb.steps[1]}};
    drawLoopPanel(dl, x, w, loopY, y + h - px(10), loop.status, lb.playback, lanes, 2, accent, artFor(live.channel, index),
                  "REC  \xC2\xB7  move a stick", index == 0 ? "##loopL" : "##loopR", index == 0 ? "axes:left_x,left_y" : "axes:right_x,right_y",
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
        status = playback == LoopPlayback::Forward ? "FWD" : playback == LoopPlayback::Reverse ? "REV" : "B&F";
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
        dl->AddRectFilled(ga, gb, kGraph, px(4));
        dl->AddRect(ga, gb, kGraph, px(4), px(1.3f));
        idleArt(dl, art, ga, gb, time_);
    }
    std::array<std::pair<ImVec2, ImVec2>, 4> laneBoxes{};
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
    if (hoverButton(id, ImVec2(ga.x, loopY - px(6)), gb))
        click(spec);
    // A right-click locks a lane's axis alone, on the header all of them.
    const ImVec2 ha(ga.x, loopY - px(6)), hb(gb.x, ga.y);
    lockOnRightClick(lockSource, playing, st.locked, -1, &ha, &hb);
    if (recording || playing || st.waiting)
        for (int k = 0; k < laneCount; ++k) {
            const LoopStatus& ls = lanes[k].lane->status;
            const auto& [la, lb] = laneBoxes[static_cast<std::size_t>(k)];
            lockOnRightClick(Engine::sourceOfTrack(lanes[k].track), ls.state == LoopStatus::State::Playing, ls.locked,
                             Engine::axisOfTrack(lanes[k].track), &la, &lb);
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
        const char* why = !look.enabled ? "not enabled  \xC2\xB7  not recorded" : st.waiting ? "move it to record" : "no loop";
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
    const float amp = playing ? static_cast<float>(st.amplitude) / 100.0f : 1.0f;
    const bool steps = playing && lane.stepSamples > 0;
    const float levels = static_cast<float>(std::clamp(look.steps.levels, 2, 128) - 1);
    auto value = [&](int i, float scale, bool stepped) {
        float v = Engine::shapeAmplitude(static_cast<float>(lane.v[static_cast<std::size_t>(i)]) / 255.0f, centre, scale);
        if (stepped)
            v = std::round(v * levels) / levels;
        // What the loop sends: a finger's Y the other way up, an inverted axis upside down.
        if (look.flip)
            v = 1.0f - v;
        return look.invert ? 1.0f - v : v;
    };
    auto yOf = [&](float v) { return ly1 - v * (ly1 - ly0); };
    auto xOfPoint = [&](int i) { return lx0 + (lx1 - lx0) * static_cast<float>(i) / static_cast<float>(lane.points - 1); };
    auto polyline = [&](float scale, ImU32 col, float thickness) {  // broken where a finger was lifted
        for (int i = 1; i < lane.points; ++i)
            if (lane.on[static_cast<std::size_t>(i)] && lane.on[static_cast<std::size_t>(i - 1)])
                dl->AddLine(ImVec2(xOfPoint(i - 1), yOf(value(i - 1, scale, false))), ImVec2(xOfPoint(i), yOf(value(i, scale, false))), col,
                            thickness);
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
        polyline(1.0f, alpha(look.color, 0.22f), px(1.3f));
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
        if (amp != 1.0f)
            polyline(1.0f, alpha(look.color, 0.22f), px(1.3f));
        polyline(amp, look.color, px(1.8f));
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
    if (st.locked)
        drawLock(dl, a.x + px(11) + textWidth(gBold, small, look.name), nameY - px(0.5f), small, look.color);
    if (b.y - a.y >= px(30)) {
        const std::string cc = "CC " + std::to_string(look.cc);
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
        if (b.y - a.y >= px(34)) {  // room for the stretch and the amplitude too
            right = drawStretchChip(dl, right, chipY, st.stretch, look.color) - (st.stretch != 0 ? px(4) : 0.0f);
            drawAmplitudeChip(dl, right, chipY, st.amplitude, look.color);
        }
        drawStretchGauge(dl, a, b, bit, st.stretch, look.color);
        drawAmplitudeGauge(dl, a, b, bit, st.amplitude, look.color);
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
        envelope(s.a.x + W * 0.04f, s.a.x + W * 0.48f, vl, soundL, "L2");
        envelope(s.a.x + W * 0.52f, s.a.x + W * 0.96f, vr, soundR, "R2");
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
        text(dl, gBold, px(12), x + px(12), midY, T::kLabel, a == Axis::LeftTrigger ? "L2" : "R2", H::Left, V::Middle);
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
    const LaneLook lanes[2] = {{&loop.lanes[0], Engine::trackOf(kTriggers, 0), T::kPink, "L2", bl.enabled, bl.invert, false, lb.ccX, lb.steps[0]},
                               {&loop.lanes[1], Engine::trackOf(kTriggers, 1), pinkLight, "R2", br.enabled, br.invert, false, lb.ccY, lb.steps[1]}};
    drawLoopPanel(dl, x, w, loopY, y + h - px(10), loop.status, lb.playback, lanes, 2, T::kPink, artFor(live.channel, 2),
                  "REC  \xC2\xB7  press a trigger", "##looptriggers", "axes:left_trigger,right_trigger", kTriggers);
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
    const float top = panel(dl, x, y, w, h, "TOUCHPAD", live.connected && live.hasTouchpad && any);
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
        shiftModeBadge(dl, s.a, s.b, live.fingerAmplitudes, fingerColors[0]);
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
                              axisMode && fb.x.enabled, fb.x.invert, false, lb.ccX, lb.steps[0]};
        lanes[laneCount++] = {&loops[f]->lanes[1], Engine::trackOf(source, 1), mix(fingerColors[f], T::kText, 0.45f), f == 0 ? "1 Y" : "2 Y",
                              axisMode && fb.y.enabled, fb.y.invert, true, lb.ccY, lb.steps[1]};
        if (ls.state == LoopStatus::State::Recording || (ls.state == LoopStatus::State::Playing && st.state != LoopStatus::State::Recording))
            st.state = ls.state;
        st.waiting = st.waiting || ls.waiting;
        st.locked = st.locked || ls.locked;
        st.length = std::max(st.length, ls.length);
    }
    if (laneCount == 0) {
        text(dl, gMedium, px(11), x + w * 0.5f, (boxTop + boxBottom) * 0.5f, T::kDim, "touchpad off", H::Center, V::Middle);
        return;
    }
    const int shown = loops[0]->status.state != LoopStatus::State::Empty ? 0 : 1;
    drawLoopPanel(dl, x, w, loopY, boxBottom, st, m.loops[static_cast<std::size_t>(2 + shown)].playback, lanes, laneCount,
                  fingerColors[shown], artFor(live.channel, 3), "REC  \xC2\xB7  touch to record", "##touchloop", "fingers",
                  static_cast<int>(LoopSource::Finger1) + shown);
}

// The touchpad's LOOP panel in 4-pad mode: the pads' notes in the note loop,
// one lane per pad (TL, TR, BL, BR), with a playhead, and below when each
// finger's pads played.
void PerformanceView::drawPadLanes(ImDrawList* dl, const Mapping& m, const LiveState& live, float x, float w, float loopY,
                                   float boxTop, float boxBottom, float timelineH) {
    const ImU32 fingerColors[] = {T::kCyan, T::kOrange};
    const ImU32 kViolet = IM_COL32(0xB0, 0x8C, 0xFF, 255);
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
        status = "REC  \xC2\xB7  play the pads";
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
    lockOnRightClick(static_cast<int>(LoopSource::Notes), playing, st.locked);
}

void PerformanceView::drawButtons(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool any = false;
    for (const auto& t : m.buttons)
        any |= t.isSet();
    // No title bar either: the pads, their names and what they play.
    (void)any;
    const float top = y;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), T::kPanel, px(8));
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), T::kBorder, px(8), px(1));

    struct Pad {
        Button button;
        ImU32 color;  // face buttons only
    };
    static constexpr Pad kPads[] = {
        {Button::LeftShoulder, 0}, {Button::LeftTrigger, 0}, {Button::LeftStick, 0},     {Button::Back, 0},
        {Button::South, kPsCross}, {Button::East, kPsCircle}, {Button::West, kPsSquare},  {Button::North, kPsTriangle},
        {Button::Start, 0},        {Button::Guide, 0},       {Button::Touchpad, 0},      {Button::Misc1, 0},
        {Button::RightStick, 0},   {Button::RightTrigger, 0}, {Button::RightShoulder, 0},
    };
    const float pad = px(12);
    const float slot = (w - 2 * pad) / static_cast<float>(std::size(kPads));
    const float body = y + h - top;
    const float cy = top + body * 0.30f;
    for (std::size_t i = 0; i < std::size(kPads); ++i) {
        const Pad& pd = kPads[i];
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
            const char* code = pd.button == Button::Back       ? "CRE"
                               : pd.button == Button::Start    ? "OPT"
                               : pd.button == Button::Touchpad ? "TP"
                               : pd.button == Button::Misc1    ? "MUTE"
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
        text(dl, gMedium, px(12), cx, ty, T::kLabel, fit(gMedium, px(12), label(pd.button), slot - 2), H::Center, V::Middle);
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
