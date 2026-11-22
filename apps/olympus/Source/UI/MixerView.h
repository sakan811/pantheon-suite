// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "LampButton.h"
#include "LevelMeter.h"
#include "../Edit.h"
#include "../Session.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

#include <functional>
#include <memory>
#include <vector>

namespace olympus::gui
{
// Console mixer view: horizontally scrollable per-track channel strips (name, plugin inserts, pan, M/S/R, fader, level meter, dB)
// and a fixed right-side MASTER strip (master plugin inserts, CLIP lamp, peak readout, stereo meter, master fader).
// Reads Session state non-blockingly (try-lock + revision check; skips locking while a render runs) and routes edits through Editor.
class MixerView : public juce::Component
{
public:
    static constexpr int kTrackStripWidth = 96;
    static constexpr int kMasterStripWidth = 104;
    static constexpr int kStripGap = 2;

    MixerView (Session& session, Editor& editor);
    ~MixerView() override;

    std::function<void (int track, int slot)> onOpenPlugin;
    std::function<void (int track, juce::Rectangle<int> anchor)> onAddPlugin;
    std::function<void (int track, int slot, juce::Rectangle<int> anchor)> onPluginMenu;
    std::function<void()> onStructure;
    std::function<void (const juce::String&)> reportError;

    void refresh();
    void tickMeters();
    void clearMasterClip();
    bool isInteracting() const;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Inspection and test helpers (used by headless UI checks)
    int trackStripCount() const;
    juce::Slider* trackFader (int track);
    juce::Slider* trackPan (int track);
    juce::Button* trackMuteButton (int track);
    juce::Button* trackSoloButton (int track);
    juce::Button* trackArmButton (int track);
    juce::Button* trackOutButton (int track);
    juce::Button* trackSendButton (int track);
    juce::Button* trackAddPluginButton (int track);
    juce::Button* trackPluginButton (int track, int slot);
    int trackPluginCount (int track) const;
    juce::String trackNameText (int track) const;
    juce::String trackDbText (int track) const;

    juce::Slider* masterFaderControl();
    juce::Button* masterAddPluginButton();
    juce::Button* masterPluginButton (int slot);
    int masterPluginCount() const;
    juce::Button* masterClipButton();
    bool isMasterClipLatched() const;
    juce::String masterPeakText() const;
    juce::String masterGainText() const;

    float trackMeterLevelForTest (int track) const;
    float masterMeterLevelLForTest() const;
    float masterMeterLevelRForTest() const;

private:
    class TrackStrip;
    class MasterStrip;

    Session& session;
    Editor& editor;
    HardwareLookAndFeel look; // declared before strips so child sliders never outlive look

    juce::Viewport stripViewport;
    juce::Component stripContainer;
    std::vector<std::unique_ptr<TrackStrip>> trackStrips;
    std::unique_ptr<MasterStrip> masterStrip;

    juce::uint64 lastRevision = 0;
    bool hasRefreshed = false;

    void layoutStrips();
    void showOutputMenu (int track, juce::Component& anchor);
    void showSendsMenu (int track, juce::Component& anchor);
    void report (const juce::String& error) { if (reportError && error.isNotEmpty()) reportError (error); }
    void changed();
    void edited();
};
} // namespace olympus::gui
