// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QtUtils/TableExport.h"

#include <algorithm>
#include <string>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDockWidget>
#include <QFile>
#include <QHeaderView>
#include <QMenu>
#include <QTabWidget>
#include <QTableView>
#include <QTreeView>

#include "DolphinQt/IconSet.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "GxBake/DataTable.h"

namespace TableExport
{
namespace
{
OpenHandler s_open_handler;

const QHeaderView* HorizontalHeader(const QAbstractItemView* view)
{
  if (const auto* table = qobject_cast<const QTableView*>(view))
    return table->horizontalHeader();
  if (const auto* tree = qobject_cast<const QTreeView*>(view))
    return tree->header();
  return nullptr;
}

bool IsRowHidden(const QAbstractItemView* view, int row, const QModelIndex& parent)
{
  if (const auto* table = qobject_cast<const QTableView*>(view))
    return table->isRowHidden(row);
  if (const auto* tree = qobject_cast<const QTreeView*>(view))
    return tree->isRowHidden(row, parent);
  return false;
}

// Columns in the order they're shown, leaving out hidden ones.
QList<int> VisibleColumns(const QAbstractItemView* view)
{
  QList<int> columns;
  const QAbstractItemModel* const model = view->model();
  const QHeaderView* const header = HorizontalHeader(view);
  const int count = model->columnCount(view->rootIndex());
  for (int visual = 0; visual < count; ++visual)
  {
    const int logical = header ? header->logicalIndex(visual) : visual;
    if (logical < 0 || (header && header->isSectionHidden(logical)))
      continue;
    columns.push_back(logical);
  }
  return columns;
}

void AddRows(const QAbstractItemView* view, const QModelIndex& parent, const QList<int>& columns,
             int depth, QList<QStringList>* rows)
{
  const QAbstractItemModel* const model = view->model();
  const int count = model->rowCount(parent);
  for (int row = 0; row < count; ++row)
  {
    if (IsRowHidden(view, row, parent))
      continue;
    QStringList cells;
    for (int i = 0; i < columns.size(); ++i)
    {
      QString text = model->index(row, columns[i], parent).data(Qt::DisplayRole).toString();
      if (i == 0 && depth > 0)
        text.prepend(QString(depth * 2, QLatin1Char(' ')));
      cells.push_back(text);
    }
    rows->push_back(cells);
    const QModelIndex child_parent = model->index(row, 0, parent);
    if (model->hasChildren(child_parent))
      AddRows(view, child_parent, columns, depth + 1, rows);
  }
}

std::string ToCsv(const QList<QStringList>& rows)
{
  GxBake::DataTable::Rows out;
  out.reserve(rows.size());
  for (const QStringList& row : rows)
  {
    GxBake::DataTable::Row cells;
    cells.reserve(row.size());
    for (const QString& cell : row)
      cells.push_back(cell.toStdString());
    out.push_back(std::move(cells));
  }
  return GxBake::DataTable::WriteDelimited(out, ',');
}

QString FileNameFor(const QString& name)
{
  QString file = name.toLower();
  for (QChar& c : file)
  {
    if (!c.isLetterOrNumber())
      c = QLatin1Char('-');
  }
  while (file.contains(QStringLiteral("--")))
    file.replace(QStringLiteral("--"), QStringLiteral("-"));
  while (file.startsWith(QLatin1Char('-')))
    file.remove(0, 1);
  while (file.endsWith(QLatin1Char('-')))
    file.chop(1);
  return (file.isEmpty() ? QStringLiteral("table") : file) + QStringLiteral(".csv");
}
}  // namespace

QList<QStringList> ToRows(const QAbstractItemView* view)
{
  QList<QStringList> rows;
  const QAbstractItemModel* const model = view ? view->model() : nullptr;
  if (!model)
    return rows;

  const QList<int> columns = VisibleColumns(view);
  const QHeaderView* const header = HorizontalHeader(view);
  QStringList header_row;
  for (const int column : columns)
    header_row.push_back(model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
  // Some tools hide their header; the first row is still the header, just with column letters.
  if (header && header->isHidden() &&
      std::all_of(header_row.begin(), header_row.end(),
                  [](const QString& s) { return s.isEmpty() || s.toInt() > 0; }))
  {
    for (int i = 0; i < header_row.size(); ++i)
      header_row[i] = QString::fromStdString(GxBake::DataTable::ColumnName(i));
  }
  rows.push_back(header_row);
  AddRows(view, view->rootIndex(), columns, 0, &rows);
  return rows;
}

QString TableName(const QAbstractItemView* view)
{
  QStringList parts;
  for (const QWidget* w = view; w; w = w->parentWidget())
  {
    if (const auto* dock = qobject_cast<const QDockWidget*>(w))
    {
      parts.prepend(dock->windowTitle());
      break;
    }
    // A tab page's parent is the tab widget's internal stack; the tab widget is one further up.
    const QWidget* const page = w;
    if (const QWidget* stack = page->parentWidget())
    {
      if (const auto* tabs = qobject_cast<const QTabWidget*>(stack->parentWidget()))
      {
        const int index = tabs->indexOf(const_cast<QWidget*>(page));
        if (index >= 0)
          parts.prepend(tabs->tabText(index).remove(QLatin1Char('&')));
      }
    }
  }
  if (parts.isEmpty())
    return QApplication::translate("TableExport", "Table");
  return parts.join(QStringLiteral(" - "));
}

void CopyAsCsv(const QAbstractItemView* view)
{
  QApplication::clipboard()->setText(QString::fromStdString(ToCsv(ToRows(view))));
}

void SaveAsCsv(QAbstractItemView* view)
{
  const QString path = DolphinFileDialog::getSaveFileName(
      view, QApplication::translate("TableExport", "Export Table to CSV"),
      FileNameFor(TableName(view)),
      QApplication::translate("TableExport", "CSV files (*.csv);;All files (*)"));
  if (path.isEmpty())
    return;

  QFile file(path);
  const std::string csv = ToCsv(ToRows(view));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      file.write(csv.data(), static_cast<qint64>(csv.size())) != static_cast<qint64>(csv.size()))
  {
    ModalMessageBox::warning(view, QApplication::translate("TableExport", "Export Table to CSV"),
                             QApplication::translate("TableExport", "Couldn't write %1: %2")
                                 .arg(path, file.errorString()));
  }
}

void AddActions(QMenu* menu, QAbstractItemView* view)
{
  if (!menu->isEmpty())
    menu->addSeparator();
  menu->addAction(IconSet::Get("copy"), QApplication::translate("TableExport", "Copy Table as CSV"),
                  view, [view] { CopyAsCsv(view); });
  menu->addAction(IconSet::Get("file-csv"),
                  QApplication::translate("TableExport", "Export Table to CSV..."), view,
                  [view] { SaveAsCsv(view); });
  if (s_open_handler)
  {
    menu->addAction(IconSet::Get("table-cells"),
                    QApplication::translate("TableExport", "Open in Data Tables"), view, [view] {
                      if (s_open_handler)
                        s_open_handler(TableName(view), ToRows(view));
                    });
  }
}

void Install(QAbstractItemView* view)
{
  view->setContextMenuPolicy(Qt::CustomContextMenu);
  QObject::connect(view, &QWidget::customContextMenuRequested, view, [view](const QPoint& pos) {
    QMenu menu(view);
    AddActions(&menu, view);
    menu.exec(view->viewport()->mapToGlobal(pos));
  });
}

void SetOpenHandler(OpenHandler handler)
{
  s_open_handler = std::move(handler);
}
}  // namespace TableExport
