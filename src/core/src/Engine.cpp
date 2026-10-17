#include "midipad/Engine.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

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
        loop.x.assign(kMaxLoopSamples, 32767);
        loop.y.assign(kMaxLoopSamples, 32767);
    }
    for (auto& events : noteEvents_)
        events.reserve(kMaxNoteEvents);
    spans_.reserve(kMaxNoteEvents);
    program_.fill(-1);
    pendingProgram_.fill(-1);
    axisProgram_.fill(-1);
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
    clockAt_ = nowNs;  // the beat clock was given for this time
    if (pendingHatDir_ && now_ >= pendingHatAt_) {
        const auto dir = pendingHatDir_;
        pendingHatDir_.reset();
        setHatDir(dir);
    }
    runLoops();
    stretchFromControls();
    // Programs chosen by axes, once settled.
    for (std::size_t k = 0; k < pendingProgram_.size(); ++k) {
        const int program = pendingProgram_[k];
        if (program < 0 || now_ - pendingSince_[k] < kProgramSettleNs)
            continue;
        pendingProgram_[k] = -1;
        const auto ch = static_cast<std::size_t>(channel_);
        const int msb = pendingBank_[k][0], lsb = pendingBank_[k][1];
        const bool bankChanged = (msb >= 0 && lastCc_[ch][0] != msb) || (lsb >= 0 && lastCc_[ch][32] != lsb);
        if (program != axisProgram_[k] || program != program_[ch] || bankChanged) {
            axisProgram_[k] = program;
            sendProgram(channel_, program, msb, lsb);
        }
    }
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
        const bool fromLoop = delayed_[next].fromLoop;
        delayed_[next] = delayed_[--delayedCount_];
        const bool saved = std::exchange(fromLoop_, fromLoop);
        sendNow(message);
        fromLoop_ = saved;
    }
}

void Engine::sendAt(std::uint64_t at, const MidiMessage& message) {
    if (at <= now_ || delayedCount_ == delayed_.size()) {
        sendNow(message);
        return;
    }
    delayed_[delayedCount_++] = {at, delayedOrder_++, message, fromLoop_};
}

void Engine::emit(const MidiMessage& message, bool fromLoop) {
    sink_.send(message);
    const int kind = message.bytes[0] & 0xF0;
    if (observer_ && (kind == 0x90 || kind == 0x80))
        observer_->noteSent(message.bytes[0] & 0x0F, message.bytes[1], kind == 0x90 ? message.bytes[2] : 0, fromLoop);
}

void Engine::sendNow(const MidiMessage& message) {
    // A note starting again first ends its previous instance, whose delayed
    // release would otherwise cut it.
    if ((message.bytes[0] & 0xF0) == 0x90) {
        for (std::size_t i = 0; i < delayedCount_;) {
            const MidiMessage& m = delayed_[i].message;
            if ((m.bytes[0] & 0xF0) == 0x80 && (m.bytes[0] & 0x0F) == (message.bytes[0] & 0x0F) &&
                m.bytes[1] == message.bytes[1]) {
                emit(m, delayed_[i].fromLoop);
                delayed_[i] = delayed_[--delayedCount_];
            } else {
                ++i;
            }
        }
    }
    emit(message, fromLoop_);
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
    if (isTrigger(axis))
        triggerPosition_[axis == Axis::LeftTrigger ? 0 : 1] = position;
    else
        stickPosition_[static_cast<std::size_t>(axis)] = position;
    if (isTrigger(axis))
        updateDigitalTrigger(axis, position);

    if (stretchHeld() && !isTrigger(axis))
        return;  // the stick stretches its loop (tick()), it does not play
    const AxisBinding& binding = mapping_->axes[static_cast<std::size_t>(axis)];
    const bool atRest = isTrigger(axis) ? position < 0.04f : std::abs(position - 0.5f) < 0.06f;
    if (binding.enabled && !isFrozen(axis))
        sendAxis(binding, position, static_cast<int>(axis), atRest);
}

void Engine::touchMoved(int finger, float x, float y) {
    if (finger < 0 || finger >= kFingerCount || std::isnan(x) || std::isnan(y))
        return;
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    fingerPosition_[static_cast<std::size_t>(finger)] = {x, y};
    fingerDown_[static_cast<std::size_t>(finger)] = true;
    const FingerBinding& binding = mapping_->fingers[static_cast<std::size_t>(finger)];

    if (stretchHeld() && binding.mode == TouchMode::Axis) {
        // The finger stretches the finger loops: a step per kStretchTouchStep of
        // its slide, right stretches. One finger at a time, from where it lands.
        // Along Y (up widens) the amplitude, whichever way it mostly slid.
        if (stretchFinger_ < 0) {
            stretchFinger_ = finger;
            stretchFingerX_ = x;
            stretchFingerY_ = y;
        } else if (stretchFinger_ == finger) {
            const float dx = x - stretchFingerX_, dy = stretchFingerY_ - y;
            if (std::abs(dx) >= std::abs(dy)) {
                const int steps = static_cast<int>(dx / kStretchTouchStep);
                if (steps != 0) {
                    stretchFingerX_ += static_cast<float>(steps) * kStretchTouchStep;
                    stretchFingerY_ = y;
                    stretchPausing(static_cast<int>(LoopSource::Finger1), steps);
                    stretchPausing(static_cast<int>(LoopSource::Finger2), steps);
                }
            } else {
                const int steps = static_cast<int>(dy / kStretchTouchStep);
                if (steps != 0) {
                    stretchFingerY_ -= static_cast<float>(steps) * kStretchTouchStep;
                    stretchFingerX_ = x;
                    amplifyPausing(static_cast<int>(LoopSource::Finger1), steps);
                    amplifyPausing(static_cast<int>(LoopSource::Finger2), steps);
                }
            }
        }
        return;
    }
    if (binding.mode == TouchMode::Axis)
        fingerHeld_[static_cast<std::size_t>(finger)] = {x, y};  // stays once lifted
    if (binding.mode == TouchMode::Axis) {
        if (binding.x.enabled)
            sendAxis(binding.x, x, kAxisCount + finger * 2, false);  // a finger rests by lifting
        if (binding.y.enabled)
            sendAxis(binding.y, 1.0f - y, kAxisCount + finger * 2 + 1, false);
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
    fingerDown_[static_cast<std::size_t>(finger)] = false;
    if (stretchFinger_ == finger)
        stretchFinger_ = -1;
    auto& current = fingerZone_[static_cast<std::size_t>(finger)];
    if (current)
        release(kZoneSlotBase + finger * kZoneCount + static_cast<int>(*current));
    current.reset();
}

void Engine::releaseAll() {
    // Releases still waiting go out now; notes not started yet never will.
    for (std::size_t i = 0; i < delayedCount_; ++i)
        if ((delayed_[i].message.bytes[0] & 0xF0) == 0x80)
            emit(delayed_[i].message, delayed_[i].fromLoop);
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
    for (std::size_t i = 0; i < loops_.size(); ++i)
        silenceLoop(i);  // they play again from their next pad
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
    stretchHolders_.reset();
    shiftLatched_ = false;
    dpadBits_ = 0;
    hatDir_.reset();
    pendingHatDir_.reset();
    fingerZone_ = {};
    fingerDown_ = {};
    triggerDown_ = {};
}

// A control's normal binding in `m` (unbound: a touchpad zone of a finger
// not in pad mode).
const TriggerBinding& Engine::normalBinding(const Mapping& m, int slot) {
    static const TriggerBinding kUnbound;
    if (slot < kHatSlotBase)
        return m.buttons[static_cast<std::size_t>(slot)];
    if (slot < kZoneSlotBase)
        return m.dpad[static_cast<std::size_t>(slot - kHatSlotBase)];
    const auto finger = static_cast<std::size_t>((slot - kZoneSlotBase) / kZoneCount);
    const auto zone = static_cast<std::size_t>((slot - kZoneSlotBase) % kZoneCount);
    return m.fingers[finger].mode == TouchMode::Zones ? m.fingers[finger].zones[zone] : kUnbound;
}

// What a control plays now. Channel 1's Shift and Loop record controls are
// the same on every channel. Otherwise, with Shift on, its Shift layer entry
// when set (never for a Shift binding itself), else its normal one.
const TriggerBinding& Engine::bindingFor(int slot) const {
    static const TriggerBinding kUnbound;
    if (const TriggerBinding& first = normalBinding(preset_->base, slot);
        first.type == ActionType::Shift || first.type == ActionType::LoopRecord)
        return first;
    const TriggerBinding* normal = &normalBinding(*mapping_, slot);
    const TriggerBinding* layered = &kUnbound;
    if (slot < kHatSlotBase) {
        layered = &mapping_->shift.buttons[static_cast<std::size_t>(slot)];
    } else if (slot < kZoneSlotBase) {
        layered = &mapping_->shift.dpad[static_cast<std::size_t>(slot - kHatSlotBase)];
    } else {
        const auto finger = static_cast<std::size_t>((slot - kZoneSlotBase) / kZoneCount);
        if (mapping_->fingers[finger].mode != TouchMode::Zones)
            return kUnbound;
        layered = &mapping_->shift.zones[finger][static_cast<std::size_t>((slot - kZoneSlotBase) % kZoneCount)];
    }
    if (shiftActive() && normal->type != ActionType::Shift && layered->isSet() && layered->type != ActionType::Shift)
        return *layered;
    return *normal;
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
    case ActionType::ProgramChange: {
        const int last = program_[static_cast<std::size_t>(channel_)];
        int program = binding.number;
        if (binding.programStep != 0) {
            program = last < 0 ? 0 : last + binding.programStep;
            program = binding.programWrap ? (program + 128) % 128 : std::clamp(program, 0, 127);
        }
        sendProgram(channel_, program, binding.bankMsb, binding.bankLsb);
        return;
    }
    case ActionType::LoopRecord:
        if (recordHolders_.none())
            beginRecordHold();
        recordHolders_.set(static_cast<std::size_t>(slot));
        return;
    case ActionType::TapTempo:
        if (const auto tempo = tapTempo_.tap(now_))
            tappedTempo_ = tempo;
        return;
    case ActionType::Shift:
        if (!stretchHeld()) {
            stickStretchRest_ = {};
            stickAmplitudeRest_ = {};
            stretchFinger_ = -1;
            lastStretchTick_ = now_;
        }
        if (binding.mode == Mode::Toggle)
            shiftLatched_ = !shiftLatched_;  // pressed once on, once off
        else
            stretchHolders_.set(static_cast<std::size_t>(slot));
        return;
    case ActionType::Note:
    case ActionType::Chord:
    case ActionType::ControlChange:
        break;
    }

    const Sounding sounding = soundingFor(binding, channel_);
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
    stretchHolders_.reset(static_cast<std::size_t>(slot));  // the stretch stays as it is
    Sounding& s = held_[static_cast<std::size_t>(slot)];
    if (s.active())
        stop(s);
    s = {};
}

