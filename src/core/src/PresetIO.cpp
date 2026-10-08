#include "midipad/PresetIO.hpp"

#include "midipad/Notes.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

namespace midipad {

using Json = nlohmann::ordered_json;

namespace {

constexpr std::string_view kFormatName = "midipad-preset";

[[noreturn]] void fail(const std::string& where, const std::string& message) {
    throw PresetError(where + ": " + message);
}

std::uint8_t readU7(const Json& object, const char* key, std::uint8_t fallback, const std::string& where) {
    if (!object.contains(key))
        return fallback;
    const Json& v = object[key];
    if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > 127)
        fail(where + "." + key, "expected an integer from 0 to 127");
    return v.get<std::uint8_t>();
}

std::string readString(const Json& object, const char* key, const char* fallback, const std::string& where) {
    if (!object.contains(key)) {
        if (fallback)
            return fallback;
        fail(where, std::string("missing \"") + key + "\"");
    }
    if (!object[key].is_string())
        fail(where + "." + key, "expected a string");
    return object[key].get<std::string>();
}

std::uint8_t readAxisMask(const Json& value, const std::string& where) {
    std::uint8_t mask = 0;
    auto add = [&](const Json& name) {
        auto axis = name.is_string() ? parseAxis(name.get<std::string>()) : std::nullopt;
        if (!axis)
            fail(where, "unknown axis " + name.dump());
        mask = static_cast<std::uint8_t>(mask | (1u << static_cast<unsigned>(*axis)));
    };
    if (value.is_array()) {
        for (const Json& name : value)
            add(name);
    } else {
        add(value);
    }
    if (mask == 0)
        fail(where, "no axis to freeze");
    return mask;
}

TriggerBinding triggerFromJson(const Json& j, const std::string& where) {
    if (!j.is_object())
        fail(where, "expected an object");
    TriggerBinding b;

    const std::string mode = readString(j, "mode", "momentary", where);
    if (mode == "momentary")
        b.mode = Mode::Momentary;
    else if (mode == "toggle")
        b.mode = Mode::Toggle;
    else
        fail(where + ".mode", "expected \"momentary\" or \"toggle\"");

    const std::string type = readString(j, "type", nullptr, where);
    if (type == "note") {
        b.type = ActionType::Note;
        auto note = j.contains("note") ? noteFromJson(j["note"]) : std::nullopt;
        if (!note)
            fail(where + ".note", "expected a MIDI note number or a name like \"C4\"");
        b.number = *note;
        b.velocity = readU7(j, "velocity", 100, where);
    } else if (type == "cc") {
        b.type = ActionType::ControlChange;
        if (!j.contains("cc"))
            fail(where, "missing \"cc\"");
        b.number = readU7(j, "cc", 0, where);
        b.onValue = readU7(j, "value", 127, where);
        b.offValue = readU7(j, "off_value", 0, where);
    } else if (type == "chord") {
        b.type = ActionType::Chord;
        b.velocity = readU7(j, "velocity", 100, where);
        if (j.contains("chord")) {
            b.chordName = readString(j, "chord", nullptr, where);
            const Json octave = j.value("octave", Json(kDefaultChordOctave));
            if (!octave.is_number_integer() || octave.get<int>() < kMinChordOctave || octave.get<int>() > kMaxChordOctave)
                fail(where + ".octave", "expected an integer from " + std::to_string(kMinChordOctave) + " to " +
                                            std::to_string(kMaxChordOctave));
            b.chordOctave = octave.get<std::int8_t>();
            auto chord = parseChord(b.chordName, b.chordOctave);
            if (!chord && parseChord(b.chordName))
                fail(where + ".octave", "\"" + b.chordName + "\" goes beyond the MIDI notes in this octave");
            if (!chord)
                fail(where + ".chord", "unknown chord \"" + b.chordName + "\"");
            b.chord = *chord;
        } else if (j.contains("notes") && j["notes"].is_array()) {
            for (const Json& n : j["notes"]) {
                auto note = noteFromJson(n);
                if (!note || b.chord.count == b.chord.notes.size())
                    fail(where + ".notes", "expected 1 to 8 MIDI notes");
                b.chord.notes[b.chord.count++] = *note;
            }
            if (b.chord.count == 0)
                fail(where + ".notes", "expected 1 to 8 MIDI notes");
        } else {
            fail(where, "missing \"chord\" (a name) or \"notes\" (a list)");
        }
    } else if (type == "transport") {
        b.type = ActionType::Transport;
        const std::string command = readString(j, "command", "start", where);
        if (command == "start")
            b.transport = TransportCommand::Start;
        else if (command == "stop")
            b.transport = TransportCommand::Stop;
        else if (command == "continue")
            b.transport = TransportCommand::Continue;
        else
            fail(where + ".command", "expected \"start\", \"stop\" or \"continue\"");
    } else if (type == "channel") {
        b.type = ActionType::ChannelStep;
        const Json step = j.value("step", Json(1));
        if (!step.is_number_integer() || step.get<int>() == 0 || std::abs(step.get<int>()) > 15)
            fail(where + ".step", "expected a non-zero integer from -15 to 15");
        b.channelStep = step.get<std::int8_t>();
    } else if (type == "freeze") {
        b.type = ActionType::Freeze;
        if (!j.contains("axes"))
            fail(where, "missing \"axes\"");
        b.freezeAxes = readAxisMask(j["axes"], where + ".axes");
    } else {
        fail(where + ".type", "unknown type \"" + type + "\"");
    }
    return b;
}

Json triggerToJson(const TriggerBinding& b) {
    Json j;
    switch (b.type) {
    case ActionType::None:
        break;
    case ActionType::Note:
        j["type"] = "note";
        j["note"] = b.number;
        if (b.velocity != 100)
            j["velocity"] = b.velocity;
        break;
    case ActionType::ControlChange:
        j["type"] = "cc";
        j["cc"] = b.number;
        if (b.onValue != 127)
            j["value"] = b.onValue;
        if (b.offValue != 0)
            j["off_value"] = b.offValue;
        break;
    case ActionType::Chord:
        j["type"] = "chord";
        if (!b.chordName.empty()) {
            j["chord"] = b.chordName;
            if (b.chordOctave != kDefaultChordOctave)
                j["octave"] = b.chordOctave;
        } else {
            Json notes = Json::array();
            for (auto n : b.chord.view())
                notes.push_back(n);
            j["notes"] = notes;
        }
        if (b.velocity != 100)
            j["velocity"] = b.velocity;
        break;
    case ActionType::Transport:
        j["type"] = "transport";
        j["command"] = b.transport == TransportCommand::Start  ? "start"
                       : b.transport == TransportCommand::Stop ? "stop"
                                                               : "continue";
        return j;
    case ActionType::ChannelStep:
        j["type"] = "channel";
        j["step"] = b.channelStep;
        return j;
    case ActionType::Freeze: {
        j["type"] = "freeze";
        Json axes = Json::array();
        for (int a = 0; a < kAxisCount; ++a)
            if (b.freezeAxes & (1u << a))
                axes.push_back(toString(static_cast<Axis>(a)));
        j["axes"] = axes;
        return j;
    }
    }
    j["mode"] = b.mode == Mode::Toggle ? "toggle" : "momentary";
    return j;
}

AxisBinding axisFromJson(const Json& j, const std::string& where) {
    if (!j.is_object())
        fail(where, "expected an object");
    if (!j.contains("cc"))
        fail(where, "missing \"cc\"");
    AxisBinding a;
    a.enabled = true;
    a.cc = readU7(j, "cc", 0, where);
    a.min = readU7(j, "min", 0, where);
    a.max = readU7(j, "max", 127, where);
    if (j.contains("invert")) {
        if (!j["invert"].is_boolean())
            fail(where + ".invert", "expected true or false");
        a.invert = j["invert"].get<bool>();
    }
    return a;
}

Json axisToJson(const AxisBinding& a) {
    Json j;
    j["cc"] = a.cc;
    if (a.min != 0)
        j["min"] = a.min;
    if (a.max != 127)
        j["max"] = a.max;
    if (a.invert)
        j["invert"] = true;
    return j;
}

const Json& requireObject(const Json& j, const std::string& where) {
    if (!j.is_object())
        fail(where, "expected an object");
    return j;
}

Mapping mappingFromJson(const Json& j, const std::string& where) {
    requireObject(j, where);
    Mapping m;

    if (j.contains("buttons")) {
        for (const auto& [key, value] : requireObject(j["buttons"], where + ".buttons").items()) {
            const std::string at = where + ".buttons." + key;
            auto button = parseButton(key);
            if (!button)
                fail(at, "unknown button");
            if (isDpad(*button))
                fail(at, "map D-pad directions under \"dpad\"");
            m.buttons[static_cast<std::size_t>(*button)] = triggerFromJson(value, at);
        }
    }
    if (j.contains("dpad")) {
        for (const auto& [key, value] : requireObject(j["dpad"], where + ".dpad").items()) {
            const std::string at = where + ".dpad." + key;
            auto dir = parseHatDir(key);
            if (!dir)
                fail(at, "unknown direction");
            m.dpad[static_cast<std::size_t>(*dir)] = triggerFromJson(value, at);
        }
    }
    if (j.contains("axes")) {
        for (const auto& [key, value] : requireObject(j["axes"], where + ".axes").items()) {
            const std::string at = where + ".axes." + key;
            auto axis = parseAxis(key);
            if (!axis)
                fail(at, "unknown axis");
            m.axes[static_cast<std::size_t>(*axis)] = axisFromJson(value, at);
        }
    }
    if (j.contains("touchpad")) {
        for (const auto& [key, value] : requireObject(j["touchpad"], where + ".touchpad").items()) {
            const std::string at = where + ".touchpad." + key;
            int finger = key == "finger1" ? 0 : key == "finger2" ? 1 : -1;
            if (finger < 0)
                fail(at, "expected \"finger1\" or \"finger2\"");
            requireObject(value, at);
            FingerBinding& f = m.fingers[static_cast<std::size_t>(finger)];
            const std::string mode = readString(value, "mode", nullptr, at);
            if (mode == "axis") {
                f.mode = TouchMode::Axis;
                if (value.contains("x"))
                    f.x = axisFromJson(value["x"], at + ".x");
                if (value.contains("y"))
                    f.y = axisFromJson(value["y"], at + ".y");
            } else if (mode == "zones") {
                f.mode = TouchMode::Zones;
                if (value.contains("zones")) {
                    for (const auto& [zoneKey, zoneValue] : requireObject(value["zones"], at + ".zones").items()) {
                        auto zone = parseZone(zoneKey);
                        if (!zone)
                            fail(at + ".zones." + zoneKey, "unknown zone");
                        f.zones[static_cast<std::size_t>(*zone)] = triggerFromJson(zoneValue, at + ".zones." + zoneKey);
                    }
                }
            } else {
                fail(at + ".mode", "expected \"axis\" or \"zones\"");
            }
        }
    }
    return m;
}

Json mappingToJson(const Mapping& m) {
    Json j = Json::object();

    Json buttons = Json::object();
    for (int i = 0; i < kButtonCount; ++i)
        if (m.buttons[static_cast<std::size_t>(i)].isSet() && !isDpad(static_cast<Button>(i)))
            buttons[std::string(toString(static_cast<Button>(i)))] = triggerToJson(m.buttons[static_cast<std::size_t>(i)]);
    if (!buttons.empty())
        j["buttons"] = buttons;

    Json dpad = Json::object();
    for (int i = 0; i < kHatDirCount; ++i)
        if (m.dpad[static_cast<std::size_t>(i)].isSet())
            dpad[std::string(toString(static_cast<HatDir>(i)))] = triggerToJson(m.dpad[static_cast<std::size_t>(i)]);
    if (!dpad.empty())
        j["dpad"] = dpad;

    Json axes = Json::object();
    for (int i = 0; i < kAxisCount; ++i)
        if (m.axes[static_cast<std::size_t>(i)].enabled)
            axes[std::string(toString(static_cast<Axis>(i)))] = axisToJson(m.axes[static_cast<std::size_t>(i)]);
    if (!axes.empty())
        j["axes"] = axes;

    Json touchpad = Json::object();
    for (int i = 0; i < kFingerCount; ++i) {
        const FingerBinding& f = m.fingers[static_cast<std::size_t>(i)];
        Json fj;
        if (f.mode == TouchMode::Axis) {
            fj["mode"] = "axis";
            if (f.x.enabled)
                fj["x"] = axisToJson(f.x);
            if (f.y.enabled)
                fj["y"] = axisToJson(f.y);
        } else if (f.mode == TouchMode::Zones) {
            fj["mode"] = "zones";
            Json zones = Json::object();
            for (int z = 0; z < kZoneCount; ++z)
                if (f.zones[static_cast<std::size_t>(z)].isSet())
                    zones[std::string(toString(static_cast<Zone>(z)))] = triggerToJson(f.zones[static_cast<std::size_t>(z)]);
            fj["zones"] = zones;
        } else {
            continue;
        }
        touchpad[i == 0 ? "finger1" : "finger2"] = fj;
    }
    if (!touchpad.empty())
        j["touchpad"] = touchpad;
    return j;
}

}  // namespace

