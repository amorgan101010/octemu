/*
 * Elektron Octatrack MKII — Freescale MCF54455 board.
 *
 * Every address and quirk cited here is read-disasm from the firmware or
 * measured against it on this emulator. Deliberately headless: UART0 (MIDI)
 * and UART2 (MK2 crossfader) are plain chardevs; UART1 is the panel link and
 * gets its own device, which answers the version handshake on the VIRTUAL
 * clock so host scheduling cannot wedge the boot. The frontend process is the
 * panel MCU for everything else.
 *
 *   -M octatrack[,mk1=on][,nvram=FILE][,dsp-audio=PATH][,throttle=off]
 *              [,capture=FILE][,interleave=N][,hold=off]
 *
 * Boot contract (read-disasm): the MAIN OS section is a raw blob loaded at
 * 0x40000400, entered as a called function with a valid SP; it reads PLL PCR
 * (fsys must be 264 MHz), a bootstrap version stamp at 0x3FFC (0x0408 takes
 * the fast path into the DSP upload), and needs low RAM, the 32 KB internal
 * SRAM at 0x80000000 (scheduler working set) and the 1 MB FlexBus CS1 SRAM at
 * 0x10000000 (battery-backed project state) all mapped.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/main-loop.h"
#include "qemu/cutils.h"
#include <sys/socket.h>
#include <sys/un.h>
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "target/m68k/cpu.h"
#include "hw/core/irq.h"
#include "hw/m68k/mcf.h"
#include "hw/core/ptimer.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "exec/ot-insn-budget.h"
#include "system/qtest.h"
#include "system/system.h"
#include "elf.h"
#include "ot-qemu.h"
#include "ot-audio-shm.h"

#define OT_SDRAM_BASE   0x40000000
#define OT_IMAGE_LOAD   0x40000400
#define OT_INIT_SP      0x47F00000
#define OT_LOWRAM_SIZE  0x00010000
#define OT_SRAM_BASE    0x80000000
#define OT_SRAM_SIZE    0x00008000
#define OT_CS1_BASE     0x10000000
#define OT_CS1_SIZE     0x00100000
#define OT_MBAR_BASE    0xFC000000
#define OT_MBAR_SIZE    0x00100000

#define OT_BOOTSTRAP_VERSION_ADDR 0x00003FFC
#define OT_BOOTSTRAP_VERSION      0x0408

#define OT_PLL_PCR      0xFC0C4000
#define OT_PCR_VALUE    0x16777731u     /* fsys = 22 * 12 MHz = 264 MHz */
#define OT_SYS_FREQ     264000000
#define OT_BUS_HZ       132000000ULL    /* fsys / 2 */

#define OT_INTC0_BASE   0xFC048000
#define OT_INTC1_BASE   0xFC04C000
#define OT_UART0_BASE   0xFC060000      /* MIDI            */
#define OT_UART1_BASE   0xFC064000      /* panel link      */
#define OT_UART2_BASE   0xFC068000      /* MK2 crossfader  */
#define OT_UART0_SRC    26              /* INTC0 sources   */
#define OT_UART1_SRC    27
#define OT_UART2_SRC    28
#define OT_EPORT_IRQ1_SRC 1             /* DSP HREQ -> frame IRQ */
#define OT_DTIM_SRC     32              /* DTIM0..3 -> INTC0 32..35 */
#define OT_EDMA_SRC     8               /* eDMA ch n -> INTC0 8+n   */
#define OT_PIT_SRC      43              /* PIT0..3 -> INTC1 43..46  */
#define OT_ATA_SRC      54              /* -> INTC1, vector 182     */
#define OT_USB_SRC      47              /* -> INTC1, vector 175: USB device
                                         * controller (fw ISR 0x4001e594,
                                         * installed at 0x4001e012)  */

/* GPIO pins answered by the MBAR catch-all. PPDSDR reflects the pin and every
 * sense line is active-low, so the default must be 0xFF, not 0. */
#define OT_PPDSDR_UART  0xFC0A4039      /* bit 3: ATA card detect, SET = none */
#define OT_PPDSDR_P10   0xFC0A403A      /* board-revision strap loopback      */
#define OT_PPDSDR_PCI   0xFC0A403C      /* bit 6: battery sense, active low   */
#define OT_PCLRR_P10    0xFC0A4052
#define OT_CORE_SELECT  0xFC0A400C      /* which DSP core the window addresses */
#define OT_P10_DRIVE    0x20
#define OT_P10_SENSE    0x40

/* ---------------------------------------------------------- block capture --
 * The per-block arms the guest sends the DSP, written out so octdsp can
 * replay them. The lab's own arms are synthetic: they keep the cores alive but
 * no VOICE renders, so nothing downstream of a playing track — AMP, the LFOs,
 * either FX slot, the master track — can be reached there. Replaying a real
 * capture puts a genuine voice in the lab, where a parameter sweep costs
 * seconds instead of a four-minute boot.
 *
 * Arms are identified by minor size, which is unambiguous: 4x336 = stream,
 * 4x64 = track records, 4x32 = the control block. The sum is not captured —
 * the guest's sum is a verbatim echo of its own readback, which the lab
 * reproduces itself with --sum-echo. Words are captured AS THE DSP RECEIVES
 * THEM, after the beat-order fixup, so a replay does not depend on it.
 */
static FILE *ot_capture_fp;
static unsigned ot_capture_blocks;

/*
 * The capture must carry PRE-TRIGGER history. A voice is set up by the
 * records a few blocks before any audio flows, and the lab boots fresh with no
 * accumulated payload state — so a capture that starts when sound appears
 * replays a voice the payload was never told about, and renders silence. The
 * arms are therefore held in a ring and flushed when the trigger fires, oldest
 * first, so the replay begins before the trig.
 */
#define OT_CAP_RING 2400

typedef struct {
    uint8_t core;
    uint16_t bytes;
    uint8_t data[1344];
} OtCapArm;

static OtCapArm ot_cap_ring[OT_CAP_RING];
static unsigned ot_cap_head, ot_cap_count;
static bool ot_cap_flushing;

static void ot_capture_emit(const OtCapArm *a)
{
    const char *kind = a->bytes == 1344 ? "stream"
                     : a->bytes == 256 ? "rec" : "ctrl";

    fprintf(ot_capture_fp, "A %u %s %u", a->core, kind, a->bytes / 2);
    for (unsigned i = 0; i < a->bytes / 2u; i++) {
        fprintf(ot_capture_fp, " %04x",
                (a->data[2 * i] << 8) | a->data[2 * i + 1]);
    }
    fputc('\n', ot_capture_fp);
    if (a->bytes == 128 && ++ot_capture_blocks >= 1200) {
        fclose(ot_capture_fp);
        ot_capture_fp = NULL;
        info_report("octatrack: DSP capture complete");
    }
}

static void ot_capture_arm(const uint8_t *buf, unsigned len, unsigned core)
{
    OtCapArm *a;

    if (ot_cap_flushing) {
        OtCapArm one;

        one.core = core;
        one.bytes = len;
        memcpy(one.data, buf, len);
        ot_capture_emit(&one);
        return;
    }
    a = &ot_cap_ring[(ot_cap_head + ot_cap_count) % OT_CAP_RING];
    if (ot_cap_count == OT_CAP_RING) {
        a = &ot_cap_ring[ot_cap_head];
        ot_cap_head = (ot_cap_head + 1) % OT_CAP_RING;
    } else {
        ot_cap_count++;
    }
    a->core = core;
    a->bytes = len;
    memcpy(a->data, buf, len);
}

static void ot_capture_trigger(void)
{
    ot_cap_flushing = true;
    for (unsigned i = 0; i < ot_cap_count && ot_capture_fp; i++) {
        ot_capture_emit(&ot_cap_ring[(ot_cap_head + i) % OT_CAP_RING]);
    }
    info_report("octatrack: DSP capture armed, %u arms of history", ot_cap_count);
}

static void ot_capture_minor(const uint8_t *buf, unsigned nbytes, unsigned core)
{
    static uint8_t acc[1344];
    static unsigned len, size, acore;
    unsigned want;

    /* A nonzero sum means a track is actually sounding: the guest's sum is its
     * own post-FX readback echoed back. That is the trigger; everything before
     * it is already in the ring. */
    if (!ot_cap_flushing && nbytes == 256) {
        for (unsigned i = 0; i < nbytes; i++) {
            if (buf[i]) {
                ot_capture_trigger();
                break;
            }
        }
    }
    if (nbytes != 336 && nbytes != 64 && nbytes != 32) {
        return;
    }
    if (len && (nbytes != size || core != acore)) {
        len = 0;
    }
    size = nbytes;
    acore = core;
    memcpy(acc + len, buf, nbytes);
    len += nbytes;
    want = nbytes == 336 ? 1344 : nbytes == 64 ? 256 : 128;
    if (len >= want) {
        ot_capture_arm(acc, len, core);
        len = 0;
    }
}

/*
 * .wire capture: when the --capture-dsp filename ends in .wire, log every
 * minor with its eDMA SOURCE address plus every host command and TX write,
 * from boot, unfiltered — the instrument that exposed the block protocol.
 * Rebuilt after the original (uncommitted) version was lost to a checkout;
 * format: "M core saddr nbytes w0..w31", "C core cmd", "T core word".
 */
static bool ot_capture_wire;

void ot_wirelog_cmd(unsigned core, unsigned val);
void ot_wirelog_cmd(unsigned core, unsigned val)
{
    if (ot_capture_fp && ot_capture_wire) {
        fprintf(ot_capture_fp, "C %u %02x\n", core, val & 0xff);
    }
}

void ot_wirelog_tx(unsigned core, unsigned word);
void ot_wirelog_tx(unsigned core, unsigned word)
{
    if (ot_capture_fp && ot_capture_wire) {
        fprintf(ot_capture_fp, "T %u %06x\n", core, word & 0xffffff);
    }
}

static void ot_wirelog_minor(const uint8_t *buf, unsigned nbytes,
                             unsigned core, uint32_t saddr)
{
    /* Log EVERY 16-bit word of the minor, not a 32-word head. The
     * audio-bearing core-1 stream is 336 bytes = 168 words; a 32-word cap kept
     * only each stream's descriptor head and DROPPED THE SAMPLES, which is why
     * a .wire replay renders silence. `buf` already holds the full nbytes
     * (edma_minor filled it), so this is a faithful transcript. */
    const unsigned n = nbytes / 2;

    fprintf(ot_capture_fp, "M %u %08x %u", core, saddr, nbytes);
    for (unsigned i = 0; i < n; i++) {
        fprintf(ot_capture_fp, " %04x", (buf[2 * i] << 8) | buf[2 * i + 1]);
    }
    fputc('\n', ot_capture_fp);
}

/* ------------------------------------------------------------------ machine */

struct OctatrackMachineState {
    MachineState parent;
    bool mk1;
    char *nvram;
    char *dsp_audio;
    char *capture;
    char *usb_notify;
    char *usb_host;
    bool throttle;
    uint32_t interleave;
    bool hold;
    bool audio_tap;
    bool hw_faithful;            /* model hardware's hostile facts, not QEMU's kindnesses */
    bool exit_with_frontend;     /* quit when the frontend that spawned us goes away */
};

#define TYPE_OCTATRACK_MACHINE MACHINE_TYPE_NAME("octatrack")
OBJECT_DECLARE_SIMPLE_TYPE(OctatrackMachineState, OCTATRACK_MACHINE)

/*
 * The board-revision strap. The firmware drives PPDSDR bit 5 and requires
 * bit 6 to follow, ten times, at 0x4001f8d8: on a MKII the two pins are
 * strapped together and img_gate_mk1_flag stays set (installing the 62-key
 * MKII panel map); on a MKI bit 6 reads 0 and the 57-key map is installed.
 */
static uint8_t ot_gpio_p10;
static bool ot_mk1;

/* ----------------------------------------------------- DSPI + DS1390 RTC -- */
/*
 * SPI master, two slaves: CS42448 codec control on PCS0 (write-only from MAIN
 * OS), DS1390 RTC on PCS1. Every transfer is polled on SR[7:4] = RXCTR — no
 * interrupt, no DMA — so the bus is a synchronous function call. PCS1 frames
 * are [MAP][DATA]; the value comes back on the SECOND POPR read (the first is
 * the byte clocked in while the address went out).
 */
#define OT_DSPI_BASE      0xFC05C000
#define DSPI_MCR          0x00
#define DSPI_SR           0x2C
#define DSPI_PUSHR        0x34
#define DSPI_POPR         0x38
#define DSPI_MCR_CLR_TXF  0x00000800u
#define DSPI_MCR_CLR_RXF  0x00000400u
#define DSPI_SR_TCF       0x80000000u
#define DSPI_SR_TXRXS     0x40000000u
#define DSPI_SR_TFFF      0x02000000u
#define DSPI_SR_RFDF      0x00020000u
#define DSPI_PUSHR_CONT   0x80000000u
#define OT_PCS1_RTC       0x02

typedef struct {
    MemoryRegion iomem;
    uint8_t  rxfifo[4];
    int      rxctr;
    bool     active;
    uint8_t  pcs, map;
    int64_t  skew_s;        /* guest-set time as an offset from host time */
    uint8_t  control, status;
} OTDspi;

static uint8_t ot_to_bcd(int v)   { return ((v / 10) << 4) | (v % 10); }
static int ot_from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static uint8_t ot_ds1390_read(OTDspi *s, uint8_t reg)
{
    time_t now = time(NULL) + s->skew_s;
    struct tm tm;

    localtime_r(&now, &tm);
    switch (reg & 0x0F) {
    case 0x00:                             /* hundredths — seeds the PRNG */
        return ot_to_bcd((int)((g_get_real_time() / 10000) % 100));
    case 0x01: return ot_to_bcd(tm.tm_sec);
    case 0x02: return ot_to_bcd(tm.tm_min);
    case 0x03: return ot_to_bcd(tm.tm_hour);           /* 24-hour mode */
    case 0x04: return ((tm.tm_wday + 6) % 7) + 1;      /* 1 = MONDAY    */
    case 0x05: return ot_to_bcd(tm.tm_mday);
    case 0x06: return ot_to_bcd(tm.tm_mon + 1);
    case 0x07: return ot_to_bcd(tm.tm_year % 100);     /* epoch 2000    */
    case 0x0D: return s->control;
    case 0x0E: return s->status;           /* OSF clear: time trustworthy */
    default:   return 0;
    }
}

