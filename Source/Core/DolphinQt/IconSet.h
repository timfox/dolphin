// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <QColor>
#include <QIcon>

// Monochrome Font Awesome Free icons (bundled as SVGs under Resources/FontAwesome), tinted at
// runtime so they match whichever palette/theme is active.
namespace IconSet
{
// Tinted with the application palette's button text colour, with a dimmed disabled state and an
// accent-coloured active (hover) state.
QIcon Get(std::string_view name);
// Tinted with an explicit colour (used e.g. for status indicators).
QIcon Get(std::string_view name, const QColor& color);
}  // namespace IconSet
