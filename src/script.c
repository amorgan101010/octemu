/*
 * The scripted walk: one JSON object per line.
 *
 * v1 grew three near-identical retry state machines (until / until_gone /
 * until_lamp) plus four separate wait_* blocks, each with its own static
 * locals. They are one thing here: every step is an ACTION plus an optional
 * CONDITION, and one evaluator polls one condition.
 *
 *   {"wait_text":"PTCH"}                       condition only
 *   {"tap":"PLAY","until_lamp":1}              action + condition
 *   {"tap":"NO","until_gone":"5RC 5ETUP"}
 *   {"press":"FUNC"} {"release":"FUNC"}
 *   {"encoder":0,"delta":1} {"crossfader":128} {"crossfader":255,"over_ms":4000}
 *   {"tap":"PLAY","crossfader":255,"over_ms":4000}   slide WITH the gesture
 *   {"encoder":0,"delta":1,"times":12,"over_ms":1400}  turn it, detent by
 *                                              detent, over that long
 *   {"screenshot":"x.ppm"} {"panelshot":"x.ppm"} {"mark":"t1"} {"dump":1}
 *   {"record":0} {"record":1}                  hold --recording shut, or open
 *   {"click":"btn-play"} {"drag":"knob-a","delta":8} {"resize":900}
 *
 * ☠ Waits gate on the UI, never on timers, because only the screen is evidence
 * that a gesture landed. A tap carrying a condition is SELF-VERIFYING: if the
 * condition still fails when the verify window closes, the key was lost (the
 * dialog can appear a beat before its handler is armed) and the tap is redone.
 *
 * Every step carries a deadline. A wedged dialog or a lost transport must
 * fail the walk in about a minute, not idle to the run timeout — so failures
 * cost a minute of iteration, not twenty-five.
 *
 * ☠ THE STEP EVALUATOR RUNS ON THE GUEST'S CLOCK, not the host's. Key holds,
 * gaps, settles, verify windows and deadlines are all counted in guest audio
 * time (one block = 16 frames at 44.1 kHz, `audio_blocks()`), because that is
 * the clock the firmware debounces and repaints on. The emulator does not hold
 * realtime on a busy host: measured at load average 121, a walk whose waits
 * summed to 52 s of WALL time gave the guest 5.0 s of audio — every gesture
 * landed in an Octatrack that had barely booted, and the reference TRIG9 burst
 * never sounded. In wall time a 150 ms hold is ~15 ms of guest time there, far
 * below what the panel link needs. The run's --timeout stays wall-clock: it is
 * the backstop against a guest that has stopped producing audio at all.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"

enum {
    A_NONE, A_TAP, A_PRESS, A_RELEASE, A_ENCODER, A_XFADER, A_SCREENSHOT,
    A_PANELSHOT, A_CLICK, A_DRAG, A_RESIZE, A_MARK, A_DUMP, A_RECORD
};
enum { C_NONE, C_TEXT, C_GONE, C_LAMP, C_MS, C_BLOCKS };

typedef struct {
    int action;
    char arg[96];
    long value, delta, push, taps, hold_ms, verify_ms, timeout_ms, over_ms;
    long settle_ms, times;
    long xfade_to;
    bool xfade;                        /* a slide riding along with the action */
    int cond;
    char text[64];
    long lamp, lamp_lit, ms, blocks;
} Step;

/* Tap timing, all measured on the Octatrack's UART framing. */
#define TAP_HOLD_MS        150     /* a single tap                       */
#define TAP_HOLD_DOUBLE_MS 100
#define TAP_GAP_MS         120     /* back-to-back edges do not register  */
/*
 * How long a tap waits before it starts judging its condition. It is not
 * politeness: a walk that polls too early can read the screen BEFORE the
 * gesture has had any effect, and for an until_gone that is a false pass —
 * the dialog it is waiting to see disappear has not appeared yet.
 *
 * ☠ So "settle_ms" may shorten this per step, but only on a POSITIVE
 * condition (until / until_lamp), where an early poll simply fails and polls
 * again. Never shorten an until_gone.
 */
