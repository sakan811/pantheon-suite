// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"

namespace hep
{
// The user's own presets: one small XML file each in a folder of the user's, so they are there in every session and project (the factory
// presets in `presets()` stay as they are and are what the host's program list shows). A file holds the value of every parameter that a
// preset sets (all but the session-wide ones, see `isGlobalParameter`), as 0..1 values by parameter id; a parameter missing from a file
// (one added after it was saved) falls back to its default, as it does for a factory preset.
class UserPresets
{
public:
    explicit UserPresets (juce::File folder) : dir (std::move (folder)) {}

    static juce::File defaultFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Pantheon").getChildFile ("Hephaestus Forge")
            .getChildFile ("Presets");
    }

    // Names, sorted, as the user typed them.
    juce::StringArray names() const
    {
        juce::StringArray list;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
            if (auto xml = juce::XmlDocument::parse (f))
                if (xml->hasTagName ("HephaestusPreset") && xml->getStringAttribute ("name").isNotEmpty())
                    list.add (xml->getStringAttribute ("name"));
        list.sortNatural();
        return list;
    }

    // Writes (or replaces) the preset called `name` from the current parameters. False for an empty name or a folder that cannot be written.
    bool save (juce::AudioProcessorValueTreeState& s, const juce::String& rawName) const
    {
        const auto name = rawName.trim().substring (0, 40);
        if (name.isEmpty() || ! dir.createDirectory().wasOk())
            return false;
        juce::XmlElement xml ("HephaestusPreset");
        xml.setAttribute ("name", name);
        for (auto* param : s.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (! isGlobalParameter (ranged->paramID))
                {
                    auto* entry = xml.createNewChildElement ("P");
                    entry->setAttribute ("id", ranged->paramID);
                    entry->setAttribute ("v", (double) ranged->getValue());
                }
        return xml.writeTo (fileFor (name));
    }

    bool remove (const juce::String& name) const { return fileFor (name.trim()).deleteFile(); }

    // Sets every parameter once, to its final value (see applyPreset). False when there is no such preset.
    bool apply (juce::AudioProcessorValueTreeState& s, const juce::String& name) const
    {
        auto xml = juce::XmlDocument::parse (fileFor (name.trim()));
        if (xml == nullptr || ! xml->hasTagName ("HephaestusPreset"))
            return false;
        ui::beginUndoStep (s, "Preset");
        std::vector<std::pair<juce::RangedAudioParameter*, float>> targets;
        for (auto* param : s.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (! isGlobalParameter (ranged->paramID))
                    targets.emplace_back (ranged, ranged->getDefaultValue());
        for (auto* p : xml->getChildWithTagNameIterator ("P"))
            for (auto& t : targets)
                if (t.first->paramID == p->getStringAttribute ("id"))
                {
                    t.second = juce::jlimit (0.0f, 1.0f, (float) p->getDoubleAttribute ("v", (double) t.first->getDefaultValue()));
                    break;
                }
        writeTargets (targets);
        return true;
    }

private:
    juce::File fileFor (const juce::String& name) const
    {
        return dir.getChildFile (juce::File::createLegalFileName (name).replaceCharacter (' ', '_') + ".xml");
    }

    juce::File dir;
};
} // namespace hep
