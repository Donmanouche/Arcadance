#include "midipad/LegacyImport.hpp"

#include "midipad/Notes.hpp"
#include "midipad/PresetIO.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <regex>

namespace midipad {

using Json = nlohmann::ordered_json;

namespace {

constexpr Button kLegacyButtons[] = {
    Button::South,        Button::East,         Button::North, Button::West,  Button::LeftShoulder,
    Button::RightShoulder, Button::LeftTrigger, Button::RightTrigger, Button::Back, Button::Start,
    Button::Guide,        Button::LeftStick,    Button::RightStick,
};

constexpr Axis kLegacyAxes[] = {
    Axis::LeftX, Axis::LeftY, Axis::LeftTrigger, Axis::RightX, Axis::RightY, Axis::RightTrigger,
};

std::optional<int> legacyIndex(const std::string& key) {
    try {
        std::size_t used = 0;
        int value = std::stoi(key, &used);
        if (used == key.size() && value >= 0)
            return value;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::optional<Axis> legacyAxis(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kLegacyAxes)))
        return std::nullopt;
    return kLegacyAxes[index];
}

class Converter {
public:
    explicit Converter(std::vector<std::string>& warnings) : warnings_(warnings) {}

    Mapping convert(const Json& file, const std::string& where) {
        Mapping m;
        if (!file.is_object() || !file.contains("mappings") || !file["mappings"].is_object())
            return m;
        const Json& mappings = file["mappings"];

        std::uint8_t inverted = 0;
        if (file.contains("axis_invert") && file["axis_invert"].is_object()) {
            for (const auto& [key, value] : file["axis_invert"].items()) {
                auto index = legacyIndex(key);
                auto axis = index ? legacyAxis(*index) : std::nullopt;
                if (axis && value.is_boolean() && value.get<bool>())
                    inverted = static_cast<std::uint8_t>(inverted | (1u << static_cast<unsigned>(*axis)));
            }
        }

        forEach(mappings, "button", where, [&](int index, const Json& value, const std::string& at) {
            if (index >= static_cast<int>(std::size(kLegacyButtons)))
                return warn(at, "no matching button on a DualSense, dropped");
            if (auto b = trigger(value, at))
                m.buttons[static_cast<std::size_t>(kLegacyButtons[index])] = *b;
        });

        forEach(mappings, "axis", where, [&](int index, const Json& value, const std::string& at) {
            auto axis = legacyAxis(index);
            if (!axis)
                return warn(at, "no matching axis on a DualSense, dropped");
            if (auto a = axisBinding(value, at)) {
                a->invert = inverted & (1u << static_cast<unsigned>(*axis));
                m.axes[static_cast<std::size_t>(*axis)] = *a;
            }
        });

        forEach(mappings, "hat", where, [&](int index, const Json& value, const std::string& at) {
            if (index != 0)
                return warn(at, "only hat 0 (the D-pad) exists on a gamepad, dropped");
            if (!value.is_object())
                return warn(at, "unexpected format, dropped");
            for (const auto& [key, direction] : value.items()) {
                auto dir = parseHatDir(key);
                if (!dir) {
                    warn(at + "." + key, "unknown direction, dropped");
                    continue;
                }
                if (auto b = trigger(direction, at + "." + key))
                    m.dpad[static_cast<std::size_t>(*dir)] = *b;
            }
        });

        forEach(mappings, "touchpad", where, [&](int index, const Json& value, const std::string& at) {
            if (index < 1 || index > kFingerCount)
                return warn(at, "fingers are numbered 1 and 2, dropped");
            if (!value.is_object())
                return warn(at, "unexpected format, dropped");
            FingerBinding& f = m.fingers[static_cast<std::size_t>(index - 1)];
            if (value.contains("pad") && value["pad"].is_object() && !value["pad"].empty()) {
                f.mode = TouchMode::Zones;
                if (value.contains("x") || value.contains("y"))
                    warn(at, "has both zones and X/Y mappings; kept the zones");
                for (const auto& [key, zone] : value["pad"].items()) {
                    auto z = parseZone(key);
                    if (!z) {
                        warn(at + ".pad." + key, "unknown zone, dropped");
                        continue;
                    }
                    if (auto b = trigger(zone, at + ".pad." + key))
                        f.zones[static_cast<std::size_t>(*z)] = *b;
                }
            } else if (value.contains("x") || value.contains("y")) {
                f.mode = TouchMode::Axis;
                if (value.contains("x"))
                    if (auto a = axisBinding(value["x"], at + ".x"))
                        f.x = *a;
                if (value.contains("y"))
                    if (auto a = axisBinding(value["y"], at + ".y"))
                        f.y = *a;
            }
        });

        for (const auto& [key, value] : mappings.items())
            if (key != "button" && key != "axis" && key != "hat" && key != "touchpad")
                warn(where + "." + key, "unknown control type, dropped");
        return m;
    }

private:
    template <typename F>
    void forEach(const Json& mappings, const char* type, const std::string& where, F&& f) {
        if (!mappings.contains(type) || !mappings[type].is_object())
            return;
        for (const auto& [key, value] : mappings[type].items()) {
            const std::string at = where + "." + type + "." + key;
            if (auto index = legacyIndex(key))
                f(*index, value, at);
            else
                warn(at, "not a control number, dropped");
        }
    }

