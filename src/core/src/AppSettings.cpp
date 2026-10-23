#include "midipad/AppSettings.hpp"
#include "midipad/Controls.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <vector>
#include <stdexcept>

namespace midipad {

using Json = nlohmann::ordered_json;

namespace {

template <typename T>
void read(const Json& j, const char* key, T& target) {
    if (!j.contains(key))
        return;
    try {
        target = j[key].get<T>();
    } catch (const Json::exception&) {
    }
}

void readPath(const Json& j, const char* key, std::filesystem::path& target) {
    std::string s = target.string();
    read(j, key, s);
    target = s;
}

}  // namespace

AppSettings loadSettings(const std::filesystem::path& file, AppSettings s) {
    std::ifstream in(file);
    if (!in)
        return s;
    Json j;
    try {
        j = Json::parse(in);
    } catch (const Json::exception&) {
        return s;
    }
    if (!j.is_object())
        return s;

    readPath(j, "presets_dir", s.presetsDir);
    read(j, "last_preset", s.lastPreset);
    std::string output;
    read(j, "output", output);
    if (output == "virtual")
        s.output = AppSettings::Output::VirtualPort;
    else if (output == "port")
        s.output = AppSettings::Output::ExistingPort;
    read(j, "virtual_port_name", s.virtualPortName);
    read(j, "existing_port_name", s.existingPortName);
    read(j, "output_chosen", s.outputChosen);
    if (s.output == AppSettings::Output::ExistingPort && !s.existingPortName.empty())
        s.outputChosen = true;  // settings written before this field existed
    int keyRoot = s.key ? s.key->root : -1;
    std::string keyScale = s.key ? std::string(s.key->type().id) : std::string();
    read(j, "key_root", keyRoot);
    read(j, "key_scale", keyScale);
    const auto scale = findScale(keyScale);
    if (keyRoot >= 0 && keyRoot < 12 && scale)
        s.key = Key{keyRoot, *scale};
    else
        s.key.reset();
    read(j, "dpad_settle_press_ms", s.dpadSettlePressMs);
    read(j, "dpad_settle_release_ms", s.dpadSettleReleaseMs);
    std::vector<int> humanizeChannels;
    read(j, "humanize_channels", humanizeChannels);
    if (j.contains("humanize_channels"))
        s.humanize.channels = 0;
    for (int c : humanizeChannels)
        if (c >= 1 && c <= 16)
            s.humanize.channels = static_cast<std::uint16_t>(s.humanize.channels | (1u << (c - 1)));
    Humanize& h = s.humanize;
    read(j, "humanize_velocity_min", h.velocityMinPercent);
    read(j, "humanize_velocity_max", h.velocityMaxPercent);
    read(j, "humanize_delay_min_ms", h.delayMinMs);
    read(j, "humanize_delay_max_ms", h.delayMaxMs);
    h.velocityMinPercent = std::clamp(h.velocityMinPercent, 0, 100);
    h.velocityMaxPercent = std::clamp(h.velocityMaxPercent, h.velocityMinPercent, 100);
    h.delayMinMs = std::clamp(h.delayMinMs, 0, Humanize::kMaxDelayMs);
    h.delayMaxMs = std::clamp(h.delayMaxMs, h.delayMinMs, Humanize::kMaxDelayMs);
    static constexpr std::pair<const char*, AppSettings::Clock> kClocks[] = {
        {"free", AppSettings::Clock::Free}, {"internal", AppSettings::Clock::Internal}, {"link", AppSettings::Clock::Link}};
    bool linkEnabled = false;  // before the internal clock
    read(j, "link_enabled", linkEnabled);
    if (linkEnabled)
        s.clock = AppSettings::Clock::Link;
    std::string clock, lastClock;
    read(j, "clock", clock);
    read(j, "clock_last", lastClock);
    for (const auto& [name, value] : kClocks) {
        if (clock == name)
            s.clock = value;
        if (lastClock == name && value != AppSettings::Clock::Free)
            s.lastClock = value;
    }
    read(j, "tempo", s.tempo);
    read(j, "midi_clock_out", s.midiClockOut);
    s.tempo = std::clamp(s.tempo, 20.0, 999.0);
    std::string snap;
    read(j, "loop_snap", snap);
    static constexpr std::pair<const char*, AppSettings::LoopSnap> kSnaps[] = {
        {"free", AppSettings::LoopSnap::Free},     {"beat", AppSettings::LoopSnap::Beat},
        {"bar", AppSettings::LoopSnap::Bar},       {"2bars", AppSettings::LoopSnap::TwoBars},
        {"4bars", AppSettings::LoopSnap::FourBars}};
    for (const auto& [name, value] : kSnaps)
        if (snap == name)
            s.loopSnap = value;
    read(j, "beats_per_bar", s.beatsPerBar);
    s.beatsPerBar = std::clamp(s.beatsPerBar, 1, 16);
    read(j, "start_stop_sync", s.startStopSync);
    std::string theme;
    read(j, "theme", theme);
    s.theme = theme == "phosphor_blue" ? AppSettings::Theme::PhosphorBlue
            : theme == "phosphor_green" ? AppSettings::Theme::PhosphorGreen : AppSettings::Theme::Default;
    std::string frameRate;
    read(j, "frame_rate", frameRate);
    if (frameRate == "screen" || frameRate == "60" || frameRate == "30")
        s.frameRate = frameRate == "screen" ? 0 : std::stoi(frameRate);
    read(j, "background_30fps", s.slowInBackground);
    read(j, "standard_resolution", s.standardResolution);
    auto navButton = [&](const char* key, int& out) {
        std::string name;
        read(j, key, name);
        if (const auto b = parseButton(name))
            out = static_cast<int>(*b);
        else if (name == "none")
            out = -1;
    };
    navButton("nav_select", s.navSelect);
    navButton("nav_menu", s.navMenu);
    navButton("nav_back", s.navBack);
    read(j, "migrated_from_midipad", s.migratedFromMidipad);
    std::string quantize;
    read(j, "quantize_notes", quantize);
    static constexpr std::pair<const char*, int> kGrids[] = {{"off", 0}, {"1/8", 2}, {"1/16", 4}, {"1/32", 8}};
    for (const auto& [name, value] : kGrids)
        if (quantize == name)
            s.quantize = value;
    if (s.virtualPortName.empty())
        s.virtualPortName = "Arcadance";
    return s;
}

void saveSettings(const AppSettings& s, const std::filesystem::path& file) {
    Json j;
    j["presets_dir"] = s.presetsDir.string();
    j["last_preset"] = s.lastPreset;
    j["output"] = s.output == AppSettings::Output::VirtualPort ? "virtual" : "port";
    j["virtual_port_name"] = s.virtualPortName;
    j["existing_port_name"] = s.existingPortName;
    j["output_chosen"] = s.outputChosen;
    j["key_root"] = s.key ? s.key->root : -1;
    j["key_scale"] = s.key ? std::string(s.key->type().id) : std::string();
    j["dpad_settle_press_ms"] = s.dpadSettlePressMs;
    j["dpad_settle_release_ms"] = s.dpadSettleReleaseMs;
    Json channels = Json::array();
    for (int c = 0; c < 16; ++c)
        if (s.humanize.appliesTo(c))
            channels.push_back(c + 1);
    j["humanize_channels"] = channels;
    j["humanize_velocity_min"] = s.humanize.velocityMinPercent;
    j["humanize_velocity_max"] = s.humanize.velocityMaxPercent;
    j["humanize_delay_min_ms"] = s.humanize.delayMinMs;
    j["humanize_delay_max_ms"] = s.humanize.delayMaxMs;
    static constexpr const char* kClockNames[] = {"free", "internal", "link"};
    j["clock"] = kClockNames[static_cast<int>(s.clock)];
    j["clock_last"] = kClockNames[static_cast<int>(s.lastClock)];
    j["tempo"] = s.tempo;
    j["midi_clock_out"] = s.midiClockOut;
    static constexpr const char* kSnapNames[] = {"free", "beat", "bar", "2bars", "4bars"};
    j["loop_snap"] = kSnapNames[static_cast<int>(s.loopSnap)];
    j["beats_per_bar"] = s.beatsPerBar;
    j["start_stop_sync"] = s.startStopSync;
    static constexpr const char* kThemeNames[] = {"default", "phosphor_blue", "phosphor_green"};
    j["theme"] = kThemeNames[static_cast<int>(s.theme)];
    j["frame_rate"] = s.frameRate == 60 ? "60" : s.frameRate == 30 ? "30" : "screen";
    j["background_30fps"] = s.slowInBackground;
    j["standard_resolution"] = s.standardResolution;
    auto navName = [](int b) { return b < 0 ? std::string("none") : std::string(toString(static_cast<Button>(b))); };
    j["nav_select"] = navName(s.navSelect);
    j["nav_menu"] = navName(s.navMenu);
    j["nav_back"] = navName(s.navBack);
    if (s.migratedFromMidipad)
        j["migrated_from_midipad"] = true;
    j["quantize_notes"] = s.quantize == 2 ? "1/8" : s.quantize == 4 ? "1/16" : s.quantize == 8 ? "1/32" : "off";

    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    if (!out)
        throw std::runtime_error(file.string() + ": cannot write");
    out << j.dump(4) << '\n';
}

}  // namespace midipad
