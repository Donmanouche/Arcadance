// The MidiPad window: preset management, MIDI output, mapping editor, log.
//
// The main thread only draws and edits; the Runtime's input thread plays.
// Every edit is pushed to the engine at once, so changes are heard live, and
// written to disk only on "Enregistrer".

#include "gui/GuiApp.hpp"

#include "MidiOutput.hpp"
#include "Runtime.hpp"
#include "gui/MappingEditor.hpp"

#include "midipad/AppSettings.hpp"
#include "midipad/Labels.hpp"
#include "midipad/LegacyImport.hpp"
#include "midipad/PresetIO.hpp"
#include "midipad/PresetLibrary.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cctype>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

extern const unsigned char kRobotoMedium[];
extern const std::size_t kRobotoMedium_size;

namespace fs = std::filesystem;
using namespace midipad;

namespace {

constexpr float kFontSize = 16.0f;
constexpr std::size_t kLogLines = 500;
constexpr ImVec4 kHintColor{0.65f, 0.65f, 0.7f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImVec4 kOkColor{0.45f, 0.85f, 0.55f, 1.0f};

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
    std::optional<MappingEditor::Tab> tab;
    std::optional<std::string> select;
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
        } else if (a == "--tab" && i + 1 < args.size()) {
            const std::string t = args[++i];
            if (t == "buttons")
                o.tab = MappingEditor::Tab::Buttons;
            else if (t == "dpad")
                o.tab = MappingEditor::Tab::Dpad;
            else if (t == "axes")
                o.tab = MappingEditor::Tab::Axes;
            else if (t == "touchpad")
                o.tab = MappingEditor::Tab::Touchpad;
            else
                o.error = "--tab attend buttons, dpad, axes ou touchpad";
        } else if (a == "--select" && i + 1 < args.size()) {
            o.select = args[++i];
        } else if (!a.empty() && a[0] == '-') {
            o.error = "option inconnue " + a;
        } else {
            o.preset = a;
        }
    }
    return o;
}

// How Bitwig names a snd-virmidi port: it only lists kernel MIDI devices, as
// "<device name>/<subdevice>", so "VirMIDI 3-0" reads "Virtual Raw MIDI/..." and
// "VirMIDI 3-1" "Virtual Raw MIDI 2/...". Empty for other ports.
std::string bitwigName(const std::string& port) {
    const auto at = port.find(":VirMIDI ");
    if (at == std::string::npos)
        return {};
    const auto dash = port.find('-', at);
    if (dash == std::string::npos || dash + 1 >= port.size() || !std::isdigit(static_cast<unsigned char>(port[dash + 1])))
        return {};
    const int device = port[dash + 1] - '0';
    return device == 0 ? "Virtual Raw MIDI" : "Virtual Raw MIDI " + std::to_string(device + 1);
}

constexpr const char* kUsage = R"(Usage : midipad [preset.json]
        midipad --headless [options] <preset.json>   (sans fenêtre, voir --headless --help)

Ouvre l'éditeur MidiPad. Avec un fichier de preset, son dossier devient le
dossier des presets et le preset est ouvert.
)";

class GuiApp {
public:
    explicit GuiApp(GuiOptions options) : options_(std::move(options)) {}
    int run();

private:
    enum class NameDialog { None, New, SaveAs };
    enum class FolderTarget { None, Presets, Import };

    bool init();
    void shutdown();
    void frame();
    void update();
    void handleEvent(const SDL_Event& e);
    bool saveScreenshot(const fs::path& file);

    // Drawing.
    void draw();
    void drawPresetBar();
    void drawOutputBar(const LiveState& live);
    void drawMappingArea(const LiveState& live);
    void drawLog();
    void drawStatusBar(const LiveState& live);
    void drawDialogs();
    void drawNameDialog();
    void drawUnsavedDialog();
    void drawTrashDialog();
    void drawImportDialog();
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
    void runImport();

    // MIDI output.
    void refreshPorts();
    bool useOutput(AppSettings::Output kind, const std::string& port);
    std::shared_ptr<MidiOutput> openOutput(AppSettings::Output kind, const std::string& port, std::string& error);

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
    bool hasFile_ = false;  // false: "Sans titre", never saved
    bool dirty_ = false;
    std::optional<fs::file_time_type> presetTime_;
    std::chrono::steady_clock::time_point nextFileCheck_{};

