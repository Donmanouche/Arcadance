#include "midipad/Notes.hpp"
#include "midipad/Scales.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace midipad;

namespace {

Key key(const char* root, const char* scale) {
    return {*parsePitch(root) % 12, *findScale(scale)};
}

std::vector<std::string> names(const Key& k) {
    std::vector<std::string> out;
    for (const auto& c : chordsInKey(k))
        out.push_back(c.name);
    return out;
}

bool has(const std::vector<std::string>& v, const std::string& name) {
    return std::find(v.begin(), v.end(), name) != v.end();
}

}  // namespace

TEST_CASE("scale membership") {
    const Key aMinor = key("A", "minor");
    CHECK(scalePitchClasses(aMinor) == std::vector{9, 11, 0, 2, 4, 5, 7});  // A B C D E F G
    CHECK(inKey(aMinor, 69));   // A4
    CHECK(inKey(aMinor, 60));   // C4
    CHECK_FALSE(inKey(aMinor, 70));  // A#4
    CHECK(inKey(key("A", "harmonic_minor"), 68));  // G#4
    CHECK(scalePitchClasses(key("C", "pentatonic_minor")).size() == 5);
}

TEST_CASE("keys are spelled with the right accidentals") {
    CHECK_FALSE(prefersFlats(key("A", "minor")));
    CHECK_FALSE(prefersFlats(key("E", "minor")));    // F#
    CHECK(prefersFlats(key("D", "minor")));          // Bb
    CHECK(prefersFlats(key("F", "major")));
    CHECK(prefersFlats(key("C", "dorian")));         // relative major Bb
    CHECK(keyName(key("A", "minor")) == "A natural minor");
    CHECK(keyName(key("Bb", "major")) == "Bb major");
    CHECK(noteName(70, true) == "Bb4");
    CHECK(pitchName(6) == "F#");
}

TEST_CASE("A minor offers its diatonic chords and nothing outside it") {
    const auto chords = names(key("A", "minor"));
    for (const char* name : {"Am", "Bdim", "C", "Dm", "Em", "F", "G", "Am7", "Cmaj7", "Dm7", "Fmaj7", "G7",
                             "Bm7b5", "Fmaj7#11", "Am9", "Csus2", "Asus4", "C6", "G13"})
        CHECK_MESSAGE(has(chords, name), name);
    for (const char* name : {"A", "E", "E7", "Bm", "Caug", "Fm"})
        CHECK_MESSAGE(!has(chords, name), name);

    // By degree, and every chord really fits the scale.
    const auto all = chordsInKey(key("A", "minor"));
    CHECK(all.front().name == "Am");
    CHECK(std::is_sorted(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.degree < b.degree; }));
    for (const auto& c : all)
        for (auto n : c.notes.view())
            CHECK(inKey(key("A", "minor"), n));
}

TEST_CASE("other scales change the chord list") {
    const auto harmonic = names(key("A", "harmonic_minor"));
    CHECK(has(harmonic, "E7"));
    CHECK(has(harmonic, "E"));
    CHECK(has(harmonic, "G#dim7"));
    CHECK_FALSE(has(harmonic, "G"));

    const auto fMajor = names(key("F", "major"));
    CHECK(has(fMajor, "Bb"));
    CHECK(has(fMajor, "Gm7"));
    CHECK_FALSE(has(fMajor, "A#"));  // spelled with flats
}

TEST_CASE("every scale has a menu entry and an identifier") {
    for (const auto& s : scaleTypes()) {
        CHECK_FALSE(s.label.empty());
        CHECK(findScale(s.id).has_value());
    }
    CHECK_FALSE(findScale("klingon"));
}
