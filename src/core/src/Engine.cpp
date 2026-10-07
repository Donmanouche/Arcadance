#include "midipad/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace midipad {

namespace {

// Toggled notes and chords are exclusive within a family of controls, like
// radio buttons: the face/shoulder buttons, the D-pad, and each touchpad finger.
int familyOf(int slot, int hatBase, int zoneBase) {
    if (slot < hatBase)
        return 0;
    if (slot < zoneBase)
        return 1;
    return 2 + (slot - zoneBase) / kZoneCount;
}

std::uint8_t axisBit(Axis a) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(a)); }

std::optional<HatDir> hatFromDpad(std::uint8_t bits) {
    auto held = [bits](Button b) { return (bits >> (static_cast<int>(b) - static_cast<int>(Button::DpadUp))) & 1; };
    const int y = held(Button::DpadUp) - held(Button::DpadDown);
    const int x = held(Button::DpadRight) - held(Button::DpadLeft);
    static constexpr std::optional<HatDir> kTable[3][3] = {
        {HatDir::DownLeft, HatDir::Down, HatDir::DownRight},
        {HatDir::Left, std::nullopt, HatDir::Right},
        {HatDir::UpLeft, HatDir::Up, HatDir::UpRight},
    };
    return kTable[y + 1][x + 1];
}

std::uint8_t transportStatus(TransportCommand c) {
    switch (c) {
    case TransportCommand::Start: return 0xFA;
    case TransportCommand::Continue: return 0xFB;
    case TransportCommand::Stop: return 0xFC;
    }
    return 0xFA;
}

}  // namespace

Engine::Engine(MidiSink& sink) : sink_(sink) {
    for (auto& channel : lastCc_)
        channel.fill(-1);
    setPreset(nullptr);
}

void Engine::setPreset(std::shared_ptr<const Preset> preset) {
    preset_ = preset ? std::move(preset) : std::make_shared<const Preset>();
    mapping_ = &preset_->forChannel(channel_);
}

void Engine::setChannel(int channel) {
    channel_ = std::clamp(channel, 0, kChannelCount - 1);
    mapping_ = &preset_->forChannel(channel_);
    if (observer_)
        observer_->channelChanged(channel_);
}

bool Engine::isFrozen(Axis axis) const { return frozen_[static_cast<std::size_t>(channel_)] & axisBit(axis); }

void Engine::buttonChanged(Button button, bool pressed) {
    if (button >= Button::LeftTrigger)  // digital triggers come from axisMoved()
        return;
    if (!isDpad(button)) {
        pressed ? press(static_cast<int>(button)) : release(static_cast<int>(button));
        return;
    }

    const auto bit = static_cast<std::uint8_t>(1u << (static_cast<int>(button) - static_cast<int>(Button::DpadUp)));
    dpadBits_ = pressed ? static_cast<std::uint8_t>(dpadBits_ | bit) : static_cast<std::uint8_t>(dpadBits_ & ~bit);
    const auto dir = hatFromDpad(dpadBits_);
    if (dir == hatDir_)
        return;
    if (hatDir_)
        release(kHatSlotBase + static_cast<int>(*hatDir_));
    hatDir_ = dir;
    if (dir)
        press(kHatSlotBase + static_cast<int>(*dir));
}

void Engine::axisMoved(Axis axis, float value) {
    if (axis >= Axis::Count || std::isnan(value))
        return;
    const float position = std::clamp(isTrigger(axis) ? value : (value + 1.0f) * 0.5f, 0.0f, 1.0f);
    if (isTrigger(axis))
        updateDigitalTrigger(axis, position);

    const AxisBinding& binding = mapping_->axes[static_cast<std::size_t>(axis)];
    if (binding.enabled && !isFrozen(axis))
        sendAxis(binding, position);
}

void Engine::touchMoved(int finger, float x, float y) {
    if (finger < 0 || finger >= kFingerCount || std::isnan(x) || std::isnan(y))
        return;
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    const FingerBinding& binding = mapping_->fingers[static_cast<std::size_t>(finger)];

    if (binding.mode == TouchMode::Axis) {
        if (binding.x.enabled)
            sendAxis(binding.x, x);
        if (binding.y.enabled)
            sendAxis(binding.y, 1.0f - y);
    } else if (binding.mode == TouchMode::Zones) {
        const Zone zone = y < 0.5f ? (x < 0.5f ? Zone::TopLeft : Zone::TopRight)
                                   : (x < 0.5f ? Zone::BottomLeft : Zone::BottomRight);
        auto& current = fingerZone_[static_cast<std::size_t>(finger)];
        if (current == zone)
            return;
        const int base = kZoneSlotBase + finger * kZoneCount;
        if (current)
            release(base + static_cast<int>(*current));
        current = zone;
        press(base + static_cast<int>(zone));
    }
}

void Engine::touchReleased(int finger) {
    if (finger < 0 || finger >= kFingerCount)
        return;
    auto& current = fingerZone_[static_cast<std::size_t>(finger)];
    if (current)
        release(kZoneSlotBase + finger * kZoneCount + static_cast<int>(*current));
    current.reset();
}

void Engine::releaseAll() {
    for (auto& s : held_) {
        if (s.active())
            stop(s);
        s = {};
    }
    for (auto& channel : latched_) {
        for (auto& s : channel) {
            if (s.active())
                stop(s);
            s = {};
        }
    }
}

std::uint64_t Engine::soundingSlots() const {
    std::uint64_t bits = 0;
    const auto& latched = latched_[static_cast<std::size_t>(channel_)];
    for (int s = 0; s < kSlotCount; ++s)
        if (held_[static_cast<std::size_t>(s)].active() || latched[static_cast<std::size_t>(s)].active())
            bits |= std::uint64_t{1} << s;
    return bits;
}

