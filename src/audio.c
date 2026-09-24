/*
 * The audio side of the frontend: consume the shim's shared ring, feed inputs
 * back, record, and drive the live monitor.
 *
 * ☠ THE RECORDINGS AND THE LIVE MONITOR ARE DIFFERENT CODE PATHS, and only the
 * recordings are covered by the automated tests. --recording appends the
 * Octatrack's timeline block by block with no rate conversion; that is what
 * audio-quality.py and checktone.py judge, and it is sample-exact. The live
 * monitor resamples through a fractional cursor whose ratio IS the playback
 * pitch, so it can drift, warble or starve while every recording assertion
 * passes. A green audio test is necessary and not sufficient.
 *
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <SDL.h>

#include "emu.h"
#include "board/ot-audio-shm.h"

#define ZOOM 4                 /* --video screen: the display, 128x64 at 4x */
#define VID_W (W * ZOOM)
#define VID_H (H * ZOOM)
#define BLOCKS_PER_VIDEO 92    /* ~29.96 video fps in audio time */

/*
 * What `--recording x.mov` puts in the video track.
 *
 *   VIDEO_PANEL   the default: the rendered front panel, read back off the
 *                 renderer. What you are looking at — LEDs, held keys, knob
 *                 positions, the crossfader. Needs the window, so a headless
 *                 run falls back to VIDEO_SCREEN and says so.
 *   VIDEO_SCREEN  the 128x64 display alone, drawn here from the panel
 *                 snapshot, which is all a run with no window can offer.
 *
 * ☠ Whatever the mode, the shutter stays on the GUEST's clock: one frame every
 * BLOCKS_PER_VIDEO audio blocks. Video and audio then cannot drift, because
 * both are counted in the same block stream. A wall-clock shutter would have to
 * be resynchronised against a machine that does not run at wall speed.
 */
static VideoMode g_vmode = VIDEO_PANEL;
static int g_vw, g_vh;                 /* the video track's size, fixed at open */
#define PANEL_W_DEFAULT 1250           /* the panel's own width, 1:1 */
static int g_panel_w = PANEL_W_DEFAULT;    /* --video-width overrides it */

/*
 * The panel shutter. The readback has to happen on the render thread (SDL owns
 * it), and the frame has to be pushed on the guest's clock, so the two hand
 * over through this: the consumer asks (`want`), the render thread answers
 * (`have`), and the consumer pushes at the next shutter. A frame is therefore
 * at most one shutter period stale, which is constant and so does not drift.
 *
 * ☠ A shutter that finds no new frame pushes the PREVIOUS one again and counts
 * it. The video's length and its alignment with the audio stay right, and the
 * count says how much of what you are watching was a repeat — a video that
 * silently dropped frames would misreport when a lamp lit, which is worse than
 * one that admits it.
 */
static pthread_mutex_t g_shutter_m = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_shutter_buf;         /* the frame the render thread produced */
static bool g_shutter_want, g_shutter_have;
/*
 * ☠ Until the render thread answers the first shutter, that buffer is a black
 * rectangle, and pushing it would open every recording with black frames.
 * Nothing is recorded — no video AND no audio, so the two still start
 * together — until there is a real frame to record.
 */
static bool g_shutter_seen;

/*
 * The recording gate. A walk can hold it shut ({"record":0}) and open it when
 * the picture is worth keeping ({"record":1}) — the RECEIVE film does, because
 * the splash is already in the other one.
 *
 * ☠ It gates AUDIO AND VIDEO TOGETHER. Gate one and a .mov's two streams start
 * at different moments and stay that far apart for the whole clip.
 *
 * ☠ It also breaks the one mapping the audio assertions rely on: with the gate
 * used, block N is no longer frame 16N of the .wav. Do not gate a walk that
 * reads its recording by block number (tests/walks/trig-one.jsonl).
 *
 * ☠ Closing it is only deterministic in --video panel, and only because the
 * main loop runs script_step() BEFORE it presents: the first frame cannot be
 * grabbed until an iteration has completed, so a {"record":0} on the walk's
 * first line always wins the race.
 */
static bool g_rec_gate = true;

void audio_record_gate(bool on)
{
    /* ☠ Opening it must throw away the frame in hand. Nothing asks the render
     * thread for a new one while the gate is shut, so the buffer still holds
     * whatever was on the panel when the recording opened — seconds of boot
     * ago. Clearing `seen` makes the next push wait for a fresh grab. */
    if (on && !g_rec_gate) {
        pthread_mutex_lock(&g_shutter_m);
        g_shutter_seen = false;
        g_shutter_have = false;
        g_shutter_want = true;
        pthread_mutex_unlock(&g_shutter_m);
    }
    g_rec_gate = on;
}
static uint64_t g_video_frames, g_video_repeats;

