#include <stdint.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/io.h"
#include "include/errno.h"
#include "include/kstring.h"

#include "drivers/char/serial.h"
#include "drivers/video/framebuffer.h"
#include "drivers/video/gpu.h"
#include "drivers/video/console.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "gfx/compositor.h"   /* compositor_pointer_tick() -- cursor/focus/drag */
#include "drivers/timer/timer.h"
#include "drivers/timer/hpet.h"
#include "drivers/timer/rtc.h"
#include "drivers/bus/pci.h"
#include "drivers/audio/ac97.h"
#include "drivers/usb/usb.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/ahci.h"
#include "drivers/video/bootanim.h"

#include "arch/x86_64/cpu/gdt.h"
#include "arch/x86_64/cpu/percpu.h"
#include "arch/x86_64/smp/smp.h"
#include "arch/x86_64/boot/boot_protocol.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/irq/pic.h"
#include "arch/x86_64/irq/irq.h"
#include "arch/x86_64/irq/lapic.h"
#include "arch/x86_64/irq/ioapic.h"
#include "arch/x86_64/cpu/fpu.h"

#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"

#include "acpi/acpi.h"
#include "net/net.h"
#include "block/block.h"
#include "block/partition.h"
#include "fs/fat32.h"
#include "fs/embkfs/embkfs.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "include/kmalloc.h"
#include "lib/ksym.h"          /* the panic symbolizer (§7): load kernel.embdbg */
#include "fs/epfs.h"
#include "ipc/channel.h"     /* channel_live_count() -- IPC Explorer */
#include "ipc/endpoint.h"    /* endpoint_live_count() */
#include "ipc/pipe.h"        /* pipe_live_count() */
#include "gfx/surface.h"     /* surface_live_count() */
#include "selftests.h"

#include "kworker/kworker.h"

#include "process/process.h"
#include "tty/tty.h"


extern uint64_t lapic_timer_get_ticks(void);

/* --------------------------------------------------------------------
 * Interactive process control (run/ps/kill/wait/nice). The kernel's own
 * shell loop below is itself a real, schedulable `current_process` (see
 * process_adopt_current()'s comment) rather than a privileged one-way
 * hand-off, so these commands are just direct calls into process.c's
 * kernel-internal API -- no capability-handle indirection needed here the
 * way cpu/syscall.c's sys_spawn/sys_wait/sys_kill need it, since this code
 * is trusted kernel code, not a sandboxed ring-3 caller.
 * -------------------------------------------------------------------- */

/* If `cmd` starts with `prefix` followed by either a space or the end of
 * the string, return a pointer to the first non-space character after it
 * (possibly the terminating NUL, if there were no arguments). NULL if
 * `cmd` doesn't start with `prefix` at all, or is a different, longer
 * command that merely happens to start with the same letters (e.g. "ps"
 * must not match a "psomething" command that doesn't exist). */
static const char *shell_match_prefix(const char *cmd, const char *prefix)
{
    size_t len = strlen(prefix);
    if (strncmp(cmd, prefix, len) != 0) {
        return NULL;
    }
    if (cmd[len] != '\0' && cmd[len] != ' ') {
        return NULL;
    }
    const char *rest = cmd + len;
    while (*rest == ' ') {
        rest++;
    }
    return rest;
}

/* Minimal unsigned base-10 parser -- no libc, and pids/priorities are
 * always small non-negative numbers typed at this shell, so anything more
 * than "stop at the first non-digit" is unneeded complexity. */
