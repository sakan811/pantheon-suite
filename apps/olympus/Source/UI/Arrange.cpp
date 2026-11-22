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

juce::String trackFaderTooltip (double db)
{
    const juce::String value = db <= -100.0 ? juce::String ("-inf") : (db >= 0.0 ? "+" : "") + juce::String (db, 1);
    return "Fader " + value + " dB (drag, double-click for 0 dB)";
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
    auto consider = [&] (double c)
    {
        largest = std::max (largest, c);
        if (c * pxPerBeat >= kMinGap && (best < 0.0 || c < best)) best = c;
    };
    double c = 0.125;
    for (int k = 0; k <= 12; ++k, c *= 2.0) consider (c);
    c = bpb;
    for (int k = 0; k <= 8; ++k, c *= 2.0) consider (c);
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
        struct PluginParamInfo { int slot = 0; int param = 0; juce::String name; };
        juce::String name;
        float gainDb = 0.0f, pan = 0.0f;
        bool mute = false, solo = false;
        bool armed = false;
        int inputLeft = 0, inputRight = -1;
        float peakL = 0.0f, peakR = 0.0f; // only when taken with peaks: the maximum since the last read
        bool isMidi = false;
        TrackKind kind = TrackKind::Audio;
        int outputTrack = -1;
        std::vector<AuxSend> sends;
        std::vector<Clip> clips;
        std::vector<MidiClip> midiClips;
        bool showAutomation = false;
        juce::String activeAutoTarget { "volume" };
        std::vector<AutomationLane> automation;
        std::vector<PluginParamInfo> pluginParams;
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

static bool sameArrangeVisual (const ArrangeSnapshot& a, const ArrangeSnapshot& b)
{
    if (! juce::exactlyEqual (a.rate, b.rate) || a.grid != b.grid || a.loopOn != b.loopOn || a.loopStart != b.loopStart || a.loopEnd != b.loopEnd)
        return false;
    if (a.tempo.numerator != b.tempo.numerator || a.tempo.denominator != b.tempo.denominator
        || a.tempo.points.size() != b.tempo.points.size())
        return false;
    for (size_t i = 0; i < a.tempo.points.size(); ++i)
        if (! juce::exactlyEqual (a.tempo.points[i].beat, b.tempo.points[i].beat) || ! juce::exactlyEqual (a.tempo.points[i].bpm, b.tempo.points[i].bpm))
            return false;
    if (a.markers.size() != b.markers.size())
        return false;
    for (size_t i = 0; i < a.markers.size(); ++i)
        if (a.markers[i].start != b.markers[i].start || a.markers[i].name != b.markers[i].name)
            return false;
    if (a.tracks.size() != b.tracks.size())
        return false;
    for (size_t i = 0; i < a.tracks.size(); ++i)
    {
        const auto& ta = a.tracks[i];
        const auto& tb = b.tracks[i];
        if (ta.name != tb.name || ta.mute != tb.mute || ta.isMidi != tb.isMidi || ta.kind != tb.kind || ta.outputTrack != tb.outputTrack
            || ta.sends.size() != tb.sends.size() || ta.showAutomation != tb.showAutomation || ta.activeAutoTarget != tb.activeAutoTarget
            || ta.clips.size() != tb.clips.size() || ta.midiClips.size() != tb.midiClips.size() || ta.automation.size() != tb.automation.size())
            return false;
        for (size_t si = 0; si < ta.sends.size(); ++si)
        {
            const auto& sa = ta.sends[si];
            const auto& sb = tb.sends[si];
            if (sa.targetTrack != sb.targetTrack || ! juce::exactlyEqual (sa.levelDb, sb.levelDb)
                || sa.preFader != sb.preFader || sa.mute != sb.mute)
                return false;
        }
        for (size_t mc = 0; mc < ta.midiClips.size(); ++mc)
        {
            const auto& ma = ta.midiClips[mc];
            const auto& mb = tb.midiClips[mc];
            if (ma.start != mb.start || ma.length != mb.length || ma.mute != mb.mute || ma.name != mb.name
                || ma.notes.size() != mb.notes.size())
                return false;
            for (size_t ni = 0; ni < ma.notes.size(); ++ni)
            {
                const auto& na = ma.notes[ni];
                const auto& nb = mb.notes[ni];
                if (! juce::exactlyEqual (na.startBeat, nb.startBeat) || ! juce::exactlyEqual (na.durationBeats, nb.durationBeats)
                    || na.pitch != nb.pitch || na.velocity != nb.velocity || na.channel != nb.channel)
                    return false;
            }
        }
        for (size_t l = 0; l < ta.automation.size(); ++l)
        {
            const auto& la = ta.automation[l];
            const auto& lb = tb.automation[l];
            if (la.targetId != lb.targetId || la.enabled != lb.enabled || la.points.size() != lb.points.size())
                return false;
            for (size_t p = 0; p < la.points.size(); ++p)
                if (la.points[p].sample != lb.points[p].sample
                    || ! juce::exactlyEqual (la.points[p].value, lb.points[p].value)
                    || la.points[p].curve != lb.points[p].curve)
                    return false;
        }
        for (size_t c = 0; c < ta.clips.size(); ++c)
        {
            const auto& ca = ta.clips[c];
            const auto& cb = tb.clips[c];
            if (ca.start != cb.start || ca.offset != cb.offset || ca.length != cb.length
                || ! juce::exactlyEqual (ca.gainDb, cb.gainDb) || ca.fadeIn != cb.fadeIn || ca.fadeOut != cb.fadeOut
                || ca.mute != cb.mute || ca.name != cb.name || ca.audio.get() != cb.audio.get())
                return false;
        }
    }
    return true;
}

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
        for (auto* b : std::initializer_list<juce::Component*> { &muteButton, &soloButton, &autoButton, &autoTargetButton, &autoEnableButton, &fxButton, &removeButton, &armButton, &inputButton }) b->setWantsKeyboardFocus (false);
        faderSlider.setWantsKeyboardFocus (false);
        panKnob.setWantsKeyboardFocus (false);
        autoButton.setClickingTogglesState (true);
        autoButton.lamp = colours::accent;
        autoButton.setTooltip ("Show or hide the automation lane");
        autoButton.onClick = [this]
        {
            juce::String error;
            if (! view.editor.setTrackShowAutomation (index, autoButton.getToggleState(), error)) view.report (error);
            view.edited();
        };
        autoTargetButton.setTooltip ("Choose the parameter to automate");
        autoTargetButton.onClick = [this] { view.autoTargetMenu (index); };
        autoEnableButton.setClickingTogglesState (true);
        autoEnableButton.lamp = colours::good;
        autoEnableButton.setTooltip ("Enable or bypass this automation lane");
        autoEnableButton.onClick = [this]
        {
            const auto snap = view.snapshot();
            if (juce::isPositiveAndBelow (index, (int) snap.tracks.size()))
            {
                juce::String error;
                if (! view.editor.setAutomationLaneEnabled (index, snap.tracks[(size_t) index].activeAutoTarget,
                                                            autoEnableButton.getToggleState(), error))
                    view.report (error);
                view.edited();
            }
        };
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
        for (auto* b : std::initializer_list<juce::Component*> { &muteButton, &soloButton, &autoButton, &fxButton, &removeButton, &armButton, &inputButton }) addAndMakeVisible (*b);
        addChildComponent (autoTargetButton);
        addChildComponent (autoEnableButton);

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
        faderSlider.setTooltip (trackFaderTooltip (0.0));
        faderSlider.onDragStart = [this] { gainGesture = std::make_unique<Gesture> (view.editor, "Track gain"); };
        faderSlider.onDragEnd = [this] { gainGesture.reset(); };
        faderSlider.onValueChange = [this]
        {
            juce::String error;
            if (! view.editor.setTrackGain (index, (float) faderSlider.getValue(), error)) view.report (error);
            lastTooltipGain = (float) faderSlider.getValue();
            faderSlider.setTooltip (trackFaderTooltip (faderSlider.getValue()));
            view.changed();
        };
        addAndMakeVisible (faderSlider);
    }

    ~ArrangeTrackHeader() override
    {
        faderSlider.setLookAndFeel (nullptr);
    }

    void setState (const juce::String& title, float gainDb, float panValue, bool isMuted, bool isSoloed, bool isArmed, const juce::String& input,
                   bool showAuto, const juce::String& targetLabel, bool autoLaneEnabled, bool busTrack)
    {
        if (isBus != busTrack)
        {
            isBus = busTrack;
            repaint();
        }
        if (! editingName && nameLabel.getText() != title)
            nameLabel.setText (title, juce::dontSendNotification);
        if (! faderSlider.isMouseButtonDown())
        {
            if (std::abs (faderSlider.getValue() - (double) gainDb) > 1e-4)
                faderSlider.setValue (gainDb, juce::dontSendNotification);
            if (! juce::exactlyEqual (lastTooltipGain, gainDb))
            {
                lastTooltipGain = gainDb;
                const auto tip = trackFaderTooltip (gainDb);
                if (faderSlider.getTooltip() != tip)
                    faderSlider.setTooltip (tip);
            }
        }
        if (panGesture == nullptr && std::abs (panKnob.getValue() - (double) panValue) > 1e-4)
            panKnob.setValue (panValue);
        muteButton.setToggleState (isMuted, juce::dontSendNotification);
        soloButton.setToggleState (isSoloed, juce::dontSendNotification);
        armButton.setToggleState (isArmed, juce::dontSendNotification);
        armButton.setEnabled (! isBus);
        armButton.setVisible (! isBus);
        inputButton.setEnabled (! isBus);
        inputButton.setVisible (! isBus);
        if (isBus)
        {
            const juce::String busTip ("Bus track receives routed tracks/sends");
            if (armButton.getTooltip() != busTip) armButton.setTooltip (busTip);
            if (inputButton.getTooltip() != busTip) inputButton.setTooltip (busTip);
        }
        else
        {
            const juce::String armTip ("Arm the track to record it (REC). Its input is beside it; the meter shows that input while the transport is stopped.");
            const juce::String inTip ("The device input this track records. Click to choose it.");
            if (armButton.getTooltip() != armTip) armButton.setTooltip (armTip);
            if (inputButton.getTooltip() != inTip) inputButton.setTooltip (inTip);
        }
        if (inputButton.getButtonText() != input) inputButton.setButtonText (input);
        autoButton.setToggleState (showAuto, juce::dontSendNotification);
        if (autoTargetButton.isVisible() != showAuto) autoTargetButton.setVisible (showAuto);
        if (autoEnableButton.isVisible() != showAuto) autoEnableButton.setVisible (showAuto);
        if (autoTargetButton.getButtonText() != targetLabel) autoTargetButton.setButtonText (targetLabel);
        autoEnableButton.setToggleState (autoLaneEnabled, juce::dontSendNotification);
    }

    // The meter: the level falls back by a few dB between reads, the held mark more slowly (MeterBallistics, shared with the master).
    void addPeaks (float peakL, float peakR)
    {
        const float prevLevel = meter.level, prevHold = meter.hold;
        meter.add ((float) dbOf (std::max (peakL, peakR)));
        if (std::abs (meter.level - prevLevel) > 1e-4f || std::abs (meter.hold - prevHold) > 1e-4f)
            repaint();
    }

    void resized() override
    {
        const int w = getWidth();
        nameLabel.setBounds (8, 5, w - 114, 20);
        autoButton.setBounds (w - 102, 5, 36, 20);
        fxButton.setBounds (w - 62, 5, 32, 20);
        removeButton.setBounds (w - 26, 5, 20, 20);
        muteButton.setBounds (8, 30, 26, 22);
        soloButton.setBounds (38, 30, 26, 22);
        panKnob.setBounds (66, 27, 100, 32);
        armButton.setBounds (8, 62, 26, 22);
        inputButton.setBounds (38, 62, 58, 22);
        faderSlider.setBounds (102, 66, w - 110, 16);
        if (getHeight() > ArrangeView::kLaneHeight)
        {
            autoTargetButton.setBounds (8, ArrangeView::kLaneHeight + 12, w - 58, 24);
            autoEnableButton.setBounds (w - 46, ArrangeView::kLaneHeight + 12, 38, 24);
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const auto base = isBus ? colours::panel.interpolatedWith (colours::accent, 0.08f) : colours::panel;
        ui::hw::fillMetal (g, area, base);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRect (area.removeFromBottom (1.0f));
        if (isBus)
        {
            const juce::Rectangle<float> badge (8.0f, 64.0f, 52.0f, 18.0f);
            g.setColour (colours::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (colours::accent.withAlpha (0.65f));
            g.drawRoundedRectangle (badge, 3.0f, 1.0f);
            g.setFont (ui::labelFont (9.5f));
            g.setColour (colours::accent);
            g.drawText ("[BUS]", badge, juce::Justification::centred, false);
        }
        if (getHeight() > ArrangeView::kLaneHeight)
        {
            g.setColour (colours::panelEdge);
            g.fillRect (0, ArrangeView::kLaneHeight, getWidth(), 1);
        }

        ui::MeterCell cell;
        cell.area = juce::Rectangle<float> (172.0f, 17.0f, 80.0f, ui::MeterCell::kTitleHeight + ui::MeterCell::kWindowHeight);
        paintLevelBar (g, cell.drawTrack (g), meter);
    }

    ArrangeView& view;
    int index;
    ArrangeLamp armButton { "R" };
    juce::TextButton inputButton { "IN" };
    ArrangeLamp autoButton { "AUTO" };
    juce::TextButton autoTargetButton { "VOL" };
    juce::Slider& fader() { return faderSlider; }
    bool isInteracting() const { return editingName || faderSlider.isMouseButtonDown() || panGesture != nullptr || gainGesture != nullptr; }

private:
    bool editingName = false;
    bool isBus = false;
    float lastTooltipGain = std::numeric_limits<float>::quiet_NaN();
    MeterBallistics meter;
    juce::Label nameLabel;
    ArrangeLamp muteButton { "M" }, soloButton { "S" }, fxButton { "FX" }, removeButton { "X" }, autoEnableButton { "ON" };
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
    pendingDialogEnd = nullptr;
    if (activeDialog != nullptr)
    {
        activeDialog->exitModalState (0);
        delete activeDialog.getComponent();
    }
    dialogFlag() = false;
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

juce::Button* ArrangeView::automationButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) headers.size()) ? &headers[(size_t) track]->autoButton : nullptr;
}