std::optional<std::uint8_t> noteFromJson(const Json& value) {
    if (value.is_number_integer()) {
        const int n = value.get<int>();
        if (n >= 0 && n <= 127)
            return static_cast<std::uint8_t>(n);
    } else if (value.is_string()) {
        if (auto n = parseNoteName(value.get<std::string>()))
            return static_cast<std::uint8_t>(*n);
    }
    return std::nullopt;
}

Preset presetFromJson(const Json& j) {
    requireObject(j, "preset");
    if (j.value("format", std::string()) != kFormatName)
        fail("format", "not a MidiPad preset (expected \"format\": \"midipad-preset\")");
    if (!j.contains("version") || !j["version"].is_number_integer())
        fail("version", "missing format version");
    if (j["version"].get<int>() > kPresetFormatVersion)
        fail("version", "written by a newer MidiPad (version " + j["version"].dump() + ")");

    Preset p;
    p.name = j.value("name", std::string());
    p.base = j.contains("base") ? mappingFromJson(j["base"], "base") : Mapping{};
    if (j.contains("channels")) {
        for (const auto& [key, value] : requireObject(j["channels"], "channels").items()) {
            int channel = 0;
            try {
                std::size_t used = 0;
                channel = std::stoi(key, &used);
                if (used != key.size())
                    channel = 0;
            } catch (const std::exception&) {
            }
            if (channel < 1 || channel > kChannelCount)
                fail("channels." + key, "expected a channel number from 1 to 16");
            p.channels[static_cast<std::size_t>(channel - 1)] = mappingFromJson(value, "channels." + key);
        }
    }
    return p;
}

