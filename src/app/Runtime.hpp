#pragma once

#include "MidiOutput.hpp"

#include "midipad/Engine.hpp"
#include "midipad/SpscRing.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Where the beat comes from: none (free), MidiPad's own clock, or Ableton Link.
enum class ClockMode : std::uint8_t { Free, Internal, Link };

// Something the input thread reports to the main thread.
struct Report {
    enum class Kind : std::uint8_t { Midi, Channel, Button, Freeze } kind = Kind::Midi;
    midipad::MidiMessage message{};
    int channel = 0;
    midipad::Button button{};
    bool pressed = false;
    std::uint8_t frozenAxes = 0;
};

std::string describe(const Report& report);

// A note the engine sent, for the piano roll.
struct SentNote {
    std::uint64_t timeNs = 0;  // SDL_GetTicksNS()
    std::uint8_t channel = 0, note = 0, velocity = 0;  // velocity 0: released
    bool fromLoop = false;
};

// Controller and engine state, for display.
struct LiveState {
    bool connected = false;
    bool hasTouchpad = false;
    std::string gamepadName;
    std::uint32_t buttons = 0;  // bit per SDL gamepad button
    std::array<std::int16_t, midipad::kAxisCount> axes{};
    struct Finger {
        bool down = false;
        float x = 0, y = 0;
    };
    std::array<Finger, midipad::kFingerCount> fingers{};
    int channel = 0;
    std::uint8_t frozenAxes = 0;  // on the active channel
    // The active channel's loops (by LoopSource), for display. A movement
    // loop has a lane per axis (X, Y; L2, R2), each its own loop: its status,
    // its curve reduced to kPoints over its whole length, the samples of a
    // step when it plays in steps (0: not).
    struct Lane {
        static constexpr int kPoints = 160;
        midipad::LoopStatus status;
        int points = 0;                      // valid entries of v and on
        std::array<std::uint8_t, kPoints> v{};  // 0-255
        std::array<bool, kPoints> on{};         // false while a finger was lifted
        int stepSamples = 0;
    };
    struct Loop {
        static constexpr int kPoints = Lane::kPoints;
        // The note loop's; a movement loop's two lanes together: recording if
        // either records, else playing if either plays; waiting, locked if
        // either is; the longer length; X's position, stretch and amplitude.
        midipad::LoopStatus status;
        std::array<Lane, 2> lanes{};
    };
    std::array<Loop, midipad::kLoopSourceCount> loops{};
    // Shift + L3 / R3, Shift + the touchpad's click: the stick, the fingers
    // change lengths, amplitudes or offsets (Engine::stickShape).
    std::array<midipad::Engine::ShapeMode, 2> stickShapes{};
    midipad::Engine::ShapeMode fingerShape = midipad::Engine::ShapeMode::Length;
    // The active channel's note loop, as notes (in loop samples).
    static constexpr int kMaxNoteLoopNotes = 128;
    std::array<midipad::NoteInterval, kMaxNoteLoopNotes> noteLoop{};
    int noteLoopCount = 0;
    // Every channel's clips, for the session screen: what each holds and a
    // sketch of it (refreshed a few times a second), the current clip, the
    // one waiting to start (kClipSlots: a stop), the record slot, which
    // channels play and how far their clip is in its turn.
    struct Clip {
        static constexpr int kCurvePoints = 24, kNotes = 24;
        std::uint8_t sources = 0;  // a bit per LoopSource holding a loop
        bool recording = false;
        // Left stick, right stick, triggers, touchpad: the axis that moves most, 0-255.
        std::array<std::array<std::uint8_t, kCurvePoints>, 4> curves{};
        int noteCount = 0;
        std::array<std::array<std::uint8_t, 3>, kNotes> notes{};  // start, end (0-255 of the loop), pitch
    };
    std::array<Clip, midipad::kChannelCount * midipad::Engine::kClipSlots> clips{};
    std::array<std::int8_t, midipad::kChannelCount> currentClip{}, queuedClip{}, recordSlot{};
    std::uint16_t clipsPlaying = 0;
    std::uint16_t muted = 0, soloed = 0;  // Engine::setMuted, setSoloed: a bit per channel
    std::array<float, midipad::kChannelCount> clipProgress{};
    // A chain: its steps (clip -1: empty; repeats 0: forever) and what comes
    // after its last step (Engine::kThenLoop, kThenStop or another chain).
    struct Chain {
        std::array<std::int8_t, midipad::Engine::kChainSteps> clip = [] {
            std::array<std::int8_t, midipad::Engine::kChainSteps> empty{};
            empty.fill(-1);
            return empty;
        }();
        std::array<std::uint8_t, midipad::Engine::kChainSteps> repeats{};
        std::int8_t then = midipad::Engine::kThenLoop;
        bool empty() const {
            for (auto c : clip)
                if (c >= 0)
                    return false;
            return true;
        }
    };
    // Every channel's chains, the one running (-1: none), its step (-1: off),
    // the turns it played, the step (and chain) asked for next (-1: none).
    struct Chains {
        std::array<Chain, midipad::Engine::kChains> list{};
        std::int8_t playing = -1, at = -1, next = -1, nextChain = -1;
        std::int8_t clipNow = -1;  // the clip the step plays (picked, for a random one)
        int turns = 0;
    };
    std::array<Chains, midipad::kChannelCount> chains{};
    // The song: whether it plays (and loops its section), the section, the
    // beats into it (below 0: waiting for its bar); every clip's turn and
    // chain's pass, in beats (Engine::clipBeats, chainBeats; a bit per chain
    // in chainOpen: it has a forever step).
    struct Song {
        bool playing = false, loop = false;
        int section = -1;
        double beat = 0;
    } song;
    std::array<std::array<float, midipad::Engine::kClipSlots>, midipad::kChannelCount> clipBeats{};
    std::array<std::array<float, midipad::Engine::kChains>, midipad::kChannelCount> chainBeats{};
    std::array<std::uint8_t, midipad::kChannelCount> chainOpen{};
    std::uint64_t sounding = 0;   // Engine::soundingSlots()
    std::uint16_t loopChannels = 0;  // Engine::channelsWithLoops()
    bool loopsPaused = false;
    bool stretchHeld = false;  // Shift is on (held or latched)
    // The Velocity and Transpose axes on the active channel (Engine::shaping).
    float velocityShaping = 0;
    float velocityLoop = 0;  // the same on the note loop's notes
    int transposeNotes = 0, transposeLoop = 0;
    // The beat clock: which one, its tempo, the beat now, how many other apps
    // take part (Ableton Link); the beats of a bar MidiPad counts.
    ClockMode clock = ClockMode::Free;
    double tempo = 120, beat = 0;
    int linkPeers = 0, beatsPerBar = 4;