juce::Button* ArrangeView::autoTargetButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) headers.size()) ? &headers[(size_t) track]->autoTargetButton : nullptr;
}

juce::Slider* ArrangeView::faderSlider (int track)
{
    return juce::isPositiveAndBelow (track, (int) headers.size()) ? &headers[(size_t) track]->fader() : nullptr;
}

juce::Rectangle<int> ArrangeView::autoLaneBounds (int track) const
{
    return autoLaneRect (snapshot(), track);
}

bool ArrangeView::isInteracting() const
{
    if (mode != Mode::none || gesture != nullptr || hasOpenDialog())
        return true;
    for (const auto& h : headers)
        if (h->isInteracting())
            return true;
    return false;
}

std::vector<ClipRef> ArrangeView::effectiveSelection() const
{
    if (selectedClips.size() > 1)
        return selectedClips;
    if (selectedClips.size() == 1)
    {
        const auto& c = selectedClips[0];
        const bool matches = c.isMidi ? (! selected.valid() && selectedMidi.track == c.track && selectedMidi.clip == c.clip)
                                      : (! selectedMidi.valid() && selected == c);
        if (matches)
            return selectedClips;
    }
    std::vector<ClipRef> out;
    if (selected.valid())
        out.push_back ({ selected.track, selected.clip, false });
    if (selectedMidi.valid())
        out.push_back ({ selectedMidi.track, selectedMidi.clip, true });
    return out;
}

bool ArrangeView::isSelected (int track, int index, bool isMidi) const
{
    for (const auto& r : effectiveSelection())
        if (r.track == track && r.clip == index && r.isMidi == isMidi)
            return true;
    return false;
}

int ArrangeView::selectedCount() const
{
    return (int) effectiveSelection().size();
}

void ArrangeView::syncPrimaryFromSelection()
{
    selected = {};
    selectedMidi = {};
    for (const auto& r : selectedClips)
    {
        if (! r.valid()) continue;
        if (! r.isMidi && ! selected.valid())
            selected = r;
        else if (r.isMidi && ! selectedMidi.valid())
            selectedMidi = { r.track, r.clip };
    }
}

void ArrangeView::clearSelection()
{
    selectedClips.clear();
    selected = {};
    selectedMidi = {};
    marqueeRect = {};
    marqueeBaseSelection.clear();
    repaint();
}

void ArrangeView::selectSingleClip (ClipRef ref)
{
    if (ref.valid())
        selectedClips = { ref };
    else
        selectedClips.clear();
    syncPrimaryFromSelection();
}

void ArrangeView::toggleClipSelection (ClipRef ref)
{
    if (! ref.valid()) return;
    if (selectedClips.empty() && (selected.valid() || selectedMidi.valid()))
        selectedClips = effectiveSelection();
    auto it = std::find (selectedClips.begin(), selectedClips.end(), ref);
    if (it != selectedClips.end())
        selectedClips.erase (it);
    else
        selectedClips.push_back (ref);
    syncPrimaryFromSelection();
}

void ArrangeView::selectAll()
{
    const auto s = snapshot();
    selectedClips.clear();
    for (int t = 0; t < (int) s.tracks.size(); ++t)
    {
        for (int c = 0; c < (int) s.tracks[(size_t) t].clips.size(); ++c)
            selectedClips.push_back ({ t, c, false });
        for (int mc = 0; mc < (int) s.tracks[(size_t) t].midiClips.size(); ++mc)
            selectedClips.push_back ({ t, mc, true });
    }
    syncPrimaryFromSelection();
    repaint();
}

std::shared_ptr<const ArrangeSnapshot> ArrangeView::snapshotPtr (bool takePeaks) const
{
    // Fast path when the session's revision has not changed since lastSnapshot was built:
    // - without peaks (paint, hit testing), return lastSnapshot immediately without acquiring session.lock;
    // - with peaks (30 Hz refresh), try-lock briefly to exchange the track peaks into peakScratch, release before cloning, and avoid
    //   allocating when all peaks are 0.0f (lastSnapshot always keeps 0.0f peaks). Never sleep/spin if the lock is busy.
    if (lastSnapshot != nullptr && session.revision.load (std::memory_order_acquire) == lastSnapshotRevision)
    {
        if (! takePeaks)
            return lastSnapshot;

        bool lockedAndMatched = false;
        bool anyPeak = false;
        {
            const juce::ScopedTryLock sl (session.lock);
            if (sl.isLocked() && session.tracks.size() == lastSnapshot->tracks.size())
            {
                lockedAndMatched = true;
                const size_t n = session.tracks.size();
                peakScratch.resize (n);
                for (size_t i = 0; i < n; ++i)
                {
                    const float pL = session.tracks[i]->peakL.exchange (0.0f);
                    const float pR = session.tracks[i]->peakR.exchange (0.0f);
                    peakScratch[i] = { pL, pR };
                    if (pL != 0.0f || pR != 0.0f)
                        anyPeak = true;
                }
            }
        }
        if (! lockedAndMatched || ! anyPeak)
            return lastSnapshot;

        auto copy = std::make_shared<ArrangeSnapshot> (*lastSnapshot);
        for (size_t i = 0; i < copy->tracks.size(); ++i)
        {
            copy->tracks[i].peakL = peakScratch[i].first;
            copy->tracks[i].peakR = peakScratch[i].second;
        }
        return copy;
    }

    // Rebuild path (first snapshot or revision changed). Try-lock first; never sleep/spin if another thread holds the lock when a
    // cached snapshot exists or a render is running. Only wait once if lastSnapshot is null and no render is running.
    auto buildSnapshot = [this, takePeaks]() -> std::shared_ptr<const ArrangeSnapshot>
    {
        const auto rev = session.revision.load (std::memory_order_acquire);
        auto s = std::make_shared<ArrangeSnapshot>();
        s->rate = session.sampleRate > 0.0 ? session.sampleRate : kFallbackRate;
        s->tempo = session.tempo;
        s->grid = session.grid;
        s->markers = session.markers;
        s->loopOn = session.loopOn;
        s->loopStart = session.loopStart;
        s->loopEnd = session.loopEnd;
        const size_t n = session.tracks.size();
        if (takePeaks)
            peakScratch.resize (n);
        bool anyPeak = false;
        for (size_t i = 0; i < n; ++i)
        {
            const auto& t = session.tracks[i];
            ArrangeSnapshot::TrackView v;
            v.name = t->name;
            v.gainDb = t->gainDb;
            v.pan = t->pan;
            v.mute = t->mute;
            v.solo = t->solo;
            v.armed = t->armed;
            v.inputLeft = t->inputLeft;
            v.inputRight = t->inputRight;
            v.isMidi = t->isMidi;
            v.kind = t->kind;
            v.outputTrack = t->outputTrack;
            v.sends = t->sends;
            v.clips = t->clips;
            v.midiClips = t->midiClips;
            v.showAutomation = t->showAutomation;
            v.activeAutoTarget = t->activeAutoTarget;
            v.automation = t->automation;
            for (int si = 0; si < (int) t->chain.size(); ++si)
            {
                const auto* inst = t->chain[(size_t) si]->instance.get();
                if (inst == nullptr) continue;
                const auto& params = inst->getParameters();
                for (int pi = 0; pi < params.size(); ++pi)
                {
                    if (params[pi] != nullptr)
                        v.pluginParams.push_back ({ si, pi, "FX" + juce::String (si + 1) + ":" + params[pi]->getName (24) });
                }
            }
            if (takePeaks)
            {
                const float pL = t->peakL.exchange (0.0f);
                const float pR = t->peakR.exchange (0.0f);
                peakScratch[i] = { pL, pR };
                if (pL != 0.0f || pR != 0.0f)
                    anyPeak = true;
            }
            s->length = juce::jmax (s->length, t->length());
            s->tracks.push_back (std::move (v));
        }
        lastSnapshot = s;
        lastSnapshotRevision = rev;
        if (! takePeaks || ! anyPeak)
            return s;

        auto copy = std::make_shared<ArrangeSnapshot> (*s);
        for (size_t i = 0; i < copy->tracks.size(); ++i)
        {
            copy->tracks[i].peakL = peakScratch[i].first;
            copy->tracks[i].peakR = peakScratch[i].second;
        }
        return copy;
    };

    {
        const juce::ScopedTryLock sl (session.lock);
        if (sl.isLocked())
            return buildSnapshot();
    }
    if (lastSnapshot != nullptr || session.renderRunning())
        return lastSnapshot != nullptr ? lastSnapshot : std::make_shared<const ArrangeSnapshot>();

    const juce::ScopedLock waitSl (session.lock);
    return buildSnapshot();
}

const ArrangeSnapshot* ArrangeView::snapshotPtrForTest (bool takePeaks) const
{
    return snapshotPtr (takePeaks).get();
}

ArrangeSnapshot ArrangeView::snapshot (bool takePeaks) const
{
    return *snapshotPtr (takePeaks);
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

int ArrangeView::trackHeight (const ArrangeSnapshot& s, int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) s.tracks.size()))
        return kLaneHeight;
    return kLaneHeight + (s.tracks[(size_t) index].showAutomation ? kAutoLaneHeight : 0);
}

int ArrangeView::trackTop (const ArrangeSnapshot& s, int index) const
{
    int y = laneTop() - scrollY;
    const int n = juce::jmin (index, (int) s.tracks.size());
    for (int i = 0; i < n; ++i)
        y += trackHeight (s, i);
    return y;
}

int ArrangeView::trackAt (const ArrangeSnapshot& s, int y) const
{
    if (y < laneTop() || y >= getHeight() - kScrollStripHeight) return -1;
    int top = laneTop() - scrollY;
    for (int i = 0; i < (int) s.tracks.size(); ++i)
    {
        const int h = trackHeight (s, i);
        if (y >= top && y < top + h)
            return i;
        top += h;
    }
    return -1;
}

juce::Rectangle<int> ArrangeView::clipRect (const ArrangeSnapshot& s, int track, juce::int64 start, juce::int64 length, double pps) const
{
    const int x0 = sampleToX ((double) start, pps);
    const int x1 = sampleToX ((double) (start + length), pps);
    const int top = trackTop (s, track);
    return { x0, top + kClipPad, juce::jmax (2, x1 - x0), kLaneHeight - 2 * kClipPad };
}

juce::Rectangle<int> ArrangeView::autoLaneRect (const ArrangeSnapshot& s, int track) const
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()) || ! s.tracks[(size_t) track].showAutomation)
        return {};
    const int y = trackTop (s, track) + kLaneHeight;
    return { kHeaderWidth, y, juce::jmax (0, getWidth() - kHeaderWidth), kAutoLaneHeight };
}

float ArrangeView::valueToAutoY (float val, const juce::Rectangle<int>& r) const
{
    const float usableH = (float) juce::jmax (1, r.getHeight() - 8);
    return (float) (r.getBottom() - 4) - juce::jlimit (0.0f, 1.0f, val) * usableH;
}

float ArrangeView::autoYToValue (int y, const juce::Rectangle<int>& r) const
{
    const float usableH = (float) juce::jmax (1, r.getHeight() - 8);
    return juce::jlimit (0.0f, 1.0f, ((float) (r.getBottom() - 4) - (float) y) / usableH);
}

