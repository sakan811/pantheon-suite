# Hephaestus Forge

A **harmonic forge for mastering** (VST3 + AU + Standalone) that fills in harmonics and missing fundamentals for tonal
balance: you draw bands over the spectrum, and each one either saturates what is inside it or adds only the harmonics that
are missing above it.

## Name origin

*Hephaestus* is the Greek god of the forge, of metalworking and of the craftsmen's tools: the one who hammers a shape out of
raw metal; *Forge* is what the plugin does to a master's spectrum, forging harmonics where the sound is thin. (CMake targets,
namespaces and plugin codes use the short name "Hephaestus".)

![Hephaestus Forge](docs/screenshot.png)

*Headless render of the editor (the hardware-style interface shared with the other plugins) on an 808-style sub note (52 Hz) with nothing above it, in the "808 Sub Fill" preset. Band 2 (teal) is a Harmonic Fill band: the dim input trace has no energy above the note, the cream output has grown harmonics at 104 and 156 Hz, and the band's tab reads FILL 85 % because that is how much of its Amount it is currently adding. The tinted zone under "HARMONICS ADDED" is where that band's harmonics land; "ADAPT LISTENS HERE" marks the region the adaptive fill measures. Under the display, the band solo strip: the six main bands of music, each switch spanning its band on the axis. Regenerate with the command under "Development".*

## Core philosophy

- **A band adds only what you ask for.** A Saturate band shapes a band-passed copy of the signal and sums only the *difference*
  (what the shaper added) onto the untouched, latency-matched input. Nothing outside the bands is filtered, so bands can
  overlap or leave gaps, and the plugin is exactly transparent at small signal for any arrangement.
- **Fill what is missing, not what is already there.** Harmonic Fill measures the *input* and generates only the shortfall
  against the amount you set, so the balance holds from one moment to the next instead of piling harmonics onto passages
  that already have them. There is no feedback loop to destabilise.
