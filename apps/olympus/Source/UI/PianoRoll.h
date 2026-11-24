// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "LampButton.h"
#include "../Edit.h"
#include "../Session.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

#include <functional>
#include <memory>
#include <vector>

namespace olympus::gui
{
struct PianoRollSnapshot;

// Piano roll & velocity lane editor for one MIDI clip on a track.
// Reads Session state non-blockingly (try-lock + revision cache; never waits while a render runs) and routes every edit through Editor.
class PianoRollView : public juce::Component
{
public:
    static constexpr int kToolbarHeight = 28;
    static constexpr int kRulerHeight = 22;
    static constexpr int kKeyWidth = 54;
    static constexpr int kVelocityHeight = 64;
    static constexpr int kRowHeight = 14;
    static constexpr double kDefaultPxPerBeat = 80.0;

    PianoRollView (Session& session, Editor& editor);
    ~PianoRollView() override;

    void setTarget (MidiClipRef ref);
    void clearTarget();
    MidiClipRef target() const { return targetRef; }
    void refresh();

    std::function<void()> onClose;
    std::function<void()> onStructure;
    std::function<void (const juce::String&)> reportError;

    double playhead = 0.0;

    // Toolbar controls (public for UI checks)
    juce::TextButton scaleButton { "Scale" };
    juce::TextButton toolsButton { "Tools" };
    juce::TextButton lengthMinusButton { "-" };
    juce::Label lengthLabel;
    juce::TextButton lengthPlusButton { "+" };
    LampButton snapButton { "Snap" };
    juce::TextButton gridButton { "1/4" };
    juce::TextButton quantizeButton { "Quantize" };
    juce::TextButton closeButton { "Close" };

    // Test helpers for headless UI checks
    void addNoteForTest (double startBeat, double durationBeats, int pitch, int velocity = 100);
    juce::Rectangle<int> noteBoundsForTest (int noteIndex) const;
    juce::Rectangle<int> velocityLaneBoundsForTest() const { return velocityLaneBounds(); }
    juce::Rectangle<int> noteGridBoundsForTest() const { return noteGridBounds(); }
    juce::Point<int> pianoKeyCenterForTest (int pitch) const;
    int selectedNotesCountForTest() const { return (int) selectedNotes.size(); }
    int clipEndXForTest() const;
    juce::String lengthTextForTest() const { return lengthLabel.getText(); }
    void setScaleForTest (int scaleIndex);
    int activeScaleForTest() const { return activeScaleIndex; }
    void setScaleLockForTest (bool lock);
    bool isScaleLockForTest() const { return scaleLock; }
    void setGhostNotesForTest (bool show);
    bool isGhostNotesForTest() const { return showGhostNotes; }
    int ghostNotesCountForTest() const;
    void runToolsActionForTest (int actionId);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    juce::MouseCursor getMouseCursor() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    enum class Mode { none, moveNote, resizeNote, resizeClipEnd, velocity, previewKey, marquee };

    struct OrigNote
    {
        int index = -1;
        double startBeat = 0.0;
        double durationBeats = 1.0;
        int pitch = 60;
    };

    Session& session;
    Editor& editor;
    MidiClipRef targetRef;

    mutable std::shared_ptr<const PianoRollSnapshot> lastSnapshot;
    mutable juce::uint64 lastSnapshotRevision = 0;
    mutable MidiClipRef lastSnapshotRef;

    double pxPerBeat = kDefaultPxPerBeat;
    double scrollBeats = 0.0;
    int scrollY = 0;
    bool centeredOnce = false;

    std::vector<int> selectedNotes;
    std::vector<OrigNote> origNotes;
    Mode mode = Mode::none;
    int dragNote = -1;
    int pressX = 0, pressY = 0;
    double origStartBeat = 0.0;
    double origDuration = 1.0;
    int origPitch = 60;
    int pressPitch = 60;
    double appliedDeltaBeats = 0.0;
    int appliedDeltaPitch = 0;
    double appliedDeltaDur = 0.0;
    juce::Point<int> marqueeOrigin;
    juce::Rectangle<int> marqueeRect;
    std::vector<int> marqueeInitialSelection;
    std::vector<MidiNote> copiedNotes;
    int activeScaleIndex = 0;
    bool scaleLock = false;
    bool showGhostNotes = false;
    int lastVelX = 0;
    int lastVel = 100;
    int previewPitch = -1;
    int dragSourceIndex = -1;
    juce::Point<int> hoverPos { -1, -1 };
    std::unique_ptr<Gesture> gesture;

    std::shared_ptr<const PianoRollSnapshot> snapshotPtr() const;
    double clipBeats (const PianoRollSnapshot& s) const;
    juce::Rectangle<int> toolbarBounds() const;
    juce::Rectangle<int> rulerBounds() const;
    juce::Rectangle<int> keyboardBounds() const;
    juce::Rectangle<int> noteGridBounds() const;
    juce::Rectangle<int> velocityLaneBounds() const;

    double beatToX (double beat) const;
    double xToBeat (double x) const;
    int pitchToY (int pitch) const;
    int yToPitch (int y) const;
    int velToY (int velocity) const;
    int yToVel (int y) const;
    int maxScrollY() const;
    void centerOnPitch (int pitch);

    juce::Rectangle<int> noteRect (const MidiNote& note) const;
    int hitNoteAt (const PianoRollSnapshot& s, juce::Point<int> p) const;
    bool isSelected (int index) const;
    void toggleSelection (int index);
    void applyVelocitySweep (const PianoRollSnapshot& s, int x0, int v0, int x1, int v1);

    void sendPreviewNoteOn (int pitch, int velocity = 100);
    void sendPreviewNoteOff();
    void cycleGrid();
    void showGridMenu();
    void showScaleMenu();
    void showToolsMenu (juce::Component* target, juce::Point<int> localPoint);
    void applyScaleIndex (int index);
    int snapPitchToScale (int pitch) const;
    int stepPitchInScale (int pitch, int direction) const;
    void runToolsAction (int actionId);
    void selectAllNotes();
    void duplicateSelectedNotes();
    void copySelectedNotes();
    void pasteNotes();
    void syncToolbar (const PianoRollSnapshot& s);
    void edited();
    void beginGesture (const juce::String& label) { gesture = std::make_unique<Gesture> (editor, label); }
    void endGesture() { gesture.reset(); }
    void report (const juce::String& error) { if (reportError && error.isNotEmpty()) reportError (error); }
};
} // namespace olympus::gui
