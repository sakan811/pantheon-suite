// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "Arrange.h"

#include <cmath>
#include <limits>
#include <map>

#include "LevelMeter.h"
#include <pantheon/ui/MeterCell.h>
#include <pantheon/ui/ValueKnob.h>

namespace olympus::gui
{
namespace
{
constexpr int kEdgeGrab = 6;         // px at a clip's edge that trims it
constexpr int kFadeGrab = 7;         // px either side of a fade handle
constexpr int kFadeZone = 12;        // px from the top of a clip where its fade handles are
constexpr int kClipPad = 3;          // px between a clip and the edges of its lane
constexpr int kMinGap = 44;          // px between two grid lines the ruler will draw
constexpr double kMinZoom = 0.05, kMaxZoom = 4000.0;
constexpr double kFallbackRate = 48000.0;

bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;aif;aiff;flac;mp3;m4a;ogg;caf"); }

juce::String panText (double v)
{
    if (std::abs (v) < 0.005) return "C";
    return (v < 0 ? "L" : "R") + juce::String (juce::roundToInt (std::abs (v) * 100.0));
}

// "L30", "R30", "C" or a plain number from -1 to 1; NaN for anything else.
double panFromText (const juce::String& text)
{
    const auto t = text.trim().toUpperCase();
    if (t == "C" || t == "CENTRE" || t == "CENTER") return 0.0;
    if (t.startsWithChar ('L') || t.startsWithChar ('R'))
    {
        const auto digits = t.substring (1).trim();
        if (! digits.containsOnly ("0123456789.")) return std::numeric_limits<double>::quiet_NaN();
        const double p = juce::jlimit (0.0, 1.0, digits.getDoubleValue() / 100.0);
        return t.startsWithChar ('L') ? -p : p;
    }
    if (! t.containsOnly ("0123456789.-+")) return std::numeric_limits<double>::quiet_NaN();
    return juce::jlimit (-1.0, 1.0, t.getDoubleValue());
}

// The grid step in beats for the zoom: the smallest of the usual steps (and whole bars) that leaves at least kMinGap pixels between lines.
double chooseStepBeats (const TempoMap& tempo, double pxPerBeat)
{
    const double bpb = tempo.beatsPerBar();
    double best = -1.0, largest = 0.0;
    std::vector<double> candidates;
    for (int k = 0; k <= 12; ++k) candidates.push_back (0.125 * std::pow (2.0, k));
    for (int k = 0; k <= 8; ++k) candidates.push_back (bpb * std::pow (2.0, k));
    for (double c : candidates)
    {
        largest = std::max (largest, c);
        if (c * pxPerBeat >= kMinGap && (best < 0.0 || c < best)) best = c;
    }
    return best > 0.0 ? best : largest;
}

// A push switch with its lamp in the colour it is given (the kit's look-and-feel lights every switch in one colour).
class ArrangeLamp : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    juce::Colour lamp = colours::amber;
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        ui::hw::drawPushButton (g, getLocalBounds().toFloat(), getToggleState(), lamp, down, highlighted, isEnabled());
        g.setFont (ui::labelFont (juce::jmin (10.0f, (float) getHeight() * 0.5f)));
        g.setColour (getToggleState() && lamp.getPerceivedBrightness() > 0.5f ? colours::lampInk : colours::text);
        g.drawText (getButtonText().toUpperCase(), getLocalBounds().translated (0, down ? 1 : 0), juce::Justification::centred);
    }
};
} // namespace

// ---- the model, copied out under the lock --------------------------------------------------------------------------------------
struct ArrangeSnapshot
{
    struct TrackView
    {
        juce::String name;
        float gainDb = 0.0f, pan = 0.0f;
        bool mute = false, solo = false;
        bool armed = false;
        int inputLeft = 0, inputRight = -1;
        float peakL = 0.0f, peakR = 0.0f; // only when taken with peaks: the maximum since the last read
        std::vector<Clip> clips;
    };

    double rate = kFallbackRate;
    TempoMap tempo;
    Grid grid = Grid::Quarter;
    bool loopOn = false;
    juce::int64 loopStart = 0, loopEnd = 0;
    std::vector<Marker> markers;
    std::vector<TrackView> tracks;
    juce::int64 length = 0;
};

// The peaks of an audio file: per block of 256 frames, the lowest and the highest sample of either channel. Built once per file.
struct ArrangeWaves
{
    static constexpr int kBlock = 256;

    struct Peaks
    {
        std::weak_ptr<const juce::AudioBuffer<float>> owner;
        std::vector<float> lo, hi;
    };
    std::map<const juce::AudioBuffer<float>*, Peaks> peaks;

    const Peaks& of (const std::shared_ptr<const juce::AudioBuffer<float>>& audio)
    {
        for (auto it = peaks.begin(); it != peaks.end();)
            it = it->second.owner.expired() ? peaks.erase (it) : std::next (it);

        auto& entry = peaks[audio.get()];
        if (entry.owner.expired())
        {
            entry.owner = audio;
            const int frames = audio->getNumSamples(), channels = audio->getNumChannels();
            const int blocks = (frames + kBlock - 1) / kBlock;
            entry.lo.assign ((size_t) blocks, 0.0f);
            entry.hi.assign ((size_t) blocks, 0.0f);
            for (int b = 0; b < blocks; ++b)
            {
                const int start = b * kBlock, n = juce::jmin (kBlock, frames - start);
                float lo = 1.0f, hi = -1.0f;
                for (int ch = 0; ch < channels; ++ch)
                {
                    const float* p = audio->getReadPointer (ch) + start;
                    for (int i = 0; i < n; ++i) { lo = std::min (lo, p[i]); hi = std::max (hi, p[i]); }
                }
                entry.lo[(size_t) b] = lo;
                entry.hi[(size_t) b] = hi;
            }
        }
        return entry;
    }
};

