# DFORM

**English** | [日本語](README.ja.md)

(Module ID: `dform`)

A Sound Generator for Schwung / Ableton Move. Inspired by the Moog DFAM, it is an
analogue-style percussion voice built from two oscillators, FM and a ladder low-pass
filter, and it **plays by itself on every 1/16 note, in time with Move's transport**.
No clips or MIDI notes needed.

It aims for the crunchy, grinding, wah-ing sound you get from a longish decay hammered at
1/16, where each hit is still ringing when the next one stacks on top of it.
Every knob's range is narrowed so that wherever you turn it, it stays musically usable.

- **Record knob motion**: turn on the REC pad (top right) and turn a knob while touching
  it. The motion is recorded into the loop and played back every pass. Only the time
  you are touching the knob is recorded.
- **Choose the steps on the pads**: the bottom two pad rows switch the 16 steps on and off.
- **Generate a melody**: press the RANDOM pad next to REC and a random melody is written
  in as a pitch per step.
- **Store 8 presets**: the second pad row from the top saves the sound, the knob motion,
  the steps and the scale together, and recalls them with one tap.
- **Pitch stays in the scale**: PITCH (knob 8) only picks notes of the selected scale
  (C Phrygian by default).

---

## Building

This repository builds on its own. The Schwung plugin API header (`plugin_api_v1.h`,
MIT) is bundled in `third_party/schwung/` (unmodified).

```bash
scripts/build.sh arm64
```

→ `dist/dform-module.tar.gz` (AArch64 `dsp.so` for Move + UI + licences)

Requirements: `aarch64-linux-gnu-gcc`, `python3`. During the build `scripts/check_abi.py`
checks that the library is AArch64, needs glibc 2.34 or older, and exports
`move_plugin_init_v2`, and fails the build otherwise.

The full test suite (native ASan/UBSan engine tests, a dlopen smoke test of the real
`dsp.so`, UI tests and finally the arm64 build):

```bash
scripts/test.sh
```

The UI tests need Node.js and a checkout of Schwung itself (for its page engine and
drawing harness): `../schwung` next to this repository, or
`SCHWUNG_ROOT=/path/to/schwung`. If neither is found, only the UI tests are skipped
(on WSL the Windows `node.exe` works too).

## Installing on Move

Built packages are on the GitHub **Releases** page (`dform-module.tar.gz`); you do not
need to build anything yourself.

Upload through Schwung Manager (`http://<Move's IP>:7700`):

```bash
# install a package downloaded from Releases
scripts/install.py http://move.local:7700 --package dform-module.tar.gz
# install your own build (dist/)
scripts/install.py http://move.local:7700
```

(`--check` only tests the connection.) Then choose **DFORM** as the Synth of a chain slot.

**Restart Move after an update.** The old `dsp.so` stays loaded in the process, so
re-selecting the slot does not switch to the new version.

## Getting started

The basic flow is: put it in a slot → press Play → shape the sound with the knobs →
build a pattern on the pads → record knob motion with REC.

### 1. Make a sound

1. Open a slot on the Schwung screen (Shift+Vol+Track, or hold a track button) and
   choose **DFORM** as the Synth.
