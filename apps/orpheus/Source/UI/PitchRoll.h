// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../Playback.h"
#include "../Session.h"
#include "OrpheusUi.h"

#include <set>

namespace orpheus::gui
{
// The window onto the recording: a piano roll with the notes the singer sang as blobs, the sung pitch drawn through them, and the
// waveform under it.
//
//   amber line   the pitch as sung, every 5 ms
//   teal line    the pitch after the edit (drawn where it differs from the sung one by more than half a cent)
//   blob         a note, where it will be sung: green when it sits on a note of the scale to within 8 cents, amber to 25, red beyond
//   dashed blob  where the note was sung, shown while it has been moved
//
// Drag a blob up or down to move it (it snaps to the scale; Shift moves it at a tenth of the speed, Alt reverses the snapping), double-click to
// snap it or to put it back, drag on empty space to select several, Cmd/Ctrl + wheel zooms time, Alt + wheel zooms pitch.
class PitchRoll : public juce::Component, public juce::SettableTooltipClient, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    PitchRoll (Session& s, Playback& p) : session (s), playback (p)
    {
        setWantsKeyboardFocus (false);
        setOpaque (false);
        pitchBar.setAutoHide (false);
        pitchBar.setColour (juce::ScrollBar::thumbColourId, colours::dim.withAlpha (0.55f));
        pitchBar.setColour (juce::ScrollBar::trackColourId, juce::Colours::black.withAlpha (0.25f));
        pitchBar.addListener (this);
        addAndMakeVisible (pitchBar);
    }

    ~PitchRoll() override { pitchBar.removeListener (this); }

    std::function<void()> onSelectionChanged, onEdited;
    std::function<void (juce::int64)> onSeek;
    juce::String overlayText;     // shown over the display instead of the notes (analysing ...)
    float overlayProgress = -1.0f;
    bool snapToScale = true;      // dragging moves a note from scale note to scale note

    // ---- view ---------------------------------------------------------------------------------------------------------
    static constexpr int kKeysWidth = 62, kRulerHeight = 22, kWaveHeight = 52;

    juce::Rectangle<int> graphArea() const { return getLocalBounds().withTrimmedLeft (kKeysWidth).withTrimmedTop (kRulerHeight).withTrimmedBottom (kWaveHeight); }
    juce::Rectangle<int> waveArea() const { return getLocalBounds().withTrimmedLeft (kKeysWidth).removeFromBottom (kWaveHeight); }

    double pxPerSec = 100.0, scrollSec = 0.0, rowH = 16.0, topMidi = 72.0;

    double xOfSample (double s) const { return kKeysWidth + (s / session.sampleRate - scrollSec) * pxPerSec; }
    double sampleOfX (double x) const { return ((x - kKeysWidth) / pxPerSec + scrollSec) * session.sampleRate; }
    double yOfMidi (double m) const { return kRulerHeight + (topMidi - m) * rowH; }
    double midiOfY (double y) const { return topMidi - (y - kRulerHeight) / rowH; }
    double visibleSeconds() const { return std::max (1, graphArea().getWidth()) / pxPerSec; }

    // Shows the first part of the recording at a readable zoom, and the pitches the singer used.
    void fitToSession()
    {
        selected.clear();
        scrollSec = 0.0;
        pxPerSec = std::clamp ((double) std::max (200, graphArea().getWidth()) / std::min (std::max (1.0, session.seconds()), 14.0), 8.0, 600.0);
        double lo = 1.0e9, hi = -1.0e9;
        for (const auto& n : session.notes)
        {
            lo = std::min<double> (lo, n.centre);
            hi = std::max<double> (hi, n.centre);
        }
        if (lo > hi)
            lo = hi = 60.0;
        const double span = std::max (14.0, hi - lo + 6.0);
        rowH = std::clamp ((double) std::max (100, graphArea().getHeight()) / span, 7.0, 30.0);
        topMidi = (lo + hi) * 0.5 + (graphArea().getHeight() / rowH) * 0.5;
        clampView();
        repaint();
    }

    void zoomTime (double factor, double anchorX)
    {
        const double anchorSec = scrollSec + (anchorX - kKeysWidth) / pxPerSec;
        pxPerSec = std::clamp (pxPerSec * factor, 4.0, 2000.0);
        scrollSec = anchorSec - (anchorX - kKeysWidth) / pxPerSec;
        clampView();
        repaint();
    }

    void zoomPitch (double factor, double anchorY)
    {
        const double anchorMidi = midiOfY (anchorY);
        rowH = std::clamp (rowH * factor, 5.0, 60.0);
        topMidi = anchorMidi + (anchorY - kRulerHeight) / rowH;
        clampView();
        repaint();
    }

