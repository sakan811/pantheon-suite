// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PianoRoll.h"

#include <algorithm>
#include <cmath>

namespace olympus::gui
{
namespace
{
constexpr int kEdgeGrab = 6;
constexpr double kMinPxPerBeat = 16.0;
constexpr double kMaxPxPerBeat = 400.0;
constexpr double kFallbackRate = 48000.0;

bool isBlackKey (int pitch)
{
    const int n = ((pitch % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

juce::String pitchName (int pitch)
{
    static const char* const kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int p = juce::jlimit (0, 127, pitch);
    const int octave = (p / 12) - 1;
    return juce::String (kNames[p % 12]) + juce::String (octave);
}

Grid nextPianoRollGrid (Grid g)
{
    switch (g)
    {
        case Grid::Bar:
        case Grid::Half:         return Grid::Quarter;
        case Grid::Quarter:      return Grid::Eighth;
        case Grid::Eighth:       return Grid::Sixteenth;
        case Grid::Sixteenth:    return Grid::ThirtySecond;
        case Grid::ThirtySecond: return Grid::Quarter;
    }
    return Grid::Quarter;
}
} // namespace

struct PianoRollSnapshot
{
    bool valid = false;
    bool missing = false;
    bool isMidiTrack = false;
    bool trackArmed = false;
    juce::String trackName;
    MidiClip clip;
    double rate = kFallbackRate;
    TempoMap tempo;
    Grid grid = Grid::Quarter;
    bool snapOn = true;
};

PianoRollView::PianoRollView (Session& s, Editor& e) : session (s), editor (e)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    snapButton.lamp = colours::amber;
    snapButton.setClickingTogglesState (false);
    snapButton.setWantsKeyboardFocus (false);
    snapButton.setTooltip ("Snap note moves and edits to the grid.");
    snapButton.onClick = [this]
    {
        const auto sp = snapshotPtr();
        editor.setSnap (! sp->snapOn);
        edited();
    };

    gridButton.setWantsKeyboardFocus (false);
    gridButton.setTooltip ("Cycle grid division (1/4, 1/8, 1/16, 1/32).");
    gridButton.onClick = [this] { cycleGrid(); };

    quantizeButton.setWantsKeyboardFocus (false);
    quantizeButton.setTooltip ("Quantize all notes in this MIDI clip to the current grid.");
    quantizeButton.onClick = [this]
    {
        if (! targetRef.valid()) return;
        const auto sp = snapshotPtr();
        juce::String error;
        if (! editor.quantizeMidiClip (targetRef.track, targetRef.clip, sp->grid, error))
            report (error);
        edited();
    };

    closeButton.setWantsKeyboardFocus (false);
    closeButton.setTooltip ("Close the Piano Roll.");
    closeButton.onClick = [this] { if (onClose) onClose(); };

    const auto stepBars = [this] (int deltaBars)
    {
        if (! targetRef.valid()) return;
        const auto sp = snapshotPtr();
        if (! sp->valid) return;
        const double bpb = sp->tempo.beatsPerBar();
        const double curBeats = clipBeats (*sp);
        double newBeats = curBeats + deltaBars * bpb;
        if (deltaBars < 0)
        {
            const double floorBeats = juce::jmin (curBeats, 1.0);
            if (curBeats <= floorBeats + 1e-6)
                return;
            const double minBeats = (curBeats > bpb + 1e-6) ? bpb : floorBeats;
            newBeats = juce::jmax (minBeats, newBeats);
        }
        const double startBeat = sp->tempo.samplesToBeats ((double) sp->clip.start);
        const auto endSample = (juce::int64) std::llround (sp->tempo.beatsToSamples (startBeat + newBeats));
        const auto newLen = juce::jmax ((juce::int64) 1, endSample - sp->clip.start);
        juce::String error;
        if (! editor.resizeMidiClip (targetRef.track, targetRef.clip, newLen, error))
            report (error);
        edited();
    };

    lengthMinusButton.setWantsKeyboardFocus (false);
    lengthMinusButton.setTooltip ("Shorten MIDI clip by 1 bar.");
    lengthMinusButton.onClick = [stepBars] { stepBars (-1); };

    lengthLabel.setFont (ui::labelFont (10.5f));
    lengthLabel.setColour (juce::Label::textColourId, colours::text);
    lengthLabel.setJustificationType (juce::Justification::centred);

    lengthPlusButton.setWantsKeyboardFocus (false);
    lengthPlusButton.setTooltip ("Extend MIDI clip by 1 bar.");
    lengthPlusButton.onClick = [stepBars] { stepBars (1); };

    for (auto* b : std::initializer_list<juce::Component*> { &lengthMinusButton, &lengthLabel, &lengthPlusButton, &snapButton, &gridButton, &quantizeButton, &closeButton })
        addAndMakeVisible (*b);

    centerOnPitch (60);
}

PianoRollView::~PianoRollView()
{
    sendPreviewNoteOff();
    gesture.reset();
}

void PianoRollView::setTarget (MidiClipRef ref)
{
    sendPreviewNoteOff();
    gesture.reset();
    mode = Mode::none;
    targetRef = ref;
    selectedNotes.clear();
    scrollBeats = 0.0;
    lastSnapshotRevision = 0;
    lastSnapshot.reset();
    const auto sp = snapshotPtr();
    syncToolbar (*sp);
    if (! centeredOnce && noteGridBounds().getHeight() > 0)
    {
        centerOnPitch (60);
        centeredOnce = true;
    }
    repaint();
}

void PianoRollView::clearTarget()
{
    sendPreviewNoteOff();
    gesture.reset();
    mode = Mode::none;
    targetRef = {};
    selectedNotes.clear();
    lastSnapshotRevision = 0;
    lastSnapshot.reset();
    lengthLabel.setText ({}, juce::dontSendNotification);
    repaint();
}

std::shared_ptr<const PianoRollSnapshot> PianoRollView::snapshotPtr() const
{
    if (! targetRef.valid())
        return std::make_shared<const PianoRollSnapshot>();

    const auto rev = session.revision.load (std::memory_order_acquire);
    if (lastSnapshot != nullptr && rev == lastSnapshotRevision
        && lastSnapshotRef.track == targetRef.track && lastSnapshotRef.clip == targetRef.clip)
    {
        return lastSnapshot;
    }

    auto build = [this]() -> std::shared_ptr<const PianoRollSnapshot>
    {
        auto s = std::make_shared<PianoRollSnapshot>();
        s->rate = session.sampleRate > 0.0 ? session.sampleRate : kFallbackRate;
        s->tempo = session.tempo;
        s->grid = session.grid;
        s->snapOn = session.snapOn;

        if (! juce::isPositiveAndBelow (targetRef.track, (int) session.tracks.size())
            || ! juce::isPositiveAndBelow (targetRef.clip, (int) session.tracks[(size_t) targetRef.track]->midiClips.size()))
        {
            s->missing = true;
            lastSnapshot = s;
            lastSnapshotRevision = session.revision.load (std::memory_order_acquire);
            lastSnapshotRef = targetRef;
            return s;
        }

        const auto& t = *session.tracks[(size_t) targetRef.track];
        s->valid = true;
        s->isMidiTrack = t.isMidi;
        s->trackArmed = t.armed;
        s->trackName = t.name;
        s->clip = t.midiClips[(size_t) targetRef.clip];

        lastSnapshot = s;
        lastSnapshotRevision = session.revision.load (std::memory_order_acquire);
        lastSnapshotRef = targetRef;
        return s;
    };

    {
        const juce::ScopedTryLock sl (session.lock);
        if (sl.isLocked())
            return build();
    }
    if (lastSnapshot != nullptr || session.renderRunning())
        return lastSnapshot != nullptr ? lastSnapshot : std::make_shared<const PianoRollSnapshot>();

    const juce::ScopedLock waitSl (session.lock);
    return build();
}

void PianoRollView::refresh()
{
    const auto sp = snapshotPtr();
    if (sp->missing && targetRef.valid())
    {
        clearTarget();
        if (onClose) onClose();
        return;
    }
    selectedNotes.erase (std::remove_if (selectedNotes.begin(), selectedNotes.end(),
                                         [&] (int idx) { return ! juce::isPositiveAndBelow (idx, (int) sp->clip.notes.size()); }),
                         selectedNotes.end());
    syncToolbar (*sp);
    repaint();
}

void PianoRollView::syncToolbar (const PianoRollSnapshot& s)
{
    snapButton.setToggleState (s.snapOn, juce::dontSendNotification);
    const auto gText = juce::String (gridName (s.grid));
    if (gridButton.getButtonText() != gText)
        gridButton.setButtonText (gText);

    if (s.valid)
    {
        const double bars = clipBeats (s) / juce::jmax (0.25, s.tempo.beatsPerBar());
        juce::String barText;
        if (std::abs (bars - std::round (bars)) < 0.005)
        {
            const int n = juce::roundToInt (bars);
            barText = juce::String (n) + (n == 1 ? " bar" : " bars");
        }
        else
        {
            auto num = juce::String (bars, 2);
            while (num.endsWithChar ('0'))
                num = num.dropLastCharacters (1);
            if (num.endsWithChar ('.'))
                num = num.dropLastCharacters (1);
            barText = num + " bars";
        }
        if (lengthLabel.getText() != barText)
            lengthLabel.setText (barText, juce::dontSendNotification);
    }
    else if (lengthLabel.getText().isNotEmpty())
    {
        lengthLabel.setText ({}, juce::dontSendNotification);
    }
}

void PianoRollView::edited()
{
    refresh();
    if (onStructure) onStructure();
}

void PianoRollView::cycleGrid()
{
    const auto sp = snapshotPtr();
    editor.setGrid (nextPianoRollGrid (sp->grid));
    edited();
}

void PianoRollView::showGridMenu()
{
    const auto sp = snapshotPtr();
    juce::PopupMenu menu;
    const Grid choices[] = { Grid::Quarter, Grid::Eighth, Grid::Sixteenth, Grid::ThirtySecond };
    for (int i = 0; i < 4; ++i)
        menu.addItem (i + 1, gridName (choices[i]), true, sp->grid == choices[i]);
    juce::Component::SafePointer<PianoRollView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&gridButton), [safe] (int result)
    {
        if (safe == nullptr || result <= 0) return;
        const Grid choicesInner[] = { Grid::Quarter, Grid::Eighth, Grid::Sixteenth, Grid::ThirtySecond };
        safe->editor.setGrid (choicesInner[result - 1]);
        safe->edited();
    });
}

juce::Rectangle<int> PianoRollView::toolbarBounds() const
{
    return { 0, 0, getWidth(), kToolbarHeight };
}

juce::Rectangle<int> PianoRollView::rulerBounds() const
{
    const int gridH = juce::jmax (0, getHeight() - kToolbarHeight - kRulerHeight - kVelocityHeight);
    juce::ignoreUnused (gridH);
    return { kKeyWidth, kToolbarHeight, juce::jmax (0, getWidth() - kKeyWidth), kRulerHeight };
}

juce::Rectangle<int> PianoRollView::keyboardBounds() const
{
    const int gridH = juce::jmax (0, getHeight() - kToolbarHeight - kRulerHeight - kVelocityHeight);
    return { 0, kToolbarHeight + kRulerHeight, kKeyWidth, gridH };
}

juce::Rectangle<int> PianoRollView::noteGridBounds() const
{
    const int gridH = juce::jmax (0, getHeight() - kToolbarHeight - kRulerHeight - kVelocityHeight);
    return { kKeyWidth, kToolbarHeight + kRulerHeight, juce::jmax (0, getWidth() - kKeyWidth), gridH };
}

juce::Rectangle<int> PianoRollView::velocityLaneBounds() const
{
    const int vy = juce::jmax (kToolbarHeight + kRulerHeight, getHeight() - kVelocityHeight);
    return { kKeyWidth, vy, juce::jmax (0, getWidth() - kKeyWidth), juce::jmax (0, getHeight() - vy) };
}

double PianoRollView::beatToX (double beat) const
{
    return (double) noteGridBounds().getX() + (beat - scrollBeats) * pxPerBeat;
}

double PianoRollView::xToBeat (double x) const
{
    return scrollBeats + (x - (double) noteGridBounds().getX()) / pxPerBeat;
}

int PianoRollView::pitchToY (int pitch) const
{
    return noteGridBounds().getY() + (127 - juce::jlimit (0, 127, pitch)) * kRowHeight - scrollY;
}

int PianoRollView::yToPitch (int y) const
{
    const int row = (int) std::floor ((double) (y - noteGridBounds().getY() + scrollY) / (double) kRowHeight);
    return juce::jlimit (0, 127, 127 - row);
}

int PianoRollView::velToY (int velocity) const
{
    const auto r = velocityLaneBounds();
    const int usableH = juce::jmax (1, r.getHeight() - 12);
    const double norm = juce::jlimit (1, 127, velocity) / 127.0;
    return r.getBottom() - 4 - juce::roundToInt (norm * usableH);
}

int PianoRollView::yToVel (int y) const
{
    const auto r = velocityLaneBounds();
    const int usableH = juce::jmax (1, r.getHeight() - 12);
    const double norm = (double) (r.getBottom() - 4 - y) / (double) usableH;
    return juce::jlimit (1, 127, juce::roundToInt (norm * 127.0));
}

int PianoRollView::maxScrollY() const
{
    return juce::jmax (0, 128 * kRowHeight - noteGridBounds().getHeight());
}

void PianoRollView::centerOnPitch (int pitch)
{
    const int gridH = juce::jmax (kRowHeight, noteGridBounds().getHeight());
    const int target = (127 - juce::jlimit (0, 127, pitch)) * kRowHeight - gridH / 2 + kRowHeight / 2;
    scrollY = juce::jlimit (0, juce::jmax (target, maxScrollY()), juce::jmax (0, target));
}

juce::Rectangle<int> PianoRollView::noteRect (const MidiNote& note) const
{
    const int x0 = juce::roundToInt (beatToX (note.startBeat));
    const int x1 = juce::roundToInt (beatToX (note.startBeat + note.durationBeats));
    const int y = pitchToY (note.pitch) + 1;
    return { x0, y, juce::jmax (4, x1 - x0), kRowHeight - 2 };
}

int PianoRollView::hitNoteAt (const PianoRollSnapshot& s, juce::Point<int> p) const
{
    for (int i = (int) s.clip.notes.size() - 1; i >= 0; --i)
        if (noteRect (s.clip.notes[(size_t) i]).contains (p))
            return i;
    return -1;
}

bool PianoRollView::isSelected (int index) const
{
    return std::find (selectedNotes.begin(), selectedNotes.end(), index) != selectedNotes.end();
}

void PianoRollView::toggleSelection (int index)
{
    auto it = std::find (selectedNotes.begin(), selectedNotes.end(), index);
    if (it != selectedNotes.end())
        selectedNotes.erase (it);
    else
        selectedNotes.push_back (index);
}

void PianoRollView::sendPreviewNoteOn (int pitch, int velocity)
{
    const int p = juce::jlimit (0, 127, pitch);
    if (previewPitch == p)
        return;
    sendPreviewNoteOff();
    const auto sp = snapshotPtr();
    if (! sp->valid || ! sp->isMidiTrack)
        return;
    previewPitch = p;
    session.liveMidiFifo.push (0x90, (std::uint8_t) p, (std::uint8_t) juce::jlimit (1, 127, velocity), 0);
    repaint();
}

void PianoRollView::sendPreviewNoteOff()
{
    if (previewPitch < 0)
        return;
    session.liveMidiFifo.push (0x80, (std::uint8_t) previewPitch, 0, 0);
    previewPitch = -1;
    repaint();
}

void PianoRollView::addNoteForTest (double startBeat, double durationBeats, int pitch, int velocity)
{
    if (! targetRef.valid()) return;
    juce::String error;
    const int idx = editor.addMidiNote (targetRef.track, targetRef.clip, startBeat, durationBeats, pitch, velocity, error);
    if (idx >= 0)
        selectedNotes = { idx };
    else
        report (error);
    edited();
}

juce::Rectangle<int> PianoRollView::noteBoundsForTest (int noteIndex) const
{
    const auto sp = snapshotPtr();
    if (! sp->valid || ! juce::isPositiveAndBelow (noteIndex, (int) sp->clip.notes.size()))
        return {};
    return noteRect (sp->clip.notes[(size_t) noteIndex]);
}

double PianoRollView::clipBeats (const PianoRollSnapshot& s) const
{
    if (! s.valid || s.tempo.sampleRate <= 0.0)
        return 4.0;
    const double b0 = s.tempo.samplesToBeats ((double) s.clip.start);
    const double b1 = s.tempo.samplesToBeats ((double) s.clip.end());
    return juce::jmax (0.25, b1 - b0);
}

int PianoRollView::clipEndXForTest() const
{
    const auto sp = snapshotPtr();
    if (! sp->valid)
        return -1;
    return juce::roundToInt (beatToX (clipBeats (*sp)));
}

juce::Point<int> PianoRollView::pianoKeyCenterForTest (int pitch) const
{
    const auto kb = keyboardBounds();
    return { kb.getCentreX(), pitchToY (pitch) + kRowHeight / 2 };
}

void PianoRollView::resized()
{
    const int w = getWidth();
    const int bh = 20;
    const int by = (kToolbarHeight - bh) / 2;
    closeButton.setBounds (w - 64, by, 56, bh);
    quantizeButton.setBounds (closeButton.getX() - 78, by, 72, bh);
    gridButton.setBounds (quantizeButton.getX() - 62, by, 56, bh);
    snapButton.setBounds (gridButton.getX() - 60, by, 54, bh);
    lengthPlusButton.setBounds (snapButton.getX() - 32, by, 24, bh);
    lengthLabel.setBounds (lengthPlusButton.getX() - 62, by, 60, bh);
    lengthMinusButton.setBounds (lengthLabel.getX() - 26, by, 24, bh);

    if (! centeredOnce && noteGridBounds().getHeight() > 0)
    {
        centerOnPitch (60);
        centeredOnce = true;
    }
    scrollY = juce::jlimit (0, maxScrollY(), scrollY);
}

void PianoRollView::paint (juce::Graphics& g)
{
    const auto sp = snapshotPtr();
    const auto& s = *sp;
    const auto bounds = getLocalBounds();

    g.fillAll (colours::background);

    // Top toolbar
    const auto tb = toolbarBounds();
    ui::hw::fillMetal (g, tb.toFloat(), colours::panel);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRect (0, tb.getBottom() - 1, getWidth(), 1);

    const auto title = s.valid ? (s.trackName + "  \xe2\x80\x94  " + s.clip.name) : juce::String ("Piano Roll");
    ui::hw::drawLabel (g, title, { 10, 0, juce::jmax (60, lengthMinusButton.getX() - 18), kToolbarHeight },
                       juce::Justification::centredLeft, s.valid ? colours::text : colours::dim, 11.0f);

    const auto ruler = rulerBounds();
    const auto kb = keyboardBounds();
    const auto grid = noteGridBounds();
    const auto vel = velocityLaneBounds();

    // Left corner above piano keys
    ui::hw::fillMetal (g, juce::Rectangle<float> (0.0f, (float) kToolbarHeight, (float) kKeyWidth, (float) kRulerHeight),
                       colours::panel.darker (0.15f));
    ui::hw::drawLabel (g, "PITCH", { 4, kToolbarHeight, kKeyWidth - 8, kRulerHeight }, juce::Justification::centred, colours::dim, 9.0f);

    // Beat ruler
    g.setColour (colours::panel.darker (0.25f));
    g.fillRect (ruler);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRect (ruler.getX(), ruler.getBottom() - 1, ruler.getWidth(), 1);

    // Note grid background & rows
    g.setColour (colours::glass);
    g.fillRect (grid);
    if (grid.getHeight() > 0 && grid.getWidth() > 0)
    {
        g.saveState();
        g.reduceClipRegion (grid);

        for (int p = 127; p >= 0; --p)
        {
            const int y = pitchToY (p);
            if (y + kRowHeight < grid.getY() || y > grid.getBottom())
                continue;
            const bool black = isBlackKey (p);
            g.setColour (black ? colours::background.darker (0.35f) : colours::glass.brighter (0.04f));
            g.fillRect (grid.getX(), y, grid.getWidth(), kRowHeight);
            g.setColour ((p % 12 == 0) ? colours::panelEdge.withAlpha (0.85f) : colours::glassGrid.withAlpha (0.45f));
            g.drawHorizontalLine (y + kRowHeight - 1, (float) grid.getX(), (float) grid.getRight());
        }

        // Shade area past clip end
        const int clipEndX = juce::roundToInt (beatToX (clipBeats (s)));
        if (clipEndX < grid.getRight())
        {
            const int sx = juce::jmax (grid.getX(), clipEndX);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (sx, grid.getY(), grid.getRight() - sx, grid.getHeight());
        }
        if (s.valid)
        {
            g.setColour (colours::amber.withAlpha (0.85f));
            g.fillRect (clipEndX - 1, grid.getY(), 2, grid.getHeight());
        }

        // Grid lines
        const double step = juce::jmax (0.125, s.tempo.gridBeats (s.grid));
        double drawStep = step;
        while (drawStep * pxPerBeat < 8.0)
            drawStep *= 2.0;
        const double bpb = s.tempo.beatsPerBar();
        const double startBeat = juce::jmax (0.0, scrollBeats);
        const double endBeat = xToBeat (grid.getRight());
        for (double b = std::floor (startBeat / drawStep) * drawStep; b <= endBeat + drawStep; b += drawStep)
        {
            if (b < 0.0) continue;
            const int x = juce::roundToInt (beatToX (b));
            if (x < grid.getX() || x > grid.getRight()) continue;
            const bool isBar = std::abs (std::fmod (b / bpb + 1.0e-9, 1.0)) < 1.0e-6;
            const bool isBeat = std::abs (b - std::round (b)) < 1.0e-6;
            g.setColour (colours::dim.withAlpha (isBar ? 0.42f : isBeat ? 0.24f : 0.11f));
            g.drawVerticalLine (x, (float) grid.getY(), (float) grid.getBottom());
        }

        // Notes
        if (s.valid)
        {
            for (int i = 0; i < (int) s.clip.notes.size(); ++i)
            {
                const auto& note = s.clip.notes[(size_t) i];
                const auto nr = noteRect (note);
                if (nr.getRight() < grid.getX() || nr.getX() > grid.getRight()
                    || nr.getBottom() < grid.getY() || nr.getY() > grid.getBottom())
                    continue;
                const bool sel = isSelected (i);
                const float velAlpha = 0.55f + 0.45f * ((float) juce::jlimit (1, 127, note.velocity) / 127.0f);
                const auto fill = (sel ? colours::amber : colours::accent).withAlpha (velAlpha);
                g.setColour (fill);
                g.fillRoundedRectangle (nr.toFloat(), 2.5f);
                g.setColour (sel ? colours::text : colours::accent.brighter (0.3f));
                g.drawRoundedRectangle (nr.toFloat().reduced (0.5f), 2.5f, sel ? 1.5f : 1.0f);
                if (nr.getWidth() >= 30)
                {
                    g.setFont (ui::labelFont (8.5f));
                    g.setColour (colours::lampInk);
                    g.drawText (pitchName (note.pitch), nr.reduced (3, 0), juce::Justification::centredLeft, false);
                }
            }
        }
        g.restoreState();
    }

    // Ruler markings
    if (ruler.getWidth() > 0)
    {
        g.saveState();
        g.reduceClipRegion (ruler);
        const double bpb = s.tempo.beatsPerBar();
        const double startBeat = juce::jmax (0.0, scrollBeats);
        const double endBeat = xToBeat (ruler.getRight());
        g.setFont (ui::labelFont (9.5f));
        for (double b = std::floor (startBeat); b <= endBeat + 1.0; b += 1.0)
        {
            if (b < 0.0) continue;
            const int x = juce::roundToInt (beatToX (b));
            if (x < ruler.getX() || x > ruler.getRight()) continue;
            const bool isBar = std::abs (std::fmod (b / bpb + 1.0e-9, 1.0)) < 1.0e-6;
            g.setColour (colours::dim.withAlpha (isBar ? 0.85f : 0.4f));
            g.drawVerticalLine (x, (float) (ruler.getBottom() - (isBar ? 12 : 6)), (float) ruler.getBottom());
            if (isBar)
                g.drawText (juce::String ((int) std::floor (b / bpb + 1.0e-9) + 1), x + 3, ruler.getY() + 2, 36, ruler.getHeight() - 4, juce::Justification::centredLeft);
            else if (pxPerBeat >= 32.0)
                g.drawText (juce::String ((int) std::fmod (std::round (b), bpb) + 1), x + 3, ruler.getY() + 4, 24, ruler.getHeight() - 6, juce::Justification::centredLeft);
        }
        if (s.valid)
        {
            const int clipEndX = juce::roundToInt (beatToX (clipBeats (s)));
            g.setColour (colours::amber);
            g.fillRect (clipEndX - 1, ruler.getY(), 2, ruler.getHeight());
        }
        g.restoreState();
    }

    // Piano keyboard on left
    if (kb.getHeight() > 0)
    {
        g.saveState();
        g.reduceClipRegion (kb);
        g.setColour (colours::panel);
        g.fillRect (kb);
        for (int p = 127; p >= 0; --p)
        {
            const int y = pitchToY (p);
            if (y + kRowHeight < kb.getY() || y > kb.getBottom())
                continue;
            const bool black = isBlackKey (p);
            const bool active = (previewPitch == p);
            const auto keyColour = active ? colours::amber : (black ? colours::background.darker (0.55f) : colours::panel.brighter (0.30f));
            g.setColour (keyColour);
            g.fillRect (kb.getX(), y, kb.getWidth() - 1, kRowHeight - 1);
            if (black && ! active)
            {
                g.setColour (colours::panel.brighter (0.22f));
                g.fillRect (kb.getRight() - 16, y, 15, kRowHeight - 1);
            }
            g.setColour (colours::panelEdge);
            g.drawHorizontalLine (y + kRowHeight - 1, (float) kb.getX(), (float) kb.getRight());
            if (p % 12 == 0)
            {
                g.setFont (ui::labelFont (8.5f));
                g.setColour (active ? colours::lampInk : colours::text);
                g.drawText (pitchName (p), kb.getX() + 3, y, kb.getWidth() - 6, kRowHeight, juce::Justification::centredLeft);
            }
        }
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.fillRect (kb.getRight() - 1, kb.getY(), 1, kb.getHeight());
        g.restoreState();
    }

    // Velocity lane gutter & graph
    const int velTop = vel.getY();
    ui::hw::fillMetal (g, juce::Rectangle<float> (0.0f, (float) velTop, (float) kKeyWidth, (float) vel.getHeight()),
                       colours::panel.darker (0.15f));
    ui::hw::drawLabel (g, "VEL", { 4, velTop, kKeyWidth - 8, vel.getHeight() }, juce::Justification::centred, colours::dim, 9.5f);
    g.setColour (colours::panelEdge);
    g.fillRect (0, velTop, bounds.getWidth(), 1);

    g.setColour (colours::glass);
    g.fillRect (vel);
    if (vel.getWidth() > 0 && vel.getHeight() > 0)
    {
        g.saveState();
        g.reduceClipRegion (vel);
        g.setColour (colours::glassGrid.withAlpha (0.5f));
        g.drawHorizontalLine (velToY (64), (float) vel.getX(), (float) vel.getRight());
        g.drawHorizontalLine (velToY (127), (float) vel.getX(), (float) vel.getRight());

        if (s.valid)
        {
            for (int i = 0; i < (int) s.clip.notes.size(); ++i)
            {
                const auto& note = s.clip.notes[(size_t) i];
                const int cx = juce::roundToInt (beatToX (note.startBeat));
                if (cx < vel.getX() - 8 || cx > vel.getRight() + 8)
                    continue;
                const int vy = velToY (note.velocity);
                const bool sel = isSelected (i);
                const auto c = sel ? colours::amber : colours::accent;
                g.setColour (c.withAlpha (0.85f));
                g.drawVerticalLine (cx, (float) vy, (float) (vel.getBottom() - 4));
                g.fillRect (cx - 1, vy, 3, juce::jmax (2, vel.getBottom() - 4 - vy));
                g.setColour (c);
                g.fillEllipse ((float) cx - 4.0f, (float) vy - 4.0f, 8.0f, 8.0f);
                if (sel)
                {
                    g.setColour (colours::text);
                    g.drawEllipse ((float) cx - 4.0f, (float) vy - 4.0f, 8.0f, 8.0f, 1.0f);
                }
            }
        }
        g.restoreState();
    }
}

void PianoRollView::mouseMove (const juce::MouseEvent& e)
{
    hoverPos = e.getPosition();
}

juce::MouseCursor PianoRollView::getMouseCursor()
{
    if (mode == Mode::resizeNote || mode == Mode::resizeClipEnd)
        return juce::MouseCursor::LeftRightResizeCursor;
    if (mode == Mode::velocity)
        return juce::MouseCursor::UpDownResizeCursor;
    const auto ruler = rulerBounds();
    const auto grid = noteGridBounds();
    if (ruler.contains (hoverPos) || grid.contains (hoverPos))
    {
        const auto sp = snapshotPtr();
        if (sp->valid)
        {
            if (grid.contains (hoverPos))
            {
                const int hit = hitNoteAt (*sp, hoverPos);
                if (hit >= 0)
                {
                    const auto r = noteRect (sp->clip.notes[(size_t) hit]);
                    if (r.getRight() - hoverPos.x <= kEdgeGrab && hoverPos.x >= r.getX() + r.getWidth() / 2)
                        return juce::MouseCursor::LeftRightResizeCursor;
                    return juce::MouseCursor::NormalCursor;
                }
            }
            const int clipEndX = juce::roundToInt (beatToX (clipBeats (*sp)));
            if (std::abs (hoverPos.x - clipEndX) <= kEdgeGrab)
                return juce::MouseCursor::LeftRightResizeCursor;
        }
    }
    return juce::MouseCursor::NormalCursor;
}

void PianoRollView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    const auto grid = noteGridBounds();
    const auto p = e.getPosition();
    if (! grid.contains (p) || ! targetRef.valid())
        return;