void Engine::reset() {
    releaseAll();
    pressed_.reset();
    dpadBits_ = 0;
    hatDir_.reset();
    fingerZone_ = {};
    triggerDown_ = {};
}

const TriggerBinding& Engine::bindingFor(int slot) const {
    static const TriggerBinding kUnbound;
    if (slot < kHatSlotBase)
        return mapping_->buttons[static_cast<std::size_t>(slot)];
    if (slot < kZoneSlotBase)
        return mapping_->dpad[static_cast<std::size_t>(slot - kHatSlotBase)];
    const auto finger = static_cast<std::size_t>((slot - kZoneSlotBase) / kZoneCount);
    const auto zone = static_cast<std::size_t>((slot - kZoneSlotBase) % kZoneCount);
    const FingerBinding& f = mapping_->fingers[finger];
    return f.mode == TouchMode::Zones ? f.zones[zone] : kUnbound;
}

void Engine::press(int slot) {
    if (pressed_.test(static_cast<std::size_t>(slot)))
        return;
    pressed_.set(static_cast<std::size_t>(slot));

    const TriggerBinding& binding = bindingFor(slot);
    switch (binding.type) {
    case ActionType::None:
        return;
    case ActionType::Transport:
        sink_.send(MidiMessage::realtime(transportStatus(binding.transport)));
        return;
    case ActionType::ChannelStep:
        setChannel(((channel_ + binding.channelStep) % kChannelCount + kChannelCount) % kChannelCount);
        return;
    case ActionType::Freeze:
        toggleFreeze(binding.freezeAxes);
        return;
    case ActionType::Note:
    case ActionType::Chord:
    case ActionType::ControlChange:
        break;
    }

    const Sounding sounding = soundingFor(binding);
    if (binding.mode == Mode::Momentary) {
        start(sounding);
        held_[static_cast<std::size_t>(slot)] = sounding;
        return;
    }

    auto& latched = latched_[static_cast<std::size_t>(channel_)];
    Sounding& mine = latched[static_cast<std::size_t>(slot)];
    if (mine.active()) {
        stop(mine);
        mine = {};
        return;
    }
    if (sounding.type != ActionType::ControlChange) {
        const int family = familyOf(slot, kHatSlotBase, kZoneSlotBase);
        for (int other = 0; other < kSlotCount; ++other) {
            Sounding& s = latched[static_cast<std::size_t>(other)];
            if (s.active() && s.type != ActionType::ControlChange &&
                familyOf(other, kHatSlotBase, kZoneSlotBase) == family) {
                stop(s);
                s = {};
            }
        }
    }
    start(sounding);
    mine = sounding;
}

void Engine::release(int slot) {
    if (!pressed_.test(static_cast<std::size_t>(slot)))
        return;
    pressed_.reset(static_cast<std::size_t>(slot));
    Sounding& s = held_[static_cast<std::size_t>(slot)];
    if (s.active())
        stop(s);
    s = {};
}

Engine::Sounding Engine::soundingFor(const TriggerBinding& binding) const {
    Sounding s;
    s.type = binding.type;
    s.channel = static_cast<std::uint8_t>(channel_);
    s.velocity = binding.velocity;
    if (binding.type == ActionType::Note) {
        s.notes.notes[0] = binding.number;
        s.notes.count = 1;
    } else if (binding.type == ActionType::Chord) {
        s.notes = binding.chord;
    } else {
        s.cc = binding.number;
        s.onValue = binding.onValue;
        s.offValue = binding.offValue;
    }
    return s;
}

void Engine::start(const Sounding& s) {
    if (s.type == ActionType::ControlChange) {
        sendCc(s.channel, s.cc, s.onValue);
        return;
    }
    for (std::uint8_t note : s.notes.view())
        sink_.send(MidiMessage::noteOn(s.channel, note, s.velocity));
}

void Engine::stop(const Sounding& s) {
    if (s.type == ActionType::ControlChange) {
        sendCc(s.channel, s.cc, s.offValue);
        return;
    }
    for (std::uint8_t note : s.notes.view())
        sink_.send(MidiMessage::noteOff(s.channel, note));
}

void Engine::sendCc(int channel, int cc, int value) {
    value = std::clamp(value, 0, 127);
    lastCc_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(cc & 0x7F)] = static_cast<std::int16_t>(value);
    sink_.send(MidiMessage::controlChange(channel, cc, value));
}

void Engine::sendAxis(const AxisBinding& binding, float position) {
    if (binding.invert)
        position = 1.0f - position;
    const int span = binding.max - binding.min;
    const int value = binding.min + static_cast<int>(std::lround(position * static_cast<float>(span)));
    // Controllers report far more steps than a 7-bit CC has: skip repeats.
    if (lastCc_[static_cast<std::size_t>(channel_)][binding.cc & 0x7F] == value)
        return;
    sendCc(channel_, binding.cc, value);
}

void Engine::toggleFreeze(std::uint8_t axes) {
    auto& frozen = frozen_[static_cast<std::size_t>(channel_)];
    frozen = (frozen & axes) == axes ? static_cast<std::uint8_t>(frozen & ~axes)
                                     : static_cast<std::uint8_t>(frozen | axes);
    if (observer_)
        observer_->freezeChanged(channel_, frozen);
}

void Engine::updateDigitalTrigger(Axis axis, float position) {
    const bool left = axis == Axis::LeftTrigger;
    bool& down = triggerDown_[left ? 0 : 1];
    const int slot = static_cast<int>(left ? Button::LeftTrigger : Button::RightTrigger);
    if (!down && position >= kTriggerPress) {
        down = true;
        press(slot);
    } else if (down && position <= kTriggerRelease) {
        down = false;
        release(slot);
    }
}

}  // namespace midipad
