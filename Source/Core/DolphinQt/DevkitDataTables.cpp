// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitDataTables.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include "DolphinQt/DevkitProject.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

namespace DTab = GxBake::DataTable;

namespace
{
constexpr char RAW_MIME_TYPE[] = "application/x-dolphin-devkit-datatable";
const QColor ERROR_COLOR(0xe5, 0x48, 0x4d);

QString Q(const std::string& s)
{
  return QString::fromStdString(s);
}

class SnapshotCommand final : public QUndoCommand
{
public:
  SnapshotCommand(DataTableModel* model, DataTableModel::Snapshot before,
                  DataTableModel::Snapshot after, const QString& text)
      : QUndoCommand(text), m_model(model), m_before(std::move(before)), m_after(std::move(after))
  {
  }

  void undo() override { m_model->Restore(m_before); }
  void redo() override { m_model->Restore(m_after); }

private:
  DataTableModel* m_model;
  DataTableModel::Snapshot m_before;
  DataTableModel::Snapshot m_after;
};

QString ErrorHelp(const QString& code)
{
  if (code == QStringLiteral("#REF!"))
    return QObject::tr("The formula refers to a cell outside the table.");
  if (code == QStringLiteral("#NAME?"))
    return QObject::tr("Unknown function or column name.");
  if (code == QStringLiteral("#VALUE!"))
    return QObject::tr("A value has the wrong type (text where a number is needed, or a range "
                       "where one cell is needed).");
  if (code == QStringLiteral("#DIV/0!"))
    return QObject::tr("Division by zero.");
  if (code == QStringLiteral("#CYCLE!"))
    return QObject::tr("The formula depends on its own result.");
  if (code == QStringLiteral("#PARSE!"))
    return QObject::tr("The formula can't be read.");
  if (code == QStringLiteral("#NUM!"))
    return QObject::tr("The result isn't a finite number.");
  if (code == QStringLiteral("#DEPTH!"))
    return QObject::tr("Too many formulas depend on each other in a chain.");
  return {};
}

bool RowIsEmpty(const DTab::Row& row)
{
  return std::all_of(row.begin(), row.end(), [](const std::string& s) { return s.empty(); });
}

QList<int> SortedUnique(QList<int> list)
{
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
  return list;
}

// Sort key: numbers first (by value), then text (case-insensitive), then empty cells.
struct SortKey
{
  int group;
  double number;
  QString text;
};

SortKey KeyFor(const DTab::Value& v)
{
  switch (v.kind)
  {
  case DTab::Value::Kind::Number:
  case DTab::Value::Kind::Boolean:
    return {0, v.number, {}};
  case DTab::Value::Kind::Text:
  case DTab::Value::Kind::Error:
    return {1, 0, Q(v.text).toCaseFolded()};
  case DTab::Value::Kind::Empty:
    break;
  }
  return {2, 0, {}};
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// DataTableModel

DataTableModel::DataTableModel(QUndoStack* undo_stack, QObject* parent)
    : QAbstractTableModel(parent), m_undo_stack(undo_stack)
{
  m_snapshot.header = {"A"};
}

DataTableModel::~DataTableModel() = default;

int DataTableModel::rowCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(m_snapshot.data.size());
}

int DataTableModel::columnCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(m_snapshot.header.size());
}

QString DataTableModel::RawText(int row, int column) const
{
  if (row < 0 || column < 0 || static_cast<size_t>(row) >= m_snapshot.data.size())
    return {};
  const DTab::Row& cells = m_snapshot.data[row];
  return static_cast<size_t>(column) < cells.size() ? Q(cells[column]) : QString();
}

DTab::Evaluator& DataTableModel::Eval() const
{
  if (!m_eval)
    m_eval = std::make_unique<DTab::Evaluator>(m_snapshot.header, m_snapshot.data);
  return *m_eval;
}

const DTab::Column& DataTableModel::ColumnInfo(int column) const
{
  if (!m_columns_valid)
  {
    m_columns = DTab::DescribeColumns(m_snapshot.header, m_snapshot.data);
    m_columns_valid = true;
  }
  static const DTab::Column empty;
  if (column < 0 || static_cast<size_t>(column) >= m_columns.size())
    return empty;
  return m_columns[column];
}

QVariant DataTableModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return {};
  const int row = index.row();
  const int column = index.column();

  switch (role)
  {
  case Qt::EditRole:
    return RawText(row, column);
  case Qt::DisplayRole:
  {
    // Values show as typed (0x10 stays hex); formulas show their result.
    const QString raw = RawText(row, column);
    if (!raw.startsWith(QLatin1Char('=')))
      return raw;
    return Q(DTab::FormatValue(Eval().Get(row, column)));
  }
  case Qt::TextAlignmentRole:
  {
    DTab::Value v = Eval().Get(row, column);
    if (v.kind == DTab::Value::Kind::Text)
      v = DTab::ParseLiteral(v.text);  // =HEX(...) results line up with the numbers
    if (v.kind == DTab::Value::Kind::Number)
      return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    if (v.kind == DTab::Value::Kind::Boolean || v.kind == DTab::Value::Kind::Error)
      return QVariant(Qt::AlignCenter);
    return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
  }
  case Qt::ForegroundRole:
    if (Eval().Get(row, column).IsError())
      return ERROR_COLOR;
    return {};
  case Qt::FontRole:
    if (RawText(row, column).startsWith(QLatin1Char('=')))
    {
      QFont font;
      font.setItalic(true);
      return font;
    }
    return {};
  case Qt::ToolTipRole:
  {
    const QString raw = RawText(row, column);
    const DTab::Value v = Eval().Get(row, column);
    QString tip = Q(DTab::CellName(row, column));
    if (raw.startsWith(QLatin1Char('=')))
      tip += QStringLiteral(": %1\n= %2").arg(raw, Q(DTab::FormatValue(v)));
    if (v.IsError())
    {
      const QString help = ErrorHelp(Q(v.text));
      if (!help.isEmpty())
        tip += QLatin1Char('\n') + help;
    }
    return tip;
  }
  default:
    return {};
  }
}