    const auto sp = snapshotPtr();
    if (! sp->valid) return;
    if (hitNoteAt (*sp, p) >= 0)
        return;

    const double step = juce::jmax (0.125, sp->tempo.gridBeats (sp->grid));
    double beat = juce::jmax (0.0, xToBeat (p.x));
    if (sp->snapOn)
        beat = juce::jmax (0.0, std::floor (beat / step + TempoMap::kGridEps) * step);
    const int pitch = yToPitch (p.y);
    juce::String error;
    const int idx = editor.addMidiNote (targetRef.track, targetRef.clip, beat, step, pitch, 100, error);
    if (idx >= 0)
        selectedNotes = { idx };
    else
        report (error);
    edited();
}

void PianoRollView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    sendPreviewNoteOff();
    endGesture();
    dragSourceIndex = e.source.getIndex();
    const auto p = e.getPosition();
    hoverPos = p;
    pressX = p.x;
    pressY = p.y;
    mode = Mode::none;

    if (e.mods.isPopupMenu())
        return;

    const auto kb = keyboardBounds();
    if (kb.contains (p))
    {
        mode = Mode::previewKey;
        sendPreviewNoteOn (yToPitch (p.y));
        return;
    }

    const auto sp = snapshotPtr();
    if (! sp->valid)
        return;

    const auto vel = velocityLaneBounds();
    if (vel.contains (p))
    {
        mode = Mode::velocity;
        beginGesture ("Edit MIDI velocity");
        lastVelX = p.x;
        lastVel = yToVel (p.y);
        applyVelocitySweep (*sp, p.x, lastVel, p.x, lastVel);
        refresh();
        if (onStructure) onStructure();
        return;
    }

    const auto ruler = rulerBounds();
    const auto grid = noteGridBounds();
    const int clipEndX = juce::roundToInt (beatToX (clipBeats (*sp)));
    if (ruler.contains (p) && std::abs (p.x - clipEndX) <= kEdgeGrab)
    {
        mode = Mode::resizeClipEnd;
        beginGesture ("Resize MIDI clip");
        return;
    }

    if (grid.contains (p))
    {
        const int hit = hitNoteAt (*sp, p);
        if (hit >= 0)
        {
            if (e.mods.isShiftDown())
                toggleSelection (hit);
            else if (! isSelected (hit))
                selectedNotes = { hit };

            if (isSelected (hit))
            {
                const auto& n = sp->clip.notes[(size_t) hit];
                const auto nr = noteRect (n);
                dragNote = hit;
                origStartBeat = n.startBeat;
                origDuration = n.durationBeats;
                origPitch = n.pitch;
                pressPitch = yToPitch (p.y);

                if (nr.getRight() - p.x <= kEdgeGrab && p.x >= nr.getX() + nr.getWidth() / 2)
                {
                    mode = Mode::resizeNote;
                    beginGesture ("Resize MIDI note");
                }
                else
                {
                    mode = Mode::moveNote;
                    beginGesture ("Move MIDI note");
                }
            }
            repaint();
            return;
        }
        if (std::abs (p.x - clipEndX) <= kEdgeGrab)
        {
            mode = Mode::resizeClipEnd;
            beginGesture ("Resize MIDI clip");
            return;
        }
        if (! e.mods.isShiftDown())
            selectedNotes.clear();
        repaint();
    }
}

