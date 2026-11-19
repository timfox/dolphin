// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitBuildPanels.h"

#include <QAbstractFileIconProvider>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "DolphinQt/IconSet.h"

namespace
{
bool IsBootable(const QFileInfo& info)
{
  static const QStringList bootable = {
      QStringLiteral("dol"), QStringLiteral("elf"),  QStringLiteral("iso"),
      QStringLiteral("gcm"), QStringLiteral("rvz"),  QStringLiteral("gcz"),
      QStringLiteral("wia"), QStringLiteral("ciso"), QStringLiteral("wbfs"),
  };
  return info.isFile() && bootable.contains(info.suffix().toLower());
}

// File type icons for the project tree.
class ProjectFileIconProvider final : public QAbstractFileIconProvider
{
  // Folders are tinted like the devkit concept's tree, so they stand out from files.
  static QIcon FolderIcon() { return IconSet::Get("folder", QColor(QStringLiteral("#ffc857"))); }

public:
  QIcon icon(IconType type) const override
  {
    return type == Folder ? FolderIcon() : IconSet::Get("file");
  }

  QIcon icon(const QFileInfo& info) const override
  {
    if (info.isDir())
      return FolderIcon();

    const QString suffix = info.suffix().toLower();
    static const QStringList code = {QStringLiteral("c"),   QStringLiteral("cpp"),
                                     QStringLiteral("cc"),  QStringLiteral("h"),
                                     QStringLiteral("hpp"), QStringLiteral("s"),
                                     QStringLiteral("inc"), QStringLiteral("ld")};
    static const QStringList images = {QStringLiteral("png"), QStringLiteral("tpl"),
                                       QStringLiteral("bmp"), QStringLiteral("jpg"),
                                       QStringLiteral("tga"), QStringLiteral("tex")};
    static const QStringList audio = {QStringLiteral("wav"), QStringLiteral("dsp"),
                                      QStringLiteral("adp"), QStringLiteral("ogg"),
                                      QStringLiteral("mp3")};
    if (suffix == QStringLiteral("dol") || suffix == QStringLiteral("elf"))
      return IconSet::Get("circle-play", QColor(QStringLiteral("#7dffc7")));
    if (IsBootable(info))
      return IconSet::Get("compact-disc");
    if (code.contains(suffix))
      return IconSet::Get("file-code");
    if (images.contains(suffix))
      return IconSet::Get("file-image");
    if (audio.contains(suffix))
      return IconSet::Get("file-audio");
    if (info.fileName() == QStringLiteral("Makefile") || suffix == QStringLiteral("mk") ||
        suffix == QStringLiteral("ini") || suffix == QStringLiteral("txt") ||
        suffix == QStringLiteral("md"))
    {
      return IconSet::Get("file-lines");
    }
    return IconSet::Get("file");
  }
};

QPushButton* MakeBigButton(std::string_view icon, const QString& text)
{
  auto* const button = new QPushButton(IconSet::Get(icon), text);
  button->setIconSize(QSize(18, 18));
  button->setMinimumHeight(36);
  return button;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// ProjectWidget

ProjectWidget::ProjectWidget(DevkitProject* project, QWidget* parent)
    : QDockWidget(parent), m_project(project)
{
  setWindowTitle(tr("Project"));
  setObjectName(QStringLiteral("project"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  // Empty state: create or open a project.
  auto* const empty = new QWidget;
  auto* const empty_layout = new QVBoxLayout(empty);
  empty_layout->setContentsMargins(16, 16, 16, 16);
  empty_layout->setSpacing(10);
  auto* const hint = new QLabel(tr("No project open.\n\nCreate a GameCube homebrew project from a "
                                   "template, or open an existing project folder."));
  hint->setObjectName(QStringLiteral("projectHint"));
  hint->setWordWrap(true);
  hint->setAlignment(Qt::AlignCenter);
  QPushButton* const new_button = MakeBigButton("folder-plus", tr("New Project..."));
  QPushButton* const open_button = MakeBigButton("folder-open", tr("Open Project Folder..."));
  connect(new_button, &QPushButton::clicked, this, &ProjectWidget::NewProjectRequested);
  connect(open_button, &QPushButton::clicked, this, &ProjectWidget::OpenProjectRequested);
  empty_layout->addStretch();
  empty_layout->addWidget(hint);
  empty_layout->addSpacing(6);
  empty_layout->addWidget(new_button);
  empty_layout->addWidget(open_button);
  empty_layout->addStretch();

  // Open state: header + file tree.
  auto* const open_page = new QWidget;
  auto* const open_layout = new QVBoxLayout(open_page);
  open_layout->setContentsMargins(0, 0, 0, 0);
  open_layout->setSpacing(0);

  auto* const header = new QWidget;
  header->setObjectName(QStringLiteral("projectHeader"));
  auto* const header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(10, 6, 6, 6);
  m_name = new QLabel;
  m_name->setObjectName(QStringLiteral("projectName"));
  auto* const settings_button = new QToolButton;
  settings_button->setIcon(IconSet::Get("gear"));
  settings_button->setToolTip(tr("Project settings (build command, run target)"));
  settings_button->setAutoRaise(true);
  connect(settings_button, &QToolButton::clicked, this, &ProjectWidget::SettingsRequested);
  header_layout->addWidget(m_name, 1);
  header_layout->addWidget(settings_button);

  m_model = new QFileSystemModel(this);
  // QFileSystemModel doesn't take ownership of its icon provider.
  static ProjectFileIconProvider s_icon_provider;
  m_model->setIconProvider(&s_icon_provider);
  m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
  m_model->setReadOnly(true);

  m_tree = new QTreeView;
  m_tree->setObjectName(QStringLiteral("projectTree"));
  m_tree->setModel(m_model);
  for (int column = 1; column < m_model->columnCount(); ++column)
    m_tree->hideColumn(column);
  m_tree->setHeaderHidden(true);
  m_tree->setAnimated(true);
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_tree, &QTreeView::activated, this, &ProjectWidget::OnActivated);
  connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](const QModelIndex& current) {
            emit FileSelected(current.isValid() ? m_model->filePath(current) : QString());
          });
  connect(m_tree, &QTreeView::customContextMenuRequested, this, &ProjectWidget::ShowContextMenu);

  open_layout->addWidget(header);
  open_layout->addWidget(m_tree);

  m_stack = new QStackedWidget;
  m_stack->addWidget(empty);
  m_stack->addWidget(open_page);
  setWidget(m_stack);

  connect(m_project, &DevkitProject::ProjectChanged, this, &ProjectWidget::OnProjectChanged);
  OnProjectChanged();
}

void ProjectWidget::OnProjectChanged()
{
  if (!m_project->IsOpen())
  {
    m_stack->setCurrentIndex(0);
    return;
  }

  m_name->setText(m_project->GetName());
  m_name->setToolTip(m_project->GetPath());
  m_tree->setRootIndex(m_model->setRootPath(m_project->GetPath()));
  m_stack->setCurrentIndex(1);
}

void ProjectWidget::OnActivated(const QModelIndex& index)
{
  const QFileInfo info = m_model->fileInfo(index);
  if (info.isDir())
    return;

  if (IsBootable(info))
    emit BootRequested(info.absoluteFilePath());
  else
    emit OpenFileRequested(info.absoluteFilePath(), 0);
}

void ProjectWidget::ShowContextMenu(const QPoint& pos)
{
  const QModelIndex index = m_tree->indexAt(pos);
  const QFileInfo info = index.isValid() ? m_model->fileInfo(index) : QFileInfo(m_project->GetPath());

  QMenu menu(this);
  if (IsBootable(info))
  {
    menu.addAction(IconSet::Get("play"), tr("Boot in Game View"), this,
                   [this, info] { emit BootRequested(info.absoluteFilePath()); });
    const QString relative = QDir(m_project->GetPath()).relativeFilePath(info.absoluteFilePath());
    menu.addAction(IconSet::Get("location-dot"), tr("Set as Run Target"), this,
                   [this, relative] { m_project->SetRunTarget(relative); });
    menu.addSeparator();
  }
  if (info.isFile())
  {
    menu.addAction(IconSet::Get("file-code"), tr("Open in Editor"), this,
                   [this, info] { emit OpenFileRequested(info.absoluteFilePath(), 0); });
    menu.addAction(IconSet::Get("file-lines"), tr("Open with Default App"), this, [info] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
    });
  }
  const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
  menu.addAction(IconSet::Get("folder-open"), tr("Show in File Manager"), this,
                 [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
  menu.addSeparator();
  menu.addAction(IconSet::Get("gear"), tr("Project Settings..."), this,
                 &ProjectWidget::SettingsRequested);
  menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------------------------
// BuildOutputWidget

BuildOutputWidget::BuildOutputWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Output"));
  setObjectName(QStringLiteral("buildoutput"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  m_text = new QPlainTextEdit;
  m_text->setObjectName(QStringLiteral("buildOutputText"));
  m_text->setReadOnly(true);
  m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_text->setMaximumBlockCount(20000);
  m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_text->setPlaceholderText(tr("Build output appears here. Open a project and press Build."));
  setWidget(m_text);
}

void BuildOutputWidget::Clear()
{
  m_text->clear();
}

void BuildOutputWidget::AppendLine(const QString& line, bool is_stderr)
{
  BuildDiagnostic diagnostic;
  QString color;
  if (DevkitBuilder::ParseDiagnostic(line, &diagnostic))
  {
    color = diagnostic.severity == BuildDiagnostic::Severity::Error ? QStringLiteral("#ff6b9d") :
                                                                        QStringLiteral("#ffc857");
  }
  else if (is_stderr)
  {
    color = QStringLiteral("#d6b8ff");
  }

  if (color.isEmpty())
    m_text->appendPlainText(line);
  else
    m_text->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>")
                           .arg(color, line.toHtmlEscaped().replace(QLatin1Char(' '),
                                                                    QStringLiteral("&nbsp;"))));
}

void BuildOutputWidget::AppendStatus(const QString& text, bool success)
{
  m_text->appendHtml(QStringLiteral("<b style=\"color:%1\">%2</b>")
                         .arg(success ? QStringLiteral("#7dffc7") : QStringLiteral("#ff6b9d"),
                              text.toHtmlEscaped()));
}

// ---------------------------------------------------------------------------------------------
// DiagnosticsWidget

DiagnosticsWidget::DiagnosticsWidget(BuildDiagnostic::Severity severity, QWidget* parent)
    : QDockWidget(parent), m_severity(severity)
{
  const bool errors = severity == BuildDiagnostic::Severity::Error;
  setObjectName(errors ? QStringLiteral("builderrors") : QStringLiteral("buildwarnings"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  m_list = new QTreeWidget;
  m_list->setColumnCount(3);
  m_list->setHeaderLabels({tr("File"), tr("Line"), tr("Message")});
  m_list->setRootIsDecorated(false);
  m_list->setAlternatingRowColors(true);
  m_list->setUniformRowHeights(true);
  m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_list->header()->setStretchLastSection(true);
  connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
    const QString path = item->data(0, Qt::UserRole).toString();
    if (QFileInfo::exists(path))
      emit OpenFileRequested(path, item->data(1, Qt::UserRole).toInt());
  });
  setWidget(m_list);

  UpdateTitle();
}

void DiagnosticsWidget::Clear()
{
  m_list->clear();
  UpdateTitle();
}

void DiagnosticsWidget::Add(const BuildDiagnostic& diagnostic, const QString& project_dir)
{
  if (diagnostic.severity != m_severity)
    return;

  const QString absolute = QDir(project_dir).absoluteFilePath(diagnostic.file);
  auto* const item = new QTreeWidgetItem(
      {QDir(project_dir).relativeFilePath(absolute),
       diagnostic.line > 0 ? QString::number(diagnostic.line) : QString(), diagnostic.message});
  item->setData(0, Qt::UserRole, absolute);
  item->setData(1, Qt::UserRole, diagnostic.line);
  item->setToolTip(2, diagnostic.message);
  item->setIcon(0, m_severity == BuildDiagnostic::Severity::Error ?
                       IconSet::Get("circle-xmark", QColor(QStringLiteral("#ff6b9d"))) :
                       IconSet::Get("triangle-exclamation", QColor(QStringLiteral("#ffc857"))));
  m_list->addTopLevelItem(item);
  UpdateTitle();
}

void DiagnosticsWidget::UpdateTitle()
{
  const int count = m_list->topLevelItemCount();
  setWindowTitle(m_severity == BuildDiagnostic::Severity::Error ? tr("Errors (%1)").arg(count) :
                                                                  tr("Warnings (%1)").arg(count));
}

// ---------------------------------------------------------------------------------------------
// ProjectSettingsDialog

ProjectSettingsDialog::ProjectSettingsDialog(DevkitProject* project, QWidget* parent)
    : QDialog(parent), m_project(project)
{
  setWindowTitle(tr("Project Settings - %1").arg(project->GetName()));
  setMinimumWidth(560);

  m_build = new QLineEdit(project->GetBuildCommand());
  m_clean = new QLineEdit(project->GetCleanCommand());
  m_run_target = new QLineEdit(project->GetRunTarget());
  m_run_target->setPlaceholderText(tr("Auto-detect the newest .dol/.elf"));

  auto* const browse = new QToolButton;
  browse->setIcon(IconSet::Get("folder-open"));
  browse->setToolTip(tr("Choose the DOL/ELF to boot"));
  connect(browse, &QToolButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose Run Target"), m_project->GetPath(),
        tr("GameCube/Wii executables (*.dol *.elf);;All files (*)"));
    if (!file.isEmpty())
      m_run_target->setText(QDir(m_project->GetPath()).relativeFilePath(file));
  });
  m_editor = new QLineEdit(project->GetEditorCommand());
  m_editor->setPlaceholderText(tr("Auto: VS Code if installed, else the default app"));
  m_editor->setToolTip(
      tr("Command used to open source files and build errors. %f is replaced by the file and "
         "%l by the line, e.g. \"code -g %f:%l\" or \"subl %f:%l\"."));

  auto* const run_row = new QHBoxLayout;
  run_row->addWidget(m_run_target);
  run_row->addWidget(browse);

  auto* const form = new QFormLayout;
  form->addRow(tr("Build command:"), m_build);
  form->addRow(tr("Clean command:"), m_clean);
  form->addRow(tr("Run target:"), run_row);
  form->addRow(tr("Open files with:"), m_editor);

  auto* const note =
      new QLabel(tr("Commands run in the project folder through the system shell. Settings are "
                    "saved in %1 in the project folder.")
                     .arg(QString::fromLatin1(DevkitProject::SETTINGS_FILE_NAME)));
  note->setWordWrap(true);
  note->setObjectName(QStringLiteral("projectHint"));

  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    Save();
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* const layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(note);
  layout->addWidget(buttons);
}

void ProjectSettingsDialog::Save()
{
  m_project->SetBuildCommand(m_build->text().trimmed());
  m_project->SetCleanCommand(m_clean->text().trimmed());
  m_project->SetRunTarget(m_run_target->text().trimmed());
  m_project->SetEditorCommand(m_editor->text().trimmed());
}
