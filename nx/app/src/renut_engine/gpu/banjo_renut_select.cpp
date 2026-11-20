// banjo-nx: the boot-time choice between banjo-nx's native renderer and Renut's (banjo_renut_select.h).
#include "renut_engine/gpu/banjo_renut_select.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

// oblivion nx (2026-11-03, renut-port bootstrap): banjo-nx's banjo_renderer ("native" default) became
// oblivion_renderer, default "emulated": banjo-nx's PM4 renderer is not part of this tree; reNut's hooks are registered
// only by a build with OBLIVION_RENUT_HOOKS (banjo_renut_select.h).
REXCVAR_DEFINE_STRING(oblivion_renderer, "emulated", "Oblivion",
                      "Renderer, chosen at boot. emulated = the SDK's emulated Xbox 360 GPU (gpu_plugin = \"xenos\"), "
                      "the default and the reference image. renut = reNut's Direct3D-level renderer "
                      "(github.com/masterspike52/reNut, through ChanseyIsTheBest's katamari-nx): the game's Direct3D "
                      "runs natively through plume. Needs a build with OBLIVION_RENUT_HOOKS (otherwise the emulated GPU "
                      "is used instead)")
    .allowed({"emulated", "renut"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::renut_gpu {

// banjo-nx (2026-10-19, CPU proposal 22): Active() is inline (banjo_renut_select.h), a byte load after the first call.
namespace detail {
int8_t g_active = 0;
bool LatchActive() {
  static const bool active = [] {
    const std::string& choice = REXCVAR_GET(oblivion_renderer);
#if BANJO_SWITCH_NO_VULKAN && OBLIVION_RENUT_HOOKS
    // katamari-nx (2026-10-20, banjo-nx): a Switch build without the Vulkan driver has only reNut's renderer on deko3d
    // (the emulated GPU needs Vulkan).
    if (choice != "renut")
      REXLOG_WARN("oblivion_renderer = \"{}\" needs Vulkan, which this build does not have: reNut's renderer on deko3d "
                  "instead", choice);
    return true;
#endif
#if REX_PLATFORM_WEB
    // reOblivion web (2026-11-17): the browser has WebGPU, no Vulkan: only reNut's renderer (plume's WebGPU backend).
    if (choice != "renut")
      REXLOG_INFO("oblivion_renderer = \"{}\": the browser has no Vulkan, reNut's renderer on WebGPU instead", choice);
    return true;
#endif
    if (choice != "renut") {
      REXLOG_INFO("oblivion_renderer = \"{}\": the SDK's emulated Xbox 360 GPU (gpu_plugin), reNut's renderer off",
                  choice);
      return false;
    }
#if !OBLIVION_RENUT_HOOKS
    REXLOG_WARN("oblivion_renderer = \"renut\", but this build registers none of reNut's hooks "
                "(OBLIVION_RENUT_HOOKS = 0): the emulated GPU instead");
    return false;
#else
    return true;
#endif
  }();
  g_active = active ? 1 : -1;
  return active;
}
}  // namespace detail

}  // namespace banjo::renut_gpu
