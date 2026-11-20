# reOblivion in the browser

Branch `web-port`. Started 2026-11-17. A personal build: it plays the player's own disc in a desktop browser (Chrome,
Edge, Brave, Firefox, Safari; a fridge's browser too). It is **never published**: no hosted build, no hosted game
files. The disc's files and the shader package made from them stay on the player's machine and in the build tree.

## How it fits together

- **WebAssembly, 64-bit memory** (Emscripten `-sMEMORY64`, `nx/web/env.sh` puts the repo's emsdk in `out/emsdk` on
  PATH). The game runs on workers (`PROXY_TO_PTHREAD`); the page's thread only hosts WebGPU and the input.
  Needs cross-origin isolation (COOP/COEP) for SharedArrayBuffer: `nx/web/serve.js` sets the headers.
- **Guest memory**: one 4.5 GB block from `sbrk` (`nx/sdk/src/core/memory_web.cpp`; emmalloc under mimalloc refuses
  4 GB or more). WebAssembly cannot alias memory, so the 360's physical mirrors (0xC0000000, 0xE0000000) are folded
  onto the 0xA0000000 window: the generated code adds `REX_PHYS_HOST_OFFSET`, the SDK `detail::GuestHostDelta`, and
  reNut's `GuestPtr` the same (without it the renderer read zeros for vertex buffers in the mirrors).
- **Suspending a thread**: a Web Worker cannot be stopped from outside, so a guest thread's suspension of another one
  is owed (as on the Switch): the thread takes it at its next safe point, an IRQL lowered below DISPATCH or a wait or a
  delay (`XThread::TakeOwedSuspensionAtSafePoint`), and a resume cancels one not taken yet.
- **The disc** is read over HTTP in pieces (`nx/app/src/web/web_game_files.cpp`): a read-only WASMFS tree at `/game`
  from the server's manifest; every read is a synchronous Range request on the worker that reads, through a cache.
- **The player's files** (`/save`: `oblivion.toml`, saves, caches, the log) are the browser's origin private file
  system (OPFS) where the browser has synchronous file handles (Chrome 102, Firefox 111, Safari 17), else memory (gone
  with the page). The page asks for persistent storage on the first click.
