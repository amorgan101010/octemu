/*
 * Octatrack DSP56721 host port (HDI24) at FlexBus CS2, 0x20000000 — the QEMU
 * side of the real DSP cores in ot-dsp-shim.cc.
 *
 * The port is a 16-bit FlexBus slave: +0x14 latches bits 23-16 (TXH) and a
 * 16-bit write to +0x1C launches one 24-bit word; the latch persists across
 * words, exactly as the silicon's TXH does. A 32-bit access (the eDMA's data
 * beats) is two 16-bit halves, high first. Reads mirror that: +0x1C consumes
 * the DSP's word, +0x14/+0x18 peek.
 *
 * Register model (read-disasm from the firmware's HDI24 driver):
 *   +0x00 ICR   0x81 = INIT|RREQ, a per-transfer FIFO reset (drain THEN clear)
 *   +0x04 CVR   host command; the guest polls bit 7 until the DSP takes it
 *   +0x08 ISR   bit 0 RXDF, bit 1 TXDE, bit 2 TRDY
 *
 * Port operations KEEP the BQL: every wait inside the shim is bounded and the
 * DSP worker runs without QEMU locks, so nothing here can deadlock the main
 * loop — and a bql_unlock/bql_lock pair per port beat measured ~30 us, which
 * capped the block loop at 32 blocks/s.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "ot-qemu.h"

/* A whole target page, not the 0x100 the port decodes: an MMIO region that
 * covers less than a page makes QEMU render the page as a SUBPAGE, and every
 * access then pays a second full memory_region_dispatch round trip. The port
 * is the hottest MMIO on the board outside card I/O (16 writes + 32 reads
 * per audio block). The undecoded tail reads 0 and logs on write. */
#define OT_DSP_SIZE   0x1000
#define OT_ISR_RXDF   (1u << 0)
#define OT_ISR_TXDE   (1u << 1)
#define OT_ISR_TRDY   (1u << 2)

typedef struct {
    MemoryRegion iomem;
    qemu_irq hreq;
    unsigned sel;              /* GPIO-selected core                     */
    uint32_t txh;              /* +0x14 latch, bits 23-16, persistent    */
    bool rreq;                 /* ICR[RREQ] gates the HREQ line          */
    int hreq_level;
    uint64_t frame_reads;      /* 16-bit pops ~= frame ISRs              */
} OTDsp;

static OTDsp *ot_dsp;

void ot_dsp_core_select(uint32_t value)
{
    if (ot_dsp) {
        ot_dsp->sel = value & 1;
    }
}

/* BQL held (every caller is an MMIO handler). */
static void ot_dsp_hreq_update(OTDsp *s)
{
    int level = s->rreq && ot_dspcore_hreq();

    s->hreq_level = level;
    qemu_set_irq(s->hreq, level);
}

/*
 * The interleaved form: called from the vCPU at a TB boundary with NO BQL
 * held, at the end of an interleave tick.
 *
 * It takes the BQL rather than writing the line bare. The m68k pending
 * level/vector must only ever be written under it — a bare cross-thread
 * qemu_set_irq was measured to LOSE other interrupt sources (panel keys
 * stopped landing). Here we ARE the vCPU, but the main loop's timers
 * (eDMA completion, ATA INTRQ, the panel link) drive the same INTC, so the
 * lock is still what keeps that state consistent. A TB boundary is the safe
 * point to take it: cpu_handle_interrupt does exactly this, two frames up.
 */
void ot_dsp_hreq_sync(void)
{
    OTDsp *s = ot_dsp;
    int level;

    if (!s) {
        return;
    }
    level = s->rreq && ot_dspcore_hreq();
    if (level == s->hreq_level) {
        return;
    }
    bql_lock();
    s->hreq_level = level;
    qemu_set_irq(s->hreq, level);
    bql_unlock();
}

