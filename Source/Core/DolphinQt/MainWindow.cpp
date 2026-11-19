// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDockWidget>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMimeData>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <fmt/format.h>

#include <future>
#include <optional>
#include <set>
#include <utility>
#include <variant>

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
#include <signal.h>

#include "QtUtils/SignalDaemon.h"
#endif

#ifndef _WIN32
#include <qpa/qplatformnativeinterface.h>
#endif

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/Version.h"
#include "Common/WindowSystemInfo.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CommonTitles.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/NANDImporter.h"
#include "DiscIO/RiivolutionPatcher.h"

#include "DolphinQt/AboutDialog.h"
#include "DolphinQt/Achievements/AchievementsWindow.h"
#include "DolphinQt/CheatsManager.h"
#include "DolphinQt/Config/FreeLookWindow.h"
#include "DolphinQt/Config/LogConfigWidget.h"
#include "DolphinQt/Config/LogWidget.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Config/SettingsWindow.h"
#include "DolphinQt/Debugger/AssemblerWidget.h"
#include "DolphinQt/Debugger/BreakpointWidget.h"
#include "DolphinQt/Debugger/CodeViewWidget.h"
#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/Debugger/JITWidget.h"
#include "DolphinQt/Debugger/MemoryAnalyzer.h"
#include "DolphinQt/Debugger/MemoryMapWidget.h"
#include "DolphinQt/Debugger/MemoryUsageWidget.h"
#include "DolphinQt/Debugger/MemoryWidget.h"
#include "DolphinQt/Debugger/NetworkWidget.h"
#include "DolphinQt/Debugger/RegisterWidget.h"
#include "DolphinQt/Debugger/ThreadWidget.h"
#include "DolphinQt/Debugger/WatchWidget.h"
#include "DolphinQt/DiscordHandler.h"
#include "DolphinQt/EmulatedUSB/LogitechMicWindow.h"
#include "DolphinQt/EmulatedUSB/WiiSpeakWindow.h"
#include "DolphinQt/FIFO/FIFOPlayerWindow.h"
#include "DolphinQt/GCMemcardManager.h"
#include "DolphinQt/GameCount.h"
#include "DolphinQt/GameList/GameList.h"
#include "DolphinQt/GameViewWidget.h"
#include "DolphinQt/DevkitBuildPanels.h"
#include "DolphinQt/DevkitPanels.h"
#include "DolphinQt/DevkitRenderProfiler.h"
#include "DolphinQt/DevkitPackager.h"
#include "DolphinQt/DevkitProject.h"
#include "DolphinQt/DevkitStatusBar.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/HotkeyScheduler.h"
#include "DolphinQt/InfinityBase/InfinityBaseWindow.h"
#include "DolphinQt/MenuBar.h"
#include "DolphinQt/NKitWarningDialog.h"
#include "DolphinQt/NetPlay/NetPlayBrowser.h"
#include "DolphinQt/NetPlay/NetPlayDialog.h"
#include "DolphinQt/NetPlay/NetPlaySetupDialog.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/FileOpenEventFilter.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/QtUtils/WindowActivationEventFilter.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/ResourcePackManager.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/RiivolutionBootWidget.h"
#include "DolphinQt/SearchBar.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/SkylanderPortal/SkylanderPortalWindow.h"
#include "DolphinQt/TAS/GBATASInputWindow.h"
#include "DolphinQt/TAS/GCTASInputWindow.h"
#include "DolphinQt/TAS/WiiTASInputWindow.h"
#include "DolphinQt/ToolBar.h"
#include "DolphinQt/WiiUpdate.h"

#include "UICommon/DiscordPresence.h"
#include "UICommon/GameFile.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/ResourcePack.h"

#include "UICommon/UICommon.h"

#include "VideoCommon/NetPlayChatUI.h"

#ifdef HAVE_XRANDR
#include "UICommon/X11Utils.h"
// This #define within X11/X.h conflicts with our WiimoteSource enum.
#undef None
#endif

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
void MainWindow::OnSignal()
{
  close();
}

static void InstallSignalHandler()
{
  struct sigaction sa;
  sa.sa_handler = &SignalDaemon::HandleInterrupt;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}
#endif

static WindowSystemType GetWindowSystemType()
{
  // Determine WSI type based on Qt platform.
  QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("windows"))
    return WindowSystemType::Windows;
  else if (platform_name == QStringLiteral("cocoa"))
    return WindowSystemType::MacOS;
  else if (platform_name == QStringLiteral("xcb"))
    return WindowSystemType::X11;
  else if (platform_name == QStringLiteral("wayland"))
    return WindowSystemType::Wayland;
  else if (platform_name == QStringLiteral("haiku"))
    return WindowSystemType::Haiku;

  ModalMessageBox::critical(
      nullptr, QStringLiteral("Error"),
      QString::asprintf("Unknown Qt platform: %s", platform_name.toStdString().c_str()));
  return WindowSystemType::Headless;
}

static WindowSystemInfo GetWindowSystemInfo(QWindow* window)
{
  WindowSystemInfo wsi;
  wsi.type = GetWindowSystemType();

  // Our Win32 Qt external doesn't have the private API.
#if defined(WIN32) || defined(__APPLE__) || defined(__HAIKU__)
  wsi.render_window = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
  wsi.render_surface = wsi.render_window;
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  wsi.display_connection = pni->nativeResourceForWindow("display", window);
  if (wsi.type == WindowSystemType::Wayland)
    wsi.render_window = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
  else
    wsi.render_window = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
  wsi.render_surface = wsi.render_window;
#endif
  wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

  return wsi;
}

static std::vector<std::string> StringListToStdVector(const QStringList& list)
{
  std::vector<std::string> result;
  result.reserve(list.size());

  for (const QString& s : list)
    result.push_back(s.toStdString());

  return result;
}

MainWindow::MainWindow(Core::System& system, std::unique_ptr<BootParameters> boot_parameters,
                       const std::string& movie_path)
    : QMainWindow(nullptr), m_system(system)
{
  setWindowTitle(QStringLiteral("GameCube"));
  setWindowIcon(Resources::GetAppIcon());
  setUnifiedTitleAndToolBarOnMac(true);
  setAcceptDrops(true);
  setAttribute(Qt::WA_NativeWindow);

  CreateComponents();

  ConnectGameList();
  ConnectHost();
  ConnectToolBar();
  ConnectRenderWidget();
  ConnectStack();
  ConnectMenuBar();

  QSettings& settings = Settings::GetQSettings();
  restoreGeometry(settings.value(QStringLiteral("mainwindow/geometry")).toByteArray());
  RestoreLayoutState();
  // Batch mode normally only shows the render window, but in the editor layout the game is drawn
  // inside the main window, so the editor has to be shown for the game to be visible at all.
  if (!Settings::Instance().IsBatchModeEnabled() || m_editor_layout_active)
  {
    show();
  }

  InitControllers();
  ConnectHotkeys();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this](Qt::ColorScheme colorScheme) {
            Settings::Instance().ApplyStyle();
            if (m_skylander_window)
              m_skylander_window->RefreshList();
          });
#endif

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_game_list, &GameList::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  InitCoreCallbacks();

  NetPlayInit();

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager::GetInstance().Init(reinterpret_cast<void*>(winId()));
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    Settings::Instance().SetDebugModeEnabled(false);
  // This needs to trigger on both RA_HARDCORE_ENABLED and RA_ENABLED
  m_config_changed_callback_id = Config::AddConfigChangedCallback(
      [this] { QueueOnObject(this, [this] { this->OnHardcoreChanged(); }); });
  // If hardcore is enabled when the emulator starts, make sure it turns off what it needs to
  if (Config::Get(Config::RA_HARDCORE_ENABLED))
    OnHardcoreChanged();
#endif  // USE_RETRO_ACHIEVEMENTS

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
  auto* daemon = new SignalDaemon(this);

  connect(daemon, &SignalDaemon::InterruptReceived, this, &MainWindow::OnSignal);

  InstallSignalHandler();
#endif

  if (boot_parameters)
  {
    m_pending_boot = std::move(boot_parameters);

    if (!movie_path.empty())
    {
      std::optional<std::string> savestate_path;
      if (m_system.GetMovie().PlayInput(movie_path, &savestate_path))
      {
        m_pending_boot->boot_session_data.SetSavestateData(std::move(savestate_path),
                                                           DeleteSavestateAfterBoot::No);
        emit RecordingStatusChanged(true);
      }
    }
  }

  m_state_slot =
      std::clamp(Settings::Instance().GetStateSlot(), 1, static_cast<int>(State::NUM_STATES));

  m_render_widget_geometry = settings.value(QStringLiteral("renderwidget/geometry")).toByteArray();

  // Restoring of window states can sometimes go wrong, resulting in widgets being visible when they
  // shouldn't be so we have to reapply all our rules afterwards.
  Settings::Instance().RefreshWidgetVisibility();

  if (!ResourcePack::Init())
  {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("Error occurred while loading some texture packs"));
  }

  for (auto& pack : ResourcePack::GetPacks())
  {
    if (!pack.IsValid())
    {
      ModalMessageBox::critical(this, tr("Error"),
                                tr("Invalid Pack %1 provided: %2")
                                    .arg(QString::fromStdString(pack.GetPath()))
                                    .arg(QString::fromStdString(pack.GetError())));
      return;
    }
  }

  Host::GetInstance()->SetMainWindowHandle(reinterpret_cast<void*>(winId()));

  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

MainWindow::~MainWindow()
{
  // Shut down NetPlay first to avoid race condition segfault
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();

#ifdef USE_RETRO_ACHIEVEMENTS
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
  AchievementManager::GetInstance().Shutdown();
#endif  // USE_RETRO_ACHIEVEMENTS

  delete m_render_widget;
  delete m_netplay_dialog;

  for (int i = 0; i < 4; i++)
  {
    delete m_gc_tas_input_windows[i];
    delete m_gba_tas_input_windows[i];
    delete m_wii_tas_input_windows[i];
  }

  ShutdownControllers();

  QSettings& settings = Settings::GetQSettings();

  if (!Settings::Instance().IsBatchModeEnabled())
  {
    SaveLayoutState();
    settings.setValue(QStringLiteral("mainwindow/geometry"), saveGeometry());
  }

  settings.setValue(QStringLiteral("renderwidget/geometry"), m_render_widget_geometry);

  Config::Save();
}

WindowSystemInfo MainWindow::GetWindowSystemInfo() const
{
  return ::GetWindowSystemInfo(m_render_widget->windowHandle());
}

void MainWindow::InitControllers()
{
  if (g_controller_interface.IsInit())
    return;

  UICommon::InitControllers(::GetWindowSystemInfo(windowHandle()));

  m_hotkey_scheduler = new HotkeyScheduler();
  m_hotkey_scheduler->Start();

  // Defaults won't work reliably without loading and saving the config first

  Wiimote::LoadConfig();
  Wiimote::GetConfig()->SaveConfig();

  Pad::LoadConfig();
  Pad::GetConfig()->SaveConfig();

  Pad::LoadGBAConfig();
  Pad::GetGBAConfig()->SaveConfig();

  Keyboard::LoadConfig();
  Keyboard::GetConfig()->SaveConfig();

  FreeLook::LoadInputConfig();
  FreeLook::GetInputConfig()->SaveConfig();
}

void MainWindow::ShutdownControllers()
{
  m_hotkey_scheduler->Stop();

  Settings::Instance().UnregisterDevicesChangedCallback();

  UICommon::ShutdownControllers();

  m_hotkey_scheduler->deleteLater();
}

void MainWindow::InitCoreCallbacks()
{
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Uninitialized)
      OnStopComplete();

    if (state == Core::State::Running && m_fullscreen_requested)
    {
      FullScreen();
      m_fullscreen_requested = false;
    }
  });
  installEventFilter(this);
  m_render_widget->installEventFilter(this);

  // Handle file open events
  auto* filter = new FileOpenEventFilter(QGuiApplication::instance());
  connect(filter, &FileOpenEventFilter::fileOpened, this, [this](const QString& file_name) {
    StartGame(BootParameters::GenerateFromFile(file_name.toStdString()));
  });
}

static void InstallHotkeyFilter(QWidget* dialog)
{
  auto* filter = new WindowActivationEventFilter(dialog);
  dialog->installEventFilter(filter);

  filter->connect(filter, &WindowActivationEventFilter::windowDeactivated,
                  [] { HotkeyManagerEmu::Enable(true); });
  filter->connect(filter, &WindowActivationEventFilter::windowActivated,
                  [] { HotkeyManagerEmu::Enable(false); });
}

