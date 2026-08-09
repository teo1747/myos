# EmbLink OS — Known Issues & Improvements

Open items only, grouped by subsystem. Completed work lives in
PROJECT_STATUS.md (and git history). Keep this file to what's actually
left to do.

---

## The Applications launcher

- [x] Opens every time, in FRONT of app windows, full-screen and searchable
      with 72px icons.
- [x] ~~Re-opening it painted nothing~~ -- the cause was in the LAYOUT ARENA,
      not the launcher, the compositor or the window order:
      `layout_destroy_node` freed a node without unlinking it from its parent's
      child list. The parent kept pointing at a slot on the free list, and the
      moment that slot was reissued with a bumped generation the parent's link
      resolved to nothing and the child walk STOPPED THERE. Every sibling after
      the destroyed node quietly left the tree -- still built, still linked in
      the instance tree, never measured, never arranged. It affected ANY
      subtree that is removed and later rebuilt; the launcher is just where it
      was noticed.
- [ ] The menu bar is covered while the launcher is up rather than staying
      above it. Lifting translucent bars above the layer is the obvious move
      and needs care: the bar's window GROWS to 792x340 for its dropdowns, so
      naively it covers the launcher instead.

## Note++ -- the OS's own editor

Working and verified on the metal: multi-file tabs, line numbers, syntax
colouring for C/Python/JS/shell/Markdown, auto-indent, current-line highlight,
undo, select-all, CLICK TO PLACE THE CARET and DRAG TO SELECT, wheel scrolling,
find/replace, the shortcut set (Ctrl+S/N/O/W/A/Z/Y/F/H/D/L, Ctrl+arrows), and a
status bar tracking line, column, selection size and language. The engine
(user/note/edit.c) has 45 host tests; `make edit-test`.

Honest score from the person using it: 0.1/10 -- a base, not an editor. What is
missing, roughly in the order a user meets it:

- [x] ~~No horizontal scrolling~~ -- a long line is WINDOWED now rather than
      clipped, and the caret drags the window sideways with a few columns of
      margin ahead of it. The 512-byte draw cap is gone with it: only the
      visible columns are ever emitted, so the limit that remains is the
      highlighter's (4096 per line, past which a line is uncoloured but still
      whole, editable and saved).
- [ ] Still no horizontal SCROLLBAR, and no way to scroll sideways with the
      wheel or a gesture -- only the caret moves the window.
- [ ] No word wrap.
- [x] ~~Find shows no match count~~ -- it says "3 of 12", or "none".
- [ ] Find still does not HIGHLIGHT the other matches, only the current one.
- [ ] Ctrl+G (go to line) opens the find box and asks for a number instead of
      being its own thing.
- [x] ~~No auto-close, no comment toggle~~ -- an opener brings its partner and
      typing the closer steps over it; Ctrl+/ comments a block and uncomments
      it only when every line already is.
- [x] ~~No prompt when closing a modified file~~ -- Cancel / Save / Discard.
- [ ] 🐛 That confirm sheet draws in the TOP-LEFT instead of centred, over the
      tab bar. Dialog inside Window() is not being centred by its overlay; it
      works and it looks broken.
