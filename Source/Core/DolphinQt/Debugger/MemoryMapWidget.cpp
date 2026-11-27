// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/MemoryMapWidget.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolTip>
#include <QVBoxLayout>

#include "DolphinQt/IconSet.h"
#include "DolphinQt/QtUtils/TableExport.h"
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

QColor Mix(const QColor& a, const QColor& b, double t)
{
  t = std::clamp(t, 0.0, 1.0);
  return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                          a.greenF() + (b.greenF() - a.greenF()) * t,
                          a.blueF() + (b.blueF() - a.blueF()) * t);
}

QIcon Swatch(const QColor& color)
{
  QPixmap pixmap(12, 12);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawRoundedRect(QRectF(0.5, 0.5, 11, 11), 3, 3);
  return QIcon(pixmap);
}

constexpr double WRITE_GLOW_SECONDS = 2.5;
constexpr int LABEL_WIDTH = 74;

enum Column
{
  ColumnRegion,
  ColumnType,
  ColumnStart,
  ColumnEnd,
  ColumnSize,
  ColumnUsed,
  ColumnNotes,
  ColumnCount
};
}  // namespace

// ---------------------------------------------------------------------------------------------
// MemoryPageMapView

MemoryPageMapView::MemoryPageMapView(MemoryAnalyzer* analyzer, QWidget* parent)
    : QWidget(parent), m_analyzer(analyzer)
{
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  connect(m_analyzer, &MemoryAnalyzer::Updated, this, qOverload<>(&QWidget::update));
}

QSize MemoryPageMapView::sizeHint() const
{
  return QSize(LABEL_WIDTH + 128 * 5, 48 * 5);
}

QSize MemoryPageMapView::minimumSizeHint() const
{
  return QSize(LABEL_WIDTH + 128, 96);
}

void MemoryPageMapView::SetSpace(MemorySpace space)
{
  m_space = space;
  m_highlight.reset();
  m_hover_page.reset();
  update();
}

void MemoryPageMapView::SetHighlight(std::optional<std::pair<u32, u32>> range)
{
  m_highlight = range;
  update();
}

MemoryPageMapView::Geometry MemoryPageMapView::ComputeGeometry() const
{
  Geometry geometry;
  const size_t pages = m_analyzer->GetPageCount(m_space);
  if (pages == 0)
    return geometry;

  geometry.rows = static_cast<int>((pages + geometry.columns - 1) / geometry.columns);
  const QRectF area = QRectF(rect()).adjusted(LABEL_WIDTH, 4, -4, -4);
  geometry.cell_width = area.width() / geometry.columns;
  // Keep cells from getting much taller than they are wide.
  geometry.cell_height = std::min(area.height() / geometry.rows, geometry.cell_width * 1.6);
  geometry.grid = QRectF(area.left(), area.top(), geometry.cell_width * geometry.columns,
                         geometry.cell_height * geometry.rows);
  return geometry;
}

std::optional<size_t> MemoryPageMapView::PageAt(const QPoint& pos) const
{
  const Geometry geometry = ComputeGeometry();
  if (geometry.rows == 0 || !geometry.grid.contains(pos))
    return std::nullopt;
  const int column = static_cast<int>((pos.x() - geometry.grid.left()) / geometry.cell_width);
  const int row = static_cast<int>((pos.y() - geometry.grid.top()) / geometry.cell_height);
  const size_t page = static_cast<size_t>(row) * geometry.columns + column;
  if (column < 0 || column >= geometry.columns || page >= m_analyzer->GetPageCount(m_space))
    return std::nullopt;
  return page;
}