static void ot_ds1390_write(OTDspi *s, uint8_t reg, uint8_t data)
{
    time_t now = time(NULL) + s->skew_s;
    struct tm tm;
    time_t want;

    switch (reg & 0x0F) {
    case 0x0D: s->control = data; return;
    case 0x0E: s->status = data & 0x81; return;
    default: break;
    }
    if ((reg & 0x0F) > 0x07) {
        return;                            /* alarms: accepted, unmodelled */
    }
    /* One field per transaction, re-encoded as a skew so the clock keeps
     * running and each write is idempotent. */
    localtime_r(&now, &tm);
    switch (reg & 0x0F) {
    case 0x01: tm.tm_sec  = ot_from_bcd(data); break;
    case 0x02: tm.tm_min  = ot_from_bcd(data); break;
    case 0x03: tm.tm_hour = ot_from_bcd(data & 0x3F); break;
    case 0x05: tm.tm_mday = ot_from_bcd(data); break;
    case 0x06: tm.tm_mon  = ot_from_bcd(data & 0x1F) - 1; break;
    case 0x07: tm.tm_year = ot_from_bcd(data) + 100; break;
    default: return;                       /* hundredths, day: derived */
    }
    tm.tm_isdst = -1;
    want = mktime(&tm);
    if (want != (time_t)-1) {
        s->skew_s = want - time(NULL);
    }
}

static void ot_dspi_rx_push(OTDspi *s, uint8_t v)
{
    if (s->rxctr < (int)ARRAY_SIZE(s->rxfifo)) {
        s->rxfifo[s->rxctr++] = v;
    }
}

static uint64_t ot_dspi_read(void *opaque, hwaddr addr, unsigned size)
{
    OTDspi *s = opaque;

    switch (addr) {
    case DSPI_SR:
        /* SR[7:4] is RXCTR, the field the firmware polls. */
        return DSPI_SR_TCF | DSPI_SR_TXRXS | DSPI_SR_TFFF
             | (s->rxctr ? DSPI_SR_RFDF : 0)
             | ((uint32_t)(s->rxctr & 0xF) << 4);
    case DSPI_POPR:
        if (s->rxctr > 0) {
            uint8_t v = s->rxfifo[0];

            memmove(s->rxfifo, s->rxfifo + 1, s->rxctr - 1);
            s->rxctr--;
            return v;
        }
        return 0;
    default:
        return 0;
    }
}

