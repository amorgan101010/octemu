/*
 * Reading the 128x64 screen, by exact template matching against the fonts in
 * the OS image itself (table 0x400ba812, 8 fonts). Exact, with vertical
 * containment, so a hit can be trusted — which is what lets every scripted
 * wait gate on the UI rather than on a timer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"

#define IMG_BASE   0x40000400u
#define FONT_TABLE 0x400ba812u
#define FONT_COUNT 8
#define FONT_BYTES 20

static uint8_t *g_img;
static size_t g_img_len;

static uint32_t img_ok(uint32_t addr, uint32_t n)
{
    return addr >= IMG_BASE && addr - IMG_BASE + n <= g_img_len;
}
static const uint8_t *img_at(uint32_t a) { return g_img + (a - IMG_BASE); }
static uint32_t img_u32(uint32_t a)
{
    const uint8_t *p = img_at(a);

    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
static uint16_t img_u16(uint32_t a)
{
    const uint8_t *p = img_at(a);

    return p[0] << 8 | p[1];
}

typedef struct {                 /* one glyph template, columns packed */
    char ch;
    int w;
    uint32_t col[16], topmask, botmask;
} Glyph;

static struct { int cell_h, bpc, top; } g_font[FONT_COUNT];
static Glyph g_glyphs[FONT_COUNT][256];
static int g_nglyphs[FONT_COUNT];

/*
 * Glyphs bucketed by their FIRST COLUMN, which is what the match tests first.
 *
 * Without this the matcher walks every glyph of every font at every (row,
 * column): 8 fonts x 859 glyphs x 60 rows x 128 columns x 2 polarities is ~13
 * million iterations, and it measured 5 ms per ocr_read — the same 5 ms on a
 * BLANK screen, because the cost is the scan, not the text. script.c polls a
 * pending wait_text every 100-250 ms for the whole of a walk, on the same
 * thread as the main loop.
 *
 * The bucket is a pure lookup: the candidate set is exactly the glyphs the old
 * inner loop accepted past its `gl->col[0] != v` test, so widest-wins and the
 * vertical-containment filter are untouched. A hash collision costs one
 * compare, because that test is still there.
 */
#define OCR_BUCKETS 256
static int16_t g_bhead[FONT_COUNT][OCR_BUCKETS];   /* glyph index, -1 = empty */
static int16_t g_bnext[FONT_COUNT][256];

static unsigned ocr_bucket(uint32_t col0)
{
    return (col0 * 2654435761u) >> 24;             /* Knuth, top 8 bits */
}

bool ocr_load_fonts(const char *os_image)
{
    FILE *f = fopen(os_image, "rb");

    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    g_img_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_img = malloc(g_img_len);
    if (!g_img || fread(g_img, 1, g_img_len, f) != g_img_len) {
        fclose(f);
        return false;
    }
    fclose(f);

    for (int i = 0; i < FONT_COUNT; i++) {
        const uint32_t a = FONT_TABLE + i * FONT_BYTES;
        const int cell_h = img_u16(a + 6);
        const int bpc = cell_h <= 8 ? 1 : 2;
        const uint32_t widths = img_u32(a + 8);
        const uint32_t offsets = img_u32(a + 12);
        const uint32_t bitmap = img_u32(a + 16);

        g_font[i].cell_h = cell_h;
        g_font[i].bpc = bpc;
        g_font[i].top = 8 * bpc - cell_h;
        if (cell_h <= 0 || cell_h > 16) {
            continue;
        }
        for (int cp = 0x20; cp < 0x100; cp++) {
            if (!img_ok(widths + cp, 1) || !img_ok(offsets + cp * 2, 2)) {
                continue;
            }
            const int w = (int8_t)*img_at(widths + cp);
            const int off = (int16_t)img_u16(offsets + cp * 2);

            if (w <= 0 || w > 15 || off < 0 ||
                !img_ok(bitmap + off, (uint32_t)(w * bpc))) {
                continue;
            }
            const uint8_t *cols = img_at(bitmap + off);
            Glyph *gl = &g_glyphs[i][g_nglyphs[i]];
            int ink = 0;

            gl->ch = (char)cp;
            gl->w = w;
            gl->topmask = gl->botmask = 0;
            for (int c = 0; c < w; c++) {
                const unsigned v = bpc == 1
                    ? cols[c]
                    : (unsigned)(cols[c * 2] << 8) | cols[c * 2 + 1];
                uint32_t packed = 0;

                for (int r = g_font[i].top; r < 8 * bpc; r++) {
                    packed |= ((v >> r) & 1u) << (r - g_font[i].top);
                }
                gl->col[c] = packed;
                ink += __builtin_popcount(packed);
                if (packed & 1) {
                    gl->topmask |= 1u << c;
                }
                if (packed & (1u << (cell_h - 1))) {
                    gl->botmask |= 1u << c;
                }
            }
            if (ink >= 2) {
                g_nglyphs[i]++;
            }
        }
    }

    /*
     * Insert DESCENDING so each chain runs ascending, the order the flat
     * scan used. The matcher's widest-wins test is `gl->w <= hit->w continue`,
     * so among EQUAL-width matches the first glyph scanned wins — and equal
     * width plus every column equal means two codepoints with identical
     * pixels, which this font has (emu.h notes O/0 and S/5). Reversing the
     * order silently re-decides those: measured on one screen, 3 became 9 and
     * 6 became 8. Ties must break exactly as before or every scripted wait
     * whose needle contains an ambiguous glyph changes meaning.
     */
    memset(g_bhead, 0xFF, sizeof g_bhead);         /* 0xFFFF == -1 */
    for (int i = 0; i < FONT_COUNT; i++) {
        for (int gi = g_nglyphs[i] - 1; gi >= 0; gi--) {
            const unsigned b = ocr_bucket(g_glyphs[i][gi].col[0]);

            g_bnext[i][gi] = g_bhead[i][b];
            g_bhead[i][b] = (int16_t)gi;
        }
    }
    return true;
}