    bool isDown(midipad::Button b) const { return (buttons >> static_cast<int>(b)) & 1u; }
    bool isSounding(int slot) const { return (sounding >> slot) & 1u; }
};

struct RuntimeStats {
    std::uint64_t ticks = 0;
    double averageWorkUs = 0, maxWorkUs = 0, maxLateUs = 0;
    std::uint64_t droppedReports = 0;
    std::uint64_t sendErrors = 0;
};

// Owns the real-time input thread: it polls the gamepad, runs the engine and
// sends MIDI. Every method is called from the main thread and never waits for
// the input thread; changes reach it on its next poll.
//
// SDL must be initialized with SDL_INIT_GAMEPAD and gamepad/joystick events
// disabled before a Runtime is created.
class Runtime {
public:
    struct Options {
        int pollHz = 1000;
        int channel = 0;
        bool reportMidi = false;
        bool reportButtons = false;
        int dpadSettlePressMs = 15, dpadSettleReleaseMs = 15;  // see Engine::setDpadSettle
        midipad::Humanize humanize;
    };
    // Called from the input thread, only on rare events (gamepad connected...).
    using Log = std::function<void(const std::string&)>;

    Runtime(std::shared_ptr<MidiOutput> output, std::shared_ptr<const midipad::Preset> preset, Options options,
            Log log);
    ~Runtime();  // turns every note off, then stops the thread
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void setPreset(std::shared_ptr<const midipad::Preset> preset);
    // Notes still on are turned off on the previous output first.
    void setOutput(std::shared_ptr<MidiOutput> output);
    void setChannel(int channel);
    void panic();  // also stops the loops, on every channel
    void clearLoop(int source, int axis = -1);  // a LoopSource (one axis, or -1 both), on the active channel
    void toggleLoopLock(int source, int axis = -1);  // see Engine::toggleLoopLock, on the active channel; -1: both axes
    // Clips (Engine::launchClip, stopClip, setRecordSlot), on any channel.
    void launchClip(int channel, int slot);
    void stopClip(int channel) { launchClip(channel, midipad::Engine::kClipSlots); }
    void setRecordSlot(int channel, int slot);
    // The session screen from the controller (Engine::setSessionMode); what
    // the controller asks of it, oldest first.
    void setSessionMode(bool on) { sessionMode_.store(on, std::memory_order_relaxed); }
    void setSessionValues(bool on) { sessionValues_.store(on, std::memory_order_relaxed); }  // Engine::setSessionValues
    // Engine::setNavButtons; -1: none.
    void setNavButtons(int select, int menu, int back) {
        navButtons_.store((select + 1) | (menu + 1) << 8 | (back + 1) << 16, std::memory_order_relaxed);
    }
    // Engine::clearClip, copyClip, resetLoopShape (on the active channel).
    void clearClip(int channel, int slot);
    void copyClip(int channel, int from, int to);
    void resetLoopShape(int source, int axis);
    // Chains (Engine::setChainStep, setChainThen, chainGoTo, stopChain), on any channel.
    void setChainStep(int channel, int chain, int step, int clip, int repeats);
    void setChainThen(int channel, int chain, int then);
    void chainGoTo(int channel, int chain, int step);
    void stopChain(int channel);
    std::optional<midipad::Engine::SessionEvent> popSessionEvent() { return sessionEvents_.pop(); }
    // Editing the active channel's note loop (Engine::moveLoopNotes,
    // removeLoopNotes), in the order asked.
    void moveLoopNotes(std::vector<midipad::LoopNoteRef> notes, int samples, int semitones, bool byDegrees = false);
    void removeLoopNotes(std::vector<midipad::LoopNoteRef> notes);
    void resizeLoopNotes(std::vector<midipad::LoopNoteRef> notes, int samples);
    void setNoteLoopLength(int samples);  // Engine::setNoteLoopLength, in the same order
    // Session files: the loops of every channel as the input thread has them
    // (nothing if it does not answer within a second), and loops to play
    // instead of all of them, after any preset given before.
    std::optional<std::vector<midipad::SessionLoop>> sessionLoops();
    void loadSessionLoops(std::vector<midipad::SessionLoop> loops);
    void loadSessionChains(std::vector<midipad::SessionChain> chains);  // Engine::loadSessionChains
    // The song (Engine::setSong, playSong, stopSong, setSongLoop).
    void setSong(midipad::Song song);
    void playSong(int section);
    void stopSong();
    void setSongLoop(bool on);
    void setMuted(int channel, bool on);  // Engine::setMuted
    void setSoloed(int channel, bool on);  // Engine::setSoloed
    void setMuteSolo(std::uint16_t muted, std::uint16_t soloed);  // Engine::setMuteSolo
    // See Engine::stretchLoop, amplifyLoop and setAxisLoopLength, on the
    // active channel; axis -1: both.
    void stretchLoop(int source, int axis, int steps);
    void amplifyLoop(int source, int axis, int steps);
    // Engine::setModulation, setModulationSendsCc and offsetLoop, on the active channel (offsetLoop: axis -1, both).
    void setModulation(int source, int axis, const midipad::LoopModulation& modulation);
    void setModulationSendsCc(int source, int axis, bool sends);
    void offsetLoop(int source, int axis, int steps);
    void setAxisLoopLength(int source, int axis, int length);
    // The transport (Engine::transportPlay, transportStop): Play the song
    // from `section` (>= 0) or the clips (-1). setTransportSong: what the
    // Play of a Link session starting plays (the song screen's section, -1 the clips).
    void transportPlay(int section);
    void transportStop();
    void setTransportSong(int section) { transportSong_.store(section, std::memory_order_relaxed); }
    void setReportMidi(bool on) { reportMidi_.store(on, std::memory_order_relaxed); }
    void setReportButtons(bool on) { reportButtons_.store(on, std::memory_order_relaxed); }
    void setDpadSettle(int pressMs, int releaseMs) {
        dpadSettlePress_.store(pressMs, std::memory_order_relaxed);
        dpadSettleRelease_.store(releaseMs, std::memory_order_relaxed);
    }
    void setHumanize(const midipad::Humanize& humanize) { humanize_.store(pack(humanize), std::memory_order_relaxed); }
    // The tonality Transpose axes move in (Engine::setKey).
    void setKey(const std::optional<midipad::Key>& key) {
        key_.store(key ? key->root * 256 + static_cast<int>(key->scale) : -1, std::memory_order_relaxed);
    }

