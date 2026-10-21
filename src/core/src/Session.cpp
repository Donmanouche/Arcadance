#include "midipad/Session.hpp"

#include "midipad/PresetIO.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>

namespace midipad {

using Json = nlohmann::ordered_json;

namespace {

constexpr std::string_view kFormatName = "arcadance-session";
constexpr std::string_view kFormerFormatName = "midipad-session";  // Arcadance's former name: still read
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
    // The channels muted and soloed, numbered from 1 (none: no field).
    for (const auto& [name, bits] : {std::pair{"muted", session.muted}, std::pair{"soloed", session.soloed}}) {
        Json channels = Json::array();
        for (int c = 0; c < 16; ++c)
            if ((bits >> c) & 1u)
                channels.push_back(c + 1);
        if (!channels.empty())
            j[name] = std::move(channels);
    }
    if (session.key)
        j["key"] = Json{{"root", session.key->root}, {"scale", std::string(session.key->type().id)}};
    else
        j["key"] = nullptr;
    j["preset"] = presetToJson(session.preset);
    Json loops = Json::array();
    for (const SessionLoop& l : session.loops) {
        Json o;
        o["channel"] = l.channel + 1;
        if (l.clip != 0)
            o["clip"] = l.clip + 1;
        if (!l.current)
            o["current"] = false;
        if (!l.playing)
            o["playing"] = false;
        o["source"] = kSources[static_cast<int>(l.source)];
        o["length"] = l.length;
        if (l.syncBeats > 0) {
            o["sync_beats"] = l.syncBeats;
            o["anchor_beat"] = l.anchorBeat;
        }
        if (l.locked)
            o["locked"] = true;
        if (l.source == LoopSource::Notes) {
            Json notes = Json::array();
            for (const SessionLoop::Note& n : l.notes)
                notes.push_back(Json::array({n.at, n.note, n.velocity, n.on ? 1 : 0, n.press}));
            o["notes"] = std::move(notes);
        } else {
            o["stretch"] = l.stretch;
            o["amplitude"] = l.amplitude;
            if (l.axis >= 0) {  // one axis
                o["axis"] = l.axis == 0 ? "x" : "y";
                o["samples"] = l.x;
            } else {
                o["x"] = l.x;
                o["y"] = l.y;
            }
        }
        loops.push_back(std::move(o));
    }
    j["loops"] = std::move(loops);
    // The chains: [clip, repeats] per step (clip from 1, or "random"; repeats 0: forever), null when empty.
    if (!session.chains.empty()) {
        Json chains = Json::array();
        for (const SessionChain& c : session.chains) {
            Json steps = Json::array();
            for (const SessionChain::Step& st : c.steps)
                steps.push_back(st.clip < 0 ? Json(nullptr)
                                : st.clip >= 8 ? Json::array({"random", st.repeats})
                                               : Json::array({st.clip + 1, st.repeats}));
            Json o;
            o["channel"] = c.channel + 1;
            o["chain"] = std::string(1, static_cast<char>('A' + c.index));
            if (c.then != -1)
                o["then"] = c.then == -2 ? std::string("stop") : std::string(1, static_cast<char>('A' + c.then));
            if (!c.name.empty())
                o["name"] = c.name;
            if (c.shown)
                o["shown"] = true;
            o["steps"] = std::move(steps);
            chains.push_back(std::move(o));
        }
        j["chains"] = std::move(chains);
    }
    // The song: its sections, each with the items of its channels ("1" to "16").
    if (!session.song.empty()) {
        Json sections = Json::array();
        for (const SongSection& sec : session.song.sections) {
            Json o;
            o["name"] = sec.name;
            o["bars"] = sec.bars;
            if (sec.fit)
                o["fit"] = true;
            Json channels = Json::object();
            for (int ch = 0; ch < kChannelCount; ++ch) {
                const auto& items = sec.cells[static_cast<std::size_t>(ch)];
                if (items.empty())
                    continue;
                Json list = Json::array();
                for (const SongItem& it : items) {
                    Json i;
                    if (it.kind == SongItem::Kind::Clip)
                        i["clip"] = it.index + 1;
                    else if (it.kind == SongItem::Kind::Chain)
                        i["chain"] = std::string(1, static_cast<char>('A' + it.index));
                    else
                        i["rest"] = it.beats;  // in beats
                    if (it.after)
                        i["after"] = true;
                    else
                        i["bar"] = it.bar + 1;
                    if (it.repeats != 1)
                        i["repeats"] = it.repeats;
                    if (it.loop)
                        i["loop"] = true;
                    list.push_back(std::move(i));
                }
                channels[std::to_string(ch + 1)] = std::move(list);
            }
            o["channels"] = std::move(channels);
            sections.push_back(std::move(o));
        }
        j["song"] = {{"sections", std::move(sections)}};
    }
    return j;
}

Session sessionFromJson(const Json& j) {
    if (!j.is_object() || !j.contains("format") || (j["format"] != kFormatName && j["format"] != kFormerFormatName))
        fail("session", "not an Arcadance session file");
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() > kFormatVersion)
        fail("session", "made by a newer Arcadance");
    Session s;
    s.channel = readInt(j, "channel", 1, 16, 1, "session") - 1;
    s.tempo = readNumber(j, "tempo", "session");
    if (s.tempo != 0 && (s.tempo < 20 || s.tempo > 999))
        fail("session.tempo", "expected 20 to 999 beats a minute");
    for (const auto& [name, bits] : {std::pair{"muted", &s.muted}, std::pair{"soloed", &s.soloed}})
        if (j.contains(name) && j[name].is_array())
            for (const Json& c : j[name])
                if (c.is_number_integer() && c.get<int>() >= 1 && c.get<int>() <= 16)
                    *bits = static_cast<std::uint16_t>(*bits | (1u << (c.get<int>() - 1)));
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
            l.clip = readInt(o, "clip", 1, 8, 1, where) - 1;
            for (const char* key : {"current", "playing"})
                if (o.contains(key) && !o[key].is_boolean())
                    fail(where + "." + key, "expected true or false");
            l.current = o.value("current", true);
            l.playing = o.value("playing", true);
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
            if (o.contains("locked")) {
                if (!o["locked"].is_boolean())
                    fail(where + ".locked", "expected true or false");
                l.locked = o["locked"].get<bool>();
            }
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
                if (o.contains("axis")) {
                    if (!o["axis"].is_string() || (o["axis"] != "x" && o["axis"] != "y"))
                        fail(where + ".axis", "expected \"x\" or \"y\"");
                    l.axis = o["axis"] == "x" ? 0 : 1;
                    l.x = readSamples(o, "samples", l.length, where);
                } else {  // from before X and Y were apart
                    l.x = readSamples(o, "x", l.length, where);
                    l.y = readSamples(o, "y", l.length, where);
                }
            }
            s.loops.push_back(std::move(l));
        }
    }
    if (j.contains("chains")) {
        if (!j["chains"].is_array())
            fail("session.chains", "expected an array");
        for (std::size_t i = 0; i < j["chains"].size(); ++i) {
            const Json& o = j["chains"][i];
            const std::string where = "session.chains[" + std::to_string(i) + "]";
            if (!o.is_object())
                fail(where, "expected an object");
            SessionChain c;
            c.channel = readInt(o, "channel", 1, 16, 1, where) - 1;
            // A chain letter, "loop", "stop" or another chain's letter.
            auto letter = [&](const Json& v, const std::string& at, bool words) {
                const std::string text = v.is_string() ? v.get<std::string>() : std::string();
                if (words && text == "loop")
                    return -1;
                if (words && text == "stop")
                    return -2;
                if (text.size() != 1 || text[0] < 'A' || text[0] > 'H')
                    fail(at, words ? "expected \"loop\", \"stop\" or a chain from \"A\" to \"H\"" : "expected a chain from \"A\" to \"H\"");
                return text[0] - 'A';
            };
            if (o.contains("chain"))
                c.index = letter(o["chain"], where + ".chain", false);
            if (o.contains("loop")) {  // from before there were several chains
                if (!o["loop"].is_boolean())
                    fail(where + ".loop", "expected true or false");
                c.then = o["loop"].get<bool>() ? -1 : -2;
            }
            if (o.contains("then"))
                c.then = letter(o["then"], where + ".then", true);
            if (o.contains("name")) {
                if (!o["name"].is_string())
                    fail(where + ".name", "expected a name");
                c.name = o["name"].get<std::string>().substr(0, 24);
            }
            if (o.contains("shown")) {
                if (!o["shown"].is_boolean())
                    fail(where + ".shown", "expected true or false");
                c.shown = o["shown"].get<bool>();
            }
            if (o.contains("steps")) {
                const Json& steps = o["steps"];
                if (!steps.is_array() || steps.size() > 8)
                    fail(where + ".steps", "expected up to 8 steps");
                for (std::size_t k = 0; k < steps.size(); ++k) {
                    SessionChain::Step st;
                    if (!steps[k].is_null()) {
                        const Json& v = steps[k];
                        const bool random = v.is_array() && v.size() == 2 && v[0] == "random";
                        if (!v.is_array() || v.size() != 2 || (!random && (!v[0].is_number_integer() || v[0].get<int>() < 1 || v[0].get<int>() > 8)) ||
                            !v[1].is_number_integer() || v[1].get<int>() < 0 || v[1].get<int>() > 64)
                            fail(where + ".steps[" + std::to_string(k) + "]", "expected [clip 1-8 or \"random\", repeats 0-64] or null");
                        st.clip = random ? 8 : v[0].get<int>() - 1;
                        st.repeats = v[1].get<int>();
                    }
                    c.steps.push_back(st);
                }
            }
            s.chains.push_back(std::move(c));
        }
    }
    if (j.contains("song")) {
        const Json& song = j["song"];
        if (!song.is_object() || (song.contains("sections") && !song["sections"].is_array()))
            fail("session.song", "expected an object with a list of sections");
        const Json sections = song.value("sections", Json::array());
        for (std::size_t i = 0; i < sections.size(); ++i) {
            const Json& o = sections[i];
            const std::string where = "session.song.sections[" + std::to_string(i) + "]";
            if (!o.is_object())
                fail(where, "expected an object");
            SongSection sec;
            if (o.contains("name")) {
                if (!o["name"].is_string())
                    fail(where + ".name", "expected a name");
                sec.name = o["name"].get<std::string>().substr(0, 24);
            }
            sec.bars = readInt(o, "bars", 1, kMaxSectionBars, 8, where);
            if (o.contains("fit")) {
                if (!o["fit"].is_boolean())
                    fail(where + ".fit", "expected true or false");
                sec.fit = o["fit"].get<bool>();
            }
            if (o.contains("channels")) {
                if (!o["channels"].is_object())
                    fail(where + ".channels", "expected an object");
                for (const auto& [key, list] : o["channels"].items()) {
                    const std::string at = where + ".channels." + key;
                    int ch = 0;
                    try {
                        ch = std::stoi(key);
                    } catch (const std::exception&) {
                        ch = 0;
                    }
                    if (ch < 1 || ch > kChannelCount || !list.is_array() || list.size() > static_cast<std::size_t>(kMaxSongItems))
                        fail(at, "expected a channel from 1 to 16 and up to 32 items");
                    auto& items = sec.cells[static_cast<std::size_t>(ch - 1)];
                    for (std::size_t k = 0; k < list.size(); ++k) {
                        const Json& v = list[k];
                        const std::string w = at + "[" + std::to_string(k) + "]";
                        if (!v.is_object())
                            fail(w, "expected an object");
                        SongItem it;
                        if (v.contains("clip")) {
                            it.kind = SongItem::Kind::Clip;
                            it.index = readInt(v, "clip", 1, 8, 1, w) - 1;
                        } else if (v.contains("chain") && v["chain"].is_string() && v["chain"].get<std::string>().size() == 1 &&
                                   v["chain"].get<std::string>()[0] >= 'A' && v["chain"].get<std::string>()[0] <= 'H') {
                            it.kind = SongItem::Kind::Chain;
                            it.index = v["chain"].get<std::string>()[0] - 'A';
                        } else if (v.contains("rest")) {
                            it.kind = SongItem::Kind::Rest;
                            it.beats = readInt(v, "rest", 1, kMaxRestBeats, 4, w);
                        } else {
                            fail(w, "expected \"clip\" (1 to 8), \"chain\" (\"A\" to \"H\") or \"rest\" (beats)");
                        }
                        it.bar = readInt(v, "bar", 1, kMaxSectionBars, 1, w) - 1;
                        it.repeats = readInt(v, "repeats", 1, kMaxSongRepeats, 1, w);
                        if (v.contains("loop")) {
                            if (!v["loop"].is_boolean())
                                fail(w + ".loop", "expected true or false");
                            it.loop = v["loop"].get<bool>();
                        }
                        if (v.contains("after")) {
                            if (!v["after"].is_boolean())
                                fail(w + ".after", "expected true or false");
                            it.after = v["after"].get<bool>();
                        }
                        items.push_back(it);
                    }
                }
            }
            s.song.sections.push_back(std::move(sec));
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
