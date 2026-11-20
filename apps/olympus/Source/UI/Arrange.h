// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../Edit.h"
#include "../Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <pantheon/ui/HardwareLookAndFeel.h>
#include <pantheon/ui/Aspect.h>

#include <functional>
#include <memory>

namespace olympus::gui
{
struct ArrangeSnapshot; // the model copied out of the session for one paint or one mouse event (Arrange.cpp)
struct ArrangeWaves;    // the min/max peaks of the audio files, per block (Arrange.cpp)
class ArrangeTrackHeader;

// The arrangement: the timeline of the session. A column of track headers on the left (name, M, S, FX, remove, pan, fader, meter);
// above the lanes a ruler with bars, beats, grid lines and tempo flags, a LOOP strip and a marker lane; then one lane per track with
// its clips (waveforms, fades, a missing-audio look), the playhead, and the loop shaded across the lanes.
//
// Every edit goes through the Editor (one undo step per drag, a gesture), and the model is read through a copy taken under
// Session::lock: nothing holds the lock while it is drawn. The view changes nothing the window does not ask for: it reports what the
// user did through its callbacks (the window owns the transport, the files, the track menus and the plugin windows).
class ArrangeView : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    static constexpr int kHeaderWidth = 260;   // the track column on the left
    static constexpr int kRulerHeight = 40;    // bars and beats
    static constexpr int kLoopHeight = 22;     // the LOOP strip
    static constexpr int kMarkerHeight = 26;   // the marker lane
    static constexpr int kLaneHeight = 92;     // one track
    static constexpr int kDesignWidth = 1280, kDesignHeight = ui::editorHeightFor (kDesignWidth);
    static constexpr double kDefaultZoom = 80.0; // pixels per second

    ArrangeView (Session& session, Editor& editor);
    ~ArrangeView() override;

    // ---- what the window sets and reads ---------------------------------------------------------------------------------------
    double playhead = 0.0;          // samples; the window sets it from the transport (a seek here also moves it)
    ClipRef selected;               // the clip the keys and the menu act on; invalid when none
    double zoom = kDefaultZoom;     // pixels per second, kept within 0.05 .. 4000
    bool followPlayhead = true;     // refresh() scrolls when the playhead moves out of view

    // ---- callbacks ------------------------------------------------------------------------------------------------------------
    std::function<void (juce::int64)> onSeek;                                  // the ruler, the marker lane or a drag there moved the playhead
    std::function<void()> onStructure;                                         // after every change made here (an edit, the mix, markers, loop, tempo)
    std::function<void (int)> onTrackMenu;                                     // the FX button of a track, or right-click on its header
    std::function<void (int)> onRemoveTrack;                                   // the X of a track: the window removes it (closes its plugin windows first)
    std::function<void (const juce::String&)> reportError;                     // an edit the Editor refused, with its reason
    // Audio files dropped on the lanes: the track under the drop, or -1 when it is below the last track, and the sample it lands on.
    std::function<void (const juce::StringArray& files, int track, juce::int64 at)> onFilesDropped;
    // The IN button of a track: the window shows the device's inputs and sets the track's input (Session::setTrackInput).
    std::function<void (int track)> onInputMenu;
    // The input a track records, the peak it reads from that input (linear; reading resets it), and whether the meters show it: the
    // window sets these (the view knows no device). An armed track's meter shows its input when showInputLevels is on.
    std::function<float (int track)> inputLevel;
    bool showInputLevels = false;
    // While a recording runs: where it began and where it is now (samples; -1 when none), and the armed tracks it records on.
    double recordingFrom = -1.0, recordingTo = -1.0;
    std::vector<int> recordingTracks;
    // Shared with the window when present: true while an askText box (or a window chooser) is open, so NEW/OPEN/Quit wait.
    bool* dialogOpen = nullptr;

    // ---- the view -------------------------------------------------------------------------------------------------------------
    // Takes the model as it is now: the meters are read here (the peak since the last read), the headers follow, and the playhead
    // scrolls into view when it moved out of it (with followPlayhead). The window calls it on its timer.
    void refresh();
    void zoomToFit();                                   // the whole session across the lanes, from the start
    void scrollTo (juce::int64 sample);                 // the left edge of the timeline

    // ---- editing from the keyboard and the window (each one is a single undo step) ---------------------------------------------
    // The window's keys call these (S, Delete, Cmd+D, M); keyPressed takes only the selection and the nudges.
    void splitAtPlayhead();      // the selected clip if the playhead is inside it, else every clip the playhead is inside
    void deleteSelected();
    void duplicateSelected();
    void addMarkerAtPlayhead();
    // The clipboard keys (Cmd+C, Cmd+X, Cmd+V) and the empty lane's menu. A paste is refused while a take runs (recordingFrom >= 0).
    void copySelected();                              // Cmd+C: nothing when no clip is selected
    void cutSelected();                               // Cmd+X: one undo step; the selection goes
    void pasteAtPlayhead();                           // Cmd+V: on the selected clip's track, else the clipboard's; the new clip is selected
    void pasteAt (int track, juce::int64 at);         // the empty lane's "Paste here": the clicked track and sample

