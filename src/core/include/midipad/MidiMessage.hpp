#pragma once

#include <array>
#include <cstdint>

namespace midipad {

// A short MIDI message (channel voice or realtime). Fixed size so the
// real-time path never allocates.
struct MidiMessage {
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t size = 0;

    // Channels are 0-15 (MIDI channel 1 is 0).
    static constexpr MidiMessage noteOn(int channel, int note, int velocity) {
        return {{status(0x90, channel), data(note), data(velocity)}, 3};
    }
    static constexpr MidiMessage noteOff(int channel, int note) {
        return {{status(0x80, channel), data(note), 0}, 3};
    }
    static constexpr MidiMessage controlChange(int channel, int controller, int value) {
        return {{status(0xB0, channel), data(controller), data(value)}, 3};
    }
    static constexpr MidiMessage programChange(int channel, int program) {
        return {{status(0xC0, channel), data(program), 0}, 2};
    }
    static constexpr MidiMessage realtime(std::uint8_t statusByte) { return {{statusByte, 0, 0}, 1}; }

    friend constexpr bool operator==(const MidiMessage&, const MidiMessage&) = default;

private:
    static constexpr std::uint8_t status(int kind, int channel) {
        return static_cast<std::uint8_t>(kind | (channel & 0x0F));
    }
    static constexpr std::uint8_t data(int value) {
        return static_cast<std::uint8_t>(value < 0 ? 0 : value > 127 ? 127 : value);
    }
};

}  // namespace midipad