Engine::Sounding Engine::soundingFor(const TriggerBinding& binding, int channel) {
    Sounding s;
    s.press = ++pressCount_ == 0 ? ++pressCount_ : pressCount_;  // never 0
    s.type = binding.type;
    s.channel = static_cast<std::uint8_t>(channel);
    s.velocity = binding.velocity;
    if (binding.type == ActionType::Note) {
        s.notes.notes[0] = binding.number;
        s.notes.count = 1;
    } else if (binding.type == ActionType::Chord) {
        s.notes = binding.chord;
    }
    if (binding.type == ActionType::Note || binding.type == ActionType::Chord) {
        // The Velocity and Transpose axes, as they are when the note starts.
        const Shaping sh = shaping(channel);
        const int base = binding.velocity;
        const float v = sh.velocity;
        s.velocity = static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(std::lround(static_cast<float>(base) + (v > 0 ? v * static_cast<float>(127 - base) : v * static_cast<float>(base - 1)))),
            1, 127));
        if (sh.transposeNotes != 0)
            for (std::size_t i = 0; i < std::min<std::size_t>(s.notes.count, s.notes.notes.size()); ++i)
                s.notes.notes[i] = static_cast<std::uint8_t>(transposed(s.notes.notes[i], sh.transposeNotes));
    } else {
        s.cc = binding.number;
        s.onValue = binding.onValue;
        s.offValue = binding.offValue;
        return s;
    }
    s.humanized = binding.humanize && humanize_.appliesTo(channel);
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
    // When the first note sounds (a humanized chord spreads its notes).
    std::uint64_t leadAt = now_;
    if (s.humanized && s.notes.count > 0)
        leadAt += static_cast<std::uint64_t>(*std::min_element(s.delayMs.begin(), s.delayMs.begin() + static_cast<std::ptrdiff_t>(s.notes.count))) *
                  1'000'000u;
    for (std::size_t i = 0; i < s.notes.count; ++i) {
        int velocity = s.velocity;
        std::uint64_t at = now_;
        if (s.humanized) {
            const int percent = randomBetween(humanize_.velocityMinPercent, humanize_.velocityMaxPercent);
            velocity = std::clamp((s.velocity * percent + 50) / 100, 1, 127);
            at += static_cast<std::uint64_t>(s.delayMs[i]) * 1'000'000u;
        }
        recordNote(s.channel, s.notes.notes[i], velocity, at, s.press, leadAt);  // into the note loop, if it records
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
        const bool late = !immediate_ && s.humanized;
        const std::uint64_t at = late ? now_ + static_cast<std::uint64_t>(s.delayMs[i]) * 1'000'000u : now_;
        recordNote(s.channel, s.notes.notes[i], 0, at, s.press);
        if (late)
            sendAt(at, off);
        else
            sendNow(off);
    }
}

void Engine::sendCc(int channel, int cc, int value) {
    value = std::clamp(value, 0, 127);
    lastCc_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(cc & 0x7F)] = static_cast<std::int16_t>(value);
    sink_.send(MidiMessage::controlChange(channel, cc, value));
}

void Engine::sendAxis(const AxisBinding& binding, float position, int key, bool atRest) {
    if (binding.invert)
        position = 1.0f - position;
    const int span = binding.max - binding.min;
    const int value = binding.min + static_cast<int>(std::lround(position * static_cast<float>(span)));
    if (binding.mode == AxisMode::Velocity || binding.mode == AxisMode::Transpose)
        return;  // read when a note starts, see shaping()
    if (binding.mode == AxisMode::Program) {
        // Chosen once the control stays on it (tick() sends it), never on the way back to rest.
        auto& pending = pendingProgram_[static_cast<std::size_t>(key)];
        if (atRest) {
            pending = -1;
        } else if (value != pending) {
            pending = value;
            pendingBank_[static_cast<std::size_t>(key)] = {binding.bankMsb, binding.bankLsb};
            pendingSince_[static_cast<std::size_t>(key)] = now_;
        }
        return;
    }
    // Controllers report far more steps than a 7-bit CC has: skip repeats.
    if (lastCc_[static_cast<std::size_t>(channel_)][binding.cc & 0x7F] == value)
        return;
    sendCc(channel_, binding.cc, value);
}

void Engine::sendProgram(int channel, int program, int bankMsb, int bankLsb) {
    if (bankMsb >= 0)
        sendCc(channel, 0, bankMsb);
    if (bankLsb >= 0)
        sendCc(channel, 32, bankLsb);
    sink_.send(MidiMessage::programChange(channel, program));
    program_[static_cast<std::size_t>(channel)] = program;
}

// --- Loops --------------------------------------------------------------------

bool Engine::sourcePosition(int source, float& x, float& y) const {
    if (source < 2) {
        x = stickPosition_[static_cast<std::size_t>(source * 2)];
        y = stickPosition_[static_cast<std::size_t>(source * 2 + 1)];
        return true;
    }
    if (source == static_cast<int>(LoopSource::Triggers)) {
        x = triggerPosition_[0];
        y = triggerPosition_[1];
        return true;
    }
    if (source == static_cast<int>(LoopSource::Notes)) {
        x = y = 0;
        return false;
    }
    const auto f = static_cast<std::size_t>(source - 2);
    x = fingerPosition_[f][0];
    y = fingerPosition_[f][1];
    return fingerDown_[f];
}

void Engine::beginRecordHold() {
    recordedThisHold_ = false;
    holdChannel_ = channel_;
    holdStartedAt_ = now_;
    for (int s = 0; s < kLoopSourceCount; ++s) {
        auto& start = holdStart_[static_cast<std::size_t>(s)];
        holdStartOn_[static_cast<std::size_t>(s)] = sourcePosition(s, start[0], start[1]);
        // A finger records its X/Y in axis mode; in 4-pad mode its pads play
        // notes, which the note loop records.
        const bool finger = s == 2 || s == 3;
        waiting_[static_cast<std::size_t>(s)] =
            !finger || preset_->forChannel(holdChannel_).fingers[static_cast<std::size_t>(s - 2)].mode == TouchMode::Axis;
    }
}

