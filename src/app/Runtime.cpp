#include "Runtime.hpp"

#include "GamepadInput.hpp"

#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"

#include <SDL3/SDL.h>
#include <ableton/Link.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#ifdef __linux__
#include <pthread.h>
#endif

using namespace midipad;

namespace {

void storeMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    if (value > target.load(std::memory_order_relaxed))
        target.store(value, std::memory_order_relaxed);
}

std::string describe(const MidiMessage& m) {
    char line[96];
    const int channel = (m.bytes[0] & 0x0F) + 1;
    switch (m.bytes[0] & 0xF0) {
    case 0x90:
        std::snprintf(line, sizeof line, "ch %2d  note on   %-4s (%3d)  vel %d", channel, noteName(m.bytes[1]).c_str(),
                      m.bytes[1], m.bytes[2]);
        break;
    case 0x80:
        std::snprintf(line, sizeof line, "ch %2d  note off  %-4s (%3d)", channel, noteName(m.bytes[1]).c_str(),
                      m.bytes[1]);
        break;
    case 0xB0:
        std::snprintf(line, sizeof line, "ch %2d  CC %3d = %d", channel, m.bytes[1], m.bytes[2]);
        break;
    default:
        std::snprintf(line, sizeof line, "%s", m.bytes[0] == 0xFA   ? "transport: start"
                                               : m.bytes[0] == 0xFB ? "transport: continue"
                                               : m.bytes[0] == 0xFC ? "transport: stop"
                                                                    : "MIDI message");
    }
    return line;
}

}  // namespace

std::string describe(const Report& r) {
    switch (r.kind) {
    case Report::Kind::Midi:
        return describe(r.message);
    case Report::Kind::Channel:
        return "-> channel " + std::to_string(r.channel + 1);
    case Report::Kind::Button:
        return "[" + label(r.button) + (r.pressed ? " pressed]" : " released]");
    case Report::Kind::Freeze: {
        std::string axes;
        for (int a = 0; a < kAxisCount; ++a)
            if (r.frozenAxes & (1u << a))
                axes += (axes.empty() ? "" : ", ") + label(static_cast<Axis>(a));
        return "[freeze channel " + std::to_string(r.channel + 1) + ": " + (axes.empty() ? "no axis" : axes) + "]";
    }
    }
    return {};
}

// Sends the engine's MIDI to the current output and reports to the main thread.
class Runtime::Tap final : public MidiSink, public EngineObserver {
public:
    Tap(Runtime& runtime, std::shared_ptr<MidiOutput> output) : runtime_(runtime), output_(std::move(output)) {}

    void setOutput(std::shared_ptr<MidiOutput> output) { output_ = std::move(output); }

    void send(const MidiMessage& m) override {
        if (output_)
            output_->send(m);
        if (runtime_.reportMidi_.load(std::memory_order_relaxed))
            runtime_.report({Report::Kind::Midi, m});
    }
    void channelChanged(int channel) override {
        Report r{Report::Kind::Channel};
        r.channel = channel;
        runtime_.report(r);
    }
    void freezeChanged(int channel, std::uint8_t frozenAxes) override {
        Report r{Report::Kind::Freeze};
        r.channel = channel;
        r.frozenAxes = frozenAxes;
        runtime_.report(r);
    }
    void noteSent(int channel, int note, int velocity, bool fromLoop) override {
        runtime_.notes_.push({SDL_GetTicksNS(), static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(note),
                              static_cast<std::uint8_t>(velocity), fromLoop});  // dropped if the window lags
    }

private:
    Runtime& runtime_;
    std::shared_ptr<MidiOutput> output_;
};

struct Runtime::LinkSession {
    ableton::Link link{120.0};
};

