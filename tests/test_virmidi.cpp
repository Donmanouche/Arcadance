#include "VirMidi.hpp"

#include <doctest/doctest.h>

TEST_CASE("the VirMIDI script loads the requested number of ports, at most 4") {
    CHECK(virmidi::createScript(2, false).find("modprobe snd_virmidi midi_devs=2 ") != std::string::npos);
    CHECK(virmidi::createScript(9, false).find("midi_devs=4 ") != std::string::npos);
    CHECK(virmidi::createScript(0, false).find("midi_devs=1 ") != std::string::npos);
}

TEST_CASE("the VirMIDI script writes the startup files only when asked") {
    const std::string persistent = virmidi::createScript(4, true);
    CHECK(persistent.find("> /etc/modules-load.d/midipad-virmidi.conf") != std::string::npos);
    CHECK(persistent.find("options snd-virmidi midi_devs=4") != std::string::npos);

    const std::string once = virmidi::createScript(4, false);
    CHECK(once.find("rm -f /etc/modules-load.d/midipad-virmidi.conf /etc/modprobe.d/midipad-virmidi.conf") !=
          std::string::npos);
    CHECK(once.find("printf") == std::string::npos);
}

TEST_CASE("removing the VirMIDI ports unloads the module and the startup files") {
    const std::string script = virmidi::removeScript();
    CHECK(script.find("modprobe -r snd_virmidi") != std::string::npos);
    CHECK(script.find("rm -f /etc/modules-load.d/midipad-virmidi.conf") != std::string::npos);
}
