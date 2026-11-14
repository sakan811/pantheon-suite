// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "ThemisUi.h"

#include <themis/MasterAdvisor.h>

// The master plan: what to do to the master, worst first, from everything Themis measures (see themis::advise). Each row
// says what to change and by how much; the tooltip says why; rows that can be listened to have a HEAR chip that sets the
// Solo / Mute audition (click again to release). Display only: it reads the measurements and never touches the audio.
class AdvicePanel : public juce::Component, public juce::TooltipClient
{
public:
    explicit AdvicePanel (ThemisProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::NormalCursor); }

    // Refresh from the measurements (message thread, a couple of times a second).
    void refresh()
    {
        inputs = proc.masterInputs();
        advice = themis::advise (inputs);
        firstRow = juce::jlimit (0, std::max (0, static_cast<int> (advice.items.size()) - 1), firstRow);
        repaint();
    }

    // The audition state changed elsewhere: only the HEAR chips care.
    void auditionChanged()
    {
        const auto now = proc.auditionSwitches();
        if (now != audition)
        {
            audition = now;
            repaint();
        }
    }

    // The most important item that can be listened to (for demos and tests), or an empty Switches.
    themis::Switches firstHearable() const
    {
        for (const auto& item : advice.items)
            if (item.hear.active())
                return item.hear;
        return {};
    }
    const themis::MasterAdvice& currentAdvice() const { return advice; }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        ui::hw::drawModule (g, area);
        area.reduce (10.0f, 8.0f);

        auto titleRow = area.removeFromTop (16.0f);
        ui::hw::drawLabel (g, "MASTER PLAN", titleRow.toNearestInt());
        const auto [verdict, verdictColour] = verdictText();
        ui::hw::drawLamp (g, { titleRow.getRight() - 5.0f, titleRow.getCentreY() }, 4.0f, verdictColour, verdictColour != colours::dim);
        g.setColour (verdictColour);
        g.setFont (ui::lcdFont (11.5f, juce::Font::bold));
        g.drawText (verdict, titleRow.withTrimmedRight (16.0f).toNearestInt(), juce::Justification::right);

        auto readout = area.removeFromTop (snapshot.have ? 50.0f : 34.0f);
        auto chips = readout.removeFromRight (kChipWidth);
        snapChip = chips.removeFromTop (16.0f).toNearestInt();
        textChip = chips.withTrimmedTop (2.0f).removeFromTop (16.0f).toNearestInt();
        ui::hw::drawPushButton (g, snapChip.toFloat(), snapshot.have, colours::amber, false, hoverSnap);
        g.setColour (snapshot.have ? colours::lampInk : colours::text);
        g.setFont (ui::labelFont (9.5f));
        g.drawText (snapshot.have ? "CLEAR" : "SNAP", snapChip, juce::Justification::centred);
        ui::hw::drawPushButton (g, textChip.toFloat(), false, colours::amber, false, hoverText);
        g.setColour (colours::text);
        g.drawText ("TEXT", textChip, juce::Justification::centred);
        readout.removeFromRight (4.0f);
        readoutArea = readout.toNearestInt();
        ui::hw::drawGlass (g, readout, 3.0f, colours::amber);
        g.setFont (ui::lcdFont (11.0f));
        g.setColour (colours::amber);
        auto text = readout.reduced (7.0f, 0.0f);
        g.drawText (readoutText(), text.removeFromTop (17.0f).toNearestInt(), juce::Justification::centredLeft);
        g.drawText (loudnessText(), text.removeFromTop (14.0f).toNearestInt(), juce::Justification::centredLeft);
        if (snapshot.have)
        {
            g.setColour (colours::text);
            g.drawText ("SINCE SNAP: " + juce::String (themis::describe (themis::compare (snapshot, themis::snapshotOf (inputs, advice)))),
                        text.toNearestInt(), juce::Justification::centredLeft);
        }
        ui::hw::drawGlassOverlay (g, readout, 3.0f, false);
        area.removeFromTop (6.0f);

        listArea = area.toNearestInt();
        if (advice.items.empty())
        {
            ui::hw::drawGlass (g, listArea.toFloat().withTrimmedBottom (4.0f), 4.0f, advice.measuring ? colours::amber : colours::good);
            g.setFont (ui::lcdFont (11.5f));
            g.setColour (advice.measuring ? colours::amber : colours::good);
            g.drawFittedText (emptyText(), listArea.reduced (12, 10), juce::Justification::topLeft, 8, 1.0f);
            return;
        }

        const int rows = visibleRows();
        for (int r = 0; r < rows && firstRow + r < static_cast<int> (advice.items.size()); ++r)
            drawRow (g, advice.items[static_cast<size_t> (firstRow + r)], rowBounds (r));

        const int hidden = static_cast<int> (advice.items.size()) - (firstRow + rows);
        if (hidden > 0 || firstRow > 0)
        {
            g.setFont (ui::font (10.5f));
            g.setColour (colours::dim);
            g.drawText (juce::String (hidden > 0 ? hidden : 0) + " more below - scroll", juce::Rectangle<int> (listArea.getX(), listArea.getBottom() - kMoreHeight, listArea.getWidth(), kMoreHeight),
                        juce::Justification::centredRight);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        hoverRow = rowAt (e.position.toInt());
        lastMouse = e.position.toInt();
        if (const bool copy = overCopy (lastMouse); copy != hoverCopy)
        {
            hoverCopy = copy;
            repaint();
        }
        const bool snap = snapChip.contains (e.position.toInt());
        if (snap != hoverSnap)
        {
            hoverSnap = snap;
            repaint (snapChip);
        }
        const bool text = textChip.contains (e.position.toInt());
        if (text != hoverText)
        {
            hoverText = text;
            repaint (textChip);
        }
        setMouseCursor (hoverSnap || hoverText || hoverCopy || (hoverRow >= 0 && advice.items[static_cast<size_t> (hoverRow)].hear.active()) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; hoverSnap = hoverCopy = hoverText = false; }

    // Called with the status text after COPY put an item's bands on the clipboard.
    std::function<void (const juce::String&)> onCopied;

    // SNAP keeps the plan as it is now; the readout then says what changed since (CLEAR forgets it). Not saved.
    void toggleSnapshot()
    {
        snapshot = snapshot.have ? themis::PlanSnapshot {} : themis::snapshotOf (inputs, advice);
        repaint();
    }
    bool hasSnapshot() const { return snapshot.have; }

    // TEXT puts the whole plan on the clipboard as plain text (the target, the numbers, every item and its why): for a note, a message, a session log.
    juce::String planText() const { return juce::String (themis::planAsText (inputs, advice)); }
    void copyPlanText()
    {
        refresh();
        juce::SystemClipboard::copyTextToClipboard (planText());
        if (onCopied)
            onCopied ("Copied the master plan as text (" + juce::String (static_cast<int> (advice.items.size())) + " item(s)): paste it in a note, a message or a session log.");
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (snapChip.contains (e.position.toInt()))
        {
            toggleSnapshot();
            return;
        }
        if (textChip.contains (e.position.toInt()))
        {
            copyPlanText();
            return;
        }
        const int row = rowAt (e.position.toInt());
        if (row >= 0 && overCopy (e.position.toInt()))
        {
            const auto& item = advice.items[static_cast<size_t> (row)];
            juce::SystemClipboard::copyTextToClipboard (ui::linkText (item.moves, nullptr, item.limiter));
            if (onCopied)
                onCopied ("Copied: paste it in " + ui::linkDestination (item.moves, item.limiter) + " (PASTE on its " + (item.moves.empty() ? "panel)." : "band panel)."));
            return;
        }
        if (row < 0 || !advice.items[static_cast<size_t> (row)].hear.active())
            return;
        const auto hear = advice.items[static_cast<size_t> (row)].hear;
        proc.setAudition (proc.auditionSwitches() == hear ? themis::Switches {} : hear); // a second click releases it
        auditionChanged();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int maxFirst = std::max (0, static_cast<int> (advice.items.size()) - visibleRows());
        const int next = juce::jlimit (0, maxFirst, firstRow + (w.deltaY < 0.0f ? 1 : w.deltaY > 0.0f ? -1 : 0));
        if (next != firstRow)
        {
            firstRow = next;
            repaint();
        }
    }

    juce::String getTooltip() override
    {
        if (hoverSnap)
            return snapshot.have ? "CLEAR forgets the snapshot." :
                   "SNAP keeps the plan as it is now. Change the master, play the same part again (press Reset first for a fair comparison), "
                   "and the plan says how many items were fixed, got better, got worse or are new, and how far the loudness and the true peak moved. "
                   "It works once the plan has finished measuring.";
        if (hoverText)
            return "TEXT puts the whole master plan on the clipboard as plain text: the delivery target and the true-peak ceiling, the measured loudness and peak, "
                   "then every item worst first with what to change and why. For a note to yourself, a message to the client or a session log. "
                   "(COPY on an item is different: it hands bands or a limiter setting to another plugin.)";
        if (hoverRow < 0 && readoutArea.contains (lastMouse))
            return "Loudness since Reset: integrated LUFS (the Deliver for target in brackets), true peak and PLR. Second line: momentary (last 400 ms) "
                   "and short-term (last 3 s) LUFS, and the loudness range LRA (EBU Tech 3342: the spread between the quieter and the louder "
                   "sections, 10th to 95th percentile of the short-term values). A low LRA (under about 4 LU) means the sections all sit at one level.";
        if (hoverRow < 0)
            return "The master plan turns every measurement into what to do next, worst first. The limits behind it are rules of thumb, not measured data, "
                   "and a plan is only as good as the audio that has played: press Reset, then play the whole track. Themis never changes the audio; "
                   "only the HEAR chips and the AUDITION buttons change what you hear.";
        const auto& item = advice.items[static_cast<size_t> (hoverRow)];
        if (hoverCopy)
            return juce::String (item.moves.empty() ? "Copy the limiter setting that makes this change" : "Copy the band(s) that make this change") + ", to paste in "
                   + ui::linkDestination (item.moves, item.limiter) + ". The amounts come from the measurement; judge the result by ear (SNAP first, to compare before and after).";
        juce::String text = juce::String (item.headline) + "\n\n" + juce::String (item.why);
        if (item.hear.active())
            text += "\n\nClick to hear it: solo " + juce::String (themis::soloText (item.hear)) + ". Click again to release.";
        return text;
    }

private:
    static constexpr int kRowHeight = 58, kMoreHeight = 14, kChipWidth = 40;

    int visibleRows() const
    {
        const bool needsMore = static_cast<int> (advice.items.size()) * kRowHeight > listArea.getHeight();
        return std::max (1, (listArea.getHeight() - (needsMore ? kMoreHeight : 0)) / kRowHeight);
    }
    juce::Rectangle<int> rowBounds (int visibleIndex) const
    {
        return { listArea.getX(), listArea.getY() + visibleIndex * kRowHeight, listArea.getWidth(), kRowHeight };
    }
    // The chip column of a row (as drawRow lays it out): HEAR on top, COPY under it.
    static juce::Rectangle<int> chipColumn (juce::Rectangle<int> row) { return row.withTrimmedBottom (4).reduced (9, 5).removeFromRight (kChipWidth); }
    static juce::Rectangle<int> hearChip (juce::Rectangle<int> column) { return column.removeFromTop (17); }
    static juce::Rectangle<int> copyChip (juce::Rectangle<int> column) { return column.withTrimmedTop (21).removeFromTop (17); }
    bool overCopy (juce::Point<int> p) const
    {
        for (int r = 0; r < visibleRows(); ++r)
            if (firstRow + r < static_cast<int> (advice.items.size()) && advice.items[static_cast<size_t> (firstRow + r)].canCopy()
                && copyChip (chipColumn (rowBounds (r))).contains (p))
                return true;
        return false;
    }

    int rowAt (juce::Point<int> p) const
    {
        for (int r = 0; r < visibleRows(); ++r)
            if (firstRow + r < static_cast<int> (advice.items.size()) && rowBounds (r).contains (p))
                return firstRow + r;
        return -1;
    }

    void drawRow (juce::Graphics& g, const themis::Advice& item, juce::Rectangle<int> row) const
    {
        row.removeFromBottom (4);
        const auto colour = colours::forLevel (item.level);
        ui::hw::drawGlass (g, row.toFloat(), 4.0f, colour);
        row.reduce (9, 5);

        ui::hw::drawLamp (g, { static_cast<float> (row.getX()) + 5.0f, static_cast<float> (row.getY()) + 7.5f }, 4.5f, colour);
        row.removeFromLeft (17);

        if (item.hear.active() || item.canCopy())
        {
            const auto column = row.removeFromRight (kChipWidth);
            row.removeFromRight (4);
            const bool hovered = hoverRow >= 0 && &advice.items[static_cast<size_t> (hoverRow)] == &item;
            if (item.hear.active())
            {
                const bool on = audition == item.hear;
                ui::hw::drawPushButton (g, hearChip (column).toFloat(), on, colours::amber, false, hovered && !hoverCopy);
                g.setColour (on ? colours::lampInk : colours::text);
                g.setFont (ui::labelFont (9.5f));
                g.drawText (on ? "STOP" : "HEAR", hearChip (column), juce::Justification::centred);
            }
            if (item.canCopy())
            {
                ui::hw::drawPushButton (g, copyChip (column).toFloat(), false, colours::amber, false, hovered && hoverCopy);
                g.setColour (colours::text);
                g.setFont (ui::labelFont (9.5f));
                g.drawText ("COPY", copyChip (column), juce::Justification::centred);
            }
        }

        const auto headlineFont = ui::font (12.0f, juce::Font::bold);
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (headlineFont, item.headline, 0.0f, 0.0f);
        const int headlineLines = glyphs.getBoundingBox (0, -1, false).getWidth() > static_cast<float> (row.getWidth()) ? 2 : 1;
        g.setColour (colours::text);
        g.setFont (headlineFont);
        g.drawFittedText (item.headline, row.removeFromTop (headlineLines * 15), juce::Justification::topLeft, headlineLines, 0.9f);
        g.setColour (colours::dim);
        g.setFont (ui::font (10.5f));
        g.drawFittedText (item.why, row.removeFromTop (14 * (3 - headlineLines)), juce::Justification::topLeft, 3 - headlineLines, 1.0f);
    }

    std::pair<juce::String, juce::Colour> verdictText() const
    {
        if (advice.measuring)
            return { inputs.analysedSeconds < 0.5 ? juce::String ("PLAY YOUR TRACK")
                                                  : "MEASURING  " + juce::String (juce::roundToInt (inputs.analysedSeconds)) + " / " + juce::String (juce::roundToInt (themis::master::kMinSeconds)) + " s",
                     colours::dim };
        if (advice.items.empty())
            return { "READY", colours::good };
        int bad = 0, warn = 0;
        for (const auto& i : advice.items)
            (i.level == themis::Level::bad ? bad : warn)++;
        juce::StringArray parts;
        if (bad > 0)
            parts.add (juce::String (bad) + " TO FIX");
        if (warn > 0)
            parts.add (juce::String (warn) + " TO CHECK");
        return { parts.joinIntoString ("  -  "), colours::forLevel (advice.level()) };
    }

    juce::String readoutText() const
    {
        juce::StringArray parts;
        if (inputs.haveLoudness)
        {
            const auto level = themis::translation::classify (std::abs (inputs.integratedLufs - inputs.targetLufs), themis::master::kLoudnessWarnLu, themis::master::kLoudnessBadLu);
            parts.add (juce::String (inputs.integratedLufs, 1) + " LUFS (" + juce::String (inputs.targetLufs, 0) + ")" + (level == themis::Level::good ? "" : level == themis::Level::warn ? " !" : " !!"));
        }
        if (inputs.clip.have)
            parts.add ("TP " + juce::String (inputs.clip.truePeakDb, 1) + " dBTP");
        if (inputs.haveLoudness && inputs.clip.have)
            parts.add ("PLR " + juce::String (inputs.clip.truePeakDb - inputs.integratedLufs, 1) + " dB");
        const int secs = static_cast<int> (inputs.analysedSeconds);
        parts.add (juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2) + " analysed");
        return parts.joinIntoString ("   ");
    }

    // The moment-to-moment loudness next to the integrated value: momentary (400 ms), short-term (3 s), and the loudness
    // range since the reset (EBU Tech 3342: how much the sections differ; a heavily limited master reads low).
    juce::String loudnessText() const
    {
        const auto& m = proc.loudnessMonitor();
        const auto lufs = [] (float v) { return v <= themis::LoudnessMeter::kSilence + 1.0f ? juce::String ("--") : juce::String (v, 1); };
        return "M " + lufs (m.momentary()) + "   S " + lufs (m.shortTerm()) + "   LRA "
               + (m.hasData() ? juce::String (m.loudnessRange(), 1) + " LU" : juce::String ("--"));
    }

    juce::String emptyText() const
    {
        if (advice.measuring)
            return "Press Reset, then play the whole track from the start. The plan appears after " + juce::String (juce::roundToInt (themis::master::kMinSeconds))
                   + " s of audio and gets more reliable the more of the song has played (set Averaging to Infinite for a whole-track measurement). Clipping is judged at once.";
        return "Nothing to fix: all " + juce::String (advice.checks) + " checks that have data are inside their limits. The limits are rules of thumb, so trust your ears for the last call.";
    }

    ThemisProcessor& proc;
    themis::MasterInputs inputs;
    themis::MasterAdvice advice;
    themis::Switches audition;
    themis::PlanSnapshot snapshot;
    juce::Rectangle<int> listArea, snapChip, textChip, readoutArea;
    juce::Point<int> lastMouse;
    int firstRow = 0, hoverRow = -1;
    bool hoverSnap = false, hoverCopy = false, hoverText = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvicePanel)
};