    std::shared_ptr<MidiOutput> output_;
    std::string outputLabel_;
    std::vector<std::string> ports_;
    std::unique_ptr<Runtime> runtime_;

    MappingEditor editor_;
    int editChannel_ = -1;  // -1: base mapping
    bool followChannel_ = true;
    int lastLiveChannel_ = -1;
    std::optional<int> pickedChannel_;

    std::deque<std::string> log_;
    bool scrollLog_ = false;
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
    bool openImportDialog_ = false;
    std::string importDir_;
    std::vector<std::string> importReport_;
    std::string errorMessage_;
    bool openErrorDialog_ = false;
    bool openOutputDialog_ = false;
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
        std::cerr << "midipad : SDL_Init a échoué : " << SDL_GetError() << '\n';
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

    // As in the Python version: reconnect to the last port used, otherwise ask
    // which one to use. The virtual port stands in until the user answers.
    refreshPorts();
    std::string error;
    if (settings_.outputChosen) {
        output_ = openOutput(settings_.output, settings_.existingPortName, error);
        if (output_) {
            log("Sortie MIDI : " + outputLabel_);
        } else {
            outputDialogReason_ = "Le dernier port utilisé, « " + settings_.existingPortName + " », est introuvable.";
            openOutputDialog_ = true;
        }
    } else {
        outputDialogReason_ = "Choisissez où MidiPad envoie son MIDI.";
        openOutputDialog_ = true;
    }
    if (!output_) {
        output_ = openOutput(AppSettings::Output::VirtualPort, {}, error);
        if (!output_)
            log("Aucune sortie MIDI : " + error);
    }

    Runtime::Options runtimeOptions;
    runtimeOptions.reportMidi = settings_.monitorMidi;
    runtimeOptions.reportButtons = settings_.showButtons;
    runtime_ = std::make_unique<Runtime>(output_, std::make_shared<const Preset>(preset_), runtimeOptions,
                                         [this](const std::string& line) {
                                             std::lock_guard lock(pendingLogMutex_);
                                             pendingLog_.push_back(line);
                                         });

    if (!SDL_CreateWindowAndRenderer("MidiPad", 1280, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window_, &renderer_)) {
        std::cerr << "midipad : impossible d'ouvrir la fenêtre : " << SDL_GetError() << '\n';
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
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoMedium), static_cast<int>(kRobotoMedium_size),
                                   kFontSize, &font);

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
    // The gamepad plays music; it must not drive the interface.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);

    if (options_.tab)
        editor_.showTab(*options_.tab);
    if (options_.select && !editor_.select(*options_.select))
        log("--select : contrôle inconnu " + *options_.select);
    log("Presets : " + settings_.presetsDir.string());
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
            frame();
            if (options_.screenshot && ++frameCount_ == 4) {
                if (!saveScreenshot(*options_.screenshot))
                    std::cerr << "midipad : capture impossible : " << SDL_GetError() << '\n';
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
    while (auto r = runtime_->popReport())
        log(describe(*r));
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
        else if (target == FolderTarget::Import)
            importDir_ = *folder;
    }

    // Follow edits made to the preset file by another program.
    const auto now = std::chrono::steady_clock::now();
    if (hasFile_ && now >= nextFileCheck_) {
        nextFileCheck_ = now + std::chrono::milliseconds(500);
        const auto time = modificationTime(library_->pathFor(preset_.name));
        if (time && time != presetTime_) {
            presetTime_ = time;
            if (dirty_) {
                log("Le fichier du preset a changé sur le disque ; vos modifications non enregistrées sont conservées.");
            } else {
                try {
                    preset_ = library_->load(preset_.name);
                    pushToEngine();
                    log("Preset rechargé (modifié sur le disque).");
                } catch (const PresetError& e) {
                    log(std::string("Preset modifié sur le disque mais invalide : ") + e.what());
                }
            }
        }
    }

    std::string title = "MidiPad - " + (hasFile_ ? preset_.name : std::string("Sans titre")) + (dirty_ ? " *" : "");
    if (title != SDL_GetWindowTitle(window_))
        SDL_SetWindowTitle(window_, title.c_str());
}

// --- Presets ----------------------------------------------------------------

void GuiApp::refreshPresetList() { presetNames_ = library_->names(); }

