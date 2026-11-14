// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <themis/Audition.h>
#include <themis/Listen.h>
#include <themis/LoudnessMonitor.h>
#include <themis/LowCrestMeter.h>
#include <themis/MasterAdvisor.h>
#include <themis/PhaseCancellation.h>
#include <themis/StereoScope.h>

#include <atomic>
#include <functional>
#include <memory>

// Audio passes through untouched (zero latency) and the plugin measures it: the long-term tonal balance against a
// reference curve, translation, phase, clipping, loudness and so on. The one exception is the Solo / Mute audition, which
// only changes the output while a button is pressed (see themis::Audition); the measurements always see the input.
class ThemisProcessor : public juce::AudioProcessor
{
public:
    ThemisProcessor();
    ~ThemisProcessor() override;

    // ---- juce::AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- shared with the editor (message thread unless noted) ----
    void resetAnalyzer()
    {
        analyzer.requestReset(); // also restarts the clip totals and the history graphs
        lowCrest.requestReset();
        scope.requestReset();
        loudness.requestReset();
    }

    // Solo / Mute audition: what is played instead of the input (nothing soloed or muted = the input, bit for bit). Not
    // saved with the session; ignored while the host renders offline so an export can never carry it.
    void setAudition (const themis::Switches& s) { audition.setSwitches (s); }
    themis::Switches auditionSwitches() const { return audition.switches(); }
    const themis::LoudnessMonitor& loudnessMonitor() const { return loudness; }

    // Listening aids, like the audition: not saved, ignored offline, bit-exact while off (see themis::ListenStage).
    // The streaming preview plays the master turned down to the Deliver for target; the reference plays a loaded track at the
    // loudness the master is heard at, on the host's timeline. updateListen() recomputes the gains from the measurements
    // (the editor's timer calls it) and frees a replaced reference once the audio thread has let go of it.
    void setStreamingPreview (bool on) { previewOn.store (on); updateListen(); }
    bool streamingPreview() const { return previewOn.load(); }
    void setReferenceOn (bool on) { listen.setReferenceOn (on && referenceClip != nullptr && !referenceStale.load()); updateListen(); }
    bool referenceOn() const { return listen.isReferenceOn(); }
    bool hasReference() const { return referenceClip != nullptr; }
    juce::String referenceName() const { return referenceFile.getFileNameWithoutExtension(); }
    float referenceLufs() const { return referenceClip != nullptr ? referenceClip->integratedLufs : themis::LoudnessMeter::kSilence; }
    themis::listen::ReferenceGain referenceGain() const { return refGain; }
    juce::String deliveryName() const { return deliveryTarget().shortName; }
    float deliveryLufs() const { return deliveryTarget().lufs; }
    float previewGainDb() const { return previewDb; }
    void loadReference (const juce::File& file, std::function<void (const juce::String& error)> onDone);
    void updateListen();
    static constexpr double kMaxReferenceSeconds = 360.0;

    // Everything the master plan is made of, read from the measurements (message thread).
    themis::MasterInputs masterInputs() const;

    // Thread-safe read of the long-term spectrum. Returns false until enough audio has been analysed.
    bool readSpectrum (themis::BandArray& out) const;

    // A held copy of the tonal curve, drawn behind the live one as a ghost trace so a change can be judged against what it was (A/B).
    // Message thread. Not saved with the session. Taking one needs a measured curve; Reset leaves it, releasing it ends it.
    bool holdCurve()
    {
        themis::BandArray now {};
        if (!readSpectrum (now))
            return false;
        heldCurve = now;
        curveHeld = true;
        return true;
    }
    void releaseHeldCurve() { curveHeld = false; }
    bool hasHeldCurve() const { return curveHeld; }
    bool readHeldCurve (themis::BandArray& out) const
    {
        if (curveHeld)
            out = heldCurve;
        return curveHeld;
    }

