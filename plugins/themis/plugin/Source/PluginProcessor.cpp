// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"

#include "PluginEditor.h"

#include <pantheon/dsp/Biquad.h>

ThemisProcessor::ThemisProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "THEMIS", params::createLayout()),
      paramReader (apvts)
{
}

ThemisProcessor::~ThemisProcessor()
{
    stopAnalysis();
    alive->store (false);
    if (pool != nullptr)
        pool->removeAllJobs (true, 10000);
}

void ThemisProcessor::stopAnalysis()
{
    if (analysisThread != nullptr)
        analysisThread->stopThread (2000);
    analysisThread.reset();
}

void ThemisProcessor::prepareToPlay (double sr, int)
{
    stopAnalysis(); // prepare() must not overlap with pump()
    analyzer.prepare (sr);
    lowCrest.prepare (sr);
    scope.prepare (sr);
    loudness.prepare (sr);
    audition.prepare (sr);
    listen.prepare (sr);
    wasPlaying = false;
    expectedPosition = -1;
    prepared = true;
    // A reference decoded for another rate would play at the wrong pitch: stop it now, whether or not the editor is open,
    // and decode it again for this rate (on the message thread).
    if (const double decoded = referenceRate.load(); decoded > 0.0 && std::abs (decoded - sr) > 0.5)
    {
        referenceStale.store (true);
        listen.setReferenceOn (false);
        juce::MessageManager::callAsync ([this, aliveFlag = alive] {
            if (aliveFlag->load())
                syncReferenceRate();
        });
    }
    analysisThread = std::make_unique<AnalysisThread> (analyzer);
    analysisThread->startThread();
}

void ThemisProcessor::releaseResources()
{
    prepared = false;
    stopAnalysis();
}

bool ThemisProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
           && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void ThemisProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (!prepared || buffer.getNumChannels() < 2 || buffer.getNumSamples() == 0)
        return;

    // Where the host is: playing or not, and its timeline position in samples (-1 when it gives none).
    bool hostPlaying = false;
    int64_t position = -1;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition(); pos.hasValue())
        {
            hostPlaying = pos->getIsPlaying();
            if (const auto t = pos->getTimeInSamples(); t.hasValue())
                position = *t;
        }
    // Reset on play: a start from the beginning of the timeline restarts the measurement (before this block is measured). Playing
    // on from where the last block ended is not a start, so a run of blocks never resets itself.
    if (hostPlaying && position >= 0 && paramReader.resetsOnPlay()
        && position <= static_cast<int64_t> (params::kStartWindowSeconds * getSampleRate()) && (!wasPlaying || position != expectedPosition))
        resetAnalyzer();
    wasPlaying = hostPlaying;
    expectedPosition = hostPlaying && position >= 0 ? position + buffer.getNumSamples() : -1;

    // Measure first: everything below sees the input, whatever is being auditioned. The FFT work happens on the analysis thread.
    const float avgSeconds = paramReader.averagingSeconds();
    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getReadPointer (1);
    const int n = buffer.getNumSamples();
    analyzer.setAveragingSeconds (avgSeconds);
    analyzer.push (l, r, n);
    lowCrest.setAveragingSeconds (avgSeconds);
    lowCrest.process (l, r, n);
    scope.push (l, r, n);
    loudness.process (l, r, n);

    // The audition is the only thing that writes to the buffer, and only while a Solo / Mute is engaged. A render
    // (non-realtime) is never altered.
    if (!isNonRealtime())
    {
        audition.process (buffer.getWritePointer (0), buffer.getWritePointer (1), n);
        listen.process (buffer.getWritePointer (0), buffer.getWritePointer (1), n, hostPlaying ? position : -1);
    }
}

