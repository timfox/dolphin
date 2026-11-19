// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <vector>

#include <QDockWidget>
#include <QString>
#include <QWidget>

#include "VideoCommon/RenderProfiler.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Bar graph of one value per frame. Bars over the warning/critical thresholds are coloured, and
// clicking a bar selects that frame.
class FrameBarGraph final : public QWidget
{
  Q_OBJECT
public:
  explicit FrameBarGraph(QWidget* parent = nullptr);

  // |unit| is appended to values in labels, e.g. "ms".
  void SetValues(std::vector<double> values, const QString& unit);
  // Drawn in the top-left corner of the plot.
  void SetTitle(const QString& title);
  // Horizontal guide lines (e.g. 16.7 ms and 33.3 ms frame budgets). Bars over |warning| are drawn
  // in the warning colour, over |critical| in the critical colour. 0 disables a threshold.
  void SetThresholds(double warning, double critical);
  void SetGuideLines(std::vector<std::pair<double, QString>> guides);
  // -1 for none (the newest frame is shown).
  void SetSelected(int index);
  int GetSelected() const { return m_selected; }

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

signals:
  void FrameSelected(int index);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  int IndexAt(int x) const;
  QRect PlotRect() const;

  std::vector<double> m_values;
  QString m_title;
  QString m_unit;
  double m_warning = 0;
  double m_critical = 0;
  std::vector<std::pair<double, QString>> m_guides;
  int m_selected = -1;
  int m_hover = -1;
};

// Small histogram: draws per TEV stage count (1-16) for one frame.
class TevHistogram final : public QWidget
{
  Q_OBJECT
public:
  explicit TevHistogram(QWidget* parent = nullptr);

  void SetCounts(const std::array<u32, RenderProfiler::MAX_TEV_STAGES>& counts);

  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  std::array<u32, RenderProfiler::MAX_TEV_STAGES> m_counts{};
};

// "Render Profiler" dock: frame time graph, a selectable per-frame GX metric graph, a counters
// table (selected frame / average / peak) and TEV stage usage, fed by VideoCommon/RenderProfiler.
class RenderProfilerWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit RenderProfilerWidget(QWidget* parent = nullptr);
  ~RenderProfilerWidget() override;

signals:
  void CaptureFrameRequested();
  void FIFOPlayerRequested();

private:
  void UpdateRecording();
  void Refresh();
  void UpdateSelection();

  QToolButton* m_record;
  QLabel* m_live;
  FrameBarGraph* m_frame_graph;
  QComboBox* m_metric;
  FrameBarGraph* m_metric_graph;
  QTableWidget* m_table;
  TevHistogram* m_tev;
  QLabel* m_tev_label;
  QTimer* m_timer;

  std::vector<RenderProfiler::FrameSample> m_history;
  // Frame number of the selected frame, if the user picked one.
  std::optional<u64> m_selected_frame;
};

// "Frame Capture" dock: every draw call and EFB/XFB copy of one captured frame, with the GX state
// of the selected event and CSV export.
class FrameCaptureWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit FrameCaptureWidget(QWidget* parent = nullptr);

  // Captures the next frame the game renders.
  void RequestCapture();

private:
  void Poll();
  void Populate();
  void ApplyFilter();
  void ShowDetails(QTreeWidgetItem* item);
  void ExportCSV();

  QToolButton* m_capture;
  QToolButton* m_export;
  QLabel* m_summary;
  QLineEdit* m_filter;
  QTreeWidget* m_tree;
  QTableWidget* m_details;
  QTimer* m_poll_timer;

  std::optional<RenderProfiler::FrameCapture> m_capture_data;
};