bool DataTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
  if (!index.isValid() || role != Qt::EditRole)
    return false;
  const QString text = value.toString();
  if (text == RawText(index.row(), index.column()))
    return false;
  SetCells(index.row(), index.column(), {{text}},
           tr("Edit %1").arg(Q(DTab::CellName(index.row(), index.column()))));
  return true;
}

QVariant DataTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation == Qt::Vertical)
  {
    if (role == Qt::DisplayRole)
      return section + 1;
    return {};
  }
  if (section < 0 || static_cast<size_t>(section) >= m_snapshot.header.size())
    return {};

  const QString letter = Q(DTab::ColumnName(section));
  const DTab::Column& info = ColumnInfo(section);
  switch (role)
  {
  case Qt::DisplayRole:
    if (info.name.empty())
      return letter;
    return QStringLiteral("%1  %2").arg(letter, Q(info.name));
  case Qt::ToolTipRole:
    return tr("Column %1: %2\nBakes to %3 (%4)%5")
        .arg(letter, Q(info.name), Q(std::string(DTab::ColumnCType(info.type))),
             Q(std::string(DTab::ColumnTypeName(info.type))),
             info.explicit_type ? QString() : tr(", inferred from the values"));
  default:
    return {};
  }
}

Qt::ItemFlags DataTableModel::flags(const QModelIndex& index) const
{
  if (!index.isValid())
    return Qt::NoItemFlags;
  return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable;
}

void DataTableModel::Normalize(Snapshot* snapshot) const
{
  if (snapshot->header.empty())
    snapshot->header.push_back("A");
  size_t columns = snapshot->header.size();
  for (const DTab::Row& row : snapshot->data)
    columns = std::max(columns, row.size());
  for (size_t i = snapshot->header.size(); i < columns; ++i)
    snapshot->header.push_back(DTab::ColumnName(i));
  for (DTab::Row& row : snapshot->data)
    row.resize(columns);
}

void DataTableModel::Invalidate()
{
  m_eval.reset();
  m_columns_valid = false;
}

void DataTableModel::Reset(Snapshot snapshot)
{
  Normalize(&snapshot);
  beginResetModel();
  m_snapshot = std::move(snapshot);
  Invalidate();
  endResetModel();
}

void DataTableModel::Restore(const Snapshot& snapshot)
{
  if (snapshot.header.size() == m_snapshot.header.size() &&
      snapshot.data.size() == m_snapshot.data.size())
  {
    // Same shape: keep the view's selection and scroll position.
    m_snapshot = snapshot;
    Invalidate();
    if (rowCount() > 0)
      emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    emit headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
    return;
  }
  Reset(snapshot);
}

void DataTableModel::Apply(Snapshot snapshot, const QString& text)
{
  Normalize(&snapshot);
  m_undo_stack->push(new SnapshotCommand(this, m_snapshot, std::move(snapshot), text));
}

void DataTableModel::SetCells(int row, int column, const QList<QStringList>& block,
                              const QString& text)
{
  if (row < 0 || column < 0 || block.isEmpty())
    return;
  Snapshot next = m_snapshot;
  for (int r = 0; r < block.size(); ++r)
  {
    const size_t target_row = static_cast<size_t>(row + r);
    if (target_row >= next.data.size())
      next.data.resize(target_row + 1);
    for (int c = 0; c < block[r].size(); ++c)
    {
      const size_t target_column = static_cast<size_t>(column + c);
      DTab::Row& cells = next.data[target_row];
      if (target_column >= cells.size())
        cells.resize(target_column + 1);
      cells[target_column] = block[r][c].toStdString();
    }
  }
  Apply(std::move(next), text);
}

void DataTableModel::InsertRows(int row, int count)
{
  Snapshot next = m_snapshot;
  row = std::clamp(row, 0, rowCount());
  next.data.insert(next.data.begin() + row, static_cast<size_t>(count),
                   DTab::Row(next.header.size()));
  Apply(std::move(next), count == 1 ? tr("Insert Row") : tr("Insert %n Row(s)", "", count));
}

void DataTableModel::RemoveRows(QList<int> rows)
{
  rows = SortedUnique(std::move(rows));
  Snapshot next = m_snapshot;
  for (auto it = rows.rbegin(); it != rows.rend(); ++it)
  {
    if (*it >= 0 && static_cast<size_t>(*it) < next.data.size())
      next.data.erase(next.data.begin() + *it);
  }
  Apply(std::move(next), tr("Delete %n Row(s)", "", static_cast<int>(rows.size())));
}

void DataTableModel::InsertColumns(int column, int count)
{
  Snapshot next = m_snapshot;
  column = std::clamp(column, 0, columnCount());
  // New columns get a name that isn't taken yet, so [name] references stay unambiguous.
  std::set<std::string> names;
  for (const std::string& h : next.header)
    names.insert(DTab::ParseColumnHeader(h).name);
  for (int i = 0; i < count; ++i)
  {
    std::string name;
    for (int n = 1;; ++n)
    {
      name = "column" + std::to_string(n);
      if (!names.count(name))
        break;
    }
    names.insert(name);
    next.header.insert(next.header.begin() + column + i, name);
    for (DTab::Row& r : next.data)
      r.insert(r.begin() + column + i, std::string());
  }
  Apply(std::move(next), tr("Insert %n Column(s)", "", count));
}

void DataTableModel::RemoveColumns(QList<int> columns)
{
  columns = SortedUnique(std::move(columns));
  if (static_cast<int>(columns.size()) >= columnCount())
    return;  // keep at least one column
  Snapshot next = m_snapshot;
  for (auto it = columns.rbegin(); it != columns.rend(); ++it)
  {
    if (*it < 0 || static_cast<size_t>(*it) >= next.header.size())
      continue;
    next.header.erase(next.header.begin() + *it);
    for (DTab::Row& r : next.data)
    {
      if (static_cast<size_t>(*it) < r.size())
        r.erase(r.begin() + *it);
    }
  }
  Apply(std::move(next), tr("Delete %n Column(s)", "", static_cast<int>(columns.size())));
}

void DataTableModel::RenameColumn(int column, const QString& name)
{
  if (column < 0 || column >= columnCount() || Q(m_snapshot.header[column]) == name)
    return;
  Snapshot next = m_snapshot;
  next.header[column] = name.toStdString();
  Apply(std::move(next), tr("Rename Column %1").arg(Q(DTab::ColumnName(column))));
}

