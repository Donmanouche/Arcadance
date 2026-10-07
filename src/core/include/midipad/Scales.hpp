#pragma once

#include "midipad/Chords.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midipad {

struct ScaleType {
    std::string_view id;     // stored in settings: "major", "harmonic_minor"...
    std::string_view label;  // for menus
    std::span<const int> intervals;
};

// Every scale MidiPad offers, in menu order.
std::span<const ScaleType> scaleTypes();
std::optional<std::size_t> findScale(std::string_view id);

// A tonality: a root pitch class (C = 0 ... B = 11) and a scale.
struct Key {
    int root = 0;
    std::size_t scale = 0;  // index into scaleTypes()

    const ScaleType& type() const { return scaleTypes()[scale]; }
    friend bool operator==(const Key&, const Key&) = default;
};

bool inKey(const Key& key, int midiNote);
// Pitch classes of the scale, from the root upwards.
std::vector<int> scalePitchClasses(const Key& key);
// Whether notes read better with flats in this key (F major, D minor, C dorian...).
bool prefersFlats(const Key& key);
// "A natural minor", "Bb major".
std::string keyName(const Key& key);

// A chord whose notes all belong to the key.
struct KeyChord {
    std::string name;  // "Am7", spelled for the key
    ChordNotes notes;
    int degree = 0;    // 0 = built on the root of the key
};

// Every chord quality of chordQualities() that fits the key, on every degree
// of the scale: by degree, then in the order of chordQualities().
std::vector<KeyChord> chordsInKey(const Key& key);

}  // namespace midipad