#define TAP_SETTLE_MS      400     /* let the Octatrack act before judging */
#define TAP_VERIFY_MS     3000
#define STEP_TIMEOUT_MS  90000

enum { PH_ACT, PH_HELD, PH_GAP, PH_SETTLE, PH_POLL };

/*
 * A crossfader slide. The panel MCU reports an ADC, so a move is a stream of
 * values, not one: {"crossfader":255,"over_ms":4000} walks there over four
 * seconds of GUEST time and holds the step open until it arrives. Every pass
 * of the main loop advances it, so the handle moves at the renderer's rate
 * rather than the walk's — a fader that stepped once per script line would
 * stutter visibly in a 33 fps recording.
 */
typedef struct {
    bool on;
    bool enc;                          /* a turn, not a slide */
    int from, to;                      /* slide: wire values */
    int idx, dir, total, done;         /* turn: which encoder, how many */
    int64_t t0, ms;
} Ramp;

/*
 * Several at once: a walk that turns two knobs together is two of these, and
 * the crossfader slide riding on a tap is a third. A step only blocks on the
 * one it starts, so give a turn its own condition ({"wait_guest_ms":0} is the
 * usual one) and the walk moves on while it keeps running.
 */
static Ramp R[4];

static Ramp *ramp_slot(bool enc, int idx)
{
    Ramp *free_slot = NULL;

    for (size_t i = 0; i < sizeof R / sizeof *R; i++) {
        if (R[i].on && R[i].enc == enc && (!enc || R[i].idx == idx)) {
            return &R[i];              /* already driving this one */
        }
        if (!R[i].on && !free_slot) {
            free_slot = &R[i];
        }
    }
    return free_slot ? free_slot : &R[0];
}

static struct {
    FILE *f;
    char line[512];
    Step s;
    bool have_step, done, failed;
    int phase, taps_left, gone_streak;
    int64_t t_phase, t_verify, t_deadline, t_poll, wall_phase;
    uint64_t blk_phase;
    uint32_t last_gen;
} S;

/* Guest audio milliseconds — the walk's clock. */
static int64_t guest_ms(void)
{
    return (int64_t)(audio_blocks() * (uint64_t)FRAMES * 1000ull / RATE);
}

/* ---- the smallest JSON reader that reads these files --------------------- */
static bool json_str(const char *line, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t n = 0;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(line, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':')) || !(p = strchr(p, '"'))) {
        return false;
    }
    p++;
    while (*p && *p != '"' && n + 1 < cap) {
        out[n++] = *p++;
    }
    out[n] = 0;
    return true;
}

static bool json_int(const char *line, const char *key, long *out)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(line, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) {
        return false;
    }
    *out = strtol(p + 1, NULL, 10);
    return true;
}

