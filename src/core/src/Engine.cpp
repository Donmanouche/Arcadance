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
    for (std::size_t i = 0; i < loops_.size(); ++i) {
        if (trackOfLoop(i) == kNoteTrack)
            continue;  // its notes are in noteEvents_
        loops_[i].x.assign(kMaxLoopSamples, 32767);
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
    // Does any control open the session screen? Without one, PS / Guide does.
    presetHasSession_ = false;
    auto look = [this](const Mapping& m) {
        auto is = [](const TriggerBinding& b) { return b.type == ActionType::SessionScreen; };
        for (const auto& b : m.buttons) presetHasSession_ = presetHasSession_ || is(b);
        for (const auto& b : m.dpad) presetHasSession_ = presetHasSession_ || is(b);
        for (const auto& b : m.shift.buttons) presetHasSession_ = presetHasSession_ || is(b);
        for (const auto& b : m.shift.dpad) presetHasSession_ = presetHasSession_ || is(b);
        for (std::size_t f = 0; f < m.fingers.size(); ++f)
            for (std::size_t z = 0; z < m.fingers[f].zones.size(); ++z)
                presetHasSession_ = presetHasSession_ || (m.fingers[f].mode == TouchMode::Zones && is(m.fingers[f].zones[z])) ||
                                    is(m.shift.zones[f][z]);
    };
    look(preset_->base);
    for (const auto& m : preset_->channels)
        if (m)
            look(*m);
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
    // Shift + a tap on Record: its stop, once no second tap came.
    if (pendingStopChannel_ >= 0 && now_ >= pendingStopAt_ &&
        (recordHolders_.none() || now_ - holdStartedAt_ >= static_cast<std::uint64_t>(kLoopTapMs) * 1'000'000u))
        flushPendingStop();
    // Clips waiting for the end of the current one's turn (before this tick's steps).
    if (!loopsPaused_)
        for (int channel = 0; channel < kChannelCount; ++channel) {
            const auto c = static_cast<std::size_t>(channel);
            if (queued_[c] < 0)
                continue;
            double turns = 0;
            if (!clipPlaying_[c] || !clipTurns(channel, turns) || static_cast<long long>(std::floor(turns)) > turnMark_[c])
                switchClip(channel, queued_[c]);
        }
    if (!loopsPaused_)
        runChains();
    if (modeSlot_ >= 0 && !modeHelp_ && now_ - modeAt_ >= static_cast<std::uint64_t>(kHelpHoldMs) * 1'000'000u) {
        modeHelp_ = true;  // held: its help
        pushSessionEvent(SessionEvent::HelpOn);
    }
    runLoops();
    stretchFromControls();
    navigate();
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

    if (navigating() && !isTrigger(axis))
        return;  // the sticks move the session screen's cursor (tick())
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

    if (stretchHeld() && binding.mode == TouchMode::Axis && recordHolders_.any())
        return;  // Shift + Record: the finger locks its loops (runLoops)
    if (stretchHeld() && binding.mode == TouchMode::Axis) {
        // The finger changes the finger loops: a step per kStretchTouchStep of
        // its slide, sideways their X loops, up and down their Y loops (right
        // and up lengthen or widen); their lengths, or (after Shift + the
        // touchpad's click) their amplitudes. One finger at a time, from where it lands.
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
                    for (int t : {trackOf(2, 0), trackOf(3, 0)})
                        fingerAmplitudes_ ? amplifyPausing(t, steps) : stretchPausing(t, steps);
                }
            } else {
                const int steps = static_cast<int>(dy / kStretchTouchStep);
                if (steps != 0) {
                    stretchFingerY_ -= static_cast<float>(steps) * kStretchTouchStep;
                    stretchFingerX_ = x;
                    for (int t : {trackOf(2, 1), trackOf(3, 1)})
                        fingerAmplitudes_ ? amplifyPausing(t, steps) : stretchPausing(t, steps);
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
    shiftTapAt_ = 0;
    pendingStopChannel_ = -1;
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
        first.type == ActionType::Shift || first.type == ActionType::LoopRecord || first.type == ActionType::SessionScreen)
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
    // The session screen: its control (PS / Guide without one); while it
    // shows, L3, R3 and the D-pad drive it, unless Record is held.
    const TriggerBinding& binding = bindingFor(slot);
    if (binding.type == ActionType::SessionScreen || (!presetHasSession_ && slot == slotOf(Button::Guide))) {
        modeSlot_ = slot;  // its event when let go (or its help, held)
        modeAt_ = now_;
        modeShift_ = stretchHeld();
        modeHelp_ = false;
        return;
    }
    if (stretchHeld() && recordHolders_.none() && !navigating()) {  // Shift + a click: lengths or amplitudes
        if (slot == slotOf(Button::LeftStick) || slot == slotOf(Button::RightStick)) {
            auto& amplitudes = stickAmplitudes_[slot == slotOf(Button::LeftStick) ? 0 : 1];
            amplitudes = !amplitudes;
            return;
        }
        if (slot == slotOf(Button::Touchpad)) {
            fingerAmplitudes_ = !fingerAmplitudes_;
            return;
        }
    }
    if (navigating()) {
        // A D-pad direction chosen: its D-pad slot (it then no longer moves the cursor).
        auto navSlot = [](Button b) {
            switch (b) {
            case Button::DpadUp: return slotOf(HatDir::Up);
            case Button::DpadDown: return slotOf(HatDir::Down);
            case Button::DpadLeft: return slotOf(HatDir::Left);
            case Button::DpadRight: return slotOf(HatDir::Right);
            default: return slotOf(b);
            }
        };
        auto is = [&](const std::optional<Button>& b) { return b && slot == navSlot(*b); };
        if (is(navSelect_) || is(navMenu_) || is(navBack_)) {
            pushSessionEvent(is(navSelect_) ? SessionEvent::Press : is(navMenu_) ? SessionEvent::Menu : SessionEvent::Back);
            return;
        }
    }
    if (navigating() && !navSelect_) {
        if (slot == slotOf(Button::LeftStick) || slot == slotOf(Button::RightStick)) {
            pushSessionEvent(sessionValues_ && slot == slotOf(Button::RightStick) ? SessionEvent::PressRight : SessionEvent::Press);
            return;
        }
    }
    if (navigating()) {
        if (slot >= kHatSlotBase && slot < kZoneSlotBase) {
            static constexpr std::optional<SessionEvent> kMoves[kHatDirCount] = {
                SessionEvent::Up, std::nullopt, SessionEvent::Right, std::nullopt,
                SessionEvent::Down, std::nullopt, SessionEvent::Left, std::nullopt};
            hatNav_ = kMoves[static_cast<std::size_t>(slot - kHatSlotBase)];
            if (hatNav_) {
                pushSessionEvent(*hatNav_);
                hatNavNext_ = now_ + static_cast<std::uint64_t>(kNavRepeatDelayMs) * 1'000'000u;
            }
            return;
        }
    }

    switch (binding.type) {
    case ActionType::None:
    case ActionType::SessionScreen:
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
    if (slot == modeSlot_) {  // the Mode control let go
        pushSessionEvent(modeHelp_ ? SessionEvent::HelpOff : modeShift_ ? SessionEvent::Edit : SessionEvent::Toggle);
        modeSlot_ = -1;
        return;
    }
    if (slot >= kHatSlotBase && slot < kZoneSlotBase)
        hatNav_.reset();
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
        // Voice leading: the inversion nearest to the chord played before on this channel.
        ChordNotes& last = lastChord_[static_cast<std::size_t>(channel)];
        s.notes = binding.chordVoiceLeading
                      ? voiceLead(binding.chord, std::span(last.notes.data() + last.bassCount, last.count - last.bassCount))
                      : binding.chord;
        last = s.notes;
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

bool Engine::trackPosition(int track, float& v) const {
    if (track == kNoteTrack) {
        v = 0;
        return false;
    }
    float x = 0, y = 0;
    const bool on = sourcePosition(sourceOfTrack(track), x, y);
    v = axisOfTrack(track) == 0 ? x : y;
    return on;
}

bool Engine::trackEnabled(int channel, int track) const {
    if (track == kNoteTrack)
        return true;
    const Mapping& m = preset_->forChannel(channel);
    const int source = sourceOfTrack(track), axis = axisOfTrack(track);
    if (source < 2)
        return m.axes[static_cast<std::size_t>(source * 2 + axis)].enabled;
    if (source == static_cast<int>(LoopSource::Triggers))
        return m.axes[static_cast<std::size_t>(axis == 0 ? Axis::LeftTrigger : Axis::RightTrigger)].enabled;
    const FingerBinding& f = m.fingers[static_cast<std::size_t>(source - 2)];
    return f.mode == TouchMode::Axis && (axis == 0 ? f.x : f.y).enabled;
}

void Engine::beginRecordHold() {
    recordedThisHold_ = false;
    lockedThisHold_ = false;
    lockToggled_.fill(false);
    holdChannel_ = channel_;
    holdStartedAt_ = now_;
    for (int t = 0; t < kTrackCount; ++t) {
        const auto i = static_cast<std::size_t>(t);
        holdStartOn_[i] = trackPosition(t, holdStart_[i]);
        // An axis records when enabled (a finger in axis mode; in 4-pad mode its
        // pads play notes, which the note loop records), and not locked.
        waiting_[i] = trackEnabled(holdChannel_, t) &&
                      !loops_[slotIndex(holdChannel_, recordSlot_[static_cast<std::size_t>(holdChannel_)], t)].locked;
    }
}

void Engine::endRecordHold() {
    waiting_.fill(false);
    for (int channel = 0; channel < kChannelCount; ++channel) {
        bool finished = false;  // a recording of this channel's current clip ends: it plays
        for (int track = 0; track < kTrackCount; ++track) {
            const int source = sourceOfTrack(track);
            Loop& loop = loops_[loopIndex(channel, track)];
            if (loop.state != LoopStatus::State::Recording)
                continue;
            if (loop.length < 2) {
                loop.state = LoopStatus::State::Empty;
                continue;
            }
            if (source == static_cast<int>(LoopSource::Notes)) {
                auto& events = eventsOf(channel);
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
            loop.sentX = -1;
            loop.turns = 0;
            finished = true;
        }
        if (finished && !clipPlaying_[static_cast<std::size_t>(channel)]) {
            // The clip was stopped (or new): its other loops start with this one.
            for (int track = 0; track < kTrackCount; ++track) {
                Loop& other = loops_[loopIndex(channel, track)];
                if (other.state == LoopStatus::State::Playing && other.nextAt != now_) {
                    const bool reverse = preset_->forChannel(channel).loops[static_cast<std::size_t>(sourceOfTrack(track))].playback == LoopPlayback::Reverse;
                    other.position = reverse ? other.length - 1 : 0;
                    other.phase = other.position;
                    other.direction = reverse ? -1 : 1;
                    other.nextAt = now_;
                    other.sentX = -1;
                    other.turns = 0;
                }
            }
            clipPlaying_[static_cast<std::size_t>(channel)] = true;
        }
    }
    if (recordedThisHold_ || lockedThisHold_)
        return;
    // Record pressed without moving anything: a tap over a playing note loop
    // turns its overdub on or off (not a locked one's), anything else stops
    // the loops (not the locked ones).
    const bool tap = now_ - holdStartedAt_ < static_cast<std::uint64_t>(kLoopTapMs) * 1'000'000u;
    constexpr int kNotes = static_cast<int>(LoopSource::Notes);
    const Loop& notes = loops_[loopIndex(holdChannel_, kNoteTrack)];
    const bool notesPlaying = notes.state == LoopStatus::State::Playing;
    if (tap && stretchHeld()) {
        // Shift + two taps: the note loop locks or unlocks, the first tap taken back.
        const std::uint64_t window = static_cast<std::uint64_t>(kDoubleTapMs) * 1'000'000u;
        if (shiftTapAt_ != 0 && shiftTapChannel_ == holdChannel_ && holdStartedAt_ - shiftTapAt_ <= window) {
            shiftTapAt_ = 0;
            overdubChannel_ = overdubBeforeTap_;
            pendingStopChannel_ = -1;
            toggleLoopLock(holdChannel_, kNotes);
            return;
        }
        flushPendingStop();
        shiftTapAt_ = now_;
        shiftTapChannel_ = holdChannel_;
        overdubBeforeTap_ = overdubChannel_;
        if (notesPlaying) {
            if (!notes.locked)
                overdubChannel_ = overdubChannel_ == holdChannel_ ? -1 : holdChannel_;
        } else {  // a stop cannot be taken back: it waits for a second tap
            pendingStopChannel_ = holdChannel_;
            pendingStopAt_ = now_ + window;
        }
        return;
    }
    shiftTapAt_ = 0;
    flushPendingStop();
    if (tap && notesPlaying) {
        if (!notes.locked)
            overdubChannel_ = overdubChannel_ == holdChannel_ ? -1 : holdChannel_;
    } else {
        stopLoops(holdChannel_);
    }
}

void Engine::flushPendingStop() {
    if (pendingStopChannel_ < 0)
        return;
    const int channel = std::exchange(pendingStopChannel_, -1);
    stopLoops(channel);
}

// Shift + Record: each stick, the triggers or a finger in axis mode moving
// (as it would start a recording) locks or unlocks its loops, once a hold.
void Engine::lockFromControls() {
    // Each axis on its own, moved that way (a finger: slid from where it landed).
    for (int track = 0; track < kNoteTrack; ++track) {
        const auto i = static_cast<std::size_t>(track);
        const int source = sourceOfTrack(track);
        const bool finger = source == 2 || source == 3;
        if (lockToggled_[i])
            continue;
        if (finger && preset_->forChannel(holdChannel_).fingers[static_cast<std::size_t>(source - 2)].mode != TouchMode::Axis)
            continue;
        float v = 0;
        if (!trackPosition(track, v))
            continue;
        if (finger && !holdStartOn_[i]) {  // it just landed: from here
            holdStart_[i] = v;
            holdStartOn_[i] = true;
            continue;
        }
        if (std::abs(v - holdStart_[i]) <= kLoopMoveThreshold)
            continue;
        lockToggled_[i] = true;
        if (toggleLoopLock(holdChannel_, source, axisOfTrack(track)))
            lockedThisHold_ = true;
    }
}

void Engine::runLoops() {
    constexpr std::uint64_t kStep = 1'000'000'000ull / kLoopRateHz;
    if (stretchHeld() && recordHolders_.any())
        lockFromControls();
    // A source that moves during the hold starts recording into its loop: a
    // stick leaving its spot, a finger landing or sliding.
    for (int track = 0; track < kNoteTrack; ++track) {  // (notes start when one plays)
        const auto i = static_cast<std::size_t>(track);
        const int source = sourceOfTrack(track);
        if (!waiting_[i])
            continue;
        if (stretchHeld())
            continue;  // under Shift, moving stretches a loop or (with Record) locks it
        if (navigating() && (source == static_cast<int>(LoopSource::LeftStick) || source == static_cast<int>(LoopSource::RightStick)))
            continue;  // it moves the session screen's cursor
        if (loops_[slotIndex(holdChannel_, recordSlot_[static_cast<std::size_t>(holdChannel_)], track)].locked) {  // locked during the hold
            waiting_[i] = false;
            continue;
        }
        // Each axis on its own: a stick leaving its spot that way, a finger landing or sliding.
        const bool finger = source == 2 || source == 3;
        float v = 0;
        const bool on = trackPosition(track, v);
        const bool moved = on && ((finger && !holdStartOn_[i]) || std::abs(v - holdStart_[i]) > kLoopMoveThreshold);
        if (!moved)
            continue;
        waiting_[i] = false;
        flushPendingStop();
        enterRecordSlot(holdChannel_);  // a new clip: there, the current one stops
        if (loopsPaused_)
            playLoops();
        const std::size_t index = loopIndex(holdChannel_, track);
        silenceLoop(index);
        Loop& loop = loops_[index];
        loop.state = LoopStatus::State::Recording;
        loop.length = 0;
        loop.syncBeats = 0;
        loop.startBeat = beatAt(now_);
        loop.stretch = 0;
        loop.amplitude = 100;
        loop.amplitudeNow = 1;
        loopShaping_[index] = -1.0f;
        loop.nextAt = now_;
        recordedThisHold_ = true;
    }

    for (std::size_t i = 0; i < loops_.size(); ++i) {
        Loop& loop = loops_[i];
        if (loop.state == LoopStatus::State::Empty)
            continue;
        const auto channel = channelOfLoop(i);
        const auto track = trackOfLoop(i);
        const auto source = sourceOfTrack(track);
        // Only each channel's current clip runs, while it plays (or records).
        if (slotOfLoop(i) != currentSlot_[static_cast<std::size_t>(channel)] ||
            (!clipPlaying_[static_cast<std::size_t>(channel)] && loop.state != LoopStatus::State::Recording))
            continue;
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
                    float v = 0;
                    const bool on = trackPosition(track, v);
                    loop.x[static_cast<std::size_t>(loop.length)] =
                        on ? static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65534.0f)) : kLifted;
                    ++loop.length;
                }
                continue;
            }
            if (loopsPaused_) {
                loop.nextAt = now_ + kStep;
                break;
            }
            loop.turns += 1.0 / (static_cast<double>(std::max(loop.length, 1)) *
                                 (source == static_cast<int>(LoopSource::Notes) ? 1.0 : stretchFactor(loop.stretch)));
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

void Engine::stretchLoop(int channel, int source, int axis, int steps) {
    if (source < 0 || source >= kLoopSourceCount || source == static_cast<int>(LoopSource::Notes))
        return;
    Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), trackOf(source, axis))];
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
            if (a.velocityTarget != VelocityTarget::Loop)
                sh.velocity += v;
            if (a.velocityTarget != VelocityTarget::Notes)
                sh.velocityLoop += v;
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
        add(a, loopShaping_[loopIndex(channel, trackOf(source, xy))], trigger, false);
    }
    for (int f = 0; f < kFingerCount; ++f) {
        const FingerBinding& fb = m.fingers[static_cast<std::size_t>(f)];
        if (fb.mode != TouchMode::Axis)
            continue;
        const auto& held = fingerHeld_[static_cast<std::size_t>(f)];

        for (int xy = 0; xy < 2; ++xy) {
            const AxisBinding& a = xy == 0 ? fb.x : fb.y;
            if (a.mode != AxisMode::Velocity && a.mode != AxisMode::Transpose)
                continue;
            if (live)
                add(a, xy == 0 ? held[0] : 1.0f - held[1], false, false);
            add(a, loopShaping_[loopIndex(channel, trackOf(2 + f, xy))], false, false);
        }
    }
    sh.velocity = std::clamp(sh.velocity, -1.0f, 1.0f);
    sh.velocityLoop = std::clamp(sh.velocityLoop, -1.0f, 1.0f);
    return sh;
}

