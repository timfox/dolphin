// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include <QDockWidget>
#include <QWidget>

#include "Common/CommonTypes.h"
#include "DolphinQt/Debugger/MemoryAnalyzer.h"

class QComboBox;
class QLabel;
class QSplitter;
class QStackedWidget;
class QTableWidget;

// Grid of memory pages (4 KiB each), coloured by what lives there, dimmed where the page is all
// zeroes and lit up where the game wrote recently.
class MemoryPageMapView final : public QWidget
{
  Q_OBJECT
public:
  MemoryPageMapView(MemoryAnalyzer* analyzer, QWidget* parent = nullptr);

  void SetSpace(MemorySpace space);
  void SetHighlight(std::optional<std::pair<u32, u32>> range);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

signals:
  void AddressClicked(u32 address);
  void AddressActivated(u32 address);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  struct Geometry
  {
    int columns = 128;
    int rows = 0;
    double cell_width = 0;
    double cell_height = 0;
    QRectF grid;
  };
  Geometry ComputeGeometry() const;
  std::optional<size_t> PageAt(const QPoint& pos) const;

  MemoryAnalyzer* m_analyzer;
  MemorySpace m_space = MemorySpace::MEM1;
  std::optional<std::pair<u32, u32>> m_highlight;
  std::optional<size_t> m_hover_page;
};

// "Memory Map" dock: page map of MEM1 / MEM2 / ARAM plus a table of the regions in it (OS low
// memory, executable sections, arena, heaps and thread stacks).
class MemoryMapWidget final : public QDockWidget
{
  Q_OBJECT
public:
  MemoryMapWidget(MemoryAnalyzer* analyzer, QWidget* parent = nullptr);

signals:
  // |auxiliary| is set for ARAM offsets.
  void ShowMemory(u32 address, bool auxiliary);
  void RequestRangedBreakpoint(u32 from, u32 to);

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  void OnUpdated();
  void UpdateSpaces();
  void UpdateTable();
  void UpdateSummary();
  void OnSelectionChanged();
  void ShowContextMenu(const QPoint& pos);
  void SelectRegionAt(u32 address);
  MemorySpace CurrentSpace() const;
  const MemoryRegion* RegionForRow(int row) const;

  MemoryAnalyzer* m_analyzer;
  QStackedWidget* m_stack;
  QLabel* m_placeholder;
  QComboBox* m_space_combo;
  QLabel* m_summary;
  QLabel* m_source;
  MemoryPageMapView* m_map;
  QSplitter* m_splitter;
  QTableWidget* m_table;
  // Start/end of each row's region, to keep the selection across refreshes.
  std::vector<std::pair<u32, u32>> m_row_ranges;
};
