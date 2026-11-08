# Pantheon Suite

A monorepo of mastering and music plugins named after Greek gods, built with [JUCE](https://juce.com) and licensed
under the AGPL-3.0-or-later. It holds source only: no binaries or downloads.

| Plugin | What it is |
|---|---|
| [Themis Verdict](plugins/themis/README.md) | A mastering check suite: tonal balance, translation, phase, clipping, loudness and a plan of what to do next. |
| [Hephaestus Forge](plugins/hephaestus/README.md) | A harmonic forge for mastering: a saturator that fills in harmonics and missing fundamentals for tonal balance. |
| [Harmonia Pitch](plugins/harmonia/README.md) | Natural-sounding pitch correction for solo vocals. |
| [Iris Prism](plugins/iris/README.md) | A dynamic equaliser: bands on left, right, mid or side, a clean or saturated mode, delta and gain match, solo for bands, curves and frequency regions, and a reference to match another track's shape. |

There is also one standalone app:

| App | What it is |
|---|---|
| [Orpheus Pitch Editor](apps/orpheus/README.md) | An offline pitch editor for solo vocals: the notes of a recording on a piano roll, drag them or correct them to a scale, export the result. |

[shared/ui](shared/ui) holds the hardware-style interface kit (metal faceplates, glass displays, lamps, knobs) that all the
plugins and the app draw with, and [shared/dsp](shared/dsp) the DSP more than one of them uses (biquads, the spectrum analyzer). The rules for working across plugins, and what is not yet in line, are in [CLAUDE.md](CLAUDE.md).

Every knob works the same way in all of them: drag it (up and down or sideways), double-click it to return to its default,
or click its read-out and type a value (Enter applies it, Esc cancels). A number may carry its unit (`250 Hz`, `2.5k`,
`-3 dB`, `0.5 s`); a switch takes the name of a position, or enough of it to tell it apart (`inf` for Infinite).

Each plugin keeps its own README, build commands and
contributor rules in `plugins/<name>/` (the app, in `apps/orpheus/`); this file only lists what is in the repository. CI lives in
[.github/](.github) and runs per plugin.
