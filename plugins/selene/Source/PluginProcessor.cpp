// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <array>
#include <pantheon/ui/Scaled.h>

SeleneProcessor::SeleneProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undo, "STATE", selene::createLayout())
{
}

bool SeleneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    for (const auto& bus : layouts.inputBuses)
        if (! bus.isDisabled())
            return false;
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SeleneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    keyboardState.reset();
}

void SeleneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    if (buffer.getNumChannels() <= 0 || numSamples <= 0)
        return;

    keyboardState.processNextMidiBuffer (midiMessages, 0, numSamples, true);

    std::array<selene::MidiEvent, 512> events {};
    int numEvents = 0;

    for (const auto meta : midiMessages)
    {
        if (numEvents >= (int) events.size())
            break;

        const auto msg = meta.getMessage();
        selene::MidiEvent ev;
        ev.sampleOffset = std::clamp (meta.samplePosition, 0, numSamples - 1);
        ev.channel = msg.getChannel();

        if (msg.isNoteOn())
        {
            ev.type = selene::MidiEvent::Type::noteOn;
            ev.data1 = msg.getNoteNumber();
            ev.value = msg.getFloatVelocity();
            events[(size_t) numEvents++] = ev;
        }
        else if (msg.isNoteOff())
        {
            ev.type = selene::MidiEvent::Type::noteOff;
            ev.data1 = msg.getNoteNumber();
            ev.value = 0.0f;
            events[(size_t) numEvents++] = ev;
        }
        else if (msg.isPitchWheel())
        {
            ev.type = selene::MidiEvent::Type::pitchBend;
            ev.value = (float) (msg.getPitchWheelValue() - 8192) / 8192.0f;
            events[(size_t) numEvents++] = ev;
        }
        else if (msg.isController())
        {
            ev.type = selene::MidiEvent::Type::controlChange;
            ev.data1 = msg.getControllerNumber();
            ev.value = (float) msg.getControllerValue() / 127.0f;
            events[(size_t) numEvents++] = ev;
        }
        else if (msg.isAllNotesOff())
        {
            ev.type = selene::MidiEvent::Type::allNotesOff;
            events[(size_t) numEvents++] = ev;
        }
        else if (msg.isAllSoundOff())
        {
            ev.type = selene::MidiEvent::Type::allSoundOff;
            events[(size_t) numEvents++] = ev;
        }
    }

    engine.process (buffer.getWritePointer (0),
                    buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr,
                    numSamples,
                    events.data(),
                    numEvents,
                    selene::readParams (apvts));
}

void SeleneProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();
    engine.reset();
}

void SeleneProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    programs.saveTo (state);
    ab.saveTo (state);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void SeleneProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            ab.loadFrom (tree);
            apvts.replaceState (tree);
            programs.loadFrom (apvts.state);
        }
}

juce::AudioProcessorEditor* SeleneProcessor::createEditor()
{
    return new ui::ScaledEditor<SeleneEditor> (*this, apvts.state, *this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SeleneProcessor(); }