static bool parse(const char *line, Step *s)
{
    char buf[96];
    long v;

    memset(s, 0, sizeof *s);
    s->taps = 1;
    s->verify_ms = TAP_VERIFY_MS;
    s->lamp_lit = 1;

    if (json_str(line, "tap", buf, sizeof buf)) {
        s->action = A_TAP;
    } else if (json_str(line, "press", buf, sizeof buf)) {
        s->action = A_PRESS;
    } else if (json_str(line, "release", buf, sizeof buf)) {
        s->action = A_RELEASE;
    } else if (json_str(line, "screenshot", buf, sizeof buf)) {
        s->action = A_SCREENSHOT;
    } else if (json_str(line, "panelshot", buf, sizeof buf)) {
        s->action = A_PANELSHOT;
    } else if (json_str(line, "click", buf, sizeof buf)) {
        s->action = A_CLICK;
    } else if (json_str(line, "drag", buf, sizeof buf)) {
        s->action = A_DRAG;
    } else if (json_str(line, "mark", buf, sizeof buf)) {
        s->action = A_MARK;
    } else if (json_int(line, "encoder", &v)) {
        s->action = A_ENCODER;
        s->value = v;
    } else if (json_int(line, "crossfader", &v)) {
        s->action = A_XFADER;
        s->value = v;
    } else if (json_int(line, "record", &v)) {
        s->action = A_RECORD;
        s->value = v;
    } else if (json_int(line, "resize", &v)) {
        s->action = A_RESIZE;
        s->value = v;
    } else if (strstr(line, "\"dump\"")) {
        s->action = A_DUMP;
    } else {
        buf[0] = 0;
    }
    snprintf(s->arg, sizeof s->arg, "%s", buf);
    /* ☠ An unknown key name used to be sent as key -1, i.e. silently
     * dropped: the trig8 fixture tapped "T1".."T8" (the keys are TRACK1..8)
     * for its whole life, so its "mute every track but T4" never muted
     * anything. Fail the walk instead. */
    if ((s->action == A_TAP || s->action == A_PRESS ||
         s->action == A_RELEASE) && panel_button_id(buf) < 0) {
        fprintf(stderr, "octemu: script: unknown key \"%s\" (see src/panel.c)\n",
                buf);
        return false;
    }

    if (json_str(line, "wait_text", s->text, sizeof s->text) ||
        json_str(line, "until", s->text, sizeof s->text)) {
        s->cond = C_TEXT;
        ocr_canon(s->text);
    } else if (json_str(line, "wait_gone", s->text, sizeof s->text) ||
               json_str(line, "until_gone", s->text, sizeof s->text)) {
        s->cond = C_GONE;
        ocr_canon(s->text);
    } else if (json_int(line, "until_lamp", &s->lamp) ||
               json_int(line, "wait_lamp", &s->lamp)) {
        s->cond = C_LAMP;
        json_int(line, "lit", &s->lamp_lit);
    } else if (json_int(line, "wait_ms", &s->ms)) {
        s->cond = C_MS;
    } else if (json_int(line, "wait_guest_ms", &s->ms)) {
        /* Guest AUDIO milliseconds — see C_BLOCKS in cond_check. */
        s->cond = C_BLOCKS;
        s->blocks = (s->ms * RATE / 1000 + FRAMES - 1) / FRAMES;
    } else if (json_int(line, "wait_blocks", &s->blocks)) {
        s->cond = C_BLOCKS;
    }

    json_int(line, "over_ms", &s->over_ms);
    if (s->action == A_XFADER && s->over_ms > 0 && !s->cond) {
        /* The slide IS the wait: the step ends when the handle arrives. */
        s->cond = C_BLOCKS;
        s->blocks = (s->over_ms * RATE / 1000 + FRAMES - 1) / FRAMES;
    } else if (s->action == A_ENCODER && s->over_ms > 0 && !s->cond) {
        /* The turn IS the wait, the same as a slide. */
        s->cond = C_BLOCKS;
        s->blocks = (s->over_ms * RATE / 1000 + FRAMES - 1) / FRAMES;
    } else if (s->over_ms > 0 && json_int(line, "crossfader", &s->xfade_to)) {
        /* A slide hung on another gesture starts with it, to the frame, and
         * keeps running while the walk moves on: put it on the tap and the
         * fader leaves its end as the key goes down, not a beat later when
         * the tap has finished proving itself. */
        s->xfade = true;
    }
    json_int(line, "delta", &s->delta);
    json_int(line, "push", &s->push);
    json_int(line, "hold_ms", &s->hold_ms);
    json_int(line, "settle_ms", &s->settle_ms);
    json_int(line, "times", &s->times);
    json_int(line, "verify_ms", &s->verify_ms);
    json_int(line, "timeout_ms", &s->timeout_ms);
    if (json_int(line, "double", &v) && v) {
        s->taps = 2;
    }
    if (s->action == A_ENCODER && !s->delta) {
        s->delta = 1;
    }
    /*
     * Deadlines differ by kind, and the difference is load-bearing.
     *
     * A TAP that carries a condition is self-verifying, so a deadline means
     * "this key never landed" — fail the walk, fast, rather than idling to the
     * run timeout. 90 s unless the step says otherwise (the PLAY step needs
     * 600 s for the bank-reload gate).
     *
     * A WAIT is a different claim: it says "get to this state", and a timeout
     * on one is often expected. The preview walk opens by waiting 8 s for
     * the SET DATE/TIME dialog, which on a settled fixture NEVER APPEARS —
     * making that fatal fails the walk on its first step. So a wait with an
     * explicit timeout logs and carries on, and a wait without one has no
     * deadline at all beyond the run's --timeout.
     */
    if (s->action == A_TAP && s->cond != C_NONE && !s->timeout_ms) {
        s->timeout_ms = STEP_TIMEOUT_MS;
    }
    return s->action != A_NONE || s->cond != C_NONE;
}

