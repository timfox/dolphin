// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitRenderProfiler.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/FromStdString.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/QtUtils/TableExport.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureDecoder.h"

namespace
{
using RenderProfiler::FrameSample;

// 60 Hz / 30 Hz frame budgets.
constexpr double BUDGET_60_MS = 1000.0 / 60.0;
constexpr double BUDGET_30_MS = 1000.0 / 30.0;

QColor WarningColor()
{
  return QColor(0xf2, 0xb1, 0x3a);
}

QColor CriticalColor()
{
  return QColor(0xe8, 0x4a, 0x5f);
}

struct Metric
{
  const char* name;
  const char* tooltip;
  double (*get)(const FrameSample&);
  // Shown with one decimal and this suffix (e.g. "ms"); integers otherwise.
  const char* unit = nullptr;
  bool graphable = true;
};

// The counters shown in the table, in order. Everything here is gathered by every video backend.
const Metric METRICS[] = {
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Frame time"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Host time between the ends of two frames."),
     [](const FrameSample& s) { return s.frame_ms; }, "ms"},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "GPU thread time"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget",
                       "Host time between the frame's first draw and its XFB copy."),
     [](const FrameSample& s) { return s.gpu_thread_ms; }, "ms"},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Draw calls"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Batches flushed to the host GPU."),
     [](const FrameSample& s) { return double(s.stats.num_draw_calls); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Culled draws"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Batches skipped because cull mode was All."),
     [](const FrameSample& s) { return double(s.culled_draws); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Primitives"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Vertices submitted through GX primitive commands."),
     [](const FrameSample& s) { return double(s.stats.num_prims + s.stats.num_dl_prims); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Primitive joins"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget",
                       "GX primitive commands merged into an existing batch."),
     [](const FrameSample& s) { return double(s.stats.num_primitive_joins); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Vertices drawn"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Vertices in batches that weren't culled."),
     [](const FrameSample& s) { return double(s.vertices); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Indices drawn"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Indices in batches that weren't culled."),
     [](const FrameSample& s) { return double(s.indices); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Pipeline changes"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget",
                       "Draws that switched shader/render state (each is a GX state change)."),
     [](const FrameSample& s) { return double(s.pipeline_changes); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Shaders compiled"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "New host shaders created this frame (stutter)."),
     [](const FrameSample& s) { return double(s.shaders_created); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Textures uploaded"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Textures decoded and uploaded this frame."),
     [](const FrameSample& s) { return double(s.textures_uploaded); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Textured draws"), nullptr,
     [](const FrameSample& s) { return double(s.textured_draws); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Blended draws"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Draws with blending, subtract or a logic op."),
     [](const FrameSample& s) { return double(s.blended_draws); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Indirect texture draws"), nullptr,
     [](const FrameSample& s) { return double(s.indirect_draws); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Fogged draws"), nullptr,
     [](const FrameSample& s) { return double(s.fog_draws); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "EFB copies (texture)"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "EFB copies kept on the host GPU only."),
     [](const FrameSample& s) { return double(s.efb_copies_to_texture); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "EFB copies (RAM)"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget",
                       "EFB copies written back to emulated RAM. These stall the GPU."),
     [](const FrameSample& s) { return double(s.efb_copies_to_ram); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "XFB copies"), nullptr,
     [](const FrameSample& s) { return double(s.xfb_copies); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Depth copies"), nullptr,
     [](const FrameSample& s) { return double(s.depth_copies); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Copied pixels"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "Pixels copied out of the EFB (native size)."),
     [](const FrameSample& s) { return double(s.efb_copy_pixels); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "EFB peeks"),
     QT_TRANSLATE_NOOP("RenderProfilerWidget", "CPU reads of the EFB. These stall the GPU."),
     [](const FrameSample& s) { return double(s.stats.num_efb_peeks); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "EFB pokes"), nullptr,
     [](const FrameSample& s) { return double(s.stats.num_efb_pokes); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "BP register loads"), nullptr,
     [](const FrameSample& s) { return double(s.stats.num_bp_loads); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "CP register loads"), nullptr,
     [](const FrameSample& s) { return double(s.stats.num_cp_loads); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "XF register loads"), nullptr,
     [](const FrameSample& s) { return double(s.stats.num_xf_loads); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Display lists called"), nullptr,
     [](const FrameSample& s) { return double(s.stats.num_dlists_called); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Vertex bytes streamed"), nullptr,
     [](const FrameSample& s) { return double(s.stats.bytes_vertex_streamed); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Index bytes streamed"), nullptr,
     [](const FrameSample& s) { return double(s.stats.bytes_index_streamed); }},
    {QT_TRANSLATE_NOOP("RenderProfilerWidget", "Uniform bytes streamed"), nullptr,
     [](const FrameSample& s) { return double(s.stats.bytes_uniform_streamed); }},
};

QString FormatValue(const Metric& metric, double value)
{
  if (metric.unit)
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 2).arg(QString::fromLatin1(metric.unit));
  return QLocale().toString(static_cast<qlonglong>(std::llround(value)));
}

QToolButton* MakeToolButton(std::string_view icon, const QString& text, const QString& tooltip)
{
  auto* const button = new QToolButton;
  button->setIcon(IconSet::Get(icon));
  button->setText(text);
  button->setToolTip(tooltip);
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  button->setAutoRaise(true);
  return button;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// FrameBarGraph

FrameBarGraph::FrameBarGraph(QWidget* parent) : QWidget(parent)
{
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize FrameBarGraph::sizeHint() const
{
  return {420, 110};
}

QSize FrameBarGraph::minimumSizeHint() const
{
  return {160, 56};
}

void FrameBarGraph::SetValues(std::vector<double> values, const QString& unit)
{
  m_values = std::move(values);
  m_unit = unit;
  if (m_selected >= static_cast<int>(m_values.size()))
    m_selected = -1;
  update();
}

void FrameBarGraph::SetTitle(const QString& title)
{
  m_title = title;
  update();
}

void FrameBarGraph::SetThresholds(double warning, double critical)
{
  m_warning = warning;
  m_critical = critical;
  update();
}

void FrameBarGraph::SetGuideLines(std::vector<std::pair<double, QString>> guides)
{
  m_guides = std::move(guides);
  update();
}

void FrameBarGraph::SetSelected(int index)
{
  m_selected = index;
  update();
}

QRect FrameBarGraph::PlotRect() const
{
  // Room on the left for the scale label.
  return rect().adjusted(4, 4, -4, -14);
}

int FrameBarGraph::IndexAt(int x) const
{
  if (m_values.empty())
    return -1;
  const QRect plot = PlotRect();
  const double slot = static_cast<double>(plot.width()) / RenderProfiler::HISTORY_SIZE;
  // Newest frame is at the right edge.
  const int from_right = static_cast<int>((plot.right() - x) / slot);
  const int index = static_cast<int>(m_values.size()) - 1 - from_right;
  if (index < 0 || index >= static_cast<int>(m_values.size()))
    return -1;
  return index;
}

void FrameBarGraph::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  const QPalette& pal = palette();
  painter.fillRect(rect(), pal.color(QPalette::Base));

  const QRect plot = PlotRect();
  QColor grid = pal.color(QPalette::Text);
  grid.setAlpha(40);
  painter.setPen(grid);
  painter.drawRect(plot.adjusted(0, 0, -1, -1));

  double max_value = 0;
  for (const double v : m_values)
    max_value = std::max(max_value, v);
  for (const auto& [value, label] : m_guides)
    max_value = std::max(max_value, value * 1.15);
  if (max_value <= 0)
    max_value = 1;

  const auto y_for = [&](double v) {
    return plot.bottom() - static_cast<int>(v / max_value * (plot.height() - 2));
  };

  const double slot = static_cast<double>(plot.width()) / RenderProfiler::HISTORY_SIZE;
  const QColor normal = pal.color(QPalette::Highlight);
  const int count = static_cast<int>(m_values.size());
  for (int i = 0; i < count; ++i)
  {
    const double v = m_values[i];
    QColor color = normal;
    if (m_critical > 0 && v > m_critical)
      color = CriticalColor();
    else if (m_warning > 0 && v > m_warning)
      color = WarningColor();
    if (i == m_hover || i == m_selected)
      color = color.lighter(150);

    // Integer bar edges, so neighbouring bars never leave a rounding gap between them.
    const int left = plot.right() + 1 - static_cast<int>(std::lround((count - i) * slot));
    const int next = plot.right() + 1 - static_cast<int>(std::lround((count - i - 1) * slot));
    const int right = std::max(left, next - 1 - (slot > 3 ? 1 : 0));
    const int top = y_for(v);
    painter.fillRect(QRect(QPoint(left, top), QPoint(right, plot.bottom() - 1)), color);
  }

  // Selected frame marker.
  if (m_selected >= 0 && m_selected < count)
  {
    const int x = static_cast<int>(plot.right() - (count - m_selected - 0.5) * slot);
    QColor marker = pal.color(QPalette::Text);
    marker.setAlpha(160);
    painter.setPen(QPen(marker, 1, Qt::DashLine));
    painter.drawLine(x, plot.top(), x, plot.bottom());
  }

  QFont small = font();
  small.setPointSizeF(std::max(6.5, small.pointSizeF() * 0.8));
  painter.setFont(small);

  const int text_h = QFontMetrics(small).height();
  int last_label_y = INT_MAX;
  for (const auto& [value, label] : m_guides)
  {
    const int y = y_for(value);
    QColor line = pal.color(QPalette::Text);
    line.setAlpha(90);
    painter.setPen(QPen(line, 1, Qt::DotLine));
    painter.drawLine(plot.left(), y, plot.right(), y);
    // Labels sit at the right end; skip one that would overlap the previous label.
    if (std::abs(last_label_y - y) > text_h && y - text_h > plot.top())
    {
      painter.drawText(QRect(plot.left() + 4, y - text_h, plot.width() - 8, text_h),
                       Qt::AlignRight | Qt::AlignBottom, label);
      last_label_y = y;
    }
  }

  if (!m_title.isEmpty())
  {
    QFont bold = font();
    bold.setBold(true);
    painter.setFont(bold);
    painter.setPen(pal.color(QPalette::Text));
    painter.drawText(plot.adjusted(6, 3, -6, -3), Qt::AlignLeft | Qt::AlignTop, m_title);
    painter.setFont(small);
  }

  // Scale and latest value along the bottom.
  QColor text = pal.color(QPalette::Text);
  text.setAlpha(170);
  painter.setPen(text);
  const QRect footer(plot.left(), plot.bottom() + 1, plot.width(), 13);
  painter.drawText(footer, Qt::AlignLeft | Qt::AlignVCenter,
                   tr("max %1 %2").arg(max_value, 0, 'f', m_unit.isEmpty() ? 0 : 1).arg(m_unit));
  if (count > 0)
  {
    const int shown = (m_selected >= 0 && m_selected < count) ? m_selected : count - 1;
    painter.drawText(footer, Qt::AlignRight | Qt::AlignVCenter,
                     (m_selected >= 0 ? tr("selected %1 %2") : tr("last %1 %2"))
                         .arg(m_values[shown], 0, 'f', m_unit.isEmpty() ? 0 : 2)
                         .arg(m_unit));
  }
  else
  {
    painter.drawText(plot, Qt::AlignCenter, tr("No frames recorded"));
  }
}

void FrameBarGraph::mousePressEvent(QMouseEvent* event)
{
  if (event->button() == Qt::LeftButton)
  {
    const int index = IndexAt(event->position().toPoint().x());
    // Clicking the selected frame again (or empty space) goes back to following the newest frame.
    m_selected = (index == m_selected) ? -1 : index;
    update();
    emit FrameSelected(m_selected);
  }
  QWidget::mousePressEvent(event);
}

void FrameBarGraph::mouseMoveEvent(QMouseEvent* event)
{
  const int index = IndexAt(event->position().toPoint().x());
  if (index != m_hover)
  {
    m_hover = index;
    update();
  }
  if (index >= 0)
  {
    QToolTip::showText(event->globalPosition().toPoint(),
                       tr("%1 %2 (%3 frames ago)")
                           .arg(m_values[index], 0, 'f', m_unit.isEmpty() ? 0 : 2)
                           .arg(m_unit)
                           .arg(static_cast<int>(m_values.size()) - 1 - index),
                       this);
  }
  QWidget::mouseMoveEvent(event);
}

void FrameBarGraph::leaveEvent(QEvent* event)
{
  m_hover = -1;
  update();
  QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------------------------
// TevHistogram

TevHistogram::TevHistogram(QWidget* parent) : QWidget(parent)
{
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setMinimumSize(150, 60);
  setToolTip(tr("Draws by number of TEV stages (1-16) in the selected frame."));
}

QSize TevHistogram::sizeHint() const
{
  return {200, 100};
}

void TevHistogram::SetCounts(const std::array<u32, RenderProfiler::MAX_TEV_STAGES>& counts)
{
  m_counts = counts;
  update();
}

void TevHistogram::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  const QPalette& pal = palette();
  painter.fillRect(rect(), pal.color(QPalette::Base));

  QFont small = font();
  small.setPointSizeF(std::max(6.5, small.pointSizeF() * 0.75));
  painter.setFont(small);
  const int label_h = QFontMetrics(small).height();

  const QRect plot = rect().adjusted(4, label_h + 2, -4, -label_h - 2);
  const u32 max_count = std::max<u32>(1, *std::max_element(m_counts.begin(), m_counts.end()));
  const double slot = static_cast<double>(plot.width()) / m_counts.size();

  QColor text = pal.color(QPalette::Text);
  text.setAlpha(170);
  for (size_t i = 0; i < m_counts.size(); ++i)
  {
    const int left = plot.left() + static_cast<int>(i * slot) + 1;
    const int width = std::max(1, static_cast<int>(slot) - 2);
    const int h = static_cast<int>(static_cast<double>(m_counts[i]) / max_count * plot.height());
    QColor color = pal.color(QPalette::Highlight);
    // More stages means more fill cost; shade the deep end.
    if (i >= 8)
      color = WarningColor();
    painter.fillRect(left, plot.bottom() - h, width, h, color);

    painter.setPen(text);
    const bool narrow = slot < QFontMetrics(small).horizontalAdvance(QStringLiteral("16")) + 4;
    if (!narrow || i % 2 == 0)
    {
      painter.drawText(QRect(left - 4, plot.bottom() + 1, width + 8, label_h), Qt::AlignCenter,
                       QString::number(i + 1));
    }
    if (m_counts[i] != 0 && !narrow)
    {
      painter.drawText(QRect(left - 6, plot.bottom() - h - label_h, width + 12, label_h),
                       Qt::AlignCenter, QString::number(m_counts[i]));
    }
  }
}

// ---------------------------------------------------------------------------------------------
// RenderProfilerWidget

RenderProfilerWidget::RenderProfilerWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Render Profiler"));
  setObjectName(QStringLiteral("renderprofiler"));
  setAllowedAreas(Qt::AllDockWidgetAreas);

  auto* const contents = new QWidget;
  auto* const layout = new QVBoxLayout(contents);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);

  // Toolbar.
  auto* const toolbar = new QHBoxLayout;
  toolbar->setSpacing(2);
  m_record = MakeToolButton("circle-dot", tr("Record"),
                            tr("Record GX statistics for every frame while this panel is open."));
  m_record->setCheckable(true);
  m_record->setChecked(true);
  auto* const clear = MakeToolButton("trash-can", tr("Clear"), tr("Drop the recorded frames."));
  auto* const capture =
      MakeToolButton("crosshairs", tr("Capture Frame"),
                     tr("Capture every draw call and EFB copy of the next frame."));
  auto* const fifo = MakeToolButton(
      "cubes", tr("FIFO Log"),
      tr("Record the raw GX command stream with the FIFO player for replay and analysis."));
  m_live = new QLabel;
  m_live->setTextFormat(Qt::PlainText);
  toolbar->addWidget(m_record);
  toolbar->addWidget(clear);
  toolbar->addWidget(capture);
  toolbar->addWidget(fifo);
  toolbar->addStretch();
  toolbar->addWidget(m_live);
  layout->addLayout(toolbar);

  m_hints = new QLabel;
  m_hints->setTextFormat(Qt::RichText);
  m_hints->setVisible(false);
  layout->addWidget(m_hints);

  // Graphs | counters | TEV usage, side by side so the panel works in the short console strip.
  auto* const splitter = new QSplitter(Qt::Horizontal);
  splitter->setChildrenCollapsible(false);

  auto* const graphs = new QWidget;
  auto* const graphs_layout = new QVBoxLayout(graphs);
  graphs_layout->setContentsMargins(0, 0, 0, 0);
  graphs_layout->setSpacing(2);
  m_frame_graph = new FrameBarGraph;
  m_frame_graph->SetTitle(tr("Frame time"));
  m_frame_graph->SetThresholds(BUDGET_60_MS + 0.5, BUDGET_30_MS + 0.5);
  m_frame_graph->SetGuideLines({{BUDGET_60_MS, tr("16.7 ms (60 Hz)")},
                                {BUDGET_30_MS, tr("33.3 ms (30 Hz)")}});
  m_metric = new QComboBox;
  for (const Metric& metric : METRICS)
  {
    if (metric.graphable && !metric.unit)
      m_metric->addItem(tr(metric.name), static_cast<int>(&metric - METRICS));
  }
  m_metric_graph = new FrameBarGraph;
  graphs_layout->addWidget(m_frame_graph, 1);
  graphs_layout->addWidget(m_metric);
  graphs_layout->addWidget(m_metric_graph, 1);
  splitter->addWidget(graphs);

  m_table = new QTableWidget(0, 4);
  m_table->setHorizontalHeaderLabels({tr("Counter"), tr("Frame"), tr("Average"), tr("Peak")});
  m_table->verticalHeader()->hide();
  m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int col = 1; col < 4; ++col)
    m_table->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  TableExport::Install(m_table);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setAlternatingRowColors(true);
  m_table->setShowGrid(false);
  m_table->setRowCount(static_cast<int>(std::size(METRICS)));
  for (size_t row = 0; row < std::size(METRICS); ++row)
  {
    auto* const name = new QTableWidgetItem(tr(METRICS[row].name));
    if (METRICS[row].tooltip)
      name->setToolTip(tr(METRICS[row].tooltip));
    m_table->setItem(static_cast<int>(row), 0, name);
    for (int col = 1; col < 4; ++col)
    {
      auto* const item = new QTableWidgetItem;
      item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      m_table->setItem(static_cast<int>(row), col, item);
    }
  }
  splitter->addWidget(m_table);

  auto* const tev = new QWidget;
  auto* const tev_layout = new QVBoxLayout(tev);
  tev_layout->setContentsMargins(0, 0, 0, 0);
  tev_layout->setSpacing(2);
  m_tev_label = new QLabel(tr("TEV stages per draw"));
  m_tev = new TevHistogram;
  tev_layout->addWidget(m_tev_label);
  tev_layout->addWidget(m_tev, 1);
  splitter->addWidget(tev);

  splitter->setStretchFactor(0, 5);
  splitter->setStretchFactor(1, 4);
  splitter->setStretchFactor(2, 2);
  layout->addWidget(splitter, 1);

  setWidget(contents);

  m_timer = new QTimer(this);
  m_timer->setInterval(250);
  connect(m_timer, &QTimer::timeout, this, &RenderProfilerWidget::Refresh);

  connect(m_record, &QToolButton::toggled, this, &RenderProfilerWidget::UpdateRecording);
  connect(toggleViewAction(), &QAction::toggled, this, &RenderProfilerWidget::UpdateRecording);
  connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
    UpdateRecording();
    if (visible)
    {
      Refresh();
      m_timer->start();
    }
    else
    {
      m_timer->stop();
    }
  });
  connect(clear, &QToolButton::clicked, this, [this] {
    RenderProfiler::ClearHistory();
    m_selected_frame.reset();
    m_frame_graph->SetSelected(-1);
    m_metric_graph->SetSelected(-1);
    Refresh();
  });
  connect(capture, &QToolButton::clicked, this, &RenderProfilerWidget::CaptureFrameRequested);
  connect(fifo, &QToolButton::clicked, this, &RenderProfilerWidget::FIFOPlayerRequested);
  connect(m_metric, &QComboBox::currentIndexChanged, this, &RenderProfilerWidget::Refresh);

  const auto on_select = [this](int index) {
    if (index < 0 || index >= static_cast<int>(m_history.size()))
      m_selected_frame.reset();
    else
      m_selected_frame = m_history[index].frame_number;
    UpdateSelection();
  };
  connect(m_frame_graph, &FrameBarGraph::FrameSelected, this, on_select);
  connect(m_metric_graph, &FrameBarGraph::FrameSelected, this, on_select);

  Refresh();
}

RenderProfilerWidget::~RenderProfilerWidget()
{
  RenderProfiler::SetEnabled(false);
}

void RenderProfilerWidget::UpdateRecording()
{
  // Record while the panel is open (even when it's a background tab), so switching back to it
  // shows recent history.
  RenderProfiler::SetEnabled(m_record->isChecked() && toggleViewAction()->isChecked());
}

void RenderProfilerWidget::Refresh()
{
  UpdateRecording();
  if (m_record->isChecked())
    m_history = RenderProfiler::GetHistory();

  std::vector<double> frame_times;
  frame_times.reserve(m_history.size());
  for (const FrameSample& sample : m_history)
    frame_times.push_back(sample.frame_ms);
  m_frame_graph->SetValues(std::move(frame_times), QStringLiteral("ms"));

  const Metric& metric = METRICS[std::clamp(m_metric->currentData().toInt(), 0,
                                            static_cast<int>(std::size(METRICS)) - 1)];
  std::vector<double> metric_values;
  metric_values.reserve(m_history.size());
  for (const FrameSample& sample : m_history)
    metric_values.push_back(metric.get(sample));
  m_metric_graph->SetValues(std::move(metric_values), QString());

  auto& system = Core::System::GetInstance();
  if (Core::IsUninitialized(system))
  {
    m_live->setText(tr("Start a game to profile it."));
  }
  else
  {
    const PerformanceMetrics& metrics = system.GetPerfMetrics();
    m_live->setText(tr("%1 FPS · %2 VPS · %3% speed")
                        .arg(metrics.GetFPS(), 0, 'f', 1)
                        .arg(metrics.GetVPS(), 0, 'f', 1)
                        .arg(metrics.GetSpeed() * 100.0, 0, 'f', 0));
  }

  UpdateSelection();
}

void RenderProfilerWidget::UpdateSelection()
{
  int selected = -1;
  if (m_selected_frame)
  {
    const auto it = std::find_if(m_history.begin(), m_history.end(), [this](const auto& s) {
      return s.frame_number == *m_selected_frame;
    });
    if (it != m_history.end())
      selected = static_cast<int>(it - m_history.begin());
    else
      m_selected_frame.reset();
  }
  m_frame_graph->SetSelected(selected);
  m_metric_graph->SetSelected(selected);

  const FrameSample* const shown =
      m_history.empty() ? nullptr :
                          &m_history[selected >= 0 ? selected : m_history.size() - 1];

  m_table->horizontalHeaderItem(1)->setText(
      shown ? tr("Frame %1").arg(shown->frame_number) : tr("Frame"));

  for (size_t row = 0; row < std::size(METRICS); ++row)
  {
    const Metric& metric = METRICS[row];
    double sum = 0;
    double peak = 0;
    // The first sample after recording starts has no frame time.
    size_t n = 0;
    for (const FrameSample& sample : m_history)
    {
      if (metric.unit && sample.frame_ms <= 0)
        continue;
      const double v = metric.get(sample);
      sum += v;
      peak = std::max(peak, v);
      ++n;
    }
    const int r = static_cast<int>(row);
    m_table->item(r, 1)->setText(shown ? FormatValue(metric, metric.get(*shown)) : QString());
    m_table->item(r, 2)->setText(n ? FormatValue(metric, sum / n) : QString());
    m_table->item(r, 3)->setText(n ? FormatValue(metric, peak) : QString());
  }

  if (shown)
  {
    m_tev->SetCounts(shown->tev_stage_histogram);
    u32 draws = 0;
    u32 weighted = 0;
    for (size_t i = 0; i < shown->tev_stage_histogram.size(); ++i)
    {
      draws += shown->tev_stage_histogram[i];
      weighted += shown->tev_stage_histogram[i] * static_cast<u32>(i + 1);
    }
    m_tev_label->setText(draws ? tr("TEV stages per draw (avg %1)")
                                     .arg(static_cast<double>(weighted) / draws, 0, 'f', 1) :
                                 tr("TEV stages per draw"));
  }
  else
  {
    m_tev->SetCounts({});
    m_tev_label->setText(tr("TEV stages per draw"));
  }

  const std::vector<RenderProfiler::Hint> hints =
      shown ? RenderProfiler::GetHints(*shown) : std::vector<RenderProfiler::Hint>{};
  m_hints->setVisible(!hints.empty());
  if (!hints.empty())
  {
    const auto color_for = [](const RenderProfiler::Hint& hint) {
      return hint.severity == RenderProfiler::Hint::Severity::Warning ? WarningColor().name() :
                                                                       QStringLiteral("#8ec5ff");
    };
    QString text = QStringLiteral("<span style=\"color:%1\">&#9679;</span> %2")
                       .arg(color_for(hints[0]))
                       .arg(QString::fromStdString(hints[0].text).toHtmlEscaped());
    if (hints.size() > 1)
      text += QStringLiteral(" <i>") + tr("(+%1 more)").arg(hints.size() - 1) +
              QStringLiteral("</i>");
    m_hints->setText(text);

    QString tooltip;
    for (const auto& hint : hints)
    {
      tooltip += QStringLiteral("<p><span style=\"color:%1\">&#9679;</span> %2</p>")
                     .arg(color_for(hint))
                     .arg(QString::fromStdString(hint.text).toHtmlEscaped());
    }
    m_hints->setToolTip(tooltip);
  }
}

// ---------------------------------------------------------------------------------------------
// FrameCaptureWidget

namespace
{
enum CaptureColumn
{
  COL_INDEX,
  COL_EVENT,
  COL_PRIMITIVE,
  COL_VERTICES,
  COL_TEV,
  COL_TEXTURES,
  COL_BLEND,
  COL_DEPTH,
  COL_COUNT
};

constexpr int ROLE_KIND = Qt::UserRole;
constexpr int ROLE_INDEX = Qt::UserRole + 1;
constexpr int KIND_DRAW = 0;
constexpr int KIND_COPY = 1;

QString Yes(bool b)
{
  return b ? QObject::tr("Yes") : QObject::tr("No");
}

QString CopyDestination(const RenderProfiler::CopyRecord& copy)
{
  if (copy.is_xfb)
    return QObject::tr("XFB copy");
  QString kind = copy.is_depth ? QObject::tr("Depth copy") : QObject::tr("EFB copy");
  if (copy.to_ram && copy.to_vram)
    return QObject::tr("%1 → texture + RAM").arg(kind);
  if (copy.to_ram)
    return QObject::tr("%1 → RAM").arg(kind);
  return QObject::tr("%1 → texture").arg(kind);
}

QString CopyFormat(const RenderProfiler::CopyRecord& copy)
{
  if (copy.is_xfb)
    return QObject::tr("YUV 4:2:2 (XFB)");
  return QString::fromStdString(RenderProfiler::GetCopyFormatName(copy.format));
}

QString CsvField(QString s)
{
  if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')))
    s = QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
  return s;
}
}  // namespace

FrameCaptureWidget::FrameCaptureWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Frame Capture"));
  setObjectName(QStringLiteral("framecapture"));
  setAllowedAreas(Qt::AllDockWidgetAreas);

  auto* const contents = new QWidget;
  auto* const layout = new QVBoxLayout(contents);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);

  auto* const toolbar = new QHBoxLayout;
  toolbar->setSpacing(2);
  m_capture = MakeToolButton("crosshairs", tr("Capture Frame"),
                             tr("Capture every draw call and EFB copy of the next frame."));
  m_export = MakeToolButton("file-export", tr("Export CSV"),
                            tr("Save the captured draw calls and copies as a CSV file."));
  m_export->setEnabled(false);
  m_filter = new QLineEdit;
  m_filter->setPlaceholderText(tr("Filter (e.g. copy, Quads, @0020, Blend)"));
  m_filter->setClearButtonEnabled(true);
  m_filter->setMaximumWidth(280);
  m_summary = new QLabel(tr("No capture yet."));
  toolbar->addWidget(m_capture);
  toolbar->addWidget(m_export);
  toolbar->addWidget(m_filter);
  toolbar->addSpacing(8);
  toolbar->addWidget(m_summary, 1);
  layout->addLayout(toolbar);

  auto* const splitter = new QSplitter(Qt::Horizontal);
  splitter->setChildrenCollapsible(false);

  m_tree = new QTreeWidget;
  m_tree->setRootIsDecorated(false);
  m_tree->setUniformRowHeights(true);
  m_tree->setAlternatingRowColors(true);
  m_tree->setColumnCount(COL_COUNT);
  m_tree->setHeaderLabels({tr("#"), tr("Event"), tr("Primitive"), tr("Vertices"), tr("TEV"),
                           tr("Textures"), tr("Blend"), tr("Depth")});
  m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
  m_tree->header()->setStretchLastSection(true);
  m_tree->setColumnWidth(COL_INDEX, 48);
  m_tree->setColumnWidth(COL_EVENT, 170);
  m_tree->setColumnWidth(COL_PRIMITIVE, 110);
  m_tree->setColumnWidth(COL_VERTICES, 70);
  m_tree->setColumnWidth(COL_TEV, 40);
  m_tree->setColumnWidth(COL_TEXTURES, 200);
  m_tree->setColumnWidth(COL_BLEND, 170);
  splitter->addWidget(m_tree);

  m_details = new QTableWidget(0, 2);
  m_details->setHorizontalHeaderLabels({tr("Property"), tr("Value")});
  m_details->verticalHeader()->hide();
  TableExport::Install(m_details);
  m_details->verticalHeader()->setDefaultSectionSize(m_details->fontMetrics().height() + 6);
  m_details->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_details->horizontalHeader()->setStretchLastSection(true);
  m_details->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_details->setWordWrap(false);
  m_details->setShowGrid(false);
  m_details->setAlternatingRowColors(true);
  m_details->setMinimumWidth(220);
  splitter->addWidget(m_details);
  splitter->setStretchFactor(0, 2);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({640, 320});
  layout->addWidget(splitter, 1);

  setWidget(contents);

  m_poll_timer = new QTimer(this);
  m_poll_timer->setInterval(100);
  connect(m_poll_timer, &QTimer::timeout, this, &FrameCaptureWidget::Poll);

  connect(m_capture, &QToolButton::clicked, this, &FrameCaptureWidget::RequestCapture);
  connect(m_export, &QToolButton::clicked, this, &FrameCaptureWidget::ExportCSV);
  connect(m_filter, &QLineEdit::textChanged, this, &FrameCaptureWidget::ApplyFilter);
  m_tree->setToolTip(
      tr("Double-click a copy or textured draw to open its memory in the Memory Viewer."));
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
          &FrameCaptureWidget::ShowContextMenu);
  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
    const auto addresses = AddressesFor(item);
    if (!addresses.empty())
      emit ShowMemoryRequested(addresses.front().second);
  });
  connect(m_tree, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem* current) { ShowDetails(current); });
}

