#pragma once

#include <cstdint>

namespace midipad {

// Random variations of the notes and chords played on chosen channels. Each
// note of a chord gets its own velocity and delay.
struct Humanize {
    std::uint16_t channels = 0;  // bit per channel (0-15) where it applies
    int velocityMinPercent = 100, velocityMaxPercent = 100;  // of the mapping's velocity
    int delayMinMs = 0, delayMaxMs = 0;                       // before the note starts

    static constexpr int kMaxDelayMs = 500;
    bool appliesTo(int channel) const { return (channels >> channel) & 1u; }
    friend bool operator==(const Humanize&, const Humanize&) = default;
};

}  // namespace midipad
