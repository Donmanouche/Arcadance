#pragma once

#include "Runtime.hpp"

#include "midipad/Preset.hpp"
#include "midipad/Scales.hpp"

#include <imgui.h>

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <utility>

// The animated view: one module per part of the controller (sticks, triggers,
// D-pad, touchpad) with a live "screen" and knobs showing the CCs sent, and a
// strip of pads for the buttons. Everything follows the gamepad.
class PerformanceView {
public:
    PerformanceView();  // picks the idle animations, at random

    // Colors of the view, shared with the chrome GuiApp draws around it.
    struct Theme {
        static constexpr ImU32 kBackground = IM_COL32(0x1C, 0x1E, 0x21, 255);
        static constexpr ImU32 kTopBar = IM_COL32(0x16, 0x18, 0x1A, 255);
        static constexpr ImU32 kPanel = IM_COL32(0x2E, 0x31, 0x36, 255);
        static constexpr ImU32 kPanelHeader = IM_COL32(0x22, 0x24, 0x28, 255);
        static constexpr ImU32 kBorder = IM_COL32(0x4A, 0x4F, 0x57, 255);
        static constexpr ImU32 kControl = IM_COL32(0x3A, 0x3E, 0x45, 255);
        static constexpr ImU32 kControlBorder = IM_COL32(0x5C, 0x62, 0x6B, 255);
        static constexpr ImU32 kRow = IM_COL32(0x26, 0x2A, 0x30, 255);
        static constexpr ImU32 kRowBorder = IM_COL32(0x4E, 0x55, 0x60, 255);
        static constexpr ImU32 kText = IM_COL32(0xF4, 0xF5, 0xF7, 255);
        static constexpr ImU32 kLabel = IM_COL32(0xBF, 0xC4, 0xCB, 255);
        static constexpr ImU32 kDim = IM_COL32(0x7E, 0x84, 0x8C, 255);
        static constexpr ImU32 kBlue = IM_COL32(0x4D, 0xB2, 0xFF, 255);
        static constexpr ImU32 kOrange = IM_COL32(0xFF, 0xB0, 0x20, 255);
        static constexpr ImU32 kPink = IM_COL32(0xFF, 0x4F, 0x8B, 255);
        static constexpr ImU32 kGreen = IM_COL32(0x3D, 0xDC, 0x84, 255);
        static constexpr ImU32 kRed = IM_COL32(0xFF, 0x3B, 0x4E, 255);
        static constexpr ImU32 kIce = IM_COL32(0xAE, 0xE3, 0xFF, 255);
        static constexpr ImU32 kCyan = IM_COL32(0x78, 0xDC, 0xFF, 255);
    };

    // The fonts of the view; null ones fall back to the default font.
    struct Fonts {
        ImFont* medium = nullptr;
        ImFont* bold = nullptr;
        ImFont* condensed = nullptr;  // badges: notes, chords, CCs
    };
    void setFonts(const Fonts& fonts) { fonts_ = fonts; }

    // A note the engine sent (velocity 0: released), at `time` (ImGui::GetTime()),
    // for the piano roll.
    void addNote(int channel, int note, int velocity, bool fromLoop, double time);

    // Draws into the remaining space of the current window. Returns the
    // control clicked, as a MappingEditor::drawControl() spec ("button:south",
    // "dpad:up", "zone:1:top_left", "axis:left_x", "finger:1"...).
    std::optional<std::string> draw(const midipad::Mapping& mapping, const LiveState& live,
                                    const std::optional<midipad::Key>& key);
    // Shift + mouse wheel over a movement loop's LOOP panel, during the last
    // draw(): the loops to stretch (a bit per LoopSource) and by how many steps
    // (see Engine::stretchLoop; up stretches, down compresses). Ctrl + wheel:
    // their amplitude instead (Engine::amplifyLoop; up widens).
    struct StretchRequest {
        std::uint8_t sources = 0;
        int steps = 0;
        bool amplitude = false;
    };
    std::optional<StretchRequest> takeStretchRequest() { return std::exchange(stretch_, std::nullopt); }

private:
    struct Trail {
        std::deque<std::pair<float, float>> points;  // normalized positions, newest last
        std::deque<double> times;
    };

    // Remembers where each stick axis was when it froze, per channel.
    void trackFrozenPositions(const LiveState& live);

