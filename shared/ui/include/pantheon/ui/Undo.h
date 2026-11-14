// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace ui
{
// UNDO / REDO for a plugin's parameters. The processor owns a `juce::UndoManager` and hands it to its AudioProcessorValueTreeState (the constructor's second
// argument); the kit's knobs, choices and switches are bound through that state, so every drag, click and typed value is recorded as one step, and the
// state records a change made any other way (a preset, a mouse gesture on the plugin's own display) when it next syncs, in the step that is open then.
// A plugin that makes several changes at once from a gesture of its own (loading a preset, adding a band) calls `ui::beginUndoStep` first, so they undo together.
// Opt-in: a plugin that does not hand the state an UndoManager has no undo and no buttons, and is drawn as before.
//
//   processor:  juce::UndoManager undo;   juce::AudioProcessorValueTreeState apvts { *this, &undo, "STATE", layout };   // undo before apvts
//   editor:     ui::UndoButtons undoButtons { processor.undo, *this };   addAndMakeVisible (undoButtons);   undoButtons.setBounds (x, y, ui::UndoButtons::kWidth, h);
//
// Cmd+Z undoes and Shift+Cmd+Z (Ctrl on other systems; also Ctrl+Y) redoes while the editor, or a control in it, has the keyboard focus; a text field being
// typed in keeps its own undo. A host that takes the keys for itself never delivers them: the buttons are always there.
// What it does not cover: state that is not a parameter (a window size, a stored analyser setting), and it is cleared when a session is loaded.
inline void beginUndoStep (juce::AudioProcessorValueTreeState& state, const juce::String& name = {})
{
    if (state.undoManager != nullptr)
        state.undoManager->beginNewTransaction (name);
}

class UndoButtons : public juce::Component, private juce::ChangeListener
{
public:
    static constexpr int kWidth = 84;

    UndoButtons (juce::UndoManager& manager, juce::Component& keyTarget) : um (manager), target (keyTarget), keys (*this)
    {
        undoButton.setButtonText ("UNDO");
        undoButton.setTooltip ("Undo the last change to a control (Cmd+Z, Ctrl+Z on Windows).");
        undoButton.onClick = [this] { undo(); };
        redoButton.setButtonText ("REDO");
        redoButton.setTooltip ("Redo what was undone (Shift+Cmd+Z, Ctrl+Y on Windows).");
        redoButton.onClick = [this] { redo(); };
        addAndMakeVisible (undoButton);
        addAndMakeVisible (redoButton);

        target.setWantsKeyboardFocus (true);
        target.addKeyListener (&keys);
        um.addChangeListener (this);
        refresh();
    }

    ~UndoButtons() override
    {
        um.removeChangeListener (this);
        target.removeKeyListener (&keys);
    }

    bool undo()
    {
        return um.undo();
    }
    bool redo() { return um.redo(); }

    // Cmd/Ctrl+Z, Shift+Cmd/Ctrl+Z and Ctrl+Y. True when the key was used.
    bool handleKey (const juce::KeyPress& key)
    {
        const auto mods = key.getModifiers();
        if (! mods.isCommandDown() || mods.isAltDown())
            return false;
        const int code = key.getKeyCode();
        if (code == 'Z' || code == 'z')
            return mods.isShiftDown() ? redo() || true : undo() || true;
        if (code == 'Y' || code == 'y')
            return redo() || true;
        return false;
    }

    juce::TextButton& undoSwitch() noexcept { return undoButton; } // for the plugins' checks
    juce::TextButton& redoSwitch() noexcept { return redoButton; }

    void resized() override
    {
        auto r = getLocalBounds();
        const int w = (r.getWidth() - 4) / 2;
        undoButton.setBounds (r.removeFromLeft (w));
        r.removeFromLeft (4);
        redoButton.setBounds (r);
    }

private:
    struct Keys : juce::KeyListener
    {
        explicit Keys (UndoButtons& o) : owner (o) {}
        bool keyPressed (const juce::KeyPress& k, juce::Component*) override { return owner.handleKey (k); }
        UndoButtons& owner;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void refresh()
    {
        undoButton.setEnabled (um.canUndo());
        redoButton.setEnabled (um.canRedo());
    }

    juce::UndoManager& um;
    juce::Component& target;
    Keys keys;
    juce::TextButton undoButton, redoButton;
};
} // namespace ui