    // The beat clock. Free, MidiPad keeps no beat. Internal, its own clock
    // runs at the internal tempo. Link, it joins the Ableton Link session on
    // the local network. With a clock, loops recorded snap to `snapBeats` (0:
    // free) and play from its beat, their notes on a grid of `quantize` steps
    // a beat (0: off); with Link and start / stop sync, Play and Stop follow
    // the session and lead it.
    void setClock(ClockMode mode);
    // The tempo of the clock running: the internal one's, or the Link
    // session's (every app in it follows). A TapTempo binding sets it too.
    void setTempo(double bpm);
    void setInternalTempo(double bpm) { internalTempo_.store(bpm, std::memory_order_relaxed); }
    // With the internal clock, MIDI Clock out (24 pulses a beat) for devices
    // to follow, Start with Play (from the beat's start) and Stop with Stop.
    void setMidiClockOut(bool on) { midiClockOut_.store(on, std::memory_order_relaxed); }
    void setLinkStartStopSync(bool on);
    void setSync(int snapBeats, int beatsPerBar, int quantize) {
        snapBeats_.store(snapBeats, std::memory_order_relaxed);
        beatsPerBar_.store(beatsPerBar, std::memory_order_relaxed);
        quantize_.store(quantize, std::memory_order_relaxed);
    }