    // Per-band mono fold in dB (<= 0): how much of each band's power survives a sum to mono.
    // Returns false until enough audio has been analysed.
    bool readMonoFold (themis::BandArray& out) const;

    // Per-band phase relationship between the channels (see themis::analysePhase). Returns false until enough audio
    // has been analysed.
    bool readPhase (themis::PhaseBands& out) const;

    // Energy per range and bass fundamental of every analysed frame, for the time graphs.
    const themis::FrameHistory& frameHistory() const { return analyzer.history(); }
    const themis::SpectrumHistory& spectrumHistory() const { return analyzer.spectrum(); }

    // The playback device selected for the translation preview, or nullptr when it is "Off".
    const themis::DeviceProfile* activeDevice() const;

    // Low-end (20-250 Hz) crest factor in dB. Returns false until enough audio has been measured.
    bool readLowCrest (float& outDb) const;

    // Peaks and clipping of everything that has played since the last reset. Returns false until audio has been measured.
    bool readClip (themis::ClipTotals& out) const;

    // Stereo image: raw L/R samples for the vectorscope and the smoothed phase correlation (-1..+1).
    const themis::StereoScope& stereoScope() const { return scope; }

    // Target curves
    themis::TargetCurve activeTarget() const; // honours the "target" parameter
    void setCustomCurve (const themis::TargetCurve& c, bool selectIt);
    bool hasCustomCurve() const;

    // Reads the given audio files in a background thread, averages their tonal balance and installs the
    // result as the "Custom" target. `onDone` is called on the message thread (error empty on success).
    void learnFromFiles (const juce::Array<juce::File>& files, std::function<void (const juce::String& error)> onDone);

    juce::AudioProcessorValueTreeState apvts;
    params::Reader paramReader;

private:
    // Runs the FFT analysis off the audio thread: processBlock() only queues samples, this drains them.
    class AnalysisThread : public juce::Thread
    {
    public:
        explicit AnalysisThread (themis::TonalAnalyzer& a) : juce::Thread ("Themis analysis"), analyzer (a) {}
        void run() override
        {
            while (!threadShouldExit())
            {
                analyzer.pump();
                wait (15);
            }
        }

    private:
        themis::TonalAnalyzer& analyzer;
    };

    void setParameterValue (const juce::String& id, float plainValue);
    const params::LoudnessTarget& deliveryTarget() const;
    void stopAnalysis();
    void syncReferenceRate();

    themis::TonalAnalyzer analyzer;
    themis::LowCrestMeter lowCrest;
    themis::StereoScope scope;
    themis::LoudnessMonitor loudness;
    themis::Audition audition;
    themis::ListenStage listen;
    std::atomic<bool> previewOn { false };
    std::unique_ptr<themis::ReferenceClip> referenceClip;
    std::vector<std::unique_ptr<themis::ReferenceClip>> retiredClips; // message thread only; the audio thread reads through `listen`
    juce::File referenceFile;
    themis::BandArray heldCurve {}; // message thread only
    bool curveHeld = false;
    std::atomic<double> referenceRate { 0.0 }; // the rate the loaded reference was decoded for; 0 = none (read by prepareToPlay)
    std::atomic<bool> referenceStale { false }; // decoded for another rate: not playable until the decode for this one lands
    float previewDb = 0.0f;
    themis::listen::ReferenceGain refGain;
    std::atomic<bool> loadingReference { false };
    std::unique_ptr<AnalysisThread> analysisThread; // exists between prepareToPlay and releaseResources
    std::atomic<bool> prepared { false };
    bool wasPlaying = false;       // audio thread: the host was playing at the previous block
    int64_t expectedPosition = -1; // audio thread: where that block ended on the host's timeline (-1: unknown)

    mutable juce::CriticalSection curveLock;
    themis::TargetCurve customCurve;
    bool customCurveValid = false;

    std::unique_ptr<juce::ThreadPool> pool; // created on first "Learn from tracks", so loading the plugin starts no threads
    std::atomic<bool> learning { false };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemisProcessor)
};
