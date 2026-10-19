#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace midipad {

// Notes of a chord, lowest first. Nine is enough for a thirteenth chord plus
// a slash bass and a doubled root.
struct ChordNotes {
    std::array<std::uint8_t, 9> notes{};
    std::uint8_t count = 0;
    std::uint8_t bassCount = 0;  // the first notes: a slash bass and a doubled root, under the chord's own notes

    std::span<const std::uint8_t> view() const { return {notes.data(), count}; }
    friend bool operator==(const ChordNotes& a, const ChordNotes& b) {
        return std::equal(a.view().begin(), a.view().end(), b.view().begin(), b.view().end());
    }
};

// "Cm9", "Bb7sus4", "Abmaj7#11", "CM7", "C6/9", "F/A"... The root is placed
// in `rootOctave` (4 -> C4 = 60); a slash bass goes one octave below it.
// `rootBelow` (1 or 2) doubles the root that many octaves lower, to anchor
// the bass. `inversion` moves the chord's lowest note up an octave that many
// times (at most its number of notes - 1; the bass notes stay). None if the
// name is unknown or a note falls outside 0..127.
std::optional<ChordNotes> parseChord(std::string_view name, int rootOctave = 4, int rootBelow = 0, int inversion = 0);

// The chord's own notes (not its bass notes) in the inversion, at the
// octave or the octave below, nearest to `previous`: the notes move as little
// as possible from one chord to the next. `chord` as it is when `previous` is
// empty.
ChordNotes voiceLead(const ChordNotes& chord, std::span<const std::uint8_t> previous);

// Canonical quality suffixes, in the order a chord picker should show them
// ("" is the major triad).
std::span<const std::string_view> chordQualities();

}  // namespace midipad