/*
 * --recording x.gif. A GIF worth putting in a README needs a palette built
 * from the whole clip, which a live pipe cannot do — palettegen has to see
 * every frame before it can emit one. So the run records an ordinary .mov
 * beside the target and converts it when the machine stops, with settings
 * fixed here rather than left to the caller:
 *
 *   100/3 fps       ☠ a GIF's frame delay is a whole number of CENTIseconds,
 *                   so the only rates that play at true speed are 100/n: 33.3
 *                   (delay 3), 25 (4), 20 (5). 30 is not one of them — asking
 *                   for it yields delay 3 and a GIF that runs 11% fast. 100/3
 *                   is the nearest exact rate above the 29.96 fps shutter.
 *   the face plate only, not the whole panel
 *                   the dark chassis surround is margin, and a README has
 *                   none to spare — skin_grab_plate() reads back that rect
 *   the plate's own width, 1:1, by default
 *                   a README renders at about 880 px on a 1x screen, but at
 *                   2x device pixels on a retina one, where 880 is upscaled
 *                   by the browser. --video-width overrides.
 *   EXACTLY ONE resample, and the cheapest correct one:
 *                   ☠ where the readback divides evenly by the target — the
 *                   usual case, a 2x display filmed at 1:1 — the box filter
 *                   in skin_grab_plate IS the ideal reduction, because a 2x
 *                   raster is supersampling and averaging its 4 pixels is
 *                   what it is for. MEASURED against a native 1x render of
 *                   the same artwork: box 0.20% RMSE, lanczos 0.87%. It is
 *                   also a QUARTER of the bytes down the pipe, which is what
 *                   keeps the encoder fed: at 8.6 MB a frame the scaler fell
 *                   behind and the ring dropped 284 of 1020 frames.
 *                   Where it does not divide evenly, the native frame goes
 *                   down the pipe and ffmpeg's lanczos does the whole job in
 *                   one step: 0.08% RMSE, against 0.82% for the old route of
 *                   box-to-1250-then-lanczos through an h264 4:2:0
 *                   intermediate. Never two resamples. An unsharp pass was
 *                   tried and is far worse: 2.11%.
 *   a LOSSLESS intermediate (ffv1)
 *                   the clip is written once and read twice; h264 in the
 *                   middle is loss for nothing. ffv1 is in every ffmpeg
 *                   build, where libx264rgb is not
 *   palettegen stats_mode=diff + paletteuse dither=sierra2_4a
 *                   0.41% RMSE on real footage against 10.6% for a single
 *                   fixed palette
 */
#define GIF_FPS   "100/3"
static int g_gif_w;                    /* 0 = the plate's own width */
static bool g_gif_plate;               /* recording the plate for a gif */
static char g_gif_out[512];            /* the .gif the caller asked for */
static char g_gif_tmp[512];            /* the .mov recorded to get there */

void audio_video_mode(VideoMode mode, int panel_width)
{
    g_vmode = mode;
    if (panel_width > 0) {
        g_panel_w = panel_width;
    }
}

uint64_t audio_video_repeats(void) { return g_video_repeats; }

static struct {
    InGen in[INS];
    uint64_t blocks;
    pthread_t thread;
    bool running;
    OtAudioShm *shm;
    int sock;
    uint32_t throttled;

    FILE *wav;
    uint32_t wav_frames;
    int video_fd, audio_pipe_fd;
    pid_t ffmpeg;

    SDL_AudioDeviceID dev;
    float phones;
    int32_t slot_peak[SLOTS];
} g = { .video_fd = -1, .audio_pipe_fd = -1, .sock = -1, .phones = 1.0f,
        .throttled = OT_THROTTLE_DEFAULT };

uint64_t audio_blocks(void) { return g.blocks; }
float audio_phones(void) { return g.phones; }
void audio_set_phones(float v)
{
    g.phones = v < 0 ? 0 : v > 1 ? 1 : v;
}

/* ---- .wav ----------------------------------------------------------------- */
static void wav_begin(const char *path)
{
    uint8_t h[44];

    g.wav = fopen(path, "wb");
    if (!g.wav) {
        emu_die("cannot write recording");
    }
    ot_wav_header(h, RATE, 2, 0);       /* length patched in by wav_end */
    fwrite(h, 1, sizeof h, g.wav);
}

static void wav_end(void)
{
    if (!g.wav) {
        return;
    }
    ot_wav_patch(g.wav, g.wav_frames, 2);
    fclose(g.wav);
    g.wav = NULL;
}

/*
 * Patch the length into a recording that is still open, for a run that is
 * about to die on a signal. ☠ Without this, ANY emulator killed mid-run
 * leaves a .wav whose header still says zero frames — the data is all there
 * on disk, but every reader sees an empty file. That silently turned a P4
 * comparison into "the recording is empty" rather than a real result.
 * Deliberately does not fclose: the process is exiting anyway, and this must
 * stay as close to async-signal-safe as the surrounding teardown.
 */
void audio_recording_flush(void)
{
    if (g.wav) {
        fflush(g.wav);
        ot_wav_patch(g.wav, g.wav_frames, 2);
        fflush(g.wav);
    }
}

/* ---- .mov -----------------------------------------------------------------
 * ffmpeg's two-input interleaving stalls its pipe reads for long stretches, so
 * each pipe gets a ring and a writer thread; overflow drops (counted) rather
 * than blocks. In v1 this mattered enormously — a blocking write from the
 * audio thread stalled the lockstep and froze the Octatrack.
 *
 * ☠ Do NOT "fix" a dropping recording by making the push wait for room. It
 * was tried. ONE thread feeds both rings, so a producer parked on the video
 * ring stops producing audio too — and ffmpeg, which interleaves its two
 * inputs, then waits for the audio that will never come and never drains the
 * video. MEASURED: the guest fell to 2% CPU and the walk never reached its
 * first mark. The way to stop dropping frames is to give the encoder less to
 * do, not to hold the guest at gunpoint.
 *
 * The audio track is PCM, never aac: aac audibly smears a pure tone
 * (measured ~100 chaotic zero-crossing periods per burst after encode against
 * ~14 in the PCM). */
