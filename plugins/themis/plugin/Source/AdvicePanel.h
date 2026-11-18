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
        eventLog = proc.eventLogCopy();
        offsets = themis::delivery::offsetsOf (inputs.integratedLufs);
        countsNow = themis::plan::countsOf (advice);
        copyAllNow = themis::plancopy::copyAll (advice);
        verdictNow = themis::plan::verdictOf (countsNow, advice.measuring, inputs.analysedSeconds);
        firstRow = juce::jlimit (0, std::max (0, static_cast<int> (advice.items.size()) - 1), firstRow);
        repaint();
    }

    // The verdict the last refresh read, in words and counts: the plate under the nameplate shows the same thing as the plan's title.
    struct VerdictState
    {
        themis::plan::Verdict verdict = themis::plan::Verdict::measuring;
        themis::plan::Counts counts;
    };
    VerdictState verdictState() const { return { verdictNow, countsNow }; }
    juce::String plateDetail() const { return verdictDetail(); } // the line under the verdict word on the plate

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

    // The painted chips at the panel's current size (SNAP, TEXT, and each shown row's HEAR and COPY), for the smoke test's control audit.
    std::vector<juce::Rectangle<int>> chipAreas() const
    {
        std::vector<juce::Rectangle<int>> chips { snapChip, textChip, copyAllChip };
        for (int r = 0; r < visibleRows() && firstRow + r < static_cast<int> (advice.items.size()); ++r)
        {
            const auto& item = advice.items[static_cast<size_t> (firstRow + r)];
            const auto column = chipColumn (rowBounds (r));
            if (item.hear.active())
                chips.push_back (hearChip (column));
            if (item.canCopy())
                chips.push_back (copyChip (column));
        }
        return chips;
    }
    juce::String loudnessTooltipText() const { return loudnessTooltip(); } // (for the smoke test: the loudness readout's hover text)

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        ui::hw::drawModule (g, area);
        area.reduce (10.0f, 8.0f);

        auto titleRow = area.removeFromTop (16.0f);
        ui::hw::drawLabel (g, "MASTER PLAN", titleRow.toNearestInt());
        // COPY ALL: the whole plan as one clipboard text, after the title. Greyed (with its reason on hover and in the status line)
        // when the plan has nothing a plugin can take.
        const auto titleBounds = titleRow.toNearestInt();
        juce::GlyphArrangement labelGlyphs;
        labelGlyphs.addLineOfText (ui::labelFont (10.5f), "MASTER PLAN", 0.0f, 0.0f);
        const int labelWidth = juce::roundToInt (labelGlyphs.getBoundingBox (0, -1, false).getWidth());
        copyAllChip = { titleBounds.getX() + labelWidth + 10, titleBounds.getY(), kCopyAllWidth, 16 };
        ui::hw::drawPushButton (g, copyAllChip.toFloat(), false, colours::amber, false, hoverCopyAll, copyAllOn());
        g.setColour (copyAllOn() ? colours::text : colours::dim);
        g.setFont (ui::labelFont (9.5f));
        g.drawText ("COPY ALL", copyAllChip, juce::Justification::centred);
        const auto [verdict, verdictColour] = verdictText();
        ui::hw::drawLamp (g, { titleRow.getRight() - 5.0f, titleRow.getCentreY() }, 4.0f, verdictColour, verdictColour != colours::dim);
        g.setColour (verdictColour);
        g.setFont (ui::lcdFont (11.5f, juce::Font::bold));
        g.drawText (verdict, titleRow.withTrimmedRight (16.0f).toNearestInt(), juce::Justification::right);
        g.setFont (ui::labelFont (8.5f));
        g.setColour (colours::dim);
        g.drawFittedText (kLegend, area.removeFromTop (12.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);

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
        const bool copyAll = copyAllChip.contains (e.position.toInt());
        if (copyAll != hoverCopyAll)
        {
            hoverCopyAll = copyAll;
            repaint (copyAllChip);
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
        setMouseCursor (hoverSnap || hoverText || hoverCopy || hoverCopyAll || (hoverRow >= 0 && advice.items[static_cast<size_t> (hoverRow)].hear.active()) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; hoverSnap = hoverCopy = hoverText = hoverCopyAll = false; }

    // Called with the status text after COPY put an item's bands on the clipboard.
    std::function<void (const juce::String&)> onCopied;

    // SNAP keeps the plan as it is now; the readout then says what changed since (CLEAR forgets it). Not saved.
    void toggleSnapshot()
    {
        snapshot = snapshot.have ? themis::PlanSnapshot {} : themis::snapshotOf (inputs, advice);
        repaint();
    }
    bool hasSnapshot() const { return snapshot.have; }

    // TEXT puts the check report on the clipboard as plain text: the plan, the readings and the event log. Shift-TEXT puts only the plan.
    juce::String planText() const { return juce::String (themis::planAsText (inputs, advice)); }
    juce::String reportText() const { return juce::String (themis::reportAsText (inputs, advice, eventLog)); }
    void copyText (bool planOnly)
    {
        refresh();
        juce::SystemClipboard::copyTextToClipboard (planOnly ? planText() : reportText());
        if (onCopied)
            onCopied (juce::String (planOnly ? "Copied the master plan as text (" : "Copied the check report as text (")
                      + juce::String (static_cast<int> (advice.items.size())) + " item(s)): paste it in a note, a message or a session log.");
    }

    // COPY ALL: the whole plan on the clipboard as one text (plancopy::copyAll), or the reason it has nothing to copy. The status line says which.
    void copyAllToClipboard()
    {
        refresh();
        if (!copyAllNow.empty())
            juce::SystemClipboard::copyTextToClipboard (juce::String (copyAllNow.text));
        if (onCopied)
            onCopied (juce::String (themis::plancopy::statusText (copyAllNow)));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (copyAllChip.contains (e.position.toInt()))
        {
            copyAllToClipboard();
            return;
        }
        if (snapChip.contains (e.position.toInt()))
        {
            toggleSnapshot();
            return;
        }
        if (textChip.contains (e.position.toInt()))
        {
            copyText (e.mods.isShiftDown());
            return;
        }
        const int row = rowAt (e.position.toInt());
        if (row >= 0 && overCopy (e.position.toInt()))
        {
            const auto& item = advice.items[static_cast<size_t> (row)];
            juce::SystemClipboard::copyTextToClipboard (gui::linkText (item.moves, nullptr, item.limiter, item.width));
            if (onCopied)
                onCopied (copyStatus (item));
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
            return "TEXT puts the check report on the clipboard as plain text: the master plan (the target, the ceiling, every item worst first with what to change and why), "
                   "then the readings (loudness, LRA, true peaks, PLR, clips, mono sum, phase and mono-fold worst bands) and the event log: the first clip, "
                   "each new true-peak maximum above the ceiling and each loudness excursion, with the time in the audio since the Reset. "
                   "Shift-TEXT copies only the plan. For a note to yourself, a message to the client or a session log. "
                   "(COPY on an item is different: it hands bands or a limiter setting to another plugin.)";
        if (hoverCopyAll)
            return copyAllNow.empty() ? juce::String ("COPY ALL is off: ") + juce::String (copyAllNow.reason) + "."
                                      : juce::String ("COPY ALL puts every item of the plan that a plugin can take on the clipboard as one text: the bands for Iris or Apollo "
                                        "(Hephaestus takes the fill band), the limiter line for Aegis and the width line for Castor. Paste it once in each plugin; each one takes "
                                        "the lines it reads and skips the rest. A width line is left out when a band on the side makes the same cut. "
                                        "COPY on a single item still works for one item at a time.");
        if (hoverRow < 0 && readoutArea.contains (lastMouse))
            return loudnessTooltip();
        if (hoverRow < 0)
            return "The master plan turns every measurement into what to do next, worst first. The limits behind it are rules of thumb, not measured data, "
                   "and a plan is only as good as the audio that has played: press Reset, then play the whole track. Themis never changes the audio; "
                   "only the HEAR chips and the AUDITION buttons change what you hear.";
        const auto& item = advice.items[static_cast<size_t> (hoverRow)];
        if (hoverCopy)
            return juce::String (item.moves.empty() ? "Copy the limiter setting that makes this change" : "Copy the band(s) that make this change") + ", to paste in "
                   + gui::linkDestination (item.moves, item.limiter, item.width) + ". "
                   + (item.width.active() ? "A width item hands the same cut twice: paste it in Iris or Apollo or in Castor, one of them, not both. " : juce::String())
                   + "The amounts come from the measurement; judge the result by ear (SNAP first, to compare before and after).";
        juce::String text = juce::String (item.headline) + "\n\n" + juce::String (item.why);
        if (item.hear.active())
            text += "\n\nClick to hear it: solo " + juce::String (themis::soloText (item.hear)) + ". Click again to release.";
        return text;
    }

private:
    themis::plan::Counts countsNow;
    themis::plan::Verdict verdictNow = themis::plan::Verdict::measuring;
    static constexpr int kRowHeight = 58, kMoreHeight = 14, kChipWidth = 40, kCopyAllWidth = 64;
    static constexpr int kLevelWordWidth = 40; // the level word at the end of a headline's first line (gui::levelMark)
    // What the chips of a row do, in one line under the title (COPY reaches Iris and Apollo for equaliser bands, Hephaestus for a fill band,
    // Aegis for a limiter and Castor for a width line).
    static constexpr const char* kLegend = "HEAR: solo only this problem. COPY: one item to Iris, Apollo, Hephaestus, Aegis or Castor. COPY ALL: the whole plan. SNAP: keep it to compare.";

    // The status line after COPY. A width item hands the same cut twice (a side bell for Iris or Apollo, a width line for Castor),
    // so the status says to paste one of them, not both.
    static juce::String copyStatus (const themis::Advice& item)
    {
        if (item.width.active())
            return "Copied: paste it in Iris or Apollo (a side bell) OR in Castor (its header PASTE), one of them, not both: each cuts the same side.";
        return "Copied: paste it in " + gui::linkDestination (item.moves, item.limiter, item.width) + " (PASTE on its "
               + (item.moves.empty() ? "panel)." : "band panel).");
    }

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
    bool copyAllOn() const { return !copyAllNow.empty(); }
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
        const int headlineLines = glyphs.getBoundingBox (0, -1, false).getWidth() > static_cast<float> (row.getWidth() - kLevelWordWidth) ? 2 : 1;
        auto headlineRow = row.removeFromTop (headlineLines * 15);
        g.setColour (colours::text);
        g.setFont (headlineFont);
        g.drawFittedText (item.headline, headlineRow.withTrimmedRight (kLevelWordWidth), juce::Justification::topLeft, headlineLines, 0.9f);
        // The level word, so the lamp's colour is not the only coding (the plan's fix and check items look alike to a colour-blind reader).
        g.setColour (colours::forLevelText (item.level));
        g.setFont (ui::labelFont (gui::kLevelWordFontHeight));
        g.drawText (gui::levelMark (item.level), headlineRow.removeFromRight (kLevelWordWidth).removeFromTop (15), juce::Justification::centredRight);
        g.setColour (colours::dim);
        g.setFont (ui::font (10.5f));
        g.drawFittedText (item.why, row.removeFromTop (14 * (3 - headlineLines)), juce::Justification::topLeft, 3 - headlineLines, 1.0f);
    }

    // What goes after the verdict word: the count line, or the measuring progress. Empty when the word says it all.
    juce::String verdictDetail() const
    {
        using V = themis::plan::Verdict;
        if (verdictNow == V::faults || verdictNow == V::check)
            return juce::String (themis::plan::countText (countsNow));
        if (verdictNow == V::measuring)
            return juce::String (juce::roundToInt (inputs.analysedSeconds)) + " / " + juce::String (juce::roundToInt (themis::master::kMinSeconds)) + " s";
        return {};
    }

    // The plan's title: the count line for faults and checks, else the word (with the measuring progress).
    std::pair<juce::String, juce::Colour> verdictText() const
    {
        using V = themis::plan::Verdict;
        const juce::String word (themis::plan::verdictWord (verdictNow));
        const auto text = verdictNow == V::faults || verdictNow == V::check ? verdictDetail()
                        : verdictNow == V::measuring ? word + "  " + verdictDetail() : word;
        return { text, colours::forVerdict (verdictNow) };
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
            parts.add ("PLR " + juce::String (themis::master::plrDb (inputs.clip.truePeakDb, inputs.integratedLufs), 1) + " dB");
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

    // The loudness readout's tooltip: the terms first, then what only needs the audio measured so far: the offset to each
    // delivery target, how far the loudest 3 s sit above the average, the crest factor, and the silence at either edge.
    // Nothing here is judged (the plan judges the Deliver for target alone); each figure is shown once the plan has its 20 s.
    juce::String loudnessTooltip() const
    {
        juce::String text = "Loudness since Reset: integrated LUFS (the Deliver for target in brackets), true peak and PLR (true peak minus integrated, "
                            "the room between the peaks and the average). Second line: momentary (last 400 ms) and short-term (last 3 s) LUFS, "
                            "and the loudness range LRA (EBU Tech 3342: the spread between the quieter and the louder sections, 10th to 95th percentile "
                            "of the short-term values). A low LRA (under about 4 LU) means the sections all sit at one level.";
        if (inputs.analysedSeconds < themis::master::kMinSeconds)
            return text + "\n\nThe figures below (the delivery targets, the loudest section, the crest factor and the silence) follow once "
                   + juce::String (juce::roundToInt (themis::master::kMinSeconds)) + " s of audio have played.";
        if (inputs.haveLoudness)
        {
            text += "\n\nAgainst each delivery target (the integrated loudness minus the target; quieter than it is negative):";
            for (size_t i = 0; i < offsets.size(); ++i)
            {
                const float off = offsets[i].offsetDb;
                const bool chosen = std::abs (offsets[i].targetLufs - inputs.targetLufs) < 0.01f;
                const juce::String amount = std::abs (off) < 0.05f ? juce::String ("level with it") : juce::String (std::abs (off), 1) + (off < 0.0f ? " dB quieter" : " dB louder");
                text += "\n  At " + juce::String (offsets[i].targetLufs, 0) + " LUFS (" + juce::String (params::loudnessTargets[i].shortName) + "): " + amount
                        + (chosen ? "   <- Deliver for" : "");
            }
        }
        if (inputs.haveLoudness)
        {
            const auto loudest = themis::loudestSection (inputs.maxShortTermLufs, inputs.integratedLufs);
            if (loudest.have)
                text += "\n\nThe loudest 3 s since the Reset sit " + juce::String (loudest.aboveAverageLu, 1) + " LU above the integrated loudness. A wide gap means the loud "
                        "sections stand out from the rest; a narrow one means the sections sit close together.";
        }
        if (inputs.mono.have)
        {
            if (inputs.mono.monoSilent)
                text += "\n\nMono sum (L + R) / 2: silent, so the channels cancel completely while the stereo programme is not.";
            else
            {
                const float delta = inputs.mono.deltaLu;
                const juce::String amount = std::abs (delta) < 0.05f ? juce::String ("reads the same as the stereo programme")
                                                                    : juce::String (std::abs (delta), 1) + (delta < 0.0f ? " LU lower" : " LU higher") + " than the stereo programme";
                text += "\n\nMono sum (L + R) / 2 integrated: " + juce::String (inputs.mono.monoLufs, 1) + " LUFS, " + amount + ". Uncorrelated channels already lose "
                        + juce::String (themis::monoloud::kUncorrelatedLossDb, 1) + " LU in a mono sum; rule of thumb: " + juce::String (themis::monoloud::kWorthALookLu, 1)
                        + " LU more than that is worth a look, since a master that cancels in mono may translate badly.";
            }
        }
        if (inputs.haveBlockDynamics)
            text += "\n\nBlock dynamics (Themis' own measure, not a standard and not comparable with other meters' dynamic range): the mean peak minus the mean RMS of the loudest "
                    + juce::String (themis::blockdynamics::kLoudestFraction * 100.0, 0) + " % of the "
                    + juce::String (themis::blockdynamics::kBlockSeconds, 0) + " s blocks since the Reset, " + juce::String (inputs.blockDynamicsDb, 1)
                    + " dB. Rule of thumb: " + juce::String (themis::blockdynamics::kHeavilyLimitedDb, 0) + " dB or less reads heavily limited, "
                    + juce::String (themis::blockdynamics::kDynamicDb, 0) + " dB or more dynamic.";
        if (inputs.clip.haveCrest())
            text += "\n\nCrest factor (full band, the sample peak over the RMS level of both channels): " + juce::String (inputs.clip.crestDb(), 1)
                    + " dB. A sine reads 3 dB and a square wave 0 dB. The plan's squashed check uses PLR, the true peak over the integrated loudness.";
        if (inputs.haveSilence)
            text += "\n\nSilence: " + juce::String (inputs.leadingSilenceSeconds, 1) + " s before the sound starts and "
                    + juce::String (inputs.trailingSilenceSeconds, 1) + " s after it, measured to the sample since the Reset. Themis cannot trim it; "
                      "it is for your edit.";
        return text;
    }

    // While the plan measures: the first-run checklist, each line ticked from what the plugin has seen. Otherwise, when nothing is to
    // fix, the step after that (the plan's own rules are the limits; the next step is the reference and the export).
    juce::String emptyText() const
    {
        if (advice.measuring)
        {
            juce::StringArray lines { "Get started:" };
            for (const auto& step : usage::checklist (proc.usageState()))
                lines.add ((step.done ? "[x]  " : "[ ]  ") + juce::String (step.label));
            lines.add ("Clipping is judged at once; the rest follows the audio that has played.");
            return lines.joinIntoString ("\n");
        }
        return "Nothing to fix: all " + juce::String (advice.checks) + " checks that have data are inside their limits. The limits are rules of thumb, so trust your ears for the last call.\n\n"
               "Next: compare with a reference (LOAD, then REF), then press CLEAR before you export.";
    }

    ThemisProcessor& proc;
    themis::MasterInputs inputs;
    themis::MasterAdvice advice;
    std::array<themis::delivery::Offset, static_cast<size_t> (themis::delivery::kCount)> offsets {};
    themis::Switches audition;
    themis::PlanSnapshot snapshot;
    juce::Rectangle<int> listArea, snapChip, textChip, copyAllChip, readoutArea;
    themis::plancopy::Result copyAllNow; // the COPY ALL text of the last refresh (empty = nothing to copy, with its reason)
    themis::events::EventLog eventLog; // the event log since the reset, copied at each refresh
    juce::Point<int> lastMouse;
    int firstRow = 0, hoverRow = -1;
    bool hoverSnap = false, hoverCopy = false, hoverText = false, hoverCopyAll = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvicePanel)
};
