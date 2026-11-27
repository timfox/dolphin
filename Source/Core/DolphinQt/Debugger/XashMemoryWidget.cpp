// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/XashMemoryWidget.h"

#include <algorithm>
#include <map>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolTip>
#include <QVBoxLayout>

#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
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

QString FormatDelta(s64 bytes)
{
  if (bytes == 0)
    return QStringLiteral("0");
  const QString size = FormatBytes(static_cast<u64>(bytes < 0 ? -bytes : bytes));
  return bytes < 0 ? QStringLiteral("−%1").arg(size) : QStringLiteral("+%1").arg(size);
}

QString FormatDuration(double seconds)
{
  const int total = static_cast<int>(seconds);
  if (total < 60)
    return QObject::tr("%1 s").arg(total);
  if (total < 3600)
    return QObject::tr("%1 min %2 s").arg(total / 60).arg(total % 60);
  return QObject::tr("%1 h %2 min").arg(total / 3600).arg((total / 60) % 60);
}

QColor CategoryColor(Xash::PoolCategory category)
{
  switch (category)
  {
  case Xash::PoolCategory::Model:
    return QColor(0x5f, 0xb3, 0xff);
  case Xash::PoolCategory::Texture:
    return QColor(0xb3, 0x7f, 0xff);
  case Xash::PoolCategory::Sound:
    return QColor(0x4c, 0xd9, 0x9a);
  case Xash::PoolCategory::Render:
    return QColor(0xff, 0x9f, 0x43);
  case Xash::PoolCategory::Server:
    return QColor(0xff, 0x6b, 0x8b);
  case Xash::PoolCategory::Client:
    return QColor(0xf2, 0xd2, 0x3d);
  case Xash::PoolCategory::FileSystem:
    return QColor(0x3d, 0xd2, 0xe0);
  case Xash::PoolCategory::Engine:
    break;
  }
  return QColor(0x9a, 0xa4, 0xb0);
}

const QColor GROW_COLOR(0xff, 0x6b, 0x6b);
const QColor SHRINK_COLOR(0x4c, 0xd9, 0x9a);

class NumericItem final : public QTableWidgetItem
{
public:
  NumericItem(const QString& text, double value) : QTableWidgetItem(text)
  {
    setData(Qt::UserRole + 1, value);
    setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  }
  bool operator<(const QTableWidgetItem& other) const override
  {
    return data(Qt::UserRole + 1).toDouble() < other.data(Qt::UserRole + 1).toDouble();
  }
};

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
  TableExport::Install(table);
  return table;
}

// Refills a sortable table while keeping the sort order and the selected row (by its key in
// column 0's UserRole).
class TableFill
{
public:
  explicit TableFill(QTableWidget* table) : m_table(table)
  {
    if (const QTableWidgetItem* item = table->currentItem())
    {
      if (const QTableWidgetItem* key = table->item(item->row(), 0))
        m_selected = key->data(Qt::UserRole);
    }
    m_sorting = table->isSortingEnabled();
    table->setSortingEnabled(false);
    table->setUpdatesEnabled(false);
  }
  ~TableFill()
  {
    m_table->setSortingEnabled(m_sorting);
    if (m_selected.isValid())
    {
      for (int row = 0; row < m_table->rowCount(); ++row)
      {
        const QTableWidgetItem* key = m_table->item(row, 0);
        if (key && key->data(Qt::UserRole) == m_selected)
        {
          const QSignalBlocker blocker(m_table);
          m_table->setCurrentCell(row, 0);
          break;
        }
      }
    }
    m_table->setUpdatesEnabled(true);
  }

private:
  QTableWidget* m_table;
  QVariant m_selected;
  bool m_sorting = false;
};

QTableWidgetItem* TextItem(const QString& text, const QVariant& key = {})
{
  auto* const item = new QTableWidgetItem(text);
  if (key.isValid())
    item->setData(Qt::UserRole, key);
  return item;
}

QTableWidgetItem* DeltaItem(s64 delta)
{
  auto* const item = new NumericItem(FormatDelta(delta), static_cast<double>(delta));
  if (delta > 0)
    item->setForeground(GROW_COLOR);
  else if (delta < 0)
    item->setForeground(SHRINK_COLOR);
  return item;
}

