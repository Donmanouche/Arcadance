#include "midipad/Notes.hpp"

#include <array>
#include <charconv>

namespace midipad {

namespace {

constexpr std::string_view kSharp = "♯";
constexpr std::string_view kFlat = "♭";

std::optional<int> letterOffset(char c) {
    switch (c) {
    case 'C': case 'c': return 0;
    case 'D': case 'd': return 2;
    case 'E': case 'e': return 4;
    case 'F': case 'f': return 5;
    case 'G': case 'g': return 7;
    case 'A': case 'a': return 9;
    case 'B': case 'b': return 11;
    default: return std::nullopt;
    }
}

}  // namespace

std::optional<int> parsePitch(std::string_view text, std::size_t* consumed) {
    if (text.empty())
        return std::nullopt;
    auto pitch = letterOffset(text[0]);
    if (!pitch)
        return std::nullopt;

    std::size_t i = 1;
    for (;;) {
        std::string_view rest = text.substr(i);
        if (rest.starts_with('#')) {
            ++*pitch, i += 1;
        } else if (rest.starts_with('b')) {
            --*pitch, i += 1;
        } else if (rest.starts_with(kSharp)) {
            ++*pitch, i += kSharp.size();
        } else if (rest.starts_with(kFlat)) {
            --*pitch, i += kFlat.size();
        } else {
            break;
        }
    }
    if (consumed)
        *consumed = i;
    return pitch;
}

std::optional<int> parseNoteName(std::string_view text) {
    std::size_t used = 0;
    auto pitch = parsePitch(text, &used);
    if (!pitch)
        return std::nullopt;

    std::string_view octaveText = text.substr(used);
    int octave = 0;
    auto [end, error] = std::from_chars(octaveText.data(), octaveText.data() + octaveText.size(), octave);
    if (error != std::errc{} || end != octaveText.data() + octaveText.size())
        return std::nullopt;

    int note = (octave + 1) * 12 + *pitch;
    if (note < 0 || note > 127)
        return std::nullopt;
    return note;
}

std::string noteName(int midiNote) {
    static constexpr std::array<std::string_view, 12> kNames = {"C",  "C#", "D",  "D#", "E",  "F",
                                                                "F#", "G",  "G#", "A",  "A#", "B"};
    if (midiNote < 0 || midiNote > 127)
        return {};
    return std::string(kNames[static_cast<std::size_t>(midiNote % 12)]) + std::to_string(midiNote / 12 - 1);
}

}  // namespace midipad
