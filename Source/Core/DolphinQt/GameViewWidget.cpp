// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/GameViewWidget.h"

#include <QLabel>
#include <QStackedWidget>

#include "DolphinQt/RenderWidget.h"

namespace
{
// The render widget has no useful size hint of its own. Give the panel a stable one so that
// QMainWindow doesn't squeeze the Game View when the render widget is swapped in at boot.
class GameViewStack final : public QStackedWidget
{
public:
  QSize sizeHint() const override { return QSize(960, 720); }
};
}  // namespace

GameViewWidget::GameViewWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Game View"));
  setObjectName(QStringLiteral("gameview"));
  setAllowedAreas(Qt::AllDockWidgetAreas);

  m_placeholder_text = tr("No game running\n\nPress Run (Ctrl+R) to build and boot your "
                          "project, or double-click a title in the Game Library.");
  m_placeholder = new QLabel(m_placeholder_text);
  m_placeholder->setObjectName(QStringLiteral("gameViewPlaceholder"));
  m_placeholder->setAlignment(Qt::AlignCenter);
  m_placeholder->setWordWrap(true);
  m_placeholder->setEnabled(false);

  m_stack = new GameViewStack;
  m_stack->addWidget(m_placeholder);
  // Let the render widget shrink freely; the presenter handles letterboxing.
  m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  m_stack->setMinimumSize(160, 120);
  setWidget(m_stack);

  // The game view can be moved around the main window and floated out into its own window, but it
  // can't be closed: closing it would hide the running game with no obvious way to get it back.
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
}

void GameViewWidget::AttachRenderWidget(RenderWidget* render_widget)
{
  if (m_render_widget == render_widget)
    return;

  if (m_render_widget)
    DetachRenderWidget(m_render_widget);

  // The render widget is a native window that only shows what the video backend draws, so blank
  // the placeholder first; otherwise its text can linger until the first frame is presented.
  m_placeholder->setText(QString());
  m_placeholder->repaint();

  m_render_widget = render_widget;
  m_stack->setCurrentIndex(m_stack->addWidget(render_widget));
  render_widget->show();

  show();
  raise();
}

void GameViewWidget::DetachRenderWidget(RenderWidget* render_widget)
{
  if (render_widget == nullptr || m_render_widget != render_widget)
    return;

  m_stack->removeWidget(render_widget);
  // Reparent to nullptr so the widget can become its own top-level window again (e.g. fullscreen).
  render_widget->setParent(nullptr);
  m_render_widget = nullptr;
  m_placeholder->setText(m_placeholder_text);
  m_stack->setCurrentWidget(m_placeholder);
}