int ArrangeView::hitAutoPoint (const ArrangeSnapshot& s, int track, juce::Point<int> p, double pps) const
{
    const auto r = autoLaneRect (s, track);
    if (r.isEmpty() || ! r.contains (p))
        return -1;
    const auto& t = s.tracks[(size_t) track];
    for (const auto& lane : t.automation)
    {
        if (lane.targetId != t.activeAutoTarget)
            continue;
        for (int i = 0; i < (int) lane.points.size(); ++i)
        {
            const float px = (float) sampleToX ((double) lane.points[(size_t) i].sample, pps);
            const float py = valueToAutoY (lane.points[(size_t) i].value, r);
            if (std::abs ((float) p.x - px) <= 7.0f && std::abs ((float) p.y - py) <= 7.0f)
                return i;
        }
        break;
    }
    return -1;
}

int ArrangeView::markerAtForTest (juce::Point<int> p) const
{
    const auto s = snapshot();
    return markerAt (s, p, pxPerSample (s.rate));
}

int ArrangeView::markerAt (const ArrangeSnapshot& s, juce::Point<int> p, double pps) const
{
    const auto font = ui::labelFont (9.5f);
    for (int i = 0; i < (int) s.markers.size(); ++i)
    {
        const int x = sampleToX ((double) s.markers[(size_t) i].start, pps);
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, s.markers[(size_t) i].name, 0.0f, 0.0f);
        const float width = juce::jmin (glyphs.getBoundingBox (0, -1, true).getWidth() + 14.0f, 165.0f); // the name is drawn in 160 px from x+5: the hit area is not wider than what is seen
        if (p.x >= x - 4 && (float) p.x <= (float) x + width) return i;
    }
    return -1;
}

int ArrangeView::maxScrollY (const ArrangeSnapshot& s) const
{
    int total = 0;
    for (int i = 0; i < (int) s.tracks.size(); ++i)
        total += trackHeight (s, i);
    return juce::jmax (0, total - (getHeight() - laneTop() - kScrollStripHeight));
}

juce::Rectangle<int> ArrangeView::scrollStripBounds() const
{
    const int h = getHeight();
    if (h <= laneTop() + kScrollStripHeight || getWidth() <= kHeaderWidth)
        return {};
    return { kHeaderWidth, h - kScrollStripHeight, getWidth() - kHeaderWidth, kScrollStripHeight };
}

void ArrangeView::scrollThumbMetrics (const ArrangeSnapshot& s, double pps, float& thumbX, float& thumbW, double& totalSamples) const
{
    const auto strip = scrollStripBounds();
    const float sw = (float) juce::jmax (1, strip.getWidth());
    const double visibleSpan = (double) juce::jmax (1, strip.getWidth()) / pps;
    totalSamples = juce::jmax ((double) s.length, visibleSpan, 1.0); // the content only: overscroll must not stretch the strip's own total
    const double fracLen = juce::jlimit (0.0, 1.0, visibleSpan / totalSamples);
    thumbW = juce::jlimit (1.0f, sw, juce::jmax (20.0f, (float) (fracLen * sw)));
    const double maxScroll = juce::jmax (0.0, totalSamples - visibleSpan);
    const double scrollRatio = maxScroll > 0.0 ? juce::jlimit (0.0, 1.0, scrollSamples / maxScroll) : 0.0;
    thumbX = (float) strip.getX() + (float) scrollRatio * (sw - thumbW);
}

bool ArrangeView::hitFadeIn (const ArrangeSnapshot& s, ClipRef ref, juce::Point<int> p, double pps) const
{
    if (! ref.valid() || ! juce::isPositiveAndBelow (ref.track, (int) s.tracks.size())) return false;
    const auto& clips = s.tracks[(size_t) ref.track].clips;
    if (! juce::isPositiveAndBelow (ref.clip, (int) clips.size())) return false;
    const auto& c = clips[(size_t) ref.clip];
    const auto r = clipRect (s, ref.track, c.start, c.length, pps);
    if (r.getWidth() < 28 || p.y < r.getY() || p.y - r.getY() >= kFadeZone) return false;
    if (c.fadeIn > 0)
    {
        const int fadeInX = sampleToX ((double) (c.start + c.fadeIn), pps);
        return std::abs (p.x - fadeInX) <= kFadeGrab;
    }
    return p.x > r.getX() + kEdgeGrab && p.x <= r.getX() + kEdgeGrab + kFadeGrab;
}

bool ArrangeView::hitFadeOut (const ArrangeSnapshot& s, ClipRef ref, juce::Point<int> p, double pps) const
{
    if (! ref.valid() || ! juce::isPositiveAndBelow (ref.track, (int) s.tracks.size())) return false;
    const auto& clips = s.tracks[(size_t) ref.track].clips;
    if (! juce::isPositiveAndBelow (ref.clip, (int) clips.size())) return false;
    const auto& c = clips[(size_t) ref.clip];
    const auto r = clipRect (s, ref.track, c.start, c.length, pps);
    if (r.getWidth() < 28 || p.y < r.getY() || p.y - r.getY() >= kFadeZone) return false;
    if (c.fadeOut > 0)
    {
        const int fadeOutX = sampleToX ((double) (c.end() - c.fadeOut), pps);
        return std::abs (p.x - fadeOutX) <= kFadeGrab;
    }
    return p.x >= r.getRight() - kEdgeGrab - kFadeGrab && p.x < r.getRight() - kEdgeGrab;
}

bool ArrangeView::hitFadeIn (ClipRef ref, juce::Point<int> point) const
{
    const auto s = snapshot();
    return hitFadeIn (s, ref, point, pxPerSample (s.rate));
}

bool ArrangeView::hitFadeOut (ClipRef ref, juce::Point<int> point) const
{
    const auto s = snapshot();
    return hitFadeOut (s, ref, point, pxPerSample (s.rate));
}

void ArrangeView::seekTo (juce::int64 sample)
{
    if (recordingFrom >= 0.0)
        return; // a take is one continuous recording: the transport refuses a seek, so the view does not ask (a ruler drag would stack one error box per event)
    const auto at = juce::jmax ((juce::int64) 0, sample);
    playhead = (double) at;
    if (onSeek) onSeek (at);
    repaint();
}

void ArrangeView::layoutHeaders()
{
    layoutHeaders (snapshot());
}

void ArrangeView::layoutHeaders (const ArrangeSnapshot& s)
{
    scrollY = juce::jlimit (0, maxScrollY (s), scrollY);
    headerPort.setBounds (0, laneTop(), kHeaderWidth, juce::jmax (0, getHeight() - laneTop() - kScrollStripHeight));
    for (int i = 0; i < (int) headers.size(); ++i)
        headers[(size_t) i]->setBounds (0, trackTop (s, i) - laneTop(), kHeaderWidth, trackHeight (s, i));
}

void ArrangeView::syncHeaders (bool readMeters)
{
    syncHeaders (snapshot (readMeters), readMeters);
}

void ArrangeView::syncHeaders (const ArrangeSnapshot& s, bool readMeters)
{
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
        juce::String targetLabel = "VOL";
        if (t.activeAutoTarget == "pan")
            targetLabel = "PAN";
        else if (t.activeAutoTarget.startsWith ("plugin:"))
        {
            targetLabel = t.activeAutoTarget.fromFirstOccurrenceOf ("plugin:", false, false);
            const auto rest = targetLabel;
            const int colon = rest.indexOfChar (':');
            if (colon > 0)
            {
                const int sIdx = rest.substring (0, colon).getIntValue();
                const int pIdx = rest.substring (colon + 1).getIntValue();
                for (const auto& info : t.pluginParams)
                    if (info.slot == sIdx && info.param == pIdx) { targetLabel = info.name; break; }
            }
        }
        bool laneEnabled = true;
        for (const auto& lane : t.automation)
            if (lane.targetId == t.activeAutoTarget) { laneEnabled = lane.enabled; break; }
        h.setState (t.name, t.gainDb, t.pan, t.mute, t.solo, t.armed, inputText (t.inputLeft, t.inputRight),
                    t.showAutomation, targetLabel, laneEnabled, t.kind == TrackKind::Bus);
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
    if (! selectedClips.empty())
    {
        const auto oldSize = selectedClips.size();
        selectedClips.erase (std::remove_if (selectedClips.begin(), selectedClips.end(), [&] (const ClipRef& r)
        {
            if (! r.valid() || r.track >= n) return true;
            const int count = r.isMidi ? (int) s.tracks[(size_t) r.track].midiClips.size()
                                       : (int) s.tracks[(size_t) r.track].clips.size();
            return r.clip >= count;
        }), selectedClips.end());
        if (selectedClips.size() != oldSize)
            syncPrimaryFromSelection();
    }
    if (selected.valid())
    {
        const bool ok = selected.track < n && selected.clip < (int) s.tracks[(size_t) selected.track].clips.size();
        if (! ok) selected = {};
    }
    if (selectedMidi.valid())
    {
        const bool ok = selectedMidi.track < n && selectedMidi.clip < (int) s.tracks[(size_t) selectedMidi.track].midiClips.size();
        if (! ok) selectedMidi = {};
    }
    layoutHeaders (s);
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
    const auto sp = snapshotPtr (true);
    const auto& s = *sp;
    syncHeaders (s, true);
    const double pps = pxPerSample (s.rate);
    const double span = juce::jmax (0.0, (double) (getWidth() - kHeaderWidth) / pps);
    if (followPlayhead && ! juce::exactlyEqual (playhead, lastPlayhead) && (playhead < scrollSamples || playhead > scrollSamples + span))
    {
        const double target = juce::jmax (0.0, playhead - span * 0.125);
        // a take runs past the end of the audio, so following it may scroll past the content; otherwise the view stays on the content
        scrollSamples = recordingFrom >= 0.0 ? target : juce::jlimit (0.0, juce::jmax (0.0, (double) s.length - span), target);
    }

    const bool fullVisualChanged = ! hasRefreshed
        || lastRefreshSnapshot == nullptr
        || ! sameArrangeVisual (s, *lastRefreshSnapshot)
        || ! juce::exactlyEqual (scrollSamples, lastRefreshScrollSamples)
        || scrollY != lastRefreshScrollY
        || ! juce::exactlyEqual (zoom, lastRefreshZoom)
        || selected.track != lastRefreshSelected.track
        || selected.clip != lastRefreshSelected.clip
        || selected.isMidi != lastRefreshSelected.isMidi
        || selectedMidi.track != lastRefreshSelectedMidi.track
        || selectedMidi.clip != lastRefreshSelectedMidi.clip
        || selectedClips != lastRefreshSelectedClips
        || marqueeRect != lastRefreshMarqueeRect
        || ! juce::exactlyEqual (recordingFrom, lastRefreshRecFrom)
        || ! juce::exactlyEqual (recordingTo, lastRefreshRecTo)
        || recordingTracks != lastRefreshRecTracks;

    if (fullVisualChanged)
    {
        repaint();
    }
    else if (! juce::exactlyEqual (playhead, lastPlayhead))
    {
        const int oldPx = sampleToX (lastPlayhead, pps);
        const int newPx = sampleToX (playhead, pps);
        if (oldPx != newPx)
        {
            const int x0 = juce::jmax (kHeaderWidth - 5, juce::jmin (oldPx, newPx) - 5);
            const int x1 = juce::jmin (getWidth(), juce::jmax (oldPx, newPx) + 6);
            if (x1 > x0)
                repaint (x0, 0, x1 - x0, getHeight());
        }
    }

    hasRefreshed = true;
    lastPlayhead = playhead;
    lastRefreshSnapshot = sp;
    lastRefreshScrollSamples = scrollSamples;
    lastRefreshScrollY = scrollY;
    lastRefreshZoom = zoom;
    lastRefreshSelected = selected;
    lastRefreshSelectedMidi = selectedMidi;
    lastRefreshSelectedClips = selectedClips;
    lastRefreshMarqueeRect = marqueeRect;
    lastRefreshRecFrom = recordingFrom;
    lastRefreshRecTo = recordingTo;
    lastRefreshRecTracks = recordingTracks;
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

void ArrangeView::zoomBy (double factor)
{
    const auto s = snapshot();
    const double oldPps = pxPerSample (s.rate);
    const int timelineWidth = juce::jmax (1, getWidth() - kHeaderWidth);
    const double leftSample = scrollSamples;
    const double rightSample = leftSample + (double) timelineWidth / oldPps;
    const bool playheadVisible = playhead >= leftSample && playhead <= rightSample;
    const double anchorSample = playheadVisible ? playhead : (leftSample + rightSample) * 0.5;
    const double anchorPxFromLeft = (anchorSample - leftSample) * oldPps;

    zoom = juce::jlimit (kMinZoom, kMaxZoom, zoom * factor);
    const double newPps = pxPerSample (s.rate);
    scrollSamples = juce::jlimit (0.0, juce::jmax (0.0, (double) s.length - (double) timelineWidth / newPps),
                                  juce::jmax (0.0, anchorSample - anchorPxFromLeft / newPps));
    repaint();
}

void ArrangeView::scrollTo (juce::int64 sample)
{
    const auto s = snapshot();
    const double span = (double) juce::jmax (1, getWidth() - kHeaderWidth) / pxPerSample (s.rate);
    scrollSamples = juce::jlimit (0.0, juce::jmax (0.0, (double) s.length - span), (double) juce::jmax ((juce::int64) 0, sample));
    repaint();
}

// ---- painting --------------------------------------------------------------------------------------------------------------------
void ArrangeView::drawWave (juce::Graphics& g, const Clip& clip, juce::Rectangle<int> area, double pps, juce::Colour colour)
{
    if (clip.audio == nullptr || clip.audio->getNumSamples() == 0 || clip.length <= 0) return;
    const auto visible = area.getIntersection (g.getClipBounds());
    if (visible.isEmpty()) return;
    const auto& buffer = *clip.audio;
    const auto& peaks = waves->of (clip.audio);
    const auto frames = (juce::int64) buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    const float mid = (float) area.getCentreY(), half = juce::jmax (1.0f, (float) area.getHeight() * 0.5f - 3.0f);
    const int blockSize = ArrangeWaves::kBlock;

    juce::RectangleList<float> rects;
    rects.ensureStorageAllocated (visible.getWidth());
    for (int x = visible.getX(); x < visible.getRight(); ++x)
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
            const size_t numBlocks = b1 - b0 + 1;
            const size_t stride = numBlocks > 4 ? numBlocks / 4 : 1;
            for (size_t k = b0; k <= b1; k += stride)
            {
                lo = std::min (lo, peaks.lo[k]);
                hi = std::max (hi, peaks.hi[k]);
            }
            if (stride > 1)
            {
                lo = std::min (lo, peaks.lo[b1]);
                hi = std::max (hi, peaks.hi[b1]);
            }
        }
        if (hi < lo) continue;
        const float y1 = mid - juce::jlimit (-1.0f, 1.0f, hi) * half;
        const float y2 = mid - juce::jlimit (-1.0f, 1.0f, lo) * half;
        rects.addWithoutMerging (juce::Rectangle<float> ((float) x, y1, 1.0f, juce::jmax (1.0f, y2 - y1)));
    }
    g.setColour (colour);
    g.fillRectList (rects);
}