void Engine::amplifyLoop(int channel, int source, int axis, int steps) {
    if (source < 0 || source >= kLoopSourceCount || source == static_cast<int>(LoopSource::Notes))
        return;
    Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), trackOf(source, axis))];
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
void Engine::amplifyPausing(int track, int steps) {
    const auto i = static_cast<std::size_t>(track);
    if (now_ < amplitudePausedUntil_[i])
        return;
    const Loop& loop = loops_[loopIndex(channel_, track)];
    const int before = loop.amplitude;
    amplifyLoop(channel_, sourceOfTrack(track), axisOfTrack(track), steps);
    if (before != 100 && loop.amplitude == 100)
        amplitudePausedUntil_[i] = now_ + static_cast<std::uint64_t>(kStretchPauseMs) * 1'000'000u;
}

// Stretches a loop of the active channel, waiting kStretchPauseMs at 1x when it
// gets there, so a stick held over makes it easy to stop on.
void Engine::stretchPausing(int track, int steps) {
    const auto i = static_cast<std::size_t>(track);
    if (now_ < stretchPausedUntil_[i])
        return;
    const Loop& loop = loops_[loopIndex(channel_, track)];
    const int before = loop.stretch;
    stretchLoop(channel_, sourceOfTrack(track), axisOfTrack(track), steps);
    if (before != 0 && loop.stretch == 0)
        stretchPausedUntil_[i] = now_ + static_cast<std::uint64_t>(kStretchPauseMs) * 1'000'000u;
}

