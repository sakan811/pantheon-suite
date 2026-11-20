// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "UI/MainComponent.h"

#include <pantheon/ui/Aspect.h>

namespace
{
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow() : juce::DocumentWindow ("Olympus", colours::background, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        content = new olympus::gui::MainComponent();
        setContentOwned (content, true);
        content->onTitleChanged = [this] { setName (content->windowTitle()); };
        setResizable (true, true);
        // the transport row needs the design width: the window grows from it. The shape stays 16:10, like every window of the suite.
        setResizeLimits (olympus::gui::MainComponent::kDesignWidth, ui::editorHeightFor (olympus::gui::MainComponent::kDesignWidth), 4000, ui::editorHeightFor (4000));
        getConstrainer()->setFixedAspectRatio (ui::kEditorAspect);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    olympus::gui::MainComponent* content = nullptr;
};

class OlympusApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Olympus"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        window = std::make_unique<MainWindow>();
        window->content->setSize (olympus::gui::MainComponent::kDesignWidth, olympus::gui::MainComponent::kDesignHeight);
        const auto path = commandLine.unquoted().trim();
        if (path.isNotEmpty() && juce::File (path).existsAsFile())
            window->content->openProject (juce::File (path));
        else
            window->content->openLastProjectAtStart();   // skipped when the last start died opening it (a pedal, Settings.h)
        window->content->offerRecovery();                 // unsaved work of the last session (an autosave), if any
    }

    void shutdown() override { window.reset(); }
    // Cmd-Q and the close button: asks first if there are changes that would be lost
    void systemRequestedQuit() override
    {
        if (window == nullptr) { quit(); return; }
        // refused (and nothing quits) while a render runs or a dialog is open: askToDiscard checks it first
        window->content->askToDiscard ("Quit and lose them?", [content = window->content] { content->dropAutosave(); quit(); });
    }
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        const auto path = commandLine.unquoted().trim();
        if (path.isNotEmpty() && juce::File (path).existsAsFile() && window != nullptr)
            window->content->openProject (juce::File (path));
    }

private:
    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION (OlympusApplication)