QString ShortSource(const QString& path)
{
  // __FILE__ is often a long relative path ("../engine/common/model.c"); keep the useful tail.
  const QStringList parts = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'))
                                .split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (parts.size() <= 2)
    return path;
  return parts.mid(parts.size() - 2).join(QLatin1Char('/'));
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// XashPoolCategoryBar

XashPoolCategoryBar::XashPoolCategoryBar(QWidget* parent) : QWidget(parent)
{
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setMouseTracking(true);
}

void XashPoolCategoryBar::SetPools(const std::vector<Xash::Pool>& pools)
{
  std::map<Xash::PoolCategory, u64> totals;
  m_total = 0;
  for (const Xash::Pool& pool : pools)
  {
    totals[pool.category] += pool.real;
    m_total += pool.real;
  }
  m_totals.assign(totals.begin(), totals.end());
  std::sort(m_totals.begin(), m_totals.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  QStringList lines;
  for (const auto& [category, bytes] : m_totals)
    lines << tr("%1: %2").arg(Xash::GetPoolCategoryName(category), FormatBytes(bytes));
  setToolTip(lines.join(QLatin1Char('\n')));
  update();
}

QSize XashPoolCategoryBar::sizeHint() const
{
  return {400, fontMetrics().height() * 2 + 12};
}

QSize XashPoolCategoryBar::minimumSizeHint() const
{
  return {120, fontMetrics().height() * 2 + 12};
}

void XashPoolCategoryBar::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const int bar_height = 10;
  const QRectF bar(0.5, 0.5, width() - 1.0, bar_height);
  painter.setPen(Qt::NoPen);
  painter.setBrush(palette().color(QPalette::AlternateBase));
  painter.drawRoundedRect(bar, 3, 3);
  if (m_total == 0)
    return;

  double x = bar.left();
  painter.save();
  QPainterPath clip;
  clip.addRoundedRect(bar, 3, 3);
  painter.setClipPath(clip);
  for (const auto& [category, bytes] : m_totals)
  {
    const double w = bar.width() * double(bytes) / double(m_total);
    painter.setBrush(CategoryColor(category));
    painter.drawRect(QRectF(x, bar.top(), w, bar.height()));
    x += w;
  }
  painter.restore();

  // Legend: the categories in size order, as many as fit.
  const QFontMetrics metrics = fontMetrics();
  int lx = 0;
  const int ly = bar_height + 6;
  painter.setPen(palette().color(QPalette::Text));
  for (const auto& [category, bytes] : m_totals)
  {
    const QString text =
        QStringLiteral("%1 %2").arg(Xash::GetPoolCategoryName(category), FormatBytes(bytes));
    const int w = 14 + metrics.horizontalAdvance(text) + 14;
    if (lx + w > width() && lx != 0)
      break;
    painter.setBrush(CategoryColor(category));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(lx, ly + (metrics.height() - 9) / 2.0, 9, 9), 2, 2);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRect(lx + 14, ly, w, metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                     text);
    lx += w;
  }
}

// ---------------------------------------------------------------------------------------------
// XashMemoryWidget