void FrameCaptureWidget::RequestCapture()
{
  RenderProfiler::RequestCapture();
  m_capture->setEnabled(false);
  auto& system = Core::System::GetInstance();
  m_summary->setText(Core::IsUninitialized(system) ?
                         tr("Waiting for a frame. Start a game to capture it.") :
                         tr("Capturing the next frame..."));
  m_poll_timer->start();
}

void FrameCaptureWidget::Poll()
{
  std::optional<RenderProfiler::FrameCapture> capture = RenderProfiler::TakeCapture();
  if (!capture)
    return;

  m_poll_timer->stop();
  m_capture->setEnabled(true);
  m_capture_data = std::move(capture);
  Populate();
}

void FrameCaptureWidget::Populate()
{
  m_tree->clear();
  m_details->setRowCount(0);
  m_export->setEnabled(m_capture_data.has_value());
  if (!m_capture_data)
    return;

  const RenderProfiler::FrameCapture& cap = *m_capture_data;
  u32 culled = 0;
  u32 vertices = 0;
  for (const auto& draw : cap.draws)
  {
    culled += draw.culled;
    vertices += draw.culled ? 0 : draw.num_vertices;
  }
  m_summary->setText(tr("Frame %1: %2 draws (%3 culled), %4 vertices, %5 copies, %6 ms")
                         .arg(cap.frame_number)
                         .arg(cap.draws.size())
                         .arg(culled)
                         .arg(QLocale().toString(vertices))
                         .arg(cap.copies.size())
                         .arg(cap.summary.gpu_thread_ms, 0, 'f', 2));

  const QIcon draw_icon = IconSet::Get("cube");
  const QIcon copy_icon = IconSet::Get("image");
  const QColor copy_color = WarningColor();
  const QColor culled_color = palette().color(QPalette::Disabled, QPalette::Text);

  QList<QTreeWidgetItem*> items;
  items.reserve(static_cast<qsizetype>(cap.draws.size() + cap.copies.size()));
  size_t copy_index = 0;
  const auto add_copies_before = [&](u32 draw_index) {
    while (copy_index < cap.copies.size() && cap.copies[copy_index].after_draw <= draw_index)
    {
      const auto& copy = cap.copies[copy_index];
      auto* const item = new QTreeWidgetItem;
      item->setData(0, ROLE_KIND, KIND_COPY);
      item->setData(0, ROLE_INDEX, static_cast<int>(copy_index));
      item->setIcon(COL_EVENT, copy_icon);
      item->setText(COL_EVENT, CopyDestination(copy));
      item->setText(COL_PRIMITIVE, QStringLiteral("%1x%2").arg(copy.width).arg(copy.height));
      item->setText(COL_TEXTURES,
                    QStringLiteral("%1 @%2")
                        .arg(CopyFormat(copy))
                        .arg(copy.dst_address, 8, 16, QLatin1Char('0')));
      for (int col = 0; col < COL_COUNT; ++col)
        item->setForeground(col, copy_color);
      items.append(item);
      ++copy_index;
    }
  };

  for (const auto& draw : cap.draws)
  {
    add_copies_before(draw.index);
    auto* const item = new QTreeWidgetItem;
    item->setData(0, ROLE_KIND, KIND_DRAW);
    item->setData(0, ROLE_INDEX, static_cast<int>(draw.index));
    item->setText(COL_INDEX, QString::number(draw.index));
    item->setIcon(COL_EVENT, draw_icon);
    item->setText(COL_EVENT, draw.culled ? tr("Draw (culled)") :
                             draw.pipeline_changed ? tr("Draw (new state)") :
                                                     tr("Draw"));
    item->setText(COL_PRIMITIVE, draw.gx_primitives.empty() ?
                                     QString::fromLatin1(
                                         RenderProfiler::GetPrimitiveName(draw.primitive)) :
                                     QString::fromStdString(draw.gx_primitives));
    item->setText(COL_VERTICES, QString::number(draw.num_vertices));
    item->setTextAlignment(COL_VERTICES, Qt::AlignRight | Qt::AlignVCenter);
    item->setText(COL_TEV, QString::number(draw.tev_stages));
    item->setTextAlignment(COL_TEV, Qt::AlignRight | Qt::AlignVCenter);
    item->setText(COL_TEXTURES, QString::fromStdString(draw.textures));
    item->setToolTip(COL_TEXTURES, QString::fromStdString(draw.textures));
    item->setText(COL_BLEND, QString::fromStdString(draw.blend));
    item->setText(COL_DEPTH, QString::fromStdString(draw.depth));
    if (draw.culled)
    {
      for (int col = 0; col < COL_COUNT; ++col)
        item->setForeground(col, culled_color);
    }
    items.append(item);
  }
  add_copies_before(UINT32_MAX);

  m_tree->addTopLevelItems(items);
  ApplyFilter();

  if (!isVisible())
    show();
  raise();
}