void ArrangeView::paintClip (juce::Graphics& g, const ArrangeSnapshot& s, double pps, int track, const Clip& clip, bool selectedNow)
{
    const auto rect = clipRect (s, track, clip.start, clip.length, pps);
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
    if (clip.fadeIn > 0)
    {
        g.setColour (colours::amber);
        g.fillRect (fadeInX - 3.0f, r.getY(), 6.0f, 6.0f);
    }
    else if (r.getWidth() >= 28.0f)
    {
        g.setColour (colours::amber.withAlpha (0.45f));
        g.fillRect (r.getX() + (float) kEdgeGrab, r.getY(), 6.0f, 6.0f);
    }
    if (clip.fadeOut > 0)
    {
        g.setColour (colours::amber);
        g.fillRect (fadeOutX - 3.0f, r.getY(), 6.0f, 6.0f);
    }
    else if (r.getWidth() >= 28.0f)
    {
        g.setColour (colours::amber.withAlpha (0.45f));
        g.fillRect (r.getRight() - (float) kEdgeGrab - 6.0f, r.getY(), 6.0f, 6.0f);
    }

    g.setColour (selectedNow ? colours::amber : colours::panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, selectedNow ? 2.0f : 1.0f);
    g.restoreState();
}

void ArrangeView::paintMidiClip (juce::Graphics& g, const ArrangeSnapshot& s, double pps, int track, const MidiClip& clip, bool selectedNow)
{
    const auto rect = clipRect (s, track, clip.start, clip.length, pps);
    const auto r = rect.toFloat();
    const bool dimmed = clip.mute || s.tracks[(size_t) track].mute;

    g.saveState();
    g.reduceClipRegion (rect.getIntersection (getLocalBounds().withTrimmedLeft (kHeaderWidth)));
    g.setColour (colours::panel.brighter (0.08f).withAlpha (0.94f));
    g.fillRoundedRectangle (r, 3.0f);

    // Header tag ("MIDI") + clip name
    const auto headerArea = r.reduced (4.0f, 2.0f).withHeight (13.0f);
    g.setFont (ui::labelFont (8.5f));
    g.setColour ((dimmed ? colours::dim : colours::accent).withAlpha (0.9f));
    g.drawText ("MIDI", headerArea.withWidth (28.0f), juce::Justification::centredLeft, false);
    g.setFont (ui::labelFont (9.5f));
    g.setColour (dimmed ? colours::dim : colours::text);
    g.drawText (clip.name + (clip.mute ? "  (muted)" : ""), headerArea.withTrimmedLeft (30.0f), juce::Justification::centredLeft, true);

    // Miniature piano-roll note preview
    const auto noteArea = r.reduced (3.0f, 3.0f).withTrimmedTop (14.0f);
    if (! clip.notes.empty() && noteArea.getWidth() >= 4.0f && noteArea.getHeight() >= 6.0f)
    {
        int minP = 127, maxP = 0;
        for (const auto& n : clip.notes)
        {
            minP = juce::jmin (minP, n.pitch);
            maxP = juce::jmax (maxP, n.pitch);
        }
        if (maxP - minP < 12)
        {
            const int mid = (minP + maxP) / 2;
            minP = juce::jmax (0, mid - 6);
            maxP = juce::jmin (127, minP + 12);
        }
        const int span = juce::jmax (1, maxP - minP + 1);
        const float noteH = juce::jlimit (2.0f, 6.0f, noteArea.getHeight() / (float) span);
        const double clipStartBeats = s.tempo.samplesToBeats ((double) clip.start);
        const auto barColour = (dimmed ? colours::dim : colours::accent).withAlpha (0.85f);
        g.setColour (barColour);
        for (const auto& n : clip.notes)
        {
            const float nx0 = (float) sampleToX (s.tempo.beatsToSamples (clipStartBeats + n.startBeat), pps);
            const float nx1 = (float) sampleToX (s.tempo.beatsToSamples (clipStartBeats + n.startBeat + n.durationBeats), pps);
            const float frac = (float) (n.pitch - minP) / (float) span;
            const float ny = noteArea.getBottom() - noteH - frac * juce::jmax (1.0f, noteArea.getHeight() - noteH);
            g.fillRoundedRectangle (nx0, ny, juce::jmax (2.0f, nx1 - nx0), noteH, 1.0f);
        }
    }

    if (clip.mute)
    {
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (r, 3.0f);
    }

    g.setColour (selectedNow ? colours::amber : colours::accent.withAlpha (0.55f));
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, selectedNow ? 2.0f : 1.0f);
    g.restoreState();
}

void ArrangeView::paintAutomationLane (juce::Graphics& g, const ArrangeSnapshot& s, double pps, int track)
{
    const auto r = autoLaneRect (s, track);
    if (r.isEmpty())
        return;

    const auto& t = s.tracks[(size_t) track];
    const auto laneBounds = r.toFloat().reduced (2.0f, 1.0f);
    g.setColour (colours::glass.withAlpha (0.9f));
    g.fillRoundedRectangle (laneBounds, 3.0f);
    g.setColour (colours::panelEdge.withAlpha (0.8f));
    g.drawRoundedRectangle (laneBounds.reduced (0.5f), 3.0f, 1.0f);

    // Reference guide line (0 dB for volume, centre for pan/plugin)
    const float refVal = t.activeAutoTarget == "volume" ? dbToAutoNorm (0.0f) : 0.5f;
    const float refY = valueToAutoY (refVal, r);
    g.setColour (colours::dim.withAlpha (0.22f));
    g.drawHorizontalLine (juce::roundToInt (refY), laneBounds.getX() + 2.0f, laneBounds.getRight() - 2.0f);

    const AutomationLane* activeLane = nullptr;
    for (const auto& lane : t.automation)
    {
        if (lane.targetId == t.activeAutoTarget)
        {
            activeLane = &lane;
            break;
        }
    }

    juce::Colour curveColour = colours::amber;
    if (t.activeAutoTarget == "pan")
        curveColour = colours::accent;
    else if (t.activeAutoTarget.startsWith ("plugin:"))
        curveColour = colours::good;
    if (activeLane != nullptr && ! activeLane->enabled)
        curveColour = curveColour.withAlpha (0.35f);

    if (activeLane == nullptr || activeLane->points.empty())
    {
        g.setFont (ui::labelFont (9.0f));
        g.setColour (colours::dim.withAlpha (0.55f));
        g.drawText ("Click to add automation point", r.reduced (8, 2), juce::Justification::centredLeft, false);
        return;
    }

    g.saveState();
    g.reduceClipRegion (r);

    const float xLeft = (float) r.getX();
    const float xRight = (float) r.getRight();
    const double leftSample = juce::jmax (0.0, xToSample (xLeft, pps));
    const auto& pts = activeLane->points;

    juce::Path path;
    const float startY = valueToAutoY (activeLane->evaluateAt ((juce::int64) std::llround (leftSample)), r);
    path.startNewSubPath (xLeft, startY);

    const float firstX = (float) sampleToX ((double) pts.front().sample, pps);
    const float firstY = valueToAutoY (pts.front().value, r);
    if (firstX > xLeft)
        path.lineTo (firstX, firstY);
    else
        path.startNewSubPath (firstX, firstY);

    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const auto& p0 = pts[i];
        const auto& p1 = pts[i + 1];
        const float x0 = (float) sampleToX ((double) p0.sample, pps);
        const float y0 = valueToAutoY (p0.value, r);
        const float x1 = (float) sampleToX ((double) p1.sample, pps);
        const float y1 = valueToAutoY (p1.value, r);

        if (p0.curve == AutoCurveType::Step)
        {
            path.lineTo (x1, y0);
            path.lineTo (x1, y1);
        }
        else if (p0.curve == AutoCurveType::Smooth && x1 > x0 + 2.0f)
        {
            const int steps = juce::jlimit (4, 32, (int) ((x1 - x0) / 4.0f));
            for (int k = 1; k <= steps; ++k)
            {
                const float frac = (float) k / (float) steps;
                const float mu = frac * frac * (3.0f - 2.0f * frac);
                const float xx = x0 + (x1 - x0) * frac;
                const float yy = y0 + (y1 - y0) * mu;
                path.lineTo (xx, yy);
            }
        }
        else
        {
            path.lineTo (x1, y1);
        }
    }

    const float lastX = (float) sampleToX ((double) pts.back().sample, pps);
    const float lastY = valueToAutoY (pts.back().value, r);
    if (xRight > lastX)
        path.lineTo (xRight, lastY);

    juce::Path fillPath (path);
    fillPath.lineTo (xRight, (float) r.getBottom() - 2.0f);
    fillPath.lineTo (xLeft, (float) r.getBottom() - 2.0f);
    fillPath.closeSubPath();
    g.setColour (curveColour.withAlpha (0.12f));
    g.fillPath (fillPath);

    g.setColour (curveColour);
    g.strokePath (path, juce::PathStrokeType (1.75f));

    for (int i = 0; i < (int) pts.size(); ++i)
    {
        const float px = (float) sampleToX ((double) pts[(size_t) i].sample, pps);
        if (px < xLeft - 8.0f || px > xRight + 8.0f) continue;
        const float py = valueToAutoY (pts[(size_t) i].value, r);
        const bool isDragged = (mode == Mode::autoPoint && dragAutoTrack == track && dragAutoPoint == i);
        const float rad = isDragged ? 5.0f : 3.75f;
        g.setColour (colours::glass);
        g.fillEllipse (px - rad - 1.0f, py - rad - 1.0f, (rad + 1.0f) * 2.0f, (rad + 1.0f) * 2.0f);
        g.setColour (isDragged ? colours::text : curveColour);
        g.fillEllipse (px - rad, py - rad, rad * 2.0f, rad * 2.0f);
    }

    g.restoreState();
}

