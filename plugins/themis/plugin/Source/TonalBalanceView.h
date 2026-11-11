// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "ThemisUi.h"

#include <themis/HistoryChecks.h>
#include <themis/LowCrestMeter.h>

#include <cstring>

// The reference-curve display: long-term spectrum of the signal against the target band.
class TonalBalanceView : public juce::Component, public juce::TooltipClient
{
public:
    explicit TonalBalanceView (ThemisProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::CrosshairCursor); }

    // Pull fresh data from the processor (message thread, ~30 Hz). Repaints only if something changed.
    void update()
    {
        themis::BandArray m {};
        const bool have = proc.readSpectrum (m);
        auto t = proc.activeTarget();
        const float tol = proc.paramReader.value (params::tolerance);
        const double rate = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0;
        float crest = 0.0f;
        const bool haveCrestNow = proc.readLowCrest (crest);
        themis::BandArray f {};
        const bool haveFoldNow = proc.readMonoFold (f);
        const int dev = static_cast<int> (proc.paramReader.value (params::device));
        themis::PhaseBands ph {};
        const bool havePhaseNow = proc.readPhase (ph);
        float hz = 0.0f, tilt = 0.0f;
        bool noHarmonics = false;
        latestFundamental (hz, tilt, noHarmonics);
        themis::ClipTotals clipNow;
        const bool haveClipNow = proc.readClip (clipNow);

        const bool changed = have != haveData || haveClipNow != haveClip || (haveClipNow && clipNow != clipTotals) || havePhaseNow != havePhase || (havePhaseNow && differs (ph, phaseBands)) || differs (hz, fundamentalHz)
                             || differs (tilt, fundamentalTiltDb) || noHarmonics != noFundamentalHarmonics
                             || haveFoldNow != haveFold || (haveFoldNow && differs (f, fold)) || dev != deviceIndex || (have && differs (m, measured)) || haveCrestNow != haveCrest
                             || (haveCrestNow && differs (crest, lowCrestDb)) || t.name != target.name
                             || differs (t.medianDb, target.medianDb) || differs (t.halfWidthDb, target.halfWidthDb)
                             || differs (tol, tolerance) || differs (rate, fs);
        if (!changed)
            return;
        haveData = have;
        haveClip = haveClipNow;
        if (haveClipNow)
            clipTotals = clipNow;
        haveFold = haveFoldNow;
        deviceIndex = dev;
        haveCrest = haveCrestNow;
        if (haveCrestNow)
            lowCrestDb = crest;
        if (have)
            measured = m;
        if (haveFoldNow)
            fold = f;
        havePhase = havePhaseNow;
        if (havePhaseNow)
            phaseBands = ph;
        fundamentalHz = hz;
        fundamentalTiltDb = tilt;
        noFundamentalHarmonics = noHarmonics;
        target = std::move (t);
        tolerance = tol;
        fs = rate;

        device = proc.activeDevice();
        deviceResponse = device != nullptr ? device->responseDb() : themis::BandArray {};
        if (device != nullptr)
            device->usableRangeHz (themis::translation::kRangeFloorDb, deviceLoHz, deviceHiHz);
        report = haveData ? themis::analyseTranslation (measured, haveFold ? &fold : nullptr, target, device) : themis::TranslationReport {};
        phase = havePhase ? themis::analysePhase (phaseBands) : themis::PhaseReport {};
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        ui::hw::drawModule (g, area);

        auto strips = area.removeFromBottom (2.0f * kStripHeight);
        broadbandStrip = strips.removeFromTop (kStripHeight).reduced (8.0f, 4.0f);
        const auto strip = broadbandStrip;
        translationStrip = strips.reduced (8.0f, 4.0f);
        screen = area.reduced (10.0f, 10.0f);
        plot = screen.reduced (38.0f, 10.0f).withTrimmedBottom (14.0f);

        ui::hw::drawGlass (g, screen);
        drawGrid (g);
        drawDeviceRange (g);
        drawTarget (g);
        if (haveData)
        {
            drawMeasured (g);
            drawMeasuredThroughDevice (g);
            drawMonoFold (g);
        }
        else
        {
            g.setColour (colours::dim);
            g.setFont (ui::font (15.0f));
            g.drawText ("Play your track - the curve builds up as audio flows through", plot.toNearestInt(), juce::Justification::centred);
        }
        drawPhaseRegion (g);
        ui::hw::drawGlassOverlay (g, screen);
        drawBroadband (g, strip);
        drawTranslation (g, translationStrip);
        drawHover (g);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        hoverBand = plot.contains (e.position) ? bandForX (e.position.x) : -1;
        overTranslationStrip = translationStrip.contains (e.position);
        overBroadbandStrip = broadbandStrip.contains (e.position);
        repaint();
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        hoverBand = -1;
        overTranslationStrip = false;
        overBroadbandStrip = false;
        repaint();
    }

    // Tooltip over the translation readouts: the numbers behind the bars.
    juce::String getTooltip() override
    {
        if (overBroadbandStrip && !overTranslationStrip)
            return fundamentalHz > 0.0f ? "FUNDAMENTAL: the lowest tonal bass peak (" + ui::hzText (fundamentalHz) + ") is "
                                              + (noFundamentalHarmonics ? juce::String ("the only tonal component found: no harmonics. ")
                                                                        : ui::signedDb (fundamentalTiltDb) + " against its harmonics. ")
                                              + "Sub-heavy bass has little above it for small speakers to play; harmonic-rich bass is carried by its upper partials. "
                                                "See the FUNDAMENTAL graph for how it moves."
                                        : juce::String();
        if (!overTranslationStrip)
            return {};
        juce::StringArray lines;
        if (report.haveDevice && device != nullptr)
            lines.add (juce::String (device->name) + ": the track changes by " + ui::signedDb (report.deviceChangeDb) + " overall, the target by "
                       + ui::signedDb (report.referenceChangeDb) + ". Positive DEVICE LOSS means this track depends more on what the device cannot "
                         "reproduce than the target does.");
        if (report.haveFold)
            lines.add ("MONO FOLD is the power lost when the channels are summed (0 dB = mono-safe, 3 dB = uncorrelated, more = phase "
                       "cancellation). The coloured strip under the curve shows it per band.");
        if (phase.have)
            lines.add (phaseTooltip());
        if (haveClip)
            lines.add (clipTooltip());
        lines.add ("Device responses are generic approximations, and the limits are rules of thumb; neither is measured data. "
                   "Nothing here changes the audio.");
        return lines.joinIntoString ("\n");
    }

