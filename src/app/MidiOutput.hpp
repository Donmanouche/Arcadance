#pragma once

#include "midipad/Engine.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class RtMidiOut;

// MIDI output through RtMidi: either a virtual port that DAWs connect to
// (Linux, macOS) or an existing port (required on Windows, e.g. loopMIDI).
class MidiOutput final : public midipad::MidiSink {
public:
    enum class Api { Default, Alsa, Jack };

    explicit MidiOutput(Api api);  // throws std::runtime_error
    ~MidiOutput() override;

    std::string apiName() const;
    std::vector<std::string> portNames();
    bool supportsVirtualPorts() const;

    void openVirtual(const std::string& name);  // throws std::runtime_error
    // Opens the first port whose name contains `fragment` (case-insensitive)
    // and returns its full name. If none does, accepts the same port under
    // another ALSA card number (see stablePortName). Throws std::runtime_error.
    std::string connect(const std::string& fragment);

    // A port name without the ALSA "client:port" numbers that change from one
    // session to the next: "VirMIDI 3-0:VirMIDI 3-0 28:0" -> "VirMIDI 3-0:VirMIDI 3-0".
    static std::string stablePortName(const std::string& name);

    // Called from the real-time thread.
    void send(const midipad::MidiMessage& message) override;
    std::uint64_t sendErrors() const { return sendErrors_.load(std::memory_order_relaxed); }

private:
    void watchSendErrors();

    std::unique_ptr<RtMidiOut> out_;
    std::atomic<std::uint64_t> sendErrors_{0};
};
