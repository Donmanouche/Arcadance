#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace midipad {

// Semitone offset of a note letter plus accidentals relative to C, without
// wrapping: "C" -> 0, "F#" -> 6, "Bb" -> 10, "Cb" -> -1. The letter is
// case-insensitive; accidentals are '#', 'b', U+266F and U+266D.
// `consumed` receives the number of bytes read.
std::optional<int> parsePitch(std::string_view text, std::size_t* consumed = nullptr);

// Scientific pitch notation, middle C = C4 = 60: "A#3" -> 58, "C-1" -> 0.
std::optional<int> parseNoteName(std::string_view text);

// 61 -> "C#4", or "Db4" with flats.
std::string noteName(int midiNote, bool flats = false);

// 1 -> "C#", or "Db" with flats.
std::string pitchName(int pitchClass, bool flats = false);

}  // namespace midipad