void ArrangeView::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    const auto sp = snapshotPtr();
    const auto& s = *sp;
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
        const int y = trackTop (s, i);
        const int h = trackHeight (s, i);
        if (y + h < top || y > height) continue;
        const auto& tr = s.tracks[(size_t) i];
        const auto lane = juce::Rectangle<float> ((float) kHeaderWidth, (float) (y + 1), (float) (width - kHeaderWidth), (float) (kLaneHeight - 2)).reduced (2.0f, 0.0f);
        if (tr.kind == TrackKind::Bus)
        {
            ui::hw::drawGlass (g, lane, 3.0f, colours::amber);
            g.setColour (colours::accent.withAlpha (0.04f));
            g.fillRoundedRectangle (lane, 3.0f);

            juce::StringArray sources;
            for (int j = 0; j < count; ++j)
            {
                if (j == i) continue;
                const auto& src = s.tracks[(size_t) j];
                bool feeds = (src.outputTrack == i);
                if (! feeds)
                {
                    for (const auto& snd : src.sends)
                        if (snd.targetTrack == i) { feeds = true; break; }
                }
                if (feeds)
                    sources.add (src.name);
            }
            juce::String label = sources.isEmpty() ? juce::String ("Bus (route tracks or sends here)")
                                                   : ("Bus <- " + sources.joinIntoString (", "));
            if (juce::isPositiveAndBelow (tr.outputTrack, count))
                label += "  -> " + s.tracks[(size_t) tr.outputTrack].name;
            g.setFont (ui::labelFont (10.0f));
            g.setColour (colours::dim.withAlpha (0.75f));
            g.drawText (label, lane.reduced (10.0f, 4.0f), juce::Justification::centredLeft, true);
        }
        else
        {
            ui::hw::drawGlass (g, lane, 3.0f, colours::accent);
            juce::StringArray parts;
            if (juce::isPositiveAndBelow (tr.outputTrack, count))
                parts.add ("-> " + s.tracks[(size_t) tr.outputTrack].name);
            if (! tr.sends.empty())
            {
                juce::StringArray sendNames;
                for (const auto& snd : tr.sends)
                    if (juce::isPositiveAndBelow (snd.targetTrack, count))
                        sendNames.add (s.tracks[(size_t) snd.targetTrack].name);
                if (! sendNames.isEmpty())
                    parts.add ("Send: " + sendNames.joinIntoString (", "));
            }
            if (! parts.isEmpty())
            {
                g.setFont (ui::labelFont (9.5f));
                g.setColour (colours::dim.withAlpha (0.7f));
                g.drawText (parts.joinIntoString ("  |  "), lane.reduced (8.0f, 3.0f), juce::Justification::topRight, true);
            }
        }
    }

    // empty state prompt when there are no tracks
    if (count == 0)
    {
        const auto body = juce::Rectangle<int> (kHeaderWidth, top, juce::jmax (0, width - kHeaderWidth), juce::jmax (0, height - top - kScrollStripHeight)).reduced (24, 12);
        g.setFont (ui::font (13.0f));
        g.setColour (colours::dim);
        g.drawFittedText (juce::String::fromUTF8 ("Drop audio files or a .olympus project here \xe2\x80\x94 or click + Track / Import Audio"),
                          body, juce::Justification::centred, 2);
    }

    // file-drag drop highlight
    if (draggingFiles)
    {
        if (dragHoverTrack >= 0 && dragHoverTrack < count)
        {
            const int y = trackTop (s, dragHoverTrack);
            const auto lane = juce::Rectangle<float> ((float) kHeaderWidth, (float) (y + 1), (float) (width - kHeaderWidth), (float) (kLaneHeight - 2)).reduced (2.0f, 0.0f);
            g.setColour (colours::amber.withAlpha (0.12f));
            g.fillRoundedRectangle (lane, 3.0f);
            g.setColour (colours::amber.withAlpha (0.8f));
            g.drawRoundedRectangle (lane.reduced (0.5f), 3.0f, 1.5f);
        }
        else
        {
            const auto dropArea = juce::Rectangle<float> ((float) kHeaderWidth, (float) top, (float) juce::jmax (0, width - kHeaderWidth),
                                                          (float) juce::jmax (0, height - top - kScrollStripHeight)).reduced (4.0f);
            g.setColour (colours::amber.withAlpha (0.08f));
            g.fillRoundedRectangle (dropArea, 4.0f);
            g.setColour (colours::amber.withAlpha (0.7f));
            g.drawRoundedRectangle (dropArea.reduced (0.5f), 4.0f, 1.5f);
        }
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

    // clips and automation lanes
    for (int i = 0; i < count; ++i)
    {
        const int y = trackTop (s, i);
        const int h = trackHeight (s, i);
        if (y + h < top || y > height) continue;
        const auto& clips = s.tracks[(size_t) i].clips;
        for (int c = 0; c < (int) clips.size(); ++c)
        {
            const auto rect = clipRect (s, i, clips[(size_t) c].start, clips[(size_t) c].length, pps);
            if (rect.getRight() < kHeaderWidth || rect.getX() > width) continue;
            paintClip (g, s, pps, i, clips[(size_t) c], isSelected (i, c, false));
        }
        const auto& midiClips = s.tracks[(size_t) i].midiClips;
        for (int mc = 0; mc < (int) midiClips.size(); ++mc)
        {
            const auto rect = clipRect (s, i, midiClips[(size_t) mc].start, midiClips[(size_t) mc].length, pps);
            if (rect.getRight() < kHeaderWidth || rect.getX() > width) continue;
            paintMidiClip (g, s, pps, i, midiClips[(size_t) mc], isSelected (i, mc, true));
        }
        if (s.tracks[(size_t) i].showAutomation)
            paintAutomationLane (g, s, pps, i);
    }

    // a take in progress: a red strip across each armed lane, from where the recording began to the playhead
    if (recordingFrom >= 0.0)
    {
        const int x0 = sampleToX (recordingFrom, pps), x1 = sampleToX (juce::jmax (recordingFrom, recordingTo), pps);
        for (int track : recordingTracks)
        {
            if (track < 0 || track >= count) continue;
            const int y = trackTop (s, track);
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

    // marquee selection rectangle
    if (mode == Mode::marquee && ! marqueeRect.isEmpty())
    {
        const auto mr = marqueeRect.toFloat();
        g.setColour (colours::amber.withAlpha (0.14f));
        g.fillRect (mr);
        g.setColour (colours::amber.withAlpha (0.75f));
        g.drawRect (mr, 1.0f);
    }
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

    // slim horizontal overview/scrollbar strip at the bottom of the lane area
    const auto strip = scrollStripBounds();
    if (! strip.isEmpty())
    {
        float thumbX = 0.0f, thumbW = 0.0f;
        double totalSamples = 1.0;
        scrollThumbMetrics (s, pps, thumbX, thumbW, totalSamples);
        g.setColour (colours::panel.darker (0.35f));
        g.fillRect (strip);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRect (strip.getX(), strip.getY(), strip.getWidth(), 1);
        g.setColour (colours::amber.withAlpha (mode == Mode::scrollX ? 0.75f : 0.45f));
        g.fillRoundedRectangle (thumbX, (float) strip.getY() + 1.5f, thumbW, (float) strip.getHeight() - 3.0f, 2.5f);
    }
}

// ---- layout ------------------------------------------------------------------------------------------------------------------------
void ArrangeView::resized()
{
    layoutHeaders();
}

ClipRef ArrangeView::clipAt (juce::Point<int> p) const
{
    if (p.x < kHeaderWidth || scrollStripBounds().contains (p)) return {};
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const int track = trackAt (s, p.y);
    if (track < 0) return {};
    const auto& clips = s.tracks[(size_t) track].clips;
    for (int c = (int) clips.size() - 1; c >= 0; --c)
        if (clipRect (s, track, clips[(size_t) c].start, clips[(size_t) c].length, pps).contains (p))
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
    return clipRect (s, ref.track, c.start, c.length, pxPerSample (s.rate));
}

MidiClipRef ArrangeView::midiClipAt (juce::Point<int> p) const
{
    if (p.x < kHeaderWidth || scrollStripBounds().contains (p)) return {};
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const int track = trackAt (s, p.y);
    if (track < 0) return {};
    const auto& clips = s.tracks[(size_t) track].midiClips;
    for (int c = (int) clips.size() - 1; c >= 0; --c)
        if (clipRect (s, track, clips[(size_t) c].start, clips[(size_t) c].length, pps).contains (p))
            return { track, c };
    return {};
}

juce::Rectangle<int> ArrangeView::midiClipBounds (MidiClipRef ref) const
{
    const auto s = snapshot();
    if (! ref.valid() || ref.track >= (int) s.tracks.size()) return {};
    const auto& clips = s.tracks[(size_t) ref.track].midiClips;
    if (ref.clip >= (int) clips.size()) return {};
    const auto& c = clips[(size_t) ref.clip];
    return clipRect (s, ref.track, c.start, c.length, pxPerSample (s.rate));
}

// ---- mouse -------------------------------------------------------------------------------------------------------------------------
void ArrangeView::mouseMove (const juce::MouseEvent& e)
{
    hoverPos = e.getPosition();
}

void ArrangeView::mouseExit (const juce::MouseEvent&)
{
    hoverPos = { -1, -1 };
}

juce::MouseCursor ArrangeView::getMouseCursor()
{
    if (mode == Mode::trimStart || mode == Mode::trimEnd || mode == Mode::fadeIn || mode == Mode::fadeOut)
        return juce::MouseCursor::LeftRightResizeCursor;
    if (mode == Mode::marker || mode == Mode::autoPoint)
        return juce::MouseCursor::PointingHandCursor;
    if (hoverPos.x < kHeaderWidth || hoverPos.y < 0 || scrollStripBounds().contains (hoverPos))
        return juce::MouseCursor::NormalCursor;

    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    if (hoverPos.y >= kRulerHeight + kLoopHeight && hoverPos.y < laneTop())
    {
        if (markerAt (s, hoverPos, pps) >= 0)
            return juce::MouseCursor::PointingHandCursor;
        return juce::MouseCursor::NormalCursor;
    }
    if (hoverPos.y < laneTop())
        return juce::MouseCursor::NormalCursor;

    const int track = trackAt (s, hoverPos.y);
    if (track < 0)
        return juce::MouseCursor::NormalCursor;
    if (s.tracks[(size_t) track].showAutomation && autoLaneRect (s, track).contains (hoverPos))
    {
        if (hitAutoPoint (s, track, hoverPos, pps) >= 0)
            return juce::MouseCursor::PointingHandCursor;
        return juce::MouseCursor::NormalCursor;
    }
    const auto& clips = s.tracks[(size_t) track].clips;
    for (int c = (int) clips.size() - 1; c >= 0; --c)
    {
        const auto r = clipRect (s, track, clips[(size_t) c].start, clips[(size_t) c].length, pps);
        if (! r.contains (hoverPos)) continue;
        const ClipRef hit { track, c };
        const auto& clip = clips[(size_t) c];
        const bool edgeStart = hoverPos.x - r.getX() <= kEdgeGrab;
        const bool edgeEnd = r.getRight() - hoverPos.x <= kEdgeGrab;
        const bool fadeInHit = (clip.fadeIn > 0 || ! edgeStart) && hitFadeIn (s, hit, hoverPos, pps);
        const bool fadeOutHit = (clip.fadeOut > 0 || ! edgeEnd) && hitFadeOut (s, hit, hoverPos, pps);
        if (fadeInHit || fadeOutHit || edgeStart || edgeEnd)
            return juce::MouseCursor::LeftRightResizeCursor;
        break;
    }
    return juce::MouseCursor::NormalCursor;
}

void ArrangeView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    endGesture(); // a second press (another pointer, or a release a modal loop swallowed) closes the first drag's step instead of nesting inside it
    dragSourceIndex = e.source.getIndex();
    const auto p = e.getPosition();
    hoverPos = p;
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    pressX = p.x;
    pressY = p.y;
    mode = Mode::none;
    dragClips.clear();
    dragMinOrigStart = 0;
    pendingSingleSelect = {};
    didDragMove = false;
    marqueeRect = {};
    marqueeAdditive = false;
    marqueeBaseSelection.clear();

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

    const auto strip = scrollStripBounds();
    if (strip.contains (p))
    {
        if (e.mods.isPopupMenu()) return;
        mode = Mode::scrollX;
        float thumbX = 0.0f, thumbW = 0.0f;
        scrollThumbMetrics (s, pps, thumbX, thumbW, dragTotalSamples);
        const double visibleSpan = (double) juce::jmax (1, strip.getWidth()) / pps;
        const double maxScroll = juce::jmax (0.0, dragTotalSamples - visibleSpan);
        if ((float) p.x < thumbX || (float) p.x > thumbX + thumbW)
        {
            const float sw = (float) juce::jmax (1, strip.getWidth());
            const float avail = juce::jmax (1.0f, sw - thumbW);
            const float targetThumbX = juce::jlimit (0.0f, avail, (float) (p.x - strip.getX()) - thumbW * 0.5f);
            scrollSamples = ((double) targetThumbX / (double) avail) * maxScroll;
        }
        dragScrollStart = scrollSamples;
        repaint();
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

    // automation lanes
    const int hoverTrack = trackAt (s, p.y);
    if (hoverTrack >= 0 && s.tracks[(size_t) hoverTrack].showAutomation)
    {
        const auto autoR = autoLaneRect (s, hoverTrack);
        if (autoR.contains (p))
        {
            const auto& targetId = s.tracks[(size_t) hoverTrack].activeAutoTarget;
            const int hitPt = hitAutoPoint (s, hoverTrack, p, pps);
            if (e.mods.isPopupMenu())
            {
                if (hitPt >= 0)
                    autoPointMenu (hoverTrack, targetId, hitPt);
                return;
            }
            if (hitPt >= 0)
            {
                mode = Mode::autoPoint;
                dragAutoTrack = hoverTrack;
                dragAutoTarget = targetId;
                dragAutoPoint = hitPt;
                beginGesture ("Move automation point");
                return;
            }
            const juce::int64 sample = editor.snap ((juce::int64) juce::jmax (0.0, xToSample (p.x, pps)));
            const float val = autoYToValue (p.y, autoR);
            beginGesture ("Add automation point");
            juce::String error;
            const int added = editor.addAutomationPoint (hoverTrack, targetId, sample, val, AutoCurveType::Linear, error);
            if (added >= 0)
            {
                mode = Mode::autoPoint;
                dragAutoTrack = hoverTrack;
                dragAutoTarget = targetId;
                dragAutoPoint = added;
                changed();
            }
            else
            {
                endGesture();
                report (error);
            }
            return;
        }
    }

    // the lanes
    auto beginGroupMove = [&] (ClipRef primaryRef, juce::int64 primaryStart)
    {
        dragClips.clear();
        dragMinOrigStart = std::numeric_limits<juce::int64>::max();
        for (const auto& r : effectiveSelection())
        {
            if (! r.valid() || ! juce::isPositiveAndBelow (r.track, (int) s.tracks.size())) continue;
            if (r.isMidi)
            {
                if (! juce::isPositiveAndBelow (r.clip, (int) s.tracks[(size_t) r.track].midiClips.size())) continue;
                const auto st = s.tracks[(size_t) r.track].midiClips[(size_t) r.clip].start;
                dragClips.push_back ({ r, st });
                dragMinOrigStart = juce::jmin (dragMinOrigStart, st);
            }
            else
            {
                if (! juce::isPositiveAndBelow (r.clip, (int) s.tracks[(size_t) r.track].clips.size())) continue;
                const auto st = s.tracks[(size_t) r.track].clips[(size_t) r.clip].start;
                dragClips.push_back ({ r, st });
                dragMinOrigStart = juce::jmin (dragMinOrigStart, st);
            }
        }
        if (dragClips.empty())
        {
            dragMinOrigStart = primaryStart;
            dragClips.push_back ({ primaryRef, primaryStart });
        }
        dragClip = primaryRef;
        origStart = primaryStart;
        mode = Mode::move;
        beginGesture ("Move clips");
    };

    const auto midiHit = midiClipAt (p);
    if (midiHit.valid())
    {
        const ClipRef midiRef { midiHit.track, midiHit.clip, true };
        if (e.mods.isPopupMenu())
        {
            if (! isSelected (midiHit.track, midiHit.clip, true))
                selectSingleClip (midiRef);
            repaint();
            midiClipMenu (midiHit);
            return;
        }
        if (e.mods.isShiftDown())
        {
            toggleClipSelection (midiRef);
            repaint();
            return;
        }
        const auto& mc = s.tracks[(size_t) midiHit.track].midiClips[(size_t) midiHit.clip];
        if (isSelected (midiHit.track, midiHit.clip, true) && effectiveSelection().size() > 1)
        {
            pendingSingleSelect = midiRef;
            repaint();
            beginGroupMove (midiRef, mc.start);
            return;
        }
        selectSingleClip (midiRef);
        repaint();
        dragMidiClip = midiHit;
        origStart = mc.start;
        mode = Mode::moveMidi;
        beginGesture ("Move MIDI clip");
        return;
    }

    const auto hit = clipAt (p);
    if (! hit.valid())
    {
        if (! e.mods.isPopupMenu())
        {
            if (e.mods.isAltDown())
            {
                seekTo (editor.snap ((juce::int64) juce::jmax (0.0, xToSample (p.x, pps))));
                return;
            }
            marqueeAdditive = e.mods.isShiftDown();
            if (marqueeAdditive)
                marqueeBaseSelection = effectiveSelection();
            else
                clearSelection();
            mode = Mode::marquee;
            marqueeRect = { p.x, p.y, 0, 0 };
        }
        else
        {
            const int track = trackAt (s, p.y);
            if (track >= 0) laneMenu (track, (juce::int64) juce::jmax (0.0, xToSample (p.x, pps)));
        }
        repaint();
        return;
    }

    const ClipRef hitRef { hit.track, hit.clip, false };
    if (e.mods.isPopupMenu())
    {
        if (! isSelected (hit.track, hit.clip, false))
            selectSingleClip (hitRef);
        repaint();
        clipMenu (hit);
        return;
    }
    if (e.mods.isShiftDown())
    {
        toggleClipSelection (hitRef);
        repaint();
        return;
    }

    const auto& c = s.tracks[(size_t) hit.track].clips[(size_t) hit.clip];
    const auto r = clipBounds (hit);
    const bool edgeStart = p.x - r.getX() <= kEdgeGrab;
    const bool edgeEnd = r.getRight() - p.x <= kEdgeGrab;
    const bool fadeInHit = c.fadeIn > 0 && hitFadeIn (s, hit, p, pps);
    const bool fadeOutHit = c.fadeOut > 0 && hitFadeOut (s, hit, p, pps);
    const bool edgeOrFade = fadeInHit || fadeOutHit || edgeStart || edgeEnd || hitFadeIn (s, hit, p, pps) || hitFadeOut (s, hit, p, pps);

    if (! edgeOrFade && isSelected (hit.track, hit.clip, false) && effectiveSelection().size() > 1)
    {
        pendingSingleSelect = hitRef;
        repaint();
        beginGroupMove (hitRef, c.start);
        return;
    }

    selectSingleClip (hitRef);
    repaint();

    dragClip = hit;
    origStart = c.start;
    origEnd = c.end();
    origFadeIn = c.fadeIn;
    origFadeOut = c.fadeOut;

    if (fadeInHit) mode = Mode::fadeIn;
    else if (fadeOutHit) mode = Mode::fadeOut;
    else if (edgeStart) mode = Mode::trimStart;
    else if (edgeEnd) mode = Mode::trimEnd;
    else if (hitFadeIn (s, hit, p, pps)) mode = Mode::fadeIn;
    else if (hitFadeOut (s, hit, p, pps)) mode = Mode::fadeOut;
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

void ArrangeView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    const auto midiHit = midiClipAt (e.getPosition());
    if (! midiHit.valid()) return;
    selectSingleClip ({ midiHit.track, midiHit.clip, true });
    repaint();
    if (onOpenMidiClip) onOpenMidiClip (midiHit);
}

void ArrangeView::mouseDrag (const juce::MouseEvent& e)
{
    if (e.source.getIndex() != dragSourceIndex) return; // only the pointer that began the drag moves it
    if (mode == Mode::none) return;
    const auto p = e.getPosition();
    hoverPos = p;
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const double deltaSamples = (double) (p.x - pressX) / pps;
    juce::String error;

    switch (mode)
    {
        case Mode::ruler:
            seekTo ((juce::int64) xToSample (p.x, pps));
            return;

        case Mode::scrollX:
        {
            const auto strip = scrollStripBounds();
            if (strip.isEmpty()) return;
            const double visibleSpan = (double) juce::jmax (1, strip.getWidth()) / pps;
            const double maxScroll = juce::jmax (0.0, dragTotalSamples - visibleSpan);
            const float sw = (float) juce::jmax (1, strip.getWidth());
            const float thumbW = juce::jlimit (1.0f, sw, juce::jmax (20.0f, (float) (visibleSpan / juce::jmax (1.0, dragTotalSamples) * sw)));
            const float avail = juce::jmax (1.0f, sw - thumbW);
            const double delta = ((double) (p.x - pressX) / (double) avail) * maxScroll;
            scrollSamples = juce::jlimit (0.0, maxScroll, dragScrollStart + delta);
            repaint();
            return;
        }

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

        case Mode::marquee:
        {
            marqueeRect = juce::Rectangle<int>::leftTopRightBottom (std::min (pressX, p.x), std::min (pressY, p.y),
                                                                    std::max (pressX, p.x), std::max (pressY, p.y));
            selectedClips = marqueeBaseSelection;
            if (p.x != pressX || p.y != pressY)
            {
                const juce::Rectangle<int> hitRect (marqueeRect.getX(), marqueeRect.getY(),
                                                    juce::jmax (1, marqueeRect.getWidth()),
                                                    juce::jmax (1, marqueeRect.getHeight()));
                for (int t = 0; t < (int) s.tracks.size(); ++t)
                {
                    const auto& clips = s.tracks[(size_t) t].clips;
                    for (int c = 0; c < (int) clips.size(); ++c)
                    {
                        if (clipRect (s, t, clips[(size_t) c].start, clips[(size_t) c].length, pps).intersects (hitRect))
                        {
                            const ClipRef r { t, c, false };
                            if (std::find (selectedClips.begin(), selectedClips.end(), r) == selectedClips.end())
                                selectedClips.push_back (r);
                        }
                    }
                    const auto& midiClips = s.tracks[(size_t) t].midiClips;
                    for (int mc = 0; mc < (int) midiClips.size(); ++mc)
                    {
                        if (clipRect (s, t, midiClips[(size_t) mc].start, midiClips[(size_t) mc].length, pps).intersects (hitRect))
                        {
                            const ClipRef r { t, mc, true };
                            if (std::find (selectedClips.begin(), selectedClips.end(), r) == selectedClips.end())
                                selectedClips.push_back (r);
                        }
                    }
                }
            }
            syncPrimaryFromSelection();
            repaint();
            return;
        }

        case Mode::move:
        {
            if (dragClips.size() > 1)
            {
                const auto snappedPrimary = editor.snap ((juce::int64) juce::jmax (0.0, (double) origStart + (double) std::llround (deltaSamples)));
                juce::int64 delta = snappedPrimary - origStart;
                if (dragMinOrigStart + delta < 0)
                    delta = -dragMinOrigStart;
                if (std::abs (p.x - pressX) > 2 || std::abs (p.y - pressY) > 2 || delta != 0)
                    didDragMove = true;
                for (const auto& item : dragClips)
                {
                    const juce::int64 target = juce::jmax ((juce::int64) 0, item.origStart + delta);
                    if (item.ref.isMidi)
                    {
                        if (! editor.moveMidiClip (item.ref.track, item.ref.clip, target, error))
                            report (error);
                    }
                    else
                    {
                        if (! editor.moveClip (item.ref.track, item.ref.clip, target, error))
                            report (error);
                    }
                }
                break;
            }
            const int lane = trackAt (s, p.y);
            const auto newStart = editor.snap ((juce::int64) juce::jmax (0.0, (double) origStart + std::llround (deltaSamples)));
            if (lane >= 0 && lane != dragClip.track)
            {
                const auto moved = editor.moveClipToTrack (dragClip.track, dragClip.clip, lane, newStart, error);
                if (moved.valid()) { dragClip = moved; selectSingleClip (moved); }
                else report (error);
            }
            else if (! editor.moveClip (dragClip.track, dragClip.clip, newStart, error))
                report (error);
            break;
        }

        case Mode::moveMidi:
        {
            const auto newStart = editor.snap ((juce::int64) juce::jmax (0.0, (double) origStart + (double) std::llround (deltaSamples)));
            if (! editor.moveMidiClip (dragMidiClip.track, dragMidiClip.clip, newStart, error))
                report (error);
            break;
        }

        case Mode::trimStart:
        {
            const auto targetStart = editor.snap ((juce::int64) juce::jmax (0.0, (double) origStart + std::llround (deltaSamples)));
            if (! editor.trimStart (dragClip.track, dragClip.clip, targetStart, error))
                report (error);
            else if (origFadeIn > 0 || origFadeOut > 0)
                editor.setClipFades (dragClip.track, dragClip.clip, origFadeIn, origFadeOut, error);
            break;
        }

        case Mode::trimEnd:
        {
            const auto targetEnd = editor.snap ((juce::int64) juce::jmax (0.0, (double) origEnd + std::llround (deltaSamples)));
            if (! editor.trimEnd (dragClip.track, dragClip.clip, targetEnd, error))
                report (error);
            else if (origFadeIn > 0 || origFadeOut > 0)
                editor.setClipFades (dragClip.track, dragClip.clip, origFadeIn, origFadeOut, error);
            break;
        }

        case Mode::fadeIn:
            if (! editor.setClipFades (dragClip.track, dragClip.clip, origFadeIn + std::llround (deltaSamples), origFadeOut, error))
                report (error);
            break;

        case Mode::fadeOut:
            if (! editor.setClipFades (dragClip.track, dragClip.clip, origFadeIn, origFadeOut - std::llround (deltaSamples), error))
                report (error);
            break;

        case Mode::autoPoint:
        {
            if (! juce::isPositiveAndBelow (dragAutoTrack, (int) s.tracks.size())) break;
            const auto autoR = autoLaneRect (s, dragAutoTrack);
            if (autoR.isEmpty()) break;
            const juce::int64 targetSample = editor.snap ((juce::int64) juce::jmax (0.0, xToSample (p.x, pps)));
            const float targetVal = autoYToValue (p.y, autoR);
            int newIndex = dragAutoPoint;
            if (! editor.moveAutomationPoint (dragAutoTrack, dragAutoTarget, dragAutoPoint, targetSample, targetVal, error, &newIndex))
            {
                report (error);
                break;
            }
            dragAutoPoint = newIndex;
            break;
        }

        case Mode::none:
            break;
    }
    changed();
}

void ArrangeView::mouseUp (const juce::MouseEvent& e)
{
    if (e.source.getIndex() != dragSourceIndex) return; // another pointer's release does not end this drag
    dragSourceIndex = -1;
    if (mode == Mode::marquee)
    {
        mode = Mode::none;
        marqueeRect = {};
        marqueeAdditive = false;
        marqueeBaseSelection.clear();
        repaint();
        return;
    }
    if (mode == Mode::move && pendingSingleSelect.valid() && ! didDragMove)
    {
        selectSingleClip (pendingSingleSelect);
        pendingSingleSelect = {};
        dragClips.clear();
        didDragMove = false;
        mode = Mode::none;
        endGesture();
        repaint();
        return;
    }
    pendingSingleSelect = {};
    dragClips.clear();
    didDragMove = false;
    const bool wasEdit = mode != Mode::none && mode != Mode::ruler && mode != Mode::scrollX;
    mode = Mode::none;
    endGesture();
    if (wasEdit) edited();
    else repaint();
}

void ArrangeView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto s = snapshot();
    const int timelineWidth = juce::jmax (1, getWidth() - kHeaderWidth);
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double x = (double) juce::jmax (kHeaderWidth, e.x);
        const double pps = pxPerSample (s.rate);
        const double anchorSample = xToSample (x, pps);
        zoom = juce::jlimit (kMinZoom, kMaxZoom, zoom * std::pow (2.0, wheel.deltaY * 4.0));
        const double newPps = pxPerSample (s.rate);
        scrollSamples = juce::jlimit (0.0, juce::jmax (0.0, (double) s.length - (double) timelineWidth / newPps),
                                      juce::jmax (0.0, anchorSample - (x - kHeaderWidth) / newPps));
    }
    else if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
    {
        const double pps = pxPerSample (s.rate);
        const bool useX = ! e.mods.isShiftDown() && std::abs (wheel.deltaX) > std::abs (wheel.deltaY);
        const double delta = useX ? wheel.deltaX : (std::abs (wheel.deltaY) > 0.0f ? wheel.deltaY : wheel.deltaX);
        scrollSamples = juce::jlimit (0.0, juce::jmax (0.0, (double) s.length - (double) timelineWidth / pps),
                                      juce::jmax (0.0, scrollSamples - delta * 0.25 * timelineWidth / pps));
    }
    else
    {
        scrollY = juce::jlimit (0, maxScrollY (s), scrollY - juce::roundToInt (wheel.deltaY * 300.0));
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
    if (code == juce::KeyPress::escapeKey) { clearSelection(); return true; }
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

    const auto sel = effectiveSelection();
    if (sel.empty())
    {
        if (dx != 0) seekTo ((juce::int64) juce::jmax (0.0, (double) playhead + dx * stepSamples));
        return;
    }

    if (sel.size() == 1)
    {
        const auto r = sel[0];
        if (! juce::isPositiveAndBelow (r.track, (int) s.tracks.size()))
        {
            clearSelection();
            return;
        }
        if (r.isMidi)
        {
            if (! juce::isPositiveAndBelow (r.clip, (int) s.tracks[(size_t) r.track].midiClips.size()))
            {
                clearSelection();
                return;
            }
            if (dx != 0)
            {
                const auto& mc = s.tracks[(size_t) r.track].midiClips[(size_t) r.clip];
                const auto target = editor.snap ((juce::int64) juce::jmax (0.0, (double) mc.start + dx * stepSamples));
                if (! editor.moveMidiClip (r.track, r.clip, target, error)) report (error);
                edited();
            }
            return;
        }
        if (! juce::isPositiveAndBelow (r.clip, (int) s.tracks[(size_t) r.track].clips.size()))
        {
            clearSelection();
            return;
        }
        const auto& c = s.tracks[(size_t) r.track].clips[(size_t) r.clip];
        if (dx != 0)
        {
            const auto target = editor.snap ((juce::int64) juce::jmax (0.0, (double) c.start + dx * stepSamples));
            if (! editor.moveClip (r.track, r.clip, target, error)) report (error);
        }
        else if (dy != 0)
        {
            const int to = r.track + dy;
            if (juce::isPositiveAndBelow (to, (int) s.tracks.size()))
            {
                const auto moved = editor.moveClipToTrack (r.track, r.clip, to, c.start, error);
                if (moved.valid()) selectSingleClip (moved);
                else report (error);
            }
        }
        edited();
        return;
    }

    if (dx != 0)
    {
        std::vector<DragClipOrigin> items;
        juce::int64 minStart = std::numeric_limits<juce::int64>::max();
        for (const auto& r : sel)
        {
            if (! r.valid() || ! juce::isPositiveAndBelow (r.track, (int) s.tracks.size())) continue;
            if (r.isMidi)
            {
                if (! juce::isPositiveAndBelow (r.clip, (int) s.tracks[(size_t) r.track].midiClips.size())) continue;
                const auto st = s.tracks[(size_t) r.track].midiClips[(size_t) r.clip].start;
                items.push_back ({ r, st });
                minStart = juce::jmin (minStart, st);
            }
            else
            {
                if (! juce::isPositiveAndBelow (r.clip, (int) s.tracks[(size_t) r.track].clips.size())) continue;
                const auto st = s.tracks[(size_t) r.track].clips[(size_t) r.clip].start;
                items.push_back ({ r, st });
                minStart = juce::jmin (minStart, st);
            }
        }
        if (items.empty())
        {
            clearSelection();
            return;
        }
        const auto primaryStart = items.front().origStart;
        const auto target0 = editor.snap ((juce::int64) juce::jmax (0.0, (double) primaryStart + dx * stepSamples));
        juce::int64 delta = target0 - primaryStart;
        if (minStart + delta < 0) delta = -minStart;
        beginGesture ("Move clips");
        for (const auto& item : items)
        {
            const juce::int64 target = juce::jmax ((juce::int64) 0, item.origStart + delta);
            if (item.ref.isMidi)
            {
                if (! editor.moveMidiClip (item.ref.track, item.ref.clip, target, error)) report (error);
            }
            else
            {
                if (! editor.moveClip (item.ref.track, item.ref.clip, target, error)) report (error);
            }
        }
        endGesture();
        edited();
    }
}

void ArrangeView::splitAtPlayhead()
{
    const auto s = snapshot();
    juce::String error;
    const auto at = (juce::int64) playhead;

    const auto inside = [&] (ClipRef r)
    {
        if (r.isMidi || ! r.valid() || ! juce::isPositiveAndBelow (r.track, (int) s.tracks.size())
            || ! juce::isPositiveAndBelow (r.clip, (int) s.tracks[(size_t) r.track].clips.size()))
            return false;
        const auto& c = s.tracks[(size_t) r.track].clips[(size_t) r.clip];
        return at > c.start && at < c.end();
    };

    if (selected.valid() && inside (selected))
    {
        const auto right = editor.splitClip (selected.track, selected.clip, at, error);
        if (right.valid()) selectSingleClip (right);
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
    clearSelection();
    edited();
}

void ArrangeView::deleteSelected()
{
    const auto sel = effectiveSelection();
    if (sel.empty()) return;
    juce::String error;
    if (sel.size() == 1)
    {
        const auto r = sel[0];
        const bool ok = r.isMidi ? editor.deleteMidiClip (r.track, r.clip, error)
                                 : editor.deleteClip (r.track, r.clip, error);
        if (ok) clearSelection();
        else report (error);
        edited();
        return;
    }
    if (editor.deleteClips (sel, error)) clearSelection();
    else report (error);
    edited();
}

void ArrangeView::duplicateSelected()
{
    const auto sel = effectiveSelection();
    if (sel.empty()) return;
    juce::String error;
    if (sel.size() == 1)
    {
        if (sel[0].isMidi) return;
        const auto copy = editor.duplicateClip (sel[0].track, sel[0].clip, error);
        if (copy.valid()) selectSingleClip (copy);
        else report (error);
        edited();
        return;
    }
    std::vector<ClipRef> copies;
    beginGesture ("Duplicate clips");
    for (const auto& r : sel)
    {
        if (r.isMidi) continue;
        const auto copy = editor.duplicateClip (r.track, r.clip, error);
        if (copy.valid()) copies.push_back (copy);
        else { report (error); break; }
    }
    endGesture();
    if (! copies.empty())
    {
        selectedClips = std::move (copies);
        syncPrimaryFromSelection();
    }
    edited();
}

void ArrangeView::copySelected()
{
    const auto sel = effectiveSelection();
    if (sel.empty()) return;
    juce::String error;
    if (sel.size() == 1)
    {
        if (! editor.copyClip (sel[0], error)) report (error);
        return;
    }
    if (! editor.copyClips (sel, error)) report (error);
}

void ArrangeView::cutSelected()
{
    const auto sel = effectiveSelection();
    if (sel.empty()) return;
    juce::String error;
    if (sel.size() == 1)
    {
        if (editor.cutClip (sel[0], error)) clearSelection();
        else report (error);
        edited();
        return;
    }
    if (editor.cutClips (sel, error)) clearSelection();
    else report (error);
    edited();
}

void ArrangeView::pasteAtPlayhead()
{
    if (! editor.hasClipboard()) { report ("The clipboard is empty: copy or cut a clip first."); return; }
    const auto sel = effectiveSelection();
    const int targetTrack = (sel.size() == 1) ? sel[0].track : editor.clipboardTrack();
    pasteAt (targetTrack, (juce::int64) playhead);
}

void ArrangeView::pasteAt (int track, juce::int64 at)
{
    if (recordingFrom >= 0.0) { report ("Stop the recording before you paste."); return; }
    juce::String error;
    const auto made = editor.pasteClips (track, at, error);
    if (made.empty()) { report (error); return; }
    selectedClips = made;
    syncPrimaryFromSelection();
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
    menu.addItem (9, "Fade in (0.5 s)");
    menu.addItem (10, "Fade out (0.5 s)");
    menu.addItem (11, "Clear fades");
    menu.addSeparator();
    menu.addItem (8, "Delete");
    if (dialogFlag()) return;
    dialogFlag() = true; // an open menu is a dialog: the timer's external reload must not replace the session under the captured index
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, ref] (int result)
    {
        if (safe == nullptr) return;
        safe->dialogFlag() = false;
        if (result > 0) safe->clipAction (ref, result);
    });
}

