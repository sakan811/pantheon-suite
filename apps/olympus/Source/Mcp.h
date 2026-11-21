// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "Analyze.h"
#include "Edit.h"
#include "Session.h"
#include "Tempo.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>

// A Model Context Protocol server over stdio (one JSON message per line, JSON-RPC 2.0), so an agent can build the session of a
// song: add the tracks and clips, put a chain of the suite's plugins on each, set the plugins' settings, lay out the tempo, markers
// and loop, save the project and render the stems. It edits the project the way the window does (the same Session, and every timeline
// edit through the Editor, so the same lock and undo steps), and a saved project opens in the window for the editing that is left. It
// does not play audio, and it reads and writes only the paths it is given.
namespace olympus
{
class McpServer
{
public:
    explicit McpServer (Session& s) : session (s), editor (s) {}

    // One incoming message; the reply, or a void var for a notification (which has none).
    juce::var handle (const juce::var& request)
    {
        if (! request.isObject() || ! request["method"].isString())
        {
            const auto id = request.isObject() ? request.getProperty ("id", juce::var()) : juce::var();
            return failure (id, -32600, "Invalid Request");
        }
        const auto method = request["method"].toString();
        const auto id = request["id"];
        // A notification carries no id, so it is never answered, whatever its method names (an answer would carry an id no
        // client sent). The check sits before the dispatch below, so a notification naming a known method ("ping") stays silent too;
        // an invalid request is still answered above, and a request that does have an id is answered by the dispatch or with -32601.
        if (! request.hasProperty ("id"))
            return {};
        if (method == "initialize")
        {
            // The versions this server speaks, newest last. The client's is echoed only when it is one of them; agreeing to an
            // unknown version would tell an agent that the server follows a protocol it has not implemented.
            static constexpr const char* kOwnProtocol = "2025-06-18";
            const auto requested = request["params"]["protocolVersion"].toString();
            const auto version = (requested == "2024-11-05" || requested == "2025-03-26" || requested == kOwnProtocol)
                                     ? requested : juce::String (kOwnProtocol);
            auto result = object ({ { "protocolVersion", version },
                                    { "capabilities", object ({ { "tools", object ({}) } }) },
                                    { "serverInfo", object ({ { "name", "olympus" }, { "version", "1.0.0" } }) },
                                    { "instructions", instructions() } });
            return reply (id, result);
        }
        if (method == "ping")
            return reply (id, object ({}));
        if (method == "tools/list")
            return reply (id, object ({ { "tools", tools() } }));
        if (method == "tools/call")
        {
            const auto name = request["params"]["name"].toString();
            const auto args = request["params"]["arguments"];
            juce::String error;
            juce::var out = call (name, args.isObject() ? args : object ({}), error);
            const bool failed = error.isNotEmpty();
            const auto text = failed ? error : juce::JSON::toString (out, true);
            juce::Array<juce::var> content;
            content.add (object ({ { "type", "text" }, { "text", text } }));
            return reply (id, object ({ { "content", content }, { "isError", failed } }));
        }
        return failure (id, -32601, "Method not found: " + method);
    }

    // Reads lines from `in` until it ends, answering each on `out`.
    void run (std::istream& in, std::ostream& out)
    {
        std::string line;
        while (std::getline (in, line))
        {
            if (juce::String (line).trim().isEmpty()) continue;
            juce::var request;
            const auto parsed = juce::JSON::parse (juce::String (line), request);
            const auto response = parsed.wasOk() ? handle (request) : failure (juce::var(), -32700, "Parse error");
            if (! response.isVoid())
                out << juce::JSON::toString (response, true).toStdString() << "\n" << std::flush;
        }
    }

private:
    Session& session;
    Editor editor; // every timeline edit goes through it (the lock, the undo steps); the server never edits the timeline any other way

    // What one call is allowed to spin up: every entry of these arrays instantiates a plugin (or copies a chain onto a track),
    // so an unbounded array would make one tool call build thousands of instances.
    static constexpr int kMaxPluginsPerChain = 64;   // set_chain
    static constexpr int kMaxChainTargets = 128;     // copy_chain