XashMemoryWidget::XashMemoryWidget(MemoryAnalyzer* analyzer, QWidget* parent)
    : QDockWidget(parent), m_analyzer(analyzer)
{
  setWindowTitle(tr("Xash3D Memory"));
  setObjectName(QStringLiteral("xashmemory"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  // Symbol bar, shared by the "not detected" page and the main page.
  m_symbols_label = new QLabel;
  m_symbols_label->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
  m_symbols_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_load_button = new QPushButton(IconSet::Get("folder-open"), tr("Load Symbols..."));
  m_load_button->setToolTip(
      tr("Load the engine's symbols from its unstripped ELF. xash3d-gc strips OUT/bin/xash before "
         "making the DOL; the unstripped one is build/engine/xash."));
  m_unload_button = new QPushButton(IconSet::Get("xmark"), tr("Unload"));
  m_mark_button = new QPushButton(IconSet::Get("location-dot"), tr("Mark"));
  m_mark_button->setToolTip(
      tr("Remember every pool's size now. The pool table then shows what grew or shrank since: "
         "mark before a changelevel, come back to the same map, and anything still bigger is a "
         "leak candidate."));
  m_clear_mark_button = new QPushButton(IconSet::Get("xmark"), tr("Clear Mark"));

  auto* const bar = new QHBoxLayout;
  bar->addWidget(m_symbols_label, 1);
  bar->addWidget(m_load_button);
  bar->addWidget(m_unload_button);
  bar->addSpacing(12);
  bar->addWidget(m_mark_button);
  bar->addWidget(m_clear_mark_button);

  m_problem = new QLabel;
  m_problem->setWordWrap(true);
  m_problem->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
  m_problem->hide();

  auto* const stats = new QHBoxLayout;
  stats->setSpacing(6);
  stats->addWidget(CreateStat(tr("Zone pools"), &m_zone_value, &m_zone_detail));
  stats->addWidget(CreateStat(tr("C heap"), &m_heap_value, &m_heap_detail));
  stats->addWidget(CreateStat(tr("Map-load arena"), &m_arena_value, &m_arena_detail));
  stats->addWidget(CreateStat(tr("ARAM"), &m_aram_value, &m_aram_detail));
  stats->addWidget(CreateStat(tr("Map"), &m_map_value, &m_map_detail));

  // Pools
  m_category_bar = new XashPoolCategoryBar;
  m_pool_table = CreateTable({tr("Pool"), tr("Category"), tr("Size"), tr("Since mark"),
                              tr("Payload"), tr("Blocks"), tr("Created at")});
  m_pool_table->setSortingEnabled(true);
  m_pool_table->sortByColumn(2, Qt::DescendingOrder);
  m_pool_table->setToolTip(
      tr("Size is what the pool holds in the C heap, headers included; Payload is what callers "
         "asked for. Double-click a pool to see where its allocations come from."));
  auto* const pool_panel = new QWidget;
  auto* const pool_layout = new QVBoxLayout(pool_panel);
  pool_layout->setContentsMargins(0, 6, 0, 0);
  pool_layout->addWidget(m_category_bar);
  pool_layout->addWidget(m_pool_table, 1);

  // Allocation sites
  m_site_pool = new QComboBox;
  m_site_pool->setMinimumContentsLength(24);
  m_site_pool->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  auto* const refresh = new QPushButton(IconSet::Get("arrows-rotate"), tr("Refresh"));
  m_site_summary = new QLabel;
  m_site_summary->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
  m_site_table = CreateTable(
      {tr("Source"), tr("Allocations"), tr("Bytes"), tr("Largest"), tr("Pools")});
  m_site_table->setSortingEnabled(true);
  m_site_table->sortByColumn(2, Qt::DescendingOrder);
  auto* const site_bar = new QHBoxLayout;
  site_bar->addWidget(new QLabel(tr("Pool:")));
  site_bar->addWidget(m_site_pool);
  site_bar->addWidget(refresh);
  site_bar->addWidget(m_site_summary, 1);
  auto* const site_panel = new QWidget;
  auto* const site_layout = new QVBoxLayout(site_panel);
  site_layout->setContentsMargins(0, 6, 0, 0);
  site_layout->addLayout(site_bar);
  site_layout->addWidget(m_site_table, 1);

  // ARAM
  m_aram_summary = new QLabel;
  m_aram_summary->setWordWrap(true);
  m_aram_summary->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
  m_aram_table =
      CreateTable({tr("Offset"), tr("End"), tr("Size"), tr("Owner"), tr("Class")});
  m_asset_table = CreateTable(
      {tr("Cached asset"), tr("Size"), tr("ARAM"), tr("Pins"), tr("State"), tr("Last use")});
  m_asset_table->setSortingEnabled(true);
  m_asset_table->sortByColumn(1, Qt::DescendingOrder);
  auto* const aram_split = new QSplitter(Qt::Horizontal);
  aram_split->addWidget(m_aram_table);
  aram_split->addWidget(m_asset_table);
  aram_split->setChildrenCollapsible(false);
  auto* const aram_panel = new QWidget;
  auto* const aram_layout = new QVBoxLayout(aram_panel);
  aram_layout->setContentsMargins(0, 6, 0, 0);
  aram_layout->addWidget(m_aram_summary);
  aram_layout->addWidget(aram_split, 1);

  // Maps
  m_map_table = CreateTable({tr("Map"), tr("Entered"), tr("Time"), tr("Zone at entry"),
                             tr("Zone now"), tr("Change"), tr("Zone peak"), tr("C heap"),
                             tr("ARAM")});
  m_map_table->horizontalHeaderItem(5)->setToolTip(
      tr("Zone memory now compared with the last visit to the same map, or with the map before "
         "it on a first visit. Growth on a revisit is the usual sign of a leak."));
  m_map_detail_label = new QLabel;
  m_map_detail_label->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
  m_map_detail_table =
      CreateTable({tr("Pool"), tr("Previous map"), tr("This map"), tr("Change")});
  m_map_detail_table->setSortingEnabled(true);
  m_map_detail_table->sortByColumn(3, Qt::DescendingOrder);
  auto* const detail_panel = new QWidget;
  auto* const detail_layout = new QVBoxLayout(detail_panel);
  detail_layout->setContentsMargins(0, 0, 0, 0);
  detail_layout->addWidget(m_map_detail_label);
  detail_layout->addWidget(m_map_detail_table, 1);
  auto* const map_split = new QSplitter(Qt::Vertical);
  map_split->addWidget(m_map_table);
  map_split->addWidget(detail_panel);
  map_split->setChildrenCollapsible(false);
  auto* const map_panel = new QWidget;
  auto* const map_layout = new QVBoxLayout(map_panel);
  map_layout->setContentsMargins(0, 6, 0, 0);
  map_layout->addWidget(map_split, 1);

  m_tabs = new QTabWidget;
  m_tabs->setDocumentMode(true);
  m_tabs->addTab(pool_panel, IconSet::Get("layer-group"), tr("Zone Pools"));
  m_tabs->addTab(site_panel, IconSet::Get("file-code"), tr("By Source Line"));
  m_tabs->addTab(aram_panel, IconSet::Get("box-archive"), tr("ARAM"));
  m_tabs->addTab(map_panel, IconSet::Get("map"), tr("Per Map"));

  auto* const contents = new QWidget;
  auto* const layout = new QVBoxLayout(contents);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);
  layout->addLayout(stats);
  layout->addWidget(m_problem);
  layout->addWidget(m_tabs, 1);

  m_placeholder = new QLabel(tr("Start a Xash3D game to see the engine's memory."));
  m_placeholder->setAlignment(Qt::AlignCenter);
  m_placeholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

  m_not_detected = new QLabel(
      tr("This program doesn't look like Xash3D, or its symbols aren't loaded.\n\n"
         "The engine's memory is found through its own symbols (poolchain, gc_aram_heap...). "
         "Booting an unstripped ELF works as is. For a disc or a stripped DOL, load the "
         "unstripped ELF it was built from: in xash3d-gc that's build/engine/xash."));
  m_not_detected->setAlignment(Qt::AlignCenter);
  m_not_detected->setWordWrap(true);
  m_not_detected->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

  m_stack = new QStackedWidget;
  m_stack->addWidget(m_placeholder);
  m_stack->addWidget(m_not_detected);
  m_stack->addWidget(contents);

  auto* const outer = new QWidget;
  auto* const outer_layout = new QVBoxLayout(outer);
  outer_layout->setContentsMargins(6, 6, 6, 0);
  outer_layout->setSpacing(0);
  outer_layout->addLayout(bar);
  outer_layout->addWidget(m_stack, 1);
  setWidget(outer);

  connect(m_analyzer, &MemoryAnalyzer::Updated, this, &XashMemoryWidget::OnUpdated);
  connect(m_load_button, &QPushButton::clicked, this, &XashMemoryWidget::LoadSymbols);
  connect(m_unload_button, &QPushButton::clicked, this,
          [this] { m_analyzer->UnloadEngineSymbols(); });
  connect(m_mark_button, &QPushButton::clicked, m_analyzer, &MemoryAnalyzer::SetMark);
  connect(m_clear_mark_button, &QPushButton::clicked, m_analyzer, &MemoryAnalyzer::ClearMark);
  connect(refresh, &QPushButton::clicked, this, [this] { UpdateSites(true); });
  connect(m_site_pool, &QComboBox::currentIndexChanged, this, [this] { UpdateSites(true); });
  connect(m_tabs, &QTabWidget::currentChanged, this, [this] { OnUpdated(); });
  connect(m_pool_table, &QTableWidget::cellDoubleClicked, this, [this](int row) {
    const QTableWidgetItem* item = m_pool_table->item(row, 0);
    if (!item)
      return;
    const int index = m_site_pool->findData(item->data(Qt::UserRole));
    if (index >= 0)
      m_site_pool->setCurrentIndex(index);
    m_tabs->setCurrentIndex(1);
    UpdateSites(true);
  });
  connect(m_aram_table, &QTableWidget::cellDoubleClicked, this, [this](int row) {
    if (const QTableWidgetItem* item = m_aram_table->item(row, 0))
      emit ShowMemory(item->data(Qt::UserRole).toUInt(), true);
  });
  connect(m_asset_table, &QTableWidget::cellDoubleClicked, this, [this](int row) {
    if (const QTableWidgetItem* item = m_asset_table->item(row, 2))
      emit ShowMemory(item->data(Qt::UserRole + 2).toUInt(), true);
  });
  connect(m_map_table, &QTableWidget::itemSelectionChanged, this,
          &XashMemoryWidget::UpdateMapDetail);

  OnUpdated();
}

QWidget* XashMemoryWidget::CreateStat(const QString& title, QLabel** value, QLabel** detail)
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

void XashMemoryWidget::showEvent(QShowEvent* event)
{
  QDockWidget::showEvent(event);
  m_analyzer->AddViewer(this);
}

void XashMemoryWidget::hideEvent(QHideEvent* event)
{
  QDockWidget::hideEvent(event);
  m_analyzer->RemoveViewer(this);
}

void XashMemoryWidget::LoadSymbols()
{
  QString start = m_analyzer->GetEngineSymbolsPath();
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Load Engine Symbols"), start,
      tr("Unstripped ELF (*.elf xash xash-*);;All files (*)"));
  if (path.isEmpty())
    return;
  const QString error = m_analyzer->LoadEngineSymbols(path);
  if (!error.isEmpty())
    ModalMessageBox::warning(this, tr("Load Engine Symbols"), error);
  OnUpdated();
}

