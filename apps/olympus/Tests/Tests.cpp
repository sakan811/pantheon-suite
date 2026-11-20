// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless checks of the session, the render, the project file and the MCP server, with plugins made in code (TestPlugin.h).
// Set OLYMPUS_PLUGIN_DIR to a folder with built plugin bundles to add the check against the real ones.

#include "Analyze.h"
#include "Check.h"
#include "Edit.h"
#include "Mcp.h"
#include "Record.h"
#include "Settings.h"
#include "TempoTests.h"
#include "TestPlugin.h"

#include <chrono>
#include <limits>
#include <sstream>
#include <thread>

using namespace olympus;

namespace test
{
inline void check (bool ok, const char* what) { check (ok, std::string (what)); }
inline void check (bool ok, const juce::String& what) { check (ok, what.toStdString()); }
}

namespace
{
juce::File tempFolder()
{
    static auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("olympus-tests-" + juce::String (juce::Time::currentTimeMillis()));
    folder.createDirectory();
    return folder;
}

// A WAV of `channels` channels at `rate`: a sine (amplitude `level`, `freq`), or, with freq 0, one impulse of 0.5 at sample `impulseAt`.
juce::File makeWav (const juce::String& name, double rate, int channels, int length, float level, double freq, int impulseAt = 100)
{
    juce::AudioBuffer<float> b (channels, length);
    b.clear();
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < length; ++i)
            b.setSample (ch, i, freq > 0.0 ? level * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / rate) : (i == impulseAt ? level : 0.0f));
    auto file = tempFolder().getChildFile (name);
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (rate).withNumChannels (channels).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (b, 0, length);
    return file;
}

juce::AudioBuffer<float> readWav (const juce::File& file, double* rate = nullptr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    juce::AudioBuffer<float> b (r != nullptr ? (int) r->numChannels : 1, r != nullptr ? (int) r->lengthInSamples : 0);
    if (r != nullptr) { r->read (&b, 0, b.getNumSamples(), 0, true, true); if (rate != nullptr) *rate = r->sampleRate; }
    return b;
}

int peakIndex (const juce::AudioBuffer<float>& b, int ch = 0)
{
    int at = -1;
    float best = 0.0f;
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs (b.getSample (ch, i)) > best) { best = std::abs (b.getSample (ch, i)); at = i; }
    return at;
}

void setGain (Session& s, int track, int slot, double db)
{
    auto* inst = s.tracks[(size_t) track]->chain[(size_t) slot]->instance.get();
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (inst->getParameters()[0]);
    p->setValue (p->convertTo0to1 ((float) db));
}
double gainOf (Session& s, int track, int slot)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (s.tracks[(size_t) track]->chain[(size_t) slot]->instance->getParameters()[0]);
    return (double) p->convertFrom0to1 (p->getValue());
}

void catalogChecks()
{
    std::printf ("[1] Catalog\n");
    Session s;
    test::addTestPlugins (s.catalog);
    juce::String err;
    test::check (s.catalog.resolve ("test gain", err) == "Test Gain", "a name in any case");
    test::check (s.catalog.resolve ("test g", err) == "Test Gain", "the start of a name");
    const auto ambiguous = s.catalog.resolve ("test", err);
    test::check (ambiguous.isEmpty() && err.contains ("could be"), "ambiguous: refused, both named (" + err + ")");
    test::check (s.catalog.resolve ("hermes", err).isEmpty() && err.contains ("No plugin"), "unknown: refused, the known ones listed");
    test::check (s.catalog.names().size() == 2, "names lists what is known");
}

// A plugin as a scan would describe it (a hand-built description, so no plugin is needed).
juce::PluginDescription described (const juce::String& name, const juce::String& maker, const juce::String& format, const juce::String& file)
{
    juce::PluginDescription d;
    d.name = name;
    d.manufacturerName = maker;
    d.pluginFormatName = format;
    d.fileOrIdentifier = file;
    d.category = "Effect";
    d.version = "1.0";
    d.uniqueId = name.hashCode() ^ maker.hashCode();
    d.numInputChannels = d.numOutputChannels = 2;
    return d;
}

void catalogCacheChecks()
{
    std::printf ("[1b] Catalog: the cache, the suite only, two plugins of one name\n");
    const auto dir = tempFolder().getChildFile ("catalog-cache");
    dir.deleteRecursively();
    dir.createDirectory();
    // what a scan leaves in the list: two plugins of one name from two makers, a VST3 and an AU of one name, the suite's own, and a blacklisted bundle
    juce::KnownPluginList scanned;
    scanned.addType (described ("Dup Plugin", "Acme", "VST3", "/x/Acme Dup.vst3"));
    scanned.addType (described ("Dup Plugin", "Beta", "VST3", "/x/Beta Dup.vst3"));
    scanned.addType (described ("Shared Name", "Acme", "VST3", "/x/Shared.vst3"));
    scanned.addType (described ("Shared Name", "Acme", "AudioUnit", "AudioUnit:Shared"));
    scanned.addType (described ("Hermes Level", "Pantheon", "VST3", "/x/Hermes Level.vst3"));
    scanned.addToBlacklist ("/x/Crashed.vst3");
    const auto first = dir.getChildFile ("plugins.xml");
    test::check (scanned.createXml()->writeTo (first), "a scan's list is written as the cache");

    Catalog a;
    test::check (a.loadCache (first), "the cache is read");
    test::check (a.names().size() == 4 && a.names().contains ("Dup Plugin (Acme)") && a.names().contains ("Dup Plugin (Beta)"), "two plugins of one name are both listed, each with its maker");
    int sharedCount = 0;
    bool sharedIsVst3 = true;
    for (const auto& d : a.descriptions())
        if (d.name == "Shared Name") { ++sharedCount; sharedIsVst3 = d.pluginFormatName == "VST3"; }
    test::check (sharedCount == 1 && sharedIsVst3, "a VST3 and an AU of one name: the VST3 is kept");
    juce::String err;
    test::check (a.resolve ("Dup Plugin", err).isEmpty() && err.contains ("Acme") && err.contains ("Beta"), "a name two plugins share is refused, both named (" + err + ")");
    test::check (a.resolve ("Dup Plugin (Beta)", err) == "Dup Plugin (Beta)", "and the one written with its maker is found");
    test::check (a.resolve ("hermes", err) == "Hermes Level", "the start of a name still finds a plugin");
    test::check (a.create ("Dup Plugin", 48000.0, 512, err) == nullptr && err.contains ("Acme"), "create refuses the shared name the same way");
    test::check (a.isSuite ("Hermes Level") && ! a.isSuite ("Dup Plugin (Acme)") && ! a.isSuite ("Shared Name"), "isSuite: the suite's own name, and not the others");
    test::check (a.suiteFound().size() == 1 && a.suiteFound()[0] == "Hermes Level", "suiteFound lists the suite's plugins that are here");
    test::check (a.blacklisted().contains ("/x/Crashed.vst3"), "a blacklisted bundle is reported");

    Catalog made;
    made.addInternal (test::GainPlugin::description ("Test Gain"), [] (double, int) -> std::unique_ptr<juce::AudioPluginInstance> { return nullptr; });
    made.addInternal (described ("Aegis Ceiling", "Pantheon", "Internal", "Aegis Ceiling"), [] (double, int) -> std::unique_ptr<juce::AudioPluginInstance> { return nullptr; });
    test::check (made.isSuite ("Aegis Ceiling") && ! made.isSuite ("Test Gain"), "isSuite holds for a made-in-code plugin with a suite name too");

    const auto second = dir.getChildFile ("saved.xml");
    test::check (a.saveCache (second), "the catalog is saved");
    Catalog b;
    test::check (b.loadCache (second) && b.names() == a.names() && b.blacklisted() == a.blacklisted() && b.descriptions().size() == a.descriptions().size(),
                 "and read back: the same plugins, makers and blacklist");

    b.setSuiteOnly (true);
    test::check (b.names().size() == 1 && b.names()[0] == "Hermes Level", "suite only: just the suite's own are listed");
    b.setSuiteOnly (false);
    test::check (b.names() == a.names(), "and all of them again when it is off");

    const auto junk = dir.getChildFile ("junk.xml");
    junk.replaceWithText ("not a catalog");
    Catalog c;
    test::check (! c.loadCache (junk) && ! c.loadCache (dir.getChildFile ("missing.xml")) && c.names().isEmpty(), "a file that is not a catalog, or is missing, is refused");

    Settings settings;
    settings.scanSuiteOnly = true;
    settings.pluginFolders.add ("/x");
    const auto settingsFile = dir.getChildFile ("settings.xml");
    settings.save (settingsFile);
    test::check (Settings::load (settingsFile).scanSuiteOnly && Settings::load (settingsFile).pluginFolders.size() == 1, "the suite-only flag is kept in the settings");
    test::check (! Settings::load (dir.getChildFile ("none.xml")).scanSuiteOnly, "and it is off by default");
    test::check (Settings::pluginCacheFile (settingsFile).getParentDirectory() == dir && Settings::scanPedalFile (settingsFile).getParentDirectory() == dir,
                 "the cache and the scan's pedal sit beside the settings");
}

void suiteNamesMatchPlugins()
{
    std::printf ("[2] The suite's names are the plugins' names\n");
    const juce::File root (PANTHEON_REPO_ROOT);
    int plugins = 0;
    bool allListed = true;
    juce::StringArray missing;
    for (auto& dir : root.getChildFile ("plugins").findChildFiles (juce::File::findDirectories, false))
    {
        const auto cmake = dir.getChildFile ("CMakeLists.txt").loadFileAsString();
        for (auto& line : juce::StringArray::fromLines (cmake))
        {
            const auto t = line.trim();
            if (t.startsWith ("set(") && t.contains ("_PLUGIN_NAME \""))
            {
                ++plugins;
                const auto name = t.fromFirstOccurrenceOf ("\"", false, false).upToFirstOccurrenceOf ("\"", false, false);
                if (! suiteNames().contains (name)) { allListed = false; missing.add (name); }
            }
        }
    }
    test::check (plugins == suiteNames().size(), test::fmt ("%d plugin folders, %d names in suiteNames()", plugins, suiteNames().size()));
    test::check (allListed, "every plugin's product name is in suiteNames() " + missing.joinIntoString (", "));
}

void trackChecks()
{
    std::printf ("[3] Tracks: mono and other rates become stereo at the session's rate\n");
    Session s;
    juce::String err;
    test::check (s.addTrack (makeWav ("a.wav", 44100.0, 1, 44100, 0.5f, 440.0), "A", err), "a mono 44.1 kHz file is added");
    test::check (std::abs (s.sampleRate - 44100.0) < 1e-9, "the first track sets the session's rate");
    const auto& first = *s.tracks[0]->clips[0].audio;
    test::check (first.getNumChannels() == 2 && juce::approximatelyEqual (first.getSample (1, 1000), first.getSample (0, 1000)), "mono is copied to both channels");
    test::check (s.addTrack (makeWav ("b.wav", 48000.0, 2, 48000, 0.5f, 440.0), "B", err), "a 48 kHz file is added");
    test::check (std::abs (s.tracks[1]->length() - 44100) <= 2, test::fmt ("and resampled: %d samples (one second at 44.1 kHz)", (int) s.tracks[1]->length()));
    test::check (! s.addTrack (tempFolder().getChildFile ("missing.wav"), "", err) && err.isNotEmpty(), "a file that is not there is refused with a reason");
    test::check (s.findTrack ("b", err) == 1 && s.findTrack (0, err) == 0 && s.findTrack (5, err) < 0, "tracks are found by name and by number");
}

void renderChecks()
{
    std::printf ("[4] Stems: chain, fader, alignment, mute, tail\n");
    Session s;
    test::addTestPlugins (s.catalog);
    juce::String err;
    s.addTrack (makeWav ("sine.wav", 48000.0, 2, 48000, 0.5f, 1000.0), "Sine", err);
    s.addTrack (makeWav ("click.wav", 48000.0, 2, 48000, 0.5f, 0.0, 1000), "Click", err);
    test::check (s.addPlugin (0, "test gain", -1, err), "a plugin goes on a track");
    setGain (s, 0, 0, -6.0);
    s.tracks[0]->gainDb = -2.0f;

    RenderOptions o;
    o.folder = tempFolder().getChildFile ("stems1");
    RenderResult r;
    test::check (s.renderStems (o, r, err), "render works: " + err);
    test::check (r.stems.size() == 2 && r.mixFile.isNotEmpty(), "one stem per track and a mix");
    const auto stem = readWav (o.folder.getChildFile ("01-Sine.wav"));
    test::check (stem.getNumSamples() == 48000, "a stem is as long as the audio");
    const float expected = 0.5f * juce::Decibels::decibelsToGain (-8.0f);
    test::check (std::abs (stem.getMagnitude (0, 0, 48000) - expected) < 0.002f, test::fmt ("the chain (-6 dB) and the fader (-2 dB) are in the stem: peak %.4f, expected %.4f", stem.getMagnitude (0, 0, 48000), expected));
    const auto click = readWav (o.folder.getChildFile ("02-Click.wav"));
    test::check (peakIndex (click) == 1000, "a track with no plugin is passed through, in place");
    const auto mix = readWav (o.folder.getChildFile ("mix.wav"));
    test::check (std::abs (mix.getSample (0, 5000) - (stem.getSample (0, 5000) + click.getSample (0, 5000))) < 1e-5f && std::abs (mix.getSample (0, 1000) - (stem.getSample (0, 1000) + click.getSample (0, 1000))) < 1e-5f, "the mix is the sum of the stems");

    // a latency is taken off
    test::check (s.addPlugin (1, "test delay", -1, err), "a plugin that reports latency");
    test::check (s.tracks[1]->latency() == 64, "its latency is the chain's");
    RenderOptions o2;
    o2.folder = tempFolder().getChildFile ("stems2");
    RenderResult r2;
    test::check (s.renderStems (o2, r2, err), "render with a latency: " + err);
    const auto aligned = readWav (o2.folder.getChildFile ("02-Click.wav"));
    test::check (aligned.getNumSamples() == 48000 && peakIndex (aligned) == 1000, test::fmt ("the stem is aligned with the audio (impulse at %d, was at 1000)", peakIndex (aligned)));
    const auto sineAgain = readWav (o2.folder.getChildFile ("01-Sine.wav"));
    test::check (std::abs (sineAgain.getSample (0, 20000) - stem.getSample (0, 20000)) < 1e-6f, "a stem does not change because another track has latency");

    // a float stem can go over full scale
    setGain (s, 0, 0, 24.0);
    RenderOptions o3;
    o3.folder = tempFolder().getChildFile ("stems3");
    o3.tailSeconds = 0.5;
    o3.writeMix = false;
    RenderResult r3;
    test::check (s.renderStems (o3, r3, err), "render with a tail: " + err);
    test::check (r3.stems[0].peakDb > 0.0, test::fmt ("a 32-bit float stem keeps a peak over full scale (%.1f dBFS)", r3.stems[0].peakDb));
    test::check (readWav (o3.folder.getChildFile ("01-Sine.wav")).getNumSamples() == 48000 + 24000 && ! o3.folder.getChildFile ("mix.wav").exists(), "the tail is added, and no mix if not asked");

    // mute and solo
    s.tracks[0]->mute = true;
    RenderOptions o4;
    o4.folder = tempFolder().getChildFile ("stems4");
    RenderResult r4;
    test::check (s.renderStems (o4, r4, err) && r4.stems.size() == 1 && ! o4.folder.getChildFile ("01-Sine.wav").exists(), "a muted track writes no stem");
    s.tracks[0]->mute = false;
    s.tracks[0]->solo = true;
    RenderOptions o5;
    o5.folder = tempFolder().getChildFile ("stems5");
    RenderResult r5;
    test::check (s.renderStems (o5, r5, err) && r5.stems.size() == 1 && r5.stems[0].track == "Sine", "with a solo, only the soloed track is written");
}

void realtimeChecks()
{
    std::printf ("[5] Real time: tracks stay aligned when plugins have latency\n");
    Session s;
    test::addTestPlugins (s.catalog);
    juce::String err;
    s.addTrack (makeWav ("c1.wav", 48000.0, 2, 4096, 0.5f, 0.0, 300), "Dry", err);
    s.addTrack (makeWav ("c2.wav", 48000.0, 2, 4096, 0.5f, 0.0, 300), "Late", err);
    s.addPlugin (1, "test delay", -1, err);
    s.prepare (false);
    juce::AudioBuffer<float> all (2, 4096), block (2, kBlockSize);
    for (int start = 0; start < 4096; start += kBlockSize)
    {
        block.setSize (2, kBlockSize, false, false, true);
        s.processBlock (block, start);
        for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, start, block, ch, 0, kBlockSize);
    }
    int count = 0, first = -1;
    for (int i = 0; i < 4096; ++i)
        if (std::abs (all.getSample (0, i)) > 0.01f) { ++count; if (first < 0) first = i; }
    test::check (count == 1 && first == 300 + 64, test::fmt ("both clicks arrive together, %d samples late (that is the longest latency); %d event(s) at %d", 64, count, first));
}

void projectChecks()
{
    std::printf ("[6] Project file: settings, relative paths, missing plugin, missing audio\n");
    juce::String err;
    const auto dir = tempFolder().getChildFile ("proj");
    dir.createDirectory();
    const auto audio = makeWav ("proj/vox.wav", 48000.0, 2, 24000, 0.5f, 220.0);
    {
        Session s;
        test::addTestPlugins (s.catalog);
        s.addTrack (audio, "Vox", err);
        s.addPlugin (0, "test gain", -1, err);
        s.addPlugin (0, "test delay", -1, err);
        setGain (s, 0, 0, -9.0);
        s.tracks[0]->gainDb = -3.0f;
        s.tracks[0]->solo = true;
        s.tracks[0]->chain[1]->bypass = true;
        test::check (s.save (dir.getChildFile ("song.olympus"), err), "saves");
        test::check (! s.dirty && s.name == "song", "saving clears the unsaved mark");
    }
    {
        Session s;
        test::addTestPlugins (s.catalog);
        test::check (s.load (dir.getChildFile ("song.olympus"), err), "loads: " + err);
        test::check (s.tracks.size() == 1 && s.tracks[0]->name == "Vox" && s.tracks[0]->chain.size() == 2, "the track and its chain are back");
        test::check (std::abs (gainOf (s, 0, 0) + 9.0) < 0.01, test::fmt ("a plugin's settings are back (%.2f dB)", gainOf (s, 0, 0)));
        test::check (std::abs (s.tracks[0]->gainDb + 3.0f) < 1e-6f && s.tracks[0]->solo && s.tracks[0]->chain[1]->bypass, "fader, solo and bypass are back");
        test::check (s.warnings.isEmpty(), "nothing to warn about");
    }
    // the folder is moved: the relative path still finds the audio
    const auto moved = tempFolder().getChildFile ("proj-moved");
    dir.copyDirectoryTo (moved);
    dir.deleteRecursively();
    {
        Session s;
        test::addTestPlugins (s.catalog);
        test::check (s.load (moved.getChildFile ("song.olympus"), err) && s.tracks[0]->length() == 24000, "the project and its audio can be moved together");
    }
    // a machine without the plugin
    {
        Session s; // no plugins known
        test::check (s.load (moved.getChildFile ("song.olympus"), err), "a project opens without its plugins");
        test::check (s.warnings.size() == 2 && s.tracks[0]->chain.size() == 2 && s.tracks[0]->chain[0]->instance == nullptr, "each missing plugin is a warning and stays in the chain");
        test::check (s.save (moved.getChildFile ("again.olympus"), err), "and it saves again");
        Session t;
        test::addTestPlugins (t.catalog);
        t.load (moved.getChildFile ("again.olympus"), err);
        test::check (std::abs (gainOf (t, 0, 0) + 9.0) < 0.01, "the missing plugin's settings were kept through the save");
    }
    // the audio is gone
    moved.getChildFile ("vox.wav").deleteFile();
    {
        Session s;
        test::addTestPlugins (s.catalog);
        test::check (s.load (moved.getChildFile ("song.olympus"), err) && s.tracks.size() == 1 && s.tracks[0]->length() == 24000 && s.tracks[0]->clips[0].audio == nullptr && s.warnings.size() == 1,
                 "missing audio: the track and its clip stay, silent, with a warning");
    }
    {
        Session s;
        moved.getChildFile ("bad.olympus").replaceWithText ("<nonsense/>");
        test::check (! s.load (moved.getChildFile ("bad.olympus"), err) && err.isNotEmpty(), "a file that is not a project is refused");
    }
}

void chainChecks()
{
    std::printf ("[7] Chain editing\n");
    Session s;
    test::addTestPlugins (s.catalog);
    juce::String err;
    for (auto n : { "a", "b", "c" })
        s.addTrack (makeWav (juce::String (n) + ".wav", 48000.0, 2, 4800, 0.5f, 100.0), n, err);
    test::check (s.setChain (0, { "test gain", "test delay" }, err) && s.tracks[0]->chain.size() == 2, "set_chain replaces a chain");
    test::check (! s.setChain (1, { "test gain", "nope" }, err) && s.tracks[1]->chain.empty(), "a wrong name changes nothing");
    setGain (s, 0, 0, -4.0);
    test::check (s.copyChain (0, { 1, 2 }, err) && s.tracks[2]->chain.size() == 2 && std::abs (gainOf (s, 2, 0) + 4.0) < 0.01, "copy_chain copies plugins and their settings");
    s.movePlugin (0, 0, 1, err);
    test::check (s.tracks[0]->chain[0]->plugin == "Test Delay", "a plugin moves");
    s.removePlugin (0, 0, err);
    test::check (s.tracks[0]->chain.size() == 1 && ! s.removePlugin (0, 9, err), "a plugin is removed; a place that is not there is refused");
    s.setBypass (1, 1, true, err);
    test::check (s.tracks[1]->latency() == 0, "a bypassed plugin adds no latency");
}

