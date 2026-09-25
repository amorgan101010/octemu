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
            c.fastForwardSlot();
            g_prodFF++;
            ran = true;
        } else if (&c != &codec && ot::atMailboxWait(c.pc()) &&
                   !ot::g_icc.rxFull(1)) {
            /* Core 1 parked at its mailbox wait with nothing pending.
             * ☠ Do NOT generalise this to "the PC did not change": DSP
             * rep/do loops hold one PC while doing real work, and a
             * generic PC-repeat park wedges the boot. */
        } else {
            c.step(kCoreSlice);
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
            g_in.pushBlock(z);              /* one block of zero input primes it */
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
            continue;                       /* nobody listening; keep the pace */
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
void stepPair(ot::Core &c, unsigned n)
{
    for (unsigned k = 0; k < n; k += kCoreSlice) {
        c.step(kCoreSlice);
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
            o.step(kCoreSlice);
        }
    }
}

ot::StepFn drainStep(ot::Core &c)
{
    return [&c] { stepPair(c, 64); };
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
    audioListen(audio_path);
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
        if (s.argPhase == 0) {
            s.lastDest = w & 0xFFFF;
        }
        s.argPhase++;
    }
    for (unsigned g = 0;
         g < 100000 && c.hdi().rxData().size() >= ot::kFifoDepth && !c.dead; g++) {
        s.writeStalls++;
        stepPair(c, 256);
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
            stepPair(c, 64);
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
    while (halves) {
        unsigned n = halves > 1024 ? 1024 : halves;

        for (unsigned i = 0; i < n; i++) {
            w[i] = (txh << 16) | ((TWord)be[2 * i] << 8) | be[2 * i + 1];
        }
        /* Drain a full FIFO inline, stepping BOTH cores: the mailbox coupling
         * wedges if only the receiver runs. */
        for (unsigned g = 0;
             g < 100000 && c.hdi().rxData().size() >= ot::kFifoDepth && !c.dead;
             g++) {
            s.writeStalls++;
            stepPair(c, 256);
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
                stepPair(c, 64);
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
