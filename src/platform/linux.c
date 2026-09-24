/*
 * The Linux port: the three things the emulator needs from the host that are
 * not POSIX. src/platform/platform.h is the contract; this file is the whole
 * of the Linux side of it, and the only place in the emulator that mentions
 * udisks or ALSA.
 *
 * Like darwin.c, the three sections below are independent.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <alsa/asoundlib.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "platform.h"

/* ---- app activation ------------------------------------------------------- */

/*
 * No portable notion of "activate this app" across X11 and Wayland
 * compositors, and SDL raises a new window itself. The header allows a no-op.
 */
void platform_activate_ui(void)
{
}

/* ---- USB DISK MODE: the host side of the cable ---------------------------- */
/*
 * udisks plays the host, without root: `loop-setup` attaches the raw card
 * image as /dev/loopN (the kernel scans its MBR into /dev/loopNp1), and
 * `mount` puts the FAT32 partition under /run/media/$USER. A desktop
 * automounter may beat us to the mount; that failure is harmless.
 * --no-user-interaction everywhere: a polkit password prompt would block the
 * run loop, which the contract forbids. Refused is better than stalled.
 */
static char g_attached[64];

bool platform_disk_attach(const char *image, char *dev, size_t cap)
{
    char abs[PATH_MAX], cmd[PATH_MAX + 128], out[PATH_MAX + 64], part[80];
    char found[64] = "";
    struct stat st;
    FILE *h;

    if (cap) {
        dev[0] = 0;
    }
    if (g_attached[0]) {                /* idempotent: one loop per image */
        snprintf(dev, cap, "%s", g_attached);
        return true;
    }
    if (!realpath(image, abs)) {
        return false;
    }
    snprintf(cmd, sizeof cmd,
             "udisksctl loop-setup --no-user-interaction -f '%s' 2>/dev/null",
             abs);
    h = popen(cmd, "r");
    if (!h) {
        return false;
    }
    /* "Mapped file /path/card.img as /dev/loop0." */
    while (fgets(out, sizeof out, h)) {
        char *p = strstr(out, " as /dev/");

        if (!found[0] && p) {
            sscanf(p + 4, "%63[^. \n]", found);
        }
    }
    pclose(h);
    if (!found[0]) {
        return false;
    }
    snprintf(g_attached, sizeof g_attached, "%s", found);
    snprintf(dev, cap, "%s", found);

    /* The partition node appears asynchronously; give it ~300 ms. */
    snprintf(part, sizeof part, "%sp1", found);
    for (int i = 0; i < 30 && stat(part, &st) != 0; i++) {
        usleep(10 * 1000);
    }
    snprintf(cmd, sizeof cmd,
             "udisksctl mount --no-user-interaction -b '%s' >/dev/null 2>&1",
             part);
    if (system(cmd) != 0) {
        /* automounted already, or no partition table: the loop is still up */
    }
    return true;
}

bool platform_disk_detach(const char *dev)
{
    char cmd[256];
    int rc;

    /* Unmount first (ignore failure: may never have been mounted), then drop
     * the loop device, which is what ejecting the image means. */
    snprintf(cmd, sizeof cmd,
             "udisksctl unmount --no-user-interaction -b '%sp1' "
             ">/dev/null 2>&1", dev);
    rc = system(cmd);
    (void)rc;
    snprintf(cmd, sizeof cmd,
             "udisksctl loop-delete --no-user-interaction -b '%s' "
             ">/dev/null 2>&1", dev);
    if (system(cmd) != 0) {
        return false;
    }
    if (!strcmp(dev, g_attached)) {
        g_attached[0] = 0;
    }
    return true;
}

/* ---- --midi: the MIDI DIN port as a pair of ALSA sequencer ports ---------- */
/*
 *   "Octatrack Emulator"      readable port: guest MIDI OUT -> host apps
 *   "Octatrack Emulator In"   writable port: host apps      -> guest MIDI IN
 *
 * The wire is ColdFire UART0, exposed by QEMU as a unix socket carrying raw
 * MIDI bytes. ALSA's snd_midi_event parser does the framing darwin.c's feed()
 * does by hand (running status, realtime mid-message, SysEx), in both
 * directions. Two threads: one blocks on the socket, one on the sequencer;
 * neither touches the SDL loop.
 */
