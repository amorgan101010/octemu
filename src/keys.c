/*
 * The computer keyboard as the panel.
 *
 * The layout is the one Gearmulator's Monomachine and Machinedrum windows use
 * (and digiemu's Digitakt and Digitone), so the keys the Elektron boxes share
 * sit on the same computer keys: the sixteen trigs on F1-F8 and 1-8, Ctrl for
 * FUNC, the arrows, Enter/Backspace for YES/NO, Space/End/Home for
 * PLAY/STOP/REC, the knobs on the left of the home and bottom rows, U-P and `
 * for the menus. Keys only the Octatrack has sit on keys that layout leaves
 * free, or on the nearest thing it has.
 *
 * Holding a key holds the panel key. Shift LATCHES what you press until Shift
 * is let go. Delete lets go of everything, as does the window losing focus.
 *
 * Knobs: hold the knob's key and tap - or = to turn it one detent, or [ or ]
 * to turn it with its push switch held (push-turn). Tapping the knob's key
 * without turning clicks the push switch. H is the crossfader, turned the same
 * way. Only the turn keys auto-repeat: holding = keeps turning.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

#include "emu.h"

#define FUNC_ID    45
#define ENC_PUSH   56              /* ENC1's key id; ENCn is ENC_PUSH + n-1 */
#define XFADER     8               /* the crossfader, as a hold-to-turn knob */
#define XF_STEP    8               /* wire units per tap, of 255 */
#define XF_BIG     32              /* ... with [ ] */
#define TAP_MS     90              /* a click's dwell, as skin.c's */

static const struct { SDL_Keycode key; const char *btn; } g_binds[] = {
    {SDLK_F1,"TRIG1"},{SDLK_F2,"TRIG2"},{SDLK_F3,"TRIG3"},{SDLK_F4,"TRIG4"},
    {SDLK_F5,"TRIG5"},{SDLK_F6,"TRIG6"},{SDLK_F7,"TRIG7"},{SDLK_F8,"TRIG8"},
    {SDLK_1,"TRIG9"},{SDLK_2,"TRIG10"},{SDLK_3,"TRIG11"},{SDLK_4,"TRIG12"},
    {SDLK_5,"TRIG13"},{SDLK_6,"TRIG14"},{SDLK_7,"TRIG15"},{SDLK_8,"TRIG16"},
    {SDLK_q,"TRACK1"},{SDLK_w,"TRACK2"},{SDLK_e,"TRACK3"},{SDLK_r,"TRACK4"},
    {SDLK_t,"TRACK5"},{SDLK_y,"TRACK6"},{SDLK_u,"TRACK7"},{SDLK_i,"TRACK8"},
    {SDLK_TAB,"CUE"},
    /* The menu row. The MM's KIT is the OT's PART; PROJ sits above it. */
    {SDLK_9,"PROJ"},{SDLK_o,"PART"},{SDLK_0,"AED"},{SDLK_p,"PTN"},
    {SDLK_b,"MIX"},{SDLK_m,"ARR"},{SDLK_n,"MIDI"},
    {SDLK_BACKSLASH,"TEMPO"},{SDLK_BACKQUOTE,"BANK"},
    /* The track pages on the home row, where the Digis keep theirs: AMP and
     * LFO on the same keys, FX1 (a filter by default) on the Digis' FLTR. */
    {SDLK_j,"FX2"},{SDLK_k,"SRC"},{SDLK_l,"FX1"},{SDLK_SEMICOLON,"AMP"},
    {SDLK_QUOTE,"LFO"},
    {SDLK_PAGEUP,"PAGE"},{SDLK_PAGEDOWN,"PAGE"},
    {SDLK_COMMA,"REC1"},{SDLK_PERIOD,"REC2"},{SDLK_SLASH,"REC3"},
    {SDLK_F9,"A"},{SDLK_F10,"B"},          /* scenes: the MM's bank keys */
    {SDLK_UP,"UP"},{SDLK_DOWN,"DOWN"},{SDLK_LEFT,"LEFT"},{SDLK_RIGHT,"RIGHT"},
    {SDLK_RETURN,"YES"},{SDLK_KP_ENTER,"YES"},{SDLK_BACKSPACE,"NO"},
    {SDLK_SPACE,"PLAY"},{SDLK_END,"STOP"},{SDLK_HOME,"REC"},
};

