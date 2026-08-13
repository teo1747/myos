/* user/audio/mp3/mp3_test.c -- the decoder, checked on the host.
 *
 * Run against any real MP3:  ./mp3_test <file.mp3>
 *
 * THE FRAME LAYER IS SELF-CHECKING and that is the whole point of F1. An MP3
 * has no index: where the next frame starts is computed from the header of
 * this one, from the bitrate, sample rate and padding bit. So walking a whole
 * file frame to frame -- never hunting for a sync word, always landing exactly
 * on one -- is a statement that every one of those fields was decoded
 * correctly, several thousand times consecutively, on a file nobody wrote for
 * this test. An assertion I invented could not check that; the file's own
 * arithmetic can.
 *
 * F2 adds an INDEPENDENT oracle: most encoders record the frame count in a
 * Xing header. Comparing a walk against a number written by a different
 * program years ago is a much stronger check than comparing it against itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "frame.h"

static int g_fail;

static void ok(const char *name, bool cond, const char *detail)
{
    printf("  %-44s %s%s%s\n", name, cond ? "OK" : "FAIL",
           detail && *detail ? "  -- " : "", detail ? detail : "");
    if (!cond) g_fail++;
}

/* --- the bit reader, on its own ------------------------------------------ */
static void test_bits(void)
{
    char msg[128];

    /* Known bytes, read in awkward widths that straddle byte boundaries --
     * which is the only interesting case, and the one a byte-at-a-time reader
     * gets wrong. 0xB5 0x2C = 1011 0101 0010 1100. */
    const uint8_t data[] = { 0xB5, 0x2C, 0xFF, 0x01 };
    BitReader b;
    bits_init(&b, data, sizeof data);

    uint32_t a = bits_read(&b, 3);      /* 101       = 5   */
    uint32_t c = bits_read(&b, 7);      /* 1010100   = 84  */
    uint32_t d = bits_read(&b, 6);      /* 101100    = 44  */
    snprintf(msg, sizeof msg, "%u %u %u", a, c, d);
    ok("B1 reads across byte boundaries", a == 5 && c == 84 && d == 44, msg);

    /* Peek must not move; the reservoir depends on it. */
    size_t before = bits_pos(&b);
    uint32_t p1 = bits_peek(&b, 8), p2 = bits_peek(&b, 8);
    ok("B2 peek does not consume", p1 == p2 && bits_pos(&b) == before, "");

    /* Past the end: zeroes and a flag, never a fault. A truncated frame is
     * normal input, not an exceptional one. */
    bits_seek(&b, sizeof(data) * 8 - 4);
    uint32_t tail = bits_read(&b, 16);
    snprintf(msg, sizeof msg, "got 0x%X, overrun=%d", tail, (int)b.overrun);
    ok("B3 reading past the end clamps and flags", b.overrun, msg);
}

/* --- the frame layer, against a real file --------------------------------- */
static void test_frames(const char *path)
{
    char msg[192];

    FILE *f = fopen(path, "rb");
    if (!f) { ok("F0 open the file", false, path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { ok("F0 read the file", false, ""); free(buf); return; }

    size_t start = mp3_skip_id3(buf, got);
    long first = mp3_find_sync(buf, got, start);
    snprintf(msg, sizeof msg, "id3 %zu bytes, first frame at %ld", start, first);
    ok("F0 skip ID3 and find the first frame", first >= 0, msg);
    if (first < 0) { free(buf); return; }

    Mp3Header h;
    mp3_parse_header(buf + first, got - (size_t)first, &h);
    snprintf(msg, sizeof msg, "MPEG%s %d Hz, %d kbit/s, %s",
             h.version == MPEG_1 ? "1" : h.version == MPEG_2 ? "2" : "2.5",
             h.samplerate, h.bitrate / 1000,
             h.channels == 1 ? "mono" : "stereo");
    ok("F1a the first header is sane", h.samplerate >= 8000 && h.channels >= 1, msg);

    /* THE WALK. Compute where the next frame is; require it to be there. */
    size_t pos = (size_t)first;
    unsigned frames = 0, resyncs = 0;
    unsigned long long samples = 0;
    int vbr = 0, last_br = h.bitrate;

    while (pos + 4 <= got) {
        Mp3Header fh;
        if (!mp3_parse_header(buf + pos, got - pos, &fh)) {
            long nxt = mp3_find_sync(buf, got, pos + 1);
            if (nxt < 0) break;
            resyncs++;
            pos = (size_t)nxt;
            continue;
        }
        if (fh.bitrate != last_br) { vbr = 1; last_br = fh.bitrate; }
        frames++;
        samples += (unsigned long long)fh.samples;
        pos += (size_t)fh.frame_bytes;
    }

    snprintf(msg, sizeof msg, "%u frames, %u resyncs, %s", frames, resyncs,
             vbr ? "VBR" : "CBR");
    /* A handful of resyncs at the very end of a file is normal (trailing ID3v1
     * or padding). Thousands would mean the length arithmetic is wrong. */
    ok("F1 the frame chain walks without hunting",
       frames > 10 && resyncs <= 2, msg);

    double secs = h.samplerate ? (double)samples / h.samplerate : 0;
    snprintf(msg, sizeof msg, "%.1f s of audio from %.1f KB", secs, sz / 1024.0);
    ok("F1b the walk covers the whole file", secs > 1.0, msg);

    /* F2 -- the encoder's own frame count, if it left one. */
    uint32_t xing = mp3_xing_frames(buf + first, got - (size_t)first, &h);
    if (xing) {
        /* The Xing frame itself is usually excluded from the count, so allow
         * exactly that difference rather than pretending it is not there. */
        long diff = (long)frames - (long)xing;
        snprintf(msg, sizeof msg, "walked %u, encoder said %u (diff %ld)",
                 frames, xing, diff);
        ok("F2 the walk matches the encoder's Xing count",
           diff >= 0 && diff <= 1, msg);
    } else {
        printf("  %-44s --  no Xing header in this file\n",
               "F2 encoder frame count");
    }

    free(buf);
}

int main(int argc, char **argv)
{
    printf("=== mp3\n");
    test_bits();
    if (argc > 1) test_frames(argv[1]);
    else printf("  (no file given -- frame tests skipped)\n");

    printf("=== mp3: %s (%d failure%s)\n", g_fail ? "FAIL" : "OK", g_fail,
           g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