// ---- one track's header: name, M, S, FX, X, pan, R, IN, fader and meter -------------------------------------------------------------
class ArrangeTrackHeader : public juce::Component
{
public:
    ArrangeTrackHeader (ArrangeView& owner, int position)
        : view (owner), index (position), panKnob (ui::ValueKnob::Layout::beside)
    {
        nameLabel.setFont (ui::font (12.5f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, colours::text);
        nameLabel.setEditable (false, true);
        nameLabel.setTooltip ("Double-click to rename the track");
        nameLabel.onEditorShow = [this] { editingName = true; };
        nameLabel.onEditorHide = [this] { editingName = false; };
        nameLabel.onTextChange = [this]
        {
            juce::String error;
            if (! view.editor.renameTrack (index, nameLabel.getText(), error)) view.report (error);
            view.edited();
        };
        addAndMakeVisible (nameLabel);

        muteButton.setClickingTogglesState (true);
        muteButton.lamp = colours::bad;
        // a header switch never takes the keyboard: Space and the other transport keys reach the window
        for (auto* b : std::initializer_list<juce::Component*> { &muteButton, &soloButton, &fxButton, &removeButton, &armButton, &inputButton }) b->setWantsKeyboardFocus (false);
        faderSlider.setWantsKeyboardFocus (false);
        panKnob.setWantsKeyboardFocus (false);
        muteButton.setTooltip ("Mute the track");
        muteButton.onClick = [this]
        {
            juce::String error;
            if (! view.editor.setTrackMute (index, muteButton.getToggleState(), error)) view.report (error);
            view.edited();
        };
        soloButton.setClickingTogglesState (true);
        soloButton.lamp = colours::good;
        soloButton.setTooltip ("Solo the track");
        soloButton.onClick = [this]
        {
            juce::String error;
            if (! view.editor.setTrackSolo (index, soloButton.getToggleState(), error)) view.report (error);
            view.edited();
        };
        fxButton.lamp = colours::amber;
        fxButton.setTooltip ("The plugins of this track");
        fxButton.onClick = [this] { if (view.onTrackMenu) view.onTrackMenu (index); };
        removeButton.lamp = colours::bad;
        removeButton.setTooltip ("Remove the track");
        removeButton.onClick = [this] { if (view.onRemoveTrack) view.onRemoveTrack (index); };
        armButton.setClickingTogglesState (true);
        armButton.lamp = colours::bad;
        armButton.setTooltip ("Arm the track to record it (REC). Its input is beside it; the meter shows that input while the transport is stopped.");
        armButton.onClick = [this]
        {
            juce::String error;
            if (! view.session.setTrackArm (index, armButton.getToggleState(), error)) view.report (error);
            view.edited();
        };
        inputButton.setTooltip ("The device input this track records. Click to choose it.");
        inputButton.onClick = [this] { if (view.onInputMenu) view.onInputMenu (index); };
        for (auto* b : std::initializer_list<juce::Component*> { &muteButton, &soloButton, &fxButton, &removeButton, &armButton, &inputButton }) addAndMakeVisible (*b);

        panKnob.setup ("Pan", -1.0, 1.0, 0.0, 0.0, [] (double v) { return panText (v); }, [] (const juce::String& t) { return panFromText (t); },
                       "Balance: drag, or click the number and type L30, C or R30.");
        panKnob.onGestureStart = [this] { panGesture = std::make_unique<Gesture> (view.editor, "Track pan"); };
        panKnob.onGestureEnd = [this] { panGesture.reset(); };
        panKnob.onChange = [this] (double v)
        {
            juce::String error;
            if (! view.editor.setTrackPan (index, (float) v, error)) view.report (error);
            view.changed();
        };
        addAndMakeVisible (panKnob);

        faderSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        faderSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        faderSlider.setLookAndFeel (&view.look);
        faderSlider.setRange (-100.0, 12.0, 0.01);
        faderSlider.setDoubleClickReturnValue (true, 0.0);
        faderSlider.setTooltip ("Fader: drag, double-click for 0 dB");
        faderSlider.onDragStart = [this] { gainGesture = std::make_unique<Gesture> (view.editor, "Track gain"); };
        faderSlider.onDragEnd = [this] { gainGesture.reset(); };
        faderSlider.onValueChange = [this]
        {
            juce::String error;
            if (! view.editor.setTrackGain (index, (float) faderSlider.getValue(), error)) view.report (error);
            view.changed();
        };
        addAndMakeVisible (faderSlider);
    }

    ~ArrangeTrackHeader() override
    {
        faderSlider.setLookAndFeel (nullptr);
    }

    void setState (const juce::String& title, float gainDb, float panValue, bool isMuted, bool isSoloed, bool isArmed, const juce::String& input)
    {
        if (! editingName && nameLabel.getText() != title)
            nameLabel.setText (title, juce::dontSendNotification);
        faderSlider.setValue (gainDb, juce::dontSendNotification);
        panKnob.setValue (panValue);
        muteButton.setToggleState (isMuted, juce::dontSendNotification);
        soloButton.setToggleState (isSoloed, juce::dontSendNotification);
        armButton.setToggleState (isArmed, juce::dontSendNotification);
        if (inputButton.getButtonText() != input) inputButton.setButtonText (input);
    }

    // The meter: the level falls back by a few dB between reads, the held mark more slowly (MeterBallistics, shared with the master).
    void addPeaks (float peakL, float peakR)
    {
        meter.add ((float) dbOf (std::max (peakL, peakR)));
        repaint();
    }

    void resized() override
    {
        const int w = getWidth();
        nameLabel.setBounds (8, 5, w - 74, 20);
        fxButton.setBounds (w - 62, 5, 32, 20);
        removeButton.setBounds (w - 26, 5, 20, 20);
        muteButton.setBounds (8, 30, 26, 22);
        soloButton.setBounds (38, 30, 26, 22);
        panKnob.setBounds (66, 27, 100, 32);
        armButton.setBounds (8, 62, 26, 22);
        inputButton.setBounds (38, 62, 58, 22);
        faderSlider.setBounds (102, 66, w - 110, 16);
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        ui::hw::fillMetal (g, area, colours::panel);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRect (area.removeFromBottom (1.0f));

        ui::MeterCell cell;
        cell.area = juce::Rectangle<float> (172.0f, 17.0f, 80.0f, ui::MeterCell::kTitleHeight + ui::MeterCell::kWindowHeight);
        paintLevelBar (g, cell.drawTrack (g), meter);
    }

    ArrangeView& view;
    int index;
    ArrangeLamp armButton { "R" };
    juce::TextButton inputButton { "IN" };

private:
    bool editingName = false;
    MeterBallistics meter;
    juce::Label nameLabel;
    ArrangeLamp muteButton { "M" }, soloButton { "S" }, fxButton { "FX" }, removeButton { "X" };
    ui::ValueKnob panKnob;
    juce::Slider faderSlider;
    std::unique_ptr<Gesture> panGesture, gainGesture;
};

// ---- ArrangeView ---------------------------------------------------------------------------------------------------------------
ArrangeView::ArrangeView (Session& s, Editor& e) : session (s), editor (e), waves (std::make_unique<ArrangeWaves>())
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    addAndMakeVisible (headerPort);
    setSize (kDesignWidth, kDesignHeight);
    syncHeaders (false);
}

ArrangeView::~ArrangeView()
{
    gesture.reset();
    headers.clear();
}

juce::String ArrangeView::inputText (int left, int right)
{
    if (right < 0) return "IN " + juce::String (left + 1);
    return "IN " + juce::String (left + 1) + "+" + juce::String (right + 1);
}

