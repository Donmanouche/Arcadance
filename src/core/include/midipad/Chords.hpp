#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace midipad {

// Notes of a chord, lowest first. Eight is enough for a thirteenth chord
// plus a slash bass.
struct ChordNotes {
    std::array<std::uint8_t, 8> notes{};
    std::uint8_t count = 0;

    std::span<const std::uint8_t> view() const { return {notes.data(), count}; }
    friend bool operator==(const ChordNotes& a, const ChordNotes& b) {
        return std::equal(a.view().begin(), a.view().end(), b.view().begin(), b.view().end());
    }
};

// "Cm9", "Bb7sus4", "Abmaj7#11", "CM7", "C6/9", "F/A"... The root is placed
// in `rootOctave` (4 -> C4 = 60); a slash bass goes one octave below it.
// None if the name is unknown or a note falls outside 0..127.
std::optional<ChordNotes> parseChord(std::string_view name, int rootOctave = 4);

// Canonical quality suffixes, in the order a chord picker should show them
// ("" is the major triad).
std::span<const std::string_view> chordQualities();

}  // namespace midipad