void MainWindow::CreateComponents()
{
  m_menu_bar = new MenuBar(this);
  m_tool_bar = new ToolBar(this);
  m_search_bar = new SearchBar(this);
  m_game_count = new GameCount(this);
  m_game_list = new GameList(this);
  m_render_widget = new RenderWidget;
  m_stack = new QStackedWidget(this);
  m_game_view_widget = new GameViewWidget(this);
  m_game_library_dock = new QDockWidget(tr("Game Library"), this);
  m_game_library_dock->setObjectName(QStringLiteral("gamelibrary"));
  m_game_library_dock->setAllowedAreas(Qt::AllDockWidgetAreas);
  m_game_library_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                   QDockWidget::DockWidgetFloatable);
  m_target_tools_widget = new TargetToolsWidget(this);
  m_property_inspector_widget = new PropertyInspectorWidget(this);
  m_devkit_status_bar = new DevkitStatusBar(this);
  m_project = new DevkitProject(this);
  m_builder = new DevkitBuilder(this);
  m_packager = new DevkitPackager(m_project, this);
  m_project_widget = new ProjectWidget(m_project, this);
  m_build_output_widget = new BuildOutputWidget(this);
  m_warnings_widget = new DiagnosticsWidget(BuildDiagnostic::Severity::Warning, this);
  m_errors_widget = new DiagnosticsWidget(BuildDiagnostic::Severity::Error, this);
  m_render_profiler_widget = new RenderProfilerWidget(this);
  m_frame_capture_widget = new FrameCaptureWidget(this);

  for (int i = 0; i < 4; i++)
  {
    m_gc_tas_input_windows[i] = new GCTASInputWindow(nullptr, i);
    m_gba_tas_input_windows[i] = new GBATASInputWindow(nullptr, i);
    m_wii_tas_input_windows[i] = new WiiTASInputWindow(nullptr, i);
  }

  m_jit_widget = new JITWidget(m_system, this);
  m_log_widget = new LogWidget(this);
  m_log_config_widget = new LogConfigWidget(this);
  m_memory_widget = new MemoryWidget(m_system, this);
  m_memory_analyzer = new MemoryAnalyzer(this);
  m_memory_map_widget = new MemoryMapWidget(m_memory_analyzer, this);
  m_memory_usage_widget = new MemoryUsageWidget(m_memory_analyzer, this);
  m_network_widget = new NetworkWidget(this);
  m_register_widget = new RegisterWidget(this);
  m_thread_widget = new ThreadWidget(this);
  m_watch_widget = new WatchWidget(this);
  m_breakpoint_widget = new BreakpointWidget(this);
  m_code_widget = new CodeWidget(this);
  m_assembler_widget = new AssemblerWidget(this);

  const auto request_watch = [this](const QString& name, u32 addr) {
    m_watch_widget->AddWatch(name, addr);
  };
  const auto request_breakpoint = [this](u32 addr) { m_breakpoint_widget->AddBP(addr); };
  const auto request_memory_breakpoint = [this](u32 addr) {
    m_breakpoint_widget->AddAddressMBP(addr);
  };
  const auto request_view_in_memory = [this](u32 addr) { m_memory_widget->SetAddress(addr); };
  const auto request_view_in_code = [this](u32 addr) {
    m_code_widget->SetAddress(addr, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  };

  connect(m_jit_widget, &JITWidget::SetCodeAddress, m_code_widget, &CodeWidget::OnSetCodeAddress);
  connect(m_watch_widget, &WatchWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_watch_widget, &WatchWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);
  connect(m_register_widget, &RegisterWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_register_widget, &RegisterWidget::RequestWatch, request_watch);
  connect(m_register_widget, &RegisterWidget::RequestViewInMemory, request_view_in_memory);
  connect(m_register_widget, &RegisterWidget::RequestViewInCode, request_view_in_code);
  connect(m_thread_widget, &ThreadWidget::RequestBreakpoint, request_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestWatch, request_watch);
  connect(m_thread_widget, &ThreadWidget::RequestViewInMemory, request_view_in_memory);
  connect(m_thread_widget, &ThreadWidget::RequestViewInCode, request_view_in_code);

  connect(m_code_widget, &CodeWidget::RequestPPCComparison, m_jit_widget,
          &JITWidget::OnRequestPPCComparison);
  connect(m_code_widget, &CodeWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);
  connect(m_memory_widget, &MemoryWidget::ShowCode, m_code_widget, [this](u32 address) {
    m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  });
  connect(m_memory_widget, &MemoryWidget::RequestWatch, request_watch);

  connect(m_breakpoint_widget, &BreakpointWidget::ShowCode, [this](u32 address) {
    if (Core::GetState(m_system) == Core::State::Paused)
      m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  });
  connect(m_breakpoint_widget, &BreakpointWidget::ShowMemory, m_memory_widget,
          &MemoryWidget::SetAddress);

  // The memory map and usage panels open regions in the Memory Viewer (the hex view).
  const auto show_in_memory_viewer = [this](u32 address, bool auxiliary) {
    ShowDebugPanel(m_memory_widget, &Settings::SetMemoryVisible);
    m_memory_widget->ShowAddress(address, auxiliary);
  };
  connect(m_memory_map_widget, &MemoryMapWidget::ShowMemory, this, show_in_memory_viewer);
  connect(m_memory_usage_widget, &MemoryUsageWidget::ShowMemory, this, show_in_memory_viewer);
  connect(m_memory_map_widget, &MemoryMapWidget::RequestRangedBreakpoint, this,
          [this](u32 from, u32 to) {
            ShowDebugPanel(m_breakpoint_widget, &Settings::SetBreakpointsVisible);
            m_breakpoint_widget->AddRangedMBP(from, to);
          });

  CreateDevkitPanels();
}

void MainWindow::ShowDebugPanel(QDockWidget* dock, void (Settings::*set_visible)(bool))
{
  Settings& settings = Settings::Instance();
  if (!settings.IsDebugModeEnabled())
    settings.SetDebugModeEnabled(true);
  (settings.*set_visible)(true);
  dock->show();
  dock->raise();
}

void MainWindow::CreateDevkitPanels()
{
  // Panel icons (shown in title bars and on dock tabs) from the bundled Font Awesome set.
  m_dock_icons = {
      {m_game_view_widget, "gamepad"},
      {m_game_library_dock, "compact-disc"},
      {m_project_widget, "folder-tree"},
      {m_build_output_widget, "file-lines"},
      {m_warnings_widget, "triangle-exclamation"},
      {m_errors_widget, "circle-xmark"},
      {m_target_tools_widget, "layer-group"},
      {m_property_inspector_widget, "table-list"},
      {m_render_profiler_widget, "chart-line"},
      {m_frame_capture_widget, "crosshairs"},
      {m_memory_widget, "memory"},
      {m_memory_map_widget, "map"},
      {m_memory_usage_widget, "chart-area"},
      {m_code_widget, "code"},
      {m_register_widget, "microchip"},
      {m_watch_widget, "eye"},
      {m_breakpoint_widget, "circle-dot"},
      {m_thread_widget, "list-ul"},
      {m_log_widget, "terminal"},
      {m_log_config_widget, "sliders"},
      {m_network_widget, "network-wired"},
      {m_jit_widget, "bolt"},
      {m_assembler_widget, "hammer"},
  };
  for (const auto& [dock, icon] : m_dock_icons)
  {
    dock->setWindowIcon(IconSet::Get(icon));
    connect(dock, &QDockWidget::dockLocationChanged, this, &MainWindow::QueueDockTabIconRefresh);
    connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::QueueDockTabIconRefresh);
    connect(dock, &QDockWidget::visibilityChanged, this, &MainWindow::QueueDockTabIconRefresh);
  }

  TargetToolsWidget* const tools = m_target_tools_widget;
  tools->AddTool("memory", tr("Memory Viewer"), tr("Inspect and edit emulated memory."),
                 [this] { ShowDebugPanel(m_memory_widget, &Settings::SetMemoryVisible); });
  tools->AddTool("map", tr("Memory Map"),
                 tr("Where MEM1 / ARAM is used: program sections, heaps, stacks and live writes."),
                 [this] {
                   m_memory_map_widget->show();
                   m_memory_map_widget->raise();
                 });
  tools->AddTool("chart-area", tr("Memory Usage"),
                 tr("Memory use over time, thread stack depth and heap allocations."), [this] {
                   m_memory_usage_widget->show();
                   m_memory_usage_widget->raise();
                 });
  tools->AddTool("code", tr("Code / Symbol Map"), tr("Disassembly, symbols and call stack."),
                 [this] { ShowDebugPanel(m_code_widget, &Settings::SetCodeVisible); });
  tools->AddTool("microchip", tr("Registers"), tr("CPU general purpose and special registers."),
                 [this] { ShowDebugPanel(m_register_widget, &Settings::SetRegistersVisible); });
  tools->AddTool("circle-dot", tr("Breakpoints"), tr("Instruction and memory breakpoints."),
                 [this] { ShowDebugPanel(m_breakpoint_widget, &Settings::SetBreakpointsVisible); });
  tools->AddTool("eye", tr("Watch"), tr("Watch expressions on emulated memory."),
                 [this] { ShowDebugPanel(m_watch_widget, &Settings::SetWatchVisible); });
  tools->AddTool("bolt", tr("JIT Profiler"), tr("Recompiled code blocks and profiling data."),
                 [this] { ShowDebugPanel(m_jit_widget, &Settings::SetJITVisible); });
  tools->AddTool("chart-line", tr("Render Profiler"),
                 tr("Frame time, draw calls, TEV usage and EFB copies for every frame."),
                 [this] {
                   m_render_profiler_widget->show();
                   m_render_profiler_widget->raise();
                 });
  tools->AddTool("crosshairs", tr("Capture Frame"),
                 tr("Capture every draw call and EFB copy of the next frame."), [this] {
                   m_frame_capture_widget->show();
                   m_frame_capture_widget->raise();
                   m_frame_capture_widget->RequestCapture();
                 });
  tools->AddTool("cubes", tr("GX / FIFO Player"),
                 tr("Record and replay graphics FIFO logs to inspect GX command streams."),
                 [this] { ShowFIFOPlayer(); });
  tools->AddToggle("gauge-high", tr("Performance Overlay"),
                   tr("Show frame time and performance graphs over the game view."),
                   Config::GFX_SHOW_GRAPHS);
  tools->AddToggle("image", tr("Texture Dump"),
                   tr("Dump decoded game textures to the User/Dump/Textures folder."),
                   Config::GFX_DUMP_TEXTURES);
  tools->AddToggle("video", tr("Frame Capture"),
                   tr("Dump every rendered frame to the User/Dump/Frames folder."),
                   Config::MAIN_MOVIE_DUMP_FRAMES);
  tools->AddTool("camera", tr("Screenshot"), tr("Save a screenshot of the game view."),
                 [this] { ScreenShot(); });

  connect(m_game_list, &GameList::SelectionChanged, m_property_inspector_widget,
          &PropertyInspectorWidget::SetGameFile);

  // Render profiling.
  connect(m_render_profiler_widget, &RenderProfilerWidget::CaptureFrameRequested, this, [this] {
    m_frame_capture_widget->show();
    m_frame_capture_widget->raise();
    m_frame_capture_widget->RequestCapture();
  });
  connect(m_render_profiler_widget, &RenderProfilerWidget::FIFOPlayerRequested, this,
          &MainWindow::ShowFIFOPlayer);

  // Project workflow: project tree, build output and diagnostics.
  connect(m_project_widget, &ProjectWidget::NewProjectRequested, this, &MainWindow::NewProject);
  connect(m_project_widget, &ProjectWidget::OpenProjectRequested, this, &MainWindow::OpenProject);
  connect(m_project_widget, &ProjectWidget::SettingsRequested, this,
          &MainWindow::ShowProjectSettings);
  connect(m_project_widget, &ProjectWidget::BootRequested, this,
          [this](const QString& path) { StartGame(path, ScanForSecondDisc::No); });
  connect(m_project, &DevkitProject::ProjectChanged, this, &MainWindow::UpdateProjectActions);
  connect(m_project_widget, &ProjectWidget::FileSelected, this, [this](const QString& path) {
    const QString run_target = m_project->ResolveRunTarget();
    m_property_inspector_widget->SetProjectFile(
        path, m_project->GetPath(),
        !run_target.isEmpty() && QFileInfo(path) == QFileInfo(run_target));
  });
  const auto open_in_editor = [this](const QString& path, int line) {
    m_project->OpenInEditor(path, line);
  };
  connect(m_project_widget, &ProjectWidget::OpenFileRequested, this, open_in_editor);
  connect(m_warnings_widget, &DiagnosticsWidget::OpenFileRequested, this, open_in_editor);
  connect(m_errors_widget, &DiagnosticsWidget::OpenFileRequested, this, open_in_editor);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &MainWindow::UpdateWindowTitle);

  connect(m_builder, &DevkitBuilder::OutputLine, m_build_output_widget,
          &BuildOutputWidget::AppendLine);
  connect(m_builder, &DevkitBuilder::DiagnosticFound, this,
          [this](const BuildDiagnostic& diagnostic) {
            m_warnings_widget->Add(diagnostic, m_project->GetPath());
            m_errors_widget->Add(diagnostic, m_project->GetPath());
          });
  // Make DOL and Pack GCM report to the Output panel like a build does.
  connect(m_packager, &DevkitPackager::OutputLine, m_build_output_widget,
          &BuildOutputWidget::AppendLine);
  connect(m_packager, &DevkitPackager::StatusMessage, this, [this](const QString& text, bool success) {
    m_build_output_widget->AppendStatus(text, success);
    m_devkit_status_bar->showMessage(text, 10000);
  });
  connect(m_packager, &DevkitPackager::Progress, this,
          [this](const QString& text) { m_devkit_status_bar->showMessage(text, 2000); });
  connect(m_packager, &DevkitPackager::PackingChanged, this, &MainWindow::UpdateProjectActions);

  connect(m_builder, &DevkitBuilder::Finished, this,
          [this](bool success, int warnings, int errors, qint64 elapsed_ms) {
            const QString summary =
                tr("%1: %2 error(s), %3 warning(s) in %4 s")
                    .arg(success ? tr("Build succeeded") : tr("Build failed"))
                    .arg(errors)
                    .arg(warnings)
                    .arg(elapsed_ms / 1000.0, 0, 'f', 1);
            m_build_output_widget->AppendStatus(summary, success);
            m_devkit_status_bar->showMessage(summary, 10000);
            if (!success && errors > 0)
            {
              m_errors_widget->show();
              m_errors_widget->raise();
            }
            const bool run = m_run_after_build && success;
            m_run_after_build = false;
            UpdateProjectActions();
            if (run)
              BootProjectOutput();
          });

  CreateProjectMenu();
  m_project->RestoreLastProject();
  UpdateProjectActions();
}