void Engine::endRecordHold() {
    waiting_.fill(false);
    for (int channel = 0; channel < kChannelCount; ++channel) {
        for (int source = 0; source < kLoopSourceCount; ++source) {
            Loop& loop = loops_[loopIndex(channel, source)];
            if (loop.state != LoopStatus::State::Recording)
                continue;
            if (loop.length < 2) {
                loop.state = LoopStatus::State::Empty;
                continue;
            }
            if (source == static_cast<int>(LoopSource::Notes)) {
                auto& events = noteEvents_[static_cast<std::size_t>(channel)];
                for (auto& e : events)  // a release delayed past the end ends with the loop
                    e.at = std::clamp(e.at, 0, loop.length - 1);
                std::stable_sort(events.begin(), events.end(), [](const NoteEvent& a, const NoteEvent& b) { return a.at < b.at; });
                // A note still on at the end (a toggle left on) ends there: so it
                // also sounds from the end when the loop plays backwards.
                std::bitset<128> held;
                for (const NoteEvent& e : events)
                    held.set(e.note, e.on);
                for (int n = 0; n < 128; ++n)
                    if (held.test(static_cast<std::size_t>(n)) && events.size() < kMaxNoteEvents)
                        events.push_back({loop.length - 1, static_cast<std::uint8_t>(n), noteVelocity_[static_cast<std::size_t>(n)], false});
                tidyNoteLoop(channel);
            }
            loop.state = LoopStatus::State::Playing;
            const LoopPlayback playback = preset_->forChannel(channel).loops[static_cast<std::size_t>(source)].playback;
            loop.position = playback == LoopPlayback::Reverse ? loop.length - 1 : 0;
            loop.phase = loop.position;
            // With the beat clock: a whole number of snaps, starting on the nearest one.
            loop.syncBeats = 0;
            if (clock_.on && snapBeats_ > 0) {
                const double snap = snapBeats_;
                const double recorded = beatAt(now_) - loop.startBeat;
                loop.syncBeats = std::max(1.0, std::round(recorded / snap)) * snap;
                loop.anchorBeat = std::round(loop.startBeat / snap) * snap;
                int dir = 1;
                syncedPhase(loop, playback, beatAt(now_), loop.phase, dir);
                loop.direction = dir;
                loop.position = std::clamp(static_cast<int>(loop.phase), 0, loop.length - 1);
                if (source == static_cast<int>(LoopSource::Notes)) {
                    quantizeNoteLoop(channel);
                    tidyNoteLoop(channel);
                }
            }
            if (source != static_cast<int>(LoopSource::Notes))
                findMiddle(loop);
            loop.direction = playback == LoopPlayback::Reverse ? -1 : 1;
            loop.nextAt = now_;
            loop.sentX = loop.sentY = -1;
        }
    }
    if (recordedThisHold_)
        return;
    // Record pressed without moving anything: a tap over a playing note loop
    // turns its overdub on or off, anything else stops the loops.
    const bool tap = now_ - holdStartedAt_ < static_cast<std::uint64_t>(kLoopTapMs) * 1'000'000u;
    const Loop& notes = loops_[loopIndex(holdChannel_, static_cast<int>(LoopSource::Notes))];
    if (tap && notes.state == LoopStatus::State::Playing)
        overdubChannel_ = overdubChannel_ == holdChannel_ ? -1 : holdChannel_;
    else
        stopLoops(holdChannel_);
}

void Engine::runLoops() {
    constexpr std::uint64_t kStep = 1'000'000'000ull / kLoopRateHz;
    // A source that moves during the hold starts recording into its loop: a
    // stick leaving its spot, a finger landing or sliding.
    for (int source = 0; source < kLoopSourceCount; ++source) {
        const auto i = static_cast<std::size_t>(source);
        if (!waiting_[i] || source == static_cast<int>(LoopSource::Notes))  // notes start when one plays
            continue;
        if (stretchHeld() && source != static_cast<int>(LoopSource::Triggers))
            continue;  // a stick or finger stretching is not a movement to record
        const bool finger = source == 2 || source == 3;
        float x = 0, y = 0;
        const bool on = sourcePosition(source, x, y);
        const bool moved = on && ((finger && !holdStartOn_[i]) || std::abs(x - holdStart_[i][0]) > kLoopMoveThreshold ||
                                  std::abs(y - holdStart_[i][1]) > kLoopMoveThreshold);
        if (!moved)
            continue;
        waiting_[i] = false;
        if (loopsPaused_)
            playLoops();
        const std::size_t index = loopIndex(holdChannel_, source);
        silenceLoop(index);
        Loop& loop = loops_[index];
        loop.state = LoopStatus::State::Recording;
        loop.length = 0;
        loop.syncBeats = 0;
        loop.startBeat = beatAt(now_);
        loop.stretch = 0;
        loop.amplitude = 100;
        loop.amplitudeNow = 1;
        loopShaping_[index] = {-1.0f, -1.0f};
        loop.nextAt = now_;
        recordedThisHold_ = true;
    }

    for (std::size_t i = 0; i < loops_.size(); ++i) {
        Loop& loop = loops_[i];
        if (loop.state == LoopStatus::State::Empty)
            continue;
        const auto channel = static_cast<int>(i / kLoopSourceCount);
        const auto source = static_cast<int>(i % kLoopSourceCount);
        if (now_ > loop.nextAt + 100 * kStep)
            loop.nextAt = now_;  // the thread stalled: do not catch up in a burst
        while (now_ >= loop.nextAt) {
            loop.nextAt += kStep;
            if (loop.state == LoopStatus::State::Recording) {
                if (source == static_cast<int>(LoopSource::Notes)) {
                    loop.length = std::min(loop.length + 1, kMaxLoopSamples);  // the notes come from recordNote
                    continue;
                }
                if (loop.length < kMaxLoopSamples) {
                    const auto at = static_cast<std::size_t>(loop.length);
                    float x = 0, y = 0;
                    const bool on = sourcePosition(source, x, y);
                    auto sample = [](float v) { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65534.0f)); };
                    loop.x[at] = on ? sample(x) : kLifted;
                    loop.y[at] = on ? sample(y) : kLifted;
                    ++loop.length;
                }
                continue;
            }
            if (loopsPaused_) {
                loop.nextAt = now_ + kStep;
                break;
            }
            // The playback as it is now: it can change while playing.
            const LoopPlayback playback = preset_->forChannel(channel).loops[static_cast<std::size_t>(source)].playback;
            // A synced loop goes where the beat clock says (at this step's time).
            const bool synced = loop.syncBeats > 0 && clock_.on;
            double phase = 0;
            int dir = 1;
            if (synced)
                syncedPhase(loop, playback, beatAt(loop.nextAt - kStep), phase, dir);
            if (source != static_cast<int>(LoopSource::Notes)) {
                fromLoop_ = true;
                playMovementLoop(i, playback, synced ? &phase : nullptr, dir);
                fromLoop_ = false;
                continue;
            }
            if (synced) {
                // Step the note loop to where the clock is, playing the notes on
                // the way; after a jump (a pause, the session moved), go straight there.
                const int target = std::clamp(static_cast<int>(phase), 0, loop.length - 1);
                int ahead = 0;
                if (playback == LoopPlayback::BackAndForth) {
                    // Straight there: the clock turns it round at the ends.
                    ahead = std::abs(target - loop.position);
                    if (target != loop.position)
                        dir = target > loop.position ? 1 : -1;
                } else {
                    ahead = dir > 0 ? (target - loop.position + loop.length) % loop.length
                                    : (loop.position - target + loop.length) % loop.length;
                }
                if (ahead > std::max(4, loop.length / 4)) {
                    fromLoop_ = true;
                    silenceLoop(i);
                    fromLoop_ = false;
                    loop.position = target;
                    loop.direction = dir;
                    continue;
                }
                loop.direction = dir;
                for (int k = 0; k < ahead; ++k) {
                    fromLoop_ = true;
                    playLoopNotes(i);
                    fromLoop_ = false;
                    const int next = loop.position + dir;
                    bool startsOver = false;
                    if (playback == LoopPlayback::BackAndForth) {
                        loop.position = std::clamp(next, 0, loop.length - 1);
                        startsOver = loop.position == 0 && dir < 0;
                    } else {
                        loop.position = (next + loop.length) % loop.length;
                        startsOver = dir > 0 ? loop.position == 0 : loop.position == loop.length - 1;
                    }
                    if (startsOver) {
                        fromLoop_ = true;
                        silenceLoop(i);  // what still sounds stops when the loop starts over
                        fromLoop_ = false;
                    }
                }
                continue;
            }
            fromLoop_ = true;
            playLoopNotes(i);
            fromLoop_ = false;
            // Then step.
            bool startsOver = false;
            if (playback == LoopPlayback::Forward) {
                loop.direction = 1;
                loop.position = (loop.position + 1) % loop.length;
                startsOver = loop.position == 0;
            } else if (playback == LoopPlayback::Reverse) {
                loop.direction = -1;
                loop.position = (loop.position - 1 + loop.length) % loop.length;
                startsOver = loop.position == loop.length - 1;
            } else {
                loop.position += loop.direction;
                if (loop.position >= loop.length) {
                    loop.direction = -1;
                    loop.position = std::max(0, loop.length - 2);
                } else if (loop.position < 0) {
                    loop.direction = 1;
                    loop.position = std::min(1, loop.length - 1);
                    startsOver = true;
                }
            }
            if (startsOver && source == static_cast<int>(LoopSource::Notes)) {
                fromLoop_ = true;
                silenceLoop(i);  // what still sounds stops when the loop starts over
                fromLoop_ = false;
            }
        }
    }
}

