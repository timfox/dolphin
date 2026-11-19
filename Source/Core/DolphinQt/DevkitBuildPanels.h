// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QDockWidget>

#include "DolphinQt/DevkitProject.h"

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTreeView;
class QTreeWidget;

// "Project" dock: the open homebrew project's file tree, or buttons to create/open one.
class ProjectWidget final : public QDockWidget
{
  Q_OBJECT
public:
  ProjectWidget(DevkitProject* project, QWidget* parent = nullptr);

signals:
  void NewProjectRequested();
  void OpenProjectRequested();
  void SettingsRequested();
  // A bootable file (DOL/ELF/disc image) was activated in the tree.
  void BootRequested(const QString& path);
  // A non-bootable file was activated in the tree. |line| is 0 when there's no line to jump to.
  void OpenFileRequested(const QString& path, int line);
  // The selected file or folder in the tree changed. Empty when nothing is selected.
  void FileSelected(const QString& path);

private:
  void OnProjectChanged();
  void OnActivated(const QModelIndex& index);
  void ShowContextMenu(const QPoint& pos);

  DevkitProject* m_project;
  QStackedWidget* m_stack;
  QLabel* m_name;
  QTreeView* m_tree;
  QFileSystemModel* m_model;
};

// "Output" dock: raw build output, with warnings and errors highlighted.
class BuildOutputWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit BuildOutputWidget(QWidget* parent = nullptr);

  void Clear();
  void AppendLine(const QString& line, bool is_stderr);
  void AppendStatus(const QString& text, bool success);

private:
  QPlainTextEdit* m_text;
};

// "Warnings" / "Errors" dock: a list of parsed diagnostics. Activating one opens the file at the
// diagnostic's line.
class DiagnosticsWidget final : public QDockWidget
{
  Q_OBJECT
public:
  DiagnosticsWidget(BuildDiagnostic::Severity severity, QWidget* parent = nullptr);

  void Clear();
  void Add(const BuildDiagnostic& diagnostic, const QString& project_dir);

signals:
  void OpenFileRequested(const QString& path, int line);

private:
  void UpdateTitle();

  BuildDiagnostic::Severity m_severity;
  QTreeWidget* m_list;
};

// Edits a project's build command, clean command and run target.
class ProjectSettingsDialog final : public QDialog
{
  Q_OBJECT
public:
  ProjectSettingsDialog(DevkitProject* project, QWidget* parent = nullptr);

private:
  void Save();

  DevkitProject* m_project;
  QLineEdit* m_build;
  QLineEdit* m_clean;
  QLineEdit* m_run_target;
  QLineEdit* m_editor;
};
