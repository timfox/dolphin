// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <QDockWidget>
#include <QFrame>

#include "Common/Config/Config.h"

class QLabel;
class QToolButton;
class QTableWidget;
class QVBoxLayout;

namespace Core
{
enum class State;
}

namespace UICommon
{
class GameFile;
}

// Compact "target" panel shown at the right end of the toolbar: emulation state, the running title,
// and the active CPU core and video backend.
class TargetStatusWidget final : public QFrame
{
  Q_OBJECT
public:
  explicit TargetStatusWidget(QWidget* parent = nullptr);

private:
  void Refresh();

  QLabel* m_icon;
  QLabel* m_state;
  QLabel* m_title;
  QLabel* m_cpu;
  QLabel* m_backend;
};

// "Target Tools" dock: a column of big launcher buttons for the inspection/debug tools.
class TargetToolsWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit TargetToolsWidget(QWidget* parent = nullptr);

  // A button that runs |action| when clicked.
  void AddTool(std::string_view icon, const QString& text, const QString& tooltip,
               std::function<void()> action);
  // A checkable button bound to a boolean config setting.
  void AddToggle(std::string_view icon, const QString& text, const QString& tooltip,
                 const Config::Info<bool>& setting);

private:
  QVBoxLayout* m_layout;
};

// "Property Inspector" dock: a Property/Value table describing the title selected in the game
// library.
class PropertyInspectorWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit PropertyInspectorWidget(QWidget* parent = nullptr);

  void SetGameFile(const std::shared_ptr<const UICommon::GameFile>& game);

private:
  void AddRow(const QString& property, const QString& value);

  QLabel* m_selected;
  QTableWidget* m_table;
};

// Custom dock title bar for the editor layout: panel icon + title, with float and close buttons.
class DockTitleBar final : public QWidget
{
  Q_OBJECT
public:
  DockTitleBar(QDockWidget* dock, std::string_view icon);

private:
  void UpdateButtons();

  QDockWidget* m_dock;
  QLabel* m_title;
  QToolButton* m_float_button;
  QToolButton* m_close_button;
};