namespace {

// The sample `k` of a loop of `length` samples, `k` possibly outside it: wrapped
// around for a loop playing in one direction, mirrored for one going back and forth.
int loopSampleIndex(int k, int length, LoopPlayback playback) {
    if (playback != LoopPlayback::BackAndForth)
        return (k % length + length) % length;
    const int last = length - 1;
    if (last == 0)
        return 0;
    const int period = 2 * last;
    k = (k % period + period) % period;
    return k <= last ? k : period - k;
}

// Monotone cubic interpolation (Fritsch-Butland) between p1 and p2 at f in
// [0, 1), p0 and p3 around them: smooth, and never beyond its neighbours, so a
// stretched gesture keeps its shape without overshooting what was recorded.
float monotoneCubic(float p0, float p1, float p2, float p3, float f) {
    auto slope = [](float a, float b) { return a * b <= 0.0f ? 0.0f : 2.0f * a * b / (a + b); };
    const float m1 = slope(p1 - p0, p2 - p1), m2 = slope(p2 - p1, p3 - p2);
    const float f2 = f * f, f3 = f2 * f;
    return (2 * f3 - 3 * f2 + 1) * p1 + (f3 - 2 * f2 + f) * m1 + (-2 * f3 + 3 * f2) * p2 + (f3 - f2) * m2;
}

}  // namespace

double Engine::stretchFactor(int stretch) {
    const double k = 1.0 + 0.5 * std::abs(std::clamp(stretch, kMinStretch, kMaxStretch));
    return stretch >= 0 ? k : 1.0 / k;
}

void Engine::stretchLoop(int channel, int source, int steps) {
    if (source < 0 || source >= kLoopSourceCount || source == static_cast<int>(LoopSource::Notes))
        return;
    Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), source)];
    if (loop.state != LoopStatus::State::Playing)
        return;
    int next = loop.stretch + steps;
    if ((loop.stretch > 0 && next < 0) || (loop.stretch < 0 && next > 0))
        next = 0;  // stop at 1x on the way
    loop.stretch = std::clamp(next, kMinStretch, kMaxStretch);
}

int Engine::transposeSteps(int semitones) const {
    semitones = std::clamp(semitones, 0, kMaxTranspose);
    if (!key_)
        return semitones;
    const auto degrees = static_cast<int>(key_->type().intervals.size());
    return semitones * degrees / 12;  // 12 semitones: an octave of degrees
}

int Engine::transposedIn(const std::optional<Key>& key, int note, int steps) {
    int out = note + steps;
    if (key && steps != 0) {
        const auto& iv = key->type().intervals;
        const auto n = static_cast<int>(iv.size());
        const int rel = note - key->root;
        const int octave = rel >= 0 ? rel / 12 : -((-rel + 11) / 12);
        const int pc = rel - octave * 12;
        // The nearest degree (the octave above counts as degree n).
        int degree = 0;
        for (int d = 0; d <= n; ++d) {
            const int at = d < n ? iv[static_cast<std::size_t>(d)] : 12;
            if (std::abs(at - pc) < std::abs((degree < n ? iv[static_cast<std::size_t>(degree)] : 12) - pc))
                degree = d;
        }
        const int target = octave * n + degree + steps;
        const int targetOctave = target >= 0 ? target / n : -((-target + n - 1) / n);
        out = key->root + targetOctave * 12 + iv[static_cast<std::size_t>(target - targetOctave * n)];
    }
    while (out > 127)
        out -= 12;
    while (out < 0)
        out += 12;
    return out;
}

Engine::Shaping Engine::shaping(int channel) const {
    Shaping sh;
    channel = std::clamp(channel, 0, kChannelCount - 1);
    const Mapping& m = preset_->forChannel(channel);
    const bool live = channel == channel_;
    // One axis at position p in [0, 1]: a stick or finger from its middle, a trigger from rest.
    auto add = [&](const AxisBinding& a, float p, bool trigger, bool stick) {
        if (!a.enabled || p < 0.0f)
            return;
        if (a.invert)
            p = 1.0f - p;
        float v = trigger ? p : (p - 0.5f) * 2.0f;
        if (stick && std::abs(v) < 0.12f)
            v = 0;  // a stick at rest changes nothing
        if (a.mode == AxisMode::Velocity) {
            sh.velocity += v;
        } else if (a.mode == AxisMode::Transpose) {
            const int steps = static_cast<int>(std::lround(v * static_cast<float>(transposeSteps(v > 0 ? a.transposeUp : a.transposeDown))));
            (a.transposeTarget == TransposeTarget::Loop ? sh.transposeLoop : sh.transposeNotes) += steps;
        }
    };
    const auto ch = static_cast<std::size_t>(channel);
    for (int i = 0; i < kAxisCount; ++i) {
        const auto axis = static_cast<Axis>(i);
        const AxisBinding& a = m.axes[static_cast<std::size_t>(i)];
        if (a.mode != AxisMode::Velocity && a.mode != AxisMode::Transpose)
            continue;
        const bool trigger = isTrigger(axis);
        if (live) {
            const float p = (frozen_[ch] & axisBit(axis)) ? frozenAt_[ch][static_cast<std::size_t>(i)]
                            : trigger ? triggerPosition_[axis == Axis::LeftTrigger ? 0 : 1]
                                      : stickPosition_[static_cast<std::size_t>(i)];
            add(a, p, trigger, !trigger);
        }
        // Its loop: the sticks' X / Y, the triggers' L2 / R2.
        const int source = trigger ? static_cast<int>(LoopSource::Triggers) : (i < 2 ? 0 : 1);
        const int xy = trigger ? (axis == Axis::LeftTrigger ? 0 : 1) : i % 2;
        add(a, loopShaping_[loopIndex(channel, source)][static_cast<std::size_t>(xy)], trigger, false);
    }
    for (int f = 0; f < kFingerCount; ++f) {
        const FingerBinding& fb = m.fingers[static_cast<std::size_t>(f)];
        if (fb.mode != TouchMode::Axis)
            continue;
        const auto& held = fingerHeld_[static_cast<std::size_t>(f)];
        const auto& looped = loopShaping_[loopIndex(channel, 2 + f)];
        for (int xy = 0; xy < 2; ++xy) {
            const AxisBinding& a = xy == 0 ? fb.x : fb.y;
            if (a.mode != AxisMode::Velocity && a.mode != AxisMode::Transpose)
                continue;
            if (live)
                add(a, xy == 0 ? held[0] : 1.0f - held[1], false, false);
            add(a, looped[static_cast<std::size_t>(xy)], false, false);
        }
    }
    sh.velocity = std::clamp(sh.velocity, -1.0f, 1.0f);
    return sh;
}

void Engine::amplifyLoop(int channel, int source, int steps) {
    if (source < 0 || source >= kLoopSourceCount || source == static_cast<int>(LoopSource::Notes))
        return;
    Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), source)];
    if (loop.state != LoopStatus::State::Playing)
        return;
    // Step by step: 5 % below 100 %, 25 % above; across 100 % it stops there.
    int a = loop.amplitude;
    const int dir = steps > 0 ? 1 : -1;
    for (int i = 0; i != steps; i += dir) {
        const int next = dir > 0 ? (a < 100 ? a + kAmplitudeStep : a + kAmplitudeStepAbove)
                                 : (a > 100 ? a - kAmplitudeStepAbove : a - kAmplitudeStep);
        if ((a < 100 && next > 100) || (a > 100 && next < 100) || (a != 100 && next == 100)) {
            a = 100;
            break;
        }
        a = std::clamp(next, 0, kMaxAmplitude);
    }
    loop.amplitude = a;
}

float Engine::shapeAmplitude(float v, float middle, float a) {
    float out = middle + (v - middle) * a;
    if (a <= 1.0f)
        return std::clamp(out, 0.0f, 1.0f);
    // A soft knee near each end, growing with the amplitude (nothing at 100 %,
    // a tenth of the travel from 200 %): the curve bends towards the end, then
    // stays flat on it.
    const float k = 0.1f * std::min(1.0f, a - 1.0f);
    if (out > 1.0f - k)
        out = 1.0f - k * std::exp(-(out - (1.0f - k)) / k);
    else if (out < k)
        out = k * std::exp((out - k) / k);
    return std::clamp(out, 0.0f, 1.0f);
}

// Changes a loop's amplitude on the active channel, waiting kStretchPauseMs at
// 100 % when it gets there, like the stretch at 1x.
void Engine::amplifyPausing(int source, int steps) {
    const auto i = static_cast<std::size_t>(source);
    if (now_ < amplitudePausedUntil_[i])
        return;
    const Loop& loop = loops_[loopIndex(channel_, source)];
    const int before = loop.amplitude;
    amplifyLoop(channel_, source, steps);
    if (before != 100 && loop.amplitude == 100)
        amplitudePausedUntil_[i] = now_ + static_cast<std::uint64_t>(kStretchPauseMs) * 1'000'000u;
}

