/* kernel/drivers/audio/audio.h -- the PCM sink the syscalls sit on.
 *
 * Device-independent on purpose: everything here is about a stream and its
 * owner, and nothing about ports or descriptors. A second driver implements
 * ac97.h; this file does not change.
 *
 * Format is fixed at the device's own: 16-bit signed, stereo, interleaved, at
 * audio_sample_rate(). Resampling and mixing are a level above a sink that
 * has one owner, and doing them badly in the kernel is worse than not doing
 * them -- see audio.c on why there is no mixer yet.
 */
#ifndef _EMBK_AUDIO_H_
#define _EMBK_AUDIO_H_

#include <stdint.h>
#include "include/types.h"

bool     audio_available(void);
uint32_t audio_sample_rate(void);
uint32_t audio_frames_per_buffer(void);

/* Claim the device. -EMBK_EBUSY if another process holds it, -EMBK_ENODEV if
 * there is no hardware. Re-opening by the same pid is not an error. */
int  audio_open(uint32_t pid);

/* Queue interleaved stereo S16. Writes what FITS and reports it in *accepted:
 * a short write means the ring is full, not that anything failed. */
int  audio_write(uint32_t pid, const int16_t *frames, uint32_t nframes,
                 uint32_t *accepted);

/* Has the hardware played everything handed to it? */
bool audio_drained(uint32_t pid);

void audio_close(uint32_t pid);
void audio_reap_pid(uint32_t pid);   /* a process died holding the device */

#endif