/* ---- conditions ----------------------------------------------------------- */
static void screenshot(const char *path)
{
    PanelState p;
    FILE *f = fopen(path, "wb");

    if (!f) {
        return;
    }
    panel_snapshot(&p);
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            fputc(p.fb[y][x] ? 255 : 0, f);
        }
    }
    fclose(f);
}

static void dump_screen(const char *why)
{
    PanelState p;
    char text[4096];

    panel_snapshot(&p);
    ocr_read(&p, text, sizeof text);
    fprintf(stderr, "octemu: %s:\n%s--\n", why, text);
}

/* -1 = not time to look yet, 0 = not met, 1 = met. */
static int cond_check(void)
{
    const Step *s = &S.s;

    switch (s->cond) {
    case C_NONE:
        return 1;
    case C_MS:
        /* Wall time, and the only wall-clock wait left — existing walks are
         * written in it. Prefer wait_guest_ms/wait_blocks in new ones. */
        return now_ms() - S.wall_phase >= s->ms;
    case C_BLOCKS:
        /*
         * ☠ Wait in GUEST time, not wall time. The emulator does not hold
         * realtime on a loaded host — measured on a host at load average 121,
         * a walk whose waits summed to 52 s of wall time gave the guest 5.0 s
         * of audio, so every gesture landed in an Octatrack that had barely
         * moved and the reference TRIG9 burst never sounded (tx slot peaks
         * 2:9 5:6, i.e. silence). Blocks are the guest's own clock: block N is
         * frame 16N of the recording, so a wait expressed in blocks means the
         * same thing on a quiet host and a saturated one.
         */
        return audio_blocks() - S.blk_phase >= (uint64_t)s->blocks;
    case C_LAMP:
        return panel_lamp_lit((int)s->lamp) == (s->lamp_lit != 0);
    case C_TEXT:
    case C_GONE: {
        PanelState p;
        char text[4096];
        const int64_t t = now_ms();
        bool present;

        /* Re-read on a framebuffer change, but also periodically: a static
         * screen (a dialog whose clock has stalled) must still match. */
        if ((panel_fb_gen() == S.last_gen && t - S.t_poll < 250) ||
            t - S.t_poll < 100) {
            return -1;
        }
        S.last_gen = panel_fb_gen();
        S.t_poll = t;
        panel_snapshot(&p);
        ocr_read(&p, text, sizeof text);
        ocr_canon(text);
        present = strstr(text, s->text) != NULL;
        if (s->cond == C_TEXT) {
            return present;
        }
        /* Debounce: a mid-repaint frame can hide text that is still there, so
         * require two consecutive confirmations before concluding "gone". */
        if (present) {
            S.gone_streak = 0;
            return 0;
        }
        return ++S.gone_streak >= 2;
    }
    default:
        return 1;
    }
}

/* ---- actions -------------------------------------------------------------- */
static void tap_down(void)
{
    panel_key(panel_button_id(S.s.arg), true);
    S.phase = PH_HELD;
    S.t_phase = guest_ms() + (S.s.hold_ms ? S.s.hold_ms
                              : S.s.taps > 1 ? TAP_HOLD_DOUBLE_MS : TAP_HOLD_MS);
}

/* Returns false if the step failed outright. */
/* Aim the crossfader somewhere and let ramp_advance() walk it there. */
static void ramp_start(int to, long ms)
{
    Ramp *r = ramp_slot(false, 0);
    PanelState p;

    panel_snapshot(&p);
    r->enc = false;
    r->from = p.xfader;
    r->to = to;
    r->t0 = guest_ms();
    r->ms = ms;
    r->on = true;
}