void DataTableModel::SortByColumn(int column, bool ascending)
{
  if (column < 0 || column >= columnCount())
    return;
  std::vector<std::pair<SortKey, size_t>> keys;
  keys.reserve(m_snapshot.data.size());
  for (size_t r = 0; r < m_snapshot.data.size(); ++r)
    keys.emplace_back(KeyFor(Eval().Get(r, column)), r);
  std::stable_sort(keys.begin(), keys.end(), [ascending](const auto& a, const auto& b) {
    const SortKey& x = a.first;
    const SortKey& y = b.first;
    if (x.group != y.group)
      return x.group < y.group;  // empty cells stay at the bottom either way
    const int cmp = x.group == 0 ? (x.number < y.number ? -1 : (x.number > y.number ? 1 : 0)) :
                                   x.text.compare(y.text);
    return ascending ? cmp < 0 : cmp > 0;
  });
  Snapshot next = m_snapshot;
  for (size_t i = 0; i < keys.size(); ++i)
    next.data[i] = m_snapshot.data[keys[i].second];
  Apply(std::move(next), tr("Sort by Column %1").arg(Q(DTab::ColumnName(column))));
}

// ---------------------------------------------------------------------------------------------
// DataTableSheet

namespace
{
class SheetView final : public QTableView
{
public:
  // Writes a cell that's still being edited (e.g. when saving with the editor open).
  void CommitEdit()
  {
    if (state() != EditingState)
      return;
    QWidget* const editor = QApplication::focusWidget();
    if (editor && isAncestorOf(editor))
    {
      commitData(editor);
      closeEditor(editor, QAbstractItemDelegate::NoHint);
    }
  }
};

// Delete/Backspace clear the selected cells instead of starting an edit.
class SheetKeyFilter final : public QObject
{
public:
  explicit SheetKeyFilter(DataTableSheet* sheet) : QObject(sheet), m_sheet(sheet) {}

  bool eventFilter(QObject* watched, QEvent* event) override
  {
    if (event->type() == QEvent::KeyPress)
    {
      const auto* const key = static_cast<QKeyEvent*>(event);
      if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) &&
          key->modifiers() == Qt::NoModifier)
      {
        m_sheet->ClearSelection();
        return true;
      }
    }
    return QObject::eventFilter(watched, event);
  }

private:
  DataTableSheet* m_sheet;
};
}  // namespace

DataTableSheet::DataTableSheet(QWidget* parent) : QWidget(parent)
{
  m_undo_stack = new QUndoStack(this);
  m_model = new DataTableModel(m_undo_stack, this);
  m_view = new SheetView;
  m_view->setObjectName(QStringLiteral("dataTableView"));
  m_view->setModel(m_model);
  m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_view->setSelectionBehavior(QAbstractItemView::SelectItems);
  m_view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                          QAbstractItemView::AnyKeyPressed);
  m_view->setWordWrap(false);
  m_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
  m_view->horizontalHeader()->setDefaultSectionSize(120);
  m_view->horizontalHeader()->setHighlightSections(true);
  m_view->verticalHeader()->setDefaultSectionSize(m_view->fontMetrics().height() + 8);
  m_view->verticalHeader()->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_view->installEventFilter(new SheetKeyFilter(this));

  const auto add_shortcut = [this](QKeySequence::StandardKey key, auto&& slot) {
    auto* const action = new QAction(m_view);
    action->setShortcut(key);
    action->setShortcutContext(Qt::WidgetShortcut);
    connect(action, &QAction::triggered, this, slot);
    m_view->addAction(action);
  };
  add_shortcut(QKeySequence::Copy, [this] { CopySelection(); });
  add_shortcut(QKeySequence::Cut, [this] { CutSelection(); });
  add_shortcut(QKeySequence::Paste, [this] { Paste(); });

  auto* const layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_view);

  connect(m_undo_stack, &QUndoStack::cleanChanged, this, &DataTableSheet::ModifiedChanged);
}

QString DataTableSheet::GetTitle() const
{
  if (m_path.isEmpty())
    return m_untitled_name;
  return QFileInfo(m_path).fileName();
}

bool DataTableSheet::IsModified() const
{
  // Untitled tables made from tool output count as unsaved even before the first edit.
  return !m_undo_stack->isClean() || (m_path.isEmpty() && m_model->rowCount() > 0 &&
                                      !std::all_of(m_model->Data().begin(), m_model->Data().end(),
                                                   RowIsEmpty));
}

bool DataTableSheet::Load(const QString& path, QString* error)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    *error = file.errorString();
    return false;
  }
  const QByteArray bytes = file.readAll();
  const std::string_view text(bytes.constData(), static_cast<size_t>(bytes.size()));
  m_separator = DTab::DetectSeparator(text, QFileInfo(path).suffix().toStdString());
  DTab::Rows rows = DTab::ParseDelimited(text, m_separator);

  DataTableModel::Snapshot snapshot;
  if (!rows.empty())
  {
    snapshot.header = std::move(rows.front());
    snapshot.data.assign(std::make_move_iterator(rows.begin() + 1),
                         std::make_move_iterator(rows.end()));
  }
  m_model->Reset(std::move(snapshot));
  m_undo_stack->clear();
  m_path = QFileInfo(path).absoluteFilePath();
  emit ModifiedChanged();
  return true;
}

bool DataTableSheet::Save(const QString& path, QString* error)
{
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix == QStringLiteral("tsv") || suffix == QStringLiteral("tab"))
    m_separator = '\t';
  else if (suffix == QStringLiteral("csv") && m_separator == '\t')
    m_separator = ',';

  DTab::Rows rows;
  rows.push_back(m_model->Header());
  rows.insert(rows.end(), m_model->Data().begin(), m_model->Data().end());
  // Blank rows at the end (the spare rows of a new table) aren't written.
  while (rows.size() > 1 && RowIsEmpty(rows.back()))
    rows.pop_back();
  const std::string text = DTab::WriteDelimited(rows, m_separator);

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(text.data(), static_cast<qint64>(text.size())) !=
          static_cast<qint64>(text.size()) ||
      !file.commit())
  {
    *error = file.errorString();
    return false;
  }
  m_path = QFileInfo(path).absoluteFilePath();
  m_undo_stack->setClean();
  emit ModifiedChanged();
  return true;
}

