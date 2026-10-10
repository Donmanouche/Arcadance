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
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
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
constexpr ImVec4 kHintColor{0.65f, 0.65f, 0.7f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};

fs::path prefPath() {
    char* path = SDL_GetPrefPath("MidiPad", "MidiPad");
    fs::path result = path ? fs::path(path) : fs::current_path();
    SDL_free(path);
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

// A moving controller state, so screenshots show the animated view at work.
LiveState demoLive(double t) {
    LiveState s;
    s.connected = true;
    s.hasTouchpad = true;
    s.gamepadName = "DualSense (demo)";
    auto axis = [](double v) { return static_cast<std::int16_t>(std::clamp(v, -1.0, 1.0) * 32767); };
    s.axes[static_cast<std::size_t>(Axis::LeftX)] = axis(0.7 * std::cos(t * 1.3));
    s.axes[static_cast<std::size_t>(Axis::LeftY)] = axis(-0.6 * std::sin(t * 1.3));
    s.axes[static_cast<std::size_t>(Axis::RightX)] = axis(0.35);
    s.axes[static_cast<std::size_t>(Axis::RightY)] = axis(-0.8);
    s.axes[static_cast<std::size_t>(Axis::LeftTrigger)] = axis(0.65);
    s.axes[static_cast<std::size_t>(Axis::RightTrigger)] = axis(0.12);
    for (Button b : {Button::South, Button::DpadUp})
        s.buttons |= 1u << static_cast<int>(b);
    for (int slot : {Engine::slotOf(Button::South), Engine::slotOf(HatDir::Up), Engine::slotOf(Button::LeftShoulder),
                     Engine::slotOf(Button::LeftTrigger)})
        s.sounding |= std::uint64_t{1} << slot;
    s.fingers[0] = {true, static_cast<float>(0.3 + 0.15 * std::sin(t * 2.0)), 0.45f};
    s.frozenAxes = static_cast<std::uint8_t>((1u << static_cast<int>(Axis::RightX)) | (1u << static_cast<int>(Axis::RightY)));
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
    enum class NameDialog { None, New, SaveAs };
    enum class FolderTarget { None, Presets };

    bool init();
    void shutdown();
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
    void drawDialogs();
    void drawNameDialog();
    void drawUnsavedDialog();
    void drawTrashDialog();
    void drawErrorDialog();
    void drawOutputDialog();

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

    std::mutex pendingLogMutex_;
    std::vector<std::string> pendingLog_;  // from the input thread

    // Dialogs.
    NameDialog nameDialog_ = NameDialog::None;
    bool openNameDialog_ = false;
    std::string nameInput_, nameError_;
    std::function<void()> afterSave_;
    std::function<void()> pendingAction_;
    bool openUnsavedDialog_ = false;
    bool openTrashDialog_ = false;
    std::string errorMessage_;
    bool openErrorDialog_ = false;
    bool openOutputDialog_ = false;
    bool outputDialogOpen_ = false;
    bool closeOutputDialog_ = false;  // VirMIDI ports created from it: done
    std::string outputDialogReason_;

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

    if (!SDL_CreateWindowAndRenderer("MidiPad", 1080, 846, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window_, &renderer_)) {
        std::cerr << "midipad: cannot open the window: " << SDL_GetError() << '\n';
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);
    SDL_SetWindowMinimumSize(window_, 900, 560);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the layout is fixed
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.TabRounding = 4;
    const float scale = std::max(SDL_GetWindowDisplayScale(window_), 1.0f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

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
            if (options_.screenshot && ++frameCount_ == screenshotFrame) {
                if (!saveScreenshot(*options_.screenshot))
                    std::cerr << "midipad: screenshot failed: " << SDL_GetError() << '\n';
                quit_ = true;
            }
        }
    }
    shutdown();
    return ok ? 0 : 1;
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
    ImGui::NewFrame();
    draw();
    ImGui::Render();

    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer_, 20, 22, 28, 255);
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

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
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
    drawDialogs();
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
        if (ImGui::MenuItem("Fullscreen", "F11", (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0))
            toggleFullscreen();
        ImGui::Separator();
        if (ImGui::MenuItem("Presets folder…"))
            browseFolder(FolderTarget::Presets, settings_.presetsDir);
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
    ImGui::SameLine(0, px(4));
    ImGui::PushStyleColor(ImGuiCol_Text, color(Theme::kRed));
    ImGui::PushStyleColor(ImGuiCol_Border, color(Theme::kRed));
    if (ImGui::Button("PANIC"))
        runtime_->panic();
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Turns off every note and CC still on");

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

    // Channel and preset, centered in the bar (never over PANIC).
    ImGui::SameLine(0, px(14));
    {
        const float groupW = px(150) + px(16) + medium->CalcTextSizeA(px(12), FLT_MAX, 0, "PRESET").x + px(8) + px(26) * 2 + px(180);
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
            if (ImGui::Selectable("Move to trash\u2026", false, hasFile_ ? 0 : ImGuiSelectableFlags_Disabled))
                openTrashDialog_ = true;
            ImGui::SetItemTooltip("Moves the preset to the \"trash\" subfolder of the presets folder");
            ImGui::PopFont();
            ImGui::EndCombo();
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(w, h));
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
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(winPos.x + px(8), winPos.y + barH + px(8)));

    // Right: the animated modules.
    const float statusH = px(26);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(Theme::kBackground));
    ImGui::BeginChild("modules", ImVec2(0, -statusH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto clicked = performance_.draw(preset_.forChannel(live.channel), live, settings_.key);
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
    // Centered, every frame: its height follows the window and the content.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (optionsAppearing_) {
        ImGui::SetNextWindowFocus();
        optionsAppearing_ = false;
    }
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::SetNextWindowSize(ImVec2(px(560), 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color(Theme::kPanelHeader));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, color(Theme::kTopBar));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, color(Theme::kTopBar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14), px(12)));
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
        auto section = [&](const char* title) {
            ImGui::Spacing();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const ImVec2 sz = bold->CalcTextSizeA(px(12), FLT_MAX, 0, title);
            dl->AddText(bold, px(12), p, Theme::kDim, title);
            dl->AddLine(ImVec2(p.x + sz.x + px(8), p.y + sz.y * 0.5f), ImVec2(p.x + w, p.y + sz.y * 0.5f), IM_COL32(0x40, 0x44, 0x4B, 255));
            ImGui::Dummy(ImVec2(w, sz.y + px(4)));
        };

        // MIDI interface: every output, the current one selected.
        section("MIDI INTERFACE");
        ImGui::PushFont(medium, 12.0f);
        ImGui::TextColored(color(Theme::kLabel), "Where MidiPad sends its MIDI. Some programs only see the VirMIDI ports.");
        ImGui::PopFont();
        ImGui::Spacing();
        struct Choice {
            std::string port;  // empty: the virtual port
            std::string name, hint;
            bool selected;
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
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const Choice& c = choices[i];
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = px(36);
            const bool clicked = ImGui::InvisibleButton(("##out" + std::to_string(i)).c_str(), ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            const ImU32 fill = c.selected ? (Theme::kBlue & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 55)
                                          : hovered ? IM_COL32(0x38, 0x3C, 0x43, 235) : IM_COL32(0x2B, 0x2E, 0x33, 230);
            dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), fill, px(6));
            dl->AddRect(a, ImVec2(a.x + w, a.y + h), c.selected ? Theme::kBlue : IM_COL32(0x45, 0x4A, 0x52, 255), px(6), px(1.3f));
            const ImVec2 dot(a.x + px(20), a.y + h * 0.5f);
            dl->AddCircle(dot, px(7), c.selected ? Theme::kBlue : Theme::kLabel, 0, px(2));
            if (c.selected)
                dl->AddCircleFilled(dot, px(3.5f), Theme::kBlue);
            ImFont* nameFont = c.selected ? bold : medium;
            const ImVec2 nsz = nameFont->CalcTextSizeA(px(14), FLT_MAX, 0, c.name.c_str());
            dl->AddText(nameFont, px(14), ImVec2(a.x + px(38), a.y + (h - nsz.y) * 0.5f), Theme::kText, c.name.c_str());
            if (!c.hint.empty()) {
                const ImVec2 hsz = medium->CalcTextSizeA(px(12), FLT_MAX, 0, c.hint.c_str());
                dl->AddText(medium, px(12), ImVec2(a.x + w - px(14) - hsz.x, a.y + (h - hsz.y) * 0.5f),
                            c.selected ? Theme::kText : Theme::kLabel, c.hint.c_str());
            }
            if (clicked && !c.selected)
                useOutput(c.port.empty() ? AppSettings::Output::VirtualPort : AppSettings::Output::ExistingPort, c.port);
            ImGui::Dummy(ImVec2(0, px(2)));
        }
        if (ports_.empty()) {
            ImGui::PushFont(medium, 12.0f);
            ImGui::TextColored(color(Theme::kLabel), "No MIDI port found: create VirMIDI ports below.");
            ImGui::PopFont();
        }
        ImGui::PushFont(bold, 13.0f);
        if (ImGui::Button("Refresh")) {
            refreshPorts();
            virmidiStatus_ = virmidi::status();
        }
        ImGui::PopFont();
        if (virmidi::supported())
            drawVirMidiSection(false);

        // D-pad: how long a direction must hold before it plays.
        ImGui::Spacing();
        section("D-PAD");
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

        // Humanization: which channels, and how much.
        ImGui::Spacing();
        section("HUMANIZE");
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

        // Presets: folder and import.
        ImGui::Spacing();
        section("PRESETS");
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
        ImGui::Spacing();
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
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
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
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, 0), ImVec2(ImGui::GetFontSize() * 40, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(Theme::kPanelHeader));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImGui::ColorConvertU32ToFloat4(Theme::kBorder));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImGui::ColorConvertU32ToFloat4(Theme::kBorder));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(0xFF4A4642));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(0xFF57524D));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(Theme::kBlue));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(Theme::kBlue));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.9f, ImGui::GetFontSize() * 0.7f));
    bool open = true;
    const std::string title = MappingEditor::controlTitle(spec) + "###mappingoverlay";
    if (ImGui::Begin(title.c_str(), &open,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar)) {
        // Edit what plays now: the active channel's own mapping, or the base.
        auto& own = preset_.channels[static_cast<std::size_t>(live.channel)];
        Mapping& target = own ? *own : preset_.base;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::kDim), "%s",
                           own ? ("Editing the mapping of channel " + std::to_string(live.channel + 1)).c_str()
                               : "Editing the base mapping (all channels)");
        ImGui::Spacing();
        // The forms scroll when the window is too short; the buttons stay in view.
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                            ImVec2(FLT_MAX, std::max(ImGui::GetFontSize() * 10,
                                                                     viewport->WorkSize.y - ImGui::GetFontSize() * 9)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("mappingbody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);
        ImGui::PopStyleColor();
        editor_.setHumanizeChannels(settings_.humanize.channels);
        if (editor_.drawControl(spec, target, live, settings_.key))
            edited();
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
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
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(7);
    if (!open)
        overlaySpec_.reset();
}

void GuiApp::drawDialogs() {
    drawNameDialog();
    drawUnsavedDialog();
    drawTrashDialog();
    drawErrorDialog();
    drawOutputDialog();
}

void GuiApp::drawNameDialog() {
    const char* title = nameDialog_ == NameDialog::New ? "New preset" : "Save as";
    if (openNameDialog_) {
        ImGui::OpenPopup(title);
        openNameDialog_ = false;
    }
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("Preset name");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
    const bool enter = ImGui::InputText("##name", &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (!nameError_.empty())
        ImGui::TextColored(kErrorColor, "%s", nameError_.c_str());

    if (ImGui::Button("OK") || enter) {
        if (auto error = PresetLibrary::nameError(nameInput_)) {
            nameError_ = *error;
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
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        afterSave_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GuiApp::drawUnsavedDialog() {
    constexpr const char* kTitle = "Unsaved changes";
    if (openUnsavedDialog_) {
        ImGui::OpenPopup(kTitle);
        openUnsavedDialog_ = false;
    }
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("The preset \"%s\" was changed.", hasFile_ ? preset_.name.c_str() : "Untitled");
    ImGui::Spacing();
    if (ImGui::Button("Save")) {
        ImGui::CloseCurrentPopup();
        if (hasFile_) {
            if (saveCurrent() && pendingAction_)
                std::exchange(pendingAction_, nullptr)();
        } else {
            afterSave_ = std::exchange(pendingAction_, nullptr);
            saveCurrent();  // asks for a name, then runs afterSave_
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't save")) {
        ImGui::CloseCurrentPopup();
        dirty_ = false;
        if (pendingAction_)
            std::exchange(pendingAction_, nullptr)();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pendingAction_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GuiApp::drawTrashDialog() {
    constexpr const char* kTitle = "Move to trash";
    if (openTrashDialog_) {
        ImGui::OpenPopup(kTitle);
        openTrashDialog_ = false;
    }
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("Move \"%s\" to the \"trash\" folder?", preset_.name.c_str());
    ImGui::TextColored(kHintColor, "The file is not deleted: it can be recovered from that folder.");
    if (ImGui::Button("Move")) {
        try {
            const fs::path moved = library_->moveToTrash(preset_.name);
            log("Preset moved to " + moved.string());
            refreshPresetList();
            openFirstOrUntitled();
        } catch (const std::exception& e) {
            showError(std::string("Cannot move:\n") + e.what());
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void GuiApp::drawErrorDialog() {
    constexpr const char* kTitle = "Error";
    if (openErrorDialog_) {
        ImGui::OpenPopup(kTitle);
        openErrorDialog_ = false;
    }
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted(errorMessage_.c_str());
    if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void GuiApp::drawOutputDialog() {
    constexpr const char* kTitle = "MIDI output";
    if (openOutputDialog_) {
        refreshPorts();
        ImGui::OpenPopup(kTitle);
        openOutputDialog_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr)) {
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

    if (ImGui::Button("Refresh"))
        refreshPorts();
    ImGui::SameLine();
    const std::string virtualLabel = "Virtual port \"" + settings_.virtualPortName + "\"";
    if (ImGui::Button(virtualLabel.c_str()) && useOutput(AppSettings::Output::VirtualPort, {}))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Later") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::SetItemTooltip("Keeps the virtual port for now; the question comes back at the next start");
    ImGui::EndPopup();
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
