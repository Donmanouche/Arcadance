#include "gui/PerformanceView.hpp"

#include "midipad/Engine.hpp"
#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    }
    return {};
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
    for (float y = a.y; y < b.y; y += 3.0f)
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(0, 0, 0, 55), 1.0f);
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
    gMedium = fonts_.medium ? fonts_.medium : ImGui::GetFont();
    gBold = fonts_.bold ? fonts_.bold : gMedium;
    gCondensed = fonts_.condensed ? fonts_.condensed : gBold;
    time_ = ImGui::GetTime();
    dt_ = ImGui::GetIO().DeltaTime;
    flats_ = key && prefersFlats(*key);
    clicked_.reset();
    trackFrozenPositions(live);

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

// The notes and chords sent on the active channel: the last seconds left of
// "now", and on its right what the note loop will play next.
void PerformanceView::drawPianoRoll(const Mapping& m, const LiveState& live, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr auto kNotes = static_cast<std::size_t>(LoopSource::Notes);
    const LoopStatus& st = live.loops[kNotes].status;
    const LoopBinding& lb = m.loops[kNotes];
    const bool recording = st.state == LoopStatus::State::Recording;
    const bool playing = st.state == LoopStatus::State::Playing;
    const ImU32 kViolet = IM_COL32(0xB0, 0x8C, 0xFF, 255);
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
    } else if (playing) {
        status = std::string("LOOP  ") +
                 (lb.playback == LoopPlayback::Forward ? "FWD" : lb.playback == LoopPlayback::Reverse ? "REV" : "BACK & FORTH") +
                 "  \xC2\xB7  " + seconds;
        statusColor = kViolet;
    }

    // Geometry: keyboard, then the roll; "now" at two thirds.
    const float ax0 = x + px(12), ay0 = y + px(10), ax1 = x + w - px(12), ay1 = y + h - px(10);
    if (ay1 - ay0 < px(30))
        return;
    const float keyW = px(20), gx0 = ax0 + keyW + px(16);
    constexpr double kPast = 4.0, kFuture = 2.0;
    const float nowX = gx0 + (ax1 - gx0) * static_cast<float>(kPast / (kPast + kFuture));
    const float pxPerSec = (nowX - gx0) / static_cast<float>(kPast);
    auto xAt = [&](double t) { return nowX + static_cast<float>(t - time_) * pxPerSec; };

    // The range: the notes in view, two octaves at least, eased.
    int lo = 127, hi = 0;
    for (const RollNote& n : rollNotes_)
        if (n.channel == live.channel && (n.sounding || n.end > time_ - kPast)) {
            lo = std::min<int>(lo, n.note);
            hi = std::max<int>(hi, n.note);
        }
    if (playing || recording)
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
        if (n % 12 == 0) {
            dl->AddLine(ImVec2(gx0, yb), ImVec2(ax1, yb), IM_COL32(255, 255, 255, 30));
            text(dl, gBold, px(8), ax0 + keyW + px(2), yb - rowH * 0.5f, T::kDim, "C" + std::to_string(n / 12 - 1), H::Left, V::Middle);
        }
    }
    // Half-second lines, scrolling with time.
    for (double t = std::floor(time_ * 2.0) / 2.0 + 0.5 * 4; xAt(t) > gx0; t -= 0.5)
        if (xAt(t) < ax1)
            dl->AddLine(ImVec2(xAt(t), ay0), ImVec2(xAt(t), ay1), IM_COL32(255, 255, 255, 18));

    // What was played: white, or violet from a loop; brighter when louder.
    bool any = false;
    for (const RollNote& n : rollNotes_) {
        if (n.channel != live.channel)
            continue;
        const double end = n.sounding ? time_ : n.end;
        if (end < time_ - kPast)
            continue;
        any = true;
        const float yb = yOf(n.note);
        const float a = 0.55f + 0.45f * static_cast<float>(n.velocity) / 127.0f;
        dl->AddRectFilled(ImVec2(std::max(xAt(n.start), gx0), yb - rowH + 0.5f), ImVec2(std::max(xAt(end), xAt(n.start) + px(2)), yb - 0.5f),
                          alpha(n.fromLoop ? kViolet : kLive, a), px(1));
    }

    // What the note loop plays next: its steps simulated over the next seconds.
    if (playing && st.length > 1 && live.noteLoopCount > 0) {
        any = true;
        constexpr int kStride = 2;
        const int steps = static_cast<int>(kFuture * Engine::kLoopRateHz) / kStride;
        std::array<int, LiveState::kMaxNoteLoopNotes> runStart;
        runStart.fill(-1);
        int p = st.position, d = st.direction;
        auto drawRun = [&](int i, int from, int to) {
            const NoteInterval& n = live.noteLoop[static_cast<std::size_t>(i)];
            const float xa = nowX + static_cast<float>(from * kStride) / Engine::kLoopRateHz * pxPerSec;
            const float xb = nowX + static_cast<float>(to * kStride) / Engine::kLoopRateHz * pxPerSec;
            const float yb = yOf(n.note);
            dl->AddRect(ImVec2(xa, yb - rowH + 0.5f), ImVec2(std::max(xb, xa + px(2)), yb - 0.5f), alpha(kViolet, 0.85f), px(1), px(1));
        };
        for (int k = 0; k <= steps; ++k) {
            for (int i = 0; i < live.noteLoopCount; ++i) {
                const NoteInterval& n = live.noteLoop[static_cast<std::size_t>(i)];
                const bool on = k < steps && p >= n.start && p < n.end;
                if (on && runStart[static_cast<std::size_t>(i)] < 0)
                    runStart[static_cast<std::size_t>(i)] = k;
                if (!on && runStart[static_cast<std::size_t>(i)] >= 0) {
                    drawRun(i, runStart[static_cast<std::size_t>(i)], k);
                    runStart[static_cast<std::size_t>(i)] = -1;
                }
            }
            for (int s2 = 0; s2 < kStride; ++s2) {  // as the engine steps
                if (lb.playback == LoopPlayback::Forward) {
                    p = (p + 1) % st.length;
                } else if (lb.playback == LoopPlayback::Reverse) {
                    p = (p - 1 + st.length) % st.length;
                } else {
                    p += d;
                    if (p >= st.length) {
                        d = -1;
                        p = std::max(0, st.length - 2);
                    } else if (p < 0) {
                        d = 1;
                        p = std::min(1, st.length - 1);
                    }
                }
            }
        }
    }
    dl->PopClipRect();

    if (any || playing) {
        dl->AddLine(ImVec2(nowX, ay0), ImVec2(nowX, ay1), recording ? alpha(T::kRed, 0.9f) : IM_COL32(255, 255, 255, 190), px(1.3f));
    } else {
        text(dl, gMedium, px(11), (gx0 + ax1) * 0.5f, (ay0 + ay1) * 0.5f, st.waiting ? T::kLabel : T::kDim,
             st.waiting ? "waiting for notes\xE2\x80\xA6" : "notes played appear here  \xC2\xB7  hold Record and play to loop them",
             H::Center, V::Middle);
    }
    if (!status.empty()) {
        const float tw = textWidth(gBold, px(10), status);
        dl->AddRectFilled(ImVec2(ax1 - tw - px(12), ay0 + px(3)), ImVec2(ax1 - px(3), ay0 + px(17)), IM_COL32(0x20, 0x22, 0x26, 230), px(4));
        text(dl, gBold, px(10), ax1 - px(7), ay0 + px(10), statusColor, status, H::Right, V::Middle);
    }
    if (hoverButton("##pianoroll", ImVec2(ax0, ay0), ImVec2(ax1, ay1)))
        click("notes");
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
    if (hoverButton(index == 0 ? "##stickL" : "##stickR", s.a, s.b))
        click(index == 0 ? "axes:left_x,left_y" : "axes:right_x,right_y");

    // What the axes send: their CCs, as chips (ice blue while frozen).
    const float midiY = s.b.y + px(18);
    section(dl, x, w, midiY, "MIDI");
    const float chipTop = midiY + px(12), chipH = px(20);
    const float cw = (w - px(30)) * 0.5f;
    auto axisChip = [&](float cx0, const AxisBinding& b, Axis a, const char* name) {
        const std::string label = std::string(name) + "  " + (b.enabled ? "CC " + std::to_string(b.cc) : "-") + (b.invert ? "  inv" : "");
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
    drawLoopPanel(dl, x, w, loopY, y + h - px(10), live.loops[static_cast<std::size_t>(index)],
                  m.loops[static_cast<std::size_t>(index)], accent, mix(accent, T::kText, 0.5f), "hold Record and move this stick",
                  "REC  \xC2\xB7  move a stick", index == 0 ? "##loopL" : "##loopR",
                  index == 0 ? "axes:left_x,left_y" : "axes:right_x,right_y", false);
}

