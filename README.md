# Morse

The FM-1 as a pocket Morse code machine, on [fm1-base](https://github.com/charlesvestal/fm1-base). MIT.

- **KEY**: key on the piano keys, as a straight key (any key) or iambic A/B paddles
  (left half dit, right half dah), and read what you sent. A sidetone, or silence and the
  lights. Your speed and dah:dit ratio as you go.
- **TYPE**: white keys A–P, black keys Q–Z and a space; hold OCT- for digits and signs.
  Each letter is played as you type it.
- **PRACTICE**: flash cards a few minutes at a time. Leitner boxes per character, two
  cards each (recognise it, send it), new letters five at a time in Koch order, a round
  counter instead of days (the FM-1 has no clock through a power-off). Learning **by
  sound** is the default: no dots and dashes on the screen. By sight is a setting.
- **CHART**: the course as a grid coloured by how well you know each letter.
- **USB-MIDI**: keying out and in as note 0 on channel 1, the Vail adapter's format, so
  [Vail](https://vailmorse.com/) and its training tools take it. Text to and from
  `host/morse` over SysEx: see [`host/morse/PROTOCOL.md`](../../host/morse/PROTOCOL.md).

Docs (Traditional Chinese): [design](../../docs/dev/morse.md),
[cheatsheet](../../docs/dev/morse-cheatsheet.md).

## Files

| File | What |
|---|---|
| `morse.c` / `.h` | code table, PARIS / Farnsworth timing, text player, iambic keyer, adaptive decoder; no platform |
| `learn.c` / `.h` | flash cards: Leitner boxes, rounds, Koch groups, the four choices; no platform |
| `tone.c` / `.h` | two sine voices (sent, received) with 5 ms ramps, mute |
| `app.c` / `.h` | the pages, the 1 kHz tick (key, keyer, players, MIDI), storage, drawing |
| `tests/morse_test.c` | `just morse-unit`: the platform-free parts on the computer |
| `tests/basic.txt` | `just fw-test morse`: the whole app in fm1-base's host simulator |

## Build and test

```sh
just morse-unit          # morse.c, learn.c
just fw-test morse       # the app in the host simulator (screenshots in build/host/)
just fw-build morse      # build/morse.fwsc
just morse-emu           # in fm1-emu with the virtual USB cable
```

Flashing it is a firmware write: owner approval each time (see `AGENTS.md`).

Storage: one record (settings and progress, about 300 bytes) at `0xDA000`–`0xDBFFF`,
written when a round ends or is left, and 3 s after a setting changes. The panel
calibration at `0xDC000` is the platform's.
