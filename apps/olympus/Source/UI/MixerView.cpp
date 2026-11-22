// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "MixerView.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace olympus::gui
{
namespace
{
constexpr int kSlotHeight = 18;
constexpr int kSlotGap = 2;
constexpr int kAddHeight = 18;
constexpr double kPeakHoldMs = 2000.0;

juce::String formatDb (double db)
{
    if (db <= -99.95)
        return "-inf";
    return (db >= 0.0 ? "+" : "") + juce::String (db, 1) + " dB";
}

juce::String formatPan (double v)
{
    if (std::abs (v) < 0.005)
        return "C";
    return (v < 0.0 ? "L" : "R") + juce::String (juce::roundToInt (std::abs (v) * 100.0));
}

juce::String faderTooltip (const juce::String& prefix, double db)
{
    const juce::String value = db <= -100.0 ? juce::String ("-inf") : (db >= 0.0 ? "+" : "") + juce::String (db, 1);
    return prefix + " " + value + " dB (drag, double-click for 0 dB)";
}

juce::String slotWetText (float wet)
{
    if (wet >= 0.999f)
        return {};
    return " (" + juce::String (juce::roundToInt (wet * 100.0f)) + "% wet)";
}

struct SlotInfo
{
    juce::String name;
    bool bypass = false;
    bool missing = false;
    float wet = 1.0f;
};

// Vertical hardware meter bar (-60 to 0 dBFS): green, amber above -12 dB, red above -3 dB, with held peak marker.
void paintVerticalLevelBar (juce::Graphics& g, const juce::Rectangle<float>& bar, const MeterBallistics& meter)
{
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.fillRoundedRectangle (bar.expanded (1.0f, 0.0f), 2.0f);
    g.setColour (colours::dim.withAlpha (0.45f));
    for (int i = 0; i <= 4; ++i)
    {
        const float y = bar.getBottom() - bar.getHeight() * (float) i * 0.25f;
        g.drawHorizontalLine (juce::roundToInt (y), bar.getRight() + 1.0f, bar.getRight() + 3.0f);
    }

    const auto yFor = [&bar] (float db)
    {
        return bar.getBottom() - bar.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    };
    const juce::Colour colour = meter.level > -3.0f ? colours::bad : meter.level > -12.0f ? colours::warn : colours::good;
    const float yTop = yFor (meter.level);
    if (yTop < bar.getBottom())
    {
        g.setColour (colour);
        g.fillRect (bar.getX(), yTop, bar.getWidth(), bar.getBottom() - yTop);
    }
    if (meter.hold > -60.0f)
    {
        const float yHold = yFor (meter.hold);
        const auto cap = juce::Rectangle<float> (bar.getX() - 1.0f, yHold - 1.5f, bar.getWidth() + 2.0f, 3.0f);
        g.setColour (colour.withAlpha (0.28f));
        g.fillRoundedRectangle (cap.expanded (1.0f, 1.5f), 2.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (cap, 1.0f);
    }
}

class MixerStripLamp : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    juce::Colour lamp = colours::amber;

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        ui::hw::drawPushButton (g, getLocalBounds().toFloat(), getToggleState(), lamp, down, highlighted, isEnabled());
        g.setFont (ui::labelFont (juce::jmin (9.5f, (float) getHeight() * 0.5f)));
        g.setColour (getToggleState() && lamp.getPerceivedBrightness() > 0.5f ? colours::lampInk : colours::text);
        g.drawText (getButtonText().toUpperCase(), getLocalBounds().translated (0, down ? 1 : 0), juce::Justification::centred);
    }
};

class MixerSlotButton : public LampButton
{
public:
    std::function<void()> onShiftClick;
    std::function<void()> onMenu;
    bool missing = false;
    bool bypassed = false;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onMenu) onMenu();
            return;
        }
        if (e.mods.isShiftDown())
        {
            shiftDown = true;
            return;
        }
        LampButton::mouseDown (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (shiftDown)
        {
            shiftDown = false;
            if (contains (e.getPosition()) && onShiftClick)
                onShiftClick();
            return;
        }
        if (! e.mods.isPopupMenu())
            LampButton::mouseUp (e);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        ui::hw::drawPushButton (g, getLocalBounds().toFloat(), getToggleState(), lamp, down, highlighted, isEnabled());
        g.setFont (ui::labelFont (9.0f));
        const auto ink = missing ? colours::bad
                       : bypassed ? colours::dim
                       : (getToggleState() && lamp.getPerceivedBrightness() > 0.5f ? colours::lampInk : colours::text);
        g.setColour (ink);
        g.drawFittedText (getButtonText().toUpperCase(), getLocalBounds().reduced (4, 0).translated (0, down ? 1 : 0),
                          juce::Justification::centred, 1);
    }

private:
    bool shiftDown = false;
};

class MixerClipLamp : public juce::Button
{
public:
    MixerClipLamp() : juce::Button ("Clip") {}
    bool lit = false;

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        ui::hw::drawLamp (g, { 7.0f, (float) getHeight() * 0.5f }, 3.8f, colours::bad, lit);
        g.setFont (ui::labelFont (9.5f));
        g.setColour (lit ? colours::bad : colours::dim);
        g.drawText ("CLIP", getLocalBounds().withTrimmedLeft (14), juce::Justification::centredLeft);
    }
};