void MainWindow::CreateProjectMenu()
{
  m_project_menu = new QMenu(tr("&Project"), m_menu_bar);
  m_project_menu->addAction(IconSet::Get("folder-plus"), tr("&New Project..."), this,
                            &MainWindow::NewProject);
  QAction* const set_project = m_project_menu->addAction(
      IconSet::Get("folder-open"), tr("&Set Project..."), this, &MainWindow::OpenProject);
  set_project->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
  m_recent_projects_menu = m_project_menu->addMenu(IconSet::Get("clock-rotate-left"),
                                                   tr("Recent &Projects"));
  connect(m_recent_projects_menu, &QMenu::aboutToShow, this, &MainWindow::UpdateRecentProjectsMenu);
  UpdateRecentProjectsMenu();
  m_close_project_action =
      m_project_menu->addAction(tr("&Close Project"), m_project, &DevkitProject::Close);
  m_project_menu->addSeparator();
  m_build_action = m_project_menu->addAction(IconSet::Get("screwdriver-wrench"), tr("&Build"),
                                             this, [this] { BuildProject(false); });
  m_build_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
  m_rebuild_action = m_project_menu->addAction(IconSet::Get("arrows-rotate"), tr("&Rebuild"), this,
                                               [this] {
                                                 // Clean, then build.
                                                 const QString command =
                                                     m_project->GetCleanCommand() +
                                                     QStringLiteral(" && ") +
                                                     m_project->GetBuildCommand();
                                                 RunCommandForProject(command, tr("Rebuild"),
                                                                      false);
                                               });
  m_clean_action = m_project_menu->addAction(IconSet::Get("broom"), tr("C&lean"), this,
                                             &MainWindow::CleanProject);
  m_run_action = m_project_menu->addAction(IconSet::Get("circle-play"), tr("R&un"), this,
                                           [this] { BuildProject(true); });
  // F5 is a default Load State hotkey, so use Ctrl+R.
  m_run_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
  m_cancel_build_action =
      m_project_menu->addAction(IconSet::Get("stop"), tr("&Stop Build"), this, [this] {
        m_builder->Cancel();
        m_packager->CancelPack();
      });
  m_project_menu->addSeparator();
  m_make_dol_action =
      m_project_menu->addAction(IconSet::Get("file-export"), tr("Make &DOL"), this, [this] {
        ShowProjectOutput();
        m_packager->MakeDol();
      });
  m_pack_gcm_action =
      m_project_menu->addAction(IconSet::Get("compact-disc"), tr("&Pack GCM"), this, [this] {
        ShowProjectOutput();
        m_packager->PackGcm();
      });
  m_project_menu->addSeparator();
  m_project_settings_action = m_project_menu->addAction(
      IconSet::Get("gear"), tr("Project &Window..."), this, &MainWindow::ShowProjectSettings);

  // Sits between File and Emulation, like an IDE's Project/Build menu.
  const QList<QAction*> menus = m_menu_bar->actions();
  m_menu_bar->insertMenu(menus.size() > 1 ? menus[1] : nullptr, m_project_menu);

  connect(m_tool_bar, &ToolBar::BuildPressed, this, [this] { BuildProject(false); });
  connect(m_tool_bar, &ToolBar::NewProjectPressed, this, &MainWindow::NewProject);
  connect(m_tool_bar, &ToolBar::SetProjectPressed, this, &MainWindow::OpenProject);
  connect(m_tool_bar, &ToolBar::MakeDolPressed, m_make_dol_action, &QAction::trigger);
  connect(m_tool_bar, &ToolBar::PackGcmPressed, m_pack_gcm_action, &QAction::trigger);
  m_tool_bar->SetRecentProjectsMenu(m_recent_projects_menu);
  connect(m_tool_bar, &ToolBar::RunPressed, this, [this] { BuildProject(true); });
}

void MainWindow::UpdateRecentProjectsMenu()
{
  m_recent_projects_menu->clear();
  const QStringList recent = DevkitProject::GetRecentProjects();
  for (const QString& path : recent)
  {
    QAction* const action = m_recent_projects_menu->addAction(
        IconSet::Get("folder"),
        QStringLiteral("%1  (%2)").arg(QFileInfo(path).fileName(), QDir::toNativeSeparators(path)),
        this, [this, path] {
          if (m_project->Open(path))
          {
            m_project_widget->show();
            m_project_widget->raise();
          }
        });
    action->setEnabled(!m_builder->IsRunning() && !m_packager->IsPacking());
  }
  if (recent.isEmpty())
    m_recent_projects_menu->addAction(tr("No recent projects"))->setEnabled(false);
  m_recent_projects_menu->addSeparator();
  m_recent_projects_menu->addAction(tr("&Clear Recent Projects"), m_project,
                                    &DevkitProject::ClearRecentProjects)
      ->setEnabled(!recent.isEmpty());
}

void MainWindow::UpdateProjectActions()
{
  const bool open = m_project->IsOpen();
  const bool building = m_builder->IsRunning() || m_packager->IsPacking();
  for (QAction* action : {m_build_action, m_rebuild_action, m_clean_action, m_run_action,
                          m_make_dol_action, m_pack_gcm_action})
    action->setEnabled(open && !building);
  m_close_project_action->setEnabled(open && !building);
  m_project_settings_action->setEnabled(open);
  m_cancel_build_action->setEnabled(building);
  m_tool_bar->SetProjectState(open, building);
  m_project_menu->menuAction()->setVisible(m_editor_layout_active);
  m_tool_bar->SetProjectActionsVisible(m_editor_layout_active);
  UpdateWindowTitle();
}

void MainWindow::UpdateWindowTitle()
{
  if (!m_editor_layout_active)
  {
    setWindowTitle(QStringLiteral("GameCube"));
    return;
  }

  // Like the devkit concept: "GameCube DevKit • <project> • Target: <title>".
  QStringList parts = {tr("GameCube DevKit")};
  if (m_project->IsOpen())
    parts.append(m_project->GetName());
  if (Core::GetState(m_system) != Core::State::Uninitialized)
  {
    const QString game_id = QString::fromStdString(SConfig::GetInstance().GetGameID());
    if (!game_id.isEmpty())
      parts.append(tr("Target: %1").arg(game_id));
  }
  setWindowTitle(parts.join(QStringLiteral(" \u2022 ")));
}

void MainWindow::NewProject()
{
  const QString path = QFileDialog::getSaveFileName(
      this, tr("New GameCube Project (choose a folder name)"),
      QDir::homePath() + QStringLiteral("/my_gamecube_game"), QString(), nullptr,
      QFileDialog::ShowDirsOnly);
  if (path.isEmpty())
    return;

  QString error;
  if (!m_project->CreateFromTemplate(path, &error))
  {
    ModalMessageBox::critical(this, tr("New Project"), error);
    return;
  }
  m_project_widget->show();
  m_project_widget->raise();
}

void MainWindow::OpenProject()
{
  // Maya-style Set Project: any folder can become the project; one without a project definition
  // gets one with the default folder rules.
  const QString path = QFileDialog::getExistingDirectory(
      this, tr("Set Project"), m_project->IsOpen() ? m_project->GetPath() : QDir::homePath());
  if (path.isEmpty() || m_builder->IsRunning() || m_packager->IsPacking() ||
      !m_project->SetProject(path))
    return;
  m_project_widget->show();
  m_project_widget->raise();
}

void MainWindow::OpenProjectForExecutable(const QString& executable_path)
{
  const QString root = DevkitProject::FindProjectRoot(executable_path);
  if (root.isEmpty())
    return;

  if (!m_project->IsOpen() ||
      QDir(m_project->GetPath()).canonicalPath() != QDir(root).canonicalPath())
  {
    if (!m_project->Open(root))
      return;
  }
  m_project_widget->show();
  m_project_widget->raise();
}

void MainWindow::ShowProjectSettings()
{
  if (!m_project->IsOpen())
    return;
  ProjectSettingsDialog dialog(m_project, this);
  dialog.exec();
}

void MainWindow::ShowProjectOutput()
{
  m_build_output_widget->show();
  m_build_output_widget->raise();
}

void MainWindow::BuildProject(bool then_run)
{
  RunCommandForProject(m_project->GetBuildCommand(), tr("Build"), then_run);
}

void MainWindow::CleanProject()
{
  RunCommandForProject(m_project->GetCleanCommand(), tr("Clean"), false);
}

void MainWindow::RunCommandForProject(const QString& command, const QString& title,
                                      bool then_run)
{
  if (!m_project->IsOpen() || m_builder->IsRunning() || m_packager->IsPacking())
    return;

  if (command.trimmed().isEmpty())
  {
    // No build step configured: Run just boots whatever is there.
    if (then_run)
      BootProjectOutput();
    return;
  }

  m_run_after_build = then_run;
  m_build_output_widget->Clear();
  m_warnings_widget->Clear();
  m_errors_widget->Clear();
  m_build_output_widget->AppendStatus(
      tr("%1 %2: %3").arg(title, m_project->GetName(), command), true);
  m_build_output_widget->show();
  m_build_output_widget->raise();
  m_devkit_status_bar->showMessage(tr("%1 running...").arg(title));

  m_builder->Run(command, m_project->GetPath());
  UpdateProjectActions();
}

void MainWindow::BootProjectOutput()
{
  const QString target = m_project->ResolveRunTarget();
  if (target.isEmpty())
  {
    ModalMessageBox::warning(
        this, tr("Run"),
        tr("No .dol or .elf was found in the project. Build the project first, or choose a run "
           "target in Project Settings."));
    return;
  }
  m_build_output_widget->AppendStatus(tr("Booting %1").arg(target), true);
  StartGame(target, ScanForSecondDisc::No);
}

void MainWindow::ApplyDockTitleBars(bool editor_layout)
{
  for (const auto& [dock, icon] : m_dock_icons)
  {
    QWidget* const old_title_bar = dock->titleBarWidget();
    if (editor_layout && old_title_bar == nullptr)
    {
      dock->setTitleBarWidget(new DockTitleBar(dock, icon));
    }
    else if (!editor_layout && old_title_bar != nullptr)
    {
      dock->setTitleBarWidget(nullptr);
      old_title_bar->deleteLater();
    }
  }
}

void MainWindow::QueueDockTabIconRefresh()
{
  // Qt rebuilds the dock tab bars lazily, so update their icons once the layout has settled.
  if (m_dock_tab_icon_refresh_queued)
    return;
  m_dock_tab_icon_refresh_queued = true;
  QTimer::singleShot(0, this, [this] {
    m_dock_tab_icon_refresh_queued = false;
    RefreshDockTabIcons();
  });
}

void MainWindow::RefreshDockTabIcons()
{
  // QMainWindow's dock tab bars don't show dock icons on their own. Each tab stores the address of
  // its dock widget as tab data, which lets us match tabs to docks.
  const auto dock_for_tab = [this](const QTabBar* tab_bar, int index) -> QDockWidget* {
    const auto dock_address = tab_bar->tabData(index).value<quintptr>();
    for (const auto& [dock, icon] : m_dock_icons)
    {
      if (reinterpret_cast<quintptr>(dock) == dock_address)
        return dock;
    }
    return nullptr;
  };

  std::set<QDockWidget*> tabbed_docks;
  for (QTabBar* const tab_bar : findChildren<QTabBar*>(Qt::FindDirectChildrenOnly))
  {
    // QMainWindow keeps unused tab bars around hidden; their tabs are stale.
    const bool in_use = !tab_bar->isHidden() && tab_bar->count() > 1;
    for (int i = 0; i < tab_bar->count(); ++i)
    {
      QDockWidget* const dock = dock_for_tab(tab_bar, i);
      if (dock == nullptr)
        continue;

      tab_bar->setTabIcon(i, dock->windowIcon());
      if (in_use)
        tabbed_docks.insert(dock);

      // In the editor layout the tab is the panel's only header, so it carries the close button.
      QWidget* const close_button = tab_bar->tabButton(i, QTabBar::RightSide);
      const bool ours =
          close_button != nullptr && close_button->property("devkitTabClose").toBool();
      const bool wants_close = m_editor_layout_active && in_use &&
                               dock->features().testFlag(QDockWidget::DockWidgetClosable);
      if (wants_close && close_button == nullptr)
      {
        auto* const button = new QToolButton(tab_bar);
        button->setObjectName(QStringLiteral("dockTabCloseButton"));
        button->setProperty("devkitTabClose", true);
        button->setIcon(IconSet::Get("xmark"));
        button->setIconSize(QSize(10, 10));
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tr("Close this panel"));
        connect(button, &QToolButton::clicked, this, [this, tab_bar, button, dock_for_tab] {
          // Tabs get reused as the layout changes, so look up the tab's dock when clicked.
          for (int tab = 0; tab < tab_bar->count(); ++tab)
          {
            if (tab_bar->tabButton(tab, QTabBar::RightSide) != button)
              continue;
            if (QDockWidget* const tab_dock = dock_for_tab(tab_bar, tab))
              tab_dock->close();
            break;
          }
        });
        tab_bar->setTabButton(i, QTabBar::RightSide, button);
      }
      else if (!wants_close && ours)
      {
        tab_bar->setTabButton(i, QTabBar::RightSide, nullptr);
        close_button->deleteLater();
      }
    }
  }

  // A docked panel in a tab group doesn't need its own title bar as well: the tab shows the same
  // icon and title, and dragging the tab moves the panel.
  for (const auto& [dock, icon] : m_dock_icons)
  {
    if (auto* const title_bar = qobject_cast<DockTitleBar*>(dock->titleBarWidget()))
      title_bar->SetCompact(!dock->isFloating() && tabbed_docks.contains(dock));
  }
}

