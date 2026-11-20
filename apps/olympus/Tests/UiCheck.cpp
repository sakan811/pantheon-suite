// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// The window driven without a screen: stems added, chains built, plugin windows opened, projects saved and opened, stems rendered, the notes
// and the layout. Headless: no audio device is opened, and the plugins are made in code (TestPlugin.h).
//
//   OlympusUiCheck

#include "Check.h"
#include "TestPlugin.h"
#include "UI/Arrange.h"
#include "UI/MainComponent.h"

using namespace olympus;
using namespace olympus::gui;

namespace test
{
inline void check (bool ok, const char* what) { check (ok, std::string (what)); }
inline void check (bool ok, const juce::String& what) { check (ok, what.toStdString()); }
}

namespace
{
std::vector<juce::String> gErrors, gAsked;
bool gAnswer = false;

juce::File tempFolder()
{
    static auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("olympus-uicheck-" + juce::String (juce::Time::currentTimeMillis()));
    folder.createDirectory();
    return folder;
}

juce::File makeWav (const juce::String& name, int length = 24000)
{
    juce::AudioBuffer<float> b (2, length);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < length; ++i)
            b.setSample (ch, i, 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * i / 48000.0));
    auto file = tempFolder().getChildFile (name);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (b, 0, length);
    return file;
}

std::unique_ptr<MainComponent> makeWindow (bool withPlugins = true)
{
    MainComponent::Options options;
    options.audioDevice = false;
    options.scanPlugins = false;
    options.settingsFile = tempFolder().getChildFile ("settings.xml");
    auto w = std::make_unique<MainComponent> (options);
    w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
    if (withPlugins) test::addTestPlugins (w->session.catalog);
    w->reportError = [] (const juce::String& m) { gErrors.push_back (m); };
    w->makeEditorWindow = [] (const juce::String&, juce::AudioProcessorEditor* editor, std::function<void()>) -> std::unique_ptr<juce::Component>
    {
        auto holder = std::make_unique<juce::Component>(); // no native window under test
        holder->addAndMakeVisible (editor);
        holder->setSize (editor->getWidth(), editor->getHeight());
        return holder;
    };
    w->ask = [] (const juce::String& message, const juce::String&, std::function<void()> onOk)
    {
        gAsked.push_back (message);
        if (gAnswer) onOk();
    };
    return w;
}

// A click, and the message loop for the moment the button takes to act on it (JUCE answers triggerClick asynchronously).
void press (juce::Button& b)
{
    b.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
}

bool noteMentions (MainComponent& w, const char* word, ui::Severity sev) { return ui::hasNote (w.notes(), word, sev); }

bool inside (const juce::Component& child, const juce::Component& parent)
{
    return parent.getLocalBounds().contains (child.getBounds());
}