// While LoopStretch is held, each stick's X turns its loop's stretch like a
// wheel kept turning: nothing in the dead zone, then faster the further out.
void Engine::stretchFromControls() {
    const double dt = std::min(static_cast<double>(now_ - lastStretchTick_) * 1e-9, 0.05);
    lastStretchTick_ = now_;
    if (!stretchHeld() || recordHolders_.any())  // Shift + Record: the sticks lock their loops
        return;
    if (navigating())
        return;  // the sticks drive the session screen
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
        // Along X its X loop, along Y its Y loop: their lengths, or their amplitudes.
        const int track = trackOf(source, alongX ? 0 : 1);
        auto& turned = alongX ? rest : ampRest;
        (alongX ? ampRest : rest) = 0;
        const double t = static_cast<double>((out - kStretchDeadZone) / (1.0f - kStretchDeadZone));
        const bool amplitudes = stickAmplitudes_[i];
        const double rate = kStretchMinRate + ((amplitudes ? kAmplitudeMaxRate : kStretchMaxRate) - kStretchMinRate) * t * t;
        turned += (d > 0 ? rate : -rate) * dt;
        const int steps = static_cast<int>(turned);
        if (steps != 0) {
            turned -= steps;
            amplitudes ? amplifyPausing(track, steps) : stretchPausing(track, steps);
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
    auto send = [&](float v) { sendLoopValue(index, stepped(index, shapeAmplitude(v, loop.middleX, loop.amplitudeNow))); };
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

    int i = static_cast<int>(std::floor(loop.phase));
    // In steps: each holds the value where it starts on the loop's beat grid.
    if (const int stepSamples = stepLength(index); stepSamples > 0) {
        const int k = i / stepSamples * stepSamples;
        if (at(loop.x, k) != kLifted)
            send(value(at(loop.x, k)));
    } else if (step <= 1.0) {
        // Between samples i and i + 1 in the loop's own order.
        const float f = static_cast<float>(loop.phase - i);
        if (at(loop.x, i) != kLifted) {  // nothing while a recorded finger was lifted
            const auto& v = loop.x;
            const float p1 = value(at(v, i));
            if (at(v, i + 1) == kLifted) {
                send(p1);  // hold up to the lift
            } else {
                const float p2 = value(at(v, i + 1));
                const float p0 = at(v, i - 1) == kLifted ? p1 : value(at(v, i - 1));
                const float p3 = at(v, i + 2) == kLifted ? p2 : value(at(v, i + 2));
                send(std::clamp(monotoneCubic(p0, p1, p2, p3, f), 0.0f, 1.0f));
            }
        }
    } else {
        // The samples this step passes over, in the playing direction.
        const int count = static_cast<int>(std::ceil(step));
        const int dir = playback == LoopPlayback::Reverse ? -1 : (playback == LoopPlayback::Forward ? 1 : loop.direction);
        float sum = 0;
        int on = 0;
        for (int j = 0; j < count; ++j) {
            const int k = i + dir * j;
            if (at(loop.x, k) == kLifted)
                continue;
            sum += value(at(loop.x, k));
            ++on;
        }
        if (on > 0)
            send(sum / static_cast<float>(on));
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

// Sends one axis of a movement loop, in [0, 1], on its CC and channel, scaled
// like the axis it was recorded from.
void Engine::sendLoopValue(std::size_t index, float p) {
    Loop& loop = loops_[index];
    const auto channel = channelOfLoop(index);
    const int track = trackOfLoop(index), source = sourceOfTrack(track), axis = axisOfTrack(track);
    const Mapping& m = preset_->forChannel(channel);
    const LoopBinding& binding = m.loops[static_cast<std::size_t>(source)];
    const AxisBinding* a = nullptr;
    if (source < 2) {
        a = &m.axes[static_cast<std::size_t>(source * 2 + axis)];
    } else if (source == static_cast<int>(LoopSource::Triggers)) {
        a = &m.axes[static_cast<std::size_t>(axis == 0 ? Axis::LeftTrigger : Axis::RightTrigger)];
    } else {
        const FingerBinding& f = m.fingers[static_cast<std::size_t>(source - 2)];
        a = axis == 0 ? &f.x : &f.y;
        if (axis == 1)
            p = 1.0f - p;  // the touchpad's Y grows downwards, its CC upwards
    }
    // An axis set to Velocity or Transpose plays its loop as that, not as a CC.
    const bool shapes = a->enabled && (a->mode == AxisMode::Velocity || a->mode == AxisMode::Transpose);
    loopShaping_[index] = shapes ? p : -1.0f;
    if (shapes)
        return;
    const float position = a->invert ? 1.0f - p : p;
    const int lo = a->enabled ? a->min : 0, hi = a->enabled ? a->max : 127;
    const int v = lo + static_cast<int>(std::lround(position * static_cast<float>(hi - lo)));
    if (v != loop.sentX) {
        sendCc(channel, axis == 0 ? binding.ccX : binding.ccY, v);
        loop.sentX = v;
    }
}

// The samples of a step of a loop in steps (0: not in steps): its beats on
// the loop's own beats when synced, else on the clock's tempo.
int Engine::stepLength(std::size_t index) const {
    const Loop& loop = loops_[index];
    const int track = trackOfLoop(index);
    if (track == kNoteTrack || loop.length <= 0)
        return 0;
    const LoopBinding& lb = preset_->forChannel(channelOfLoop(index)).loops[static_cast<std::size_t>(sourceOfTrack(track))];
    const StepGrid& grid = lb.steps[static_cast<std::size_t>(axisOfTrack(track))];
    if (!grid.on)
        return 0;
    const double beats = kStepBeats[static_cast<std::size_t>(std::clamp(grid.step, 0, kStepChoices - 1))];
    const double stepBeats = beats > 0 ? beats : static_cast<double>(beatsPerBar_);
    const double perBeat = loop.syncBeats > 0 ? loop.length / loop.syncBeats : kLoopRateHz * 60.0 / std::max(clock_.tempo, 1.0);
    return std::max(1, static_cast<int>(std::lround(stepBeats * perBeat)));
}

int Engine::loopStepSamples(int channel, int source, int axis) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    return stepLength(loopIndex(channel, trackOf(source, axis)));
}

// A value of a loop in steps, on the nearest of its grid's levels.
float Engine::stepped(std::size_t index, float v) const {
    const LoopBinding& lb = preset_->forChannel(channelOfLoop(index)).loops[static_cast<std::size_t>(sourceOfTrack(trackOfLoop(index)))];
    const StepGrid& grid = lb.steps[static_cast<std::size_t>(axisOfTrack(trackOfLoop(index)))];
    if (!grid.on)
        return v;
    const float n = static_cast<float>(std::clamp(grid.levels, 2, 128) - 1);
    return std::round(v * n) / n;
}

// Stops what a note loop has on.
void Engine::silenceLoop(std::size_t index) {
    if (trackOfLoop(index) == kNoteTrack) {
        const auto channel = channelOfLoop(index);
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
    constexpr auto kNotes = static_cast<std::size_t>(kNoteTrack);
    const bool starts = velocity > 0 && waiting_[kNotes] && channel == holdChannel_;
    if (starts && loops_[slotIndex(channel, recordSlot_[static_cast<std::size_t>(channel)], kNoteTrack)].locked)
        waiting_[kNotes] = false;  // locked during the hold
    else if (starts)
        enterRecordSlot(channel);  // a new clip: there, the current one stops
    const std::size_t index = loopIndex(channel, kNoteTrack);
    Loop& loop = loops_[index];
    if (velocity > 0 && waiting_[kNotes] && channel == holdChannel_) {
        waiting_[kNotes] = false;
        flushPendingStop();
        if (loopsPaused_)
            playLoops();
        silenceLoop(index);
        loop.state = LoopStatus::State::Recording;
        loop.length = 0;
        loop.syncBeats = 0;
        loop.startBeat = beatAt(now_);
        loop.nextAt = now_;
        noteRecordStart_ = now_;
        eventsOf(channel).clear();
        recordedThisHold_ = true;
        if (overdubChannel_ == channel)
            overdubChannel_ = -1;
    }
    if (loop.state == LoopStatus::State::Playing && channel == overdubChannel_ && !loop.locked) {
        overdubNote(channel, note, velocity, at, press, leadAt);
        return;
    }
    auto& events = eventsOf(channel);
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
    const Loop& loop = loops_[loopIndex(channel, kNoteTrack)];
    const double step = quantizeStep(loop);
    if (step <= 0)
        return;
    auto& events = eventsOf(channel);
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
    const Loop& loop = loops_[loopIndex(channel, kNoteTrack)];
    auto& events = eventsOf(channel);
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
    const Loop& loop = loops_[loopIndex(channel, kNoteTrack)];
    auto& events = eventsOf(channel);
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
    const auto channel = channelOfLoop(index);
    const auto& events = noteEvents_[index / kTrackCount];
    auto& on = noteLoopOn_[static_cast<std::size_t>(channel)];
    const auto lo = std::lower_bound(events.begin(), events.end(), loop.position,
                                     [](const NoteEvent& e, int at) { return e.at < at; });
    auto hi = lo;
    while (hi != events.end() && hi->at == loop.position)
        ++hi;
    auto& shift = loopShift_[static_cast<std::size_t>(channel)];
    const Shaping sh = shaping(channel);  // a Transpose or Velocity axis set on the loop
    const int steps = sh.transposeLoop;
    auto play = [&](const NoteEvent& e, bool start) {
        if (start) {
            if (on.test(e.note))  // the same note again: the one on stops first
                sendNow(MidiMessage::noteOff(channel, e.note + shift[e.note]));
            const int note = transposed(e.note, steps);
            shift[e.note] = static_cast<std::int8_t>(note - e.note);
            // Louder or softer from its own velocity: the accents stay.
            const float v = sh.velocityLoop, base = static_cast<float>(e.velocity);
            const int velocity = std::clamp(static_cast<int>(std::lround(base + (v > 0 ? v * (127.0f - base) : v * (base - 1.0f)))), 1, 127);
            sendNow(MidiMessage::noteOn(channel, note, velocity));
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
    const auto& events = eventsOf(channel);
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
    const Loop& loop = loops_[loopIndex(channel, kNoteTrack)];
    if (loop.state != LoopStatus::State::Playing || loop.length < 2)
        return false;
    auto& events = eventsOf(channel);
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
    const Loop& loop = loops_[loopIndex(channel, kNoteTrack)];
    if (loop.state != LoopStatus::State::Playing || loop.length < 2)
        return false;
    auto& events = eventsOf(channel);
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

bool Engine::setNoteLoopLength(int channel, int length) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    const std::size_t index = loopIndex(channel, kNoteTrack);
    Loop& loop = loops_[index];
    if (loop.state != LoopStatus::State::Playing || loop.length < 2)
        return false;
    if (loop.syncBeats > 0) {  // whole beats, at the beat's length as recorded
        const double perBeat = static_cast<double>(loop.length) / loop.syncBeats;
        const double most = std::max(1.0, std::floor(kMaxLoopSamples / perBeat));
        const double beats = std::clamp(std::round(length / perBeat), 1.0, most);
        length = static_cast<int>(std::lround(beats * perBeat));
        loop.syncBeats = beats;
    }
    length = std::clamp(length, 2, kMaxLoopSamples);
    if (length == loop.length)
        return true;
    fromLoop_ = true;
    silenceLoop(index);
    fromLoop_ = false;
    // Notes starting at or past the end go, with their releases; those still on there end with it.
    auto& events = eventsOf(channel);
    std::bitset<128> dropped;
    for (NoteEvent& e : events) {
        if (e.on) {
            dropped.set(e.note, e.at >= length);
            if (e.at >= length)
                e.at = -1;
        } else if (dropped.test(e.note)) {
            dropped.reset(e.note);
            e.at = -1;
        } else if (e.at >= length) {
            e.at = length - 1;
        }
    }
    events.erase(std::remove_if(events.begin(), events.end(), [](const NoteEvent& e) { return e.at < 0; }), events.end());
    loop.length = length;
    if (loop.position >= length) {
        loop.position = loop.direction < 0 ? length - 1 : 0;
        loop.phase = loop.position;
    }
    tidyNoteLoop(channel);
    return true;
}

bool Engine::setAxisLoopLength(int channel, int source, int axis, int length) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    if (source < 0 || source >= kLoopSourceCount || source == static_cast<int>(LoopSource::Notes))
        return false;
    const std::size_t index = loopIndex(channel, trackOf(source, axis));
    Loop& loop = loops_[index];
    if (loop.state != LoopStatus::State::Playing || loop.length < 2)
        return false;
    if (loop.syncBeats > 0) {  // whole beats, at the beat's length as recorded
        const double perBeat = static_cast<double>(loop.length) / loop.syncBeats;
        const double most = std::max(1.0, std::floor(kMaxLoopSamples / perBeat));
        const double beats = std::clamp(std::round(length / perBeat), 1.0, most);
        length = static_cast<int>(std::lround(beats * perBeat));
        loop.syncBeats = beats;
    }
    length = std::clamp(length, 2, kMaxLoopSamples);
    if (length == loop.length)
        return true;
    if (length > loop.length) {
        // The time added: a straight line from its last value back to its first
        // (a finger lifted at either end: lifted there).
        const std::uint16_t from = loop.x[static_cast<std::size_t>(loop.length - 1)], to = loop.x[0];
        const int added = length - loop.length;
        for (int k = 0; k < added; ++k) {
            const float t = static_cast<float>(k + 1) / static_cast<float>(added + 1);
            loop.x[static_cast<std::size_t>(loop.length + k)] =
                from == kLifted || to == kLifted ? kLifted
                                                 : static_cast<std::uint16_t>(std::lround(from + (static_cast<float>(to) - from) * t));
        }
    }
    loop.length = length;
    if (loop.position >= length) {
        loop.position = loop.direction < 0 ? length - 1 : 0;
        loop.phase = loop.position;
    }
    if (loop.phase >= length)
        loop.phase = std::fmod(loop.phase, static_cast<double>(length));
    findMiddle(loop);
    return true;
}

bool Engine::removeLoopNotes(int channel, const std::vector<LoopNoteRef>& notes) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    auto& events = eventsOf(channel);
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
    return clipNoteIntervals(channel, currentSlot_[static_cast<std::size_t>(channel)], out, max);
}

int Engine::clipNoteIntervals(int channel, int slot, NoteInterval* out, int max) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    slot = std::clamp(slot, 0, kClipSlots - 1);
    const std::size_t index = slotIndex(channel, slot, kNoteTrack);
    const Loop& loop = loops_[index];
    if (loop.state == LoopStatus::State::Empty)
        return 0;
    std::array<int, 128> open;
    open.fill(-1);
    int count = 0;
    for (const NoteEvent& e : noteEvents_[index / kTrackCount]) {
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

LoopStatus Engine::loopStatus(int channel, int source, int axis) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    const int track = trackOf(source, axis);
    const Loop& loop = loops_[loopIndex(channel, track)];
    const bool waiting = waiting_[static_cast<std::size_t>(track)] && holdChannel_ == channel;
    const bool overdub = track == kNoteTrack && overdubChannel_ == channel;
    return {loop.state, waiting, overdub, loop.length, loop.position, loop.direction, channel, loop.stretch, loop.amplitude,
            loop.syncBeats > 0 ? loop.syncBeats * stretchFactor(loop.stretch) : 0.0, loop.locked};
}

bool Engine::loopLocked(int channel, int source) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    return loops_[loopIndex(channel, trackOf(source, 0))].locked || loops_[loopIndex(channel, trackOf(source, 1))].locked;
}

bool Engine::toggleLoopLock(int channel, int source) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    if (source == static_cast<int>(LoopSource::Notes))
        return toggleLoopLock(channel, source, 0);
    Loop& x = loops_[loopIndex(channel, trackOf(source, 0))];
    Loop& y = loops_[loopIndex(channel, trackOf(source, 1))];
    if (x.locked || y.locked) {
        x.locked = y.locked = false;
        return true;
    }
    x.locked = x.state == LoopStatus::State::Playing;  // an axis without a loop stays free to record
    y.locked = y.state == LoopStatus::State::Playing;
    return x.locked || y.locked;
}

bool Engine::toggleLoopLock(int channel, int source, int axis) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    Loop& loop = loops_[loopIndex(channel, trackOf(source, axis))];
    if (!loop.locked && loop.state != LoopStatus::State::Playing)
        return false;
    loop.locked = !loop.locked;
    if (loop.locked && source == static_cast<int>(LoopSource::Notes) && overdubChannel_ == channel)
        overdubChannel_ = -1;
    return true;
}