/*
 * A turn, paid out one detent at a time across `ms`. One frame carrying the
 * whole delta would be a jump: the firmware reads it as a flick, and the
 * panel's turn hint has nothing to animate.
 */
static void turn_start(int enc, int dir, int times, long ms)
{
    Ramp *r = ramp_slot(true, enc);

    r->enc = true;
    r->idx = enc;
    r->dir = dir < 0 ? -1 : 1;
    r->total = times;
    r->done = 0;
    r->t0 = guest_ms();
    r->ms = ms;
    r->on = true;
}

static bool act(void)
{
    const Step *s = &S.s;

    /* Before the gesture, not after: a slide asked for on this step leaves at
     * the same instant the key goes down. */
    if (s->xfade) {
        ramp_start((int)s->xfade_to, s->over_ms);
    }
    switch (s->action) {
    case A_NONE:
        break;
    case A_TAP:
        S.taps_left = (int)s->taps;
        tap_down();
        return true;
    case A_PRESS:
        panel_key(panel_button_id(s->arg), true);
        break;
    case A_RELEASE:
        panel_key(panel_button_id(s->arg), false);
        break;
    case A_ENCODER:
        if (s->over_ms > 0 && s->times > 0) {
            turn_start((int)s->value, (int)s->delta, (int)s->times,
                       s->over_ms);
        } else {
            panel_encoder((int)s->value, (int)s->delta);
        }
        break;
    case A_XFADER:
        if (s->over_ms > 0) {
            ramp_start((int)s->value, s->over_ms);
        } else {
            panel_xfader((int)s->value);
        }
        break;
    case A_RECORD:
        audio_record_gate(s->value != 0);
        break;
    case A_SCREENSHOT:
        screenshot(s->arg);
        break;
    case A_MARK:
        /* The BLOCK NUMBER is the recording's own timeline — block N is frame
         * 16N of the .wav — so a mark pins a gesture to an exact sample
         * offset. Wall time cannot do that: the pace varies with what the
         * guest is doing. */
        fprintf(stderr, "[mark] blk=%llu %lld %s\n",
                (unsigned long long)audio_blocks(),
                (long long)(now_ms() % 1000000), s->arg);
        break;
    case A_DUMP:
        dump_screen("dump");
        break;
    case A_PANELSHOT:
        if (!skin_ok()) {
            fprintf(stderr, "octemu: panelshot needs the panel "
                    "skin (windowed, make panel)\n");
            return false;
        }
        skin_shot(s->arg);
        break;
    case A_RESIZE:
        skin_resize((int)s->value);
        break;
    case A_CLICK:
    case A_DRAG:
        if (!skin_ok()) {
            fprintf(stderr, "octemu: %s needs the panel skin "
                    "(windowed, make panel)\n",
                    s->action == A_DRAG ? "drag" : "click");
            return false;
        }
        if (!(s->action == A_DRAG
                  ? skin_script_drag(s->arg, s->delta, s->push != 0)
                  : skin_script_click(s->arg))) {
            fprintf(stderr, "octemu: no such control: %s\n",
                    s->arg);
            return false;
        }
        break;
    }
    S.phase = PH_POLL;
    S.t_phase = guest_ms();
    S.wall_phase = now_ms();
    S.blk_phase = audio_blocks();
    return true;
}

/* ---- the evaluator -------------------------------------------------------- */
bool script_open(const char *path)
{
    S.f = fopen(path, "r");
    return S.f != NULL;
}

bool script_done(void)   { return S.done; }
bool script_failed(void) { return S.failed; }

static void fail(const char *why)
{
    fprintf(stderr, "octemu: %s at: %s", why, S.line);
    dump_screen("the screen reads");
    S.failed = true;
    S.done = true;
}