    // The label of a track's input: "IN 1" for a mono input, "IN 1+2" for a pair (inputs are numbered from 1 here).
    static juce::String inputText (int left, int right);

    // The header's ARM and IN buttons and fader (the checks press and read them; nullptr when there is no such track).
    juce::Button* armButton (int track);
    juce::Button* inputButton (int track);
    juce::Slider* faderSlider (int track);
    bool isInteracting() const;

    // Checks only: drive an open askText box (clip gain, rename clip, rename marker) without clicking its native modal buttons.
    bool hasOpenDialog() const { return activeDialog != nullptr || (bool) pendingDialogEnd; }
    void openClipActionForTest (ClipRef ref, int action) { clipAction (ref, action); }
    void openMarkerActionForTest (int index, int action) { markerAction (index, action); }
    void finishOpenDialogForTest (bool accept, const juce::String& text = {});

    // ---- geometry (view coordinates, for the checks and the window) -------------------------------------------------------------
    ClipRef clipAt (juce::Point<int> point) const;       // the clip under the point, invalid when none
    juce::Rectangle<int> clipBounds (ClipRef ref) const; // where the clip is drawn; empty when the reference is not a clip

    // ---- juce::Component ------------------------------------------------------------------------------------------------------
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed (const juce::KeyPress& key) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    enum class Mode { none, ruler, loop, marker, move, trimStart, trimEnd, fadeIn, fadeOut };
    friend class ArrangeTrackHeader; // the header column reads and edits through the view's Editor and callbacks

    Session& session;
    Editor& editor;
    HardwareLookAndFeel look;                       // the sliders' look (declared first: the headers use it)
    std::unique_ptr<ArrangeWaves> waves;
    juce::Component headerPort;                         // clips the track headers to the lanes
    std::vector<std::unique_ptr<ArrangeTrackHeader>> headers;
    std::unique_ptr<Gesture> gesture;                   // the undo step of the drag in progress

    Mode mode = Mode::none;
    int pressX = 0, pressY = 0;
    juce::int64 anchor = 0;                             // a loop drag: where it began (snapped)
    ClipRef dragClip;
    juce::int64 origStart = 0, origEnd = 0, origFadeIn = 0, origFadeOut = 0;
    juce::int64 origMarker = 0;
    int dragMarker = -1;
    double scrollSamples = 0.0;                         // the timeline's left edge, in samples
    int scrollY = 0;                                    // the lanes' top, in pixels
    double lastPlayhead = 0.0;
    bool hasRefreshed = false;
    std::shared_ptr<const ArrangeSnapshot> lastRefreshSnapshot;
    double lastRefreshScrollSamples = 0.0;
    int lastRefreshScrollY = 0;
    double lastRefreshZoom = kDefaultZoom;
    ClipRef lastRefreshSelected;
    double lastRefreshRecFrom = -1.0, lastRefreshRecTo = -1.0;
    std::vector<int> lastRefreshRecTracks;

    // The model of the last snapshot taken with the lock. While a render holds the session's lock, snapshot returns this instead of waiting.
    mutable std::shared_ptr<const ArrangeSnapshot> lastSnapshot;
    ArrangeSnapshot snapshot (bool takePeaks = false) const;
    double pxPerSample (double rate) const;
    int sampleToX (double sample, double pps) const;
    double xToSample (double x, double pps) const;
    int laneTop() const;
    int trackTop (int index) const;
    int trackAt (int y, int trackCount) const;
    juce::Rectangle<int> clipRect (int track, juce::int64 start, juce::int64 length, double pps) const;
    int markerAt (const ArrangeSnapshot& s, juce::Point<int> p, double pps) const;
    int maxScrollY (int trackCount) const;

    void seekTo (juce::int64 sample);
    void layoutHeaders();
    void layoutHeaders (const ArrangeSnapshot& s);
    void syncHeaders (bool readMeters);
    void syncHeaders (const ArrangeSnapshot& s, bool readMeters);
    void changed();   // repaint and tell the window (during a drag)
    void edited();    // as changed, after the headers took the model
    void report (const juce::String& error) { if (reportError && error.isNotEmpty()) reportError (error); }
    void drawWave (juce::Graphics& g, const Clip& clip, juce::Rectangle<int> area, double pps, juce::Colour colour);
    void paintClip (juce::Graphics& g, const ArrangeSnapshot& s, double pps, int track, const Clip& clip, bool selectedNow);
    void clipMenu (ClipRef ref);
    void clipAction (ClipRef ref, int action);
    void laneMenu (int track, juce::int64 at);
    void markerMenu (int index);
    void markerAction (int index, int action);
    void nudgeSelected (int dx, int dy, bool bar);
    void askText (const juce::String& title, const juce::String& prompt, const juce::String& initial, std::function<void (const juce::String&)> onOk);
    void beginGesture (const juce::String& label) { gesture = std::make_unique<Gesture> (editor, label); }
    void endGesture() { gesture.reset(); }
    bool& dialogFlag() { return dialogOpen != nullptr ? *dialogOpen : localDialogOpen; }

    bool localDialogOpen = false;
    juce::Component::SafePointer<juce::AlertWindow> activeDialog;
    std::function<void (bool, const juce::String&)> pendingDialogEnd;
};
} // namespace olympus::gui
