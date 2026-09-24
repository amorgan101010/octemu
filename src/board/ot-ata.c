/*
 * Elektron Octatrack — MCF54455 on-chip ATA controller + CompactFlash card.
 *
 * Register map from MCF54455RM Table 23-2; behaviour read-disasm from the
 * firmware's storage driver. Three firmware traps shape this model: the ISR
 * HALTs the CPU if BSY is still set when it runs, and HALTs if ERR is set for
 * any command that is not 0xC8/0xCA/0xEF — so completion interrupts are
 * delayed off the register write and ERR is never set. A sector count of 0
 * means 256. The interface runs big-endian: a data-port read returns bytes at
 * the current sector offset in disk order, so the sector lands in guest memory
 * byte-exact; IDENTIFY's little-endian numeric fields are serialised in wire
 * order because the ISR's 0xEC path byte-swaps them.
 *
 * PIO only: IDENTIFY word 49 bit 8 is clear, so the firmware's factory builds
 * the PIO-only vtable and never enqueues READ/WRITE DMA. The guest moves every
 * sector by programmed I/O — a 4x-unrolled loop of `movew 0x900000a0,%d0` at
 * 0x4001546c, 256 halfword reads per sector, at IPL 5 — which is why card
 * traffic turns into stalled audio blocks and why the counters below are worth
 * keeping.
 *
 * ☠ Page-sizing this window was TRIED AND IS WORTHLESS — do not redo it. It is
 * the hottest MMIO on the board while the card moves (5.1 million accesses
 * inside the 102 stalled audio blocks of one 30 s window), so it looks like
 * the obvious case for the subpage fix that helped the DSP/eDMA/DTIM windows.
 * Interleaved A/B in one build: 0.867x / 0.882x page-sized against 0.871x /
 * 0.903x subpage — no effect. The ~490 ns each access really costs is the
 * guest's four TCG instructions per halfword plus the io path, not the subpage
 * indirection. The win is to stop FREEZING THE DSP across card I/O.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "qom/object.h"
#include "qemu/main-loop.h"
#include "exec/ot-insn-budget.h"
#include "ot-qemu.h"

#define OT_ATA_SIZE         0x1000         /* a whole target page */
#define OT_SECTOR           512

#define ATA_TIME_CYC        0x17
#define ATA_CR              0x24
#define ATA_ISR             0x28
#define ATA_IER             0x2c
#define ATA_ICR             0x30
#define ATA_FIFO_ALARM      0x34
#define ATA_DRIVE_DATA      0xa0
#define ATA_DRIVE_FEATURES  0xa4
#define ATA_DRIVE_SECCOUNT  0xa8
#define ATA_DRIVE_LBA_LOW   0xac
#define ATA_DRIVE_LBA_MID   0xb0
#define ATA_DRIVE_LBA_HIGH  0xb4
#define ATA_DRIVE_DEV_HEAD  0xb8
#define ATA_DRIVE_STATUS    0xbc
#define ATA_DRIVE_ALT_STATUS 0xd8

#define ST_DRDY             0x40
#define ST_DSC              0x10
#define ST_DRQ              0x08
#define ST_IDLE             (ST_DRDY | ST_DSC)

#define CR_RESET            0x40           /* ata_rst_b, active low */
#define IER_INT             0x08

#define CMD_REQUEST_SENSE   0x03
#define CMD_READ_SECTORS    0x20
#define CMD_WRITE_SECTORS   0x30
#define CMD_TRANSLATE_SECT  0x87
#define CMD_ERASE_SECTORS   0xc0
#define CMD_READ_DMA        0xc8
#define CMD_WRITE_DMA       0xca
#define CMD_STANDBY_IMM     0xe0
#define CMD_CHECK_POWER     0xe5
#define CMD_IDENTIFY        0xec
#define CMD_SET_FEATURES    0xef

