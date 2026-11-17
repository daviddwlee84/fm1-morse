# How MORSE teaches, and why

MORSE does not invent a way to learn Morse code. It puts a few well-known methods into a pocket
device. This page says what each method is, where it comes from, and what MORSE does with it, so
you can judge the choices or change them. Where a claim comes from secondary sources, it says so.

## Learn each character as a sound (Koch)

**The method.** The German psychologist Ludwig Koch reported in 1936 that Morse is best learnt as
acoustic shapes, at a speed fast enough that you cannot count the dots and dashes, and that visual
charts should not be used. His learners started with two characters at full speed and added one
more each time they copied the ones they had at about 90 %. The commonly quoted result is a class
copying 12 words per minute in under 14 hours; that figure comes from summaries, not the original
text. ([summary of Koch's 1936 report](https://longislandcwclub.org/wp-content/uploads/2022/11/EXECUTIVE-SUMMARY-KOCH-1936.pdf),
[Hackaday on the method](https://hackaday.com/2020/02/21/learning-morse-code-the-ludwig-koch-way))

**Why it matters.** People who learn from a chart of dots and dashes, or at slow character speeds,
tend to hear a letter, count its elements and look it up. That works at 5 WPM and stalls around
10-15 WPM, and the habit is hard to break. CW Academy calls the alternative *instant character
recognition*: the whole sound of a character, recognised at once.
([CW Academy beginner curriculum](https://cwops.org/wp-content/uploads/2025/02/Beginner-curriculum.htm),
[LCWO ICR guidelines](https://cwops.org/wp-content/uploads/2024/08/LCWO-ICR-Guidelines.pdf))

**In MORSE.**

- New letters come in Koch order as [LCWO](https://lcwo.net/) uses it (K M U R E S N A P T L W I . J Z
  = F O Y , V G 5 / Q 9 2 H 3 8 B ? 4 7 C 1 D 6 0 X), five at a time. The next five open once every
  open letter is recognised well (box 2) and sent right once (box 1): a stricter, card-by-card form
  of Koch's "90 %, then one more".
- Characters are played at 20 WPM by default (the START preset).
- **LEARN BY: SOUND** is the default: no dots and dashes on the screen while you learn to recognise
  a letter. Muted, the key lights flash its rhythm instead of drawing it.
- A peek is allowed where it does no harm: `SEQ` shows a new letter's code, or an answer's, and it
  hides again on the next card. Cards that test recognition cannot be peeked at.
- Sending is another skill. Seeing your own marks is useful feedback there, so KEY's CODE and
  TIMING views, RHYTHM, and a keyed answer's SHOULD / YOU bars are shown whatever LEARN BY says.

## Fast characters, stretched gaps (Farnsworth)

**The method.** Farnsworth timing keeps each character at a high speed but lengthens the gaps
between characters and words, so the overall speed is slow while each letter still sounds like
itself. It is written as two speeds, the character speed and the overall (effective) speed. The
ARRL wrote its own timing standard for it, since no definitive one existed; W1AW's practice files
currently use 15 WPM characters below 18 WPM.
([ARRL timing standard, by Jon Bloom KE3Z](https://uoxyc.github.io/morse/kodbok/fs_fansworth_x9004008.pdf),
[ARRL code practice files](https://www.arrl.org/code-practice-files))

Koch himself did not stretch gaps; modern trainers such as LCWO combine Koch's order with
Farnsworth spacing, and so does MORSE.

**In MORSE.** `K1` is the character speed, `K2` the effective speed. The presets go from START
(characters at 20, overall 8) through BUILDING (20/13) and QSO (22/18) to FAST (25/25). Speeds use
PARIS, 50 units: a dit is 1200 / WPM ms. These presets are our reading of the methods above, not
values from any one course. Some guidance (LCWO's ICR guidelines) suggests 25-30 WPM characters to
stop counting entirely; turn `K1` up if 20 lets you count.

## Your hand is slower than your ear

Koch and Farnsworth are about hearing. Sending is a motor skill, and a beginner's hand cannot key
20 WPM evenly. So MORSE keeps two speeds: hearing (`K1`, `K2`) and sending (`K4`, default 10 WPM).

A keyed answer in PRACTICE is judged by **its own rhythm**, not a speed: a dah about three dits, a
gap inside a letter about one. Its shortest dah must be at least 1.5 times its longest dit, at
whatever speed you key. Letters of one kind of element (E T I M S O H 5 0) have nothing inside them
to compare, so they are judged against your hand's speed, carried from card to card. Each keyed
answer gets a grade for how close it is to the textbook 1:3:1, and a near miss counts as *almost*.
`JUDGING: STRICT` judges at the send speed instead. RHYTHM drills even presses on their own.

## Spaced repetition by rounds (Leitner)

**The method.** Sebastian Leitner proposed in 1972 (*So lernt man lernen*) a box system for flash
cards: a card answered right moves to the next box, a card missed goes back to the first, and lower
boxes come round more often. It is a simple form of spaced repetition.
([Leitner system](https://en.wikipedia.org/wiki/Leitner_system))

**In MORSE.**

- Two cards per letter, *recognise it* (hear it, pick it from four; later key what you hear) and
  *send it* (see the letter, key its code), each in a box 0-5. Box n comes back every 2^n rounds.
- Rounds, not days: the FM-1 has no clock that keeps running when it is off.
- A round asks new and due cards first; the weak ones among them (box 0-1, new letters too) come
  back once more later in the round, the way a deck repeats a new card on its first day; only then
  are free places filled with letters you know.
- A missed card is asked again at the end of the round; right the second time, it does not move up.
- The chart colours each letter by its boxes. You can mark letters known, open groups, step every
  box back when you are rusty, or take a placement test; each change waits for your `PLAY`.

## What MORSE leaves to others

Words, call signs, contest exchanges, long copy and talking to people are trained better elsewhere.
[Vail](https://vailmorse.com/) and its [training site](https://training.vailmorse.com/) do all of
them, and take the FM-1 as a MIDI key. MORSE is for the daily few minutes: the characters, their
sound, and an even hand.

## Sources

- Koch: [summary of the 1936 report](https://longislandcwclub.org/wp-content/uploads/2022/11/EXECUTIVE-SUMMARY-KOCH-1936.pdf),
  [Hackaday](https://hackaday.com/2020/02/21/learning-morse-code-the-ludwig-koch-way),
  [LCWO](https://lcwo.net/) (the order MORSE uses).
- Instant character recognition: [CW Academy beginner curriculum](https://cwops.org/wp-content/uploads/2025/02/Beginner-curriculum.htm),
  [LCWO ICR guidelines](https://cwops.org/wp-content/uploads/2024/08/LCWO-ICR-Guidelines.pdf).
- Farnsworth: [ARRL timing standard](https://uoxyc.github.io/morse/kodbok/fs_fansworth_x9004008.pdf),
  [ARRL code practice files](https://www.arrl.org/code-practice-files).
- Leitner: [Wikipedia](https://en.wikipedia.org/wiki/Leitner_system).
