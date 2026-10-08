#include "midipad/Chords.hpp"

#include "midipad/Notes.hpp"

#include <string>

namespace midipad {

namespace {

struct Quality {
    std::string_view name;
    std::array<std::int8_t, 7> intervals;
    std::uint8_t count;
};

constexpr Quality kQualities[] = {
    {"", {0, 4, 7}, 3},
    {"m", {0, 3, 7}, 3},
    {"5", {0, 7}, 2},
    {"dim", {0, 3, 6}, 3},
    {"aug", {0, 4, 8}, 3},
    {"sus2", {0, 2, 7}, 3},
    {"sus4", {0, 5, 7}, 3},
    {"6", {0, 4, 7, 9}, 4},
    {"m6", {0, 3, 7, 9}, 4},
    {"7", {0, 4, 7, 10}, 4},
    {"maj7", {0, 4, 7, 11}, 4},
    {"m7", {0, 3, 7, 10}, 4},
    {"mmaj7", {0, 3, 7, 11}, 4},
    {"dim7", {0, 3, 6, 9}, 4},
    {"m7b5", {0, 3, 6, 10}, 4},
    {"7sus4", {0, 5, 7, 10}, 4},
    {"7#5", {0, 4, 8, 10}, 4},
    {"7b5", {0, 4, 6, 10}, 4},
    {"maj7#5", {0, 4, 8, 11}, 4},
    {"add9", {0, 4, 7, 14}, 4},
    {"69", {0, 4, 7, 9, 14}, 5},
    {"m69", {0, 3, 7, 9, 14}, 5},
    {"9", {0, 4, 7, 10, 14}, 5},
    {"maj9", {0, 4, 7, 11, 14}, 5},
    {"m9", {0, 3, 7, 10, 14}, 5},
    {"mmaj9", {0, 3, 7, 11, 14}, 5},
    {"9sus4", {0, 5, 7, 10, 14}, 5},
    {"m9b5", {0, 3, 6, 10, 14}, 5},
    {"7#9", {0, 4, 7, 10, 15}, 5},
    {"7b9", {0, 4, 7, 10, 13}, 5},
    {"11", {0, 4, 7, 10, 14, 17}, 6},
    {"maj11", {0, 4, 7, 11, 14, 17}, 6},
    {"m11", {0, 3, 7, 10, 14, 17}, 6},
    {"m11b5", {0, 3, 6, 10, 14, 17}, 6},
    {"maj7#11", {0, 4, 7, 11, 14, 18}, 6},
    {"9#11", {0, 4, 7, 10, 14, 18}, 6},
    {"9b13", {0, 4, 7, 10, 14, 20}, 6},
    {"13", {0, 4, 7, 10, 14, 17, 21}, 7},
    {"maj13", {0, 4, 7, 11, 14, 17, 21}, 7},
    {"m13", {0, 3, 7, 10, 14, 17, 21}, 7},
};

constexpr auto kQualityNames = [] {
    std::array<std::string_view, std::size(kQualities)> names{};
    for (std::size_t i = 0; i < names.size(); ++i)
        names[i] = kQualities[i].name;
    return names;
}();

// Spellings that differ from the canonical names once normalized.
constexpr std::pair<std::string_view, std::string_view> kAliases[] = {
    {"maj", ""},       {"dom7", "7"},       {"sus", "sus4"},     {"7sus", "7sus4"},
    {"9sus", "9sus4"}, {"aug7", "7#5"},     {"augmaj7", "maj7#5"}, {"dimmaj7", "mmaj7"},
    {"mm7", "m7"},  // older presets wrote "mm7" for a minor seventh
};

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != prefix[i])
            return false;
    }
    return true;
}