void FrameCaptureWidget::ApplyFilter()
{
  const QString filter = m_filter->text().trimmed();
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
  {
    QTreeWidgetItem* const item = m_tree->topLevelItem(i);
    bool match = filter.isEmpty();
    for (int col = 0; !match && col < COL_COUNT; ++col)
      match = item->text(col).contains(filter, Qt::CaseInsensitive);
    item->setHidden(!match);
  }
}

void FrameCaptureWidget::ShowDetails(QTreeWidgetItem* item)
{
  m_details->setRowCount(0);
  if (!item || !m_capture_data)
    return;

  const auto add = [this](const QString& property, const QString& value) {
    const int row = m_details->rowCount();
    m_details->insertRow(row);
    m_details->setItem(row, 0, new QTableWidgetItem(property));
    auto* const value_item = new QTableWidgetItem(value);
    value_item->setToolTip(value);
    m_details->setItem(row, 1, value_item);
  };

  const int index = item->data(0, ROLE_INDEX).toInt();
  if (item->data(0, ROLE_KIND).toInt() == KIND_COPY)
  {
    const auto& copy = m_capture_data->copies[index];
    add(tr("Event"), CopyDestination(copy));
    add(tr("After draw"), QString::number(copy.after_draw));
    add(tr("Destination"), QStringLiteral("0x%1").arg(copy.dst_address, 8, 16, QLatin1Char('0')));
    add(tr("Format"), CopyFormat(copy));
    add(tr("Size"), QStringLiteral("%1 x %2").arg(copy.width).arg(copy.height));
    add(tr("Depth source"), Yes(copy.is_depth));
    add(tr("Half scale"), Yes(copy.half_scale));
    add(tr("Copied to texture"), Yes(copy.to_vram));
    add(tr("Copied to RAM"), Yes(copy.to_ram));
    return;
  }

  const auto& draw = m_capture_data->draws[index];
  add(tr("Draw"), QString::number(draw.index));
  add(tr("GX primitives"), QString::fromStdString(draw.gx_primitives));
  add(tr("GX primitive commands"), QString::number(draw.gx_commands));
  add(tr("Host primitive"), QString::fromLatin1(RenderProfiler::GetPrimitiveName(draw.primitive)));
  add(tr("Vertices"), QString::number(draw.num_vertices));
  add(tr("Indices"), QString::number(draw.num_indices));
  add(tr("Culled"), Yes(draw.culled));
  add(tr("New pipeline"), Yes(draw.pipeline_changed));
  add(tr("Projection"), draw.perspective ? tr("Perspective") : tr("Orthographic"));
  add(tr("TEV stages"), QString::number(draw.tev_stages));
  add(tr("Indirect stages"), QString::number(draw.indirect_stages));
  add(tr("Tex gens"), QString::number(draw.tex_gens));
  add(tr("Color channels"), QString::number(draw.color_channels));
  add(tr("Textures"), QString::fromStdString(draw.textures));
  add(tr("Blend"), QString::fromStdString(draw.blend));
  add(tr("Depth"), QString::fromStdString(draw.depth));
  add(tr("Alpha test"), QString::fromStdString(draw.alpha_test));
  add(tr("Cull mode"), QString::fromStdString(draw.cull));
  add(tr("Fog"), QString::fromStdString(draw.fog));
}