void DataTableSheet::SetRows(const QList<QStringList>& rows)
{
  DataTableModel::Snapshot snapshot;
  for (qsizetype r = 0; r < rows.size(); ++r)
  {
    DTab::Row row;
    for (const QString& cell : rows[r])
      row.push_back(cell.toStdString());
    if (r == 0)
      snapshot.header = std::move(row);
    else
      snapshot.data.push_back(std::move(row));
  }
  m_model->Reset(std::move(snapshot));
  m_undo_stack->clear();
  emit ModifiedChanged();
}

namespace
{
struct Bounds
{
  int top = 0, left = 0, bottom = -1, right = -1;
  bool IsValid() const { return bottom >= top && right >= left; }
};

Bounds SelectionBounds(const QTableView* view)
{
  Bounds b;
  const QModelIndexList indexes = view->selectionModel()->selectedIndexes();
  if (indexes.isEmpty())
  {
    const QModelIndex current = view->currentIndex();
    if (current.isValid())
      b = {current.row(), current.column(), current.row(), current.column()};
    return b;
  }
  b = {indexes[0].row(), indexes[0].column(), indexes[0].row(), indexes[0].column()};
  for (const QModelIndex& index : indexes)
  {
    b.top = std::min(b.top, index.row());
    b.left = std::min(b.left, index.column());
    b.bottom = std::max(b.bottom, index.row());
    b.right = std::max(b.right, index.column());
  }
  return b;
}
}  // namespace

void DataTableSheet::CommitEdit()
{
  static_cast<SheetView*>(m_view)->CommitEdit();
}

void DataTableSheet::CopySelection()
{
  const Bounds b = SelectionBounds(m_view);
  if (!b.IsValid())
    return;
  DTab::Rows raw, shown;
  for (int r = b.top; r <= b.bottom; ++r)
  {
    DTab::Row raw_row, shown_row;
    for (int c = b.left; c <= b.right; ++c)
    {
      const QModelIndex index = m_model->index(r, c);
      raw_row.push_back(m_model->RawText(r, c).toStdString());
      shown_row.push_back(index.data(Qt::DisplayRole).toString().toStdString());
    }
    raw.push_back(std::move(raw_row));
    shown.push_back(std::move(shown_row));
  }
  // Other apps get the values; pasting back into a data table keeps the formulas.
  auto* const mime = new QMimeData;
  mime->setText(Q(DTab::WriteDelimited(shown, '\t')));
  mime->setData(QString::fromLatin1(RAW_MIME_TYPE),
                QByteArray::fromStdString(DTab::WriteDelimited(raw, '\t')));
  QApplication::clipboard()->setMimeData(mime);
}

void DataTableSheet::CutSelection()
{
  CopySelection();
  ClearSelection();
}

void DataTableSheet::Paste()
{
  const QModelIndex current = m_view->currentIndex();
  if (!current.isValid())
    return;
  const QMimeData* const mime = QApplication::clipboard()->mimeData();
  if (!mime)
    return;
  std::string text;
  if (mime->hasFormat(QString::fromLatin1(RAW_MIME_TYPE)))
    text = mime->data(QString::fromLatin1(RAW_MIME_TYPE)).toStdString();
  else if (mime->hasText())
    text = mime->text().toStdString();
  if (text.empty())
    return;

  const char separator = text.find('\t') != std::string::npos ? '\t' : DTab::DetectSeparator(text, "");
  // A single value with commas in it (a sentence) is one cell, not a row.
  const bool single_line = text.find('\n') == std::string::npos ||
                           text.find('\n') == text.size() - 1;
  DTab::Rows rows = (single_line && separator != '\t') ? DTab::Rows{{text}} :
                                                       DTab::ParseDelimited(text, separator);
  if (single_line && separator != '\t' && !rows[0][0].empty() && rows[0][0].back() == '\n')
    rows[0][0].pop_back();

  QList<QStringList> block;
  for (const DTab::Row& row : rows)
  {
    QStringList cells;
    for (const std::string& cell : row)
      cells.push_back(Q(cell));
    block.push_back(cells);
  }
  m_model->SetCells(current.row(), current.column(), block, tr("Paste"));

  // Select what was pasted.
  int width = 0;
  for (const QStringList& r : block)
    width = std::max(width, static_cast<int>(r.size()));
  const QModelIndex bottom_right = m_model->index(
      std::min(m_model->rowCount() - 1, current.row() + static_cast<int>(block.size()) - 1),
      std::min(m_model->columnCount() - 1, current.column() + width - 1));
  m_view->selectionModel()->select(QItemSelection(current, bottom_right),
                                   QItemSelectionModel::ClearAndSelect);
}

void DataTableSheet::ClearSelection()
{
  const QModelIndexList indexes = m_view->selectionModel()->selectedIndexes();
  if (indexes.isEmpty())
    return;
  DataTableModel::Snapshot next = m_model->Current();
  bool changed = false;
  for (const QModelIndex& index : indexes)
  {
    std::string& cell = next.data[index.row()][index.column()];
    changed = changed || !cell.empty();
    cell.clear();
  }
  if (changed)
    m_model->Apply(std::move(next), tr("Clear"));
}

QList<int> DataTableSheet::SelectedRows() const
{
  QList<int> rows;
  for (const QModelIndex& index : m_view->selectionModel()->selectedIndexes())
    rows.push_back(index.row());
  if (rows.isEmpty() && m_view->currentIndex().isValid())
    rows.push_back(m_view->currentIndex().row());
  return SortedUnique(rows);
}

QList<int> DataTableSheet::SelectedColumns() const
{
  QList<int> columns;
  for (const QModelIndex& index : m_view->selectionModel()->selectedIndexes())
    columns.push_back(index.column());
  if (columns.isEmpty() && m_view->currentIndex().isValid())
    columns.push_back(m_view->currentIndex().column());
  return SortedUnique(columns);
}

// ---------------------------------------------------------------------------------------------
// DataTablesWidget

