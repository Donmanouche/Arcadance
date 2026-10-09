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
    for (auto& loop : loops_) {
        loop.x.assign(kMaxLoopSamples, 32768);
        loop.y.assign(kMaxLoopSamples, 32768);
    }
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
    pendingHatDir_.reset();
    if (dir == hatDir_)
        return;
    const std::uint64_t settle = pressed ? settlePressNs_ : settleReleaseNs_;
    if (!dir || settle == 0) {
        setHatDir(dir);
        return;
    }
    pendingHatDir_ = dir;
    pendingHatAt_ = now_ + settle;
}

void Engine::setDpadSettle(int pressMs, int releaseMs) {
    settlePressNs_ = static_cast<std::uint64_t>(std::max(pressMs, 0)) * 1'000'000u;
    settleReleaseNs_ = static_cast<std::uint64_t>(std::max(releaseMs, 0)) * 1'000'000u;
}

void Engine::tick(std::uint64_t nowNs) {
    now_ = nowNs;
    if (pendingHatDir_ && now_ >= pendingHatAt_) {
        const auto dir = pendingHatDir_;
        pendingHatDir_.reset();
        setHatDir(dir);
    }
    runLoops();
    // Humanized notes now due, oldest first.
    while (delayedCount_ > 0) {
        std::size_t next = delayedCount_;
        for (std::size_t i = 0; i < delayedCount_; ++i) {
            const Delayed& d = delayed_[i];
            if (d.at <= now_ && (next == delayedCount_ || d.at < delayed_[next].at ||
                                 (d.at == delayed_[next].at && d.order < delayed_[next].order)))
                next = i;
        }
        if (next == delayedCount_)
            break;
        const MidiMessage message = delayed_[next].message;
        delayed_[next] = delayed_[--delayedCount_];
        sendNow(message);
    }
}

void Engine::sendAt(std::uint64_t at, const MidiMessage& message) {
    if (at <= now_ || delayedCount_ == delayed_.size()) {
        sendNow(message);
        return;
    }
    delayed_[delayedCount_++] = {at, delayedOrder_++, message};
}

void Engine::sendNow(const MidiMessage& message) {
    // A note starting again first ends its previous instance, whose delayed
    // release would otherwise cut it.
    if ((message.bytes[0] & 0xF0) == 0x90) {
        for (std::size_t i = 0; i < delayedCount_;) {
            const MidiMessage& m = delayed_[i].message;
            if ((m.bytes[0] & 0xF0) == 0x80 && (m.bytes[0] & 0x0F) == (message.bytes[0] & 0x0F) &&
                m.bytes[1] == message.bytes[1]) {
                sink_.send(m);
                delayed_[i] = delayed_[--delayedCount_];
            } else {
                ++i;
            }
        }
    }
    sink_.send(message);
}

int Engine::randomBetween(int low, int high) {
    if (low > high)
        std::swap(low, high);
    random_ ^= random_ << 13;  // xorshift64
    random_ ^= random_ >> 7;
    random_ ^= random_ << 17;
    return low + static_cast<int>(random_ % static_cast<std::uint64_t>(high - low + 1));
}

void Engine::setHatDir(std::optional<HatDir> dir) {
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
    if (!isTrigger(axis))
        stickPosition_[static_cast<std::size_t>(axis)] = position;
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
    // Releases still waiting go out now; notes not started yet never will.
    for (std::size_t i = 0; i < delayedCount_; ++i)
        if ((delayed_[i].message.bytes[0] & 0xF0) == 0x80)
            sink_.send(delayed_[i].message);
    delayedCount_ = 0;
    immediate_ = true;
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
    immediate_ = false;
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
    if (recordHolders_.any()) {  // the release will never come
        recordHolders_.reset();
        endRecordHold();
    }
    releaseAll();
    pressed_.reset();
    dpadBits_ = 0;
    hatDir_.reset();
    pendingHatDir_.reset();
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
    case ActionType::LoopRecord:
        if (recordHolders_.none())
            beginRecordHold();
        recordHolders_.set(static_cast<std::size_t>(slot));
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
    if (recordHolders_.test(static_cast<std::size_t>(slot))) {
        recordHolders_.reset(static_cast<std::size_t>(slot));
        if (recordHolders_.none())
            endRecordHold();
    }
    Sounding& s = held_[static_cast<std::size_t>(slot)];
    if (s.active())
        stop(s);
    s = {};
}

