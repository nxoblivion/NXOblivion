// Memory probe for the Oblivion Switch port. Tests what Horizon lets a homebrew NRO do with
// address space: big VA reservations, true RW aliases of one backing block, permission
// changes on aliased pages and resumable write faults. Results go to the console, to
// svcOutputDebugString, and to sdmc:/memprobe.txt.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>

#define MiB (1024ull * 1024ull)
#define GiB (1024ull * MiB)

// A heap-less launch (not via hbloader) reports a 0 own-process handle; use the pseudo handle.
static Handle own_handle(void) {
    Handle h = envGetOwnProcessHandle();
    return h ? h : CUR_PROCESS_HANDLE;
}

// The default libnx heap takes ALL free memory, leaving none for shared memory: bound it.
extern char* fake_heap_start;
extern char* fake_heap_end;
void __libnx_initheap(void) {
    if (envHasHeapOverride()) {
        fake_heap_start = (char*)envGetHeapOverrideAddr();
        fake_heap_end = fake_heap_start + envGetHeapOverrideSize();
        return;
    }
    void* addr = NULL;
    u64 size = 320ull * 1024 * 1024;
    svcSetHeapSize(&addr, size);
    fake_heap_start = (char*)addr;
    fake_heap_end = (char*)addr + size;
}

static char g_log[64 * 1024];
static size_t g_len;

static void L(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    line[n] = 0;
    fputs(line, stdout);
    consoleUpdate(NULL);
    svcOutputDebugString(line, n);
    if (g_len + n < sizeof g_log) { memcpy(g_log + g_len, line, n); g_len += n; }
}

static void flush_file(void) {
    FILE* f = fopen("sdmc:/memprobe.txt", "wb");
    if (!f) { printf("cannot open sdmc:/memprobe.txt\n"); return; }
    fwrite(g_log, 1, g_len, f);
    fclose(f);
}

static u64 info(u32 id, Handle h, u64 id1) {
    u64 v = 0;
    Result rc = svcGetInfo(&v, id, h, id1);
    if (R_FAILED(rc)) { L("  svcGetInfo(%u) failed 0x%x", id, rc); return 0; }
    return v;
}

static const char* memtype(u32 t) {
    switch (t & 0xFF) {
        case 0: return "Unmapped"; case 1: return "Io"; case 2: return "Normal"; case 3: return "CodeStatic";
        case 4: return "CodeMutable"; case 5: return "Heap"; case 6: return "SharedMem"; case 7: return "Weird";
        case 8: return "ModuleCodeStatic"; case 9: return "ModuleCodeMutable"; case 10: return "IpcBuf0";
        case 11: return "Mapped"; case 12: return "ThreadLocal"; case 13: return "TransferMemIsolated";
        case 14: return "TransferMem"; case 15: return "ProcessMem"; case 16: return "Reserved";
        case 17: return "IpcBuf1"; case 18: return "IpcBuf3"; case 19: return "KernelStack";
        case 20: return "CodeReadOnly"; case 21: return "CodeWritable"; default: return "?";
    }
}

static void query(const char* what, void* addr) {
    MemoryInfo mi; u32 pi;
    Result rc = svcQueryMemory(&mi, &pi, (u64)addr);
    if (R_FAILED(rc)) { L("  query %s @%p failed 0x%x", what, addr, rc); return; }
    L("  query %s @%p: base=%p size=0x%llx type=%s perm=%u attr=%u", what, addr, (void*)mi.addr,
      (unsigned long long)mi.size, memtype(mi.type), mi.perm, mi.attr);
}

// ---------------- fault handling ----------------
static volatile u64 g_fault_far, g_fault_pc;
static volatile u32 g_fault_esr, g_fault_desc, g_fault_count;
static volatile u64 g_fix_addr, g_fix_size;
static volatile int g_fix_mode;  // 1 = svcSetMemoryPermission RW, 2 = svcSetProcessMemoryPermission RW
static volatile u32 g_fix_rc;