// Stretches a loop of the active channel, waiting kStretchPauseMs at 1x when it
// gets there, so a stick held over makes it easy to stop on.
void Engine::stretchPausing(int source, int steps) {
    const auto i = static_cast<std::size_t>(source);
    if (now_ < stretchPausedUntil_[i])
        return;
    const Loop& loop = loops_[loopIndex(channel_, source)];
    const int before = loop.stretch;
    stretchLoop(channel_, source, steps);
    if (before != 0 && loop.stretch == 0)
        stretchPausedUntil_[i] = now_ + static_cast<std::uint64_t>(kStretchPauseMs) * 1'000'000u;
}

// While LoopStretch is held, each stick's X turns its loop's stretch like a
// wheel kept turning: nothing in the dead zone, then faster the further out.
void Engine::stretchFromControls() {
    const double dt = std::min(static_cast<double>(now_ - lastStretchTick_) * 1e-9, 0.05);
    lastStretchTick_ = now_;
    if (!stretchHeld())
        return;
    for (int source = 0; source < 2; ++source) {
        const auto i = static_cast<std::size_t>(source);
        auto& rest = stickStretchRest_[i];
        auto& ampRest = stickAmplitudeRest_[i];
        const float dx = (stickPosition_[i * 2] - 0.5f) * 2.0f;         // -1 left .. 1 right
        const float dy = (0.5f - stickPosition_[i * 2 + 1]) * 2.0f;     // -1 down .. 1 up
        const bool alongX = std::abs(dx) >= std::abs(dy);               // only the way it mostly moves
        const float d = alongX ? dx : dy, out = std::abs(d);
        if (out <= kStretchDeadZone) {
            rest = ampRest = 0;
            continue;
        }
        const double t = static_cast<double>((out - kStretchDeadZone) / (1.0f - kStretchDeadZone));
        if (alongX) {
            ampRest = 0;
            const double rate = kStretchMinRate + (kStretchMaxRate - kStretchMinRate) * t * t;
            rest += (d > 0 ? rate : -rate) * dt;
            const int steps = static_cast<int>(rest);
            if (steps != 0) {
                rest -= steps;
                stretchPausing(source, steps);
            }
        } else {
            rest = 0;
            const double rate = kStretchMinRate + (kAmplitudeMaxRate - kStretchMinRate) * t * t;
            ampRest += (d > 0 ? rate : -rate) * dt;
            const int steps = static_cast<int>(ampRest);
            if (steps != 0) {
                ampRest -= steps;
                amplifyPausing(source, steps);
            }
        }
    }
}

void Engine::syncedPhase(const Loop& loop, LoopPlayback playback, double beat, double& phase, int& direction) const {
    const double span = loop.syncBeats * stretchFactor(loop.stretch);  // one pass, in beats
    const double u = (beat - loop.anchorBeat) / span;
    const double length = loop.length;
    if (playback == LoopPlayback::BackAndForth) {
        const double p = u - 2.0 * std::floor(u / 2.0);  // [0, 2)
        direction = p < 1.0 ? 1 : -1;
        phase = (p < 1.0 ? p : 2.0 - p) * (length - 1);
        return;
    }
    const double f = u - std::floor(u);  // [0, 1)
    direction = playback == LoopPlayback::Reverse ? -1 : 1;
    phase = playback == LoopPlayback::Reverse ? (1.0 - f) * length : f * length;
    if (phase >= length)
        phase -= length;
}

// Plays one step of a movement loop: sends its value at the current point,
// then moves on by one sample, divided by the stretch factor. Slower than
// recorded, the value is interpolated between samples; faster, it is the
// average of the samples passed over, so nothing flickers.
void Engine::playMovementLoop(std::size_t index, LoopPlayback playback, const double* syncedPhase, int syncedDirection) {
    Loop& loop = loops_[index];
    const int length = loop.length;
    // The amplitude eases in: at most a tenth of the recorded swing a step
    // (50 ms from 100 % to 0 %), faster when widening far.
    const float target = static_cast<float>(loop.amplitude) / 100.0f;
    const float ease = 0.1f * std::max(1.0f, std::max(target, loop.amplitudeNow) / 2.0f);
    loop.amplitudeNow += std::clamp(target - loop.amplitudeNow, -ease, ease);
    auto send = [&](float x, float y) {
        const float a = loop.amplitudeNow;
        sendLoopValue(index, shapeAmplitude(x, loop.middleX, a), shapeAmplitude(y, loop.middleY, a));
    };
    double step = 1.0 / stretchFactor(loop.stretch);
    if (syncedPhase) {
        // Synced: the clock gives the point; the step is how far it moved since.
        double moved = std::abs(*syncedPhase - loop.phase);
        moved = std::min(moved, static_cast<double>(length) - moved);
        step = std::max(moved, 1e-6);
        loop.phase = *syncedPhase;
        loop.direction = syncedDirection;
    }
    auto at = [&](const std::vector<std::uint16_t>& v, int k) { return v[static_cast<std::size_t>(loopSampleIndex(k, length, playback))]; };
    auto value = [](std::uint16_t v) { return static_cast<float>(v) / 65534.0f; };

    const int i = static_cast<int>(std::floor(loop.phase));
    if (step <= 1.0) {
        // Between samples i and i + 1 in the loop's own order.
        const float f = static_cast<float>(loop.phase - i);
        if (at(loop.x, i) != kLifted) {  // nothing while a recorded finger was lifted
            const bool nextOn = at(loop.x, i + 1) != kLifted;
            auto interpolate = [&](const std::vector<std::uint16_t>& v) {
                const float p1 = value(at(v, i));
                if (!nextOn)
                    return p1;  // hold up to the lift
                const float p2 = value(at(v, i + 1));
                const float p0 = at(v, i - 1) == kLifted ? p1 : value(at(v, i - 1));
                const float p3 = at(v, i + 2) == kLifted ? p2 : value(at(v, i + 2));
                return std::clamp(monotoneCubic(p0, p1, p2, p3, f), 0.0f, 1.0f);
            };
            send(interpolate(loop.x), interpolate(loop.y));
        }
    } else {
        // The samples this step passes over, in the playing direction.
        const int count = static_cast<int>(std::ceil(step));
        const int dir = playback == LoopPlayback::Reverse ? -1 : (playback == LoopPlayback::Forward ? 1 : loop.direction);
        float sx = 0, sy = 0;
        int on = 0;
        for (int j = 0; j < count; ++j) {
            const int k = i + dir * j;
            if (at(loop.x, k) == kLifted)
                continue;
            sx += value(at(loop.x, k));
            sy += value(at(loop.y, k));
            ++on;
        }
        if (on > 0)
            send(sx / static_cast<float>(on), sy / static_cast<float>(on));
    }

    // Move on (a synced loop is moved by the clock).
    if (syncedPhase) {
        loop.position = std::clamp(static_cast<int>(loop.phase), 0, std::max(0, length - 1));
        return;
    }
    if (length < 2) {
        loop.phase = 0;
    } else if (playback == LoopPlayback::Forward) {
        loop.direction = 1;
        loop.phase = std::fmod(loop.phase + step, static_cast<double>(length));
    } else if (playback == LoopPlayback::Reverse) {
        loop.direction = -1;
        loop.phase -= step;
        while (loop.phase < 0)
            loop.phase += length;
    } else {
        const double last = length - 1;
        loop.phase += loop.direction * step;
        while (loop.phase > last || loop.phase < 0) {
            if (loop.phase > last) {
                loop.phase = 2 * last - loop.phase;
                loop.direction = -1;
            } else {
                loop.phase = -loop.phase;
                loop.direction = 1;
            }
        }
    }
    loop.position = std::clamp(static_cast<int>(loop.phase), 0, std::max(0, length - 1));
}

