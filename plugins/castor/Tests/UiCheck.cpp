// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the knobs, the audio path,
// the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace castor;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Castor UI check\n");
    CastorProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == kBands + 6, fmt ("%d parameters: a width for each of %d bands, mid, side, mono below, balance, output and listen", proc.getParameters().size(), kBands));
    const auto d = readParams (proc.apvts);
    bool defaultsOk = std::fabs (d.midDb) < 1e-4f && std::fabs (d.sideDb) < 1e-4f && d.monoBelowHz <= kMonoOffHz + 0.01f && std::fabs (d.balance) < 1e-4f && std::fabs (d.outDb) < 1e-4f;
    for (float w : d.width)
        defaultsOk = defaultsOk && std::fabs (w - 1.0f) < 1e-4f;
    check (defaultsOk && isNeutral (d) && proc.getLatencySamples() == 0, "a new instance is neutral: every width 100%, nothing else, no latency");
    for (int b = 0; b < kBands; ++b)
        if (! juce::String (proc.apvts.getParameter (id::width (b))->getName (64)).startsWith (pdsp::bands::kBands[(size_t) b].title))
            check (false, "a width parameter is named after its band");

    // The audio path through the real processor: a side-only tone with every width at 0% is silent on the difference.
    {
        CastorProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        for (int b = 0; b < kBands; ++b)
            setValue (q.apvts, id::width (b), 0.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        float diff = 0.0f;
        for (int block = 0; block < 100; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.3f * std::sin (2.0f * 3.14159265f * 700.0f * (float) (block * 512 + i) / 48000.0f);
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, -v);
            }
            q.processBlock (buffer, midi);
            if (block > 50)
                for (int i = 0; i < 512; ++i)
                    diff = std::max (diff, std::fabs (buffer.getSample (0, i) - buffer.getSample (1, i)));
        }
        check (diff < 1e-4f, fmt ("every width 0%%: a side-only tone comes out mono through the processor (the left and right differ by %.1e)", diff));
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<CastorEditor> (*holder);
        editor.setVisible (true);
        check (editor.getWidth() > 800 && editor.widths[5]->getRight() < editor.getWidth() && editor.outKnob.getBottom() < editor.getHeight(), "the six width knobs and the five others fit the window");
        editor.widths[2]->setVisible (true);
        check (ui::hasNote (editor.notes(), "Nothing is changed yet", ui::Severity::info), "a neutral instance says that nothing is changed yet");
        setValue (proc.apvts, id::width (1), 200.0f);
        check (ui::hasNote (editor.notes(), "A wide Bass", ui::Severity::warning), "a wide bass is warned about");
        setValue (proc.apvts, id::monoBelow, 120.0f);
        check (! ui::hasNote (editor.notes(), "A wide Bass", ui::Severity::warning), "... unless MONO BELOW is on");
        setValue (proc.apvts, id::width (1), 100.0f);
        setValue (proc.apvts, id::monoBelow, 20.0f);
    }

    // A mono bus: BALANCE is ignored (it would only turn the one channel down), and the check is not applied.
    {
        CastorProcessor q;
        q.setPlayConfigDetails (1, 1, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::balance, 100.0f);
        setValue (q.apvts, id::listen, 2.0f);
        juce::AudioBuffer<float> buffer (1, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 20; ++block)
        {
            for (int i = 0; i < 512; ++i)
                buffer.setSample (0, i, 0.4f * std::sin (2.0f * 3.14159265f * 300.0f * (float) (block * 512 + i) / 48000.0f));
            q.processBlock (buffer, midi);
            if (block >= 10)
                peak = std::max (peak, buffer.getMagnitude (0, 512));
        }
        check (std::fabs (peak - 0.4f) < 0.005f, fmt ("a mono bus with BALANCE at +100 and LISTEN on Side comes out unchanged (peak %.3f of 0.400)", peak));
    }

    // LISTEN through the real processor, and the correlation minimum in the editor.
    {
        CastorProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<CastorEditor> (*holder);
        editor.setVisible (true);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        const auto play = [&] (bool outOfPhase, int blocks)
        {
            float diff = 0.0f, sum = 0.0f;
            for (int block = 0; block < blocks; ++block)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.3f * std::sin (2.0f * 3.14159265f * 400.0f * (float) (block * 512 + i) / 48000.0f);
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, outOfPhase ? -v : v);
                }
                q.processBlock (buffer, midi);
                diff = std::max (diff, std::fabs (buffer.getSample (0, 100) - buffer.getSample (1, 100)));
                sum = std::max (sum, buffer.getMagnitude (0, 512));
                if (block % 8 == 0)
                    editor.tickDisplay();
            }
            return std::make_pair (diff, sum);
        };
        setValue (q.apvts, id::listen, 1.0f);
        auto r = play (true, 40);
        check (r.second < 1e-6f && r.first == 0.0f, fmt ("LISTEN Mono: an out-of-phase tone is silent (%.1e) in both channels", r.second));
        check (ui::hasNote (editor.notes(), "LISTEN is not Stereo", ui::Severity::warning), "LISTEN away from Stereo is flagged");
        setValue (q.apvts, id::listen, 2.0f);
        r = play (true, 10);
        check (r.second > 0.25f && r.first == 0.0f, fmt ("LISTEN Side: the out-of-phase tone is heard (%.2f), the same in both channels", r.second));
        setValue (q.apvts, id::listen, 0.0f);
        play (false, 60);
        editor.tickDisplay();
        check (editor.correlationMinHold() < -0.5f, fmt ("the correlation minimum shows the out-of-phase passage after it is over: %.2f", editor.correlationMinHold()));
        check (editor.minButton.getButtonText().startsWith ("MIN -"), "the MIN button reads it");
        editor.minButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        play (false, 10);
        editor.tickDisplay();
        check (editor.correlationMinHold() > 0.5f, fmt ("clicking MIN starts it again (%.2f)", editor.correlationMinHold()));
        check (editor.listen.getRight() < editor.getWidth() && editor.minButton.getRight() <= editor.getWidth() && editor.minButton.getBottom() < editor.getHeight() && ! editor.listen.getBounds().intersects (editor.minButton.getBounds()), "LISTEN and MIN fit under the correlation bar");
    }

    // The state survives a round trip.
    {
        setValue (proc.apvts, id::width (3), 150.0f);
        setValue (proc.apvts, id::monoBelow, 90.0f);
        setValue (proc.apvts, id::balance, -25.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        CastorProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        const auto o = readParams (other.apvts);
        check (std::fabs (o.width[3] - 1.5f) < 0.01f && std::fabs (o.monoBelowHz - 90.0f) < 1.0f && std::fabs (o.balance + 0.25f) < 0.01f, "the state of a session comes back as it was saved");
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).size() == 1, "a default instance says only that nothing is changed");
        in.params.width[3] = 1.4f;
        in.correlation = -0.4f;
        check (ui::hasNote (adviceFor (in), "out of phase", ui::Severity::warning) && ui::hasNote (adviceFor (in), "turn the phase around the band edges", ui::Severity::info), "a negative correlation is warned about, and the widths' phase turn is explained");
        in.params.listen = Listen::mono;
        check (ui::hasNote (adviceFor (in), "LISTEN is not Stereo", ui::Severity::warning), "a check is flagged");
        in.params.listen = Listen::stereo;
        in.params.sideDb = 8.0f;
        in.params.balance = 0.2f;
        check (ui::hasNote (adviceFor (in), "SIDE above +6 dB", ui::Severity::warning) && ui::hasNote (adviceFor (in), "BALANCE turns", ui::Severity::info), "a high SIDE and a BALANCE are flagged");
        in.params.width.fill (0.0f);
        check (ui::hasNote (adviceFor (in), "Every width is 0%", ui::Severity::info), "all widths at 0%: the output is mono");
        bool tooLong = false;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        in.params.width = { 1, 2, 1, 1, 1, 1 };
        in.params.monoBelowHz = 20.0f;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }

    // Presets: each applies, has a name and a one-line purpose, the chosen one is kept with the session, and the box is in the header.
    {
        CastorProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        bool all = q.getNumPrograms() == (int) presets().size() && q.getNumPrograms() >= 6;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            q.setCurrentProgram (i);
            const auto len = std::strlen (presets()[(size_t) i].purpose);
            all = all && q.getProgramName (i).isNotEmpty() && q.getCurrentProgram() == i && len > 20 && len <= 140;
        }
        check (all, fmt ("all %d presets apply, have a name and a one-line purpose", q.getNumPrograms()));
        const auto indexOf = [&] (const char* name)
        {
            for (int i = 0; i < q.getNumPrograms(); ++i)
                if (q.getProgramName (i) == name)
                    return i;
            check (false, std::string ("a preset called ") + name);
            return 0;
        };
        const auto raw = [&] (const juce::String& pid) { return q.apvts.getRawParameterValue (pid)->load(); };
        setValue (q.apvts, id::balance, 40.0f);
        setValue (q.apvts, id::out, 2.0f);
        q.setCurrentProgram (indexOf ("Club Safe"));
        check (std::fabs (raw (id::width (0))) < 0.5f && std::fabs (raw (id::width (1)) - 40.0f) < 0.5f && std::fabs (raw (id::width (4)) - 100.0f) < 0.5f && std::fabs (raw (id::monoBelow) - 150.0f) < 0.5f, "Club Safe: the sub in mono, the bass at 40%, the rest at 100%, mono below 150 Hz");
        check (std::fabs (raw (id::balance) - 40.0f) < 0.5f && std::fabs (raw (id::out) - 2.0f) < 0.01f, "BALANCE and OUTPUT are kept by a preset");
        q.setCurrentProgram (0);
        check (std::fabs (raw (id::width (0)) - 100.0f) < 0.5f && std::fabs (raw (id::monoBelow) - kMonoOffHz) < 0.5f, "Init: the widths are back at 100% and the mono bass is off");
        const int chosen = indexOf ("Widen Highs");
        q.setCurrentProgram (chosen);
        check (raw (id::width (5)) > 150.0f, "Widen Highs opens the top band");
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        CastorProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (other.getCurrentProgram() == chosen, "the chosen preset is saved with the session");
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<CastorEditor> (*holder);
        editor.setVisible (true);
        check (editor.presetBox.getNumItems() == q.getNumPrograms() && editor.presetBox.getSelectedId() == chosen + 1, "the editor's preset box lists the presets and shows the current one");
        check (editor.presetBox.getRight() < editor.getWidth() && ! editor.presetBox.getBounds().intersects (editor.guideButton.getBounds()), "the preset box fits the header beside the GUIDE button");
    }

    return test::finish ("CastorUiCheck");
}
