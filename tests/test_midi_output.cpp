#include "MidiOutput.hpp"

#include <doctest/doctest.h>

TEST_CASE("remembered port names drop the ALSA client:port numbers") {
    CHECK(MidiOutput::stablePortName("Virtual Raw MIDI 3-0:VirMIDI 3-0 28:0") == "Virtual Raw MIDI 3-0:VirMIDI 3-0");
    CHECK(MidiOutput::stablePortName("Scarlett 4i4 4th Gen:Scarlett 4i4 4th Gen MIDI 1 24:0") ==
          "Scarlett 4i4 4th Gen:Scarlett 4i4 4th Gen MIDI 1");
    CHECK(MidiOutput::stablePortName("loopMIDI Port") == "loopMIDI Port");
}
