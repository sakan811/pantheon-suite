# Hestia Space

A **stem spacer** (VST3 + AU + Standalone) for mixing: it makes room between the stems of a mix, in frequency and in depth, while they play. Put one
instance on each stem (kick, bass, vocal, guitars, pads...), give each its **role** and the same **mix** letter, and each one gives up the
frequencies where a stem with a bigger claim is playing, only while it plays, and by no more than **CARVE**. The stem that owns a band is
never touched there. **DEPTH** then puts each stem back from the front with the cues distance gives a sound: the air takes the highs, the level falls, the
sides narrow and the first reflections of a room come up (no reverb, no tail). The instances find each other on their own, with no routing, and any track
can join the mix through the **sidechain** input, with Hestia on it or not.

## Name origin

*Hestia* is the goddess of the hearth, the fire at the middle of the house around which everyone has a place. Her gift is that there is
room for all of them. (CMake targets, namespaces and plugin codes use the short name "Hestia".)

## What it does

| | |
|---|---|
| **Role** | What the stem is: Kick, Bass, Drums, Hats / Cymbals, Lead Vocal, Backing Vocal, Guitar, Keys, Pad, Lead Synth, Strings, FX or Full Range. A role is a *claim* on each of thirty third-octave bands from 25 Hz to 20 kHz (a kick's thump, a bass's low end, a voice's presence), drawn from common mixing practice. They are rules of thumb, and the display shows what each stem holds. |
| **Mix** | Off, or A to H. Stems of the same letter see each other; a new instance is in A, so the stems of a session find each other with no set-up. Off: the stem is alone and nothing is cut. |
| **Importance** | 0 to 100%, 50% by default. Scales the role's claim from half to one and a half times: raise it on the stem that must not lose room, lower it on the one that may. Two stems of the same role and importance have equal claims and leave each other alone. |
| **Carve** | 0 to 18 dB, 6 by default: the most any band is cut, when a stem with a clearly bigger claim is playing there as loud as this one. |
| **Depth** | 0 to 100%, 0 (the front) by default: how far back the stem sits. At 100%: a high shelf at 5 kHz of -9 dB (the air), -4 dB of level, 30% of the sides gone, and four early reflections on each channel (6 to 37 ms, summed and darkened, at a gain of 0.45). A stem at the back also has up to 35% less claim, so it gives way to the ones in front. At 0% it is the input bit for bit. |
| **Sidechain** | A second input (off until the host routes a track to it). The track on it is heard by Hestia as one more stem of the mix, with the **SC role** and **SC importance** you set; it is only listened to, never heard. Use it for a track that has no Hestia on it (any other chain, any other vendor's plugins) or that you cannot put one on. |
| **Reach** | 0 to 36 dB, 12 by default: how much quieter than this stem another can be and still be given room. 0 dB: only when it is as loud or louder. |
| **Attack / Release** | How fast a cut comes in (30 ms) and goes out (250 ms). |
| **Output** | Gain after the cut. |
| **Delta** | Listen to what Hestia changes in the stem (its output minus its input: the cut and the depth). Monitoring, not saved with the session. |
| **Presets** | Ten starting points, one for each kind of stem (Init, Kick, Bass, Drum Bus, Lead Vocal, Backing Vocals, Guitars, Lead Synth, Pads and Strings, FX and Ambience): a role, an importance, a cut and a depth. The mix letter, the sidechain's role and importance, and OUTPUT are kept. |

The display (above, all the stems' levels, in the colour of each, and the sidechain as a white line; the strip, who owns each band; the amber bars, the cut
this stem is making now; on the right, the depth map: where each stem of the mix sits from the front to the back) is what the plugin reads and decides,
not a separate analysis.

## How it works

1. Each instance measures its input in thirty third-octave bands (a fourth-order band-pass per band, mean square, 50 ms; no FFT, so no block and
   no delay) and publishes the levels, its role and its importance to a small table of 32 slots that lives in the plugin binary. All the
   instances a host loads from that binary into one process share the table.
2. Every 64 samples, each instance reads the other stems of its mix that are live (published in the last 500 ms) and, for each band, asks of each
   one: is its claim there bigger than mine (by 0.5 or more it takes all of it), is it there at all (-80 to -60 dB), and is it within REACH of
   my level. What those take, combined as shares of the band, times CARVE, is the cut wanted in that band. The track on the sidechain is measured
   the same way and is one more of the stems it asks about.
3. The cut follows that with the ATTACK and RELEASE, and is made by thirty peaking bells in series, minimum phase, no delay. The bells overlap, so
   the cut wanted at the band centres is turned into bell gains by inverting the overlap: a flat -6 dB is -6 dB at every centre.
4. The depth is added after the cut: the high shelf, the narrowing of the sides, the first reflections (a delay line with eight taps, each heard once,
   so no feedback and no tail) and the level. The direct sound is never delayed.

## Measured (`HestiaTests`, 48 kHz)

- The cut: a flat -6 dB is within 0.4 dB of -6 at every band centre; one band cut by 9 dB is 9 dB down within 0.4 dB at its own centre, whichever
  of the thirty it is, and three or more bands away it is untouched within 0.4 dB. With nothing to cut the bells are not run, so the stem is the input bit for bit (also in a build that fuses multiplies and adds).
- The level bank: a sine on the centre of any band reads its level within 0.8 dB; three bands away it is 20 dB under, six or more 40 dB.
- A bass and a lead vocal on the same tone at 79 Hz: the vocal is cut by 5.9 dB (DEPTH 6), the bass by 0.0 dB; on 2 kHz it is the other way round;
  on different tones neither is cut.
- Stems of the same role and importance are not cut; raise one's IMPORTANCE and the other gives way. With REACH 6 dB a stem 12 dB louder than the
  other is not cut for it; with 24 dB it is half way in (-3 dB of 6).
- A stem that stops being processed is forgotten half a second later, and the cut goes away.
- Depth: at 100% the direct sound of an impulse is the shelf's first sample times the narrowing times the level (left 0.2400, right 0.0424: the
  expected values to four places); each left reflection is at 6.3, 13.1, 21.7 and 31.9 ms at 0.45 x (0.5, 0.4, 0.3, 0.2) x the damping pole x the level, to
  under 1%, the right channel has none from a left-only impulse, and 45 ms after the impulse there is nothing left. Back at 0% the stem is bit for bit the input
  again. Two guitars on one tone, one at 80%: the near one is untouched, the far one gives up 2.6 dB more (2.7 expected from its 28% smaller claim).
- Sidechain: a vocal with a bass on the sidechain on the same tone is cut by 5.9 dB, the same as with a bass stem on the bus (5.9 dB); with the sidechain
  silent, disconnected, or the mix Off, the stem passes bit for bit; a disconnected sidechain ends its cut and its level on the display.
- The output is the same bit for bit for blocks of 1, 7, 64, 100, 512 and 4096 samples, with a neighbour on the bus and a depth of 60%, and with a sidechain
  and a depth of 50%.
- A stem alone in its mix, or in mix Off, or with CARVE 0 and DEPTH 0, passes bit for bit.
- Thirty-two stems each get a place on the bus; a thirty-third works alone.

## Limits

- **One process.** The instances with Hestia on them see each other only inside one host process. A host that runs each plugin in its own process (some hosts'
  sandboxing or bridging), or a bounce on another machine, does not link them: each then works as if alone, and says so in the notes.
- **It only cuts.** It never lifts a band, and it never touches the stem that owns one. For a fixed correction use an equaliser (Iris).
- **Depth is not a reverb.** It is distance cues and eight early reflections up to 37 ms, with no tail and no feedback: it says how far back the dry sound
  sits, it does not make a space around it. Use a reverb for that. The cues are fixed in size (9 dB of air, 4 dB of level, 30% of the sides, 0.45 of
  room at 100%), and there is no pre-delay or room size to set.
- **The sidechain is one stem, and it is only as good as its role.** Hestia cannot tell what a track on the sidechain is; it is the SC role and importance
  you set. The sidechain works wherever the host can route it (it does not need the one-process limit); the host has to be able to feed a second input.
- **The roles are rules of thumb.** A claim is a curve from mixing practice, not a measurement of your sound; two stems with the same role and
  importance do not cut each other, and the stem that matters is yours to say (IMPORTANCE).
- **A note is cut after it begins.** The other stems are read as they play, not ahead of time, and the level is smoothed over 50 ms, so the first
  tens of milliseconds of a hard note are not carved. There is no look-ahead and no delay.
- **Offline bounces can differ in the finest details.** The stems of a session are not processed in lockstep (they run on different threads, in
  an order the host chooses), so what one reads of the others is as it stands, not a fixed value; the result for one stem is the same for any
  block size when the others do not change (test [8]) but not bit for bit across runs of a whole session.
- **Third-octave resolution.** The cut is smooth over about a third of an octave; it cannot notch a single narrow resonance.
- **Stereo and mono.** The same cut is made on both channels; there is no per-channel or mid/side cut.
- **Plugin binaries.** Each plugin format (VST3, AU) and each build has its own table: a VST3 and an AU of Hestia in one session do not see each other.

## Building

Requires CMake 3.22 or newer and a C++20 compiler. JUCE is fetched by CMake (see [cmake/Juce.cmake](../../cmake/Juce.cmake)).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Hestia_VST3 Hestia_AU Hestia_Standalone -j 10
```

Building does **not** install the plugins; add `-DHESTIA_INSTALL_AFTER_BUILD=ON` for that (macOS).

## Development

```bash
cmake --build build --target HestiaTests && ./build/HestiaTests_artefacts/Release/HestiaTests            # DSP: no plugin, no GUI
cmake --build build --target HestiaUiCheck && ./build/HestiaUiCheck_artefacts/Release/HestiaUiCheck      # the editor on the real processor
cmake --build build --target HestiaSnapshot && ./build/HestiaSnapshot_artefacts/Release/HestiaSnapshot docs/screenshot.png
```

`docs/screenshot.png` is the output of `HestiaSnapshot` (arguments in the file's header), never edited by hand. See
[CLAUDE.md](CLAUDE.md) for the rules that keep the plugin correct.

## Licence

AGPL-3.0-or-later, source only. See [LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md) (third-party software, trademarks,
the source-only policy), [CONTRIBUTING.md](../../CONTRIBUTING.md) and [SECURITY.md](../../SECURITY.md).
