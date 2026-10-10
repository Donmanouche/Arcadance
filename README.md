# MidiPad

**Turn a game controller into a MIDI instrument and a live MIDI looper.**

MidiPad reads a DualSense (PlayStation 5) controller and sends MIDI to your DAW or
synthesizer: the sticks, the analog triggers and the two-finger touchpad send CCs,
the buttons play notes and chords, and every channel can carry its own layout.
Hold one button, play, release: what you played loops at once, notes and gestures
alike, on as many channels as you like.

Written in C++20, Linux first; Windows and macOS packages are planned.

- **Download**: a ready-to-run Linux build is attached to each
  [release](https://github.com/Donmanouche/MidiPad/releases).
- **User manual**: [MidiPad User Manual (PDF)](docs/MidiPad%20User%20Manual.pdf).

![MidiPad at work: sticks, triggers, touchpad, loops and the piano roll](docs/media/overview.gif)

## Features

- **A MIDI looper in your hands.** Loop the notes and chords you play, and the
  movements of the sticks, triggers and touchpad fingers as CC curves, like
  hand-drawn LFOs. Every MIDI channel has its own loops, all playing together:
  layer chords on channel 1, a bass line on channel 2, a filter sweep on top.
  Overdub, forward / reverse / back-and-forth playback, Play and Stop for every
  loop at once.
- **Chords made easy.** Named chords (`Fm9`, `Dbmaj7#11`, `Eb9sus4`…) with octave
  and doubled bass root; choose a tonality and the editor offers only the notes
  and chords of the scale.
- **Expressive controls.** Two sticks, two analog triggers and two touchpad
  fingers, each axis with its own CC, range and direction. Freeze a stick at its
  current value; split the touchpad into four pads.
- **Sixteen channels, sixteen layouts.** Switch channels while playing; each
  channel can have its own mapping, so one controller drives a whole
  multitimbral setup.
- **Program Change** from a button (fixed, next, previous) or by sweeping an
  axis through a range of programs, with optional Bank Select.
- **Humanize** velocity and timing per channel.
- **Built for the stage.** One window that plays and edits at the same time, a
  1 kHz real-time input thread, a PANIC button, and a headless mode without any
  window.

![Loops of a stick, the triggers and a touchpad finger, playing beside the live controls](docs/media/loops.gif)

![The piano roll as a loop clip](docs/media/pianoroll.gif)

## Building

Requirements: CMake 3.25 or newer, Ninja, a C++20 compiler (GCC 13 tested). The
libraries (nlohmann/json, SDL3, RtMidi, Dear ImGui) are downloaded by CMake. On Linux you need the ALSA headers (`libasound2-dev` on Debian and
Ubuntu); JACK is optional.

```bash
cmake --preset release
cmake --build --preset release
```

The program is built as `build/release/src/app/midipad`.

### Touchpad access (Linux)

The DualSense touchpad is read from `/dev/hidraw*`, which needs a udev rule.
Without it the controller works, but without the touchpad, and MidiPad says so
when the controller connects. Create `/etc/udev/rules.d/70-dualsense.rules`:

```
# USB
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="054c", ATTRS{idProduct}=="0ce6", MODE="0666"
# Bluetooth
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", KERNELS=="0005:054C:0CE6.*", MODE="0666"
```

then run `sudo udevadm control --reload-rules && sudo udevadm trigger` and
reconnect the controller.

## The window

```bash
build/release/src/app/midipad
```

The window is an animated view built for the stage, which plays while it edits.
**F11** toggles fullscreen, **Ctrl+S** saves the preset, **Ctrl+Q** quits.

![The modules follow the controller and show what each control sends](docs/media/modules.gif)

- One module per part of the controller: left and right sticks, triggers,
  D-pad, touchpad. Each has an animated screen that follows the controller, the
  CCs it sends, and a LOOP panel.
- Below the modules: the piano roll, the SCALE block (the tonality on a
  one-octave keyboard; click to choose it) and the CHANNEL block (the active
  channel in large digits, the sixteen channels, a violet dot where a loop
  plays).
- A strip of pads for the buttons (✕ ○ □ △ in PlayStation colours): they flash
  on every press and stay lit while their note or chord sounds.
- At the top: the ☰ menu, the channel, the preset, ▶ / ■ for the loops, PANIC.
- At the bottom: the MIDI output on the left, the controller on the right.

Click a control (a pad, a D-pad direction, a touchpad zone, an axis pill, a
screen, a LOOP panel) to edit it in a window over the view. It edits the mapping
of the active channel if it has one, otherwise the base mapping.

![A mapping window: a chord on the Cross button](docs/media/mapping.png)

**Presets**: the PRESET menu (with previous / next) opens a preset, and offers
New…, Save as… and Delete… (the file is deleted after a confirmation). A preset
is one JSON file, channel mappings included. Changes are heard at once and
written to disk when you save.

**Tonality** (optional, shared by all channels), chosen by clicking the SCALE
screen: a root and a scale (major, natural, harmonic and melodic minor, the
modes, pentatonics, blues, whole tone). The mapping window then offers only the
notes of the scale and, for chords, a *Chord of the key* menu listing, degree by
degree, every chord whose notes belong to the scale. Free entry stays
available; a note or chord outside the scale is flagged.

**Options** (☰ menu): the MIDI interface (a drop-down showing the name under
which each port is seen; a saved port that is missing shows in red), the
VirMIDI ports, the D-pad delay, humanization and the presets folder.

![The Options window](docs/media/options.png)

Settings (presets folder, last preset, output, options) are kept in
`~/.local/share/MidiPad/MidiPad/settings.json`. The default presets folder is
`~/.local/share/MidiPad/MidiPad/presets` (`midipad path/to/preset.json` uses
that preset's folder instead).

### VirMIDI ports (Linux)

Some programs only see the kernel's raw MIDI devices (`hw:`), not ALSA software
ports such as MidiPad's virtual port. The `snd-virmidi` module provides such
devices.

MidiPad can create them: ☰ › **Options** › MIDI INTERFACE › *VirMIDI ports*.
Choose the number of ports (1 to 4) and, if you like, *At every startup*; the
system asks for the administrator password (through `pkexec`). Without that box
the ports disappear at the next reboot; with it, MidiPad writes
`/etc/modules-load.d/midipad-virmidi.conf` and
`/etc/modprobe.d/midipad-virmidi.conf`. *Remove* deletes them (close the
programs using them first). By hand: `sudo modprobe snd-virmidi midi_devs=4`.

Then:

1. in MidiPad, choose the output `VirMIDI N-0`;
2. in the program receiving the MIDI, use the input `Virtual Raw MIDI/…`
   (`VirMIDI N-1` appears as `Virtual Raw MIDI 2/…`, and so on).

At startup MidiPad reconnects to the last port used. If it is missing (or on the
very first launch), a window asks which port to use, showing for each VirMIDI
port the name under which it is seen, and offers to create VirMIDI ports if
there are none; the virtual port is used meanwhile. The card number N depends on
the order in which devices are found; MidiPad finds its VirMIDI port again
regardless.

The controller is still read while the MidiPad window is in the background.

### D-pad diagonals

Nobody presses or releases the two arrows of a diagonal at exactly the same
time, so a new direction plays only once it has stayed stable for a moment
(15 ms by default, on press and on release, adjustable in Options): the
direction crossed on the way stays silent. Releasing the whole D-pad stops the
note at once.

## Playing without a window

```bash
build/release/src/app/midipad --headless my-preset.json
```

Creates a virtual ALSA MIDI port named *MidiPad* to connect to your DAW, and
reads the first controller connected, before or after the start. The preset is
reloaded as soon as its file is saved; if it is invalid, the previous one stays
active and the error is printed.

| Option | Effect |
|---|---|
| `-c NAME` | send to an existing port (for example `-c "VirMIDI 0-0"`) instead of the virtual port |
| `-n NAME` | name of the virtual port (for example `-n "MidiPad Control"`) |
| `--api alsa\|jack` | MIDI API on Linux (default: alsa) |
| `--channel N` | starting channel (1-16) |
| `-m` | monitor: print every message sent |
| `-i` | also print the buttons and freeze changes |
| `--stats` | print the measured processing times on exit |
| `--poll-hz N` | controller readings per second (default: 1000) |
| `--list-ports` | list the MIDI output ports |

### Latency

A dedicated thread reads the controller 1,000 times per second (`--poll-hz`),
with real-time priority when the system allows it, and sends the MIDI directly.
Measured with a DualSense over an 18-minute session: 21 µs of processing per
reading on average (0.5 ms at worst), woken up at most 1.6 ms late, no send
error.

SDL's gamepad events are deliberately disabled: the main thread's event loop
draws the interface, and drawing must never delay the MIDI. For the same reason
the controller does not drive the interface (keyboard and mouse only).

## Things to know

- **Chords**: a capital `M` means major (`CM7` = Cmaj7, not Cm7); `C6/9` is
  accepted. The octave of the root is set in the mapping window (`octave` in the
  preset, 4 by default: C4 = MIDI note 60); a chord with a note outside the MIDI
  range is refused. *Bass root* doubles the root one or two octaves lower to
  anchor the bass (`root_below`: 1 or 2).
- **Movement loops (Loop record)**: while a *Loop record* button is held,
  MidiPad records what moves: a stick (beyond about 12 % of its travel), a
  touchpad finger or the triggers (their travel, not the notes they play as
  buttons), several at once if needed. On release, each X/Y movement loops,
  like an LFO, on the channel where it was recorded. The moments when a finger
  was lifted are recorded too, and play nothing. The loop plays beside the
  stick, finger or triggers, which stay free: it has CCs of its own (70/71 and
  72/73 for the sticks, 74/75 and 76/77 for the fingers, 78/79 for L2/R2 by
  default) and its playback mode (forward, reverse, back and forth), set in the
  window of the stick, touchpad or triggers (`loops` in the preset). Each
  channel has its own loops: record on channel 1, switch to channel 2, record
  again; all play together. A press without moving anything stops the loops of
  the active channel; PANIC stops those of every channel. Loops are not kept
  from one session to the next.
- **Piano roll and note loops**: the piano roll shows the notes and chords of
  the active channel. Without a note loop, the last seconds scroll towards the
  *now* line on the right edge (white; violet for what a loop played). Hold
  *Loop record* while playing notes or chords (buttons, D-pad, triggers as
  buttons, touchpad pads) to record them: on release they loop on the channel
  where they were played, with their velocity and humanization, forward,
  backward or back and forth (one note loop per channel). A note still held at
  the end sounds until the end of the loop; everything sounding stops when the
  loop starts over. While it plays, the piano roll becomes a **clip**: the
  whole loop, standing still, swept by a playhead; its notes light up as the
  playhead crosses them. Click the panel to set the playback mode
  (`loops.notes` in the preset).
- **Overdub**: while a note loop plays, a short tap (under 500 ms, without
  playing) on *Loop record* turns overdub on: the notes played next are added
  to the loop where it plays, without changing its length. A second tap turns
  it off. During overdub the piano roll shows *OVERDUB* with an orange
  playhead, each added note appears in orange while held, and the Record pad is
  circled in orange. Holding *Loop record* while playing records a new note
  loop that replaces the old one. A long press without playing, or a short one
  without a note loop, stops the loops of the channel.
- **Play / Stop**: ▶ and ■ in the top bar drive the loops of every channel. ■
  pauses them without erasing them; ▶ restarts them all from their start and
  turns green while they play. A recording started while stopped restarts all
  loops.
- **Empty LOOP boxes** show a small grey pixel animation in the style of 1980s
  and 1990s video games, drawn at random at startup, different for each box of
  a channel.
- **Freeze** holds axes at their current value. In the view the shape stays ice
  blue and follows the stick; a ghost and a crosshair mark the frozen spot until
  it is released, so you can go back to it before releasing.
- **Triggers L2/R2** work both as axes (CC) and as buttons; as buttons they
  press at 25 % of their travel and release at 15 %.
- **Program Change from a button**: a fixed program (1 to 128), or *Next (+1)* /
  *Previous (-1)* from the last program sent on the channel (the first press
  sends 1). *Wrap around* goes from 128 to 1 and back; otherwise stepping stops
  at the ends. *Bank select* sends CC 0 (MSB) and/or CC 32 (LSB) first. In the
  preset: `"type": "program"` with `"program"` (0 to 127) or `"step"` (1 or -1)
  and `"wrap"`, plus optional `"bank_msb"` / `"bank_lsb"`.
- **Program Change from an axis**: a stick, trigger or touchpad finger axis can
  choose a program in a range (*From* / *To*) instead of sending a CC. The
  program is sent once the axis has stayed on it for 150 ms, so sweeping does
  not load every program on the way, and nothing is sent when the axis springs
  back to rest: aim, wait, release. *Bank select* sets a bank for that axis,
  sent before each program, so two axes can browse two different banks. In the
  preset: `"program": true` instead of `"cc"`, plus optional `"bank_msb"` /
  `"bank_lsb"`. Pills show *PC 1-16*, pads *PC 12* or *PC +1*.
- **Transport**: start, stop and continue work from buttons too.
- **No stuck notes**: a note always stops on the channel where it started, even
  after a channel or preset change while playing.
- **Per-channel mappings**: changing channel while playing (a `channel` button)
  also switches to that channel's mapping.
- **Axes** send a CC only when its value changes.
- **Humanize**: in Options, choose the channels, a velocity range (in % of the
  mapping's velocity, 100 % by default) and a delay range (0 to 500 ms). Each
  note, including each note of a chord, draws its velocity and delay within
  these ranges; its release is delayed by the same amount, so its length does
  not change. A control can opt out (*Humanize* box in the mapping window,
  `"humanize": false` in the preset). PANIC stops everything at once, waiting
  notes included.
- **Exclusive toggles**: notes and chords in toggle mode exclude each other
  within one family of controls (buttons, D-pad, each touchpad finger). CC
  toggles are always independent.

## Architecture

```
src/app/       midipad: SDL3 input (GamepadInput), RtMidi output (MidiOutput),
               real-time thread (Runtime), console mode (Headless)
  gui/         ImGui window (GuiApp), animated view (PerformanceView) and
               mapping window (MappingEditor)
src/core/      library without system dependencies (tested)
  Engine       controller events → MIDI messages; no allocation at run time
  Preset*      mapping model, JSON reading and writing
  Chords/Notes music theory
  PresetLibrary, AppSettings, Labels   presets folder, settings, labels
assets/fonts/  Roboto Bold and Roboto Condensed Bold (Apache 2.0 licence), embedded
docs/          the user manual (PDF) and the images of this page
```

Controls are named after SDL3's gamepad API (`south`, `left_x`, `up_right`…).

The animations on this page come from the window itself:
`midipad --demo --record DIR --frames N --fps F` renders a demo state off screen
and saves each frame as a BMP, ready to be turned into a GIF.

## Status

| Step | Content | Status |
|---|---|---|
| 1 | Engine (pure logic), chords, preset format | done |
| 2 | SDL3 real-time thread (controller + touchpad) and RtMidi, headless executable | done, tested with a DualSense |
| 3 | Interface (Dear ImGui): animated view, mapping, presets, options | done |
| 4 | Loop recording, humanization, Program Change | done |
| 5 | Windows and macOS packages | to do |
