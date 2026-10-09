#include "midipad/Chords.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace midipad;

namespace {

std::vector<int> notes(std::string_view name) {
    auto chord = parseChord(name);
    if (!chord)
        return {};
    return {chord->view().begin(), chord->view().end()};
}

}  // namespace

TEST_CASE("chords used by the existing presets keep their notes") {
    CHECK(notes("Abmaj7#11") == std::vector{68, 72, 75, 79, 82, 86});
    CHECK(notes("Bb7sus4") == std::vector{70, 75, 77, 80});
    CHECK(notes("Cm") == std::vector{60, 63, 67});
    CHECK(notes("Cm9") == std::vector{60, 63, 67, 70, 74});
    CHECK(notes("Ebmaj11") == std::vector{63, 67, 70, 74, 77, 80});
    CHECK(notes("Fm7") == std::vector{65, 68, 72, 75});
    CHECK(notes("Fm7b5") == std::vector{65, 68, 71, 75});
}

TEST_CASE("E and B roots are not shifted down") {
    CHECK(notes("E") == std::vector{64, 68, 71});
    CHECK(notes("Bm") == std::vector{71, 74, 78});
}

TEST_CASE("upper-case M means major, lower-case m minor") {
    const std::vector maj7{60, 64, 67, 71};
    CHECK(notes("CM7") == maj7);
    CHECK(notes("Cmaj7") == maj7);
    CHECK(notes("CMaj7") == maj7);
    CHECK(notes("Cmajor7") == maj7);
    CHECK(notes("CΔ") == maj7);
    CHECK(notes("CΔ7") == maj7);
    CHECK(notes("Cm7") == std::vector{60, 63, 67, 70});
    CHECK(notes("CmM7") == std::vector{60, 63, 67, 71});
    CHECK(notes("Cm(maj7)") == std::vector{60, 63, 67, 71});
    CHECK(notes("CM11") == notes("Cmaj11"));
    CHECK(notes("CM11").size() == 6);
}

TEST_CASE("common alternative spellings") {
    CHECK(notes("Cmin") == notes("Cm"));
    CHECK(notes("Cminor") == notes("Cm"));
    CHECK(notes("C-") == notes("Cm"));
    CHECK(notes("C-7") == notes("Cm7"));
    CHECK(notes("C°") == notes("Cdim"));
    CHECK(notes("Co7") == notes("Cdim7"));
    CHECK(notes("Cø") == notes("Cm7b5"));
    CHECK(notes("Cø7") == notes("Cm7b5"));
    CHECK(notes("Cm7-5") == notes("Cm7b5"));
    CHECK(notes("C+") == notes("Caug"));
    CHECK(notes("C7+5") == notes("C7#5"));
    CHECK(notes("C7-9") == notes("C7b9"));
    CHECK(notes("Csus") == notes("Csus4"));
    CHECK(notes("C6/9") == notes("C69"));
    CHECK(notes("Cm6/9") == notes("Cm69"));
    CHECK(notes("C♯") == notes("C#"));
}

TEST_CASE("slash chords add the bass one octave below") {
    CHECK(notes("C/E") == std::vector{52, 60, 64, 67});
    CHECK(notes("F/A") == std::vector{57, 65, 69, 72});
    CHECK(notes("C/G") == std::vector{55, 60, 64, 67});
    CHECK(notes("C13/D").size() == 8);
}

TEST_CASE("the root octave can be changed") {
    auto chord = parseChord("C", 3);
    REQUIRE(chord);
    CHECK(chord->notes[0] == 48);
}

TEST_CASE("the root can be doubled one or two octaves lower") {
    auto one = parseChord("Ebmaj7", 4, 1);
    REQUIRE(one);
    CHECK(std::vector(one->view().begin(), one->view().end()) == std::vector<std::uint8_t>{51, 63, 67, 70, 74});
    auto two = parseChord("F/A", 4, 2);
    REQUIRE(two);
    CHECK(two->notes[0] == 41);  // F2, under the A3 slash bass
    CHECK(parseChord("C13/D", 4, 1)->count == 9);
    CHECK_FALSE(parseChord("C", 0, 2));  // below MIDI note 0
}

TEST_CASE("invalid chords are rejected") {
    CHECK_FALSE(parseChord(""));
    CHECK_FALSE(parseChord("H"));
    CHECK_FALSE(parseChord("Cfoo"));
    CHECK_FALSE(parseChord("m7"));
}

TEST_CASE("the quality list is usable by a chord picker") {
    auto q = chordQualities();
    CHECK(q.front().empty());
    CHECK(std::find(q.begin(), q.end(), "maj7") != q.end());
    for (auto name : q) {
        CAPTURE(name);
        CHECK(parseChord("C" + std::string(name)));
    }
}