// The empty lane's menu: Paste here (when clipboard holds a clip) and Create MIDI clip.
void ArrangeView::laneMenu (int track, juce::int64 at)
{
    juce::PopupMenu menu;
    if (editor.hasClipboard())
        menu.addItem (1, "Paste here");
    menu.addItem (2, "Create MIDI clip");
    if (dialogFlag()) return;
    dialogFlag() = true;
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, track, at] (int result)
    {
        if (safe == nullptr) return;
        safe->dialogFlag() = false;
        if (result > 0) safe->laneAction (track, at, result);
    });
}

void ArrangeView::laneAction (int track, juce::int64 at, int action)
{
    if (action == 1)
        pasteAt (track, at);
    else if (action == 2)
        createMidiClipAt (track, at);
}

void ArrangeView::createMidiClipAt (int track, juce::int64 at)
{
    const auto s = snapshot();
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size())) return;
    const juce::int64 start = editor.snap (juce::jmax ((juce::int64) 0, at));
    const double startBeats = s.tempo.sampleRate > 0.0 ? s.tempo.samplesToBeats ((double) start) : 0.0;
    const double barBeats = juce::jmax (1.0, s.tempo.beatsPerBar());
    const juce::int64 len = s.tempo.sampleRate > 0.0
        ? juce::jmax ((juce::int64) 1, (juce::int64) std::llround (s.tempo.beatsToSamples (startBeats + barBeats) - (double) start))
        : (juce::int64) (s.rate * 2.0);
    juce::String error;
    const auto made = editor.addMidiClip (track, start, len, {}, error);
    if (! made.valid()) { report (error); return; }
    selectSingleClip ({ made.track, made.clip, true });
    edited();
}