static void ot_dspi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    OTDspi *s = opaque;
    uint8_t pcs = (val >> 16) & 0xFF;
    uint8_t data = val & 0xFF;

    switch (addr) {
    case DSPI_MCR:
        if (val & (DSPI_MCR_CLR_RXF | DSPI_MCR_CLR_TXF)) {
            s->rxctr = 0;
            s->active = false;
        }
        break;
    case DSPI_PUSHR:
        if (!s->active || s->pcs != pcs) {
            s->active = true;              /* first frame: the address byte */
            s->pcs = pcs;
            s->map = data;
            ot_dspi_rx_push(s, 0);
        } else if (pcs == OT_PCS1_RTC) {
            if (s->map & 0x80) {
                ot_ds1390_write(s, s->map & 0x7F, data);
                ot_dspi_rx_push(s, 0);
            } else {
                ot_dspi_rx_push(s, ot_ds1390_read(s, s->map));
            }
        } else {
            ot_dspi_rx_push(s, 0);         /* codec: write-only from MAIN OS */
        }
        if (!(val & DSPI_PUSHR_CONT)) {
            s->active = false;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ot_dspi_ops = {
    .read = ot_dspi_read,
    .write = ot_dspi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * Reference-match / rollover fires per timer: 0-3 are DTIM0..3, 4-7 PIT0..3.
 *
 * ☠ PIT0's is the one that means something. It is the firmware's PREEMPTIVE
 * CONTEXT SWITCH (handler 0x40000550), and a PIT0 tick landing inside a DSP
 * transfer sequence is the necessary condition for that sequence to run long —
 * the per-block transfer sequence is what fixes it here. Silicon fixes the
 * ratio at 100 Hz against 2756.25 blocks/s = 0.0363 ticks per block; here it
 * is whatever QEMU_CLOCK_VIRTUAL gives, so `pit0/blk` in the exit report says
 * how far this run is from the hardware it is modelling.
 */
uint64_t ot_timer_fire_count[8];

void ot_timer_stats(char *buf, size_t len)
{
    snprintf(buf, len, "timers pit0=%llu dtim0=%llu dtim1=%llu dtim2=%llu "
             "dtim3=%llu",
             (unsigned long long)ot_timer_fire_count[4],
             (unsigned long long)ot_timer_fire_count[0],
             (unsigned long long)ot_timer_fire_count[1],
             (unsigned long long)ot_timer_fire_count[2],
             (unsigned long long)ot_timer_fire_count[3]);
}

/* -------------------------------------------------------------------- PIT -- */
/*
 * All four PITs; INTC1 sources 43..46. PIT0 is the 10 ms scheduler tick, PIT2
 * a 1.000 ms calibrated polled delay. This firmware writes PCSR (with EN set)
 * BEFORE PMR, so the PMR case must re-run the timer once a real limit exists —
 * without that, ptimer disables itself on the zero limit and the timer is dead.
 */
#define OT_PIT0_BASE    0xFC080000
#define PCSR_EN         0x0001
#define PCSR_RLD        0x0002
#define PCSR_PIF        0x0004
#define PCSR_PIE        0x0008
#define PCSR_OVW        0x0010
#define PCSR_PRE_SHIFT  8
#define PCSR_PRE_MASK   0x0f00

typedef struct {
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint16_t pcsr, pmr;
    unsigned idx;
} OTPit;

static void ot_pit_update(OTPit *s)
{
    qemu_set_irq(s->irq,
                 (s->pcsr & (PCSR_PIE | PCSR_PIF)) == (PCSR_PIE | PCSR_PIF));
}

static void ot_pit_trigger(void *opaque)
{
    OTPit *s = opaque;

    ot_timer_fire_count[4 + s->idx]++;
    s->pcsr |= PCSR_PIF;
    ot_pit_update(s);
}

static uint64_t ot_pit_read(void *opaque, hwaddr addr, unsigned size)
{
    OTPit *s = opaque;

    switch (addr) {
    case 0x00: return s->pcsr;
    case 0x02: return s->pmr;
    case 0x04: return ptimer_get_count(s->timer);
    default:   return 0;
    }
}

static void ot_pit_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    OTPit *s = opaque;
    int prescale, limit;

    switch (addr) {
    case 0x00:
        if (value & PCSR_PIF) {            /* write-1-to-clear */
            s->pcsr &= ~PCSR_PIF;
            value &= ~PCSR_PIF;
        }
        if (((s->pcsr ^ value) & ~PCSR_PIE) == 0) {
            s->pcsr = value;
            break;
        }
        ptimer_transaction_begin(s->timer);
        if (s->pcsr & PCSR_EN) {
            ptimer_stop(s->timer);
        }
        s->pcsr = value;
        prescale = 1 << ((s->pcsr & PCSR_PRE_MASK) >> PCSR_PRE_SHIFT);
        ptimer_set_freq(s->timer, (OT_SYS_FREQ / 2) / prescale);
        limit = (s->pcsr & PCSR_RLD) ? s->pmr : 0xffff;
        ptimer_set_limit(s->timer, limit, 0);
        if ((s->pcsr & PCSR_EN) && limit) {
            ptimer_run(s->timer, 0);
        }
        ptimer_transaction_commit(s->timer);
        break;
    case 0x02:
        ptimer_transaction_begin(s->timer);
        s->pmr = value;
        s->pcsr &= ~PCSR_PIF;
        if (s->pcsr & PCSR_RLD) {
            ptimer_set_limit(s->timer, value, s->pcsr & PCSR_OVW);
        } else if (s->pcsr & PCSR_OVW) {
            ptimer_set_count(s->timer, value);
        }
        if ((s->pcsr & PCSR_EN) && value) {
            ptimer_run(s->timer, 0);       /* PCSR-before-PMR re-arm */
        }
        ptimer_transaction_commit(s->timer);
        break;
    default:
        break;
    }
    ot_pit_update(s);
}

static const MemoryRegionOps ot_pit_ops = {
    .read = ot_pit_read,
    .write = ot_pit_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------- DMA timers -- */
/*
 * DTIM0..3 at 0xFC070000, the board's time base; INTC0 sources 32..35.
 * DTIM1's 120 Hz reference match drives the 60 Hz system tick that polls card
 * detect and mounts the filesystem; DTIM2/3 are elapsed-time stopwatches.
 * DTMR[CLK] selects bus clock /1 or /16 and is not decoration (ignoring it
 * runs the tick at 16x). The match fires after DTRR + 1 intervals (RM §30.2.4)
 * and DTER[REF] is write-1-to-clear. A write to DTCN clears it — the RM's
 * "read-only" table entry is contradicted by its own register description, and
 * the firmware starts its stopwatch with exactly that write.
 */
#define OT_DTIM0_BASE   0xFC070000
#define DTMR_RST        0x0001
#define DTMR_CLK        0x0006
#define DTMR_FRR        0x0008
#define DTMR_ORRI       0x0010
#define DTXMR_DMAEN     0x80
#define DTER_REF        0x02

typedef struct {
    MemoryRegion iomem;
    uint16_t dtmr;
    uint8_t dtxmr, dter;
    uint32_t dtrr, dtcr;
    int64_t start_ns;
    qemu_irq irq;
    QEMUTimer *ref;
    unsigned idx;
} OTDtim;

static unsigned ot_dtim_divider(OTDtim *s)
{
    switch (s->dtmr & DTMR_CLK) {
    case 0x2: return 1 * (((s->dtmr >> 8) & 0xff) + 1);
    case 0x4: return 16 * (((s->dtmr >> 8) & 0xff) + 1);
    default:  return 0;                    /* stopped, or the unwired pin */
    }
}

static uint64_t ot_dtim_period_ns(OTDtim *s)
{
    unsigned div = ot_dtim_divider(s);

    if (!div || !s->dtrr) {
        return 0;
    }
    return (uint64_t)(s->dtrr + 1) * div * 1000000000ULL / OT_BUS_HZ;
}

static uint32_t ot_dtim_count(OTDtim *s)
{
    unsigned div = ot_dtim_divider(s);
    uint64_t ticks;

    if (!(s->dtmr & DTMR_RST) || !div) {
        return 0;
    }
    ticks = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns;
    return (uint32_t)(ticks * OT_BUS_HZ / 1000000000ULL / div);
}

static void ot_dtim_update_irq(OTDtim *s)
{
    qemu_set_irq(s->irq, (s->dter & DTER_REF)
                      && (s->dtmr & DTMR_ORRI)
                      && !(s->dtxmr & DTXMR_DMAEN));
}

/* Arm only what the firmware asked for, never with a zero reference: DTIM3
 * runs RST|FRR with ORRI clear and DTRR 0, and a zero-period timer re-enters
 * its own callback without the clock advancing, wedging the whole emulated
 * Octatrack. */
static void ot_dtim_arm(OTDtim *s)
{
    uint64_t period = ot_dtim_period_ns(s);

    if (!(s->dtmr & DTMR_RST) || !(s->dtmr & DTMR_ORRI) || !period) {
        timer_del(s->ref);
        return;
    }
    timer_mod(s->ref, s->start_ns + period);
}

static void ot_dtim_ref_fire(void *opaque)
{
    OTDtim *s = opaque;
    uint64_t period = ot_dtim_period_ns(s);

    ot_timer_fire_count[s->idx]++;
    s->dter |= DTER_REF;
    ot_dtim_update_irq(s);
    if (!period) {
        return;
    }
    if (s->dtmr & DTMR_FRR) {
        s->start_ns += period;
    } else {
        /* Restart mode off: the next match is a full 32-bit wrap away. */
        s->start_ns += (1ULL << 32) / (s->dtrr + 1) * period;
    }
    ot_dtim_arm(s);
}

static uint64_t ot_dtim_read(void *opaque, hwaddr addr, unsigned size)
{
    OTDtim *s = opaque;

    switch (addr) {
    case 0x00: return s->dtmr;
    case 0x02: return s->dtxmr;
    case 0x03: return s->dter;
    case 0x04: return s->dtrr;
    case 0x08: return s->dtcr;
    case 0x0c: return ot_dtim_count(s);
    default:   return 0;
    }
}

static void ot_dtim_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    OTDtim *s = opaque;

    switch (addr) {
    case 0x00:
        if (!(s->dtmr & DTMR_RST) && (val & DTMR_RST)) {
            s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        s->dtmr = val;
        ot_dtim_arm(s);
        ot_dtim_update_irq(s);
        break;
    case 0x02:
        s->dtxmr = val;
        ot_dtim_update_irq(s);
        break;
    case 0x03:
        s->dter &= ~(uint8_t)val;          /* write-1-to-clear */
        ot_dtim_update_irq(s);
        break;
    case 0x04:
        s->dtrr = val;
        ot_dtim_arm(s);
        break;
    case 0x08:
        s->dtcr = val;
        break;
    case 0x0c:
        s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ot_dtim_arm(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ot_dtim_ops = {
    .read = ot_dtim_read,
    .write = ot_dtim_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------- eDMA -- */
/*
 * MCF54455 eDMA, 16 channels; channel n interrupts on INTC0 source 8+n. QEMU
 * has no eDMA model. The audio block loop is built out of MAJORELINK: the
 * frame ISR writes TCD1.CSR = 0x0621 and ch1's completion services ch6, whose
 * completion services ch7, whose INTMAJOR finally vectors the state ISR. The
 * completion flag is LATCHED until CINT clears it — a pulse is dropped before
 * the CPU ever sees it. TCD 16-bit halves are swapped relative to the Kinetis
 * layout: +04 ATTR +06 SOFF +14 CITER +16 DOFF +1C BITER +1E CSR.
 */
#define OT_EDMA_BASE    0xFC044000
#define OT_EDMA_TCD     0xFC045000
#define OT_EDMA_CHANS   16
#define EDMA_CINT       0x1C
#define EDMA_SSRT       0x1E
#define CSR_START       (1u << 0)
#define CSR_INTMAJOR    (1u << 1)
#define CSR_MAJORELINK  (1u << 5)
#define CSR_ACTIVE      (1u << 6)
#define CSR_DONE        (1u << 7)
#define OT_EDMA_LINK_DEPTH 8

/*
 * Completion interrupts from a CSR-START transfer are raised from a timer,
 * never from inside the arming write: the frame ISR arms its readback chain
 * at IPL 5 and only ~10 instructions LATER resets the state counter, so a
 * synchronous ch7 INTMAJOR preempts it with the stale counter — state 7's
 * entry never acks ch7 and the re-entry storm at IPL 6 starves the frame IRQ
 * for tens of milliseconds per block (measured 71M spurious deliveries of
 * source 15 in 20 s). Same mechanism as the ATA model's delayed INTRQ.
 *
 * SSRT-started transfers raise SYNCHRONOUSLY: those stores come from the
 * state machine ISR at IPL 6, the level-6 completion cannot preempt it, and
 * by its rte the counter is already advanced — and a timer per state costs a
 * main-loop wakeup each, most of a millisecond per block.
 */
/*
 * THE GATE. Not a delay — the observable itself.
 *
 * Read-disasm on OS 1.40C, and the reason this is exact rather than a margin:
 *
 *     4000ac12  movew %a2,0xfc04503e      TCD1.CSR = 0x0621   <- the arm
 *     4000ac18  movel 0x80004800,%d0
 *     4000ac1e  lsll #7,%d0
 *     4000ac20  addil #-2147462048,%d0
 *     4000ac26  movel %d0,0x80003c10
 *     4000ac2c  clrl 0x46104d3a
 *     4000ac32  clrl 0x46104d3e           <- the state counter reset
 *
 * Six instructions, straight-line, no branch between them. The same counter is
 * INCREMENTED at the tail of every DSP transfer state (addql #1,0x46104d3e at
 * 0x400049c0 and 0x40004a2e), so it is identified from both sides: the state
 * machine counts with it, the frame ISR clears it before re-arming. Measured
 * live, a block that ran seven states reads 7 at the arm and 0 after.
 *
 * So "the counter is zero" IS "the frame ISR has reset it", and holding the
 * completion until then is the requirement, not an approximation of it.
 *
 * Why polling it on GUEST PROGRESS is exact and not a race: while the
 * completion is held, the state ISR cannot run — it is vectored by the very
 * ch7 INTMAJOR being held — so nothing can increment the counter back out of
 * zero behind our back. The window stays open until we deliver it.
 *
 * The degenerate case, measured: on the FIRST arm of a run the counter is
 * already zero, because no state has ever run. Delivering there is safe by
 * definition — "stale" requires a previous value, and there is none.
 *
 * ☠ There is NO ESCAPE. If the counter has not been reset within
 * OT_EDMA_GATE_MAX_INSN retired guest instructions the model is wrong about
 * this firmware path, and that is a bug to be named rather than a stall to be
 * ridden out. The bound is ~20x the largest gap ever measured (15087
 * instructions, across an idle stretch; the tight case is 10).
 */
#define OT_FW_EDMA_STATE_CTR  0x46104d3e   /* OS 1.40C, cleared at 0x4000ac32 */
#define OT_EDMA_GATE_MAX_INSN 300000

typedef struct {
    MemoryRegion reg, tcdmem;
    uint8_t tcd[OT_EDMA_CHANS][0x20];
    qemu_irq irq[OT_EDMA_CHANS];
    uint16_t intflags;
    uint16_t pending;
    unsigned link_depth;
    bool defer_irq;                     /* set for CSR-START-armed chains */
    uint64_t gate_at;                   /* retired instructions at the arm   */
} OTEdma;

static OTEdma *ot_edma;
/* How long the gate actually holds, in retired guest instructions — the only
 * load-invariant way to say it. A gate that is guessing shows up here as a
 * distribution that does not fit the six-instruction window it models. */
static uint64_t ot_edma_gate_polls, ot_edma_gate_max;
/* Sequences started vs sums actually armed: the firmware's two guards on
 * state 7 mean these need not be equal. */
static uint64_t ot_edma_seqs, ot_edma_sums;

/* Structural, load-invariant: how much work the model does per audio block.
 * `dispatches` is generic-path memory accesses — the element walk collapses to
 * one per side when the cursor is contiguous over plain RAM. Counts mean the
 * same thing on a busy host as on a quiet one, which wall timings do not. */
static uint64_t ot_edma_minors, ot_edma_dispatches, ot_edma_bytes;

void ot_edma_stats(char *buf, size_t len)
{
    snprintf(buf, len, "edma minors=%llu dispatches=%llu bytes=%llu "
             "gate_polls=%llu gate_max_insn=%llu seqs=%llu sums=%llu",
             (unsigned long long)ot_edma_minors,
             (unsigned long long)ot_edma_dispatches,
             (unsigned long long)ot_edma_bytes,
             (unsigned long long)ot_edma_gate_polls,
             (unsigned long long)ot_edma_gate_max,
             (unsigned long long)ot_edma_seqs,
             (unsigned long long)ot_edma_sums);
}

uint64_t ot_guest_insn(void)
{
    return ot_insn_retired();
}

/* Retired-instruction stamp of the FIRST arm of the block in flight — the
 * moment the firmware's seven-state transfer sequence started. */
static uint64_t ot_edma_seq_start;
/* state_isr's jump table at 0x400ab61a holds exactly 8 entries. */
#define OT_EDMA_STATES 8
static uint64_t ot_edma_state_overflows;

uint64_t ot_edma_seq_started(void)
{
    return ot_edma_seq_start;
}

uint32_t ot_edma_state_ctr(void)
{
    uint32_t be = 0;

    address_space_read(&address_space_memory, OT_FW_EDMA_STATE_CTR,
                       MEMTXATTRS_UNSPECIFIED, &be, sizeof be);
    return ldl_be_p(&be);
}

static void ot_edma_irq_fire(OTEdma *s);

/*
 * Everything that happens on GUEST PROGRESS, in order. The eDMA gate first:
 * its completion is what lets the guest's state machine advance, so a block
 * held one quantum longer than it needs is guest time lost. Then the DSP,
 * which is where most of the quantum goes.
 */
static void ot_guest_progress(void)
{
    ot_edma_gate_poll();
    ot_dsp_interleave();
}

/*
 * The gate, polled on GUEST PROGRESS: the interleave hook calls this every
 * `interleave` retired guest instructions. See OT_FW_EDMA_STATE_CTR above for
 * why a poll on that clock is exact rather than a race.
 */
void ot_edma_gate_poll(void)
{
    OTEdma *s = ot_edma;
    uint32_t be = 0;

    if (!s || !s->pending) {
        return;
    }
    address_space_read(&address_space_memory, OT_FW_EDMA_STATE_CTR,
                       MEMTXATTRS_UNSPECIFIED, &be, sizeof be);
    if (ldl_be_p(&be) != 0) {
        const uint64_t held = ot_insn_retired() - s->gate_at;

        ot_edma_gate_polls++;
        if (held > OT_EDMA_GATE_MAX_INSN) {
            error_report("octatrack: eDMA completion gate never opened — "
                         "state counter %#x still %u after %llu guest "
                         "instructions, pending=%#x. The model is wrong about "
                         "this firmware path.", OT_FW_EDMA_STATE_CTR,
                         ldl_be_p(&be), (unsigned long long)held, s->pending);
            abort();
        }
        return;
    }
    {
        const uint64_t held = ot_insn_retired() - s->gate_at;

        if (held > ot_edma_gate_max) {
            ot_edma_gate_max = held;
        }
    }
    bql_lock();
    ot_edma_irq_fire(s);
    bql_unlock();
}

static void ot_edma_irq_fire(OTEdma *s)
{
    uint16_t p = s->pending;


    s->pending = 0;
    for (unsigned ch = 0; ch < OT_EDMA_CHANS; ch++) {
        if (p & (1u << ch)) {
            s->intflags |= 1u << ch;
            qemu_set_irq(s->irq[ch], 1);
        }
    }
}

static uint32_t tcd_r32(OTEdma *s, int ch, int off)
{
    const uint8_t *p = &s->tcd[ch][off];
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static uint16_t tcd_r16(OTEdma *s, int ch, int off)
{
    const uint8_t *p = &s->tcd[ch][off];
    return (p[0] << 8) | p[1];
}
static void tcd_w32(OTEdma *s, int ch, int off, uint32_t v)
{
    uint8_t *p = &s->tcd[ch][off];
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void tcd_w16(OTEdma *s, int ch, int off, uint16_t v)
{
    uint8_t *p = &s->tcd[ch][off];
    p[0] = v >> 8; p[1] = v;
}

/* ATTR SSIZE/DSIZE: 0=1B 1=2B 2=4B 4=16B 5=32B. */
static unsigned edma_esize(unsigned code)
{
    switch (code & 7) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    case 4: return 16;
    case 5: return 32;
    default: return 4;
    }
}

/*
 * A 32-bit source element crossing to the 16-bit FlexBus port leaves as two
 * beats LOW HALF FIRST (and a 32-bit destination element reassembles port
 * beats the same way). Shipping high-half-first lands every 32-bit control
 * field one port word off inside the DSP: the mixer reads the zero CUE word
 * where the track level belongs and the master sum goes silent — while the
 * low-16 descriptor consumers still parse, which hid this for a long time.
 * The gate is the PORT-side element size: a 4-byte port element is two beats
 * (swap); 2-byte port elements (the boot module upload) cross unswapped.
 *
 * ☠ ONLY the 32-byte control minors swap. Stream, records, sum, readback and
 * upload all cross STRAIGHT — the state-6 sum is a verbatim CPU echo, and
 * swapping it measured Goertzel 0.001 broadband against 1.000 straight.
 */
static void edma_beat_swap(uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i + 3 < n; i += 4) {
        uint8_t a = buf[i], b = buf[i + 1];

        buf[i] = buf[i + 2];
        buf[i + 1] = buf[i + 3];
        buf[i + 2] = a;
        buf[i + 3] = b;
    }
}

/*
 * True when [addr, addr+nbytes) lies entirely inside one plain-RAM region.
 *
 * Checked by TRANSLATING, never by probing with a map: address_space_map on an
 * MMIO range performs the access into a bounce buffer, so a probe that then
 * fell back to the element loop would run every read TWICE. Translation has no
 * side effects.
 */
static bool edma_range_is_ram(hwaddr addr, uint32_t nbytes, bool is_write)
{
    hwaddr xlat, len = nbytes;
    MemoryRegion *mr;

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&address_space_memory, addr, &xlat, &len,
                                 is_write, MEMTXATTRS_UNSPECIFIED);
    if (!mr || len < nbytes || !memory_region_is_ram(mr)) {
        return false;
    }
    return !is_write || !memory_region_is_rom(mr);
}

/*
 * One side of a minor loop, source or destination. Returns the advanced cursor.
 *
 * A contiguous cursor (offset == element size) over plain RAM is one bulk call
 * instead of one address_space dispatch per element — byte-identical, because
 * for a RAM region both forms reduce to a memcpy and the element loop's cursor
 * lands on exactly addr + nbytes. MMIO and strided/pinned cursors keep the
 * element loop, because there the access SIZE is architecturally visible; a
 * pinned cursor (offset 0) is what makes an address behave as a FIFO.
 *
 * Worth 1340 -> ~104 generic dispatches per audio block. Note that is a
 * single-digit-percent win, not a lever: the element walk measured 30 us of
 * the 363 us block budget.
 */
static uint32_t edma_side(uint32_t addr, int16_t off, unsigned esize,
                          uint8_t *buf, uint32_t nbytes, bool is_write)
{
    if (off == (int16_t)esize && esize && (nbytes % esize) == 0 &&
        edma_range_is_ram(addr, nbytes, is_write)) {
        if (is_write) {
            address_space_write(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, buf, nbytes);
        } else {
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf, nbytes);
        }
        ot_edma_dispatches++;
        return addr + nbytes;
    }
    for (uint32_t i = 0; i < nbytes; i += esize) {
        uint32_t n = MIN(esize, nbytes - i);

        if (is_write) {
            address_space_write(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, buf + i, n);
        } else {
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf + i, n);
        }
        addr += off;
        ot_edma_dispatches++;
    }
    return addr;
}

/*
 * Minor loops are small and there are a lot of them: measured over a 4259-block
 * boot, 296528 minors moving 42172928 bytes — 69.6 minors per audio block at a
 * mean of 142 bytes. That was a g_malloc/g_free pair each, on the block's
 * critical path, for a buffer that almost always fits in a cache line or two.
 *
 * A stack buffer covers the common case and costs nothing. It is also the
 * re-entrancy-safe form: edma_minor's element loop writes through
 * address_space_write, which can in principle land in the TCD window and start
 * another channel, so a single shared scratch buffer on the device would be
 * wrong.
 *
 * Only the MEAN was measured, not the distribution, so this is sized to
 * cover the block protocol's known transfers (control block 128 B, track
 * records 256 B, sum 1024 B, readback 1536 B) rather than to a measured
 * quantile — and anything larger still falls back to the heap rather than
 * being truncated. Bounded above by stack: edma_start recurses up to
 * OT_EDMA_LINK_DEPTH through MAJORELINK.
 */
#define OT_EDMA_STACK_BUF 2048

/*
 * Record tap (OCTA_REC_LOG=1) — word 30 of every 64-byte track record, on the
 * transfer that carries it to the DSP.
 *
 * Word 30 is the voice-lifecycle word: the DSP gates its amp envelope on a
 * note-on bit there, and that bit never appears for a NEIGHBOR-class machine —
 * which is why a NEIGHBOR-class machine drones and why the AMP page does nothing for
 * them. Polling for it over the gdbstub CANNOT settle the question: a strobe
 * that lasts one block is 0.36 ms of guest time, and six samples across a run
 * will miss it every time. This sees every block, and prints only CHANGES, so a
 * strobe shows up as a pair of lines rather than a flood.
 */
#define OT_REC_BASE 0x80000110u
#define OT_REC_END  0x80000410u

static int ot_rec_on = -1;
static uint16_t ot_rec_w30[8];
static int ot_rec_seen[8];

static void ot_rec_tap(uint32_t saddr, const uint8_t *buf, uint32_t nbytes)
{
    if (ot_rec_on < 0) {
        ot_rec_on = getenv("OCTA_REC_LOG") != NULL;
    }
    if (!ot_rec_on || saddr < OT_REC_BASE || saddr >= OT_REC_END) {
        return;
    }
    for (unsigned t = 0; t < 8; t++) {
        uint32_t off = OT_REC_BASE + t * 64 + 60;   /* word 30 */

        if (off < saddr || off + 2 > saddr + nbytes) {
            continue;
        }
        const uint8_t *p = buf + (off - saddr);
        uint16_t v = (uint16_t)((p[0] << 8) | p[1]);

        if (!ot_rec_seen[t] || v != ot_rec_w30[t]) {
            fprintf(stderr, "octatrack: rec word30 track%u %04x -> %04x%s\n",
                    t + 1, ot_rec_seen[t] ? ot_rec_w30[t] : 0, v,
                    (v & 0x1000) ? "   NOTE-ON (bit 12)" : "");
            ot_rec_w30[t] = v;
            ot_rec_seen[t] = 1;
        }
    }
}

/*
 * Readback tap (OCTA_RB_LOG=1) — what actually lands in the post-FX readback.
 *
 * The readback arena at 0x80003190 (2 banks x 8 tracks x 128 B) is the ONLY
 * place a RECEIVE machine can take another track's audio from, so "did track N
 * reach the readback" is the question every internal-mixer test rests on.
 * Asking it from the guest side is awkward: over --gdb the boot runs ~5x slower
 * and the halt lands at an arbitrary point inside the block, so a zero read
 * cannot be told from a zero buffer. Here the answer is exact — this is the
 * write itself, before anything can overwrite it.
 *
 * One line per OT_RB_PERIOD writes: the peak |sample| deposited in each 128 B
 * slot since the previous line, and the slot's destination address, so the
 * arena's layout is MEASURED rather than assumed.
 */
#define OT_RB_BASE   0x80003190u
#define OT_RB_SPAN   0x800u
#define OT_RB_SLOTS  (OT_RB_SPAN / 128)
#define OT_RB_PERIOD 2000

static int ot_rb_on = -1;
static uint32_t ot_rb_peak[OT_RB_SLOTS];
static uint64_t ot_rb_writes, ot_rb_bytes;
static uint32_t ot_rb_seen[8];          /* first distinct (daddr,nbytes) shapes */
static unsigned ot_rb_nseen;

static void ot_rb_tap(uint32_t daddr, uint32_t saddr, int16_t soff,
                      const uint8_t *buf, uint32_t nbytes)
{
    if (ot_rb_on < 0) {
        ot_rb_on = getenv("OCTA_RB_LOG") != NULL;
    }
    if (!ot_rb_on || daddr + nbytes <= OT_RB_BASE ||
        daddr >= OT_RB_BASE + OT_RB_SPAN) {
        return;
    }
    for (uint32_t i = 0; i + 4 <= nbytes; i += 4) {
        const uint32_t a = daddr + i;
        int32_t v;
        uint32_t m, slot;

        if (a < OT_RB_BASE || a >= OT_RB_BASE + OT_RB_SPAN) {
            continue;
        }
        slot = (a - OT_RB_BASE) / 128;
        v = (int32_t)(((uint32_t)buf[i] << 24) | ((uint32_t)buf[i + 1] << 16) |
                      ((uint32_t)buf[i + 2] << 8) | buf[i + 3]);
        m = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
        if (m > ot_rb_peak[slot]) {
            ot_rb_peak[slot] = m;
        }
    }
    /* Layout, measured: the first few distinct write shapes that land in the
     * arena. Which channel writes how many bytes where is the difference
     * between "the readback is silent" and "the readback is not where the map
     * says", and only the write itself can tell them apart. */
    if (ot_rb_nseen < 8) {
        unsigned k;

        for (k = 0; k < ot_rb_nseen; k++) {
            if (ot_rb_seen[k] == daddr) {
                break;
            }
        }
        if (k == ot_rb_nseen) {
            ot_rb_seen[ot_rb_nseen++] = daddr;
            fprintf(stderr, "octatrack: readback write %#010x + %u B "
                    "(slot %u/%u) from %#010x soff %d %s "
                    "[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                    daddr, nbytes, (daddr - OT_RB_BASE) / 1024,
                    ((daddr - OT_RB_BASE) % 1024) / 128, saddr, soff,
                    (saddr == OT_DSP_PORT_DATA && soff == 0) ? "BULK-POP"
                        : saddr == OT_DSP_PORT_DATA ? "MMIO-WALK" : "memcpy",
                    buf[0], buf[1], buf[2], buf[3],
                    buf[4], buf[5], buf[6], buf[7]);
        }
    }
    ot_rb_bytes += nbytes;
    if (++ot_rb_writes % OT_RB_PERIOD == 0) {
        char line[512];
        int n = snprintf(line, sizeof line, "octatrack: readback peaks");

        for (unsigned k = 0; k < OT_RB_SLOTS; k++) {
            n += snprintf(line + n, sizeof line - n, " %u/%u:%u",
                          k / 8, k % 8, ot_rb_peak[k]);
            ot_rb_peak[k] = 0;
        }
        n += snprintf(line + n, sizeof line - n, " | last %#010x+%u:",
                      daddr, nbytes);
        for (unsigned b = 0; b < 12 && b < nbytes; b++) {
            n += snprintf(line + n, sizeof line - n, " %02x", buf[b]);
        }
        fprintf(stderr, "%s (writes %llu, %llu B)\n", line,
                (unsigned long long)ot_rb_writes,
                (unsigned long long)ot_rb_bytes);
    }
}

/* One minor loop. Independent source and destination cursors are what make a
 * pinned address (SOFF or DOFF 0) behave as a FIFO — the DSP host port. */
static void edma_minor(OTEdma *s, int ch)
{
    uint32_t nbytes = tcd_r32(s, ch, 0x08);
    uint32_t saddr  = tcd_r32(s, ch, 0x00);
    uint32_t daddr  = tcd_r32(s, ch, 0x10);
    uint16_t attr   = tcd_r16(s, ch, 0x04);
    int16_t  soff   = (int16_t)tcd_r16(s, ch, 0x06);
    int16_t  doff   = (int16_t)tcd_r16(s, ch, 0x16);
    unsigned ss = edma_esize(attr >> 8), ds = edma_esize(attr);
    uint8_t stackbuf[OT_EDMA_STACK_BUF];
    g_autofree uint8_t *heap = NULL;
    uint8_t *buf;

    const uint32_t saddr_in = saddr;

    if (!nbytes || nbytes > (1u << 20)) {
        return;
    }
    ot_edma_minors++;
    ot_edma_bytes += nbytes;
    buf = nbytes <= sizeof stackbuf ? stackbuf : (heap = g_malloc(nbytes));

    /* The DSP data port pinned as source or destination gets one bulk call per
     * minor loop instead of a locked round trip per beat. */
    if (saddr == OT_DSP_PORT_DATA && soff == 0) {
        ot_dsp_edma_read(buf, nbytes);
    } else {
        saddr = edma_side(saddr, soff, ss, buf, nbytes, false);
    }
    if (daddr == OT_DSP_PORT_DATA && doff == 0) {
        ot_rec_tap(saddr_in, buf, nbytes);
        if (ds == 4 && nbytes == 32) {
            /*
             * The control block's beat order, one bit per 32-bit element of
             * the 128-byte block. It is NOT uniform, and every bit here is a
             * measurement — only five elements carry asymmetric content, and
             * each has its own observable:
             *
             *   elements 0,2..18  slot (cue, level): SWAP. The guest writes
             *      0x6c00 in the second word, the payload reads the level from
             *      the first — straight, TRIG9 is silent on ALL EIGHT slots,
             *      not merely moved to another pair.
             *   element 20  words 0x28/0x29 (CUE_LEVEL, MAIN_LEVEL): SWAP.
             *      Straight, a track's level on MAIN follows the project's
             *      CUE_LEVEL setting and vice versa — measured with the two
             *      named settings, quadratically (0x7f -> 4x, 0x10 -> 1/16).
             *   element 21  words 0x2a/0x2b: STRAIGHT. Swapped, the phones bus
             *      (TX 5/6) drops from -3 dB under MAIN to -35 dB.
             *   element 24  words 0x30/0x31 (metronome flag, pitch): STRAIGHT.
             *      Core 1 tests bit 4 of DSP word 0x30; swapped it gets the
             *      pitch and the click never arms.
             *   element 25  words 0x32/0x33 (metronome CUE, MAIN volume): SWAP.
             *      Straight, METRONOME_MAIN_VOLUME comes out of the CUE jacks
             *      and vice versa — which is also how CUE and PHONES were
             *      finally identified.
             *
             * The pattern is semantic, not electrical: EVERY (cue, main) pair
             * the guest writes needs transposing — the slot's (cue send, track
             * level), the two output levels, the metronome's two volumes —
             * while the one pair that is not a cue/main pair, the metronome's
             * (flag, pitch), does not. The guest orders cue before main; the
             * payload reads main before cue.
             *
             * Every other element is zero or symmetric in the blocks the guest
             * actually sends, so its bit is untested and cannot matter yet.
             */
            const uint32_t kControlSwapMask = 0x02155555u;
            const uint32_t s0 = tcd_r32(s, ch, 0x00);
            const unsigned e0 = ((s0 - 0x60) & 0x7f) / 4;

            for (unsigned i = 0; i < 8; i++) {
                if (kControlSwapMask & (1u << ((e0 + i) & 31))) {
                    uint8_t *q = buf + i * 4;
                    const uint8_t a = q[0], b = q[1];

                    q[0] = q[2];
                    q[1] = q[3];
                    q[2] = a;
                    q[3] = b;
                }
            }

            /*
             * MASTER TRACK reroute. In the delivered element the first word
             * is the track's direct-MAIN level (solo/mute-gated LEVEL) and
             * the second feeds the block-top premix the DSP stages into
             * track 8's stream slot — the master track's input (see
             * the master track). With MASTER_TRACK on,
             * the real device routes every track through track 8 by
             * default: no direct MAIN, the premix carries each track at
             * its gated LEVEL (so faders, mutes and solos keep working —
             * the send reuses the already-gated word). The guest never
             * stages this arrangement itself (the master toggle writes no
             * routing state — watch-verified), so it is synthesized here,
             * per delivery, from the guest's own MASTER_TRACK byte.
             * Elements 0,2,..,12 are tracks 1-7; track 8 (element 14) is
             * left alone — its LEVEL stays on MAIN (the master output
             * level) and its own premix send stays zero (no feedback).
             */
            {
                uint8_t master;

                address_space_read(&address_space_memory, 0x80000034,
                                   MEMTXATTRS_UNSPECIFIED, &master, 1);
                /* ☠ Core 0 (the mixer core) ONLY. Core 1 renders the voices
                 * and consumes the same control layout for its own output
                 * levels — zeroing its first words silences every source
                 * before the mixer ever sees them (measured: total silence,
                 * not a routing change). */
                if (master && ot_dsp_selected_core() == 0) {
                    for (unsigned i = 0; i < 8; i++) {
                        const unsigned e = (e0 + i) & 31;
                        uint8_t *q = buf + i * 4;

                        if ((e & 1) == 0 && e < 14) {
                            q[2] = q[0];
                            q[3] = q[1];
                            q[0] = 0;
                            q[1] = 0;
                        } else if (e == 14) {
                            /* Track 8: word1 is the MASTER OUTPUT level
                             * (level-table position 17, the pair-2/ring
                             * feedback scale) — zero here mutes the whole
                             * master output stage. Measured: with the
                             * studio-mode delivery [level, level] position
                             * 17 builds nonzero; with [level, 0] it builds
                             * zero and MAIN is exact silence. */
                            q[2] = q[0];
                            q[3] = q[1];
                        }
                    }
                }
            }
        }
        if (ot_capture_fp) {
            if (ot_capture_wire) {
                /* saddr read BEFORE the cursor writeback below */
                ot_wirelog_minor(buf, nbytes, ot_dsp_selected_core(),
                                 tcd_r32(s, ch, 0x00));
            } else {
                ot_capture_minor(buf, nbytes, ot_dsp_selected_core());
            }
        }
        ot_dsp_edma_write(buf, nbytes);
    } else {
        ot_rb_tap(daddr, saddr_in, soff, buf, nbytes);
        daddr = edma_side(daddr, doff, ds, buf, nbytes, true);
    }
    tcd_w32(s, ch, 0x00, saddr);
    tcd_w32(s, ch, 0x10, daddr);
}

static void edma_start(OTEdma *s, int ch)
{
    uint16_t citer = tcd_r16(s, ch, 0x14) & 0x1FF;
    uint16_t biter = tcd_r16(s, ch, 0x1C);
    uint16_t csr   = tcd_r16(s, ch, 0x1E);
    unsigned iters = citer ? citer : 1;

    /* CITER self-links, so one request runs the whole major loop inline. */
    for (unsigned n = 0; n < iters; n++) {
        edma_minor(s, ch);
    }
    tcd_w32(s, ch, 0x00, tcd_r32(s, ch, 0x00) + tcd_r32(s, ch, 0x0C)); /* SLAST */
    tcd_w32(s, ch, 0x10, tcd_r32(s, ch, 0x10) + tcd_r32(s, ch, 0x18)); /* DLAST */
    tcd_w16(s, ch, 0x14, biter);
    csr = (csr & ~(CSR_START | CSR_ACTIVE)) | CSR_DONE;
    tcd_w16(s, ch, 0x1E, csr);

    if (csr & CSR_INTMAJOR) {
        if (s->defer_irq) {
            /* Held until the frame ISR resets the state counter. */
            s->pending |= 1u << ch;
            s->gate_at = ot_insn_retired();
        } else {
            s->intflags |= 1u << ch;
            qemu_set_irq(s->irq[ch], 1);
        }
    }
    if (csr & CSR_MAJORELINK) {
        int link = (csr >> 8) & 0xF;

        if (s->link_depth + 1 >= OT_EDMA_LINK_DEPTH) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "eDMA ch%d links beyond depth %d — cycle?\n",
                          ch, OT_EDMA_LINK_DEPTH);
        } else if (link != ch) {
            s->link_depth++;
            edma_start(s, link);
            s->link_depth--;
        }
    }
}

static uint64_t ot_edma_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void ot_edma_reg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    OTEdma *s = opaque;

    switch (addr) {
    case EDMA_SSRT:
        if ((val & 0x40) == 0 && (val & 0x1F) < OT_EDMA_CHANS) {
            /* Every state of the firmware's transfer sequence ends with an
             * SSRT; the state counter is still 0 at the first one, so this
             * stamps when the sequence for the block in flight began. */
            {   /* The counter is bumped AFTER each SSRT, so its value here
                 * names the state being launched: 0 starts a sequence, 6 is
                 * the conditional sum arm. */
                const uint32_t st = ot_edma_state_ctr();

                /* state_isr (0x40004840) dispatches through an 8-entry jump
                 * table at 0x400ab61a with NO bounds check:
                 *     movel 0x46104d3e,%d0
                 *     lea   0x400ab61a,%a0
                 *     moveal %a0@(0,%d0:l:4),%a0
                 *     jmp   %a0@
                 * Past entry 7 the table runs into an unrelated ramp table, so
                 * a counter that gets ahead of frame_isr's clear (0x4000ac32)
                 * jumps to a garbage address — on a unit that surfaces as the
                 * EXCEPTION screen with an address in the audio path, with
                 * nothing to say what caused it. Any code added to the audio
                 * ISR can push the sequence over, so name the condition here,
                 * where it is cheap, instead of letting it wild-jump. */
                if (st >= OT_EDMA_STATES) {
                    static uint64_t reported;

                    if (reported++ < 8) {
                        fprintf(stderr, "octatrack: EDMA STATE OVERFLOW: "
                                "seq counter %u >= %u at SSRT — state_isr "
                                "will dispatch past its jump table and jump "
                                "to garbage (this is the audio-path EXCEPTION "
                                "signature). Something is holding off "
                                "frame_isr.\n", st, OT_EDMA_STATES);
                    }
                    ot_edma_state_overflows++;
                }
                if (st == 0) {
                    ot_edma_seq_start = ot_insn_retired();
                    ot_edma_seqs++;
                } else if (st == 6) {
                    ot_edma_sums++;
                }
            }
            edma_start(s, val & 0x1F);
        }
        break;
    case EDMA_CINT:
        if (val & 0x40) {                  /* CAIR: clear all */
            for (unsigned ch = 0; ch < OT_EDMA_CHANS; ch++) {
                if (s->intflags & (1u << ch)) {
                    qemu_set_irq(s->irq[ch], 0);
                }
            }
            s->intflags = 0;
            s->pending = 0;
        } else if ((val & 0x1F) < OT_EDMA_CHANS) {
            unsigned ch = val & 0x1F;

            s->intflags &= ~(1u << ch);
            s->pending &= ~(1u << ch);
            qemu_set_irq(s->irq[ch], 0);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ot_edma_reg_ops = {
    .read = ot_edma_reg_read,
    .write = ot_edma_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t ot_edma_tcd_read(void *opaque, hwaddr addr, unsigned size)
{
    OTEdma *s = opaque;
    int ch = addr / 0x20, off = addr % 0x20;
    uint64_t v = 0;

    if (ch >= OT_EDMA_CHANS) {
        return 0;
    }
    for (unsigned i = 0; i < size; i++) {
        v = (v << 8) | s->tcd[ch][(off + i) & 0x1F];
    }
    return v;
}

static void ot_edma_tcd_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    OTEdma *s = opaque;
    int ch = addr / 0x20, off = addr % 0x20;

    if (ch >= OT_EDMA_CHANS) {
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        s->tcd[ch][(off + i) & 0x1F] = (val >> (8 * (size - 1 - i))) & 0xFF;
    }
    /* Writing CSR with START set is the other way to service a channel. */
    if (off <= 0x1E && off + size > 0x1E && (tcd_r16(s, ch, 0x1E) & CSR_START)) {
        s->defer_irq = true;
        edma_start(s, ch);
        s->defer_irq = false;
    }
}

static const MemoryRegionOps ot_edma_tcd_ops = {
    .read = ot_edma_tcd_read,
    .write = ot_edma_tcd_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* -------------------------------------------------------------- USB device -- */
/*
 * The Chipidea/ARC device controller at 0xFC0B0100-0xFC0B01FF, modeled just
 * deeply enough for USB DISK MODE to be OBSERVABLE: registers are stored and
 * read back (the firmware enters and leaves the mode cleanly against this —
 * measured, no polling wedge), and the one semantic the host cares about is
 * decoded: usb_attach (fw 0x4001eb44) writes USBCMD (+0x140) with bits 16-23
 * cleared to ATTACH, and additionally bit 19 SET to DETACH. Each edge is
 * appended as a line ("attach"/"detach") to the usb-notify file, where the
 * frontend reacts (host-mounting the card image — DISK MODE's
 * contract is that the guest has unmounted its filesystem while attached).
 */
#define OT_USB_BASE   0xFC0B0000u
#define OT_USB_END    0xFC0B0200u
#define OT_USBCMD     0xFC0B0140u
#define OT_USB_DETACH_BIT (1u << 19)

static uint32_t ot_usb_regs[(OT_USB_END - OT_USB_BASE) / 4];
static bool ot_usb_attached;
static const char *ot_usb_notify_path;

static void ot_usb_notify(const char *what)
{
    FILE *f;

    if (!ot_usb_notify_path) {
        return;
    }
    f = fopen(ot_usb_notify_path, "a");
    if (f) {
        fprintf(f, "%s\n", what);
        fclose(f);
    }
}

#define OT_USBMODE    0xFC0B01A8u
#define OT_OTGSC      0xFC0B01A4u
#define OT_OTGSC_BSV  0x00000800u     /* B-session valid: cable present  */
#define OT_OTGSC_BSVIS 0x00080000u    /* B-session valid IRQ status, w1c */
#define OT_OTGSC_BSVIE 0x08000000u    /* B-session valid IRQ enable      */

/* Chipidea operational registers the packet bench decodes. */
#define OT_USBSTS     0xFC0B0144u
#define OT_USBINTR    0xFC0B0148u
#define OT_DEVICEADDR 0xFC0B0154u
#define OT_EPLISTADDR 0xFC0B0158u
#define OT_PORTSC1    0xFC0B0184u
#define OT_EPSETUPSR  0xFC0B01ACu
#define OT_EPPRIME    0xFC0B01B0u
#define OT_EPFLUSH    0xFC0B01B4u
#define OT_EPSR       0xFC0B01B8u
#define OT_EPCOMPLETE 0xFC0B01BCu

#define OT_USBSTS_UI  0x01u
#define OT_USBSTS_PCI 0x04u
#define OT_USBSTS_URI 0x40u
#define OT_USBSTS_W1C 0x01FFu

static qemu_irq ot_usb_irq;
static uint32_t ot_usb_otgsc_is;      /* latched interrupt status bits */

static void ot_usbh_try(void);        /* the packet bench, below */
static bool ot_usbh_enabled(void);
static bool ot_usbh_speed_hs;
static uint32_t ot_usbh_ldl(uint32_t a);
static uint32_t ot_usbh_cur_td[8];

static inline uint32_t *ot_usb_reg(uint32_t a)
{
    return &ot_usb_regs[(a - OT_USB_BASE) / 4];
}

static void ot_usb_irq_update(void)
{
    uint32_t otgsc = *ot_usb_reg(OT_OTGSC);
    bool otg = (otgsc & OT_OTGSC_BSVIE) && (ot_usb_otgsc_is & OT_OTGSC_BSVIS);
    bool dev = (*ot_usb_reg(OT_USBSTS) & *ot_usb_reg(OT_USBINTR)) != 0;

    if (ot_usb_irq) {
        qemu_set_irq(ot_usb_irq, otg || dev);
    }
}

static uint64_t ot_usb_read(hwaddr a)
{
    uint64_t val = ot_usb_regs[(a - OT_USB_BASE) / 4];

    /* The host cable is always plugged: without B-session valid the
     * firmware's DISK MODE never brings the controller up (measured — no
     * bring-up writes until OTGSC reports a session). The controller runs
     * big-endian (USBMODE ES=1, written as raw 0xE), so no byte swap. */
    if (a == OT_OTGSC) {
        val |= OT_OTGSC_BSV | ot_usb_otgsc_is;
    }
    if (ot_usbh_enabled()) {
        if (a == OT_PORTSC1) {
            /* CCS + port speed bits 27:26 (the firmware picks its FS or HS
             * config descriptor from these — usb_cfg_desc_send). */
            val = 1u | (ot_usbh_speed_hs ? (2u << 26) : 0);
        } else if (a == OT_EPFLUSH) {
            val = 0;                   /* flushes complete instantly */
        }
    }
    return val;
}

static void ot_usb_write(hwaddr a, uint64_t val)
{
    uint32_t old = ot_usb_regs[(a - OT_USB_BASE) / 4];

    if (getenv("OCTA_USB_LOG")) {
        fprintf(stderr, "octatrack: usb W +%03x <- %08x\n",
                (unsigned)(a - OT_USB_BASE), (unsigned)val);
    }
    if (a == OT_OTGSC) {
        ot_usb_regs[(a - OT_USB_BASE) / 4] = val;
        /* Status bits are write-1-to-clear. BSVIS latches only on a BSVIE
         * ENABLE EDGE: the cable is constant, so "session became valid"
         * happens once per enable. Latching it on every write with BSVIE
         * set (the first model) made an interrupt storm — the firmware's
         * own ack keeps BSVIE set, which re-latched, which re-interrupted,
         * and the bring-up sequence looped forever (measured). */
        uint32_t was_en = old & OT_OTGSC_BSVIE;

        ot_usb_otgsc_is &= ~(val & OT_OTGSC_BSVIS);
        if ((val & OT_OTGSC_BSVIE) && !was_en) {
            ot_usb_otgsc_is |= OT_OTGSC_BSVIS;
        }
        ot_usb_irq_update();
        return;
    }
    if (ot_usbh_enabled()) {
        switch (a) {
        case OT_USBSTS:                /* write-1-to-clear */
        case OT_EPSETUPSR:
        case OT_EPCOMPLETE:
            *ot_usb_reg(a) = old & ~(uint32_t)val;
            ot_usb_irq_update();
            return;
        case OT_EPPRIME: {
            /* Priming latches the dQH's next-dTD immediately: the prime bit
             * itself clears at once (the firmware POLLS for exactly that
             * after every prime, 0x4001e646) and the transfer stays pending
             * in ENDPTSTAT until the host side moves the bytes. */
            uint32_t eplist = *ot_usb_reg(OT_EPLISTADDR);
            int ep, dir;

            for (ep = 0; ep < 4; ep++) {
                for (dir = 0; dir < 2; dir++) {
                    if ((uint32_t)val & (1u << (ep + (dir ? 16 : 0)))) {
                        uint32_t qh = eplist + (ep * 2 + dir) * 0x40u;

                        ot_usbh_cur_td[ep + 4 * dir] = ot_usbh_ldl(qh + 8);
                    }
                }
            }
            *ot_usb_reg(OT_EPSR) |= (uint32_t)val;
            *ot_usb_reg(OT_EPPRIME) = 0;
            ot_usbh_try();
            return;
        }
        case OT_EPFLUSH:
            *ot_usb_reg(OT_EPSR) &= ~(uint32_t)val;
            *ot_usb_reg(OT_EPFLUSH) = 0;
            return;
        default:
            break;
        }
    }
    ot_usb_regs[(a - OT_USB_BASE) / 4] = val;
    /* No MMIO write is DISK-MODE-specific once the cable is modeled: with
     * B-session valid at boot the firmware brings the whole device
     * controller up immediately (measured: USBMODE=0xE, EPLISTADDR, RUN,
     * USBINTR=0x57 right after the BSV interrupt) — outside disk mode it
     * merely answers SCSI with "no medium". The attach/detach edges for the
     * host handoff come from polling the guest's own mode flag instead
     * (ot_usb_poll_active below). */
}

/* --------------------------------------------------- USB packet bench ---- */
/*
 * A scripted USB HOST for the device controller: -M octatrack,usb-host=PATH
 * listens on a unix socket and speaks a line protocol (tests/usb-host.py
 * is the driver):
 *
 *   setup <16 hex>          deliver a SETUP packet          -> "ok"
 *   in <ep> <maxlen>        run an IN transfer on EP n      -> "in <ep> <hex>"
 *   out <ep> [<hex>]        deliver bytes (or a ZLP) to EP n-> "out <ep> <n>"
 *   reset                   USB bus reset                   -> "ok"
 *   speed hs|fs             port speed for PORTSC1          -> "ok"
 *
 * "in"/"out" replies arrive when the transfer actually completes, which may
 * be after the guest primes the endpoint — the rendezvous is event-driven
 * from both sides (ENDPTPRIME writes and socket commands), one outstanding
 * op per endpoint direction. dQH/dTD parsing follows the Chipidea layout
 * the firmware itself uses (dTD: +0 next, +4 token, +8..0x18 buffer pages;
 * dQH: +8 next-dTD, +0x28 setup buffer). The controller runs big-endian
 * (USBMODE ES=1), so descriptors are read/written with the BE loads the
 * firmware uses.
 */
#define OT_USBH_MAX 8192

typedef struct {
    bool pending;
    int want;
} OtUsbhIn;

typedef struct {
    bool pending;
    int len, off;
    uint8_t data[OT_USBH_MAX];
} OtUsbhOut;

static char *ot_usbh_path;
static int ot_usbh_listen_fd = -1;
static int ot_usbh_fd = -1;
static OtUsbhIn ot_usbh_in[4];
static OtUsbhOut ot_usbh_out[4];
static char ot_usbh_line[2 * OT_USBH_MAX + 64];
static size_t ot_usbh_line_len;
/* ot_usbh_cur_td (declared above): current dTD per endpoint direction
 * (ep + 4*dir). The firmware can chain dTDs (MSC queues INQUIRY data with
 * the CSW linked behind it), so a host op that satisfies its appetite
 * mid-chain must leave the rest pending — the walk resumes there, not from
 * the dQH, and ENDPTSTAT stays set while an ACTIVE dTD remains. Reloaded
 * from dQH+8 on every ENDPTPRIME write. */

static bool ot_usbh_enabled(void)
{
    return ot_usbh_path != NULL;
}

static uint32_t ot_usbh_ldl(uint32_t a)
{
    uint32_t be;

    address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                       &be, sizeof be);
    return ldl_be_p(&be);
}

static void ot_usbh_stl(uint32_t a, uint32_t v)
{
    uint32_t be;

    stl_be_p(&be, v);
    address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                        &be, sizeof be);
}

static void ot_usbh_reply(const char *fmt, ...)
{
    char buf[2 * OT_USBH_MAX + 64];
    va_list ap;
    int n;

    if (ot_usbh_fd < 0) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t ignored = write(ot_usbh_fd, buf, n);
        (void)ignored;
    }
}

/* Copy len bytes between guest memory and buf following the dTD's 4 KB
 * buffer-page list (page 0 carries the starting offset). */
static void ot_usbh_td_copy(uint32_t td, int len, uint8_t *buf, bool to_host)
{
    uint32_t ptr = ot_usbh_ldl(td + 8);
    int page = 0, done = 0;

    while (done < len && ptr) {
        uint32_t page_end = (ptr & ~0xFFFu) + 0x1000u;
        int n = MIN(len - done, (int)(page_end - ptr));

        if (to_host) {
            address_space_read(&address_space_memory, ptr,
                               MEMTXATTRS_UNSPECIFIED, buf + done, n);
        } else {
            address_space_write(&address_space_memory, ptr,
                                MEMTXATTRS_UNSPECIFIED, buf + done, n);
        }
        done += n;
        page++;
        ptr = page <= 4 ? (ot_usbh_ldl(td + 8 + 4 * page) & ~0xFFFu) : 0;
    }
}

/* One rendezvous: guest has an active transfer on (ep, dir) AND the host has
 * an outstanding op for it. Walks the dTD chain from the dQH, moves bytes,
 * retires each dTD (ACTIVE cleared, remaining bytes written back), raises
 * ENDPTCOMPLETE + USBSTS.UI, and answers the host. */
static void ot_usbh_service(int ep, bool dir_in)
{
    uint32_t bit = 1u << (ep + (dir_in ? 16 : 0));
    int slot = ep + (dir_in ? 4 : 0);
    uint32_t td = ot_usbh_cur_td[slot];
    static uint8_t buf[OT_USBH_MAX];
    int moved = 0;

    if (dir_in) {
        OtUsbhIn *op = &ot_usbh_in[ep];

        while (!(td & 1u) && moved < op->want) {
            uint32_t token = ot_usbh_ldl(td + 4);
            int total, n;

            if (!(token & 0x80u)) {
                break;
            }
            total = (token >> 16) & 0x7FFF;
            n = MIN(total, op->want - moved);
            ot_usbh_td_copy(td, n, buf + moved, true);
            moved += n;
            ot_usbh_stl(td + 4, ((uint32_t)(total - n) << 16) |
                                (token & 0x8000u));
            td = ot_usbh_ldl(td);
            if (total < 64) {          /* short packet ends the transfer */
                break;
            }
        }
        op->pending = false;
        if (moved > 0) {
            char hex[2 * OT_USBH_MAX + 1];
            int i;

            for (i = 0; i < moved; i++) {
                sprintf(hex + 2 * i, "%02x", buf[i]);
            }
            ot_usbh_reply("in %d %s\n", ep, hex);
        } else {
            ot_usbh_reply("in %d\n", ep);
        }
    } else {
        OtUsbhOut *op = &ot_usbh_out[ep];

        do {
            uint32_t token;
            int total, n;

            if (td & 1u) {
                break;
            }
            token = ot_usbh_ldl(td + 4);
            if (!(token & 0x80u)) {
                break;
            }
            total = (token >> 16) & 0x7FFF;
            n = MIN(total, op->len - op->off);
            ot_usbh_td_copy(td, n, op->data + op->off, false);
            op->off += n;
            moved += n;
            ot_usbh_stl(td + 4, ((uint32_t)(total - n) << 16) |
                                (token & 0x8000u));
            td = ot_usbh_ldl(td);
        } while (op->off < op->len);
        op->pending = false;
        ot_usbh_reply("out %d %d\n", ep, moved);
    }
    ot_usbh_cur_td[slot] = td;
    /* Chain remainder still ACTIVE -> the transfer stays pending. */
    if ((td & 1u) || !(ot_usbh_ldl(td + 4) & 0x80u)) {
        *ot_usb_reg(OT_EPSR) &= ~bit;
    }
    *ot_usb_reg(OT_EPCOMPLETE) |= bit;
    *ot_usb_reg(OT_USBSTS) |= OT_USBSTS_UI;
    ot_usb_irq_update();
}

static void ot_usbh_try(void)
{
    int ep;

    if (ot_usbh_fd < 0 || !*ot_usb_reg(OT_EPLISTADDR)) {
        return;
    }
    for (ep = 0; ep < 4; ep++) {
        if (ot_usbh_in[ep].pending &&
            (*ot_usb_reg(OT_EPSR) & (1u << (ep + 16)))) {
            ot_usbh_service(ep, true);
        }
        if (ot_usbh_out[ep].pending &&
            (*ot_usb_reg(OT_EPSR) & (1u << ep))) {
            ot_usbh_service(ep, false);
        }
    }
}

static int ot_usbh_hex(const char *s, uint8_t *out, int max)
{
    int n = 0;

    while (s[0] && s[1] && n < max) {
        unsigned v;

        if (sscanf(s, "%2x", &v) != 1) {
            break;
        }
        out[n++] = v;
        s += 2;
    }
    return n;
}

static void ot_usbh_cmd(char *line)
{
    if (getenv("OCTA_USB_LOG")) {
        fprintf(stderr, "octatrack: usbh cmd: %s\n", line);
    }
    if (strncmp(line, "setup ", 6) == 0) {
        uint8_t pkt[8] = { 0 };
        uint32_t eplist = *ot_usb_reg(OT_EPLISTADDR);

        ot_usbh_hex(line + 6, pkt, 8);
        if (!eplist) {
            ot_usbh_reply("err no-eplist\n");
            return;
        }
        /* The setup buffer lives at +0x28 of the EP0 OUT dQH as two
         * LITTLE-ENDIAN dwords — in the big-endian bus view each 4-byte
         * group is wire-reversed, which is exactly what the firmware's
         * byterev of its BE loads undoes (measured: usb_setup_dispatch
         * compares the first processed long against 0x8006xx01). */
        {
            uint8_t rev[8] = { pkt[3], pkt[2], pkt[1], pkt[0],
                               pkt[7], pkt[6], pkt[5], pkt[4] };

            address_space_write(&address_space_memory, eplist + 0x28,
                                MEMTXATTRS_UNSPECIFIED, rev, 8);
        }
        *ot_usb_reg(OT_EPSETUPSR) |= 1u;
        *ot_usb_reg(OT_USBSTS) |= OT_USBSTS_UI;
        ot_usb_irq_update();
        ot_usbh_reply("ok\n");
    } else if (strncmp(line, "in ", 3) == 0) {
        int ep = 0, want = 0;

        sscanf(line + 3, "%d %d", &ep, &want);
        ep &= 3;
        ot_usbh_in[ep].pending = true;
        ot_usbh_in[ep].want = MIN(want, OT_USBH_MAX);
        ot_usbh_try();
    } else if (strncmp(line, "out ", 4) == 0) {
        int ep = 0;
        char *sp;

        sscanf(line + 4, "%d", &ep);
        ep &= 3;
        sp = strchr(line + 4, ' ');
        ot_usbh_out[ep].len = sp ? ot_usbh_hex(sp + 1, ot_usbh_out[ep].data,
                                               OT_USBH_MAX) : 0;
        ot_usbh_out[ep].off = 0;
        ot_usbh_out[ep].pending = true;
        ot_usbh_try();
    } else if (strncmp(line, "reset", 5) == 0) {
        *ot_usb_reg(OT_DEVICEADDR) = 0;
        *ot_usb_reg(OT_EPPRIME) = 0;
        *ot_usb_reg(OT_EPSR) = 0;
        *ot_usb_reg(OT_EPCOMPLETE) = 0;
        *ot_usb_reg(OT_EPSETUPSR) = 0;
        *ot_usb_reg(OT_USBSTS) |= OT_USBSTS_URI | OT_USBSTS_PCI;
        ot_usb_irq_update();
        ot_usbh_reply("ok\n");
    } else if (strncmp(line, "speed ", 6) == 0) {
        ot_usbh_speed_hs = strncmp(line + 6, "hs", 2) == 0;
        ot_usbh_reply("ok\n");
    } else {
        ot_usbh_reply("err unknown\n");
    }
}

static void ot_usbh_read_cb(void *opaque)
{
    char buf[1024];
    ssize_t n = read(ot_usbh_fd, buf, sizeof buf);
    size_t i;

    if (n <= 0) {
        qemu_set_fd_handler(ot_usbh_fd, NULL, NULL, NULL);
        close(ot_usbh_fd);
        ot_usbh_fd = -1;
        ot_usbh_line_len = 0;
        memset(ot_usbh_in, 0, sizeof ot_usbh_in);
        memset(ot_usbh_out, 0, sizeof ot_usbh_out);
        return;
    }
    for (i = 0; i < (size_t)n; i++) {
        if (buf[i] == '\n') {
            ot_usbh_line[ot_usbh_line_len] = 0;
            ot_usbh_cmd(ot_usbh_line);
            ot_usbh_line_len = 0;
        } else if (ot_usbh_line_len + 1 < sizeof ot_usbh_line) {
            ot_usbh_line[ot_usbh_line_len++] = buf[i];
        }
    }
}

static void ot_usbh_accept_cb(void *opaque)
{
    int fd = accept(ot_usbh_listen_fd, NULL, NULL);

    if (fd < 0) {
        return;
    }
    if (ot_usbh_fd >= 0) {
        close(fd);                     /* one host at a time */
        return;
    }
    ot_usbh_fd = fd;
    qemu_set_fd_handler(fd, ot_usbh_read_cb, NULL, NULL);
}

static void ot_usbh_start(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    ot_usbh_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ot_usbh_listen_fd < 0) {
        return;
    }
    pstrcpy(addr.sun_path, sizeof addr.sun_path, path);
    unlink(path);
    if (bind(ot_usbh_listen_fd, (struct sockaddr *)&addr, sizeof addr) ||
        listen(ot_usbh_listen_fd, 1)) {
        close(ot_usbh_listen_fd);
        ot_usbh_listen_fd = -1;
        return;
    }
    qemu_set_fd_handler(ot_usbh_listen_fd, ot_usbh_accept_cb, NULL, NULL);
}

/* ---------------------------------------------------- MAIN audio tap ------ */
/*
 * -M octatrack,audio-tap=on mirrors every shipped MAIN block into guest
 * SDRAM scratch, converted EXACTLY as the frontend's recorder converts
 * (src/audio.c: MAIN = TX 1/2 + 3/4 summed, >>8, int16 clamp, LE
 * interleaved), so the ring is byte-identical to a --recording data chunk.
 * The layout is the USB-audio payload's ABI:
 *
 *   0x48010000  magic 'OTAP'
 *   0x48010004  producer frame count, u32 BE (the guest reads it movel)
 *   0x48010040  ring, 16384 frames x 4 B (int16 LE L, int16 LE R)
 *
 * Called from the vCPU thread (the DSP shim's closeBlock), which is also
 * the pacing tick: when the guest has enabled SOF in USBINTR (bit 7 —
 * stock bring-up writes 0x57, so this is the payload's opt-in) each block
 * raises USBSTS.SRI, and the payload's usb_isr shim builds iso packets in
 * guest-audio time. Default off: with the tap off nothing writes guest
 * memory and the SDRAM canary claims stay exactly as verified.
 */
#define OT_ATAP_BASE   0x48010000u
#define OT_ATAP_STREAM (OT_ATAP_BASE + 0x08u)  /* payload's "EP3 iso up" flag */
#define OT_ATAP_RING   (OT_ATAP_BASE + 0x40u)
#define OT_ATAP_FRAMES 16384u
#define OT_USBSTS_SRI  0x80u

static bool ot_audio_tap_on;
static bool ot_hw_faithful;
static void ot_usb_frame_signal(void);
static uint32_t ot_atap_produced;

/* ---------------------------------------------------------------------------
 * hw-faithful mode: remove the two kindnesses QEMU extends to a guest that a
 * real MCF54455 board does not.
 *
 * (1) SDRAM does not power up as zeros. QEMU's RAM does. A payload that reads
 *     an "uninitialized" scratch word gets 0 here and garbage on silicon, so
 *     scratch is poisoned at init with a pattern that is neither 0 nor a
 *     plausible pointer — any code that consumes un-initialized scratch then
 *     misbehaves HERE, where it is cheap to find.
 * (2) SOF/frame interrupts come from the HOST, at its own rate, whenever the
 *     cable is live — not from anything the guest sets. ot_audio_tap_block()
 *     gates the SOF on the payload's own scratch flag, which means the
 *     EMULATOR enforces a gate the FIRMWARE is supposed to enforce. In
 *     hw-faithful mode the frame signal is raised whenever the device is
 *     enumerated, exactly as a real host does, so a shim that forgets to
 *     check whether its stream is up gets exercised the way hardware
 *     exercises it.
 */
#define OT_SCRATCH_LO   0x48000000u
#define OT_SCRATCH_HI   0x4ec94800u
#define OT_SCRATCH_POISON 0xa5c3a5c3u   /* not 0, not a valid pointer */

static void ot_scratch_poison(void)
{
    uint32_t buf[1024];
    uint32_t a;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(buf); i++) {
        buf[i] = OT_SCRATCH_POISON;
    }
    for (a = OT_SCRATCH_LO; a < OT_SCRATCH_HI; a += sizeof buf) {
        uint32_t n = OT_SCRATCH_HI - a;

        if (n > sizeof buf) {
            n = sizeof buf;
        }
        address_space_write(&address_space_memory, a,
                            MEMTXATTRS_UNSPECIFIED, buf, n);
    }
}

