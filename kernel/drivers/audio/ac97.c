/* kernel/drivers/audio/ac97.c -- sound out, on the Intel 82801AA AC'97.
 *
 * The smallest device on this machine that makes a noise. Two BARs of I/O
 * ports (a mixer and a bus master), a ring of up to 32 buffer descriptors in
 * physical memory, and one interrupt when a buffer is done. No codec graph to
 * walk, no command ring to arbitrate -- which is why this and not Intel HDA is
 * the first audio driver here: HDA is what real hardware has and is three or
 * four times the work, and none of that work is about whether the rest of the
 * OS can carry a sample from a program to a speaker.
 *
 * WHAT THE HARDWARE WANTS, in order, because every AC'97 driver that does not
 * work has skipped one of these:
 *   1. bus mastering enabled in PCI command (the device DMAs; without this it
 *      silently reads nothing)
 *   2. the codec reset and unmuted -- it comes up MUTED with volume at zero,
 *      so a correct driver with no mixer writes produces perfect silence
 *   3. a Buffer Descriptor List whose entries are PHYSICAL addresses and whose
 *      lengths are in SAMPLES, not bytes. It is a 16-bit count of 16-bit
 *      samples, so the largest descriptor is 128 KiB and the unit is the thing
 *      most often got wrong.
 *   4. Last Valid Index written LAST -- that is what starts the DMA walking.
 *
 * Fixed at 48 kHz, 16-bit, stereo: that is what AC'97 does natively without
 * the variable-rate extension, and resampling belongs above a device driver.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/io.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/bus/pci.h"
#include "mm/pmm.h"

#define AC97_VENDOR_INTEL   0x8086
#define AC97_DEVICE_82801AA 0x2415

/* --- mixer registers, in BAR0's I/O space --- */
#define AC97_RESET          0x00   /* any write resets the codec            */
#define AC97_MASTER_VOL     0x02   /* 0 = loudest; bit 15 = mute            */
#define AC97_PCM_VOL        0x18   /* the PCM-out path's own attenuation    */

/* --- bus master registers, in BAR1's I/O space. PCM OUT is the 0x10 box --- */
#define AC97_PO_BDBAR       0x10   /* physical address of the descriptor list */
#define AC97_PO_CIV         0x14   /* current index (read-only)               */
#define AC97_PO_LVI         0x15   /* last valid index -- writing this starts  */
#define AC97_PO_SR          0x16   /* status; write-1-to-clear                */
#define AC97_PO_PICB        0x18   /* position in current buffer, in samples  */
#define AC97_PO_CR          0x1B   /* control                                 */
#define AC97_GLOB_CNT       0x2C   /* global control: cold reset, interrupts  */

#define AC97_CR_RPBM        0x01   /* run/pause bus master: 1 = run          */
#define AC97_CR_RR          0x02   /* reset registers                        */
#define AC97_CR_IOCE        0x10   /* interrupt on completion enable         */
#define AC97_SR_BCIS        0x08   /* buffer completion interrupt status     */
#define AC97_SR_LVBCI       0x04   /* last valid buffer completed            */

#define AC97_BDL_ENTRIES    32
#define AC97_SAMPLE_RATE    48000

/* One descriptor: a physical address and a SAMPLE count, plus two control
 * bits in the top of the second word. Packed because the hardware walks it. */
struct ac97_bd {
    uint32_t addr;                 /* physical address of the PCM data      */
    uint16_t samples;              /* number of 16-bit SAMPLES, not bytes   */
    uint16_t flags;                /* bit 15 = interrupt on completion      */
} __attribute__((packed));

#define AC97_BD_IOC  0x8000

static struct {
    bool     present;
    uint16_t mixer;                /* BAR0 I/O base */
    uint16_t bus;                  /* BAR1 I/O base */
    struct ac97_bd *bdl;           /* virtual pointer into the direct map   */
    uint64_t bdl_phys;
    uint64_t buf_phys[AC97_BDL_ENTRIES];
    int16_t *buf[AC97_BDL_ENTRIES]; /* one page each, as samples            */
    uint32_t buf_samples;          /* samples per buffer (both channels)    */
} g_ac97;

/* AC'97 codec registers are reached through BAR0's ports directly on this
 * controller -- there is no index/data pair to sequence. */
static void mixer_write(uint8_t reg, uint16_t value)
{
    outw((uint16_t)(g_ac97.mixer + reg), value);
}

static uint16_t mixer_read(uint8_t reg)
{
    return inw((uint16_t)(g_ac97.mixer + reg));
}

bool ac97_present(void) { return g_ac97.present; }
uint32_t ac97_sample_rate(void) { return AC97_SAMPLE_RATE; }

