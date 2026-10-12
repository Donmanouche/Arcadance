#include "midipad/Session.hpp"

#include "midipad/PresetIO.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>

namespace midipad {

using Json = nlohmann::ordered_json;

namespace {

constexpr std::string_view kFormatName = "midipad-session";
constexpr int kFormatVersion = 1;
// As the preset's "loops" keys, by LoopSource.
constexpr const char* kSources[] = {"left", "right", "finger1", "finger2", "triggers", "notes"};
constexpr int kMaxSamples = 60 * 200;  // Engine::kMaxLoopSamples
constexpr int kMaxNotes = 4096;        // Engine::kMaxNoteEvents

[[noreturn]] void fail(const std::string& where, const std::string& message) {
    throw SessionError(where + ": " + message);
}

int readInt(const Json& object, const char* key, int low, int high, int fallback, const std::string& where) {
    if (!object.contains(key))
        return fallback;
    const Json& v = object[key];
    if (!v.is_number_integer() || v.get<long long>() < low || v.get<long long>() > high)
        fail(where + "." + key, "expected an integer from " + std::to_string(low) + " to " + std::to_string(high));
    return v.get<int>();
}

double readNumber(const Json& object, const char* key, const std::string& where) {
    if (!object.contains(key))
        return 0;
    if (!object[key].is_number())
        fail(where + "." + key, "expected a number");
    return object[key].get<double>();
}

std::vector<std::uint16_t> readSamples(const Json& loop, const char* key, int length, const std::string& where) {
    if (!loop.contains(key) || !loop[key].is_array() || static_cast<int>(loop[key].size()) != length)
        fail(where + "." + key, "expected " + std::to_string(length) + " samples");
    std::vector<std::uint16_t> out;
    out.reserve(static_cast<std::size_t>(length));
    for (const Json& v : loop[key]) {
        if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > 0xFFFF)
            fail(where + "." + key, "expected samples from 0 to 65535");
        out.push_back(v.get<std::uint16_t>());
    }
    return out;
}

}  // namespace

Json sessionToJson(const Session& session) {
    Json j;
    j["format"] = kFormatName;
    j["version"] = kFormatVersion;
    j["channel"] = session.channel + 1;
    if (session.tempo > 0)
        j["tempo"] = session.tempo;
    if (session.key)
        j["key"] = Json{{"root", session.key->root}, {"scale", std::string(session.key->type().id)}};
    else
        j["key"] = nullptr;
    j["preset"] = presetToJson(session.preset);
    Json loops = Json::array();
    for (const SessionLoop& l : session.loops) {
        Json o;
        o["channel"] = l.channel + 1;
        o["source"] = kSources[static_cast<int>(l.source)];
        o["length"] = l.length;
        if (l.syncBeats > 0) {
            o["sync_beats"] = l.syncBeats;
            o["anchor_beat"] = l.anchorBeat;
        }
        if (l.source == LoopSource::Notes) {
            Json notes = Json::array();
            for (const SessionLoop::Note& n : l.notes)
                notes.push_back(Json::array({n.at, n.note, n.velocity, n.on ? 1 : 0, n.press}));
            o["notes"] = std::move(notes);
        } else {
            o["stretch"] = l.stretch;
            o["amplitude"] = l.amplitude;
            o["x"] = l.x;
            o["y"] = l.y;
        }
        loops.push_back(std::move(o));
    }
    j["loops"] = std::move(loops);
    return j;
}