Runtime::Runtime(std::shared_ptr<MidiOutput> output, std::shared_ptr<const Preset> preset, Options options, Log log)
    : log_(std::move(log)), reportMidi_(options.reportMidi), reportButtons_(options.reportButtons),
      dpadSettlePress_(options.dpadSettlePressMs), dpadSettleRelease_(options.dpadSettleReleaseMs),
      humanize_(pack(options.humanize)),
      output_(output), channel_(options.channel), link_(std::make_unique<LinkSession>()) {
    pendingOutput_ = std::move(output);
    thread_ = std::jthread([this, options, preset](std::stop_token stop) { run(stop, options, preset); });
}

Runtime::~Runtime() {
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();
}

void Runtime::setPreset(std::shared_ptr<const Preset> preset) {
    std::lock_guard lock(pendingMutex_);
    pendingPreset_ = std::move(preset);
    hasPending_.store(true, std::memory_order_release);
}

void Runtime::setOutput(std::shared_ptr<MidiOutput> output) {
    output_ = output;
    std::lock_guard lock(pendingMutex_);
    pendingOutput_ = std::move(output);
    hasPending_.store(true, std::memory_order_release);
}

void Runtime::setChannel(int channel) { commands_.push({Command::Kind::SetChannel, channel}); }

void Runtime::setClock(ClockMode mode) {
    link_->link.enable(mode == ClockMode::Link);
    clock_.store(mode, std::memory_order_relaxed);
}

void Runtime::setTempo(double bpm) {
    bpm = std::clamp(bpm, midipad::TapTempo::kMinTempo, midipad::TapTempo::kMaxTempo);
    if (clock_.load(std::memory_order_relaxed) == ClockMode::Internal)
        internalTempo_.store(bpm, std::memory_order_relaxed);
    requestedTempo_.store(bpm, std::memory_order_relaxed);
}

void Runtime::setLinkStartStopSync(bool on) {
    link_->link.enableStartStopSync(on);
    startStopSync_.store(on, std::memory_order_relaxed);
}

void Runtime::panic() { commands_.push({Command::Kind::Panic}); }

void Runtime::clearLoop(int source) { commands_.push({Command::Kind::ClearLoop, source}); }

void Runtime::stretchLoop(int source, int steps) { commands_.push({Command::Kind::StretchLoop, source, steps}); }

void Runtime::amplifyLoop(int source, int steps) { commands_.push({Command::Kind::AmplifyLoop, source, steps}); }

std::optional<std::vector<midipad::SessionLoop>> Runtime::sessionLoops() {
    std::future<std::vector<midipad::SessionLoop>> answer;
    {
        std::lock_guard lock(sessionMutex_);
        loopsWanted_.emplace();
        answer = loopsWanted_->get_future();
    }
    commands_.push({Command::Kind::SaveLoops});
    if (answer.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
        std::lock_guard lock(sessionMutex_);
        loopsWanted_.reset();
        return std::nullopt;
    }
    return answer.get();
}

void Runtime::loadSessionLoops(std::vector<midipad::SessionLoop> loops) {
    {
        std::lock_guard lock(sessionMutex_);
        loopsToLoad_ = std::move(loops);
    }
    commands_.push({Command::Kind::LoadLoops});
}

void Runtime::moveLoopNotes(std::vector<midipad::LoopNoteRef> notes, int samples, int semitones, bool byDegrees) {
    {
        std::lock_guard lock(sessionMutex_);
        noteEdits_.push_back({std::move(notes), false, samples, semitones, byDegrees, false});
    }
    commands_.push({Command::Kind::EditNotes});
}

void Runtime::removeLoopNotes(std::vector<midipad::LoopNoteRef> notes) {
    {
        std::lock_guard lock(sessionMutex_);
        noteEdits_.push_back({std::move(notes), true, 0, 0, false, false});
    }
    commands_.push({Command::Kind::EditNotes});
}

void Runtime::resizeLoopNotes(std::vector<midipad::LoopNoteRef> notes, int samples) {
    {
        std::lock_guard lock(sessionMutex_);
        noteEdits_.push_back({std::move(notes), false, samples, 0, false, true});
    }
    commands_.push({Command::Kind::EditNotes});
}