// Sends a movement loop's X, Y in [0, 1] on its CCs and channel, scaled like
// the axes it was recorded from.
void Engine::sendLoopValue(std::size_t index, float px, float py) {
    Loop& loop = loops_[index];
    const auto channel = static_cast<int>(index / kLoopSourceCount);
    const auto source = static_cast<int>(index % kLoopSourceCount);
    const Mapping& m = preset_->forChannel(channel);
    const LoopBinding& binding = m.loops[static_cast<std::size_t>(source)];
    auto value = [](const AxisBinding& a, float position) {
        if (a.invert)
            position = 1.0f - position;
        const int lo = a.enabled ? a.min : 0, hi = a.enabled ? a.max : 127;
        return lo + static_cast<int>(std::lround(position * static_cast<float>(hi - lo)));
    };
    const AxisBinding *bx = nullptr, *by = nullptr;
    if (source < 2) {
        bx = &m.axes[static_cast<std::size_t>(source == 0 ? Axis::LeftX : Axis::RightX)];
        by = &m.axes[static_cast<std::size_t>(source == 0 ? Axis::LeftY : Axis::RightY)];
    } else if (source == static_cast<int>(LoopSource::Triggers)) {
        bx = &m.axes[static_cast<std::size_t>(Axis::LeftTrigger)];
        by = &m.axes[static_cast<std::size_t>(Axis::RightTrigger)];
    } else {
        const FingerBinding& f = m.fingers[static_cast<std::size_t>(source - 2)];
        bx = &f.x;
        by = &f.y;
        py = 1.0f - py;  // the touchpad's Y grows downwards, its CC upwards
    }
    // An axis set to Velocity or Transpose plays its loop as that, not as a CC.
    auto shapes = [](const AxisBinding& a) { return a.enabled && (a.mode == AxisMode::Velocity || a.mode == AxisMode::Transpose); };
    loopShaping_[index] = {shapes(*bx) ? px : -1.0f, shapes(*by) ? py : -1.0f};
    const int vx = value(*bx, px), vy = value(*by, py);
    if (!shapes(*bx) && vx != loop.sentX) {
        sendCc(channel, binding.ccX, vx);
        loop.sentX = vx;
    }
    if (!shapes(*by) && vy != loop.sentY) {
        sendCc(channel, binding.ccY, vy);
        loop.sentY = vy;
    }
}

// Stops what a note loop has on.
void Engine::silenceLoop(std::size_t index) {
    if (index % kLoopSourceCount == static_cast<std::size_t>(LoopSource::Notes)) {
        const auto channel = static_cast<int>(index / kLoopSourceCount);
        auto& on = noteLoopOn_[static_cast<std::size_t>(channel)];
        const auto& shift = loopShift_[static_cast<std::size_t>(channel)];
        for (int n = 0; n < 128; ++n)
            if (on.test(static_cast<std::size_t>(n)))
                sendNow(MidiMessage::noteOff(channel, n + shift[static_cast<std::size_t>(n)]));
        on.reset();
    }
}

// Records a note played on `channel` at `at` (velocity 0: its release) into
// the channel's note loop, starting it if Record is held and waits for notes.
void Engine::recordNote(int channel, int note, int velocity, std::uint64_t at, std::uint32_t press, std::uint64_t leadAt) {
    constexpr auto kNotes = static_cast<std::size_t>(LoopSource::Notes);
    const std::size_t index = loopIndex(channel, static_cast<int>(kNotes));
    Loop& loop = loops_[index];
    if (velocity > 0 && waiting_[kNotes] && channel == holdChannel_) {
        waiting_[kNotes] = false;
        if (loopsPaused_)
            playLoops();
        silenceLoop(index);
        loop.state = LoopStatus::State::Recording;
        loop.length = 0;
        loop.syncBeats = 0;
        loop.startBeat = beatAt(now_);
        loop.nextAt = now_;
        noteRecordStart_ = now_;
        noteEvents_[static_cast<std::size_t>(channel)].clear();
        recordedThisHold_ = true;
        if (overdubChannel_ == channel)
            overdubChannel_ = -1;
    }
    if (loop.state == LoopStatus::State::Playing && channel == overdubChannel_) {
        overdubNote(channel, note, velocity, at, press, leadAt);
        return;
    }
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    if (loop.state != LoopStatus::State::Recording || events.size() >= kMaxNoteEvents || at < noteRecordStart_)
        return;
    constexpr std::uint64_t kStep = 1'000'000'000ull / kLoopRateHz;
    const auto sample = static_cast<std::int32_t>((at - noteRecordStart_) / kStep);
    const auto n = static_cast<std::size_t>(note & 0x7F);
    if (velocity > 0)
        noteVelocity_[n] = static_cast<std::uint8_t>(velocity);
    events.push_back({sample, static_cast<std::uint8_t>(n), noteVelocity_[n], velocity > 0, press});
}

// The grid step of a synced note loop, in its samples: the loop's beats span
// its length. 0 when it is free or the quantizer is off.
double Engine::quantizeStep(const Loop& loop) const {
    if (quantize_ <= 0 || loop.syncBeats <= 0 || loop.length < 2)
        return 0;
    return static_cast<double>(loop.length) / (loop.syncBeats * quantize_);
}

// Puts the notes of a note loop just recorded on the grid, press by press:
// its first note on the nearest step (the loop's end wraps to its start), its
// other notes and releases moved alike, so the lengths and a humanized chord's
// spread stay as played.
void Engine::quantizeNoteLoop(int channel) {
    const Loop& loop = loops_[loopIndex(channel, static_cast<int>(LoopSource::Notes))];
    const double step = quantizeStep(loop);
    if (step <= 0)
        return;
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    auto slot = [&](std::uint32_t press) -> PressShift& { return pressShift_[press % pressShift_.size()]; };
    for (const NoteEvent& e : events)  // each press's first note
        if (e.on && e.press != 0) {
            PressShift& p = slot(e.press);
            if (p.press != e.press)
                p = {e.press, e.at, 0};
            else
                p.first = std::min(p.first, e.at);
        }
    for (NoteEvent& e : events) {
        if (e.press == 0 || slot(e.press).press != e.press)
            continue;  // a release of a note started before the recording
        const PressShift& p = slot(e.press);
        int target = static_cast<int>(std::lround(std::round(p.first / step) * step));
        if (target >= loop.length)
            target -= loop.length;  // on the loop's start
        e.at = std::clamp(e.at + target - p.first, 0, loop.length - 1);
    }
    for (PressShift& p : pressShift_)
        p = {};
    std::stable_sort(events.begin(), events.end(), [](const NoteEvent& a, const NoteEvent& b) { return a.at < b.at; });
}

// Adds a note played over a channel's note loop where the loop plays it,
// kept in the loop's order: played backwards, its release is where it starts.
void Engine::overdubNote(int channel, int note, int velocity, std::uint64_t at, std::uint32_t press, std::uint64_t leadAt) {
    const Loop& loop = loops_[loopIndex(channel, static_cast<int>(LoopSource::Notes))];
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    if (events.size() >= kMaxNoteEvents || loop.length < 2)
        return;
    constexpr std::uint64_t kStep = 1'000'000'000ull / kLoopRateHz;
    const auto ahead = static_cast<int>((at > now_ ? at - now_ : 0) / kStep);  // a humanized delay
    int position = ((loop.position + loop.direction * ahead) % loop.length + loop.length) % loop.length;
    // Quantized: the press moves by as much as its first note needs to land on the grid.
    if (const double step = quantizeStep(loop); step > 0 && press != 0) {
        PressShift& p = pressShift_[press % pressShift_.size()];
        if (velocity > 0 && p.press != press) {
            const auto lead = static_cast<int>((leadAt > now_ ? leadAt - now_ : 0) / kStep);
            const int first = ((loop.position + loop.direction * lead) % loop.length + loop.length) % loop.length;
            p = {press, first, static_cast<std::int32_t>(std::lround(std::round(first / step) * step)) - first};
        }
        if (p.press == press)
            position = ((position + p.shift) % loop.length + loop.length) % loop.length;
    }
    const auto n = static_cast<std::size_t>(note & 0x7F);
    if (velocity > 0)
        noteVelocity_[n] = static_cast<std::uint8_t>(velocity);
    const bool on = loop.direction > 0 ? velocity > 0 : velocity == 0;
    const NoteEvent event{position, static_cast<std::uint8_t>(n), noteVelocity_[n], on, press};
    const auto where = std::upper_bound(events.begin(), events.end(), position,
                                        [](int at2, const NoteEvent& e) { return at2 < e.at; });
    events.insert(where, event);  // the vector has its capacity: no allocation
    if (velocity == 0)  // a note complete
        tidyNoteLoop(channel);
}

