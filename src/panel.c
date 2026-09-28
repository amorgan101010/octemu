/*
 * The panel link: this process IS the panel MCU.
 *
 * Inbound it parses the multiplexed wire — display blocks, LED group
 * snapshots, per-bit brightness, palette writes — into one PanelState.
 * Outbound it sends key-group snapshots, encoder deltas and crossfader
 * positions.
 *
 * The version handshake is NOT here. It lives in the QEMU panel-uart device,
 * answered on the virtual clock, because the guest's polled reader wants its
 * reply inside a narrow window and host scheduling cannot be relied on to hit
 * it — a miss wedges the deferred parser for the whole run. The device also
 * consumes the query rather than forwarding it, so this file never sees one.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "emu.h"
#include "board/ot-panel-wire.h"

/* ---- key ids (read-disasm / measured against the firmware) ---------------- */
static const struct { const char *name; int id; } g_buttons[] = {
    {"TRIG1",0},{"TRIG2",1},{"TRIG3",2},{"TRIG4",3},{"TRIG5",4},{"TRIG6",5},
    {"TRIG7",6},{"TRIG8",7},{"TRIG9",8},{"TRIG10",9},{"TRIG11",10},
    {"TRIG12",11},{"TRIG13",12},{"TRIG14",13},{"TRIG15",14},{"TRIG16",15},
    {"TRACK1",16},{"TRACK2",17},{"TRACK3",18},{"TRACK4",19},{"TRACK5",20},
    {"TRACK6",21},{"TRACK7",22},{"TRACK8",23},
    {"TEMPO",24},{"A",25},{"B",26},{"PAGE",27},
    {"PROJ",28},{"PART",29},{"AED",30},{"ARR",31},
    {"DOWN",32},{"RIGHT",33},
    {"SRC",34},{"AMP",35},{"LFO",36},{"FX1",37},{"FX2",38},
    {"STOP",39},{"PLAY",40},{"REC",41},{"CUE",42},
    {"REC1",43},{"REC2",44},{"FUNC",45},{"PTN",46},{"BANK",47},{"MIX",48},
    {"YES",49},{"NO",50},{"UP",51},{"LEFT",52},{"MIDI",53},{"REC3",54},
    {"ENC1",56},{"ENC2",57},{"ENC3",58},{"ENC4",59},{"ENC5",60},{"ENC6",61},
    {"ENC7",62},
};

int panel_button_id(const char *name)
{
    for (size_t i = 0; i < sizeof g_buttons / sizeof *g_buttons; i++) {
        if (!strcasecmp(g_buttons[i].name, name)) {
            return g_buttons[i].id;
        }
    }
    if (isdigit((unsigned char)name[0])) {     /* raw key id, for probing */
        long v = strtol(name, NULL, 10);

        if (v >= 0 && v < 64) {
            return (int)v;
        }
    }
    return -1;
}

/* ---- the face plate's elements, by SVG id ----------------------------------
 * Shared by the window (skin.c) and the browser panel (remote.c), which both
 * name controls by their ids in assets/panel/octatrack.svg. */

/* Lamp map, measured. L<n> = bit ids 2n/2n+1; chan is which channels this
 * element shows (3 = both, additive — the meter lamps split their two channels
 * across two physical LEDs). */