void GuiApp::openPreset(const std::string& name) {
    try {
        preset_ = library_->load(name);
    } catch (const PresetError& e) {
        showError(std::string("Impossible d'ouvrir le preset :\n") + e.what());
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
        nameInput_ = library_->uniqueName("Nouveau preset");
        nameError_.clear();
        openNameDialog_ = true;
        return false;
    }
    try {
        library_->save(preset_);
    } catch (const std::exception& e) {
        showError(std::string("Enregistrement impossible :\n") + e.what());
        return false;
    }
    dirty_ = false;
    presetTime_ = modificationTime(library_->pathFor(preset_.name));
    log("Preset « " + preset_.name + " » enregistré.");
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
    log("Presets : " + dir.string());
}

void GuiApp::runImport() {
    importReport_.clear();
    const fs::path source = importDir_;
    if (!fs::is_directory(source)) {
        importReport_.push_back("Dossier introuvable : " + source.string());
        return;
    }
    settings_.legacyImportDir = source;
    persistSettings();

    const auto imported = importLegacyDirectory(source);
    for (const auto& error : imported.errors)
        importReport_.push_back("ERREUR " + error);
    int count = 0;
    for (const auto& result : imported.presets) {
        if (library_->contains(result.preset.name)) {
            importReport_.push_back("Ignoré : « " + result.preset.name + " » existe déjà.");
            continue;
        }
        try {
            library_->save(result.preset);
            importReport_.push_back("Importé : « " + result.preset.name + " »");
            ++count;
        } catch (const std::exception& e) {
            importReport_.push_back("ERREUR « " + result.preset.name + " » : " + e.what());
        }
        for (const auto& warning : result.warnings)
            importReport_.push_back("    ! " + warning);
    }
    if (imported.presets.empty() && imported.errors.empty())
        importReport_.push_back("Aucun preset trouvé dans ce dossier.");
    refreshPresetList();
    log(std::to_string(count) + " preset(s) importé(s) depuis " + source.string());
    if (!hasFile_ && !dirty_ && count > 0)
        openFirstOrUntitled();
}

// --- MIDI output ------------------------------------------------------------

void GuiApp::refreshPorts() {
    ports_.clear();
    try {
        MidiOutput probe(MidiOutput::Api::Default);
        for (auto& name : probe.portNames()) {
            // Skip our own ports and the system ones nobody plays into, as the
            // Python version did.
            if (name.starts_with("MidiPad:") || name.find("Midi Through") != std::string::npos ||
                name.find("rtpmidid") != std::string::npos || name.find("Announce") != std::string::npos ||
                (name.starts_with("Client-") && name.find("pipewire") == std::string::npos))
                continue;
            ports_.push_back(std::move(name));
        }
    } catch (const std::exception& e) {
        log(std::string("Liste des ports MIDI indisponible : ") + e.what());
    }
}

std::shared_ptr<MidiOutput> GuiApp::openOutput(AppSettings::Output kind, const std::string& port, std::string& error) {
    try {
        auto out = std::make_shared<MidiOutput>(MidiOutput::Api::Default);
        if (kind == AppSettings::Output::ExistingPort) {
            outputLabel_ = out->connect(port);
        } else {
            out->openVirtual(settings_.virtualPortName);
            outputLabel_ = "Port virtuel « " + settings_.virtualPortName + " »";
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
        showError("Sortie MIDI indisponible :\n" + error);
        return false;
    }
    output_ = out;
    runtime_->setOutput(std::move(out));
    settings_.output = kind;
    settings_.outputChosen = true;
    if (kind == AppSettings::Output::ExistingPort)
        settings_.existingPortName = MidiOutput::stablePortName(port);
    persistSettings();
    log("Sortie MIDI : " + outputLabel_);
    return true;
}

// --- Helpers ----------------------------------------------------------------

void GuiApp::log(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > kLogLines)
        log_.pop_front();
    scrollLog_ = true;
}

void GuiApp::showError(const std::string& message) {
    errorMessage_ = message;
    openErrorDialog_ = true;
    log(message);
}