// ---- listening aids -------------------------------------------------------------------------
void ThemisProcessor::updateListen()
{
    if (!retiredClips.empty() && listen.retiredClipSafe())
        retiredClips.clear();

    syncReferenceRate();

    const float master = loudness.hasData() ? loudness.integrated() : themis::LoudnessMeter::kSilence;
    previewDb = previewOn.load() ? themis::listen::previewGainDb (master, deliveryTarget().lufs) : 0.0f;
    listen.setMasterGainDb (previewDb);
    refGain = referenceClip != nullptr ? themis::listen::referenceGainDb (*referenceClip, master + previewDb) : themis::listen::ReferenceGain {};
    listen.setReferenceGainDb (refGain.db);
}

// Message thread. A reference decoded for another sample rate would play at the wrong pitch: it is switched off (prepareToPlay
// does that too, at once) and decoded again for the current rate.
void ThemisProcessor::syncReferenceRate()
{
    const double sr = getSampleRate();
    if (referenceClip == nullptr || sr <= 0.0 || std::abs (referenceRate.load() - sr) <= 0.5)
        return;
    referenceStale.store (true);
    listen.setReferenceOn (false);
    if (!loadingReference.load())
        loadReference (referenceFile, [] (const juce::String&) {});
}

void ThemisProcessor::loadReference (const juce::File& file, std::function<void (const juce::String&)> onDone)
{
    if (loadingReference.exchange (true))
    {
        onDone ("Already loading a reference.");
        return;
    }
    const double rate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    if (pool == nullptr)
        pool = std::make_unique<juce::ThreadPool> (1);
    pool->addJob ([this, file, rate, onDone, aliveFlag = alive] {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        juce::String error;
        auto clip = std::make_unique<themis::ReferenceClip>();
        if (reader == nullptr || reader->lengthInSamples <= 0)
            error = "Could not read " + file.getFileName();
        else
        {
            const auto length = std::min<juce::int64> (reader->lengthInSamples, static_cast<juce::int64> (kMaxReferenceSeconds * reader->sampleRate));
            juce::AudioBuffer<float> source (2, static_cast<int> (length));
            const bool mono = reader->numChannels < 2;
            reader->read (&source, 0, static_cast<int> (length), 0, true, !mono);
            if (mono)
                source.copyFrom (1, 0, source, 0, 0, static_cast<int> (length));

            // Resampled once, here, so the audio thread only copies samples.
            const double ratio = reader->sampleRate / rate;
            if (ratio > 1.0)
            {
                // The interpolator below has no low-pass of its own: going down (a 96 kHz file at 48 kHz) everything above the new
                // Nyquist would fold back into the band. Two eighth-order Butterworths at 0.42 of the new rate take it out first
                // (about 55 dB down at 1.5x the new Nyquist).
                for (int c = 0; c < 2; ++c)
                {
                    pdsp::Butterworth lowpass[2];
                    for (auto& f : lowpass)
                        f.setup (pdsp::Butterworth::Type::lowpass, 8, reader->sampleRate, 0.42 * rate);
                    float* x = source.getWritePointer (c);
                    for (int i = 0; i < static_cast<int> (length); ++i)
                        x[i] = static_cast<float> (lowpass[1].process (lowpass[0].process (static_cast<double> (x[i]))));
                }
            }
            const auto outLength = static_cast<size_t> (static_cast<double> (length) / ratio);
            clip->left.resize (outLength);
            clip->right.resize (outLength);
            for (int c = 0; c < 2; ++c)
            {
                juce::LagrangeInterpolator interp;
                auto& out = c == 0 ? clip->left : clip->right;
                interp.process (ratio, source.getReadPointer (c), out.data(), static_cast<int> (outLength), static_cast<int> (length), 0);
            }
            clip->measure (rate);
            if (clip->integratedLufs <= themis::LoudnessMeter::kSilence + 1.0f)
                error = file.getFileName() + " is too short or silent to measure.";
        }
        juce::MessageManager::callAsync ([this, file, rate, onDone, error, aliveFlag, raw = clip.release()] {
            std::unique_ptr<themis::ReferenceClip> loaded (raw);
            if (!aliveFlag->load())
                return;
            loadingReference.store (false);
            if (error.isEmpty() && getSampleRate() > 0.0 && std::abs (rate - getSampleRate()) > 0.5)
            {
                loaded.reset(); // the host changed the rate while this was decoding: decode again for the new one
                loadReference (file, onDone);
                return;
            }
            if (error.isEmpty())
            {
                // The old clip stays alive until the audio thread has finished a block without it (updateListen frees it).
                listen.setClip (loaded.get());
                if (referenceClip != nullptr)
                    retiredClips.push_back (std::move (referenceClip));
                referenceClip = std::move (loaded);
                referenceFile = file;
                referenceRate.store (rate);
                referenceStale.store (false);
                updateListen();
            }
            onDone (error);
        });
    });
}

