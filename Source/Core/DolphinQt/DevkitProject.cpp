// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitProject.h"

#include <cstdlib>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>

#include "DolphinQt/Settings.h"

namespace
{
const QString LAST_PROJECT_KEY = QStringLiteral("devkit/lastProject");

// A small devkitPPC/libogc project based on devkitPro's standard GameCube template.
constexpr const char TEMPLATE_MAKEFILE[] = R"MAKE(#---------------------------------------------------------------------------------
# GameCube homebrew project created by the Dolphin devkit editor.
# Requires devkitPro with devkitPPC and libogc: https://devkitpro.org
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/gamecube_rules

TARGET   := $(notdir $(CURDIR))
BUILD    := build
SOURCES  := source
DATA     := data
INCLUDES := include

CFLAGS   = -g -O2 -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)
LDFLAGS  = -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS     := -logc -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                 $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES_BIN     := $(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
)MAKE";

constexpr const char TEMPLATE_MAIN[] = R"MAIN(#include <stdio.h>
#include <stdlib.h>

#include <gccore.h>

static void* s_framebuffer = NULL;
static GXRModeObj* s_rmode = NULL;

int main(int argc, char** argv)
{
  VIDEO_Init();
  PAD_Init();

  s_rmode = VIDEO_GetPreferredMode(NULL);
  s_framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
  console_init(s_framebuffer, 20, 20, s_rmode->fbWidth, s_rmode->xfbHeight,
               s_rmode->fbWidth * VI_DISPLAY_PIX_SZ);

  VIDEO_Configure(s_rmode);
  VIDEO_SetNextFramebuffer(s_framebuffer);
  VIDEO_SetBlack(false);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  if (s_rmode->viTVMode & VI_NON_INTERLACE)
    VIDEO_WaitVSync();

  printf("\x1b[2;0H");
  printf("Hello from the Dolphin devkit!\n\n");
  printf("Edit source/main.c, then press Run to rebuild and boot.\n");

  while (1)
  {
    PAD_ScanPads();
    if (PAD_ButtonsDown(0) & PAD_BUTTON_START)
      exit(0);
    VIDEO_WaitVSync();
  }

  return 0;
}
)MAIN";

constexpr const char TEMPLATE_GITIGNORE[] = "build/\n*.elf\n*.dol\n*.map\n";
}  // namespace

// ---------------------------------------------------------------------------------------------
// DevkitProject

DevkitProject::DevkitProject(QObject* parent) : QObject(parent)
{
}

QString DevkitProject::GetName() const
{
  return QFileInfo(m_path).fileName();
}

bool DevkitProject::Open(const QString& path)
{
  const QFileInfo info(path);
  if (!info.isDir())
    return false;

  m_path = info.canonicalFilePath();
  Settings::GetQSettings().setValue(LAST_PROJECT_KEY, m_path);
  emit ProjectChanged();
  return true;
}

void DevkitProject::Close()
{
  if (!IsOpen())
    return;

  m_path.clear();
  Settings::GetQSettings().remove(LAST_PROJECT_KEY);
  emit ProjectChanged();
}

QString DevkitProject::FindProjectRoot(const QString& file)
{
  // Build outputs usually sit in the project root or one or two levels below it (e.g. build/).
  constexpr int MAX_LEVELS = 4;

  QDir dir = QFileInfo(file).absoluteDir();
  QString makefile_root;
  for (int level = 0; level < MAX_LEVELS; ++level)
  {
    if (dir.exists(QString::fromLatin1(SETTINGS_FILE_NAME)))
      return dir.absolutePath();
    if (makefile_root.isEmpty() &&
        (dir.exists(QStringLiteral("Makefile")) || dir.exists(QStringLiteral("makefile"))))
    {
      makefile_root = dir.absolutePath();
    }
    if (!dir.cdUp())
      break;
  }
  return makefile_root;
}

void DevkitProject::RestoreLastProject()
{
  const QString last = Settings::GetQSettings().value(LAST_PROJECT_KEY).toString();
  if (!last.isEmpty() && QFileInfo(last).isDir())
    Open(last);
}