juce::Button* ArrangeView::armButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) headers.size()) ? &headers[(size_t) track]->armButton : nullptr;
}

juce::Button* ArrangeView::inputButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) headers.size()) ? &headers[(size_t) track]->inputButton : nullptr;
}

ArrangeSnapshot ArrangeView::snapshot (bool takePeaks) const
{
    ArrangeSnapshot s;
    // The message thread never waits for the lock: a render holds it for its whole run. While it is busy the last model is returned (the
    // tracks do not change under a render: the window is modal over it).
    // The audio thread holds it for a moment each block, so a miss is retried; only a render (renderRunning) makes this give up.
    const juce::ScopedTryLock sl (session.lock);
    while (! sl.isLocked())
    {
        if (session.renderRunning())
            return lastSnapshot != nullptr ? *lastSnapshot : s;
        juce::Thread::sleep (1);
        sl.retryLock();
    }
    s.rate = session.sampleRate > 0.0 ? session.sampleRate : kFallbackRate;
    s.tempo = session.tempo;
    s.grid = session.grid;
    s.markers = session.markers;
    s.loopOn = session.loopOn;
    s.loopStart = session.loopStart;
    s.loopEnd = session.loopEnd;
    for (auto& t : session.tracks)
    {
        ArrangeSnapshot::TrackView v;
        v.name = t->name;
        v.gainDb = t->gainDb;
        v.pan = t->pan;
        v.mute = t->mute;
        v.solo = t->solo;
        v.armed = t->armed;
        v.inputLeft = t->inputLeft;
        v.inputRight = t->inputRight;
        v.clips = t->clips;
        if (takePeaks)
        {
            v.peakL = t->peakL.exchange (0.0f);
            v.peakR = t->peakR.exchange (0.0f);
        }
        s.length = juce::jmax (s.length, t->length());
        s.tracks.push_back (std::move (v));
    }
    lastSnapshot = std::make_shared<const ArrangeSnapshot> (s);
    return s;
}

double ArrangeView::pxPerSample (double rate) const
{
    return juce::jlimit (kMinZoom, kMaxZoom, zoom) / rate;
}

int ArrangeView::sampleToX (double sample, double pps) const
{
    return kHeaderWidth + juce::roundToInt ((sample - scrollSamples) * pps);
}

double ArrangeView::xToSample (double x, double pps) const
{
    return scrollSamples + (x - kHeaderWidth) / pps;
}

int ArrangeView::laneTop() const
{
    return kRulerHeight + kLoopHeight + kMarkerHeight;
}

int ArrangeView::trackTop (int index) const
{
    return laneTop() + index * kLaneHeight - scrollY;
}

int ArrangeView::trackAt (int y, int trackCount) const
{
    if (y < laneTop()) return -1;
    const int i = (y - laneTop() + scrollY) / kLaneHeight;
    return i < trackCount ? i : -1;
}

juce::Rectangle<int> ArrangeView::clipRect (int track, juce::int64 start, juce::int64 length, double pps) const
{
    const int x0 = sampleToX ((double) start, pps);
    const int x1 = sampleToX ((double) (start + length), pps);
    const int top = trackTop (track);
    return { x0, top + kClipPad, juce::jmax (2, x1 - x0), kLaneHeight - 2 * kClipPad };
}

int ArrangeView::markerAt (const ArrangeSnapshot& s, juce::Point<int> p, double pps) const
{
    const auto font = ui::labelFont (9.5f);
    for (int i = 0; i < (int) s.markers.size(); ++i)
    {
        const int x = sampleToX ((double) s.markers[(size_t) i].start, pps);
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, s.markers[(size_t) i].name, 0.0f, 0.0f);
        const float width = glyphs.getBoundingBox (0, -1, true).getWidth() + 14.0f;
        if (p.x >= x - 4 && (float) p.x <= (float) x + width) return i;
    }
    return -1;
}

int ArrangeView::maxScrollY (int trackCount) const
{
    return juce::jmax (0, trackCount * kLaneHeight - (getHeight() - laneTop()));
}

void ArrangeView::seekTo (juce::int64 sample)
{
    const auto at = juce::jmax ((juce::int64) 0, sample);
    playhead = (double) at;
    if (onSeek) onSeek (at);
    repaint();
}

void ArrangeView::layoutHeaders()
{
    const auto s = snapshot();
    const int count = (int) s.tracks.size();
    scrollY = juce::jlimit (0, maxScrollY (count), scrollY);
    headerPort.setBounds (0, laneTop(), kHeaderWidth, juce::jmax (0, getHeight() - laneTop()));
    for (int i = 0; i < (int) headers.size(); ++i)
        headers[(size_t) i]->setBounds (0, trackTop (i) - laneTop(), kHeaderWidth, kLaneHeight);
}

void ArrangeView::syncHeaders (bool readMeters)
{
    const auto s = snapshot (readMeters);
    const int n = (int) s.tracks.size();
    while ((int) headers.size() > n)
    {
        headerPort.removeChildComponent (headers.back().get());
        headers.pop_back();
    }
    while ((int) headers.size() < n)
    {
        headers.push_back (std::make_unique<ArrangeTrackHeader> (*this, (int) headers.size()));
        headerPort.addAndMakeVisible (*headers.back());
    }
    for (int i = 0; i < n; ++i)
    {
        auto& h = *headers[(size_t) i];
        const auto& t = s.tracks[(size_t) i];
        h.index = i;
        h.setState (t.name, t.gainDb, t.pan, t.mute, t.solo, t.armed, inputText (t.inputLeft, t.inputRight));
        if (! readMeters) continue;
        if (t.armed && showInputLevels && inputLevel)
        {
            const float peak = inputLevel (i);
            h.addPeaks (peak, peak);
        }
        else
            h.addPeaks (t.peakL, t.peakR);
    }

    // a selection the model no longer has is dropped
    if (selected.valid())
    {
        const bool ok = selected.track < n && selected.clip < (int) s.tracks[(size_t) selected.track].clips.size();
        if (! ok) selected = {};
    }
    layoutHeaders();
}

void ArrangeView::changed()
{
    if (onStructure) onStructure();
    repaint();
}

void ArrangeView::edited()
{
    syncHeaders (false);
    changed();
}

void ArrangeView::refresh()
{
    syncHeaders (true);
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const double span = juce::jmax (0.0, (double) (getWidth() - kHeaderWidth) / pps);
    if (followPlayhead && playhead != lastPlayhead && (playhead < scrollSamples || playhead > scrollSamples + span))
        scrollSamples = juce::jmax (0.0, playhead - span * 0.125);
    lastPlayhead = playhead;
    repaint();
}