float Engine::loopSample(int channel, int source, int axis, int index) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    return clipSample(channel, currentSlot_[static_cast<std::size_t>(channel)], source, axis, index);
}

float Engine::clipSample(int channel, int slot, int source, int axis, int index) const {
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    if (source == static_cast<int>(LoopSource::Notes))
        return -1.0f;
    const Loop& loop = loops_[slotIndex(std::clamp(channel, 0, kChannelCount - 1), std::clamp(slot, 0, kClipSlots - 1), trackOf(source, axis))];
    const auto i = static_cast<std::size_t>(std::clamp(index, 0, kMaxLoopSamples - 1));
    const std::uint16_t v = loop.x[i];
    return v == kLifted ? -1.0f : static_cast<float>(v) / 65534.0f;
}

void Engine::clearLoop(int channel, int source, int axis) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    if (source == static_cast<int>(LoopSource::Notes)) {
        clearLoop(channel, source);
        return;
    }
    clearTrack(loopIndex(channel, trackOf(source, axis)));
    if (!currentHasLoops(channel))
        clipPlaying_[static_cast<std::size_t>(channel)] = false;
}

void Engine::clearLoop(int channel, int source) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    if (source == static_cast<int>(LoopSource::Notes)) {
        clearTrack(loopIndex(channel, kNoteTrack));
        return;
    }
    clearTrack(loopIndex(channel, trackOf(source, 0)));
    clearTrack(loopIndex(channel, trackOf(source, 1)));
}