bool DevkitProject::CreateFromTemplate(const QString& path, QString* error)
{
  QDir dir(path);
  if (dir.exists() && !dir.isEmpty())
  {
    *error = tr("The folder \"%1\" isn't empty. Choose a new or empty folder.").arg(path);
    return false;
  }
  if (!dir.mkpath(QStringLiteral("source")) || !dir.mkpath(QStringLiteral("include")) ||
      !dir.mkpath(QStringLiteral("data")))
  {
    *error = tr("Couldn't create the project folders in \"%1\".").arg(path);
    return false;
  }

  const auto write = [&dir, error](const QString& name, const char* contents) {
    QFile file(dir.filePath(name));
    if (!file.open(QFile::WriteOnly | QFile::Truncate) || file.write(contents) < 0)
    {
      *error = tr("Couldn't write \"%1\".").arg(file.fileName());
      return false;
    }
    return true;
  };

  if (!write(QStringLiteral("Makefile"), TEMPLATE_MAKEFILE) ||
      !write(QStringLiteral("source/main.c"), TEMPLATE_MAIN) ||
      !write(QStringLiteral(".gitignore"), TEMPLATE_GITIGNORE))
  {
    return false;
  }

  if (!Open(path))
  {
    *error = tr("Couldn't open \"%1\".").arg(path);
    return false;
  }
  // Write the defaults out so they're visible and editable in the project folder.
  SetBuildCommand(GetBuildCommand());
  SetCleanCommand(GetCleanCommand());
  return true;
}

QString DevkitProject::ReadSetting(const QString& key, const QString& fallback) const
{
  if (!IsOpen())
    return fallback;
  QSettings settings(QDir(m_path).filePath(QString::fromLatin1(SETTINGS_FILE_NAME)),
                     QSettings::IniFormat);
  return settings.value(key, fallback).toString();
}

void DevkitProject::WriteSetting(const QString& key, const QString& value)
{
  if (!IsOpen())
    return;
  QSettings settings(QDir(m_path).filePath(QString::fromLatin1(SETTINGS_FILE_NAME)),
                     QSettings::IniFormat);
  settings.setValue(key, value);
}

QString DevkitProject::GetBuildCommand() const
{
  return ReadSetting(QStringLiteral("build/command"), QStringLiteral("make"));
}

QString DevkitProject::GetCleanCommand() const
{
  return ReadSetting(QStringLiteral("build/clean"), QStringLiteral("make clean"));
}

QString DevkitProject::GetRunTarget() const
{
  return ReadSetting(QStringLiteral("run/target"), QString());
}

void DevkitProject::SetBuildCommand(const QString& command)
{
  WriteSetting(QStringLiteral("build/command"), command);
}

void DevkitProject::SetCleanCommand(const QString& command)
{
  WriteSetting(QStringLiteral("build/clean"), command);
}

void DevkitProject::SetRunTarget(const QString& target)
{
  WriteSetting(QStringLiteral("run/target"), target);
}

QString DevkitProject::ResolveRunTarget() const
{
  if (!IsOpen())
    return {};

  const QDir root(m_path);
  const QString configured = GetRunTarget();
  if (!configured.isEmpty())
  {
    const QString absolute = root.absoluteFilePath(configured);
    return QFileInfo::exists(absolute) ? absolute : QString();
  }

  // Prefer a DOL over an ELF built at the same time, then the newest.
  QFileInfo best;
  QDirIterator it(m_path, {QStringLiteral("*.dol"), QStringLiteral("*.elf")}, QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext())
  {
    const QFileInfo candidate(it.next());
    // Don't descend into anything deeper than a few levels (e.g. vendored SDKs).
    if (root.relativeFilePath(candidate.absoluteFilePath()).count(QLatin1Char('/')) > 3)
      continue;

    if (!best.exists())
    {
      best = candidate;
      continue;
    }

    const qint64 delta = candidate.lastModified().msecsTo(best.lastModified());
    const bool newer = delta < -2000;
    const bool same_time_but_dol = std::abs(delta) <= 2000 &&
                                   candidate.suffix().compare(QStringLiteral("dol"),
                                                              Qt::CaseInsensitive) == 0;
    if (newer || same_time_but_dol)
      best = candidate;
  }
  return best.exists() ? best.absoluteFilePath() : QString();
}

// ---------------------------------------------------------------------------------------------
// DevkitBuilder

DevkitBuilder::DevkitBuilder(QObject* parent) : QObject(parent)
{
}

DevkitBuilder::~DevkitBuilder()
{
  if (m_process)
  {
    m_process->disconnect(this);
    m_process->kill();
    m_process->waitForFinished(2000);
  }
}

bool DevkitBuilder::IsRunning() const
{
  return m_process != nullptr;
}

