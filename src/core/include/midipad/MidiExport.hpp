#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "midipad/Session.hpp"

namespace midipad {

// A clip or the whole song, played by an engine of its own as fast as it
// goes (on its own clock, from beat 0), what it sends written down as a
// Standard MIDI File: notes, CCs and program changes, the tempo and the
// time signature. Chains, random steps and chances play as they would live,
// once; humanizing, mutes and solos are left out.
struct MidiExportOptions {
    double tempo = 120;
    int beatsPerBar = 4;
    std::string title;                           // the file's name inside it
    std::array<std::string, 16> channelNames{};  // a track name each (empty: "Channel n")
};

// The song from its first section to its end, a track per channel that plays
// (format 1); nothing when it has no section.
std::vector<std::uint8_t> exportSongMidi(const Session& session, const MidiExportOptions& options);
// One turn of clip `slot` of `channel` (at least a beat), on one track (format 0).
std::vector<std::uint8_t> exportClipMidi(const Session& session, const MidiExportOptions& options, int channel, int slot);

void writeFile(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes);  // throws on I/O error

}  // namespace midipad