void mcpChecks()
{
    std::printf ("[8] MCP server\n");
    Session s;
    test::addTestPlugins (s.catalog);
    McpServer server (s);
    int id = 0;
    const auto call = [&] (const juce::String& tool, const juce::String& argsJson) -> juce::var
    {
        juce::var req;
        juce::JSON::parse ("{\"jsonrpc\":\"2.0\",\"id\":" + juce::String (++id) + ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool + "\",\"arguments\":" + argsJson + "}}", req);
        return server.handle (req)["result"];
    };
    const auto text = [] (const juce::var& r) { return r["content"][0]["text"].toString(); };
    const auto parsed = [&] (const juce::var& r) { juce::var v; juce::JSON::parse (text (r), v); return v; };

    juce::var init;
    juce::JSON::parse ("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\"}}", init);
    const auto ir = server.handle (init)["result"];
    test::check (ir["serverInfo"]["name"].toString() == "olympus" && ir["protocolVersion"].toString() == "2025-03-26" && ir["capabilities"].hasProperty ("tools"), "initialize answers with the tools capability");
    juce::var list;
    juce::JSON::parse ("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", list);
    const auto tools = server.handle (list)["result"]["tools"];
    test::check (tools.size() == 37 && tools[0]["inputSchema"]["type"].toString() == "object", test::fmt ("tools/list: %d tools, each with a schema", (int) tools.size()));

    juce::var note;
    juce::JSON::parse ("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", note);
    test::check (server.handle (note).isVoid(), "a notification gets no reply");
    juce::var unknown;
    juce::JSON::parse ("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"nope\"}", unknown);
    test::check ((int) server.handle (unknown)["error"]["code"] == -32601, "an unknown method is an error");

    test::check (parsed (call ("list_plugins", "{}"))["installed"].size() == 2, "list_plugins");
    const auto wav = makeWav ("mcp.wav", 48000.0, 2, 48000, 0.5f, 500.0).getFullPathName().replace ("\\", "\\\\");
    const auto added = call ("add_track", "{\"file\":\"" + wav + "\",\"name\":\"Pad\"}");
    test::check (! (bool) added["isError"] && parsed (added)["tracks"].size() == 1, "add_track");
    test::check (! (bool) call ("set_chain", "{\"track\":\"pad\",\"plugins\":[\"test gain\"]}")["isError"], "set_chain, the track by name");
    const auto params = parsed (call ("list_parameters", "{\"track\":0,\"slot\":\"Test Gain\"}"));
    test::check (params["parameters"].size() == 1 && params["parameters"][0]["id"].toString() == "gain", "list_parameters");
    const auto set = parsed (call ("set_parameter", "{\"track\":0,\"slot\":0,\"parameter\":\"gain\",\"text\":\"-6\"}"));
    test::check (std::abs (gainOf (s, 0, 0) + 6.0) < 0.2, test::fmt ("set_parameter by text: %.2f dB, the plugin shows '%s'", gainOf (s, 0, 0), set["text"].toString().toRawUTF8()));
    call ("set_parameter", "{\"track\":0,\"slot\":0,\"parameter\":\"Gain\",\"value\":0.5}");
    test::check (std::abs (gainOf (s, 0, 0)) < 0.01, "set_parameter by value, the setting by name");
    test::check (parsed (call ("list_presets", "{\"track\":0,\"slot\":0}"))["presets"].size() == 2, "list_presets");
    call ("load_preset", "{\"track\":0,\"slot\":0,\"preset\":\"boost\"}");
    test::check (std::abs (gainOf (s, 0, 0) - 6.0) < 0.01, "load_preset by name");
    {
        const auto st = parsed (call ("get_plugin_state", "{\"track\":0,\"slot\":0}"));
        const auto stateText = st["state"].toString();
        test::check (stateText.isNotEmpty() && st["plugin"].toString().isNotEmpty(), "get_plugin_state returns base64 text");
        call ("set_parameter", "{\"track\":0,\"slot\":0,\"parameter\":\"Gain\",\"value\":0.5}");
        call ("set_plugin_state", "{\"track\":0,\"slot\":0,\"state\":\"" + stateText + "\"}");
        test::check (std::abs (gainOf (s, 0, 0) - 6.0) < 0.01, "set_plugin_state restores the saved settings");
        test::check ((bool) call ("set_plugin_state", "{\"track\":0,\"slot\":0,\"state\":\"\"}")["isError"], "set_plugin_state with no state is an error");
        test::check ((bool) call ("set_plugin_state", "{\"track\":0,\"slot\":0,\"state\":\"!!not base64!!\"}")["isError"], "set_plugin_state with bad text is an error");
        const auto notes = parsed (call ("plugin_notes", "{}"));
        test::check (notes["guide"].toString().isNotEmpty() && notes["notes"].isArray(), "plugin_notes gives the guide and a list of notes");
    }
    call ("set_track", "{\"track\":0,\"gain_db\":-1.5,\"name\":\"Pad 1\"}");
    test::check (std::abs (s.tracks[0]->gainDb + 1.5f) < 1e-6f && s.tracks[0]->name == "Pad 1", "set_track");

    const auto saved = tempFolder().getChildFile ("mcp.olympus").getFullPathName().replace ("\\", "\\\\");
    test::check (! (bool) call ("save_project", "{\"path\":\"" + saved + "\"}")["isError"] && juce::File (saved).existsAsFile(), "save_project");
    const auto stems = tempFolder().getChildFile ("mcp-stems").getFullPathName().replace ("\\", "\\\\");
    const auto rendered = parsed (call ("render_stems", "{\"folder\":\"" + stems + "\"}"));
    test::check (rendered["stems"].size() == 1 && juce::File (rendered["stems"][0]["file"].toString()).existsAsFile() && juce::File (rendered["mix"].toString()).existsAsFile(), "render_stems writes the stem and the mix");

    const auto bad = call ("add_plugin", "{\"track\":0,\"plugin\":\"nothing\"}");
    test::check ((bool) bad["isError"] && text (bad).contains ("No plugin called"), "a tool that fails says why, as an error result: " + text (bad));
    test::check ((bool) call ("set_chain", "{\"track\":7,\"plugins\":[]}")["isError"], "a track that is not there is an error result");
    test::check ((bool) call ("open_project", "{\"path\":\"/nowhere.olympus\"}")["isError"], "a project that is not there is an error result");

    std::istringstream in ("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\nnot json\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    std::ostringstream out;
    server.run (in, out);
    const auto lines = juce::StringArray::fromLines (out.str());
    test::check (lines.size() >= 2 && (int) juce::JSON::parse (lines[0])["id"] == 9 && (int) juce::JSON::parse (lines[1])["error"]["code"] == -32700 && ! out.str().empty() && out.str().back() == '\n', "stdio: one line per answer, a parse error answered, a notification not");
    test::check (juce::JSON::toString (juce::JSON::parse (lines[0]), true).indexOfChar ('\n') < 0, "an answer is a single line");
}

// A tools/call of `server`, as a client makes it: the result (content and isError).
juce::var toolCall (McpServer& server, const juce::String& tool, const juce::String& argsJson)
{
    static int id = 1000;
    juce::var req;
    juce::JSON::parse ("{\"jsonrpc\":\"2.0\",\"id\":" + juce::String (++id) + ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool + "\",\"arguments\":" + argsJson + "}}", req);
    return server.handle (req)["result"];
}
juce::String toolText (const juce::var& result) { return result["content"][0]["text"].toString(); }
juce::var toolJson (const juce::var& result) { juce::var v; juce::JSON::parse (toolText (result), v); return v; }

// [40] The MCP server's undo and redo: the same history as the window's (Editor). An edit is undone and redone by name; an empty history is an
// error result with the reason; adding a plugin clears the history, as the window does.
void mcpUndoChecks()
{
    std::printf ("[40] MCP undo and redo: an edit is undone and redone through the Editor; an empty history is an error result; a plugin change clears it\n");
    tempFolder().getChildFile ("mcp-undo").createDirectory();
    juce::String err;
    const auto wav = makeWav ("mcp-undo/a.wav", 48000.0, 2, 48000, 0.5f, 440.0);
    const auto path = wav.getFullPathName().replace ("\\", "\\\\");
    Session s;
    test::addTestPlugins (s.catalog);
    s.addTrack (wav, "A", err);
    McpServer server (s);
    const auto call = [&server] (const char* tool, const juce::String& args) { return toolCall (server, tool, args); };
    const auto failed = [] (const juce::var& r) { return (bool) r["isError"]; };
    const auto said = [] (const juce::var& r, const char* words) { return toolText (r).contains (words); };

    test::check (failed (call ("undo", "{}")) && said (call ("undo", "{}"), "Nothing to undo"), "undo with an empty history is an error result with the reason");
    test::check (! failed (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_seconds\":1}")) && s.tracks[0]->clips.size() == 2, "add_clip: one clip more");
    const auto undone = call ("undo", "{}");
    test::check (! failed (undone) && s.tracks[0]->clips.size() == 1 && toolJson (undone)["undone"].toString() == "Add clip",
                 "undo takes the clip off again and names the edit it undid");
    test::check ((bool) toolJson (undone)["can_redo"] && ! (bool) toolJson (undone)["can_undo"], "after the undo, redo is possible and undo is not");
    const auto redone = call ("redo", "{}");
    test::check (! failed (redone) && s.tracks[0]->clips.size() == 2 && toolJson (redone)["redone"].toString() == "Add clip", "redo brings the clip back and names it");
    test::check (failed (call ("redo", "{}")) && said (call ("redo", "{}"), "Nothing to redo"), "redo with nothing undone is an error result with the reason");

    test::check (! failed (call ("add_plugin", "{\"track\":0,\"plugin\":\"Test Gain\"}")), "add_plugin puts a plugin on the track");
    test::check (failed (call ("undo", "{}")) && said (call ("undo", "{}"), "Nothing to undo"), "adding a plugin clears the history: the clip is not undone");
    tempFolder().getChildFile ("mcp-undo").deleteRecursively();
}

// The timeline tools of the MCP server (added with the clip model): every one goes through the Editor and answers an error result with a
// reason when a track, clip or marker is not there. A one-second file is at 48 kHz, and the tempo is 120 bpm: a beat is 24000 samples.
void mcpTimelineChecks()
{
    std::printf ("[29] MCP timeline tools: clips, tempo, markers, loop and master; a bad index is an error result with the reason\n");
    tempFolder().getChildFile ("mcp-tl").createDirectory();
    juce::String err;
    const auto wav = makeWav ("mcp-tl/a.wav", 48000.0, 2, 48000, 0.5f, 440.0);
    const auto path = wav.getFullPathName().replace ("\\", "\\\\");
    Session s;
    s.addTrack (wav, "A", err);
    s.addTrack (wav, "B", err);
    McpServer server (s);
    const auto call = [&server] (const char* tool, const juce::String& args) { return toolCall (server, tool, args); };
    const auto failed = [] (const juce::var& r) { return (bool) r["isError"]; };
    const auto said = [] (const juce::var& r, const char* words) { return toolText (r).contains (words); };

    juce::var list;
    juce::JSON::parse ("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", list);
    const auto tools = server.handle (list)["result"]["tools"];
    int known = 0;
    for (const char* n : { "add_clip", "move_clip", "split_clip", "trim_clip", "delete_clip", "set_clip", "set_tempo", "set_time_signature",
                           "add_marker", "remove_marker", "set_loop", "set_master" })
        for (auto& t : *tools.getArray())
            if (t["name"].toString() == n) ++known;
    test::check (tools.size() == 37 && known == 12, test::fmt ("tools/list: %d tools, %d of the 12 timeline tools among them", (int) tools.size(), known));

    // clips
    test::check (! failed (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_seconds\":1}")) && s.tracks[0]->clips.size() == 2 && s.tracks[0]->clips[1].start == 48000,
                 "add_clip: a file placed at one second");
    test::check (! failed (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_beats\":4,\"offset_seconds\":0.25,\"length_seconds\":0.5}"))
                     && s.tracks[0]->clips.size() == 3 && s.tracks[0]->clips[2].start == 96000 && s.tracks[0]->clips[2].offset == 12000 && s.tracks[0]->clips[2].length == 24000,
                 "add_clip: at beat 4 (two seconds), from 0.25 s in the file, for 0.5 s");
    test::check (failed (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_seconds\":0,\"length_seconds\":0}")) && said (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_seconds\":0,\"length_seconds\":0}"), "more than 0")
                     && s.tracks[0]->clips.size() == 3, "add_clip: a zero length is refused, and nothing is added");
    test::check (failed (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_seconds\":0,\"offset_seconds\":10}")) && s.tracks[0]->clips.size() == 3,
                 "add_clip: an offset past the end of the file is refused, and nothing is added");
    test::check (failed (call ("add_clip", "{\"track\":9,\"file\":\"" + path + "\",\"start_seconds\":0}")) && said (call ("add_clip", "{\"track\":9,\"file\":\"" + path + "\",\"start_seconds\":0}"), "No track 9"),
                 "add_clip: a track that is not there is an error result");
    test::check (failed (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_seconds\":1,\"start_beats\":2}")) && said (call ("add_clip", "{\"track\":0,\"file\":\"" + path + "\",\"start_seconds\":1,\"start_beats\":2}"), "exactly one"),
                 "add_clip: a start in seconds and in beats at once is refused");
    test::check (failed (call ("split_clip", "{\"track\":0,\"clip\":0,\"at_seconds\":0.5}")) == false && s.tracks[0]->clips.size() == 4 && s.tracks[0]->clips[0].length == 24000
                     && s.tracks[0]->clips[1].start == 24000 && s.tracks[0]->clips[1].offset == 24000 && s.tracks[0]->clips[1].length == 24000,
                 "split_clip: the left piece keeps number 0, the right one is number 1 at the cut");
    test::check (failed (call ("split_clip", "{\"track\":0,\"clip\":0,\"at_seconds\":5}")) && said (call ("split_clip", "{\"track\":0,\"clip\":0,\"at_seconds\":5}"), "inside"),
                 "split_clip: a point outside the clip is refused with the reason");
    test::check (! failed (call ("move_clip", "{\"track\":0,\"clip\":3,\"to_track\":1,\"start_seconds\":0.5}")) && s.tracks[0]->clips.size() == 3 && s.tracks[1]->clips.size() == 2
                     && s.tracks[1]->clips[1].start == 24000, "move_clip: a clip goes to another track, appended there");
    test::check (failed (call ("move_clip", "{\"track\":0,\"clip\":9,\"start_seconds\":0}")) && said (call ("move_clip", "{\"track\":0,\"clip\":9,\"start_seconds\":0}"), "No clip 9"),
                 "move_clip: a clip that is not there is an error result");
    test::check (failed (call ("move_clip", "{\"track\":0,\"clip\":0,\"to_track\":7,\"start_seconds\":1}")) && said (call ("move_clip", "{\"track\":0,\"clip\":0,\"to_track\":7,\"start_seconds\":1}"), "No track 7"),
                 "move_clip: a track that is not there is an error result");
    test::check (! failed (call ("move_clip", "{\"track\":1,\"clip\":0,\"start_seconds\":0.25}")) && s.tracks[1]->clips[0].start == 12000, "move_clip: along its own track");
    test::check (! failed (call ("trim_clip", "{\"track\":0,\"clip\":0,\"end_seconds\":0.25}")) && s.tracks[0]->clips[0].length == 12000, "trim_clip: the right edge to 0.25 s");
    test::check (failed (call ("trim_clip", "{\"track\":0,\"clip\":0}")) && said (call ("trim_clip", "{\"track\":0,\"clip\":0}"), "Give the new"), "trim_clip: no edge given is refused");
    test::check (! failed (call ("trim_clip", "{\"track\":0,\"clip\":0,\"start_seconds\":0.1}")) && s.tracks[0]->clips[0].start == 4800 && s.tracks[0]->clips[0].offset == 4800
                     && s.tracks[0]->clips[0].length == 7200, "trim_clip: the left edge to 0.1 s, the audio stays where it is");
    test::check (failed (call ("trim_clip", "{\"track\":0,\"clip\":9,\"start_seconds\":0.1}")) && said (call ("trim_clip", "{\"track\":0,\"clip\":9,\"start_seconds\":0.1}"), "No clip 9"),
                 "trim_clip: a clip that is not there is an error result");
    test::check (! failed (call ("delete_clip", "{\"track\":0,\"clip\":1}")) && s.tracks[0]->clips.size() == 2 && s.tracks[0]->clips[1].start == 48000, "delete_clip");
    test::check (failed (call ("delete_clip", "{\"track\":0,\"clip\":42}")) && said (call ("delete_clip", "{\"track\":0,\"clip\":42}"), "No clip 42"), "delete_clip: a clip that is not there is an error result");
    test::check (! failed (call ("set_clip", "{\"track\":0,\"clip\":0,\"gain_db\":-6,\"mute\":true,\"name\":\"Intro\"}")) && std::abs (s.tracks[0]->clips[0].gainDb + 6.0f) < 1e-4f
                     && s.tracks[0]->clips[0].mute && s.tracks[0]->clips[0].name == "Intro", "set_clip: gain, mute and name");
    test::check (! failed (call ("set_clip", "{\"track\":0,\"clip\":0,\"fade_in_seconds\":0.01}")) && s.tracks[0]->clips[0].fadeIn == 480 && s.tracks[0]->clips[0].fadeOut == 0,
                 "set_clip: a fade in of 0.01 s, the fade out left as it was");
    test::check (failed (call ("set_clip", "{\"track\":0,\"clip\":0}")) && said (call ("set_clip", "{\"track\":0,\"clip\":0}"), "Give at least one"), "set_clip: nothing to set is refused");
    test::check (failed (call ("set_clip", "{\"track\":0,\"clip\":0,\"name\":\"  \"}")) && s.tracks[0]->clips[0].name == "Intro", "set_clip: an empty name is refused and nothing changes");

    // tempo, time signature, markers, loop, master
    test::check (! failed (call ("set_tempo", "{\"bpm\":140,\"beat\":4}")) && std::abs (s.tempo.bpmAtBeat (4.0) - 140.0) < 1e-6 && std::abs (s.tempo.bpmAtBeat (0.0) - 120.0) < 1e-6,
                 "set_tempo: 140 bpm from beat 4, 120 before it");
    test::check (failed (call ("set_tempo", "{\"bpm\":500}")) && said (call ("set_tempo", "{\"bpm\":500}"), "20 to 300"), "set_tempo: a tempo outside 20 to 300 is refused");
    test::check (failed (call ("set_tempo", "{\"bpm\":100,\"beat\":-1}")) && said (call ("set_tempo", "{\"bpm\":100,\"beat\":-1}"), "0 or more"), "set_tempo: a beat before the start is refused");
    test::check (failed (call ("set_tempo", "{\"beat\":4}")) && said (call ("set_tempo", "{\"beat\":4}"), "Give 'bpm'"), "set_tempo: no bpm is refused");
    test::check (! failed (call ("set_time_signature", "{\"numerator\":6,\"denominator\":8}")) && s.tempo.numerator == 6 && s.tempo.denominator == 8, "set_time_signature: 6/8");
    test::check (failed (call ("set_time_signature", "{\"numerator\":3,\"denominator\":3}")) && said (call ("set_time_signature", "{\"numerator\":3,\"denominator\":3}"), "denominator")
                     && s.tempo.denominator == 8, "set_time_signature: a denominator that is not a power of two is refused");
    const auto verse = call ("add_marker", "{\"name\":\"Verse\",\"at_beats\":2}");
    test::check (! failed (verse) && (int) toolJson (verse)["added_index"] == 0 && s.markers.size() == 1 && s.markers[0].start == 48000, "add_marker: at beat 2, with its number in the answer");
    const auto unnamed = call ("add_marker", "{\"at_seconds\":0.5}");
    test::check (! failed (unnamed) && s.markers.size() == 2 && s.markers[0].name == "Marker" && s.markers[1].name == "Verse" && (int) toolJson (unnamed)["added_index"] == 0,
                 "add_marker: no name gives 'Marker'; the markers stay in time order");
    test::check (! failed (call ("remove_marker", "{\"marker\":0}")) && s.markers.size() == 1 && s.markers[0].name == "Verse", "remove_marker");
    test::check (failed (call ("remove_marker", "{\"marker\":5}")) && said (call ("remove_marker", "{\"marker\":5}"), "No marker 5"), "remove_marker: a marker that is not there is an error result");
    test::check (! failed (call ("set_loop", "{\"start_beats\":0,\"end_seconds\":1}")) && s.loopOn && s.loopStart == 0 && s.loopEnd == 48000, "set_loop: from 0 to one second, on by default");
    test::check (failed (call ("set_loop", "{\"start_seconds\":1,\"end_seconds\":0.5}")) && said (call ("set_loop", "{\"start_seconds\":1,\"end_seconds\":0.5}"), "after its start"),
                 "set_loop: an end before the start is refused");
    test::check (! failed (call ("set_loop", "{\"start_seconds\":0,\"end_seconds\":1,\"on\":false}")) && ! s.loopOn, "set_loop: on false switches it off");
    test::check (! failed (call ("set_master", "{\"master_db\":-3}")) && std::abs (s.masterDb + 3.0f) < 1e-4f, "set_master: -3 dB");
    test::check (failed (call ("set_master", "{\"master_db\":20}")) && said (call ("set_master", "{\"master_db\":20}"), "-100 to +12") && std::abs (s.masterDb + 3.0f) < 1e-4f,
                 "set_master: out of range is refused and the master stays");

    // the track settings go through the Editor too
    test::check (! failed (call ("set_track", "{\"track\":0,\"gain_db\":-2,\"pan\":0.5,\"mute\":true,\"solo\":true,\"name\":\"Lead\"}")) && std::abs (s.tracks[0]->gainDb + 2.0f) < 1e-4f
                     && std::abs (s.tracks[0]->pan - 0.5f) < 1e-6f && s.tracks[0]->mute && s.tracks[0]->solo && s.tracks[0]->name == "Lead", "set_track: fader, pan, mute, solo and name");
    test::check (! failed (call ("set_track", "{\"track\":0,\"name\":\"\"}")) && s.tracks[0]->name == "Lead", "set_track: an empty name leaves the name");
    test::check (failed (call ("set_track", "{\"track\":9,\"gain_db\":-1}")) && said (call ("set_track", "{\"track\":9,\"gain_db\":-1}"), "No track 9"), "set_track: a track that is not there is an error result");

    // project_info shows all of it
    const auto d = toolJson (call ("project_info", "{}"));
    test::check (d["tracks"][0]["clips"].size() == 2 && d["tempo"].size() == 2 && d["markers"].size() == 1 && ! (bool) d["loop"]["on"]
                     && std::abs ((double) d["master_db"] + 3.0) < 1e-4 && d["time_signature"].toString() == "6/8",
                 "project_info: the clips, tempo, markers, loop, master and time signature");
}

bool close (double a, double b) { return std::abs (a - b) < 1e-4; }

// Two channels of `length` samples of `value`: the audio of a clip made in code.
std::shared_ptr<juce::AudioBuffer<float>> constantAudio (int length, float value)
{
    auto b = std::make_shared<juce::AudioBuffer<float>> (2, length);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill (b->getWritePointer (ch), value, length);
    return b;
}

void clipChecks()
{
    std::printf ("[10] Clips: gain, fades, summing, offset and length, the pan law\n");
    Track t;
    Clip c;
    c.audio = constantAudio (1000, 0.5f);
    c.length = 1000;
    c.gainDb = -6.0f;
    c.fadeIn = 100;
    c.fadeOut = 200;
    t.clips.push_back (c);
    juce::AudioBuffer<float> dest (2, 1000);
    Session::mixClips (t, 0, 1000, dest);
    const float g = juce::Decibels::decibelsToGain (-6.0f) * 0.5f;
    const auto at = [&dest] (int i) { return dest.getSample (0, i); };
    test::check (at (0) == 0.0f, "a clip starts from silence (its fade in)");
    test::check (close (at (50), 0.5 * g), test::fmt ("halfway through the fade in: %.5f, expected %.5f", at (50), 0.5 * g));
    test::check (close (at (100), g) && close (at (800), g), "full gain between the fades");
    test::check (close (at (900), 0.5 * g), "halfway through the fade out");
    test::check (close (at (999), g / 200.0), "and nearly silent at the last sample");

    Track u;
    Clip a;
    a.audio = constantAudio (100, 0.25f);
    a.length = 100;
    Clip b;
    b.audio = constantAudio (100, 0.5f);
    b.start = 50;
    b.length = 100;
    u.clips = { a, b };
    juce::AudioBuffer<float> sum (2, 200);
    Session::mixClips (u, 0, 200, sum);
    test::check (close (sum.getSample (0, 25), 0.25) && close (sum.getSample (0, 75), 0.75) && close (sum.getSample (0, 125), 0.5) && sum.getSample (0, 160) == 0.0f,
                 "two overlapping clips sum, and each stops at its end");

    auto ramp = std::make_shared<juce::AudioBuffer<float>> (2, 1000);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 1000; ++i)
            ramp->setSample (ch, i, (float) i * 0.001f);
    Track v;
    Clip r;
    r.audio = ramp;
    r.start = 100;
    r.offset = 300;
    r.length = 50;
    v.clips.push_back (r);
    juce::AudioBuffer<float> window (2, 80);
    Session::mixClips (v, 90, 80, window);
    test::check (window.getSample (0, 9) == 0.0f && close (window.getSample (0, 10), 0.3) && close (window.getSample (0, 59), 0.349) && window.getSample (0, 60) == 0.0f,
                 "a clip plays from its offset, for its own length, at its place on the timeline");

    float left = 0.0f, right = 0.0f;
    Session::panGains (0.0f, left, right);
    test::check (left == 1.0f && right == 1.0f, "centre: both sides unity");
    Session::panGains (-1.0f, left, right);
    test::check (left == 1.0f && right == 0.0f, "hard left: nothing in the right");
    Session::panGains (1.0f, left, right);
    test::check (left == 0.0f && right == 1.0f, "hard right: nothing in the left");
    Session::panGains (-0.5f, left, right);
    test::check (close (left, 1.0) && close (right, 0.5), "half left: the far side at half");

    tempFolder().getChildFile ("pan").createDirectory();
    Session p;
    juce::String err;
    p.addTrack (makeWav ("pan/sine.wav", 48000.0, 2, 48000, 0.5f, 1000.0), "Pan", err);
    p.tracks[0]->pan = -1.0f;
    juce::AudioBuffer<float> out (2, kBlockSize);
    p.processBlock (out, 0);
    test::check (out.getMagnitude (1, 0, kBlockSize) == 0.0f && out.getMagnitude (0, 0, kBlockSize) > 0.1f, "a hard-left track puts nothing in the right channel");
}

