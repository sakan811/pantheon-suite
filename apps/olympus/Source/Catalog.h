// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

// The plugins Olympus can put on a stem: the suite's own, found as built plugin bundles (VST3) and loaded
// at run time. Nothing here is linked against a plugin, so the dependency still points one way: Olympus knows the plugins' names, never their code.
namespace olympus
{
// The product name of every plugin of plugins/ (the `*_PLUGIN_NAME` of its CMakeLists.txt). A test compares this list with the folders, so a new
// plugin that is not added here fails it.
inline const juce::StringArray& suiteNames()
{
    static const juce::StringArray names { "Aegis Ceiling", "Apollo Curve", "Atlas Span", "Castor Pair", "Dionysus Warmth", "Harmonia Pitch", "Hephaestus Forge",
                                           "Hera Bond", "Hermes Level", "Hestia Space", "Iris Prism", "Nemesis Hush", "Themis Verdict" };
    return names;
}

class Catalog
{
public:
    using Factory = std::function<std::unique_ptr<juce::AudioPluginInstance> (double sampleRate, int blockSize)>;

    Catalog() { formats.addFormat (std::make_unique<juce::VST3PluginFormat>()); }

    // The suite's own VST3 bundles, found by their file name ("Hermes Level.vst3") in the system's VST3 folder, in `extra` and in the folders under it
    // (the build folders of plugins/ hold them too). Only those bundles are opened, so a folder full of other people's plugins costs nothing and none of
    // them is loaded. AU is not hosted: the suite's VST3 builds are what a session uses.
    // `progress` is told the bundle being read.
    void scan (const juce::FileSearchPath& extra, bool includeSystem = true, const std::function<void (const juce::String&)>& progress = {})
    {
        found.clear();
        juce::FileSearchPath path = includeSystem ? formats.getFormat (0)->getDefaultLocationsToSearch() : juce::FileSearchPath();
        path.addPath (extra);
        juce::Array<juce::File> bundles;
        for (int i = 0; i < path.getNumPaths(); ++i)
            findBundles (path[i], 0, bundles);
        for (const auto& bundle : bundles)
        {
            if (! suiteNames().contains (bundle.getFileNameWithoutExtension()) || has (bundle.getFileNameWithoutExtension()))
                continue;
            if (progress)
                progress (bundle.getFileName());
            juce::OwnedArray<juce::PluginDescription> types;
            formats.getFormat (0)->findAllTypesForFile (types, bundle.getFullPathName());
            for (auto* d : types)
                if (suiteNames().contains (d->name) && ! has (d->name))
                    found.add (*d);
        }
    }

    // A plugin that is made in code, for the tests (it takes the place of a built bundle).
    void addInternal (const juce::PluginDescription& description, Factory factory) { internals.push_back ({ description, std::move (factory) }); }

    juce::StringArray names() const
    {
        juce::StringArray out;
        for (const auto& i : internals) out.addIfNotAlreadyThere (i.description.name);
        for (const auto& d : found) out.addIfNotAlreadyThere (d.name);
        out.sort (true);
        return out;
    }

    // The exact name of the plugin `query` means: its whole name, or the start of it ("hermes"), in any case. Empty, with the reason in `error`, if there is
    // none or more than one.
    juce::String resolve (const juce::String& query, juce::String& error) const
    {
        const auto all = names();
        const auto q = query.trim();
        for (const auto& n : all)
            if (n.equalsIgnoreCase (q))
                return n;
        juce::StringArray matches;
        for (const auto& n : all)
            if (n.startsWithIgnoreCase (q) && q.isNotEmpty())
                matches.add (n);
        if (matches.size() == 1)
            return matches[0];
        error = matches.isEmpty() ? "No plugin called '" + q + "'. Known: " + all.joinIntoString (", ")
                                  : "'" + q + "' could be " + matches.joinIntoString (" or ");
        return {};
    }

    bool has (const juce::String& name) const
    {
        for (const auto& i : internals)
            if (i.description.name == name) return true;
        for (const auto& d : found)
            if (d.name == name) return true;
        return false;
    }

    // A new instance of the plugin with that exact name; null, with the reason, when it cannot be had.
    std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& name, double sampleRate, int blockSize, juce::String& error)
    {
        for (auto& i : internals)
            if (i.description.name == name)
                return i.factory (sampleRate, blockSize);
        for (const auto& d : found)
            if (d.name == name)
                return formats.createPluginInstance (d, sampleRate, blockSize, error);
        error = "The plugin '" + name + "' is not installed (see the plugin folders searched).";
        return nullptr;
    }

private:
    // The .vst3 bundles under `dir`, not looking inside a bundle or a build's own dependency folders.
    static void findBundles (const juce::File& dir, int depth, juce::Array<juce::File>& out)
    {
        for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
        {
            if (f.hasFileExtension ("vst3"))
                out.add (f);
            else if (f.isDirectory() && ! f.isSymbolicLink() && depth < 9 && f.getFileName() != "_deps" && f.getFileName() != "CMakeFiles")
                findBundles (f, depth + 1, out);
        }
    }

    struct Internal { juce::PluginDescription description; Factory factory; };
    juce::AudioPluginFormatManager formats;
    juce::Array<juce::PluginDescription> found;
    std::vector<Internal> internals;
};
}