typedef struct {
    uint8_t *buf;
    size_t cap, len, pos;
    pthread_mutex_t m;
    pthread_cond_t c;
    bool eof;
    int fd;
    uint64_t dropped;
    pthread_t th;
} RecPipe;

static RecPipe g_vrec, g_arec;
static volatile bool g_rec_live;       /* the first real block has arrived */

static void *rec_writer(void *arg)
{
    RecPipe *p = arg;
    static _Thread_local uint8_t tmp[1 << 16];

    for (;;) {
        size_t n, first, put = 0;

        pthread_mutex_lock(&p->m);
        while (!p->len && !p->eof) {
            pthread_cond_wait(&p->c, &p->m);
        }
        if (!p->len) {
            pthread_mutex_unlock(&p->m);
            return NULL;
        }
        n = p->len < sizeof tmp ? p->len : sizeof tmp;
        first = p->cap - p->pos < n ? p->cap - p->pos : n;
        memcpy(tmp, p->buf + p->pos, first);
        memcpy(tmp + first, p->buf, n - first);
        p->pos = (p->pos + n) % p->cap;
        p->len -= n;
        pthread_mutex_unlock(&p->m);
        while (put < n) {
            ssize_t r = write(p->fd, tmp + put, n - put);

            if (r <= 0) {
                return NULL;
            }
            put += (size_t)r;
        }
    }
}

/* Whole chunks only: a partial video frame would shear the stream. */
static void rec_push(RecPipe *p, const void *data, size_t n)
{
    pthread_mutex_lock(&p->m);
    if (p->cap - p->len < n) {
        p->dropped += n;
    } else {
        const size_t at = (p->pos + p->len) % p->cap;
        const size_t first = p->cap - at < n ? p->cap - at : n;

        memcpy(p->buf + at, data, first);
        memcpy(p->buf, (const uint8_t *)data + first, n - first);
        p->len += n;
        pthread_cond_signal(&p->c);
    }
    pthread_mutex_unlock(&p->m);
}

static void rec_begin(RecPipe *p, int fd, size_t cap)
{
    p->fd = fd;
    p->cap = cap;
    p->buf = malloc(cap);
    if (!p->buf) {
        emu_die("recording ring");
    }
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->c, NULL);
    pthread_create(&p->th, NULL, rec_writer, p);
}

static void rec_end(RecPipe *p)
{
    if (!p->buf) {
        return;
    }
    pthread_mutex_lock(&p->m);
    p->eof = true;
    pthread_cond_signal(&p->c);
    pthread_mutex_unlock(&p->m);
    pthread_join(p->th, NULL);
    close(p->fd);
    if (p->dropped) {
        fprintf(stderr, "octemu: recording dropped %llu bytes\n",
                (unsigned long long)p->dropped);
    }
}

/* The 128x64 display at 4x, from the panel snapshot. RGB24. This is what a run
 * with no window can record, and nothing else. */
static void compose(uint8_t *rgb)
{
    PanelState p;

    panel_snapshot(&p);
    for (int y = 0; y < H * ZOOM; y++) {
        for (int x = 0; x < W * ZOOM; x++) {
            const int on = p.fb[y / ZOOM][x / ZOOM];
            uint8_t *q = rgb + (y * VID_W + x) * 3;

            q[0] = on ? 0xD8 : 0x10;
            q[1] = on ? 0xF0 : 0x10;
            q[2] = on ? 0xFF : 0x14;
        }
    }
}

/*
 * One frame into the video pipe, in whatever mode is in force. Called on the
 * consumer thread (the guest's clock) and, before the first block arrives, on
 * the boot thread.
 */
static void push_frame(uint8_t *scratch)
{
    if (!g_rec_gate) {
        return;
    }
    if (g_vmode != VIDEO_PANEL) {
        compose(scratch);
        rec_push(&g_vrec, scratch, (size_t)g_vw * g_vh * 3);
        g_video_frames++;
        return;
    }
    pthread_mutex_lock(&g_shutter_m);
    if (g_shutter_buf && g_shutter_seen) {
        if (!g_shutter_have) {
            g_video_repeats++;          /* the render thread did not keep up */
        }
        rec_push(&g_vrec, g_shutter_buf, (size_t)g_vw * g_vh * 3);
        g_video_frames++;
        g_shutter_have = false;
        g_shutter_want = true;          /* ask for the next one */
    }
    pthread_mutex_unlock(&g_shutter_m);
}

/*
 * The render thread's half of the shutter: called right after a present, it
 * answers a pending request and does nothing at all otherwise, so a run that
 * is not recording the panel pays one atomic read per frame.
 */
void audio_video_tick(void)
{
    bool want;

    if (g_vmode != VIDEO_PANEL || g.video_fd < 0) {
        return;
    }
    pthread_mutex_lock(&g_shutter_m);
    want = g_shutter_want && g_shutter_buf;
    pthread_mutex_unlock(&g_shutter_m);
    if (!want) {
        return;
    }
    {
        static uint8_t *tmp;
        static size_t cap;
        const size_t need = (size_t)g_vw * g_vh * 3;

        if (cap < need) {
            free(tmp);
            tmp = malloc(need);
            cap = tmp ? need : 0;
        }
        if (!tmp || (g_gif_plate ? !skin_grab_plate(tmp, g_vw, g_vh)
                                 : !skin_grab(tmp, g_vw, g_vh))) {
            return;                     /* try again after the next present */
        }
        pthread_mutex_lock(&g_shutter_m);
        memcpy(g_shutter_buf, tmp, need);
        g_shutter_have = true;
        g_shutter_seen = true;
        g_shutter_want = false;
        pthread_mutex_unlock(&g_shutter_m);
    }
}

