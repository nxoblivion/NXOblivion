# Debugging the Switch NRO on Atmosphère

Research notes plus a practical workflow. Nothing here has been run against a device yet; see "Verified vs uncertain" at the end. Code snippets are suggestions, not applied.

## Deploy from PC

Goal: build on the PC, get the NRO onto the Switch and run it, without moving the SD card. Script: `ports/switch/deploy.sh`.

### One-time setup on the Switch
1. Connect the Switch to the same LAN as the PC (Wi-Fi is fine). Note its IP (Settings > Internet > Connection Status).
2. Install an FTP server for persistent installs. Pick one:
   - **ftpd (mtheall)** NRO: drop `ftpd.nro` into `sdmc:/switch/`, launch it from hbmenu, it shows IP and port (default 5000, anonymous login accepted; configurable with `SITE PORT/USER/PASS/SAVE`). Simple, but it runs only while open, and using it from the title-takeover hbmenu is fine (it is small). Source: <https://github.com/mtheall/ftpd>
   - **sys-ftpd-light** sysmodule (always on, toggled from the Tesla overlay): config at `sdmc:/config/sys-ftpd/config.ini`. Since 1.0.4 anonymous login is off and user/password are empty by default, so set them. A user-posted config used port 5000; check yours. Sources: <https://github.com/cathery/sys-ftpd/releases>, <https://gbatemp.net/threads/possible-to-have-a-persistent-ftp-connection.620058/>
3. For live launch: in hbmenu, press **Y** to open the netloader ("NetLoader" screen, B to stop). The Switch must stay on that screen.
4. First FTP push creates `sdmc:/switch/oblivion/` automatically (script uses `--ftp-create-dirs`).

### PC side
```sh
# persistent install only (reachable FTP server needed)
SWITCH_IP=192.168.1.50 ports/switch/deploy.sh ftp out/oblivion.nro
# launch + stream the log (netloader open on the Switch)
SWITCH_IP=192.168.1.50 ports/switch/deploy.sh run  out/oblivion.nro
# both
SWITCH_IP=192.168.1.50 ports/switch/deploy.sh both
```
Logs land in `out/switch-logs/<timestamp>.txt`. Without `SWITCH_IP`, `run` relies on nxlink UDP broadcast discovery (see below). Env: `FTP_PORT`, `FTP_USER`, `FTP_PASS`, `REMOTE_NRO`, `NXLINK`, `NXLINK_ARGS`.