void MemoryPageMapView::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  const QColor background = palette().color(QPalette::Base);
  painter.fillRect(rect(), background);

  const Geometry geometry = ComputeGeometry();
  if (geometry.rows == 0)
    return;

  const u32 base = m_analyzer->GetSpaceBase(m_space);
  const size_t pages = m_analyzer->GetPageCount(m_space);
  const double gap = geometry.cell_width >= 5 ? 1.0 : 0.0;
  const QColor glow(0xff, 0xff, 0xff);

  for (size_t page = 0; page < pages; ++page)
  {
    const int row = static_cast<int>(page / geometry.columns);
    const int column = static_cast<int>(page % geometry.columns);
    const QRectF cell(geometry.grid.left() + column * geometry.cell_width,
                      geometry.grid.top() + row * geometry.cell_height,
                      geometry.cell_width - gap, geometry.cell_height - gap);

    QColor color = GetMemoryRegionKindColor(m_analyzer->GetPageKind(m_space, page));
    if (!m_analyzer->IsPageInUse(m_space, page))
      color = Mix(background, color, 0.28);
    const double age = m_analyzer->GetPageWriteAge(m_space, page);
    if (age >= 0 && age < WRITE_GLOW_SECONDS)
      color = Mix(color, glow, 0.8 * (1.0 - age / WRITE_GLOW_SECONDS));
    if (m_highlight)
    {
      const u32 address = base + static_cast<u32>(page) * MemoryAnalyzer::PAGE_SIZE;
      const bool inside = address + MemoryAnalyzer::PAGE_SIZE > m_highlight->first &&
                          address < m_highlight->second;
      if (!inside)
        color = Mix(background, color, 0.35);
    }
    painter.fillRect(cell, color);
  }

  if (m_hover_page)
  {
    const int row = static_cast<int>(*m_hover_page / geometry.columns);
    const int column = static_cast<int>(*m_hover_page % geometry.columns);
    painter.setPen(QPen(palette().color(QPalette::BrightText), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(geometry.grid.left() + column * geometry.cell_width,
                            geometry.grid.top() + row * geometry.cell_height,
                            geometry.cell_width, geometry.cell_height)
                         .adjusted(-1, -1, 0, 0));
  }

  // Row address labels, spaced so they don't overlap.
  QFont font = this->font();
  font.setPointSizeF(font.pointSizeF() * 0.85);
  font.setStyleHint(QFont::Monospace);
  font.setFamily(QStringLiteral("monospace"));
  painter.setFont(font);
  painter.setPen(palette().color(QPalette::PlaceholderText));
  const int label_height = QFontMetrics(font).height();
  const int step = std::max(1, static_cast<int>(std::ceil(label_height * 1.6 /
                                                          std::max(geometry.cell_height, 1.0))));
  for (int row = 0; row < geometry.rows; row += step)
  {
    const u32 address = base + static_cast<u32>(row) * geometry.columns * MemoryAnalyzer::PAGE_SIZE;
    const QRectF label(0, geometry.grid.top() + row * geometry.cell_height - 1, LABEL_WIDTH - 8,
                       label_height);
    painter.drawText(label, Qt::AlignRight | Qt::AlignTop, Hex32(address));
  }
}

void MemoryPageMapView::mouseMoveEvent(QMouseEvent* event)
{
  const std::optional<size_t> page = PageAt(event->position().toPoint());
  if (page != m_hover_page)
  {
    m_hover_page = page;
    update();
  }
  if (!page)
  {
    QToolTip::hideText();
    return;
  }

  const u32 address =
      m_analyzer->GetSpaceBase(m_space) + static_cast<u32>(*page) * MemoryAnalyzer::PAGE_SIZE;
  const MemoryRegion* const region = m_analyzer->FindRegion(m_space, address);
  const double age = m_analyzer->GetPageWriteAge(m_space, *page);
  QString activity;
  if (!m_analyzer->IsPageInUse(m_space, *page))
    activity = tr("All zero");
  else if (age < 0)
    activity = tr("In use, unchanged since sampling began");
  else
    activity = tr("In use, last written %1 s ago").arg(age, 0, 'f', 1);

  QString text = QStringLiteral("<b>%1 – %2</b><br>")
                     .arg(Hex32(address), Hex32(address + MemoryAnalyzer::PAGE_SIZE - 1));
  if (region)
  {
    text += QStringLiteral("%1 <span style='color:gray'>(%2)</span><br>")
                .arg(region->name.toHtmlEscaped(),
                     GetMemoryRegionKindName(m_analyzer->GetPageKind(m_space, *page)));
  }
  text += activity;
  QToolTip::showText(event->globalPosition().toPoint(), text, this);
}

