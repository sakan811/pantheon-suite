// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PitchRoll.h"

namespace orpheus::gui
{
// The whole recording on one line, under the display: its waveform, a tick for every note, the playhead, and a bright window that is the
// part the display shows. Drag the window to scroll; drag either edge to zoom; click anywhere else to bring that place to the middle.
class Navigator : public juce::Component, private juce::Timer
{
public:
    Navigator (Session& s, PitchRoll& r, Playback& p) : session (s), roll (r), playback (p) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        const auto all = getLocalBounds().toFloat();
        ui::hw::drawGlass (g, all, 4.0f);
        if (! session.isAnalysed())
        {
            ui::hw::drawGlassOverlay (g, all, 4.0f);
            return;
        }
        const auto area = trackArea();
        const double duration = session.seconds();
        const float mid = area.getCentreY(), half = area.getHeight() * 0.42f;
        g.saveState();
        g.reduceClipRegion (getLocalBounds());

        // the waveform, normalised to the loudest peak so a quiet recording still shows its shape
        float loudest = 0.0f;
        for (size_t b = 0; b < session.peakMax.size(); ++b)
            loudest = std::max ({ loudest, session.peakMax[b], -session.peakMin[b] });
        const float gain = loudest > 0.0f ? 1.0f / loudest : 1.0f;
        g.setColour (colours::accent.withAlpha (0.38f));
        for (int x = (int) area.getX(); x < (int) area.getRight(); ++x)
        {
            const double t0 = ((float) x - area.getX()) / area.getWidth() * duration, t1 = ((float) x + 1.0f - area.getX()) / area.getWidth() * duration;
            const int b0 = (int) (t0 * session.sampleRate / Session::kPeakBlock), b1 = std::max (b0, (int) (t1 * session.sampleRate / Session::kPeakBlock));
            if (b0 >= (int) session.peakMax.size())
                break;
            float lo = 0.0f, hi = 0.0f;
            for (int b = b0; b <= std::min (b1, (int) session.peakMax.size() - 1); ++b)
            {
                lo = std::min (lo, session.peakMin[(size_t) b]);
                hi = std::max (hi, session.peakMax[(size_t) b]);
            }
            g.drawVerticalLine (x, mid - hi * gain * half, mid - lo * gain * half + 1.0f);
        }

        // notes: one tick each, at a height that follows its pitch
        if (! session.notes.empty())
        {
            float lo = 1000.0f, hi = -1000.0f;
            for (const auto& n : session.notes)
            {
                lo = std::min (lo, n.target);
                hi = std::max (hi, n.target);
            }
            for (const auto& n : session.notes)
            {
                const float y = area.getBottom() - 3.0f - (hi > lo ? (n.target - lo) / (hi - lo) : 0.5f) * (area.getHeight() - 6.0f);
                const float x0 = xOf (n.start * session.contour.hop / session.sampleRate), x1 = xOf (n.end * session.contour.hop / session.sampleRate);
                g.setColour (n.edited() ? colours::accent : colours::amber);
                g.fillRect (x0, y - 1.0f, std::max (2.0f, x1 - x0), 2.0f);
            }
        }

        // the part the display shows
        const auto view = viewRect();
        g.setColour (colours::text.withAlpha (0.10f));
        g.fillRect (view);
        g.setColour (colours::text.withAlpha (0.85f));
        g.drawRect (view, 1.5f);
        g.fillRect (view.getX() - 1.0f, view.getY() + 4.0f, 3.0f, view.getHeight() - 8.0f); // the grips at the two ends
        g.fillRect (view.getRight() - 2.0f, view.getY() + 4.0f, 3.0f, view.getHeight() - 8.0f);

        g.setColour (colours::amber);
        g.drawVerticalLine ((int) xOf ((double) playback.position() / session.sampleRate), area.getY(), area.getBottom());
        g.restoreState();
        ui::hw::drawGlassOverlay (g, all, 4.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! session.isAnalysed())
            return;
        const auto view = viewRect();
        const double t = secondsAt (e.x);
        start0 = roll.scrollSec;
        end0 = roll.scrollSec + roll.visibleSeconds();
        if (std::fabs ((float) e.x - view.getX()) < 7.0f)
            mode = Mode::left;
        else if (std::fabs ((float) e.x - view.getRight()) < 7.0f)
            mode = Mode::right;
        else
        {
            mode = Mode::move;
            grab = view.contains ((float) e.x, (float) e.y) ? t - roll.scrollSec : roll.visibleSeconds() * 0.5;
            roll.scrollToSeconds (t - grab);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! session.isAnalysed())
            return;
        const double t = std::clamp (secondsAt (e.x), 0.0, session.seconds());
        switch (mode)
        {
            case Mode::move: roll.scrollToSeconds (t - grab); break;
            case Mode::left: roll.setViewSeconds (std::min (t, end0 - 0.2), end0); break;
            case Mode::right: roll.setViewSeconds (start0, std::max (t, start0 + 0.2)); break;
            case Mode::none: break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override { mode = Mode::none; }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto view = viewRect();
        const bool edge = session.isAnalysed() && (std::fabs ((float) e.x - view.getX()) < 7.0f || std::fabs ((float) e.x - view.getRight()) < 7.0f);
        setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const float d = std::fabs (w.deltaX) > std::fabs (w.deltaY) ? w.deltaX : -w.deltaY;
        roll.scrollToSeconds (roll.scrollSec + d * roll.visibleSeconds() * 1.5);
    }

    // Where the display's window sits on this bar, for the checks.
    juce::Rectangle<float> viewRect() const
    {
        const auto area = trackArea();
        const float x0 = xOf (roll.scrollSec), x1 = xOf (roll.scrollSec + roll.visibleSeconds());
        return { std::clamp (x0, area.getX(), area.getRight() - 3.0f), area.getY(), std::max (4.0f, std::min (x1, area.getRight()) - std::max (x0, area.getX())), area.getHeight() };
    }

private:
    enum class Mode { none, move, left, right };

    juce::Rectangle<float> trackArea() const { return getLocalBounds().toFloat().reduced (5.0f, 3.0f); }
    float xOf (double seconds) const
    {
        const auto a = trackArea();
        return a.getX() + (float) (seconds / std::max (0.001, session.seconds())) * a.getWidth();
    }
    double secondsAt (int x) const
    {
        const auto a = trackArea();
        return ((float) x - a.getX()) / a.getWidth() * session.seconds();
    }

    // The window moves and the playhead runs: repaint when either has.
    void timerCallback() override
    {
        const auto state = std::make_tuple (roll.scrollSec, roll.pxPerSec, playback.position(), session.version.load(), session.isAnalysed());
        if (state != lastState)
        {
            lastState = state;
            repaint();
        }
    }

    Session& session;
    PitchRoll& roll;
    Playback& playback;
    Mode mode = Mode::none;
    double grab = 0.0, start0 = 0.0, end0 = 0.0;
    std::tuple<double, double, juce::int64, int, bool> lastState {};
};
} // namespace orpheus::gui
