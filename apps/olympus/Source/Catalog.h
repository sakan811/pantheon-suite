// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Settings.h"

#include <functional>

// The plugins Olympus can put on a stem: every VST3 bundle the folders hold (and every Audio Unit on macOS), read when the catalog is scanned and
// loaded at run time. Nothing here is linked against a plugin, so the dependency still points one way: Olympus never links a plugin's code.
namespace olympus
{
// The product name of every plugin of plugins/ (the `*_PLUGIN_NAME` of its CMakeLists.txt). A test compares this list with the folders, so a new
// plugin that is not added here fails it. It is also what "suite only" (Settings::scanSuiteOnly) keeps.
inline const juce::StringArray& suiteNames()
{
    static const juce::StringArray names { "Aegis Ceiling", "Apollo Curve", "Atlas Span", "Castor Pair", "Dionysus Warmth", "Harmonia Pitch", "Hephaestus Forge",
                                           "Hera Bond", "Hermes Level", "Hestia Space", "Iris Prism", "Nemesis Hush", "Themis Verdict" };
    return names;
}

// What a scan does. `rescan` tests every bundle again; otherwise a bundle already in the catalog and unchanged is taken as it is, so a scan after
// a new plugin was installed reads only that one. `deadMansPedal` is the file a plugin is written to while it is read: one that crashes in that read
// is blacklisted and skipped by the next scan (the list's blacklist is saved with the catalog). `includeSystem` false looks only in `extra`
// (the tests use it); Audio Units are the system's, so they are scanned only with it.
struct ScanOptions
{
    bool includeSystem = true;
    bool rescan = false;
    bool suiteOnly = false;
    juce::File cacheFile;
    juce::File deadMansPedal = Settings::scanPedalFile();
    std::function<void (const juce::String&)> progress; // told the name of each plugin as it is read
};

class Catalog
{
public:
    static constexpr int kMaxBundleDepth = 5;
    using Factory = std::function<std::unique_ptr<juce::AudioPluginInstance> (double sampleRate, int blockSize)>;

    Catalog()
    {
        formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
#if JUCE_PLUGINHOST_AU && JUCE_MAC
        formats.addFormat (std::make_unique<juce::AudioUnitPluginFormat>());
#endif
    }

    static juce::String candidatePluginName (juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier)
    {
        if (format.getName() == "VST3" || fileOrIdentifier.endsWithIgnoreCase (".vst3"))
            return juce::File (fileOrIdentifier).getFileNameWithoutExtension().trim();
        auto n = format.getNameOfPluginFromIdentifier (fileOrIdentifier).trim();
        if (n.contains (": "))
            n = n.fromFirstOccurrenceOf (": ", false, false).trim();
        return n;
    }

    static bool isTccProtectedDir (const juce::File& dir)
    {
        if (dir == juce::File())
            return false;
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        if (dir == juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
            || dir == juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            || dir == juce::File::getSpecialLocation (juce::File::userMusicDirectory)
            || dir == juce::File::getSpecialLocation (juce::File::userMoviesDirectory)
            || dir == juce::File::getSpecialLocation (juce::File::userPicturesDirectory)
            || dir == home.getChildFile ("Downloads"))
            return true;

        const auto name = dir.getFileName();
        static const char* const kProtectedNames[] = {
            "Desktop", "Documents", "Downloads", "Music", "Pictures", "Movies", "Photos", "Photo Booth Library",
            "Mail", "Messages", "Safari", "Calendars", "Contacts", "Reminders", "Notes", "Mobile Documents",
            "CloudStorage", "Containers", "Group Containers", "Application Support", "Caches", "Saved Application State",
            "Trash", ".Trash"
        };
        for (const char* p : kProtectedNames)
            if (name.equalsIgnoreCase (p))
                return true;
        return false;
    }

    static bool isForbiddenPluginRoot (const juce::File& dir)
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        return dir == juce::File()
            || dir == juce::File ("/")
            || dir == juce::File ("/Volumes")
            || dir == juce::File ("/Users")
            || dir == juce::File ("/System")
            || dir == juce::File ("/Library")
            || dir == juce::File ("/Applications")
            || dir == home
            || dir == home.getChildFile ("Library")
            || dir == home.getChildFile ("Applications")
            || isTccProtectedDir (dir);
    }

    static bool isUnsafePluginRoot (const juce::File& dir)
    {
        return isForbiddenPluginRoot (dir) || ! dir.isDirectory();
    }

