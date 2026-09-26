/*
 * The two DSP56721 cores behind QEMU's HDI24 window. The chip itself is
 * ot-dsp56k.{h,cc}, shared with octdsp; this file is the coupling —
 * how the guest, the DSP and the outside world take turns.
 *
 * THE SHAPE, and why it is this shape:
 *
 *   THE VCPU THREAD STEPS BOTH CORES, at a cadence set by GUEST PROGRESS.
 *   patches/qemu/0012 counts retired guest instructions in the TB preamble —
 *   icount's decrementer with icount's clock removed — and calls
 *   ot_dsp_interleave() every `interleave` of them. That tick steps the chip,
 *   and when the block closes it ships the block and holds the pace. Nothing
 *   else touches the DSP, so there is no lock to take, no handoff to wait for,
 *   and no ordering left for the host scheduler to decide: two chips that
 *   silicon keeps in a fixed ratio are now in a fixed ratio here.
 *
 *   The quantum is a CLOCK RATIO, not a speed knob, and it is calibrated
 *   against an observable (does the frame ISR finish delivering a block's arms
 *   before the codec crosses into bank setup at P:0x54?) rather than against
 *   264:400 — instructions are not cycles.
 *
 *   The DSP is FROZEN while the codec core has an unread published word (the
 *   gearmulator halt). Without it the ESAI burns block time as underruns
 *   during the host IRQ round trip and the ESAI:render:ISR ratio goes to
 *   2.7:1 instead of 1.00:1.
 *
 *   THE PACE STOPS THE GUEST TOO, because it is a blocking sleep on the one
 *   thread that runs both. The frontend is still a pure consumer and cannot
 *   stall the Octatrack. The throttle must BLOCK: sleep_for(50 us) poll 0.437x,
 *   yield() spin 0.615x, blocking 0.665x.
 *
 * ☠ FOUR STRUCTURAL CHANGES WERE MEASURED AND ARE ALL DEAD. Do not re-derive:
 *   - Letting the DSP run ahead is IMPOSSIBLE, not merely unhelpful: the guest
 *     commissions ~1385 words, 6 commands and 4 ICR resets every block and the
 *     DSP renders exactly one block from them. Out-ring occupancy at pop time
 *     was 0 or 1 across a whole run, never 2, with a 64-block ring.
 *   - Merging the DSP thread into the vCPU buys ~nil IN THROUGHPUT. It was
 *     done for DETERMINISM; at interleave=1024 capacity read 0.939x against
 *     the thread design's 1.014x in the same binary. Speed at smaller quanta
 *     is a modelling choice, not an efficiency gain.
 *   - The inline ICR/CVR drains cannot be skipped. 3.27 of 3.28 million PC
 *     samples sit in one of two wait states, which looks like pure spin, but
 *     instruction retirement IS the passage of peripheral time here: the
 *     codec's spin at the block-top DSR2 poll is what clocks the ESAI.
 *     Fast-forwarding it there measured 0.921x -> 0.837x.
 *
 * SPDX-License-Identifier: MIT
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "dsp56kEmu/disasm.h"
#include "ot-dsp56k.h"
#include "ot-qemu.h"
#include "ot-audio-shm.h"

using namespace dsp56k;
using ot::kFrames;
using ot::kIns;
using ot::kSlots;

namespace {

/* ---- counters ------------------------------------------------------------
 * Structural only. Every one is a COUNT of round trips or dispatches, so it
 * means the same thing on a loaded host as on a quiet one — the only kind of
 * measurement worth taking here. Wall-clock throughput is the weakest evidence
 * available; it is not collected.
 *
 * Written from the vCPU thread and read once at exit, so plain integers. */
std::atomic<bool> g_running {true};   /* cleared from atexit, on another thread */

uint64_t g_prodFF, g_holdTx, g_holdDeliver, g_holdOther;
uint64_t g_shipNonzero, g_blocksLate, g_resyncs;
/* How far behind the pace a block may fall before the debt is forgiven; see
 * closeBlock. */
constexpr auto kMaxDebt = std::chrono::milliseconds(150);
/* How often the delivery hold's wall-clock escape actually fires, and the
 * wall it let go of. The hold's PRIMARY release is the sum landing; the
 * escape exists for a delivery that never completes. Measured: the worst
 * blocks in a run sit at 193 and 200 ms against this 250 ms constant,
 * i.e. the escape ENDING them rather than protecting against a rare case —
 * so its firing rate is what decides whether a state-based gate is worth
 * the firmware work it would take. */
uint64_t g_holdHatch, g_holdHatchUs;

/* ---- the interleave ------------------------------------------------------
 * Guest instructions per DSP slice: the budget in patches/qemu/0012 calls
 * ot_dsp_interleave() every `g_interleave` retired guest instructions. It is
 * the RATIO the two chips run at, calibrated against an observable rather than
 * against 264:400. ☠ It cannot be interpolated: 768 is catastrophic where 512
 * and 1024 are fine, because the cadence can phase-lock with the block
 * sequence. Measure any new value; never guess between two good ones. */
uint32_t g_interleave = OT_INTERLEAVE_DEFAULT;
/* A TB holds up to TCG_MAX_INSNS (512) instructions and the budget is checked
 * at the TB's ENTRY, so the decrementer cannot resolve finer than that. To go
 * below it, keep the budget at 512 and run 512/quantum rounds per firing —
 * one knob, "guest instructions per DSP round", all the way down. */
unsigned g_roundsPerTick = 1;
/* The txFrames value that closes the block in flight. The interleave never
 * runs past it: reaching it IS the block boundary, where the block is shipped
 * and the pace held. */
uint64_t g_blockTarget = 0;             /* vCPU thread only */
bool g_inInterleave = false;            /* vCPU thread only */
uint64_t g_ticks, g_tickRan, g_tickHeld;

/*
 * The calibration observable, and the evidence Phase 4 needs.
 *
 * The question the interleave ratio has to answer is not "is 264:400 right" —
 * instructions are not cycles and QEMU models no cycle timing — but "does the
 * frame ISR finish delivering the block's arms before the codec crosses into
 * bank setup at P:0x54, every block, with margin?" That crossing is what the
 * delivery hold exists to protect, so its margin distribution is what decides
 * whether the hold can go.
 *
 * Margin is measured in the codec's OWN time (its exec count), between the
 * block's last transfer landing (the sum arm, dest 0x6400) and the codec
 * leaving the block-top poll. A crossing taken with a delivery still OPEN —
 * the hold's own predicate, pubsPopped > sumsLanded — is a STRADDLE: the rest
 * of that block's transfers land in the wrong bank.
 *
 * With the hold ENABLED the straddle count is trivially ~the hatch count,
 * because the hold's whole job is to make it zero. `-M octatrack,hold=off`
 * turns the hold off so the straddle rate can be measured for what it is.
 */
uint64_t g_sumExecs;
uint64_t g_marginMiss, g_marginBlocks;
/* Straddles split three ways, because the three want different answers:
 *  - boot: before the first nonzero readback. The hold is deliberately OFF
 *    here (a sustained hold starves the frame cadence and the firmware's
 *    watchdog parks the scheduler), and a silent mix cannot tear audibly.
 *  - live+ata: a straddle while the card is mid-PIO transfer. On silicon the
 *    eDMA would carry the delivery through that; here the minors are driven
 *    by guest stores and the CPU is at IPL 5 in the sector loop.
 *  - live: everything else — the genuine ratio race. */
uint64_t g_missBoot, g_missAta, g_missLive;
uint64_t g_stateHist[16][2];
/* Per-block sequence timing, in retired GUEST INSTRUCTIONS — load-invariant by
 * construction. [0] clean crossings, [1] straddled ones. `lag` is publish ->
 * first arm (did the sequence start late?); `span` is first arm -> crossing
 * (did it run long?). */
uint64_t g_lagSum[2], g_lagMax[2], g_spanSum[2], g_spanMax[2], g_seqN[2];
uint64_t g_pubInsn;
uint64_t g_marginHist[20];
/* Per-block cycle time, bucketed: bucket i covers [2^i, 2^(i+1)) us. The MEAN
 * cannot tell "every block is 15% too slow" (attack steady-state cost) from
 * "1% of blocks stall for milliseconds" (attack the stall), and those want
 * completely different work. Measured: 91-95% of blocks finish inside the
 * 363 us real-time budget and a 0.3-1.1% tail carries 13-21% of the wall. */
uint64_t g_blockHist[20];

/* ---- the audio rings ------------------------------------------------------ */
/* No lock on either ring. Both are touched only by the vCPU thread — pushed
 * from the ESAI callbacks while the DSP is being stepped, drained in
 * closeBlock() — and the frontend reads the SHARED-MEMORY ring, not these,
 * from another process. */
