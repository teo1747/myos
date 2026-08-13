/* kernel/drivers/audio/audio.c -- one PCM sink, and who is allowed to use it.
 *
 * The device-independent half. ac97.c knows about I/O ports and descriptor
 * lists; this knows about a stream: who owns it, how far the hardware has got,
 * and what to do when a program writes faster or slower than the speaker
 * consumes. A second driver (Intel HDA, virtio-sound) implements ac97.h's four
 * operations and nothing here changes.
 *
 * ONE OWNER AT A TIME, deliberately. Mixing several streams means resampling,
 * summing and clipping policy, and a mixer that is wrong is worse than no
 * mixer -- it makes every program quieter and none of them correct. A second
 * opener is REFUSED with a real error rather than silently sharing, so the
 * failure is visible at the call that caused it. The owner is a pid, so a
 * process that dies holding the device does not keep it forever.
 *
 * THE RING is the driver's 32 descriptors used as a circle. `head` is the next
 * descriptor to fill; the hardware's CIV says which it is playing. Writing is
 * allowed while there is a gap between them -- and the gap is what makes this
 * a stream rather than a one-shot, because the writer keeps moving LVI ahead
 * of a device that never stops.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "drivers/audio/ac97.h"
#include "drivers/audio/audio.h"

#define RING          32          /* must match the driver's descriptor count */
/* Never fill the descriptor being played, or the one after it: the device may
 * already have prefetched the next, and rewriting it is a click the writer
 * cannot hear and cannot explain. */
#define RING_HEADROOM 2

/* PREFILL BEFORE STARTING. Starting the hardware on the first buffer means it
 * reaches the end of that buffer, halts at LVI, and waits for the writer --
 * once per buffer, for the whole sound. That is not a click, it is a stutter
 * with silence between every 21ms of audio, and under TCG (where the guest is
 * slower than real time) it is most of the output: `beep` measured 46%
 * non-silent and the checker read the gaps as HALF the frequency.
 *
 * So the device does not start until there is runway behind it. Eight buffers
 * is ~170ms at 48kHz, which is enough for the writer to stay ahead, and it is
 * the difference between a stream and a series of one-shots. */
#define RING_START_MIN 8

static struct {
    bool     open;
    uint32_t owner_pid;
    int      head;                /* next descriptor to fill                  */
    int      last;                /* highest index handed to the hardware     */
    int      queued;              /* descriptors filled but not yet started   */
    bool     started;
    uint64_t frames_written;
} g_au;

bool audio_available(void)
{
    return ac97_present();
}

uint32_t audio_sample_rate(void)
{
    return ac97_sample_rate();
}

uint32_t audio_frames_per_buffer(void)
{
    return ac97_frames_per_buffer();
}

int audio_open(uint32_t pid)
{
    if (!ac97_present()) return -EMBK_ENODEV;
    /* Re-opening by the SAME process is not an error -- a program that
     * restarts its own playback should not have to know whether it closed. */
    if (g_au.open && g_au.owner_pid != pid) return -EMBK_EBUSY;

    g_au.open = true;
    g_au.owner_pid = pid;
    g_au.head = 0;
    g_au.last = -1;
    g_au.queued = 0;
    g_au.started = false;
    g_au.frames_written = 0;
    return EMBK_OK;
}

/* How many descriptors are free to write into right now. */
static int ring_free(void)
{
    if (!g_au.started) return RING - RING_HEADROOM;

    int civ = (int)ac97_civ();
    int used = g_au.head - civ;
    if (used < 0) used += RING;
    int freed = RING - used - RING_HEADROOM;
    return freed > 0 ? freed : 0;
}

int audio_write(uint32_t pid, const int16_t *frames, uint32_t nframes,
                uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!ac97_present()) return -EMBK_ENODEV;
    if (!g_au.open || g_au.owner_pid != pid) return -EMBK_EPERM;
    if (frames == NULL) return -EMBK_EINVAL;

    uint32_t taken = 0;
    int slots = ring_free();

    while (slots > 0 && taken < nframes) {
        uint32_t n = ac97_fill(g_au.head, frames + (size_t)taken * 2,
                               nframes - taken);
        if (n == 0) break;
        taken += n;
        g_au.last = g_au.head;
        g_au.head = (g_au.head + 1) % RING;
        g_au.queued++;
        slots--;
    }

    if (taken > 0) {
        g_au.frames_written += taken;
        /* Start on the FIRST write, extend on every one after. Starting from
         * audio_open would run the device over empty descriptors and put a
         * burst of silence at the head of every sound. */
        if (!g_au.started) {
            /* Hold until there is enough queued to keep the device fed. A
             * writer with less than this in total gets it started by
             * audio_drained(), which is what "I have finished writing" means
             * from out here. */
            if (g_au.queued >= RING_START_MIN) {
                ac97_play(g_au.last);
                g_au.started = true;
            }
        } else {
            ac97_set_last(g_au.last);
        }
    }

    if (accepted) *accepted = taken;
    /* Short writes are NORMAL and are not an error: the ring is full and the
     * caller should come back. Saying EAGAIN for a partial accept would make
     * every well-behaved writer look like it failed. */
    return EMBK_OK;
}

/* Has the hardware finished everything handed to it? */
bool audio_drained(uint32_t pid)
{
    if (!ac97_present() || !g_au.open || g_au.owner_pid != pid) return true;
    if (!g_au.started) {
        /* Asking whether it drained is the writer saying it has stopped
         * writing -- so a sound shorter than the prefill threshold plays HERE
         * rather than never. Without this a 50ms beep is silent. */
        if (g_au.queued > 0) { ac97_play(g_au.last); g_au.started = true; return false; }
        return true;
    }
    return ac97_done(g_au.last);
}

void audio_close(uint32_t pid)
{
    if (!g_au.open || g_au.owner_pid != pid) return;
    /* What the stream actually carried, on the serial log. The WAV says what
     * came OUT; this says what went IN, and the difference between them is the
     * only way to tell a writer that gave up from a device that stopped. */
    kprintf("audio: stream closed after %llu frames (%llu ms)\n",
            (unsigned long long)g_au.frames_written,
            (unsigned long long)(g_au.frames_written * 1000 /
                                 (ac97_sample_rate() ? ac97_sample_rate() : 1)));
    ac97_stop();
    g_au.open = false;
    g_au.started = false;
    g_au.owner_pid = 0;
}

/* Called when a process dies: a program that exits mid-note must not leave the
 * speaker running and the device claimed. The compositor learned this lesson
 * with windows (compositor_reap_pid); sound is the same shape. */
void audio_reap_pid(uint32_t pid)
{
    if (g_au.open && g_au.owner_pid == pid) {
        kprintf("audio: reclaiming the device from pid %u\n", pid);
        audio_close(pid);
    }
}
