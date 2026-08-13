/* user/audio/mp3/frame.h -- finding the frames, before decoding any of them.
 *
 * An MP3 file is a sequence of self-describing frames with no index and no
 * container: each starts with eleven set bits, and where the next one starts
 * is COMPUTED from the header of this one. That makes the frame layer
 * self-checking in a way most parsers are not -- if the bitrate, sample rate
 * or padding bit is read wrong, the computed length is wrong, and the next
 * sync word is not where it was predicted to be. A parser that walks a whole
 * file frame to frame without ever hunting for a sync word is a parser that
 * got every one of those fields right, thousands of times in a row.
 *
 * That property is the test (see mp3_test.c). It is worth more than any
 * assertion I could write by hand, because it checks the fields against the
 * file's own arithmetic rather than against my reading of the spec.
 */
#ifndef _EMBLINK_MP3_FRAME_H_
#define _EMBLINK_MP3_FRAME_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* MPEG version and layer, as stored (the encodings are not sequential and the
 * spec's own numbering is confusing, so they are named here once). */
enum { MPEG_2_5 = 0, MPEG_RESERVED = 1, MPEG_2 = 2, MPEG_1 = 3 };
enum { LAYER_RESERVED = 0, LAYER_III = 1, LAYER_II = 2, LAYER_I = 3 };

/* Channel modes. Layer III's joint stereo carries MS and/or intensity coding,
 * which the decoder must undo -- so the mode is not merely informational. */
enum { MODE_STEREO = 0, MODE_JOINT = 1, MODE_DUAL = 2, MODE_MONO = 3 };

typedef struct {
    int  version;             /* MPEG_1 / MPEG_2 / MPEG_2_5             */
    int  layer;               /* LAYER_III etc                          */
    bool crc;                 /* a 16-bit CRC follows the header        */
    int  bitrate;             /* bits per second, already resolved      */
    int  samplerate;          /* Hz, already resolved                   */
    bool padding;
    int  mode;                /* MODE_*                                 */
    int  mode_ext;            /* joint-stereo detail: MS / intensity    */
    int  channels;            /* 1 or 2                                 */
    int  samples;             /* PCM frames this frame decodes to       */
    int  frame_bytes;         /* including the 4 header bytes           */
    int  side_info_bytes;     /* how much follows the header/CRC        */
} Mp3Header;

/* Parse 4 bytes at `p`. Returns false if it is not a valid Layer III header --
 * including the reserved encodings, which are rejected rather than guessed at,
 * because guessing is how a parser resynchronises onto garbage and produces
 * noise instead of an error. */
bool mp3_parse_header(const uint8_t *p, size_t avail, Mp3Header *out);

/* Find the next frame at or after `from`. Returns its offset, or -1.
 * Hunting is the FALLBACK, not the method: the normal path computes the next
 * frame's position and confirms the sync word is there. */
long mp3_find_sync(const uint8_t *data, size_t size, size_t from);

/* Skip an ID3v2 tag if the file starts with one -- almost every real file
 * does, and a decoder that starts at byte 0 finds "ID3" where it wants a sync
 * word and either fails or resynchronises into the middle of the tag. */
size_t mp3_skip_id3(const uint8_t *data, size_t size);

/* A Xing/Info/VBRI header, if the first frame carries one. This is an
 * INDEPENDENT ORACLE for the frame layer: the encoder wrote down how many
 * frames it produced, so a walk of the file can be checked against a number
 * this decoder did not compute. Returns 0 if there is no such header. */
uint32_t mp3_xing_frames(const uint8_t *frame, size_t size, const Mp3Header *h);

#endif /* _EMBLINK_MP3_FRAME_H_ */