void MemoryPageMapView::mousePressEvent(QMouseEvent* event)
{
  if (event->button() != Qt::LeftButton)
    return;
  if (const std::optional<size_t> page = PageAt(event->position().toPoint()))
  {
    emit AddressClicked(m_analyzer->GetSpaceBase(m_space) +
                        static_cast<u32>(*page) * MemoryAnalyzer::PAGE_SIZE);
  }
}

void MemoryPageMapView::mouseDoubleClickEvent(QMouseEvent* event)
{
  if (event->button() != Qt::LeftButton)
    return;
  if (const std::optional<size_t> page = PageAt(event->position().toPoint()))
  {
    emit AddressActivated(m_analyzer->GetSpaceBase(m_space) +
                          static_cast<u32>(*page) * MemoryAnalyzer::PAGE_SIZE);
  }
}

void MemoryPageMapView::leaveEvent(QEvent*)
{
  m_hover_page.reset();
  update();
}

// ---------------------------------------------------------------------------------------------
// MemoryMapWidget

MemoryMapWidget::MemoryMapWidget(MemoryAnalyzer* analyzer, QWidget* parent)
    : QDockWidget(parent), m_analyzer(analyzer)
{
  setWindowTitle(tr("Memory Map"));
  setObjectName(QStringLiteral("memorymap"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  auto* const contents = new QWidget;
  auto* const layout = new QVBoxLayout(contents);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);

  auto* const header = new QHBoxLayout;
  m_space_combo = new QComboBox;
  m_space_combo->setToolTip(tr("Memory to show"));
  m_summary = new QLabel;
  m_summary->setTextFormat(Qt::RichText);
  m_source = new QLabel;
  m_source->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
  header->addWidget(m_space_combo);
  header->addWidget(m_summary, 1);
  header->addWidget(m_source);
  layout->addLayout(header);

  m_map = new MemoryPageMapView(m_analyzer);

  auto* const legend = new QHBoxLayout;
  legend->setSpacing(12);
  for (MemoryRegionKind kind :
       {MemoryRegionKind::System, MemoryRegionKind::Text, MemoryRegionKind::Data,
        MemoryRegionKind::Bss, MemoryRegionKind::Heap, MemoryRegionKind::HeapFree,
        MemoryRegionKind::Stack, MemoryRegionKind::Arena, MemoryRegionKind::Reserved})
  {
    auto* const item = new QLabel;
    item->setText(QStringLiteral("<span style='color:%1'>■</span> %2")
                      .arg(GetMemoryRegionKindColor(kind).name(), GetMemoryRegionKindName(kind)));
    legend->addWidget(item);
  }
  auto* const hint = new QLabel(tr("Dim: all zero · White: written in the last few seconds"));
  hint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
  legend->addStretch(1);
  legend->addWidget(hint);

  auto* const map_panel = new QWidget;
  auto* const map_layout = new QVBoxLayout(map_panel);
  map_layout->setContentsMargins(0, 0, 0, 0);
  map_layout->addWidget(m_map, 1);
  map_layout->addLayout(legend);

  m_table = new QTableWidget(0, ColumnCount);
  m_table->setHorizontalHeaderLabels(
      {tr("Region"), tr("Type"), tr("Start"), tr("End"), tr("Size"), tr("Used"), tr("Notes")});
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::SingleSelection);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setContextMenuPolicy(Qt::CustomContextMenu);
  m_table->verticalHeader()->hide();
  m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 8);
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->setWordWrap(false);
  m_table->setAlternatingRowColors(true);
  m_table->setShowGrid(false);

  m_splitter = new QSplitter(Qt::Vertical);
  m_splitter->addWidget(map_panel);
  m_splitter->addWidget(m_table);
  m_splitter->setStretchFactor(0, 3);
  m_splitter->setStretchFactor(1, 2);
  m_splitter->setChildrenCollapsible(false);
  layout->addWidget(m_splitter, 1);

  m_placeholder = new QLabel(tr("Start a game to see its memory map.\n\nThe map shows MEM1 (and "
                                "MEM2 or ARAM) page by page: OS low memory, the program's "
                                "sections, the arena and heaps, and thread stacks."));
  m_placeholder->setAlignment(Qt::AlignCenter);
  m_placeholder->setWordWrap(true);
  m_placeholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

  m_stack = new QStackedWidget;
  m_stack->addWidget(m_placeholder);
  m_stack->addWidget(contents);
  setWidget(m_stack);

  connect(m_analyzer, &MemoryAnalyzer::Updated, this, &MemoryMapWidget::OnUpdated);
  connect(m_space_combo, &QComboBox::currentIndexChanged, this, [this] {
    m_map->SetSpace(CurrentSpace());
    m_row_ranges.clear();
    OnUpdated();
  });
  connect(m_table, &QTableWidget::itemSelectionChanged, this,
          &MemoryMapWidget::OnSelectionChanged);
  connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row) {
    if (const MemoryRegion* region = RegionForRow(row))
      emit ShowMemory(region->start, CurrentSpace() == MemorySpace::ARAM);
  });
  connect(m_table, &QTableWidget::customContextMenuRequested, this,
          &MemoryMapWidget::ShowContextMenu);
  connect(m_map, &MemoryPageMapView::AddressClicked, this, &MemoryMapWidget::SelectRegionAt);
  connect(m_map, &MemoryPageMapView::AddressActivated, this,
          [this](u32 address) { emit ShowMemory(address, CurrentSpace() == MemorySpace::ARAM); });

  UpdateSpaces();
}