static uint64_t ot_dsp_read(void *opaque, hwaddr addr, unsigned size)
{
    OTDsp *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case 0x04:                                   /* CVR: command taken */
        return 0;
    case 0x08:
        return OT_ISR_TRDY | OT_ISR_TXDE
             | (ot_dspcore_rx_pending(s->sel) ? OT_ISR_RXDF : 0);
    case 0x14:
        return (ot_dspcore_rx_peek(s->sel) >> 16) & 0xFF;
    case 0x18:
        return (ot_dspcore_rx_peek(s->sel) >> 8) & 0xFFFF;
    case 0x1C:
        if (size == 4) {
            uint32_t hi = ot_dspcore_rx_pop(s->sel);
            uint32_t lo = ot_dspcore_rx_pop(s->sel);

            val = ((hi & 0xFFFF) << 16) | (lo & 0xFFFF);
        } else {
            val = ot_dspcore_rx_pop(s->sel) & 0xFFFF;
            s->frame_reads++;
        }
        ot_dsp_hreq_update(s);
        return val;
    default:
        return 0;
    }
}

static void ot_dsp_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    OTDsp *s = opaque;

    switch (addr) {
    case 0x00:                                   /* ICR */
        s->rreq = val & 1;
        if (val & 0x80) {
            ot_dspcore_icr(s->sel, val);
        }
        ot_dsp_hreq_update(s);
        break;
    case 0x04:                                   /* CVR */
        {
            void ot_wirelog_cmd(unsigned core, unsigned val);
            ot_wirelog_cmd(s->sel, val);
        }
        ot_dspcore_cvr(s->sel, val);
        break;
    case 0x14:
        s->txh = val & 0xFF;
        break;
    case 0x1C:
        {
            void ot_wirelog_tx(unsigned core, unsigned word);
            if (size == 4) {
                ot_wirelog_tx(s->sel, (s->txh << 16) | ((val >> 16) & 0xFFFF));
                ot_wirelog_tx(s->sel, (s->txh << 16) | (val & 0xFFFF));
            } else {
                ot_wirelog_tx(s->sel, (s->txh << 16) | (val & 0xFFFF));
            }
        }
        if (size == 4) {
            ot_dspcore_write(s->sel, (s->txh << 16) | ((val >> 16) & 0xFFFF));
            ot_dspcore_write(s->sel, (s->txh << 16) | (val & 0xFFFF));
        } else {
            ot_dspcore_write(s->sel, (s->txh << 16) | (val & 0xFFFF));
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "DSP  W +%#04tx <- %#08" PRIx64 "\n",
                      (ptrdiff_t)addr, val);
        break;
    }
}

static const MemoryRegionOps ot_dsp_ops = {
    .read = ot_dsp_read,
    .write = ot_dsp_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* Which core the GPIO core-select currently addresses. The block capture needs
 * it: the two cores get different arms and a replay must keep them apart. */
unsigned ot_dsp_selected_core(void)
{
    return ot_dsp ? ot_dsp->sel : 0;
}

void ot_dsp_edma_write(const uint8_t *buf, uint32_t nbytes)
{
    ot_dspcore_write_burst(ot_dsp->sel, ot_dsp->txh, buf, nbytes / 2);
}

void ot_dsp_edma_read(uint8_t *buf, uint32_t nbytes)
{
    ot_dspcore_read_burst(ot_dsp->sel, buf, nbytes / 2);
    ot_dsp_hreq_update(ot_dsp);
}

static void ot_dsp_report(void)
{
    char dsp[1024], edma[128], ata[128], panel[128], timers[160];

    ot_dsp_stats(dsp, sizeof(dsp));
    ot_edma_stats(edma, sizeof(edma));
    ot_ata_stats(ata, sizeof(ata));
    ot_panel_stats(panel, sizeof(panel));
    ot_timer_stats(timers, sizeof(timers));
    info_report("octatrack: frame_reads=%llu %s | %s | %s | %s | %s",
                (unsigned long long)(ot_dsp ? ot_dsp->frame_reads : 0),
                dsp, edma, ata, panel, timers);
}

void ot_dsp_install(MemoryRegion *sysmem, struct IRQState *hreq,
                     const char *audio, int throttle, uint32_t interleave,
                     int exit_with_frontend)
{
    OTDsp *s = g_new0(OTDsp, 1);

    s->hreq = (qemu_irq)hreq;
    ot_dsp = s;
    ot_dspcore_init(audio, throttle, interleave, exit_with_frontend);
    memory_region_init_io(&s->iomem, NULL, &ot_dsp_ops, s,
                          "octatrack.dsp", OT_DSP_SIZE);
    memory_region_add_subregion(sysmem, OT_DSP_PORT_BASE, &s->iomem);
    atexit(ot_dsp_report);
}
