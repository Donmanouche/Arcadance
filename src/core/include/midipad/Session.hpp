#pragma once

#include "midipad/Preset.hpp"
#include "midipad/Scales.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace midipad {

// One recorded loop of a session, as the engine plays it.
struct SessionLoop {
    int channel = 0;  // 0-15
    LoopSource source = LoopSource::LeftStick;
    int length = 0;  // samples, at Engine::kLoopRateHz
    int stretch = 0, amplitude = 100;  // movement loops
    double syncBeats = 0, anchorBeat = 0;  // synced to the beat clock: its beats and where it starts; 0: free
    // Movement loops: X and Y per sample, [0, 1] as 0-65534, 0xFFFF where the
    // finger was lifted.
    std::vector<std::uint16_t> x, y;
    // The note loop: its notes on and off, in loop order.
    struct Note {
        std::int32_t at = 0;
        std::uint8_t note = 0, velocity = 0;
        bool on = false;
        std::uint32_t press = 0;  // the press it came from: a chord's notes share it (0: unknown)
    };
    std::vector<Note> notes;
};

// What a session file keeps: the loops of every channel, the preset they play
// with (a copy, so the session opens as it was even if the preset changed
// since), the tonality, the active channel and the tempo. Synced loops keep
// their beats, not the clock they came from: a session saved with the
// internal clock opens with Ableton Link, and the other way round.
struct Session {
    Preset preset;
    std::optional<Key> key;
    int channel = 0;
    double tempo = 0;  // of the clock when saved, in beats a minute; 0: none
    std::vector<SessionLoop> loops;
};

class SessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr const char* kSessionExtension = ".midipad";

nlohmann::ordered_json sessionToJson(const Session& session);
Session sessionFromJson(const nlohmann::ordered_json& json);  // throws SessionError, PresetError
void saveSession(const Session& session, const std::filesystem::path& file);  // throws on I/O error
Session loadSession(const std::filesystem::path& file);

}  // namespace midipad