struct OutRing {                        /* DSP -> world, 8 slots per frame */
    /* Generous: the inline FIFO drains can tick the ESAI many blocks ahead of
     * the pops, and a full ring DROPS frames from the Octatrack's timeline —
     * audible phase tears in a steady tone (measured out_drop=2826 over a walk
     * with an 8-block ring). This is transport slack, not coupling. */
    static constexpr unsigned kCap = 64 * kFrames;
    int32_t buf[kCap][kSlots];
    unsigned head = 0, count = 0;
    uint64_t frames = 0, dropped = 0;

    void push(const int32_t *slots)     /* never blocks: demand-driven */
    {
        if (count == kCap) {
            head = (head + 1) % kCap;
            count--;
            dropped++;
        }
        memcpy(buf[(head + count) % kCap], slots, sizeof buf[0]);
        count++;
        frames++;
    }

    bool popBlock(int32_t out[kFrames][kSlots])
    {
        if (count < kFrames) {
            return false;
        }
        for (unsigned f = 0; f < kFrames; f++) {
            memcpy(out[f], buf[head], sizeof buf[0]);
            head = (head + 1) % kCap;
        }
        count -= kFrames;
        return true;
    }
} g_out;

struct InRing {                         /* world -> DSP, 4 slots per frame */
    static constexpr unsigned kCap = 4 * kFrames;
    int32_t buf[kCap][kIns];
    unsigned head = 0, count = 0;
    uint64_t underruns = 0;

    void pushBlock(const int32_t in[kFrames][kIns])
    {
        for (unsigned f = 0; f < kFrames && count < kCap; f++) {
            memcpy(buf[(head + count) % kCap], in[f], sizeof buf[0]);
            count++;
        }
    }
    void popFrame(int32_t *slots)
    {
        if (!count) {
            underruns++;
            memset(slots, 0, kIns * sizeof *slots);
            return;
        }
        memcpy(slots, buf[head], kIns * sizeof *slots);
        head = (head + 1) % kCap;
        count--;
    }
} g_in;

/* ---- per-core host state -------------------------------------------------- */
struct ShimCore {
    ot::Core *c = nullptr;
    size_t argOcc = 0;
    uint64_t pubsPopped = 0, sumsLanded = 0;
    int argPhase = 2;
    unsigned bootWords = 0;
    uint32_t bootAddr = 0;              /* 0x31000 = payload A, the codec */
    uint32_t lastDest = 0, lastRx = 0;
    uint64_t wordsIn = 0, commands = 0, icrResets = 0;
    uint64_t writeStalls = 0, icrTimeouts = 0, cmdFails = 0, popStale = 0;
};
ShimCore g_shim[2];
ot::Chip g_chip;

bool g_sumSeen = false;
bool g_audioLive = false;
/* The delivery hold, on unless a measurement asks for it off — see the
 * straddle counter above. */
bool g_holdOn = true;

/* ---- transport ------------------------------------------------------------ */
int g_listenFd = -1;
std::atomic<int> g_clientFd {-1};
OtAudioShm *g_shm = nullptr;
char g_shmName[64];
bool g_throttle = OT_THROTTLE_DEFAULT; /* ot_dspcore_init always overrides */
/*
 * -M octatrack,exit-with-frontend=on: octemu sets this on the QEMU
 * it spawns. The frontend is the only thing that ever connects to the audio
 * socket, so once one has connected and then gone, the Octatrack has no
 * operator and no consumer: quit rather than run on as an orphan (the
 * frontend's own SIGTERM/atexit paths cannot cover a SIGKILL or a crash). Off
 * by default, so a QEMU run by hand still waits for a frontend and survives
 * one detaching.
 */
bool g_exitWithFrontend = false;
bool g_hadFrontend = false;
uint64_t g_blocks = 0;

/* The chip's own block period — REAL TIME, what the hardware does. Zero means
 * UNTHROTTLED, which is the only way to read the emulator's raw capacity, since
 * a throttle is a cap and cannot measure above itself. It is a measurement
 * mode, not a usable one: flat out, the vCPU saturates and the guest stops
 * being a user interface.
 *
 * ☠ Throttling is not optional and it is not a performance setting. The guest
 * pays several hundred microseconds of TCG time per block for its frame ISR
 * and state machine; consuming blocks at the emulator's raw capacity saturates
 * the vCPU, and the first thing to starve is the IPL-4 key parser — dialogs
 * stall and taps stop landing long before the audio degrades. Anything that
 * loosens this must re-test KEY DELIVERY, not just blocks per second. */
std::chrono::nanoseconds blockPeriod()
{
    return std::chrono::nanoseconds(g_throttle ? 1000000000ull * kFrames / ot::kRate
                                               : 0);
}

bool shmPublish(int fd)
{
    if (g_shm) {                        /* reconnect: reuse, but resync */
        g_shm->out_tail = g_shm->out_head;
        g_shm->in_tail = g_shm->in_head;
    } else {
        snprintf(g_shmName, sizeof g_shmName, "/octemu-audio-%d", (int)getpid());
        shm_unlink(g_shmName);
        int sfd = shm_open(g_shmName, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (sfd < 0) {
            return false;
        }
        if (ftruncate(sfd, sizeof(OtAudioShm)) != 0) {
            close(sfd);
            return false;
        }
        void *p = mmap(nullptr, sizeof(OtAudioShm), PROT_READ | PROT_WRITE,
                       MAP_SHARED, sfd, 0);
        close(sfd);
        if (p == MAP_FAILED) {
            return false;
        }
        g_shm = (OtAudioShm *)p;
        memset(g_shm, 0, sizeof *g_shm);
        g_shm->magic = OT_SHM_MAGIC;
        g_shm->throttled = g_throttle;
    }
    g_shm->consumer_alive = 1;

    char line[128];
    int n = snprintf(line, sizeof line, "%s%s\n", OT_SHM_HELLO, g_shmName);
    const char *w = line;
    while (n > 0) {
        ssize_t k = ::write(fd, w, (size_t)n);
        if (k <= 0) {
            return false;
        }
        w += k;
        n -= (int)k;
    }
    return true;
}

void audioListen(const char *path)
{
    struct sockaddr_un a = {};

    if (!path || !*path) {
        return;
    }
    if (strlen(path) >= sizeof a.sun_path) {
        fprintf(stderr, "octdsp: audio path too long: %s\n", path);
        return;
    }
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    unlink(path);
    g_listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listenFd < 0 || bind(g_listenFd, (struct sockaddr *)&a, sizeof a) < 0
        || listen(g_listenFd, 1) < 0) {
        fprintf(stderr, "octdsp: cannot listen on %s\n", path);
        if (g_listenFd >= 0) {
            close(g_listenFd);
        }
        g_listenFd = -1;
    }
}

/* ---- the produce loop ----------------------------------------------------- */
ot::Core &codecCore()
{
    return *(g_shim[1].bootAddr == 0x31000 ? g_shim[1].c : g_shim[0].c);
}
ShimCore &codecShim()
{
    return g_shim[1].bootAddr == 0x31000 ? g_shim[1] : g_shim[0];
}

/*
 * The delivery hold. Mid-delivery, with the codec parked at the block-top DSR2
 * poll, crossing into bank setup (P:0x54) flips the landing masks and strands
 * the rest of this block's transfers in the wrong bank — measured as ~200
 * impulse bursts per second. Three details are load-bearing, each found by ear:
 *
 *   - the release valve is 250 ms of WALL time. A ~2 ms valve re-opens the
 *     race (~15 audible clicks per walk).
 *   - the hold arms on the first NONZERO READBACK, i.e. before audio can reach
 *     the mix. Arming on the codec's own first output is too late: the first
 *     blocks race, the torn garbage seeds the recirculating master, and
 *     broadband noise self-sustains.
 *   - it must NOT be active during the silent boot. Sustained holds there
 *     starve the frame cadence and the firmware's watchdog parks the scheduler
 *     in its halt net at 0x4001fc9c (~1 boot in 5). A held silent mix cannot
 *     tear audibly anyway.
 *
 * ☠ The 250 ms is not a rarely-fired safety net: the worst blocks in a run
 * measure 193 and 200 ms against it, so it is what ENDS them. Gating on actual
 * bank state instead of a wall clock is the one open lever here — but it is a
 * change to be measured against tests/audio-quality.py, not assumed.
 */
constexpr auto kDeliveryHold = std::chrono::milliseconds(250);

