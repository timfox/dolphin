// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QColor>
#include <QToolBar>

class QAction;

namespace Core
{
enum class State;
}

class ToolBar final : public QToolBar
{
  Q_OBJECT
  // The buttons between separators are drawn on a shared raised panel, like the devkit concept's
  // button clusters. Themes set the colors with qproperty-groupBackground / qproperty-groupBorder;
  // an invalid (unset) background turns the panels off.
  Q_PROPERTY(QColor groupBackground MEMBER m_group_background)
  Q_PROPERTY(QColor groupBorder MEMBER m_group_border)

public:
  explicit ToolBar(QWidget* parent = nullptr);

  void closeEvent(QCloseEvent*) override;

protected:
  bool event(QEvent* event) override;
  void actionEvent(QActionEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

public:

  // Project (Build/Run) buttons are only shown in the editor layout.
  void SetProjectActionsVisible(bool visible);
  void SetProjectState(bool project_open, bool building);
signals:
  void OpenPressed();
  void BuildPressed();
  void RunPressed();
  void RefreshPressed();
  void PlayPressed();
  void PausePressed();
  void StopPressed();
  void ResetPressed();
  void FullScreenPressed();
  void ScreenShotPressed();

  void SettingsPressed();
  void ControllersPressed();
  void GraphicsPressed();

  void StepPressed();
  void StepOverPressed();
  void StepOutPressed();
  void SkipPressed();
  void ShowPCPressed();
  void SetPCPressed();

private:
  void OnEmulationStateChanged(Core::State state);
  void OnDebugModeToggled(bool enabled);

  void MakeActions();
  void UpdateIcons();
  void UpdatePausePlayButtonState(bool playing_state);
  void UpdateMuteButtonState();

  QAction* m_open_action;
  QAction* m_refresh_action;
  QAction* m_build_action;
  QAction* m_run_action;
  QAction* m_build_separator;
  QAction* m_pause_play_action;
  QAction* m_stop_action;
  QAction* m_reset_action;
  QAction* m_fullscreen_action;
  QAction* m_screenshot_action;
  QAction* m_mute_action;
  QAction* m_config_action;
  QAction* m_controllers_action;
  QAction* m_graphics_action;

  QAction* m_step_action;
  QAction* m_step_over_action;
  QAction* m_step_out_action;
  QAction* m_skip_action;
  QAction* m_show_pc_action;
  QAction* m_set_pc_action;
  QAction* m_debug_separator;

  QColor m_group_background;
  QColor m_group_border;
};
