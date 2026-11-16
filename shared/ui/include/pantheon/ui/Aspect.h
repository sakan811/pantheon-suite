// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

namespace ui
{
// One window shape for every plugin and app: 16:10. It is the shape of the laptop and desktop screens plugin windows are opened on, it is close to where the plugins
// already were (1.54 to 1.67 for the ones with a main display), and a fixed ratio is what the hosts' own corner and edge drags keep as well. A plugin's design
// width is a multiple of 8, so its height is a whole number: `kWidth = 800; kHeight = ui::editorHeightFor (kWidth);`, and the main display takes what the
// rest of the layout leaves (a plugin never picks its own ratio).
inline constexpr int kEditorAspectWidth = 16, kEditorAspectHeight = 10;
inline constexpr double kEditorAspect = (double) kEditorAspectWidth / (double) kEditorAspectHeight;

constexpr int editorHeightFor (int width) { return width * kEditorAspectHeight / kEditorAspectWidth; }
constexpr bool hasEditorAspect (int width, int height) { return width * kEditorAspectHeight == height * kEditorAspectWidth; }
} // namespace ui
