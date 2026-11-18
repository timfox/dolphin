// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/ToolBar.h"

#include <QAction>
#include <QIcon>
#include <QSizePolicy>

#include "AudioCommon/AudioCommon.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/DevkitPanels.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/Settings.h"

static QSize ICON_SIZE(26, 26);

ToolBar::ToolBar(QWidget* parent) : QToolBar(parent)
{
  setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  setMovable(!Settings::Instance().AreWidgetsLocked());
  setFloatable(false);
  setIconSize(ICON_SIZE);
  setVisible(Settings::Instance().IsToolBarVisible());

  setWindowTitle(tr("Toolbar"));
  setObjectName(QStringLiteral("toolbar"));

  MakeActions();
  connect(&Settings::Instance(), &Settings::ThemeChanged, this, &ToolBar::UpdateIcons);
  UpdateIcons();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          [this](Core::State state) { OnEmulationStateChanged(state); });

  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this,
          [this] { OnEmulationStateChanged(Core::GetState(Core::System::GetInstance())); });

  connect(&Settings::Instance(), &Settings::DebugModeToggled, this, &ToolBar::OnDebugModeToggled);

  // Mute can also change from the hotkey, the throttle hotkey or the audio settings pane.
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &ToolBar::UpdateMuteButtonState);

  connect(&Settings::Instance(), &Settings::ToolBarVisibilityChanged, this, &ToolBar::setVisible);
  connect(this, &ToolBar::visibilityChanged, &Settings::Instance(), &Settings::SetToolBarVisible);

  connect(&Settings::Instance(), &Settings::WidgetLockChanged, this,
          [this](bool locked) { setMovable(!locked); });

  connect(&Settings::Instance(), &Settings::GameListRefreshRequested, this,
          [this] { m_refresh_action->setEnabled(false); });
  connect(&Settings::Instance(), &Settings::GameListRefreshStarted, this,
          [this] { m_refresh_action->setEnabled(true); });

  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  OnDebugModeToggled(Settings::Instance().IsDebugModeEnabled());
}

void ToolBar::OnEmulationStateChanged(Core::State state)
{
  bool running = state != Core::State::Uninitialized;
  m_stop_action->setEnabled(running);
  m_reset_action->setEnabled(running);
  m_fullscreen_action->setEnabled(running);
  m_screenshot_action->setEnabled(running);

  bool playing = running && state != Core::State::Paused;
  UpdatePausePlayButtonState(playing);

  const bool paused = Core::GetState(Core::System::GetInstance()) == Core::State::Paused;
  m_step_action->setEnabled(paused);
  m_step_over_action->setEnabled(paused);
  m_step_out_action->setEnabled(paused);
  m_skip_action->setEnabled(paused);
  m_set_pc_action->setEnabled(paused);
}

void ToolBar::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetToolBarVisible(false);
}

void ToolBar::OnDebugModeToggled(bool enabled)
{
  m_step_action->setVisible(enabled);
  m_step_over_action->setVisible(enabled);
  m_step_out_action->setVisible(enabled);
  m_skip_action->setVisible(enabled);
  m_show_pc_action->setVisible(enabled);
  m_set_pc_action->setVisible(enabled);

  const bool paused = Core::GetState(Core::System::GetInstance()) == Core::State::Paused;
  m_step_action->setEnabled(paused);
  m_step_over_action->setEnabled(paused);
  m_step_out_action->setEnabled(paused);
  m_skip_action->setEnabled(paused);
  m_set_pc_action->setEnabled(paused);
}

