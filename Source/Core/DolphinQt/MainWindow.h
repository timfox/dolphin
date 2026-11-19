// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QMainWindow>
#include <QStringList>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef USE_RETRO_ACHIEVEMENTS
#include "Common/Config/Config.h"
#endif  // USE_RETRO_ACHIEVEMENTS

#include "Core/Boot/Boot.h"
#include "DolphinQt/FIFO/FIFOPlayerWindow.h"

class QDockWidget;
class QMenu;
class QStackedWidget;
class QString;

class AchievementsWindow;
class AssemblerWidget;
class BreakpointWidget;
struct BootParameters;
class CheatsManager;
class CodeWidget;
class DiscordHandler;
class DragEnterEvent;
class FreeLookWindow;
class GameCount;
class GameList;
class GameViewWidget;
class BuildOutputWidget;
class DevkitBuilder;
class DevkitProject;
class DevkitStatusBar;
class DiagnosticsWidget;
class FrameCaptureWidget;
class ProjectWidget;
class PropertyInspectorWidget;
class TargetToolsWidget;
class GBATASInputWindow;
class GCTASInputWindow;
class HotkeyScheduler;
class InfinityBaseWindow;
class JITWidget;
class LogConfigWidget;
class LogWidget;
class MappingWindow;
class MemoryWidget;
class MenuBar;
class NetPlayDialog;
class NetPlaySetupDialog;
class NetworkWidget;
class RegisterWidget;
class RenderProfilerWidget;
class RenderWidget;
class SearchBar;
class Settings;
class SettingsWindow;
class SkylanderPortalWindow;
class ThreadWidget;
class ToolBar;
class WatchWidget;
class WiiTASInputWindow;
class WiiSpeakWindow;
class LogitechMicWindow;
struct WindowSystemInfo;

namespace Core
{
class System;
}

namespace DiscIO
{
enum class Region;
}

namespace UICommon
{
class GameFile;
}

namespace X11Utils
{
class XRRConfiguration;
}

class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(Core::System& system, std::unique_ptr<BootParameters> boot_parameters,
                      const std::string& movie_path);
  ~MainWindow() override;

  WindowSystemInfo GetWindowSystemInfo() const;

  bool eventFilter(QObject* object, QEvent* event) override;
  QMenu* createPopupMenu() override;

  void ShowTriforceWindow();

signals:
  void ReadOnlyModeChanged(bool read_only);
  void RecordingStatusChanged(bool recording);

