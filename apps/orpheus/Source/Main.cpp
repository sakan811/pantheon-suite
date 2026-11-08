// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "UI/MainComponent.h"

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
        setResizable (true, true);
        setResizeLimits (1020, 620, 4000, 3000);
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
        open (commandLine);
    }

    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }
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
