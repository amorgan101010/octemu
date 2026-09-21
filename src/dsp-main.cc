/*
 * octdsp — the Octatrack's two DSP56721 cores end to end, no QEMU.
 * Boots both from the OS image the way the ColdFire does, then plays the
 * ColdFire's role in the per-block host protocol from first principles. The
 * chip itself lives in src/board/ot-dsp56k.{h,cc}, shared verbatim with the QEMU
 * shim, so this program is a real regression test for the code the emulator
 * runs — and it answers a DSP question in four seconds instead of two minutes.
 *
 *   ./octdsp --in-a sin:440 --out-main out.wav --timeout 2
 *
 * Inputs a-d are ESAI RX slots 0-3 (IN A-D); the audible path is the mixer's
 * DIR input, gated by the control block. MAIN is TX slots 1/2 (read-disasm);
 * CUE 3/4 and PHONES 5/6 are the remaining pairs, unvalidated — use
 * --out-slot N when in doubt.
 *
 * SPDX-License-Identifier: MIT
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "board/ot-dsp56k.h"
#include "wav.h"

using namespace dsp56k;
using ot::kFrames;
using ot::kIns;
using ot::kRate;
using ot::kSlots;
using ot::TWord;

namespace {

/* MAIN OS image geometry (OS 1.40C). ColdFire VA base 0x40000400. */
constexpr uint32_t kImgBase = 0x40000400;
struct Blob { uint32_t va, len, dspAddr; };
constexpr Blob kBootstrap[2] = {{0x400e21e0, 0x96, 0x31000},
                                {0x400e2276, 0xae, 0x32000}};
/* Core-1 payload length carries 0x500 bytes past the stock 0x12d05 stream so
 * the lab reads records SPLICED into the payload (a new load record relocates
 * the [0x3][0x38000] terminator into the free padding after payload B). The
 * loader stops at the terminator, so the extra words are inert for the stock
 * image (verified: stock boots identically). A real unit's ColdFire
 * loader is not length-bound and needs no such bump. */
constexpr Blob kPayload[2]   = {{0x400e2324, 0x136cb, 0},
                                {0x400f59ef, 0x13205, 0}};

/* Per-block host->DSP transfers, per core: dest, count-1 (16-bit port words). */
struct Arm { TWord dest, cnt; };
constexpr Arm kStream = {0x6080, 0x29f};   /* 4 x 336-B stream records */
constexpr Arm kCtrl   = {0x6800, 0x03f};   /* global control block     */
constexpr Arm kRec    = {0x6000, 0x07f};   /* 4 x 64-B track records   */
constexpr Arm kSum    = {0x6400, 0x1ff};   /* 8 tracks' post-FX sum    */
constexpr Arm kRead   = {0x6600, 0x2ff};   /* DSP->host readback (768) */

uint64_t g_budget = 0, g_spent = 0;
bool budgetLeft() { return !g_budget || g_spent < g_budget; }

/* ---- inputs and outputs ---------------------------------------------------
 * InGen, ot_in_next and the WAV codec are shared with the frontend; see
 * src/wav.h. */
void writeWav(const char *path, const std::vector<int32_t> *ch, unsigned nch)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "octdsp: cannot write %s\n", path);
        exit(2);
    }
    size_t frames = 0;
    for (unsigned c = 0; c < nch; c++)
        frames = std::max(frames, ch[c].size());
    uint8_t h[44];
    ot_wav_header(h, kRate, nch, (uint32_t)(frames * 2 * nch));
    fwrite(h, 1, sizeof h, f);
    for (size_t i = 0; i < frames; i++)
        for (unsigned c = 0; c < nch; c++) {
            const int16_t s = i < ch[c].size() ? (int16_t)(ch[c][i] >> 8) : 0;
            fputc(s & 0xFF, f);
            fputc((s >> 8) & 0xFF, f);
        }
    fclose(f);
}