void ot_audio_tap_block(const int32_t (*out)[8])
{
    uint8_t buf[16 * 4];
    uint32_t be;
    int f;

    if (!ot_audio_tap_on) {
        /* No tap, but the host's frame clock does not depend on the tap. */
        ot_usb_frame_signal();
        return;
    }
    for (f = 0; f < 16; f++) {
        const int32_t l = (out[f][1] >> 8) + (out[f][3] >> 8);
        const int32_t r = (out[f][2] >> 8) + (out[f][4] >> 8);
        const int16_t sl = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        const int16_t sr = r > 32767 ? 32767 : r < -32768 ? -32768 : r;

        buf[4 * f + 0] = (uint8_t)sl;
        buf[4 * f + 1] = (uint8_t)((uint16_t)sl >> 8);
        buf[4 * f + 2] = (uint8_t)sr;
        buf[4 * f + 3] = (uint8_t)((uint16_t)sr >> 8);
    }
    if (ot_atap_produced == 0) {
        uint32_t zero = 0;

        stl_be_p(&be, 0x4f544150u);            /* 'OTAP' */
        address_space_write(&address_space_memory, OT_ATAP_BASE,
                            MEMTXATTRS_UNSPECIFIED, &be, sizeof be);
        /* Clear the stream flag: scratch is not zeroed, and only the payload
         * (once loaded) should ever raise it. */
        address_space_write(&address_space_memory, OT_ATAP_STREAM,
                            MEMTXATTRS_UNSPECIFIED, &zero, sizeof zero);
    }
    /* 16 divides the ring size, so a block never wraps mid-write. */
    address_space_write(&address_space_memory,
                        OT_ATAP_RING + (ot_atap_produced % OT_ATAP_FRAMES) * 4,
                        MEMTXATTRS_UNSPECIFIED, buf, sizeof buf);
    ot_atap_produced += 16;
    stl_be_p(&be, ot_atap_produced);
    address_space_write(&address_space_memory, OT_ATAP_BASE + 4,
                        MEMTXATTRS_UNSPECIFIED, &be, sizeof be);
    /* The block-clock SOF, raised only while the payload has EP3 iso up: it
     * sets a streaming flag in its own scratch (0x48010008) at alt 1 and
     * clears it at alt 0. Gating on that — not on a controller register —
     * keeps stock images untouched AND survives the firmware's own
     * usb_dev_bringup, which re-initializes USBINTR (measured: it wiped a
     * USBINTR SOF-enable bit the payload had set).
     * usb_isr reaches the shim site (0x4001e606) only on the UI path
     * (btst #0 at 0x4001e5f6 branches away otherwise), so raise UI (already
     * enabled in the stock USBINTR 0x57) alongside the SRI signal: the shim
     * runs, finds no EP completion pending, and kicks on the SRI bit. UI with
     * no completion is a no-op in the stock dispatch. */
    ot_usb_frame_signal();
}