juce::AudioProcessorEditor* ThemisProcessor::createEditor()
{
    return new ThemisEditor (*this);
}

// ---- state ------------------------------------------------------------------------------------
void ThemisProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock sl (curveLock);
        if (customCurveValid)
            state.setProperty ("customCurve", juce::String (customCurve.toJson()), nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ThemisProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName (apvts.state.getType()))
        return;
    const auto tree = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (tree);

    // The loaded state replaces the custom curve; one without a (valid) curve must not keep the previous one.
    themis::TargetCurve c;
    const bool valid = themis::TargetCurve::fromJson (tree.getProperty ("customCurve").toString().toStdString(), c);
    const juce::ScopedLock sl (curveLock);
    customCurveValid = valid;
    if (valid)
        customCurve = c;
}

// ---- analysis / targets -----------------------------------------------------------------------
bool ThemisProcessor::readSpectrum (themis::BandArray& out) const
{
    if (!analyzer.hasData())
        return false;
    analyzer.readNormalisedDb (out);
    return true;
}

bool ThemisProcessor::readMonoFold (themis::BandArray& out) const
{
    if (!analyzer.hasMonoFoldData())
        return false;
    analyzer.readMonoFoldDb (out);
    return true;
}

bool ThemisProcessor::readPhase (themis::PhaseBands& out) const
{
    if (!analyzer.hasMonoFoldData())
        return false;
    analyzer.readPhase (out);
    return true;
}

const themis::DeviceProfile* ThemisProcessor::activeDevice() const
{
    return params::deviceForChoice (static_cast<int> (paramReader.value (params::device)));
}

bool ThemisProcessor::readLowCrest (float& outDb) const
{
    if (!lowCrest.hasData())
        return false;
    outDb = lowCrest.crestDb();
    return true;
}

const params::LoudnessTarget& ThemisProcessor::deliveryTarget() const
{
    return params::loudnessTargets[juce::jlimit (0, params::kNumLoudnessTargets - 1, static_cast<int> (paramReader.value (params::loudnessTarget)))];
}

themis::MasterInputs ThemisProcessor::masterInputs() const
{
    themis::MasterInputs in;
    in.analysedSeconds = loudness.seconds();
    in.haveLoudness = loudness.hasData();
    in.integratedLufs = loudness.integrated();
    const auto& delivery = deliveryTarget();
    in.targetLufs = delivery.lufs;
    in.truePeakCeilingDb = paramReader.truePeakCeilingDb();
    in.deliveryName = delivery.shortName;
    readClip (in.clip);

    themis::BandArray measured {}, fold {};
    if (readSpectrum (measured) && readMonoFold (fold)) // the plan only uses the mono fold: the curve and the device are judged against the reference, which is taste
        themis::addMonoFold (in.translation, measured, fold);

    themis::PhaseBands phase {};
    in.havePhase = readPhase (phase);
    if (in.havePhase)
        in.phase = themis::analysePhase (phase);
    in.haveCrest = readLowCrest (in.lowCrestDb);

    in.frames.resize (themis::FrameHistory::kCapacity);
    in.frames.resize (static_cast<size_t> (frameHistory().copyRecent (in.frames.data(), themis::FrameHistory::kCapacity)));
    return in;
}

