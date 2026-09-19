/*
 * The macOS port: the three things the emulator needs from the host that are
 * not POSIX. src/platform/platform.h is the contract; this file is the whole
 * of the Darwin side of it, and the only place in the emulator that mentions
 * Cocoa, hdiutil or CoreMIDI.
 *
 * It is one file rather than three because a port is a unit of work: whoever
 * writes src/platform/linux.c writes ONE file and satisfies ONE header, and
 * the build says which file is missing. The three sections below are
 * independent — nothing above the section rules is shared — and the MIDI
 * bridge, the only one with real substance, keeps its own commentary.
 *
 * SPDX-License-Identifier: MIT
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "platform.h"

/* ---- app activation ------------------------------------------------------- */

/* A CLI-spawned app opens behind the terminal; activate it explicitly. */
void platform_activate_ui(void)
{
    id app = ((id (*)(Class, SEL))objc_msgSend)(
        objc_getClass("NSApplication"), sel_registerName("sharedApplication"));

    ((void (*)(id, SEL, BOOL))objc_msgSend)(
        app, sel_registerName("activateIgnoringOtherApps:"), 1);
}

/* ---- USB DISK MODE: the host side of the cable ---------------------------- */
/*
 * hdiutil plays the host. `attach` mounts the raw card image as a disk and
 * reports the device node hdiutil named; `detach` ejects it again. Both are one
 * short-lived child process, which is inside the run loop's budget — the guest
 * has already unmounted the card by the time either runs, so it is waiting for
 * the host either way.
 */
bool platform_disk_attach(const char *image, char *dev, size_t cap)
{
    char cmd[512], out[256], found[64] = "";
    FILE *h;

    if (cap) {
        dev[0] = 0;
    }
    snprintf(cmd, sizeof cmd,
             "hdiutil attach -imagekey diskimage-class=CRawDiskImage "
             "'%s' 2>/dev/null", image);
    h = popen(cmd, "r");
    if (!h) {
        return false;
    }
    /*
     * FIRST match, not last: an MBR-partitioned card makes hdiutil print the
     * whole disk and then its slice (/dev/diskN, then /dev/diskNs1 with the
     * mount point). Detaching the slice is not what ejecting the image means,
     * and taking the last line is how you end up doing it.
     */
    while (fgets(out, sizeof out, h)) {
        if (!found[0] && !strncmp(out, "/dev/disk", 9)) {
            sscanf(out, "%63s", found);
        }
    }
    pclose(h);
    if (!found[0]) {
        return false;
    }
    snprintf(dev, cap, "%s", found);
    return true;
}

bool platform_disk_detach(const char *dev)
{
    char cmd[128];

    snprintf(cmd, sizeof cmd, "hdiutil detach '%s' >/dev/null 2>&1", dev);
    return system(cmd) == 0;
}

/* ---- --midi: the MIDI DIN port as a pair of CoreMIDI endpoints ------------ */
/*
 *   "Octatrack Emulator"      a virtual SOURCE:      guest MIDI OUT -> host apps
 *   "Octatrack Emulator In"   a virtual DESTINATION: host apps      -> guest MIDI IN
 *
 * The wire is ColdFire UART0 (31250 baud in the guest, byte-transparent here),
 * which QEMU exposes as a unix socket. Guest->host bytes are framed into
 * complete MIDI messages before CoreMIDI sees them (running status honoured,
 * realtime bytes pass through even mid-message, SysEx accumulated to a cap),
 * because CoreMIDI consumers expect whole messages per packet. Host->guest
 * needs no framing: the firmware's own parser (midi_parser, 0x40092bf4) eats
 * the raw byte stream.
 *
 * It runs on its own thread inside the emulator. CoreMIDI calls dest_read on a
 * thread of its own, and the reader thread blocks on the socket, so neither
 * touches the SDL loop.
 */
#define SYSEX_MAX 4096

static int          g_fd = -1;
static MIDIEndpointRef g_source;

/* ---- host -> guest: raw bytes straight onto the wire ---- */
static void dest_read(const MIDIPacketList *pktlist, void *ref, void *conn)
{
    const MIDIPacket *pkt = &pktlist->packet[0];

    (void)ref; (void)conn;
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        ssize_t ignored = write(g_fd, pkt->data, pkt->length);
        (void)ignored;
        pkt = MIDIPacketNext(pkt);
    }
}