void XashMemoryWidget::OnUpdated()
{
  const QString status = m_analyzer->GetEngineSymbolsStatus();
  m_symbols_label->setText(status.isEmpty() ? tr("Symbols: Dolphin's symbol map") :
                                              tr("Symbols: %1").arg(status));
  m_unload_button->setVisible(!m_analyzer->GetEngineSymbolsPath().isEmpty());
  m_clear_mark_button->setEnabled(m_analyzer->GetMark().has_value());

  if (!m_analyzer->HasData())
  {
    m_stack->setCurrentWidget(m_placeholder);
    m_mark_button->setEnabled(false);
    return;
  }
  const auto& xash = m_analyzer->GetXash();
  if (!xash)
  {
    m_stack->setCurrentWidget(m_not_detected);
    m_mark_button->setEnabled(false);
    return;
  }
  m_stack->setCurrentWidget(m_stack->widget(2));
  m_mark_button->setEnabled(true);
  m_problem->setVisible(!xash->problem.isEmpty());
  m_problem->setText(xash->problem);

  if (!isVisible())
    return;
  UpdateStats();
  UpdateSiteCombo();
  switch (m_tabs->currentIndex())
  {
  case 0:
    UpdatePools();
    break;
  case 1:
    UpdateSites(false);
    break;
  case 2:
    UpdateAram();
    break;
  case 3:
    UpdateMaps();
    break;
  default:
    break;
  }
}

