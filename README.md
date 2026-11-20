# NXOblivion

An unofficial Nintendo Switch port of **The Elder Scrolls IV: Oblivion**, made from the Xbox 360 version by static
recompilation. The game's PowerPC code is translated to native code with [ReXGlue](https://github.com/rexglue/rexglue-sdk),
and the game is drawn with reNut's Direct3D-level renderer on deko3d. It is not an emulated Xbox 360 GPU.

**No game data and no game code are included**, here or in the releases. The installer builds the port on your own PC
from your own copy of the Xbox 360 game.

## Supported game version

| Version | Status |
| --- | --- |
| **Xbox 360, original release** (2006, single DVD, title ID `425307D1`) | **Works** |
| Xbox 360, Game of the Year Edition | Coming soon |
| Other platforms (PC, PS3) or remasters | Not supported |

The installer accepts only the original release's `default.xex`. It checks its SHA-256, which must be
`6b215fed4662d4dbff9c98e99beb6d9a1d864d8f8382e3e71aeb3936d9ad53a6`. Any other version is refused before anything is
built. Title updates and DLC are not used.

## Prerequisites

To play:

- **Your own Xbox 360 copy** of the supported version, as a disc image (`.iso`, about 6.3 GB) or as the disc's files
  (`default.xex`, `Oblivion.ini`, `data`, `Data2`).
- **A Windows 10/11 PC (64-bit)** for the installer: about 3 GB of free disk for the build. Nothing else needs to be
  installed, because the compiler, the code generator, the shader tools and Python come in the zip.