    std::optional<Report> popReport() { return reports_.pop(); }
    std::optional<SentNote> popNote() { return notes_.pop(); }
    LiveState live() const;
    RuntimeStats stats() const;

private:
    class Tap;
    struct Command {
        enum class Kind : std::uint8_t {
            SetChannel, Panic, ClearLoop, ToggleLock, ChainStep, ChainThen, ChainGo, ChainStop, ClearClip, CopyClip, ResetShape, LaunchClip, RecordSlot, StretchLoop, AmplifyLoop, AxisLength, PauseLoops, PlayLoops,
            SaveLoops, LoadLoops, LoadChains, LoadSong, SongPlay, SongStop, SongLoop, Mute, Solo, MuteSolo, Modulation, ModulationCc, OffsetLoop, EditNotes
        } kind;
        int channel = 0;
        int value = 0;
        int axis = -1;
        int extra = 0;
        int chain = 0;  // ChainStep, ChainThen, ChainGo
    };

    void run(std::stop_token stop, Options options, std::shared_ptr<const midipad::Preset> preset);
    // Humanize in one atomic word: channels, velocity and delay bounds.
    static std::uint64_t pack(const midipad::Humanize& h);
    static midipad::Humanize unpack(std::uint64_t word);
    void report(const Report& r);
    void publish(const class GamepadInput& gamepad, const midipad::Engine& engine);

    Log log_;
    std::atomic<bool> reportMidi_, reportButtons_;
    std::atomic<int> dpadSettlePress_, dpadSettleRelease_;
    std::atomic<std::uint64_t> humanize_;
    std::shared_ptr<MidiOutput> output_;  // main thread's view, for stats

    midipad::SpscRing<Report, 4096> reports_;
    midipad::SpscRing<SentNote, 2048> notes_;
    std::atomic<std::uint64_t> droppedReports_{0};
    midipad::SpscRing<Command, 64> commands_;
    midipad::SpscRing<midipad::Engine::SessionEvent, 64> sessionEvents_;
    std::atomic<bool> sessionMode_{false}, sessionValues_{false};
    std::atomic<int> navButtons_{0};