std::vector<std::pair<QString, u32>>
FrameCaptureWidget::AddressesFor(const QTreeWidgetItem* item) const
{
  std::vector<std::pair<QString, u32>> result;
  if (!item || !m_capture_data)
    return result;

  const int index = item->data(0, ROLE_INDEX).toInt();
  if (item->data(0, ROLE_KIND).toInt() == KIND_COPY)
  {
    result.emplace_back(tr("copy destination"), m_capture_data->copies[index].dst_address);
    return result;
  }

  const auto& draw = m_capture_data->draws[index];
  for (size_t i = 0; i < draw.texture_addresses.size(); ++i)
    result.emplace_back(tr("texture %1").arg(i + 1), draw.texture_addresses[i]);
  return result;
}

void FrameCaptureWidget::ShowContextMenu(const QPoint& pos)
{
  QTreeWidgetItem* const item = m_tree->itemAt(pos);
  const auto addresses = AddressesFor(item);

  QMenu menu(this);
  for (const auto& [label, address] : addresses)
  {
    menu.addAction(IconSet::Get("memory"),
                   tr("Show %1 in Memory Viewer (0x%2)")
                       .arg(label)
                       .arg(address, 8, 16, QLatin1Char('0')),
                   this, [this, address] { emit ShowMemoryRequested(address); });
  }
  TableExport::AddActions(&menu, m_tree);
  menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void FrameCaptureWidget::ExportCSV()
{
  if (!m_capture_data)
    return;

  const QString path = DolphinFileDialog::getSaveFileName(
      this, tr("Export Frame Capture"),
      QStringLiteral("frame-%1.csv").arg(m_capture_data->frame_number), tr("CSV files (*.csv)"));
  if (path.isEmpty())
    return;

  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    return;

  QTextStream out(&file);
  out << "event,draw,gx_primitives,host_primitive,vertices,indices,culled,new_pipeline,projection,tev_stages,"
         "indirect_stages,tex_gens,color_channels,textures,blend,depth,alpha_test,cull,fog,"
         "copy_destination,copy_format,copy_width,copy_height,copy_to_ram,copy_to_texture\n";

  size_t copy_index = 0;
  const auto& cap = *m_capture_data;
  const auto write_copies_before = [&](u32 draw_index) {
    while (copy_index < cap.copies.size() && cap.copies[copy_index].after_draw <= draw_index)
    {
      const auto& c = cap.copies[copy_index++];
      out << CsvField(CopyDestination(c)) << ',' << c.after_draw << ",,,,,,,,,,,,,,,,,,"
          << QStringLiteral("0x%1").arg(c.dst_address, 8, 16, QLatin1Char('0')) << ','
          << CsvField(CopyFormat(c)) << ','
          << c.width << ',' << c.height << ',' << int(c.to_ram) << ',' << int(c.to_vram) << '\n';
    }
  };
  for (const auto& d : cap.draws)
  {
    write_copies_before(d.index);
    out << "draw," << d.index << ',' << CsvField(QString::fromStdString(d.gx_primitives)) << ','
        << RenderProfiler::GetPrimitiveName(d.primitive) << ','
        << d.num_vertices << ',' << d.num_indices << ',' << int(d.culled) << ','
        << int(d.pipeline_changed) << ',' << (d.perspective ? "perspective" : "orthographic")
        << ',' << d.tev_stages << ',' << d.indirect_stages << ',' << d.tex_gens << ','
        << d.color_channels << ',' << CsvField(QString::fromStdString(d.textures)) << ','
        << CsvField(QString::fromStdString(d.blend)) << ','
        << CsvField(QString::fromStdString(d.depth)) << ','
        << CsvField(QString::fromStdString(d.alpha_test)) << ','
        << CsvField(QString::fromStdString(d.cull)) << ','
        << CsvField(QString::fromStdString(d.fog)) << ",,,,,,\n";
  }
  write_copies_before(UINT32_MAX);
}