void PianoRollView::mouseDrag (const juce::MouseEvent& e)
{
    if (e.source.getIndex() != dragSourceIndex) return;
    const auto p = e.getPosition();
    hoverPos = p;

    if (mode == Mode::previewKey)
    {
        if (keyboardBounds().contains (p))
            sendPreviewNoteOn (yToPitch (p.y));
        return;
    }

    if (mode == Mode::none || ! targetRef.valid())
        return;

    const auto sp = snapshotPtr();
    if (! sp->valid) return;
    juce::String error;

    if (mode == Mode::moveNote)
    {
        const double deltaBeats = (double) (p.x - pressX) / pxPerBeat;
        const int deltaPitch = yToPitch (p.y) - pressPitch;
        const double rawStart = juce::jmax (0.0, origStartBeat + deltaBeats);
        const double step = juce::jmax (0.125, sp->tempo.gridBeats (sp->grid));
        const double targetStart = sp->snapOn
            ? juce::jmax (0.0, std::floor (rawStart / step + 0.5 + TempoMap::kGridEps) * step)
            : rawStart;
        const int targetPitch = juce::jlimit (0, 127, origPitch + deltaPitch);
        if (! editor.moveMidiNote (targetRef.track, targetRef.clip, dragNote, targetStart, targetPitch, error))
            report (error);
        refresh();
        if (onStructure) onStructure();
    }
    else if (mode == Mode::resizeNote)
    {
        const double deltaBeats = (double) (p.x - pressX) / pxPerBeat;
        const double step = juce::jmax (0.125, sp->tempo.gridBeats (sp->grid));
        const double rawEnd = origStartBeat + origDuration + deltaBeats;
        double targetDur = juce::jmax (0.05, origDuration + deltaBeats);
        if (sp->snapOn)
        {
            const double snappedEnd = std::floor (rawEnd / step + 0.5 + TempoMap::kGridEps) * step;
            targetDur = juce::jmax (step, snappedEnd - origStartBeat);
        }
        if (! editor.resizeMidiNote (targetRef.track, targetRef.clip, dragNote, targetDur, error))
            report (error);
        refresh();
        if (onStructure) onStructure();
    }
    else if (mode == Mode::resizeClipEnd)
    {
        const double step = juce::jmax (0.125, sp->tempo.gridBeats (sp->grid));
        const double rawBeat = juce::jmax (0.25, xToBeat (p.x));
        const double targetBeat = sp->snapOn
            ? juce::jmax (step, std::floor (rawBeat / step + 0.5 + TempoMap::kGridEps) * step)
            : rawBeat;
        const double clipStartBeat = sp->tempo.samplesToBeats ((double) sp->clip.start);
        const auto endSample = (juce::int64) std::llround (sp->tempo.beatsToSamples (clipStartBeat + targetBeat));
        const auto newLen = juce::jmax ((juce::int64) 1, endSample - sp->clip.start);
        if (! editor.resizeMidiClip (targetRef.track, targetRef.clip, newLen, error))
            report (error);
        refresh();
        if (onStructure) onStructure();
    }
    else if (mode == Mode::velocity)
    {
        const int curVel = yToVel (p.y);
        applyVelocitySweep (*sp, lastVelX, lastVel, p.x, curVel);
        lastVelX = p.x;
        lastVel = curVel;
        refresh();
        if (onStructure) onStructure();
    }
}

