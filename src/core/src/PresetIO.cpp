#include "midipad/PresetIO.hpp"

#include "midipad/Files.hpp"
#include "midipad/Notes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <fstream>
#include <string>

namespace midipad {

using Json = nlohmann::ordered_json;

namespace {

constexpr std::string_view kFormatName = "arcadance-preset";
constexpr std::string_view kFormerFormatName = "midipad-preset";  // Arcadance's former name: still read

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

    if (j.contains("humanize")) {
        if (!j["humanize"].is_boolean())
            fail(where + ".humanize", "expected true or false");
        b.humanize = j["humanize"].get<bool>();
    }

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
            const Json below = j.value("root_below", Json(0));
            if (!below.is_number_integer() || below.get<int>() < 0 || below.get<int>() > 2)
                fail(where + ".root_below", "expected 0, 1 or 2 (octaves below the root)");
            b.chordRootBelow = below.get<std::uint8_t>();
            const Json inversion = j.value("inversion", Json(0));
            if (!inversion.is_number_integer() || inversion.get<int>() < 0 || inversion.get<int>() > 8)
                fail(where + ".inversion", "expected 0 (root position) to 8");
            b.chordInversion = inversion.get<std::uint8_t>();
            auto chord = parseChord(b.chordName, b.chordOctave, b.chordRootBelow, b.chordInversion);
            if (!chord && parseChord(b.chordName))
                fail(where + ".octave", "\"" + b.chordName + "\" goes beyond the MIDI notes in this octave");
            if (!chord)
                fail(where + ".chord", "unknown chord \"" + b.chordName + "\"");
            b.chord = *chord;
        } else if (j.contains("notes") && j["notes"].is_array()) {
            for (const Json& n : j["notes"]) {
                auto note = noteFromJson(n);
                if (!note || b.chord.count == 8)
                    fail(where + ".notes", "expected 1 to 8 MIDI notes");
                b.chord.notes[b.chord.count++] = *note;
            }
            if (b.chord.count == 0)
                fail(where + ".notes", "expected 1 to 8 MIDI notes");
        } else {
            fail(where, "missing \"chord\" (a name) or \"notes\" (a list)");
        }
        if (j.contains("voice_leading")) {
            if (!j["voice_leading"].is_boolean())
                fail(where + ".voice_leading", "expected true or false");
            b.chordVoiceLeading = j["voice_leading"].get<bool>();
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
    } else if (type == "program") {
        b.type = ActionType::ProgramChange;
        if (j.contains("step")) {
            const Json step = j["step"];
            if (!step.is_number_integer() || (step.get<int>() != 1 && step.get<int>() != -1))
                fail(where + ".step", "expected 1 or -1");
            b.programStep = step.get<std::int8_t>();
            if (j.contains("wrap")) {
                if (!j["wrap"].is_boolean())
                    fail(where + ".wrap", "expected true or false");
                b.programWrap = j["wrap"].get<bool>();
            }
        } else {
            if (!j.contains("program"))
                fail(where, "missing \"program\" (0-127) or \"step\" (1 or -1)");
            b.number = readU7(j, "program", 0, where);
        }
        if (j.contains("bank_msb"))
            b.bankMsb = readU7(j, "bank_msb", 0, where);
        if (j.contains("bank_lsb"))
            b.bankLsb = readU7(j, "bank_lsb", 0, where);
    } else if (type == "loop_record") {
        b.type = ActionType::LoopRecord;
    } else if (type == "shift" || type == "loop_stretch") {  // "loop_stretch": before the Shift layer
        b.type = ActionType::Shift;
    } else if (type == "tap_tempo") {
        b.type = ActionType::TapTempo;
    } else if (type == "session") {
        b.type = ActionType::SessionScreen;
    } else if (type == "recenter_tilt") {
        b.type = ActionType::RecenterTilt;
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
        if (!b.humanize)
            j["humanize"] = false;
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
            if (b.chordRootBelow != 0)
                j["root_below"] = b.chordRootBelow;
            if (b.chordInversion != 0)
                j["inversion"] = b.chordInversion;
        } else {
            Json notes = Json::array();
            for (auto n : b.chord.view())
                notes.push_back(n);
            j["notes"] = notes;
        }
        if (b.chordVoiceLeading)
            j["voice_leading"] = true;
        if (b.velocity != 100)
            j["velocity"] = b.velocity;
        if (!b.humanize)
            j["humanize"] = false;
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
    case ActionType::LoopRecord:
        j["type"] = "loop_record";
        return j;
    case ActionType::Shift:
        j["type"] = "shift";
        if (b.mode == Mode::Toggle)
            j["mode"] = "toggle";
        return j;
    case ActionType::TapTempo:
        j["type"] = "tap_tempo";
        return j;
    case ActionType::SessionScreen:
        j["type"] = "session";
        return j;
    case ActionType::RecenterTilt:
        j["type"] = "recenter_tilt";
        return j;
    case ActionType::ProgramChange:
        j["type"] = "program";
        if (b.programStep != 0) {
            j["step"] = b.programStep;
            if (!b.programWrap)
                j["wrap"] = false;
        } else {
            j["program"] = b.number;
        }
        if (b.bankMsb >= 0)
            j["bank_msb"] = b.bankMsb;
        if (b.bankLsb >= 0)
            j["bank_lsb"] = b.bankLsb;
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
    AxisBinding a;
    a.enabled = true;
    auto flag = [&](const char* key) {
        if (!j.contains(key))
            return false;
        if (!j[key].is_boolean())
            fail(where + "." + key, "expected true or false");
        return j[key].get<bool>();
    };
    if (flag("program"))
        a.mode = AxisMode::Program;
    if (flag("velocity")) {
        a.mode = AxisMode::Velocity;
        const std::string to = readString(j, "velocity_to", "notes", where);
        if (to == "notes")
            a.velocityTarget = VelocityTarget::Notes;
        else if (to == "loop")
            a.velocityTarget = VelocityTarget::Loop;
        else if (to == "both")
            a.velocityTarget = VelocityTarget::Both;
        else
            fail(where + ".velocity_to", "expected \"notes\", \"loop\" or \"both\"");
    }
    if (j.contains("transpose")) {
        const std::string at = where + ".transpose";
        if (!j["transpose"].is_object())
            fail(at, "expected an object");
        const Json& t = j["transpose"];
        a.mode = AxisMode::Transpose;
        a.transposeDown = readU7(t, "down", 12, at);
        a.transposeUp = readU7(t, "up", 12, at);
        if (a.transposeDown > kMaxTranspose || a.transposeUp > kMaxTranspose)
            fail(at, "expected at most 24 semitones each way");
        const std::string to = readString(t, "to", "notes", at);
        if (to == "notes")
            a.transposeTarget = TransposeTarget::Notes;
        else if (to == "loop")
            a.transposeTarget = TransposeTarget::Loop;
        else
            fail(at + ".to", "expected \"notes\" or \"loop\"");
    }
    if (a.mode == AxisMode::Cc && !j.contains("cc"))
        fail(where, "missing \"cc\"");
    a.cc = readU7(j, "cc", 0, where);
    a.min = readU7(j, "min", 0, where);
    a.max = readU7(j, "max", 127, where);
    a.invert = flag("invert");
    if (a.mode == AxisMode::Program) {
        if (j.contains("bank_msb"))
            a.bankMsb = readU7(j, "bank_msb", 0, where);
        if (j.contains("bank_lsb"))
            a.bankLsb = readU7(j, "bank_lsb", 0, where);
    }
    return a;
}