/* The host's frame clock. In hw-faithful mode it is raised whenever the
 * device is enumerated, like a real host's 1 kHz SOF; otherwise it is gated
 * on the payload's own scratch flag (the historical behaviour, which lets the
 * emulator stand in for a gate the firmware should own). */
static void ot_usb_frame_signal(void)
{
    uint32_t flag = 0;

    if (!ot_usbh_enabled()) {
        return;
    }
    if (!ot_hw_faithful) {
        address_space_read(&address_space_memory, OT_ATAP_STREAM,
                           MEMTXATTRS_UNSPECIFIED, &flag, sizeof flag);
        if (!flag) {
            return;
        }
    }
    bql_lock();
    *ot_usb_reg(OT_USBSTS) |= OT_USBSTS_SRI | OT_USBSTS_UI;
    ot_usb_irq_update();
    bql_unlock();
}

/* The firmware's own "USB DISK MODE is active" flag (usb_diskmode_active,
 * 0x460e76a0): 1 exactly while the mode is active, set AFTER the guest has
 * unmounted its filesystem and cleared at exit before remounting — the
 * correct, unambiguous handoff edge for the host mount. Polled at 5 Hz on
 * the virtual clock. */
#define OT_USB_ACTIVE_FLAG 0x460e76a0u

static QEMUTimer *ot_usb_poll_timer;