void Engine::clearTrack(std::size_t index) {
    silenceLoop(index);
    Loop& loop = loops_[index];
    loop.state = LoopStatus::State::Empty;
    loop.length = 0;
    loop.stretch = 0;
    loop.amplitude = 100;
    loop.amplitudeNow = 1;
    loopShaping_[index] = -1.0f;
    loop.locked = false;
    const int track = trackOfLoop(index);
    if (track == kNoteTrack && overdubChannel_ == channelOfLoop(index))
        overdubChannel_ = -1;
}

void Engine::clearClip(int channel, int slot) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    slot = std::clamp(slot, 0, kClipSlots - 1);
    const auto c = static_cast<std::size_t>(channel);
    for (int track = 0; track < kTrackCount; ++track)
        clearTrack(slotIndex(channel, slot, track));
    noteEvents_[c * kClipSlots + static_cast<std::size_t>(slot)].clear();
    if (queued_[c] == slot)
        queued_[c] = -1;
    if (slot == currentSlot_[c])
        clipPlaying_[c] = false;
}

bool Engine::copyClip(int channel, int from, int to) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    from = std::clamp(from, 0, kClipSlots - 1);
    to = std::clamp(to, 0, kClipSlots - 1);
    if (from == to || !clipHasLoops(channel, from))
        return false;
    const auto c = static_cast<std::size_t>(channel);
    for (int track = 0; track < kTrackCount; ++track) {
        const std::size_t si = slotIndex(channel, from, track), di = slotIndex(channel, to, track);
        silenceLoop(di);
        Loop& src = loops_[si];
        Loop& dst = loops_[di];
        // Everything but the samples, then the samples into the buffer it has (no allocation).
        std::vector<std::uint16_t> srcX = std::move(src.x), dstX = std::move(dst.x);
        dst = src;
        src.x = std::move(srcX);
        dst.x = std::move(dstX);
        if (!src.x.empty() && !dst.x.empty())
            std::copy_n(src.x.begin(), std::min(src.x.size(), dst.x.size()), dst.x.begin());
        dst.position = 0;
        dst.phase = 0;
        dst.turns = 0;
        dst.locked = false;
        dst.nextAt = now_;
        dst.sentX = -1;
    }
    noteEvents_[c * kClipSlots + static_cast<std::size_t>(to)] = noteEvents_[c * kClipSlots + static_cast<std::size_t>(from)];
    if (to == currentSlot_[c] && clipPlaying_[c])
        startClipLoops(channel);
    return true;
}