void XashMemoryWidget::UpdateStats()
{
  const Xash::Snapshot& xash = *m_analyzer->GetXash();
  const auto& mark = m_analyzer->GetMark();

  m_zone_value->setText(FormatBytes(xash.zone_real));
  QString zone_detail = tr("%1 pools, payload %2").arg(xash.pools.size()).arg(
      FormatBytes(xash.zone_payload));
  if (mark && mark->zone_allocated)
  {
    zone_detail += QStringLiteral(" · ") +
                   tr("%1 since mark")
                       .arg(FormatDelta(s64(xash.zone_real) - s64(*mark->zone_allocated)));
  }
  else if (xash.hwm)
  {
    zone_detail += QStringLiteral(" · ") + tr("peak %1").arg(FormatBytes(*xash.hwm));
  }
  m_zone_detail->setText(zone_detail);

  if (xash.heap)
  {
    const Xash::CHeap& heap = *xash.heap;
    m_heap_value->setText(FormatBytes(heap.used));
    m_heap_detail->setText(tr("%1 free inside, %2 unclaimed; largest free %3")
                               .arg(FormatBytes(heap.free), FormatBytes(heap.Unclaimed()),
                                    FormatBytes(heap.largest_free)));
    m_heap_value->setToolTip(
        tr("%1, %2 to %3. Every zone pool, and every direct malloc (game DLL code, XFBs, BSP "
           "pins), lives here.")
            .arg(heap.allocator, Hex32(heap.start), Hex32(heap.end)));
  }
  else
  {
    m_heap_value->setText(QStringLiteral("–"));
    m_heap_detail->setText(tr("Allocator symbols not found"));
  }

  if (xash.map_arena)
  {
    const Xash::MapArena& arena = *xash.map_arena;
    m_arena_value->setText(FormatBytes(arena.size));
    m_arena_detail->setText(tr("%1; tail %2, peak %3")
                                .arg(arena.in_use ? tr("BSP borrowed") : tr("idle"),
                                     FormatBytes(arena.tail_highwater),
                                     FormatBytes(arena.tail_peak)));
  }
  else
  {
    m_arena_value->setText(QStringLiteral("–"));
    m_arena_detail->setText(tr("Not allocated yet"));
  }

  if (xash.aram)
  {
    const Xash::Aram& aram = *xash.aram;
    const u64 used = u64(aram.session_bytes) + aram.stream_bytes;
    m_aram_value->setText(tr("%1 / %2").arg(FormatBytes(used), FormatBytes(aram.end - aram.base)));
    QString detail = tr("largest free %1").arg(FormatBytes(aram.LargestFree()));
    if (aram.failures)
      detail += QStringLiteral(" · ") + tr("%n refused", nullptr, int(aram.failures));
    m_aram_detail->setText(detail);
  }
  else
  {
    m_aram_value->setText(QStringLiteral("–"));
    m_aram_detail->setText(tr("ARAM manager not started"));
  }

  m_map_value->setText(xash.map.isEmpty() ? tr("(none)") : xash.map);
  const auto& visits = m_analyzer->GetXashMapVisits();
  m_map_detail->setText(visits.empty() ?
                            QString() :
                            tr("%1 on this map, %n visit(s)", nullptr, int(visits.size()))
                                .arg(FormatDuration(visits.back().last_seen -
                                                    visits.back().entered)));
}

