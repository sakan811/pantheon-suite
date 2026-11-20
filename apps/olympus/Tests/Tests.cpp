// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless checks of the session, the render, the project file and the MCP server, with plugins made in code (TestPlugin.h).
// Set OLYMPUS_PLUGIN_DIR to a folder with built plugin bundles to add the check against the real ones.

#include "Check.h"
#include "Edit.h"
#include "Mcp.h"
#include "Record.h"
#include "Settings.h"
#include "TempoTests.h"
#include "TestPlugin.h"

#include <chrono>
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
    test::check (tools.size() == 31 && tools[0]["inputSchema"]["type"].toString() == "object", test::fmt ("tools/list: %d tools, each with a schema", (int) tools.size()));

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
    test::check (tools.size() == 31 && known == 12, test::fmt ("tools/list: %d tools, %d of the 12 timeline tools among them", (int) tools.size(), known));

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
    tempFolder().deleteRecursively();
    return test::finish ("OlympusTests");
}