void ArrangeView::zoomToFit()
{
    const auto s = snapshot();
    if (s.length <= 0)
    {
        zoom = kDefaultZoom;
    }
    else
    {
        const double seconds = (double) s.length / s.rate;
        const double avail = juce::jmax (100.0, (double) (getWidth() - kHeaderWidth - 24));
        zoom = juce::jlimit (kMinZoom, kMaxZoom, avail / seconds);
    }
    scrollSamples = 0.0;
    repaint();
}

void ArrangeView::scrollTo (juce::int64 sample)
{
    scrollSamples = (double) juce::jmax ((juce::int64) 0, sample);
    repaint();
}

// ---- painting --------------------------------------------------------------------------------------------------------------------
void ArrangeView::drawWave (juce::Graphics& g, const Clip& clip, juce::Rectangle<int> area, double pps, juce::Colour colour)
{
    if (clip.audio == nullptr || clip.audio->getNumSamples() == 0 || clip.length <= 0) return;
    const auto& buffer = *clip.audio;
    const auto& peaks = waves->of (clip.audio);
    const auto frames = (juce::int64) buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    const float mid = (float) area.getCentreY(), half = juce::jmax (1.0f, (float) area.getHeight() * 0.5f - 3.0f);
    const int blockSize = ArrangeWaves::kBlock;

    g.setColour (colour);
    for (int x = area.getX(); x < area.getRight(); ++x)
    {
        // the column's samples, relative to the clip's start and to its place in the file
        const double t0 = xToSample (x, pps) - (double) clip.start;
        const double t1 = t0 + 1.0 / pps;
        const double a = juce::jlimit (0.0, (double) clip.length, t0), b = juce::jlimit (0.0, (double) clip.length, t1);
        if (b <= a) continue;
        juce::int64 f0 = clip.offset + (juce::int64) std::floor (a);
        juce::int64 f1 = clip.offset + (juce::int64) std::ceil (b);
        f0 = juce::jlimit ((juce::int64) 0, frames - 1, f0);
        f1 = juce::jlimit (f0 + 1, frames, f1);

        float lo = 1.0f, hi = -1.0f;
        if (f1 - f0 <= blockSize)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float* p = buffer.getReadPointer (ch);
                for (juce::int64 i = f0; i < f1; ++i) { lo = std::min (lo, p[i]); hi = std::max (hi, p[i]); }
            }
        }
        else
        {
            const auto b0 = (size_t) (f0 / blockSize), b1 = (size_t) ((f1 - 1) / blockSize);
            for (auto k = b0; k <= b1; ++k) { lo = std::min (lo, peaks.lo[k]); hi = std::max (hi, peaks.hi[k]); }
        }
        if (hi < lo) continue;
        const float y1 = mid - juce::jlimit (-1.0f, 1.0f, hi) * half;
        const float y2 = mid - juce::jlimit (-1.0f, 1.0f, lo) * half;
        g.fillRect ((float) x, y1, 1.0f, juce::jmax (1.0f, y2 - y1));
    }
}

void ArrangeView::paintClip (juce::Graphics& g, const ArrangeSnapshot& s, double pps, int track, const Clip& clip, bool selectedNow)
{
    const auto rect = clipRect (track, clip.start, clip.length, pps);
    const auto r = rect.toFloat();
    const bool dimmed = clip.mute || s.tracks[(size_t) track].mute;

    g.saveState();
    g.reduceClipRegion (rect.getIntersection (getLocalBounds().withTrimmedLeft (kHeaderWidth)));
    g.setColour (colours::panel.brighter (0.06f).withAlpha (0.92f));
    g.fillRoundedRectangle (r, 3.0f);

    if (clip.audio == nullptr)
    {
        g.setColour (colours::warn.withAlpha (0.28f));
        for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += 9.0f)
            g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 1.0f);
        g.setColour (colours::warn);
        g.setFont (ui::labelFont (9.5f));
        g.drawText ("MISSING AUDIO", r.reduced (5.0f, 3.0f), juce::Justification::topLeft, false);
    }
    else
    {
        drawWave (g, clip, rect.reduced (0, 2), pps, (dimmed ? colours::dim : colours::accent).withAlpha (0.8f));
    }

    // fades: the silence above the curve
    const float fadeInX = r.getX() + (float) clip.fadeIn * (float) pps;
    const float fadeOutX = r.getRight() - (float) clip.fadeOut * (float) pps;
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    if (clip.fadeIn > 0)
    {
        juce::Path p;
        p.startNewSubPath (r.getX(), r.getY());
        p.lineTo (fadeInX, r.getY());
        p.lineTo (r.getX(), r.getBottom());
        p.closeSubPath();
        g.fillPath (p);
    }
    if (clip.fadeOut > 0)
    {
        juce::Path p;
        p.startNewSubPath (fadeOutX, r.getY());
        p.lineTo (r.getRight(), r.getY());
        p.lineTo (r.getRight(), r.getBottom());
        p.closeSubPath();
        g.fillPath (p);
    }

    if (clip.mute)
    {
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (r, 3.0f);
    }

    juce::String title = clip.name;
    if (std::abs (clip.gainDb) > 0.05f) title += "  " + juce::String (clip.gainDb, 1) + " dB";
    if (clip.mute) title += "  (muted)";
    if (clip.audio != nullptr)
    {
        g.setFont (ui::labelFont (9.5f));
        g.setColour (dimmed ? colours::dim : colours::text);
        g.drawText (title, r.reduced (5.0f, 2.0f), juce::Justification::topLeft, true);
    }
    else
    {
        g.setFont (ui::labelFont (9.5f));
        g.setColour (colours::dim);
        g.drawText (title, r.withTrimmedTop (14.0f).reduced (5.0f, 0.0f), juce::Justification::topLeft, true);
    }

    // fade handles, at the top
    g.setColour (colours::amber);
    if (clip.fadeIn > 0) g.fillRect (fadeInX - 3.0f, r.getY(), 6.0f, 6.0f);
    if (clip.fadeOut > 0) g.fillRect (fadeOutX - 3.0f, r.getY(), 6.0f, 6.0f);

    g.setColour (selectedNow ? colours::amber : colours::panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, selectedNow ? 2.0f : 1.0f);
    g.restoreState();
}