std::vector<SlotInfo> readSlotInfos (const std::vector<std::unique_ptr<Slot>>& chain)
{
    std::vector<SlotInfo> out;
    out.reserve (chain.size());
    for (const auto& s : chain)
        out.push_back ({ s->plugin, s->bypass, s->instance == nullptr, s->wet });
    return out;
}

template <typename OnShift>
void syncSlotButtons (MixerView& view,
                      int targetTrack,
                      const std::vector<SlotInfo>& infos,
                      juce::OwnedArray<MixerSlotButton>& buttons,
                      juce::Component& container,
                      juce::TextButton& addButton,
                      OnShift&& makeShiftHandler)
{
    bool structureChanged = buttons.size() != (int) infos.size();
    while (buttons.size() > (int) infos.size())
        buttons.removeLast();
    while (buttons.size() < (int) infos.size())
    {
        auto* btn = buttons.add (new MixerSlotButton());
        btn->setClickingTogglesState (false);
        btn->setWantsKeyboardFocus (false);
        btn->lamp = colours::amber;
        container.addAndMakeVisible (btn);
    }
    for (int i = 0; i < (int) infos.size(); ++i)
    {
        auto* btn = buttons[i];
        const auto& info = infos[(size_t) i];
        if (btn->getButtonText() != info.name)
            btn->setButtonText (info.name);
        btn->missing = info.missing;
        btn->bypassed = info.bypass;
        const bool lit = ! info.bypass && ! info.missing;
        if (btn->getToggleState() != lit)
            btn->setToggleState (lit, juce::dontSendNotification);
        const auto tip = info.name + (info.missing ? " (not installed)" : info.bypass ? " (bypassed)" : "")
                         + slotWetText (info.wet)
                         + " — click to open, Shift+click to bypass, right-click for menu";
        if (btn->getTooltip() != tip)
            btn->setTooltip (tip);
        btn->onClick = [&view, targetTrack, i]
        {
            if (view.onOpenPlugin) view.onOpenPlugin (targetTrack, i);
        };
        btn->onShiftClick = makeShiftHandler (i, info.bypass);
        btn->onMenu = [&view, btn, targetTrack, i]
        {
            if (view.onPluginMenu)
                view.onPluginMenu (targetTrack, i, view.getLocalArea (btn, btn->getLocalBounds()));
        };
        btn->repaint();
    }
    if (structureChanged)
    {
        addButton.toFront (false);
    }
}

void layoutInsertsContainer (juce::Viewport& viewport,
                             juce::Component& container,
                             juce::OwnedArray<MixerSlotButton>& buttons,
                             juce::TextButton& addButton)
{
    const int vw = juce::jmax (1, viewport.getWidth());
    const int count = buttons.size();
    const int neededH = count * (kSlotHeight + kSlotGap) + kAddHeight;
    const bool needScroll = neededH > viewport.getHeight();
    const int cw = juce::jmax (1, vw - (needScroll ? viewport.getScrollBarThickness() + 1 : 0));
    container.setSize (cw, juce::jmax (viewport.getHeight(), neededH));
    int y = 0;
    for (int i = 0; i < count; ++i)
    {
        buttons[i]->setBounds (0, y, cw, kSlotHeight);
        y += kSlotHeight + kSlotGap;
    }
    addButton.setBounds (0, y, cw, kAddHeight);
}
} // namespace

