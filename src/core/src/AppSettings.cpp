#include "midipad/AppSettings.hpp"

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
    if (s.virtualPortName.empty())
        s.virtualPortName = "MidiPad";
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

    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    if (!out)
        throw std::runtime_error(file.string() + ": cannot write");
    out << j.dump(4) << '\n';
}

}  // namespace midipad