private:
  void Open();
  void RefreshGameList();
  void Play(const std::optional<std::string>& savestate_path = {});
  void Pause();
  void TogglePause();

  // May ask for confirmation. Returns whether or not it actually stopped.
  bool RequestStop();
  void ForceStop();
  void Reset();
  void FrameAdvance();
  void StateLoad();
  void StateSave();
  void StateLoadSlot();
  void StateSaveSlot();
  void StateLoadSlotAt(int slot);
  void StateSaveSlotAt(int slot);
  void StateLoadLastSavedAt(int slot);
  void StateLoadUndo();
  void StateSaveUndo();
  void StateSaveOldest();
  void SetStateSlot(int slot);
  void IncrementSelectedStateSlot();
  void DecrementSelectedStateSlot();
  void BootWiiSystemMenu();

  void PerformOnlineUpdate(const std::string& region);

  void SetFullScreenResolution(bool fullscreen);

  void FullScreen();
  void UnlockCursor();
  void ScreenShot();

  void CreateComponents();

  void ConnectGameList();
  void ConnectHost();
  void ConnectHotkeys();
  void ConnectMenuBar();
  void ConnectRenderWidget();
  void ConnectStack();

  // Editor (devkit) layout: game view, game library and tools are all dock panels.
  void ApplyLayoutMode(bool editor_layout);
  void SetEditorLayoutEnabled(bool enabled);
  void ResetEditorLayout();
  void CreateDevkitPanels();
  void CreateProjectMenu();
  void UpdateProjectActions();
  void UpdateWindowTitle();
  void QueueDefaultDockSizes();
  void NewProject();
  void OpenProject();
  void OpenProjectForExecutable(const QString& executable_path);
  void ShowProjectSettings();
  // Builds the open project; if |then_run|, boots the result when the build succeeds.
  void BuildProject(bool then_run);
  void CleanProject();
  void RunCommandForProject(const QString& command, const QString& title, bool then_run);
  void BootProjectOutput();
  void ApplyDockTitleBars(bool editor_layout);
  void QueueDockTabIconRefresh();
  void RefreshDockTabIcons();
  void ShowDebugPanel(QDockWidget* dock, void (Settings::*set_visible)(bool));
  void SaveLayoutState() const;
  void RestoreLayoutState();
  void ConnectToolBar();

  void InitControllers();
  void ShutdownControllers();

  void InitCoreCallbacks();

  enum class ScanForSecondDisc
  {
    Yes,
    No,
  };

  void ScanForSecondDiscAndStartGame(const UICommon::GameFile& game,
                                     std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const QString& path, ScanForSecondDisc scan,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const std::string& path, ScanForSecondDisc scan,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const std::vector<std::string>& paths,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(std::unique_ptr<BootParameters>&& parameters);
  void ShowRenderWidget();
  void HideRenderWidget(bool reinit = true, bool is_exit = false);

  void ShowSettingsWindow();
  void ShowGeneralWindow();
  void ShowAudioWindow();
  void ShowControllersWindow();
  void ShowGraphicsWindow();
  void ShowFreeLookWindow();
  void ShowAboutDialog();
  void ShowHotkeyDialog();
  void ShowNetPlaySetupDialog();
  void ShowNetPlayBrowser();
  void ShowFIFOPlayer();
  void ShowSkylanderPortal();
  void ShowInfinityBase();
  void ShowWiiSpeakWindow();
  void ShowLogitechMicWindow();
  void ShowMemcardManager();
  void ShowResourcePackManager();
  void ShowCheatsManager();
  void ShowRiivolutionBootWidget(const UICommon::GameFile& game);

#ifdef USE_RETRO_ACHIEVEMENTS
  void ShowAchievementsWindow();
  void ShowAchievementSettings();
  void OnHardcoreChanged();
#endif  // USE_RETRO_ACHIEVEMENTS

  void NetPlayInit();
  bool NetPlayJoin();
  bool NetPlayHost(const UICommon::GameFile& game);
  void NetPlayQuit();

  void OnBootGameCubeIPL(DiscIO::Region region);
  void OnImportNANDBackup();
  void OnConnectWiiRemote(int id);

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
  void OnSignal();
#endif

  void OnPlayRecording();
  void OnStartRecording();
  void OnStopRecording();
  void OnExportRecording();
  void OnActivateChat();
  void OnRequestGolfControl();
  void ShowTASInput();
  void ShowOSDWindow();

  void ChangeDisc();
  void EjectDisc();

  void OpenUserFolder();
  void OpenConfigFolder();
  void OpenCacheFolder();

  QStringList PromptFileNames();

  void UpdateScreenSaverInhibition();

  void OnStopComplete();
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  QSize sizeHint() const override;

  Core::System& m_system;

#ifdef HAVE_XRANDR
  std::unique_ptr<X11Utils::XRRConfiguration> m_xrr_config;
#endif

  QStackedWidget* m_stack;
  QWidget* m_editor_central_placeholder = nullptr;
  QDockWidget* m_game_library_dock = nullptr;
  GameViewWidget* m_game_view_widget = nullptr;
  TargetToolsWidget* m_target_tools_widget = nullptr;
  PropertyInspectorWidget* m_property_inspector_widget = nullptr;
  DevkitStatusBar* m_devkit_status_bar = nullptr;
  DevkitProject* m_project = nullptr;
  DevkitBuilder* m_builder = nullptr;
  ProjectWidget* m_project_widget = nullptr;
  BuildOutputWidget* m_build_output_widget = nullptr;
  DiagnosticsWidget* m_warnings_widget = nullptr;
  DiagnosticsWidget* m_errors_widget = nullptr;
  RenderProfilerWidget* m_render_profiler_widget = nullptr;
  FrameCaptureWidget* m_frame_capture_widget = nullptr;
  QMenu* m_project_menu = nullptr;
  QAction* m_close_project_action = nullptr;
  QAction* m_build_action = nullptr;
  QAction* m_rebuild_action = nullptr;
  QAction* m_clean_action = nullptr;
  QAction* m_run_action = nullptr;
  QAction* m_cancel_build_action = nullptr;
  QAction* m_project_settings_action = nullptr;
  bool m_run_after_build = false;
  // Every dock panel and the Font Awesome icon it shows in its title bar and tab.
  std::vector<std::pair<QDockWidget*, std::string_view>> m_dock_icons;
  bool m_dock_tab_icon_refresh_queued = false;
  // Set by Reset Editor Layout: re-apply the default dock proportions when the window resizes.
  bool m_keep_default_dock_sizes = false;
  bool m_default_dock_sizes_queued = false;
  bool m_editor_layout_active = false;
  ToolBar* m_tool_bar;
  MenuBar* m_menu_bar;
  SearchBar* m_search_bar;
  GameList* m_game_list;
  GameCount* m_game_count;
  RenderWidget* m_render_widget = nullptr;
  bool m_rendering_to_main;
  bool m_stop_confirm_showing = false;
  bool m_stop_requested = false;
  bool m_exit_requested = false;
  bool m_fullscreen_requested = false;
  bool m_is_screensaver_inhibited = false;
  u32 m_state_slot = 1;
  std::unique_ptr<BootParameters> m_pending_boot;

  SettingsWindow* m_settings_window = nullptr;
  // m_fifo_window doesn't set MainWindow as its parent so that the fifo can be focused without
  // raising the main window, so use a unique_ptr to make sure it gets destroyed.
  std::unique_ptr<FIFOPlayerWindow> m_fifo_window = nullptr;
  SkylanderPortalWindow* m_skylander_window = nullptr;
  InfinityBaseWindow* m_infinity_window = nullptr;
  WiiSpeakWindow* m_wii_speak_window = nullptr;
  LogitechMicWindow* m_logitech_mic_window = nullptr;
  MappingWindow* m_hotkey_window = nullptr;
  FreeLookWindow* m_freelook_window = nullptr;

  HotkeyScheduler* m_hotkey_scheduler;
  NetPlayDialog* m_netplay_dialog;
  DiscordHandler* m_netplay_discord;
  NetPlaySetupDialog* m_netplay_setup_dialog;
  static constexpr int num_gc_controllers = 4;
  std::array<GCTASInputWindow*, num_gc_controllers> m_gc_tas_input_windows{};
  std::array<GBATASInputWindow*, num_gc_controllers> m_gba_tas_input_windows{};
  static constexpr int num_wii_controllers = 4;
  std::array<WiiTASInputWindow*, num_wii_controllers> m_wii_tas_input_windows{};

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementsWindow* m_achievements_window = nullptr;
  Config::ConfigChangedCallbackID m_config_changed_callback_id;
  bool m_former_hardcore_setting = false;
#endif  // USE_RETRO_ACHIEVEMENTS

  AssemblerWidget* m_assembler_widget;
  BreakpointWidget* m_breakpoint_widget;
  CodeWidget* m_code_widget;
  JITWidget* m_jit_widget;
  LogWidget* m_log_widget;
  LogConfigWidget* m_log_config_widget;
  MemoryWidget* m_memory_widget;
  NetworkWidget* m_network_widget;
  RegisterWidget* m_register_widget;
  ThreadWidget* m_thread_widget;
  WatchWidget* m_watch_widget;
  CheatsManager* m_cheats_manager{};
  QByteArray m_render_widget_geometry;
};