// ---- TrackStrip -----------------------------------------------------------------------------------------------------------------
class MixerView::TrackStrip : public juce::Component
{
public:
    TrackStrip (MixerView& owner, int index) : view (owner), trackIndex (index)
    {
        nameLabel.setFont (ui::font (11.5f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, colours::text);
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (nameLabel);

        insertsViewport.setViewedComponent (&insertsContainer, false);
        insertsViewport.setScrollBarsShown (true, false);
        insertsViewport.setScrollBarThickness (5);
        insertsViewport.setWantsKeyboardFocus (false);
        addAndMakeVisible (insertsViewport);

        addButton.setWantsKeyboardFocus (false);
        addButton.setTooltip ("Add plugin to track");
        addButton.onClick = [this]
        {
            if (view.onAddPlugin)
                view.onAddPlugin (trackIndex, view.getLocalArea (&addButton, addButton.getLocalBounds()));
        };
        insertsContainer.addAndMakeVisible (addButton);

        outButton.setWantsKeyboardFocus (false);
        outButton.setTooltip ("Output bus routing (click to change)");
        outButton.onClick = [this] { view.showOutputMenu (trackIndex, outButton); };
        addAndMakeVisible (outButton);

        sendButton.setWantsKeyboardFocus (false);
        sendButton.setTooltip ("Aux sends (click to configure)");
        sendButton.onClick = [this] { view.showSendsMenu (trackIndex, sendButton); };
        addAndMakeVisible (sendButton);

        panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        panSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        panSlider.setLookAndFeel (&view.look);
        panSlider.setRange (-1.0, 1.0, 0.01);
        panSlider.setDoubleClickReturnValue (true, 0.0);
        panSlider.setWantsKeyboardFocus (false);
        panSlider.setTooltip ("Pan: C (drag, double-click for centre)");
        panSlider.onDragStart = [this] { panGesture = std::make_unique<Editor::Gesture> (view.editor, "Track pan"); };
        panSlider.onDragEnd = [this] { panGesture.reset(); };
        panSlider.onValueChange = [this]
        {
            juce::String error;
            if (! view.editor.setTrackPan (trackIndex, (float) panSlider.getValue(), error))
                view.report (error);
            panSlider.setTooltip ("Pan: " + formatPan (panSlider.getValue()) + " (drag, double-click for centre)");
            view.changed();
        };
        addAndMakeVisible (panSlider);

        muteButton.setClickingTogglesState (true);
        muteButton.lamp = colours::bad;
        muteButton.setTooltip ("Mute track");
        muteButton.setWantsKeyboardFocus (false);
        muteButton.onClick = [this]
        {
            juce::String error;
            if (! view.editor.setTrackMute (trackIndex, muteButton.getToggleState(), error))
                view.report (error);
            view.edited();
        };

        soloButton.setClickingTogglesState (true);
        soloButton.lamp = colours::good;
        soloButton.setTooltip ("Solo track");
        soloButton.setWantsKeyboardFocus (false);
        soloButton.onClick = [this]
        {
            juce::String error;
            if (! view.editor.setTrackSolo (trackIndex, soloButton.getToggleState(), error))
                view.report (error);
            view.edited();
        };

        armButton.setClickingTogglesState (true);
        armButton.lamp = colours::bad;
        armButton.setTooltip ("Arm track for recording");
        armButton.setWantsKeyboardFocus (false);
        armButton.onClick = [this]
        {
            juce::String error;
            if (! view.session.setTrackArm (trackIndex, armButton.getToggleState(), error))
                view.report (error);
            view.edited();
        };

        for (auto* b : { &muteButton, &soloButton, &armButton })
            addAndMakeVisible (*b);

        faderSlider.setSliderStyle (juce::Slider::LinearVertical);
        faderSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        faderSlider.setLookAndFeel (&view.look);
        faderSlider.setRange (-100.0, 12.0, 0.01);
        faderSlider.setDoubleClickReturnValue (true, 0.0);
        faderSlider.setWantsKeyboardFocus (false);
        faderSlider.setTooltip (faderTooltip ("Fader", 0.0));
        faderSlider.onDragStart = [this] { gainGesture = std::make_unique<Editor::Gesture> (view.editor, "Track gain"); };
        faderSlider.onDragEnd = [this] { gainGesture.reset(); };
        faderSlider.onValueChange = [this]
        {
            juce::String error;
            if (! view.editor.setTrackGain (trackIndex, (float) faderSlider.getValue(), error))
                view.report (error);
            const auto dbStr = formatDb (faderSlider.getValue());
            if (dbLabel.getText() != dbStr)
                dbLabel.setText (dbStr, juce::dontSendNotification);
            faderSlider.setTooltip (faderTooltip ("Fader", faderSlider.getValue()));
            view.changed();
        };
        addAndMakeVisible (faderSlider);

        dbLabel.setFont (ui::lcdFont (10.0f));
        dbLabel.setColour (juce::Label::textColourId, colours::amber);
        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setText ("+0.0 dB", juce::dontSendNotification);
        addAndMakeVisible (dbLabel);
    }

    ~TrackStrip() override
    {
        panSlider.setLookAndFeel (nullptr);
        faderSlider.setLookAndFeel (nullptr);
    }

    void sync (int index, const Track& t, const Session& s)
    {
        trackIndex = index;
        if (nameLabel.getText() != t.name)
            nameLabel.setText (t.name, juce::dontSendNotification);

        const bool busNow = t.isBus();
        if (isBus != busNow)
        {
            isBus = busNow;
            repaint();
        }
        armButton.setEnabled (! isBus);
        armButton.setVisible (! isBus);
        const juce::String armTip = isBus ? juce::String ("Bus track receives routed tracks/sends")
                                          : juce::String ("Arm track for recording");
        if (armButton.getTooltip() != armTip)
            armButton.setTooltip (armTip);

        juce::String outText ("Master");
        if (juce::isPositiveAndBelow (t.outputTrack, (int) s.tracks.size()))
            outText = s.tracks[(size_t) t.outputTrack]->name;
        if (outButton.getButtonText() != outText)
            outButton.setButtonText (outText);
        const auto outTip = "Output: " + outText + " (click to route)";
        if (outButton.getTooltip() != outTip)
            outButton.setTooltip (outTip);

        const juce::String sendText = t.sends.empty() ? juce::String ("SEND")
                                                    : ("SEND (" + juce::String ((int) t.sends.size()) + ")");
        if (sendButton.getButtonText() != sendText)
            sendButton.setButtonText (sendText);
        const auto sendTip = t.sends.empty() ? juce::String ("Aux sends (click to add)")
                                             : (juce::String ((int) t.sends.size()) + " active aux send(s)");
        if (sendButton.getTooltip() != sendTip)
            sendButton.setTooltip (sendTip);

        const auto infos = readSlotInfos (t.chain);
        const int prevCount = slotButtons.size();
        syncSlotButtons (view, trackIndex, infos, slotButtons, insertsContainer, addButton,
                         [this] (int slotIdx, bool wasBypassed)
                         {
                             return [this, slotIdx, wasBypassed]
                             {
                                 juce::String error;
                                 if (! view.editor.bypassPlugin (trackIndex, slotIdx, ! wasBypassed, error))
                                     view.report (error);
                                 view.edited();
                             };
                         });
        if (prevCount != slotButtons.size())
            layoutInsertsContainer (insertsViewport, insertsContainer, slotButtons, addButton);

        if (panGesture == nullptr && ! panSlider.isMouseButtonDown()
            && std::abs (panSlider.getValue() - (double) t.pan) > 1.0e-4)
        {
            panSlider.setValue (t.pan, juce::dontSendNotification);
            panSlider.setTooltip ("Pan: " + formatPan (t.pan) + " (drag, double-click for centre)");
        }
        muteButton.setToggleState (t.mute, juce::dontSendNotification);
        soloButton.setToggleState (t.solo, juce::dontSendNotification);
        armButton.setToggleState (t.armed, juce::dontSendNotification);

        if (gainGesture == nullptr && ! faderSlider.isMouseButtonDown()
            && std::abs (faderSlider.getValue() - (double) t.gainDb) > 1.0e-4)
        {
            faderSlider.setValue (t.gainDb, juce::dontSendNotification);
            faderSlider.setTooltip (faderTooltip ("Fader", t.gainDb));
        }
        const auto dbStr = formatDb (faderSlider.getValue());
        if (dbLabel.getText() != dbStr)
            dbLabel.setText (dbStr, juce::dontSendNotification);
    }

    void addPeaks (float peakL, float peakR)
    {
        const float prevL = meter.level, prevH = meter.hold;
        meter.add ((float) dbOf (std::max (peakL, peakR)));
        if (std::abs (meter.level - prevL) > 1.0e-4f || std::abs (meter.hold - prevH) > 1.0e-4f)
            repaint (meterArea.expanded (4.0f, 4.0f).toNearestInt());
    }

    bool isInteracting() const
    {
        return faderSlider.isMouseButtonDown() || panSlider.isMouseButtonDown()
               || gainGesture != nullptr || panGesture != nullptr;
    }

    void resized() override
    {
        const int w = getWidth(), h = getHeight();
        const int pad = 4;
        const int innerW = juce::jmax (1, w - 2 * pad);
        const int insertsH = juce::jlimit (36, 108, (h - 130) / 3);

        int y = pad;
        nameLabel.setBounds (pad, y, innerW, 17);
        y += 19;
        insertsViewport.setBounds (pad, y, innerW, insertsH);
        layoutInsertsContainer (insertsViewport, insertsContainer, slotButtons, addButton);
        y += insertsH + 3;

        outButton.setBounds (pad, y, innerW, 17);
        y += 19;
        sendButton.setBounds (pad, y, innerW, 17);
        y += 20;

        panSlider.setBounds (pad, y, innerW, 16);
        y += 19;

        const int btnGap = 3;
        const int btnW = (innerW - 2 * btnGap) / 3;
        muteButton.setBounds (pad, y, btnW, 18);
        soloButton.setBounds (pad + btnW + btnGap, y, btnW, 18);
        armButton.setBounds (pad + 2 * (btnW + btnGap), y, innerW - 2 * (btnW + btnGap), 18);
        y += 22;

        const int dbH = 15;
        const int faderH = juce::jmax (24, h - pad - dbH - y);
        faderSlider.setBounds (pad + 8, y, 28, faderH);
        meterArea = juce::Rectangle<float> ((float) (pad + 50), (float) y + 4.0f, 12.0f, (float) juce::jmax (8, faderH - 8));
        dbLabel.setBounds (pad, h - pad - dbH, innerW, dbH);
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        ui::hw::fillMetal (g, area, isBus ? colours::panel.interpolatedWith (colours::accent, 0.08f) : colours::panel);
        g.setColour (isBus ? colours::accent.withAlpha (0.45f) : colours::panelEdge.withAlpha (0.8f));
        g.drawRect (getLocalBounds(), 1);
        if (isBus)
        {
            const auto badge = armButton.getBounds().toFloat();
            g.setColour (colours::accent.withAlpha (0.18f));
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (colours::accent.withAlpha (0.7f));
            g.drawRoundedRectangle (badge, 3.0f, 1.0f);
            g.setFont (ui::labelFont (8.0f));
            g.setColour (colours::accent);
            g.drawFittedText ("[BUS]", armButton.getBounds(), juce::Justification::centred, 1);
        }
        paintVerticalLevelBar (g, meterArea, meter);
    }

    MixerView& view;
    int trackIndex = 0;
    bool isBus = false;
    juce::Label nameLabel, dbLabel;
    juce::Viewport insertsViewport;
    juce::Component insertsContainer;
    juce::OwnedArray<MixerSlotButton> slotButtons;
    juce::TextButton addButton { "+" };
    juce::TextButton outButton { "Master" }, sendButton { "SEND" };
    juce::Slider panSlider, faderSlider;
    MixerStripLamp muteButton { "M" }, soloButton { "S" }, armButton { "R" };
    MeterBallistics meter;
    juce::Rectangle<float> meterArea;
    std::unique_ptr<Editor::Gesture> panGesture, gainGesture;
};

// ---- MasterStrip ----------------------------------------------------------------------------------------------------------------
class MixerView::MasterStrip : public juce::Component
{
public:
    explicit MasterStrip (MixerView& owner) : view (owner)
    {
        titleLabel.setFont (ui::labelFont (11.0f));
        titleLabel.setColour (juce::Label::textColourId, colours::amber);
        titleLabel.setJustificationType (juce::Justification::centred);
        titleLabel.setText ("MASTER", juce::dontSendNotification);
        addAndMakeVisible (titleLabel);

        insertsViewport.setViewedComponent (&insertsContainer, false);
        insertsViewport.setScrollBarsShown (true, false);
        insertsViewport.setScrollBarThickness (5);
        insertsViewport.setWantsKeyboardFocus (false);
        addAndMakeVisible (insertsViewport);

        addButton.setWantsKeyboardFocus (false);
        addButton.setTooltip ("Add plugin to Master bus");
        addButton.onClick = [this]
        {
            if (view.onAddPlugin)
                view.onAddPlugin (-2, view.getLocalArea (&addButton, addButton.getLocalBounds()));
        };
        insertsContainer.addAndMakeVisible (addButton);

        peakLabel.setFont (ui::lcdFont (10.0f));
        peakLabel.setColour (juce::Label::textColourId, colours::dim);
        peakLabel.setJustificationType (juce::Justification::centred);
        peakLabel.setText ("-inf", juce::dontSendNotification);
        peakLabel.setTooltip ("Master peak hold over 2 seconds (dBFS)");
        addAndMakeVisible (peakLabel);

        clipLamp.setWantsKeyboardFocus (false);
        clipLamp.setTooltip ("Master CLIP indicator (click to clear)");
        clipLamp.onClick = [this] { clearClip(); };
        addAndMakeVisible (clipLamp);

        faderSlider.setSliderStyle (juce::Slider::LinearVertical);
        faderSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        faderSlider.setLookAndFeel (&view.look);
        faderSlider.setRange (-100.0, 12.0, 0.01);
        faderSlider.setDoubleClickReturnValue (true, 0.0);
        faderSlider.setWantsKeyboardFocus (false);
        faderSlider.setTooltip (faderTooltip ("Master gain", 0.0));
        faderSlider.onDragStart = [this] { gainGesture = std::make_unique<Editor::Gesture> (view.editor, "Master gain"); };
        faderSlider.onDragEnd = [this] { gainGesture.reset(); };
        faderSlider.onValueChange = [this]
        {
            view.editor.setMasterDb ((float) faderSlider.getValue());
            const auto dbStr = formatDb (faderSlider.getValue());
            if (gainLabel.getText() != dbStr)
                gainLabel.setText (dbStr, juce::dontSendNotification);
            faderSlider.setTooltip (faderTooltip ("Master gain", faderSlider.getValue()));
            view.changed();
        };
        addAndMakeVisible (faderSlider);

        gainLabel.setFont (ui::lcdFont (10.0f));
        gainLabel.setColour (juce::Label::textColourId, colours::amber);
        gainLabel.setJustificationType (juce::Justification::centred);
        gainLabel.setText ("+0.0 dB", juce::dontSendNotification);
        addAndMakeVisible (gainLabel);
    }