void ArrangeView::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const int width = getWidth(), height = getHeight(), top = laneTop();
    const auto timeline = juce::Rectangle<int> (kHeaderWidth, 0, juce::jmax (0, width - kHeaderWidth), height);
    const int count = (int) s.tracks.size();
    const double leftSample = scrollSamples, rightSample = xToSample (width, pps);

    // the column on the left, above the lanes
    ui::hw::fillMetal (g, juce::Rectangle<float> (0.0f, 0.0f, (float) kHeaderWidth, (float) top), colours::panel.darker (0.15f));
    ui::hw::drawLabel (g, "Tracks  " + juce::String (count), { 10, 0, kHeaderWidth - 20, kRulerHeight }, juce::Justification::centredLeft, colours::dim, 10.0f);
    ui::hw::drawLabel (g, s.loopOn ? "Loop on" : "Loop off  (click)", { 10, kRulerHeight, kHeaderWidth - 20, kLoopHeight },
                       juce::Justification::centredLeft, s.loopOn ? colours::amber : colours::dim, 10.0f);
    ui::hw::drawLabel (g, "Markers  " + juce::String ((int) s.markers.size()), { 10, kRulerHeight + kLoopHeight, kHeaderWidth - 20, kMarkerHeight },
                       juce::Justification::centredLeft, colours::dim, 10.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRect (kHeaderWidth - 1, 0, 1, top);

    g.setColour (colours::background.darker (0.2f));
    g.fillRect (timeline);

    g.saveState();
    g.reduceClipRegion (timeline);

    // lanes
    for (int i = 0; i < count; ++i)
    {
        const int y = trackTop (i);
        if (y + kLaneHeight < top || y > height) continue;
        const auto lane = juce::Rectangle<float> ((float) kHeaderWidth, (float) (y + 1), (float) (width - kHeaderWidth), (float) (kLaneHeight - 2)).reduced (2.0f, 0.0f);
        ui::hw::drawGlass (g, lane, 3.0f, colours::accent);
    }

    // the loop, shaded across the lanes
    if (s.loopEnd > s.loopStart)
    {
        const int x0 = sampleToX ((double) s.loopStart, pps), x1 = sampleToX ((double) s.loopEnd, pps);
        g.setColour (colours::amber.withAlpha (s.loopOn ? 0.10f : 0.04f));
        g.fillRect (x0, top, x1 - x0, height - top);
    }

    // grid lines from the tempo map
    {
        const double bpm = s.tempo.bpmAtSample (leftSample);
        const double step = chooseStepBeats (s.tempo, pps * s.rate * 60.0 / bpm);
        const double bpb = s.tempo.beatsPerBar();
        const double startBeat = s.tempo.samplesToBeats (leftSample), endBeat = s.tempo.samplesToBeats (rightSample);
        for (double b = std::floor (std::max (0.0, startBeat) / step) * step; b <= endBeat; b += step)
        {
            if (b < 0.0) continue;
            const int x = sampleToX (s.tempo.beatsToSamples (b), pps);
            const bool bar = std::abs (std::fmod (b / bpb + 1.0e-9, 1.0)) < 1.0e-6;
            g.setColour (colours::dim.withAlpha (bar ? 0.35f : 0.14f));
            g.drawVerticalLine (x, (float) top, (float) height);
        }
    }

    // clips
    for (int i = 0; i < count; ++i)
    {
        const auto& clips = s.tracks[(size_t) i].clips;
        for (int c = 0; c < (int) clips.size(); ++c)
        {
            const auto rect = clipRect (i, clips[(size_t) c].start, clips[(size_t) c].length, pps);
            if (rect.getRight() < kHeaderWidth || rect.getX() > width) continue;
            paintClip (g, s, pps, i, clips[(size_t) c], selected.track == i && selected.clip == c);
        }
    }

    // a take in progress: a red strip across each armed lane, from where the recording began to the playhead
    if (recordingFrom >= 0.0)
    {
        const int x0 = sampleToX (recordingFrom, pps), x1 = sampleToX (juce::jmax (recordingFrom, recordingTo), pps);
        for (int track : recordingTracks)
        {
            if (track < 0 || track >= count) continue;
            const int y = trackTop (track);
            g.setColour (colours::bad.withAlpha (0.22f));
            g.fillRect (x0, y + 1, juce::jmax (0, x1 - x0), kLaneHeight - 2);
            g.setColour (colours::bad);
            g.fillRect (x0, y + 1, 2, kLaneHeight - 2);
        }
    }

    // markers run down the lanes
    for (const auto& m : s.markers)
    {
        const int x = sampleToX ((double) m.start, pps);
        g.setColour (colours::amber.withAlpha (0.45f));
        g.drawVerticalLine (x, (float) kRulerHeight + kLoopHeight, (float) height);
    }

    // playhead
    const int px = sampleToX (playhead, pps);
    g.setColour (colours::amber);
    g.fillRect (px - 1, kRulerHeight, 2, height - kRulerHeight);
    g.restoreState();

    // the ruler
    g.setColour (colours::panel.darker (0.25f));
    g.fillRect (kHeaderWidth, 0, juce::jmax (0, width - kHeaderWidth), kRulerHeight);
    {
        const double bpb = s.tempo.beatsPerBar();
        const double bpm = s.tempo.bpmAtSample (leftSample);
        const double step = chooseStepBeats (s.tempo, pps * s.rate * 60.0 / bpm);
        const double startBeat = s.tempo.samplesToBeats (leftSample), endBeat = s.tempo.samplesToBeats (rightSample);
        g.setFont (ui::labelFont (10.0f));
        for (double b = std::floor (std::max (0.0, startBeat) / step) * step; b <= endBeat; b += step)
        {
            if (b < 0.0) continue;
            const int x = sampleToX (s.tempo.beatsToSamples (b), pps);
            const bool bar = std::abs (std::fmod (b / bpb + 1.0e-9, 1.0)) < 1.0e-6;
            const bool wholeBeat = std::abs (b - std::round (b)) < 1.0e-6;
            g.setColour (colours::dim.withAlpha (bar ? 0.9f : 0.5f));
            g.drawVerticalLine (x, (float) (kRulerHeight - (bar ? 14 : 7)), (float) kRulerHeight);
            if (bar)
                g.drawText (juce::String ((int) std::floor (b / bpb + 1.0e-9) + 1), x + 3, 4, 60, 16, juce::Justification::centredLeft);
            else if (wholeBeat && step < bpb)
            {
                const int beatInBar = (int) std::fmod (std::round (b), bpb) + 1;
                g.drawText (juce::String (beatInBar), x + 3, 20, 40, 14, juce::Justification::centredLeft);
            }
        }
    }
    // tempo flags: each change of tempo after the start
    for (size_t i = 1; i < s.tempo.points.size(); ++i)
    {
        const int x = sampleToX (s.tempo.beatsToSamples (s.tempo.points[i].beat), pps);
        if (x < kHeaderWidth || x > width) continue;
        g.setColour (colours::amber);
        g.fillRect (x, 0, 2, kRulerHeight);
        g.setFont (ui::lcdFont (10.0f));
        g.drawText (juce::String (s.tempo.points[i].bpm, 1), x + 4, 0, 60, 12, juce::Justification::centredLeft);
    }

    // the loop strip
    g.setColour (colours::glass);
    g.fillRect (kHeaderWidth, kRulerHeight, juce::jmax (0, width - kHeaderWidth), kLoopHeight);
    if (s.loopEnd > s.loopStart)
    {
        const int x0 = sampleToX ((double) s.loopStart, pps), x1 = sampleToX ((double) s.loopEnd, pps);
        g.setColour (colours::amber.withAlpha (s.loopOn ? 0.75f : 0.25f));
        g.fillRect (x0, kRulerHeight + 4, juce::jmax (2, x1 - x0), kLoopHeight - 8);
    }

    // the marker lane
    g.setColour (colours::glass);
    g.fillRect (kHeaderWidth, kRulerHeight + kLoopHeight, juce::jmax (0, width - kHeaderWidth), kMarkerHeight);
    for (const auto& m : s.markers)
    {
        const int x = sampleToX ((double) m.start, pps);
        if (x < kHeaderWidth - 60 || x > width) continue;
        g.setColour (colours::amber);
        g.fillRect (x, kRulerHeight + kLoopHeight + 2, 2, kMarkerHeight - 4);
        g.setFont (ui::labelFont (9.5f));
        g.drawText (m.name, x + 5, kRulerHeight + kLoopHeight + 4, 160, kMarkerHeight - 6, juce::Justification::centredLeft);
    }

    // the playhead's head, in the ruler
    g.setColour (colours::amber);
    if (px >= kHeaderWidth - 1 && px <= width)
        g.fillRect (px - 4, 0, 9, 6);
}