    double visibleStartSeconds() const { return scrollSec; }

    // Scrolls so that the display starts at this time.
    void scrollToSeconds (double start)
    {
        scrollSec = start;
        clampView();
        repaint();
    }

    // Shows exactly [start, end] seconds (what the navigator's edges do).
    void setViewSeconds (double start, double end)
    {
        end = std::max (end, start + 0.2);
        pxPerSec = std::clamp ((double) std::max (1, graphArea().getWidth()) / (end - start), 4.0, 2000.0);
        scrollSec = start;
        clampView();
        repaint();
    }

    void scrollPitch (double semitones)
    {
        topMidi += semitones;
        clampView();
        repaint();
    }

    // Keeps the playhead on screen (called while playing).
    void followPlayhead()
    {
        const double t = (double) playback.position() / session.sampleRate;
        if (t < scrollSec || t > scrollSec + visibleSeconds() * 0.92)
        {
            scrollSec = std::max (0.0, t - visibleSeconds() * 0.08);
            clampView();
        }
    }

    void showSample (double sample)
    {
        const double t = sample / session.sampleRate;
        if (t < scrollSec || t > scrollSec + visibleSeconds())
        {
            scrollSec = std::max (0.0, t - visibleSeconds() * 0.1);
            clampView();
        }
    }

    // ---- selection ----------------------------------------------------------------------------------------------------
    bool isSelected (const Note& n) const { return selected.count (n.start) != 0; }
    bool hasSelection() const { return ! selected.empty(); }

