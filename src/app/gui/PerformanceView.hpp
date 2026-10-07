#pragma once

#include "Runtime.hpp"

#include "midipad/Preset.hpp"
#include "midipad/Scales.hpp"

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
        static constexpr unsigned kBackground = 0xFF2B2826;  // ABGR, as ImU32
        static constexpr unsigned kPanel = 0xFF3B3734;
        static constexpr unsigned kPanelHeader = 0xFF312E2C;
        static constexpr unsigned kBorder = 0xFF201E1C;
        static constexpr unsigned kText = 0xFFDCDCDC;
        static constexpr unsigned kDim = 0xFF948F8A;
        static constexpr unsigned kBlue = 0xFFFFA03A;
        static constexpr unsigned kOrange = 0xFF1AA5F2;
        static constexpr unsigned kPink = 0xFF6D33E8;
        static constexpr unsigned kGreen = 0xFF84DC3D;
    };

    // Draws into the remaining space of the current window. Returns the
    // control clicked, as a MappingEditor::select() spec ("button:south",
    // "dpad:up", "zone:1:top_left", "tab:axes"...), to open the editor on it.
    std::optional<std::string> draw(const midipad::Mapping& mapping, const LiveState& live,
                                    const std::optional<midipad::Key>& key);

private:
    struct Trail {
        std::deque<std::pair<float, float>> points;  // normalized positions, newest last
        std::deque<double> times;
    };

    void drawStick(int index, const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawTriggers(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawDpad(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawTouchpad(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);
    void drawButtons(const midipad::Mapping& m, const LiveState& live, float x, float y, float w, float h);

    // Smooths a displayed value towards its target; `id` names it across frames.
    float smooth(int id, float target, float speed = 14.0f);
    void click(const std::string& spec);

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
};
