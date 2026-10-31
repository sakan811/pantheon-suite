// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

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
// cancellation, a low end or image that does not survive mono, an unbalanced image, a squashed or spiky low end, a bass
// that small speakers lose. How the spectrum differs from the reference curve (and how it would sound on a device, which
// is measured against that curve too) is a matter of taste and genre, not a fault, so it is shown on the tonal view and
// never turned into a to-do.
namespace master
{
inline constexpr double kMinSeconds = 20.0; // less audio than this and the long-term averages are not worth advice

// Programme loudness against the chosen delivery target, in LU (= dB).
inline constexpr float kLoudnessWarnLu = 2.0f, kLoudnessBadLu = 4.0f;
inline constexpr float kStreamingLufs = -14.0f; // what the big streaming services normalise to (they only turn louder masters down)

} // namespace master

// What an item is about; the order is the order of priority among items of the same severity.
enum class Topic { clipping, loudness, phase, lowEndMono, monoFold, lowCrest, balance, bass, swing };

struct Advice
{
    Level level = Level::warn;
    Topic topic = Topic::clipping;
    std::string headline; // what to do, with the numbers
    std::string why;      // what was measured and what it means
    Switches hear;        // the Solo / Mute setting that lets you listen to it (nothing = no suggestion)
};

struct MasterInputs
{
    double analysedSeconds = 0.0; // audio measured since the reset

    bool haveLoudness = false;
    float integratedLufs = LoudnessMeter::kSilence;
    float targetLufs = master::kStreamingLufs; // the delivery the master is for
    std::string deliveryName = "Streaming";

    ClipTotals clip; // peaks and clips since the reset

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

inline Advice make (Level level, Topic topic, std::string headline, std::string why, Switches hear = {})
{
    return { level, topic, std::move (headline), std::move (why), hear };
}

inline Switches soloRange (int range) { return { Switches::bit (static_cast<Source> (kFirstBandSource + range)), 0u }; }
inline Switches soloSideRange (int range) { return { Switches::bit (Source::side) | soloRange (range).solo, 0u }; }
inline Switches soloSide() { return { Switches::bit (Source::side), 0u }; }
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
        const Level level = history::clip::level (in.clip.truePeakDb, in.clip.clipEvents, in.clip.overSamples);
        check (level);
        const float overDb = in.clip.truePeakDb - history::clip::kTruePeakWarnDb; // how far above the ceiling
        if (in.clip.clipEvents > 0 || in.clip.overSamples > 0)
            out.items.push_back (make (level, Topic::clipping,
                                       overDb >= 0.05f ? format ("Stop the clipping: ease the clipper or limiter drive and lower the ceiling %.1f dB, to %.1f dBTP", static_cast<double> (overDb),
                                                               static_cast<double> (history::clip::kTruePeakWarnDb))
                                                     : std::string ("Stop the clipping: ease the clipper or limiter drive until the flat tops are gone"),
                                       format ("%llu flat-topped clips and %llu samples at 0 dBFS, true peak %+.1f dBTP. Clipping adds distortion that a lossy encode makes worse.",
                                               static_cast<unsigned long long> (in.clip.clipEvents), static_cast<unsigned long long> (in.clip.overSamples),
                                               static_cast<double> (in.clip.truePeakDb))));
        else if (level != Level::good)
            out.items.push_back (make (level, Topic::clipping,
                                       format ("Lower the limiter ceiling by %.1f dB, to %.1f dBTP", static_cast<double> (overDb),
                                               static_cast<double> (history::clip::kTruePeakWarnDb)),
                                       format ("True peak is %+.1f dBTP. AAC, MP3 and Ogg encodes overshoot the peaks they are fed, so a master above %.0f dBTP can clip after conversion.",
                                               static_cast<double> (in.clip.truePeakDb), static_cast<double> (history::clip::kTruePeakWarnDb))));
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
                const bool streaming = in.targetLufs <= master::kStreamingLufs + 0.5f;
                out.items.push_back (make (level, Topic::loudness,
                                           format ("Take about %s dB off the limiter's gain: %.1f LUFS against a %.0f LUFS %s target", master::rounded (deltaLu).c_str(),
                                                   static_cast<double> (in.integratedLufs), static_cast<double> (in.targetLufs), in.deliveryName.c_str()),
                                           std::string (streaming ? format ("Streaming services turn a master louder than %.0f LUFS down, so the extra %.1f dB buys no loudness there and costs punch.",
                                                                            static_cast<double> (master::kStreamingLufs), static_cast<double> (in.integratedLufs - master::kStreamingLufs))
                                                                  : format ("%.1f LU above the target.", static_cast<double> (deltaLu)))
                                               + plrText));
            }
            else
            {
                const float headroomDb = in.clip.have ? history::clip::kTruePeakWarnDb - in.clip.truePeakDb : 0.0f; // gain before the true peak reaches the ceiling
                const float raise = -deltaLu;
                std::string why = format ("%.1f LU under the target.", static_cast<double> (raise));
                if (in.clip.have)
                    why += headroomDb >= raise ? format (" The peak (%+.1f dBTP) leaves room to raise it by plain gain.", static_cast<double> (in.clip.truePeakDb))
                                               : format (" Only %.1f dB fits under %.0f dBTP as plain gain; the rest has to come from limiting.", static_cast<double> (std::max (headroomDb, 0.0f)),
                                                         static_cast<double> (history::clip::kTruePeakWarnDb));
                out.items.push_back (make (level, Topic::loudness,
                                           format ("Raise the level about %s dB: %.1f LUFS against a %.0f LUFS %s target", master::rounded (raise).c_str(),
                                                   static_cast<double> (in.integratedLufs), static_cast<double> (in.targetLufs), in.deliveryName.c_str()),
                                           why + plrText));
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
                                       master::soloSideRange (0)));
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
                                       master::soloRange (0)));
        else if (outside > 0.0f)
            out.items.push_back (make (level, Topic::lowCrest, format ("Tame the low-end peaks: compress or saturate the kick and bass by about %.0f dB", static_cast<double> (std::max (1.0f, outside))),
                                       format ("The 20 - 250 Hz crest factor is %.1f dB, %.1f dB over the typical %.0f - %.0f dB: isolated low peaks make the limiter work hard and pump the whole mix.", static_cast<double> (in.lowCrestDb),
                                               static_cast<double> (outside), static_cast<double> (lowcrest::kZoneLoDb), static_cast<double> (lowcrest::kZoneHiDb)),
                                       master::soloRange (0)));
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

        const auto bass = history::analyseBass (frames, numFrames);
        if (bass.have)
        {
            check (bass.floorLevel);
            if (bass.floorLevel != Level::good)
                out.items.push_back (make (bass.floorLevel, Topic::bass,
                                           format ("Move the bass up or add harmonics: it sits under %.0f Hz for %.0f %% of the time", history::kSubFloorHz, static_cast<double> (bass.belowFloorShare) * 100.0),
                                           format ("Phones, laptops and small monitors cannot play under %.0f Hz, so a bass line that lives there disappears. Saturate the bass so its harmonics carry the note, "
                                                   "and high-pass the master near 30 Hz.", history::kSubFloorHz),
                                           master::soloRange (0)));
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
                                           master::soloRange (0)));
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
                                           master::soloRange (0)));
        }
    }

    std::stable_sort (out.items.begin(), out.items.end(), [] (const Advice& a, const Advice& b) {
        return a.level != b.level ? a.level > b.level : a.topic < b.topic;
    });
    return out;
}
} // namespace themis
