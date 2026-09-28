/*
 * octemu — spawns the patched QEMU and is the Octatrack's panel.
 *
 * It is NOT the Octatrack's clock. The DSP shim inside QEMU holds the block
 * loop to real time (-M octatrack,throttle=on) and publishes blocks into a
 * shared ring; this
 * process consumes them whenever it runs, so being preempted costs nothing
 * but ring occupancy. It also does not answer the version handshake — the
 * QEMU panel-uart device does, on the virtual clock.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <SDL.h>

#include "emu.h"
#include "board/ot-audio-shm.h"
#include "platform/platform.h"

bool g_quit;

static struct {
    bool headless, mk1, read_only;
    VideoMode video;
    int video_width;
    int throttle, interleave;
    const char *recording, *nvram, *cf, *script, *keymap, *os, *machine;
    const char *usb_host;           /* --usb-host PATH: packet-bench socket */
    bool audio_tap;                 /* --audio-tap: MAIN mirror in guest RAM */
    bool hw_faithful;               /* --hw-faithful: model hardware's hostile facts */
    const char *capture;
    int gdb;                        /* --gdb PORT: QEMU gdbstub */
    double timeout;
    InGen in[INS];
    bool midi;                      /* --midi: host MIDI bridge on UART0 */
    bool no_hints;                  /* --no-hints: no press/turn chrome */
    int remote;                     /* -1 --no-remote, 1 --remote, 0 auto */
    int remote_port;
    pid_t qemu;
    char dir[64];
} g;

int64_t now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void emu_die(const char *msg)
{
    fprintf(stderr, "octemu: %s\n", msg);
    exit(2);
}

