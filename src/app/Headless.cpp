// midipad --headless: plays a preset from a gamepad, without a window.
// The preset is reloaded when its file changes.

#include "Headless.hpp"

#include "MidiOutput.hpp"
#include "Runtime.hpp"

#include "midipad/PresetIO.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace fs = std::filesystem;
using namespace midipad;

namespace {

constexpr const char* kUsage = R"(Usage: midipad --headless [options] <preset.json>

Plays a MidiPad preset with the first gamepad connected, without a window.
The preset is reloaded automatically when its file changes.

Options:
  -c, --connect NAME   send to an existing MIDI port whose name contains NAME,
                       instead of creating a virtual port
  -n, --port-name NAME name of the virtual port (default: MidiPad)
      --api alsa|jack  MIDI API on Linux (default: alsa)
      --channel N      starting MIDI channel, 1 to 16 (default: 1)
  -m, --monitor        print every MIDI message sent
  -i, --input          print the buttons pressed
      --stats          print the measured processing times on exit
      --poll-hz N      gamepad reads per second (default: 1000)
      --list-ports     list the MIDI output ports and exit
  -h, --help           show this help
)";

struct Options {
    fs::path preset;
    std::optional<std::string> connect;
    std::string portName = "MidiPad";
    MidiOutput::Api api = MidiOutput::Api::Default;
    int channel = 0;
    bool monitor = false;
    bool showInput = false;
    bool stats = false;
    int pollHz = 1000;
    bool listPorts = false;
};

std::optional<int> parseInt(const std::string& s) {
    try {
        std::size_t used = 0;
        int v = std::stoi(s, &used);
        if (used == s.size())
            return v;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

// Returns the options, or an exit code when the program should stop.
std::variant<Options, int> parseArgs(const std::vector<std::string>& args) {
    Options o;
    auto fail = [](const std::string& message) {
        std::cerr << "midipad: " << message << "\n\n" << kUsage;
        return 2;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size())
                return std::nullopt;
            return args[++i];
        };
        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            return 0;
        } else if (arg == "-c" || arg == "--connect") {
            o.connect = value();
            if (!o.connect)
                return fail(arg + " expects a port name");
        } else if (arg == "-n" || arg == "--port-name") {
            auto v = value();
            if (!v || v->empty())
                return fail(arg + " expects a name");
            o.portName = *v;
        } else if (arg == "--api") {
            auto v = value();
            if (v == "alsa")
                o.api = MidiOutput::Api::Alsa;
            else if (v == "jack")
                o.api = MidiOutput::Api::Jack;
            else
                return fail("--api expects alsa or jack");
        } else if (arg == "--channel") {
            auto v = value();
            auto n = v ? parseInt(*v) : std::nullopt;
            if (!n || *n < 1 || *n > 16)
                return fail("--channel expects a number from 1 to 16");
            o.channel = *n - 1;
        } else if (arg == "--poll-hz") {
            auto v = value();
            auto n = v ? parseInt(*v) : std::nullopt;
            if (!n || *n < 50 || *n > 8000)
                return fail("--poll-hz expects a number from 50 to 8000");
            o.pollHz = *n;
        } else if (arg == "-m" || arg == "--monitor") {
            o.monitor = true;
        } else if (arg == "-i" || arg == "--input") {
            o.showInput = true;
        } else if (arg == "--stats") {
            o.stats = true;
        } else if (arg == "--list-ports") {
            o.listPorts = true;
        } else if (!arg.empty() && arg[0] == '-') {
            return fail("unknown option " + arg);
        } else if (o.preset.empty()) {
            o.preset = arg;
        } else {
            return fail("one preset at a time");
        }
    }
    if (o.preset.empty() && !o.listPorts)
        return fail("give a preset file");
    return o;
}

std::atomic<bool> gQuit{false};
extern "C" void onSignal(int) { gQuit.store(true); }