static uint32_t shell_parse_uint(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/* Parse a 64-bit value, auto-detecting base: a "0x"/"0X" prefix means hex,
 * otherwise decimal. For addresses (pagewalk) which are naturally hex. Stops at
 * the first non-digit. `*endp`, if non-NULL, is set past the last digit. */
static uint64_t shell_parse_u64(const char *s, const char **endp)
{
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;;) {
            char c = *s;
            uint64_t d;
            if (c >= '0' && c <= '9') d = (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
            else break;
            v = (v << 4) | d;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    }
    if (endp) *endp = s;
    return v;
}

/* Skip spaces. */
static const char *shell_skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

/* The lowest-pid USER (non-kthread) process, for the no-pid forms of vmmap/
 * pagewalk. Returns 0 if none. */
static uint32_t shell_default_user_pid(void)
{
    struct process_info procs[MAX_PROCESSES];
    int np = process_list(procs, MAX_PROCESSES);
    uint32_t best = 0;
    for (int i = 0; i < np; i++)
        if (!procs[i].is_kthread && (best == 0 || procs[i].pid < best))
            best = procs[i].pid;
    return best;
}

/* Decode a leaf PTE's permission bits into a fixed "rwxu" string:
 *   r always (a present page is readable), w if WRITABLE, x if !NX, u if USER.
 * Writes exactly 4 chars + NUL into `buf` (>= 5 bytes). */
static void mem_perm_str(uint64_t flags, char *buf)
{
    buf[0] = 'r';
    buf[1] = (flags & VMM_WRITABLE) ? 'w' : '-';
    buf[2] = (flags & VMM_NX)       ? '-' : 'x';
    buf[3] = (flags & VMM_USER)     ? 'u' : 'k';
    buf[4] = '\0';
}

/* PCI base-class-code -> short human name, for the Device Explorer. */
static const char *pci_class_name(uint8_t class_code)
{
    switch (class_code) {
        case 0x00: return "unclassified";
        case 0x01: return "storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x04: return "multimedia";
        case 0x05: return "memory";
        case 0x06: return "bridge";
        case 0x07: return "comm";
        case 0x08: return "system";
        case 0x09: return "input";
        case 0x0C: return "serial-bus";
        case 0x0D: return "wireless";
        default:   return "other";
    }
}

/* USB host-controller kind -> name (usb_hc_name is private to usb.c). */
static const char *usb_kind_name(enum usb_hc_kind kind)
{
    switch (kind) {
        case USB_HC_UHCI: return "UHCI";
        case USB_HC_OHCI: return "OHCI";
        case USB_HC_EHCI: return "EHCI";
        case USB_HC_XHCI: return "xHCI";
        default:          return "unknown";
    }
}

/* fd backing kind -> name, for the VFS `files` view. */
static const char *fd_backing_name(uint8_t b)
{
    switch (b) {
        case FD_BACKING_VNODE:   return "file";
        case FD_BACKING_CONSOLE: return "console";
        case FD_BACKING_PIPE:    return "pipe";
        case FD_BACKING_NULLDEV: return "null";
        default:                 return "none";
    }
}

/* obj_handle kind -> name, for the Handle/IPC explorers. */
static const char *handle_kind_name(uint8_t k)
{
    switch (k) {
        case HANDLE_KIND_SURFACE:  return "surface";
        case HANDLE_KIND_CHANNEL:  return "channel";
        case HANDLE_KIND_ENDPOINT: return "endpoint";
        case HANDLE_KIND_PIPE:     return "pipe";
        default:                   return "none";
    }
}

/* vnode type VFS_DT_* -> single char, for compact fd listings. */
static char vfs_dt_char(uint8_t t)
{
    switch (t) {
        case VFS_DT_REG:      return 'f';
        case VFS_DT_DIR:      return 'd';
        case VFS_DT_LNK:      return 'l';
        case VFS_DT_ENDPOINT: return 'e';
        case VFS_DT_CHAR:     return 'c';
        case VFS_DT_FIFO:     return 'p';
        default:              return '?';
    }
}

static const char *process_state_name(enum process_state s)
{
    switch (s) {
        case PROCESS_UNUSED:  return "UNUSED";
        case PROCESS_READY:   return "READY";
        case PROCESS_RUNNING: return "RUNNING";
        case PROCESS_BLOCKED: return "BLOCKED";
        case PROCESS_ZOMBIE:  return "ZOMBIE";
        default:              return "?";
    }
}

/* Returns true if `cmd` was one of the process-control commands (handled,
 * whether or not it succeeded) so the caller doesn't also try the
 * selftests dispatcher or print "unknown command". */
/* ======================================================================
 * EmbDBG LIVE — a full-screen, MULTI-PANEL kernel debugger dashboard over the
 * serial console (`embdbg` / `monitor`). Tabbed live views + a SPLIT-SCREEN
 * master/detail mode (a list on the left, the selected thread/process's full
 * inspect + kernel-stack backtrace on the right), row selection, scrolling,
 * pause, a help overlay, and auto terminal-size detection. Renders straight to
 * serial with absolute cursor positioning (never kprintf — that can hit the VGA
 * console userspace owns), so it never disturbs the graphical desktop.
 * ==================================================================== */
#define TUI_NTABS 8
struct tline { char t[192]; unsigned char attr; };   /* attr: 0 normal 1 header 2 sel 3 dim */

static void tl_push(struct tline *L, int *n, int max, unsigned char attr, const char *s)
{
    if (*n >= max) return;
    int i = 0; for (; s[i] && i < 191; i++) L[*n].t[i] = s[i]; L[*n].t[i] = 0;
    L[*n].attr = attr; (*n)++;
}
static const char *tui_attrcode(unsigned char a)
{
    return a == 1 ? "\033[1m" : a == 2 ? "\033[7m" : a == 3 ? "\033[2m" : (const char *)0;
}
static void tui_draw_cell(int row, int col, int width, const struct tline *ln)
{
    char pos[24]; snprintf(pos, sizeof pos, "\033[%d;%dH", row, col);
    serial_write_string(pos);
    const char *a = ln ? tui_attrcode(ln->attr) : (const char *)0;
    if (a) serial_write_string(a);
    int i = 0;
    if (ln) for (; ln->t[i] && i < width; i++) serial_write_char(ln->t[i]);
    for (; i < width; i++) serial_write_char(' ');
    if (a) serial_write_string("\033[0m");
}
/* A pane with a FROZEN header (L[0]) at `top` and data rows L[1..] scrolled. */
static void tui_draw_pane(int top, int bot, int col, int width, struct tline *L, int n, int scr)
{
    tui_draw_cell(top, col, width, n > 0 ? &L[0] : (struct tline *)0);
    for (int r = top + 1; r <= bot; r++) {
        int idx = 1 + scr + (r - top - 1);
        tui_draw_cell(r, col, width, (idx < n) ? &L[idx] : (struct tline *)0);
    }
}

/* Ask the terminal its size: park the cursor at the far corner + request its
 * position (ESC[6n). The reply ESC[<rows>;<cols>R is clamped to sane bounds;
 * if nothing answers within ~200ms (piped/dumb terminal) we default 80x24. */
static void tui_query_size(int *rows, int *cols)
{
    *rows = 24; *cols = 80;
    serial_write_string("\033[999;999H\033[6n");
    char buf[40]; int bi = 0; int got = 0;
    uint64_t start = lapic_timer_get_ticks();
    while (lapic_timer_get_ticks() < start + 20) {          /* ~200ms at 100Hz */
        while (serial_has_char()) {
            char c = serial_read_char();
            if (bi < 39) buf[bi++] = c;
            if (c == 'R') { got = 1; break; }
        }
        if (got) break;
        __asm__ volatile ("hlt");
    }
    buf[bi] = 0;
    int r = 0, c = 0; const char *p = buf;
    while (*p && *p != '[') p++;
    if (*p == '[') { p++;
        while (*p >= '0' && *p <= '9') r = r * 10 + (*p++ - '0');
        if (*p == ';') { p++; while (*p >= '0' && *p <= '9') c = c * 10 + (*p++ - '0'); }
    }
    if (r >= 12 && r <= 200) *rows = r;
    if (c >= 50 && c <= 380) *cols = c;
}

/* ---- list producers: fill L[] (L[0] = header), return line count. For the
 *      two selectable tabs, when i==sel capture the row's entity id. -------- */
static int tui_lines_threads(struct tline *L, int max, int sel, uint32_t *sel_id)
{
    int n = 0; char b[192];
    tl_push(L, &n, max, 1, "TID  PID  STATE   PRI CPU PIN FLAG    DISPATCH   MIG  KIND");
    struct thread_info thr[MAX_THREADS]; int cnt = thread_list(thr, MAX_THREADS);
    for (int i = 0; i < cnt; i++) {
        char cpu[6], pin[6];
        if (thr[i].running_cpu >= 0) snprintf(cpu, sizeof cpu, "%d", thr[i].running_cpu); else { cpu[0]='-'; cpu[1]=0; }
        if (thr[i].pinned_cpu  >= 0) snprintf(pin, sizeof pin, "%d", thr[i].pinned_cpu);  else { pin[0]='-'; pin[1]=0; }
        snprintf(b, sizeof b, "%-4u %-4u %-7s %-3u %-3s %-3s %-7s %-10llu %-4u %s",
                 thr[i].tid, thr[i].pid, process_state_name(thr[i].state), (unsigned)thr[i].priority,
                 cpu, pin, thr[i].suspended ? "SUSPEND" : "-",
                 (unsigned long long)thr[i].dispatch_count, thr[i].migrations,
                 thr[i].is_kthread ? "kthread" : "user");
        tl_push(L, &n, max, i == sel ? 2 : 0, b);
        if (i == sel && sel_id) *sel_id = thr[i].tid;
    }
    return n;
}
static int tui_lines_procs(struct tline *L, int max, int sel, uint32_t *sel_id)
{
    int n = 0; char b[192];
    tl_push(L, &n, max, 1, "PID  PPID STATE   PRI KIND    THREADS EXIT");
    struct process_info procs[MAX_PROCESSES]; struct thread_info thr[MAX_THREADS];
    int np = process_list(procs, MAX_PROCESSES); int nt = thread_list(thr, MAX_THREADS);
    for (int i = 0; i < np; i++) {
        int tc = 0; for (int j = 0; j < nt; j++) if (thr[j].pid == procs[i].pid) tc++;
        snprintf(b, sizeof b, "%-4u %-4u %-7s %-3u %-7s %-7d %d",
                 procs[i].pid, procs[i].parent_pid, process_state_name(procs[i].state),
                 (unsigned)procs[i].priority, procs[i].is_kthread ? "kthread" : "process", tc, procs[i].exit_code);
        tl_push(L, &n, max, i == sel ? 2 : 0, b);
        if (i == sel && sel_id) *sel_id = procs[i].pid;
    }
    return n;
}
static int tui_lines_sched(struct tline *L, int max)
{
    int n = 0; char b[192];
    struct sched_stats_snapshot st; sched_stats_snapshot(&st);
    struct thread_info thr[MAX_THREADS]; int cnt = thread_list(thr, MAX_THREADS);
    snprintf(b, sizeof b, "Context switches: %llu total", (unsigned long long)st.total);
    tl_push(L, &n, max, 1, b);
    for (uint32_t c = 0; c < st.ncpu; c++) {
        unsigned pct = st.total ? (unsigned)(st.per_cpu[c] * 100 / st.total) : 0;
        char bar[24]; unsigned k = 0; for (; k < pct/5 && k < 20; k++) bar[k] = '#'; bar[k] = 0;
        snprintf(b, sizeof b, "  cpu%-2u %12llu  %3u%%  %s", c, (unsigned long long)st.per_cpu[c], pct, bar);
        tl_push(L, &n, max, 0, b);
    }
    int q[5] = {0,0,0,0,0};
    for (int i = 0; i < cnt; i++) if (thr[i].state <= PROCESS_ZOMBIE) q[thr[i].state]++;
    snprintf(b, sizeof b, "  %d running, %d ready, %d blocked, %d zombie  (%d threads)",
             q[PROCESS_RUNNING], q[PROCESS_READY], q[PROCESS_BLOCKED], q[PROCESS_ZOMBIE], cnt);
    tl_push(L, &n, max, 0, b);
    tl_push(L, &n, max, 3, "busiest (by dispatch):");
    char taken[MAX_THREADS]; for (int i = 0; i < cnt; i++) taken[i] = 0;
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < cnt; i++) { if (taken[i]) continue; if (best < 0 || thr[i].dispatch_count > thr[best].dispatch_count) best = i; }
        if (best < 0) break; taken[best] = 1;
        snprintf(b, sizeof b, "  t%-3u pid%-3u %11llu disp  %u mig",
                 thr[best].tid, thr[best].pid, (unsigned long long)thr[best].dispatch_count, thr[best].migrations);
        tl_push(L, &n, max, 0, b);
    }
    return n;
}
static int tui_lines_memory(struct tline *L, int max)
{
    int n = 0; char b[192];
    uint32_t pid = shell_default_user_pid();
    if (pid == 0) { tl_push(L, &n, max, 1, "(no user process)"); return n; }
    struct process_detail d; struct thread_info one[1];
    if (process_inspect(pid, &d, one, 1) < 0) { tl_push(L, &n, max, 1, "(process vanished)"); return n; }
    snprintf(b, sizeof b, "vmmap pid %u  pml4 0x%llx  (%d regions)", pid, (unsigned long long)d.pml4_phys, 0);
    tl_push(L, &n, max, 1, b);
    static struct vmm_region regs[96];
    int nr = vmm_enum_user_regions(d.pml4_phys, regs, 96);
    snprintf(L[0].t, sizeof L[0].t, "vmmap pid %u  pml4 0x%llx  (%d regions)", pid, (unsigned long long)d.pml4_phys, nr);
    for (int i = 0; i < nr; i++) {
        char perm[5]; mem_perm_str(regs[i].flags, perm);
        uint64_t sz = regs[i].end - regs[i].start;
        const char *type = "";
        if (d.heap_brk && regs[i].start <= d.heap_brk - 1 && d.heap_brk - 1 < regs[i].end) type = "heap";
        else if (!(regs[i].flags & VMM_WRITABLE) && !(regs[i].flags & VMM_NX)) type = "code";
        snprintf(b, sizeof b, "0x%012llx-0x%012llx %6lluK %s %s%s",
                 (unsigned long long)regs[i].start, (unsigned long long)regs[i].end,
                 (unsigned long long)(sz/1024), perm, regs[i].huge ? "huge " : "", type);
        tl_push(L, &n, max, 0, b);
    }
    return n;
}
static int tui_lines_handles(struct tline *L, int max)
{
    int n = 0; char b[192];
    tl_push(L, &n, max, 1, "OWNER KIND      OBJECT           MAPPING");
    static struct ipc_handle_snap ih[128]; int cnt = ipc_handles_snapshot(ih, 128);
    for (int i = 0; i < cnt; i++) {
        int p = snprintf(b, sizeof b, "pid%-3u %-9s 0x%012llx", (unsigned)ih[i].owner_pid,
                         handle_kind_name(ih[i].kind), (unsigned long long)ih[i].obj);
        if (ih[i].map_bytes) snprintf(b + p, sizeof b - p, " 0x%llx+%lluK",
                 (unsigned long long)ih[i].map_base, (unsigned long long)(ih[i].map_bytes/1024));
        tl_push(L, &n, max, 0, b);
    }
    if (cnt == 0) tl_push(L, &n, max, 3, "(no obj-handles held; kernel-side compositor)");
    return n;
}
static int tui_lines_ipc(struct tline *L, int max)
{
    int n = 0; char b[192];
    snprintf(b, sizeof b, "channels %u  surfaces %u  pipes %u  endpoints %u",
             channel_live_count(), surface_live_count(), pipe_live_count(), endpoint_live_count());
    tl_push(L, &n, max, 1, b);
    struct epfs_ep_info eps[16]; int ne = epfs_endpoints_snapshot(eps, 16);
    for (int i = 0; i < ne; i++) {
        snprintf(b, sizeof b, "  /run/%-14s 0x%llx", eps[i].name, (unsigned long long)eps[i].endpoint);
        tl_push(L, &n, max, 0, b);
    }
    if (ne == 0) tl_push(L, &n, max, 3, "(no published endpoints)");
    return n;
}
static int tui_lines_irq(struct tline *L, int max)
{
    int n = 0; char b[192];
    tl_push(L, &n, max, 1, "IRQ  VEC  COUNT          HANDLER");
    struct irq_line_info irqs[16]; irq_snapshot(irqs);
    static const char *known[16] = { "PIT","keyboard",0,0,0,0,0,0,"RTC",0,0,0,"mouse",0,"ATA-pri","ATA-sec" };
    for (int i = 0; i < 16; i++) {
        if (!irqs[i].handler_addr && irqs[i].count == 0) continue;
        char sym[96];
        if (irqs[i].handler_addr && ksym_ready()) ksym_symbolize(irqs[i].handler_addr, sym, sizeof sym);
        else if (irqs[i].handler_addr) snprintf(sym, sizeof sym, "0x%llx", (unsigned long long)irqs[i].handler_addr);
        else snprintf(sym, sizeof sym, "(unwired)");
        snprintf(b, sizeof b, "%-4d %-4d %-14llu %s%s%s", i, 32 + i,
                 (unsigned long long)irqs[i].count, sym, known[i] ? "  " : "", known[i] ? known[i] : "");
        tl_push(L, &n, max, 0, b);
    }
    snprintf(b, sizeof b, "--   48   %-14llu LAPIC timer (scheduler tick)",
             (unsigned long long)lapic_timer_get_ticks());
    tl_push(L, &n, max, 0, b);
    return n;
}
static int tui_lines_devices(struct tline *L, int max)
{
    int n = 0; char b[192];
    snprintf(b, sizeof b, "PCI devices (%u)  block (%u)  usb-ctrl (%u)",
             pci_devices_count(), embk_block_count(), usb_controller_count());
    tl_push(L, &n, max, 1, b);
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *p = pci_get_device(i); if (!p) continue;
        snprintf(b, sizeof b, "  %02x:%02x.%u %04x:%04x %s", p->bus, p->device, p->function,
                 p->vendor_id, p->device_id, pci_class_name(p->class_code));
        tl_push(L, &n, max, 0, b);
    }
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *bd = embk_block_get(i); if (!bd) continue;
        snprintf(b, sizeof b, "  blk %-8s %lluMB", bd->name,
                 (unsigned long long)(bd->block_count * bd->block_size / (1024*1024)));
        tl_push(L, &n, max, 0, b);
    }
    const struct gpu_driver *g = gpu_active();
    snprintf(b, sizeof b, "  gpu %s", g ? g->name : "plain VBE");
    tl_push(L, &n, max, 0, b);
    return n;
}

/* ---- detail producers (right pane in split): a full inspect of the selected
 *      thread/process, incl. a symbolized kernel-stack backtrace. ---------- */