static void ot_usb_poll_active(void *opaque)
{
    uint32_t flag = 0;
    bool active;

    address_space_read(&address_space_memory, OT_USB_ACTIVE_FLAG,
                       MEMTXATTRS_UNSPECIFIED, &flag, sizeof flag);
    active = flag != 0;
    if (active != ot_usb_attached) {
        ot_usb_attached = active;
        ot_usb_notify(active ? "attach" : "detach");
    }
    timer_mod(ot_usb_poll_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200 * 1000000LL);
}

/* ------------------------------------------------------------------- MBAR -- */

static uint64_t ot_mbar_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = 0;

    switch (OT_MBAR_BASE + addr) {
    case OT_PLL_PCR:
        val = OT_PCR_VALUE;
        break;
    case OT_PPDSDR_UART:
        val = ot_ata_ppdsdr_uart();
        break;
    case OT_PPDSDR_PCI:
        val = 0xFF;                        /* active-low senses: none asserted */
        break;
    case OT_PPDSDR_P10:
        val = ot_gpio_p10;
        if (!ot_mk1 && (ot_gpio_p10 & OT_P10_DRIVE)) {
            val |= OT_P10_SENSE;           /* MKII: bit 6 follows bit 5 */
        }
        break;
    default:
        if (OT_MBAR_BASE + addr >= OT_USB_BASE &&
            OT_MBAR_BASE + addr < OT_USB_END) {
            val = ot_usb_read(OT_MBAR_BASE + addr);
            break;
        }
        qemu_log_mask(LOG_UNIMP, "MBAR R %#010" HWADDR_PRIx " %u -> 0\n",
                      OT_MBAR_BASE + addr, size);
        break;
    }
    return val;
}

