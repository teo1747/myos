/* user/audio/mp3/frame.c -- see frame.h. */
#include <string.h>

#include "frame.h"

/* Bitrate in kbit/s, indexed [version_is_mpeg1][layer][index].
 * Index 0 is "free format" (the stream states no bitrate) and 15 is invalid;
 * both are rejected rather than treated as a number. */
static const uint16_t k_bitrate_v1_l3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const uint16_t k_bitrate_v2_l3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};

/* Sample rate by version. MPEG-2 halves MPEG-1's rates and MPEG-2.5 halves
 * them again -- the "2.5" extension is not in the ISO standard but is
 * everywhere in the wild, and a decoder that rejects it fails on real files. */
static const uint32_t k_samplerate[4][3] = {
    [MPEG_2_5] = { 11025, 12000,  8000 },
    [MPEG_RESERVED] = { 0, 0, 0 },
    [MPEG_2]   = { 22050, 24000, 16000 },
    [MPEG_1]   = { 44100, 48000, 32000 },
};

bool mp3_parse_header(const uint8_t *p, size_t avail, Mp3Header *out)
{
    if (avail < 4) return false;

    /* Eleven sync bits. Some old files use eleven, some twelve (the twelfth
     * being the MPEG-2.5 flag), which is why the version field is read from
     * the bits BELOW the sync rather than assumed. */
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;

    int version = (p[1] >> 3) & 3;
    int layer   = (p[1] >> 1) & 3;
    if (version == MPEG_RESERVED || layer != LAYER_III) return false;

    int br_index = (p[2] >> 4) & 0xF;
    int sr_index = (p[2] >> 2) & 3;
    if (br_index == 0 || br_index == 15) return false;   /* free/invalid */
    if (sr_index == 3) return false;

    int mode = (p[3] >> 6) & 3;

    out->version    = version;
    out->layer      = layer;
    out->crc        = (p[1] & 1) == 0;      /* the bit is "protection ABSENT" */
    out->bitrate    = 1000 * (version == MPEG_1 ? k_bitrate_v1_l3[br_index]
                                                : k_bitrate_v2_l3[br_index]);
    out->samplerate = (int)k_samplerate[version][sr_index];
    out->padding    = (p[2] >> 1) & 1;
    out->mode       = mode;
    out->mode_ext   = (p[3] >> 4) & 3;
    out->channels   = (mode == MODE_MONO) ? 1 : 2;
    if (out->bitrate == 0 || out->samplerate == 0) return false;

    /* A Layer III frame is 1152 samples on MPEG-1 and 576 on MPEG-2/2.5 --
     * the later versions dropped to one granule per frame. Getting this wrong
     * makes the frame length wrong, which the sync chain catches immediately. */
    out->samples = (version == MPEG_1) ? 1152 : 576;

    /* frame_bytes = samples/8 * bitrate / samplerate + padding, which is the
     * spec's formula with the /8 folded in. Integer division on purpose: the
     * remainder is what the padding bit exists to carry. */
    out->frame_bytes = (out->samples / 8) * out->bitrate / out->samplerate
                     + (out->padding ? 1 : 0);
    if (out->frame_bytes < 24) return false;

    /* Side info size depends on version AND channel count -- MPEG-2 halved it
     * along with the granule count. */
    if (version == MPEG_1) out->side_info_bytes = (out->channels == 1) ? 17 : 32;
    else                   out->side_info_bytes = (out->channels == 1) ?  9 : 17;

    return true;
}

long mp3_find_sync(const uint8_t *data, size_t size, size_t from)
{
    Mp3Header h;
    for (size_t i = from; i + 4 <= size; i++) {
        if (data[i] != 0xFF) continue;
        if (!mp3_parse_header(data + i, size - i, &h)) continue;
        /* CONFIRM WITH THE NEXT FRAME. 0xFF followed by plausible bits occurs
         * constantly inside compressed audio, so a single valid-looking header
         * is not evidence. Requiring that the frame it describes ends on
         * another valid header is what makes resynchronisation reliable, and
         * it is the same self-checking property the whole frame layer rests
         * on. (A frame at the very end of the file has nothing after it, so
         * that case is accepted on its own.) */
        size_t next = i + (size_t)h.frame_bytes;
        if (next + 4 > size) return (long)i;
        Mp3Header h2;
        if (mp3_parse_header(data + next, size - next, &h2)) return (long)i;
    }
    return -1;
}

size_t mp3_skip_id3(const uint8_t *data, size_t size)
{
    if (size < 10 || memcmp(data, "ID3", 3) != 0) return 0;
    /* A SYNCHSAFE integer: seven bits per byte, so the length can never
     * contain a 0xFF byte that would look like a sync word. Reading it as a
     * plain big-endian 32 is a classic bug that lands the decoder inside the
     * tag on any file with cover art. */
    uint32_t len = ((uint32_t)(data[6] & 0x7F) << 21)
                 | ((uint32_t)(data[7] & 0x7F) << 14)
                 | ((uint32_t)(data[8] & 0x7F) <<  7)
                 |  (uint32_t)(data[9] & 0x7F);
    size_t total = 10 + (size_t)len;
    if (data[5] & 0x10) total += 10;            /* a footer is present */
    return total <= size ? total : size;
}

uint32_t mp3_xing_frames(const uint8_t *frame, size_t size, const Mp3Header *h)
{
    /* The tag sits where the first frame's audio data would be, at a fixed
     * offset that depends on version and channel count. */
    size_t off = 4 + (size_t)h->side_info_bytes;
    if (h->crc) off += 2;
    if (off + 12 > size) return 0;

    const uint8_t *p = frame + off;
    if (memcmp(p, "Xing", 4) != 0 && memcmp(p, "Info", 4) != 0) return 0;

    uint32_t flags = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
                   | ((uint32_t)p[6] << 8) | p[7];
    if (!(flags & 1)) return 0;                 /* no frame count present */
    return ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16)
         | ((uint32_t)p[10] << 8) | p[11];
}
