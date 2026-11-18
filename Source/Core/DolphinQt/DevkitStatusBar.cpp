// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitStatusBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/PerformanceMetrics.h"

DevkitStatusBar::DevkitStatusBar(QWidget* parent) : QStatusBar(parent)
{
  setObjectName(QStringLiteral("devkitStatusBar"));
  setSizeGripEnabled(false);

  m_state = new QLabel;
  m_state->setObjectName(QStringLiteral("statusState"));
  addWidget(m_state, 1);

  m_title = AddSegment("gamepad");
  m_cpu = AddSegment("microchip");
  m_mem = AddSegment("memory");
  m_backend = AddSegment("display");
  m_speed = AddSegment("gauge-high");
  m_fps = AddSegment("film");

  m_timer = new QTimer(this);
  m_timer->setInterval(500);
  connect(m_timer, &QTimer::timeout, this, &DevkitStatusBar::RefreshLive);

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &DevkitStatusBar::OnEmulationStateChanged);
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &DevkitStatusBar::RefreshStatic);

  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

QLabel* DevkitStatusBar::AddSegment(const char* icon)
{
  auto* const segment = new QWidget;
  segment->setObjectName(QStringLiteral("statusSegment"));
  auto* const layout = new QHBoxLayout(segment);
  layout->setContentsMargins(10, 0, 10, 0);
  layout->setSpacing(6);

  auto* const icon_label = new QLabel;
  icon_label->setPixmap(IconSet::Get(icon, QColor(QStringLiteral("#8f7dff"))).pixmap(14, 14));
  auto* const text = new QLabel;
  layout->addWidget(icon_label);
  layout->addWidget(text);

  addPermanentWidget(segment);
  return text;
}

void DevkitStatusBar::OnEmulationStateChanged(Core::State state)
{
  QString text;
  QString color;
  switch (state)
  {
  case Core::State::Running:
    text = tr("Running");
    color = QStringLiteral("#3ddc84");
    break;
  case Core::State::Paused:
    text = tr("Paused");
    color = QStringLiteral("#ffc857");
    break;
  case Core::State::Starting:
    text = tr("Starting");
    color = QStringLiteral("#5fe3ff");
    break;
  case Core::State::Stopping:
    text = tr("Stopping");
    color = QStringLiteral("#ff6b9d");
    break;
  case Core::State::Uninitialized:
  default:
    text = tr("Ready");
    color = QStringLiteral("#3ddc84");
    break;
  }
  m_state->setText(QStringLiteral("<span style=\"color:%1\">&#9679;</span>&nbsp;&nbsp;%2")
                       .arg(color, text.toHtmlEscaped()));

  if (state == Core::State::Running || state == Core::State::Paused)
    m_timer->start();
  else
    m_timer->stop();

  RefreshStatic();
  RefreshLive();
}

void DevkitStatusBar::RefreshStatic()
{
  auto& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  const bool active = state == Core::State::Running || state == Core::State::Paused;

  const std::string game_id = active ? SConfig::GetInstance().GetGameID() : std::string();
  m_title->setText(game_id.empty() ? tr("No title") : QString::fromStdString(game_id));

  if (active)
  {
    const double mhz = system.GetSystemTimers().GetTicksPerSecond() / 1'000'000.0;
    m_cpu->setText(tr("CPU: %1 MHz").arg(mhz, 0, 'f', 0));

    const auto& memory = system.GetMemory();
    const u32 mem1 = memory.GetRamSizeReal() / (1024 * 1024);
    const u32 mem2 = memory.GetExRamSizeReal() / (1024 * 1024);
    m_mem->setText(mem2 != 0 ? tr("MEM: %1 MB + %2 MB").arg(mem1).arg(mem2) :
                               tr("MEM: %1 MB").arg(mem1));
  }
  else
  {
    m_cpu->setText(tr("CPU: --"));
    m_mem->setText(tr("MEM: --"));
  }

  m_backend->setText(QString::fromStdString(Config::Get(Config::MAIN_GFX_BACKEND)));
}

void DevkitStatusBar::RefreshLive()
{
  auto& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  if (state != Core::State::Running && state != Core::State::Paused)
  {
    m_speed->setText(tr("Speed: --"));
    m_fps->setText(tr("-- FPS"));
    return;
  }

  const auto& metrics = system.GetPerfMetrics();
  m_speed->setText(tr("Speed: %1%").arg(metrics.GetSpeed() * 100.0, 0, 'f', 0));
  m_fps->setText(tr("%1 FPS").arg(metrics.GetFPS(), 0, 'f', 2));
}