static void ot_mbar_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    switch (OT_MBAR_BASE + addr) {
    case OT_PPDSDR_P10:                    /* writing 1s SETS pins   */
        ot_gpio_p10 |= val & 0xFF;
        break;
    case OT_PCLRR_P10:                     /* writing 0s CLEARS pins */
        ot_gpio_p10 &= val & 0xFF;
        break;
    case OT_CORE_SELECT:
        ot_dsp_core_select(val);
        break;
    default:
        if (OT_MBAR_BASE + addr >= OT_USB_BASE &&
            OT_MBAR_BASE + addr < OT_USB_END) {
            ot_usb_write(OT_MBAR_BASE + addr, val);
            break;
        }
        qemu_log_mask(LOG_UNIMP,
                      "MBAR W %#010" HWADDR_PRIx " %u <- %#010" PRIx64 "\n",
                      OT_MBAR_BASE + addr, size, val);
        break;
    }
}

static const MemoryRegionOps ot_mbar_ops = {
    .read = ot_mbar_read,
    .write = ot_mbar_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ NVRAM -- */
/*
 * On the real unit the CS1 SRAM is battery-backed: the mounted set and loaded
 * project names live in it, which is what makes "boots into the previous
 * project" possible — and why a project's live state can be present with the
 * card showing nothing. The nvram property attaches a 1 MB backing file,
 * loaded at start and written back at exit. Not a savestate — one region only.
 */
static void *ot_nvram_ptr;
static const char *ot_nvram_path;

static void ot_nvram_save(void)
{
    FILE *f = fopen(ot_nvram_path, "wb");

    if (!f || fwrite(ot_nvram_ptr, 1, OT_CS1_SIZE, f) != OT_CS1_SIZE) {
        warn_report("octatrack: cannot write NVRAM %s", ot_nvram_path);
    }
    if (f) {
        fclose(f);
    }
}

static void ot_nvram_attach(MemoryRegion *cs1, const char *path)
{
    FILE *f;

    if (!path || !*path) {
        return;
    }
    ot_nvram_ptr = memory_region_get_ram_ptr(cs1);
    ot_nvram_path = g_strdup(path);
    f = fopen(path, "rb");
    if (f) {
        size_t got = fread(ot_nvram_ptr, 1, OT_CS1_SIZE, f);

        fclose(f);
        info_report("octatrack: NVRAM %s restored (%zu bytes)", path, got);
    }
    atexit(ot_nvram_save);
}

/* ------------------------------------------------------------------ board -- */

static void octatrack_init(MachineState *machine)
{
    OctatrackMachineState *om = OCTATRACK_MACHINE(machine);
    M68kCPU *cpu = M68K_CPU(cpu_create(machine->cpu_type));
    CPUM68KState *env = &cpu->env;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *lowram = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *cs1 = g_new(MemoryRegion, 1);
    MemoryRegion *mbar = g_new(MemoryRegion, 1);
    const char *image = machine->kernel_filename;
    hwaddr entry = OT_IMAGE_LOAD;
    DeviceState *intc, *intc1;
    int size;

    ot_mk1 = om->mk1;
    env->vbr = 0;

    memory_region_add_subregion(sysmem, OT_SDRAM_BASE, machine->ram);
    memory_region_init_ram(lowram, NULL, "octatrack.lowram",
                           OT_LOWRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, 0, lowram);
    memory_region_init_ram(sram, NULL, "octatrack.sram",
                           OT_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, OT_SRAM_BASE, sram);
    memory_region_init_ram(cs1, NULL, "octatrack.cs1",
                           OT_CS1_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, OT_CS1_BASE, cs1);
    ot_nvram_attach(cs1, om->nvram);
    if (om->usb_notify) {
        ot_usb_notify_path = g_strdup(om->usb_notify);
        ot_usb_poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         ot_usb_poll_active, NULL);
        timer_mod(ot_usb_poll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000 * 1000000LL);
    }
    if (om->usb_host) {
        ot_usbh_path = g_strdup(om->usb_host);
        ot_usbh_start(ot_usbh_path);
    }
    ot_audio_tap_on = om->audio_tap;
    ot_hw_faithful = om->hw_faithful;
    if (ot_hw_faithful) {
        ot_scratch_poison();
    }

    /* MBAR catch-all at the lowest priority; real models overlap it. */
    memory_region_init_io(mbar, NULL, &ot_mbar_ops, NULL,
                          "octatrack.mbar", OT_MBAR_SIZE);
    memory_region_add_subregion_overlap(sysmem, OT_MBAR_BASE, mbar, 0);

    intc = mcf_intc_init(sysmem, OT_INTC0_BASE, cpu);
    intc1 = mcf_intc_init_vec(sysmem, OT_INTC1_BASE, cpu, 128);

    ot_dsp_install(sysmem,
                    (struct IRQState *)qdev_get_gpio_in(intc, OT_EPORT_IRQ1_SRC),
                    om->dsp_audio, om->throttle, om->interleave,
                    om->exit_with_frontend);
    ot_dsp_hold_set(om->hold);
    /* Before any TB is translated: the decrementer lives in the TB preamble. */
    ot_insn_budget_enable(om->interleave, ot_guest_progress);
    if (om->capture) {
        ot_capture_wire = g_str_has_suffix(om->capture, ".wire");
        ot_capture_fp = fopen(om->capture, "w");
        if (!ot_capture_fp) {
            error_report("octatrack: cannot write capture %s", om->capture);
        }
    }

    {
        OTDspi *spi = g_new0(OTDspi, 1);

        memory_region_init_io(&spi->iomem, NULL, &ot_dspi_ops, spi,
                              "octatrack.dspi", 0x100);
        memory_region_add_subregion_overlap(sysmem, OT_DSPI_BASE,
                                            &spi->iomem, 1);
    }

    for (int i = 0; i < 4; i++) {
        OTPit *pit = g_new0(OTPit, 1);

        pit->idx = i;
        pit->timer = ptimer_init(ot_pit_trigger, pit, PTIMER_POLICY_LEGACY);
        pit->irq = qdev_get_gpio_in(intc1, OT_PIT_SRC + i);
        memory_region_init_io(&pit->iomem, NULL, &ot_pit_ops, pit,
                              "octatrack.pit", 0x4000);
        memory_region_add_subregion_overlap(sysmem,
                                            OT_PIT0_BASE + 0x4000 * i,
                                            &pit->iomem, 1);
    }

    for (int i = 0; i < 4; i++) {
        OTDtim *t = g_new0(OTDtim, 1);

        t->idx = i;
        t->irq = qdev_get_gpio_in(intc, OT_DTIM_SRC + i);
        t->ref = timer_new_ns(QEMU_CLOCK_VIRTUAL, ot_dtim_ref_fire, t);
        /* Page-sized: see the eDMA TCD region below. */
        memory_region_init_io(&t->iomem, NULL, &ot_dtim_ops, t,
                              "octatrack.dtim", 0x1000);
        memory_region_add_subregion_overlap(sysmem,
                                            OT_DTIM0_BASE + 0x4000 * i,
                                            &t->iomem, 1);
    }

    {
        OTEdma *ed = g_new0(OTEdma, 1);

        ot_edma = ed;
        memory_region_init_io(&ed->reg, NULL, &ot_edma_reg_ops, ed,
                              "octatrack.edma", 0x1000);
        memory_region_add_subregion_overlap(sysmem, OT_EDMA_BASE, &ed->reg, 1);
        /* Page-sized, not the 0x400 the TCDs occupy: an MMIO region shorter
         * than a target page makes QEMU render the page as a SUBPAGE, and
         * every access then pays a second full memory_region_dispatch round
         * trip. The TCD is written several times per audio block. */
        memory_region_init_io(&ed->tcdmem, NULL, &ot_edma_tcd_ops, ed,
                              "octatrack.edma-tcd", 0x1000);
        memory_region_add_subregion_overlap(sysmem, OT_EDMA_TCD,
                                            &ed->tcdmem, 1);
        for (int i = 0; i < OT_EDMA_CHANS; i++) {
            ed->irq[i] = qdev_get_gpio_in(intc, OT_EDMA_SRC + i);
        }
    }

    ot_ata_create(sysmem, 0x90000000, qdev_get_gpio_in(intc1, OT_ATA_SRC));
    ot_usb_irq = qdev_get_gpio_in(intc1, OT_USB_SRC);

    mcf_uart_create_mmap(OT_UART0_BASE,
                         qdev_get_gpio_in(intc, OT_UART0_SRC), serial_hd(0));
    ot_panel_uart_create(sysmem, OT_UART1_BASE,
                          qdev_get_gpio_in(intc, OT_UART1_SRC), serial_hd(1),
                          om->mk1);
    mcf_uart_create_mmap(OT_UART2_BASE,
                         qdev_get_gpio_in(intc, OT_UART2_SRC), serial_hd(2));

    if (!image) {
        if (qtest_enabled()) {
            return;
        }
        error_report("an Octatrack MAIN OS section is required: "
                     "-kernel out/os/main.bin");
        exit(1);
    }

    /* MAIN OS is a raw blob at OT_IMAGE_LOAD; bare-metal tests are ELFs. */
    {
        uint64_t elf_entry = 0;

        size = load_elf(image, NULL, NULL, NULL, &elf_entry,
                        NULL, NULL, NULL, ELFDATA2MSB, EM_68K, 0, 0);
        if (size > 0) {
            entry = elf_entry;
        } else {
            size = load_image_targphys(
                image, OT_IMAGE_LOAD,
                machine->ram_size - (OT_IMAGE_LOAD - OT_SDRAM_BASE), NULL);
            if (size < 0) {
                error_report("could not load '%s' as ELF or raw", image);
                exit(1);
            }
        }
    }

    /* The bootstrap version stamp: the fast path into the DSP upload. */
    {
        uint8_t stamp[2] = { OT_BOOTSTRAP_VERSION >> 8,
                             OT_BOOTSTRAP_VERSION & 0xFF };

        address_space_write(&address_space_memory, OT_BOOTSTRAP_VERSION_ADDR,
                            MEMTXATTRS_UNSPECIFIED, stamp, sizeof(stamp));
    }

    env->pc = entry;
    env->aregs[7] = OT_INIT_SP;
}