    void warn(const std::string& where, const std::string& message) { warnings_.push_back(where + ": " + message); }

    static std::uint8_t u7(const Json& j, const char* key, std::uint8_t fallback) {
        if (!j.contains(key) || !j[key].is_number_integer())
            return fallback;
        return static_cast<std::uint8_t>(std::clamp(j[key].get<int>(), 0, 127));
    }

    std::optional<TriggerBinding> trigger(const Json& j, const std::string& at) {
        if (!j.is_object()) {
            warn(at, "unexpected format, dropped");
            return std::nullopt;
        }
        TriggerBinding b;
        b.mode = j.value("mode", std::string()) == "toggle" ? Mode::Toggle : Mode::Momentary;
        const std::string type = j.value("midi_type", std::string());
        const Json id = j.value("midi_id", Json());

        if (type == "note") {
            b.type = ActionType::Note;
            auto note = noteFromJson(id);
            if (!note) {
                warn(at, "invalid note " + id.dump() + ", dropped");
                return std::nullopt;
            }
            if (id.is_string())
                warn(at, "note name " + id.dump() + " read as " + std::to_string(*note) + " (" + noteName(*note) +
                             "); the Python version misread names containing E or B");
            b.number = *note;
            b.velocity = u7(j, "velocity", 100);
        } else if (type == "cc") {
            b.type = ActionType::ControlChange;
            if (!id.is_number_integer()) {
                warn(at, "invalid CC number " + id.dump() + ", dropped");
                return std::nullopt;
            }
            b.number = static_cast<std::uint8_t>(std::clamp(id.get<int>(), 0, 127));
            b.onValue = u7(j, "cc_value", 127);
        } else if (type == "chord") {
            b.type = ActionType::Chord;
            b.velocity = u7(j, "velocity", 100);
            if (id.is_string()) {
                auto chord = parseChord(id.get<std::string>());
                if (!chord) {
                    warn(at, "unknown chord " + id.dump() + ", dropped");
                    return std::nullopt;
                }
                b.chord = *chord;
                b.chordName = id.get<std::string>();
            } else if (id.is_array() && !id.empty() && id.size() <= b.chord.notes.size()) {
                for (const Json& n : id) {
                    auto note = noteFromJson(n);
                    if (!note) {
                        warn(at, "invalid chord notes " + id.dump() + ", dropped");
                        return std::nullopt;
                    }
                    b.chord.notes[b.chord.count++] = *note;
                }
            } else {
                warn(at, "invalid chord " + id.dump() + ", dropped");
                return std::nullopt;
            }
        } else if (type == "transport") {
            b.type = ActionType::Transport;
            const std::string command = id.is_string() ? id.get<std::string>() : "start";
            if (command == "stop")
                b.transport = TransportCommand::Stop;
            else if (command == "continue")
                b.transport = TransportCommand::Continue;
            else
                b.transport = TransportCommand::Start;
        } else if (type == "channel") {
            b.type = ActionType::ChannelStep;
            const int step = id.is_number_integer() ? id.get<int>() : 0;
            if (step == 0 || step < -15 || step > 15) {
                warn(at, "invalid channel step " + id.dump() + ", dropped");
                return std::nullopt;
            }
            b.channelStep = static_cast<std::int8_t>(step);
        } else if (type == "freeze") {
            b.type = ActionType::Freeze;
            const Json axes = j.value("freeze_axis", Json(0));
            for (const Json& index : axes.is_array() ? axes : Json::array({axes})) {
                auto axis = index.is_number_integer() ? legacyAxis(index.get<int>()) : std::nullopt;
                if (axis)
                    b.freezeAxes = static_cast<std::uint8_t>(b.freezeAxes | (1u << static_cast<unsigned>(*axis)));
                else
                    warn(at, "unknown axis " + index.dump() + " in freeze_axis, ignored");
            }
            if (b.freezeAxes == 0) {
                warn(at, "freeze without a valid axis, dropped");
                return std::nullopt;
            }
        } else {
            warn(at, "unknown midi_type \"" + type + "\", dropped");
            return std::nullopt;
        }
        return b;
    }

