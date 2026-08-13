/* user/audio/mp3/bits.h -- reading a stream that does not respect bytes.
 *
 * Almost everything in an MP3 frame is packed to the BIT: a scalefactor may be
 * three bits, a Huffman code seventeen, and neither cares where a byte ends.
 * So the whole decoder is written against this rather than against pointers,
 * and every "off by one" that would otherwise be an off-by-one BIT lives in
 * one file that can be tested on its own.
 *
 * Reading past the end returns ZEROES and sets an overrun flag rather than
 * faulting. That is deliberate: an MP3 is untrusted input -- a truncated or
 * malicious frame will claim more bits than it has -- and the decoder's job is
 * to produce silence for a bad frame and carry on, not to crash. The flag is
 * how the caller finds out it happened.
 */
#ifndef _EMBLINK_MP3_BITS_H_
#define _EMBLINK_MP3_BITS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    const uint8_t *data;
    size_t         size;      /* bytes available                    */
    size_t         pos;       /* bit position from the start        */
    bool           overrun;   /* a read ran past the end            */
} BitReader;

void     bits_init(BitReader *b, const uint8_t *data, size_t size);
/* Up to 32 bits, MSB first -- the order everything in MPEG is stored in. */
uint32_t bits_read(BitReader *b, int n);
/* Look without consuming; same clamping rules. */
uint32_t bits_peek(const BitReader *b, int n);
void     bits_skip(BitReader *b, int n);
size_t   bits_left(const BitReader *b);
/* Where we are, in bits -- the bit reservoir needs to seek to an absolute
 * position that belongs to an EARLIER frame's data. */
static inline size_t bits_pos(const BitReader *b) { return b->pos; }
static inline void   bits_seek(BitReader *b, size_t bitpos) { b->pos = bitpos; }

#endif /* _EMBLINK_MP3_BITS_H_ */
