#pragma once

#include "midipad/Humanize.hpp"
#include "midipad/MidiMessage.hpp"
#include "midipad/Preset.hpp"
#include "midipad/Session.hpp"
#include "midipad/TapTempo.hpp"
#include "midipad/Scales.hpp"

#include <array>
#include <bitset>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace midipad {

class MidiSink {
public:
    virtual ~MidiSink() = default;
    virtual void send(const MidiMessage& message) = 0;
};

class EngineObserver {
public:
    virtual ~EngineObserver() = default;
    virtual void channelChanged(int /*channel*/) {}
    // One bit per Axis that is now frozen on `channel`.
    virtual void freezeChanged(int /*channel*/, std::uint8_t /*frozenAxes*/) {}
    // A note sent (velocity 0: its release), by a loop or not.
    virtual void noteSent(int /*channel*/, int /*note*/, int /*velocity*/, bool /*fromLoop*/) {}
};

// One note of a note loop, in loop samples (Engine::kLoopRateHz).
struct NoteInterval {
    std::int32_t start = 0, end = 0;
    std::uint8_t note = 0, velocity = 0;
    bool open = false;  // not released yet (being recorded): `end` is the loop's end
    std::uint32_t press = 0;  // the press it came from: a chord's notes share it (0: unknown)
};

// A note of a note loop: the one starting at sample `start` with pitch `note`.
struct LoopNoteRef {
    std::int32_t start = 0;
    std::uint8_t note = 0;
};

// What a loop is doing, for display.
struct LoopStatus {
    enum class State : std::uint8_t { Empty, Recording, Playing } state = State::Empty;
    bool waiting = false;  // Record held, this source has not moved yet
    bool overdub = false;  // a note loop taking the notes played on top of it
    int length = 0;        // samples, at Engine::kLoopRateHz
    int position = 0;      // sample playing now
    int direction = 1;     // 1 forwards, -1 backwards
    int channel = 0;
    int stretch = 0;       // movement loops: see Engine::stretchFactor
    int amplitude = 100;   // movement loops: percent of the recorded swing, around its middle
    double beats = 0;      // synced to the beat clock: its length in beats (stretched); 0: free
    bool locked = false;   // see Engine::toggleLoopLock
};

// Turns controller events into MIDI according to the active preset.
//
// Not thread-safe: own it from the input thread and hand it presets from
// there. No call allocates except setPreset().
//
// Every note or CC a press starts is recorded with the channel it went out on,
// so its release always turns off exactly what was sent, even if the channel
// or the preset changed in between.
class Engine {
public:
    // Discrete controls share one index space: buttons, then D-pad directions,
    // then touchpad zones per finger.
    static constexpr int kHatSlotBase = kButtonCount;
    static constexpr int kZoneSlotBase = kHatSlotBase + kHatDirCount;
    static constexpr int kSlotCount = kZoneSlotBase + kFingerCount * kZoneCount;
    static_assert(kSlotCount <= 64, "soundingSlots() packs the slots in 64 bits");

    explicit Engine(MidiSink& sink);

    void setPreset(std::shared_ptr<const Preset> preset);
    const Preset& preset() const { return *preset_; }

    void setChannel(int channel);  // 0-15
    int channel() const { return channel_; }

    void setObserver(EngineObserver* observer) { observer_ = observer; }

    // A new D-pad direction plays only once it has held for this long: pressing
    // or releasing two buttons of a diagonal is never simultaneous, and the
    // direction in between would otherwise sound. `press` applies when a
    // button went down, `release` when one went up; releasing the whole D-pad
    // is immediate. 0 (the default) plays every direction at once.
    void setDpadSettle(int pressMs, int releaseMs);
    // The time, from any steady clock; call it on every poll, before the events.
    // Also sends the humanized notes that are due.
    void tick(std::uint64_t nowNs);

    void setHumanize(const Humanize& humanize) { humanize_ = humanize; }
    // Sync. The beat clock of a Link session, given before each tick: whether
    // it runs, its tempo (beats a minute) and the beat at that tick's time.
    struct BeatClock {
        bool on = false;
        double tempo = 120;
        double beat = 0;
    };
    void setBeatClock(const BeatClock& clock) { clock_ = clock; }
    // A TapTempo binding tapped in time: the tempo it asks for, once.
    std::optional<double> takeTappedTempo() { return std::exchange(tappedTempo_, std::nullopt); }
    // Loops recorded while the clock runs snap to `snapBeats` (0: they stay
    // free): their length is rounded to whole snaps (at least one), their start
    // to the nearest snap; they then play from the clock, so they follow its
    // tempo and stay in phase with the session. Free loops play as recorded.
    // `quantize` (grid steps a beat: 2 eighth notes, 4 sixteenths, 8
    // thirty-seconds; 0 off) puts the notes recorded into a synced note loop,
    // and those overdubbed onto it, on that grid: each press moves as a whole,
    // its first note on the nearest step, its other notes (a humanized chord)
    // and its releases by as much. What sounds live is never delayed.
    void setSync(int snapBeats, int beatsPerBar, int quantize = 0) {
        snapBeats_ = std::max(snapBeats, 0);
        beatsPerBar_ = std::max(beatsPerBar, 1);
        quantize_ = std::max(quantize, 0);
    }
    int beatsPerBar() const { return beatsPerBar_; }