/* ---- guest -> host: frame the byte stream into MIDI messages ---- */
static void emit(const unsigned char *bytes, int n)
{
    unsigned char buf[SYSEX_MAX + 128];
    MIDIPacketList *pl = (MIDIPacketList *)buf;
    MIDIPacket *pkt = MIDIPacketListInit(pl);

    MIDIPacketListAdd(pl, sizeof buf, pkt, 0, n, bytes);
    MIDIReceived(g_source, pl);
}

static int data_len(unsigned char s)
{
    switch (s & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
    default:
        switch (s) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        default: return 0;              /* 0xF6 tune request etc. */
        }
    }
}

static void feed(unsigned char b)
{
    static unsigned char status;        /* latched running status */
    static unsigned char data[2];
    static int ndata;
    static unsigned char sysex[SYSEX_MAX];
    static int nsysex;
    static int in_sysex;

    if (b >= 0xF8) {                    /* realtime passes through anywhere */
        emit(&b, 1);
        return;
    }
    if (in_sysex) {
        if (b == 0xF7) {
            if (nsysex < SYSEX_MAX) {
                sysex[nsysex++] = b;
            }
            emit(sysex, nsysex);
            in_sysex = 0;
        } else if (b & 0x80) {          /* interrupted sysex: drop, redo b */
            in_sysex = 0;
            feed(b);
        } else if (nsysex < SYSEX_MAX) {
            sysex[nsysex++] = b;
        }
        return;
    }
    if (b == 0xF0) {
        sysex[0] = b;
        nsysex = 1;
        in_sysex = 1;
        return;
    }
    if (b & 0x80) {                     /* a status byte */
        ndata = 0;
        if (data_len(b) == 0) {
            emit(&b, 1);
            status = 0;                 /* system common cancels running status */
        } else {
            status = b;
        }
        return;
    }
    if (!status) {                      /* data with no status latched: drop */
        return;
    }
    data[ndata++] = b;
    if (ndata == data_len(status)) {
        unsigned char msg[3] = { status, data[0], data[1] };

        emit(msg, 1 + ndata);
        ndata = 0;                      /* channel status stays latched */
        if (status >= 0xF0) {
            status = 0;                 /* system common does not latch */
        }
    }
}

/* The reader thread: frame everything the guest sends until QEMU goes away. */
static void *midi_reader(void *arg)
{
    unsigned char buf[512];
    ssize_t n;

    (void)arg;
    while ((n = read(g_fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            feed(buf[i]);
        }
    }
    close(g_fd);
    g_fd = -1;
    return NULL;
}

/*
 * Publish the endpoints and start reading. `sock` is QEMU's UART0 chardev.
 * Returns false (having said why) if the host or QEMU refuses; --midi is then
 * simply absent, which is better than failing a run that is otherwise fine.
 */
bool platform_midi_start(const char *sock, const char *name)
{
    char inname[128];
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    MIDIClientRef client;
    MIDIEndpointRef dest;
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

    MIDIClientCreate(CFSTR("octemu"), NULL, NULL, &client);
    if (MIDISourceCreate(client,
                         CFStringCreateWithCString(NULL, name,
                                                   kCFStringEncodingUTF8),
                         &g_source) != noErr) {
        fprintf(stderr, "octemu: --midi: MIDISourceCreate failed\n");
        return false;
    }
    snprintf(inname, sizeof inname, "%s In", name);
    if (MIDIDestinationCreate(client,
                              CFStringCreateWithCString(NULL, inname,
                                                        kCFStringEncodingUTF8),
                              dest_read, NULL, &dest) != noErr) {
        fprintf(stderr, "octemu: --midi: MIDIDestinationCreate "
                        "failed\n");
        return false;
    }
    if (pthread_create(&th, NULL, midi_reader, NULL)) {
        fprintf(stderr, "octemu: --midi: reader thread failed\n");
        return false;
    }
    pthread_detach(th);
    fprintf(stderr, "octemu: --midi: '%s' / '%s' up\n", name, inname);
    return true;
}