void Engine::tidyNoteLoop(int channel) {
    const Loop& loop = loops_[loopIndex(channel, static_cast<int>(LoopSource::Notes))];
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    if (loop.length < 2)
        return;
    // The notes as spans: each release closes the note of its press (a
    // release of no press, the first one on of its pitch).
    spans_.clear();
    constexpr int kOpen = 8;  // notes of one pitch on at once, at most
    std::array<std::array<std::int16_t, kOpen>, 128> open{};
    std::array<std::uint8_t, 128> openCount{};
    for (const NoteEvent& e : events) {
        auto& list = open[e.note];
        auto& count = openCount[e.note];
        if (e.on) {
            if (count < kOpen)
                list[count++] = static_cast<std::int16_t>(spans_.size());
            spans_.push_back({e.at, -1, e.note, e.velocity, e.press});
            continue;
        }
        int found = -1;
        for (int k = 0; k < count && found < 0; ++k)
            if (spans_[static_cast<std::size_t>(list[static_cast<std::size_t>(k)])].press == e.press)
                found = k;
        if (found < 0 && e.press == 0 && count > 0)
            found = 0;
        if (found < 0) {
            spans_.push_back({-1, e.at, e.note, e.velocity, e.press});  // its note is not in the loop: kept as it is
            continue;
        }
        spans_[static_cast<std::size_t>(list[static_cast<std::size_t>(found)])].end = e.at;
        std::copy(list.begin() + found + 1, list.begin() + count, list.begin() + found);
        --count;
    }
    // Overlaps on a pitch: the earlier note ends where the next starts; two
    // starting together merge.
    std::array<int, 128> last;
    last.fill(-1);
    for (std::size_t k = 0; k < spans_.size(); ++k) {
        NoteSpan& s = spans_[k];
        if (s.start < 0)
            continue;
        if (int& l = last[s.note]; l >= 0) {
            NoteSpan& prev = spans_[static_cast<std::size_t>(l)];
            if (prev.end < 0 || prev.end > s.start) {
                if (prev.start >= s.start) {
                    s.end = prev.end < 0 || s.end < 0 ? -1 : std::max(prev.end, s.end);
                    prev.start = -2;  // merged into `s`
                } else {
                    prev.end = s.start;
                }
            }
        }
        last[s.note] = static_cast<int>(k);
    }
    events.clear();
    for (const NoteSpan& s : spans_) {
        if (s.start == -2)
            continue;
        if (s.start < 0) {
            events.push_back({s.end, s.note, s.velocity, false, s.press});
            continue;
        }
        const int start = std::min(s.start, loop.length - 2);  // room for its release after it
        events.push_back({start, s.note, s.velocity, true, s.press});
        if (s.end >= 0)
            events.push_back({std::clamp(s.end, start + 1, loop.length - 1), s.note, s.velocity, false, s.press});
    }
    // In loop order; where a note ends and the next starts, the release first.
    std::stable_sort(events.begin(), events.end(),
                     [](const NoteEvent& a, const NoteEvent& b) { return a.at != b.at ? a.at < b.at : !a.on && b.on; });
}

// A note loop at its current step: its notes there start or stop; played
// backwards, a release starts its note and a start stops it.
void Engine::playLoopNotes(std::size_t index) {
    const Loop& loop = loops_[index];
    const auto channel = static_cast<int>(index / kLoopSourceCount);
    const auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    auto& on = noteLoopOn_[static_cast<std::size_t>(channel)];
    const auto lo = std::lower_bound(events.begin(), events.end(), loop.position,
                                     [](const NoteEvent& e, int at) { return e.at < at; });
    auto hi = lo;
    while (hi != events.end() && hi->at == loop.position)
        ++hi;
    auto& shift = loopShift_[static_cast<std::size_t>(channel)];
    const int steps = shaping(channel).transposeLoop;  // a Transpose axis set on the loop
    auto play = [&](const NoteEvent& e, bool start) {
        if (start) {
            if (on.test(e.note))  // the same note again: the one on stops first
                sendNow(MidiMessage::noteOff(channel, e.note + shift[e.note]));
            const int note = transposed(e.note, steps);
            shift[e.note] = static_cast<std::int8_t>(note - e.note);
            sendNow(MidiMessage::noteOn(channel, note, e.velocity));
            on.set(e.note);
        } else if (on.test(e.note)) {
            sendNow(MidiMessage::noteOff(channel, e.note + shift[e.note]));
            on.reset(e.note);
        }
    };
    if (loop.direction > 0)
        for (auto it = lo; it != hi; ++it)
            play(*it, it->on);
    else
        for (auto it = hi; it != lo;) {
            --it;
            play(*it, !it->on);
        }
}

bool Engine::findLoopNote(int channel, int start, int note, int& on, int& off) const {
    const auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    on = off = -1;
    for (std::size_t i = 0; i < events.size(); ++i)
        if (events[i].on && events[i].at == start && events[i].note == note) {
            on = static_cast<int>(i);
            break;
        }
    if (on < 0)
        return false;
    for (std::size_t i = static_cast<std::size_t>(on) + 1; i < events.size(); ++i)
        if (events[i].note == note) {  // notes of a pitch never overlap: its release
            if (!events[i].on)
                off = static_cast<int>(i);
            break;
        }
    return true;
}

void Engine::silenceLoopNote(int channel, int note) {
    auto& on = noteLoopOn_[static_cast<std::size_t>(channel)];
    if (!on.test(static_cast<std::size_t>(note)))
        return;
    fromLoop_ = true;
    sendNow(MidiMessage::noteOff(channel, note + loopShift_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(note)]));
    fromLoop_ = false;
    on.reset(static_cast<std::size_t>(note));
}

bool Engine::moveLoopNotes(int channel, const std::vector<LoopNoteRef>& notes, int samples, int semitones, bool byDegrees) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    const Loop& loop = loops_[loopIndex(channel, static_cast<int>(LoopSource::Notes))];
    if (loop.state != LoopStatus::State::Playing || loop.length < 2)
        return false;
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    // The notes there, by index (note on, release or -1), and how far the
    // group may go: all of it inside the loop and the MIDI range.
    struct Found {
        int on, off;
    };
    std::vector<Found> found;
    int first = loop.length, last = 0, low = 127, high = 0;
    for (const LoopNoteRef& ref : notes) {
        int on = -1, off = -1;
        if (!findLoopNote(channel, ref.start, ref.note, on, off))
            continue;
        found.push_back({on, off});
        const int end = off >= 0 ? std::max(events[static_cast<std::size_t>(off)].at, ref.start + 1) : ref.start + 1;
        first = std::min(first, ref.start);
        last = std::max(last, end);
        low = std::min<int>(low, ref.note);
        high = std::max<int>(high, ref.note);
    }
    if (found.empty())
        return false;
    samples = std::clamp(samples, -first, std::max(loop.length - 1 - last, -first));
    if (!byDegrees)
        semitones = std::clamp(semitones, -low, 127 - high);
    auto moved = [&](int note) { return byDegrees ? transposed(note, semitones) : std::clamp(note + semitones, 0, 127); };
    for (const Found& f : found) {
        silenceLoopNote(channel, events[static_cast<std::size_t>(f.on)].note);
        silenceLoopNote(channel, moved(events[static_cast<std::size_t>(f.on)].note));
    }
    for (const Found& f : found) {
        const int note = moved(events[static_cast<std::size_t>(f.on)].note);  // its release goes with it
        for (const int i : {f.on, f.off}) {
            if (i < 0)
                continue;
            auto& e = events[static_cast<std::size_t>(i)];
            e.at = std::clamp(e.at + samples, 0, loop.length - 1);
            e.note = static_cast<std::uint8_t>(note);
        }
    }
    tidyNoteLoop(channel);  // in order again; notes they land on are cut short
    return true;
}

bool Engine::resizeLoopNotes(int channel, const std::vector<LoopNoteRef>& notes, int samples) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    const Loop& loop = loops_[loopIndex(channel, static_cast<int>(LoopSource::Notes))];
    if (loop.state != LoopStatus::State::Playing || loop.length < 2)
        return false;
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    bool any = false;
    for (const LoopNoteRef& ref : notes) {
        int on = -1, off = -1;
        if (!findLoopNote(channel, ref.start, ref.note, on, off) || off < 0)
            continue;
        silenceLoopNote(channel, ref.note);  // it sounds again from its next start
        auto& end = events[static_cast<std::size_t>(off)];
        end.at = std::clamp(end.at + samples, std::min(ref.start + 1, loop.length - 1), loop.length - 1);
        any = true;
    }
    if (any)
        tidyNoteLoop(channel);  // in order again; a note now reaching the next of its pitch stops there
    return any;
}

bool Engine::removeLoopNotes(int channel, const std::vector<LoopNoteRef>& notes) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    bool any = false;
    for (const LoopNoteRef& ref : notes) {
        int on = -1, off = -1;
        if (!findLoopNote(channel, ref.start, ref.note, on, off))
            continue;
        silenceLoopNote(channel, ref.note);
        events[static_cast<std::size_t>(on)].at = -1;  // marked, removed below
        if (off >= 0)
            events[static_cast<std::size_t>(off)].at = -1;
        any = true;
    }
    events.erase(std::remove_if(events.begin(), events.end(), [](const NoteEvent& e) { return e.at < 0; }), events.end());
    return any;
}

int Engine::noteLoopIntervals(int channel, NoteInterval* out, int max) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    const Loop& loop = loops_[loopIndex(channel, static_cast<int>(LoopSource::Notes))];
    if (loop.state == LoopStatus::State::Empty)
        return 0;
    std::array<int, 128> open;
    open.fill(-1);
    int count = 0;
    for (const NoteEvent& e : noteEvents_[static_cast<std::size_t>(channel)]) {
        if (e.on) {
            if (count == max)
                break;
            open[e.note] = count;
            out[count++] = {e.at, -1, e.note, e.velocity, false, e.press};
        } else if (open[e.note] >= 0) {
            out[open[e.note]].end = std::max(e.at, out[open[e.note]].start + 1);
            open[e.note] = -1;
        }
    }
    for (int i = 0; i < count; ++i)  // still on when the loop ends (or now, while recording)
        if (out[i].end < 0) {
            out[i].end = std::max(loop.length, out[i].start + 1);
            out[i].open = true;
        }
    return count;
}

