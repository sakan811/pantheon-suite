# Pantheon Suite

A monorepo of mastering and music plugins named after Greek gods, built with [JUCE](https://juce.com) and licensed
under the AGPL-3.0-or-later. It holds source only: no binaries or downloads.

| Plugin | What it is |
|---|---|
| [Themis Verdict](plugins/themis/README.md) | A mastering check suite: tonal balance, translation, phase, clipping, loudness and a plan of what to do next. |
| [Hephaestus Forge](plugins/hephaestus/README.md) | A harmonic forge for mastering: a saturator that fills in harmonics and missing fundamentals for tonal balance. |
| [Harmonia Pitch](plugins/harmonia/README.md) | Natural-sounding pitch correction for solo vocals. |
| [Iris Prism](plugins/iris/README.md) | A dynamic equaliser: bands on left, right, mid or side, a clean or saturated mode, delta and gain match, and solo for bands, curves and frequency regions. |

[shared/ui](shared/ui) holds the hardware-style interface kit (metal faceplates, glass displays, lamps, knobs) that all the
plugins draw with, and [shared/dsp](shared/dsp) the DSP more than one of them uses (biquads, the spectrum analyzer). The rules for working across plugins, and what is not yet in line, are in [CLAUDE.md](CLAUDE.md).

Each plugin keeps its own README, build commands and
contributor rules in `plugins/<name>/`; this file only lists what is in the repository. CI lives in
[.github/](.github) and runs per plugin.
