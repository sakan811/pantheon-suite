// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless checks of the session, the render, the project file and the MCP server, with plugins made in code (TestPlugin.h).
// Set OLYMPUS_PLUGIN_DIR to a folder with built plugin bundles to add the check against the real ones.

#include "Check.h"
#include "Mcp.h"
#include "TestPlugin.h"

#include <sstream>

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
    test::check (s.tracks[0]->audio.getNumChannels() == 2 && juce::approximatelyEqual (s.tracks[0]->audio.getSample (1, 1000), s.tracks[0]->audio.getSample (0, 1000)), "mono is copied to both channels");
    test::check (s.addTrack (makeWav ("b.wav", 48000.0, 2, 48000, 0.5f, 440.0), "B", err), "a 48 kHz file is added");
    test::check (std::abs (s.tracks[1]->length() - 44100) <= 2, test::fmt ("and resampled: %d samples (one second at 44.1 kHz)", s.tracks[1]->length()));
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
        test::check (s.load (moved.getChildFile ("song.olympus"), err) && s.tracks.size() == 1 && s.tracks[0]->length() == 0 && s.warnings.size() == 1, "missing audio: the track stays, silent, with a warning");
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
    test::check (tools.size() == 19 && tools[0]["inputSchema"]["type"].toString() == "object", test::fmt ("tools/list: %d tools, each with a schema", (int) tools.size()));

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
    s.catalog.scan (juce::FileSearchPath (dir), false);
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
    test::check (xml->getChildElement (0)->getChildElement (0)->getStringAttribute ("state").isNotEmpty(), "its state is written to the project");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    catalogChecks();
    suiteNamesMatchPlugins();
    trackChecks();
    renderChecks();
    realtimeChecks();
    projectChecks();
    chainChecks();
    mcpChecks();
    realPlugins();
    tempFolder().deleteRecursively();
    return test::finish ("OlympusTests");
}
