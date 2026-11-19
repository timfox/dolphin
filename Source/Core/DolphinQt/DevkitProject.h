// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

// A homebrew project folder opened in the devkit editor, set like a Maya project: the project
// folder is the workspace every relative path resolves against, and its ".dolphin-devkit.ini"
// (the project definition, like Maya's workspace.mel) holds the project's name, its folder rules
// and its build settings, so they can be committed with the project's source.
class DevkitProject final : public QObject
{
  Q_OBJECT
public:
  explicit DevkitProject(QObject* parent = nullptr);

  static constexpr const char* SETTINGS_FILE_NAME = ".dolphin-devkit.ini";

  // A folder the project definition maps to a role, like Maya's file rules.
  struct FolderRule
  {
    const char* key;          // [folders] key in the project definition
    const char* default_dir;  // relative to the project folder
    QString label;
    QString description;
  };
  static const std::vector<FolderRule>& GetFolderRules();

  bool IsOpen() const { return !m_path.isEmpty(); }
  const QString& GetPath() const { return m_path; }
  // The project's display name: the name in the project definition, else the folder's name.
  QString GetName() const;
  void SetName(const QString& name);
  bool HasDefinition() const;

  // Opens |path| as the current project (remembered across restarts and added to the recent
  // projects). Returns false if it isn't a directory.
  bool Open(const QString& path);
  // Maya's "Set Project": opens |path| and, if it has no project definition yet, writes one with
  // the default folder rules and build settings.
  bool SetProject(const QString& path);
  void Close();
  // Re-opens the project that was open when Dolphin last exited, if it still exists.
  void RestoreLastProject();

  // Most recent first; folders that no longer exist are left out.
  static QStringList GetRecentProjects();
  void ClearRecentProjects();

  // A folder rule's folder, relative to the project (as set) or absolute.
  QString GetFolder(const char* key) const;
  QString GetFolderPath(const char* key) const;
  void SetFolder(const char* key, const QString& dir);

  // If |file| (e.g. a DOL/ELF being booted) lives inside a homebrew project, returns that
  // project's root folder: the nearest parent with a .dolphin-devkit.ini, or failing that a
  // Makefile. Empty if it doesn't look like part of a project.
  static QString FindProjectRoot(const QString& file);

  // Creates a minimal devkitPPC/libogc GameCube project (Makefile + source/main.c) in |path|,
  // which must be empty or not exist yet, and opens it.
  bool CreateFromTemplate(const QString& path, QString* error);

  QString GetBuildCommand() const;
  QString GetCleanCommand() const;
  // Path to the DOL/ELF that Run boots, relative to the project root. Empty means auto-detect.
  QString GetRunTarget() const;
  void SetBuildCommand(const QString& command);
  void SetCleanCommand(const QString& command);
  void SetRunTarget(const QString& target);
  // Command that opens a file in the user's editor, with %f for the file and %l for the line.
  // Empty means auto-detect.
  QString GetEditorCommand() const;
  void SetEditorCommand(const QString& command);

  // Opens |path| in the configured editor, at |line| when the editor supports it (0 = no line).
  // Falls back to the system's default app for the file.
  void OpenInEditor(const QString& path, int line) const;

  // The run target as an absolute path: the configured one, or else the most recently modified
  // .dol/.elf in the project. Empty if nothing has been built yet.
  QString ResolveRunTarget() const;

signals:
  void ProjectChanged();
  void RecentProjectsChanged();

private:
  QString ReadSetting(const QString& key, const QString& fallback) const;
  void WriteSetting(const QString& key, const QString& value);

  QString m_path;
};

// One compiler/linker diagnostic parsed from build output.
struct BuildDiagnostic
{
  enum class Severity
  {
    Warning,
    Error,
  };

  Severity severity;
  QString file;  // As printed by the tool; may be relative to the project directory.
  int line = 0;
  int column = 0;
  QString message;
};

// Runs a project's build or clean command and parses GCC/Clang-style diagnostics out of its output.
class DevkitBuilder final : public QObject
{
  Q_OBJECT
public:
  explicit DevkitBuilder(QObject* parent = nullptr);
  ~DevkitBuilder() override;

  bool IsRunning() const;
  // Runs |command| through the platform shell in |working_dir|.
  void Run(const QString& command, const QString& working_dir);
  void Cancel();

  // Parses one line of tool output; returns true and fills |out| if it is a warning or error.
  static bool ParseDiagnostic(const QString& line, BuildDiagnostic* out);

signals:
  void Started(const QString& command);
  void OutputLine(const QString& line, bool is_stderr);
  void DiagnosticFound(const BuildDiagnostic& diagnostic);
  void Finished(bool success, int warnings, int errors, qint64 elapsed_ms);

private:
  void ReadOutput(bool is_stderr);
  void HandleLine(const QString& line, bool is_stderr);

  QProcess* m_process = nullptr;
  QByteArray m_stdout_buffer;
  QByteArray m_stderr_buffer;
  QElapsedTimer m_timer;
  int m_warnings = 0;
  int m_errors = 0;
};