double goertzel(const std::vector<int32_t> &x, double hz)
{
    if (x.size() < 1024)
        return 0;
    const size_t n = std::min(x.size(), (size_t)kRate);
    const size_t off = x.size() - n;
    const double k = round(hz * n / kRate), w = 2 * M_PI * k / n, c = 2 * cos(w);
    double s1 = 0, s2 = 0, total = 0;
    for (size_t i = 0; i < n; i++) {
        const double v = x[off + i] / 8388608.0;
        const double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
        total += v * v;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return total > 0 ? std::min(1.0, p / (total * n / 2)) : 0;
}

/* A detector that has never been seen to fail proves nothing: prove it on a
 * known tone, a DC level and a wrong frequency before trusting any zero. */
bool detectorSelfTest()
{
    std::vector<int32_t> t(kRate), d(kRate, 4000000);
    for (size_t i = 0; i < t.size(); i++)
        t[i] = (int32_t)(0.5 * sin(2 * M_PI * 440.0 * i / kRate) * 8388607.0);
    return goertzel(t, 440) > 0.9 && goertzel(d, 440) < 0.1 &&
           goertzel(t, 1000) < 0.1;
}

/* ---- the chip, and this program's half of the block protocol -------------- */
ot::Chip g_chip;
InGen g_in[kIns] = {};
uint32_t g_slotPeak[2][kSlots];
std::vector<int32_t> g_outSlot[2][kSlots];

struct Port;
Port *g_port[2];
void stepBoth(unsigned n);

/* The host end of one core's port. */
struct Port {
    ot::Core &c;
    size_t argOcc = 0;
    uint64_t wordsIn = 0, wordsOut = 0, commands = 0, dropped = 0;
    uint64_t icrResets = 0, icrTimeouts = 0, cmdFails = 0;

    explicit Port(ot::Core &core) : c(core) {}

    /* ☠ Every step of this core must DRAIN as it goes. The TX latch is depth
     * 1 and HTDE-gated, so a core with an unread published word stops dead —
     * and the module loader echoes every uploaded word. Stepping without
     * draining wedges the payload upload with a full RX FIFO. */
    uint64_t run(unsigned n)
    {
        if (!c.runnable() || !budgetLeft())
            return 0;
        const uint64_t before = c.dsp->getInstructionCounter();
        for (unsigned i = 0; i < n; i++) {
            if (!c.step(1))
                break;
            drain();
        }
        const uint64_t did = c.dsp->getInstructionCounter() - before;
        g_spent += did;
        return did;
    }
    ot::StepFn stepper() { return [this] { run(64); }; }

    /* The readback words, when we are collecting them. The guest's "sum" is a
     * verbatim echo of its own readback — measured on the Octatrack: the readback
     * DMA lands in 0x80003190..0x80003590 and the sum arm sends those same
     * buffers straight back — so the lab reproduces it by feeding what it
     * drained. Without this the lab renders no track at all: the mixer has a
     * post-FX sum of zeros to work with. */
    std::vector<TWord> rb;
    bool collecting = false;

    void drain()
    {
        while (c.hdi().hasTX()) {
            const TWord w = c.hdi().readTX();

            if (collecting) {
                rb.push_back(w & 0xffff);
            }
            wordsOut++;
        }
    }

    /* Boot words go to the ROM-bootstrap model; argument words are never run
     * over (a still-armed host-receive DMA channel would eat them). */
    void push(TWord w, bool isArg)
    {
        wordsIn++;
        if (!c.booted) {
            if (c.boot->hdiWriteTX(w))
                c.booted = true;
            return;
        }
        if (isArg) {
            c.hdi().writeRX(&w, 1);
            return;
        }
        for (unsigned g = 0;
             g < 4096 && c.hdi().rxData().size() >= ot::kFifoDepth; g++)
            if (run(128) == 0)
                break;
        if (c.hdi().rxData().size() >= ot::kFifoDepth) {
            dropped++;
            return;
        }
        c.hdi().writeRX(&w, 1);
    }

    /* Bulk, paced against the FIFO in chunks as TRDY does per word. */
    void pushBulk(const std::vector<TWord> &words)
    {
        size_t i = 0;
        for (unsigned guard = 0; i < words.size() && guard < 100000;) {
            const size_t occ = c.hdi().rxData().size();
            if (occ < ot::kFifoDepth) {
                const size_t n = std::min(words.size() - i,
                                          (size_t)ot::kFifoDepth - occ);
                c.hdi().writeRX(words.data() + i, n);
                i += n;
                wordsIn += n;
                continue;
            }
            guard++;
            if (run(512) == 0)
                break;
            stepBoth(64);          /* the mailbox wedges if only one runs */
        }
        dropped += words.size() - i;
    }

    void command(TWord cvr)
    {
        commands++;
        if (!c.runnable())
            return;
        if (!ot::portCommand(c, cvr, argOcc, stepper()))
            cmdFails++;
    }

    void icrReset()
    {
        icrResets++;
        if (!ot::portIcrReset(c, stepper()))
            icrTimeouts++;
    }

    void beginArgs()
    {
        argOcc = c.hdi().rxData().size();
        c.disarmHostRx();
    }

    void xfer(const Arm &a, const std::vector<TWord> &words)
    {
        icrReset();
        beginArgs();
        push(a.dest, true);
        push(a.cnt, true);
        command(0x08);
        pushBulk(words);
    }
};

/* ☠ The two cores must be interleaved FINELY. The payload hands core 1 its
 * per-block message through the shared window (x:$30000..$30047 — the
 * metronome click flag among it) and then publishes the mailbox word; core 1
 * reads that window three instructions after it takes the word, while core 0
 * overwrites the same window from its own snapshot later in the block. A slice
 * long enough to carry core 0 from the publish to that overwrite loses the
 * message every time: measured on the click flag, core 1 sees it at a slice of
 * 16 and never at 512. */
constexpr unsigned kSlice = 16;

bool g_sumEcho;
bool g_traceRb;
int g_traceX1 = -1;         /* core-1 X address to sample per render tick */
std::string g_dumpMem;

/* ---- replay of a captured emulator block stream --------------------------
 * The lab's own arms keep the cores alive but render no VOICE: the records
 * carry no sample, so everything downstream of a playing track — AMP, the
 * LFOs, both FX slots, the master track — is unreachable here. Replaying what
 * the emulator actually sent (octemu --capture-dsp) puts a real
 * voice in the lab, and --ctl/--rec still apply on top, which is what makes a
 * parameter sweep possible at all.
 *
 * Per core and per kind the payloads are replayed in the order they were
 * captured, wrapping when they run out. The sum is not replayed: the guest's
 * sum is an echo of its own readback, and --sum-echo reproduces that live. */
struct Capture {
    std::vector<std::vector<TWord>> arm[2][3];   /* [core][stream|rec|ctrl] */
    size_t next[2][3] = {};
    bool loaded = false;

    bool load(const char *path)
    {
        FILE *f = fopen(path, "r");
        char kind[16];
        unsigned core, n;

        if (!f) {
            fprintf(stderr, "octdsp: cannot read %s\n", path);
            return false;
        }
        while (fscanf(f, " A %u %15s %u", &core, kind, &n) == 3) {
            const int k = !strcmp(kind, "stream") ? 0
                        : !strcmp(kind, "rec") ? 1 : 2;
            std::vector<TWord> v(n);

            for (unsigned i = 0; i < n; i++) {
                unsigned w = 0;

                if (fscanf(f, " %x", &w) != 1)
                    break;
                v[i] = w & 0xffff;
            }
            if (core < 2)
                arm[core][k].push_back(std::move(v));
        }
        fclose(f);
        loaded = true;
        fprintf(stderr, "octdsp: replay: core0 %zu/%zu/%zu, "
                "core1 %zu/%zu/%zu (stream/rec/ctrl)\n",
                arm[0][0].size(), arm[0][1].size(), arm[0][2].size(),
                arm[1][0].size(), arm[1][1].size(), arm[1][2].size());
        return arm[0][0].size() > 0;
    }

    /* nullptr when that core never got this arm in the capture. */
    const std::vector<TWord> *take(unsigned core, int kind)
    {
        auto &v = arm[core][kind];

        if (v.empty())
            return nullptr;
        return &v[next[core][kind]++ % v.size()];
    }
};
Capture g_capture;

/* Both cores' data memory at exit. This is the mapping instrument: run once,
 * change ONE parameter, run again, and diff — what moved is where that
 * parameter lands inside the payload, which is otherwise invisible. Drive it
 * with a silent input so the audio buffers stay zero and only parameter state
 * differs. */
void dumpMemory(const std::string &prefix)
{
    for (unsigned i = 0; i < 2; i++) {
        const std::string path = prefix + ".core" + std::to_string(i);
        FILE *f = fopen(path.c_str(), "w");

        if (!f) {
            fprintf(stderr, "octdsp: cannot write %s\n", path.c_str());
            return;
        }
        /* P is dumped too: it is the PAYLOAD, so a diff of two runs shows
         * code as well as data. */
        for (EMemArea ar : {MemArea_X, MemArea_Y, MemArea_P}) {
            const char c = ar == MemArea_X ? 'X' : ar == MemArea_Y ? 'Y' : 'P';

            /* ☠ 0x8000 is NOT enough. Every reverb keeps its coefficient and
             * modulo tables above it — PLATE at X:$8040/$804a/$8050, SPRING's
             * decay at X:$8630/$86b0/$8730, DARK's moduli at X:$87bb..$87ca —
             * and no other effect reads that region. A dump that stops at
             * $8000 hides exactly the data the reverbs depend on. */
            for (TWord a = 0; a < 0x10000; a++)
                fprintf(f, "%c:%05x %06x\n", c, a,
                        g_chip.core[i].mem->get(ar, a));
            for (TWord a = 0x30000; a < 0x30100; a++)
                fprintf(f, "%c:%05x %06x\n", c, a,
                        g_chip.core[i].mem->get(ar, a));
        }
        fclose(f);
    }
}

/* The 512 words the guest would hand back this block: the front of what the
 * DSP published in the readback, which is what its own buffers hold. */
std::vector<TWord> echoSum(Port &p)
{
    std::vector<TWord> v(512, 0);

    for (size_t i = 0; i < v.size() && i < p.rb.size(); i++) {
        v[i] = p.rb[i];
    }
    return v;
}

void stepBoth(unsigned n)
{
    for (unsigned done = 0; done < n; done += kSlice) {
        const unsigned q = n - done < kSlice ? n - done : kSlice;

        for (auto *p : g_port)
            if (p)
                p->run(q);
    }
}

/* ---- boot ----------------------------------------------------------------- */
std::vector<uint8_t> g_img;

std::vector<uint8_t> slice(const Blob &b)
{
    const size_t off = b.va - kImgBase;
    if (off + b.len > g_img.size()) {
        fprintf(stderr, "octdsp: image too short\n");
        exit(2);
    }
    return {g_img.begin() + off, g_img.begin() + off + b.len};
}

TWord w24(const std::vector<uint8_t> &b, size_t i)
{
    return b[i] | b[i + 1] << 8 | (TWord)b[i + 2] << 16;
}

void bootCore(Port &p, unsigned i)
{
    const auto bs = slice(kBootstrap[i]);
    /* ROM bootstrap framing: length, address, then data words. */
    p.push(bs.size() / 3, false);
    p.push(kBootstrap[i].dspAddr, false);
    for (size_t o = 0; o + 3 <= bs.size(); o += 3)
        p.push(w24(bs, o), false);
    if (!p.c.booted) {
        fprintf(stderr, "octdsp: core %u bootstrap did not complete\n", i);
        exit(1);
    }
    /* The bootstrap is a module loader; the payload is its input stream
     * (records of space/addr/count/words, command 3 = jump) MINUS two 6-byte
     * headers the ColdFire's own parser consumes. */
    const auto pl = slice(kPayload[i]);
    size_t o = 0;
    if (w24(pl, 0) == 3) o += 6;
    if (w24(pl, o) == 4) o += 6;
    for (; o + 3 <= pl.size(); o += 3)
        p.push(w24(pl, o), false);
    stepBoth(100000);
    printf("core %u booted: pc=%#x\n", i, p.c.pc());
}

/* ---- per-block host content ----------------------------------------------- */
void put32(std::vector<TWord> &v, uint32_t x)
{
    v.push_back((x >> 16) & 0xFFFF);
    v.push_back(x & 0xFFFF);
}

std::vector<TWord> streamWords(uint64_t frame0)
{
    /* 4 x 168-word stream records: a two-descriptor chain, then the frames.
     * Inputs 1/2 ride as the stereo payload. */
    std::vector<TWord> v;
    for (unsigned t = 0; t < 4; t++) {
        const size_t rec0 = v.size();
        put32(v, 0);              /* desc0: the [0,split) call — empty */
        put32(v, 0);
        put32(v, 0x04000000);     /* rate 1.0, Q6.26                   */
        put32(v, 0);
        put32(v, kFrames);        /* desc1: the whole block            */
        put32(v, (uint32_t)frame0);
        put32(v, 0x04000000);
        put32(v, 0);
        for (unsigned f = 0; f < kFrames; f++) {
            put32(v, (uint32_t)(ot_in_next(&g_in[0], frame0 + f, kRate) << 8));
            put32(v, (uint32_t)(ot_in_next(&g_in[1], frame0 + f, kRate) << 8));
        }
        v.resize(rec0 + 168, 0);
    }
    return v;
}

/* Per-word overrides of the control block, applied after it is built.
 * The payload's control layout is only known by measurement — "which word does
 * it read the DIR gain from?" is a knockout sweep over these, four seconds an
 * arm, instead of a rebuild each time. */
TWord g_ctlSet[64];
bool g_ctlHas[64];

/* The same for the 32-word track record. Between them these two blocks are
 * every per-block parameter the guest sends, so overriding a word and watching
 * the eight TX slots is how the payload's parameter map gets built. */
TWord g_recSet[32];
bool g_recHas[32];

/* --rec1: the same, applied to CORE 1's tracks only. An FX id override sent
 * to core 0 kills it — its payload is not the FX core and the id word means
 * something else there — so per-track FX probes need this form. */
TWord g_rec1Set[32];
bool g_rec1Has[32];

/* The metronome, which is core 1's: core 0 copies control words 0x30-0x33 into
 * the shared window at x:$30044, and core 1 tests bit 4 of the first of them
 * (P:0xb2), takes the frame offset from its low nibble, the pitch from the
 * next word, bit 5 as the accent and bit 6 as TONAL, and renders a click with
 * an exponential envelope. --metro N pulses that bit every N blocks. */
unsigned g_metroEvery, g_metroPitch = 12, g_metroCue = 0x7f, g_metroMain = 0x7f;
TWord g_metroIcc, g_metroClk, g_metroEnv;
bool g_probeMpyi, g_probeLimit, g_probeModulo;
const char *g_replayWireFn;
bool g_trace;
uint64_t g_metroLive;

/* --ctl and --rec on top of a replayed arm, so a captured block can be
 * perturbed one word at a time. --rec applies to every track in the arm. */
std::vector<TWord> withCtlOverrides(std::vector<TWord> v)
{
    for (unsigned i = 0; i < 64 && i < v.size(); i++) {
        if (g_ctlHas[i])
            v[i] = g_ctlSet[i];
    }
    return v;
}

std::vector<TWord> withRecOverrides(std::vector<TWord> v, unsigned core = 2)
{
    for (size_t t = 0; t + 32 <= v.size(); t += 32) {
        for (unsigned i = 0; i < 32; i++) {
            if (g_recHas[i])
                v[t + i] = g_recSet[i];
            /* --rec1 targets core 1's records specifically — the voice core,
             * which is what a per-track (AMP/LFO/FX) sweep perturbs. It was
             * only wired into the synthetic recordWords(); apply it on the
             * replay path too. */
            if (core == 1 && g_rec1Has[i])
                v[t + i] = g_rec1Set[i];
        }
    }
    return v;
}

/* The 64-word global control block: 10 slots x 4 words (cue, level, pan,
 * smoother nibble), master scalars at +0x28/+0x29, and the input
 * conditioner's fields in the tail.
 *
 * Which words are load-bearing is measured, not assumed — knocked out one at a
 * time with `--ctl I=0` and a sine on IN A. For that path (slot 9, the DIR A/B
 * pair) the passthrough dies on +0x24, +0x27 and +0x28, and drops a tenth on
 * +0x34; the other sixty words change nothing. The conditioner at P:0x1cb
 * multiplies the ESAI receive ring IN PLACE by a gain ramped toward +0x27, so
 * ZERO there actively ERASES the input, and +0x34/+0x35 are its headroom-LUT
 * indices (X:0x7400) — zero picks a shift that silences it. Never sent to
 * core 1. */
std::vector<TWord> ctrlWords(unsigned core)
{
    std::vector<TWord> v(64, 0);
    for (unsigned s = 0; s < 10; s++) {
        v[s * 4 + 0] = 0x6c00;
        v[s * 4 + 1] = 0x6c00;
        v[s * 4 + 2] = 0x4000;
    }
    v[0x23] = 0x007f;
    v[0x27] = 0x007f;
    v[0x28] = 0x0040;
    v[0x29] = 0x0040;
    if (core == 0) {
        v[0x34] = 0x5a5a;
        v[0x35] = 0x5a5a;
    }
    for (unsigned i = 0; i < 64; i++) {
        if (g_ctlHas[i])
            v[i] = g_ctlSet[i];
    }
    return v;
}

/* 4 x 32-word track records. AMP: no attack, infinite hold, full VOL, centre
 * BAL; FX id 8 is the null passthrough stub. Flags word 30 needs bits 9+10
 * (the MASTER combination) or the track render dispatches into unloaded voice
 * state and retires the core. */
std::vector<TWord> recordWords(unsigned core)
{
    std::vector<TWord> v;
    for (unsigned t = 0; t < 4; t++) {
        TWord r[32] = {0};
        r[1] = r[2] = r[3] = r[5] = 0x7f00;
        r[4] = 0x4000;
        r[27] = r[28] = 0x0008;
        r[30] = 0x0640;
        for (unsigned i = 0; i < 32; i++) {
            if (g_recHas[i])
                r[i] = g_recSet[i];
            if (core == 1 && g_rec1Has[i])
                r[i] = g_rec1Set[i];
        }
        v.insert(v.end(), r, r + 32);
    }
    return v;
}

/* ---- CLI ------------------------------------------------------------------ */
const char *kUsage =
    "usage: octdsp [--in-a SPEC]..[--in-d SPEC] [--out-main X.wav]\n"
    "                     [--out-cue X.wav] [--out-phones X.wav]\n"
    "                     [--out-slot N X.wav] [--timeout SECS]\n"
    "                     [--expect-tone HZ] [--os PATH]\n"
    "\n"
    "Runs the Octatrack's two DSP56721 cores alone — no QEMU — booting them\n"
    "from the OS image and driving the host-port block protocol itself. The same\n"
    "cores the emulator runs, and a DSP answer in four seconds.\n"
    "\n"
    "Common:\n"
    "  --in-a..d S   what is on inputs IN A-D: sin:HZ, cos:HZ, loop:F.wav,\n"
    "                one:F.wav, silence\n"
    "  --out-main F  stereo WAV of the MAIN pair (ESAI TX slots 1/2)\n"
    "  --out-cue F   stereo WAV of TX slots 3/4 (assumed CUE, unvalidated)\n"
    "  --out-phones F  stereo WAV of TX slots 5/6 (assumed PHONES, unvalidated)\n"
    "  --out-slot N F  mono WAV of raw TX slot N (0-7)\n"
    "  --timeout S   how many seconds of audio to render (default 2)\n"
    "  --expect-tone HZ  assert HZ is present on some slot (Goertzel >= 0.5) and\n"
    "                exit nonzero if it is not: this is what the test gates read\n"
    "  --os PATH     boot the cores from this MAIN OS image (default\n"
    "                out/os/main.bin)\n"
    "  --help, -h    this text\n"
    "\n"
    "Getting it to render a voice:\n"
    "  --sum-echo    feed the DSP's readback back as the next block's sum, as\n"
    "                the guest does — without it no track renders here\n"
    "  --replay F    replay an emulator capture (octemu\n"
    "                --capture-dsp F) so a real voice renders here; --ctl and\n"
    "                --rec still apply on top\n"
    "  --replay-wire F  replay a full .wire transcript (--capture-dsp F.wire)\n"
    "                verbatim in captured order; renders audio but not yet a\n"
    "                clean voice (block-alignment WIP)\n"
    "  --metro N     pulse the metronome click bit every N blocks (control word\n"
    "                0x30); core 1 renders the click. Reports what it made\n"
    "  --metro-pitch V, --metro-cue V, --metro-main V\n"
    "                the click's pitch and its two output volumes\n"
    "\n"
    "Measuring what a payload word does:\n"
    "  --ctl I=V     override control word I (0-63) with V; repeatable. The\n"
    "                DSP-side control layout is only known by measurement, and\n"
    "                this is how it gets measured: knock one word out, see what\n"
    "                the output loses\n"
    "  --rec I=V     override track-record word I (0-31) on every track;\n"
    "                repeatable. The record is the per-track parameter block\n"
    "  --rec1 I=V    the same, core 1's tracks only. FX id overrides MUST use\n"
    "                this form: core 0's payload is not the FX core and an id\n"
    "                in its record kills it\n"
    "  --dump-mem P  write both cores' X and Y memory to P.core0/P.core1 at\n"
    "                exit; diff two runs that differ in one parameter to see\n"
    "                where that parameter lands in the payload\n"
    "  --trace-rb    one line per render tick with the peak of each core's\n"
    "                readback since the last, so an AMP envelope or an LFO\n"
    "                reads as a block-by-block amplitude curve\n"
    "  --trace-x1 A  also print core 1's X:A on every --trace-rb line, to watch\n"
    "                one payload word move (0x-prefix for hex)\n"
    "\n"
    "Is the JIT wrong? (each of these answers one question and exits):\n"
    "  --probe-mpyi  MPYI with a negative immediate, on its own\n"
    "  --probe-limit the DATA LIMITER on an accumulator read, on its own\n"
    "  --probe-modulo AGU MODULO addressing, JIT vs interpreter vs the rule\n"
    "  --trace       print the setup loads of --probe-modulo: what each of\n"
    "                the two engines actually reads\n";

InGen parseSpec(const char *s)
{
    InGen g = {};
    if (!strncmp(s, "sin:", 4))       { g.kind = IN_SIN; g.hz = atof(s + 4); }
    else if (!strncmp(s, "cos:", 4))  { g.kind = IN_COS; g.hz = atof(s + 4); }
    else if (!strncmp(s, "loop:", 5) || !strncmp(s, "one:", 4)) {
        g.kind = s[0] == 'l' ? IN_LOOP : IN_ONE;
        g.wav = ot_wav_read(strchr(s, ':') + 1, &g.n);
        if (!g.wav) {
            fprintf(stderr, "octdsp: cannot read %s "
                    "(want 16- or 24-bit PCM)\n", strchr(s, ':') + 1);
            exit(2);
        }
    }
    else if (strcmp(s, "silence")) {
        fprintf(stderr, "octdsp: bad input spec %s\n", s);
        exit(2);
    }
    return g;
}

} // namespace