    ~MasterStrip() override
    {
        faderSlider.setLookAndFeel (nullptr);
    }

    void sync (const Session& s)
    {
        const auto infos = readSlotInfos (s.masterChain);
        const int prevCount = slotButtons.size();
        syncSlotButtons (view, -2, infos, slotButtons, insertsContainer, addButton,
                         [this] (int slotIdx, bool wasBypassed)
                         {
                             return [this, slotIdx, wasBypassed]
                             {
                                 juce::String error;
                                 if (! view.editor.bypassMasterPlugin (slotIdx, ! wasBypassed, error))
                                     view.report (error);
                                 view.edited();
                             };
                         });
        if (prevCount != slotButtons.size())
            layoutInsertsContainer (insertsViewport, insertsContainer, slotButtons, addButton);

        if (gainGesture == nullptr && ! faderSlider.isMouseButtonDown()
            && std::abs (faderSlider.getValue() - (double) s.masterDb) > 0.05)
        {
            faderSlider.setValue (s.masterDb, juce::dontSendNotification);
            faderSlider.setTooltip (faderTooltip ("Master gain", s.masterDb));
        }
        const auto dbStr = formatDb (faderSlider.getValue());
        if (gainLabel.getText() != dbStr)
            gainLabel.setText (dbStr, juce::dontSendNotification);
    }

