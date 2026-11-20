// oblivion - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <rex/rex_app.h>
#include "crash_handler.h"
#include "oblivion_fp_guard.h"
#include <rex/filesystem.h>

class OblivionApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<OblivionApp>(new OblivionApp(ctx, "oblivion",
        PPCImageConfig));
  }

  // If no game data root was given, walk up from the executable looking for
  // assets/default.xex so the exe works straight from the build tree.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    std::error_code ec;
    auto dir = rex::filesystem::GetExecutableFolder();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
      if (std::filesystem::exists(dir / "assets" / "default.xex", ec)) {
        paths.game_data_root = dir / "assets";
        return;
      }
      auto parent = dir.parent_path();
      if (parent == dir) break;
      dir = parent;
    }
  }

  // Developer aid: set OBLIVION_DUMP_IMAGE=<file> to dump the loaded guest
  // image (0x82000000..0x82B60000) so it can be disassembled offline.
  void OnPostSetup() override {
#ifndef _WIN32
    fp_guard_ = InstallGuestFpExceptionHandlerPosix();
#endif
  }

  void OnPostLoadXexImage() override {
    const char* out = std::getenv("OBLIVION_DUMP_IMAGE");
    if (!out || !runtime()) return;
    auto* p = runtime()->memory()->TranslateVirtual<const uint8_t*>(0x82000000);
    if (FILE* f = std::fopen(out, "wb")) {
      std::fwrite(p, 1, 0xB60000, f);
      std::fclose(f);
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    InstallOblivionCrashHandler();
#ifdef _WIN32
    fp_guard_ = InstallGuestFpExceptionHandlerWin();
#endif
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

 private:
  void* fp_guard_ = nullptr;
};