// A LOOP panel: its header (state, playback, length), and the loop's two
// curves over time with a playhead and the CCs it sends.
void PerformanceView::drawLoopPanel(ImDrawList* dl, float x, float w, float loopY, float bottom, const LiveState::Loop& loop,
                                    const LoopBinding& lb, ImU32 colorX, ImU32 colorY, const char* idleHint,
                                    const char* waitStatus, const char* id, const std::string& spec, bool upward,
                                    const char* legendX, const char* legendY) {
    const float gTop = loopY + px(10), gBottom = bottom;
    const LoopStatus& st = loop.status;
    const bool recording = st.state == LoopStatus::State::Recording;
    const bool playing = st.state == LoopStatus::State::Playing;
    const float seconds = static_cast<float>(st.length) / Engine::kLoopRateHz;
    char secondsText[16];
    std::snprintf(secondsText, sizeof secondsText, "%.1f s", static_cast<double>(seconds));

    // Header: LOOP ——— status.
    const float size = px(11);
    text(dl, gBold, size, x + px(12), loopY, T::kDim, "LOOP", H::Left, V::Middle);
    std::string status;
    ImU32 statusColor = T::kDim;
    if (recording) {
        status = std::string("REC  ") + secondsText;
        statusColor = T::kRed;
    } else if (st.waiting) {
        status = waitStatus;
        statusColor = T::kRed;
    } else if (playing) {
        status = std::string(lb.playback == LoopPlayback::Forward   ? "FWD"
                             : lb.playback == LoopPlayback::Reverse ? "REV"
                                                                    : "BACK & FORTH") +
                 "  \xC2\xB7  " + secondsText;
        statusColor = colorX;
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
                dl->AddTriangleFilled(ImVec2(cx - s2, loopY), ImVec2(cx + s2, loopY - s2 * 1.2f), ImVec2(cx + s2, loopY + s2 * 1.2f), colorX);
            else
                dl->AddTriangleFilled(ImVec2(cx + s2, loopY), ImVec2(cx - s2, loopY - s2 * 1.2f), ImVec2(cx - s2, loopY + s2 * 1.2f), colorX);
        };
        if (lb.playback == LoopPlayback::BackAndForth) {
            arrow(iconX - px(4), true);
            arrow(iconX + px(3), false);
        } else {
            arrow(iconX, lb.playback == LoopPlayback::Reverse);
        }
    }
    const float ruleEnd = status.empty() ? x + w - px(12) : iconX - px(10);
    const float ruleStart = x + px(16) + textWidth(gBold, size, "LOOP");
    if (ruleEnd > ruleStart)
        dl->AddLine(ImVec2(ruleStart, loopY), ImVec2(ruleEnd, loopY), IM_COL32(0x40, 0x44, 0x4B, 255), px(1));

    // The box, its border telling the state.
    const ImVec2 ga(x + px(12), gTop), gb(x + w - px(12), gBottom);
    dl->AddRectFilled(ga, gb, kGraph, px(4));
    const ImU32 border = recording ? T::kRed : st.waiting ? alpha(T::kRed, 0.5f) : playing ? alpha(colorX, 0.45f) : kGraph;
    dl->AddRect(ga, gb, border, px(4), px(1.3f));
    const ImVec2 mid((ga.x + gb.x) * 0.5f, (ga.y + gb.y) * 0.5f);
    if (!recording && !playing) {
        text(dl, gMedium, px(11), mid.x, mid.y, st.waiting ? T::kLabel : T::kDim,
             st.waiting ? "waiting for movement\xE2\x80\xA6" : idleHint, H::Center, V::Middle);
    } else if (loop.points > 1) {
        // While recording the curve grows over a 4-second window, then fills the box.
        const float lx0 = ga.x + px(6), lx1 = ga.x + px(6) + (gb.x - ga.x - px(12)) * (recording ? std::min(1.0f, seconds / 4.0f) : 1.0f);
        const float ly0 = ga.y + px(5), ly1 = gb.y - px(5);
        auto at = [&](int i, int axis) {
            const float t = static_cast<float>(i) / static_cast<float>(loop.points - 1);
            const float v = static_cast<float>((axis == 0 ? loop.x : loop.y)[static_cast<std::size_t>(i)]) / 255.0f;
            return ImVec2(lx0 + t * (lx1 - lx0), upward ? ly1 - v * (ly1 - ly0) : ly0 + v * (ly1 - ly0));
        };
        for (int axis = 0; axis < 2; ++axis) {
            ImVec2 pts[LiveState::Loop::kPoints];
            for (int i = 0; i < loop.points; ++i)
                pts[i] = at(i, axis);
            dl->AddPolyline(pts, loop.points, axis == 0 ? colorX : colorY, px(1.8f));
        }
        if (recording) {
            dl->AddLine(ImVec2(lx1, ga.y + px(2)), ImVec2(lx1, gb.y - px(2)), alpha(T::kRed, 0.8f), px(1.3f));
        } else {
            // Playhead, with a dot on each curve.
            const float t = st.length > 1 ? static_cast<float>(st.position) / static_cast<float>(st.length - 1) : 0.0f;
            const float hx = lx0 + t * (lx1 - lx0);
            dl->AddLine(ImVec2(hx, ga.y + px(2)), ImVec2(hx, gb.y - px(2)), IM_COL32(255, 255, 255, 170), px(1.3f));
            const int i = std::clamp(static_cast<int>(std::lround(t * static_cast<float>(loop.points - 1))), 0, loop.points - 1);
            for (int axis = 0; axis < 2; ++axis) {
                const ImVec2 p(hx, at(i, axis).y);
                dl->AddCircleFilled(p, px(3.2f), axis == 0 ? colorX : colorY);
                dl->AddCircle(p, px(3.2f), IM_COL32_WHITE, 0, px(1));
            }
            // The CCs the loop sends.
            const std::string ccs = "CC " + std::to_string(lb.ccX) + " / " + std::to_string(lb.ccY);
            const float bw = textWidth(gCondensed, px(10), ccs) + px(10);
            const ImVec2 ba(ga.x + px(4), gb.y - px(15)), bb(ga.x + px(4) + bw, gb.y - px(4));
            dl->AddRectFilled(ba, bb, alpha(colorX, 0.2f), px(5));
            dl->AddRect(ba, bb, alpha(colorX, 0.8f), px(5), px(1));
            text(dl, gCondensed, px(10), ba.x + px(5), (ba.y + bb.y) * 0.5f, T::kText, ccs, H::Left, V::Middle);
        }
    }
    if (legendX && playing) {  // which curve is which
        text(dl, gBold, px(10), gb.x - px(24), gb.y - px(9), colorY, legendY, H::Right, V::Middle);
        text(dl, gBold, px(10), gb.x - px(42), gb.y - px(9), colorX, legendX, H::Right, V::Middle);
    }
    if (hoverButton(id, ImVec2(ga.x, loopY - px(6)), gb))
        click(spec);
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
        badge(dl, gCondensed, px(13), ccX0 + ccW * 0.5f, midY - px(10), px(20), ab.enabled ? "CC " + std::to_string(ab.cc) : "-",
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
    drawLoopPanel(dl, x, w, loopY, y + h - px(10), live.loops[static_cast<std::size_t>(LoopSource::Triggers)],
                  m.loops[static_cast<std::size_t>(LoopSource::Triggers)], T::kPink, pinkLight,
                  "hold Record and press a trigger", "REC  \xC2\xB7  press a trigger", "##looptriggers",
                  "axes:left_trigger,right_trigger", true, "L2", "R2");
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
                const std::string label = std::string(k == 0 ? "X  " : "Y  ") + (b.enabled ? "CC " + std::to_string(b.cc) : "-");
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
    const LiveState::Loop* loops[] = {&live.loops[2], &live.loops[3]};
    auto state = [&](int f) { return loops[f]->status.state; };
    const bool recording = state(0) == LoopStatus::State::Recording || state(1) == LoopStatus::State::Recording;
    const bool waiting = loops[0]->status.waiting || loops[1]->status.waiting;
    const int shown = state(0) == LoopStatus::State::Playing ? 0 : state(1) == LoopStatus::State::Playing ? 1 : -1;
    const bool anyOn = m.fingers[0].mode != TouchMode::Off || m.fingers[1].mode != TouchMode::Off;

    std::string status;
    ImU32 statusColor = T::kDim;
    char secondsText[16];
    if (recording) {
        const int f = state(0) == LoopStatus::State::Recording ? 0 : 1;
        std::snprintf(secondsText, sizeof secondsText, "%.1f s",
                      static_cast<double>(loops[f]->status.length) / Engine::kLoopRateHz);
        status = std::string("REC  ") + secondsText;
        statusColor = T::kRed;
    } else if (waiting) {
        status = "REC  \xC2\xB7  touch to record";
        statusColor = T::kRed;
    } else if (shown >= 0) {
        const LoopPlayback pb = m.loops[static_cast<std::size_t>(2 + shown)].playback;
        std::snprintf(secondsText, sizeof secondsText, "%.1f s",
                      static_cast<double>(loops[shown]->status.length) / Engine::kLoopRateHz);
        status = std::string(pb == LoopPlayback::Forward ? "FWD" : pb == LoopPlayback::Reverse ? "REV" : "BACK & FORTH") +
                 "  \xC2\xB7  " + secondsText;
        statusColor = fingerColors[shown];
    }
    text(dl, gBold, px(11), x + px(12), loopY, T::kDim, "LOOP", H::Left, V::Middle);
    const float statusX = x + w - px(12) - textWidth(gBold, px(10), status);
    if (!status.empty())
        text(dl, gBold, px(10), statusX, loopY, statusColor, status, H::Left, V::Middle);
    if (recording || waiting)
        dl->AddCircleFilled(ImVec2(statusX - px(9), loopY), px(3.5f), alpha(T::kRed, recording ? 1.0f : 0.6f));
    const float ruleEnd = status.empty() ? x + w - px(12) : statusX - px(recording || waiting ? 18 : 8);
    const float ruleStart = x + px(16) + textWidth(gBold, px(11), "LOOP");
    if (ruleEnd > ruleStart)
        dl->AddLine(ImVec2(ruleStart, loopY), ImVec2(ruleEnd, loopY), IM_COL32(0x40, 0x44, 0x4B, 255), px(1));

    const ImVec2 ga(x + px(12), boxTop), gb(x + w - px(12), boxBottom);
    dl->AddRectFilled(ga, gb, kGraph, px(4));
    const ImU32 border = recording ? T::kRed : waiting ? alpha(T::kRed, 0.5f) : shown >= 0 ? alpha(fingerColors[shown], 0.45f) : kGraph;
    dl->AddRect(ga, gb, border, px(4), px(1.3f));
    const ImVec2 pa(ga.x + px(6), ga.y + px(6)), pb(gb.x - px(6), gb.y - px(6) - timelineH);
    // 4-pad loops: one lane per pad, a block while a pad played. Otherwise the
    // paths, over a small touchpad.
    bool lanes = false;
    for (int f = 0; f < kFingerCount; ++f)
        lanes |= state(f) != LoopStatus::State::Empty && loops[f]->status.zones && loops[f]->points > 1;
    if (!lanes) {
        for (float gx = pa.x + px(6); gx < pb.x; gx += px(12))
            for (float gy = pa.y + px(6); gy < pb.y; gy += px(12))
                dl->AddCircleFilled(ImVec2(gx, gy), 1.0f, IM_COL32(255, 255, 255, 22));
        dl->AddLine(ImVec2((pa.x + pb.x) * 0.5f, pa.y), ImVec2((pa.x + pb.x) * 0.5f, pb.y), IM_COL32(255, 255, 255, 30));
        dl->AddLine(ImVec2(pa.x, (pa.y + pb.y) * 0.5f), ImVec2(pb.x, (pa.y + pb.y) * 0.5f), IM_COL32(255, 255, 255, 30));
    }
    bool drewAny = false;
    if (lanes) {
        static constexpr const char* kPadNames[] = {"TL", "TR", "BL", "BR"};
        const float laneH = (pb.y - pa.y) / kZoneCount;
        const float lx0 = pa.x + px(18), lx1 = pb.x;
        for (int z = 0; z < kZoneCount; ++z) {
            const float ly = pa.y + laneH * static_cast<float>(z);
            text(dl, gBold, px(9), pa.x, ly + laneH * 0.5f, T::kDim, kPadNames[z], H::Left, V::Middle);
            if (z > 0)
                dl->AddLine(ImVec2(lx0, ly), ImVec2(lx1, ly), IM_COL32(255, 255, 255, 18));
        }
        for (int f = 0; f < kFingerCount; ++f) {
            const LiveState::Loop& L = *loops[f];
            if (state(f) == LoopStatus::State::Empty || !L.status.zones || L.points < 2)
                continue;
            drewAny = true;
            const bool both = loops[1 - f]->status.zones && state(1 - f) != LoopStatus::State::Empty;
            const float span = state(f) == LoopStatus::State::Recording
                                   ? std::min(1.0f, static_cast<float>(L.status.length) / (4.0f * Engine::kLoopRateHz))
                                   : 1.0f;
            auto xAt = [&](int i) { return lx0 + (lx1 - lx0) * span * static_cast<float>(i) / static_cast<float>(L.points - 1); };
            int start = -1, pad = -1;
            for (int i = 0; i <= L.points; ++i) {
                const int p = i < L.points && L.on[static_cast<std::size_t>(i)] ? L.x[static_cast<std::size_t>(i)] : -1;
                if (p == pad)
                    continue;
                if (pad >= 0 && pad < kZoneCount) {
                    float blockTop = pa.y + laneH * static_cast<float>(pad) + px(2), blockBottom = blockTop + laneH - px(4);
                    if (both)  // finger 1 above, finger 2 below, within the lane
                        (f == 0 ? blockBottom : blockTop) = (blockTop + blockBottom) * 0.5f;
                    dl->AddRectFilled(ImVec2(xAt(start), blockTop), ImVec2(std::max(xAt(i - 1), xAt(start) + px(2)), blockBottom),
                                      alpha(fingerColors[f], 0.75f), px(2));
                }
                pad = p;
                start = i;
            }
            const float hx = state(f) == LoopStatus::State::Recording
                                 ? xAt(L.points - 1)
                                 : lx0 + (lx1 - lx0) * (L.status.length > 1 ? static_cast<float>(L.status.position) /
                                                                                 static_cast<float>(L.status.length - 1)
                                                                            : 0.0f);
            dl->AddLine(ImVec2(hx, pa.y), ImVec2(hx, pb.y),
                        state(f) == LoopStatus::State::Recording ? alpha(T::kRed, 0.8f) : alpha(fingerColors[f], 0.9f), px(1.3f));
        }
    }
    for (int f = 0; f < kFingerCount; ++f) {
        const LiveState::Loop& L = *loops[f];
        if (lanes || state(f) == LoopStatus::State::Empty || L.points < 2)
            continue;
        drewAny = true;
        auto at = [&](int i) {
            return ImVec2(pa.x + static_cast<float>(L.x[static_cast<std::size_t>(i)]) / 255.0f * (pb.x - pa.x),
                          pa.y + static_cast<float>(L.y[static_cast<std::size_t>(i)]) / 255.0f * (pb.y - pa.y));
        };
        // The path, broken where the finger was lifted.
        for (int i = 1; i < L.points; ++i)
            if (L.on[static_cast<std::size_t>(i)] && L.on[static_cast<std::size_t>(i - 1)])
                dl->AddLine(at(i - 1), at(i), alpha(fingerColors[f], 0.6f), px(1.6f));
        // Where it plays now (or the newest point while recording).
        const int i = state(f) == LoopStatus::State::Recording
                          ? L.points - 1
                          : std::clamp(L.status.length > 1 ? static_cast<int>(std::lround(static_cast<float>(L.status.position) /
                                                                                              static_cast<float>(L.status.length - 1) *
                                                                                              static_cast<float>(L.points - 1)))
                                                           : 0,
                                       0, L.points - 1);
        if (L.on[static_cast<std::size_t>(i)]) {
            const ImVec2 c = at(i);
            dl->AddCircleFilled(c, px(8), alpha(fingerColors[f], 0.25f));
            dl->AddCircleFilled(c, px(4), state(f) == LoopStatus::State::Recording ? T::kRed : fingerColors[f]);
            text(dl, gBold, px(10), c.x + px(6), c.y - px(14), T::kText, f == 0 ? "1" : "2");
        }
    }
    if (!drewAny)
        text(dl, gMedium, px(11), (ga.x + gb.x) * 0.5f, (ga.y + gb.y) * 0.5f, waiting ? T::kLabel : T::kDim,
             waiting ? "waiting for a finger\xE2\x80\xA6" : anyOn ? "hold Record and touch the pad" : "touchpad off",
             H::Center, V::Middle);

    // Timeline: when each finger touched, and where the loop plays.
    for (int f = 0; f < kFingerCount; ++f) {
        const LiveState::Loop& L = *loops[f];
        if (state(f) == LoopStatus::State::Empty || L.points < 2)
            continue;
        const float ty = gb.y - timelineH + px(5) + static_cast<float>(f) * px(8);
        const float tx0 = ga.x + px(2), tx1 = gb.x - px(2);
        const float span = state(f) == LoopStatus::State::Recording
                               ? std::min(1.0f, static_cast<float>(L.status.length) / (4.0f * Engine::kLoopRateHz))
                               : 1.0f;
        int start = -1;
        for (int i = 0; i <= L.points; ++i) {
            const bool on = i < L.points && L.on[static_cast<std::size_t>(i)];
            if (on && start < 0)
                start = i;
            if (!on && start >= 0) {
                const float a = tx0 + (tx1 - tx0) * span * static_cast<float>(start) / static_cast<float>(L.points - 1);
                const float b = tx0 + (tx1 - tx0) * span * static_cast<float>(std::max(i - 1, start)) / static_cast<float>(L.points - 1);
                dl->AddRectFilled(ImVec2(a, ty - px(2.5f)), ImVec2(std::max(b, a + px(2)), ty + px(2.5f)), fingerColors[f], px(2));
                start = -1;
            }
        }
        if (state(f) == LoopStatus::State::Playing && L.status.length > 1) {
            const float hx = tx0 + (tx1 - tx0) * static_cast<float>(L.status.position) / static_cast<float>(L.status.length - 1);
            dl->AddLine(ImVec2(hx, ty - px(4)), ImVec2(hx, ty + px(4)), IM_COL32_WHITE, px(1.5f));
        }
    }
    if (hoverButton("##touchloop", ImVec2(ga.x, loopY - px(6)), gb))
        click("fingers");
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
