#pragma once

#include "midipad/Preset.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace midipad {

// The song: sections played one after the other (INTRO, VERSE, CHORUS...).
// In a section, each channel plays a sequence of items, clips or chains, in
// order: each placed on a bar of the section, or right after the item before
// it (`after`: where that one's repeats end, even within a bar). An item plays its repeats (turns of a clip,
// passes of a chain), then silence until the next item, or with `loop` it
// plays on until the next item or the end of the section. A section lasts
// `bars` bars, or with `fit` as long as its longest channel (whole bars);
// what goes past its end is cut. A Rest item is a silence of `beats` beats:
// the channel stops there, and an item glued after it starts at its end.
struct SongItem {
    enum class Kind : std::uint8_t { Clip, Chain, Rest };
    Kind kind = Kind::Clip;
    int index = 0;      // the clip slot (0-7) or the chain (0-7, A to H)
    int bar = 0;        // where it starts in its section, from 0 (unless `after`)
    int repeats = 1;    // 1 to kMaxSongRepeats
    bool loop = false;  // after its repeats: on until the next item or the section's end
    bool after = false; // starts where the item before it ends (the first: on its bar)
    int beats = 4;      // a Rest's length, 1 to kMaxRestBeats
    friend bool operator==(const SongItem&, const SongItem&) = default;
};

inline constexpr int kMaxSongRepeats = 64;
inline constexpr int kMaxSectionBars = 256;
inline constexpr int kMaxSongItems = 32;  // per channel and section
inline constexpr int kMaxRestBeats = 1024;

struct SongSection {
    std::string name;
    int bars = 8;
    bool fit = false;
    std::array<std::vector<SongItem>, kChannelCount> cells;  // each in playing order
    friend bool operator==(const SongSection&, const SongSection&) = default;
};

// Where each item of a cell starts, in beats into the section;
// `itemBeats(item)` gives an item's length in beats.
template <class ItemBeats>
std::vector<double> songItemStarts(const std::vector<SongItem>& items, int beatsPerBar, ItemBeats itemBeats) {
    std::vector<double> starts;
    starts.reserve(items.size());
    const double bar = beatsPerBar < 1 ? 1 : beatsPerBar;
    for (std::size_t i = 0; i < items.size(); ++i)
        starts.push_back(items[i].after && i > 0 ? starts[i - 1] + itemBeats(items[i - 1]) : items[i].bar * bar);
    return starts;
}

// How many bars a section lasts: its own, or with `fit` as long as its
// longest channel (whole bars, at least past each item's first bar);
// `itemBeats(channel, item)` gives an item's length in beats.
template <class ItemBeats>
int sectionBarsOf(const SongSection& section, int beatsPerBar, ItemBeats itemBeats) {
    if (!section.fit)
        return section.bars < 1 ? 1 : section.bars > kMaxSectionBars ? kMaxSectionBars : section.bars;
    const double bar = beatsPerBar < 1 ? 1 : beatsPerBar;
    double end = bar;
    for (int ch = 0; ch < kChannelCount; ++ch) {
        const auto& items = section.cells[static_cast<std::size_t>(ch)];
        const auto starts = songItemStarts(items, beatsPerBar, [&](const SongItem& it) { return itemBeats(ch, it); });
        for (std::size_t i = 0; i < items.size(); ++i) {
            const double start = starts[i], length = itemBeats(ch, items[i]);
            end = end > start + length ? end : start + length;
            end = end > start + 0.25 ? end : start + 0.25;
        }
    }
    const int bars = static_cast<int>(end / bar - 1e-6) + 1;
    return bars > kMaxSectionBars ? kMaxSectionBars : bars;
}

struct Song {
    std::vector<SongSection> sections;
    bool loop = false;  // after its last section, its first again
    bool empty() const { return sections.empty(); }
    friend bool operator==(const Song&, const Song&) = default;
};

}  // namespace midipad