    std::vector<size_t> selectedIndices() const
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < session.notes.size(); ++i)
            if (isSelected (session.notes[i]))
                out.push_back (i);
        return out;
    }

    void select (const std::vector<size_t>& indices)
    {
        selected.clear();
        for (const auto i : indices)
            if (i < session.notes.size())
                selected.insert (session.notes[i].start);
        selectionChanged();
    }
    void selectAll()
    {
        selected.clear();
        for (const auto& n : session.notes)
            selected.insert (n.start);
        selectionChanged();
    }
    void clearSelection()
    {
        if (selected.empty())
            return;
        selected.clear();
        selectionChanged();
    }
    // After undo, split or merge: drop what no longer exists.
    void pruneSelection()
    {
        std::set<int> alive;
        for (const auto& n : session.notes)
            if (selected.count (n.start))
                alive.insert (n.start);
        if (alive != selected)
        {
            selected = alive;
            selectionChanged();
        }
    }

    // The samples the selection covers (empty selection: none).
    bool selectionSpan (juce::int64& from, juce::int64& to) const
    {
        bool any = false;
        from = std::numeric_limits<juce::int64>::max();
        to = 0;
        for (const auto& n : session.notes)
            if (isSelected (n))
            {
                from = std::min<juce::int64> (from, (juce::int64) n.start * session.contour.hop);
                to = std::max<juce::int64> (to, (juce::int64) n.end * session.contour.hop);
                any = true;
            }
        return any;
    }

    // ---- editing the selection ---------------------------------------------------------------------------------------
    void nudgeSelected (double semitones)
    {
        const auto which = selectedIndices();
        if (which.empty())
            return;
        session.pushUndo();
        for (const auto i : which)
            session.notes[i].target += (float) semitones;
        applyEdit (which);
    }

    void snapSelected (float amount = 1.0f)
    {
        const auto which = selectedIndices();
        if (which.empty())
            return;
        session.pushUndo();
        for (const auto i : which)
            edit::snap (session.notes[i], session.key, amount);
        applyEdit (which);
    }

    void resetSelected()
    {
        const auto which = selectedIndices();
        if (which.empty())
            return;
        session.pushUndo();
        for (const auto i : which)
            session.notes[i].reset();
        applyEdit (which);
    }

    void straightenSelected()
    {
        const auto which = selectedIndices();
        if (which.empty())
            return;
        session.pushUndo();
        for (const auto i : which)
        {
            session.notes[i].drift = 1.0f;
            session.notes[i].vibrato = 0.0f;
        }
        applyEdit (which);
    }

    bool splitAtSample (juce::int64 sample)
    {
        const int frame = session.contour.frameAtSample ((double) sample);
        session.pushUndo();
        const int i = edit::splitAt (session.notes, session.contour, frame);
        if (i < 0)
        {
            session.undo();
            return false;
        }
        selected.insert (session.notes[(size_t) i].start);
        selected.insert (session.notes[(size_t) i + 1].start);
        session.changed (session.notes[(size_t) i].start, session.notes[(size_t) i + 1].end);
        selectionChanged();
        editedNow();
        return true;
    }

    bool mergeSelected()
    {
        auto which = selectedIndices();
        if (which.empty())
            return false;
        // a lone note merges with the next one; several merge into one
        const size_t first = which.front(), last = which.size() == 1 ? first + 1 : which.back();
        if (last >= session.notes.size() || last == first)
            return false;
        session.pushUndo();
        const int a = session.notes[first].start, b = session.notes[last].end;
        for (size_t k = first; k < last; ++k)
            edit::mergeWithNext (session.notes, session.contour, (int) first);
        selected.clear();
        selected.insert (session.notes[first].start);
        session.changed (a, b);
        selectionChanged();
        editedNow();
        return true;
    }

    // ---- painting -----------------------------------------------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        const auto all = getLocalBounds().toFloat();
        const auto graph = graphArea();
        ui::hw::drawGlass (g, all.withTrimmedLeft ((float) kKeysWidth), 4.0f);

        if (! session.isLoaded() || overlayText.isNotEmpty())
        {
            paintOverlay (g, all.withTrimmedLeft ((float) kKeysWidth));
            ui::hw::drawGlassOverlay (g, all.withTrimmedLeft ((float) kKeysWidth), 4.0f);
            paintKeys (g);
            return;
        }

        const double grid = session.key.gridOffset();
        const int hop = session.contour.hop;
        const double sr = session.sampleRate;

        g.saveState();
        g.reduceClipRegion (graph);

        // pitch rows
        const int kLow = (int) std::floor (midiOfY (graph.getBottom()) - grid) - 1, kHigh = (int) std::ceil (midiOfY (graph.getY()) - grid) + 1;
        const bool scaled = session.key.scale != kChromatic;
        for (int k = kLow; k <= kHigh; ++k)
        {
            const float y = (float) yOfMidi (k + grid);
            const bool inScale = session.key.allows (k);
            const bool black = isBlack (k);
            if (black && ! scaled)
            {
                g.setColour (juce::Colours::black.withAlpha (0.22f));
                g.fillRect ((float) graph.getX(), y - (float) rowH * 0.5f, (float) graph.getWidth(), (float) rowH);
            }
            else if (scaled && ! inScale)
            {
                g.setColour (juce::Colours::black.withAlpha (0.34f));
                g.fillRect ((float) graph.getX(), y - (float) rowH * 0.5f, (float) graph.getWidth(), (float) rowH);
            }
            g.setColour ((pitchClassOf (k) == 0 ? colours::dim.withAlpha (0.55f) : colours::glassGrid.brighter (inScale && scaled ? 0.5f : 0.0f)));
            g.drawHorizontalLine ((int) y, (float) graph.getX(), (float) graph.getRight());
            if (scaled && inScale && pitchClassOf (k) == session.key.root)
            {
                g.setColour (colours::accent.withAlpha (0.10f));
                g.fillRect ((float) graph.getX(), y - (float) rowH * 0.5f, (float) graph.getWidth(), (float) rowH);
            }
        }

        // time grid
        const double step = niceStep (80.0 / pxPerSec);
        for (double t = std::floor (scrollSec / step) * step; t < scrollSec + visibleSeconds() + step; t += step)
        {
            const float x = (float) xOfSample (t * sr);
            g.setColour (colours::glassGrid);
            g.drawVerticalLine ((int) x, (float) graph.getY(), (float) graph.getBottom());
        }

        // loop range
        if (playback.looping.load() && playback.loopEnd.load() > playback.loopStart.load())
        {
            const float x0 = (float) xOfSample ((double) playback.loopStart.load()), x1 = (float) xOfSample ((double) playback.loopEnd.load());
            g.setColour (colours::amber.withAlpha (0.07f));
            g.fillRect (x0, (float) graph.getY(), x1 - x0, (float) graph.getHeight());
            g.setColour (colours::amber.withAlpha (0.35f));
            g.drawVerticalLine ((int) x0, (float) graph.getY(), (float) graph.getBottom());
            g.drawVerticalLine ((int) x1, (float) graph.getY(), (float) graph.getBottom());
        }

        const int firstFrame = std::max (0, (int) (sampleOfX (graph.getX()) / hop) - 1), lastFrame = std::min (session.contour.numFrames() - 1, (int) (sampleOfX (graph.getRight()) / hop) + 1);
        const int stride = std::max (1, (int) (0.5 * (sr / pxPerSec) / hop)); // frames per half pixel

        // notes
        const double bh = std::max (6.0, rowH * 0.74);
        for (size_t i = 0; i < session.notes.size(); ++i)
        {
            const auto& n = session.notes[i];
            if (n.end < firstFrame || n.start > lastFrame)
                continue;
            const auto r = noteRect (n);
            const bool sel = isSelected (n);
            const double offScale = (session.key.snap (n.target) - n.target) * 100.0;
            const auto verdict = verdictColour (offScale);
            if (std::fabs (n.target - n.centre) > 0.04f)
            {
                const auto ghost = juce::Rectangle<float> (r.getX(), (float) (yOfMidi (n.centre) - bh * 0.5), r.getWidth(), (float) bh);
                g.setColour (colours::dim.withAlpha (0.55f));
                juce::Path p;
                p.addRoundedRectangle (ghost, 3.0f);
                const float dashes[] = { 4.0f, 3.0f };
                juce::Path dashed;
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, p, dashes, 2);
                g.fillPath (dashed);
                g.setColour (colours::dim.withAlpha (0.35f));
                const float cx = r.getCentreX();
                g.drawLine (cx, ghost.getCentreY(), cx, r.getCentreY(), 1.0f);
            }
            g.setColour (verdict.withAlpha (sel ? 0.46f : 0.26f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (sel ? colours::text : verdict.withAlpha (0.85f));
            g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, sel ? 1.8f : 1.0f);
            if (r.getWidth() > 46.0f)
            {
                g.setFont (ui::lcdFont (10.0f, juce::Font::bold));
                g.setColour (sel ? colours::text : colours::text.withAlpha (0.8f));
                g.drawText (noteText (n.target, grid), r.withY (r.getY() - 14.0f).withHeight (13.0f), juce::Justification::centredLeft, false);
                if (n.edited())
                {
                    g.setColour (colours::accent);
                    g.fillEllipse (r.getRight() - 9.0f, r.getY() - 10.0f, 5.0f, 5.0f);
                }
            }
        }

        // pitch as sung, then as edited
        {
            juce::Path sung, planned;
            bool sungOpen = false, plannedOpen = false;
            for (int f = firstFrame; f <= lastFrame; f += stride)
            {
                const bool v = session.contour.isVoiced (f);
                const float x = (float) xOfSample ((double) f * hop);
                if (v)
                {
                    const float y = (float) yOfMidi (session.contour.midi[(size_t) f]);
                    sungOpen ? sung.lineTo (x, y) : sung.startNewSubPath (x, y);
                    sungOpen = true;
                }
                else
                    sungOpen = false;
                if (v && f < session.plan.numFrames() && std::fabs (session.plan.shiftCents[(size_t) f]) > 0.5f)
                {
                    const float y = (float) yOfMidi (session.plan.outMidi[(size_t) f]);
                    plannedOpen ? planned.lineTo (x, y) : planned.startNewSubPath (x, y);
                    plannedOpen = true;
                }
                else
                    plannedOpen = false;
            }
            g.setColour (colours::amber.withAlpha (0.62f));
            g.strokePath (sung, juce::PathStrokeType (1.2f));
            g.setColour (colours::accent);
            g.strokePath (planned, juce::PathStrokeType (1.7f));
        }

        // playhead
        {
            const float x = (float) xOfSample ((double) playback.position());
            g.setColour (colours::text.withAlpha (0.9f));
            g.drawVerticalLine ((int) x, (float) graph.getY(), (float) graph.getBottom());
        }

        if (marquee.active)
        {
            g.setColour (colours::accent.withAlpha (0.12f));
            g.fillRect (marquee.rect());
            g.setColour (colours::accent.withAlpha (0.8f));
            g.drawRect (marquee.rect(), 1.0f);
        }
        g.restoreState();

        paintWave (g);
        paintRuler (g);
        paintKeys (g);

        // read-out of the pointer / of the drag
        if (drag.started || hover.valid)
        {
            const juce::String text = drag.started ? dragText : hover.text;
            if (text.isNotEmpty())
            {
                g.setFont (ui::lcdFont (11.0f));
                const int w = textWidth (g.getCurrentFont(), text) + 14;
                const auto box = juce::Rectangle<float> ((float) graph.getX() + 8.0f, (float) graph.getBottom() - 24.0f, (float) w, 18.0f);
                g.setColour (juce::Colours::black.withAlpha (0.72f));
                g.fillRoundedRectangle (box, 3.0f);
                g.setColour (drag.started ? colours::accent : colours::amber);
                g.drawText (text, box.toNearestInt().reduced (7, 0), juce::Justification::centredLeft, false);
            }
        }
        ui::hw::drawGlassOverlay (g, all.withTrimmedLeft ((float) kKeysWidth), 4.0f);
    }

    // ---- mouse --------------------------------------------------------------------------------------------------------
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (auto* p = getParentComponent())
            p->grabKeyboardFocus();
        if (! session.isAnalysed())
            return;
        const auto pos = e.getPosition();
        drag = {};
        marquee = {};
        if (pos.x < kKeysWidth)
        {
            if (pos.y >= kRulerHeight && pos.y < getHeight() - kWaveHeight)
            {
                toneDown = true;
                playback.playTone (std::round (midiOfY ((double) pos.y) - session.key.gridOffset()) + session.key.gridOffset());
            }
            return;
        }
        if (pos.y < kRulerHeight || pos.y >= getHeight() - kWaveHeight)
        {
            seeking = true;
            seekTo (pos.x);
            return;
        }
        if (e.mods.isPopupMenu())
        {
            const int hit = noteAtPoint (pos);
            if (hit >= 0 && ! isSelected (session.notes[(size_t) hit]))
                select ({ (size_t) hit });
            showMenu (pos, hit);
            return;
        }
        const int hit = noteAtPoint (pos);
        if (hit >= 0)
        {
            auto& n = session.notes[(size_t) hit];
            // Shift on a selected note is both "take it out of the selection" (a click) and "fine drag" (a drag): which one is known at mouse-up
            drag.toggleOff = e.mods.isShiftDown() && isSelected (n);
            if (! isSelected (n))
            {
                if (! e.mods.isShiftDown())
                    selected.clear();
                selected.insert (n.start);
                selectionChanged();
            }
            drag.active = true;
            drag.down = pos;
            drag.primary = (size_t) hit;
            drag.fine = e.mods.isShiftDown();
            for (const auto i : selectedIndices())
                drag.origin.emplace_back (i, session.notes[i].target);
            drag.primaryOrigin = n.target;
        }
        else
        {
            marquee.active = true;
            marquee.a = marquee.b = pos.toFloat();
            marquee.additive = e.mods.isShiftDown();
            marquee.before = selected;
            if (! marquee.additive)
                clearSelection();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        if (seeking)
        {
            seekTo (pos.x);
            return;
        }
        if (drag.active)
        {
            if (! drag.started)
            {
                if (std::abs (pos.y - drag.down.y) < 3)
                    return;
                drag.started = true;
                session.pushUndo();
                startTimer (30);
            }
            const bool fine = e.mods.isShiftDown();
            // Shift: a tenth of the speed, never snapped. Alt reverses SNAP for this drag.
            const bool snapping = (snapToScale != e.mods.isAltDown()) && ! fine;
            double raw = -(double) (pos.y - drag.down.y) / rowH;
            if (fine)
                raw *= 0.1;
            double delta = raw;
            if (snapping)
                delta = session.key.snap (drag.primaryOrigin + raw) - drag.primaryOrigin;
            for (const auto& [i, target] : drag.origin)
                session.notes[i].target = target + (float) delta;
            const auto& primary = session.notes[drag.primary];
            dragText = noteText (primary.target, session.key.gridOffset()) + juce::String::formatted ("   moved %+.0f ct", (primary.target - primary.centre) * 100.0)
                       + (snapping ? "" : (fine ? "   fine" : "   free"));
            renderPending = true;
            repaint();
            return;
        }
        if (marquee.active)
        {
            marquee.b = pos.toFloat();
            std::set<int> now = marquee.additive ? marquee.before : std::set<int>();
            const auto box = marquee.rect();
            for (const auto& n : session.notes)
                if (noteRect (n).expanded (0.0f, 3.0f).intersects (box))
                    now.insert (n.start);
            if (now != selected)
            {
                selected = now;
                selectionChanged();
            }
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (toneDown)
        {
            toneDown = false;
            playback.stopTone();
        }
        seeking = false;
        if (drag.started)
        {
            stopTimer();
            flushRender();
            editedNow();
        }
        else if (drag.active && e.getDistanceFromDragStart() < 3)
        {
            // a click on a note without a drag: select it alone (unless shift was held) and leave the playhead where it is
            if (drag.toggleOff)
            {
                selected.erase (session.notes[drag.primary].start);
                selectionChanged();
            }
            else if (! e.mods.isShiftDown())
            {
                const auto& n = session.notes[drag.primary];
                if (selected.size() > 1)
                {
                    selected.clear();
                    selected.insert (n.start);
                    selectionChanged();
                }
            }
        }
        else if (marquee.active && e.getDistanceFromDragStart() < 3)
        {
            if (! marquee.additive)
                clearSelection();
            seekTo (e.x);
        }
        drag = {};
        marquee = {};
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! session.isAnalysed() || e.x < kKeysWidth)
            return;
        const int hit = noteAtPoint (e.getPosition());
        if (hit < 0)
            return;
        auto& n = session.notes[(size_t) hit];
        select ({ (size_t) hit });
        session.pushUndo();
        // in tune already: take the correction back; otherwise snap
        const bool onScale = std::fabs (n.target - (float) session.key.snap (n.centre)) < 0.005f;
        if (onScale && std::fabs (n.target - n.centre) > 0.005f)
            n.target = n.centre;
        else
            edit::snap (n, session.key, 1.0f);
        applyEdit ({ (size_t) hit });
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        hover.valid = false;
        if (session.isAnalysed() && e.x >= kKeysWidth && e.y >= kRulerHeight && e.y < getHeight() - kWaveHeight)
        {
            const double sample = sampleOfX (e.x);
            const int frame = session.contour.frameAtSample (sample);
            hover.sample = (juce::int64) sample;
            hover.text = timeText (sample / session.sampleRate);
            if (session.contour.isVoiced (frame))
            {
                const double midi = session.contour.midi[(size_t) frame];
                hover.text << "   sung " << noteText (midi, session.key.gridOffset());
                if (session.plan.numFrames() > frame && std::fabs (session.plan.shiftCents[(size_t) frame]) > 0.5f)
                    hover.text << "  ->  " << noteText (session.plan.outMidi[(size_t) frame], session.key.gridOffset());
            }
            hover.valid = true;
            setMouseCursor (noteAtPoint (e.getPosition()) >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
        }
        else
            setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hover.valid = false;
        repaint();
    }

    // The wheel scrolls along the recording (a mouse wheel has only one direction, and time is the long axis; a trackpad's sideways
    // swipe does the same). Shift + wheel scrolls the pitch, Cmd/Ctrl + wheel and a pinch zoom time, Alt + wheel zooms pitch.
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const float lead = std::fabs (w.deltaX) > std::fabs (w.deltaY) ? w.deltaX : w.deltaY;
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            zoomTime (std::exp (lead * 1.2), e.x);
        else if (e.mods.isAltDown())
            zoomPitch (std::exp (lead), e.y);
        else if (e.mods.isShiftDown())
            scrollPitch (lead * 12.0);
        else
        {
            scrollSec -= lead * visibleSeconds() * 0.9;
            clampView();
            repaint();
        }
    }

    void mouseMagnify (const juce::MouseEvent& e, float scale) override { zoomTime (scale, e.x); }

    void resized() override
    {
        pitchBar.setBounds (getWidth() - 12, kRulerHeight, 12, std::max (1, getHeight() - kRulerHeight - kWaveHeight));
        clampView();
    }

    // The index of the note under a point of the display, or -1.
    int noteAtPoint (juce::Point<int> p) const
    {
        for (int i = (int) session.notes.size() - 1; i >= 0; --i)
            if (noteRect (session.notes[(size_t) i]).expanded (0.0f, 3.0f).contains (p.toFloat()))
                return i;
        return -1;
    }

    juce::Rectangle<float> noteRect (const Note& n) const
    {
        const double x0 = xOfSample ((double) n.start * session.contour.hop), x1 = xOfSample ((double) n.end * session.contour.hop);
        const double bh = std::max (6.0, rowH * 0.74);
        return { (float) x0, (float) (yOfMidi (n.target) - bh * 0.5), (float) std::max (3.0, x1 - x0), (float) bh };
    }

    // Applies a pending dragged change to the audio (the drag does it every 30 ms and at the end).
    void flushRender()
    {
        if (! renderPending)
            return;
        renderPending = false;
        int first = std::numeric_limits<int>::max(), last = 0;
        for (const auto& [i, target] : drag.origin)
        {
            first = std::min (first, session.notes[i].start);
            last = std::max (last, session.notes[i].end);
        }
        if (first <= last)
            session.changed (first, last);
    }

