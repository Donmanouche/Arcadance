#pragma once

#include "midipad/Humanize.hpp"
#include "midipad/Scales.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace midipad {

// What the application remembers between sessions.
struct AppSettings {
    std::filesystem::path presetsDir;
    std::string lastPreset;

    enum class Output { VirtualPort, ExistingPort };
    Output output = Output::VirtualPort;
    std::string virtualPortName = "MidiPad";
    std::string existingPortName;  // matched as a substring, like --connect
    bool outputChosen = false;     // false: ask which output to use at startup

    // Narrows the notes and chords offered by the editor; none: every note.
    std::optional<Key> key;

    // Engine::setDpadSettle, in milliseconds.
    int dpadSettlePressMs = 15;
    int dpadSettleReleaseMs = 15;

    Humanize humanize;  // off on every channel by default

    // Sync: a beat clock, MidiPad's own (at `tempo`) or Ableton Link's, which
    // shares tempo and beat with the DAW. With one, loops snap to `loopSnap`
    // (their length rounded to it, their start on it); without, MidiPad runs free.
    enum class Clock { Free, Internal, Link };
    Clock clock = Clock::Free;
    Clock lastClock = Clock::Link;  // the one the top bar's button turns on
    double tempo = 120;             // the internal clock's, in beats a minute
    bool midiClockOut = true;       // with the internal clock: MIDI Clock, Start and Stop out
    enum class LoopSnap { Free, Beat, Bar, TwoBars, FourBars };
    LoopSnap loopSnap = LoopSnap::Beat;
    int beatsPerBar = 4;
    bool startStopSync = true;  // Play / Stop follow, and lead, the Link session's
    // The notes recorded into a synced note loop (or overdubbed onto it) go on
    // a grid of this many steps a beat: 0 off, 2 eighth notes, 4 sixteenths,
    // 8 thirty-seconds (Engine::setSync).
    int quantize = 0;
    // Moving notes in the note loop editor: onto the tonality's notes, by degrees.
    bool notesInScale = true;
    // The window's frames a second: 0 as fast as the screen refreshes, or at
    // most 60 or 30; and at most 30 while another window has the focus.
    int frameRate = 0;
    bool slowInBackground = false;
    // On a scaled screen (Wayland), open the window without high pixel
    // density: the compositor stretches it; fewer pixels, a little less sharp.
    bool standardResolution = false;
    // The buttons driving the session and edit screens (Engine::setNavButtons),
    // as Button values; -1: none (select: L3 and R3).
    int navSelect = -1, navMenu = -1, navBack = -1;
    // The snap in beats: 0 for Free.
    int snapBeats() const {
        switch (loopSnap) {
        case LoopSnap::Free: return 0;
        case LoopSnap::Beat: return 1;
        case LoopSnap::Bar: return beatsPerBar;
        case LoopSnap::TwoBars: return 2 * beatsPerBar;
        case LoopSnap::FourBars: return 4 * beatsPerBar;
        }
        return 0;
    }
};

// Missing or unreadable values keep the ones of `defaults`.
AppSettings loadSettings(const std::filesystem::path& file, AppSettings defaults);
void saveSettings(const AppSettings& settings, const std::filesystem::path& file);  // throws on I/O error

}  // namespace midipad