void XashMemoryWidget::UpdatePools()
{
  const Xash::Snapshot& xash = *m_analyzer->GetXash();
  const auto& mark_pools = m_analyzer->GetXashMarkPools();
  const bool has_mark = m_analyzer->GetMark().has_value();
  auto& reader = m_analyzer->GetXashReader();
  auto& system = Core::System::GetInstance();

  m_category_bar->SetPools(xash.pools);

  // Mark deltas are per pool name; split a name's delta over its pools so duplicates add up.
  std::unordered_map<QString, u64> current;
  for (const Xash::Pool& pool : xash.pools)
    current[pool.name] += pool.real;

  {
    TableFill fill(m_pool_table);
    m_pool_table->setRowCount(static_cast<int>(xash.pools.size()));
    int row = 0;
    std::unordered_map<QString, bool> delta_shown;
    for (const Xash::Pool& pool : xash.pools)
    {
      auto* const name = TextItem(pool.name, pool.index);
      name->setToolTip(tr("mempool_t at %1").arg(Hex32(pool.address)));
      m_pool_table->setItem(row, 0, name);
      auto* const category = TextItem(Xash::GetPoolCategoryName(pool.category));
      category->setForeground(CategoryColor(pool.category));
      m_pool_table->setItem(row, 1, category);
      m_pool_table->setItem(row, 2, new NumericItem(FormatBytes(pool.real), pool.real));
      if (has_mark && !delta_shown[pool.name])
      {
        delta_shown[pool.name] = true;
        const auto it = mark_pools.find(pool.name);
        const s64 before = it == mark_pools.end() ? 0 : s64(it->second);
        auto* const delta = DeltaItem(s64(current[pool.name]) - before);
        if (it == mark_pools.end())
          delta->setToolTip(tr("Created since the mark"));
        m_pool_table->setItem(row, 3, delta);
      }
      else
      {
        m_pool_table->setItem(row, 3, new NumericItem(QString(), 0));
      }
      m_pool_table->setItem(row, 4, new NumericItem(FormatBytes(pool.payload), pool.payload));
      m_pool_table->setItem(
          row, 5,
          new NumericItem(pool.blocks_complete ? QString::number(pool.blocks) :
                                                 QStringLiteral("%1+").arg(pool.blocks),
                          pool.blocks));
      const QString file = reader.ReadCString(system, pool.created_file);
      m_pool_table->setItem(row, 6,
                            TextItem(QStringLiteral("%1:%2").arg(ShortSource(file))
                                         .arg(pool.created_line)));
      ++row;
    }
  }

  if (!m_pools_sized && !xash.pools.empty())
  {
    m_pools_sized = true;
    m_pool_table->resizeColumnsToContents();
    m_pool_table->setColumnWidth(0, std::max(m_pool_table->columnWidth(0), 180));
  }
}

void XashMemoryWidget::UpdateSiteCombo()
{
  const Xash::Snapshot& xash = *m_analyzer->GetXash();
  QList<QPair<QString, int>> entries;
  entries.append({tr("All pools"), -1});
  std::vector<const Xash::Pool*> sorted;
  for (const Xash::Pool& pool : xash.pools)
    sorted.push_back(&pool);
  std::sort(sorted.begin(), sorted.end(), [](const Xash::Pool* a, const Xash::Pool* b) {
    return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
  });
  for (const Xash::Pool* pool : sorted)
    entries.append({pool->name, pool->index});

  bool same = entries.size() == m_site_pool->count();
  for (int i = 0; same && i < entries.size(); ++i)
  {
    same = m_site_pool->itemData(i).toInt() == entries[i].second &&
           m_site_pool->itemText(i) == entries[i].first;
  }
  if (same)
    return;

  const QVariant current = m_site_pool->currentData();
  const QSignalBlocker blocker(m_site_pool);
  m_site_pool->clear();
  for (const auto& [name, index] : entries)
    m_site_pool->addItem(name, index);
  const int restored = current.isValid() ? m_site_pool->findData(current) : 0;
  m_site_pool->setCurrentIndex(std::max(restored, 0));
}

void XashMemoryWidget::UpdateSites(bool force)
{
  // Walking every allocation is the expensive part; do it every couple of seconds at most.
  if (!force && m_site_timer.isValid() && m_site_timer.elapsed() < 2000)
    return;
  if (!m_analyzer->GetXash())
    return;
  m_site_timer.start();

  const int pool = m_site_pool->currentData().isValid() ? m_site_pool->currentData().toInt() : -1;
  auto& system = Core::System::GetInstance();
  auto& reader = m_analyzer->GetXashReader();
  const std::vector<Xash::AllocationSite> sites = reader.ReadAllocationSites(system, pool);

  std::unordered_map<int, QString> pool_names;
  for (const Xash::Pool& info : m_analyzer->GetXash()->pools)
    pool_names[info.index] = info.name;

  u64 total = 0;
  int count = 0;
  {
    TableFill fill(m_site_table);
    m_site_table->setRowCount(static_cast<int>(sites.size()));
    int row = 0;
    for (const Xash::AllocationSite& site : sites)
    {
      const QString source =
          site.line ? QStringLiteral("%1:%2").arg(ShortSource(site.file)).arg(site.line) :
                      site.file;
      auto* const item = TextItem(source, QStringLiteral("%1:%2").arg(site.file).arg(site.line));
      item->setToolTip(site.line ? QStringLiteral("%1:%2").arg(site.file).arg(site.line) :
                                   site.file);
      m_site_table->setItem(row, 0, item);
      m_site_table->setItem(row, 1, new NumericItem(QString::number(site.count), site.count));
      m_site_table->setItem(row, 2, new NumericItem(FormatBytes(site.bytes), double(site.bytes)));
      m_site_table->setItem(row, 3,
                            new NumericItem(FormatBytes(site.largest), double(site.largest)));
      QStringList names;
      for (int index : site.pools)
      {
        if (names.size() == 3)
        {
          names << tr("+%1 more").arg(site.pools.size() - 3);
          break;
        }
        names << pool_names[index];
      }
      m_site_table->setItem(row, 4, TextItem(names.join(QStringLiteral(", "))));
      total += site.bytes;
      count += site.count;
      ++row;
    }
  }
  m_site_summary->setText(tr("%n allocation(s), %1 from %2 source lines", nullptr, count)
                              .arg(FormatBytes(total))
                              .arg(sites.size()));
  if (m_site_table->columnWidth(0) < 200 && !sites.empty())
  {
    m_site_table->resizeColumnsToContents();
    m_site_table->setColumnWidth(0, std::max(m_site_table->columnWidth(0), 240));
  }
}