- **Exact where it can be, honest where it cannot.** The harmonics are weighted exactly (Chebyshev polynomials of the band
  signal divided by its own envelope, or, in the **Clean** mode, sines locked to the band's strongest note), aliasing is
  controlled by ADAA inside linear-phase oversampling, and every claim in
  this README is a measurement from the test suite. Where something is a heuristic, the [Limitations](#limitations) say so.
- **Made for judging, with mastering tools in reach.** **Delta** (hear only what the plugin adds), **Gain Match**
  (loudness-matched A/B), **Mid/Side** mode, a latency-compensated **Mix** and a live input-vs-output spectrum, so more
  saturation is not mistaken for "better".
- **Analog look, digital brain.** The interface is a piece of studio hardware drawn entirely from code by the kit shared
  with the other plugins ([shared/ui](../../shared/ui)): no image, font or sound files are shipped.
- **Free, public, source only.** The repository holds the code, not a download, under the AGPL-3.0-or-later (see
  [License and terms](#license-and-terms)).

## Features

| Area | What it does |
|---|---|
| **Bands** | Up to six bands, any ranges, overlapping or not. Double-click the spectrum where you want one and choose *Saturate* or *Harmonic Fill*, or use the buttons in the band panel; drag a band's edges to resize it and its body to move it; right-click it to change its mode or remove it. Each band has its own **Low** / **High** edge (20 Hz – 20 kHz; a fill band stops at 2 kHz, and an edge can't come closer than a ratio of 1.5). Bands fade in and out over one block when they are added or removed, and a removed band costs no CPU. |
| **Saturate band** (amber) | Drives the sound inside the band into a waveshaper (ADAA + linear-phase oversampling), so harmonics appear above it. Per-band **Drive**; **Curve** and **Even** are shared. Anything outside the band is left exactly as it is. **Drive** is how hard the band is driven into the curve; small signals stay untouched whatever the drive. **Curve**: Tanh (smooth), Atan (open, tape-like) or Soft (algebraic, gritty, strongest upper odd harmonics). **Even**: 0 % = symmetric curve, odd harmonics only (3rd, 5th…); higher = asymmetric, adds 2nd, 4th… (tube-like). |
| **Harmonic Fill band** (teal) | Adds harmonics (H2–H5) of the band's content *above* the band, but only as many as are missing. The energy just above the band is measured against the band's own, and only the shortfall to **Amount** is generated (see [Why Harmonic Fill is adaptive](#why-harmonic-fill-is-adaptive)). **Amount**: harmonic energy above the band relative to the band's own, in dB; also what Adapt aims to keep there; off at the minimum. **Tone**: which harmonics carry that energy: dark = mostly the 2nd; middle = the usual virtual-bass roll-off (−3 / −9 / −16 dB for H3 / H4 / H5 re H2); bright = led by the 3rd. The four weights always carry unit energy, so Amount means the same at any tone. **Adapt**: 0 % = a fixed amount, 100 % = only what is missing. **Rebuild** also regenerates a fundamental that is physically missing while its harmonics are present (difference tone, 3f − 2f = f), and only where it is missing; off by default and experimental on dense material. |
| **Warm / Clean** (fill band, **CLEAN** switch, top of the band panel) | How a fill band makes its harmonics. **Warm** (off, the default, and how every earlier session sounds) is a waveshaper: Chebyshev polynomials of the band signal over its own envelope. That is the usual way to make virtual bass and it behaves like one, so the harmonics follow what the band holds and how fast its level moves: two notes at once intermodulate, and when the level falls faster than the envelope follows (a tremolo, a gated or fast-decaying note) the harmonics fall away, by about 30 dB at the bottom of a 3 Hz tremolo. **Clean** follows the one strongest note in the band and builds each harmonic as a pure sine at *k* times its frequency, locked to its phase and at its amplitude, with the same Amount, Tone and Adapt weights. Nothing else in the band is bent into new frequencies, and the balance of the harmonics does not depend on the level or the envelope. The price: it needs **one dominant note** (an 808, a sub, a bass line); it takes about 100 ms to lock onto a new note; and while the band holds several notes of similar level it adds less, down to nothing, instead of intermodulating (see [Why Clean follows one note](#why-clean-follows-one-note)). **Rebuild** is a squarer and stays what it is in either mode. |
| **Where the harmonics land** | Both kinds show where they add harmonics: the selected band gets a bracketed zone labelled **HARMONICS ADDED**, from the 2nd harmonic of its Low edge to the 5th of its High edge (stopping short of Nyquist, and for a fill band short of any harmonic that would alias); every other band shows it as a faint tint. A fill band with Amount off adds none and shows none. |
| **Spectrum analyzer** | Live input-vs-output spectrum that follows FabFilter Pro-Q's: an 8192-point FFT, **4.5 dB/oct tilt around 1 kHz** by default (so typical programme material looks flat), a 90 dB range, fast attack and a selectable release. Right-click empty space in it for Range (60 / 90 / 120 dB), Speed and Tilt (0 / 3 / 4.5 / 6 dB/oct); the choice is saved with the project. |
| **ABOVE read-out** | A fill band's tab shows, next to FILL %, **ABOVE**: the energy already in the region the adaptive fill listens to, relative to the band's own, in dB (the scale **Amount** is set on). ABOVE well under Amount means the harmonics are missing and the fill is adding them; ABOVE at or over Amount means they are already there. It is what the fill compares, so it includes the band's own tone leaking through the region's filter skirt (a lone 60 Hz tone in a 40–120 Hz band reads about −16 dB; test [5c]). |
| **Solo a band** | **SOLO BAND** and **SOLO ADDS** (band panel, left of PASTE) listen to the selected band alone; press again to release. **BAND** plays the signal through a filter over the band's range (what the band works on), **ADDS** plays only what the band adds, its distortion or its filled harmonics, so the soloed bands' ADDS sum to the Delta (test [12]). The solo replaces Mix and Delta, fades over 10 ms, is not saved with the session and ends when the editor closes; a solo on another band than the one shown is named in the panel. |
| **Solo a main band** | The strip under the display is six switches, the main frequency bands of music, each spanning its range on the display's axis: **Sub** (20–60 Hz), **Bass** (60–250 Hz), **Low-Mid** (250–500 Hz), **Midrange** (500 Hz–2 kHz), **Upper-Mid** (2–6 kHz, presence) and **High** (6–20 kHz, air); hover one for what lives there. Click to solo a band, click more to add others, Alt-click to hear one alone, click again to release. They are the same bands as Themis' AUDITION ranges and Iris' regions (defined once in [MusicBands.h](../../shared/dsp/include/pantheon/dsp/MusicBands.h)), so what Themis flags can be heard here over the same part of the spectrum. 4th-order Linkwitz-Riley crossovers with all-pass compensation: a band passes a tone in its middle within 1.68 dB (3.88 dB for the octave-wide Low-Mid), keeps the next band's tone at least 14.0 dB down and the others 48.2 dB; all six together are the plain output to 0.000 dB; releasing it returns the plain output bit for bit (test [12b]). It works on whatever you hear, so it combines with DELTA and a band solo; it fades over 10 ms, is not saved with the session and ends when the editor closes. |
| **Paste** | **PASTE** (top right of the band panel) adds the harmonic-fill bands another plugin of the suite copied (Themis: COPY on a bass item of its master plan), each in the next free slot over the range it measured. Equaliser bands are Iris' and are left out. |
| **Delta / Gain Match / Mix** | **Delta** plays only what the plugin adds (wet − dry). **Gain Match** is a slow (~0.6 s) RMS match, clamped to ±12 dB, for a loudness-matched A/B; it is off by default. **Mix** is latency-compensated: 0 is the input delayed by exactly the reported latency. |
| **Stereo mode** | **Mid/Side** saturates M and S; fill bands then work on Mid only so bass stays centred. |
| **Oversampling** | Off / 2× / 4× / 8×, linear-phase; 4× is the default. Latency is reported to the host and changes with this setting. |
| **Presets** | Nine, including *808 Sub Fill*, *Clean Sub Fill* (the same fill, Clean, with no saturate band), *Missing Fundamental*, *Small-Speaker Bass*, *Master Glue*, *Warm Tube*, *Tape Body* and *Air Exciter* (see [Using it](#using-it)). |

### Why Harmonic Fill is adaptive

A tonal-balance EQ fixes the *average* curve. It cannot fix a mix whose curve looks balanced but whose energy is not: a sub note with nothing above it, a bar where the low mids go empty, a section where they are full. Boosting those regions with an EQ lifts whatever happens to be there, and a fixed-amount harmonic generator adds the same harmonics whether or not they are already present. Harmonic Fill closes the loop instead:

```
band energy        E_b   (the band-passed input, ~100 ms)
energy above band  E_a   (input, from 1.25 x High up to 5.2 x High, same smoothing)
wanted             E_w = E_b x Amount          (Amount in dB, as an energy ratio)
shortfall          s   = 1 - min (1, E_a / E_w)
gain on harmonics  g   = (1 - Adapt) + Adapt x sqrt (s)       (smoothed over ~50 ms)
```

The measurement is made on the *input*, so there is no feedback to destabilise. **Adapt 0 %** is the old fixed-amount behaviour; **Adapt 100 %** adds only the shortfall. The harmonics are generated with exact weights (Chebyshev polynomials of the band signal divided by its own envelope, see below), so a hollow passage gets precisely `Amount` of harmonic energy and a full one gets none. This was measured (see [What has and hasn't been verified](#what-has-and-hasnt-been-verified)): at Amount −6 dB a hollow passage gets **−6.3 dB** of added energy relative to the band, a passage that already has plenty above the band gets **nothing**, and a quarter-filled one gets 0.76 of the full amount where the law predicts 0.80.

What it does *not* do: it judges "already there" by energy in a fixed region above the band, not by whether that energy is musically the right harmonics. Dense material above the band will make it back off. Raise Amount to ask for more energy above the band, lower Adapt to add regardless.

### Why Clean follows one note

A waveshaper makes harmonics of *everything* in its input at once, so with two notes in the band it also makes their sums and differences (the energy between the notes that makes a mix sound dirty), and the amount of each harmonic depends on how the level compares with the shaper's envelope. Measured on a 50 Hz note with 70 Hz beside it (test [5g]): with the second 20 dB quieter, the energy that is not on a multiple of either note is **-9 dB** re the harmonics in Warm and **-25 dB** in Clean; with the two equal, Warm adds **+2.6 dB** more than a lone note would, Clean **-8.4 dB** less. On a 60 Hz note under a 3 Hz tremolo (30 - 100 %) Warm's 2nd harmonic goes from **-2.3 dB** re the fundamental at the crest to **-34 dB** at the trough (69 dB of movement in 50 ms windows, because the envelope is still above the peaks when the level falls and the k-th Chebyshev harmonic scales as a^k); Clean's moves **9.3 dB**, all of it the integrators' lag of about 20 ms.

Clean cannot be made free of that for arbitrary material: a harmonic *of a partial* needs that partial separated from the rest, and a bass that is a chord has nothing to separate. So it follows one note and says when it cannot:

```
PITCH       a bank of resonators over the band (8 per octave) -> the strongest one, interpolated -> the note's frequency
PHASE       one second-order integrator per channel, tuned there -> the note as (v, q) in quadrature
HARMONIC k  amplitude x Re ((v + jq) / |v + jq|)^k       (exact sines, weights as Warm)
CONFIDENCE  the note's energy / the band's energy  ->  fades the fill out between 0.5 and 0.8
```

A frequency-locked loop on its own does not find a note (tried: on two notes it settled on neither), so the pitch is measured by the bank and the integrators only follow the phase from there. A note with its own 2nd harmonic in the band (the fundamental 6 dB above it) has a confidence of about 0.8 and still passes; a second note about as strong as that harmonic is not told from it, and is faded the same way. The integrators are narrow enough not to be carried off by a note 20 dB quieter a minor third away, and that is also why Clean takes about 100 ms to reach full level on a new note (**100 ms** to 90 % on a hit at full level, against 20 ms for Warm). Quicker settings of the pitch bank and the integrators were tried first and let the non-harmonic part rise (by 14 dB at the bottom of a 40-120 Hz band), which is the one thing Clean is for.

### Research this is based on

- **Missing fundamental / virtual bass.** Larsen & Aarts' book *Audio Bandwidth Extension* (Wiley, 2004) frames the nonlinear-device (NLD) approach; Bai & Lin (JAES 2006) introduced phase-vocoder virtual bass; Mu, Gan & Tan (ICASSP 2012) and Hill & Hawksford (2010) built hybrids. The trade-off reported in that literature: **NLDs keep transients but give no control over individual harmonics; phase vocoders give precise harmonic weighting but smear transients.** Overview via [Eloi et al., DAFx 2020](https://acris.aalto.fi/ws/portalfiles/portal/51973173/Eloi2020_Virtual_bass_DAFx2020.pdf).
  → *Hephaestus' answer:* divide the bass band by its own envelope, apply Chebyshev polynomials T2–T5, multiply the envelope back. For a unit sine, T*k* gives exactly the *k*-th harmonic (the weights are exact and level-independent), yet the processing stays in the time domain (no smearing). See [Csound `chebyshevpoly`](https://csound.com/manual/opcodes/chebyshevpoly/) and the [UW lecture notes](https://courses.cs.washington.edu/courses/cse490s/11au/lectures/E-Non-linearSynthesis.pdf).
- **Typical harmonic weighting.** Hobbyist virtual-bass designs use roughly −6/−12/−18/−24 dB for H2–H5 ([DIYAudio thread](https://www.diyaudio.com/community/goto/post?id=3684217)); the defaults here are in that region. Vendor claims for how far below the speaker's limit MaxxBass works vary (1.5–2 octaves), and its algorithm is proprietary, so this is not a MaxxBass clone.
- **Aliasing.** First-order antiderivative anti-aliasing (Parker, Zavalishin & Le Bivic, DAFx 2016; extended in [Werner & Azelborn, DAFx 2023](https://www.dafx.de/paper-archive/2023/DAFx23_paper_61.pdf)) suppresses but does not eliminate aliasing, and misbehaves in single precision at extreme drive ([JUCE forum](https://forum.juce.com/t/anti-derivative-anti-aliasing-adaa-issues/46993)). → ADAA runs in `double`, with a midpoint fallback, *inside* linear-phase oversampling.
- **Host behaviour.** Latency changes must be reported from the message thread ([JUCE forum](https://forum.juce.com/t/bug-setlatencysamples-crashes-ableton-live-10-vst3-fixed/40216)), and the legacy parameter API breaks VST3 automation ([JUCE forum](https://forum.juce.com/t/vst3-automation-with-legacy-parameter-handling-broken/44854)). → latency goes through an `AsyncUpdater`; all parameters use the versioned `ParameterID` API.
- **Even vs odd harmonics.** Sources disagree on which sounds "warmer" ([iZotope](https://www.izotope.com/en/learn/what-is-audio-saturation) vs. a [CreativeLive/Ozone lesson](https://phoenix.creativelive.com/class/mastering-music-with-izotope-ozone-11-tomas-george/lessons/saturation-vintage-tape-module-and-exciter-module)), so Even is a continuous control rather than fixed "tube/tape" types. Trust your ears and Delta.

### Signal flow

```
in -> gain -+- dry (delayed by latency) ------------------------------------------------------+
            +- [M/S enc] -+- fill band x N (base rate, adaptive) -> sum -> delay -----+          |
                          +- saturate band x N: band-pass -> up -> ADAA -> down -> sum +- + -- [M/S dec] -- [gain match] -- mix/delta -- out
                          +- clean copy, delayed by the oversampling latency ----------+
```

Details that matter:

- **A saturate band adds only its distortion.** Each band shapes a band-passed copy of the signal and the *difference* (what the shaper added) is summed onto the untouched, latency-matched input. Nothing the user has not asked for is filtered, so bands can overlap or leave gaps and the plugin is exactly transparent at small signal for any arrangement (0.000 dB, six overlapping bands at 18 dB drive). It replaces a fixed three-band LR4 split, whose bands must sum flat and so cannot move freely.
- **ADAA is applied to the nonlinear residual only** (`f(u)` minus its tangent line at the bias point). Plain first-order ADAA is a `[1 1]/2` low-pass in the linear regime (about -5 dB at 15 kHz at 48 kHz); this form keeps small-signal gain exactly 1 while still anti-aliasing the distortion. The DC blocker acts on the residual only, so bass in the clean path is never phase-shifted.
- **The fundamental engine runs at the host rate**: a fill band's range stops at 2 kHz, so its harmonics stay well below Nyquist (a harmonic that would land above 0.48 x fs is dropped) and there is nothing to alias.
- **The Chebyshev envelope releases slowly (`6 / lo`).** The k-th harmonic scales as `a^k` where `a` is the normalised amplitude, so envelope droop between waveform peaks costs accuracy mostly in H4/H5. Doubling the release from `3/lo` cut the worst-case error from 1.26 dB to 0.38 dB at 60 Hz, with harmonics still 48 dB down 50 ms after a note ends.
- **Clean runs the same path as Warm**: the same band filters, the same band-limiting of what it generates, the same adaptive gain, the same latency alignment (it adds none), the same solo and delta. Only the generator inside the fundamental engine differs ([PartialTracker.h](Source/DSP/PartialTracker.h)). It costs one resonator per eighth of an octave of the band, on one channel (a fill band of 40-120 Hz: 14, a band of 20 Hz-2 kHz: 55) plus three integrators, all at the host rate.
- **Rebuild's gap detector** compares input vs restored energy per frequency in a bank of narrow resonators. Comparing against the whole band mistakes a 2nd harmonic for an existing fundamental, and correlating with the restored signal fails because a difference tone is in phase quadrature with the true fundamental of saw-like waveforms. Both were tried first and failed the test suite.

## How to use

### Build and install (macOS)

Hephaestus Forge is source only: you build it yourself. Formats: **VST3 + AU + Standalone** (macOS, universal arm64 + x86_64).

```bash
# SDKROOT works around Command Line Tools shipping an SDK this Xcode's linker can't read.
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release    # fetches JUCE (version in cmake/Juce.cmake); an offline copy in third_party/JUCE at the repository root is used if present
cmake --build build --target Hephaestus_VST3 Hephaestus_AU Hephaestus_Standalone
```

Building the plugin targets **installs** them to `~/Library/Audio/Plug-Ins/{VST3,Components}` (the VST3 is
`~/Library/Audio/Plug-Ins/VST3/Hephaestus Forge.vst3`). Then rescan plugins in your host; in FL Studio that is *Options → Manage
plugins → Find plugins*. The binary is ad-hoc signed, which is enough for local use. Apple's validator checks the AU:

```bash
auval -v aufx Hphs Hpha
```

Builds are for your own use; if you hand one to somebody else, see [License and terms](#license-and-terms).

### Using it

1. **Put it on the master (or a bus) before the limiter.** Latency (shown by FL as PDC) is reported automatically; it changes
   with the Oversampling setting.
2. **Make bands.** Double-click the spectrum where you want one and choose *Saturate* or *Harmonic Fill*, or use the buttons
   in the band panel. Drag a band's edges to resize it, its body to move it; right-click it to change its mode or remove it.
   The band panel below shows the selected band's controls; the numbered chips pick a band.
3. **Or start from a preset:**
   - **Thin or small-speaker bass:** *Missing Fundamental* or *Small-Speaker Bass*. Put a Harmonic Fill band over the fundamentals you want to support. Watch the FILL % on its tab and confirm with **Delta**.
   - **808 / sub that disappears on laptops:** *808 Sub Fill*.
   - **The same, without the amp-like character:** *Clean Sub Fill*, or press **CLEAN** on a fill band. Use it when the band holds one bass note at a time; if the harmonics fade in and out, the band holds more than one thing and Warm is the better tool.
   - **Glue and warmth:** *Master Glue* / *Warm Tube* / *Tape Body*. Use **Gain Match** so more saturation isn't mistaken for "better".
   - **Dull top end:** *Air Exciter* (a saturate band over the highs; harmonics land above it).
4. **Judge with Delta and with the analyzer.** The display is tilted +4.5 dB/oct by default so typical programme material looks
   flat.

## Development

Built with [JUCE](https://juce.com). All DSP lives in `Source/DSP`, which has no GUI dependencies and is covered by
`Tests/Tests.cpp`; the plugin layer only draws and wires it up. See [CLAUDE.md](CLAUDE.md) for the design invariants and the
single-source-of-truth map before changing anything.

### Tests, screenshots and layout

```bash
cmake --build build --target HephaestusTests && ./build/HephaestusTests_artefacts/Release/HephaestusTests
cmake --build build --target HephaestusUiCheck && ./build/HephaestusUiCheck_artefacts/Release/HephaestusUiCheck
cmake --build build --target HephaestusHostCheck && ./build/HephaestusHostCheck_artefacts/Release/HephaestusHostCheck "$HOME/Library/Audio/Plug-Ins/VST3/Hephaestus Forge.vst3"
cmake --build build --target HephaestusSnapshot && ./build/HephaestusSnapshot_artefacts/Release/HephaestusSnapshot docs/screenshot.png "808 Sub Fill" sub 2   # writes the README image
```

`docs/screenshot.png` is the output of that command, never edited by hand. It is regenerated after every change that touches
the UI or what it shows, and its caption above is kept true to the demo (the rules for contributors are in [CLAUDE.md](CLAUDE.md)).

Layout (the one file map; update it when a source file is added, renamed or removed):

```
Source/DSP/Band.h                band modes, range rules shared by engine and display
Source/DSP/Waveshaper.h          tanh / atan / algebraic curves + residual ADAA
Source/DSP/Saturator.h           user-placed saturate bands, per-band oversampling + shaping
Source/DSP/FundamentalEngine.h   Chebyshev harmonics + adaptive fill + difference-tone rebuild + gap detector
Source/DSP/Engine.h              latency alignment, band mixing, M/S, gain match, delta, meters
Source/Parameters.h              parameter layout, presets, band helpers, legacy-session migration
Source/PluginProcessor.*         JUCE wrapper
Source/PluginEditor.*, UI/       band panel, spectrum view with draggable bands, HephaestusUi.h (band colours); the look itself is shared/ui
../../shared/ui/                 the drawing kit of the interface, shared with the other plugins of the repository
../../shared/dsp/                biquads, Butterworth cascades, the display's lock-free FIFOs and its EQ-style FFT analyzer (GUI-free, unit-tested here), shared with Iris; MusicBands.h (the six main bands and their splitter, the band solo strip), shared with Iris and Themis
Tests/                           DSP tests, UI check, VST3 host check, editor snapshot, shared helpers
docs/screenshot.png              editor render (generated by HephaestusSnapshot)
```

### Continuous integration

CI does not build or test Hephaestus yet; the workflows in [.github/workflows](../../.github/workflows) cover Themis and Iris only (the
state of that is tracked in the repository's [CLAUDE.md](../../CLAUDE.md)). Run the commands above before you rely on a change.

### What has and hasn't been verified

`HephaestusTests` (headless DSP, `Tests/Tests.cpp`) is measured, not assumed, and was run on the final binaries:

| Check | Result |
|---|---|
| Small-signal frequency response, 40 Hz-20 kHz, every oversampling mode | flat within **0.001 dB**; also with six overlapping bands at 18 dB drive |
| Dry path vs reported latency | dry impulse lands **exactly** on the reported latency (49 / 61 / 65 samples at 2x / 4x / 8x); the wet impulse lands on it too |
| Alias energy, 5.3 kHz sine, +18 dB drive (re fundamental) | naive tanh **-18 dB** -> ADAA only **-30 dB** -> with oversampling **-70 to -72 dB** |
| Even control | bias 0: H2 < -160 dB; bias 0.5: H2 -15 dB |
| A saturate band acts only on its range | 100 Hz sine, +24 dB drive: 3rd harmonic **-11 dB** with the band on the tone, **-120 dB** with the band at 2-4 kHz |
| Where the harmonics land (the shaded zone) | band-limited noise through a saturate band at 6 / 12 / 24 dB drive: **0.00 - 0.02 %** of what it adds lies above 5 x High; through a fill band: **0.45 %**, with 81 % of the added energy inside the zone (the rest sits just below it, from intermodulation of a noisy band) |
| Sample rates 22.05 - 192 kHz | small-signal error **3e-8** (-60 dBFS in, input delayed by the reported latency), loud mixed bands finite and bounded |
| Harmonic weights vs targets (-6/-9/-15/-22), 45 / 60 / 90 Hz, two input levels | within **0.6 dB** (0.3 dB mid-range), identical at -6 and -26 dBFS |
| **Clean fill**, 40-120 Hz band, 45 / 60 / 90 Hz, 0.5 and 0.005 amplitude | weights within **0.1 dB** of -6/-9/-15/-22 (Warm: 0.6 dB), identical at both levels; energy not on a multiple of the note **-66 / -58 / -57 dB** (Warm -28 / -30 / -34 dB) |
| Clean fill, a decaying 60 Hz note (30 dB lower at 2 s than at 0.3 s) | 2nd harmonic re fundamental **-5.61 dB** at 0.3 s and at 2 s |
| Clean fill, a note that starts at full level | peak of the first 200 ms **0.594** vs 0.597 settled (no overshoot); **100 ms** to 90 % of its level |
| Clean fill, two notes (50 + 70 Hz) | one 20 dB quieter: energy between the notes **-25 dB** re the harmonics (Warm -9 dB); equal: output **-8.4 dB** re a lone note (Warm +2.6 dB) |
| Clean fill, 60 Hz under a 3 Hz tremolo (30 - 100 %) | 2nd harmonic re the fundamental moves **9.3 dB** over the cycle (Warm **69.0 dB**: -2.3 dB at the crest, -34 dB at the trough) |
| Clean fill, 100 -> 50 Hz glide then held | 2nd harmonic **-6.0 dB** (target -6.0), not on a harmonic **-58 dB**, level during the glide -0.1 dB re held |
| Clean fill, left-only note; 44.1 / 96 / 192 kHz; switching Warm <-> Clean while playing | right channel silent (**-240 dB**); 2nd harmonic **-6.0 dB** at every rate; finite and bounded |
| Harmonic tails | 48 dB down 50 ms after note-off, 95 dB down at 100 ms |
| Tone | the four H2-H5 weights carry unit energy at every tone; dark is H2, bright is led by H3 |
| **Adaptive fill**, 60 Hz band, Amount -6 dB | Adapt 0 %: a hollow and a full passage get the same (+0.6 dB apart). Adapt 100 %: hollow passage **-6.3 dB** re band (target -6.0); plenty above the band: **nothing**; a quarter of the target above the band: **0.76** of the full amount (law: 0.80) |
| Rebuild, 120+180 Hz present, 60 Hz missing | restores 60 Hz at 0.299 (target 0.3); tracks level linearly (-6.0 dB for -6 dB); backs off to **0.000** when the real fundamental exists (0.230 when missing) |
| Mid/Side keeps bass centred | 2nd harmonic of a side-only tone: -5.7 dB in L/R mode, **-93 dB** in M/S mode |
| Mix / Delta | mix = 0 is the input delayed by exactly the latency; delta = wet - dry and mix = 0.37 is the linear blend, all **sample-exact** |
| Switching a band off | after the fade the output is the delayed input, **sample-exact** (max error 0) |
| Gain Match | a -4.9 dB level drop becomes **0.0 dB** |
| Spectrum analyzer | a bin-centred -6.02 dBFS sine reads **-6.02 dB**; 4 kHz with 4.5 dB/oct tilt reads +2.98 dB (expected +2.98); off-bin worst case -7.44 dB (Hann scalloping, < 1.5 dB); left-only signal reads -3 dB (L/R power is averaged) |
| 600 randomised blocks (all params incl. six random bands, block sizes 1-1500, mono/stereo, mode switches) | always finite |
| CPU, 10 s stereo, six bands on (4 saturate, 2 adaptive fill with Rebuild) | 2 % (no OS) -> 15 % of one core (8x), Apple M4 |

| Verified | Not yet verified |
|---|---|
| **`HephaestusUiCheck`** drives the real spectrum component with synthetic mouse events: a band added at 1 kHz takes the first free slot; clicking where bands overlap selects the narrowest; dragging an edge moves only that edge; dragging the body keeps the width ratio; an edge can't cross the minimum width; a fill band is pulled to at most 2 kHz; a full plugin refuses a seventh band and reuses a freed slot | **Inside FL Studio itself.** It was validated through JUCE's VST3 host and `auval`; *pluginval* was not run (the sandbox blocked executing the downloaded binary). Load it in FL and report anything odd |
| **`HephaestusHostCheck`** loads the installed **VST3** through JUCE's host wrapper: 5 sample rates (22.05-192 kHz) x 5 block sizes (1-4096) with every parameter randomised, latency reporting follows the oversampling choice (0 / 49 / 61 / 65 samples) and an impulse through the loaded plugin comes out exactly that many samples later, state and all 9 presets round-trip, and a session saved with the old fixed three-band layout loads as three saturate bands plus one fill band. Apple's `auval` passes for the **AU** | **The mouse interaction by hand in a DAW.** Dragging, the add/remove menus and the band panel are covered by `HephaestusUiCheck` and screenshots, but not by a person using them |
| | Windows and Linux builds. JUCE is cross-platform, but they were not built or tested |

## Limitations

- **The adaptive fill judges "already there" by energy, not by harmonic content.** It compares the energy from 1.25 x High to 5.2 x High with the band's own; anything in that region counts, whether or not it is related to the band. On dense material it will often back off completely: that is the intended behaviour (the energy exists), but raise Amount or lower Adapt if you want it to add regardless. The Chebyshev weights are exact for sine-like band content; noise-like band content gives somewhat different harmonic energy, so the target is approximate there.
- **Clean needs one dominant note.** It follows the strongest note in the band; a band that holds a chord, or a kick and a bass note of similar level, is faded out rather than filled (the confidence gate, 0.5 to 0.8 of the band's energy in the note), and the harmonics of a weaker second note are not made at all. It locks on in about 100 ms, so the first part of a short hit gets less than Warm gives. A note's own harmonics inside the band keep its confidence below 1 (fundamental 6 dB over its 2nd: about 0.8). The pitch bank is only coarse (the sines are locked to the note's phase, so the pitch only has to be close), and the part of the output that is not on a harmonic is -57 to -66 dB on a steady note (test [5g]), not zero.
- **Rebuild is experimental.** A squarer produces difference tones between *all* component pairs in its input band, so dense low-mid material can yield inharmonic bass content. It suits sparse bass material; leave it off for full mixes unless Delta says it helps.
- **A saturate band shapes a band-passed copy** (4th-order Butterworth edges), and what the shaper added is summed onto the clean signal. Near a band's edges, where that copy's phase has turned, the shaper's gain reduction on the fundamental is applied with that phase. This was not measured separately; overlap bands or check with Delta if a band edge sits on something important.
- **Gain Match** is a slow (~0.6 s) RMS match, clamped to ±12 dB. It will partly undo a deliberate bass boost, since low-end energy dominates RMS; that's why it's off by default.
- **The analyzer's absolute level** uses a Hann window, so a sine falling between FFT bins reads up to 1.4 dB low. Its tilt and range options mirror Pro-Q's, but it is not a Pro-Q clone: Pro-Q's smoothing and peak-hold details are not published.
- **Ad-hoc signed, not notarised.** Fine on this Mac. Distributing it needs a Developer ID signature and notarisation.

## License and terms

Hephaestus is free software under the AGPL-3.0-or-later, and is source only. The licence, JUCE and the other third-party
components, the trademark note and what you owe somebody if you hand them a build are in [NOTICE.md](../../NOTICE.md); they are
the same for every plugin of the repository. Suggestions and bug reports are welcome as issues, code contributions are not
accepted ([CONTRIBUTING.md](../../CONTRIBUTING.md)).

- **No warranty.** Hephaestus is provided "as is", without warranty of any kind. It is an audio effect: judge the result with Delta and your ears, and check levels before you export.
- **Passing it on.** macOS quarantines a copy that is not signed with a Developer ID certificate and notarised
  (`xattr -cr "Hephaestus Forge.vst3"` clears it for personal use).
