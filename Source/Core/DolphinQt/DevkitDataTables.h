// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include <QAbstractTableModel>
#include <QDockWidget>
#include <QList>
#include <QStringList>

#include "GxBake/DataTable.h"

class DevkitProject;
class QAction;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTabWidget;
class QTableView;
class QUndoStack;

// One data table: the header row (column names) and the data rows, as stored in the file. Cells
// show their evaluated value and edit their raw text (formulas included). Every change goes
// through the undo stack.
class DataTableModel final : public QAbstractTableModel
{
  Q_OBJECT
public:
  explicit DataTableModel(QUndoStack* undo_stack, QObject* parent = nullptr);
  ~DataTableModel() override;

  struct Snapshot
  {
    GxBake::DataTable::Row header;
    GxBake::DataTable::Rows data;
  };

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role) override;
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;

  const GxBake::DataTable::Row& Header() const { return m_snapshot.header; }
  const GxBake::DataTable::Rows& Data() const { return m_snapshot.data; }
  QString RawText(int row, int column) const;
  const GxBake::DataTable::Column& ColumnInfo(int column) const;

  // Replaces the whole table without an undo step (loading a file).
  void Reset(Snapshot snapshot);
  // Replaces the whole table as one undo step named |text|.
  void Apply(Snapshot snapshot, const QString& text);
  // Restores a snapshot (used by the undo commands).
  void Restore(const Snapshot& snapshot);
  Snapshot Current() const { return m_snapshot; }

  // Writes |block| (rows of cells) with its top-left corner at (row, column), growing the table
  // as needed. One undo step.
  void SetCells(int row, int column, const QList<QStringList>& block, const QString& text);
  void InsertRows(int row, int count);
  void RemoveRows(QList<int> rows);
  void InsertColumns(int column, int count);
  void RemoveColumns(QList<int> columns);
  void RenameColumn(int column, const QString& name);
  void SortByColumn(int column, bool ascending);

private:
  void Invalidate();
  GxBake::DataTable::Evaluator& Eval() const;
  void Normalize(Snapshot* snapshot) const;

  QUndoStack* m_undo_stack;
  Snapshot m_snapshot;
  mutable std::unique_ptr<GxBake::DataTable::Evaluator> m_eval;
  mutable std::vector<GxBake::DataTable::Column> m_columns;
  mutable bool m_columns_valid = false;
};

// One open data table: its grid, file and undo history.
class DataTableSheet final : public QWidget
{
  Q_OBJECT
public:
  explicit DataTableSheet(QWidget* parent = nullptr);

  bool Load(const QString& path, QString* error);
  bool Save(const QString& path, QString* error);
  void SetRows(const QList<QStringList>& rows);

  const QString& GetPath() const { return m_path; }
  QString GetTitle() const;
  void SetUntitledName(const QString& name) { m_untitled_name = name; }
  bool IsModified() const;

  DataTableModel* Model() const { return m_model; }
  QTableView* View() const { return m_view; }
  QUndoStack* UndoStack() const { return m_undo_stack; }

  void CommitEdit();
  void CopySelection();
  void CutSelection();
  void Paste();
  void ClearSelection();
  QList<int> SelectedRows() const;
  QList<int> SelectedColumns() const;

signals:
  void ModifiedChanged();

private:
  QString m_path;
  QString m_untitled_name;
  char m_separator = ',';
  QUndoStack* m_undo_stack;
  DataTableModel* m_model;
  QTableView* m_view;
};

// "Data Tables" dock: a spreadsheet editor for a project's game data (CSV/TSV files), with
// formulas, typed columns and baking to a C header the game compiles in.
class DataTablesWidget final : public QDockWidget
{
  Q_OBJECT
public:
  DataTablesWidget(DevkitProject* project, QWidget* parent = nullptr);

  static bool IsDataTableFile(const QString& path);

  void NewTable();
  void OpenTable();
  void OpenFile(const QString& path);
  // Opens rows (header first) as a new, unsaved table named |name|.
  void OpenRows(const QString& name, const QList<QStringList>& rows);

  // Asks about every table with unsaved changes. Returns false if the user cancelled.
  bool ConfirmClose();

private:
  DataTableSheet* CurrentSheet() const;
  DataTableSheet* AddSheet(DataTableSheet* sheet);
  bool SaveSheet(DataTableSheet* sheet, bool save_as);
  bool CloseSheet(int index);
  void BakeHeader(DataTableSheet* sheet, bool ask);
  void UpdateActions();
  void UpdateFormulaBar();
  void UpdateStatus(const QString& message = {});
  void CommitFormulaBar();
  void ShowCellMenu(const QPoint& pos);
  void ShowHeaderMenu(const QPoint& pos);
  void RenameColumn(int column);
  QString DefaultFolder() const;

  DevkitProject* m_project;
  QStackedWidget* m_stack;
  QTabWidget* m_tabs;
  QLabel* m_cell_name;
  QLineEdit* m_formula;
  QLabel* m_status;
  QAction* m_save;
  QAction* m_save_as;
  QAction* m_undo;
  QAction* m_redo;
  QAction* m_insert_row;
  QAction* m_delete_rows;
  QAction* m_insert_column;
  QAction* m_delete_columns;
  QAction* m_bake;
  int m_untitled_count = 0;
};