void Engine::resetLoopShape(int channel, int source, int axis) {
    if (source < 0 || source >= kLoopSourceCount || source == static_cast<int>(LoopSource::Notes))
        return;
    Loop& loop = loops_[loopIndex(std::clamp(channel, 0, kChannelCount - 1), trackOf(source, axis))];
    loop.stretch = 0;
    loop.amplitude = 100;  // eased in
}

void Engine::stopLoops(int channel) {
    for (int track = 0; track < kTrackCount; ++track) {
        const Loop& loop = loops_[loopIndex(channel, track)];
        if (loop.state == LoopStatus::State::Playing && !loop.locked)
            clearTrack(loopIndex(channel, track));
    }
    if (!currentHasLoops(channel))
        clipPlaying_[static_cast<std::size_t>(channel)] = false;
}

void Engine::pauseLoops() {
    if (loopsPaused_)
        return;
    loopsPaused_ = true;
    loopShaping_.fill(-1.0f);
    fromLoop_ = true;
    for (std::size_t i = 0; i < loops_.size(); ++i)
        silenceLoop(i);  // the notes the note loops have on
    fromLoop_ = false;
}

void Engine::playLoops() {
    loopsPaused_ = false;
    for (int channel = 0; channel < kChannelCount; ++channel) {
        if (!clipPlaying_[static_cast<std::size_t>(channel)])
            continue;
        startClipLoops(channel);
        double turns = 0;  // a clip waiting to start counts its turns from here
        if (queued_[static_cast<std::size_t>(channel)] >= 0 && clipTurns(channel, turns))
            turnMark_[static_cast<std::size_t>(channel)] = static_cast<long long>(std::floor(turns));
    }
}

void Engine::startClipLoops(int channel) {
    for (int track = 0; track < kTrackCount; ++track) {
        const int source = sourceOfTrack(track);
        const std::size_t index = loopIndex(channel, track);
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
        loop.sentX = -1;
        loop.turns = 0;
    }
}

bool Engine::clipHasLoops(int channel, int slot) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    slot = std::clamp(slot, 0, kClipSlots - 1);
    for (int track = 0; track < kTrackCount; ++track)
        if (loops_[slotIndex(channel, slot, track)].state != LoopStatus::State::Empty)
            return true;
    return false;
}

LoopStatus Engine::clipLoopStatus(int channel, int slot, int source, int axis) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    slot = std::clamp(slot, 0, kClipSlots - 1);
    source = std::clamp(source, 0, kLoopSourceCount - 1);
    const Loop& loop = loops_[slotIndex(channel, slot, trackOf(source, axis))];
    return {loop.state, false, false, loop.length, loop.position, loop.direction, channel, loop.stretch, loop.amplitude,
            loop.syncBeats > 0 ? loop.syncBeats * stretchFactor(loop.stretch) : 0.0, loop.locked};
}

bool Engine::clipTurns(int channel, double& turns) const {
    const Loop* lead = nullptr;
    double leadLength = 0, leadBeats = 0;
    const Loop& notes = loops_[loopIndex(channel, kNoteTrack)];
    if (notes.state == LoopStatus::State::Playing && notes.length > 1) {
        lead = &notes;
        leadLength = notes.length;
        leadBeats = notes.syncBeats;
    } else {
        for (int track = 0; track < kNoteTrack; ++track) {
            const Loop& loop = loops_[loopIndex(channel, track)];
            const double length = loop.length * stretchFactor(loop.stretch);
            if (loop.state == LoopStatus::State::Playing && loop.length > 1 && length > leadLength) {
                lead = &loop;
                leadLength = length;
                leadBeats = loop.syncBeats * stretchFactor(loop.stretch);
            }
        }
    }
    if (!lead)
        return false;
    if (leadBeats > 0 && clock_.on) {
        turns = (beatAt(now_) - lead->anchorBeat) / leadBeats;
        return true;
    }
    // Free: its turns so far, and the steps this tick is about to play.
    constexpr std::uint64_t kStep = 1'000'000'000ull / kLoopRateHz;
    const double pending = now_ >= lead->nextAt ? 1.0 + static_cast<double>((now_ - lead->nextAt) / kStep) : 0.0;
    turns = lead->turns + pending / leadLength;
    return true;
}

void Engine::launchClip(int channel, int slot) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    stopChain(channel);  // the hand wins
    queueClip(channel, slot);
}

void Engine::queueClip(int channel, int slot) {
    slot = std::clamp(slot, 0, kClipSlots);
    const auto c = static_cast<std::size_t>(channel);
    if ((slot < kClipSlots && slot == currentSlot_[c] && clipPlaying_[c]) || (slot == kClipSlots && !clipPlaying_[c])) {
        queued_[c] = -1;  // already so
        return;
    }
    double turns = 0;
    if (!clipPlaying_[c] || loopsPaused_ || !clipTurns(channel, turns)) {
        switchClip(channel, slot);
        return;
    }
    queued_[c] = slot;
    turnMark_[c] = static_cast<long long>(std::floor(turns));
}

float Engine::clipProgress(int channel) const {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    double turns = 0;
    if (!clipPlaying_[static_cast<std::size_t>(channel)] || !clipTurns(channel, turns))
        return -1.0f;
    return static_cast<float>(turns - std::floor(turns));
}

void Engine::pushSessionEvent(SessionEvent e) {
    if (sessionEventCount_ < static_cast<int>(sessionEvents_.size()))
        sessionEvents_[static_cast<std::size_t>(sessionEventCount_++)] = e;
}

std::optional<Engine::SessionEvent> Engine::takeSessionEvent() {
    if (sessionEventCount_ == 0)
        return std::nullopt;
    const SessionEvent e = sessionEvents_[0];
    std::move(sessionEvents_.begin() + 1, sessionEvents_.begin() + sessionEventCount_, sessionEvents_.begin());
    --sessionEventCount_;
    return e;
}