    void addPeaks (float left, float right, double nowMs)
    {
        const float peak = juce::jmax (left, right);
        const float prevL = meterL.level, prevLH = meterL.hold;
        const float prevR = meterR.level, prevRH = meterR.hold;
        meterL.add ((float) dbOf (left));
        meterR.add ((float) dbOf (right));
        if (peak >= 1.0f)
            clipLatched = true;
        if (clipLamp.lit != clipLatched)
        {
            clipLamp.lit = clipLatched;
            clipLamp.repaint();
        }
        const float db = (float) dbOf (peak);
        if (db >= holdDb || nowMs - holdMs >= kPeakHoldMs)
        {
            holdDb = db;
            holdMs = nowMs;
        }
        const auto pText = holdDb <= -100.0f ? juce::String ("-inf") : juce::String (holdDb, 1) + " dB";
        if (peakLabel.getText() != pText)
            peakLabel.setText (pText, juce::dontSendNotification);
        if (std::abs (meterL.level - prevL) > 1.0e-4f || std::abs (meterL.hold - prevLH) > 1.0e-4f
            || std::abs (meterR.level - prevR) > 1.0e-4f || std::abs (meterR.hold - prevRH) > 1.0e-4f)
            repaint (barL.getUnion (barR).expanded (4.0f, 4.0f).toNearestInt());
    }