LoopStatus Engine::loopStatus(int channel, int source) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    const Loop& loop = loops_[loopIndex(channel, source)];
    const bool waiting = waiting_[static_cast<std::size_t>(source)] && holdChannel_ == channel;
    const bool overdub = source == static_cast<int>(LoopSource::Notes) && overdubChannel_ == channel;
    return {loop.state, waiting, overdub, loop.length, loop.position, loop.direction, channel, loop.stretch, loop.amplitude,
            loop.syncBeats > 0 ? loop.syncBeats * stretchFactor(loop.stretch) : 0.0};
}

float Engine::loopSample(int channel, int source, int axis, int index) const {
    const Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), std::clamp(source, 0, kLoopSourceCount - 1))];
    const auto i = static_cast<std::size_t>(std::clamp(index, 0, kMaxLoopSamples - 1));
    const std::uint16_t v = axis == 0 ? loop.x[i] : loop.y[i];
    return v == kLifted ? -1.0f : static_cast<float>(v) / 65534.0f;
}

void Engine::clearLoop(int channel, int source) {
    const std::size_t index = loopIndex(std::clamp(channel, 0, kChannelCount - 1), std::clamp(source, 0, kLoopSourceCount - 1));
    silenceLoop(index);
    Loop& loop = loops_[index];
    loop.state = LoopStatus::State::Empty;
    loop.length = 0;
    loop.stretch = 0;
    loop.amplitude = 100;
    loop.amplitudeNow = 1;
    loopShaping_[index] = {-1.0f, -1.0f};
    if (index % kLoopSourceCount == static_cast<std::size_t>(LoopSource::Notes) &&
        overdubChannel_ == static_cast<int>(index / kLoopSourceCount))
        overdubChannel_ = -1;
}

void Engine::stopLoops(int channel) {
    for (int source = 0; source < kLoopSourceCount; ++source)
        if (loops_[loopIndex(channel, source)].state == LoopStatus::State::Playing)
            clearLoop(channel, source);
}

void Engine::pauseLoops() {
    if (loopsPaused_)
        return;
    loopsPaused_ = true;
    for (auto& v : loopShaping_)
        v = {-1.0f, -1.0f};
    fromLoop_ = true;
    for (std::size_t i = 0; i < loops_.size(); ++i)
        silenceLoop(i);  // the notes the note loops have on
    fromLoop_ = false;
}

void Engine::playLoops() {
    loopsPaused_ = false;
    for (int channel = 0; channel < kChannelCount; ++channel) {
        for (int source = 0; source < kLoopSourceCount; ++source) {
            const std::size_t index = loopIndex(channel, source);
            Loop& loop = loops_[index];
            if (loop.state != LoopStatus::State::Playing)
                continue;
            fromLoop_ = true;
            silenceLoop(index);
            fromLoop_ = false;
            const bool reverse = preset_->forChannel(channel).loops[static_cast<std::size_t>(source)].playback == LoopPlayback::Reverse;
            loop.position = reverse ? loop.length - 1 : 0;
            loop.phase = loop.position;
            loop.direction = reverse ? -1 : 1;
            loop.nextAt = now_;
            loop.sentX = loop.sentY = -1;
        }
    }
}

// Its middle, where the amplitude closes in or widens.
void Engine::findMiddle(Loop& loop) {
    std::uint16_t lx = 0xFFFE, hx = 0, ly = 0xFFFE, hy = 0;
    for (int k = 0; k < loop.length; ++k) {
        const auto at = static_cast<std::size_t>(k);
        if (loop.x[at] == kLifted)
            continue;
        lx = std::min(lx, loop.x[at]), hx = std::max(hx, loop.x[at]);
        ly = std::min(ly, loop.y[at]), hy = std::max(hy, loop.y[at]);
    }
    if (lx <= hx) {
        loop.middleX = (static_cast<float>(lx) + static_cast<float>(hx)) * 0.5f / 65534.0f;
        loop.middleY = (static_cast<float>(ly) + static_cast<float>(hy)) * 0.5f / 65534.0f;
    }
}

std::vector<SessionLoop> Engine::sessionLoops() const {
    std::vector<SessionLoop> out;
    for (int channel = 0; channel < kChannelCount; ++channel) {
        for (int source = 0; source < kLoopSourceCount; ++source) {
            const Loop& loop = loops_[loopIndex(channel, source)];
            if (loop.state != LoopStatus::State::Playing || loop.length < 2)
                continue;
            SessionLoop l;
            l.channel = channel;
            l.source = static_cast<LoopSource>(source);
            l.length = loop.length;
            l.syncBeats = loop.syncBeats;
            l.anchorBeat = loop.anchorBeat;
            if (l.source == LoopSource::Notes) {
                for (const NoteEvent& e : noteEvents_[static_cast<std::size_t>(channel)])
                    l.notes.push_back({e.at, e.note, e.velocity, e.on, e.press});
            } else {
                l.stretch = loop.stretch;
                l.amplitude = loop.amplitude;
                l.x.assign(loop.x.begin(), loop.x.begin() + loop.length);
                l.y.assign(loop.y.begin(), loop.y.begin() + loop.length);
            }
            out.push_back(std::move(l));
        }
    }
    return out;
}

void Engine::loadSessionLoops(const std::vector<SessionLoop>& loops) {
    waiting_.fill(false);
    recordHolders_.reset();
    overdubChannel_ = -1;
    fromLoop_ = true;
    for (std::size_t i = 0; i < loops_.size(); ++i)
        clearLoop(static_cast<int>(i / kLoopSourceCount), static_cast<int>(i % kLoopSourceCount));
    fromLoop_ = false;
    for (const SessionLoop& l : loops) {
        if (l.channel < 0 || l.channel >= kChannelCount || l.length < 2 || l.length > kMaxLoopSamples)
            continue;
        const int source = static_cast<int>(l.source);
        Loop& loop = loops_[loopIndex(l.channel, source)];
        if (l.source == LoopSource::Notes) {
            auto& events = noteEvents_[static_cast<std::size_t>(l.channel)];
            events.clear();
            for (const SessionLoop::Note& n : l.notes)
                if (events.size() < kMaxNoteEvents)
                    events.push_back({std::clamp(n.at, 0, l.length - 1), static_cast<std::uint8_t>(n.note & 0x7F), n.velocity, n.on, n.press});
            for (const SessionLoop::Note& n : l.notes)  // new presses never take a number of these
                pressCount_ = std::max(pressCount_, n.press);
        } else {
            if (static_cast<int>(l.x.size()) < l.length || static_cast<int>(l.y.size()) < l.length)
                continue;
            std::copy(l.x.begin(), l.x.begin() + l.length, loop.x.begin());
            std::copy(l.y.begin(), l.y.begin() + l.length, loop.y.begin());
            loop.stretch = std::clamp(l.stretch, kMinStretch, kMaxStretch);
            loop.amplitude = std::clamp(l.amplitude, 0, kMaxAmplitude);
            loop.amplitudeNow = static_cast<float>(loop.amplitude) / 100.0f;
        }
        loop.length = l.length;
        loop.syncBeats = l.syncBeats;
        loop.anchorBeat = l.anchorBeat;
        loop.state = LoopStatus::State::Playing;
        if (l.source != LoopSource::Notes)
            findMiddle(loop);
    }
    playLoops();  // from their start; synced ones take their place from the beat clock
}

std::uint16_t Engine::channelsWithLoops() const {
    std::uint16_t bits = 0;
    for (std::size_t i = 0; i < loops_.size(); ++i)
        if (loops_[i].state != LoopStatus::State::Empty)
            bits = static_cast<std::uint16_t>(bits | (1u << (i / kLoopSourceCount)));
    return bits;
}

void Engine::stopLoops() {
    for (int channel = 0; channel < kChannelCount; ++channel)
        stopLoops(channel);
}

void Engine::toggleFreeze(std::uint8_t axes) {
    auto& frozen = frozen_[static_cast<std::size_t>(channel_)];
    for (int a = 0; a < kAxisCount; ++a)  // where newly frozen axes stay, for Velocity / Transpose
        if ((axes & axisBit(static_cast<Axis>(a))) && !(frozen & axisBit(static_cast<Axis>(a))))
            frozenAt_[static_cast<std::size_t>(channel_)][static_cast<std::size_t>(a)] =
                isTrigger(static_cast<Axis>(a)) ? triggerPosition_[a == static_cast<int>(Axis::LeftTrigger) ? 0 : 1]
                                                : stickPosition_[static_cast<std::size_t>(a)];
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