static int tui_detail_thread(struct tline *L, int max, uint32_t tid)
{
    int n = 0; char b[192];
    struct thread_detail d;
    if (thread_inspect(tid, &d) < 0) { tl_push(L, &n, max, 1, "(no such thread)"); return n; }
    uint64_t now = lapic_timer_get_ticks();
    snprintf(b, sizeof b, "Thread %u  pid %u  %s", (unsigned)d.tid, (unsigned)d.pid, d.is_kthread ? "kernel" : "user");
    tl_push(L, &n, max, 1, b);
    snprintf(b, sizeof b, "state   : %s%s%s", process_state_name(d.state),
             d.suspended ? " +SUSP" : "", (d.state == PROCESS_BLOCKED && d.blocked_on_wq) ? " (wq)" : "");
    tl_push(L, &n, max, 0, b);
    if (d.running_cpu >= 0) snprintf(b, sizeof b, "cpu     : cpu%d (running)", d.running_cpu);
    else snprintf(b, sizeof b, "cpu     : -");
    tl_push(L, &n, max, 0, b);
    if (d.pinned_cpu >= 0) snprintf(b, sizeof b, "affinity: pinned cpu%d", d.pinned_cpu);
    else snprintf(b, sizeof b, "affinity: any core");
    tl_push(L, &n, max, 0, b);
    snprintf(b, sizeof b, "priority: %u   dispatch %llu   mig %u", (unsigned)d.priority,
             (unsigned long long)d.dispatch_count, d.migrations);
    tl_push(L, &n, max, 0, b);
    if (d.exit_tick) snprintf(b, sizeof b, "lifetime: born %llu exit %llu (%llu ticks)",
             (unsigned long long)d.born_tick, (unsigned long long)d.exit_tick,
             (unsigned long long)(d.exit_tick - d.born_tick));
    else snprintf(b, sizeof b, "lifetime: born %llu  age %llu ticks", (unsigned long long)d.born_tick,
             (unsigned long long)(now >= d.born_tick ? now - d.born_tick : 0));
    tl_push(L, &n, max, 0, b);
    snprintf(b, sizeof b, "entry   : 0x%llx", (unsigned long long)d.entry_point); tl_push(L, &n, max, 0, b);
    if (!d.is_kthread) {
        snprintf(b, sizeof b, "user rsp: 0x%llx  TLS 0x%llx",
                 (unsigned long long)d.user_rsp, (unsigned long long)d.fs_base);
        tl_push(L, &n, max, 0, b);
    }
    snprintf(b, sizeof b, "kstack  : 0x%llx", (unsigned long long)d.kstack_top); tl_push(L, &n, max, 0, b);
    if (!d.walkable) {
        snprintf(b, sizeof b, "(running on cpu%d -- live stack not walkable)", d.running_cpu);
        tl_push(L, &n, max, 3, b);
    } else if (!ksym_ready()) {
        tl_push(L, &n, max, 3, "(kernel.embdbg not loaded -- no backtrace)");
    } else if (d.ctx_rip < 0xffffffff80000000ULL) {
        tl_push(L, &n, max, 3, "(saved rip not in kernel .text)");
    } else {
        char sym[160];
        tl_push(L, &n, max, 1, "kernel stack (parked):");
        ksym_symbolize(d.ctx_rip, sym, sizeof sym);
        snprintf(b, sizeof b, "  %s", sym); tl_push(L, &n, max, 0, b);
        uint64_t rbp = d.ctx_rbp;
        for (int i = 0; i < 20 && n < max; i++) {
            if (rbp < 0xffff800000000000ULL || (rbp & 0x7)) break;
            uint64_t ret  = *(volatile uint64_t *)(uintptr_t)(rbp + 8);
            uint64_t next = *(volatile uint64_t *)(uintptr_t)(rbp);
            if (ret < 0xffffffff80000000ULL) break;
            ksym_symbolize(ret, sym, sizeof sym);
            snprintf(b, sizeof b, "  %s", sym); tl_push(L, &n, max, 0, b);
            if (next <= rbp) break;
            rbp = next;
        }
    }
    return n;
}
static int tui_detail_proc(struct tline *L, int max, uint32_t pid)
{
    int n = 0; char b[192];
    struct process_detail d; struct thread_info thr[MAX_THREADS];
    if (process_inspect(pid, &d, thr, MAX_THREADS) < 0) { tl_push(L, &n, max, 1, "(no such process)"); return n; }
    uint64_t now = lapic_timer_get_ticks();
    snprintf(b, sizeof b, "Process %u  (%s)", (unsigned)d.pid, d.is_kthread ? "kernel" : "user");
    tl_push(L, &n, max, 1, b);
    snprintf(b, sizeof b, "state   : %s%s", process_state_name(d.state),
             d.state == PROCESS_ZOMBIE ? " (zombie)" : ""); tl_push(L, &n, max, 0, b);
    snprintf(b, sizeof b, "parent  : %u   priority %u", (unsigned)d.parent_pid, (unsigned)d.priority); tl_push(L, &n, max, 0, b);
    snprintf(b, sizeof b, "caps    : 0x%llx   threads %d", (unsigned long long)d.cap_set, d.live_thread_count); tl_push(L, &n, max, 0, b);
    if (d.exit_tick) snprintf(b, sizeof b, "lifetime: born %llu exit %llu (%llu ticks) exit=%d",
             (unsigned long long)d.born_tick, (unsigned long long)d.exit_tick,
             (unsigned long long)(d.exit_tick - d.born_tick), d.exit_code);
    else snprintf(b, sizeof b, "lifetime: born %llu  age %llu ticks", (unsigned long long)d.born_tick,
             (unsigned long long)(now >= d.born_tick ? now - d.born_tick : 0));
    tl_push(L, &n, max, 0, b);
    if (!d.is_kthread) { snprintf(b, sizeof b, "heap    : brk 0x%llx  top 0x%llx",
             (unsigned long long)d.heap_brk, (unsigned long long)d.heap_mapped_top); tl_push(L, &n, max, 0, b); }
    tl_push(L, &n, max, 1, "TID  STATE   PRI CPU DISPATCH  MIG");
    for (int i = 0; i < d.thread_count; i++) {
        char cpu[6]; if (thr[i].running_cpu >= 0) snprintf(cpu, sizeof cpu, "%d", thr[i].running_cpu); else { cpu[0]='-'; cpu[1]=0; }
        snprintf(b, sizeof b, "%-4u %-7s %-3u %-3s %-9llu %u", thr[i].tid, process_state_name(thr[i].state),
                 (unsigned)thr[i].priority, cpu, (unsigned long long)thr[i].dispatch_count, thr[i].migrations);
        tl_push(L, &n, max, 0, b);
    }
    return n;
}

/* A centered help overlay drawn over the current screen. */
static void tui_draw_help(int W, int H)
{
    static const char *help[] = {
        " EmbDBG live — keys ",
        "",
        " 1..8        select panel",
        " Tab / n / p  next / prev panel",
        " j / k / arrows  move selection / scroll",
        " s            toggle split (master/detail)",
        " space        pause / resume auto-refresh",
        " r            force refresh",
        " ? or h       this help",
        " q            quit",
        "",
        " split shows the selected thread/process's",
        " full inspect + kernel-stack backtrace.",
        " press any key to close help ",
    };
    int rows = (int)(sizeof help / sizeof help[0]);
    int bw = 46, bh = rows + 2;
    int r0 = (H - bh) / 2; if (r0 < 1) r0 = 1;
    int c0 = (W - bw) / 2; if (c0 < 1) c0 = 1;
    for (int r = 0; r < bh; r++) {
        struct tline t; t.attr = (r == 0 || r == 1) ? 1 : 0;
        const char *s = (r >= 1 && r - 1 < rows) ? help[r - 1] : "";
        int i = 0; for (; s[i] && i < 191; i++) t.t[i] = s[i]; t.t[i] = 0;
        tui_draw_cell(r0 + r, c0, bw, &t);
    }
}

static void tui_render(int tab, int sel, int scr, int split, int paused, int help, int W, int H,
                       uint32_t *out_ndata)
{
    static const char *tag[TUI_NTABS]  = { "1:Thr","2:Prc","3:Sch","4:Mem","5:Hnd","6:IPC","7:IRQ","8:Dev" };
    static const char *name[TUI_NTABS] = { "Threads","Processes","Scheduler","Memory","Handles","IPC","Interrupts","Devices" };
    static struct tline ML[300], DL[80];
    int selectable = (tab == 0 || tab == 1);
    uint32_t sel_id = 0;
    int mn = 0;
    switch (tab) {
        case 0: mn = tui_lines_threads(ML, 300, sel, &sel_id); break;
        case 1: mn = tui_lines_procs(ML, 300, sel, &sel_id); break;
        case 2: mn = tui_lines_sched(ML, 300); break;
        case 3: mn = tui_lines_memory(ML, 300); break;
        case 4: mn = tui_lines_handles(ML, 300); break;
        case 5: mn = tui_lines_ipc(ML, 300); break;
        case 6: mn = tui_lines_irq(ML, 300); break;
        default: mn = tui_lines_devices(ML, 300); break;
    }
    if (out_ndata) *out_ndata = (mn > 0) ? (uint32_t)(mn - 1) : 0;

    /* tab bar (row 1) */
    char bar[240]; int p = 0;
    p += snprintf(bar + p, sizeof bar - p, "\033[7m EmbDBG live \033[0m ");
    for (int i = 0; i < TUI_NTABS; i++)
        p += snprintf(bar + p, sizeof bar - p, i == tab ? "\033[7m%s\033[0m " : "%s ", tag[i]);
    { char pos[16]; snprintf(pos, sizeof pos, "\033[1;1H\033[K"); serial_write_string(pos); serial_write_string(bar); }
    char sub[160];
    snprintf(sub, sizeof sub, "%s%s%s   [?] help", name[tab],
             split ? "  |  split" : "", paused ? "  |  PAUSED" : "  (live ~1s)");
    { char pos[16]; snprintf(pos, sizeof pos, "\033[2;1H\033[K"); serial_write_string(pos);
      serial_write_string("\033[1m"); serial_write_string(sub); serial_write_string("\033[0m"); }
    { char pos[16]; snprintf(pos, sizeof pos, "\033[3;1H\033[K"); serial_write_string(pos); }

    int top = 4, bot = H - 1;
    int leftW = split ? (W - 1) / 2 : W;
    tui_draw_pane(top, bot, 1, leftW, ML, mn, scr);
    if (split) {
        for (int r = top; r <= bot; r++) { char pos[16]; snprintf(pos, sizeof pos, "\033[%d;%dH", r, leftW + 1);
            serial_write_string(pos); serial_write_string("\033[2m|\033[0m"); }
        int dn = 0;
        if (tab == 0) dn = tui_detail_thread(DL, 80, sel_id);
        else if (tab == 1) dn = tui_detail_proc(DL, 80, sel_id);
        else tl_push(DL, &dn, 80, 3, "(split detail: Threads/Processes only)");
        tui_draw_pane(top, bot, leftW + 2, W - leftW - 1, DL, dn, 0);
    }

    /* footer (row H) */
    char foot[200];
    snprintf(foot, sizeof foot, "[1-8] panel  [jk] %s  [s] split  [space] %s  [r] refresh  [q] quit",
             selectable ? "select" : "scroll", paused ? "resume" : "pause");
    { char pos[16]; snprintf(pos, sizeof pos, "\033[%d;1H\033[K", H); serial_write_string(pos);
      serial_write_string("\033[7m "); serial_write_string(foot); serial_write_string(" \033[0m"); }

    if (help) tui_draw_help(W, H);
}

static void kernel_tui(void)
{
    int W, H; tui_query_size(&H, &W);
    if (W > 200) W = 200; if (H > 60) H = 60;
    int tab = 0, sel = 0, scr = 0, split = 0, paused = 0, help = 0, dirty = 1;
    uint32_t ndata = 0;
    uint64_t next = 0;
    serial_write_string("\033[?25l\033[2J");
    for (;;) {
        int quit = 0, moved = 0;
        while (serial_has_char()) {
            char c = serial_read_char();
            if (help) { help = 0; dirty = 1; continue; }   /* any key closes help */
            if (c == 'q' || c == 3) { quit = 1; break; }
            else if (c >= '1' && c <= '8') { tab = c - '1'; sel = 0; scr = 0; dirty = 1; }
            else if (c == '\t' || c == 'n') { tab = (tab + 1) % TUI_NTABS; sel = 0; scr = 0; dirty = 1; }
            else if (c == 'p') { tab = (tab + TUI_NTABS - 1) % TUI_NTABS; sel = 0; scr = 0; dirty = 1; }
            else if (c == 'j') { sel++; moved = 1; }
            else if (c == 'k') { if (sel > 0) sel--; moved = 1; }
            else if (c == 's') { split = !split; dirty = 1; }
            else if (c == ' ') { paused = !paused; dirty = 1; }
            else if (c == '?' || c == 'h') { help = 1; dirty = 1; }
            else if (c == 'r') { dirty = 1; }
            else if (c == 27) {
                if (serial_has_char() && serial_read_char() == '[') {
                    char d = serial_has_char() ? serial_read_char() : 0;
                    if (d == 'A') { if (sel > 0) sel--; moved = 1; }
                    else if (d == 'B') { sel++; moved = 1; }
                    else if (d == 'C') { tab = (tab + 1) % TUI_NTABS; sel = 0; scr = 0; dirty = 1; }
                    else if (d == 'D') { tab = (tab + TUI_NTABS - 1) % TUI_NTABS; sel = 0; scr = 0; dirty = 1; }
                }
            }
        }
        if (quit) break;

        /* clamp selection to the data + keep it visible (scroll follows). */
        if (ndata == 0) sel = 0;
        else if (sel >= (int)ndata) sel = (int)ndata - 1;
        if (sel < 0) sel = 0;
        int bodyrows = (H - 1) - (4 + 1) + 1;      /* rows below the frozen header */
        if (bodyrows < 1) bodyrows = 1;
        if (sel < scr) scr = sel;
        if (sel >= scr + bodyrows) scr = sel - bodyrows + 1;
        if (moved) dirty = 1;

        uint64_t now = lapic_timer_get_ticks();
        if (dirty || (!paused && now >= next)) {
            tui_render(tab, sel, scr, split, paused, help, W, H, &ndata);
            next = now + 100; dirty = 0;
        }
        __asm__ volatile ("hlt");
    }
    serial_write_string("\033[?25h\033[2J\033[H");
}

