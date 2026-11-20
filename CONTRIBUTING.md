# Contributing

This repository is maintained by one person, who writes and merges all the code. **Suggestions are welcome; code
contributions are not accepted.** That keeps it reviewable by a single maintainer and keeps the principles in
[CLAUDE.md](CLAUDE.md) and in each plugin's CLAUDE.md intact (for Themis: it only measures; its one exception is listening, where AUDITION and COMPARE change the output only while engaged and are not saved;
advice is computed from measurements and never invented; everywhere: one place for every fact and threshold).

## How to suggest something

Open an issue and say which plugin it is about. Bug reports, ideas, a limit that looks wrong on your material, wording that
confused you and questions are all fine. A useful suggestion says:

- **What you saw or want**, and what you expected instead. For a bug: the plugin, the host and version, macOS version, plugin
  format (VST3 / AU) and the steps to reproduce it.
- **For a measurement or a limit:** the audio or the numbers behind it, described rather than attached unless you own the
  rights to the file. The limits are rules of thumb, so a case where one is wrong is exactly the feedback that helps.
- **Why it belongs there:** each plugin's README says what it is for and what it deliberately does not do (for Themis, "Core
  philosophy": it helps you *judge* a master and never *processes* the signal).

Found a vulnerability? Report it privately, see [SECURITY.md](SECURITY.md).

## Pull requests

Pull requests are not merged. You may open one to show an idea, but expect the maintainer to implement it separately, or to
close it with a note. Please do not spend time polishing one.

If you paste code, a curve or any other material into an issue, do it only if you wrote it yourself or it comes with a licence
compatible with the AGPL; do not paste code, fonts, images, presets or curves from other products. Anything you post that
ends up in this repository is used under the same terms as the project, **AGPL-3.0-or-later** (see [LICENSE](LICENSE)).

## For the maintainer

The checks before a change goes in are in [CLAUDE.md](CLAUDE.md) and in the CLAUDE.md of the plugin you touch: its tests pass,
its sources stay warning-free, its README is updated with any change to what it does, a limit or a default, and its README
images are regenerated after a change to the interface (the commands are in its README). A change in `shared/` is checked in
every plugin that uses it. Workflow changes keep actions pinned to a commit SHA and the token permissions read-only.
