#pragma once

#include "Runtime.hpp"

#include "midipad/Preset.hpp"
#include "midipad/Scales.hpp"

#include <imgui.h>

#include <array>
#include <deque>
#include <optional>
#include <string>

// The animated view: one module per part of the controller (sticks, triggers,
// D-pad, touchpad) with a live "screen" and knobs showing the CCs sent, and a
// strip of pads for the buttons. Everything follows the gamepad.
class PerformanceView {
public:
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

    // Draws into the remaining space of the current window. Returns the
    // control clicked, as a MappingEditor::drawControl() spec ("button:south",
    // "dpad:up", "zone:1:top_left", "axis:left_x", "finger:1"...).
    std::optional<std::string> draw(const midipad::Mapping& mapping, const LiveState& live,
                                    const std::optional<midipad::Key>& key);

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
    void drawLoopPanel(ImDrawList* dl, float x, float w, float loopY, float bottom, const LiveState::Loop& loop,
                       const midipad::LoopBinding& binding, ImU32 colorX, ImU32 colorY, const char* idleHint,
                       const char* waitStatus, const char* id, const std::string& spec, bool upward,
                       const char* legendX = nullptr, const char* legendY = nullptr);

    // Smooths a displayed value towards its target; `id` names it across frames.
    float smooth(int id, float target, float speed = 14.0f);
    void click(const std::string& spec);

    Fonts fonts_;
    double time_ = 0;
    float dt_ = 0;
    bool flats_ = false;
    std::optional<std::string> clicked_;
    std::array<float, 64> smoothed_{};
    std::array<bool, 64> smoothedInit_{};
    std::array<double, 32> lastPress_ = [] {  // per SDL button, for the flash
        std::array<double, 32> never{};
        never.fill(-1e9);
        return never;
    }();
    std::uint32_t previousButtons_ = 0;
    std::array<Trail, midipad::kFingerCount> trails_;
    // Stick positions (-1..1) at the moment each axis froze, per channel; a bit
    // of frozenKnown_ per stick axis once recorded.
    std::array<std::array<float, 4>, 16> frozenAt_{};
    std::array<std::uint8_t, 16> frozenKnown_{};
    int lastChannel_ = -1;
    std::uint8_t lastFrozen_ = 0;
};