void PianoRollView::mouseUp (const juce::MouseEvent& e)
{
    if (e.source.getIndex() != dragSourceIndex) return;
    dragSourceIndex = -1;
    if (mode == Mode::previewKey)
        sendPreviewNoteOff();
    const bool wasEdit = (mode == Mode::moveNote || mode == Mode::resizeNote || mode == Mode::resizeClipEnd || mode == Mode::velocity);
    mode = Mode::none;
    dragNote = -1;
    endGesture();
    if (wasEdit)
        edited();
}

void PianoRollView::applyVelocitySweep (const PianoRollSnapshot& s, int x0, int v0, int x1, int v1)
{
    const int xMin = juce::jmin (x0, x1) - 6;
    const int xMax = juce::jmax (x0, x1) + 6;
    const int dx = x1 - x0;
    juce::String error;
    for (int i = 0; i < (int) s.clip.notes.size(); ++i)
    {
        const int cx = juce::roundToInt (beatToX (s.clip.notes[(size_t) i].startBeat));
        if (cx < xMin || cx > xMax)
            continue;
        int vel = v1;
        if (std::abs (dx) >= 2)
        {
            const double t = juce::jlimit (0.0, 1.0, (double) (cx - x0) / (double) dx);
            vel = juce::jlimit (1, 127, juce::roundToInt ((double) v0 + ((double) v1 - (double) v0) * t));
        }
        if (! editor.setMidiNoteVelocity (targetRef.track, targetRef.clip, i, vel, error))
            report (error);
    }
}