Json axisToJson(const AxisBinding& a) {
    Json j;
    if (a.mode == AxisMode::Program) {
        j["program"] = true;
        if (a.bankMsb >= 0)
            j["bank_msb"] = a.bankMsb;
        if (a.bankLsb >= 0)
            j["bank_lsb"] = a.bankLsb;
    } else if (a.mode == AxisMode::Velocity) {
        j["velocity"] = true;
        if (a.velocityTarget != VelocityTarget::Notes)
            j["velocity_to"] = a.velocityTarget == VelocityTarget::Loop ? "loop" : "both";
    } else if (a.mode == AxisMode::Transpose) {
        j["transpose"] = {{"down", a.transposeDown},
                          {"up", a.transposeUp},
                          {"to", a.transposeTarget == TransposeTarget::Loop ? "loop" : "notes"}};
    } else {
        j["cc"] = a.cc;
    }
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
    if (j.contains("tilt")) {
        const std::string at = where + ".tilt";
        const Json& tj = requireObject(j["tilt"], at);
        TiltBinding& t = m.tilt;
        if (tj.contains("enabled")) {
            if (!tj["enabled"].is_boolean())
                fail(at + ".enabled", "expected true or false");
            t.enabled = tj["enabled"].get<bool>();
        }
        const std::string active = readString(tj, "active", "held", at);
        if (active == "held")
            t.whileHeld = true;
        else if (active == "always")
            t.whileHeld = false;
        else
            fail(at + ".active", "expected \"held\" or \"always\"");
        if (tj.contains("hold")) {
            const std::string name = readString(tj, "hold", nullptr, at);
            auto button = parseButton(name);
            if (!button)
                fail(at + ".hold", "unknown button");
            t.hold = *button;
        }
        if (tj.contains("angle")) {
            if (!tj["angle"].is_number_integer() || tj["angle"].get<int>() < kMinTiltAngle || tj["angle"].get<int>() > kMaxTiltAngle)
                fail(at + ".angle", "expected 15 to 90");
            t.angle = tj["angle"].get<int>();
        }
        // An axis turned off is written as null; a missing one keeps its default.
        if (tj.contains("x"))
            t.x = tj["x"].is_null() ? AxisBinding{} : axisFromJson(tj["x"], at + ".x");
        if (tj.contains("y"))
            t.y = tj["y"].is_null() ? AxisBinding{} : axisFromJson(tj["y"], at + ".y");
    }
    if (j.contains("shift")) {
        const std::string at = where + ".shift";
        const Json& sj = requireObject(j["shift"], at);
        if (sj.contains("buttons"))
            for (const auto& [key, value] : requireObject(sj["buttons"], at + ".buttons").items()) {
                auto button = parseButton(key);
                if (!button || isDpad(*button))
                    fail(at + ".buttons." + key, "unknown button");
                m.shift.buttons[static_cast<std::size_t>(*button)] = triggerFromJson(value, at + ".buttons." + key);
            }
        if (sj.contains("dpad"))
            for (const auto& [key, value] : requireObject(sj["dpad"], at + ".dpad").items()) {
                auto dir = parseHatDir(key);
                if (!dir)
                    fail(at + ".dpad." + key, "unknown direction");
                m.shift.dpad[static_cast<std::size_t>(*dir)] = triggerFromJson(value, at + ".dpad." + key);
            }
        if (sj.contains("touchpad"))
            for (const auto& [key, value] : requireObject(sj["touchpad"], at + ".touchpad").items()) {
                const int finger = key == "finger1" ? 0 : key == "finger2" ? 1 : -1;
                if (finger < 0)
                    fail(at + ".touchpad." + key, "expected \"finger1\" or \"finger2\"");
                for (const auto& [zoneKey, zoneValue] : requireObject(value, at + ".touchpad." + key).items()) {
                    auto zone = parseZone(zoneKey);
                    if (!zone)
                        fail(at + ".touchpad." + key + "." + zoneKey, "unknown zone");
                    m.shift.zones[static_cast<std::size_t>(finger)][static_cast<std::size_t>(*zone)] =
                        triggerFromJson(zoneValue, at + ".touchpad." + key + "." + zoneKey);
                }
            }
    }
    if (j.contains("loops")) {
        for (const auto& [key, value] : requireObject(j["loops"], where + ".loops").items()) {
            const std::string at = where + ".loops." + key;
            static constexpr std::string_view kKeys[] = {"left", "right", "finger1", "finger2", "triggers", "tilt", "notes"};
            const auto found = std::find(std::begin(kKeys), std::end(kKeys), key);
            if (found == std::end(kKeys))
                fail(at, "expected \"left\", \"right\", \"finger1\", \"finger2\", \"triggers\", \"tilt\" or \"notes\"");
            const auto source = found - std::begin(kKeys);
            requireObject(value, at);
            LoopBinding& loop = m.loops[static_cast<std::size_t>(source)];
            const std::string playback = readString(value, "playback", "forward", at);
            if (playback == "forward")
                loop.playback = LoopPlayback::Forward;
            else if (playback == "reverse")
                loop.playback = LoopPlayback::Reverse;
            else if (playback == "back_and_forth")
                loop.playback = LoopPlayback::BackAndForth;
            else
                fail(at + ".playback", "expected \"forward\", \"reverse\" or \"back_and_forth\"");
            loop.ccX = readU7(value, "cc_x", loop.ccX, at);
            loop.ccY = readU7(value, "cc_y", loop.ccY, at);
            // Steps, per axis: {"levels": 8, "step": "1/16"}.
            for (int axis = 0; axis < 2; ++axis) {
                const char* stepsKey = axis == 0 ? "steps_x" : "steps_y";
                if (!value.contains(stepsKey))
                    continue;
                const std::string here = at + "." + stepsKey;
                const Json& g = requireObject(value[stepsKey], here);
                StepGrid& grid = loop.steps[static_cast<std::size_t>(axis)];
                grid.on = true;
                if (g.contains("levels")) {
                    if (!g["levels"].is_number_integer() || g["levels"].get<int>() < 2 || g["levels"].get<int>() > 128)
                        fail(here + ".levels", "expected 2 to 128");
                    grid.levels = g["levels"].get<int>();
                }
                const std::string step = readString(g, "step", "1/16", here);
                const auto name = std::find(kStepNames.begin(), kStepNames.end(), step);
                if (name == kStepNames.end())
                    fail(here + ".step", "expected 1/32, 1/16, 1/8, 1/4, 1/2 or 1 bar");
                grid.step = static_cast<int>(name - kStepNames.begin());
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

    if (m.tilt != TiltBinding{}) {
        const TiltBinding& t = m.tilt;
        Json tj;
        tj["enabled"] = t.enabled;
        tj["active"] = t.whileHeld ? "held" : "always";
        tj["hold"] = toString(t.hold);
        tj["angle"] = t.angle;
        tj["x"] = t.x.enabled ? axisToJson(t.x) : Json(nullptr);
        tj["y"] = t.y.enabled ? axisToJson(t.y) : Json(nullptr);
        j["tilt"] = tj;
    }

    Json shift = Json::object();
    {
        Json sb = Json::object(), sd = Json::object(), st = Json::object();
        for (int i = 0; i < kButtonCount; ++i)
            if (m.shift.buttons[static_cast<std::size_t>(i)].isSet() && !isDpad(static_cast<Button>(i)))
                sb[std::string(toString(static_cast<Button>(i)))] = triggerToJson(m.shift.buttons[static_cast<std::size_t>(i)]);
        for (int i = 0; i < kHatDirCount; ++i)
            if (m.shift.dpad[static_cast<std::size_t>(i)].isSet())
                sd[std::string(toString(static_cast<HatDir>(i)))] = triggerToJson(m.shift.dpad[static_cast<std::size_t>(i)]);
        for (int f = 0; f < kFingerCount; ++f) {
            Json zones = Json::object();
            for (int z = 0; z < kZoneCount; ++z)
                if (m.shift.zones[static_cast<std::size_t>(f)][static_cast<std::size_t>(z)].isSet())
                    zones[std::string(toString(static_cast<Zone>(z)))] = triggerToJson(m.shift.zones[static_cast<std::size_t>(f)][static_cast<std::size_t>(z)]);
            if (!zones.empty())
                st[f == 0 ? "finger1" : "finger2"] = zones;
        }
        if (!sb.empty())
            shift["buttons"] = sb;
        if (!sd.empty())
            shift["dpad"] = sd;
        if (!st.empty())
            shift["touchpad"] = st;
    }
    if (!shift.empty())
        j["shift"] = shift;

    Json loops = Json::object();
    for (std::size_t s = 0; s < m.loops.size(); ++s) {
        const LoopBinding& loop = m.loops[s];
        if (loop == kDefaultLoops[s])
            continue;
        Json lj;
        lj["playback"] = loop.playback == LoopPlayback::Forward   ? "forward"
                         : loop.playback == LoopPlayback::Reverse ? "reverse"
                                                                  : "back_and_forth";
        lj["cc_x"] = loop.ccX;
        lj["cc_y"] = loop.ccY;
        for (int axis = 0; axis < 2; ++axis) {
            const StepGrid& grid = loop.steps[static_cast<std::size_t>(axis)];
            if (grid.on)
                lj[axis == 0 ? "steps_x" : "steps_y"] = Json{{"levels", grid.levels}, {"step", kStepNames[static_cast<std::size_t>(std::clamp(grid.step, 0, kStepChoices - 1))]}};
        }
        static constexpr const char* kKeys[] = {"left", "right", "finger1", "finger2", "triggers", "tilt", "notes"};
        loops[kKeys[s]] = lj;
    }
    if (!loops.empty())
        j["loops"] = loops;
    return j;
}

}  // namespace

std::string truncateUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes)
        return std::string(text);
    std::size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)  // text[n] continues a character
        --n;
    return std::string(text.substr(0, n));
}

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
    if (const std::string format = j.value("format", std::string()); format != kFormatName && format != kFormerFormatName)
        fail("format", "not an Arcadance preset (expected \"format\": \"arcadance-preset\")");
    if (!j.contains("version") || !j["version"].is_number_integer())
        fail("version", "missing format version");
    if (j["version"].get<int>() > kPresetFormatVersion)
        fail("version", "written by a newer Arcadance (version " + j["version"].dump() + ")");

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
    if (j.contains("channel_names")) {
        for (const auto& [key, value] : requireObject(j["channel_names"], "channel_names").items()) {
            int channel = 0;
            try {
                std::size_t used = 0;
                channel = std::stoi(key, &used);
                if (used != key.size())
                    channel = 0;
            } catch (const std::exception&) {
            }
            if (channel < 1 || channel > kChannelCount || !value.is_string())
                fail("channel_names." + key, "expected a channel number from 1 to 16 and a name");
            p.channelNames[static_cast<std::size_t>(channel - 1)] = truncateUtf8(value.get<std::string>(), 24);
        }
    }
    if (j.contains("chromatic_channels")) {
        const Json& list = j["chromatic_channels"];
        if (!list.is_array())
            fail("chromatic_channels", "expected a list of channels from 1 to 16");
        for (const Json& c : list) {
            if (!c.is_number_integer() || c.get<int>() < 1 || c.get<int>() > kChannelCount)
                fail("chromatic_channels", "expected a list of channels from 1 to 16");
            p.chromaticChannels = static_cast<std::uint16_t>(p.chromaticChannels | (1u << (c.get<int>() - 1)));
        }
    }
    // Channel 1's mapping is the base one, which every channel without its own
    // plays: a mapping saved for channel 1 takes its place.
    if (p.channels[0]) {
        p.base = std::move(*p.channels[0]);
        p.channels[0].reset();
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
    Json names = Json::object();
    for (int i = 0; i < kChannelCount; ++i)
        if (!p.channelNames[static_cast<std::size_t>(i)].empty())
            names[std::to_string(i + 1)] = p.channelNames[static_cast<std::size_t>(i)];
    if (!names.empty())
        j["channel_names"] = names;
    Json chromatic = Json::array();
    for (int i = 0; i < kChannelCount; ++i)
        if ((p.chromaticChannels >> i) & 1u)
            chromatic.push_back(i + 1);
    if (!chromatic.empty())
        j["chromatic_channels"] = chromatic;
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
    const std::string text = presetToJson(preset).dump(4) + '\n';
    try {
        writeFileAtomically(file, text);  // a crash never leaves half a preset
    } catch (const std::filesystem::filesystem_error&) {
        throw;
    } catch (const std::runtime_error& e) {
        throw PresetError(e.what());
    }
}

}  // namespace midipad