/* ---- DIAG: lone-core stepping and the handoff trace ------------------------
 * Both off by default. With neither set, no hook is installed and the chip
 * steps exactly as it does without this block.
 *
 * OCTA_LONE_CORE=1 brings back the pre-74930ca stepping: every stepPair call
 * steps its core alone, and write_burst's full-FIFO stall steps the peer 64
 * after it, as it used to. That is the dropped-FLEX-trig bug on demand.
 *
 * OCTA_HANDOFF_TRACE=file traces the core 0 -> core 1 handoff through the
 * mailbox (y:$ffffd3-d7) and the shared window x:$30000..$30047. One line per
 * event, ordered by `seq` (every exec of either core), each core also timed
 * by its own exec count and instruction counter:
 *   P  core 0 posts a mailbox word; the window is snapshotted
 *   T  core 1 takes it: the exposure since the post, the execs each call site
 *      ran in between (by core), and which window words differ from the
 *      snapshot (nonzero = core 1 reads a message core 0 did not post)
 *   O  core 0's first window write after a post: its distance from the post
 *      and, once core 1 has taken the word, from the take — the margin
 *   W  every window write (detail range only): core, PC, changed words
 *   L  every stepPair call (detail range only): site, core, n, peer PC
 *   Q  any other mailbox traffic (detail range only)
 *   H  host-port traffic, either core (detail range only): ICR resets, host
 *      commands, argument words (the transfer's dest), write bursts
 *   R  core 1 takes a bank index (detail range only): the bank it will render
 *      from, word 30 (the gate/strobe) of all 8 slots, and slot 3 (T4) whole
 *   B  per block: instructions and execs per core, execs by call site and core
 *   M  a watched X word changed (whole run): OCTA_HANDOFF_WATCH is
 *      a comma list of hex X addresses on core 1 (default 207e,407e: word 30
 *      of slot 3 in both record banks); logs the exec's core, PC and site —
 *      a write by DMA shows up against whatever code happened to run
 *   V  core 1 enters a watched PC (OCTA_HANDOFF_PCS, hex list, default 38b:
 *      the amp/gate render, once per track): x:$207 (bank) and x:$208 (the
 *      track's record). Whole run, since it is the read side of the record
 *      handoff; M lines carry the same e1 (core-1 execs since it took the 2)
 *   S1 strobe-shaped words ($03 01dX) in a write burst: offset and value
 *   D  a disassembly of both cores' whole payloads, once
 * Blocks are RECORDING blocks — the frontend's `[mark] blk=` numbering, frame
 * 16N of the --recording — so OCTA_HANDOFF_FROM/_TO (the detail range) come
 * straight from a trig8 log. -1 before the frontend attaches.
 */
bool g_loneCore = false;
unsigned g_sliceIns = 0;           /* OCTA_SLICE_INS, see sliceStep */
bool g_sliceMatch = false;         /* OCTA_SLICE_MATCH, see stepIns */
uint64_t g_lastSliceIns = 0;

