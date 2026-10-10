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
};

// What a loop is doing, for display.
struct LoopStatus {
    enum class State : std::uint8_t { Empty, Recording, Playing } state = State::Empty;
    bool waiting = false;  // Record held, this source has not moved yet
    bool zones = false;    // a finger in 4-pad mode: the samples are pads (0-3), not X/Y
    int length = 0;        // samples, at Engine::kLoopRateHz
    int position = 0;      // sample playing now
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

    // Loops, one per LoopSource and channel: a LoopRecord binding held records
    // each stick or touchpad finger that moves, X and Y, into its loop on the
    // active channel; when a finger is lifted nothing is recorded nor, later,
    // played. A finger in 4-pad mode records the pads it plays: the loop plays
    // them again (as mapped), and cuts what still sounds when it starts over. On release each recording plays in a loop on its own
    // CCs (Mapping::loops), beside the stick or finger. Every channel's loops
    // play at once. A hold where nothing moves stops the active channel's loops.
    static constexpr int kLoopRateHz = 200;
    static constexpr int kMaxLoopSamples = 60 * kLoopRateHz;
    static constexpr float kLoopMoveThreshold = 0.06f;  // of the [0, 1] travel
    LoopStatus loopStatus(int channel, int source) const;
    // [0, 1]; axis 0: X, 1: Y (a finger's Y grows downwards); for a 4-pad
    // loop, the pad (0-3). Negative while a finger was lifted.
    float loopSample(int channel, int source, int axis, int index) const;
    void clearLoop(int channel, int source);
    void stopLoops(int channel);
    void stopLoops();  // every channel
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
    void sendAxis(const AxisBinding& binding, float position);
    void toggleFreeze(std::uint8_t axes);
    void updateDigitalTrigger(Axis axis, float position);
    void setHatDir(std::optional<HatDir> dir);
    void beginRecordHold();
    void endRecordHold();
    void runLoops();
    void sendLoopSample(std::size_t loop);
    void playLoopPads(std::size_t loop);
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
    };
    static constexpr std::size_t kMaxDelayed = 512;
    std::array<Delayed, kMaxDelayed> delayed_{};
    std::size_t delayedCount_ = 0;
    std::uint64_t delayedOrder_ = 0;
    bool immediate_ = false;  // releaseAll(): no delays

    struct Loop {
        LoopStatus::State state = LoopStatus::State::Empty;
        // kMaxLoopSamples each, allocated once; [0, 1] as 0-65534, kLifted
        // when the finger was off the touchpad.
        std::vector<std::uint16_t> x, y;
        int length = 0, position = 0, direction = 1;
        std::uint64_t nextAt = 0;
        int sentX = -1, sentY = -1;
        // 4-pad loops: the pad playing at the previous step, and what the loop
        // has on, held (momentary) or latched (toggle) per pad.
        bool zones = false;
        int lastPad = -1;
        Sounding held;
        std::array<Sounding, kZoneCount> latched{};
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
    std::array<int, kFingerCount> holdStartPad_{};  // -1: none
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
