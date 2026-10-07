#include "midipad/AppSettings.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
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
    std::string view = s.performanceView ? "performance" : "editor";
    read(j, "view", view);
    s.performanceView = view == "performance";
    read(j, "dpad_settle_press_ms", s.dpadSettlePressMs);
    read(j, "dpad_settle_release_ms", s.dpadSettleReleaseMs);
    read(j, "monitor_midi", s.monitorMidi);
    read(j, "show_buttons", s.showButtons);
    readPath(j, "legacy_import_dir", s.legacyImportDir);
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
    j["view"] = s.performanceView ? "performance" : "editor";
    j["dpad_settle_press_ms"] = s.dpadSettlePressMs;
    j["dpad_settle_release_ms"] = s.dpadSettleReleaseMs;
    j["monitor_midi"] = s.monitorMidi;
    j["show_buttons"] = s.showButtons;
    j["legacy_import_dir"] = s.legacyImportDir.string();

    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    if (!out)
        throw std::runtime_error(file.string() + ": cannot write");
    out << j.dump(4) << '\n';
}

}  // namespace midipad