// Rewrites the many common spellings of a quality to the canonical names:
// "M7"/"Maj7"/"major7"/"Δ" -> "maj7", "min7"/"-7" -> "m7", "mM7" -> "mmaj7",
// "°"/"o" -> "dim", "ø" -> "m7b5", "7-9" -> "7b9", "+" -> "aug", "6/9" -> "69".
// Upper-case M means major and lower-case m minor, so case matters only there.
std::string normalizeQuality(std::string_view q) {
    std::string out;
    bool delta = false;
    for (std::size_t i = 0; i < q.size();) {
        std::string_view rest = q.substr(i);
        const bool leading = out.empty();
        if (startsWithNoCase(rest, "major")) {
            out += "maj", i += 5;
        } else if (startsWithNoCase(rest, "minor")) {
            out += "m", i += 5;
        } else if (startsWithNoCase(rest, "maj")) {
            out += "maj", i += 3;
        } else if (startsWithNoCase(rest, "min")) {
            out += "m", i += 3;
        } else if (startsWithNoCase(rest, "dim") || startsWithNoCase(rest, "aug") ||
                   startsWithNoCase(rest, "sus") || startsWithNoCase(rest, "add") ||
                   startsWithNoCase(rest, "dom")) {
            for (int k = 0; k < 3; ++k)
                out += static_cast<char>(rest[static_cast<std::size_t>(k)] | 0x20);
            i += 3;
        } else if (rest.starts_with("Δ")) {  // Δ
            out += "maj", i += 2, delta = true;
        } else if (rest.starts_with("ø")) {  // ø, ø7
            out += "m7b5", i += 2;
            if (q.substr(i).starts_with('7'))
                ++i;
        } else if (rest.starts_with("°")) {  // °
            out += "dim", i += 2;
        } else if (rest.starts_with("⁰")) {  // ⁰
            out += "dim", i += 3;
        } else if (rest.starts_with("♯")) {  // ♯
            out += '#', i += 3;
        } else if (rest.starts_with("♭")) {  // ♭
            out += 'b', i += 3;
        } else {
            char c = rest[0];
            ++i;
            if (c == 'M')
                out += "maj";
            else if (c == 'o' || c == 'O')
                out += "dim";
            else if (c == '-')
                out += leading ? "m" : "b";
            else if (c == '+')
                out += leading ? "aug" : "#";
            else if (c == '/' || c == '(' || c == ')' || c == ' ' || c == '_')
                continue;
            else
                out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
    if (delta && out == "maj")
        out = "maj7";
    for (auto [alias, canonical] : kAliases)
        if (out == alias)
            return std::string(canonical);
    return out;
}

const Quality* findQuality(std::string_view canonical) {
    for (const auto& q : kQualities)
        if (q.name == canonical)
            return &q;
    return nullptr;
}

}  // namespace

std::span<const std::string_view> chordQualities() { return kQualityNames; }

std::optional<ChordNotes> parseChord(std::string_view name, int rootOctave) {
    while (!name.empty() && name.front() == ' ')
        name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ')
        name.remove_suffix(1);

    std::size_t rootLength = 0;
    auto rootPitch = parsePitch(name, &rootLength);
    if (!rootPitch)
        return std::nullopt;
    std::string_view quality = name.substr(rootLength);

    // "F/A" is a slash chord; in "C6/9" the slash belongs to the quality.
    std::optional<int> bassPitch;
    if (auto slash = quality.rfind('/'); slash != std::string_view::npos) {
        std::string_view bass = quality.substr(slash + 1);
        std::size_t bassLength = 0;
        if (auto pitch = parsePitch(bass, &bassLength); pitch && bassLength == bass.size()) {
            bassPitch = pitch;
            quality = quality.substr(0, slash);
        }
    }

    const Quality* q = findQuality(normalizeQuality(quality));
    if (!q)
        return std::nullopt;

    const int root = (rootOctave + 1) * 12 + *rootPitch;
    ChordNotes chord;
    bool inRange = true;
    auto add = [&chord, &inRange](int note) {
        if (note < 0 || note > 127) {
            inRange = false;  // a chord missing notes would mislead
            return;
        }
        for (std::uint8_t i = 0; i < chord.count; ++i)
            if (chord.notes[i] == note)
                return;
        chord.notes[chord.count++] = static_cast<std::uint8_t>(note);
    };
    if (bassPitch)
        add(rootOctave * 12 + *bassPitch);  // the bass pitch, one octave below the root's octave
    for (std::uint8_t i = 0; i < q->count; ++i)
        add(root + q->intervals[i]);
    if (!inRange || chord.count == 0)
        return std::nullopt;
    std::sort(chord.notes.begin(), chord.notes.begin() + chord.count);
    return chord;
}

}  // namespace midipad