namespace ht {

enum Site { S_ROUND, S_FF, S_DRAIN, S_WSTALL, S_RXPOP, S_WBURST, S_RBURST, S_N };
const char *const kSiteName[S_N] = {"round", "ff", "drain", "wstall",
                                    "rxpop", "wburst", "rburst"};
constexpr TWord kWinBase = 0x30000;
constexpr unsigned kWin = 0x48;

FILE *f = nullptr;
int64_t from = 0, to = INT64_MAX;
int site = S_ROUND;
uint64_t seq, execs[2], blkExecs[S_N][2];
TWord shadow[kWin];
bool dumped = false;

struct Post {
    bool live = false, taken = false, over = false;
    TWord w = 0;
    uint64_t seq = 0, e0 = 0, i0 = 0, e1 = 0;   /* at the post */
    uint64_t te0 = 0, ti0 = 0, te1 = 0;         /* at the take */
    uint64_t between[S_N][2] = {};              /* execs, post -> take */
    TWord snap[kWin] = {};
} post;

/* Whole-run tallies, by mailbox word class: [0] bank index 0/1, [1] the 2. */
uint64_t nPost[2], nChanged[2], nOverFirst[2], nUntaken[2];
uint64_t mHistE[2][24], mHistI[2][24];
uint64_t mMinE[2] = {UINT64_MAX, UINT64_MAX}, mMinI[2] = {UINT64_MAX, UINT64_MAX};

TWord *win() { return g_chip.core[0].mem->getMemAreaPtr(MemArea_X) + kWinBase; }
uint64_t ins(unsigned i) { return g_chip.core[i].dsp->getInstructionCounter(); }
int64_t recBlock()
{
    if (!g_shm || g_clientFd.load() < 0) {
        return -1;
    }
    return (int64_t)((g_shm->out_head * kFrames + g_out.count) / kFrames);
}
bool detail(int64_t b) { return b >= from && b <= to; }
unsigned nWatch = 0, nPcs = 0;
TWord watchPc[16];
uint64_t e1AtTake2 = 0;
TWord watchAddr[16], watchVal[16];
TWord lastTake1 = 0;
uint64_t lastTake1Seq = 0;

void host(unsigned idx, const char *what, unsigned a, unsigned b2)
{
    const int64_t b = recBlock();

    if (!f || !detail(b)) {
        return;
    }
    fprintf(f, "H seq=%llu blk=%lld core=%u %s %#x %u pc0=%#x pc1=%#x "
            "c1took=%u(%llu ago) c1full=%d rx=%zu\n",
            (unsigned long long)seq, (long long)b, idx, what, a, b2,
            g_chip.core[0].pc(), g_chip.core[1].pc(), lastTake1,
            (unsigned long long)(seq - lastTake1Seq), ot::g_icc.rxFull(1) ? 1 : 0,
            g_chip.core[idx & 1].hdi().rxData().size());
}
unsigned bucket(uint64_t v)
{
    unsigned b = 0;
    for (; v > 1 && b < 23; v >>= 1) {
        b++;
    }
    return b;
}

void disasm(unsigned core, TWord lo, TWord hi)
{
    ot::Core &c = g_chip.core[core];
    const TWord *p = c.mem->getMemAreaPtr(MemArea_P);
    Disassembler d(c.dsp->opcodes());

    for (TWord pc = lo; pc < hi;) {
        std::string s;
        const uint32_t n = d.disassemble(s, p[pc], p[pc + 1], 0,
                                         c.dsp->regs().omr.toWord(), pc);
        fprintf(f, "D core=%u p=%#06x %06x %s\n", core, pc, p[pc], s.c_str());
        pc += n ? n : 1;
    }
}

void onExec(ot::Core &c, TWord pcBefore)
{
    const unsigned k = c.index;

    seq++;
    execs[k]++;
    blkExecs[site][k]++;
    if (post.live && !post.taken) {
        post.between[site][k]++;
    }
    if (k == 1) {
        for (unsigned i = 0; i < nPcs; i++) {
            if (pcBefore == watchPc[i]) {
                const TWord *x1 = g_chip.core[1].mem->getMemAreaPtr(MemArea_X);

                fprintf(f, "V seq=%llu blk=%lld pc=%#x e1=%llu bank=%#x rec=%#x site=%s\n",
                        (unsigned long long)seq, (long long)recBlock(), pcBefore,
                        (unsigned long long)(execs[1] - e1AtTake2), x1[0x207], x1[0x208],
                        kSiteName[site]);
            }
        }
    }
    if (nWatch) {
        const TWord *x1 = g_chip.core[1].mem->getMemAreaPtr(MemArea_X);

        for (unsigned i = 0; i < nWatch; i++) {
            const TWord v = x1[watchAddr[i]];
            if (v != watchVal[i]) {
                const int64_t b = recBlock();
                {   /* whole run: fires only when a watched word changes */
                    fprintf(f, "M seq=%llu blk=%lld x:%#x %06x>%06x core=%u pc=%#x site=%s "
                            "pc0=%#x pc1=%#x c1took=%u(%llu ago) e1=%llu rx1=%zu i0=%llu g=%llu\n",
                            (unsigned long long)seq, (long long)b, watchAddr[i],
                            watchVal[i], v, k, pcBefore, kSiteName[site],
                            g_chip.core[0].pc(), g_chip.core[1].pc(), lastTake1,
                            (unsigned long long)(seq - lastTake1Seq),
                            (unsigned long long)(execs[1] - e1AtTake2),
                            g_chip.core[1].hdi().rxData().size(),
                            (unsigned long long)ins(0),
                            (unsigned long long)ot_guest_insn());
                }
                watchVal[i] = v;
            }
        }
    }
    TWord *w = win();
    if (!memcmp(w, shadow, sizeof shadow)) {
        return;
    }
    const int64_t b = recBlock();
    if (detail(b)) {
        fprintf(f, "W seq=%llu blk=%lld core=%u pc=%#x->%#x site=%s ins=%llu",
                (unsigned long long)seq, (long long)b, k, pcBefore, c.pc(),
                kSiteName[site], (unsigned long long)ins(k));
        unsigned n = 0;
        for (unsigned i = 0; i < kWin; i++) {
            if (w[i] != shadow[i] && n++ < 16) {
                fprintf(f, " +%02x:%06x>%06x", i, shadow[i], w[i]);
            }
        }
        fprintf(f, " n=%u\n", n);
    }
    if (k == 0 && post.live && !post.over &&
        memcmp(w, post.snap, sizeof post.snap)) {
        const unsigned cls = post.w == 2;

        post.over = true;
        fprintf(f, "O seq=%llu blk=%lld w=%u pc=%#x site=%s post_e0=%llu post_i0=%llu",
                (unsigned long long)seq, (long long)b, post.w, pcBefore,
                kSiteName[site], (unsigned long long)(execs[0] - post.e0),
                (unsigned long long)(ins(0) - post.i0));
        if (post.taken) {
            const uint64_t me = execs[0] - post.te0, mi = ins(0) - post.ti0;

            fprintf(f, " take_e0=%llu take_i0=%llu take_e1=%llu\n",
                    (unsigned long long)me, (unsigned long long)mi,
                    (unsigned long long)(execs[1] - post.te1));
            mHistE[cls][bucket(me)]++;
            mHistI[cls][bucket(mi)]++;
            if (me < mMinE[cls]) mMinE[cls] = me;
            if (mi < mMinI[cls]) mMinI[cls] = mi;
        } else {
            fprintf(f, " BEFORE-TAKE\n");
            nOverFirst[cls]++;
        }
    }
    memcpy(shadow, w, sizeof shadow);
}

void onIcc(unsigned core, bool send, TWord w)
{
    const int64_t b = recBlock();

    if (send && core == 0) {
        if (post.live && !post.taken) {
            nUntaken[post.w == 2]++;
            fprintf(f, "X seq=%llu blk=%lld untaken w=%u replaced by %u\n",
                    (unsigned long long)seq, (long long)b, post.w, w);
        }
        post = Post();
        post.live = true;
        post.w = w;
        post.seq = seq;
        post.e0 = execs[0];
        post.i0 = ins(0);
        post.e1 = execs[1];
        memcpy(post.snap, win(), sizeof post.snap);
        nPost[w == 2]++;
        if (detail(b)) {
            fprintf(f, "P seq=%llu blk=%lld w=%u pc0=%#x pc1=%#x site=%s\n",
                    (unsigned long long)seq, (long long)b, w,
                    g_chip.core[0].pc(), g_chip.core[1].pc(), kSiteName[site]);
        }
        return;
    }
    if (!send && core == 1 && post.live && !post.taken) {
        const unsigned cls = post.w == 2;
        const TWord *now = win();
        unsigned n = 0;

        if (!dumped && b >= 0) {
            dumped = true;
            disasm(0, 0, 0x1fe0);
            disasm(1, 0, 0x1da0);
        }
        lastTake1 = w;
        lastTake1Seq = seq;
        if (w == 2) {
            e1AtTake2 = execs[1];
        }
        if (w < 2 && detail(b)) {
            const TWord *x = g_chip.core[1].mem->getMemAreaPtr(MemArea_X)
                             + (w ? 0x4000 : 0x2000);

            fprintf(f, "R seq=%llu blk=%lld bank=%#x w30", (unsigned long long)seq,
                    (long long)b, w ? 0x4000 : 0x2000);
            for (unsigned sl = 0; sl < 8; sl++) {
                fprintf(f, " %06x", x[sl * 0x20 + 30]);
            }
            fprintf(f, " t4");
            for (unsigned i = 0; i < 32; i++) {
                fprintf(f, " %06x", x[3 * 0x20 + i]);
            }
            fprintf(f, "\n");
        }
        post.taken = true;
        post.te0 = execs[0];
        post.ti0 = ins(0);
        post.te1 = execs[1];
        for (unsigned i = 0; i < kWin; i++) {
            n += now[i] != post.snap[i];
        }
        if (n) {
            nChanged[cls]++;
        }
        fprintf(f, "T seq=%llu blk=%lld w=%u got=%u g=%llu i0=%llu exp_e0=%llu exp_i0=%llu exp_e1=%llu "
                "changed=%u over=%d",
                (unsigned long long)seq, (long long)b, post.w, w,
                (unsigned long long)ot_guest_insn(), (unsigned long long)ins(0),
                (unsigned long long)(execs[0] - post.e0),
                (unsigned long long)(ins(0) - post.i0),
                (unsigned long long)(execs[1] - post.e1), n, post.over ? 1 : 0);
        for (unsigned i = 0, m = 0; i < kWin && m < 8; i++) {
            if (now[i] != post.snap[i]) {
                fprintf(f, " +%02x:%06x>%06x", i, post.snap[i], now[i]);
                m++;
            }
        }
        for (unsigned s = 0; s < S_N; s++) {
            if (post.between[s][0] || post.between[s][1]) {
                fprintf(f, " %s=%llu/%llu", kSiteName[s],
                        (unsigned long long)post.between[s][0],
                        (unsigned long long)post.between[s][1]);
            }
        }
        fprintf(f, "\n");
        return;
    }
    if (detail(b)) {
        fprintf(f, "Q seq=%llu blk=%lld core=%u %s w=%u pc=%#x\n",
                (unsigned long long)seq, (long long)b, core,
                send ? "send" : "recv", w, g_chip.core[core].pc());
    }
}

void onBlock()
{
    const int64_t b = recBlock();

    static uint64_t lastIns[2], lastEx[2];
    const uint64_t i0 = ins(0), i1 = ins(1);
    fprintf(f, "B blk=%lld hatch=%llu ins=%llu/%llu ex=%llu/%llu", (long long)b,
            (unsigned long long)g_holdHatch,
            (unsigned long long)(i0 - lastIns[0]), (unsigned long long)(i1 - lastIns[1]),
            (unsigned long long)(execs[0] - lastEx[0]),
            (unsigned long long)(execs[1] - lastEx[1]));
    lastIns[0] = i0;
    lastIns[1] = i1;
    lastEx[0] = execs[0];
    lastEx[1] = execs[1];
    for (unsigned s = 0; s < S_N; s++) {
        if (blkExecs[s][0] || blkExecs[s][1]) {
            fprintf(f, " %s=%llu/%llu", kSiteName[s],
                    (unsigned long long)blkExecs[s][0],
                    (unsigned long long)blkExecs[s][1]);
        }
    }
    fprintf(f, "\n");
    memset(blkExecs, 0, sizeof blkExecs);
    fflush(f);
}

void summary()
{
    if (!f) {
        return;
    }
    for (unsigned c = 0; c < 2; c++) {
        fprintf(f, "S w=%s posts=%llu changed_at_take=%llu overwritten_before_take=%llu "
                "untaken=%llu margin_min e0=%lld i0=%lld | e0hist",
                c ? "2" : "0/1", (unsigned long long)nPost[c],
                (unsigned long long)nChanged[c], (unsigned long long)nOverFirst[c],
                (unsigned long long)nUntaken[c],
                mMinE[c] == UINT64_MAX ? -1LL : (long long)mMinE[c],
                mMinI[c] == UINT64_MAX ? -1LL : (long long)mMinI[c]);
        for (unsigned i = 0; i < 24; i++) {
            if (mHistE[c][i]) {
                fprintf(f, " %u:%llu", 1u << i, (unsigned long long)mHistE[c][i]);
            }
        }
        fprintf(f, " | i0hist");
        for (unsigned i = 0; i < 24; i++) {
            if (mHistI[c][i]) {
                fprintf(f, " %u:%llu", 1u << i, (unsigned long long)mHistI[c][i]);
            }
        }
        fprintf(f, "\n");
    }
    fclose(f);
    f = nullptr;
}

void init()
{
    const char *e = getenv("OCTA_LONE_CORE");

    g_loneCore = e && *e && *e != '0';
    if ((e = getenv("OCTA_SLICE_MATCH")) && *e && *e != '0') {
        g_sliceMatch = true;
        fprintf(stderr, "octdsp: OCTA_SLICE_MATCH: peer slices match instructions (EXPERIMENT)\n");
    }
    if ((e = getenv("OCTA_SLICE_INS")) && atoi(e) > 0) {
        g_sliceIns = (unsigned)atoi(e);
        fprintf(stderr, "octdsp: OCTA_SLICE_INS=%u: slices in instructions (EXPERIMENT)\n",
                g_sliceIns);
    }
    if (g_loneCore) {
        fprintf(stderr, "octdsp: OCTA_LONE_CORE: cores step alone outside stepRound "
                "(pre-74930ca, DIAG)\n");
    }
    const char *path = getenv("OCTA_HANDOFF_TRACE");
    if (!path || !*path) {
        return;
    }
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "octdsp: cannot open %s\n", path);
        return;
    }
    setvbuf(f, nullptr, _IOFBF, 1 << 20);
    if ((e = getenv("OCTA_HANDOFF_FROM"))) from = strtoll(e, nullptr, 0);
    if ((e = getenv("OCTA_HANDOFF_TO")))   to = strtoll(e, nullptr, 0);
    {
        const char *wl = getenv("OCTA_HANDOFF_WATCH");
        char *end;

        if (!wl) {
            wl = "207e,407e";
        }
        while (*wl && nWatch < 16) {
            watchAddr[nWatch++] = (TWord)strtoul(wl, &end, 16) & 0x3ffff;
            wl = *end == ',' ? end + 1 : end;
            if (end == wl && *end != ',') {
                break;
            }
        }
    }
    {
        const char *pl = getenv("OCTA_HANDOFF_PCS");
        char *end;

        if (!pl) {
            pl = "38b";
        }
        while (*pl && nPcs < 16) {
            watchPc[nPcs++] = (TWord)strtoul(pl, &end, 16);
            if (*end != ',') {
                break;
            }
            pl = end + 1;
        }
    }
    ot::g_execHook = onExec;
    ot::g_iccHook = onIcc;
    atexit(summary);
    fprintf(stderr, "octdsp: OCTA_HANDOFF_TRACE=%s detail blocks %lld..%lld (DIAG)\n",
            path, (long long)from, (long long)to);
}

} // namespace ht