/*
 * The completion interrupt must not be raised from inside the command write:
 * the requester stores the completion-event pointer, unmasks interrupts,
 * writes the command and only then waits. An inline INTRQ runs the ISR BEFORE
 * that wait is armed; the ISR signals the event and then NULLS the pointer, so
 * the requester re-reads zero and blocks on a null event forever — the card is
 * never mounted, so the boot never reaches LOADING FILES. See the gate below
 * for the exact instruction sequence; the Octatrack stays fully alive, which is
 * what makes this look like a UI freeze rather than a storage failure.
 *
 * ☠ ZERO IS NOT ENOUGH, and it used to be zero. The reasoning for 0 was that
 * a QEMU_CLOCK_VIRTUAL timer at deadline "now" still fires at the next TB
 * boundary, i.e. after the guest's in-flight instruction — which is true, and
 * is not the same as "after the guest has armed its wait". The main loop can
 * service that deadline before the vCPU makes any further progress, and
 * patches/qemu/0010 (which spins the main loop for sub-millisecond deadlines)
 * makes that MORE likely, not less. It is a race, and it was measured losing:
 * with a windowed frontend competing for the host, 2 boots in 9 wedged with
 * exactly the signature above — IDENTIFY issued, no further ATA command ever,
 * sectors_r=0.
 *
 * AND IT IS BOUNDED ABOVE, by the audio path. This same timer completes the
 * READ commands that stream a STATIC sample during playback, so the delay sits
 * on the audio-critical path. 100 us was tried first and BROKE PLAYBACK: the
 * TRIG9 fixture went from a reliable 3 clean bursts to 1, 5 and 1 across three
 * runs, with ship_nz swinging 2757..47674 against its canonical 8271.
 *
 * ☠ AND 10 us DOES NOT CLOSE IT EITHER — this comment used to claim it did, on
 * 12 of 12 clean boots. A larger sample says otherwise: 1 of 16, then 2 of 32
 * windowed boots still wedge with exactly this signature. 12 of 12 was luck,
 * not evidence; at a ~6% rate a clean run of 12 has better than even odds.
 *
 * So this constant is now only the FIRST POLL of a state gate, not the fix.
 * What actually closes the race is ot_ata_requester_parked() below, which holds
 * INTRQ until the guest is demonstrably waiting. The value still matters a
 * little — it is the delay before the first gate check, and it is bounded above
 * by the audio path: 100 us was tried and BROKE PLAYBACK (the TRIG9 fixture
 * went from a reliable 3 clean bursts to 1, 5 and 1, ship_nz swinging
 * 2757..47674 against its canonical 8271). Leave it at 10 us.
 *
 * This timer is NOT on the write path. Write completions have their own
 * hazard — the issue routine at 0x40014848 IPL-7-protects only the command
 * write, and its shared sector counter is decremented later at task IPL, so no
 * fixed latency is safe there at all — and they are delivered instead by
 * ot_ata_wr_fire once the guest's own counter has caught up. Do not merge the
 * two: a 20 us delay on the write path lands inside the next request's
 * pre-IPL7 window and the ISR spins on DRQ at 0x4001551e forever.
 */
/* was: #define OT_ATA_IRQ_LATENCY 10000 (ns of virtual time) — see OT_ATA_IRQ_INSN */
/*
 * ☠ Now counted in GUEST instructions and delivered from ot_ata_progress(),
 * for the same reason as the write path below (OT_WR_BACKSTOP_NS): a 10 us
 * QEMU_CLOCK_VIRTUAL timer is host time, and on Linux each one cost a
 * main-loop wakeup contending the BQL with an MMIO-heavy vCPU. Measured: a
 * project load read 19735 sectors at ~400 us of GUEST time each (one audio
 * block per sector, ~8 s to load), which also starves STATIC streaming.
 * 10 us at the emulated ColdFire's ~132 insns/us is ~1320 instructions; the
 * hook runs every `interleave` (512) retired, so delivery lands 1024-1535
 * instructions after the command — 8-12 us of guest time, on any host.
 */
#define OT_ATA_IRQ_INSN     1024
#define OT_ATA_BACKSTOP_NS  1000000        /* 1 ms: only if the guest idles */
/*
 * WRITE completions are the hard case. The issue routine (0x40014848,
 * OS 1.40C) IPL-7-protects only the command write; the 512-byte push loop and
 * the shared sector counter's decrement (0x4001494a) run at task IPL with no
 * device access in between. On silicon the card's program time (~100 us+)
 * guarantees INTRQ lands after that bookkeeping; in emulation NO fixed latency
 * is safe. So the device holds the completion INTRQ until the guest's own
 * counter matches its transfer state — the emulation equivalent of "program
 * time exceeds the driver's bookkeeping".
 */
#define OT_FW_SECTORS_LEFT  0x46c8c592     /* OS 1.40C ata_sectors_left */
/*
 * ☠ POLLED ON GUEST PROGRESS, like the eDMA gate — not on a fast host timer.
 * The original 2 us QEMU_CLOCK_VIRTUAL poll woke the main loop ~500k times a
 * second, and every wakeup takes the BQL. The frame ISR touches MMIO every
 * few instructions and each access needs that same lock; on Linux (unfair
 * futex mutexes) the vCPU then lost nearly every handoff and ran ~0.5M
 * guest insns/s instead of ~100M. The guest never got back to task level to
 * decrement the counter, the 25000-poll cap fired after ~1 s of host time,
 * and the forced INTRQ wedged the driver (a project save = ~8.6k writes, one
 * of which always lost). Measured: in the stall window the DSP interleave
 * hook fired ZERO times, i.e. < 512 guest instructions retired in 950 ms.
 *
 * So ot_ata_progress() checks the counter every `interleave` retired guest
 * instructions on the vCPU thread (no BQL until it delivers), the timer is a
 * slow backstop for a guest that stops retiring instructions, and the escape
 * is counted in GUEST instructions, which a slow or contended host cannot
 * burn through.
 */
#define OT_WR_BACKSTOP_NS   1000000        /* 1 ms: only if the guest idles */
#define OT_WR_MAX_INSN      20000000ULL    /* ~150 ms of guest time, then deliver */