void ArrangeView::midiClipMenu (MidiClipRef ref)
{
    const auto s = snapshot();
    if (! ref.valid() || ref.track >= (int) s.tracks.size()
        || ref.clip >= (int) s.tracks[(size_t) ref.track].midiClips.size()) return;
    juce::PopupMenu menu;
    menu.addItem (1, "Edit in Piano Roll");
    menu.addItem (2, "Quantize");
    menu.addSeparator();
    menu.addItem (3, "Delete");
    if (dialogFlag()) return;
    dialogFlag() = true;
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, ref] (int result)
    {
        if (safe == nullptr) return;
        safe->dialogFlag() = false;
        if (result > 0) safe->midiClipAction (ref, result);
    });
}

void ArrangeView::midiClipAction (MidiClipRef ref, int action)
{
    const auto s = snapshot();
    if (! ref.valid() || ref.track >= (int) s.tracks.size()
        || ref.clip >= (int) s.tracks[(size_t) ref.track].midiClips.size()) return;
    juce::String error;
    switch (action)
    {
        case 1:
            if (onOpenMidiClip) onOpenMidiClip (ref);
            return;
        case 2:
            if (! editor.quantizeMidiClip (ref.track, ref.clip, s.grid, error)) report (error);
            edited();
            return;
        case 3:
            if (editor.deleteMidiClip (ref.track, ref.clip, error))
            {
                if (isSelected (ref.track, ref.clip, true))
                    clearSelection();
            }
            else report (error);
            edited();
            return;
        default:
            return;
    }
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
                selectSingleClip (ref);
                splitAtPlayhead();
            }
            else report ("The playhead is not inside that clip.");
            return;
        case 2:
            if (auto copy = editor.duplicateClip (ref.track, ref.clip, error); copy.valid()) selectSingleClip (copy);
            else report (error);
            break;
        case 3:
            if (! editor.copyClip (ref, error)) report (error);
            return;
        case 4:
            if (editor.cutClip (ref, error)) clearSelection();
            else report (error);
            break;
        case 5:
            if (! editor.setClipMute (ref.track, ref.clip, ! c.mute, error)) report (error);
            break;
        case 6:
        {
            const auto clipStart = c.start, clipLength = c.length, clipOffset = c.offset;
            const auto clipName = c.name;
            juce::Component::SafePointer<ArrangeView> safe (this);
            askText ("Clip gain", "Gain in dB, from -100 to +24", juce::String (c.gainDb, 1),
                     [safe, ref, clipStart, clipLength, clipOffset, clipName] (const juce::String& text)
                     {
                         if (safe == nullptr) return;
                         if (! text.containsAnyOf ("0123456789")) { safe->report ("Type a number of dB."); return; }
                         const auto now = safe->snapshot();
                         if (! ref.valid() || ! juce::isPositiveAndBelow (ref.track, (int) now.tracks.size())
                             || ! juce::isPositiveAndBelow (ref.clip, (int) now.tracks[(size_t) ref.track].clips.size()))
                             return;
                         const auto& cur = now.tracks[(size_t) ref.track].clips[(size_t) ref.clip];
                         if (cur.start != clipStart || cur.length != clipLength || cur.offset != clipOffset || cur.name != clipName)
                             return;
                         juce::String e;
                         if (! safe->editor.setClipGain (ref.track, ref.clip, text.getFloatValue(), e)) safe->report (e);
                         safe->edited();
                     });
            return;
        }
        case 7:
        {
            const auto clipStart = c.start, clipLength = c.length, clipOffset = c.offset;
            const auto clipName = c.name;
            juce::Component::SafePointer<ArrangeView> safe (this);
            askText ("Rename clip", "The new name", c.name,
                     [safe, ref, clipStart, clipLength, clipOffset, clipName] (const juce::String& text)
                     {
                         if (safe == nullptr) return;
                         const auto now = safe->snapshot();
                         if (! ref.valid() || ! juce::isPositiveAndBelow (ref.track, (int) now.tracks.size())
                             || ! juce::isPositiveAndBelow (ref.clip, (int) now.tracks[(size_t) ref.track].clips.size()))
                             return;
                         const auto& cur = now.tracks[(size_t) ref.track].clips[(size_t) ref.clip];
                         if (cur.start != clipStart || cur.length != clipLength || cur.offset != clipOffset || cur.name != clipName)
                             return;
                         juce::String e;
                         if (! safe->editor.renameClip (ref.track, ref.clip, text, e)) safe->report (e);
                         safe->edited();
                     });
            return;
        }
        case 8:
            if (editor.deleteClip (ref.track, ref.clip, error)) clearSelection();
            else report (error);
            break;
        case 9:
            if (! editor.setClipFades (ref.track, ref.clip, (juce::int64) std::llround (0.5 * s.rate), c.fadeOut, error)) report (error);
            break;
        case 10:
            if (! editor.setClipFades (ref.track, ref.clip, c.fadeIn, (juce::int64) std::llround (0.5 * s.rate), error)) report (error);
            break;
        case 11:
            if (! editor.setClipFades (ref.track, ref.clip, 0, 0, error)) report (error);
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
    if (dialogFlag()) return;
    dialogFlag() = true;
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, index] (int result)
    {
        if (safe == nullptr) return;
        safe->dialogFlag() = false;
        if (result > 0) safe->markerAction (index, result);
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
    const auto markerStart = s.markers[(size_t) index].start;
    const auto markerName = s.markers[(size_t) index].name;
    juce::Component::SafePointer<ArrangeView> safe (this);
    askText ("Rename marker", "The new name", markerName, [safe, index, markerStart, markerName] (const juce::String& text)
    {
        if (safe == nullptr) return;
        const auto now = safe->snapshot();
        if (! juce::isPositiveAndBelow (index, (int) now.markers.size())
            || now.markers[(size_t) index].start != markerStart || now.markers[(size_t) index].name != markerName)
            return;
        juce::String e;
        if (! safe->editor.renameMarker (index, text, e)) safe->report (e);
        safe->edited();
    });
}