static const char *k_usage =
    "usage: octemu [--cf-card PATH|none] [--nvram PATH|none] [--os PATH]\n"
    "                          [--read-only] [--headless] [--mk1] [--keymap PATH]\n"
    "                          [--script F.jsonl] [--timeout SECS]\n"
    "                          [--recording X.wav|X.mov|X.gif]\n"
    "                          [--video MODE] [--video-width N] [--no-hints]\n"
    "                          [--midi] [--remote|--no-remote] [--remote-port N]\n"
    "                          [--in-a SPEC]..[--in-d SPEC]\n"
    "\n"
    "Boots the Octatrack on the patched QEMU with the real DSP cores, at real\n"
    "time. With no arguments it uses a default battery (out/state/nvram.bin) and\n"
    "CF card (out/state/card.img, built by 'make card'), and the clock tracks\n"
    "system time. Pass 'none' for either to run without a battery or a card.\n"
    "\n"
    "Run it from the repo root: the QEMU binary, the panel raster\n"
    "(out/panel/panel.bin) and everything under out/ are found relative to the\n"
    "current directory.\n"
    "\n"
    "Common:\n"
    "  --cf-card P   boot from this CF card image, or 'none' for no card\n"
    "  --nvram P     battery-backed NVRAM: what the Octatrack remembers across\n"
    "                power cycles. 'none' is an Octatrack with a dead battery\n"
    "  --os PATH     boot this OS image instead of the default out/os/main.bin.\n"
    "                Each 'make fw-*' target writes one .os file — that is the\n"
    "                form to pass here. The .bin and .syx written beside it are\n"
    "                containers for flashing a real unit and will not boot\n"
    "  --read-only   discard the card's writes at exit (a qemu snapshot overlay).\n"
    "                Also turns the USB DISK MODE host mount off, because that\n"
    "                would show the backing file and not the overlay\n"
    "  --headless    run with no window; the Octatrack still runs (for --script)\n"
    "  --script F    drive the panel from a scripted walk, one JSON object per\n"
    "                line — the full step vocabulary is in src/script.c\n"
    "  --timeout S   stop after S seconds; exit 1 if a script has not finished\n"
    "  --recording F MAIN to F.wav; audio and video to F.mov; or F.gif, an\n"
    "                animated GIF sized and palettised for a README: 33 fps,\n"
    "                the face plate alone at its own width with the chassis\n"
    "                surround cropped off, no audio. .mov/.gif need ffmpeg\n"
    "  --video M     what an F.mov records: 'panel' (default) the rendered front\n"
    "                panel itself, LEDs and held keys and all, or 'screen' the\n"
    "                128x64 display alone. Panel is a readback off the window,\n"
    "                so --headless records the display and says so\n"
    "  --video-width N  how wide the video is: for .mov, how wide 'panel' is\n"
    "                recorded (default 1250, the panel's own width — a retina\n"
    "                readback is ~13 MB a frame and encoding all of it buys\n"
    "                nothing); for .gif, how wide the GIF is, by default the\n"
    "                face plate's own 1177. A width the readback divides\n"
    "                evenly by costs nothing and is the sharpest; any other\n"
    "                is one lanczos off the native frame\n"
    "  --no-hints    do not draw the yellow press/turn hints on the panel.\n"
    "                They are chrome the hardware has no idea about, and they\n"
    "                are drawn into --recording as well as the window\n"
    "  --midi        put the MIDI DIN port on the host as two virtual MIDI\n"
    "                ports, so other apps can play it and be played by it:\n"
    "                'Octatrack Emulator' is the Octatrack's MIDI OUT,\n"
    "                'Octatrack Emulator In' its MIDI IN\n"
    "  --remote      serve the panel to a browser on the local network (an\n"
    "                iPad, say): the real face plate with the live screen and\n"
    "                lamps, played with multitouch, and the sound streamed to\n"
    "                it once its sound button is tapped (the headphones knob\n"
    "                is the host's volume only). On by default with a window;\n"
    "                --headless and --script runs need --remote for it. The\n"
    "                address is printed at start. No password: anyone on the\n"
    "                network can play it\n"
    "  --no-remote   do not serve it\n"
    "  --remote-port N  the first port tried (default 8798; the next nine\n"
    "                are tried if it is taken)\n"
    "  --in-a..d S   what is plugged into inputs IN A-D: sin:HZ, cos:HZ,\n"
    "                loop:F.wav, one:F.wav, silence\n"
    "  --mk1         present the panel as a MKI (57 keys) instead of a MKII\n"
    "  --keymap P    extra key bindings, one 'SDLKEYNAME BUTTON' per line\n"
    "  --help, -h    this text\n"
    "\n"
    "Advanced (measurement, and the USB test gates):\n"
    "  --unthrottled a MEASUREMENT mode, not a speed setting: run flat out\n"
    "                instead of at real time, because capacity cannot be read\n"
    "                through a throttle. Flat out, the guest stops being\n"
    "                something a person can play\n"
    "  --audio-buffer N  live monitor SDL device buffer, frames (default 512)\n"
    "  --audio-cushion MS live monitor cushion, ms (default 250); larger\n"
    "                rides out emulator stalls at the cost of latency\n"
    "  --interleave N a CALIBRATION knob, not a tuning one: guest instructions\n"
    "                per DSP slice, the ratio the ColdFire and the DSP run at\n"
    "                (default 512). Values cannot be interpolated — measure any\n"
    "                new one before trusting it\n"
    "  --hw-faithful model what the hardware does and QEMU hides: poison the\n"
    "                0x48000000 scratch region (SDRAM is not zero at power-on)\n"
    "                and raise the USB frame signal whenever enumerated rather\n"
    "                than when the guest payload asks\n"
    "  --usb-host P  listen on unix socket P as a scripted USB host driving the\n"
    "                device controller packet bench (tests/usb-host.py)\n"
    "  --audio-tap   mirror MAIN into guest SDRAM at 0x48010000, where the\n"
    "                USB-audio payload reads its source ring\n"
    "  --capture-dsp F write the per-block DSP arms to F, so octdsp\n"
    "                --replay can render the same voice without QEMU\n"
    "  --gdb PORT    expose QEMU's gdbstub on PORT — how the test harnesses read\n"
    "                and poke guest memory\n"
    "  --machine M   the QEMU machine type, i.e. the board model this\n"
    "                emulator boots (default octatrack)\n"
    "\n"
    "USB DISK MODE (PROJECT > SYSTEM > USB DISK MODE on the Octatrack) mounts\n"
    "the card image as a disk on the HOST while active — the guest unmounts its\n"
    "filesystem first, so the handoff is clean — and ejects it again on exit.\n"
    "No flag needed; the guest's own menu drives it.\n"
    "\n"
    "Keys (the Monomachine/Machinedrum layout — see src/keys.c): F1-F8 and 1-8\n"
    "trigs, q-i tracks 1-8, ctrl FUNC, shift latches keys until let go, delete\n"
    "lets go of everything. Knobs: hold a s d / z x c (A-F), g LEVEL or h the\n"
    "crossfader and tap - = to turn, [ ] to push-turn; tap alone = click.\n"
    "space PLAY, end STOP, home REC, arrows, enter YES, backspace NO,\n"
    "pgup/pgdn PAGE, 9 PROJ, o PART, 0 AED, b MIX, m ARR, n MIDI, tab CUE,\n"
    "p PTN, ` BANK, \\ TEMPO, k l ; ' j SRC FX1 AMP LFO FX2, , . / REC1-3,\n"
    "F9 F10 the A and B scene keys. alt+1-7 picks the encoder the wheel turns\n"
    "(alt+wheel = push-turn).\n"
    "Mouse: click buttons; drag knobs vertically to turn (click = encoder push,\n"
    "right-drag = push-turn, the wheel over a knob also turns it); drag the\n"
    "crossfader and the headphones volume.\n";