/*
 * READ/IDENTIFY completions have the same shape of hazard, and NO delay closes
 * it either — which is why this gates on guest state rather than on time.
 *
 * The requester (0x400159c0 for IDENTIFY, read-disasm on OS 1.40C):
 *
 *     40015a34   movel %d0,0x46c8c598      publish the completion event
 *     40015a46   movew %d4,%sr             interrupts back ON
 *     40015a4a   moveb #0xEC,STATUS        the command        <- window opens
 *     40015a50   movel 0x46c8c598,%d0      RE-READ the event pointer
 *     40015a58   jsr 0x40000818            wait on it         <- window closes
 *
 * and the ISR's completion path (0x400155f4):
 *
 *     400155fc   movel 0x46c8c598,%d0      signal it via 0x40000968
 *     40015612   clrl 0x46c8c598           *** and NULL it ***
 *
 * So an INTRQ that lands inside those four instructions makes the requester
 * re-read a pointer the ISR has just zeroed and call the wait primitive with
 * NULL. 0x40000818 then dereferences address 0, finds a value <= 0, parks the
 * task forever and writes its task pointer to address 4. The card is never
 * mounted, the boot never reaches LOADING FILES, and the Octatrack otherwise
 * runs perfectly — LEDs, panel and audio all alive. ✓ MEASURED at ~6% of
 * windowed boots (2 of 32): one 0xEC command, 256 data words, 285 register
 * accesses in the whole run, sectors_r=0.
 *
 * ☠ A LONGER LATENCY IS NOT A FIX, and a shorter one is not either. The
 * predecessor project measured latency 0 mounting reliably, because a
 * zero-deadline QEMU_CLOCK_VIRTUAL timer still fires at the next TB boundary,
 * by which point the requester has reached its wait. That is not true here:
 * patches/qemu/0010 spins the main loop for sub-millisecond deadlines, so the
 * deadline can be serviced before the vCPU retires another instruction. And
 * with icount off, QEMU_CLOCK_VIRTUAL tracks the HOST clock, so "10 us" buys a
 * number of guest instructions that varies with host load — which is exactly
 * why the failure is intermittent and load-dependent.
 *
 * The gate: 0x40000818 registers the waiting task at event+4 (movel %a0,%a1@(4))
 * before it traps, so "the requester is parked" is directly observable. Hold
 * INTRQ until it is, poll in virtual time, and keep a bounded escape so a
 * request that never parks degrades to the old behaviour instead of hanging.
 */
#define OT_FW_ATA_EVENT     0x46c8c598     /* OS 1.40C ata_completion_event  */
#define OT_FW_EVENT_WAITER  4              /* event+4: the parked task, or 0 */
/*
 * The escape is a SAFETY VALVE, not a tuning knob, and it must be generous
 * for the same reason the latency could never be tuned: with icount off,
 * QEMU_CLOCK_VIRTUAL tracks the HOST clock, so a cap in virtual time buys a
 * number of guest instructions that collapses under host load. At ~5 ms this
 * valve FIRED and re-created the exact wedge it exists to prevent, one command
 * later than the original: ✓ measured, 1 of 48 boots, cap[0xef]=1, IDENTIFY
 * already past and SET_FEATURES hung with sectors_r=0. Matched to the write
 * path's ~50 ms.
 *
 * At ~50 ms, gate_caps across 80 windowed boots was 1, on 0xE0 STANDBY_IMM at
 * teardown, in a run that booted fine — that command evidently does not park
 * either. A cap there is harmless: after 50 ms of virtual time the requester
 * has either parked long ago (safe to deliver) or never parks at all (nothing
 * to lose), so the escape cannot recreate the wedge. A cap on any command that
 * DOES park is a bug — it means the gate mismodels that command's wait, which
 * is exactly how the 5 ms version reintroduced the hang at 0xEF. That is what
 * the per-command cap histogram in the exit report is for.
 */
#define OT_ATA_GATE_MAX_INSN 6600000ULL  /* ~50 ms of guest time, then deliver */

typedef enum { XFER_NONE, XFER_TO_HOST, XFER_FROM_HOST } OTAtaXfer;

#define TYPE_OCTATRACK_ATA "octatrack-ata"
OBJECT_DECLARE_SIMPLE_TYPE(OTAta, OCTATRACK_ATA)

struct OTAta {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    BlockBackend *blk;

    bool card_present;
    uint64_t nsectors;

    uint8_t timing[ATA_TIME_CYC + 1];      /* write-only scratch, reads back */
    uint8_t cr, ier, fifo_alarm;

    uint8_t features, error, seccount;
    uint8_t lba_low, lba_mid, lba_high, dev_head;
    uint8_t status;

    uint8_t buf[OT_SECTOR];
    int buf_pos;
    OTAtaXfer xfer;
    uint32_t xfer_lba;
    unsigned xfer_left;