    void drawStick(int index, const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawTriggers(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawDpad(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawTouchpad(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawButtons(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawScale(const std::optional<midipad::Key>& key, float x, float y, float w, float h);
    void drawChannel(const LiveState& live, float x, float y, float w, float h);
    void drawPadLanes(ImDrawList* dl, const midipad::Mapping& m, const LiveState& live, float x, float w, float loopY,
                      float boxTop, float boxBottom, float timelineH);
    void drawPianoRoll(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawLoopPanel(ImDrawList* dl, float x, float w, float loopY, float bottom, const LiveState::Loop& loop,
                       const midipad::LoopBinding& binding, ImU32 colorX, ImU32 colorY, int idleArt,
                       const char* waitStatus, const char* id, const std::string& spec, bool upward, int source,
                       const char* legendX = nullptr, const char* legendY = nullptr);
    // Stretching loops: reads Shift + wheel over a LOOP panel (a, b), for the
    // loops in `sources`; draws the gauge of `stretch` at the bottom of the box
    // while the wheel turns; the header chip and length text for a stretch.
    void stretchWheel(std::uint8_t sources, ImVec2 a, ImVec2 b);
    void drawStretchGauge(ImDrawList* dl, ImVec2 ga, ImVec2 gb, std::uint8_t sources, int stretch, ImU32 color);
    float drawStretchChip(ImDrawList* dl, float right, float y, int stretch, ImU32 color);  // returns its left edge
    // The same for the amplitude (percent of the recorded swing): chip "AMP 60%", gauge 0-100 %.
    float drawAmplitudeChip(ImDrawList* dl, float right, float y, int amplitude, ImU32 color);
    void drawAmplitudeGauge(ImDrawList* dl, ImVec2 ga, ImVec2 gb, std::uint8_t sources, int amplitude, ImU32 color);
    float drawChip(ImDrawList* dl, float right, float y, const std::string& label, ImU32 color);

    // Smooths a displayed value towards its target; `id` names it across frames.
    float smooth(int id, float target, float speed = 14.0f);
    void click(const std::string& spec);

    Fonts fonts_;
    double time_ = 0;
    float dt_ = 0;
    bool flats_ = false;
    std::optional<std::string> clicked_;
    std::optional<StretchRequest> stretch_;
    float wheelRest_ = 0;           // a partial wheel notch (touchpads scroll by fractions)
    double lastWheel_ = -1e9;       // when the last notch turned, for the acceleration
    int wheelStreak_ = 0;           // notches turned in a quick row
    std::array<int, 8> lastStretch_{};  // per LoopSource, on lastStretchChannel_
    std::array<int, 8> lastAmplitude_ = [] {
        std::array<int, 8> full{};
        full.fill(100);
        return full;
    }();
    std::array<double, 8> amplitudeShown_ = [] {  // per LoopSource: when its amplitude last changed
        std::array<double, 8> never{};
        never.fill(-1e9);
        return never;
    }();
    int lastStretchChannel_ = -1;
    std::array<double, 8> stretchShown_ = [] {  // per LoopSource: when its stretch last changed
        std::array<double, 8> never{};
        never.fill(-1e9);
        return never;
    }();
    std::array<float, 64> smoothed_{};
    std::array<bool, 64> smoothedInit_{};
    std::array<double, 32> lastPress_ = [] {  // per SDL button, for the flash
        std::array<double, 32> never{};
        never.fill(-1e9);
        return never;
    }();
    std::uint32_t previousButtons_ = 0;
    std::array<Trail, midipad::kFingerCount> trails_;
    // The notes sent recently, for the piano roll.
    struct RollNote {
        double start = 0, end = 0;  // `end` once released
        std::uint8_t channel = 0, note = 0, velocity = 0;
        bool fromLoop = false;
        bool sounding = true;
    };
    std::deque<RollNote> rollNotes_;
    // The animation of each empty LOOP box (sticks, triggers, touchpad), per
    // channel: drawn at random at startup, all different within a channel.
    std::array<std::array<std::uint8_t, 4>, 16> idleArt_{};
    int artFor(int channel, int box) const {
        return idleArt_[static_cast<std::size_t>(std::clamp(channel, 0, 15))][static_cast<std::size_t>(box)];
    }
    // Stick positions (-1..1) at the moment each axis froze, per channel; a bit
    // of frozenKnown_ per stick axis once recorded.
    std::array<std::array<float, 4>, 16> frozenAt_{};
    std::array<std::uint8_t, 16> frozenKnown_{};
    int lastChannel_ = -1;
    std::uint8_t lastFrozen_ = 0;
};
