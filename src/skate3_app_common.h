#pragma once

#include "generated/skate3_init.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <rex/rex_app.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

#include "skate3_dev_console_dialog.h"
#include "skate3_nui_dialog.h"
#include "skate3_input_sampler.h"
#include "skate3_native_debug_dialog.h"
#include "skate3_map_editor_spawn_dialog.h"
#include "skate3_release_updater.h"
#include "skate3_vanilla_ui/skate3_vanilla_ui_dialog.h"

namespace rex::ui {
class ImGuiDrawer;
}

class Skate3BaseApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~Skate3BaseApp() override;

 protected:
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnPostSetup() override;
  void OnShutdown() override;

 private:
  void InstallRecipeOverlay();
  void InstallBigDeviceAliases();
  void InstallDlcPackages();
  void ToggleSimpleSettings();
  void ToggleVanillaUiPrototype();
  void ToggleNativeDebug();
  void ApplySettingsCursorMode();
  void ApplyGameplayCursorMode();
  void RestartGame();
  void SaveDrawFingerprintLog();
  void LogUserMarker();
  void LogDebugMarker();
  void ApplySelectedProfileToRuntime();

  static bool IsRecipeNameChar(char c);
  static std::set<std::string> DiscoverRecipeAliases(
      const std::filesystem::path& content_root);
  static bool CreateOverlayDirectory(const std::filesystem::path& overlay_root,
                                     std::string_view guest_path);

  std::filesystem::path config_path_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path profiles_path_;
  std::filesystem::path maps_path_;
  std::unique_ptr<rex::ui::SimpleSettingsDialog> simple_settings_dialog_;
  std::unique_ptr<skate3::vanilla_ui::PrototypeDialog>
      vanilla_ui_prototype_dialog_;
  std::unique_ptr<skate3::ReleaseUpdater> release_updater_;
  std::unique_ptr<skate3::NativeDebugDialog> native_debug_dialog_;
  std::unique_ptr<skate3::RenderModeIndicator> render_mode_indicator_;
  std::unique_ptr<skate3::MapEditorSpawnDialog>
      map_editor_spawn_dialog_;
  std::unique_ptr<skate3::Skate3DevConsoleDialog> dev_console_dialog_;
  // NUI has no keybind and no visibility of its own: it is on whenever a
  // resource has published a ui_page, and focused whenever a script says so
  // via SetNuiFocus. See skate3_nui_dialog.h.
  std::unique_ptr<skate3::Skate3NuiDialog> nui_dialog_;
  // Draws nothing; exists to sample input once a frame for scripts.
  std::unique_ptr<skate3::Skate3InputSamplerDialog> input_sampler_dialog_;
  bool recipe_overlay_installed_ = false;
  bool big_device_aliases_installed_ = false;
  std::atomic<uint32_t> debug_marker_count_{0};
};