    bool intrq;                            /* drive INTRQ line */
    bool irq_level;                        /* CPU-visible (IER-gated) level */
    QEMUTimer *irq_timer;                  /* backstop only; see OT_ATA_IRQ_INSN */
    bool irq_due;                          /* a completion INTRQ awaits delivery */
    uint64_t irq_due_at;                   /* ot_insn_retired() it may land at */
    uint64_t irq_seen;                     /* retired count at the last backstop */
    bool wr_pending;                       /* write completion awaiting delivery */
    uint64_t wr_at;                        /* ot_insn_retired() when it went pending */
    unsigned gate_polls;                   /* completion-gate polls this cmd */
    bool gate_armed;                       /* gate this command's completion  */
    QEMUTimer *wr_timer;
};

static OTAta *ot_ata_singleton;

/* DIAG: OCTA_ATA_LOG=1 logs read commands and completions with retired
 * guest-instruction stamps. */
static bool ot_ata_log(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("OCTA_ATA_LOG") != NULL;
    }
    return on;
}

/* Structural counters. Sector counts and MMIO access counts are fixed by the
 * firmware's PIO loop, so they say the same thing under any host load. */
static uint64_t ot_ata_reads, ot_ata_writes, ot_ata_mmio, ot_ata_reread;
/* Completion-gate health: how often delivery had to wait for the requester to
 * park, and how often the bounded escape fired. A nonzero cap count means the
 * gate is guessing and the audio path is paying for it. */
static uint64_t ot_ata_gate_polls, ot_ata_gate_caps;
/* Which pending command was in flight when the gate gave up. A gate that caps
 * is guessing, so this says WHICH command's wait pattern the gate mismodels. */
static uint64_t ot_ata_gate_cap_cmd[256];
/* One bit per sector of the first 512 MB of card. A high reread ratio means a
 * model-side sector cache would pay; a low one means genuine streaming and a
 * cache is dead weight. Costs 128 KB and answers the question in one run. */
static uint8_t *ot_ata_seen;

/* Is a sector transfer in flight? The DSP shim uses it to attribute the
 * straddles it counts: on silicon the eDMA carries a control-block delivery
 * through a card transfer, here the minors are guest stores and the CPU is at
 * IPL 5 inside the 4x-unrolled sector loop. */
int ot_ata_busy(void)
{
    return ot_ata_singleton && ot_ata_singleton->xfer != XFER_NONE;
}

void ot_ata_stats(char *buf, size_t len)
{
    snprintf(buf, len, "ata sectors_r=%llu sectors_w=%llu mmio=%llu reread=%llu "
             "gate_polls=%llu gate_caps=%llu",
             (unsigned long long)ot_ata_reads, (unsigned long long)ot_ata_writes,
             (unsigned long long)ot_ata_mmio, (unsigned long long)ot_ata_reread,
             (unsigned long long)ot_ata_gate_polls,
             (unsigned long long)ot_ata_gate_caps);
    /* Only ever printed when the gate gave up, which should be never. */
    for (unsigned c = 0; c < 256; c++) {
        size_t o = strlen(buf);

        if (ot_ata_gate_cap_cmd[c] && o + 24 < len) {
            snprintf(buf + o, len - o, " cap[%#04x]=%llu", c,
                     (unsigned long long)ot_ata_gate_cap_cmd[c]);
        }
    }
}

/* Drive INTRQ and the CPU-visible line are distinct: IER masks delivery but
 * must not LOSE a pending INTRQ — unmasking delivers it. */
static void ot_ata_update_irq(OTAta *s)
{
    bool gated = s->intrq && (s->ier & IER_INT);

    if (gated != s->irq_level) {
        s->irq_level = gated;
        qemu_set_irq(s->irq, gated);
    }
}

static void ot_ata_set_irq(OTAta *s, bool level)
{
    if (!level) {
        s->irq_due = false;                /* an ack cancels an unlanded raise */
        timer_del(s->irq_timer);
    }
    s->intrq = level;
    ot_ata_update_irq(s);
}

/*
 * True once the requester is demonstrably parked on the completion event, or
 * when there is nothing to synchronise with.
 *
 * A null ata_completion_event means the request is fire-and-forget (the queue
 * record's event field is 0 when its flags bit 1 is set), so nobody can miss
 * the signal and delivery is unconditionally safe. The ISR also zeroes this
 * word after signalling, so between requests it reads 0 and the gate is open.
 */