void MemoryMapWidget::showEvent(QShowEvent* event)
{
  QDockWidget::showEvent(event);
  m_analyzer->AddViewer(this);
}

void MemoryMapWidget::hideEvent(QHideEvent* event)
{
  QDockWidget::hideEvent(event);
  m_analyzer->RemoveViewer(this);
}

void MemoryMapWidget::resizeEvent(QResizeEvent* event)
{
  QDockWidget::resizeEvent(event);
  // Wide, short panels (e.g. tabbed with the game view) put the region table beside the map.
  const Qt::Orientation orientation =
      width() > height() * 2 ? Qt::Horizontal : Qt::Vertical;
  if (m_splitter->orientation() != orientation)
  {
    m_splitter->setOrientation(orientation);
    // Sizes are relative: the splitter scales them to its real size.
    m_splitter->setSizes({600, 400});
  }
}

MemorySpace MemoryMapWidget::CurrentSpace() const
{
  const QVariant space_data = m_space_combo->currentData();
  return space_data.isValid() ? static_cast<MemorySpace>(space_data.toInt()) : MemorySpace::MEM1;
}

void MemoryMapWidget::UpdateSpaces()
{
  const MemorySpace current = CurrentSpace();
  std::vector<MemorySpace> spaces;
  for (MemorySpace space : {MemorySpace::MEM1, MemorySpace::MEM2, MemorySpace::ARAM})
  {
    if (m_analyzer->IsSpaceAvailable(space) || (space == MemorySpace::MEM1))
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
  const int index = m_space_combo->findData(static_cast<int>(current));
  m_space_combo->setCurrentIndex(std::max(index, 0));
  m_map->SetSpace(CurrentSpace());
  m_row_ranges.clear();
}

void MemoryMapWidget::OnUpdated()
{
  if (!m_analyzer->HasData())
  {
    m_stack->setCurrentWidget(m_placeholder);
    m_table->setRowCount(0);
    m_row_ranges.clear();
    return;
  }
  m_stack->setCurrentIndex(1);
  UpdateSpaces();
  UpdateSummary();
  UpdateTable();
}

void MemoryMapWidget::UpdateSummary()
{
  const MemorySpace space = CurrentSpace();
  const u32 size = m_analyzer->GetSpaceSize(space);
  const auto& history = m_analyzer->GetHistory();
  const u64 in_use = history.empty() ? 0 : history.back().in_use[static_cast<int>(space)];

  QString text = tr("<b>%1</b> in use of %2 (%3%)")
                     .arg(FormatBytes(in_use), FormatBytes(size))
                     .arg(size ? in_use * 100.0 / size : 0.0, 0, 'f', 1);

  if (space != MemorySpace::ARAM)
  {
    u64 program = 0;
    for (const MemoryRegion& region : m_analyzer->GetRegions(space))
    {
      if (region.nested)
        continue;
      if (region.kind == MemoryRegionKind::Text || region.kind == MemoryRegionKind::Data ||
          region.kind == MemoryRegionKind::Bss)
      {
        program += region.Size();
      }
    }
    if (program)
      text += tr(" · program %1").arg(FormatBytes(program));
    for (const MemoryRegion& region : m_analyzer->GetRegions(space))
    {
      if (region.kind == MemoryRegionKind::Arena && !region.nested)
        text += tr(" · arena %1").arg(FormatBytes(region.Size()));
    }
  }
  m_summary->setText(text);

  m_source->setVisible(space != MemorySpace::ARAM);
  const auto& executable = m_analyzer->GetExecutableLayout();
  if (executable)
    m_source->setText(tr("Sections: %1").arg(executable->source));
  else
    m_source->setText(tr("No executable section info"));
  m_source->setToolTip(executable ? QString() :
                                    tr("Section info is read from the DOL/ELF or the disc's "
                                       "main DOL when a game boots."));
}

void MemoryMapWidget::UpdateTable()
{
  const MemorySpace space = CurrentSpace();
  const auto& regions = m_analyzer->GetRegions(space);

  std::vector<std::pair<u32, u32>> ranges;
  ranges.reserve(regions.size());
  for (const MemoryRegion& region : regions)
    ranges.emplace_back(region.start, region.end);

  const auto set_text = [this](int row, int column, const QString& text) {
    QTableWidgetItem* item = m_table->item(row, column);
    if (!item)
    {
      item = new QTableWidgetItem;
      if (column >= ColumnStart && column <= ColumnUsed)
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      m_table->setItem(row, column, item);
    }
    if (item->text() != text)
      item->setText(text);
    return item;
  };

  const bool rebuild = ranges != m_row_ranges;
  std::optional<std::pair<u32, u32>> selected;
  if (rebuild)
  {
    const auto selection = m_table->selectionModel()->selectedRows();
    if (!selection.isEmpty() && static_cast<size_t>(selection.first().row()) < m_row_ranges.size())
      selected = m_row_ranges[selection.first().row()];
  }

  const QSignalBlocker blocker(m_table);
  if (rebuild)
    m_table->setRowCount(static_cast<int>(regions.size()));

  QFont mono = m_table->font();
  mono.setFamily(QStringLiteral("monospace"));
  mono.setStyleHint(QFont::Monospace);

  for (int row = 0; row < static_cast<int>(regions.size()); ++row)
  {
    const MemoryRegion& region = regions[row];
    QTableWidgetItem* const name = set_text(row, ColumnRegion, region.name);
    if (rebuild)
      name->setIcon(Swatch(GetMemoryRegionKindColor(region.kind)));
    set_text(row, ColumnType, GetMemoryRegionKindName(region.kind));
    set_text(row, ColumnStart, Hex32(region.start))->setFont(mono);
    set_text(row, ColumnEnd, Hex32(region.end - 1))->setFont(mono);
    set_text(row, ColumnSize, FormatBytes(region.Size()));
    QString used;
    if (region.used && region.Size())
    {
      used = QStringLiteral("%1 (%2%)")
                 .arg(FormatBytes(*region.used))
                 .arg(*region.used * 100.0 / region.Size(), 0, 'f', 0);
    }
    set_text(row, ColumnUsed, used);
    QTableWidgetItem* const notes = set_text(row, ColumnNotes, region.note);
    notes->setToolTip(region.note);
  }

  if (rebuild)
  {
    m_row_ranges = std::move(ranges);
    m_table->resizeColumnsToContents();
    m_table->clearSelection();
    if (selected)
    {
      const auto it = std::find(m_row_ranges.begin(), m_row_ranges.end(), *selected);
      if (it != m_row_ranges.end())
        m_table->selectRow(static_cast<int>(it - m_row_ranges.begin()));
    }
    OnSelectionChanged();
  }
}

const MemoryRegion* MemoryMapWidget::RegionForRow(int row) const
{
  const auto& regions = m_analyzer->GetRegions(CurrentSpace());
  if (row < 0 || row >= static_cast<int>(regions.size()))
    return nullptr;
  return &regions[row];
}

void MemoryMapWidget::OnSelectionChanged()
{
  const auto selection = m_table->selectionModel()->selectedRows();
  const MemoryRegion* const region =
      selection.isEmpty() ? nullptr : RegionForRow(selection.first().row());
  if (region)
    m_map->SetHighlight(std::make_pair(region->start, region->end));
  else
    m_map->SetHighlight(std::nullopt);
}

void MemoryMapWidget::SelectRegionAt(u32 address)
{
  const MemoryRegion* const region = m_analyzer->FindRegion(CurrentSpace(), address);
  if (!region)
  {
    m_table->clearSelection();
    return;
  }
  const auto it = std::find(m_row_ranges.begin(), m_row_ranges.end(),
                            std::make_pair(region->start, region->end));
  if (it == m_row_ranges.end())
    return;
  const int row = static_cast<int>(it - m_row_ranges.begin());
  if (!m_table->selectionModel()->isRowSelected(row, {}))
    m_table->selectRow(row);
  else
    m_table->clearSelection();
  m_table->scrollToItem(m_table->item(row, 0));
}

void MemoryMapWidget::ShowContextMenu(const QPoint& pos)
{
  const int row = m_table->rowAt(pos.y());
  const MemoryRegion* const region = RegionForRow(row);
  QMenu menu(this);
  if (!region)
  {
    TableExport::AddActions(&menu, m_table);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
    return;
  }

  const bool aram = CurrentSpace() == MemorySpace::ARAM;
  const u32 start = region->start;
  const u32 end = region->end;

  menu.addAction(IconSet::Get("memory"), tr("Show in Memory Viewer"), this,
                 [this, start, aram] { emit ShowMemory(start, aram); });
  menu.addAction(IconSet::Get("location-dot"), tr("Show End in Memory Viewer"), this,
                 [this, end, aram] { emit ShowMemory(end - 1, aram); });
  menu.addSeparator();
  menu.addAction(tr("Copy Start Address"), this,
                 [start] { QApplication::clipboard()->setText(Hex32(start)); });
  menu.addAction(tr("Copy Range"), this, [start, end] {
    QApplication::clipboard()->setText(QStringLiteral("%1-%2").arg(Hex32(start), Hex32(end - 1)));
  });
  if (!aram)
  {
    menu.addSeparator();
    menu.addAction(IconSet::Get("circle-dot"), tr("Break on Access to This Region"), this,
                   [this, start, end] { emit RequestRangedBreakpoint(start, end - 1); });
  }
  TableExport::AddActions(&menu, m_table);
  menu.exec(m_table->viewport()->mapToGlobal(pos));
}