int main(int argc, char **argv)
{
    std::string outSlot[kSlots], outMain, outCue, outPhones;
    std::string os = "out/os/main.bin";
    double timeout = 2.0, expectTone = 0;

    for (int i = 1; i < argc; i++) {
        const std::string s = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "octdsp: %s needs a value\n", s.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (s == "--help" || s == "-h") { printf("%s", kUsage); return 0; }
        else if (s.rfind("--in-", 0) == 0 && s.size() == 6 &&
                 s[5] >= 'a' && s[5] <= 'd')
            g_in[s[5] - 'a'] = parseSpec(val());
        else if (s == "--out-main")   outMain = val();
        else if (s == "--out-cue")    outCue = val();
        else if (s == "--out-phones") outPhones = val();
        else if (s == "--out-slot") {
            const int n = atoi(val());
            if (n < 0 || n >= (int)kSlots) {
                fprintf(stderr, "octdsp: slot out of range\n");
                return 2;
            }
            outSlot[n] = val();
        }
        else if (s == "--rec") {
            const char *a = val();
            const char *eq = strchr(a, '=');
            const long idx = strtol(a, nullptr, 0);

            if (!eq || idx < 0 || idx > 31) {
                fprintf(stderr, "octdsp: --rec wants I=V, I in 0-31\n");
                return 2;
            }
            g_recSet[idx] = (TWord)strtoul(eq + 1, nullptr, 0) & 0xffffff;
            g_recHas[idx] = true;
        }
        else if (s == "--rec1") {
            const char *a = val();
            const char *eq = strchr(a, '=');
            const long idx = strtol(a, nullptr, 0);

            if (!eq || idx < 0 || idx > 31) {
                fprintf(stderr, "octdsp: --rec1 wants I=V, I in 0-31\n");
                return 2;
            }
            g_rec1Set[idx] = (TWord)strtoul(eq + 1, nullptr, 0) & 0xffffff;
            g_rec1Has[idx] = true;
        }
        else if (s == "--ctl") {
            const char *a = val();
            const char *eq = strchr(a, '=');
            const long idx = strtol(a, nullptr, 0);

            if (!eq || idx < 0 || idx > 63) {
                fprintf(stderr, "octdsp: --ctl wants I=V, I in 0-63\n");
                return 2;
            }
            g_ctlSet[idx] = (TWord)strtoul(eq + 1, nullptr, 0) & 0xffffff;
            g_ctlHas[idx] = true;
        }
        else if (s == "--sum-echo")     g_sumEcho = true;
        else if (s == "--trace-rb")     g_traceRb = true;
        else if (s == "--trace-x1")     g_traceX1 = (int)strtoul(val(), nullptr, 0);
        else if (s == "--replay") {
            if (!g_capture.load(val()))
                return 2;
        }
        else if (s == "--replay-wire") g_replayWireFn = val();
        else if (s == "--dump-mem")     g_dumpMem = val();
        else if (s == "--metro")        g_metroEvery = (unsigned)atoi(val());
        else if (s == "--metro-pitch")  g_metroPitch = (unsigned)strtoul(val(), 0, 0);
        else if (s == "--probe-mpyi")   g_probeMpyi = true;
        else if (s == "--probe-limit")  g_probeLimit = true;
        else if (s == "--probe-modulo") g_probeModulo = true;
        else if (s == "--trace")        g_trace = true;
        else if (s == "--metro-cue")    g_metroCue = (unsigned)strtoul(val(), 0, 0);
        else if (s == "--metro-main")   g_metroMain = (unsigned)strtoul(val(), 0, 0);
        else if (s == "--timeout")      timeout = atof(val());
        else if (s == "--expect-tone")  expectTone = atof(val());
        else if (s == "--os")           os = val();
        else {
            fprintf(stderr, "octdsp: unknown flag %s\n%s", s.c_str(), kUsage);
            return 2;
        }
    }

    if (!detectorSelfTest()) {
        fprintf(stderr, "octdsp: tone detector self-test failed\n");
        return 3;
    }

    FILE *f = fopen(os.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "octdsp: cannot read %s (run 'make os')\n",
                os.c_str());
        return 2;
    }
    fseek(f, 0, SEEK_END);
    g_img.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(g_img.data(), 1, g_img.size(), f) != g_img.size()) {
        fprintf(stderr, "octdsp: short image read\n");
        return 2;
    }
    fclose(f);

    g_budget = (uint64_t)(timeout * 1.6e9) + 2000000000ull;

    if (!g_chip.open()) {
        fprintf(stderr, "octdsp: shm failed\n");
        return 1;
    }
    for (unsigned i = 0; i < 2; i++) {
        g_chip.core[i].init(i,
            [i](int32_t *in) {
                const uint64_t fr = g_chip.core[i].rxFrames;
                for (unsigned s = 0; s < kIns; s++)
                    in[s] = ot_in_next(&g_in[s], fr, kRate);
            },
            [i](const int32_t *slots) {
                for (unsigned s = 0; s < kSlots; s++) {
                    const uint32_t a = (uint32_t)std::abs(slots[s]);
                    if (a > g_slotPeak[i][s])
                        g_slotPeak[i][s] = a;
                    if (g_outSlot[i][s].size() < (16u << 20))
                        g_outSlot[i][s].push_back(slots[s]);
                }
            });
    }

    /* Aliasing self-check: one write must be visible in all six views. */
    g_chip.core[0].mem->set(MemArea_X, 0x30040, 0xA5A5A5);
    for (unsigned i = 0; i < 2; i++)
        for (EMemArea ar : {MemArea_P, MemArea_X, MemArea_Y})
            if (g_chip.core[i].mem->get(ar, 0x30040) != 0xA5A5A5) {
                fprintf(stderr, "octdsp: shared window is not aliased\n");
                return 1;
            }
    g_chip.core[0].mem->set(MemArea_X, 0x30040, 0);

    Port port[2] = {Port(g_chip.core[0]), Port(g_chip.core[1])};
    g_port[0] = &port[0];
    g_port[1] = &port[1];
    bootCore(port[0], 0);
    bootCore(port[1], 1);
    port[0].push(0, false);      /* release P:0x3000e's HRDF wait -> P:0x40 */
    stepBoth(200000);

    const uint64_t targetFrames = (uint64_t)(timeout * kRate);
    std::vector<TWord> ctrl[2] = {ctrlWords(0), ctrlWords(1)};
    if (g_probeMpyi) {
        /* MPYI with a negative 24-bit immediate, on its own: the operand is
         * a SIGNED fraction, and the JIT used to hand it to the multiply raw.
         * The metronome's phase increment is one such multiply — see
         * patches/dsp56300/0010. */
        ot::Core &c = g_chip.core[1];
        auto &r = c.dsp->regs();
        const TWord base = 0x200;
        const TWord prog[] = { 0x0141e8, 0xfcf669, 0x0c0000 + base + 2 };
        const struct { TWord x1; const char *what; } cases[] = {
            { 0xbfe9ce, "the table value the metronome reads (-0.5007)" },
            { 0x400000, "+0.5" },
            { 0xc00000, "-0.5" },
            { 0x7fffff, "+1.0" },
        };

        for (unsigned i = 0; i < sizeof prog / sizeof *prog; i++)
            c.mem->set(MemArea_P, base + i, prog[i]);
        c.dsp->getJit().destroyAllBlocks();
        for (const auto &t : cases) {
            r.x.var = ((uint64_t)t.x1 << 24);           /* x1 = t.x1 */
            r.b.var = 0;
            c.dsp->setPC(base);
            c.step(1);
            printf("mpyi #$fcf669 x $%06x -> b %014llx  (%s)\n", t.x1,
                   (unsigned long long)(r.b.var & 0xffffffffffffffull), t.what);
        }
        return 0;
    }
    if (g_probeLimit) {
        /*
         * The DSP56300 DATA LIMITER, on its own.
         *
         * Reading A or B as a 24-bit operand does NOT truncate: when the
         * extension byte a2 is not a sign extension of a1's MSB the accumulator
         * is out of 24-bit range, and the hardware substitutes the nearest
         * limit, $7fffff or $800000. Truncating instead turns a value just
         * under 2.0 into $fffffe, which reads as -2 — a large positive gain
         * becomes a negative one two LSBs from zero.
         *
         * That is exactly what PLATE REV's wet/dry crossfade computes at
         * MIX=127: the mixer at P:$1828 does `add y1,a` with both operands at
         * $7fffff, i.e. 1.0 + 1.0, and hands the result to `move a,y1`.
         */
        ot::Core &c = g_chip.core[1];
        auto &r = c.dsp->regs();
        const TWord base = 0x200;
        /* move a,y1 ; move b,y0 ; then halt */
        const TWord prog[] = { 0x21c700, 0x21e600, 0x0c0000 + base + 2 };
        const struct { uint64_t a; TWord want; const char *what; } cases[] = {
            { 0x01fffffe000000ull, 0x7fffff, "1.9999998 (PLATE's wet gain at MIX=127)" },
            { 0x01000000000000ull, 0x7fffff, "exactly 2.0" },
            { 0x007fffff000000ull, 0x7fffff, "0.9999999 — in range, must pass through" },
            { 0x00400000000000ull, 0x400000, "0.5 — in range" },
            { 0xff000000000000ull, 0x800000, "-2.0" },
        };
        for (unsigned i = 0; i < sizeof prog / sizeof *prog; i++)
            c.mem->set(MemArea_P, base + i, prog[i]);
        c.dsp->getJit().destroyAllBlocks();
        int bad = 0;
        /* Also exercise SCALING MODE. SR bit 10 (S0) selects scale-down and
         * bit 11 (S1) scale-up on data read from A/B; SPRING REV sets S1 around
         * its processing (bset #$b,sr at P:0x1201, cleared at P:0x123c), and a
         * scaling mode that is stored but not applied is a factor-of-two gain
         * error — which is what a runaway feedback looks like. */
        const struct { TWord bit; const char *name; int shift; } modes[] = {
            { 0, "no scaling", 0 }, { 10, "S0 scale-down", -1 }, { 11, "S1 scale-up", +1 },
        };
        for (const auto &md : modes) {
            const uint64_t a0 = 0x00200000000000ull;   /* 0.25, well in range */
            r.a.var = a0; r.b.var = a0; r.y.var = 0;
            r.sr.var = md.bit ? (0x300 | (1u << md.bit)) : 0x300;
            c.dsp->getJit().destroyAllBlocks();
            c.dsp->setPC(base);
            c.step(1);
            const TWord y1 = (TWord)((r.y.var >> 24) & 0xffffff);
            const TWord want = md.shift == 0 ? 0x200000
                             : md.shift < 0  ? 0x100000 : 0x400000;
            printf("a=0.25  %-14s move a,y1 -> $%06x  want $%06x  %s\n",
                   md.name, y1, want, y1 == want ? "ok" : "SCALING NOT APPLIED");
        }
        r.sr.var = 0x300;
        for (const auto &t : cases) {
            r.a.var = t.a;
            r.b.var = t.a;
            r.y.var = 0;
            c.dsp->setPC(base);
            c.step(1);
            const TWord y1 = (TWord)((r.y.var >> 24) & 0xffffff);
            const TWord y0 = (TWord)(r.y.var & 0xffffff);
            const bool ok = y1 == t.want && y0 == t.want;
            bad += !ok;
            printf("a=%014llx  move a,y1 -> $%06x   move b,y0 -> $%06x   "
                   "want $%06x  %s  (%s)\n",
                   (unsigned long long)t.a, y1, y0, t.want,
                   ok ? "ok" : "WRONG", t.what);
        }
        printf("%s\n", bad ? "LIMITER IS NOT APPLIED — accumulator reads truncate"
                           : "limiter ok on every case");
        return bad ? 1 : 0;
    }
    if (g_probeModulo) {
        /*
         * AGU MODULO addressing, on its own.
         *
         * The three broken reverbs each run SEVERAL delay lines and reload M4
         * from memory per line inside one block; every effect that works runs
         * exactly ONE line and never changes M mid-block. And the library has
         * TWO implementations of the M-register rule — a host-side one for
         * `move #imm,Mn` (jitdspregs.cpp:99) and a generated-code one for
         * `move x:(..),Mn` — so they can disagree with each other and with the
         * hardware.
         *
         * The rule (DSP56300 family manual): for M = mod-1 with mod <= 0x8000,
         * (Rn)+ walks a circular buffer whose base is aligned to the next
         * power of two >= mod, and wraps after exactly `mod` steps.
         *
         * PLATE's own line lengths are used as the moduli.
         */
        ot::Core &c = g_chip.core[1];
        auto &r = c.dsp->regs();
        const TWord base = 0x200, buf = 0x4000;
        const TWord mods[] = { 0xa77, 0x99f, 0x724, 0x665, 0x800, 0x400 };
        int bad = 0;
        printf("modulus  steps   expected      JIT   interp\n");
        for (TWord mod : mods) {
            for (int viaMem = 0; viaMem < 2; viaMem++) {
                std::vector<TWord> prog;
                if (viaMem) {           /* move x:(r0),m4 — the runtime path */
                    c.mem->set(MemArea_X, 0x100, mod - 1);
                    prog.push_back(0x63f400); prog.push_back(0x000100); /* #$100,r3 */
                    prog.push_back(0x0a73e4); prog.push_back(0x000000); /* x:(r3+0),m4
                       — the exact encoding PLATE uses at P:0xe7e */
                } else {                /* move #imm,m4 — the host-side path */
                    prog.push_back(0x05f424); prog.push_back(mod - 1);
                }
                prog.push_back(0x64f400); prog.push_back(buf);           /* #buf,r4 */
                const TWord steps = mod + 3;
                for (TWord i = 0; i < steps; i++)
                    prog.push_back(0x44dc00);                            /* x:(r4)+,x0 */
                const TWord halt = base + (TWord)prog.size();
                prog.push_back(0x0c0000 + halt);
                for (size_t i = 0; i < prog.size(); i++)
                    c.mem->set(MemArea_P, base + (TWord)i, prog[i]);

                if (g_trace) {   /* setup only: what do the two engines load? */
                    const TWord halt2 = base + (viaMem ? 6 : 4);
                    for (size_t i = 0; i < prog.size(); i++)
                        c.mem->set(MemArea_P, base + (TWord)i, prog[i]);
                    c.mem->set(MemArea_P, halt2, 0x0c0000 + halt2);
                    for (int pass = 0; pass < 2; pass++) {
                        c.dsp->getJit().destroyAllBlocks();
                        r.r[4].var = 0; r.m[4].var = 0xffffff; r.r[3].var = 0;
                        c.dsp->setPC(base);
                        for (unsigned n = 0; n < 100; n++) {
                            if (c.dsp->getPC().toWord() == halt2) break;
                            if (pass == 0) c.dsp->execJit(); else c.dsp->execInterpreter();
                        }
                        printf("   %s %s after setup: r3=%06x r4=%06x m4=%06x pc=%04x\n",
                               viaMem ? "mem" : "imm", pass ? "INT" : "JIT",
                               r.r[3].var & 0xffffff, r.r[4].var & 0xffffff,
                               r.m[4].var & 0xffffff, c.dsp->getPC().toWord());
                    }
                    continue;
                }
                TWord got[2];
                for (int pass = 0; pass < 2; pass++) {
                    c.dsp->getJit().destroyAllBlocks();
                    r.r[4].var = 0; r.m[4].var = 0xffffff;
                    c.dsp->setPC(base);
                    unsigned n = 0;
                    for (; n < 200000; n++) {
                        if (c.dsp->getPC().toWord() == halt) break;
                        if (pass == 0) c.dsp->execJit(); else c.dsp->execInterpreter();
                    }
                    got[pass] = (n >= 200000) ? 0xdead : (r.r[4].var & 0xffffff);
                }
                const TWord want = buf + (steps % mod);
                const bool ok = got[0] == want && got[1] == want;
                bad += !ok;
                printf("%#06x %s %5u  %#08x  %#08x %#08x  %s\n", mod,
                       viaMem ? "mem" : "imm", steps, want, got[0], got[1],
                       ok ? "ok" : (got[0] != got[1] ? "JIT != INTERPRETER"
                                                     : "BOTH WRONG"));
            }
        }
        printf("%s\n", bad ? "MODULO ADDRESSING IS WRONG"
                           : "modulo addressing correct on every case");
        return bad ? 1 : 0;
    }
    const std::vector<TWord> recs0 = recordWords(0), recs1 = recordWords(1);
    /* The tracks' post-FX sum stays zero: with MASTER-mode records the
     * raw-copied stream (headers included) would feed back and saturate. */
    const std::vector<TWord> sum(512, 0);
    uint64_t frame0 = 0, blocks = 0;
    uint64_t lastRender = ot::g_icc.renders;

    /* ---- .wire transcript replay ------------------------------------------
     * Replay the exact per-block port traffic the EMULATOR sent
     * (octemu --capture-dsp F.wire), verbatim and in captured
     * order, against the already-booted lab cores. Unlike --replay (which
     * reconstructs arm-classified blocks and loses the evolving per-track
     * setup, so no voice renders), this feeds every T arg word, C command and
     * M bulk minor as-is — the samples ride core 1's stream, the params ride
     * the records — so a real voice appears in the lab. --rec/--ctl still apply
     * on top (withRecOverrides/withCtlOverrides) for parameter sweeps.
     *
     * The wire's own boot upload is SKIPPED: the lab already booted the cores
     * from the image, so replay begins at the first steady-state bulk (the
     * first M record) and picks up cleanly at the next transaction boundary. */
    if (g_replayWireFn) {
        FILE *wf = fopen(g_replayWireFn, "r");
        if (!wf) {
            fprintf(stderr, "octdsp: cannot read %s\n", g_replayWireFn);
            return 2;
        }
        for (auto &p : port)
            p.collecting = true;         /* keep the readback sum alive */

        char line[4096];
        bool started = false;            /* passed the boot upload yet? */
        bool sawM = false;               /* seen the first bulk?        */
        std::vector<TWord> pend[2];      /* pending arg words per core  */
        unsigned txns = 0;
        size_t rbMark[2] = {0, 0};       /* --trace-rb: rb high-water   */

        while (fgets(line, sizeof line, wf) &&
               g_chip.core[0].txFrames < targetFrames && budgetLeft() &&
               !g_chip.core[0].dead) {
            if (line[0] == 'M') {
                unsigned core, nbytes;
                uint32_t saddr;
                char *q = line + 2;
                core = strtoul(q, &q, 16);
                saddr = strtoul(q, &q, 16);
                nbytes = strtoul(q, &q, 10);
                (void)saddr;
                sawM = true;
                if (!started || core > 1)
                    continue;
                std::vector<TWord> w;
                w.reserve(nbytes / 2);
                for (unsigned i = 0; i < nbytes / 2; i++)
                    w.push_back((TWord)strtoul(q, &q, 16) & 0xffff);
                /* the record arm is where --rec overrides bite; classify by
                 * size the way the guest does (64B = records) */
                /* ☠ Do NOT step between the minors of one arm. A stream or sum
                 * arm is several consecutive M records delivered as ONE
                 * contiguous DMA burst under one command; stepping the DSP
                 * between them lets it consume a partial arm and desync. Only
                 * step after a command completes (below). */
                if (nbytes == 64)
                    port[core].pushBulk(withRecOverrides(w, core));
                else if (nbytes == 32)
                    port[core].pushBulk(withCtlOverrides(w));
                else
                    port[core].pushBulk(w);
            } else if (line[0] == 'C') {
                unsigned core = strtoul(line + 2, nullptr, 16);
                unsigned cmd = strtoul(line + 4, nullptr, 16);
                if (core > 1)
                    continue;
                /* start replaying only once the guest reached steady state:
                 * the first bulk marks it, then begin at the next fresh
                 * transaction (a command after we have seen an M). */
                if (!started) {
                    if (sawM)
                        started = true;
                    pend[core].clear();
                    continue;
                }
                port[core].icrReset();
                port[core].beginArgs();
                for (TWord a : pend[core])
                    port[core].push(a, true);
                pend[core].clear();
                port[core].command(cmd);
                /* Fine interleave after each armed transaction: the payload
                 * hands core 1 its per-block message through the shared window
                 * and a coarse slice loses it (kSlice reason, see stepBoth). */
                stepBoth(kSlice);
                ++txns;
                /* --trace-rb: one line per render tick with the peak of each
                 * core's readback words since the previous tick. Core 1's
                 * readback is its per-block rendered output (the buffers the
                 * ColdFire DMAs into 0x80003190 on silicon), so a time-domain
                 * modulation — the AMP envelope, an LFO — appears here as a
                 * block-by-block amplitude curve. */
                if (g_traceRb && ot::g_icc.renders != lastRender) {
                    lastRender = ot::g_icc.renders;
                    unsigned pk[2] = {0, 0}, nw[2];
                    for (unsigned c = 0; c < 2; c++) {
                        nw[c] = (unsigned)(port[c].rb.size() - rbMark[c]);
                        for (size_t i = rbMark[c]; i < port[c].rb.size(); i++) {
                            int v = (int16_t)port[c].rb[i];
                            if (v < 0)
                                v = -v;
                            if ((unsigned)v > pk[c])
                                pk[c] = (unsigned)v;
                        }
                        rbMark[c] = port[c].rb.size();
                    }
                    if (g_traceX1 >= 0) {
                        const TWord xv = g_chip.core[1].mem->get(
                            MemArea_X, (TWord)g_traceX1) & 0xffffff;
                        printf("RB r=%llu c0+%u %04x c1+%u %04x x1=%06x\n",
                               (unsigned long long)lastRender,
                               nw[0], pk[0], nw[1], pk[1], xv);
                    } else {
                        printf("RB r=%llu c0+%u %04x c1+%u %04x\n",
                               (unsigned long long)lastRender,
                               nw[0], pk[0], nw[1], pk[1]);
                    }
                }
            } else if (line[0] == 'T') {
                unsigned core = strtoul(line + 2, nullptr, 16);
                TWord w = (TWord)strtoul(line + 4, nullptr, 16);
                if (core <= 1 && started)
                    pend[core].push_back(w);
            }
        }
        fclose(wf);
        for (auto &p : port)
            p.collecting = g_sumEcho;
        goto replay_done;
    }

    while (budgetLeft() && g_chip.core[0].txFrames < targetFrames &&
           !g_chip.core[0].dead) {
        /* Core 0's per-block bank-index publish is the block clock. */
        for (unsigned g = 0;
             ot::g_icc.renders == lastRender && budgetLeft() && g < 100000; g++)
            stepBoth(512);
        if (ot::g_icc.renders == lastRender)
            break;
        lastRender = ot::g_icc.renders;

        if (g_metroEvery) {
            const bool tick = (blocks % g_metroEvery) == 1;

            for (unsigned c = 0; c < 2; c++) {
                ctrl[c][0x30] = (1u << 6) | (tick ? 0x10u : 0u);
                ctrl[c][0x31] = g_metroPitch;
                ctrl[c][0x32] = g_metroCue;
                ctrl[c][0x33] = g_metroMain;
            }
        }
        const auto stream = streamWords(frame0);
        auto deliver = [&](unsigned i) {
            Port &p = port[i];
            if (p.c.dead)
                return;
            if (g_capture.loaded) {
                /* ☠ The two cores do NOT get the same arms. Captured from the
                 * emulator: core 0 gets stream, records, control and sum; core 1
                 * gets stream and records only — its control data arrives
                 * through the shared window, copied by core 0. Sending core 1
                 * the extra arms desyncs its command handling and it renders
                 * nothing. */
                const std::vector<TWord> *cs = g_capture.take(i, 0);
                const std::vector<TWord> *cr = g_capture.take(i, 1);
                const std::vector<TWord> *cc = g_capture.take(i, 2);

                if (cs)
                    p.xfer(kStream, *cs);
                if (cc)
                    p.xfer(kCtrl, withCtlOverrides(*cc));
                if (cr)
                    p.xfer(kRec, withRecOverrides(*cr, i));
                if (cc)                       /* the sum rides with core 0 */
                    p.xfer(kSum, g_sumEcho ? echoSum(p) : sum);
                if (cc) {
                    /* And core 1 never gets 0x8c either — captured from the
                     * emulator, its whole per-block command set is 0x88 and
                     * 0x89. */
                    p.command(0x0C);
                }
            } else {
                p.xfer(kStream, stream);
                p.xfer(kCtrl, ctrl[i]);
                p.xfer(kRec, i ? recs1 : recs0);
                p.xfer(kSum, g_sumEcho ? echoSum(p) : sum);
            }
            if (!g_capture.loaded)
                p.command(0x0C);          /* bypass off; no ICR, no args */
            p.icrReset();
            p.beginArgs();
            p.push(kRead.dest, true);
            p.push(kRead.cnt, true);
            p.rb.clear();
            p.collecting = g_sumEcho;
            p.command(0x09);
        };
        deliver(0);
        /* Core 1's transfers must land in the SECOND mailbox phase, after the
         * trailing 2 — otherwise core 1 dies at P:0xfff000, every time. */
        const uint64_t p2 = ot::g_icc.phase2;
        for (unsigned g = 0;
             ot::g_icc.phase2 == p2 && budgetLeft() && g < 100000; g++)
            stepBoth(256);
        deliver(1);
        if (g_metroEvery) {
            const auto peak = [](unsigned area, TWord lo, TWord hi) {
                TWord m = 0;

                for (TWord a = lo; a < hi; a++) {
                    const TWord v = g_chip.core[1].mem->get(
                        area ? MemArea_Y : MemArea_X, a) & 0xffffff;
                    const TWord u = v & 0x800000 ? 0x1000000 - v : v;

                    if (u > m)
                        m = u;
                }
                return m;
            };
            const TWord icc = peak(0, 0x38000, 0x38010);
            const TWord clk = peak(0, 0x0000, 0x0010);
            const TWord env = peak(1, 0x0293, 0x0294);
            if (icc > g_metroIcc)   g_metroIcc = icc;
            if (clk > g_metroClk)   g_metroClk = clk;
            if (env > g_metroEnv)   g_metroEnv = env;
            if (env)                g_metroLive++;
        }
        frame0 += kFrames;
        blocks++;
    }

