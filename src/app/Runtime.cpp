#include "Runtime.hpp"

#include "GamepadInput.hpp"

#include "midipad/Labels.hpp"
#include "midipad/Notes.hpp"

#include <SDL3/SDL.h>

#include <cstdio>

#ifdef __linux__
#include <pthread.h>
#endif

using namespace midipad;

namespace {

void storeMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    if (value > target.load(std::memory_order_relaxed))
        target.store(value, std::memory_order_relaxed);
}

std::string describe(const MidiMessage& m) {
    char line[96];
    const int channel = (m.bytes[0] & 0x0F) + 1;
    switch (m.bytes[0] & 0xF0) {
    case 0x90:
        std::snprintf(line, sizeof line, "ch %2d  note on   %-4s (%3d)  vel %d", channel, noteName(m.bytes[1]).c_str(),
                      m.bytes[1], m.bytes[2]);
        break;
    case 0x80:
        std::snprintf(line, sizeof line, "ch %2d  note off  %-4s (%3d)", channel, noteName(m.bytes[1]).c_str(),
                      m.bytes[1]);
        break;
    case 0xB0:
        std::snprintf(line, sizeof line, "ch %2d  CC %3d = %d", channel, m.bytes[1], m.bytes[2]);
        break;
    default:
        std::snprintf(line, sizeof line, "%s", m.bytes[0] == 0xFA   ? "transport: start"
                                               : m.bytes[0] == 0xFB ? "transport: continue"
                                               : m.bytes[0] == 0xFC ? "transport: stop"
                                                                    : "MIDI message");
    }
    return line;
}

}  // namespace

std::string describe(const Report& r) {
    switch (r.kind) {
    case Report::Kind::Midi:
        return describe(r.message);
    case Report::Kind::Channel:
        return "-> channel " + std::to_string(r.channel + 1);
    case Report::Kind::Button:
        return "[" + label(r.button) + (r.pressed ? " pressed]" : " released]");
    case Report::Kind::Freeze: {
        std::string axes;
        for (int a = 0; a < kAxisCount; ++a)
            if (r.frozenAxes & (1u << a))
                axes += (axes.empty() ? "" : ", ") + label(static_cast<Axis>(a));
        return "[freeze channel " + std::to_string(r.channel + 1) + ": " + (axes.empty() ? "no axis" : axes) + "]";
    }
    }
    return {};
}

// Sends the engine's MIDI to the current output and reports to the main thread.
class Runtime::Tap final : public MidiSink, public EngineObserver {
public:
    Tap(Runtime& runtime, std::shared_ptr<MidiOutput> output) : runtime_(runtime), output_(std::move(output)) {}

    void setOutput(std::shared_ptr<MidiOutput> output) { output_ = std::move(output); }

    void send(const MidiMessage& m) override {
        if (output_)
            output_->send(m);
        if (runtime_.reportMidi_.load(std::memory_order_relaxed))
            runtime_.report({Report::Kind::Midi, m});
    }
    void channelChanged(int channel) override {
        Report r{Report::Kind::Channel};
        r.channel = channel;
        runtime_.report(r);
    }
    void freezeChanged(int channel, std::uint8_t frozenAxes) override {
        Report r{Report::Kind::Freeze};
        r.channel = channel;
        r.frozenAxes = frozenAxes;
        runtime_.report(r);
    }

private:
    Runtime& runtime_;
    std::shared_ptr<MidiOutput> output_;
};

Runtime::Runtime(std::shared_ptr<MidiOutput> output, std::shared_ptr<const Preset> preset, Options options, Log log)
    : log_(std::move(log)), reportMidi_(options.reportMidi), reportButtons_(options.reportButtons),
      dpadSettlePress_(options.dpadSettlePressMs), dpadSettleRelease_(options.dpadSettleReleaseMs),
      humanize_(pack(options.humanize)),
      output_(output), channel_(options.channel) {
    pendingOutput_ = std::move(output);
    thread_ = std::jthread([this, options, preset](std::stop_token stop) { run(stop, options, preset); });
}

