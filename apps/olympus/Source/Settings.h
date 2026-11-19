// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_core/juce_core.h>

// What Olympus remembers between runs: the folders it looks for the suite's plugins in, and the project it had open. The window and the
// MCP server read the same file, so the folder chosen in the window is the one an agent's session searches too.
namespace olympus
{
struct Settings
{
    juce::StringArray pluginFolders;
    juce::String lastProject;

    // OLYMPUS_SETTINGS names another file (the tests use it, so they never touch the real one).
    static juce::File defaultFile()
    {
        const auto custom = juce::SystemStats::getEnvironmentVariable ("OLYMPUS_SETTINGS", {});
        if (custom.isNotEmpty())
            return juce::File (custom);
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Olympus").getChildFile ("settings.xml");
    }

    static Settings load (const juce::File& file = defaultFile())
    {
        Settings s;
        if (auto xml = juce::parseXML (file); xml != nullptr && xml->hasTagName ("OlympusSettings"))
        {
            for (auto* f : xml->getChildWithTagNameIterator ("PluginFolder"))
                s.pluginFolders.addIfNotAlreadyThere (f->getStringAttribute ("path"));
            s.lastProject = xml->getStringAttribute ("lastProject");
        }
        return s;
    }

    void save (const juce::File& file = defaultFile()) const
    {
        juce::XmlElement xml ("OlympusSettings");
        xml.setAttribute ("lastProject", lastProject);
        for (auto& f : pluginFolders)
            xml.createNewChildElement ("PluginFolder")->setAttribute ("path", f);
        file.getParentDirectory().createDirectory();
        xml.writeTo (file);
    }

    // The folders to search besides the system's: the chosen ones and those named in OLYMPUS_PLUGIN_PATH.
    juce::FileSearchPath searchPath() const
    {
        juce::FileSearchPath path (juce::SystemStats::getEnvironmentVariable ("OLYMPUS_PLUGIN_PATH", {}));
        for (auto& f : pluginFolders)
            path.add (juce::File (f));
        return path;
    }
};
}