// Where a stick pushes the session screen's cursor: past kNavPush the way it
// mostly goes; back under kNavRest, nowhere; in between, as it was.
std::optional<Engine::SessionEvent> Engine::stickPush(int stick) const {
    const auto i = static_cast<std::size_t>(stick * 2);
    const float dx = (stickPosition_[i] - 0.5f) * 2.0f, dy = (stickPosition_[i + 1] - 0.5f) * 2.0f;  // y: down
    const float out = std::max(std::abs(dx), std::abs(dy));
    if (out >= kNavPush)
        return std::abs(dx) >= std::abs(dy) ? (dx > 0 ? SessionEvent::Right : SessionEvent::Left)
                                            : (dy > 0 ? SessionEvent::Down : SessionEvent::Up);
    if (out < kNavRest)
        return std::nullopt;
    return stickNav_[static_cast<std::size_t>(stick)];
}

// The session screen's cursor, from either stick (a step a push) and from a
// D-pad direction held: both repeat after kNavRepeatDelayMs. When navigation
// starts (the screen opens, Record is let go), a stick already pushed counts
// as held: it moves nothing until back at rest.
void Engine::navigate() {
    const bool now = navigating();
    if (!now) {
        wasNavigating_ = false;
        return;
    }
    const std::uint64_t delay = static_cast<std::uint64_t>(kNavRepeatDelayMs) * 1'000'000u;
    const std::uint64_t repeat = static_cast<std::uint64_t>(kNavRepeatMs) * 1'000'000u;
    for (int stick = 0; stick < 2; ++stick) {
        const auto k = static_cast<std::size_t>(stick);
        std::optional<SessionEvent> dir = stickPush(stick);
        if (dir && stick == 1 && sessionValues_) {  // the right stick sets the chain's values
            // (Between push and rest, stickPush gives back the last one, already a value.)
            switch (*dir) {
            case SessionEvent::Up: dir = SessionEvent::ValueUp; break;
            case SessionEvent::Down: dir = SessionEvent::ValueDown; break;
            case SessionEvent::Left: dir = SessionEvent::ValueLeft; break;
            case SessionEvent::Right: dir = SessionEvent::ValueRight; break;
            default: break;
            }
        }
        if (!wasNavigating_) {
            stickNav_[k] = dir;
            stickNavNext_[k] = ~0ull;
            continue;
        }
        if (dir != stickNav_[k]) {
            stickNav_[k] = dir;
            if (dir) {
                pushSessionEvent(*dir);
                stickNavNext_[k] = now_ + delay;
            }
        } else if (dir && now_ >= stickNavNext_[k]) {
            pushSessionEvent(*dir);
            stickNavNext_[k] = now_ + repeat;
        }
    }
    if (!wasNavigating_)
        hatNav_.reset();
    wasNavigating_ = true;
    if (hatNav_ && now_ >= hatNavNext_) {
        pushSessionEvent(*hatNav_);
        hatNavNext_ = now_ + repeat;
    }
}

void Engine::setRecordSlot(int channel, int slot) {
    recordSlot_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))] = std::clamp(slot, 0, kClipSlots - 1);
}

void Engine::switchClip(int channel, int slot) {
    const auto c = static_cast<std::size_t>(channel);
    fromLoop_ = true;
    for (int track = 0; track < kTrackCount; ++track) {
        silenceLoop(loopIndex(channel, track));
        loopShaping_[loopIndex(channel, track)] = -1.0f;
    }
    fromLoop_ = false;
    if (overdubChannel_ == channel)
        overdubChannel_ = -1;
    queued_[c] = -1;
    if (slot >= kClipSlots) {  // a stop: its clips stay
        clipPlaying_[c] = false;
        return;
    }
    currentSlot_[c] = recordSlot_[c] = slot;
    if (clock_.on)  // synced loops start on the beat, from their start
        for (int track = 0; track < kTrackCount; ++track) {
            Loop& loop = loops_[loopIndex(channel, track)];
            if (loop.state == LoopStatus::State::Playing && loop.syncBeats > 0)
                loop.anchorBeat = std::round(beatAt(now_));
        }
    startClipLoops(channel);
    clipPlaying_[c] = currentHasLoops(channel);
}

void Engine::enterRecordSlot(int channel) {
    const auto c = static_cast<std::size_t>(channel);
    if (recordSlot_[c] != currentSlot_[c]) {
        stopChain(channel);
        switchClip(channel, recordSlot_[c]);
    }
}

void Engine::setChainStep(int channel, int step, ChainStep s) {
    if (step < 0 || step >= kChainSteps)
        return;
    s.clip = s.clip < 0 ? -1 : std::min(s.clip, kChainRandom);
    s.repeats = std::clamp(s.repeats, 0, kMaxRepeats);
    chains_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))][static_cast<std::size_t>(step)] = s;
}

Engine::ChainStep Engine::chainStep(int channel, int step) const {
    if (step < 0 || step >= kChainSteps)
        return {};
    return chains_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))][static_cast<std::size_t>(step)];
}

void Engine::setChainLoop(int channel, bool on) { chainLoop_[static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1))] = on; }

bool Engine::chainStepPlays(int channel, int step) const {
    const ChainStep& s = chains_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)];
    if (s.clip == kChainRandom) {
        for (int slot = 0; slot < kClipSlots; ++slot)
            if (clipHasLoops(channel, slot))
                return true;
        return false;
    }
    return s.clip >= 0 && clipHasLoops(channel, s.clip);
}

int Engine::chainPick(int channel, int step) {
    const ChainStep& s = chains_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(step)];
    int clip = s.clip;
    if (clip == kChainRandom) {
        std::array<int, kClipSlots> recorded{};
        int count = 0;
        for (int slot = 0; slot < kClipSlots; ++slot)
            if (clipHasLoops(channel, slot))
                recorded[static_cast<std::size_t>(count++)] = slot;
        clip = count > 0 ? recorded[static_cast<std::size_t>(randomBetween(0, count - 1))] : -1;
    }
    chainClip_[static_cast<std::size_t>(channel)] = clip;
    return clip;
}

int Engine::chainFind(int channel, int from) const {
    for (int k = 0; k < kChainSteps; ++k) {
        const int step = (from + k) % kChainSteps;
        if (chainStepPlays(channel, step))
            return step;
    }
    return -1;
}

void Engine::chainGoTo(int channel, int step) {
    channel = std::clamp(channel, 0, kChannelCount - 1);
    const auto c = static_cast<std::size_t>(channel);
    step = std::clamp(step, 0, kChainSteps - 1);
    if (chainAt_[c] >= 0 && !chainStarting_[c]) {  // running: there at the end of the turn
        chainJump_[c] = step;
        return;
    }
    const int at = chainFind(channel, step);
    if (at < 0) {
        stopChain(channel);
        return;
    }
    chainAt_[c] = at;
    chainJump_[c] = -1;
    chainTurns_[c] = 0;
    chainStarting_[c] = true;  // counts once its clip plays
    queueClip(channel, chainPick(channel, at));
}

void Engine::stopChain(int channel) {
    const auto c = static_cast<std::size_t>(std::clamp(channel, 0, kChannelCount - 1));
    chainAt_[c] = chainJump_[c] = chainClip_[c] = -1;
    chainStarting_[c] = false;
}