- **Rendering**: reNut's renderer on plume's WebGPU back end (`nx/web/plume/plume_webgpu.cpp` on the workers records
  command words; `nx/web/plume/webgpu_server.js` on the page's thread makes the WebGPU calls). Shaders are WGSL from
  `nx/tools/webgpu_shaders.py` (XenosRecomp -> SPIR-V -> GLSL -> glslang + Tint in the `reoblivion-tint` Docker
  image, `nx/web/tint/Dockerfile`), packaged as `app/out/web/reoblivion_wgsl.bin` beside the page.
  - WebGPU is stricter than Vulkan/D3D12: a pipeline's attachments must match its pass exactly and an indexed strip
    needs the index format in the pipeline. The server makes a variant of a pipeline for the pass and index format it
    is drawn with (`pipelineFor`), cached on the pipeline.
  - WebGPU wants a viewport's depth range the right way round (minDepth <= maxDepth); Oblivion's is reversed (MinZ 1,
    MaxZ 0). The server sets 0 to 1 and every vertex shader maps its depth itself (`injectDepthMap`: two overrides,
    the identity by default, set in the pipeline's variant for a reversed range). Before (2026-11-19) the range was
    made 1 to 1: every depth 1, the last draw over a pixel won, and the walls drawn after the actors covered them (the
    player's face and body in the race menu, the people, the chains and the furniture in the cell).
  - 16-bit unorm textures cannot be filtered on WebGPU: the web build keeps the game's own post-process precision
    (`renut_post_process_precision = "Game"`) and 2_10_10_10 targets are float16.
  - The frame is letterboxed in a page of another shape (no trim: a 4:3 page trimmed the character menu); a page wider
    than 16:9 at startup gets reNut's Hor+ widescreen (the canvas's size, not SDL's window, which stays 2560x1440).
- **Input**: keyboard and mouse, laid out like Oblivion on the PC (`nx/app/oblivion_web_defaults.toml`). The page reads
  them (`nx/web/shell.html`) and hands them to the SDK's keyboard-and-mouse driver (`rex_web_key`, `rex_web_mouse`):
  SDL3's keyboard events never reached the app's event loop on Emscripten. A click locks the pointer for mouse look;
  Escape gives it back (the browser's rule) and opens the pause menu. F5, F9 (the log), F11 and F12 stay the browser's.
  The layout is PC Oblivion's own default (its `Oblivion_default.ini` `[Controls]`), played on the 360 game's pad:
  WASD / arrows move, Space activates (Z holds to grab), E jumps, F readies the weapon, C casts, the left button
  attacks, the right button or Alt blocks, Control sneaks, R or the middle button changes the view, Tab opens the menu,
  T waits, Escape pauses, 1-8 are the hotkeys (the 360's hotkey wheel: 1 up, then clockwise, the diagonals two D-pad
  directions at once). Shift walks (or runs with Always Run off), CapsLock toggles Always Run, Q toggles Auto Move.
  The 360 has no quick save or quick load (F5 and F9 stay the browser's anyway).
  The mouse turns the player as on the PC (2026-11-19): the 360's look update (`sub_82417860`) still turns by mouse
  counts times `fMouseSensitivity`; the stick is only made into counts first. A mid-function hook in the manifest
  (`[[entrypoint.midasm_hook]]` at 0x82418024, `nx/app/src/oblivion_mouse_look.cpp`) adds the mouse's own counts
  there (`rex::input::mnk::TakeMouseLook`, `mnk_mouse_direct`): no stick dead zone, acceleration or top speed, and
  `bInvertYValues` still applies. While the game takes them it is in play: binds prefixed `Game:` apply; otherwise (the
  menus, which want the pad) those prefixed `Menu:` do, and the mouse is the right stick there as before (past the
  dead zone, `mnk_mouse_deadzone = 8700`). In the menus Enter / Space / the left button choose, Escape / Tab /
  Backspace / the right button go back, Q and E change tabs, the wheel scrolls.
  The game's text prompts (the character's name) are the browser's own `prompt()` dialog.
- **Touch** (a phone, a tablet; 2026-11-19): on-screen controls laid out like the 360 pad, drawn over the picture at the
  first touch (at once on a phone) and hidden by a mouse click. The left part of the screen is a stick where the thumb
  lands (analog: `rex_web_stick`, added to the pad's left stick), a drag on the right part looks around (fed as mouse
  movement, twice as far), and the buttons press the same keys as the keyboard's: Use, Menu, Weapon, Jump, Attack,
  Block, Cast, Auto (Auto Move), Pause, Wait, Sneak, View and the quick keys 1, 3, 5 and 7. The first touch asks for full screen and landscape
  where the browser allows it (Android; not the iPhone's Safari).

## Building

```sh
sh nx/tools/build_web.sh          # app/out/web: oblivion.html/.js/.wasm (needs app/generated from the PC build)
python nx/tools/webgpu_shaders.py # app/out/web/reoblivion_wgsl.bin (Docker: docker build -t reoblivion-tint nx/web/tint)
```

## Playing

```sh
cd nx/web && node serve.js --root ../app/out/web --port 8650 --mount "/game=<the extracted disc>"
```

Open `http://localhost:8650/oblivion.html` (localhost is a secure context). On another device on the LAN (the fridge)
use the HTTPS port (8651, the probe's self-signed certificate): WebGPU and threads need a secure context. `?log` shows
the log over the picture; F9 toggles it.

On Git Bash, run the server with `MSYS_NO_PATHCONV=1` so the `/game=` mount is not rewritten as a Windows path.

## Debugging from the page's console

- `WebGpuServer.trace(n, cap, filter)`: the next n submits' draws (target, pipeline, viewport, scissor, bound groups,
  buffers; at most `cap`, 200 by default), with the target changes, clears, copies and resolves between them. With a
  `filter` (text of the pipeline's label, e.g. `'shader 544 +'`) only those draws, and each one's 256 vertex constants
  (`traceConstants()`), shared constants and first 4 KB of vertex data as the GPU had them (`traceVertices()`).
- `WebGpuServer.debug('solid')`: blended draws in magenta without depth tests (where geometry lands); `'opaque'`:
  their own colours without blending; `'mark:<label>'`: the draws of the pipelines whose label has the text in
  magenta (depth test off, depth written, both sides); `''` off. A pipeline's label names its shaders as linked to its
  pass (`shader 544 + shader 545`), not as the game made them.
- `WebGpuServer.inspect(id)` (a server object: a pipeline's description and WGSL), `readBuffer(id, offset, size)` (a
  GPU buffer's bytes now), `linkStages(vs, ps)`.
- `reoblivionLog`: the log lines so far.

## Status (2026-11-19)

Checked in Chromium (the desktop app's browser pane, Windows, NVIDIA):

- Boot, logos, Bink videos, the menus; a new game through the name prompt and the character creation to play in the
  prison cell, rendered in 3D at 60 fps (game thread); the faces and bodies (the race menu, the intro, third person)
  since the depth range fix. Keyboard: menus, walking. Mouse look (motion fed to
  `rex_web_mouse`; the real pointer lock is a click away in a normal tab).
- Saves: the autosave and manual saves are written to OPFS, survive a reload, and load ("Continue" on the main menu).
  **Known issue**: a manual save shows "Save failed." although the save is written, listed and loads; no kernel call
  fails around it (NtWriteFile / NtSetInformationFile / NtQueryInformationFile now warn when they do). Unexplained:
  maybe the save's screenshot (the save slots show no picture).

Not checked here: Chrome/Edge/Brave on their own (same engine as the pane), Firefox, Safari, the fridge. The page says
what a browser lacks (WebGPU, 64-bit WebAssembly memory, cross-origin isolation) instead of failing silently; Safari's
64-bit WebAssembly memory support is the one most in doubt.