/*
 * The second pass, once the recording is closed: build a palette from the whole
 * clip, then map the clip onto it. Two ffmpeg runs, because the first has to
 * finish before the second can start.
 */
static void gif_convert(void)
{
    char cmd[2048];

    if (!g_gif_out[0]) {
        return;
    }
    if (access(g_gif_tmp, R_OK)) {
        fprintf(stderr, "octemu: no video was recorded, no gif\n");
        return;
    }
    /* Sizing already happened, once, on the way into the recording. All that
     * is left is the frame rate and the palette. */
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -y -i '%s' -vf 'fps=" GIF_FPS
             ",palettegen=stats_mode=diff' '%s.palette.png' && "
             "ffmpeg -v error -y -i '%s' -i '%s.palette.png' "
             "-lavfi 'fps=" GIF_FPS "[x];[x][1:v]paletteuse="
             "dither=sierra2_4a:diff_mode=rectangle' '%s'",
             g_gif_tmp, g_gif_out, g_gif_tmp, g_gif_out, g_gif_out);
    if (system(cmd)) {
        fprintf(stderr, "octemu: gif conversion failed; the "
                        "recording is still at %s\n", g_gif_tmp);
        return;
    }
    snprintf(cmd, sizeof cmd, "%s.palette.png", g_gif_out);
    unlink(cmd);
    unlink(g_gif_tmp);
    fprintf(stderr, "octemu: wrote %s\n", g_gif_out);
}

/* Until the link produces its first block the mov would be empty; pace boot
 * video on the wall clock with silent audio so the whole session is on film,
 * then hand over to the Octatrack's own clock at the first real block. */
static void *rec_boot_thread(void *arg)
{
    uint8_t *rgb = malloc((size_t)g_vw * g_vh * 3);
    static int16_t sil[BLOCKS_PER_VIDEO * FRAMES][2];

    (void)arg;
    while (!g_rec_live && !g_quit) {
        if (g_rec_live) {
            break;
        }
        /* Panel mode has nothing to film until the first present; holding the
         * audio back with it keeps the streams aligned. */
        if ((g_vmode != VIDEO_PANEL || g_shutter_seen) && g_rec_gate) {
            push_frame(rgb);
            if (g.audio_pipe_fd >= 0) {
                rec_push(&g_arec, sil, sizeof sil);
            }
        }
        usleep(2 * 1000000ll * BLOCKS_PER_VIDEO * FRAMES / RATE);
    }
    free(rgb);
    return NULL;
}