    static bool shouldSkipBundleDir (const juce::File& dir)
    {
        const auto name = dir.getFileName();
        return name.startsWithChar ('.')
            || isForbiddenPluginRoot (dir)
            || isTccProtectedDir (dir)
            || name.equalsIgnoreCase ("_deps")
            || name.equalsIgnoreCase ("CMakeFiles")
            || name.equalsIgnoreCase ("node_modules")
            || dir.hasFileExtension ("app;component;framework;dSYM;xcodeproj;xcworkspace;vst;lv2;clap");
    }

    static juce::Array<juce::File> findBundlesIn (const juce::File& dir)
    {
        juce::Array<juce::File> out;
        findBundles (dir, 0, out);
        return out;
    }

    juce::StringArray candidatesForFormat (const juce::String& formatName, const juce::FileSearchPath& extra, bool includeSystem, bool onlySuite = false) const
    {
        for (int f = 0; f < formats.getNumFormats(); ++f)
            if (auto* fmt = formats.getFormat (f); fmt != nullptr && fmt->getName().equalsIgnoreCase (formatName))
                return candidatesFor (*fmt, extra, includeSystem, onlySuite);
        return {};
    }

    // On: only the bundles and plugins named as one of the suite's are read and listed (Settings::scanSuiteOnly).
    bool suiteOnly() const { return suiteOnlyMode; }
    void setSuiteOnly (bool suiteOnly)
    {
        suiteOnlyMode = suiteOnly;
        refresh();
    }

    // Reads the plugins the folders hold: the system's VST3 folders (with `includeSystem`), `extra` and the folders under them (the build folders
    // of plugins/ hold bundles too), and on macOS the Audio Units. A plugin that crashes or hangs in its read is the dead-man's-pedal's to
    // blacklist; a hang is only noticed by the next run, since the read is in this process. Nothing is left loaded.
    void scan (const juce::FileSearchPath& extra, const ScanOptions& options = {})
    {
        options.deadMansPedal.getParentDirectory().createDirectory();
        if (options.cacheFile != juce::File())
            saveCache (options.cacheFile);
        int scannedSinceSave = 0;
        for (int f = 0; f < formats.getNumFormats(); ++f)
        {
            auto& format = *formats.getFormat (f);
            const bool isVst3 = format.getName() == "VST3";
            if (! isVst3 && ! options.includeSystem)
                continue;

            const auto candidates = candidatesFor (format, extra, options.includeSystem, options.suiteOnly);
            // a plugin no longer where it was (or, with a rescan, every one of this format) is taken out before it is read again
            const bool filterSuite = suiteOnlyMode || options.suiteOnly;
            const auto types = list.getTypes();
            for (const auto& t : types)
                if (t.pluginFormatName == format.getName()
                    && (! filterSuite || suiteNames().contains (t.name))
                    && (options.rescan || ! candidates.contains (t.fileOrIdentifier)))
                    list.removeType (t);

            juce::PluginDirectoryScanner scanner (list, format, juce::FileSearchPath(), false, options.deadMansPedal, false);
            scanner.setFilesOrIdentifiersToScan (candidates);
            juce::String name;
            bool more = true;
            while (more)
            {
                name.clear();
                more = scanner.scanNextFile (! options.rescan, name);
                if (name.isNotEmpty() && options.cacheFile != juce::File() && ++scannedSinceSave >= 10)
                {
                    saveCache (options.cacheFile);
                    scannedSinceSave = 0;
                }
                if (options.progress && name.isNotEmpty())
                    options.progress (name);
            }
        }
        refresh();
        if (options.cacheFile != juce::File())
            saveCache (options.cacheFile);
    }

    // The catalog as the last save left it (Settings::pluginCacheFile): what the window starts from, so it does not look again at every start.
    // False when the file is missing or is not a catalog.
    bool loadCache (const juce::File& file)
    {
        auto xml = juce::parseXML (file);
        if (xml == nullptr || ! xml->hasTagName ("KNOWNPLUGINS"))
            return false;
        list.recreateFromXml (*xml);
        refresh();
        return true;
    }

    // Writes the catalog, its blacklist included, in the format loadCache reads.
    bool saveCache (const juce::File& file) const
    {
        auto xml = list.createXml();
        if (xml == nullptr)
            return false;
        file.getParentDirectory().createDirectory();
        juce::TemporaryFile temp (file);
        return xml->writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();
    }

