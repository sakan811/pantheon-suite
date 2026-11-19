// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Session.h"

#include <iostream>

// A Model Context Protocol server over stdio (one JSON message per line, JSON-RPC 2.0), so an agent can build the session of a
// song: add the stems, put a chain of the suite's plugins on each, set the plugins' settings, save the project and render the
// stems. It edits the project the way the window does (the same Session), and a saved project opens in the window for the
// editing that is left. It does not play audio, and it reads and writes only the paths it is given.
namespace olympus
{
class McpServer
{
public:
    explicit McpServer (Session& s) : session (s) {}

    // One incoming message; the reply, or a void var for a notification (which has none).
    juce::var handle (const juce::var& request)
    {
        const auto method = request["method"].toString();
        const auto id = request["id"];
        const bool isNotification = ! request.hasProperty ("id");
        if (method == "initialize")
        {
            auto requested = request["params"]["protocolVersion"].toString();
            auto result = object ({ { "protocolVersion", requested.isNotEmpty() ? requested : juce::String ("2025-06-18") },
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
        if (isNotification)
            return {};
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

    static juce::String instructions()
    {
        return "Olympus is a session host for the Pantheon plugin suite. Typical use: new_project, add_track for each stem (a WAV or other audio file), "
               "set_chain or add_plugin to put plugins on a stem, set_parameter / load_preset to set them, save_project (the user opens the .olympus file "
               "in the Olympus window to edit further), render_stems to write one WAV per stem. Tracks and plugin slots are numbered from 0; a track may also "
               "be named, a plugin may be given by the start of its name ('hermes'). Call list_plugins first: only plugins that are installed can be used. "
               "Parameter values are 0..1, or text as the plugin shows it ('-3.0 dB'). Nothing is played: this server only edits and renders.";
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
        list.add (tool ("list_plugins", "The suite's plugins that are installed and can be put on a track.", {}));
        list.add (tool ("project_info", "The whole session: tracks, faders, chains, latency.", {}));
        list.add (tool ("new_project", "Empties the session.", { { "name", prop ("string", "Project name") } }));
        list.add (tool ("open_project", "Opens a saved .olympus project.", { { "path", prop ("string", "Absolute path of the project file") } }, { "path" }));
        list.add (tool ("save_project", "Saves the project (to `path`, or where it was opened from).", { { "path", prop ("string", "Absolute path ending in .olympus") } }));
        list.add (tool ("add_track", "Adds a track from an audio file (every track becomes stereo at the session's sample rate).",
                        { { "file", prop ("string", "Absolute path of the audio file") }, { "name", prop ("string", "Track name (default: the file's name)") } }, { "file" }));
        list.add (tool ("remove_track", "Removes a track.", { { "track", trackRef } }, { "track" }));
        list.add (tool ("set_track", "Fader (dB), mute, solo or name of a track.",
                        { { "track", trackRef }, { "gain_db", prop ("number", "Fader in dB") }, { "mute", prop ("boolean", "") }, { "solo", prop ("boolean", "") }, { "name", prop ("string", "") } }, { "track" }));
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
        list.add (tool ("render_stems", "Writes one WAV per audible track (after its chain and fader, aligned to the audio) and mix.wav, into a folder.",
                        { { "folder", prop ("string", "Absolute path of the folder (made if needed)") }, { "tail_seconds", prop ("number", "Extra time at the end for tails; default 0") },
                          { "mix", prop ("boolean", "Also write mix.wav; default true") }, { "bits", prop ("integer", "16, 24 or 32 (float, default)") } }, { "folder" }));
        return list;
    }

    juce::var call (const juce::String& name, const juce::var& a, juce::String& error)
    {
        if (name == "list_plugins")
        {
            juce::Array<juce::var> names;
            for (auto& n : session.catalog.names()) names.add (n);
            return object ({ { "installed", names }, { "note", names.isEmpty() ? "None found: the suite's plugins are built or installed as VST3 bundles; see the Olympus README for the folders searched." : "" } });
        }
        if (name == "project_info")
            return session.describe();
        if (name == "new_project")
        {
            session.tracks.clear();
            session.sampleRate = 0.0;
            session.projectFile = juce::File();
            session.name = a["name"].toString().isNotEmpty() ? a["name"].toString() : "Untitled";
            session.dirty = false;
            return session.describe();
        }
        if (name == "open_project")
        {
            if (! session.load (juce::File (a["path"].toString()), error)) return {};
            auto d = session.describe();
            juce::Array<juce::var> w;
            for (auto& s : session.warnings) w.add (s);
            d.getDynamicObject()->setProperty ("warnings", w);
            return d;
        }
        if (name == "save_project")
        {
            const auto path = a["path"].toString();
            const auto file = path.isNotEmpty() ? juce::File (path) : session.projectFile;
            if (file == juce::File()) { error = "No path: this project has not been saved yet."; return {}; }
            if (! session.save (file.hasFileExtension ("olympus") ? file : file.withFileExtension ("olympus"), error)) return {};
            return object ({ { "saved", session.projectFile.getFullPathName() } });
        }
        if (name == "add_track")
        {
            if (! session.addTrack (juce::File (a["file"].toString()), a["name"].toString(), error)) return {};
            return session.describe();
        }

        // everything below names a track
        const auto trackArg = a.hasProperty ("from_track") ? a["from_track"] : a["track"];
        if (name == "render_stems" || trackArg.isVoid())
        {
            if (name == "render_stems")
            {
                RenderOptions o;
                o.folder = juce::File (a["folder"].toString());
                o.tailSeconds = a.hasProperty ("tail_seconds") ? (double) a["tail_seconds"] : 0.0;
                o.writeMix = a.hasProperty ("mix") ? (bool) a["mix"] : true;
                o.bits = a.hasProperty ("bits") ? (int) a["bits"] : 32;
                RenderResult r;
                if (! session.renderStems (o, r, error)) return {};
                juce::Array<juce::var> stems;
                for (auto& s : r.stems) stems.add (object ({ { "track", s.track }, { "file", s.file }, { "peak_db", s.peakDb } }));
                return object ({ { "stems", stems }, { "mix", r.mixFile }, { "mix_peak_db", r.mixPeakDb }, { "seconds", r.seconds } });
            }
            error = "Unknown tool '" + name + "' or a missing 'track'.";
            return {};
        }
        if (name == "copy_chain")
        {
            const int from = session.findTrack (trackArg, error);
            if (from < 0) return {};
            std::vector<int> to;
            for (auto& t : *a["to_tracks"].getArray())
            {
                const int i = session.findTrack (t, error);
                if (i < 0) return {};
                to.push_back (i);
            }
            if (! session.copyChain (from, to, error)) return {};
            return session.describe();
        }
        const int track = session.findTrack (trackArg, error);
        if (track < 0) return {};

        if (name == "remove_track")
            return session.removeTrack (track, error) ? session.describe() : juce::var();
        if (name == "set_track")
        {
            const juce::ScopedLock sl (session.lock);
            auto& t = *session.tracks[(size_t) track];
            if (a.hasProperty ("gain_db")) t.gainDb = (float) (double) a["gain_db"];
            if (a.hasProperty ("mute")) t.mute = (bool) a["mute"];
            if (a.hasProperty ("solo")) t.solo = (bool) a["solo"];
            if (a.hasProperty ("name") && a["name"].toString().isNotEmpty()) t.name = a["name"].toString();
            session.dirty = true;
            return session.describe();
        }
        if (name == "add_plugin")
            return session.addPlugin (track, a["plugin"].toString(), a.hasProperty ("position") ? (int) a["position"] : -1, error) ? session.describe() : juce::var();
        if (name == "set_chain")
        {
            juce::StringArray plugins;
            if (auto* arr = a["plugins"].getArray())
                for (auto& p : *arr) plugins.add (p.toString());
            return session.setChain (track, plugins, error) ? session.describe() : juce::var();
        }
        if (name == "move_plugin")
            return session.movePlugin (track, (int) a["from"], (int) a["to"], error) ? session.describe() : juce::var();

        const int slot = findSlot (track, a["slot"], error);
        if (slot < 0) return {};
        if (name == "remove_plugin")
            return session.removePlugin (track, slot, error) ? session.describe() : juce::var();
        if (name == "bypass_plugin")
            return session.setBypass (track, slot, (bool) a["bypass"], error) ? session.describe() : juce::var();

        auto* instance = session.tracks[(size_t) track]->chain[(size_t) slot]->instance.get();
        if (instance == nullptr) { error = "That plugin is not installed on this machine, so it has no settings to read or change."; return {}; }
        if (name == "list_parameters")
            return parameters (*instance);
        if (name == "set_parameter")
            return setParameter (*instance, a, error);
        if (name == "list_presets")
        {
            juce::Array<juce::var> list;
            for (int i = 0; i < instance->getNumPrograms(); ++i)
                list.add (object ({ { "index", i }, { "name", instance->getProgramName (i) } }));
            return object ({ { "current", instance->getCurrentProgram() }, { "presets", list } });
        }
        if (name == "load_preset")
        {
            const auto wanted = a["preset"].toString();
            int found = -1;
            for (int i = 0; i < instance->getNumPrograms() && found < 0; ++i)
                if (instance->getProgramName (i).equalsIgnoreCase (wanted)) found = i;
            if (found < 0 && wanted.containsOnly ("0123456789") && wanted.isNotEmpty()) found = wanted.getIntValue();
            if (! juce::isPositiveAndBelow (found, instance->getNumPrograms())) { error = "No preset '" + wanted + "' (see list_presets)."; return {}; }
            session.dirty = true;
            instance->setCurrentProgram (found);
            return parameters (*instance);
        }
        error = "Unknown tool '" + name + "'.";
        return {};
    }

    int findSlot (int track, const juce::var& which, juce::String& error) const
    {
        auto& chain = session.tracks[(size_t) track]->chain;
        if (which.isString() && ! which.toString().containsOnly ("0123456789"))
        {
            for (int i = 0; i < (int) chain.size(); ++i)
                if (chain[(size_t) i]->plugin.startsWithIgnoreCase (which.toString().trim())) return i;
            error = "No plugin '" + which.toString() + "' on that track";
            return -1;
        }
        const int i = which.isString() ? which.toString().getIntValue() : (int) which;
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

    juce::var setParameter (juce::AudioPluginInstance& instance, const juce::var& a, juce::String& error)
    {
        const auto which = a["parameter"].toString().trim();
        juce::AudioProcessorParameter* found = nullptr;
        int i = 0;
        for (auto* p : instance.getParameters())
        {
            if (idOf (*p).equalsIgnoreCase (which) || p->getName (64).equalsIgnoreCase (which)) { found = p; break; }
            ++i;
        }
        if (found == nullptr && which.containsOnly ("0123456789") && which.isNotEmpty())
            found = juce::isPositiveAndBelow (which.getIntValue(), instance.getParameters().size()) ? instance.getParameters()[which.getIntValue()] : nullptr;
        if (found == nullptr) { error = "No setting '" + which + "' (see list_parameters)."; return {}; }
        float value;
        if (a.hasProperty ("value")) value = (float) (double) a["value"];
        else if (a["text"].toString().isNotEmpty()) value = found->getValueForText (a["text"].toString());
        else { error = "Give 'value' (0..1) or 'text'."; return {}; }
        found->beginChangeGesture();
        found->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value));
        found->endChangeGesture();
        session.dirty = true;
        return object ({ { "id", idOf (*found) }, { "name", found->getName (64) }, { "value", (double) found->getValue() }, { "text", found->getCurrentValueAsText() } });
    }
};
}