    void clearClip()
    {
        clipLatched = false;
        clipLamp.lit = false;
        clipLamp.repaint();
    }

    bool isInteracting() const
    {
        return faderSlider.isMouseButtonDown() || gainGesture != nullptr;
    }

    void resized() override
    {
        const int w = getWidth(), h = getHeight();
        const int pad = 4;
        const int innerW = juce::jmax (1, w - 2 * pad);
        const int insertsH = juce::jlimit (42, 120, (h - 90) / 3);

        int y = pad;
        titleLabel.setBounds (pad, y, innerW, 18);
        y += 21;
        insertsViewport.setBounds (pad, y, innerW, insertsH);
        layoutInsertsContainer (insertsViewport, insertsContainer, slotButtons, addButton);
        y += insertsH + 4;

        peakLabel.setBounds (pad, y, innerW, 18);
        y += 21;
        clipLamp.setBounds (pad + 22, y, innerW - 22, 20);
        y += 24;

        const int dbH = 16;
        const int faderH = juce::jmax (24, h - pad - dbH - y);
        faderSlider.setBounds (pad + 8, y, 28, faderH);
        const float barY = (float) y + 4.0f;
        const float barH = (float) juce::jmax (8, faderH - 8);
        barL = juce::Rectangle<float> ((float) (pad + 48), barY, 11.0f, barH);
        barR = juce::Rectangle<float> ((float) (pad + 64), barY, 11.0f, barH);
        gainLabel.setBounds (pad, h - pad - dbH, innerW, dbH);
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        ui::hw::fillMetal (g, area, colours::panel.brighter (0.04f));
        g.setColour (colours::amber.withAlpha (0.35f));
        g.drawRect (getLocalBounds(), 1);
        paintVerticalLevelBar (g, barL, meterL);
        paintVerticalLevelBar (g, barR, meterR);
    }

    MixerView& view;
    juce::Label titleLabel, peakLabel, gainLabel;
    juce::Viewport insertsViewport;
    juce::Component insertsContainer;
    juce::OwnedArray<MixerSlotButton> slotButtons;
    juce::TextButton addButton { "+" };
    MixerClipLamp clipLamp;
    juce::Slider faderSlider;
    MeterBallistics meterL, meterR;
    juce::Rectangle<float> barL, barR;
    bool clipLatched = false;
    float holdDb = -100.0f;
    double holdMs = -1.0e9;
    std::unique_ptr<Editor::Gesture> gainGesture;
};

// ---- MixerView ------------------------------------------------------------------------------------------------------------------
MixerView::MixerView (Session& s, Editor& e) : session (s), editor (e)
{
    setOpaque (true);
    setWantsKeyboardFocus (false);

    stripViewport.setViewedComponent (&stripContainer, false);
    stripViewport.setScrollBarsShown (false, true);
    stripViewport.setScrollBarThickness (8);
    stripViewport.setWantsKeyboardFocus (false);
    addAndMakeVisible (stripViewport);

    masterStrip = std::make_unique<MasterStrip> (*this);
    addAndMakeVisible (*masterStrip);

    refresh();
}

MixerView::~MixerView()
{
    trackStrips.clear();
    masterStrip.reset();
}

void MixerView::refresh()
{
    if (session.renderRunning())
        return;

    const auto rev = session.revision.load (std::memory_order_acquire);
    if (hasRefreshed && rev == lastRevision)
        return;

    const juce::ScopedTryLock sl (session.lock);
    if (! sl.isLocked())
        return;

    hasRefreshed = true;
    lastRevision = rev;

    const int n = (int) session.tracks.size();
    bool countChanged = (int) trackStrips.size() != n;
    if (countChanged && ! isInteracting())
    {
        while ((int) trackStrips.size() > n)
        {
            stripContainer.removeChildComponent (trackStrips.back().get());
            trackStrips.pop_back();
        }
        while ((int) trackStrips.size() < n)
        {
            trackStrips.push_back (std::make_unique<TrackStrip> (*this, (int) trackStrips.size()));
            stripContainer.addAndMakeVisible (*trackStrips.back());
        }
    }

    const int syncCount = juce::jmin (n, (int) trackStrips.size());
    for (int i = 0; i < syncCount; ++i)
        trackStrips[(size_t) i]->sync (i, *session.tracks[(size_t) i], session);

    masterStrip->sync (session);

    if (countChanged)
    {
        layoutStrips();
        repaint();
    }
}

void MixerView::tickMeters()
{
    if (session.renderRunning())
        return;

    const juce::ScopedTryLock sl (session.lock);
    if (! sl.isLocked())
        return;

    const int syncCount = juce::jmin ((int) trackStrips.size(), (int) session.tracks.size());
    for (int i = 0; i < syncCount; ++i)
    {
        const auto& t = *session.tracks[(size_t) i];
        trackStrips[(size_t) i]->addPeaks (t.peakL.load(), t.peakR.load());
    }
    masterStrip->addPeaks (session.masterPeakL.load(), session.masterPeakR.load(),
                           juce::Time::getMillisecondCounterHiRes());
}

void MixerView::clearMasterClip()
{
    if (masterStrip != nullptr)
        masterStrip->clearClip();
}

bool MixerView::isInteracting() const
{
    if (masterStrip != nullptr && masterStrip->isInteracting())
        return true;
    for (const auto& s : trackStrips)
        if (s->isInteracting())
            return true;
    return false;
}

void MixerView::layoutStrips()
{
    const int vw = stripViewport.getWidth();
    const int vh = stripViewport.getHeight();
    if (vw <= 0 || vh <= 0)
        return;

    const int n = (int) trackStrips.size();
    const int totalW = n * kTrackStripWidth + juce::jmax (0, n - 1) * kStripGap;
    const bool needHScroll = totalW > vw;
    const int stripH = juce::jmax (1, vh - (needHScroll ? stripViewport.getScrollBarThickness() : 0));
    stripContainer.setSize (juce::jmax (vw, totalW), stripH);
    for (int i = 0; i < n; ++i)
        trackStrips[(size_t) i]->setBounds (i * (kTrackStripWidth + kStripGap), 0, kTrackStripWidth, stripH);
}

void MixerView::resized()
{
    const auto r = getLocalBounds();
    const int masterW = juce::jmin (kMasterStripWidth, r.getWidth());
    const int gap = r.getWidth() > masterW + 4 ? 4 : 0;
    stripViewport.setBounds (0, 2, juce::jmax (0, r.getWidth() - masterW - gap), juce::jmax (0, r.getHeight() - 2));
    masterStrip->setBounds (r.getRight() - masterW, 2, masterW, juce::jmax (0, r.getHeight() - 2));
    layoutStrips();
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::panelEdge);
    g.fillRect (0, 0, getWidth(), 1);
    if (trackStrips.empty())
        ui::hw::drawLabel (g, "No tracks yet", stripViewport.getBounds(), juce::Justification::centred, colours::dim, 11.0f);
}