void layoutCheck (MainComponent& w, const char* label)
{
    bool ok = true;
    for (auto* row : w.trackList.rows)
    {
        ok = ok && row->getBounds().getRight() <= w.trackList.getWidth() && row->getHeight() >= 84;
        for (auto* c : { (juce::Component*) &row->wave, (juce::Component*) &row->nameLabel, (juce::Component*) &row->muteButton, (juce::Component*) &row->soloButton,
                         (juce::Component*) &row->gainKnob, (juce::Component*) &row->addPluginButton })
            ok = ok && inside (*c, *row);
        for (auto* s : row->slotButtons) ok = ok && inside (*s, *row);
        // nothing sits on the waveform, and slots do not overlap each other
        for (auto* s : row->slotButtons)
        {
            ok = ok && ! s->getBounds().intersects (row->wave.getBounds());
            for (auto* o : row->slotButtons)
                if (o != s) ok = ok && ! s->getBounds().intersects (o->getBounds());
        }
    }
    for (auto* c : { (juce::Component*) &w.newButton, (juce::Component*) &w.renderButton, (juce::Component*) &w.guideButton, (juce::Component*) &w.playButton })
        ok = ok && inside (*c, w);
    ok = ok && ! w.renderButton.getBounds().intersects (w.guideButton.getBounds());
    test::check (ok, juce::String ("layout at ") + label + ": everything inside its panel, nothing overlapping");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::printf ("[1] Window shape\n");
    test::check (ui::hasEditorAspect (MainComponent::kDesignWidth, MainComponent::kDesignHeight) && MainComponent::kDesignWidth % 8 == 0, "16:10, the design width a multiple of 8");

    std::printf ("[2] Empty window\n");
    {
        auto w = makeWindow();
        test::check (w->windowTitle() == "Olympus - Untitled", "the title: " + w->windowTitle());
        test::check (noteMentions (*w, "Drop the stems", ui::Severity::info), "it says what to do first");
        auto none = makeWindow (false);
        test::check (noteMentions (*none, "No plugin of the suite", ui::Severity::warning), "with no plugin found it says where to look");
        for (auto& n : none->notes()) test::check ((int) n.text.size() <= ui::kMaxNoteChars, "a note fits its line: " + juce::String (n.text));
        layoutCheck (*w, "the design size");
        none->session.catalog.addInternal (test::GainPlugin::description ("Test Gain"), [] (double, int) { return std::make_unique<test::GainPlugin> (0); });
    }

    std::printf ("[3] Stems and chains\n");
    {
        auto w = makeWindow();
        const auto a = makeWav ("vocal.wav"), b = makeWav ("drums.wav");
        w->addTracks ({ a.getFullPathName(), b.getFullPathName() });
        test::check (w->trackList.rows.size() == 2 && w->session.tracks.size() == 2, "two files, two rows");
        test::check (w->trackList.rows[0]->nameLabel.getText() == "vocal", "a row is named after its file");
        test::check (w->windowTitle().endsWith ("*"), "a change puts a star in the title");

        juce::String error;
        w->session.addPlugin (0, "test gain", -1, error);
        w->session.addPlugin (0, "test delay", -1, error);
        w->rebuildNow();
        test::check (w->trackList.rows[0]->slotButtons.size() == 2 && w->trackList.rows[1]->slotButtons.isEmpty(), "the chain is shown as one button per plugin");
        test::check (w->trackList.rows[0]->slotButtons[0]->getButtonText() == "Test Gain", "a button carries the plugin's name");
        test::check (noteMentions (*w, "delay it by 64 samples", ui::Severity::info), "a latency is mentioned, with the alignment promise");

        press (*w->trackList.rows[0]->slotButtons[0]);
        test::check (w->openEditors() == 1, "a click on a plugin opens its window");
        press (*w->trackList.rows[0]->slotButtons[0]);
        test::check (w->openEditors() == 0, "and a second click closes it");
        w->openEditor (0, 1);
        w->openEditor (0, 1);
        test::check (w->openEditors() == 1, "opening one that is open brings it to the front, not a second window");
        w->closeEditors();

        press (w->trackList.rows[0]->muteButton);
        test::check (w->session.tracks[0]->mute, "MUTE");
        press (w->trackList.rows[1]->soloButton);
        test::check (w->session.tracks[1]->solo, "SOLO");
        test::check (noteMentions (*w, "", ui::Severity::warning) == false, "a solo with the rest muted is fine; nothing to warn about");
        w->trackList.rows[0]->gainKnob.setValue (-7.5, juce::sendNotificationSync);
        test::check (std::abs (w->session.tracks[0]->gainDb + 7.5f) < 1e-4f, "the fader knob");
        w->trackList.rows[0]->nameLabel.setText ("Lead vocal", juce::sendNotificationSync);
        test::check (w->session.tracks[0]->name == "Lead vocal", "the name can be edited");
        layoutCheck (*w, "the design size with chains");

        // a long chain wraps onto a second line, and the row grows
        for (int i = 0; i < 6; ++i) w->session.addPlugin (1, "test gain", -1, error);
        w->rebuildNow();
        test::check (w->trackList.rows[1]->getHeight() > w->trackList.rows[0]->getHeight(), "a long chain wraps and the row grows");
        layoutCheck (*w, "the design size with a long chain");
        w->setSize (1024, ui::editorHeightFor (1024));
        w->resized();
        layoutCheck (*w, "the smallest size");
        w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
        w->resized();

        // playhead
        w->trackList.rows[0]->wave.onSeek (0.5);
        test::check (w->position() == w->session.tracks[0]->length() / 2, "a click on a waveform puts the playhead there");
        w->togglePlay();
        test::check (w->isPlaying(), "PLAY");
        w->togglePlay();
        test::check (! w->isPlaying(), "STOP");
        w->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        test::check (w->isPlaying(), "the space bar plays");
        w->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));

        // removing a track while its plugin window is open
        w->openEditor (0, 0);
        press (w->trackList.rows[0]->removeButton);
        w->rebuildNow();
        test::check (w->openEditors() == 0 && w->session.tracks.size() == 1, "removing a track closes its plugin windows first");
    }

    std::printf ("[4] Project and render\n");
    {
        auto w = makeWindow();
        const auto a = makeWav ("keys.wav");
        w->addTracks ({ a.getFullPathName() });
        juce::String error;
        w->session.addPlugin (0, "test gain", -1, error);
        w->rebuildNow();
        const auto file = tempFolder().getChildFile ("song.olympus");
        test::check (w->saveTo (file) && ! w->session.dirty && w->windowTitle() == "Olympus - song", "save: the star goes: " + w->windowTitle());
        test::check (Settings::load (tempFolder().getChildFile ("settings.xml")).lastProject == file.getFullPathName(), "the last project is remembered");

        gAsked.clear();
        gAnswer = false;
        w->session.dirty = true;
        w->askToDiscard ("Quit?", [] {});
        test::check (gAsked.size() == 1, "with unsaved changes it asks before going on");
        gAnswer = true;
        bool went = false;
        w->askToDiscard ("Quit?", [&] { went = true; });
        test::check (went, "and goes on when told to");
        w->session.dirty = false;
        went = false;
        w->askToDiscard ("Quit?", [&] { went = true; });
        test::check (went && gAsked.size() == 2, "with nothing unsaved it does not ask");

        auto other = makeWindow (false);
        other->openProject (file);
        test::check (other->session.tracks.size() == 1 && other->trackList.rows.size() == 1, "the project opens in another window");
        test::check (noteMentions (*other, "not installed here", ui::Severity::warning), "a plugin that is not installed is said");
        test::check (! gErrors.empty() && gErrors.back().contains ("not installed"), "and told in a message too");
        test::check (other->trackList.rows[0]->slotButtons[0]->getToggleState(), "its button is lit, as a missing plugin is shown");

        w->newProject();
        test::check (w->session.tracks.empty() && w->trackList.rows.isEmpty() && w->windowTitle() == "Olympus - Untitled", "NEW empties the session");
        w->openProject (file);
        const auto stems = tempFolder().getChildFile ("out");
        test::check (w->renderTo (stems).isEmpty() && stems.getChildFile ("01-keys.wav").existsAsFile() && stems.getChildFile ("mix.wav").existsAsFile(), "RENDER STEMS writes the stem and the mix");

        gErrors.clear();
        w->addTracks ({ tempFolder().getChildFile ("missing.wav").getFullPathName() });
        test::check (gErrors.size() == 1, "a file that is not audio is told, not silently skipped");
        w->filesDropped ({ makeWav ("dropped.wav").getFullPathName() }, 0, 0);
        test::check (w->session.tracks.size() == 2, "a dropped file becomes a track");
    }

    std::printf ("[5] Missing audio\n");
    {
        auto w = makeWindow();
        const auto a = makeWav ("gone.wav");
        w->addTracks ({ a.getFullPathName() });
        const auto file = tempFolder().getChildFile ("gone.olympus");
        w->saveTo (file);
        a.deleteFile();
        auto other = makeWindow();
        other->openProject (file);
        test::check (noteMentions (*other, "audio file is missing", ui::Severity::warning), "a missing audio file is said, and the track stays");
    }

    std::printf ("[6] Arrange view\n");
    {
        olympus::Session session;
        test::addTestPlugins (session.catalog);
        olympus::Editor editor (session);
        juce::String error;
        const auto a = makeWav ("arrange-a.wav"), b = makeWav ("arrange-b.wav");
        test::check (session.addTrack (a, "Vocal", error) && session.addTrack (b, "Drums", error), "two tracks from two files");
        test::check (session.addClip (0, a, 30000, error) == 1, "a second clip on the first track, at 30000");
        session.addPlugin (0, "test gain", -1, error);

        ArrangeView view (session, editor);
        view.setSize (ArrangeView::kDesignWidth, ArrangeView::kDesignHeight);
        view.zoomToFit();
        view.refresh();
        const double rate = session.sampleRate;
        auto same = [] (ClipRef r, int t, int c) { return r.track == t && r.clip == c; };
        auto mouse = [&view] (juce::Point<int> p, juce::ModifierKeys mods = {})
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &view, &view, now, p.toFloat(), now, 1, false);
        };
        auto clipCount = [&session] (int t) { return (int) session.tracks[(size_t) t]->clips.size(); };

        // geometry: after zoomToFit everything is across the lanes, and clipAt and clipBounds agree with the model
        {
            bool fits = true, agrees = true, scaled = true;
            const double pps = view.zoom / rate;
            for (int t = 0; t < 2; ++t)
                for (int c = 0; c < clipCount (t); ++c)
                {
                    const auto r = view.clipBounds ({ t, c });
                    const auto& clip = session.tracks[(size_t) t]->clips[(size_t) c];
                    fits = fits && r.getRight() <= view.getWidth() && r.getX() >= ArrangeView::kHeaderWidth;
                    agrees = agrees && same (view.clipAt (r.getCentre()), t, c);
                    scaled = scaled && std::abs (r.getWidth() - (double) clip.length * pps) <= 1.5;
                }
            test::check (fits, "zoomToFit: every clip is across the lanes, inside the window");
            test::check (agrees, "clipAt of a clip's centre is that clip (clipBounds)");
            test::check (scaled, "a clip's width is its length at the zoom");
            test::check (! view.clipAt ({ 10, 10 }).valid() && view.clipBounds ({ 0, 9 }).isEmpty() && ! view.clipAt ({ 400, 2 }).valid(), "nothing in the header column, the ruler or past the last clip");
        }

        // a drag moves the clip, snapped to the grid (a quarter note, 24000 samples at 120 bpm); undo puts it back
        {
            const auto p = view.clipBounds ({ 0, 0 }).getCentre();
            view.mouseDown (mouse (p));
            view.mouseDrag (mouse (p + juce::Point<int> (400, 0)));
            view.mouseUp (mouse (p + juce::Point<int> (400, 0)));
            const auto moved = session.tracks[0]->clips[0].start;
            test::check (moved == 24000, "a drag moves the clip, snapped to the grid: " + juce::String (moved));
            test::check (editor.undo() && session.tracks[0]->clips[0].start == 0, "undo restores the clip's place");
            view.refresh();
        }

        // a drag of the right edge trims the clip (snap off so the new length is not moved to the grid)
        {
            editor.setSnap (false);
            const auto r = view.clipBounds ({ 1, 0 });
            const auto p = juce::Point<int> (r.getRight() - 2, r.getCentreY());
            view.mouseDown (mouse (p));
            view.mouseDrag (mouse (p + juce::Point<int> (-200, 0)));
            view.mouseUp (mouse (p + juce::Point<int> (-200, 0)));
            const auto length = session.tracks[1]->clips[0].length;
            test::check (length > 0 && length < 24000, "an edge drag trims the clip: " + juce::String (length));
            test::check (editor.undo() && session.tracks[1]->clips[0].length == 24000, "undo restores the length");
            editor.setSnap (true);
            view.refresh();
        }

        // split at the playhead gives two clips
        {
            view.playhead = 12000;
            view.selected = { 0, 0 };
            view.splitAtPlayhead();
            const auto& t0 = session.tracks[0]->clips;
            test::check (t0.size() == 3 && t0[0].length == 12000 && t0[1].start == 12000 && t0[1].length == 12000, "split at the playhead gives two clips");
            test::check (editor.undo() && clipCount (0) == 2, "undo joins them again");
        }

        // delete removes the selected clip, and undo brings it back
        {
            view.selected = { 1, 0 };
            test::check (view.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)) && clipCount (1) == 0, "the Delete key removes the clip");
            test::check (editor.undo() && clipCount (1) == 1, "undo brings it back");
        }

        // Cmd+D duplicates the selected clip, after its end
        {
            view.selected = { 0, 0 };
            test::check (view.keyPressed (juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 'd')) && clipCount (0) == 3, "Cmd+D duplicates the clip");
            test::check (session.tracks[0]->clips.back().start == 24000, "the copy starts where the original ends");
            editor.undo();
            view.refresh();
        }

        // snapping: the nearest grid line, halfway going up
        test::check (editor.snap (12345) == 24000 && editor.snap (11000) == 0, "snap: the nearest quarter note");

        // painting, at several zooms, to an image
        {
            bool ok = true;
            for (double z : { 0.05, 80.0, 4000.0 })
            {
                view.zoom = z;
                view.refresh();
                auto img = view.createComponentSnapshot (view.getLocalBounds());
                ok = ok && ! img.isNull() && img.getWidth() == ArrangeView::kDesignWidth;
            }
            test::check (ok, "painting to an image does not crash, at the smallest, a normal and the largest zoom");
        }
    }

    tempFolder().deleteRecursively();
    return test::finish ("OlympusUiCheck");
}
