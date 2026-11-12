# Pantheon Suite

A monorepo of mastering and music plugins named after Greek gods, built with [JUCE](https://juce.com) and licensed
under the AGPL-3.0-or-later. It holds source only: no binaries or downloads.

| Plugin | What it is |
|---|---|
| [Themis Verdict](plugins/themis/README.md) | A mastering check suite: tonal balance, translation, phase, clipping, loudness and a plan of what to do next. |
| [Hephaestus Forge](plugins/hephaestus/README.md) | A harmonic forge for mastering: a saturator that fills in harmonics and missing fundamentals for tonal balance. |
| [Harmonia Pitch](plugins/harmonia/README.md) | Natural-sounding pitch correction for solo vocals. |
| [Iris Prism](plugins/iris/README.md) | A dynamic equaliser: bands on left, right, mid or side, a clean or saturated mode, delta and gain match, solo for bands, curves and frequency regions, and a reference to match another track's shape. |
| [Atlas Span](plugins/atlas/README.md) | A six-band compressor on the six main bands of music: each band on stereo, L/R, M/S or one of them, with its own solo, delta and gain match; a zero-latency or linear-phase split, and oversampling. |
| [Hestia Space](plugins/hestia/README.md) | A stem spacer: one instance on each stem, a role and a mix for each; the stems that share a mix give up the bands where a more important one is playing, found through each other with no routing, and each can be put back from the front (depth). A sidechain input brings in any track. |
| [Aegis Ceiling](plugins/aegis/README.md) | A true-peak limiter: input gain, a ceiling in dBTP, release and look-ahead; the peaks between the samples are read at 8x so a converter or a codec after it cannot go over. |
| [Apollo Curve](plugins/apollo/README.md) | An eight-band parametric equaliser: bell, shelves, notch and 12 to 48 dB/oct filters, each on stereo, mid or side; minimum phase or linear phase. |
| [Hera Bond](plugins/hera/README.md) | A compressor: peak or RMS detector, soft knee, auto release and makeup, a parallel mix, a stereo link, and a sidechain with a high-pass on the detector. |
| [Castor Pair](plugins/castor/README.md) | A stereo imager: the width of the sides in each of the six bands of music, mid and side level, a mono bass, a goniometer and a correlation meter. |
| [Nemesis Hush](plugins/nemesis/README.md) | A de-esser: a band detector, split-band or wideband, absolute or relative detection, and a listen switch to find the s. |
| [Dionysus Warmth](plugins/dionysus/README.md) | A saturator: tape, tube, transformer, diode, fold and clip curves, bias, tone, a parallel mix, auto gain and oversampling up to 8x. |
| [Hermes Level](plugins/hermes/README.md) | Gain staging and loudness: trim, polarity, mono / one channel / side checks, BS.1770 loudness and true-peak meters, and a level-matched reference A/B. |

There is also one standalone app:

| App | What it is |
|---|---|
| [Orpheus Pitch Editor](apps/orpheus/README.md) | An offline pitch editor for solo vocals: the notes of a recording on a piano roll, drag them or correct them to a scale, export the result. |

[shared/ui](shared/ui) holds the hardware-style interface kit (metal faceplates, glass displays, lamps, knobs) that all the
plugins and the app draw with, and [shared/dsp](shared/dsp) the DSP more than one of them uses (biquads, the spectrum analyzer, the true-peak reader). The rules for working across plugins, and what is not yet in line, are in [CLAUDE.md](CLAUDE.md).

Every knob works the same way in all of them: drag it (up and down or sideways), double-click it to return to its default,
or click its read-out and type a value (Enter applies it, Esc cancels). A number may carry its unit (`250 Hz`, `2.5k`,
`-3 dB`, `0.5 s`); a switch takes the name of a position, or enough of it to tell it apart (`inf` for Infinite).

Each plugin keeps its own README, build commands and
contributor rules in `plugins/<name>/` (the app, in `apps/orpheus/`); this file only lists what is in the repository. CI lives in
[.github/](.github) and runs per plugin.
