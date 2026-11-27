// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitPanels.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/QtUtils/TableExport.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"

namespace
{
QString CPUCoreName(PowerPC::CPUCore core)
{
  switch (core)
  {
  case PowerPC::CPUCore::Interpreter:
    return QObject::tr("Interpreter");
  case PowerPC::CPUCore::CachedInterpreter:
    return QObject::tr("Cached Interpreter");
  case PowerPC::CPUCore::JIT64:
    return QObject::tr("JIT x86-64");
  case PowerPC::CPUCore::JITARM64:
    return QObject::tr("JIT ARM64");
  }
  return QObject::tr("Unknown");
}

QString PlatformName(DiscIO::Platform platform)
{
  switch (platform)
  {
  case DiscIO::Platform::GameCubeDisc:
    return QObject::tr("GameCube disc");
  case DiscIO::Platform::Triforce:
    return QObject::tr("Triforce");
  case DiscIO::Platform::WiiDisc:
    return QObject::tr("Wii disc");
  case DiscIO::Platform::WiiWAD:
    return QObject::tr("Wii WAD");
  case DiscIO::Platform::ELFOrDOL:
    return QObject::tr("ELF / DOL");
  default:
    return QObject::tr("Unknown");
  }
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// TargetStatusWidget

TargetStatusWidget::TargetStatusWidget(QWidget* parent) : QFrame(parent)
{
  setObjectName(QStringLiteral("targetStatus"));

  m_icon = new QLabel;
  m_icon->setPixmap(IconSet::Get("cube", QColor(QStringLiteral("#8f7dff"))).pixmap(40, 40));
  m_state = new QLabel;
  m_state->setObjectName(QStringLiteral("targetStatusState"));
  m_title = new QLabel;
  m_cpu = new QLabel;
  m_backend = new QLabel;
  for (QLabel* label : {m_title, m_cpu, m_backend})
    label->setObjectName(QStringLiteral("targetStatusDetail"));

  auto* const layout = new QGridLayout(this);
  layout->setContentsMargins(10, 4, 14, 4);
  layout->setHorizontalSpacing(12);
  layout->setVerticalSpacing(0);
  layout->addWidget(m_icon, 0, 0, 4, 1, Qt::AlignVCenter);
  layout->addWidget(m_state, 0, 1);
  layout->addWidget(m_title, 1, 1);
  layout->addWidget(m_cpu, 2, 1);
  layout->addWidget(m_backend, 3, 1);

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, &TargetStatusWidget::Refresh);
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &TargetStatusWidget::Refresh);
  connect(Host::GetInstance(), &Host::RequestTitle, this, &TargetStatusWidget::Refresh);
  Refresh();
}

void TargetStatusWidget::Refresh()
{
  const Core::State state = Core::GetState(Core::System::GetInstance());

  QString state_text;
  QString dot_color;
  switch (state)
  {
  case Core::State::Running:
    state_text = tr("Running");
    dot_color = QStringLiteral("#3ddc84");
    break;
  case Core::State::Paused:
    state_text = tr("Paused");
    dot_color = QStringLiteral("#ffc857");
    break;
  case Core::State::Starting:
    state_text = tr("Starting");
    dot_color = QStringLiteral("#5fe3ff");
    break;
  case Core::State::Stopping:
    state_text = tr("Stopping");
    dot_color = QStringLiteral("#ff6b9d");
    break;
  case Core::State::Uninitialized:
  default:
    state_text = tr("Idle");
    dot_color = QStringLiteral("#746d98");
    break;
  }
  m_state->setText(QStringLiteral("<span style=\"color:%1\">&#9679;</span>&nbsp;%2")
                       .arg(dot_color, state_text.toHtmlEscaped()));

  const bool running = state != Core::State::Uninitialized;
  const std::string game_id = running ? SConfig::GetInstance().GetGameID() : std::string();
  m_title->setText(running && !game_id.empty() ?
                       tr("Title: %1").arg(QString::fromStdString(game_id)) :
                       tr("Title: (none)"));
  m_cpu->setText(tr("CPU: %1").arg(CPUCoreName(Config::Get(Config::MAIN_CPU_CORE))));
  m_backend->setText(
      tr("Video: %1").arg(QString::fromStdString(Config::Get(Config::MAIN_GFX_BACKEND))));
}

// ---------------------------------------------------------------------------------------------
// TargetToolsWidget