- **A Nintendo Switch with custom firmware** (Atmosphère) and the Homebrew Menu, with about 7 GB free on the SD card.
- Optional: [DBI](https://github.com/rashevskyv/dbi) on the Switch, to install over USB (MTP) without taking the SD card
  out.

To build from source (developers): Windows 11, Git, CMake 3.25 or newer, Ninja, LLVM/Clang 22, Visual Studio (the
MSVC environment), Python 3, the .NET 10 SDK, and Docker Desktop with the `devkitpro/devkita64` image (devkitA64,
libnx, deko3d). `nx/tools/build_kit.sh` describes the steps.

## Install (Switch)

1. Download `NXOblivion-<version>-installer.zip` from [Releases](https://github.com/nxoblivion/NXOblivion/releases)
   and unpack it. Keep the folder together; nothing else needs to be installed.
2. Start `OblivionNX-Installer.exe` (Windows 10/11, 64-bit).
3. Pick your Oblivion Xbox 360 disc image (`.iso`) or its extracted files (`default.xex`, `Oblivion.ini`, `data`,
   `Data2`). The installer checks that it is the version the port was made for.
4. Build. The game code and the game's shaders are translated and compiled on your PC. The first build takes a few
   minutes and about 3 GB of disk.
5. Install to the SD card, to a folder, or to the Switch over USB (DBI's MTP responder).
6. On the Switch (Atmosphère), hold R while starting any game to open the Homebrew Menu, then start **NXOblivion**.
   Don't start it from the Album: it gives homebrew too little memory.

The installer's `README.txt` lists the settings (`sd:/switch/reoblivion/oblivion.toml`) and every file it writes.

## Status

- **Tested on a real Switch and very playable.** It runs at about **45 fps** outdoors and **25-35 fps** in cities.
- The default frame cap is 30 fps, like the Xbox 360 version. To go above it, set `oblivion_renut_frame_cap = 60.0` in
  `sd:/switch/reoblivion/oblivion.toml`. The resolution adapts to the load (540-720 lines).
- Also tested in the Eden emulator: title screen, new game, character creation, the Imperial Prison in 3D, autosaves.
- Alpha: expect bugs and the occasional crash.
- Bug reports are welcome. Please attach `sd:/switch/reoblivion/logs/`.

## Repository layout

| Path | What |
| --- | --- |
| `nx/` | The reNut Switch build (NXOblivion): app, renderer, the SDK fork (`nx/sdk`), tools, installer kit script |
| `nx/sdk` | The ReXGlue SDK fork used by the Switch build (Horizon layer, codegen options), BSD-3-Clause |
| `nx/tools/build_kit.sh` | Builds the installer folder (`OblivionNX-Installer.exe` + `kit/`) |
| `ports/switch/installer` | The installer's source (C#, .NET 10, WinForms) |
| `ports/switch` | The earlier Switch port on the emulated GPU (OpenGL backend) |
| `nx/web` | An experimental browser build (WebAssembly + WebGPU). No prebuilt game; build it from your own copy |
| `src`, `CMakeLists.txt` | The Windows build of the recompilation (see `docs/windows-port.md`) |
| `thirdparty/rexglue-sdk` | Submodule: [nxoblivion/rexglue-sdk](https://github.com/nxoblivion/rexglue-sdk), branch `nxoblivion` (ReXGlue v0.10.0 + the ports' changes) |
| `docs` | Design notes: `renut-port.md` (Switch renderer), `web-port.md`, `windows-port.md` |

Clone with `git clone --recurse-submodules https://github.com/nxoblivion/NXOblivion.git`. Game data goes in
`assets/` (Windows build) or `nx/app/assets` (Switch build). It is ignored by git, and never commit it.

## Credits

NXOblivion exists because of the following projects and people. Thank you!

**The renderer**
- **[reNut](https://github.com/masterspike52/reNut)** by **masterspike52**: the Banjo-Kazooie: Nuts & Bolts
  recompilation, whose Direct3D-level renderer draws NXOblivion. It reimplements the game's Direct3D, which runs on a
  shadow of the guest device, and its shaders are translated ahead of time.

**The Switch ports this one is built on** (GPL-3.0), by **ChanseyIsTheBest**
- [renut-nx](https://github.com/ChanseyIsTheBest/renut-nx): reNut on deko3d, on the Switch.
- [katamari-nx](https://github.com/ChanseyIsTheBest/katamari-nx): the app structure, the Switch build and the
  codegen tuning.
- [zuma-nx](https://github.com/ChanseyIsTheBest/zuma-nx) and
  [feedingfrenzy-nx](https://github.com/ChanseyIsTheBest/feedingfrenzy-nx): reNut's Direct3D layer adapted to older
  XDKs, like the one Oblivion was built with (2.0.2417).

**The recompiler**
- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** by **Tom Clay** and contributors: the static recompiler,
  code generator and runtime. NXOblivion's fork is [nxoblivion/rexglue-sdk](https://github.com/nxoblivion/rexglue-sdk)
  (the `thirdparty/rexglue-sdk` submodule), plus the reNut build's copy in `nx/sdk`.
- **[Xenia](https://github.com/xenia-project/xenia)**: the Xbox 360 research that ReXGlue's kernel and GPU code come from.
- **StevenSND**: ReXGlue's Horizon (Switch) layer.

**Tools and libraries**
- [devkitPro](https://devkitpro.org): devkitA64, [libnx](https://github.com/switchbrew/libnx),
  [deko3d](https://github.com/devkitPro/deko3d) and uam.
- [plume](https://github.com/renderbag/plume) (renderbag): the rendering interface reNut uses.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (hedge-dev): Xbox 360 shaders to HLSL.
- [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler),
  [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), LLVM/Clang and lld.
- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) and [DBI](https://github.com/rashevskyv/dbi) (duckbill),
  on the console.

Each component keeps its own license; the installer's `kit/licenses` folder holds them.

## License

GPL-3.0 (`LICENSE`). The SDK fork in `nx/sdk` keeps its BSD-3-Clause license (`nx/sdk/LICENSE`). Third-party
components keep their own licenses.

This is an unofficial fan project, not affiliated with or endorsed by Bethesda Softworks, ZeniMax, Microsoft or
Nintendo. The Elder Scrolls and Oblivion are trademarks of their owners. You need your own legally obtained copy of
the game.
