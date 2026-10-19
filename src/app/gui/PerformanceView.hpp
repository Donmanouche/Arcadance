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
#include <vector>

// A synced loop's length ("1 BAR", "3 BEATS"), a free one's ("4.0 s", "1:30 min").
std::string loopBeatsText(double beats, int beatsPerBar);
std::string loopDurationText(double seconds);

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
    // Shift + mouse wheel over a movement loop's lane (or its LOOP header:
    // every lane), during the last draw(): the axis loops to stretch (a bit
    // per Engine track) and by how many steps (see Engine::stretchLoop; up
    // stretches, down compresses). Ctrl + wheel: their amplitude instead
    // (Engine::amplifyLoop; up widens).
    struct StretchRequest {
        std::uint16_t tracks = 0;
        int steps = 0;
        bool amplitude = false;
    };
    std::optional<StretchRequest> takeStretchRequest() { return std::exchange(stretch_, std::nullopt); }
    // The session screen, in place of the modules: a column of clips per
    // channel (eight at a time), the cursor on `cursorSlot` of the active
    // channel. What a click asks for, if anything: launching a clip (or
    // choosing an empty slot), stopping a channel, choosing a channel.
    //
    // Under the clips, the chain of the active channel: its steps, each a clip
    // and its repeats, played left to right (Engine::setChainStep). The cursor
    // is in it when `cursorSlot` is kClipSlots, on step `chainCursor` (the end
    // cell after the last step: kChainSteps). A click on a step puts the
    // cursor there; the wheel over its number changes the clip, over its chip
    // the repeats; a right-click opens its menu; a clip dragged from the grid
    // onto it sets it. The strip shows one of the channel's chains (the one
    // setChains gives); the CHAIN cell after the last step shows its letter:
    // its arrows or the wheel show another chain (ChainShow), a click plays
    // or stops it. The CHAIN button starts or stops it too.
    struct SessionAction {
        enum class Kind : std::uint8_t { Launch, Stop, Channel, ChainSet, ChainCursor, ChainGo, ChainStop, ChainShow } kind = Kind::Launch;
        int channel = 0, slot = 0;  // ChainSet / ChainCursor / ChainGo: slot is the step; ChainShow: the chain
        int clip = -1, repeats = 1;  // ChainSet
    };
    std::optional<SessionAction> drawSession(const LiveState& live, int cursorSlot, int chainCursor);
    // The song screen: on top, the whole song at scale, in real time (its
    // sections, a lane per channel, the playhead); under it a row per
    // section (its name, its length; "+ section" after the last) and a
    // column per channel (eight at a time), each cell its items at scale;
    // at the bottom the editor of the cell under the cursor, its items on
    // the bars of the section. The cursor: a row, a column (-1: the
    // section's name), and in the editor (`editing`) an item (its count: the
    // "+" after the last). What a click, a drag or the wheel asks for, if anything.
    struct SongCursor {
        int row = 0, col = -1, item = 0;
        bool editing = false;
    };
    struct SongAction {
        enum class Kind : std::uint8_t { Cursor, Play, Stop, Loop, AddSection, EditCell, Bars, ItemSelect, ItemMove, ItemContent, ItemRepeats, AddItem } kind =
            Kind::Cursor;
        int row = 0, col = -1, item = 0, value = 0;
    };
    std::optional<SongAction> drawSong(const LiveState& live, const midipad::Song& song, const SongCursor& cursor);
    // An item's length in beats (`open`: it has a forever step), and a section's in bars, from what the engine says.
    static double songItemBeats(const LiveState& live, int channel, const midipad::SongItem& item, bool* open = nullptr);
    static int songSectionBars(const LiveState& live, const midipad::SongSection& section);
    static std::string restText(int beats, int beatsPerBar);  // "2 bars 1 beat"
    static std::vector<double> songItemBars(const LiveState& live, int channel, const std::vector<midipad::SongItem>& items);
    std::optional<SessionAction> drawChainStrip(ImDrawList* dl, const LiveState& live, ImVec2 at, float w, float h, int cursor);
    // The repeats a chain step may take, in order (0: forever), and the one
    // after or before `repeats` (by `steps`, kept within them).
    static constexpr std::array<int, 15> kChainRepeats{1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32, 48, 64, 0};
    static int nextRepeats(int repeats, int steps);
    struct LockRequest {  // a LoopSource and an axis (-1: both)
        int source = 0, axis = -1;
    };
    // What a context menu is for: a lane (an axis loop), a LOOP panel (its
    // axes), the note loop, a control (its window, `spec`), a clip, a chain
    // step (`slot` is the step), a channel's column, the CHANNEL block, a
    // chain (the CHAIN cell; `slot` is the chain), a song section (`slot`),
    // a song cell (`slot` the section, `channel`), a song item (also `axis`:
    // the item), the clips and chains of a channel to put in a song cell
    // (SongPick: `axis` the item they replace, -1 a new one after the last).
    struct MenuTarget {
        enum class Kind : std::uint8_t { Lane, Panel, NoteLoop, Control, Clip, Step, Channel, ChannelBlock, Chain, Section, SongCell, SongItem, SongPick } kind =
            Kind::Control;
        int channel = 0, slot = 0, source = 0, axis = -1;
        std::string spec;
        MenuTarget() = default;
        MenuTarget(Kind k, int ch = 0, int sl = 0, int src = 0, int ax = -1, std::string sp = {})
            : kind(k), channel(ch), slot(sl), source(src), axis(ax), spec(std::move(sp)) {}
    };
    // A right-click on one of them during the last draw: its menu, opened
    // where the mouse is.
    struct MenuRequest {
        MenuTarget target;
        ImVec2 at;
    };
    std::optional<MenuRequest> takeMenuRequest() { return std::exchange(menuRequest_, std::nullopt); }
    // The CHANNEL block's arrows or the wheel over it, during the last draw: the channel asked for.
    std::optional<int> takeChannelRequest() { return std::exchange(channelRequest_, std::nullopt); }
    // The channels' names (Preset::channelNames), shown with their numbers.
    void setChannelNames(const std::array<std::string, midipad::kChannelCount>& names) { channelNames_ = names; }
    // The chain the session screen shows for each channel, and the chains' names.
    using ChainNames = std::array<std::array<std::string, midipad::Engine::kChains>, midipad::kChannelCount>;
    void setChains(const std::array<int, midipad::kChannelCount>& shown, const ChainNames& names) {
        shownChains_ = shown;
        chainNames_ = names;
    }
    static std::string chainLetter(int chain) { return std::string(1, static_cast<char>('A' + chain)); }
    // Edit mode: a cursor on the controls, the lanes and the piano roll of the
    // main view, drawn by the next draw(). moveFocus goes to the nearest one
    // that way; activateFocus clicks it at the next draw (opens its window,
    // or the note loop's editor); focusTarget: what is under it, for its menu.
    void setEditMode(bool on) { editMode_ = on; }
    void moveFocus(int dx, int dy);
    void activateFocus() { focusClick_ = true; }
    std::optional<MenuTarget> focusTarget() const;
    // Where the controller's cursor is (edit mode, or the session screen, as
    // last drawn): its box, for a menu opened beside it.
    std::pair<ImVec2, ImVec2> cursorBox() const;
    // The help: a legend of (keys, what they do).
    void drawHelp(const std::string& title, ImU32 color, const std::string& closeHint,
                  const std::vector<std::pair<std::vector<std::string>, std::string>>& rows);
    // A click on the x of a lane, during the last draw(): that axis' loop to
    // clear (Engine::clearLoop), as a LockRequest.
    std::optional<LockRequest> takeClearRequest() { return std::exchange(clearRequest_, std::nullopt); }
    // The note loop's editor: a large piano roll, `w` by `h`, at the cursor of
    // the current window (the main view's piano roll only shows the loop; a
    // click on it opens this editor). Dragging a note moves it, or the
    // selection it belongs to, as a block (its first note onto the quantize
    // grid when there is one; up and down by degrees of the tonality when
    // moves stay on the scale); dragging over empty space selects the notes in
    // a rectangle; Ctrl + click adds or takes out a note; a double-click
    // selects the chord it belongs to. Dragging the end of a selected note
    // lengthens or shortens every selected note alike (its end onto the
    // quantize grid when there is one). A right-click, or Delete, removes a
    // note or the selection it belongs to. A click elsewhere or Escape clears
    // the selection. The edits come out of takeNoteEdit().
    void drawNoteEditor(const midipad::Mapping& mapping, const LiveState& live, float w, float h);
    // Moves up and down go by degrees of the tonality (when there is one).
    void setScaleMoves(bool on) { scaleMoves_ = on; }
    struct NoteEdit {
        std::vector<midipad::LoopNoteRef> notes;
        bool remove = false;
        int samples = 0, semitones = 0;  // moved by; semitones are degrees when byDegrees
        bool byDegrees = false;
        bool resize = false;  // lengthened by `samples` instead
        int loopLength = 0;   // > 0: the loop's new length, in samples, instead
    };
    std::optional<NoteEdit> takeNoteEdit() { return std::exchange(noteEdit_, std::nullopt); }
    // Escape, during the last drawNoteEditor(), cancelled a drag or cleared the
    // selection: the window should stay open.
    bool tookEscape() const { return escapeTaken_; }
    void setQuantize(int stepsPerBeat) { quantize_ = stepsPerBeat; }  // AppSettings::quantize

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
    // A lane of a LOOP panel: an axis loop, how it looks and what it sends.
    struct LaneLook {
        const LiveState::Lane* lane = nullptr;
        int track = 0;  // Engine::trackOf
        ImU32 color = 0;
        const char* name = "";
        bool enabled = true, invert = false;
        bool flip = false;  // a finger's Y: sent the other way up
        int cc = 0;  // the loop's CC, or below 0 when its axis shapes the notes (laneTarget)
        midipad::StepGrid steps;
    };
    // `st`: the lanes together (LiveState::Loop::status); `lockSource` the
    // LoopSource a right-click locks.
    void drawLoopPanel(ImDrawList* dl, float x, float w, float loopY, float bottom, const midipad::LoopStatus& st,
                       midipad::LoopPlayback playback, const LaneLook* lanes, int laneCount, ImU32 color, int idleArt,
                       const char* waitStatus, const char* id, const std::string& spec, int lockSource);
    void drawLane(ImDrawList* dl, ImVec2 a, ImVec2 b, const LaneLook& look);
    // Stretching loops: reads Shift + wheel over (a, b), for the tracks in
    // `tracks`; draws the gauge of `stretch` at the bottom of the box while
    // the wheel turns; the chip for a stretch.
    void stretchWheel(std::uint16_t tracks, ImVec2 a, ImVec2 b);
    void drawStretchGauge(ImDrawList* dl, ImVec2 ga, ImVec2 gb, std::uint16_t tracks, int stretch, ImU32 color);
    float drawStretchChip(ImDrawList* dl, float right, float y, int stretch, ImU32 color);  // returns its left edge
    // The same for the amplitude (percent of the recorded swing): chip "AMP 60%", gauge 0-800 %.
    float drawAmplitudeChip(ImDrawList* dl, float right, float y, int amplitude, ImU32 color);
    void drawAmplitudeGauge(ImDrawList* dl, ImVec2 ga, ImVec2 gb, std::uint16_t tracks, int amplitude, ImU32 color);
    float drawChip(ImDrawList* dl, float right, float y, const std::string& label, ImU32 color);

    // Smooths a displayed value towards its target; `id` names it across frames.
    float smooth(int id, float target, float speed = 14.0f);
    void click(const std::string& spec);

    Fonts fonts_;
    double time_ = 0;
    float dt_ = 0;
    bool flats_ = false;
    std::optional<std::string> clicked_;
    int beatsPerBar_ = 4;  // of the sync, for the loops' beat grids
    std::optional<StretchRequest> stretch_;
    std::optional<LockRequest> clearRequest_;
    std::optional<MenuRequest> menuRequest_;
    std::optional<int> channelRequest_;
    std::array<std::string, midipad::kChannelCount> channelNames_;
    std::array<int, midipad::kChannelCount> shownChains_{};
    int songScroll_ = 0, songFirst_ = 0;  // the first section row and channel shown
    int songDragBars_ = 0;                // an item being dragged: by how many bars
    ChainNames chainNames_;
    // "CHANNEL 3", or "3  PADS" when it has a name.
    std::string channelTitle(int channel) const;
    bool editMode_ = false, focusClick_ = false;
    std::string focus_;  // the id of the item under the edit cursor
    std::pair<ImVec2, ImVec2> sessionCursorBox_{};  // the session screen's cursor, as last drawn
    struct FocusItem {
        std::string id;
        ImVec2 a, b;
    };
    std::vector<FocusItem> focusItems_;  // gathered by the last draw in edit mode
    std::vector<std::pair<std::string, MenuTarget>> focusTargets_;  // the items with a menu of their own
    // A lane, the piano roll: a focus item with its menu; true when the edit cursor clicks it.
    bool focusable(const std::string& id, ImVec2 a, ImVec2 b, const MenuTarget& target);
    std::optional<LockRequest> laneClearHovered_;  // the x under the mouse, while drawing a LOOP panel
    // Over the item just drawn (a LOOP panel, the piano roll), within (a, b)
    // when given: a right-click asks for the menu of `target`; a tooltip says
    // so once there is a loop.
    void menuOnRightClick(const MenuTarget& target, bool recorded, bool locked, const ImVec2* a = nullptr, const ImVec2* b = nullptr);
    std::optional<NoteEdit> noteEdit_;
    int quantize_ = 0;
    bool editing_ = false;     // drawing the note loop editor, not the view's piano roll
    bool scaleMoves_ = true;
    std::optional<midipad::Key> key_;
    struct NoteDrag {
        std::vector<midipad::NoteInterval> notes;  // the notes moving together
        float grabSample = 0;  // where the mouse took them, in loop samples
        float grabY = 0;
        int samples = 0, semitones = 0;  // where they go, so far
        bool resize = false;  // their ends, not the notes: `samples` lengthens them
        int grabEnd = 0;      // resizing: the end grabbed, in loop samples
    };
    std::optional<NoteDrag> drag_;
    // The loop's end being dragged in the editor: where the mouse took it, the
    // loop's length and beats then (0: free), and the new length so far.
    struct LengthDrag {
        float grabX = 0;
        int length = 0;
        double beats = 0;
        int newLength = 0;
    };
    std::optional<LengthDrag> lengthDrag_;
    bool lengthHovered_ = false;  // the mouse over the loop's end handle
    // The editor's view of the loop: zoomed this many times (1: all of it),
    // from this fraction of it; where its roll was drawn, for the scrollbar
    // under it, and where the scrollbar's thumb was taken.
    float viewZoom_ = 1, viewStart_ = 0;
    float rollGx0_ = 0, rollAx1_ = 0;
    bool editorClip_ = false;
    float scrollGrab_ = 0;
    bool escapeTaken_ = false;
    int sessionFirst_ = 0;  // the first channel the session screen shows (0 or 8)
    int sessionChannelSeen_ = -1;
    std::vector<midipad::LoopNoteRef> selection_;  // in the active channel's note loop
    int selectionChannel_ = -1;
    double lastNoteEdit_ = -1e9;
    std::optional<ImVec2> bandFrom_;  // a selection rectangle being drawn, from there
    bool selected(const midipad::NoteInterval& n) const;
    float wheelRest_ = 0;           // a partial wheel notch (touchpads scroll by fractions)
    double lastWheel_ = -1e9;       // when the last notch turned, for the acceleration
    int wheelStreak_ = 0;           // notches turned in a quick row
    std::array<int, 16> lastStretch_{};  // per Engine track, on lastStretchChannel_
    std::array<int, 16> lastAmplitude_ = [] {
        std::array<int, 16> full{};
        full.fill(100);
        return full;
    }();
    std::array<double, 16> amplitudeShown_ = [] {  // per track: when its amplitude last changed
        std::array<double, 16> never{};
        never.fill(-1e9);
        return never;
    }();
    int lastStretchChannel_ = -1;
    std::array<double, 16> stretchShown_ = [] {  // per track: when its stretch last changed
        std::array<double, 16> never{};
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
