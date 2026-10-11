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
- **Ableton Link sync, or none.** Join the tempo of Live, Bitwig, a phone app or
  any Link peer on the network: loops snap to whole beats (1 beat, 3 beats,
  1 bar, 2.5 bars…), start on the beat, follow tempo changes, and Play / Stop
  can follow the session. With Link off, MidiPad plays free as before.
- **Humanize** velocity and timing per channel.
- **Built for the stage.** One window that plays and edits at the same time, a
  1 kHz real-time input thread, a PANIC button, and a headless mode without any
  window.

![Loops of a stick, the triggers and a touchpad finger, playing beside the live controls](docs/media/loops.gif)

![The piano roll as a loop clip](docs/media/pianoroll.gif)

## Building

Requirements: CMake 3.25 or newer, Ninja, a C++20 compiler (GCC 13 tested). The
libraries (nlohmann/json, SDL3, RtMidi, Dear ImGui, Ableton Link) are downloaded by CMake. On Linux you need the ALSA headers (`libasound2-dev` on Debian and
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
- At the top: the ☰ menu, the channel, the preset, ▶ / ■ for the loops, LINK
  (the tempo, the peers and the beat; click to turn Link on or off), PANIC.
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
VirMIDI ports, SYNC (Ableton Link, the loops' snap, beats per bar, start / stop
sync), the D-pad delay, humanization and the presets folder.

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

## License

MidiPad is free software under the [GNU General Public License v3](LICENSE).
It uses [Ableton Link](https://github.com/Ableton/link) (GPL v2 or later).