void MainWindow::ConnectMenuBar()
{
  // View
  connect(m_menu_bar, &MenuBar::EditorLayoutToggled, this, &MainWindow::SetEditorLayoutEnabled);
  connect(m_menu_bar, &MenuBar::ResetEditorLayout, this, &MainWindow::ResetEditorLayout);

  setMenuBar(m_menu_bar);
  // File
  connect(m_menu_bar, &MenuBar::Open, this, &MainWindow::Open);
  connect(m_menu_bar, &MenuBar::Exit, this, &MainWindow::close);
  connect(m_menu_bar, &MenuBar::EjectDisc, this, &MainWindow::EjectDisc);
  connect(m_menu_bar, &MenuBar::ChangeDisc, this, &MainWindow::ChangeDisc);
  connect(m_menu_bar, &MenuBar::OpenUserFolder, this, &MainWindow::OpenUserFolder);
  connect(m_menu_bar, &MenuBar::OpenConfigFolder, this, &MainWindow::OpenConfigFolder);
  connect(m_menu_bar, &MenuBar::OpenCacheFolder, this, &MainWindow::OpenCacheFolder);

  // Emulation
  connect(m_menu_bar, &MenuBar::Pause, this, &MainWindow::Pause);
  connect(m_menu_bar, &MenuBar::Play, this, [this] { Play(); });
  connect(m_menu_bar, &MenuBar::Stop, this, &MainWindow::RequestStop);
  connect(m_menu_bar, &MenuBar::Reset, this, &MainWindow::Reset);
  connect(m_menu_bar, &MenuBar::Fullscreen, this, &MainWindow::FullScreen);
  connect(m_menu_bar, &MenuBar::FrameAdvance, this, &MainWindow::FrameAdvance);
  connect(m_menu_bar, &MenuBar::Screenshot, this, &MainWindow::ScreenShot);
  connect(m_menu_bar, &MenuBar::StateLoad, this, &MainWindow::StateLoad);
  connect(m_menu_bar, &MenuBar::StateSave, this, &MainWindow::StateSave);
  connect(m_menu_bar, &MenuBar::StateLoadSlot, this, &MainWindow::StateLoadSlot);
  connect(m_menu_bar, &MenuBar::StateSaveSlot, this, &MainWindow::StateSaveSlot);
  connect(m_menu_bar, &MenuBar::StateLoadSlotAt, this, &MainWindow::StateLoadSlotAt);
  connect(m_menu_bar, &MenuBar::StateSaveSlotAt, this, &MainWindow::StateSaveSlotAt);
  connect(m_menu_bar, &MenuBar::StateLoadUndo, this, &MainWindow::StateLoadUndo);
  connect(m_menu_bar, &MenuBar::StateSaveUndo, this, &MainWindow::StateSaveUndo);
  connect(m_menu_bar, &MenuBar::StateSaveOldest, this, &MainWindow::StateSaveOldest);
  connect(m_menu_bar, &MenuBar::SetStateSlot, this, &MainWindow::SetStateSlot);

  // Options
  connect(m_menu_bar, &MenuBar::Configure, this, &MainWindow::ShowSettingsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureGraphics, this, &MainWindow::ShowGraphicsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureAudio, this, &MainWindow::ShowAudioWindow);
  connect(m_menu_bar, &MenuBar::ConfigureControllers, this, &MainWindow::ShowControllersWindow);
  connect(m_menu_bar, &MenuBar::ConfigureHotkeys, this, &MainWindow::ShowHotkeyDialog);
  connect(m_menu_bar, &MenuBar::ConfigureFreelook, this, &MainWindow::ShowFreeLookWindow);

  // Tools
  connect(m_menu_bar, &MenuBar::ShowMemcardManager, this, &MainWindow::ShowMemcardManager);
  connect(m_menu_bar, &MenuBar::ShowResourcePackManager, this,
          &MainWindow::ShowResourcePackManager);
  connect(m_menu_bar, &MenuBar::ShowCheatsManager, this, &MainWindow::ShowCheatsManager);
  connect(m_menu_bar, &MenuBar::BootGameCubeIPL, this, &MainWindow::OnBootGameCubeIPL);
  connect(m_menu_bar, &MenuBar::ImportNANDBackup, this, &MainWindow::OnImportNANDBackup);
  connect(m_menu_bar, &MenuBar::PerformOnlineUpdate, this, &MainWindow::PerformOnlineUpdate);
  connect(m_menu_bar, &MenuBar::BootWiiSystemMenu, this, &MainWindow::BootWiiSystemMenu);
  connect(m_menu_bar, &MenuBar::StartNetPlay, this, &MainWindow::ShowNetPlaySetupDialog);
  connect(m_menu_bar, &MenuBar::BrowseNetPlay, this, &MainWindow::ShowNetPlayBrowser);
  connect(m_menu_bar, &MenuBar::ShowFIFOPlayer, this, &MainWindow::ShowFIFOPlayer);
  connect(m_menu_bar, &MenuBar::ShowSkylanderPortal, this, &MainWindow::ShowSkylanderPortal);
  connect(m_menu_bar, &MenuBar::ShowInfinityBase, this, &MainWindow::ShowInfinityBase);
  connect(m_menu_bar, &MenuBar::ShowWiiSpeakWindow, this, &MainWindow::ShowWiiSpeakWindow);
  connect(m_menu_bar, &MenuBar::ShowLogitechMicWindow, this, &MainWindow::ShowLogitechMicWindow);
  connect(m_menu_bar, &MenuBar::ConnectWiiRemote, this, &MainWindow::OnConnectWiiRemote);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_menu_bar, &MenuBar::ShowAchievementsWindow, this, &MainWindow::ShowAchievementsWindow);
#endif  // USE_RETRO_ACHIEVEMENTS

  // Movie
  connect(m_menu_bar, &MenuBar::PlayRecording, this, &MainWindow::OnPlayRecording);
  connect(m_menu_bar, &MenuBar::StartRecording, this, &MainWindow::OnStartRecording);
  connect(m_menu_bar, &MenuBar::StopRecording, this, &MainWindow::OnStopRecording);
  connect(m_menu_bar, &MenuBar::ExportRecording, this, &MainWindow::OnExportRecording);
  connect(m_menu_bar, &MenuBar::ShowTASInput, this, &MainWindow::ShowTASInput);
  connect(m_menu_bar, &MenuBar::ConfigureOSD, this, &MainWindow::ShowOSDWindow);

  // View
  connect(m_menu_bar, &MenuBar::ShowList, m_game_list, &GameList::SetListView);
  connect(m_menu_bar, &MenuBar::ShowGrid, m_game_list, &GameList::SetGridView);
  connect(m_menu_bar, &MenuBar::PurgeGameListCache, m_game_list, &GameList::PurgeCache);
  connect(m_menu_bar, &MenuBar::ShowSearch, m_search_bar, &SearchBar::Show);

  connect(m_menu_bar, &MenuBar::ColumnVisibilityToggled, m_game_list,
          &GameList::OnColumnVisibilityToggled);

  connect(m_menu_bar, &MenuBar::GameListPlatformVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);
  connect(m_menu_bar, &MenuBar::GameListRegionVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);

  connect(m_menu_bar, &MenuBar::ShowAboutDialog, this, &MainWindow::ShowAboutDialog);

  connect(m_game_list, &GameList::SelectionChanged, m_menu_bar, &MenuBar::SelectionChanged);
  connect(this, &MainWindow::ReadOnlyModeChanged, m_menu_bar, &MenuBar::ReadOnlyModeChanged);
  connect(this, &MainWindow::RecordingStatusChanged, m_menu_bar, &MenuBar::RecordingStatusChanged);
}

void MainWindow::ConnectHotkeys()
{
  connect(m_hotkey_scheduler, &HotkeyScheduler::Open, this, &MainWindow::Open);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ChangeDisc, this, &MainWindow::ChangeDisc);
  connect(m_hotkey_scheduler, &HotkeyScheduler::EjectDisc, this, &MainWindow::EjectDisc);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExitHotkey, this, &MainWindow::close);
  connect(m_hotkey_scheduler, &HotkeyScheduler::UnlockCursor, this, &MainWindow::UnlockCursor);
  connect(m_hotkey_scheduler, &HotkeyScheduler::TogglePauseHotkey, this, &MainWindow::TogglePause);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ActivateChat, this, &MainWindow::OnActivateChat);
  connect(m_hotkey_scheduler, &HotkeyScheduler::RequestGolfControl, this,
          &MainWindow::OnRequestGolfControl);
  connect(m_hotkey_scheduler, &HotkeyScheduler::RefreshGameListHotkey, this,
          &MainWindow::RefreshGameList);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StopHotkey, this, &MainWindow::RequestStop);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ResetHotkey, this, &MainWindow::Reset);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ScreenShotHotkey, this, &MainWindow::ScreenShot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::FullScreenHotkey, this, &MainWindow::FullScreen);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadSlot, this, &MainWindow::StateLoadSlotAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveSlot, this, &MainWindow::StateSaveSlotAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadLastSaved, this,
          &MainWindow::StateLoadLastSavedAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadUndo, this, &MainWindow::StateLoadUndo);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveUndo, this, &MainWindow::StateSaveUndo);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveOldest, this,
          &MainWindow::StateSaveOldest);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveFile, this, &MainWindow::StateSave);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadFile, this, &MainWindow::StateLoad);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadSlotHotkey, this,
          &MainWindow::StateLoadSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveSlotHotkey, this,
          &MainWindow::StateSaveSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::SetStateSlotHotkey, this,
          &MainWindow::SetStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::IncrementSelectedStateSlotHotkey, this,
          &MainWindow::IncrementSelectedStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::DecrementSelectedStateSlotHotkey, this,
          &MainWindow::DecrementSelectedStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StartRecording, this,
          &MainWindow::OnStartRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::PlayRecording, this, &MainWindow::OnPlayRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExportRecording, this,
          &MainWindow::OnExportRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ConnectWiiRemote, this,
          &MainWindow::OnConnectWiiRemote);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ToggleReadOnlyMode, [this] {
    auto& movie = m_system.GetMovie();
    bool read_only = !movie.IsReadOnly();
    movie.SetReadOnly(read_only);
    emit ReadOnlyModeChanged(read_only);
  });
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_hotkey_scheduler, &HotkeyScheduler::OpenAchievements, this,
          &MainWindow::ShowAchievementsWindow, Qt::QueuedConnection);
#endif  // USE_RETRO_ACHIEVEMENTS

  connect(m_hotkey_scheduler, &HotkeyScheduler::Step, m_code_widget, &CodeWidget::Step);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StepOver, m_code_widget, &CodeWidget::StepOver);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StepOut, m_code_widget, &CodeWidget::StepOut);
  connect(m_hotkey_scheduler, &HotkeyScheduler::Skip, m_code_widget, &CodeWidget::Skip);

  connect(m_hotkey_scheduler, &HotkeyScheduler::ShowPC, m_code_widget, &CodeWidget::ShowPC);
  connect(m_hotkey_scheduler, &HotkeyScheduler::SetPC, m_code_widget, &CodeWidget::SetPC);

  connect(m_hotkey_scheduler, &HotkeyScheduler::ToggleBreakpoint, m_code_widget,
          &CodeWidget::ToggleBreakpoint);
  connect(m_hotkey_scheduler, &HotkeyScheduler::AddBreakpoint, m_code_widget,
          &CodeWidget::AddBreakpoint);

  connect(m_hotkey_scheduler, &HotkeyScheduler::SkylandersPortalHotkey, this,
          &MainWindow::ShowSkylanderPortal);
  connect(m_hotkey_scheduler, &HotkeyScheduler::InfinityBaseHotkey, this,
          &MainWindow::ShowInfinityBase);
}

void MainWindow::ConnectToolBar()
{
  addToolBar(m_tool_bar);

  connect(m_tool_bar, &ToolBar::OpenPressed, this, &MainWindow::Open);
  connect(m_tool_bar, &ToolBar::RefreshPressed, this, &MainWindow::RefreshGameList);

  connect(m_tool_bar, &ToolBar::PlayPressed, this, [this] { Play(); });
  connect(m_tool_bar, &ToolBar::PausePressed, this, &MainWindow::Pause);
  connect(m_tool_bar, &ToolBar::StopPressed, this, &MainWindow::RequestStop);
  connect(m_tool_bar, &ToolBar::ResetPressed, this, &MainWindow::Reset);
  connect(m_tool_bar, &ToolBar::FullScreenPressed, this, &MainWindow::FullScreen);
  connect(m_tool_bar, &ToolBar::ScreenShotPressed, this, &MainWindow::ScreenShot);
  connect(m_tool_bar, &ToolBar::SettingsPressed, this, &MainWindow::ShowSettingsWindow);
  connect(m_tool_bar, &ToolBar::ControllersPressed, this, &MainWindow::ShowControllersWindow);
  connect(m_tool_bar, &ToolBar::GraphicsPressed, this, &MainWindow::ShowGraphicsWindow);

  connect(m_tool_bar, &ToolBar::StepPressed, m_code_widget, &CodeWidget::Step);
  connect(m_tool_bar, &ToolBar::StepOverPressed, m_code_widget, &CodeWidget::StepOver);
  connect(m_tool_bar, &ToolBar::StepOutPressed, m_code_widget, &CodeWidget::StepOut);
  connect(m_tool_bar, &ToolBar::SkipPressed, m_code_widget, &CodeWidget::Skip);
  connect(m_tool_bar, &ToolBar::ShowPCPressed, m_code_widget, &CodeWidget::ShowPC);
  connect(m_tool_bar, &ToolBar::SetPCPressed, m_code_widget, &CodeWidget::SetPC);
}

void MainWindow::ConnectGameList()
{
  connect(m_game_list, &GameList::GameSelected, this, [this] { Play(); });
  connect(m_game_list, &GameList::NetPlayHost, this, &MainWindow::NetPlayHost);
  connect(m_game_list, &GameList::OnStartWithRiivolution, this,
          &MainWindow::ShowRiivolutionBootWidget);

  connect(m_game_list, &GameList::OpenGeneralSettings, this, &MainWindow::ShowGeneralWindow);
  connect(m_game_list, &GameList::OpenGraphicsSettings, this, &MainWindow::ShowGraphicsWindow);
}