static bool shell_handle_process_command(const char *cmd)
{
    const char *arg;

    if ((arg = shell_match_prefix(cmd, "run")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[run] usage: run <path>\n");
            return true;
        }
        char *argv[] = { (char *)arg, NULL };
        int pid = process_create(arg, argv, 1, NULL, 0);
        if (pid < 0) {
            kprintf("\n[run] failed to start %s: %s\n", arg, embk_strerror(pid));
        } else {
            kprintf("\n[run] started %s as pid %d\n", arg, pid);
        }
        return true;
    }

    if (strcmp(cmd, "ps") == 0) {
        struct process_info procs[MAX_PROCESSES];
        int n = process_list(procs, MAX_PROCESSES);
        kprintf("\nPID  PPID STATE   PRI KIND    EXIT\n");
        for (int i = 0; i < n; i++) {
            kprintf("%-4u %-4u %-7s %-3u %-7s %d\n",
                    (unsigned int)procs[i].pid, (unsigned int)procs[i].parent_pid,
                    process_state_name(procs[i].state),
                    (unsigned int)procs[i].priority,
                    procs[i].is_kthread ? "kthread" : "process",
                    procs[i].exit_code);
        }
        return true;
    }

    /* --- EmbDBG v2: kernel-aware introspection. Live snapshots of the very
     * tables the scheduler runs on — processes, threads, and their scheduling
     * state — walked under g_sched_lock and printed. No instrumentation, no
     * new syscalls: it reads what the kernel already holds. --- */
    if (strcmp(cmd, "process list") == 0 || strcmp(cmd, "process") == 0) {
        struct process_info procs[MAX_PROCESSES];
        struct thread_info  thr[MAX_THREADS];
        int np = process_list(procs, MAX_PROCESSES);
        int nt = thread_list(thr, MAX_THREADS);
        kprintf("\nPID  PPID STATE   PRI KIND    THREADS EXIT\n");
        for (int i = 0; i < np; i++) {
            int tc = 0;
            for (int j = 0; j < nt; j++) if (thr[j].pid == procs[i].pid) tc++;
            kprintf("%-4u %-4u %-7s %-3u %-7s %-7d %d\n",
                    (unsigned)procs[i].pid, (unsigned)procs[i].parent_pid,
                    process_state_name(procs[i].state), (unsigned)procs[i].priority,
                    procs[i].is_kthread ? "kthread" : "process", tc, procs[i].exit_code);
        }
        kprintf("%d process(es), %d thread(s)\n", np, nt);
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "process inspect")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[inspect] usage: process inspect <pid>\n");
            return true;
        }
        uint32_t pid = shell_parse_uint(arg);
        struct process_detail d;
        struct thread_info thr[MAX_THREADS];
        if (process_inspect(pid, &d, thr, MAX_THREADS) < 0) {
            kprintf("\n[inspect] no such process %u\n", (unsigned)pid);
            return true;
        }
        uint64_t now = lapic_timer_get_ticks();
        kprintf("\nProcess %u (%s)\n", (unsigned)d.pid,
                d.is_kthread ? "kernel thread" : "user process");
        kprintf("  state     : %s", process_state_name(d.state));
        if (d.state == PROCESS_ZOMBIE) kprintf("  (exit code %d)", d.exit_code);
        kprintf("\n");
        kprintf("  parent    : %u\n", (unsigned)d.parent_pid);
        kprintf("  priority  : %u\n", (unsigned)d.priority);
        kprintf("  caps      : 0x%llx\n", (unsigned long long)d.cap_set);
        kprintf("  threads   : %d live\n", d.live_thread_count);
        /* Lifetime: born tick, and either age (alive) or lifespan (dead). */
        kprintf("  born tick : %llu\n", (unsigned long long)d.born_tick);
        if (d.exit_tick) {
            kprintf("  exit tick : %llu  (lifespan %llu ticks)\n",
                    (unsigned long long)d.exit_tick,
                    (unsigned long long)(d.exit_tick - d.born_tick));
        } else {
            kprintf("  age       : %llu ticks (alive)\n",
                    (unsigned long long)(now >= d.born_tick ? now - d.born_tick : 0));
        }
        if (!d.is_kthread) {
            kprintf("  heap brk  : 0x%llx  (mapped top 0x%llx)\n",
                    (unsigned long long)d.heap_brk,
                    (unsigned long long)d.heap_mapped_top);
        }
        /* Per-thread breakdown, including the instrumentation history. */
        kprintf("  TID  STATE   PRI CPU DISPATCH  MIG  AGE\n");
        for (int i = 0; i < d.thread_count; i++) {
            kprintf("  %-4u %-7s %-3u ", thr[i].tid,
                    process_state_name(thr[i].state), (unsigned)thr[i].priority);
            if (thr[i].running_cpu >= 0) kprintf("%-3d ", thr[i].running_cpu);
            else kprintf("-   ");
            uint64_t age = thr[i].exit_tick
                         ? thr[i].exit_tick - thr[i].born_tick
                         : (now >= thr[i].born_tick ? now - thr[i].born_tick : 0);
            kprintf("%8llu %4u  %llu\n",
                    (unsigned long long)thr[i].dispatch_count, thr[i].migrations,
                    (unsigned long long)age);
        }
        return true;
    }

    if (strcmp(cmd, "threads") == 0) {
        struct thread_info thr[MAX_THREADS];
        int n = thread_list(thr, MAX_THREADS);
        kprintf("\nTID  PID  STATE   PRI CPU PIN FLAG    KIND\n");
        for (int i = 0; i < n; i++) {
            kprintf("%-4u %-4u %-7s %-3u ", thr[i].tid, thr[i].pid,
                    process_state_name(thr[i].state), (unsigned)thr[i].priority);
            if (thr[i].running_cpu >= 0) kprintf("%-3d ", thr[i].running_cpu); else kprintf("-   ");
            if (thr[i].pinned_cpu  >= 0) kprintf("%-3d ", thr[i].pinned_cpu);  else kprintf("-   ");
            kprintf("%-7s %s\n", thr[i].suspended ? "SUSPEND" : "-",
                    thr[i].is_kthread ? "kthread" : "user");
        }
        kprintf("%d thread(s)\n", n);
        return true;
    }

    if (strcmp(cmd, "scheduler stats") == 0) {
        struct sched_stats_snapshot st;
        sched_stats_snapshot(&st);
        kprintf("\nContext switches: %llu total\n", (unsigned long long)st.total);
        /* Per-core share of the switch load -- how evenly work spread. */
        for (uint32_t c = 0; c < st.ncpu; c++) {
            unsigned pct = st.total ? (unsigned)(st.per_cpu[c] * 100 / st.total) : 0;
            kprintf("  cpu%-2u %10llu  %3u%%  ", c,
                    (unsigned long long)st.per_cpu[c], pct);
            for (unsigned b = 0; b < pct / 5; b++) kprintf("#");   /* 20-wide bar */
            kprintf("\n");
        }
        /* Busiest threads by lifetime dispatch count (a coarse CPU-time proxy),
         * plus their migration history. Simple top-8 selection over the snapshot. */
        struct thread_info thr[MAX_THREADS];
        int n = thread_list(thr, MAX_THREADS);
        bool taken[MAX_THREADS];
        for (int i = 0; i < n; i++) taken[i] = false;
        int show = n < 8 ? n : 8;
        kprintf("Busiest threads (by dispatches):\n");
        kprintf("  TID  PID  DISPATCHES  MIGRATIONS  LASTCPU\n");
        for (int k = 0; k < show; k++) {
            int best = -1;
            for (int i = 0; i < n; i++) {
                if (taken[i]) continue;
                if (best < 0 || thr[i].dispatch_count > thr[best].dispatch_count) best = i;
            }
            if (best < 0) break;
            taken[best] = true;
            kprintf("  %-4u %-4u %11llu %11u  ", thr[best].tid, thr[best].pid,
                    (unsigned long long)thr[best].dispatch_count, thr[best].migrations);
            if (thr[best].last_ran_cpu >= 0) kprintf("cpu%d\n", thr[best].last_ran_cpu);
            else kprintf("-\n");
        }
        return true;
    }

    if (strcmp(cmd, "scheduler timeline") == 0) {
        struct sched_event_view ev[SCHED_TIMELINE_MAX];
        int n = sched_timeline_snapshot(ev, SCHED_TIMELINE_MAX);
        if (n == 0) {
            kprintf("\n[timeline] no context switches recorded yet\n");
            return true;
        }
        kprintf("\nRecent context switches (oldest first, %d shown):\n", n);
        kprintf("  %-14s CPU  FROM -> TO\n", "TICK");
        for (int i = 0; i < n; i++) {
            kprintf("  %-14llu cpu%-2u t%u -> t%u%s\n",
                    (unsigned long long)ev[i].ts, ev[i].cpu,
                    ev[i].from_tid, ev[i].to_tid,
                    ev[i].migrated ? "   (migrated)" : "");
        }
        return true;
    }

    if (strcmp(cmd, "scheduler") == 0 || strcmp(cmd, "scheduler queues") == 0) {
        struct thread_info thr[MAX_THREADS];
        int n = thread_list(thr, MAX_THREADS);
        int cnt[5] = { 0, 0, 0, 0, 0 };
        for (int i = 0; i < n; i++)
            if (thr[i].state <= PROCESS_ZOMBIE) cnt[thr[i].state]++;
        kprintf("\nScheduler: %d running, %d ready, %d blocked, %d zombie  (%d threads)\n",
                cnt[PROCESS_RUNNING], cnt[PROCESS_READY], cnt[PROCESS_BLOCKED],
                cnt[PROCESS_ZOMBIE], n);
        /* Per-queue membership: which threads sit in each scheduling state. */
        static const struct { enum process_state st; const char *label; } queues[] = {
            { PROCESS_RUNNING, "RUNNING (on a CPU)" },
            { PROCESS_READY,   "READY   (runnable)" },
            { PROCESS_BLOCKED, "BLOCKED (waiting)"  },
            { PROCESS_ZOMBIE,  "ZOMBIE  (exited)"   },
        };
        for (unsigned q = 0; q < sizeof queues / sizeof queues[0]; q++) {
            kprintf("  %s:", queues[q].label);
            int any = 0;
            for (int i = 0; i < n; i++) {
                if (thr[i].state != queues[q].st) continue;
                any = 1;
                if (thr[i].state == PROCESS_RUNNING && thr[i].running_cpu >= 0)
                    kprintf(" t%u(pid%u,cpu%d)", thr[i].tid, thr[i].pid, thr[i].running_cpu);
                else
                    kprintf(" t%u(pid%u)", thr[i].tid, thr[i].pid);
            }
            if (!any) kprintf(" (none)");
            kprintf("\n");
        }
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "process suspend")) != NULL) {
        uint32_t pid = shell_parse_uint(arg);
        int n = process_suspend(pid);
        if (n < 0) kprintf("\n[suspend] no such process %u\n", (unsigned)pid);
        else kprintf("\n[suspend] pid %u frozen (%d thread%s)\n", (unsigned)pid, n, n == 1 ? "" : "s");
        return true;
    }
    if ((arg = shell_match_prefix(cmd, "process resume")) != NULL) {
        uint32_t pid = shell_parse_uint(arg);
        int n = process_resume(pid);
        if (n < 0) kprintf("\n[resume] no such process %u\n", (unsigned)pid);
        else kprintf("\n[resume] pid %u resumed (%d thread%s)\n", (unsigned)pid, n, n == 1 ? "" : "s");
        return true;
    }
    if ((arg = shell_match_prefix(cmd, "thread inspect")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[inspect] usage: thread inspect <tid>\n");
            return true;
        }
        uint32_t tid = shell_parse_uint(arg);
        struct thread_detail d;
        if (thread_inspect(tid, &d) < 0) {
            kprintf("\n[inspect] no such thread %u\n", (unsigned)tid);
            return true;
        }
        uint64_t now = lapic_timer_get_ticks();
        kprintf("\nThread %u  (pid %u, %s)\n", (unsigned)d.tid, (unsigned)d.pid,
                d.is_kthread ? "kernel" : "user");
        kprintf("  state     : %s%s%s\n", process_state_name(d.state),
                d.suspended ? " +SUSPENDED" : "",
                (d.state == PROCESS_BLOCKED && d.blocked_on_wq) ? " (on wait-queue)" : "");
        kprintf("  priority  : %u\n", (unsigned)d.priority);
        /* CPU affinity: where it runs now, where it's pinned, where it last ran. */
        if (d.running_cpu >= 0) kprintf("  cpu       : cpu%d (running)\n", d.running_cpu);
        else kprintf("  cpu       : - (not running)\n");
        if (d.pinned_cpu >= 0) kprintf("  affinity  : pinned to cpu%d\n", d.pinned_cpu);
        else kprintf("  affinity  : any core\n");
        /* Migration history + dispatch count (the instrumentation counters). */
        kprintf("  dispatch  : %llu   migrations: %u   last cpu: ",
                (unsigned long long)d.dispatch_count, d.migrations);
        if (d.last_ran_cpu >= 0) kprintf("cpu%d\n", d.last_ran_cpu); else kprintf("-\n");
        /* Lifetime. */
        if (d.exit_tick)
            kprintf("  lifetime  : born %llu, exit %llu (lifespan %llu ticks)\n",
                    (unsigned long long)d.born_tick, (unsigned long long)d.exit_tick,
                    (unsigned long long)(d.exit_tick - d.born_tick));
        else
            kprintf("  lifetime  : born %llu, age %llu ticks (alive)\n",
                    (unsigned long long)d.born_tick,
                    (unsigned long long)(now >= d.born_tick ? now - d.born_tick : 0));
        /* Register/stack anchors. */
        kprintf("  entry     : 0x%llx\n", (unsigned long long)d.entry_point);
        if (!d.is_kthread)
            kprintf("  user rsp  : 0x%llx    TLS (fs_base): 0x%llx\n",
                    (unsigned long long)d.user_rsp, (unsigned long long)d.fs_base);
        kprintf("  kstack top: 0x%llx\n", (unsigned long long)d.kstack_top);
        /* Stack Viewer: symbolized kernel-stack backtrace. Only meaningful for a
         * NOT-running thread (its saved ctx is a real parked frame); walking a
         * live thread's stale ctx would name where it last parked, not where it
         * is. Guarded to kernel VAs + capped, same discipline as the panic path. */
        if (!d.walkable) {
            kprintf("  kstack    : (thread is RUNNING on cpu%d -- live stack not walkable)\n",
                    d.running_cpu);
        } else if (!ksym_ready()) {
            kprintf("  kstack    : (kernel.embdbg not loaded -- backtrace unavailable)\n");
        } else if (d.ctx_rip < 0xffffffff80000000ULL) {
            kprintf("  kstack    : (saved rip 0x%llx not in kernel .text)\n",
                    (unsigned long long)d.ctx_rip);
        } else {
            char sym[176];
            kprintf("  kernel stack (parked at):\n");
            ksym_symbolize(d.ctx_rip, sym, sizeof sym);
            kprintf("    %s\n", sym);
            uint64_t rbp = d.ctx_rbp;
            for (int i = 0; i < 24; i++) {
                if (rbp < 0xffff800000000000ULL || (rbp & 0x7)) break;
                uint64_t ret  = *(volatile uint64_t *)(uintptr_t)(rbp + 8);
                uint64_t next = *(volatile uint64_t *)(uintptr_t)(rbp);
                if (ret < 0xffffffff80000000ULL) break;   /* left kernel .text */
                ksym_symbolize(ret, sym, sizeof sym);
                kprintf("    %s\n", sym);
                if (next <= rbp) break;                    /* chain must climb */
                rbp = next;
            }
        }
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "thread suspend")) != NULL) {
        uint32_t tid = shell_parse_uint(arg);
        int n = thread_suspend(tid);
        kprintf(n < 0 ? "\n[suspend] tid %u not suspendable (unused or pinned)\n"
                      : "\n[suspend] tid %u frozen\n", (unsigned)tid);
        return true;
    }
    if ((arg = shell_match_prefix(cmd, "thread resume")) != NULL) {
        uint32_t tid = shell_parse_uint(arg);
        int n = thread_resume(tid);
        kprintf(n < 0 ? "\n[resume] tid %u not resumable\n"
                      : "\n[resume] tid %u resumed\n", (unsigned)tid);
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "vmmap")) != NULL) {
        arg = shell_skip_ws(arg);
        uint32_t pid = arg[0] ? (uint32_t)shell_parse_u64(arg, NULL)
                              : shell_default_user_pid();
        if (pid == 0) { kprintf("\n[vmmap] no user process to map\n"); return true; }
        struct process_detail d;
        struct thread_info one[1];
        if (process_inspect(pid, &d, one, 1) < 0) {
            kprintf("\n[vmmap] no such process %u\n", (unsigned)pid);
            return true;
        }
        /* main thread's user rsp -> deterministic stack-region label. */
        uint64_t ursp = 0;
        if (d.thread_count > 0) {
            struct thread_detail td;
            if (thread_inspect(one[0].tid, &td) == 0) ursp = td.user_rsp;
        }
        static struct vmm_region regions[96];
        int nr = vmm_enum_user_regions(d.pml4_phys, regions, 96);
        kprintf("\nvmmap pid %u (%s)  pml4 0x%llx\n", (unsigned)pid,
                d.is_kthread ? "kernel-space" : "user-space",
                (unsigned long long)d.pml4_phys);
        kprintf("  %-18s %-18s %-9s PERM  TYPE\n", "START", "END", "SIZE");
        uint64_t total = 0;
        for (int i = 0; i < nr; i++) {
            char perm[5]; mem_perm_str(regions[i].flags, perm);
            uint64_t sz = regions[i].end - regions[i].start;
            total += sz;
            const char *type = "";
            if (d.heap_brk && regions[i].start <= d.heap_brk - 1 &&
                d.heap_brk - 1 < regions[i].end) type = "heap";
            else if (ursp && regions[i].start <= ursp && ursp < regions[i].end) type = "stack";
            else if (!(regions[i].flags & VMM_WRITABLE) && !(regions[i].flags & VMM_NX))
                type = "code";
            kprintf("  0x%016llx 0x%016llx %6lluK  %s  %s%s\n",
                    (unsigned long long)regions[i].start,
                    (unsigned long long)regions[i].end,
                    (unsigned long long)(sz / 1024), perm,
                    regions[i].huge ? "(huge) " : "", type);
        }
        kprintf("%d region(s), %lluK mapped\n", nr, (unsigned long long)(total / 1024));
        if (nr == 96) kprintf("(list capped at 96 regions)\n");
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "pagewalk")) != NULL) {
        arg = shell_skip_ws(arg);
        if (!arg[0]) {
            kprintf("\n[pagewalk] usage: pagewalk <addr> | pagewalk <pid> <addr>\n");
            return true;
        }
        const char *e1;
        uint64_t v1 = shell_parse_u64(arg, &e1);
        const char *rest = shell_skip_ws(e1);
        uint64_t pml4, addr;
        uint32_t pid = 0;
        bool kernel_space = false;
        if (rest[0]) {
            /* two tokens: <pid> <addr> */
            pid = (uint32_t)v1;
            addr = shell_parse_u64(rest, NULL);
            struct process_detail d; struct thread_info one[1];
            if (process_inspect(pid, &d, one, 1) < 0) {
                kprintf("\n[pagewalk] no such process %u\n", (unsigned)pid);
                return true;
            }
            pml4 = d.pml4_phys;
        } else {
            /* one token: <addr>. Kernel VA -> kernel space; user VA -> default proc. */
            addr = v1;
            if (addr >= 0xffff800000000000ULL) {
                pml4 = vmm_get_kernel_pml4();
                kernel_space = true;
            } else {
                pid = shell_default_user_pid();
                if (pid == 0) { kprintf("\n[pagewalk] no user process; give a kernel addr or <pid> <addr>\n"); return true; }
                struct process_detail d; struct thread_info one[1];
                process_inspect(pid, &d, one, 1);
                pml4 = d.pml4_phys;
            }
        }
        struct vmm_walk_result r;
        vmm_walk(pml4, addr, &r);
        kprintf("\npagewalk 0x%llx  in %s", (unsigned long long)addr,
                kernel_space ? "kernel space" : "");
        if (!kernel_space) kprintf("pid %u space", (unsigned)pid);
        kprintf("  (pml4 0x%llx)\n", (unsigned long long)pml4);
        static const char *lvlname[4] = { "PML4E", "PDPTE", "PDE  ", "PTE  " };
        for (int i = 0; i < r.levels_read; i++) {
            uint64_t e = r.entry[i];
            char bits[8];
            bits[0] = (e & VMM_PRESENT)  ? 'P' : '-';
            bits[1] = (e & VMM_WRITABLE) ? 'W' : '-';
            bits[2] = (e & VMM_USER)     ? 'U' : '-';
            bits[3] = (e & VMM_ACCESSED) ? 'A' : '-';
            bits[4] = (e & VMM_DIRTY)    ? 'D' : '-';
            bits[5] = (e & VMM_HUGE)     ? 'H' : '-';
            bits[6] = (e & VMM_NX)       ? 'X' : '-';   /* X = NX set (no-execute) */
            bits[7] = '\0';
            kprintf("  %s[%3d] = 0x%016llx  [%s]", lvlname[i], r.index[i],
                    (unsigned long long)e, bits);
            if (!(e & VMM_PRESENT)) kprintf("  <- NOT PRESENT (walk stops)");
            else if ((e & VMM_HUGE) && (i == 1 || i == 2))
                kprintf("  <- %s huge leaf", i == 1 ? "1GiB" : "2MiB");
            kprintf("\n");
        }
        if (r.mapped)
            kprintf("  => phys 0x%llx%s\n", (unsigned long long)r.phys,
                    r.huge ? (r.huge_level == 1 ? "  (1GiB page)" : "  (2MiB page)") : "");
        else
            kprintf("  => NOT MAPPED\n");
        return true;
    }

    if (strcmp(cmd, "embdbg") == 0 || strcmp(cmd, "monitor") == 0) {
        kernel_tui();
        return true;
    }

    if (strcmp(cmd, "interrupts") == 0 || strcmp(cmd, "irq") == 0) {
        struct irq_line_info irqs[16];
        irq_snapshot(irqs);
        /* Known ISA/PS2 line names -- the handler symbol is authoritative, but a
         * friendly device label helps; "" falls back to the symbolized handler. */
        static const char *known[16] = {
            "PIT timer", "keyboard", 0, 0, 0, 0, 0, 0,
            "RTC", 0, 0, 0, "mouse", 0, "ATA primary", "ATA secondary"
        };
        kprintf("\nIRQ  VEC  COUNT         HANDLER\n");
        for (int i = 0; i < 16; i++) {
            if (!irqs[i].handler_addr && irqs[i].count == 0) continue; /* unwired + never fired */
            kprintf("%-4d %-4d %-13llu ", i, 32 + i, (unsigned long long)irqs[i].count);
            if (irqs[i].handler_addr && ksym_ready()) {
                char sym[160];
                ksym_symbolize(irqs[i].handler_addr, sym, sizeof sym);
                kprintf("%s", sym);
            } else if (irqs[i].handler_addr) {
                kprintf("0x%llx", (unsigned long long)irqs[i].handler_addr);
            } else {
                kprintf("(unwired)");
            }
            if (known[i]) kprintf("  [%s]", known[i]);
            kprintf("\n");
        }
        /* The LAPIC timer (vector 48) is the real scheduler tick and is OFF the
         * irq_handlers[] path -- report it from its own counter. */
        kprintf("--   48   %-13llu LAPIC timer (scheduler tick)\n",
                (unsigned long long)lapic_timer_get_ticks());
        return true;
    }

    if (strcmp(cmd, "kobjects") == 0) {
        uint64_t lt = lapic_timer_get_ticks();
        kprintf("\nKernel objects\n");
        /* Timers/counters (no software-timer registry exists -- these are the
         * free-running hardware counters + the scheduler tick). */
        kprintf("  timers:\n");
        kprintf("    LAPIC tick   : %llu ticks (~%llus uptime @100Hz)\n",
                (unsigned long long)lt, (unsigned long long)(lt / 100));
        kprintf("    TSC frequency: %llu Hz\n", (unsigned long long)tsc_get_freq_hz());
        if (hpet_available())
            kprintf("    HPET counter : %llu (period %llu fs)\n",
                    (unsigned long long)hpet_read_counter(),
                    (unsigned long long)hpet_period_fs());
        else
            kprintf("    HPET         : not available\n");
        /* The one work queue (deferred vnode obj_put teardown). */
        kprintf("  work queue (kworker deferred teardown): %u/64 pending\n",
                (unsigned)kworker_pending());
        /* Capability model (compile-time fixed set; per-process bits via `caps`). */
        kprintf("  capability classes: 10 (FILESYSTEM..DEBUG) -- see `caps <pid>`\n");
        /* Wait queues have no global registry (embedded per-object); blocked
         * thread count is the observable proxy via `scheduler`. */
        kprintf("  wait queues: no global registry (per-object; see `scheduler`)\n");
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "caps")) != NULL) {
        arg = shell_skip_ws(arg);
        uint32_t pid = arg[0] ? (uint32_t)shell_parse_u64(arg, NULL)
                              : shell_default_user_pid();
        if (pid == 0) { kprintf("\n[caps] usage: caps <pid>\n"); return true; }
        struct process_detail d;
        struct thread_info one[1];
        if (process_inspect(pid, &d, one, 1) < 0) {
            kprintf("\n[caps] no such process %u\n", (unsigned)pid);
            return true;
        }
        static const char *capname[11] = {
            0, "FILESYSTEM", "NETWORK", "GPU", "AUDIO", "CAMERA",
            "USB", "SERIAL", "RAWDISK", "KERNEL_EXT", "DEBUG"
        };
        kprintf("\nCapabilities of pid %u  (cap_set 0x%llx)\n", (unsigned)pid,
                (unsigned long long)d.cap_set);
        for (int id = 1; id <= EMBK_CAP_MAX_ID; id++) {
            bool held = (d.cap_set & (1ULL << id)) != 0;
            kprintf("  %-12s cap_id %-2d  %s\n", capname[id], id,
                    held ? "GRANTED" : "-");
        }
        /* Only DEBUG is actually enforced at a syscall today (the rest are
         * represented/attenuated but not yet gated) -- honest note. */
        kprintf("(only DEBUG is syscall-enforced today; others are represented)\n");
        return true;
    }

    if (strcmp(cmd, "devices") == 0) {
        /* PCI is the master device list (every device appears here). */
        uint32_t npci = pci_devices_count();
        kprintf("\nPCI devices (%u):\n", npci);
        kprintf("  IDX  B:D.F     VEND:DEV   CLASS\n");
        for (uint32_t i = 0; i < npci; i++) {
            const struct pci_device *p = pci_get_device(i);
            if (!p) continue;
            kprintf("  %-4u %02x:%02x.%u  %04x:%04x  %s (%02x/%02x)\n", i,
                    p->bus, p->device, p->function, p->vendor_id, p->device_id,
                    pci_class_name(p->class_code), p->class_code, p->subclass);
        }
        /* Storage: the unified block layer (ATA/AHCI/USB-MSC all register here). */
        uint32_t nblk = embk_block_count();
        kprintf("Storage / block devices (%u):\n", nblk);
        for (uint32_t i = 0; i < nblk; i++) {
            struct embk_block_device *b = embk_block_get(i);
            if (!b) continue;
            uint64_t mb = (b->block_count * b->block_size) / (1024 * 1024);
            kprintf("  %-8s %llu blocks x %u B = %lluMB\n", b->name,
                    (unsigned long long)b->block_count, b->block_size,
                    (unsigned long long)mb);
        }
        /* USB host controllers. */
        uint32_t nusb = usb_controller_count();
        kprintf("USB controllers (%u):\n", nusb);
        for (uint32_t i = 0; i < nusb; i++) {
            const struct usb_controller *c = usb_get_controller(i);
            if (!c) continue;
            kprintf("  %-5s irq%u  %u ports  %u device(s)%s\n",
                    usb_kind_name(c->kind), c->irq_line, c->max_ports,
                    c->devices_present, c->initialized ? "" : "  (uninit)");
        }
        /* GPU (single active driver). */
        const struct gpu_driver *g = gpu_active();
        if (g) {
            struct gpu_mode m;
            if (g->get_mode && g->get_mode(&m))
                kprintf("GPU: %s  %ux%u\n", g->name, m.width, m.height);
            else
                kprintf("GPU: %s\n", g->name);
        } else {
            kprintf("GPU: plain VBE (no accelerated driver)\n");
        }
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "device inspect")) != NULL) {
        arg = shell_skip_ws(arg);
        uint32_t idx = (uint32_t)shell_parse_u64(arg, NULL);
        const struct pci_device *p = pci_get_device(idx);
        if (!p) { kprintf("\n[device] no PCI device at index %u\n", (unsigned)idx); return true; }
        kprintf("\nPCI device %u: %02x:%02x.%u\n", (unsigned)idx, p->bus, p->device, p->function);
        kprintf("  vendor:device : %04x:%04x\n", p->vendor_id, p->device_id);
        kprintf("  class         : %s (class %02x sub %02x prog-if %02x)\n",
                pci_class_name(p->class_code), p->class_code, p->subclass, p->prog_if);
        kprintf("  header type   : %02x\n", p->header_type);
        kprintf("  BARs:\n");
        for (uint8_t b = 0; b < 6; b++) {
            struct pci_bar bar = pci_read_bar(p->bus, p->device, p->function, b);
            if (!bar.valid) continue;
            kprintf("    BAR%u: %s 0x%llx  size %llu%s%s\n", b,
                    bar.is_mmio ? "MMIO" : "I/O ",
                    (unsigned long long)bar.address, (unsigned long long)bar.size,
                    bar.is_64bit ? " 64-bit" : "", bar.prefetchable ? " prefetch" : "");
        }
        return true;
    }

    if (strcmp(cmd, "drivers") == 0) {
        /* No formal driver registry exists in this kernel -- drivers are *_init()
         * functions. Synthesize the view from the subsystems that carry state. */
        kprintf("\nDrivers (synthesized -- no runtime registry):\n");
        kprintf("  %-10s %s\n", "pci", "bus enumeration");
        kprintf("  %-10s %u drive(s)\n", "ata", ata_drive_count());
        kprintf("  %-10s %u block device(s)\n", "block", embk_block_count());
        kprintf("  %-10s %u controller(s)\n", "usb", usb_controller_count());
        const struct gpu_driver *g = gpu_active();
        kprintf("  %-10s %s\n", "gpu", g ? g->name : "plain VBE");
        kprintf("  %-10s PS/2\n", "keyboard");
        kprintf("  %-10s PS/2\n", "mouse");
        kprintf("(use `driver inspect <name>`: ata|block|usb|gpu|pci)\n");
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "driver inspect")) != NULL) {
        arg = shell_skip_ws(arg);
        if (strncmp(arg, "ata", 3) == 0) {
            uint32_t n = ata_drive_count();
            kprintf("\nata: %u drive(s)\n", n);
            for (uint32_t i = 0; i < n; i++) {
                const struct ata_drive *d = ata_get_drive(i);
                if (!d) continue;
                kprintf("  [%u] %s  %u sectors (%lluMB)%s\n", i, d->model,
                        d->total_sectors,
                        (unsigned long long)((uint64_t)d->total_sectors * 512 / (1024 * 1024)),
                        d->is_slave ? "  slave" : "");
            }
        } else if (strncmp(arg, "block", 5) == 0) {
            uint32_t n = embk_block_count();
            kprintf("\nblock: %u device(s)\n", n);
            for (uint32_t i = 0; i < n; i++) {
                struct embk_block_device *b = embk_block_get(i);
                if (b) kprintf("  %s: %llu x %u B%s\n", b->name,
                               (unsigned long long)b->block_count, b->block_size,
                               b->flush ? "  (write-cache)" : "");
            }
        } else if (strncmp(arg, "usb", 3) == 0) {
            uint32_t n = usb_controller_count();
            kprintf("\nusb: %u controller(s)\n", n);
            for (uint32_t i = 0; i < n; i++) {
                const struct usb_controller *c = usb_get_controller(i);
                if (c) kprintf("  %s: irq%u, %u ports, %u device(s)\n",
                               usb_kind_name(c->kind), c->irq_line, c->max_ports,
                               c->devices_present);
            }
        } else if (strncmp(arg, "gpu", 3) == 0) {
            const struct gpu_driver *g = gpu_active();
            if (!g) { kprintf("\ngpu: plain VBE (no accelerated driver)\n"); return true; }
            struct gpu_mode m;
            kprintf("\ngpu: %s\n", g->name);
            if (g->get_mode && g->get_mode(&m))
                kprintf("  mode %ux%u\n", m.width, m.height);
        } else if (strncmp(arg, "pci", 3) == 0) {
            kprintf("\npci: %u device(s) enumerated (see `devices`)\n", pci_devices_count());
        } else {
            kprintf("\n[driver] unknown driver '%s' (try ata|block|usb|gpu|pci)\n", arg);
        }
        return true;
    }

    if (strcmp(cmd, "mounts") == 0) {
        struct vfs_mount_info mnts[8];
        int nm = vfs_mounts_snapshot(mnts, 8);
        kprintf("\nMOUNT POINT       FS       ROOT-INO\n");
        for (int i = 0; i < nm; i++)
            kprintf("  %-16s %-8s %llu\n", mnts[i].at, mnts[i].fs,
                    (unsigned long long)mnts[i].root_ino);
        kprintf("%d mount(s)\n", nm);
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "files")) != NULL) {
        arg = shell_skip_ws(arg);
        uint32_t pid = arg[0] ? (uint32_t)shell_parse_u64(arg, NULL)
                              : shell_default_user_pid();
        if (pid == 0) { kprintf("\n[files] usage: files <pid>\n"); return true; }
        static struct fd_snap_info fds[FD_MAX_OPEN];
        int nf = process_fds_snapshot(pid, fds, FD_MAX_OPEN);
        if (nf < 0) { kprintf("\n[files] no such process %u\n", (unsigned)pid); return true; }
        kprintf("\nOpen files of pid %u:\n", (unsigned)pid);
        kprintf("  FD  BACKING  DETAIL\n");
        for (int i = 0; i < nf; i++) {
            kprintf("  %-3d %-8s ", fds[i].fd, fd_backing_name(fds[i].backing));
            if (fds[i].backing == FD_BACKING_VNODE)
                kprintf("ino=%llu pos=%llu type=%c",
                        (unsigned long long)fds[i].ino,
                        (unsigned long long)fds[i].pos, vfs_dt_char(fds[i].vtype));
            else if (fds[i].backing == FD_BACKING_PIPE)
                kprintf("%s end", fds[i].pipe_side == 0 ? "read" : "write");
            kprintf("\n");
        }
        kprintf("%d open fd(s)\n", nf);
        return true;
    }

    if (strcmp(cmd, "vfs") == 0) {
        struct vfs_mount_info mnts[8];
        int nm = vfs_mounts_snapshot(mnts, 8);
        kprintf("\nVFS overview\n");
        kprintf("  cwd: /  (no per-process cwd in v1 -- all paths absolute)\n");
        kprintf("  mounts (%d):\n", nm);
        for (int i = 0; i < nm; i++)
            kprintf("    %-16s %s (root ino %llu)\n", mnts[i].at, mnts[i].fs,
                    (unsigned long long)mnts[i].root_ino);
        /* Total open fds across all live processes. */
        struct process_info procs[MAX_PROCESSES];
        int np = process_list(procs, MAX_PROCESSES);
        int total = 0;
        static struct fd_snap_info fds[FD_MAX_OPEN];
        for (int i = 0; i < np; i++) {
            int nf = process_fds_snapshot(procs[i].pid, fds, FD_MAX_OPEN);
            if (nf > 0) total += nf;
        }
        kprintf("  open files: %d across %d process(es)  (use `files <pid>`)\n", total, np);
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "handle inspect")) != NULL) {
        arg = shell_skip_ws(arg);
        const char *e1;
        uint32_t pid = (uint32_t)shell_parse_u64(arg, &e1);
        const char *rest = shell_skip_ws(e1);
        if (!rest[0]) { kprintf("\n[handle] usage: handle inspect <pid> <id>\n"); return true; }
        uint32_t id = (uint32_t)shell_parse_u64(rest, NULL);
        static struct obj_handle_snap objs[OBJ_HANDLE_MAX];
        int no = process_obj_handles_snapshot(pid, objs, OBJ_HANDLE_MAX);
        if (no < 0) { kprintf("\n[handle] no such process %u\n", (unsigned)pid); return true; }
        for (int i = 0; i < no; i++) {
            if ((uint32_t)objs[i].id != id) continue;
            kprintf("\nHandle %u:%u\n", (unsigned)pid, (unsigned)id);
            kprintf("  kind      : %s\n", handle_kind_name(objs[i].kind));
            kprintf("  object    : 0x%llx\n", (unsigned long long)objs[i].obj);
            if (objs[i].kind == HANDLE_KIND_SURFACE)
                kprintf("  mapping   : 0x%llx .. 0x%llx (%llu bytes)\n",
                        (unsigned long long)objs[i].map_base,
                        (unsigned long long)(objs[i].map_base + objs[i].map_bytes),
                        (unsigned long long)objs[i].map_bytes);
            kprintf("(access is structural -- by kind; no rights bitmask in this model)\n");
            return true;
        }
        kprintf("\n[handle] pid %u has no handle %u\n", (unsigned)pid, (unsigned)id);
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "handles")) != NULL) {
        arg = shell_skip_ws(arg);
        uint32_t pid = arg[0] ? (uint32_t)shell_parse_u64(arg, NULL)
                              : shell_default_user_pid();
        if (pid == 0) { kprintf("\n[handles] usage: handles <pid>\n"); return true; }
        static struct obj_handle_snap objs[OBJ_HANDLE_MAX];
        struct proc_handle_snap pids[PROC_HANDLE_MAX];
        int no = process_obj_handles_snapshot(pid, objs, OBJ_HANDLE_MAX);
        if (no < 0) { kprintf("\n[handles] no such process %u\n", (unsigned)pid); return true; }
        int npd = process_pid_handles_snapshot(pid, pids, PROC_HANDLE_MAX);
        kprintf("\nObject handles of pid %u (%d):\n", (unsigned)pid, no);
        kprintf("  ID  KIND      OBJECT              MAPPING\n");
        for (int i = 0; i < no; i++) {
            kprintf("  %-3d %-9s 0x%016llx", objs[i].id, handle_kind_name(objs[i].kind),
                    (unsigned long long)objs[i].obj);
            if (objs[i].map_bytes)
                kprintf("  0x%llx+%lluK", (unsigned long long)objs[i].map_base,
                        (unsigned long long)(objs[i].map_bytes / 1024));
            kprintf("\n");
        }
        kprintf("Pid handles (wait/kill map) (%d):", npd);
        for (int i = 0; i < npd; i++) kprintf(" [%d]->pid%u", pids[i].id, (unsigned)pids[i].pid);
        kprintf("\n");
        return true;
    }

    if (strcmp(cmd, "ipc") == 0 || strcmp(cmd, "ipc inspect") == 0) {
        bool detail = (strcmp(cmd, "ipc inspect") == 0);
        kprintf("\nIPC objects (live counts):\n");
        kprintf("  channels : %u\n", channel_live_count());
        kprintf("  surfaces : %u  (shared memory)\n", surface_live_count());
        kprintf("  pipes    : %u\n", pipe_live_count());
        kprintf("  endpoints: %u\n", endpoint_live_count());
        kprintf("  (semaphores/mutexes: embedded by value, no object identity; events: n/a)\n");
        /* Published endpoints with their /run names (epfs registry). */
        struct epfs_ep_info eps[16];
        int ne = epfs_endpoints_snapshot(eps, 16);
        if (ne > 0) {
            kprintf("Published endpoints:\n");
            for (int i = 0; i < ne; i++)
                kprintf("  /run/%-14s -> endpoint 0x%llx\n", eps[i].name,
                        (unsigned long long)eps[i].endpoint);
        }
        /* Per-object handle enumeration across all processes, deduped by obj. */
        static struct ipc_handle_snap ih[256];
        int n = ipc_handles_snapshot(ih, 256);
        kprintf("Live objects held via handles (deduped):\n");
        kprintf("  KIND      OBJECT              HOLDERS\n");
        for (int i = 0; i < n; i++) {
            /* skip if this obj already printed (an earlier holder). */
            bool seen = false;
            for (int j = 0; j < i; j++)
                if (ih[j].obj == ih[i].obj) { seen = true; break; }
            if (seen) continue;
            kprintf("  %-9s 0x%016llx  pid%u", handle_kind_name(ih[i].kind),
                    (unsigned long long)ih[i].obj, (unsigned)ih[i].owner_pid);
            /* list the other holder pids of the same object */
            for (int j = i + 1; j < n; j++)
                if (ih[j].obj == ih[i].obj) kprintf(",pid%u", (unsigned)ih[j].owner_pid);
            if (detail && ih[i].map_bytes)
                kprintf("  [mapped 0x%llx+%lluK]", (unsigned long long)ih[i].map_base,
                        (unsigned long long)(ih[i].map_bytes / 1024));
            kprintf("\n");
        }
        if (n == 0) kprintf("  (none)\n");
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "kill")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[kill] usage: kill <pid>\n");
            return true;
        }
        uint32_t pid = shell_parse_uint(arg);
        process_kill(pid);
        kprintf("\n[kill] sent to pid %u\n", (unsigned int)pid);
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "wait")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[wait] usage: wait <pid>\n");
            return true;
        }
        uint32_t pid = shell_parse_uint(arg);
        kprintf("\n[wait] blocking for pid %u...\n", (unsigned int)pid);
        int code = process_wait(pid);
        if (code < 0) {
            kprintf("[wait] pid %u: %s\n", (unsigned int)pid, embk_strerror(code));
        } else {
            kprintf("[wait] pid %u exited with code %d\n", (unsigned int)pid, code);
        }
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "nice")) != NULL) {
        uint32_t pid = shell_parse_uint(arg);
        const char *prio_arg = arg;
        while (*prio_arg && *prio_arg != ' ') {
            prio_arg++;
        }
        while (*prio_arg == ' ') {
            prio_arg++;
        }
        if (!arg[0] || !prio_arg[0]) {
            kprintf("\n[nice] usage: nice <pid> <priority 0-3, 0=highest>\n");
            return true;
        }
        uint32_t prio = shell_parse_uint(prio_arg);
        int rc = process_set_priority(pid, (uint8_t)prio);
        if (rc != 0) {
            kprintf("\n[nice] pid %u: %s\n", (unsigned int)pid, embk_strerror(rc));
        } else {
            kprintf("\n[nice] pid %u priority set to %u\n", (unsigned int)pid, (unsigned int)prio);
        }
        return true;
    }

    return false;
}