Engine::Sounding Engine::soundingFor(const TriggerBinding& binding) {
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
        return s;
    }
    s.humanized = binding.humanize && humanize_.appliesTo(channel_);
    if (s.humanized)
        for (std::size_t i = 0; i < s.notes.count; ++i)
            s.delayMs[i] = static_cast<std::uint16_t>(std::clamp(
                randomBetween(humanize_.delayMinMs, humanize_.delayMaxMs), 0, Humanize::kMaxDelayMs));
    return s;
}

void Engine::start(const Sounding& s) {
    if (s.type == ActionType::ControlChange) {
        sendCc(s.channel, s.cc, s.onValue);
        return;
    }
    for (std::size_t i = 0; i < s.notes.count; ++i) {
        int velocity = s.velocity;
        std::uint64_t at = now_;
        if (s.humanized) {
            const int percent = randomBetween(humanize_.velocityMinPercent, humanize_.velocityMaxPercent);
            velocity = std::clamp((s.velocity * percent + 50) / 100, 1, 127);
            at += static_cast<std::uint64_t>(s.delayMs[i]) * 1'000'000u;
        }
        sendAt(at, MidiMessage::noteOn(s.channel, s.notes.notes[i], velocity));
    }
}

void Engine::stop(const Sounding& s) {
    if (s.type == ActionType::ControlChange) {
        sendCc(s.channel, s.cc, s.offValue);
        return;
    }
    for (std::size_t i = 0; i < s.notes.count; ++i) {
        const MidiMessage off = MidiMessage::noteOff(s.channel, s.notes.notes[i]);
        if (immediate_ || !s.humanized)
            sendNow(off);
        else
            sendAt(now_ + static_cast<std::uint64_t>(s.delayMs[i]) * 1'000'000u, off);
    }
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

// --- Loops --------------------------------------------------------------------

void Engine::beginRecordHold() {
    recordedThisHold_ = false;
    holdChannel_ = channel_;
    waiting_ = {true, true};
    holdStart_ = stickPosition_;
}

void Engine::endRecordHold() {
    waiting_ = {false, false};
    for (int channel = 0; channel < kChannelCount; ++channel) {
        for (int stick = 0; stick < 2; ++stick) {
            Loop& loop = loops_[loopIndex(channel, stick)];
            if (loop.state != LoopStatus::State::Recording)
                continue;
            if (loop.length < 2) {
                loop.state = LoopStatus::State::Empty;
                continue;
            }
            loop.state = LoopStatus::State::Playing;
            const LoopPlayback playback = preset_->forChannel(channel).loops[static_cast<std::size_t>(stick)].playback;
            loop.position = playback == LoopPlayback::Reverse ? loop.length - 1 : 0;
            loop.direction = playback == LoopPlayback::Reverse ? -1 : 1;
            loop.nextAt = now_;
            loop.sentX = loop.sentY = -1;
        }
    }
    if (!recordedThisHold_)
        stopLoops(holdChannel_);  // Record pressed without moving a stick
}

void Engine::runLoops() {
    constexpr std::uint64_t kStep = 1'000'000'000ull / kLoopRateHz;
    // A stick that moves during the hold starts recording into its loop.
    for (int stick = 0; stick < 2; ++stick) {
        const auto sx = static_cast<std::size_t>(stick * 2), sy = sx + 1;
        if (!waiting_[static_cast<std::size_t>(stick)] ||
            (std::abs(stickPosition_[sx] - holdStart_[sx]) <= kLoopMoveThreshold &&
             std::abs(stickPosition_[sy] - holdStart_[sy]) <= kLoopMoveThreshold))
            continue;
        waiting_[static_cast<std::size_t>(stick)] = false;
        Loop& loop = loops_[loopIndex(holdChannel_, stick)];
        loop.state = LoopStatus::State::Recording;
        loop.length = 0;
        loop.nextAt = now_;
        recordedThisHold_ = true;
    }

    for (std::size_t i = 0; i < loops_.size(); ++i) {
        Loop& loop = loops_[i];
        if (loop.state == LoopStatus::State::Empty)
            continue;
        const auto channel = static_cast<int>(i / 2);
        const auto stick = i % 2;
        if (now_ > loop.nextAt + 100 * kStep)
            loop.nextAt = now_;  // the thread stalled: do not catch up in a burst
        while (now_ >= loop.nextAt) {
            loop.nextAt += kStep;
            if (loop.state == LoopStatus::State::Recording) {
                if (loop.length < kMaxLoopSamples) {
                    auto sample = [](float v) { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
                    loop.x[static_cast<std::size_t>(loop.length)] = sample(stickPosition_[stick * 2]);
                    loop.y[static_cast<std::size_t>(loop.length)] = sample(stickPosition_[stick * 2 + 1]);
                    ++loop.length;
                }
                continue;
            }
            sendLoopSample(i);
            // Then step, as the playback asks now (it can change while playing).
            const LoopPlayback playback = preset_->forChannel(channel).loops[stick].playback;
            if (playback == LoopPlayback::Forward) {
                loop.direction = 1;
                loop.position = (loop.position + 1) % loop.length;
            } else if (playback == LoopPlayback::Reverse) {
                loop.direction = -1;
                loop.position = (loop.position - 1 + loop.length) % loop.length;
            } else {
                loop.position += loop.direction;
                if (loop.position >= loop.length) {
                    loop.direction = -1;
                    loop.position = std::max(0, loop.length - 2);
                } else if (loop.position < 0) {
                    loop.direction = 1;
                    loop.position = std::min(1, loop.length - 1);
                }
            }
        }
    }
}

// Sends a loop's current sample on its CCs and channel, scaled like the stick's axes.
void Engine::sendLoopSample(std::size_t index) {
    Loop& loop = loops_[index];
    const auto channel = static_cast<int>(index / 2);
    const auto stick = index % 2;
    const Mapping& m = preset_->forChannel(channel);
    const LoopBinding& binding = m.loops[stick];
    auto value = [&](Axis axis, std::uint16_t sample) {
        const AxisBinding& a = m.axes[static_cast<std::size_t>(axis)];
        float position = static_cast<float>(sample) / 65535.0f;
        if (a.invert)
            position = 1.0f - position;
        const int lo = a.enabled ? a.min : 0, hi = a.enabled ? a.max : 127;
        return lo + static_cast<int>(std::lround(position * static_cast<float>(hi - lo)));
    };
    const auto i = static_cast<std::size_t>(loop.position);
    const int vx = value(stick == 0 ? Axis::LeftX : Axis::RightX, loop.x[i]);
    const int vy = value(stick == 0 ? Axis::LeftY : Axis::RightY, loop.y[i]);
    if (vx != loop.sentX) {
        sendCc(channel, binding.ccX, vx);
        loop.sentX = vx;
    }
    if (vy != loop.sentY) {
        sendCc(channel, binding.ccY, vy);
        loop.sentY = vy;
    }
}

LoopStatus Engine::loopStatus(int channel, int stick) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    stick = std::clamp(stick, 0, 1);
    const Loop& loop = loops_[loopIndex(channel, stick)];
    const bool waiting = waiting_[static_cast<std::size_t>(stick)] && holdChannel_ == channel;
    return {loop.state, waiting, loop.length, loop.position, channel};
}

float Engine::loopSample(int channel, int stick, int axis, int index) const {
    const Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), std::clamp(stick, 0, 1))];
    const auto i = static_cast<std::size_t>(std::clamp(index, 0, kMaxLoopSamples - 1));
    return static_cast<float>(axis == 0 ? loop.x[i] : loop.y[i]) / 65535.0f;
}

void Engine::clearLoop(int channel, int stick) {
    Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), std::clamp(stick, 0, 1))];
    loop.state = LoopStatus::State::Empty;
    loop.length = 0;
}

void Engine::stopLoops(int channel) {
    for (int stick = 0; stick < 2; ++stick)
        if (loops_[loopIndex(channel, stick)].state == LoopStatus::State::Playing)
            clearLoop(channel, stick);
}

void Engine::stopLoops() {
    for (int channel = 0; channel < kChannelCount; ++channel)
        stopLoops(channel);
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