/*
 * How far one core runs before the other gets the host lock.
 *
 * ☠ This is not a throughput knob, it is a CORRECTNESS one. The payload hands
 * core 1 its per-block message through the shared window (x:$30000..$30047 —
 * the metronome click flag among it) and then publishes the mailbox word; core
 * 1 reads that window three instructions after it takes the word, while core 0
 * overwrites the same window from its own snapshot later in the same block. A
 * slice long enough to carry core 0 from the publish to that overwrite loses
 * the message, and on silicon the peer wins that race by thousands of cycles.
 * Measured on the metronome, which is exactly one such message: at 512 the
 * click arrives a handful of times in a 30 s run, at 16 it arrives on the beat.
 */
constexpr unsigned kCoreSlice = 16;
/*
 * EXPERIMENT (OCTA_SLICE_INS=N): a slice of N retired INSTRUCTIONS instead of
 * kCoreSlice JIT blocks. Both cores share one clock on silicon, but a JIT block
 * is 36 instructions on the codec and 14-24 on core 1, so equal block counts
 * run core 1 at about half the codec's rate. 0 = off (JIT blocks).
 */
/* EXPERIMENT (OCTA_SLICE_MATCH=1): the peer of a slice gets exactly the
 * instructions its partner just retired (the codec keeps kCoreSlice JIT blocks,
 * so its pace and the calibrated ColdFire:DSP ratio are unchanged), because the
 * two cores share one clock on silicon. */
void stepIns(ot::Core &c, uint64_t n)
{
    const uint64_t t = c.dsp->getInstructionCounter() + n;
    for (unsigned g = 0; g < 8192 && c.runnable() &&
                         c.dsp->getInstructionCounter() < t; g++) {
        c.step(1);
    }
}
void sliceStep(ot::Core &c)
{
    if (!g_sliceIns) {
        c.step(kCoreSlice);
        return;
    }
    const uint64_t t = c.dsp->getInstructionCounter() + g_sliceIns;
    for (unsigned g = 0; g < 4096 && c.runnable() &&
                         c.dsp->getInstructionCounter() < t; g++) {
        c.step(1);
    }
}

/*
 * ONE PASS over both cores: the whole of the chip's turn-taking. Returns
 * false when nothing could run, i.e. the DSP is held and only the guest can
 * release it.
 */
int g_lastHoldWhy;   /* DIAG: 0 ran, 1 tx latch, 2 delivery, 3 other */
bool stepRound()
{
    ot::Core &codec = codecCore();
    ShimCore &cs = codecShim();
    static std::chrono::steady_clock::time_point heldSince;
    static bool wasHeld;
    static bool wasPolling;
    bool ran = false;
    int holdWhy = 0;

    for (auto &c : g_chip.core) {
        bool hold = false;

        if (&c == &codec) {
            if (c.hdi().hasTX()) {
                /* The depth-1 TX latch with an unread word: on silicon the
                 * core can only spin at its HTDE wait, and every spin
                 * instruction ticks the ESAI. */
                hold = true;
                holdWhy = 1;
            } else if (g_holdOn &&
                       g_audioLive &&
                       cs.pubsPopped >
                           cs.sumsLanded &&
                       ot::atBlockPoll(c.pc())) {
                const auto now = std::chrono::steady_clock::now();

                if (!wasHeld) {
                    wasHeld = true;
                    heldSince = now;
                }
                if (now - heldSince > kDeliveryHold) {
                    g_holdHatch++;
                    g_holdHatchUs += (uint64_t)
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            now - heldSince).count();
                    /* The hatch is a deadlock breaker, so say what deadlocked.
                     * It fires ~once a run, and when it fires the Octatrack
                     * loses a quarter second of DSP time — which is what kills
                     * the first burst of a walk. */
                    fprintf(stderr, "octdsp: delivery hatch at block "
                            "%llu: codec pc=%#x outstanding=%llu tx=%d ata=%d\n",
                            (unsigned long long)g_blocks, c.pc(),
                            (unsigned long long)(cs.pubsPopped - cs.sumsLanded),
                            c.hdi().hasTX() ? 1 : 0, ot_ata_busy());
                    cs.sumsLanded = cs.pubsPopped;
                    wasHeld = false;
                } else {
                    hold = true;
                    holdWhy = 2;
                }
            } else {
                wasHeld = false;
            }
        }
        if (!c.runnable() || hold) {
            continue;
        }
        if (&c == &codec && ot::atBlockPoll(c.pc())) {
            /* Parked at the block-top DSR2 poll: only time can move it,
             * and time here is the instruction-driven ESAI clock. Running
             * the spin through the JIT costs a peripheral-callback read
             * per iteration — measured ~11 ms of a ~14 ms block wall. */
            ht::site = ht::S_FF;
            {
                const uint64_t i0 = c.dsp->getInstructionCounter();
                c.fastForwardSlot();
                g_lastSliceIns = c.dsp->getInstructionCounter() - i0;
            }
            ht::site = ht::S_ROUND;
            g_prodFF++;
            ran = true;
        } else if (&c != &codec && ot::atMailboxWait(c.pc()) &&
                   !ot::g_icc.rxFull(1)) {
            /* Core 1 parked at its mailbox wait with nothing pending.
             * ☠ Do NOT generalise this to "the PC did not change": DSP
             * rep/do loops hold one PC while doing real work, and a
             * generic PC-repeat park wedges the boot. */
        } else if (g_sliceMatch && &c != &codec) {
            stepIns(c, g_lastSliceIns);
            ran = true;
        } else {
            const uint64_t i0 = c.dsp->getInstructionCounter();
            sliceStep(c);
            g_lastSliceIns = c.dsp->getInstructionCounter() - i0;
            ran = true;
        }
    }

    /* The margin observable: the codec has just left the block-top poll for
     * bank setup. How much of its own time passed since the block's last
     * transfer landed? */
    {
        const bool polling = ot::atBlockPoll(codec.pc());

        if (wasPolling && !polling) {
            {   /* THROWAWAY: what state did the firmware's transfer state
                 * machine reach on this block, and does a straddle mean it
                 * stopped short of arming the sum? */
                const uint32_t st = ot_edma_state_ctr();
                const bool straddle =
                    cs.pubsPopped >
                    cs.sumsLanded;

                if (g_audioLive) {
                    const unsigned k = straddle ? 1 : 0;
                    const uint64_t now = ot_guest_insn();
                    const uint64_t start = ot_edma_seq_started();
                    const uint64_t pub = g_pubInsn;

                    g_stateHist[st < 15 ? st : 15][k]++;
                    if (start > pub && now > start) {
                        const uint64_t lag = start - pub, span = now - start;

                        g_seqN[k]++;
                        g_lagSum[k] += lag;
                        g_spanSum[k] += span;
                        if (lag > g_lagMax[k])   g_lagMax[k] = lag;
                        if (span > g_spanMax[k]) g_spanMax[k] = span;
                    }
                }
            }
            g_marginBlocks++;
            if (cs.pubsPopped >
                cs.sumsLanded) {
                g_marginMiss++;         /* a delivery straddled the crossing */
                if (!g_audioLive) {
                    g_missBoot++;
                } else if (ot_ata_busy()) {
                    g_missAta++;
                } else {
                    g_missLive++;
                }
            } else {
                uint64_t m = codec.execs - g_sumExecs;
                unsigned b = 0;

                for (uint64_t v = m; v > 1 && b < 19; v >>= 1) {
                    b++;
                }
                g_marginHist[b]++;
            }
        }
        wasPolling = polling;
    }

    g_lastHoldWhy = ran ? 0 : holdWhy ? holdWhy : 3;
    if (ran) {
        return true;
    }
    if (holdWhy == 1)      g_holdTx++;
    else if (holdWhy == 2) g_holdDeliver++;
    else                   g_holdOther++;
    return false;
}

int g_client = -1;
std::chrono::steady_clock::time_point g_deadline, g_blkT0;

/*
 * A BLOCK HAS CLOSED: ship everything the produce phase made, service the
 * audio socket, and hold the pace.
 *
 * Interleaved, this runs on the VCPU at a TB boundary, so the throttle stops
 * the guest too — which is the point: the two chips and the wall clock then
 * advance together instead of the guest free-running while the DSP waits.
 *
 * ☠ THE THROTTLE MUST BLOCK. Three designs, measured: sleep_for(50 us) poll
 * 0.437x, yield() spin 0.615x, blocking 0.665x. macOS sleeps hundreds of
 * microseconds long, and burning a core to avoid a syscall is a direct loss
 * because the vCPU is the thing that needs the core.
 */