/* `snap create|list|delete|rollback <name>` -- the shell surface for v2.2
 * Phase 5b's snapshots. Own dispatcher (not folded into
 * shell_handle_process_command) since it's a distinct feature area, but
 * follows the exact same shell_match_prefix() pattern. */
static bool shell_handle_snapshot_command(const char *cmd)
{
    const char *rest = shell_match_prefix(cmd, "snap");
    if (!rest) return false;

    struct embkfs_volume *vol = embkfs_live_volume();
    if (!vol) {
        kprintf("\n[snap] no EMBKFS volume mounted\n");
        return true;
    }

    const char *arg;
    if ((arg = shell_match_prefix(rest, "create")) != NULL) {
        if (!arg[0]) { kprintf("\n[snap] usage: snap create <name>\n"); return true; }
        int rc = embkfs_snapshot_create(vol, arg);
        if (rc != EMBK_OK) kprintf("\n[snap] create '%s' failed: %s\n", arg, embk_strerror(rc));
        else kprintf("\n[snap] created '%s'\n", arg);
        return true;
    }
    if ((arg = shell_match_prefix(rest, "delete")) != NULL) {
        if (!arg[0]) { kprintf("\n[snap] usage: snap delete <name>\n"); return true; }
        int rc = embkfs_snapshot_delete(vol, arg);
        if (rc != EMBK_OK) kprintf("\n[snap] delete '%s' failed: %s\n", arg, embk_strerror(rc));
        else kprintf("\n[snap] deleted '%s'\n", arg);
        return true;
    }
    if ((arg = shell_match_prefix(rest, "rollback")) != NULL) {
        if (!arg[0]) { kprintf("\n[snap] usage: snap rollback <name>\n"); return true; }
        int rc = embkfs_snapshot_rollback(vol, arg);
        if (rc != EMBK_OK) kprintf("\n[snap] rollback '%s' failed: %s\n", arg, embk_strerror(rc));
        else kprintf("\n[snap] rolled back to '%s'\n", arg);
        return true;
    }
    if (strcmp(rest, "list") == 0) {
        struct embk_snapshot_item items[EMBKFS_MAX_SNAPSHOTS];
        uint32_t n = 0;
        embkfs_snapshot_list(vol, items, EMBKFS_MAX_SNAPSHOTS, &n);
        kprintf("\n[snap] %u snapshot%s:\n", (unsigned int)n, n == 1 ? "" : "s");
        for (uint32_t i = 0; i < n; i++) {
            char name[33];
            memcpy(name, items[i].name, 32);
            name[32] = '\0';
            kprintf("  %-31s gen %-6lu %lu ns\n", name,
                    (unsigned long)items[i].generation, (unsigned long)items[i].timestamp);
        }
        return true;
    }

    kprintf("\n[snap] usage: snap create|list|delete|rollback <name>\n");
    return true;
}

