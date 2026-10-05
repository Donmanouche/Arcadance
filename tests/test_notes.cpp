#include "midipad/Notes.hpp"

#include <doctest/doctest.h>

using namespace midipad;

TEST_CASE("note names use scientific pitch notation") {
    CHECK(parseNoteName("C4") == 60);
    CHECK(parseNoteName("A4") == 69);
    CHECK(parseNoteName("c4") == 60);
    CHECK(parseNoteName("C-1") == 0);
    CHECK(parseNoteName("G9") == 127);
    CHECK(parseNoteName("Db3") == 49);
    CHECK(parseNoteName("A#3") == 58);
    CHECK(parseNoteName("C♯4") == 61);
}

TEST_CASE("E and B are read as themselves (the Python version shifted them)") {
    CHECK(parseNoteName("E4") == 64);
    CHECK(parseNoteName("B3") == 59);
    CHECK(parseNoteName("Eb4") == 63);
    CHECK(parseNoteName("Bb3") == 58);
}

TEST_CASE("accidentals can cross the octave boundary") {
    CHECK(parseNoteName("Cb4") == 59);
    CHECK(parseNoteName("B#3") == 60);
}

TEST_CASE("invalid note names are rejected") {
    CHECK_FALSE(parseNoteName(""));
    CHECK_FALSE(parseNoteName("C"));
    CHECK_FALSE(parseNoteName("H4"));
    CHECK_FALSE(parseNoteName("C4x"));
    CHECK_FALSE(parseNoteName("G#9"));
    CHECK_FALSE(parseNoteName("C-2"));
}

TEST_CASE("note numbers print with sharps") {
    CHECK(noteName(60) == "C4");
    CHECK(noteName(61) == "C#4");
    CHECK(noteName(0) == "C-1");
    CHECK(noteName(128).empty());
}