const PanelLamp panel_lamps[PANEL_NLAMPS] = {
    {"trig-1",0,3},{"trig-2",1,3},{"trig-3",2,3},{"trig-4",3,3},
    {"trig-5",4,3},{"trig-6",5,3},{"trig-7",6,3},{"trig-8",7,3},
    {"trig-9",8,3},{"trig-10",9,3},{"trig-11",10,3},{"trig-12",11,3},
    {"trig-13",12,3},{"trig-14",13,3},{"trig-15",14,3},{"trig-16",15,3},
    {"btn-a",16,3},{"btn-b",17,3},
    {"led-card-status",18,3},          /* green, flickers with CF activity */
    {"btn-tempo",19,3},                /* pulses with the tempo */
    {"btn-t1",20,3},{"btn-t2",21,3},{"btn-t3",22,3},{"btn-t4",23,3},
    {"btn-t5",24,3},{"btn-t6",25,3},{"btn-t7",26,3},{"btn-t8",27,3},
    {"led-rec-status",32,3},           /* FUNC+ARR lights it red */
    {"btn-src",33,3},{"btn-amp",34,3},{"btn-midi",35,3},
    /* page LEDs: dim red = the page exists, bright = the active edit page in
     * grid rec, amber beat-flash = the page the sequencer is playing */
    {"led-page-1",36,3},{"led-page-2",37,3},{"led-page-3",38,3},
    {"led-page-4",39,3},
    {"btn-fx1",40,3},{"btn-fx2",41,3},{"btn-rec",42,3},{"btn-lfo",43,3},
    {"btn-stop",44,3},{"btn-play",45,3},
    {"btn-mix",51,3},
    {"btn-proj",60,3},{"btn-part",61,3},{"btn-aed",62,3},{"btn-arr",63,3},
    {"led-in-a",64,1},{"led-in-b",64,2},
    {"led-in-c",65,1},{"led-in-d",65,2},
    {"led-int-l",66,1},{"led-int-r",66,2},
};

/* tint = sum over lit channels of palette colour x brightness, with 0x44 x 15
 * as full scale (the firmware's maximum) */
void panel_lamp_rgb(const PanelState *p, int k, uint8_t out[3])
{
    int acc[3] = {0, 0, 0};

    for (int bit = 0; bit < 2; bit++) {
        const int id = 2 * panel_lamps[k].lamp + bit;
        const int br = p->bright[id];
        const uint8_t *c = p->rgb[4 * panel_lamps[k].lamp + 1 + bit];

        if (!(panel_lamps[k].chan >> bit & 1) || !br ||
            !(p->lit[id >> 3] >> (id & 7) & 1)) {
            continue;
        }
        for (int ch = 0; ch < 3; ch++) {
            acc[ch] += c[ch] * br;
        }
    }
    for (int ch = 0; ch < 3; ch++) {
        const int v = acc[ch] * 255 / (0x44 * 15);

        out[ch] = (uint8_t)(v > 255 ? 255 : v);
    }
}

static const struct { const char *svg; const char *btn; } g_svgbtn[] = {
    {"btn-midi","MIDI"},{"btn-rec1","REC1"},{"btn-rec2","REC2"},
    {"btn-rec3","REC3"},{"btn-proj","PROJ"},{"btn-part","PART"},
    {"btn-aed","AED"},{"btn-mix","MIX"},{"btn-arr","ARR"},{"btn-func","FUNC"},
    {"btn-cue","CUE"},{"btn-ptn","PTN"},{"btn-bank","BANK"},{"btn-yes","YES"},
    {"btn-no","NO"},{"btn-up","UP"},{"btn-down","DOWN"},{"btn-left","LEFT"},
    {"btn-right","RIGHT"},{"btn-rec","REC"},{"btn-play","PLAY"},
    {"btn-stop","STOP"},{"btn-src","SRC"},{"btn-amp","AMP"},{"btn-lfo","LFO"},
    {"btn-fx1","FX1"},{"btn-fx2","FX2"},{"btn-tempo","TEMPO"},{"btn-a","A"},
    {"btn-b","B"},{"btn-page","PAGE"},
};