private:
    // Bitwise comparison: only used to decide whether a repaint is needed.
    template <typename T>
    static bool differs (const T& a, const T& b) { return std::memcmp (&a, &b, sizeof (T)) != 0; }

    static constexpr float kTopDb = 12.0f, kBottomDb = -30.0f;
    static constexpr float kStripHeight = ui::MeterCell::kStripHeight;
    static constexpr float kFoldStripHeight = 5.0f; // the mono-fold strip along the bottom of the plot (and the phase region outline around it)

    // Display slope of 4.5 dB/oct, a common choice for spectrum analysers. The analysis is constant-Q
    // band energy, where pink noise (-3 dB/oct power density) is flat, so 4.5 dB/oct reads as
    // +1.5 dB/oct on this grid. Applied identically to signal and target, pivoting at 1 kHz; deltas and
    // colours are unaffected.
    static constexpr float kDisplaySlopeDbPerOct = 4.5f, kPinkSlopeDbPerOct = 3.0f;
    static constexpr float kTiltDbPerOct = kDisplaySlopeDbPerOct - kPinkSlopeDbPerOct;
    static float tiltDb (int band)
    {
        return kTiltDbPerOct * static_cast<float> (std::log2 (themis::BandGrid::centreHz (band) / 1000.0));
    }
    static constexpr int kBandCount = themis::BandGrid::kNum;

    // The plot spans [kF0, kMaxHz] on a log axis.
    static double logSpan() { return std::log (themis::BandGrid::kMaxHz / themis::BandGrid::kF0); }
    float xFor (double hz) const
    {
        return plot.getX() + plot.getWidth() * static_cast<float> (std::log (hz / themis::BandGrid::kF0) / logSpan());
    }
    float yFor (float db) const
    {
        return plot.getY() + plot.getHeight() * (kTopDb - juce::jlimit (kBottomDb - 6.0f, kTopDb + 6.0f, db)) / (kTopDb - kBottomDb);
    }
    int bandForX (float x) const
    {
        const double hz = themis::BandGrid::kF0 * std::exp ((x - plot.getX()) / plot.getWidth() * logSpan());
        return juce::jlimit (0, kBandCount - 1, static_cast<int> (std::lround (themis::BandGrid::indexForHz (hz))));
    }
    int lastDrawableBand() const
    {
        int last = kBandCount - 1;
        while (last > 0 && themis::BandGrid::centreHz (last) > themis::BandGrid::usableMaxHz (fs))
            --last;
        return last;
    }

    // Appends the curve `dbAt (band)` (plus the display tilt) to `p`, low to high frequency or reversed; a
    // reversed trace continues the path it is appended to.
    template <typename DbAt>
    void traceCurve (juce::Path& p, DbAt dbAt, bool reverse = false) const
    {
        const int last = lastDrawableBand();
        for (int n = 0; n <= last; ++n)
        {
            const int i = reverse ? last - n : n;
            const float x = xFor (themis::BandGrid::centreHz (i)), y = yFor (dbAt (static_cast<size_t> (i)) + tiltDb (i));
            n == 0 && !reverse ? p.startNewSubPath (x, y) : p.lineTo (x, y);
        }
    }

    // A row of text along the top of the plot: left-aligned labels or a right-aligned readout.
    juce::Rectangle<int> annotationRow (int row, bool alignRight) const
    {
        const int y = static_cast<int> (plot.getY()) + 2 + row * 16;
        return alignRight ? juce::Rectangle<int> (static_cast<int> (plot.getRight()) - 520, y, 516, 16)
                          : juce::Rectangle<int> (static_cast<int> (plot.getX()) + 6, y, 320, 16);
    }

    void drawGrid (juce::Graphics& g) const
    {
        g.setFont (ui::font (11.0f));
        for (double hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            const float x = xFor (hz);
            g.setColour (colours::glassGrid);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            g.setColour (colours::dim);
            const juce::String label = hz >= 1000.0 ? juce::String (static_cast<int> (hz / 1000.0)) + "k" : juce::String (static_cast<int> (hz));
            g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 13.0f).toNearestInt(),
                        juce::Justification::centred);
        }
        for (float db = kTopDb; db >= kBottomDb - 0.1f; db -= 6.0f)
        {
            const float y = yFor (db);
            g.setColour (std::abs (db) < 0.01f ? colours::dim.withAlpha (0.5f) : colours::glassGrid);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (colours::dim);
            g.drawText (juce::String (static_cast<int> (db)), juce::Rectangle<float> (plot.getX() - 36.0f, y - 7.0f, 32.0f, 14.0f).toNearestInt(),
                        juce::Justification::centredRight);
        }
    }

    void drawTarget (juce::Graphics& g) const
    {
        juce::Path band, median;
        traceCurve (band, [&] (size_t s) { return target.medianDb[s] + target.halfWidthDb[s] * tolerance; });
        traceCurve (band, [&] (size_t s) { return target.medianDb[s] - target.halfWidthDb[s] * tolerance; }, true);
        band.closeSubPath();
        g.setColour (colours::accent.withAlpha (0.16f));
        g.fillPath (band);
        g.setColour (colours::accent.withAlpha (0.45f));
        g.strokePath (band, juce::PathStrokeType (1.0f));

        traceCurve (median, [&] (size_t s) { return target.medianDb[s]; });
        g.setColour (colours::accent.withAlpha (0.14f));
        g.strokePath (median, juce::PathStrokeType (5.0f));
        g.setColour (colours::accent.withAlpha (0.9f));
        g.strokePath (median, juce::PathStrokeType (1.5f));

        g.setColour (colours::accent);
        g.setFont (ui::font (12.0f));
        g.drawText ("Target: " + juce::String (target.name), annotationRow (0, false), juce::Justification::left);
    }

    void drawMeasured (juce::Graphics& g) const
    {
        const int last = lastDrawableBand();
        auto sm = measured; // light display smoothing (3-band moving average)
        themis::smoothBands (sm, 1);
        for (int i = 0; i < last; ++i)
        {
            const auto s = static_cast<size_t> (i);
            const auto c = colours::forLevel (themis::translation::toleranceLevel (sm[s] - target.medianDb[s], target.halfWidthDb[s] * tolerance));
            const juce::Line<float> segment (xFor (themis::BandGrid::centreHz (i)), yFor (sm[s] + tiltDb (i)), xFor (themis::BandGrid::centreHz (i + 1)),
                                             yFor (sm[s + 1] + tiltDb (i + 1)));
            g.setColour (c.withAlpha (0.16f)); // phosphor glow
            g.drawLine (segment, 7.0f);
            g.setColour (c);
            g.drawLine (segment, 2.2f);
        }
    }

    void drawBroadband (juce::Graphics& g, juce::Rectangle<float> strip) const
    {
        const int last = lastDrawableBand();
        drawLowCrest (g, ui::MeterCell::inStrip (strip, 4)); // four tonal bands + the low-end crest meter + the fundamental
        drawFundamental (g, ui::MeterCell::inStrip (strip, 5));
        for (int r = 0; r < themis::kNumEnergyRanges; ++r)
        {
            const auto cell = ui::MeterCell::inStrip (strip, r);
            cell.drawTitle (g, themis::kEnergyRanges[r].name);

            double delta = 0, tol = 0;
            int n = 0;
            for (int i = 0; i <= last; ++i)
            {
                if (themis::kEnergyRanges[r].contains (i))
                {
                    const auto s = static_cast<size_t> (i);
                    delta += measured[s] - target.medianDb[s];
                    tol += target.halfWidthDb[s] * tolerance;
                    ++n;
                }
            }
            if (n == 0 || !haveData)
            {
                cell.drawEmpty (g);
                continue;
            }
            delta /= n;
            tol /= n;

            const auto c = colours::forLevel (themis::translation::toleranceLevel (static_cast<float> (delta), static_cast<float> (tol)));
            const auto bar = cell.drawTrack (g);
            const float centre = bar.getCentreX();
            const float half = bar.getWidth() * 0.5f;
            const float pos = static_cast<float> (juce::jlimit (-1.0, 1.0, delta / (tol * 3.0))) * half;
            ui::MeterCell::fillSpan (g, bar, centre - half / 3.0f, centre + half / 3.0f, colours::accent.withAlpha (0.35f)); // in-tolerance zone
            ui::MeterCell::drawMarker (g, bar, centre + pos, c);
            cell.drawReadout (g, ui::signedDb (static_cast<float> (delta)), c);
        }
    }

    // Crest factor (peak/RMS) of the 20-250 Hz band: a
    // low-end that is too dynamic sits to the left, one that is too compressed to the right. The
    // "typical" zone is a hand-authored guess, not measured data.
    void drawLowCrest (juce::Graphics& g, const ui::MeterCell& cell) const
    {
        constexpr float kDynamicDb = 15.0f, kCompressedDb = 3.0f; // scale ends (left, right)
        using themis::lowcrest::kZoneHiDb;
        using themis::lowcrest::kZoneLoDb;

        cell.drawTitle (g, "LOW CREST");
        if (!haveCrest)
        {
            cell.drawEmpty (g);
            return;
        }

        const auto bar = cell.drawTrack (g);
        const auto xForCrest = [&] (float db) {
            return bar.getX() + bar.getWidth() * (kDynamicDb - juce::jlimit (kCompressedDb, kDynamicDb, db)) / (kDynamicDb - kCompressedDb);
        };
        ui::MeterCell::fillSpan (g, bar, xForCrest (kZoneHiDb), xForCrest (kZoneLoDb), colours::accent.withAlpha (0.35f)); // typical zone

        const auto c = colours::forLevel (themis::lowcrest::level (lowCrestDb));
        ui::MeterCell::drawMarker (g, bar, xForCrest (lowCrestDb), c);

        const juce::String note = lowCrestDb > kZoneHiDb ? "  dynamic" : lowCrestDb < kZoneLoDb ? "  compressed" : "";
        cell.drawReadout (g, juce::String (lowCrestDb, 1) + " dB" + note, c);
    }

    // Darkens the frequencies the selected device cannot reproduce (more than kRangeFloorDb below its passband).
    void drawDeviceRange (juce::Graphics& g) const
    {
        if (device == nullptr)
            return;
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        const float loX = xFor (deviceLoHz), hiX = xFor (deviceHiHz);
        if (loX > plot.getX() + 1.0f)
            g.fillRect (plot.getX(), plot.getY(), loX - plot.getX(), plot.getHeight());
        if (hiX < plot.getRight() - 1.0f)
            g.fillRect (hiX, plot.getY(), plot.getRight() - hiX, plot.getHeight());
    }

    // The measured curve as the selected device would reproduce it (measured + the device's response).
    void drawMeasuredThroughDevice (juce::Graphics& g) const
    {
        if (device == nullptr)
            return;
        juce::Path p;
        traceCurve (p, [&] (size_t s) { return measured[s] + deviceResponse[s]; });
        juce::Path dashed;
        const float dashes[] = { 5.0f, 4.0f };
        juce::PathStrokeType (1.4f).createDashedStroke (dashed, p, dashes, 2);
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt()); // a roll-off runs far below the plotted range
        g.setColour (colours::text.withAlpha (0.85f));
        g.fillPath (dashed);
        g.restoreState();
        g.setFont (ui::font (12.0f));
        g.drawText ("Via " + juce::String (device->name) + "  (plays " + ui::hzText (deviceLoHz) + " - " + ui::hzText (deviceHiHz) + ")", annotationRow (1, false),
                    juce::Justification::left);
    }

    // A thin strip along the bottom of the plot: how much of each band's power would be lost in a mono sum.
    void drawMonoFold (juce::Graphics& g) const
    {
        if (!haveFold)
            return;
        for (int i = 0; i <= lastDrawableBand(); ++i)
        {
            const auto s = static_cast<size_t> (i);
            if (measured[s] < themis::translation::kWorstBandFloorDb) // too little energy for the verdict to matter
                continue;
            const float loss = -fold[s];
            g.setColour (colours::forLevel (themis::translation::bandMonoLevel (i, loss)).withAlpha (0.9f));
            const float x0 = xFor (themis::BandGrid::lowEdgeHz (i)), x1 = xFor (themis::BandGrid::highEdgeHz (i));
            g.fillRect (std::max (x0, plot.getX()), plot.getBottom() - kFoldStripHeight, std::min (x1, plot.getRight()) - std::max (x0, plot.getX()), kFoldStripHeight);
        }
    }

    // One-sided meter: 0 on the left, the marker moves right as `value` grows past the warn and bad limits.
    static void drawMeter (juce::Graphics& g, const ui::MeterCell& cell, const juce::String& title, float value, float warnAt, float badAt,
                           themis::Level level, const juce::String& text)
    {
        cell.drawTitle (g, title);
        const auto bar = cell.drawTrack (g);
        const float scale = badAt * 1.5f;
        const auto xAt = [&] (float v) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, v / scale); };
        ui::MeterCell::fillSpan (g, bar, bar.getX(), xAt (warnAt), colours::good.withAlpha (0.25f));
        ui::MeterCell::fillSpan (g, bar, xAt (warnAt), xAt (badAt), colours::warn.withAlpha (0.25f));
        ui::MeterCell::fillSpan (g, bar, xAt (badAt), bar.getRight(), colours::bad.withAlpha (0.25f));
        ui::MeterCell::drawMarker (g, bar, xAt (value), colours::forLevel (level));
        cell.drawReadout (g, text, colours::forLevel (level));
    }

    // Second readout row: how the balance is likely to translate (device loss vs the target, mono compatibility).
    void drawTranslation (juce::Graphics& g, juce::Rectangle<float> strip) const
    {
        namespace tr = themis::translation;
        const auto cell = [&] (int i) { return ui::MeterCell::inStrip (strip, i); };
        // The phase check does not need the tonal curve: a fully out-of-phase signal has no mid to build one from.
        if (havePhase)
            drawPhase (g, cell (4));
        else
            cell (4).drawNote (g, "PHASE CHECK", "measuring...", colours::dim);
        drawClip (g, cell (5));
        if (!haveData)
        {
            for (const auto& [i, title] : { std::pair<int, const char*> { 0, "DEVICE LOSS" }, { 1, "MONO FOLD" }, { 2, "LOW-END MONO" }, { 3, "WORST BAND" } })
            {
                cell (i).drawTitle (g, title);
                cell (i).drawEmpty (g);
            }
            return;
        }

        if (report.haveDevice)
            drawMeter (g, cell (0), "DEVICE LOSS", report.extraLossDb, tr::kDeviceWarnDb, tr::kDeviceBadDb, report.deviceLevel,
                       ui::signedDb (report.extraLossDb) + " vs target");
        else
            cell (0).drawNote (g, "DEVICE LOSS", "choose a device", colours::dim);

        if (report.haveFold)
        {
            drawMeter (g, cell (1), "MONO FOLD", report.monoLossDb, tr::kMonoWarnDb, tr::kMonoBadDb, report.monoLevel,
                       "-" + juce::String (report.monoLossDb, 1) + " dB");
            drawMeter (g, cell (2), "LOW-END MONO", report.lowMonoLossDb, tr::kLowMonoWarnDb, tr::kLowMonoBadDb, report.lowMonoLevel,
                       "-" + juce::String (report.lowMonoLossDb, 1) + " dB < " + juce::String (juce::roundToInt (tr::kLowEndHz)) + " Hz");
            if (report.worstBand >= 0)
                cell (3).drawNote (g, "WORST BAND",
                                   ui::hzText (themis::BandGrid::centreHz (report.worstBand)) + "  -" + juce::String (report.worstBandLossDb, 1) + " dB",
                                   colours::forLevel (tr::bandMonoLevel (report.worstBand, report.worstBandLossDb)));
            else
                cell (3).drawNote (g, "WORST BAND", "-", colours::dim);
        }
        else
        {
            cell (1).drawNote (g, "MONO FOLD", "measuring...", colours::dim);
        }
    }

    // Peaks and clipping since the last reset: a true-peak meter whose zones are the verdict's limits, and the clip count.
    // The PEAKS tab of the LEVEL graph shows when it happened.
    void drawClip (juce::Graphics& g, const ui::MeterCell& cell) const
    {
        namespace clip = themis::history::clip;
        if (!haveClip)
        {
            cell.drawNote (g, "CLIPPING", "measuring...", colours::dim);
            return;
        }
        constexpr float kLowDb = -9.0f, kHighDb = 3.0f; // scale ends
        cell.drawTitle (g, "CLIPPING");
        const auto bar = cell.drawTrack (g);
        const auto xAt = [&] (float db) { return bar.getX() + bar.getWidth() * (juce::jlimit (kLowDb, kHighDb, db) - kLowDb) / (kHighDb - kLowDb); };
        ui::MeterCell::fillSpan (g, bar, bar.getX(), xAt (clip::kTruePeakWarnDb), colours::good.withAlpha (0.25f));
        ui::MeterCell::fillSpan (g, bar, xAt (clip::kTruePeakWarnDb), xAt (clip::kTruePeakBadDb), colours::warn.withAlpha (0.25f));
        ui::MeterCell::fillSpan (g, bar, xAt (clip::kTruePeakBadDb), bar.getRight(), colours::bad.withAlpha (0.25f));
        const auto level = clip::level (clipTotals.truePeakDb, clipTotals.clipEvents, clipTotals.overSamples);
        const auto c = colours::forLevel (level);
        ui::MeterCell::drawMarker (g, bar, xAt (clipTotals.truePeakDb), c);
        const juce::String count = clipTotals.clipEvents >= 1000 ? juce::String (static_cast<double> (clipTotals.clipEvents) / 1000.0, 1) + "k" : juce::String (static_cast<juce::int64> (clipTotals.clipEvents));
        cell.drawReadout (g, juce::String (clipTotals.truePeakDb, 1) + " dBTP  "
                                 + (clipTotals.clipEvents > 0 ? count + " clips" : clipTotals.overSamples > 0 ? juce::String ("over 0 dB") : juce::String ("clean")),
                          c);
    }

    juce::String clipTooltip() const
    {
        namespace clip = themis::history::clip;
        juce::String text = "CLIPPING since the last reset: sample peak " + juce::String (clipTotals.samplePeakDb, 1) + " dBFS, true peak (the waveform between the samples, 4x oversampled) "
                            + juce::String (clipTotals.truePeakDb, 1) + " dBTP. "
                            + juce::String (static_cast<juce::int64> (clipTotals.clipEvents)) + " flat-topped clips (" + juce::String (static_cast<juce::int64> (clipTotals.clipSamples))
                            + " samples) and " + juce::String (static_cast<juce::int64> (clipTotals.overSamples)) + " samples at or over 0 dBFS. ";
        text += "DC offset (the average of the samples, which should be zero): " + juce::String (clipTotals.dcOffsetDb(), 0) + " dBFS"
                + (themis::history::dc::level (clipTotals.dcOffsetDb()) == themis::Level::good ? " (none worth removing). " : ", worth removing: see the master plan. ");
        text += "Any clip or over is bad, a true peak above " + juce::String (clip::kTruePeakWarnDb, 0) + " dBTP is a warning (lossy encoding can push it over) and above "
                + juce::String (clip::kTruePeakBadDb, 0) + " dBTP bad: rules of thumb. A synthesised square wave or a held level also looks like a flat top. "
                  "The PEAKS tab of the LEVEL graph shows when it happened.";
        return text;
    }

    // Outlines the widest cancelling region on the mono-fold strip.
    void drawPhaseRegion (juce::Graphics& g) const
    {
        if (!phase.have || phase.cause == themis::PhaseCause::none)
            return;
        const float x0 = std::max (xFor (phase.loHz), plot.getX()), x1 = std::min (xFor (phase.hiHz), plot.getRight());
        g.setColour (colours::text.withAlpha (0.9f));
        g.drawRect (x0, plot.getBottom() - kFoldStripHeight - 1.0f, x1 - x0, kFoldStripHeight + 2.0f, 1.0f);
    }

    static juce::String phaseVerdict (const themis::PhaseReport& p)
    {
        switch (p.cause)
        {
            case themis::PhaseCause::none: return "OK";
            case themis::PhaseCause::polarity: return "Polarity flipped";
            case themis::PhaseCause::delay: return "Delay " + juce::String (p.delayMs, 2) + " ms";
            case themis::PhaseCause::other: break;
        }
        return ui::hzText (p.loHz) + " - " + ui::hzText (p.hiHz);
    }

    // Does a mono sum lose more than uncorrelated channels would, and why.
    void drawPhase (juce::Graphics& g, const ui::MeterCell& cell) const
    {
        cell.drawNote (g, "PHASE CHECK", phaseVerdict (phase), colours::forLevel (phase.level));
    }

    juce::String phaseTooltip() const
    {
        const auto where = ui::hzText (phase.loHz) + " - " + ui::hzText (phase.hiHz);
        const auto depth = juce::String (phase.flaggedBands) + (phase.flaggedBands == 1 ? " band" : " bands") + " lose up to "
                           + juce::String (phase.worstLossDb, 1) + " dB in a mono sum";
        switch (phase.cause)
        {
            case themis::PhaseCause::none:
                return "PHASE CHECK: no band loses more in a mono sum than uncorrelated channels would (3 dB), so nothing is cancelling.";
            case themis::PhaseCause::polarity:
                return "PHASE CHECK: the channels are in opposite polarity" + (phase.delayMs > 0.0f ? " (and offset by " + juce::String (phase.delayMs, 2) + " ms)" : juce::String())
                       + ". " + depth + "; the outlined region (" + where + ") is the widest. Check a polarity / phase-invert switch.";
            case themis::PhaseCause::delay:
                return "PHASE CHECK: one channel (" + juce::String (phase.leftLate ? "left" : "right") + ") is about " + juce::String (phase.delayMs, 2)
                       + " ms late, which makes a comb filter with the first notch near " + ui::hzText (phase.notchHz) + " in mono. " + depth
                       + "; widest region " + where + ".";
            case themis::PhaseCause::other:
                break;
        }
        return "PHASE CHECK: " + depth + "; the widest region is " + where
               + ". The phase between the channels is not a plain delay or polarity flip: look for stereo wideners, phase-shifting effects "
                 "or a source that is panned with a time offset.";
    }

    // The newest tonal bass in the last ~1 s of the history: its frequency and how far its fundamental sits above its harmonics.
    void latestFundamental (float& hz, float& tiltDb, bool& noHarmonics) const
    {
        std::array<themis::FrameStats, 12> recent {};
        const int n = proc.frameHistory().copyRecent (recent.data(), static_cast<int> (recent.size()));
        for (int i = n - 1; i >= 0; --i)
        {
            const auto& f = recent[static_cast<size_t> (i)];
            if (!f.hasFundamental())
                continue;
            hz = f.f0Hz;
            noHarmonics = !themis::hasLevel (f.harmonicsDb);
            tiltDb = noHarmonics ? 0.0f : f.fundamentalDb - f.harmonicsDb;
            return;
        }
        noHarmonics = false;
    }

    // Where the bass fundamental sits against its harmonics (hand-authored labels, see themis::fundamental::balanceOf).
    void drawFundamental (juce::Graphics& g, const ui::MeterCell& cell) const
    {
        if (fundamentalHz <= 0.0f)
        {
            cell.drawNote (g, "FUNDAMENTAL", "-", colours::dim);
            return;
        }
        using themis::fundamental::Balance;
        const auto balance = noFundamentalHarmonics ? Balance::subHeavy : themis::fundamental::balanceOf (fundamentalTiltDb);
        cell.drawNote (g, "FUNDAMENTAL", ui::hzText (fundamentalHz) + "  " + themis::fundamental::name (balance), colours::text);
    }

    void drawHover (juce::Graphics& g) const
    {
        if (hoverBand < 0 || !haveData)
            return;
        const auto s = static_cast<size_t> (hoverBand);
        const double hz = themis::BandGrid::centreHz (hoverBand);
        g.setColour (colours::text.withAlpha (0.35f));
        g.drawVerticalLine (juce::roundToInt (xFor (hz)), plot.getY(), plot.getBottom());
        const float d = measured[s] - target.medianDb[s];
        g.setColour (colours::text);
        g.setFont (ui::font (12.0f));
        const float tilt = tiltDb (hoverBand);
        juce::String text = ui::hzText (hz) + "   signal " + juce::String (measured[s] + tilt, 1) + "   target " + juce::String (target.medianDb[s] + tilt, 1)
                            + "   delta " + ui::signedDb (d);
        if (haveFold)
            text += "   mono -" + juce::String (-fold[s], 1) + " dB";
        g.drawText (text, annotationRow (0, true), juce::Justification::right);
        if (device != nullptr)
            g.drawText ("via " + juce::String (device->name) + "   " + juce::String (measured[s] + deviceResponse[s] + tilt, 1) + "   (device "
                            + ui::signedDb (deviceResponse[s]) + ")",
                        annotationRow (1, true), juce::Justification::right);
    }

    ThemisProcessor& proc;
    themis::BandArray measured {};
    themis::BandArray fold {};           // mono fold per band (dB, <= 0)
    themis::BandArray deviceResponse {};
    themis::TargetCurve target;
    themis::TranslationReport report;
    themis::PhaseBands phaseBands {};
    themis::PhaseReport phase;
    bool havePhase = false;
    bool haveClip = false;
    themis::ClipTotals clipTotals;
    float fundamentalHz = 0.0f, fundamentalTiltDb = 0.0f; // newest tonal bass; 0 Hz when there is none
    bool noFundamentalHarmonics = false;
    bool overBroadbandStrip = false;
    juce::Rectangle<float> broadbandStrip;
    const themis::DeviceProfile* device = nullptr;
    double deviceLoHz = themis::BandGrid::kF0, deviceHiHz = themis::BandGrid::kMaxHz;
    int deviceIndex = -1;
    bool haveFold = false;
    bool overTranslationStrip = false;
    juce::Rectangle<float> translationStrip;
    bool haveData = false;
    bool haveCrest = false;
    float lowCrestDb = 0.0f;
    float tolerance = 1.0f;
    double fs = 48000.0;
    juce::Rectangle<float> plot, screen; // the plot, and the glass window it and its axis labels sit in
    int hoverBand = -1;
};