### nxlink facts (from the source)
- **Does it write to the SD card?** Yes. The hbmenu netloader decompresses the stream straight into a file under the hbmenu root path (path sanitised) and then launches that file; the NRO is never held whole in RAM (16 KB zlib buffers + 128 KB stdio buffer). An 88 MB NRO is fine (size field is int32, zlib-compressed on the wire; no size limit in code). Sources: [nxlink.c](https://github.com/switchbrew/switch-tools/blob/master/src/nxlink.c), [netloader.c](https://github.com/switchbrew/nx-hbmenu/blob/master/common/netloader.c). Exact destination folder: not confirmed; expect it next to other apps, check the SD after the first run.
- **Application mode / full memory?** Strong indication yes, not tested. nx-hbloader detects once whether its own process is the application (`svcGetInfo(IsApplication)`) and sizes the heap from that; on "next load" it reuses the same hbloader process and heap and just maps the next NRO into it. The netloader launches via that same next-load mechanism, so an NRO started from a title-takeover hbmenu inherits the application-mode memory budget. Source: [nx-hbloader main.c](https://github.com/switchbrew/nx-hbloader/blob/master/source/main.c). Verify by logging `svcGetInfo` total/used memory (or `appletGetAppletType()`) at startup and comparing with a menu-launched run.
- **Protocol/ports:** discovery = UDP broadcast `nxboot` to port 28280, reply `bootnx` on UDP 28771; upload = TCP 28280; `-s` stdio = the Switch connects back to the PC on **TCP 28771**. Windows firewall must allow inbound TCP 28771 and UDP 28771 for nxlink.exe. `-a <ip>` skips discovery.
- **Arguments:** trailing args / `--args "..."` are NUL-packed into a 3072-byte buffer (argv[0] is the NRO path set by hbmenu).
- **Finding the Switch IP:** `nxlink` with no `-a` broadcasts and prints the IP it finds (retries `-r`, default 10). Broadcasts do not cross Docker Desktop's NAT or some Wi-Fi AP client isolation; use `-a`.

### Which nxlink binary
- Docker image `devkitpro/devkita64` contains `/opt/devkitpro/tools/bin/nxlink` (checked). `aarch64-none-elf-gdb` and `addr2line` are also in the image.
- Docker Desktop NAT: the TCP upload with `-a` should work, UDP discovery will not, and the `-s` callback from the Switch to the PC needs `-p 28771:28771` and the Switch's connection to land on the container (unverified). Recommended: install the devkitPro Windows installer (<https://github.com/devkitPro/installer/releases>) to get native `C:\devkitPro\tools\bin\nxlink.exe`; the script looks there. This PC currently has no `C:\devkitPro`.
- The script prints clear errors for: no NRO, no `SWITCH_IP`, unreachable FTP (exit 3), "No response from Switch" (exit 4).

### Speed for large assets (~6 GB)
Not measured. FTP over Wi-Fi to the Switch typically manages only a few MB/s (order of tens of minutes to hours for 6 GB; Switch Wi-Fi is 802.11ac but SD write and the ftpd CPU limit matter). A USB-C LAN adapter helps (Ethernet works through the dock or a USB adapter). MTP via DBI or Atmosphère's Haze (USB, built in since 1.3; 64-bit transfers since 1.6) is usually faster than Wi-Fi FTP. For the initial 6 GB use the existing `copy-assets.ps1` with the SD card in the PC; use FTP/nxlink only for NRO iteration. Haze: <https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/changelog.md>.

---

## 1. GDB (Atmosphère built-in stub)

Added in **Atmosphère 1.2.3** as experimental standalone; later fixes: 1.4.0 (correct module name for homebrew), 1.6.0 (`monitor wait <program id>`), 1.7.0 (QStartNoAckMode, breakpoint-count crash fix). Source: [changelog](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/changelog.md).

### Enable
`sdmc:/atmosphere/config/system_settings.ini` (create from `config_templates`):
```ini
[atmosphere]
enable_htc = u8!0x0
enable_standalone_gdbstub = u8!0x1
```
Reboot (or restart through Hekate/fusee). The `enable_standalone_gdbstub` key is documented in the 1.2.3 changelog but is absent from the current `config_templates/system_settings.ini`, so check it still applies on your Atmosphère version (current is 1.12.0 per the changelog; not confirmed on hardware). The stub uses sockets itself: debugging socket-related system processes can hang it.

### Connect (Git Bash, gdb inside the container)
gdb in the container only needs outbound TCP to the Switch, which Docker Desktop NAT allows (unverified but standard).
```sh
MSYS_NO_PATHCONV=1 docker run --rm -it -v "$(pwd -W):/work" -w /work devkitpro/devkita64:latest \
  aarch64-none-elf-gdb out/oblivion.elf
```
(`aarch64-none-elf-gdb` is on PATH in the image; confirmed to exist in `devkitA64/bin`.) Inside gdb:
```
set pagination off
target extended-remote 192.168.1.50:22225
info os processes                # list PIDs; homebrew shows as the application/hbloader process
attach <pid>
monitor get info                 # address space layout + module list with base addresses
monitor get mappings
```
Alternative: install the devkitPro Windows package and run `C:\devkitPro\devkitA64\bin\aarch64-none-elf-gdb.exe` natively; no Docker layer.

### Breaking on launch
```
monitor wait application         # next application is started suspended
# ... launch the title (takeover game) on the Switch, then:
attach <pid>                     # attaches at first instruction
```
`monitor wait <hex program id>` (1.6.0+) waits for a specific program. For an NRO launched from the takeover hbmenu the process is already running by the time you launch the NRO (it is a next-load inside the same process), so the practical flow is: launch hbmenu (takeover), `attach` to that PID, `continue`, then netloader/run the NRO; stop with Ctrl-C. Attach-on-first-instruction failures may destabilise the system per the changelog.

### Symbols at the NRO load address
From `monitor get info`, take the base of the NRO module (module name should now be the NRO path, per 1.4.0). The NRO's `.text` starts at base (header at +0, text follows at +0x80 for a standard NRO; the ELF `.text` vma is 0 for libnx NROs linked with `switch.ld`). So:
```
add-symbol-file out/oblivion.elf -o 0x<base>
```
or, if `.text` has a non-zero vma, `add-symbol-file out/oblivion.elf <base+texttoff>` (check `aarch64-none-elf-objdump -h out/oblivion.elf`; both approaches are the usual practice but untested here). An 88 MB ELF will take a while to load; `set debuginfo-flags`/`gdb-add-index` can help. NRO ASLR: base differs each launch, redo `add-symbol-file` each run.

### Usage
```
info threads / thread N / thread apply all bt
break some_function            # software breakpoints (hardware too: hbreak)
hbreak *0x...
watch -l *(int*)0x...          # hardware watchpoints (limited count, ~4)
stepi / nexti / continue
set scheduler-locking on       # keep the other 2 cores from running while stepping
```
### Caveats
- All 3 app cores are halted while stopped; network and audio threads will time out. Expect Wi-Fi gdb to be slow with an 88 MB ELF (symbol reads are local, memory reads go over the wire).
- Guest memory mapped by `svcMapMemory` into the stack region appears as separate mappings; use `monitor get mappings` to confirm addresses before `x/` or `watch`.
- Stopping on crash: the stub reports exceptions as signals when attached; attach before reproducing. If the process dies un-attached, use the crash report (next section).
- Hekate/emuMMC are not required for the stub.

## 2. Crash reports

### Atmosphère-generated
On a crash of a process without handler, Atmosphère `fatal` writes `sdmc:/atmosphere/crash_reports/<time>_<titleid>.log` (plus `dumps/` with binary dumps, and `fatal_errors/` / erpt reports in `atmosphere/erpt_reports` on newer versions). The text log contains (from memory of the format; not re-verified here): process name/title ID, error description, register dump (x0..x30, SP, PC, PSTATE), the **stack trace with addresses and module-relative offsets** and the module base address. For homebrew the module is the NRO.

Symbolize:
```sh
# offset = PC - NRO base (the log gives the module-relative form directly)
docker run --rm -v "$(pwd -W):/work" -w /work devkitpro/devkita64 \
  aarch64-none-elf-addr2line -f -C -e out/oblivion.elf 0x<offset> 0x<offset2>
```
Batch helper (suggested, not added):
```sh
grep -oE '0x[0-9a-fA-F]+' crash.log | sort -u | xargs aarch64-none-elf-addr2line -f -C -p -e out/oblivion.elf
```
Existing tools: [switchbrew crash-report parsers](https://github.com/switchbrew) exist but none verified; the sed/grep above is enough. If the offset is relative to the NRO base and the ELF `.text` vma is not 0, add that vma.

### In-app handler (suggested snippet for main.cpp)
```cpp
#include <switch.h>
#include <stdio.h>
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    FILE* f = fopen("sdmc:/oblivion/crash.txt", "a");
    if (f) {
        fprintf(f, "EXC desc=%u pc=%lx lr=%lx sp=%lx fp=%lx far=%lx esr=%x\n",
                ctx->error_desc, ctx->pc.x, ctx->lr.x, ctx->sp.x, ctx->fp.x, ctx->far.x, ctx->esr);
        for (int i = 0; i < 29; i++) fprintf(f, "x%d=%lx\n", i, ctx->cpu_gprs[i].x);
        // frame-pointer walk (needs -fno-omit-frame-pointer to be reliable)
        u64* fp = (u64*)ctx->fp.x;
        for (int d = 0; d < 32 && fp && ((u64)fp & 7) == 0; d++) {
            fprintf(f, "bt%d=%lx\n", d, fp[1]); fp = (u64*)fp[0];
        }
        fclose(f);
    }
    // handler return: libnx then terminates via fatal; Atmosphère will still write its report.
}
```
Also print `pc` minus the NRO base: `envGetNextLoadPath` is not the base; use `svcQueryMemory(&mi,&pi,(u64)&main)` and `mi.addr` for the module's code base. Field names follow libnx `ThreadExceptionDump` (`pc`, `lr`, `sp`, `fp`, `far`, `esr`, `cpu_gprs[29]`); check against your libnx header. Reference: <https://switchbrew.github.io/libnx/>. SD writes inside an exception handler may fail if the crash was a bad fs state; also write to a socket if nxlink is up.

## 3. Network logging (suggested snippet)
```cpp
#include <switch.h>
#include <stdio.h>
static int g_nxlink = -1;
void log_net_init() {
    if (R_SUCCEEDED(socketInitializeDefault())) g_nxlink = nxlinkStdio();  // stdout/stderr -> PC
}
void log_net_exit() { if (g_nxlink >= 0) close(g_nxlink); socketExit(); }
```
`nxlinkStdio()` only connects when launched by nxlink `-s` (hbloader passes the PC address in the environment); otherwise it returns -1 and you keep logging to `sdmc:/oblivion/log.txt`. Use `setvbuf(stdout,NULL,_IONBF,0)` for immediate lines. Socket service memory: the default socket config uses a few MB of heap; check that it fits alongside the guest memory budget. Add the `-s` flag via `deploy.sh run`.

## 4. Profiling and GPU
Nothing turnkey was found. Options: `armGetSystemTick()` (19.2 MHz counter) for manual scopes; `svcGetInfo(InfoType_TotalMemorySize/UsedMemorySize)` for memory; a poor-man sampling profiler can be built by a thread that periodically `svcGetThreadContext3`s the other threads and logs PCs (symbolize with addr2line); or attach gdb and interrupt repeatedly. Mesa debug env vars (`MESA_DEBUG`, `NV50_PROG_DEBUG`, `GALLIUM_HUD`) exist in desktop Mesa; whether the Switch (nouveau on nvservices) port honours them is untested; set them with `setenv()` before `eglInitialize`. Output goes to stderr, so they work well with `nxlinkStdio`.
Atmosphère `dmnt:cht` (cheat engine service, used by EdiZon/Breeze) offers memory read/write and mappings of the *application* process ([dmnt docs](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/dmnt.md)); usable for memory inspection but gdb is more capable.

## 5. Other tools
- **dmnt.gen2** is the module that hosts the gdbstub and cheat services in current Atmosphère.
- **Twili/twib**: old third-party debug monitor; I could not confirm active maintenance, treat as unmaintained. The Atmosphère gdbstub supersedes it.
- **Hekate**: bootloader only (boot Atmosphère, create/select emuMMC); not involved in debugging.
- **Risk (brief):** homebrew on CFW can trigger bans if the console goes online with the sysNAND. Use **emuMMC** for homebrew, keep sysNAND offline or incognito, and use 90DNS-style blocking on any online profile. Title takeover launches under a real game's identity: do not go online in it.

## 6. v0.4 boot diagnostics (boot.txt, crash.txt, USB log)
The v0.4 build writes breadcrumbs to `sdmc:/oblivion/boot.txt` (one open/write/close per line, survives a hang), writes `sdmc:/oblivion/crash.txt` on a host exception (registers, backtrace, code addresses found on the stack; the code-block base is printed so `rel` values can go straight to `aarch64-none-elf-addr2line -e oblivion_gl.elf`), shows the early boot steps on screen ("Oblivion: starting... step N"), and streams every breadcrumb and spdlog line over USB.

### USB log with Zadig (Windows)
1. `pip install pyusb libusb-package` (`python ports/switch/usb_log.py --deps` prints it).
2. Run Oblivion on the Switch (title takeover) and connect the USB-C cable to the PC. The device appears once the app has started (about a second after launch).
3. Open Zadig, menu Options -> List All Devices. Pick the device whose USB ID is `057E 3000` (name is usually "Nintendo Switch" or "Unknown Device"). Select the driver `WinUSB` (or `libusbK`) in the right-hand box and click Install Driver / Replace Driver.
4. Run `python ports/switch/usb_log.py`. It prints lines live and saves them to `out/switch-logs/usb-<timestamp>.txt`; it reconnects by itself if you restart the app. Zadig only needs to be run once per PC/port.
5. DBI's MTP mode uses a different USB ID (probably 057E with another PID, e.g. its own MTP descriptor; not verified from DBI docs), so replacing the driver for `057E 3000` should not affect it. Check the ID Zadig shows before installing, and install only for `057E 3000`.
The game never waits on USB: lines go into a 256 KiB ring and a dedicated thread writes them; the ring drops lines when full or when no reader is attached.

## Recommended workflow
1. `system_settings.ini` as above; install ftpd; native `nxlink.exe` on the PC.
2. `deploy.sh both` for each build; read `out/switch-logs/*.txt` live.
3. Crash: copy the Atmosphère crash report, `addr2line` the offsets; add the in-app handler for registers/backtrace.
4. Hard bugs: gdb attach (hbmenu PID), `monitor get info`, `add-symbol-file`.

## Verified vs uncertain
Verified from sources: GDB stub existence, setting names, port 22225, `info os processes`, monitor commands, version history ([changelog](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/changelog.md)); nxlink ports/protocol/args/compression ([nxlink.c](https://github.com/switchbrew/switch-tools/blob/master/src/nxlink.c)); netloader writes to SD and streams ([netloader.c](https://github.com/switchbrew/nx-hbmenu/blob/master/common/netloader.c)); hbloader reuses process/heap on next-load ([main.c](https://github.com/switchbrew/nx-hbloader/blob/master/source/main.c)); the Docker image has nxlink, gdb, addr2line; ftpd supported commands ([README](https://github.com/mtheall/ftpd)).
Uncertain or untested: application-mode memory for netloader-launched NROs (inferred); exact NRO destination folder; `enable_standalone_gdbstub` on current Atmosphère; add-symbol-file offset arithmetic; crash report exact layout; Docker Desktop networking for `-s` and gdb; FTP/MTP speeds; Mesa env vars on the Switch port; sys-ftpd-light defaults; Twili status.