void GuiApp::persistSettings() {
    try {
        saveSettings(settings_, settingsFile_);
    } catch (const std::exception& e) {
        log(std::string("Réglages non enregistrés : ") + e.what());
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
    const LiveState live = runtime_->live();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("MidiPad", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
        saveCurrent();

    drawPresetBar();
    drawOutputBar(live);
    ImGui::Separator();

    const float logHeight = ImGui::GetTextLineHeightWithSpacing() * 8;
    const float statusHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("mapping", ImVec2(0, -(logHeight + statusHeight)));
    drawMappingArea(live);
    ImGui::EndChild();
    drawLog();
    drawStatusBar(live);

    drawDialogs();
    ImGui::End();
}

void GuiApp::drawPresetBar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    const std::string current = (hasFile_ ? preset_.name : std::string("Sans titre")) + (dirty_ ? " *" : "");
    if (ImGui::BeginCombo("##preset", current.c_str())) {
        if (presetNames_.empty())
            ImGui::TextDisabled("Aucun preset dans ce dossier");
        for (const auto& name : presetNames_) {
            if (ImGui::Selectable(name.c_str(), hasFile_ && name == preset_.name) && !(hasFile_ && name == preset_.name))
                guard([this, name] { openPreset(name); });
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Nouveau"))
        guard([this] {
            nameDialog_ = NameDialog::New;
            nameInput_ = library_->uniqueName("Nouveau preset");
            nameError_.clear();
            openNameDialog_ = true;
        });
    ImGui::SameLine();
    ImGui::BeginDisabled(hasFile_ && !dirty_);
    if (ImGui::Button("Enregistrer"))
        saveCurrent();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+S");
    ImGui::SameLine();
    if (ImGui::Button("Enregistrer sous…")) {
        nameDialog_ = NameDialog::SaveAs;
        nameInput_ = library_->uniqueName(hasFile_ ? preset_.name : "Nouveau preset");
        nameError_.clear();
        openNameDialog_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasFile_ || !dirty_);
    if (ImGui::Button("Annuler les modifications"))
        openPreset(preset_.name);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasFile_);
    if (ImGui::Button("Corbeille"))
        openTrashDialog_ = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Déplace le preset dans le sous-dossier « corbeille »");

    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    if (ImGui::Button("Importer…")) {
        importDir_ = settings_.legacyImportDir.string();
        importReport_.clear();
        openImportDialog_ = true;
    }
    ImGui::SetItemTooltip("Importer les presets de la version Python (dossier configs)");
    ImGui::SameLine();
    if (ImGui::Button("Dossier…"))
        browseFolder(FolderTarget::Presets, settings_.presetsDir);
    ImGui::SetItemTooltip("Dossier des presets :\n%s", settings_.presetsDir.string().c_str());
}

void GuiApp::drawOutputBar(const LiveState& live) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sortie MIDI");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    if (ImGui::BeginCombo("##output", outputLabel_.c_str())) {
        const std::string virtualLabel = "Port virtuel « " + settings_.virtualPortName + " »";
        if (ImGui::Selectable(virtualLabel.c_str(), settings_.output == AppSettings::Output::VirtualPort))
            useOutput(AppSettings::Output::VirtualPort, {});
        ImGui::Separator();
        if (ports_.empty())
            ImGui::TextDisabled("Aucun port MIDI");
        for (const auto& port : ports_)
            if (ImGui::Selectable(port.c_str(), outputLabel_ == port))
                useOutput(AppSettings::Output::ExistingPort, port);
            else if (const auto name = bitwigName(port); !name.empty())
                ImGui::SetItemTooltip("Dans Bitwig : %s/…", name.c_str());
        ImGui::EndCombo();
    }
    if (const auto name = bitwigName(outputLabel_); !name.empty())
        ImGui::SetItemTooltip("Dans Bitwig, cette sortie s'appelle %s/…", name.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Actualiser"))
        refreshPorts();

    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Canal actif");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##chdown", ImGuiDir_Left))
        runtime_->setChannel((live.channel + 15) % 16);
    ImGui::SameLine();
    ImGui::Text("%2d", live.channel + 1);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##chup", ImGuiDir_Right))
        runtime_->setChannel((live.channel + 1) % 16);

    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    if (ImGui::Button("Tout couper"))
        runtime_->panic();
    ImGui::SetItemTooltip("Coupe toutes les notes et CC encore actifs (notes collées, toggles)");

    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::AlignTextToFramePadding();
    if (live.connected)
        ImGui::TextColored(kOkColor, "Manette : %s%s", live.gamepadName.c_str(),
                           live.hasTouchpad ? "" : " (sans touchpad)");
    else
        ImGui::TextColored(kHintColor, "Aucune manette : branchez-la, elle sera détectée");
}