void XashMemoryWidget::UpdateAram()
{
  const Xash::Snapshot& xash = *m_analyzer->GetXash();
  if (!xash.aram)
  {
    m_aram_summary->setText(
        tr("The ARAM manager (gc_aram_heap) hasn't started, or its symbols aren't loaded."));
    m_aram_table->setRowCount(0);
    m_asset_table->setRowCount(0);
    return;
  }
  const Xash::Aram& aram = *xash.aram;

  QString summary = tr("Session %1 (from the bottom), stream %2 (from the top), free %3, largest "
                       "free %4, peak %5.")
                        .arg(FormatBytes(aram.session_bytes), FormatBytes(aram.stream_bytes),
                             FormatBytes(aram.FreeBytes()), FormatBytes(aram.LargestFree()),
                             FormatBytes(aram.peak_bytes));
  if (aram.failures)
    summary += QLatin1Char(' ') + tr("%n allocation(s) refused.", nullptr, int(aram.failures));
  if (aram.has_cache)
  {
    const u32 lookups = aram.hits + aram.misses;
    summary += QStringLiteral("\n") +
               tr("Asset cache: %1 of %2 budget, %3 hits / %4 misses (%5%), %6 stores, %7 "
                  "evictions, %8 refused.")
                   .arg(FormatBytes(aram.cache_bytes), FormatBytes(aram.cache_budget))
                   .arg(aram.hits)
                   .arg(aram.misses)
                   .arg(lookups ? 100 * aram.hits / lookups : 0)
                   .arg(aram.stores)
                   .arg(aram.evictions)
                   .arg(aram.refusals);
  }
  if (xash.prefetch_capacity && *xash.prefetch_capacity)
  {
    summary += QStringLiteral("\n") + tr("Menu prefetch: %1 of %2, %3 files")
                                          .arg(FormatBytes(xash.prefetch_used.value_or(0)),
                                               FormatBytes(*xash.prefetch_capacity))
                                          .arg(xash.prefetch_entries.value_or(0));
  }
  m_aram_summary->setText(summary);

  {
    TableFill fill(m_aram_table);
    m_aram_table->setRowCount(static_cast<int>(aram.segments.size()));
    int row = 0;
    for (const Xash::AramSegment& segment : aram.segments)
    {
      m_aram_table->setItem(row, 0, TextItem(Hex32(segment.offset), segment.offset));
      m_aram_table->setItem(row, 1, TextItem(Hex32(segment.offset + segment.size)));
      m_aram_table->setItem(row, 2, new NumericItem(FormatBytes(segment.size), segment.size));
      auto* const owner = TextItem(segment.used ? (segment.tag.isEmpty() ? tr("(untagged)") :
                                                                           segment.tag) :
                                                  tr("Free"));
      if (!segment.used)
        owner->setForeground(palette().color(QPalette::PlaceholderText));
      m_aram_table->setItem(row, 3, owner);
      m_aram_table->setItem(row, 4, TextItem(!segment.used  ? QString() :
                                             segment.stream ? tr("Stream") :
                                                              tr("Session")));
      ++row;
    }
  }
  {
    TableFill fill(m_asset_table);
    m_asset_table->setRowCount(static_cast<int>(aram.assets.size()));
    int row = 0;
    for (const Xash::AramAsset& asset : aram.assets)
    {
      m_asset_table->setItem(row, 0, TextItem(asset.name, asset.name));
      m_asset_table->setItem(row, 1, new NumericItem(FormatBytes(asset.size), asset.size));
      auto* const where = TextItem(Hex32(asset.aram));
      where->setData(Qt::UserRole + 2, asset.aram);
      m_asset_table->setItem(row, 2, where);
      m_asset_table->setItem(row, 3, new NumericItem(QString::number(asset.pins), asset.pins));
      m_asset_table->setItem(row, 4, TextItem(asset.ready ? tr("Ready") : tr("Streaming")));
      const u32 age = aram.cache_clock >= asset.last_use ? aram.cache_clock - asset.last_use : 0;
      m_asset_table->setItem(
          row, 5,
          new NumericItem(age == 0 ? tr("latest") : tr("%n lookup(s) ago", nullptr, int(age)),
                          age));
      ++row;
    }
  }
  for (QTableWidget* table : {m_aram_table, m_asset_table})
  {
    if (table->rowCount() != 0 && !table->property("sized").toBool())
    {
      table->setProperty("sized", true);
      table->resizeColumnsToContents();
    }
  }
}