static void ocr_pass(const uint64_t *cols, char *out, size_t cap, size_t *pn)
{
    size_t n = *pn;

    for (int fi = 0; fi < FONT_COUNT; fi++) {
        const int h = g_font[fi].cell_h;

        if (h <= 0 || h > 16 || !g_nglyphs[fi]) {
            continue;
        }
        const uint32_t mask = (1u << h) - 1;

        for (int y0 = 0; y0 + h <= H; y0++) {
            char run[128];
            int rn = 0, gap = 0, x = 0;

            while (x < W) {
                const uint32_t v = (uint32_t)(cols[x] >> y0) & mask;
                const Glyph *hit = NULL;

                for (int gi = g_bhead[fi][ocr_bucket(v)]; gi >= 0;
                     gi = g_bnext[fi][gi]) {
                    const Glyph *gl = &g_glyphs[fi][gi];
                    int ok = 1;

                    if (gl->col[0] != v || x + gl->w > W) {
                        continue;
                    }
                    if (hit && gl->w <= hit->w) {
                        continue;              /* widest wins */
                    }
                    for (int k = 1; k < gl->w && ok; k++) {
                        if (((uint32_t)(cols[x + k] >> y0) & mask) != gl->col[k]) {
                            ok = 0;
                        }
                    }
                    /* Vertical containment: ink immediately above or below a
                     * glyph's own top/bottom row means this is part of
                     * something taller, not the glyph. Box rules on a real
                     * screen earned this filter. */
                    for (int k = 0; k < gl->w && ok; k++) {
                        if (y0 > 0 && (gl->topmask >> k & 1) &&
                            (cols[x + k] >> (y0 - 1) & 1)) {
                            ok = 0;
                        }
                        if (y0 + h < H && (gl->botmask >> k & 1) &&
                            (cols[x + k] >> (y0 + h) & 1)) {
                            ok = 0;
                        }
                    }
                    if (ok) {
                        hit = gl;
                    }
                }
                if (hit) {
                    if (rn && gap >= 2 && rn < (int)sizeof run - 1) {
                        run[rn++] = ' ';
                    }
                    if (rn < (int)sizeof run - 1) {
                        run[rn++] = hit->ch;
                    }
                    x += hit->w;
                    gap = 0;
                    continue;
                }
                if (v == 0 && rn && gap < 4) {
                    gap++;
                    x++;
                    continue;
                }
                if (rn) {
                    char seen[64] = {0};
                    int an = 0;

                    run[rn] = 0;
                    for (int k = 0; k < rn; k++) {
                        if (isalnum((unsigned char)run[k])) {
                            an++;
                            if (!strchr(seen, run[k]) && strlen(seen) < 60) {
                                seen[strlen(seen)] = run[k];
                            }
                        }
                    }
                    /* Three characters, two of them alphanumeric and at least
                     * two distinct: enough to reject rules and dither noise
                     * matching one-pixel glyphs. */
                    if (rn >= 3 && an >= 2 && strlen(seen) >= 2 &&
                        n + rn + 2 < cap) {
                        memcpy(out + n, run, rn);
                        n += rn;
                        out[n++] = '\n';
                        out[n] = 0;
                    }
                }
                rn = 0;
                gap = 0;
                x++;
            }
        }
    }
    *pn = n;
}

void ocr_read(const PanelState *p, char *out, size_t cap)
{
    uint64_t cols[W];
    size_t n = 0;

    out[0] = 0;
    /* Titles and selections are inverse video, so read both polarities. */
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < W; x++) {
            uint64_t v = 0;

            for (int y = 0; y < H; y++) {
                v |= (uint64_t)(p->fb[y][x] ^ pass) << y;
            }
            cols[x] = v;
        }
        ocr_pass(cols, out, cap, &n);
    }
}

void ocr_canon(char *s)
{
    for (; *s; s++) {
        if (*s == 'O')      *s = '0';
        else if (*s == 'S') *s = '5';
        else if (*s == 'l' || *s == 'I') *s = '1';
    }
}
