# Iris Prism

A **dynamic equaliser** (VST3 + AU + Standalone) for mixing and mastering: eight bands that each work on both channels, on
left or right alone, or on the mid or the side of the stereo image; any band can follow the level of its own frequency
region; a **Clean** mode that is a plain equaliser and a **Sat** mode that warms up the boosts; **Delta** and **Gain Match**
to judge what you did; and **solo** for a band, for what a band adds, and for the low, low-mid, mid, high-mid and high parts
of the spectrum.

## Name origin

*Iris* is the Greek goddess of the rainbow, the messenger who carries light across the sky; a *Prism* is what splits light
into its bands, which is what this plugin lets you do to a sound (and listen to one band at a time). (CMake targets,
namespaces and plugin codes use the short name "Iris".)

![Iris Prism](docs/screenshot.png)

*Headless render of the editor (the hardware-style interface shared with the other plugins) in the "Mono Bass, Wide Air" preset
over a stereo programme whose top is on the sides. Band 1 (violet) is a low cut on the **Side** only, so the bass stays in the
centre; band 2 (violet, selected) is a high shelf on the Side; band 3 (teal) is a bell on the **Mid**. The display draws one curve for each
channel that has a band of its own, in the band's colour (Mid teal, Side violet; Left blue and Right rose when a band is placed on one
channel); the dim trace is the spectrum going in, the cream one coming out. Along the bottom, the five frequency regions are the solo
switches, and the triangles between them move the crossovers. Regenerate with the command under "Development".*

![A dynamic band](docs/dynamics.png)

*The "De-Esser" preset on the same programme: a bell at 7.2 kHz whose gain moves with the level of its own region. The bar
hanging from the node is the range the dynamics can move it through (down to -7 dB); the white dot is where it is now (-1.1 dB, still
releasing after the sibilant burst), and the DYNAMICS panel reads the level the band hears and the gain it applies.*

![Solo](docs/solo.png)

*A solo in progress: the low-mid and mid regions are lit (and shaded in the display, the rest dimmed), and band 2 is on
**CURVE** solo, so what plays is only what that band adds. The pill at the top left says what is soloed; clicking it clears
every solo.*

## Core philosophy

- **A band touches only what you give it.** A band on Left, Right, Mid or Side takes that signal out of the stereo pair, filters it
  and puts back only the *difference*. Everything it does not touch is bit for bit what went in (this is tested), so a Side band
  cannot change the mid, and a plugin with nothing switched on is exactly transparent, with zero latency.
- **Clean means clean.** In Clean mode the equaliser adds no harmonics at any level (below -160 dB in the tests). Sat is a
  deliberate, separate mode with its own controls.
- **Listen to the thing you are changing.** A band, the curve a band draws, one part of the spectrum, or only what the whole
  equaliser changes (Delta), each one switch away; and a Gain Match so that louder does not pass for better.
- **Monitoring is not a setting.** Solos and Delta are not parameters: they are never saved, never recorded as automation, and
  always start off, so no session ever opens with only the lows playing.
- **Analog look, digital brain.** The interface is a piece of studio hardware drawn entirely from code by the kit shared with the
  other plugins ([shared/ui](../../shared/ui)); no image, font or sound files are shipped.