static void mov_begin(const char *path)
{
    int vp[2], ap[2];
    pthread_t bt;
    char vf[64] = "";
    const bool gif = g_gif_out[0] != 0;
    size_t ring;

    /* The size is fixed here because ffmpeg is told it once, at exec. */
    if (g_vmode == VIDEO_PANEL) {
        int ow, oh;

        if (!skin_output_size(&ow, &oh)) {
            emu_die("--video panel needs the panel window (not --headless, "
                    "and 'make panel' must have run)");
        }
        if (gif) {
            int pw, ph, nw, nh;

            g_gif_plate = true;
            if (!skin_plate_size(&pw, &ph) || !skin_plate_native(&nw, &nh)) {
                emu_die("the panel has no plate rect (run 'make panel')");
            }
            if (!g_gif_w) {
                g_gif_w = nw;            /* the plate as the artwork draws it */
            }
            if (g_gif_w <= pw && pw % g_gif_w == 0 &&
                ph % (pw / g_gif_w) == 0) {
                g_vw = g_gif_w;          /* an exact reduction: do it here */
                g_vh = ph / (pw / g_gif_w);
            } else {
                g_vw = pw;               /* anything else: one lanczos there */
                g_vh = ph;
                if (pw > g_gif_w) {
                    snprintf(vf, sizeof vf, "scale=%d:-1:flags=lanczos",
                             g_gif_w);
                }
            }
        } else {
            g_vw = g_panel_w & ~1;                   /* h264 wants even sides */
            g_vh = ((int)((int64_t)g_vw * oh / ow)) & ~1;
        }
        g_shutter_buf = calloc(1, (size_t)g_vw * g_vh * 3);
        g_shutter_want = true;
        if (!g_shutter_buf) {
            emu_die("out of memory for the panel video buffer");
        }
    } else {
        g_vw = VID_W;
        g_vh = VID_H;
    }
    if (pipe(vp) || (!gif && pipe(ap))) {
        emu_die("pipe");
    }
    if (gif) {
        ap[0] = ap[1] = -1;
    }
    g.ffmpeg = fork();
    if (g.ffmpeg == 0) {
        char size[32], rate[32];
        /* ☠ Count the entries before adding one: this held 32 and the list
         * grew to 35, which smashed the child's stack and left a recording
         * whose every frame was dropped because nothing was reading the pipe.
         * The assert below is the guard that would have caught it. */
        const char *argv[48];
        int n = 0;

        dup2(vp[0], 3);
        close(vp[1]);
        if (!gif) {
            dup2(ap[0], 4);
            close(ap[1]);
        }
        snprintf(size, sizeof size, "%dx%d", g_vw, g_vh);
        snprintf(rate, sizeof rate, "%.4f",
                 (double)RATE / FRAMES / BLOCKS_PER_VIDEO);
        argv[n++] = "ffmpeg";     argv[n++] = "-y";
        argv[n++] = "-loglevel";  argv[n++] = "error";
        argv[n++] = "-f";         argv[n++] = "rawvideo";
        argv[n++] = "-pix_fmt";   argv[n++] = "rgb24";
        argv[n++] = "-s";         argv[n++] = size;
        argv[n++] = "-framerate"; argv[n++] = rate;
        argv[n++] = "-i";         argv[n++] = "/dev/fd/3";
        /* ☠ A GIF has no audio, and giving ffmpeg a second input it has to
         * interleave against is what made it stop draining the video: it
         * blocks waiting on the slower stream while the ring behind the fast
         * one overflows. One input, and the frames go through as fast as the
         * encoder can take them. */
        if (!gif) {
            argv[n++] = "-f";     argv[n++] = "s16le";
            argv[n++] = "-ar";    argv[n++] = "44100";
            argv[n++] = "-ac";    argv[n++] = "2";
            argv[n++] = "-i";     argv[n++] = "/dev/fd/4";
        }
        if (vf[0]) {
            argv[n++] = "-vf";    argv[n++] = vf;
            /* ☠ The scale filter runs single-threaded by default, and a plate
             * frame is 8.6 MB: MEASURED, one thread sustained about 10 fps of
             * lanczos and the ring dropped 292 of 873 frames — a film that
             * silently plays a third too fast. Say how many threads it may
             * have and it keeps up with room to spare. */
            argv[n++] = "-filter_threads"; argv[n++] = "4";
        }
        if (gif) {
            argv[n++] = "-c:v";   argv[n++] = "ffv1";
            argv[n++] = "-threads"; argv[n++] = "4";
        } else {
            argv[n++] = "-c:v";   argv[n++] = "h264";
            argv[n++] = "-pix_fmt"; argv[n++] = "yuv420p";
        }
        if (!gif) {
            argv[n++] = "-c:a";   argv[n++] = "pcm_s16le";
            argv[n++] = "-shortest";
        }
        argv[n++] = path;
        assert(n < (int)(sizeof argv / sizeof *argv));
        argv[n] = NULL;
        execvp("ffmpeg", (char *const *)argv);
        /* Say so. Without this a missing ffmpeg is a .mov recording that
         * silently produces nothing: the child dies here, the parent keeps
         * filling a pipe with nobody on the other end, and the run looks
         * normal until you go looking for the file. */
        perror("octemu: exec ffmpeg (brew install ffmpeg)");
        _exit(127);
    }
    close(vp[0]);
    if (!gif) {
        close(ap[0]);
    }
    /* Sized in frames, not bytes: a plate grab at retina is ~8 MB, so a fixed
     * 32 MB would be four frames of slack and a brief encoder stall would show
     * up as dropped video. Sixteen frames of slack, wherever that lands. */
    ring = (size_t)g_vw * g_vh * 3 * 16;
    rec_begin(&g_vrec, vp[1], ring < (32u << 20) ? 32u << 20 : ring);
    if (!gif) {
        rec_begin(&g_arec, ap[1], 8u << 20);
    }
    g.video_fd = vp[1];
    g.audio_pipe_fd = gif ? -1 : ap[1];
    pthread_create(&bt, NULL, rec_boot_thread, NULL);
    pthread_detach(bt);
}

/* ---- live monitor ---------------------------------------------------------
 * The Octatrack runs below real time, so the monitor stretches its output to
 * the wall clock. The producer only ever appends to a ring; SDL's callback
 * pulls through one fractional read cursor that is never reset, so the output
 * is one continuous resampling of the Octatrack's stream — it can be
 * pitch-shifted, never torn. Underflow HOLDS the last sample rather than
 * emitting a zero (a zero is a click; a hold is a tiny flat spot).
 */
#define MON_CAP (1 << 17)             /* frames; ~3 s at 44.1 kHz */
static struct {
    int16_t l[MON_CAP], r[MON_CAP];
    volatile uint64_t w;              /* producer write cursor, frames */
    double rd;                        /* consumer read cursor, frames  */
    double ratio;                     /* source frames per output frame */
    uint64_t w_prev;
    double rate;                      /* the pitch anchor */
    int16_t last_l, last_r;
    bool primed;
    uint64_t starved;
    /* The ratio IS the pitch, and this path has no test over it, so its spread
     * is reported at exit — always, not behind a flag. A correct monitor holds
     * the ratio near the Octatrack's pace; drift shows up here as cents. */
    double lo, hi;
} g_mon;

/*
 * Source frames of cushion. It is the only thing between a stall and a held
 * sample, and it sets how hard the level trim has to work — and the trim
 * is audible, so a small cushion buys latency at the price of pitch wobble.
 * Measured on the trig fixture, and ☠ NOTE THE VARIANCE: repeat runs of the
 * SAME setting disagree, because how much the Octatrack stalls varies per run.
 *
 *    100 ms  187k starved frames (4.2 s of held sample in a 60 s run)
 *    250 ms  0.6k-43k starved, 171-280 cents of post-boot pitch wobble
 *    750 ms  0-44k starved, 98-264 cents
 *
 * 250 and 750 are indistinguishable in that noise and 100 is clearly worse, so
 * this is 250 — the cheapest setting the evidence supports. Do not tune it
 * against a single run.
 */
