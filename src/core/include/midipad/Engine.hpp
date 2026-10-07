#pragma once

#include "midipad/MidiMessage.hpp"
#include "midipad/Preset.hpp"

#include <array>
#include <bitset>
#include <memory>
#include <optional>

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

    void buttonChanged(Button button, bool pressed);
    // Sticks in [-1, 1] (SDL convention: up is negative), triggers in [0, 1].
    void axisMoved(Axis axis, float value);
    // Normalized position, origin at the top-left corner of the touchpad.
    void touchMoved(int finger, float x, float y);
    void touchReleased(int finger);

    // Turns off every note and CC still on, latched or held.
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

        bool active() const { return type != ActionType::None; }
    };

    const TriggerBinding& bindingFor(int slot) const;
    void press(int slot);
    void release(int slot);
    Sounding soundingFor(const TriggerBinding& binding) const;
    void start(const Sounding& s);
    void stop(const Sounding& s);
    void sendCc(int channel, int cc, int value);
    void sendAxis(const AxisBinding& binding, float position);
    void toggleFreeze(std::uint8_t axes);
    void updateDigitalTrigger(Axis axis, float position);

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
    std::array<std::optional<Zone>, kFingerCount> fingerZone_{};
    std::array<bool, 2> triggerDown_{};
};

}  // namespace midipad