void Runtime::pauseLoops() { commands_.push({Command::Kind::PauseLoops}); }

void Runtime::playLoops() { commands_.push({Command::Kind::PlayLoops}); }

LiveState Runtime::live() const {
    LiveState s;
    s.connected = connected_.load(std::memory_order_relaxed);
    s.hasTouchpad = hasTouchpad_.load(std::memory_order_relaxed);
    s.buttons = buttons_.load(std::memory_order_relaxed);
    for (std::size_t a = 0; a < s.axes.size(); ++a)
        s.axes[a] = axes_[a].load(std::memory_order_relaxed);
    for (std::size_t f = 0; f < s.fingers.size(); ++f)
        s.fingers[f] = {fingerDown_[f].load(std::memory_order_relaxed), fingerX_[f].load(std::memory_order_relaxed),
                        fingerY_[f].load(std::memory_order_relaxed)};
    s.channel = channel_.load(std::memory_order_relaxed);
    s.frozenAxes = frozen_.load(std::memory_order_relaxed);
    s.sounding = sounding_.load(std::memory_order_relaxed);
    s.loopChannels = loopChannels_.load(std::memory_order_relaxed);
    s.loopsPaused = loopsPaused_.load(std::memory_order_relaxed);
    s.stretchHeld = stretchHeld_.load(std::memory_order_relaxed);
    s.velocityShaping = velocityShaping_.load(std::memory_order_relaxed);
    s.transposeNotes = transposeNotes_.load(std::memory_order_relaxed);
    s.transposeLoop = transposeLoop_.load(std::memory_order_relaxed);
    s.clock = clock_.load(std::memory_order_relaxed);
    s.tempo = tempo_.load(std::memory_order_relaxed);
    s.beat = beat_.load(std::memory_order_relaxed);
    s.linkPeers = s.clock == ClockMode::Link ? static_cast<int>(link_->link.numPeers()) : 0;
    s.beatsPerBar = beatsPerBar_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(loopsMutex_);
        s.loops = loops_;
        s.noteLoop = noteLoop_;
        s.noteLoopCount = noteLoopCount_;
    }
    std::lock_guard lock(nameMutex_);
    s.gamepadName = gamepadName_;
    return s;
}

RuntimeStats Runtime::stats() const {
    RuntimeStats s;
    s.ticks = ticks_.load();
    s.averageWorkUs = s.ticks ? static_cast<double>(totalWorkNs_.load()) / static_cast<double>(s.ticks) / 1000.0 : 0;
    s.maxWorkUs = static_cast<double>(maxWorkNs_.load()) / 1000.0;
    s.maxLateUs = static_cast<double>(maxLateNs_.load()) / 1000.0;
    s.droppedReports = droppedReports_.load();
    s.sendErrors = output_ ? output_->sendErrors() : 0;
    return s;
}