static int        g_fd = -1;
static snd_seq_t *g_seq;
static int        g_out_port, g_in_port;

/* guest -> host: bytes off the socket, framed into sequencer events */
static void *midi_reader(void *arg)
{
    unsigned char buf[512];
    snd_midi_event_t *enc;
    snd_seq_event_t ev;
    ssize_t n;

    (void)arg;
    if (snd_midi_event_new(4096, &enc) < 0) {
        return NULL;
    }
    snd_midi_event_no_status(enc, 1);
    while ((n = read(g_fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            snd_seq_ev_clear(&ev);
            if (snd_midi_event_encode_byte(enc, buf[i], &ev) == 1) {
                snd_seq_ev_set_source(&ev, g_out_port);
                snd_seq_ev_set_subs(&ev);
                snd_seq_ev_set_direct(&ev);
                snd_seq_event_output_direct(g_seq, &ev);
            }
        }
    }
    snd_midi_event_free(enc);
    close(g_fd);
    g_fd = -1;
    return NULL;
}

/* host -> guest: sequencer events decoded back to raw bytes on the socket */
static void *midi_writer(void *arg)
{
    unsigned char buf[4096];
    snd_midi_event_t *dec;
    snd_seq_event_t *ev;

    (void)arg;
    if (snd_midi_event_new(sizeof buf, &dec) < 0) {
        return NULL;
    }
    snd_midi_event_no_status(dec, 1);
    while (snd_seq_event_input(g_seq, &ev) >= 0) {
        long len;

        if (!ev || ev->dest.port != g_in_port) {
            continue;
        }
        len = snd_midi_event_decode(dec, buf, sizeof buf, ev);
        if (len > 0 && g_fd >= 0) {
            ssize_t ignored = write(g_fd, buf, (size_t)len);
            (void)ignored;
        }
    }
    snd_midi_event_free(dec);
    return NULL;
}

bool platform_midi_start(const char *sock, const char *name)
{
    char inname[128];
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    pthread_t th;

    if (!name) {
        name = "Octatrack Emulator";
    }

    /* QEMU may not have created the socket yet; retry ~10 s. */
    strncpy(addr.sun_path, sock, sizeof addr.sun_path - 1);
    for (int tries = 0; tries < 100; tries++) {
        g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(g_fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            break;
        }
        close(g_fd);
        g_fd = -1;
        usleep(100 * 1000);
    }
    if (g_fd < 0) {
        fprintf(stderr, "octemu: --midi: cannot connect %s\n", sock);
        return false;
    }

    if (snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        fprintf(stderr, "octemu: --midi: cannot open the ALSA sequencer "
                        "(is snd-seq loaded?)\n");
        return false;
    }
    snd_seq_set_client_name(g_seq, name);
    g_out_port = snd_seq_create_simple_port(g_seq, name,
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    snprintf(inname, sizeof inname, "%s In", name);
    g_in_port = snd_seq_create_simple_port(g_seq, inname,
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (g_out_port < 0 || g_in_port < 0) {
        fprintf(stderr, "octemu: --midi: cannot create sequencer ports\n");
        return false;
    }
    if (pthread_create(&th, NULL, midi_reader, NULL)) {
        fprintf(stderr, "octemu: --midi: reader thread failed\n");
        return false;
    }
    pthread_detach(th);
    if (pthread_create(&th, NULL, midi_writer, NULL)) {
        fprintf(stderr, "octemu: --midi: writer thread failed\n");
        return false;
    }
    pthread_detach(th);
    fprintf(stderr, "octemu: --midi: ALSA client '%s' ports '%s' / '%s' up\n",
            name, name, inname);
    return true;
}