static bool ot_ata_requester_parked(void)
{
    uint8_t buf[4];
    uint32_t event, waiter;

    address_space_read(&address_space_memory, OT_FW_ATA_EVENT,
                       MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
    event = ldl_be_p(buf);
    if (!event) {
        return true;
    }
    address_space_read(&address_space_memory, event + OT_FW_EVENT_WAITER,
                       MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
    waiter = ldl_be_p(buf);
    return waiter != 0;
}

/*
 * BQL held. Deliver a due completion once its latency has elapsed in guest
 * instructions and, for gated commands, the requester is parked. `idle` is
 * the backstop's verdict that the guest retired nothing for a whole backstop
 * period: then nothing is racing us, so deliver. Returns whether it delivered.
 */
static bool ot_ata_irq_try(OTAta *s, bool idle)
{
    const uint64_t now = ot_insn_retired();
    if (!s->irq_due) {
        return true;
    }
    if (!idle && now < s->irq_due_at) {
        return false;
    }
    if (!idle && s->gate_armed && !ot_ata_requester_parked()) {
        if (now - s->irq_due_at < OT_ATA_GATE_MAX_INSN) {
            s->gate_polls++;
            ot_ata_gate_polls++;
            return false;
        }
        /* Bounded escape: a request that never parks must degrade to the old
         * timing-based behaviour, not hang the device. */
        uint8_t cmd = 0;
        ot_ata_gate_caps++;
        address_space_read(&address_space_memory, OT_FW_ATA_EVENT - 5,
                           MEMTXATTRS_UNSPECIFIED, &cmd, 1);
        ot_ata_gate_cap_cmd[cmd]++;
    }
    s->irq_due = false;
    timer_del(s->irq_timer);
    ot_ata_set_irq(s, true);
    return true;
}
/* The backstop timer, main loop, BQL held. */
static void ot_ata_irq_fire(void *opaque)
{
    OTAta *s = opaque;
    const uint64_t now = ot_insn_retired();
    const bool idle = now == s->irq_seen;
    s->irq_seen = now;
    if (!ot_ata_irq_try(s, idle)) {
        timer_mod(s->irq_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OT_ATA_BACKSTOP_NS);
    }
}
static void ot_ata_set_irq_later(OTAta *s)
{
    s->gate_polls = 0;
    /* READ_SECTORS never parks: measured, every single gate timeout across six
     * boots was cap[0x20] and nothing else (9-20 per boot, and those caps
     * accounted for essentially all of the ~50k polls, so every other command
     * cleared the gate on its first check). The driver spins on DRQ for reads
     * instead of blocking on an event, so there is no waiter to wait for — and
     * this is the audio-critical command, so it keeps the plain latency it has
     * always had. Every observed wedge was at IDENTIFY with sectors_r=0, before
     * any read is ever issued. */
    s->irq_due = true;
    s->irq_due_at = ot_insn_retired() + OT_ATA_IRQ_INSN;
    s->irq_seen = ot_insn_retired();
    timer_mod(s->irq_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OT_ATA_BACKSTOP_NS);
}

/* Has the guest's own sector counter caught up with the device? RAM read,
 * safe without the BQL. */
static bool ot_ata_wr_caught_up(OTAta *s, uint8_t *left)
{
    address_space_read(&address_space_memory, OT_FW_SECTORS_LEFT,
                       MEMTXATTRS_UNSPECIFIED, left, 1);
    return *left == (s->xfer_left & 0xff);
}
/* BQL held. Deliver if the counter caught up or the escape expired; returns
 * whether it delivered. */
static bool ot_ata_wr_try(OTAta *s)
{
    uint8_t left;
    if (!s->wr_pending) {
        return true;
    }
    if (!ot_ata_wr_caught_up(s, &left)) {
        if (ot_insn_retired() - s->wr_at < OT_WR_MAX_INSN) {
            return false;
        }
        warn_report("octatrack-ata: write INTRQ forced after %llu guest "
                    "instructions (guest=%u dev=%u)",
                    (unsigned long long)OT_WR_MAX_INSN, left, s->xfer_left);
    }
    s->wr_pending = false;
    timer_del(s->wr_timer);
    ot_ata_set_irq(s, true);
    return true;
}
/* The backstop timer, main loop, BQL held. */
static void ot_ata_wr_fire(void *opaque)
{
    OTAta *s = opaque;
    if (!ot_ata_wr_try(s)) {
        timer_mod(s->wr_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OT_WR_BACKSTOP_NS);
    }
}
/* Guest progress, vCPU thread, no BQL held (see ot_guest_progress). */
void ot_ata_progress(void)
{
    OTAta *s = ot_ata_singleton;
    uint8_t left;
    if (!s) {
        return;
    }
    /* Cheap unlocked pre-checks; both paths re-check under the BQL. */
    if (qatomic_read(&s->irq_due) &&
        ot_insn_retired() >= qatomic_read(&s->irq_due_at)) {
        bql_lock();
        ot_ata_irq_try(s, false);
        bql_unlock();
    }
    if (qatomic_read(&s->wr_pending) &&
        (ot_ata_wr_caught_up(s, &left) ||
         ot_insn_retired() - s->wr_at >= OT_WR_MAX_INSN)) {
        bql_lock();
        ot_ata_wr_try(s);
        bql_unlock();
    }
}
/* ATA strings: space padded, first character in the high byte. */
static void ot_ata_str(uint16_t *id, int word, int words, const char *str)
{
    size_t len = strlen(str);

    for (int i = 0; i < words; i++) {
        uint8_t a = (size_t)(2 * i)     < len ? (uint8_t)str[2 * i]     : ' ';
        uint8_t b = (size_t)(2 * i + 1) < len ? (uint8_t)str[2 * i + 1] : ' ';

        id[word + i] = ((uint16_t)a << 8) | b;
    }
}

static void ot_ata_build_identify(OTAta *s)
{
    uint16_t id[256];
    uint32_t lba28 = s->nsectors > 0x0fffffff ? 0x0fffffff : s->nsectors;
    uint32_t cyls = lba28 / (16 * 63);

    memset(id, 0, sizeof(id));
    id[0] = 0x848a;                        /* CompactFlash signature */
    id[1] = cyls > 16383 ? 16383 : cyls;
    id[3] = 16;
    id[6] = 63;
    ot_ata_str(id, 10, 10, "OCTAEMU-QEMU-CF-0001");
    ot_ata_str(id, 23, 4, "QEMU0001");
    ot_ata_str(id, 27, 20, "QEMU Octatrack CompactFlash");
    id[47] = 0x8001;
    id[49] = (1 << 9) | (1 << 11);         /* LBA + IORDY, NO DMA (bit 8) */
    id[50] = 0x4000;
    id[51] = 0x0200;                       /* PIO mode 2 */
    id[53] = 0x0003;                       /* words 54-58, 64-70 valid; not 88 */
    id[54] = id[1];
    id[55] = id[3];
    id[56] = id[6];
    id[57] = lba28 & 0xffff;
    id[58] = lba28 >> 16;
    id[59] = 0x0101;
    id[60] = lba28 & 0xffff;               /* LBA28 capacity */
    id[61] = lba28 >> 16;
    id[64] = 0x0003;                       /* PIO modes 3 and 4 */
    id[67] = 120;
    id[68] = 120;
    id[80] = 0x0070;                       /* ATA-4/5/6 */

    /* Wire order: low byte first; the ISR's 0xEC path swaps each word back. */
    for (int i = 0; i < 256; i++) {
        s->buf[2 * i]     = id[i] & 0xff;
        s->buf[2 * i + 1] = id[i] >> 8;
    }
}

static uint32_t ot_ata_taskfile_lba(OTAta *s)
{
    return ((uint32_t)(s->dev_head & 0x0f) << 24)
         | ((uint32_t)s->lba_high << 16)
         | ((uint32_t)s->lba_mid << 8)
         |  (uint32_t)s->lba_low;
}

static void ot_ata_fetch_sector(OTAta *s, uint32_t lba)
{
    if (!ot_ata_seen) {
        ot_ata_seen = g_malloc0(1u << 17);
    }
    if (lba < (1u << 20)) {
        if (ot_ata_seen[lba >> 3] & (1u << (lba & 7))) {
            ot_ata_reread++;
        } else {
            ot_ata_seen[lba >> 3] |= 1u << (lba & 7);
        }
    }
    memset(s->buf, 0, OT_SECTOR);
    if (s->blk &&
        blk_pread(s->blk, (int64_t)lba * OT_SECTOR, OT_SECTOR, s->buf, 0) < 0) {
        qemu_log_mask(LOG_UNIMP, "ATA  read LBA %u FAILED\n", lba);
    }
    ot_ata_reads++;
}

static void ot_ata_complete_nodata(OTAta *s)
{
    s->xfer = XFER_NONE;
    s->buf_pos = 0;
    s->xfer_left = 0;
    s->status = ST_IDLE;
    ot_ata_set_irq_later(s);
}

/* Present one sector: DRQ up, INTRQ inline at a data-block boundary (as
 * silicon: within 400 ns), via the timer at command time (mount hazard). */
static void ot_ata_present_read_sector(OTAta *s, bool inline_irq)
{
    ot_ata_fetch_sector(s, s->xfer_lba);
    s->buf_pos = 0;
    s->xfer = XFER_TO_HOST;
    s->status = ST_IDLE | ST_DRQ;
    if (inline_irq) {
        ot_ata_set_irq(s, true);
    } else {
        ot_ata_set_irq_later(s);
    }
}

static void ot_ata_command(OTAta *s, uint8_t cmd)
{
    uint32_t lba = ot_ata_taskfile_lba(s);
    unsigned count = s->seccount ? s->seccount : 256;   /* 0 means 256 */

    /* ATA spec: INTRQ is negated by a Command register write (and by a Status
     * read, and by completion of a data block transfer). This firmware acks
     * almost exclusively through the latter two — it reads ALT_STATUS in its
     * polls, so a model that only clears on STATUS reads holds the level, the
     * IPL-5 ISR re-enters between commands, and at a write-queue end its
     * sector counter wraps and it spins on DRQ at 0x4001551e forever. */
    ot_ata_set_irq(s, false);
    s->wr_pending = false;
    timer_del(s->wr_timer);
    s->gate_armed = (cmd != CMD_READ_SECTORS);

    s->error = 0;
    if (!s->card_present) {
        return;                            /* the dispatcher refuses these */
    }

    switch (cmd) {
    case CMD_READ_SECTORS:
        if (ot_ata_log()) {                                    /* DIAG */
            fprintf(stderr, "ATARD cmd lba=%u n=%u ret=%llu blk=%llu\n", lba, count,
                    (unsigned long long)ot_insn_retired(),
                    (unsigned long long)ot_dsp_blocks());
        }
        s->xfer_lba = lba;
        s->xfer_left = count;
        ot_ata_present_read_sector(s, false);
        break;
    case CMD_TRANSLATE_SECT:
        s->xfer_lba = lba;
        s->xfer_left = 1;
        memset(s->buf, 0, OT_SECTOR);
        s->buf_pos = 0;
        s->xfer = XFER_TO_HOST;
        s->status = ST_IDLE | ST_DRQ;
        ot_ata_set_irq_later(s);
        break;
    case CMD_IDENTIFY:
        ot_ata_build_identify(s);
        s->buf_pos = 0;
        s->xfer = XFER_TO_HOST;
        s->xfer_left = 1;
        s->xfer_lba = 0;
        s->status = ST_IDLE | ST_DRQ;
        ot_ata_set_irq_later(s);
        break;
    case CMD_WRITE_SECTORS:
        /* PIO write: DRQ for the first block WITHOUT an interrupt — the
         * handler pushes it inline after spinning on ALT_STATUS DRQ. */
        s->xfer_lba = lba;
        s->xfer_left = count;
        s->buf_pos = 0;
        s->xfer = XFER_FROM_HOST;
        s->status = ST_IDLE | ST_DRQ;
        break;
    case CMD_CHECK_POWER:
        s->seccount = 0xff;                /* active/idle */
        ot_ata_complete_nodata(s);
        break;
    case CMD_STANDBY_IMM:
        if (s->blk) {
            blk_flush(s->blk);
        }
        ot_ata_complete_nodata(s);
        break;
    case CMD_SET_FEATURES:
    case CMD_REQUEST_SENSE:
        ot_ata_complete_nodata(s);
        break;
    case CMD_ERASE_SECTORS:
        if (s->blk) {
            memset(s->buf, 0, OT_SECTOR);
            for (unsigned i = 0; i < count; i++) {
                blk_pwrite(s->blk, (int64_t)(lba + i) * OT_SECTOR,
                           OT_SECTOR, s->buf, 0);
            }
        }
        ot_ata_complete_nodata(s);
        break;
    case CMD_READ_DMA:
    case CMD_WRITE_DMA:
        /* Advertised unsupported; do not complete — a blocked completion event
         * is diagnosable, ERR would halt the ISR. */
        qemu_log_mask(LOG_UNIMP, "ATA  CMD %#04x is DMA, not modelled\n", cmd);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ATA  CMD %#04x unknown, completing\n", cmd);
        ot_ata_complete_nodata(s);
        break;
    }
}

static uint64_t ot_ata_data_read(OTAta *s, unsigned size)
{
    uint64_t val = 0;

    if (s->xfer != XFER_TO_HOST) {
        return 0;
    }
    for (unsigned i = 0; i < size; i++) {   /* big-endian: disk order */
        val = (val << 8) | s->buf[s->buf_pos + i];
    }
    s->buf_pos += size;
    if (s->buf_pos >= OT_SECTOR) {
        s->buf_pos = 0;
        s->xfer_left--;
        s->xfer_lba++;
        ot_ata_set_irq(s, false);      /* data block complete negates INTRQ */
        if (s->xfer_left > 0) {
            ot_ata_present_read_sector(s, true);
        } else {
            s->xfer = XFER_NONE;
            s->status = ST_IDLE;
            if (ot_ata_log()) {                                /* DIAG */
                fprintf(stderr, "ATARD done ret=%llu blk=%llu\n",
                        (unsigned long long)ot_insn_retired(),
                        (unsigned long long)ot_dsp_blocks());
            }
        }
    }
    return val;
}

static void ot_ata_data_write(OTAta *s, uint64_t val, unsigned size)
{
    if (s->xfer != XFER_FROM_HOST) {
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        s->buf[s->buf_pos + i] = (val >> (8 * (size - 1 - i))) & 0xff;
    }
    s->buf_pos += size;
    if (s->buf_pos < OT_SECTOR) {
        return;
    }
    if (s->blk &&
        blk_pwrite(s->blk, (int64_t)s->xfer_lba * OT_SECTOR, OT_SECTOR,
                   s->buf, 0) < 0) {
        qemu_log_mask(LOG_UNIMP, "ATA  write LBA %u FAILED\n", s->xfer_lba);
    }
    ot_ata_writes++;
    s->buf_pos = 0;
    s->xfer_left--;
    s->xfer_lba++;
    if (s->xfer_left > 0) {
        s->status = ST_IDLE | ST_DRQ;
    } else {
        s->xfer = XFER_NONE;
        s->status = ST_IDLE;
        if (s->blk) {
            blk_flush(s->blk);
        }
    }
    ot_ata_set_irq(s, false);          /* data block complete negates INTRQ */
    s->wr_pending = true;              /* raise once the guest's counter has */
    s->wr_at = ot_insn_retired();      /* caught up; see OT_FW_SECTORS_LEFT  */
    ot_ata_wr_fire(s);
}

static uint64_t ot_ata_read(void *opaque, hwaddr addr, unsigned size)
{
    OTAta *s = opaque;

    ot_ata_mmio++;
    if (addr <= ATA_TIME_CYC) {
        return s->timing[addr];
    }
    switch (addr) {
    case ATA_CR:         return s->cr;
    case ATA_ISR:        return 0x10 | (s->irq_level ? 0x08 : 0);
    case ATA_IER:        return s->ier;
    case ATA_FIFO_ALARM: return s->fifo_alarm;
    case ATA_DRIVE_DATA: return ot_ata_data_read(s, size);
    case ATA_DRIVE_FEATURES: return s->error;
    case ATA_DRIVE_SECCOUNT: return s->seccount;
    case ATA_DRIVE_LBA_LOW:  return s->lba_low;
    case ATA_DRIVE_LBA_MID:  return s->lba_mid;
    case ATA_DRIVE_LBA_HIGH: return s->lba_high;
    case ATA_DRIVE_DEV_HEAD: return s->dev_head;
    case ATA_DRIVE_STATUS:
        ot_ata_set_irq(s, false);      /* reading STATUS acknowledges INTRQ */
        return s->status;
    case ATA_DRIVE_ALT_STATUS:
        return s->status;              /* ...ALT_STATUS does not */
    default:
        return 0;
    }
}

static void ot_ata_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    OTAta *s = opaque;

    ot_ata_mmio++;
    if (addr <= ATA_TIME_CYC) {
        s->timing[addr] = val;
        return;
    }
    switch (addr) {
    case ATA_CR:
        /* Bit 6 is drive reset, active low; on release, a clean task file. */
        if (!(s->cr & CR_RESET) && (val & CR_RESET)) {
            s->status = ST_IDLE;
            s->xfer = XFER_NONE;
            s->buf_pos = 0;
            s->xfer_left = 0;
            s->error = 0;
            ot_ata_set_irq(s, false);
        }
        s->cr = val;
        break;
    case ATA_IER:
        s->ier = val;
        ot_ata_update_irq(s);
        break;
    case ATA_ICR:
        /* Controller-level write-1-to-clear. The firmware acks INTRQ HERE, not
         * by reading DRIVE_STATUS (measured: zero STATUS reads across a whole
         * project-creation write burst) — dropping this write leaves the level
         * high, the ISR re-enters, its per-command sector counter wraps 0->255
         * and it spins on DRQ at 0x4001551e forever at IPL 5, which also
         * silences every lower-priority task (keys, UI). */
        if (val & IER_INT) {
            ot_ata_set_irq(s, false);
        }
        break;
    case ATA_FIFO_ALARM:
        s->fifo_alarm = val;
        break;
    case ATA_DRIVE_DATA:     ot_ata_data_write(s, val, size); break;
    case ATA_DRIVE_FEATURES: s->features = val; break;
    case ATA_DRIVE_SECCOUNT: s->seccount = val; break;
    case ATA_DRIVE_LBA_LOW:  s->lba_low = val; break;
    case ATA_DRIVE_LBA_MID:  s->lba_mid = val; break;
    case ATA_DRIVE_LBA_HIGH: s->lba_high = val; break;
    case ATA_DRIVE_DEV_HEAD: s->dev_head = val; break;
    case ATA_DRIVE_STATUS:   ot_ata_command(s, val & 0xff); break;
    default:
        break;
    }
}

static const MemoryRegionOps ot_ata_ops = {
    .read = ot_ata_read,
    .write = ot_ata_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

uint8_t ot_ata_ppdsdr_uart(void)
{
    OTAta *s = ot_ata_singleton;

    return (s && s->card_present) ? 0x00 : 0x08;
}

static const Property octatrack_ata_properties[] = {
    DEFINE_PROP_DRIVE("drive", OTAta, blk),
};

static void octatrack_ata_realize(DeviceState *dev, Error **errp)
{
    OTAta *s = OCTATRACK_ATA(dev);

    s->status = ST_IDLE;
    s->dev_head = 0xe0;
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ot_ata_irq_fire, s);
    s->wr_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ot_ata_wr_fire, s);

    if (s->blk) {
        int64_t len = blk_getlength(s->blk);
        uint64_t perm = BLK_PERM_CONSISTENT_READ
                      | (blk_supports_write_perm(s->blk) ? BLK_PERM_WRITE : 0);

        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (len < OT_SECTOR) {
            error_setg(errp, "octatrack-ata: card image shorter than a sector");
            return;
        }
        s->nsectors = len / OT_SECTOR;
        s->card_present = true;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &ot_ata_ops, s,
                          "octatrack.ata", OT_ATA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void octatrack_ata_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, octatrack_ata_properties);
    dc->realize = octatrack_ata_realize;
}

static const TypeInfo octatrack_ata_info = {
    .name          = TYPE_OCTATRACK_ATA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OTAta),
    .class_init    = octatrack_ata_class_init,
};

static void octatrack_ata_register_types(void)
{
    type_register_static(&octatrack_ata_info);
}

type_init(octatrack_ata_register_types)

void ot_ata_create(MemoryRegion *sysmem, uint64_t base, struct IRQState *irq)
{
    DeviceState *dev = qdev_new(TYPE_OCTATRACK_ATA);
    DriveInfo *dinfo = drive_get(IF_IDE, 0, 0);

    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    ot_ata_singleton = OCTATRACK_ATA(dev);
    memory_region_add_subregion(sysmem, base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, (qemu_irq)irq);
}