    using Pair = std::pair<const char*, juce::var>;
    static juce::var object (std::initializer_list<Pair> items)
    {
        auto* o = new juce::DynamicObject();
        for (auto& [k, v] : items) o->setProperty (k, v);
        return juce::var (o);
    }
    static juce::var reply (const juce::var& id, const juce::var& result) { return object ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } }); }
    static juce::var failure (const juce::var& id, int code, const juce::String& message)
    {
        return object ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", object ({ { "code", code }, { "message", message } }) } });
    }
    static juce::File resolvePath (const juce::String& p)
    {
        if (p.isEmpty())
            return {};
        return juce::File::isAbsolutePath (p) ? juce::File (p)
                                              : juce::File::getCurrentWorkingDirectory().getChildFile (p);
    }

    static juce::String instructions()
    {
        return "Olympus is a session host for the Pantheon plugin suite. Typical use: new_project, add_track for each stem (a WAV or other audio file), "
               "set_chain or add_plugin to put plugins on a stem, set_parameter / load_preset to set them (get_plugin_state / set_plugin_state keep and restore a whole plugin state; plugin_notes gives the GUIDE and the session's notes), save_project (the user opens the .olympus file "
               "in the Olympus window to edit further), render_stems to write one WAV per stem. The timeline: add_clip places a file on a track at a "
               "position (seconds or beats), split_clip / move_clip / trim_clip / delete_clip / set_clip edit clips, set_tempo, set_time_signature, "
               "add_marker / remove_marker, set_loop and set_master set the rest. Tracks, clips, markers and plugin slots are numbered from 0; a track may also "
               "be named, a plugin may be given by the start of its name ('hermes'). Call list_plugins first: only plugins that are installed can be used. "
               "Parameter values are 0..1, or text as the plugin shows it ('-3.0 dB'). Nothing is played: this server only edits and renders. "
               "Use analyze_audio for DSP mixing and mastering analysis of the session mix/stems, a single track, or an audio file "
               "(loudness, true peak, tonal balance, resonance, harshness, phase/mono compatibility, and inter-track masking).";
    }

    // ---- the tools ------------------------------------------------------------------------------------------------------
    static juce::var prop (const char* type, const char* description) { return object ({ { "type", type }, { "description", description } }); }
    static juce::var tool (const char* name, const char* description, std::initializer_list<Pair> properties, std::initializer_list<const char*> required = {})
    {
        juce::Array<juce::var> req;
        for (auto* r : required) req.add (juce::String (r));
        auto props = object (properties);
        return object ({ { "name", name }, { "description", description },
                         { "inputSchema", object ({ { "type", "object" }, { "properties", props }, { "required", req } }) } });
    }

    static juce::var tools()
    {
        const auto trackRef = prop ("string", "Track number (from 0) or name");
        const auto slotRef = prop ("string", "Position in the chain (from 0) or the plugin's name");
        juce::Array<juce::var> list;
        list.add (tool ("list_plugins", "The plugins that are installed (VST3, and Audio Units on macOS) and can be put on a track.", {}));
        list.add (tool ("project_info", "The whole session: tracks, faders, chains, latency.", {}));
        list.add (tool ("new_project", "Empties the session.", { { "name", prop ("string", "Project name") } }));
        list.add (tool ("open_project", "Opens a saved .olympus project.", { { "path", prop ("string", "Absolute path of the project file") } }, { "path" }));
        list.add (tool ("save_project", "Saves the project (to `path`, or where it was opened from).", { { "path", prop ("string", "Absolute path ending in .olympus") } }));
        list.add (tool ("add_track", "Adds a track from an audio file (every track becomes stereo at the session's sample rate).",
                        { { "file", prop ("string", "Absolute path of the audio file") }, { "name", prop ("string", "Track name (default: the file's name)") } }, { "file" }));
        list.add (tool ("remove_track", "Removes a track.", { { "track", trackRef } }, { "track" }));
        list.add (tool ("set_track", "Fader (dB), pan, mute, solo or name of a track.",
                        { { "track", trackRef }, { "gain_db", prop ("number", "Fader in dB") }, { "pan", prop ("number", "-1 (left) .. 1 (right)") },
                          { "mute", prop ("boolean", "") }, { "solo", prop ("boolean", "") }, { "name", prop ("string", "") } }, { "track" }));
        list.add (tool ("add_plugin", "Puts a plugin (at its defaults) in a track's chain.",
                        { { "track", trackRef }, { "plugin", prop ("string", "Plugin name or the start of it") }, { "position", prop ("integer", "Place in the chain (default: the end)") } }, { "track", "plugin" }));
        list.add (tool ("remove_plugin", "Takes a plugin out of a chain.", { { "track", trackRef }, { "slot", slotRef } }, { "track", "slot" }));
        list.add (tool ("move_plugin", "Moves a plugin to another place in the same chain.", { { "track", trackRef }, { "from", prop ("integer", "") }, { "to", prop ("integer", "") } }, { "track", "from", "to" }));
        list.add (tool ("bypass_plugin", "Switches a plugin of a chain off or on.", { { "track", trackRef }, { "slot", slotRef }, { "bypass", prop ("boolean", "") } }, { "track", "slot", "bypass" }));
        list.add (tool ("set_chain", "Replaces a track's chain with these plugins (at their defaults), in order.",
                        { { "track", trackRef }, { "plugins", object ({ { "type", "array" }, { "items", object ({ { "type", "string" } }) } }) } }, { "track", "plugins" }));
        list.add (tool ("copy_chain", "Gives other tracks the same chain, with the same settings, as one track.",
                        { { "from_track", trackRef }, { "to_tracks", object ({ { "type", "array" }, { "items", object ({ { "type", "string" } }) } }) } }, { "from_track", "to_tracks" }));
        list.add (tool ("list_parameters", "The settings of a plugin: id, name, value (0..1) and the text the plugin shows.", { { "track", trackRef }, { "slot", slotRef } }, { "track", "slot" }));
        list.add (tool ("set_parameter", "Sets one setting, by its value (0..1) or by text.",
                        { { "track", trackRef }, { "slot", slotRef }, { "parameter", prop ("string", "id, name or number from list_parameters") },
                          { "value", prop ("number", "0..1") }, { "text", prop ("string", "As the plugin shows it, e.g. '-3.0 dB'") } }, { "track", "slot", "parameter" }));
        list.add (tool ("list_presets", "The factory presets of a plugin, where it has any.", { { "track", trackRef }, { "slot", slotRef } }, { "track", "slot" }));
        list.add (tool ("load_preset", "Loads a preset into a plugin (it replaces the plugin's settings).", { { "track", trackRef }, { "slot", slotRef }, { "preset", prop ("string", "Name or number") } }, { "track", "slot", "preset" }));
        list.add (tool ("get_plugin_state", "A plugin's complete state (every setting, and what the plugin keeps besides) as base64 text, to keep or to hand to set_plugin_state.", { { "track", trackRef }, { "slot", slotRef } }, { "track", "slot" }));
        list.add (tool ("set_plugin_state", "Restores a state from get_plugin_state into a plugin of the same kind (it replaces all the plugin's settings).",
                        { { "track", trackRef }, { "slot", slotRef }, { "state", prop ("string", "Base64 text from get_plugin_state") } }, { "track", "slot", "state" }));
        list.add (tool ("plugin_notes", "What Olympus is for and not for (the GUIDE text), and the notes it gives about this session (warnings first).", {}));
        list.add (tool ("render_stems", "Writes one WAV per audible track (after its chain and fader, aligned to the audio) and mix.wav, into a folder.",
                        { { "folder", prop ("string", "Absolute path of the folder (made if needed)") }, { "tail_seconds", prop ("number", "Extra time at the end for tails; default 0") },
                          { "mix", prop ("boolean", "Also write mix.wav; default true") }, { "bits", prop ("integer", "16, 24 or 32 (float, default)") } }, { "folder" }));
        list.add (tool ("analyze_audio",
                        "DSP mixing and mastering analysis (BS.1770-4 LUFS, true peak, LRA, crest/PLR, 1/6-oct tonal balance vs target curve, resonance rings, 4-10 kHz harshness, stereo correlation/mono loss, inter-track masking, and plugin recommendations). Analyzes the session mix and stems by default, or a specific 'track' or audio 'file'.",
                        { { "track", trackRef },
                          { "file", prop ("string", "Optional absolute path of an audio file to analyze directly") },
                          { "target", prop ("string", "Tonal target curve: modern_pop (default), electronic, rock, acoustic, flat") },
                          { "target_lufs", prop ("number", "Target integrated loudness in LUFS (default -14.0)") } }));

        // the timeline: a position is given as seconds or as beats (quarter notes from the start), exactly one of the two
        const auto pos = [] (const char* what) { return prop ("number", what); };
        const auto clipRef = prop ("integer", "Clip number on the track (from 0)");
        const auto markerRef = prop ("integer", "Marker number (from 0, in time order: see project_info)");
        list.add (tool ("add_clip", "Places a clip of an audio file on a track, at a position on the timeline. The file is read into the session (the first file sets its rate).",
                        { { "track", trackRef }, { "file", prop ("string", "Absolute path of the audio file") },
                          { "start_seconds", pos ("Where the clip begins, in seconds from the start") }, { "start_beats", pos ("Where the clip begins, in quarter notes from the start") },
                          { "offset_seconds", pos ("Where in the file it begins, in seconds (default 0)") },
                          { "length_seconds", pos ("How long it plays, in seconds (default: to the end of the file)") } }, { "track", "file" }));
        list.add (tool ("move_clip", "Moves a clip along its track, or to another track (appended there) with to_track.",
                        { { "track", trackRef }, { "clip", clipRef }, { "to_track", trackRef },
                          { "start_seconds", pos ("The new start, in seconds") }, { "start_beats", pos ("The new start, in quarter notes") } }, { "track", "clip" }));
        list.add (tool ("split_clip", "Cuts a clip in two at a position inside it. The left piece keeps its number; the right piece is the next one.",
                        { { "track", trackRef }, { "clip", clipRef }, { "at_seconds", pos ("Where to cut, in seconds") }, { "at_beats", pos ("Where to cut, in quarter notes") } },
                        { "track", "clip" }));
        list.add (tool ("trim_clip", "Moves the left and/or the right edge of a clip; the audio stays where it is on the timeline.",
                        { { "track", trackRef }, { "clip", clipRef },
                          { "start_seconds", pos ("New left edge, in seconds") }, { "start_beats", pos ("New left edge, in quarter notes") },
                          { "end_seconds", pos ("New right edge, in seconds") }, { "end_beats", pos ("New right edge, in quarter notes") } }, { "track", "clip" }));
        list.add (tool ("delete_clip", "Removes a clip from its track.", { { "track", trackRef }, { "clip", clipRef } }, { "track", "clip" }));
        list.add (tool ("set_clip", "Gain, fades, mute or name of a clip (give any of them).",
                        { { "track", trackRef }, { "clip", clipRef }, { "gain_db", prop ("number", "Clip gain in dB, -100 to +24") },
                          { "fade_in_seconds", pos ("Fade in from silence, in seconds") }, { "fade_out_seconds", pos ("Fade out to silence, in seconds") },
                          { "mute", prop ("boolean", "") }, { "name", prop ("string", "") } }, { "track", "clip" }));
        list.add (tool ("set_tempo", "Sets the tempo from a beat on (a tempo change takes effect at its beat; audio does not follow it, the grid does).",
                        { { "bpm", prop ("number", "20 to 300") }, { "beat", prop ("number", "Quarter note from the start where it takes effect (default 0: the whole project)") } }, { "bpm" }));
        list.add (tool ("set_time_signature", "Sets the project's time signature.",
                        { { "numerator", prop ("integer", "Beats per bar, 1 to 32") }, { "denominator", prop ("integer", "1, 2, 4, 8, 16 or 32") } }, { "numerator", "denominator" }));
        list.add (tool ("add_marker", "Adds a named marker at a position. Its number is in the answer (added_index).",
                        { { "name", prop ("string", "Default: Marker") }, { "at_seconds", pos ("Where, in seconds") }, { "at_beats", pos ("Where, in quarter notes") } }, {}));
        list.add (tool ("remove_marker", "Removes a marker.", { { "marker", markerRef } }, { "marker" }));
        list.add (tool ("set_loop", "Sets the loop: from start to end (the end must be after the start), on or off (default on).",
                        { { "start_seconds", pos ("in seconds") }, { "start_beats", pos ("in quarter notes") },
                          { "end_seconds", pos ("in seconds") }, { "end_beats", pos ("in quarter notes") }, { "on", prop ("boolean", "Default true") } }, {}));
        list.add (tool ("set_master", "The master gain: applied to the playback and to mix.wav, never to the stems.",
                        { { "master_db", prop ("number", "-100 to +12 dB") } }, { "master_db" }));
        list.add (tool ("undo", "Undoes the last timeline edit (a clip, the mix, the master, the tempo, a marker, the loop...). Tracks and plugins are not undone, and adding or removing one clears the history.", {}));
        list.add (tool ("redo", "Redoes the edit that undo took back.", {}));
        return list;
    }

    juce::var call (const juce::String& name, const juce::var& a, juce::String& error)
    {
        if (name == "list_plugins")
        {
            juce::Array<juce::var> names;
            for (auto& n : session.catalog.names()) names.add (n);
            return object ({ { "installed", names }, { "note", names.isEmpty() ? "None found: plugins are installed as VST3 bundles (or Audio Units on macOS); see the Olympus README for the folders searched." : "" } });
        }
        if (name == "project_info")
            return session.describe();
        if (name == "plugin_notes")
        {
            juce::Array<juce::var> list;
            for (auto& n : adviceFor (session))
                list.add (object ({ { "severity", n.severity == ui::Severity::warning ? "warning" : "info" }, { "text", juce::String (n.text) } }));
            return object ({ { "guide", guideText() }, { "notes", list } });
        }
        if (name == "new_project")
        {
            session.newProject (a["name"].toString());
            editor.clearUndo();
            return session.describe();
        }
        if (name == "open_project")
        {
            if (! session.load (resolvePath (a["path"].toString()), error)) return {};
            editor.clearUndo();
            auto d = session.describe();
            juce::Array<juce::var> w;
            for (auto& s : session.warnings) w.add (s);
            d.getDynamicObject()->setProperty ("warnings", w);
            return d;
        }
        if (name == "save_project")
        {
            const auto path = a["path"].toString().trim();
            if (a.hasProperty ("path") && ! a["path"].isVoid() && (! a["path"].isString() || path.isEmpty()))
            { error = "Give 'path': a non-empty project path (or omit 'path' to save to the current file)."; return {}; }
            const auto file = path.isNotEmpty() ? resolvePath (path) : session.projectFile;
            if (file == juce::File()) { error = "No path: this project has not been saved yet."; return {}; }
            if (! session.save (file.hasFileExtension ("olympus") ? file : file.withFileExtension ("olympus"), error)) return {};
            return object ({ { "saved", session.projectFile.getFullPathName() } });
        }
        if (name == "add_track")
        {
            // the file is named before anything reads it: a missing or a non-string 'file' is the caller's mistake, not " contains no audio."
            if (! a.hasProperty ("file") || a["file"].isVoid())
            { error = "Give 'file' (an audio file path)."; return {}; }
            if (! a["file"].isString() || a["file"].toString().trim().isEmpty())
            { error = "'file' must be the path of an audio file (a string)."; return {}; }
            if (! session.addTrack (resolvePath (a["file"].toString()), a["name"].toString(), error)) return {};
            editor.clearUndo(); // tracks are not undone (see Edit.h)
            return session.describe();
        }

        // the timeline that names no track: the tempo, the time signature, markers, the loop and the master
        if (name == "set_tempo")
        {
            if (! a.hasProperty ("bpm")) { error = "Give 'bpm' (20 to 300)."; return {}; }
            double bpm = 0.0;
            if (! finiteNumber (a["bpm"], bpm)) { error = "'bpm' must be a number (20 to 300)."; return {}; }
            double beat = 0.0;
            if (a.hasProperty ("beat"))
            {
                if (! finiteNumber (a["beat"], beat) || beat < 0.0)
                { error = "'beat' must be a number 0 or more: quarter notes from the start of the project."; return {}; }
            }
            if (! (bpm >= TempoMap::kMinBpm && bpm <= TempoMap::kMaxBpm)) { error = "'bpm' must be 20 to 300 (got " + juce::String (bpm) + ")."; return {}; }
            if (! editor.setTempo (beat, bpm, error)) return {};
            return session.describe();
        }
        if (name == "set_time_signature")
        {
            int num = 0, den = 0;
            if (! a.hasProperty ("numerator") || ! wholeNumber (a["numerator"], num) || num < 1 || num > 32)
            { error = "'numerator' must be a whole number from 1 to 32 (beats per bar)."; return {}; }
            if (! a.hasProperty ("denominator") || ! wholeNumber (a["denominator"], den)
                || (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32))
            { error = "'denominator' must be 1, 2, 4, 8, 16 or 32."; return {}; }
            editor.setSignature (num, den);
            return session.describe();
        }
        if (name == "add_marker")
        {
            const auto at = position (a, "at", error);
            if (! at) return {};
            const auto label = a.hasProperty ("name") ? a["name"].toString().trim() : juce::String();
            const int index = editor.addMarker (label.isEmpty() ? juce::String ("Marker") : label, *at);
            auto d = session.describe();
            d.getDynamicObject()->setProperty ("added_index", index);
            return d;
        }
        if (name == "remove_marker")
        {
            const int marker = indexArg (a, "marker", error);
            if (marker < 0) return {};
            return editor.removeMarker (marker, error) ? session.describe() : juce::var();
        }
        if (name == "set_loop")
        {
            const auto start = position (a, "start", error);
            if (! start) return {};
            const auto end = position (a, "end", error);
            if (! end) return {};
            if (*end <= *start) { error = "The loop end must be after its start."; return {}; }
            if (a.hasProperty ("on") && ! a["on"].isBool()) { error = "'on' must be true or false."; return {}; }
            editor.setLoop (*start, *end, a.hasProperty ("on") ? (bool) a["on"] : true);
            return session.describe();
        }
        if (name == "set_master")
        {
            if (! a.hasProperty ("master_db")) { error = "Give 'master_db' (-100 to +12)."; return {}; }
            double db = 0.0;
            if (! finiteNumber (a["master_db"], db)) { error = "'master_db' must be a number (-100 to +12 dB)."; return {}; }
            if (! (db >= -100.0 && db <= 12.0)) { error = "'master_db' must be -100 to +12 dB (got " + juce::String (db) + ")."; return {}; }
            editor.setMasterDb ((float) db);
            return session.describe();
        }
        if (name == "undo" || name == "redo")
        {
            // the same history as the window's Undo and Redo (Editor): the answer names the edit, and both flags say what is left
            const bool undoing = name == "undo";
            const auto label = undoing ? editor.undoLabel() : editor.redoLabel();
            if (undoing ? ! editor.canUndo() : ! editor.canRedo())
            {
                error = undoing ? "Nothing to undo: no edit is in the history (tracks and plugins are not undone)."
                                : "Nothing to redo: no edit has been undone.";
                return {};
            }
            if (! (undoing ? editor.undo() : editor.redo()))
            {
                error = "The edit cannot be " + juce::String (undoing ? "undone" : "redone") + ": the number of tracks changed since it was made.";
                return {};
            }
            auto d = session.describe();
            d.getDynamicObject()->setProperty (undoing ? "undone" : "redone", label);
            d.getDynamicObject()->setProperty ("can_undo", editor.canUndo());
            d.getDynamicObject()->setProperty ("can_redo", editor.canRedo());
            return d;
        }

        if (name == "analyze_audio")
        {
            const bool hasFile = a.hasProperty ("file") && ! a["file"].isVoid();
            if (hasFile && (! a["file"].isString() || a["file"].toString().trim().isEmpty()))
            {
                error = "Give a valid 'file' path (or omit 'file').";
                return {};
            }
            const bool hasTrack = a.hasProperty ("track") && ! a["track"].isVoid();
            if (hasTrack && a["track"].isString() && a["track"].toString().trim().isEmpty())
            {
                error = "Give 'track' (a track number from 0, or a track name), or omit 'track' to analyze the whole mix.";
                return {};
            }
            if (hasFile && hasTrack)
            {
                error = "Give either 'file' or 'track', not both.";
                return {};
            }

            juce::String target = "modern_pop";
            if (a.hasProperty ("target") && ! a["target"].isVoid())
            {
                const auto t = a["target"].toString().trim();
                if (t.isNotEmpty())
                {
                    const auto lower = t.toLowerCase();
                    if (lower != "modern_pop" && lower != "pop" && lower != "streaming"
                        && lower != "electronic" && lower != "rock" && lower != "acoustic" && lower != "flat")
                    {
                        error = "Unknown 'target' curve '" + t + "'. Use: modern_pop, electronic, rock, acoustic, or flat.";
                        return {};
                    }
                    target = lower;
                }
            }

            double targetLufs = -14.0;
            if (a.hasProperty ("target_lufs") && ! a["target_lufs"].isVoid())
            {
                if (! (a["target_lufs"].isDouble() || a["target_lufs"].isInt() || a["target_lufs"].isInt64()))
                {
                    error = "'target_lufs' must be a number in [-60.0, 0.0].";
                    return {};
                }
                const double v = (double) a["target_lufs"];
                if (! std::isfinite (v) || v < -60.0 || v > 0.0)
                {
                    error = "'target_lufs' must be between -60.0 and 0.0.";
                    return {};
                }
                targetLufs = v;
            }

            auto readWavFile = [] (const juce::File& f, juce::AudioBuffer<float>& buf, double& rate, juce::String& err) -> bool
            {
                if (! f.existsAsFile())
                {
                    err = "Audio file not found: " + f.getFullPathName();
                    return false;
                }
                // The same guards Session::readAudio applies before it allocates (it is private, so they are repeated here rather
                // than shared): a header claiming more frames or more channels than the file can hold would reach setSize below
                // with an impossible size and die inside JUCE instead of answering with an isError.
                const juce::int64 fileSize = f.getSize();
                if (fileSize <= 0) { err = f.getFileName() + " contains no audio."; return false; }
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
                if (reader == nullptr)
                {
                    err = "Cannot read audio file: " + f.getFullPathName();
                    return false;
                }
                if (! std::isfinite (reader->sampleRate) || reader->sampleRate <= 0.0)
                {
                    err = f.getFileName() + " has an invalid sample rate.";
                    return false;
                }
                if (reader->numChannels == 0 || reader->lengthInSamples <= 0)
                {
                    err = f.getFileName() + " contains no audio.";
                    return false;
                }
                if (reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max())
                {
                    err = f.getFileName() + " is too long to load.";
                    return false;
                }
                if ((double) reader->lengthInSamples > reader->sampleRate * 3600.0 * 2.0)
                {
                    err = f.getFileName() + " is longer than two hours.";
                    return false;
                }
                const bool isWav = reader->getFormatName() == "WAV file";
                const bool isAiff = reader->getFormatName() == "AIFF file";
                if (isWav || isAiff)
                {
                    const juce::int64 minHeader = isWav ? 44 : 54;
                    const juce::int64 bytesPerSample = (juce::int64) (reader->bitsPerSample / 8);
                    if (reader->bitsPerSample < 8 || (reader->bitsPerSample % 8) != 0 || bytesPerSample <= 0
                        || fileSize <= minHeader
                        || (juce::int64) reader->numChannels > (fileSize - minHeader) / bytesPerSample)
                    {
                        err = f.getFileName() + " is truncated or corrupt.";
                        return false;
                    }
                    const juce::int64 frameBytes = (juce::int64) reader->numChannels * bytesPerSample;
                    if (reader->lengthInSamples > (fileSize - minHeader) / frameBytes)
                    {
                        err = f.getFileName() + " is truncated or corrupt.";
                        return false;
                    }
                }
                else if (reader->lengthInSamples / 8192 > fileSize)
                {
                    err = f.getFileName() + " is truncated or corrupt.";
                    return false;
                }
                const int n = (int) reader->lengthInSamples;
                buf.setSize ((int) reader->numChannels, n);
                if (n > 0 && ! reader->read (&buf, 0, n, 0, true, true))
                {
                    err = "Failed to read audio samples from: " + f.getFullPathName();
                    return false;
                }
                rate = reader->sampleRate;
                return true;
            };

            if (hasFile)
            {
                juce::AudioBuffer<float> buf;
                double rate = 0.0;
                if (! readWavFile (resolvePath (a["file"].toString()), buf, rate, error))
                    return {};
                return analyze::analyzeBuffer (buf, rate, target, targetLufs).toVar();
            }

            struct TempDirGuard
            {
                juce::File dir;
                ~TempDirGuard() { dir.deleteRecursively(); }
            };
            const TempDirGuard tempGuard {
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("olympus-analyze-" + juce::Uuid().toString())
            };

            if (hasTrack)
            {
                const int trackIdx = session.findTrack (a["track"], error);
                if (trackIdx < 0)
                    return {};
                juce::String trackName;
                {
                    const juce::ScopedLock sl (session.lock);
                    if (trackIdx < (int) session.tracks.size())
                        trackName = session.tracks[(size_t) trackIdx]->name;
                }

                RenderOptions opts;
                opts.folder = tempGuard.dir;
                opts.writeStems = true;
                opts.writeMix = false;
                opts.bits = 32;
                RenderResult res;
                if (! session.renderStems (opts, res, error))
                    return {};

                const juce::String prefix = juce::String (trackIdx + 1).paddedLeft ('0', 2) + "-";
                juce::File stemFile;
                for (const auto& s : res.stems)
                {
                    const juce::File f (s.file);
                    if (f.getFileName().startsWith (prefix))
                    {
                        stemFile = f;
                        break;
                    }
                }
                if (! stemFile.existsAsFile())
                {
                    error = "Track '" + trackName + "' is muted or not soloed, so it produces no audio.";
                    return {};
                }

                juce::AudioBuffer<float> buf;
                double rate = 0.0;
                if (! readWavFile (stemFile, buf, rate, error))
                    return {};
                return analyze::analyzeBuffer (buf, rate > 0.0 ? rate : session.sampleRate, target, targetLufs).toVar();
            }

            RenderOptions opts;
            opts.folder = tempGuard.dir;
            opts.writeStems = true;
            opts.writeMix = true;
            opts.bits = 32;
            RenderResult res;
            if (! session.renderStems (opts, res, error))
                return {};

            analyze::AnalysisResult mixAnalysis;
            double effectiveRate = 0.0;
            {
                juce::AudioBuffer<float> mixBuf;
                double mixRate = 0.0;
                if (! readWavFile (juce::File (res.mixFile), mixBuf, mixRate, error))
                    return {};
                effectiveRate = mixRate > 0.0 ? mixRate : session.sampleRate;
                mixAnalysis = analyze::analyzeBuffer (mixBuf, effectiveRate, target, targetLufs);
            }

            std::vector<analyze::TrackSummary> trackSummaries;
            std::vector<std::array<double, pdsp::bands::kCount>> trackBands;
            trackSummaries.reserve (res.stems.size());
            trackBands.reserve (res.stems.size());
            double loudestBandDb = -120.0;
            {
                juce::AudioBuffer<float> sb;
                for (const auto& s : res.stems)
                {
                    double sRate = 0.0;
                    if (! readWavFile (juce::File (s.file), sb, sRate, error))
                        return {};
                    analyze::TrackSummary summary;
                    std::array<double, pdsp::bands::kCount> bands {};
                    analyze::analyzeTrackForMasking (s.track, sb, effectiveRate, summary, bands, loudestBandDb);
                    trackSummaries.push_back (std::move (summary));
                    trackBands.push_back (bands);
                }
            }
            const auto masking = analyze::buildMaskingResult (std::move (trackSummaries), trackBands, loudestBandDb);
            const auto maskingVar = masking.toVar();

            return object ({ { "mix", mixAnalysis.toVar() },
                             { "tracks", maskingVar["tracks"] },
                             { "masking", maskingVar } });
        }

        if (name == "render_stems")
        {
            if (! a.hasProperty ("folder") || ! a["folder"].isString() || a["folder"].toString().trim().isEmpty())
            { error = "Give 'folder': a non-empty folder path."; return {}; }
            RenderOptions o;
            o.folder = resolvePath (a["folder"].toString().trim());
            if (a.hasProperty ("tail_seconds"))
            {
                double tail = 0.0;
                if (! finiteNumber (a["tail_seconds"], tail) || tail < 0.0 || tail > 60.0)
                { error = "'tail_seconds' must be a number from 0 to 60."; return {}; }
                o.tailSeconds = tail;
            }
            if (a.hasProperty ("mix"))
            {
                if (! a["mix"].isBool()) { error = "'mix' must be true or false."; return {}; }
                o.writeMix = (bool) a["mix"];
            }
            if (a.hasProperty ("bits"))
            {
                int bits = 0;
                if (! wholeNumber (a["bits"], bits) || (bits != 16 && bits != 24 && bits != 32))
                { error = "'bits' must be 16, 24 or 32."; return {}; }
                o.bits = bits;
            }
            RenderResult r;
            if (! session.renderStems (o, r, error)) return {};
            juce::Array<juce::var> stems;
            for (auto& s : r.stems) stems.add (object ({ { "track", s.track }, { "file", s.file }, { "peak_db", s.peakDb } }));
            return object ({ { "stems", stems }, { "mix", r.mixFile }, { "mix_peak_db", r.mixPeakDb }, { "seconds", r.seconds } });
        }

        // everything below names a track: verify tool name before resolving track
        const bool isCopyChain = (name == "copy_chain");
        const bool isTrackTool = isCopyChain || name == "remove_track" || name == "set_track"
                                 || name == "add_clip" || name == "move_clip" || name == "split_clip"
                                 || name == "trim_clip" || name == "delete_clip" || name == "set_clip"
                                 || name == "add_plugin" || name == "set_chain" || name == "move_plugin"
                                 || name == "remove_plugin" || name == "bypass_plugin"
                                 || name == "list_parameters" || name == "set_parameter"
                                 || name == "get_plugin_state" || name == "set_plugin_state"
                                 || name == "list_presets" || name == "load_preset";
        if (! isTrackTool)
        {
            error = "Unknown tool '" + name + "'.";
            return {};
        }

        const auto trackArg = isCopyChain ? (a.hasProperty ("from_track") ? a["from_track"] : a["track"]) : a["track"];
        if (trackArg.isVoid())
        {
            error = isCopyChain ? "Give 'from_track' (a track number from 0, or a track name)."
                                : "Give 'track' (a track number from 0, or a track name).";
            return {};
        }
        if (isCopyChain)
        {
            const int from = session.findTrack (trackArg, error);
            if (from < 0) return {};
            const auto* arr = a["to_tracks"].getArray();
            if (arr == nullptr) { error = "Give 'to_tracks' (an array of track numbers or names)."; return {}; }
            if (arr->isEmpty()) { error = "Give at least one target track in 'to_tracks'."; return {}; }
            if (arr->size() > kMaxChainTargets)
            { error = "copy_chain writes at most " + juce::String (kMaxChainTargets) + " tracks in one call ('to_tracks' has " + juce::String (arr->size()) + ")."; return {}; }
            std::vector<int> to;
            for (auto& t : *arr)
            {
                const int i = session.findTrack (t, error);
                if (i < 0) return {};
                to.push_back (i);
            }
            if (! session.copyChain (from, to, error)) return {};
            editor.clearUndo(); // a chain is not undone (see Edit.h)
            return session.describe();
        }
        const int track = session.findTrack (trackArg, error);
        if (track < 0) return {};

        if (name == "remove_track")
        {
            if (! session.removeTrack (track, error)) return {};
            editor.clearUndo(); // tracks are not undone (see Edit.h)
            editor.clearClipboard(); // as the window does: the clipboard remembers the copy's track by number, and the tracks after this one have moved up
            return session.describe();
        }
        if (name == "set_track")
        {
            // every value is checked before anything is applied: a call that fails changes nothing and pushes no undo step
            double gainDb = 0.0, pan = 0.0;
            if (a.hasProperty ("gain_db") && (! finiteNumber (a["gain_db"], gainDb) || ! std::isfinite ((float) gainDb)))
            { error = "'gain_db' must be a finite number."; return {}; }
            if (a.hasProperty ("pan") && (! finiteNumber (a["pan"], pan) || ! std::isfinite ((float) pan)))
            { error = "'pan' must be a finite number (-1 to 1)."; return {}; }
            if (a.hasProperty ("mute") && ! a["mute"].isBool())
            { error = "'mute' must be true or false."; return {}; }
            if (a.hasProperty ("solo") && ! a["solo"].isBool())
            { error = "'solo' must be true or false."; return {}; }
            const bool renaming = a.hasProperty ("name") && a["name"].toString().isNotEmpty();
            if (renaming && a["name"].toString().trim().isEmpty()) { error = "The name is empty."; return {}; }
            // one gesture: the settings given are one undo step
            Gesture g (editor, "Track settings");
            if (a.hasProperty ("gain_db") && ! editor.setTrackGain (track, (float) gainDb, error)) return {};
            if (a.hasProperty ("pan") && ! editor.setTrackPan (track, (float) pan, error)) return {};
            if (a.hasProperty ("mute") && ! editor.setTrackMute (track, (bool) a["mute"], error)) return {};
            if (a.hasProperty ("solo") && ! editor.setTrackSolo (track, (bool) a["solo"], error)) return {};
            if (renaming && ! editor.renameTrack (track, a["name"].toString(), error)) return {};
            return session.describe();
        }

        // clips: numbered from 0 on the track; positions as seconds or beats (see position)
        if (name == "add_clip")
        {
            if (! a.hasProperty ("file") || ! a["file"].isString() || a["file"].toString().trim().isEmpty())
            { error = "Give 'file': the absolute path of an audio file."; return {}; }
            const auto file = resolvePath (a["file"].toString());

            const bool inSeconds = a.hasProperty ("start_seconds");
            const bool inBeats = a.hasProperty ("start_beats");
            if (inSeconds && inBeats) { error = "Give 'start_seconds' or 'start_beats' (exactly one)."; return {}; }
            juce::int64 startSample = 0;
            if (inSeconds || inBeats)
            {
                const std::string key = inSeconds ? "start_seconds" : "start_beats";
                double val = 0.0;
                if (! finiteNumber (a[key.c_str()], val) || val < 0.0)
                { error = juce::String (key.c_str()) + " must be a number 0 or more."; return {}; }
                if (val > 0.0)
                {
                    const auto s = samplesOf (a, key, inBeats, error);
                    if (! s) return {};
                    startSample = *s;
                }
            }

            juce::int64 offset = 0, length = 0;
            if (a.hasProperty ("offset_seconds"))
            {
                double val = 0.0;
                if (! finiteNumber (a["offset_seconds"], val) || val < 0.0)
                { error = "offset_seconds must be a number 0 or more."; return {}; }
                if (val > 0.0)
                {
                    const auto v = samplesOf (a, "offset_seconds", false, error);
                    if (! v) return {};
                    offset = *v;
                }
            }
            if (a.hasProperty ("length_seconds"))
            {
                const auto v = samplesOf (a, "length_seconds", false, error);
                if (! v) return {};
                if (*v == 0) { error = "'length_seconds' must be more than 0 (leave it out to play to the end of the file)."; return {}; }
                length = *v;
            }
            if (! editor.addClip (track, file, startSample, offset, length, error).valid()) return {};
            return session.describe();
        }
        if (name == "move_clip")
        {
            const int clip = indexArg (a, "clip", error);
            if (clip < 0) return {};
            const auto start = position (a, "start", error);
            if (! start) return {};
            int to = track;
            if (a.hasProperty ("to_track"))
            {
                to = session.findTrack (a["to_track"], error);
                if (to < 0) return {};
            }
            if (! editor.moveClipToTrack (track, clip, to, *start, error).valid()) return {};
            return session.describe();
        }
        if (name == "split_clip")
        {
            const int clip = indexArg (a, "clip", error);
            if (clip < 0) return {};
            const auto at = position (a, "at", error);
            if (! at) return {};
            if (! editor.splitClip (track, clip, *at, error).valid()) return {};
            return session.describe();
        }
        if (name == "trim_clip")
        {
            const int clip = indexArg (a, "clip", error);
            if (clip < 0) return {};
            const bool hasStart = a.hasProperty ("start_seconds") || a.hasProperty ("start_beats");
            const bool hasEnd = a.hasProperty ("end_seconds") || a.hasProperty ("end_beats");
            if (! hasStart && ! hasEnd) { error = "Give the new 'start_seconds' or 'start_beats', or 'end_seconds' or 'end_beats' (or both)."; return {}; }
            std::optional<juce::int64> start, end;
            if (hasStart)
            {
                start = position (a, "start", error);
                if (! start) return {};
            }
            if (hasEnd)
            {
                end = position (a, "end", error);
                if (! end) return {};
            }
            Gesture g (editor, "Trim clip");
            if (start && ! editor.trimStart (track, clip, *start, error)) return {};
            if (end && ! editor.trimEnd (track, clip, *end, error)) return {};
            return session.describe();
        }
        if (name == "delete_clip")
        {
            const int clip = indexArg (a, "clip", error);
            if (clip < 0) return {};
            return editor.deleteClip (track, clip, error) ? session.describe() : juce::var();
        }
        if (name == "set_clip")
        {
            const int clip = indexArg (a, "clip", error);
            if (clip < 0) return {};
            const bool given = a.hasProperty ("gain_db") || a.hasProperty ("fade_in_seconds") || a.hasProperty ("fade_out_seconds")
                               || a.hasProperty ("mute") || a.hasProperty ("name");
            if (! given) { error = "Give at least one of 'gain_db', 'fade_in_seconds', 'fade_out_seconds', 'mute' or 'name'."; return {}; }
            double gainDb = 0.0;
            if (a.hasProperty ("gain_db") && (! finiteNumber (a["gain_db"], gainDb) || ! std::isfinite ((float) gainDb)))
            { error = "'gain_db' must be a finite number."; return {}; }
            if (a.hasProperty ("mute") && ! a["mute"].isBool())
            { error = "'mute' must be true or false."; return {}; }
            if (a.hasProperty ("name") && a["name"].toString().trim().isEmpty()) { error = "The name is empty."; return {}; }

            // every value is read and checked before anything changes
            std::optional<juce::int64> fadeIn, fadeOut;
            if (a.hasProperty ("fade_in_seconds"))
            {
                fadeIn = samplesOf (a, "fade_in_seconds", false, error);
                if (! fadeIn) return {};
            }
            if (a.hasProperty ("fade_out_seconds"))
            {
                fadeOut = samplesOf (a, "fade_out_seconds", false, error);
                if (! fadeOut) return {};
            }
            juce::int64 currentIn = 0, currentOut = 0;
            {
                const juce::ScopedLock sl (session.lock);
                const auto& clips = session.tracks[(size_t) track]->clips;
                if (! juce::isPositiveAndBelow (clip, (int) clips.size()))
                {
                    error = "No clip " + juce::String (clip) + " on track " + juce::String (track) + " (it has " + juce::String ((int) clips.size()) + ")";
                    return {};
                }
                currentIn = clips[(size_t) clip].fadeIn;
                currentOut = clips[(size_t) clip].fadeOut;
            }

            Gesture g (editor, "Clip settings");
            if (a.hasProperty ("gain_db") && ! editor.setClipGain (track, clip, (float) gainDb, error)) return {};
            if ((fadeIn || fadeOut) && ! editor.setClipFades (track, clip, fadeIn ? *fadeIn : currentIn, fadeOut ? *fadeOut : currentOut, error)) return {};
            if (a.hasProperty ("mute") && ! editor.setClipMute (track, clip, (bool) a["mute"], error)) return {};
            if (a.hasProperty ("name") && ! editor.renameClip (track, clip, a["name"].toString(), error)) return {};
            return session.describe();
        }
        if (name == "add_plugin")
        {
            if (! a["plugin"].isString() || a["plugin"].toString().trim().isEmpty())
            { error = "Give 'plugin' (plugin name)."; return {}; }
            int pos = -1;
            if (a.hasProperty ("position"))
            {
                pos = indexArg (a, "position", error);
                if (pos < 0) return {};
            }
            if (! session.addPlugin (track, a["plugin"].toString(), pos, error)) return {};
            editor.clearUndo(); // plugins are not undone (see Edit.h)
            return session.describe();
        }
        if (name == "set_chain")
        {
            const auto* arr = a["plugins"].getArray();
            if (arr == nullptr) { error = "Give 'plugins' (an array of plugin names, or [] to clear the chain)."; return {}; }
            if (arr->size() > kMaxPluginsPerChain)
            { error = "A chain takes at most " + juce::String (kMaxPluginsPerChain) + " plugins in one call ('plugins' has " + juce::String (arr->size()) + ")."; return {}; }
            juce::StringArray plugins;
            for (const auto& p : *arr)
            {
                if (! p.isString()) { error = "Each entry in 'plugins' must be a plugin name (string)."; return {}; }
                plugins.add (p.toString());
            }
            if (! session.setChain (track, plugins, error)) return {};
            editor.clearUndo(); // plugins are not undone (see Edit.h)
            return session.describe();
        }
        if (name == "move_plugin")
        {
            const int from = indexArg (a, "from", error);
            if (from < 0) return {};
            const int to = indexArg (a, "to", error);
            if (to < 0) return {};
            return session.movePlugin (track, from, to, error) ? session.describe() : juce::var();
        }

        const int slot = findSlot (track, a["slot"], error);
        if (slot < 0) return {};
        if (name == "remove_plugin")
        {
            if (! session.removePlugin (track, slot, error)) return {};
            editor.clearUndo(); // plugins are not undone (see Edit.h)
            return session.describe();
        }
        if (name == "bypass_plugin")
        {
            if (! a.hasProperty ("bypass") || ! a["bypass"].isBool()) { error = "Give 'bypass' (true or false)."; return {}; }
            return session.setBypass (track, slot, (bool) a["bypass"], error) ? session.describe() : juce::var();
        }

        // What these tools do to the arguments is decoded first, outside the lock below (base64 text and a preset name are the
        // caller's, not the session's), with the reasons unchanged.
        juce::MemoryBlock stateFromArg;
        if (name == "set_plugin_state")
        {
            const auto text = a["state"].toString().trim();
            if (text.isEmpty() || ! stateFromArg.fromBase64Encoding (text) || stateFromArg.getSize() == 0)
            {
                error = "Give 'state' as the base64 text from get_plugin_state.";
                return {};
            }
        }
        juce::String wantedPreset;
        if (name == "load_preset" && ! nameOrIndexArg (a["preset"], wantedPreset, error, "Give 'preset' (name or number from list_presets)."))
            return {};

        // The instance belongs to the session: anything that reads or changes one, and the dirty flag, holds its lock, as every
        // other chain change does (one Session, three users). This is the message-thread path JUCE expects a plugin to be touched
        // from; the audio callback only tries the lock and goes silent for a block, so the hold here is a momentary one.
        const juce::ScopedLock sl (session.lock);
        auto* instance = session.tracks[(size_t) track]->chain[(size_t) slot]->instance.get();
        if (instance == nullptr) { error = "That plugin is not installed on this machine, so it has no settings to read or change."; return {}; }
        if (name == "list_parameters")
            return parameters (*instance);
        if (name == "set_parameter")
            return setParameter (*instance, a, error);
        if (name == "get_plugin_state")
        {
            juce::MemoryBlock state;
            instance->getStateInformation (state);
            return object ({ { "plugin", instance->getName() }, { "bytes", (int) state.getSize() }, { "state", state.toBase64Encoding() } });
        }
        if (name == "set_plugin_state")
        {
            session.dirty = true;
            instance->setStateInformation (stateFromArg.getData(), (int) stateFromArg.getSize());
            return parameters (*instance);
        }
        if (name == "list_presets")
        {
            juce::Array<juce::var> list;
            for (int i = 0; i < instance->getNumPrograms(); ++i)
                list.add (object ({ { "index", i }, { "name", instance->getProgramName (i) } }));
            return object ({ { "current", instance->getCurrentProgram() }, { "presets", list } });
        }
        if (name == "load_preset")
        {
            int found = -1;
            for (int i = 0; i < instance->getNumPrograms() && found < 0; ++i)
                if (instance->getProgramName (i).equalsIgnoreCase (wantedPreset)) found = i;
            if (found < 0)
                if (const auto idx = parseNonNegativeInt (wantedPreset))
                    found = *idx;
            if (! juce::isPositiveAndBelow (found, instance->getNumPrograms())) { error = "No preset '" + wantedPreset + "' (see list_presets)."; return {}; }
            session.dirty = true;
            instance->setCurrentProgram (found);
            return parameters (*instance);
        }
        error = "Unknown tool '" + name + "'.";
        return {};
    }

    static bool finiteNumber (const juce::var& v, double& out) noexcept
    {
        if (! (v.isInt() || v.isInt64() || v.isDouble()))
            return false;
        out = (double) v;
        return std::isfinite (out);
    }

    static bool wholeNumber (const juce::var& v, int& out) noexcept
    {
        if (v.isInt()) { out = (int) v; return true; }
        if (v.isInt64())
        {
            const auto n = (juce::int64) v;
            if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max()) return false;
            out = (int) n;
            return true;
        }
        if (v.isDouble())
        {
            const double d = (double) v;
            if (! std::isfinite (d) || std::trunc (d) != d
                || d < (double) std::numeric_limits<int>::min() || d > (double) std::numeric_limits<int>::max())
                return false;
            out = (int) d;
            return true;
        }
        return false;
    }

    static bool nameOrIndexArg (const juce::var& v, juce::String& out, juce::String& error, const char* prompt)
    {
        if (v.isString())
        {
            out = v.toString().trim();
            if (out.isNotEmpty())
                return true;
            error = prompt;
            return false;
        }
        if (v.isInt() || v.isInt64())
        {
            const juce::int64 n = (juce::int64) v;
            if (n < 0 || n > (juce::int64) std::numeric_limits<int>::max())
            {
                error = prompt;
                return false;
            }
            out = juce::String ((int) n);
            return true;
        }
        if (v.isDouble())
        {
            const double d = (double) v;
            if (! std::isfinite (d) || std::trunc (d) != d || d < 0.0 || d > (double) std::numeric_limits<int>::max())
            {
                error = prompt;
                return false;
            }
            out = juce::String ((int) d);
            return true;
        }
        error = prompt;
        return false;
    }

    static std::optional<int> parseNonNegativeInt (const juce::String& s)
    {
        if (s.isEmpty() || ! s.containsOnly ("0123456789")) return {};
        juce::int64 parsed = 0;
        for (int k = 0; k < s.length(); ++k)
        {
            const int d = (int) (s[k] - '0');
            if (parsed > (std::numeric_limits<int>::max() - d) / 10) return {};
            parsed = parsed * 10 + d;
        }
        return (int) parsed;
    }

    // A number from 0 given as `key`, or -1 with the reason (missing, not a number, or negative).
    static int indexArg (const juce::var& a, const char* key, juce::String& error)
    {
        if (! a.hasProperty (key)) { error = juce::String ("Give '") + key + "' (a number, from 0)."; return -1; }
        const auto& v = a[key];
        if (v.isInt())
        {
            const int i = (int) v;
            if (i < 0) { error = juce::String ("'") + key + "' must be 0 or more (numbered from 0)."; return -1; }
            return i;
        }
        if (v.isInt64())
        {
            const juce::int64 n = (juce::int64) v;
            if (n < 0) { error = juce::String ("'") + key + "' must be 0 or more (numbered from 0)."; return -1; }
            if (n > (juce::int64) std::numeric_limits<int>::max()) { error = juce::String ("'") + key + "' is too large."; return -1; }
            return (int) n;
        }
        if (v.isDouble())
        {
            const double d = (double) v;
            if (! std::isfinite (d) || std::trunc (d) != d) { error = juce::String ("'") + key + "' must be a whole number, from 0."; return -1; }
            if (d < 0.0) { error = juce::String ("'") + key + "' must be 0 or more (numbered from 0)."; return -1; }
            if (d > (double) std::numeric_limits<int>::max()) { error = juce::String ("'") + key + "' is too large."; return -1; }
            return (int) d;
        }
        error = juce::String ("'") + key + "' must be a number, from 0.";
        return -1;
    }

    // A time given as the number `name` ("start_seconds" or "start_beats"), as samples at the session's rate. Nothing when it is not a
    // number, is negative, or the session has no sample rate yet (no track has been added).
    std::optional<juce::int64> samplesOf (const juce::var& a, const std::string& name, bool beats, juce::String& error) const
    {
        const auto& v = a[name.c_str()];
        if (! (v.isInt() || v.isInt64() || v.isDouble())) { error = juce::String (name.c_str()) + " must be a number."; return {}; }
        const double value = (double) v;
        if (! std::isfinite (value) || value < 0.0) { error = juce::String (name.c_str()) + " must be 0 or more."; return {}; }
        const juce::ScopedLock sl (session.lock);
        if (session.sampleRate <= 0.0) { error = "The session has no sample rate yet, so no time can be placed: add a track first."; return {}; }
        constexpr double kMaxSamples = (double) (std::numeric_limits<juce::int64>::max() / 4);
        if (! std::isfinite (value * session.sampleRate) || std::abs (value * session.sampleRate) > kMaxSamples)
        {
            error = juce::String (name.c_str()) + " is too large.";
            return {};
        }
        double samples = 0.0;
        if (! beats)
        {
            samples = value * session.sampleRate;
        }
        else
        {
            TempoMap map = session.tempo;
            map.sampleRate = session.sampleRate;
            samples = map.beatsToSamples (value);
        }
        if (! std::isfinite (samples) || std::abs (samples) > kMaxSamples)
        {
            error = juce::String (name.c_str()) + " is too large.";
            return {};
        }
        return (juce::int64) std::llround (samples);
    }

    // A position on the timeline: `key`_seconds or `key`_beats, exactly one of them.
    std::optional<juce::int64> position (const juce::var& a, const std::string& key, juce::String& error) const
    {
        const bool inSeconds = a.hasProperty ((key + "_seconds").c_str());
        const bool inBeats = a.hasProperty ((key + "_beats").c_str());
        if (inSeconds == inBeats) { error = juce::String (("Give '" + key + "_seconds' or '" + key + "_beats' (exactly one).").c_str()); return {}; }
        return inSeconds ? samplesOf (a, key + "_seconds", false, error) : samplesOf (a, key + "_beats", true, error);
    }

    int findSlot (int track, const juce::var& which, juce::String& error) const
    {
        auto& chain = session.tracks[(size_t) track]->chain;
        int i = -1;
        if (which.isString())
        {
            const auto s = which.toString().trim();
            if (s.isEmpty())
            {
                error = "Give 'slot' (a plugin number from 0, or a plugin name).";
                return -1;
            }
            if (! s.containsOnly ("0123456789"))
            {
                for (int slot = 0; slot < (int) chain.size(); ++slot)
                    if (chain[(size_t) slot]->plugin.equalsIgnoreCase (s)) return slot;
                int prefixMatch = -1;
                int prefixCount = 0;
                for (int slot = 0; slot < (int) chain.size(); ++slot)
                {
                    if (chain[(size_t) slot]->plugin.startsWithIgnoreCase (s))
                    {
                        if (prefixMatch < 0) prefixMatch = slot;
                        ++prefixCount;
                    }
                }
                if (prefixCount == 1) return prefixMatch;
                if (prefixCount > 1)
                {
                    error = "Plugin name '" + which.toString() + "' is ambiguous on that track (matches " + juce::String (prefixCount) + " slots); use a slot number.";
                    return -1;
                }
                error = "No plugin '" + which.toString() + "' on that track";
                return -1;
            }
            if (const auto parsed = parseNonNegativeInt (s))
                i = *parsed;
            else
            {
                error = "No plugin " + s + " on that track (it has " + juce::String ((int) chain.size()) + ", numbered from 0)";
                return -1;
            }
        }
        else if (which.isInt())
        {
            i = (int) which;
        }
        else if (which.isInt64())
        {
            const juce::int64 n = (juce::int64) which;
            if (n < 0 || n > (juce::int64) std::numeric_limits<int>::max())
            {
                error = "No plugin " + which.toString() + " on that track (it has " + juce::String ((int) chain.size()) + ", numbered from 0)";
                return -1;
            }
            i = (int) n;
        }
        else if (which.isDouble())
        {
            const double d = (double) which;
            if (! std::isfinite (d) || std::trunc (d) != d)
            {
                error = "'slot' must be a whole number (from 0) or a plugin name.";
                return -1;
            }
            if (d < 0.0 || d > (double) std::numeric_limits<int>::max())
            {
                error = "No plugin " + which.toString() + " on that track (it has " + juce::String ((int) chain.size()) + ", numbered from 0)";
                return -1;
            }
            i = (int) d;
        }
        else
        {
            error = "Give 'slot' (a plugin number from 0, or a plugin name).";
            return -1;
        }
        if (juce::isPositiveAndBelow (i, (int) chain.size())) return i;
        error = "No plugin " + juce::String (i) + " on that track (it has " + juce::String ((int) chain.size()) + ", numbered from 0)";
        return -1;
    }

    static juce::String idOf (const juce::AudioProcessorParameter& p)
    {
        if (auto* withId = dynamic_cast<const juce::HostedAudioProcessorParameter*> (&p)) return withId->getParameterID();
        return juce::String (p.getParameterIndex());
    }

    static juce::var parameters (juce::AudioPluginInstance& instance)
    {
        juce::Array<juce::var> list;
        int i = 0;
        for (auto* p : instance.getParameters())
        {
            list.add (object ({ { "index", i++ }, { "id", idOf (*p) }, { "name", p->getName (64) }, { "value", (double) p->getValue() },
                                { "text", p->getCurrentValueAsText() }, { "label", p->getLabel() }, { "steps", p->getNumSteps() } }));
        }
        return object ({ { "plugin", instance.getName() }, { "parameters", list } });
    }

    // Does the text the plugin shows say what the caller wrote? JUCE's default text-to-value converter reads what it cannot parse as
    // 0 (and clamps what is out of range), so a value is only taken when the plugin's own read-out matches the text back: exactly,
    // ignoring the space a unit leaves ("100 %" for "100%"), or as the same number written with fewer decimals ("-6" for "-6.00").
    // A plugin whose converter means something else entirely is refused, which is the honest answer to text we cannot confirm.
    static bool textAsShown (const juce::String& shownIn, const juce::String& wantedIn)
    {
        const auto shown = shownIn.trim(), wanted = wantedIn.trim();
        if (shown.equalsIgnoreCase (wanted)) return true;
        const auto shownTight = shown.removeCharacters (" \t"), wantedTight = wanted.removeCharacters (" \t");
        if (shownTight.equalsIgnoreCase (wantedTight)) return true;
        if (! wantedTight.containsAnyOf ("0123456789") || ! shownTight.containsAnyOf ("0123456789")) return false;
        // the number is only the leading part: getFloatValue reads a prefix and ignores the rest, so "12abc" would pass as 12.
        // Split both at the end of their number and require the leftovers to agree too ("dB" for "dB", nothing for nothing).
        const auto splitNumber = [] (const juce::String& t)
        {
            int i = 0;
            if (i < t.length() && (t[i] == '+' || t[i] == '-')) ++i;
            while (i < t.length() && juce::CharacterFunctions::isDigit (t[i])) ++i;
            if (i < t.length() && t[i] == '.') { ++i; while (i < t.length() && juce::CharacterFunctions::isDigit (t[i])) ++i; }
            if (i < t.length() && (t[i] == 'e' || t[i] == 'E'))
            {
                int j = i + 1;
                if (j < t.length() && (t[j] == '+' || t[j] == '-')) ++j;
                if (j < t.length() && juce::CharacterFunctions::isDigit (t[j])) { while (j < t.length() && juce::CharacterFunctions::isDigit (t[j])) ++j; i = j; }
            }
            return std::make_pair (t.substring (0, i), t.substring (i));
        };
        const auto askedParts = splitNumber (wantedTight), saysParts = splitNumber (shownTight);
        if (askedParts.first.isEmpty() || saysParts.first.isEmpty()) return false;
        const double asked = askedParts.first.getDoubleValue(), says = saysParts.first.getDoubleValue();
        return std::isfinite (asked) && std::isfinite (says)
            && std::abs (asked - says) <= 0.001 * std::max (1.0, std::abs (says))
            && askedParts.second.equalsIgnoreCase (saysParts.second);
    }

    juce::var setParameter (juce::AudioPluginInstance& instance, const juce::var& a, juce::String& error)
    {
        juce::String which;
        if (! nameOrIndexArg (a["parameter"], which, error, "Give 'parameter' (id, name or number from list_parameters)."))
            return {};
        juce::AudioProcessorParameter* found = nullptr;
        for (auto* p : instance.getParameters())
            if (idOf (*p).equalsIgnoreCase (which) || p->getName (64).equalsIgnoreCase (which)) { found = p; break; }
        if (found == nullptr)
            if (const auto idx = parseNonNegativeInt (which))
                found = juce::isPositiveAndBelow (*idx, instance.getParameters().size()) ? instance.getParameters()[*idx] : nullptr;
        if (found == nullptr) { error = "No setting '" + which + "' (see list_parameters)."; return {}; }
        const float previous = found->getValue();
        juce::String wantedText; // stays empty when the value came as a number: the check below is only for the text path
        float value = 0.0f;
        if (a.hasProperty ("value"))
        {
            double d = 0.0;
            if (! finiteNumber (a["value"], d) || d < 0.0 || d > 1.0)
            { error = "'value' must be a number in 0..1."; return {}; }
            value = (float) d;
        }
        else if (a["text"].isString() && a["text"].toString().isNotEmpty())
        {
            wantedText = a["text"].toString().trim();
            value = found->getValueForText (a["text"].toString());
            if (! std::isfinite (value))
            { error = "Could not parse parameter text '" + a["text"].toString() + "'."; return {}; }
            value = juce::jlimit (0.0f, 1.0f, value);
        }
        else { error = "Give 'value' (0..1) or 'text'."; return {}; }
        found->beginChangeGesture();
        found->setValueNotifyingHost (value);
        found->endChangeGesture();
        // Text is the plugin's to interpret, so the plugin's own read-out decides whether it took: JUCE's default converter reads
        // what it cannot parse (and what is out of range) as an end of the range, which would set a value nobody asked for and
        // report it as a success. Nothing is left changed when it does not match.
        if (wantedText.isNotEmpty() && ! textAsShown (found->getCurrentValueAsText(), wantedText))
        {
            const auto shownNow = found->getCurrentValueAsText(); // what the plugin read the text as, before the restore puts the old value back
            found->beginChangeGesture();
            found->setValueNotifyingHost (previous);
            found->endChangeGesture();
            error = "The plugin does not read '" + wantedText + "' as a value of '" + idOf (*found) + "' (it shows '"
                        + shownNow + "'), so nothing changed. Give 'value' as a number 0..1 (see list_parameters).";
            return {};
        }
        session.dirty = true;
        return object ({ { "id", idOf (*found) }, { "name", found->getName (64) }, { "value", (double) found->getValue() }, { "text", found->getCurrentValueAsText() } });
    }
};
}
