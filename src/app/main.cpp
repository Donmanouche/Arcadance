// midipad: plays a preset from a gamepad, without a user interface.
//
// Threads:
//  - the input thread polls the gamepad at --poll-hz, runs the engine and
//    sends MIDI; it never blocks on the main thread and never allocates while
//    playing;
//  - the main thread reloads the preset when its file changes and prints
//    what the input thread reports through a lock-free queue.

#include "GamepadInput.hpp"
#include "MidiOutput.hpp"

#include "midipad/Engine.hpp"
#include "midipad/Notes.hpp"
#include "midipad/PresetIO.hpp"
#include "midipad/SpscRing.hpp"

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

#ifdef __linux__
#include <pthread.h>
#endif

namespace fs = std::filesystem;
using namespace midipad;

namespace {

constexpr const char* kUsage = R"(Usage : midipad [options] <preset.json>

Joue un preset MidiPad avec la première manette connectée.
Le preset est rechargé automatiquement quand son fichier change.

Options :
  -c, --connect NOM    envoie vers un port MIDI existant dont le nom contient NOM,
                       au lieu de créer un port virtuel
  -n, --port-name NOM  nom du port virtuel (défaut : MidiPad)
      --api alsa|jack  API MIDI sous Linux (défaut : alsa)
      --channel N      canal MIDI de départ, de 1 à 16 (défaut : 1)
  -m, --monitor        affiche chaque message MIDI envoyé
  -i, --input          affiche les boutons appuyés et les changements de freeze
      --stats          affiche à la sortie les temps de traitement mesurés
      --poll-hz N      lectures de la manette par seconde (défaut : 1000)
      --list-ports     liste les ports MIDI de sortie et quitte
  -h, --help           affiche cette aide

Les presets de la version Python se convertissent avec midipad-import.
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
std::variant<Options, int> parseArgs(int argc, char** argv) {
    Options o;
    auto fail = [](const std::string& message) {
        std::cerr << "midipad : " << message << "\n\n" << kUsage;
        return 2;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc)
                return std::nullopt;
            return std::string(argv[++i]);
        };
        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            return 0;
        } else if (arg == "-c" || arg == "--connect") {
            o.connect = value();
            if (!o.connect)
                return fail(arg + " attend un nom de port");
        } else if (arg == "-n" || arg == "--port-name") {
            auto v = value();
            if (!v || v->empty())
                return fail(arg + " attend un nom");
            o.portName = *v;
        } else if (arg == "--api") {
            auto v = value();
            if (v == "alsa")
                o.api = MidiOutput::Api::Alsa;
            else if (v == "jack")
                o.api = MidiOutput::Api::Jack;
            else
                return fail("--api attend alsa ou jack");
        } else if (arg == "--channel") {
            auto v = value();
            auto n = v ? parseInt(*v) : std::nullopt;
            if (!n || *n < 1 || *n > 16)
                return fail("--channel attend un nombre de 1 à 16");
            o.channel = *n - 1;
        } else if (arg == "--poll-hz") {
            auto v = value();
            auto n = v ? parseInt(*v) : std::nullopt;
            if (!n || *n < 50 || *n > 8000)
                return fail("--poll-hz attend un nombre de 50 à 8000");
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
            return fail("option inconnue " + arg);
        } else if (o.preset.empty()) {
            o.preset = arg;
        } else {
            return fail("un seul preset à la fois");
        }
    }
    if (o.preset.empty() && !o.listPorts)
        return fail("indiquez un fichier de preset");
    return o;
}

std::atomic<bool> gQuit{false};
extern "C" void onSignal(int) { gQuit.store(true); }

std::mutex gPrintMutex;
void say(const std::string& line) {
    std::lock_guard lock(gPrintMutex);
    std::cout << line << std::endl;
}

// What the input thread reports to the main thread.
struct Report {
    enum class Kind : std::uint8_t { Midi, Channel, Button, Freeze } kind;
    MidiMessage message{};
    int channel = 0;
    Button button{};
    bool pressed = false;
    std::uint8_t frozenAxes = 0;
};

struct Shared {
    SpscRing<Report, 4096> reports;
    std::atomic<std::uint64_t> droppedReports{0};

    std::mutex presetMutex;
    std::shared_ptr<const Preset> pendingPreset;
    std::atomic<bool> hasPendingPreset{false};

    std::atomic<std::uint64_t> ticks{0}, totalWorkNs{0}, maxWorkNs{0}, maxLateNs{0};
};

void storeMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    if (value > target.load(std::memory_order_relaxed))
        target.store(value, std::memory_order_relaxed);
}

// Sends to the MIDI port and, when monitoring, copies to the report queue.
class OutputTap final : public MidiSink, public EngineObserver {
public:
    OutputTap(MidiOutput& out, Shared& shared, bool monitor) : out_(out), shared_(shared), monitor_(monitor) {}