void MixerView::changed()
{
    if (onStructure) onStructure();
}

void MixerView::edited()
{
    refresh();
    changed();
}

void MixerView::showOutputMenu (int track, juce::Component& anchor)
{
    if (session.renderRunning())
    {
        report ("A render is running: wait for it or cancel it first.");
        return;
    }
    const auto safe = juce::Component::SafePointer<MixerView> (this);
    juce::PopupMenu menu;
    {
        const juce::ScopedTryLock sl (session.lock);
        if (! sl.isLocked() || ! juce::isPositiveAndBelow (track, (int) session.tracks.size()))
            return;
        const int curOut = session.tracks[(size_t) track]->outputTrack;
        menu.addItem ("Master", true, curOut == -1, [safe, track]
        {
            if (safe == nullptr) return;
            juce::String error;
            if (! safe->editor.setTrackOutput (track, -1, error))
                safe->report (error);
            safe->edited();
        });
        for (int b = 0; b < (int) session.tracks.size(); ++b)
        {
            if (b == track || ! session.tracks[(size_t) b]->isBus())
                continue;
            const auto label = "Track " + juce::String (b + 1) + ": " + session.tracks[(size_t) b]->name;
            menu.addItem (label, true, curOut == b, [safe, track, b]
            {
                if (safe == nullptr) return;
                juce::String error;
                if (! safe->editor.setTrackOutput (track, b, error))
                    safe->report (error);
                safe->edited();
            });
        }
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor));
}

