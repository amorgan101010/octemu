/*
 * WAV I/O and the test-signal generators, shared by the frontend (C) and
 * octdsp (C++).
 *
 * Both programs feed the same four ESAI RX slots from the same four spec
 * strings and write the same 16-bit PCM files, and they had two independent
 * implementations of each. They had drifted: the frontend's reader assumed a
 * canonical 44-byte header and read 16-bit only, so any WAV carrying a LIST or
 * fact chunk before `data` — which is what ffmpeg writes — was decoded from
 * the wrong offset and came out as noise, while octdsp read the same
 * file correctly. The chunk-walking reader below is the one that was right.
 *
 * Sample convention throughout: mono Q23 in an int32_t, taking the FIRST
 * channel of each frame (both readers already did this).
 *
 * Header only, so octdsp needs no extra objects to link.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OT_WAV_H
#define OT_WAV_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- test signals --------------------------------------------------------- */
enum { IN_SILENCE, IN_SIN, IN_COS, IN_LOOP, IN_ONE };

typedef struct {
    int kind;
    double hz;
    int32_t *wav;              /* Q23 mono, malloc'd; NULL for the tones */
    size_t n;                  /* frames in wav                          */
} InGen;

static inline int32_t ot_in_next(const InGen *g, uint64_t frame, unsigned rate)
{
    switch (g->kind) {
    case IN_SIN:
        return (int32_t)(0.5 * sin(2 * M_PI * g->hz * (double)frame / rate)
                         * 8388607.0);
    case IN_COS:
        return (int32_t)(0.5 * cos(2 * M_PI * g->hz * (double)frame / rate)
                         * 8388607.0);
    case IN_LOOP: return g->n ? g->wav[frame % g->n] : 0;
    case IN_ONE:  return frame < g->n ? g->wav[frame] : 0;
    default:      return 0;
    }
}

/* ---- reading -------------------------------------------------------------- */
/*
 * Walk the chunk list rather than assuming `data` starts at byte 44: the
 * offset is only fixed for the minimal header some encoders happen to emit.
 * Returns NULL on any failure (unreadable, not a WAV, unsupported depth); the
 * caller reports, because the two programs die differently.
 */
static inline int32_t *ot_wav_read(const char *path, size_t *nframes)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12], c[8];
    unsigned nch = 1, bits = 16, bytes, stride;
    int32_t *out = NULL;
    size_t want = 0;

    *nframes = 0;
    if (!f) {
        return NULL;
    }
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) ||
        memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return NULL;
    }
    while (fread(c, 1, 8, f) == 8) {
        const uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;

        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fmt[16];

            if (len < 16 || fread(fmt, 1, 16, f) != 16) {
                break;
            }
            nch = fmt[2] | fmt[3] << 8;
            bits = fmt[14] | fmt[15] << 8;
            if (fseek(f, (long)len - 16, SEEK_CUR)) {
                break;
            }
            continue;
        }
        if (memcmp(c, "data", 4)) {
            if (fseek(f, (long)len + (len & 1), SEEK_CUR)) {  /* chunks pad */
                break;
            }
            continue;
        }

        bytes = bits / 8;
        stride = bytes * nch;
        if ((bits != 16 && bits != 24) || !stride) {
            break;                              /* unsupported depth */
        }
        want = len / stride;
        out = (int32_t *)malloc(want ? want * sizeof *out : 1);
        if (!out) {
            break;
        }
        for (size_t i = 0; i < want; i++) {
            uint8_t s[3];

            if (fread(s, 1, bytes, f) != bytes) {
                want = i;                       /* truncated: keep what we got */
                break;
            }
            out[i] = bits == 16
                ? (int32_t)(int16_t)(s[0] | s[1] << 8) << 8
                : (int32_t)((uint32_t)(s[0] | s[1] << 8 |
                                       (uint32_t)s[2] << 16) << 8) >> 8;
            if (stride > bytes && fseek(f, (long)(stride - bytes), SEEK_CUR)) {
                want = i + 1;
                break;
            }
        }
        break;
    }
    fclose(f);
    if (!out) {
        return NULL;
    }
    *nframes = want;
    return out;
}

/* ---- writing -------------------------------------------------------------- */
/* The 44-byte canonical PCM16 header. `dlen` may be 0 for a stream whose
 * length is patched in afterwards by ot_wav_patch. */
static inline void ot_wav_header(uint8_t h[44], unsigned rate, unsigned nch,
                                 uint32_t dlen)
{
    const uint32_t block = 2 * nch, brate = rate * block, rlen = 36 + dlen;
    static const uint8_t tmpl[44] = {
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0, 1,0, 0,0, 0,0,0,0, 0,0,0,0, 0,0, 16,0, 'd','a','t','a',
        0,0,0,0
    };

    memcpy(h, tmpl, 44);
    h[4]  = rlen;  h[5]  = rlen >> 8;  h[6]  = rlen >> 16;  h[7]  = rlen >> 24;
    h[22] = nch;   h[23] = nch >> 8;
    h[24] = rate;  h[25] = rate >> 8;  h[26] = rate >> 16;  h[27] = rate >> 24;
    h[28] = brate; h[29] = brate >> 8; h[30] = brate >> 16; h[31] = brate >> 24;
    h[32] = block; h[33] = block >> 8;
    h[40] = dlen;  h[41] = dlen >> 8;  h[42] = dlen >> 16;  h[43] = dlen >> 24;
}

/* Patch the two length fields of a header written with dlen 0, for a file
 * whose length was not known when it was opened. */
static inline void ot_wav_patch(FILE *f, uint32_t frames, unsigned nch)
{
    const uint32_t dlen = frames * 2 * nch, rlen = 36 + dlen;
    const uint8_t r[4] = { (uint8_t)rlen, (uint8_t)(rlen >> 8),
                           (uint8_t)(rlen >> 16), (uint8_t)(rlen >> 24) };
    const uint8_t d[4] = { (uint8_t)dlen, (uint8_t)(dlen >> 8),
                           (uint8_t)(dlen >> 16), (uint8_t)(dlen >> 24) };

    fseek(f, 4, SEEK_SET);
    fwrite(r, 1, 4, f);
    fseek(f, 40, SEEK_SET);
    fwrite(d, 1, 4, f);
}

#endif