2. Open the slot's Synth (DFORM) to get the DFORM screen.
3. Press **Play** on Move. DFORM **plays by itself on every 1/16 note, in time with the
   tempo**. On Stop the sound fades out in about 25 ms and nothing plays while stopped.
   MIDI notes do not play it (the Move track's clips can stay empty).

### 2. Shape the sound (main page)

| Knobs | | | |
|---|---|---|---|
| 1 VOLUME | 2 VCF DECAY | 3 P.ENV | 4 FM AMOUNT |
| 5 P.DEC | 6 CUTOFF | 7 RES/DRIVE | 8 PITCH |

These four are good ones to start with:

- **VCF DECAY** (knob 2): how long the filter takes to close after opening on each hit.
  Longer values keep the filter moving widely through the stream of hits.
- **CUTOFF** (knob 6) and **RES/DRIVE** (knob 7): brightness and roughness. Raising
  RES/DRIVE goes from round → booming → crunchy → savage.
- **FM AMOUNT** (knob 4): higher goes from a fat tom → metallic → a crashing sound.
- **PITCH** (knob 8): pitch. Each click moves to the next note of the scale and the
  note name appears on screen.

**P.ENV / P.DEC** (knobs 3 / 5) are how far and how fast the pitch drops from above at
the moment of the hit (the "boom" or "pew" of a drum).

**Turn the jog wheel** to move to the other pages (see the page table below).
The note length (VCA DECAY) and the filter envelope amount (VCF ENV) are on page 2.

### 3. Build a pattern (pads)

While the screen is open the pads belong to DFORM (they do not reach Move).

```
                              RANDOM  REC   ← right two pads of the top row
 P1  P2  P3  P4  P5  P6  P7  P8             ← second row from the top (presets)
  9  10  11  12  13  14  15  16             ← second row from the bottom
  1   2   3   4   5   6   7   8             ← bottom row
```

- The **bottom two rows** switch whether that step plays: on (bright) / off (dark).
  The step lit white is the one playing now.
- **RANDOM** writes a new melody into PITCH. Press it as often as you like until you
  get one you like. The range is centred on PITCH (knob 8). It works with REC off and
  while stopped. To undo, **hold Shift and touch knob 8** (clears the PITCH recording).
  See "RANDOM (melody generation)" below for details.
- **The second row from the top (P1–P8)** holds presets. **Hold Shift and tap** to save
  the current state to that pad; **tap** to recall it. See "Presets" below.

### 4. Record knob motion (REC)

1. Press **REC** (the top-right pad) to turn it on (solid red; dim when off).
2. With Move playing, **touch and turn** the knob you want to record. The motion is
   recorded into the loop only while you are touching it; letting go ends the take.
   Even without turning, touching overwrites with the current value.
3. A recorded knob plays its motion back on every pass from then on. The knob's own
   value does not change, and wherever nothing is recorded the knob's value plays.
4. To erase: **hold Shift and touch a knob** to clear that knob's recording.
   **Shift + touching knob 1 (VOLUME)** clears everything.
5. When you are done, press REC again to turn it off so a stray touch records nothing.

The nine sound knobs other than VOLUME can be recorded (VCF DECAY, P.ENV, FM AMOUNT,
P.DEC, CUTOFF, RES/DRIVE, PITCH, VCA DECAY, VCF ENV). VOLUME is not recorded, to avoid a
sudden blast from an accidental take.

### 5. When you are done

Close the screen with **Back**. The pads go back to Move with their colours restored.
The sound, recordings and step on/off are saved with the set (the slot's autosave).
The REC pad is not saved and always starts off when the screen is reopened.

## Controls (reference)

The screen and knobs look and work like Schwung's standard knob screen (eight knobs per
page), with knob touch (for recording) and the pads added.

| Control | Action |
|---|---|
| Knobs 1–8 | Parameters of the current page (page table below) |
| Touch a knob (REC on, playing) | Records that knob's motion |
| Shift + touch a knob | Clears that knob's recording; on VOLUME (knob 1) clears **everything** |
| Hold Shift while turning | Fine adjustment |
| Jog turn / click | Change page, select (same as Schwung's standard knob screen) |
| Back | Close the screen (pads go back to Move) |

Pages:

| Page | Knobs 1–8 |
|---|---|
| 1 Main | VOLUME, VCF DECAY, P.ENV, FM AMOUNT, P.DEC, CUTOFF, RES/DRIVE, PITCH |
| 2 Tone | VCA DECAY, VCF ENV, VCO2 TN, VCO2, NOISE |
| 3 Scale | ROOT, SCALE |
| 4 Motion | LENGTH |

The top of the screen shows the loop position (e.g. `DFORM 5/16`), `ARM` while REC is on,
`REC` while recording, and the note name while PITCH is touched (e.g. `PITCH Eb2`).
Knobs with a recording use Schwung's "modulated" display (the playing value moves).

### Knobs and ranges

Values at the 0 / 25 / 50 / 75 / 100 % positions, from left to right.

| Knob | What it does | Range |
|---|---|---|
| VOLUME | Level. 0 % is complete silence | -12 dB at 50 % |
| VCF DECAY | Time for the filter to close | 80 / 270 / 620 / 1280 / 2500 ms |
| P.ENV | Pitch jump at the moment of the hit | 0 / 4.5 / 12.7 / 23.4 / 36 semitones |
| FM AMOUNT | FM depth from oscillator 1 → 2 | 0 / 0.33 / 0.92 / 1.69 / 2.6 (FM index) |
| P.DEC | How fast the jumped pitch comes back | 5 / 15 / 45 / 134 / 400 ms |
| CUTOFF | Low-pass filter frequency | 25 / 125 / 628 / 3145 / 15760 Hz |
| RES/DRIVE | Resonance + distortion | Clean at 0 %; more character and distortion as it rises |
| PITCH | Pitch (scale notes only) | C1 (32.7 Hz) to C5 (523 Hz); 29 notes in C Phrygian |
| VCA DECAY | Note length | 50 / 170 / 415 / 940 / 2000 ms |
| VCF ENV | How far each hit opens the filter | 0 / 1.0 / 2.7 / 4.7 / 7.0 octaves |
| VCO2 TN | Oscillator 2 pitch (offset from 1) | -24 to +24 semitones (default +7.3) |
| VCO2 | Oscillator 2 level | |
| NOISE | Noise amount | |
| ROOT | Root note of the scale | C to B (default C) |
| SCALE | Scale | Phrygian (default) / Minor / Dorian / Major / Mixolyd / Lydian / Locrian / HarmMin / PhrygDom / MinPent / In (Miyako-bushi) / Chromatic |
| LENGTH | Loop length | 1/2 Bar / 1 Bar (default) / 2 Bars / 4 Bars |

- All times are how long the sound takes to fall to 1/1000 (-60 dB). Even at its middle
  VCF DECAY lasts 0.6 s, so the filter keeps moving for four 1/16 notes at 120 BPM.
- With VCA DECAY longer than a 1/16, each hit overlaps the previous one while it still
  sounds, giving a continuous "bwom bwom".
- Even at its maximum RES/DRIVE stops just short of self-oscillation, so the level never
  jumps suddenly.
- PITCH and P.ENV take their value at the moment of each 1/16 hit. Turning them mid-step
  does not glide the pitch; it changes on the next 1/16.
- Changing ROOT or SCALE moves PITCH, and any recorded PITCH motion, to the nearest note
  of the new scale.

### Pads

**While this screen is open, DFORM takes over the pads.** Move's own instruments and
sequencer do not receive pad presses. Closing the screen gives the pads back to Move with
their colours restored (in Schwung a module can only take the pads while its screen is
shown).

```
top     .   .   .   .   .   .   RANDOM  REC      ← REC is the top-right pad
        P1  P2  P3  P4  P5  P6  P7      P8       ← presets 1–8
        9  10  11  12  13  14   15      16       ← triggers 9–16
bottom  1   2   3   4   5   6    7       8       ← triggers 1–8
```

| Pad | Action |
|---|---|
| **REC** (top right) | Knob-motion recording on/off. **On = solid red, off = dim** |
| **RANDOM** (left of REC) | Generates a new melody and writes it into PITCH, step by step |
| Second row from the top (8 pads) | Presets. Tap = recall, Shift + tap = save |
| Bottom two rows (16 pads) | Each step's hit on/off. On = bright, off = dark, the playing step = white |
| Everything else | Nothing |

- Hits are 1/16 notes locked to Move's transport, with the first step on the bar line.
- An off step starts no new hit; the previous note keeps decaying. Recorded knob motion
  keeps running through off steps.
- With LENGTH at 1/2 Bar the second row from the bottom (9–16) is dark and unused. At
  2 Bars / 4 Bars the same 16 steps repeat every bar.
- See the next section for the details of RANDOM.

### Presets

The eight pads of the second row from the top each store the whole current state.

- **Save**: hold Shift and tap a pad. The top of the screen shows e.g. `SAVED PRESET 3`.
  Saving to the same pad again overwrites it.
- **Recall**: tap a pad. It switches on the spot, playing or stopped. An empty pad shows
  `EMPTY 3 (SHIFT+PAD SAVES)` and changes nothing.
- **What is saved**: the values of every knob on every page, the recorded knob motion,
  the on/off of the 16 steps, and ROOT / SCALE / LENGTH. The REC pad's state is not saved.
- **Pad colours**: off = empty, dim = saved, white = the one last recalled (or saved).
- Presets are stored on Move in `/data/UserData/schwung/dform/presets.json`. The same
  eight are available to DFORM in every slot and survive switching sets.

### RANDOM (melody generation)

RANDOM **builds a slightly different melody each time from the MIDI files (source
phrases) in `patterns/` and writes it into PITCH**. Six source phrases are included.
You can replace them with your own MIDI, which changes the melodies RANDOM makes
(see "Replacing the source phrases" below).

**What happens when you press it**

1. One source phrase is picked at random (the one used last time is less likely).
2. Its sequence of notes is varied by "50 % mutation": roughly half of the notes change
   — moving to a neighbouring scale note, jumping an octave, repeating the previous
   note, and so on — and the rest stay as in the source. The result differs every time
   and never repeats the previous melody.
3. No rests are made. Rests in the source phrase are filled with notes too, so every step
   gets a pitch.
4. The melody is written into the loop's steps as a PITCH recording.
   - With LENGTH at 1/2 Bar an 8-step melody is made; at 1 Bar or longer a 16-step
     melody (two 8-step variations in a row), repeated at 2 / 4 Bars.
   - Any earlier PITCH recording is overwritten for the length of the loop.
   - Whether a step plays is set by the bottom two pad rows. RANDOM does not change the
     step on/off.

**Range and scale**

- Every written note is a note of the scale set on the Scale page (ROOT, SCALE). Like
  knob 8's value it is written as "the nth note of the scale", so it never drifts later.
- The range is **based on the note of knob 8 (PITCH), from a fifth below to an octave and
  a half above** (-7 to +17 semitones). Notes outside it are folded back by octaves. The
  melody keeps its shape and knob 8 sets the overall register (set knob 8 before
  pressing).
- At the ends of the pitch range the range is cut short. With knob 8 at C1 the melody
  stays within C1 to F2.

**Undo / try again**

- Press it again to write a different melody.
- **Shift + touch knob 8** clears the PITCH recording and goes back to knob 8's note alone.
- With REC on, touch and turn knob 8 to rewrite just some of the steps by hand.

#### Replacing the source phrases

The source MIDI is embedded in `dsp.so` at build time (nothing is read from files on
Move). After replacing it you must **rebuild and reinstall**.

1. Put the MIDI files you want (Standard MIDI Files, `.mid`) in `patterns/`.
2. List the files to use in `patterns/patterns.txt`, one per line (1 to 16). The line
   order is the source phrase number. Everything after `#` is a comment.

   ```
   my_riff.mid          # 8 steps from the start of the file
   long_phrase.mid 16   # 8 steps from step 17 (the second column is the start)
   ```

   If `patterns.txt` is deleted, every `.mid` in `patterns/` is used in name order.
3. Build, install, and restart Move.

   ```bash
   scripts/build.sh arm64
   scripts/install.py http://move.local:7700
   ```

Which part of a MIDI file is used:

- Only **eight 1/16 notes from the start position** of each file (anything longer is
  ignored). To use another part of a long file, give the start position (in 1/16 notes)
  in the second column.
- Only **the pitches and their order** are used. Velocity and note length are ignored
  (DFORM hits every 1/16 at the same strength). Rests are filled with notes as above.
- Notes are snapped to the nearest 1/16. When a step holds several notes (a chord, for
  example) only the loudest one (the first, on a tie) is used. All tracks and channels
  are read together.
- Source phrases are first snapped to **C Phrygian** when loaded, varied, and finally
  fitted to the current ROOT and SCALE. MIDI written in anything other than C Phrygian
  changes slightly, so writing in Phrygian on C keeps the shape best.
- A file with no notes in the eight steps from its start position is not used. File
  names are read up to 31 bytes and up to 256 notes per file.

### Recording knob motion (details)

- With REC on and Move playing, a knob's motion is recorded into the loop **while you
  touch it**. It is recorded at 8 points per 1/16 note, so even a slow filter sweep
  comes back smoothly.
- Touching a knob that already has a recording lets you continue from the value playing
  now (the sound does not jump when you touch it).
- CUTOFF, FM AMOUNT, RES/DRIVE and VCF ENV are smoothed between the recorded points;
  VCF DECAY, VCA DECAY, PITCH, P.ENV and P.DEC use each point's value as is (so
  step-by-step changes stay crisp).
- Touching a knob while stopped records nothing. The recording position is the loop
  position counted from Move's transport start.
- Closing the screen with a knob still touched stops the recording automatically.
- Knob touches only reach DFORM while its screen is open. When you leave the Schwung
  screen for Move's, the knobs control Move itself.

## How it sounds the way it does

```
oscillator 1 (square) ──FM──> oscillator 2 (triangle)
   P.ENV (pitch jump) on both; the FM depth swells on each hit and falls back
oscillator 1 + oscillator 2 + noise
   -> drive -> 4-pole ladder low-pass filter (VCF ENV opens it on each hit)
   -> VCA (VCA DECAY) -> VOLUME
```

- The oscillators and filter run at twice the sample rate to keep harsh aliasing down.
- Every hit restarts the four envelopes (level, filter, pitch, FM) **from their current
  values** (not from zero). A hit arriving while the previous note still rings joins it
  into one continuous sound without a click.
- CUTOFF, FM AMOUNT, RES/DRIVE, VCF ENV and VOLUME are smoothed so they do not produce
  zipper noise.
- A light limiter at the end keeps the sound from clipping at VOLUME 100 % with any
  settings.

The sound engine code is original, written for this module. See `LICENSE` (MIT) for the
licence and `THIRD_PARTY_LICENSES.md` for other bundled code.

DFAM is a product (and trademark) of Moog Music. DFORM is unofficial software, not
affiliated with Moog, and uses none of the DFAM's circuit or firmware code.