/* The panel's 2x3 grid (A B C over D E F), LEVEL and the crossfader after. */
static const struct { SDL_Keycode key; int enc; } g_knobs[] = {
    {SDLK_a,0},{SDLK_s,1},{SDLK_d,2},{SDLK_z,3},{SDLK_x,4},{SDLK_c,5},
    {SDLK_g,6},{SDLK_h,XFADER},
};

enum { HELD_BUTTON, HELD_KNOB, HELD_TURN };

static struct { SDL_Keycode key; int id; } g_user[128];
static int g_nuser;

static struct { SDL_Keycode key; int kind, id; } g_held[32];
static int g_nheld;

static uint64_t g_latched;          /* key ids latched by Shift */
static bool g_func;                 /* Ctrl holds FUNC */
static int g_knob = -1;             /* the knob whose key is held */
static bool g_turned;               /* ... and it was turned */
static int g_push = -1;             /* the push switch a push-turn holds */
static int g_tap = -1;              /* a click's push switch, until g_tap_up */
static int64_t g_tap_up;

bool keys_bind(int key, int id)
{
    if (g_nuser == (int)(sizeof g_user / sizeof *g_user)) {
        return false;
    }
    g_user[g_nuser].key = key;
    g_user[g_nuser++].id = id;
    return true;
}

static int button_for(SDL_Keycode key)
{
    for (size_t i = 0; i < sizeof g_binds / sizeof *g_binds; i++) {
        if (g_binds[i].key == key) {
            return panel_button_id(g_binds[i].btn);
        }
    }
    return -1;
}

static int user_for(SDL_Keycode key)
{
    for (int i = 0; i < g_nuser; i++) {
        if (g_user[i].key == key) {
            return g_user[i].id;
        }
    }
    return -1;
}

static int knob_for(SDL_Keycode key)
{
    for (size_t i = 0; i < sizeof g_knobs / sizeof *g_knobs; i++) {
        if (g_knobs[i].key == key) {
            return g_knobs[i].enc;
        }
    }
    return -1;
}

static int turn_for(SDL_Keycode key, bool *push)
{
    *push = key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET;
    return key == SDLK_MINUS || key == SDLK_LEFTBRACKET ? -1
         : key == SDLK_EQUALS || key == SDLK_RIGHTBRACKET ? 1 : 0;
}

static int held_find(SDL_Keycode key)
{
    for (int i = 0; i < g_nheld; i++) {
        if (g_held[i].key == key) {
            return i;
        }
    }
    return -1;
}

static void held_add(SDL_Keycode key, int kind, int id)
{
    if (g_nheld < (int)(sizeof g_held / sizeof *g_held)) {
        g_held[g_nheld].key = key;
        g_held[g_nheld].kind = kind;
        g_held[g_nheld++].id = id;
    }
}

static bool button_held(int id)
{
    for (int i = 0; i < g_nheld; i++) {
        if (g_held[i].kind == HELD_BUTTON && g_held[i].id == id) {
            return true;
        }
    }
    return id == FUNC_ID && g_func;
}

static void release(int id)
{
    if (!(g_latched >> id & 1)) {
        panel_key(id, false);
    }
}

static void release_latched(void)
{
    const uint64_t l = g_latched;

    g_latched = 0;
    for (int id = 0; id < 64; id++) {
        if (l >> id & 1 && !button_held(id)) {
            panel_key(id, false);
        }
    }
}

static void end_push_turn(void)
{
    if (g_push >= 0) {
        panel_key(g_push, false);
        g_push = -1;
    }
}

static void click(int id)
{
    if (g_tap >= 0) {
        panel_key(g_tap, false);
    }
    panel_key(id, true);
    g_tap = id;
    g_tap_up = now_ms() + TAP_MS;
}

