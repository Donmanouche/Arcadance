#pragma once

#include "midipad/Preset.hpp"

#include <cstdint>
#include <string>

namespace midipad {

// The family of the controller connected, whose names the buttons take:
// PlayStation (the reference: Cross, Circle...), Xbox (A at the bottom),
// Nintendo (A on the right, B at the bottom), or generic (by place: Bottom,
// Right...). Positions never change, only names: South is South everywhere.
enum class ControllerFamily : std::uint8_t { PlayStation, Xbox, Nintendo, Generic };
void setLabelFamily(ControllerFamily family);
ControllerFamily labelFamily();

// User-facing names, in the family set above.
std::string label(Button b);
std::string label(Axis a);
std::string label(HatDir d);
std::string label(Zone z);

// One-line summaries for tables: "Note C3 (48), toggle", "CC 40 (0-127), inverted".
// `flats` spells notes with flats (Bb rather than A#).
std::string describe(const TriggerBinding& b, bool flats = false);
std::string describe(const AxisBinding& a);

}  // namespace midipad
