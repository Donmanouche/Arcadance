#pragma once

#include "midipad/Preset.hpp"

#include <string>

namespace midipad {

// French, user-facing names. PlayStation names for the buttons, since the
// DualSense is the reference controller.
std::string label(Button b);
std::string label(Axis a);
std::string label(HatDir d);
std::string label(Zone z);

// One-line summaries for tables: "Note C3 (48), bascule", "CC 40 (0-127), inversé".
std::string describe(const TriggerBinding& b);
std::string describe(const AxisBinding& a);

}  // namespace midipad