Json presetToJson(const Preset& p) {
    Json j;
    j["format"] = kFormatName;
    j["version"] = kPresetFormatVersion;
    j["name"] = p.name;
    j["base"] = mappingToJson(p.base);
    Json channels = Json::object();
    for (int i = 0; i < kChannelCount; ++i)
        if (const auto& m = p.channels[static_cast<std::size_t>(i)])
            channels[std::to_string(i + 1)] = mappingToJson(*m);
    if (!channels.empty())
        j["channels"] = channels;
    return j;
}

Preset loadPreset(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in)
        throw PresetError(file.string() + ": cannot open");
    try {
        return presetFromJson(Json::parse(in));
    } catch (const Json::exception& e) {
        throw PresetError(file.string() + ": " + e.what());
    } catch (const PresetError& e) {
        throw PresetError(file.string() + ": " + e.what());
    }
}

void savePreset(const Preset& preset, const std::filesystem::path& file) {
    // Write next to the target, then rename, so a crash never leaves half a preset.
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp);
        if (!out)
            throw PresetError(temp.string() + ": cannot write");
        out << presetToJson(preset).dump(4) << '\n';
        if (!out)
            throw PresetError(temp.string() + ": write failed");
    }
    std::filesystem::rename(temp, file);
}

}  // namespace midipad