replay_done:
    if (!g_dumpMem.empty()) {
        dumpMemory(g_dumpMem);
    }
    if (g_metroEvery) {
        printf("\nmetronome: envelope live in %llu blocks (peak %06x), click "
               "buffer peak %06x, core 1 -> core 0 audio peak %06x\n",
               (unsigned long long)g_metroLive, g_metroEnv, g_metroClk,
               g_metroIcc);
    }
    printf("\nblocks %llu  budget %llu/%llu  renders %llu\n",
           (unsigned long long)blocks, (unsigned long long)g_spent,
           (unsigned long long)g_budget,
           (unsigned long long)ot::g_icc.renders.load());
    for (unsigned i = 0; i < 2; i++) {
        const ot::Core &c = g_chip.core[i];
        const Port &p = port[i];
        printf("core %u: %s pc=%#07x in=%llu out=%llu cmds=%llu(%llu bad) "
               "icr=%llu(%llu late) drops=%llu rx=%llu tx=%llu\n",
               i, c.dead ? "DEAD" : "alive", c.pc(),
               (unsigned long long)p.wordsIn, (unsigned long long)p.wordsOut,
               (unsigned long long)p.commands, (unsigned long long)p.cmdFails,
               (unsigned long long)p.icrResets,
               (unsigned long long)p.icrTimeouts,
               (unsigned long long)p.dropped,
               (unsigned long long)c.rxFrames, (unsigned long long)c.txFrames);
        printf("  tx slots:");
        for (unsigned s = 0; s < kSlots; s++)
            printf(" %u:%.4f", s, g_slotPeak[i][s] / 8388608.0);
        printf("\n");
    }

    bool pass = true;
    if (expectTone > 0) {
        double best = 0;
        for (unsigned c = 0; c < 2; c++)
            for (unsigned s = 0; s < kSlots; s++) {
                const double peak = g_slotPeak[c][s] / 8388608.0;
                if (peak < 1e-4)
                    continue;
                const double r = goertzel(g_outSlot[c][s], expectTone);
                printf("tone %.1f Hz: core %u slot %u peak %.6f ratio %.3f\n",
                       expectTone, c, s, peak, r);
                best = std::max(best, r);
            }
        printf("tone %.1f Hz -> %s\n", expectTone, best > 0.5 ? "PASS" : "FAIL");
        pass = best > 0.5;
    }

    for (unsigned s = 0; s < kSlots; s++)
        if (!outSlot[s].empty())
            writeWav(outSlot[s].c_str(), &g_outSlot[0][s], 1);
    if (!outMain.empty())   writeWav(outMain.c_str(),   &g_outSlot[0][1], 2);
    if (!outCue.empty())    writeWav(outCue.c_str(),    &g_outSlot[0][3], 2);
    if (!outPhones.empty()) writeWav(outPhones.c_str(), &g_outSlot[0][5], 2);

    return pass ? 0 : 1;
}
