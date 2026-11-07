#include "midipad/MidiExport.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>

#include "midipad/Engine.hpp"

namespace midipad {

namespace {

constexpr int kTicksPerBeat = 480;
constexpr std::uint64_t kStepNs = 1'000'000;  // the engine ticks every millisecond, as live
constexpr std::uint64_t kStartNs = 1'000'000'000;

struct Timed {
    std::uint64_t at;
    MidiMessage message;
};

// What the engine sends, with when.
class Recorder : public MidiSink {
public:
    void send(const MidiMessage& m) override {
        if (m.size == 0 || (m.bytes[0] & 0xF0) == 0xF0)
            return;
        if ((m.bytes[0] & 0xF0) == 0xB0 && m.bytes[1] >= 120)  // all notes off and the like: not musical
            return;
        events.push_back({now, m});
    }
    std::uint64_t now = 0;
    std::vector<Timed> events;
};

// An engine with the session in it, its clock at beat 0, nothing playing.
struct Player {
    Recorder recorder;
    Engine engine{recorder};
    double tempo;
    std::uint64_t now = kStartNs;

    Player(const Session& session, const MidiExportOptions& o) : tempo(o.tempo > 0 ? o.tempo : 120) {
        engine.setPreset(std::make_shared<const Preset>(session.preset));
        engine.setSync(0, std::max(o.beatsPerBar, 1), 0);
        engine.setKey(session.key);
        clock();
        engine.tick(now);
        engine.loadSessionLoops(session.loops);
        engine.loadSessionChains(session.chains);
        engine.setSong(session.song);
        engine.setSongLoop(false);
        engine.transportStop();
        engine.forgetPausedClips();
        engine.tick(now);
        recorder.events.clear();  // what loading sent
    }
    double beat() const { return static_cast<double>(now - kStartNs) * tempo / 60e9; }
    void clock() { engine.setBeatClock({true, tempo, beat()}); }
    void step() {
        now += kStepNs;
        recorder.now = now;
        clock();
        engine.tick(now);
    }
    void end() {  // what still sounds, off
        engine.allNotesOff();
        step();
    }
};

void put16(std::vector<std::uint8_t>& out, int v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
}
void putVar(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t bytes[5];
    int n = 0;
    bytes[n++] = static_cast<std::uint8_t>(v & 0x7F);
    while ((v >>= 7) != 0)
        bytes[n++] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
    while (n > 0)
        out.push_back(bytes[--n]);
}

// A track: its events (tick, bytes) in order, then its end.
class Track {
public:
    void meta(std::uint32_t tick, std::uint8_t type, const std::vector<std::uint8_t>& data) {
        delta(tick);
        body_.push_back(0xFF);
        body_.push_back(type);
        putVar(body_, static_cast<std::uint32_t>(data.size()));
        body_.insert(body_.end(), data.begin(), data.end());
    }
    void name(const std::string& text) { meta(0, 0x03, std::vector<std::uint8_t>(text.begin(), text.end())); }
    void tempo(double bpm, int beatsPerBar) {
        const auto us = static_cast<std::uint32_t>(std::lround(60e6 / bpm));
        meta(0, 0x51, {static_cast<std::uint8_t>(us >> 16), static_cast<std::uint8_t>(us >> 8), static_cast<std::uint8_t>(us)});
        meta(0, 0x58, {static_cast<std::uint8_t>(std::clamp(beatsPerBar, 1, 255)), 2, 24, 8});
    }
    void message(std::uint32_t tick, const MidiMessage& m) {
        delta(tick);
        for (int i = 0; i < m.size; ++i)
            body_.push_back(m.bytes[static_cast<std::size_t>(i)]);
    }
    void writeTo(std::vector<std::uint8_t>& out, std::uint32_t endTick) {
        meta(std::max(endTick, last_), 0x2F, {});
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        put32(out, static_cast<std::uint32_t>(body_.size()));
        out.insert(out.end(), body_.begin(), body_.end());
    }

private:
    void delta(std::uint32_t tick) {
        tick = std::max(tick, last_);
        putVar(body_, tick - last_);
        last_ = tick;
    }
    std::vector<std::uint8_t> body_;
    std::uint32_t last_ = 0;
};

std::uint32_t tickOf(const Player& p, std::uint64_t at, std::uint64_t from) {
    const double beats = static_cast<double>(at > from ? at - from : 0) * p.tempo / 60e9;
    return static_cast<std::uint32_t>(std::lround(beats * kTicksPerBeat));
}

std::string channelName(const MidiExportOptions& o, int channel) {
    const std::string& name = o.channelNames[static_cast<std::size_t>(channel)];
    return name.empty() ? "Channel " + std::to_string(channel + 1) : name;
}

std::vector<std::uint8_t> header(int format, int tracks) {
    std::vector<std::uint8_t> out{'M', 'T', 'h', 'd', 0, 0, 0, 6};
    put16(out, format);
    put16(out, tracks);
    put16(out, kTicksPerBeat);
    return out;
}

}  // namespace

std::vector<std::uint8_t> exportSongMidi(const Session& session, const MidiExportOptions& options) {
    if (session.song.sections.empty())
        return {};
    const auto player = std::make_unique<Player>(session, options);  // an engine is large: not on the stack
    Player& p = *player;
    const std::uint64_t from = p.now;
    p.recorder.now = p.now;
    p.engine.transportPlay(0);
    constexpr std::uint64_t kMost = 3600ull * 1'000'000'000ull;  // an hour at most
    while (p.engine.songPlaying() && p.now - from < kMost)
        p.step();
    // The end: on the bar after the last note.
    const double beats = std::ceil(p.beat() / std::max(options.beatsPerBar, 1) - 1e-6) * std::max(options.beatsPerBar, 1);
    p.end();
    const auto endTick = static_cast<std::uint32_t>(std::lround(beats * kTicksPerBeat));

    std::array<std::vector<const Timed*>, 16> byChannel;
    for (const Timed& t : p.recorder.events)
        byChannel[static_cast<std::size_t>(t.message.bytes[0] & 0x0F)].push_back(&t);
    int count = 1;
    for (const auto& events : byChannel)
        count += events.empty() ? 0 : 1;
    std::vector<std::uint8_t> out = header(1, count);
    Track conductor;
    conductor.name(options.title);
    conductor.tempo(p.tempo, options.beatsPerBar);
    conductor.writeTo(out, endTick);
    for (int ch = 0; ch < 16; ++ch) {
        const auto& events = byChannel[static_cast<std::size_t>(ch)];
        if (events.empty())
            continue;
        Track track;
        track.name(channelName(options, ch));
        for (const Timed* t : events)
            track.message(std::min(tickOf(p, t->at, from), endTick), t->message);
        track.writeTo(out, endTick);
    }
    return out;
}

std::vector<std::uint8_t> exportClipMidi(const Session& session, const MidiExportOptions& options, int channel, int slot) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    slot = std::clamp(slot, 0, Engine::kClipSlots - 1);
    const auto player = std::make_unique<Player>(session, options);  // an engine is large: not on the stack
    Player& p = *player;
    const std::uint64_t from = p.now;
    p.recorder.now = p.now;
    p.engine.transportPlay(-1);
    p.engine.launchClip(channel, slot);
    double beats = p.engine.clipBeats(channel, slot);
    if (beats > 0) {
        while (p.beat() < beats)
            p.step();
    } else {  // free: one turn, as the clip's progress tells it
        float last = 0;
        for (int k = 0; k < 600'000; ++k) {
            p.step();
            const float progress = p.engine.clipProgress(channel);
            if (progress < 0 || (k > 10 && progress < last))
                break;
            last = progress;
        }
        beats = std::max(1.0, p.beat());
    }
    p.end();
    const auto endTick = static_cast<std::uint32_t>(std::lround(beats * kTicksPerBeat));
    std::vector<std::uint8_t> out = header(0, 1);
    Track track;
    track.name(options.title.empty() ? channelName(options, channel) : options.title);
    track.tempo(p.tempo, options.beatsPerBar);
    for (const Timed& t : p.recorder.events)
        if ((t.message.bytes[0] & 0x0F) == channel)
            track.message(std::min(tickOf(p, t.at, from), endTick), t.message);
    track.writeTo(out, endTick);
    return out;
}

void writeFile(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write " + file.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
        throw std::runtime_error("cannot write " + file.string());
}

}  // namespace midipad
