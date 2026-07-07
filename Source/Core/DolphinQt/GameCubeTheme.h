// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

class QApplication;
class QPalette;

namespace GameCubeTheme
{
void ApplyPalette(QPalette& palette);
void ApplyFonts(QApplication* app);
QString DisplayFontFamily();
QString LoadStyleSheet();
}  // namespace GameCubeTheme