private:
    struct Marquee
    {
        bool active = false, additive = false;
        juce::Point<float> a, b;
        std::set<int> before;
        juce::Rectangle<float> rect() const { return juce::Rectangle<float> (a, b); }
    };
    struct Drag
    {
        bool active = false, started = false, fine = false, toggleOff = false;
        juce::Point<int> down;
        size_t primary = 0;
        float primaryOrigin = 0;
        std::vector<std::pair<size_t, float>> origin;
    };
    struct Hover
    {
        bool valid = false;
        juce::int64 sample = 0;
        juce::String text;
    };

    static bool isBlack (int k) { const int pc = pitchClassOf (k); return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

    static double niceStep (double minSeconds)
    {
        for (const double s : { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0 })
            if (s >= minSeconds)
                return s;
        return 300.0;
    }

    void timerCallback() override { flushRender(); }

    void clampView()
    {
        const double maxScroll = std::max (0.0, session.seconds() - visibleSeconds() * 0.5);
        scrollSec = std::clamp (scrollSec, -0.05 * visibleSeconds(), maxScroll);
        const double visibleSemis = graphArea().getHeight() / rowH;
        topMidi = std::clamp (topMidi, std::min (36.0 + visibleSemis, 108.0), 108.0);
        pitchBar.setRangeLimits (0.0, 72.0);
        pitchBar.setCurrentRange (108.0 - topMidi, std::min (visibleSemis, 72.0), juce::dontSendNotification);
    }

    void scrollBarMoved (juce::ScrollBar*, double newStart) override
    {
        topMidi = 108.0 - newStart;
        clampView();
        repaint();
    }

    void seekTo (int x)
    {
        const auto s = (juce::int64) std::clamp (sampleOfX ((double) x), 0.0, (double) session.numSamples());
        if (onSeek)
            onSeek (s);
        repaint();
    }

    void selectionChanged()
    {
        if (onSelectionChanged)
            onSelectionChanged();
        repaint();
    }
    void editedNow()
    {
        if (onEdited)
            onEdited();
        repaint();
    }

    void applyEdit (const std::vector<size_t>& which)
    {
        int first = std::numeric_limits<int>::max(), last = 0;
        for (const auto i : which)
        {
            first = std::min (first, session.notes[i].start);
            last = std::max (last, session.notes[i].end);
        }
        session.changed (first, last);
        editedNow();
    }

    void showMenu (juce::Point<int> pos, int hit)
    {
        juce::PopupMenu m;
        const bool any = hasSelection();
        m.addItem (1, "Snap to scale", any);
        m.addItem (2, "Straighten (no drift, no vibrato)", any);
        m.addItem (3, "Reset note", any);
        m.addSeparator();
        m.addItem (4, "Split here", hit >= 0);
        m.addItem (5, "Merge with next", any);
        m.addSeparator();
        m.addItem (6, "Select all", true);
        const auto sampleAtClick = (juce::int64) sampleOfX ((double) pos.x);
        m.showMenuAsync (juce::PopupMenu::Options(),
                         [this, sampleAtClick] (int id)
                         {
                             switch (id)
                             {
                                 case 1: snapSelected(); break;
                                 case 2: straightenSelected(); break;
                                 case 3: resetSelected(); break;
                                 case 4: splitAtSample (sampleAtClick); break;
                                 case 5: mergeSelected(); break;
                                 case 6: selectAll(); break;
                                 default: break;
                             }
                         });
    }

    void paintOverlay (juce::Graphics& g, juce::Rectangle<float> area)
    {
        g.setColour (colours::dim);
        g.setFont (ui::labelFont (13.0f));
        const auto text = overlayText.isNotEmpty() ? overlayText : juce::String ("Drop an audio file here, or press OPEN");
        g.drawText (text.toUpperCase(), area.toNearestInt().withSizeKeepingCentre (area.toNearestInt().getWidth(), 24), juce::Justification::centred);
        if (overlayProgress >= 0.0f)
        {
            const auto bar = area.withSizeKeepingCentre (260.0f, 6.0f).translated (0.0f, 24.0f);
            g.setColour (colours::glassGrid);
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (colours::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * std::clamp (overlayProgress, 0.0f, 1.0f)), 3.0f);
        }
    }

    void paintWave (juce::Graphics& g)
    {
        const auto area = waveArea();
        g.saveState();
        g.reduceClipRegion (area);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (area);
        const float mid = (float) area.getCentreY(), half = (float) area.getHeight() * 0.46f;
        g.setColour (colours::accent.withAlpha (0.55f));
        for (int x = area.getX(); x < area.getRight(); ++x)
        {
            const auto s0 = (juce::int64) std::max (0.0, sampleOfX (x)), s1 = (juce::int64) std::max (0.0, sampleOfX (x + 1));
            const int b0 = (int) (s0 / Session::kPeakBlock), b1 = (int) std::max<juce::int64> (b0, s1 / Session::kPeakBlock);
            if (b0 >= (int) session.peakMax.size())
                break;
            float lo = 0.0f, hi = 0.0f;
            for (int b = b0; b <= std::min (b1, (int) session.peakMax.size() - 1); ++b)
            {
                lo = std::min (lo, session.peakMin[(size_t) b]);
                hi = std::max (hi, session.peakMax[(size_t) b]);
            }
            const float gain = 3.0f;
            g.drawVerticalLine (x, mid - std::min (1.0f, hi * gain) * half, mid - std::max (-1.0f, lo * gain) * half + 1.0f);
        }
        const float px = (float) xOfSample ((double) playback.position());
        g.setColour (colours::text.withAlpha (0.9f));
        g.drawVerticalLine ((int) px, (float) area.getY(), (float) area.getBottom());
        g.restoreState();
    }

    void paintRuler (juce::Graphics& g)
    {
        const auto ruler = getLocalBounds().withTrimmedLeft (kKeysWidth).removeFromTop (kRulerHeight);
        g.saveState();
        g.reduceClipRegion (ruler);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (ruler);
        const double step = niceStep (80.0 / pxPerSec);
        g.setFont (ui::lcdFont (10.0f));
        for (double t = std::floor (scrollSec / step) * step; t < scrollSec + visibleSeconds() + step; t += step)
        {
            const int x = (int) xOfSample (t * session.sampleRate);
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawVerticalLine (x, (float) ruler.getBottom() - 7.0f, (float) ruler.getBottom());
            g.setColour (colours::dim);
            g.drawText (timeText (t), x + 3, ruler.getY(), 70, ruler.getHeight() - 2, juce::Justification::centredLeft, false);
        }
        const float px = (float) xOfSample ((double) playback.position());
        juce::Path tri;
        tri.addTriangle (px - 5.0f, (float) ruler.getBottom() - 8.0f, px + 5.0f, (float) ruler.getBottom() - 8.0f, px, (float) ruler.getBottom());
        g.setColour (colours::text);
        g.fillPath (tri);
        g.restoreState();
    }

    void paintKeys (juce::Graphics& g)
    {
        const auto keys = getLocalBounds().removeFromLeft (kKeysWidth).withTrimmedTop (kRulerHeight).withTrimmedBottom (kWaveHeight);
        g.saveState();
        g.reduceClipRegion (keys.expanded (0, 0));
        g.setColour (colours::background.darker (0.2f));
        g.fillRect (keys);
        if (session.isAnalysed())
        {
            const double grid = session.key.gridOffset();
            const int kLow = (int) std::floor (midiOfY (keys.getBottom()) - grid) - 1, kHigh = (int) std::ceil (midiOfY (keys.getY()) - grid) + 1;
            const bool scaled = session.key.scale != kChromatic;
            for (int k = kLow; k <= kHigh; ++k)
            {
                const bool black = isBlack (k), inScale = session.key.allows (k);
                const auto cell = juce::Rectangle<float> ((float) keys.getX(), (float) (yOfMidi (k + grid) - rowH * 0.5), (float) keys.getWidth() - 3.0f, (float) rowH);
                g.setColour (black ? juce::Colour (0xff23211e) : juce::Colour (0xffd9d2bd).withMultipliedBrightness (inScale || ! scaled ? 1.0f : 0.55f));
                g.fillRect (cell.reduced (0.0f, 0.5f).withTrimmedRight (black ? 14.0f : 0.0f));
                if (rowH >= 11.0 && (! black || rowH >= 15.0))
                {
                    g.setFont (ui::lcdFont (9.5f, pitchClassOf (k) == 0 ? juce::Font::bold : juce::Font::plain));
                    g.setColour (black ? colours::dim : juce::Colour (0xff3a362e));
                    if (pitchClassOf (k) == 0 || rowH >= 14.0)
                        g.drawText (juce::String (noteName (k)), cell.toNearestInt().withTrimmedLeft (4), juce::Justification::centredLeft, false);
                }
                if (scaled && inScale)
                {
                    g.setColour (colours::amber);
                    g.fillEllipse (cell.getRight() - 9.0f, cell.getCentreY() - 2.0f, 4.0f, 4.0f);
                }
            }
        }
        g.restoreState();
    }

    Session& session;
    Playback& playback;
    std::set<int> selected;     // by the first frame of the note, which is unique and survives a split, a merge and an undo
    Marquee marquee;
    Drag drag;
    Hover hover;
    juce::String dragText;
    juce::ScrollBar pitchBar { true };
    bool seeking = false, toneDown = false, renderPending = false;
};
} // namespace orpheus::gui