void XashMemoryWidget::UpdateMaps()
{
  const auto& visits = m_analyzer->GetXashMapVisits();
  {
    TableFill fill(m_map_table);
    m_map_table->setRowCount(static_cast<int>(visits.size()));
    for (size_t i = 0; i < visits.size(); ++i)
    {
      // Newest first.
      const int row = static_cast<int>(visits.size() - 1 - i);
      const XashMapVisit& visit = visits[i];
      m_map_table->setItem(row, 0, TextItem(visit.map, static_cast<int>(i)));
      m_map_table->setItem(row, 1, TextItem(FormatDuration(visit.entered)));
      m_map_table->setItem(row, 2, TextItem(FormatDuration(visit.last_seen - visit.entered)));
      m_map_table->setItem(row, 3, new NumericItem(FormatBytes(visit.zone_at_entry),
                                                   double(visit.zone_at_entry)));
      m_map_table->setItem(row, 4,
                           new NumericItem(FormatBytes(visit.zone_now), double(visit.zone_now)));
      // Against the last visit to the same map if there is one (that's the leak signal),
      // otherwise against the map before.
      const XashMapVisit* previous = nullptr;
      for (size_t j = i; j-- > 0;)
      {
        if (visits[j].map == visit.map)
        {
          previous = &visits[j];
          break;
        }
      }
      if (previous)
      {
        auto* const item = DeltaItem(s64(visit.zone_now) - s64(previous->zone_now));
        item->setToolTip(tr("Compared with the last visit to %1").arg(visit.map));
        m_map_table->setItem(row, 5, item);
      }
      else if (i > 0)
      {
        auto* const item = DeltaItem(s64(visit.zone_now) - s64(visits[i - 1].zone_now));
        item->setToolTip(tr("Compared with %1, the map before").arg(visits[i - 1].map));
        m_map_table->setItem(row, 5, item);
      }
      else
      {
        m_map_table->setItem(row, 5, TextItem(QString()));
      }
      m_map_table->setItem(row, 6,
                           new NumericItem(FormatBytes(visit.zone_peak), double(visit.zone_peak)));
      m_map_table->setItem(row, 7,
                           new NumericItem(FormatBytes(visit.heap_now), double(visit.heap_now)));
      m_map_table->setItem(row, 8,
                           new NumericItem(FormatBytes(visit.aram_now), double(visit.aram_now)));
    }
  }
  if (m_map_table->rowCount() != 0 && !m_map_table->property("sized").toBool())
  {
    m_map_table->setProperty("sized", true);
    m_map_table->resizeColumnsToContents();
  }
  UpdateMapDetail();
}

void XashMemoryWidget::UpdateMapDetail()
{
  const auto& visits = m_analyzer->GetXashMapVisits();
  int index = static_cast<int>(visits.size()) - 1;
  if (const QTableWidgetItem* item = m_map_table->currentItem())
  {
    if (const QTableWidgetItem* key = m_map_table->item(item->row(), 0))
      index = key->data(Qt::UserRole).toInt();
  }
  if (index < 0 || index >= static_cast<int>(visits.size()))
  {
    m_map_detail_label->setText(tr("Pool changes appear here after the first map change."));
    m_map_detail_table->setRowCount(0);
    return;
  }
  const XashMapVisit& visit = visits[index];
  const XashMapVisit* previous = nullptr;
  for (int j = index; j-- > 0;)
  {
    if (visits[j].map == visit.map)
    {
      previous = &visits[j];
      break;
    }
  }
  if (!previous && index > 0)
    previous = &visits[index - 1];
  if (!previous)
  {
    m_map_detail_label->setText(tr("%1 is the first map seen; nothing to compare with yet.")
                                    .arg(visit.map));
    m_map_detail_table->setRowCount(0);
    return;
  }
  m_map_detail_label->setText(
      previous->map == visit.map ?
          tr("Pools that changed between the previous visit to %1 and this one:").arg(visit.map) :
          tr("Pools that changed between %1 and %2:").arg(previous->map, visit.map));

  std::map<QString, std::pair<u64, u64>> changes;
  for (const auto& [name, bytes] : previous->pools)
    changes[name].first = bytes;
  for (const auto& [name, bytes] : visit.pools)
    changes[name].second = bytes;
  std::erase_if(changes, [](const auto& entry) {
    return entry.second.first == entry.second.second;
  });

  TableFill fill(m_map_detail_table);
  m_map_detail_table->setRowCount(static_cast<int>(changes.size()));
  int row = 0;
  for (const auto& [name, sizes] : changes)
  {
    m_map_detail_table->setItem(row, 0, TextItem(name, name));
    m_map_detail_table->setItem(
        row, 1,
        new NumericItem(sizes.first ? FormatBytes(sizes.first) : tr("—"), double(sizes.first)));
    m_map_detail_table->setItem(
        row, 2,
        new NumericItem(sizes.second ? FormatBytes(sizes.second) : tr("freed"),
                        double(sizes.second)));
    m_map_detail_table->setItem(row, 3, DeltaItem(s64(sizes.second) - s64(sizes.first)));
    ++row;
  }
}