/* One pass of a slide in flight. Guest time, like every other clock here. */
static void ramp_advance(void)
{
    for (size_t i = 0; i < sizeof R / sizeof *R; i++) {
        Ramp *r = &R[i];
        int64_t dt;

        if (!r->on) {
            continue;
        }
        dt = guest_ms() - r->t0;
        if (r->enc) {
            int want = dt >= r->ms ? r->total : (int)(r->total * dt / r->ms);

            while (r->done < want) {
                panel_encoder(r->idx, r->dir);
                r->done++;
            }
            r->on = r->done < r->total;
            continue;
        }
        if (dt >= r->ms) {
            panel_xfader(r->to);
            r->on = false;
            continue;
        }
        panel_xfader(r->from + (int)((r->to - r->from) * dt / r->ms));
    }
}

bool script_step(void)
{
    if (!S.f || S.done) {
        return false;
    }
    ramp_advance();
    for (;;) {
        if (!S.have_step) {
            if (!fgets(S.line, sizeof S.line, S.f)) {
                S.done = true;
                return false;
            }
            if (S.line[0] == '\n' || S.line[0] == '#') {
                continue;
            }
            if (!parse(S.line, &S.s)) {
                fail("bad script step");
                return false;
            }
            S.have_step = true;
            S.phase = PH_ACT;
            S.gone_streak = 0;
            S.t_deadline = guest_ms() + S.s.timeout_ms;
            S.t_poll = 0;
        }

        switch (S.phase) {
        case PH_ACT:
            if (!act()) {
                S.failed = true;
                S.done = true;
                return false;
            }
            return true;                   /* one action per pass */

        case PH_HELD:
            if (guest_ms() < S.t_phase) {
                return true;
            }
            panel_key(panel_button_id(S.s.arg), false);
            if (--S.taps_left > 0) {
                S.phase = PH_GAP;
                S.t_phase = guest_ms() + TAP_GAP_MS;
                return true;
            }
            if (S.s.cond == C_NONE) {
                S.have_step = false;       /* nothing to judge; step is done */
                break;
            }
            /* Give the Octatrack time to act before judging: it runs below
             * wall pace and a dialog can take a second to open or close. */
            S.phase = PH_SETTLE;
            S.t_phase = guest_ms() + (S.s.settle_ms > 0 ? S.s.settle_ms
                                                        : TAP_SETTLE_MS);
            S.t_verify = guest_ms() + S.s.verify_ms;
            return true;

        case PH_GAP:
            if (guest_ms() < S.t_phase) {
                return true;
            }
            tap_down();
            return true;

        case PH_SETTLE:
            if (guest_ms() < S.t_phase) {
                return true;
            }
            S.phase = PH_POLL;
            S.t_phase = guest_ms();
            S.wall_phase = now_ms();
            S.blk_phase = audio_blocks();
            /* fall through */

        case PH_POLL: {
            const int met = cond_check();

            if (met == 1) {
                if (S.s.cond != C_NONE && S.s.cond != C_MS &&
                    S.s.cond != C_BLOCKS) {
                    fprintf(stderr, "octemu: %s %s\n",
                            S.s.cond == C_GONE ? "gone:" : "saw",
                            S.s.cond == C_LAMP ? "lamp" : S.s.text);
                }
                S.have_step = false;
                break;                     /* next step, same pass */
            }
            if (met == 0 && S.s.timeout_ms && guest_ms() >= S.t_deadline) {
                if (S.s.action == A_TAP) {
                    fail("STEP TIMEOUT");
                    return false;
                }
                fprintf(stderr, "octemu: TIMEOUT waiting %s\n",
                        S.s.cond == C_LAMP ? "lamp" : S.s.text);
                S.have_step = false;
                break;                     /* advisory: carry on */
            }
            /* A verified tap re-taps once its window closes without the
             * condition holding: the key was lost, not merely slow. */
            if (met == 0 && S.s.action == A_TAP && guest_ms() >= S.t_verify) {
                fprintf(stderr, "octemu: re-tap (key lost) at: %s", S.line);
                S.taps_left = (int)S.s.taps;
                S.gone_streak = 0;
                tap_down();
            }
            return true;
        }
        }
    }
}