void PianoRollView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const int anchorX = juce::jmax (noteGridBounds().getX(), e.x);
        const double anchorBeat = juce::jmax (0.0, xToBeat (anchorX));
        pxPerBeat = juce::jlimit (kMinPxPerBeat, kMaxPxPerBeat, pxPerBeat * std::pow (2.0, wheel.deltaY * 3.0));
        scrollBeats = juce::jmax (0.0, anchorBeat - (double) (anchorX - noteGridBounds().getX()) / pxPerBeat);
    }
    else if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
    {
        const double delta = (! e.mods.isShiftDown() && std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
            ? wheel.deltaX : (std::abs (wheel.deltaY) > 0.0f ? wheel.deltaY : wheel.deltaX);
        scrollBeats = juce::jmax (0.0, scrollBeats - delta * 2.0);
    }
    else
    {
        scrollY = juce::jlimit (0, maxScrollY(), scrollY - juce::roundToInt (wheel.deltaY * 180.0));
    }
    repaint();
}

bool PianoRollView::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        if (! targetRef.valid())
            return false;
        if (selectedNotes.empty())
            return true;
        std::sort (selectedNotes.begin(), selectedNotes.end(), std::greater<int>());
        selectedNotes.erase (std::unique (selectedNotes.begin(), selectedNotes.end()), selectedNotes.end());
        beginGesture ("Delete MIDI note");
        juce::String error;
        for (int idx : selectedNotes)
            if (! editor.deleteMidiNote (targetRef.track, targetRef.clip, idx, error))
                report (error);
        endGesture();
        selectedNotes.clear();
        edited();
        return true;
    }
    if (code == juce::KeyPress::escapeKey)
    {
        selectedNotes.clear();
        repaint();
        return true;
    }
    return false;
}
} // namespace olympus::gui