static bool ot_get_mk1(Object *obj, Error **errp)
{
    return OCTATRACK_MACHINE(obj)->mk1;
}

static void ot_set_mk1(Object *obj, bool value, Error **errp)
{
    OCTATRACK_MACHINE(obj)->mk1 = value;
}

static char *ot_get_nvram(Object *obj, Error **errp)
{
    return g_strdup(OCTATRACK_MACHINE(obj)->nvram);
}

static void ot_set_nvram(Object *obj, const char *value, Error **errp)
{
    OctatrackMachineState *om = OCTATRACK_MACHINE(obj);

    g_free(om->nvram);
    om->nvram = g_strdup(value);
}

static char *ot_get_dsp_audio(Object *obj, Error **errp)
{
    return g_strdup(OCTATRACK_MACHINE(obj)->dsp_audio);
}

static void ot_set_dsp_audio(Object *obj, const char *value, Error **errp)
{
    OctatrackMachineState *om = OCTATRACK_MACHINE(obj);

    g_free(om->dsp_audio);
    om->dsp_audio = g_strdup(value);
}

static char *ot_get_capture(Object *obj, Error **errp)
{
    return g_strdup(OCTATRACK_MACHINE(obj)->capture);
}

static void ot_set_capture(Object *obj, const char *value, Error **errp)
{
    OctatrackMachineState *om = OCTATRACK_MACHINE(obj);

    g_free(om->capture);
    om->capture = g_strdup(value);
}

static char *ot_get_usb_notify(Object *obj, Error **errp)
{
    return g_strdup(OCTATRACK_MACHINE(obj)->usb_notify);
}

static char *ot_get_usb_host(Object *obj, Error **errp)
{
    return g_strdup(OCTATRACK_MACHINE(obj)->usb_host);
}

static void ot_set_usb_host(Object *obj, const char *value, Error **errp)
{
    OctatrackMachineState *om = OCTATRACK_MACHINE(obj);

    g_free(om->usb_host);
    om->usb_host = g_strdup(value);
}

static void ot_set_usb_notify(Object *obj, const char *value, Error **errp)
{
    OctatrackMachineState *om = OCTATRACK_MACHINE(obj);

    g_free(om->usb_notify);
    om->usb_notify = g_strdup(value);
}

/*
 * The throttle. The Octatrack's block clock is 44.1 kHz / 16 frames and the
 * shim holds it to exactly that — REAL TIME, what the hardware does. There is
 * no divisor: the only other state is `throttle=off`, which runs flat out so
 * capacity can be measured, and which is not usable for anything else.
 *
 * ☠ Throttling lives HERE, not in the frontend, and it is not optional. The guest
 * pays several hundred microseconds of TCG time per block for its frame ISR
 * and state machine; consuming blocks at the emulator's raw capacity saturates
 * the vCPU and starves the guest's IPL-4 key parser — taps stop registering
 * long before the audio does. Anything that loosens the throttle must re-test
 * KEY DELIVERY, not just blocks/s.
 */
static bool ot_get_throttle(Object *obj, Error **errp)
{
    return OCTATRACK_MACHINE(obj)->throttle;
}

static void ot_set_throttle(Object *obj, bool value, Error **errp)
{
    OCTATRACK_MACHINE(obj)->throttle = value;
}

/*
 * The interleave quantum: guest instructions between DSP slices.
 *
 * ☠ It is a RATIO, not a rate, and it is what ties the two chips together.
 * ColdFire fsys is 264 MHz and the DSP is two cores at 200 MHz, but that
 * datasheet ratio is only a starting guess: DSP56300 retires ~1 instruction
 * per cycle, ColdFire V4e does not, and QEMU models no cycle timing at all.
 * The number is calibrated against an OBSERVABLE instead — whether the frame
 * ISR finishes delivering the block's arms before the codec crosses into bank
 * setup at P:0x54, and by how much. See ot_dsp_stats()'s margin histogram.
 *
 * 0 (default) leaves the DSP on its own thread, arbitrated by the shim's
 * mutex, exactly as before — so both designs live in ONE binary and can be
 * A/B'd interleaved, which is the only kind of throughput comparison this
 * repo trusts.
 */
static bool ot_get_audio_tap(Object *obj, Error **errp)
{
    return OCTATRACK_MACHINE(obj)->audio_tap;
}

static void ot_set_audio_tap(Object *obj, bool value, Error **errp)
{
    OCTATRACK_MACHINE(obj)->audio_tap = value;
}

/*
 * exit-with-frontend: quit once the frontend that launched us disconnects.
 *
 * octemu kills its QEMU child on every exit path it can see, but
 * SIGKILL and a crash are not among them, and an orphaned QEMU keeps
 * running at full pace — several have survived one session that way, burning a
 * core each and poisoning every timing measurement taken afterwards. The
 * frontend is the only thing that ever connects to the audio socket, so its
 * disconnect IS the signal. Off by default, so a QEMU started by hand still
 * waits for a frontend to attach, and still survives one detaching.
 */
static bool ot_get_exit_with_frontend(Object *obj, Error **errp)
{
    return OCTATRACK_MACHINE(obj)->exit_with_frontend;
}

static void ot_set_exit_with_frontend(Object *obj, bool value, Error **errp)
{
    OCTATRACK_MACHINE(obj)->exit_with_frontend = value;
}

static bool ot_get_hw_faithful(Object *obj, Error **errp)
{
    return OCTATRACK_MACHINE(obj)->hw_faithful;
}

static void ot_set_hw_faithful(Object *obj, bool value, Error **errp)
{
    OCTATRACK_MACHINE(obj)->hw_faithful = value;
}

static bool ot_get_hold(Object *obj, Error **errp)
{
    return OCTATRACK_MACHINE(obj)->hold;
}

static void ot_set_hold(Object *obj, bool value, Error **errp)
{
    OCTATRACK_MACHINE(obj)->hold = value;
}

static void ot_get_interleave(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    uint32_t value = OCTATRACK_MACHINE(obj)->interleave;

    visit_type_uint32(v, name, &value, errp);
}

static void ot_set_interleave(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    OctatrackMachineState *om = OCTATRACK_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value) {
        error_setg(errp, "interleave must be nonzero: the DSP is stepped from "
                         "the vCPU and there is no other clock to step it on");
        return;
    }
    om->interleave = value;
}

static void octatrack_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Elektron Octatrack MKII (MCF54455), v2";
    mc->init = octatrack_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("cfv4e");
    mc->default_ram_id = "octatrack.ram";
    /* 256 MiB: the storage buffers span up to 244.7 MB above the SDRAM base. */
    mc->default_ram_size = 256 * MiB;

    object_class_property_add_bool(oc, "mk1", ot_get_mk1, ot_set_mk1);
    object_class_property_set_description(oc, "mk1",
        "Present as a MKI (board-revision strap open; 57-key panel map)");
    object_class_property_add_str(oc, "nvram", ot_get_nvram, ot_set_nvram);
    object_class_property_set_description(oc, "nvram",
        "Battery-backed CS1 SRAM image (1 MB), loaded at start, saved at exit");
    object_class_property_add_str(oc, "dsp-audio",
                                  ot_get_dsp_audio, ot_set_dsp_audio);
    object_class_property_add_str(oc, "usb-notify", ot_get_usb_notify,
                                  ot_set_usb_notify);
    object_class_property_set_description(oc, "usb-notify",
        "append 'attach'/'detach' lines to this file as the guest's USB "
        "DISK MODE attaches/detaches the device controller");
    object_class_property_add_str(oc, "usb-host", ot_get_usb_host,
                                  ot_set_usb_host);
    object_class_property_set_description(oc, "usb-host",
        "listen on this unix socket as a scripted USB host driving the "
        "device controller packet bench (tests/usb-host.py)");
    object_class_property_add_str(oc, "capture", ot_get_capture,
                                  ot_set_capture);
    object_class_property_set_description(oc, "capture",
        "write the per-block DSP arms to this file, for octdsp --replay");
    object_class_property_set_description(oc, "dsp-audio",
        "Unix socket announcing the shared-memory audio ring (listener)");
    object_class_property_add_bool(oc, "throttle",
                                   ot_get_throttle, ot_set_throttle);
    object_class_property_set_description(oc, "throttle",
        "Hold the block clock to REAL TIME (default on). Off runs flat out and "
        "is a MEASUREMENT mode — capacity cannot be read through a throttle");
    object_class_property_add(oc, "interleave", "uint32",
                              ot_get_interleave, ot_set_interleave, NULL, NULL);
    object_class_property_set_description(oc, "interleave",
        "Guest instructions per DSP slice (default 512). ☠ Cannot be "
        "interpolated: 768 measures far worse than either 512 or 1024");
    object_class_property_add_bool(oc, "hold", ot_get_hold, ot_set_hold);
    object_class_property_set_description(oc, "hold",
        "The shim's delivery hold (default on); off measures the straddle rate");
    object_class_property_add_bool(oc, "exit-with-frontend",
                                   ot_get_exit_with_frontend,
                                   ot_set_exit_with_frontend);
    object_class_property_set_description(oc, "exit-with-frontend",
        "quit when the frontend that spawned this machine disconnects");
    object_class_property_add_bool(oc, "hw-faithful",
                                   ot_get_hw_faithful, ot_set_hw_faithful);
    object_class_property_set_description(oc, "hw-faithful",
        "Model what real hardware does and QEMU otherwise hides: poison the "
        "0x48000000 scratch region (SDRAM is not zero at power-on) and raise "
        "the USB frame signal whenever the device is enumerated, instead of "
        "gating it on the guest payload's own flag (default off)");
    object_class_property_add_bool(oc, "audio-tap",
                                   ot_get_audio_tap, ot_set_audio_tap);
    object_class_property_set_description(oc, "audio-tap",
        "Mirror every shipped MAIN block into guest SDRAM at 0x48010000 "
        "(recorder-exact int16 LE) and raise SOF per block while the guest "
        "enables it — the USB-audio payload's source ring (default off)");
}

/*
 * The Octatrack runs at REAL TIME, which is what the hardware does, and there
 * is no longer a divisor to pick — only `throttle=off`, which is a measurement
 * mode.
 *
 * It used to be a divisor defaulting to 2 (half real time), and that was a
 * FOSSIL. The reasoning for 2 was
 * "capacity measures 0.784x even at host load average 25, so 0.5x keeps ~36%
 * headroom" — i.e. it was chosen when the emulator could not run real time at
 * all. Capacity is now 1.465x, so real time is 68% utilisation and ~32%
 * headroom, within a point of what justified pace 2. The rule outlived its
 * number.
 *
 * Re-run before changing: the sweep this file used to demand, 8 runs per arm,
 * real time against half speed, INTERLEAVED because host conditions drift
 * over 16 minutes. Result: 8/8 fully clean on BOTH arms — trig landed, audio clean,
 * bursts 1 and 2 bit-identical — against 6/8 each in the original sweep, and
 * zero live straddles either way. Real time additionally puts PIT0's
 * preemption rate at 1.06x the silicon ratio where half speed reads 2.09x
 * (the per-block transfer sequence), so real time is
 * not a cost here, it is the correction.
 *
 * ☠ The canary is KEY DELIVERY, not audio: at capacity the vCPU saturates and
 * the guest's IPL-4 key parser starves long before the sound does. Judge any
 * future change on whether the trig LANDED (ship_nz, sectors_r), not on how
 * the recording sounds.
 *
 * `throttle=off` runs flat out and is a MEASUREMENT mode, not a setting —
 * It is how capacity is read, which cannot be read through a
 * throttle because any throttle is a cap that cannot measure above itself.
 */
static void octatrack_instance_init(Object *obj)
{
    OCTATRACK_MACHINE(obj)->throttle = true;
    OCTATRACK_MACHINE(obj)->hold = true;
    OCTATRACK_MACHINE(obj)->interleave = OT_INTERLEAVE_DEFAULT;
}

static const TypeInfo octatrack_machine_info = {
    .name          = TYPE_OCTATRACK_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(OctatrackMachineState),
    .instance_init = octatrack_instance_init,
    .class_init    = octatrack_machine_class_init,
};

static void octatrack_machine_register_types(void)
{
    type_register_static(&octatrack_machine_info);
}

type_init(octatrack_machine_register_types)
