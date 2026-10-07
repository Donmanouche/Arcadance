#include "MidiOutput.hpp"

#include <RtMidi.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

namespace {

constexpr const char* kClientName = "MidiPad";

RtMidi::Api toRtMidi(MidiOutput::Api api) {
    switch (api) {
    case MidiOutput::Api::Alsa: return RtMidi::LINUX_ALSA;
    case MidiOutput::Api::Jack: return RtMidi::UNIX_JACK;
    case MidiOutput::Api::Default: break;
    }
#ifdef __linux__
    // ALSA sequencer ports are visible to every DAW, including through PipeWire.
    return RtMidi::LINUX_ALSA;
#else
    return RtMidi::UNSPECIFIED;
#endif
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

MidiOutput::MidiOutput(Api api) {
    try {
        out_ = std::make_unique<RtMidiOut>(toRtMidi(api), kClientName);
    } catch (const RtMidiError& e) {
        throw std::runtime_error("MIDI indisponible : " + e.getMessage());
    }
}

MidiOutput::~MidiOutput() = default;

std::string MidiOutput::apiName() const { return RtMidi::getApiDisplayName(out_->getCurrentApi()); }

std::vector<std::string> MidiOutput::portNames() {
    std::vector<std::string> names;
    const unsigned count = out_->getPortCount();
    for (unsigned i = 0; i < count; ++i)
        names.push_back(out_->getPortName(i));
    return names;
}

bool MidiOutput::supportsVirtualPorts() const { return out_->getCurrentApi() != RtMidi::WINDOWS_MM; }

void MidiOutput::openVirtual(const std::string& name) {
    if (!supportsVirtualPorts())
        throw std::runtime_error("cette plateforme ne permet pas de port virtuel : utilisez --connect "
                                 "(par exemple avec loopMIDI)");
    try {
        out_->openVirtualPort(name);
    } catch (const RtMidiError& e) {
        throw std::runtime_error("impossible de créer le port virtuel « " + name + " » : " + e.getMessage());
    }
    watchSendErrors();
}

std::string MidiOutput::stablePortName(const std::string& name) {
    static const std::regex kClientPort(R"( [0-9]+:[0-9]+$)");
    return std::regex_replace(name, kClientPort, "");
}

namespace {

// Card numbers move with the plugging order ("Virtual Raw MIDI 3-0" becomes
// "4-0" when another card comes first); the device number after the dash stays.
std::string withoutCardNumbers(const std::string& name) {
    static const std::regex kCardDevice(R"([0-9]+-([0-9]+))");
    return std::regex_replace(lower(MidiOutput::stablePortName(name)), kCardDevice, "#-$1");
}

}  // namespace

std::string MidiOutput::connect(const std::string& fragment) {
    const auto names = portNames();
    const std::string wanted = lower(fragment);
    auto matches = [&](const std::string& name) { return lower(name).find(wanted) != std::string::npos; };
    const bool any = std::any_of(names.begin(), names.end(), matches);
    for (unsigned i = 0; i < names.size(); ++i) {
        if (any ? !matches(names[i]) : withoutCardNumbers(names[i]) != withoutCardNumbers(fragment))
            continue;
        try {
            out_->openPort(i, kClientName);
        } catch (const RtMidiError& e) {
            throw std::runtime_error("impossible d'ouvrir « " + names[i] + " » : " + e.getMessage());
        }
        watchSendErrors();
        return names[i];
    }
    throw std::runtime_error("aucun port MIDI ne contient « " + fragment + " » (voir --list-ports)");
}

void MidiOutput::send(const midipad::MidiMessage& message) { out_->sendMessage(message.bytes.data(), message.size); }

void MidiOutput::watchSendErrors() {
    // Once a port is open, report failures as a counter instead of printing or
    // throwing from the real-time thread.
    out_->setErrorCallback(
        [](RtMidiError::Type, const std::string&, void* self) {
            static_cast<MidiOutput*>(self)->sendErrors_.fetch_add(1, std::memory_order_relaxed);
        },
        this);
}