void MainWindow::ConnectRenderWidget()
{
  m_rendering_to_main = false;
  m_render_widget->hide();
  connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
  connect(m_render_widget, &RenderWidget::FocusChanged, this, [this](bool focus) {
    if (m_render_widget->isFullScreen())
      SetFullScreenResolution(focus);
  });
}

void MainWindow::ConnectHost()
{
  connect(Host::GetInstance(), &Host::RequestStop, this, &MainWindow::RequestStop);
}

void MainWindow::ConnectStack()
{
  auto* widget = new QWidget;
  auto* layout = new QVBoxLayout;
  widget->setLayout(layout);

  layout->addWidget(m_game_list);
  layout->addWidget(m_search_bar);
  layout->addWidget(m_game_count);
  layout->setSpacing(0);
  layout->setContentsMargins(0, 0, 0, 0);

  connect(m_search_bar, &SearchBar::Search, m_game_list, &GameList::SetSearchTerm);
  connect(m_game_list, &GameList::GameCountUpdated, m_game_count, &GameCount::OnGameCountUpdated);

  m_game_list->UpdateGameCount();

  const auto update_spacing = [this](const bool game_count_is_visible) {
    // The bottom margin of the search bar and the top margin of the game count are both suitable
    // when the other widget is hidden, but when both are visible the gap created by the combination
    // is too large. To fix this we set the bottom margin of the search bar to 0 when the game count
    // is visible and set it to the top margin when the game count is hidden.
    m_game_count->setVisible(game_count_is_visible);
    auto* const search_layout = m_search_bar->layout();
    QMargins search_margins = search_layout->contentsMargins();
    const int new_bottom_margin = game_count_is_visible ? 0 : search_margins.top();
    search_margins.setBottom(new_bottom_margin);
    search_layout->setContentsMargins(search_margins);
  };
  update_spacing(Settings::Instance().IsGameCountVisible());

  connect(&Settings::Instance(), &Settings::GameCountVisibilityChanged, update_spacing);

  m_stack->addWidget(widget);

  setCentralWidget(m_stack);

  setDockOptions(DockOption::AllowNestedDocks | DockOption::AllowTabbedDocks);
  setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
  addDockWidget(Qt::LeftDockWidgetArea, m_log_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_log_config_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_code_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_register_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_thread_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_watch_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_breakpoint_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_memory_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_network_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_jit_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_assembler_widget);

  tabifyDockWidget(m_log_widget, m_log_config_widget);
  tabifyDockWidget(m_log_widget, m_code_widget);
  tabifyDockWidget(m_log_widget, m_register_widget);
  tabifyDockWidget(m_log_widget, m_thread_widget);
  tabifyDockWidget(m_log_widget, m_watch_widget);
  tabifyDockWidget(m_log_widget, m_breakpoint_widget);
  tabifyDockWidget(m_log_widget, m_memory_widget);
  tabifyDockWidget(m_log_widget, m_network_widget);
  tabifyDockWidget(m_log_widget, m_jit_widget);
  tabifyDockWidget(m_log_widget, m_assembler_widget);

  ApplyLayoutMode(Config::Get(Config::MAIN_EDITOR_LAYOUT));
}

void MainWindow::ApplyLayoutMode(bool editor_layout)
{
  m_editor_layout_active = editor_layout;

  if (editor_layout)
  {
    // The game library moves out of the central area into its own dock panel. Qt requires a
    // central widget, so a hidden placeholder takes its place and the docks fill the window.
    if (centralWidget() == m_stack)
      takeCentralWidget();
    if (!m_editor_central_placeholder)
      m_editor_central_placeholder = new QWidget;
    setCentralWidget(m_editor_central_placeholder);
    m_editor_central_placeholder->hide();

    // The console runs underneath the library and game view; the inspector column on the right
    // spans the full height. (Corners aren't part of saveState(), so set them here.)
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    m_game_library_dock->setWidget(m_stack);
    addDockWidget(Qt::LeftDockWidgetArea, m_game_library_dock);
    addDockWidget(Qt::LeftDockWidgetArea, m_game_view_widget);
    addDockWidget(Qt::RightDockWidgetArea, m_target_tools_widget);
    addDockWidget(Qt::RightDockWidgetArea, m_property_inspector_widget);
    addDockWidget(Qt::LeftDockWidgetArea, m_project_widget);
    addDockWidget(Qt::LeftDockWidgetArea, m_memory_map_widget);
    for (QDockWidget* dock : {static_cast<QDockWidget*>(m_build_output_widget),
                              static_cast<QDockWidget*>(m_warnings_widget),
                              static_cast<QDockWidget*>(m_errors_widget),
                              static_cast<QDockWidget*>(m_memory_usage_widget),
                              static_cast<QDockWidget*>(m_render_profiler_widget),
                              static_cast<QDockWidget*>(m_frame_capture_widget)})
    {
      addDockWidget(Qt::BottomDockWidgetArea, dock);
    }
    m_game_library_dock->show();
    m_game_view_widget->show();

    m_log_widget->setWindowTitle(tr("Console"));

    if (statusBar() != m_devkit_status_bar)
      setStatusBar(m_devkit_status_bar);
    m_devkit_status_bar->show();
  }
  else
  {
    removeDockWidget(m_game_view_widget);
    removeDockWidget(m_game_library_dock);
    removeDockWidget(m_target_tools_widget);
    removeDockWidget(m_property_inspector_widget);
    removeDockWidget(m_project_widget);
    removeDockWidget(m_build_output_widget);
    removeDockWidget(m_warnings_widget);
    removeDockWidget(m_errors_widget);
    removeDockWidget(m_memory_map_widget);
    removeDockWidget(m_memory_usage_widget);
    removeDockWidget(m_render_profiler_widget);
    removeDockWidget(m_frame_capture_widget);
    m_devkit_status_bar->hide();
    m_log_widget->setWindowTitle(tr("Log"));
    m_game_library_dock->setWidget(nullptr);

    if (centralWidget() == m_editor_central_placeholder)
    {
      takeCentralWidget();
      m_editor_central_placeholder->deleteLater();
      m_editor_central_placeholder = nullptr;
    }
    setCentralWidget(m_stack);
    m_stack->show();
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
  }

  setDockNestingEnabled(true);
  ApplyDockTitleBars(editor_layout);
  if (m_project_menu)
    UpdateProjectActions();
  QueueDockTabIconRefresh();
}

void MainWindow::ResetEditorLayout()
{
  if (!m_editor_layout_active)
    return;

  // Arrangement modelled on the devkit concept: library on the left, the game view in the middle
  // tabbed with the big inspection tools, Target Tools and the Property Inspector on the right,
  // and the console (log) tabbed with the CPU state panels along the bottom.
  QDockWidget* const center_tabs[] = {m_memory_map_widget, m_memory_widget, m_code_widget,
                                      m_jit_widget,        m_assembler_widget,
                                      m_frame_capture_widget};
  // Tall panels (threads, network, log config) go in the full-height right column so the default
  // layout fits on a 1080p screen.
  QDockWidget* const inspector_tabs[] = {m_property_inspector_widget, m_thread_widget,
                                         m_network_widget, m_log_config_widget};
  QDockWidget* const console_tabs[] = {
      m_log_widget,          m_build_output_widget,    m_warnings_widget, m_errors_widget,
      m_memory_usage_widget, m_render_profiler_widget, m_register_widget, m_watch_widget,
      m_breakpoint_widget};

  for (QDockWidget* dock :
       {static_cast<QDockWidget*>(m_game_library_dock), static_cast<QDockWidget*>(m_game_view_widget),
        static_cast<QDockWidget*>(m_memory_widget), static_cast<QDockWidget*>(m_code_widget),
        static_cast<QDockWidget*>(m_jit_widget), static_cast<QDockWidget*>(m_assembler_widget),
        static_cast<QDockWidget*>(m_register_widget), static_cast<QDockWidget*>(m_watch_widget),
        static_cast<QDockWidget*>(m_breakpoint_widget), static_cast<QDockWidget*>(m_thread_widget),
        static_cast<QDockWidget*>(m_log_widget), static_cast<QDockWidget*>(m_log_config_widget),
        static_cast<QDockWidget*>(m_network_widget),
        static_cast<QDockWidget*>(m_target_tools_widget),
        static_cast<QDockWidget*>(m_property_inspector_widget),
        static_cast<QDockWidget*>(m_project_widget),
        static_cast<QDockWidget*>(m_build_output_widget),
        static_cast<QDockWidget*>(m_warnings_widget),
        static_cast<QDockWidget*>(m_errors_widget),
        static_cast<QDockWidget*>(m_memory_map_widget),
        static_cast<QDockWidget*>(m_memory_usage_widget),
        static_cast<QDockWidget*>(m_render_profiler_widget),
        static_cast<QDockWidget*>(m_frame_capture_widget)})
  {
    dock->setFloating(false);
  }

  addDockWidget(Qt::LeftDockWidgetArea, m_project_widget);
  splitDockWidget(m_project_widget, m_game_view_widget, Qt::Horizontal);
  tabifyDockWidget(m_project_widget, m_game_library_dock);
  for (QDockWidget* dock : center_tabs)
    tabifyDockWidget(m_game_view_widget, dock);

  addDockWidget(Qt::RightDockWidgetArea, m_target_tools_widget);
  splitDockWidget(m_target_tools_widget, inspector_tabs[0], Qt::Vertical);
  for (size_t i = 1; i < std::size(inspector_tabs); ++i)
    tabifyDockWidget(inspector_tabs[0], inspector_tabs[i]);
  m_target_tools_widget->show();
  m_property_inspector_widget->show();

  addDockWidget(Qt::BottomDockWidgetArea, console_tabs[0]);
  for (size_t i = 1; i < std::size(console_tabs); ++i)
    tabifyDockWidget(console_tabs[0], console_tabs[i]);

  m_game_library_dock->show();
  m_project_widget->show();
  for (QDockWidget* dock : {static_cast<QDockWidget*>(m_build_output_widget),
                            static_cast<QDockWidget*>(m_warnings_widget),
                            static_cast<QDockWidget*>(m_errors_widget),
                            static_cast<QDockWidget*>(m_memory_map_widget),
                            static_cast<QDockWidget*>(m_memory_usage_widget),
                            static_cast<QDockWidget*>(m_render_profiler_widget),
                            static_cast<QDockWidget*>(m_frame_capture_widget)})
  {
    dock->show();
  }
  m_game_view_widget->show();

  // Turn on the panels the editor layout is built around. The debugger panels still only appear
  // while debug mode is enabled.
  Settings& settings = Settings::Instance();
  settings.SetLogVisible(true);
  settings.SetCodeVisible(true);
  settings.SetMemoryVisible(true);
  settings.SetRegistersVisible(true);
  settings.SetWatchVisible(true);
  settings.SetBreakpointsVisible(true);
  settings.RefreshWidgetVisibility();

  // Raise the first tab of each group last: showing panels above can bring their tab forward.
  m_project_widget->raise();
  m_game_view_widget->raise();
  inspector_tabs[0]->raise();
  console_tabs[0]->raise();
  // The log shows itself once its settings apply, which brings the last tab back to the front.
  QTimer::singleShot(0, this, [this] {
    m_game_view_widget->raise();
    m_log_widget->raise();
  });

  // Sizes can only be applied once the window has its real geometry, which on first launch comes
  // after this runs (the window is shown, then maximized or restored by the window manager). Keep
  // the default proportions on every resize until the user drags a dock separator themselves.
  m_keep_default_dock_sizes = true;
  QueueDefaultDockSizes();
}

void MainWindow::QueueDefaultDockSizes()
{
  if (m_default_dock_sizes_queued)
    return;
  m_default_dock_sizes_queued = true;
  QTimer::singleShot(0, this, [this] {
    m_default_dock_sizes_queued = false;
    if (!m_editor_layout_active)
      return;

    const int total = width();
    resizeDocks({m_project_widget, m_game_view_widget, m_target_tools_widget},
                {total * 17 / 100, total * 61 / 100, total * 22 / 100}, Qt::Horizontal);
    resizeDocks({m_target_tools_widget, m_property_inspector_widget},
                {height() * 52 / 100, height() * 36 / 100}, Qt::Vertical);

    if (m_log_widget->isVisible())
    {
      resizeDocks({m_game_view_widget, m_log_widget}, {height() * 62 / 100, height() * 20 / 100},
                  Qt::Vertical);
    }
  });
}

void MainWindow::SetEditorLayoutEnabled(bool enabled)
{
  if (enabled == m_editor_layout_active)
    return;

  // Moving the render widget between layouts mid-emulation isn't supported; the menu action is
  // disabled while a game is running, but guard here too.
  if (!Core::IsUninitialized(m_system))
    return;

  SaveLayoutState();
  Config::SetBaseOrCurrent(Config::MAIN_EDITOR_LAYOUT, enabled);
  ApplyLayoutMode(enabled);
  RestoreLayoutState();
}

void MainWindow::SaveLayoutState() const
{
  QSettings& settings = Settings::GetQSettings();
  settings.setValue(m_editor_layout_active ? QStringLiteral("mainwindow/editorstate") :
                                             QStringLiteral("mainwindow/state"),
                    saveState());
}