void MixerView::showSendsMenu (int track, juce::Component& anchor)
{
    if (session.renderRunning())
    {
        report ("A render is running: wait for it or cancel it first.");
        return;
    }
    const auto safe = juce::Component::SafePointer<MixerView> (this);
    juce::PopupMenu menu;
    {
        const juce::ScopedTryLock sl (session.lock);
        if (! sl.isLocked() || ! juce::isPositiveAndBelow (track, (int) session.tracks.size()))
            return;
        const auto& trk = *session.tracks[(size_t) track];
        bool anyBus = false;
        for (int b = 0; b < (int) session.tracks.size(); ++b)
        {
            if (b == track || ! session.tracks[(size_t) b]->isBus())
                continue;
            anyBus = true;
            int sendIdx = -1;
            AuxSend cur;
            for (int si = 0; si < (int) trk.sends.size(); ++si)
            {
                if (trk.sends[(size_t) si].targetTrack == b)
                {
                    sendIdx = si;
                    cur = trk.sends[(size_t) si];
                    break;
                }
            }
            juce::PopupMenu busMenu;
            if (sendIdx < 0)
            {
                for (float db : { 0.0f, -6.0f, -12.0f })
                {
                    busMenu.addItem ("Send at " + juce::String (juce::roundToInt (db)) + " dB", true, false, [safe, track, b, db]
                    {
                        if (safe == nullptr) return;
                        juce::String error;
                        if (! safe->editor.setTrackSend (track, b, db, false, false, error))
                            safe->report (error);
                        safe->edited();
                    });
                }
            }
            else
            {
                for (float db : { 0.0f, -6.0f, -12.0f })
                {
                    const bool ticked = std::abs (cur.levelDb - db) < 0.05f;
                    busMenu.addItem (juce::String (juce::roundToInt (db)) + " dB", true, ticked, [safe, track, b, db, pre = cur.preFader, mute = cur.mute]
                    {
                        if (safe == nullptr) return;
                        juce::String error;
                        if (! safe->editor.setTrackSend (track, b, db, pre, mute, error))
                            safe->report (error);
                        safe->edited();
                    });
                }
                busMenu.addSeparator();
                busMenu.addItem ("Pre-fader", true, cur.preFader, [safe, track, b, lvl = cur.levelDb, pre = cur.preFader, mute = cur.mute]
                {
                    if (safe == nullptr) return;
                    juce::String error;
                    if (! safe->editor.setTrackSend (track, b, lvl, ! pre, mute, error))
                        safe->report (error);
                    safe->edited();
                });
                busMenu.addItem ("Mute", true, cur.mute, [safe, track, b, lvl = cur.levelDb, pre = cur.preFader, mute = cur.mute]
                {
                    if (safe == nullptr) return;
                    juce::String error;
                    if (! safe->editor.setTrackSend (track, b, lvl, pre, ! mute, error))
                        safe->report (error);
                    safe->edited();
                });
                busMenu.addSeparator();
                busMenu.addItem ("Remove", true, false, [safe, track, sendIdx]
                {
                    if (safe == nullptr) return;
                    juce::String error;
                    if (! safe->editor.removeTrackSend (track, sendIdx, error))
                        safe->report (error);
                    safe->edited();
                });
            }
            auto busLabel = "Track " + juce::String (b + 1) + ": " + session.tracks[(size_t) b]->name;
            if (sendIdx >= 0)
                busLabel += " (" + juce::String (cur.levelDb, 1) + " dB" + (cur.preFader ? ", pre" : "") + (cur.mute ? ", muted" : "") + ")";
            menu.addSubMenu (busLabel, busMenu);
        }
        if (! anyBus)
            menu.addItem ("No bus tracks yet", false, false, nullptr);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor));
}

int MixerView::trackStripCount() const { return (int) trackStrips.size(); }

juce::Slider* MixerView::trackFader (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->faderSlider : nullptr;
}

juce::Slider* MixerView::trackPan (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->panSlider : nullptr;
}

juce::Button* MixerView::trackMuteButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->muteButton : nullptr;
}

juce::Button* MixerView::trackSoloButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->soloButton : nullptr;
}

juce::Button* MixerView::trackArmButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->armButton : nullptr;
}

juce::Button* MixerView::trackOutButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->outButton : nullptr;
}

juce::Button* MixerView::trackSendButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->sendButton : nullptr;
}

juce::Button* MixerView::trackAddPluginButton (int track)
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? &trackStrips[(size_t) track]->addButton : nullptr;
}

juce::Button* MixerView::trackPluginButton (int track, int slot)
{
    if (! juce::isPositiveAndBelow (track, (int) trackStrips.size()))
        return nullptr;
    auto& btns = trackStrips[(size_t) track]->slotButtons;
    return juce::isPositiveAndBelow (slot, btns.size()) ? btns[slot] : nullptr;
}

int MixerView::trackPluginCount (int track) const
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? trackStrips[(size_t) track]->slotButtons.size() : 0;
}

juce::String MixerView::trackNameText (int track) const
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? trackStrips[(size_t) track]->nameLabel.getText() : juce::String();
}

juce::String MixerView::trackDbText (int track) const
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? trackStrips[(size_t) track]->dbLabel.getText() : juce::String();
}

juce::Slider* MixerView::masterFaderControl() { return &masterStrip->faderSlider; }
juce::Button* MixerView::masterAddPluginButton() { return &masterStrip->addButton; }
juce::Button* MixerView::masterPluginButton (int slot)
{
    return juce::isPositiveAndBelow (slot, masterStrip->slotButtons.size()) ? masterStrip->slotButtons[slot] : nullptr;
}
int MixerView::masterPluginCount() const { return masterStrip->slotButtons.size(); }
juce::Button* MixerView::masterClipButton() { return &masterStrip->clipLamp; }
bool MixerView::isMasterClipLatched() const { return masterStrip->clipLatched; }
juce::String MixerView::masterPeakText() const { return masterStrip->peakLabel.getText(); }
juce::String MixerView::masterGainText() const { return masterStrip->gainLabel.getText(); }

float MixerView::trackMeterLevelForTest (int track) const
{
    return juce::isPositiveAndBelow (track, (int) trackStrips.size()) ? trackStrips[(size_t) track]->meter.level : -100.0f;
}
float MixerView::masterMeterLevelLForTest() const { return masterStrip->meterL.level; }
float MixerView::masterMeterLevelRForTest() const { return masterStrip->meterR.level; }
} // namespace olympus::gui
