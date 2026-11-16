// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "UI/MainComponent.h"

#include <pantheon/ui/Aspect.h>

namespace
{
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow() : juce::DocumentWindow ("Orpheus", colours::background, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        content = new orpheus::gui::MainComponent();
        setContentOwned (content, true);
        content->onTitleChanged = [this] { setName (content->windowTitle()); };
        setResizable (true, true);
        setResizeLimits (1024, ui::editorHeightFor (1024), 4000, ui::editorHeightFor (4000)); // every edge and corner of the window; the shape stays 16:10 (the native frame is left out of it)
        getConstrainer()->setFixedAspectRatio (ui::kEditorAspect);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    orpheus::gui::MainComponent* content = nullptr;
};

class OrpheusApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Orpheus"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        window = std::make_unique<MainWindow>();
        if (commandLine.unquoted().trim().isNotEmpty())
            open (commandLine);
        else
            window->content->reopenLast();
    }

    void shutdown() override { window.reset(); }
    // Cmd-Q and the close button: asks first if there are edits that would be lost
    void systemRequestedQuit() override
    {
        if (window == nullptr)
            quit();
        else
            window->content->askToDiscard ("Quit and lose them?", [] { quit(); });
    }
    void anotherInstanceStarted (const juce::String& commandLine) override { open (commandLine); }

private:
    void open (const juce::String& commandLine)
    {
        const auto path = commandLine.unquoted().trim();
        if (path.isNotEmpty() && juce::File (path).existsAsFile() && window != nullptr)
            window->content->loadFile (juce::File (path));
    }

    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION (OrpheusApplication)
