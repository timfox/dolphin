// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/IconSet.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace
{
constexpr int ICON_SIZES[] = {16, 20, 24, 32, 48};
// Rendered at 2x so icons stay crisp on high-DPI screens.
constexpr qreal RENDER_SCALE = 2.0;

QByteArray LoadSvg(std::string_view name)
{
  QFile file(QStringLiteral(":/fontawesome/%1.svg")
                 .arg(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))));
  if (!file.open(QFile::ReadOnly))
    return {};
  return file.readAll();
}

QPixmap Render(const QByteArray& svg, const QColor& color, int size)
{
  // Font Awesome paths have no fill of their own, so a fill on the root element tints the icon.
  QByteArray tinted = svg;
  QByteArray attributes("<svg fill=\"");
  attributes.append(color.name(QColor::HexRgb).toUtf8());
  attributes.append("\" fill-opacity=\"");
  attributes.append(QByteArray::number(color.alphaF()));
  attributes.append("\" ");
  tinted.replace(QByteArrayLiteral("<svg "), attributes);

  QSvgRenderer renderer(tinted);
  const int px = static_cast<int>(size * RENDER_SCALE);
  QPixmap pixmap(px, px);
  pixmap.fill(Qt::transparent);
  if (!renderer.isValid())
    return pixmap;

  // Keep the glyph's aspect ratio and centre it in the square icon.
  QSizeF glyph = renderer.defaultSize();
  glyph.scale(px, px, Qt::KeepAspectRatio);
  const QRectF target((px - glyph.width()) / 2, (px - glyph.height()) / 2, glyph.width(),
                      glyph.height());

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  renderer.render(&painter, target);
  painter.end();

  pixmap.setDevicePixelRatio(RENDER_SCALE);
  return pixmap;
}

QIcon Build(std::string_view name, const QColor& normal, const QColor& disabled,
            const QColor& active)
{
  const QByteArray svg = LoadSvg(name);
  if (svg.isEmpty())
    return {};

  QIcon icon;
  for (const int size : ICON_SIZES)
  {
    icon.addPixmap(Render(svg, normal, size), QIcon::Normal);
    icon.addPixmap(Render(svg, disabled, size), QIcon::Disabled);
    icon.addPixmap(Render(svg, active, size), QIcon::Active);
    icon.addPixmap(Render(svg, active, size), QIcon::Selected);
  }
  return icon;
}
}  // namespace

namespace IconSet
{
QIcon Get(std::string_view name)
{
  const QPalette palette = QApplication::palette();
  const QColor normal = palette.color(QPalette::Active, QPalette::ButtonText);
  QColor disabled = normal;
  disabled.setAlphaF(0.35f);
  const QColor active = palette.color(QPalette::Active, QPalette::Highlight).lighter(150);
  return Build(name, normal, disabled, active);
}

QIcon Get(std::string_view name, const QColor& color)
{
  QColor disabled = color;
  disabled.setAlphaF(0.35f);
  return Build(name, color, disabled, color.lighter(130));
}
}  // namespace IconSet