void MainWindow::RestoreLayoutState()
{
  QSettings& settings = Settings::GetQSettings();
  if (m_editor_layout_active)
  {
    const QByteArray state = settings.value(QStringLiteral("mainwindow/editorstate")).toByteArray();
    if (state.isEmpty() || !restoreState(state))
    {
      ResetEditorLayout();
    }
    else
    {
      // Panels added after the layout was saved go where the default layout puts them.
      if (!restoreDockWidget(m_memory_usage_widget))
        tabifyDockWidget(m_log_widget, m_memory_usage_widget);
      if (!restoreDockWidget(m_memory_map_widget))
      {
        tabifyDockWidget(m_game_view_widget, m_memory_map_widget);
        m_game_view_widget->raise();
      }
      if (!restoreDockWidget(m_render_profiler_widget))
        tabifyDockWidget(m_log_widget, m_render_profiler_widget);
      if (!restoreDockWidget(m_frame_capture_widget))
      {
        tabifyDockWidget(m_game_view_widget, m_frame_capture_widget);
        m_game_view_widget->raise();
      }
    }
    // A saved state can't bring back the game view/library if they were somehow lost.
    m_game_library_dock->show();
    m_game_view_widget->show();
  }
  else
  {
    restoreState(settings.value(QStringLiteral("mainwindow/state")).toByteArray());
  }
  Settings::Instance().RefreshWidgetVisibility();
  QueueDockTabIconRefresh();
}

void MainWindow::RefreshGameList()
{
  Settings::Instance().ReloadTitleDB();
  Settings::Instance().RefreshGameList();
}

QStringList MainWindow::PromptFileNames()
{
  auto& settings = Settings::Instance().GetQSettings();
  QStringList paths = DolphinFileDialog::getOpenFileNames(
      this, tr("Select a File"),
      settings.value(QStringLiteral("mainwindow/lastdir"), QString{}).toString(),
      QStringLiteral("%1 (*.elf *.dol *.gcm *.bin *.iso *.tgc *.wbfs *.ciso *.gcz *.wia *.rvz "
                     "hif_000000.nfs *.wad *.dff *.m3u *.json);;%2 (*)")
          .arg(tr("All GC/Wii files"))
          .arg(tr("All Files")));

  if (!paths.isEmpty())
  {
    settings.setValue(QStringLiteral("mainwindow/lastdir"),
                      QFileInfo(paths.front()).absoluteDir().absolutePath());
  }

  return paths;
}

void MainWindow::ChangeDisc()
{
  std::vector<std::string> paths = StringListToStdVector(PromptFileNames());

  if (paths.empty())
    return;

  m_system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{m_system}, paths);
}

void MainWindow::EjectDisc()
{
  m_system.GetDVDInterface().EjectDisc(Core::CPUThreadGuard{m_system}, DVD::EjectCause::User);
}

void MainWindow::OpenUserFolder()
{
  std::string path = File::GetUserPath(D_USER_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::OpenConfigFolder()
{
  std::string path = File::GetUserPath(D_CONFIG_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::OpenCacheFolder()
{
  std::string path = File::GetUserPath(D_CACHE_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::Open()
{
  QStringList files = PromptFileNames();
  if (!files.isEmpty())
    StartGame(StringListToStdVector(files));
}

void MainWindow::Play(const std::optional<std::string>& savestate_path)
{
  // If we're in a paused game, start it up again.
  // Otherwise, play the selected game, if there is one.
  // Otherwise, play the default game.
  // Otherwise, play the last played game, if there is one.
  // Otherwise, prompt for a new game.
  if (Core::GetState(m_system) == Core::State::Paused)
  {
    Core::SetState(m_system, Core::State::Running);
  }
  else
  {
    std::shared_ptr<const UICommon::GameFile> selection = m_game_list->GetSelectedGame();
    if (selection)
    {
      StartGame(selection->GetFilePath(), ScanForSecondDisc::Yes,
                std::make_unique<BootSessionData>(savestate_path, DeleteSavestateAfterBoot::No));
    }
    else
    {
      const QString default_path = QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO));
      if (!default_path.isEmpty() && QFile::exists(default_path))
      {
        StartGame(default_path, ScanForSecondDisc::Yes,
                  std::make_unique<BootSessionData>(savestate_path, DeleteSavestateAfterBoot::No));
      }
      else
      {
        Open();
      }
    }
  }
}

void MainWindow::Pause()
{
  Core::SetState(m_system, Core::State::Paused);
}

void MainWindow::TogglePause()
{
  if (Core::GetState(m_system) == Core::State::Paused)
  {
    Play();
  }
  else
  {
    Pause();
  }
}

void MainWindow::OnStopComplete()
{
  m_stop_requested = false;
  HideRenderWidget(!m_exit_requested, m_exit_requested);
#ifdef USE_DISCORD_PRESENCE
  if (!m_netplay_dialog->isVisible())
    Discord::UpdateDiscordPresence();
#endif

  SetFullScreenResolution(false);

  if (m_exit_requested || Settings::Instance().IsBatchModeEnabled())
  {
    if (m_assembler_widget->ApplicationCloseRequest())
    {
      QGuiApplication::exit(0);
    }
    else
    {
      m_exit_requested = false;
    }
  }

  // If the current emulation prevented the booting of another, do that now
  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

bool MainWindow::RequestStop()
{
  if (Core::IsUninitialized(m_system))
  {
    Core::QueueHostJob([this](Core::System&) { OnStopComplete(); }, true);
    return true;
  }

  const bool rendered_widget_was_active =
      Settings::Instance().IsKeepWindowOnTopEnabled() ||
      (m_render_widget->isActiveWindow() && !m_render_widget->isFullScreen());
  QWidget* confirm_parent = (!m_rendering_to_main && rendered_widget_was_active) ?
                                m_render_widget :
                                static_cast<QWidget*>(this);
  const bool was_cursor_locked = m_render_widget->IsCursorLocked();

  if (!m_render_widget->isFullScreen())
    m_render_widget_geometry = m_render_widget->saveGeometry();
  else
    FullScreen();

  bool confirm_on_stop = Config::Get(Config::MAIN_CONFIRM_ON_STOP);
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  confirm_on_stop = confirm_on_stop || AchievementManager::GetInstance().CheckForModifications();
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (confirm_on_stop)
  {
    if (std::exchange(m_stop_confirm_showing, true))
      return true;

    Common::ScopeGuard confirm_lock([this] { m_stop_confirm_showing = false; });

    const Core::State state = Core::GetState(m_system);

    // Only pause the game, if NetPlay is not running
    bool pause = !Settings::Instance().GetNetPlayClient();

    if (pause)
      Core::SetState(m_system, Core::State::Paused);

    if (rendered_widget_was_active)
    {
      // We have to do this before creating the message box, otherwise we might receive the window
      // activation event before we know we need to lock the cursor again.
      m_render_widget->SetCursorLockedOnNextActivation(was_cursor_locked);
    }

    // This is to avoid any "race conditions" between the "Window Activate" message and the
    // message box returning, which could break cursor locking depending on the order
    m_render_widget->SetWaitingForMessageBox(true);
    QString message;
    if (m_stop_requested)
    {
      message = tr("A shutdown is already in progress. Unsaved data "
                   "may be lost if you stop the current emulation "
                   "before it completes. Force stop?");
    }
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
    else if (AchievementManager::GetInstance().CheckForModifications())
    {
      message = tr(
          "Do you want to stop the current emulation? Unsaved achievement modifications detected.");
    }
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
    else
    {
      message = tr("Do you want to stop the current emulation?");
    }
    auto confirm = ModalMessageBox::question(confirm_parent, tr("Confirm"), message,
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::NoButton, Qt::ApplicationModal);

    // If a user confirmed stopping the emulation, we do not capture the cursor again,
    // even if the render widget will stay alive for a while.
    // If a used rejected stopping the emulation, we instead capture the cursor again,
    // and let them continue playing as if nothing had happened
    // (assuming cursor locking is on).
    if (confirm != QMessageBox::Yes)
    {
      m_render_widget->SetWaitingForMessageBox(false);

      if (pause)
        Core::SetState(m_system, state);

      return false;
    }
    else
    {
      m_render_widget->SetCursorLockedOnNextActivation(false);
      // This needs to be after SetCursorLockedOnNextActivation(false) as it depends on it
      m_render_widget->SetWaitingForMessageBox(false);
    }
  }

  OnStopRecording();
  // TODO: Add Debugger shutdown

  if (!m_stop_requested && UICommon::TriggerSTMPowerEvent())
  {
    m_stop_requested = true;

    // Unpause because gracefully shutting down needs the game to actually request a shutdown.
    // TODO: Do not unpause in debug mode to allow debugging until the complete shutdown.
    if (Core::GetState(m_system) == Core::State::Paused)
      Core::SetState(m_system, Core::State::Running);

    // Tell NetPlay about the power event
    if (NetPlay::IsNetPlayRunning())
      NetPlay::SendPowerButtonEvent();

    return true;
  }

  ForceStop();
#ifdef Q_OS_WIN
  // Allow windows to idle or turn off display again
  SetThreadExecutionState(ES_CONTINUOUS);
#endif
  return true;
}

void MainWindow::ForceStop()
{
  Core::Stop(m_system);
}

void MainWindow::Reset()
{
  auto& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    movie.SetReset(true);
  m_system.GetProcessorInterface().ResetButton_Tap();
}

void MainWindow::FrameAdvance()
{
  Core::DoFrameStep(m_system);
}

void MainWindow::FullScreen()
{
  // If the render widget is fullscreen we want to reset it to whatever is in
  // settings. If it's set to be fullscreen then it just remakes the window,
  // which probably isn't ideal.
  bool was_fullscreen = m_render_widget->isFullScreen();

  if (!was_fullscreen)
    m_render_widget_geometry = m_render_widget->saveGeometry();

  HideRenderWidget(false);
  SetFullScreenResolution(!was_fullscreen);

  if (was_fullscreen)
  {
    ShowRenderWidget();
  }
  else
  {
    m_render_widget->showFullScreen();
  }
}

void MainWindow::UnlockCursor()
{
  if (!m_render_widget->isFullScreen())
    m_render_widget->SetCursorLocked(false);
}

void MainWindow::ScreenShot()
{
  Core::SaveScreenShot();
}

void MainWindow::ScanForSecondDiscAndStartGame(const UICommon::GameFile& game,
                                               std::unique_ptr<BootSessionData> boot_session_data)
{
  auto second_game = m_game_list->FindSecondDisc(game);

  std::vector<std::string> paths = {game.GetFilePath()};
  if (second_game != nullptr)
    paths.push_back(second_game->GetFilePath());

  StartGame(paths, std::move(boot_session_data));
}

void MainWindow::StartGame(const QString& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(path.toStdString(), scan, std::move(boot_session_data));
}

void MainWindow::StartGame(const std::string& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  if (scan == ScanForSecondDisc::Yes)
  {
    std::shared_ptr<const UICommon::GameFile> game = m_game_list->FindGame(path);
    if (game != nullptr)
    {
      ScanForSecondDiscAndStartGame(*game, std::move(boot_session_data));
      return;
    }
  }

  StartGame(BootParameters::GenerateFromFile(
      path, boot_session_data ? std::move(*boot_session_data) : BootSessionData()));
}

void MainWindow::StartGame(const std::vector<std::string>& paths,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(BootParameters::GenerateFromFile(
      paths, boot_session_data ? std::move(*boot_session_data) : BootSessionData()));
}

void MainWindow::StartGame(std::unique_ptr<BootParameters>&& parameters)
{
  if (parameters && std::holds_alternative<BootParameters::Disc>(parameters->parameters))
  {
    if (std::get<BootParameters::Disc>(parameters->parameters).volume->IsNKit())
    {
      if (!NKitWarningDialog::ShowUnlessDisabled())
        return;
    }

    const auto volume_type =
        std::get<BootParameters::Disc>(parameters->parameters).volume->GetVolumeType();
    if (volume_type != DiscIO::Platform::Triforce)
    {
      const bool triforce_hardware_sp1 =
          Config::Get(Config::MAIN_SERIAL_PORT_1) == ExpansionInterface::EXIDeviceType::Baseboard;
      const bool triforce_hardware_port_1 = Config::Get(Config::GetInfoForSIDevice(0)) ==
                                            SerialInterface::SIDevices::SIDEVICE_AM_BASEBOARD;

      // Some Triforce tools don't include a boot.id file, but they can still be launched.
      if (triforce_hardware_sp1)
      {
        ModalMessageBox::warning(this, tr("Warning"),
                                 tr("Non-Triforce games cannot be booted with Triforce hardware "
                                    "attached.\nPlease remove the Triforce Baseboard from SP1."),
                                 QMessageBox::Ok);
      }
      if (triforce_hardware_port_1)
      {
        ModalMessageBox::warning(this, tr("Warning"),
                                 tr("Non-Triforce games cannot be booted with Triforce hardware "
                                    "attached.\nPlease remove the Triforce Baseboard from Port 1."),
                                 QMessageBox::Ok);
      }
    }
  }

  // If we're running, only start a new game once we've stopped the last.
  if (!Core::IsUninitialized(m_system))
  {
    if (!RequestStop())
      return;

    // As long as the shutdown isn't complete, we can't boot, so let's boot later
    m_pending_boot = std::move(parameters);
    return;
  }

  // Testing a homebrew build: open the project it belongs to so its files, build output and
  // diagnostics are right next to the running game.
  if (parameters && m_editor_layout_active &&
      std::holds_alternative<BootParameters::Executable>(parameters->parameters))
  {
    OpenProjectForExecutable(
        QString::fromStdString(std::get<BootParameters::Executable>(parameters->parameters).path));
  }

  // The memory tools need to know where the program's sections are.
  if (parameters)
    m_memory_analyzer->SetExecutableLayout(MemoryAnalyzer::ReadExecutableLayout(*parameters));

  // We need the render widget before booting.
  ShowRenderWidget();

  // Boot up, show an error if it fails to load the game.
  if (!BootManager::BootCore(m_system, std::move(parameters),
                             ::GetWindowSystemInfo(m_render_widget->windowHandle())))
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to init core"), QMessageBox::Ok);
    HideRenderWidget();
    return;
  }

#ifdef USE_DISCORD_PRESENCE
  if (!NetPlay::IsNetPlayRunning())
    Discord::UpdateDiscordPresence();
#endif

  if (Config::Get(Config::MAIN_FULLSCREEN))
    m_fullscreen_requested = true;
}

void MainWindow::SetFullScreenResolution(bool fullscreen)
{
  if (Config::Get(Config::MAIN_FULLSCREEN_DISPLAY_RES) == "Auto")
    return;
#ifdef _WIN32

  if (!fullscreen)
  {
    ChangeDisplaySettings(nullptr, CDS_FULLSCREEN);
    return;
  }

  DEVMODE screen_settings;
  memset(&screen_settings, 0, sizeof(screen_settings));
  screen_settings.dmSize = sizeof(screen_settings);
  sscanf(Config::Get(Config::MAIN_FULLSCREEN_DISPLAY_RES).c_str(), "%lux%lu",
         &screen_settings.dmPelsWidth, &screen_settings.dmPelsHeight);
  screen_settings.dmBitsPerPel = 32;
  screen_settings.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

  // Try To Set Selected Mode And Get Results.  NOTE: CDS_FULLSCREEN Gets Rid Of Start Bar.
  ChangeDisplaySettings(&screen_settings, CDS_FULLSCREEN);
#elif defined(HAVE_XRANDR) && HAVE_XRANDR
  if (m_xrr_config)
    m_xrr_config->ToggleDisplayMode(fullscreen);
#endif
}

void MainWindow::ShowRenderWidget()
{
  SetFullScreenResolution(false);
  Host::GetInstance()->SetRenderFullscreen(false);

  if (m_editor_layout_active)
  {
    // Editor layout: the game renders inside the dockable Game View panel.
    m_rendering_to_main = true;

    m_game_view_widget->AttachRenderWidget(m_render_widget);

    Host::GetInstance()->SetRenderFocus(m_game_view_widget->isActiveWindow());
  }
  else if (Config::Get(Config::MAIN_RENDER_TO_MAIN))
  {
    // If we're rendering to main, add it to the stack and keep the tool title stable.
    m_rendering_to_main = true;

    m_stack->setCurrentIndex(m_stack->addWidget(m_render_widget));
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_stack->repaint();

    Host::GetInstance()->SetRenderFocus(isActiveWindow());
  }
  else
  {
    // Otherwise, just show it.
    m_rendering_to_main = false;

    m_render_widget->showNormal();
    m_render_widget->restoreGeometry(m_render_widget_geometry);
  }
}

void MainWindow::HideRenderWidget(bool reinit, bool is_exit)
{
  if (m_rendering_to_main && m_game_view_widget->HasRenderWidget())
  {
    m_game_view_widget->DetachRenderWidget(m_render_widget);
    m_rendering_to_main = false;
    UpdateWindowTitle();
  }
  else if (m_rendering_to_main)
  {
    // Remove the widget from the stack and reparent it to nullptr, so that it can draw
    // itself in a new window if it wants.
    m_stack->removeWidget(m_render_widget);
    m_render_widget->setParent(nullptr);
    m_rendering_to_main = false;
    m_stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    UpdateWindowTitle();
  }

  // The following code works around a driver bug that would lead to Dolphin crashing when changing
  // graphics backends (e.g. OpenGL to Vulkan). To avoid this the render widget is (safely)
  // recreated
  if (reinit)
  {
    m_render_widget->hide();
    disconnect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);

    m_render_widget->removeEventFilter(this);
    m_render_widget->deleteLater();

    m_render_widget = new RenderWidget;

    m_render_widget->installEventFilter(this);
    connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
    connect(m_render_widget, &RenderWidget::FocusChanged, this, [this](bool focus) {
      if (m_render_widget->isFullScreen())
        SetFullScreenResolution(focus);
    });

    // The controller interface will still be registered to the old render widget, if the core
    // has booted. Therefore, we should re-bind it to the main window for now. When the core
    // is next started, it will be swapped back to the new render widget.
    g_controller_interface.ChangeWindow(::GetWindowSystemInfo(windowHandle()).render_window,
                                        is_exit ? ControllerInterface::WindowChangeReason::Exit :
                                                  ControllerInterface::WindowChangeReason::Other);
  }
}

void MainWindow::ShowControllersWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Controllers);
}

void MainWindow::ShowTriforceWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Triforce);
}

