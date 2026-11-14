// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "EqMove.h"
#include "Audition.h"
#include "ClipMeter.h"
#include "HistoryChecks.h"
#include "Loudness.h"
#include "LowCrestMeter.h"
#include "PhaseCancellation.h"
#include "Translation.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace themis
{
// Turns the faults the monitoring readouts find into a short list of things to do to the master, worst first: what to
// change, by how much and where, and why. A pure function on the measurements (so it is tested without a UI); it never
// touches the audio. Every number it recommends comes from a measurement; every limit it judges them by is a hand-authored
// rule of thumb (the ones in `translation::`, `history::`, `lowcrest::` and the few below), not measured data.
//
// Only objective faults are listed: clipping, a peak over the ceiling, loudness away from the delivery target, phase
// cancellation, a low end or image that does not survive mono (as a whole and range by range), an unbalanced image, a squashed or spiky low end, a bass
// that small speakers lose. How the spectrum differs from the reference curve (and how it would sound on a device, which
// is measured against that curve too) is a matter of taste and genre, not a fault, so it is shown on the tonal view and
// never turned into a to-do.
namespace master
{
inline constexpr double kMinSeconds = 20.0; // less audio than this and the long-term averages are not worth advice

// Programme loudness against the chosen delivery target, in LU (= dB).
inline constexpr float kLoudnessWarnLu = 2.0f, kLoudnessBadLu = 4.0f;
inline constexpr float kStreamingLufs = -14.0f; // what Spotify, YouTube and Tidal normalise to: a louder master is turned down (Spotify also lifts a quieter one, but only as far as its true peak allows)

} // namespace master

// What an item is about; the order is the order of priority among items of the same severity.
enum class Topic { clipping, loudness, phase, lowEndMono, monoFold, width, lowCrest, balance, bass, swing, dcOffset };

struct Advice
{
    Level level = Level::warn;
    Topic topic = Topic::clipping;
    std::string headline; // what to do, with the numbers
    std::string why;      // what was measured and what it means
    Switches hear;        // the Solo / Mute setting that lets you listen to it (nothing = no suggestion)
    EqMoves moves;        // the band(s) that make the change, where an equaliser or a harmonic generator can (empty = none)
    LimiterMove limiter;  // the ceiling and/or gain change that makes it, where a limiter can (not active = none)

    bool canCopy() const { return !moves.empty() || limiter.active(); } // COPY has something to put on the clipboard
};

struct MasterInputs
{
    double analysedSeconds = 0.0; // audio measured since the reset

    bool haveLoudness = false;
    float integratedLufs = LoudnessMeter::kSilence;
    float targetLufs = master::kStreamingLufs; // the delivery the master is for
    std::string deliveryName = "Streaming";

    ClipTotals clip; // peaks and clips since the reset
    float truePeakCeilingDb = history::clip::kTruePeakWarnDb; // the true-peak ceiling the master is held to (the user's setting)

    TranslationReport translation; // mono fold (the device part is not used: it is judged against the reference curve)

    bool havePhase = false;
    PhaseReport phase;

    bool haveCrest = false;
    float lowCrestDb = 0.0f;

    std::vector<FrameStats> frames; // the history window, oldest first
};

struct MasterAdvice
{
    bool measuring = true;      // not enough audio yet for the long-term checks
    int checks = 0, passed = 0; // checks that had data, and those that found nothing to do
    std::vector<Advice> items;  // worst first
    Level level() const { return items.empty() ? Level::good : items.front().level; }
};

namespace master
{
inline std::string rounded (float db) { return format ("%.1f", static_cast<double> (std::round (db * 2.0f) / 2.0f)); }

// A harmonic-fill band over the bass notes: half an octave either side of the median fundamental, inside the range the
// fundamental tracker looks at.
inline EqMove fillAround (double medianHz)
{
    EqMove m;
    m.kind = EqMove::Kind::harmonicFill;
    m.place = EqMove::Place::mid; // bass stays centred
    m.loHz = static_cast<float> (std::max (fundamental::kMinHz, medianHz / 1.5));
    m.hiHz = static_cast<float> (std::min (fundamental::kMaxHz, medianHz * 1.5));
    m.freqHz = static_cast<float> (medianHz);
    return m;
}
inline constexpr float kMaxBoostDb = 6.0f; // the most a suggested bell asks for; past that the problem is not an EQ move
inline constexpr float kMaxCutDb = 6.0f;   // the most a suggested side cut asks for
inline constexpr float kMinBoostDb = 0.5f; // the least: under it a suggested bell is not worth handing over

inline Advice make (Level level, Topic topic, std::string headline, std::string why, Switches hear = {})
{
    return { level, topic, std::move (headline), std::move (why), hear, {} };
}

// What a check about one of the readouts' ranges (a column of kEnergyRanges) plays: every audition band that overlaps it.
// The readouts keep their own four ranges (their limits were set on them), so this is a superset: the LOW range (up to
// 120 Hz) plays SUB and BASS (up to 250 Hz).
inline uint32_t soloBitsOf (int column) { return auditionBitsOver (kEnergyRanges[column].loHz, kEnergyRanges[column].hiHz); }
// The low end of the checks (the readouts' LOW range): the sub and the bass.
inline Switches soloLow() { return { soloBitsOf (0), 0u }; }
inline Switches soloSideLow() { return { Switches::bit (Source::side) | soloLow().solo, 0u }; }
inline Switches soloSide() { return { Switches::bit (Source::side), 0u }; }
// The side signal of one readout range (a width item: the part of the image a mono sum loses).
inline Switches soloSideOf (int column) { return { Switches::bit (Source::side) | soloBitsOf (column), 0u }; }

// The band that takes `cutDb` off the side signal over an energy range (a column of `kEnergyRanges`): a shelf for the top
// range, a bell across the others, at the geometric centre and as wide as the range (Q from its width in octaves).
inline EqMove sideCutOver (int column, float cutDb)
{
    const auto& r = kEnergyRanges[column];
    EqMove m;
    m.place = EqMove::Place::side;
    m.gainDb = -cutDb;
    if (column == kNumEnergyRanges - 1)
    {
        m.kind = EqMove::Kind::highShelf;
        m.freqHz = static_cast<float> (r.loHz);
        return m;
    }
    const double octaves = std::log2 (r.hiHz / r.loHz), span = std::pow (2.0, octaves);
    m.kind = EqMove::Kind::bell;
    m.freqHz = static_cast<float> (std::sqrt (r.loHz * r.hiHz));
    m.q = static_cast<float> (std::sqrt (span) / (span - 1.0)); // the bell's Q for a bandwidth of that many octaves
    return m;
}
} // namespace master

// The list of things to do.
inline MasterAdvice advise (const MasterInputs& in)
{
    using master::make;
    MasterAdvice out;
    out.measuring = in.analysedSeconds < master::kMinSeconds;
    const auto check = [&] (Level level) {
        ++out.checks;
        if (level == Level::good)
            ++out.passed;
    };

    // ---- clipping and peak ceiling: exact, so it is judged as soon as there is any audio ---------
    if (in.clip.have)
    {
        const Level level = history::clip::level (in.clip.truePeakDb, in.clip.clipEvents, in.clip.overSamples, in.truePeakCeilingDb);
        check (level);
        const float overDb = in.clip.truePeakDb - in.truePeakCeilingDb; // how far above the ceiling
        if (in.clip.clipEvents > 0 || in.clip.overSamples > 0)
            out.items.push_back (make (level, Topic::clipping,
                                       overDb >= 0.05f ? format ("Stop the clipping: ease the clipper or limiter drive and lower the ceiling %.1f dB, to %.1f dBTP", static_cast<double> (overDb),
                                                               static_cast<double> (in.truePeakCeilingDb))
                                                     : std::string ("Stop the clipping: ease the clipper or limiter drive until the flat tops are gone"),
                                       format ("%llu flat-topped clips and %llu samples at 0 dBFS, true peak %+.1f dBTP. Clipping adds distortion that a lossy encode makes worse.",
                                               static_cast<unsigned long long> (in.clip.clipEvents), static_cast<unsigned long long> (in.clip.overSamples),
                                               static_cast<double> (in.clip.truePeakDb))));
        else if (level != Level::good)
            out.items.push_back (make (level, Topic::clipping,
                                       format ("Lower the limiter ceiling by %.1f dB, to %.1f dBTP", static_cast<double> (overDb),
                                               static_cast<double> (in.truePeakCeilingDb)),
                                       format ("True peak is %+.1f dBTP. AAC, MP3 and Ogg encodes overshoot the peaks they are fed, so a master above %.1f dBTP can clip after conversion.",
                                               static_cast<double> (in.clip.truePeakDb), static_cast<double> (in.truePeakCeilingDb))));
        // Aegis (or any limiter that takes the text) can set the ceiling it names; an item about flat tops alone has no ceiling to name.
        if (overDb >= 0.05f && (in.clip.clipEvents > 0 || in.clip.overSamples > 0 || level != Level::good))
            out.items.back().limiter = { true, false, in.truePeakCeilingDb, 0.0f };
    }

    // ---- DC offset: the mean of the samples, once it has been averaged for long enough to be one ----
    if (in.clip.have && in.analysedSeconds >= history::dc::kMinSeconds)
    {
        const float offsetDb = in.clip.dcOffsetDb();
        const Level level = history::dc::level (offsetDb);
        check (level);
        if (level != Level::good)
        {
            const bool left = std::abs (in.clip.dcLeft) >= std::abs (in.clip.dcRight);
            const float worst = left ? in.clip.dcLeft : in.clip.dcRight;
            out.items.push_back (make (level, Topic::dcOffset,
                                       format ("Remove the DC offset: the %s channel sits %.2f %% of full scale %s zero (%.0f dBFS)", left ? "left" : "right",
                                               static_cast<double> (std::abs (worst) * 100.0f), worst > 0.0f ? "above" : "below", static_cast<double> (offsetDb)),
                                       format ("The average of the samples is %+.4f (left) and %+.4f (right) instead of 0. An offset takes headroom from one side of the waveform, "
                                               "clicks where the track starts and ends, and makes a limiter work unevenly. A high-pass filter at 20 Hz or under, or a DC-removal tool, "
                                               "takes it out; find the processor that adds it first.",
                                               static_cast<double> (in.clip.dcLeft), static_cast<double> (in.clip.dcRight))));
        }
    }

    // ---- loudness against the delivery target ------------------------------------------------------
    if (in.haveLoudness && !out.measuring)
    {
        const float deltaLu = in.integratedLufs - in.targetLufs; // positive: louder than the target
        const Level level = translation::classify (std::abs (deltaLu), master::kLoudnessWarnLu, master::kLoudnessBadLu);
        check (level);
        if (level != Level::good)
        {
            const float plr = in.clip.have ? in.clip.truePeakDb - in.integratedLufs : 0.0f;
            const std::string plrText = in.clip.have ? format (" Peak-to-loudness ratio %.1f dB.", static_cast<double> (plr)) : std::string();
            if (deltaLu > 0.0f)
            {
                const bool streaming = std::abs (in.targetLufs - master::kStreamingLufs) <= 0.5f; // the text below is about the -14 LUFS services
                out.items.push_back (make (level, Topic::loudness,
                                           format ("Take about %s dB off the limiter's gain: %.1f LUFS against a %.0f LUFS %s target", master::rounded (deltaLu).c_str(),
                                                   static_cast<double> (in.integratedLufs), static_cast<double> (in.targetLufs), in.deliveryName.c_str()),
                                           std::string (streaming ? format ("Streaming services turn a master louder than %.0f LUFS down, so the extra %.1f dB buys no loudness there and costs punch.",
                                                                            static_cast<double> (master::kStreamingLufs), static_cast<double> (in.integratedLufs - master::kStreamingLufs))
                                                                  : format ("%.1f LU above the target.", static_cast<double> (deltaLu)))
                                               + plrText));
                out.items.back().limiter = { false, true, 0.0f, -deltaLu };
            }
            else
            {
                const float headroomDb = in.clip.have ? in.truePeakCeilingDb - in.clip.truePeakDb : 0.0f; // gain before the true peak reaches the ceiling
                const float raise = -deltaLu;
                std::string why = format ("%.1f LU under the target.", static_cast<double> (raise));
                if (in.clip.have)
                    why += headroomDb >= raise ? format (" The peak (%+.1f dBTP) leaves room to raise it by plain gain.", static_cast<double> (in.clip.truePeakDb))
                                               : format (" Only %.1f dB fits under %.1f dBTP as plain gain; the rest has to come from limiting.", static_cast<double> (std::max (headroomDb, 0.0f)),
                                                         static_cast<double> (in.truePeakCeilingDb));
                out.items.push_back (make (level, Topic::loudness,
                                           format ("Raise the level about %s dB: %.1f LUFS against a %.0f LUFS %s target", master::rounded (raise).c_str(),
                                                   static_cast<double> (in.integratedLufs), static_cast<double> (in.targetLufs), in.deliveryName.c_str()),
                                           why + plrText));
                out.items.back().limiter = { false, true, 0.0f, raise };
            }
        }
    }

    if (out.measuring)
        return out; // everything below is a long-term average

    // ---- stereo: phase, then bass, then the whole spectrum --------------------------------------
    if (in.havePhase && in.phase.have)
    {
        const auto& p = in.phase;
        check (p.level);
        if (p.level != Level::good)
        {
            const std::string where = hzText (p.loHz) + " - " + hzText (p.hiHz);
            std::string headline, why;
            switch (p.cause)
            {
                case PhaseCause::polarity:
                    headline = "Flip the polarity of the inverted channel or source (" + where + ")";
                    why = format ("The channels are in antiphase there; a mono sum loses up to %.1f dB (3 dB is what unrelated channels lose).", static_cast<double> (p.worstLossDb));
                    break;
                case PhaseCause::delay:
                    headline = format ("Remove the %.2f ms offset between the channels (%s)", static_cast<double> (p.delayMs), where.c_str());
                    why = format ("The %s channel arrives late, which is a comb filter in mono: first notch at %s, up to %.1f dB lost. Look for a stereo delay, a widener or a spaced-mic recording.",
                                  p.leftLate ? "left" : "right", hzText (static_cast<double> (p.notchHz)).c_str(), static_cast<double> (p.worstLossDb));
                    break;
                case PhaseCause::none: // (a level above good always has a cause; treated like "other")
                case PhaseCause::other:
                    headline = "Find the stereo effect that cancels " + where + " and reduce it";
                    why = format ("A mono sum loses up to %.1f dB there, and it is not a plain delay or polarity flip: suspect a stereo widener, a phase-shifting effect or an out-of-phase source. Bypass them one at a time.",
                                  static_cast<double> (p.worstLossDb));
                    break;
            }
            out.items.push_back (make (p.level, Topic::phase, headline, why, master::soloSide()));
        }
    }

    if (in.translation.haveFold)
    {
        const auto& t = in.translation;
        check (t.lowMonoLevel);
        if (t.lowMonoLevel != Level::good)
            out.items.push_back (make (t.lowMonoLevel, Topic::lowEndMono,
                                       format ("Make the low end mono: high-pass the side channel around %.0f Hz", translation::kLowEndHz),
                                       format ("A mono sum loses %.1f dB below %.0f Hz. Bass and kick should sit in the middle: club systems and phones sum the low end to mono, and out-of-phase bass cancels there.",
                                              static_cast<double> (t.lowMonoLossDb), translation::kLowEndHz),
                                       master::soloSideLow()));
        if (t.lowMonoLevel != Level::good)
            out.items.back().moves = { { EqMove::Kind::lowCut, EqMove::Place::side, static_cast<float> (translation::kLowEndHz), 0.0f, 0.7071f, 0.0f, 0.0f } };
        check (t.monoLevel);
        if (t.monoLevel != Level::good)
            out.items.push_back (make (t.monoLevel, Topic::monoFold, format ("Narrow the stereo image: a mono sum loses %.1f dB", static_cast<double> (t.monoLossDb)),
                                       format ("Reduce widening on the master or on the widest buses.%s", t.worstBand >= 0
                                                   ? format (" The worst band is %s (%.1f dB lost).", hzText (BandGrid::centreHz (t.worstBand)).c_str(), static_cast<double> (t.worstBandLossDb)).c_str()
                                                   : ""),
                                       master::soloSide()));
    }

    // ---- dynamics of the low end ----------------------------------------------------------------
    if (in.haveCrest)
    {
        const Level level = lowcrest::level (in.lowCrestDb);
        check (level);
        const float outside = lowcrest::outsideZoneDb (in.lowCrestDb);
        if (outside < 0.0f)
            out.items.push_back (make (level, Topic::lowCrest, format ("Open up the low end: ease the compression or clipping on the bass by about %.0f dB", static_cast<double> (std::max (1.0f, -outside))),
                                       format ("The 20 - 250 Hz crest factor is %.1f dB, %.1f dB under the typical %.0f - %.0f dB: a squashed low end sounds flat and loses punch.", static_cast<double> (in.lowCrestDb),
                                               static_cast<double> (-outside), static_cast<double> (lowcrest::kZoneLoDb), static_cast<double> (lowcrest::kZoneHiDb)),
                                       master::soloLow()));
        else if (outside > 0.0f)
            out.items.push_back (make (level, Topic::lowCrest, format ("Tame the low-end peaks: compress or saturate the kick and bass by about %.0f dB", static_cast<double> (std::max (1.0f, outside))),
                                       format ("The 20 - 250 Hz crest factor is %.1f dB, %.1f dB over the typical %.0f - %.0f dB: isolated low peaks make the limiter work hard and pump the whole mix.", static_cast<double> (in.lowCrestDb),
                                               static_cast<double> (outside), static_cast<double> (lowcrest::kZoneLoDb), static_cast<double> (lowcrest::kZoneHiDb)),
                                       master::soloLow()));
    }

    // ---- left / right balance, bass and sections, from the history window ---------------------
    const auto* frames = in.frames.data();
    const int numFrames = static_cast<int> (in.frames.size());
    if (numFrames >= history::kMinFrames)
    {
        const auto balance = history::analyseBalance (frames, numFrames, kAllRanges);
        if (balance.have)
        {
            check (balance.level);
            if (balance.level != Level::good)
            {
                const bool leftLouder = balance.leftMinusRightDb > 0.0f;
                out.items.push_back (make (balance.level, Topic::balance,
                                           format ("Re-centre the image: bring the %s channel down about %.1f dB", leftLouder ? "left" : "right", static_cast<double> (std::abs (balance.leftMinusRightDb))),
                                           format ("%s is %.1f dB louder than %s over the last stretch. Look for a panned element, a one-sided effect or an unbalanced master bus.", leftLouder ? "Left" : "Right",
                                                   static_cast<double> (std::abs (balance.leftMinusRightDb)), leftLouder ? "right" : "left")));
            }
        }

        // A range other than the low end (that has its own item above) that loses too much in a mono sum: the amount is how far
        // its side signal has to come down for the loss to reach the warning limit, from the same window as the MID / SIDE graph.
        for (int column = 1; column < kNumEnergyRanges; ++column)
        {
            const auto w = history::analyseWidth (frames, numFrames, column);
            if (!w.have)
                continue;
            check (w.level);
            if (w.level == Level::good)
                continue;
            const float cut = std::ceil (history::sideCutToMonoOkDb (w, column) * 2.0f) / 2.0f; // to the next half dB, so the cut is enough
            const auto& range = kEnergyRanges[column];
            const std::string where = hzText (range.loHz) + " - " + hzText (range.hiHz);
            out.items.push_back (make (w.level, Topic::width,
                                       format ("Narrow %s (%s): take about %s dB off its side signal", range.name, where.c_str(), master::rounded (cut).c_str()),
                                       format ("A mono sum loses %.1f dB there (side %s against mid; the limit is %.1f dB). Reduce the widening on the elements in this range, or cut its side channel.",
                                               static_cast<double> (w.monoLossDb), signedDbText (w.sideMinusMidDb).c_str(), static_cast<double> (history::monoWarnDb (column))),
                                       master::soloSideOf (column)));
            out.items.back().moves = { master::sideCutOver (column, std::min (master::kMaxCutDb, cut)) };
        }

        const auto bass = history::analyseBass (frames, numFrames);
        if (bass.have)
        {
            check (bass.floorLevel);
            if (bass.floorLevel != Level::good)
                out.items.push_back (make (bass.floorLevel, Topic::bass,
                                           format ("Move the bass up or add harmonics: it sits under %.0f Hz for %.0f %% of the time", history::kSubFloorHz, static_cast<double> (bass.belowFloorShare) * 100.0),
                                           format ("Phones, laptops and small monitors cannot play under %.0f Hz, so a bass line that lives there disappears. Saturate the bass so its harmonics carry the note, "
                                                   "and high-pass the master near 30 Hz.", history::kSubFloorHz),
                                           master::soloLow()));
            if (bass.floorLevel != Level::good)
                out.items.back().moves = { master::fillAround (bass.medianHz) };
            check (bass.balanceLevel);
            if (bass.balanceLevel != Level::good)
            {
                const bool subHeavy = bass.dominant == fundamental::Balance::subHeavy;
                out.items.push_back (make (bass.balanceLevel, Topic::bass,
                                           subHeavy ? std::string ("Add upper harmonics to the bass so it carries on small speakers")
                                                    : format ("Strengthen the bass fundamental around %s", hzText (bass.medianHz).c_str()),
                                           format ("The bass fundamental is %s against its harmonics (median %s, %.0f %% of the time): %s", signedDbText (bass.medianTiltDb).c_str(),
                                                   hzText (bass.medianHz).c_str(), static_cast<double> (bass.share[static_cast<size_t> (bass.dominant)]) * 100.0,
                                                   subHeavy ? "little above it for small speakers to play."
                                                            : "the note is carried by its upper partials; the weight underneath is thin."),
                                           master::soloLow()));
                // Sub-heavy: fill in harmonics. Thin: lift the fundamental by how far it sits under the balanced zone.
                // The class is where it sat most of the time, the lift comes from the median, and the two can disagree (a
                // median inside the balanced zone): then there is no measured lift to hand over, and no move.
                if (subHeavy)
                    out.items.back().moves = { master::fillAround (bass.medianHz) };
                else if (const float lift = fundamental::kHarmonicRichDb - bass.medianTiltDb; lift >= master::kMinBoostDb)
                    out.items.back().moves = { { EqMove::Kind::bell, EqMove::Place::mid, static_cast<float> (bass.medianHz),
                                                 std::min (master::kMaxBoostDb, lift), 1.4f, 0.0f, 0.0f } };
            }
        }

        // Only the low end is held to steady section to section: the highs follow the arrangement.
        const auto low = history::summarise (frames, numFrames, [] (const FrameStats& f) { return f.energy (Signal::stereo, 0); });
        if (low.n >= history::kMinFrames)
        {
            const Level level = translation::classify (low.swingDb(), history::kSwingWarnDb, history::kSwingBadDb) == Level::good ? Level::good : Level::warn;
            check (level);
            if (level != Level::good)
                out.items.push_back (make (level, Topic::swing, format ("Even out the low end between sections: it moves %.0f dB", static_cast<double> (low.swingDb())),
                                           "The level of 20 - 120 Hz differs a lot from moment to moment. If a section is meant to lose its bass that is fine; if not, match the bass level between verse and chorus.",
                                           master::soloLow()));
        }
    }

    std::stable_sort (out.items.begin(), out.items.end(), [] (const Advice& a, const Advice& b) {
        return a.level != b.level ? a.level > b.level : a.topic < b.topic;
    });
    return out;
}
// Before / after: a snapshot of the plan, and what changed since. The user takes one, changes the master, and the
// plan says how many items went away, how many appeared and how far the loudness and the peak moved, so a change is
// judged on the same measurements instead of on memory. Pure functions, nothing is saved with the session.
struct PlanSnapshot
{
    bool have = false;
    std::vector<std::pair<Topic, Level>> items;
    bool haveLoudness = false, havePeak = false;
    float integratedLufs = 0.0f, truePeakDb = 0.0f;
};

inline PlanSnapshot snapshotOf (const MasterInputs& in, const MasterAdvice& a)
{
    PlanSnapshot s;
    s.have = !a.measuring;
    for (const auto& i : a.items)
        s.items.emplace_back (i.topic, i.level);
    s.haveLoudness = in.haveLoudness;
    s.integratedLufs = in.integratedLufs;
    s.havePeak = in.clip.have;
    s.truePeakDb = in.clip.truePeakDb;
    return s;
}

struct PlanChange
{
    int fixed = 0, better = 0, worse = 0, added = 0; // items gone, bad -> warn, warn -> bad, new
    bool haveLoudness = false, havePeak = false;
    float loudnessDelta = 0.0f, truePeakDelta = 0.0f;
};

inline PlanChange compare (const PlanSnapshot& before, const PlanSnapshot& after)
{
    PlanChange c;
    // A topic can carry more than one item (bass: under 40 Hz and the balance), so each item is paired with at most one on the
    // other side: same topic and level first, then same topic. What is left unpaired was fixed (before) or is new (after).
    std::vector<int> pairOf (before.items.size(), -1);
    std::vector<bool> taken (after.items.size(), false);
    for (const bool sameLevel : { true, false })
        for (size_t i = 0; i < before.items.size(); ++i)
            for (size_t j = 0; j < after.items.size() && pairOf[i] < 0; ++j)
                if (!taken[j] && after.items[j].first == before.items[i].first && (!sameLevel || after.items[j].second == before.items[i].second))
                {
                    pairOf[i] = static_cast<int> (j);
                    taken[j] = true;
                }
    for (size_t i = 0; i < before.items.size(); ++i)
    {
        const Level was = before.items[i].second;
        if (pairOf[i] < 0)
        {
            ++c.fixed;
            continue;
        }
        const Level now = after.items[static_cast<size_t> (pairOf[i])].second;
        if (now < was)      ++c.better;
        else if (now > was) ++c.worse;
    }
    for (const bool t : taken)
        if (!t)
            ++c.added;
    c.haveLoudness = before.haveLoudness && after.haveLoudness;
    c.loudnessDelta = after.integratedLufs - before.integratedLufs;
    c.havePeak = before.havePeak && after.havePeak;
    c.truePeakDelta = after.truePeakDb - before.truePeakDb;
    return c;
}

inline std::string describe (const PlanChange& c)
{
    std::string out;
    const auto add = [&] (int n, const char* what) {
        if (n > 0)
            out += (out.empty() ? "" : ", ") + std::to_string (n) + " " + what;
    };
    add (c.fixed, "fixed");
    add (c.better, "better");
    add (c.worse, "worse");
    add (c.added, "new");
    if (out.empty())
        out = "same items";
    if (c.haveLoudness)
        out += format (", %+.1f LU", static_cast<double> (c.loudnessDelta));
    if (c.havePeak)
        out += format (", TP %+.1f dB", static_cast<double> (c.truePeakDelta));
    return out;
}

// The plan as plain text, for the clipboard (a note to yourself, a message to the client, a session log): the target and the
// measured numbers, then every item worst first with its headline and its why. Plain text, one fact per line, no markup.
inline std::string planAsText (const MasterInputs& in, const MasterAdvice& advice)
{
    std::string out = "Themis master plan\n";
    out += format ("Deliver for: %s, %.0f LUFS; true-peak ceiling %.1f dBTP\n", in.deliveryName.c_str(), static_cast<double> (in.targetLufs),
                   static_cast<double> (in.truePeakCeilingDb));
    std::string measured;
    const auto part = [&] (const std::string& text) { measured += (measured.empty() ? "" : ", ") + text; };
    if (in.haveLoudness)
        part (format ("%.1f LUFS integrated", static_cast<double> (in.integratedLufs)));
    if (in.clip.have)
        part (format ("true peak %+.1f dBTP", static_cast<double> (in.clip.truePeakDb)));
    if (in.haveLoudness && in.clip.have)
        part (format ("PLR %.1f dB", static_cast<double> (in.clip.truePeakDb - in.integratedLufs)));
    const int seconds = static_cast<int> (in.analysedSeconds);
    part (format ("%d:%02d analysed", seconds / 60, seconds % 60));
    out += "Measured: " + measured + "\n";

    int bad = 0, warn = 0;
    for (const auto& item : advice.items)
        (item.level == Level::bad ? bad : warn)++;
    if (advice.measuring)
        out += format ("Status: still measuring (the long-term checks need %.0f s of audio; only clipping is judged so far)\n", master::kMinSeconds);
    else if (advice.items.empty())
        out += format ("Status: ready, all %d checks inside their limits\n", advice.checks);
    else
        out += format ("Status: %d to fix, %d to check\n", bad, warn);

    int number = 0;
    for (const auto& item : advice.items)
    {
        out += format ("\n%d. [%s] %s\n", ++number, item.level == Level::bad ? "FIX" : "CHECK", item.headline.c_str());
        if (!item.why.empty())
            out += "   " + item.why + "\n";
    }
    return out;
}
} // namespace themis