TargetToolsWidget::TargetToolsWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Target Tools"));
  setObjectName(QStringLiteral("targettools"));
  setWindowIcon(IconSet::Get("layer-group"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  // Not closable: View > Reset Editor Layout is the only way back, so keep it simple.
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  auto* const contents = new QWidget;
  m_layout = new QVBoxLayout(contents);
  m_layout->setContentsMargins(8, 8, 8, 8);
  m_layout->setSpacing(6);
  m_layout->addStretch();

  auto* const scroll = new QScrollArea;
  scroll->setObjectName(QStringLiteral("targetToolsScroll"));
  scroll->setWidget(contents);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  setWidget(scroll);
}

static QToolButton* MakeToolButton(std::string_view icon, const QString& text,
                                   const QString& tooltip)
{
  auto* const button = new QToolButton;
  button->setObjectName(QStringLiteral("targetToolButton"));
  button->setIcon(IconSet::Get(icon));
  button->setIconSize(QSize(20, 20));
  // Leading spaces give the label some breathing room from the icon.
  button->setText(QStringLiteral("  ") + text);
  button->setToolTip(tooltip);
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  button->setMinimumHeight(36);
  return button;
}

void TargetToolsWidget::AddTool(std::string_view icon, const QString& text, const QString& tooltip,
                                std::function<void()> action)
{
  QToolButton* const button = MakeToolButton(icon, text, tooltip);
  connect(button, &QToolButton::clicked, this, [action = std::move(action)] { action(); });
  m_layout->insertWidget(m_layout->count() - 1, button);
}

void TargetToolsWidget::AddToggle(std::string_view icon, const QString& text,
                                  const QString& tooltip, const Config::Info<bool>& setting)
{
  QToolButton* const button = MakeToolButton(icon, text, tooltip);
  button->setCheckable(true);
  button->setChecked(Config::Get(setting));
  connect(button, &QToolButton::toggled, this,
          [setting](bool checked) { Config::SetBaseOrCurrent(setting, checked); });
  connect(&Settings::Instance(), &Settings::ConfigChanged, button, [button, setting] {
    const QSignalBlocker blocker(button);
    button->setChecked(Config::Get(setting));
  });
  m_layout->insertWidget(m_layout->count() - 1, button);
}

// ---------------------------------------------------------------------------------------------
// PropertyInspectorWidget

PropertyInspectorWidget::PropertyInspectorWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Property Inspector"));
  setObjectName(QStringLiteral("propertyinspector"));
  setWindowIcon(IconSet::Get("table-list"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  // Not closable: View > Reset Editor Layout is the only way back, so keep it simple.
  setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

  m_selected = new QLabel;
  m_selected->setObjectName(QStringLiteral("inspectorSelected"));
  m_selected->setTextInteractionFlags(Qt::TextSelectableByMouse);

  m_table = new QTableWidget(0, 2);
  m_table->setHorizontalHeaderLabels({tr("Property"), tr("Value")});
  TableExport::Install(m_table);
  m_table->verticalHeader()->hide();
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setAlternatingRowColors(true);
  m_table->setWordWrap(false);

  auto* const contents = new QWidget;
  auto* const layout = new QVBoxLayout(contents);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(6);
  layout->addWidget(m_selected);
  layout->addWidget(m_table);
  setWidget(contents);

  SetGameFile(nullptr);
}

void PropertyInspectorWidget::AddRow(const QString& property, const QString& value)
{
  const int row = m_table->rowCount();
  m_table->insertRow(row);
  m_table->setItem(row, 0, new QTableWidgetItem(property));
  auto* const value_item = new QTableWidgetItem(value);
  value_item->setToolTip(value);
  m_table->setItem(row, 1, value_item);
}

void PropertyInspectorWidget::SetGameFile(const std::shared_ptr<const UICommon::GameFile>& game)
{
  m_table->setRowCount(0);

  if (!game)
  {
    m_selected->setText(tr("Selected: (none)"));
    return;
  }

  const QString name = QString::fromStdString(
      game->GetName(UICommon::GameFile::Variant::LongAndPossiblyCustom));
  m_selected->setText(tr("Selected: %1").arg(name));

  const auto str = [](const std::string& s) { return QString::fromStdString(s); };

  AddRow(tr("Name"), name);
  AddRow(tr("Game ID"), str(game->GetGameID()));
  if (game->GetTitleID() != 0)
    AddRow(tr("Title ID"), str(fmt::format("{:016x}", game->GetTitleID())));
  AddRow(tr("Maker"), str(game->GetMaker(UICommon::GameFile::Variant::LongAndPossiblyCustom)));
  AddRow(tr("Maker ID"), str(game->GetMakerID()));
  AddRow(tr("Platform"), PlatformName(game->GetPlatform()));
  AddRow(tr("Region"), str(DiscIO::GetName(game->GetRegion(), true)));
  AddRow(tr("Country"), str(DiscIO::GetName(game->GetCountry(), true)));
  AddRow(tr("Revision"), QString::number(game->GetRevision()));
  if (DiscIO::IsDisc(game->GetPlatform()))
    AddRow(tr("Disc"), QString::number(game->GetDiscNumber() + 1));
  AddRow(tr("Internal Name"), str(game->GetInternalName()));
  if (!game->GetApploaderDate().empty())
    AddRow(tr("Apploader Date"), str(game->GetApploaderDate()));
  AddRow(tr("File Format"), str(game->GetFileFormatName()));
  if (!game->GetCompressionMethod().empty())
    AddRow(tr("Compression"), str(game->GetCompressionMethod()));
  AddRow(tr("File Size"), str(UICommon::FormatSize(game->GetFileSize())));
  AddRow(tr("Path"), str(game->GetFilePath()));
}

namespace
{
QString DescribeFileType(const QFileInfo& info)
{
  if (info.isDir())
    return QObject::tr("Folder");

  const QString suffix = info.suffix().toLower();
  if (suffix == QStringLiteral("dol"))
    return QObject::tr("GameCube/Wii executable (DOL)");
  if (suffix == QStringLiteral("elf"))
    return QObject::tr("ELF executable");
  if (suffix == QStringLiteral("gcm") || suffix == QStringLiteral("iso"))
    return QObject::tr("Disc image");
  if (suffix == QStringLiteral("rvz") || suffix == QStringLiteral("gcz") ||
      suffix == QStringLiteral("wia") || suffix == QStringLiteral("ciso") ||
      suffix == QStringLiteral("wbfs"))
  {
    return QObject::tr("Compressed disc image");
  }
  if (suffix == QStringLiteral("c"))
    return QObject::tr("C source");
  if (suffix == QStringLiteral("cpp") || suffix == QStringLiteral("cc") ||
      suffix == QStringLiteral("cxx"))
  {
    return QObject::tr("C++ source");
  }
  if (suffix == QStringLiteral("h") || suffix == QStringLiteral("hpp"))
    return QObject::tr("Header");
  if (suffix == QStringLiteral("s"))
    return QObject::tr("Assembly source");
  if (suffix == QStringLiteral("ld"))
    return QObject::tr("Linker script");
  if (suffix == QStringLiteral("tpl"))
    return QObject::tr("Texture palette (TPL)");
  if (suffix == QStringLiteral("png") || suffix == QStringLiteral("bmp") ||
      suffix == QStringLiteral("jpg") || suffix == QStringLiteral("tga"))
  {
    return QObject::tr("Image");
  }
  if (suffix == QStringLiteral("wav") || suffix == QStringLiteral("dsp") ||
      suffix == QStringLiteral("adp") || suffix == QStringLiteral("ogg") ||
      suffix == QStringLiteral("mp3"))
  {
    return QObject::tr("Audio");
  }
  if (info.fileName() == QStringLiteral("Makefile") || suffix == QStringLiteral("mk"))
    return QObject::tr("Makefile");
  if (suffix == QStringLiteral("sh"))
    return QObject::tr("Shell script");
  if (suffix == QStringLiteral("ini"))
    return QObject::tr("Settings");
  if (suffix.isEmpty())
    return QObject::tr("File");
  return QObject::tr("%1 file").arg(suffix.toUpper());
}

u32 ReadBE32(const QByteArray& data, int offset)
{
  const auto* const p = reinterpret_cast<const unsigned char*>(data.constData()) + offset;
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

QString Hex32(u32 value)
{
  return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
}
}  // namespace

void PropertyInspectorWidget::SetProjectFile(const QString& path, const QString& project_root,
                                             bool is_run_target)
{
  m_table->setRowCount(0);

  const QFileInfo info(path);
  if (path.isEmpty() || !info.exists())
  {
    m_selected->setText(tr("Selected: (none)"));
    return;
  }

  m_selected->setText(tr("Selected: %1").arg(info.fileName()));
  AddRow(tr("Name"), info.fileName());
  AddRow(tr("Type"), DescribeFileType(info));
  AddRow(tr("Path"), QDir(project_root).relativeFilePath(info.absoluteFilePath()));

  if (info.isDir())
  {
    const QDir dir(info.absoluteFilePath());
    AddRow(tr("Folders"),
           QString::number(dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).count()));
    AddRow(tr("Files"), QString::number(dir.entryList(QDir::Files).count()));
  }
  else
  {
    AddRow(tr("File Size"), QStringLiteral("%1 (%2 bytes)")
                                .arg(QString::fromStdString(UICommon::FormatSize(info.size())))
                                .arg(QLocale().toString(info.size())));
  }
  AddRow(tr("Modified"), QLocale().toString(info.lastModified(), QLocale::ShortFormat));

  const QString suffix = info.suffix().toLower();
  if (suffix == QStringLiteral("dol") || suffix == QStringLiteral("elf"))
    AddRow(tr("Run Target"), is_run_target ? tr("Yes") : tr("No"));

  QFile file(info.absoluteFilePath());
  if (suffix == QStringLiteral("dol") && file.open(QIODevice::ReadOnly))
  {
    // DOL header: 7 text + 11 data section file offsets, load addresses and sizes, then the BSS
    // address and size and the entry point.
    const QByteArray header = file.read(0x100);
    if (header.size() == 0x100)
    {
      constexpr int TEXT_SECTIONS = 7;
      constexpr int DATA_SECTIONS = 11;
      u32 text_size = 0, data_size = 0;
      int text_count = 0, data_count = 0;
      for (int i = 0; i < TEXT_SECTIONS + DATA_SECTIONS; ++i)
      {
        const u32 size = ReadBE32(header, 0x90 + i * 4);
        if (size == 0)
          continue;
        if (i < TEXT_SECTIONS)
        {
          ++text_count;
          text_size += size;
        }
        else
        {
          ++data_count;
          data_size += size;
        }
      }
      AddRow(tr("Entry Point"), Hex32(ReadBE32(header, 0xE0)));
      AddRow(tr("Text Sections"),
             tr("%1 (%2)").arg(text_count).arg(
                 QString::fromStdString(UICommon::FormatSize(text_size))));
      AddRow(tr("Data Sections"),
             tr("%1 (%2)").arg(data_count).arg(
                 QString::fromStdString(UICommon::FormatSize(data_size))));
      AddRow(tr("BSS"), tr("%1, %2").arg(Hex32(ReadBE32(header, 0xD8)))
                            .arg(QString::fromStdString(
                                UICommon::FormatSize(ReadBE32(header, 0xDC)))));
    }
  }
  else if (suffix == QStringLiteral("elf") && file.open(QIODevice::ReadOnly))
  {
    // 32-bit big-endian PowerPC ELF: the entry point is at 0x18.
    const QByteArray header = file.read(0x34);
    if (header.size() == 0x34 && header.startsWith("\x7f" "ELF") && header[4] == 1 &&
        header[5] == 2)
    {
      AddRow(tr("Entry Point"), Hex32(ReadBE32(header, 0x18)));
    }
  }
}

// ---------------------------------------------------------------------------------------------
// DockTitleBar

DockTitleBar::DockTitleBar(QDockWidget* dock, std::string_view icon) : QWidget(dock), m_dock(dock)
{
  setObjectName(QStringLiteral("dockTitleBar"));
  setAttribute(Qt::WA_StyledBackground);

  auto* const icon_label = new QLabel;
  icon_label->setObjectName(QStringLiteral("dockTitleIcon"));
  icon_label->setPixmap(IconSet::Get(icon, QColor(QStringLiteral("#a99cff"))).pixmap(16, 16));

  m_title = new QLabel(dock->windowTitle());
  m_title->setObjectName(QStringLiteral("dockTitleText"));
  connect(dock, &QDockWidget::windowTitleChanged, m_title, &QLabel::setText);

  const auto make_button = [this](std::string_view button_icon, const QString& tooltip) {
    auto* const button = new QToolButton;
    button->setObjectName(QStringLiteral("dockTitleButton"));
    button->setIcon(IconSet::Get(button_icon));
    button->setIconSize(QSize(12, 12));
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
  };
  m_float_button = make_button("up-right-from-square", tr("Float / dock this panel"));
  m_close_button = make_button("xmark", tr("Close this panel"));
  connect(m_float_button, &QToolButton::clicked, dock,
          [dock] { dock->setFloating(!dock->isFloating()); });
  connect(m_close_button, &QToolButton::clicked, dock, &QDockWidget::close);
  connect(dock, &QDockWidget::featuresChanged, this, &DockTitleBar::UpdateButtons);
  connect(dock, &QDockWidget::topLevelChanged, this, &DockTitleBar::UpdateButtons);

  m_contents = new QWidget;
  auto* const outer_layout = new QHBoxLayout(this);
  outer_layout->setContentsMargins(0, 0, 0, 0);
  outer_layout->addWidget(m_contents);

  auto* const layout = new QHBoxLayout(m_contents);
  layout->setContentsMargins(10, 5, 6, 5);
  layout->setSpacing(8);
  layout->addWidget(icon_label);
  layout->addWidget(m_title);
  layout->addStretch();
  layout->addWidget(m_float_button);
  layout->addWidget(m_close_button);

  UpdateButtons();
}

void DockTitleBar::SetCompact(bool compact)
{
  if (compact == m_compact)
    return;
  m_compact = compact;
  m_contents->setVisible(!compact);
  if (compact)
  {
    setFixedHeight(0);
  }
  else
  {
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);
  }
}

void DockTitleBar::UpdateButtons()
{
  const auto features = m_dock->features();
  m_float_button->setVisible(features.testFlag(QDockWidget::DockWidgetFloatable));
  m_float_button->setIcon(
      IconSet::Get(m_dock->isFloating() ? "window-restore" : "up-right-from-square"));
  m_close_button->setVisible(features.testFlag(QDockWidget::DockWidgetClosable));
}