- [x] ~~No double-click for a word, no triple-click for a line~~ -- both in,
      counted by time AND place so a slow second click elsewhere is a new
      selection. (Not confirmed on hardware: QMP cannot click fast enough to
      make a double-click; the engine's half is covered by tests.)
- [ ] Still no shift-click to extend a selection.
- [ ] No scrollbar; the only indication of position is the line number.
- [ ] Undo cannot redo an insert (the pool stores removed text only), and undo
      is per-document only while that document is bound.
- [ ] No column selection, no multiple carets, no folding, no minimap.
- [ ] The tab bar does not scroll and caps at 8 documents.
- [ ] Nothing is remembered between runs: no session, no recent files, no
      per-file cursor position.

## The JavaScript gap -- it is the PLATFORM, not the engine

QuickJS (ES2020) is already complete; nothing is left to port. What real pages
die on is the web platform around it, and nobody ships that as a portable
library -- every browser's DOM is welded to its own engine, so V8 or
SpiderMonkey would arrive with no more of it than we have. Ranked by what the
corpus actually throws:

- [ ] `window` is not defined. The global alias nearly every script touches in
      its first ten lines; jsdom.c exposes `document`, `console`, `fetch`,
      `location`, `localStorage` and element-level `addEventListener` but not
      this. Close to a one-liner, and it unblocks scripts that currently die
      before doing anything.
- [ ] `URL` is not defined.
- [ ] `addEventListener` at global scope (it exists on elements only).
- [ ] Considered and rejected: porting LibCSS (NetSurf). It is genuinely
      portable and does parsing/selection/cascade well, but it stops at
      COMPUTED STYLE -- and our measured gap is properties that need PAINTING
      (opacity, box-shadow, transform), which it would hand us and not draw.
      It is also roughly CSS 2.1 + Selectors 3, so it would not have given us
      `:is`/`:where`/`:not` either, which took ~150 lines to write.

## What still makes a page look older than it is

Measured against the CSS of the seventeen real sites, ranked by how often the
web asks for them and how much they change a page:

- [ ] `opacity` (900 uses) and `box-shadow` (886). Shadow is most of what
      reads as "depth" on a modern page; without it every card is a flat
      rectangle.
- [ ] `transform` (782). Used for layout offsets as often as for effects.
- [ ] SVG is parsed but not DRAWN, so every logo and icon on a modern site is
      alt text or nothing. rust-lang.org's header is the word "Rust Logo".
- [ ] No `@font-face` (80 uses): every page renders in DejaVu Sans whatever it
      asked for, which alone dates a design.
- [ ] `background-image` (465), including gradients.
- [ ] `z-index` (629) -- painting is still document order.
- [ ] `white-space` (456), `min-width`/`min-height`/`max-height` (~1100).
- [ ] `margin-right` has nowhere to go: the box model carries
      `margin_top/bottom` and a left `indent` only.

## Browser chrome -- what is still missing

- [x] ~~The address field shows the whole URL in one weight~~ -- the host is
      emphasised and the scheme and path recede, via a one-shot span the kit's
      field draws when it is not being edited. Plain again while editing: what
      you are editing is the whole string.
- [x] ~~No favicon anywhere~~ -- one per ORIGIN, cached across navigation,
      downsampled to 32px and drawn at 14. Needed an .ico decoder, because
      that is what the web actually serves: of nine sites checked, six answer
      /favicon.ico with an .ico container holding DIBs at 32, 8 and 4 bits per
      pixel, or a whole PNG.
- [ ] A site whose ONLY icon is an SVG shows none. Both candidates are tried
      (declared link, then /favicon.ico) and an SVG declaration is skipped
      rather than fetched-and-failed, so this only bites a site with no .ico
      at all.
- [ ] The search engine is compiled in (`SEARCH_URL` in vellum.c). It should be
      a setting. Google is not the default and the reason is measured, not
      chosen: it answers a scriptless browser with a 302 to a consent page, and
      DuckDuckGo with a CAPTCHA. Mojeek answers with results.
- [ ] The address bar has no completion -- no history matching, no suggestions.
      Every visit is fully typed.
- [x] ~~Large pages render blank on the metal~~ -- root cause found and fixed,
      and it was not rendering at all: the kernel's blocking TCP receive
      returned 0 on TIMEOUT, which every layer above reads as end-of-stream.
      A Brave results page arrived as 8192 of 291486 bytes with status 200 and
      no error anywhere. Now 291486/291486. See net_tcp_recv.
- [ ] A large page over HTTPS is SLOW: ~90-200s for 291KB under TCG. Now that
      the transfer completes instead of being cut short, throughput is the next
      wall. Unmeasured which of these dominates -- our AES-GCM decrypt cost, the
      TCP receive window, or the 4KB read chunk in net.c. Measure before
      theorising.
- [ ] Wikipedia's "Operating system" article is 666KB and TAB_SRC_MAX is 512KB,
      so it is truncated before the parser sees it regardless of the above.
- [ ] The status line still reports node and rule counts -- developer
      telemetry as a permanent resting state. Useful here, but not what a
      reader wants the bottom of the window to say.
- [ ] Tabs do not scroll or shrink: six is the cap and the strip would simply
      run out of room before that on a narrow window.
- [ ] No context menu, no bookmarks, no downloads UI.

## Bootloader

### Stage 1
- [x] Hardcoded Stage 2 sector count (8 sectors) — should be dynamic.

### Stage 2
- [x] Loads a fixed 512 sectors for the kernel regardless of actual size.
  - LESSON (learned the hard way): when the kernel outgrew the old 90-sector
    limit, the tail wasn't loaded. Symptoms were truncated MMIO addresses and
    garbled E820 — looked like a pointer bug, was actually missing code/data
    in RAM. The Makefile now warns if the kernel ELF exceeds 512 sectors.
  - PROPER FIX (bootloader v2): read the ELF header in real mode, parse the
    PT_LOAD segments (max p_offset + p_filesz), load exactly the sectors
    needed. Do this alongside the UEFI rework.
- [ ] Hardcoded kernel LBA start (sector 9) — works only because we control
  the disk layout. A real OS finds the kernel via a filesystem.
- [ ] Disk image must be padded (truncate) — workaround for reading past the
  actual kernel data. Goes away with ELF-aware loading.

### General
- [x] ~~No USB/CD boot — hard disk only.~~ — **USB / single-disk boot shipped.**
  `tools/mkbootdisk.sh` lays the kernel + an EMBKFS partition on ONE partitioned
  medium; `dd usb.img → /dev/sdX` boots the whole system off that stick (or disk).
  `make run-usb` / `run-usb-ide`. The kernel reaches it via the xHCI mass-storage
  block path (fixed the ATA `nIEN` clear so the completion IRQ fires). **CD /
  El Torito is still open** (ISO9660 + 2048-byte sectors + boot catalog).
- [x] ~~No error recovery on failed disk reads (just halts).~~ — stage1/stage2 now
  reset + retry (3×) a failed INT 13h read before giving up (flaky-USB spin-up).
- [x] ~~BIOS only — no UEFI support.~~ — **from-scratch UEFI loader** (`boot/uefi/`,
  own PE32+ EFI app, no GNU-EFI) boots to the desktop under OVMF; both firmwares
  hand the kernel the same `boot_protocol`. `make run-uefi` / `run-uefi-cow`.
- [ ] **EmbBoot** (UEFI boot manager, [EMBBOOT_Design.md](EMBBOOT_Design.md)):
  M1 (menu) done. Open: M2 `.embfw` payload format + kernel-on-ESP; M3 verify
  (HMAC v1 → Ed25519 v2); M4 Recovery + Diagnostics; M5 Boot Manager + self-update.
- [ ] Hardcoded kernel LBA start (sector 9) still stands for the BIOS path — a
  real filesystem-aware BIOS backend is EmbBoot §11 (stage1.5 / 64-bit core).

---

## Memory Management

### PMM
- [x] ~~Linear scan for a free page is O(n) — slow under heavy allocation~~ —
  **fixed, and measured.** Two small changes rather than the buddy allocator
  this entry reached for: a **rotating hint** (resume where the last success
  left off, wrap once — still exact, it cannot report ENOMEM while a free page
  exists) and **byte-at-a-time skipping** (a `0xFF` byte is eight used pages,
  so one test replaces eight). `pmm_free_page` also aims the hint at the page
  it just freed, so free-then-allocate costs one test.
  Controlled A/B via `test pmm`, same build, hint disabled vs enabled:

  | | hint off (old) | hint on | |
  |---|---:|---:|---|
  | scan tests, 2048 allocations | 2,289,920 | 2,048 | |
  | **tests per allocation** | **1118** | **1** | **1118× fewer** |

  The old cost grew with how much memory was already used — low memory fills
  first and stays full, so every allocation re-walked it. Note the "old" column
  already includes byte-skipping; the true original (bit-at-a-time from page 0)
  was ~8× worse again.
  - [ ] A **buddy allocator** is still the eventual answer *if contiguous
    multi-page allocation is ever needed* — that is what it buys that this does
    not. Nothing asks for it yet, so it is not built speculatively.
- [x] ~~Stack region hardcoded at 0x200000~~ — the boot stack is a 128 KiB
  `resb` in the kernel's own `.bss` (`kentry.asm`), so it moves with the kernel
  and there is no hardcoded address left.
- [x] ~~Stack and PMM bitmap could collide if the stack grows > 1MB~~ —
  structurally impossible for the same reason: the stack is inside the kernel
  image, below `kernel_end`, and the bitmap sits *at* `kernel_end`. They cannot
  overlap without the linker placing `.bss` on top of itself.
  - [ ] Still absent: **guard pages** on the boot stack. A deep recursion would
    run off the bottom into whatever `.bss` precedes it rather than faulting.
    Bounded (the kernel's call depth is shallow and known), but real.

### VMM — kernel mapping extent (NEW — today's near-miss)
- [x] ~~vmm_init maps only the first 2MB of the kernel into the kernel range~~ —
  done, and the near-miss it warned about did arrive: the kernel is now ~1076
  sectors (550 KB of image, ~4 MB total with `.bss`), well past 2 MB. `vmm_init`
  maps `[0 .. max(kernel_end, pmm_reserved_phys_end()))`, rounded up — the
  bitmap sits *at* `kernel_end` and is written before the remap, so the mapping
  has to cover it too or the first bitmap access after the CR3 switch faults.
  Visible at boot: `Mapping Kernel at 0xFFFFFFFF80000000 up to phys: 0x467000`.

### VMM — page-table location limit (measured)
- [x] Boots fine at 4 GB. Page tables (~28 KB) + bitmap (160 KB) fit under
  physical 2 MB, so KP2V works.
- [ ] HARD LIMIT ~32 GB RAM: the PMM bitmap alone (~1 MB at 32 GB) plus page
  tables pushes allocations past physical 2 MB, where KP2V (kernel mapping is
  0–2 MB only) faults. 128 GB → ~4 MB bitmap → guaranteed crash.
  - FIX (when needed): two-phase paging bootstrap.
    * Phase 1: pre-map the first ~16 MB into the kernel range (bootloader),
      or build a minimal direct map covering where page tables will live.
    * Phase 2: build the full direct map using P2V (direct-map access).
    * Then switch VMM table access from KP2V to P2V.

### vmm_map_mmio

- [x] `vmm_map_mmio_wc()` for write-combining. `vmm_pat_init_this_cpu()`
  programs IA32_PAT entry 4 = WC (entries 0-3 keep power-on types, so every
  existing mapping is untouched); a leaf PTE with the PAT bit (`VMM_PTE_PAT`,
  bit 7) + PCD=PWT=0 selects it. Per-core, called from vmm_init + every AP
  (ap_main). The linear framebuffer now maps WC. 🪤 bit 7 is PAT on a LEAF but
  PS (huge page) on a PDPTE/PDE — `vmm_map_in` masks it out of the intermediate-
  table flags so it only lands on the leaf. Verified: `test vmm` reads PAT
  entry 4 == 0x01, fb renders, smp=4 clean.
- [x] `vmm_unmap_mmio(virt, size)` — clears the PTEs and returns the VA range
  to a small free list (`mmio_free[]`, first-fit, splits remainder) so it can be
  reused instead of leaked. `vmm_kmap_pages` also draws from it now. Verified:
  `test vmm` unmaps then re-maps and gets the SAME VA back.
- [x] Bounds check — `MMIO_END` (= `KSTACK_REGION_BASE`) is the hard ceiling;
  `mmio_reserve_va_locked` refuses to bump past it (also catches wrap) and
  returns 0 → the map fails cleanly instead of running into the kernel-stack
  region. Shared by `vmm_map_mmio[_wc]` and `vmm_kmap_pages`.

### Heap
- [x] kmalloc/kfree locking — spinlock with cli/sti save-restore.
- [x] ~~Slab allocator — fixed-size pools (16..1024) with O(1) common-case alloc
  (DEFERRED: metadata collision issues).~~ **DONE, and enabled.** The deferral
  reason is now the design. The first attempt had TWO fatal bugs:
  1. **Metadata collision** — it tagged each object `[0xAA][pool]` and hoped free
     could tell slab from general by that byte, but 0xAA occurs in real data.
     FIX: a **range registry** (`g_slab_ranges[]`) — every region records its
     `[start,end)`, and free/realloc route a pointer by RANGE lookup, which
     cannot false-positive the way a byte can. (This is the "track slab block
     ranges separately" the note asked for.)
  2. **Self-deadlock** — its free-list was a side array grown with `kheap_alloc()`
     called from *under* heap_lock, re-locking the same spinlock. FIX: an
     **intrusive free list** — a free object stores the next pointer in its own
     first 8 bytes (O(1), allocates nothing). No per-object header at all.
  A region is a permanent general allocation (`kheap_alloc_locked`, not the
  re-locking public wrapper) subdivided into objects. `slab_class_for` rounds UP
  (kmalloc(20)->32 pool) so slabs actually fire; on slab OOM / registry-full it
  falls back to general (an optimisation, never a requirement). krealloc/kfree
  range-check; kcalloc is transparent.
  Verified: `test kheap` 11/11 — every size class end-to-end, the **0xAA collision
  regression** (a general block full of the old magic byte frees correctly),
  multi-grow with no deadlock, no object aliasing, krealloc slab->general keeps
  data; kheap_check() clean after each phase. Plus: the whole kernel's small
  allocations now route through it, so booting to the desktop + `test posix` ALL
  PASS (smp=4) is thousands of live slab ops with zero corruption.
- [x] krealloc in-place expansion — checks if next block is free before allocating
  new; coalesces and returns same pointer if expansion succeeds. Reduces copies
  and fragmentation in dynamic array growth (e.g., embkfs_free_index_reserve).
- [ ] First-fit is O(n) — for allocations > 1024 bytes (small ones are now O(1)
  via the slab above). Segregated free lists by size class for the general path
  remain an option (lower priority).
- [ ] Security hardening (later): guard pages between allocations, allocation
  randomization, quarantine/delayed-free for use-after-free detection.

### Synchronization

- [x] **Sleeping mutex + counting semaphore** (`kernel/process/ksync.{c,h}`).
  Built on the wait-queue primitives that `keyboard.c`/`pipe.c`/`block.c` had
  each open-coded. A **sleeping** lock, not a spinlock: the distinction is a
  property of the CALLER (does it sleep while holding?), and the motivating case
  — the block-layer bounce buffer, held across a `hlt`-wait for the disk — does.
  Mutex is non-recursive and **detects self-deadlock** (re-lock by the owner is
  a loud halt, not a silent hang) and non-owner unlock (a warning). `test ksync`
  = 19/19 invariants; the bounce buffer now uses it (`test posix` still ALL PASS).
  - [x] Cancellation-aware acquire — `mutex_lock_interruptible` /
    `sem_wait_interruptible` return `-EMBK_ECANCELED` when the caller's process
    is cancelled while it *would block*, so a waiter behind a long-held lock
    unwinds on ^C instead of sleeping through the whole operation. The plain
    `mutex_lock`/`sem_wait` stay **uninterruptible** on purpose (a cleanup path
    must still be able to take a lock even after cancellation, and the flag is
    sticky). Cancellation gates only blocking: an uncontended acquire still
    succeeds regardless of the flag (completed-op-wins, as `process_wait`
    returns a ready child's status before considering cancellation). Proven by
    `test ksync`: a kthread blocked on a held mutex is `process_cancel`'d, wakes,
    and returns `-ECANCELED` without stealing the lock (the driver round-trips).
  - [ ] Not IRQ-safe by design (a handler cannot sleep). Spinlocks remain the
    IRQ-context tool. No compile-time guard enforces this yet.
  - [ ] No priority inheritance — a low-prio holder can be starved while a
    high-prio waiter sleeps. Irrelevant until priorities drive scheduling.

- [ ] Spinlock has no deadlock detection / lock-ordering checks.
- [x] Reader-writer locks — rwlock_t in kernel/cpu/rwlock.c (many readers OR one
  writer, single atomic state word; irqsave/irqrestore since multiple readers
  can't share one saved-flags slot). Smoke test: `test rwlock`. Not yet wired
  into any data structure (read-mostly candidates arrive with SMP).
  - [ ] Reader-preferring — a steady reader stream can starve a writer. Add a
    writer-preferring variant (pending-writer flag blocking new readers) if
    needed.
- [ ] Per-CPU heap caches to reduce lock contention (multi-core, future).

### Memory Management
- [x] EFER.NXE enabled (in gdt_init) so PTE bit 63 (NX) is legal — required
  before any VMM_NX mapping. Was missing; surfaced as a reserved-bit #PF
  (error 0x0C) the first time W^X set NX on the data segment. Consider moving
  the enable into vmm_init/CPU-feature init where it belongs logically.

---

## Interrupts & Timers

### APIC / IRQ
- [x] MADT interrupt source overrides now stored (`acpi_info.int_overrides`,
  parse_madt) and applied: `acpi_resolve_isa_irq()` maps an ISA IRQ → real GSI +
  polarity/trigger (MPS INTI flags), and `ioapic_route_isa()` routes through it.
  Keyboard/mouse/ATA switched to it; identity/edge/high when no override, so
  override-free machines are unchanged. Verified live: QEMU's `src=0 → gsi=2`
  (PIT) is parsed, and the ISA lines log their resolved GSI/trigger.
- [x] Spurious-interrupt vector (0xFF) now has an IDT handler
  (`lapic_spurious_stub`, isr.asm; installed in idt_init). Sends NO EOI (SDM:
  a spurious vector sets no ISR bit, so an EOI would ack a real interrupt) —
  a bare `iretq`. One install covers every core (shared IDT).
- [x] Spurious IRQ 7 / IRQ 15 detection — `pic_irq_is_spurious()` reads the PIC
  ISR via OCW3; `irq_handler` swallows a phantom (no EOI for a master-7, cascade
  EOI for a slave-15). Gated on NO registered handler so a real IO-APIC device
  on the same vector (ATA secondary = IRQ15 → vector 47) is never mis-dropped.
  Dormant while the PIC is masked; correct the moment a PIC line is used again.
- [x] Per-CPU LAPIC init — already in place: `lapic_init_this_cpu()`
  (MSR enable + SVR) is called by every AP in `ap_main()` (smp.c). Verified:
  smp=4 brings all APs online, each with its own LAPIC timer.
- [x] LAPIC timer TSC-deadline mode — implemented + CPUID-gated
  (`CPUID.01H:ECX[24]`), periodic LVT as the fallback. ⚠️ The active path is
  UNVERIFIED: QEMU's TCG APIC does not implement TSC-deadline and clears the
  bit on every CPU model (even -cpu max / +tsc-deadline), and no KVM is
  available here, so only the periodic path runs under our bring-up. Written to
  the SDM (Vol.3 10.5.4.1); needs KVM/real hardware to exercise.
- [x] Deduplicated the register save/restore across isr_common / irq_common /
  irq_common_lapic in isr.asm via shared `PUSH_GPRS` / `POP_GPRS` macros (the
  frame the C-side `struct registers` mirrors now has one definition).
- [x] irq_register/irq_unregister now save the line's prior PIC mask state on
  register and restore it on unregister, instead of unconditionally
  unmasking/masking (register/unregister is transparent).

### Exceptions
- [x] Page-fault handler prints CR2 + decodes the error code (P/W/U/RSVD/I)
  per Intel SDM Vol 3A §4.7.

### Timer / Time
- [x] Migrate to TSC + HPET for precise time (HPET one-shot, TSC high-res),
  both discovered via ACPI.
- [x] ~~Tick handler only does counter++ — eventually: preemption check +
  scheduler invocation~~ — **preemption is live**, it just is not in the handler
  this entry was looking at. `timer.c`'s PIT handler (IRQ 0) really does only
  `ticks++`, and correctly: it is the legacy wall clock. The scheduler runs off
  the **LAPIC timer** — `lapic_timer_handler()` EOIs, re-arms the one-shot
  TSC-deadline, then calls `schedule()`, on **every core's own tick**. Only the
  BSP advances the shared tick counter, deliberately, so it stays a wall clock
  rather than becoming a sum over online cores.

---

## Storage

### ATA / DMA
- [x] PRD_EOT had a stray trailing semicolon (`#define PRD_EOT 0x8000;`) in
  ata.c. Works in the current single-assignment use but will break inside a
  larger expression — remove the semicolon.
- [ ] Multi-PRD scatter-gather for buffers > 64KB or spanning pages (current
  limit: single contiguous region ≤ 64KB / ≤ 128 sectors).
- [ ] LBA48 DMA (READ DMA EXT 0x25) for > 128 sectors / disks > 128GB on the
  IDE/DMA path. (AHCI path already does LBA48.)
- [x] DMA buffer no longer has to be contiguous in callers: ATA DMA now uses
  direct multi-PRD for physically contiguous mappings and a bounce-buffer
  fallback for arbitrary virtual buffers.
  For arbitrary virtual
  buffers: walk pages + multi-PRD, or bounce-buffer.
- [x] ~~Secondary channel DMA (offset 0x08 in BMIDE) not handled~~ — done
  (EMBKFS v2 Phase 21): per-channel IRQ handling (IRQ14 AND IRQ15, each
  with its own completion flag) plus a `bmide_channel_base()` helper
  (secondary channel's Bus-Master command/status/PRDT registers are at
  `BAR4 + 0x08`, not `+0x00`). Found because any I/O to a 3rd/4th drive
  was silently hanging for ~2.7 hours (`ata_wait_irq()`'s 1e6-iteration
  timeout) before this fix — see `docs/EMBKFS_spec_v2.2.md` §2.
- [ ] No cache-coherency handling (fine on QEMU/x86; matters on some HW).

### AHCI
- [ ] Polls PxCI for completion — switch to interrupt-driven (PxIS + the
  controller's interrupt) for efficiency.
- [ ] Single PRD only — add multi-PRD scatter-gather.
- [ ] Only port 0 / first device exercised — generalize across all present
  ports if multiple SATA disks appear.

### Block layer
- [x] Partition support — MBR (DOS) table parsed; each primary partition is
  exposed as a child block device (sda1, sda2…) that delegates to its parent
  disk at the partition's start LBA, bounded by its length. Mount probe sees
  partitions alongside whole disks. See kernel/block/partition.c.
  - [x] ~~GPT not parsed yet~~ **DONE** — header at LBA 1 behind the protective
    MBR, **CRC32 validated before any field is trusted** (every number we act on
    -- entry array location, count, size -- comes out of that block, so a corrupt
    one would register partitions over arbitrary sectors: fail closed). Sparse
    tables handled (all-zero type GUID = unused slot); `last_lba` is INCLUSIVE.
    ⚠️ **GPT needs CRC32-IEEE (0xEDB88320)** — `kernel/fs/embkfs/crc32c.c` is
    CRC32**C** (Castagnoli), a DIFFERENT polynomial. Reusing it because "we
    already have a crc32" would reject every real GPT on earth. `crc32_ieee()`
    now lives in partition.c.
    Verified against `sfdisk` as an independent oracle (`make part_gpt.img`):
    3/3 partitions, start+length exact.
    - [ ] *(verified still open — `partition.c:225` returns 0 with the comment
  "the backup header at the last LBA could be tried; not yet")* Backup header
  (last LBA) not tried when the primary's CRC fails —
      we refuse rather than recover. The recovery is the point of the backup.
    - [ ] Entry-array CRC (`partition_entry_array_crc32`) not checked; only the
      header's.
  - [x] ~~Extended/logical partitions (type 0x05/0x0F) detected but not walked.~~
    **DONE** — the EBR chain walks; logicals register as `sda5+` (the container
    itself gets no device: nothing could mount it).
    🪤 **THE EBR TRAP: the two entries use DIFFERENT BASES.** Entry 0 (the
    logical) is relative to THIS EBR's LBA; entry 1 (the link to the next EBR) is
    relative to the EXTENDED partition's start. One base for both "works" for the
    first logical and then walks into nonsense. The walk is also bounded (32) and
    rejects self-links — a corrupt EBR must not hang the boot.
    Verified against `sfdisk` (`make part_ext.img`): sda1/5/6/7 exact.
    🪤 Names went TWO-digit: logicals start at 5 and GPT allows 128, so the old
    single digit made `sda12` collide with `sda2` — two devices, one name.
  - [x] ~~Scan must run after `sti` (ATA read path is IRQ-driven)~~ — done, and
    the entry already described the fix: it sits in `main.c` immediately before
    block enumeration / mount probe precisely so interrupts are on by then.
- [x] ~~Block-layer reads/writes on IDE secondary channel hang~~ — done
  (EMBKFS v2 Phase 21), see the ATA/DMA entry above. Disks on the
  secondary channel (3rd/4th drive) now work; FAT32's test disk staying
  on IDE primary slave is now just convention, not a hard requirement.
- [x] ~~Block-layer DMA bounce buffer is shared global state — needs a lock~~
  **DONE.** The old "fine while synchronous" note had expired: SMP, preemption
  and several processes doing file I/O (CPython/git read while a UI app runs)
  mean two concurrent bounce-path transfers would memcpy over each other and each
  return the other's sectors — **silent corruption, not a crash**. The facts
  that decided the design, established before writing it:
  - callers are `fat32.c` / `embkfs.c` / `partition.c` — **not IRQ handlers**;
  - **nothing serialises it higher up** (`embkfs` has no lock at all);
  - **the ATA path `hlt`-spins and is preemptible**, so the buffer is held across
    a multi-millisecond disk wait.
  ⚠️ **That last fact makes a spinlock WRONG** — a preempted holder would deadlock
  the next thread into the block layer on the same core ("slept holding a
  spinlock"). The kernel has no mutex, so this is a **sleeping lock built from
  the wait-queue primitives** `keyboard.c`/`pipe.c` already use: a flag + a queue,
  `{test → set}` made atomic by `g_sched_lock`.
  Notes for the next reader: the lock spans the **whole chunk loop** (releasing
  between chunks would let another caller reuse the buffer mid-transfer — the
  exact corruption being prevented), **both `rc != OK` error paths release**, and
  **pre-scheduler safety is by construction**: boot callers are single-threaded,
  so `busy` is never true, the `while` never runs, and `sched_block` is never
  reached without a thread to block.
  Verified: boots (the pre-scheduler partition scan is the thing that would hang),
  `test posix` **ALL PASS**, `test ioperf` unchanged (440 ms/MB cold, 194%
  amplification, disk still 1% of wall).
  - [x] ~~⚠️ **The race itself is NOT covered by a test.**~~ — written, and it
    found something better than a pass. **`test blockrace`** spawns 4 readers
    (`ioracer.elf`) before waiting on any, each verifying every byte of a
    single-byte-filled fixture, so one stolen sector is unmistakable. All four
    get their own bytes — **and the bounce lock is contended ZERO times.**
    - **That is not a weak test, it is a finding:** the **EMBKFS big lock
      serialises every filesystem operation**, so two processes cannot be
      inside the block layer at once *by way of the filesystem*. The bounce
      lock sits underneath a lock that already excludes the concurrency it
      defends against. Asserting "contention > 0" would have been demanding a
      race the architecture currently forbids — no amount of hammering
      produces it.
    - So the test asserts the **relationship** instead: contention through the
      fs path must be **zero**, because the big lock serialises above it. If it
      ever fires non-zero, the layering changed — a finer-grained fs lock (the
      open item above), a second filesystem alongside EMBKFS (`fat32.c` does
      **not** take the EMBKFS lock), or a non-fs block user — and the bounce
      lock stopped being defence-in-depth. The test is how that transition
      announces itself instead of being found by corruption.
    - Instrumentation added to make any of this checkable: `blkstat` now counts
      `bounce_reads` / `bounce_writes` / `bounce_contended`. Measured: a single
      `test posix` takes the bounce path **8617 times** — it is the hot path,
      not an edge case, because kmalloc memory is in the direct map while ATA
      needs the kernel range. Also visible: ~3 MB of logical reads produced only
      216 device bounce reads, the rest served by EMBKFS's object cache — a
      test assuming "bytes read == device reads" would be measuring the cache.
  - [ ] Bounce always copies; multi-PRD scatter-gather (page-walk via
    `vmm_get_phys`) would be the zero-copy alternative, and would retire the
    shared buffer — and this lock with it. Also: bounce always copies; multi-PRD scatter-gather (page-walk
  via vmm_get_phys) would be the zero-copy alternative (already on TODO).

---

## PCI

- [~] BARs are not cached in `struct pci_device`, but "only printed" is wrong:
  `pci_read_bar()` reads and **sizes** any BAR on demand and returns a typed
  `struct pci_bar` (address, size, MMIO-vs-IO, 64-bit, prefetchable, valid),
  and every driver that needs one calls it (ATA's BAR4, xHCI, virtio-gpu, ...).
  What is missing is only the caching.
  - [ ] Cache them in `pci_device` if a caller ever needs BARs without a
    config-space round trip. Nothing does today — enumeration is once at boot.
  Cache them so drivers retrieve without re-reading.
- [ ] No ECAM/MCFG (PCIe memory-mapped config) — only legacy CAM. Parse the
  MCFG ACPI table, map config space, support 4KB extended config.
- [ ] No recursive bridge scanning — brute force works but doesn't follow
  secondary buses behind PCI-to-PCI bridges properly.
- [x] ~~No capability-list parsing (MSI/MSI-X, power management)~~ — the
  capability list IS walked: `pci.c` checks the status-register capabilities
  bit, then chains through looking for **MSI (id 0x05)** and **MSI-X (id
  0x11)**, and programs table entry 0 to deliver a chosen vector
  (`pci_enable_msi` / `pci_enable_msix`). xHCI uses the MSI-X path in anger.
  - [ ] Still unparsed: **power management** (id 0x01) and everything else on
    the chain — only the two interrupt capabilities are looked for.
- [x] ~~No interrupt routing — wire device INTx/MSI to IO-APIC/LAPIC~~ — both
  directions exist: ISA/INTx lines go through the IO-APIC with MADT overrides
  applied (visible at boot, e.g. `IO-APIC: ISA IRQ 14 -> GSI 14 (edge)` then
  `GSI 14 -> vector 46`), and MSI/MSI-X are programmed straight to a LAPIC
  vector by `pci_enable_msi`/`pci_enable_msix`.
  - [ ] Still absent: reading the **ACPI `_PRT`** to discover a PCI device's
    INTx→GSI mapping. Today a driver is told its line rather than deriving it,
    which works because this machine's devices are known.
- [ ] No device-specific driver-binding mechanism yet.
- [ ] Vendor/device ID → human-name database (currently only class names).

---

## Drivers — Display / Console

### Framebuffer / VBE
- [x] ~~Real-mode VBE path hardcoded to mode 0x118~~ — done: `stage2.asm`'s
  `vbe_init` now does EDID query (INT 10h AX=4F15h BL=01h, Detailed Timing
  Descriptor #1 for native resolution) → exact-match search through the
  enumerated VBE mode list (`VideoModePtr` at VBE Info Block offset 0x0E,
  `find_mode_for_resolution` requiring ModeAttributes
  {supported, graphics, LFB} and MemoryModel 6/direct-color, preferring
  higher bpp among matches) → a fixed fallback list (1920×1080 → 1280×1024
  → 1024×768 → 800×600 → 640×480) → the original hardcoded mode 0x118 if
  literally nothing else worked. `selected_mode` defaults to 0x118 and is
  only ever overwritten by a mode `find_mode_for_resolution` actually
  confirmed exists, so any bug in the new EDID/enumeration path degrades to
  exactly the old behavior rather than something worse — deliberate, since
  stage2 has no serial debugging to fall back on if real-mode BIOS calls
  misbehave. Verified in QEMU with `-vga cirrus` (a device neither
  `bochs_vbe.c` nor `virtio_gpu.c` claims, so this path actually runs):
  picked 1280×1024×16bpp from the fallback list (Cirrus doesn't support
  32/24bpp direct-color there), rendering verified correct via screendump.
  Remaining: no scoring by "closest to native" when an EXACT resolution
  match isn't found (only the fixed fallback list is tried in that case);
  no final text-mode-at-0xB8000 fallback if VBE itself is entirely absent
  (still `.vbe_failed` → hang, unchanged from before).
  - Refs: VBE 3.0 (Function 15h DDC), EDID 1.4 (VESA E-EDID).
- [x] ~~UEFI GOP path (modern alternative to VBE; needs the UEFI bootloader).~~ —
  the UEFI loader queries GOP and hands the framebuffer to the kernel via
  `boot_protocol.fb_*` (the kernel's `framebuffer.c` uses it as the fallback mode).
- [x] ~~GPU acceleration~~ — done: `gpu.c` probes PCI for a GPU driver before
  `fb_init`; `bochs_vbe.c` does runtime DISPI modeset, `virtio_gpu.c` drives
  an accelerated guest-memory scan-out (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
  per dirty rect). `framebuffer.c` gained a RAM backbuffer with dirty-rect
  `fb_present()`, clipped fill/copy rects, lines, circles, alpha-blit.
  Remaining: `virtio_gpu.c` only negotiates VIRTIO_F_VERSION_1 (no indirect
  descriptors, no multiple scanouts, no 3D/Virgl); `bochs_vbe.c` has no
  `-vga qxl` support. (The LFB mapping is now write-combining via
  `vmm_map_mmio_wc` — see the `vmm_map_mmio` entry above.)

### Keyboard

*(`kernel/drivers/input/keyboard.c`. Several items below were closed by work
that needed them: **Ctrl** by Ctrl-C ([INTERRUPTION.md](INTERRUPTION.md)),
**arrows/Home/End** by the EmUI text editor and the shell's history recall.)*

- [x] ~~Only ASCII press events. Add release tracking for proper modifier
  state.~~ **DONE for the keys that need it** — Shift (`0xAA`/`0xB6`) and Ctrl
  (`0x9D`) breaks are tracked. Non-modifier releases are still dropped
  deliberately: nothing consumes key-up, and a make/break API is a bigger change
  (see the open item below).
- [x] ~~No modifier handling (Shift, ..., Ctrl, ...)~~ — **Shift and Ctrl are
  handled**: `shift_down`/`ctrl_down` plus a full `scan_to_ascii_shift[]` table,
  so uppercase and `|` are typeable. **Ctrl works from BOTH keys** — right Ctrl
  arrives as `0xE0,0x1D` and is tracked identically, so `^C` works from either.
  🪤 The driver never tracked Ctrl at all until Ctrl-C needed it: **Ctrl+C simply
  typed `c`**, which made `^C` untypeable and looked like a routing bug.
- [x] ~~No extended scan codes (0xE0)~~ — **arrows, Home/End, PgUp/PgDn and Del
  are delivered** as private single-byte `EK_*` codes (`keyboard.h`), NOT ANSI
  escape sequences, so consumers need no escape state machine. Mirrored as
  `EMBK_KEY_*` in `user/lib/embk.h` — **change one, change both.**
  Also handles set-1's **fake shift**: an `0xE0`-prefixed `0x2A`/`0xAA` pads nav
  keys and must never touch real shift state.
- [x] ~~**Caps Lock**~~ **DONE** — a real latch: it flips on the MAKE and ignores
  its own break, applies to **letters only**, and **XORs with Shift**
  (Caps+Shift+a = 'a'). Applying it to the shift table — the obvious
  implementation — would make Caps type `!` for `1`, which no keyboard does.
  LED driven via `0xED`. Verified live: press → `mods=0x10`, press → `0x00`.
- [x] ~~**Alt**~~ **DONE** — `EKM_ALT`, both sides, on the event stream
  (verified live: `EKC_LALT` down → `mods=0x04`, up → `0x00`).
- [x] ~~**F1–F12**~~ **DONE** — `EKC_F1..EKC_F12` on the **event** stream. They
  get no character *by design*: C0 is Ctrl+letter's, and inventing an escape
  sequence would force every reader to become a state machine.
- [x] ~~**Ins**, **Windows/Menu**~~ **DONE** — `EKC_INS`/`EKC_LWIN`/`EKC_RWIN`/
  `EKC_MENU`, event-only for the same reason.
- [x] ~~**No key-up delivery / no make-break API.**~~ **DONE** — `struct
  key_event {code, mods, pressed}` + `sys_key_event_poll` (65) / `sys_key_mods`
  (66). A **second stream** beside the characters, not a re-encoding: the char
  stream is out of room and ambiguous (Up and Ctrl+S are both `0x13`), so text
  readers keep doing byte compares and are untouched. Still open: **typematic
  repeat-rate** control (`0xF3`).
- [x] ~~No layout abstraction~~ **DONE** — `struct keymap` + `keyboard_set_layout()`;
  **us** and **dvorak** ship. An unknown name is REFUSED, not silently ignored.
  ⚠️ **AZERTY is still not shippable, and not for want of a table**: French needs
  é è ç à ù, which are **not ASCII**, and the char stream is `char`. An "AZERTY"
  today could only be QWERTY-with-letters-moved plus silent holes — a worse lie
  than saying no. **Real AZERTY needs the char stream to carry a wider encoding
  first**; that is the actual open item, not the keymap.
- [x] ~~No PS/2 controller (8042) init~~ **DONE** — explicit config-byte RMW
  (IRQ1 on, kbd clock on, translation on) + an explicit `set 2` select.
  🪤 **Scancode set and translation are ONE decision**: device-in-set-1 with
  translation on makes the controller translate set-1 as set-2 → dead keyboard.
  We pick the standard pairing (device set 2 + translation) because our tables
  are set 1, and say so.
  🪤 **`mouse.c` RMWs the SAME config byte.** We touch only keyboard bits (0/4/6)
  and deliberately issue **no controller self-test (0xAA) and no port disables**
  (0xAD/0xA7) — every textbook init does, and here they would reset state the
  mouse depends on. Verified: `IntelliMouse wheel enabled` still appears.
- [x] ~~Migrate to USB HID once there's a USB stack~~ — done, see USB section.

---

## USB Host Controllers

All four generations now bring up their root ports and enumerate devices:
`xhci.c` (own IRQ-driven path, HID + mass storage), `ehci.c` (async
QH/qTD schedule, periodic interrupt QHs, releases FS/LS ports to a
companion), `uhci.c` (I/O BAR4, frame-list schedule), `ohci.c` (MMIO,
ED/TD lists + HCCA periodic table). `usb_core.c` is the shared HCD-agnostic
layer: enumeration, a HID boot-keyboard driver (with shift map), a
mass-storage BOT/SCSI driver that registers block devices, and hub support
(below). Legacy HCs are polled from the main loop (`usb_poll`); xHCI stays
interrupt-driven.

- [x] ~~No hub support on the legacy HCs~~ — done for UHCI/OHCI/EHCI:
  `usb_core.c`'s `usb_hub_attach` fetches the class-specific hub descriptor
  (port count), then for each port does power-on + reset + status read
  (mirroring each HCD's own root-port scan, just through class-specific hub
  requests instead of native port registers) and enumerates whatever's
  connected — single level only, with a depth guard against a malformed/
  looping topology. Verified in QEMU: a keyboard behind a `usb-hub` on UHCI
  enumerates and works (`addr 2`, boot keyboard ready). Deliberately NOT
  covered: xHCI (separate legacy code path, not `usb_core.c` — still just
  logs "USB Hub detected" and stops, matching its pre-existing "only the
  first device enumerates" limitation) and full/low-speed devices behind a
  high-speed EHCI hub (needs a Transaction Translator, EHCI spec §4.14 — a
  genuinely separate, larger feature; only same-speed downstream devices
  are enumerated). Both are documented gaps, not silent ones.
- [ ] No isochronous transfers (audio/video class devices unsupported).
- [ ] No USB3/SuperSpeed on xHCI beyond what already worked before this
  change — streams, bursting, and the SuperSpeedPlus descriptors are unused.
- [ ] Static per-controller-type tables (`UHCI_MAX_HC`/`OHCI_MAX_HC`/
  `EHCI_MAX_HC` = 2, `USB_MAX_DEVICES` = 16) — fine for QEMU testing, will
  need to become dynamic for real hardware with many devices.
- [ ] Root ports (and now hub ports) are scanned once at boot; no hot-plug
  (connect-status-change interrupt) handling anywhere in the USB stack.
- [ ] EHCI/UHCI/OHCI control and bulk transfers are synchronous busy-polls
  (bounded by a spin-count timeout, not wall-clock) — fine for boot-time
  enumeration, would stall the kernel if called after multitasking exists.
  (Multitasking now exists — see Process & Scheduling below — but nothing
  currently calls into USB from more than one process at a time, so this
  hasn't bitten yet; revisit before that changes.)
- [x] ~~No automated selftest for the display or USB stack~~ — partially
  done: `test usb` (`usb_run_selftests`, `usb.c`) cross-checks the USB
  controller table against a fresh PCI class 0x0C/subclass 0x03 scan and
  asserts every discovered controller was classified into a real HC kind
  and (if it has a usable BAR) actually initialized — deliberately
  independent of which HC/device happens to be attached, a real assertion
  about the discovery/classification code path itself. `test gpu`
  (`fb_run_selftests`, `framebuffer.c`) draws known primitives
  (`fb_fill_rect`, `fb_copy_rect`) and asserts exact colors read back via
  `fb_get_pixel`, exercising the actual color pack/unpack path for whatever
  mode is active. Neither covers live HID/mass-storage data transfer or
  actual on-screen/on-host rendering — those are still manual-QEMU-only
  (screendumps + serial-log inspection per HC generation, done this
  session for UHCI/OHCI/EHCI/xHCI and Bochs DISPI/VirtIO-GPU).

---

## Filesystem

### EMBKFS
- [x] ~~Crash while a file is unlinked-but-open LEAKS its inode~~ — the
  mount-time sweep FIX below is done: `embkfs_mount_orphan_sweep()` runs
  on every read-write mount, scanning for `links==0` inodes and reclaiming
  them. STILL OPEN (the "STRONGER" tier): an on-disk orphan list for
  crash-safe deferred delete, replayed on mount — bigger, only worth it if
  crash-safety of the unlinked-open window specifically matters.
- [~] Open-ref table (`g_open_refs`, EMBKFS_MAX_OPEN_OBJECTS = 64) is a fixed
  array with linear scan and no lock **of its own** — but it is no longer
  unprotected: `obj_get`/`obj_put` reach it through public entry points that
  all take the EMBKFS big lock, so concurrent access is serialised with the
  rest of the filesystem. What remains is the FIXED SIZE (64) and the linear
  scan, which are capacity/efficiency limits, not races. The block-layer
  bounce buffer below is still the genuinely unprotected one.
- [ ] `embkfs_parent_dir_oid` resolves `..` by a full-tree scan (O(tree) per
  `..`). VFS now does dot-dot itself with a breadcrumb stack, so this path is
  only hit by EMBKFS-internal callers — but if those stay, a stored parent
  back-ref would make it O(1). Low priority.
- [~] The extent-supersede bug (shrinking write → `-EMBK_EINVAL` when the
  prior data landed as ONE multi-block extent): **no longer reproducible, and
  not knowingly fixed.** Re-checked 2026-07-23 — the documented sequence
  (`test embkfs timestamps` then `test embkfs obj` on a fresh volume) passes
  *with the triggering shape present* (the log shows the 4103-byte write
  landing as `1 extent, 2 blk`, then the truncate succeeding). No fix was
  identified: `puts_cap` and the allocate/supersede loop are unchanged since
  the report (checked against 00fa091), so either something else in the
  intervening work moved it, or the trigger needed a finer bitmap state than
  "one extent vs several". **Cannot-reproduce is not fixed**, so this stays
  on the list rather than being ticked.
  - What changed instead: the invariant is now *tested*. **`test embkfs
    shrink`** runs 12 shrinking writes over whatever extent shapes the volume
    produces (truncate-to-empty, block-boundary and off-by-one cuts, a
    hole-bearing file) and checks the surviving BYTES, not just the return
    code. Verified green twice — once on a fresh volume *with* the trigger
    shape, once on a churned volume without it.
  - The lesson, recorded because it cost this investigation: the original
    repro depended on the free-block bitmap state two *unrelated* tests
    happened to leave behind. That is a coincidence with a procedure attached,
    not a repro — and it decayed into a passing sequence that could no longer
    distinguish "fixed" from "hidden". The replacement asserts the invariant
    and *reports* which shapes it saw, so a run that missed the trigger cannot
    read as one that hit it. (The first draft of the new test repeated the old
    mistake — it demanded a shape and skipped when the allocator refused, and
    7 of 9 cases went unexercised. It reported `try again` rather than green,
    which is how the flaw was caught.)

#### EMBKFS v2 (see `docs/EMBKFS_spec_v2.2.md` for full detail on all of these)
- [ ] Snapshot allocator hold-back is conservative ("hold every freed block
  while ANY snapshot exists"), not true per-block reference counting. Safe
  (never frees something a snapshot needs) but can delay reclaiming space
  that isn't actually still needed. True refcounting would need tracking,
  per block, which snapshot(s) if any still reference it.
- [x] ~~Rolling back to a snapshot reverts the snapshot registry too — any
  snapshot taken AFTER the rollback target becomes inaccessible~~ — **DONE
  (v2.3, `EMBKFS_INCOMPAT_SNAPREG`)**: the registry moved out of the CoW tree
  into one fixed block (block 1, in the pre-superblock region the formatter
  already left unused) that no transaction rewrites. Rollback swaps the root
  and never touches it, so snapshots on both sides of the target survive and
  rollback is navigable in both directions. Own CRC32C (a failed check reports
  EMPTY rather than serving bogus roots at the allocator); `bitmap_build`
  reserves the block and mkfs counts it, so the mount-time free-count oracle
  still agrees exactly. Legacy volumes without the bit keep the old in-tree
  path and the old limitation. Proof: **`test embkfs snapreg`** — s2 survives
  a rollback to s1, then rolls *forward* to s2 and back again (which also
  proves s2's frozen tree stayed physically intact), and survives a full
  bitmap rebuild. `verify_embkfs.py` §1a validates the block independently.
- [ ] Verified-root boot check uses one HMAC key embedded in the kernel
  binary (authentication against OFFLINE tampering), not real asymmetric
  signing (which would also defend against someone who has the kernel
  image itself). A true upgrade (Ed25519 or similar) is its own
  crypto-primitive-sized body of work on top of the existing SHA-256/AES.
- [ ] AES-256-XTS has no ciphertext stealing — every write happens to
  always be block-size-rounded today, so it's never been needed, but a
  future call site that ISN'T block-rounded would need it added.
- [ ] Crypto header (Phase 4) and verify header (Phase 5d) both live past
  the superblock's own CRC32C checksum coverage (`EMBKFS_SB_BODY_SIZE`) —
  deliberate (corruption there fails closed, not open) but means a
  corrupted crypto/verify header isn't caught by the SAME mechanism that
  catches a corrupted superblock body; each has its own magic-number
  sanity check instead.

### VFS
- [x] ~~Single mount only~~ — done: `vfs_find_mount` does real longest-prefix
  matching (`vfs_mount_is_prefix` + a `best`/`best_len` scan) across up to
  `VFS_MAX_MOUNTS = 8` slots. Landed alongside EMBKFS v2's multi-volume
  mounting (each volume gets its own `/<device_name>` mount point).
- [ ] No `.readlink` op. Symlinks resolve to the LINK vnode but can't be
  followed; EMBKFS already has `embkfs_readlink_object`.
  - FIX: add `.readlink` to vfs_ops + adapter, follow links inside
    `vfs_resolve` with a hop-count bound (ELOOP). NOTE: true `..` then differs
    from the lexical breadcrumb `..` (a symlink's real parent vs its path
    parent) — revisit the stack semantics when this lands.
- [x] ~~No `.truncate` op → O_TRUNC can't be honored at open().~~ **DONE** —
  `vfs.h` has `.truncate(vn, size)`, `vfs_fd_truncate()` exposes it as
  `ftruncate(2)`, and O_TRUNC is wired in `vfs_open`. A filesystem that leaves
  the op NULL fails an O_TRUNC open with `-ENOSYS` rather than silently opening
  a file it did not truncate. Driven by git, which ftruncates its index.
- [ ] stat() simplifications: directory nlink hardcoded to 2 (real = 2 +
  subdir count); directory size reported as ENTRY COUNT, not bytes (ls tags it
  'e'). Decide whether anything depends on either before "fixing."
- [x] ~~`unlink` op has remove(3) semantics — it rmdirs an empty directory.~~
  **DONE** — `embkfs_unlink()` returns **`-EISDIR`** on a directory; `rmdir` is
  a separate, dirs-only path that returns `-ENOTEMPTY` on a populated one.
  (The v2.2 `unlink` was worse than mis-specified: it was a **stale lie** that
  reported success without removing anything. git found it.)
- [x] ~~Absolute paths only — no cwd / relative resolution.~~ **DONE**, but
  deliberately NOT in the kernel: the VFS stays the one absolute-only path
  parser and never sees a relative path. cwd is **libc state**
  (`user/lib/syscalls.c`), which makes it per-process for free — every process
  already has its own libc data, so there is no kernel state to leak. Not
  inherited: a parent names `PWD` and the child's crt0 seeds from it.
  `path_abs()` also NORMALIZES (`/a/../b` used to reach the kernel raw and die
  on a `..` the VFS has no rule for — git walks UP to find a repo root).
  Still open: `fchdir()` is honest `ENOSYS` — an fd does not know its own path
  (`fd_entry` holds mnt+ino, and nothing maps an ino back to a name).
- [ ] `vfs_mount` duplicate-mount check is a raw strcmp on `at` (not a
  normalized path). Fine for the single "/" mount.

### File descriptors
- [x] ~~fd table is fixed (64), global, per-boot. No per-process fd tables~~ —
  done: `struct fd_entry` moved to `kernel/fs/fd.h` (public) and `struct
  process` now embeds `struct fd_entry fds[FD_MAX_OPEN]`. `fd.c` picks the
  table via a `fd_table()` helper — `current_process->fds` if there is a
  current process, else a boot-time-only `g_boot_fds` (preserves the
  existing pre-process `test ring3`/selftest behavior that runs before any
  process exists). Verified via QEMU: two spawned processes opening
  different files see independent fd numbering/state, no cross-talk.
  Open-file-description sharing (fork / dup / dup2, shared cursor across
  dup'd fds) is still NOT modeled — each fd entry still owns its own
  cursor; only isolation between processes was added, not fd aliasing
  within one.
- [x] ~~g_boot_fds / per-process fds arrays are unlocked mutable state — needs
  a lock once syscalls from the same process can race.~~ **DONE**, and the race
  was already REACHABLE, not hypothetical: the note's "a single process only
  runs one syscall at a time" was **stale**. `sys_thread_create` (14) exists, and
  the scheduler picks any READY thread per core (only `pinned_cpu` threads are
  core-locked) — so two threads of one process run on two cores and race the
  shared `fds[]`. The open path was the worst: it found a free slot, did a
  possibly-blocking `obj_get`, and only THEN marked the slot used — a wide window
  for two threads to claim the same slot.
  Fix: a per-process **`struct mutex fd_lock`** (the new ksync mutex — a SLEEPING
  lock, because obj_get/close do I/O). `fdlock(p)` keys on the process (the
  install paths mutate a TARGET's table) and no-ops for the boot context (NULL =
  g_boot_fds = single-threaded). Open now claims the slot atomically; close frees
  under the lock then tears down OUTSIDE it on a snapshot (mandatory: pipe close
  takes g_sched_lock, which the mutex is built on). The one rule — never take
  fd_lock under g_sched_lock — holds at all five sites.
  Verified: `test posix` ALL PASS on **smp=4** (heavy open/close/read/write, no
  deadlock), `test thread smp` OK (8 threads/one proc — the process_alloc change).
  - [ ] ⚠️ **The race trigger is still not covered by a test** — every suite is
    effectively single-threaded per fd table, so the lock is never contended.
    A concurrent open/close hammer (N processes, assert no two fds alias one
    slot) is the missing harness.
    - **The template now exists**: `test blockrace` + `user/bin/ioracer.c` do
      exactly this shape for the block layer — spawn N processes *before*
      waiting on any, then assert on content rather than on return codes, and
      report whether the lock was actually contended so a vacuous pass is
      visible. Copy that structure; the fd version needs an ioracer-equivalent
      that opens/closes rather than reads.
    - Worth knowing before writing it: the bounce-buffer version came back
      **zero-contended** because the EMBKFS big lock serialises above it. Check
      whether the fd table has a similar layer above before concluding the
      absence of contention means the lock is unnecessary.
- [x] ~~O_TRUNC reserved but not honored.~~ **DONE** — `fd.c` shrinks to zero
  through the VFS truncate op at open time.
- [ ] O_APPEND is implemented by re-stat-to-end before each write — one extra
  stat per write, and a TOCTOU window between the stat and the write. **NOT
  fixed by the fd_lock above** — that guards the fd TABLE, but two O_APPEND
  writers (even in different processes, or two fds to one file) race on the
  file's CONTENTS, not the table. A correct fix is FS-level: an atomic
  append-at-end op (or a per-object append lock) in the write path, not fd.c.
  A per-fd lock would give false confidence (misses the two-fds-one-file case).
  Now genuinely reachable (threads on SMP), so no longer just theoretical.

---

## User Mode & Syscalls

### ELF Loading (COMPLETED)
- [x] ELF64 loader (kernel/cpu/elf.c/h) loads PT_LOAD segments with correct
  permissions (PF_X → no NX, PF_W → writable). Maps pages in user VA space
  (0x0000400000000000–0x0000700000000000 low-half), restores real p_flags
  after loading (NX on data, exec on code).
- [x] Entry point (e_entry) from ELF header used instead of hardcoded
  0x400000. init_blob is now an ELF-format binary, not flat .bin.
- [x] Context save/restore (kernel_ctx_save / kcontext) allows kernel to
  resume after user program exits via sys_exit.

### Ring-3 Entry & Exit (COMPLETED)
- [x] iretq path to ring 3 with user code/data selectors, user RSP, EFLAGS=0x202
  (IF+reserved). Works with interrupt handling + IRQ re-enable on exit.
- [x] sys_exit (syscall 2) restores saved kernel context → control returns to
  enter_user_mode (no halt).
- [x] Interrupts live during user execution (no CLI mask); IRQs pre-enabled
  before entering ring 3.

### Security — user-pointer validation (HIGH, the real hole) — DONE
- [x] ~~`sys_write` dereferences a raw user pointer with NO validation~~ — done:
  `kernel/cpu/usercopy.c/h` adds `access_ok(ptr, len)` (checks the range is
  below the canonical low-half boundary `0x0000800000000000` and every page
  in it is actually mapped in `current_process->pml4_phys` via
  `vmm_get_phys_in`), plus `copy_from_user`/`copy_to_user` (access_ok +
  memcpy, `-EMBK_EFAULT` on failure) and `copy_string_from_user` (bounded,
  byte-at-a-time, for path arguments). Wired into every syscall that takes a
  user buffer or path (`sys_write`, `sys_read`, `sys_open`, `sys_stat`,
  `sys_readdir`, `sys_spawn`). Verified with a temporary test in
  `user/init.c`: a normal buffer round-trips correctly, and a deliberately
  bad pointer (a kernel address) is rejected with `-EMBK_EFAULT` instead of
  being dereferenced — confirmed via QEMU, then the test scaffold was
  reverted.

### Userspace loader
- [x] elf_load partial-load cleanup now happens by loading into a fresh
  address space and destroying that address space on load failure.
  but segment N+1's frame alloc fails, segment N's pages stay mapped and
  nothing unmaps them. Harmless in the single shared address space today;
  becomes a real leak once per-process address spaces exist (a failed load
  must discard the whole attempt). FIX: unwind mapped segments on error, or
  build into a fresh address space that's simply dropped on failure.
- [x] Loads /init.elf from the filesystem (multi-block extent read verified).
  Embedded-blob path removed.

### Syscall transport
- [x] `int 0x80` (software gate) implemented: dispatch table now has 13 calls
  — write=1, exit=2, yield=3, open=4, close=5, read=6, lseek=7, stat=8,
  readdir=9, spawn=10, wait=11, getpid=12, kill=13. `spawn` returns a
  capability handle (not the raw pid); `wait`/`kill` take one.
- [ ] Fast path `syscall`/`sysret` deferred: needs STAR/LSTAR/SFMASK MSRs + EFER.SCE,
  AND swapgs + a per-CPU GS base for the kernel-stack switch (syscall does NOT
  switch stacks via the TSS). Wants per-CPU/SMP infra. GDT already laid out user
  data-before-code for it.
- [x] ~~Syscall table: wire the fd/VFS syscalls~~ — done: `sys_open`,
  `sys_close`, `sys_read`, `sys_lseek`, `sys_stat`, `sys_readdir` (via a
  callback-based `sys_readdir_cb` walking `vfs_readdir`, filling a
  `struct sys_dirent { ino, type, name[59] }` per entry) all go straight
  through to the existing `vfs_*`/`vfs_fd_*` layer, guarded by
  `copy_from_user`/`copy_to_user`/`copy_string_from_user` (see
  user-pointer validation above).
- [x] ~~`sys_write` is serial-only and ignores fd~~ — done: fd==1 still goes
  straight to serial (no fd-table round trip needed for the common case);
  any other fd routes through `vfs_fd_write` via a bounce buffer.
- [x] Found bug (not on this list originally): `sys_exit` read the exit code
  from `r->rax` (the syscall number — always 2) instead of `r->rdi` (the
  actual argument), so every process's exit code was hardcoded to 2
  regardless of what it passed. Fixed to read `r->rdi`; verified exit code
  changes correctly (6 for `/init.elf`'s counter+bss-sum check).
- [x] Found bug (not on this list originally): `int 0x80` is an interrupt
  gate, so IF is auto-cleared for the entire syscall handler — any syscall
  that waits on a hardware completion IRQ (e.g. disk I/O inside `sys_open`)
  hung forever. Fixed by adding `sti` as the first instruction of
  `syscall_dispatch()`.

### Ring-3 / TSS plumbing
- [x] Ring-3 entry point: enter_user_mode() loads ELF, maps user stack,
  saves kernel context, enters ring 3 via iretq.
- [x] ~~`tss_set_rsp0` exists but RSP0 is a single static kernel stack~~ — done:
  `schedule()`/`process_start_first()` call `tss_set_rsp0(next->kstack_top)`
  before every switch. See "Process & Scheduling" below for the subsystem
  this now belongs to.

### Per-process address spaces
- [x] vmm_destroy_address_space walks the user half, frees mapped frames and
  user page-table pages, and is called on ELF load failure, stack setup
  failure, and normal process exit.
- [x] Per-process PML4 (kernel half aliased 256/511, user half private);
  vmm_map_in/get_phys_in; late CR3 switch; higher-half p_vaddr rejected.

---

## User Interface (EmUI), Compositor & Userland Runtime

Design/usage docs: `docs/EMUI_GUIDE.md` (how to build an app),
`docs/EMUI_INTERNALS.md` (how the toolkit is built), `docs/BUILD_SETUP.md`
(newlib + dynamic linking). Open items only — the toolkit itself, the
compositor, and dynamic linking are all built and live-verified; what
follows are known gaps and rough edges, not missing features. Since
2026-07-23 the OS also **builds** EmUI apps on itself (`test tcc dyn`):
tcc compiles and dynamically links against `libembk.so`, and the widget
renders — and since the same day EmbBuild **builds** one from a manifest
(`test embbuild gui`, `/data/src/ui/build.ebm`), so the rebuild-self claim
covers the GUI, not just static C. See `docs/PORTS.md` § "The GUI wall, and
how it came down" and BUILD.md §6.

- [ ] **The menu bar's drag-and-drop is one-directional** (`user/bin/topbar.c`
  + `em_dock`). You can drag a status chip to reorder it or pull it out of the
  bar to remove it, but you cannot yet drag a *new* item **into** the bar from
  an external tray — that needs either a same-window chip palette the dock
  accepts drops from, or true cross-window drag transfer (the compositor would
  have to hand a drag payload from the tray window to the bar window). The
  reorder/drag-out half is built and live; the drag-in half is the open piece
  of the user's "drag stuff into it or out of it" request.
- [ ] **Only ONE EmUI app has a build manifest** (clockw). home/uidemo/wmdemo
  and the rest of `user/bin/*.c` are still host-built only. This is now
  breadth, not capability — each needs the same three-stanza shape plus its
  own header closure.
- [ ] **Header inputs in manifests are hand-written and can go silently
  stale.** The clockw manifest's first draft listed 8 of its 12 transitive
  headers; it would have built fine and skipped rebuilds on a `backend.h`
  edit. Auto-depfiles (BUILD.md §2.4's deferred item) are the fix; until
  then, derive the list from the include graph rather than from the top of
  the .c file.

- [ ] **Dynamic linker: eager relocation only, no lazy PLT binding.** Every
  `R_X86_64_JUMP_SLOT` is resolved at load time in
  `kernel/arch/x86_64/syscall/elf.c`, not on first call. Simpler and
  currently fine at this app count/size; revisit if process start latency
  becomes a real cost.
- [x] ~~**A residual, unexplained transient `EFAULT` under SMP** in
  `copy_from_user`/`copy_to_user`, masked by retrying `access_ok` up to 8
  times~~ — **the workaround is REMOVED.** The entry described the state
  before two real fixes landed in that exact path:
  - `access_ok` stopped re-reading per-CPU state per page and now captures the
    pml4 **once with IF=0** (the migration race: `this_cpu()` resolved a core,
    a timer IRQ migrated the thread, the deref returned a **foreign** pml4);
  - `vmm_get_phys_in` took `vmm_lock`, closing the multi-level walk against a
    concurrent mapper — the compositor installing shared window pages into a
    client's PML4 while that client walked it (`font.ttf open failed -14`).

  Either could have been the whole cause, so it was **measured, not assumed**.
  `test usercopy` runs both halves of the original repro at once — 6 readers
  hammering the syscall boundary while UI launches force the compositor to map
  into client PML4s mid-flight, on `-smp 4`:

      3496 validations, 0 transient retries, 0 hard faults, deepest 1 attempt

  Retries are now **1 attempt**. A retry loop is not free insurance, it is a
  silencer: it cannot tell a transient from the first symptom of a *new* bug,
  which is exactly how this one survived long enough to be called "residual,
  unexplained". A refusal is now loud (prints the address and length) and
  counted, so a recurrence announces itself instead of being papered over.
  - [ ] ⚠️ Zero occurrences over one run is **evidence, not proof**. If
    `usercopy: access_ok REFUSED ...` ever appears from a program known to
    pass valid pointers, the transient is back — the counters are still there,
    and re-arming the retry is a one-line change. **Do the diagnosis first
    this time.**

- [ ] Still coarse: ONE lock for the whole filesystem. **Measured before
    deciding what to do about it** (`test blockrace` reports it; counters live
    in `struct embkfs_lockstat`). Under 4 concurrent readers:

        fs big lock: 860 acquire(s), 0 recursive, 65 waited (7%), 31464 ms blocked

    **~484 ms blocked per wait** (guest time — TCG inflates it, but the ratio
    and the per-wait magnitude are the signal). So the contention is real, not
    theoretical. Two conclusions, both of which change what "fix it" means:
    - **Per-VOLUME locking is worthless here.** One EMBKFS volume carries all
      traffic (`/`; `/run` is epfs, a different filesystem). Splitting the lock
      per volume splits nothing. This was the obvious-sounding fix and the
      measurement killed it.
    - **The cost is HOLD TIME, not granularity** — and the hold time was the
      *single-slot* whole-object cache thrashing. **Fixed:** `rcache` is now
      4-way (LRU, 12 MB budget). Controlled A/B, same build and counters, only
      `EMBKFS_RCACHE_SLOTS` changed:

      | | 1 slot | 4 slots | |
      |---|---:|---:|---|
      | rcache misses | 63 | 8 | 7.9× fewer |
      | …self-inflicted evictions | 61 | 4 | **96% of 1-slot misses** |
      | device bounce reads | 228 | 16 | 14× fewer |
      | **total ms blocked on the fs lock** | **172813** | **79108** | **2.2× less** |
      | ms per wait | 1585 | 437 | 3.6× shorter |

      96% of the old misses were evictions the cache inflicted on *itself* —
      two readers alternating between two files evicted each other on every
      switch, and each miss re-decoded a whole object while holding the global
      lock. Note the wait *count* went UP (109 → 181): threads now make more
      progress so they queue more often, while each wait is 3.6× shorter. Wait
      count alone would have read as a regression; total blocked time is the
      honest metric.
    - **The other two caches were checked and left alone.** `ecache` (extent
      map) and `icache` (inode) are *also* single-slot, and "same shape as the
      rcache bug" looked obvious. Measured instead — with a real >8 MB workload
      (`cxxdemo.elf`, the only file on the image past `RCACHE_MAX`, which is
      what drives the ecache path at all):

          ecache: 1608 hit, 11 miss  |  icache: 10784 hit, 11 miss  (1 slot each)

      **99.3% and 99.9% hit on one slot.** No thrash, so no change. The
      hypothesis was wrong, which is the useful outcome — an N-way rewrite of
      either would have been churn with a plausible story attached.
      - ⚠️ **Bounded claim:** there is only ONE >8 MB file on the image, so two
        big files never alternate — the pattern that *would* thrash ecache is
        untested because it cannot be built here today. If a second large file
        appears, re-run `test blockrace` before assuming ecache is still fine.
    - **The rcache counter was lying, and it was my counter.** That same run
      reported `rcache: 806 hit, 9186 miss` — an apparent 8% hit rate on a cache
      that had just been shown to work. Nearly all of those "misses" were reads
      of the 9 MB file, which is **over `RCACHE_MAX` and therefore uncacheable
      by policy** — a bypass, not a failure (predicted 9169, reported 9186; the
      17-difference is the real misses). Now counted separately. A broken
      instrument is worse than none: this one invited "fixing" a cache that was
      already fine.
    - Prerequisite for per-OBJECT locking, if it is ever wanted: **34 shared
      `static uint8_t [4096]` scratch buffers in embkfs.c** would have to
      become per-caller first — *they* are what the lock actually protects
      (see the big-lock comment). That is the real price, and it should not be
      paid until shortening the hold time has been tried.
    - Also latency, not correctness: `test embkfs timestamps` holds the lock
      across a ~2s RTC-resolution wait, and any long selftest blocks unrelated
      fs I/O for its duration.
  - ⚠️ **The rule that keeps it safe:** never hold this lock across a wait on
    *another thread* doing fs I/O (spawn-and-wait, say) — the child's ops take
    the same lock from a different thread and would block on a holder that is
    itself blocked on the child. This is exactly why the selftest dispatcher
    does not wrap commands wholesale; the leaf entry points lock instead.
- [x] ~~**`mkfs_embkfs.py`'s single-leaf image builder overflows past ~7
  packed files.**~~ — done: `build_btree()` (with `pack_items_into_leaves()`)
  greedily packs metadata items into as many level-0 leaves as they need and
  adds a level-1 internal root above them when there's more than one leaf; the
  root stays at block 17 so the superblock pointer is stable. `make_image`
  auto-splits (a two-pass build learns the metadata-block count, then places
  the data region after it — leaf packing is invariant to data-block
  placement since an extent's `disk_block` is fixed-width). The
  `wgyehkb.txt`/`illoeuw.txt` collision-chain fixtures are restored to both
  images. Verified: the oracle passes, and the kernel MOUNTS + DESCENDS the
  level-1 tree live (`root node OK level 1 (internal) nritems 2`), lists all
  13 files, resolves the collision chain, and launches an app read entirely
  through the 2-level tree — no exceptions. `make_tree_image` keeps its
  deliberate forced 2-leaf split as a fixed descent regression. *(Remaining
  headroom: one internal node holds ~72 leaf slots; a level-2 tree isn't
  implemented and `build_btree` raises if an image ever exceeds that — far
  off at current app counts.)*
- [x] ~~**Overlay/modal flagged unstable in live interaction.**~~ —
  root-caused and fixed. The "modal doesn't render / half-drawn scrim" was
  NOT a hang or a tree/layout bug (it renders correctly on the host, and the
  live dirty-rect union was already the full window). The scrim is a
  **full-window semi-transparent solid** (`a=0.55`), and `cpu_draw_rect`'s
  fast interior path was **opaque-only** (`a >= 0.999`) — so every one of the
  ~425k scrim pixels took the per-pixel float `blend_over`. Under TCG that's
  the documented "float-per-pixel = poison": one modal frame took ~25s, so
  screenshots caught it mid-render (scrim covering only the top rows, dialog
  not reached yet). Fix: a **constant-alpha integer-LUT fast path** in
  `cpu_backend.c` (`out = round(dst*(1-a)) + round(src*a)`, carry-safe, four
  256-entry LUTs) — benefits EVERY translucent solid, not just the scrim.
  Result: first modal frame ~3s render (~8x faster), renders cleanly.
  Verified with an always-open-modal `EM_APPLICATION` app (retained runtime)
  live — dialog + scrim correct, no corruption, no exceptions; host render
  pixel-identical, all 6 UI suites pass. Also fixed alongside: the retained
  runtime (`em_app.c`) now includes `em_overlay_active()` in its build gate,
  so a modal in an `EM_APPLICATION`/`EM_WIDGET` app keeps building frames
  while it's up (needed for the scrim-dismiss debounce `g_ov_frames >= 3` to
  advance, and for any modal animation).
- [x] ~~**Adding a new app requires three manual registration points.**~~ —
  done: the build system now auto-discovers apps. A Makefile pattern rule
  (`build/%.elf` over `$(EMUI_APPS) := $(wildcard user/bin/*.c)` minus the
  special-linked `init.c`/`hello.c`) builds any `user/bin/*.c` as a
  dynamically-linked EmUI app, and `mkfs_embkfs.py`'s
  `discover_userland_objects()` globs `build/*.elf` and packs them all.
  Adding an app is now just "drop `user/bin/foo.c` in, `make embkfs.img`" —
  verified by dropping a throwaway `probeapp.c` and watching it compile,
  link, and land on the image with zero build-file edits. *(Minor residual:
  deleting an app's `.c` leaves a stale `build/*.elf` that keeps getting
  packed until `make clean` — adding is free, removing needs the clean.)*
- [x] ~~**No real TTY.** The framebuffer console is output-only (no
  scrollback, no line editing)~~ — **a real TTY exists** (`kernel/tty/`): a
  line discipline with cooked and raw modes, line buffering, echo, backspace
  erase (which refuses to back over the prompt), `^D` EOF on an empty line,
  and `^C` cancellation returning `-EMBK_ECANCELED`. The console fd's read op
  in `fs/fd.c` is a thin shim over `tty_read()`, so everything a terminal does
  that a raw keyboard does not lives in one place. Proven by `test tty`
  (injection-driven, so it needs no real key presses): **OK**.
  - The entry bundled two different things and only one of them is still open:
    **line editing** is done (input side), **scrollback** is not (output side).
    `console.c`'s `scroll_up()` is overflow scrolling — it advances the screen
    when text reaches the bottom — not a history buffer you can page back
    through. Those are separate features and the original wording made them
    look like one gap.
  - [ ] **No scrollback history.** Output that scrolls off the top is gone;
    there is no saved-line ring to page back through. Wanted for reading a long
    boot log or a command's output on the framebuffer console — the serial log
    is the current workaround, which is why this has never bitten hard.
- [ ] **`home.elf` plays an informal init/service-manager role** (spawns and
  tracks every app the user launches, including the clock widget at boot)
  without being a real PID-1/service-manager abstraction — fine for a
  single-user desktop today, a real gap if multiple always-running services
  are ever needed.

---

## Process & Scheduling

Full phased spec, comparative analysis (Linux/Windows/BSD/XNU), and every bug
below in detail: `docs/architecture/process-and-scheduling.md`. Current
mechanism: `thread_table[MAX_THREADS=256]` (schedulable unit) split from
`process_table[MAX_PROCESSES=64]` (resource owner), real SMP (per-CPU
`current_thread`, AP bring-up, one global `g_sched_lock` held across the
context switch), timer-driven preemptive priority-band round-robin (100Hz
LAPIC tick, 4 bands + aging), wait queues for blocking, an uncatchable
`process_kill`, real parent-blocked `process_wait()`, per-process fd
tables, per-process ring-3 handle tables, and joinable ring-3 threads
(`thread_create_user`/`thread_join`) sharing one process's address space.
`process_create()` builds a fresh address space + page-mapped
guarded kernel stack from an ELF path and fabricates a first context that
lands in `process_trampoline`, which `iretq`s to ring 3. The kernel's own
interactive shell (`main.c`) is itself a real process now
(`process_adopt_current()`), with `run`/`ps`/`kill`/`wait`/`nice` commands.
Phases B, C, and D (see roadmap in the architecture doc) are all now
substantially complete.

- [x] ~~Ring-3 entry `#GP` on every process start~~ — `process_trampoline`'s
  inline-asm `iretq` frame pushed the literal `$2` instead of the `%2`
  operand (the real `0x23` selector) for CS. Loading CS from the resulting
  null-descriptor selector faulted immediately. Fixed.
- [x] ~~`kstack_top` truncated to 16 bits~~ — `struct process::kstack_top` was
  `uint16_t` but stored a 64-bit heap address; `tss_set_rsp0()` read the
  truncated field. Would have corrupted RSP0 on the first ring-3 interrupt.
  Now `uint64_t`. Fixed.
- [x] ~~Zombie processes never reclaimed~~ — nothing transitioned
  `PROCESS_ZOMBIE` back to `PROCESS_UNUSED`; after exactly `MAX_PROCESSES`
  exits, `process_create` would fail forever. Fixed: `process_reap()`, deferred
  one `schedule()` call behind the actual exit (can't free a stack still being
  executed on). Not yet exercised in practice — `main.c` only ever creates one
  process today, so the deferred-reap path has never actually fired; needs
  either a second process or the `test sched reap` selftest below to prove it.
- [x] ~~No kernel-stack guard page~~ — `alloc_kernel_stack` was a flat
  `kmalloc`, silently corrupting whatever sat next to it on overflow. Fixed:
  `vmm_alloc_kernel_stack`/`vmm_free_kernel_stack` page-map the stack with an
  unmapped guard page directly below it.
- [x] ~~Kernel-stack region invisible in the first process's own page
  tables~~ — introduced *while fixing* the guard-page bug above: the new
  region was placed in a PML4 slot untouched before boot.
  `vmm_create_address_space()` shares the kernel half by copying PML4 entries
  *by value* at creation time, not by live reference — a slot that's
  not-present at that moment stays not-present in that process forever, even
  after the kernel's own table fills it in moments later. `#PF` on first
  touch, which can't even push its own fault frame (same region backs
  `TSS.RSP0`) → `#DF`. Fixed by reusing `MMIO_BASE`'s already-populated PML4
  slot (256 GiB in, clear of the real MMIO bump allocator) instead of a fresh
  one.
- [x] ~~No preemption~~ — done: `lapic_timer_handler` (`lapic.c`) now calls
  `schedule()` after `lapic_send_eoi()`, at the existing 100Hz tick rate.
  Genuine timer-driven round-robin, verified via `test sched roundrobin`
  (two kthreads interleave without either calling `sys_yield`/`sys_exit`).
- [x] ~~No blocking / wait queues~~ — done: `struct wait_queue { struct
  process *head; }` plus an intrusive `wait_next` link on `struct process`.
  `wait_queue_block()` marks the caller `PROCESS_BLOCKED`, links it in, and
  calls `schedule()`; `wait_queue_wake_one`/`wait_queue_wake_all` unlink and
  mark `PROCESS_READY`. Verified via `test sched roundrobin`'s blocking
  variant.
- [x] ~~No priority~~ — done: 4 fixed bands (`PRIORITY_REALTIME`/
  `INTERACTIVE`/`NORMAL`/`BACKGROUND`, 0=highest), round-robin within a
  band, aging (`PRIORITY_AGE_TICKS = 20`, ~200ms/band) bumps a starved
  READY process up one band so a busy high band can't starve a low one
  forever. Verified via `test sched priority`. Note the aging period was
  deliberately kept short (200ms, not a "nicer-looking" multi-second
  value) — the worst case (a BACKGROUND process recovering all the way to
  REALTIME contention) needs `SCHED_PRIORITY_BANDS - 1` full periods, and
  at a multi-second period that's long enough for even the kernel's own
  shell (PRIORITY_NORMAL) to visibly freeze if a spawned child ever ran
  busier and higher-priority than it.
- [x] ~~No uncatchable kernel-level kill~~ — done: `process_kill(pid)`
  (`process.c`) forces `PROCESS_ZOMBIE` regardless of current state, unlinks
  the target from any wait queue it's blocked on, and reaps it immediately
  unless it's the currently-running process (in which case it defers via
  the existing `g_pending_reap` mechanism, same as a normal exit). Matches
  `docs/ARCHITECTURE.md` §3.3's requirement that a process which never
  cooperates can still be stopped. Verified via `test sched kill`.
- [x] ~~No per-process file descriptor table~~ — done, see "File
  descriptors" under Filesystem above (`struct process::fds`, `fd_table()`
  helper in `fs/fd.c`).
- [x] ~~No `sys_wait`/`sys_spawn` syscalls~~ — done, and since upgraded past
  the original busy-poll: `sys_spawn` calls `process_create()` on a
  user-supplied path (via `copy_string_from_user`) and returns a
  **capability handle**, not the raw pid (see the handle-model bullet
  below); `sys_wait` resolves the handle and calls `process_wait()`, which
  genuinely **blocks** the caller on the target's parent's `child_wait`
  queue until that specific child exits or is killed — no more polling.
  `sys_yield` wired to syscall number 3; also added `sys_getpid` and
  `sys_kill`. Verified via a temporary `user/init.c` scaffold exercising
  the full handle lifecycle (spawn → handle, wait on a bad handle →
  `-EINVAL`, wait on the real handle → correct exit code, wait on the
  now-freed handle again → `-EINVAL`); reverted after verification.
- [x] ~~Real blocking `sys_wait` (was Phase D in the architecture spec)~~ —
  done: `process::parent`/`parent_pid`/`zombie_head`/`zombie_next`/
  `child_wait` (`process.h`) implement the hand-off. `parent_pid` exists
  specifically because `parent` is a raw pointer into a slot that gets
  recycled after reaping — checking `state != PROCESS_UNUSED` alone can't
  tell "still my real parent" from "an unrelated new process reused the
  slot," a real bug caught while building this (`parent_is_alive()`).
  **Found and fixed a real deadlock getting this exercised for the first
  time**: `schedule()`'s zombie hand-off ran *after* the "nothing else
  runnable" early return, but the one scenario `process_wait()` creates is
  exactly a parent sitting `BLOCKED` (not READY/RUNNING) with nothing else
  runnable at that moment — so the hand-off that would wake it never ran.
  `test sched wait` hung on its very first run; fixed by moving the
  zombie-handling decision before the "is there anything else to run"
  search. Full postmortem: `docs/architecture/process-and-scheduling.md`
  §16, Bug 11.
- [x] ~~Ring-3 process handles (`docs/ARCHITECTURE.md` §3.4/§3.5)~~ — done:
  each process has its own `struct proc_handle handles[PROC_HANDLE_MAX]`
  table; `sys_spawn` allocates a handle pointing at the new child's real
  pid and returns the handle, `sys_wait`/`sys_kill` resolve their handle
  argument back to a pid before acting. Closes the confused-deputy gap a
  raw pid argument left open (any ring-3 process could otherwise name any
  pid it could guess) — this was explicitly flagged as a known, deliberate
  divergence earlier in this file and is now closed.
- [x] ~~No `sys_kill`~~ — done: exposes the already-existing (Phase B)
  `process_kill()` to ring 3 via the handle table. Does not free the
  handle — the caller still needs it to `sys_wait()` afterward and collect
  the "killed" exit code (-1).
- [x] ~~Kernel has no way to actually launch/manage processes
  interactively~~ — not originally on this list, but became a real gap
  once `main.c`'s old single-hardcoded-process auto-launch
  (`process_start_first()`, a one-way hand-off) was removed: nothing
  called `process_create` at all anymore, and the removal left behind a
  stray unconditional inner `for (;;) hlt` loop that trapped the shell
  after at most one keystroke. Fixed: `main.c` now calls
  `process_adopt_current()` to make the interactive shell itself a real,
  permanently-scheduled `current_process` (not a one-way hand-off), with
  new `run <path>` / `ps` / `kill <pid>` / `wait <pid>` / `nice <pid>
  <priority>` shell commands calling straight into `process.c`'s
  kernel-internal API (no handle indirection needed there — trusted code,
  not a sandboxed ring-3 caller). Verified interactively in QEMU via
  monitor-injected keystrokes: `run /init.elf` spawns a real child as the
  shell's sibling, `ps` shows both with correct PID/PPID/state/priority,
  `wait` blocks and correctly retrieves the real exit code, `kill`/`wait`
  on an already-exited or nonexistent pid are safe no-ops.
- [x] ~~`process_create`/`proc_alloc` both increment `next_pid`~~ — fixed:
  removed the redundant `proc->pid = next_pid++` from `process_create()`;
  `proc_alloc()` is now the sole assigner.
- [x] ~~No automated selftest~~ — done: `test sched roundrobin`, `test sched
  kill`, `test sched reap`, `test sched stackguard` (`process_test_*` in
  `process.c`, wired into `selftests.c`), matching the four selftests
  `docs/architecture/process-and-scheduling.md` §12 specifies. `reap` in
  particular exercises the deferred-reap path noted above, which nothing
  had actually triggered before (main.c previously only ever created one
  process). Two more added alongside priority scheduling and real blocking
  wait: `test sched priority` (a REALTIME kthread dominates a BACKGROUND
  one, but aging still rescues the BACKGROUND one from total starvation)
  and `test sched wait` (process_wait()'s two hand-off paths: normal exit
  and uncatchable kill).
- [x] ~~Single core only~~ — done (SMP phase, see `docs/architecture/
  process-and-scheduling.md` §13). `current_thread` is a real per-CPU
  field (`cpu_table[]`); a single global `g_sched_lock` guards
  `thread_table`/`process_table`/`current_thread`/`pending_*_reap`, held
  ACROSS the context switch itself (released only by whichever thread
  resumes on the far side) — the load-bearing part, not just "add a
  lock." Per-CPU run queues remain deliberately deferred (§8/§17) until
  this single lock is measured to bottleneck.

### New bugs found while building preemption/syscalls/spawn (not on this list originally)
- [x] **IF=0 leak into every voluntary-block wake** (ledger Bug 26, the
  full write-up lives in docs/architecture/process-and-scheduling.md).
  `sched_block_current_locked()` sleepers resumed with interrupts off whenever
  a *timer-initiated* switch-in woke them; the leak reached `ata_wait_irq`'s
  bare `hlt` and froze the machine. Ten call sites had accumulated without the
  IF-restore Bug 25 declared a "documented rule" — the fix is now structural
  (`sti` inside the primitive itself), `ata_wait_irq` uses `sti;hlt` + a loud
  canary, and `test writestorm` (200 write/unlink cycles + IF probes, any leak
  fails) pins it. Found while porting tcc headers: the probabilistic freeze
  masqueraded as "tcc hangs on-OS" until the QEMU monitor's `info registers`
  showed `RIP` at ata.c's `hlt` with `RFL.IF=0`.
- [x] **RFLAGS.IF corruption in context switch.** `kernel_ctx_switch`/
  `kernel_ctx_save` (`kcontext.asm`) capture RFLAGS via `pushfq`/`pop rax`
  and are always called from inside an interrupt-gate ISR (timer IRQ,
  `int 0x80`), which auto-clears IF on entry — so the *live* flags always
  read IF=0 at the capture point, regardless of the outgoing process's true
  state. Saving that corrupted snapshot meant every resumed process came
  back with interrupts permanently masked, hanging after its first
  preemption. Found via `test sched roundrobin` (scheduler hung after one
  cycle). Fixed by forcing `or rax, 0x200` (set IF) before storing to the
  RFLAGS field in both functions — safe because reaching that save point at
  all proves the process had IF=1 moments earlier (a maskable IRQ can't
  fire with IF=0, and ring-3 can't execute cli/sti, it's privileged and
  would #GP).
- [x] **`int 0x80` leaves IF=0 for the whole syscall.** Same root cause as
  above, different symptom: any syscall that blocks on a hardware
  completion IRQ (disk I/O inside `sys_open`/`sys_read`/`sys_spawn`'s ELF
  load) hung forever because the IRQ that would satisfy it could never
  fire. Found while verifying file I/O syscalls. Fixed with `sti` as the
  first instruction of `syscall_dispatch()`.
- [x] **`proc_alloc()` race between allocation and initialization.** Found
  while verifying `sys_spawn`: with real preemption now active, a second
  `process_create()` call (from a live syscall context) could be preempted
  mid-setup during its slow ELF-load disk I/O — but `proc_alloc()` had
  already marked the new slot `PROCESS_READY` immediately, so the scheduler
  could pick a half-initialized PCB (no pml4/kstack/ctx yet) and crash.
  Fixed: `proc_alloc()` now marks the slot `PROCESS_BLOCKED` instead;
  `process_create()`'s own final step is the only place that sets
  `PROCESS_READY`, after everything is actually built.
- [x] **PCB leak on `process_create()` error paths.** Found via code review
  while fixing the race above: none of the four early-return failure paths
  (address-space creation, ELF load, user-stack alloc, kernel-stack alloc)
  reset `proc->state` back to `PROCESS_UNUSED`, so a failed `process_create`
  permanently leaked the PCB slot. Fixed by adding the reset to all four
  paths.
- [x] **`schedule()` was reentrant against the timer ISR when called from
  syscall context.** Found by re-reading the finished preemption/syscall
  work (not by a crash): `syscall_dispatch()`'s `sti` (needed for the
  `int 0x80` fix above) meant `schedule()` calls from `sys_exit`/`sys_yield`
  ran with IF=1, so a timer IRQ could land mid-`schedule()` and re-enter it
  while the outer call was still mutating shared scheduler state. Fixed:
  `schedule()` now `cli`s at entry (saving the caller's IF) and restores it
  on every early-return path, the same `pushfq`/`cli`/conditional-`sti`
  idiom `cpu/spinlock.c` already uses elsewhere.
- [x] **`schedule()`'s zombie hand-off deadlocked the first real use of
  blocking `process_wait()`.** The hand-off/auto-reap decision for a dying
  process was only reachable *after* confirming some other runnable
  process existed — true for every scenario that existed before
  `process_wait()`, but false the instant a parent can be `BLOCKED`
  (not READY/RUNNING) waiting specifically for this exit with nothing else
  runnable. `test sched wait` hung on its very first run. Fixed by moving
  the hand-off decision before the "anything else to run" search.
- [x] **`parent` pointer could alias an unrelated process after PCB slot
  reuse.** Found while implementing the parent/child tracking above:
  `struct process::parent` is a raw pointer into the static process table;
  checking only `parent->state != PROCESS_UNUSED` to mean "still alive"
  breaks the moment the real parent exits, is reaped, and a totally
  different new process gets allocated into that same recycled slot — the
  state check would wrongly read as "alive" for the wrong process, handing
  an exit off to a stranger. Fixed by also storing `parent_pid` (the
  parent's pid at the moment `parent` was set) and comparing
  `parent->pid == parent_pid` too (`parent_is_alive()`).

---

## Core / Library

- [x] ~~Refactor: kprintf + snprintf share one `format_string` core~~ — done
  (the entry said so while staying checked-open).
  - [ ] Optional extras, only if something needs them: `%b` (binary) and
    field-precision. Both wrappers would benefit for free.
- [ ] kstring: only the subset in use is implemented. Add more (strstr, strtok,
  memchr, etc.) as needed — don't pre-build the whole libc.

---

## Architecture (big-ticket, later)

- [x] ~~SMP — multi-core bring-up~~ — done: AP startup (INIT-SIPI-SIPI,
  `kernel/cpu/smp.c` + `ap_trampoline.asm`/`ap_entry.asm`), per-CPU data
  (`kernel/cpu/percpu.c/h`, `cpu_table[]`), per-CPU LAPIC/GDT/TSS. Full
  detail: `docs/architecture/process-and-scheduling.md` §13. Per-CPU RUN
  QUEUES specifically are still deferred (see Process & Scheduling above)
  — a real, separate, deliberately-scoped-out next step.
- [ ] Portability / HAL discipline — keep new upper-layer code arch-neutral
  (no inb/outb, no direct page-table pokes, no x86 asm in logic); route
  arch-specific operations through arch_* interfaces. Real ARM64 port is a
  later dedicated campaign — don't pre-abstract against a single architecture.
- [x] ~~**embbuild** — the native build tool (the make-equivalent)~~ —
  **BUILT AND SHIPPED**, not merely designed. `shell/tools/embbuild.c`; proven
  by `test embbuild` (cases a–f including the §3 `/system` install refusal),
  `test embbuild self` (EmbBuild rebuilds EmbBuild, cross-checked by a
  two-implementations oracle), `test embbuild shell` (the shell rebuilds the
  shell and the OS adopts it), and `test embbuild gui` (a `libembk.so` EmUI app
  built from a manifest and adopted). The design record below is kept because
  the reasoning is still the justification for the shape:
  manifest format, content-hash stamps, stage/adopt via atomic rename,
  `/data/build/` tree, `test embbuild` acceptance).
  The fork was audited and ratified: a **native structured tool**, not a make
  port. The deciding facts, each verified against the tree: every userland
  recipe is one argv = one `spawn()` (no `/bin/sh` buys nothing); the host
  Makefile cannot run on-OS regardless (no compatibility payoff); the RTC's
  one-second mtimes against millisecond TCC compiles make timestamp staleness
  structurally false-fresh (→ staleness by **content hash** of inputs + argv).
  Shape: targets as typed records (name, sources, `-I`, objects, link inputs,
  install path), recipes as argv arrays, `/data/src/<project>/` as the source
  convention, the ABI as ambient constants — the schema `test tcc tally`
  already executes hand-unrolled. Deliberately absent from v1: variables,
  pattern rules, parallelism (the real graph is ~50 explicit nodes). Honest
  rebuild-self scope with TCC: static newlib C, **and `libembk.so` GUI apps
  since 2026-07-23** (`test tcc dyn`); still no `__thread` (no linker
  scripts/PT_TLS), no C++, kernel wants GCC.
  make itself arrives later as opt-in compat with the foreign-tree ports
  story. Nice detail available: build it on the sval SDK, which is on-image
  and already proven self-rebuildable.
  - [x] v1 staleness detail: hashing file bytes in userspace — **that is what
    shipped** (CRC32C over inputs + argv + tool version).
    - [ ] Still open, and still "pulled by need": exposing a cheap content
      identity from EMBKFS's CoW generation machinery, so a stamp need not
      re-read the file to know it changed.
  - [x] Prerequisite DONE: separate compile-then-link with tcc-produced
    objects, proven live (`test tcc tally`). *(Was checked-open while its own
    text said "already DONE".)*

## Userspace authority (v2), packaging & toolchain

The userspace authority model shipped end-to-end — see `docs/USERSPACE_v2.md` for
the design and the live proofs (`test namespace`, `ns_spawn_test`,
`mu_isolation_test`). Open items:

- [x] ~~UP1 init as root of authority; UP2 per-process namespaces + sealed /system
  RO; UP2b spawn-grant narrowing; UP4 declared per-app namespaces; UP3 multi-user
  as namespace domains~~ — **all shipped.**
- [ ] **UP3b — live multi-user.** Login/session switching between users, and
  confining the *desktop* to a non-owner user. Needs the apps' broad grants (chiefly
  `files.ns`'s `rw /`) tightened to the user's home, and the file manager's start dir
  moved into `$HOME`.
- [ ] **NS_BIND rebind** — a namespace grant where the child prefix ≠ the source
  path (e.g. `/downloads` → `/data/users/<u>/downloads`). Needed for well-known
  folders as grant targets (USERSPACE_v2 §11) and for `$HOME`-style rebinds.
- [ ] **`adduser`** — user *creation* (homes are baked by mkfs today). Its skeleton
  seeds grant-target folders, not a `Documents/Downloads` taxonomy (§11).

### Packaging & SDK (designed, not built — `docs/PACKAGING_AND_SDK.md`)

- [x] ~~**PK1** — the package manifest format + local `pkg install`.~~ **DONE,
  metal-proven (`test pkg`).** Manifest (§3) parser + EMBX reader/`build_id`
  verifier in `user/pkg/`; `pkg verify|install|run|list` (`user/bin/pkg.c`).
  `install` recomputes the EMBX `build_id` (SHA-256 with `build_id`+`header_checksum`
  zeroed) and matches it to the header *and* the manifest, cross-checks the
  manifest `caps:` against the EMBX cap table + `abi`, presents the declared
  authority, and adopts the bundle into `/data/apps/<name>/` writing the `.ns` home
  enforces + a `/data/pkg/registry` entry. `pkg run` spawns an installed app under
  EXACTLY its declared caps (SET_CAPS) + namespace (NS_BIND) — `pkgprobe` self-checks
  it holds only `filesystem`, reaches `/system`, and cannot name `/data/users`; a
  tampered bundle (build_id fails to recompute) is refused. Bundle+manifest built by
  `tools/embx/mkembx.py` + `tools/embx/mkpkg.py` (manifest derived from the EMBX, so
  caps/build_id are consistent by construction). **Scope honestly deferred:** direct
  copy, no atomic snapshot/rollback (no userspace EMBKFS snapshot API yet → PK3); no
  signing (PK3); no network (PK4). The manifest is PK1's source of truth until PK2
  makes `build.ebm` the source.
- [x] ~~**PK2** — the SDK generator.~~ **DONE (generator), metal-verified.**
  `tools/embx/pkggen.py`: ONE `.pkgspec` (name/version/caps/grant) → all three
  views — the EMBX cap table (mkembx/EmbLD bakes it), the `.ns` (from the grant),
  and the package manifest (mkpkg re-derives caps from the EMBX so it cannot
  drift) — consistent BY CONSTRUCTION. Proven: adding `network` to the one spec
  line makes it appear in both the binary's cap table AND the manifest; there is
  no way to build a bundle whose declared authority disagrees with itself (§4).
  `pkgprobe`'s authority is now declared once in `user/pkg/pkgprobe.pkgspec`
  (was scattered across `mkembx --cap` + `mkpkg --ns` flags); `test pkg` runs
  green on the pkggen-produced bundle.
- [x] ~~**PK2b** — on-device package generation.~~ **DONE, metal-proven
  (`test pkgbuild`).** `user/pkg/embxgen.c` is a C EMBX writer (repackage a linked
  ELF → EMBX with a cap table) — a faithful port of `mkembx.py`, host-verified
  BYTE-IDENTICAL, so the build_id matches. `user/bin/pkgbuild.c` reads ONE
  `.pkgspec` + an ELF and emits all three views (EMBX/`.ns`/manifest) ON THE OS.
  On-device builds are unsigned, so `pkg install --local` adopts a dev build (a
  present signature is still verified; caps/build_id/re-negotiation still apply).
  `test pkgbuild`: pkgbuild generates pkgprobe's bundle from the staged
  `.pkgspec` + ELF → `pkg install --local` → `pkg run` confined — the whole
  SDK→pkgmgr loop on the metal.
  - **Structured EmbBuild `package:` stanza — DONE (`test pkgstanza`).** EmbBuild
    (`shell/tools/embbuild.c`) gained a `kind: package` with `version`/`caps`/
    `grant` fields: it writes a `.pkgspec` from them and drives `pkgbuild`, with the
    fields folded into the rebuild stamp. A `build.ebm` declares an app's authority
    inline; `embbuild` produces the bundle on-device. Metal: embbuild package stanza
    → bundle → `pkg install --local` → run confined.
- [x] ~~**PK3** — signing + update/rollback + registry.~~ **DONE, metal-proven
  (`test pkg`, 8 checks).**
  - **Signing**: `tools/embx/pkgsign.py` signs each manifest (ECDSA P-256 over the
    canonical manifest — the file minus its `signature:` line, covering name/
    version/abi/build_id/caps/namespace/provides). `pkg` verifies against the
    trusted key in `user/pkg/pkgkey.h` (our own `ecdsa_verify`) and refuses
    unsigned/altered/wrongly-keyed manifests. Signing is part of `pkggen` (dev key
    `tools/embx/pkgkey_dev.pem`). Trust is in the signature, not the channel (§9.2).
  - **Update + rollback + authority re-negotiation**: `pkg install` over an
    installed version retains the previous bundle at `/data/pkg/versions/<name>/
    prev/` as the rollback point (§6 — each app is self-contained, so its 3 files
    ARE the rollback point; no whole-FS snapshot needed), and REFUSES an update
    that WIDENS caps or namespace (`+ NEW capability: network`) unless
    `--allow-widen` — a new version cannot silently widen its reach.
    `pkg rollback/remove/info` complete the set.
  - **Proven**: `test pkg` install → run confined → tampered-binary reject →
    bad-signature reject → update to v1.1 → rollback to v1.0 → widening update
    REFUSED → same update with `--allow-widen` accepted.
  - *EMBKFS-snapshot-backed update — ASSESSED, deliberately not built for pkg.*
    The kernel API exists (`embkfs_snapshot_create/rollback/delete`, tested) but
    EMBKFS snapshots are **whole-volume** ("a frozen root block_ptr", spec §6b):
    a rollback restores ALL of `/data`, not one package — the wrong granularity
    for per-package rollback, which the **retained-bundle** approach above does
    correctly. A whole-volume snapshot's only `pkg` benefit would be install
    *atomicity* (snapshot → install → rollback-on-failure), and that means rolling
    back the LIVE mounted volume mid-session (risky) for marginal value on a
    self-contained-bundle install. Verdict: not worth a risky live-volume rollback
    in `pkg`. If snapshots are wanted as a GENERAL userspace capability (backups,
    experiments), expose the syscall deliberately for that — a separate feature,
    not a packaging one.
- [x] ~~**PK4** — the git registry.~~ **DONE, metal-proven (`test pkgregistry`).**
  The registry is a git repo (**github.com/teo1747/emblink-packages**): one dir per
  package holds its SIGNED manifest + the EMBX bundle, plus a top-level `index`.
  The OS clones it over HTTPS with our own git-over-TLS client (`gitclone` →
  `/data/registry`), then `pkg install /data/registry/<name>` verifies the
  signature against the trusted key **on arrival** and adopts — a compromised host
  cannot inject a bad binary (§9.2): trust is in the signature, not the channel.
  `pkg` has the apt-like UX: **`pkg sync [url]`** clones the registry into
  `/data/registry` (spawning `gitclone`, which inherits its CAP_NETWORK) and
  remembers the url; **`pkg install <name>`** resolves a bare name from the synced
  index and installs it (a `/path` still installs a local bundle). `test
  pkgregistry` (metal): `pkg sync <url>` → `pkg install pkgprobe` (signature valid
  on arrival) → `pkg run` confined — the transport is the very git-over-HTTPS
  client from the git arc.
  - **Release-asset binaries (§9.1) — DONE.** The registry `main` ships only the
    signed manifest + a `<name>.url`; the binary lives elsewhere (here an `assets`
    branch, served raw over HTTPS). `pkg install` fetches it with `wget` and checks
    the fetched binary's `build_id` against the SIGNED manifest — a tampered host
    cannot inject a bad binary (§9.2). Metal-proven: wget authenticates
    raw.githubusercontent (Let's Encrypt → our ISRG Root X1), 200/89288 bytes,
    signature valid, adopted.

### Toolchain (EmbCC/EmbLD — tracked in full in `EmbCC/docs/todo.md`)

- [ ] **A1** — an on-OS `.asm` assembler (grow EmbCC an Intel/NASM front-end), to
  drop `nasm`. THE blocker for EmbBuild-builds-the-kernel (see `BUILD.md` §12). *In
  progress.*
- [ ] **L1** — EmbLD linker-defined symbols (`kernel_end`) so the kernel links with
  no external tools and no diagnostic stub.
- Usage: `docs/TOOLCHAIN.md` (building for/on the OS) + `EmbCC/docs/USAGE.md` (CLI).
---

## TLS / HTTPS (`docs/TLS.md`, Phase 28)

The OS speaks **authenticated TLS 1.3** on its own crypto (T1–T4 done, T5 partial).
This section is the honest ledger of what was deliberately **left unbuilt** — the
parts skipped to keep each phase shippable. Green today: `make test-tls-crypto`
(15 host suites) + on-OS `test tls` (Cloudflare/EC), `test tls rsa` (Let's
Encrypt/RSA), `test wget https`, `test pypi`.

### T5 — the consumers
- [x] ~~**Install a real PyPI package over HTTPS.**~~ **DONE via `pkgfetch`**
  (commit `f476790`): a native installer (`user/bin/pkgfetch.c` + our own
  `user/lib/inflate.c` DEFLATE + `user/lib/unzip.c`) fetches a package's PEP-503
  index + wheel over authenticated libtls and unpacks it into
  `/data/py/site-packages`. `test pkgfetch` installs `six` from pypi.org /
  files.pythonhosted.org on the metal; `PYTHONPATH=… python -c 'import six'` then
  works. This was chosen over rebuilding CPython because the proxy path is blocked
  (no loopback in the net stack).
  - **Scope / still open:** pure-Python wheels only (no C-extension compile step);
    one package at a time (**no dependency resolution** — a package needing deps
    won't pull them); picks the last `*-none-any.whl` in the index (simple "newest"
    heuristic, not full PEP 440 version sorting); no wheel hash/signature check
    beyond TLS transport auth.
- [ ] **Real `pip` (the tool)** — a multi-brick foundation (Python had NO
  networking at all: no socket headers, `socket()`=ENOSYS, `_socket`/`_ssl`
  unbuilt, pip absent). Progress:
  - [x] **Brick 1 — POSIX BSD sockets in the newlib libc** (commit `64afb42`):
    the porting-side socket layer (`socket`/`connect`/`getaddrinfo`/… → `embk_net_*`)
    is now real. `test sockdemo` proves a plain POSIX HTTP client works — the same
    symbols `_socket` resolves to.
  - [x] **Brick 2 — `_socket` compiled into the external CPython** (~/cross/build-py)
    — **DONE, live: `test python net` -> `PYNET HTTP/1.1 200 OK`, exit 0.** CPython
    opens a real socket, resolves+connects to example.com:80, and reads the reply
    entirely through our POSIX socket layer. The build is now reproducible:
    `tools/cpython/configure-py-emblink.sh` asserts the full socket HAVE_* set in
    pyconfig.h + `_socket` in Setup.local (configure leaves them all off on a
    cross-build). Root cause of the earlier ENOTSUP: without `HAVE_SOCKET`,
    socketmodule.c `#define socket stub_socket` shadows the libc call with a stub
    that just `errno=ENOTSUP` -- the real `socket()` was never reached. Fixes that
    landed with it: `fcntl` F_GETFL/F_SETFL (set_inheritable needs F_GETFD, the
    blocking-flag read), `SOMAXCONN` in `sys/socket.h` (HAVE_LISTEN pulls it in),
    and `test python net` grants CAP_FILESYSTEM too (CPython reads its stdlib zip).
    *Note:* sockets are BLOCKING-only (fcntl O_NONBLOCK is refused) and
    `select()`/`poll()` on socket fds are unverified -- fine for simple blocking
    fetches, may bite urllib3 timeouts.
  - [x] **Brick 3 — a native `_embtls` module** (NOT a libtls-backed `_ssl`:
    real `_ssl` wants the whole OpenSSL API we don't have) — **DONE, live:
    `test python tls` -> `PYTLS HTTP/1.1 200 OK`, exit 0.** CPython does an
    authenticated TLS 1.3 https:// fetch to pypi.org over our OWN libtls
    (cert + hostname verified against the embedded GTS Root R4), zero OpenSSL.
    `tools/cpython/_embtlsmodule.c` is a ~180-line capsule module over
    `user/lib/tls/tls_handle.{c,h}` (an opaque-handle wrapper that keeps libtls's
    kshim/kernel include world out of the same TU as Python.h); the libtls
    objects (`build/tls_*.o`) link straight in. Reproducible via the configure
    script (symlinks the module into Modules/, declares it in Setup.local).
  - [x] **Brick 3b — a pure-Python `ssl.py` shim** over `_embtls` — **DONE,
    live: `test python https` -> `HTTPS 200 OK`, exit 0.** The whole stdlib path
    works: `http.client.HTTPSConnection` -> our `ssl.py`
    (`SSLContext`/`wrap_socket`/`SSLSocket` with `sendall`/`recv`/`makefile`) ->
    `_embtls` handshake -> request -> response headers read back through the
    buffered TLS stream. The shim REPLACES the stdlib ssl.py (which imports
    OpenSSL's `_ssl`) via a `tools/mkpystdlib.py` OVERRIDES map; kept in-repo at
    `tools/cpython/ssl.py`. It also forced four more socket-method HAVE_* macros
    (SETSOCKOPT/GETSOCKNAME/GETPEERNAME/SHUTDOWN) that http.client's TCP setup
    needs. Honesty note: libtls always authenticates, so `ssl.py` has no
    unverified mode -- `_create_unverified_context` still verifies (errs toward
    more security). Sockets are blocking-only; `select`/`poll` unverified.
  - [x] **Brick 4 — pip runs on the OS** — **DONE, live: `test python pip` ->
    `pip 26.1.2 from /data/apps/python/pip.zip/pip (python 3.14)`, exit 0.**
    `python -m pip --version` imports pip's whole ~450-module tree (vendored
    urllib3/requests/rich/platformdirs) and runs. Pieces:
    - `tools/mkpip.py` repacks CPython's bundled pip wheel STORED+precompiled
      into `build/pip.zip` (a DEFLATED wheel can't be zipimported -- no zlib on
      the path yet); packed beside the interpreter, added to `._pth`'s sys.path.
    - **zlib** cross-compiled (`configure-py-emblink.sh` builds `libz.a` from
      zlib-1.3.1 + declares the stdlib `zlib` module) -- pip's rich imports it.
    - `tools/mkpystdlib.py` grew a PATCHES map (subprocess `_can_fork_exec=False`
      -- no fork/exec/wait here) and a build-extras step (packs
      `_sysconfigdata__emblink_` which sysconfig imports).
    - os-function macros exposed: `HAVE_READLINK/GETUID/GETEUID/GETPPID/UMASK`
      (posixpath.realpath/platformdirs reference them; our libc backs each).
  - [x] **Brick 5 — `python -m pip install` WORKS** — **DONE, live:
    `test python pip install` -> `Successfully installed six-1.17.0`, exit 0.**
    `pip install six` on the metal: index fetch from pypi.org, wheel download
    from files.pythonhosted.org (both over our own TLS -- their cert chains
    verified against the embedded GTS Root R4), unpack + install to `--target`.
    The whole real pip pipeline. What it took, each an honest fix:
    - **Non-blocking sockets (kernel feature)** -- the enabler. urllib3 sets a
      socket timeout -> non-blocking; we now support it for real:
      `fcntl(O_NONBLOCK)` stores a kernel fd flag; `connect` returns
      `-EINPROGRESS` and completes in the background (the RX kthread advances
      SYN_SENT->ESTABLISHED); a `select()` over a new `sys_fd_poll` reports
      readiness; `recv` returns `-EAGAIN` when no data. `test nbsock` proves the
      whole shape natively. NOT the faked "O_NONBLOCK is set" the meta-lesson
      warns against -- it genuinely works.
    - `ssl.py` grew what urllib3 v2 reads: OPENSSL_VERSION/verify_flags, and a
      getpeercert() that reports the name libtls VERIFIED during the handshake
      (so urllib3's redundant re-check passes). wrap_socket sets the fd blocking
      first (libtls does blocking record I/O). fileno() returns the real fd
      (urllib3 select()s on it for connection reuse).
    - `_ssl`/`mmap` stub modules; truststore patched to ImportError (it needs a
      memory-BIO trust store we don't have) so pip takes its certifi+ssl fallback.
    - `fsync`/`fdatasync` now return 0 -- HONEST: EMBKFS has no write-back cache
      (writes go straight to the device), so the flush is vacuously satisfied
      (the FD_CLOEXEC case). `HAVE_CHMOD` on (os.chmod was doing errno=ENOSYS
      instead of calling our real chmod). pip writes+chmods every installed file.
  `pkgfetch` already covers the common "get me this pure package" case without any
  of this.
  - **What pip still LACKS (verified working = a single pure-Python wheel to a
    writable `--target`):**
    - **socket timeouts are not ENFORCED** -- `settimeout(T)` is accepted but the
      op blocks to completion (see [[networking-stack]] non-blocking note); a
      hung server would hang pip. select() honors its own timeout, but a blocking
      recv inside libtls/the SSLSocket does not. Fine for a responsive PyPI.
    - **needs a writable `--target` + `TMPDIR`** (we pass `/data/tmp`). No
      default site-packages / `--user` scheme wired; installing into a sealed
      `/system` is correctly refused, not handled.
    - **pure wheels only.** An sdist (or any package with a build step) needs
      PEP 517 -> a subprocess to run the backend -> fork/exec, which EmbLink does
      not have. `pip install <sdist>` will fail at the build isolation step.
    - **`--no-deps` is what's proven.** Multi-package dependency resolution +
      several sequential downloads is plausible (same code path) but untested at
      scale; the resolver can be slow under TCG.
    - **no cache** (`--no-cache-dir` used): cachecontrol's disk cache rides on
      `mmap`, which is a stub. Also no `keyring`/auth, no VCS/editable installs.
    - threading: pip is largely single-threaded here (fine), but any parallel
      path would hit our thread gap ([[cpython-port]]).
- [x] ~~**git clone over HTTPS.**~~ **DONE (G1-G4), metal-proven.** The stock git
  CANNOT do this here: its HTTPS transport fork/execs `git-remote-https` AND
  `index-pack`/`unpack-objects`, and EmbLink has no fork/exec (`start_command` ->
  `fork()` -> ENOSYS). So it's a from-scratch clone tool (`user/git/` + `user/bin/
  gitclone.c`) that drives git's smart-HTTP protocol directly over libtls and
  writes a real `.git` the on-OS git can then use. `test gitclone` clones
  github.com/octocat/Hello-World into `/data/hello` over our own TLS:
  - **G1 ref discovery** -- `githttp` (smart-HTTP GET/POST over libtls, HTTP/1.0
    read-to-EOF) + `pktline` (git framing); parses the `info/refs?service=git-
    upload-pack` advertisement. Metal: `GITCLONE 3362 refs, HEAD=7fd1a60b (master)`.
  - **G2 fetch** -- POST `git-upload-pack` with `want <sha>`+flush+`done`, parse
    `NAK\n` + the raw packfile. Metal: `GITFETCH pack 7 objects, 700 bytes`.
  - **G3 unpack** -- `user/git/pack.c` (own zlib-streaming inflate over the libz.a
    built for pip, ofs/ref-delta resolution) + `user/git/sha1.c` (own SHA-1,
    FIPS-vector + git empty-blob verified) -> SHA-named objects. Metal:
    `GITUNPACK 7 objects (3 commit, 2 tree, 2 blob), HEAD reconstructed`.
  - **G4 write + checkout** -- `user/git/repo.c` writes loose objects (zlib-deflate
    `"<type> <size>\0"`+content, sha-addressed), `HEAD`/`refs/heads/<branch>`/
    `config`, then recursively checks out the working tree. Metal:
    `GITCHECKOUT /data/hello: 7 objects, 1 files, branch master -> OK`, and the
    README lands on EMBKFS (13 bytes = "Hello World!\n", `vfs_stat` confirmed).
    A **real** upstream git reads the result (`git fsck` clean, `git log` shows
    all 3 commits) -- verified on the host against the same packfile.
  - **G5 deltas -- PROVEN (host SHA-exact + live metal).** Hello-World is
    delta-free, so delta reconstruction is exercised separately: a host harness
    (`packtest`) unpacks genuinely deltified packs and every object's sha+type is
    **byte-identical to `git verify-pack -v`** -- both ofs-delta (github's default,
    chained to depth 3) and ref-delta (`--no-delta-base-offset`), 24/24 objects
    each, plus Spoon-Knife's real github pack (16/16). Live on the metal,
    `test gitclone delta` clones **octocat/Spoon-Knife -> /data/spoon**: github
    sends a 10-object HEAD pack containing **1 ofs-delta** (confirmed by parsing
    the entry types), our unpacker resolves it (`GITUNPACK 10 objects … HEAD
    reconstructed`), and the 3-file working tree (incl. the delta-derived blob)
    checks out on branch `main` (`index.html` = 355 bytes on EMBKFS). Branch
    detection also proven here (`main`, not `master`).
  - **G6 loop closed -- the OS's OWN ported `git.elf` operates on the clone.**
    After `test gitclone[ delta]` checks out, the selftest runs the ported git in
    the cloned dir (via `PWD=`, the per-process cwd `test git cwd` relies on -- no
    chdir). Metal: `git log --oneline` **exit 0** prints Spoon-Knife's real 3-commit
    history (`d0dd1f6 (HEAD -> main) Pointing to the guide for forking` …) --
    proving the on-OS git reads OUR refs + inflates OUR loose commit objects; and
    `git cat-file --batch-all-objects` **exit 0** re-inflates and hash-checks
    **every** object we wrote (10/10 listed with type+size). So our from-scratch
    HTTPS clone writes a repo the real git implementation fully accepts, on the
    metal. (`git fsck` also verifies the objects -- `Checking object directories:
    100%, done`, no object errors -- but then SPAWNS commit-graph/multi-pack-index
    sub-checks; fork/exec is ENOSYS here, so its *exit* is nonzero for a reason
    unrelated to repo validity. The test gates on `log`, which never forks.)
  - **The blocker (G1) was an uninitialized-memory HEISENBUG** in libtls's ECDSA
    verify (`ecdsa.c` read an uninitialized stack `bn`): benign garbage on the
    host, but under QEMU it broke github's P-256 leaf verification (rc=-103) --
    data-dependent, so GTS/pypi leaves passed. Diagnosis: linked the exact target
    objects into a host harness -- they verified github's chain fine; the failure
    only reproduced on the OS and flipped with any codegen perturbation (a debug
    print "fixed" it). **Fix: `-ftrivial-auto-var-init=zero` in NEWLIB_CFLAGS**
    (zero every uninit auto var -- deterministic, right for security-critical
    crypto). Also added github's root (USERTrust ECC, P-384) to the trust store +
    `ecdsa_secp384r1_sha384` (0x0503) to the ClientHello sig-algs.
  - **G7 shallow clone (`--depth N`) -- metal-proven.** `gitclone --depth N`
    sends a `deepen N` line in the upload-pack request; the server prefixes the
    response with `shallow <sha>` boundary lines (ended by a flush) before NAK,
    which we parse and write to `.git/shallow`. Validated against the REAL github
    server on the host first (curl replayed our exact request bytes -> the exact
    `shallow`/flush/NAK/PACK framing our parser consumes -> our unpacker produced
    the 5-object depth-1 pack matching real git). Live: `test gitclone shallow`
    clones **octocat/Spoon-Knife --depth 1 -> /data/spoonshallow** --
    `GITSHALLOW depth 1, 1 boundary commit(s)`, `GITUNPACK 5 objects` (just the
    tip vs 10 for a full clone), and the on-OS git reads it as genuinely shallow:
    `d0dd1f6 (grafted, HEAD -> main) …` (**1** commit, not 3; `git log`/`cat-file`
    both exit 0). githttp also gained coarse `recv NN KB` progress prints -- the
    22800-ref advertisement is ~1.5 MB and streams slowly over TLS under TCG
    (~10 min), which without progress output is indistinguishable from a hang.
  - **G8 push (`gitpush`) -- authenticated, metal-proven.** The inverse of clone:
    `gitpush <url> <file> <path> [branch]` builds a blob+tree+commit, serializes
    them with a NEW **packfile writer** (`pack_write`, the inverse of pack_unpack
    -- host-proven: round-trips every sha AND `git index-pack` accepts our pack),
    and drives **git-receive-pack** (`user/git/push.c`) over libtls with **HTTP
    Basic auth** (`githttp` gained an `authb64` arg; own base64, byte-checked vs
    coreutils). Token comes from `$GITPUSH_TOKEN` (env, never argv). Proven three
    ways: (a) the request feeds a real local `git receive-pack` which accepts it +
    updates the ref + `fsck` clean; (b) the exact bytes + auth POST to a real
    GitHub repo return `unpack ok`/`ok <ref>`; (c) LIVE on the metal, `test
    gitpush` pushed a first commit CREATING `refs/heads/main` on
    github.com/teo1747/mblink-push-test (`GITPUSH refs/heads/main <sha> -> OK`),
    and a real `git clone` reads it back -- author `EmbLink <os@emblink>`, the
    pushed README, `fsck` clean. Credentials are staged into the LOCAL image only
    (mkfs env-gated: `EMBK_GITPUSH_TOKEN_FILE` + `EMBK_GITPUSH_URL`), never
    committed.
  - **G9 incremental commit push -- metal-proven.** A push onto a NON-empty branch:
    gitpush sees the tip is non-zero, FETCHES it (shallow upload-pack, reusing the
    clone machinery), reads the parent commit's root tree, and SPLICES it
    (`push_make_next_commit`/`splice_tree` in push.c: add/replace the target
    top-level entry, keep every other, re-serialize in git's tree order) into a new
    tree; the new commit carries the tip as `parent`. Only the 3 NEW objects are
    packed -- the unchanged blobs already live on the server. Host-proven (a first
    then an incremental push to a local `git receive-pack`: the pre-existing file
    survives, 2-commit chain, fsck clean). LIVE: `test gitpush` now pushes TWICE --
    `README.md` (first commit, creates `main`) then `NOTES.md` (incremental); on
    github `main` ends with BOTH files (README.md survived the splice) and a
    `e709a83 parent=6d96e3b` two-commit chain, fsck clean.
  - **G10 the write/negotiate frontier -- metal-proven (`test gitfeat`).** Four
    features, all live on the metal in one sequence + exhaustively host-proven
    against a real `git receive-pack`:
    - **multi-ref clone** (`gitclone --ref <branch|tag>`): read_refs selects the
      requested ref (full or short name, heads or tags) instead of HEAD. Live:
      cloned the `dev` branch (not the default) and checked out its content.
    - **nested-subtree splice**: `push_make_commit` + a RECURSIVE `splice_path`
      rewrite every tree along a `a/b/c` path (creating missing subdirs), so an
      incremental commit to `docs/guide.md` preserves all other files/dirs. Host:
      `docs/deep/notes.md` + `docs/guide.md` coexist through three commits, the
      first file survives every splice. Live: pushed `docs/guide.md` (4 objects =
      blob + 2 trees + commit).
    - **force-push** (`--force`): push an unrelated orphan commit as a non-ff
      update. Host + live: `main`'s whole history replaced by a single commit.
    - **delete** (`--delete <branch>`): a zero-id update + empty pack. Host + live:
      the `dev` branch removed (`unpack ok`/`ok`, ref gone).
  - **Deferred (small tail):** `want`-list has no `have`/negotiation beyond deepen;
    no side-band-64k progress; no thin-pack completion; deltas seen live are
    shallow chains (depth 1) -- deep chains host-proven (depth 3) but no large live
    clone under TCG (slow). The ported stock git.elf still uses its OWN transport
    only for local ops.
- [ ] **Robustness: a transient `test pkgfetch` rc=-106 (chain USAGE) was seen on
  one boot** then vanished (later boots verify the same pypi chain fine). Suspect a
  fragmented Certificate-message / net-read edge case leaving a cert parsed with
  is_ca=0 rather than failing outright. Investigate the flight reassembly under
  multi-record certs; possibly related to the sys_read byte-drop history.
- [ ] **Our own package registry fetch** (packaging PK4) over libtls — the
  authority-declaring bundle download. Design only.

### Ciphersuites & key exchange (only one of each today)
- [ ] `TLS_CHACHA20_POLY1305_SHA256` — self-contained AEAD, great on cores without
  AES-NI. Needs ChaCha20 + Poly1305 (new crypto).
- [ ] `TLS_AES_256_GCM_SHA384` — the GCM core is already cipher-agnostic and
  AES-256 exists; needs the SHA-384 transcript/key-schedule variant wired.
- [ ] P-256 / P-384 ECDHE key_share (only **X25519** is offered/accepted now).
  Refused-group HelloRetryRequest would then matter.

### Handshake features not implemented
- [ ] **HelloRetryRequest** — detected and rejected (`-2`), never handled. A server
  that wants a different group fails. Fine while we only offer X25519 to servers
  that accept it.
- [ ] **KeyUpdate** — post-handshake key rotation is ignored; a server that sends
  one makes subsequent records undecryptable. Rare in a single fetch.
- [ ] **Session resumption / 0-RTT / PSK** — no NewSessionTicket use (tickets are
  read and discarded), no early data. Every connection is a full handshake.
- [ ] **Client certificates** — CertificateRequest not handled (we only do the
  common server-auth case).
- [ ] **Record-layer omissions:** no key-update seq reset, no max-record-age /
  rekey, we emit **no padding**, and we don't fragment outgoing handshake messages
  (fine — ours are small). Alert handling is minimal (any alert on read = EOF).

### Certificate verification — remaining gaps
- [ ] **More trust anchors.** Only 3 bundled: GTS Root R4 (EC), ISRG Root X1 (RSA),
  GlobalSign Root R3 (RSA). Broad but not universal — a DigiCert/Sectigo/etc. site
  fails with `-105` (no anchor). Want a curated set under `/system/etc/ca` the user
  can grow (docs/TLS.md §5.1), verified-boot-sealed.
- [ ] **RSA-PSS-*signed* certs** (`id-RSASSA-PSS` in the cert, not just in
  CertificateVerify) — parsed as `X509_SIG_NONE`, so a chain link signed that way
  won't verify. Rare for CAs today.
- [ ] **Ed25519 / EdDSA** certificate + CertificateVerify signatures — not
  implemented.
- [x] ~~**Basic Constraints (CA:TRUE / pathLen), Key Usage (keyCertSign), Extended
  Key Usage (serverAuth)** not enforced.~~ **DONE** (commit `4ca72bb`): the parser
  reads all three; `x509_verify_chain` requires every issuer to be a CA with
  keyCertSign (+ pathLen bound) and the leaf to be server-auth-usable. Closes the
  classic leaf-as-CA forgery — proven by `test_constraints.c` (a validly-signed
  cert from a non-CA leaf is refused with `X509_ERR_USAGE`).
- [ ] **Name Constraints** (permitted/excluded dNSName subtrees on a CA) — still
  NOT enforced. Rare in the public web (mostly enterprise/gov CAs); moderate
  parsing effort. A constrained CA could currently issue outside its subtree and
  we'd accept it.
- [ ] **Revocation checking** — still absent. Deliberately deferred, not faked: a
  revocation check that soft-fails on any error (no responder, parse error) is
  security theater and worse than honest absence. It is a genuine multi-protocol
  network feature, and the ecosystem is mid-transition, so it's ~a phase of work:
  - **OCSP stapling** (cleanest, no extra round trip): send `status_request` in
    ClientHello, read the stapled `CertificateStatus`/CertificateEntry extension,
    parse `BasicOCSPResponse`, verify its signature (issuer or delegated responder
    + its own EKU `id-kp-OCSPSigning`), check `certStatus`. *Observed:* pypi.org
    staples; **Cloudflare and Let's Encrypt do NOT** — so stapling alone covers
    little.
  - **CRL** (where the leaf has a CRL Distribution Point — Let's Encrypt now does,
    having dropped OCSP in 2025): fetch the `.crl` over HTTP, parse the signed
    `CertificateList`, verify its signature, check the serial. CRLs can be large /
    sharded.
  - **OCSP request** (for leaves with only an AIA OCSP URL, e.g. Cloudflare):
    build a DER OCSPRequest, HTTP POST to the responder, verify the response.
  Doing it *right* means all three + response-signature verification; anything
  less silently passes on the sites it doesn't cover. Prereq for treating the
  stack as trustworthy against key compromise.
- [ ] **Hostname matching is minimal:** DNS SANs only (no IP-address SANs, no CN
  fallback — CN fallback is deliberately omitted, which is correct), single-label
  `*.` wildcard only.
- [ ] **Validity uses the OS wall clock** (`gettimeofday`); a wrong RTC would
  wrongly accept/reject. No max-chain-age or "not-after within anchor validity"
  cross-check.

### Verification / testing gaps
- [ ] **Live *negative* boot test** — host tests reject wrong-host/expired/tampered,
  but there's no on-OS test that connects to a deliberately-bad server (expired.
  badssl.com / wrong.host.badssl.com / self-signed.badssl.com) and confirms the
  handshake is *refused*. The security property is only host-proven, not metal-
  proven.
- [ ] The `test pypi` live run is timing-flaky under TCG (the desktop clock widget
  starves the kernel serial-console poll, so the fed command isn't always drained);
  the pypi chain + GlobalSign R3 anchor are host-verified deterministically, and the
  identical wget-HTTPS-to-disk pipeline is metal-proven by `test wget https`.

### Crypto hardening (verify-only today, not constant-time)
- [ ] The Montgomery bignum, ECDSA, RSA, and X25519 are written for **verification /
  public-value** use and are **NOT constant-time**. Before any *signing* or private-
  key handling on the OS (client certs, our own CA, key storage) they need constant-
  time review. `getentropy` is RDRAND-only (fine; documented).

### Terminal / shell (post "complete terminal" pass, 2026-08-05)
- [ ] Copy/paste in the terminal -- blocked on an OS clipboard (none exists yet)
- [ ] Ctrl+C interrupting a RUNNING command from the GUI terminal (needs the
      async-signal gap closed, or the interrupt-route delegated across the pipe)
- [ ] Resizable terminal window (T_COLS/T_ROWS fixed; needs reflow on resize)
- [ ] Wheel scrollback direction untested on real hardware (sign chosen from
      ui_scroll_begin's convention; flip in term_view if it feels inverted)
- [ ] SGR: only fg colour + bold honoured; backgrounds/underline dropped

### Minimize / restore (shipped 2026-08-06)
- [ ] No minimize ANIMATION -- the window vanishes and reappears instantly.
      The Mac genie is the obvious want; a scale+fade toward the dock icon is
      the cheap version. Blocked on the same window-motion work as the reverted
      open/close animation below.
- [ ] Restore is per-PROCESS, not per-window: an app with two windows gets both
      back. Fine while apps are single-window; revisit with a real window list.
- [ ] No keyboard path (Super+M) and no "minimize all" -- the dock icon is the
      only way back.
- [ ] A minimized app still runs its render loop if it is timer-driven (a clock
      widget keeps painting into a buffer nobody composites). Retained-update
      apps idle correctly; a `minimized` bit in win_input would let the rest
      stop too.

### Window depth: drop shadow + rounded corners (shipped 2026-08-06)
- [ ] Focused-vs-unfocused shadow depth is code-verified, not photographed:
      every metal pairing so far had the back window fully covered, and the
      desktop is a fallback focus owner so clicking wallpaper cannot defocus a
      window. Compare the two bands side by side next time two windows overlap.
- [ ] WIN_RADIUS is a compositor constant (9px), not a theme token -- it should
      come from theme.c alongside the toolkit's own corner radii, or in-window
      cards and the window frame will drift apart.
- [ ] Glass/widget/translucent windows are excluded from BOTH shadow and
      rounding (they draw their own rounded pill in-surface). Fine today, but
      a glass APP window would want the compositor treatment.
- [ ] The carve costs ~324 px/window/repaint. Invisible against a full window
      blit, but if per-frame cost is ever profiled, this is a candidate for
      caching the coverage mask (it only changes with the radius).

### UI framework: menu-scrim / click-through (investigated 2026-08-05, reverted)
- [ ] Clicking the top bar's transparent canvas under an open dropdown does
      NOTHING (menu stays open). TWO stacked causes found:
      1. the menu's dismiss scrim is an overlay, and overlays size to their
         PARENT -- here the 28px menubar strip, so the scrim covers almost
         nothing. Overlays honouring an explicit fixed size fixes this half
         (layout.c grid+stack overlay arms).
      2. the hit test has NO layering: "topmost" = last in document order, so
         the bar's trailing Spacer wins the point over the earlier scrim.
      A "modal hit layer" flag (2-pass instance_at: modal subtrees first) was
      prototyped and REVERTED: marking kit's Overlay + the menu scrim modal
      made the bar's own content vanish while a menu was open and broke
      launcher grid clicks -- interaction with reconciliation/epoch or with
      paint order not understood yet. Needs a designed z-layer story (paint
      AND hit), not a flag bolted onto hit only.

### Z-layers: metal menu-dismiss anomaly (2026-08-06, OPEN — evidence attached)
- [ ] On metal ONLY, while a top-bar dropdown is open, presses into the bar's
      window stop having any effect: the viewport-sized layer-1 scrim resolves
      the hit (traced: "PRESS -> inst 9.1 layer=1", and g_menu_scrim == 9.1,
      same index AND generation), yet ui_consume_click(g_menu_scrim) never
      closes the menu; in later runs presses stopped arriving at all (no PRESS
      trace despite QMP button events). Host repro (build/menurepro pattern:
      dispatch_click -> frame) passes all three checks: open, canvas-dismiss,
      settle+reopen. Every link re-derived sound; suspect an em_app input-feed
      / build-gate interaction under TCG, or a consume between press and
      em_menu_end_. Escape hatch meanwhile: selecting a menu item closes.
      Menus render correctly open+closed (identity fix); launcher/dock/ghost
      unaffected with menus closed.

### Essential apps (Files / Settings / Terminal, 2026-08-06)
- [x] ~~RIGHT-CLICK NEVER REACHES AN APPLICATION~~ -- WRONG, and worth keeping
      as a caution. Right-click works fine; the observation was made during the
      window when `timeout 200 make embkfs.img` was silently producing no image
      and the VM was booting a stale one (see Harness). The whole chain --
      compositor button mask, sys_win_input, em_feed_pointer, em_right_clicked
      -- was correct all along. A negative result from an unverified build is
      not a result.
- [ ] Button labels are always CENTRED. em_button_impl uses ui_box_begin, and
      justify appears not to apply to a box the way it does to a stack, so
      .leading() still leaves a file name centred in its column. Needs the
      button's label to live in a stack, or box justify to work.
- [ ] Files list view has no column headers and no sort-by-column.
- [ ] ContextMenu's anchor is PARENT-relative while em_right_clicked reports
      WINDOW-local coordinates, so a menu emitted inside a pane opens offset by
      that pane's origin. Emitting it at the window's top level fixes the
      anchor and breaks the menu -- the dismiss scrim then covers the press
      point and eats the opening click. em_menu_panel_open should subtract its
      overlay's resolved origin (available a frame late via ui_open_rect), or
      take window coordinates outright.
- [ ] Files' status line ("N items") may still sit tight against the window's
      bottom edge. The viewport-height bug that caused the worst of it is fixed
      (em_app published height 0 until the first resize); re-check and, if it
      persists, measure the resolved rects rather than guessing constants.
- [ ] Files has no delete, rename, copy or move. unlink/rmdir/rename syscalls
      all exist -- what is missing is the confirmation design, and a delete
      that ships before its confirmation is a bug with a keyboard shortcut.
- [ ] Files opens a file by spawning the editor; there is no "open with", and
      a file whose kind we name (Image, Archive) still opens in the editor.
- [ ] Settings' light theme is applied but has never been looked at seriously
      -- the whole shell was designed on the dark ground.
- [ ] Settings changes reach a RUNNING application only if it is the desktop
      (which polls). Other apps pick up the accent at their next launch. A
      config-changed broadcast would fix it; the IPC exists.
- [ ] The Terminal still has no tabs, no text selection and no font-size
      control. Tabs need a second shell process per tab, which is the real work.
- [ ] The Terminal's prompt is inline now, but the transcript does not scroll
      on its own as output arrives past the bottom -- it re-windows to the last
      N lines. Fine at a screenful; a real pty would want a scrolling region.
- [ ] `$(wildcard build/*.elf)` in the embkfs.img rule is evaluated at parse
      time, so the FIRST build after adding an app packs an image without it
      (spawn fails -ENOENT). Second `make` is correct. Cost me a boot cycle.

### Adaptive menu-bar ink (shipped 2026-08-06)
- [ ] Only the LIGHT ink is verified against a real wallpaper; the dark branch
      was proven by temporarily inverting the threshold (bar text went
      near-black, docs say it works, but no light wallpaper exists yet to see
      it in earnest). Revisit when wallpapers are user-selectable.
- [ ] The sample is one 6px band directly under the bar. A wallpaper that is
      light at the left and dark at the right gets one ink for the whole bar;
      per-item sampling would be the real answer.

### Harness
- [ ] `make embkfs.img` takes ~110s after a wide rebuild. A `timeout 200` around
      it is NOT generous -- it killed the build mid-flight, left no image, and
      the following `cp embkfs.img ...` failed silently, so the VM booted the
      PREVIOUS image and several "the fix didn't work" observations were really
      "the fix was never in the image". Always `test -f` the artifact before
      copying it, and give the image build 900s.

### Host test rot
- [ ] `make font-test` does not compile: font_test.c calls `be->draw_text` with
      8 arguments against the 11-argument backend signature (box_w/box_h/paint
      were added for gradient + clipped text). Pre-existing -- it fails at HEAD
      too -- but it means the font suite has not run in a while. The other five
      suites (scene/layout/backend/declare/reactive) are green.

### Window motion (shipped 2026-08-06, compositor-side)
- [ ] Closing animates via a full-window pixel COPY (~1.4MB kmalloc per close;
      the live buffer is freed under the ghost when the process is reaped).
      Cheap enough at one motion at a time, but if closes ever overlap or
      windows get much bigger, a deferred free of the shared buffer would beat
      copying it.
- [ ] The park target is the bottom CENTRE of the screen, not the app's actual
      dock icon: the dock is drawn by home and the kernel has no idea where its
      icons are. Plumbing the icon rect through (win_set_park_target?) would
      make it land on the right icon.
- [ ] Only one motion runs at a time; a second window animating while one is in
      flight is simply not animated (anim_start returns).
- [ ] The window is un-hittable for the 170-200ms it is in flight (it is hidden
      and the ghost takes no input). Fine at these durations, wrong if they grow.
- [ ] Frame pacing rides the main loop's ~100Hz timer wake. Under heavy TCG load
      the loop can be slower, and the motion degrades to fewer frames rather
      than slowing down (it is time-based, so it still lands on schedule).

### Window-materialize motion (2026-08-06, reverted -- findings attached)
- [ ] Wrapping the app view in an opacity/offset wrapper (fade+rise on open)
      left the window CROPPED to ~content-text width with an offset ghost of
      the input row, and it never settled. Suspect the opacity<1 group path:
      whole-window subtree -> cpu_scratch_acquire at full window size, plus
      moved-wrapper dirty across animated frames. Chase it in the host
      harness (menurepro pattern) before re-attempting; the menu drop-in
      (small area, same mechanism) works fine, so it is scale-related.
      SUPERSEDED for window open/close: doing the motion in the COMPOSITOR
      (see above) avoids the toolkit group path entirely. This entry stands
      only for in-APP whole-view transitions, if one is ever wanted.

### Vellum, the browser (B1 shipped 2026-08-07) -- what B1 does NOT do
- [x] ~~**B2: the network.**~~ DONE 2026-08-07: `user/web/net.{c,h}` +
      `url.{c,h}`, http:// and https:// live on the metal.
- [ ] No chunked transfer-encoding. The client is HTTP/1.0 with
      `Connection: close`, so a server that chunks anyway would confuse it.
      Deliberate for now (see BROWSER.md); needed before HTTP/1.1 keep-alive.
- [x] ~~The fetch is SYNCHRONOUS~~ DONE 2026-08-07: it runs on a worker thread
      (`user/web/fetchjob.{c,h}`) and the view polls. The window stays alive and
      the page lands without a freeze. Three real bugs BELOW the browser had to
      be fixed to get there -- see the commit; all three were latent for anyone
      who tried the same thing.
- [x] ~~An app renders DIRECTLY INTO THE SHARED WINDOW BUFFER~~ FIXED
      2026-08-07: em_app renders into a private back buffer and copies the
      FINISHED frame across (only the presented band). The compositor can no
      longer see a frame mid-draw. Vellum keeps its page, its chrome and its
      loading indicator intact for the whole of an https fetch.
- [x] ~~em_widget_run still renders straight into the shared buffer~~ FIXED
      2026-08-07, same back buffer as em_app_run. NOTE ON PROOF: the app
      runtime's version is metal-verified through a live https fetch; the
      widget version is compile- and review-verified only, because no widget
      runs at boot and the terminal would not launch through QMP to start one.
      To exercise it: run `clockw` from the shell and watch for "widget up" on
      serial. A glass widget is the case that matters -- it clears its whole
      window on every build.
- [ ] Under ONE emulated core the loading counter updates only once or twice
      during a fetch: each frame competes with the crypto worker for the core,
      so few complete. Correct, just coarse -- and a real machine or -smp 2 does
      not have the problem. If it ever matters, the fix is to make the view
      cheap while loading (skip re-rendering the document, which has not
      changed) rather than to tick harder.
- [ ] Nothing caches. Every navigation, including Back, refetches.
- [ ] No Content-Type handling: everything is parsed as HTML. An image or a
      binary will be rendered as garbage rather than refused or downloaded.
- [ ] The response buffer is 512KB (SRC_MAX); larger pages are truncated and
      say so. Fine, but a real limit worth revisiting with images.
- [x] ~~**Capabilities for a GUI app.**~~ DONE 2026-08-07: `<name>.caps`
      sidecar, parsed with the `.ns` one by `user/lib/appauth.{c,h}`
      (docs/USERSPACE_v2.md UP5). Vellum is born holding exactly
      {filesystem, network, gpu}.
- [ ] Only `home` reads the sidecars. The shell spawns apps too and grants
      whatever it holds; it should ask `appauth` the same question.
- [ ] An EMBX binary should carry its declaration inline (a section beside its
      capability table) instead of as a sidecar file -- the carrier note in
      USERSPACE_v2 UP4. Sidecars are the ELF-shaped answer.
- [ ] Nothing tells the USER what an app asked for. The launcher knows the mask
      at spawn time and could show it, which is the point of declaring it.
- [x] ~~No CSS~~ DONE 2026-08-07 (B5): user/web/css/{decl,sel,sheet}.c --
      declarations, selectors, cascade. 47 host assertions.
- [ ] CSS gaps, each deliberate: no percentages (need a containing block the
      box model does not expose), no float/position (the layout engine has no
      such concept), no `!important`, no pseudo-elements, no `@media`
      evaluation (dropped, not misapplied). `>` `+` `~` parse as descendant.
- [ ] font-size maps onto four toolkit roles by px threshold. Honest, but a
      continuum would need the text leaf to take a size directly.
- [x] ~~images~~ DONE 2026-08-07 (B6): user/web/png.c + imgcache.c. 24 host
      assertions on the decoder alone.
- [ ] PNG only. JPEG is the other half of the real web and is a DCT decoder,
      not an afternoon. GIF/WebP not considered.
- [ ] No interlaced (Adam7) PNG -- refused, not half-decoded.
- [x] ~~Images are not SIZED by the markup~~ DONE 2026-08-07: width/height
      attributes + CSS width/height/max-width, cascade order, aspect-preserving
      clamp to the column, and a reserved placeholder box so a sized picture
      causes NO reflow (proven by the reflow check in `make browser-render`).
- [ ] An image with no stated size still reflows when it lands -- unavoidable
      without knowing its dimensions, and true of every browser. CSS
      `aspect-ratio` would be the modern answer if a page supplies it.
- [ ] The image arena is 6.4MB for 8 slots, per page. A page wanting more gets
      alt text for the rest.
- [x] ~~tables~~ DONE 2026-08-07: data tables over the layout engine's grid --
      aligned columns, header rows, colspan, captions, ragged rows. Revisits
      the BROWSER.md §5 exclusion with evidence (documentation pages ARE
      tables); "tables as layout" is still refused.
- [ ] Table columns are EQUAL width. Content-proportional tracks need weighted
      grid tracks in the layout engine -- the renderer cannot measure, it emits.
- [ ] rowspan is parsed but ignored (colspan works). A rowspan needs the grid
      to reserve a cell in a LATER row, which auto-flow has no way to express.
- [ ] Cells do not stretch to their row's height, so a short cell's border box
      is shorter than a tall neighbour's. Cosmetic; needs per-row stretch.
- [x] ~~no JS~~ ENGINE DONE 2026-08-07: QuickJS 2024-01-13 runs on the OS
      (`js.elf`, `make js`, one two-hunk patch). Absent source => skipped.
- [x] ~~JS has NO DOM BINDINGS~~ DONE 2026-08-07: user/web/jsdom.c --
      querySelector(All), document.title, textContent, setText, getAttribute,
      setStyle, console.log. Scripts run at load; mutations re-render.
- [x] ~~NO EVENTS~~ DONE 2026-08-07: addEventListener('click'), setTimeout,
      setInterval, clearTimeout/clearInterval. The renderer asks the engine
      which elements listen and makes ONLY those clickable.
- [ ] Only 'click'. No keyboard, focus, hover or form events -- an unsupported
      event name THROWS rather than registering silently.
- [ ] No event bubbling: a click reaches the element that has the listener,
      not its ancestors. Fine for buttons, wrong for delegation.
- [x] ~~No fetch() binding~~ DONE 2026-08-07: real Promises, shared worker,
      microtask drain (JS_ExecutePendingJob every frame). Response = {ok,
      status, text()} -- deliberately small.
- [ ] The DOM surface is read-mostly: no createElement/appendChild/remove, no
      classList, no attribute WRITES except style. Each absent rather than
      faked -- a binding that accepts a call and changes nothing observable is
      worse than a missing one.
- [ ] `js -e` needs shell quoting (`js "-e" "2+2"`): the structured shell reads
      a bare `-e` as a unary minus. Either the shell should pass through
      unknown leading-dash tokens, or js should take a different flag.
- [ ] No Atomics.* (SharedArrayBuffer across threads) -- disabled by the port.
- [x] ~~forms~~ DONE 2026-08-07: user/web/form.{c,h} -- text fields, submit
      buttons, GET and POST, Enter to submit, el.value from script. 11 host
      assertions on the encoding and method rules.
- [ ] Only text inputs and buttons. No checkbox, radio, select, textarea
      (parsed as a field but rendered single-line), or file upload.
- [ ] No client-side validation (required, pattern, type=email).
- [ ] No form.submit() / onsubmit from script -- a script can read and set
      values but cannot submit or intercept.
- [ ] Link words are separate ghost Buttons (one per word, for per-word hit
      testing when a link wraps). Their 2px padding makes a link's inter-word
      gaps slightly wider than body text. Cosmetic; a per-run hit region would
      be the real fix.
- [ ] `make browser-render` renders with DejaVu from the host, so the on-metal
      metrics differ slightly from the host PNG. Good enough for geometry --
      do not use it to judge kerning.

### Dock / desktop input (2026-08-07)
- [x] ~~App windows covered the top of the dock~~ FIXED: em_app reserved a
      hard-coded 64px bottom strip while the dock band is dock_size+32+14 --
      84px at the default and 106 at the largest. The dock is drawn by the
      DESKTOP window, which sits behind every app window, and pointer input
      goes to the topmost window under the cursor, so anything overlapping the
      dock silently ate its clicks. The band is now one formula in oscfg.h
      (`oscfg_dock_band`) that both the desktop and every app window obey.
      Verified: Files' window is now 792x484 (600-32-84) and the dock is fully
      clear of it.
- [x] ~~The dock's magnifier and label named the WRONG icon~~ FIXED: three
      places computed the slot pitch as DOCK_BASE+10 while the layout uses a
      slot of DOCK_BASE+8 separated by spacing 10, i.e. DOCK_BASE+18. Proven
      from the captured world rect: the dock is 238px wide for four slots and
      4*(38+8)+3*10+2*12 is exactly 238, while the +10 formula predicts 216.
      The error compounds with the slot index -- 8px off at slot 1, 24px at
      slot 3 -- so the wrong icon swelled and the label named the wrong app.
      One helper, dock_slot_x0(), now serves all three.
- [x] ~~Dock clicks died while the magnifier animated~~ FIXED + locked by
      declare-test T8. em_image_button keyed its image LEAF by the PIXEL
      POINTER, which names the mip level -- and a magnifying icon swaps levels
      as it swells, so the leaf's identity churned during animation. A press
      edge captures LAST frame's instance; when the press landed on the same
      frame as a level flip, the captured handle was destroyed by that very
      build and ui_is_active() walked up from a dead instance. Clicks died
      exactly while the magnifier animated, which is exactly when a person
      clicks; the launcher grid never magnifies, so it never missed. Found by
      HOST repro after five metal boots of instrumentation narrowed it; the
      host test failed in two seconds and the fix is one stable key.
- [x] ~~Scrolling a document in Vellum is slow~~ IMPROVED 3.5x at the measured
      cost center (2026-08-07): text measurement is now MEMOIZED on the layout
      node, keyed by the scene node's content hash (+ font, size, width). A
      wheel tick used to re-measure every word of the document through the font
      engine -- one-line widths in the intrinsic pass, and the full wrap
      SIMULATION from seven call sites. All of it is a pure function of
      (content, font, size, width), none of which change while scrolling.
      Host-measured on the browser's real document: 0.75 -> 0.21 ms per
      build+layout pass (`make browser-render` now prints this). Every text-
      heavy surface benefits -- terminal, Files, the editor.
- [x] ~~A page with a photograph took 21 SECONDS to appear~~ FIXED 2026-08-07,
      and the fix was nowhere near where it looked. Suspicion fell on the new
      JPEG decoder's float IDCT (float is emulated under TCG). Rewriting it in
      fixed point with a DC-only fast path won 1.75x on the host and NOTHING on
      the metal -- 21.1s became 20.2s. Instrumented instead: `net 6ms, dec
      96ms, poll 4758ms` -- the image sat decoded for four and a half seconds
      waiting for a frame. One frame took 4.7s; all of it was
      `scene_render_frame`; all of THAT was `draw_rect`, 5262ms over 136 rects.
      `cpu_draw_rect` had no early-out for a fill that paints nothing, so every
      invisible block background walked its whole box (clip coverage, rounded
      SDF + sqrtf, paint, float blend) and threw the result away at the end.
      Never seen in our own apps, where an unstyled box is not emitted; a
      DOCUMENT emits one per block element. Frame render 4946ms -> 170-483ms,
      image round trip 4773ms -> 343ms, scroll ~4s -> 0.74s.
      Landed with two related hoists: `box_fully_covered()` answers "is coverage
      trivially 1?" once per primitive instead of once per pixel (the old test
      was `no dirty region AND no clips at all`, which a scrollable view makes
      permanently false), used by the image blit, the solid fill and the glyph
      blit; and the image blit steps its source coordinates in 16.16 fixed point
      rather than a float divide per pixel.
- [x] ~~You cannot copy anything out of the browser~~ SHIPPED 2026-08-07:
      drag to select, Ctrl+A, Ctrl+C to the system clipboard, Esc to clear.
      `user/web/select.c`, all post-layout over the scene. Proven on the metal
      end to end (copied in Vellum, pasted into the shell with Ctrl+V).
      Two bugs found building it, both worth remembering: `scene_mark_dirty`
      sets `dirty` but NOT `dirty_content`, so the highlight was applied and
      copied correctly while drawing zero pixels; and having the declarative
      build clear each run's background made a live selection re-dirty every
      selected word twice per frame (a full repaint per frame). Whoever sets a
      background owns removing it.
- [ ] HOW FAR VELLUM IS FROM A BROWSER, measured 2026-08-07 against four real
      pages rather than our own documents. example.com: correct.
      motherfuckingwebsite.com: essentially perfect. danluu.com: readable, but
      its `d{width:4em}` and `li{display:flex}` do nothing so dates weld onto
      titles. news.ycombinator.com: 1307 nodes in, "1. by | 2. by |" out --
      the story titles ARE in the DOM and the renderer drops them, which is a
      nested-<table> bug, not a CSS one. In priority order, what is missing:
        1. ~~External stylesheets are never fetched~~ FIXED (see below).
        2. PARTLY CLOSED 2026-08-07: added background/background-color (incl.
           the shorthand, scanned for a colour), border + border-width/color/
           style, border-radius, real padding (padding-top/right/bottom/left,
           and the shorthand no longer aliased onto margin -- once a box can be
           painted, inside-vs-outside is visible), text-align, line-height,
           and rgb()/rgba() colours. Still missing: position/top/left/z-index,
           float, any flex-*, gap, overflow, box-sizing, opacity, %/vh/vw/
           calc(), media queries, :hover/:focus, ::before/::after, var(), and
           per-edge borders (border-left alone paints all four).
           line-height is PARSED and stored but not yet used by layout.
        3. ~~Nested tables collapse (HN)~~ FIXED (see below).
        3b. ~~danluu's first list item renders as overlapping text~~ EXPLAINED
           and FIXED: it was the 64-child cap in layout (see below).
        3c. ~~Table columns are equal-width; every cell gets a UA border~~ BOTH
           FIXED 2026-08-07. Columns are now sized to content (a miniature of
           CSS's automatic table layout: a column is as wide as its widest
           cell, surplus and shortfall shared out in proportion), and cell
           rules are drawn only when the table asks with <table border=N> --
           the web's default is no border, which is why half the old web can
           use tables for LAYOUT.
        4. JS is 'click' only -- no bubbling, no createElement/appendChild,
           no querySelector.
        5. Form controls are text and submit only -- no checkbox, radio,
           select, textarea, file.
        6. No cookies, no cache, no charset handling beyond UTF-8, no iframe,
           canvas, svg, video or audio.
- [x] ~~<link rel=stylesheet> is parsed and thrown away~~ FIXED 2026-08-07:
      the parser records the hrefs (doc->cssref, document order, capped at 8
      and truncating rather than overrunning -- T22), and `user/web/cssref.c`
      fetches them on the shared worker, one at a time, BEFORE the page's
      images: a page that paints pictures before it knows what colour anything
      is shows the reader a wrong page and then rearranges it. A sheet that
      404s is not fatal -- the page renders with whatever styling arrived.
      Cascade order is every external sheet in document order, then the
      document's own <style>; the true CSS order interleaves them as they
      appear in the source, and that difference is written down in cssref.h
      rather than pretended away. Metal-proven on /system/web/styled.html.
      Also fixed: the status line composed its rule count ONCE at load and so
      reported "1 css rule" on a page with seven, three of which were already
      on screen -- it is now recomposed whenever a sheet lands.
- [x] ~~`make` did not build the browser~~ FIXED 2026-08-07, and this one cost
      most of an afternoon. mkfs AUTO-DISCOVERS every build/*.elf, and
      build/vellum.elf was never a prerequisite of anything -- it survived from
      an old explicit `make build/vellum.elf`. When a link failed and deleted
      it, `make` reported "Nothing to be done" and every image after that
      packed either NOTHING or a stale vellum.elf against a freshly built
      libembk.so. EmProps crosses that library boundary BY VALUE, so a stale
      app is a wild-pointer crash: a ring-3 page fault at CR2=0x9FFFFFFFF
      inside strlen, deterministic, on the second frame. It was bisected
      against four subsystems before the build was suspected -- the lesson is
      to check that the binary under test is the binary you just built.
      build/vellum.elf is now in the staged-app list. Any app reachable only by
      an explicit target has the same hazard; see docs/TODO.md.
- [ ] The instance pool (ui/declare) is a fixed INST_MAX array in libembk.so's
      .bss, so raising it costs every process on the desktop (65536 -> a 48MB
      shared library). It should be PAGED like the scene and layout arenas.
      8192 covers the worst real page measured (danluu 2752) with headroom.
- [x] ~~A container with more than 64 children silently lost the rest~~ FIXED
      2026-08-07, and this was the single worst bug found so far. `arrange()`
      gathered children into `kids[64]` on the C STACK; past 64 they were never
      positioned, so they kept their default 0x0 and every one of them painted
      at the PARENT'S ORIGIN, stacked on top of the first row. A <ul> with a
      hundred items, or a table with thirty rows, walks straight through it --
      i.e. any real document. It looked like a renderer bug (danluu.com's first
      list item was a pile of overlapping text) and was a layout one.
      They could not simply be made bigger: arrange() recurses per nesting
      level, so 1024-entry stack arrays would be megabytes deep on a real page.
      The four arrays now come from a shared bump pool that arrange() pops on
      exit -- correct because arrange() is strictly depth-first -- and overflow
      past the pool is COUNTED (`layout_children_dropped()`), so browser-render
      fails instead of quietly drawing wreckage. Same for the declarative
      instance pool: `ui_instance_overflow()` now reports refused allocations,
      and INST_MAX went 4096 -> 65536 because a document needs one instance per
      WORD (danluu: 2214, HN: 1255).
- [x] ~~Hacker News rendered as "1. by | 2. by |" -- 1307 nodes, no content~~
      FIXED 2026-08-07. TWO causes, neither of them tables. <center> was an
      unknown tag and so defaulted to INLINE, which put HN's entire nested-table
      document inside one inline formatting context; and emit_inline had a
      depth>8 guard, so with <center><table><tr><td><table><tr><td><span><b><a>
      being nine levels, every link's TEXT was dropped while the plain " | "
      between them -- one level shallower -- came through. <center> is now a
      block (with text-align:center, which is what it means), a block-level box
      inside an inline run now BREAKS the run and renders as a block, and the
      guard is 24. HN now renders its nav, ranks, titles, sites and subtext;
      text nodes went from ~30 to 776.
- [x] ~~text-align did nothing, on any page~~ FIXED 2026-08-07, and the bug
      was in the LAYOUT ENGINE, not the CSS. arrange()'s wrap arm started every
      line at the padding edge and never consulted `justify`; the non-wrap arm
      always had. A paragraph of text IS a wrapping row, so `text-align:center`
      could not work anywhere. Each line box now justifies on its own, which is
      what text-align means -- centre every line, not the paragraph as a block.
- [x] ~~`>` `+` `~` were parsed as DESCENDANT, so every scoped rule over-matched~~
      FIXED 2026-08-07: real child, adjacent-sibling and general-sibling
      combinators, plus `:first-child` / `:last-child`. Pinned by
      tests/web/selectors.html, which asserts what must NOT match as well --
      the old behaviour turns GRANDCHILD red and the corpus says so.
- [x] ~~var() was unsupported, so a modern stylesheet resolved to no colours~~
      FIXED 2026-08-07: `user/web/css/vars.c`. Custom properties are collected
      across the WHOLE sheet before any rule is applied (so a rule may use a
      var defined below it), and var() is expanded in decl.c before a value is
      interpreted -- which gives every property support for free instead of
      each one remembering. `var(--x, fallback)` honoured.
      Found while writing the test: the collector treated the sheet as one
      declaration block, and `:root {` begins with a COLON -- so the commonest
      place in the world to define a variable was the one place it was missed.
      It scans brace blocks now.
- [x] ~~@media blocks were skipped whole, losing every desktop rule~~ FIXED
      2026-08-07: `user/web/css/media.c`. Media types (`all`/`screen`/`print`/
      `speech`, `only`), `and`, comma-separated alternatives, a leading `not`,
      and the features that decide layout: min/max width and height,
      orientation, prefers-color-scheme. A matching block's rules are parsed
      IN PLACE so they keep their source position -- which is what lets a media
      override beat the base rule it overrides. An unrecognised feature makes
      its conjunction false (what CSS says, and the safe direction).
      Skipping was right while nothing could evaluate the condition, and stops
      being right the moment real pages are the target: most stylesheets are
      mobile-first, so the base rules ARE the phone layout and everything else
      lives behind a min-width. Skipping them renders the phone version at
      desktop size.
      The environment is set by the app, so Vellum answers with its own content
      width and the user's dark/light preference -- and RE-PARSES when the
      window is resized past a breakpoint, which a parse-once scheme gets wrong.
      Pinned by tests/web/media.html and its NARROW TWIN at 360px: the same
      rules must give opposite answers, which is what tests evaluation rather
      than parsing.
- [ ] @media is evaluated at PARSE time, so a var or rule behind a query is
      re-read on resize but a `@media` nested inside another is not handled.
      Also unsupported: @supports, @font-face (web fonts), @keyframes.
- [x] ~~display:flex and display:grid from CSS~~ SHIPPED 2026-08-07 (C2).
      flex-direction, flex-wrap, justify-content, align-items, gap/row-gap/
      column-gap, flex/flex-grow, grid-template-columns (the track COUNT --
      `repeat(3, 1fr)` and `1fr 1fr 1fr` are both three, and the widths come
      from content). Cheap because the layout engine has done flex and grid
      since it was written: this is the CSS spelling of machinery the whole
      toolkit already runs on.
      Two things CSS requires that had to be added: element children of a flex
      container are BLOCKIFIED (without it a nav bar's links merge into one
      inline run, so the gap and the justification apply to the strip instead
      of between the links), and whitespace-only text makes NO anonymous item
      (the newline between two <div>s was becoming a third grid cell, landing
      every card one column late).
      space-around / space-evenly fall back to space-between: the gap is in the
      wrong place but the items are still spread.
- [x] ~~A widget kept the SIZE it was given on a previous frame~~ FIXED, and
      this is a TOOLKIT bug that reached every EmUI app. em_apply_box calls
      ui_set_size only when a prop asks for one, so a reused instance keeps
      whatever size it last had. Invisible in a harness that renders one tree a
      few times; very visible in an app, which builds an empty view first and
      the document second -- instances are matched by POSITION across two
      different trees, so a box inherited `grow` from whatever held its slot
      before, ate the row's leftover and shoved its siblings to the right edge.
      The browser now states each box's size explicitly every frame
      (render.c size_box). The general fix -- em_apply_box always stating a
      size -- is still open, because every existing app is written against the
      current behaviour and would need re-checking.
- [x] ~~Makefile header dependencies were written by hand~~ FIXED: -MMD -MP.
      build/web_jsdom.o listed jsdom.c/jsdom.h/html.h but not style.h, so when
      `struct vstyle` grew, jsdom.o kept the OLD layout and was linked into the
      same binary as everything using the new one. Nothing about that symptom
      points at a Makefile. Found while chasing the flex divergence above; it
      was not the cause, and it was a real bug either way.
- [x] ~~Percentage widths, vw/vh, and box-sizing~~ SHIPPED 2026-08-07 (C3,
      first half). `width: 50%` travels as a PERCENTAGE through vstyle and is
      resolved by layout against the containing block, because that number does
      not exist while the stylesheet is being read -- a new SIZE_PERCENT mode
      whose fixed_value holds the fraction. Anywhere the mode is not handled it
      degrades to intrinsic, which is a sane fallback rather than a wrong
      number. vw/vh read the same viewport a media query asks about, so there
      is one environment and not two copies of it.
      box-sizing turned out to be the reverse of the expected job: the layout
      engine subtracts padding from a node's size to get its content box, so a
      layout node IS a border box. `border-box` was therefore already right and
      the CSS DEFAULT (content-box) was the broken case -- a stated width is
      the CONTENT, and the border box is that much wider. Pinned side by side
      in tests/web/sizing.html: 200px content-box renders 240 wide, 200px
      border-box renders 200.
      The corpus gained EXPECT-X, which pins a resolved position -- so a
      percentage is tested for the NUMBER it produced (22 + 30% of 896 = 290.8)
      and not merely for not crashing.
- [x] ~~THE BUILD SYSTEM~~ FIXED 2026-08-07, after three of one afternoon's
      four bugs turned out to be build problems rather than code. Four changes,
      each verified to FAIL when its trap is re-introduced:
        1. The drift guard FAILS the build instead of warning. It already
           existed, and on the day build/vellum.elf stopped being rebuilt it
           printed exactly the right warning -- which scrolled past unread. A
           check that cannot stop the build is not a check.
        2. ...and the reverse: every app named in EMBKFS_APPS must EXIST after
           the build, so a recipe that "succeeds" while producing nothing is
           caught rather than packed as an absent file.
        3. -MMD -MP -MF $@.d on every remaining compiler (user, emlibc; newlib
           was done earlier), with `-include build/*.o.d`. Hand-written header
           lists go stale: build/web_jsdom.o did not name style.h, so when
           struct vstyle grew, jsdom kept the OLD layout inside a binary where
           everything else had the new one. The kernel was already safe -- it
           is one compile with every kernel header as a prerequisite, and its
           comment records that trap costing two 35-minute boots.
        4. build/browser_render got real prerequisites. It had NONE, so it only
           rebuilt when missing, and only worked because it was deleted by hand
           before each run. A harness that does not rebuild is worse than no
           harness: it reports the previous build's answer with total
           confidence.
      Verified from a clean tree (2m39s): touching style.h rebuilds
      web_jsdom.o; touching ui/dsl/em.h rebuilds libembk.so AND every app that
      links it (the EmProps-by-value hazard); an orphan .elf in build/ fails
      the build; a deleted app is rebuilt rather than silently dropped.
- [x] ~~C3 REMAINDER: position, overflow, calc()~~ SHIPPED 2026-08-07, except
      float. `position: relative` offsets a box AFTER the flow has placed it,
      so its siblings never notice -- which is the whole difference from
      absolute and the reason relative is safe to apply late. `absolute` and
      `fixed` go out of flow through the engine's existing overlay path, now
      taught CSS's insets: a stated edge pins that side, both edges on an axis
      give the size, neither leaves the box at the content origin.
      `overflow: hidden/auto/scroll` all CLIP (the difference between them is a
      scrollbar this renderer does not draw on an arbitrary box; the clipping
      is the part that changes the layout, and leaving it out is how an
      overflowing box paints across the rest of the page).
      calc() reduces to a LINEAR EXPRESSION -- pct% of the container plus px --
      because the percentage is against a block that does not exist when the
      sheet is read. `calc(100% - 240px)` is width_pct 100 with width -240.
      Real operator precedence: getting `calc(100% - 2 * 20px)` wrong is off by
      exactly one gap, which looks like a rounding bug.
- [x] ~~float / clear~~ SHIPPED 2026-08-07, as a ROW rather than as exclusion
      regions, and the difference is worth stating. CSS floats shorten the LINE
      BOXES of everything that follows, so text wraps around a floated image
      and then RECLAIMS THE FULL WIDTH below it. This renderer has no exclusion
      regions -- an inline run is a wrapping row that knows nothing about boxes
      beside it -- so a float and the content flowing beside it become an
      actual row: [float][the rest], or [the rest][float] for float:right, with
      `clear` ending the row.
      That is exactly right for the two shapes floats are really used in (an
      image with text beside it, and float-based columns) and WRONG in one
      visible way: the text never reclaims the full width below the float, it
      stays in its column. A tall float beside a short paragraph therefore
      leaves a gap that a real browser would fill.
      Doing it properly means teaching the wrap arm about exclusion rects --
      it already walks lines with a y cursor, so it is reachable; the hard part
      is that the floats live in an ancestor block and the text is in a nested
      Flow, i.e. a coordinate-space problem rather than an algorithm one.
- [ ] `position: fixed` is treated as ABSOLUTE against the nearest positioned
      ancestor, not the viewport. The difference only shows when the page
      scrolls under a fixed header. `sticky` is treated as relative.
- [ ] `position: relative` on an INLINE element does nothing -- an inline run
      has no box to offset. Absolute DOES work on one, because CSS blockifies
      an absolutely positioned box and the renderer now does too.
- [ ] z-index is not honoured; paint order is document order. An absolutely
      positioned box written later in the source paints on top, which is what
      most pages expect anyway.
- [x] ~~C4 part 1: the DOM a script BUILDS, and events that bubble~~ SHIPPED
      2026-08-08. document.createElement / createTextNode / body,
      el.appendChild / removeChild / remove / setAttribute, el.className and
      el.classList.{add,remove,toggle,contains}. Nodes come from the SAME
      arenas the parse used, so a script's nodes live exactly as long as the
      document and there is no second lifetime; when an arena is full they FAIL
      and set `truncated` rather than growing into a page's hands. A node
      cannot be appended into its own subtree -- every walker in this browser
      recurses without a visited set.
      Events BUBBLE: click, submit, input and change fire on the node and then
      on each ancestor, with `event.target` staying the node it happened on,
      `currentTarget` the one listening, and stopPropagation ending the walk.
      An event name we cannot deliver is still refused LOUDLY.
      Two bugs found doing it, both of the same shape -- a feature that appears
      to work while being useless:
        * jsdom_has_listener asked only about the node itself, so a DELEGATED
          listener (the way most pages are written) left its children unclickable.
        * render_block returned early for list items, images, tables and
          controls, all BEFORE the clickable-box code -- so a click on an <li>
          inside a listening <ul> was consumed by the ul and arrived with
          event.target set to the ul. Delegation exists precisely to ask which
          item was clicked. The hit box now wraps every display path, and the
          INNERMOST listening box consumes.
- [x] ~~preventDefault, and 'input'/'change' never firing~~ BOTH CLOSED
      2026-08-08. preventDefault is a real veto on `submit`: the browser asks
      after dispatching and does not navigate if a handler said no, which is
      how a page validates a form or submits it with fetch() itself.
      'input'/'change' fire from a per-frame POLL of the value table
      (form_take_changed) rather than a callback, because the toolkit writes
      into the value buffer in place and there is no edit event to hook -- so
      the only way to know is to have kept what the field used to say.
- [x] ~~No checkbox / radio / select~~ SHIPPED 2026-08-08. A boolean control
      keeps a stable bool per node (the toolkit binds a pointer to it) with the
      FORM's copy still the source of truth for submission; the working copy is
      synced in before the control draws and out straight after, so a click is
      visible to form_submit in the same frame. Radios clear their group by
      `name`, which is the whole difference between a radio and a round
      checkbox. A <select> is its <option> children -- labels from their text,
      the submitted value from `value` or the text -- and an <option> is
      display:none so it cannot leak into the page as stray text.
- [x] ~~No cookies~~ SHIPPED 2026-08-08: `user/web/cookie.c`. A cookie is how a
      site remembers you between two requests, and without one every page load
      is a stranger arriving. Sent on every request for a matching host+path,
      taken from Set-Cookie BEFORE a redirect is followed (the hop carries the
      session the next request needs), and exposed as document.cookie with
      HttpOnly cookies EXCLUDED -- that exclusion is the whole security value
      of the flag.
      16 host assertions (T23) pin the scoping rules, and one caught a real
      bug: a cookie with no Domain must be HOST-ONLY. Defaulting it to
      domain-scoped quietly widens every cookie a site sets, handing a session
      set on example.com to any subdomain including one an attacker controls.
      Also pinned: a suffix match must fall on a dot (else evil-example.com
      claims example.com's cookies), /app must not match /applesauce, a host
      cannot set a cookie for another domain, Secure stays off plain http, and
      Max-Age=0 deletes -- which is how logout works.
      Metal-proven against a real server: the browser stored two cookies from
      one response and the next request came back "SERVERSAW sid=SESSION42;
      pref=dark".
- [x] ~~The jar is not persisted / no localStorage / Expires is not parsed~~
      ALL THREE DONE 2026-08-08, and the three questions the last entry raised
      were answered rather than dodged:
        WHERE: $HOME/.vellum, and the browser may name nothing else. vellum.ns
          used to be comments only -- which means INHERIT the whole session --
          so writing the grant down made the browser MORE confined than it was
          when it could not persist at all.
          It is $HOME and not /data/apps/vellum because the session grants
          `ro /data/apps` on purpose: an application rewriting its own
          installed files is what a package manager exists to prevent. A
          browser's cookies are the USER's data anyway.
        WHO MAY READ IT: whoever can name that directory -- the session's own
          tree, the same boundary that separates one user's documents from
          another's.
        WHEN THINGS EXPIRE: by the wall clock (embk_now_unix, the CMOS RTC),
          and only when there is one. An unset clock reads as "no opinion"
          everywhere rather than as 1970, so a machine whose RTC was never set
          does not silently throw away every saved session on boot.
      Expires is parsed as a real RFC 1123 date, with the leap-year rule, and
      checked to the second against independently computed values. Session
      cookies (no expiry) are deliberately NOT saved: they are defined to end
      with the session, and writing them would redefine what the user agreed to.
      Metal-proven across a REBOOT on one disk: keeper=LIVES came back and the
      session cookie did not.
- [ ] The `$HOME` token in a .ns manifest is new and only Vellum uses it. Any
      app keeping per-user state wants it; the alternative was hard-coding a
      user name in a manifest, which is wrong on a machine with two users.
- [ ] A granted prefix must EXIST at spawn time -- the kernel resolves it in
      the PARENT's namespace -- so $HOME/.vellum is shipped by mkfs. An app
      cannot create what it has not been granted, and cannot be granted what
      does not exist. That chicken-and-egg wants a real answer (a spawn-time
      "create if absent" grant?) before the second app hits it.
- [ ] <textarea> renders as a SINGLE-LINE field. It submits correctly and it
      does not look like a text area; a multi-line control needs the toolkit to
      grow one.
- [ ] Custom properties are DOCUMENT-scoped, not element-scoped: one table for
      the sheet, last definition wins. Correct for a single `:root` block (the
      overwhelmingly common shape) and wrong for per-component theming
      (`.dark { --bg: black }` re-theming one panel). Needs the table to hang
      off the element and inherit.
- [ ] Still unsupported in selectors: attribute selectors (`[type=checkbox]`),
      every pseudo-class other than the two above, and pseudo-elements. All are
      SKIPPED rather than dropped -- `a:hover` still styles `a`, which is
      closer to the author's page than no rule.
- [x] ~~No find-in-page~~ SHIPPED 2026-08-08 (C5): Ctrl+F, a bar with a live
      count ("2 of 4"), Prev/Next that WRAP, Enter for next, Esc to close, and
      every match highlighted with the CURRENT one a different colour -- without
      that, "next" moves an indicator you cannot see, which is the one thing
      find has to show.
      `user/web/find.c` reuses the runs select.c already collected rather than
      walking the scene again: two walks would be two chances to disagree about
      what is page text and what is chrome, and the first bug that produces is
      "find highlights the address bar". Matching is case-insensitive and SPANS
      RUNS -- the renderer emits one box per word, so "operating system" is two
      boxes and a matcher confined to one fails on every phrase anyone searches
      for; the page is flattened into one string with a run index per byte.
      A real bug the harness caught: the count on screen was ONE FRAME STALE
      (the view draws it before the post-layout hook rescans) and nothing asked
      for another frame, so typing "browser" left "1 of 24" -- which is the
      answer for "b". The host said 3 and the metal said 24, which is what made
      it obvious.
- [x] ~~Persistent history~~ SHIPPED 2026-08-08. Back/forward was never
      history: it is a stack that dies with the window. `user/web/history.c`
      keeps the list you consult when you cannot remember what a page was
      called, newest first, one entry per URL (revisiting MOVES a row rather
      than adding a second -- a history where an address appears forty times is
      one you cannot read, and reading it is the only thing it is for).
      Persisted through the same store, directory and three answers as the jar.
      Shown as `about:history`, a page the BROWSER writes and then parses with
      its own engine -- which is why there is no history widget anywhere in the
      app. A list of links is a page. It also means the history is styled by
      the same cascade, selectable by the same selection and searchable by the
      same find. Titles are HTML-escaped: a title comes off the network, and
      the browser vouches for this document.
- [x] ~~An inherited colour outranked the user-agent's LINK colour~~ FIXED, and
      it was general rather than cosmetic: `body { color: ... }` inherits into
      every <a>, so on most pages -- most pages set a body colour -- every link
      lost its colouring. CSS gives the UA's `a` rule precedence over
      inheritance, and only a rule naming the link itself beats the UA.
      vstyle now records whether a colour was set ON the element or inherited.
- [ ] TABS, and why they are not done. A tab needs its own parse arenas AND its
      own JS context; jsdom is built around one global g_ctx/g_doc. Doing it by
      re-parsing on switch would re-run every page's scripts, so a tab would
      silently lose its state on every switch -- a feature that looks right and
      behaves wrongly, which is the failure this project keeps catching. It
      wants jsdom made multi-instance first.
- [ ] Still C5: zoom, downloads, favicon. Zoom needs page-level font scaling
      that render.c does not currently express (the toolkit's scale moves the
      chrome too, which is UI zoom and not page zoom -- worth naming rather
      than shipping under the wrong label).
- [ ] Selection is WORD granular, not character. The renderer emits one node per
      word, so that is the grain available without measuring glyph prefixes
      through the font engine on every pointer move. Also missing: double-click
      to select a word, triple-click for a paragraph, shift-click to extend, and
      selecting inside the chrome (address bar / status line -- excluded on
      purpose, the walk only counts nodes inside the document's clip). Copy is
      plain text; there is no text/html flavour. A document past WORD_MAX (4096)
      words has an unselectable tail.
- [ ] JPEG is BASELINE only. Progressive is refused, not half-decoded (it is a
      different decoder: coefficients arrive across multiple scans and the whole
      coefficient buffer must stay live between them). Also no arithmetic
      coding, no CMYK/YCCK, no EXIF orientation, no restart-marker resync after
      corrupt data (a bad scan decodes as far as it can and stops).
- [x] ~~The raster was the remaining scroll cost~~ FIXED: the renderer now has
      a SCROLL BLIT (scene_render.c Step 1s). When a frame is provably a pure
      scroll -- many nodes translated by one shared delta, no content change,
      no vacated ghost, and nothing moved-but-visible outside the scrolled clip
      -- the blitted region is memmove'd and only the exposed strip (plus the
      clip's fractional edge rows) is repainted. Host-proven PIXEL-EXACT
      against a from-scratch render (`make browser-render` asserts it, and the
      check watched the blit path actually being TAKEN).
      ...EXCEPT it was not being taken on any real page, and the check said
      PIXEL-EXACT anyway because the two renders agreed by being the SAME
      render. Fixed 2026-08-07: the classifier tested each moved node's raw
      FOOTPRINT for "moved content visible outside the clip", but a document
      taller than its viewport always has content scrolled past the bottom of
      its container -- clipped away, staining nothing, and vetoing the fast
      path on every page. `gather` now threads the inherited clip down the
      tree so every node carries its VISIBLE extent (foot ∩ ancestor clips),
      and the classifier reasons about that. A moved node that itself CLIPS
      still aborts (its descendants' extents were computed against a clip in
      motion). `make browser-render` now FAILS if the blit path is not
      exercised, and exits non-zero so it is a test rather than a report. Needed a scene-level
      split of dirty into geometry-vs-content (dirty_content), because
      scrolling is itself a transform and was vetoing its own fast path.
- [ ] Metal scroll-FEEL still not verifiable headless: QMP wheel synthesis does
      not reach the guest's USB-HID path. Feel it in an interactive run.
- [ ] The scroll blit keys on ONE clip region per frame; two views scrolling in
      the same frame fall back to the full repaint (correct, just slower).
- [ ] The wrap-height memo keys on ONE width. A node measured at two widths
      alternately (never observed; would need the same text in two differently
      sized parents) would thrash the memo -- correct, just uncached.

### Scroll latency (2026-08-07, after the blit)
- [x] ~~Wheel scroll lagged ~4s behind the hand~~ IMPROVED: usb_core_poll
      drained ONE HID report per kernel tick, and QEMU queues every wheel notch
      as its own report -- a flick took seconds of guest time to trickle in,
      worse under TCG where the guest clock runs slow. The poll now drains the
      whole queue per tick (bounded at 32); the deltas coalesce in the pointer
      state and the app sees one jump. Expected effect: the wheel lag drops to
      roughly one frame (~0.3s under TCG). The user's hunch stands: under KVM
      all of this would be ~10-30x faster and feel instant.
- [ ] UNVERIFIED: Vellum gained Space/b page-scrolling (vellum_key hook +
      ui_any_focus accessor). Compiled and shipped, but the QMP probe showed
      the page NOT moving on Space, cause not yet established -- could be the
      hook not firing, space not delivered as a char to GUI apps, or the
      ui_scroll_end clamp. The wheel path (which users actually use) is
      unaffected either way. Verify by pressing Space in Vellum with the URL
      bar unfocused; if it does not page, instrument vellum_key with a serial
      print first.

### Browser: tabs and zoom (shipped; the parts deliberately left out)

- [ ] A background tab's SCRIPTS do not run and its DOM is rebuilt from source
      when you return to it -- so a clock in a background tab is not ticking,
      and what a script wrote into the DOM (or what you typed into a form)
      is gone on return. This is the stated design in `user/web/tabs.h`, not an
      oversight: one bounded document arena is shared by whichever tab is on
      screen. Lifting it means a document arena per tab, which is the real
      work behind "tabs stay alive".
- [ ] Tabs do not persist across a restart, unlike the cookie jar and
      localStorage. The machinery to do it exists (`store_put_blob`); what is
      missing is a decision about whether restoring a session should re-fetch.
- [ ] TAB_MAX is 6, and each slot reserves a 512KB source buffer up front --
      3MB of the browser's BSS. A tab pool that allocated on demand would cost
      nothing for the common one-or-two-tab case.
- [ ] Switching tabs while a fetch is in flight is REFUSED (the status line
      says so) because the worker's bytes would land in whichever tab is
      current when they arrive. A per-tab fetch slot would remove the refusal.
- [ ] No tab reordering (drag), no "reopen closed tab", no middle-click to
      open a link in a background tab -- `tab_open` deliberately does not steal
      focus, so the model is ready for that; nothing calls it that way yet.
- [ ] Zoom is bound to bare `+`/`-`/`0` rather than `Ctrl+=`/`Ctrl+-`/`Ctrl+0`
      because the keyboard driver turns Ctrl+LETTER into a control byte and
      passes Ctrl+SYMBOL through unchanged (`keyboard.c` is explicit about
      this). The real fix is a driver that reports modifiers alongside symbol
      keys; then the zoom keys can be the chord everyone already knows.
- [ ] Zoom scales text and stated lengths, and is NOT persisted per site --
      every browser remembers zoom per origin, this one remembers it per tab
      for the life of the process.
- [ ] Image and border-width lengths do not scale with zoom; only text and the
      box lengths render.c states (padding, margin, width/height, gap).

### Browser: what a run against 17 real sites turned up (2026-08-08)

Method: fetch real pages WITH their stylesheets, render each through
`build/browser_render`, and rank what breaks. Everything below was found this
way and is not in any corpus page, because the corpus was written by the same
person who wrote the engine. Five bugs were fixed on the spot (deep-tree
segfault, pseudo-elements matching real elements, display:table dropping its
subtree, over-long selectors applying to an ancestor, and the cascade's
unsigned-char rule index). These are what remain.

- [x] ~~STYLE RESOLUTION IS O(rules x elements)~~ -- DONE, plus two costs that
      were bigger and were not where anyone would have guessed. Per pass, on
      the host: Wikipedia 315 -> 88ms, bbc 194 -> 24, python.org 104 -> 12,
      rust-lang 56 -> 2.4, mdn 67 -> 12.5. Three separate fixes, each measured:
      the selector index (rules filed by their subject's most selective name);
      a no-op guard in the reconciler's relink, which had been unlinking every
      child every frame by walking the parent's list to find its predecessor
      (O(N^2) per parent -- 2000 sibling divs went 58.7 -> 5.7ms); and a
      per-pass memo on computed style, because the renderer asked for the same
      element's style from sixteen call sites and was running the whole cascade
      each time.
- [ ] Wikipedia is still the outlier at 88ms a pass, and it is now BUILD, not
      style and not layout (78ms build, 9ms layout). It is also the only page
      that fills the 8192-node arena. Worth a profile with something better
      than gprof, whose call counts on an inlined -O2 build were wrong by two
      orders of magnitude and sent this chase down a wrong path once already.
- [ ] The DOM arena (8192 nodes) truncates Wikipedia and bbc.com. The string
      arena (256KB) truncates bbc.com first -- both are reported as
      "TRUNCATED" without saying which ran out, which cost time to work out.
      Report them separately.
- [ ] The declare layer's instance pool (INST_MAX 8192) overflows on Wikipedia:
      1452 views dropped. Bounded and reported, but it means the bottom of a
      long page is not built. Paging that arena is the fix (already noted at
      the top of declare.c).
- [ ] python.org fails the scroll-blit pixel-exactness check ("pixels differ").
      Not yet diagnosed -- the blit path is taken and disagrees with a full
      repaint, which is a correctness bug in the fast path.
- [ ] `:is()`, `:not()` and `:has()` with a COMMA inside are split on that
      comma by the selector-group scanner, which produces one bogus selector
      per argument. They currently fail to match rather than mis-matching, so
      the damage is limited, but the rules are silently lost.
- [ ] Selector matching keeps the rightmost 6 compounds (99.97% of real
      selectors). Longer ones drop their outermost ancestor constraints and so
      match a SUPERSET -- correct element, looser condition. Documented in
      css_sel_parse rather than fixed.
- [ ] github.com ships 6.9MB of CSS across 41 sheets and 40942 rules. It is
      capped and reported, not crashed, and chasing that number is not the
      plan; noted so the limit is a known one.
- [ ] Several sites serve a bot-check page rather than content to a plain
      fetch. That is not a rendering bug and the triage has to keep separating
      the two: acmqueue was 45 nodes of "enable JavaScript", not a parse
      failure.

### Browser C6: looking at the pages instead of counting them

The previous run proved every real site produced text runs. It could not prove
any of them LOOKED right, because a run count says a word reached the screen
and nothing about where. Rendering them to images found five defects in the
first two pages looked at, all of which had been passing every check.

Fixed: a table no longer inherits text-align (so <center> around a layout
table centres the table, not its cells); link words carry no horizontal
padding (they had 2px a side, setting linked text looser than the text beside
it); whitespace-only text nodes between inline elements are a real space
("tosh7 hours ago"), collapsed into the following word so a line never opens
with one; bgcolor/align/color attributes map to CSS declarations; and a table
cell paints its own background.

Both follow-ups are DONE: every site has been looked at (`make web-real
SHOTS=1`, and the fetcher lives in tools/web_real.py so the run is repeatable),
and the corpus now compares each page against a stored reference image. Looking
found the biggest fidelity bug yet -- the page was drawn with the DESKTOP's
palette, so Wikipedia's own white background carried our light-grey text.

Still open, from looking:
- [ ] Wikipedia now lays out in two columns above its own 1120px breakpoint --
      title, tabs, infobox and sidebar all in place -- but the article's BODY
      text is squeezed into a column a few characters wide, and the header
      overlaps itself. Both look like `position` (sticky/absolute) rather than
      grid; that is the next layer and it has not been diagnosed yet.
- [x] ~~Wikipedia's chrome renders as full-width stacked rows~~ -- FOUR things
      were wrong, none of them the layout engine. The first was not a layout bug at all:
      entities were not decoded in ATTRIBUTE values, so its stylesheet URL
      (`load.php?lang=en&amp;modules=...`) was requested literally and
      MediaWiki replied "no modules were requested" -- 196 bytes instead of
      216KB. The page really was unstyled. With the CSS in, what remains is
      NAMED GRID AREAS: `.mw-page-container-inner` is
      `grid-template-areas: 'siteNotice siteNotice' 'columnStart pageContent'
      'footer footer'` and its children carry `grid-area: columnStart` etc.
      Track SIZES now work; placement by name does not, so the children
      auto-flow and stack. Measured across the seventeen sites:
      grid-template-columns 363 uses / 6 sites (done), grid-area 361 / 3,
      grid-template-areas 125 / 3. That is the next piece and it is bounded:
      parse the area grid on the container, the name on the child, place.
- [ ] python.org draws its no-JS fallback with every dropdown menu in flow and
      overlapping the article -- `position: absolute` on menus that a real
      browser also hides. Untangling which of those two is ours needs a
      separate look.
- [x] ~~FORM CONTROLS come from the theme~~ -- done: the kit takes a scoped
      control palette (ui_set_control_palette), which the browser opens around
      the document the way it opens the zoom bracket. Fields, buttons,
      checkboxes, radios and selects all follow it.
- [x] ~~sqlite.org welds "Reliable.Choose any three."~~ -- it was a <br>, which
      had no style at all and rendered as nothing. It is a block with nothing
      in it now, so it splits the inline run, which is the only line-breaking
      machinery there is.
- [ ] The kit's remaining controls (toggle, slider, stepper, segmented,
      progress) do not consult the control palette. Nothing in a document
      reaches them yet -- <input type=range> is not implemented -- so this is
      a gap that will matter when one does, not a visible bug.
- [ ] The visual references are pixel-exact and the font comes from the host's
      DejaVu, so a machine with a different build of it will see every page
      differ. `make web-corpus BLESS=1` regenerates them; a font-independent
      signature would be better and is not obviously worth the loss of
      precision.
- [ ] Presentational hints ride the INLINE style, so they beat author CSS
      instead of losing to it. Correct for the common case (a page that has
      the attribute and no rule) and wrong for a page whose stylesheet
      contradicts its own bgcolor.
- [ ] HN's rank column is align=right in the markup and now maps to CSS, but
      table cells do not yet honour text-align for their own content in every
      path -- worth checking against the real page.

### Sizes, and why they are all measured now

Every arena that a real page can fill is sized from a count taken across the
seventeen sites rather than from a round number, and every one of them that can
overflow says so:

- DOM nodes 8192 -> 16384 (Wikipedia needs ~9400, MDN ~8100). A full node arena
  STOPS THE DOCUMENT; it is not a truncation, and `trunc_nodes` now says which
  arena ran out so the two are distinguishable.
- Document strings 256KB -> 1MB.
- Declare instances 8192 -> 24576 (Wikipedia builds ~18000 views).
- CSS rules 256 -> 4096, external CSS 128KB -> 1MB, one sheet 64KB -> 512KB.
  A sheet that does not fit is counted (`cssref_dropped`) instead of vanishing.

Vellum's BSS is 28MB as a result, up from 20MB. The remaining known overflow is
github, which ships 6.9MB of CSS across 41 sheets; that bound is reported and
not worth chasing.

### C7: positioning and stacking

- [x] ~~An absolutely positioned box is placed against its IMMEDIATE PARENT~~ --
      fixed. CSS places it against the nearest POSITIONED ancestor, which is
      the entire reason a page writes `position: relative` on a wrapper with no
      offsets of its own. Every dropdown, tooltip and badge on the web is built
      that way, and python.org's menus were landing on top of its article.
      `position: fixed` now lands against the root (the viewport) as a
      consequence, which is what fixed means.
- [ ] Z-INDEX is still not honoured; paint order is document order within a
      layer. Worth saying what it would and would not fix: the overlap left on
      python.org is menus a real browser HIDES until hover, not menus painted
      in the wrong order, so z-index is not what that page is waiting for. The
      scene supports layers 0..3 and CSS z-index is an arbitrary integer, so a
      faithful mapping needs a stacking-context pass, and the page must not be
      able to paint over the browser's own chrome.
- [ ] `position: sticky` is treated as `relative`, so a sticky header scrolls
      away instead of pinning. 68 uses across 6 sites.
- [ ] Negative z-index (paint BEHIND the parent's background) has nowhere to go
      in a layer scheme that starts at the flow.
- [ ] Wikipedia's article body is still squeezed into a narrow column and its
      header overlaps itself, at 206ms a frame with the full stylesheet in.
      Not yet diagnosed; positioning was not the whole of it.

### <details>, <svg>, and the MDN mystery

Looking at the rendered images again found three real bugs and one thing I
could not explain, which is written down here rather than guessed at.

- [x] ~~`<details>` renders every collapsed section expanded~~ -- implemented.
      A disclosure shows its <summary> always and the rest only when open, the
      summary toggles it, and the open set is dropped with the rest of the
      per-page UI state.
- [x] ~~BOOLEAN attributes were never read~~ -- `<details open>` carries no
      value, and the check for it sat inside the branch that only runs when
      there is an `=`. It never fired for any boolean attribute.
- [x] ~~`<svg>` children render as document blocks~~ -- it is a REPLACED
      element; its paths and groups are a different rendering model. MDN's
      83x24 logo came out 3340 pixels tall. It reserves its stated box now.
      Drawing the vector is a renderer this browser does not have.
- [x] ~~MDN'S TEXT IS PLACED OUTSIDE ITS BOXES~~ -- FOUND AND FIXED. A
      PERCENTAGE HEIGHT WAS RESOLVING AGAINST AN AUTO-HEIGHT PARENT.
      `height: 100%` inside a parent whose own height is auto does not mean "as
      tall as the parent" -- the parent has no height yet, it is about to get
      one from those very children. CSS computes such a percentage to auto, and
      that rule is what stops the feedback: without it every child is handed
      the height the parent just measured, they stack, and the parent's height
      no longer matches what is inside it. MDN went 307394px -> 14859px, which
      is its document box. Pinned by tests/web/pct-height.html, which also
      checks that a percentage against a DEFINITE height still resolves.

### C8: real pages at usable speed

The style memo now SURVIVES ACROSS FRAMES. A frame that scrolls changes
nobody's computed style -- same document, same sheet, a different offset -- so
recomputing it was work whose answer was already known. Wikipedia was
re-parsing 2267KB of declaration text every frame for a page nothing had
changed. Per build+layout pass on the host:

    wikipedia  206 -> 37ms      github     42 -> 4.1ms
    bbc         20 -> 5.2ms     mdn        13 -> 5.2ms
    python.org  26 -> 2.2ms     lobste.rs  ~7 -> 1.9ms

Invalidated on the three things that can change an answer: a new document, a
stylesheet arriving, and a script mutating the DOM. Inheritance needs no
announcement -- the key already hashes the parent style, so a changed ancestor
misses by construction. Verified interactively on the metal
(`/system/web/restyle.html`: a click sets a class, the box restyles).

- [ ] Wikipedia is still the outlier at 37ms, and it is LAYOUT now (21ms) not
      build (16ms). measure_wrap_height and layout_measure_height_at_width are
      the top entries after the cascade; both are height-at-width measurements
      that a page of 10900 nodes does a great many of. Whether they are
      redundant has not been established.
- [ ] The style memo is direct-mapped on the node index and sized to NODE_MAX,
      so collisions are impossible rather than rare -- but that ties 2.6MB of
      BSS to the arena size. Both grow together if the arena does.
- [ ] github still parses 5711 rules of a 6.9MB stylesheet. It renders; nobody
      has looked at whether it renders RIGHT.

### C9: the real web, on the machine

Everything before this was verified on a host harness. Vellum now fetches and
renders LIVE SITES on the metal over the OS's own TLS: example.com and Hacker
News both render authenticated, with current content.

Three things were in the way, and only one of them was TLS.

- [x] ~~The trust store had four hand-transcribed roots~~ -- tools/mkroots.py
      generates roots.h from the host's CA bundle, so adding a root is adding a
      line. 22 anchors now. Four roots means a browser reaches the fraction of
      the web that happens to chain to those four.
- [x] ~~example.com refused~~ -- and NOT for the reason assumed. It chains to
      SSL.com TLS ECC Root CA 2022, not DigiCert; one `openssl s_client` said
      so in a second, after a guess had already been implemented.
- [x] ~~A MAKEFILE ORDERING BUG kept shipping a stale browser~~ --
      `TLS_LIB_OBJS` was defined AFTER the rule that lists it as a
      prerequisite, and make expands prerequisites when it reads the rule, so
      it expanded to nothing. Changing a trust anchor rebuilt the object and
      never relinked vellum. The OS kept refusing a site the host verifier
      accepted.

Still open:
- [x] ~~No page has been seen WITH ITS IMAGES~~ -- they are now, on both sides.
      `make web-real` fetches a page's images alongside its stylesheets, and
      the harness could not load ANY of them: it stripped the leading slash off
      every image path to make it repo-relative, which is right for the corpus
      and wrong for an absolute path, so every real page rendered with grey
      boxes. xkcd's comic and kernel.org's icon render on the host; kernel.org
      renders WITH ITS IMAGE on the metal, fetched over the network.

      What the web actually serves, counted across the seventeen sites:
      png 73, webp 28, svg 23, jpeg 13, gif 1. We decode PNG and JPEG.

- [ ] WEBP IS 28 OF 138 IMAGES and decodes as nothing -- bbc serves 12, github
      16. It is the one missing format that matters; GIF is a single image in
      the whole sample and SVG needs a vector renderer, not a decoder.
- [x] ~~TLS 1.2 IS NOT SPOKEN~~ -- it is now, alongside 1.3, in its own files
      (prf12/record12/tls12). One ClientHello offers both; the server's choice
      decides which half of the client runs. xkcd.com renders on the metal.
      Scope: ECDHE + AES-128-GCM + SHA-256, RSA or ECDSA server key, full
      certificate verification plus the ServerKeyExchange signature. No static
      RSA, no CBC, no renegotiation, no resumption.
- [x] ~~Navigating from one page to the next leaves the previous page showing
      through~~ -- and it was not a repaint problem at all, which is what the
      first two attempted fixes assumed. The view was being RECONCILED against
      the page that had been there: a reused instance keeps whatever size
      nobody restated, so the second document came out with the right boxes and
      the previous page's text positions. The document's container is keyed by
      a per-load counter now, so a new page is a new subtree. A fresh load was
      always correct, which is exactly why only the second navigation showed
      it.
- [ ] The trust set is 23 anchors and five of the listed roots are not in the
      host's bundle at all (DigiCert Global Root CA, High Assurance EV,
      Baltimore, GTS R2, Entrust G2) -- so the generator's list should be
      checked against what a current bundle actually holds rather than from
      memory of which CAs matter.
- [ ] Only two live sites have been tried. The seventeen-site corpus is
      fetched by curl and rendered on the host; the same list run on the metal
      would be a different test and would find different things (memory at
      521MB, TCG speed, decode cost).
- [ ] No revocation checking of any kind, and the trust set is compiled in.
      docs/TLS.md already logs both; the packaging story is where a
      user-growable, verified-boot-sealed root set belongs.

### A ring-3 backtrace

The kernel prints a symbolized backtrace on a KERNEL fault and stopped at the
kernel's own .text, which is no help at all when the faulting code is an
application -- and "RIP=0" says a call went through a null pointer without
saying whose. It walks the user rbp chain now, through access_ok so a corrupt
frame ends the walk rather than faulting the fault handler, and prints raw
addresses; `nm build/<app>.elf` names them, and an ET_EXEC app loads at bias 0
so they match directly.

It found a bug in one boot that an afternoon of elimination had not:
`gctr <- gcm_seal <- tls_record_seal <- tls_close <- vnet_fetch`.
