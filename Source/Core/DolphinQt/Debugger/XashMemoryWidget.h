// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDockWidget>
#include <QElapsedTimer>
#include <QWidget>

#include "Common/CommonTypes.h"
#include "DolphinQt/Debugger/MemoryAnalyzer.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableWidget;

// One bar split by pool category, so the biggest consumers stand out at a glance.
class XashPoolCategoryBar final : public QWidget
{
  Q_OBJECT
public:
  explicit XashPoolCategoryBar(QWidget* parent = nullptr);

  void SetPools(const std::vector<Xash::Pool>& pools);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  std::vector<std::pair<Xash::PoolCategory, u64>> m_totals;
  u64 m_total = 0;
};

// "Xash3D Memory" dock: the engine's own view of memory in a running xash3d-gc build. Zone
// pools (one per model, sprite and WAD, so also a list of what's resident), allocations grouped
// by the source line that made them, the C heap, the ARAM heap and asset cache, and memory per
// map visit.
class XashMemoryWidget final : public QDockWidget
{
  Q_OBJECT
public:
  XashMemoryWidget(MemoryAnalyzer* analyzer, QWidget* parent = nullptr);

signals:
  void ShowMemory(u32 address, bool auxiliary);

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  QWidget* CreateStat(const QString& title, QLabel** value, QLabel** detail);
  void OnUpdated();
  void LoadSymbols();
  void UpdateStats();
  void UpdatePools();
  void UpdateSites(bool force);
  void UpdateSiteCombo();
  void UpdateAram();
  void UpdateMaps();
  void UpdateMapDetail();

  MemoryAnalyzer* m_analyzer;
  QStackedWidget* m_stack;
  QLabel* m_placeholder;
  QLabel* m_not_detected;
  QLabel* m_symbols_label;
  QPushButton* m_load_button;
  QPushButton* m_unload_button;
  QPushButton* m_mark_button;
  QPushButton* m_clear_mark_button;
  QLabel* m_problem;

  QLabel* m_zone_value;
  QLabel* m_zone_detail;
  QLabel* m_heap_value;
  QLabel* m_heap_detail;
  QLabel* m_arena_value;
  QLabel* m_arena_detail;
  QLabel* m_aram_value;
  QLabel* m_aram_detail;
  QLabel* m_map_value;
  QLabel* m_map_detail;

  QTabWidget* m_tabs;
  XashPoolCategoryBar* m_category_bar;
  QTableWidget* m_pool_table;
  QComboBox* m_site_pool;
  QTableWidget* m_site_table;
  QLabel* m_site_summary;
  QElapsedTimer m_site_timer;
  QTableWidget* m_aram_table;
  QTableWidget* m_asset_table;
  QLabel* m_aram_summary;
  QTableWidget* m_map_table;
  QTableWidget* m_map_detail_table;
  QLabel* m_map_detail_label;
  bool m_pools_sized = false;
};