static InGen parse_spec(const char *s)
{
    InGen gs = {0};

    if (!strncmp(s, "sin:", 4)) {
        gs.kind = IN_SIN;
        gs.hz = atof(s + 4);
    } else if (!strncmp(s, "cos:", 4)) {
        gs.kind = IN_COS;
        gs.hz = atof(s + 4);
    } else if (!strncmp(s, "loop:", 5) || !strncmp(s, "one:", 4)) {
        gs.kind = s[0] == 'l' ? IN_LOOP : IN_ONE;
        gs.wav = ot_wav_read(strchr(s, ':') + 1, &gs.n);
        if (!gs.wav) {
            emu_die("cannot read input WAV (want 16- or 24-bit PCM)");
        }
    } else if (strcmp(s, "silence")) {
        emu_die("bad input spec");
    }
    return gs;
}

/*
 * USB DISK MODE handoff — the policy half. The board appends "attach"/"detach"
 * lines to <rundir>/usb as the guest's USB DISK MODE brings the device
 * controller up and down (src/board/ot-board.c decodes usb_attach's USBCMD
 * writes). The host side of the cable is platform_disk_attach/detach: on attach
 * the card image becomes a disk on the host — DISK MODE's contract is that the
 * guest has unmounted its filesystem first, so the handoff is clean — and on
 * detach (leaving the mode with NO) it is ejected again and the guest remounts.
 * Suppressed under --read-only: qemu runs the card through a snapshot
 * overlay there, and a host mount of the backing file would show a lie.
 */
static long usb_notify_pos;
static char usb_disk_dev[64];

static void usb_poll(void)
{
    char path[128], line[128];
    FILE *f;

    if (!g.cf || g.read_only) {
        return;
    }
    snprintf(path, sizeof path, "%s/usb", g.dir);
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    fseek(f, usb_notify_pos, SEEK_SET);
    while (fgets(line, sizeof line, f)) {
        usb_notify_pos = ftell(f);
        if (!strncmp(line, "attach", 6) && !usb_disk_dev[0]) {
            platform_disk_attach(g.cf, usb_disk_dev, sizeof usb_disk_dev);
            fprintf(stderr, "octemu: usb disk mode: card "
                    "mounted on host as %s\n",
                    usb_disk_dev[0] ? usb_disk_dev : "(mount failed)");
        } else if (!strncmp(line, "detach", 6) && usb_disk_dev[0]) {
            if (platform_disk_detach(usb_disk_dev)) {
                fprintf(stderr, "octemu: usb disk mode: %s "
                        "ejected, card handed back to the guest\n",
                        usb_disk_dev);
            } else {
                fprintf(stderr, "octemu: usb disk mode: could "
                        "not eject %s (still open on the host?)\n",
                        usb_disk_dev);
            }
            usb_disk_dev[0] = 0;
        }
    }
    fclose(f);
}