    // The tonality Transpose axes move in, by degrees; none: by semitones.
    void setKey(const std::optional<Key>& key) { key_ = key; }

    // What the Velocity and Transpose axes do on a channel now, live and from
    // the loops of their sources: the velocity in [-1, 1] of the way to 127 or
    // to 1, and the transpositions in steps (degrees of the key, or semitones).
    struct Shaping {
        float velocity = 0;
        int transposeNotes = 0, transposeLoop = 0;
    };
    Shaping shaping(int channel) const;
    // `note` moved by `steps`: degrees of the key (a note outside it first goes
    // to the nearest one of the key), semitones without a key; kept in range.
    int transposed(int note, int steps) const { return transposedIn(key_, note, steps); }
    static int transposedIn(const std::optional<Key>& key, int note, int steps);
    // How many steps a Transpose axis may go for `semitones`.
    int transposeSteps(int semitones) const;

    // Loops, one per LoopSource and channel. A LoopRecord binding held records,
    // on the active channel, each stick, touchpad finger (axis mode) or the
    // triggers that move, X and Y, into their loops (when a finger is lifted
    // nothing is recorded nor, later, played), and the notes played (buttons,
    // D-pad, triggers as buttons, touchpad pads) into the note loop, which cuts
    // what still sounds when it starts over. On release each recording plays
    // in a loop: on its own CCs (Mapping::loops) beside its source, or the
    // notes. Every channel's loops play at once.
    // A press where nothing moves: short (under kLoopTapMs) while the active
    // channel's note loop plays, it turns overdub on or off: the notes then
    // played (buttons, D-pad, triggers, pads) join the loop where it plays;
    // otherwise it stops the active channel's loops.
    // While Shift is on, Record held and a stick, the triggers or a finger
    // (axis mode) moving locks or unlocks that loop instead (once a hold),
    // and two taps within kDoubleTapMs lock or unlock the note loop: the first
    // tap's overdub is then taken back, and its stop waits that long.
    static constexpr int kLoopRateHz = 200;
    static constexpr int kMaxLoopSamples = 60 * kLoopRateHz;
    static constexpr float kLoopMoveThreshold = 0.06f;  // of the [0, 1] travel
    static constexpr int kLoopTapMs = 500;
    static constexpr int kDoubleTapMs = 350;
    // Movement loops keep their two axes apart: a stick's left-right (X) and
    // up-down (Y), a finger's, L2 (X) and R2 (Y). Each axis records when it
    // moves (an axis not enabled never does) and has its own length, stretch,
    // amplitude and sync; `axis` is 0 for X, 1 for Y. The note loop has one.
    LoopStatus loopStatus(int channel, int source, int axis = 0) const;
    // Clips: each channel keeps kClipSlots clips, each a full set of loops
    // (notes, sticks, triggers, fingers). A channel plays one clip at a time,
    // independently of the others; the loops above are those of its current
    // clip, which Record records into. launchClip, while the current clip
    // plays, waits for the end of its turn (its note loop's, else its longest
    // movement loop's; on the beat clock, its beats) and then starts the clip
    // from its start (on the beat); with nothing playing it starts at once.
    // Launching an empty slot, or stopClip, stops the channel there, its
    // clips kept. setRecordSlot: the next recording on that channel goes
    // into this slot, a new clip when it is empty; the current clip stops
    // when that recording starts.
    static constexpr int kClipSlots = 8;
    // A clip's tracks: each movement source's X and Y (source * 2 + axis), then the note loop.
    static constexpr int kTrackCount = 11, kNoteTrack = 10;
    static constexpr int trackOf(int source, int axis) {
        return source == static_cast<int>(LoopSource::Notes) ? kNoteTrack : source * 2 + (axis & 1);
    }
    static constexpr int sourceOfTrack(int track) { return track == kNoteTrack ? static_cast<int>(LoopSource::Notes) : track / 2; }
    static constexpr int axisOfTrack(int track) { return track == kNoteTrack ? 0 : track % 2; }
    int currentClip(int channel) const { return currentSlot_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }
    bool clipPlaying(int channel) const { return clipPlaying_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }
    int queuedClip(int channel) const { return queued_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }  // -1 none, kClipSlots a stop
    bool clipHasLoops(int channel, int slot) const;
    LoopStatus clipLoopStatus(int channel, int slot, int source, int axis = 0) const;
    float clipSample(int channel, int slot, int source, int axis, int index) const;  // as loopSample
    int clipNoteIntervals(int channel, int slot, NoteInterval* out, int max) const;  // as noteLoopIntervals
    float clipProgress(int channel) const;  // how far the playing clip is in its turn, 0-1; -1: none plays
    // Chains: each channel can play its clips in a sequence of kChainSteps
    // steps, each a clip played `repeats` turns (0: until asked to go on),
    // then the next step; empty steps (no clip, or an empty one) are skipped.
    // After the last step it goes back to the first, or with loop off stops
    // the channel. chainGoTo starts the chain from a step (at the end of the
    // playing clip's turn; at once with nothing playing) or, while it runs,
    // goes to that step at the end of the turn. Launching or stopping a clip
    // (launchClip, stopClip), recording into another clip, PANIC and
    // stopChain turn the chain off; its steps stay.
    // A step's clip may be kChainRandom: each time the chain comes to that
    // step it picks one of the channel's clips that hold loops, at random,
    // and plays it its repeats.
    static constexpr int kChainSteps = 8, kMaxRepeats = 64, kChainRandom = kClipSlots;
    struct ChainStep {
        int clip = -1;    // -1: empty; kChainRandom: any
        int repeats = 1;  // 1 to kMaxRepeats; 0: forever
        friend bool operator==(const ChainStep&, const ChainStep&) = default;
    };
    void setChainStep(int channel, int step, ChainStep s);
    ChainStep chainStep(int channel, int step) const;
    void setChainLoop(int channel, bool on);
    bool chainLoop(int channel) const { return chainLoop_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }
    void chainGoTo(int channel, int step);
    void stopChain(int channel);
    int chainAt(int channel) const { return chainAt_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }  // -1: off
    int chainTurns(int channel) const { return chainTurns_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }  // of the step, done
    int chainNext(int channel) const { return chainJump_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }  // asked for, -1 none
    int chainClip(int channel) const { return chainClip_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }  // the step's clip (picked, for a random one)
    void launchClip(int channel, int slot);
    void stopClip(int channel) { launchClip(channel, kClipSlots); }
    void setRecordSlot(int channel, int slot);
    int recordSlot(int channel) const { return recordSlot_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))]; }
    // The session screen, from the controller. A SessionScreen binding (channel
    // 1's works on every channel; without one in the preset, PS / Guide) asks
    // to open or close it. While setSessionMode(true), both sticks and the
    // D-pad move its cursor (a step a push, repeating while held) and L3 or R3
    // presses the clip under it; meanwhile they send, play and record nothing,
    // except while Record is held: then they play and record as usual (a stick
    // moves the cursor again once back at rest). What is asked comes out of
    // takeSessionEvent(), oldest first.
    // With setSessionValues(true) (the cursor in a chain), the right stick
    // asks for ValueUp... instead of moving the cursor, and R3 for PressRight.
    enum class SessionEvent : std::uint8_t { Toggle, Up, Down, Left, Right, Press, ValueUp, ValueDown, ValueLeft, ValueRight, PressRight };
    void setSessionMode(bool on) { sessionMode_ = on; }
    bool sessionMode() const { return sessionMode_; }
    void setSessionValues(bool on) { sessionValues_ = on; }
    std::optional<SessionEvent> takeSessionEvent();
    static constexpr float kNavPush = 0.6f, kNavRest = 0.35f;  // of the stick's half travel
    static constexpr int kNavRepeatDelayMs = 400, kNavRepeatMs = 150;
    // [0, 1]; axis 0: X, 1: Y (a finger's Y grows downwards). Negative while a
    // finger was lifted.
    float loopSample(int channel, int source, int axis, int index) const;
    void clearLoop(int channel, int source);  // both axes; also unlocks it
    void clearLoop(int channel, int source, int axis);  // one axis of a movement loop
    // A locked loop stays: Record neither replaces it nor stops it, nor
    // overdubs a locked note loop. Clearing it (clearLoop, PANIC) unlocks it.
    // Each axis loop locks on its own: toggleLoopLock with an axis locks or
    // unlocks that one; without, the source's (both axes: unlocked if either
    // is locked, else those that play). Only a loop that plays can be locked;
    // false when there is none.
    bool loopLocked(int channel, int source) const;  // either axis
    bool toggleLoopLock(int channel, int source);
    bool toggleLoopLock(int channel, int source, int axis);
    // A movement loop (not the note loop) played slower or faster, in steps
    // of a half: stretch > 0 lasts 1 + stretch / 2 times as long (up to 100),
    // stretch < 0 1 + |stretch| / 2 times less (down to 20). It plays on from
    // the same point of the loop, smoothed: between samples when slower,
    // averaged over the samples it skips when faster. Recording a loop again
    // puts it back to 0. Moving across 0 stops there, so 1x is easy to reach.
    static constexpr int kMaxStretch = 198, kMinStretch = -38;
    static double stretchFactor(int stretch);  // how many times longer it lasts
    void stretchLoop(int channel, int source, int axis, int steps);
    void stretchLoop(int channel, int source, int steps) {  // both axes
        stretchLoop(channel, source, 0, steps);
        stretchLoop(channel, source, 1, steps);
    }
    // A movement loop's swing narrowed or widened around its middle (halfway
    // between its lowest and highest values): from 0 % (still) through 100 % (as
    // recorded) up to kMaxAmplitude, by kAmplitudeStep percent a step below 100 %
    // and kAmplitudeStepAbove above. Widened, what would pass the ends of the
    // travel is softly clipped: it bends towards them, then stays flat there
    // (see shapeAmplitude). Eased in, so it never jumps. Moving across 100 %
    // stops there. Recording the loop again puts it back to 100 %.
    static constexpr int kAmplitudeStep = 5, kAmplitudeStepAbove = 25, kMaxAmplitude = 800;
    void amplifyLoop(int channel, int source, int axis, int steps);
    void amplifyLoop(int channel, int source, int steps) {  // both axes
        amplifyLoop(channel, source, 0, steps);
        amplifyLoop(channel, source, 1, steps);
    }
    // One axis of a movement loop shortened or lengthened to `length` samples
    // (2 up to kMaxLoopSamples): longer, the time added goes in a straight line
    // from its last value back to its first, so it closes without a jump. A
    // synced one keeps whole beats. False without that loop playing.
    bool setAxisLoopLength(int channel, int source, int axis, int length);
    // An axis playing in steps (LoopBinding::steps): the samples of a step; 0 when not.
    int loopStepSamples(int channel, int source, int axis) const;
    // Shift + a stick's click (L3, R3) or the touchpad's: the stick, or the
    // fingers, change the loops' amplitudes instead of their lengths (again: back).
    bool stickAmplitudes(int stick) const { return stickAmplitudes_[static_cast<std::size_t>(stick & 1)]; }
    bool fingerAmplitudes() const { return fingerAmplitudes_; }
    // One recorded value `v` in [0, 1] with the amplitude `a` (1 = as recorded)
    // around `middle`, softly clipped to [0, 1] when widened. Shared with the
    // view, which draws what plays.
    static float shapeAmplitude(float v, float middle, float a);
    // While Shift is on, a stick or a finger on the touchpad (in axis mode)
    // changes its loops on the active channel: along X its X loop, along Y its
    // Y loop; their stretch, as Shift + wheel does, or (after a click of the
    // stick, or of the touchpad) their amplitude, as Ctrl + wheel does (up
    // widens). Only the way it mostly moves counts. A stick works as a
    // wheel kept turning, faster the further it is pushed (kStretchMinRate to
    // kStretchMaxRate stretch steps, kAmplitudeMaxRate amplitude steps a second
    // beyond kStretchDeadZone); a finger counts a step every kStretchTouchStep
    // of its slide. Meanwhile they send no CC and start no recording. Reaching
    // 1x the stretch waits kStretchPauseMs before going on.
    static constexpr float kStretchDeadZone = 0.25f;       // of the stick's half travel
    static constexpr double kStretchMinRate = 2, kStretchMaxRate = 40;
    static constexpr float kStretchTouchStep = 0.03f;      // of the touchpad's width
    static constexpr int kStretchPauseMs = 400;
    static constexpr double kAmplitudeMaxRate = 20;
    // A Shift binding held (momentary) or latched (toggle): the Shift layer
    // plays, and the sticks and fingers stretch their loops.
    bool stretchHeld() const { return stretchHolders_.any() || shiftLatched_; }
    bool shiftActive() const { return stretchHeld(); }
    // The notes of a channel's note loop, at most `max`; returns how many.
    int noteLoopIntervals(int channel, NoteInterval* out, int max) const;
    // Editing a note loop: notes moved together by `samples` and `semitones`
    // (their lengths and the gaps between them kept, the group inside the loop
    // and the MIDI range, as a block), or removed. Notes they then cut short
    // or sounding now stop. False when none of them is there. `byDegrees`:
    // `semitones` counts degrees of the tonality instead (see transposed), so
    // the notes stay on the scale.
    bool moveLoopNotes(int channel, const std::vector<LoopNoteRef>& notes, int samples, int semitones, bool byDegrees = false);
    bool removeLoopNotes(int channel, const std::vector<LoopNoteRef>& notes);
    // Notes lengthened (or shortened) together by `samples`: each keeps one
    // sample at least, ends inside the loop, and stops where the next note of
    // its pitch starts. Their start and pitch stay. False when none is there.
    bool resizeLoopNotes(int channel, const std::vector<LoopNoteRef>& notes, int samples);
    // The note loop shortened or lengthened to `length` samples (2 up to
    // kMaxLoopSamples): the notes starting at or past its new end go, those
    // still on there end with it, what sounds stops. A synced loop keeps whole
    // beats: `length` is rounded to them (one at least) and its beats follow.
    // False without a note loop playing.
    bool setNoteLoopLength(int channel, int length);
    // One note: moved to `newStart` and `newNote`, or removed.
    bool moveLoopNote(int channel, int start, int note, int newStart, int newNote) {
        return moveLoopNotes(channel, {{start, static_cast<std::uint8_t>(note)}}, newStart - start, newNote - note);
    }
    bool removeLoopNote(int channel, int start, int note) {
        return removeLoopNotes(channel, {{start, static_cast<std::uint8_t>(note)}});
    }
    static constexpr int kMaxNoteEvents = 4096;  // per note loop
    void stopLoops(int channel);  // but the locked ones
    // A bit per channel where a loop plays or records.
    std::uint16_t channelsWithLoops() const;
    void stopLoops();  // every channel's current clip, the locked loops too (PANIC)
    // Pauses every channel's loops, keeping them (what they have on stops), or
    // plays them all again from their start. A loop that starts recording
    // while they are paused plays them all again.
    void pauseLoops();
    void playLoops();
    bool loopsPaused() const { return loopsPaused_; }
    // Session files. The loops of every channel that play, as they are (their
    // stretch, amplitude and sync included), and back: loading one replaces
    // every loop and plays them (synced ones on the beat clock again).
    // Both allocate.
    std::vector<SessionLoop> sessionLoops() const;
    void loadSessionLoops(const std::vector<SessionLoop>& loops);
    void seedRandom(std::uint64_t seed) { random_ = seed ? seed : 0x9E3779B97F4A7C15ull; }

    void buttonChanged(Button button, bool pressed);
    // Sticks in [-1, 1] (SDL convention: up is negative), triggers in [0, 1].
    void axisMoved(Axis axis, float value);
    // Normalized position, origin at the top-left corner of the touchpad.
    void touchMoved(int finger, float x, float y);
    void touchReleased(int finger);

    // Turns off every note and CC still on, latched or held, at once: humanized
    // notes not started yet are dropped.
    void releaseAll();
    // releaseAll(), and forgets which controls are held: for a disconnected controller.
    void reset();

    bool isFrozen(Axis axis) const;

    // Discrete controls share one index space (see slotOf). Bit `slot` is set
    // while that control has a note, chord or CC on: held, or latched on the
    // active channel. For display.
    std::uint64_t soundingSlots() const;
    static constexpr int slotOf(Button b) { return static_cast<int>(b); }
    static constexpr int slotOf(HatDir d) { return kHatSlotBase + static_cast<int>(d); }
    static constexpr int slotOf(int finger, Zone z) { return kZoneSlotBase + finger * kZoneCount + static_cast<int>(z); }

    // Digital trigger thresholds on the [0, 1] trigger travel (with hysteresis).
    static constexpr float kTriggerPress = 0.25f;
    static constexpr float kTriggerRelease = 0.15f;

