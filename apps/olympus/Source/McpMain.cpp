// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// OlympusMcp [--plugins <folder>] [--project <file.olympus>]
//
// The session without a window, speaking MCP (JSON-RPC, one message per line) on stdin and stdout. Anything else it says goes to stderr.
// Plugins are looked for in the system's VST3 folders (and Audio Units on macOS), in `--plugins` (repeatable), in the folders of OLYMPUS_PLUGIN_PATH
// and in the folders chosen in the window (PLUGINS). The last scan is loaded from the settings folder; the folders are read again for what is new.

#include "Mcp.h"
#include "Settings.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    olympus::Session session;
    const auto settings = olympus::Settings::load();
    juce::FileSearchPath extra = settings.searchPath(); // the folders chosen in the window, and OLYMPUS_PLUGIN_PATH
    juce::String project;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--plugins" && i + 1 < argc) extra.add (juce::File (argv[++i]));
        else if (arg == "--project" && i + 1 < argc) project = argv[++i];
        else { std::fprintf (stderr, "usage: OlympusMcp [--plugins <folder>] [--project <file.olympus>]\n"); return 2; }
    }
    session.catalog.setSuiteOnly (settings.scanSuiteOnly);
    const auto cache = olympus::Settings::pluginCacheFile();
    session.catalog.loadCache (cache);
    session.catalog.scan (extra);
    session.catalog.saveCache (cache);
    std::fprintf (stderr, "OlympusMcp: %d plugins found\n", session.catalog.names().size());
    if (project.isNotEmpty())
    {
        juce::String error;
        if (! session.load (juce::File (project), error))
        {
            std::fprintf (stderr, "OlympusMcp: %s\n", error.toRawUTF8());
            return 1;
        }
    }
    olympus::McpServer server (session);
    server.run (std::cin, std::cout);
    return 0;
}