/* `stat <path>` -- the ls-adjacent surface for v2.2 Phase 5c's process-
 * provenance tracking (writer_pid), alongside the timestamps Phase 0
 * already stores. Root-relative path only (matches ls's own convention
 * elsewhere in this shell). */
static bool shell_handle_stat_command(const char *cmd)
{
    const char *arg = shell_match_prefix(cmd, "stat");
    if (!arg) return false;

    struct embkfs_volume *vol = embkfs_live_volume();
    if (!vol) { kprintf("\n[stat] no EMBKFS volume mounted\n"); return true; }
    if (!arg[0]) { kprintf("\n[stat] usage: stat <path>\n"); return true; }

    uint64_t oid = 0;
    int rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, arg, &oid);
    if (rc != EMBK_OK) { kprintf("\n[stat] %s: %s\n", arg, embk_strerror(rc)); return true; }

    struct embk_inode_item ino;
    rc = embkfs_stat_object(vol, oid, &ino);
    if (rc != EMBK_OK) { kprintf("\n[stat] %s: %s\n", arg, embk_strerror(rc)); return true; }

    kprintf("\n[stat] %s (object %lu)\n", arg, oid);
    kprintf("  size %lu  blocks %lu  links %lu  mode 0x%x\n",
            (unsigned long)ino.size, (unsigned long)ino.blocks, (unsigned long)ino.links,
            (unsigned int)ino.mode);
    kprintf("  atime %lu  mtime %lu  ctime %lu  btime %lu  (ns since epoch)\n",
            (unsigned long)ino.atime, (unsigned long)ino.mtime,
            (unsigned long)ino.ctime, (unsigned long)ino.btime);
    uint32_t wpid = embk_inode_writer_pid(&ino);
    if (wpid) kprintf("  writer_pid %u\n", (unsigned int)wpid);
    else      kprintf("  writer_pid unknown (predates process-provenance, or written by mkfs)\n");
    return true;
}