void poolChecks()
{
    std::printf ("[11] The audio pool: a file is read once and shared, clips, empty tracks\n");
    tempFolder().getChildFile ("pool").createDirectory();
    const auto wav = makeWav ("pool/shared.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    Session s;
    juce::String err;
    auto first = s.loadPooled (wav, err);
    auto second = s.loadPooled (wav, err);
    test::check (first != nullptr && first.get() == second.get(), "loading the same file twice gives one buffer");
    test::check (s.addTrack (wav, "Shared", err) && s.tracks[0]->clips[0].audio.get() == first.get(), "a track's clip uses the pooled buffer");
    const int index = s.addClip (0, wav, 1000, err);
    test::check (index == 1 && s.tracks[0]->clips[1].audio.get() == first.get() && s.tracks[0]->clips[1].start == 1000, "addClip appends a clip at its start, sharing the buffer");
    test::check (s.addClip (7, wav, 0, err) == -1 && err.isNotEmpty(), "a track that is not there is refused, with the reason");
    test::check (s.addClip (0, tempFolder().getChildFile ("pool/none.wav"), 0, err) == -1 && err.isNotEmpty(), "a file that is not there is refused, with the reason");
    test::check (s.addEmptyTrack ("Empty") == 1 && s.tracks[1]->clips.empty() && s.tracks[1]->length() == 0, "an empty track is valid");
    test::check (s.length() == 5800, "the session is as long as its last clip ends");
    s.prepare (false);
    juce::AudioBuffer<float> out (2, kBlockSize);
    s.processBlock (out, 0);
    test::check (s.endOfAudio() == 5800 && s.tracks[0]->length() == 5800, "endOfAudio is the same as length");
}

void markerChecks()
{
    std::printf ("[12] Markers: kept sorted, moved, removed\n");
    Session s;
    juce::String err;
    s.addMarker ("late", 300);
    s.addMarker ("early", 100);
    s.addMarker ("middle", 200);
    test::check (s.markers.size() == 3 && s.markers[0].name == "early" && s.markers[2].name == "late", "markers are kept sorted");
    test::check (s.moveMarker (0, 250, err) && s.markers[0].name == "middle" && s.markers[1].name == "early" && s.markers[1].start == 250,
                 "a moved marker is put back in order");
    test::check (s.removeMarker (1, err) && s.markers.size() == 2 && ! s.removeMarker (9, err) && err.isNotEmpty(), "a marker is removed; one that is not there is refused");
}

void projectV2Checks()
{
    std::printf ("[14] Project file v2: clips, markers, tempo, loop, pan and the view settings come back\n");
    juce::String err;
    const auto dir = tempFolder().getChildFile ("rt");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto wav = makeWav ("rt/clip.wav", 48000.0, 2, 48000, 0.5f, 330.0);
    const auto file = dir.getChildFile ("song.olympus");
    {
        Session s;
        s.addTrack (wav, "Vox", err);
        s.addClip (0, wav, 12000, err);
        auto& clip = s.tracks[0]->clips[1];
        clip.offset = 1000;
        clip.length = 5000;
        clip.gainDb = -3.0f;
        clip.fadeIn = 50;
        clip.fadeOut = 400;
        s.tracks[0]->pan = 0.25f;
        s.tracks[0]->gainDb = -2.0f;
        s.tracks[0]->solo = true;
        s.addEmptyTrack ("Empty");
        s.addMarker ("Chorus", 96000);
        s.addMarker ("Intro", 0);
        s.tempo.setTempo (4.0, 140.0);
        s.tempo.setSignature (3, 4);
        s.loopStart = 1000;
        s.loopEnd = 30000;
        s.loopOn = true;
        s.metronomeOn = true;
        s.snapOn = false;
        s.grid = Grid::Eighth;
        s.masterDb = -2.5f;
        test::check (s.save (file, err), "a project with clips, markers and a tempo is saved");
    }
    Session s;
    test::check (s.load (file, err), "and loads: " + err);
    test::check (s.warnings.isEmpty() && s.tracks.size() == 2 && s.tracks[0]->clips.size() == 2 && s.tracks[1]->name == "Empty" && s.tracks[1]->clips.empty(),
                 "tracks, clips and an empty track are back");
    const auto& clip = s.tracks[0]->clips[1];
    test::check (clip.start == 12000 && clip.offset == 1000 && clip.length == 5000 && close (clip.gainDb, -3.0) && clip.fadeIn == 50 && clip.fadeOut == 400 && clip.audio != nullptr,
                 "a clip keeps its place, its part of the file, its gain and fades");
    test::check (s.tracks[0]->clips[0].audio.get() == clip.audio.get(), "two clips of one file share the buffer again");
    test::check (close (s.tracks[0]->pan, 0.25) && close (s.tracks[0]->gainDb, -2.0) && s.tracks[0]->solo, "pan, fader and solo are back");
    test::check (s.markers.size() == 2 && s.markers[0].name == "Intro" && s.markers[1].name == "Chorus" && s.markers[1].start == 96000, "markers are back, in order");
    test::check (s.tempo.points.size() == 2 && close (s.tempo.points[1].beat, 4.0) && close (s.tempo.points[1].bpm, 140.0) && s.tempo.numerator == 3 && s.tempo.denominator == 4,
                 "the tempo points and the time signature are back");
    test::check (s.loopStart == 1000 && s.loopEnd == 30000 && s.loopOn && s.metronomeOn && ! s.snapOn && s.grid == Grid::Eighth && close (s.masterDb, -2.5),
                 "the loop, the metronome, snap, the grid and the master are back");
    test::check (close (s.sampleRate, 48000.0) && close (s.tempo.sampleRate, 48000.0), "the rate is kept, and the tempo has it too");
}

void legacyChecks()
{
    std::printf ("[15] Project file v1: a track with one file loads as one clip\n");
    juce::String err;
    const auto dir = tempFolder().getChildFile ("v1");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto wav = makeWav ("v1/old.wav", 48000.0, 2, 24000, 0.5f, 220.0);
    const auto old = dir.getChildFile ("old.olympus");
    old.replaceWithText ("<OlympusProject version=\"1\" name=\"old\" sampleRate=\"48000\">"
                         "<Track name=\"Old\" file=\"old.wav\" path=\"" + wav.getFullPathName() + "\" gainDb=\"-2\" mute=\"0\" solo=\"0\">"
                         "<Slot plugin=\"Test Gain\" bypass=\"0\" state=\"\"/></Track></OlympusProject>");
    Session s;
    test::addTestPlugins (s.catalog);
    test::check (s.load (old, err), "a version 1 project loads: " + err);
    test::check (s.tracks.size() == 1 && s.tracks[0]->clips.size() == 1, "its track has one clip");
    const auto& clip = s.tracks[0]->clips[0];
    test::check (clip.start == 0 && clip.offset == 0 && clip.length == 24000 && clip.audio != nullptr && clip.file == wav,
                 "the clip covers the whole file, from sample 0");
    test::check (close (s.tracks[0]->gainDb, -2.0) && s.tracks[0]->chain.size() == 1 && s.warnings.isEmpty(), "the fader and the chain are read as before");
    test::check (s.save (dir.getChildFile ("new.olympus"), err), "and it is saved as version 2");
}

void missingAudioChecks()
{
    std::printf ("[16] A missing audio file keeps its clip through save and load\n");
    juce::String err;
    const auto dir = tempFolder().getChildFile ("gone");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto wav = makeWav ("gone/g.wav", 48000.0, 2, 12000, 0.5f, 220.0);
    {
        Session s;
        s.addTrack (wav, "G", err);
        s.tracks[0]->clips[0].start = 3000;
        s.tracks[0]->clips[0].gainDb = -4.0f;
        test::check (s.save (dir.getChildFile ("gone.olympus"), err), "saved while the audio is there");
    }
    wav.deleteFile();
    Session s;
    test::check (s.load (dir.getChildFile ("gone.olympus"), err) && s.tracks.size() == 1 && s.tracks[0]->clips.size() == 1, "the project opens without its audio");
    const auto& clip = s.tracks[0]->clips[0];
    test::check (clip.audio == nullptr && clip.length == 12000 && clip.start == 3000 && s.warnings.size() == 1, "the clip stays, silent, with its length and a warning");
    test::check (s.save (dir.getChildFile ("again.olympus"), err), "and saves again");
    Session t;
    test::check (t.load (dir.getChildFile ("again.olympus"), err), "which loads");
    const auto& back = t.tracks[0]->clips[0];
    test::check (back.audio == nullptr && back.length == 12000 && back.start == 3000 && close (back.gainDb, -4.0) && back.file == wav,
                 "with the same place, length, gain and file");
}

void metronomeChecks()
{
    std::printf ("[17] The metronome: a click on every beat, a higher one on the bar, not after its beat\n");
    Session s;
    s.sampleRate = 48000.0;
    s.tempo.sampleRate = 48000.0;
    s.metronomeOn = true;
    juce::AudioBuffer<float> block (2, kBlockSize);
    const auto peak = [&block] (int from, int to) { return block.getMagnitude (0, from, to - from); };

    s.processBlock (block, 0);
    test::check (peak (0, 960) > 0.1f, test::fmt ("a block that starts on a beat has its click in the first 20 ms (%.3f)", peak (0, 960)));
    s.processBlock (block, 24000);
    test::check (peak (0, 960) > 0.1f, "every beat clicks (beat 2 at 120 bpm is sample 24000)");
    s.processBlock (block, 24000 - 100);
    test::check (peak (0, 100) == 0.0f && peak (100, kBlockSize) > 0.1f, "a block that starts before a beat gets the click where it falls");
    s.processBlock (block, 96000);
    test::check (peak (0, 960) > 0.1f, "the bar clicks too");
    s.processBlock (block, 5000);
    test::check (peak (0, kBlockSize) == 0.0f, "between beats, with no click reaching into the block, it is silent");
    s.metronomeOn = false;
    s.processBlock (block, 0);
    test::check (peak (0, kBlockSize) == 0.0f, "off, there is no click");
}

void headChecks()
{
    std::printf ("[18] The playhead the plugins read: tempo, time signature, position in beats, loop\n");
    Session s;
    s.sampleRate = 48000.0;
    s.tempo.sampleRate = 48000.0;
    s.tempo.setSignature (3, 4);
    juce::AudioBuffer<float> block (2, kBlockSize);
    s.processBlock (block, 24000);
    const auto info = s.head.getPosition().orFallback (juce::AudioPlayHead::PositionInfo());
    test::check (close (info.getBpm().orFallback (-1.0), 120.0), "the playhead reports the tempo");
    test::check (close (info.getPpqPosition().orFallback (-1.0), 1.0) && info.getTimeInSamples().orFallback (-1) == 24000, "and its position in beats (1 at sample 24000)");
    test::check (info.getTimeSignature().hasValue() && info.getTimeSignature()->numerator == 3, "and the time signature");
    test::check (! info.getIsLooping(), "not looping when the loop is off");
    s.loopOn = true;
    s.loopStart = 0;
    s.loopEnd = 96000;
    s.processBlock (block, 24000);
    const auto looped = s.head.getPosition().orFallback (juce::AudioPlayHead::PositionInfo());
    test::check (looped.getIsLooping() && close (looped.getLoopPoints()->ppqEnd, 4.0), "with the loop on, the loop points are in beats");
    s.tempo.setTempo (4.0, 60.0);
    s.processBlock (block, 96000);
    test::check (close (s.head.getPosition()->getBpm().orFallback (-1.0), 60.0), "a tempo change is reported from its beat on");
}

void editChecks()
{
    std::printf ("[19] Undo state: capture and restore, and a track count that changed is ignored\n");
    tempFolder().getChildFile ("edit").createDirectory();
    const auto wav = makeWav ("edit/e.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    Session s;
    juce::String err;
    s.addTrack (wav, "A", err);
    s.addTrack (wav, "B", err);
    const auto before = s.captureEdit();

    s.tracks[0]->clips[0].start = 5000;
    s.tracks[0]->gainDb = -4.0f;
    s.tracks[0]->pan = 0.5f;
    s.tracks[0]->mute = true;
    s.masterDb = -6.0f;
    s.addMarker ("m", 100);
    s.loopOn = true;
    s.loopEnd = 9000;
    s.tempo.setTempo (0.0, 90.0);
    const auto after = s.captureEdit();

    s.restoreEdit (before);
    test::check (s.tracks[0]->clips[0].start == 0 && close (s.tracks[0]->gainDb, 0.0) && close (s.tracks[0]->pan, 0.0) && ! s.tracks[0]->mute, "restore puts the clips and the mix back");
    test::check (close (s.masterDb, 0.0) && s.markers.empty() && ! s.loopOn && close (s.tempo.bpmAtBeat (0.0), 120.0), "and the master, markers, loop and tempo");
    s.restoreEdit (after);
    test::check (s.tracks[0]->clips[0].start == 5000 && s.tracks[0]->mute && s.markers.size() == 1 && s.loopOn && close (s.tempo.bpmAtBeat (0.0), 90.0), "restoring the later state works too");
    s.addEmptyTrack ("C");
    s.restoreEdit (before);
    test::check (s.tracks.size() == 3 && s.tracks[0]->clips[0].start == 5000, "a state from another number of tracks is ignored");
}

void masterChecks()
{
    std::printf ("[20] The master gain is in mix.wav, not in the stems\n");
    tempFolder().getChildFile ("master").createDirectory();
    Session s;
    test::addTestPlugins (s.catalog);
    juce::String err;
    s.addTrack (makeWav ("master/m.wav", 48000.0, 2, 48000, 0.5f, 440.0), "M", err);
    s.masterDb = -6.0f;
    RenderOptions o;
    o.folder = tempFolder().getChildFile ("stems-master");
    RenderResult r;
    test::check (s.renderStems (o, r, err), "render with a master gain: " + err);
    const auto stem = readWav (o.folder.getChildFile ("01-M.wav"));
    const auto mix = readWav (o.folder.getChildFile ("mix.wav"));
    const float g = juce::Decibels::decibelsToGain (-6.0f);
    test::check (std::abs (stem.getMagnitude (0, 0, 48000) - 0.5f) < 0.002f, "the stem has no master gain");
    test::check (std::abs (mix.getMagnitude (0, 0, 48000) - 0.5f * g) < 0.002f, test::fmt ("the mix has it (%.4f, expected %.4f)", mix.getMagnitude (0, 0, 48000), 0.5f * g));
}

void describeChecks()
{
    std::printf ("[21] describe(): what the server reports about clips, the timeline and the loop\n");
    tempFolder().getChildFile ("desc").createDirectory();
    Session s;
    juce::String err;
    s.addTrack (makeWav ("desc/d.wav", 48000.0, 2, 4800, 0.5f, 440.0), "D", err);
    s.addMarker ("m", 2400);
    s.loopOn = true;
    s.loopEnd = 9600;
    const auto d = s.describe();
    const auto track = d["tracks"][0];
    test::check (track["clips"].size() == 1 && close ((double) track["clips"][0]["length_seconds"], 0.1) && ! track.hasProperty ("file"), "a track lists its clips, and has no file of its own");
    test::check ((bool) track["audio_found"] && close ((double) track["pan"], 0.0), "audio_found and pan");
    test::check (d["time_signature"].toString() == "4/4" && d["tempo"].size() == 1 && d["markers"].size() == 1 && d["markers"][0]["sample"].toString() == "2400",
                 "the root has the tempo, the time signature and the markers");
    test::check (close ((double) d["loop"]["end_seconds"], 0.2) && (bool) d["loop"]["on"] && d.hasProperty ("grid") && d.hasProperty ("master_db"),
                 "the loop, the grid and the master");
}

juce::AudioBuffer<float> mixOf (Session& s, int track, juce::int64 from, int n)
{
    juce::AudioBuffer<float> b (2, n);
    Session::mixClips (*s.tracks[(size_t) track], from, n, b);
    return b;
}

// The samples [from, to) of both channels are equal in a and b.
bool sameSamples (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from, int to)
{
    for (int ch = 0; ch < 2; ++ch)
        for (int i = from; i < to; ++i)
            if (std::abs (a.getSample (ch, i) - b.getSample (ch, i)) > 0.0f)
                return false;
    return true;
}

void splitChecks()
{
    std::printf ("[22] Editor: a split cuts the clip where asked, and its two pieces play what the clip played\n");
    tempFolder().getChildFile ("ed").createDirectory();
    const auto wav = makeWav ("ed/split.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "S", err);
    s.tracks[0]->clips[0].start = 100;
    s.tracks[0]->clips[0].offset = 300;
    s.tracks[0]->clips[0].length = 600;
    const auto before = mixOf (s, 0, 0, 1000);
    Editor ed (s);
    const auto right = ed.splitClip (0, 0, 400, err);
    const auto& clips = s.tracks[0]->clips;
    test::check (right.track == 0 && right.clip == 1 && clips.size() == 2, "a split returns the right piece, placed after the left one");
    test::check (clips[0].start == 100 && clips[0].offset == 300 && clips[0].length == 300, "the left piece keeps the start and the offset");
    test::check (clips[1].start == 400 && clips[1].offset == 600 && clips[1].length == 300, "the right piece starts at the cut, at the place in the file where the cut falls");
    test::check (sameSamples (before, mixOf (s, 0, 0, 1000), 0, 1000), "the two pieces play exactly what the clip played");
    test::check (! ed.splitClip (0, 0, 100, err).valid() && ! ed.splitClip (0, 0, 700, err).valid() && err.isNotEmpty(), "a cut at the start or the end of a clip is refused, with the reason");
    test::check (! ed.splitClip (0, 9, 200, err).valid() && ! ed.splitClip (4, 0, 200, err).valid(), "a clip or a track that is not there is refused");

    Session f;
    f.addTrack (wav, "F", err);
    f.tracks[0]->clips[0].length = 1000;
    f.tracks[0]->clips[0].fadeIn = 100;
    f.tracks[0]->clips[0].fadeOut = 200;
    Editor fe (f);
    fe.splitClip (0, 0, 400, err);
    const auto& fc = f.tracks[0]->clips;
    test::check (fc[0].fadeIn == 100 && fc[0].fadeOut == 0 && fc[1].fadeIn == 0 && fc[1].fadeOut == 200, "the left piece keeps the fade in, the right piece the fade out");
}

void moveChecks()
{
    std::printf ("[23] Editor: moves (clamped at 0, to another track), duplicate and delete\n");
    tempFolder().getChildFile ("ed").createDirectory();
    const auto wav = makeWav ("ed/move.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "A", err);
    s.addTrack (wav, "B", err);
    Editor ed (s);
    test::check (ed.moveClip (0, 0, -500, err) && s.tracks[0]->clips[0].start == 0, "a move before 0 is clamped to 0");
    test::check (ed.moveClip (0, 0, 2000, err) && s.tracks[0]->clips[0].start == 2000, "a clip moves along its track");
    test::check (! ed.moveClip (0, 5, 0, err) && err.isNotEmpty(), "a clip that is not there is refused, with the reason");
    const auto moved = ed.moveClipToTrack (0, 0, 1, 700, err);
    test::check (moved.track == 1 && moved.clip == 1 && s.tracks[0]->clips.empty() && s.tracks[1]->clips[1].start == 700, "a clip goes to another track, appended at its end");
    test::check (s.tracks[1]->clips[1].audio == s.tracks[1]->clips[0].audio, "and keeps its audio");
    test::check (! ed.moveClipToTrack (1, 0, 9, 0, err).valid(), "a track that is not there is refused");
    const auto copy = ed.duplicateClip (1, 0, err);
    test::check (copy.valid() && copy.clip == 2 && s.tracks[1]->clips[2].start == 4800 && s.tracks[1]->clips[2].length == 4800, "a duplicate is placed right after the original ends");
    test::check (s.tracks[1]->clips[2].audio == s.tracks[1]->clips[0].audio, "and shares the audio");
    test::check (ed.deleteClip (1, 2, err) && s.tracks[1]->clips.size() == 2 && ! ed.deleteClip (1, 2, err), "delete removes the clip; a second delete is refused");
}

void trimChecks()
{
    std::printf ("[24] Editor: trims keep the audio where it is on the timeline, and stay inside the source\n");
    tempFolder().getChildFile ("ed").createDirectory();
    const auto wav = makeWav ("ed/trim.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "T", err);
    Editor ed (s);
    const auto setClip = [&s] (juce::int64 start, juce::int64 offset, juce::int64 length)
    {
        auto& c = s.tracks[0]->clips[0];
        c.start = start;
        c.offset = offset;
        c.length = length;
        c.fadeIn = c.fadeOut = 0;
    };
    const auto clip = [&s]() -> const Clip& { return s.tracks[0]->clips[0]; };

    setClip (1000, 500, 2000);
    const auto before = mixOf (s, 0, 0, 6000);
    test::check (ed.trimStart (0, 0, 1400, err) && clip().start == 1400 && clip().offset == 900 && clip().length == 1600, "trim start: the start moves, and the offset and the length follow");
    const auto after = mixOf (s, 0, 0, 6000);
    test::check (after.getMagnitude (0, 0, 1400) == 0.0f && sameSamples (after, before, 1400, 3000), "the audio stays where it was on the timeline (what is cut is silent)");

    setClip (1000, 500, 2000);
    test::check (ed.trimStart (0, 0, -100, err) && clip().start == 500 && clip().offset == 0 && clip().length == 2500, "trim start before the start of the source is clamped there");
    setClip (1000, 500, 2000);
    test::check (ed.trimStart (0, 0, 99999, err) && clip().start == 2999 && clip().offset == 2499 && clip().length == 1, "trim start past the end is clamped to one sample");
    setClip (1000, 500, 2000);
    s.tracks[0]->clips[0].fadeIn = 500;
    ed.trimStart (0, 0, 2800, err);
    test::check (clip().length == 200 && clip().fadeIn == 200, "a fade in longer than the new length is shortened");

    setClip (1000, 500, 2000);
    const auto endBefore = mixOf (s, 0, 0, 6000);
    test::check (ed.trimEnd (0, 0, 2500, err) && clip().length == 1500, "trim end: the length follows the new end");
    const auto endAfter = mixOf (s, 0, 0, 6000);
    test::check (sameSamples (endAfter, endBefore, 0, 2500) && endAfter.getMagnitude (0, 2500, 3500) == 0.0f, "the audio stays where it was, and the clip stops at the new end");
    setClip (1000, 500, 2000);
    ed.trimEnd (0, 0, 99999, err);
    test::check (clip().length == 4300 && clip().offset == 500, "trim end past the source is clamped to what the file has left");
    ed.trimEnd (0, 0, 0, err);
    test::check (clip().length == 1, "trim end before the start is clamped to one sample");
    setClip (1000, 500, 2000);
    s.tracks[0]->clips[0].fadeOut = 500;
    ed.trimEnd (0, 0, 1100, err);
    test::check (clip().length == 100 && clip().fadeOut == 100, "a fade out longer than the new length is shortened");
}

void mixEditChecks()
{
    std::printf ("[25] Editor: fades, clip and track gains, pans and the master are clamped; the loop and markers are checked\n");
    tempFolder().getChildFile ("ed").createDirectory();
    const auto wav = makeWav ("ed/mix.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "M", err);
    Editor ed (s);
    const auto clip = [&s]() -> Clip& { return s.tracks[0]->clips[0]; };

    test::check (ed.setClipFades (0, 0, 10000, 10000, err) && clip().fadeIn == 4800 && clip().fadeOut == 0, "fades are clamped to the length, and the fade out to what the fade in leaves");
    test::check (ed.setClipFades (0, 0, -5, 300, err) && clip().fadeIn == 0 && clip().fadeOut == 300, "a negative fade is 0");
    test::check (ed.setClipFades (0, 0, 500, 900, err) && clip().fadeIn == 500 && clip().fadeOut == 900, "fades inside the length are kept");
    test::check (ed.setClipGain (0, 0, 100.0f, err) && close (clip().gainDb, 24.0), "clip gain is clamped to +24");
    test::check (ed.setClipGain (0, 0, -500.0f, err) && close (clip().gainDb, -100.0), "and to -100");
    test::check (ed.setClipMute (0, 0, true, err) && clip().mute && ed.setClipMute (0, 0, false, err) && ! clip().mute, "a clip is muted and unmuted");
    test::check (! ed.renameClip (0, 0, "  ", err) && err.isNotEmpty() && ed.renameClip (0, 0, " Vox ", err) && clip().name == "Vox", "a clip is renamed (trimmed); an empty name is refused");

    test::check (ed.setTrackGain (0, 50.0f, err) && close (s.tracks[0]->gainDb, 12.0) && ed.setTrackGain (0, -500.0f, err) && close (s.tracks[0]->gainDb, -100.0), "the fader is clamped to -100..+12");
    test::check (ed.setTrackPan (0, 3.0f, err) && close (s.tracks[0]->pan, 1.0) && ed.setTrackPan (0, -3.0f, err) && close (s.tracks[0]->pan, -1.0), "the pan is clamped to -1..1");
    test::check (ed.setTrackMute (0, true, err) && s.tracks[0]->mute && ed.setTrackSolo (0, true, err) && s.tracks[0]->solo, "a track is muted and soloed");
    test::check (! ed.renameTrack (0, "", err) && ed.renameTrack (0, "Lead", err) && s.tracks[0]->name == "Lead", "a track is renamed; an empty name is refused");
    test::check (! ed.setTrackGain (9, 0.0f, err) && err.isNotEmpty(), "a track that is not there is refused");
    ed.setMasterDb (99.0f);
    test::check (close (s.masterDb, 12.0), "the master is clamped to +12");
    ed.setMasterDb (-500.0f);
    test::check (close (s.masterDb, -100.0), "and to -100");

    ed.setLoop (1000, 500, true);
    test::check (s.loopStart == 1000 && s.loopEnd == 500 && ! s.loopOn, "a loop that ends before it starts stays off");
    ed.setLoop (-5, 2000, true);
    test::check (s.loopStart == 0 && s.loopEnd == 2000 && s.loopOn, "a loop start is not before 0");
    ed.setLoop (-5, -1, true);
    test::check (s.loopStart == 0 && s.loopEnd == 0 && ! s.loopOn, "an empty loop is off");

    ed.addMarker ("a", 100);
    const int c = ed.addMarker ("c", 200);
    ed.addMarker ("b", 300);
    test::check (c == 1 && s.markers[0].name == "a" && s.markers[1].name == "c" && s.markers[2].name == "b", "a marker is added in order, and the index is where it went");
    test::check (ed.moveMarker (0, 400, err) && s.markers[2].name == "a" && s.markers[2].start == 400 && s.markers[0].name == "c", "a moved marker keeps its name and the list stays sorted");
    test::check (! ed.moveMarker (9, 0, err) && ! ed.removeMarker (9, err) && ed.renameMarker (2, "A", err) && s.markers[2].name == "A", "a marker that is not there is refused; a marker is renamed");
    test::check (ed.removeMarker (0, err) && s.markers.size() == 2, "a marker is removed");
}

void snapChecks()
{
    std::printf ("[26] Editor: snap to each grid at 120 bpm 4/4 (24000 samples a beat at 48 kHz); halfway goes up; off leaves the sample\n");
    tempFolder().getChildFile ("ed").createDirectory();
    const auto wav = makeWav ("ed/snap.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "S", err);
    Editor ed (s);
    ed.setSnap (false);
    test::check (ed.snap (12345) == 12345 && ed.snap (-100) == 0, "snap off: the sample itself, and never before 0");
    ed.setSnap (true);
    const auto at = [&ed] (Grid g, juce::int64 x) { ed.setGrid (g); return ed.snap (x); };
    test::check (at (Grid::Quarter, 12000) == 24000 && at (Grid::Quarter, 11999) == 0 && at (Grid::Quarter, 35999) == 24000, "quarter (24000): halfway goes up");
    test::check (at (Grid::Eighth, 12000) == 12000 && at (Grid::Eighth, 7000) == 12000, "eighth (12000)");
    test::check (at (Grid::Sixteenth, 7000) == 6000 && at (Grid::Sixteenth, 9000) == 12000, "sixteenth (6000)");
    test::check (at (Grid::ThirtySecond, 3000) == 3000 && at (Grid::ThirtySecond, 4600) == 6000, "thirty-second (3000)");
    test::check (at (Grid::Half, 40000) == 48000, "half (48000)");
    test::check (at (Grid::Bar, 50000) == 96000 && at (Grid::Bar, 47000) == 0, "bar (96000 in 4/4)");
}

void undoChecks()
{
    std::printf ("[27] Editor: undo and redo: a split, a move, a gesture of 20 fader changes, the redo cleared, the cap, a track added\n");
    tempFolder().getChildFile ("ed").createDirectory();
    const auto wav = makeWav ("ed/undo.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "U", err);
    Editor ed (s);
    test::check (! ed.canUndo() && ! ed.canRedo() && ed.undoLabel().isEmpty() && ! ed.undo() && ! ed.redo(), "nothing to undo or redo at first");
    ed.splitClip (0, 0, 2400, err);
    test::check (ed.canUndo() && ed.undoLabel() == "Split clip", "a split is an undo step, with its label");
    test::check (ed.undo() && s.tracks[0]->clips.size() == 1 && s.tracks[0]->clips[0].length == 4800 && ed.canRedo(), "undo puts the clip back in one piece");
    test::check (ed.redo() && s.tracks[0]->clips.size() == 2 && s.tracks[0]->clips[1].start == 2400, "redo splits it again");
    ed.undo();
    ed.moveClip (0, 0, 5000, err);
    test::check (ed.undoLabel() == "Move clip" && ! ed.canRedo() && ! ed.redo(), "a new edit clears the redo stack, so there is nothing to redo");
    ed.undo();
    test::check (s.tracks[0]->clips[0].start == 0, "undo of a move puts the clip back");

    Session g;
    g.addTrack (wav, "G", err);
    Editor ged (g);
    {
        Gesture drag (ged, "Fader");
        for (int i = 1; i <= 20; ++i)
            ged.setTrackGain (0, -(float) i, err);
    }
    test::check (ged.undoLabel() == "Fader" && close (g.tracks[0]->gainDb, -20.0), "a gesture of 20 fader changes is one undo step");
    ged.undo();
    test::check (close (g.tracks[0]->gainDb, 0.0) && ! ged.canUndo(), "one undo takes the fader back to where the gesture began");
    {
        Gesture nothing (ged, "Nothing");
        ged.setTrackGain (0, 0.0f, err);
    }
    test::check (! ged.canUndo(), "a gesture that changes nothing leaves no step");

    Session c;
    c.addTrack (wav, "C", err);
    Editor ced (c);
    for (int i = 1; i <= 105; ++i)
        ced.moveClip (0, 0, i, err);
    int undone = 0;
    while (ced.undo())
        ++undone;
    test::check (undone == 100 && c.tracks[0]->clips[0].start == 5, test::fmt ("the stack holds 100 steps: %d undone, and the clip is back at 5", undone));

    Session t;
    t.addTrack (wav, "T", err);
    Editor ted (t);
    ted.moveClip (0, 0, 100, err);
    t.addEmptyTrack ("New");
    test::check (! ted.undo() && ! ted.canUndo(), "after a track is added, undo refuses and the stacks are cleared");
}

void fixChecks()
{
    std::printf ("[28] Session: a copy that cannot be made changes no track; a render with no audio, a long tail or a clip past 3 h; a missing clip's length\n");
    tempFolder().getChildFile ("ed").createDirectory();
    const auto wav = makeWav ("ed/fix.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    {
        Session s;
        test::addTestPlugins (s.catalog);
        auto made = std::make_shared<int> (0);
        // a plugin that can be made twice, then not: the second target of a copy cannot get it
        s.catalog.addInternal (test::GainPlugin::description ("Flaky Gain"), [made] (double, int) -> std::unique_ptr<juce::AudioPluginInstance>
                               { return ++*made <= 2 ? std::unique_ptr<juce::AudioPluginInstance> (new test::GainPlugin()) : nullptr; });
        for (int i = 0; i < 3; ++i)
            s.addTrack (wav, "C" + juce::String (i), err);
        test::check (s.setChain (0, { "test gain", "flaky gain" }, err), "a chain with the flaky plugin is set");
        test::check (! s.copyChain (0, { 1, 2 }, err) && err.isNotEmpty(), "a copy to two tracks fails when the second copy cannot be made: " + err);
        test::check (s.tracks[1]->chain.empty() && s.tracks[2]->chain.empty(), "and the first target was not changed either");
    }
    {
        Session s;
        s.addEmptyTrack ("E");
        RenderOptions o;
        o.folder = tempFolder().getChildFile ("norender");
        RenderResult r;
        test::check (! s.renderStems (o, r, err) && err == "Nothing to render: no audio has been added.", "a render with tracks but no audio is refused");
        test::check (! o.folder.exists(), "and writes nothing");
    }
    {
        Session s;
        const auto short8k = makeWav ("ed/tail.wav", 8000.0, 2, 8000, 0.5f, 440.0);
        s.addTrack (short8k, "T", err);
        RenderOptions o;
        o.folder = tempFolder().getChildFile ("tail");
        o.tailSeconds = 1000.0;
        o.bits = 16;
        o.writeMix = false;
        RenderResult r;
        test::check (s.renderStems (o, r, err) && std::abs (r.seconds - 61.0) < 1e-6, test::fmt ("a tail of 1000 s is clamped to a minute: %.3f s rendered", r.seconds));
    }
    {
        Session s;
        s.addTrack (wav, "L", err);
        s.addClip (0, wav, 3LL * 3600 * 48000 + 10, err);
        RenderOptions o;
        o.folder = tempFolder().getChildFile ("long");
        RenderResult r;
        test::check (! s.renderStems (o, r, err) && err.contains ("three hours") && ! o.folder.exists(), "a render longer than three hours is refused before anything is written");
    }
    {
        const auto dir = tempFolder().getChildFile ("clamp");
        dir.deleteRecursively();
        dir.createDirectory();
        dir.getChildFile ("clamp.olympus").replaceWithText ("<OlympusProject version=\"2\" sampleRate=\"48000\">"
                                                            "<Track name=\"Gone\"><Clip name=\"g\" file=\"none/none.wav\" path=\"/none/none.wav\" start=\"0\" offset=\"0\" "
                                                            "length=\"2000000000\" gainDb=\"0\" fadeIn=\"0\" fadeOut=\"0\" mute=\"0\"/></Track></OlympusProject>");
        Session s;
        test::check (s.load (dir.getChildFile ("clamp.olympus"), err) && s.tracks.size() == 1 && s.tracks[0]->clips[0].audio == nullptr
                         && s.tracks[0]->clips[0].length == (juce::int64) (48000.0 * 3600 * 2),
                     "a missing clip keeps its saved length, but no more than two hours at the session rate");
    }
}

// ---- the monitor chain, recording, and the settings they keep ----------------------------------------------------------------------------------

void setMonitorGain (Session& s, int slot, double db)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (s.monitor[(size_t) slot]->instance->getParameters()[0]);
    p->setValue (p->convertTo0to1 ((float) db));
}

double monitorGainOf (Session& s, int slot)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (s.monitor[(size_t) slot]->instance->getParameters()[0]);
    return (double) p->convertFrom0to1 (p->getValue());
}

// Two buffers hold the same samples, exactly.
bool sameAudio (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return false;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            if (a.getSample (ch, i) != b.getSample (ch, i))
                return false;
    return true;
}

// Every sample of `out` is `in` times `gain` (to 1e-5).
bool scaledBy (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& in, float gain)
{
    for (int ch = 0; ch < out.getNumChannels(); ++ch)
        for (int i = 0; i < out.getNumSamples(); ++i)
            if (std::abs (out.getSample (ch, i) - in.getSample (ch, i) * gain) > 1e-5f)
                return false;
    return true;
}

void fillSine (juce::AudioBuffer<float>& b, double freq, float level, double rate)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (ch, i, level * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / rate));
}

// The largest difference between channel `ch` of `b` and `x`; a length that differs counts as a large error.
double maxError (const juce::AudioBuffer<float>& b, int ch, const std::vector<float>& x)
{
    if (b.getNumSamples() != (int) x.size())
        return 1e9;
    double worst = 0.0;
    for (int i = 0; i < b.getNumSamples(); ++i)
        worst = juce::jmax (worst, std::abs ((double) b.getSample (ch, i) - (double) x[(size_t) i]));
    return worst;
}

bool isFloatFile (const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    return r != nullptr && r->usesFloatingPointData;
}

// Pushes `in` into the recorder in blocks of 100, 512, 37 and 1100 frames: the last is longer than the recorder's block, so it is cut.
void feedRecorder (Recorder& rec, const std::vector<std::vector<float>>& in, int frames)
{
    const int sizes[] = { 100, 512, 37, 1100 };
    int pos = 0, k = 0;
    while (pos < frames)
    {
        const int n = juce::jmin (sizes[k++ % 4], frames - pos);
        const float* ptrs[2] = { in[0].data() + pos, in[1].data() + pos };
        rec.push (ptrs, 2, n);
        pos += n;
    }
}

void monitorChecks()
{
    std::printf ("[30] Monitor chain: a gain changes the playback block, bypass restores it, a delay is in time across blocks, a busy lock or an empty chain leaves the block alone\n");
    tempFolder().getChildFile ("mon").createDirectory();
    const auto wav = makeWav ("mon/m.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    s.addTrack (wav, "M", err);
    test::check (s.addMonitorPlugin ("test gain", -1, err) && s.monitor.size() == 1 && s.monitor[0]->instance != nullptr, "a plugin goes in the monitor chain");
    test::check (! s.addMonitorPlugin ("nope", -1, err) && err.isNotEmpty() && s.monitor.size() == 1, "an unknown plugin is refused and nothing is added");

    juce::AudioBuffer<float> in (2, 1300), out (2, 1300);
    fillSine (in, 440.0, 0.5f, 48000.0);
    setMonitorGain (s, 0, 6.0);
    const float six = juce::Decibels::decibelsToGain (6.0f);
    out.makeCopyOf (in);
    s.processMonitor (out);
    test::check (scaledBy (out, in, six), "the playback block goes through the monitor gain (+6 dB, 1300 samples in one call)");
    s.setMonitorBypass (true);
    out.makeCopyOf (in);
    s.processMonitor (out);
    test::check (sameAudio (out, in), "the chain's bypass leaves the block as it was");
    s.setMonitorBypass (false);
    test::check (s.setMonitorPluginBypass (0, true, err), "a plugin of the chain can be bypassed");
    out.makeCopyOf (in);
    s.processMonitor (out);
    test::check (sameAudio (out, in) && s.monitorLatency() == 0, "a bypassed plugin does nothing");
    test::check (s.setMonitorPluginBypass (0, false, err), "and it is put back");
    out.makeCopyOf (in);
    s.processMonitor (out);
    test::check (scaledBy (out, in, six), "with the bypass off the gain is back");

    Session d;
    test::addTestPlugins (d.catalog);
    d.addTrack (wav, "D", err);
    test::check (d.addMonitorPlugin ("test delay", -1, err) && d.monitorLatency() == 64, "a delay in the monitor chain reports its latency");
    juce::AudioBuffer<float> impulse (2, 1300);
    impulse.clear();
    impulse.setSample (0, 0, 0.5f);
    impulse.setSample (1, 0, 0.5f);
    d.processMonitor (impulse);
    bool inTime = true;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 1300; ++i)
            if (std::abs (impulse.getSample (ch, i) - (i == 64 ? 0.5f : 0.0f)) > 1e-6f)
                inTime = false;
    test::check (inTime, "the delay plays the impulse 64 samples later, across the block boundaries at 512 and 1024");

    {
        std::atomic<bool> held { false }, release { false };
        std::thread holder ([&s, &held, &release]
        {
            const juce::ScopedLock sl (s.lock);
            held = true;
            while (! release.load())
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        });
        while (! held.load())
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        out.makeCopyOf (in);
        s.processMonitor (out);
        test::check (sameAudio (out, in), "while the lock is busy the block is left as it is");
        release = true;
        holder.join();
    }
    test::check (s.removeMonitorPlugin (0, err) && ! s.removeMonitorPlugin (0, err), "a plugin leaves the chain; a place that is not there is refused");
    out.makeCopyOf (in);
    s.processMonitor (out);
    test::check (sameAudio (out, in), "an empty chain leaves the block as it was");
}

void monitorStateChecks()
{
    std::printf ("[31] Monitor chain: processBlock and the renders are the same with it; it is not in the project; saved apart, a missing plugin keeps its settings; described\n");
    const auto dir = tempFolder().getChildFile ("monst");
    dir.createDirectory();
    const auto wav = makeWav ("monst/a.wav", 48000.0, 2, 9600, 0.4f, 300.0);
    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    s.addTrack (wav, "R", err);
    test::check (s.setChain (0, { "test gain" }, err), "a track with a plugin");
    setGain (s, 0, 0, -4.0);

    const auto play = [&s]
    {
        juce::AudioBuffer<float> all (2, 9600);
        for (juce::int64 start = 0; start < 9600; start += kBlockSize)
        {
            const int n = (int) juce::jmin ((juce::int64) kBlockSize, 9600 - start);
            juce::AudioBuffer<float> block (2, n);
            s.processBlock (block, start);
            for (int ch = 0; ch < 2; ++ch)
                all.copyFrom (ch, (int) start, block, ch, 0, n);
        }
        return all;
    };
    const auto before = play();
    test::check (s.addMonitorPlugin ("test delay", -1, err), "a delay goes in the monitor chain");
    test::check (sameAudio (before, play()), "processBlock plays the same with the monitor chain in place");

    RenderOptions o;
    RenderResult without, with;
    s.removeMonitorPlugin (0, err);
    o.folder = dir.getChildFile ("without");
    test::check (s.renderStems (o, without, err), "a render with no monitor chain: " + err);
    s.addMonitorPlugin ("test delay", -1, err);
    o.folder = dir.getChildFile ("with");
    test::check (s.renderStems (o, with, err), "a render with the monitor chain: " + err);
    test::check (with.stems.size() == 1 && without.stems.size() == 1
                     && sameAudio (readWav (juce::File (with.stems[0].file)), readWav (juce::File (without.stems[0].file)))
                     && sameAudio (readWav (juce::File (with.mixFile)), readWav (juce::File (without.mixFile))),
                 "the stem and the mix are the same sample for sample with the monitor chain");

    const auto project = dir.getChildFile ("p.olympus");
    test::check (s.save (project, err) && ! project.loadFileAsString().contains ("Test Delay"), "the project file does not hold the monitor chain");

    const auto settings = dir.getChildFile ("settings.xml");
    const auto file = Settings::monitorFile (settings);
    test::check (file.getFileName() == "monitor.xml" && file.getParentDirectory() == settings.getParentDirectory(), "the monitor chain is kept beside the settings, as monitor.xml");
    s.addMonitorPlugin ("test gain", -1, err);
    setMonitorGain (s, 1, -3.0);
    s.setMonitorPluginBypass (0, true, err);
    test::check (s.saveMonitor (file, err), "the chain is saved: " + err);

    Session t;
    test::addTestPlugins (t.catalog);
    juce::StringArray warnings;
    test::check (t.loadMonitor (file, warnings) && warnings.isEmpty() && t.monitor.size() == 2 && t.monitor[0]->bypass && ! t.monitor[1]->bypass
                     && std::abs (monitorGainOf (t, 1) + 3.0) < 1e-3,
                 "the chain comes back: its plugins, their bypass and their settings");

    juce::MemoryBlock kept;
    kept.append ("abc", 3);
    dir.getChildFile ("gone.xml").replaceWithText ("<OlympusMonitor version=\"1\" bypass=\"0\"><Plugin plugin=\"Gone Plugin\" bypass=\"1\" state=\""
                                                   + kept.toBase64Encoding() + "\"/></OlympusMonitor>");
    Session u;
    test::addTestPlugins (u.catalog);
    juce::StringArray gone;
    test::check (u.loadMonitor (dir.getChildFile ("gone.xml"), gone) && u.monitor.size() == 1 && u.monitor[0]->instance == nullptr && gone.size() == 1
                     && u.monitor[0]->savedState.matches ("abc", 3),
                 "a missing plugin is a warning, stays in the chain and keeps its settings");
    const auto again = dir.getChildFile ("again.xml");
    test::check (u.saveMonitor (again, err), "saved again");
    Session v;
    test::addTestPlugins (v.catalog);
    juce::StringArray twice;
    test::check (v.loadMonitor (again, twice) && v.monitor.size() == 1 && v.monitor[0]->savedState.matches ("abc", 3) && v.monitor[0]->bypass,
                 "and loaded again its settings and bypass are still there");

    dir.getChildFile ("project.xml").replaceWithText ("<OlympusProject version=\"2\"/>");
    juce::StringArray bad;
    test::check (! v.loadMonitor (dir.getChildFile ("project.xml"), bad) && bad.size() == 1 && v.monitor.size() == 1, "a file that is not a monitor chain is refused, and the chain stays");

    const auto d = v.describe();
    test::check (d["monitor"].size() == 1 && d["monitor"][0]["plugin"].toString() == "Gone Plugin" && ! (bool) d["monitor"][0]["installed"] && (bool) d["monitor"][0]["bypass"],
                 "describe() lists the monitor chain with its plugins, bypass and installed flags");
}

void recorderChecks()
{
    std::printf ("[32] Recorder: two inputs in odd blocks, a mono take, two armed tracks, a second take never overwrites, no armed track is refused\n");
    const auto dir = tempFolder().getChildFile ("rec");
    dir.deleteRecursively();
    const double rate = 48000.0;
    const int frames = 24000; // half a second
    std::vector<std::vector<float>> in (2, std::vector<float> ((size_t) frames));
    for (int i = 0; i < frames; ++i)
    {
        in[0][(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rate);
        in[1][(size_t) i] = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 2000.0 * i / rate);
    }

    Recorder rec;
    rec.prepare (rate, 512);
    juce::String err;
    Session s;
    s.addEmptyTrack ("Vox");
    s.addEmptyTrack ("Gtr");
    test::check (! beginRecording (rec, s, dir, rate, err) && err.contains ("armed") && ! rec.isRecording(), "no armed track: the start is refused, and the reason says why");
    test::check (s.setTrackArm (0, true, err) && s.setTrackArm (1, true, err) && s.setTrackInput (0, 0, 1, err) && s.setTrackInput (1, 1, -1, err),
                 "two tracks are armed, on different inputs");
    test::check (! s.setTrackInput (0, -1, 0, err) && ! s.setTrackArm (9, true, err) && s.tracks[0]->inputLeft == 0, "a bad input or track is refused and changes nothing");
    err.clear();
    test::check (beginRecording (rec, s, dir, rate, err) && rec.isRecording(), "the armed tracks start recording: " + err);

    feedRecorder (rec, in, frames);
    test::check (rec.framesRecorded() == frames, "every frame pushed is counted");
    test::check (std::abs (rec.inputPeak (0) - 0.5f) < 0.01f && rec.inputPeak (0) == 0.0f, "the meter reads the peak of input 0, and reading it resets it");

    const auto first = rec.stop();
    test::check (! rec.isRecording() && first.size() == 2 && rec.samplesDropped() == 0, "stop ends both takes, and nothing was dropped");
    test::check (first.size() == 2 && first[0].file.getFileName() == "Vox-take-1.wav" && first[1].file.getFileName() == "Gtr-take-1.wav"
                     && first[0].track == 0 && first[1].track == 1 && first[0].frames == frames && first[1].frames == frames,
                 "one file per take, named after its track, with every frame in it");
    if (first.size() == 2)
    {
        double fileRate = 0.0;
        const auto vox = readWav (first[0].file, &fileRate);
        const auto gtr = readWav (first[1].file);
        test::check (vox.getNumSamples() == frames && fileRate == rate && isFloatFile (first[0].file), "the file has the length, the rate and float samples");
        test::check (maxError (vox, 0, in[0]) < 1e-6 && maxError (vox, 1, in[1]) < 1e-6, "Vox holds input 0 on the left and input 1 on the right, within 1e-6");
        test::check (maxError (gtr, 0, in[1]) < 1e-6 && maxError (gtr, 1, in[1]) < 1e-6, "a mono take (input 1) is copied to both channels");
    }

    std::vector<std::vector<float>> quieter = in;
    for (auto& ch : quieter)
        for (auto& x : ch)
            x *= 0.1f;
    Recorder again;
    again.prepare (rate, 512);
    err.clear();
    test::check (beginRecording (again, s, dir, rate, err), "a second start in the same folder: " + err);
    feedRecorder (again, quieter, frames);
    const auto second = again.stop();
    test::check (second.size() == 2 && second[0].file.getFileName() == "Vox-take-2.wav" && second[1].file.getFileName() == "Gtr-take-2.wav",
                 "the second take gets the next number");
    test::check (first.size() == 2 && readWav (first[0].file).getNumSamples() == frames && maxError (readWav (first[0].file), 0, in[0]) < 1e-6,
                 "and the first take is not overwritten");
}

void finishChecks()
{
    std::printf ("[33] Finish: a take lands on its track minus the latency (never before 0); a session without a rate takes the take's; one undo removes all takes\n");
    const auto dir = tempFolder().getChildFile ("fin");
    dir.createDirectory();
    const auto file44 = makeWav ("fin/t.wav", 44100.0, 2, 44100, 0.5f, 440.0);
    const auto file44b = makeWav ("fin/u.wav", 44100.0, 2, 22050, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addEmptyTrack ("A");
    s.addEmptyTrack ("B");
    test::check (s.sampleRate == 0.0, "a new session has no rate");
    Editor ed (s);
    const std::vector<Recorder::Finished> takes { { 0, file44, 44100 }, { 1, file44b, 22050 } };
    test::check (finishRecording (takes, 10000, 2000, ed, err) && s.sampleRate == 44100.0, "both takes are added, and the session takes the recording's rate: " + err);
    test::check (s.tracks[0]->clips.size() == 1 && s.tracks[0]->clips[0].start == 8000 && s.tracks[0]->clips[0].length == 44100,
                 "track A: the take starts 2000 samples before where recording began");
    test::check (s.tracks[1]->clips.size() == 1 && s.tracks[1]->clips[0].start == 8000 && s.tracks[1]->clips[0].length == 22050, "track B: the same place, its own length");
    test::check (ed.undoLabel() == "Record" && ed.undo() && s.tracks[0]->clips.empty() && s.tracks[1]->clips.empty(), "one undo step removes both takes");
    test::check (ed.redo() && s.tracks[0]->clips.size() == 1 && s.tracks[1]->clips.size() == 1, "and redo puts them back");

    Session c;
    c.addEmptyTrack ("C");
    Editor ec (c);
    const std::vector<Recorder::Finished> early { { 0, file44, 44100 }, { 0, file44b, 0 } };
    test::check (finishRecording (early, 500, 2000, ec, err) && c.tracks[0]->clips.size() == 1 && c.tracks[0]->clips[0].start == 0 && c.sampleRate == 44100.0,
                 "a take that would start before 0 starts at 0; an empty take adds nothing");
    test::check (c.tracks[0]->clips[0].offset == 1500 && c.tracks[0]->clips[0].length == 44100 - 1500,
                 "and its head that falls before 0 is skipped: the offset is the part before 0 (1500), so the rest plays on time");

    // a take starting at 100 with 441 samples of latency: placed at 0, its first 341 samples skipped
    Session h;
    h.addEmptyTrack ("H");
    Editor eh (h);
    test::check (finishRecording ({ { 0, file44, 44100 } }, 100, 441, eh, err) && h.tracks[0]->clips.size() == 1 && h.tracks[0]->clips[0].start == 0
                     && h.tracks[0]->clips[0].offset == 341 && h.tracks[0]->clips[0].length == 44100 - 341,
                 "start 100, latency 441: the clip starts at 0 from sample 341 of the file");
    Session z;
    z.addEmptyTrack ("Z");
    Editor ez (z);
    test::check (finishRecording ({ { 0, file44, 44100 } }, 100, 0, ez, err) && z.tracks[0]->clips[0].start == 100 && z.tracks[0]->clips[0].offset == 0
                     && z.tracks[0]->clips[0].length == 44100, "latency 0: the take is placed where recording began, unchanged");
    Session t;
    t.addEmptyTrack ("T");
    Editor et (t);
    err.clear();
    test::check (! finishRecording ({ { 0, file44, 300 } }, 100, 441, et, err) && t.tracks[0]->clips.empty() && err.contains ("was not placed"),
                 "a take shorter than its latency is not placed, and the reason says so");
}

void recordSettingsChecks()
{
    std::printf ("[34] Arm and inputs in the project (version 2; an old project loads with the defaults); the audio device and extra latency in settings, and an old settings file\n");
    const auto dir = tempFolder().getChildFile ("set");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto wav = makeWav ("set/s.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "Vox", err);
    s.addEmptyTrack ("Gtr");
    test::check (s.setTrackArm (1, true, err) && s.setTrackInput (1, 3, -1, err), "a track is armed, on input 3 in mono");
    test::check (! s.setTrackInput (0, -2, 0, err) && err.isNotEmpty() && s.tracks[0]->inputLeft == 0, "a bad input is refused and changes nothing");
    const auto project = dir.getChildFile ("p.olympus");
    test::check (s.save (project, err), "the project is saved");

    Session t;
    test::check (t.load (project, err) && ! t.tracks[0]->armed && t.tracks[1]->armed && t.tracks[1]->inputLeft == 3 && t.tracks[1]->inputRight == -1
                     && t.tracks[0]->inputRight == 1,
                 "they come back, and a track that had none keeps the defaults");
    const auto d = t.describe();
    test::check ((bool) d["tracks"][1]["armed"] && (int) d["tracks"][1]["input_left"] == 3 && (int) d["tracks"][1]["input_right"] == -1, "describe() reports them");

    dir.getChildFile ("old.olympus").replaceWithText ("<OlympusProject version=\"2\" sampleRate=\"48000\"><Track name=\"Old\"/></OlympusProject>");
    Session o;
    test::check (o.load (dir.getChildFile ("old.olympus"), err) && o.tracks.size() == 1 && ! o.tracks[0]->armed && o.tracks[0]->inputLeft == 0 && o.tracks[0]->inputRight == 1,
                 "a project without the attributes loads with the defaults");

    const auto settings = dir.getChildFile ("settings.xml");
    Settings st;
    st.lastProject = "x.olympus";
    st.extraLatencySamples = 128;
    juce::XmlElement device ("DEVICESETUP");
    device.setAttribute ("deviceType", "CoreAudio");
    device.setAttribute ("audioOutputDeviceName", "Built-in");
    st.audioDeviceState = device.toString();
    st.save (settings);
    const auto back = Settings::load (settings);
    const auto parsed = juce::parseXML (back.audioDeviceState);
    test::check (back.extraLatencySamples == 128 && back.lastProject == "x.olympus" && parsed != nullptr && parsed->isEquivalentTo (&device, true),
                 "the audio device state and the extra latency come back");

    dir.getChildFile ("old-settings.xml").replaceWithText ("<OlympusSettings lastProject=\"y.olympus\" scanSuiteOnly=\"1\"/>");
    const auto old = Settings::load (dir.getChildFile ("old-settings.xml"));
    test::check (old.lastProject == "y.olympus" && old.scanSuiteOnly && old.extraLatencySamples == 0 && old.audioDeviceState.isEmpty(),
                 "a settings file without the new fields loads with the defaults");
}

void realPlugins()
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("OLYMPUS_PLUGIN_DIR", {});
    if (dir.isEmpty())
    {
        std::printf ("[9] Built plugins: skipped (set OLYMPUS_PLUGIN_DIR)\n");
        return;
    }
    std::printf ("[9] Built plugins in %s\n", dir.toRawUTF8());
    Session s;
    ScanOptions options;
    options.includeSystem = false;
    options.deadMansPedal = tempFolder().getChildFile ("real-scan.pedal");
    s.catalog.scan (juce::FileSearchPath (dir), options);
    const auto names = s.catalog.names();
    std::printf ("    found: %s\n", names.joinIntoString (", ").toRawUTF8());
    test::check (names.size() > 0, "the suite's plugins are found in the folder");
    if (names.isEmpty()) return;
    juce::String err;
    s.addTrack (makeWav ("real.wav", 48000.0, 2, 48000, 0.3f, 300.0), "Real", err);
    test::check (s.setChain (0, { names[0] }, err), "a built plugin goes on a track: " + err);
    RenderOptions o;
    o.folder = tempFolder().getChildFile ("real-stems");
    RenderResult r;
    test::check (s.renderStems (o, r, err) && r.stems.size() == 1 && r.stems[0].peakDb > -90.0, "and renders a stem that is not silent: " + err);
    const auto xml = s.toXml (tempFolder().getChildFile ("real.olympus"));
    test::check (xml->getChildByName ("Track")->getChildByName ("Slot")->getStringAttribute ("state").isNotEmpty(), "its state is written to the project");
}
} // namespace

void deviceRateChecks()
{
    std::printf ("[35] The session follows the device's rate: an empty session takes it, its monitor chain is set up again, a session with clips is converted once\n");
    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    test::check (s.addMonitorPlugin ("test gain", -1, err) && s.monitor.size() == 1 && s.monitor[0]->instance != nullptr, "a monitor plugin is added before any rate is known");
    auto* mon = dynamic_cast<test::GainPlugin*> (s.monitor[0]->instance.get());
    test::check (mon != nullptr && mon->preparedRate == 48000.0, "it is set up at 48 kHz, the default, while the session has no rate");
    test::check (s.adoptRate (44100.0, err) && s.sampleRate == 44100.0 && s.tempo.sampleRate == 44100.0, "an empty session takes the device's 44.1 kHz");
    test::check (mon != nullptr && mon->preparedRate == 44100.0, "and the monitor chain is set up again at it");
    s.addEmptyTrack ("Empty");
    test::check (s.addPlugin (0, "test gain", -1, err), "a plugin on the empty track");
    auto* g = dynamic_cast<test::GainPlugin*> (s.tracks[0]->chain[0]->instance.get());
    test::check (g != nullptr && g->preparedRate == 44100.0, "the track's plugin is set up at 44.1 kHz");
    test::check (s.adoptRate (96000.0, err) && g->preparedRate == 96000.0 && mon->preparedRate == 96000.0, "96 kHz: the track's plugin and the monitor chain are set up again");
    test::check (s.adoptRate (96000.0, err) && s.sampleRate == 96000.0, "the same rate changes nothing");
    test::check (! s.adoptRate (0.0, err) && err.isNotEmpty() && s.sampleRate == 96000.0, "a rate of zero is refused, with the reason");

    Session c;
    test::addTestPlugins (c.catalog);
    auto src = makeWav ("rate-src.wav", 48000.0, 2, 48000, 0.5f, 440.0);
    test::check (c.addTrack (src, "Shared", err) && c.addClip (0, src, 24000, err) == 1, "a track with two clips of one file, the second at 0.5 s");
    c.tracks[0]->clips[0].fadeIn = 480;
    test::check (c.addMarker ("Mid", 24000) == 0, "a marker at 0.5 s");
    c.loopStart = 12000;
    c.loopEnd = 36000;
    c.loopOn = true;
    test::check (c.length() == 72000, "the session is 1.5 s long at 48 kHz");
    test::check (c.adoptRate (44100.0, err) && c.sampleRate == 44100.0, "the session is converted to 44.1 kHz");
    auto& a = c.tracks[0]->clips[0];
    auto& b = c.tracks[0]->clips[1];
    test::check (a.audio != nullptr && a.audio.get() == b.audio.get() && std::abs (a.audio->getNumSamples() - 44100) <= 1, "the one buffer is still shared by both clips, now 44100 samples");
    test::check (std::abs (a.length - 44100) <= 1 && std::abs (b.start - 22050) <= 1 && std::abs (b.length - 44100) <= 1,
                 test::fmt ("the clips keep their seconds: 1 s, the second at 0.5 s (%d, %d, %d samples)", (int) a.length, (int) b.start, (int) b.length));
    test::check (std::abs (c.markers[0].start - 22050) <= 1 && std::abs (c.loopStart - 11025) <= 1 && std::abs (c.loopEnd - 33075) <= 1 && std::abs (a.fadeIn - 441) <= 1,
                 "the marker, the loop and the fade are scaled by the same ratio");
    test::check (std::abs (c.tempo.sampleRate - 44100.0) < 0.5 && c.length() == b.end() && std::abs (c.length() - 66150) <= 1, "the tempo's rate follows, and the session's length is 1.5 s");

    RenderOptions o;
    o.folder = tempFolder().getChildFile ("converted");
    RenderResult r;
    test::check (c.renderStems (o, r, err) && r.stems.size() == 1, "a render after the conversion writes its stem");
    double renderedRate = 0.0;
    const auto rendered = readWav (juce::File (r.stems[0].file), &renderedRate);
    test::check (std::abs (renderedRate - 44100.0) < 0.5 && std::abs (rendered.getNumSamples() - (int) c.length()) <= 1,
                 test::fmt ("the stem is 44.1 kHz and as long as the session (%d samples)", rendered.getNumSamples()));

    const auto file = tempFolder().getChildFile ("converted.olympus");
    test::check (c.save (file, err), "saved after the conversion");
    Session d;
    test::addTestPlugins (d.catalog);
    test::check (d.load (file, err) && std::abs (d.sampleRate - 44100.0) < 0.5 && d.tracks[0]->clips.size() == 2 && d.tracks[0]->clips[0].audio != nullptr
                     && std::abs (d.tracks[0]->length() - 66150) <= 1, "the project reloads at 44.1 kHz with its clips and length");
    test::check (c.adoptRate (48000.0, err) && std::abs (c.tracks[0]->length() - 72000) <= 2, "back to 48 kHz: the length is 72000 samples again");
    test::check (c.adoptRate (44100.0, err) && c.tracks[0]->clips[0].audio.get() == c.tracks[0]->clips[1].audio.get(), "and to 44.1 kHz again, the buffer still shared");
}

void busChecks()
{
    std::printf ("[36] Bus layouts: a sidechain is switched off and the plugin runs stereo; a mono plugin is refused with the reason; a project with one keeps it unusable; no plugin is handed more channels than it has\n");
    juce::String err;
    const auto wav = makeWav ("bus-src.wav", 48000.0, 2, 48000, 0.3f, 300.0);
    Session s;
    test::addTestPlugins (s.catalog);
    test::addBusPlugins (s.catalog);
    test::check (s.addTrack (wav, "Bus", err), "a track with audio");
    test::check (s.addPlugin (0, "test aux", -1, err) && s.tracks[0]->chain.size() == 1 && s.tracks[0]->chain[0]->instance != nullptr, "a plugin with a sidechain is added");
    auto* aux = dynamic_cast<test::PassPlugin*> (s.tracks[0]->chain[0]->instance.get());
    test::check (aux != nullptr && aux->getTotalNumInputChannels() == 2 && aux->getChannelCountOfBus (true, 1) == 0, "its sidechain is switched off, and its main pair is stereo");
    test::check (aux != nullptr && std::abs (aux->preparedRate - 48000.0) < 0.5, "it is set up at the session's rate");

    juce::AudioBuffer<float> out (2, kBlockSize);
    for (int i = 0; i < 200; ++i)
    {
        s.processBlock (out, (juce::int64) i * kBlockSize);
        if (i % 3 == 0) { juce::AudioBuffer<float> mono (1, kBlockSize); mono.clear(); s.processMonitor (mono); }
    }
    test::check (aux != nullptr && aux->blocks >= 200 && aux->shortBlocks == 0 && aux->lastChannels >= 2, "playing 200 blocks: every block had the plugin's two channels");

    test::check (s.addMonitorPlugin ("test aux", -1, err) && s.monitor.size() == 1, "the same plugin in the monitor chain");
    auto* monAux = dynamic_cast<test::PassPlugin*> (s.monitor[0]->instance.get());
    juce::AudioBuffer<float> device (1, kBlockSize);  // a mono device: the plugin still gets its two channels
    for (int i = 0; i < 50; ++i)
    {
        device.clear();
        s.processMonitor (device);
        out.clear();
        s.processMonitor (out);
    }
    test::check (monAux != nullptr && monAux->shortBlocks == 0 && monAux->lastChannels == 2, "the monitor chain is run on two channels, for a mono device too");

    const bool monoAdded = s.addPlugin (0, "test mono", -1, err);
    test::check (! monoAdded && err.contains ("cannot run on a stereo track") && err.contains ("1 in, 1 out"), "a mono plugin is refused on a track, with the reason: " + err);
    test::check (s.tracks[0]->chain.size() == 1, "and nothing is added");
    test::check (! s.addMonitorPlugin ("test mono", -1, err) && s.monitor.size() == 1, "and refused on the monitor chain");

    // a project that holds a mono plugin (made by hand: the chain was stereo when it was saved) keeps the slot, unusable, with its state
    Session p;
    test::addTestPlugins (p.catalog);
    test::addBusPlugins (p.catalog);
    test::check (p.addTrack (wav, "Mono", err) && p.addPlugin (0, "test gain", -1, err), "a project with a plugin on a track");
    tempFolder().getChildFile ("bus").createDirectory();
    const auto file = tempFolder().getChildFile ("bus/mono.olympus");
    test::check (p.save (file, err), "saved");
    auto xml = juce::parseXML (file);
    if (xml != nullptr && xml->getChildByName ("Track") != nullptr && xml->getChildByName ("Track")->getChildByName ("Slot") != nullptr)
        xml->getChildByName ("Track")->getChildByName ("Slot")->setAttribute ("plugin", "Test Mono");
    test::check (xml != nullptr && xml->writeTo (file), "the saved plugin is renamed to the mono one");
    Session q;
    test::addTestPlugins (q.catalog);
    test::addBusPlugins (q.catalog);
    const bool loaded = q.load (file, err);
    test::check (loaded && q.tracks.size() == 1 && q.tracks[0]->chain.size() == 1, "the project loads");
    if (loaded && q.tracks.size() == 1 && q.tracks[0]->chain.size() == 1)
    {
        test::check (q.tracks[0]->chain[0]->instance == nullptr && q.tracks[0]->chain[0]->plugin == "Test Mono" && q.tracks[0]->chain[0]->savedState.getSize() > 0,
                     "the mono slot is kept unusable, with its saved state");
        test::check (q.warnings.size() == 1 && q.warnings[0].contains ("cannot run on a stereo track"), "a warning says why");
        const auto again = tempFolder().getChildFile ("bus/mono-again.olympus");
        test::check (q.save (again, err), "saved again");
        auto back = juce::parseXML (again);
        test::check (back != nullptr && back->getChildByName ("Track") != nullptr && back->getChildByName ("Track")->getChildByName ("Slot") != nullptr
                         && back->getChildByName ("Track")->getChildByName ("Slot")->getStringAttribute ("plugin") == "Test Mono"
                         && back->getChildByName ("Track")->getChildByName ("Slot")->getStringAttribute ("state").isNotEmpty(),
                     "and it is still in the project with its state");
    }
}

void addClipLockChecks()
{
    std::printf ("[37] Adding a clip: the file is read with the session's lock free (a decode under it would silence the audio callback), and a refusal changes nothing\n");
    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    test::check (s.adoptRate (48000.0, err), "the session is at 48 kHz");
    s.addEmptyTrack ("Lane");
    Editor editor (s);
    const auto big = makeWav ("big-read.wav", 44100.0, 2, 44100 * 20, 0.3f, 330.0); // 20 s at 44.1 kHz: read and resampled to 48 kHz
    std::atomic<bool> reading { false }, done { false };
    std::atomic<int> freeTries { 0 };
    std::thread spy ([&]
    {
        while (! done.load())
            if (reading.load() && s.lock.tryEnter())
            {
                if (reading.load()) ++freeTries;
                s.lock.exit();
            }
    });
    reading = true;
    const auto clip = editor.addClip (0, big, 0, 0, 0, err);
    reading = false;
    done = true;
    spy.join();
    test::check (clip.valid() && s.tracks[0]->clips.size() == 1 && s.tracks[0]->clips[0].audio->getNumSamples() == 960000,
                 test::fmt ("the 20 s file is added, resampled (%d samples)", clip.valid() ? (int) s.tracks[0]->clips[0].audio->getNumSamples() : -1));
    // the spy gets the lock only while the read runs outside it: a read under the lock leaves it one or two tries, in the window before the add takes it
    test::check (freeTries.load() > 1000, test::fmt ("the lock was free %d times while the file was read (a read under it allows one or two)", freeTries.load()));

    s.dirty = false;
    const bool wasUndoable = editor.undo();   // the add is one undo step
    test::check (wasUndoable && s.tracks[0]->clips.empty(), "the add is one undo step");
    s.dirty = false;
    test::check (! editor.addClip (0, big, 0, 999999999, 0, err).valid() && ! err.isEmpty() && s.tracks[0]->clips.empty() && ! s.dirty,
                 "a clip from past the end of the file is refused, and the session is untouched");
    test::check (! editor.addClip (5, big, 0, 0, 0, err).valid() && err.contains ("No track 5") && ! s.dirty, "a track that is not there is refused, and nothing changes");
    test::check (! editor.addClip (0, tempFolder().getChildFile ("bus/none.wav"), 0, 0, 0, err).valid() && err.isNotEmpty() && s.tracks[0]->clips.empty(),
                 "a file that is not there is refused with the reason");
}

// [38] A plugin that changes its latency: after the timer's call the tracks follow it; a missing plugin a scan finds is given its slot; slots
// that listen to a plugin are removed safely by every way out of a session.
void latencyChecks()
{
    std::printf ("[38] Latency changes: a plugin that reports a new latency moves the tracks' delays once the window has followed it; a missing plugin is retried; removals with listeners are safe\n");
    juce::String err;
    {
        Session s;
        test::addTestPlugins (s.catalog);
        s.addEmptyTrack ("A");
        s.addEmptyTrack ("B");
        test::check (s.setChain (0, { "test gain" }, err) && s.setChain (1, { "test delay" }, err), "track A has no latency, track B a delay of 64");
        test::check (s.tracks[0]->delaySamples == 64 && s.tracks[1]->delaySamples == 0, "so A is delayed by 64 to line up with B");
        auto* gain = dynamic_cast<test::GainPlugin*> (s.tracks[0]->chain[0]->instance.get());
        test::check (gain != nullptr, "A's plugin is the test gain");
        gain->changeLatency (100);   // a linear-phase switch: the plugin reports it, and the flag is set (no lock, any thread)
        test::check (s.latencyWatch.changed.load(), "the plugin's report is noticed");
        test::check (s.takeLatencyChange() && ! s.takeLatencyChange(), "takeLatencyChange reads the report once");
        s.updateLatencies();   // what the window's timer does after it
        test::check (s.tracks[0]->delaySamples == 0 && s.tracks[1]->delaySamples == 36, "B is now delayed by 36 (100 - 64) and A by 0: the tracks follow the new latency");
        test::check (s.maxLatency() == 100, "and the session's latency is the new one");
    }

    {
        // a project's missing plugin (a track's and a monitor chain's) gets another try when the scan has found it
        Session m;
        test::addTestPlugins (m.catalog);
        m.addEmptyTrack ("M");
        m.tracks[0]->chain.push_back (std::make_unique<Slot>());
        m.tracks[0]->chain.back()->plugin = "Late Plugin";
        m.monitor.push_back (std::make_unique<Slot>());
        m.monitor.back()->plugin = "Late Plugin";
        test::check (m.retryMissing() == 0 && m.tracks[0]->chain.back()->instance == nullptr, "a plugin not found is still missing: nothing changes");
        m.catalog.addInternal (test::GainPlugin::description ("Late Plugin"), [] (double, int) { return std::make_unique<test::GainPlugin> ("Late Plugin", 0); });
        test::check (m.retryMissing() == 2 && m.tracks[0]->chain.back()->instance != nullptr && m.monitor.back()->instance != nullptr,
                     "found by a scan: its track slot and its monitor slot take the plugin");
        test::check (m.retryMissing() == 0, "a plugin that is there is not made again");
    }

    {
        // slots that listen to their plugins are removed on every way out: a plugin removed, a track removed, the session destroyed
        Session r;
        test::addTestPlugins (r.catalog);
        r.addEmptyTrack ("R");
        r.addEmptyTrack ("R2");
        test::check (r.setChain (0, { "test gain", "test delay" }, err) && r.setChain (1, { "test delay" }, err), "two chains to take apart");
        test::check (r.removePlugin (0, 0, err) && r.removeTrack (1, err) && r.tracks.size() == 1, "a plugin and a track are removed");
        test::check (r.addMonitorPlugin ("test gain", -1, err) && r.monitor.size() == 1, "the monitor chain takes a plugin too");
        // the session goes out of scope with its listening plugins: their destructors remove the listener first (no crash, the checks reach the end)
    }
    test::check (true, "a session with listening plugins was destroyed");
}

// [39] Settings: written through a temporary file (no half-written settings.xml left); a file that is not settings is kept once as .bad.
void settingsFileChecks()
{
    std::printf ("[39] Settings: the file is written whole, and a damaged settings file is kept once as settings.xml.bad\n");
    const auto dir = tempFolder().getChildFile ("settings-safe");
    dir.deleteRecursively();
    const auto file = dir.getChildFile ("settings.xml");
    Settings good;
    good.lastProject = "/tmp/song.olympus";
    good.extraLatencySamples = 128;
    good.save (file);
    const auto back = Settings::load (file);
    test::check (back.lastProject == "/tmp/song.olympus" && back.extraLatencySamples == 128, "the settings are written and read back");
    test::check (dir.getNumberOfChildFiles (juce::File::findFiles, "*") == 1, "and no temporary file is left beside them");

    file.replaceWithText ("this is not XML <<");
    const auto broken = Settings::load (file);
    const auto bad = dir.getChildFile ("settings.xml.bad");
    test::check (broken.lastProject.isEmpty() && bad.existsAsFile() && bad.loadFileAsString() == "this is not XML <<", "a damaged file is read as no settings, and kept as settings.xml.bad");
    file.replaceWithText ("second damage");
    (void) Settings::load (file);
    test::check (bad.loadFileAsString() == "this is not XML <<", "a second damaged file does not replace the first copy");
}

// [41] Takes and the MCP track name: a take the timeline cannot take does not stop the others (the first reason is reported, the files stay);
// set_track with a blank name is refused before anything is applied (no setting changes, no undo step).
void takeAndNameChecks()
{
    std::printf ("[41] A take that cannot be placed does not stop the others, and the first reason is reported; set_track with a blank name changes nothing and pushes no undo step\n");
    tempFolder().getChildFile ("takes41").createDirectory();
    const auto good = makeWav ("takes41/good.wav", 44100.0, 2, 44100, 0.5f, 440.0);
    const auto missing = tempFolder().getChildFile ("takes41/missing.wav");   // never written: addClip cannot read it
    juce::String err;
    Session s;
    s.addEmptyTrack ("A");
    s.addEmptyTrack ("B");
    Editor ed (s);
    juce::String first;
    test::check (! ed.addClip (0, missing, 0, 0, 44100, first).valid() && first.isNotEmpty(), "a missing file cannot be added as a clip, and the reason is given");

    const std::vector<Recorder::Finished> takes { { 0, missing, 44100 }, { 1, good, 44100 } };
    juce::String problem;
    test::check (! finishRecording (takes, 0, 0, ed, problem) && s.tracks[1]->clips.size() == 1 && s.tracks[0]->clips.empty(),
                 "the first take fails, and the second one still lands on its track as a clip");
    test::check (problem == first, "the reason returned is the first take's, and the take's file is kept");
    test::check (missing.getParentDirectory().getNumberOfChildFiles (juce::File::findFiles) == 1 && good.existsAsFile(), "the files stay on disk (the good take's file is there)");

    // the first reason is kept when a later take fails for another reason too (a track that is not there)
    juce::String again;
    const std::vector<Recorder::Finished> more { { 0, missing, 44100 }, { 7, good, 44100 } };
    test::check (! finishRecording (more, 0, 0, ed, again) && again == first, "a later failure does not replace the first reason");

    // MCP set_track: a blank name is refused before the fader and pan are applied, and pushes no undo step
    Session m;
    m.addTrack (good, "M", err);
    McpServer server (m);
    const auto call = [&server] (const char* tool, const juce::String& args) { return toolCall (server, tool, args); };
    const auto failed = [] (const juce::var& r) { return (bool) r["isError"]; };
    const float gainBefore = m.tracks[0]->gainDb, panBefore = m.tracks[0]->pan;
    const auto blank = call ("set_track", "{\"track\":0,\"gain_db\":-6,\"pan\":0.5,\"name\":\"   \"}");
    test::check (failed (blank) && toolText (blank).isNotEmpty(), "set_track with a blank name is an error result with a reason");
    test::check (m.tracks[0]->gainDb == gainBefore && m.tracks[0]->pan == panBefore && m.tracks[0]->name == "M",
                 "and the fader, pan and name are unchanged");
    test::check (failed (call ("undo", "{}")) && toolText (call ("undo", "{}")).contains ("Nothing to undo"), "and no undo step was pushed");
    missing.getParentDirectory().deleteRecursively();
}

// [42] The clipboard (the Editor): a copy pastes an identical clip on the chosen track, at the place asked; a paste and a cut are each one
// undo step; a refusal changes nothing and says why; the clipboard survives an undo and a change of rate refuses a paste.
void clipboardChecks()
{
    std::printf ("[42] Clipboard: a copy pastes an identical clip on the chosen track (one undo step); a cut is one step; refusals change nothing; the clipboard survives undo; a paste at another rate is refused\n");
    tempFolder().getChildFile ("clip42").createDirectory();
    const auto wav = makeWav ("clip42/a.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    s.addTrack (wav, "A", err);
    s.addEmptyTrack ("B");
    s.tracks[0]->clips[0].start = 100;
    s.tracks[0]->clips[0].offset = 300;
    s.tracks[0]->clips[0].length = 600;
    s.tracks[0]->clips[0].gainDb = -3.0f;
    s.tracks[0]->clips[0].fadeIn = 50;
    s.tracks[0]->clips[0].fadeOut = 60;
    s.tracks[0]->clips[0].mute = true;
    const auto src = s.tracks[0]->clips[0];
    Editor ed (s);

    test::check (! ed.hasClipboard() && ! ed.pasteClip (1, 0, err).valid() && err.isNotEmpty(), "a paste with an empty clipboard is refused, with the reason");
    test::check (! ed.copyClip ({ 7, 0 }, err) && ! ed.copyClip ({ 0, 9 }, err) && ! ed.hasClipboard(), "a copy of a clip or a track that is not there is refused");
    test::check (ed.copyClip ({ 0, 0 }, err) && ed.hasClipboard() && ed.clipboardTrack() == 0, "a copy fills the clipboard, and records the track it came from");
    test::check (s.tracks[0]->clips.size() == 1 && s.tracks[0]->clips[0].start == 100 && ! ed.canUndo(), "and a copy changes nothing in the session and makes no undo step");

    const auto pasted = ed.pasteClip (1, 5000, err);
    const auto& dst = s.tracks[1]->clips;
    test::check (pasted.track == 1 && pasted.clip == 0 && dst.size() == 1, "a paste puts the clip on the chosen track and returns it");
    const auto& c = dst[0];
    test::check (c.audio.get() == src.audio.get() && c.start == 5000 && c.offset == 300 && c.length == 600, "the copy shares the audio, and keeps the place in the file and the length");
    test::check (c.gainDb == -3.0f && c.fadeIn == 50 && c.fadeOut == 60 && c.mute && c.name == src.name && c.file == src.file, "and its gain, fades, mute, name and file");
    test::check (sameSamples (mixOf (s, 0, 100, 600), mixOf (s, 1, 5000, 600), 0, 600), "the pasted clip plays exactly what the original plays");
    test::check (ed.undoLabel() == "Paste clip" && ed.undo() && s.tracks[1]->clips.empty(), "the paste is one undo step, and undo removes it");
    test::check (ed.hasClipboard() && ed.redo() && s.tracks[1]->clips.size() == 1, "the clipboard survives the undo, and redo brings the clip back");

    // a cut: copy and delete as one step; undo brings the clip back where it was; a paste then puts it elsewhere
    test::check (ed.cutClip ({ 0, 0 }, err) && s.tracks[0]->clips.empty() && ed.undoLabel() == "Cut clip", "a cut removes the clip, as one undo step labelled so");
    test::check (ed.undo() && s.tracks[0]->clips.size() == 1 && s.tracks[0]->clips[0].start == 100, "undo brings the cut clip back where it was");
    test::check (ed.cutClip ({ 0, 0 }, err) && ed.clipboardTrack() == 0 && ed.pasteClip (1, 0, err).valid() && s.tracks[1]->clips.size() == 2,
                 "a cut, then a paste on the other track, puts the clip there");
    test::check (s.tracks[1]->clips.back().start == 0 && s.tracks[1]->clips.back().audio.get() == src.audio.get(), "the restored clip is at the new place with the same audio");

    // refusals change nothing: a track that is not there, a place before 0, and the undo label
    const auto label = ed.undoLabel();
    const auto before = s.tracks[1]->clips.size();
    test::check (! ed.pasteClip (9, 0, err).valid() && ! ed.pasteClip (1, -5, err).valid() && err.isNotEmpty(), "a track that is not there and a place before 0 are refused, with the reason");
    test::check (s.tracks[1]->clips.size() == before && ed.undoLabel() == label, "and the refused pastes change nothing and make no undo step");

    // a change of rate: the copy is at the old rate, so a paste is refused until the clip is copied again
    test::check (s.adoptRate (96000.0, err) && ! ed.pasteClip (1, 0, err).valid() && err.containsIgnoreCase ("rate"), "a paste after the session changed its rate is refused, with the reason");
    ed.clearClipboard();
    test::check (! ed.hasClipboard() && ed.clipboardTrack() == -1 && ! ed.pasteClip (1, 0, err).valid(), "clearing the clipboard empties it");
}

namespace
{
struct SpyState
{
    std::atomic<int> inProcess { 0 };
    std::atomic<int> inState { 0 };
    std::atomic<int> raceDetected { 0 };
    std::atomic<int> processCalls { 0 };
    std::atomic<int> stateCalls { 0 };
    std::atomic<bool> blockInProcess { false };
    std::atomic<bool> enteredProcess { false };
};

class SpyPlugin : public test::GainPlugin
{
public:
    SpyPlugin (const juce::String& pluginName, std::shared_ptr<SpyState> s)
        : test::GainPlugin (pluginName, 0), state (std::move (s)) {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
    {
        state->inProcess.fetch_add (1);
        if (state->inState.load() != 0)
            state->raceDetected.fetch_add (1);
        state->processCalls.fetch_add (1);
        if (state->blockInProcess.load())
        {
            state->enteredProcess.store (true);
            for (int ms = 0; ms < 2000 && state->blockInProcess.load(); ++ms)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        test::GainPlugin::processBlock (b, m);
        state->inProcess.fetch_sub (1);
    }
    using test::GainPlugin::processBlock;

    void getStateInformation (juce::MemoryBlock& m) override
    {
        state->inState.fetch_add (1);
        if (state->inProcess.load() != 0)
            state->raceDetected.fetch_add (1);
        state->stateCalls.fetch_add (1);
        std::this_thread::yield();
        test::GainPlugin::getStateInformation (m);
        state->inState.fetch_sub (1);
    }

private:
    std::shared_ptr<SpyState> state;
};

juce::File writeRawWav (const juce::String& name, juce::uint32 sampleRate, juce::uint32 claimedDataBytes, juce::int64 actualPayloadBytes = -1)
{
    auto file = tempFolder().getChildFile (name);
    file.deleteFile();
    juce::FileOutputStream out (file);
    out.write ("RIFF", 4);
    out.writeInt ((int) (36 + claimedDataBytes));
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    out.writeInt (16);
    out.writeShort (1); // PCM
    out.writeShort (2); // channels
    out.writeInt ((int) sampleRate);
    out.writeInt ((int) (sampleRate * 4));
    out.writeShort (4); // block align
    out.writeShort (16); // bits per sample
    out.write ("data", 4);
    out.writeInt ((int) claimedDataBytes);
    const juce::int64 toWrite = actualPayloadBytes < 0 ? (juce::int64) claimedDataBytes : actualPayloadBytes;
    for (juce::int64 i = 0; i < toWrite; ++i)
        out.writeByte (0);
    out.flush();
    return file;
}

juce::File writeTruncatedAiff (const juce::String& name, juce::uint32 claimedSamples)
{
    auto file = tempFolder().getChildFile (name);
    file.deleteFile();
    juce::FileOutputStream out (file);
    const juce::uint32 claimedAudioBytes = claimedSamples * 4;
    out.write ("FORM", 4);
    out.writeIntBigEndian ((int) (46 + claimedAudioBytes));
    out.write ("AIFF", 4);
    out.write ("COMM", 4);
    out.writeIntBigEndian (18);
    out.writeShortBigEndian (2); // channels
    out.writeIntBigEndian ((int) claimedSamples);
    out.writeShortBigEndian (16); // bits per sample
    // 80-bit IEEE 754 extended for 48000 Hz: 0x400E, 0xBB80000000000000
    const unsigned char sr48k[10] = { 0x40, 0x0e, 0xbb, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    out.write (sr48k, 10);
    out.write ("SSND", 4);
    out.writeIntBigEndian ((int) (claimedAudioBytes + 8));
    out.writeIntBigEndian (0); // offset
    out.writeIntBigEndian (0); // blockSize
    out.flush();
    return file;
}
} // namespace

// [43] R2.2: MCP copy_chain null-checks to_tracks and rejects non-array or empty to_tracks before touching state or undo.
void mcpCopyChainChecks()
{
    std::printf ("[43] MCP copy_chain: missing, non-array, empty or invalid to_tracks fails without touching state or undo\n");
    tempFolder().getChildFile ("mcp-copy").createDirectory();
    const auto wav = makeWav ("mcp-copy/a.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    s.addTrack (wav, "A", err);
    s.addTrack (wav, "B", err);
    s.setChain (0, { "test gain" }, err);
    setGain (s, 0, 0, -6.0);
    s.setChain (1, { "test delay" }, err);

    McpServer server (s);
    const auto call = [&server] (const char* tool, const juce::String& args) { return toolCall (server, tool, args); };
    const auto failed = [] (const juce::var& r) { return (bool) r["isError"]; };
    test::check (! failed (call ("set_master", "{\"master_db\":-1}")), "seed undo history with a master edit");

    for (const auto* badCopy : { "{\"from_track\":0}",
                                 "{\"from_track\":0,\"to_tracks\":null}",
                                 "{\"from_track\":0,\"to_tracks\":\"1\"}",
                                 "{\"from_track\":0,\"to_tracks\":1}",
                                 "{\"from_track\":0,\"to_tracks\":true}",
                                 "{\"from_track\":0,\"to_tracks\":{}}",
                                 "{\"from_track\":0,\"to_tracks\":[]}",
                                 "{\"from_track\":0,\"to_tracks\":[1,\"\"]}",
                                 "{\"from_track\":0,\"to_tracks\":[1,null]}",
                                 "{\"from_track\":0,\"to_tracks\":[1,99]}" })
    {
        const auto r = call ("copy_chain", badCopy);
        test::check (failed (r) && toolText (r).isNotEmpty(), juce::String ("copy_chain refused: ") + badCopy);
        test::check (s.tracks[1]->chain.size() == 1 && s.tracks[1]->chain[0]->plugin == "Test Delay", "copy_chain error left target chain untouched");
    }
    test::check (! failed (call ("undo", "{}")) && std::abs (s.masterDb) < 1e-4f, "failed copy_chain did not clear undo");

    test::check (! failed (call ("copy_chain", "{\"from_track\":0,\"to_tracks\":[1]}")) && s.tracks[1]->chain.size() == 1
                     && s.tracks[1]->chain[0]->plugin == "Test Gain" && std::abs (gainOf (s, 1, 0) + 6.0) < 0.01,
                 "valid copy_chain copies chain and state");
    tempFolder().getChildFile ("mcp-copy").deleteRecursively();
}

// [44] R2.7: Session::copyChain holds Session::lock while snapshotting chain/state, releases lock across makeSlot, and deduplicates targets.
void copyChainLockChecks()
{
    std::printf ("[44] Session::copyChain: lock protects getStateInformation vs processBlock without blocking audio during makeSlot\n");
    tempFolder().getChildFile ("copy-lock").createDirectory();
    const auto wav = makeWav ("copy-lock/a.wav", 48000.0, 2, 4800, 0.5f, 440.0);

    // Part A: deterministic barrier proving copyChain waits for Session::lock before calling getStateInformation
    {
        auto spy = std::make_shared<SpyState>();
        Session s;
        s.catalog.addInternal (test::GainPlugin::description ("Spy Plugin"), [spy] (double, int)
                               { return std::make_unique<SpyPlugin> ("Spy Plugin", spy); });
        juce::String err;
        s.addTrack (wav, "T0", err);
        s.addTrack (wav, "T1", err);
        test::check (s.setChain (0, { "Spy Plugin" }, err), "spy plugin loaded on track 0");

        spy->blockInProcess.store (true);
        std::thread audioThread ([&]
        {
            juce::AudioBuffer<float> buf (2, kBlockSize);
            s.processBlock (buf, 0);
        });
        for (int ms = 0; ms < 2000 && ! spy->enteredProcess.load(); ++ms)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        test::check (spy->enteredProcess.load(), "audio thread entered processBlock");

        bool copyOk = false;
        juce::String copyErr;
        std::thread copyThread ([&] { copyOk = s.copyChain (0, { 1 }, copyErr); });
        std::this_thread::sleep_for (std::chrono::milliseconds (15));
        test::check (spy->stateCalls.load() == 0, "copyChain waits for Session::lock before reading getStateInformation");

        spy->blockInProcess.store (false);
        audioThread.join();
        copyThread.join();
        test::check (copyOk && spy->raceDetected.load() == 0 && spy->stateCalls.load() == 1 && s.tracks[1]->chain.size() == 1,
                     "after lock release copyChain completes with zero raceDetected");
    }

    // Part B: slow makeSlot does not hold Session::lock, plus concurrent stress with duplicate targets
    {
        auto spy = std::make_shared<SpyState>();
        auto slowMade = std::make_shared<std::atomic<bool>> (false);
        auto audioRanDuringMakeSlot = std::make_shared<std::atomic<bool>> (false);
        Session s;
        s.catalog.addInternal (test::GainPlugin::description ("Spy Plugin"), [spy] (double, int)
                               { return std::make_unique<SpyPlugin> ("Spy Plugin", spy); });
        s.catalog.addInternal (test::GainPlugin::description ("Slow Spy"), [spy, slowMade, audioRanDuringMakeSlot] (double, int)
                               {
                                   if (slowMade->exchange (true))
                                   {
                                       const int startCalls = spy->processCalls.load();
                                       for (int ms = 0; ms < 500; ++ms)
                                       {
                                           if (spy->processCalls.load() > startCalls)
                                           {
                                               audioRanDuringMakeSlot->store (true);
                                               break;
                                           }
                                           std::this_thread::sleep_for (std::chrono::milliseconds (1));
                                       }
                                   }
                                   return std::make_unique<SpyPlugin> ("Slow Spy", spy);
                               });
        juce::String err;
        s.addTrack (wav, "T0", err);
        s.addTrack (wav, "T1", err);
        s.addTrack (wav, "T2", err);
        test::check (s.setChain (0, { "Slow Spy" }, err), "slow spy loaded on track 0");

        std::atomic<bool> stopAudio { false };
        std::thread audioThread ([&]
        {
            juce::AudioBuffer<float> buf (2, kBlockSize);
            while (! stopAudio.load())
            {
                s.processBlock (buf, 0);
                std::this_thread::sleep_for (std::chrono::microseconds (200));
            }
        });
        for (int ms = 0; ms < 500 && spy->processCalls.load() == 0; ++ms)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));

        test::check (s.copyChain (0, { 1 }, err), "copyChain with Slow Spy succeeds");
        test::check (audioRanDuringMakeSlot->load(), "processBlock continues running while makeSlot runs outside Session::lock");

        test::check (s.setChain (0, { "Spy Plugin" }, err), "switch track 0 to Spy Plugin for stress loop");
        for (int round = 0; round < 20; ++round)
            test::check (s.copyChain (0, { 1, 1, 2 }, err), "concurrent copyChain with duplicate targets succeeds");

        stopAudio.store (true);
        audioThread.join();
        test::check (spy->raceDetected.load() == 0 && s.tracks[1]->chain.size() == 1 && s.tracks[2]->chain.size() == 1,
                     "concurrent processBlock + copyChain finishes with raceDetected == 0 and deduplicated target chains");
    }
    tempFolder().getChildFile ("copy-lock").deleteRecursively();
}

// [45] R2.8: Session::processBlock ignores n <= 0 and chunks n > kBlockSize identically to kBlockSize slices.
void processBlockChunkChecks()
{
    std::printf ("[45] Session::processBlock: n <= 0 is a no-op and n > kBlockSize matches kBlockSize chunking bitwise\n");
    tempFolder().getChildFile ("pb-chunk").createDirectory();
    const auto imp = makeWav ("pb-chunk/imp.wav", 48000.0, 2, 4800, 0.5f, 0.0, 300);

    auto setupSession = [&] (Session& s)
    {
        juce::String err;
        test::addTestPlugins (s.catalog);
        s.addTrack (imp, "Dry", err);
        s.addTrack (imp, "Wet", err);
        s.addTrack (imp, "Muted", err);
        s.setChain (1, { "test gain", "test delay" }, err);
        setGain (s, 1, 0, -3.0);
        s.setChain (2, { "test delay" }, err);
        // Prime Track 2's delayDirty while unmuted, then mute it
        juce::AudioBuffer<float> prime (2, 16);
        s.processBlock (prime, 0);
        s.tracks[0]->peakL = 0.0f; s.tracks[0]->peakR = 0.0f;
        s.tracks[1]->peakL = 0.0f; s.tracks[1]->peakR = 0.0f;
        s.tracks[2]->peakL = 0.0f; s.tracks[2]->peakR = 0.0f;
        s.masterPeakL = 0.0f; s.masterPeakR = 0.0f;
        s.tracks[2]->mute = true;
        s.metronomeOn = true;
        s.masterDb = -3.0f;
        s.loopStart = 0;
        s.loopEnd = 96000;
        s.loopOn = true;
        s.prepare (false);
    };

    // n <= 0 check
    {
        Session s;
        setupSession (s);
        s.head.set (1234, false);
        juce::AudioBuffer<float> empty (2, 0);
        s.processBlock (empty, 0);
        test::check (s.head.getPosition()->getTimeInSamples().orFallback (-1) == 1234
                         && s.tracks[0]->peakL.load() == 0.0f && s.masterPeakL.load() == 0.0f,
                     "processBlock with n == 0 leaves head and peaks untouched");
    }

    // n > kBlockSize bitwise equivalence check
    Session sBig, sChunked;
    setupSession (sBig);
    setupSession (sChunked);

    juce::AudioBuffer<float> big1 (2, 1300), big2 (2, 700);
    sBig.processBlock (big1, 0);
    sBig.processBlock (big2, 1300);

    juce::AudioBuffer<float> chk1 (2, 1300), chk2 (2, 700);
    const auto runChunked = [&] (juce::AudioBuffer<float>& dest, juce::int64 start)
    {
        const int total = dest.getNumSamples();
        for (int off = 0; off < total; off += kBlockSize)
        {
            const int cnt = juce::jmin (kBlockSize, total - off);
            juce::AudioBuffer<float> slice (2, cnt);
            sChunked.processBlock (slice, start + off);
            for (int ch = 0; ch < 2; ++ch)
                dest.copyFrom (ch, off, slice, ch, 0, cnt);
        }
    };
    runChunked (chk1, 0);
    runChunked (chk2, 1300);

    test::check (sameAudio (big1, chk1) && sameAudio (big2, chk2), "oversized processBlock matches kBlockSize slices bitwise");
    test::check (sBig.tracks[0]->peakL.load() == sChunked.tracks[0]->peakL.load()
                     && sBig.tracks[1]->peakL.load() == sChunked.tracks[1]->peakL.load()
                     && sBig.masterPeakL.load() == sChunked.masterPeakL.load()
                     && sBig.masterPeakR.load() == sChunked.masterPeakR.load(),
                 "track and master peaks match chunked runs");
    test::check (sBig.head.getPosition()->getTimeInSamples().orFallback (-1) == sChunked.head.getPosition()->getTimeInSamples().orFallback (-1)
                     && close (sBig.head.getPosition()->getPpqPosition().orFallback (-1.0), sChunked.head.getPosition()->getPpqPosition().orFallback (-2.0)),
                 "final playhead position and ppq match chunked runs");
    test::check (! sBig.tracks[2]->delayDirty && sBig.tracks[2]->delayPos == 0, "muted track cleared delayDirty on oversized block");
    tempFolder().getChildFile ("pb-chunk").deleteRecursively();
}

// [46] R3.12: Recorder::push latches recording and holds busy across the entire multi-chunk call.
void recorderChunkAtomicityChecks()
{
    std::printf ("[46] Recorder::push: busy and latched recording keep frames a multiple of block and align sample 0 to first true\n");
    const auto dir = tempFolder().getChildFile ("rec-atom");
    dir.deleteRecursively();
    dir.createDirectory();

    for (int cycle = 0; cycle < 10; ++cycle)
    {
        Recorder rec;
        rec.prepare (48000.0, 64); // 256 = 4 * blockMax
        Session s;
        s.addEmptyTrack ("T");
        s.tracks[0]->armed = true;

        std::atomic<bool> stopWorker { false };
        std::atomic<int> blocksPushed { 0 };
        std::atomic<int> firstTrueBlock { -1 };

        std::thread audioThread ([&]
        {
            std::vector<float> l (256), r (256);
            int idx = 0;
            while (! stopWorker.load())
            {
                const float tag = (float) (idx + 1);
                std::fill (l.begin(), l.end(), tag);
                std::fill (r.begin(), r.end(), tag);
                const float* ptrs[2] = { l.data(), r.data() };
                if (rec.push (ptrs, 2, 256))
                {
                    int expected = -1;
                    firstTrueBlock.compare_exchange_strong (expected, idx);
                }
                ++idx;
                blocksPushed.fetch_add (1);
                std::this_thread::sleep_for (std::chrono::microseconds (150));
            }
        });

        while (blocksPushed.load() < 5)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));

        juce::String err;
        test::check (beginRecording (rec, s, dir, 48000.0, err), "beginRecording succeeds on cycle " + juce::String (cycle));
        const int atStart = blocksPushed.load();
        while (blocksPushed.load() < atStart + 10)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));

        const auto done = rec.stop();
        stopWorker.store (true);
        audioThread.join();

        const int firstIdx = firstTrueBlock.load();
        test::check (firstIdx >= 0, "push returned true for the first recorded block");
        test::check (done.size() == 1 && done[0].frames == rec.framesRecorded(), "finished lane frames match framesRecorded()");
        test::check (done[0].frames > 0 && done[0].frames % 256 == 0,
                     test::fmt ("recorded frames (%lld) is a positive multiple of 256 (no mid-block tearing)", (long long) done[0].frames));

        const auto wav = readWav (done[0].file);
        test::check (wav.getNumSamples() == (int) done[0].frames, "WAV sample count matches finished frames");
        bool aligned = true;
        const int numBlocks = wav.getNumSamples() / 256;
        for (int k = 0; k < numBlocks && aligned; ++k)
        {
            const float expectedVal = (float) (firstIdx + k + 1);
            for (int i = 0; i < 256; ++i)
            {
                if (std::abs (wav.getSample (0, k * 256 + i) - expectedVal) > 1e-5f)
                {
                    aligned = false;
                    break;
                }
            }
        }
        test::check (aligned, "WAV sample 0 aligns identically with firstTrueBlock and every 256-sample chunk is intact");
    }
    dir.deleteRecursively();
}

// [47] R3.13: MCP set_chain rejects missing, non-array, or non-string plugins while preserving explicit [].
void mcpSetChainChecks()
{
    std::printf ("[47] MCP set_chain: missing, non-array or non-string plugins fails without wiping chain or undo; [] clears\n");
    tempFolder().getChildFile ("mcp-setchain").createDirectory();
    const auto wav = makeWav ("mcp-setchain/a.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    s.addTrack (wav, "A", err);
    s.setChain (0, { "test gain" }, err);

    McpServer server (s);
    const auto call = [&server] (const char* tool, const juce::String& args) { return toolCall (server, tool, args); };
    const auto failed = [] (const juce::var& r) { return (bool) r["isError"]; };
    test::check (! failed (call ("set_master", "{\"master_db\":-1}")), "seed undo history");

    for (const auto* badChain : { "{\"track\":0}",
                                  "{\"track\":0,\"plugins\":null}",
                                  "{\"track\":0,\"plugins\":\"Test Gain\"}",
                                  "{\"track\":0,\"plugins\":123}",
                                  "{\"track\":0,\"plugins\":true}",
                                  "{\"track\":0,\"plugins\":{}}",
                                  "{\"track\":0,\"plugins\":[123]}",
                                  "{\"track\":0,\"plugins\":[null]}",
                                  "{\"track\":0,\"plugins\":[false]}",
                                  "{\"track\":0,\"plugins\":[\"Test Gain\",{}]}" })
    {
        const auto r = call ("set_chain", badChain);
        test::check (failed (r) && toolText (r).isNotEmpty(), juce::String ("set_chain refused: ") + badChain);
        test::check (s.tracks[0]->chain.size() == 1 && s.tracks[0]->chain[0]->plugin == "Test Gain", "set_chain error left track chain intact");
    }
    test::check (! failed (call ("undo", "{}")) && std::abs (s.masterDb) < 1e-4f, "failed set_chain did not clear undo");

    test::check (! failed (call ("set_chain", "{\"track\":0,\"plugins\":[]}")) && s.tracks[0]->chain.empty(),
                 "set_chain with explicit [] clears the chain");
    tempFolder().getChildFile ("mcp-setchain").deleteRecursively();
}

// [48] R3.14: findTrack, findSlot, indexArg, move_plugin and related MCP parsers reject blank/missing/non-numeric/overflow args.
void mcpIndexValidationChecks()
{
    std::printf ("[48] MCP validation: findTrack, findSlot, move_plugin, indexArg and sibling parsers reject coercible args\n");
    tempFolder().getChildFile ("mcp-idx").createDirectory();
    const auto wav = makeWav ("mcp-idx/a.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    s.addTrack (wav, "A", err);
    s.addTrack (wav, "B", err);
    s.addEmptyTrack (""); // ensure whitespace-only "   " never matches an empty-named track

    McpServer server (s);
    const auto call = [&server] (const char* tool, const juce::String& args) { return toolCall (server, tool, args); };
    const auto failed = [] (const juce::var& r) { return (bool) r["isError"]; };

    // Direct Session::findTrack checks
    test::check (s.findTrack (0, err) == 0 && s.findTrack ("0", err) == 0 && s.findTrack (" 1 ", err) == 1
                     && s.findTrack ("b", err) == 1 && s.findTrack (1.0, err) == 1,
                 "findTrack accepts valid int, digit string, name and integral double");
    for (const auto& bad : { juce::var(), juce::var (false), juce::var (true), juce::var (""), juce::var ("   "),
                             juce::var (0.5), juce::var (-0.5), juce::var (std::numeric_limits<double>::quiet_NaN()),
                             juce::var (std::numeric_limits<double>::infinity()),
                             juce::var ((juce::int64) 4294967296LL), juce::var ("4294967296"),
                             juce::var ("99999999999999999999"), juce::var ("-1"),
                             juce::var (juce::Array<juce::var>()), juce::var (new juce::DynamicObject()) })
        test::check (s.findTrack (bad, err) < 0 && err.isNotEmpty(), "findTrack rejects invalid/coercible var");

    // remove_track, move_clip to_track, delete_clip indexArg
    for (const char* badTrack : { "", "   ", "4294967296" })
        test::check (failed (call ("remove_track", "{\"track\":\"" + juce::String (badTrack) + "\"}")) && s.tracks.size() == 3,
                     "remove_track rejects blank/overflow string");
    for (const char* badJson : { "{\"track\":null}", "{\"track\":false}", "{\"track\":true}", "{\"track\":0.5}",
                                 "{\"track\":{}}", "{\"track\":[]}", "{\"track\":4294967296}" })
        test::check (failed (call ("remove_track", badJson)) && s.tracks.size() == 3, juce::String ("remove_track rejects ") + badJson);
    test::check (failed (call ("move_clip", "{\"track\":1,\"clip\":0,\"to_track\":null,\"start_seconds\":0}")) && s.tracks[1]->clips.size() == 1,
                 "move_clip rejects null to_track");
    test::check (failed (call ("delete_clip", "{\"track\":0,\"clip\":0.5}")) && failed (call ("delete_clip", "{\"track\":0,\"clip\":4294967296}"))
                     && s.tracks[0]->clips.size() == 1,
                 "indexArg rejects float and int64 overflow");

    // set_track empty name no-op preserved
    test::check (! failed (call ("set_track", "{\"track\":0,\"name\":\"\"}")) && s.tracks[0]->name == "A",
                 "set_track empty name remains a valid no-op");

    // move_plugin, findSlot, add_plugin position, bypass_plugin, set_parameter, load_preset
    test::check (! failed (call ("set_chain", "{\"track\":0,\"plugins\":[\"Test Gain\",\"Test Delay\"]}")), "set two plugins for slot checks");
    for (const char* badMove : { "{\"track\":0,\"from\":1}",
                                 "{\"track\":0,\"from\":0,\"to\":null}",
                                 "{\"track\":0,\"from\":0,\"to\":false}",
                                 "{\"track\":0,\"from\":0,\"to\":\"\"}",
                                 "{\"track\":0,\"from\":0,\"to\":\"1\"}",
                                 "{\"track\":0,\"from\":0,\"to\":0.5}",
                                 "{\"track\":0,\"from\":0,\"to\":4294967296}",
                                 "{\"track\":0,\"from\":-1,\"to\":1}" })
    {
        test::check (failed (call ("move_plugin", badMove)) && s.tracks[0]->chain[0]->plugin == "Test Gain",
                     juce::String ("move_plugin rejects ") + badMove);
    }
    test::check (! failed (call ("move_plugin", "{\"track\":0,\"from\":0,\"to\":1}")) && s.tracks[0]->chain[0]->plugin == "Test Delay",
                 "valid move_plugin reorders chain");

    for (const char* badSlot : { "{\"track\":0}",
                                 "{\"track\":0,\"slot\":null}",
                                 "{\"track\":0,\"slot\":false}",
                                 "{\"track\":0,\"slot\":true}",
                                 "{\"track\":0,\"slot\":\"\"}",
                                 "{\"track\":0,\"slot\":\"   \"}",
                                 "{\"track\":0,\"slot\":0.5}",
                                 "{\"track\":0,\"slot\":4294967296}",
                                 "{\"track\":0,\"slot\":\"4294967296\"}",
                                 "{\"track\":0,\"slot\":{}}" })
    {
        test::check (failed (call ("remove_plugin", badSlot)) && s.tracks[0]->chain.size() == 2,
                     juce::String ("remove_plugin rejects ") + badSlot);
        test::check (failed (call ("list_parameters", badSlot)), juce::String ("list_parameters rejects ") + badSlot);
        test::check (failed (call ("list_presets", badSlot)), juce::String ("list_presets rejects ") + badSlot);
    }
    test::check (failed (call ("bypass_plugin", "{\"track\":0,\"slot\":\"   \",\"bypass\":true}")) && ! s.tracks[0]->chain[0]->bypass,
                 "bypass_plugin rejects blank slot");
    test::check (failed (call ("bypass_plugin", "{\"track\":0,\"slot\":0,\"bypass\":null}")) && ! s.tracks[0]->chain[0]->bypass,
                 "bypass_plugin rejects non-bool bypass");
    test::check (failed (call ("add_plugin", "{\"track\":0,\"plugin\":\"Test Gain\",\"position\":null}"))
                     && failed (call ("add_plugin", "{\"track\":0,\"plugin\":\"Test Gain\",\"position\":0.5}"))
                     && failed (call ("add_plugin", "{\"track\":0,\"plugin\":\"Test Gain\",\"position\":false}"))
                     && failed (call ("add_plugin", "{\"track\":0,\"plugin\":\"Test Gain\",\"position\":-1}"))
                     && failed (call ("add_plugin", "{\"track\":0,\"plugin\":true}"))
                     && s.tracks[0]->chain.size() == 2,
                 "add_plugin rejects bad position and non-string plugin");
    test::check (failed (call ("set_parameter", "{\"track\":0,\"slot\":1,\"parameter\":false,\"value\":0.1}"))
                     && failed (call ("set_parameter", "{\"track\":0,\"slot\":1,\"parameter\":-1.0,\"value\":0.1}"))
                     && failed (call ("set_parameter", "{\"track\":0,\"slot\":1,\"parameter\":\"4294967296\",\"value\":0.1}"))
                     && failed (call ("set_parameter", "{\"track\":0,\"slot\":1,\"parameter\":0,\"value\":null}"))
                     && failed (call ("set_parameter", "{\"track\":0,\"slot\":1,\"parameter\":0,\"text\":false}"))
                     && std::abs (gainOf (s, 0, 1)) < 0.01,
                 "set_parameter rejects boolean/negative-double/overflow parameter and non-numeric value/non-string text");
    test::check (failed (call ("load_preset", "{\"track\":0,\"slot\":1,\"preset\":false}"))
                     && failed (call ("load_preset", "{\"track\":0,\"slot\":1,\"preset\":true}"))
                     && failed (call ("load_preset", "{\"track\":0,\"slot\":1,\"preset\":-1.0}"))
                     && failed (call ("load_preset", "{\"track\":0,\"slot\":1,\"preset\":\"4294967296\"}"))
                     && std::abs (gainOf (s, 0, 1)) < 0.01,
                 "load_preset rejects boolean, negative double and overflow preset");
    test::check (! failed (call ("load_preset", "{\"track\":0,\"slot\":1,\"preset\":1.0}"))
                     && std::abs (gainOf (s, 0, 1) - 6.0) < 0.01,
                 "load_preset accepts non-negative integral double 1.0");
    test::check (! failed (call ("set_parameter", "{\"track\":0,\"slot\":1,\"parameter\":0.0,\"value\":0.5}"))
                     && std::abs (gainOf (s, 0, 1)) < 0.01,
                 "set_parameter accepts non-negative integral double 0.0");
    tempFolder().getChildFile ("mcp-idx").deleteRecursively();
}

// [49] R3.15: readClip path resolution, NaN/Inf/fade/loopOn guards in load(), and readAudio invalid sampleRate/length checks.
void projectValidationChecks()
{
    std::printf ("[49] Project validation: readClip paths, NaN/Inf/fade/loopOn on load, and readAudio sampleRate/length checks\n");
    const auto dir = tempFolder().getChildFile ("proj-val");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto wav = makeWav ("proj-val/rel.wav", 48000.0, 2, 4800, 0.5f, 440.0);
    const auto otherWav = makeWav ("proj-val/other.wav", 48000.0, 2, 4800, 0.5f, 220.0);
    juce::String err;

    // A: readClip path resolution
    {
        const auto p = dir.getChildFile ("paths.olympus");
        p.replaceWithText ("<OlympusProject version=\"2\" sampleRate=\"48000\">"
                           "<Track name=\"T1\">"
                           "<Clip name=\"c1\" file=\"missing.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"1000\"/>"
                           "<Clip name=\"c2\" file=\"missing2.wav\" path=\"sub/rel.wav\" start=\"0\" offset=\"0\" length=\"1000\"/>"
                           "<Clip name=\"c3\" file=\"\" path=\"\" start=\"0\" offset=\"0\" length=\"1000\"/>"
                           "<Clip name=\"c4\" file=\"rel.wav\" path=\"/no/such/file.wav\" start=\"0\" offset=\"0\" length=\"1000\"/>"
                           "<Clip name=\"c5\" file=\"missing.wav\" path=\"" + otherWav.getFullPathName().replace ("\"", "&quot;") + "\" start=\"0\" offset=\"0\" length=\"1000\"/>"
                           "</Track></OlympusProject>");
        Session s;
        test::check (s.load (p, err) && s.tracks.size() == 1 && s.tracks[0]->clips.size() == 5, "paths project loads");
        const auto& clips = s.tracks[0]->clips;
        test::check (clips[0].file == dir.getChildFile ("missing.wav"), "empty path attribute preserves relative file");
        test::check (clips[1].file == dir.getChildFile ("missing2.wav"), "non-absolute path attribute preserves relative file");
        test::check (clips[2].file == juce::File(), "both file and path empty gives empty juce::File()");
        test::check (clips[3].file == wav && clips[3].audio != nullptr, "existing relative file wins over missing path");
        test::check (clips[4].file == otherWav && clips[4].audio != nullptr, "existing absolute path used when relative is missing");
        test::check (s.save (dir.getChildFile ("paths-resaved.olympus"), err), "resave succeeds");
    }

    // B + C + D: NaN/Inf, fades, extreme rate, loopOn
    {
        const auto p = dir.getChildFile ("nan.olympus");
        p.replaceWithText ("<OlympusProject version=\"2\" sampleRate=\"nan\" masterDb=\"nan\" loopStart=\"1000\" loopEnd=\"1000\" loopOn=\"1\">"
                           "<Tempo beat=\"nan\" bpm=\"140\"/>"
                           "<Tempo beat=\"4\" bpm=\"nan\"/>"
                           "<Track name=\"T1\" gainDb=\"nan\" pan=\"inf\">"
                           "<Clip name=\"c1\" file=\"rel.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"4800\" gainDb=\"nan\" fadeIn=\"10000\" fadeOut=\"10000\"/>"
                           "<Clip name=\"c2\" file=\"rel.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"4800\" gainDb=\"0\" fadeIn=\"3000\" fadeOut=\"3000\"/>"
                           "<Clip name=\"c3\" file=\"gone.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"1000\" fadeIn=\"800\" fadeOut=\"800\"/>"
                           "</Track></OlympusProject>");
        Session s;
        test::check (s.load (p, err) && s.tracks.size() == 1 && s.tracks[0]->clips.size() == 3, "NaN project loads cleanly");
        test::check (std::isfinite (s.sampleRate) && std::abs (s.sampleRate - 48000.0) < 0.5, "NaN sampleRate recovers from valid clip rate");
        test::check (std::isfinite (s.masterDb) && s.masterDb == 0.0f, "NaN masterDb falls back to 0.0f");
        test::check (std::isfinite (s.tracks[0]->gainDb) && s.tracks[0]->gainDb == 0.0f
                         && std::isfinite (s.tracks[0]->pan) && s.tracks[0]->pan == 0.0f,
                     "NaN/Inf track gainDb and pan fall back to 0.0f");
        test::check (s.tempo.points.size() == 1 && s.tempo.points[0].beat == 0.0 && s.tempo.points[0].bpm == 120.0,
                     "NaN Tempo entries are dropped and default 120 bpm remains");
        const auto& clips = s.tracks[0]->clips;
        test::check (std::isfinite (clips[0].gainDb) && clips[0].gainDb == 0.0f
                         && clips[0].fadeIn == 4800 && clips[0].fadeOut == 0,
                     "NaN clip gainDb falls back to 0 and oversized fadeIn/fadeOut clamp to length");
        test::check (clips[1].fadeIn == 3000 && clips[1].fadeOut == 1800, "fadeIn + fadeOut clamped to <= length");
        test::check (clips[2].length == 1000 && clips[2].fadeIn == 800 && clips[2].fadeOut == 200, "missing clip fadeIn + fadeOut clamped to length");
        test::check (! s.loopOn, "loopOn disabled when loopEnd <= loopStart");

        // Extreme rate + valid loopOn + loopStart > loopEnd
        const auto p2 = dir.getChildFile ("extreme.olympus");
        p2.replaceWithText ("<OlympusProject version=\"2\" sampleRate=\"1e20\" masterDb=\"18\" loopStart=\"2000\" loopEnd=\"1000\" loopOn=\"1\">"
                            "<Track name=\"T\"><Clip name=\"m\" file=\"gone.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"1000\"/></Track>"
                            "</OlympusProject>");
        Session s2;
        test::check (s2.load (p2, err) && s2.tracks[0]->clips[0].length == 1000 && ! s2.loopOn
                         && std::abs (s2.masterDb - 12.0f) < 1e-4f && s2.warnings.joinIntoString ("\n").contains ("+12 dB"),
                     "extreme rate missing clip safe, loopEnd < loopStart clears loopOn, masterDb > 12 warns");

        const auto p3 = dir.getChildFile ("goodloop.olympus");
        p3.replaceWithText ("<OlympusProject version=\"2\" sampleRate=\"48000\" loopStart=\"1000\" loopEnd=\"2000\" loopOn=\"1\"/>");
        Session s3;
        test::check (s3.load (p3, err) && s3.loopOn, "valid loopStart < loopEnd keeps loopOn");
    }

    // E: readAudio validation (0 sample rate, 0 length, truncated-payload WAV/AIFF, and tiny valid WAV)
    {
        const auto zeroRate = writeRawWav ("proj-val/zero-rate.wav", 0, 16);
        const auto zeroLen = writeRawWav ("proj-val/zero-len.wav", 48000, 0);
        const auto spoof50M = writeRawWav ("proj-val/spoof-50m.wav", 48000, 200000000, 0);
        const auto spoof100 = writeRawWav ("proj-val/spoof-100.wav", 48000, 100, 0);
        const auto partialTrunc = writeRawWav ("proj-val/partial-trunc.wav", 48000, 4000, 100);
        const auto truncAiff = writeTruncatedAiff ("proj-val/trunc.aif", 50000000);
        const auto tinyValid = writeRawWav ("proj-val/tiny-valid.wav", 48000, 4, 4);
        Session s;
        test::check (! s.addTrack (zeroRate, "ZR", err) && err.isNotEmpty(), "readAudio rejects 0 sample rate WAV: " + err);
        test::check (! s.addTrack (zeroLen, "ZL", err) && err.isNotEmpty(), "readAudio rejects 0-sample WAV: " + err);
        test::check (! s.addTrack (spoof50M, "S50M", err) && err.isNotEmpty(), "readAudio rejects 44-byte WAV claiming 200MB payload: " + err);
        test::check (! s.addTrack (spoof100, "S100", err) && err.isNotEmpty(), "readAudio rejects 44-byte WAV claiming 100B payload: " + err);
        test::check (! s.addTrack (partialTrunc, "PT", err) && err.isNotEmpty(), "readAudio rejects partially truncated WAV: " + err);
        test::check (! s.addTrack (truncAiff, "AIFF", err) && err.isNotEmpty(), "readAudio rejects truncated AIFF: " + err);
        test::check (s.addTrack (tinyValid, "Tiny", err) && s.tracks.size() == 1 && s.tracks[0]->clips[0].length == 1,
                     "readAudio still loads a tiny valid 1-sample WAV");
    }

    // F: missing clip BEFORE a valid clip when sampleRate="nan" or omitted, and missing-only project with omitted sampleRate
    {
        const auto pNan = dir.getChildFile ("missing-first-nan.olympus");
        pNan.replaceWithText ("<OlympusProject version=\"2\" sampleRate=\"nan\">"
                              "<Track name=\"T1\">"
                              "<Clip name=\"m\" file=\"does_not_exist.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"48000\" fadeIn=\"500\" fadeOut=\"500\"/>"
                              "<Clip name=\"ok\" file=\"rel.wav\" path=\"\" start=\"48000\" offset=\"0\" length=\"4800\"/>"
                              "</Track></OlympusProject>");
        Session sNan;
        test::check (sNan.load (pNan, err) && std::abs (sNan.sampleRate - 48000.0) < 0.5
                         && sNan.tracks.size() == 1 && sNan.tracks[0]->clips.size() == 2
                         && sNan.tracks[0]->clips[0].audio == nullptr
                         && sNan.tracks[0]->clips[0].length == 48000
                         && sNan.tracks[0]->clips[0].fadeIn == 500 && sNan.tracks[0]->clips[0].fadeOut == 500,
                     "missing clip before valid clip with sampleRate=nan keeps saved length and fades");

        const auto pOmit = dir.getChildFile ("missing-first-omit.olympus");
        pOmit.replaceWithText ("<OlympusProject version=\"2\">"
                               "<Track name=\"T1\">"
                               "<Clip name=\"m\" file=\"does_not_exist.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"48000\" fadeIn=\"500\" fadeOut=\"500\"/>"
                               "<Clip name=\"ok\" file=\"rel.wav\" path=\"\" start=\"48000\" offset=\"0\" length=\"4800\"/>"
                               "</Track></OlympusProject>");
        Session sOmit;
        test::check (sOmit.load (pOmit, err) && std::abs (sOmit.sampleRate - 48000.0) < 0.5
                         && sOmit.tracks[0]->clips[0].length == 48000
                         && sOmit.tracks[0]->clips[0].fadeIn == 500 && sOmit.tracks[0]->clips[0].fadeOut == 500,
                     "missing clip before valid clip with omitted sampleRate keeps saved length and fades");

        const auto pOnly = dir.getChildFile ("missing-only-omit.olympus");
        pOnly.replaceWithText ("<OlympusProject version=\"2\">"
                               "<Track name=\"T1\">"
                               "<Clip name=\"m1\" file=\"missing1.wav\" path=\"\" start=\"0\" offset=\"0\" length=\"48000\" fadeIn=\"500\" fadeOut=\"500\"/>"
                               "<Clip name=\"m2\" file=\"missing2.wav\" path=\"\" start=\"48000\" offset=\"0\" length=\"2000000000\" fadeIn=\"400000000\" fadeOut=\"400000000\"/>"
                               "</Track></OlympusProject>");
        Session sOnly;
        const juce::int64 cap48k = (juce::int64) (48000.0 * 3600.0 * 2.0);
        test::check (sOnly.load (pOnly, err) && sOnly.tracks.size() == 1 && sOnly.tracks[0]->clips.size() == 2
                         && sOnly.tracks[0]->clips[0].length == 48000
                         && sOnly.tracks[0]->clips[0].fadeIn == 500 && sOnly.tracks[0]->clips[0].fadeOut == 500
                         && sOnly.tracks[0]->clips[1].length == cap48k
                         && sOnly.tracks[0]->clips[1].fadeIn == cap48k && sOnly.tracks[0]->clips[1].fadeOut == 0,
                     "missing-only project with omitted sampleRate preserves 48000 and caps oversized clip at 2h");
    }
    dir.deleteRecursively();
}

// [50] R1.1, R1.3, R1.4: suite-only candidate filtering, folder walk depth/TCC/hidden/bundle guards, pre-load VST3/AU deduplication, and incremental cache persistence.
static void catalogSafetyAndScanChecks()
{
    std::printf ("[50] Catalog safety and scan checks (R1.1, R1.3, R1.4)\n");
    const auto dir = tempFolder().getChildFile ("catalog-r1");
    dir.deleteRecursively();
    dir.createDirectory();

    // 1. Settings::scanSuiteOnly defaults to false
    Settings defaultSettings;
    test::check (! defaultSettings.scanSuiteOnly, "Settings::scanSuiteOnly defaults to false");

    // 2. VST3 candidate filtering with onlySuite = true and setSuiteOnly(true)
    const auto vstDir = dir.getChildFile ("vst-root");
    const auto hermesBundle = vstDir.getChildFile ("Hermes Level.vst3");
    const auto thirdPartyBundle = vstDir.getChildFile ("ThirdParty Synth.vst3");
    hermesBundle.createDirectory();
    thirdPartyBundle.createDirectory();

    Catalog cat;
    const juce::FileSearchPath extra (vstDir.getFullPathName());
    const auto allCandidates = cat.candidatesForFormat ("VST3", extra, false, false);
    test::check (allCandidates.size() == 2
                     && allCandidates.contains (hermesBundle.getFullPathName())
                     && allCandidates.contains (thirdPartyBundle.getFullPathName()),
                 "candidatesForFormat with onlySuite=false returns both suite and third-party VST3 bundles");

    const auto suiteCandidates = cat.candidatesForFormat ("VST3", extra, false, true);
    test::check (suiteCandidates.size() == 1 && suiteCandidates[0] == hermesBundle.getFullPathName(),
                 "candidatesForFormat with onlySuite=true keeps Hermes Level.vst3 and skips ThirdParty Synth.vst3 before loading");

    cat.setSuiteOnly (true);
    const auto modeSuiteCandidates = cat.candidatesForFormat ("VST3", extra, false, false);
    test::check (modeSuiteCandidates.size() == 1 && modeSuiteCandidates[0] == hermesBundle.getFullPathName(),
                 "Catalog::setSuiteOnly(true) also filters VST3 candidates by plugin name before loading");
    cat.setSuiteOnly (false);

    // 3. findBundlesIn depth limit (kMaxBundleDepth == 5), forbidden roots, and skipped subdirectories
    test::check (Catalog::kMaxBundleDepth == 5, "Catalog::kMaxBundleDepth is 5");
    const auto walkRoot = dir.getChildFile ("walk");
    walkRoot.createDirectory();

    // Depth 5 from walkRoot: d1/d2/d3/d4/d5/Depth5.vst3 (depth 0=walkRoot, 1=d1, 2=d2, 3=d3, 4=d4, 5=d5 -> found inside d5!)
    const auto d5Dir = walkRoot.getChildFile ("d1/d2/d3/d4/d5");
    d5Dir.createDirectory();
    const auto depth5Bundle = d5Dir.getChildFile ("Depth5.vst3");
    depth5Bundle.createDirectory();

    // Depth 6 from walkRoot: d1/d2/d3/d4/d5/d6/Depth6.vst3 (should NOT be reached)
    const auto d6Dir = d5Dir.getChildFile ("d6");
    d6Dir.createDirectory();
    const auto depth6Bundle = d6Dir.getChildFile ("Depth6.vst3");
    depth6Bundle.createDirectory();

    // Skipped subdirectories inside walkRoot, each holding a .vst3 bundle that must NOT be found
    const char* const skippedDirs[] = {
        "Desktop", "Documents", "Downloads", "Music", "Pictures", "Movies",
        "Application Support", "Caches", "Mobile Documents", "Containers",
        ".git", ".Trash", "_deps", "CMakeFiles", "node_modules",
        "SomeApp.app", "Other.component", "Foo.framework"
    };
    for (const char* sub : skippedDirs)
        walkRoot.getChildFile (sub).getChildFile ("HiddenInside.vst3").createDirectory();

    const auto foundBundles = Catalog::findBundlesIn (walkRoot);
    test::check (foundBundles.size() == 1 && foundBundles[0] == depth5Bundle,
                 "findBundlesIn finds bundle at depth 5 and skips depth 6, TCC subdirs, hidden dirs, build dirs, and non-VST3 bundles");

    // Forbidden roots at depth 0 return empty immediately
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    for (const auto& forbidden : { juce::File(),
                                   juce::File ("/"),
                                   juce::File ("/Volumes"),
                                   juce::File ("/Users"),
                                   juce::File ("/System"),
                                   juce::File ("/Library"),
                                   juce::File ("/Applications"),
                                   home,
                                   home.getChildFile ("Library"),
                                   home.getChildFile ("Applications"),
                                   juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                   home.getChildFile ("Downloads"),
                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                   juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
                                   juce::File::getSpecialLocation (juce::File::userMoviesDirectory),
                                   walkRoot.getChildFile ("Desktop"),
                                   walkRoot.getChildFile ("Documents"),
                                   walkRoot.getChildFile ("Downloads"),
                                   walkRoot.getChildFile ("Music"),
                                   walkRoot.getChildFile ("Pictures"),
                                   walkRoot.getChildFile ("Movies") })
    {
        test::check (Catalog::isForbiddenPluginRoot (forbidden) && Catalog::findBundlesIn (forbidden).isEmpty(),
                     "findBundlesIn refuses forbidden root: " + (forbidden == juce::File() ? juce::String ("<empty>") : forbidden.getFullPathName()));
    }

    // 4. Pre-load VST3/AU deduplication
    {
        struct FakeAuFormat : juce::AudioPluginFormat
        {
            juce::String getName() const override { return "AudioUnit"; }
            void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override {}
            bool fileMightContainThisPluginType (const juce::String&) override { return true; }
            juce::String getNameOfPluginFromIdentifier (const juce::String& id) override { return id.fromFirstOccurrenceOf (":", false, false); }
            bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
            bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
            bool canScanForPlugins() const override { return true; }
            bool isTrivialToScan() const override { return true; }
            juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override
            {
                return { "AudioUnit:Hermes Level", "AudioUnit:Unique AU" };
            }
            juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
            void createPluginInstance (const juce::PluginDescription&, double, int, PluginCreationCallback) override {}
            bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
        } fakeAu;

        test::check (Catalog::candidatePluginName (fakeAu, "AudioUnit:Hermes Level") == "Hermes Level",
                     "candidatePluginName extracts AU plugin name via format.getNameOfPluginFromIdentifier");
        test::check (Catalog::candidatePluginName (fakeAu, "AudioUnit:Acme: Hermes Level") == "Hermes Level",
                     "candidatePluginName strips Manufacturer: prefix from AU plugin name");

        juce::KnownPluginList preloaded;
        preloaded.addType (described ("Hermes Level", "Pantheon", "VST3", hermesBundle.getFullPathName()));
        const auto cacheWithVst3 = dir.getChildFile ("with-vst3.xml");
        test::check (preloaded.createXml()->writeTo (cacheWithVst3), "wrote cache with VST3 plugin");

        Catalog dedupCat;
        test::check (dedupCat.loadCache (cacheWithVst3) && dedupCat.names().contains ("Hermes Level"),
                     "loaded VST3 Hermes Level into Catalog");
#if JUCE_PLUGINHOST_AU && JUCE_MAC
        // Also seed any system AU into VST3 list or test with a real system AU if present, or verify via candidatesForFormat:
        const auto allAus = Catalog().candidatesForFormat ("AudioUnit", {}, true, false);
        if (! allAus.isEmpty())
        {
            juce::AudioUnitPluginFormat realAuFmt;
            const auto firstAuId = allAus[0];
            const auto firstAuName = Catalog::candidatePluginName (realAuFmt, firstAuId);
            if (firstAuName.isNotEmpty())
            {
                juce::KnownPluginList withMatchingVst3;
                withMatchingVst3.addType (described (firstAuName, "Pantheon", "VST3", "/x/" + firstAuName + ".vst3"));
                const auto matchXml = dir.getChildFile ("match-au.xml");
                withMatchingVst3.createXml()->writeTo (matchXml);
                Catalog auDedupCat;
                auDedupCat.loadCache (matchXml);
                const auto filteredAus = auDedupCat.candidatesForFormat ("AudioUnit", {}, true, false);
                test::check (! filteredAus.contains (firstAuId),
                             "pre-load VST3/AU deduplication skips AudioUnit candidate when a VST3 of the same name is already in list");
            }
        }
#endif
    }

    // 5. Incremental plugins.xml persistence even when 0 plugins are found
    {
        const auto emptyDir = dir.getChildFile ("empty-plugins");
        emptyDir.createDirectory();
        const auto cacheFile = dir.getChildFile ("incremental/plugins.xml");
        test::check (! cacheFile.existsAsFile(), "cacheFile does not exist before scan");

        Catalog emptyScanCat;
        ScanOptions opts;
        opts.includeSystem = false;
        opts.suiteOnly = true;
        opts.cacheFile = cacheFile;
        opts.deadMansPedal = dir.getChildFile ("incremental/scan.pedal");
        emptyScanCat.scan (juce::FileSearchPath (emptyDir.getFullPathName()), opts);

        test::check (cacheFile.existsAsFile(), "Catalog::scan writes options.cacheFile even when 0 plugins are found");
        Catalog reloaded;
        test::check (reloaded.loadCache (cacheFile) && reloaded.names().isEmpty(),
                     "loadCache returns true on a valid 0-plugin <KNOWNPLUGINS> plugins.xml");

        // 6. Scanning with options.suiteOnly = true preserves cached third-party plugins in Catalog (when suiteOnlyMode is false)
        juce::KnownPluginList withThirdParty;
        withThirdParty.addType (described ("ThirdParty Synth", "Acme", "VST3", thirdPartyBundle.getFullPathName()));
        const auto tpCacheFile = dir.getChildFile ("preserve-tp/plugins.xml");
        tpCacheFile.getParentDirectory().createDirectory();
        test::check (withThirdParty.createXml()->writeTo (tpCacheFile), "wrote cache containing third-party plugin");

        Catalog preserveCat;
        test::check (preserveCat.loadCache (tpCacheFile) && preserveCat.names().contains ("ThirdParty Synth"),
                     "loaded cached third-party plugin into Catalog");
        ScanOptions suiteScanOpts;
        suiteScanOpts.includeSystem = false;
        suiteScanOpts.suiteOnly = true;
        suiteScanOpts.cacheFile = tpCacheFile;
        suiteScanOpts.deadMansPedal = dir.getChildFile ("preserve-tp/scan.pedal");
        preserveCat.scan (juce::FileSearchPath (emptyDir.getFullPathName()), suiteScanOpts);
        test::check (! preserveCat.suiteOnly() && preserveCat.names().contains ("ThirdParty Synth"),
                     "Catalog::scan with options.suiteOnly=true preserves cached third-party plugin when suiteOnlyMode is false");
    }

    dir.deleteRecursively();
}

void analyzeChecks()
{
    std::printf ("[51] DSP analysis (Analyze.h)\n");

    // 1. Pink-like multi-sine + 1 kHz resonance spike (+12 dB)
    {
        constexpr double sr = 48000.0;
        constexpr int n = 48000 * 2;
        juce::AudioBuffer<float> buf (2, n);
        buf.clear();

        // Equal energy per 1/6-octave band (flat in constant-Q)
        for (int b = 0; b < analyze::kNumSpectrumBands; ++b)
        {
            const double f = 20.0 * std::pow (2.0, b / 6.0);
            if (f > 18000.0)
                break;
            const double phase = b * 1.2345;
            const float amp = (std::abs (f - 1000.0) < 80.0) ? 0.08f : 0.02f;
            for (int i = 0; i < n; ++i)
            {
                const float s = amp * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * f * i / sr + phase));
                buf.addSample (0, i, s);
                buf.addSample (1, i, s);
            }
        }

        const auto r = analyze::analyzeBuffer (buf, sr, "flat", -14.0);
        test::check (r.valid, "analyzeBuffer returns valid result");
        bool found1kRing = false;
        for (const auto& ring : r.resonanceRings)
            if (std::abs (ring.freqHz - 1000.0) < 160.0 && ring.standOutDb >= 4.0)
                found1kRing = true;
        test::check (found1kRing, "resonance spike at 1 kHz detected as ring");
        test::check (juce::JSON::toString (r.toVar()).contains ("resonance_rings"), "toVar serializes spectrum and rings");
    }

    // 2. Out-of-phase stereo sine (L = +sin, R = -sin)
    {
        constexpr double sr = 48000.0;
        constexpr int n = 48000;
        juce::AudioBuffer<float> buf (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s = 0.25f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr));
            buf.setSample (0, i, s);
            buf.setSample (1, i, -s);
        }
        const auto r = analyze::analyzeBuffer (buf, sr);
        test::check (r.stereoCorrelationMean < -0.95, "out-of-phase stereo correlation near -1.0");
        test::check (r.monoLossDb > 40.0, "out-of-phase stereo reports high mono loss");
        bool hasPhaseRec = false;
        for (const auto& rec : r.recommendations)
            if (rec.category == "phase" && rec.severity == "bad")
                hasPhaseRec = true;
        test::check (hasPhaseRec, "out-of-phase stereo emits bad phase recommendation");
    }

    // 3. Clipped / hot signal (1.5f peak)
    {
        constexpr double sr = 48000.0;
        constexpr int n = 24000;
        juce::AudioBuffer<float> buf (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s = 1.5f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / sr));
            buf.setSample (0, i, s);
            buf.setSample (1, i, s);
        }
        const auto r = analyze::analyzeBuffer (buf, sr);
        test::check (r.truePeakDb > 0.0, "clipped signal true peak > 0 dBTP");
        test::check (r.clippedSamples > 0, "clipped signal counts clipped samples");
        bool hasBadHeadroom = false;
        for (const auto& rec : r.recommendations)
            if (rec.category == "headroom" && rec.severity == "bad")
                hasBadHeadroom = true;
        test::check (hasBadHeadroom, "clipped signal emits bad headroom recommendation");
    }

    // 4. BS.1770-4 LUFS accuracy at 44.1 kHz and 48 kHz on -20 dBFS 1 kHz stereo sine (amp = 0.1f)
    for (double sr : { 44100.0, 48000.0 })
    {
        const int n = static_cast<int> (sr * 2.0);
        juce::AudioBuffer<float> buf (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s = 0.1f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / sr));
            buf.setSample (0, i, s);
            buf.setSample (1, i, s);
        }
        const auto r = analyze::analyzeBuffer (buf, sr);
        test::check (std::abs (r.integratedLufs - (-20.0)) <= 0.8,
                     "LUFS at " + juce::String (sr, 0) + " Hz within +/- 0.8 LU of -20.0 (got " + juce::String (r.integratedLufs, 2) + ")");
    }

    // 5. Silence & NaN/Inf buffer safety
    {
        juce::AudioBuffer<float> nanBuf (2, 8192);
        nanBuf.clear();
        nanBuf.setSample (0, 100, std::numeric_limits<float>::quiet_NaN());
        nanBuf.setSample (1, 200, std::numeric_limits<float>::infinity());
        const auto rNan = analyze::analyzeBuffer (nanBuf, 48000.0);
        test::check (rNan.valid && std::isfinite (rNan.integratedLufs) && std::isfinite (rNan.truePeakDb)
                         && std::isfinite (rNan.stereoCorrelationMean) && std::isfinite (rNan.rmsDistanceDb),
                     "silence/NaN buffer produces finite outputs without crashing");

        juce::AudioBuffer<float> emptyBuf;
        const auto rEmpty = analyze::analyzeBuffer (emptyBuf, 0.0);
        test::check (! rEmpty.valid && std::isfinite (rEmpty.integratedLufs), "empty/0-rate buffer handled safely");
    }

    // 6. Multitrack masking check at 350 Hz (Low-Mid)
    {
        constexpr double sr = 48000.0;
        constexpr int n = 48000;
        juce::AudioBuffer<float> t1 (2, n), t2 (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s1 = 0.2f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 350.0 * i / sr));
            const float s2 = 0.18f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 355.0 * i / sr));
            t1.setSample (0, i, s1);
            t1.setSample (1, i, s1);
            t2.setSample (0, i, s2);
            t2.setSample (1, i, s2);
        }
        const auto mr = analyze::analyzeMasking ({ { "Guitar", t1 }, { "Keys", t2 } }, sr);
        test::check (mr.tracks.size() == 2, "analyzeMasking summarizes both tracks");
        bool foundLowMidConflict = false;
        for (const auto& c : mr.conflicts)
            if (c.band == "Low-Mid" && c.overlapDb <= 6.0)
                foundLowMidConflict = true;
        test::check (foundLowMidConflict, "analyzeMasking detects Low-Mid masking conflict at 350 Hz");
        test::check (juce::JSON::toString (mr.toVar()).contains ("Low-Mid"), "MaskingResult::toVar serializes conflicts");
    }
}