DataTablesWidget::DataTablesWidget(DevkitProject* project, QWidget* parent)
    : QDockWidget(parent), m_project(project)
{
  setWindowTitle(tr("Data Tables"));
  setObjectName(QStringLiteral("datatables"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
              QDockWidget::DockWidgetClosable);

  auto* const toolbar = new QToolBar;
  toolbar->setObjectName(QStringLiteral("dataTablesToolBar"));
  toolbar->setIconSize(QSize(16, 16));
  toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

  const auto add = [this, toolbar](const char* icon, const QString& text, auto&& slot,
                                   const QKeySequence& shortcut = {}) {
    QAction* const action = toolbar->addAction(IconSet::Get(icon), text);
    if (!shortcut.isEmpty())
    {
      action->setShortcut(shortcut);
      action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
      action->setToolTip(QStringLiteral("%1 (%2)").arg(
          text, shortcut.toString(QKeySequence::NativeText)));
      addAction(action);
    }
    connect(action, &QAction::triggered, this, slot);
    return action;
  };

  add("file-circle-plus", tr("New Data Table"), [this] { NewTable(); });
  add("folder-open", tr("Open Data Table..."), [this] { OpenTable(); });
  m_save = add("floppy-disk", tr("Save"), [this] { SaveSheet(CurrentSheet(), false); },
               QKeySequence::Save);
  m_save_as = new QAction(tr("Save As..."), this);
  connect(m_save_as, &QAction::triggered, this, [this] { SaveSheet(CurrentSheet(), true); });
  toolbar->addSeparator();
  m_undo = add("rotate-left", tr("Undo"), [this] {
    if (DataTableSheet* sheet = CurrentSheet())
      sheet->UndoStack()->undo();
  }, QKeySequence::Undo);
  m_redo = add("rotate-right", tr("Redo"), [this] {
    if (DataTableSheet* sheet = CurrentSheet())
      sheet->UndoStack()->redo();
  }, QKeySequence::Redo);
  toolbar->addSeparator();
  m_insert_row = add("plus", tr("Insert Row Below"), [this] {
    if (DataTableSheet* sheet = CurrentSheet())
    {
      const QList<int> rows = sheet->SelectedRows();
      sheet->Model()->InsertRows(rows.isEmpty() ? sheet->Model()->rowCount() : rows.back() + 1, 1);
    }
  });
  m_delete_rows = add("minus", tr("Delete Selected Rows"), [this] {
    if (DataTableSheet* sheet = CurrentSheet())
      sheet->Model()->RemoveRows(sheet->SelectedRows());
  });
  m_insert_column = add("table-columns", tr("Insert Column Right"), [this] {
    if (DataTableSheet* sheet = CurrentSheet())
    {
      const QList<int> columns = sheet->SelectedColumns();
      sheet->Model()->InsertColumns(
          columns.isEmpty() ? sheet->Model()->columnCount() : columns.back() + 1, 1);
    }
  });
  m_delete_columns = add("trash-can", tr("Delete Selected Columns"), [this] {
    if (DataTableSheet* sheet = CurrentSheet())
      sheet->Model()->RemoveColumns(sheet->SelectedColumns());
  });
  toolbar->addSeparator();
  m_bake = add("file-code", tr("Bake to C Header..."), [this] { BakeHeader(CurrentSheet(), true); });
  m_bake->setToolTip(tr("Bake to C Header: writes the table as a struct array the game compiles "
                        "in, and re-bakes it every time the table is saved."));

  // Formula bar: the current cell's name and raw text.
  m_cell_name = new QLabel;
  m_cell_name->setObjectName(QStringLiteral("dataTableCellName"));
  m_cell_name->setMinimumWidth(m_cell_name->fontMetrics().horizontalAdvance(QStringLiteral("AAA0000")));
  m_cell_name->setAlignment(Qt::AlignCenter);
  auto* const fx = new QLabel(QStringLiteral("fx"));
  QFont fx_font = fx->font();
  fx_font.setItalic(true);
  fx_font.setBold(true);
  fx->setFont(fx_font);
  m_formula = new QLineEdit;
  m_formula->setObjectName(QStringLiteral("dataTableFormula"));
  m_formula->setPlaceholderText(tr("Value or =formula, e.g. =SUM(B1:B10) or =[hp]*2"));
  m_formula->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  connect(m_formula, &QLineEdit::returnPressed, this, &DataTablesWidget::CommitFormulaBar);

  auto* const formula_bar = new QHBoxLayout;
  formula_bar->setContentsMargins(4, 2, 4, 2);
  formula_bar->addWidget(m_cell_name);
  formula_bar->addWidget(fx);
  formula_bar->addWidget(m_formula, 1);

  // Empty state.
  auto* const empty = new QWidget;
  auto* const empty_layout = new QVBoxLayout(empty);
  empty_layout->addStretch();
  auto* const empty_label =
      new QLabel(tr("Data tables are CSV/TSV spreadsheets for game data: item stats, enemy "
                    "spawns, level lists. Formulas are evaluated, and a table can be baked to a C "
                    "header the game compiles in."));
  empty_label->setWordWrap(true);
  empty_label->setAlignment(Qt::AlignCenter);
  empty_layout->addWidget(empty_label);
  auto* const empty_buttons = new QHBoxLayout;
  auto* const new_button = new QPushButton(IconSet::Get("file-circle-plus"), tr("New Data Table"));
  auto* const open_button = new QPushButton(IconSet::Get("folder-open"), tr("Open..."));
  connect(new_button, &QPushButton::clicked, this, &DataTablesWidget::NewTable);
  connect(open_button, &QPushButton::clicked, this, &DataTablesWidget::OpenTable);
  empty_buttons->addStretch();
  empty_buttons->addWidget(new_button);
  empty_buttons->addWidget(open_button);
  empty_buttons->addStretch();
  empty_layout->addLayout(empty_buttons);
  empty_layout->addStretch();

  m_tabs = new QTabWidget;
  m_tabs->setObjectName(QStringLiteral("dataTableTabs"));
  m_tabs->setDocumentMode(true);
  m_tabs->setTabsClosable(true);
  m_tabs->setMovable(true);
  connect(m_tabs, &QTabWidget::tabCloseRequested, this, &DataTablesWidget::CloseSheet);
  connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
    UpdateActions();
    UpdateFormulaBar();
    UpdateStatus();
  });

  m_stack = new QStackedWidget;
  m_stack->addWidget(empty);
  m_stack->addWidget(m_tabs);

  m_status = new QLabel;
  m_status->setObjectName(QStringLiteral("dataTableStatus"));
  m_status->setContentsMargins(6, 2, 6, 2);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto* const body = new QWidget;
  auto* const layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(toolbar);
  layout->addLayout(formula_bar);
  layout->addWidget(m_stack, 1);
  layout->addWidget(m_status);
  setWidget(body);

  UpdateActions();
  UpdateFormulaBar();
  UpdateStatus();
}

