#pragma once

#include <string>

// Kernel MIDI ports, for the programs that see nothing else on Linux: the
// snd-virmidi module, loaded as administrator through pkexec, which asks for
// the password in a system dialog. Linux only; elsewhere supported() is false.
namespace virmidi {

inline constexpr int kMaxPorts = 4;  // per snd-virmidi card

struct Status {
    bool loaded = false;     // the module is loaded
    bool atStartup = false;  // MidiPad's files load it at every startup
};

bool supported();
Status status();

// The shell scripts run as administrator. `ports` is clamped to 1..kMaxPorts;
// nothing else goes into them.
std::string createScript(int ports, bool atStartup);
std::string removeScript();

// Loads the module with `ports` ports (reloading it if it has another count),
// and writes or deletes the files that load it at startup. Blocks until the
// password is given or refused: call it off the UI thread. Returns an error
// message, empty on success.
std::string create(int ports, bool atStartup);
// Unloads the module and deletes the startup files.
std::string remove();

}  // namespace virmidi