    // A plugin that is made in code, for the tests (it takes the place of a built bundle).
    void addInternal (const juce::PluginDescription& description, Factory factory)
    {
        internals.push_back ({ description, std::move (factory) });
        refresh();
    }

    // The names the plugins are called by, sorted. A name that two different plugins share is given with its maker: "Name (Maker)".
    juce::StringArray names() const
    {
        auto out = labels;
        out.sort (true);
        return out;
    }

    // Every plugin the catalog holds (the made-in-code ones first), with its manufacturer and category, for a UI. names() gives the labels.
    const juce::Array<juce::PluginDescription>& descriptions() const { return all; }

    // The names (as names() gives them) of the suite's own plugins that are here.
    juce::StringArray suiteFound() const
    {
        juce::StringArray out;
        for (int i = 0; i < all.size(); ++i)
            if (suiteNames().contains (all.getReference (i).name))
                out.add (labels[i]);
        out.sort (true);
        return out;
    }

    // True when the plugin `name` means is one of the suite's (its product name is in suiteNames()).
    bool isSuite (const juce::String& name) const
    {
        juce::String error;
        const int at = indexOf (name, error);
        return at >= 0 && suiteNames().contains (all.getReference (at).name);
    }

    // The bundles (and Audio Units) the scan refused to load a second time because they crashed or hung: skipped until the cache is deleted.
    juce::StringArray blacklisted() const { return list.getBlacklistedFiles(); }
    // Forgets the blacklist: the next scan reads those bundles again (PLUGINS > Forget blacklisted plugins).
    void clearBlacklist() { list.clearBlacklistedFiles(); }

    // The name of the plugin `query` means: its whole name as names() gives it, or the start of one, in any case. A name two different plugins
    // share is refused with both of them named. Empty, with the reason in `error`, if there is none or more than one.
    juce::String resolve (const juce::String& query, juce::String& error) const
    {
        const auto q = query.trim();
        for (const auto& label : labels)
            if (label.equalsIgnoreCase (q))
                return label;
        juce::StringArray sameName;
        const int at = byName (q, sameName);
        if (sameName.size() == 1)
            return labels[at];
        if (sameName.size() > 1)
        {
            error = namesakes (q, sameName);
            return {};
        }
        juce::StringArray matches;
        for (const auto& label : labels)
            if (label.startsWithIgnoreCase (q) && q.isNotEmpty())
                matches.add (label);
        if (matches.size() == 1)
            return matches[0];
        error = matches.isEmpty() ? "No plugin called '" + q + "'. Known: " + names().joinIntoString (", ")
                                  : "'" + q + "' could be " + matches.joinIntoString (" or ");
        return {};
    }

    bool has (const juce::String& name) const
    {
        juce::String error;
        return indexOf (name, error) >= 0;
    }

    // A new instance of the plugin `name` means; null, with the reason, when it cannot be had.
    std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& name, double sampleRate, int blockSize, juce::String& error)
    {
        const int at = indexOf (name, error);
        if (at < 0)
            return nullptr;
        if (at < (int) internals.size())
            return internals[(size_t) at].factory (sampleRate, blockSize);
        return formats.createPluginInstance (all.getReference (at), sampleRate, blockSize, error);
    }

