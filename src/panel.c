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

/* ---- state ---------------------------------------------------------------- */
static struct {
    PanelState st;
    pthread_mutex_t lock;
    pthread_t thread;
    int listen_fd, fd;
    bool running, peer_gone;
    uint8_t pkt[10];
    int have, want;
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
/* One write() per frame, so frames from different threads cannot interleave. */
static void panel_send(const uint8_t *b, size_t n)
{
    if (g.fd >= 0 && write(g.fd, b, n) < 0 && errno != EAGAIN) {
        /* peer gone; the reader notices */
    }
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
     * its neighbours. */
    f[0] = (uint8_t)(0x20 | grp);
    f[1] = g.st.keys[grp];
    pthread_mutex_unlock(&g.lock);
    panel_send(f, 2);
}

void panel_encoder(int enc, int delta)
{
    uint8_t f[2];

    if (enc < 0 || enc > 6 || !delta) {
        return;
    }
    if (delta < -128) delta = -128;
    if (delta > 127)  delta = 127;
    pthread_mutex_lock(&g.lock);
    g.st.enc_ticks[enc] += delta;
    pthread_mutex_unlock(&g.lock);
    f[0] = (uint8_t)(0x30 | enc);
    f[1] = (uint8_t)(int8_t)delta;
    panel_send(f, 2);
}

void panel_xfader(int pos)
{
    uint8_t f[2];

    if (pos < 0)   pos = 0;
    if (pos > 255) pos = 255;
    pthread_mutex_lock(&g.lock);
    g.st.xfader = pos;                 /* the skin draws the handle here */
    pthread_mutex_unlock(&g.lock);
    f[0] = 0x40;
    f[1] = (uint8_t)pos;
    panel_send(f, 2);
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
                g.fd = accept(g.listen_fd, NULL, NULL);
                if (g.fd >= 0) {
                    fcntl(g.fd, F_SETFL, O_NONBLOCK);
                }
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
            pthread_mutex_unlock(&g.lock);
            panel_send(f, 2);
        }
        struct pollfd p = { g.fd, POLLIN, 0 };

        if (poll(&p, 1, 20) > 0) {
            uint8_t buf[4096];
            ssize_t r;

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