void closeBlock()
{
    {   /* The true per-block cycle time, pacing sleep and all. The MEAN cannot
         * tell "every block 15% slow" from "1% of blocks stall for
         * milliseconds", and those want completely different work. */
        const auto n = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            n - g_blkT0).count();
        unsigned b = 0;

        g_blkT0 = n;
        for (int64_t v = us; v > 1 && b < 19; v >>= 1) {
            b++;
        }
        g_blockHist[b]++;
    }
    if (ht::f) {
        ht::onBlock();
    }

    if (g_exitWithFrontend && g_hadFrontend && g_client < 0) {
        fprintf(stderr, "octatrack: the frontend is gone, exiting\n");
        exit(0);
    }

    if (g_client < 0 && g_listenFd >= 0) {
        struct pollfd p = { g_listenFd, POLLIN, 0 };

        if (poll(&p, 1, 0) > 0) {
            g_client = accept(g_listenFd, nullptr, nullptr);
            g_clientFd = g_client;
            if (g_client >= 0) {
                g_hadFrontend = true;
            }
            if (g_client >= 0 && !shmPublish(g_client)) {
                close(g_client);
                g_client = -1;
                g_clientFd = -1;
            }
            int32_t z[kFrames][kIns] = {};
            if (!g_in.count) {
                g_in.pushBlock(z);          /* one block of zero input primes it */
            }
            g_deadline = std::chrono::steady_clock::now();
        }
    }

    /* Ship EVERY block the produce phase made: one pop per pass leaks
     * over-production into ring drops, which are audible as phase tears
     * (measured out_drop=5822 per 25k blocks). Heavy FIFO drains tick the
     * ESAI well past 16 frames in a single pass. */
    int32_t out[kFrames][kSlots];
    while (g_out.popBlock(out)) {
        ot_audio_tap_block(out);
        for (unsigned f = 0; f < kFrames; f++) {
            if (out[f][1] || out[f][2] || out[f][3] || out[f][4]) {
                g_shipNonzero++;            /* did MAIN carry audio? */
                break;
            }
        }
        g_blocks++;
        if (!g_shm || g_client < 0) {
            /* Nobody listening: keep the pace, and feed the DSP silence so
             * in_underruns only ever counts real ones. It used to count every
             * input frame read before the frontend attached — measured
             * 13911 in a windowed session whose PipeWire startup took ~0.3 s,
             * against 16-112 headless, all at blocks 0-1 or pre-connect. */
            int32_t z[kFrames][kIns] = {};
            g_in.pushBlock(z);
            continue;
        }
        /* Backstop only: the frontend has stopped, not merely hiccuped. */
        while (g_running && g_shm->consumer_alive && g_client >= 0 &&
               g_shm->out_head - g_shm->out_tail >= OT_SHM_MAX_AHEAD) {
            char b;
            if (::recv(g_client, &b, 1, MSG_DONTWAIT | MSG_PEEK) == 0) {
                close(g_client);
                g_client = -1;
                g_clientFd = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (g_client < 0) {
            continue;
        }
        const uint64_t h = g_shm->out_head;
        memcpy(g_shm->out[h & (OT_SHM_OUT_CAP - 1)], out, sizeof out);
        __atomic_store_n(&g_shm->out_head, h + 1, __ATOMIC_RELEASE);

        int32_t in[kFrames][kIns];
        const uint64_t t = g_shm->in_tail;
        if (__atomic_load_n(&g_shm->in_head, __ATOMIC_ACQUIRE) != t) {
            memcpy(in, g_shm->in[t & (OT_SHM_IN_CAP - 1)], sizeof in);
            __atomic_store_n(&g_shm->in_tail, t + 1, __ATOMIC_RELEASE);
        } else {
            memset(in, 0, sizeof in);       /* starved: silence */
        }
        g_in.pushBlock(in);
    }

    if (g_client >= 0) {                    /* death notification only */
        char b;
        if (::recv(g_client, &b, 1, MSG_DONTWAIT | MSG_PEEK) == 0) {
            close(g_client);
            g_client = -1;
            g_clientFd = -1;
        }
    }

    /* THE PACE. Absorb small overshoot by keeping the schedule; resync
     * only on a genuine stall (a full period late).
     * Resetting the deadline on ANY lateness throws away the sleep's
     * own overshoot too — macOS returns 50-100 us late, so every block
     * lost that much permanently and the Octatrack ran ~8% below the pace
     * it was asked for (nominal 919 blk/s, measured 841). */
    if (!g_throttle) {
        return;                             /* unthrottled: measurement only */
    }
    const auto period = blockPeriod();
    const auto now = std::chrono::steady_clock::now();

    /*
     * ☠ REPAY LATENESS, don't forgive it. This used to resync the schedule
     * on anything a full period (363 us) late, so every heavy block (a card
     * read, a UI redraw, a JIT compile) lost its overrun for good: the stream
     * averaged below real time and the live monitor's rate servo turned the
     * shortfall into pitch — the warble. With headroom above real time the
     * guest can instead run ahead unslept until it is back on schedule. Only
     * a genuine stall (kMaxDebt, well inside the monitor's 250 ms cushion)
     * resyncs.
     */
    g_deadline += period;
    if (now - g_deadline > period) {
        g_blocksLate++;                     /* behind; catching up */
    }
    if (now - g_deadline > kMaxDebt) {
        g_resyncs++;                        /* a real stall: give up the debt */
        g_deadline = now;
    } else if (g_deadline > now) {
        std::this_thread::sleep_until(g_deadline);
    }
}

void shutdownAudio()
{
    int fd = g_clientFd.exchange(-1);

    g_running = false;
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
}

/*
 * One drain quantum for the inline ICR/CVR drains — the loops that run the DSP
 * on the vCPU thread, inside an MMIO write, until the core has consumed what
 * the guest just handed it.
 *
 * THIS IS DELIBERATELY JUST step(64), and three ways of making it cleverer
 * were measured as WORSE. Sampling the DSP's PC per iteration shows 3.27 of
 * 3.28 million samples parked in one of two wait states, which looks exactly
 * like pure spin on a path costing 55 us and 2320 JIT entries per block. It is
 * not spin: fastForwarding the codec out of its DSR2 poll here measured icr
 * 45 -> 66 us/blk and 0.921x -> 0.837x, because the drain then took ownership
 * of the ESAI clock away from the producer. Running the peer core every
 * iteration, or only at a mailbox park, cost 22% and 12% more DSP work per
 * block for throughput that overlapped completely. The work is not duplicated,
 * only relocated onto the vCPU — attack the holds, not the drain.
 *
 * ☠ BUT "just step(64)" ON ONE CORE WAS A CORRECTNESS BUG, found later, and
 * the peer now runs with it (stepPair) — for correctness, not throughput.
 * These drains (and the rx pops and write stalls) advanced ONE core 64-256
 * instructions while its peer stood still, which breaks the kCoreSlice
 * lockstep the two cores' mailbox/shared-window handoff depends on. Measured
 * on a FLEX single-cycle slice trigged on quarter notes (tests/trig8-repro.sh):
 * a trig lands 2 frames later in its block each beat (1378.125 blocks/beat),
 * and whichever phase coincided with a lone-core burst lost the note's
 * sustain — ~0.3% of trigs at stock (3/672), and EVERY 8th trig at
 * --interleave 1024 or with an exact-counting budget (qemu 0017). With
 * stepPair: 0 drops in 1680+ beats across all three. The peer is skipped
 * exactly where stepRound would not step it (frozen codec, codec parked at the
 * block poll whose ESAI time belongs to the producer, core 1 idle at its
 * mailbox), which keeps the cost to ~2% (PGO: 3420 -> ~3480 Mcycles/emu-s),
 * not the 12-22% the unconditional variants measured.
 */
/*
 * Step core `c` by `n` instructions WITH its peer, in kCoreSlice alternation —
 * the only way the two cores may advance relative to each other (see
 * kCoreSlice). The peer is skipped where stepRound would hold it: not
 * runnable, or the codec with an unread published word.
 */
void stepPair(ot::Core &c, unsigned n, int site)
{
    const int prev = ht::site;

    ht::site = site;
    if (ht::f && ht::detail(ht::recBlock())) {
        ot::Core &o = g_chip.core[c.index ^ 1];
        fprintf(ht::f, "L seq=%llu blk=%lld site=%s core=%u n=%u pc=%#x peer_pc=%#x%s\n",
                (unsigned long long)ht::seq, (long long)ht::recBlock(),
                ht::kSiteName[site], c.index, n, c.pc(), o.pc(),
                g_loneCore ? " lone" : "");
    }
    if (g_loneCore) {                   /* DIAG: the pre-74930ca stepping */
        c.step(n);
        ht::site = prev;
        return;
    }
    for (unsigned k = 0; k < n; k += kCoreSlice) {
        const uint64_t ci0 = c.dsp->getInstructionCounter();
        sliceStep(c);
        const uint64_t cdone = c.dsp->getInstructionCounter() - ci0;
        for (auto &o : g_chip.core) {
            if (&o == &c || !o.runnable()) {
                continue;
            }
            if (&o == &codecCore()) {
                /* Frozen with an unread word, or parked at the block-top
                 * DSR2 poll — where only ESAI time moves it, and that time
                 * belongs to the producer (stepRound fast-forwards it). */
                if (o.hdi().hasTX() || ot::atBlockPoll(o.pc())) {
                    continue;
                }
            } else if (ot::atMailboxWait(o.pc()) && !ot::g_icc.rxFull(1)) {
                continue;               /* core 1 idle at its mailbox: nothing to hand over */
            }
            if (g_sliceMatch) {
                stepIns(o, cdone);
            } else {
                sliceStep(o);
            }
        }
    }
    ht::site = prev;
}

ot::StepFn drainStep(ot::Core &c)
{
    return [&c] { stepPair(c, 64, ht::S_DRAIN); };
}

} // namespace

