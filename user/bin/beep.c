/* user/bin/beep.c -- the first program on this OS to make a noise.
 *
 * The point is not the tone. It is that a PROGRAM asked for the speaker,
 * the kernel checked whether it was allowed, and the sound came out -- which
 * is the whole path the audio syscalls exist for, and the first use of
 * EMBK_CAP_AUDIO, a capability that has been defined and empty since the
 * model was written.
 *
 *   beep            a 440 Hz note for half a second
 *   beep 880 250    frequency in Hz, duration in ms
 *
 * Verified by `make test-audio` measuring the WAV, not by listening.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "embk.h"

/* One buffer's worth at a time. The kernel copies through a staging buffer of
 * its own and will not take more than this per call, so asking for more just
 * means more short writes. */
#define CHUNK_FRAMES 1024

int main(int argc, char **argv)
{
    int hz = argc > 1 ? atoi(argv[1]) : 440;
    int ms = argc > 2 ? atoi(argv[2]) : 500;
    if (hz < 20 || hz > 20000) hz = 440;
    if (ms < 10 || ms > 10000) ms = 500;

    uint32_t rate = embk_audio_rate();
    if ((int)rate <= 0) {
        fprintf(stderr, "beep: no audio device (or no `audio` capability)\n");
        return 1;
    }

    int rc = embk_audio_open();
    if (rc < 0) {
        /* -EBUSY here means another program owns the speaker. Saying so beats
         * playing nothing and exiting 0, which is what a program that ignores
         * this return does. */
        fprintf(stderr, "beep: cannot open the audio device (%d)\n", rc);
        return 1;
    }

    uint32_t total = (uint32_t)((uint64_t)rate * (uint32_t)ms / 1000);
    uint32_t period = rate / (uint32_t)hz;
    if (period < 2) period = 2;

    static int16_t chunk[CHUNK_FRAMES * 2];
    uint32_t done = 0;
    uint32_t spins = 0;

    printf("beep: %d Hz for %d ms at %u Hz sample rate\n", hz, ms, rate);

    while (done < total) {
        uint32_t n = total - done;
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;

        for (uint32_t f = 0; f < n; f++) {
            /* A square wave: exact fundamental, no libm, and the host-side
             * checker recovers its frequency by counting zero crossings. */
            int16_t v = ((done + f) % period) < (period / 2) ? 8000 : -8000;
            chunk[f * 2 + 0] = v;
            chunk[f * 2 + 1] = v;
        }

        int took = embk_audio_write(chunk, n);
        if (took < 0) {
            fprintf(stderr, "beep: write failed (%d)\n", took);
            embk_audio_close();
            return 1;
        }
        if (took == 0) {
            /* The ring is full, which is the NORMAL state of a writer that is
             * ahead of the speaker. Yield rather than spin: this is the point
             * where a busy loop would eat the core the audio is playing on. */
            if (++spins > 100000) {
                fprintf(stderr, "beep: the device stopped consuming\n");
                break;
            }
            embk_sleep_ms(2);
            continue;
        }
        spins = 0;
        done += (uint32_t)took;
    }

    /* WAIT FOR IT TO FINISH. Closing while the hardware still holds buffers
     * cuts the end off the sound -- and exiting does the same, because the
     * kernel reclaims the device from a dead process. */
    for (int i = 0; i < 5000 && !embk_audio_drained(); i++) embk_sleep_ms(2);

    embk_audio_close();
    printf("beep: played %u frames\n", done);
    return 0;
}