/* Ctrl is FUNC. Every event carries the modifier state, so a lost Ctrl key-up
 * is caught by the next key. */
static void sync_func(bool ctrl, bool shift)
{
    if (ctrl && !g_func) {
        g_func = true;
        panel_key(FUNC_ID, true);
        if (shift) {
            g_latched |= 1ull << FUNC_ID;
        }
    } else if (!ctrl && g_func) {
        g_func = false;
        release(FUNC_ID);
    }
}

static void turn(int step, bool push)
{
    if (g_knob < 0) {
        return;
    }
    g_turned = true;
    if (g_knob == XFADER) {
        PanelState p;
        int pos;

        panel_snapshot(&p);             /* 255 is far LEFT: + moves right */
        pos = p.xfader - step * (push ? XF_BIG : XF_STEP);
        panel_xfader(pos < 0 ? 0 : pos > 255 ? 255 : pos);
        return;
    }
    if (push && g_push < 0) {
        g_push = ENC_PUSH + g_knob;
        panel_key(g_push, true);
    }
    panel_encoder(g_knob, step);
}

void keys_release_all(void)
{
    uint64_t down = g_latched;          /* each key let go once */

    for (int i = 0; i < g_nheld; i++) {
        if (g_held[i].kind == HELD_BUTTON) {
            down |= 1ull << g_held[i].id;
        }
    }
    if (g_func) {
        down |= 1ull << FUNC_ID;
    }
    g_nheld = 0;
    g_latched = 0;
    g_func = false;
    g_knob = -1;                        /* a knob let go this way is no click */
    end_push_turn();
    for (int id = 0; id < 64; id++) {
        if (down >> id & 1) {
            panel_key(id, false);
        }
    }
}

bool keys_key(int key, bool down, bool repeat, int mod)
{
    const bool ctrl_key = key == SDLK_LCTRL || key == SDLK_RCTRL;
    const bool shift_key = key == SDLK_LSHIFT || key == SDLK_RSHIFT;
    const bool shift = (mod & KMOD_SHIFT) != 0;
    int i, id, step;
    bool push;

    sync_func(ctrl_key ? down : (mod & KMOD_CTRL) != 0, shift);
    if (ctrl_key) {
        return true;
    }
    if (shift_key) {
        if (!down) {
            release_latched();
        }
        return true;
    }
    if (!down) {
        i = held_find(key);
        if (i < 0) {
            return false;
        }
        const int kind = g_held[i].kind;

        id = g_held[i].id;
        g_held[i] = g_held[--g_nheld];
        if (kind == HELD_BUTTON) {
            release(id);
        } else if (kind == HELD_TURN) {
            if (id) {
                end_push_turn();
            }
        } else if (id == g_knob) {
            if (!g_turned && g_knob < 7) {
                click(ENC_PUSH + g_knob);
            }
            end_push_turn();
            g_knob = -1;
        }
        return true;
    }

    if (key == SDLK_DELETE) {
        keys_release_all();
        return true;
    }
    id = user_for(key);                 /* --keymap wins over everything */
    step = id < 0 ? turn_for(key, &push) : 0;
    if (step) {                         /* the one key that repeats */
        if (held_find(key) < 0) {
            held_add(key, HELD_TURN, push);
        }
        turn(step, push);
        return true;
    }
    if (repeat || held_find(key) >= 0) {
        return held_find(key) >= 0;
    }
    if (id < 0 && (i = knob_for(key)) >= 0) {
        held_add(key, HELD_KNOB, i);
        end_push_turn();
        g_knob = i;
        g_turned = false;
        return true;
    }
    if (id < 0) {
        id = button_for(key);
    }
    if (id < 0) {
        return false;
    }
    held_add(key, HELD_BUTTON, id);
    if (shift) {
        g_latched |= 1ull << id;
    }
    panel_key(id, true);
    return true;
}

void keys_tick(void)
{
    if (g_tap >= 0 && now_ms() >= g_tap_up) {
        panel_key(g_tap, false);
        g_tap = -1;
    }
}