int panel_svg_key(const char *svg)
{
    if (!strncmp(svg, "trig-", 5) && isdigit((unsigned char)svg[5])) {
        const int n = atoi(svg + 5);

        return n >= 1 && n <= 16 ? n - 1 : -1;
    }
    if (!strncmp(svg, "btn-t", 5) && svg[5] >= '1' && svg[5] <= '8' &&
        !svg[6]) {
        return 16 + svg[5] - '1';                  /* btn-tN -> TRACK N */
    }
    for (size_t i = 0; i < sizeof g_svgbtn / sizeof *g_svgbtn; i++) {
        if (!strcmp(svg, g_svgbtn[i].svg)) {
            return panel_button_id(g_svgbtn[i].btn);
        }
    }
    return -1;
}

/* -> the SVG id of key `id`, or NULL; `buf` holds the ids that are built. */
const char *panel_key_svg(int id, char *buf, size_t cap)
{
    if (id >= 0 && id < 16) {
        snprintf(buf, cap, "trig-%d", id + 1);
        return buf;
    }
    if (id >= 16 && id < 24) {
        snprintf(buf, cap, "btn-t%d", id - 15);
        return buf;
    }
    for (size_t i = 0; i < sizeof g_svgbtn / sizeof *g_svgbtn; i++) {
        if (panel_button_id(g_svgbtn[i].btn) == id) {
            return g_svgbtn[i].svg;
        }
    }
    return NULL;
}

int panel_svg_knob(const char *svg)
{
    if (!strcmp(svg, "knob-level")) return 6;
    if (!strcmp(svg, "knob-phones")) return 7;     /* analog pot, local */
    if (!strncmp(svg, "knob-", 5) && svg[5] >= 'a' && svg[5] <= 'f' &&
        !svg[6]) {
        return svg[5] - 'a';
    }
    return -1;
}

/* ---- state ---------------------------------------------------------------- */
static struct {
    PanelState st;
    pthread_mutex_t lock;
    pthread_t thread;
    int listen_fd, fd;
    bool running, peer_gone;
    uint8_t pkt[10];
    int have, want;
    /* Outbound bytes the socket would not take yet (under lock). */
    uint8_t out[4096];
    size_t out_len;
    unsigned long out_lost;            /* whole frames with no room at all */
} g;

void panel_snapshot(PanelState *out)
{
    pthread_mutex_lock(&g.lock);
    *out = g.st;
    pthread_mutex_unlock(&g.lock);
}

/* ☠ Do NOT mask the id to 7 bits. lit[] carries 136 bits and ids 128-133 are
 * the six meter LEDs (see emu.h) — the only lamps past 127. Masking made the
 * group index wrap while the bit index did not, so a query for a meter lamp
 * quietly answered with group 0. */
bool panel_lamp_lit(int bit_id)
{
    bool on;

    if (bit_id < 0 || bit_id >= (int)(sizeof g.st.lit * 8)) {
        return false;
    }
    pthread_mutex_lock(&g.lock);
    on = (g.st.lit[bit_id >> 3] >> (bit_id & 7) & 1) != 0;
    pthread_mutex_unlock(&g.lock);
    return on;
}

uint32_t panel_fb_gen(void)
{
    uint32_t v;

    pthread_mutex_lock(&g.lock);
    v = g.st.fb_gen;
    pthread_mutex_unlock(&g.lock);
    return v;
}

bool panel_peer_gone(void) { return g.peer_gone; }

/* ---- outbound ------------------------------------------------------------- */
/* Write what is waiting in g.out. Call with g.lock held. */
static void panel_flush(void)
{
    while (g.fd >= 0 && g.out_len) {
        const ssize_t w = write(g.fd, g.out, g.out_len);

        if (w <= 0) {
            if (w < 0 && errno != EAGAIN && errno != EINTR) {
                g.out_len = 0;         /* peer gone; the reader notices */
            }
            return;
        }
        memmove(g.out, g.out + w, g.out_len - (size_t)w);
        g.out_len -= (size_t)w;
    }
}

/* Send one frame. Call with g.lock held, so frames from different threads
 * cannot interleave or overtake each other. The fd is nonblocking: what it
 * will not take now is kept, whole frames in order, and goes out ahead of the
 * next frame or on the panel thread's next pass. A key frame dropped here
 * used to leave the firmware holding (or never seeing) a key the window
 * showed down. */
