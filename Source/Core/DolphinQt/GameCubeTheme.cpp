// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/GameCubeTheme.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>

namespace
{
QString s_display_font_family = QStringLiteral("Sans Serif");

QString FindRepoRoot()
{
  const QByteArray env_root = qgetenv("XASH3D_GC_ROOT");
  if (!env_root.isEmpty())
  {
    const QDir env_dir(QString::fromLocal8Bit(env_root));
    if (env_dir.exists(QStringLiteral("fonts/GameCube.ttf")))
      return env_dir.absolutePath();
  }

  QDir dir(QCoreApplication::applicationDirPath());
  for (int depth = 0; depth < 8; ++depth)
  {
    if (dir.exists(QStringLiteral("fonts/GameCube.ttf")))
      return dir.absolutePath();
    if (!dir.cdUp())
      break;
  }

  return {};
}

QString LoadFontFamily(const QString& repo_root, const QString& relative_path,
                       const QString& fallback)
{
  if (repo_root.isEmpty())
    return fallback;

  const QString font_path = QDir(repo_root).filePath(relative_path);
  const int font_id = QFontDatabase::addApplicationFont(font_path);
  if (font_id < 0)
    return fallback;

  const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
  return families.isEmpty() ? fallback : families.front();
}
}  // namespace

namespace GameCubeTheme
{
void ApplyPalette(QPalette& palette)
{
  palette.setColor(QPalette::All, QPalette::Window, QColor(QStringLiteral("#090814")));
  palette.setColor(QPalette::Disabled, QPalette::Window, QColor(QStringLiteral("#090814")));
  palette.setColor(QPalette::All, QPalette::WindowText, QColor(QStringLiteral("#f8f6ff")));
  palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#746d98")));
  palette.setColor(QPalette::All, QPalette::Base, QColor(QStringLiteral("#0d0b18")));
  palette.setColor(QPalette::Disabled, QPalette::Base, QColor(QStringLiteral("#131020")));
  palette.setColor(QPalette::All, QPalette::AlternateBase, QColor(QStringLiteral("#171229")));
  palette.setColor(QPalette::Disabled, QPalette::AlternateBase, QColor(QStringLiteral("#171229")));
  palette.setColor(QPalette::All, QPalette::ToolTipBase, QColor(QStringLiteral("#151225")));
  palette.setColor(QPalette::Disabled, QPalette::ToolTipBase, QColor(QStringLiteral("#151225")));
  palette.setColor(QPalette::All, QPalette::ToolTipText, QColor(QStringLiteral("#f8f6ff")));
  palette.setColor(QPalette::Disabled, QPalette::ToolTipText, QColor(QStringLiteral("#f8f6ff")));
  palette.setColor(QPalette::All, QPalette::PlaceholderText, QColor(QStringLiteral("#746d98")));
  palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor(QStringLiteral("#746d98")));
  palette.setColor(QPalette::All, QPalette::Text, QColor(QStringLiteral("#f8f6ff")));
  palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#746d98")));
  palette.setColor(QPalette::All, QPalette::Button, QColor(QStringLiteral("#382d68")));
  palette.setColor(QPalette::Disabled, QPalette::Button, QColor(QStringLiteral("#181429")));
  palette.setColor(QPalette::All, QPalette::ButtonText, QColor(QStringLiteral("#f8f6ff")));
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#746d98")));
  palette.setColor(QPalette::All, QPalette::BrightText, QColor(QStringLiteral("#5fe3ff")));
  palette.setColor(QPalette::Disabled, QPalette::BrightText, QColor(QStringLiteral("#f8f6ff")));
  palette.setColor(QPalette::All, QPalette::Light, QColor(QStringLiteral("#6656bf")));
  palette.setColor(QPalette::Disabled, QPalette::Light, QColor(QStringLiteral("#6656bf")));
  palette.setColor(QPalette::All, QPalette::Midlight, QColor(QStringLiteral("#271f4b")));
  palette.setColor(QPalette::Disabled, QPalette::Midlight, QColor(QStringLiteral("#271f4b")));
  palette.setColor(QPalette::All, QPalette::Dark, QColor(QStringLiteral("#171229")));
  palette.setColor(QPalette::Disabled, QPalette::Dark, QColor(QStringLiteral("#171229")));
  palette.setColor(QPalette::All, QPalette::Mid, QColor(QStringLiteral("#382d68")));
  palette.setColor(QPalette::Disabled, QPalette::Mid, QColor(QStringLiteral("#382d68")));
  palette.setColor(QPalette::All, QPalette::Shadow, QColor(QStringLiteral("#05050c")));
  palette.setColor(QPalette::Disabled, QPalette::Shadow, QColor(QStringLiteral("#05050c")));
  palette.setColor(QPalette::All, QPalette::Highlight, QColor(QStringLiteral("#4f46a8")));
  palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#382d68")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  palette.setColor(QPalette::All, QPalette::Accent, QColor(QStringLiteral("#5fe3ff")));
  palette.setColor(QPalette::Disabled, QPalette::Accent, QColor(QStringLiteral("#382d68")));
#endif
  palette.setColor(QPalette::All, QPalette::HighlightedText, QColor(QStringLiteral("#f8f6ff")));
  palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(QStringLiteral("#746d98")));
  palette.setColor(QPalette::All, QPalette::Link, QColor(QStringLiteral("#5fe3ff")));
  palette.setColor(QPalette::Disabled, QPalette::Link, QColor(QStringLiteral("#382d68")));
  palette.setColor(QPalette::All, QPalette::LinkVisited, QColor(QStringLiteral("#7dffc7")));
  palette.setColor(QPalette::Disabled, QPalette::LinkVisited, QColor(QStringLiteral("#382d68")));
}

void ApplyFonts(QApplication* app)
{
  const QString repo_root = FindRepoRoot();
  const QString rodin_family =
      LoadFontFamily(repo_root, QStringLiteral("fonts/FOT-Rodin Pro DB.otf"), s_display_font_family);
  s_display_font_family =
      LoadFontFamily(repo_root, QStringLiteral("fonts/GameCube.ttf"), rodin_family);

  QFont font(s_display_font_family, 9);
  font.setStyleHint(QFont::SansSerif);
  app->setFont(font);
}

QString DisplayFontFamily()
{
  return s_display_font_family;
}

QString LoadStyleSheet()
{
  QFile file(QStringLiteral(":/gamecube_theme/gamecube.qss"));
  if (!file.open(QFile::ReadOnly))
    return {};

  QString stylesheet = QString::fromUtf8(file.readAll());
  return stylesheet.replace(QStringLiteral("%GC_FONT%"), DisplayFontFamily());
}
}  // namespace GameCubeTheme