Runtime::~Runtime() {
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();
}

void Runtime::setPreset(std::shared_ptr<const Preset> preset) {
    std::lock_guard lock(pendingMutex_);
    pendingPreset_ = std::move(preset);
    hasPending_.store(true, std::memory_order_release);
}

void Runtime::setOutput(std::shared_ptr<MidiOutput> output) {
    output_ = output;
    std::lock_guard lock(pendingMutex_);
    pendingOutput_ = std::move(output);
    hasPending_.store(true, std::memory_order_release);
}

void Runtime::setChannel(int channel) { commands_.push({Command::Kind::SetChannel, channel}); }

void Runtime::panic() { commands_.push({Command::Kind::Panic}); }

LiveState Runtime::live() const {
    LiveState s;
    s.connected = connected_.load(std::memory_order_relaxed);
    s.hasTouchpad = hasTouchpad_.load(std::memory_order_relaxed);
    s.buttons = buttons_.load(std::memory_order_relaxed);
    for (std::size_t a = 0; a < s.axes.size(); ++a)
        s.axes[a] = axes_[a].load(std::memory_order_relaxed);
    for (std::size_t f = 0; f < s.fingers.size(); ++f)
        s.fingers[f] = {fingerDown_[f].load(std::memory_order_relaxed), fingerX_[f].load(std::memory_order_relaxed),
                        fingerY_[f].load(std::memory_order_relaxed)};
    s.channel = channel_.load(std::memory_order_relaxed);
    s.frozenAxes = frozen_.load(std::memory_order_relaxed);
    s.sounding = sounding_.load(std::memory_order_relaxed);
    std::lock_guard lock(nameMutex_);
    s.gamepadName = gamepadName_;
    return s;
}

RuntimeStats Runtime::stats() const {
    RuntimeStats s;
    s.ticks = ticks_.load();
    s.averageWorkUs = s.ticks ? static_cast<double>(totalWorkNs_.load()) / static_cast<double>(s.ticks) / 1000.0 : 0;
    s.maxWorkUs = static_cast<double>(maxWorkNs_.load()) / 1000.0;
    s.maxLateUs = static_cast<double>(maxLateNs_.load()) / 1000.0;
    s.droppedReports = droppedReports_.load();
    s.sendErrors = output_ ? output_->sendErrors() : 0;
    return s;
}