/* ---- the C API ------------------------------------------------------------ */

extern "C" {

void ot_dspcore_init(const char *audio_path, int throttle, uint32_t interleave,
                     int exit_with_frontend)
{
    g_throttle = throttle != 0;              /* off = unthrottled, see above */
    g_exitWithFrontend = exit_with_frontend != 0;
    g_interleave = interleave;
    /* A TB holds at most TCG_MAX_INSNS instructions and the budget is checked
     * at its ENTRY, so 512 is the finest the decrementer can resolve. Below
     * that, keep the budget at 512 and run several rounds per firing. */
    g_roundsPerTick = interleave && interleave < 512 ? 512 / interleave : 1;
    if (!g_chip.open()) {
        fprintf(stderr, "octdsp: shm failed\n");
        abort();
    }
    for (unsigned i = 0; i < 2; i++) {
        g_shim[i].c = &g_chip.core[i];
        g_chip.core[i].init(i,
            [](int32_t *in) { g_in.popFrame(in); },
            [](const int32_t *slots) { g_out.push(slots); });
    }
    {
        int32_t z[kFrames][kIns] = {};
        g_in.pushBlock(z);                  /* input for the very first block */
    }
    audioListen(audio_path);
    ht::init();
    g_blkT0 = g_deadline = std::chrono::steady_clock::now();
    atexit(shutdownAudio);
}

void ot_dsp_hold_set(int on)
{
    g_holdOn = on != 0;
}

/*
 * One interleave quantum, on the vCPU thread at a TB boundary — the guest has
 * just retired `interleave` instructions, so the DSP gets the slice of its own
 * time that buys. See ot-qemu.h and patches/qemu/0012.
 *
 * This is the whole scheduler when the interleave is armed: it steps the
 * chip, and when the block closes it ships it and holds the pace, all on this
 * one thread. Nothing else touches the DSP, so there is no lock to take, no
 * handoff to wait for, and no ordering left for the host scheduler to decide.
 *
 * ☠ The re-entrancy guard is not paranoia. The vCPU also steps the DSP from
 * inside MMIO handlers (the inline ICR/CVR drains and the eDMA bursts); those
 * run mid-TB, where this hook cannot fire — but the guard is what makes that
 * an invariant rather than a happy accident.
 */
void ot_dsp_interleave(void)
{
    ot::Core &codec = codecCore();

    if (g_inInterleave || !g_running) {
        return;
    }
    g_ticks++;
    if (!codec.runnable()) {
        return;
    }
    g_inInterleave = true;
    for (unsigned r = 0; r < g_roundsPerTick; r++) {
        if (codec.txFrames >= g_blockTarget) {
            break;
        }
        if (!stepRound()) {
            g_tickHeld++;
            break;                  /* held: only the guest can release it */
        }
        g_tickRan++;
    }
    if (codec.txFrames >= g_blockTarget) {
        closeBlock();
        g_blockTarget = codec.txFrames + kFrames;
    }
    g_inInterleave = false;
    /* HREQ is raised on the vCPU by construction now: no bottom half, no
     * async_run_on_cpu, no cross-thread qemu_set_irq. */
    ot_dsp_hreq_sync();
}

void ot_dspcore_write(unsigned idx, uint32_t w)
{
    ShimCore &s = g_shim[idx & 1];
    ot::Core &c = *s.c;

    s.wordsIn++;
    if (!c.booted) {
        s.bootWords++;
        if (s.bootWords == 2) {
            s.bootAddr = w;         /* 0x31000 = payload A, the codec core */
        }
        if (c.boot->hdiWriteTX(w & 0xFFFFFF)) {
            c.booted = true;
            fprintf(stderr, "octdsp: core %u ROM boot done, pc=%#x\n",
                    idx & 1, c.boot->getInitialPC());
        }
        return;
    }
    if (c.dead) {
        return;
    }
    /* Argument words are never run over: a still-armed host-receive DMA
     * channel would eat them ahead of the command handler. */
    if (s.argPhase < 2) {
        ht::host(idx, "arg", w & 0xFFFFFF, s.argPhase);
        if (s.argPhase == 0) {
            s.lastDest = w & 0xFFFF;
        }
        s.argPhase++;
    }
    for (unsigned g = 0;
         g < 100000 && c.hdi().rxData().size() >= ot::kFifoDepth && !c.dead; g++) {
        s.writeStalls++;
        stepPair(c, 256, ht::S_WSTALL);
    }
    TWord word = w & 0xFFFFFF;
    c.hdi().writeRX(&word, 1);
}

unsigned ot_dspcore_rx_pending(unsigned idx)
{
    return g_shim[idx & 1].c->hdi().hasTX() ? 1 : 0;
}

uint32_t ot_dspcore_rx_peek(unsigned idx)
{
    ot::Core &c = *g_shim[idx & 1].c;

    return c.hdi().hasTX() ? (c.hdi().txData().front() & 0xFFFFFF) : 0;
}

uint32_t ot_dspcore_rx_pop(unsigned idx)
{
    ShimCore &s = g_shim[idx & 1];
    ot::Core &c = *s.c;

    if (!c.hdi().hasTX()) {
        if (!c.hostTxArmed()) {
            s.popStale++;
            return s.lastRx;     /* silicon: an unsupplied read is stale */
        }
        /* Pull the next word out of the armed DSP-side channel directly — the
         * request-paced supply, without a per-word thread round trip. */
        c.periphX.getDMA().trigger(DmaChannel::RequestSource::HostTransmitData);
        if (!c.hdi().hasTX()) {
            stepPair(c, 64, ht::S_RXPOP);
        }
        if (!c.hdi().hasTX()) {
            s.popStale++;
            return s.lastRx;
        }
    }
    s.lastRx = c.hdi().readTX() & 0xFFFFFF;
    if (g_sumSeen && &s == &codecShim() && s.lastRx < 2 && !c.hdi().hasTX()) {
        s.pubsPopped++;
        g_pubInsn = ot_guest_insn();
    }
    return s.lastRx;
}

void ot_dspcore_icr(unsigned idx, unsigned val)
{
    ShimCore &s = g_shim[idx & 1];
    ot::Core &c = *s.c;

    if (!(val & 0x80) || !c.booted) {          /* INIT */
        return;
    }
    s.argPhase = 0;
    s.icrResets++;
    ht::host(idx, "icr", val, 0);
    if (!ot::portIcrReset(c, drainStep(c))) {
        s.icrTimeouts++;
    }
    s.argOcc = c.hdi().rxData().size();
}

void ot_dspcore_cvr(unsigned idx, unsigned val)
{
    ShimCore &s = g_shim[idx & 1];
    ot::Core &c = *s.c;

    s.commands++;
    ht::host(idx, "cvr", val, 0);
    if (!c.runnable()) {
        return;
    }
    if (!ot::portCommand(c, val, s.argOcc, drainStep(c))) {
        s.cmdFails++;
    }
}

void ot_dspcore_write_burst(unsigned idx, uint32_t txh,
                             const uint8_t *be, unsigned halves)
{
    ShimCore &s = g_shim[idx & 1];
    ot::Core &c = *s.c;
    TWord w[1024];

    if (!c.runnable()) {
        return;
    }
    /* The block's final transfer: landing it releases the delivery hold. */
    if (s.lastDest == 0x6400) {
        g_sumSeen = true;
        s.sumsLanded = s.pubsPopped;
        /* Stamp the margin observable: this is "the block's arms are all in".
         * stepRound() reads it back when the codec leaves the block poll. */
        g_sumExecs = codecCore().execs;
    }
    s.wordsIn += halves;
    ht::host(idx, "burst", s.lastDest, halves);
    while (halves) {
        unsigned n = halves > 1024 ? 1024 : halves;

        for (unsigned i = 0; i < n; i++) {
            w[i] = (txh << 16) | ((TWord)be[2 * i] << 8) | be[2 * i + 1];
        }
        if (ht::f && ht::detail(ht::recBlock())) {
            for (unsigned i = 0; i < n; i++) {
                if ((w[i] & 0xFFFFF0) == 0x0301d0) {
                    fprintf(ht::f, "S1 seq=%llu blk=%lld core=%u dest=%#x off=%u of %u %06x\n",
                            (unsigned long long)ht::seq, (long long)ht::recBlock(),
                            idx, s.lastDest, i, n, w[i]);
                }
            }
        }
        /* Drain a full FIFO inline, stepping BOTH cores: the mailbox coupling
         * wedges if only the receiver runs. */
        for (unsigned g = 0;
             g < 100000 && c.hdi().rxData().size() >= ot::kFifoDepth && !c.dead;
             g++) {
            s.writeStalls++;
            if (g_loneCore) {           /* DIAG: exactly the pre-74930ca stall */
                ht::site = ht::S_WBURST;
                c.step(256);
                for (auto &o : g_chip.core) {
                    if (&o != &c) {
                        o.step(64);
                    }
                }
                ht::site = ht::S_ROUND;
            } else {
                stepPair(c, 256, ht::S_WBURST);
            }
        }
        c.hdi().writeRX(w, n);
        be += 2 * n;
        halves -= n;
    }
}

void ot_dspcore_read_burst(unsigned idx, uint8_t *be, unsigned halves)
{
    ShimCore &s = g_shim[idx & 1];
    ot::Core &c = *s.c;

    for (unsigned i = 0; i < halves; i++) {
        uint32_t w;

        if (!c.hdi().hasTX() && c.hostTxArmed()) {
            c.periphX.getDMA().trigger(DmaChannel::RequestSource::HostTransmitData);
            if (!c.hdi().hasTX()) {
                stepPair(c, 64, ht::S_RBURST);
            }
        }
        if (c.hdi().hasTX()) {
            w = s.lastRx = c.hdi().readTX() & 0xFFFFFF;
            /* Arm the delivery hold on the first nonzero readback — a track is
             * rendering, and this happens before audio can reach the mix. */
            if (w & 0xFFFF) {
                g_audioLive = true;
            }
        } else {
            s.popStale++;
            w = s.lastRx;
        }
        be[2 * i] = (w >> 8) & 0xFF;
        be[2 * i + 1] = w & 0xFF;
    }
}

int ot_dspcore_hreq(void)
{
    /* The codec core (payload A, ROM boot address 0x31000) publishes the bank
     * index; its latch is the frame line. Fall back to core 0 before boot. */
    for (auto &s : g_shim) {
        if (s.bootAddr == 0x31000) {
            return s.c->hdi().hasTX() ? 1 : 0;
        }
    }
    return g_shim[0].c->hdi().hasTX() ? 1 : 0;
}

uint64_t ot_dsp_blocks(void)   /* DIAG: blocks shipped, for ATA logging */
{
    return g_blocks;
}

/* Frames the codec core has clocked out of the ESAI: the board's guest time
 * base (ot_gclk_ns in ot-board.c). Only ever grows for a given core; the
 * codec's identity is settled by its boot address, and the caller clamps. */
int ot_dsp_hold_why(void)   /* DIAG */
{
    return g_shim[0].c ? g_lastHoldWhy * 0x100000 + (codecCore().pc() & 0xfffff)
                       : -1;
}
uint64_t ot_dsp_frames(void)
{
    return g_shim[0].c ? codecCore().txFrames : 0;
}

void ot_dsp_stats(char *buf, size_t len)
{
    const uint64_t nb = g_blocks ? g_blocks : 1;
    size_t o;

    o = (size_t)snprintf(buf, len,
             "throttle=%s ilv=%u blocks=%llu renders=%llu esai_blocks=%llu ship_nz=%llu "
             "in_underruns=%llu out_drop=%llu late=%llu resyncs=%llu "
             "hatch=%llu hatch_us=%llu | "
             "PER-BLK ff=%.0f execs=%.0f holds tx=%.2f dlv=%.2f oth=%.2f "
             "ticks=%.0f (ran=%.0f held=%.0f) | "
             "core0 %s pc=%#x in=%llu cmds=%llu(%llu bad) icr=%llu(%llu late) "
             "stalls=%llu stale=%llu tx=%llu | "
             "core1 %s pc=%#x in=%llu cmds=%llu(%llu bad) icr=%llu(%llu late) "
             "stalls=%llu stale=%llu tx=%llu",
             g_throttle ? "on" : "off", g_interleave,
             (unsigned long long)g_blocks,
             (unsigned long long)ot::g_icc.renders.load(),
             (unsigned long long)(g_chip.core[0].txFrames
                                  + g_chip.core[1].txFrames) / kFrames,
             (unsigned long long)g_shipNonzero,
             (unsigned long long)g_in.underruns,
             (unsigned long long)g_out.dropped,
             (unsigned long long)g_blocksLate,
             (unsigned long long)g_resyncs,
             (unsigned long long)g_holdHatch,
             (unsigned long long)g_holdHatchUs,
             (double)g_prodFF / (double)nb,
             (double)(g_chip.core[0].execs + g_chip.core[1].execs) / (double)nb,
             (double)g_holdTx / (double)nb, (double)g_holdDeliver / (double)nb,
             (double)g_holdOther / (double)nb,
             (double)g_ticks / (double)nb, (double)g_tickRan / (double)nb,
             (double)g_tickHeld / (double)nb,
             g_chip.core[0].dead ? "DEAD" : "alive", g_chip.core[0].pc(),
             (unsigned long long)g_shim[0].wordsIn,
             (unsigned long long)g_shim[0].commands,
             (unsigned long long)g_shim[0].cmdFails,
             (unsigned long long)g_shim[0].icrResets,
             (unsigned long long)g_shim[0].icrTimeouts,
             (unsigned long long)g_shim[0].writeStalls,
             (unsigned long long)g_shim[0].popStale,
             (unsigned long long)g_chip.core[0].txFrames,
             g_chip.core[1].dead ? "DEAD" : "alive", g_chip.core[1].pc(),
             (unsigned long long)g_shim[1].wordsIn,
             (unsigned long long)g_shim[1].commands,
             (unsigned long long)g_shim[1].cmdFails,
             (unsigned long long)g_shim[1].icrResets,
             (unsigned long long)g_shim[1].icrTimeouts,
             (unsigned long long)g_shim[1].writeStalls,
             (unsigned long long)g_shim[1].popStale,
             (unsigned long long)g_chip.core[1].txFrames);

    if (o + 16 < len) {
        o += (size_t)snprintf(buf + o, len - o, " | blkhist");
    }
    for (unsigned b = 0; b < 20 && o + 28 < len; b++) {
        uint64_t n = g_blockHist[b];

        if (n) {
            o += (size_t)snprintf(buf + o, len - o, " %u:%llu",
                                  1u << b, (unsigned long long)n);
        }
    }
    /* The delivery margin, in codec execs between the block's last transfer
     * landing and the codec crossing into bank setup. `miss` counts the
     * crossings that saw no new arms at all — the race the delivery hold
     * exists to catch, and the number that decides whether it can go. */
    if (o + 32 < len) {
        o += (size_t)snprintf(buf + o, len - o, " | margin n=%llu miss=%llu",
                              (unsigned long long)g_marginBlocks,
                              (unsigned long long)g_marginMiss);
    }
    for (unsigned k = 0; k < 2 && o + 72 < len; k++) {
        if (g_seqN[k]) {
            o += (size_t)snprintf(buf + o, len - o,
                                  " %s n=%llu lag=%llu/%llu span=%llu/%llu",
                                  k ? "STRADDLE" : "clean",
                                  (unsigned long long)g_seqN[k],
                                  (unsigned long long)(g_lagSum[k] / g_seqN[k]),
                                  (unsigned long long)g_lagMax[k],
                                  (unsigned long long)(g_spanSum[k] / g_seqN[k]),
                                  (unsigned long long)g_spanMax[k]);
        }
    }
    for (unsigned st = 0; st < 16 && o + 40 < len; st++) {
        if (g_stateHist[st][0] || g_stateHist[st][1]) {
            o += (size_t)snprintf(buf + o, len - o, " st%u:%llu/%llu", st,
                                  (unsigned long long)g_stateHist[st][0],
                                  (unsigned long long)g_stateHist[st][1]);
        }
    }
    if (o + 48 < len) {
        o += (size_t)snprintf(buf + o, len - o,
                              "(boot=%llu ata=%llu live=%llu)",
                              (unsigned long long)g_missBoot,
                              (unsigned long long)g_missAta,
                              (unsigned long long)g_missLive);
    }
    for (unsigned b = 0; b < 20 && o + 28 < len; b++) {
        uint64_t n = g_marginHist[b];

        if (n) {
            o += (size_t)snprintf(buf + o, len - o, " %u:%llu",
                                  1u << b, (unsigned long long)n);
        }
    }
}

} // extern "C"