/* How many stereo FRAMES one descriptor's buffer holds. */
uint32_t ac97_frames_per_buffer(void)
{
    return g_ac97.buf_samples / 2;
}

/* Fill descriptor `i` from `frames` stereo frames of interleaved S16. Returns
 * how many frames were taken -- the caller keeps the rest for the next one. */
uint32_t ac97_fill(int i, const int16_t *frames, uint32_t nframes)
{
    if (!g_ac97.present || i < 0 || i >= AC97_BDL_ENTRIES) return 0;

    uint32_t cap = ac97_frames_per_buffer();
    uint32_t take = nframes < cap ? nframes : cap;

    memcpy(g_ac97.buf[i], frames, (size_t)take * 2 * sizeof(int16_t));
    /* Silence the tail rather than leaving the previous buffer's audio in it:
     * a short final block otherwise repeats whatever was there, which is the
     * click at the end of every sound a half-finished driver plays. */
    if (take < cap) {
        memset(g_ac97.buf[i] + take * 2, 0,
               (size_t)(cap - take) * 2 * sizeof(int16_t));
    }

    g_ac97.bdl[i].addr    = (uint32_t)g_ac97.buf_phys[i];
    g_ac97.bdl[i].samples = (uint16_t)(cap * 2);   /* SAMPLES, both channels */
    g_ac97.bdl[i].flags   = AC97_BD_IOC;
    return take;
}

/* Which descriptor the device is playing RIGHT NOW. Everything above this
 * file schedules against it: a ring that writes past the current index
 * overwrites audio that has not been heard yet. */
uint8_t ac97_civ(void)
{
    if (!g_ac97.present) return 0;
    return inb((uint16_t)(g_ac97.bus + AC97_PO_CIV));
}

/* Extend how far the list is valid, without restarting. This is how a stream
 * differs from a one-shot: the device keeps walking and the writer keeps
 * moving the goalpost ahead of it. */
void ac97_set_last(int last)
{
    if (!g_ac97.present) return;

    /* CLEAR THE STATUS FIRST. When the device reaches LVI it halts and latches
     * "last valid buffer completed"; while that bit is set, writing a new LVI
     * and re-asserting RUN does not restart it. The one-shot path never sees
     * this -- it fills every descriptor before starting -- so the bug lives
     * exactly where a program streams, which is what `beep` found: 0.03s of
     * audio out of half a second, and the checker called it silence. */
    uint16_t sr = inw((uint16_t)(g_ac97.bus + AC97_PO_SR));
    bool halted = (sr & AC97_SR_LVBCI) != 0;

    /* MOVE THE GOALPOST FIRST, and only touch anything else if the device has
     * actually stopped. Rewriting CR on every extend is what kept a stream at
     * the length of its prefill no matter how much was queued after it: the
     * control write restarts the run rather than continuing it, so the device
     * walked the descriptors (CIV reached 29 of a 94-buffer stream) while the
     * speaker only ever heard the first burst. A one-shot never showed it --
     * it writes CR once. */
    outb((uint16_t)(g_ac97.bus + AC97_PO_LVI), (uint8_t)last);

    if (halted) {
        /* It DID reach the old end and stop. Clear the latch -- while it is
         * set, a new LVI does not restart anything -- and run again. An
         * underrun costs a gap, not the rest of the sound. */
        outw((uint16_t)(g_ac97.bus + AC97_PO_SR), (uint16_t)(sr & (AC97_SR_BCIS | AC97_SR_LVBCI)));
        outb((uint16_t)(g_ac97.bus + AC97_PO_CR), AC97_CR_IOCE | AC97_CR_RPBM);
    } else if (sr & AC97_SR_BCIS) {
        /* Buffer-completed is routine bookkeeping and must be cleared, but it
         * is NOT a reason to touch the control register. */
        outw((uint16_t)(g_ac97.bus + AC97_PO_SR), AC97_SR_BCIS);
    }
}