static void kernel_handle_line_command(const char *cmd)
{
    if (shell_handle_process_command(cmd))
        return;

    if (shell_handle_snapshot_command(cmd))
        return;

    if (shell_handle_stat_command(cmd))
        return;

    if (selftests_handle_command(cmd))
        return;

    if (cmd[0])
        kprintf("\n[cmd] unknown command: %s\n", cmd);
}

void kernel_main(uint64_t bp_phys) {   /* bp_phys: the boot-protocol record
                                        * pointer the loader left in RDI, relayed
                                        * by kentry.asm as the first argument. */
    // --- Core init ---
     serial_init();
    boot_protocol_capture(bp_phys);   /* FIRST -- pmm_init reads through it */
    boot_protocol_dump();

    pmm_init();
    
    gdt_init_bsp();   // this_cpu()/percpu_init_topology() aren't usable yet
                      // (need ACPI+LAPIC below) -- operates on cpu_table[0]
                      // (the BSP) directly, see gdt_init_bsp()'s comment.
    idt_init();
    syscall_init();   // install int 0x80 (DPL3) + #DF on IST1; needs idt_init first
    pic_init();
    irq_install();
    fpu_init_this_cpu();   // CR0/CR4 are per-core -- every AP repeats this in
                            // ap_main() (smp.c). Must land before this core's
                            // first kernel_ctx_switch() ever runs an FXSAVE/
                            // FXRSTOR (process.c's schedule_locked()).

    // --- Memory ---
    pmm_init();
    vmm_init();
    kheap_init();
    ap_bootstrap_map();   // permanent low-1MB identity map, needed before
                          // any AP can be started (see smp.h's comment)

    // --- Interrupt controllers (ACPI -> LAPIC -> IO-APIC) ---
    acpi_init();
    lapic_init();
    // this_cpu() becomes usable core-wide from here on: needs both ACPI's
    // MADT CPU list (acpi_init, just above) and a working lapic_get_id()
    // (lapic_init, just above) to identify which cpu_table[] entry is "us".
    percpu_init_topology();
    process_init();       // MUST run before smp_bringup(): each AP's
                          // ap_main() adopts a PCB out of proc_table
                          // (process_adopt_current()) as part of its own
                          // bring-up. process_init() blanket-resets every
                          // slot to PROCESS_UNUSED -- running it AFTER the
                          // APs are up (as an earlier version did, from
                          // the old single-core spot right before the
                          // shell's own adoption below) silently WIPED the
                          // APs' already-adopted idle PCBs: each AP's
                          // current_process kept pointing at a slot the
                          // allocator now considered free and happily
                          // recycled into test kthreads -- two cores
                          // executing "the same" PCB, the root of a whole
                          // family of intermittent SMP corruption.
    smp_bringup();        // start every other core found in the MADT;
                          // each one parks in its own idle loop (ap_main,
                          // smp.c) as a real, adopted scheduler
                          // participant with its own LAPIC timer
    ioapic_init();

    // --- HPET: must come after acpi_init + vmm (for MMIO map) ---
    hpet_init();

    // --- RTC: port I/O only, no MMIO dependency -- the one source of
    // real calendar time this kernel has (EMBKFS inode timestamps and
    // everything downstream of them read this, not LAPIC/HPET/PIT, which
    // only ever count elapsed ticks, not wall-clock date/time). ---
    kprintf("\n=== RTC init ===\n");
    kprintf("RTC: current time (Unix epoch seconds) = %lu\n",
            (unsigned long)rtc_now_unix());

    // --- Devices ---
    
    pci_init();
    ac97_init();   // sound out; harmless when no AC97 device is attached
    usb_init();
    ata_init();    // registers ATA drives as block devices internally
    ahci_init();   // runs IDENTIFY per port, stores sector counts

    // Register AHCI drives as block devices (after ahci_init filled sector counts)
    ahci_register_block_devices();



    // --- Display + input ---
    gpu_init();   // pick VirtIO-GPU / Bochs DISPI before the fb comes up
    fb_init();
    console_init();
    keyboard_init();
    ioapic_route_isa(1, 33, 0);   // keyboard: ISA IRQ 1 -> vector 33 -> CPU 0 (override-aware)
    // Clamp the cursor to the ACTUAL screen size (varies by GPU: virtio-gpu is
    // 1280x800, stdvga 1024x768) so it can reach every corner of the desktop.
    {
        const fb_info_t *fbi = fb_get_info();
        mouse_init(fbi ? (int)fbi->width : 1024, fbi ? (int)fbi->height : 768);
    }
    ioapic_route_isa(12, 44, 0);  // mouse: ISA IRQ 12 -> vector 44 -> CPU 0 (override-aware)

    // --- TSC calibration (uses HPET if available, else PIT fallback) ---
    // MUST precede lapic_timer_init(): if the CPU supports TSC-deadline mode the
    // timer arms an absolute TSC deadline and needs the calibrated frequency
    // here. Only depends on HPET/PIT (both up by now), not on the LAPIC timer.
    tsc_calibrate();

    // --- Timer (LAPIC) + retire PIC ---
    lapic_timer_init(48);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    __asm__ volatile ("sti");

    // --- Boot splash ---
    boot_animation();
    console_clear();

// ============================================================
    //  Block device enumeration
    // ============================================================
    // Probe each whole disk for an MBR and expose its partitions (sda1, sda2,
    // ...) as block devices. Must run after `sti` above: the ATA read path is
    // IRQ-driven and would hang waiting on an interrupt that can't fire yet.
    // Done before enumeration/mount so partitions appear in the listing and the
    // mount probe below sees them alongside whole disks.
    embk_partition_scan_all();

    kprintf("\n=== Block devices ===\n");
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *dev = embk_block_get(i);
        kprintf("  %s: %u blocks (%u KB)\n",
                dev->name,
                (unsigned int)dev->block_count,
                (unsigned int)((dev->block_count * dev->block_size) / 1024));
    }

    // ============================================================
    //  Mount FAT32 (probe every disk, mount the first valid one)
    // ============================================================
    embkfs_init();
    static struct fat32_volume vol;
    bool found = false;
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *d = embk_block_get(i);
        if (fat32_mount(d, &vol) == EMBK_OK) {
            found = true;
            break;
        }
    }
    selftests_init(&vol, found);
    if (!found) {
        kprintf("No FAT32 volume found on any disk\n");
    }

    vfs_init();

    // EmbLink UI Piece 1, Layer B: the RAM-backed endpoint filesystem, mounted
    // at /run independent of whatever real storage was found above -- IPC
    // rendezvous (chan_listen/connect) shouldn't depend on a disk existing.
    epfs_init();
    {
        int rc = epfs_vfs_register("/run");
        if (rc != EMBK_OK) {
            kprintf("VFS: epfs register at /run failed: %s\n", embk_strerror(rc));
        }
    }

    bool vfs_ready = false;
    struct embkfs_volume *embk_live = embkfs_live_volume();

    if (embk_live) {
        int rc = embkfs_vfs_register("/", embk_live);
        if (rc != EMBK_OK) {
            kprintf("VFS: EMBKFS register failed: %s\n", embk_strerror(rc));
        } else {
            vfs_ready = true;
        }
    }

    // v2.2 (Phase 1): register every EMBKFS volume BEYOND the primary
    // (embkfs_init() now mounts up to EMBKFS_MAX_VOLUMES, not just one --
    // see that function's comment) at its own mount point, one per
    // underlying block device name. Index 0 is embk_live (already
    // registered at "/" above); this only runs for index 1+, so it's a
    // complete no-op on a machine with exactly one EMBKFS volume, which
    // is every machine before this phase and most after it.
    for (uint32_t vi = 1; vi < embkfs_volume_count(); vi++) {
        struct embkfs_volume *extra = embkfs_volume_at(vi);
        if (!extra) continue;
        char mp[64] = "/";
        strcat(mp, extra->dev->name);   // e.g. "/sdb" -- device names are
                                         // short (BLOCK_NAME_LEN) and this
                                         // buffer has ample headroom
        int rc = embkfs_vfs_register(mp, extra);
        if (rc != EMBK_OK) {
            kprintf("VFS: EMBKFS register at %s failed: %s\n", mp, embk_strerror(rc));
        } else {
            kprintf("VFS: EMBKFS volume %s mounted at %s\n", extra->dev->name, mp);
        }
    }

    if (found) {
        const char *fat_mp = vfs_ready ? "/fat32" : "/";
        int rc = fat32_vfs_register(fat_mp, &vol);
        if (rc != EMBK_OK) {
            kprintf("VFS: FAT32 register at %s failed: %s\n", fat_mp, embk_strerror(rc));
        } else {
            vfs_ready = true;
        }
    }

    if (!vfs_ready)
        kprintf("VFS: no filesystem mounted\n");

    selftests_set_vfs_ready(vfs_ready);
    if (vfs_ready)
        vfs_fd_init();

    /* Load the kernel's own .embdbg so a panic's isr_handler dump names
     * func:line instead of a bare hex RIP (EMBDBG_Specification.md §7). The
     * sidecar sits at /system/kernel.embdbg (mkfs stages build/kernel.embdbg
     * there). Best-effort: a missing/short file just leaves the symbolizer
     * off and the dump falls back to hex. */
    if (vfs_ready) {
        struct vfs_stat kst;
        if (vfs_stat("/system/kernel.embdbg", &kst) == 0 && kst.size > 64) {
            int fd = vfs_open("/system/kernel.embdbg", O_RDONLY, 0);
            if (fd >= 0) {
                uint32_t sz = (uint32_t)kst.size;
                void *buf = kmalloc(sz);
                if (buf) {
                    size_t total = 0, got = 0;
                    while (total < sz &&
                           vfs_fd_read(fd, (char *)buf + total, sz - total, &got) == EMBK_OK &&
                           got > 0)
                        total += got;
                    if (total == sz && ksym_load(buf, sz) == 0)
                        kprintf("ksym: kernel panic symbols loaded (%u bytes)\n", sz);
                    else
                        kprintf("ksym: /system/kernel.embdbg present but unusable\n");
                }
                vfs_close(fd);
            }
        }
    }

    vfs_ls("/");

    // Turn THIS execution context -- the interactive shell below -- into a
    // real, schedulable process (docs/architecture/process-and-scheduling.md
    // §17/process_adopt_current()'s comment), instead of the old design
    // where main.c auto-launched exactly one hardcoded process
    // (process_start_first(), a ONE-WAY hand-off that never returned here
    // at all). Now the shell is just another round-robin participant:
    // `run <path>` spawns real children as siblings, preemption keeps
    // giving the shell its own turns back, and `ps`/`kill`/`wait` manage
    // whatever's running, all without the shell itself ever exiting.
    // (process_init() itself already ran much earlier -- BEFORE
    // smp_bringup(), see the comment there for why that ordering is
    // load-bearing.)
    if (!process_adopt_current()) {
        kprintf("\nfailed to start the process subsystem -- run/ps/kill/wait unavailable\n");
    }

    // One dedicated idle kthread per core, pinned to it, at the lowest
    // priority band: the guaranteed always-available switch target that
    // lets ANY core immediately get off a dying or blocking process's
    // stack even when nothing else is runnable -- see
    // process_create_idle_for_cpu()'s comment (process.c) for the
    // liveness argument. Created only now (not at smp_bringup time)
    // because kthread creation needs the full VMM/heap up for stacks.
    for (uint32_t ci = 0; ci < cpu_count; ci++) {
        if (!process_create_idle_for_cpu(ci)) {
            kprintf("warning: no idle kthread for cpu %u\n", (unsigned int)ci);
        }
    }

    kworker_init();

    // --- Networking (M1): virtio-net + Ethernet/ARP/IPv4/ICMP. After PCI is
    // enumerated and the scheduler is live (net_init spawns an RX poll kthread).
    // Static config for now; `test net` pings the gateway. No ring-3 surface yet.
    net_init();

    // Enter userspace through INIT: the kernel spawns exactly ONE user process,
    // /system/bin/init.elf, the root of userspace authority (docs/USERSPACE_v2.md
    // "authority IS the namespace", UP1). init in turn brings up the graphical
    // desktop session (home.elf) and SUPERVISES it -- so the desktop is init's
    // child, not the first process. init runs as an ordinary round-robin sibling
    // of this shell context; the loop below keeps pumping the compositor pointer
    // and USB, and the serial/keyboard REPL stays available as a debug console.
    {
        char *iargv[] = { (char *)"/system/bin/init.elf", NULL };
        /* The desktop is about to own the framebuffer -- disable the text
         * console's on-screen half BEFORE userspace becomes schedulable.
         * Ordering matters: process_create() makes init RUNNABLE, and userspace
         * stdout/stderr (fd 1/2) now route through console_putchar. If the fb
         * were still enabled here, a timer preemption in the gap between spawn
         * and this call could let the desktop's first write() paint over the
         * boot screen. Disabling first closes that window. All kernel logging +
         * the serial debug console below stay on COM1 and never touch the fb. */
        console_set_fb_enabled(false);
        int ipid = process_create("/system/bin/init.elf", iargv, 1, NULL, 0);
        if (ipid < 0)
            kprintf("\ninit: failed to launch /system/bin/init.elf: %s\n", embk_strerror(ipid));
        else
            kprintf("\ninit: launched /system/bin/init.elf as pid %d\n", ipid);
    }

    // Main loop (boot CPU): pump the polled drivers (legacy USB + the window
    // compositor's pointer) and service a serial-only kernel debug console.
    // The interactive keyboard + screen belong to userspace.
    uint64_t last = 0;
    char cmd_buf[128];
    uint32_t cmd_len = 0;
    for (;;) {
        uint64_t now = lapic_timer_get_ticks();
        if (now >= last + 500) { last = now; }
        // Legacy USB HCs (UHCI/OHCI/EHCI) are polled: drain any completed
        // interrupt-IN transfers and re-arm them. xHCI input is IRQ-driven.
        usb_poll();
        // Drive the window compositor's pointer: cursor, click-to-focus, and
        // title-bar drag. Runs here (schedulable shell-process context) so the
        // compositor spinlock is never taken from an IRQ handler. No-op until a
        // window exists.
        compositor_pointer_tick();
        // Advance any window open/minimize motion. Same reasoning as above: it
        // repaints, so it must run in schedulable context, not an IRQ.
        compositor_anim_tick();
        // Kernel DEBUG CONSOLE over SERIAL (COM1). The keyboard + screen belong
        // to userspace now (the launcher, and the shell that reads fd 0); the
        // kernel keeps a SERIAL-ONLY console so selftests + state inspection stay
        // reachable when userspace is wedged -- and, crucially, it never touches
        // the keyboard buffer a userspace reader owns, so there's no contention.
        // Polled (the UART RX IRQ is left disabled); the ~100Hz timer wakes the
        // hlt below, giving debug-console-grade latency. Drain the FIFO per pass.
        while (serial_has_char()) {
            char c = serial_read_char();
            if (c == '\r' || c == '\n') {
                serial_write_char('\n');
                cmd_buf[cmd_len] = '\0';
                kernel_handle_line_command(cmd_buf);
                cmd_len = 0;
            } else if ((c == '\b' || c == 127) && cmd_len > 0) {
                cmd_len--;
                serial_write_string("\b \b");   // erase the char on the terminal
            } else if (c >= 32 && c <= 126) {
                if (cmd_len + 1 < sizeof cmd_buf) {
                    cmd_buf[cmd_len++] = c;
                    serial_write_char(c);        // echo so the remote terminal shows input
                }
            }
        }

        __asm__ volatile ("hlt");   // wake on any IRQ (timer, PS/2, or xHCI)
    }
}