void Runtime::report(const Report& r) {
    if (!reports_.push(r))
        droppedReports_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Runtime::pack(const Humanize& h) {
    auto field = [](int v, int high) { return static_cast<std::uint64_t>(std::clamp(v, 0, high)); };
    return static_cast<std::uint64_t>(h.channels) | field(h.velocityMinPercent, 127) << 16 |
           field(h.velocityMaxPercent, 127) << 23 | field(h.delayMinMs, 1023) << 30 | field(h.delayMaxMs, 1023) << 40;
}

Humanize Runtime::unpack(std::uint64_t w) {
    Humanize h;
    h.channels = static_cast<std::uint16_t>(w & 0xFFFF);
    h.velocityMinPercent = static_cast<int>((w >> 16) & 0x7F);
    h.velocityMaxPercent = static_cast<int>((w >> 23) & 0x7F);
    h.delayMinMs = static_cast<int>((w >> 30) & 0x3FF);
    h.delayMaxMs = static_cast<int>((w >> 40) & 0x3FF);
    return h;
}

void Runtime::publish(const GamepadInput& gamepad, const Engine& engine) {
    const bool connected = gamepad.connected();
    if (connected != connected_.load(std::memory_order_relaxed)) {
        std::lock_guard lock(nameMutex_);  // rare: only when a gamepad comes or goes
        gamepadName_ = gamepad.name();
        hasTouchpad_.store(gamepad.hasTouchpad(), std::memory_order_relaxed);
        connected_.store(connected, std::memory_order_relaxed);
    }
    buttons_.store(static_cast<std::uint32_t>(gamepad.buttons().to_ulong()), std::memory_order_relaxed);
    for (std::size_t a = 0; a < axes_.size(); ++a)
        axes_[a].store(gamepad.axes()[a], std::memory_order_relaxed);
    for (std::size_t f = 0; f < fingerDown_.size(); ++f) {
        fingerDown_[f].store(gamepad.fingers()[f].down, std::memory_order_relaxed);
        fingerX_[f].store(gamepad.fingers()[f].x, std::memory_order_relaxed);
        fingerY_[f].store(gamepad.fingers()[f].y, std::memory_order_relaxed);
    }
    channel_.store(engine.channel(), std::memory_order_relaxed);
    std::uint8_t frozen = 0;
    for (int a = 0; a < kAxisCount; ++a)
        if (engine.isFrozen(static_cast<Axis>(a)))
            frozen = static_cast<std::uint8_t>(frozen | (1u << a));
    frozen_.store(frozen, std::memory_order_relaxed);
    sounding_.store(engine.soundingSlots(), std::memory_order_relaxed);
    loopChannels_.store(engine.channelsWithLoops(), std::memory_order_relaxed);
    loopsPaused_.store(engine.loopsPaused(), std::memory_order_relaxed);
    stretchHeld_.store(engine.stretchHeld(), std::memory_order_relaxed);
    const auto shaping = engine.shaping(engine.channel());
    velocityShaping_.store(shaping.velocity, std::memory_order_relaxed);
    transposeNotes_.store(shaping.transposeNotes, std::memory_order_relaxed);
    transposeLoop_.store(shaping.transposeLoop, std::memory_order_relaxed);

    // The loops, if the main thread is not reading them right now.
    std::unique_lock lock(loopsMutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    const int channel = engine.channel();  // the loops shown are the active channel's
    for (int s = 0; s < kLoopSourceCount; ++s) {
        LiveState::Loop& loop = loops_[static_cast<std::size_t>(s)];
        loop.status = engine.loopStatus(channel, s);
        const int length = loop.status.length;
        loop.points = s == static_cast<int>(LoopSource::Notes) ? 0 : std::min(length, LiveState::Loop::kPoints);
        for (int i = 0; i < loop.points; ++i) {
            const int at = loop.points > 1 ? static_cast<int>(static_cast<long long>(i) * (length - 1) / (loop.points - 1)) : 0;
            const float x = engine.loopSample(channel, s, 0, at), y = engine.loopSample(channel, s, 1, at);
            constexpr float scale = 255.0f;
            loop.on[static_cast<std::size_t>(i)] = x >= 0;
            loop.x[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(std::max(x, 0.0f) * scale));
            loop.y[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(std::max(y, 0.0f) * scale));
        }
    }
    noteLoopCount_ = engine.noteLoopIntervals(channel, noteLoop_.data(), LiveState::kMaxNoteLoopNotes);
}

void Runtime::run(std::stop_token stop, Options options, std::shared_ptr<const Preset> preset) {
#ifdef __linux__
    sched_param param{};
    param.sched_priority = 10;
    const bool realtime = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
    log_(realtime ? "Gamepad thread running at real-time priority."
                  : "Real-time priority refused (normal without rtprio rights): normal priority.");
#endif

    std::shared_ptr<MidiOutput> output;
    {
        std::lock_guard lock(pendingMutex_);
        output = std::move(pendingOutput_);
    }
    Tap tap(*this, std::move(output));  // the tap alone owns it, so a replaced output is closed
    Engine engine(tap);
    engine.seedRandom(SDL_GetTicksNS() ^ static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));
    engine.setPreset(std::move(preset));
    engine.setChannel(options.channel);
    engine.setObserver(&tap);
    GamepadInput gamepad(engine, log_);
    gamepad.setButtonTap([this](Button b, bool pressed) {
        if (!reportButtons_.load(std::memory_order_relaxed))
            return;
        Report r{Report::Kind::Button};
        r.button = b;
        r.pressed = pressed;
        report(r);
    });

    bool linkPlaying = false, linkPlayingKnown = false;  // the Link session's transport, as last seen
    double internalBeat = 0;                           // the internal clock's beat, at internalAt
    std::uint64_t internalAt = 0;
    const std::uint64_t period = 1'000'000'000ull / static_cast<std::uint64_t>(std::max(options.pollHz, 1));
    std::uint64_t next = SDL_GetTicksNS();
    while (!stop.stop_requested()) {
        const std::uint64_t start = SDL_GetTicksNS();
        storeMax(maxLateNs_, start > next ? start - next : 0);

        if (hasPending_.exchange(false, std::memory_order_acquire)) {
            std::shared_ptr<const Preset> newPreset;
            std::shared_ptr<MidiOutput> newOutput;
            {
                std::lock_guard lock(pendingMutex_);
                newPreset = std::move(pendingPreset_);
                newOutput = std::move(pendingOutput_);
            }
            if (newPreset)
                engine.setPreset(std::move(newPreset));
            if (newOutput) {
                engine.releaseAll();  // to the old output
                tap.setOutput(std::move(newOutput));
            }
        }
        while (auto command = commands_.pop()) {
            if (command->kind == Command::Kind::SetChannel) {
                engine.setChannel(command->channel);
            } else if (command->kind == Command::Kind::ClearLoop) {
                engine.clearLoop(engine.channel(), command->channel);  // `channel` holds the source
            } else if (command->kind == Command::Kind::StretchLoop) {
                engine.stretchLoop(engine.channel(), command->channel, command->value);  // `channel` holds the source
            } else if (command->kind == Command::Kind::AmplifyLoop) {
                engine.amplifyLoop(engine.channel(), command->channel, command->value);  // `channel` holds the source
            } else if (command->kind == Command::Kind::EditNotes) {
                std::lock_guard lock(sessionMutex_);
                if (!noteEdits_.empty()) {
                    const NoteEdit& edit = noteEdits_.front();
                    if (edit.remove)
                        engine.removeLoopNotes(engine.channel(), edit.notes);
                    else if (edit.resize)
                        engine.resizeLoopNotes(engine.channel(), edit.notes, edit.samples);
                    else
                        engine.moveLoopNotes(engine.channel(), edit.notes, edit.samples, edit.semitones, edit.byDegrees);
                    noteEdits_.erase(noteEdits_.begin());
                }
            } else if (command->kind == Command::Kind::SaveLoops) {
                std::lock_guard lock(sessionMutex_);
                if (loopsWanted_) {
                    loopsWanted_->set_value(engine.sessionLoops());
                    loopsWanted_.reset();
                }
            } else if (command->kind == Command::Kind::LoadLoops) {
                std::lock_guard lock(sessionMutex_);
                engine.loadSessionLoops(loopsToLoad_);
                loopsToLoad_.clear();
            } else if (command->kind == Command::Kind::PauseLoops || command->kind == Command::Kind::PlayLoops) {
                const bool play = command->kind == Command::Kind::PlayLoops;
                // The loops at once, whatever the session does: they play even
                // while it is stopped, so asking it to stop may change nothing.
                if (play)
                    engine.playLoops();
                else
                    engine.pauseLoops();
                if (clock_.load(std::memory_order_relaxed) == ClockMode::Link && startStopSync_.load(std::memory_order_relaxed)) {
                    // And the session, for the other apps; MidiPad is already there.
                    auto state = link_->link.captureAudioSessionState();
                    state.setIsPlaying(play, link_->link.clock().micros());
                    link_->link.commitAudioSessionState(state);
                    linkPlaying = play;
                    linkPlayingKnown = true;
                }
            } else {
                engine.releaseAll();
                engine.stopLoops();
            }
        }

        // The beat clock, from the Link session (its calls here are real-time safe).
        const int beatsPerBar = beatsPerBar_.load(std::memory_order_relaxed);
        engine.setSync(snapBeats_.load(std::memory_order_relaxed), beatsPerBar, quantize_.load(std::memory_order_relaxed));
        // A tempo asked for: from the window, or tapped on the controller.
        double tempoWanted = requestedTempo_.exchange(0, std::memory_order_relaxed);
        if (const auto tapped = engine.takeTappedTempo())
            tempoWanted = *tapped;
        const ClockMode mode = clock_.load(std::memory_order_relaxed);
        if (mode == ClockMode::Link) {
            auto state = link_->link.captureAudioSessionState();
            const auto time = link_->link.clock().micros();
            if (tempoWanted > 0) {
                state.setTempo(tempoWanted, time);
                link_->link.commitAudioSessionState(state);
            }
            const double tempo = state.tempo();
            const double beat = state.beatAtTime(time, beatsPerBar);
            engine.setBeatClock({true, tempo, beat});
            tempo_.store(tempo, std::memory_order_relaxed);
            beat_.store(beat, std::memory_order_relaxed);
            // Start / stop sync: the loops follow the session's transport.
            const bool playing = state.isPlaying();
            if (startStopSync_.load(std::memory_order_relaxed) && linkPlayingKnown && playing != linkPlaying) {
                if (playing)
                    engine.playLoops();
                else
                    engine.pauseLoops();
            }
            linkPlaying = playing;
            linkPlayingKnown = true;
        } else if (mode == ClockMode::Internal) {
            // MidiPad's own clock: its beat runs on at the internal tempo.
            if (tempoWanted > 0)
                internalTempo_.store(tempoWanted, std::memory_order_relaxed);
            const double tempo = internalTempo_.load(std::memory_order_relaxed);
            if (internalAt != 0 && start > internalAt)
                internalBeat += static_cast<double>(start - internalAt) * tempo / 60e9;
            engine.setBeatClock({true, tempo, internalBeat});
            tempo_.store(tempo, std::memory_order_relaxed);
            beat_.store(internalBeat, std::memory_order_relaxed);
            linkPlayingKnown = false;
        } else {
            engine.setBeatClock({});
            linkPlayingKnown = false;
        }
        internalAt = start;
        engine.setDpadSettle(dpadSettlePress_.load(std::memory_order_relaxed),
                             dpadSettleRelease_.load(std::memory_order_relaxed));
        engine.setHumanize(unpack(humanize_.load(std::memory_order_relaxed)));
        if (const int key = key_.load(std::memory_order_relaxed); key >= 0)
            engine.setKey(midipad::Key{key / 256, static_cast<std::size_t>(key % 256)});
        else
            engine.setKey(std::nullopt);
        engine.tick(start);
        gamepad.poll(start);
        publish(gamepad, engine);

        const std::uint64_t end = SDL_GetTicksNS();
        ticks_.fetch_add(1, std::memory_order_relaxed);
        totalWorkNs_.fetch_add(end - start, std::memory_order_relaxed);
        storeMax(maxWorkNs_, end - start);

        next += period;
        if (next <= end)
            next = end + period;  // fell behind: skip ahead instead of bursting
        SDL_DelayNS(next - end);
    }
    engine.releaseAll();
}
