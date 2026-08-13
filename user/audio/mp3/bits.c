/* user/audio/mp3/bits.c -- see bits.h. */
#include "bits.h"

void bits_init(BitReader *b, const uint8_t *data, size_t size)
{
    b->data = data;
    b->size = size;
    b->pos = 0;
    b->overrun = false;
}

size_t bits_left(const BitReader *b)
{
    size_t total = b->size * 8;
    return b->pos >= total ? 0 : total - b->pos;
}

/* One bit at a time.
 *
 * The obvious optimisation is to keep a 64-bit window and refill it, and it is
 * a real speedup on a decoder that reads millions of bits. It is also where
 * the subtle bugs live -- refill boundaries, the last partial byte, a seek
 * that lands mid-window -- and the bit reservoir SEEKS BACKWARDS into a
 * previous frame's bytes, which is exactly the case a window gets wrong.
 * Correct first. If the profile says this matters, the window can be added
 * behind these same four functions with the tests already written. */
static inline uint32_t get1(BitReader *b)
{
    size_t byte = b->pos >> 3;
    if (byte >= b->size) { b->overrun = true; return 0; }
    uint32_t bit = (b->data[byte] >> (7 - (b->pos & 7))) & 1u;
    b->pos++;
    return bit;
}

uint32_t bits_read(BitReader *b, int n)
{
    uint32_t v = 0;
    if (n <= 0) return 0;
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) v = (v << 1) | get1(b);
    return v;
}

uint32_t bits_peek(const BitReader *b, int n)
{
    BitReader copy = *b;                 /* by value: peeking must not move  */
    return bits_read(&copy, n);
}

void bits_skip(BitReader *b, int n)
{
    if (n <= 0) return;
    b->pos += (size_t)n;
    if (b->pos > b->size * 8) { b->pos = b->size * 8; b->overrun = true; }
}
