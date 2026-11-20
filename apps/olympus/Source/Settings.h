// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_core/juce_core.h>

// What Olympus remembers between runs: the folders it looks for plugins in, whether it hosts only the suite's own, and the project it had open. The
// window and the MCP server read the same file, so the folder chosen in the window is the one an agent's session searches too. The plugin scan
// (Catalog.h) keeps its cache and its dead-man's pedal next to this file.
namespace olympus
{
struct Settings
{
    juce::StringArray pluginFolders;
    juce::String lastProject;
    // false: every VST3 (and AU on macOS) the folders hold is scanned and can be hosted. true: only bundles named as one of the suite's are
    // read, as before the scan looked at everything (a scan then never opens a third-party plugin).
    bool scanSuiteOnly = false;
    // The audio device's settings as XML text (juce::AudioDeviceManager::createStateXml), so the device comes back as it was chosen.
    juce::String audioDeviceState;
    // A round-trip latency the user types in, added to the device's own when a take is placed (Record.h). Saved as an attribute.
    int extraLatencySamples = 0;

    // OLYMPUS_SETTINGS names another file (the tests use it, so they never touch the real one).
    static juce::File defaultFile()
    {
        const auto custom = juce::SystemStats::getEnvironmentVariable ("OLYMPUS_SETTINGS", {});
        if (custom.isNotEmpty())
            return juce::File (custom);
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Olympus").getChildFile ("settings.xml");
    }

    // The scan's result as XML (what the window and the server start from), and the file a plugin is written to while it is being read, so a
    // plugin that crashes in that read is blacklisted the next time. Both sit in the settings folder.
    static juce::File pluginCacheFile (const juce::File& settingsFile = defaultFile()) { return settingsFile.getSiblingFile ("plugins.xml"); }
    static juce::File scanPedalFile (const juce::File& settingsFile = defaultFile()) { return settingsFile.getSiblingFile ("plugin-scan.pedal"); }
    // The monitor chain (the headphone correction plugins, Session::saveMonitor) is kept beside this file too: it belongs to the headphones.
    static juce::File monitorFile (const juce::File& settingsFile = defaultFile()) { return settingsFile.getSiblingFile ("monitor.xml"); }

    static Settings load (const juce::File& file = defaultFile())
    {
        Settings s;
        if (auto xml = juce::parseXML (file); xml != nullptr && xml->hasTagName ("OlympusSettings"))
        {
            for (auto* f : xml->getChildWithTagNameIterator ("PluginFolder"))
                s.pluginFolders.addIfNotAlreadyThere (f->getStringAttribute ("path"));
            s.lastProject = xml->getStringAttribute ("lastProject");
            s.scanSuiteOnly = xml->getBoolAttribute ("scanSuiteOnly", false);
            s.extraLatencySamples = juce::jlimit (0, 1000000, xml->getIntAttribute ("extraLatency", 0));
            if (auto* device = xml->getChildByName ("AudioDevice"); device != nullptr && device->getFirstChildElement() != nullptr)
                s.audioDeviceState = device->getFirstChildElement()->toString();
        }
        return s;
    }

    void save (const juce::File& file = defaultFile()) const
    {
        juce::XmlElement xml ("OlympusSettings");
        xml.setAttribute ("lastProject", lastProject);
        xml.setAttribute ("scanSuiteOnly", scanSuiteOnly ? 1 : 0);
        xml.setAttribute ("extraLatency", extraLatencySamples);
        for (auto& f : pluginFolders)
            xml.createNewChildElement ("PluginFolder")->setAttribute ("path", f);
        if (auto device = juce::parseXML (audioDeviceState); device != nullptr)
            xml.createNewChildElement ("AudioDevice")->addChildElement (device.release());
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