bool ThemisProcessor::readClip (themis::ClipTotals& out) const
{
    out = analyzer.clipTotals();
    return out.have;
}

themis::TargetCurve ThemisProcessor::activeTarget() const
{
    const auto& built = themis::TargetCurve::builtIns();
    const int idx = static_cast<int> (paramReader.value (params::target));
    if (idx >= 0 && idx < params::customTargetIndex())
        return built[static_cast<size_t> (idx)];
    const juce::ScopedLock sl (curveLock);
    return customCurveValid ? customCurve : built.front();
}

bool ThemisProcessor::hasCustomCurve() const
{
    const juce::ScopedLock sl (curveLock);
    return customCurveValid;
}

void ThemisProcessor::setParameterValue (const juce::String& id, float plainValue)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

void ThemisProcessor::setCustomCurve (const themis::TargetCurve& c, bool selectIt)
{
    {
        const juce::ScopedLock sl (curveLock);
        customCurve = c;
        customCurveValid = true;
    }
    if (selectIt)
        setParameterValue (params::target, static_cast<float> (params::customTargetIndex()));
}

void ThemisProcessor::learnFromFiles (const juce::Array<juce::File>& files, std::function<void (const juce::String&)> onDone)
{
    if (learning.exchange (true))
    {
        onDone ("Already analysing reference tracks.");
        return;
    }

    if (pool == nullptr)
        pool = std::make_unique<juce::ThreadPool> (1);
    pool->addJob ([this, files, onDone, aliveFlag = alive] {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::vector<themis::BandArray> tracks;
        juce::StringArray failed;
        for (const auto& file : files)
        {
            if (!aliveFlag->load())
                return;
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
            if (reader == nullptr || reader->lengthInSamples <= 0)
            {
                failed.add (file.getFileName());
                continue;
            }
            // On the heap: a TonalAnalyzer holds its history rings (about 390 KB), too big for a pool thread's stack.
            auto analyzerOwner = std::make_unique<themis::TonalAnalyzer>();
            auto& trackAnalyzer = *analyzerOwner;
            trackAnalyzer.prepareOffline (reader->sampleRate); // whole-track average

            constexpr int chunkSize = 1 << 15;
            juce::AudioBuffer<float> buf (2, chunkSize);
            const bool mono = reader->numChannels < 2;
            for (juce::int64 pos = 0; pos < reader->lengthInSamples && aliveFlag->load(); pos += chunkSize)
            {
                const int n = static_cast<int> (std::min<juce::int64> (chunkSize, reader->lengthInSamples - pos));
                reader->read (&buf, 0, n, pos, true, !mono);
                trackAnalyzer.process (buf.getReadPointer (0), buf.getReadPointer (mono ? 0 : 1), n);
            }
            themis::BandArray arr {};
            if (trackAnalyzer.hasData())
            {
                trackAnalyzer.readNormalisedDb (arr);
                tracks.push_back (arr);
            }
            else
                failed.add (file.getFileName() + " (too short or silent)");
        }

        juce::String error;
        themis::TargetCurve curve;
        if (tracks.empty())
            error = "Could not analyse any of the files" + (failed.isEmpty() ? juce::String() : ": " + failed.joinIntoString (", "));
        else
        {
            const auto name = files.size() == 1 ? files[0].getFileNameWithoutExtension()
                                                : "Custom (" + juce::String (static_cast<int> (tracks.size())) + " tracks)";
            curve = themis::TargetCurve::fromTracks (name.toStdString(), tracks);
            if (!failed.isEmpty())
                error = "Skipped: " + failed.joinIntoString (", ");
        }

        juce::MessageManager::callAsync ([this, aliveFlag, curve, error, onDone, ok = !tracks.empty()] {
            if (!aliveFlag->load())
                return;
            if (ok)
                setCustomCurve (curve, true);
            learning.store (false);
            onDone (error);
        });
    });
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ThemisProcessor();
}