void Runtime::report(const Report& r) {
    if (!reports_.push(r))
        droppedReports_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Runtime::pack(const Humanize& h) {
    auto field = [](int v, int high) { return static_cast<std::uint64_t>(std::clamp(v, 0, high)); };
    return static_cast<std::uint64_t>(h.channels) | field(h.velocityMinPercent, 127) << 16 |
           field(h.velocityMaxPercent, 127) << 23 | field(h.delayMinMs, 1023) << 30 | field(h.delayMaxMs, 1023) << 40;
}

Humanize Runtime::unpack(std::uint64_t w) {
    Humanize h;
    h.channels = static_cast<std::uint16_t>(w & 0xFFFF);
    h.velocityMinPercent = static_cast<int>((w >> 16) & 0x7F);
    h.velocityMaxPercent = static_cast<int>((w >> 23) & 0x7F);
    h.delayMinMs = static_cast<int>((w >> 30) & 0x3FF);
    h.delayMaxMs = static_cast<int>((w >> 40) & 0x3FF);
    return h;
}

void Runtime::publish(const GamepadInput& gamepad, const Engine& engine) {
    const bool connected = gamepad.connected();
    if (connected != connected_.load(std::memory_order_relaxed)) {
        std::lock_guard lock(nameMutex_);  // rare: only when a gamepad comes or goes
        gamepadName_ = gamepad.name();
        hasTouchpad_.store(gamepad.hasTouchpad(), std::memory_order_relaxed);
        connected_.store(connected, std::memory_order_relaxed);
    }
    buttons_.store(static_cast<std::uint32_t>(gamepad.buttons().to_ulong()), std::memory_order_relaxed);
    for (std::size_t a = 0; a < axes_.size(); ++a)
        axes_[a].store(gamepad.axes()[a], std::memory_order_relaxed);
    for (std::size_t f = 0; f < fingerDown_.size(); ++f) {
        fingerDown_[f].store(gamepad.fingers()[f].down, std::memory_order_relaxed);
        fingerX_[f].store(gamepad.fingers()[f].x, std::memory_order_relaxed);
        fingerY_[f].store(gamepad.fingers()[f].y, std::memory_order_relaxed);
    }
    channel_.store(engine.channel(), std::memory_order_relaxed);
    std::uint8_t frozen = 0;
    for (int a = 0; a < kAxisCount; ++a)
        if (engine.isFrozen(static_cast<Axis>(a)))
            frozen = static_cast<std::uint8_t>(frozen | (1u << a));
    frozen_.store(frozen, std::memory_order_relaxed);
    sounding_.store(engine.soundingSlots(), std::memory_order_relaxed);
}

void Runtime::run(std::stop_token stop, Options options, std::shared_ptr<const Preset> preset) {
#ifdef __linux__
    sched_param param{};
    param.sched_priority = 10;
    const bool realtime = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
    log_(realtime ? "Gamepad thread running at real-time priority."
                  : "Real-time priority refused (normal without rtprio rights): normal priority.");
#endif

    std::shared_ptr<MidiOutput> output;
    {
        std::lock_guard lock(pendingMutex_);
        output = std::move(pendingOutput_);
    }
    Tap tap(*this, std::move(output));  // the tap alone owns it, so a replaced output is closed
    Engine engine(tap);
    engine.seedRandom(SDL_GetTicksNS() ^ static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));
    engine.setPreset(std::move(preset));
    engine.setChannel(options.channel);
    engine.setObserver(&tap);
    GamepadInput gamepad(engine, log_);
    gamepad.setButtonTap([this](Button b, bool pressed) {
        if (!reportButtons_.load(std::memory_order_relaxed))
            return;
        Report r{Report::Kind::Button};
        r.button = b;
        r.pressed = pressed;
        report(r);
    });

    const std::uint64_t period = 1'000'000'000ull / static_cast<std::uint64_t>(std::max(options.pollHz, 1));
    std::uint64_t next = SDL_GetTicksNS();
    while (!stop.stop_requested()) {
        const std::uint64_t start = SDL_GetTicksNS();
        storeMax(maxLateNs_, start > next ? start - next : 0);

        if (hasPending_.exchange(false, std::memory_order_acquire)) {
            std::shared_ptr<const Preset> newPreset;
            std::shared_ptr<MidiOutput> newOutput;
            {
                std::lock_guard lock(pendingMutex_);
                newPreset = std::move(pendingPreset_);
                newOutput = std::move(pendingOutput_);
            }
            if (newPreset)
                engine.setPreset(std::move(newPreset));
            if (newOutput) {
                engine.releaseAll();  // to the old output
                tap.setOutput(std::move(newOutput));
            }
        }
        while (auto command = commands_.pop()) {
            if (command->kind == Command::Kind::SetChannel)
                engine.setChannel(command->channel);
            else
                engine.releaseAll();
        }

        engine.setDpadSettle(dpadSettlePress_.load(std::memory_order_relaxed),
                             dpadSettleRelease_.load(std::memory_order_relaxed));
        engine.setHumanize(unpack(humanize_.load(std::memory_order_relaxed)));
        engine.tick(start);
        gamepad.poll(start);
        publish(gamepad, engine);

        const std::uint64_t end = SDL_GetTicksNS();
        ticks_.fetch_add(1, std::memory_order_relaxed);
        totalWorkNs_.fetch_add(end - start, std::memory_order_relaxed);
        storeMax(maxWorkNs_, end - start);

        next += period;
        if (next <= end)
            next = end + period;  // fell behind: skip ahead instead of bursting
        SDL_DelayNS(next - end);
    }
    engine.releaseAll();
}
