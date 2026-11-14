// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_core/juce_core.h>

namespace orpheus::gui
{
// The files opened or saved last, newest first, kept in a small text file (one path per line) so that the app can offer them again
// and reopen the last one at launch. With no store (tests, the snapshot) it keeps the list in memory only and touches no file.
class RecentFiles
{
public:
    static constexpr int kMax = 8;

    explicit RecentFiles (const juce::File& storeFile = {}) : store (storeFile)
    {
        if (store != juce::File() && store.existsAsFile())
        {
            juce::StringArray lines;
            lines.addLines (store.loadFileAsString());
            for (const auto& line : lines)
                if (line.trim().isNotEmpty())
                    paths.add (line.trim());
            paths.removeDuplicates (false);
            trim();
        }
    }

    // The default place of the list: the application data folder of the user.
    static juce::File defaultStore() { return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Orpheus").getChildFile ("recent.txt"); }

    void add (const juce::File& f)
    {
        if (f == juce::File())
            return;
        paths.removeString (f.getFullPathName());
        paths.insert (0, f.getFullPathName());
        trim();
        save();
    }

    void clear()
    {
        paths.clear();
        save();
    }

    // Only the files that are still there.
    juce::Array<juce::File> files() const
    {
        juce::Array<juce::File> out;
        for (const auto& p : paths)
            if (juce::File (p).existsAsFile())
                out.add (juce::File (p));
        return out;
    }

    juce::File last() const
    {
        const auto all = files();
        return all.isEmpty() ? juce::File() : all.getFirst();
    }

private:
    void trim()
    {
        while (paths.size() > kMax)
            paths.remove (paths.size() - 1);
    }

    void save() const
    {
        if (store == juce::File())
            return;
        store.getParentDirectory().createDirectory();
        store.replaceWithText (paths.joinIntoString ("\n") + "\n");
    }

    juce::File store;
    juce::StringArray paths;
};
} // namespace orpheus::gui