std::mutex gPrintMutex;
void say(const std::string& line) {
    std::lock_guard lock(gPrintMutex);
    std::cout << line << std::endl;
}

std::optional<fs::file_time_type> modificationTime(const fs::path& file) {
    std::error_code ec;
    auto t = fs::last_write_time(file, ec);
    return ec ? std::nullopt : std::optional(t);
}

int run(const Options& options) {
    std::shared_ptr<MidiOutput> out;
    try {
        out = std::make_shared<MidiOutput>(options.api);
    } catch (const std::exception& e) {
        std::cerr << "midipad: " << e.what() << '\n';
        return 1;
    }

    if (options.listPorts) {
        const auto ports = out->portNames();
        std::cout << "MIDI output ports (" << out->apiName() << "):\n";
        if (ports.empty())
            std::cout << "  none\n";
        for (const auto& name : ports)
            std::cout << "  " << name << '\n';
        return 0;
    }

    std::shared_ptr<const Preset> preset;
    try {
        preset = std::make_shared<const Preset>(loadPreset(options.preset));
    } catch (const PresetError& e) {
        std::cerr << "midipad: " << e.what() << '\n';
        return 1;
    }

    try {
        if (options.connect)
            say("MIDI output: " + out->connect(*options.connect) + " (" + out->apiName() + ")");
        else {
            out->openVirtual(options.portName);
            say("MIDI output: virtual port \"" + options.portName + "\" (" + out->apiName() + ")");
        }
    } catch (const std::exception& e) {
        std::cerr << "midipad: " << e.what() << '\n';
        return 1;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::cerr << "midipad: SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }
    SDL_SetGamepadEventsEnabled(false);  // the runtime polls the gamepad itself
    SDL_SetJoystickEventsEnabled(false);

    say("Preset: " + (preset->name.empty() ? options.preset.stem().string() : preset->name) + ", channel " +
        std::to_string(options.channel + 1) + ". Waiting for a gamepad... (Ctrl+C to quit)");

    RuntimeStats stats;
    {
        Runtime runtime(out, preset, {options.pollHz, options.channel, options.monitor, options.showInput}, say);
        auto lastModified = modificationTime(options.preset);
        auto nextPresetCheck = std::chrono::steady_clock::now();
        while (!gQuit.load()) {
            while (auto r = runtime.popReport())
                say(describe(*r));

            if (std::chrono::steady_clock::now() >= nextPresetCheck) {
                nextPresetCheck += std::chrono::milliseconds(300);
                auto modified = modificationTime(options.preset);
                if (modified && modified != lastModified) {
                    lastModified = modified;
                    try {
                        runtime.setPreset(std::make_shared<const Preset>(loadPreset(options.preset)));
                        say("Preset reloaded.");
                    } catch (const PresetError& e) {
                        say(std::string("Preset not reloaded, the previous one stays active: ") + e.what());
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        say("Stopping: every note is turned off.");
        stats = runtime.stats();
    }  // stops the input thread, which turns every note off first

    if (options.stats) {
        std::printf("Gamepad reads: %llu\n", static_cast<unsigned long long>(stats.ticks));
        std::printf("Processing per read: average %.1f µs, max %.1f µs\n", stats.averageWorkUs, stats.maxWorkUs);
        std::printf("Max wake-up delay: %.1f µs\n", stats.maxLateUs);
    }
    if (stats.droppedReports)
        std::printf("Messages not shown (monitor overflow): %llu\n",
                    static_cast<unsigned long long>(stats.droppedReports));
    if (stats.sendErrors)
        std::printf("MIDI send errors: %llu\n", static_cast<unsigned long long>(stats.sendErrors));

    SDL_Quit();
    return 0;
}

}  // namespace

int runHeadless(const std::vector<std::string>& args) {
    auto parsed = parseArgs(args);
    if (auto* code = std::get_if<int>(&parsed))
        return *code;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    return run(std::get<Options>(parsed));
}
