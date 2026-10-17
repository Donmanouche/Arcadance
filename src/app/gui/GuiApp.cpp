// The MidiPad window: preset management, MIDI output, mapping editor, log.
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
#include "midipad/Labels.hpp"
#include "midipad/PresetIO.hpp"
#include "midipad/PresetLibrary.hpp"
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
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
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
    char* path = SDL_GetPrefPath("MidiPad", "MidiPad");
    fs::path result = path ? fs::path(path) : fs::current_path();
    SDL_free(path);
    return result;
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

// Channel 1's Shift and Loop record controls work on every channel: the
// mapping `m` shows them in their place.
Mapping withChannelOneControls(Mapping m, const Mapping& first) {
    auto keep = [](TriggerBinding& shown, const TriggerBinding& own) {
        if (own.type == ActionType::Shift || own.type == ActionType::LoopRecord)
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
    std::vector<std::string> p;
    for (std::size_t at = 0; at <= spec.size();) {
        const std::size_t next = std::min(spec.find(':', at), spec.size());
        p.push_back(spec.substr(at, next - at));
        at = next + 1;
    }
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

constexpr const char* kReleasesUrl = "https://github.com/Donmanouche/MidiPad/releases/latest";

// Asks GitHub for the latest release (through curl, so MidiPad links no
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
                           "https://api.github.com/repos/Donmanouche/MidiPad/releases/latest 2>/dev/null",
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
        result.message = "Could not reach GitHub. Check the connection (and that curl is installed),\n"
                         "or look at the releases page.";
        return result;
    }
    result.ok = true;
    const std::string latest = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    result.newer = versionNumbers(latest) > versionNumbers(MIDIPAD_VERSION);
    result.message = result.newer ? "MidiPad " + latest + " is available."
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
    auto curve = [&](LoopSource source, int length, double fx, double fy, double phase, bool backAndForth) {
        auto& l = s.loops[static_cast<std::size_t>(source)];
        l.status.state = LoopStatus::State::Playing;
        l.status.length = length;
        const int lap = static_cast<int>(samples) / length;
        const int at = static_cast<int>(samples) % length;
        l.status.direction = backAndForth && lap % 2 ? -1 : 1;
        l.status.position = l.status.direction > 0 ? at : length - 1 - at;
        l.points = LiveState::Loop::kPoints;
        for (int i = 0; i < l.points; ++i) {
            const double u = 2 * 3.14159265358979 * i / l.points;
            const auto k = static_cast<std::size_t>(i);
            l.x[k] = static_cast<std::uint8_t>(128 + 100 * std::sin(fx * u + phase));
            l.y[k] = static_cast<std::uint8_t>(128 + 90 * std::sin(fy * u));
            l.on[k] = source != LoopSource::Finger1 || i % 53 < 40;  // a lifted finger now and then
        }
    };
    curve(LoopSource::LeftStick, 800, 1, 2, 0, true);
    curve(LoopSource::Finger1, 800, 2, 3, 0.5, false);
    curve(LoopSource::Triggers, 400, 1, 1, 1.2, false);

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

constexpr const char* kUsage = R"(Usage: midipad [preset.json]
       midipad --headless [options] <preset.json>   (no window, see --headless --help)

Opens the MidiPad window. With a preset file, its folder becomes the presets
folder and the preset is opened.

In the window: F11 toggles fullscreen, Ctrl+Q quits.
)";

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
    void drawOptionsOverlay();
    void drawPerformanceStatus(const LiveState& live, float height);
    void drawPerformance(const LiveState& live);
    void drawMappingOverlay(const LiveState& live);
    void toggleFullscreen();
    // Every window over the view looks like the Options panel.
    void pushWindowLook();
    void popWindowLook();
    // False when closed; dialogDismissed_ then tells whether its close button just closed it.
    bool beginDialog(const char* title, ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize);
    bool dialogDismissed_ = false;
    void endDialog();
    int dialogButtons(std::initializer_list<const char*> labels);  // the one clicked, or -1
    void applySync();  // the Link settings, to the runtime
    void drawDialogs();
    void drawNameDialog();
    void drawUnsavedDialog();
    void drawDeleteDialog();
    void drawErrorDialog();
    void drawOutputDialog();
    void drawOpenSessionDialog();
    void drawAboutDialog();

    // Sessions: every channel's loops, the preset and the tonality, in a file.
    std::vector<std::string> sessionNames() const;
    fs::path sessionPath(const std::string& name) const { return sessionsDir_ / (name + kSessionExtension); }
    void openSession(const std::string& name);
    void saveSession();
    bool saveSessionAs(const std::string& name);

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
    void browseFolder(FolderTarget target, const fs::path& start);
    static void SDLCALL onFolderChosen(void* self, const char* const* files, int filter);

    GuiOptions options_;
    ImGuiStyle baseStyle_;   // the style at the page's own size
    float pageScale_ = 0;    // how much the page is scaled to fit the window
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool quit_ = false;
    int frameCount_ = 0;

    fs::path settingsFile_;
    AppSettings settings_;
    std::unique_ptr<PresetLibrary> library_;
    std::vector<std::string> presetNames_;

    Preset preset_;
    bool hasFile_ = false;  // false: "Untitled", never saved
    bool dirty_ = false;
    std::optional<fs::file_time_type> presetTime_;
    std::chrono::steady_clock::time_point nextFileCheck_{};

    std::shared_ptr<MidiOutput> output_;
    std::string outputLabel_;
    std::vector<std::string> ports_;
    std::unique_ptr<Runtime> runtime_;
    virmidi::Status virmidiStatus_;
    int virmidiPorts_ = virmidi::kMaxPorts;  // the choices of the Options panel
    bool virmidiAtStartup_ = false;
    std::future<std::string> virmidiJob_;  // pkexec, waiting for the password
    bool virmidiRemoving_ = false;

    MappingEditor editor_;
    PerformanceView performance_;
    PerformanceView::Fonts uiFonts_;
    std::optional<std::string> overlaySpec_;  // control edited over the animated view
    bool overlayAppearing_ = false;
    int overlayPlacingFrames_ = 0;
    bool optionsOpen_ = false;  // the Options panel over the animated view
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
        std::cerr << "midipad: SDL_Init failed: " << SDL_GetError() << '\n';
        return false;
    }
    SDL_SetGamepadEventsEnabled(false);  // the runtime polls the gamepad itself
    SDL_SetJoystickEventsEnabled(false);

    const fs::path pref = prefPath();
    settingsFile_ = pref / "settings.json";
    sessionsDir_ = pref / "Sessions";
    AppSettings defaults;
    defaults.presetsDir = pref / "presets";
    settings_ = loadSettings(settingsFile_, defaults);
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
        outputDialogReason_ = "Choose where MidiPad sends its MIDI.";
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

    if (!SDL_CreateWindowAndRenderer("MidiPad", 1080, 846, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window_, &renderer_)) {
        std::cerr << "midipad: cannot open the window: " << SDL_GetError() << '\n';
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);
    // Where the window's coordinates are pixels while the screen is scaled
    // (X11 at 200 %, say), open it at the page's size in those pixels. Where
    // they already are scaled points (Wayland), the page fits as it is.
    const float pixelsPerPoint = std::max(SDL_GetWindowPixelDensity(window_), 1.0f);
    const float unscaled = std::max(SDL_GetWindowDisplayScale(window_), 1.0f) / pixelsPerPoint;
    float minScale = 1.0f;
    if (unscaled > 1.05f) {
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef NDEBUG
    io.ConfigDebugHighlightIdConflicts = false;  // a programmer's warning: debug builds only
#endif
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.TabRounding = 4;
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
                    std::cerr << "midipad: screenshot failed: " << SDL_GetError() << '\n';
                quit_ = true;
            }
            // A recording starts where a screenshot would be taken.
            if (options_.record && frameCount_ >= screenshotFrame) {
                const int index = frameCount_ - screenshotFrame;
                char name[32];
                std::snprintf(name, sizeof name, "frame_%04d.bmp", index);
                if (!saveScreenshot(*options_.record / name)) {
                    std::cerr << "midipad: recording failed: " << SDL_GetError() << '\n';
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
    ImGui::NewFrame();
    draw();
    ImGui::Render();

    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
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
        guard([this] { quit_ = true; });
}

void GuiApp::update() {
    finishVirMidiJob();
    runtime_->setKey(settings_.key);
    if (const auto source = editor_.takeClearLoopRequest())
        runtime_->clearLoop(*source);
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
    if (hasFile_ && now >= nextFileCheck_) {
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

    std::string title = "MidiPad - " + (hasFile_ ? preset_.name : std::string("Untitled")) + (dirty_ ? " *" : "");
    if (title != SDL_GetWindowTitle(window_))
        SDL_SetWindowTitle(window_, title.c_str());
}

// --- Presets ----------------------------------------------------------------

void GuiApp::refreshPresetList() { presetNames_ = library_->names(); }

void GuiApp::openPreset(const std::string& name) {
    try {
        preset_ = library_->load(name);
    } catch (const PresetError& e) {
        showError(std::string("Cannot open the preset:\n") + e.what());
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

void GuiApp::startUntitled() {
    preset_ = Preset{};
    hasFile_ = false;
    dirty_ = false;
    presetTime_.reset();
    pushToEngine();
}

bool GuiApp::saveCurrent() {
    if (!hasFile_) {
        nameDialog_ = NameDialog::SaveAs;
        nameInput_ = library_->uniqueName("New preset");
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
    dirty_ = true;
    pushToEngine();
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
            if (name.starts_with("MidiPad:") || name.find("Midi Through") != std::string::npos ||
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
void GuiApp::log(const std::string& line) { std::cout << "midipad: " << line << '\n'; }

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
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(viewport->WorkPos.x, pagePos.y),
                                                  ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, pagePos.y + 50.0f * pageScale_),
                                                  PerformanceView::Theme::kTopBar);
    ImGui::SetNextWindowPos(pagePos);
    ImGui::SetNextWindowSize(page);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(PerformanceView::Theme::kBackground));
    ImGui::Begin("MidiPad", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
        saveCurrent();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
        guard([this] { quit_ = true; });
    if (ImGui::Shortcut(ImGuiKey_F11, ImGuiInputFlags_RouteGlobal))
        toggleFullscreen();

    drawPerformance(live);
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

void GuiApp::toggleFullscreen() {
    const bool fullscreen = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(window_, !fullscreen);
}

void GuiApp::drawPerformance(const LiveState& live) {
    using Theme = PerformanceView::Theme;
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
    ImGui::PushStyleColor(ImGuiCol_CheckMark, color(Theme::kBlue));
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
        ImGui::Separator();
        if (ImGui::MenuItem("Options…", nullptr, false)) {
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
        if (ImGui::MenuItem("Fullscreen", "F11", (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0))
            toggleFullscreen();
        if (ImGui::MenuItem("About…"))
            openAboutDialog_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q"))
            guard([this] { quit_ = true; });
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, px(12));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float size = px(22);
        const ImVec2 sz = bold->CalcTextSizeA(size, FLT_MAX, 0, "MIDIPAD");
        dl->AddText(bold, size, ImVec2(p.x, p.y + (px(32) - sz.y) * 0.5f), Theme::kText, "MIDIPAD");
        ImGui::Dummy(ImVec2(sz.x + px(14), px(32)));
    }
    // Small arrow buttons drawn as triangles, for the channel and the preset.
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

    // Channel and preset, centered in the bar.
    ImGui::SameLine(0, px(14));
    {
        const float groupW = px(150) + px(16) + medium->CalcTextSizeA(px(12), FLT_MAX, 0, "PRESET").x + px(8) + px(26) * 2 + px(180) +
                             px(14) + px(70) + px(10) + px(200) + px(6) + px(44);  // then Play and Stop, the clock, TAP
        const float centered = winPos.x + (winW - groupW) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, centered), ImGui::GetCursorScreenPos().y));
    }
    // Channel: one framed control with arrows on both sides.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = px(150), h = px(32);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Theme::kControl, px(5));
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), Theme::kControlBorder, px(5));
        if (arrow("##chdown", p, px(30), h, true))
            runtime_->setChannel((live.channel + 15) % 16);
        if (arrow("##chup", ImVec2(p.x + w - px(30), p.y), px(30), h, false))
            runtime_->setChannel((live.channel + 1) % 16);
        const std::string channel = "CHANNEL " + std::to_string(live.channel + 1);
        const ImVec2 sz = bold->CalcTextSizeA(px(15), FLT_MAX, 0, channel.c_str());
        dl->AddText(bold, px(15), ImVec2(p.x + (w - sz.x) * 0.5f, p.y + (h - sz.y) * 0.5f), Theme::kText, channel.c_str());
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(w, h));
    }
    auto caption = [&](const char* s) {
        ImGui::SameLine(0, px(16));
        ImGui::PushFont(medium, 12.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(color(Theme::kLabel), "%s", s);
        ImGui::PopFont();
        ImGui::SameLine(0, px(8));
    };
    // Preset: arrows around a drop-down list, an orange dot when modified, SAVE next to it.
    caption("PRESET");
    {
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
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = px(32), arrowW = px(26), comboW = px(180);
        const float w = arrowW * 2 + comboW;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Theme::kControl, px(5));
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), Theme::kBlue, px(5));
        if (arrow("##presetprev", p, arrowW, h, true))
            openAt(index < 0 ? 0 : index - 1);
        if (arrow("##presetnext", ImVec2(p.x + w - arrowW, p.y), arrowW, h, false))
            openAt(index + 1);
        dl->AddLine(ImVec2(p.x + w - arrowW, p.y + px(5)), ImVec2(p.x + w - arrowW, p.y + h - px(5)), Theme::kControlBorder);
        ImGui::SetCursorScreenPos(ImVec2(p.x + arrowW, p.y));
        ImGui::SetNextItemWidth(comboW);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, color(0));
        ImGui::PushStyleColor(ImGuiCol_Border, color(0));
        ImGui::PushStyleColor(ImGuiCol_Button, color(0));
        ImGui::PushFont(bold, 15.0f);
        const std::string current = hasFile_ ? preset_.name : std::string("Untitled");
        const bool open = ImGui::BeginCombo("##presetcombo", current.c_str(), ImGuiComboFlags_HeightLarge);
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        if (dirty_) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddCircleFilled(ImVec2(b.x - ImGui::GetFrameHeight() - px(8), (a.y + b.y) * 0.5f), px(4), Theme::kOrange);
        }
        if (open) {
            ImGui::PushFont(medium, 14.0f);
            if (presetNames_.empty())
                ImGui::TextDisabled("No preset in this folder");
            for (const auto& name : presetNames_) {
                const bool selected = hasFile_ && name == preset_.name;
                if (ImGui::Selectable(name.c_str(), selected) && !selected)
                    guard([this, name] { openPreset(name); });
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            // Managing presets.
            ImGui::Separator();
            if (ImGui::Selectable("New\u2026"))
                guard([this] {
                    nameDialog_ = NameDialog::New;
                    nameInput_ = library_->uniqueName("New preset");
                    nameError_.clear();
                    openNameDialog_ = true;
                });
            if (ImGui::Selectable("Save as\u2026")) {
                nameDialog_ = NameDialog::SaveAs;
                nameInput_ = library_->uniqueName(hasFile_ ? preset_.name : "New preset");
                nameError_.clear();
                openNameDialog_ = true;
            }
            if (ImGui::Selectable("Delete\u2026", false, hasFile_ ? 0 : ImGuiSelectableFlags_Disabled))
                openDeleteDialog_ = true;
            ImGui::SetItemTooltip("Deletes the preset file");
            ImGui::PopFont();
            ImGui::EndCombo();
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(w, h));

        // Play and Stop every channel's loops: Play lit green while they play.
        const bool anyLoop = live.loopChannels != 0;
        const bool loopsPlaying = anyLoop && !live.loopsPaused;
        auto transport = [&](const char* id, bool play) {
            ImGui::SameLine(0, play ? px(14) : px(6));
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
        if (transport("##loopsplay", true))
            runtime_->playLoops();
        ImGui::SetItemTooltip("Plays every channel's loops again, from their start");
        if (transport("##loopsstop", false))
            runtime_->pauseLoops();
        ImGui::SetItemTooltip("Stops every channel's loops, keeping them");

        // The clock: FREE, or its tempo (the wheel turns it, a double-click
        // types it, TAP taps it) and the beats of the bar; a click elsewhere
        // on it turns it off or on again.
        {
            ImGui::SameLine(0, px(10));
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float lw = px(200), lh = px(32);
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
                const float sq = px(8), gap = px(4);
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
                ImGui::SetItemTooltip(overTempo ? "Turn the wheel to change the tempo (Shift: by 0.1), double-click to type it."
                                      : on      ? (link ? "Ableton Link: tempo and beat shared with the session. Click to run free."
                                                        : "Internal clock: MidiPad's own tempo and beat. Click to run free.")
                                                : (link ? "Free: MidiPad keeps no beat. Click to join the Ableton Link session."
                                                        : "Free: MidiPad keeps no beat. Click to start the internal clock."));
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
            ImGui::SetItemTooltip(on ? "Tap in time: the tempo follows from the second tap" : "Tap tempo, with a clock running");
        }

        if (dirty_) {
            ImGui::SameLine(0, px(8));
            ImGui::PushStyleColor(ImGuiCol_Text, color(Theme::kOrange));
            ImGui::PushStyleColor(ImGuiCol_Border, color(Theme::kOrange));
            if (ImGui::Button("SAVE"))
                saveCurrent();
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip("Ctrl+S");
        }
    }

    // PANIC, in the top right corner.
    {
        const float panicW = ImGui::CalcTextSize("PANIC").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetCursorScreenPos(ImVec2(winPos.x + winW - px(8) - panicW, winPos.y + px(9)));
        ImGui::PushStyleColor(ImGuiCol_Text, color(Theme::kRed));
        ImGui::PushStyleColor(ImGuiCol_Border, color(Theme::kRed));
        if (ImGui::Button("PANIC"))
            runtime_->panic();
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Turns off every note and CC still on");
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(winPos.x + px(8), winPos.y + barH + px(8)));

    // Right: the animated modules.
    const float statusH = px(26);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(Theme::kBackground));
    ImGui::BeginChild("modules", ImVec2(0, -statusH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    performance_.setQuantize(settings_.quantize);
    const auto clicked = performance_.draw(withChannelOneControls(preset_.forChannel(live.channel), preset_.base), live, settings_.key);
    if (auto edit = performance_.takeNoteEdit()) {  // from the note loop's editor (the last frame's)
        if (edit->remove)
            runtime_->removeLoopNotes(std::move(edit->notes));
        else if (edit->resize)
            runtime_->resizeLoopNotes(std::move(edit->notes), edit->samples);
        else
            runtime_->moveLoopNotes(std::move(edit->notes), edit->samples, edit->semitones, edit->byDegrees);
    }
    if (const auto source = performance_.takeLockRequest())  // a right-click on a LOOP panel or the piano roll
        runtime_->toggleLoopLock(*source);
    if (const auto stretch = performance_.takeStretchRequest())  // Shift + wheel over a LOOP panel
        for (int source = 0; source < kLoopSourceCount; ++source)
            if (stretch->sources & (1u << source)) {
                if (stretch->amplitude)
                    runtime_->amplifyLoop(source, stretch->steps);
                else
                    runtime_->stretchLoop(source, stretch->steps);
            }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    drawPerformanceStatus(live, statusH);

    // The SCALE screen opens the tonality menu; any other control is edited
    // in a panel over the view.
    if (clicked && *clicked == "scale") {
        ImGui::OpenPopup("##keymenu");
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
        drawOptionsOverlay();
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

void GuiApp::drawOptionsOverlay() {
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
    const bool visible = ImGui::Begin("Options###optionsoverlay", &open,
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
        ImGui::PushFont(medium, 12.0f);
        ImGui::TextColored(color(Theme::kLabel), "Where MidiPad sends its MIDI. Some programs only see the VirMIDI ports.");
        ImGui::PopFont();
        ImGui::Spacing();
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
                    ImGui::TextColored(color(Theme::kLabel), "  No MIDI port found: create VirMIDI ports below.");
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

        // Sync: Ableton Link, and how loops snap to its beat.
        beginCard("##cardsync", "SYNC", Theme::kGreen);
        {
            const LiveState live = runtime_->live();
            ImGui::PushFont(medium, 12.0f);
            ImGui::TextColored(color(Theme::kLabel),
                               settings_.clock == AppSettings::Clock::Link
                                   ? "Ableton Link shares tempo and beat with your DAW and other apps on this network."
                               : settings_.clock == AppSettings::Clock::Internal
                                   ? "Internal clock: MidiPad's own tempo and beat, no network needed."
                                   : "Free: MidiPad keeps no beat. Choose a clock to play loops in time.");
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::PushFont(medium, 13.0f);
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
            ImGui::PushFont(medium, 12.0f);
            ImGui::TextColored(color(Theme::kLabel), "With a clock, a loop takes a whole number of these, starts on the\n"
                                                     "beat and follows the tempo; its notes go on the quantize grid as\n"
                                                     "they are recorded (what you hear live is never delayed). With Link,\n"
                                                     "Play and Stop follow the session. With the internal clock, MIDI clock\n"
                                                     "out sends 24 pulses a beat, Start with Play and Stop with Stop.");
            ImGui::PopFont();
            if (changed) {
                applySync();
                persistSettings();
            }
        }
        endCard();

        // D-pad: how long a direction must hold before it plays.
        beginCard("##carddpad", "D-PAD", Theme::kRed);
        ImGui::PushFont(medium, 12.0f);
        ImGui::TextColored(color(Theme::kLabel),
                           "A new direction plays once held this long, so the one crossed while\n"
                           "pressing or releasing a diagonal stays silent. 0: at once.");
        ImGui::PopFont();
        ImGui::Spacing();
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
        beginCard("##cardhumanize", "HUMANIZE", IM_COL32(0xB0, 0x8C, 0xFF, 255));
        ImGui::PushFont(medium, 12.0f);
        ImGui::TextColored(color(Theme::kLabel),
                           "Random velocity and timing for the notes and chords of the chosen channels;\n"
                           "each note of a chord varies on its own. A mapping can opt out.");
        ImGui::PopFont();
        ImGui::Spacing();
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
        beginCard("##cardpresets", "PRESETS", Theme::kOrange);
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

// Creating the snd-virmidi ports, inside the Options panel.
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
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(Theme::kBlue));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(Theme::kBlue));
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
                    ImGui::TextColored(kHintColor, "Revert brings it back until you save the preset.");
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
        // The forms scroll when the window is too short; the buttons stay in view.
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                            ImVec2(FLT_MAX, std::max(ImGui::GetFontSize() * 10,
                                                                     viewport->WorkSize.y - ImGui::GetFontSize() * 9)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("mappingbody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);
        ImGui::PopStyleColor();
        editor_.setHumanizeChannels(settings_.humanize.channels);
        // Channel 1's Shift or Loop record control: the same on every channel, set on channel 1.
        const TriggerBinding* firstControl = live.channel != 0 ? specBinding(preset_.base, spec) : nullptr;
        const bool channelOneControl =
            firstControl && (firstControl->type == ActionType::Shift || firstControl->type == ActionType::LoopRecord);
        if (channelOneControl) {
            ImGui::TextColored(kWarningColor, "On every channel, this control is channel 1's %s.",
                               firstControl->type == ActionType::Shift ? "Shift" : "Loop record");
            ImGui::TextColored(kHintColor, "Change it on channel 1.");
            ImGui::Spacing();
        }
        ImGui::BeginDisabled(channelOneControl);
        if (spec == "notes") {  // the note loop's editor: a large piano roll, above its settings
            const float em = ImGui::GetFontSize();
            ImGui::BeginDisabled(!settings_.key);
            bool inScale = settings_.notesInScale;
            if (ImGui::Checkbox("Move notes only onto the scale", &inScale)) {
                settings_.notesInScale = inScale;
                persistSettings();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (settings_.key)
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s %s", pitchName(settings_.key->root).c_str(),
                                   std::string(settings_.key->type().label).c_str());
            else
                ImGui::TextColored(kHintColor, "(no tonality: choose one on the SCALE screen)");
            ImGui::SameLine();
            ImGui::TextColored(kHintColor, settings_.notesInScale && settings_.key ? "  up and down go by degrees of the scale"
                                                                                   : "  up and down go by semitones");
            performance_.setScaleMoves(settings_.notesInScale);
            performance_.drawNoteEditor(target, live, em * 56, em * 24);
            ImGui::TextColored(kHintColor, "Drag: move a note or the selection  \xC2\xB7  Drag on empty space: select  \xC2\xB7  "
                                           "Ctrl + click: add  \xC2\xB7  Double-click: a chord");
            ImGui::TextColored(kHintColor, "Drag the end of a selected note: its length  \xC2\xB7  Right-click or Delete: delete");
            ImGui::Spacing();
        }
        if (editor_.drawControl(spec, target, live, settings_.key)) {
            if (!own && !first)
                own = std::move(copy);  // its first change: the channel gets its own mapping
            edited();
        }
        ImGui::EndDisabled();
        if (backToBase) {
            own.reset();
            edited();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushFont(uiFonts_.bold ? uiFonts_.bold : ImGui::GetFont(), 13.0f);
        // At the bottom right, as in Options.
        const ImGuiStyle& style = ImGui::GetStyle();
        float buttonsW = style.ItemSpacing.x * 2;
        for (const char* label : {"Save preset", "Revert", "Close"})
            buttonsW += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonsW));
        ImGui::BeginDisabled(hasFile_ && !dirty_);
        if (ImGui::Button("Save preset"))
            saveCurrent();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+S");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasFile_ || !dirty_);
        if (ImGui::Button("Revert"))
            openPreset(preset_.name);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close") ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)))
            open = false;
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
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

// A modal over the view: the Options look, its bold title, text in the medium font.
bool GuiApp::beginDialog(const char* title, ImGuiWindowFlags flags) {
    using Theme = PerformanceView::Theme;
    const float em = ImGui::GetFontSize();
    pushWindowLook();
    // As the Options window: translucent, the view behind it not dimmed.
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

// The buttons closing a dialog, at the bottom right as in Options.
int GuiApp::dialogButtons(std::initializer_list<const char*> labels) {
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
        if (ImGui::Button(label, ImVec2(std::max(minW, ImGui::CalcTextSize(label).x + style.FramePadding.x * 2), 0)))
            clicked = i;
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
    preset_ = session.preset;
    hasFile_ = !PresetLibrary::nameError(preset_.name).has_value();
    dirty_ = true;
    presetTime_.reset();
    if (hasFile_ && library_->contains(preset_.name)) {
        try {
            dirty_ = presetToJson(library_->load(preset_.name)) != presetToJson(preset_);
        } catch (const std::exception&) {
        }
        presetTime_ = modificationTime(library_->pathFor(preset_.name));
        settings_.lastPreset = preset_.name;
    }
    settings_.key = session.key;
    // Its tempo: for the internal clock, or for a Link session MidiPad is
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
    sessionName_ = name;
    log("Session \"" + name + "\" opened.");
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

bool GuiApp::saveSessionAs(const std::string& name) {
    auto loops = runtime_->sessionLoops();
    if (!loops) {
        showError("Cannot save the session:\nthe loops did not answer.");
        return false;
    }
    Session session;
    session.preset = preset_;
    session.key = settings_.key;
    const LiveState now = runtime_->live();
    session.channel = now.channel;
    session.tempo = now.clock != ClockMode::Free ? now.tempo : settings_.tempo;
    session.loops = std::move(*loops);
    try {
        midipad::saveSession(session, sessionPath(name));
    } catch (const std::exception& e) {
        showError(std::string("Cannot save the session:\n") + e.what());
        return false;
    }
    sessionName_ = name;
    log("Session \"" + name + "\" saved (" + std::to_string(session.loops.size()) + " loops).");
    return true;
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
        ImGui::TextUnformatted("No session saved yet: \u2630 \u203a Save as\u2026 saves the loops playing now.");
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
        ImGui::TextColored(kHintColor, "Opening one replaces every loop, the preset and the tonality.\nRight-click a session to delete it.");
    }
    const int button = dialogButtons({"Open", "Cancel"});
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
    constexpr const char* kTitle = "About MidiPad";
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
    ImGui::TextUnformatted("MidiPad");
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
    drawNameDialog();
    drawUnsavedDialog();
    drawDeleteDialog();
    drawErrorDialog();
    drawOutputDialog();
    drawOpenSessionDialog();
    drawAboutDialog();
}

void GuiApp::drawNameDialog() {
    const bool session = nameDialog_ == NameDialog::SessionSaveAs;
    const char* title = nameDialog_ == NameDialog::New ? "New preset" : session ? "Save session as" : "Save as";
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
    ImGui::TextUnformatted(session ? "Session name" : "Preset name");
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
            else if (saveSessionAs(nameInput_))
                ImGui::CloseCurrentPopup();
        } else if (library_->contains(nameInput_)) {
            nameError_ = "A preset already has this name.";
        } else if (nameDialog_ == NameDialog::New) {
            preset_ = Preset{};
            preset_.name = nameInput_;
            hasFile_ = true;
            dirty_ = true;
            if (saveCurrent()) {
                settings_.lastPreset = preset_.name;
                persistSettings();
                refreshPresetList();
                pushToEngine();
            }
            ImGui::CloseCurrentPopup();
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
    ImGui::Text("The preset \"%s\" was changed.", hasFile_ ? preset_.name.c_str() : "Untitled");
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
    constexpr const char* kTitle = "Delete preset";
    if (openDeleteDialog_) {
        ImGui::OpenPopup(kTitle);
        openDeleteDialog_ = false;
    }
    if (!beginDialog(kTitle))
        return;
    ImGui::Text("Delete the preset \"%s\"?", preset_.name.c_str());
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
    ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
    ImGui::TextWrapped("Some programs do not see the virtual port \"%s\": choose a VirMIDI port, "
                       "then in your program the input shown on the right.",
                       settings_.virtualPortName.c_str());
    ImGui::PopStyleColor();

    ImGui::BeginChild("ports", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9), ImGuiChildFlags_Borders);
    if (ports_.empty())
        ImGui::TextDisabled("No MIDI port found: create VirMIDI ports above.");
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
        std::cerr << "midipad : " << options.error << "\n\n" << kUsage;
        return 2;
    }
    GuiApp app(std::move(options));
    return app.run();
}
