# Security policy

The plugins in this repository (Themis Verdict, Hephaestus Forge, Harmonia Pitch, Iris Prism) are source only: they ship no binaries, have no network
code (JUCE's web browser and curl support are switched off in every build) and no installer. Of them, only Themis
reads files you pick (reference masters for *Learn from tracks…*, `.themiscurve` target files); it decodes them with JUCE's
audio format readers, so a malformed file is the most realistic way to hurt it.

## Reporting a vulnerability

Please report privately through GitHub: **Security → Report a vulnerability** on this repository (private vulnerability
reporting). Please do not open a public issue for a suspected vulnerability.

Include which plugin, what you did, what happened (crash, hang, memory error, unexpected file access) and, if you can, the
file that triggers it. This is a small project maintained in spare time: expect an acknowledgement within about a week, and a
fix or an explanation after that. There is no bug bounty.

## Scope

In scope: crashes, hangs or memory errors triggered by a crafted audio file or `.themiscurve`, and problems in this
repository's build or CI configuration. Out of scope: vulnerabilities in JUCE itself or in a host application (report those
upstream), and the audio-quality limits listed in each README's "Limitations".

## Supported versions

Only the latest release and the latest commit on `main`.

## How the repository is kept safe

The CI and repository settings are described in the Themis README ("Continuous integration"); the rules for contributors are
in [CONTRIBUTING.md](CONTRIBUTING.md).