/* OCTEMU_QEMU overrides it, for A/B-ing optimized builds (tools/bench.sh). */
#define QEMU_BIN (getenv("OCTEMU_QEMU") && *getenv("OCTEMU_QEMU") ? getenv("OCTEMU_QEMU") : "vendor/qemu/build/qemu-system-m68k")

static void spawn_qemu(const char *panel_sock, const char *audio_sock)
{
    char machine[512], panel[256], drive[512];
    static char midi[160];
    static char monitor[128];

    /* Check the binary HERE, in the parent. A missing or unbuilt QEMU used to
     * be a perror from the forked child, after which the parent sat waiting on
     * a panel socket nobody would ever connect to — a hang, for a one-line
     * mistake. Paths are relative to the current directory, so "run from the
     * repo root" is part of the answer. */
    if (access(QEMU_BIN, X_OK)) {
        fprintf(stderr, "octemu: cannot run %s: %s\n"
                "  run 'make qemu' (and run the emulator from the repo root: "
                "the QEMU binary, the panel raster and out/ are all found "
                "relative to the current directory)\n",
                QEMU_BIN, strerror(errno));
        exit(2);
    }

    snprintf(monitor, sizeof monitor, "unix:%s/mon,server=on,wait=off", g.dir);
    snprintf(machine, sizeof machine,
             "%s%s%s%s%s%s,dsp-audio=%s,throttle=%s,interleave=%d"
             ",usb-notify=%s/usb%s%s%s%s,exit-with-frontend=on",
             g.machine, g.mk1 ? ",mk1=on" : "",
             g.nvram ? ",nvram=" : "", g.nvram ? g.nvram : "",
             g.capture ? ",capture=" : "", g.capture ? g.capture : "",
             audio_sock, g.throttle ? "on" : "off", g.interleave, g.dir,
             g.usb_host ? ",usb-host=" : "", g.usb_host ? g.usb_host : "",
             g.audio_tap ? ",audio-tap=on" : "",
             g.hw_faithful ? ",hw-faithful=on" : "");
    snprintf(panel, sizeof panel, "unix:%s", panel_sock);
    if (g.cf) {
        snprintf(drive, sizeof drive, "if=ide,index=0,format=raw,file=%s%s",
                 g.cf, g.read_only ? ",snapshot=on" : "");
    }

    g.qemu = fork();
    if (g.qemu == 0) {
        const char *argv[32];
        int n = 0;

        argv[n++] = QEMU_BIN;
        argv[n++] = "-M"; argv[n++] = machine;
        argv[n++] = "-kernel"; argv[n++] = g.os;
        argv[n++] = "-display"; argv[n++] = "none";
        if (g.midi) {                                   /* MIDI (UART0) */
            snprintf(midi, sizeof midi,
                     "unix:%s/midi,server=on,wait=off", g.dir);
            argv[n++] = "-serial"; argv[n++] = midi;
        } else {
            argv[n++] = "-serial"; argv[n++] = "null";
        }
        argv[n++] = "-serial"; argv[n++] = panel;
        argv[n++] = "-serial"; argv[n++] = "null";      /* crossfader */
        /* -no-reboot makes a guest-initiated reset (and the monitor's
         * system_reset) EXIT rather than reboot — the normal, safe default.
         * OCTA_ALLOW_REBOOT opts a harness into true warm-reset semantics
         * (CPU + devices reset, SDRAM preserved), which the SDRAM
         * boot-wipe preflight needs to see what boot writes to RAM. */
        if (!getenv("OCTA_ALLOW_REBOOT")) {
            argv[n++] = "-no-reboot";
        }
        argv[n++] = "-monitor"; argv[n++] = monitor;
        static char gdbdev[32];
        if (g.gdb) {
            snprintf(gdbdev, sizeof gdbdev, "tcp::%d", g.gdb);
            argv[n++] = "-gdb"; argv[n++] = gdbdev;
        }
        if (g.cf) {
            argv[n++] = "-drive";
            argv[n++] = drive;
        }
        argv[n] = NULL;
        execv(argv[0], (char *const *)argv);
        perror("octemu: exec qemu");
        _exit(127);
    }
}

