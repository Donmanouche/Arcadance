#include "VirMidi.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

#ifdef __linux__
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace virmidi {

namespace {

constexpr const char* kModuleDir = "/sys/module/snd_virmidi";
constexpr const char* kLoadFile = "/etc/modules-load.d/midipad-virmidi.conf";
constexpr const char* kOptionsFile = "/etc/modprobe.d/midipad-virmidi.conf";

// Exit codes of the scripts; pkexec itself uses 126 (dialog dismissed) and
// 127 (not authorized).
constexpr int kInUse = 3, kLoadFailed = 4, kWriteFailed = 5;

std::string unload() {
    return std::string("if [ -d ") + kModuleDir + " ]; then modprobe -r snd_virmidi || exit " +
           std::to_string(kInUse) + "; fi\n";
}

std::string runAsAdmin([[maybe_unused]] const std::string& script) {
#ifdef __linux__
    const char* argv[] = {"pkexec", "/bin/sh", "-c", script.c_str(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, "pkexec", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0)
        return "pkexec is not available: load snd-virmidi by hand (see the README).";
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
        return "The administrator command did not finish.";
    switch (WEXITSTATUS(status)) {
    case 0: return {};
    case 126:
    case 127: return "Not authorized: the administrator password was refused or the dialog closed.";
    case kInUse: return "The VirMIDI ports are in use: close the programs using them and try again.";
    case kLoadFailed: return "Loading the snd-virmidi module failed.";
    case kWriteFailed: return "Writing the startup files in /etc failed.";
    default: return "The administrator command failed (code " + std::to_string(WEXITSTATUS(status)) + ").";
    }
#else
    return "VirMIDI ports only exist on Linux.";
#endif
}

}  // namespace

bool supported() {
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

Status status() {
    std::error_code ec;
    Status s;
    s.loaded = std::filesystem::is_directory(kModuleDir, ec);
    s.atStartup = std::filesystem::exists(kLoadFile, ec);
    return s;
}

std::string createScript(int ports, bool atStartup) {
    const std::string n = std::to_string(std::clamp(ports, 1, kMaxPorts));
    std::string script = std::string("if [ -d ") + kModuleDir + " ] && [ \"$(cut -d, -f1 " + kModuleDir +
                         "/parameters/midi_devs)\" != " + n + " ]; then modprobe -r snd_virmidi || exit " +
                         std::to_string(kInUse) + "; fi\n";
    script += "modprobe snd_virmidi midi_devs=" + n + " || exit " + std::to_string(kLoadFailed) + "\n";
    if (atStartup) {
        const std::string fail = " || exit " + std::to_string(kWriteFailed) + "\n";
        script += std::string("printf 'snd-virmidi\\n' > ") + kLoadFile + fail;
        script += "printf 'options snd-virmidi midi_devs=" + n + "\\n' > " + kOptionsFile + fail;
    } else {
        script += std::string("rm -f ") + kLoadFile + " " + kOptionsFile + "\n";
    }
    return script;
}

std::string removeScript() { return unload() + "rm -f " + kLoadFile + " " + kOptionsFile + "\n"; }

std::string create(int ports, bool atStartup) { return runAsAdmin(createScript(ports, atStartup)); }

std::string remove() { return runAsAdmin(removeScript()); }

}  // namespace virmidi