- **Free, public, source only.** The repository holds the code, not a download, under the AGPL-3.0-or-later (see
  [License and terms](#license-and-terms)).

## Features

| Area | What it does |
|---|---|
| **Bands** | Up to eight. Double-click the display where you want one (a bell, at the gain you clicked), drag its node (frequency across, gain up and down; Shift for fine), turn the wheel over the node for Q (for a cut, the wheel steps the slope), right-click it for its menu. **Types**: Bell, Low Shelf, High Shelf, Low Cut, High Cut (12 / 24 / 36 / 48 dB per octave, Butterworth) and Notch. Frequency 20 Hz – 20 kHz, gain ±24 dB, Q 0.1 – 18. All filters are double-precision biquads; a band glides to new settings and fades in and out, so nothing zippers or clicks. |
| **Left / Right / Mid / Side** | Each band has its own **Works on**: Stereo (both channels), Left, Right, Mid or Side. A mid band and a side band are two independent equalisers. Mid is (L+R)/2 and Side is (L-R)/2, and the round trip is exact. A one-channel track is treated as mid. |
| **Curves** | The display sums the bands into one curve per channel that has a band of its own: Left (blue), Right (rose), Mid (teal), Side (violet), and a single amber curve when every band is on Stereo. The selected band's own curve is filled. The dynamic bands' curves move live. |
| **Dynamic** | Any bell or shelf can be dynamic. The band listens to its own frequency region (a band-pass for a bell, a low-pass below a low shelf, a high-pass above a high shelf) on what goes *into* the band, so its own boost cannot feed back. **Threshold** (dB, as the peak of a sine), **Ratio**, **Range** (the most the gain moves; negative cuts further, positive boosts), **Attack**, **Release**, and **Above** or **Below** (engage while the level is over, or under, the threshold). The band rests at its **Gain** and moves towards Gain + Range; a 6 dB soft knee smooths the start. On a Stereo band the two channels are linked. See [How the dynamics work](#how-the-dynamics-work). |
| **Clean / Sat** | **Clean** is a plain equaliser. **Sat** passes what a *boosting* band adds through a soft curve, so a boost that gets loud warms up and rounds off instead of staying perfectly linear: small signals stay linear, cuts and cut filters stay clean. **Drive** (0 – 30 dB) is how far below full scale the knee sits; **Style** is the curve: **Tape** (tanh, odd harmonics), **Tube** (asymmetric, adds the even ones, DC-blocked) or **Console** (arctan, open and gentle). No latency and no oversampling: the shaper is anti-aliased (see [How Sat works](#how-sat-works)). |
| **Delta** | Plays only what the equaliser changes (output minus input). Not saved; a band solo takes over from it. |
| **Gain Match** | A slow (about 0.6 s) RMS match of the output to the input, never more than 12 dB either way, so a boost that is merely louder is not mistaken for better. Off by default; while it is on, the gain it is applying is read out above its button. |
| **Solo a band** | Two ways, both on the band panel (and in the node's menu). **BAND** plays the output through a filter that matches the band, at its frequency and width (below a low shelf or low cut, above a high one): what the band is working on. **CURVE** plays only what the band adds or takes away: its own curve, nothing else. Press the same button again to release. |
| **Solo a frequency region** | The strip along the bottom of the display is five switches: **Low, Low-Mid, Mid, High-Mid, High**, cut at four crossovers (120 Hz, 500 Hz, 2 kHz, 6 kHz by default). Click to solo a region, click more to add others, Alt-click to solo one alone. Drag a triangle between two regions to move the crossover (it cannot be dragged into the next one), double-click it to put it back. Linkwitz-Riley 4th order, and every region goes through the all-pass of the crossovers above it, so any combination of regions adds up exactly: soloing all five sounds like soloing none. The region solo works on whatever you hear, so it combines with Delta and with a band solo. |
| **Meters and output** | IN and OUT peak meters (a NaN or inf sample cannot pin them), output gain ±24 dB. |
| **Presets** | Nine: *Init*, *Master Air*, *Mono Bass, Wide Air*, *Mud Duck*, *De-Esser*, *Open the Top*, *Tilt*, *Warm Tube Lift*, *Console Presence*. Output gain, Gain Match and the crossovers are kept when a preset loads. |
| **Display** | The spectrum before (dim) and after (cream), calibrated in dBFS, 8192-point FFT, a 4.5 dB/oct tilt by default. Right-click empty space for Gain scale (±6 / 12 / 18 / 24 dB), Analyzer range, Speed and Tilt; the choice is saved with the project. |

### How the dynamics work

```
level        L   = what the band hears: the input through the band's filter, mean-square over about two cycles of its
                   own frequency, read as the peak of a sine of the same power (dB)
over         x   = L - Threshold           (Above)      or      Threshold - L           (Below)
moved        m   = x x (1 - 1 / Ratio), through a 6 dB soft knee, never more than |Range|
wanted gain  g   = Gain + sign (Range) x m
applied gain      = g, reached with Attack while it moves further from Gain and with Release while it comes back
```

Attack and release act on the gain, in dB, so the times you set are the times you get: with a 100 ms attack the gain is 2.0 dB of
the way after 15 ms (a single-pole curve gives 1.7), and with a 400 ms release it is at -1.25 dB 900 ms after the signal stops
(the curve gives -1.26). The level reading does not depend on the times: a sine at -26 dB reads -26 dB. The detector cannot be
faster than about two cycles of the band's own frequency, which is a limit of measuring a level, not of this one. The gain and
the coefficients are updated every 32 samples (0.67 ms at 48 kHz).

### How Sat works

The shaper works on the band's *difference* `d = H(x) - x` (what the band adds), not on the signal: `out = x + d + residual(d)`,
where `residual` is the curve minus its straight line, `f(drive x d) / drive - d`. Three things follow, all measured:

- **Small signals are exactly linear.** The residual is cubic, so at -50 dBFS Clean and Sat differ by 0.002 dB.
- **Only boosts saturate.** A cut would be shallower the louder it got, which is not what an equaliser should do; cuts, cut
  filters and notches give the same output bit for bit in both modes.
- **Drive is where the knee is.** With a +6 dB boost on a 0.2 amplitude sine, the 3rd harmonic is at -55.9 dB with the Drive at 0 dB,
  -44.3 dB at 6 dB and -33.6 dB at 12 dB. Push the knee far below the level of the boost itself and the boost gets limited
  (that is what a ceiling does); the display and the ears will tell you where that is.

The residual is anti-aliased with first-order antiderivatives (ADAA) and added to the untouched band output, which is why no oversampling
(and so no latency) is needed: the 3rd harmonic of a 9.7 kHz tone folds to 18.9 kHz, and it is -36.2 dB with the anti-aliasing against -26.5 dB
without. It reduces aliasing, it does not remove it; see [Limitations](#limitations).

### Signal flow

```
in -+-> [band 1] -> [band 2] -> ... -> [band 8] -+-> gain match -+-> output gain -> [region solo] -> out
    |    each band: take Stereo / L / R / Mid / Side out of the pair, filter it, add back only the difference
    |    (the difference of a boosting band goes through the SAT shaper first; a dynamic band moves its gain)
    +-> dry ---------------------------------------+-> DELTA = (matched wet) - dry replaces the wet
                                                   +-> BAND SOLO replaces it with the band's region, or its difference
```

## Using it

1. **Add a band**: double-click the display where you want it. Drag the node; wheel for Q. Choose its **Type** and what it
   **Works on** in the BAND panel.
2. **Find a problem**: put a narrow bell on it and press **BAND** (solo) to hear the region it covers; sweep until it jumps out. Press
   **CURVE** to hear exactly what the band is removing.
3. **Make it follow the sound**: switch **DYNAMIC** on, set the Threshold to where the problem starts (the readout shows the level
   the band hears), set the Range to how much to take off, and watch the white dot move.
4. **Keep the centre solid**: put a low cut on **Side** only; widen the top with a high shelf on **Side**.
5. **Judge it**: **DELTA** to hear what you changed, **GAIN MATCH** to compare at equal loudness, a region solo to check one part of the
   spectrum at a time. Close the plugin (or click CLEAR in the pill) and every solo and Delta end.
6. **Warm the boosts**: switch the mode to **Sat**, pick a Style, and raise Drive until the loud boosts start to round off.

## Build

Iris Prism is source only: you build it yourself. Formats: **VST3 + AU + Standalone** (macOS, universal arm64 + x86_64).
Needs CMake 3.22+ and a C++20 compiler; JUCE is fetched by CMake (the version is in [cmake/Juce.cmake](../../cmake/Juce.cmake)),
or taken from `third_party/JUCE` at the repository root for an offline build.

```bash
# On a machine whose Command Line Tools ship an SDK that Xcode's linker cannot read:
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)

cd plugins/iris
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Iris_VST3 Iris_AU Iris_Standalone -j 10
```

The plugins are in `build/Iris_artefacts/Release/{VST3,AU,Standalone}/`. Building does **not** install them; add
`-DIRIS_INSTALL_AFTER_BUILD=ON` to copy them to `~/Library/Audio/Plug-Ins/{VST3,Components}`, or copy them yourself. On Apple
Silicon an unsigned bundle needs an ad-hoc signature before a host loads it
(`codesign --force --deep --sign - "Iris Prism.vst3"`; `xattr -cr` clears a quarantine flag for personal use). Then rescan plugins in your host.

## Development

```bash
cmake --build build --target IrisTests && ./build/IrisTests_artefacts/Release/IrisTests            # DSP: no plugin, no GUI
cmake --build build --target IrisUiCheck && ./build/IrisUiCheck_artefacts/Release/IrisUiCheck      # mouse drags, wheel, regions, editor panels
cmake --build build --target IrisHostCheck && ./build/IrisHostCheck_artefacts/Release/IrisHostCheck "build/Iris_artefacts/Release/VST3/Iris Prism.vst3"
cmake --build build --target IrisSnapshot && ./build/IrisSnapshot_artefacts/Release/IrisSnapshot docs/screenshot.png "Mono Bass, Wide Air" band=2
auval -v aufx Irsp Irsa                                                                            # Apple's AU validator (the AU must be installed)
```

`docs/screenshot.png`, `dynamics.png` and `solo.png` are the output of `IrisSnapshot` (arguments in the file's header), never
edited by hand: `"Mono Bass, Wide Air" band=2`, `"De-Esser" band=1`, `"Warm Tube Lift" band=1 region=6 solo=2:curve`. They are
regenerated after any change that touches the UI or what it shows. The rules for working on the code are in
[CLAUDE.md](CLAUDE.md); the rules for the repository are in [the root CLAUDE.md](../../CLAUDE.md).

```
Source/DSP/Band.h            band types, placements, BandParams, BandFilter (one band's filter), bandResponseDb (the drawn curve)
Source/DSP/Dynamics.h        the detector and the gain computer
Source/DSP/Saturator.h       the SAT shaper: three curves, residual ADAA, DC block
Source/DSP/Crossover.h       Linkwitz-Riley 4th order and the five-region splitter
Source/DSP/Engine.h          bands in series, placements, dynamics, SAT, gain match, delta, solo, meters, analyzer taps
Source/Parameters.h          parameter layout, presets, readParams(), band helpers  <- the single source of truth for parameters
Source/Plugin*.{h,cpp}       JUCE wrapper and editor
Source/UI/EqView.h           the display: spectrum, curves, nodes, region strip; IrisUi.h: the two words and the placement colours
../../shared/dsp/            biquads (with the EQ designs), the analyzer taps and the FFT analyzer, shared with Hephaestus
../../shared/ui/             the drawing kit of the interface, shared with the other plugins of the repository
Tests/                       DSP tests, UI check, VST3 host check, editor snapshot, shared helpers
docs/                        the three screenshots (generated by IrisSnapshot)
```

### What has and hasn't been verified

Everything in this README that is a number comes from `IrisTests` (headless, 48 kHz unless stated); all of it passes.

- **Transparent.** With no band on, with bands on at 0 dB, and at the default settings in a hosted VST3, the output is the input bit for bit; the
  latency is 0. An output gain of 0.000001 dB (what a host's 0.0 looks like after JUCE snaps it) is treated as exactly 0.
- **The curves are the designed ones.** A +6 dB bell reads +6.000 dB at its centre; shelves and the notch match the design; the
  display's curve (`bandResponseDb`) agrees with the measured response to 0.000 dB at six more frequencies. The cuts are -12.3, -24.1,
  -36.1 and -48.2 dB at half the corner frequency (Butterworth: -12.30, -24.08, -36.12, -48.16) and -3.01 dB at the corner. (A digital
  filter is the analogue one with its frequency axis warped, so a high cut at 8 kHz is -38.2 dB at 16 kHz, as the warped Butterworth gives.)
- **Placement is exact.** A Mid band on a pure-side signal, a Side band on a mono signal, the other channel of a Left or Right band:
  untouched, bit for bit. Mid and Side bands give +6.00 dB where they should.
- **Dynamics.** Under the threshold +0.000 dB; 4 dB over at 4:1 is -3.02 dB (the law gives -3.00); far over it stops at the range (-12.00 dB);
  Below mode gives +6.00 dB on quiet material and +0.006 dB on loud; a loud tone far from the band does not move it. Attack and
  release as quoted above.
- **Clean / Sat.** Clean: 2nd harmonic -240 dB, 3rd -163 dB. Sat, 0.3 amplitude through a +12 dB bell, Drive 12 dB: Tape has the 3rd at
  -17.8 dB, 5th at -26.0 dB and the 2nd at -221 dB (none); Tube has the 2nd at -26.4 dB; Console's 3rd is -19.5 dB (gentler than Tape).
- **Delta** is the processed signal minus the input to 3e-8. **Gain Match**: a +9 dB low shelf on noise is 2.4 dB louder and 0.03 dB
  with the match on; it never takes off more than 12 dB.
- **Solo.** A region passes a tone in the middle of its range within 1.7 dB (the crossovers are 4th order: a tone is -6 dB at its
  crossover) and keeps the next range's tone at least 20.7 dB down and the others at least 68 dB; all five together are flat to
  0.000 dB over ten tones including the crossover frequencies; releasing a solo restores the output bit for bit. BAND solo of a 1 kHz
  bell: +0.00 dB at 1 kHz, -64 dB at 100 Hz, -67 dB at 10 kHz. CURVE solo: a +6 dB bell adds 0.995 of the input at its centre (-0.04 dB).
- **Robust.** The output is identical whether the host sends blocks of 4096, 37 or 1 samples; every band at its extreme with Sat,
  dynamics and a region solo stays finite at 22.05, 44.1, 48, 96 and 192 kHz; a NaN and an inf in the input do not leave the output broken;
  a 12 dB jump, an off/on and a frequency jump never step the output by more than 0.0104 (a click would be over 0.05). Eight dynamic
  bands in Sat mode on every placement cost about 1.1 % of one core (10 s of stereo audio in 0.11 s).
- **As a plugin.** `IrisHostCheck` loads the built VST3 and fuzzes it at 5 sample rates x 5 block sizes (1 to 4096), mono and stereo
  layouts, state and preset round trips and the editor; `IrisUiCheck` drives the display with mouse events; Apple's `auval` passes the AU.
  pluginval was not run, and it has not been tried in a DAW.

### Limitations

- **Minimum phase only.** The bands are IIR filters (there is no linear-phase mode), so they have the phase of the analogue
  filters they model.
- **The dynamics listen to themselves.** The detector is internal: there is no external side-chain, and a band's detector listens to
  the signal entering it, after the bands before it.
- **Sat is not oversampled.** The anti-aliasing is first-order, and aliasing is reduced (about 10 dB on the tone tested), not
  removed. Drive the shaper hard on bright material and it will show.
- **Sat only saturates boosts.** By design; a cut band is always clean.
- **Region solo adds phase while it is on.** The magnitudes add up flat, but each region has the phase of its crossovers; with
  one region soloed you are hearing a band-pass, which is the point.
- **BAND solo of a Left, Right or Mid band plays in both ears** (a Side band plays as a side signal, opposite in the two), so you can hear it.
- **Eight bands**, and a minimum spacing of a ratio of 1.2 between crossovers.
- **Not saved on purpose:** solos and Delta.

## License and terms

Copyright (C) 2026 The Pantheon authors. Free software under the **GNU Affero General Public License, version 3 or (at your
option) any later version**, with **no warranty**. This repository holds source only: no binaries or releases are published
from it. See [LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md) (third-party software, trademarks, the source-only policy),
[CONTRIBUTING.md](../../CONTRIBUTING.md) and [SECURITY.md](../../SECURITY.md).