// A small text box in a modal window; the answer goes to `onOk` when it is OK.
void ArrangeView::askText (const juce::String& title, const juce::String& prompt, const juce::String& initial, std::function<void (const juce::String&)> onOk)
{
    if (dialogFlag()) return;
    dialogFlag() = true;
    juce::Component::SafePointer<ArrangeView> safe (this);
    pendingDialogEnd = [safe, okCb = std::move (onOk)] (bool accept, const juce::String& text)
    {
        if (safe == nullptr) return;
        safe->pendingDialogEnd = nullptr;
        safe->activeDialog = nullptr;
        safe->dialogFlag() = false;
        if (accept && okCb) okCb (text);
    };
    if (headlessDialogs) return;   // checks only: no native window; finishOpenDialogForTest ends the dialog
    auto* box = new juce::AlertWindow (title, prompt, juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("text", initial, {});
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    activeDialog = box;
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, box] (int result)
    {
        if (safe == nullptr || ! safe->pendingDialogEnd) return;
        const auto text = result == 1 ? box->getTextEditorContents ("text") : juce::String();
        auto done = std::move (safe->pendingDialogEnd);
        done (result == 1, text);
    }), true);
}

void ArrangeView::finishOpenDialogForTest (bool accept, const juce::String& text)
{
    if (! pendingDialogEnd) return;
    auto done = std::move (pendingDialogEnd);
    if (activeDialog != nullptr)
    {
        auto* box = activeDialog.getComponent();
        activeDialog = nullptr;
        box->exitModalState (0);
        delete box;
    }
    done (accept, text);
}

void ArrangeView::autoTargetMenu (int track)
{
    const auto s = snapshot();
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size())) return;
    const auto& t = s.tracks[(size_t) track];

    juce::PopupMenu menu;
    std::vector<juce::String> targets;
    auto addItem = [&] (const juce::String& targetId, const juce::String& label)
    {
        targets.push_back (targetId);
        menu.addItem ((int) targets.size(), label, true, t.activeAutoTarget == targetId);
    };
    addItem ("volume", "Volume");
    addItem ("pan", "Pan");
    if (! t.pluginParams.empty())
    {
        menu.addSeparator();
        for (const auto& pp : t.pluginParams)
            addItem ("plugin:" + juce::String (pp.slot) + ":" + juce::String (pp.param), pp.name);
    }

    if (dialogFlag()) return;
    dialogFlag() = true;
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, track, menuTargets = std::move (targets)] (int result)
    {
        if (safe == nullptr) return;
        safe->dialogFlag() = false;
        if (result > 0 && result <= (int) menuTargets.size())
        {
            juce::String error;
            if (! safe->editor.setTrackActiveAutoTarget (track, menuTargets[(size_t) (result - 1)], error))
                safe->report (error);
            safe->edited();
        }
    });
}

void ArrangeView::autoPointMenu (int track, const juce::String& targetId, int pointIndex)
{
    const auto s = snapshot();
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size())) return;
    AutoCurveType cur = AutoCurveType::Linear;
    for (const auto& lane : s.tracks[(size_t) track].automation)
    {
        if (lane.targetId == targetId)
        {
            if (! juce::isPositiveAndBelow (pointIndex, (int) lane.points.size())) return;
            cur = lane.points[(size_t) pointIndex].curve;
            break;
        }
    }

    juce::PopupMenu menu;
    menu.addItem (1, "Curve: Linear", true, cur == AutoCurveType::Linear);
    menu.addItem (2, "Curve: Step", true, cur == AutoCurveType::Step);
    menu.addItem (3, "Curve: Smooth", true, cur == AutoCurveType::Smooth);
    menu.addSeparator();
    menu.addItem (4, "Delete point");

    if (dialogFlag()) return;
    dialogFlag() = true;
    juce::Component::SafePointer<ArrangeView> safe (this);
    menu.showMenuAsync ({}, [safe, track, targetId, pointIndex] (int result)
    {
        if (safe == nullptr) return;
        safe->dialogFlag() = false;
        if (result > 0) safe->autoPointAction (track, targetId, pointIndex, result);
    });
}

void ArrangeView::autoPointAction (int track, const juce::String& targetId, int pointIndex, int action)
{
    juce::String error;
    switch (action)
    {
        case 1: editor.setAutomationPointCurve (track, targetId, pointIndex, AutoCurveType::Linear, error); break;
        case 2: editor.setAutomationPointCurve (track, targetId, pointIndex, AutoCurveType::Step, error); break;
        case 3: editor.setAutomationPointCurve (track, targetId, pointIndex, AutoCurveType::Smooth, error); break;
        case 4: editor.deleteAutomationPoint (track, targetId, pointIndex, error); break;
        default: return;
    }
    report (error);
    edited();
}

// ---- files ---------------------------------------------------------------------------------------------------------------------------
bool ArrangeView::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (files.size() == 1 && juce::File (files[0]).hasFileExtension ("olympus"))
        return (bool) onProjectDropped;
    for (auto& f : files)
        if (isAudioFile (juce::File (f))) return true;
    return false;
}

void ArrangeView::updateDropHover (const juce::StringArray& files, int x, int y)
{
    const bool isProject = files.size() == 1 && juce::File (files[0]).hasFileExtension ("olympus");
    const int newTrack = (isProject || y < laneTop() || x < kHeaderWidth) ? -1 : trackAt (snapshot(), y);
    if (! draggingFiles || dragHoverTrack != newTrack)
    {
        draggingFiles = true;
        dragHoverTrack = newTrack;
        repaint();
    }
}

void ArrangeView::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    updateDropHover (files, x, y);
}

void ArrangeView::fileDragMove (const juce::StringArray& files, int x, int y)
{
    updateDropHover (files, x, y);
}

void ArrangeView::fileDragExit (const juce::StringArray&)
{
    if (draggingFiles || dragHoverTrack != -1)
    {
        draggingFiles = false;
        dragHoverTrack = -1;
        repaint();
    }
}

void ArrangeView::filesDropped (const juce::StringArray& files, int x, int y)
{
    draggingFiles = false;
    dragHoverTrack = -1;
    repaint();
    if (files.size() == 1 && juce::File (files[0]).hasFileExtension ("olympus"))
    {
        if (onProjectDropped) onProjectDropped (juce::File (files[0]));
        return;
    }
    if (y < laneTop() || ! onFilesDropped) return;
    const auto s = snapshot();
    const double pps = pxPerSample (s.rate);
    const int track = trackAt (s, y);
    const auto at = x < kHeaderWidth ? juce::int64 (0) : editor.snap ((juce::int64) juce::jmax (0.0, xToSample (x, pps)));
    juce::StringArray audio;
    for (auto& f : files)
        if (isAudioFile (juce::File (f))) audio.add (f);
    if (! audio.isEmpty()) onFilesDropped (audio, track, at);
}
} // namespace olympus::gui
