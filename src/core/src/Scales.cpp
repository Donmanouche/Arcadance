#include "midipad/Scales.hpp"

#include "midipad/Notes.hpp"

#include <algorithm>

namespace midipad {

namespace {

constexpr int kMajor[] = {0, 2, 4, 5, 7, 9, 11};
constexpr int kMinor[] = {0, 2, 3, 5, 7, 8, 10};
constexpr int kHarmonicMinor[] = {0, 2, 3, 5, 7, 8, 11};
constexpr int kMelodicMinor[] = {0, 2, 3, 5, 7, 9, 11};
constexpr int kDorian[] = {0, 2, 3, 5, 7, 9, 10};
constexpr int kPhrygian[] = {0, 1, 3, 5, 7, 8, 10};
constexpr int kLydian[] = {0, 2, 4, 6, 7, 9, 11};
constexpr int kMixolydian[] = {0, 2, 4, 5, 7, 9, 10};
constexpr int kLocrian[] = {0, 1, 3, 5, 6, 8, 10};
constexpr int kPentatonicMajor[] = {0, 2, 4, 7, 9};
constexpr int kPentatonicMinor[] = {0, 3, 5, 7, 10};
constexpr int kBlues[] = {0, 3, 5, 6, 7, 10};
constexpr int kWholeTone[] = {0, 2, 4, 6, 8, 10};

const ScaleType kScales[] = {
    {"major", "major", kMajor},
    {"minor", "natural minor", kMinor},
    {"harmonic_minor", "harmonic minor", kHarmonicMinor},
    {"melodic_minor", "melodic minor", kMelodicMinor},
    {"dorian", "dorian", kDorian},
    {"phrygian", "phrygian", kPhrygian},
    {"lydian", "lydian", kLydian},
    {"mixolydian", "mixolydian", kMixolydian},
    {"locrian", "locrian", kLocrian},
    {"pentatonic_major", "major pentatonic", kPentatonicMajor},
    {"pentatonic_minor", "minor pentatonic", kPentatonicMinor},
    {"blues", "blues", kBlues},
    {"whole_tone", "whole tone", kWholeTone},
};

// How far above the root of the scale its relative major lies; decides the
// key signature, hence sharps or flats.
int relativeMajorOffset(std::string_view id) {
    if (id == "minor" || id == "harmonic_minor" || id == "melodic_minor" || id == "pentatonic_minor" || id == "blues")
        return 3;
    if (id == "dorian")
        return 10;
    if (id == "phrygian")
        return 8;
    if (id == "lydian")
        return 7;
    if (id == "mixolydian")
        return 5;
    if (id == "locrian")
        return 1;
    return 0;
}

int pitchClass(int note) { return ((note % 12) + 12) % 12; }

}  // namespace

std::span<const ScaleType> scaleTypes() { return kScales; }

std::optional<std::size_t> findScale(std::string_view id) {
    for (std::size_t i = 0; i < std::size(kScales); ++i)
        if (kScales[i].id == id)
            return i;
    return std::nullopt;
}

bool inKey(const Key& key, int midiNote) {
    const int offset = pitchClass(midiNote - key.root);
    const auto& iv = key.type().intervals;
    return std::find(iv.begin(), iv.end(), offset) != iv.end();
}

std::vector<int> scalePitchClasses(const Key& key) {
    std::vector<int> pcs;
    for (int interval : key.type().intervals)
        pcs.push_back(pitchClass(key.root + interval));
    return pcs;
}

bool prefersFlats(const Key& key) {
    const int major = pitchClass(key.root + relativeMajorOffset(key.type().id));
    // F, Bb, Eb, Ab, Db, Gb majors are written with flats.
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1 || major == 6;
}

std::string keyName(const Key& key) { return pitchName(key.root, prefersFlats(key)) + " " + std::string(key.type().label); }

std::vector<KeyChord> chordsInKey(const Key& key) {
    std::vector<KeyChord> chords;
    const bool flats = prefersFlats(key);
    const auto degrees = scalePitchClasses(key);
    for (std::size_t degree = 0; degree < degrees.size(); ++degree) {
        const std::string root = pitchName(degrees[degree], flats);
        for (std::string_view quality : chordQualities()) {
            std::string name = root + std::string(quality);
            auto notes = parseChord(name);
            if (!notes)
                continue;
            const auto view = notes->view();
            if (std::all_of(view.begin(), view.end(), [&](std::uint8_t n) { return inKey(key, n); }))
                chords.push_back({std::move(name), *notes, static_cast<int>(degree)});
        }
    }
    return chords;
}

}  // namespace midipad