// Each running chain, at the end of each turn of its clip: one more turn of
// the step; done (or asked to go on), the next step's clip starts there.
void Engine::runChains() {
    for (int channel = 0; channel < kChannelCount; ++channel) {
        const auto c = static_cast<std::size_t>(channel);
        if (chainAt_[c] < 0 || queued_[c] >= 0)
            continue;
        double turns = 0;
        if (!clipPlaying_[c] || !clipTurns(channel, turns)) {
            if (!chainStarting_[c])
                stopChain(channel);  // stopped, or nothing left to play
            continue;
        }
        const auto whole = static_cast<long long>(std::floor(turns));
        if (chainStarting_[c]) {  // its first clip plays: count from here
            chainStarting_[c] = false;
            chainMark_[c] = whole;
            continue;
        }
        if (whole <= chainMark_[c])
            continue;
        chainMark_[c] = whole;
        ++chainTurns_[c];
        const ChainStep& now = chains_[c][static_cast<std::size_t>(chainAt_[c])];
        int next = -1;
        if (chainJump_[c] >= 0) {
            next = chainFind(channel, chainJump_[c]);
        } else if (now.repeats > 0 && chainTurns_[c] >= now.repeats) {
            const int after = chainAt_[c] + 1;
            if (after >= kChainSteps && !chainLoop_[c]) {
                stopChain(channel);
                switchClip(channel, kClipSlots);  // the end: the channel stops
                continue;
            }
            next = chainFind(channel, after % kChainSteps);
            if (!chainLoop_[c] && next >= 0 && next <= chainAt_[c]) {  // only empty steps after it
                stopChain(channel);
                switchClip(channel, kClipSlots);
                continue;
            }
        } else {
            continue;
        }
        chainJump_[c] = -1;
        if (next < 0) {
            stopChain(channel);
            continue;
        }
        chainAt_[c] = next;
        chainTurns_[c] = 0;
        switchClip(channel, chainPick(channel, next));
        if (clipTurns(channel, turns))
            chainMark_[c] = static_cast<long long>(std::floor(turns));
    }
}

// Its middle, where the amplitude closes in or widens.
void Engine::findMiddle(Loop& loop) {
    std::uint16_t lx = 0xFFFE, hx = 0;
    for (int k = 0; k < loop.length; ++k) {
        const auto at = static_cast<std::size_t>(k);
        if (loop.x[at] == kLifted)
            continue;
        lx = std::min(lx, loop.x[at]), hx = std::max(hx, loop.x[at]);
    }
    if (lx <= hx)
        loop.middleX = (static_cast<float>(lx) + static_cast<float>(hx)) * 0.5f / 65534.0f;
}

std::vector<SessionLoop> Engine::sessionLoops() const {
    std::vector<SessionLoop> out;
    for (int channel = 0; channel < kChannelCount; ++channel) {
        const auto c = static_cast<std::size_t>(channel);
        for (int slot = 0; slot < kClipSlots; ++slot) {
            for (int track = 0; track < kTrackCount; ++track) {
                const std::size_t index = slotIndex(channel, slot, track);
                const Loop& loop = loops_[index];
                if (loop.state != LoopStatus::State::Playing || loop.length < 2)
                    continue;
                SessionLoop l;
                l.channel = channel;
                l.clip = slot;
                l.current = slot == currentSlot_[c];
                l.playing = l.current && clipPlaying_[c];
                l.source = static_cast<LoopSource>(sourceOfTrack(track));
                l.length = loop.length;
                l.syncBeats = loop.syncBeats;
                l.anchorBeat = loop.anchorBeat;
                l.locked = loop.locked;
                if (track == kNoteTrack) {
                    for (const NoteEvent& e : noteEvents_[index / kTrackCount])
                        l.notes.push_back({e.at, e.note, e.velocity, e.on, e.press});
                } else {
                    l.axis = axisOfTrack(track);
                    l.stretch = loop.stretch;
                    l.amplitude = loop.amplitude;
                    l.x.assign(loop.x.begin(), loop.x.begin() + loop.length);
                }
                out.push_back(std::move(l));
            }
        }
    }
    return out;
}

void Engine::loadSessionLoops(const std::vector<SessionLoop>& loops) {
    waiting_.fill(false);
    pendingStopChannel_ = -1;
    shiftTapAt_ = 0;
    recordHolders_.reset();
    overdubChannel_ = -1;
    queued_.fill(-1);
    for (int channel = 0; channel < kChannelCount; ++channel)
        stopChain(channel);
    fromLoop_ = true;
    for (int channel = 0; channel < kChannelCount; ++channel)  // every clip of every channel goes
        for (int slot = 0; slot < kClipSlots; ++slot) {
            for (int track = 0; track < kTrackCount; ++track)
                clearTrack(slotIndex(channel, slot, track));
            noteEvents_[static_cast<std::size_t>(channel * kClipSlots + slot)].clear();
        }
    fromLoop_ = false;
    currentSlot_.fill(0);
    clipPlaying_.fill(false);
    std::array<bool, kChannelCount> playing{};
    for (const SessionLoop& l : loops) {
        if (l.channel < 0 || l.channel >= kChannelCount || l.clip < 0 || l.clip >= kClipSlots || l.length < 2 ||
            l.length > kMaxLoopSamples)
            continue;
        const auto c = static_cast<std::size_t>(l.channel);
        if (l.current)
            currentSlot_[c] = l.clip;
        playing[c] = playing[c] || l.playing;
        const int source = static_cast<int>(l.source);
        auto fill = [&](int track, const std::vector<std::uint16_t>* samples) {
            const std::size_t index = slotIndex(l.channel, l.clip, track);
            Loop& loop = loops_[index];
            if (track == kNoteTrack) {
                auto& events = noteEvents_[index / kTrackCount];
                events.clear();
                for (const SessionLoop::Note& n : l.notes)
                    if (events.size() < kMaxNoteEvents)
                        events.push_back({std::clamp(n.at, 0, l.length - 1), static_cast<std::uint8_t>(n.note & 0x7F), n.velocity, n.on, n.press});
                for (const SessionLoop::Note& n : l.notes)  // new presses never take a number of these
                    pressCount_ = std::max(pressCount_, n.press);
            } else {
                if (static_cast<int>(samples->size()) < l.length)
                    return;
                std::copy(samples->begin(), samples->begin() + l.length, loop.x.begin());
                loop.stretch = std::clamp(l.stretch, kMinStretch, kMaxStretch);
                loop.amplitude = std::clamp(l.amplitude, 0, kMaxAmplitude);
                loop.amplitudeNow = static_cast<float>(loop.amplitude) / 100.0f;
            }
            loop.length = l.length;
            loop.syncBeats = l.syncBeats;
            loop.anchorBeat = l.anchorBeat;
            loop.locked = l.locked;
            loop.state = LoopStatus::State::Playing;
            if (track != kNoteTrack)
                findMiddle(loop);
        };
        if (l.source == LoopSource::Notes) {
            fill(kNoteTrack, nullptr);
        } else if (l.axis >= 0) {
            fill(trackOf(source, l.axis), &l.x);
        } else {  // from before X and Y were apart: both, alike
            fill(trackOf(source, 0), &l.x);
            fill(trackOf(source, 1), &l.y);
        }
    }
    for (int channel = 0; channel < kChannelCount; ++channel) {
        const auto c = static_cast<std::size_t>(channel);
        recordSlot_[c] = currentSlot_[c];
        clipPlaying_[c] = playing[c] && currentHasLoops(channel);
    }
    playLoops();  // from their start; synced ones take their place from the beat clock
}

std::uint16_t Engine::channelsWithLoops() const {
    std::uint16_t bits = 0;
    for (int channel = 0; channel < kChannelCount; ++channel) {
        bool recording = false;
        for (int track = 0; track < kTrackCount; ++track)
            recording = recording || loops_[loopIndex(channel, track)].state == LoopStatus::State::Recording;
        if ((clipPlaying_[static_cast<std::size_t>(channel)] && currentHasLoops(channel)) || recording)
            bits = static_cast<std::uint16_t>(bits | (1u << channel));
    }
    return bits;
}

void Engine::stopLoops() {
    pendingStopChannel_ = -1;
    queued_.fill(-1);
    for (int channel = 0; channel < kChannelCount; ++channel)
        stopChain(channel);
    for (int channel = 0; channel < kChannelCount; ++channel) {  // the current clips; the other clips stay
        for (int track = 0; track < kTrackCount; ++track)
            if (loops_[loopIndex(channel, track)].state == LoopStatus::State::Playing)
                clearTrack(loopIndex(channel, track));
        clipPlaying_[static_cast<std::size_t>(channel)] = false;
    }
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