static void panel_send(const uint8_t *b, size_t n)
{
    if (g.fd < 0) {
        return;
    }
    panel_flush();
    if (!g.out_len) {
        const ssize_t w = write(g.fd, b, n);

        if (w == (ssize_t)n) {
            return;
        }
        if (w < 0 && errno != EAGAIN && errno != EINTR) {
            return;                    /* peer gone; the reader notices */
        }
        if (w > 0) {
            b += w;
            n -= (size_t)w;
        }
    }
    if (g.out_len + n > sizeof g.out) {
        g.out_lost++;                  /* the guest stopped reading */
        return;
    }
    memcpy(g.out + g.out_len, b, n);
    g.out_len += n;
}

void panel_key(int id, bool down)
{
    uint8_t f[2];
    int grp, bit;

    if (id < 0 || id > 63) {
        return;
    }
    grp = id >> 3;
    bit = id & 7;
    pthread_mutex_lock(&g.lock);
    if (down) {
        g.st.keys[grp] |= 1u << bit;
    } else {
        g.st.keys[grp] &= ~(1u << bit);
    }
    /* A key frame is a SNAPSHOT of the whole group; a bare key would release
     * its neighbours. It is sent under the lock: the window and the remote
     * panel's threads both press keys, and a snapshot overtaken by a newer
     * one would land last and release what the newer one holds. The fd is
     * nonblocking, so the write does not wait. */
    f[0] = (uint8_t)(0x20 | grp);
    f[1] = g.st.keys[grp];
    panel_send(f, 2);
    pthread_mutex_unlock(&g.lock);
}

void panel_encoder(int enc, int delta)
{
    uint8_t f[2];

    if (enc < 0 || enc > 6 || !delta) {
        return;
    }
    if (delta < -128) delta = -128;
    if (delta > 127)  delta = 127;
    f[0] = (uint8_t)(0x30 | enc);
    f[1] = (uint8_t)(int8_t)delta;
    pthread_mutex_lock(&g.lock);
    g.st.enc_ticks[enc] += delta;
    panel_send(f, 2);                  /* under the lock, as panel_key */
    pthread_mutex_unlock(&g.lock);
}

void panel_xfader(int pos)
{
    uint8_t f[2];

    if (pos < 0)   pos = 0;
    if (pos > 255) pos = 255;
    pthread_mutex_lock(&g.lock);
    g.st.xfader = pos;                 /* the skin draws the handle here */
    f[0] = 0x40;
    f[1] = (uint8_t)pos;
    panel_send(f, 2);                  /* under the lock, as panel_key */
    pthread_mutex_unlock(&g.lock);
}

/* ---- inbound -------------------------------------------------------------- */
static void panel_apply(void)
{
    const uint8_t b = g.pkt[0];

    pthread_mutex_lock(&g.lock);
    if ((b & 0xF0) == 0x10) {
        const int page = b & 0x0F, col = g.pkt[1];

        for (int k = 0; k < 8 && col + k < W; k++) {
            for (int bit = 0; bit < 8; bit++) {
                const int y = page * 8 + (7 - bit);   /* bit 7 is lowest y */

                if (y < H) {
                    g.st.fb[H - 1 - y][col + k] = (g.pkt[k + 2] >> bit) & 1;
                }
            }
        }
        g.st.fb_gen++;
    } else if ((b & 0xF0) == 0x20 || (b & 0xF0) == 0xA0) {
        const int grp = (b & 0x0F) + ((b & 0xF0) == 0xA0 ? 16 : 0);

        if (grp < 17) {
            g.st.lit[grp] = g.pkt[1];      /* ABSOLUTE snapshot, not a mask */
        }
    } else if ((b & 0xF0) == 0x30) {
        g.st.bright[g.pkt[1]] = b & 0x0F;
    } else if (b == 0xB5) {
        const int id = g.pkt[1] << 8 | g.pkt[2];

        if (id < 512) {
            g.st.rgb[id][0] = g.pkt[3];
            g.st.rgb[id][1] = g.pkt[4];
            g.st.rgb[id][2] = g.pkt[5];
        }
    }
    pthread_mutex_unlock(&g.lock);
}

