// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../Catalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <pantheon/ui/Hardware.h>

#include <cmath>
#include <functional>
#include <vector>

namespace olympus::gui
{
// Searchable plugin browser displayed inside a CallOutBox (or standalone in tests).
// Filters installed plugins case-insensitively by name and maker as the user types.
class PluginPicker : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::KeyListener
{
public:
    struct Entry
    {
        juce::String name;
        juce::String maker;
    };

    PluginPicker (std::vector<Entry> items, std::function<void (const juce::String& pluginName)> onPick)
        : entries (std::move (items)), onPickCallback (std::move (onPick))
    {
        searchBox.setFont (ui::font (13.5f));
        searchBox.setTextToShowWhenEmpty ("Search plugins...", colours::dim);
        searchBox.setColour (juce::TextEditor::backgroundColourId, colours::glass);
        searchBox.setColour (juce::TextEditor::textColourId, colours::text);
        searchBox.setColour (juce::TextEditor::outlineColourId, colours::panelEdge);
        searchBox.setColour (juce::TextEditor::focusedOutlineColourId, colours::amber);
        searchBox.setColour (juce::TextEditor::highlightColourId, colours::amber.withAlpha (0.35f));
        searchBox.setColour (juce::TextEditor::highlightedTextColourId, colours::text);
        searchBox.onTextChange = [this] { applyFilter(); };
        searchBox.onReturnKey = [this]
        {
            const int row = list.getSelectedRow();
            pickRow (row >= 0 ? row : 0);
        };
        searchBox.onEscapeKey = [this] { closeCallOut(); };
        searchBox.addKeyListener (this);
        addAndMakeVisible (searchBox);

        list.setModel (this);
        list.setRowHeight (26);
        list.setColour (juce::ListBox::backgroundColourId, colours::glass);
        list.setColour (juce::ListBox::outlineColourId, colours::panelEdge);
        list.setOutlineThickness (1);
        list.setMultipleSelectionEnabled (false);
        addAndMakeVisible (list);

        applyFilter();
        setSize (340, 380);
    }

    PluginPicker (const Catalog& catalog, std::function<void (const juce::String& pluginName)> onPick)
        : PluginPicker (entriesFromCatalog (catalog), std::move (onPick))
    {
    }

    ~PluginPicker() override
    {
        searchBox.removeKeyListener (this);
        list.setModel (nullptr);
    }

    static std::vector<Entry> entriesFromCatalog (const Catalog& catalog)
    {
        std::vector<Entry> out;
        const auto names = catalog.names();
        const auto& descs = catalog.descriptions();
        out.reserve ((size_t) names.size());
        for (const auto& label : names)
        {
            juce::String maker;
            for (const auto& d : descs)
            {
                const auto suffix = d.manufacturerName.isNotEmpty() ? d.manufacturerName : d.pluginFormatName;
                if (label == d.name || label == d.name + " (" + suffix + ")")
                {
                    maker = d.manufacturerName;
                    break;
                }
            }
            out.push_back ({ label, maker });
        }
        return out;
    }

    void setFilter (const juce::String& query)
    {
        if (searchBox.getText() != query)
            searchBox.setText (query, juce::dontSendNotification);
        applyFilter();
    }

    int filteredCount() const { return (int) filtered.size(); }

    juce::String filteredNameAt (int row) const
    {
        if (! juce::isPositiveAndBelow (row, (int) filtered.size()))
            return {};
        return entries[(size_t) filtered[(size_t) row]].name;
    }

    void pickRow (int row)
    {
        if (! juce::isPositiveAndBelow (row, (int) filtered.size()))
            return;
        const auto name = entries[(size_t) filtered[(size_t) row]].name;
        auto cb = onPickCallback;
        closeCallOut();
        if (cb)
            cb (name);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::panel);
        g.setColour (colours::panelEdge);
        g.drawRect (getLocalBounds(), 1);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (filtered.empty())
        {
            g.setColour (colours::dim);
            g.setFont (ui::font (13.0f));
            g.drawText ("No matching plugins", list.getBounds(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        searchBox.setBounds (b.removeFromTop (28));
        b.removeFromTop (6);
        list.setBounds (b);
    }

    void parentHierarchyChanged() override { focusSearchBox(); }
    void visibilityChanged() override { focusSearchBox(); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            closeCallOut();
            return true;
        }
        if (key == juce::KeyPress::returnKey)
        {
            const int row = list.getSelectedRow();
            pickRow (row >= 0 ? row : 0);
            return true;
        }
        return false;
    }

    juce::TextEditor searchBox;
    juce::ListBox list;

private:
    int getNumRows() override { return (int) filtered.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (! juce::isPositiveAndBelow (rowNumber, (int) filtered.size()))
            return;
        const auto& entry = entries[(size_t) filtered[(size_t) rowNumber]];
        if (rowIsSelected)
            g.fillAll (colours::amber.darker (0.55f));
        else if ((rowNumber & 1) != 0)
            g.fillAll (colours::panel.withAlpha (0.25f));

        auto bounds = juce::Rectangle<int> (0, 0, width, height).reduced (8, 0);
        if (entry.maker.isNotEmpty())
        {
            const auto makerFont = ui::lcdFont (11.0f);
            const int makerW = juce::jmin (width / 2, (int) std::ceil (juce::GlyphArrangement::getStringWidth (makerFont, entry.maker)) + 10);
            auto makerArea = bounds.removeFromRight (makerW);
            bounds.removeFromRight (6);
            g.setFont (makerFont);
            g.setColour (rowIsSelected ? colours::text.withAlpha (0.75f) : colours::dim);
            g.drawText (entry.maker, makerArea, juce::Justification::centredRight, true);
        }
        g.setFont (ui::font (13.0f));
        g.setColour (rowIsSelected ? colours::text : colours::amber);
        g.drawText (entry.name, bounds, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override { pickRow (row); }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { pickRow (row); }
    void returnKeyPressed (int lastRowSelected) override { pickRow (lastRowSelected); }

    bool keyPressed (const juce::KeyPress& key, juce::Component* originatingComponent) override
    {
        if (originatingComponent != &searchBox || filtered.empty())
            return false;
        if (key == juce::KeyPress::downKey)
        {
            const int next = juce::jlimit (0, (int) filtered.size() - 1, list.getSelectedRow() + 1);
            list.selectRow (next);
            return true;
        }
        if (key == juce::KeyPress::upKey)
        {
            const int prev = juce::jlimit (0, (int) filtered.size() - 1, list.getSelectedRow() - 1);
            list.selectRow (prev);
            return true;
        }
        return false;
    }

    void applyFilter()
    {
        const auto q = searchBox.getText().trim();
        filtered.clear();
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            const auto& e = entries[(size_t) i];
            if (q.isEmpty() || e.name.containsIgnoreCase (q) || e.maker.containsIgnoreCase (q))
                filtered.push_back (i);
        }
        list.updateContent();
        if (! filtered.empty())
            list.selectRow (0);
        else
            list.deselectAllRows();
        repaint();
    }

    void focusSearchBox()
    {
        if (! isShowing() || ! searchBox.isShowing())
            return;
        searchBox.grabKeyboardFocus();
        auto safe = juce::Component::SafePointer<PluginPicker> (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr && safe->isShowing() && safe->searchBox.isShowing())
                safe->searchBox.grabKeyboardFocus();
        });
    }

    void closeCallOut()
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->exitModalState (0);
    }

    std::vector<Entry> entries;
    std::vector<int> filtered;
    std::function<void (const juce::String&)> onPickCallback;
};
} // namespace olympus::gui

namespace olympus
{
using gui::PluginPicker;
} // namespace olympus
