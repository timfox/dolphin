// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QStatusBar>

class QLabel;
class QTimer;

namespace Core
{
enum class State;
}

// Devkit-style status bar: emulation state on the left; running title, CPU clock, memory size,
// video backend, speed and frame rate on the right.
class DevkitStatusBar final : public QStatusBar
{
  Q_OBJECT
public:
  explicit DevkitStatusBar(QWidget* parent = nullptr);

private:
  void OnEmulationStateChanged(Core::State state);
  void RefreshStatic();
  void RefreshLive();
  QLabel* AddSegment(const char* icon);

  QLabel* m_state;
  QLabel* m_title;
  QLabel* m_cpu;
  QLabel* m_mem;
  QLabel* m_vi;
  QLabel* m_backend;
  QLabel* m_speed;
  QLabel* m_fps;
  QTimer* m_timer;
};
