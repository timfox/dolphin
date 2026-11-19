// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/MemoryUsageWidget.h"

#include <algorithm>
#include <cmath>

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "DolphinQt/IconSet.h"
#include "UICommon/UICommon.h"

namespace
{
QString Hex32(u32 value)
{
  return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

QString FormatBytes(u64 bytes)
{
  return QString::fromStdString(UICommon::FormatSize(bytes));
}

const QColor IN_USE_COLOR(0x5f, 0xe3, 0xff);
const QColor HEAP_COLOR = GetMemoryRegionKindColor(MemoryRegionKind::Heap);
const QColor WRITE_COLOR(0xf2, 0xb3, 0x3d);
const QColor WARNING_COLOR(0xff, 0x6b, 0x6b);

double WriteRate(const std::deque<MemoryUsageSample>& history, size_t i, int space)
{
  if (i == 0)
    return 0;
  const double dt = history[i].time - history[i - 1].time;
  return dt > 0 ? history[i].written[space] / dt : 0;
}

// Rounds |value| up to a 1/2/5 step so axis labels are tidy.
double NiceCeiling(double value)
{
  if (value <= 0)
    return 1;
  const double magnitude = std::pow(10.0, std::floor(std::log10(value)));
  for (double step : {1.0, 2.0, 5.0, 10.0})
  {
    if (value <= step * magnitude)
      return step * magnitude;
  }
  return 10 * magnitude;
}

QTableWidget* CreateTable(const QStringList& headers)
{
  auto* const table = new QTableWidget(0, headers.size());
  table->setHorizontalHeaderLabels(headers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->hide();
  table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 8);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setWordWrap(false);
  table->setAlternatingRowColors(true);
  table->setShowGrid(false);
  return table;
}

void SetCell(QTableWidget* table, int row, int column, const QString& text, bool right = false)
{
  QTableWidgetItem* item = table->item(row, column);
  if (!item)
  {
    item = new QTableWidgetItem;
    if (right)
      item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->setItem(row, column, item);
  }
  if (item->text() != text)
    item->setText(text);
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// MemoryUsageGraph

MemoryUsageGraph::MemoryUsageGraph(MemoryAnalyzer* analyzer, QWidget* parent)
    : QWidget(parent), m_analyzer(analyzer)
{
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  connect(m_analyzer, &MemoryAnalyzer::Updated, this, qOverload<>(&QWidget::update));
}

QSize MemoryUsageGraph::sizeHint() const
{
  return QSize(600, 180);
}

QSize MemoryUsageGraph::minimumSizeHint() const
{
  return QSize(240, 110);
}

void MemoryUsageGraph::SetSpace(MemorySpace space)
{
  m_space = space;
  update();
}

void MemoryUsageGraph::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QColor background = palette().color(QPalette::Base);
  const QColor grid = palette().color(QPalette::Mid);
  const QColor text = palette().color(QPalette::PlaceholderText);
  painter.fillRect(rect(), background);

  QFont font = this->font();
  font.setPointSizeF(font.pointSizeF() * 0.85);
  painter.setFont(font);
  const QFontMetrics metrics(font);

  const QRectF plot =
      QRectF(rect()).adjusted(64, metrics.height() + 12, -64, -metrics.height() - 8);
  if (plot.width() < 20 || plot.height() < 20)
    return;

  const auto& history = m_analyzer->GetHistory();
  const int space = static_cast<int>(m_space);
  const double capacity = std::max<double>(m_analyzer->GetSpaceSize(m_space), 1);
  const double window =
      MemoryAnalyzer::HISTORY_LENGTH * MemoryAnalyzer::SAMPLE_INTERVAL_MS / 1000.0;
  // "Now" is always the right edge; a short history fills in from the right.
  const double end_time = history.empty() ? 0 : history.back().time;
  const double start_time = end_time - window;

  double max_rate = 0;
  for (size_t i = 0; i < history.size(); ++i)
    max_rate = std::max(max_rate, WriteRate(history, i, space));
  const double rate_scale = NiceCeiling(std::max(max_rate, 64.0 * 1024));

  const auto x_for = [&](double time) {
    return plot.left() + (time - start_time) / window * plot.width();
  };
  const auto y_for = [&](double bytes) { return plot.bottom() - bytes / capacity * plot.height(); };

  // Grid and axes.
  painter.setPen(QPen(grid, 1, Qt::DotLine));
  for (int i = 0; i <= 4; ++i)
  {
    const double y = plot.bottom() - plot.height() * i / 4;
    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    painter.setPen(text);
    painter.drawText(QRectF(0, y - metrics.height() / 2.0, plot.left() - 6, metrics.height()),
                     Qt::AlignRight | Qt::AlignVCenter, FormatBytes(u64(capacity * i / 4)));
    painter.drawText(
        QRectF(plot.right() + 6, y - metrics.height() / 2.0, 64, metrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        tr("%1/s").arg(FormatBytes(u64(rate_scale * i / 4))));
    painter.setPen(QPen(grid, 1, Qt::DotLine));
  }
  painter.setPen(text);
  for (int seconds = 0; seconds <= static_cast<int>(window); seconds += 30)
  {
    const double x = plot.right() - seconds / window * plot.width();
    painter.drawText(QRectF(x - 40, plot.bottom() + 4, 80, metrics.height()), Qt::AlignCenter,
                     seconds == 0 ? tr("now") : tr("-%1 s").arg(seconds));
  }

  if (history.empty())
    return;

  painter.save();
  painter.setClipRect(plot.adjusted(-2, -2, 2, 2));

  // Write traffic as bars along the bottom (right axis).
  const double bar_width = std::max(1.0, plot.width() / MemoryAnalyzer::HISTORY_LENGTH - 1);
  QColor bar_color = WRITE_COLOR;
  bar_color.setAlphaF(0.55);
  for (size_t i = 1; i < history.size(); ++i)
  {
    const double rate = WriteRate(history, i, space);
    if (rate <= 0)
      continue;
    const double height = std::min(rate / rate_scale, 1.0) * plot.height();
    painter.fillRect(QRectF(x_for(history[i].time) - bar_width, plot.bottom() - height, bar_width,
                            height),
                     bar_color);
  }

  // In-use line with a soft fill underneath.
  QPainterPath line;
  for (size_t i = 0; i < history.size(); ++i)
  {
    const QPointF point(x_for(history[i].time), y_for(double(history[i].in_use[space])));
    if (i == 0)
      line.moveTo(point);
    else
      line.lineTo(point);
  }
  QPainterPath fill = line;
  fill.lineTo(x_for(history.back().time), plot.bottom());
  fill.lineTo(x_for(history.front().time), plot.bottom());
  fill.closeSubpath();
  QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
  QColor top = IN_USE_COLOR;
  top.setAlphaF(0.35);
  QColor bottom = IN_USE_COLOR;
  bottom.setAlphaF(0.03);
  gradient.setColorAt(0, top);
  gradient.setColorAt(1, bottom);
  painter.fillPath(fill, gradient);
  painter.setPen(QPen(IN_USE_COLOR, 2));
  painter.drawPath(line);

  // Heap allocations (MEM1 graph only: heap totals aren't split by space).
  const bool show_heap = m_space == MemorySpace::MEM1 && history.back().heap_allocated;
  if (show_heap)
  {
    QPainterPath heap;
    bool started = false;
    for (const MemoryUsageSample& sample : history)
    {
      if (!sample.heap_allocated)
        continue;
      const QPointF point(x_for(sample.time), y_for(double(*sample.heap_allocated)));
      if (!started)
        heap.moveTo(point);
      else
        heap.lineTo(point);
      started = true;
    }
    painter.setPen(QPen(HEAP_COLOR, 2));
    painter.drawPath(heap);
  }
  painter.restore();

  // Legend.
  painter.setRenderHint(QPainter::Antialiasing, false);
  double x = plot.left();
  const auto legend_item = [&](const QColor& color, const QString& label) {
    painter.fillRect(QRectF(x, 6 + metrics.height() / 2.0 - 2, 14, 4), color);
    x += 18;
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(QPointF(x, 6 + metrics.ascent()), label);
    x += metrics.horizontalAdvance(label) + 16;
  };
  legend_item(IN_USE_COLOR, tr("In use (non-zero pages)"));
  if (show_heap)
    legend_item(HEAP_COLOR, tr("Heap allocated"));
  legend_item(WRITE_COLOR, tr("Written per second"));
}

// ---------------------------------------------------------------------------------------------
// MemoryUsageWidget

MemoryUsageWidget::MemoryUsageWidget(MemoryAnalyzer* analyzer, QWidget* parent)
    : QDockWidget(parent), m_analyzer(analyzer)
{
  setWindowTitle(tr("Memory Usage"));
  setObjectName(QStringLiteral("memoryusage"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  auto* const contents = new QWidget;
  auto* const layout = new QVBoxLayout(contents);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);

  auto* const stats = new QHBoxLayout;
  stats->setSpacing(6);
  stats->addWidget(CreateStat(tr("In use"), &m_in_use_value, &m_in_use_detail));
  stats->addWidget(CreateStat(tr("Program"), &m_program_value, &m_program_detail));
  stats->addWidget(CreateStat(tr("Heap"), &m_heap_value, &m_heap_detail));
  stats->addWidget(CreateStat(tr("Stacks"), &m_stack_value, &m_stack_detail));
  stats->addWidget(CreateStat(tr("Writes"), &m_write_value, &m_write_detail));
  m_space_combo = new QComboBox;
  m_space_combo->setToolTip(tr("Memory to graph"));
  stats->addWidget(m_space_combo, 0, Qt::AlignTop);
  layout->addLayout(stats);

  m_graph = new MemoryUsageGraph(m_analyzer);

  m_stack_table = CreateTable({tr("Thread"), tr("Stack"), tr("Size"), tr("Peak use"),
                               tr("Saved SP"), tr("Status")});
  m_heap_table = CreateTable(
      {tr("Heap"), tr("Range"), tr("Size"), tr("Allocated"), tr("Free"), tr("Blocks")});
  m_heap_hint = new QLabel(
      tr("Heaps are read from the OSAlloc symbols (HeapArray, NumHeaps). Load the game's symbol "
         "map, or build with symbols, to see them."));
  m_heap_hint->setWordWrap(true);
  m_heap_hint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
  auto* const heap_panel = new QWidget;
  auto* const heap_layout = new QVBoxLayout(heap_panel);
  heap_layout->setContentsMargins(0, 0, 0, 0);
  heap_layout->addWidget(m_heap_hint);
  heap_layout->addWidget(m_heap_table, 1);

  auto* const tabs = new QTabWidget;
  tabs->setDocumentMode(true);
  tabs->addTab(m_stack_table, IconSet::Get("list-ul"), tr("Thread Stacks"));
  tabs->addTab(heap_panel, IconSet::Get("layer-group"), tr("Heaps"));

  auto* const splitter = new QSplitter(Qt::Horizontal);
  splitter->addWidget(m_graph);
  splitter->addWidget(tabs);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setChildrenCollapsible(false);
  layout->addWidget(splitter, 1);

  m_placeholder = new QLabel(tr("Start a game to graph its memory use."));
  m_placeholder->setAlignment(Qt::AlignCenter);
  m_placeholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

  m_stack = new QStackedWidget;
  m_stack->addWidget(m_placeholder);
  m_stack->addWidget(contents);
  setWidget(m_stack);

  connect(m_analyzer, &MemoryAnalyzer::Updated, this, &MemoryUsageWidget::OnUpdated);
  connect(m_space_combo, &QComboBox::currentIndexChanged, this, [this] {
    m_graph->SetSpace(CurrentSpace());
    UpdateStats();
  });
  connect(m_stack_table, &QTableWidget::cellDoubleClicked, this, [this](int row) {
    const auto& stacks = m_analyzer->GetStacks();
    if (row >= 0 && row < static_cast<int>(stacks.size()))
      emit ShowMemory(stacks[row].high - stacks[row].peak, false);
  });
  connect(m_heap_table, &QTableWidget::cellDoubleClicked, this, [this](int row) {
    const auto& heaps = m_analyzer->GetHeaps();
    if (row >= 0 && row < static_cast<int>(heaps.size()))
      emit ShowMemory(heaps[row].start, false);
  });

  UpdateSpaces();
}

QWidget* MemoryUsageWidget::CreateStat(const QString& title, QLabel** value, QLabel** detail)
{
  auto* const frame = new QFrame;
  frame->setObjectName(QStringLiteral("memoryStat"));
  frame->setFrameShape(QFrame::StyledPanel);
  frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* const layout = new QVBoxLayout(frame);
  layout->setContentsMargins(8, 4, 8, 4);
  layout->setSpacing(0);

  auto* const title_label = new QLabel(title.toUpper());
  QFont title_font = title_label->font();
  title_font.setPointSizeF(title_font.pointSizeF() * 0.8);
  title_font.setBold(true);
  title_label->setFont(title_font);
  title_label->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

  *value = new QLabel(QStringLiteral("–"));
  QFont value_font = (*value)->font();
  value_font.setPointSizeF(value_font.pointSizeF() * 1.25);
  value_font.setBold(true);
  (*value)->setFont(value_font);

  *detail = new QLabel;
  QFont detail_font = (*detail)->font();
  detail_font.setPointSizeF(detail_font.pointSizeF() * 0.85);
  (*detail)->setFont(detail_font);
  (*detail)->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

  layout->addWidget(title_label);
  layout->addWidget(*value);
  layout->addWidget(*detail);
  return frame;
}

void MemoryUsageWidget::showEvent(QShowEvent* event)
{
  QDockWidget::showEvent(event);
  m_analyzer->AddViewer(this);
}

void MemoryUsageWidget::hideEvent(QHideEvent* event)
{
  QDockWidget::hideEvent(event);
  m_analyzer->RemoveViewer(this);
}

MemorySpace MemoryUsageWidget::CurrentSpace() const
{
  const QVariant space_data = m_space_combo->currentData();
  return space_data.isValid() ? static_cast<MemorySpace>(space_data.toInt()) : MemorySpace::MEM1;
}

void MemoryUsageWidget::UpdateSpaces()
{
  const MemorySpace current = CurrentSpace();
  std::vector<MemorySpace> spaces;
  for (MemorySpace space : {MemorySpace::MEM1, MemorySpace::MEM2, MemorySpace::ARAM})
  {
    if (space == MemorySpace::MEM1 || m_analyzer->IsSpaceAvailable(space))
      spaces.push_back(space);
  }
  bool same = static_cast<int>(spaces.size()) == m_space_combo->count();
  for (size_t i = 0; same && i < spaces.size(); ++i)
    same = m_space_combo->itemData(static_cast<int>(i)).toInt() == static_cast<int>(spaces[i]);
  if (same)
    return;

  const QSignalBlocker blocker(m_space_combo);
  m_space_combo->clear();
  for (MemorySpace space : spaces)
  {
    m_space_combo->addItem(IconSet::Get("memory"), MemoryAnalyzer::GetSpaceName(space),
                           static_cast<int>(space));
  }
  m_space_combo->setCurrentIndex(std::max(m_space_combo->findData(static_cast<int>(current)), 0));
  m_graph->SetSpace(CurrentSpace());
}

void MemoryUsageWidget::OnUpdated()
{
  if (!m_analyzer->HasData())
  {
    m_stack->setCurrentWidget(m_placeholder);
    m_stack_table->setRowCount(0);
    m_heap_table->setRowCount(0);
    return;
  }
  m_stack->setCurrentIndex(1);
  UpdateSpaces();
  UpdateStats();
  UpdateStacks();
  UpdateHeaps();
}

void MemoryUsageWidget::UpdateStats()
{
  const auto& history = m_analyzer->GetHistory();
  if (history.empty())
    return;
  const MemoryUsageSample& latest = history.back();
  const MemorySpace space = CurrentSpace();
  const int index = static_cast<int>(space);
  const u32 capacity = m_analyzer->GetSpaceSize(space);

  m_in_use_value->setText(FormatBytes(latest.in_use[index]));
  m_in_use_detail->setText(tr("of %1 %2 (%3%)")
                               .arg(FormatBytes(capacity), MemoryAnalyzer::GetSpaceName(space))
                               .arg(capacity ? latest.in_use[index] * 100.0 / capacity : 0.0, 0,
                                    'f', 1));

  u64 code = 0, data_size = 0, bss = 0;
  for (const MemoryRegion& region : m_analyzer->GetRegions(MemorySpace::MEM1))
  {
    if (region.kind == MemoryRegionKind::Text)
      code += region.Size();
    else if (region.kind == MemoryRegionKind::Data)
      data_size += region.Size();
    else if (region.kind == MemoryRegionKind::Bss)
      bss += region.Size();
  }
  if (code + data_size + bss)
  {
    m_program_value->setText(FormatBytes(code + data_size + bss));
    m_program_detail->setText(
        tr("code %1 · data %2 · bss %3")
            .arg(FormatBytes(code), FormatBytes(data_size), FormatBytes(bss)));
  }
  else
  {
    m_program_value->setText(QStringLiteral("–"));
    m_program_detail->setText(tr("no section info"));
  }

  if (latest.heap_allocated && latest.heap_size)
  {
    int blocks = 0;
    for (const HeapInfo& heap : m_analyzer->GetHeaps())
      blocks += heap.allocated_blocks;
    m_heap_value->setText(FormatBytes(*latest.heap_allocated));
    m_heap_detail->setText(tr("of %1 in %n heap(s) · %2 blocks", "",
                              static_cast<int>(m_analyzer->GetHeaps().size()))
                               .arg(FormatBytes(*latest.heap_size))
                               .arg(blocks));
  }
  else
  {
    m_heap_value->setText(QStringLiteral("–"));
    m_heap_detail->setText(tr("needs symbols"));
  }

  const auto& stacks = m_analyzer->GetStacks();
  m_stack_value->setText(tr("%n thread(s)", "", static_cast<int>(stacks.size())));
  const ThreadStackInfo* deepest = nullptr;
  bool overflow = false;
  for (const ThreadStackInfo& stack : stacks)
  {
    overflow |= !stack.magic_ok;
    const double ratio = double(stack.peak) / std::max(1u, stack.high - stack.low);
    if (!deepest || ratio > double(deepest->peak) / std::max(1u, deepest->high - deepest->low))
      deepest = &stack;
  }
  if (overflow)
  {
    m_stack_detail->setText(tr("stack overflow detected"));
    m_stack_detail->setStyleSheet(QStringLiteral("color: %1;").arg(WARNING_COLOR.name()));
  }
  else
  {
    m_stack_detail->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    if (deepest)
    {
      m_stack_detail->setText(tr("deepest %1 of %2")
                                  .arg(FormatBytes(deepest->peak),
                                       FormatBytes(deepest->high - deepest->low)));
    }
    else
    {
      m_stack_detail->setText(tr("no OS threads yet"));
    }
  }

  double rate = 0;
  if (history.size() >= 2)
  {
    const MemoryUsageSample& previous = history[history.size() - 2];
    const double dt = latest.time - previous.time;
    if (dt > 0)
      rate = latest.written[index] / dt;
  }
  m_write_value->setText(tr("%1/s").arg(FormatBytes(u64(rate))));
  const int pages_changed = static_cast<int>(latest.written[index] / MemoryAnalyzer::PAGE_SIZE);
  m_write_detail->setText(tr("%n page(s) changed", "", pages_changed));
}

void MemoryUsageWidget::UpdateStacks()
{
  const auto& stacks = m_analyzer->GetStacks();
  const bool resize = m_stack_table->rowCount() != static_cast<int>(stacks.size());
  m_stack_table->setRowCount(static_cast<int>(stacks.size()));
  for (int row = 0; row < static_cast<int>(stacks.size()); ++row)
  {
    const ThreadStackInfo& stack = stacks[row];
    const u32 size = stack.high - stack.low;
    SetCell(m_stack_table, row, 0,
            Hex32(stack.thread) + (stack.current ? tr(" (current)") : QString()));
    SetCell(m_stack_table, row, 1,
            QStringLiteral("%1–%2").arg(Hex32(stack.low), Hex32(stack.high - 1)));
    SetCell(m_stack_table, row, 2, FormatBytes(size), true);
    SetCell(m_stack_table, row, 3,
            QStringLiteral("%1 (%2%)")
                .arg(FormatBytes(stack.peak))
                .arg(size ? stack.peak * 100.0 / size : 0.0, 0, 'f', 0),
            true);
    SetCell(m_stack_table, row, 4, Hex32(stack.sp));
    QString status = tr("OK");
    if (!stack.magic_ok)
      status = tr("Overflow: end marker overwritten");
    else if (size && stack.peak * 10 >= size * 9)
      status = tr("Nearly full");
    SetCell(m_stack_table, row, 5, status);
    m_stack_table->item(row, 5)->setForeground(
        stack.magic_ok && status == tr("OK") ? palette().color(QPalette::Text) : WARNING_COLOR);
  }
  if (resize)
    m_stack_table->resizeColumnsToContents();
}

void MemoryUsageWidget::UpdateHeaps()
{
  m_heap_hint->setVisible(!m_analyzer->HasHeapSymbols());
  const auto& heaps = m_analyzer->GetHeaps();
  const bool resize = m_heap_table->rowCount() != static_cast<int>(heaps.size());
  m_heap_table->setRowCount(static_cast<int>(heaps.size()));
  for (int row = 0; row < static_cast<int>(heaps.size()); ++row)
  {
    const HeapInfo& heap = heaps[row];
    const u32 size = heap.end - heap.start;
    SetCell(m_heap_table, row, 0,
            tr("Heap %1").arg(heap.index) + (heap.current ? tr(" (current)") : QString()));
    SetCell(m_heap_table, row, 1,
            QStringLiteral("%1–%2").arg(Hex32(heap.start), Hex32(heap.end - 1)));
    SetCell(m_heap_table, row, 2, FormatBytes(size), true);
    SetCell(m_heap_table, row, 3,
            QStringLiteral("%1 (%2%)")
                .arg(FormatBytes(heap.allocated))
                .arg(size ? heap.allocated * 100.0 / size : 0.0, 0, 'f', 0),
            true);
    SetCell(m_heap_table, row, 4, FormatBytes(heap.free), true);
    SetCell(m_heap_table, row, 5,
            tr("%1 used / %2 free").arg(heap.allocated_blocks).arg(heap.free_blocks));
  }
  if (resize)
    m_heap_table->resizeColumnsToContents();
}