void DevkitBuilder::Run(const QString& command, const QString& working_dir)
{
  if (IsRunning())
    return;

  m_warnings = 0;
  m_errors = 0;
  m_stdout_buffer.clear();
  m_stderr_buffer.clear();

  m_process = new QProcess(this);
  m_process->setWorkingDirectory(working_dir);
  connect(m_process, &QProcess::readyReadStandardOutput, this, [this] { ReadOutput(false); });
  connect(m_process, &QProcess::readyReadStandardError, this, [this] { ReadOutput(true); });

  const auto finish = [this](bool success) {
    // Flush any partial last lines.
    if (!m_stdout_buffer.isEmpty())
      HandleLine(QString::fromLocal8Bit(m_stdout_buffer), false);
    if (!m_stderr_buffer.isEmpty())
      HandleLine(QString::fromLocal8Bit(m_stderr_buffer), true);
    m_stdout_buffer.clear();
    m_stderr_buffer.clear();

    m_process->deleteLater();
    m_process = nullptr;
    emit Finished(success && m_errors == 0, m_warnings, m_errors, m_timer.elapsed());
  };

  connect(m_process, &QProcess::finished, this,
          [this, finish](int exit_code, QProcess::ExitStatus status) {
            ReadOutput(false);
            ReadOutput(true);
            finish(status == QProcess::NormalExit && exit_code == 0);
          });
  connect(m_process, &QProcess::errorOccurred, this, [this, finish](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart)
      return;
    HandleLine(tr("Couldn't start the build: %1").arg(m_process->errorString()), true);
    finish(false);
  });

  emit Started(command);
  m_timer.start();

#ifdef _WIN32
  m_process->setProgram(QStringLiteral("cmd.exe"));
  m_process->setArguments({QStringLiteral("/C"), command});
#else
  m_process->setProgram(QStringLiteral("/bin/sh"));
  m_process->setArguments({QStringLiteral("-c"), command});
#endif
  m_process->start();
}

void DevkitBuilder::Cancel()
{
  if (m_process)
    m_process->kill();
}

void DevkitBuilder::ReadOutput(bool is_stderr)
{
  if (!m_process)
    return;

  QByteArray& buffer = is_stderr ? m_stderr_buffer : m_stdout_buffer;
  buffer.append(is_stderr ? m_process->readAllStandardError() :
                            m_process->readAllStandardOutput());

  qsizetype newline;
  while ((newline = buffer.indexOf('\n')) >= 0)
  {
    QString line = QString::fromLocal8Bit(buffer.left(newline));
    if (line.endsWith(QLatin1Char('\r')))
      line.chop(1);
    buffer.remove(0, newline + 1);
    HandleLine(line, is_stderr);
  }
}

void DevkitBuilder::HandleLine(const QString& line, bool is_stderr)
{
  emit OutputLine(line, is_stderr);

  BuildDiagnostic diagnostic;
  if (!ParseDiagnostic(line, &diagnostic))
    return;

  if (diagnostic.severity == BuildDiagnostic::Severity::Error)
    ++m_errors;
  else
    ++m_warnings;
  emit DiagnosticFound(diagnostic);
}

bool DevkitBuilder::ParseDiagnostic(const QString& line, BuildDiagnostic* out)
{
  // GCC/Clang: "path/file.c:12:5: warning: message" (column optional). The file part may contain
  // a Windows drive letter, hence the non-greedy match.
  static const QRegularExpression gcc_style(QStringLiteral(
      R"(^\s*(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning):\s*(.*)$)"));
  // GNU ld: "path/file.o: in function `main': ... undefined reference ..." has no line number;
  // treat "undefined reference" lines as errors against the object file.
  static const QRegularExpression ld_style(
      QStringLiteral(R"(^\s*(.+?):(?:(\d+):)?\s*(undefined reference to .*)$)"));

  if (const auto match = gcc_style.match(line); match.hasMatch())
  {
    const QString kind = match.captured(4);
    out->severity = kind == QStringLiteral("warning") ? BuildDiagnostic::Severity::Warning :
                                                         BuildDiagnostic::Severity::Error;
    out->file = match.captured(1);
    out->line = match.captured(2).toInt();
    out->column = match.captured(3).toInt();
    out->message = match.captured(5).trimmed();
    return true;
  }

  if (const auto match = ld_style.match(line); match.hasMatch())
  {
    out->severity = BuildDiagnostic::Severity::Error;
    out->file = match.captured(1);
    out->line = match.captured(2).toInt();
    out->column = 0;
    out->message = match.captured(3).trimmed();
    return true;
  }

  return false;
}