static void panel_rx(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = buf[i];

        if (g.have == 0) {
            g.want = ot_panel_pktlen(b);
            if (!g.want) {
                continue;                  /* unframed: resync */
            }
        }
        g.pkt[g.have++] = b;
        if (g.have >= g.want) {
            panel_apply();
            g.have = 0;
        }
    }
}

static void *panel_thread(void *arg)
{
    int beat = 0;

    (void)arg;
    while (g.running) {
        if (g.fd < 0) {
            struct pollfd p = { g.listen_fd, POLLIN, 0 };

            if (poll(&p, 1, 20) > 0) {
                const int fd = accept(g.listen_fd, NULL, NULL);

                if (fd >= 0) {
                    fcntl(fd, F_SETFL, O_NONBLOCK);
                }
                pthread_mutex_lock(&g.lock);
                g.out_len = 0;             /* nothing owed a new peer */
                g.fd = fd;
                pthread_mutex_unlock(&g.lock);
            }
            continue;
        }
        /* The real crossfader is an ADC the panel MCU keeps reporting, so the
         * firmware always knows its position. Without a periodic report the
         * firmware-side fader level tables (0x80003c60, and 0x80000c80 built
         * from them — the per-track scene/crossfader levels the DSP control
         * words carry) stay ZERO until the fader first moves, which silences
         * every path scaled by them. One frame per second mimics ADC chatter. */
        if (++beat >= 50) {
            uint8_t f[2];

            beat = 0;
            pthread_mutex_lock(&g.lock);
            f[0] = 0x40;
            f[1] = (uint8_t)g.st.xfader;
            panel_send(f, 2);          /* under the lock, as panel_key */
            pthread_mutex_unlock(&g.lock);
        }
        /* Anything panel_send had to keep goes out here once the socket
         * drains, even if no key moves again to push it along. */
        pthread_mutex_lock(&g.lock);
        panel_flush();
        const bool pending = g.out_len != 0;
        pthread_mutex_unlock(&g.lock);
        struct pollfd p = { g.fd, (short)(POLLIN | (pending ? POLLOUT : 0)), 0 };

        if (poll(&p, 1, 20) > 0) {
            uint8_t buf[4096];
            ssize_t r;

            if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;                  /* writable only: flushed above */
            }
            while ((r = read(g.fd, buf, sizeof buf)) > 0) {
                panel_rx(buf, (size_t)r);
            }
            if (r == 0) {                  /* QEMU exited */
                g.peer_gone = true;
                return NULL;
            }
        }
    }
    return NULL;
}

bool panel_start(const char *sock_path)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };

    pthread_mutex_init(&g.lock, NULL);
    g.fd = -1;
    g.st.xfader = 0;
    g.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    strncpy(a.sun_path, sock_path, sizeof a.sun_path - 1);
    unlink(sock_path);
    /* We listen and QEMU connects at machine init, so no boot byte is lost. */
    if (g.listen_fd < 0 || bind(g.listen_fd, (struct sockaddr *)&a, sizeof a) ||
        listen(g.listen_fd, 1)) {
        return false;
    }
    g.running = true;
    return pthread_create(&g.thread, NULL, panel_thread, NULL) == 0;
}

void panel_stop(void)
{
    if (!g.running) {
        return;
    }
    g.running = false;
    pthread_join(g.thread, NULL);
    if (g.fd >= 0) {
        close(g.fd);
    }
    if (g.listen_fd >= 0) {
        close(g.listen_fd);
    }
}