// ---- layout ------------------------------------------------------------------------------------------------------------------------
void ArrangeView::resized()
{
    layoutHeaders();
}

ClipRef ArrangeView::clipAt (juce::Point<int> p) const
{
    if (p.x < kHeaderWidth) return {};
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const int track = trackAt (p.y, (int) s.tracks.size());
    if (track < 0) return {};
    const auto& clips = s.tracks[(size_t) track].clips;
    for (int c = (int) clips.size() - 1; c >= 0; --c)
        if (clipRect (track, clips[(size_t) c].start, clips[(size_t) c].length, pps).contains (p))
            return { track, c };
    return {};
}

juce::Rectangle<int> ArrangeView::clipBounds (ClipRef ref) const
{
    const auto s = snapshot();
    if (! ref.valid() || ref.track >= (int) s.tracks.size()) return {};
    const auto& clips = s.tracks[(size_t) ref.track].clips;
    if (ref.clip >= (int) clips.size()) return {};
    const auto& c = clips[(size_t) ref.clip];
    return clipRect (ref.track, c.start, c.length, pxPerSample (s.rate));
}

// ---- mouse -------------------------------------------------------------------------------------------------------------------------
void ArrangeView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto p = e.getPosition();
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    pressX = p.x;
    pressY = p.y;
    mode = Mode::none;

    if (p.x < kHeaderWidth)
    {
        // the LOOP label switches the loop on and off
        if (p.y >= kRulerHeight && p.y < kRulerHeight + kLoopHeight)
        {
            editor.setLoop (s.loopStart, s.loopEnd, ! s.loopOn);
            edited();
        }
        return;
    }

    if (p.y < kRulerHeight)
    {
        mode = Mode::ruler;
        seekTo ((juce::int64) xToSample (p.x, pps));
        return;
    }

    if (p.y < kRulerHeight + kLoopHeight)
    {
        if (e.mods.isPopupMenu()) return;
        mode = Mode::loop;
        anchor = editor.snap ((juce::int64) juce::jmax (0.0, xToSample (p.x, pps)));
        beginGesture ("Loop");
        return;
    }

    if (p.y < laneTop())
    {
        const int m = markerAt (s, p, pps);
        if (m >= 0)
        {
            if (e.mods.isPopupMenu()) { markerMenu (m); return; }
            mode = Mode::marker;
            dragMarker = m;
            origMarker = s.markers[(size_t) m].start;
            beginGesture ("Move marker");
            return;
        }
        mode = Mode::ruler;
        seekTo ((juce::int64) xToSample (p.x, pps));
        return;
    }

    // the lanes
    const auto hit = clipAt (p);
    if (! hit.valid())
    {
        if (! e.mods.isPopupMenu()) selected = {};
        else if (editor.hasClipboard())
        {
            const int track = trackAt (p.y, (int) s.tracks.size());
            if (track >= 0) laneMenu (track, (juce::int64) juce::jmax (0.0, xToSample (p.x, pps)));
        }
        repaint();
        return;
    }
    selected = hit;
    repaint();
    if (e.mods.isPopupMenu()) { clipMenu (hit); return; }

    const auto& c = s.tracks[(size_t) hit.track].clips[(size_t) hit.clip];
    const auto r = clipBounds (hit);
    dragClip = hit;
    origStart = c.start;
    origEnd = c.end();
    origFadeIn = c.fadeIn;
    origFadeOut = c.fadeOut;

    const int fadeInX = sampleToX ((double) (c.start + c.fadeIn), pps);
    const int fadeOutX = sampleToX ((double) (c.end() - c.fadeOut), pps);
    const bool top = p.y - r.getY() < kFadeZone;
    if (top && c.fadeIn > 0 && std::abs (p.x - fadeInX) <= kFadeGrab) mode = Mode::fadeIn;
    else if (top && c.fadeOut > 0 && std::abs (p.x - fadeOutX) <= kFadeGrab) mode = Mode::fadeOut;
    else if (p.x - r.getX() <= kEdgeGrab) mode = Mode::trimStart;
    else if (r.getRight() - p.x <= kEdgeGrab) mode = Mode::trimEnd;
    else mode = Mode::move;

    switch (mode)
    {
        case Mode::fadeIn: beginGesture ("Clip fade in"); break;
        case Mode::fadeOut: beginGesture ("Clip fade out"); break;
        case Mode::trimStart: beginGesture ("Trim clip start"); break;
        case Mode::trimEnd: beginGesture ("Trim clip end"); break;
        default: beginGesture ("Move clip"); break;
    }
}

