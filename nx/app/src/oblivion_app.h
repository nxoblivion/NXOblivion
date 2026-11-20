// oblivion (nx) - the app class: the repository's src/oblivion_app.h (paths, crash handler, floating-point guard,
// image dump) plus the reNut renderer glue of katamari-nx's app/src/renut_app.h (GPL-3.0), without katamari-nx's
// overlays, profilers, DLC installer, captures and Switch settings.
//
// The renderer is chosen at boot by oblivion_renderer (src/renut_engine/gpu/banjo_renut_select.cpp): "emulated" (the
// default) loads the SDK's emulated Xenos GPU plugin; "renut" runs reNut's renderer in the SDK's detached mode once
// its hooks are at Oblivion's functions (OBLIVION_RENUT_HOOKS; until then it falls back to "emulated").

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "crash_handler.h"
#if REX_PLATFORM_WEB
#include <spdlog/sinks/stdout_sinks.h>

#include "web/web_game_files.h"
#endif
#if defined(OBLIVION_BUILTIN_CONFIG)
#include "oblivion_builtin_config.h"
#endif
#include "oblivion_fp_guard.h"

#if BANJO_RENUT_RENDERER
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/perf/frame_stats.h"
#endif

// src/oblivion_switch_clocks.cpp: the console's clocks while the game runs (switch_clocks; nothing off the Switch).
namespace oblivion::clocks {
void Start();
void Restore();
}  // namespace oblivion::clocks

class OblivionApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<OblivionApp>(new OblivionApp(ctx, "oblivion", PPCImageConfig));
  }

#if defined(OBLIVION_BUILTIN_CONFIG)
  // Switch (katamari-nx's design): the fixed settings compiled in, applied before the player's oblivion.toml next to the
  // NRO, which is written there when missing.
  std::string_view BuiltInConfig() const override { return kOblivionBuiltInConfig; }
  std::string_view DefaultConfigFile() const override { return kOblivionPlayerConfig; }
#endif

  // If no game data root was given, walk up from the executable looking for assets/default.xex (nx/app/assets, a
  // junction to the extracted game: nx/tools/link_assets.ps1), so the exe works straight from the build tree. On the
  // Switch: game_root next to the NRO, else the emulated-GPU port's sdmc:/oblivion/assets (ports/switch/README.md).
#if REX_PLATFORM_WEB
  // The browser: the log also goes to stdout, which is the page's console and its ?log overlay (the log file is in
  // /save, out of the player's sight).
  void OnPostInitLogging() override {
    auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    sink->set_pattern("[%l] [%n] %v");
    rex::AddSink(sink);
  }
#endif

  void OnConfigurePaths(rex::PathConfig& paths) override {
#if defined(OBLIVION_BUILTIN_CONFIG)
    paths.config_path = rex::filesystem::GetExecutableFolder() / "oblivion.toml";
#endif
#if REX_PLATFORM_WEB
    // The browser (docs/web-port.md): the disc is read over HTTP (web/web_game_files.cpp); settings, saves and caches
    // live in /save (the browser's storage).
    {
      std::error_code ec;
      oblivion::web::MountSaveStorage("/save");
      std::filesystem::create_directories("/save/cache", ec);
      paths.user_data_root = "/save";
      paths.cache_root = "/save/cache";
      paths.config_path = "/save/oblivion.toml";
      if (paths.game_data_root.empty()) paths.game_data_root = oblivion::web::MountGameFiles();
      return;
    }
#endif
    if (!paths.game_data_root.empty()) return;
#if REX_PLATFORM_SWITCH
    {
      std::error_code ec;
      for (const std::filesystem::path& root : {rex::filesystem::GetExecutableFolder() / "game_root",
                                                std::filesystem::path("/oblivion/assets")}) {
        if (std::filesystem::exists(root / "default.xex", ec)) {
          paths.game_data_root = root;
          return;
        }
      }
    }
#endif
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

  void OnPreSetup(rex::RuntimeConfig& config) override {
    InstallOblivionCrashHandler();
    oblivion::clocks::Start();  // the settings are read by now; before the game's threads exist
#ifdef _WIN32
    fp_guard_ = InstallGuestFpExceptionHandlerWin();
#endif
#if BANJO_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) {
      // No SDK graphics system and no GPU plugin: the renderer is the app's (detached mode).
      renut::gpu::ConfigureRuntime(config);
      return;
    }
#endif
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  }

  void OnPostSetup() override {
#ifndef _WIN32
    fp_guard_ = InstallGuestFpExceptionHandlerPosix();
#endif
  }

  // Developer aid: OBLIVION_DUMP_IMAGE=<file> dumps the loaded guest image (0x82000000..0x82B60000).
  void OnPostLoadXexImage() override {
    const char* out = std::getenv("OBLIVION_DUMP_IMAGE");
    if (!out || !runtime()) return;
    auto* p = runtime()->memory()->TranslateVirtual<const uint8_t*>(0x82000000);
    if (FILE* f = std::fopen(out, "wb")) {
      std::fwrite(p, 1, 0xB60000, f);
      std::fclose(f);
    }
  }

#if BANJO_RENUT_RENDERER
  // reNut's renderer draws the SDK's ImGui through its own plume drawer, and resizes its swapchain itself.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return rex::ReXApp::OnCreateImmediateDrawer();
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
  // The window exists here: reNut's renderer comes up before the game starts.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active()) {
      renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
      // The debug overlay's FPS line (F3, L+R+Up; on the Switch open from the start: switch_fps_overlay).
      SetGuestFrameStats([] {
        const renut::frame_stats::Snapshot s = renut::frame_stats::Get();
        return rex::ui::FrameStats{s.frame_ms, s.fps, uint64_t(s.history_count)};
      });
    }
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
  void OnShutdown() override {
    oblivion::clocks::Restore();
    if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
  }
#else
  void OnShutdown() override { oblivion::clocks::Restore(); }
#endif

 private:
  void* fp_guard_ = nullptr;
};