// libnx calls this on the exception stack. Returning with svcReturnFromException(0) resumes
// the faulting instruction; we first restore write access so it does not fault again.
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    g_fault_far = ctx->far.x;
    g_fault_pc = ctx->pc.x;
    g_fault_esr = ctx->esr;
    g_fault_desc = ctx->error_desc;
    g_fault_count++;
    if (g_fault_count > 4) svcReturnFromException(0xf801);  // give up: terminate
    if (g_fix_mode == 1) g_fix_rc = svcSetMemoryPermission((void*)g_fix_addr, g_fix_size, Perm_Rw);
    else if (g_fix_mode == 2) g_fix_rc = svcSetProcessMemoryPermission(own_handle(), g_fix_addr, g_fix_size, Perm_Rw);
    else g_fix_rc = 0xdead;
    svcReturnFromException(0);
}

static void fault_test(const char* name, volatile u32* page, int fixmode, u32 initial_perm) {
    Result rc;
    if (fixmode == 1) rc = svcSetMemoryPermission((void*)page, 0x1000, initial_perm);
    else rc = svcSetProcessMemoryPermission(own_handle(), (u64)page, 0x1000, initial_perm);
    L("  [%s] set perm %u via mode %d: rc=0x%x", name, initial_perm, fixmode, rc);
    if (R_FAILED(rc)) return;
    query(name, (void*)page);
    g_fix_addr = (u64)page; g_fix_size = 0x1000; g_fix_mode = fixmode; g_fault_count = 0; g_fault_far = 0;
    L("  [%s] writing to protected page (expect a fault)...", name);
    page[0] = 0x12345678;
    L("  [%s] survived: faults=%u far=%p pc=%p esr=0x%x desc=0x%x fixrc=0x%x value=0x%x", name, g_fault_count,
      (void*)g_fault_far, (void*)g_fault_pc, g_fault_esr, g_fault_desc, g_fix_rc, page[0]);
    g_fix_mode = 0;
}

static int alias_check(const char* name, volatile u32* a, volatile u32* b) {
    a[0] = 0xA5A50001u; a[1023] = 0x5A5A0002u;
    int ok = (b[0] == 0xA5A50001u && b[1023] == 0x5A5A0002u);
    b[10] = 0xC0DE0003u;
    ok = ok && (a[10] == 0xC0DE0003u);
    L("  alias %s: write-a/read-b and write-b/read-a => %s", name, ok ? "TRUE ALIAS" : "NOT aliased");
    return ok;
}

