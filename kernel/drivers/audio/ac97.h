/* kernel/drivers/audio/ac97.h -- the AC'97 PCM-out contract.
 *
 * Deliberately narrow: fill descriptors, start, ask if it finished, stop.
 * Everything about mixing, resampling and who is allowed to make a noise
 * belongs above this, in the device-independent layer -- so that a second
 * driver (Intel HDA, virtio-sound) implements these four operations and
 * nothing else has to change.
 */
#ifndef _EMBK_AC97_H_
#define _EMBK_AC97_H_

#include <stdint.h>
#include "include/types.h"

void     ac97_init(void);
bool     ac97_present(void);
uint32_t ac97_sample_rate(void);

/* Stereo frames one descriptor holds. A frame is one sample per channel. */
uint32_t ac97_frames_per_buffer(void);

/* Copy interleaved S16 stereo into descriptor `i`; returns frames taken. */
uint32_t ac97_fill(int i, const int16_t *frames, uint32_t nframes);

uint8_t  ac97_civ(void);      /* descriptor being played now */
void     ac97_set_last(int last);  /* extend the valid range, keep running */

void ac97_play(int last);     /* walk descriptors 0..last, inclusive */
bool ac97_done(int last);     /* has it walked past `last` yet?      */
void ac97_stop(void);

#endif