void MainWindow::ShowFreeLookWindow()
{
  if (!m_freelook_window)
  {
    m_freelook_window = new FreeLookWindow(this);
    InstallHotkeyFilter(m_freelook_window);

#ifdef USE_RETRO_ACHIEVEMENTS
    connect(m_freelook_window, &FreeLookWindow::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  }

  m_freelook_window->show();
  m_freelook_window->raise();
  m_freelook_window->activateWindow();
}

void MainWindow::ShowSettingsWindow()
{
  if (!m_settings_window)
  {
#ifdef HAVE_XRANDR
    if (GetWindowSystemType() == WindowSystemType::X11)
    {
      m_xrr_config = std::make_unique<X11Utils::XRRConfiguration>(
          static_cast<Display*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow(
              "display", windowHandle())),
          winId());
    }
#endif
    m_settings_window = new SettingsWindow(this);
    InstallHotkeyFilter(m_settings_window);
  }

  m_settings_window->show();
  m_settings_window->raise();
  m_settings_window->activateWindow();
}

void MainWindow::ShowAudioWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Audio);
}

void MainWindow::ShowGeneralWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::General);
}

void MainWindow::ShowOSDWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::OnScreenDisplay);
}

void MainWindow::ShowAboutDialog()
{
  AboutDialog about{this};
  about.exec();
}

void MainWindow::ShowHotkeyDialog()
{
  if (!m_hotkey_window)
  {
    m_hotkey_window = new MappingWindow(this, MappingWindow::Type::MAPPING_HOTKEYS, 0);
    InstallHotkeyFilter(m_hotkey_window);
  }

  m_hotkey_window->show();
  m_hotkey_window->raise();
  m_hotkey_window->activateWindow();
}

void MainWindow::ShowGraphicsWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Graphics);
}

void MainWindow::ShowNetPlaySetupDialog()
{
  m_netplay_setup_dialog->show();
  m_netplay_setup_dialog->raise();
  m_netplay_setup_dialog->activateWindow();
}

void MainWindow::ShowNetPlayBrowser()
{
  auto* browser = new NetPlayBrowser(this);
  browser->setAttribute(Qt::WA_DeleteOnClose, true);
  connect(browser, &NetPlayBrowser::Join, this, &MainWindow::NetPlayJoin);
  browser->exec();
}

void MainWindow::ShowFIFOPlayer()
{
  if (!m_fifo_window)
  {
    m_fifo_window.reset(new FIFOPlayerWindow(m_system.GetFifoPlayer(), m_system.GetFifoRecorder()));
    connect(m_fifo_window.get(), &FIFOPlayerWindow::LoadFIFORequested, this,
            [this](const QString& path) { StartGame(path, ScanForSecondDisc::No); });
  }

  m_fifo_window->show();
  m_fifo_window->raise();
  m_fifo_window->activateWindow();
}

void MainWindow::ShowSkylanderPortal()
{
  if (!m_skylander_window)
  {
    m_skylander_window = new SkylanderPortalWindow();
  }

  m_skylander_window->show();
  m_skylander_window->raise();
  m_skylander_window->activateWindow();
}

void MainWindow::ShowInfinityBase()
{
  if (!m_infinity_window)
  {
    m_infinity_window = new InfinityBaseWindow();
  }

  m_infinity_window->show();
  m_infinity_window->raise();
  m_infinity_window->activateWindow();
}

void MainWindow::ShowWiiSpeakWindow()
{
  if (!m_wii_speak_window)
  {
    m_wii_speak_window = new WiiSpeakWindow();
  }

  m_wii_speak_window->show();
  m_wii_speak_window->raise();
  m_wii_speak_window->activateWindow();
}

void MainWindow::ShowLogitechMicWindow()
{
  if (!m_logitech_mic_window)
  {
    m_logitech_mic_window = new LogitechMicWindow();
  }

  m_logitech_mic_window->show();
  m_logitech_mic_window->raise();
  m_logitech_mic_window->activateWindow();
}

void MainWindow::StateLoad()
{
  QString dialog_path = (Config::Get(Config::MAIN_CURRENT_STATE_PATH).empty()) ?
                            QDir::currentPath() :
                            QString::fromStdString(Config::Get(Config::MAIN_CURRENT_STATE_PATH));
  QString path = DolphinFileDialog::getOpenFileName(
      this, tr("Select a File"), dialog_path, tr("All Save States (*.sav *.s??);; All Files (*)"));
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(path).dir().path().toStdString());
  if (!path.isEmpty())
    State::LoadAs(m_system, path.toStdString());
}

void MainWindow::StateSave()
{
  QString dialog_path = (Config::Get(Config::MAIN_CURRENT_STATE_PATH).empty()) ?
                            QDir::currentPath() :
                            QString::fromStdString(Config::Get(Config::MAIN_CURRENT_STATE_PATH));
  QString path = DolphinFileDialog::getSaveFileName(
      this, tr("Select a File"), dialog_path, tr("All Save States (*.sav *.s??);; All Files (*)"));
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(path).dir().path().toStdString());
  if (!path.isEmpty())
    State::SaveAs(m_system, path.toStdString());
}

void MainWindow::StateLoadSlot()
{
  State::Load(m_system, m_state_slot);
}

void MainWindow::StateSaveSlot()
{
  State::Save(m_system, m_state_slot);
}

void MainWindow::StateLoadSlotAt(int slot)
{
  State::Load(m_system, slot);
}

void MainWindow::StateLoadLastSavedAt(int slot)
{
  State::LoadLastSaved(m_system, slot);
}

void MainWindow::StateSaveSlotAt(int slot)
{
  State::Save(m_system, slot);
}

void MainWindow::StateLoadUndo()
{
  State::UndoLoadState(m_system);
}

void MainWindow::StateSaveUndo()
{
  State::UndoSaveState(m_system);
}

void MainWindow::StateSaveOldest()
{
  State::SaveFirstSaved(m_system);
}

void MainWindow::SetStateSlot(int slot)
{
  Settings::Instance().SetStateSlot(slot);
  m_state_slot = slot;

  Core::DisplayMessage(fmt::format("Selected slot {} - {}", m_state_slot,
                                   State::GetInfoStringOfSlot(m_state_slot, false)),
                       2500);
}

void MainWindow::IncrementSelectedStateSlot()
{
  u32 state_slot = m_state_slot + 1;
  if (state_slot > State::NUM_STATES)
    state_slot = 1;
  m_menu_bar->SetStateSlot(state_slot);
}

void MainWindow::DecrementSelectedStateSlot()
{
  u32 state_slot = m_state_slot - 1;
  if (state_slot < 1)
    state_slot = State::NUM_STATES;
  m_menu_bar->SetStateSlot(state_slot);
}

void MainWindow::PerformOnlineUpdate(const std::string& region)
{
  WiiUpdate::PerformOnlineUpdate(region, this);
  // Since the update may have installed a newer system menu, trigger a refresh.
  Settings::Instance().NANDRefresh();
}

void MainWindow::BootWiiSystemMenu()
{
  StartGame(std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU}));
}

void MainWindow::NetPlayInit()
{
  const auto& game_list_model = m_game_list->GetGameListModel();
  m_netplay_setup_dialog = new NetPlaySetupDialog(game_list_model, this);
  m_netplay_dialog = new NetPlayDialog(
      game_list_model,
      [this](const std::string& path, std::unique_ptr<BootSessionData> boot_session_data) {
        StartGame(path, ScanForSecondDisc::Yes, std::move(boot_session_data));
      });
#ifdef USE_DISCORD_PRESENCE
  m_netplay_discord = new DiscordHandler(this);
#endif

  connect(m_netplay_dialog, &NetPlayDialog::Stop, this, &MainWindow::ForceStop);
  connect(m_netplay_dialog, &NetPlayDialog::rejected, this, &MainWindow::NetPlayQuit);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Join, this, &MainWindow::NetPlayJoin);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Host, this, &MainWindow::NetPlayHost);
#ifdef USE_DISCORD_PRESENCE
  connect(m_netplay_discord, &DiscordHandler::Join, this, &MainWindow::NetPlayJoin);

  Discord::InitNetPlayFunctionality(*m_netplay_discord);
  m_netplay_discord->Start();
#endif
  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &MainWindow::UpdateScreenSaverInhibition);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &MainWindow::UpdateScreenSaverInhibition);
}

bool MainWindow::NetPlayJoin()
{
  if (!Core::IsUninitialized(m_system))
  {
    ModalMessageBox::critical(nullptr, tr("Error"),
                              tr("Can't start a NetPlay Session while a game is still running!"));
    return false;
  }

  if (m_netplay_dialog->isVisible())
  {
    ModalMessageBox::critical(nullptr, tr("Error"),
                              tr("A NetPlay Session is already in progress!"));
    return false;
  }

  auto server = Settings::Instance().GetNetPlayServer();

  // Settings
  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const bool is_traversal = traversal_choice == "traversal";

  std::string host_ip;
  u16 host_port;
  if (server)
  {
    host_ip = "127.0.0.1";
    host_port = server->GetPort();
  }
  else
  {
    host_ip = is_traversal ? Config::Get(Config::NETPLAY_HOST_CODE) :
                             Config::Get(Config::NETPLAY_ADDRESS);
    host_port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  }

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);
  const std::string network_mode = Config::Get(Config::NETPLAY_NETWORK_MODE);
  const bool host_input_authority = network_mode == "hostinputauthority" || network_mode == "golf";

  if (server)
  {
    server->SetHostInputAuthority(host_input_authority);
    server->AdjustPadBufferSize(Config::Get(Config::NETPLAY_BUFFER_SIZE));
  }

  // Create Client
  const bool is_hosting_netplay = server != nullptr;
  Settings::Instance().ResetNetPlayClient(new NetPlay::NetPlayClient(
      host_ip, host_port, m_netplay_dialog, nickname,
      NetPlay::NetTraversalConfig{is_hosting_netplay ? false : is_traversal, traversal_host,
                                  traversal_port}));

  if (!Settings::Instance().GetNetPlayClient()->IsConnected())
  {
    NetPlayQuit();
    return false;
  }

  m_netplay_setup_dialog->close();
  m_netplay_dialog->show(nickname, is_traversal);

  return true;
}