int main(int argc, char** argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    fsdevMountSdmc();

    L("== Oblivion Switch memprobe ==");
    // ---- (d) system / process memory ----
    L("[d] process memory");
    AppletType at = appletGetAppletType();
    L("  appletType=%d (0=Application 1=SystemApplet 2=LibraryApplet 3=OverlayApplet 4=SystemApplication 5=Default/hbl-applet)", (int)at);
    L("  envIsSyscallHinted: 0x73(SetProcessMemoryPermission)=%d 0x74(MapProcessMemory)=%d 0x75=%d",
      envIsSyscallHinted(0x73), envIsSyscallHinted(0x74), envIsSyscallHinted(0x75));
    L("  own process handle=0x%x", own_handle());
    u64 total = info(InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    u64 used = info(InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    L("  TotalMemorySize=%llu MiB UsedMemorySize=%llu MiB free=%llu MiB", total / MiB, used / MiB, (total - used) / MiB);
    L("  TotalNonSystem=%llu MiB UsedNonSystem=%llu MiB SystemResource total=%llu MiB used=%llu MiB",
      info(InfoType_TotalNonSystemMemorySize, CUR_PROCESS_HANDLE, 0) / MiB,
      info(InfoType_UsedNonSystemMemorySize, CUR_PROCESS_HANDLE, 0) / MiB,
      info(InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0) / MiB,
      info(InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0) / MiB);
    u64 aslr_a = info(InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0), aslr_s = info(InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
    u64 alias_a = info(InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0), alias_s = info(InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
    u64 heap_a = info(InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0), heap_s = info(InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    u64 stack_a = info(InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0), stack_s = info(InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);
    L("  ASLR  region %llx + %llx (%llu GiB)", aslr_a, aslr_s, aslr_s / GiB);
    L("  Alias region %llx + %llx (%llu GiB)", alias_a, alias_s, alias_s / GiB);
    L("  Heap  region %llx + %llx (%llu GiB)", heap_a, heap_s, heap_s / GiB);
    L("  Stack region %llx + %llx (%llu GiB)", stack_a, stack_s, stack_s / GiB);
    u64 sys_total = 0, sys_used = 0;
    svcGetInfo(&sys_total, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, 0);
    svcGetInfo(&sys_used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, 0);
    L("  system DRAM total=%llu MiB used=%llu MiB", sys_total / MiB, sys_used / MiB);

    // ---- (a) reserve VA ----
    L("[a] VA reservation");
    virtmemLock();
    void* found[3]; u64 sizes[3] = {1 * GiB, 4608 * MiB, 16 * GiB};
    for (int i = 0; i < 3; i++) {
        found[i] = virtmemFindAslr(sizes[i], 0x1000);
        L("  virtmemFindAslr(%llu MiB) -> %p", sizes[i] / MiB, found[i]);
    }
    char* base = (char*)found[1];
    VirtmemReservation* rv = base ? virtmemAddReservation(base, 4608 * MiB) : NULL;
    virtmemUnlock();
    if (!base) { L("  cannot reserve 4.5 GiB: aborting aliasing tests"); goto done; }
    L("  reserved 4.5 GiB at %p (reservation=%p), 4 GiB aligned? %s", base, rv, ((u64)base & 0xFFFFFFFF) == 0 ? "yes" : "no");
    query("reservation", base);

    // ---- (b) aliasing ----
    L("[b] aliases of a backing block");
    {
        const u64 BACK = 512 * MiB;
        SharedMemory sh;
        Result rc = shmemCreate(&sh, BACK, Perm_Rw, Perm_Rw);
        L("  b1 shmemCreate(512 MiB) rc=0x%x handle=0x%x", rc, sh.handle);
        if (R_SUCCEEDED(rc)) {
            u64 used2 = info(InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            L("  used memory after shmem create: %llu MiB (+%llu)", used2 / MiB, (used2 - used) / MiB);
            char* v[4]; int nmapped = 0;
            for (int i = 0; i < 4; i++) {
                v[i] = base + (u64)(i + 1) * BACK;  // 512 MiB slots inside the reservation
                rc = svcMapSharedMemory(sh.handle, v[i], BACK, Perm_Rw);
                L("  b1 map view %d at %p rc=0x%x", i, v[i], rc);
                if (R_SUCCEEDED(rc)) nmapped++; else break;
            }
            if (nmapped >= 2) {
                alias_check("shmem view0/view1", (volatile u32*)v[0], (volatile u32*)v[1]);
                if (nmapped >= 3) alias_check("shmem view0/view2", (volatile u32*)v[0], (volatile u32*)v[2]);
                query("view0", v[0]);
            }
            // partial map (needed for 0xE0000000 at +0x1000 and sub-views)?
            char* pv = base + 6 * BACK;
            rc = svcMapSharedMemory(sh.handle, pv, 64 * MiB, Perm_Rw);
            L("  b1 partial map (64 MiB of 512) rc=0x%x", rc);
            if (R_SUCCEEDED(rc)) svcUnmapSharedMemory(sh.handle, pv, 64 * MiB);
            if (nmapped >= 1) {
                rc = svcSetMemoryPermission(v[0] + 0x1000, 0x1000, Perm_R);
                L("  b1 svcSetMemoryPermission(R) on shmem page rc=0x%x", rc);
                if (R_SUCCEEDED(rc)) svcSetMemoryPermission(v[0] + 0x1000, 0x1000, Perm_Rw);
                rc = svcSetProcessMemoryPermission(own_handle(), (u64)v[0] + 0x1000, 0x1000, Perm_R);
                L("  b1 svcSetProcessMemoryPermission(R) on shmem page rc=0x%x", rc);
                if (R_SUCCEEDED(rc)) svcSetProcessMemoryPermission(own_handle(), (u64)v[0] + 0x1000, 0x1000, Perm_Rw);
                L("[c] fault tests on shared-memory view");
                fault_test("shmem/setmemperm", (volatile u32*)(v[0] + 0x2000), 1, Perm_R);
                fault_test("shmem/setprocperm", (volatile u32*)(v[0] + 0x3000), 2, Perm_R);
            }
            for (int i = 0; i < nmapped; i++) svcUnmapSharedMemory(sh.handle, v[i], BACK);
            shmemClose(&sh);
        }
    }
    {
        const u64 SZ = 4 * MiB;
        void* heap = memalign(0x200000, 64 * MiB);  // Heap-type memory out of the bounded libnx heap
        Result rc = heap ? 0 : 1;
        L("  memalign(64 MiB) heap=%p", heap);
        if (R_SUCCEEDED(rc)) {
            volatile u32* src = (volatile u32*)((char*)heap + 8 * MiB);
            src[0] = 0x11112222;
            char* dst = base + 3584 * MiB;
            rc = svcMapProcessMemory(dst, own_handle(), (u64)src, SZ);
            L("  b3 svcMapProcessMemory(dst=%p, own handle, src=%p) rc=0x%x", dst, src, rc);
            if (R_SUCCEEDED(rc)) {
                query("b3 dst", dst); query("b3 src", (void*)src);
                volatile u32* d = (volatile u32*)dst;
                L("  b3 dst[0]=0x%x (src was 0x11112222)", d[0]);
                alias_check("heap/processmem", src, d);
                fault_test("procmem/setprocperm", (volatile u32*)(dst + 0x2000), 2, Perm_R);
                svcUnmapProcessMemory(dst, own_handle(), (u64)src, SZ);
            }
            virtmemLock();
            void* sdst = virtmemFindStack(SZ, 0x1000);
            virtmemUnlock();
            L("  b2 stack-region dst=%p", sdst);
            if (sdst) {
                rc = svcMapMemory(sdst, (void*)src, SZ);
                L("  b2 svcMapMemory rc=0x%x", rc);
                if (R_SUCCEEDED(rc)) {
                    query("b2 src after map", (void*)src);
                    query("b2 dst", sdst);
                    svcUnmapMemory(sdst, (void*)src, SZ);
                }
            }
            // Walk the stack region with svcQueryMemory to see what already lives there.
            {
                u64 a = stack_a; int n = 0;
                while (a < stack_a + stack_s && n < 24) {
                    MemoryInfo mi; u32 pi;
                    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
                    L("  stack-region block @%llx size=%llx type=%s perm=%u", (unsigned long long)mi.addr, (unsigned long long)mi.size, memtype(mi.type), mi.perm);
                    a = mi.addr + mi.size; n++;
                }
            }
            // b2b: svcMapMemory exactly at the start / end of the stack region (edge behaviour).
            {
                const u64 U = 0x10000;
                u64 offs[3] = {0, U, stack_s - U};
                const char* names[3] = {"stack start", "stack start+64K", "stack end-64K"};
                for (int i = 0; i < 3; i++) {
                    rc = svcMapMemory((void*)(stack_a + offs[i]), (void*)((char*)heap + 32 * MiB), U);
                    L("  b2b svcMapMemory at %s (%llx): rc=0x%x", names[i], (unsigned long long)(stack_a + offs[i]), rc);
                    if (R_SUCCEEDED(rc)) svcUnmapMemory((void*)(stack_a + offs[i]), (void*)((char*)heap + 32 * MiB), U);
                }
            }
            L("[c] fault test on plain heap memory (baseline)");
            fault_test("heap/setmemperm", (volatile u32*)((char*)heap + 16 * MiB), 1, Perm_R);
            rc = svcMapPhysicalMemory((void*)alias_a, 2 * MiB);
            L("  b4 svcMapPhysicalMemory(alias_a, 2 MiB) rc=0x%x", rc);
            if (R_SUCCEEDED(rc)) { ((volatile u32*)alias_a)[0] = 1; svcUnmapPhysicalMemory((void*)alias_a, 2 * MiB); }
        }
    }
    total = info(InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    used = info(InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    L("[d] after tests: total=%llu MiB used=%llu MiB", total / MiB, used / MiB);
done:
    L("== done ==");
    flush_file();
    printf("\nResults written to sdmc:/memprobe.txt. Press + to exit.\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