bool DataTablesWidget::IsDataTableFile(const QString& path)
{
  const QString suffix = QFileInfo(path).suffix().toLower();
  return suffix == QStringLiteral("csv") || suffix == QStringLiteral("tsv") ||
         suffix == QStringLiteral("tab");
}

DataTableSheet* DataTablesWidget::CurrentSheet() const
{
  return qobject_cast<DataTableSheet*>(m_tabs->currentWidget());
}

QString DataTablesWidget::DefaultFolder() const
{
  if (m_project->IsOpen())
    return m_project->GetFolderPath("tables");
  return QDir::homePath();
}

DataTableSheet* DataTablesWidget::AddSheet(DataTableSheet* sheet)
{
  const int index = m_tabs->addTab(sheet, IconSet::Get("table-cells"), sheet->GetTitle());
  m_tabs->setTabToolTip(index, QDir::toNativeSeparators(sheet->GetPath()));

  const auto refresh_title = [this, sheet] {
    const int i = m_tabs->indexOf(sheet);
    if (i < 0)
      return;
    m_tabs->setTabText(i, sheet->GetTitle() + (sheet->IsModified() ? QStringLiteral(" *") : QString()));
    m_tabs->setTabToolTip(i, QDir::toNativeSeparators(sheet->GetPath()));
    if (sheet == CurrentSheet())
      UpdateActions();
  };
  connect(sheet, &DataTableSheet::ModifiedChanged, this, refresh_title);
  connect(sheet->UndoStack(), &QUndoStack::indexChanged, this, [this, sheet, refresh_title] {
    refresh_title();
    if (sheet == CurrentSheet())
    {
      UpdateFormulaBar();
      UpdateStatus();
    }
  });
  connect(sheet->View()->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this, sheet] {
            if (sheet == CurrentSheet())
            {
              UpdateFormulaBar();
              UpdateStatus();
            }
          });
  connect(sheet->View(), &QWidget::customContextMenuRequested, this,
          &DataTablesWidget::ShowCellMenu);
  connect(sheet->View()->horizontalHeader(), &QWidget::customContextMenuRequested, this,
          &DataTablesWidget::ShowHeaderMenu);
  connect(sheet->View()->horizontalHeader(), &QHeaderView::sectionDoubleClicked, this,
          &DataTablesWidget::RenameColumn);
  refresh_title();

  m_stack->setCurrentIndex(1);
  m_tabs->setCurrentIndex(index);
  sheet->View()->setCurrentIndex(sheet->Model()->index(0, 0));
  sheet->View()->setFocus();
  show();
  raise();
  return sheet;
}

void DataTablesWidget::NewTable()
{
  auto* const sheet = new DataTableSheet;
  sheet->SetUntitledName(tr("Untitled %1").arg(++m_untitled_count));
  DataTableModel::Snapshot snapshot;
  snapshot.header = {"name", "value"};
  snapshot.data.resize(8, DTab::Row(2));
  sheet->Model()->Reset(std::move(snapshot));
  AddSheet(sheet);
}

void DataTablesWidget::OpenTable()
{
  const QStringList paths = DolphinFileDialog::getOpenFileNames(
      this, tr("Open Data Table"), DefaultFolder(),
      tr("Data tables (*.csv *.tsv *.tab);;All files (*)"));
  for (const QString& path : paths)
    OpenFile(path);
}

void DataTablesWidget::OpenFile(const QString& path)
{
  const QFileInfo info(path);
  for (int i = 0; i < m_tabs->count(); ++i)
  {
    const auto* const sheet = qobject_cast<DataTableSheet*>(m_tabs->widget(i));
    if (sheet && !sheet->GetPath().isEmpty() && QFileInfo(sheet->GetPath()) == info)
    {
      m_tabs->setCurrentIndex(i);
      show();
      raise();
      return;
    }
  }

  auto* const sheet = new DataTableSheet;
  QString error;
  if (!sheet->Load(path, &error))
  {
    delete sheet;
    ModalMessageBox::warning(this, tr("Open Data Table"),
                             tr("Couldn't open %1: %2").arg(QDir::toNativeSeparators(path), error));
    return;
  }
  AddSheet(sheet);
}

void DataTablesWidget::OpenRows(const QString& name, const QList<QStringList>& rows)
{
  auto* const sheet = new DataTableSheet;
  sheet->SetUntitledName(name);
  sheet->SetRows(rows);
  AddSheet(sheet);
  UpdateStatus(tr("%n row(s) from %1. Save to keep them.", "", static_cast<int>(rows.size()) - 1)
                   .arg(name));
}

bool DataTablesWidget::SaveSheet(DataTableSheet* sheet, bool save_as)
{
  if (!sheet)
    return false;
  sheet->CommitEdit();

  QString path = sheet->GetPath();
  if (save_as || path.isEmpty())
  {
    const QString folder = DefaultFolder();
    QDir().mkpath(folder);
    QString name = sheet->GetTitle();
    if (!DataTablesWidget::IsDataTableFile(name))
    {
      for (QChar& c : name)
      {
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_'))
          c = QLatin1Char('_');
      }
      name = name.toLower() + QStringLiteral(".csv");
    }
    path = DolphinFileDialog::getSaveFileName(
        this, tr("Save Data Table"), path.isEmpty() ? QDir(folder).filePath(name) : path,
        tr("CSV (*.csv);;Tab-separated (*.tsv)"));
    if (path.isEmpty())
      return false;
    if (QFileInfo(path).suffix().isEmpty())
      path += QStringLiteral(".csv");
  }

  QString error;
  if (!sheet->Save(path, &error))
  {
    ModalMessageBox::warning(this, tr("Save Data Table"),
                             tr("Couldn't save %1: %2").arg(QDir::toNativeSeparators(path), error));
    return false;
  }
  UpdateStatus(tr("Saved %1").arg(QDir::toNativeSeparators(path)));

  // Tables with a header re-bake on every save, like assets do on boot.
  if (!m_project->GetDataTableHeader(path).isEmpty())
    BakeHeader(sheet, false);
  return true;
}