    void send(const MidiMessage& m) override {
        out_.send(m);
        if (monitor_)
            report({Report::Kind::Midi, m, 0});
    }
    void channelChanged(int channel) override { report({Report::Kind::Channel, {}, channel}); }
    void freezeChanged(int channel, std::uint8_t frozenAxes) override {
        Report r{Report::Kind::Freeze};
        r.channel = channel;
        r.frozenAxes = frozenAxes;
        report(r);
    }
    void buttonChanged(Button button, bool pressed) {
        Report r{Report::Kind::Button};
        r.button = button;
        r.pressed = pressed;
        report(r);
    }

private:
    void report(const Report& r) {
        if (!shared_.reports.push(r))
            shared_.droppedReports.fetch_add(1, std::memory_order_relaxed);
    }

    MidiOutput& out_;
    Shared& shared_;
    bool monitor_;
};

void tryRealtimePriority() {
#ifdef __linux__
    sched_param param{};
    param.sched_priority = 10;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0)
        say("Thread manette en priorité temps réel.");
    else
        say("Priorité temps réel refusée (normal sans droits rtprio) : priorité normale.");
#endif
}

void inputThread(std::stop_token stop, const Options& options, MidiOutput& out, Shared& shared,
                 std::shared_ptr<const Preset> initial) {
    tryRealtimePriority();

    OutputTap tap(out, shared, options.monitor);
    Engine engine(tap);
    engine.setPreset(std::move(initial));
    engine.setChannel(options.channel);
    engine.setObserver(&tap);
    GamepadInput gamepad(engine, say);
    if (options.showInput)
        gamepad.setButtonTap([&tap](Button b, bool pressed) { tap.buttonChanged(b, pressed); });

    const std::uint64_t period = 1'000'000'000ull / static_cast<std::uint64_t>(options.pollHz);
    std::uint64_t next = SDL_GetTicksNS();
    while (!stop.stop_requested()) {
        const std::uint64_t start = SDL_GetTicksNS();
        storeMax(shared.maxLateNs, start > next ? start - next : 0);

        if (shared.hasPendingPreset.exchange(false, std::memory_order_acquire)) {
            std::lock_guard lock(shared.presetMutex);
            engine.setPreset(std::move(shared.pendingPreset));
        }
        gamepad.poll(start);

        const std::uint64_t end = SDL_GetTicksNS();
        shared.ticks.fetch_add(1, std::memory_order_relaxed);
        shared.totalWorkNs.fetch_add(end - start, std::memory_order_relaxed);
        storeMax(shared.maxWorkNs, end - start);

        next += period;
        if (next <= end)
            next = end + period;  // fell behind: skip ahead instead of bursting
        SDL_DelayNS(next - end);
    }
    engine.releaseAll();
}

std::string describe(const MidiMessage& m) {
    char line[96];
    const int channel = (m.bytes[0] & 0x0F) + 1;
    switch (m.bytes[0] & 0xF0) {
    case 0x90:
        std::snprintf(line, sizeof line, "canal %2d  note on   %-4s (%3d)  vél. %d", channel, noteName(m.bytes[1]).c_str(),
                      m.bytes[1], m.bytes[2]);
        break;
    case 0x80:
        std::snprintf(line, sizeof line, "canal %2d  note off  %-4s (%3d)", channel, noteName(m.bytes[1]).c_str(),
                      m.bytes[1]);
        break;
    case 0xB0:
        std::snprintf(line, sizeof line, "canal %2d  CC %3d = %d", channel, m.bytes[1], m.bytes[2]);
        break;
    default:
        std::snprintf(line, sizeof line, "%s", m.bytes[0] == 0xFA   ? "transport : start"
                                               : m.bytes[0] == 0xFB ? "transport : continue"
                                               : m.bytes[0] == 0xFC ? "transport : stop"
                                                                    : "message MIDI");
    }
    return line;
}

// PlayStation names, falling back to the preset identifiers.
std::string buttonLabel(Button b) {
    switch (b) {
    case Button::South: return "Croix";
    case Button::East: return "Rond";
    case Button::West: return "Carré";
    case Button::North: return "Triangle";
    case Button::Back: return "Create";
    case Button::Guide: return "PS";
    case Button::Start: return "Options";
    case Button::LeftStick: return "L3";
    case Button::RightStick: return "R3";
    case Button::LeftShoulder: return "L1";
    case Button::RightShoulder: return "R1";
    case Button::Misc1: return "Micro";
    case Button::Touchpad: return "Clic touchpad";
    default: return std::string(toString(b));
    }
}

