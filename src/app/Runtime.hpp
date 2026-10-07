#pragma once

#include "MidiOutput.hpp"

#include "midipad/Engine.hpp"
#include "midipad/SpscRing.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Something the input thread reports to the main thread.
struct Report {
    enum class Kind : std::uint8_t { Midi, Channel, Button, Freeze } kind = Kind::Midi;
    midipad::MidiMessage message{};
    int channel = 0;
    midipad::Button button{};
    bool pressed = false;
    std::uint8_t frozenAxes = 0;
};

std::string describe(const Report& report);

// Controller and engine state, for display.
struct LiveState {
    bool connected = false;
    bool hasTouchpad = false;
    std::string gamepadName;
    std::uint32_t buttons = 0;  // bit per SDL gamepad button
    std::array<std::int16_t, midipad::kAxisCount> axes{};
    struct Finger {
        bool down = false;
        float x = 0, y = 0;
    };
    std::array<Finger, midipad::kFingerCount> fingers{};
    int channel = 0;
    std::uint8_t frozenAxes = 0;  // on the active channel

    bool isDown(midipad::Button b) const { return (buttons >> static_cast<int>(b)) & 1u; }
};

struct RuntimeStats {
    std::uint64_t ticks = 0;
    double averageWorkUs = 0, maxWorkUs = 0, maxLateUs = 0;
    std::uint64_t droppedReports = 0;
    std::uint64_t sendErrors = 0;
};

// Owns the real-time input thread: it polls the gamepad, runs the engine and
// sends MIDI. Every method is called from the main thread and never waits for
// the input thread; changes reach it on its next poll.
//
// SDL must be initialized with SDL_INIT_GAMEPAD and gamepad/joystick events
// disabled before a Runtime is created.
class Runtime {
public:
    struct Options {
        int pollHz = 1000;
        int channel = 0;
        bool reportMidi = false;
        bool reportButtons = false;
    };
    // Called from the input thread, only on rare events (gamepad connected...).
    using Log = std::function<void(const std::string&)>;

    Runtime(std::shared_ptr<MidiOutput> output, std::shared_ptr<const midipad::Preset> preset, Options options,
            Log log);
    ~Runtime();  // turns every note off, then stops the thread
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void setPreset(std::shared_ptr<const midipad::Preset> preset);
    // Notes still on are turned off on the previous output first.
    void setOutput(std::shared_ptr<MidiOutput> output);
    void setChannel(int channel);
    void panic();
    void setReportMidi(bool on) { reportMidi_.store(on, std::memory_order_relaxed); }
    void setReportButtons(bool on) { reportButtons_.store(on, std::memory_order_relaxed); }

    std::optional<Report> popReport() { return reports_.pop(); }
    LiveState live() const;
    RuntimeStats stats() const;

private:
    class Tap;
    struct Command {
        enum class Kind : std::uint8_t { SetChannel, Panic } kind;
        int channel = 0;
    };

    void run(std::stop_token stop, Options options, std::shared_ptr<const midipad::Preset> preset);
    void report(const Report& r);
    void publish(const class GamepadInput& gamepad, const midipad::Engine& engine);

    Log log_;
    std::atomic<bool> reportMidi_, reportButtons_;
    std::shared_ptr<MidiOutput> output_;  // main thread's view, for stats

    midipad::SpscRing<Report, 4096> reports_;
    std::atomic<std::uint64_t> droppedReports_{0};
    midipad::SpscRing<Command, 64> commands_;

    std::mutex pendingMutex_;
    std::shared_ptr<const midipad::Preset> pendingPreset_;
    std::shared_ptr<MidiOutput> pendingOutput_;
    std::atomic<bool> hasPending_{false};

    // Published by the input thread after each poll.
    std::atomic<bool> connected_{false}, hasTouchpad_{false};
    std::atomic<std::uint32_t> buttons_{0};
    std::array<std::atomic<std::int16_t>, midipad::kAxisCount> axes_{};
    std::array<std::atomic<bool>, midipad::kFingerCount> fingerDown_{};
    std::array<std::atomic<float>, midipad::kFingerCount> fingerX_{}, fingerY_{};
    std::atomic<int> channel_{0};
    std::atomic<std::uint8_t> frozen_{0};
    mutable std::mutex nameMutex_;
    std::string gamepadName_;

    std::atomic<std::uint64_t> ticks_{0}, totalWorkNs_{0}, maxWorkNs_{0}, maxLateNs_{0};

    std::jthread thread_;  // last: stops before the members above go away
};