void DataTablesWidget::BakeHeader(DataTableSheet* sheet, bool ask)
{
  if (!sheet)
    return;
  if (sheet->GetPath().isEmpty() && !SaveSheet(sheet, false))
    return;

  const QString table_path = sheet->GetPath();
  QString header_path = m_project->GetDataTableHeader(table_path);
  if (ask)
  {
    QString suggestion = header_path;
    if (suggestion.isEmpty())
    {
      const QString folder =
          m_project->IsOpen() ? m_project->GetFolderPath("include") : QFileInfo(table_path).path();
      suggestion = QDir(folder).filePath(QFileInfo(table_path).completeBaseName() +
                                         QStringLiteral(".h"));
      QDir().mkpath(folder);
    }
    header_path = DolphinFileDialog::getSaveFileName(this, tr("Bake to C Header"), suggestion,
                                                     tr("C headers (*.h)"));
    if (header_path.isEmpty())
      return;
  }
  if (header_path.isEmpty())
    return;

  const QString source_name =
      m_project->IsOpen() ? QDir(m_project->GetPath()).relativeFilePath(table_path) :
                            QFileInfo(table_path).fileName();
  const DTab::HeaderResult result =
      DTab::ExportCHeader(sheet->Model()->Header(), sheet->Model()->Data(),
                        QFileInfo(table_path).completeBaseName().toStdString(),
                        source_name.toStdString());
  if (!result.ok)
  {
    const QString message = tr("Not baked: %1").arg(Q(result.error));
    UpdateStatus(message);
    if (ask)
      ModalMessageBox::warning(this, tr("Bake to C Header"), message);
    return;
  }

  QSaveFile file(header_path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(result.text.data(), static_cast<qint64>(result.text.size())) !=
          static_cast<qint64>(result.text.size()) ||
      !file.commit())
  {
    const QString message = tr("Couldn't write %1: %2")
                                .arg(QDir::toNativeSeparators(header_path), file.errorString());
    UpdateStatus(message);
    if (ask)
      ModalMessageBox::warning(this, tr("Bake to C Header"), message);
    return;
  }

  if (m_project->IsOpen())
    m_project->SetDataTableHeader(table_path, header_path);
  const QString shown = m_project->IsOpen() ?
                            QDir(m_project->GetPath()).relativeFilePath(header_path) :
                            QDir::toNativeSeparators(header_path);
  UpdateStatus(tr("Saved and baked to %1").arg(shown));
}

