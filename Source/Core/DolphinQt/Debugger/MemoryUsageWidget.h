// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDockWidget>
#include <QWidget>

#include "Common/CommonTypes.h"
#include "DolphinQt/Debugger/MemoryAnalyzer.h"

class QComboBox;
class QLabel;
class QStackedWidget;
class QTableWidget;

// Line graph of memory use over the last couple of minutes: bytes in use (non-zero pages), heap
// allocations when the heaps are known, and write traffic.
class MemoryUsageGraph final : public QWidget
{
  Q_OBJECT
public:
  MemoryUsageGraph(MemoryAnalyzer* analyzer, QWidget* parent = nullptr);

  void SetSpace(MemorySpace space);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  MemoryAnalyzer* m_analyzer;
  MemorySpace m_space = MemorySpace::MEM1;
};

// "Memory Usage" dock: live usage graph with headline numbers, plus thread stack and heap tables.
class MemoryUsageWidget final : public QDockWidget
{
  Q_OBJECT
public:
  MemoryUsageWidget(MemoryAnalyzer* analyzer, QWidget* parent = nullptr);

signals:
  void ShowMemory(u32 address, bool auxiliary);

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  QWidget* CreateStat(const QString& title, QLabel** value, QLabel** detail);
  void OnUpdated();
  void UpdateSpaces();
  void UpdateStats();
  void UpdateStacks();
  void UpdateHeaps();
  MemorySpace CurrentSpace() const;

  MemoryAnalyzer* m_analyzer;
  QStackedWidget* m_stack;
  QLabel* m_placeholder;
  QComboBox* m_space_combo;
  MemoryUsageGraph* m_graph;

  QLabel* m_in_use_value;
  QLabel* m_in_use_detail;
  QLabel* m_program_value;
  QLabel* m_program_detail;
  QLabel* m_heap_value;
  QLabel* m_heap_detail;
  QLabel* m_stack_value;
  QLabel* m_stack_detail;
  QLabel* m_write_value;
  QLabel* m_write_detail;

  QTableWidget* m_stack_table;
  QTableWidget* m_heap_table;
  QLabel* m_heap_hint;
};