void ArrangeView::mouseDrag (const juce::MouseEvent& e)
{
    if (mode == Mode::none) return;
    const auto p = e.getPosition();
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const double deltaSamples = (double) (p.x - pressX) / pps;
    juce::String error;

    switch (mode)
    {
        case Mode::ruler:
            seekTo ((juce::int64) xToSample (p.x, pps));
            return;

        case Mode::loop:
        {
            const auto end = editor.snap ((juce::int64) juce::jmax (0.0, xToSample (p.x, pps)));
            if (end != anchor)
                editor.setLoop (std::min (anchor, end), std::max (anchor, end), true);
            break;
        }

        case Mode::marker:
        {
            if (! juce::isPositiveAndBelow (dragMarker, (int) s.markers.size())) return;
            const auto name = s.markers[(size_t) dragMarker].name;
            const auto target = editor.snap ((juce::int64) juce::jmax (0.0, (double) origMarker + deltaSamples));
            if (! editor.moveMarker (dragMarker, target, error)) { report (error); return; }
            // the list stays sorted: the marker is found again by its name and place
            const auto after = snapshot();
            for (int i = 0; i < (int) after.markers.size(); ++i)
                if (after.markers[(size_t) i].name == name && after.markers[(size_t) i].start == target) dragMarker = i;
            break;
        }

        case Mode::move:
        {
            const int lane = trackAt (p.y, (int) s.tracks.size());
            const auto newStart = editor.snap ((juce::int64) juce::jmax (0.0, (double) origStart + std::llround (deltaSamples)));
            if (lane >= 0 && lane != dragClip.track)
            {
                const auto moved = editor.moveClipToTrack (dragClip.track, dragClip.clip, lane, newStart, error);
                if (moved.valid()) { dragClip = moved; selected = moved; }
                else report (error);
            }
            else if (! editor.moveClip (dragClip.track, dragClip.clip, newStart, error))
                report (error);
            break;
        }

        case Mode::trimStart:
            if (! editor.trimStart (dragClip.track, dragClip.clip, editor.snap ((juce::int64) juce::jmax (0.0, (double) origStart + std::llround (deltaSamples))), error))
                report (error);
            break;

        case Mode::trimEnd:
            if (! editor.trimEnd (dragClip.track, dragClip.clip, editor.snap ((juce::int64) juce::jmax (0.0, (double) origEnd + std::llround (deltaSamples))), error))
                report (error);
            break;

        case Mode::fadeIn:
            if (! editor.setClipFades (dragClip.track, dragClip.clip, origFadeIn + std::llround (deltaSamples), origFadeOut, error))
                report (error);
            break;

        case Mode::fadeOut:
            if (! editor.setClipFades (dragClip.track, dragClip.clip, origFadeIn, origFadeOut - std::llround (deltaSamples), error))
                report (error);
            break;

        case Mode::none:
            break;
    }
    changed();
}

void ArrangeView::mouseUp (const juce::MouseEvent&)
{
    const bool wasEdit = mode != Mode::none && mode != Mode::ruler;
    mode = Mode::none;
    endGesture();
    if (wasEdit) edited();
}

void ArrangeView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto s = snapshot();
    const int timelineWidth = juce::jmax (1, getWidth() - kHeaderWidth);
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double pps = pxPerSample (s.rate);
        const double anchorSample = xToSample (e.x, pps);
        zoom = juce::jlimit (kMinZoom, kMaxZoom, zoom * std::pow (2.0, wheel.deltaY * 4.0));
        const double newPps = pxPerSample (s.rate);
        scrollSamples = juce::jmax (0.0, anchorSample - (e.x - kHeaderWidth) / newPps);
    }
    else if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) || e.mods.isShiftDown())
    {
        const double pps = pxPerSample (s.rate);
        const double delta = std::abs (wheel.deltaX) > 0.0 ? wheel.deltaX : wheel.deltaY;
        scrollSamples = juce::jmax (0.0, scrollSamples - delta * 0.25 * timelineWidth / pps);
    }
    else
    {
        scrollY = juce::jlimit (0, maxScrollY ((int) s.tracks.size()), scrollY - juce::roundToInt (wheel.deltaY * 300.0));
    }
    layoutHeaders();
    repaint();
}

// ---- keys and the menus --------------------------------------------------------------------------------------------------------
bool ArrangeView::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    const auto mods = key.getModifiers();

    // The clip keys (Delete, Cmd+D, S) and the transport keys belong to the window, which calls the methods above; the view takes only
    // the selection and the nudges, so a key is never handled twice.
    if (mods.isCommandDown()) return false;
    if (code == juce::KeyPress::escapeKey) { selected = {}; repaint(); return true; }
    if (code == juce::KeyPress::leftKey) { nudgeSelected (-1, 0, mods.isShiftDown()); return true; }
    if (code == juce::KeyPress::rightKey) { nudgeSelected (1, 0, mods.isShiftDown()); return true; }
    if (code == juce::KeyPress::upKey) { nudgeSelected (0, -1, false); return true; }
    if (code == juce::KeyPress::downKey) { nudgeSelected (0, 1, false); return true; }
    return false;
}

// Left and right move the selected clip one grid step (a bar with Shift); up and down move it to the track above or below. Without a
// selection left and right move the playhead one step.
void ArrangeView::nudgeSelected (int dx, int dy, bool bar)
{
    const auto s = snapshot();
    const double beats = bar ? s.tempo.beatsPerBar() : s.tempo.gridBeats (s.grid);
    const double stepSamples = s.tempo.beatsToSamples (beats) - s.tempo.beatsToSamples (0.0);
    juce::String error;

    if (! selected.valid())
    {
        if (dx != 0) seekTo ((juce::int64) juce::jmax (0.0, (double) playhead + dx * stepSamples));
        return;
    }
    const auto& c = s.tracks[(size_t) selected.track].clips[(size_t) selected.clip];
    if (dx != 0)
    {
        const auto target = editor.snap ((juce::int64) juce::jmax (0.0, (double) c.start + dx * stepSamples));
        if (! editor.moveClip (selected.track, selected.clip, target, error)) report (error);
    }
    else if (dy != 0)
    {
        const int to = selected.track + dy;
        if (juce::isPositiveAndBelow (to, (int) s.tracks.size()))
        {
            const auto moved = editor.moveClipToTrack (selected.track, selected.clip, to, c.start, error);
            if (moved.valid()) selected = moved;
            else report (error);
        }
    }
    edited();
}

void ArrangeView::splitAtPlayhead()
{
    const auto s = snapshot();
    juce::String error;
    const auto at = (juce::int64) playhead;

    const auto inside = [&] (ClipRef r)
    {
        const auto& c = s.tracks[(size_t) r.track].clips[(size_t) r.clip];
        return at > c.start && at < c.end();
    };

    if (selected.valid() && inside (selected))
    {
        const auto right = editor.splitClip (selected.track, selected.clip, at, error);
        if (right.valid()) selected = right;
        else report (error);
        edited();
        return;
    }

    bool any = false;
    beginGesture ("Split clip");
    for (int t = 0; t < (int) s.tracks.size(); ++t)
        for (int c = (int) s.tracks[(size_t) t].clips.size() - 1; c >= 0; --c)
            if (inside ({ t, c })) { editor.splitClip (t, c, at, error); any = true; }
    endGesture();
    if (! any) report ("No clip is under the playhead.");
    selected = {};
    edited();
}