bool DataTablesWidget::CloseSheet(int index)
{
  auto* const sheet = qobject_cast<DataTableSheet*>(m_tabs->widget(index));
  if (!sheet)
    return true;
  if (sheet->IsModified())
  {
    m_tabs->setCurrentIndex(index);
    const int answer = ModalMessageBox::question(
        this, tr("Data Tables"), tr("Save changes to %1?").arg(sheet->GetTitle()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
      return false;
    if (answer == QMessageBox::Save && !SaveSheet(sheet, false))
      return false;
  }
  m_tabs->removeTab(index);
  sheet->deleteLater();
  if (m_tabs->count() == 0)
    m_stack->setCurrentIndex(0);
  UpdateActions();
  UpdateFormulaBar();
  UpdateStatus();
  return true;
}

bool DataTablesWidget::ConfirmClose()
{
  for (int i = 0; i < m_tabs->count(); ++i)
  {
    auto* const sheet = qobject_cast<DataTableSheet*>(m_tabs->widget(i));
    if (!sheet || !sheet->IsModified())
      continue;
    show();
    raise();
    m_tabs->setCurrentIndex(i);
    const int answer = ModalMessageBox::question(
        this, tr("Data Tables"), tr("Save changes to %1?").arg(sheet->GetTitle()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
      return false;
    if (answer == QMessageBox::Save && !SaveSheet(sheet, false))
      return false;
  }
  return true;
}

void DataTablesWidget::UpdateActions()
{
  DataTableSheet* const sheet = CurrentSheet();
  const bool has = sheet != nullptr;
  for (QAction* action : {m_save, m_save_as, m_insert_row, m_delete_rows, m_insert_column,
                          m_delete_columns, m_bake})
  {
    action->setEnabled(has);
  }
  m_undo->setEnabled(has && sheet->UndoStack()->canUndo());
  m_redo->setEnabled(has && sheet->UndoStack()->canRedo());
  m_undo->setToolTip(has && sheet->UndoStack()->canUndo() ?
                         tr("Undo %1").arg(sheet->UndoStack()->undoText()) :
                         tr("Undo"));
  m_redo->setToolTip(has && sheet->UndoStack()->canRedo() ?
                         tr("Redo %1").arg(sheet->UndoStack()->redoText()) :
                         tr("Redo"));
  m_formula->setEnabled(has);
}

void DataTablesWidget::UpdateFormulaBar()
{
  DataTableSheet* const sheet = CurrentSheet();
  const QModelIndex current = sheet ? sheet->View()->currentIndex() : QModelIndex();
  if (!current.isValid())
  {
    m_cell_name->clear();
    m_formula->clear();
    return;
  }
  m_cell_name->setText(Q(DTab::CellName(current.row(), current.column())));
  if (!m_formula->hasFocus() || !m_formula->isModified())
  {
    m_formula->setText(sheet->Model()->RawText(current.row(), current.column()));
    m_formula->setModified(false);
  }
}

void DataTablesWidget::CommitFormulaBar()
{
  DataTableSheet* const sheet = CurrentSheet();
  if (!sheet)
    return;
  const QModelIndex current = sheet->View()->currentIndex();
  if (!current.isValid())
    return;
  sheet->Model()->setData(current, m_formula->text(), Qt::EditRole);
  m_formula->setModified(false);
  // Move down, like pressing Enter in a cell.
  const QModelIndex below = sheet->Model()->index(current.row() + 1, current.column());
  if (below.isValid())
    sheet->View()->setCurrentIndex(below);
  sheet->View()->setFocus();
}

void DataTablesWidget::UpdateStatus(const QString& message)
{
  DataTableSheet* const sheet = CurrentSheet();
  if (!sheet)
  {
    m_status->setText(message);
    return;
  }
  const DataTableModel* const model = sheet->Model();
  QStringList parts;
  parts.push_back(tr("%n row(s)", "", model->rowCount()));
  const QModelIndex current = sheet->View()->currentIndex();
  if (current.isValid())
  {
    const DTab::Column& column = model->ColumnInfo(current.column());
    parts.push_back(tr("Column %1 \"%2\": %3")
                        .arg(Q(DTab::ColumnName(current.column())), Q(column.name),
                             Q(std::string(DTab::ColumnCType(column.type)))));
  }
  const QString header = m_project->GetDataTableHeader(sheet->GetPath());
  if (!header.isEmpty())
    parts.push_back(tr("Bakes to %1").arg(QDir(m_project->GetPath()).relativeFilePath(header)));
  if (!message.isEmpty())
    parts.push_back(message);
  m_status->setText(parts.join(QStringLiteral("   |   ")));
}

void DataTablesWidget::ShowCellMenu(const QPoint& pos)
{
  DataTableSheet* const sheet = CurrentSheet();
  if (!sheet)
    return;
  QMenu menu(this);
  menu.addAction(IconSet::Get("copy"), tr("Copy"), sheet, &DataTableSheet::CopySelection);
  menu.addAction(tr("Cut"), sheet, &DataTableSheet::CutSelection);
  menu.addAction(IconSet::Get("paste"), tr("Paste"), sheet, &DataTableSheet::Paste);
  menu.addAction(tr("Clear"), sheet, &DataTableSheet::ClearSelection);
  menu.addSeparator();
  const QList<int> rows = sheet->SelectedRows();
  const int first = rows.isEmpty() ? 0 : rows.front();
  const int last = rows.isEmpty() ? sheet->Model()->rowCount() - 1 : rows.back();
  menu.addAction(tr("Insert Row Above"), this, [sheet, first] { sheet->Model()->InsertRows(first, 1); });
  menu.addAction(IconSet::Get("plus"), tr("Insert Row Below"), this,
                 [sheet, last] { sheet->Model()->InsertRows(last + 1, 1); });
  menu.addAction(IconSet::Get("minus"), tr("Delete %n Row(s)", "", static_cast<int>(rows.size())),
                 this, [sheet, rows] { sheet->Model()->RemoveRows(rows); });
  menu.exec(sheet->View()->viewport()->mapToGlobal(pos));
}

void DataTablesWidget::ShowHeaderMenu(const QPoint& pos)
{
  DataTableSheet* const sheet = CurrentSheet();
  if (!sheet)
    return;
  QHeaderView* const header = sheet->View()->horizontalHeader();
  const int column = header->logicalIndexAt(pos);
  if (column < 0)
    return;
  DataTableModel* const model = sheet->Model();

  QMenu menu(this);
  menu.addAction(tr("Rename Column..."), this, [this, column] { RenameColumn(column); });

  // Type: rewrites the header's ":type" suffix.
  QMenu* const type_menu = menu.addMenu(tr("Type"));
  const DTab::Column& info = model->ColumnInfo(column);
  QAction* const automatic = type_menu->addAction(tr("Automatic"), this, [model, column] {
    model->RenameColumn(column, Q(DTab::ParseColumnHeader(model->Header()[column]).name));
  });
  automatic->setCheckable(true);
  automatic->setChecked(!info.explicit_type);
  type_menu->addSeparator();
  for (const DTab::ColumnType type :
       {DTab::ColumnType::S8, DTab::ColumnType::S16, DTab::ColumnType::S32, DTab::ColumnType::U8,
        DTab::ColumnType::U16, DTab::ColumnType::U32, DTab::ColumnType::Hex, DTab::ColumnType::F32,
        DTab::ColumnType::F64, DTab::ColumnType::Bool, DTab::ColumnType::String})
  {
    const QString name = Q(std::string(DTab::ColumnTypeName(type)));
    QAction* const action = type_menu->addAction(
        QStringLiteral("%1  (%2)").arg(name, Q(std::string(DTab::ColumnCType(type)))), this,
        [model, column, name] {
          model->RenameColumn(column, Q(DTab::ParseColumnHeader(model->Header()[column]).name) +
                                          QLatin1Char(':') + name);
        });
    action->setCheckable(true);
    action->setChecked(info.explicit_type && info.type == type);
  }

  menu.addSeparator();
  menu.addAction(tr("Sort Ascending"), this, [model, column] { model->SortByColumn(column, true); });
  menu.addAction(tr("Sort Descending"), this,
                 [model, column] { model->SortByColumn(column, false); });
  menu.addSeparator();
  menu.addAction(tr("Insert Column Left"), this,
                 [model, column] { model->InsertColumns(column, 1); });
  menu.addAction(IconSet::Get("table-columns"), tr("Insert Column Right"), this,
                 [model, column] { model->InsertColumns(column + 1, 1); });
  QAction* const remove = menu.addAction(IconSet::Get("trash-can"), tr("Delete Column"), this,
                                         [model, column] { model->RemoveColumns({column}); });
  remove->setEnabled(model->columnCount() > 1);
  menu.exec(header->mapToGlobal(pos));
}

void DataTablesWidget::RenameColumn(int column)
{
  DataTableSheet* const sheet = CurrentSheet();
  if (!sheet || column < 0 || column >= sheet->Model()->columnCount())
    return;
  bool ok = false;
  const QString name = QInputDialog::getText(
      this, tr("Rename Column"),
      tr("Name for column %1 (add :u8, :s16, :f32, :hex, :bool, :str ... to set its C type):")
          .arg(Q(DTab::ColumnName(column))),
      QLineEdit::Normal, Q(sheet->Model()->Header()[column]), &ok);
  if (ok)
    sheet->Model()->RenameColumn(column, name.trimmed());
}
