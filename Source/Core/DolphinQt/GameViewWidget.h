// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDockWidget>
#include <QString>

class QLabel;
class QStackedWidget;
class RenderWidget;

// A dockable "Game View" panel. In the editor layout the emulator's render output lives here, so
// it can be docked anywhere in the main window, tabbed alongside the debugging tools, or floated
// out as its own window.
class GameViewWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit GameViewWidget(QWidget* parent = nullptr);

  // Places the render widget inside the panel and brings the panel to the front.
  void AttachRenderWidget(RenderWidget* render_widget);
  // Takes the render widget back out of the panel (reparenting it to nullptr) and shows the
  // idle placeholder again. Does nothing if the widget isn't currently attached.
  void DetachRenderWidget(RenderWidget* render_widget);

  bool HasRenderWidget() const { return m_render_widget != nullptr; }

private:
  QStackedWidget* m_stack;
  QLabel* m_placeholder;
  QString m_placeholder_text;
  RenderWidget* m_render_widget = nullptr;
};
