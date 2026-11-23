// The Arcadance window: preset management, MIDI output, mapping editor, log.
//
// The main thread only draws and edits; the Runtime's input thread plays.
// Every edit is pushed to the engine at once, so changes are heard live, and
// written to disk only on "Save".

#include "gui/GuiApp.hpp"

#include "MidiOutput.hpp"
#include "Runtime.hpp"
#include "VirMidi.hpp"
#include "gui/MappingEditor.hpp"
#include "gui/PerformanceView.hpp"

#include "midipad/AppSettings.hpp"
#include "midipad/Files.hpp"
#include "midipad/Labels.hpp"
#include "midipad/PresetIO.hpp"
#include "midipad/PresetLibrary.hpp"
#include "midipad/MidiExport.hpp"
#include "midipad/Scales.hpp"
#include "midipad/Session.hpp"
#include "midipad/TapTempo.hpp"
#include "midipad/Notes.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <fstream>
#include <optional>
#include <utility>

extern const unsigned char kRobotoMedium[];
extern const std::size_t kRobotoMedium_size;
extern const unsigned char kRobotoBold[];
extern const std::size_t kRobotoBold_size;
extern const unsigned char kRobotoCondensedBold[];
extern const std::size_t kRobotoCondensedBold_size;

namespace fs = std::filesystem;
using namespace midipad;

namespace {

constexpr float kFontSize = 16.0f;
// The view is laid out on a page of this size, scaled as one to fit the window.
constexpr float kPageW = 1080.0f, kPageH = 846.0f;
constexpr ImVec4 kHintColor{0.65f, 0.65f, 0.7f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImVec4 kWarningColor{1.0f, 0.78f, 0.35f, 1.0f};

fs::path prefPath() {
    char* path = SDL_GetPrefPath("Arcadance", "Arcadance");
    fs::path result = path ? fs::path(path) : fs::current_path();
    SDL_free(path);
    return result;
}

// --- From MidiPad to Arcadance. Remove from version 0.5 on: formerPrefPath,
// takeOverFormerFolder, renameFormerFiles, AppSettings::migratedFromMidipad
// (the readers keep accepting the former format names).

// Where MidiPad, its former name, kept its settings, presets and sessions:
// beside the new folder (SDL: <data>/<org>/<app>/).
fs::path formerPrefPath(const fs::path& pref) {
    const fs::path base = pref.lexically_normal().parent_path();  // without its trailing separator
    return base.parent_path().parent_path() / "MidiPad" / "MidiPad";
}

// The first time Arcadance starts, MidiPad's settings, presets and sessions
// are copied over (the old folder stays as it was).
void takeOverFormerFolder(const fs::path& pref) {
    const fs::path former = formerPrefPath(pref);
    std::error_code ec;
    if (fs::exists(pref / "settings.json", ec) || !fs::exists(former / "settings.json", ec))
        return;
    fs::copy(former, pref, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
}

// Once: the presets of the presets folder say "arcadance-preset", the
// sessions end in .arcadance and say "arcadance-session". A file that cannot
// be read stays as it is (the readers take the former names anyway).
void renameFormerFiles(const fs::path& presetsDir, const fs::path& sessionsDir) {
    auto rewrite = [](const fs::path& from, const fs::path& to, const char* oldFormat, const char* newFormat, int indent) {
        try {
            std::ifstream in(from);
            nlohmann::ordered_json j = nlohmann::ordered_json::parse(in);
            in.close();
            if (!j.is_object() || j.value("format", std::string()) != oldFormat)
                return;
            j["format"] = newFormat;
            writeFileAtomically(to, (indent > 0 ? j.dump(indent) : j.dump()) + '\n');
            if (from != to)
                fs::remove(from);
        } catch (const std::exception&) {
        }
    };
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(presetsDir, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            rewrite(entry.path(), entry.path(), "midipad-preset", "arcadance-preset", 4);
    for (const auto& entry : fs::directory_iterator(sessionsDir, ec))
        if (entry.is_regular_file() && entry.path().extension() == kFormerSessionExtension) {
            fs::path to = entry.path();
            to.replace_extension(kSessionExtension);
            if (!fs::exists(to, ec))
                rewrite(entry.path(), to, "midipad-session", "arcadance-session", 0);
        }
}

// "0.21" as numbers, for comparing versions; a leading "v" is skipped.
std::vector<int> versionNumbers(std::string_view text) {
    std::vector<int> out;
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
        text.remove_prefix(1);
    int n = 0;
    bool any = false;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            n = n * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            out.push_back(n);
            n = 0;
        } else {
            break;
        }
    }
    if (any)
        out.push_back(n);
    return out;
}

// Channel 1's Shift, Loop record and Session screen controls work on every
// channel: the mapping `m` shows them in their place.
Mapping withChannelOneControls(Mapping m, const Mapping& first) {
    auto keep = [](TriggerBinding& shown, const TriggerBinding& own) {
        if (own.type == ActionType::Shift || own.type == ActionType::LoopRecord || own.type == ActionType::SessionScreen)
            shown = own;
    };
    for (std::size_t i = 0; i < m.buttons.size(); ++i)
        keep(m.buttons[i], first.buttons[i]);
    for (std::size_t i = 0; i < m.dpad.size(); ++i)
        keep(m.dpad[i], first.dpad[i]);
    for (std::size_t f = 0; f < m.fingers.size(); ++f)
        if (first.fingers[f].mode == TouchMode::Zones)
            for (std::size_t z = 0; z < m.fingers[f].zones.size(); ++z)
                keep(m.fingers[f].zones[z], first.fingers[f].zones[z]);
    return m;
}

// The binding a control spec ("button:south", "dpad:up", "zone:1:top_left")
// names in `m`, if it names one.
const TriggerBinding* specBinding(const Mapping& m, const std::string& spec) {
    const std::vector<std::string> p = MappingEditor::splitSpec(spec);
    if (p.size() == 2 && p[0] == "button")
        if (auto b = parseButton(p[1]); b && !isDpad(*b))
            return &m.buttons[static_cast<std::size_t>(*b)];
    if (p.size() == 2 && p[0] == "dpad")
        if (auto d = parseHatDir(p[1]))
            return &m.dpad[static_cast<std::size_t>(*d)];
    if (p.size() == 3 && p[0] == "zone" && (p[1] == "1" || p[1] == "2"))
        if (auto z = parseZone(p[2]))
            return &m.fingers[static_cast<std::size_t>(p[1][0] - '1')].zones[static_cast<std::size_t>(*z)];
    return nullptr;
}

// Copies the control a spec names ("button:south", "axes:left_x,left_y", "finger:1"...) from one
// mapping into another: its bindings, its Shift layer's, and a whole stick's, the triggers' or a
// finger's loop settings. False when the spec names nothing to copy.
bool copyControl(const Mapping& from, Mapping& to, const std::string& spec) {
    const std::vector<std::string> p = MappingEditor::splitSpec(spec);
    if (p.size() == 2 && p[0] == "button")
        if (auto b = parseButton(p[1]); b && !isDpad(*b)) {
            const auto i = static_cast<std::size_t>(*b);
            to.buttons[i] = from.buttons[i];
            to.shift.buttons[i] = from.shift.buttons[i];
            return true;
        }
    if (p.size() == 2 && p[0] == "dpad")
        if (auto d = parseHatDir(p[1])) {
            const auto i = static_cast<std::size_t>(*d);
            to.dpad[i] = from.dpad[i];
            to.shift.dpad[i] = from.shift.dpad[i];
            return true;
        }
    if (p.size() == 3 && p[0] == "zone" && (p[1] == "1" || p[1] == "2"))
        if (auto z = parseZone(p[2])) {
            const auto f = static_cast<std::size_t>(p[1][0] - '1'), i = static_cast<std::size_t>(*z);
            to.fingers[f].zones[i] = from.fingers[f].zones[i];
            to.shift.zones[f][i] = from.shift.zones[f][i];
            return true;
        }
    if ((p.size() == 2 && p[0] == "axis") || (p.size() == 3 && p[0] == "axes")) {
        bool any = false;
        for (std::size_t i = 1; i < p.size(); ++i)
            if (const auto a = parseAxis(p[i])) {
                to.axes[static_cast<std::size_t>(*a)] = from.axes[static_cast<std::size_t>(*a)];
                any = true;
            }
        if (p.size() == 3)  // a whole stick, or both triggers (with their buttons): their loop too
            if (const auto a = parseAxis(p[1])) {
                const int source = *a == Axis::LeftX ? 0 : *a == Axis::RightX ? 1 : static_cast<int>(LoopSource::Triggers);
                to.loops[static_cast<std::size_t>(source)] = from.loops[static_cast<std::size_t>(source)];
                if (source == static_cast<int>(LoopSource::Triggers))
                    for (Button b : {Button::LeftTrigger, Button::RightTrigger}) {
                        to.buttons[static_cast<std::size_t>(b)] = from.buttons[static_cast<std::size_t>(b)];
                        to.shift.buttons[static_cast<std::size_t>(b)] = from.shift.buttons[static_cast<std::size_t>(b)];
                    }
            }
        return any;
    }
    auto finger = [&](int f) {
        to.fingers[static_cast<std::size_t>(f)] = from.fingers[static_cast<std::size_t>(f)];
        to.shift.zones[static_cast<std::size_t>(f)] = from.shift.zones[static_cast<std::size_t>(f)];
        to.loops[static_cast<std::size_t>(2 + f)] = from.loops[static_cast<std::size_t>(2 + f)];
    };
    if (p.size() == 2 && p[0] == "finger" && (p[1] == "1" || p[1] == "2")) {
        finger(p[1][0] - '1');
        return true;
    }
    if (p.size() == 1 && p[0] == "fingers") {
        finger(0);
        finger(1);
        return true;
    }
    if (p.size() == 1 && p[0] == "notes") {
        to.loops[static_cast<std::size_t>(LoopSource::Notes)] = from.loops[static_cast<std::size_t>(LoopSource::Notes)];
        return true;
    }
    if (p.size() == 2 && p[0] == "loop") {  // a movement loop's settings alone
        const int source = std::atoi(p[1].c_str());
        if (source < 0 || source >= static_cast<int>(LoopSource::Notes))
            return false;
        to.loops[static_cast<std::size_t>(source)] = from.loops[static_cast<std::size_t>(source)];
        return true;
    }
    if (p.size() == 1 && p[0] == "tilt") {
        to.tilt = from.tilt;
        to.loops[static_cast<std::size_t>(LoopSource::Tilt)] = from.loops[static_cast<std::size_t>(LoopSource::Tilt)];
        return true;
    }
    return false;
}

// The parts of a mapping "Copy mapping" copies, a bit each.
enum CopyPart : unsigned { kCopyButtons = 1, kCopyDpad = 2, kCopyLeftStick = 4, kCopyRightStick = 8, kCopyTriggers = 16, kCopyTouchpad = 32, kCopyLoops = 64, kCopyTilt = 128 };
constexpr std::pair<CopyPart, const char*> kCopyParts[] = {{kCopyButtons, "Buttons"},     {kCopyDpad, "D-pad"},       {kCopyLeftStick, "Left stick"},
                                                           {kCopyRightStick, "Right stick"}, {kCopyTriggers, "Triggers"}, {kCopyTouchpad, "Touchpad"},
                                                           {kCopyTilt, "Tilt"},             {kCopyLoops, "Loop settings"}};

void copyParts(const Mapping& from, Mapping& to, unsigned parts) {
    if (parts & kCopyButtons)
        for (int b = 0; b < kButtonCount; ++b)
            if (!isDpad(static_cast<Button>(b)) && static_cast<Button>(b) != Button::LeftTrigger && static_cast<Button>(b) != Button::RightTrigger) {
                to.buttons[static_cast<std::size_t>(b)] = from.buttons[static_cast<std::size_t>(b)];
                to.shift.buttons[static_cast<std::size_t>(b)] = from.shift.buttons[static_cast<std::size_t>(b)];
            }
    if (parts & kCopyDpad) {
        to.dpad = from.dpad;
        to.shift.dpad = from.shift.dpad;
    }
    for (auto [part, a, b] : {std::tuple{kCopyLeftStick, Axis::LeftX, Axis::LeftY}, std::tuple{kCopyRightStick, Axis::RightX, Axis::RightY},
                              std::tuple{kCopyTriggers, Axis::LeftTrigger, Axis::RightTrigger}})
        if (parts & part) {
            to.axes[static_cast<std::size_t>(a)] = from.axes[static_cast<std::size_t>(a)];
            to.axes[static_cast<std::size_t>(b)] = from.axes[static_cast<std::size_t>(b)];
        }
    if (parts & kCopyTriggers)
        for (Button b : {Button::LeftTrigger, Button::RightTrigger}) {
            to.buttons[static_cast<std::size_t>(b)] = from.buttons[static_cast<std::size_t>(b)];
            to.shift.buttons[static_cast<std::size_t>(b)] = from.shift.buttons[static_cast<std::size_t>(b)];
        }
    if (parts & kCopyTouchpad) {
        to.fingers = from.fingers;
        to.shift.zones = from.shift.zones;
    }
    if (parts & kCopyTilt)
        to.tilt = from.tilt;
    if (parts & kCopyLoops)
        to.loops = from.loops;
}

constexpr const char* kReleasesUrl = "https://github.com/Donmanouche/Arcadance/releases/latest";

// Asks GitHub for the latest release (through curl, so Arcadance links no
// network library): what to tell the user.
struct UpdateCheck {
    bool ok = false;
    bool newer = false;
    std::string message;
};

UpdateCheck checkForUpdates() {
    UpdateCheck result;
    std::string reply;
    if (FILE* pipe = popen("curl -fsSL --max-time 10 -H 'Accept: application/vnd.github+json' "
                           "https://api.github.com/repos/Donmanouche/Arcadance/releases/latest 2>/dev/null",
                           "r")) {
        char buffer[4096];
        std::size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0)
            reply.append(buffer, n);
        pclose(pipe);
    }
    std::string tag;
    try {
        const auto json = nlohmann::json::parse(reply);
        if (json.contains("tag_name") && json["tag_name"].is_string())
            tag = json["tag_name"].get<std::string>();
    } catch (const std::exception&) {
    }
    if (tag.empty()) {
        result.message = "Could not reach GitHub.";
        return result;
    }
    result.ok = true;
    const std::string latest = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    result.newer = versionNumbers(latest) > versionNumbers(MIDIPAD_VERSION);
    result.message = result.newer ? "Arcadance " + latest + " is available."
                                  : "You have the latest version.";
    return result;
}

std::optional<fs::file_time_type> modificationTime(const fs::path& file) {
    std::error_code ec;
    auto t = fs::last_write_time(file, ec);
    return ec ? std::nullopt : std::optional(t);
}

struct GuiOptions {
    std::optional<fs::path> preset;
    std::optional<fs::path> screenshot;  // render a few frames, save a BMP, quit
    std::vector<std::pair<float, float>> clicks;  // simulated mouse clicks, for screenshots
    bool demo = false;                             // fake controller state, for screenshots
    std::optional<fs::path> record;  // save `frames` frames as BMPs in this folder, at `fps`, then quit
    int frames = 125, fps = 25;
    bool help = false;
    std::string error;
};

GuiOptions parseGuiArgs(const std::vector<std::string>& args) {
    GuiOptions o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help") {
            o.help = true;
        } else if (a == "--screenshot" && i + 1 < args.size()) {
            o.screenshot = args[++i];
        } else if (a == "--click" && i + 1 < args.size()) {
            float x = 0, y = 0;
            if (std::sscanf(args[++i].c_str(), "%f,%f", &x, &y) == 2)
                o.clicks.emplace_back(x, y);
            else
                o.error = "--click expects X,Y";
        } else if (a == "--demo") {
            o.demo = true;
        } else if (a == "--record" && i + 1 < args.size()) {
            o.record = args[++i];
        } else if ((a == "--frames" || a == "--fps") && i + 1 < args.size()) {
            const int n = std::atoi(args[++i].c_str());
            if (n <= 0)
                o.error = a + " expects a positive number";
            else
                (a == "--frames" ? o.frames : o.fps) = n;
        } else if (!a.empty() && a[0] == '-') {
            o.error = "unknown option " + a;
        } else {
            o.preset = a;
        }
    }
    return o;
}

// The kernel device name of a snd-virmidi port, which programs reading raw MIDI
// devices show, as "<device name>/<subdevice>": "VirMIDI 3-0" reads
// "Virtual Raw MIDI/..." and "VirMIDI 3-1" "Virtual Raw MIDI 2/...". Empty for
// other ports.
std::string rawMidiName(const std::string& port) {
    const auto at = port.find(":VirMIDI ");
    if (at == std::string::npos)
        return {};
    const auto dash = port.find('-', at);
    if (dash == std::string::npos || dash + 1 >= port.size() || !std::isdigit(static_cast<unsigned char>(port[dash + 1])))
        return {};
    const int device = port[dash + 1] - '0';
    return device == 0 ? "Virtual Raw MIDI" : "Virtual Raw MIDI " + std::to_string(device + 1);
}

// A moving controller state, so screenshots and recordings show the animated
// view at work: sticks, triggers and a finger in motion, a few presses, and
// loops playing on channels 1 and 2. `t` in seconds.
LiveState demoLive(double t) {
    LiveState s;
    s.connected = true;
    s.hasTouchpad = true;
    s.gamepadName = "DualSense (demo)";
    s.presentButtons = 0;  // a DualSense's: no paddles
    for (Button b : {Button::South, Button::East, Button::West, Button::North, Button::Back, Button::Guide, Button::Start, Button::LeftStick,
                     Button::RightStick, Button::LeftShoulder, Button::RightShoulder, Button::DpadUp, Button::DpadDown, Button::DpadLeft,
                     Button::DpadRight, Button::Misc1, Button::Touchpad, Button::LeftTrigger, Button::RightTrigger})
        s.presentButtons |= 1u << static_cast<int>(b);
    auto axis = [](double v) { return static_cast<std::int16_t>(std::clamp(v, -1.0, 1.0) * 32767); };
    // Every motion repeats within the note loop's eight seconds, so a recording of them loops seamlessly.
    const double w = 2 * 3.14159265358979 / 8.0;
    s.axes[static_cast<std::size_t>(Axis::LeftX)] = axis(0.7 * std::cos(t * 2 * w));
    s.axes[static_cast<std::size_t>(Axis::LeftY)] = axis(-0.6 * std::sin(t * 2 * w));
    s.axes[static_cast<std::size_t>(Axis::RightX)] = axis(0.35 + 0.1 * std::sin(t * w));
    s.axes[static_cast<std::size_t>(Axis::RightY)] = axis(-0.8 + 0.1 * std::cos(t * w));
    s.axes[static_cast<std::size_t>(Axis::LeftTrigger)] = axis(0.5 + 0.45 * std::sin(t * 2 * w));
    s.axes[static_cast<std::size_t>(Axis::RightTrigger)] = axis(0.25 + 0.25 * std::sin(t * 3 * w + 1.0));
    s.fingers[0] = {true, static_cast<float>(0.5 + 0.25 * std::cos(t * w)), static_cast<float>(0.5 + 0.25 * std::sin(t * 2 * w))};
    s.hasMotion = true;
    s.tiltActive = true;
    s.tiltX = static_cast<float>(0.5 + 0.3 * std::sin(t * w));
    s.tiltY = static_cast<float>(0.5 + 0.25 * std::sin(t * 2 * w + 1.0));
    s.frozenAxes = static_cast<std::uint8_t>((1u << static_cast<int>(Axis::RightX)) | (1u << static_cast<int>(Axis::RightY)));

    // Presses: Cross and Triangle in turn, the D-pad's Up latched, L1 and L2 held.
    auto press = [&](Button b, int slot) {
        s.buttons |= 1u << static_cast<int>(b);
        s.sounding |= std::uint64_t{1} << slot;
    };
    const double beat = std::fmod(t, 4.0);
    if (beat < 0.6)
        press(Button::South, Engine::slotOf(Button::South));
    if (beat >= 2.0 && beat < 2.6)
        press(Button::North, Engine::slotOf(Button::North));
    if (std::fmod(t, 8.0) < 0.3)
        s.buttons |= 1u << static_cast<int>(Button::DpadUp);
    for (int slot : {Engine::slotOf(HatDir::Up), Engine::slotOf(Button::LeftShoulder), Engine::slotOf(Button::LeftTrigger)})
        s.sounding |= std::uint64_t{1} << slot;

    // Movement loops, recorded at Engine::kLoopRateHz, playing.
    const double samples = t * Engine::kLoopRateHz;
    auto curve = [&](LoopSource source, int lengthX, int lengthY, double fx, double fy, double phase, bool backAndForth) {
        auto& l = s.loops[static_cast<std::size_t>(source)];
        for (int k = 0; k < 2; ++k) {
            auto& lane = l.lanes[static_cast<std::size_t>(k)];
            const int length = k == 0 ? lengthX : lengthY;
            lane.status.state = LoopStatus::State::Playing;
            lane.status.length = length;
            const int lap = static_cast<int>(samples) / length;
            const int at = static_cast<int>(samples) % length;
            lane.status.direction = backAndForth && lap % 2 ? -1 : 1;
            lane.status.position = lane.status.direction > 0 ? at : length - 1 - at;
            lane.points = LiveState::Lane::kPoints;
            for (int i = 0; i < lane.points; ++i) {
                const double u = 2 * 3.14159265358979 * i / lane.points;
                const auto p = static_cast<std::size_t>(i);
                lane.v[p] = static_cast<std::uint8_t>(k == 0 ? 128 + 100 * std::sin(fx * u + phase) : 128 + 90 * std::sin(fy * u));
                lane.on[p] = source != LoopSource::Finger1 || i % 53 < 40;  // a lifted finger now and then
            }
        }
        l.status = l.lanes[0].status;
        l.status.length = std::max(lengthX, lengthY);
    };
    curve(LoopSource::LeftStick, 800, 600, 1, 2, 0, true);
    curve(LoopSource::Finger1, 800, 800, 2, 3, 0.5, false);
    curve(LoopSource::Triggers, 400, 1000, 1, 1, 1.2, false);

    // A note loop: Fm9, Bbm9, Dbmaj9, Ebadd9 over eight seconds, a melody on top.
    auto& n = s.loops[static_cast<std::size_t>(LoopSource::Notes)];
    n.status.state = LoopStatus::State::Playing;
    n.status.length = 1600;
    n.status.position = static_cast<int>(samples) % n.status.length;
    static constexpr int kNotes[][3] = {
        {0, 340, 41},     {0, 340, 56},     {0, 340, 60},     {0, 340, 63},     {0, 340, 67},
        {400, 740, 46},   {400, 740, 61},   {400, 740, 65},   {400, 740, 68},   {800, 1140, 49},
        {800, 1140, 60},  {800, 1140, 63},  {800, 1140, 68},  {1200, 1540, 51}, {1200, 1540, 58},
        {1200, 1540, 63}, {1200, 1540, 65}, {120, 240, 72},   {520, 640, 73},   {920, 1040, 75},
        {1320, 1440, 77}, {1460, 1580, 75},
    };
    for (const auto& x : kNotes) {
        auto& iv = s.noteLoop[static_cast<std::size_t>(s.noteLoopCount++)];
        iv.start = x[0];
        iv.end = x[1];
        iv.note = static_cast<std::uint8_t>(x[2]);
        iv.velocity = 100;
    }
    s.loopChannels = 0b11;
    return s;
}

constexpr const char* kUsage = R"(Usage: arcadance [preset.json]
       arcadance --headless [options] <preset.json>   (no window, see --headless --help)

Opens the Arcadance window. With a preset file, its folder becomes the presets
folder and the preset is opened.

In the window: F11 toggles fullscreen, Ctrl+Q quits.
)";

// A phosphor screen's look: every colour of the frame turned to its brightness,
// tinted green or blue (90 %: a little grey kept), whiter at the top.
void phosphor(ImDrawData* data, bool blue) {
    constexpr float kTint = 0.9f;
    auto remap = [blue](ImU32 c) {
        const float r = static_cast<float>(c & 0xFF) / 255.0f, g = static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                    b = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
        const float mx = std::max({r, g, b});
        const float lum = 0.3f * r + 0.59f * g + 0.11f * b;
        // The darkest to black, the middle tones lifted: lines, grids and small text stay readable.
        const float d = std::pow(std::clamp((std::max(lum, mx * 0.8f) - 0.10f) / 0.75f, 0.0f, 1.0f), 0.8f);
        const float w = std::clamp((d - 0.82f) / 0.18f, 0.0f, 1.0f) * 0.7f;
        const float tr = 0.25f, tg = blue ? 0.78f : 1.0f, tb = blue ? 1.0f : 0.45f;
        const float grey = d * 0.95f;
        auto q = [&](float tint) {
            const float v = grey + ((tint * (1 - w) + w) * d - grey) * kTint;
            return static_cast<ImU32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return q(tr) | (q(tg) << 8) | (q(tb) << 16) | (c & 0xFF000000u);
    };
    for (ImDrawList* list : data->CmdLists)
        for (ImDrawVert& v : list->VtxBuffer)
            v.col = remap(v.col);
}

class GuiApp {
public:
    explicit GuiApp(GuiOptions options) : options_(std::move(options)) {}
    int run();

private:
    enum class NameDialog { None, New, SaveAs, SessionSaveAs };
    enum class FolderTarget { None, Presets };

    bool init();
    void shutdown();
    void fitPage();
    void frame();
    void update();
    void handleEvent(const SDL_Event& e);
    bool saveScreenshot(const fs::path& file);

    // Drawing.
    void draw();
    bool drawKeyChoices();
    void drawOptionsOverlay(const LiveState& live);
    void drawPerformanceStatus(const LiveState& live, float height);
    void drawPerformance(const LiveState& live);
    void drawMappingOverlay(const LiveState& live);
    void toggleFullscreen();
    // The session screen, in place of the modules: shown or not, and its
    // cursor's slot (its channel is the active one). Leaving it, every
    // channel records into its current clip again.
    void toggleSession(const LiveState& live);
    void showView(int view, const LiveState& live);
    void drawNotesPanel(const LiveState& live);  // the note loop's editor, a panel of the main screen
    void openNotes(const LiveState& live) {      // it, on the main screen
        showView(0, live);
        notesPanel_ = true;
        overlaySpec_.reset();
    }  // 0: the main screen, 1: the session's, 2: the song's
    bool sessionView_ = false;
    // The song screen: the song (kept in the session, given to the engine at
    // each change), the cursor on it, items copied from a cell.
    void toggleSong(const LiveState& live);
    void songEvent(Engine::SessionEvent e, const LiveState& live);
    void songAction(const PerformanceView::SongAction& a, const LiveState& live);
    void songEdited() {
        songUndo();
        runtime_->setSong(song_);
    }
    // Undo and redo, for the edits of the session and song screens: each step
    // keeps the song, a channel's chains (with their names) or clips' loops
    // as they were before it; undoing one keeps them as they are, for redo.
    struct UndoStep {
        enum class Kind : std::uint8_t { Song, Chains, Clips } kind = Kind::Song;
        std::string what;
        double at = 0;  // when: the same edit soon after only moves this on
        midipad::Song song;
        int channel = 0;
        std::array<LiveState::Chain, Engine::kChains> chains{};
        std::array<std::string, Engine::kChains> chainNames;
        std::vector<std::pair<int, int>> clips;  // (channel, slot)
        std::vector<midipad::SessionLoop> loops;
        std::vector<std::string> clipNames;  // one per clip
    };
    std::vector<UndoStep> undo_, redo_;
    midipad::Song songShadow_;  // the song before the edit being made
    std::string undoWhat_;      // what the edit being made is (the menu entry chosen), this frame
    std::array<LiveState::Chains, kChannelCount> liveChains_{};  // as last drawn
    std::string toast_;  // in the status bar, a moment: what was done, undone or redone
    bool toastUndone_ = false;
    bool toastPlain_ = false;  // a message alone: no Undo
    bool notesPanel_ = false;  // the note editor, in the main screen's modules' place
    int songLoopFrom_ = -1, songLoopTo_ = -1;  // the part of the song looped, in beats of the whole song (-1: none)
    int songStartBar_ = -1;                     // where Play starts in the song, a bar (-1: the cursor's section)
    // Where Play starts: the marker's section and beats into it, or the cursor's section.
    std::pair<int, int> songStart(const LiveState& live) const {
        if (song_.sections.empty())
            return {-1, 0};
        if (songStartBar_ < 0)
            return {std::clamp(songCursor_.row, 0, static_cast<int>(song_.sections.size()) - 1), 0};
        int bar = 0;
        for (int i = 0; i < static_cast<int>(song_.sections.size()); ++i) {
            const auto& sec = song_.sections[static_cast<std::size_t>(i)];
            const int length = PerformanceView::songSectionBars(live, sec) * std::clamp(sec.repeats, 1, midipad::kMaxSectionRepeats);
            if (songStartBar_ < bar + length)
                return {i, (songStartBar_ - bar) * std::max(live.beatsPerBar, 1)};
            bar += length;
        }
        return {0, 0};
    }
    double toastUntil_ = 0;
    void pushUndo(UndoStep step);
    void songUndo();
    void rememberChains(int channel, const char* what);
    void rememberClips(std::vector<std::pair<int, int>> clips, const char* what);
    // A clip (slot of channel), or the whole song (channel -1), written as a MIDI file in the
    // export folder (Music/Arcadance), which opens: from there, it drags into a DAW.
    void exportMidi(int channel, int slot);
    UndoStep currentState(const UndoStep& like);
    void applyState(const UndoStep& state);
    void undoRedo(bool redo);
    void resetUndo();
    // The keyboard on the session and song screens: the arrows and Enter as
    // the controller's cursor, Space as Play / Stop, Delete, Ctrl+C, Ctrl+V,
    // Ctrl+D on what the cursor is on.
    void screenKeys(const LiveState& live);
    // A clip or a chain step dropped in the session screen (or moved from a menu): see SessionAction.
    void sessionDrop(const PerformanceView::SessionAction& a, const LiveState& live);
    // A row of the session screen: its clips launched on every channel with one there (the others play on).
    void launchRow(int slot, const LiveState& live);
    void captureRow(const LiveState& live);  // the clips playing, copied into the first empty row
    // Clips copied (Ctrl+C), for Ctrl+V: each where it was from the first (channels, slots) and its loops.
    struct ClipCopy {
        int channels = 0, slots = 0;
        std::vector<midipad::SessionLoop> loops;
        std::string name;
    };
    std::vector<ClipCopy> clipClipboard_;
    // The clips selected in the session screen (Shift+click, a rectangle), and the items of a song cell.
    std::vector<std::pair<int, int>> sessionSel_;
    std::vector<int> songSel_;
    int songSelRow_ = -1, songSelCol_ = -1;
    bool songClipboardPart_ = false;  // the song clipboard holds items picked out of a cell: pasted among the others
    std::vector<midipad::SongItem>* songCell(int row, int col);
    int placeSongItem(int row, int col, midipad::SongItem item, const LiveState& live);  // on its bar, in playing order: its index there
    // A new item after the cell's last (the menu picks what it plays).
    void addSongItem(int row, int col, const LiveState& live, midipad::SongItem::Kind kind, int index);
    // `bar`: a new item on that beat of the section + 1 (0: after the last).
    void openSongPick(int row, int col, int item, const LiveState& live, bool fromController, ImVec2 at, int bar = 0);
    void addSongSection(int at);
    // Play from a section (its name, its menu): a jump while the song plays,
    // else the song plays from there, what played stopped first.
    void playSongFrom(int section, const LiveState& live);
    bool songView_ = false;
    midipad::Song song_;
    PerformanceView::SongCursor songCursor_;
    std::vector<midipad::SongItem> songClipboard_;
    int renameSection_ = -1;
    int renameRow_ = -1, renameClipChannel_ = -1, renameClipSlot_ = -1;  // a row, a clip being renamed
    // The names of the session screen's rows and of the clips (empty: none),
    // and the clips seen holding something: one emptied loses its name.
    std::array<std::string, Engine::kClipSlots> rowNames_{};
    std::array<std::array<std::string, Engine::kClipSlots>, kChannelCount> clipNames_{};
    std::array<std::uint8_t, kChannelCount * Engine::kClipSlots> clipSeen_{};
    std::string clipName(int channel, int slot) const {  // its name, or "Clip n"
        const std::string& n = clipNames_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))][static_cast<std::size_t>(std::clamp(slot, 0, Engine::kClipSlots - 1))];
        return n.empty() ? "Clip " + std::to_string(slot + 1) : n;
    }
    bool editView_ = false;   // edit mode: a cursor on the main view (Shift + Mode)
    bool helpShown_ = false;  // the Mode control held: the help of the screen
    // Context menus: the same small list for the mouse (where it clicked) and
    // the controller (beside its cursor).
    struct MenuEntry {
        std::string label;
        int id = 0;
        bool enabled = true;
        bool checked = false;  // a check mark after it
        std::string shortcut;  // its key, dimmed on the right
        bool separator = false;  // a line between groups, not an entry
        bool danger = false;     // what clears or deletes: red, last
        std::vector<MenuEntry> sub;  // a submenu: chosen, its entries take the menu's place
    };
    struct ContextMenu {
        PerformanceView::MenuTarget target;
        std::string title;
        std::vector<MenuEntry> items;
        bool fromController = false, opened = false;  // opened by the controller: beside its cursor
        bool above = false;  // above it (pivot on its bottom)
        int selected = 0;
        std::vector<std::pair<std::string, std::vector<MenuEntry>>> parents;  // in a submenu: the menus above (title, entries)
        ImVec2 at;
    };
    std::optional<ContextMenu> menu_;
    // An entry chosen in `menu`, said for undo: "Clear (clip 3)".
    static std::string menuWhat(const ContextMenu& menu, std::string label) {
        if (label.size() >= 3 && label.compare(label.size() - 3, 3, "\xE2\x80\xA6") == 0)
            label.resize(label.size() - 3);
        std::string title = menu.title.substr(0, menu.title.find('\n'));
        for (char& ch : title)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        while (title.find("  ") != std::string::npos)
            title.erase(title.find("  "), 1);
        return title.empty() ? label : label + " (" + title + ")";
    }
    bool menuFromController_ = false;  // the menu last chosen from, for a menu it opens in turn
    ImVec2 menuAt_{};
    // Renaming a channel (its block's menu).
    int renameChannel_ = -1;
    int renameChain_ = -1;  // a chain of that channel; -1: the channel itself
    std::string renameInput_;
    bool openRename_ = false;
    void drawRenameDialog();
    // Select and Menu on the same button: a press opens the menu, the same button then chooses.
    bool sharedSelectMenu() const { return settings_.navSelect >= 0 && settings_.navSelect == settings_.navMenu; }
    // What a button does in the preset that matters for the edit menu: red,
    // it cannot be one of its buttons; amber, a reminder; empty, nothing.
    struct NavConflict {
        bool blocking = false;
        std::string text;
    };
    NavConflict navConflict(int button) const;
    void toggleEdit(const LiveState& live);
    void openMenu(const PerformanceView::MenuTarget& target, const LiveState& live, bool fromController, ImVec2 at);
    // The modulation menu of a loop (MenuTarget::ModPick): the loops it modulates, each how deep and
    // which way, as set there (modEdit_, per track: the runtime answers a frame later); the depth and
    // direction rows set modFocus_'s. `step` 1: a click or Select (the depth up, after 100 % back to
    // 10 %), 2 / -2: the depth up or down.
    std::array<midipad::LoopModulation, Engine::kNoteTrack> modEdit_{};
    int modFocus_ = -1;
    void fillModMenu(ContextMenu& menu, const LiveState& live) const;
    void changeModulation(int id, int step, const LiveState& live);
    void runMenu(const PerformanceView::MenuTarget& target, int id, const LiveState& live);
    void enterSubmenu(int entry);  // the open menu's entry `entry` (a submenu): its entries in its place
    void menuEvent(Engine::SessionEvent e, const LiveState& live);
    void editEvent(Engine::SessionEvent e, const LiveState& live);
    void drawContextMenu(const LiveState& live);
    void drawHelpOverlay();
    // A button of the controller as named in the help, from its role.
    std::string buttonName(int button, const char* fallback) const {
        return button >= 0 ? label(static_cast<Button>(button)) : std::string(fallback);
    }
    std::string bindingName(ActionType type, const char* fallback) const;
    int sessionSlot_ = 0, sessionChannel_ = 0;  // sessionSlot_ kClipSlots: the cursor is in the chain
    int chainCursor_ = 0;  // its step there (kChainSteps: the CHAIN cell)
    int sessionButton_ = -1;  // the cursor on the stop (0), mute (1) or solo (2) button under the clips; -1: not there
    void sessionButtonEvent(Engine::SessionEvent e, const LiveState& live);
    void toggleMute(int channel, const LiveState& live) { runtime_->setMuted(channel, ((live.muted >> channel) & 1u) == 0); }
    void toggleSolo(int channel, const LiveState& live) { runtime_->setSoloed(channel, ((live.soloed >> channel) & 1u) == 0); }
    std::array<int, kChannelCount> shownChain_{};  // the chain the session screen shows, per channel
    PerformanceView::ChainNames chainNames_;  // kept in the session
    // The cursor in the chain: L3 (`press`), R3 (`clear`), the right stick (`clipSteps` up / down, `repeatSteps` left / right).
    void chainEvent(Engine::SessionEvent e, const LiveState& live);
    // On the chain the session screen shows for the channel.
    int shownChain(int channel) const { return shownChain_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }
    void setChainStep(int channel, int step, int clip, int repeats) {
        rememberChains(channel, "Chain step changed");
        runtime_->setChainStep(channel, shownChain(channel), step, clip, repeats);
    }
    void showChain(int channel, int chain) {
        shownChain_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))] = (chain % Engine::kChains + Engine::kChains) % Engine::kChains;
    }
    int sessionChannelAsked_ = -1;  // a channel asked of the runtime, not shown by it yet
    void sessionEvent(midipad::Engine::SessionEvent e, const LiveState& live);
    void moveSessionCursor(int channel, int slot, const LiveState& live);
    // A clip of the session screen pressed (L3, R3 or a click): the one
    // playing stops (one waiting to start, or a stop waiting, is called off);
    // another one is launched; an empty slot is only chosen.
    void pressClip(int channel, int slot, const LiveState& live);
    // The clips picked (channel, slot), into the clipboard; pasted from (channel, slot), as they were placed.
    void copyClips(const std::vector<std::pair<int, int>>& picked, const std::string& name);
    void pasteClips(int channel, int slot, const std::string& name);
    void clearClips(const std::vector<std::pair<int, int>>& picked, const std::string& what);
    // Every window over the view looks like the Settings panel.
    void pushWindowLook();
    void popWindowLook();
    // False when closed; dialogDismissed_ then tells whether its close button just closed it.
    bool beginDialog(const char* title, ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize);
    bool dialogDismissed_ = false;
    void endDialog();
    int dialogButtons(std::initializer_list<const char*> labels, int disabled = -1);  // the one clicked, or -1
    void applySync();  // the Link settings, to the runtime
    void drawDialogs();
    void drawNameDialog();
    void drawUnsavedDialog();
    void drawDeleteDialog();
    void drawErrorDialog();
    void drawOutputDialog();
    void drawOpenSessionDialog();
    void drawNewSongDialog();
    // Copy mapping: into a channel, from another, the parts chosen (CopyPart bits).
    void drawCopyDialog();
    bool openCopyDialog_ = false;
    // A channel's colour: the channel (from its menu).
    void drawColourDialog();
    // A value typed after a double-click: a chain step's repeats (0: forever; row: the step, item: its
    // clip), a song item's repeats or a silence's beats (col, row, item), a section's bars (row).
    struct ValueAsk {
        enum class Kind : std::uint8_t { ChainRepeats, ItemRepeats, RestBeats, SectionBars, NoteVelocity, NoteChance, SectionRepeats } kind = Kind::ItemRepeats;
        int col = 0, row = 0, item = 0, value = 0;
    };
    std::vector<midipad::LoopNoteRef> valueAskNotes_;  // NoteVelocity, NoteChance: the notes set
    std::optional<ValueAsk> valueAsk_;
    bool openValueAsk_ = false;
    void drawValueAsk();
    static Preset newPreset();
    bool openColour_ = false;
    int colourChannel_ = -1;
    int copyTo_ = 0, copyFrom_ = 0;
    unsigned copyParts_ = 0xFF;
    int copyControlFrom_ = -1;  // the channel a control's window copies from (-1: channel 1, or 2 on channel 1)
    std::string channelLabel(int channel) const {
        const std::string& name = preset_.channelNames[static_cast<std::size_t>(channel)];
        return "Channel " + std::to_string(channel + 1) + (name.empty() ? "" : "  " + name);
    }
    void drawAboutDialog();

    // Sessions: every channel's loops, the preset and the tonality, in a file.
    std::vector<std::string> sessionNames() const;
    fs::path sessionPath(const std::string& name) const { return sessionsDir_ / (name + kSessionExtension); }
    void openSession(const std::string& name);
    void newSong();  // an empty session: no clip, chain or section; the preset and tonality stay
    void saveSession();
    bool saveSessionAs(const std::string& name);
    // The session as Save would write it (empty when the loops do not answer).
    std::optional<Session> collectSession();
    // What a session holds of the work (loops, clips, chains, song, mute and
    // solo), as text: not the preset, tonality, tempo or active channel.
    static std::string sessionPrint(Session session, bool withMapping = false);
    // Quitting: when the session changed since it was saved, opened or
    // started, asks to save it first.
    void quitAsked();
    void drawQuitDialog();
    std::optional<std::string> sessionPrint_;  // the session as saved, opened or started
    int sessionPrintIn_ = 0;  // frames until it is taken (once the loops and chains are published)
    bool openQuitDialog_ = false;

    // Presets.
    void refreshPresetList();
    void openPreset(const std::string& name);
    void openFirstOrUntitled();
    void startUntitled();
    bool saveCurrent();
    bool saveAs(const std::string& name);
    void edited();
    void pushToEngine();
    void guard(std::function<void()> action);
    void setPresetsDir(const fs::path& dir);

    // MIDI output.
    void refreshPorts();
    bool useOutput(AppSettings::Output kind, const std::string& port);
    std::shared_ptr<MidiOutput> openOutput(AppSettings::Output kind, const std::string& port, std::string& error);
    int virmidiPortCount() const;
    void startVirMidiJob(bool remove);
    void finishVirMidiJob();
    void drawVirMidiSection(bool inOutputDialog);

    void log(const std::string& line);
    void showError(const std::string& message);
    void persistSettings();
    PerformanceView::Look look() const {
        return settings_.theme == AppSettings::Theme::PhosphorGreen ? PerformanceView::Look::PhosphorGreen
             : settings_.theme == AppSettings::Theme::PhosphorBlue  ? PerformanceView::Look::PhosphorBlue
                                                                    : PerformanceView::Look::Classic;
    }
    void browseFolder(FolderTarget target, const fs::path& start);
    static void SDLCALL onFolderChosen(void* self, const char* const* files, int filter);

    GuiOptions options_;
    ImGuiStyle baseStyle_;   // the style at the page's own size
    float pageScale_ = 0;    // how much the page is scaled to fit the window
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool quit_ = false;
    int frameCount_ = 0;
    std::uint64_t nextFrameAt_ = 0;  // with a frame rate set in Settings: when the next frame may start
    bool standardResolution_ = false;  // the window opened without high pixel density (AppSettings)

    fs::path settingsFile_;
    AppSettings settings_;
    std::unique_ptr<PresetLibrary> library_;
    std::vector<std::string> presetNames_;

    Preset preset_;
    bool hasFile_ = false;  // false: "Untitled", never saved
    bool dirty_ = false;  // the mapping changed since saved (no song saved yet: it is the mapping's)
    // Once a song is saved (or opened), the mapping is its own: changes go to it
    // (songMappingDirty_), saved with it; the mapping files are left alone.
    bool songOwnsMapping() const { return sessionName_.has_value(); }
    void mappingChanged() {
        if (songOwnsMapping())
            songMappingDirty_ = true;
        else
            dirty_ = true;
    }
    bool saveShown();  // SAVE, Ctrl+S: the song once saved, the mapping before
    bool songMappingDirty_ = false;
    Preset songMapping_;  // the song's mapping as saved or opened (Revert)
    std::optional<fs::file_time_type> presetTime_;
    std::chrono::steady_clock::time_point nextFileCheck_{};

    std::shared_ptr<MidiOutput> output_;
    std::string outputLabel_;
    std::vector<std::string> ports_;
    std::unique_ptr<Runtime> runtime_;
    virmidi::Status virmidiStatus_;
    int virmidiPorts_ = virmidi::kMaxPorts;  // the choices of the Settings panel
    bool virmidiAtStartup_ = false;
    std::future<std::string> virmidiJob_;  // pkexec, waiting for the password
    bool virmidiRemoving_ = false;

    MappingEditor editor_;
    PerformanceView performance_;
    PerformanceView::Fonts uiFonts_;
    std::optional<std::string> overlaySpec_;  // control edited over the animated view
    bool overlayAppearing_ = false;
    int overlayPlacingFrames_ = 0;
    bool optionsOpen_ = false;  // the Settings panel over the animated view
    bool optionsAppearing_ = false;
    int optionsPlacingFrames_ = 0;

    std::mutex pendingLogMutex_;
    std::vector<std::string> pendingLog_;  // from the input thread

    // Dialogs.
    NameDialog nameDialog_ = NameDialog::None;
    bool openNameDialog_ = false;
    std::string nameInput_, nameError_;
    std::function<void()> afterSave_;
    std::function<void()> pendingAction_;
    bool openUnsavedDialog_ = false;
    bool openDeleteDialog_ = false;
    std::string errorMessage_;
    bool openErrorDialog_ = false;
    bool openOutputDialog_ = false;
    bool outputDialogOpen_ = false;
    bool closeOutputDialog_ = false;  // VirMIDI ports created from it: done
    std::string outputDialogReason_;

    // The clock's tempo in the top bar: tapped, typed, turned with the wheel.
    TapTempo tap_;
    bool editingTempo_ = false, tempoEditAppearing_ = false;
    std::string tempoText_;
    float tempoWheelRest_ = 0;
    double lastTempoWheel_ = -1e9;
    int tempoWheelStreak_ = 0;
    double tempoSaveAt_ = 0;  // when to save the internal tempo changed (0: nothing to save)
    void applyTempo(double bpm);

    fs::path sessionsDir_;
    std::optional<std::string> sessionName_;  // the session opened or saved last
    bool openSessionDialog_ = false;
    bool openNewSongDialog_ = false;
    std::vector<std::string> sessionList_;
    int sessionChoice_ = -1;
    std::string deleteSessionName_;
    bool openDeleteSessionDialog_ = false;
    std::string replaceName_;  // Save session as: the existing session to replace, once asked
    bool openAboutDialog_ = false;
    std::future<UpdateCheck> updateJob_;
    std::optional<UpdateCheck> updateResult_;

    std::mutex folderMutex_;
    FolderTarget folderTarget_ = FolderTarget::None;
    std::optional<std::string> chosenFolder_;
};

// --- Setup ------------------------------------------------------------------

bool GuiApp::init() {
    // Keep playing when another window (the DAW) has the focus: by default SDL
    // ignores controllers while none of its windows is focused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::cerr << "arcadance: SDL_Init failed: " << SDL_GetError() << '\n';
        return false;
    }
    SDL_SetGamepadEventsEnabled(false);  // the runtime polls the gamepad itself
    SDL_SetJoystickEventsEnabled(false);

    const fs::path pref = prefPath();
    takeOverFormerFolder(pref);
    settingsFile_ = pref / "settings.json";
    sessionsDir_ = pref / "Sessions";
    AppSettings defaults;
    defaults.presetsDir = pref / "presets";
    settings_ = loadSettings(settingsFile_, defaults);
    if (!settings_.migratedFromMidipad) {  // MidiPad's files, once (remove from 0.5 on)
        const std::string former = formerPrefPath(pref).string(), dir = settings_.presetsDir.lexically_normal().string();
        if (dir.starts_with(former))  // presets kept in MidiPad's folder: now in Arcadance's copy of it
            settings_.presetsDir = pref.lexically_normal().parent_path() / dir.substr(former.size() + (dir.size() > former.size() ? 1 : 0));
        renameFormerFiles(settings_.presetsDir, sessionsDir_);
        settings_.migratedFromMidipad = true;
        try {
            saveSettings(settings_, settingsFile_);
        } catch (const std::exception&) {
        }
    }
    if (options_.preset) {
        const fs::path file = fs::absolute(*options_.preset);
        settings_.presetsDir = file.parent_path();
        settings_.lastPreset = file.stem().string();
    }

    library_ = std::make_unique<PresetLibrary>(settings_.presetsDir);
    refreshPresetList();
    if (library_->contains(settings_.lastPreset))
        openPreset(settings_.lastPreset);
    else
        openFirstOrUntitled();

    // Reconnect to the last port used, otherwise ask
    // which one to use. The virtual port stands in until the user answers.
    refreshPorts();
    std::string error;
    if (settings_.outputChosen) {
        output_ = openOutput(settings_.output, settings_.existingPortName, error);
        if (output_) {
            log("MIDI output: " + outputLabel_);
        } else {
            outputDialogReason_ = "The last port used, \"" + settings_.existingPortName + "\", was not found.";
            openOutputDialog_ = true;
        }
    } else {
        outputDialogReason_ = "Where Arcadance sends its MIDI.";
        openOutputDialog_ = true;
    }
    if (!output_) {
        output_ = openOutput(AppSettings::Output::VirtualPort, {}, error);
        if (!output_)
            log("No MIDI output: " + error);
    }

    Runtime::Options runtimeOptions;
    runtimeOptions.dpadSettlePressMs = settings_.dpadSettlePressMs;
    runtimeOptions.dpadSettleReleaseMs = settings_.dpadSettleReleaseMs;
    runtimeOptions.humanize = settings_.humanize;
    runtime_ = std::make_unique<Runtime>(output_, std::make_shared<const Preset>(preset_), runtimeOptions,
                                         [this](const std::string& line) {
                                             std::lock_guard lock(pendingLogMutex_);
                                             pendingLog_.push_back(line);
                                         });
    applySync();
    runtime_->setNavButtons(settings_.navSelect, settings_.navMenu, settings_.navBack);

    // At standard resolution, a scaled screen's compositor stretches the
    // window, drawn at its size in points: fewer pixels for Arcadance to draw.
    standardResolution_ = settings_.standardResolution;
    if (!SDL_CreateWindowAndRenderer("Arcadance", 1080, 846,
                                     SDL_WINDOW_RESIZABLE | (standardResolution_ ? 0 : SDL_WINDOW_HIGH_PIXEL_DENSITY),
                                     &window_, &renderer_)) {
        std::cerr << "arcadance: cannot open the window: " << SDL_GetError() << '\n';
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);
    // Where the window's coordinates are pixels while the screen is scaled
    // (X11 at 200 %, say), open it at the page's size in those pixels. Where
    // they already are scaled points (Wayland), the page fits as it is.
    const float pixelsPerPoint = std::max(SDL_GetWindowPixelDensity(window_), 1.0f);
    const float unscaled = std::max(SDL_GetWindowDisplayScale(window_), 1.0f) / pixelsPerPoint;
    float minScale = 1.0f;
    const bool wayland = std::strcmp(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "", "wayland") == 0;
    if (unscaled > 1.05f && !(standardResolution_ && wayland)) {  // (Wayland stretches the standard window itself)
        SDL_Rect usable{};
        float fit = unscaled;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window_), &usable) && usable.w > 0 && usable.h > 0)
            fit = std::min({unscaled, static_cast<float>(usable.w) / kPageW, static_cast<float>(usable.h) / kPageH});
        SDL_SetWindowSize(window_, static_cast<int>(kPageW * fit), static_cast<int>(kPageH * fit));
        minScale = unscaled;
    }
    SDL_SetWindowMinimumSize(window_, static_cast<int>(640 * minScale), static_cast<int>(500 * minScale));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the layout is fixed
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
#ifdef NDEBUG
    io.ConfigDebugHighlightIdConflicts = false;  // a programmer's warning: debug builds only
#endif
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 5;
    style.GrabRounding = 4;
    style.TabRounding = 5;
    style.PopupRounding = 6;
    style.ScrollbarRounding = 6;
    style.FrameBorderSize = 1;
    style.SeparatorTextBorderSize = 1;
    {  // the flat look of the screens, for the windows and dialogs too: their greys, one green accent
        using Theme = PerformanceView::Theme;
        auto c = [](ImU32 v) { return ImGui::ColorConvertU32ToFloat4(v); };
        ImVec4* col = style.Colors;
        col[ImGuiCol_Text] = c(Theme::kText);
        col[ImGuiCol_TextDisabled] = c(Theme::kDim);
        col[ImGuiCol_WindowBg] = c(Theme::kPanelHeader);
        col[ImGuiCol_PopupBg] = c(Theme::kPanel);
        col[ImGuiCol_Border] = c(IM_COL32(0x4A, 0x4F, 0x57, 200));
        col[ImGuiCol_FrameBg] = c(Theme::kControl);
        col[ImGuiCol_FrameBgHovered] = c(IM_COL32(0x47, 0x4C, 0x54, 255));
        col[ImGuiCol_FrameBgActive] = c(IM_COL32(0x52, 0x58, 0x62, 255));
        col[ImGuiCol_TitleBg] = col[ImGuiCol_TitleBgActive] = col[ImGuiCol_TitleBgCollapsed] = c(Theme::kTopBar);
        col[ImGuiCol_CheckMark] = c(Theme::kGreen);
        col[ImGuiCol_SliderGrab] = c(Theme::kGreen);
        col[ImGuiCol_SliderGrabActive] = c(IM_COL32(0x6C, 0xF0, 0xA6, 255));
        col[ImGuiCol_Button] = c(Theme::kControl);
        col[ImGuiCol_ButtonHovered] = c(IM_COL32(0x47, 0x4C, 0x54, 255));
        col[ImGuiCol_ButtonActive] = c(IM_COL32(0x52, 0x58, 0x62, 255));
        col[ImGuiCol_Header] = c(IM_COL32(0x3F, 0x46, 0x52, 255));
        col[ImGuiCol_HeaderHovered] = c(IM_COL32(0x38, 0x3D, 0x45, 255));
        col[ImGuiCol_HeaderActive] = c(IM_COL32(0x45, 0x4C, 0x58, 255));
        col[ImGuiCol_Separator] = col[ImGuiCol_SeparatorHovered] = c(IM_COL32(0x4A, 0x4F, 0x57, 255));
        col[ImGuiCol_SeparatorActive] = c(Theme::kGreen);
        col[ImGuiCol_Tab] = c(Theme::kControl);
        col[ImGuiCol_TabHovered] = c(IM_COL32(0x47, 0x4C, 0x54, 255));
        col[ImGuiCol_TabSelected] = c(IM_COL32(0x2A, 0x5B, 0x43, 255));
        col[ImGuiCol_TabSelectedOverline] = c(Theme::kGreen);
        col[ImGuiCol_ScrollbarBg] = c(IM_COL32(0, 0, 0, 40));
        col[ImGuiCol_ScrollbarGrab] = c(IM_COL32(0x4E, 0x53, 0x5C, 255));
        col[ImGuiCol_ResizeGrip] = c(IM_COL32(0x4E, 0x53, 0x5C, 120));
        col[ImGuiCol_NavCursor] = c(Theme::kCyan);
    }
    baseStyle_ = style;  // scaled with the page, frame by frame (see fitPage)

    ImFontConfig font;
    font.FontDataOwnedByAtlas = false;
    // The first font added is the default one.
    uiFonts_.medium = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoMedium),
                                                     static_cast<int>(kRobotoMedium_size), kFontSize, &font);
    uiFonts_.bold = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoBold),
                                                   static_cast<int>(kRobotoBold_size), kFontSize, &font);
    uiFonts_.condensed = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoCondensedBold),
                                                        static_cast<int>(kRobotoCondensedBold_size), kFontSize, &font);
    performance_.setFonts(uiFonts_);

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
    // The gamepad plays music; it must not drive the interface.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);

    log("Presets: " + settings_.presetsDir.string());
    sessionPrintIn_ = 1;  // the session it starts with, empty: taken at the first frame
    return true;
}

void GuiApp::shutdown() {
    runtime_.reset();  // turns every note off
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (renderer_)
        SDL_DestroyRenderer(renderer_);
    if (window_)
        SDL_DestroyWindow(window_);
    SDL_Quit();
}

int GuiApp::run() {
    const bool ok = init();
    if (ok) {
        while (!quit_) {
            // Simulated clicks: one every four frames (press, release, two idle
            // frames so popups can appear), from the third frame on.
            const int click = (frameCount_ - 2) / 4;
            const int phase = (frameCount_ - 2) % 4;
            if (frameCount_ >= 2 && phase < 2 && click < static_cast<int>(options_.clicks.size())) {
                SDL_Event e{};
                const auto [x, y] = options_.clicks[static_cast<std::size_t>(click)];
                e.type = (phase == 0) ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.windowID = SDL_GetWindowID(window_);
                e.button.button = SDL_BUTTON_LEFT;
                e.button.down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                e.button.x = x;
                e.button.y = y;
                SDL_Event motion{};
                motion.type = SDL_EVENT_MOUSE_MOTION;
                motion.motion.windowID = e.button.windowID;
                motion.motion.x = x;
                motion.motion.y = y;
                SDL_PushEvent(&motion);
                SDL_PushEvent(&e);
            }
            frame();
            const int screenshotFrame = 4 + 4 * static_cast<int>(options_.clicks.size());
            ++frameCount_;
            if (options_.screenshot && frameCount_ == screenshotFrame) {
                if (!saveScreenshot(*options_.screenshot))
                    std::cerr << "arcadance: screenshot failed: " << SDL_GetError() << '\n';
                quit_ = true;
            }
            // A recording starts where a screenshot would be taken.
            if (options_.record && frameCount_ >= screenshotFrame) {
                const int index = frameCount_ - screenshotFrame;
                char name[32];
                std::snprintf(name, sizeof name, "frame_%04d.bmp", index);
                if (!saveScreenshot(*options_.record / name)) {
                    std::cerr << "arcadance: recording failed: " << SDL_GetError() << '\n';
                    quit_ = true;
                }
                if (index + 1 >= options_.frames)
                    quit_ = true;
            }
        }
    }
    shutdown();
    return ok ? 0 : 1;
}

// The page scaled as one to the window: the same proportions in any window,
// on any screen scale, and in full screen.
void GuiApp::fitPage() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x <= 0 || size.y <= 0)
        return;
    const float scale = std::clamp(std::min(size.x / kPageW, size.y / kPageH), 0.4f, 6.0f);
    if (std::abs(scale - pageScale_) < 0.002f)
        return;
    pageScale_ = scale;
    ImGuiStyle& style = ImGui::GetStyle();
    style = baseStyle_;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;  // every size of the view follows the font's
}

void GuiApp::frame() {
    // The frame rate set in Settings (vsync still paces the screen's own).
    int fps = settings_.frameRate;
    if (settings_.slowInBackground && !(SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS))
        fps = fps > 0 ? std::min(fps, 30) : 30;
    if (fps > 0 && !options_.screenshot && !options_.record) {
        const std::uint64_t period = 1'000'000'000ull / static_cast<std::uint64_t>(fps);
        std::uint64_t now = SDL_GetTicksNS();
        if (nextFrameAt_ > now && nextFrameAt_ - now <= period) {
            SDL_DelayNS(nextFrameAt_ - now);
            now = nextFrameAt_;
        }
        nextFrameAt_ = now > nextFrameAt_ + period ? now + period : nextFrameAt_ + period;
    } else {
        nextFrameAt_ = 0;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handleEvent(e);
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(20);
        update();
        return;
    }
    update();

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    fitPage();
    if (options_.record)  // the same time step whatever the rendering speed
        ImGui::GetIO().DeltaTime = 1.0f / static_cast<float>(options_.fps);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;  // fed by editEvent, never by the backend
    ImGui::NewFrame();
    PerformanceView::setLook(look());
    draw();
    ImGui::Render();
    if (look() != PerformanceView::Look::Classic)
        phosphor(ImGui::GetDrawData(), look() == PerformanceView::Look::PhosphorBlue);

    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    if (look() != PerformanceView::Look::Classic)  // black beside the page
        SDL_SetRenderDrawColor(renderer_, 0x02, 0x03, 0x04, 255);
    else
        SDL_SetRenderDrawColor(renderer_, 0x1C, 0x1E, 0x21, 255);  // Theme::kBackground, beside the page
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);
}

bool GuiApp::saveScreenshot(const fs::path& file) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
    if (!surface)
        return false;
    const bool ok = SDL_SaveBMP(surface, file.string().c_str());
    SDL_DestroySurface(surface);
    return ok;
}

void GuiApp::handleEvent(const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
    if (e.type == SDL_EVENT_QUIT ||
        (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window_)))
        guard([this] { quitAsked(); });
}

void GuiApp::update() {
    finishVirMidiJob();
    if (sessionPrintIn_ > 0 && --sessionPrintIn_ == 0)  // the session as opened or started
        if (const auto session = collectSession())
            sessionPrint_ = sessionPrint(*session, songOwnsMapping());
    runtime_->setKey(settings_.key);
    if (const auto clear = editor_.takeClearLoopRequest())
        runtime_->clearLoop(clear->source, clear->axis);
    if (const auto mod = editor_.takeModulationRequest()) {  // a loop's modulation, set in a control's window
        if (mod->freed >= 0)
            runtime_->setModulation(Engine::sourceOfTrack(mod->freed), Engine::axisOfTrack(mod->freed), {});
        if (mod->sendsCc)
            runtime_->setModulationSendsCc(Engine::sourceOfTrack(mod->track), Engine::axisOfTrack(mod->track), *mod->sendsCc);
        else
            runtime_->setModulation(Engine::sourceOfTrack(mod->track), Engine::axisOfTrack(mod->track), mod->modulation);
    }
    if (const auto length = editor_.takeAxisLengthRequest())  // an axis loop's end dragged in a control's window
        runtime_->setAxisLoopLength(length->source, length->axis, length->length);
    if (editor_.takeRecenterTiltRequest())
        runtime_->recenterTilt();
    while (runtime_->popReport()) {
    }
    // The notes sent, for the piano roll, at the time they were sent.
    const std::uint64_t nowNs = SDL_GetTicksNS();
    while (const auto n = runtime_->popNote()) {
        const double age = static_cast<double>(nowNs > n->timeNs ? nowNs - n->timeNs : 0) * 1e-9;
        performance_.addNote(n->channel, n->note, n->velocity, n->fromLoop, ImGui::GetTime() - age);
    }
    {
        std::lock_guard lock(pendingLogMutex_);
        for (auto& line : pendingLog_)
            log(line);
        pendingLog_.clear();
    }

    std::optional<std::string> folder;
    FolderTarget target;
    {
        std::lock_guard lock(folderMutex_);
        folder = std::exchange(chosenFolder_, std::nullopt);
        target = folderTarget_;
    }
    if (folder) {
        if (target == FolderTarget::Presets)
            guard([this, dir = *folder] { setPresetsDir(dir); });
    }

    // Follow edits made to the preset file by another program.
    const auto now = std::chrono::steady_clock::now();
    if (hasFile_ && !songOwnsMapping() && now >= nextFileCheck_) {
        nextFileCheck_ = now + std::chrono::milliseconds(500);
        const auto time = modificationTime(library_->pathFor(preset_.name));
        if (time && time != presetTime_) {
            presetTime_ = time;
            if (dirty_) {
                log("The preset file changed on disk; your unsaved changes are kept.");
            } else {
                try {
                    preset_ = library_->load(preset_.name);
                    pushToEngine();
                    log("Preset reloaded (changed on disk).");
                } catch (const PresetError& e) {
                    log(std::string("Preset changed on disk but invalid: ") + e.what());
                }
            }
        }
    }

    std::string title = songOwnsMapping() ? "Arcadance - " + *sessionName_ + (songMappingDirty_ ? " *" : "")
                                          : "Arcadance - " + (hasFile_ ? preset_.name : std::string("Untitled")) + (dirty_ ? " *" : "");
    if (title != SDL_GetWindowTitle(window_))
        SDL_SetWindowTitle(window_, title.c_str());
}

// --- Presets ----------------------------------------------------------------

void GuiApp::refreshPresetList() { presetNames_ = library_->names(); }

void GuiApp::openPreset(const std::string& name) {
    try {
        preset_ = library_->load(name);
    } catch (const PresetError& e) {
        showError(std::string("Cannot open the mapping:\n") + e.what());
        return;
    }
    hasFile_ = true;
    dirty_ = false;
    presetTime_ = modificationTime(library_->pathFor(name));
    settings_.lastPreset = name;
    persistSettings();
    pushToEngine();
}

void GuiApp::openFirstOrUntitled() {
    for (const auto& name : presetNames_) {
        openPreset(name);
        if (hasFile_ && preset_.name == name)
            return;
    }
    startUntitled();
}

// A new preset: its movement loops play on their axes' CCs.
Preset GuiApp::newPreset() {
    Preset p;
    for (LoopBinding& loop : p.base.loops)
        loop.ownCcs = false;
    return p;
}

void GuiApp::startUntitled() {
    preset_ = newPreset();
    hasFile_ = false;
    dirty_ = false;
    presetTime_.reset();
    pushToEngine();
}

bool GuiApp::saveCurrent() {
    if (!hasFile_) {
        nameDialog_ = NameDialog::SaveAs;
        nameInput_ = library_->uniqueName("New mapping");
        nameError_.clear();
        openNameDialog_ = true;
        return false;
    }
    try {
        library_->save(preset_);
    } catch (const std::exception& e) {
        showError(std::string("Cannot save:\n") + e.what());
        return false;
    }
    dirty_ = false;
    presetTime_ = modificationTime(library_->pathFor(preset_.name));
    log("Preset \"" + preset_.name + "\" saved.");
    return true;
}

bool GuiApp::saveAs(const std::string& name) {
    const std::string previous = preset_.name;
    const bool previousHasFile = hasFile_;
    preset_.name = name;
    hasFile_ = true;
    if (!saveCurrent()) {
        preset_.name = previous;
        hasFile_ = previousHasFile;
        return false;
    }
    settings_.lastPreset = name;
    persistSettings();
    refreshPresetList();
    return true;
}

void GuiApp::edited() {
    mappingChanged();
    pushToEngine();
}

bool GuiApp::saveShown() {
    if (songOwnsMapping())
        return saveSessionAs(*sessionName_);
    return saveCurrent();
}

void GuiApp::pushToEngine() {
    if (runtime_)
        runtime_->setPreset(std::make_shared<const Preset>(preset_));
}

void GuiApp::guard(std::function<void()> action) {
    if (!dirty_) {
        action();
        return;
    }
    pendingAction_ = std::move(action);
    openUnsavedDialog_ = true;
}

void GuiApp::setPresetsDir(const fs::path& dir) {
    settings_.presetsDir = dir;
    library_ = std::make_unique<PresetLibrary>(dir);
    refreshPresetList();
    openFirstOrUntitled();
    persistSettings();
    log("Presets: " + dir.string());
}

// --- MIDI output ------------------------------------------------------------

void GuiApp::refreshPorts() {
    ports_.clear();
    try {
        MidiOutput probe(MidiOutput::Api::Default);
        for (auto& name : probe.portNames()) {
            // Skip our own ports and the system ones nobody plays into.
            if ((name.starts_with("Arcadance:") || name.starts_with("MidiPad:")) || name.find("Midi Through") != std::string::npos ||
                name.find("rtpmidid") != std::string::npos || name.find("Announce") != std::string::npos ||
                (name.starts_with("Client-") && name.find("pipewire") == std::string::npos))
                continue;
            ports_.push_back(std::move(name));
        }
    } catch (const std::exception& e) {
        log(std::string("MIDI port list unavailable: ") + e.what());
    }
}

std::shared_ptr<MidiOutput> GuiApp::openOutput(AppSettings::Output kind, const std::string& port, std::string& error) {
    try {
        auto out = std::make_shared<MidiOutput>(MidiOutput::Api::Default);
        if (kind == AppSettings::Output::ExistingPort) {
            outputLabel_ = out->connect(port);
        } else {
            out->openVirtual(settings_.virtualPortName);
            outputLabel_ = "Virtual port \"" + settings_.virtualPortName + "\"";
        }
        return out;
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

bool GuiApp::useOutput(AppSettings::Output kind, const std::string& port) {
    const std::string previousLabel = outputLabel_;
    std::string error;
    auto out = openOutput(kind, port, error);
    if (!out) {
        outputLabel_ = previousLabel;
        showError("MIDI output unavailable:\n" + error);
        return false;
    }
    output_ = out;
    runtime_->setOutput(std::move(out));
    settings_.output = kind;
    settings_.outputChosen = true;
    if (kind == AppSettings::Output::ExistingPort)
        settings_.existingPortName = MidiOutput::stablePortName(port);
    persistSettings();
    log("MIDI output: " + outputLabel_);
    return true;
}

// The snd-virmidi ports among the outputs.
int GuiApp::virmidiPortCount() const {
    return static_cast<int>(std::count_if(ports_.begin(), ports_.end(),
                                          [](const std::string& p) { return !rawMidiName(p).empty(); }));
}

void GuiApp::startVirMidiJob(bool remove) {
    // Our own connection keeps the module busy: let go of it first.
    const bool reload = remove || (virmidiStatus_.loaded && virmidiPorts_ != virmidiPortCount());
    if (reload && settings_.output == AppSettings::Output::ExistingPort && !rawMidiName(outputLabel_).empty())
        useOutput(AppSettings::Output::VirtualPort, {});
    virmidiRemoving_ = remove;
    const int ports = virmidiPorts_;
    const bool atStartup = virmidiAtStartup_;
    virmidiJob_ = std::async(std::launch::async, [remove, ports, atStartup] {
        return remove ? virmidi::remove() : virmidi::create(ports, atStartup);
    });
}

void GuiApp::finishVirMidiJob() {
    if (!virmidiJob_.valid() || virmidiJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const std::string error = virmidiJob_.get();
    refreshPorts();
    virmidiStatus_ = virmidi::status();
    virmidiAtStartup_ = virmidiStatus_.atStartup;
    if (!error.empty()) {
        showError(error);
        return;
    }
    if (virmidiRemoving_) {
        log("VirMIDI ports removed");
        return;
    }
    log("VirMIDI ports created: " + std::to_string(virmidiPortCount()));
    if (outputDialogOpen_) {
        // Created from the startup question: back to the last port used if it
        // was one of them, otherwise the first one.
        closeOutputDialog_ = true;
        if (settings_.output == AppSettings::Output::ExistingPort && !rawMidiName(settings_.existingPortName).empty()) {
            std::string openError;
            if (auto out = openOutput(AppSettings::Output::ExistingPort, settings_.existingPortName, openError)) {
                output_ = out;
                runtime_->setOutput(std::move(out));
                log("MIDI output: " + outputLabel_);
                return;
            }
        }
        settings_.output = AppSettings::Output::VirtualPort;
    }
    // Play into the first one right away, unless already on a VirMIDI port.
    if (settings_.output == AppSettings::Output::VirtualPort)
        for (const auto& port : ports_)
            if (rawMidiName(port) == "Virtual Raw MIDI") {
                useOutput(AppSettings::Output::ExistingPort, port);
                break;
            }
}

// --- Helpers ----------------------------------------------------------------

// Notes for a terminal the window was started from.
void GuiApp::log(const std::string& line) { std::cout << "arcadance: " << line << '\n'; }

void GuiApp::showError(const std::string& message) {
    errorMessage_ = message;
    openErrorDialog_ = true;
    log(message);
}

void GuiApp::persistSettings() {
    try {
        saveSettings(settings_, settingsFile_);
    } catch (const std::exception& e) {
        log(std::string("Settings not saved: ") + e.what());
    }
}

void GuiApp::browseFolder(FolderTarget target, const fs::path& start) {
    {
        std::lock_guard lock(folderMutex_);
        folderTarget_ = target;
    }
    const std::string location = start.string();
    SDL_ShowOpenFolderDialog(&GuiApp::onFolderChosen, this, window_, location.empty() ? nullptr : location.c_str(),
                             false);
}

void SDLCALL GuiApp::onFolderChosen(void* self, const char* const* files, int) {
    // May run on another thread.
    auto* app = static_cast<GuiApp*>(self);
    if (!files || !files[0])
        return;
    std::lock_guard lock(app->folderMutex_);
    app->chosenFolder_ = files[0];
}

// --- Drawing ----------------------------------------------------------------

void GuiApp::draw() {
    const LiveState live = options_.demo ? demoLive(ImGui::GetTime() + 2.0) : runtime_->live();
    setLabelFamily(live.family);  // the buttons take the names of the controller connected
    if (!options_.demo && live.clock == ClockMode::Internal && settings_.clock == AppSettings::Clock::Internal &&
        std::abs(live.tempo - settings_.tempo) > 1e-6) {  // tapped on the controller
        settings_.tempo = live.tempo;
        tempoSaveAt_ = ImGui::GetTime() + 1.0;
    }
    if (tempoSaveAt_ > 0 && ImGui::GetTime() >= tempoSaveAt_) {
        tempoSaveAt_ = 0;
        persistSettings();
    }

    // The page, centered; the top bar's band runs across the whole window.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 page(kPageW * pageScale_, kPageH * pageScale_);
    const ImVec2 pagePos(viewport->WorkPos.x + std::max(0.0f, (viewport->WorkSize.x - page.x) * 0.5f),
                         viewport->WorkPos.y + std::max(0.0f, (viewport->WorkSize.y - page.y) * 0.5f));
    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        const float y1 = pagePos.y + 50.0f * pageScale_, x0 = viewport->WorkPos.x, x1 = x0 + viewport->WorkSize.x;
        const PerformanceView::Look lk = look();
        bg->AddRectFilled(ImVec2(x0, pagePos.y), ImVec2(x1, y1),
                          lk == PerformanceView::Look::Classic ? PerformanceView::Theme::kTopBar
                          : lk == PerformanceView::Look::PhosphorGreen ? IM_COL32(4, 7, 11, 255) : IM_COL32(0x1E, 0x21, 0x25, 255));
        if (lk == PerformanceView::Look::PhosphorGreen) {  // a floor grid in perspective
            const float horizon = pagePos.y + page.y * 0.42f, bottom = pagePos.y + page.y, cx = pagePos.x + page.x * 0.5f;
            for (int k = 1; k < 14; ++k) {
                const float t = static_cast<float>(k) / 14.0f, y = horizon + (bottom - horizon) * t * t;
                bg->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(0x3C, 0xD8, 0xFF, static_cast<int>(10 + 40 * t)), 1.0f);
            }
            for (int k = -12; k <= 12; ++k)
                bg->AddLine(ImVec2(cx + static_cast<float>(k) * page.x * 0.02f, horizon), ImVec2(cx + static_cast<float>(k) * page.x * 0.16f, bottom),
                            IM_COL32(0x3C, 0xD8, 0xFF, 34), 1.0f);
        }
    }
    ImGui::SetNextWindowPos(pagePos);
    ImGui::SetNextWindowSize(page);
    // No background of its own: the clear colour of each frame is the page's
    // (Theme::kBackground), so its pixels are not drawn twice.
    ImGui::Begin("Arcadance", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBackground);
    ImGui::SetScrollY(0.0f);  // the page never scrolls: a wheel turned over a value moves nothing else
    // Escape closes what is open first (a menu, a window, the note editor, a
    // selection); with nothing, it leaves fullscreen.
    const bool escapeFree = !ImGui::GetIO().WantTextInput && !overlaySpec_ && !optionsOpen_ && !menu_ && !notesPanel_ && !editingTempo_ &&
                            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive() && sessionSel_.empty() &&
                            songSel_.empty() && !performance_.escapeBusy();

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
        saveShown();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
        guard([this] { quitAsked(); });
    if (ImGui::Shortcut(ImGuiKey_F11, ImGuiInputFlags_RouteGlobal))
        toggleFullscreen();
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
        undoRedo(false);
    if (!ImGui::GetIO().WantTextInput && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal) ||
                                          ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal)))
        undoRedo(true);
    if (!ImGui::GetIO().WantTextInput && !overlaySpec_ && !optionsOpen_ && ImGui::Shortcut(ImGuiKey_Tab, ImGuiInputFlags_RouteGlobal)) {
        if (sessionView_ || songView_)  // the session screen, then the song, then back
            toggleSong(live);
        else
            toggleSession(live);
    }

    drawPerformance(live);
    if (escapeFree && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN))
        toggleFullscreen();
    ImGui::End();
}

// The tonality, for every channel: a root (or none), then a scale. Drawn in
// the menu the SCALE screen opens.
bool GuiApp::drawKeyChoices() {
    bool changed = false;
    const float column = ImGui::GetFontSize() * 7.5f;
    if (ImGui::BeginTable("##keytable", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("root", ImGuiTableColumnFlags_WidthFixed, column * 0.75f);
        ImGui::TableSetupColumn("scale", ImGuiTableColumnFlags_WidthFixed, column * 1.4f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Root");
        // Choosing a root keeps the list open, so the scale can be picked next.
        if (ImGui::Selectable("None", !settings_.key, ImGuiSelectableFlags_NoAutoClosePopups)) {
            settings_.key.reset();
            changed = true;
        }
        for (int pc = 0; pc < 12; ++pc) {
            const std::string name = pitchName(pc) == pitchName(pc, true) ? pitchName(pc)
                                                                         : pitchName(pc) + " / " + pitchName(pc, true);
            if (ImGui::Selectable(name.c_str(), settings_.key && settings_.key->root == pc,
                                  ImGuiSelectableFlags_NoAutoClosePopups)) {
                settings_.key = Key{pc, settings_.key ? settings_.key->scale : 0};
                changed = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Scale");
        ImGui::BeginDisabled(!settings_.key);
        for (std::size_t i = 0; i < scaleTypes().size(); ++i) {
            const std::string name(scaleTypes()[i].label);
            if (ImGui::Selectable(name.c_str(), settings_.key && settings_.key->scale == i)) {
                settings_.key->scale = i;
                changed = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    return changed;
}

// The cursor of the session screen, there: its channel becomes the active
// one (the one it left records into its current clip again), and Record
// records into its slot.
void GuiApp::moveSessionCursor(int channel, int slot, const LiveState& live) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    slot = std::clamp(slot, 0, Engine::kClipSlots - 1);
    if (channel != sessionChannel_) {
        runtime_->setRecordSlot(sessionChannel_, live.currentClip[static_cast<std::size_t>(sessionChannel_)]);
        runtime_->setChannel(channel);
        sessionChannel_ = channel;
        sessionChannelAsked_ = channel;
    }
    sessionSlot_ = slot;
    runtime_->setRecordSlot(channel, slot);
}

void GuiApp::pressClip(int channel, int slot, const LiveState& live) {
    const auto c = static_cast<std::size_t>(channel);
    const int current = live.currentClip[c];
    const bool playing = (live.clipsPlaying >> channel) & 1u;
    if (playing && (slot == current || live.queuedClip[c] == slot)) {
        if (live.queuedClip[c] >= 0)
            runtime_->launchClip(channel, current);  // calls off what waits
        else
            runtime_->stopClip(channel);
    } else if (live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(slot)].sources) {
        runtime_->launchClip(channel, slot);
    }
}

void GuiApp::copyClips(const std::vector<std::pair<int, int>>& picked, const std::string& name) {
    const auto loops = runtime_->sessionLoops();
    if (!loops || picked.empty())
        return;
    int c0 = kChannelCount, s0 = Engine::kClipSlots;
    for (const auto& [c2, s2] : picked) {
        c0 = std::min(c0, c2);
        s0 = std::min(s0, s2);
    }
    clipClipboard_.clear();
    for (const auto& [c2, s2] : picked) {
        ClipCopy copied{c2 - c0, s2 - s0, {}, clipNames_[static_cast<std::size_t>(c2)][static_cast<std::size_t>(s2)]};
        for (const midipad::SessionLoop& l : *loops)
            if (l.channel == c2 && l.clip == s2)
                copied.loops.push_back(l);
        clipClipboard_.push_back(std::move(copied));
    }
    toast_ = picked.size() > 1 ? "Copied (" + std::to_string(picked.size()) + " clips)" : "Copied (" + name + ")";
    toastUndone_ = false;
    toastPlain_ = true;
    toastUntil_ = ImGui::GetTime() + 2.0;
}

void GuiApp::clearClips(const std::vector<std::pair<int, int>>& picked, const std::string& what) {
    undoWhat_ = what;
    rememberClips(picked, "Clips cleared");
    undoWhat_.clear();
    for (const auto& [c2, s2] : picked) {
        runtime_->clearClip(c2, s2);
        clipNames_[static_cast<std::size_t>(c2)][static_cast<std::size_t>(s2)].clear();
    }
    sessionSel_.clear();
}

void GuiApp::pasteClips(int ch, int slot, const std::string& name) {
    std::vector<std::pair<int, int>> to;
    for (const ClipCopy& copied : clipClipboard_)
        if (ch + copied.channels < kChannelCount && slot + copied.slots < Engine::kClipSlots)
            to.push_back({ch + copied.channels, slot + copied.slots});
    if (to.empty())
        return;
    undoWhat_ = to.size() > 1 ? "Paste (" + std::to_string(to.size()) + " clips)" : "Paste (" + name + ")";
    rememberClips(to, "Paste");
    undoWhat_.clear();
    for (const ClipCopy& copied : clipClipboard_)
        if (ch + copied.channels < kChannelCount && slot + copied.slots < Engine::kClipSlots) {
            runtime_->restoreClip(ch + copied.channels, slot + copied.slots, copied.loops);
            clipNames_[static_cast<std::size_t>(ch + copied.channels)][static_cast<std::size_t>(slot + copied.slots)] = copied.name;
        }
    sessionSel_ = to.size() > 1 ? to : std::vector<std::pair<int, int>>{};
}

void GuiApp::sessionEvent(Engine::SessionEvent e, const LiveState& live) {
    using E = Engine::SessionEvent;
    if (e == E::HelpOn || e == E::HelpOff) {
        helpShown_ = e == E::HelpOn;
        return;
    }
    if (e == E::Edit) {
        toggleEdit(live);
        return;
    }
    if (e == E::Toggle) {  // Mode: the session screen, then the song, then back
        if (editView_)
            toggleEdit(live);
        else if (sessionView_ || songView_)
            toggleSong(live);
        else
            toggleSession(live);
        return;
    }
    if (menu_ && menu_->fromController) {
        menuEvent(e, live);
        return;
    }
    if (editView_) {
        editEvent(e, live);
        return;
    }
    if (songView_) {
        songEvent(e, live);
        return;
    }
    if (!sessionView_)
        return;
    if (e == E::Press && sharedSelectMenu())  // Select = Menu: the press opens the menu
        e = E::Menu;
    if (e == E::Menu) {  // the menu of the clip, step or end under the cursor
        using K = PerformanceView::MenuTarget::Kind;
        if (sessionButton_ >= 0)
            openMenu({K::Channel, sessionChannel_}, live, true, {});
        else if (sessionSlot_ < Engine::kClipSlots)
            openMenu({K::Clip, sessionChannel_, sessionSlot_}, live, true, {});
        else if (chainCursor_ < Engine::kChainSteps)
            openMenu({K::Step, sessionChannel_, chainCursor_}, live, true, {});
        else
            openMenu({K::Chain, sessionChannel_, shownChain(sessionChannel_)}, live, true, {});
        return;
    }
    if (e == E::Back) {  // back to the main screen
        toggleSession(live);
        return;
    }
    if (sessionSlot_ == Engine::kClipSlots) {  // in the chain
        chainEvent(e, live);
        return;
    }
    if (sessionButton_ >= 0) {  // on the stop, mute and solo buttons
        sessionButtonEvent(e, live);
        return;
    }
    switch (e) {
    case E::Up: moveSessionCursor(sessionChannel_, sessionSlot_ - 1, live); break;
    case E::Down:
        if (sessionSlot_ == Engine::kClipSlots - 1)  // past the last clip: the stop button
            sessionButton_ = 0;
        else {
            moveSessionCursor(sessionChannel_, sessionSlot_ + 1, live);
        }
        break;
    // Left of channel 1 is channel 16, right of 16 is 1.
    case E::Left: moveSessionCursor((sessionChannel_ + kChannelCount - 1) % kChannelCount, sessionSlot_, live); break;
    case E::Right: moveSessionCursor((sessionChannel_ + 1) % kChannelCount, sessionSlot_, live); break;
    case E::Press:
    case E::PressRight: pressClip(sessionChannel_, sessionSlot_, live); break;
    case E::ValueLeft:  // the right stick: the channel
    case E::ValueRight:
        moveSessionCursor((sessionChannel_ + (e == E::ValueRight ? 1 : kChannelCount - 1)) % kChannelCount, sessionSlot_, live);
        break;
    default: break;
    }
}

// The buttons under a column: left and right go from one to the next (and
// on to the next channel's), up back to the clips, down into the chain.
void GuiApp::sessionButtonEvent(Engine::SessionEvent e, const LiveState& live) {
    using E = Engine::SessionEvent;
    const int ch = sessionChannel_;
    switch (e) {
    case E::Up:
        sessionButton_ = -1;
        moveSessionCursor(ch, Engine::kClipSlots - 1, live);
        break;
    case E::Down: {
        const LiveState::Chains& chains = live.chains[static_cast<std::size_t>(ch)];
        sessionButton_ = -1;
        sessionSlot_ = Engine::kClipSlots;
        chainCursor_ = chains.at >= 0 && chains.playing == shownChain(ch) ? chains.at : 0;
        break;
    }
    case E::Left:
        if (sessionButton_ > 0) {
            --sessionButton_;
        } else {
            moveSessionCursor((ch + kChannelCount - 1) % kChannelCount, sessionSlot_, live);
            sessionButton_ = 2;
        }
        break;
    case E::Right:
        if (sessionButton_ < 2) {
            ++sessionButton_;
        } else {
            moveSessionCursor((ch + 1) % kChannelCount, sessionSlot_, live);
            sessionButton_ = 0;
        }
        break;
    case E::ValueLeft:
    case E::ValueRight:
        moveSessionCursor((ch + (e == E::ValueRight ? 1 : kChannelCount - 1)) % kChannelCount, sessionSlot_, live);
        break;
    case E::Press:
    case E::PressRight:
        if (sessionButton_ == 0)
            runtime_->stopClip(ch);
        else if (sessionButton_ == 1)
            toggleMute(ch, live);
        else
            toggleSolo(ch, live);
        break;
    default: break;
    }
}

void GuiApp::chainEvent(Engine::SessionEvent e, const LiveState& live) {
    using E = Engine::SessionEvent;
    const int ch = sessionChannel_;
    const LiveState::Chains& chains = live.chains[static_cast<std::size_t>(ch)];
    const int shown = shownChain(ch);
    const LiveState::Chain& chain = chains.list[static_cast<std::size_t>(shown)];
    const bool runs = chains.at >= 0 && chains.playing == shown;
    const bool onCell = chainCursor_ == Engine::kChainSteps;  // the CHAIN cell
    const auto k = static_cast<std::size_t>(std::min(chainCursor_, Engine::kChainSteps - 1));
    const int clip = chain.clip[k], repeats = chain.repeats[k];
    switch (e) {
    case E::Up:  // back to the buttons under the clips
        moveSessionCursor(ch, Engine::kClipSlots - 1, live);
        sessionButton_ = 0;
        break;
    case E::Left: chainCursor_ = std::max(chainCursor_ - 1, 0); break;
    case E::Right: chainCursor_ = std::min(chainCursor_ + 1, Engine::kChainSteps); break;
    case E::Press:  // Select: start there; on the step playing, go on; on the CHAIN cell, play or stop the chain
        if (onCell) {
            if (runs)
                runtime_->stopChain(ch);
            else
                runtime_->chainGoTo(ch, shown, 0);
        } else if (runs && chains.at == chainCursor_) {
            runtime_->chainGoTo(ch, shown, (chains.at + 1) % Engine::kChainSteps);
        } else {
            runtime_->chainGoTo(ch, shown, chainCursor_);
        }
        break;
    case E::PressRight:  // R3: empty the step
        if (!onCell && clip >= 0)
            setChainStep(ch, chainCursor_, -1, repeats);
        break;
    case E::ValueUp:
    case E::ValueDown:
        if (onCell)  // another chain: up the previous, down the next
            showChain(ch, shown + (e == E::ValueUp ? -1 : 1));
        else
            setChainStep(ch, chainCursor_, std::clamp(clip + (e == E::ValueUp ? 1 : -1), -1, Engine::kChainSilence), clip >= 0 ? repeats : 1);
        break;
    case E::ValueLeft:
    case E::ValueRight:
        if (onCell) {  // what comes after its last step: back to step 1, stop, chain A, B...
            std::vector<int> choices{Engine::kThenLoop, Engine::kThenStop};
            for (int n = 0; n < Engine::kChains; ++n)
                if (n != shown)
                    choices.push_back(n);
            const auto it = std::find(choices.begin(), choices.end(), static_cast<int>(chain.then));
            const int at = it == choices.end() ? 0 : static_cast<int>(it - choices.begin());
            const int count = static_cast<int>(choices.size());
            runtime_->setChainThen(ch, shown, choices[static_cast<std::size_t>((at + (e == E::ValueRight ? 1 : count - 1)) % count)]);
        } else if (clip >= 0) {
            setChainStep(ch, chainCursor_, clip, PerformanceView::nextRepeats(repeats, e == E::ValueRight ? 1 : -1));
        }
        break;
    default: break;
    }
}

void GuiApp::toggleEdit(const LiveState& live) {
    if (sessionView_)
        toggleSession(live);
    songView_ = false;
    editView_ = !editView_;
    menu_.reset();
    runtime_->setSessionMode(sessionView_ || editView_ || songView_);
}

// In edit mode: the cursor on the main view, or, while a window is open, the
// window's fields (ImGui's gamepad navigation, fed from here).
void GuiApp::editEvent(Engine::SessionEvent e, const LiveState& live) {
    using E = Engine::SessionEvent;
    ImGuiIO& io = ImGui::GetIO();
    auto tap = [&](ImGuiKey key) {
        io.AddKeyEvent(key, true);
        io.AddKeyEvent(key, false);
    };
    if (overlaySpec_ || optionsOpen_ || notesPanel_) {  // a window or the note editor: its fields
        switch (e) {
        case E::Up: tap(ImGuiKey_GamepadDpadUp); break;
        case E::Down: tap(ImGuiKey_GamepadDpadDown); break;
        case E::Left: tap(ImGuiKey_GamepadDpadLeft); break;
        case E::Right: tap(ImGuiKey_GamepadDpadRight); break;
        case E::Press: tap(ImGuiKey_GamepadFaceDown); break;
        case E::ValueLeft: tap(ImGuiKey_GamepadDpadLeft); break;
        case E::ValueRight: tap(ImGuiKey_GamepadDpadRight); break;
        case E::Back:
            if (ImGui::IsAnyItemActive()) {
                tap(ImGuiKey_GamepadFaceRight);
            } else if (overlaySpec_ || optionsOpen_) {
                overlaySpec_.reset();
                optionsOpen_ = false;
            } else {
                notesPanel_ = false;
            }
            break;
        default: break;
        }
        return;
    }
    using K = PerformanceView::MenuTarget::Kind;
    const auto target = performance_.focusTarget();
    switch (e) {
    case E::Up: performance_.moveFocus(0, -1); break;
    case E::Down: performance_.moveFocus(0, 1); break;
    case E::Left: performance_.moveFocus(-1, 0); break;
    case E::Right: performance_.moveFocus(1, 0); break;
    case E::Press:
    case E::PressRight:  // the CHANNEL block has only its menu; with Select = Menu, every item does
        if (target && (sharedSelectMenu() || target->kind == K::ChannelBlock))
            openMenu(*target, live, true, {});
        else
            performance_.activateFocus();
        break;
    case E::Menu:
        if (target)
            openMenu(*target, live, true, {});
        break;
    case E::ValueLeft:  // the right stick: the channel, wherever the cursor is
    case E::ValueRight: runtime_->setChannel((live.channel + (e == E::ValueRight ? 1 : kChannelCount - 1)) % kChannelCount); break;
    case E::ValueUp:  // and the value under the cursor: a lane's length
    case E::ValueDown:
        if (target && target->kind == K::Lane)
            runtime_->stretchLoop(target->source, target->axis, e == E::ValueUp ? 1 : -1);
        break;
    case E::Back: toggleEdit(live); break;
    default: break;
    }
}

void GuiApp::toggleSession(const LiveState& live) {
    if (editView_ && !sessionView_) {
        editView_ = false;
        menu_.reset();
    }
    songView_ = false;
    sessionView_ = !sessionView_;
    runtime_->setSessionMode(sessionView_ || editView_ || songView_);
    if (sessionView_) {
        sessionChannel_ = live.channel;
        sessionSlot_ = live.recordClip[static_cast<std::size_t>(live.channel)];
    } else {
        for (int ch = 0; ch < kChannelCount; ++ch)
            runtime_->setRecordSlot(ch, live.currentClip[static_cast<std::size_t>(ch)]);
    }
}

void GuiApp::drawNotesPanel(const LiveState& live) {
    using Theme = PerformanceView::Theme;
    const auto [a, b] = performance_.panelRect();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    auto px = [em](float v) { return v * em / 16.0f; };
    ImFont* bold = uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont();
    ImFont* medium = uiFonts_.medium ? uiFonts_.medium : ImGui::GetFont();
    const int ch = std::clamp(live.channel, 0, kChannelCount - 1);
    const auto c = static_cast<std::size_t>(ch);
    constexpr auto kNotes = static_cast<std::size_t>(LoopSource::Notes);
    const LoopStatus& st = live.loops[kNotes].status;
    const bool has = st.state == LoopStatus::State::Playing && st.length > 1;
    dl->AddRectFilled(a, b, Theme::kPanel, px(8));
    dl->AddRect(a, b, Theme::kBorder, px(8), px(1));
    // The title: what is edited.
    const float hy = a.y + px(10), hh = px(30), cy = hy + hh * 0.5f;
    float x = a.x + px(14);
    auto label = [&](ImFont* f, float size, ImU32 col, const std::string& t) {
        const ImVec2 sz = f->CalcTextSizeA(px(size), FLT_MAX, 0, t.c_str());
        dl->AddText(f, px(size), ImVec2(x, cy - sz.y * 0.5f), col, t.c_str());
        x += sz.x + px(12);
    };
    label(bold, 14, Theme::kText, "NOTES");
    const std::string& chName = preset_.channelNames[c];
    label(bold, 14, performance_.channelColor(ch), std::to_string(ch + 1) + (chName.empty() ? "" : " " + chName));
    std::string what = clipName(ch, live.currentClip[c]);
    if (has) {
        char len[32];
        if (st.beats > 0)
            std::snprintf(len, sizeof len, "%g %s", st.beats / std::max(live.beatsPerBar, 1), st.beats == std::max(live.beatsPerBar, 1) ? "bar" : "bars");
        else
            std::snprintf(len, sizeof len, "%.1f s", static_cast<double>(st.length) / Engine::kLoopRateHz);
        what += "  \xC2\xB7  " + std::string(len);
    }
    label(bold, 14, Theme::kLabel, what);
    // Its settings, right-aligned: the rows (SCALE or CHROMATIC), the grid, QUANTIZE, playback, CLEAR, close.
    const auto bit = static_cast<std::uint16_t>(1u << ch);
    const bool chromatic = (preset_.chromaticChannels & bit) != 0;
    LoopPlayback playback = preset_.forChannel(ch).loops[kNotes].playback;
    static constexpr std::pair<int, const char*> kGrids[] = {{0, "No grid"}, {2, "1/8"}, {3, "1/8 T"}, {4, "1/16"}, {6, "1/16 T"}, {8, "1/32"}};
    const char* gridName = "1/16";
    for (const auto& [steps, name] : kGrids)
        if (steps == performance_.editorGrid())
            gridName = name;
    const char* playName = playback == LoopPlayback::Reverse ? "REV" : playback == LoopPlayback::BackAndForth ? "B&F" : "FWD";
    ImGui::PushFont(bold, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(10), px(6)));
    auto width = [&](const char* t) { return ImGui::CalcTextSize(t).x + px(20); };
    const float gap = px(6);
    const float total = width("SCALE") + width("CHROMATIC") + gap + width(gridName) + px(14) + gap + width("QUANTIZE") + gap + width(playName) + px(14) + gap +
                        width("CLEAR") + gap + px(30);
    float rx = std::max(b.x - px(12) - total, x + px(8));
    auto at = [&](float w) {
        ImGui::SetCursorScreenPos(ImVec2(rx, hy));
        rx += w + gap;
    };
    auto toggle = [&](const char* t, bool on) {
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.24f, 0.86f, 0.52f, 0.30f) : ImGui::ColorConvertU32ToFloat4(Theme::kControl));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(on ? Theme::kGreen : Theme::kControlBorder));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(on ? Theme::kGreen : Theme::kLabel));
        const bool clicked = ImGui::Button(t, ImVec2(width(t), hh));
        ImGui::PopStyleColor(3);
        return clicked;
    };
    at(width("SCALE"));
    if (toggle("SCALE", !chromatic) && chromatic) {
        preset_.chromaticChannels = static_cast<std::uint16_t>(preset_.chromaticChannels ^ bit);
        edited();
    }
    ImGui::SetItemTooltip("Notes move on the tonality's degrees%s", settings_.key ? ("  (" + pitchName(settings_.key->root) + " " + std::string(settings_.key->type().label) + ")").c_str() : "");
    rx -= gap - px(2);
    at(width("CHROMATIC"));
    if (toggle("CHROMATIC", chromatic) && !chromatic) {
        preset_.chromaticChannels = static_cast<std::uint16_t>(preset_.chromaticChannels ^ bit);
        edited();
    }
    ImGui::SetItemTooltip("Notes move by semitones, every row named (drums)");
    // A chip with a little arrow: a menu of its choices.
    auto chip = [&](const char* id, const char* t, ImU32 col) {
        const float w = width(t) + px(14);
        at(w);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        const bool clicked = ImGui::Button((std::string(t) + "      ##" + id).c_str(), ImVec2(w, hh));
        ImGui::PopStyleColor();
        const float tx = p.x + w - px(14), ty = p.y + hh * 0.5f;
        dl->AddTriangleFilled(ImVec2(tx - px(4), ty - px(2)), ImVec2(tx + px(4), ty - px(2)), ImVec2(tx, ty + px(3)), Theme::kLabel);
        if (clicked)
            ImGui::OpenPopup(id);
        return ImGui::BeginPopup(id);
    };
    if (chip("##grid", gridName, Theme::kLabel)) {
        for (const auto& [steps, name] : kGrids)
            if (ImGui::Selectable(name, steps == performance_.editorGrid()))
                performance_.setEditorGrid(steps);
        ImGui::EndPopup();
    }
    ImGui::SetItemTooltip("The grid notes move, stretch, are added and quantized on");
    at(width("QUANTIZE"));
    ImGui::BeginDisabled(!has);
    if (ImGui::Button("QUANTIZE", ImVec2(width("QUANTIZE"), hh)))
        performance_.askQuantize();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The selected notes, or all of them, on the grid (Q)");
    if (chip("##playback", playName, Theme::kViolet)) {
        static constexpr std::pair<LoopPlayback, const char*> kWays[] = {
            {LoopPlayback::Forward, "Forward"}, {LoopPlayback::Reverse, "Reverse"}, {LoopPlayback::BackAndForth, "Back & forth"}};
        for (const auto& [way, name] : kWays)
            if (ImGui::Selectable(name, way == playback) && way != playback) {
                // Channel 1 edits the base mapping; another channel's first change gives it its own.
                auto& own = preset_.channels[c];
                if (!own && ch != 0)
                    own = preset_.base;
                (own ? *own : preset_.base).loops[kNotes].playback = way;
                edited();
            }
        ImGui::EndPopup();
    }
    ImGui::SetItemTooltip("How the note loop plays");
    at(width("CLEAR"));
    ImGui::BeginDisabled(!has && st.state == LoopStatus::State::Empty);
    if (ImGui::Button("CLEAR", ImVec2(width("CLEAR"), hh))) {
        rememberClips({{ch, live.currentClip[c]}}, "Note loop cleared");
        runtime_->clearLoop(static_cast<int>(LoopSource::Notes), -1);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Clears the note loop");
    at(px(30));
    const ImVec2 xp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##closenotes", ImVec2(px(30), hh)))
        notesPanel_ = false;
    const ImU32 xc = ImGui::IsItemHovered() ? Theme::kText : Theme::kLabel;
    const ImVec2 xm(xp.x + px(15), xp.y + hh * 0.5f);
    dl->AddLine(ImVec2(xm.x - px(6), xm.y - px(6)), ImVec2(xm.x + px(6), xm.y + px(6)), xc, px(2));
    dl->AddLine(ImVec2(xm.x - px(6), xm.y + px(6)), ImVec2(xm.x + px(6), xm.y - px(6)), xc, px(2));
    ImGui::SetItemTooltip("Close (Esc)");
    ImGui::PopStyleVar();
    ImGui::PopFont();
    // The editor: the roll, its scrollbar and the velocity / chance lane.
    const float ey = hy + hh + px(10);
    ImGui::SetCursorScreenPos(ImVec2(a.x + px(10), ey));
    performance_.setScaleMoves(!chromatic);
    ImGui::PushFont(medium, 13.0f);
    performance_.drawNoteEditor(preset_.forChannel(ch), live, b.x - a.x - px(20), b.y - ey - px(12));
    ImGui::PopFont();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !performance_.tookEscape() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        !ImGui::GetIO().WantTextInput)
        notesPanel_ = false;
}

void GuiApp::showView(int view, const LiveState& live) {
    if (view == 0) {
        if (sessionView_)
            toggleSession(live);
        else if (songView_)
            toggleSong(live);
        else if (editView_)
            toggleEdit(live);
    } else if (view == 1 && !sessionView_) {
        toggleSession(live);
    } else if (view == 2 && !songView_) {
        toggleSong(live);
    }
}

// The song screen, from the session screen (Mode, Tab, the SONG button); again: the main screen.
void GuiApp::toggleSong(const LiveState& live) {
    if (editView_) {
        editView_ = false;
        menu_.reset();
    }
    if (sessionView_)
        toggleSession(live);  // (it closes the song: open it after)
    songView_ = !songView_;
    menu_.reset();
    songCursor_.editing = false;
    songCursor_.row = std::clamp(songCursor_.row, 0, static_cast<int>(song_.sections.size()));
    runtime_->setSessionMode(sessionView_ || editView_ || songView_);
}

std::vector<midipad::SongItem>* GuiApp::songCell(int row, int col) {
    if (row < 0 || row >= static_cast<int>(song_.sections.size()) || col < 0 || col >= kChannelCount)
        return nullptr;
    return &song_.sections[static_cast<std::size_t>(row)].cells[static_cast<std::size_t>(col)];
}

int GuiApp::placeSongItem(int row, int col, midipad::SongItem item, const LiveState& live) {
    auto* items = songCell(row, col);
    if (!items)
        return 0;
    const int bpb = std::max(live.beatsPerBar, 1);
    item.bar = std::clamp(item.bar, 0, midipad::kMaxSectionBars - 1);
    item.beat = std::clamp(item.beat, 0, bpb - 1);
    item.after = false;
    const auto starts = PerformanceView::songItemBars(live, col, *items);
    const double start = item.bar + static_cast<double>(item.beat) / bpb;
    std::size_t at = 0;
    while (at < starts.size() && starts[at] <= start + 1e-6)
        ++at;
    items->insert(items->begin() + static_cast<std::ptrdiff_t>(at), item);
    return static_cast<int>(at);
}

// A new item after the cell's last, from where the last one ends.
void GuiApp::addSongItem(int row, int col, const LiveState& live, midipad::SongItem::Kind kind, int index) {
    auto* items = songCell(row, col);
    if (!items || static_cast<int>(items->size()) >= midipad::kMaxSongItems)
        return;
    midipad::SongItem item;
    item.kind = kind;
    item.index = index;
    item.after = !items->empty();  // glued to the last one
    if (item.after)
        item.bar = static_cast<int>(PerformanceView::songItemBars(live, col, *items).back());
    items->push_back(item);
    songCursor_.item = static_cast<int>(items->size()) - 1;
    songCursor_.editing = true;
    songEdited();
}

// The clips that hold loops and the chains that have steps on the channel, to pick from.
void GuiApp::openSongPick(int row, int col, int item, const LiveState& live, bool fromController, ImVec2 at, int bar) {
    if (!songCell(row, col))
        return;
    openMenu({PerformanceView::MenuTarget::Kind::SongPick, col, row, bar, item}, live, fromController, at);
}

void GuiApp::playSongFrom(int section, const LiveState& live) {
    if (live.song.playing) {
        runtime_->playSong(section);
        return;
    }
    if (live.loopChannels != 0 && !live.loopsPaused)
        runtime_->transportStop();
    runtime_->transportPlay(section);
}

// --- Undo and redo -----------------------------------------------------------

void GuiApp::pushUndo(UndoStep step) {
    step.at = ImGui::GetTime();
    UndoStep* top = undo_.empty() ? nullptr : &undo_.back();
    if (top && step.kind != UndoStep::Kind::Clips && top->kind == step.kind && top->channel == step.channel && top->what == step.what &&
        step.at - top->at < 1.0) {
        top->at = step.at;  // the same edit again (a wheel, a stick held): one step
    } else {
        undo_.push_back(std::move(step));
        if (undo_.size() > 100)
            undo_.erase(undo_.begin());
    }
    redo_.clear();
    toast_ = undo_.back().what;
    toastUndone_ = false;
    toastPlain_ = false;
    toastUntil_ = ImGui::GetTime() + 4.0;
}

// The song changed (songEdited): a step with the song as it was.
void GuiApp::songUndo() {
    if (song_ == songShadow_)
        return;
    UndoStep step;
    step.what = undoWhat_.empty() ? "Song changed" : undoWhat_;
    step.song = songShadow_;
    songShadow_ = song_;
    pushUndo(std::move(step));
}

// A channel's chains about to change: a step with them as they are.
void GuiApp::rememberChains(int channel, const char* what) {
    UndoStep step;
    step.kind = UndoStep::Kind::Chains;
    step.what = undoWhat_.empty() ? what : undoWhat_;
    step.channel = std::clamp(channel, 0, kChannelCount - 1);
    step = currentState(step);
    pushUndo(std::move(step));
}

// Clips about to change: a step with their loops as they are.
void GuiApp::rememberClips(std::vector<std::pair<int, int>> clips, const char* what) {
    UndoStep step;
    step.kind = UndoStep::Kind::Clips;
    step.what = undoWhat_.empty() ? what : undoWhat_;
    step.clips = std::move(clips);
    step = currentState(step);
    pushUndo(std::move(step));
}

// What `like` keeps (the song, its channel's chains, its clips), as it is now.
GuiApp::UndoStep GuiApp::currentState(const UndoStep& like) {
    UndoStep now = like;
    const auto c = static_cast<std::size_t>(like.channel);
    switch (like.kind) {
    case UndoStep::Kind::Song: now.song = song_; break;
    case UndoStep::Kind::Chains:
        now.chains = liveChains_[c].list;
        now.chainNames = chainNames_[c];
        break;
    case UndoStep::Kind::Clips:
        now.clipNames.clear();
        for (const auto& [channel, slot] : like.clips)
            now.clipNames.push_back(clipNames_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]
                                              [static_cast<std::size_t>(std::clamp(slot, 0, Engine::kClipSlots - 1))]);
        now.loops.clear();
        if (const auto loops = runtime_->sessionLoops())
            for (const midipad::SessionLoop& l : *loops)
                if (std::find(like.clips.begin(), like.clips.end(), std::pair{l.channel, l.clip}) != like.clips.end())
                    now.loops.push_back(l);
        break;
    }
    return now;
}

void GuiApp::applyState(const UndoStep& state) {
    const auto c = static_cast<std::size_t>(state.channel);
    switch (state.kind) {
    case UndoStep::Kind::Song:
        song_ = state.song;
        songShadow_ = song_;
        runtime_->setSong(song_);
        songCursor_.row = std::clamp(songCursor_.row, 0, static_cast<int>(song_.sections.size()));
        songCursor_.editing = false;
        break;
    case UndoStep::Kind::Chains:
        for (int k = 0; k < Engine::kChains; ++k) {
            const LiveState::Chain& chain = state.chains[static_cast<std::size_t>(k)];
            for (int i = 0; i < Engine::kChainSteps; ++i)
                runtime_->setChainStep(state.channel, k, i, chain.clip[static_cast<std::size_t>(i)], chain.repeats[static_cast<std::size_t>(i)]);
            runtime_->setChainThen(state.channel, k, chain.then);
        }
        chainNames_[c] = state.chainNames;
        break;
    case UndoStep::Kind::Clips:
        for (std::size_t i = 0; i < state.clips.size() && i < state.clipNames.size(); ++i) {
            const auto& [channel, slot] = state.clips[i];
            if (channel < 0 || channel >= kChannelCount || slot < 0 || slot >= Engine::kClipSlots)
                continue;
            clipNames_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)] = state.clipNames[i];
        }
        for (const auto& [channel, slot] : state.clips) {
            std::vector<midipad::SessionLoop> loops;
            for (const midipad::SessionLoop& l : state.loops)
                if (l.channel == channel && l.clip == slot)
                    loops.push_back(l);
            runtime_->restoreClip(channel, slot, std::move(loops));
        }
        break;
    }
}

void GuiApp::undoRedo(bool redo) {
    auto& from = redo ? redo_ : undo_;
    auto& to = redo ? undo_ : redo_;
    if (from.empty())
        return;
    UndoStep step = std::move(from.back());
    from.pop_back();
    UndoStep now = currentState(step);
    applyState(step);
    to.push_back(std::move(now));
    toast_ = (redo ? "Redone: " : "Undone: ") + step.what;
    toastPlain_ = false;
    toastUndone_ = !redo;
    toastUntil_ = ImGui::GetTime() + 4.0;
}

void GuiApp::launchRow(int slot, const LiveState& live) {
    if (slot < 0 || slot >= Engine::kClipSlots)
        return;
    // Every clip of the row playing it (or about to): the row stops. Otherwise its
    // clips are launched, and the channels without one there stop.
    bool any = false, all = true;
    for (int ch = 0; ch < kChannelCount; ++ch) {
        const auto c = static_cast<std::size_t>(ch);
        if (!live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(slot)].sources)
            continue;
        any = true;
        const bool plays = ((live.clipsPlaying >> ch) & 1u) && live.currentClip[c] == slot;
        all = all && (plays || live.queuedClip[c] == slot);
    }
    if (!any)
        return;
    for (int ch = 0; ch < kChannelCount; ++ch) {
        const auto c = static_cast<std::size_t>(ch);
        const bool here = live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(slot)].sources;
        const bool busy = ((live.clipsPlaying >> ch) & 1u) || live.queuedClip[c] >= 0 || live.chains[c].at >= 0;
        if (all) {
            if (here && busy)
                runtime_->stopClip(ch);
        } else if (here) {
            const bool plays = ((live.clipsPlaying >> ch) & 1u) && live.currentClip[c] == slot;
            if (!plays && live.queuedClip[c] != slot)
                runtime_->launchClip(ch, slot);
        } else if (busy) {
            runtime_->stopClip(ch);
        }
    }
}

void GuiApp::captureRow(const LiveState& live) {
    // What plays: a clip per channel (stopped, nothing).
    std::vector<std::pair<int, int>> playing;
    for (int ch = 0; ch < kChannelCount; ++ch)
        if (((live.clipsPlaying >> ch) & 1u) && !live.loopsPaused)
            playing.push_back({ch, live.currentClip[static_cast<std::size_t>(ch)]});
    auto toast = [&](const char* what) {
        toast_ = what;
        toastUndone_ = false;
        toastPlain_ = true;
        toastUntil_ = ImGui::GetTime() + 2.5;
    };
    if (playing.empty())
        return toast("Nothing plays to capture");
    int row = -1;  // the first row with no clip on any channel
    for (int k = 0; k < Engine::kClipSlots && row < 0; ++k) {
        bool empty = true;
        for (int ch = 0; ch < kChannelCount && empty; ++ch)
            empty = live.clips[static_cast<std::size_t>(ch * Engine::kClipSlots + k)].sources == 0;
        if (empty)
            row = k;
    }
    if (row < 0)
        return toast("No empty row to capture into");
    std::vector<std::pair<int, int>> into;
    for (const auto& [ch, slot] : playing)
        into.push_back({ch, row});
    undoWhat_ = "Capture (row " + std::to_string(row + 1) + ")";
    rememberClips(into, "Capture");
    undoWhat_.clear();
    for (const auto& [ch, slot] : playing) {
        runtime_->copyClip(ch, slot, row);
        clipNames_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(row)] = clipNames_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(slot)];
    }
}

void GuiApp::sessionDrop(const PerformanceView::SessionAction& a, const LiveState& live) {
    using Kind = PerformanceView::SessionAction::Kind;
    const int ch = std::clamp(a.channel, 0, kChannelCount - 1);
    if (a.kind == Kind::ClipDrop) {  // moved there (what was there comes here), or copied
        if (a.fromChannel < 0 || a.fromSlot < 0 || (a.fromChannel == ch && a.fromSlot == a.slot))
            return;
        const auto loops = runtime_->sessionLoops();
        if (!loops)
            return;
        const std::pair<int, int> grabbed{a.fromChannel, a.fromSlot};
        if (sessionSel_.size() > 1 && std::find(sessionSel_.begin(), sessionSel_.end(), grabbed) != sessionSel_.end()) {
            // The selection, all moved (or copied) as the clip dragged: onto free slots only.
            const int dc = ch - a.fromChannel, ds = a.slot - a.fromSlot;
            std::vector<std::pair<int, int>> to;
            for (const auto& [c2, s2] : sessionSel_) {
                const std::pair<int, int> t{c2 + dc, s2 + ds};
                const bool free = std::none_of(loops->begin(), loops->end(), [&](const midipad::SessionLoop& l) { return l.channel == t.first && l.clip == t.second; });
                const bool moving = !a.copy && std::find(sessionSel_.begin(), sessionSel_.end(), t) != sessionSel_.end();
                if (t.first < 0 || t.first >= kChannelCount || t.second < 0 || t.second >= Engine::kClipSlots || !(free || moving)) {
                    toast_ = "Not there: some clips would be overwritten";
                    toastUndone_ = false;
                    toastPlain_ = true;
                    toastUntil_ = ImGui::GetTime() + 2.5;
                    return;
                }
                to.push_back(t);
            }
            std::vector<std::pair<int, int>> touched = sessionSel_;
            touched.insert(touched.end(), to.begin(), to.end());
            undoWhat_ = std::string(a.copy ? "Copy" : "Move") + " (" + std::to_string(to.size()) + " clips)";
            rememberClips(touched, "Clips moved");
            undoWhat_.clear();
            auto loopsOf = [&](const std::pair<int, int>& clip) {
                std::vector<midipad::SessionLoop> out;
                for (const midipad::SessionLoop& l : *loops)
                    if (l.channel == clip.first && l.clip == clip.second)
                        out.push_back(l);
                return out;
            };
            std::vector<std::string> names;  // their names go with them
            for (const auto& from : sessionSel_)
                names.push_back(clipNames_[static_cast<std::size_t>(from.first)][static_cast<std::size_t>(from.second)]);
            if (!a.copy)
                for (const auto& from : sessionSel_)
                    if (std::find(to.begin(), to.end(), from) == to.end()) {
                        runtime_->restoreClip(from.first, from.second, {});
                        clipNames_[static_cast<std::size_t>(from.first)][static_cast<std::size_t>(from.second)].clear();
                    }
            for (std::size_t k = 0; k < to.size(); ++k) {
                runtime_->restoreClip(to[k].first, to[k].second, loopsOf(sessionSel_[k]));
                clipNames_[static_cast<std::size_t>(to[k].first)][static_cast<std::size_t>(to[k].second)] = names[k];
            }
            sessionSel_ = to;
            moveSessionCursor(ch, a.slot, live);
            return;
        }
        std::vector<midipad::SessionLoop> from, to;
        for (const midipad::SessionLoop& l : *loops) {
            if (l.channel == a.fromChannel && l.clip == a.fromSlot)
                from.push_back(l);
            else if (l.channel == ch && l.clip == a.slot)
                to.push_back(l);
        }
        auto name = [&](int channel, int slot) {
            return "clip " + std::to_string(slot + 1) + (channel != a.fromChannel || channel != ch ? " of channel " + std::to_string(channel + 1) : "");
        };
        undoWhat_ = std::string(a.copy ? "Copy" : to.empty() ? "Move" : "Swap") + " (" + name(a.fromChannel, a.fromSlot) + " to " + name(ch, a.slot) + ")";
        rememberClips({{a.fromChannel, a.fromSlot}, {ch, a.slot}}, "Clip moved");
        undoWhat_.clear();
        runtime_->restoreClip(ch, a.slot, from);
        if (!a.copy)
            runtime_->restoreClip(a.fromChannel, a.fromSlot, to);
        {  // the names go with them
            std::string& fromName = clipNames_[static_cast<std::size_t>(a.fromChannel)][static_cast<std::size_t>(a.fromSlot)];
            std::string& toName = clipNames_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(a.slot)];
            const std::string moved = fromName;
            if (!a.copy)
                fromName = to.empty() ? std::string() : toName;
            toName = moved;
        }
        moveSessionCursor(ch, a.slot, live);
        return;
    }
    // A chain's steps: one moved, two swapped, or a clip put in between.
    const int shown = shownChain(ch);
    const LiveState::Chain& chain = liveChains_[static_cast<std::size_t>(ch)].list[static_cast<std::size_t>(shown)];
    std::vector<std::pair<int, int>> steps;
    for (int k = 0; k < Engine::kChainSteps; ++k)
        steps.push_back({chain.clip[static_cast<std::size_t>(k)], chain.repeats[static_cast<std::size_t>(k)]});
    int cursor = a.slot;
    if (a.kind == Kind::ClipInsert) {
        if (steps.back().first >= 0 || a.clip < 0) {  // no room left: the last step would be lost
            toast_ = "The chain is full";
            toastUndone_ = false;
            toastPlain_ = true;
            toastUntil_ = ImGui::GetTime() + 2.0;
            return;
        }
        steps.insert(steps.begin() + std::clamp(a.slot, 0, Engine::kChainSteps), {a.clip, 1});
        steps.pop_back();
        undoWhat_ = "Insert (clip " + std::to_string(a.clip + 1) + ")";
    } else if (a.fromSlot >= 0 && a.fromSlot < Engine::kChainSteps) {
        if (a.onStep) {
            std::swap(steps[static_cast<std::size_t>(a.fromSlot)], steps[static_cast<std::size_t>(std::clamp(a.slot, 0, Engine::kChainSteps - 1))]);
        } else {
            const auto moved = steps[static_cast<std::size_t>(a.fromSlot)];
            steps.erase(steps.begin() + a.fromSlot);
            cursor = std::clamp(a.slot > a.fromSlot ? a.slot - 1 : a.slot, 0, Engine::kChainSteps - 1);
            steps.insert(steps.begin() + cursor, moved);
        }
        undoWhat_ = "Move (step " + std::to_string(a.fromSlot + 1) + ")";
    } else {
        return;
    }
    rememberChains(ch, "Chain changed");
    undoWhat_.clear();
    for (int k = 0; k < Engine::kChainSteps; ++k)
        runtime_->setChainStep(ch, shown, k, steps[static_cast<std::size_t>(k)].first, steps[static_cast<std::size_t>(k)].second);
    sessionSlot_ = Engine::kClipSlots;
    chainCursor_ = std::clamp(cursor, 0, Engine::kChainSteps - 1);
}

// A session opened or a new song: nothing to undo.
void GuiApp::resetUndo() {
    undo_.clear();
    redo_.clear();
    songShadow_ = song_;
    toastUntil_ = 0;
}

void GuiApp::addSongSection(int at) {
    at = std::clamp(at, 0, static_cast<int>(song_.sections.size()));
    midipad::SongSection section;
    section.name = "SECTION " + std::to_string(song_.sections.size() + 1);
    song_.sections.insert(song_.sections.begin() + at, section);
    songCursor_.row = at;
    songCursor_.col = -1;
    songCursor_.editing = false;
    songEdited();
}

void GuiApp::songAction(const PerformanceView::SongAction& a, const LiveState& live) {
    using K = PerformanceView::SongAction::Kind;
    auto& c = songCursor_;
    auto* items = songCell(a.row, a.col);
    const bool hasItem = items && a.item >= 0 && a.item < static_cast<int>(items->size());
    switch (a.kind) {
    case K::Cursor:
        c.row = a.row;
        c.col = a.col;
        c.editing = false;
        break;
    case K::Play:
        c.row = a.row;
        playSongFrom(a.row, live);
        break;
    case K::Stop: runtime_->stopSong(); break;
    case K::Loop: runtime_->setSongLoop(a.value != 0); break;
    case K::LoopSong:
        song_.loop = a.value != 0;
        songEdited();
        break;
    case K::AddSection: addSongSection(static_cast<int>(song_.sections.size())); break;
    case K::AddFromRow: {  // a section playing a row of the session screen: its clips, as long as the longest
        addSongSection(static_cast<int>(song_.sections.size()));
        midipad::SongSection& sec = song_.sections.back();
        const int row = std::clamp(a.value, 0, Engine::kClipSlots - 1);
        if (!rowNames_[static_cast<std::size_t>(row)].empty())
            sec.name = rowNames_[static_cast<std::size_t>(row)];
        sec.fit = true;
        for (int ch = 0; ch < kChannelCount; ++ch)
            if (live.clips[static_cast<std::size_t>(ch * Engine::kClipSlots + row)].sources != 0) {
                midipad::SongItem item;
                item.kind = midipad::SongItem::Kind::Clip;
                item.index = row;
                sec.cells[static_cast<std::size_t>(ch)].push_back(item);
            }
        undoWhat_ = "Section from row " + std::to_string(row + 1);
        songEdited();
        break;
    }
    case K::LoopRange:
        songLoopFrom_ = a.row;
        songLoopTo_ = a.row < 0 ? -1 : a.value;
        runtime_->setSongLoopRange(songLoopFrom_, songLoopTo_);
        if (songLoopFrom_ >= 0 && live.song.loop)  // one loop at a time
            runtime_->setSongLoop(false);
        break;
    case K::StartAt: songStartBar_ = a.value; break;
    case K::Times:  // the times a section plays in a row
        if (a.row >= 0 && a.row < static_cast<int>(song_.sections.size())) {
            auto& sec = song_.sections[static_cast<std::size_t>(a.row)];
            const int times = std::clamp(std::max(sec.repeats, 1) + a.value, 1, midipad::kMaxSectionRepeats);
            if (times != sec.repeats) {
                sec.repeats = times;
                undoWhat_ = "Times played (section " + std::to_string(a.row + 1) + ")";
                songEdited();
            }
        }
        break;
    case K::TimesAsk:
        if (a.row >= 0 && a.row < static_cast<int>(song_.sections.size())) {
            valueAsk_ = ValueAsk{ValueAsk::Kind::SectionRepeats, -1, a.row, 0, std::max(song_.sections[static_cast<std::size_t>(a.row)].repeats, 1)};
            openValueAsk_ = true;
        }
        break;
    case K::EditCell:
        c.row = a.row;
        c.col = a.col;
        c.editing = true;
        c.item = 0;
        songSel_.clear();
        break;
    case K::Bars:
        if (a.row >= 0 && a.row < static_cast<int>(song_.sections.size())) {
            auto& sec = song_.sections[static_cast<std::size_t>(a.row)];
            sec.bars = std::clamp(PerformanceView::songSectionBars(live, sec) + a.value, 1, midipad::kMaxSectionBars);
            sec.fit = false;
            songEdited();
        }
        break;
    case K::ItemSelect:
        c.row = a.row;
        c.col = a.col;
        c.item = a.item;
        c.editing = true;
        songSel_ = {a.item};
        songSelRow_ = a.row;
        songSelCol_ = a.col;
        break;
    case K::ItemToggle:
        if (songSelRow_ != a.row || songSelCol_ != a.col) {  // another cell: from the item under the cursor there
            songSel_.clear();
            if (c.row == a.row && c.col == a.col && c.editing)
                songSel_.push_back(c.item);
            songSelRow_ = a.row;
            songSelCol_ = a.col;
        }
        if (const auto it = std::find(songSel_.begin(), songSel_.end(), a.item); it != songSel_.end())
            songSel_.erase(it);
        else
            songSel_.push_back(a.item);
        c = {a.row, a.col, a.item, true};
        break;
    case K::ItemMove:
        if (hasItem && songSel_.size() > 1 && songSelRow_ == a.row && songSelCol_ == a.col &&
            std::find(songSel_.begin(), songSel_.end(), a.item) != songSel_.end()) {  // the selection, all moved alike
            const int bpb = std::max(live.beatsPerBar, 1);
            const auto starts = PerformanceView::songItemBars(live, a.col, *items);
            const int delta = a.value - static_cast<int>(std::lround(starts[static_cast<std::size_t>(a.item)] * bpb));
            std::vector<int> picked = songSel_;
            std::sort(picked.begin(), picked.end());
            picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
            std::vector<midipad::SongItem> moved;
            for (int k : picked)
                if (k >= 0 && k < static_cast<int>(items->size())) {
                    midipad::SongItem it = (*items)[static_cast<std::size_t>(k)];
                    const int to = std::max(static_cast<int>(std::lround(starts[static_cast<std::size_t>(k)] * bpb)) + delta, 0);
                    it.after = false;
                    it.bar = to / bpb;
                    it.beat = to % bpb;
                    moved.push_back(it);
                }
            for (auto k = picked.rbegin(); k != picked.rend(); ++k)
                if (*k >= 0 && *k < static_cast<int>(items->size()))
                    items->erase(items->begin() + *k);
            for (const midipad::SongItem& it : moved)
                placeSongItem(a.row, a.col, it, live);
            songSel_.clear();  // where they are now
            std::vector<bool> taken(items->size(), false);
            for (const midipad::SongItem& it : moved)
                for (std::size_t k = 0; k < items->size(); ++k)
                    if (!taken[k] && (*items)[k] == it) {
                        taken[k] = true;
                        songSel_.push_back(static_cast<int>(k));
                        break;
                    }
            c.item = songSel_.empty() ? 0 : songSel_.front();
            c.editing = true;
            undoWhat_ = "Move (" + std::to_string(moved.size()) + " items)";
            songEdited();
            undoWhat_.clear();
        } else if (hasItem) {
            midipad::SongItem it = (*items)[static_cast<std::size_t>(a.item)];
            items->erase(items->begin() + a.item);
            it.bar = a.value / std::max(live.beatsPerBar, 1);
            it.beat = a.value % std::max(live.beatsPerBar, 1);
            c.item = placeSongItem(a.row, a.col, it, live);
            c.editing = true;
            songEdited();
        }
        break;
    case K::ItemContent:
        if (hasItem && (*items)[static_cast<std::size_t>(a.item)].kind == midipad::SongItem::Kind::Rest) {  // a silence: a bar longer or shorter
            auto& it = (*items)[static_cast<std::size_t>(a.item)];
            it.beats = std::clamp(it.beats + a.value * std::max(live.beatsPerBar, 1), 1, midipad::kMaxRestBeats);
            songEdited();
        } else if (hasItem) {  // clips 1 to 8, then chains A to H
            auto& it = (*items)[static_cast<std::size_t>(a.item)];
            const int v = ((it.kind == midipad::SongItem::Kind::Clip ? it.index : 8 + it.index) + a.value + 16) % 16;
            it.kind = v < 8 ? midipad::SongItem::Kind::Clip : midipad::SongItem::Kind::Chain;
            it.index = v % 8;
            songEdited();
        }
        break;
    case K::ItemRepeats:
        if (hasItem && (*items)[static_cast<std::size_t>(a.item)].kind == midipad::SongItem::Kind::Rest) {  // a silence: a beat longer or shorter
            auto& it = (*items)[static_cast<std::size_t>(a.item)];
            it.beats = std::clamp(it.beats + a.value, 1, midipad::kMaxRestBeats);
            songEdited();
        } else if (hasItem) {
            static constexpr int kRepeats[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32, 48, 64};
            auto& it = (*items)[static_cast<std::size_t>(a.item)];
            int k = 0;
            while (k < 13 && kRepeats[k] < it.repeats)
                ++k;
            it.repeats = kRepeats[std::clamp(k + a.value, 0, 13)];
            songEdited();
        }
        break;
    case K::AddItem:
        c.row = a.row;
        c.col = a.col;
        c.editing = true;
        c.item = static_cast<int>(songCell(a.row, a.col) ? songCell(a.row, a.col)->size() : 0);
        openSongPick(a.row, a.col, -1, live, false, ImGui::GetIO().MousePos, a.value);
        break;
    case K::RepeatsAsk:
        if (hasItem) {
            const midipad::SongItem& it = (*items)[static_cast<std::size_t>(a.item)];
            const bool rest = it.kind == midipad::SongItem::Kind::Rest;
            c = {a.row, a.col, a.item, true};
            valueAsk_ = ValueAsk{rest ? ValueAsk::Kind::RestBeats : ValueAsk::Kind::ItemRepeats, a.col, a.row, a.item, rest ? it.beats : it.repeats};
            openValueAsk_ = true;
        }
        break;
    case K::BarsAsk:
        if (a.row >= 0 && a.row < static_cast<int>(song_.sections.size())) {
            c.row = a.row;
            c.col = -1;
            valueAsk_ = ValueAsk{ValueAsk::Kind::SectionBars, -1, a.row, 0, PerformanceView::songSectionBars(live, song_.sections[static_cast<std::size_t>(a.row)])};
            openValueAsk_ = true;
        }
        break;
    case K::DropNew:  // a clip or a chain of the channel, on a bar of the cell
        if (items) {
            midipad::SongItem it;
            it.kind = a.item < 8 ? midipad::SongItem::Kind::Clip : midipad::SongItem::Kind::Chain;
            it.index = a.item % 8;
            it.after = false;
            it.bar = std::max(a.value - 1, 0) / std::max(live.beatsPerBar, 1);
            it.beat = std::max(a.value - 1, 0) % std::max(live.beatsPerBar, 1);
            undoWhat_ = std::string(a.item < 8 ? "Add (clip " + std::to_string(it.index + 1) : "Add (chain " + PerformanceView::chainLetter(it.index)) + ")";
            c = {a.row, a.col, placeSongItem(a.row, a.col, it, live), true};
            songEdited();
            undoWhat_.clear();
        }
        break;
    case K::DropItem:  // an item of the channel, moved (copied) onto a bar of this cell
        if (auto* from = songCell(a.fromRow, a.col); items && from && a.item >= 0 && a.item < static_cast<int>(from->size())) {
            midipad::SongItem it = (*from)[static_cast<std::size_t>(a.item)];
            if (!a.copy)
                from->erase(from->begin() + a.item);
            it.after = false;
            it.bar = std::max(a.value - 1, 0) / std::max(live.beatsPerBar, 1);
            it.beat = std::max(a.value - 1, 0) % std::max(live.beatsPerBar, 1);
            undoWhat_ = a.copy ? "Copy (item)" : "Move (item)";
            c = {a.row, a.col, placeSongItem(a.row, a.col, it, live), true};
            songEdited();
            undoWhat_.clear();
        }
        break;
    case K::BeatsTo:
        if (hasItem && (*items)[static_cast<std::size_t>(a.item)].kind == midipad::SongItem::Kind::Rest) {
            (*items)[static_cast<std::size_t>(a.item)].beats = std::clamp(a.value, 1, midipad::kMaxRestBeats);
            undoWhat_ = "Length (silence)";
            c = {a.row, a.col, a.item, true};
            songEdited();
            undoWhat_.clear();
        }
        break;
    case K::RepeatsTo:
        if (hasItem) {
            (*items)[static_cast<std::size_t>(a.item)].repeats = std::clamp(a.value, 1, midipad::kMaxSongRepeats);
            undoWhat_ = "Repeats (item)";
            c = {a.row, a.col, a.item, true};
            songEdited();
            undoWhat_.clear();
        }
        break;
    case K::SectionMove: {  // moved (or copied) before the section `row`
        const int count = static_cast<int>(song_.sections.size());
        if (a.item < 0 || a.item >= count)
            break;
        int to = std::clamp(a.row, 0, count);
        const midipad::SongSection moved = song_.sections[static_cast<std::size_t>(a.item)];
        if (!a.copy) {
            if (to == a.item || to == a.item + 1)
                break;  // where it is
            if (live.song.playing)
                runtime_->stopSong();
            song_.sections.erase(song_.sections.begin() + a.item);
            if (to > a.item)
                --to;
        }
        song_.sections.insert(song_.sections.begin() + to, moved);
        undoWhat_ = std::string(a.copy ? "Copy" : "Move") + " (section " + std::to_string(a.item + 1) + ")";
        c.row = to;
        c.col = -1;
        c.editing = false;
        songEdited();
        undoWhat_.clear();
        break;
    }
    case K::Rename:
        if (a.row >= 0 && a.row < static_cast<int>(song_.sections.size())) {
            c.row = a.row;
            c.col = -1;
            renameSection_ = a.row;
            renameChain_ = -1;
            renameInput_ = song_.sections[static_cast<std::size_t>(a.row)].name;
            renameRow_ = renameClipChannel_ = renameClipSlot_ = -1;
        openRename_ = true;
        }
        break;
    }
}

void GuiApp::songEvent(Engine::SessionEvent e, const LiveState& live) {
    using E = Engine::SessionEvent;
    using K = PerformanceView::MenuTarget::Kind;
    using A = PerformanceView::SongAction;
    auto& c = songCursor_;
    const int sections = static_cast<int>(song_.sections.size());
    c.row = std::clamp(c.row, 0, sections);
    if (e == E::Back) {
        if (c.editing)
            c.editing = false;
        else
            toggleSong(live);
        return;
    }
    if (e == E::Press && sharedSelectMenu())  // Select = Menu: the press opens the menu
        e = E::Menu;
    if (c.editing) {
        auto* items = songCell(c.row, c.col);
        if (!items) {
            c.editing = false;
            return;
        }
        const int count = static_cast<int>(items->size());
        c.item = std::clamp(c.item, 0, count);
        const bool onItem = c.item < count;
        switch (e) {
        case E::Left: c.item = std::max(c.item - 1, 0); break;
        case E::Right: c.item = std::min(c.item + 1, count); break;
        case E::Up: c.editing = false; break;
        case E::Press:
        case E::Menu:
            if (onItem)
                openMenu({K::SongItem, c.col, c.row, 0, c.item}, live, true, {});
            else
                openSongPick(c.row, c.col, -1, live, true, {});
            break;
        case E::ValueUp:
        case E::ValueDown:
            if (onItem)
                songAction({A::Kind::ItemContent, c.row, c.col, c.item, e == E::ValueUp ? 1 : -1}, live);
            break;
        case E::ValueLeft:
        case E::ValueRight:
            if (onItem)
                songAction({A::Kind::ItemRepeats, c.row, c.col, c.item, e == E::ValueRight ? 1 : -1}, live);
            break;
        default: break;
        }
        return;
    }
    switch (e) {
    case E::Up: c.row = std::max(c.row - 1, 0); break;
    case E::Down:
        c.row = std::min(c.row + 1, sections);
        if (c.row == sections)
            c.col = -1;
        break;
    case E::Left:  // among the channels shown; left of the first (or of the names), the last
    case E::Right: {  // right of the last, the names, then the first
        const std::vector<int> shown = PerformanceView::songChannels(live, song_);
        if (c.row >= sections || shown.empty()) {
            c.col = -1;
            break;
        }
        // In order: the name (-1), the times (-2), the channels; round again.
        const auto it = std::find(shown.begin(), shown.end(), c.col);
        const int at = it == shown.end() ? -1 : static_cast<int>(it - shown.begin()), last = static_cast<int>(shown.size()) - 1;
        if (e == E::Left)
            c.col = c.col == -1 ? shown.back() : c.col == -2 ? -1 : at <= 0 ? -2 : shown[static_cast<std::size_t>(at - 1)];
        else
            c.col = c.col == -1 ? -2 : c.col == -2 ? shown.front() : at == last ? -1 : shown[static_cast<std::size_t>(at + 1)];
        break;
    }
    case E::Press:
        if (c.row == sections)
            addSongSection(sections);
        else if (c.col == -2)
            songAction({A::Kind::TimesAsk, c.row}, live);
        else if (c.col < 0)
            songAction(live.song.playing && live.song.section == c.row ? A{A::Kind::Stop} : A{A::Kind::Play, c.row}, live);
        else
            songAction({A::Kind::EditCell, c.row, c.col}, live);
        break;
    case E::Menu:
        if (c.row < sections)
            openMenu(c.col < 0 ? PerformanceView::MenuTarget{K::Section, 0, c.row} : PerformanceView::MenuTarget{K::SongCell, c.col, c.row}, live, true, {});
        break;
    case E::ValueUp:
    case E::ValueDown:
    case E::ValueLeft:
    case E::ValueRight:
        if (c.row < sections && c.col == -2)  // the times it plays: one more up or right, one fewer down or left
            songAction({A::Kind::Times, c.row, -2, 0, e == E::ValueUp || e == E::ValueRight ? 1 : -1}, live);
        else if (c.row < sections && c.col < 0)  // the section's length: a bar up / down, four left / right
            songAction({A::Kind::Bars, c.row, -1, 0, e == E::ValueUp ? 1 : e == E::ValueDown ? -1 : e == E::ValueRight ? 4 : -4}, live);
        break;
    default: break;
    }
}

namespace {

enum MenuId : int {
    kLaunch, kStop, kDuplicate, kAddToChain, kClearClip,
    kStartHere, kRandom, kSilence, kInsertStep, kDeleteStep, kEmptyStep,
    kChannelStop, kChannelMute, kChannelSolo, kChainOn, kChainOff, kThenLoop, kThenStop, kThenNext, kPrevChain, kNextChain, kDuplicateChain, kRenameChain, kClearChain,
    kLock, kUnlock, kClearLoop, kResetShape, kSettings, kEditNotes, kRename, kColour, kUndo,
    kClipUp, kClipDown, kStepLeft, kStepRight,
    kSongPlay, kSongStop, kSongLoop, kSectionRename, kSectionFit, kSectionRepeats, kSectionDuplicate, kSectionInsert, kSectionUp, kSectionDown, kSectionDelete,
    kCellEdit, kCellClear, kCellCopy, kCellPaste,
    kRestBarMore, kRestBarLess, kRestBeatMore, kRestBeatLess,
    kItemChange, kItemGlue, kItemLeft, kItemRight, kItemLoop, kItemKind, kItemDuplicate, kItemDelete, kEditClipNotes, kItemEditNotes, kExportClip, kRenameClip, kRenameRow, kRowLaunch,
    kModPick, kModStop, kModSendCc, kModDepth, kModDirection, kCopyMapping, kNewClip, kNewClipCopy,
    kClipCopy, kClipPaste,
    kCopyFromClip = 900,  // + the clip (0-7) a loop is copied from, into the current one
    kPick = 1000,  // + the clip (0-7) or 8 + the chain: what a song item plays
    kModTarget = 2000  // + the movement track a loop modulates
};

// The entries of the modulation menu that leave it open, to go on setting it.
bool keepsModMenu(int id) { return id == kModDepth || id == kModDirection || (id >= kModTarget && id < kModTarget + Engine::kNoteTrack); }

}  // namespace

void GuiApp::screenKeys(const LiveState& live) {
    ImGuiIO& io = ImGui::GetIO();
    const bool free = !io.WantTextInput && !overlaySpec_ && !optionsOpen_ && !menu_ && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    // The arrows move the screen's cursor, not ImGui's own keyboard focus.
    if (free)
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (!free || io.KeyAlt)
        return;
    using E = Engine::SessionEvent;
    using K = PerformanceView::MenuTarget::Kind;
    const bool ctrl = io.KeyCtrl;
    auto pressed = [&](ImGuiKey key, bool withCtrl = false) { return ctrl == withCtrl && ImGui::IsKeyPressed(key, !withCtrl); };
    if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_RightArrow)) {
        sessionSel_.clear();  // the cursor alone again
        songSel_.clear();
    }
    if (pressed(ImGuiKey_UpArrow))
        sessionEvent(E::Up, live);
    if (pressed(ImGuiKey_DownArrow))
        sessionEvent(E::Down, live);
    if (pressed(ImGuiKey_LeftArrow))
        sessionEvent(E::Left, live);
    if (pressed(ImGuiKey_RightArrow))
        sessionEvent(E::Right, live);
    if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        if (io.KeyShift && sessionView_ && sessionSlot_ < Engine::kClipSlots)  // Shift+Enter: the cursor's row
            launchRow(sessionSlot_, live);
        else
            sessionEvent(E::Press, live);
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {  // as Play / Stop in the top bar
        if ((live.loopChannels != 0 && !live.loopsPaused) || live.song.playing)
            runtime_->transportStop();
        else
            runtime_->transportPlay(songView_ && !song_.sections.empty() ? std::clamp(songCursor_.row, 0, static_cast<int>(song_.sections.size()) - 1) : -1);
    }
    const bool erase = !ctrl && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
    const bool copy = pressed(ImGuiKey_C, true), paste = pressed(ImGuiKey_V, true), duplicate = pressed(ImGuiKey_D, true);
    auto run = [&](const PerformanceView::MenuTarget& target, int id, const std::string& what) {
        undoWhat_ = what;
        runMenu(target, id, live);
        undoWhat_.clear();
    };
    if (sessionView_) {
        const int ch = sessionChannel_;
        if (sessionSlot_ < Engine::kClipSlots && sessionButton_ < 0) {
            const int slot = sessionSlot_;
            const std::string name = "clip " + std::to_string(slot + 1);
            const bool has = live.clips[static_cast<std::size_t>(ch * Engine::kClipSlots + slot)].sources != 0;
            // Several clips selected: Delete and Ctrl+C take them all.
            std::vector<std::pair<int, int>> picked = sessionSel_.size() > 1 ? sessionSel_ : std::vector<std::pair<int, int>>{};
            if (picked.empty() && has)
                picked = {{ch, slot}};
            const bool cut = pressed(ImGuiKey_X, true);
            if ((copy || cut) && !picked.empty())
                copyClips(picked, name);
            if ((erase || cut) && !picked.empty())
                clearClips(picked, std::string(cut ? "Cut " : "Clear ") + (picked.size() > 1 ? "(" + std::to_string(picked.size()) + " clips)" : "(" + name + ")"));
            if (has && duplicate && sessionSel_.size() <= 1)
                run({K::Clip, ch, slot}, kDuplicate, "Duplicate (" + name + ")");
            if (paste && !clipClipboard_.empty())
                pasteClips(ch, slot, name);
        } else if (sessionSlot_ == Engine::kClipSlots && chainCursor_ < Engine::kChainSteps && erase) {
            run({K::Step, ch, chainCursor_}, kEmptyStep, "Empty it (step " + std::to_string(chainCursor_ + 1) + ")");
        }
        return;
    }
    if (!songView_)
        return;
    const PerformanceView::SongCursor cur = songCursor_;
    if (cur.row < 0 || cur.row >= static_cast<int>(song_.sections.size()))
        return;
    if (cur.col < 0) {  // a section
        const std::string name = "section " + std::to_string(cur.row + 1);
        if (erase)
            run({K::Section, 0, cur.row}, kSectionDelete, "Delete (" + name + ")");
        if (duplicate)
            run({K::Section, 0, cur.row}, kSectionDuplicate, "Duplicate (" + name + ")");
        return;
    }
    const auto* items = songCell(cur.row, cur.col);
    if (!items)
        return;
    const bool several = songSel_.size() > 1 && songSelRow_ == cur.row && songSelCol_ == cur.col;
    if (several && (erase || copy)) {  // the items selected: deleted, or copied where they are
        std::vector<int> picked = songSel_;
        std::sort(picked.begin(), picked.end());
        picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        auto* cell = songCell(cur.row, cur.col);
        const int bpb = std::max(live.beatsPerBar, 1);
        const auto starts = PerformanceView::songItemBars(live, cur.col, *cell);
        if (copy) {
            songClipboard_.clear();
            for (int k : picked)
                if (k >= 0 && k < static_cast<int>(cell->size())) {
                    midipad::SongItem it = (*cell)[static_cast<std::size_t>(k)];
                    const int at = static_cast<int>(std::lround(starts[static_cast<std::size_t>(k)] * bpb));
                    it.after = false;
                    it.bar = at / bpb;
                    it.beat = at % bpb;
                    songClipboard_.push_back(it);
                }
            songClipboardPart_ = true;
            toast_ = "Copied (" + std::to_string(songClipboard_.size()) + " items)";
            toastUndone_ = false;
            toastPlain_ = true;
            toastUntil_ = ImGui::GetTime() + 2.0;
        } else {
            undoWhat_ = "Delete (" + std::to_string(picked.size()) + " items)";
            for (auto k = picked.rbegin(); k != picked.rend(); ++k)
                if (*k >= 0 && *k < static_cast<int>(cell->size()))
                    cell->erase(cell->begin() + *k);
            songSel_.clear();
            songCursor_.item = 0;
            songEdited();
            undoWhat_.clear();
        }
        if (paste && !songClipboard_.empty())
            run({K::SongCell, cur.col, cur.row}, kCellPaste, "Paste (cell)");
        return;
    }
    if (cur.editing && cur.item >= 0 && cur.item < static_cast<int>(items->size())) {  // an item of the cell
        if (erase)
            run({K::SongItem, cur.col, cur.row, 0, cur.item}, kItemDelete, "Delete (item)");
        if (duplicate)
            run({K::SongItem, cur.col, cur.row, 0, cur.item}, kItemDuplicate, "Duplicate (item)");
    } else if (erase && !items->empty()) {
        run({K::SongCell, cur.col, cur.row}, kCellClear, "Clear (cell)");
    }
    if (copy) {
        run({K::SongCell, cur.col, cur.row}, kCellCopy, "Copy (cell)");
        songClipboardPart_ = false;
    }
    if (paste && !songClipboard_.empty())
        run({K::SongCell, cur.col, cur.row}, kCellPaste, "Paste (cell)");
}

void GuiApp::openMenu(const PerformanceView::MenuTarget& t, const LiveState& live, bool fromController, ImVec2 at) {
    using K = PerformanceView::MenuTarget::Kind;
    ContextMenu menu;
    menu.target = t;
    menu.fromController = fromController;
    menu.at = at;
    // Every menu in the same order, its groups between lines: what plays; what
    // changes its content; copy, paste and move; name and export; settings;
    // last, in red, what clears or deletes. Lists go in submenus.
    std::vector<MenuEntry>* into = &menu.items;
    auto add = [&](std::string label, int id, bool enabled = true) {
        MenuEntry e;
        e.label = std::move(label);
        e.id = id;
        e.enabled = enabled;
        into->push_back(std::move(e));
    };
    auto key = [&](const char* k) { into->back().shortcut = k; };
    auto sep = [&] {
        if (!into->empty() && !into->back().separator) {
            MenuEntry e;
            e.separator = true;
            e.enabled = false;
            into->push_back(std::move(e));
        }
    };
    auto danger = [&] { into->back().danger = true; };
    auto sub = [&](std::string label, const std::function<void()>& fill) {
        std::vector<MenuEntry> list;
        std::vector<MenuEntry>* outer = std::exchange(into, &list);
        fill();
        into = outer;
        if (list.empty())
            return;
        MenuEntry e;
        e.label = std::move(label);
        e.id = -1;
        e.enabled = std::any_of(list.begin(), list.end(), [](const MenuEntry& m) { return m.enabled; });
        e.shortcut = "\xE2\x80\xBA";
        e.sub = std::move(list);
        into->push_back(std::move(e));
    };
    // The same loop from another clip of the channel, into the current one (as it is: stretch, amplitude...).
    auto addCopyFrom = [&](int loopSource) {
        const auto lc = static_cast<std::size_t>(std::clamp(live.channel, 0, kChannelCount - 1));
        for (int k = 0; k < Engine::kClipSlots; ++k)
            if (k != live.currentClip[lc] && ((live.clips[lc * Engine::kClipSlots + static_cast<std::size_t>(k)].sources >> loopSource) & 1u))
                add("Copy from " + clipName(live.channel, k), kCopyFromClip + k);
    };
    const auto c = static_cast<std::size_t>(std::clamp(t.channel, 0, kChannelCount - 1));
    const LiveState::Chains& chains = live.chains[c];
    const int shown = t.kind == K::Chain ? std::clamp(t.slot, 0, Engine::kChains - 1) : shownChain(t.channel);
    const LiveState::Chain& chain = chains.list[static_cast<std::size_t>(shown)];
    const bool runs = chains.at >= 0 && chains.playing == shown;
    const std::string chainName = "CHAIN " + PerformanceView::chainLetter(shown) +
                                  (chainNames_[c][static_cast<std::size_t>(shown)].empty() ? "" : "  " + chainNames_[c][static_cast<std::size_t>(shown)]);
    static constexpr const char* kSourceNames[] = {"LEFT STICK", "RIGHT STICK", "FINGER 1", "FINGER 2", "TRIGGERS", "TILT", "D-PAD", "NOTES"};
    const int source = std::clamp(t.source, 0, kLoopSourceCount - 1);
    switch (t.kind) {
    case K::Clip: {
        const bool has = live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(t.slot)].sources != 0;
        const bool plays = ((live.clipsPlaying >> t.channel) & 1u) && live.currentClip[c] == t.slot;
        bool freeSlot = false, freeStep = false;
        for (int k = 0; k < Engine::kClipSlots; ++k)
            freeSlot = freeSlot || live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(k)].sources == 0;
        for (int k = 0; k < Engine::kChainSteps; ++k)
            freeStep = freeStep || chain.clip[static_cast<std::size_t>(k)] < 0;
        menu.title = (clipNames_[c][static_cast<std::size_t>(t.slot)].empty() ? "CLIP " + std::to_string(t.slot + 1) : clipNames_[c][static_cast<std::size_t>(t.slot)]) +
                     "\nchannel " + std::to_string(t.channel + 1) + ", clip " + std::to_string(t.slot + 1);
        if (plays)
            add("Stop", kStop);
        else
            add("Launch", kLaunch, has);
        add("Edit the notes\xE2\x80\xA6", kEditClipNotes, ((live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(t.slot)].sources >> static_cast<int>(LoopSource::Notes)) & 1u) != 0);
        sep();
        add("Copy", kClipCopy, has);
        key("Ctrl+C");
        add("Paste", kClipPaste, !clipClipboard_.empty());
        key("Ctrl+V");
        add("Duplicate", kDuplicate, has && freeSlot);
        key("Ctrl+D");
        sub("Move", [&] {
            add("Up", kClipUp, has && t.slot > 0);
            add("Down", kClipDown, has && t.slot < Engine::kClipSlots - 1);
        });
        add("Add to the chain", kAddToChain, has && freeStep);
        sep();
        add("Rename\xE2\x80\xA6", kRenameClip, has);
        add("Export as MIDI", kExportClip, has);
        sep();
        add("Clear", kClearClip, has);
        key("Del");
        danger();
        break;
    }
    case K::Step: {
        const bool has = chain.clip[static_cast<std::size_t>(t.slot)] >= 0;
        menu.title = "STEP " + std::to_string(t.slot + 1) + "\nchain " + PerformanceView::chainLetter(shown);
        add("Start from here", kStartHere, has);
        sep();
        add("Random (R)", kRandom);
        add("Silence (S)", kSilence);
        sep();
        sub("Move", [&] {
            add("Left", kStepLeft, has && t.slot > 0);
            add("Right", kStepRight, has && t.slot < Engine::kChainSteps - 1);
        });
        add("Insert a step", kInsertStep);
        sep();
        add("Empty it", kEmptyStep, has);
        key("Del");
        danger();
        add("Delete the step", kDeleteStep);
        danger();
        break;
    }
    case K::Channel:  // slot 1: from the song screen, without what belongs to the session screen
        menu.title = "CHANNEL " + std::to_string(t.channel + 1);
        if (t.slot != 1)
            add("Stop", kChannelStop, ((live.clipsPlaying >> t.channel) & 1u) != 0);
        add(((live.muted >> t.channel) & 1u) ? "Unmute" : "Mute", kChannelMute);
        add(((live.soloed >> t.channel) & 1u) ? "Solo off" : "Solo", kChannelSolo);
        if (t.slot != 1) {
            if (chains.at >= 0)
                add("Chain off", kChainOff);
            else
                add("Chain " + PerformanceView::chainLetter(shown) + " on", kChainOn, !chain.empty());
        }
        sep();
        add("Rename\xE2\x80\xA6", kRename);
        add("Colour\xE2\x80\xA6", kColour);
        if (t.slot != 1)
            add("Copy mapping from\xE2\x80\xA6", kCopyMapping);
        break;
    case K::Row: {  // a row of the session screen: its clips on every channel
        const std::string& n = rowNames_[static_cast<std::size_t>(std::clamp(t.slot, 0, Engine::kClipSlots - 1))];
        menu.title = (n.empty() ? "ROW " + std::to_string(t.slot + 1) : n) + (n.empty() ? "" : "\nrow " + std::to_string(t.slot + 1));
        bool any = false, all = true;
        for (int k = 0; k < kChannelCount; ++k) {
            const auto kc = static_cast<std::size_t>(k);
            if (!live.clips[kc * Engine::kClipSlots + static_cast<std::size_t>(t.slot)].sources)
                continue;
            any = true;
            all = all && ((((live.clipsPlaying >> k) & 1u) && live.currentClip[kc] == t.slot) || live.queuedClip[kc] == t.slot);
        }
        add(any && all ? "Stop the row" : "Launch the row", kRowLaunch, any);
        sep();
        add("Rename\xE2\x80\xA6", kRenameRow);
        break;
    }
    case K::Chain: {
        menu.title = chainName + "\nchannel " + std::to_string(t.channel + 1);
        if (runs)
            add("Stop the chain", kChainOff);
        else
            add("Play", kChainOn, !chain.empty());
        // Then another chain: the one after the chain it goes to now (after this one at first), never itself.
        int next = chain.then >= 0 ? (chain.then + 1) % Engine::kChains : (shown + 1) % Engine::kChains;
        if (next == shown)
            next = (next + 1) % Engine::kChains;
        sep();
        sub("Then", [&] {
            add("Back to step 1", kThenLoop, chain.then != Engine::kThenLoop);
            add("Stop", kThenStop, chain.then != Engine::kThenStop);
            add("Chain " + PerformanceView::chainLetter(next), kThenNext);
        });
        sep();
        bool free = false;
        for (const LiveState::Chain& other : chains.list)
            free = free || (&other != &chain && other.empty());
        add("Duplicate", kDuplicateChain, !chain.empty() && free);
        sub("Show", [&] {
            add("Previous chain", kPrevChain);
            add("Next chain", kNextChain);
        });
        sep();
        add("Rename\xE2\x80\xA6", kRenameChain);
        sep();
        add("Clear", kClearChain, !chain.empty() || chain.then != Engine::kThenLoop);
        danger();
        break;
    }
    case K::Lane:
    case K::Panel: {
        const LiveState::Loop& loop = live.loops[static_cast<std::size_t>(source)];
        const bool lane = t.kind == K::Lane;
        const LoopStatus& st = lane ? loop.lanes[static_cast<std::size_t>(t.axis & 1)].status : loop.status;
        const bool triggers = source == static_cast<int>(LoopSource::Triggers);
        const std::string axis = !lane ? "" : triggers ? " " + label(t.axis == 0 ? Button::LeftTrigger : Button::RightTrigger) : (t.axis == 0 ? " X" : " Y");
        menu.title = std::string(kSourceNames[source]) + "\nloop" + axis;
        const bool plays = st.state == LoopStatus::State::Playing;
        if (st.locked)
            add("Unlock", kUnlock);
        else
            add("Lock", kLock, plays);
        add("Back to 1x \xC2\xB7 100%", kResetShape, plays);
        sep();
        sub("Copy from", [&] { addCopyFrom(source); });
        if (lane && source != static_cast<int>(LoopSource::Notes))  // modulating another loop's amplitude
            sub("Modulate", [&] {
                add("The amplitude of\xE2\x80\xA6", kModPick, plays);
                add("Stop modulating", kModStop, st.modTargets != 0);
                add(st.modSendsCc ? "Send its CC: on" : "Send its CC: off", kModSendCc, st.modTargets != 0);
            });
        sep();
        add("Settings\xE2\x80\xA6", kSettings);
        sep();
        add(lane ? "Clear" + axis : "Clear", kClearLoop, st.state != LoopStatus::State::Empty);
        danger();
        break;
    }
    case K::ModPick:
        fillModMenu(menu, live);
        break;
    case K::NoteLoop: {
        const LoopStatus& st = live.loops[static_cast<std::size_t>(LoopSource::Notes)].status;
        menu.title = "NOTE LOOP";
        if (st.locked)
            add("Unlock", kUnlock);
        else
            add("Lock", kLock, st.state == LoopStatus::State::Playing);
        add("Edit the notes\xE2\x80\xA6", kEditNotes);
        {  // a new clip of the channel: the first empty one, to record into, or a copy of this one there
            const auto lc = static_cast<std::size_t>(std::clamp(live.channel, 0, kChannelCount - 1));
            bool freeSlot = false;
            for (int k = 0; k < Engine::kClipSlots; ++k)
                freeSlot = freeSlot || live.clips[lc * Engine::kClipSlots + static_cast<std::size_t>(k)].sources == 0;
            const bool has = live.clips[lc * Engine::kClipSlots + static_cast<std::size_t>(live.currentClip[lc])].sources != 0;
            sep();
            add("New clip", kNewClip, freeSlot && has);
            add("New clip, a copy of this one", kNewClipCopy, freeSlot && has);
        }
        sub("Copy from", [&] { addCopyFrom(static_cast<int>(LoopSource::Notes)); });
        sep();
        add("Clear", kClearLoop, st.state != LoopStatus::State::Empty);
        danger();
        break;
    }
    case K::Control:
        menu.title = "CONTROL";
        add("Settings\xE2\x80\xA6", kSettings);
        break;
    case K::ChannelBlock:
        menu.title = "CHANNEL " + std::to_string(t.channel + 1);
        add("Rename\xE2\x80\xA6", kRename);
        add("Colour\xE2\x80\xA6", kColour);
        add("Copy mapping from\xE2\x80\xA6", kCopyMapping);
        break;
    case K::Section: {
        const int count = static_cast<int>(song_.sections.size());
        if (t.slot < 0 || t.slot >= count)
            return;
        const midipad::SongSection& sec = song_.sections[static_cast<std::size_t>(t.slot)];
        menu.title = (sec.name.empty() ? "SECTION " + std::to_string(t.slot + 1) : sec.name) + "\nsection " + std::to_string(t.slot + 1);
        if (live.song.playing && live.song.section == t.slot)
            add("Stop the song", kSongStop);
        else
            add(live.song.playing ? "Go here (next bar)" : "Play from here", kSongPlay);
        add("Loop this section", kSongLoop);
        into->back().checked = live.song.loop;
        sep();
        add("Times played: " + std::to_string(std::max(sec.repeats, 1)) + "\xE2\x80\xA6", kSectionRepeats);
        add(sec.fit ? "Length: fixed" : "Length: = longest channel", kSectionFit);
        sep();
        add("Duplicate", kSectionDuplicate);
        key("Ctrl+D");
        add("Insert a section above", kSectionInsert);
        sub("Move", [&] {
            add("Up", kSectionUp, t.slot > 0);
            add("Down", kSectionDown, t.slot < count - 1);
        });
        sep();
        add("Rename\xE2\x80\xA6", kSectionRename);
        sep();
        add("Delete", kSectionDelete);
        key("Del");
        danger();
        break;
    }
    case K::SongCell: {
        const auto* items = songCell(t.slot, t.channel);
        if (!items)
            return;
        menu.title = "CELL\nchannel " + std::to_string(t.channel + 1);
        add("Edit", kCellEdit);
        sep();
        add("Copy", kCellCopy, !items->empty());
        key("Ctrl+C");
        add("Paste", kCellPaste, !songClipboard_.empty());
        key("Ctrl+V");
        sep();
        add("Clear", kCellClear, !items->empty());
        key("Del");
        danger();
        break;
    }
    case K::SongPick: {
        const auto c2 = static_cast<std::size_t>(std::clamp(t.channel, 0, kChannelCount - 1));
        const double bar = std::max(live.beatsPerBar, 1);
        auto bars = [&](double beats) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f bars", beats / bar);
            return std::string(buf);
        };
        menu.title = std::string(t.axis < 0 ? "ADD" : "CHANGE") + "\nchannel " + std::to_string(t.channel + 1);
        bool any = false;
        for (int k = 0; k < Engine::kClipSlots; ++k)
            if (const double b = live.clipBeats[c2][static_cast<std::size_t>(k)]; b > 0) {
                add(clipName(t.channel, k) + "   " + bars(b), kPick + k);
                any = true;
            }
        for (int n = 0; n < Engine::kChains; ++n)
            if (!live.chains[c2].list[static_cast<std::size_t>(n)].empty()) {
                const std::string& name = chainNames_[c2][static_cast<std::size_t>(n)];
                const bool open = (live.chainOpen[c2] >> n) & 1u;
                add("Chain " + PerformanceView::chainLetter(n) + (name.empty() ? "" : " " + name) + "   " + bars(live.chainBeats[c2][static_cast<std::size_t>(n)]) +
                        (open ? " + \xE2\x88\x9E" : ""),
                    kPick + 8 + n);
                any = true;
            }
        if (!any)
            add("Nothing recorded on this channel yet", kPick, false);
        // A silence of a beat: its length is then dragged, wheeled or set with the right stick.
        add("Silence", kPick + 16);
        break;
    }
    case K::SongItem: {
        const auto* items = songCell(t.slot, t.channel);
        if (!items || t.axis < 0 || t.axis >= static_cast<int>(items->size()))
            return;
        const midipad::SongItem& it = (*items)[static_cast<std::size_t>(t.axis)];
        const double start = PerformanceView::songItemBars(live, t.channel, *items)[static_cast<std::size_t>(t.axis)];
        const bool glued = it.after && t.axis > 0;
        char where[32];
        std::snprintf(where, sizeof where, "%.2g", start + 1);
        const bool rest = it.kind == midipad::SongItem::Kind::Rest;
        menu.title = (rest ? std::string("SILENCE")
                      : it.kind == midipad::SongItem::Kind::Clip ? "CLIP " + std::to_string(it.index + 1) : "CHAIN " + PerformanceView::chainLetter(it.index)) +
                     (glued ? std::string("\nright after the one before") : "\nbar " + std::string(where));
        add("Change\xE2\x80\xA6", kItemChange);
        if (it.kind == midipad::SongItem::Kind::Clip)  // its notes, in the editor, as from the session screen
            add("Edit the notes\xE2\x80\xA6", kItemEditNotes,
                ((live.clips[static_cast<std::size_t>(t.channel * Engine::kClipSlots + std::clamp(it.index, 0, Engine::kClipSlots - 1))].sources >>
                  static_cast<int>(LoopSource::Notes)) & 1u) != 0);
        if (!rest) {
            add(it.loop ? "After its repeats: silence" : "After its repeats: loop on", kItemLoop);
            add(it.kind == midipad::SongItem::Kind::Clip ? "Make it a chain" : "Make it a clip", kItemKind);
        }
        sep();
        add("Duplicate", kItemDuplicate, static_cast<int>(items->size()) < midipad::kMaxSongItems);
        key("Ctrl+D");
        sub("Move", [&] {
            add("A bar left", kItemLeft, start > 1e-6);
            add("A bar right", kItemRight, start < midipad::kMaxSectionBars - 1);
            if (glued)
                add("On a bar of its own", kItemGlue);
            else
                add("Right after the one before", kItemGlue, t.axis > 0);
        });
        sep();
        add("Delete", kItemDelete);
        key("Del");
        danger();
        break;
    }
    }
    // Opened from the controller, the menus of the session and song screens end
    // with Undo: its way to it (the keyboard has Ctrl+Z).
    if (fromController &&
        (t.kind == K::Clip || t.kind == K::Step || t.kind == K::Chain || t.kind == K::Channel || t.kind == K::Section ||
         t.kind == K::SongCell || t.kind == K::SongItem) &&
        !menu.items.empty()) {
        sep();
        add(undo_.empty() ? std::string("Undo") : "Undo: " + undo_.back().what, kUndo, !undo_.empty());
    }
    while (!menu.items.empty() && menu.items.back().separator)
        menu.items.pop_back();
    menu.selected = 0;
    while (menu.selected < static_cast<int>(menu.items.size()) - 1 && !menu.items[static_cast<std::size_t>(menu.selected)].enabled)
        ++menu.selected;
    menu_ = std::move(menu);
}

void GuiApp::fillModMenu(ContextMenu& menu, const LiveState& live) const {
    const PerformanceView::MenuTarget& t = menu.target;
    const int self = Engine::trackOf(t.source, t.axis);
    std::string name = PerformanceView::trackName(self);
    for (char& ch : name)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    menu.title = name + " MODULATES\nthe amplitude of";
    menu.items.clear();
    auto statusOf = [&](int track) -> const LoopStatus& {
        return live.loops[static_cast<std::size_t>(Engine::sourceOfTrack(track))].lanes[static_cast<std::size_t>(Engine::axisOfTrack(track))].status;
    };
    for (int track = 0; track < Engine::kNoteTrack; ++track) {
        if (track == self)
            continue;
        // Never a loop this one is modulated by, even through others.
        bool cycle = false;
        for (int at = self, hops = 0; at >= 0 && hops < Engine::kNoteTrack && !cycle; ++hops) {
            at = modEdit_[static_cast<std::size_t>(at)].source;
            cycle = at == track;
        }
        const LoopModulation& m = modEdit_[static_cast<std::size_t>(track)];
        const bool routed = m.source == self;
        const std::string label = std::string(PerformanceView::trackName(track)) +
                                  (routed ? "   " + std::to_string(m.depth) + " %" + (m.inverted ? " \xC2\xB7 inverted" : "") : "");
        menu.items.push_back({label, kModTarget + track, statusOf(track).state == LoopStatus::State::Playing && !cycle, routed, {}, false, false, {}});
    }
    const bool on = modFocus_ >= 0;
    const std::string focus = on ? std::string(" \xC2\xB7 ") + PerformanceView::trackName(modFocus_) : "";
    const LoopModulation& f = modEdit_[static_cast<std::size_t>(std::max(modFocus_, 0))];
    menu.items.push_back({"Depth" + focus + ": \xE2\x80\xB9 " + std::to_string(on ? f.depth : 100) + " % \xE2\x80\xBA", kModDepth, on, false, {}, false, false, {}});
    menu.items.push_back({"Direction" + focus + (on && f.inverted ? ": inverted" : ": normal"), kModDirection, on, false, {}, false, false, {}});
}

void GuiApp::changeModulation(int id, int step, const LiveState& live) {
    if (!menu_ || menu_->target.kind != PerformanceView::MenuTarget::Kind::ModPick)
        return;
    const int self = Engine::trackOf(menu_->target.source, menu_->target.axis);
    int track = modFocus_;
    if (id >= kModTarget) {  // a loop ticked (modulated from now on, fully) or unticked
        track = id - kModTarget;
        LoopModulation& m = modEdit_[static_cast<std::size_t>(track)];
        m = m.source == self ? LoopModulation{} : LoopModulation{self, 100, false};
        modFocus_ = m.source == self ? track : -1;
    } else if (track >= 0 && id == kModDepth) {
        int& depth = modEdit_[static_cast<std::size_t>(track)].depth;
        depth = step == 1 ? (depth >= 100 ? 10 : depth + 10) : std::clamp(depth + (step > 0 ? 10 : -10), 10, 100);
    } else if (track >= 0 && id == kModDirection) {
        modEdit_[static_cast<std::size_t>(track)].inverted = !modEdit_[static_cast<std::size_t>(track)].inverted;
    }
    if (track >= 0)
        runtime_->setModulation(Engine::sourceOfTrack(track), Engine::axisOfTrack(track), modEdit_[static_cast<std::size_t>(track)]);
    if (modFocus_ < 0)  // the focus on another loop it modulates
        for (int t = 0; t < Engine::kNoteTrack && modFocus_ < 0; ++t)
            if (modEdit_[static_cast<std::size_t>(t)].source == self)
                modFocus_ = t;
    const int selected = menu_->selected;
    fillModMenu(*menu_, live);
    menu_->selected = std::clamp(selected, 0, static_cast<int>(menu_->items.size()) - 1);
}

void GuiApp::runMenu(const PerformanceView::MenuTarget& t, int id, const LiveState& live) {
    using K = PerformanceView::MenuTarget::Kind;
    if (id >= kCopyFromClip && id < kCopyFromClip + Engine::kClipSlots &&
        (t.kind == K::Lane || t.kind == K::Panel || t.kind == K::NoteLoop)) {  // a loop copied from another clip of the channel
        const int ch = live.channel, from = id - kCopyFromClip, to = live.currentClip[static_cast<std::size_t>(ch)];
        const int source = t.kind == K::NoteLoop ? static_cast<int>(LoopSource::Notes) : std::clamp(t.source, 0, kLoopSourceCount - 1);
        std::uint16_t tracks = 0;
        if (source == static_cast<int>(LoopSource::Notes))
            tracks = static_cast<std::uint16_t>(1u << Engine::kNoteTrack);
        else
            for (int a = 0; a < 2; ++a)
                if (t.kind == K::Panel || (t.axis & 1) == a)
                    tracks = static_cast<std::uint16_t>(tracks | (1u << Engine::trackOf(source, a)));
        undoWhat_ = "Loop copied from " + clipName(ch, from);
        rememberClips({{ch, to}}, "Loop copied");
        undoWhat_.clear();
        runtime_->copyTracks(ch, from, to, tracks);
        return;
    }
    if (id == kNewClip || id == kNewClipCopy) {  // the channel's first empty clip: the next recording goes there, or this one is copied there
        const int ch = live.channel;
        const auto c = static_cast<std::size_t>(ch);
        const int from = live.currentClip[c];
        for (int k = 0; k < Engine::kClipSlots; ++k)
            if (live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(k)].sources == 0) {
                if (id == kNewClip) {
                    runtime_->setRecordSlot(ch, k);
                    sessionSlot_ = k;
                } else {
                    rememberClips({{ch, k}}, "Clip duplicated");
                    runtime_->copyClip(ch, from, k);
                    clipNames_[c][static_cast<std::size_t>(k)] = clipNames_[c][static_cast<std::size_t>(from)];
                    runtime_->launchClip(ch, k);
                }
                break;
            }
        return;
    }
    if (t.kind == K::SongPick && id >= kPick) {  // what a song item plays: a new one, or this one's
        const int pick = id - kPick, bpb = std::max(live.beatsPerBar, 1);
        const auto kind = pick < 8 ? midipad::SongItem::Kind::Clip : pick < 16 ? midipad::SongItem::Kind::Chain : midipad::SongItem::Kind::Rest;
        const int index = pick % 8;
        const int beats = pick == 16 ? 1 : pick == 17 ? bpb : pick == 18 ? 2 * bpb : 4 * bpb;
        auto* items = songCell(t.slot, t.channel);
        if (!items)
            return;
        if (t.axis < 0) {
            addSongItem(t.slot, t.channel, live, kind, kind == midipad::SongItem::Kind::Rest ? 0 : index);
            if (kind == midipad::SongItem::Kind::Rest && !items->empty())
                items->back().beats = beats;
            if (t.source > 0 && !items->empty()) {  // on the bar it was asked on
                midipad::SongItem item = items->back();
                items->pop_back();
                item.after = false;
                item.bar = (t.source - 1) / std::max(live.beatsPerBar, 1);
                item.beat = (t.source - 1) % std::max(live.beatsPerBar, 1);
                songCursor_.item = placeSongItem(t.slot, t.channel, item, live);
            }
            songEdited();
        } else if (t.axis < static_cast<int>(items->size())) {
            (*items)[static_cast<std::size_t>(t.axis)].kind = kind;
            (*items)[static_cast<std::size_t>(t.axis)].index = kind == midipad::SongItem::Kind::Rest ? 0 : index;
            if (kind == midipad::SongItem::Kind::Rest)
                (*items)[static_cast<std::size_t>(t.axis)].beats = beats;
            songCursor_ = {t.slot, t.channel, t.axis, true};
            songEdited();
        }
        return;
    }
    const int ch = std::clamp(t.channel, 0, kChannelCount - 1);
    const auto c = static_cast<std::size_t>(ch);
    const int shown = t.kind == K::Chain ? std::clamp(t.slot, 0, Engine::kChains - 1) : shownChain(ch);
    const LiveState::Chains& chains = live.chains[c];
    const LiveState::Chain& chain = chains.list[static_cast<std::size_t>(shown)];
    switch (id) {  // what can be undone, as it is before
    case kAddToChain: case kInsertStep: case kDeleteStep: case kThenLoop: case kThenStop: case kThenNext: case kDuplicateChain: case kClearChain:
        rememberChains(ch, "Chain changed");
        break;
    case kClearClip: rememberClips({{ch, t.slot}}, "Clip cleared"); break;
    case kDuplicate:
        for (int k = 0; k < Engine::kClipSlots; ++k)
            if (live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(k)].sources == 0) {
                rememberClips({{ch, k}}, "Clip duplicated");
                break;
            }
        break;
    case kUndo: undoRedo(false); return;
    case kClipUp:
    case kClipDown: {  // swapped with the one above or below
        PerformanceView::SessionAction a{PerformanceView::SessionAction::Kind::ClipDrop, ch, t.slot + (id == kClipUp ? -1 : 1)};
        a.fromChannel = ch;
        a.fromSlot = t.slot;
        sessionDrop(a, live);
        return;
    }
    case kStepLeft:
    case kStepRight: {
        PerformanceView::SessionAction a{PerformanceView::SessionAction::Kind::StepDrop, ch, t.slot + (id == kStepLeft ? -1 : 1)};
        a.fromSlot = t.slot;
        a.onStep = true;
        sessionDrop(a, live);
        return;
    }
    default: break;
    }
    auto openWindow = [&](const std::string& spec) {
        if (spec == "notes") {  // the note editor: a panel of the main screen
            openNotes(live);
            return;
        }
        overlayAppearing_ = overlaySpec_ != spec;
        overlaySpec_ = spec;
        optionsOpen_ = false;
    };
    // A chain's steps, rewritten from `steps` (clip, repeats).
    auto writeChain = [&](const std::vector<std::pair<int, int>>& steps, int into) {
        for (int k = 0; k < Engine::kChainSteps; ++k)
            runtime_->setChainStep(ch, into, k, steps[static_cast<std::size_t>(k)].first, steps[static_cast<std::size_t>(k)].second);
    };
    std::vector<std::pair<int, int>> steps;
    for (int k = 0; k < Engine::kChainSteps; ++k)
        steps.push_back({chain.clip[static_cast<std::size_t>(k)], chain.repeats[static_cast<std::size_t>(k)]});
    const bool loopTarget = t.kind == K::Lane || t.kind == K::Panel || t.kind == K::NoteLoop;
    const int axis = t.kind == K::Lane ? t.axis : -1;
    switch (id) {
    case kLaunch: runtime_->launchClip(ch, t.slot); break;
    case kStop: runtime_->stopClip(ch); break;
    case kClipCopy: {  // the selection when the clip is in it, else the clip
        std::vector<std::pair<int, int>> picked{{ch, t.slot}};
        if (sessionSel_.size() > 1 && std::find(sessionSel_.begin(), sessionSel_.end(), std::pair<int, int>{ch, t.slot}) != sessionSel_.end())
            picked = sessionSel_;
        const std::string name = picked.size() > 1 ? std::to_string(picked.size()) + " clips" : "clip " + std::to_string(t.slot + 1);
        copyClips(picked, name);
        break;
    }
    case kClipPaste: pasteClips(ch, t.slot, "clip " + std::to_string(t.slot + 1)); break;
    case kDuplicate:
        for (int k = 0; k < Engine::kClipSlots; ++k)
            if (live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(k)].sources == 0) {
                runtime_->copyClip(ch, t.slot, k);
                clipNames_[c][static_cast<std::size_t>(k)] = clipNames_[c][static_cast<std::size_t>(t.slot)];
                break;
            }
        break;
    case kAddToChain:
        for (int k = 0; k < Engine::kChainSteps; ++k)
            if (chain.clip[static_cast<std::size_t>(k)] < 0) {
                setChainStep(ch, k, t.slot, 1);
                break;
            }
        break;
    case kClearClip:
        runtime_->clearClip(ch, t.slot);
        clipNames_[c][static_cast<std::size_t>(t.slot)].clear();
        break;
    case kStartHere: runtime_->chainGoTo(ch, shown, t.slot); break;
    case kRandom:
        setChainStep(ch, t.slot, Engine::kChainRandom, std::max<int>(chain.repeats[static_cast<std::size_t>(t.slot)], 0));
        break;
    case kSilence:
        setChainStep(ch, t.slot, Engine::kChainSilence, std::max<int>(chain.repeats[static_cast<std::size_t>(t.slot)], 0));
        break;
    case kInsertStep:
        steps.insert(steps.begin() + t.slot, {-1, 1});
        steps.pop_back();
        writeChain(steps, shown);
        break;
    case kDeleteStep:
        steps.erase(steps.begin() + t.slot);
        steps.push_back({-1, 1});
        writeChain(steps, shown);
        break;
    case kEmptyStep: setChainStep(ch, t.slot, -1, 1); break;
    case kChannelStop: runtime_->stopClip(ch); break;
    case kChannelMute: toggleMute(ch, live); break;
    case kChannelSolo: toggleSolo(ch, live); break;
    case kChainOn: runtime_->chainGoTo(ch, shown, 0); break;
    case kChainOff: runtime_->stopChain(ch); break;
    case kThenLoop: runtime_->setChainThen(ch, shown, Engine::kThenLoop); break;
    case kThenStop: runtime_->setChainThen(ch, shown, Engine::kThenStop); break;
    case kThenNext: {  // the chain after the one it goes to now (after this one, at first), never itself
        int next = chain.then >= 0 ? (chain.then + 1) % Engine::kChains : (shown + 1) % Engine::kChains;
        if (next == shown)
            next = (next + 1) % Engine::kChains;
        runtime_->setChainThen(ch, shown, next);
        break;
    }
    case kPrevChain: showChain(ch, shown - 1); break;
    case kNextChain: showChain(ch, shown + 1); break;
    case kDuplicateChain:
        for (int n = 1; n < Engine::kChains; ++n) {  // into the first empty chain after it, shown then
            const int into = (shown + n) % Engine::kChains;
            if (!chains.list[static_cast<std::size_t>(into)].empty())
                continue;
            writeChain(steps, into);
            runtime_->setChainThen(ch, into, chain.then == into ? Engine::kThenLoop : chain.then);
            chainNames_[c][static_cast<std::size_t>(into)] = chainNames_[c][static_cast<std::size_t>(shown)];
            showChain(ch, into);
            break;
        }
        break;
    case kRenameChain:
        renameChannel_ = ch;
        renameChain_ = shown;
        renameSection_ = -1;
        renameInput_ = chainNames_[c][static_cast<std::size_t>(shown)];
        renameRow_ = renameClipChannel_ = renameClipSlot_ = -1;
        openRename_ = true;
        break;
    case kClearChain:
        if (chains.playing == shown)
            runtime_->stopChain(ch);
        writeChain(std::vector<std::pair<int, int>>(Engine::kChainSteps, {-1, 1}), shown);
        runtime_->setChainThen(ch, shown, Engine::kThenLoop);
        chainNames_[c][static_cast<std::size_t>(shown)].clear();
        break;
    case kLock:
    case kUnlock:
        if (loopTarget)
            runtime_->toggleLoopLock(t.source, axis);
        break;
    case kClearLoop:
        if (loopTarget)
            runtime_->clearLoop(t.source, axis);
        break;
    case kModPick: {  // the menu of what it modulates, from what every loop does now
        const int self = Engine::trackOf(t.source, t.axis & 1);
        modFocus_ = -1;
        for (int track = 0; track < Engine::kNoteTrack; ++track) {
            const LoopStatus& st = live.loops[static_cast<std::size_t>(Engine::sourceOfTrack(track))].lanes[static_cast<std::size_t>(Engine::axisOfTrack(track))].status;
            modEdit_[static_cast<std::size_t>(track)] = {st.modSource, st.modDepth, st.modInverted};
            if (st.modSource == self && modFocus_ < 0)
                modFocus_ = track;
        }
        openMenu({K::ModPick, ch, 0, t.source, t.axis & 1}, live, menuFromController_, menuAt_);
        break;
    }
    case kModStop: {  // every loop it modulates plays its own amplitude again
        const LoopStatus& st = live.loops[static_cast<std::size_t>(t.source)].lanes[static_cast<std::size_t>(t.axis & 1)].status;
        for (int track = 0; track < Engine::kNoteTrack; ++track)
            if ((st.modTargets >> track) & 1u)
                runtime_->setModulation(Engine::sourceOfTrack(track), Engine::axisOfTrack(track), {});
        break;
    }
    case kModSendCc: {
        const LoopStatus& st = live.loops[static_cast<std::size_t>(t.source)].lanes[static_cast<std::size_t>(t.axis & 1)].status;
        runtime_->setModulationSendsCc(t.source, t.axis & 1, !st.modSendsCc);
        break;
    }
    case kResetShape:
        for (int a = 0; a < 2; ++a)
            if (axis < 0 || axis == a)
                runtime_->resetLoopShape(t.source, a);
        break;
    case kEditNotes: openWindow("notes"); break;
    case kCopyMapping:
        copyTo_ = ch;
        copyFrom_ = ch == 0 ? 1 : 0;
        openCopyDialog_ = true;
        break;
    case kRenameClip:
        renameChannel_ = ch;
        renameChain_ = renameSection_ = -1;
        renameInput_ = clipNames_[c][static_cast<std::size_t>(std::clamp(t.slot, 0, Engine::kClipSlots - 1))];
        renameRow_ = -1;
        openRename_ = true;
        renameClipChannel_ = ch;
        renameClipSlot_ = std::clamp(t.slot, 0, Engine::kClipSlots - 1);
        break;
    case kRenameRow:
        renameChain_ = renameSection_ = -1;
        renameInput_ = rowNames_[static_cast<std::size_t>(std::clamp(t.slot, 0, Engine::kClipSlots - 1))];
        openRename_ = true;
        renameRow_ = std::clamp(t.slot, 0, Engine::kClipSlots - 1);
        break;
    case kRowLaunch: launchRow(t.slot, live); return;
    case kRename:
        renameChannel_ = ch;
        renameChain_ = -1;
        renameSection_ = -1;
        renameInput_ = preset_.channelNames[c];
        renameRow_ = renameClipChannel_ = renameClipSlot_ = -1;
        openRename_ = true;
        break;
    case kColour:
        colourChannel_ = ch;
        openColour_ = true;
        break;
    case kExportClip: exportMidi(ch, t.slot); break;
    case kEditClipNotes:  // the clip made the channel's current one (played, if another plays), then its notes
    case kItemEditNotes: {
        int slot = t.slot;
        if (id == kItemEditNotes) {  // a song item: its clip
            const auto* items = songCell(t.slot, t.channel);
            if (!items || t.axis < 0 || t.axis >= static_cast<int>(items->size()))
                break;
            slot = std::clamp((*items)[static_cast<std::size_t>(t.axis)].index, 0, Engine::kClipSlots - 1);
            runtime_->setChannel(ch);
        } else {
            moveSessionCursor(ch, slot, live);
        }
        if (live.currentClip[c] != slot || live.queuedClip[c] >= 0) {
            if ((live.clipsPlaying >> ch) & 1u)
                runtime_->launchClip(ch, slot);
            else
                runtime_->selectClip(ch, slot);
        }
        openWindow("notes");
        break;
    }
    case kItemChange: openSongPick(t.slot, t.channel, t.axis, live, menuFromController_, menuAt_); break;
    case kSongPlay:
        songCursor_.row = t.slot;
        playSongFrom(t.slot, live);
        break;
    case kSongStop: runtime_->stopSong(); break;
    case kSongLoop: runtime_->setSongLoop(!live.song.loop); break;
    case kSectionRename:
        if (t.slot >= 0 && t.slot < static_cast<int>(song_.sections.size())) {
            renameSection_ = t.slot;
            renameChain_ = -1;
            renameInput_ = song_.sections[static_cast<std::size_t>(t.slot)].name;
            renameRow_ = renameClipChannel_ = renameClipSlot_ = -1;
        openRename_ = true;
        }
        break;
    case kSectionRepeats:
        if (t.slot >= 0 && t.slot < static_cast<int>(song_.sections.size())) {
            valueAsk_ = ValueAsk{ValueAsk::Kind::SectionRepeats, -1, t.slot, 0, std::max(song_.sections[static_cast<std::size_t>(t.slot)].repeats, 1)};
            openValueAsk_ = true;
        }
        break;
    case kSectionFit:
        if (t.slot >= 0 && t.slot < static_cast<int>(song_.sections.size())) {
            auto& sec = song_.sections[static_cast<std::size_t>(t.slot)];
            if (sec.fit)
                sec.bars = PerformanceView::songSectionBars(live, sec);  // fixed where it was
            sec.fit = !sec.fit;
            songEdited();
        }
        break;
    case kSectionDuplicate:
        if (t.slot >= 0 && t.slot < static_cast<int>(song_.sections.size())) {
            const midipad::SongSection copy = song_.sections[static_cast<std::size_t>(t.slot)];
            song_.sections.insert(song_.sections.begin() + t.slot + 1, copy);
            songCursor_.row = t.slot + 1;
            songEdited();
        }
        break;
    case kSectionInsert: addSongSection(t.slot); break;
    case kSectionUp:
    case kSectionDown: {
        const int other = t.slot + (id == kSectionUp ? -1 : 1);
        if (t.slot >= 0 && other >= 0 && other < static_cast<int>(song_.sections.size()) && t.slot < static_cast<int>(song_.sections.size())) {
            std::swap(song_.sections[static_cast<std::size_t>(t.slot)], song_.sections[static_cast<std::size_t>(other)]);
            songCursor_.row = other;
            songEdited();
        }
        break;
    }
    case kSectionDelete:
        if (t.slot >= 0 && t.slot < static_cast<int>(song_.sections.size())) {
            if (live.song.playing && live.song.section >= t.slot)
                runtime_->stopSong();
            song_.sections.erase(song_.sections.begin() + t.slot);
            songCursor_.row = std::clamp(t.slot, 0, static_cast<int>(song_.sections.size()));
            songEdited();
        }
        break;
    case kCellEdit:
        songCursor_ = {t.slot, t.channel, 0, true};
        break;
    case kCellClear:
    case kCellCopy:
    case kCellPaste:
        if (auto* items = songCell(t.slot, t.channel)) {
            if (id == kCellCopy) {
                songClipboard_ = *items;
                songClipboardPart_ = false;
            } else if (id == kCellPaste && songClipboardPart_) {  // items picked out of a cell: among those there
                for (const midipad::SongItem& it : songClipboard_)
                    if (static_cast<int>(items->size()) < midipad::kMaxSongItems)
                        placeSongItem(t.slot, t.channel, it, live);
                songEdited();
            } else {
                *items = id == kCellPaste ? songClipboard_ : std::vector<midipad::SongItem>{};
                songEdited();
            }
        }
        break;
    case kRestBarMore:
    case kRestBarLess:
    case kRestBeatMore:
    case kRestBeatLess:
        songAction({id == kRestBarMore || id == kRestBarLess ? PerformanceView::SongAction::Kind::ItemContent : PerformanceView::SongAction::Kind::ItemRepeats,
                    t.slot, t.channel, t.axis, id == kRestBarMore || id == kRestBeatMore ? 1 : -1},
                   live);
        songCursor_ = {t.slot, t.channel, t.axis, true};
        break;
    case kItemGlue:
    case kItemLeft:
    case kItemRight:
    case kItemLoop:
    case kItemKind:
    case kItemDuplicate:
    case kItemDelete:
        if (auto* items = songCell(t.slot, t.channel); items && t.axis >= 0 && t.axis < static_cast<int>(items->size())) {
            midipad::SongItem it = (*items)[static_cast<std::size_t>(t.axis)];
            songCursor_ = {t.slot, t.channel, t.axis, true};
            const double start = PerformanceView::songItemBars(live, t.channel, *items)[static_cast<std::size_t>(t.axis)];
            const int bpb = std::max(live.beatsPerBar, 1);
            const int startBeat = static_cast<int>(std::lround(start * bpb));  // in the section
            if (id == kItemGlue) {
                auto& at = (*items)[static_cast<std::size_t>(t.axis)];
                at.bar = startBeat / bpb;  // a place of its own: where it starts now
                at.beat = startBeat % bpb;
                at.after = !at.after;
            } else if (id == kItemLoop || id == kItemKind) {
                auto& at = (*items)[static_cast<std::size_t>(t.axis)];
                if (id == kItemLoop)
                    at.loop = !at.loop;
                else
                    at.kind = at.kind == midipad::SongItem::Kind::Clip ? midipad::SongItem::Kind::Chain : midipad::SongItem::Kind::Clip;
            } else if (id == kItemDelete) {
                items->erase(items->begin() + t.axis);
                songCursor_.item = std::min(t.axis, static_cast<int>(items->size()));
            } else if (id == kItemDuplicate) {  // right after it
                it.after = true;
                items->insert(items->begin() + t.axis + 1, it);
                songCursor_.item = t.axis + 1;
            } else {
                items->erase(items->begin() + t.axis);
                const int to = std::max(startBeat + (id == kItemLeft ? -bpb : bpb), 0);  // a bar earlier or later, on the same beat
                it.bar = to / bpb;
                it.beat = to % bpb;
                songCursor_.item = placeSongItem(t.slot, t.channel, it, live);
            }
            songEdited();
        }
        break;
    case kSettings:
        if (t.kind == K::Control)
            performance_.activateFocus();  // its window, as a click
        else if (!t.spec.empty())
            openWindow(t.spec);
        break;
    default: break;
    }
}

void GuiApp::enterSubmenu(int entry) {
    if (!menu_ || entry < 0 || entry >= static_cast<int>(menu_->items.size()) || menu_->items[static_cast<std::size_t>(entry)].sub.empty())
        return;
    MenuEntry parent = menu_->items[static_cast<std::size_t>(entry)];
    menu_->parents.emplace_back(menu_->title, std::move(menu_->items));
    const auto nl = menu_->title.find('\n');
    menu_->title = menu_->title.substr(0, nl) + "  \xE2\x80\xBA  " + parent.label + (nl == std::string::npos ? "" : menu_->title.substr(nl));
    menu_->items = std::move(parent.sub);
    menu_->selected = 0;
    while (menu_->selected < static_cast<int>(menu_->items.size()) - 1 && !menu_->items[static_cast<std::size_t>(menu_->selected)].enabled)
        ++menu_->selected;
}

// The controller in a menu: up and down (D-pad or a stick) go from item to
// item, Select chooses (a submenu: opens it), Back goes up from a submenu,
// Menu or Back closes.
void GuiApp::menuEvent(Engine::SessionEvent e, const LiveState& live) {
    using E = Engine::SessionEvent;
    if (!menu_)
        return;
    const int n = static_cast<int>(menu_->items.size());
    auto move = [&](int by) {
        for (int tries = 0; tries < n; ++tries) {
            menu_->selected = (menu_->selected + by + n) % n;
            if (menu_->items[static_cast<std::size_t>(menu_->selected)].enabled)
                break;
        }
    };
    // In the modulation menu, a loop it modulates under the cursor takes the depth and direction rows.
    auto focusRow = [&] {
        if (menu_->target.kind != PerformanceView::MenuTarget::Kind::ModPick)
            return;
        const MenuEntry& row = menu_->items[static_cast<std::size_t>(menu_->selected)];
        if (row.id >= kModTarget && row.checked && modFocus_ != row.id - kModTarget) {
            modFocus_ = row.id - kModTarget;
            const int selected = menu_->selected;
            fillModMenu(*menu_, live);
            menu_->selected = selected;
        }
    };
    switch (e) {
    case E::Down:
    case E::ValueDown:
        move(1);
        focusRow();
        break;
    case E::Up:
    case E::ValueUp:
        move(-1);
        focusRow();
        break;
    case E::ValueLeft:
    case E::ValueRight:  // the depth, down or up
        if (menu_->items[static_cast<std::size_t>(menu_->selected)].id == kModDepth)
            changeModulation(kModDepth, e == E::ValueRight ? 2 : -2, live);
        break;
    case E::Press: {
        const MenuEntry entry = menu_->items[static_cast<std::size_t>(menu_->selected)];
        const PerformanceView::MenuTarget target = menu_->target;
        if (entry.enabled && !entry.sub.empty()) {
            enterSubmenu(menu_->selected);
        } else if (entry.enabled && keepsModMenu(entry.id)) {
            changeModulation(entry.id, 1, live);
        } else if (entry.enabled) {
            menuFromController_ = true;
            menuAt_ = menu_->at;
            undoWhat_ = menuWhat(*menu_, entry.label);
            menu_.reset();
            runMenu(target, entry.id, live);
            undoWhat_.clear();
        }
        break;
    }
    case E::Back:
        if (!menu_->parents.empty()) {  // up from a submenu
            auto [title, items] = std::move(menu_->parents.back());
            menu_->parents.pop_back();
            menu_->title = std::move(title);
            menu_->items = std::move(items);
            menu_->selected = 0;
            while (menu_->selected < static_cast<int>(menu_->items.size()) - 1 && !menu_->items[static_cast<std::size_t>(menu_->selected)].enabled)
                ++menu_->selected;
            break;
        }
        menu_.reset();
        break;
    case E::Menu: menu_.reset(); break;
    default: break;
    }
}

// The same small menu for the mouse (where it clicked) and the controller
// (beside its cursor, the selected item lit; the stick moves it, Select
// chooses, Menu or Back closes).
void GuiApp::drawContextMenu(const LiveState& live) {
    if (!menu_->opened) {
        ImGui::OpenPopup("##context");
        menu_->opened = true;
        if (menu_->fromController) {  // beside the controller's cursor: under it, or above when there is no room
            const auto [a, b] = performance_.cursorBox();
            const float h = ImGui::GetFrameHeightWithSpacing() * static_cast<float>(menu_->items.size() + 2), gap = ImGui::GetFontSize() * 0.25f;
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            menu_->above = b.y + gap + h >= vp->Pos.y + vp->Size.y;  // no room under it: above, its bottom against the item
            menu_->at = menu_->above ? ImVec2(a.x, a.y - gap) : ImVec2(a.x, b.y + gap);
        }
    }
    {  // inside the window: moved left when it would go past its right edge
        float w = ImGui::CalcTextSize(menu_->title.c_str()).x + ImGui::GetFontSize() * 2;
        for (const MenuEntry& item : menu_->items)
            w = std::max(w, ImGui::CalcTextSize(item.label.c_str()).x +
                                (item.shortcut.empty() ? 0.0f : ImGui::CalcTextSize(item.shortcut.c_str()).x + ImGui::GetFontSize() * 2));
        w += ImGui::GetStyle().WindowPadding.x * 2 + ImGui::GetFontSize();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        menu_->at.x = std::max(vp->Pos.x, std::min(menu_->at.x, vp->Pos.x + vp->Size.x - w));
    }
    ImGui::SetNextWindowPos(menu_->at, ImGuiCond_Appearing, ImVec2(0, menu_->above ? 1.0f : 0.0f));
    if (ImGui::BeginPopup("##context")) {
        const std::string title = menu_->title.substr(0, menu_->title.find('\n'));
        const auto nl = menu_->title.find('\n');
        ImGui::TextDisabled("%s%s%s", title.c_str(), nl == std::string::npos ? "" : "  \xC2\xB7  ",
                            nl == std::string::npos ? "" : menu_->title.substr(nl + 1).c_str());
        ImGui::Separator();
        std::optional<int> chosen;
        int opened = -1;  // a submenu chosen
        float keyedW = 0;  // the entries with a key: as wide as the widest, their keys in a column
        for (const MenuEntry& item : menu_->items)
            if (!item.shortcut.empty())
                for (const MenuEntry& other : menu_->items)
                    keyedW = std::max(keyedW, ImGui::CalcTextSize(other.label.c_str()).x + ImGui::GetFontSize() * 2 +
                                                  (other.shortcut.empty() ? 0.0f : ImGui::CalcTextSize(other.shortcut.c_str()).x));
        keyedW = std::max(keyedW, ImGui::GetContentRegionAvail().x);
        for (int k = 0; k < static_cast<int>(menu_->items.size()); ++k) {
            const MenuEntry& item = menu_->items[static_cast<std::size_t>(k)];
            if (item.separator) {
                ImGui::Separator();
                continue;
            }
            const bool submenu = !item.sub.empty();
            const ImGuiSelectableFlags flags = (item.enabled ? 0 : ImGuiSelectableFlags_Disabled) |
                                               (keepsModMenu(item.id) || submenu ? ImGuiSelectableFlags_NoAutoClosePopups : 0);
            const float shortcutW = item.shortcut.empty() ? 0.0f : ImGui::CalcTextSize(item.shortcut.c_str()).x;
            if (item.danger && item.enabled)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PerformanceView::Theme::kRed));
            const bool clicked = ImGui::Selectable(item.label.c_str(), menu_->fromController && k == menu_->selected, flags,
                                                   ImVec2(shortcutW > 0 ? keyedW : 0.0f, 0.0f));  // room for its key on the right
            if (item.danger && item.enabled)
                ImGui::PopStyleColor();
            if (clicked && submenu)
                opened = k;
            else if (clicked)
                chosen = item.id;
            if (shortcutW > 0)
                ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMax().x - shortcutW, ImGui::GetItemRectMin().y + ImGui::GetStyle().ItemSpacing.y * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), item.shortcut.c_str());
            // In the list of what a song item plays, 1 to 8 choose a clip, A to H a chain.
            if (menu_->target.kind == PerformanceView::MenuTarget::Kind::SongPick && item.enabled && item.id >= kPick && item.id < kPick + 16) {
                const int n = item.id - kPick;
                if (ImGui::IsKeyPressed(n < 8 ? static_cast<ImGuiKey>(ImGuiKey_1 + n) : static_cast<ImGuiKey>(ImGuiKey_A + n - 8), false)) {
                    chosen = item.id;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (item.checked) {  // drawn: the font has no check mark
                const ImVec2 r = ImGui::GetItemRectMin();
                const float sz = ImGui::GetFontSize();
                const float x = r.x + ImGui::CalcTextSize(item.label.c_str()).x + sz * 0.6f, y = r.y + sz * 0.5f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddLine(ImVec2(x, y), ImVec2(x + sz * 0.25f, y + sz * 0.25f), PerformanceView::Theme::kGreen, sz * 0.12f);
                dl->AddLine(ImVec2(x + sz * 0.25f, y + sz * 0.25f), ImVec2(x + sz * 0.7f, y - sz * 0.3f), PerformanceView::Theme::kGreen, sz * 0.12f);
            }
        }
        ImGui::EndPopup();
        if (opened >= 0) {
            enterSubmenu(opened);
        } else if (chosen && keepsModMenu(*chosen)) {
            changeModulation(*chosen, 1, live);
        } else if (chosen) {
            const PerformanceView::MenuTarget target = menu_->target;
            for (const MenuEntry& entry : menu_->items)
                if (entry.id == *chosen)
                    undoWhat_ = menuWhat(*menu_, entry.label);
            menuFromController_ = menu_->fromController;
            menuAt_ = ImGui::GetIO().MousePos;
            menu_.reset();
            runMenu(target, *chosen, live);
            undoWhat_.clear();
        }
    } else {
        menu_.reset();  // closed
    }
}

std::string GuiApp::bindingName(ActionType type, const char* fallback) const {
    for (int b = 0; b < kButtonCount; ++b)
        if (preset_.base.buttons[static_cast<std::size_t>(b)].type == type)
            return label(static_cast<Button>(b));
    return fallback;
}

// While the Mode control is held: what the controller does on this screen, with its buttons.
void GuiApp::drawHelpOverlay() {
    using T = PerformanceView::Theme;
    const std::string mode = bindingName(ActionType::SessionScreen, label(Button::Guide).c_str()), rec = bindingName(ActionType::LoopRecord, "Rec"),
                      shift = bindingName(ActionType::Shift, "Shift");
    std::vector<std::string> select = settings_.navSelect >= 0 ? std::vector<std::string>{buttonName(settings_.navSelect, "")}
                                                               : std::vector<std::string>{label(Button::LeftStick), label(Button::RightStick)};
    const std::string menu = buttonName(settings_.navMenu, ""), back = buttonName(settings_.navBack, "");
    const std::string unset = "(none set)";
    std::vector<std::pair<std::vector<std::string>, std::string>> rows;
    const std::string closeHint = "let go of " + mode + " to close";
    if (sessionView_) {
        rows = {{{"Left stick", "D-pad"}, "move the cursor (down past the clips: stop, mute, solo, then the chain)"},
                {select, "launch or stop the clip; on stop, M, S: press it; in the chain: start there, or go on; on CHAIN: play or stop it"},
                {{menu.empty() ? "Menu" : menu}, menu.empty() ? "the menu " + unset : "the menu: duplicate, clear, add to the chain\xE2\x80\xA6"},
                {{"Right stick"}, "left / right: the channel; in the chain: the clip (up, down), its repeats; on CHAIN: another chain (up, down), what comes after it"},
                {{rec}, "record into the slot under the cursor"},
                {{back.empty() ? "Back" : back}, back.empty() ? "close a menu " + unset : "close a menu"},
                {{mode}, "the song;  " + shift + " + " + mode + ": edit mode"}};
        performance_.drawHelp("SESSION", T::kGreen, closeHint, rows);
    } else if (songView_) {
        rows = {{{"Left stick", "D-pad"}, "move the cursor; in a cell's editor, from item to item"},
                {select, "on a section's name: play from there (on the next bar); on a cell: edit it; on +: add"},
                {{menu.empty() ? "Menu" : menu}, menu.empty() ? "the menu " + unset : "the menu: rename, length, duplicate, move, loop\xE2\x80\xA6"},
                {{"Right stick"}, "on a name: its length;  on an item: the clip or chain (up, down), its repeats (left, right)"},
                {{back.empty() ? "Back" : back}, back.empty() ? "close " + unset : "close the editor, or back to the main screen"},
                {{mode}, "back to the main screen"}};
        performance_.drawHelp("SONG", IM_COL32(0xFF, 0xB0, 0x20, 255), closeHint, rows);
    } else if (editView_) {
        rows = {{{"Left stick", "D-pad"}, "move the cursor; in a window, from field to field"},
                {{"Right stick"}, "left / right: the channel;  up / down: a lane's length"},
                {select, "open (a control's window, the note editor); in a window, change the field"},
                {{menu.empty() ? "Menu" : menu}, menu.empty() ? "the menu " + unset : "the menu: lock, clear, back to 1x\xE2\x80\xA6"},
                {{back.empty() ? "Back" : back}, back.empty() ? "close, leave " + unset : "close the window, or leave edit mode"},
                {{shift, mode}, "leave edit mode"}};
        performance_.drawHelp("EDIT", PerformanceView::Theme::kViolet, closeHint, rows);
    } else {
        rows = {{{rec}, "held: record what you play or move;  a tap: stop the loops, or overdub"},
                {{shift}, "the Shift layer; with a stick or a finger: the loops' lengths"},
                {{shift, label(Button::LeftStick)}, "a stick: lengths, amplitudes or offsets"},
                {{mode}, "the session screen (clips and chains); again: the song"},
                {{shift, mode}, "edit mode: set things up from the controller"}};
        performance_.drawHelp("MAIN SCREEN", T::kBlue, closeHint, rows);
    }
}

void GuiApp::toggleFullscreen() {
    const bool fullscreen = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(window_, !fullscreen);
}

void GuiApp::drawPerformance(const LiveState& live) {
    using Theme = PerformanceView::Theme;
    liveChains_ = live.chains;
    undoWhat_.clear();
    performance_.syncSounding(live.soundingNotes, ImGui::GetTime());
    if (sessionView_ || songView_)
        screenKeys(live);
    else
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    auto color = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    const float em = ImGui::GetFontSize();
    auto px = [em](float v) { return v * em / 16.0f; };
    ImFont* bold = uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont();
    ImFont* medium = uiFonts_.medium ? uiFonts_.medium : ImGui::GetFont();

    ImGui::PushStyleColor(ImGuiCol_Text, color(Theme::kText));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, color(Theme::kControl));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, color(IM_COL32(0x47, 0x4C, 0x54, 255)));
    ImGui::PushStyleColor(ImGuiCol_Button, color(Theme::kControl));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(IM_COL32(0x47, 0x4C, 0x54, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color(IM_COL32(0x52, 0x58, 0x62, 255)));
    ImGui::PushStyleColor(ImGuiCol_Header, color(IM_COL32(0x3F, 0x46, 0x52, 255)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color(IM_COL32(0x38, 0x3D, 0x45, 255)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, color(IM_COL32(0x45, 0x4C, 0x58, 255)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color(Theme::kPanel));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(Theme::kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, color(Theme::kControlBorder));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, color(Theme::kGreen));
    constexpr int kColors = 13;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(5));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12), px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(6));
    constexpr int kVars = 4;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Top bar: everything needed on stage, one click away.
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float winW = ImGui::GetWindowWidth();
    const float barH = px(50);
    if (look() == PerformanceView::Look::Classic)  // a phosphor look's bar is on the background
        dl->AddRectFilled(winPos, ImVec2(winPos.x + winW, winPos.y + barH), Theme::kTopBar);
    ImGui::SetCursorScreenPos(ImVec2(winPos.x + px(8), winPos.y + px(9)));
    ImGui::PushFont(bold, 13.0f);

    {  // Menu button with a hamburger icon.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##menu", ImVec2(px(32), px(32))))
            ImGui::OpenPopup("menu");
        for (int i = 0; i < 3; ++i)
            dl->AddLine(ImVec2(p.x + px(8), p.y + px(9.0f + 7.0f * static_cast<float>(i))),
                        ImVec2(p.x + px(24), p.y + px(9.0f + 7.0f * static_cast<float>(i))), Theme::kText, px(2));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(6));
    const bool menuOpen = ImGui::BeginPopup("menu");
    ImGui::PopStyleVar(2);
    if (menuOpen) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(12), px(14)));
        ImGui::PushFont(medium, 15.0f);
        if (ImGui::MenuItem("New Song"))
            openNewSongDialog_ = true;
        if (ImGui::MenuItem("Open…")) {
            sessionList_ = sessionNames();
            sessionChoice_ = -1;
            openSessionDialog_ = true;
        }
        if (ImGui::MenuItem("Save"))
            saveSession();
        if (ImGui::MenuItem("Save as…")) {
            nameDialog_ = NameDialog::SessionSaveAs;
            nameInput_ = sessionName_.value_or("Session");
            nameError_.clear();
            openNameDialog_ = true;
        }
        if (ImGui::MenuItem("Export the song as MIDI", nullptr, false, !song_.sections.empty()))
            exportMidi(-1, 0);
        ImGui::Separator();
        if (ImGui::MenuItem("Settings", nullptr, false)) {
            refreshPorts();
            virmidiStatus_ = virmidi::status();
            if (virmidiStatus_.loaded && virmidiPortCount() > 0)
                virmidiPorts_ = std::min(virmidiPortCount(), virmidi::kMaxPorts);
            virmidiAtStartup_ = virmidiStatus_.atStartup;
            optionsOpen_ = true;
            optionsAppearing_ = true;
            overlaySpec_.reset();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Themes")) {
            static constexpr std::pair<AppSettings::Theme, const char*> kThemes[] = {
                {AppSettings::Theme::Default, "Default"}, {AppSettings::Theme::PhosphorBlue, "Phosphor Blue"}, {AppSettings::Theme::PhosphorGreen, "Phosphor Green"}};
            for (const auto& [theme, name] : kThemes)
                if (ImGui::MenuItem(name, nullptr, settings_.theme == theme) && settings_.theme != theme) {
                    settings_.theme = theme;
                    persistSettings();
                }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("All notes off"))
            runtime_->allNotesOff();
        if (ImGui::MenuItem("Fullscreen", "F11", (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0))
            toggleFullscreen();
        if (ImGui::MenuItem("About…"))
            openAboutDialog_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q"))
            guard([this] { quitAsked(); });
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, px(12));
    {
        // ARCADANCE in pixel letters, violet to pink, a hard shadow: 3 points a pixel.
        static constexpr const char* kGlyphs[6][7] = {
            {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},  // A
            {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"},  // R
            {".####", "#....", "#....", "#....", "#....", "#....", ".####"},  // C
            {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."},  // D
            {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"},  // N
            {"#####", "#....", "#....", "####.", "#....", "#....", "#####"},  // E
        };
        static constexpr int kWord[] = {0, 1, 2, 0, 3, 0, 4, 2, 5};  // A R C A D A N C E
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cell = std::round(px(3));
        const float wordW = cell * (9 * 6 - 1 + 1), top = std::round(p.y + (px(32) - cell * 8) * 0.5f - px(2));
        auto pixel = [&](float x, float y, ImU32 col) { dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell, y + cell), col); };
        for (int pass = 0; pass < 2; ++pass)  // the shadow, then the letters
            for (int k = 0; k < 9; ++k)
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col) {
                        if (kGlyphs[kWord[k]][row][col] != '#')
                            continue;
                        const float x = std::round(p.x) + cell * static_cast<float>(k * 6 + col), y = top + cell * static_cast<float>(row);
                        if (pass == 0) {
                            pixel(x + cell, y + cell, IM_COL32(10, 10, 14, 255));
                        } else {
                            const float t = static_cast<float>(row) / 6.0f;  // violet (176, 120, 255) to pink (255, 70, 140)
                            pixel(x, y, IM_COL32(static_cast<int>(176 + 79 * t), static_cast<int>(120 - 50 * t), static_cast<int>(255 - 115 * t), 255));
                        }
                    }
        ImGui::Dummy(ImVec2(wordW + px(14), px(32)));
    }
    // Small arrow buttons drawn as triangles, for the preset.
    auto arrow = [&](const char* id, ImVec2 at, float w, float h, bool left) {
        ImGui::SetCursorScreenPos(at);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        const ImU32 col = ImGui::IsItemHovered() ? Theme::kText : Theme::kLabel;
        const float cx = at.x + w * 0.5f, cy = at.y + h * 0.5f, s = px(4.5f);
        if (left)
            dl->AddTriangleFilled(ImVec2(cx + s, cy - s * 1.2f), ImVec2(cx + s, cy + s * 1.2f), ImVec2(cx - s, cy), col);
        else
            dl->AddTriangleFilled(ImVec2(cx - s, cy - s * 1.2f), ImVec2(cx - s, cy + s * 1.2f), ImVec2(cx + s, cy), col);
        return clicked;
    };
    [[maybe_unused]] auto caption = [&](const char* s) {
        ImGui::SameLine(0, px(16));
        ImGui::PushFont(medium, 12.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(color(Theme::kLabel), "%s", s);
        ImGui::PopFont();
        ImGui::SameLine(0, px(8));
    };
    // The screens, in the Mode button's order, as icons: a controller (the main
    // screen; violet in the controller's edit mode), four squares (the session),
    // lanes of blocks (the song). A click shows one.
    // From the screens to the preset, a group centred in the bar (never over the logo);
    // SAVE, when the preset changed, at the right end.
    {
        ImGui::SameLine(0, px(4));
        const float groupW = px(36 * 3 + 4 * 2) + px(12) + px(32 + 6 + 32) + (songView_ ? px(10 + 70) : 0.0f) + px(10 + 166 + 6 + 44) + px(12) +
                             px(22 * 2 + 186);
        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(std::max(after.x, winPos.x + (winW - groupW) * 0.5f), after.y));
    }
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        static constexpr const char* kTips[] = {"Play: the controller and its loops (Tab)", "Session: the clips of every channel (Tab)",
                                                "Song: the sections of the song, the clips and chains each channel plays in them (Tab)"};
        const int shown = songView_ ? 2 : sessionView_ ? 1 : 0;
        float x = at.x;
        for (int v = 0; v < 3; ++v) {
            const float w = px(36), h = px(32);
            ImGui::SetCursorScreenPos(ImVec2(x, at.y));
            ImGui::PushID(v);
            const bool clicked = ImGui::InvisibleButton("##view", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetItemTooltip("%s", v == 0 && editView_ ? "Edit mode: the controller moves a cursor on the screen (Back: leave it)" : kTips[v]);
            ImGui::PopID();
            const bool on = v == shown;
            const ImU32 accent = v == 0 && editView_ ? Theme::kViolet : Theme::kGreen;
            // Its background, as drawn (lit: the accent faint over the bar), for the holes in the controller.
            auto blend = [](ImU32 under, ImU32 over, float a) {
                auto ch = [&](int shift) {
                    const float u = static_cast<float>((under >> shift) & 0xFF), o = static_cast<float>((over >> shift) & 0xFF);
                    return static_cast<ImU32>(std::lround(u + (o - u) * a)) << shift;
                };
                return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | IM_COL32_A_MASK;
            };
            const ImU32 fill = on ? blend(Theme::kTopBar, accent, 60.0f / 255.0f) : hovered ? Theme::kControlBorder : Theme::kControl;
            dl->AddRectFilled(ImVec2(x, at.y), ImVec2(x + w, at.y + h), fill, px(5));
            dl->AddRect(ImVec2(x, at.y), ImVec2(x + w, at.y + h), on || (v == 0 && editView_) ? accent : Theme::kControlBorder, px(5), px(on ? 1.3f : 1.0f));
            const ImU32 ink = on || (v == 0 && editView_) ? accent : Theme::kLabel;
            const float cx = x + w * 0.5f, cy = at.y + h * 0.5f;
            if (v == 0) {  // a controller: its body and grips filled, its cross and four buttons cut out
                dl->AddRectFilled(ImVec2(cx - px(11), cy - px(6)), ImVec2(cx + px(11), cy + px(2)), ink, px(4));
                dl->AddEllipseFilled(ImVec2(cx - px(8.5f), cy + px(2.5f)), ImVec2(px(4.5f), px(5.5f)), ink);
                dl->AddEllipseFilled(ImVec2(cx + px(8.5f), cy + px(2.5f)), ImVec2(px(4.5f), px(5.5f)), ink);
                dl->AddRectFilled(ImVec2(cx - px(9), cy - px(2.6f)), ImVec2(cx - px(5), cy - px(1.4f)), fill);
                dl->AddRectFilled(ImVec2(cx - px(7.6f), cy - px(4)), ImVec2(cx - px(6.4f), cy), fill);
                for (const auto& [dx, dy] : {std::pair{0.0f, -2.2f}, {2.2f, 0.0f}, {0.0f, 2.2f}, {-2.2f, 0.0f}})
                    dl->AddCircleFilled(ImVec2(cx + px(7.0f + dx), cy + px(-2.0f + dy)), px(0.9f), fill);
            } else if (v == 1) {  // four squares
                for (int k = 0; k < 4; ++k) {
                    const float gx = cx + (k % 2 ? px(1.5f) : -px(8.5f)), gy = cy + (k / 2 ? px(1.5f) : -px(8.5f));
                    dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + px(7), gy + px(7)), ink, px(1.5f));
                }
            } else {  // lanes of blocks, as an arrangement
                static constexpr float kLanes[4][3] = {{-8, 1, -7}, {-3, 8, -1.5f}, {-8, -1, 4}, {2, 8, 4}};  // x from, x to, y: a block each
                for (const auto& lane : kLanes)
                    dl->AddRectFilled(ImVec2(cx + px(lane[0]), cy + px(lane[2])), ImVec2(cx + px(lane[1]), cy + px(lane[2] + 3.5f)), ink, px(1.2f));
            }
            if (clicked && !on)
                showView(v, live);
            x += w + px(4);
        }
        ImGui::SetCursorScreenPos(ImVec2(x - px(4), at.y));
        ImGui::Dummy(ImVec2(0, px(32)));
    }
    float clockRight = 0, presetRight = 0;
    {
        // Play and Stop, the same for the song and the clips: Play plays the
        // song in the song screen, the clips elsewhere, and nothing while
        // something plays; Stop stops whatever plays. Play lit green while
        // something plays.
        const bool anyLoop = live.loopChannels != 0 || live.song.playing;
        const bool loopsPlaying = (live.loopChannels != 0 && !live.loopsPaused) || live.song.playing;
        auto transport = [&](const char* id, bool play) {
            ImGui::SameLine(0, play ? px(12) : px(6));
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float side = px(32);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
            const bool hovered = ImGui::IsItemHovered();
            const bool lit = play && loopsPlaying;
            dl->AddRectFilled(q, ImVec2(q.x + side, q.y + side),
                              lit ? (Theme::kGreen & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 60)
                                  : hovered ? Theme::kControlBorder : Theme::kControl, px(5));
            dl->AddRect(q, ImVec2(q.x + side, q.y + side), lit ? Theme::kGreen : Theme::kControlBorder, px(5), px(lit ? 1.3f : 1.0f));
            const ImVec2 c(q.x + side * 0.5f, q.y + side * 0.5f);
            if (play) {
                const float t = px(6.5f);
                dl->AddTriangleFilled(ImVec2(c.x - t * 0.75f, c.y - t), ImVec2(c.x - t * 0.75f, c.y + t), ImVec2(c.x + t, c.y),
                                      lit ? Theme::kGreen : Theme::kLabel);
            } else {
                const float t = px(5.5f);
                dl->AddRectFilled(ImVec2(c.x - t, c.y - t), ImVec2(c.x + t, c.y + t),
                                  anyLoop && !loopsPlaying ? Theme::kText : Theme::kLabel, px(1.5f));
            }
            return clicked;
        };
        const auto [songSection, songOffset] = songView_ ? songStart(live) : std::pair<int, int>{-1, 0};
        runtime_->setTransportSong(songSection, songOffset);  // for a Link session starting
        if (transport("##loopsplay", true))
            runtime_->transportPlay(songSection, songOffset);
        ImGui::SetItemTooltip(songView_ ? "Plays the song from the section under the cursor (nothing while something plays)"
                                        : "Plays every channel's clips again, from their start (nothing while something plays)");
        if (transport("##loopsstop", false)) {
            if (loopsPlaying)
                runtime_->transportStop();
            else
                runtime_->allNotesOff();  // pressed again: every note off
        }
        ImGui::SetItemTooltip("Stops what plays: the song, or every channel's loops, keeping them; again: every note off");
        // The song screen: where the song is, its bar and beat (stopped, a dash).
        if (songView_) {
            ImGui::SameLine(0, px(10));
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float pw = px(70), ph = px(32);
            ImGui::Dummy(ImVec2(pw, ph));
            ImGui::SetItemTooltip("Bar and beat of the song");
            dl->AddRectFilled(q, ImVec2(q.x + pw, q.y + ph), IM_COL32(0x10, 0x11, 0x13, 255), px(5));
            dl->AddRect(q, ImVec2(q.x + pw, q.y + ph), Theme::kControlBorder, px(5), px(1));
            const int bpb = std::max(live.beatsPerBar, 1);
            std::string where = "\xE2\x80\x93";
            if (live.song.playing) {
                const auto whole = static_cast<long long>(std::floor(std::max(live.song.beat, 0.0)));
                long long before = 0;  // the bars of the sections before the one playing
                for (int i = 0; i < std::min(live.song.section, static_cast<int>(song_.sections.size())); ++i)
                    before += PerformanceView::songSectionBars(live, song_.sections[static_cast<std::size_t>(i)]) *
                              std::clamp(song_.sections[static_cast<std::size_t>(i)].repeats, 1, midipad::kMaxSectionRepeats);
                if (live.song.section >= 0 && live.song.section < static_cast<int>(song_.sections.size()))  // its times played before this one
                    before += static_cast<long long>(live.song.pass) * PerformanceView::songSectionBars(live, song_.sections[static_cast<std::size_t>(live.song.section)]);
                where = std::to_string(before + whole / bpb + 1) + "." + std::to_string(whole % bpb + 1);
            }
            const ImVec2 wsz = bold->CalcTextSizeA(px(18), FLT_MAX, 0, where.c_str());
            dl->AddText(bold, px(18), ImVec2(q.x + (pw - wsz.x) * 0.5f, q.y + (ph - wsz.y) * 0.5f), live.song.playing ? Theme::kGreen : Theme::kDim,
                        where.c_str());
        }
        // The clock: FREE, or its tempo (the wheel turns it, a double-click
        // types it, TAP taps it) and the beats of the bar; a click elsewhere
        // on it turns it off or on again.
        {
            ImGui::SameLine(0, px(10));
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float lw = px(166), lh = px(32);
            const bool clicked = ImGui::InvisibleButton("##clock", ImVec2(lw, lh));
            const bool hovered = ImGui::IsItemHovered();
            const bool on = live.clock != ClockMode::Free;
            const bool link = on ? live.clock == ClockMode::Link : settings_.lastClock == AppSettings::Clock::Link;
            const char* label = link ? "LINK" : "CLOCK";
            dl->AddRectFilled(q, ImVec2(q.x + lw, q.y + lh),
                              on ? (Theme::kGreen & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 40) : hovered ? Theme::kControlBorder : Theme::kControl,
                              px(5));
            dl->AddRect(q, ImVec2(q.x + lw, q.y + lh), on ? Theme::kGreen : Theme::kControlBorder, px(5), px(on ? 1.3f : 1.0f));
            const float cy = q.y + lh * 0.5f;
            const ImVec2 lsz = bold->CalcTextSizeA(px(13), FLT_MAX, 0, label);
            dl->AddText(bold, px(13), ImVec2(q.x + px(10), cy - lsz.y * 0.5f), on ? Theme::kGreen : Theme::kLabel, label);
            char tempo[24], info[48];
            std::snprintf(tempo, sizeof tempo, "%.1f BPM", live.tempo);
            if (on && link)
                std::snprintf(info, sizeof info, "%s \xC2\xB7 %d", tempo, live.linkPeers);
            else
                std::snprintf(info, sizeof info, "%s", on ? tempo : "FREE");
            if (!on)
                std::snprintf(info, sizeof info, "FREE \xC2\xB7 %.1f", settings_.tempo);
            const float tx = q.x + px(18) + lsz.x;
            const ImVec2 isz = medium->CalcTextSizeA(px(13), FLT_MAX, 0, info);
            const float tw = medium->CalcTextSizeA(px(13), FLT_MAX, 0, tempo).x;
            const bool overTempo = on && hovered && io.MousePos.x >= tx - px(4) && io.MousePos.x <= tx + tw + px(4);
            if (overTempo)
                dl->AddRectFilled(ImVec2(tx - px(4), q.y + px(5)), ImVec2(tx + tw + px(4), q.y + lh - px(5)), IM_COL32(255, 255, 255, 22), px(3));
            if (!editingTempo_)
                dl->AddText(medium, px(13), ImVec2(tx, cy - isz.y * 0.5f), on ? Theme::kText : Theme::kDim, info);
            if (on) {  // the beats of the bar: the one playing lit, the first white
                const int beats = std::clamp(live.beatsPerBar, 1, 8);
                const int now = static_cast<int>(std::floor(live.beat)) % std::max(live.beatsPerBar, 1);
                // In the room the text leaves, narrower when the bar has many beats.
                float sq = px(8), gap = px(4);
                const float room = q.x + lw - px(10) - (tx + isz.x + px(8));
                const float needed = static_cast<float>(beats) * (sq + gap) - gap;
                if (needed > room && room > 0) {
                    const float scale = std::max(room / needed, 0.25f);
                    sq *= scale;
                    gap *= scale;
                }
                float x = q.x + lw - px(10) - static_cast<float>(beats) * (sq + gap) + gap;
                for (int b = 0; b < beats; ++b, x += sq + gap) {
                    const ImU32 c = b == now ? (b == 0 ? IM_COL32_WHITE : Theme::kGreen) : IM_COL32(0x3A, 0x3E, 0x45, 255);
                    dl->AddRectFilled(ImVec2(x, cy - sq * 0.5f), ImVec2(x + sq, cy + sq * 0.5f), c, px(1.5f));
                }
            }
            if (overTempo && io.MouseWheel != 0.0f) {  // the wheel: 1 BPM a notch, 0.1 with Shift; faster turned quickly
                tempoWheelRest_ += io.MouseWheel;
                const int notches = static_cast<int>(tempoWheelRest_);
                if (notches != 0) {
                    tempoWheelRest_ -= static_cast<float>(notches);
                    const double nowT = ImGui::GetTime();
                    tempoWheelStreak_ = nowT - lastTempoWheel_ < 0.08 ? tempoWheelStreak_ + 1 : 0;
                    lastTempoWheel_ = nowT;
                    const double step = io.KeyShift ? 0.1 : std::min(1.0 + tempoWheelStreak_ / 3, 5.0);
                    applyTempo(live.tempo + notches * step);
                }
            }
            if (overTempo && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                editingTempo_ = true;
                tempoEditAppearing_ = true;
                char text[16];
                std::snprintf(text, sizeof text, "%.1f", live.tempo);
                tempoText_ = text;
            } else if (clicked && !overTempo) {
                if (settings_.clock != AppSettings::Clock::Free) {
                    settings_.lastClock = settings_.clock;
                    settings_.clock = AppSettings::Clock::Free;
                } else {
                    settings_.clock = settings_.lastClock;
                }
                applySync();
                persistSettings();
            }
            if (!editingTempo_)
                ImGui::SetItemTooltip(on ? (link ? "Ableton Link: tempo and beat shared with the session"
                                                 : "Internal clock: Arcadance's own tempo and beat")
                                         : "Free: Arcadance keeps no beat");
            if (editingTempo_) {  // typing the tempo, over it
                ImGui::SetCursorScreenPos(ImVec2(tx - px(1), q.y + px(4)));
                ImGui::SetNextItemWidth(px(78));
                if (tempoEditAppearing_)
                    ImGui::SetKeyboardFocusHere();
                const bool enter = ImGui::InputText("##tempoedit", &tempoText_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                                        ImGuiInputTextFlags_CharsDecimal);
                if (enter) {
                    char* end = nullptr;
                    const double bpm = std::strtod(tempoText_.c_str(), &end);
                    if (end != tempoText_.c_str())
                        applyTempo(bpm);
                    editingTempo_ = false;
                } else if (!tempoEditAppearing_ && !ImGui::IsItemActive()) {
                    editingTempo_ = false;  // Escape, or a click elsewhere
                }
                tempoEditAppearing_ = false;
            }
            // TAP: the tempo of the last taps, from the second.
            ImGui::SetCursorScreenPos(ImVec2(q.x + lw + px(6), q.y));
            const float tapW = px(44);
            const bool tapped = ImGui::InvisibleButton("##tap", ImVec2(tapW, lh));
            const bool tapHovered = ImGui::IsItemHovered();
            const bool tapDown = ImGui::IsItemActive();
            dl->AddRectFilled(ImVec2(q.x + lw + px(6), q.y), ImVec2(q.x + lw + px(6) + tapW, q.y + lh),
                              on && tapDown ? (Theme::kGreen & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 90)
                              : on && tapHovered ? Theme::kControlBorder : Theme::kControl, px(5));
            dl->AddRect(ImVec2(q.x + lw + px(6), q.y), ImVec2(q.x + lw + px(6) + tapW, q.y + lh), Theme::kControlBorder, px(5), px(1));
            const ImVec2 tsz = bold->CalcTextSizeA(px(12), FLT_MAX, 0, "TAP");
            dl->AddText(bold, px(12), ImVec2(q.x + lw + px(6) + (tapW - tsz.x) * 0.5f, cy - tsz.y * 0.5f), on ? Theme::kText : Theme::kDim, "TAP");
            if (tapped && on)
                if (const auto bpm = tap_.tap(SDL_GetTicksNS()))
                    applyTempo(*bpm);
            ImGui::SetItemTooltip(on ? "Tap tempo" : "Tap tempo, with a clock running");
            clockRight = q.x + lw + px(6) + tapW;
        }
    }
    {
        const float presetFrom = clockRight + px(12);
        // The mapping: before a song is saved, the mappings to choose from
        // (arrows, a list), its changes its own; once the song is saved, the
        // song's name, its mapping the song's (its list: save it as a mapping).
        const bool song = songOwnsMapping();
        const bool changed = song ? songMappingDirty_ : dirty_;
        int index = -1;
        for (std::size_t i = 0; i < presetNames_.size(); ++i)
            if (hasFile_ && presetNames_[i] == preset_.name)
                index = static_cast<int>(i);
        const int count = static_cast<int>(presetNames_.size());
        auto openAt = [&](int i) {
            if (count == 0)
                return;
            const std::string name = presetNames_[static_cast<std::size_t>((i % count + count) % count)];
            guard([this, name] { openPreset(name); });
        };
        // Narrower when the bar lacks room (SAVE showing at its end).
        const float saveRoom = changed ? bold->CalcTextSizeA(13.0f * em / 16.0f, FLT_MAX, 0, "SAVE").x + px(24) + px(8) : 0.0f;
        const float h = px(32), arrowW = song ? 0.0f : px(22);
        const float boxW = std::clamp(winPos.x + winW - px(8) - saveRoom - presetFrom - arrowW * 2, px(110), px(186));
        const float w = arrowW * 2 + boxW;
        const ImVec2 p(presetFrom, winPos.y + px(9));
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Theme::kControl, px(5));
        if (!song) {
            if (arrow("##presetprev", p, arrowW, h, true))
                openAt(index < 0 ? 0 : index - 1);
            if (arrow("##presetnext", ImVec2(p.x + w - arrowW, p.y), arrowW, h, false))
                openAt(index + 1);
            dl->AddLine(ImVec2(p.x + arrowW, p.y + px(5)), ImVec2(p.x + arrowW, p.y + h - px(5)), Theme::kControlBorder);
            dl->AddLine(ImVec2(p.x + w - arrowW, p.y + px(5)), ImVec2(p.x + w - arrowW, p.y + h - px(5)), Theme::kControlBorder);
        }
        // Its middle: what it is; a click opens its list, under it.
        const ImVec2 ma(p.x + arrowW, p.y);
        ImGui::SetCursorScreenPos(ma);
        if (ImGui::InvisibleButton("##mappingbox", ImVec2(boxW, h)))
            ImGui::OpenPopup("##mappingmenu");
        const bool boxHovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip(song ? "The song's mapping: its changes are saved with the song" : "The mapping: its changes are saved in it");
        if (boxHovered)
            dl->AddRectFilled(ma, ImVec2(ma.x + boxW, p.y + h), IM_COL32(255, 255, 255, 14), px(4));
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), song ? Theme::kGreen : Theme::kBlue, px(5));
        auto shorten = [&](std::string t, float size, float room) {  // with an ellipsis, whole UTF-8 characters
            if (bold->CalcTextSizeA(px(size), FLT_MAX, 0, t.c_str()).x <= room)
                return t;
            while (!t.empty() && bold->CalcTextSizeA(px(size), FLT_MAX, 0, (t + "\xE2\x80\xA6").c_str()).x > room) {
                t.pop_back();
                while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80)
                    t.pop_back();
                if (!t.empty() && static_cast<unsigned char>(t.back()) >= 0xC0)
                    t.pop_back();
            }
            return t + "\xE2\x80\xA6";
        };
        const float tx = ma.x + px(9), room = boxW - px(9) - px(26) - (changed ? px(14) : 0.0f);
        if (song) {
            const std::string name = shorten(*sessionName_, 15, room);
            const ImVec2 nsz = bold->CalcTextSizeA(px(15), FLT_MAX, 0, name.c_str());
            dl->AddText(bold, px(15), ImVec2(tx, p.y + (h - nsz.y) * 0.5f), Theme::kText, name.c_str());
        } else {
            dl->AddText(bold, px(10), ImVec2(tx, p.y + px(2)), Theme::kDim, "MAPPING");
            const std::string name = shorten(hasFile_ ? preset_.name : std::string("Untitled"), 14, room);
            dl->AddText(bold, px(14), ImVec2(tx, p.y + px(13)), Theme::kText, name.c_str());
        }
        const float ax = ma.x + boxW - px(14), ay = p.y + h * 0.5f + px(1);
        dl->AddTriangleFilled(ImVec2(ax - px(4.5f), ay - px(2.5f)), ImVec2(ax + px(4.5f), ay - px(2.5f)), ImVec2(ax, ay + px(3)), Theme::kText);
        if (changed)
            dl->AddCircleFilled(ImVec2(ax - px(16), ay), px(4), Theme::kOrange);
        ImGui::SetNextWindowPos(ImVec2(p.x, p.y + h + px(3)));
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(w, px(220)), 0), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::BeginPopup("##mappingmenu")) {
            ImGui::PushFont(medium, 14.0f);
            if (song) {
                if (ImGui::Selectable("Save it as a mapping\u2026")) {
                    nameDialog_ = NameDialog::SaveAs;
                    nameInput_ = library_->uniqueName(preset_.name.empty() ? *sessionName_ : preset_.name);
                    nameError_.clear();
                    openNameDialog_ = true;
                }
            } else {
                if (presetNames_.empty())
                    ImGui::TextDisabled("No mapping in this folder");
                for (const auto& name : presetNames_) {
                    const bool selected = hasFile_ && name == preset_.name;
                    if (ImGui::Selectable(name.c_str(), selected) && !selected)
                        guard([this, name] { openPreset(name); });
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                // Managing mappings.
                ImGui::Separator();
                if (ImGui::Selectable("New\u2026"))
                    guard([this] {
                        nameDialog_ = NameDialog::New;
                        nameInput_ = library_->uniqueName("New mapping");
                        nameError_.clear();
                        openNameDialog_ = true;
                    });
                if (ImGui::Selectable("Save as\u2026")) {
                    nameDialog_ = NameDialog::SaveAs;
                    nameInput_ = library_->uniqueName(hasFile_ ? preset_.name : "New mapping");
                    nameError_.clear();
                    openNameDialog_ = true;
                }
                if (ImGui::Selectable("Delete\u2026", false, hasFile_ ? 0 : ImGuiSelectableFlags_Disabled))
                    openDeleteDialog_ = true;
                ImGui::SetItemTooltip("Deletes the mapping file");
            }
            ImGui::PopFont();
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(w, h));
        presetRight = p.x + w;
        if (changed) {  // SAVE, at the right end of the bar: the song once saved, the mapping before
            const ImVec2 sz = bold->CalcTextSizeA(13.0f * em / 16.0f, FLT_MAX, 0, "SAVE");
            ImGui::SetCursorScreenPos(ImVec2(std::max(winPos.x + winW - px(8) - sz.x - px(24), presetRight + px(8)), winPos.y + px(9)));
            ImGui::PushStyleColor(ImGuiCol_Text, color(Theme::kOrange));
            ImGui::PushStyleColor(ImGuiCol_Border, color(Theme::kOrange));
            if (ImGui::Button("SAVE"))
                saveShown();
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip(song ? "Saves the song (Ctrl+S)" : "Saves the mapping (Ctrl+S)");
        }
    }

    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(winPos.x + px(8), winPos.y + barH + px(8)));

    // The main screen: a rail of the sixteen channels under the bar, each its
    // colour, name and state (playing, recording, muted, solo). A click
    // chooses one, a double-click renames it, a right-click opens its menu,
    // the wheel goes through them.
    // In the session screen too: there, the eight channels shown are framed, and
    // a channel chosen shows its eight.
    if (!songView_) {
        performance_.setChannelColors(preset_.channelColors);
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float gap = px(4), chipH = px(34), chipW = (winW - px(16) - gap * (kChannelCount - 1)) / kChannelCount;
        bool overRail = false;
        for (int ch = 0; ch < kChannelCount; ++ch) {
            const auto c = static_cast<std::size_t>(ch);
            const ImVec2 a(r0.x + static_cast<float>(ch) * (chipW + gap), r0.y), b(a.x + chipW, a.y + chipH);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(2000 + ch);
            const bool clicked = ImGui::InvisibleButton("##rail", ImVec2(chipW, chipH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            overRail = overRail || hovered;
            const std::string& name = preset_.channelNames[c];
            if (hovered)
                ImGui::SetTooltip("%s", (std::to_string(ch + 1) + (name.empty() ? "" : "  " + name)).c_str());
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renameChannel_ = ch;
                renameChain_ = -1;
                renameSection_ = -1;
                renameInput_ = name;
                renameRow_ = renameClipChannel_ = renameClipSlot_ = -1;
        openRename_ = true;
            } else if (clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                openMenu(PerformanceView::MenuTarget(PerformanceView::MenuTarget::Kind::Channel, ch), live, false, io.MousePos);
            } else if (clicked && ch != live.channel) {
                runtime_->setChannel(ch);
            }
            bool used = !name.empty() || ((live.loopChannels >> ch) & 1u) != 0, recording = false;
            for (int k = 0; k < Engine::kClipSlots; ++k) {
                const LiveState::Clip& clip = live.clips[c * Engine::kClipSlots + static_cast<std::size_t>(k)];
                used = used || clip.sources != 0;
                recording = recording || clip.recording;
            }
            const bool active = ch == live.channel, muted = ((live.muted >> ch) & 1u) != 0, soloed = ((live.soloed >> ch) & 1u) != 0;
            const bool plays = ((live.clipsPlaying >> ch) & 1u) != 0;
            const ImU32 col = performance_.channelColor(ch);
            dl->AddRectFilled(a, b, active ? (col & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 48) : hovered ? IM_COL32(0x2C, 0x30, 0x36, 255)
                                                                                                         : IM_COL32(0x22, 0x24, 0x28, 255), px(4));
            dl->AddRect(a, b, active ? col : IM_COL32(0x30, 0x34, 0x3A, 255), px(4), px(active ? 1.4f : 1.0f));
            dl->AddRectFilled(ImVec2(a.x + px(2), a.y + px(5)), ImVec2(a.x + px(4.5f), b.y - px(5)), used ? col : IM_COL32(0x3A, 0x3E, 0x45, 255), px(1));
            // Its state on the number's line, on the right: a dot (recording red, playing green), M, S.
            float right = b.x - px(6);
            const float sy = a.y + px(10);
            if (recording || plays) {
                dl->AddCircleFilled(ImVec2(right - px(3), sy), px(3.5f), recording ? Theme::kRed : Theme::kGreen);
                right -= px(11);
            }
            for (const auto& [on, letter, lc] : {std::tuple{soloed, "S", Theme::kBlue}, std::tuple{muted, "M", Theme::kOrange}})
                if (on) {
                    const ImVec2 lsz = bold->CalcTextSizeA(px(11), FLT_MAX, 0, letter);
                    dl->AddText(bold, px(11), ImVec2(right - lsz.x, sy - lsz.y * 0.5f), lc, letter);
                    right -= lsz.x + px(4);
                }
            // Its number small on top, its name below (shortened with an ellipsis).
            const ImU32 ink = muted ? Theme::kDim : active ? Theme::kText : used ? Theme::kLabel : IM_COL32(0x55, 0x5A, 0x62, 255);
            dl->AddText(bold, px(10.5f), ImVec2(a.x + px(9), a.y + px(3)), used ? Theme::kDim : IM_COL32(0x55, 0x5A, 0x62, 255), std::to_string(ch + 1).c_str());
            if (!name.empty()) {
                std::string label = name;
                const float room = b.x - px(4) - (a.x + px(9));
                if (bold->CalcTextSizeA(px(13), FLT_MAX, 0, label.c_str()).x > room) {
                    while (label.size() > 1 && bold->CalcTextSizeA(px(13), FLT_MAX, 0, (label + "\xE2\x80\xA6").c_str()).x > room)
                        label.pop_back();
                    label += "\xE2\x80\xA6";
                }
                dl->AddText(bold, px(13), ImVec2(a.x + px(9), a.y + px(15)), ink, label.c_str());
            }
        }
        if (sessionView_) {  // the eight the session screen shows
            const int first = performance_.sessionFirst();
            const float fa = r0.x + static_cast<float>(first) * (chipW + gap) - px(2), fb = fa + 8 * chipW + 7 * gap + px(4);
            dl->AddRect(ImVec2(fa, r0.y - px(2)), ImVec2(fb, r0.y + chipH + px(2)), Theme::kCyan, px(6), px(1.8f));
        }
        if (overRail && io.MouseWheel != 0.0f)
            runtime_->setChannel((live.channel + (io.MouseWheel < 0 ? 1 : kChannelCount - 1)) % kChannelCount);
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + chipH + px(6)));
    }

    // Right: the animated modules.
    const float statusH = px(26);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));  // the page's background shows through
    ImGui::BeginChild("modules", ImVec2(0, -statusH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    performance_.setQuantize(settings_.quantize);
    std::optional<std::string> clicked;
    while (const auto e = runtime_->popSessionEvent())  // from the controller
        sessionEvent(*e, live);
    if (sessionChannelAsked_ >= 0) {
        if (live.channel == sessionChannelAsked_)
            sessionChannelAsked_ = -1;
    } else if (sessionView_ && live.channel != sessionChannel_) {  // the channel changed elsewhere: the cursor follows
        runtime_->setRecordSlot(sessionChannel_, live.currentClip[static_cast<std::size_t>(sessionChannel_)]);
        sessionChannel_ = live.channel;
        sessionSlot_ = live.recordClip[static_cast<std::size_t>(live.channel)];
    }
    // The right stick sets the chain's values, and moves in a menu.
    // In the session screen and edit mode, the left stick moves and the right one sets values.
    runtime_->setSessionValues(sessionView_ || editView_ || songView_);
    performance_.setChannelNames(preset_.channelNames);
    performance_.setChannelColors(preset_.channelColors);
    for (std::size_t i = 0; i < clipSeen_.size(); ++i) {  // a clip emptied (cleared, moved away) loses its name
        const bool holds = live.clips[i].sources != 0 || live.clips[i].recording;
        if (clipSeen_[i] && !holds)
            clipNames_[i / Engine::kClipSlots][i % Engine::kClipSlots].clear();
        clipSeen_[i] = holds ? 1 : 0;
    }
    performance_.setNames(rowNames_, clipNames_);
    performance_.setChains(shownChain_, chainNames_);
    performance_.setEditMode(editView_ && !overlaySpec_ && !optionsOpen_);
    if (sessionView_) {
        // A clip clicked: the cursor goes there, its channel becomes the active
        // one, and Record records there; a clip is launched, an empty slot only chosen.
        const bool onButtons = sessionButton_ >= 0 && sessionSlot_ < Engine::kClipSlots;
        performance_.setSelections(sessionSel_, {});
        const auto act = performance_.drawSession(live, onButtons ? -1 : sessionSlot_, chainCursor_, onButtons ? sessionButton_ : -1);
        if (auto band = performance_.takeClipBand()) {
            sessionSel_ = std::move(*band);
            if (!sessionSel_.empty())
                moveSessionCursor(sessionSel_.front().first, sessionSel_.front().second, live);
        }
        if (act) {
            using Kind = PerformanceView::SessionAction::Kind;
            if (act->kind != Kind::Mute && act->kind != Kind::Solo && act->kind != Kind::Stop)
                sessionButton_ = -1;
            if (act->kind == Kind::Mute) {
                runtime_->setMuted(act->channel, act->slot != 0);
            } else if (act->kind == Kind::Solo) {
                runtime_->setSoloed(act->channel, act->slot != 0);
            } else if (act->kind == Kind::ChainSet) {
                setChainStep(act->channel, act->slot, act->clip, act->repeats);
            } else if (act->kind == Kind::ChainCursor) {
                sessionSlot_ = Engine::kClipSlots;
                chainCursor_ = act->slot;
            } else if (act->kind == Kind::ChainGo) {
                runtime_->chainGoTo(act->channel, shownChain(act->channel), act->slot);
            } else if (act->kind == Kind::ChainStop) {
                runtime_->stopChain(act->channel);
            } else if (act->kind == Kind::ChainShow) {
                showChain(act->channel, act->slot);
            } else if (act->kind == Kind::Stop) {
                runtime_->stopClip(act->channel);
            } else if (act->kind == Kind::Launch) {
                moveSessionCursor(act->channel, act->slot, live);
                pressClip(act->channel, act->slot, live);
            } else if (act->kind == Kind::Select) {
                moveSessionCursor(act->channel, act->slot, live);
                sessionSel_ = {{act->channel, act->slot}};
            } else if (act->kind == Kind::SelectToggle) {
                if (sessionSel_.empty() && sessionSlot_ < Engine::kClipSlots &&
                    live.clips[static_cast<std::size_t>(sessionChannel_ * Engine::kClipSlots + sessionSlot_)].sources)
                    sessionSel_ = {{sessionChannel_, sessionSlot_}};  // the clip under the cursor, first
                const std::pair<int, int> clip{act->channel, act->slot};
                if (const auto it = std::find(sessionSel_.begin(), sessionSel_.end(), clip); it != sessionSel_.end())
                    sessionSel_.erase(it);
                else
                    sessionSel_.push_back(clip);
                moveSessionCursor(act->channel, act->slot, live);
            } else if (act->kind == Kind::LaunchRow) {
                launchRow(act->slot, live);
            } else if (act->kind == Kind::CaptureRow) {
                captureRow(live);
            } else if (act->kind == Kind::ClipDrop || act->kind == Kind::StepDrop || act->kind == Kind::ClipInsert) {
                sessionDrop(*act, live);
            } else if (act->kind == Kind::RepeatsAsk) {
                sessionSlot_ = Engine::kClipSlots;
                chainCursor_ = act->slot;
                valueAsk_ = ValueAsk{ValueAsk::Kind::ChainRepeats, act->channel, act->slot, act->clip, act->repeats};
                openValueAsk_ = true;
            } else {
                moveSessionCursor(act->channel, live.currentClip[static_cast<std::size_t>(act->channel)], live);
            }
        }
    } else if (songView_) {
        const bool selHere = songSelRow_ == songCursor_.row && songSelCol_ == songCursor_.col;
        performance_.setSelections({}, selHere ? songSel_ : std::vector<int>{});
        if (songLoopFrom_ >= 0) {  // the loop past the song's end (sections shortened or gone): none
            int beats = 0;
            for (const auto& sec : song_.sections)
                beats += PerformanceView::songSectionBars(live, sec) * std::clamp(sec.repeats, 1, midipad::kMaxSectionRepeats) * std::max(live.beatsPerBar, 1);
            if (songLoopFrom_ >= beats) {
                songLoopFrom_ = songLoopTo_ = -1;
                runtime_->setSongLoopRange(-1, -1);
            } else if (songLoopTo_ > beats) {
                songLoopTo_ = beats;
                runtime_->setSongLoopRange(songLoopFrom_, songLoopTo_);
            }
        }
        performance_.setSongMarks(songLoopFrom_, songLoopTo_, songStartBar_);
        const auto act = performance_.drawSong(live, song_, songCursor_);
        if (auto band = performance_.takeItemBand()) {
            songSel_ = std::move(*band);
            songSelRow_ = songCursor_.row;
            songSelCol_ = songCursor_.col;
            if (!songSel_.empty()) {
                songCursor_.item = songSel_.front();
                songCursor_.editing = true;
            }
        }
        if (act)
            songAction(*act, live);
    } else {
        performance_.setNotesPanel(notesPanel_);
        clicked = performance_.draw(withChannelOneControls(preset_.forChannel(live.channel), preset_.base), live, settings_.key);
        if (notesPanel_)
            drawNotesPanel(live);
    }
    if (auto ask = performance_.takeNoteValueAsk()) {  // a stem double-clicked: its value typed
        valueAsk_ = ValueAsk{ask->chance ? ValueAsk::Kind::NoteChance : ValueAsk::Kind::NoteVelocity, 0, 0, 0, ask->value};
        valueAskNotes_ = std::move(ask->notes);
        openValueAsk_ = true;
    }
    if (auto edit = performance_.takeNoteEdit()) {  // from the note loop's editor (the last frame's)
        // Undoable: the clip as it is, first.
        const char* what = edit->loopLength > 0                                  ? "Loop length"
                           : !edit->added.empty()                                ? "Note added"
                           : edit->quantizeStep > 0                              ? "Notes quantized"
                           : !edit->velocities.empty()                           ? "Velocity"
                           : !edit->chances.empty()                              ? "Chance"
                           : edit->remove                                        ? "Notes deleted"
                           : edit->resize                                        ? "Notes lengthened"
                                                                                 : "Notes moved";
        rememberClips({{live.channel, live.currentClip[static_cast<std::size_t>(live.channel)]}}, what);
        if (!edit->added.empty())
            runtime_->addLoopNotes(std::move(edit->added));
        else if (edit->quantizeStep > 0)
            runtime_->quantizeLoopNotes(std::move(edit->notes), edit->quantizeStep);
        else if (!edit->velocities.empty() || !edit->chances.empty())
            runtime_->setLoopNoteValues(std::move(edit->notes), std::move(edit->velocities), std::move(edit->chances));
        else if (edit->loopLength > 0)
            runtime_->setNoteLoopLength(edit->loopLength);
        else if (edit->remove)
            runtime_->removeLoopNotes(std::move(edit->notes));
        else if (edit->resize)
            runtime_->resizeLoopNotes(std::move(edit->notes), edit->samples);
        else
            runtime_->moveLoopNotes(std::move(edit->notes), edit->samples, edit->semitones, edit->byDegrees);
    }
    if (const auto request = performance_.takeMenuRequest())  // a right-click
        openMenu(request->target, live, false, request->at);
    if (const auto channel = performance_.takeChannelRequest())  // the CHANNEL block's arrows or wheel
        runtime_->setChannel(*channel);
    if (const auto renamed = performance_.takeRenameRequest()) {  // a channel's name double-clicked
        renameChannel_ = *renamed;
        renameChain_ = -1;
        renameSection_ = -1;
        renameInput_ = preset_.channelNames[static_cast<std::size_t>(*renamed)];
        renameRow_ = renameClipChannel_ = renameClipSlot_ = -1;
        openRename_ = true;
    }
    if (const auto clear = performance_.takeClearRequest())  // the x of a lane
        runtime_->clearLoop(clear->source, clear->axis);
    if (const auto stretch = performance_.takeStretchRequest())  // Shift + wheel over a LOOP panel
        for (int track = 0; track < Engine::kNoteTrack; ++track)
            if (stretch->tracks & (1u << track)) {
                using K = PerformanceView::StretchRequest::Kind;
                if (stretch->kind == K::Amplitude)
                    runtime_->amplifyLoop(Engine::sourceOfTrack(track), Engine::axisOfTrack(track), stretch->steps);
                else if (stretch->kind == K::Offset)
                    runtime_->offsetLoop(Engine::sourceOfTrack(track), Engine::axisOfTrack(track), stretch->steps);
                else
                    runtime_->stretchLoop(Engine::sourceOfTrack(track), Engine::axisOfTrack(track), stretch->steps);
            }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    drawPerformanceStatus(live, statusH);

    // The SCALE screen opens the tonality menu; any other control is edited
    // in a panel over the view.
    if (clicked && *clicked == "scale") {
        ImGui::OpenPopup("##keymenu");
    } else if (clicked && *clicked == "notes") {
        openNotes(live);
    } else if (clicked) {
        overlayAppearing_ = overlaySpec_ != clicked;
        overlaySpec_ = clicked;
        optionsOpen_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(6));
    const bool keyMenu = ImGui::BeginPopup("##keymenu");
    ImGui::PopStyleVar(2);
    if (keyMenu) {
        ImGui::PushFont(medium, 14.0f);
        if (drawKeyChoices())
            persistSettings();
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    if (overlaySpec_)
        drawMappingOverlay(live);
    if (optionsOpen_)
        drawOptionsOverlay(live);
    if (menu_)
        drawContextMenu(live);
    if (helpShown_)
        drawHelpOverlay();
    drawDialogs();

    ImGui::PopStyleVar(kVars);
    ImGui::PopStyleColor(kColors);
}

namespace {

// "Virtual Raw MIDI 3-0:VirMIDI 3-0 28:0" -> "VirMIDI 3-0".
std::string shortPortName(const std::string& port) {
    std::string name = MidiOutput::stablePortName(port);
    if (const auto colon = name.find(':'); colon != std::string::npos)
        name = name.substr(colon + 1);
    return name;
}

}  // namespace

void GuiApp::drawPerformanceStatus(const LiveState& live, float height) {
    using Theme = PerformanceView::Theme;
    const float em = ImGui::GetFontSize();
    auto px = [em](float v) { return v * em / 16.0f; };
    ImFont* bold = uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont();
    ImFont* medium = uiFonts_.medium ? uiFonts_.medium : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cy = p.y + height * 0.5f;
    const float width = ImGui::GetContentRegionAvail().x;

    // Left: where the MIDI goes.
    const bool virtualPort = settings_.output == AppSettings::Output::VirtualPort;
    std::string out = virtualPort ? "Virtual port \"" + settings_.virtualPortName + "\"" : shortPortName(outputLabel_);
    if (const auto name = rawMidiName(outputLabel_); !name.empty())
        out += "  (seen as " + name + ")";
    const ImVec2 lsz = medium->CalcTextSizeA(px(12), FLT_MAX, 0, "MIDI out:");
    dl->AddText(medium, px(12), ImVec2(p.x + px(6), cy - lsz.y * 0.5f), Theme::kDim, "MIDI out:");
    dl->AddText(medium, px(12), ImVec2(p.x + px(12) + lsz.x, cy - lsz.y * 0.5f), Theme::kLabel, out.c_str());

    // In the middle, a moment: what was just done (Undo), or undone (Redo).
    if (ImGui::GetTime() < toastUntil_ && !toast_.empty()) {
        const char* link = toastUndone_ ? "Redo" : "Undo";
        const char* keys = toastUndone_ ? "Ctrl+Shift+Z" : "Ctrl+Z";
        const ImVec2 tsz = medium->CalcTextSizeA(px(12), FLT_MAX, 0, toast_.c_str());
        const ImVec2 bsz = bold->CalcTextSizeA(px(12), FLT_MAX, 0, link), ksz = medium->CalcTextSizeA(px(11), FLT_MAX, 0, keys);
        const float w = toastPlain_ ? px(14) + tsz.x + px(14) : px(14) + tsz.x + px(18) + bsz.x + px(8) + ksz.x + px(14);
        const ImVec2 a(p.x + (width - w) * 0.5f, cy - px(11)), b(a.x + w, cy + px(11));
        dl->AddRectFilled(a, b, IM_COL32(0x23, 0x26, 0x2B, 255), px(6));
        dl->AddRect(a, b, IM_COL32(0x3A, 0x3E, 0x45, 255), px(6), px(1));
        dl->AddText(medium, px(12), ImVec2(a.x + px(14), cy - tsz.y * 0.5f), Theme::kText, toast_.c_str());
        const ImVec2 la(a.x + px(14) + tsz.x + px(18), cy - bsz.y * 0.5f);
        if (!toastPlain_) {
        ImGui::SetCursorScreenPos(ImVec2(la.x - px(4), a.y));
        const bool clicked = ImGui::InvisibleButton("##undotoast", ImVec2(bsz.x + px(8) + ksz.x + px(8), b.y - a.y));
        dl->AddText(bold, px(12), la, ImGui::IsItemHovered() ? Theme::kText : Theme::kCyan, link);
        dl->AddText(medium, px(11), ImVec2(la.x + bsz.x + px(8), cy - ksz.y * 0.5f), Theme::kDim, keys);
        if (clicked)
            undoRedo(toastUndone_);
        }
        ImGui::SetCursorScreenPos(p);
    }

    // Right: the gamepad.
    const std::string name = live.connected ? live.gamepadName : "No gamepad";
    const char* extra = live.connected && live.hasTouchpad ? "+ touchpad" : "";
    const ImVec2 nsz = bold->CalcTextSizeA(px(13), FLT_MAX, 0, name.c_str());
    const ImVec2 esz = medium->CalcTextSizeA(px(12), FLT_MAX, 0, extra);
    const float x0 = p.x + width - px(6) - esz.x - px(8) - nsz.x - px(18);
    if (live.connected)
        dl->AddCircleFilled(ImVec2(x0 + px(5), cy), px(9), (Theme::kGreen & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 60));
    dl->AddCircleFilled(ImVec2(x0 + px(5), cy), px(5), live.connected ? Theme::kGreen : Theme::kDim);
    dl->AddText(bold, px(13), ImVec2(x0 + px(18), cy - nsz.y * 0.5f), live.connected ? Theme::kText : Theme::kDim, name.c_str());
    dl->AddText(medium, px(12), ImVec2(x0 + px(26) + nsz.x, cy - esz.y * 0.5f), Theme::kLabel, extra);
    ImGui::Dummy(ImVec2(width, height));
}

void GuiApp::drawOptionsOverlay(const LiveState& live) {
    using Theme = PerformanceView::Theme;
    auto color = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    const float em = ImGui::GetFontSize();
    auto px = [em](float v) { return v * em / 16.0f; };
    ImFont* bold = uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont();
    ImFont* medium = uiFonts_.medium ? uiFonts_.medium : ImGui::GetFont();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Centered as it opens (a few frames, while its size settles); then it
    // stays where it is moved.
    if (optionsAppearing_) {
        ImGui::SetNextWindowFocus();
        optionsAppearing_ = false;
        optionsPlacingFrames_ = 12;
    }
    if (optionsPlacingFrames_ > 0) {
        --optionsPlacingFrames_;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(px(560), 0));
    pushWindowLook();
    bool open = true;
    ImGui::PushFont(bold, 16.0f);
    const bool visible = ImGui::Begin("Settings###optionsoverlay", &open,
                                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopFont();
    if (visible) {
        // The sections scroll when the window is too short; Close stays in view.
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(px(200), viewport->WorkSize.y - px(150))));
        ImGui::BeginChild("optionsbody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Each section in a card: its own background, a colored bar on the left
        // and a title of the same color.
        ImU32 cardColor = 0;
        auto beginCard = [&](const char* id, const char* title, ImU32 accent) {
            cardColor = accent;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, color(IM_COL32(0x2B, 0x2E, 0x33, 255)));
            ImGui::PushStyleColor(ImGuiCol_Border, color(IM_COL32(0x40, 0x44, 0x4B, 255)));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(8));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(10)));
            ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            dl = ImGui::GetWindowDrawList();
            ImGui::PushFont(bold, 12.5f);
            ImGui::TextColored(color(accent), "%s", title);
            ImGui::PopFont();
        };
        auto endCard = [&] {
            // The bar, inside the card so its background does not cover it.
            const ImVec2 a = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            dl->PushClipRect(a, ImVec2(a.x + size.x, a.y + size.y), false);
            dl->AddRectFilled(a, ImVec2(a.x + px(5), a.y + size.y), cardColor, px(8), ImDrawFlags_RoundCornersLeft);
            dl->PopClipRect();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            dl = ImGui::GetWindowDrawList();
            ImGui::Dummy(ImVec2(0, px(6)));
        };

        // MIDI interface: every output, the current one selected.
        beginCard("##cardmidi", "MIDI INTERFACE", Theme::kBlue);
        {  // the output drop-down
            struct Choice {
                std::string port;  // empty: the virtual port
                std::string name, hint;
                bool selected;
                bool missing = false;  // the saved port, not plugged in
            };
            std::vector<Choice> choices;
            const bool virtualSelected = settings_.output == AppSettings::Output::VirtualPort;
            choices.push_back({"", "Virtual port \"" + settings_.virtualPortName + "\"", "software port", virtualSelected});
            for (const auto& port : ports_) {
                const auto bw = rawMidiName(port);
                const bool selected = !virtualSelected && (port == outputLabel_ ||
                                                           MidiOutput::stablePortName(port) == settings_.existingPortName);
                choices.push_back({port, shortPortName(port), bw.empty() ? "" : "seen as " + bw, selected});
            }
            const Choice* current = nullptr;
            for (const auto& c : choices)
                if (c.selected)
                    current = &c;
            Choice missing{"", shortPortName(settings_.existingPortName) + " (not found)", "", true, true};
            if (!current)
                current = &missing;

            // A row: radio dot, name, and the hint on the right before `right`.
            const float h = px(36);
            auto drawRow = [&](ImDrawList* d, const Choice& c, ImVec2 a, float w, float right, bool dot) {
                float x = a.x + px(14);
                if (dot) {
                    const ImVec2 at(a.x + px(20), a.y + h * 0.5f);
                    d->AddCircle(at, px(7), c.selected ? Theme::kBlue : Theme::kLabel, 0, px(2));
                    if (c.selected)
                        d->AddCircleFilled(at, px(3.5f), Theme::kBlue);
                    x = a.x + px(38);
                }
                ImFont* nameFont = c.selected ? bold : medium;
                const ImVec2 nsz = nameFont->CalcTextSizeA(px(14), FLT_MAX, 0, c.name.c_str());
                d->AddText(nameFont, px(14), ImVec2(x, a.y + (h - nsz.y) * 0.5f), c.missing ? Theme::kRed : Theme::kText,
                           c.name.c_str());
                if (!c.hint.empty()) {
                    const ImVec2 hsz = medium->CalcTextSizeA(px(12), FLT_MAX, 0, c.hint.c_str());
                    d->AddText(medium, px(12), ImVec2(a.x + w - right - hsz.x, a.y + (h - hsz.y) * 0.5f),
                               c.selected ? Theme::kText : Theme::kLabel, c.hint.c_str());
                }
            };

            // The current output in a drop-down, Refresh beside it.
            ImGui::PushFont(bold, 13.0f);
            const float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::PopFont();
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x - refreshW - spacing;
            if (ImGui::InvisibleButton("##output", ImVec2(w, h)))
                ImGui::OpenPopup("##outputs");
            const bool hovered = ImGui::IsItemHovered();
            const bool listOpen = ImGui::IsPopupOpen("##outputs");
            dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h),
                              hovered || listOpen ? IM_COL32(0x38, 0x3C, 0x43, 235) : IM_COL32(0x2B, 0x2E, 0x33, 230), px(6));
            dl->AddRect(a, ImVec2(a.x + w, a.y + h), current->missing ? Theme::kRed : Theme::kBlue, px(6), px(1.3f));
            drawRow(dl, *current, a, w, px(40), false);
            const ImVec2 chevron(a.x + w - px(20), a.y + h * 0.5f);
            const float cs = px(5);
            dl->AddTriangleFilled(ImVec2(chevron.x - cs, chevron.y - cs * 0.5f), ImVec2(chevron.x + cs, chevron.y - cs * 0.5f),
                                  ImVec2(chevron.x, chevron.y + cs * 0.6f), Theme::kLabel);
            ImGui::SameLine();
            ImGui::PushFont(bold, 13.0f);
            if (ImGui::Button("Refresh", ImVec2(refreshW, h))) {
                refreshPorts();
                virmidiStatus_ = virmidi::status();
            }
            ImGui::PopFont();

            ImGui::SetNextWindowPos(ImVec2(a.x, a.y + h + px(4)));
            ImGui::SetNextWindowSize(ImVec2(w, 0));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, color(IM_COL32(0x24, 0x27, 0x2B, 250)));
            ImGui::PushStyleColor(ImGuiCol_Border, color(IM_COL32(0x45, 0x4A, 0x52, 255)));
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(6));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4), px(4)));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, px(2)));
            if (ImGui::BeginPopup("##outputs")) {
                ImDrawList* pd = ImGui::GetWindowDrawList();
                for (std::size_t i = 0; i < choices.size(); ++i) {
                    const Choice& c = choices[i];
                    const ImVec2 ra = ImGui::GetCursorScreenPos();
                    const float rw = ImGui::GetContentRegionAvail().x;
                    const bool clicked = ImGui::InvisibleButton(("##out" + std::to_string(i)).c_str(), ImVec2(rw, h));
                    if (c.selected)
                        pd->AddRectFilled(ra, ImVec2(ra.x + rw, ra.y + h), (Theme::kBlue & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 55), px(5));
                    else if (ImGui::IsItemHovered())
                        pd->AddRectFilled(ra, ImVec2(ra.x + rw, ra.y + h), IM_COL32(0x38, 0x3C, 0x43, 235), px(5));
                    drawRow(pd, c, ra, rw, px(14), true);
                    if (clicked) {
                        if (!c.selected)
                            useOutput(c.port.empty() ? AppSettings::Output::VirtualPort : AppSettings::Output::ExistingPort, c.port);
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (ports_.empty()) {
                    ImGui::PushFont(medium, 12.0f);
                    ImGui::TextColored(color(Theme::kLabel), "  No MIDI port found.");
                    ImGui::PopFont();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
        }
        if (virmidi::supported())
            drawVirMidiSection(false);
        endCard();

        // A row of buttons, one lit: returns the one clicked, or -1.
        auto choice = [&](const char* id, std::initializer_list<const char*> labels, int current) {
            int clicked = -1, i = 0;
            ImGui::PushID(id);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4), px(4)));
            for (const char* label : labels) {
                const bool selected = i == current;
                ImGui::PushStyleColor(ImGuiCol_Button, color(selected ? (Theme::kGreen & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 110)
                                                                      : Theme::kControl));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(selected ? Theme::kGreen : Theme::kControlBorder));
                ImGui::PushStyleColor(ImGuiCol_Border, color(selected ? Theme::kGreen : Theme::kControlBorder));
                if (ImGui::Button(label))
                    clicked = i;
                ImGui::PopStyleColor(3);
                ImGui::SameLine();
                ++i;
            }
            ImGui::PopStyleVar();
            ImGui::NewLine();
            ImGui::PopID();
            return clicked;
        };
        auto rowLabel = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(color(Theme::kLabel), "%s", text);
            ImGui::SameLine(px(150));
        };
        // Sync: Ableton Link, and how loops snap to its beat.
        beginCard("##cardsync", "SYNC", Theme::kGreen);
        {
            ImGui::PushFont(medium, 13.0f);
            bool changed = false;
            const float clockRowY = ImGui::GetCursorPosY();
            rowLabel("Clock");
            const int clock = choice("clock", {"Free", "Internal", "Ableton Link"}, static_cast<int>(settings_.clock));
            if (clock >= 0 && clock != static_cast<int>(settings_.clock)) {
                settings_.clock = static_cast<AppSettings::Clock>(clock);
                if (settings_.clock != AppSettings::Clock::Free)
                    settings_.lastClock = settings_.clock;
                changed = true;
            }
            if (live.clock != ClockMode::Free) {
                // On the clock's row, after its buttons.
                const float nextRowY = ImGui::GetCursorPosY();
                ImGui::SetCursorPos(ImVec2(px(150) + px(232), clockRowY));
                ImGui::AlignTextToFramePadding();
                if (live.clock == ClockMode::Link)
                    ImGui::TextColored(color(Theme::kGreen), "%d %s \xC2\xB7 %.1f BPM", live.linkPeers,
                                       live.linkPeers == 1 ? "peer" : "peers", live.tempo);
                else
                    ImGui::TextColored(color(Theme::kGreen), "%.1f BPM", live.tempo);
                ImGui::SetCursorPosY(nextRowY);
            }
            rowLabel("Loops snap to");
            const int snap = choice("snap", {"Free", "1 beat", "1 bar", "2 bars", "4 bars"}, static_cast<int>(settings_.loopSnap));
            if (snap >= 0 && snap != static_cast<int>(settings_.loopSnap)) {
                settings_.loopSnap = static_cast<AppSettings::LoopSnap>(snap);
                changed = true;
            }
            rowLabel("Beats per bar");
            ImGui::SetNextItemWidth(px(110));
            int bpb = settings_.beatsPerBar;
            if (ImGui::InputInt("##bpb", &bpb)) {
                settings_.beatsPerBar = std::clamp(bpb, 1, 16);
                changed = true;
            }
            rowLabel("Quantize notes");
            static constexpr int kGrids[] = {0, 2, 4, 8};
            const int grid = static_cast<int>(std::find(std::begin(kGrids), std::end(kGrids), settings_.quantize) - std::begin(kGrids));
            const int quantize = choice("quantize", {"Off", "1/8", "1/16", "1/32"}, grid);
            if (quantize >= 0 && kGrids[quantize] != settings_.quantize) {
                settings_.quantize = kGrids[quantize];
                changed = true;
            }
            rowLabel("MIDI clock out");
            ImGui::BeginDisabled(settings_.clock != AppSettings::Clock::Internal);
            const int clockOut = choice("clockout", {"Off", "On"}, settings_.midiClockOut ? 1 : 0);
            ImGui::EndDisabled();
            if (clockOut >= 0 && (clockOut == 1) != settings_.midiClockOut) {
                settings_.midiClockOut = clockOut == 1;
                changed = true;
            }
            rowLabel("Start / stop sync");
            const int startStop = choice("startstop", {"Off", "On"}, settings_.startStopSync ? 1 : 0);
            if (startStop >= 0 && (startStop == 1) != settings_.startStopSync) {
                settings_.startStopSync = startStop == 1;
                changed = true;
            }
            ImGui::PopFont();
            if (changed) {
                applySync();
                persistSettings();
            }
        }
        endCard();

        // D-pad: how long a direction must hold before it plays.
        beginCard("##carddpad", "D-PAD", Theme::kRed);
        ImGui::PushFont(medium, 13.0f);
        auto settleSlider = [&](const char* label, const char* id, int& ms) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(color(Theme::kLabel), "%s", label);
            ImGui::SameLine(px(150));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt(id, &ms, 0, 80, "%d ms", ImGuiSliderFlags_AlwaysClamp))
                runtime_->setDpadSettle(settings_.dpadSettlePressMs, settings_.dpadSettleReleaseMs);
            if (ImGui::IsItemDeactivatedAfterEdit())
                persistSettings();
        };
        settleSlider("Delay on press", "##settlepress", settings_.dpadSettlePressMs);
        settleSlider("Delay on release", "##settlerelease", settings_.dpadSettleReleaseMs);
        ImGui::PopFont();

        endCard();

        // Humanization: which channels, and how much.
        beginCard("##cardhumanize", "HUMANIZE", Theme::kViolet);
        ImGui::PushFont(medium, 13.0f);
        Humanize& h = settings_.humanize;
        bool humanizeChanged = false;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(color(Theme::kLabel), "Channels");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4), px(4)));
        const float channelsX = px(150);
        for (int c = 0; c < 16; ++c) {
            if (c == 0)
                ImGui::SameLine(channelsX);
            else if (c == 8)
                ImGui::SetCursorPosX(channelsX);
            else
                ImGui::SameLine();
            const bool on = h.appliesTo(c);
            ImGui::PushStyleColor(ImGuiCol_Button, color(on ? (Theme::kBlue & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 150)
                                                            : Theme::kControl));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(on ? Theme::kBlue : Theme::kControlBorder));
            ImGui::PushStyleColor(ImGuiCol_Border, color(on ? Theme::kBlue : Theme::kControlBorder));
            if (ImGui::Button(("##hc" + std::to_string(c)).c_str(), ImVec2(px(34), 0))) {
                h.channels = static_cast<std::uint16_t>(h.channels ^ (1u << c));
                humanizeChanged = true;
            }
            ImGui::PopStyleColor(3);
            // The label drawn over the button, so the id stays stable.
            const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
            const std::string number = std::to_string(c + 1);
            const ImVec2 tsz = ImGui::CalcTextSize(number.c_str());
            ImGui::GetWindowDrawList()->AddText(ImVec2((bmin.x + bmax.x - tsz.x) * 0.5f, (bmin.y + bmax.y - tsz.y) * 0.5f),
                                                on ? Theme::kText : Theme::kLabel, number.c_str());
            if (c == 7 || c == 15) {
                ImGui::SameLine(0, px(12));
                if (c == 7 && ImGui::Button("All", ImVec2(px(48), 0))) {
                    h.channels = 0xFFFF;
                    humanizeChanged = true;
                }
                if (c == 15 && ImGui::Button("None", ImVec2(px(48), 0))) {
                    h.channels = 0;
                    humanizeChanged = true;
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::Spacing();
        // A min/max pair of sliders on one row; min never exceeds max.
        auto rangeRow = [&](const char* label, const char* id, int& low, int& high, int limit, const char* format) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(color(Theme::kLabel), "%s", label);
            ImGui::SameLine(px(150));
            const float w = (ImGui::GetContentRegionAvail().x - px(8)) * 0.5f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderInt((std::string("##min") + id).c_str(), &low, 0, limit, (std::string("min ") + format).c_str(),
                                 ImGuiSliderFlags_AlwaysClamp)) {
                high = std::max(high, low);
                humanizeChanged = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                persistSettings();
            ImGui::SameLine(0, px(8));
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderInt((std::string("##max") + id).c_str(), &high, 0, limit, (std::string("max ") + format).c_str(),
                                 ImGuiSliderFlags_AlwaysClamp)) {
                low = std::min(low, high);
                humanizeChanged = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                persistSettings();
        };
        rangeRow("Velocity", "vel", h.velocityMinPercent, h.velocityMaxPercent, 100, "%d %%");
        ImGui::SetItemTooltip("Percent of the velocity set in the mapping");
        rangeRow("Delay", "delay", h.delayMinMs, h.delayMaxMs, Humanize::kMaxDelayMs, "%d ms");
        ImGui::SetItemTooltip("Each note starts this much later; its release is delayed alike");
        ImGui::PopFont();
        if (humanizeChanged) {
            runtime_->setHumanize(h);
            if (!ImGui::IsAnyItemActive())
                persistSettings();
        }

        endCard();

        // Presets: the folder.
        beginCard("##cardpresets", "MAPPINGS", Theme::kOrange);
        ImGui::PushFont(medium, 13.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(color(Theme::kLabel), "Folder");
        ImGui::SameLine();
        std::string folder = settings_.presetsDir.string();
        const float folderW = ImGui::GetContentRegionAvail().x - px(120);
        while (folder.size() > 4 && medium->CalcTextSizeA(px(13), FLT_MAX, 0, ("…" + folder).c_str()).x > folderW - px(16))
            folder.erase(0, 1);
        if (folder != settings_.presetsDir.string())
            folder = "…" + folder;
        ImGui::SetNextItemWidth(folderW);
        ImGui::InputText("##folder", &folder, ImGuiInputTextFlags_ReadOnly);
        ImGui::SetItemTooltip("%s", settings_.presetsDir.string().c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushFont(bold, 13.0f);
        if (ImGui::Button("Change…", ImVec2(-FLT_MIN, 0)))
            browseFolder(FolderTarget::Presets, settings_.presetsDir);
        ImGui::PopFont();
        endCard();

        // Display: how many frames a second the window draws.
        beginCard("##carddisplay", "DISPLAY", Theme::kCyan);
        ImGui::PushFont(medium, 13.0f);
        {
            rowLabel("Frame rate");
            const int current = settings_.frameRate == 60 ? 1 : settings_.frameRate == 30 ? 2 : 0;
            const int rate = choice("framerate", {"Screen", "60", "30"}, current);
            if (rate >= 0 && rate != current) {
                settings_.frameRate = rate == 1 ? 60 : rate == 2 ? 30 : 0;
                persistSettings();
            }
            ImGui::SetCursorPosX(px(150));  // under the buttons
            if (ImGui::Checkbox("30 fps when in the background", &settings_.slowInBackground))
                persistSettings();
            ImGui::SetCursorPosX(px(150));
            if (ImGui::Checkbox("Render at standard resolution", &settings_.standardResolution))
                persistSettings();
            ImGui::SetItemTooltip("On a scaled screen (125 %%, 200 %%...): fewer pixels to draw, a little less sharp");
            if (settings_.standardResolution != standardResolution_) {
                ImGui::SameLine();
                ImGui::TextColored(color(Theme::kLabel), "(at the next start)");
            }
        }
        ImGui::PopFont();
        endCard();

        // Edit menu: the buttons that drive the session screen and the edit mode.
        beginCard("##cardeditmenu", "EDIT MENU", Theme::kCyan);
        ImGui::PushFont(medium, 13.0f);
        {
            static constexpr Button kChoices[] = {Button::South, Button::East, Button::West, Button::North, Button::LeftStick,
                                                  Button::RightStick, Button::LeftShoulder, Button::RightShoulder, Button::Touchpad,
                                                  Button::Back, Button::Start, Button::Guide, Button::Misc1, Button::DpadUp,
                                                  Button::DpadDown, Button::DpadLeft, Button::DpadRight};
            auto navCombo = [&](const char* name, const char* what, int& value, const char* none) {
                rowLabel(name);
                ImGui::SetNextItemWidth(px(200));
                const std::string shown = value < 0 ? std::string(none) : label(static_cast<Button>(value));
                bool changed = false;
                if (ImGui::BeginCombo((std::string("##nav") + name).c_str(), shown.c_str(), ImGuiComboFlags_HeightLarge)) {
                    if (ImGui::Selectable(none, value < 0)) {
                        value = -1;
                        changed = true;
                    }
                    for (Button b : kChoices) {
                        // Each with what it does in the preset: red, it cannot be chosen.
                        const NavConflict conflict = navConflict(static_cast<int>(b));
                        if (ImGui::Selectable(label(b).c_str(), value == static_cast<int>(b), conflict.blocking ? ImGuiSelectableFlags_Disabled : 0)) {
                            value = static_cast<int>(b);
                            changed = true;
                        }
                        if (!conflict.text.empty()) {  // a dot, red or amber; what it means on hover
                            ImGui::SameLine(px(150));
                            const ImVec2 at = ImGui::GetCursorScreenPos();
                            const float r = px(4.5f), lineH = ImGui::GetTextLineHeight();
                            ImGui::Dummy(ImVec2(r * 2, lineH));
                            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + r, at.y + lineH * 0.5f), r,
                                                                        ImGui::GetColorU32(conflict.blocking ? kErrorColor : kWarningColor));
                            ImGui::SetItemTooltip("%s", conflict.text.c_str());
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextColored(color(Theme::kLabel), "%s", what);
                if (changed) {
                    persistSettings();
                    runtime_->setNavButtons(settings_.navSelect, settings_.navMenu, settings_.navBack);
                }
            };
            navCombo("Select", "launch, choose, open", settings_.navSelect, "L3 / R3");
            navCombo("Menu", sharedSelectMenu() ? "same as Select: a press opens the menu" : "the menu under the cursor", settings_.navMenu, "None");
            navCombo("Back", "close a menu, leave edit mode", settings_.navBack, "None");
            // What the buttons chosen do elsewhere.
            ImGui::PushFont(medium, 12.0f);
            std::vector<int> shown;
            for (int b : {settings_.navSelect, settings_.navMenu, settings_.navBack}) {
                if (b < 0 || std::find(shown.begin(), shown.end(), b) != shown.end())
                    continue;  // once each
                shown.push_back(b);
                const NavConflict conflict = navConflict(b);
                if (!conflict.text.empty())
                    ImGui::TextColored(conflict.blocking ? kErrorColor : kWarningColor, "%s %s", label(static_cast<Button>(b)).c_str(),
                                       conflict.text.c_str());
            }
            ImGui::PopFont();
        }
        ImGui::PopFont();
        endCard();
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::PushFont(bold, 13.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(80));
        if (ImGui::Button("Close", ImVec2(px(80), 0)) ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)))
            open = false;
        ImGui::PopFont();
    }
    ImGui::End();
    popWindowLook();
    if (!open)
        optionsOpen_ = false;
}

// Creating the snd-virmidi ports, inside the Settings panel.
void GuiApp::drawVirMidiSection(bool inOutputDialog) {
    using Theme = PerformanceView::Theme;
    auto color = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    const float em = ImGui::GetFontSize();
    auto px = [em](float v) { return v * em / 16.0f; };
    ImFont* bold = uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont();
    ImFont* medium = uiFonts_.medium ? uiFonts_.medium : ImGui::GetFont();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushFont(bold, 13.0f);
    ImGui::TextColored(color(Theme::kText), "VirMIDI ports");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(medium, 12.0f);
    const int count = virmidiPortCount();
    std::string state = !virmidiStatus_.loaded ? "not created"
                        : count > 0            ? std::to_string(count) + (count == 1 ? " port" : " ports")
                                               : "loaded";
    if (virmidiStatus_.atStartup)
        state += ", created at every startup";
    ImGui::TextColored(color(virmidiStatus_.loaded ? Theme::kGreen : Theme::kDim), "%s", state.c_str());
    ImGui::TextColored(color(Theme::kLabel), "Kernel MIDI ports, seen by every program. Creating them asks for the administrator password.");
    ImGui::PopFont();
    ImGui::Spacing();

    const bool busy = virmidiJob_.valid();
    ImGui::BeginDisabled(busy);
    ImGui::PushFont(medium, 13.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(color(Theme::kLabel), "Ports");
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4), px(4)));
    for (int n = 1; n <= virmidi::kMaxPorts; ++n) {
        const bool selected = virmidiPorts_ == n;
        ImGui::PushStyleColor(ImGuiCol_Button, color(selected ? (Theme::kBlue & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 150)
                                                              : Theme::kControl));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(selected ? Theme::kBlue : Theme::kControlBorder));
        ImGui::PushStyleColor(ImGuiCol_Border, color(selected ? Theme::kBlue : Theme::kControlBorder));
        if (ImGui::Button(std::to_string(n).c_str(), ImVec2(px(34), 0)))
            virmidiPorts_ = n;
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
    }
    ImGui::PopStyleVar();
    ImGui::SameLine(0, px(18));
    ImGui::Checkbox(inOutputDialog ? "Create them at every startup" : "At every startup", &virmidiAtStartup_);
    ImGui::SetItemTooltip("Creates the ports again at every startup of the computer");
    ImGui::PopFont();

    ImGui::PushFont(bold, 13.0f);
    const bool changed = !virmidiStatus_.loaded || virmidiPorts_ != count || virmidiAtStartup_ != virmidiStatus_.atStartup;
    const char* createLabel = inOutputDialog ? "Create the ports" : virmidiStatus_.loaded ? "Apply" : "Create";
    const float createW = inOutputDialog ? px(130) : px(90);
    const bool canRemove = !inOutputDialog && (virmidiStatus_.loaded || virmidiStatus_.atStartup);
    const float buttonsW = createW + (canRemove ? px(98) : 0);
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < buttonsW + px(10))  // no room left: on the next line, to the right
        ImGui::NewLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonsW);
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button(createLabel, ImVec2(createW, 0)))
        startVirMidiJob(false);
    ImGui::EndDisabled();
    if (canRemove) {
        ImGui::SameLine(0, px(8));
        if (ImGui::Button("Remove", ImVec2(px(90), 0)))
            startVirMidiJob(true);
        ImGui::SetItemTooltip("Removes the ports and stops creating them at startup");
    }
    ImGui::PopFont();
    ImGui::EndDisabled();
    if (busy) {
        ImGui::PushFont(medium, 12.0f);
        ImGui::TextColored(color(Theme::kOrange), "Waiting for the administrator password...");
        ImGui::PopFont();
    }
    ImGui::Spacing();
}

void GuiApp::drawMappingOverlay(const LiveState& live) {
    using Theme = PerformanceView::Theme;
    const std::string spec = *overlaySpec_;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Centered for its first frames, while its size settles; movable afterwards.
    if (overlayAppearing_) {
        overlayPlacingFrames_ = 3;
        ImGui::SetNextWindowFocus();
        overlayAppearing_ = false;
    }
    if (overlayPlacingFrames_ > 0) {
        --overlayPlacingFrames_;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    // The note loop's editor is wide, for its large piano roll.
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, 0), ImVec2(ImGui::GetFontSize() * (spec == "notes" ? 62 : 40), FLT_MAX));
    pushWindowLook();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(0xFF4A4642));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(0xFF57524D));
    // The channel edited gives the accent, as on the screen.
    const ImU32 accent = performance_.channelColor(live.channel);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(accent));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(accent));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(accent));
    bool open = true;
    const std::string title = MappingEditor::controlTitle(spec) + "###mappingoverlay";
    ImGui::PushFont(uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont(), 16.0f);
    const bool visible = ImGui::Begin(title.c_str(), &open,
                                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopFont();
    if (visible) {
        // Channel 1 edits the base mapping, which every channel plays until it
        // is changed on its own; another channel's first change gives it its own
        // mapping, a copy of channel 1's.
        auto& own = preset_.channels[static_cast<std::size_t>(live.channel)];
        const bool first = live.channel == 0;
        Mapping copy;
        if (!own && !first)
            copy = preset_.base;
        Mapping& target = own ? *own : first ? preset_.base : copy;
        const std::string channelName = "channel " + std::to_string(live.channel + 1);
        bool backToBase = false;  // done once the window no longer uses `own`
        bool copied = false;
        if (spec != "notes") {  // this control as another channel has it
            int& from = copyControlFrom_;
            if (from < 0 || from == live.channel)
                from = live.channel == 0 ? 1 : 0;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Copy from");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
            if (ImGui::BeginCombo("##copyfrom", channelLabel(from).c_str())) {
                for (int ch = 0; ch < kChannelCount; ++ch)
                    if (ch != live.channel && ImGui::Selectable(channelLabel(ch).c_str(), ch == from))
                        from = ch;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy")) {
                const Mapping source = preset_.forChannel(from);
                copied = copyControl(source, target, spec);
            }
            ImGui::Separator();
        }
        if (spec != "notes") {  // the note loop editor goes straight to its piano roll
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::kDim), "%s",
                               own     ? ("Editing the mapping of " + channelName + " (its own)").c_str()
                               : first ? "Editing channel 1: every channel not changed on its own plays this mapping"
                                       : ("Editing " + channelName + ": it plays channel 1's mapping until you change it").c_str());
            if (own) {
                ImGui::SameLine();
                ImGui::PushFont(uiFonts_.medium ? uiFonts_.medium : ImGui::GetFont(), 12.0f);
                if (ImGui::SmallButton("Use channel 1's mapping again"))
                    ImGui::OpenPopup("Use the base mapping");
                ImGui::SetItemTooltip("Removes this channel's own mapping: it then plays channel 1's, as the other channels do");
                ImGui::PopFont();
                if (beginDialog("Use the base mapping")) {
                    ImGui::Text("Channel %d's own mapping is removed; it then plays channel 1's.", live.channel + 1);
                    const int button = dialogButtons({"Remove", "Cancel"});
                    if (button == 0) {
                        backToBase = true;
                        ImGui::CloseCurrentPopup();
                    }
                    if (button == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
                        ImGui::CloseCurrentPopup();
                    endDialog();
                }
            }
            ImGui::Spacing();
        }
        // The forms scroll when the window is too short; the buttons stay in
        // view: what is above (title, Copy from...) and below (the buttons) is
        // taken from the page's height.
        {
            const ImGuiStyle& st = ImGui::GetStyle();
            const float above = ImGui::GetCursorPosY();
            const float below = st.ItemSpacing.y * 3 + st.ItemSpacing.y + ImGui::GetFrameHeight() + st.WindowPadding.y;
            const float room = viewport->WorkSize.y - above - below - ImGui::GetFontSize();  // a margin
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(ImGui::GetFontSize() * 10, room)));
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("mappingbody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);
        ImGui::PopStyleColor();
        editor_.setHumanizeChannels(settings_.humanize.channels);
        // Channel 1's Shift or Loop record control: the same on every channel, set on channel 1.
        const TriggerBinding* firstControl = live.channel != 0 ? specBinding(preset_.base, spec) : nullptr;
        const bool channelOneControl =
            firstControl && (firstControl->type == ActionType::Shift || firstControl->type == ActionType::LoopRecord ||
                             firstControl->type == ActionType::SessionScreen);
        if (channelOneControl) {
            ImGui::TextColored(kWarningColor, "On every channel, this control is channel 1's %s.",
                               firstControl->type == ActionType::Shift        ? "Shift"
                               : firstControl->type == ActionType::LoopRecord ? "Loop record"
                                                                              : "Mode");
            ImGui::Spacing();
        }
        ImGui::BeginDisabled(channelOneControl);
        if (spec == "notes") {  // the note loop's editor: a large piano roll, above its settings
            const float em = ImGui::GetFontSize();
            // SCALE: notes move on the tonality's degrees; CHROMATIC (drums): by semitones, every row named.
            const auto bit = static_cast<std::uint16_t>(1u << live.channel);
            const bool chromatic = (preset_.chromaticChannels & bit) != 0;
            for (int k = 0; k < 2; ++k) {
                const bool on = (k == 1) == chromatic;
                ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.24f, 0.86f, 0.52f, 0.35f) : ImVec4(0.2f, 0.21f, 0.23f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, on ? ImVec4(0.24f, 0.86f, 0.52f, 0.9f) : ImVec4(0.35f, 0.37f, 0.4f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                if (ImGui::Button(k == 0 ? "SCALE" : "CHROMATIC") && !on) {
                    preset_.chromaticChannels = static_cast<std::uint16_t>(preset_.chromaticChannels ^ bit);
                    edited();
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                ImGui::SameLine(0, em * 0.25f);
            }
            ImGui::SameLine(0, em);
            if (settings_.key)
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s %s", pitchName(settings_.key->root).c_str(),
                                   std::string(settings_.key->type().label).c_str());
            else
                ImGui::TextColored(kHintColor, "(no tonality)");
            ImGui::SameLine(0, em);
            if (ImGui::Button("QUANTIZE"))
                performance_.askQuantize();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The selected notes, or all of them, on the grid (Q)");
            performance_.setScaleMoves(!chromatic);
            performance_.drawNoteEditor(target, live, em * 56, em * 24);
            ImGui::Spacing();
        }
        if (editor_.drawControl(spec, target, live, settings_.key) || copied) {
            if (!own && !first)
                own = std::move(copy);  // its first change: the channel gets its own mapping
            edited();
        }
        ImGui::EndDisabled();
        // Channel 1's Loop record control: held or toggled, chosen on each channel.
        if (const TriggerBinding* rec = specBinding(preset_.base, spec); rec && rec->type == ActionType::LoopRecord) {
            const auto bit = static_cast<std::uint16_t>(1u << live.channel);
            const bool toggle = (preset_.loopRecordToggle & bit) != 0;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Mode on channel %d", live.channel + 1);
            for (int k = 0; k < 2; ++k) {
                ImGui::SameLine();
                if (ImGui::RadioButton(k == 0 ? "Momentary##recordmode" : "Toggle##recordmode", toggle == (k == 1)) && toggle != (k == 1)) {
                    preset_.loopRecordToggle = static_cast<std::uint16_t>(preset_.loopRecordToggle ^ bit);
                    edited();
                }
            }
        }
        if (backToBase) {
            own.reset();
            edited();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushFont(uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont(), 13.0f);
        // At the bottom right, as in Settings.
        const ImGuiStyle& style = ImGui::GetStyle();
        float buttonsW = style.ItemSpacing.x * 2;
        const bool song = songOwnsMapping();
        const char* saveLabel = song ? "Save song" : "Save mapping";
        for (const char* label : {saveLabel, "Revert", "Close"})
            buttonsW += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonsW));
        ImGui::BeginDisabled(song ? !songMappingDirty_ : hasFile_ && !dirty_);
        if (ImGui::Button(saveLabel))
            saveShown();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+S");
        ImGui::SameLine();
        ImGui::BeginDisabled(song ? !songMappingDirty_ : !hasFile_ || !dirty_);
        if (ImGui::Button("Revert"))
            ImGui::OpenPopup("Revert the mapping");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                       ImGui::IsKeyPressed(ImGuiKey_Escape) && !(spec == "notes" && performance_.tookEscape()) &&
                                       !editor_.tookEscape()))
            open = false;
        ImGui::PopFont();
        // Revert loses every change since the mapping (or the song) was saved, on every channel: asked first.
        if (beginDialog("Revert the mapping")) {
            ImGui::Text("Every change to the mapping of \"%s\" since it was saved is lost.", song ? sessionName_->c_str() : preset_.name.c_str());
            const int button = dialogButtons({"Revert", "Cancel"});
            if (button == 0) {
                if (song) {  // the song's mapping, as saved
                    preset_ = songMapping_;
                    songMappingDirty_ = false;
                    pushToEngine();
                } else {
                    openPreset(preset_.name);
                }
                ImGui::CloseCurrentPopup();
            }
            if (button == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            endDialog();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(5);
    popWindowLook();
    if (!open)
        overlaySpec_.reset();
}

void GuiApp::pushWindowLook() {
    using Theme = PerformanceView::Theme;
    const float em = ImGui::GetFontSize();
    auto color = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color((Theme::kPanelHeader & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 230)));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, color(Theme::kTopBar));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, color(Theme::kTopBar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, em * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 14 / 16, em * 12 / 16));
}

void GuiApp::popWindowLook() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}

// A modal over the view: the Settings look, its bold title, text in the medium font.
bool GuiApp::beginDialog(const char* title, ImGuiWindowFlags flags) {
    using Theme = PerformanceView::Theme;
    const float em = ImGui::GetFontSize();
    pushWindowLook();
    // As the Settings window: translucent, the view behind it not dimmed.
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
                          ImGui::ColorConvertU32ToFloat4((Theme::kPanelHeader & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 230)));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, em * 0.5f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushFont(uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont(), 16.0f);
    bool keep = true;  // false: its close button was clicked
    const bool open = ImGui::BeginPopupModal(title, &keep, flags | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopFont();
    dialogDismissed_ = !keep;
    if (!open) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        popWindowLook();
        return false;
    }
    ImGui::PushFont(uiFonts_.medium ? uiFonts_.medium : ImGui::GetFont(), 14.0f);
    return true;
}

void GuiApp::endDialog() {
    ImGui::PopFont();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    popWindowLook();
}

// The buttons closing a dialog, at the bottom right as in Settings.
int GuiApp::dialogButtons(std::initializer_list<const char*> labels, int disabled) {
    ImGui::PushFont(uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont(), 13.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float minW = ImGui::GetFontSize() * 80 / 13;
    float total = 0;
    for (const char* label : labels)
        total += std::max(minW, ImGui::CalcTextSize(label).x + style.FramePadding.x * 2) + style.ItemSpacing.x;
    total -= style.ItemSpacing.x;
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - total));
    int clicked = -1, i = 0;
    for (const char* label : labels) {
        if (i > 0)
            ImGui::SameLine();
        ImGui::BeginDisabled(i == disabled);
        if (ImGui::Button(label, ImVec2(std::max(minW, ImGui::CalcTextSize(label).x + style.FramePadding.x * 2), 0)))
            clicked = i;
        ImGui::EndDisabled();
        ++i;
    }
    ImGui::PopFont();
    return clicked;
}

// --- Sessions ---------------------------------------------------------------

std::vector<std::string> GuiApp::sessionNames() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sessionsDir_, ec))
        if (entry.is_regular_file() && entry.path().extension() == kSessionExtension)
            names.push_back(entry.path().stem().string());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) <
                                                                        std::tolower(static_cast<unsigned char>(y)); });
    });
    return names;
}

void GuiApp::openSession(const std::string& name) {
    Session session;
    try {
        session = loadSession(sessionPath(name));
    } catch (const std::exception& e) {
        showError(std::string("Cannot open the session:\n") + e.what());
        return;
    }
    // Its preset, as it was: the one of that name if unchanged, otherwise a
    // changed copy to save (or not).
    // Its mapping, as it was: the song's own (the mapping files are left alone).
    preset_ = session.preset;
    hasFile_ = !PresetLibrary::nameError(preset_.name).has_value() && library_->contains(preset_.name);
    dirty_ = false;
    presetTime_.reset();
    songMapping_ = preset_;
    songMappingDirty_ = false;
    settings_.key = session.key;
    // Its tempo: for the internal clock, or for a Link session Arcadance is
    // alone in (another app's tempo is never changed by opening a session).
    if (session.tempo > 0) {
        const LiveState now = runtime_->live();
        if (settings_.clock != AppSettings::Clock::Link) {
            settings_.tempo = session.tempo;
            runtime_->setInternalTempo(session.tempo);
        } else if (now.linkPeers == 0) {
            runtime_->setTempo(session.tempo);
        }
    }
    persistSettings();
    pushToEngine();
    runtime_->setChannel(session.channel);
    runtime_->loadSessionLoops(std::move(session.loops));
    // Its chains (the others emptied), their names, the one shown on each channel.
    shownChain_.fill(0);
    for (auto& names : chainNames_)
        names.fill({});
    for (const SessionChain& c : session.chains) {
        if (c.channel < 0 || c.channel >= kChannelCount || c.index < 0 || c.index >= Engine::kChains)
            continue;
        chainNames_[static_cast<std::size_t>(c.channel)][static_cast<std::size_t>(c.index)] = c.name;
        if (c.shown)
            shownChain_[static_cast<std::size_t>(c.channel)] = c.index;
    }
    runtime_->loadSessionChains(std::move(session.chains));
    runtime_->setMuteSolo(session.muted, session.soloed);
    rowNames_ = session.rowNames;
    for (auto& names : clipNames_)
        names.fill({});
    for (const Session::ClipName& c : session.clipNames)
        if (c.channel >= 0 && c.channel < kChannelCount && c.clip >= 0 && c.clip < Engine::kClipSlots)
            clipNames_[static_cast<std::size_t>(c.channel)][static_cast<std::size_t>(c.clip)] = c.name;
    clipSeen_.fill(0);
    runtime_->stopSong();
    song_ = std::move(session.song);
    songLoopFrom_ = songLoopTo_ = songStartBar_ = -1;
    runtime_->setSongLoopRange(-1, -1);
    songCursor_ = {};
    songEdited();
    resetUndo();
    sessionName_ = name;
    sessionPrintIn_ = 10;  // taken once its loops and chains are published
    log("Session \"" + name + "\" opened.");
}

void GuiApp::newSong() {
    runtime_->stopSong();
    runtime_->transportStop();
    runtime_->loadSessionLoops({});
    runtime_->loadSessionChains({});
    runtime_->setMuteSolo(0, 0);
    shownChain_.fill(0);
    for (auto& names : chainNames_)
        names.fill({});
    rowNames_.fill({});
    for (auto& names : clipNames_)
        names.fill({});
    clipSeen_.fill(0);
    song_ = {};
    songCursor_ = {};
    songEdited();
    resetUndo();
    sessionName_.reset();
    songMappingDirty_ = false;
    songLoopFrom_ = songLoopTo_ = songStartBar_ = -1;
    runtime_->setSongLoopRange(-1, -1);
    if (library_->contains(preset_.name))  // the mapping again, as its file has it
        openPreset(preset_.name);
    else
        dirty_ = false;
    sessionPrintIn_ = 10;
    log("New song.");
}

void GuiApp::exportMidi(int channel, int slot) {
    auto session = collectSession();
    auto fail = [&](const std::string& why) {
        toast_ = why;
        toastPlain_ = true;
        toastUntil_ = ImGui::GetTime() + 6.0;
    };
    if (!session)
        return fail("Not exported: the session did not answer");
    midipad::MidiExportOptions o;
    o.tempo = session->tempo > 0 ? session->tempo : settings_.tempo;
    o.beatsPerBar = settings_.beatsPerBar;
    o.title = sessionName_.value_or("Arcadance");
    o.channelNames = preset_.channelNames;
    auto clean = [](std::string name) {  // a file name: no slash
        std::replace(name.begin(), name.end(), '/', '-');
        return name;
    };
    std::string name;
    std::vector<std::uint8_t> bytes;
    if (channel < 0) {
        bytes = midipad::exportSongMidi(*session, o);
        name = o.title + " - song";
    } else {
        const std::string& chName = preset_.channelNames[static_cast<std::size_t>(channel)];
        const std::string& clip = clipNames_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
        name = o.title + " - " + (chName.empty() ? "channel " + std::to_string(channel + 1) : chName) + " - " + (clip.empty() ? "clip " + std::to_string(slot + 1) : clip);
        o.title = name;
        bytes = midipad::exportClipMidi(*session, o, channel, slot);
    }
    if (bytes.empty())
        return fail("Nothing to export");
    fs::path dir;
    if (const char* music = SDL_GetUserFolder(SDL_FOLDER_MUSIC))
        dir = fs::path(music) / "Arcadance";
    else
        dir = prefPath() / "MIDI";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (clean(name) + ".mid");
    try {
        midipad::writeFile(file, bytes);
    } catch (const std::exception& e) {
        log(e.what());
        return fail("Not exported: " + file.string() + " cannot be written");
    }
    log("Exported " + file.string());
    toast_ = "Exported: " + file.filename().string();
    toastPlain_ = true;
    toastUntil_ = ImGui::GetTime() + 6.0;
    SDL_OpenURL(("file://" + dir.string()).c_str());
}

void GuiApp::saveSession() {
    if (!sessionName_) {
        nameDialog_ = NameDialog::SessionSaveAs;
        nameInput_ = "Session";
        nameError_.clear();
        openNameDialog_ = true;
        return;
    }
    saveSessionAs(*sessionName_);
}

std::optional<Session> GuiApp::collectSession() {
    auto loops = runtime_->sessionLoops();
    if (!loops)
        return std::nullopt;
    Session session;
    session.preset = preset_;
    session.key = settings_.key;
    const LiveState now = runtime_->live();
    session.channel = now.channel;
    session.tempo = now.clock != ClockMode::Free ? now.tempo : settings_.tempo;
    session.loops = std::move(*loops);
    session.song = song_;
    session.muted = now.muted;
    session.soloed = now.soloed;
    session.rowNames = rowNames_;
    for (int ch = 0; ch < kChannelCount; ++ch)
        for (int k = 0; k < Engine::kClipSlots; ++k)
            if (const std::string& n = clipNames_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(k)]; !n.empty())
                session.clipNames.push_back({ch, k, n});
    for (int ch = 0; ch < kChannelCount; ++ch) {  // the chains that hold something, or are shown
        const auto c = static_cast<std::size_t>(ch);
        for (int n = 0; n < Engine::kChains; ++n) {
            const LiveState::Chain& live = now.chains[c].list[static_cast<std::size_t>(n)];
            SessionChain chain;
            chain.channel = ch;
            chain.index = n;
            chain.then = live.then;
            chain.name = chainNames_[c][static_cast<std::size_t>(n)];
            chain.shown = n == shownChain_[c] && n != 0;
            for (int k = 0; k < Engine::kChainSteps; ++k)
                chain.steps.push_back({live.clip[static_cast<std::size_t>(k)], live.repeats[static_cast<std::size_t>(k)]});
            if (!live.empty() || live.then != Engine::kThenLoop || !chain.name.empty() || chain.shown)
                session.chains.push_back(std::move(chain));
        }
    }
    return session;
}

std::string GuiApp::sessionPrint(Session session, bool withMapping) {
    if (!withMapping)
        session.preset = {};
    session.key.reset();
    session.channel = 0;
    session.tempo = 0;
    return midipad::sessionToJson(session).dump();
}

bool GuiApp::saveSessionAs(const std::string& name) {
    auto collected = collectSession();
    if (!collected) {
        showError("Cannot save the session:\nthe loops did not answer.");
        return false;
    }
    Session& session = *collected;
    try {
        midipad::saveSession(session, sessionPath(name));
    } catch (const std::exception& e) {
        showError(std::string("Cannot save the session:\n") + e.what());
        return false;
    }
    sessionName_ = name;
    sessionPrint_ = sessionPrint(session, true);
    songMapping_ = preset_;
    songMappingDirty_ = false;
    dirty_ = false;
    log("Session \"" + name + "\" saved (" + std::to_string(session.loops.size()) + " loops).");
    return true;
}

void GuiApp::quitAsked() {
    std::optional<Session> now = collectSession();
    if (now && sessionPrintIn_ > 0)  // just opened or started: nothing changed since
        sessionPrint_ = sessionPrint(*now, songOwnsMapping());
    sessionPrintIn_ = 0;
    if (now && sessionPrint_ && sessionPrint(*now, songOwnsMapping()) == *sessionPrint_)
        quit_ = true;
    else
        openQuitDialog_ = true;
}

void GuiApp::drawQuitDialog() {
    constexpr const char* kTitle = "Quit";
    if (openQuitDialog_) {
        ImGui::OpenPopup(kTitle);
        openQuitDialog_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    ImGui::Text("Save the session \"%s\" before quitting?", sessionName_.value_or("Untitled").c_str());
    const int button = dialogButtons({"Save", "Don't save", "Cancel"});
    if (button == 0) {
        ImGui::CloseCurrentPopup();
        if (sessionName_) {
            if (saveSessionAs(*sessionName_))
                quit_ = true;
        } else {
            afterSave_ = [this] { quit_ = true; };
            saveSession();  // asks for a name, then runs afterSave_
        }
    }
    if (button == 1) {
        ImGui::CloseCurrentPopup();
        quit_ = true;
    }
    if (button == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    endDialog();
}

void GuiApp::drawOpenSessionDialog() {
    constexpr const char* kTitle = "Open session";
    if (openSessionDialog_) {
        ImGui::OpenPopup(kTitle);
        openSessionDialog_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    bool open = false;
    if (sessionList_.empty()) {
        ImGui::TextUnformatted("No session saved yet.");
    } else {
        if (ImGui::BeginChild("##sessions", ImVec2(ImGui::GetFontSize() * 26, 0),
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
            for (int i = 0; i < static_cast<int>(sessionList_.size()); ++i) {
                const std::string& name = sessionList_[static_cast<std::size_t>(i)];
                if (ImGui::Selectable(name.c_str(), sessionChoice_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                    sessionChoice_ = i;
                    open = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                }
                if (ImGui::BeginPopupContextItem()) {  // right-click: Delete
                    if (ImGui::MenuItem("Delete\u2026")) {
                        deleteSessionName_ = name;
                        openDeleteSessionDialog_ = true;
                    }
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::EndChild();
        ImGui::TextColored(kHintColor, "Opening one replaces every loop, the mapping and the tonality.");
    }
    const int button = dialogButtons({"Open", "Cancel"}, sessionChoice_ < 0 ? 0 : -1);  // Open: once one is chosen
    if ((button == 0 || open) && sessionChoice_ >= 0) {
        ImGui::CloseCurrentPopup();
        const std::string name = sessionList_[static_cast<std::size_t>(sessionChoice_)];
        guard([this, name] { openSession(name); });
    }
    // Deleting a session, once confirmed: over this window.
    constexpr const char* kDeleteTitle = "Delete session";
    if (button == 1 || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(kDeleteTitle)))
        ImGui::CloseCurrentPopup();
    if (openDeleteSessionDialog_) {
        ImGui::OpenPopup(kDeleteTitle);
        openDeleteSessionDialog_ = false;
    }
    if (beginDialog(kDeleteTitle)) {
        ImGui::Text("Delete the session \"%s\"?", deleteSessionName_.c_str());
        ImGui::TextColored(kHintColor, "The file is deleted for good.");
        const int confirm = dialogButtons({"Delete", "Cancel"});
        if (confirm == 0) {
            std::error_code ec;
            if (fs::remove(sessionPath(deleteSessionName_), ec)) {
                log("Session deleted: " + sessionPath(deleteSessionName_).string());
                if (sessionName_ == deleteSessionName_)
                    sessionName_.reset();
            } else {
                showError("Cannot delete the session:\n" + (ec ? ec.message() : std::string("file not found")));
            }
            sessionList_ = sessionNames();
            sessionChoice_ = -1;
            ImGui::CloseCurrentPopup();
        }
        if (confirm == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        endDialog();
    }
    endDialog();
}

void GuiApp::drawAboutDialog() {
    constexpr const char* kTitle = "About Arcadance";
    if (openAboutDialog_) {
        ImGui::OpenPopup(kTitle);
        openAboutDialog_ = false;
        updateResult_.reset();
    }
    if (updateJob_.valid() && updateJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        updateResult_ = updateJob_.get();
    if (!beginDialog(kTitle))
        return;
    ImGui::PushFont(uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont(), 22.0f);
    ImGui::TextUnformatted("Arcadance");
    ImGui::PopFont();
    ImGui::Text("Version %s", MIDIPAD_VERSION);
    ImGui::Spacing();
    ImGui::TextUnformatted("Turns a game controller into a MIDI instrument and a live MIDI looper.");
    ImGui::TextColored(kHintColor, "Free software under the GNU General Public License 3.0.");
    ImGui::Spacing();
    const bool checking = updateJob_.valid();
    if (checking)
        ImGui::TextColored(kHintColor, "Checking GitHub for a newer version…");
    else if (updateResult_)
        ImGui::TextColored(updateResult_->ok ? (updateResult_->newer ? ImVec4(0.45f, 0.85f, 0.55f, 1.0f) : ImVec4(1, 1, 1, 1)) : kErrorColor,
                           "%s", updateResult_->message.c_str());
    const bool offerPage = updateResult_ && (updateResult_->newer || !updateResult_->ok);
    ImGui::BeginDisabled(checking);
    const int button = offerPage ? dialogButtons({"Download page", "Check for updates", "Close"})
                                 : dialogButtons({"Check for updates", "Close"});
    ImGui::EndDisabled();
    const int check = offerPage ? 1 : 0, close = offerPage ? 2 : 1;
    if (offerPage && button == 0)
        SDL_OpenURL(kReleasesUrl);
    if (button == check) {
        updateResult_.reset();
        updateJob_ = std::async(std::launch::async, checkForUpdates);
    }
    if (button == close || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    endDialog();
}

void GuiApp::applySync() {
    runtime_->setLinkStartStopSync(settings_.startStopSync);
    runtime_->setSync(settings_.snapBeats(), settings_.beatsPerBar, settings_.quantize);
    runtime_->setInternalTempo(settings_.tempo);
    runtime_->setMidiClockOut(settings_.midiClockOut);
    runtime_->setClock(static_cast<ClockMode>(settings_.clock));  // the same order: Free, Internal, Link
}

// A tempo for the clock running, to a tenth; the internal one is saved a
// moment later.
void GuiApp::applyTempo(double bpm) {
    bpm = std::clamp(std::round(bpm * 10.0) / 10.0, TapTempo::kMinTempo, TapTempo::kMaxTempo);
    runtime_->setTempo(bpm);
    if (settings_.clock == AppSettings::Clock::Internal) {
        settings_.tempo = bpm;
        tempoSaveAt_ = ImGui::GetTime() + 1.0;
    }
}

void GuiApp::drawDialogs() {
    drawRenameDialog();
    drawNameDialog();
    drawUnsavedDialog();
    drawDeleteDialog();
    drawErrorDialog();
    drawOutputDialog();
    drawOpenSessionDialog();
    drawNewSongDialog();
    drawQuitDialog();
    drawCopyDialog();
    drawColourDialog();
    drawValueAsk();
    drawAboutDialog();
}

// Renames a channel (kept in the preset) or one of its chains (kept in the session).
void GuiApp::drawRenameDialog() {
    constexpr const char* kTitle = "Rename";
    if (openRename_) {
        ImGui::OpenPopup(kTitle);
        openRename_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    const bool row = renameRow_ >= 0 && renameRow_ < Engine::kClipSlots;
    const bool clip = !row && renameClipChannel_ >= 0 && renameClipChannel_ < kChannelCount && renameClipSlot_ >= 0 && renameClipSlot_ < Engine::kClipSlots;
    const bool section = !row && !clip && renameSection_ >= 0 && renameSection_ < static_cast<int>(song_.sections.size());
    const bool chain = !row && !clip && !section && renameChain_ >= 0 && renameChain_ < Engine::kChains;
    if (row)
        ImGui::Text("Row %d", renameRow_ + 1);
    else if (clip)
        ImGui::Text("Clip %d  \xC2\xB7  channel %d", renameClipSlot_ + 1, renameClipChannel_ + 1);
    else if (section)
        ImGui::Text("Section %d", renameSection_ + 1);
    else if (chain)
        ImGui::Text("Chain %s  \xC2\xB7  channel %d", PerformanceView::chainLetter(renameChain_).c_str(), renameChannel_ + 1);
    else
        ImGui::Text("Channel %d", renameChannel_ + 1);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    const bool inSession = chain || section || row || clip;
    const bool enter = ImGui::InputTextWithHint("##channelname", clip ? "GROOVE, FILL, RISER\xE2\x80\xA6" : inSession ? "VERSE, CHORUS, BREAK\xE2\x80\xA6" : "BASS, PADS, DRUMS\xE2\x80\xA6",
                                                &renameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled(inSession ? "Kept in the session. Empty: no name." : "Kept in the mapping. Empty: no name.");
    const int button = dialogButtons({"OK", "Cancel"});
    if (button == 0 || enter) {
        if (row || clip || section || (renameChannel_ >= 0 && renameChannel_ < kChannelCount)) {
            std::string name = truncateUtf8(renameInput_, 24);
            while (!name.empty() && name.back() == ' ')
                name.pop_back();
            if (row) {
                rowNames_[static_cast<std::size_t>(renameRow_)] = name;
            } else if (clip) {
                rememberClips({{renameClipChannel_, renameClipSlot_}}, "Clip renamed");
                clipNames_[static_cast<std::size_t>(renameClipChannel_)][static_cast<std::size_t>(renameClipSlot_)] = name;
            } else if (section) {
                song_.sections[static_cast<std::size_t>(renameSection_)].name = name;
                songEdited();
            } else if (chain) {
                rememberChains(renameChannel_, "Chain renamed");
                chainNames_[static_cast<std::size_t>(renameChannel_)][static_cast<std::size_t>(renameChain_)] = name;
            } else {
                preset_.channelNames[static_cast<std::size_t>(renameChannel_)] = name;
                edited();
            }
        }
        ImGui::CloseCurrentPopup();
    } else if (button == 1) {
        ImGui::CloseCurrentPopup();
    }
    endDialog();
}

GuiApp::NavConflict GuiApp::navConflict(int button) const {
    if (button < 0)
        return {};
    const auto b = static_cast<Button>(button);
    const TriggerBinding* binding = &preset_.base.buttons[static_cast<std::size_t>(button)];
    bool dpad = false;
    switch (b) {
    case Button::DpadUp: binding = &preset_.base.dpad[static_cast<std::size_t>(HatDir::Up)]; dpad = true; break;
    case Button::DpadDown: binding = &preset_.base.dpad[static_cast<std::size_t>(HatDir::Down)]; dpad = true; break;
    case Button::DpadLeft: binding = &preset_.base.dpad[static_cast<std::size_t>(HatDir::Left)]; dpad = true; break;
    case Button::DpadRight: binding = &preset_.base.dpad[static_cast<std::size_t>(HatDir::Right)]; dpad = true; break;
    default: break;
    }
    if (binding->type == ActionType::LoopRecord)
        return {true, "Loop record: you could not record in the session screen"};
    if (binding->type == ActionType::Shift)
        return {true, "Shift: Shift + Mode would no longer open the edit mode"};
    if (binding->type == ActionType::SessionScreen || (b == Button::Guide && bindingName(ActionType::SessionScreen, "").empty()))
        return {true, "it is the Mode button"};
    std::string text = binding->isSet() ? "plays " + describe(*binding, false) + " on the main screen" : "";
    if (dpad)
        text += std::string(text.empty() ? "" : "; ") + "no longer moves the cursor (the left stick still does)";
    return {false, text};
}

void GuiApp::drawNameDialog() {
    const bool session = nameDialog_ == NameDialog::SessionSaveAs;
    const char* title = nameDialog_ == NameDialog::New ? "New mapping" : session ? "Save session as" : songOwnsMapping() ? "Save as a mapping" : "Save as";
    if (openNameDialog_) {
        ImGui::OpenPopup(title);
        openNameDialog_ = false;
        replaceName_.clear();
    }
    if (!beginDialog(title)) {
        if (dialogDismissed_)
            afterSave_ = nullptr;
        return;
    }
    ImGui::TextUnformatted(session ? "Session name" : "Mapping name");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
    const bool enter = ImGui::InputText("##name", &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool replacing = session && !replaceName_.empty() && replaceName_ == nameInput_;
    if (replacing)
        ImGui::TextColored(kWarningColor, "A session named \"%s\" already exists.\nReplace it?", nameInput_.c_str());
    else if (!nameError_.empty())
        ImGui::TextColored(kErrorColor, "%s", nameError_.c_str());

    const int button = dialogButtons({replacing ? "Replace" : "OK", "Cancel"});
    if (button == 0 || enter) {
        if (auto error = PresetLibrary::nameError(nameInput_)) {
            nameError_ = *error;
        } else if (session) {
            std::error_code ec;
            if (fs::exists(sessionPath(nameInput_), ec) && !replacing)
                replaceName_ = nameInput_;  // asks first
            else if (saveSessionAs(nameInput_)) {
                ImGui::CloseCurrentPopup();
                if (afterSave_)
                    std::exchange(afterSave_, nullptr)();
            }
        } else if (library_->contains(nameInput_)) {
            nameError_ = "A mapping already has this name.";
        } else if (nameDialog_ == NameDialog::New) {
            preset_ = newPreset();
            preset_.name = nameInput_;
            hasFile_ = true;
            dirty_ = true;
            pushToEngine();  // the engine plays what the window shows, saved or not
            if (saveCurrent()) {
                settings_.lastPreset = preset_.name;
                persistSettings();
                refreshPresetList();
            }
            ImGui::CloseCurrentPopup();
        } else if (songOwnsMapping()) {  // the song's mapping, copied into a mapping file: the song keeps its own
            Preset copy = preset_;
            copy.name = nameInput_;
            try {
                library_->save(copy);
                refreshPresetList();
                log("Mapping \"" + copy.name + "\" saved from the song.");
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& e) {
                nameError_ = std::string("Cannot save: ") + e.what();
            }
        } else if (saveAs(nameInput_)) {
            ImGui::CloseCurrentPopup();
            if (afterSave_)
                std::exchange(afterSave_, nullptr)();
        }
    }
    if (button == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        afterSave_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    endDialog();
}

// A channel's colour (kept in the preset): one of 48, any other, or its own
// again; shown at once on the session and song screens.
void GuiApp::drawColourDialog() {
    constexpr const char* kTitle = "Colour";
    if (openColour_) {
        ImGui::OpenPopup(kTitle);
        openColour_ = false;
    }
    if (colourChannel_ < 0 || colourChannel_ >= kChannelCount || !beginDialog(kTitle))
        return;
    auto& own = preset_.channelColors[static_cast<std::size_t>(colourChannel_)];
    const ImU32 shown = performance_.channelColor(colourChannel_);
    auto set = [&](std::optional<std::uint32_t> colour) {
        if (colour == own)
            return;
        own = colour;
        mappingChanged();  // the mapping changed; the engine has no use for it
    };
    ImGui::TextUnformatted(channelLabel(colourChannel_).c_str());
    ImGui::Spacing();
    // 12 hues on 4 rows: pale, bright, deep, dark.
    static constexpr float kRows[4][2] = {{0.45f, 1.0f}, {0.70f, 1.0f}, {0.85f, 0.85f}, {0.90f, 0.60f}};
    const float swatch = ImGui::GetFontSize() * 1.5f;
    for (int row = 0; row < 4; ++row)
        for (int hue = 0; hue < 12; ++hue) {
            float r = 0, g = 0, b = 0;
            ImGui::ColorConvertHSVtoRGB(static_cast<float>(hue) / 12.0f, kRows[row][0], kRows[row][1], r, g, b);
            const auto rgb = static_cast<std::uint32_t>(std::lround(r * 255)) << 16 | static_cast<std::uint32_t>(std::lround(g * 255)) << 8 |
                             static_cast<std::uint32_t>(std::lround(b * 255));
            if (hue > 0)
                ImGui::SameLine(0, ImGui::GetFontSize() * 0.25f);
            ImGui::PushID(row * 12 + hue);
            const bool chosen = own && *own == rgb;
            if (chosen)
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 255));
            if (chosen)
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            if (ImGui::ColorButton("##swatch", ImVec4(r, g, b, 1), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder * !chosen,
                                   ImVec2(swatch, swatch)))
                set(rgb);
            if (chosen) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
    ImGui::Spacing();
    float rgb[3] = {static_cast<float>((shown >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f, static_cast<float>((shown >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                    static_cast<float>((shown >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f};
    ImGui::SetNextItemWidth(swatch * 12 + ImGui::GetFontSize() * 0.25f * 11 - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::ColorPicker3("##picker", rgb,
                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayRGB |
                                ImGuiColorEditFlags_DisplayHex))
        set(static_cast<std::uint32_t>(std::lround(rgb[0] * 255)) << 16 | static_cast<std::uint32_t>(std::lround(rgb[1] * 255)) << 8 |
            static_cast<std::uint32_t>(std::lround(rgb[2] * 255)));
    const int button = dialogButtons({"Default", "Close"});
    if (button == 0)
        set(std::nullopt);
    if (button == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    endDialog();
}

void GuiApp::drawValueAsk() {
    constexpr const char* kId = "##valueask";
    if (openValueAsk_) {
        ImGui::OpenPopup(kId);
        openValueAsk_ = false;
    }
    if (!valueAsk_ || !ImGui::BeginPopup(kId))
        return;
    using K = ValueAsk::Kind;
    ValueAsk& v = *valueAsk_;
    static constexpr const char* kNames[] = {"Repeats", "Repeats", "Beats", "Bars", "Velocity", "Chance %", "Times"};
    ImGui::TextUnformatted(kNames[static_cast<int>(v.kind)]);
    ImGui::SameLine();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    const bool enter = ImGui::InputInt("##value", &v.value, 1, 4, ImGuiInputTextFlags_EnterReturnsTrue);
    if (v.kind == K::ChainRepeats && v.value == 0) {
        ImGui::SameLine();
        ImGui::TextUnformatted("\xE2\x88\x9E");
    }
    if (enter) {
        switch (v.kind) {
        case K::ChainRepeats:
            undoWhat_ = "Repeats (step " + std::to_string(v.row + 1) + ")";
            setChainStep(v.col, v.row, v.item, std::clamp(v.value, 0, Engine::kMaxRepeats));
            break;
        case K::ItemRepeats:
        case K::RestBeats:
            if (auto* items = songCell(v.row, v.col); items && v.item >= 0 && v.item < static_cast<int>(items->size())) {
                auto& it = (*items)[static_cast<std::size_t>(v.item)];
                if (v.kind == K::RestBeats)
                    it.beats = std::clamp(v.value, 1, midipad::kMaxRestBeats);
                else
                    it.repeats = std::clamp(v.value, 1, midipad::kMaxSongRepeats);
                undoWhat_ = v.kind == K::RestBeats ? "Beats (silence)" : "Repeats (item)";
                songEdited();
            }
            break;
        case K::NoteVelocity:
        case K::NoteChance: {
            const int value = v.kind == K::NoteChance ? std::clamp(v.value, 1, 100) : std::clamp(v.value, 1, 127);
            std::vector<int> set(valueAskNotes_.size(), value), keep(valueAskNotes_.size(), -1);
            {
                const LiveState now = runtime_->live();
                rememberClips({{now.channel, now.currentClip[static_cast<std::size_t>(now.channel)]}}, v.kind == K::NoteChance ? "Chance" : "Velocity");
            }
            if (v.kind == K::NoteChance)
                runtime_->setLoopNoteValues(valueAskNotes_, keep, set);
            else
                runtime_->setLoopNoteValues(valueAskNotes_, set, keep);
            break;
        }
        case K::SectionRepeats:
            if (v.row >= 0 && v.row < static_cast<int>(song_.sections.size())) {
                song_.sections[static_cast<std::size_t>(v.row)].repeats = std::clamp(v.value, 1, midipad::kMaxSectionRepeats);
                undoWhat_ = "Times played (section " + std::to_string(v.row + 1) + ")";
                songEdited();
            }
            break;
        case K::SectionBars:
            if (v.row >= 0 && v.row < static_cast<int>(song_.sections.size())) {
                auto& sec = song_.sections[static_cast<std::size_t>(v.row)];
                sec.bars = std::clamp(v.value, 1, midipad::kMaxSectionBars);
                sec.fit = false;
                undoWhat_ = "Bars (section " + std::to_string(v.row + 1) + ")";
                songEdited();
            }
            break;
        }
        undoWhat_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GuiApp::drawCopyDialog() {
    constexpr const char* kTitle = "Copy mapping";
    if (openCopyDialog_) {
        ImGui::OpenPopup(kTitle);
        openCopyDialog_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    ImGui::TextUnformatted(("To " + channelLabel(copyTo_)).c_str());
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("From");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::BeginCombo("##from", channelLabel(copyFrom_).c_str())) {
        for (int ch = 0; ch < kChannelCount; ++ch)
            if (ch != copyTo_ && ImGui::Selectable(channelLabel(ch).c_str(), ch == copyFrom_))
                copyFrom_ = ch;
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    for (auto [part, name] : kCopyParts) {
        bool on = (copyParts_ & part) != 0;
        if (ImGui::Checkbox(name, &on))
            copyParts_ = on ? copyParts_ | part : copyParts_ & ~static_cast<unsigned>(part);
    }
    const int button = dialogButtons({"Copy", "Cancel"});
    if (button == 0 && copyParts_ != 0) {
        const Mapping from = preset_.forChannel(copyFrom_);  // a copy: the target may be the same mapping
        auto& own = preset_.channels[static_cast<std::size_t>(copyTo_)];
        if (copyTo_ != 0 && !own)
            own = preset_.base;  // the channel gets its own mapping
        copyParts(from, copyTo_ == 0 ? preset_.base : *own, copyParts_);
        edited();
        log("Mapping copied from channel " + std::to_string(copyFrom_ + 1) + " to channel " + std::to_string(copyTo_ + 1) + ".");
        ImGui::CloseCurrentPopup();
    }
    if (button == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    endDialog();
}

void GuiApp::drawNewSongDialog() {
    constexpr const char* kTitle = "New song";
    if (openNewSongDialog_) {
        ImGui::OpenPopup(kTitle);
        openNewSongDialog_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    ImGui::Text("Save the session \"%s\" before starting a new song?", sessionName_.value_or("Untitled").c_str());
    ImGui::TextColored(kHintColor, "The new song starts empty: no clip, chain or section.\nIts mapping is the mapping the song started from, as saved; the tonality stays.");
    const int button = dialogButtons({"Save", "Don't save", "Cancel"});
    if (button == 0) {
        ImGui::CloseCurrentPopup();
        if (sessionName_) {
            if (saveSessionAs(*sessionName_))
                newSong();
        } else {
            afterSave_ = [this] { newSong(); };
            saveSession();  // asks for a name, then runs afterSave_
        }
    }
    if (button == 1) {
        ImGui::CloseCurrentPopup();
        newSong();
    }
    if (button == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    endDialog();
}

void GuiApp::drawUnsavedDialog() {
    constexpr const char* kTitle = "Unsaved changes";
    if (openUnsavedDialog_) {
        ImGui::OpenPopup(kTitle);
        openUnsavedDialog_ = false;
    }
    if (!beginDialog(kTitle)) {
        if (dialogDismissed_)
            pendingAction_ = nullptr;
        return;
    }
    ImGui::Text("The mapping \"%s\" was changed.", hasFile_ ? preset_.name.c_str() : "Untitled");
    const int button = dialogButtons({"Save", "Don't save", "Cancel"});
    if (button == 0) {
        ImGui::CloseCurrentPopup();
        if (hasFile_) {
            if (saveCurrent() && pendingAction_)
                std::exchange(pendingAction_, nullptr)();
        } else {
            afterSave_ = std::exchange(pendingAction_, nullptr);
            saveCurrent();  // asks for a name, then runs afterSave_
        }
    }
    if (button == 1) {
        ImGui::CloseCurrentPopup();
        dirty_ = false;
        if (pendingAction_)
            std::exchange(pendingAction_, nullptr)();
    }
    if (button == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pendingAction_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    endDialog();
}

void GuiApp::drawDeleteDialog() {
    constexpr const char* kTitle = "Delete mapping";
    if (openDeleteDialog_) {
        ImGui::OpenPopup(kTitle);
        openDeleteDialog_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    ImGui::Text("Delete the mapping \"%s\"?", preset_.name.c_str());
    ImGui::TextColored(kHintColor, "The file is deleted for good.");
    const int button = dialogButtons({"Delete", "Cancel"});
    if (button == 0) {
        try {
            const fs::path removed = library_->remove(preset_.name);
            log("Preset deleted: " + removed.string());
            refreshPresetList();
            openFirstOrUntitled();
        } catch (const std::exception& e) {
            showError(std::string("Cannot delete:\n") + e.what());
        }
        ImGui::CloseCurrentPopup();
    }
    if (button == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    endDialog();
}

void GuiApp::drawErrorDialog() {
    constexpr const char* kTitle = "Error";
    if (openErrorDialog_) {
        ImGui::OpenPopup(kTitle);
        openErrorDialog_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    ImGui::TextUnformatted(errorMessage_.c_str());
    if (dialogButtons({"OK"}) == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
        ImGui::CloseCurrentPopup();
    endDialog();
}

void GuiApp::drawOutputDialog() {
    constexpr const char* kTitle = "MIDI output";
    if (openOutputDialog_) {
        refreshPorts();
        ImGui::OpenPopup(kTitle);
        openOutputDialog_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, 0), ImGuiCond_Appearing);
    if (!beginDialog(kTitle, 0)) {
        outputDialogOpen_ = false;
        return;
    }
    if (!outputDialogOpen_) {
        outputDialogOpen_ = true;
        virmidiStatus_ = virmidi::status();
        virmidiAtStartup_ = true;  // they were lost at the last restart, most likely
    }
    if (closeOutputDialog_) {
        closeOutputDialog_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::TextWrapped("%s", outputDialogReason_.c_str());
    ImGui::Spacing();
    // No VirMIDI port: offer to create them first, some programs see nothing else.
    if (virmidi::supported() && virmidiPortCount() == 0) {
        drawVirMidiSection(true);
        ImGui::Separator();
        ImGui::Spacing();
    }

    ImGui::BeginChild("ports", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9), ImGuiChildFlags_Borders);
    if (ports_.empty())
        ImGui::TextDisabled("No MIDI port found.");
    for (const auto& port : ports_) {
        const std::string name = rawMidiName(port);
        const std::string text = name.empty() ? port : port + "      -> seen as " + name + "/…";
        if (ImGui::Selectable(text.c_str()) && useOutput(AppSettings::Output::ExistingPort, port))
            ImGui::CloseCurrentPopup();
    }
    ImGui::EndChild();

    const std::string virtualLabel = "Virtual port \"" + settings_.virtualPortName + "\"";
    const int button = dialogButtons({"Refresh", virtualLabel.c_str(), "Later"});
    if (button == 0)
        refreshPorts();
    if (button == 1 && useOutput(AppSettings::Output::VirtualPort, {}))
        ImGui::CloseCurrentPopup();
    if (button == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::SetItemTooltip("Keeps the virtual port for now; the question comes back at the next start");
    endDialog();
}

}  // namespace

int runGui(const std::vector<std::string>& args) {
    GuiOptions options = parseGuiArgs(args);
    if (options.help) {
        std::cout << kUsage;
        return 0;
    }
    if (!options.error.empty()) {
        std::cerr << "arcadance: " << options.error << "\n\n" << kUsage;
        return 2;
    }
    GuiApp app(std::move(options));
    return app.run();
}
