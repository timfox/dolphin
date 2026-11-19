// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDockWidget>

class QCheckBox;
class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace Core
{
enum class State;
}

// "GX State" dock: the GPU's current blitting processor state, as the game set it up through GX:
// the TEV stages with their color and alpha combiner equations, the textures they sample, the TEV
// registers and the pixel engine (depth, blending, alpha test, fog). Refreshed when emulation
// pauses, on request, or live once a second.
class GXStateWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit GXStateWidget(QWidget* parent = nullptr);

  void Refresh();

private:
  void OnEmulationStateChanged(Core::State state);

  QTreeWidget* m_tree;
  QLabel* m_status;
  QCheckBox* m_live;
  QTimer* m_timer;
};