    std::mutex pendingMutex_;
    std::shared_ptr<const midipad::Preset> pendingPreset_;
    std::shared_ptr<MidiOutput> pendingOutput_;
    std::atomic<bool> hasPending_{false};
    std::mutex sessionMutex_;  // the loops going to and from a session file
    std::optional<std::promise<std::vector<midipad::SessionLoop>>> loopsWanted_;
    std::vector<midipad::SessionLoop> loopsToLoad_;
    std::vector<midipad::SessionChain> chainsToLoad_;
    std::atomic<int> transportSong_{-1};
    std::optional<midipad::Song> songToLoad_;
    struct NoteEdit {
        std::vector<midipad::LoopNoteRef> notes;
        bool remove = false;
        int samples = 0, semitones = 0;
        bool byDegrees = false;
        bool resize = false;  // `samples` lengthens them
        int loopLength = 0;   // > 0: the loop's new length instead
    };
    std::vector<NoteEdit> noteEdits_;  // under sessionMutex_, oldest first

    // Published by the input thread after each poll.
    std::atomic<bool> connected_{false}, hasTouchpad_{false};
    std::atomic<std::uint32_t> buttons_{0};
    std::array<std::atomic<std::int16_t>, midipad::kAxisCount> axes_{};
    std::array<std::atomic<bool>, midipad::kFingerCount> fingerDown_{};
    std::array<std::atomic<float>, midipad::kFingerCount> fingerX_{}, fingerY_{};
    std::atomic<int> channel_{0};
    std::atomic<std::uint8_t> frozen_{0};
    std::atomic<std::uint64_t> sounding_{0};
    std::atomic<std::uint16_t> loopChannels_{0};
    std::atomic<bool> loopsPaused_{false};
    std::atomic<std::uint16_t> muted_{0}, soloed_{0};
    std::atomic<bool> stretchHeld_{false};
    std::atomic<std::uint8_t> shapes_{0};  // two bits each (Engine::ShapeMode): left stick, right stick, fingers
    std::atomic<int> key_{-1};  // root * 256 + scale, -1: none
    struct LinkSession;  // ableton::Link, kept out of this header
    std::unique_ptr<LinkSession> link_;
    std::atomic<ClockMode> clock_{ClockMode::Free};
    std::atomic<bool> startStopSync_{true};
    std::atomic<bool> midiClockOut_{true};
    std::atomic<double> internalTempo_{120}, requestedTempo_{0};  // 0: no tempo asked for
    std::atomic<int> snapBeats_{1}, beatsPerBar_{4}, quantize_{0};
    std::atomic<double> tempo_{120}, beat_{0};
    std::atomic<float> velocityShaping_{0}, velocityLoop_{0};
    std::atomic<int> transposeNotes_{0}, transposeLoop_{0};
    mutable std::mutex loopsMutex_;  // the input thread only try_locks it
    std::array<LiveState::Loop, midipad::kLoopSourceCount> loops_{};
    std::array<midipad::NoteInterval, LiveState::kMaxNoteLoopNotes> noteLoop_{};
    int noteLoopCount_ = 0;
    std::array<LiveState::Clip, midipad::kChannelCount * midipad::Engine::kClipSlots> clips_{};
    std::array<std::int8_t, midipad::kChannelCount> currentClip_{}, queuedClip_{}, recordSlot_{};
    std::uint16_t clipsPlaying_ = 0;
    std::array<float, midipad::kChannelCount> clipProgress_{};
    std::array<LiveState::Chains, midipad::kChannelCount> chains_{};
    LiveState::Song song_{};
    std::array<std::array<float, midipad::Engine::kClipSlots>, midipad::kChannelCount> clipBeats_{};
    std::array<std::array<float, midipad::Engine::kChains>, midipad::kChannelCount> chainBeats_{};
    std::array<std::uint8_t, midipad::kChannelCount> chainOpen_{};
    int clipSketchCountdown_ = 0;  // polls until the clips' sketches are drawn again
    mutable std::mutex nameMutex_;
    std::string gamepadName_;

    std::atomic<std::uint64_t> ticks_{0}, totalWorkNs_{0}, maxWorkNs_{0}, maxLateNs_{0};

    std::jthread thread_;  // last: stops before the members above go away
};
