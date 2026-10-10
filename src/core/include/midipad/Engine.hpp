#pragma once

#include "midipad/Humanize.hpp"
#include "midipad/MidiMessage.hpp"
#include "midipad/Preset.hpp"

#include <array>
#include <bitset>
#include <memory>
#include <optional>
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
    static constexpr int kLoopRateHz = 200;
    static constexpr int kMaxLoopSamples = 60 * kLoopRateHz;
    static constexpr float kLoopMoveThreshold = 0.06f;  // of the [0, 1] travel
    static constexpr int kLoopTapMs = 500;
    LoopStatus loopStatus(int channel, int source) const;
    // [0, 1]; axis 0: X, 1: Y (a finger's Y grows downwards). Negative while a
    // finger was lifted.
    float loopSample(int channel, int source, int axis, int index) const;
    void clearLoop(int channel, int source);
    // The notes of a channel's note loop, at most `max`; returns how many.
    int noteLoopIntervals(int channel, NoteInterval* out, int max) const;
    static constexpr int kMaxNoteEvents = 4096;  // per note loop
    void stopLoops(int channel);
    // A bit per channel where a loop plays or records.
    std::uint16_t channelsWithLoops() const;
    void stopLoops();  // every channel
    // Pauses every channel's loops, keeping them (what they have on stops), or
    // plays them all again from their start. A loop that starts recording
    // while they are paused plays them all again.
    void pauseLoops();
    void playLoops();
    bool loopsPaused() const { return loopsPaused_; }
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

        bool active() const { return type != ActionType::None; }
    };

    const TriggerBinding& bindingFor(int slot) const;
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
    void sendLoopSample(std::size_t loop);
    void playLoopNotes(std::size_t loop);
    void recordNote(int channel, int note, int velocity, std::uint64_t at);
    void overdubNote(int channel, int note, int velocity, std::uint64_t at);
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
        // kMaxLoopSamples each, allocated once; [0, 1] as 0-65534, kLifted
        // when the finger was off the touchpad.
        std::vector<std::uint16_t> x, y;
        int length = 0, position = 0, direction = 1;
        std::uint64_t nextAt = 0;
        int sentX = -1, sentY = -1;
    };
    static constexpr std::uint16_t kLifted = 0xFFFF;
    static constexpr std::size_t loopIndex(int channel, int source) {
        return static_cast<std::size_t>(channel * kLoopSourceCount + source);
    }
    std::array<Loop, kChannelCount * kLoopSourceCount> loops_;  // loopIndex(channel, source)
    // Where a source is now: X, Y in [0, 1]; false for a lifted finger.
    bool sourcePosition(int source, float& x, float& y) const;
    // The hold of a LoopRecord binding: on which channel, which sources may
    // still start recording, and where they were when it began.
    int holdChannel_ = 0;
    std::array<bool, kLoopSourceCount> waiting_{};
    std::array<std::array<float, 2>, kLoopSourceCount> holdStart_{};
    std::array<bool, kLoopSourceCount> holdStartOn_{};
    // Note loops: the notes recorded per channel, sorted by `at` once recorded,
    // and the notes each loop has on.
    struct NoteEvent {
        std::int32_t at = 0;  // loop sample
        std::uint8_t note = 0, velocity = 0;
        bool on = false;
    };
    std::array<std::vector<NoteEvent>, kChannelCount> noteEvents_;
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
    bool recordedThisHold_ = false;
    std::array<std::optional<Zone>, kFingerCount> fingerZone_{};
    std::array<bool, 2> triggerDown_{};
};

}  // namespace midipad