bool MainWindow::NetPlayHost(const UICommon::GameFile& game)
{
  if (!Core::IsUninitialized(m_system))
  {
    ModalMessageBox::critical(nullptr, tr("Error"),
                              tr("Can't start a NetPlay Session while a game is still running!"));
    return false;
  }

  if (m_netplay_dialog->isVisible())
  {
    ModalMessageBox::critical(nullptr, tr("Error"),
                              tr("A NetPlay Session is already in progress!"));
    return false;
  }

  // Settings
  u16 host_port = Config::Get(Config::NETPLAY_HOST_PORT);
  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const bool is_traversal = traversal_choice == "traversal";
  const bool use_upnp = Config::Get(Config::NETPLAY_USE_UPNP);

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const u16 traversal_port_alt = Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT);

  if (is_traversal)
    host_port = Config::Get(Config::NETPLAY_LISTEN_PORT);

  // Create Server
  Settings::Instance().ResetNetPlayServer(
      new NetPlay::NetPlayServer(host_port, use_upnp, m_netplay_dialog,
                                 NetPlay::NetTraversalConfig{is_traversal, traversal_host,
                                                             traversal_port, traversal_port_alt}));

  if (!Settings::Instance().GetNetPlayServer()->is_connected)
  {
    ModalMessageBox::critical(
        nullptr, tr("Failed to open server"),
        tr("Failed to listen on port %1. Is another instance of the NetPlay server running?")
            .arg(host_port));
    NetPlayQuit();
    return false;
  }

  Settings::Instance().GetNetPlayServer()->ChangeGame(game.GetSyncIdentifier(),
                                                      m_game_list->GetNetPlayName(game));

  // Join our local server
  return NetPlayJoin();
}

void MainWindow::NetPlayQuit()
{
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void MainWindow::UpdateScreenSaverInhibition()
{
  const bool inhibit = Config::Get(Config::MAIN_DISABLE_SCREENSAVER) &&
                       (Core::GetState(m_system) == Core::State::Running);

  if (inhibit == m_is_screensaver_inhibited)
    return;

  m_is_screensaver_inhibited = inhibit;

  UICommon::InhibitScreenSaver(inhibit);
}

bool MainWindow::eventFilter(QObject* object, QEvent* event)
{
  if (object == this && m_keep_default_dock_sizes)
  {
    // Dock separators aren't widgets, so a press on the main window itself is the user starting
    // to resize the docks: from then on, their sizes are the user's.
    if (event->type() == QEvent::MouseButtonPress)
      m_keep_default_dock_sizes = false;
    else if (event->type() == QEvent::Resize && m_editor_layout_active)
      QueueDefaultDockSizes();
  }

  if (event->type() == QEvent::Close)
  {
    if (RequestStop() && object == this)
      m_exit_requested = true;

    static_cast<QCloseEvent*>(event)->ignore();
    return true;
  }

  return false;
}

QMenu* MainWindow::createPopupMenu()
{
  // Disable the default popup menu as it exposes the debugger UI even when the debugger UI is
  // disabled, which can lead to user confusion (see e.g. https://bugs.dolphin-emu.org/issues/13306)
  return nullptr;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1)
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QList<QUrl>& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  QStringList files;
  QStringList folders;

  for (const QUrl& url : urls)
  {
    QFileInfo file_info(url.toLocalFile());
    QString path = file_info.filePath();

    if (!file_info.exists() || !file_info.isReadable())
    {
      ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
      return;
    }

    (file_info.isFile() ? files : folders).append(path);
  }

  if (!files.isEmpty())
  {
    StartGame(StringListToStdVector(files));
  }
  else
  {
    Settings& settings = Settings::Instance();
    const bool show_confirm = !settings.GetPaths().empty();

    for (const QString& folder : folders)
    {
      if (show_confirm)
      {
        if (ModalMessageBox::question(
                this, tr("Confirm"),
                tr("Do you want to add \"%1\" to the list of Game Paths?").arg(folder)) !=
            QMessageBox::Yes)
          return;
      }
      settings.AddPath(folder);
    }
  }
}

QSize MainWindow::sizeHint() const
{
  return QSize(800, 600);
}

void MainWindow::OnBootGameCubeIPL(DiscIO::Region region)
{
  StartGame(std::make_unique<BootParameters>(BootParameters::IPL{region}));
}

void MainWindow::OnImportNANDBackup()
{
  auto response = ModalMessageBox::question(
      this, tr("Question"),
      tr("Merging a new NAND over your currently selected NAND will overwrite any channels "
         "and savegames that already exist. This process is not reversible, so it is "
         "recommended that you keep backups of both NANDs. Are you sure you want to "
         "continue?"));

  if (response == QMessageBox::No)
    return;

  QString file =
      DolphinFileDialog::getOpenFileName(this, tr("Select NAND Backup"), QDir::currentPath(),
                                         tr("BootMii NAND backup file (*.bin);;"
                                            "All Files (*)"));

  if (file.isEmpty())
    return;

  ParallelProgressDialog dialog(this);
  dialog.GetRaw()->setWindowTitle(tr("Importing NAND backup"));

  std::future<void> result = std::async(std::launch::async, [&] {
    DiscIO::NANDImporter().ImportNANDBin(
        file.toStdString(),
        [&dialog](DiscIO::NANDImporter::Step step, u32 cur, u32 max) {
          switch (step)
          {
          case DiscIO::NANDImporter::Step::Loading:
            dialog.SetLabelText(tr("Loading NAND..."));
            break;
          case DiscIO::NANDImporter::Step::Extracting:
            dialog.SetLabelText(tr("Extracting NAND..."));
            break;
          }
          dialog.SetValue(cur);
          dialog.SetMaximum(max);
          return dialog.WasCanceled();
        },
        [this] {
          std::optional<std::string> keys_file = RunOnObject(this, [this] {
            return DolphinFileDialog::getOpenFileName(
                       this, tr("Select Keys File (OTP/SEEPROM Dump)"), QDir::currentPath(),
                       tr("BootMii keys file (*.bin);;"
                          "All Files (*)"))
                .toStdString();
          });
          if (keys_file)
            return *keys_file;
          return std::string("");
        });
    dialog.Reset();
  });

  dialog.GetRaw()->exec();

  result.wait();

  m_menu_bar->UpdateToolsMenu(Core::State::Uninitialized);
}

void MainWindow::OnPlayRecording()
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    ModalMessageBox::critical(
        this, tr("Error"),
        tr("Playback of input recordings is disabled in RetroAchievements hardcore mode."));
    return;
  }

  QString dtm_file = DolphinFileDialog::getOpenFileName(
      this, tr("Select the Recording File to Play"), QString(), tr("Dolphin TAS Movies (*.dtm)"));

  if (dtm_file.isEmpty())
    return;

  auto& movie = m_system.GetMovie();
  if (!movie.IsReadOnly())
  {
    // let's make the read-only flag consistent at the start of a movie.
    movie.SetReadOnly(true);
    emit ReadOnlyModeChanged(true);
  }

  std::optional<std::string> savestate_path;
  if (movie.PlayInput(dtm_file.toStdString(), &savestate_path))
  {
    emit RecordingStatusChanged(true);

    Play(savestate_path);
  }
}

void MainWindow::OnStartRecording()
{
  auto& movie = m_system.GetMovie();
  if (Core::GetState(m_system) == Core::State::Starting ||
      Core::GetState(m_system) == Core::State::Stopping || movie.IsRecordingInput() ||
      movie.IsPlayingInput())
  {
    return;
  }

  if (movie.IsReadOnly())
  {
    // The user just chose to record a movie, so that should take precedence
    movie.SetReadOnly(false);
    emit ReadOnlyModeChanged(true);
  }

  Movie::ControllerTypeArray controllers{};
  Movie::WiimoteEnabledArray wiimotes{};

  for (int i = 0; i < 4; i++)
  {
    const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[i] = Movie::ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(si_device))
      controllers[i] = Movie::ControllerType::GC;
    else
      controllers[i] = Movie::ControllerType::None;
    wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
  }

  if (movie.BeginRecordingInput(controllers, wiimotes))
  {
    emit RecordingStatusChanged(true);

    if (Core::IsUninitialized(m_system))
      Play();
  }
}

void MainWindow::OnStopRecording()
{
  auto& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    OnExportRecording();
  if (movie.IsMovieActive())
    movie.EndPlayInput(false);
  emit RecordingStatusChanged(false);
}

void MainWindow::OnExportRecording()
{
  const Core::CPUThreadGuard guard(m_system);

  QString dtm_file = DolphinFileDialog::getSaveFileName(
      this, tr("Save Recording File As"), QString(), tr("Dolphin TAS Movies (*.dtm)"));
  if (!dtm_file.isEmpty())
    m_system.GetMovie().SaveRecording(dtm_file.toStdString());
}

void MainWindow::OnActivateChat()
{
  if (g_netplay_chat_ui)
    g_netplay_chat_ui->Activate();
}

void MainWindow::OnRequestGolfControl()
{
  auto client = Settings::Instance().GetNetPlayClient();
  if (client)
    client->RequestGolfControl();
}

void MainWindow::ShowTASInput()
{
  for (int i = 0; i < num_gc_controllers; i++)
  {
    const auto si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
    {
      m_gba_tas_input_windows[i]->show();
      m_gba_tas_input_windows[i]->raise();
      m_gba_tas_input_windows[i]->activateWindow();
    }
    else if (si_device != SerialInterface::SIDEVICE_NONE &&
             si_device != SerialInterface::SIDEVICE_GC_GBA)
    {
      m_gc_tas_input_windows[i]->show();
      m_gc_tas_input_windows[i]->raise();
      m_gc_tas_input_windows[i]->activateWindow();
    }
  }

  for (int i = 0; i < num_wii_controllers; i++)
  {
    if (Config::Get(Config::GetInfoForWiimoteSource(i)) == WiimoteSource::Emulated &&
        (!Core::IsRunning(m_system) || m_system.IsWii()))
    {
      m_wii_tas_input_windows[i]->show();
      m_wii_tas_input_windows[i]->raise();
      m_wii_tas_input_windows[i]->activateWindow();
    }
  }
}

void MainWindow::OnConnectWiiRemote(int id)
{
  const Core::CPUThreadGuard guard(m_system);
  if (const auto bt = WiiUtils::GetBluetoothEmuDevice())
  {
    const auto wm = bt->AccessWiimoteByIndex(id);
    wm->Activate(!wm->IsConnected());
  }
}

#ifdef USE_RETRO_ACHIEVEMENTS
void MainWindow::ShowAchievementsWindow()
{
  if (!m_achievements_window)
  {
    m_achievements_window = new AchievementsWindow(this);
  }

  m_achievements_window->show();
  m_achievements_window->raise();
  m_achievements_window->activateWindow();
  m_achievements_window->UpdateData(AchievementManager::UpdatedItems{.all = true});
}

void MainWindow::ShowAchievementSettings()
{
  ShowAchievementsWindow();
  m_achievements_window->ForceSettingsTab();
}

void MainWindow::OnHardcoreChanged()
{
  bool hardcore_active = AchievementManager::GetInstance().IsHardcoreModeActive();
  if (hardcore_active)
    Settings::Instance().SetDebugModeEnabled(false);
  // EmulationStateChanged causes several dialogs to redraw, including anything affected by hardcore
  // mode. Every dialog that depends on hardcore mode is redrawn by EmulationStateChanged.
  if (hardcore_active != m_former_hardcore_setting)
    emit Settings::Instance().EmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  m_former_hardcore_setting = hardcore_active;
}
#endif  // USE_RETRO_ACHIEVEMENTS

void MainWindow::ShowMemcardManager()
{
  GCMemcardManager manager(this);

  manager.exec();
}

void MainWindow::ShowResourcePackManager()
{
  ResourcePackManager manager(this);

  manager.exec();
}

void MainWindow::ShowCheatsManager()
{
  if (!m_cheats_manager)
  {
    m_cheats_manager = new CheatsManager(m_system, this);

    connect(m_cheats_manager, &CheatsManager::ShowMemory, m_memory_widget,
            &MemoryWidget::SetAddress);
    connect(m_cheats_manager, &CheatsManager::RequestWatch, m_watch_widget, &WatchWidget::AddWatch);
    connect(m_cheats_manager, &CheatsManager::OpenGeneralSettings, this,
            &MainWindow::ShowGeneralWindow);

#ifdef USE_RETRO_ACHIEVEMENTS
    connect(m_cheats_manager, &CheatsManager::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  }

  m_cheats_manager->show();
}

void MainWindow::ShowRiivolutionBootWidget(const UICommon::GameFile& game)
{
  auto second_game = m_game_list->FindSecondDisc(game);
  std::vector<std::string> paths = {game.GetFilePath()};
  if (second_game != nullptr)
    paths.push_back(second_game->GetFilePath());
  std::unique_ptr<BootParameters> boot_params = BootParameters::GenerateFromFile(paths);
  if (!boot_params)
    return;
  if (!std::holds_alternative<BootParameters::Disc>(boot_params->parameters))
    return;

  auto& disc = std::get<BootParameters::Disc>(boot_params->parameters);
  RiivolutionBootWidget w(disc.volume->GetGameID(), disc.volume->GetRevision(),
                          disc.volume->GetDiscNumber(), game.GetFilePath(), this);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(&w, &RiivolutionBootWidget::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  w.exec();
  if (!w.ShouldBoot())
    return;

  AddRiivolutionPatches(boot_params.get(), std::move(w.GetPatches()));
  StartGame(std::move(boot_params));
}
