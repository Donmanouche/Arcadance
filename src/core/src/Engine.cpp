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
        if (stretchFinger_ < 0) {
            stretchFinger_ = finger;
            stretchFingerX_ = x;
        } else if (stretchFinger_ == finger) {
            const int steps = static_cast<int>((x - stretchFingerX_) / kStretchTouchStep);
            if (steps != 0) {
                stretchFingerX_ += static_cast<float>(steps) * kStretchTouchStep;
                stretchPausing(static_cast<int>(LoopSource::Finger1), steps);
                stretchPausing(static_cast<int>(LoopSource::Finger2), steps);
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
    dpadBits_ = 0;
    hatDir_.reset();
    pendingHatDir_.reset();
    fingerZone_ = {};
    fingerDown_ = {};
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
    case ActionType::LoopStretch:
        if (stretchHolders_.none()) {
            stickStretchRest_ = {};
            stretchFinger_ = -1;
            lastStretchTick_ = now_;
        }
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
    for (std::size_t i = 0; i < s.notes.count; ++i) {
        int velocity = s.velocity;
        std::uint64_t at = now_;
        if (s.humanized) {
            const int percent = randomBetween(humanize_.velocityMinPercent, humanize_.velocityMaxPercent);
            velocity = std::clamp((s.velocity * percent + 50) / 100, 1, 127);
            at += static_cast<std::uint64_t>(s.delayMs[i]) * 1'000'000u;
        }
        recordNote(s.channel, s.notes.notes[i], velocity, at);  // into the note loop, if it records
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
        recordNote(s.channel, s.notes.notes[i], 0, at);
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
            }
            loop.state = LoopStatus::State::Playing;
            const LoopPlayback playback = preset_->forChannel(channel).loops[static_cast<std::size_t>(source)].playback;
            loop.position = playback == LoopPlayback::Reverse ? loop.length - 1 : 0;
            loop.phase = loop.position;
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
        loop.stretch = 0;
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
            if (source != static_cast<int>(LoopSource::Notes)) {
                fromLoop_ = true;
                playMovementLoop(i, playback);
                fromLoop_ = false;
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

int Engine::transposed(int note, int steps) const {
    int out = note + steps;
    if (key_ && steps != 0) {
        const auto& iv = key_->type().intervals;
        const auto n = static_cast<int>(iv.size());
        const int rel = note - key_->root;
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
        out = key_->root + targetOctave * 12 + iv[static_cast<std::size_t>(target - targetOctave * n)];
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
        auto& rest = stickStretchRest_[static_cast<std::size_t>(source)];
        const float d = (stickPosition_[static_cast<std::size_t>(source * 2)] - 0.5f) * 2.0f;  // -1 left .. 1 right
        const float out = std::abs(d);
        if (out <= kStretchDeadZone) {
            rest = 0;
            continue;
        }
        const double t = static_cast<double>((out - kStretchDeadZone) / (1.0f - kStretchDeadZone));
        const double rate = kStretchMinRate + (kStretchMaxRate - kStretchMinRate) * t * t;
        rest += (d > 0 ? rate : -rate) * dt;
        const int steps = static_cast<int>(rest);
        if (steps != 0) {
            rest -= steps;
            stretchPausing(source, steps);
        }
    }
}

// Plays one step of a movement loop: sends its value at the current point,
// then moves on by one sample, divided by the stretch factor. Slower than
// recorded, the value is interpolated between samples; faster, it is the
// average of the samples passed over, so nothing flickers.
void Engine::playMovementLoop(std::size_t index, LoopPlayback playback) {
    Loop& loop = loops_[index];
    const int length = loop.length;
    const double step = 1.0 / stretchFactor(loop.stretch);
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
            sendLoopValue(index, interpolate(loop.x), interpolate(loop.y));
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
            sendLoopValue(index, sx / static_cast<float>(on), sy / static_cast<float>(on));
    }

    // Move on.
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
void Engine::recordNote(int channel, int note, int velocity, std::uint64_t at) {
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
        loop.nextAt = now_;
        noteRecordStart_ = now_;
        noteEvents_[static_cast<std::size_t>(channel)].clear();
        recordedThisHold_ = true;
        if (overdubChannel_ == channel)
            overdubChannel_ = -1;
    }
    if (loop.state == LoopStatus::State::Playing && channel == overdubChannel_) {
        overdubNote(channel, note, velocity, at);
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
    events.push_back({sample, static_cast<std::uint8_t>(n), noteVelocity_[n], velocity > 0});
}

// Adds a note played over a channel's note loop where the loop plays it,
// kept in the loop's order: played backwards, its release is where it starts.
void Engine::overdubNote(int channel, int note, int velocity, std::uint64_t at) {
    const Loop& loop = loops_[loopIndex(channel, static_cast<int>(LoopSource::Notes))];
    auto& events = noteEvents_[static_cast<std::size_t>(channel)];
    if (events.size() >= kMaxNoteEvents || loop.length < 2)
        return;
    constexpr std::uint64_t kStep = 1'000'000'000ull / kLoopRateHz;
    const auto ahead = static_cast<int>((at > now_ ? at - now_ : 0) / kStep);  // a humanized delay
    const int position = ((loop.position + loop.direction * ahead) % loop.length + loop.length) % loop.length;
    const auto n = static_cast<std::size_t>(note & 0x7F);
    if (velocity > 0)
        noteVelocity_[n] = static_cast<std::uint8_t>(velocity);
    const bool on = loop.direction > 0 ? velocity > 0 : velocity == 0;
    const NoteEvent event{position, static_cast<std::uint8_t>(n), noteVelocity_[n], on};
    const auto where = std::upper_bound(events.begin(), events.end(), position,
                                        [](int at2, const NoteEvent& e) { return at2 < e.at; });
    events.insert(where, event);  // the vector has its capacity: no allocation
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
            out[count++] = {e.at, -1, e.note, e.velocity, false};
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
    return {loop.state, waiting, overdub, loop.length, loop.position, loop.direction, channel, loop.stretch};
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