/* The per-run socket directory. Removed on EVERY exit path, including a
 * signal death: without this each run leaves a directory of dead unix sockets
 * behind in /tmp forever, and they accumulate into the hundreds. */
static void remove_rundir(void)
{
    static const char *names[] = { "panel", "audio", "mon", "midi", "usb" };

    if (!g.dir[0]) {
        return;
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char p[128];

        snprintf(p, sizeof p, "%s/%s", g.dir, names[i]);
        unlink(p);
    }
    rmdir(g.dir);
}

static void cleanup(void)
{
    g_quit = true;
    remote_stop();                      /* its pages let go of their keys */
    if (g.qemu > 0) {
        kill(g.qemu, SIGTERM);
        waitpid(g.qemu, NULL, 0);
        g.qemu = -1;
    }
    if (usb_disk_dev[0]) {              /* died while host-mounted: eject */
        platform_disk_detach(usb_disk_dev);   /* best effort */
        usb_disk_dev[0] = 0;
    }
    audio_stop();
    panel_stop();
    remove_rundir();
}

/*
 * ☠ atexit does NOT run on a signal death, and QEMU is a forked child rather
 * than a process-group member the shell will reap: a plain `kill` of this
 * process orphans a full-speed emulator. Three of them survived one session
 * that way, burning 110% CPU each and quietly poisoning every timing
 * measurement taken afterwards.
 */
static void on_term(int sig)
{
    if (g.qemu > 0) {
        kill(g.qemu, SIGTERM);
    }
    audio_recording_flush();  /* or the .wav header stays at zero frames */
    remove_rundir();     /* unlink/rmdir are async-signal-safe */
    _exit(128 + sig);
}

