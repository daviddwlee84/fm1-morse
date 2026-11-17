# MORSE for the M-VAVE FM-1

A pocket Morse code machine for the [M-VAVE FM-1](https://www.m-vave.com/), built on
[fm1-base](https://github.com/charlesvestal/fm1-base). Key on the piano keys and read what you
sent, drill short and long presses, learn the code by sound with flash cards, listen to endless
practice, type letters and CW phrases on the keys. GPL-3.0-or-later.

![MORSE on the FM-1](docs/screen.png)

## What it does

| Page | What |
|---|---|
| **PRACTICE** | Flash cards a few minutes at a time. Two cards per letter (recognise it, send it), each in a Leitner box; new letters five at a time in Koch order. The schedule counts rounds, not days (the FM-1 keeps no clock through a power-off). Learning **by sound** is the default: no dots and dashes on the screen. |
| **LISTEN** | Plays on and on: Koch groups of the letters you have, common words, a made-up QSO, CW abbreviations with their meaning, sayings. Shown as it plays, or hidden for copying. |
| **KEY** | Key freely and read what you sent: as text, as each letter over the dits and dahs you keyed, or as bars to scale. Your speed, dah:dit ratio and the gaps inside letters. |
| **RHYTHM** | Drills of short and long presses against the target length, a judgement for each press, a hint, and a send speed that fits your hand. |
| **TYPE** | The keys are letters, digits and signs, or CW phrases and prosigns (`CQ`, `<SK>`, `73`...). Each key plays at once, or compose a line and send it. |
| **CHART** | The course as a grid, coloured by how well you know each letter. |

A straight key (any key) or iambic A/B paddles (left half dit, right half dah). A sidetone at your
pitch, received signals a third higher, or silence and the key lights. Hearing and sending have
their own speeds: Koch wants the characters heard fast, a beginner's hand is slow.

Keying goes out and comes in over USB-MIDI as note 0 on channel 1, the
[Vail adapter's format](https://github.com/Vail-CW/vail-adapter/blob/master/docs/MIDI_INTEGRATION_SPEC.md),
so [Vail](https://vailmorse.com/) and its training tools take it. A host program
([`host/morse`](https://github.com/daviddwlee84/M-VAVE_FM-1/tree/main/host/morse)) types what you
send into the computer and sends text for the FM-1 to play.

## Controls

| Control | Anywhere |
|---|---|
| `HOME` | the menu (again: back) |
| `ALGO` | the next page |
| `FX` | sound on / off (off: read the key lights) |
| `K1`, `K2` | hearing speed, and the spacing between letters (Farnsworth) |
| `K3` | pitch |
| `K4` | sending speed: the keyer, the decoder's start, the RHYTHM target |
| `MASTER` | volume |

| Page | Controls |
|---|---|
| PRACTICE | `PLAY` start / again; `E4` `F4` `G4` `A4` (lit) pick one of four, or `SELECT` + `SEL`; key the cards that ask for it; `SEL` don't know |
| LISTEN | `PLAY` start / stop; `SELECT` program; `SEQ` show / hide; `SEL` peek |
| KEY | `SEQ` view (text, code, timing); `OCT-` clear |
| RHYTHM | `SELECT` drill; `PLAY` start again |
| TYPE | `OCT-` / `OCT+` page (letters, digits and signs, phrases); `REC` delete, held: clear; `PLAY` play / stop; `SEL` live / compose; `SEQ` the code on the keys |
| CHART | `SELECT` move; `PLAY` hear it |
| SETTINGS | `SELECT` item; `OCT-` / `OCT+` change. PRESET sets the speeds together: START (hear 20/8, send 10), BUILDING, QSO, FAST |

A longer guide (Traditional Chinese):
[design](https://daviddwlee84.github.io/M-VAVE_FM-1/dev/morse/),
[cheatsheet](https://daviddwlee84.github.io/M-VAVE_FM-1/dev/morse-cheatsheet/).

## Install

Back up your FM-1's flash first ([M-Flasher](https://github.com/jvitkauskas/m-flasher)).

- From a release: download `morse-X.Y.fwsc` from
  [Releases](https://github.com/daviddwlee84/fm1-morse/releases) and install it with M-Flasher, or
  over USB-MIDI with fm1-base's `tools/fm1_install.py morse-X.Y.fwsc`.
- The package carries fm1-base's update loader, so another firmware can be installed from MORSE
  the usual way.
- Flash chips: the build saves to and updates on both the first FM-1s' (`856014`) and the newer
  ones' (`B36014`). Tested on a unit with the newer chip.

## Other firmwares' data

MORSE does not read other firmwares' projects or samples. It keeps one record, its settings and
progress (about 300 bytes), at `0xDA000`-`0xDBFFF`, written when a round ends and a few seconds
after a setting changes. On Felucca-family firmwares (SLOOP and others) that range is inside the
area for samples: if you keep many samples there, back up before switching. It writes nothing else
outside its own firmware area, and does not touch the global settings at `0xFC000`. (An update to
another firmware, as with every fm1-base firmware, stages its loader at `0xE0000`.)

## Build

```sh
git clone --recursive https://github.com/daviddwlee84/fm1-morse
cd fm1-morse
python3 fm1-base/tools/build.py . --host          # the host simulator (see fm1-base's README for its tools)
cd build/host && ./morse_host ../../tests/basic.txt .
cc -Wall -Wextra -o /tmp/morse_test tests/morse_test.c && /tmp/morse_test   # the platform-free parts
python3 fm1-base/tools/build.py .                 # the device package (JieLi toolchain: fm1-base's README)
```

| File | What |
|---|---|
| `morse.c` | code table, PARIS / Farnsworth timing, text player with prosigns, iambic keyer, adaptive decoder |
| `learn.c` | flash cards: Leitner boxes, rounds, Koch groups, the four choices |
| `content.c` | what LISTEN plays and TYPE's phrases |
| `tone.c` | two sine voices with 5 ms ramps, mute |
| `app.c` | the pages, the 1 kHz tick (key, keyer, players, MIDI), storage, drawing |
| `common/` | hostlink (SysEx with a host over USB / BLE) and ledfx (LED patterns) |
| `tests/` | `morse_test.c` for the platform-free parts, `basic.txt` for fm1-base's host simulator |

Development and the host program live in
[M-VAVE_FM-1](https://github.com/daviddwlee84/M-VAVE_FM-1), which has this repository as a
submodule.

## License

GPL-3.0-or-later (see `LICENSE`): you may share and change it, and what you build from it and
share must stay open too. Versions up to commit `57ea786` were published under MIT and stay
available under it.

- `common/` (hostlink, ledfx) is MIT, shared with other firmwares in
  [M-VAVE_FM-1](https://github.com/daviddwlee84/M-VAVE_FM-1).
- fm1-base, in `fm1-base/`, is MPL-2.0 (its files stay under it).
- The Koch order is LCWO's; the MIDI keying follows the Vail adapter's published format.
- Everything you hear is generated in code: no samples.