Session sessionFromJson(const Json& j) {
    if (!j.is_object() || !j.contains("format") || j["format"] != kFormatName)
        fail("session", "not a MidiPad session file");
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() > kFormatVersion)
        fail("session", "made by a newer MidiPad");
    Session s;
    s.channel = readInt(j, "channel", 1, 16, 1, "session") - 1;
    s.tempo = readNumber(j, "tempo", "session");
    if (s.tempo != 0 && (s.tempo < 20 || s.tempo > 999))
        fail("session.tempo", "expected 20 to 999 beats a minute");
    if (j.contains("key") && j["key"].is_object()) {
        const Json& k = j["key"];
        const int root = readInt(k, "root", 0, 11, 0, "session.key");
        const auto scale = k.contains("scale") && k["scale"].is_string() ? findScale(k["scale"].get<std::string>()) : std::nullopt;
        if (!scale)
            fail("session.key.scale", "unknown scale");
        s.key = Key{root, *scale};
    }
    if (!j.contains("preset"))
        fail("session", "missing \"preset\"");
    s.preset = presetFromJson(j["preset"]);
    if (j.contains("loops")) {
        if (!j["loops"].is_array())
            fail("session.loops", "expected an array");
        for (std::size_t i = 0; i < j["loops"].size(); ++i) {
            const Json& o = j["loops"][i];
            const std::string where = "session.loops[" + std::to_string(i) + "]";
            if (!o.is_object())
                fail(where, "expected an object");
            SessionLoop l;
            l.channel = readInt(o, "channel", 1, 16, 1, where) - 1;
            const std::string source = o.contains("source") && o["source"].is_string() ? o["source"].get<std::string>() : "";
            const auto found = std::find(std::begin(kSources), std::end(kSources), source);
            if (found == std::end(kSources))
                fail(where + ".source", "unknown source \"" + source + "\"");
            l.source = static_cast<LoopSource>(found - std::begin(kSources));
            l.length = readInt(o, "length", 2, kMaxSamples, 0, where);
            if (l.length < 2)
                fail(where, "missing \"length\"");
            l.syncBeats = std::max(0.0, readNumber(o, "sync_beats", where));
            l.anchorBeat = readNumber(o, "anchor_beat", where);
            if (l.source == LoopSource::Notes) {
                if (!o.contains("notes") || !o["notes"].is_array() || o["notes"].size() > static_cast<std::size_t>(kMaxNotes))
                    fail(where + ".notes", "expected at most " + std::to_string(kMaxNotes) + " notes");
                for (const Json& n : o["notes"]) {
                    if (!n.is_array() || n.size() < 4 || n.size() > 5 || !n[0].is_number_integer() || !n[1].is_number_integer() ||
                        !n[2].is_number_integer() || !n[3].is_number_integer() || (n.size() == 5 && !n[4].is_number_unsigned()))
                        fail(where + ".notes", "expected [sample, note, velocity, on, press]");
                    const int at = n[0].get<int>(), note = n[1].get<int>(), velocity = n[2].get<int>();
                    if (at < 0 || at >= l.length || note < 0 || note > 127 || velocity < 0 || velocity > 127)
                        fail(where + ".notes", "a note out of range");
                    l.notes.push_back({at, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity), n[3].get<int>() != 0,
                                       n.size() == 5 ? n[4].get<std::uint32_t>() : 0u});
                }
                std::stable_sort(l.notes.begin(), l.notes.end(),
                                 [](const SessionLoop::Note& a, const SessionLoop::Note& b) { return a.at < b.at; });
            } else {
                l.stretch = readInt(o, "stretch", -38, 198, 0, where);
                l.amplitude = readInt(o, "amplitude", 0, 800, 100, where);
                l.x = readSamples(o, "x", l.length, where);
                l.y = readSamples(o, "y", l.length, where);
            }
            s.loops.push_back(std::move(l));
        }
    }
    return s;
}

void saveSession(const Session& session, const std::filesystem::path& file) {
    // Write next to the target, then rename, so a crash never leaves half a session.
    std::filesystem::create_directories(file.parent_path());
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp);
        if (!out)
            throw SessionError(temp.string() + ": cannot write");
        out << sessionToJson(session).dump() << '\n';  // compact: the loops are long
        if (!out)
            throw SessionError(temp.string() + ": write failed");
    }
    std::filesystem::rename(temp, file);
}

Session loadSession(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in)
        throw SessionError(file.string() + ": cannot open");
    try {
        return sessionFromJson(Json::parse(in));
    } catch (const Json::exception& e) {
        throw SessionError(file.string() + ": " + e.what());
    } catch (const std::runtime_error& e) {  // SessionError, PresetError
        throw SessionError(file.string() + ": " + e.what());
    }
}

}  // namespace midipad