private:
    // The index in `all` of the plugin `name` means: its label, or its name when no other plugin has it. Otherwise -1, with the reason.
    int indexOf (const juce::String& name, juce::String& error) const
    {
        const auto q = name.trim();
        for (int i = 0; i < labels.size(); ++i)
            if (labels[i].equalsIgnoreCase (q))
                return i;
        juce::StringArray sameName;
        const int at = byName (q, sameName);
        if (sameName.size() == 1)
            return at;
        error = sameName.isEmpty() ? "The plugin '" + q + "' is not installed (see the plugin folders searched)." : namesakes (q, sameName);
        return -1;
    }

    // The plugins whose name is `q` (in any case): their labels go to `out`; the index of the plugin is returned when there is exactly one.
    int byName (const juce::String& q, juce::StringArray& out) const
    {
        int at = -1;
        for (int i = 0; i < all.size(); ++i)
            if (all.getReference (i).name.equalsIgnoreCase (q))
            {
                out.add (labels[i]);
                at = i;
            }
        return at;
    }

    static juce::String namesakes (const juce::String& q, const juce::StringArray& labelsOfSameName)
    {
        return "'" + q + "' is the name of " + juce::String (labelsOfSameName.size()) + " different plugins: " + labelsOfSameName.joinIntoString (" and ")
               + ". Name the one you mean as it is written in brackets.";
    }

    // The files of `format` to read: the VST3 bundles under the system's folders and `extra`, or the Audio Units the system registers. Only the
    // suite's when suiteOnlyMode or onlySuite is on.
    juce::StringArray candidatesFor (juce::AudioPluginFormat& format, const juce::FileSearchPath& extra, bool includeSystem, bool onlySuite = false) const
    {
        juce::StringArray out;
        if (format.getName() == "VST3")
        {
            juce::FileSearchPath path = includeSystem ? format.getDefaultLocationsToSearch() : juce::FileSearchPath();
            path.addPath (extra);
            juce::Array<juce::File> bundles;
            for (int i = 0; i < path.getNumPaths(); ++i)
                findBundles (path[i], 0, bundles);
            for (const auto& bundle : bundles)
                out.addIfNotAlreadyThere (bundle.getFullPathName());
        }
        else
        {
            out = format.searchPathsForPlugins (juce::FileSearchPath(), false, false);
        }
        const bool filterSuite = suiteOnlyMode || onlySuite;
        const bool isAu = format.getName() == "AudioUnit";
        const auto currentTypes = list.getTypes();
        juce::StringArray kept;
        for (const auto& c : out)
        {
            const auto candidateName = candidatePluginName (format, c);
            if (filterSuite && ! suiteNames().contains (candidateName) && ! suiteNames().contains (format.getNameOfPluginFromIdentifier (c)))
                continue;
            if (isAu && candidateName.isNotEmpty() && hasVst3Named (currentTypes, candidateName))
                continue;
            kept.add (c);
        }
        return kept;
    }

    // Rebuilds `all` (the made-in-code ones first, then the scanned ones) and the labels from the list. Two plugins of one name are both kept, a
    // VST3 over an Audio Unit of the same name, and a second copy of the same plugin (same name, maker and format) is shown once.
    void refresh()
    {
        all.clear();
        labels.clear();
        for (const auto& i : internals)
            all.add (i.description);
        const auto scanned = list.getTypes();
        for (const auto& t : scanned)
        {
            if (suiteOnlyMode && ! suiteNames().contains (t.name))
                continue;
            if (t.pluginFormatName == "AudioUnit" && hasVst3Named (scanned, t.name))
                continue;
            bool copy = false;
            for (int i = (int) internals.size(); i < all.size(); ++i)
                copy = copy || (all.getReference (i).name == t.name && all.getReference (i).manufacturerName == t.manufacturerName
                                && all.getReference (i).pluginFormatName == t.pluginFormatName);
            if (! copy)
                all.add (t);
        }
        for (int i = 0; i < all.size(); ++i)
        {
            const auto& d = all.getReference (i);
            int same = 0;
            for (int j = 0; j < all.size(); ++j)
                same += all.getReference (j).name == d.name ? 1 : 0;
            labels.add (same > 1 ? d.name + " (" + (d.manufacturerName.isNotEmpty() ? d.manufacturerName : d.pluginFormatName) + ")" : d.name);
        }
    }

    static bool hasVst3Named (const juce::Array<juce::PluginDescription>& types, const juce::String& name)
    {
        for (const auto& t : types)
            if (t.pluginFormatName == "VST3" && t.name.equalsIgnoreCase (name))
                return true;
        return false;
    }

    // The .vst3 bundles under `dir`, not looking inside a bundle or a build's own dependency folders.
    static void findBundles (const juce::File& dir, int depth, juce::Array<juce::File>& out)
    {
        if (depth == 0 && (isForbiddenPluginRoot (dir) || ! dir.isDirectory()))
            return;
        for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
        {
            if (f.hasFileExtension ("vst3"))
                out.add (f);
            else if (f.isDirectory() && ! f.isSymbolicLink() && depth < kMaxBundleDepth && ! shouldSkipBundleDir (f))
                findBundles (f, depth + 1, out);
        }
    }

    struct Internal { juce::PluginDescription description; Factory factory; };
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList list;
    bool suiteOnlyMode = false;
    std::vector<Internal> internals;
    juce::Array<juce::PluginDescription> all; // the made-in-code ones, then the scanned ones (see refresh)
    juce::StringArray labels;                 // parallel to all
};
}