private:

    struct Sounding {
        ActionType type = ActionType::None;  // Note, Chord or ControlChange
        std::uint8_t channel = 0;
        std::uint8_t velocity = 0;
        std::uint8_t cc = 0, onValue = 0, offValue = 0;
        ChordNotes notes;
        bool humanized = false;
        std::array<std::uint16_t, 9> delayMs{};  // per note, humanized: the release is delayed alike
        std::uint32_t press = 0;                 // which press it came from, for the quantizer

        bool active() const { return type != ActionType::None; }
    };

    const TriggerBinding& bindingFor(int slot) const;
    static const TriggerBinding& normalBinding(const Mapping& m, int slot);
    void press(int slot);
    void release(int slot);
    Sounding soundingFor(const TriggerBinding& binding, int channel);
    void start(const Sounding& s);
    void stop(const Sounding& s);
    void sendCc(int channel, int cc, int value);
    // `key` names the control (an Axis, then 6 + finger * 2 + 0/1 for X/Y);
    // `atRest`: a stick back at its center, a trigger released.
    void sendAxis(const AxisBinding& binding, float position, int key, bool atRest);
    void sendProgram(int channel, int program, int bankMsb, int bankLsb);
    void toggleFreeze(std::uint8_t axes);
    void updateDigitalTrigger(Axis axis, float position);
    void setHatDir(std::optional<HatDir> dir);
    void beginRecordHold();
    void endRecordHold();
    void runLoops();
    void playMovementLoop(std::size_t loop, LoopPlayback playback, const double* syncedPhase = nullptr, int syncedDirection = 1);
    double beatAt(std::uint64_t t) const { return clock_.beat + (static_cast<double>(t) - static_cast<double>(clockAt_)) * clock_.tempo / 60e9; }
    BeatClock clock_;
    std::uint64_t clockAt_ = 0;
    TapTempo tapTempo_;
    std::optional<double> tappedTempo_;
    int snapBeats_ = 1, beatsPerBar_ = 4, quantize_ = 0;
    void sendLoopValue(std::size_t loop, float v);
    void playLoopNotes(std::size_t loop);
    // `press`: the press the note comes from (0: none); `leadAt`: when the
    // press's first note sounds, which the quantizer puts on the grid.
    void recordNote(int channel, int note, int velocity, std::uint64_t at, std::uint32_t press = 0, std::uint64_t leadAt = 0);
    void overdubNote(int channel, int note, int velocity, std::uint64_t at, std::uint32_t press, std::uint64_t leadAt);
    void emit(const MidiMessage& message, bool fromLoop);  // to the sink, telling the observer of notes
    void silenceLoop(std::size_t loop);
    // Sends `message` at `at` (now if it is not later); in time order.
    void sendAt(std::uint64_t at, const MidiMessage& message);
    void sendNow(const MidiMessage& message);
    int randomBetween(int low, int high);

    MidiSink& sink_;
    EngineObserver* observer_ = nullptr;
    std::shared_ptr<const Preset> preset_;
    const Mapping* mapping_ = nullptr;
    int channel_ = 0;

    std::bitset<kSlotCount> pressed_;
    std::array<Sounding, kSlotCount> held_{};
    std::array<std::array<Sounding, kSlotCount>, kChannelCount> latched_{};
    std::array<std::uint8_t, kChannelCount> frozen_{};
    std::array<std::array<float, kAxisCount>, kChannelCount> frozenAt_{};  // where frozen axes stopped
    std::optional<Key> key_;
    // Where each finger last was, kept once lifted (Velocity / Transpose axes); 0.5: untouched.
    std::array<std::array<float, 2>, kFingerCount> fingerHeld_{{{0.5f, 0.5f}, {0.5f, 0.5f}}};
    // The X, Y a movement loop last played for Velocity / Transpose axes; < 0: none.
    std::array<float, kChannelCount * kClipSlots * kTrackCount> loopShaping_ = [] {
        std::array<float, kChannelCount * kClipSlots * kTrackCount> none{};
        none.fill(-1.0f);
        return none;
    }();
    // Per channel and note of the note loop: how far the note on was transposed.
    std::array<std::array<std::int8_t, 128>, kChannelCount> loopShift_{};
    std::array<std::array<std::int16_t, 128>, kChannelCount> lastCc_{};

    std::uint8_t dpadBits_ = 0;
    std::optional<HatDir> hatDir_;
    std::optional<HatDir> pendingHatDir_;  // waiting to settle
    std::uint64_t pendingHatAt_ = 0;
    std::uint64_t now_ = 0;
    std::uint64_t settlePressNs_ = 0, settleReleaseNs_ = 0;

    Humanize humanize_;
    std::uint64_t random_ = 0x9E3779B97F4A7C15ull;
    struct Delayed {
        std::uint64_t at = 0, order = 0;
        MidiMessage message;
        bool fromLoop = false;
    };
    static constexpr std::size_t kMaxDelayed = 512;
    std::array<Delayed, kMaxDelayed> delayed_{};
    std::size_t delayedCount_ = 0;
    std::uint64_t delayedOrder_ = 0;
    bool immediate_ = false;  // releaseAll(): no delays
    bool fromLoop_ = false;   // a loop is sending: for EngineObserver::noteSent

    struct Loop {
        LoopStatus::State state = LoopStatus::State::Empty;
        // One axis: kMaxLoopSamples, allocated once; [0, 1] as 0-65534, kLifted
        // when the finger was off the touchpad.
        std::vector<std::uint16_t> x;
        int length = 0, position = 0, direction = 1;
        double phase = 0;  // movement loops: the position, between samples
        int stretch = 0;
        int amplitude = 100;
        double syncBeats = 0, anchorBeat = 0, startBeat = 0;  // synced: length, where it starts on the clock
        float amplitudeNow = 1;            // eased towards amplitude / 100
        float middleX = 0.5f;  // halfway between its lowest and highest samples
        std::uint64_t nextAt = 0;
        int sentX = -1;
        bool locked = false;
        double turns = 0;  // free: how many times round since it started
    };
    static constexpr std::uint16_t kLifted = 0xFFFF;
    static void findMiddle(Loop& loop);  // a movement loop's middle, from its samples
    // A synced loop's place now, from the beat clock: its phase in samples and the way it goes.
    void syncedPhase(const Loop& loop, LoopPlayback playback, double beat, double& phase, int& direction) const;
    // A clip's tracks: each movement source's X and Y (track source * 2 +
    // axis), then the note loop. Every clip's: slotIndex(channel, slot,
    // track); loopIndex, the current clip's.
    static constexpr int kChannelLoops = kClipSlots * kTrackCount;
    static constexpr std::size_t slotIndex(int channel, int slot, int track) {
        return static_cast<std::size_t>((channel * kClipSlots + slot) * kTrackCount + track);
    }
    static constexpr int channelOfLoop(std::size_t index) { return static_cast<int>(index / kChannelLoops); }
    static constexpr int trackOfLoop(std::size_t index) { return static_cast<int>(index % kTrackCount); }
    static constexpr int slotOfLoop(std::size_t index) { return static_cast<int>(index / kTrackCount % kClipSlots); }
    std::size_t loopIndex(int channel, int track) const {
        return slotIndex(channel, currentSlot_[static_cast<std::size_t>(channel)], track);
    }
    void clearTrack(std::size_t index);
    // Where a track's control is now, [0, 1]; false for a lifted finger (or the notes).
    bool trackPosition(int track, float& v) const;
    // Is that axis enabled in the mapping of `channel` (and a finger in axis mode)?
    bool trackEnabled(int channel, int track) const;
    int stepLength(std::size_t index) const;  // see loopStepSamples
    // Its playing value: held on the step grid's levels when it plays in steps.
    float stepped(std::size_t index, float v) const;
    std::array<Loop, kChannelCount * kChannelLoops> loops_;
    std::array<int, kChannelCount> currentSlot_{}, recordSlot_{};
    std::array<bool, kChannelCount> clipPlaying_{};
    std::array<int, kChannelCount> queued_ = [] { std::array<int, kChannelCount> a{}; a.fill(-1); return a; }();
    std::array<long long, kChannelCount> turnMark_{};
    std::array<std::array<ChainStep, kChainSteps>, kChannelCount> chains_{};
    std::array<bool, kChannelCount> chainLoop_ = [] { std::array<bool, kChannelCount> a{}; a.fill(true); return a; }();
    std::array<int, kChannelCount> chainAt_ = [] { std::array<int, kChannelCount> a{}; a.fill(-1); return a; }();
    std::array<int, kChannelCount> chainJump_ = [] { std::array<int, kChannelCount> a{}; a.fill(-1); return a; }();
    std::array<int, kChannelCount> chainTurns_{};
    std::array<long long, kChannelCount> chainMark_{};
    std::array<bool, kChannelCount> chainStarting_{};
    std::array<int, kChannelCount> chainClip_ = [] { std::array<int, kChannelCount> a{}; a.fill(-1); return a; }();
    int chainPick(int channel, int step);  // the clip a step plays: its own, or one picked at random
    bool chainStepPlays(int channel, int step) const;
    int chainFind(int channel, int from) const;  // the first step from there that plays, -1 none
    void runChains();
    void queueClip(int channel, int slot);  // launchClip, the chain left as it is
    // The current clip's turns so far, from its leading loop (false: none plays).
    bool clipTurns(int channel, double& turns) const;
    void switchClip(int channel, int slot);  // now
    void enterRecordSlot(int channel);       // a recording starts: into the record slot
    void startClipLoops(int channel);        // the current clip's loops, from their start
    bool currentHasLoops(int channel) const { return clipHasLoops(channel, currentSlot_[static_cast<std::size_t>(channel)]); }
    // Where a source is now: X, Y in [0, 1]; false for a lifted finger.
    bool sourcePosition(int source, float& x, float& y) const;
    // The hold of a LoopRecord binding: on which channel, which sources may
    // still start recording, and where they were when it began.
    int holdChannel_ = 0;
    std::array<bool, kTrackCount> waiting_{};
    std::array<float, kTrackCount> holdStart_{};
    std::array<bool, kTrackCount> holdStartOn_{};
    // Note loops: the notes recorded per channel, sorted by `at` once recorded,
    // and the notes each loop has on.
    struct NoteEvent {
        std::int32_t at = 0;  // loop sample
        std::uint8_t note = 0, velocity = 0;
        bool on = false;
        std::uint32_t press = 0;  // the press it came from (0: none)
    };
    // The quantizer: a synced note loop's grid step in samples (0: none), and
    // how far each recent press moved, by press number (slot press % size).
    double quantizeStep(const Loop& loop) const;
    void quantizeNoteLoop(int channel);
    struct PressShift {
        std::uint32_t press = 0;
        std::int32_t first = 0, shift = 0;
    };
    std::array<PressShift, kMaxNoteEvents> pressShift_{};
    std::uint32_t pressCount_ = 0;
    // Keeps the notes of one pitch in a note loop from overlapping: a note
    // still on when the next one of its pitch starts ends there, and two that
    // start together become one. `spans_` is its scratch room, allocated once.
    void tidyNoteLoop(int channel);
    // The note on at `start` with pitch `note` and its release (-1: none), by index.
    bool findLoopNote(int channel, int start, int note, int& on, int& off) const;
    void silenceLoopNote(int channel, int note);
    struct NoteSpan {
        std::int32_t start = 0, end = 0;  // start < 0: a release alone; end < 0: still on
        std::uint8_t note = 0, velocity = 0;
        std::uint32_t press = 0;
    };
    std::vector<NoteSpan> spans_;
    std::array<std::vector<NoteEvent>, kChannelCount * kClipSlots> noteEvents_;  // per channel and clip
    std::vector<NoteEvent>& eventsOf(int channel) {
        return noteEvents_[static_cast<std::size_t>(channel * kClipSlots + currentSlot_[static_cast<std::size_t>(channel)])];
    }
    const std::vector<NoteEvent>& eventsOf(int channel) const {
        return noteEvents_[static_cast<std::size_t>(channel * kClipSlots + currentSlot_[static_cast<std::size_t>(channel)])];
    }
    std::array<std::bitset<128>, kChannelCount> noteLoopOn_{};
    std::array<std::uint8_t, 128> noteVelocity_{};  // of the last note-on recorded, for its release
    std::uint64_t noteRecordStart_ = 0;
    int overdubChannel_ = -1;        // the channel whose note loop takes new notes
    bool loopsPaused_ = false;
    // Program Change: the last one sent per channel (-1: none yet), and for
    // axes choosing one, the program waiting to settle, its bank and since when.
    std::array<int, kChannelCount> program_{};
    static constexpr int kProgramControls = kAxisCount + kFingerCount * 2;
    static constexpr std::uint64_t kProgramSettleNs = 150'000'000;
    std::array<int, kProgramControls> pendingProgram_{};
    std::array<std::array<std::int16_t, 2>, kProgramControls> pendingBank_{};  // MSB, LSB
    std::array<int, kProgramControls> axisProgram_{};  // the last one an axis sent
    std::array<std::uint64_t, kProgramControls> pendingSince_{};
    std::uint64_t holdStartedAt_ = 0;  // when Record went down
    std::array<std::array<float, 2>, kFingerCount> fingerPosition_{};  // raw touchpad X, Y
    std::array<bool, kFingerCount> fingerDown_{};
    std::array<float, 4> stickPosition_{0.5f, 0.5f, 0.5f, 0.5f};  // [0, 1], LeftX..RightY
    std::array<float, 2> triggerPosition_{};                       // [0, 1], L2 and R2
    std::bitset<kSlotCount> recordHolders_;
    // LoopStretch: who holds it, the stick turns not yet a whole step, the
    // finger sliding and where it last counted, when 1x was reached per source.
    std::bitset<kSlotCount> stretchHolders_;
    bool shiftLatched_ = false;  // a Shift binding in toggle mode, pressed once
    std::array<double, 2> stickStretchRest_{}, stickAmplitudeRest_{};
    int stretchFinger_ = -1;
    float stretchFingerX_ = 0, stretchFingerY_ = 0;
    std::array<std::uint64_t, kTrackCount> stretchPausedUntil_{};
    std::array<bool, 2> stickAmplitudes_{};
    bool fingerAmplitudes_ = false;
    std::uint64_t lastStretchTick_ = 0;
    void stretchFromControls();
    void stretchPausing(int source, int steps);
    void amplifyPausing(int source, int steps);
    std::array<std::uint64_t, kTrackCount> amplitudePausedUntil_{};
    bool recordedThisHold_ = false;
    // The session screen: its button, whether it shows, the events asked, and
    // the stick's and the D-pad's pushes held, with when they repeat.
    bool sessionMode_ = false, presetHasSession_ = false, wasNavigating_ = false, sessionValues_ = false;
    bool navigating() const { return sessionMode_ && recordHolders_.none(); }
    std::array<SessionEvent, 16> sessionEvents_{};
    int sessionEventCount_ = 0;
    void pushSessionEvent(SessionEvent e);
    std::array<std::optional<SessionEvent>, 2> stickNav_{};
    std::optional<SessionEvent> hatNav_;
    std::array<std::uint64_t, 2> stickNavNext_{};
    std::uint64_t hatNavNext_ = 0;
    std::optional<SessionEvent> stickPush(int stick) const;  // where a stick is pushed, past kNavPush (or held)
    void navigate();
    // Shift + Record: which sources toggled their lock this hold (a finger
    // counts for both), and whether one did.
    void lockFromControls();
    std::array<bool, kTrackCount> lockToggled_{};
    bool lockedThisHold_ = false;
    // Shift + two taps on Record: when the first tap ended (0: none), on which
    // channel, the overdub before it, and the stop it asked for, held back.
    std::uint64_t shiftTapAt_ = 0;
    int shiftTapChannel_ = 0, overdubBeforeTap_ = -1;
    int pendingStopChannel_ = -1;
    std::uint64_t pendingStopAt_ = 0;
    void flushPendingStop();
    std::array<std::optional<Zone>, kFingerCount> fingerZone_{};
    std::array<bool, 2> triggerDown_{};
};

}  // namespace midipad