void mcpAnalyzeAudioChecks()
{
    std::printf ("[52] MCP analyze_audio: direct file, session mix/stems/masking, single track, and error validation\n");
    tempFolder().getChildFile ("mcp-analyze").createDirectory();
    const auto wavA = makeWav ("mcp-analyze/a.wav", 48000.0, 2, 48000, 0.2f, 400.0);
    const auto wavB = makeWav ("mcp-analyze/b.wav", 48000.0, 2, 48000, 0.18f, 405.0);
    const auto pathA = wavA.getFullPathName().replace ("\\", "\\\\");

    juce::String err;
    Session s;
    test::addTestPlugins (s.catalog);
    s.addTrack (wavA, "Guitar", err);
    s.addTrack (wavB, "Keys", err);
    McpServer server (s);
    const auto call = [&server] (const char* tool, const juce::String& args) { return toolCall (server, tool, args); };
    const auto failed = [] (const juce::var& r) { return (bool) r["isError"]; };

    test::check (! failed (call ("add_plugin", "{\"track\":0,\"plugin\":\"Test Gain\"}")), "add_plugin puts Test Gain on track 0");

    // 1. Direct WAV file analysis
    {
        const auto r = call ("analyze_audio", "{\"file\":\"" + pathA + "\",\"target\":\"electronic\",\"target_lufs\":-12.0}");
        const auto j = toolJson (r);
        test::check (! failed (r) && (bool) j["valid"] && j["target_name"].toString() == "electronic"
                         && j["loudness"].isObject() && j["peaks"].isObject()
                         && j["spectrum"].isObject() && j["stereo"].isObject() && j["recommendations"].isArray(),
                     "analyze_audio on file returns loudness, peaks, spectrum, stereo, and recommendations");
    }

    // 2. Full session analysis (mix + tracks + masking)
    {
        const auto r = call ("analyze_audio", "{}");
        const auto j = toolJson (r);
        test::check (! failed (r) && j["mix"].isObject() && (bool) j["mix"]["valid"]
                         && j["tracks"].isArray() && j["tracks"].size() == 2
                         && j["masking"].isObject() && j["masking"]["conflicts"].isArray()
                         && ! j["masking"]["conflicts"].getArray()->isEmpty(),
                     "analyze_audio on 2-track session returns mix, 2 track summaries, and masking conflicts");
    }

    // 3. Single track analysis
    {
        const auto r = call ("analyze_audio", "{\"track\":0}");
        const auto j = toolJson (r);
        test::check (! failed (r) && (bool) j["valid"] && j["loudness"].isObject() && ! j.hasProperty ("mix"),
                     "analyze_audio on track 0 returns single-track analysis");
    }

    // 4. Error cases: muted track, invalid target, and file + track both given
    {
        s.tracks[0]->mute = true;
        const auto rMuted = call ("analyze_audio", "{\"track\":0}");
        test::check (failed (rMuted) && toolText (rMuted).contains ("muted"),
                     "analyze_audio on muted track returns error");
        s.tracks[0]->mute = false;

        const auto rBadTarget = call ("analyze_audio", "{\"file\":\"" + pathA + "\",\"target\":\"unknown_genre\"}");
        test::check (failed (rBadTarget), "analyze_audio with invalid target returns isError");

        const auto rBoth = call ("analyze_audio", "{\"file\":\"" + pathA + "\",\"track\":0}");
        test::check (failed (rBoth), "analyze_audio with both file and track returns isError");
    }

    tempFolder().getChildFile ("mcp-analyze").deleteRecursively();
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    catalogChecks();
    catalogCacheChecks();
    tempoChecks();
    suiteNamesMatchPlugins();
    trackChecks();
    renderChecks();
    realtimeChecks();
    projectChecks();
    chainChecks();
    clipChecks();
    poolChecks();
    markerChecks();
    projectV2Checks();
    legacyChecks();
    missingAudioChecks();
    metronomeChecks();
    headChecks();
    editChecks();
    masterChecks();
    describeChecks();
    mcpChecks();
    mcpTimelineChecks();
    realPlugins();
    splitChecks();
    moveChecks();
    trimChecks();
    mixEditChecks();
    snapChecks();
    undoChecks();
    fixChecks();
    monitorChecks();
    monitorStateChecks();
    recorderChecks();
    finishChecks();
    recordSettingsChecks();
    deviceRateChecks();
    busChecks();
    addClipLockChecks();
    latencyChecks();
    settingsFileChecks();
    mcpUndoChecks();
    takeAndNameChecks();
    clipboardChecks();
    mcpCopyChainChecks();
    copyChainLockChecks();
    processBlockChunkChecks();
    recorderChunkAtomicityChecks();
    mcpSetChainChecks();
    mcpIndexValidationChecks();
    projectValidationChecks();
    catalogSafetyAndScanChecks();
    analyzeChecks();
    mcpAnalyzeAudioChecks();
    tempFolder().deleteRecursively();
    return test::finish ("OlympusTests");
}