int main(int argc, char **argv)
{
    char panel_sock[96], audio_sock[96];
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Cursor *cur_arrow = NULL, *cur_hand = NULL;
    bool cursor_hand = false;
    int active_enc = 0;
    int64_t start;

    g.os = "out/os/main.bin";
    g.machine = "octatrack";
    g.throttle = OT_THROTTLE_DEFAULT;   /* real time; see ot-board.c */
    g.interleave = OT_INTERLEAVE_DEFAULT;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = NULL;

        #define ARG() (val = (i + 1 < argc) ? argv[++i] \
                             : (emu_die("missing value"), NULL))
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("%s", k_usage);
            return 0;
        } else if (!strcmp(a, "--headless"))  g.headless = true;
        else if (!strcmp(a, "--mk1"))         g.mk1 = true;
        else if (!strcmp(a, "--read-only"))   g.read_only = true;
        else if (!strcmp(a, "--usb-host"))    { ARG(); g.usb_host = val; }
        else if (!strcmp(a, "--audio-tap"))   { g.audio_tap = true; }
        else if (!strcmp(a, "--hw-faithful")) { g.hw_faithful = true; }
        else if (!strcmp(a, "--recording"))   { ARG(); g.recording = val; }
        else if (!strcmp(a, "--video")) {
            ARG();
            if (!strcmp(val, "panel"))       g.video = VIDEO_PANEL;
            else if (!strcmp(val, "screen")) g.video = VIDEO_SCREEN;
            else emu_die("--video takes panel or screen");
        }
        else if (!strcmp(a, "--video-width")) { ARG(); g.video_width = atoi(val); }
        else if (!strcmp(a, "--nvram"))       { ARG(); g.nvram = val; }
        else if (!strcmp(a, "--capture-dsp")) { ARG(); g.capture = val; }
        else if (!strcmp(a, "--gdb"))         { ARG(); g.gdb = atoi(val); }
        else if (!strcmp(a, "--cf-card"))     { ARG(); g.cf = val; }
        else if (!strcmp(a, "--script"))      { ARG(); g.script = val; }
        else if (!strcmp(a, "--keymap"))      { ARG(); g.keymap = val; }
        else if (!strcmp(a, "--os"))          { ARG(); g.os = val; }
        else if (!strcmp(a, "--machine"))     { ARG(); g.machine = val; }
        else if (!strcmp(a, "--timeout"))     { ARG(); g.timeout = atof(val); }
        else if (!strcmp(a, "--unthrottled"))  { g.throttle = 0; }
        else if (!strcmp(a, "--midi"))        g.midi = true;
        else if (!strcmp(a, "--no-hints"))   { g.no_hints = true; }
        else if (!strcmp(a, "--remote"))      { g.remote = 1; }
        else if (!strcmp(a, "--no-remote"))   { g.remote = -1; }
        else if (!strcmp(a, "--remote-port")) { ARG(); g.remote_port = atoi(val); }
        else if (!strcmp(a, "--interleave"))  { ARG(); g.interleave = atoi(val); }
        else if (!strcmp(a, "--audio-buffer")) { ARG(); audio_set_buffers((unsigned)atoi(val), 0); }
        else if (!strcmp(a, "--audio-cushion")) { ARG(); audio_set_buffers(0, (unsigned)atoi(val)); }
        else if (!strncmp(a, "--in-", 5) && strlen(a) == 6 &&
                 a[5] >= 'a' && a[5] <= 'd') {
            const int idx = a[5] - 'a';

            ARG();
            g.in[idx] = parse_spec(val);
        } else {
            fprintf(stderr, "octemu: unknown flag %s\n%s",
                    a, k_usage);
            return 2;
        }
        #undef ARG
    }

    /* Zero-config default state: a battery and a card under out/state/, so a
     * bare ./octemu boots to a mounted Octatrack that remembers
     * its projects. 'none' opts out (tests want a cold one). */
    if (!g.nvram) {
        g.nvram = "out/state/nvram.bin";
    } else if (!strcmp(g.nvram, "none")) {
        g.nvram = NULL;
    }
    if (!g.cf) {
        g.cf = "out/state/card.img";
    } else if (!strcmp(g.cf, "none")) {
        g.cf = NULL;
    }
    if (g.cf && access(g.cf, F_OK)) {
        if (strcmp(g.cf, "out/state/card.img")) {
            char msg[320];

            snprintf(msg, sizeof msg, "no card image at %s (--cf-card)", g.cf);
            emu_die(msg);
        }
        /* The default card is a BUILD product. Making it here would mean a
         * program reaching back into its own source tree at runtime, which is
         * why this used to work only from the repo root. */
        emu_die("no card image at out/state/card.img\n"
                "  run 'make card' to build one, or pass --cf-card PATH\n"
                "  (--cf-card none runs the Octatrack with no card at all)");
    }
    if (g.nvram) {
        char d[256];
        char *slash;

        snprintf(d, sizeof d, "%s", g.nvram);
        slash = strrchr(d, '/');
        if (slash) {
            *slash = 0;
            mkdir(d, 0755);
        }
    }

    if (!ocr_load_fonts(g.os)) {
        emu_die("cannot read the OS image (run 'make os')");
    }
    if (g.keymap) {
        FILE *f = fopen(g.keymap, "r");
        char kn[64], bn[64];

        if (!f) {
            emu_die("cannot read keymap");
        }
        while (fscanf(f, "%63s %63s", kn, bn) == 2) {
            const SDL_Keycode k = SDL_GetKeyFromName(kn);
            const int id = panel_button_id(bn);

            if (k == SDLK_UNKNOWN || id < 0) {
                fprintf(stderr, "octemu: keymap: bad line %s %s\n",
                        kn, bn);
            } else if (!keys_bind(k, id)) {
                break;
            }
        }
        fclose(f);
    }

    snprintf(g.dir, sizeof g.dir, "/tmp/octemu-%d", (int)getpid());
    mkdir(g.dir, 0700);
    snprintf(panel_sock, sizeof panel_sock, "%s/panel", g.dir);
    snprintf(audio_sock, sizeof audio_sock, "%s/audio", g.dir);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_term);
    atexit(cleanup);

    if (g.script && !script_open(g.script)) {
        emu_die("cannot read script");
    }
    /*
     * Panel video is the default because it is what a person wants to see, but
     * it is a readback off the renderer and a headless run has none. Fall back
     * and SAY SO: the alternative is either refusing every headless .mov, or a
     * file that quietly contains something other than what was asked for.
     */
    if (g.video == VIDEO_PANEL && g.headless) {
        if (g.recording && strstr(g.recording, ".mov")) {
            fprintf(stderr, "octemu: --headless has no panel to "
                            "record; recording the display instead\n");
        }
        g.video = VIDEO_SCREEN;
    }
    if (g.video_width && g.video != VIDEO_PANEL) {
        emu_die("--video-width sizes the panel recording; it means nothing "
                "with --video screen");
    }
    audio_video_mode(g.video, g.video_width);
    if (!panel_start(panel_sock)) {
        emu_die("panel socket");
    }
    spawn_qemu(panel_sock, audio_sock);
    /* Off by default for headless and scripted runs: the harnesses run those
     * by the batch, in parallel, and none of them should open a port. */
    if (g.remote > 0 || (!g.remote && !g.headless && !g.script)) {
        remote_start(g.remote_port > 0 ? g.remote_port : REMOTE_PORT);
    }
    if (g.midi) {
        char sock[128];

        snprintf(sock, sizeof sock, "%s/midi", g.dir);
        platform_midi_start(sock, NULL);
    }

    if (!g.headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            emu_die(SDL_GetError());
        }
        if (!skin_load()) {
            emu_die("the window needs out/panel/panel.bin (run 'make panel')");
        }
        skin_hints(!g.no_hints);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        win = SDL_CreateWindow("octemu", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, 1250, 682,
                               SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
        if (remote_url()) {             /* where a tablet finds the panel */
            char title[160];

            snprintf(title, sizeof title, "octemu - remote panel at %s",
                     remote_url());
            SDL_SetWindowTitle(win, title);
        }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
        skin_window(win, ren);
        /* Raising and activating is right when a PERSON launched the
         * emulator — a CLI-spawned app opens behind the terminal — and wrong
         * when a harness launches twenty of them, because
         * activateIgnoringOtherApps: takes the keyboard away from whatever the
         * user is actually doing, once per run. OCTA_NO_FOCUS opts out, so
         * automated runs can exercise the real windowed path (SDL audio
         * callback, vsync render loop, live monitor) without stealing focus. */
        if (!getenv("OCTA_NO_FOCUS")) {
            SDL_RaiseWindow(win);
            platform_activate_ui();
        }
        cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        cur_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    }
    audio_start(audio_sock, g.in, g.recording, !g.headless);

    start = now_ms();
    while (!g_quit) {
        int status;

        script_step();
        if (g.script && script_done()) {
            break;                          /* --timeout is only a ceiling */
        }
        if (g.timeout && now_ms() - start > g.timeout * 1000) {
            break;
        }
        if (panel_peer_gone()) {
            break;
        }
        if (g.qemu > 0 && waitpid(g.qemu, &status, WNOHANG) == g.qemu) {
            g.qemu = -1;
            break;
        }
        usb_poll();
        if (g.headless) {
            usleep(5000);
            continue;
        }

        SDL_Event ev;

        skin_view_update();
        while (SDL_PollEvent(&ev)) {
            /* Alt, not Ctrl, is the mouse's push-turn: Ctrl is FUNC now. */
            const bool alt = (SDL_GetModState() & KMOD_ALT) != 0;

            switch (ev.type) {
            case SDL_QUIT:
                g_quit = true;
                break;
            case SDL_WINDOWEVENT:
                /* The key-ups of whatever is held will go to another window. */
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    keys_release_all();
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                const bool down = ev.type == SDL_KEYDOWN;
                const SDL_Keycode k = ev.key.keysym.sym;

                if (alt && k >= SDLK_1 && k <= SDLK_7) {
                    if (down) {
                        active_enc = k - SDLK_1;
                    }
                    break;
                }
                keys_key(k, down, ev.key.repeat != 0, ev.key.keysym.mod);
                break;
            }
            case SDL_MOUSEWHEEL: {
                const int d = ev.wheel.y > 0 ? 1 : ev.wheel.y < 0 ? -1 : 0;
                int enc = active_enc, mx, my, over;
                float lx, ly;

                if (!d) {
                    break;
                }
                SDL_GetMouseState(&mx, &my);
                skin_view_mouse(mx, my, &lx, &ly);
                over = skin_knob_at(lx, ly);       /* the knob under it wins */
                if (over >= 0) {
                    enc = over;
                }
                if (enc == 7) {                    /* headphones pot */
                    audio_set_phones(audio_phones() + d * 0.05f);
                    break;
                }
                if (alt) {                         /* push-turn */
                    panel_key(56 + enc, true);
                    panel_encoder(enc, d);
                    panel_key(56 + enc, false);
                } else {
                    panel_encoder(enc, d);
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                float lx, ly;

                if (ev.button.button != SDL_BUTTON_LEFT &&
                    ev.button.button != SDL_BUTTON_RIGHT) {
                    break;
                }
                skin_view_mouse(ev.button.x, ev.button.y, &lx, &ly);
                if (ev.type == SDL_MOUSEBUTTONDOWN) {
                    /* Right-drag is push-turn: Ctrl is FUNC, and Alt-drag
                     * belongs to the window manager on most Linux desktops. */
                    skin_mouse_down(lx, ly,
                                    ev.button.button == SDL_BUTTON_RIGHT);
                } else {
                    skin_mouse_up();
                }
                break;
            }
            case SDL_MOUSEMOTION: {
                float lx, ly;
                bool over;

                skin_view_mouse(ev.motion.x, ev.motion.y, &lx, &ly);
                skin_mouse_motion(lx, ly);
                over = skin_over_control(lx, ly);
                if (over != cursor_hand) {
                    cursor_hand = over;
                    SDL_SetCursor(over ? cur_hand : cur_arrow);
                }
                break;
            }
            }
        }
        skin_tick();
        keys_tick();
        SDL_RenderClear(ren);
        skin_render();
        /* ☠ The grab goes BEFORE the present, not after. SDL_RenderReadPixels
         * reads the current target, and once the frame is presented that is
         * the NEXT back buffer: reading after the present films every frame
         * one late and opens the recording with a black one, because the
         * first back buffer has never been drawn to. MEASURED: three black
         * frames at the head of a .mov, then one after the shutter learned to
         * wait for a real frame, then none once the order was this. */
        audio_video_tick();
        SDL_RenderPresent(ren);
        /* ☠ A frame cap for when vsync is not in force (offscreen, some
         * compositors): SDL_RENDERER_PRESENTVSYNC is a request, and without it
         * this loop redraws the whole panel as fast as it can. Measured with
         * the offscreen driver: the frontend at ~50% of a core, the emulator's
         * vCPU saturated and behind the pace on every block while doing the
         * same guest work as a headless run — the redraw thrashes the cache
         * the vCPU shares. 60 fps is plenty for a panel; with vsync at 60 Hz
         * the present has already blocked and this sleeps nothing. */
        {
            static uint64_t last;
            const uint64_t now = SDL_GetPerformanceCounter();
            const uint64_t hz = SDL_GetPerformanceFrequency();
            const uint64_t frame = hz / 60;

            if (last && now - last < frame) {
                SDL_Delay((Uint32)((frame - (now - last)) * 1000 / hz));
            }
            last = SDL_GetPerformanceCounter();
        }
    }

    {
        const bool failed = script_failed() || (g.script && !script_done());

        cleanup();
        if (!g.headless) {
            SDL_Quit();
        }
        return failed ? 1 : 0;
    }
}