void GuiApp::drawMappingArea(const LiveState& live) {
    // Following shows what plays on the active channel: its own mapping if it
    // has one, the base otherwise. A channel picked in the list stays shown
    // once the engine has switched to it.
    if (followChannel_ && live.channel != lastLiveChannel_ && (!pickedChannel_ || live.channel == *pickedChannel_)) {
        const bool own = preset_.channels[static_cast<std::size_t>(live.channel)].has_value();
        editChannel_ = own || pickedChannel_ ? live.channel : -1;
        pickedChannel_.reset();
        lastLiveChannel_ = live.channel;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mapping");
    ImGui::SameLine();
    auto channelLabel = [this](int c) {
        if (c < 0)
            return std::string("Base (tous les canaux)");
        return "Canal " + std::to_string(c + 1) +
               (preset_.channels[static_cast<std::size_t>(c)] ? "  (spécifique)" : "  (utilise la base)");
    };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo("##editchannel", channelLabel(editChannel_).c_str())) {
        if (ImGui::Selectable(channelLabel(-1).c_str(), editChannel_ < 0)) {
            editChannel_ = -1;
            followChannel_ = false;
        }
        for (int c = 0; c < kChannelCount; ++c) {
            if (ImGui::Selectable(channelLabel(c).c_str(), editChannel_ == c)) {
                editChannel_ = c;
                if (c != live.channel) {
                    pickedChannel_ = c;
                    runtime_->setChannel(c);  // hear what you edit
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Suivre le canal actif", &followChannel_);
    ImGui::SetItemTooltip("Affiche le mapping du canal actif, y compris quand il change depuis la manette");

    Mapping* target = &preset_.base;
    bool readOnly = false;
    if (editChannel_ >= 0) {
        auto& slot = preset_.channels[static_cast<std::size_t>(editChannel_)];
        ImGui::SameLine();
        if (slot) {
            target = &*slot;
            if (ImGui::Button("Revenir à la base pour ce canal")) {
                slot.reset();
                edited();
                target = &preset_.base;
                readOnly = true;
            }
            ImGui::SetItemTooltip("Supprime le mapping spécifique de ce canal");
        } else {
            readOnly = true;
            if (ImGui::Button("Créer un mapping spécifique (copie de la base)")) {
                slot = preset_.base;
                edited();
                target = &*slot;
                readOnly = false;
            }
        }
    }
    if (readOnly) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextUnformatted("Ce canal joue le mapping de base, affiché ci-dessous en lecture seule.");
        ImGui::PopStyleColor();
    }

    if (editor_.draw(*target, live, readOnly))
        edited();
}

void GuiApp::drawLog() {
    ImGui::BeginChild("log", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    if (ImGui::Checkbox("Moniteur MIDI", &settings_.monitorMidi)) {
        runtime_->setReportMidi(settings_.monitorMidi);
        persistSettings();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Boutons", &settings_.showButtons)) {
        runtime_->setReportButtons(settings_.showButtons);
        persistSettings();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Effacer"))
        log_.clear();
    ImGui::Separator();
    ImGui::BeginChild("lines");
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log_.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            ImGui::TextUnformatted(log_[static_cast<std::size_t>(i)].c_str());
    if (scrollLog_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight() * 3)
        ImGui::SetScrollHereY(1.0f);
    scrollLog_ = false;
    ImGui::EndChild();
    ImGui::EndChild();
}

void GuiApp::drawStatusBar(const LiveState& live) {
    const RuntimeStats stats = runtime_->stats();
    std::string frozen;
    for (int a = 0; a < kAxisCount; ++a)
        if (live.frozenAxes & (1u << a))
            frozen += (frozen.empty() ? "" : ", ") + label(static_cast<Axis>(a));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kHintColor, "Freeze : %s    |    Traitement : %.0f µs en moyenne, %.0f µs max    |    "
                                   "Erreurs MIDI : %llu",
                       frozen.empty() ? "aucun" : frozen.c_str(), stats.averageWorkUs, stats.maxWorkUs,
                       static_cast<unsigned long long>(stats.sendErrors));
}

// --- Dialogs ----------------------------------------------------------------

void GuiApp::drawDialogs() {
    drawNameDialog();
    drawUnsavedDialog();
    drawTrashDialog();
    drawImportDialog();
    drawErrorDialog();
    drawOutputDialog();
}

void GuiApp::drawNameDialog() {
    const char* title = nameDialog_ == NameDialog::New ? "Nouveau preset" : "Enregistrer sous";
    if (openNameDialog_) {
        ImGui::OpenPopup(title);
        openNameDialog_ = false;
    }
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("Nom du preset");
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
            nameError_ = "Un preset porte déjà ce nom.";
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
    if (ImGui::Button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        afterSave_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GuiApp::drawUnsavedDialog() {
    constexpr const char* kTitle = "Modifications non enregistrées";
    if (openUnsavedDialog_) {
        ImGui::OpenPopup(kTitle);
        openUnsavedDialog_ = false;
    }
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("Le preset « %s » a été modifié.", hasFile_ ? preset_.name.c_str() : "Sans titre");
    ImGui::Spacing();
    if (ImGui::Button("Enregistrer")) {
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
    if (ImGui::Button("Ne pas enregistrer")) {
        ImGui::CloseCurrentPopup();
        dirty_ = false;
        if (pendingAction_)
            std::exchange(pendingAction_, nullptr)();
    }
    ImGui::SameLine();
    if (ImGui::Button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pendingAction_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GuiApp::drawTrashDialog() {
    constexpr const char* kTitle = "Mettre à la corbeille";
    if (openTrashDialog_) {
        ImGui::OpenPopup(kTitle);
        openTrashDialog_ = false;
    }
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("Déplacer « %s » dans le dossier « corbeille » ?", preset_.name.c_str());
    ImGui::TextColored(kHintColor, "Le fichier n'est pas supprimé : il reste récupérable dans ce dossier.");
    if (ImGui::Button("Déplacer")) {
        try {
            const fs::path moved = library_->moveToTrash(preset_.name);
            log("Preset déplacé vers " + moved.string());
            refreshPresetList();
            openFirstOrUntitled();
        } catch (const std::exception& e) {
            showError(std::string("Déplacement impossible :\n") + e.what());
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void GuiApp::drawImportDialog() {
    constexpr const char* kTitle = "Importer les presets de la version Python";
    if (openImportDialog_) {
        ImGui::OpenPopup(kTitle);
        openImportDialog_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr))
        return;
    ImGui::TextWrapped("Choisissez le dossier « configs » de la version Python. Chaque preset et ses variantes "
                       "par canal (<nom>-<canal>.json) deviennent un preset unique dans votre dossier de presets. "
                       "Les presets déjà présents ne sont pas écrasés.");
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7);
    ImGui::InputText("##importdir", &importDir_);
    ImGui::SameLine();
    if (ImGui::Button("Parcourir…"))
        browseFolder(FolderTarget::Import, importDir_);
    if (ImGui::Button("Importer"))
        runImport();
    ImGui::SameLine();
    if (ImGui::Button("Fermer") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    if (!importReport_.empty()) {
        ImGui::Separator();
        ImGui::BeginChild("report", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), ImGuiChildFlags_Borders);
        for (const auto& line : importReport_)
            ImGui::TextUnformatted(line.c_str());
        ImGui::EndChild();
    }
    ImGui::EndPopup();
}

void GuiApp::drawErrorDialog() {
    constexpr const char* kTitle = "Erreur";
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
    constexpr const char* kTitle = "Sortie MIDI";
    if (openOutputDialog_) {
        refreshPorts();
        ImGui::OpenPopup(kTitle);
        openOutputDialog_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr))
        return;
    ImGui::TextWrapped("%s", outputDialogReason_.c_str());
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
    ImGui::TextWrapped("Bitwig sous Linux ne voit pas le port virtuel « %s » : choisissez un port VirMIDI, "
                       "puis l'entrée indiquée à droite dans Bitwig.",
                       settings_.virtualPortName.c_str());
    ImGui::PopStyleColor();

    ImGui::BeginChild("ports", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9), ImGuiChildFlags_Borders);
    if (ports_.empty())
        ImGui::TextDisabled("Aucun port MIDI trouvé. Pour Bitwig, chargez le module snd-virmidi (voir le README).");
    for (const auto& port : ports_) {
        const std::string name = bitwigName(port);
        const std::string text = name.empty() ? port : port + "      -> Bitwig : " + name + "/…";
        if (ImGui::Selectable(text.c_str()) && useOutput(AppSettings::Output::ExistingPort, port))
            ImGui::CloseCurrentPopup();
    }
    ImGui::EndChild();

    if (ImGui::Button("Actualiser"))
        refreshPorts();
    ImGui::SameLine();
    const std::string virtualLabel = "Port virtuel « " + settings_.virtualPortName + " »";
    if (ImGui::Button(virtualLabel.c_str()) && useOutput(AppSettings::Output::VirtualPort, {}))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Plus tard") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::SetItemTooltip("Garde le port virtuel pour cette fois ; la question reviendra au prochain démarrage");
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