/* Start the DMA walking descriptors 0..last inclusive. */
void ac97_play(int last)
{
    if (!g_ac97.present) return;

    outl((uint16_t)(g_ac97.bus + AC97_PO_BDBAR), (uint32_t)g_ac97.bdl_phys);
    outb((uint16_t)(g_ac97.bus + AC97_PO_CR), AC97_CR_IOCE);
    /* CLEAR THE LATCHED STATUS BEFORE STARTING. "Last valid buffer completed"
     * stays set from whatever ran before, and ac97_done() reads it -- so a
     * fresh playback reported itself finished the instant it began, the writer
     * closed the device, and the sound was cut to whatever had been prefilled.
     * Exactly 0.18s of a 0.5s beep, which is the prefill and nothing else. */
    outw((uint16_t)(g_ac97.bus + AC97_PO_SR), AC97_SR_BCIS | AC97_SR_LVBCI);
    /* LVI LAST: this is the write that tells the device how far the list is
     * valid, and therefore the one that starts it moving. */
    outb((uint16_t)(g_ac97.bus + AC97_PO_LVI), (uint8_t)last);
    outb((uint16_t)(g_ac97.bus + AC97_PO_CR), AC97_CR_IOCE | AC97_CR_RPBM);
}

/* True once the device has walked past the last descriptor we gave it. */
bool ac97_done(int last)
{
    if (!g_ac97.present) return true;
    uint8_t civ = inb((uint16_t)(g_ac97.bus + AC97_PO_CIV));
    uint16_t sr = inw((uint16_t)(g_ac97.bus + AC97_PO_SR));
    if (sr & (AC97_SR_BCIS | AC97_SR_LVBCI))
        outw((uint16_t)(g_ac97.bus + AC97_PO_SR), sr);  /* write-1-to-clear */
    bool done = civ > (uint8_t)last || (sr & AC97_SR_LVBCI) != 0;
    if (done) kprintf("ac97: done civ=%u last=%d sr=0x%x\n",
                      (unsigned)civ, last, (unsigned)sr);
    return done;
}

void ac97_stop(void)
{
    if (!g_ac97.present) return;
    outb((uint16_t)(g_ac97.bus + AC97_PO_CR), 0);
}

void ac97_init(void)
{
    uint32_t n = pci_devices_count();
    const struct pci_device *dev = NULL;

    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (d != NULL && d->vendor_id == AC97_VENDOR_INTEL &&
            d->device_id == AC97_DEVICE_82801AA) { dev = d; break; }
    }
    if (dev == NULL) {
        kprintf("ac97: no controller (add -device AC97 to the QEMU line)\n");
        return;
    }

    struct pci_bar b0 = pci_read_bar(dev->bus, dev->device, dev->function, 0);
    struct pci_bar b1 = pci_read_bar(dev->bus, dev->device, dev->function, 1);
    if (!b0.valid || !b1.valid || b0.is_mmio || b1.is_mmio) {
        kprintf("ac97: expected two I/O BARs, got mmio=%d/%d\n",
                (int)b0.is_mmio, (int)b1.is_mmio);
        return;
    }
    g_ac97.mixer = (uint16_t)b0.address;
    g_ac97.bus   = (uint16_t)b1.address;

    /* The device DMAs its own audio: without bus mastering it reads nothing
     * and plays silence while every register looks correct. */
    pci_enable_bus_mastering(dev->bus, dev->device, dev->function);

    /* Cold reset, then reset the codec. */
    outl((uint16_t)(g_ac97.bus + AC97_GLOB_CNT), 0x00000002);
    mixer_write(AC97_RESET, 0);

    /* UNMUTE. An AC'97 codec powers up muted with attenuation at maximum, so
     * a driver that never touches the mixer is a driver that works perfectly
     * and is inaudible. 0 is loudest; bit 15 is the mute. */
    mixer_write(AC97_MASTER_VOL, 0x0000);
    mixer_write(AC97_PCM_VOL,    0x0000);

    /* The descriptor list, and one page of PCM behind each entry. Physical
     * addresses, because the device walks this itself and knows nothing about
     * our page tables. */
    uint64_t bdl_page = pmm_alloc_page();
    if (!bdl_page) { kprintf("ac97: no page for the descriptor list\n"); return; }
    g_ac97.bdl_phys = bdl_page;
    g_ac97.bdl = (struct ac97_bd *)P2V(bdl_page);
    memset(g_ac97.bdl, 0, PAGE_SIZE);

    g_ac97.buf_samples = PAGE_SIZE / sizeof(int16_t);   /* 2048 samples/page */
    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) { kprintf("ac97: only %d buffers\n", i); break; }
        g_ac97.buf_phys[i] = p;
        g_ac97.buf[i] = (int16_t *)P2V(p);
        memset(g_ac97.buf[i], 0, PAGE_SIZE);
    }

    g_ac97.present = true;
    kprintf("ac97: 82801AA at mixer 0x%x bus 0x%x, %u Hz, %u frames/buffer\n",
            g_ac97.mixer, g_ac97.bus, AC97_SAMPLE_RATE,
            ac97_frames_per_buffer());
    kprintf("ac97: codec id 0x%x\n", mixer_read(AC97_RESET));
}