    std::optional<AxisBinding> axisBinding(const Json& j, const std::string& at) {
        if (!j.is_object() || j.value("midi_type", std::string()) != "cc" || !j.contains("midi_id") ||
            !j["midi_id"].is_number_integer()) {
            warn(at, "only CC is supported on axes, dropped");
            return std::nullopt;
        }
        AxisBinding a;
        a.enabled = true;
        a.cc = static_cast<std::uint8_t>(std::clamp(j["midi_id"].get<int>(), 0, 127));
        a.min = u7(j, "cc_min", 0);
        a.max = u7(j, "cc_max", 127);
        return a;
    }

    std::vector<std::string>& warnings_;
};

}  // namespace

LegacyImportResult importLegacyPreset(std::string name, const Json& baseFile,
                                      const std::map<int, Json>& channelFiles) {
    LegacyImportResult result;
    Converter converter(result.warnings);
    result.preset.name = std::move(name);
    result.preset.base = converter.convert(baseFile, "base");
    for (const auto& [channel, file] : channelFiles) {
        if (channel < 1 || channel > kChannelCount) {
            result.warnings.push_back("channel " + std::to_string(channel) + ": not a MIDI channel, dropped");
            continue;
        }
        result.preset.channels[static_cast<std::size_t>(channel - 1)] =
            converter.convert(file, "channel " + std::to_string(channel));
    }
    return result;
}

LegacyDirectoryImport importLegacyDirectory(const std::filesystem::path& configsDir) {
    LegacyDirectoryImport out;
    struct Group {
        std::optional<Json> base;
        std::map<int, Json> channels;
    };
    std::map<std::string, Group> groups;
    static const std::regex kChannelSuffix(R"((.+)-([0-9]{1,2}))");

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(configsDir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        Json content;
        try {
            std::ifstream in(entry.path());
            content = Json::parse(in);
        } catch (const std::exception& e) {
            out.errors.push_back(entry.path().filename().string() + ": " + e.what());
            continue;
        }
        const std::string stem = entry.path().stem().string();
        std::smatch match;
        if (std::regex_match(stem, match, kChannelSuffix)) {
            int channel = std::stoi(match[2].str());
            if (channel >= 1 && channel <= kChannelCount) {
                groups[match[1].str()].channels[channel] = std::move(content);
                continue;
            }
        }
        groups[stem].base = std::move(content);
    }
    if (ec)
        out.errors.push_back(configsDir.string() + ": " + ec.message());

    for (auto& [name, group] : groups) {
        auto result = importLegacyPreset(name, group.base.value_or(Json::object()), group.channels);
        if (!group.base)
            result.warnings.push_back("no " + name + ".json found: the base mapping is empty");
        out.presets.push_back(std::move(result));
    }
    return out;
}

}  // namespace midipad