std::string describe(const Report& r) {
    switch (r.kind) {
    case Report::Kind::Midi: return describe(r.message);
    case Report::Kind::Channel: return "-> canal " + std::to_string(r.channel + 1);
    case Report::Kind::Button: return "[" + buttonLabel(r.button) + (r.pressed ? " appuyé]" : " relâché]");
    case Report::Kind::Freeze: {
        std::string axes;
        for (int a = 0; a < kAxisCount; ++a)
            if (r.frozenAxes & (1u << a))
                axes += (axes.empty() ? "" : ", ") + std::string(toString(static_cast<Axis>(a)));
        return "[freeze canal " + std::to_string(r.channel + 1) + " : " + (axes.empty() ? "aucun axe" : axes) + "]";
    }
    }
    return {};
}

std::optional<fs::file_time_type> modificationTime(const fs::path& file) {
    std::error_code ec;
    auto t = fs::last_write_time(file, ec);
    return ec ? std::nullopt : std::optional(t);
}

int run(const Options& options) {
    std::unique_ptr<MidiOutput> out;
    try {
        out = std::make_unique<MidiOutput>(options.api);
    } catch (const std::exception& e) {
        std::cerr << "midipad : " << e.what() << '\n';
        return 1;
    }

    if (options.listPorts) {
        const auto ports = out->portNames();
        std::cout << "Ports MIDI de sortie (" << out->apiName() << ") :\n";
        if (ports.empty())
            std::cout << "  aucun\n";
        for (const auto& name : ports)
            std::cout << "  " << name << '\n';
        return 0;
    }

    std::shared_ptr<const Preset> preset;
    try {
        preset = std::make_shared<const Preset>(loadPreset(options.preset));
    } catch (const PresetError& e) {
        std::cerr << "midipad : " << e.what() << '\n';
        return 1;
    }

    try {
        if (options.connect)
            say("Sortie MIDI : " + out->connect(*options.connect) + " (" + out->apiName() + ")");
        else {
            out->openVirtual(options.portName);
            say("Sortie MIDI : port virtuel « " + options.portName + " » (" + out->apiName() + ")");
        }
    } catch (const std::exception& e) {
        std::cerr << "midipad : " << e.what() << '\n';
        return 1;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::cerr << "midipad : SDL_Init a échoué : " << SDL_GetError() << '\n';
        return 1;
    }
    // GamepadInput polls the state itself, from its own thread.
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);

    say("Preset : " + (preset->name.empty() ? options.preset.stem().string() : preset->name) + ", canal " +
        std::to_string(options.channel + 1) + ". En attente d'une manette… (Ctrl+C pour quitter)");

    Shared shared;
    auto lastModified = modificationTime(options.preset);
    {
        std::jthread input(inputThread, std::cref(options), std::ref(*out), std::ref(shared), preset);

        auto nextPresetCheck = std::chrono::steady_clock::now();
        while (!gQuit.load()) {
            while (auto r = shared.reports.pop())
                say(describe(*r));

            if (std::chrono::steady_clock::now() >= nextPresetCheck) {
                nextPresetCheck += std::chrono::milliseconds(300);
                auto modified = modificationTime(options.preset);
                if (modified && modified != lastModified) {
                    lastModified = modified;
                    try {
                        auto reloaded = std::make_shared<const Preset>(loadPreset(options.preset));
                        {
                            std::lock_guard lock(shared.presetMutex);
                            shared.pendingPreset = std::move(reloaded);
                        }
                        shared.hasPendingPreset.store(true, std::memory_order_release);
                        say("Preset rechargé.");
                    } catch (const PresetError& e) {
                        say(std::string("Preset non rechargé, l'ancien reste actif : ") + e.what());
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        say("Arrêt : toutes les notes sont coupées.");
    }  // joins the input thread, which releases every sounding note first

    if (options.stats) {
        const auto ticks = std::max<std::uint64_t>(shared.ticks.load(), 1);
        std::printf("Lectures de la manette : %llu\n", static_cast<unsigned long long>(shared.ticks.load()));
        std::printf("Traitement par lecture : moyenne %.1f µs, max %.1f µs\n",
                    static_cast<double>(shared.totalWorkNs.load()) / static_cast<double>(ticks) / 1000.0,
                    static_cast<double>(shared.maxWorkNs.load()) / 1000.0);
        std::printf("Retard de réveil max : %.1f µs\n", static_cast<double>(shared.maxLateNs.load()) / 1000.0);
    }
    if (auto dropped = shared.droppedReports.load())
        std::printf("Messages non affichés (moniteur saturé) : %llu\n", static_cast<unsigned long long>(dropped));
    if (auto errors = out->sendErrors())
        std::printf("Erreurs d'envoi MIDI : %llu\n", static_cast<unsigned long long>(errors));

    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    auto parsed = parseArgs(argc, argv);
    if (auto* code = std::get_if<int>(&parsed))
        return *code;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    return run(std::get<Options>(parsed));
}