void ToolBar::MakeActions()
{
  // Layout follows the devkit concept: file actions, then emulation control, then settings, then
  // the debugger stepping controls (debug mode only), with the target status panel on the right.
  m_open_action = addAction(tr("Open"), this, &ToolBar::OpenPressed);
  m_refresh_action = addAction(tr("Refresh"), [this] { emit RefreshPressed(); });
  m_refresh_action->setEnabled(false);

  addSeparator();

  m_build_action = addAction(tr("Build"), this, &ToolBar::BuildPressed);
  m_build_action->setToolTip(tr("Build the open project (Ctrl+B)"));
  m_run_action = addAction(tr("Run"), this, &ToolBar::RunPressed);
  m_run_action->setToolTip(tr("Build the open project and boot it in the Game View (Ctrl+R)"));
  m_build_separator = addSeparator();

  m_pause_play_action = addAction(tr("Play"), this, &ToolBar::PlayPressed);
  m_stop_action = addAction(tr("Stop"), this, &ToolBar::StopPressed);
  m_reset_action = addAction(tr("Reset"), this, &ToolBar::ResetPressed);
  m_fullscreen_action = addAction(tr("Fullscreen"), this, &ToolBar::FullScreenPressed);
  m_screenshot_action = addAction(tr("Screenshot"), this, &ToolBar::ScreenShotPressed);
  m_mute_action = addAction(tr("Mute"), this, [this] {
    AudioCommon::ToggleMuteVolume(Core::System::GetInstance());
    UpdateMuteButtonState();
  });
  m_mute_action->setCheckable(true);

  addSeparator();

  m_config_action = addAction(tr("Config"), this, &ToolBar::SettingsPressed);
  m_graphics_action = addAction(tr("Graphics"), this, &ToolBar::GraphicsPressed);
  m_controllers_action = addAction(tr("Controllers"), this, &ToolBar::ControllersPressed);

  // i18n: Here, "Step" is a verb. This feature is used for
  // going through code step by step.
  m_step_action = addAction(tr("Step"), this, &ToolBar::StepPressed);
  // i18n: Here, "Step" is a verb. This feature is used for
  // going through code step by step.
  m_step_over_action = addAction(tr("Step Over"), this, &ToolBar::StepOverPressed);
  // i18n: Here, "Step" is a verb. This feature is used for
  // going through code step by step.
  m_step_out_action = addAction(tr("Step Out"), this, &ToolBar::StepOutPressed);
  m_skip_action = addAction(tr("Skip"), this, &ToolBar::SkipPressed);
  // i18n: Here, PC is an acronym for program counter, not personal computer.
  m_show_pc_action = addAction(tr("Show PC"), this, &ToolBar::ShowPCPressed);
  // i18n: Here, PC is an acronym for program counter, not personal computer.
  m_set_pc_action = addAction(tr("Set PC"), this, &ToolBar::SetPCPressed);

  auto* const spacer = new QWidget(this);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  addWidget(spacer);
  addWidget(new TargetStatusWidget(this));

  // Give the buttons a common minimum width so the row looks even, without padding short labels
  // out to the width of the longest one.
  for (QAction* const action : actions())
  {
    if (QWidget* const widget = widgetForAction(action); widget && action->isSeparator() == false &&
                                                         !action->text().isEmpty())
    {
      widget->setMinimumWidth(56);
    }
  }
}

void ToolBar::UpdatePausePlayButtonState(const bool playing_state)
{
  if (playing_state)
  {
    disconnect(m_pause_play_action, nullptr, nullptr, nullptr);
    m_pause_play_action->setText(tr("Pause"));
    m_pause_play_action->setIcon(IconSet::Get("pause"));
    connect(m_pause_play_action, &QAction::triggered, this, &ToolBar::PausePressed);
  }
  else
  {
    disconnect(m_pause_play_action, nullptr, nullptr, nullptr);
    m_pause_play_action->setText(tr("Play"));
    m_pause_play_action->setIcon(IconSet::Get("play"));
    connect(m_pause_play_action, &QAction::triggered, this, &ToolBar::PlayPressed);
  }
}

void ToolBar::UpdateMuteButtonState()
{
  const bool muted = Config::Get(Config::MAIN_AUDIO_MUTED);
  m_mute_action->setChecked(muted);
  m_mute_action->setText(muted ? tr("Unmute") : tr("Mute"));
  m_mute_action->setToolTip(muted ? tr("Unmute audio") : tr("Mute audio"));
  m_mute_action->setIcon(IconSet::Get(muted ? "volume-xmark" : "volume-high"));
}

void ToolBar::UpdateIcons()
{
  // Font Awesome Free icons, tinted to match the active palette.
  m_step_action->setIcon(IconSet::Get("arrow-turn-down"));
  m_step_over_action->setIcon(IconSet::Get("share"));
  m_step_out_action->setIcon(IconSet::Get("arrow-turn-up"));
  m_skip_action->setIcon(IconSet::Get("forward"));
  m_show_pc_action->setIcon(IconSet::Get("location-crosshairs"));
  m_set_pc_action->setIcon(IconSet::Get("location-dot"));

  m_open_action->setIcon(IconSet::Get("folder-open"));
  m_build_action->setIcon(IconSet::Get("screwdriver-wrench"));
  m_run_action->setIcon(IconSet::Get("circle-play"));
  m_refresh_action->setIcon(IconSet::Get("rotate-right"));

  const Core::State state = Core::GetState(Core::System::GetInstance());
  const bool playing = state != Core::State::Uninitialized && state != Core::State::Paused;
  m_pause_play_action->setIcon(IconSet::Get(playing ? "pause" : "play"));

  m_stop_action->setIcon(IconSet::Get("stop"));
  m_reset_action->setIcon(IconSet::Get("rotate-left"));
  m_fullscreen_action->setIcon(IconSet::Get("expand"));
  m_screenshot_action->setIcon(IconSet::Get("camera"));
  UpdateMuteButtonState();
  m_config_action->setIcon(IconSet::Get("gear"));
  m_controllers_action->setIcon(IconSet::Get("gamepad"));
  m_graphics_action->setIcon(IconSet::Get("display"));
}

void ToolBar::SetProjectActionsVisible(bool visible)
{
  m_build_action->setVisible(visible);
  m_run_action->setVisible(visible);
  m_build_separator->setVisible(visible);
}

void ToolBar::SetProjectState(bool project_open, bool building)
{
  m_build_action->setEnabled(project_open && !building);
  m_run_action->setEnabled(project_open && !building);
  m_build_action->setText(building ? tr("Building") : tr("Build"));
}