static unsigned kAim = RATE / 4;       /* --audio-cushion overrides it */
/* SDL device buffer, frames: --audio-buffer overrides it. */
static unsigned g_dev_samples = 512;

void audio_set_buffers(unsigned dev_samples, unsigned cushion_ms)
{
    if (dev_samples) {
        g_dev_samples = dev_samples;
    }
    if (cushion_ms) {
        kAim = (unsigned)((uint64_t)RATE * cushion_ms / 1000);
    }
}
/* Anchor creep per callback (~23 s time constant at 512 frames) and the two
 * level-trim gains. */
static const double kAnchorCreep = 0.0005, kTrimP = 0.02, kTrimQ = 0.10;

static void monitor_push(const int32_t out[FRAMES][SLOTS])
{
    const uint64_t w = g_mon.w;

    for (int f = 0; f < FRAMES; f++) {
        /* MAIN = the direct pair (TX 1/2) plus the master pair (TX 3/4).
         * In normal mode 3/4 is silent; with a MASTER track the program
         * moves there entirely (measured: the direct pair goes quiet and
         * the master output lands on 3/4), so the sum is the audible MAIN
         * in both modes and keeps non-master recordings bit-identical. */
        const int32_t l = (out[f][1] >> 8) + (out[f][3] >> 8);
        const int32_t r = (out[f][2] >> 8) + (out[f][4] >> 8);

        g_mon.l[(w + f) % MON_CAP] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        g_mon.r[(w + f) % MON_CAP] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    __atomic_store_n(&g_mon.w, w + FRAMES, __ATOMIC_RELEASE);
}

static void monitor_cb(void *unused, Uint8 *stream, int len)
{
    int16_t *o = (int16_t *)stream;
    const int n = len / (int)sizeof(int16_t) / 2;
    const uint64_t w = __atomic_load_n(&g_mon.w, __ATOMIC_ACQUIRE);
    double avail, inst, err;

    (void)unused;
    if (!g_mon.primed) {
        if (w < kAim) {                       /* wait for a cushion */
            memset(stream, 0, (size_t)len);
            return;
        }
        g_mon.primed = true;
        g_mon.rd = (double)w - kAim;
        g_mon.w_prev = w;
        /*
         * The pitch anchor. Three things it must NOT be, each measured:
         *
         *  - the integral of the buffer error (the original bug). Buffer level
         *    is itself an integral, so that is two integrators with no damping
         *    — an undamped oscillator. Measured: ratio orbiting 0.229..0.435
         *    on a ~5 s period, an ELEVEN SEMITONE drift, forever.
         *  - a fast measurement of arrivals. The link carries no debt, so a
         *    stall is followed by a catch-up burst and a fast tracker renders
         *    that burst as pitch (253 cents after boot).
         *  - the nominal pace as a CONSTANT. The Octatrack does not sustain
         *    nominal, so the trim ends up permanently pinned against the
         *    shortfall (141-145 cents, cushion never filling).
         *
         * So: seed at nominal, then creep toward the measured sustained rate
         * with a time constant far longer than any burst. The slow anchor
         * removes the steady-state offset; the proportional trim supplies the
         * damping the original loop never had.
         */
        /* The Octatrack runs at REAL TIME, so nominal is 1.0 either way: the
         * throttle holds it there and unthrottled overshoots it. */
        g_mon.rate = 1.0;
        g_mon.ratio = g_mon.rate;
    }

    avail = (double)w - g_mon.rd;
    inst = (double)(w - g_mon.w_prev) / (double)n;
    g_mon.w_prev = w;
    if (inst > 0.0) {
        g_mon.rate += kAnchorCreep * (inst - g_mon.rate);
    }
    /* The buffer level only TRIMS the anchor, through a proportional term that
     * cannot accumulate. The quadratic term is idle in the normal band (0.1%
     * at err=0.1) and gives the loop authority only when the buffer really is
     * draining. */
    err = (avail - kAim) / (double)kAim;
    if (err > 1.0)  err = 1.0;
    if (err < -1.0) err = -1.0;
    g_mon.ratio = g_mon.rate * (1.0 + kTrimP * err + kTrimQ * err * fabs(err));
    if (g_mon.ratio < 0.02) g_mon.ratio = 0.02;
    if (g_mon.ratio > 4.0)  g_mon.ratio = 4.0;
    if (!g_mon.lo || g_mon.ratio < g_mon.lo) g_mon.lo = g_mon.ratio;
    if (g_mon.ratio > g_mon.hi) g_mon.hi = g_mon.ratio;

    for (int i = 0; i < n; i++) {
        const uint64_t idx = (uint64_t)g_mon.rd;
        double fr, vl, vr;

        if (idx + 1 >= w) {                   /* starved: hold, don't click */
            g_mon.starved++;
            o[2 * i] = (int16_t)(g.phones * g_mon.last_l);
            o[2 * i + 1] = (int16_t)(g.phones * g_mon.last_r);
            continue;
        }
        fr = g_mon.rd - (double)idx;
        vl = g_mon.l[idx % MON_CAP]
           + (g_mon.l[(idx + 1) % MON_CAP] - g_mon.l[idx % MON_CAP]) * fr;
        vr = g_mon.r[idx % MON_CAP]
           + (g_mon.r[(idx + 1) % MON_CAP] - g_mon.r[idx % MON_CAP]) * fr;
        g_mon.last_l = (int16_t)vl;
        g_mon.last_r = (int16_t)vr;
        /* The ring already holds 16-BIT samples (monitor_push shifts the
         * 24-bit MAIN slots down), so phones is the ONLY gain allowed here.
         * Dividing by 256 as well leaves the monitor 48 dB down — silent —
         * while every recording stays perfect. */
        o[2 * i] = (int16_t)(g.phones * vl);
        o[2 * i + 1] = (int16_t)(g.phones * vr);
        g_mon.rd += g_mon.ratio;
    }
    /* The producer may have lapped us while we were behind; never read stale. */
    if ((double)w - g_mon.rd > (double)(MON_CAP - 4 * FRAMES)) {
        g_mon.rd = (double)w - kAim;
    }
}

/* ---- the consumer thread -------------------------------------------------- */
static bool audio_connect(const char *path)
{
    char line[128];
    size_t got = 0;
    int sfd;
    void *p;

    for (int i = 0; i < 200 && !g_quit; i++) {
        struct sockaddr_un a = { .sun_family = AF_UNIX };

        g.sock = socket(AF_UNIX, SOCK_STREAM, 0);
        strncpy(a.sun_path, path, sizeof a.sun_path - 1);
        if (connect(g.sock, (struct sockaddr *)&a, sizeof a) == 0) {
            break;
        }
        close(g.sock);
        g.sock = -1;
        usleep(100000);
    }
    if (g.sock < 0) {
        return false;
    }
    /* One hello line names the shared ring; the socket then carries nothing
     * and stays open only as a death notification. */
    while (got < sizeof line - 1) {
        ssize_t r = read(g.sock, line + got, 1);

        if (r <= 0) {
            return false;
        }
        if (line[got] == '\n') {
            break;
        }
        got += (size_t)r;
    }
    line[got] = 0;
    if (strncmp(line, OT_SHM_HELLO, strlen(OT_SHM_HELLO)) != 0) {
        fprintf(stderr, "octemu: bad audio hello '%s'\n", line);
        return false;
    }
    sfd = shm_open(line + strlen(OT_SHM_HELLO), O_RDWR, 0);
    if (sfd < 0) {
        return false;
    }
    p = mmap(NULL, sizeof(OtAudioShm), PROT_READ | PROT_WRITE, MAP_SHARED,
             sfd, 0);
    close(sfd);
    if (p == MAP_FAILED) {
        return false;
    }
    g.shm = p;
    if (g.shm->magic != OT_SHM_MAGIC) {
        fprintf(stderr, "octemu: audio shm magic mismatch\n");
        return false;
    }
    /* Adopt the shim's throttle state UNCONDITIONALLY, including 0 — the
     * shim owns it, not this process. (It used to be a divisor, and treating
     * 0 as "unset" left the monitor's pitch anchor at the throttled default
     * while the Octatrack ran flat out.) */
    g.throttled = g.shm->throttled;
    return true;
}

static void *audio_main(void *arg)
{
    const char *path = arg;
    uint64_t frame = 0;
    uint8_t *rgb = malloc((size_t)g_vw * g_vh * 3);
    int spins = 0;

    if (!audio_connect(path)) {
        free(rgb);
        return NULL;
    }
    while (!g_quit) {
        int32_t out[FRAMES][SLOTS], in[FRAMES][INS];
        uint64_t o, h;

        /* Wait for a block. There is no deadline and no throttle here: the
         * shim paces the guest, and this process simply takes what has been
         * produced. Being descheduled costs nothing but ring occupancy. */
        while (__atomic_load_n(&g.shm->out_head, __ATOMIC_ACQUIRE)
                   == g.shm->out_tail) {
            char b;

            if (g_quit) {
                goto done;
            }
            if (recv(g.sock, &b, 1, MSG_DONTWAIT | MSG_PEEK) == 0) {
                goto done;
            }
            usleep(spins++ < 8 ? 20 : 200);
        }
        spins = 0;
        o = g.shm->out_tail;
        memcpy(out, g.shm->out[o & (OT_SHM_OUT_CAP - 1)], sizeof out);
        __atomic_store_n(&g.shm->out_tail, o + 1, __ATOMIC_RELEASE);

        for (int f = 0; f < FRAMES; f++, frame++) {
            for (int s = 0; s < INS; s++) {
                in[f][s] = ot_in_next(&g.in[s], frame, RATE);
            }
        }
        h = g.shm->in_head;
        memcpy(g.shm->in[h & (OT_SHM_IN_CAP - 1)], in, sizeof in);
        __atomic_store_n(&g.shm->in_head, h + 1, __ATOMIC_RELEASE);

        /* Which TX slots carried audio, reported at exit. MAIN is slots 1/2
         * (read-disasm) and the rest are assumptions, so a run that puts sound
         * somewhere unexpected should be able to say so — ship_nz and the
         * recording both look at 1/2 alone and cannot. */
        for (int f = 0; f < FRAMES; f++) {
            for (int s = 0; s < SLOTS; s++) {
                const int32_t v = out[f][s];
                const int32_t m = v < 0 ? -v : v;

                if (m > g.slot_peak[s]) {
                    g.slot_peak[s] = m;
                }
            }
        }
        if (g.wav || g.audio_pipe_fd >= 0) {
            int16_t st[FRAMES][2];      /* MAIN = TX 1/2 + 3/4 (master) */

            for (int f = 0; f < FRAMES; f++) {
                const int32_t l = (out[f][1] >> 8) + (out[f][3] >> 8);
                const int32_t r = (out[f][2] >> 8) + (out[f][4] >> 8);

                st[f][0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                st[f][1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
            if (g.wav) {
                fwrite(st, 1, sizeof st, g.wav);
                g.wav_frames += FRAMES;
            }
            if (g.audio_pipe_fd >= 0) {
                rec_push(&g_arec, st, sizeof st);
            }
        }
        if (g.dev) {
            monitor_push(out);
        }
        /* ☠ A real block has arrived, so the boot thread must stop pacing the
         * video off the wall clock. This lives HERE, not beside the audio
         * push: a .gif is recorded without an audio stream at all, and when
         * this flag sat inside that branch the boot thread never handed over.
         * It went on pushing wall-paced frames for the whole run, on top of
         * the guest-paced ones — a 25 s walk came out as a 116 s film. */
        g_rec_live = true;
        g.blocks++;
        if (g.video_fd >= 0 && g.blocks % BLOCKS_PER_VIDEO == 0) {
            push_frame(rgb);
        }
    }
done:
    if (g.shm) {
        g.shm->consumer_alive = 0;
    }
    fprintf(stderr, "octemu: consumed %llu blocks\n",
            (unsigned long long)g.blocks);
    if (g.video_fd >= 0) {
        fprintf(stderr, "octemu: video frames=%llu repeats=%llu\n",
                (unsigned long long)g_video_frames,
                (unsigned long long)g_video_repeats);
    }
    fprintf(stderr, "octemu: tx slot peaks");
    for (int s = 0; s < SLOTS; s++) {
        fprintf(stderr, " %d:%d", s, g.slot_peak[s]);
    }
    fputc('\n', stderr);
    if (g_mon.primed) {
        fprintf(stderr, "octemu: monitor ratio %.4f-%.4f "
                "(%.0f cents) anchor %.4f starved %llu frames\n",
                g_mon.lo, g_mon.hi,
                1200.0 * log2(g_mon.hi / (g_mon.lo > 0 ? g_mon.lo : 1)),
                g_mon.rate, (unsigned long long)g_mon.starved);
    }
    free(rgb);
    return NULL;
}

bool audio_start(const char *sock_path, const InGen in[INS],
                 const char *recording, bool live_monitor)
{
    memcpy(g.in, in, sizeof g.in);
    if (recording) {
        const char *dot = strrchr(recording, '.');

        if (dot && !strcmp(dot, ".wav")) {
            wav_begin(recording);
        } else if (dot && !strcmp(dot, ".gif")) {
            snprintf(g_gif_out, sizeof g_gif_out, "%s", recording);
            /* Matroska, because the intermediate is ffv1. */
            snprintf(g_gif_tmp, sizeof g_gif_tmp, "%s.recording.mkv", recording);
            /* --video-width sizes the GIF itself; mov_begin picks the
             * plate's own width when nothing was asked for. */
            g_gif_w = g_panel_w != PANEL_W_DEFAULT ? g_panel_w : 0;
            mov_begin(g_gif_tmp);
        } else {
            mov_begin(recording);
        }
    }
    if (live_monitor) {
        SDL_AudioSpec want = {0}, have;

        want.freq = RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = (Uint16)(g_dev_samples > 32768 ? 32768 : g_dev_samples);
        want.callback = monitor_cb;
        g.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g.dev) {
            fprintf(stderr, "octemu: live monitor: device buffer %u frames "
                    "(asked %u, %.1f ms), cushion %u frames (%.0f ms), driver %s\n",
                    have.samples, want.samples, 1000.0 * have.samples / have.freq,
                    kAim, 1000.0 * kAim / RATE, SDL_GetCurrentAudioDriver());
            SDL_PauseAudioDevice(g.dev, 0);
        }
    }
    g.running = true;
    return pthread_create(&g.thread, NULL, audio_main, (void *)sock_path) == 0;
}

void audio_stop(void)
{
    if (!g.running) {
        return;
    }
    g.running = false;
    pthread_join(g.thread, NULL);
    if (g.video_fd >= 0) {
        rec_end(&g_vrec);
        /* ☠ Dropped video is not a cosmetic loss. Every recorded frame is one
         * frame of the film, so frames the encoder never saw come out as time
         * REMOVED: a third of them missing and the whole demo plays a third
         * too fast, with nothing on the picture to say so. */
        if (g_vrec.dropped) {
            fprintf(stderr, "octemu: ☠ %llu video frames never "
                    "reached the encoder — the film is short by that much and "
                    "PLAYS FAST\n",
                    (unsigned long long)(g_vrec.dropped /
                                         ((size_t)g_vw * g_vh * 3)));
        }
    }
    if (g.audio_pipe_fd >= 0) {
        rec_end(&g_arec);
    }
    if (g.ffmpeg > 0) {
        int st;

        waitpid(g.ffmpeg, &st, 0);
    }
    gif_convert();
    wav_end();
    if (g.sock >= 0) {
        close(g.sock);
    }
}
