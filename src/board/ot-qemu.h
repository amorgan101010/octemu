/*
 * Interfaces between the Octatrack v2 board (C) and its devices, including the
 * C++ DSP shim. Plain C, no QEMU headers, so both sides can include it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OT_QEMU_H
#define OT_QEMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MemoryRegion;
struct IRQState;   /* qemu_irq, opaque */
struct Chardev;

/* The HDI24 window on FlexBus CS2, and its data port: a 16-bit write to +0x1C
 * launches one 24-bit word (bits 23-16 come from the +0x14 latch). An eDMA
 * cursor PINNED at this address is how the port behaves as a FIFO. */
#define OT_DSP_PORT_BASE 0x20000000
#define OT_DSP_PORT_DATA 0x2000001C

/* ---- board-facing --------------------------------------------------------- */

/* Map the host port window. `hreq` is the DSP's host-request line, wired by the
 * board to EPORT IRQ1 (the frame IRQ). `audio` is a unix socket path announcing
 * the shared-memory audio ring, or NULL. `throttle` holds the 44.1 kHz/16
 * block clock to REAL TIME; off runs flat out, for measurement only. */
void ot_dsp_install(struct MemoryRegion *sysmem, struct IRQState *hreq,
                     const char *audio, int throttle, uint32_t interleave,
                     int exit_with_frontend);

/* GPIO 0xFC0A400C selects which core the window addresses. */
void ot_dsp_core_select(uint32_t value);

/* Bulk eDMA data beats through the pinned port: `buf` is big-endian 16-bit
 * halfwords, the wire form of the 16-bit FlexBus port. One lock acquisition
 * per transfer — the per-word form measured ~1 ms of mutex ping-pong per
 * audio block. */
void ot_dsp_edma_write(const uint8_t *buf, uint32_t nbytes);
unsigned ot_dsp_selected_core(void);
void ot_dsp_edma_read(uint8_t *buf, uint32_t nbytes);

/* The on-chip ATA controller plus a CompactFlash card. */
void ot_ata_create(struct MemoryRegion *sysmem, uint64_t base,
                    struct IRQState *irq);
/* PPDSDR_UART bit 3 is the card-detect line: SET means NO card. */
uint8_t ot_ata_ppdsdr_uart(void);
/* Is a sector transfer in flight? See the DSP shim's straddle attribution. */
int ot_ata_busy(void);

/*
 * UART1, the panel link, with the version handshake answered HERE.
 *
 * Why in the device: the guest's polled handshake reader waits for its
 * five-byte reply in a narrow window, and bytes that miss it fall into the
 * deferred parser, which boots in a broken body-mode state and wedges FOREVER
 * — every later key frame swallowed, the dialog clock frozen, the Octatrack
 * apparently alive because the LED heartbeat keeps flowing. Answering from
 * the frontend ties that window to HOST scheduling, which is exactly what
 * cannot be relied on: measured ~1 boot in 6 wedged with six emulators booting
 * concurrently. In here the reply is on the virtual clock and host load
 * cannot reach it. Everything else on the link is still the frontend's.
 */
void ot_panel_uart_create(struct MemoryRegion *sysmem, uint64_t base,
                           struct IRQState *irq, struct Chardev *chr,
                           int mk1);

/* ---- exit report ---------------------------------------------------------- */
/* All load-invariant: counts of round trips, dispatches and lock handoffs mean
 * the same thing on a loaded host as on a quiet one, which no wall-clock
 * throughput number does. */
void ot_edma_stats(char *buf, size_t len);
void ot_dsp_stats(char *buf, size_t len);
/* Device-timer fires. pit0 is the firmware's preemptive context switch;
 * silicon puts it at 0.0363 per audio block. */
void ot_timer_stats(char *buf, size_t len);
void ot_ata_stats(char *buf, size_t len);
void ot_panel_stats(char *buf, size_t len);

/* ---- DSP host port, called from the vCPU thread --------------------------- */
void ot_dspcore_write(unsigned idx, uint32_t word24);
unsigned ot_dspcore_rx_pending(unsigned idx);
uint32_t ot_dspcore_rx_peek(unsigned idx);
uint32_t ot_dspcore_rx_pop(unsigned idx);
void ot_dspcore_icr(unsigned idx, unsigned val);
void ot_dspcore_cvr(unsigned idx, unsigned val);
void ot_dspcore_write_burst(unsigned idx, uint32_t txh,
                             const uint8_t *be, unsigned halves);
void ot_dspcore_read_burst(unsigned idx, uint8_t *be, unsigned halves);
/* The codec core's host-readable level — the HREQ/frame line. */
int ot_dspcore_hreq(void);
void ot_dspcore_init(const char *audio_path, int throttle,
                      uint32_t interleave, int exit_with_frontend);

/*
 * One interleave quantum, called from the vCPU thread at a TB boundary once
 * the guest has retired `interleave` instructions (patches/qemu/0012). The
 * DSP gets the slice of its own time that the guest just earned, so the two
 * chips' ordering is fixed by GUEST PROGRESS rather than by which host thread
 * the scheduler picked.
 */
void ot_dsp_interleave(void);
/* Deliver a held eDMA completion once the frame ISR has reset its state
 * counter. Polled on guest progress; see OT_FW_EDMA_STATE_CTR in ot-board.c
 * for why that is exact and why it has no escape. */
void ot_edma_gate_poll(void);
/* Deliver a held ATA write completion once the guest's sector counter has
 * caught up. Polled on guest progress; see OT_WR_BACKSTOP_NS in ot-ata.c. */
void ot_ata_progress(void);
uint64_t ot_dsp_blocks(void);
/* The firmware's per-block DSP transfer state counter (0x46104d3e). */
uint32_t ot_edma_state_ctr(void);
/* Retired guest instructions, and the stamp of the first arm of the block in
 * flight — the two together say whether a straddling block started late or
 * ran long. */
uint64_t ot_guest_insn(void);
uint64_t ot_edma_seq_started(void);
/* Turn the shim's delivery hold off, for measuring the straddle rate it
 * exists to suppress rather than the hold's own firing rate. */
void ot_dsp_hold_set(int on);
/* Mirror one shipped block into the guest-SDRAM audio-tap ring (no-op unless
 * -M octatrack,audio-tap=on). Called by the shim per popped block, from the
 * vCPU thread; also the block-clock SOF tick for the USB-audio payload. */
void ot_audio_tap_block(const int32_t (*out)[8]);
/* Push the DSP's host-request line to the CPU. Called by the shim at the end
 * of an interleave tick, from the vCPU thread WITHOUT the BQL — it takes the
 * BQL itself. ot_dsp_hreq_update() is the BQL-held form used from MMIO. */
void ot_dsp_hreq_sync(void);

#ifdef __cplusplus
}
#endif

#endif