void ArrangeView::deleteSelected()
{
    if (! selected.valid()) return;
    juce::String error;
    if (editor.deleteClip (selected.track, selected.clip, error)) selected = {};
    else report (error);
    edited();
}

void ArrangeView::duplicateSelected()
{
    if (! selected.valid()) return;
    juce::String error;
    const auto copy = editor.duplicateClip (selected.track, selected.clip, error);
    if (copy.valid()) selected = copy;
    else report (error);
    edited();
}

void ArrangeView::copySelected()
{
    if (! selected.valid()) return;
    juce::String error;
    if (! editor.copyClip (selected, error)) report (error);
}

void ArrangeView::cutSelected()
{
    if (! selected.valid()) return;
    juce::String error;
    if (editor.cutClip (selected, error)) selected = {};
    else report (error);
    edited();
}

void ArrangeView::pasteAtPlayhead()
{
    if (! editor.hasClipboard()) { report ("The clipboard is empty: copy or cut a clip first."); return; }
    pasteAt (selected.valid() ? selected.track : editor.clipboardTrack(), (juce::int64) playhead);
}

void ArrangeView::pasteAt (int track, juce::int64 at)
{
    if (recordingFrom >= 0.0) { report ("Stop the recording before you paste."); return; }
    juce::String error;
    const auto made = editor.pasteClip (track, at, error);
    if (! made.valid()) { report (error); return; }
    selected = made;
    edited();
}

void ArrangeView::addMarkerAtPlayhead()
{
    const auto s = snapshot();
    editor.addMarker ("Marker " + juce::String ((int) s.markers.size() + 1), (juce::int64) playhead);
    edited();
}

void ArrangeView::clipMenu (ClipRef ref)
{
    const auto s = snapshot();
    if (! ref.valid() || ref.track >= (int) s.tracks.size()) return;
    const auto& c = s.tracks[(size_t) ref.track].clips[(size_t) ref.clip];

    juce::PopupMenu menu;
    menu.addItem (1, "Split at playhead");
    menu.addItem (2, "Duplicate");
    menu.addItem (3, "Copy");
    menu.addItem (4, "Cut");
    menu.addItem (5, c.mute ? "Unmute" : "Mute");
    menu.addItem (6, "Gain...");
    menu.addItem (7, "Rename...");
    menu.addSeparator();
    menu.addItem (8, "Delete");
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, ref] (int result)
    {
        if (safe != nullptr && result > 0) safe->clipAction (ref, result);
    });
}

// The empty lane's menu: Paste here, at the clicked track and sample (only when the clipboard holds a clip).
void ArrangeView::laneMenu (int track, juce::int64 at)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Paste here");
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, track, at] (int result)
    {
        if (safe != nullptr && result > 0) safe->pasteAt (track, at);
    });
}

void ArrangeView::clipAction (ClipRef ref, int action)
{
    const auto s = snapshot();
    if (! ref.valid() || ref.track >= (int) s.tracks.size() || ref.clip >= (int) s.tracks[(size_t) ref.track].clips.size()) return;
    const auto c = s.tracks[(size_t) ref.track].clips[(size_t) ref.clip];
    juce::String error;

    switch (action)
    {
        case 1:
            if (playhead > (double) c.start && playhead < (double) c.end())
            {
                selected = ref;
                splitAtPlayhead();
            }
            else report ("The playhead is not inside that clip.");
            return;
        case 2:
            if (auto copy = editor.duplicateClip (ref.track, ref.clip, error); copy.valid()) selected = copy;
            else report (error);
            break;
        case 3:
            if (! editor.copyClip (ref, error)) report (error);
            return;
        case 4:
            if (editor.cutClip (ref, error)) selected = {};
            else report (error);
            break;
        case 5:
            if (! editor.setClipMute (ref.track, ref.clip, ! c.mute, error)) report (error);
            break;
        case 6:
            askText ("Clip gain", "Gain in dB, from -100 to +24", juce::String (c.gainDb, 1), [this, ref] (const juce::String& text)
            {
                if (! text.containsAnyOf ("0123456789")) { report ("Type a number of dB."); return; }
                juce::String e;
                if (! editor.setClipGain (ref.track, ref.clip, text.getFloatValue(), e)) report (e);
                edited();
            });
            return;
        case 7:
            askText ("Rename clip", "The new name", c.name, [this, ref] (const juce::String& text)
            {
                juce::String e;
                if (! editor.renameClip (ref.track, ref.clip, text, e)) report (e);
                edited();
            });
            return;
        case 8:
            if (editor.deleteClip (ref.track, ref.clip, error)) selected = {};
            else report (error);
            break;
        default:
            return;
    }
    edited();
}

void ArrangeView::markerMenu (int index)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Rename...");
    menu.addItem (2, "Delete");
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, index] (int result)
    {
        if (safe != nullptr && result > 0) safe->markerAction (index, result);
    });
}

void ArrangeView::markerAction (int index, int action)
{
    const auto s = snapshot();
    if (! juce::isPositiveAndBelow (index, (int) s.markers.size())) return;
    juce::String error;
    if (action == 2)
    {
        if (! editor.removeMarker (index, error)) report (error);
        edited();
        return;
    }
    askText ("Rename marker", "The new name", s.markers[(size_t) index].name, [this, index] (const juce::String& text)
    {
        juce::String e;
        if (! editor.renameMarker (index, text, e)) report (e);
        edited();
    });
}

// A small text box in a modal window; the answer goes to `onOk` when it is OK.
void ArrangeView::askText (const juce::String& title, const juce::String& prompt, const juce::String& initial, std::function<void (const juce::String&)> onOk)
{
    auto* box = new juce::AlertWindow (title, prompt, juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("text", initial, {});
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([box, onOk] (int result)
    {
        if (result == 1) onOk (box->getTextEditorContents ("text"));
    }), true);
}

// ---- files ---------------------------------------------------------------------------------------------------------------------------
bool ArrangeView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile (juce::File (f))) return true;
    return false;
}

void ArrangeView::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (y < laneTop() || ! onFilesDropped) return;
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const int track = trackAt (y, (int) s.tracks.size());
    const auto at = x < kHeaderWidth ? juce::int64 (0) : editor.snap ((juce::int64) juce::jmax (0.0, xToSample (x, pps)));
    juce::StringArray audio;
    for (auto& f : files)
        if (isAudioFile (juce::File (f))) audio.add (f);
    if (! audio.isEmpty()) onFilesDropped (audio, track, at);
}
} // namespace olympus::gui
