/*
 * The audio link between the DSP shim (inside QEMU) and the frontend process:
 * two shared-memory SPSC rings, plus a Unix socket that carries one hello line
 * and is then kept open only so each side notices the other exiting.
 *
 * WHY NOT A SOCKET FOR THE DATA. The old link exchanged one 16-frame block per
 * socket round trip: the shim wrote 512 bytes, then BLOCKED in read() until
 * the frontend wrote 256 back. Two measured facts about that:
 *
 *   - It is not slower. Interleaved in one build under the same host load, the
 *     two transports are INDISTINGUISHABLE (socket 0.823x/0.766x against shm
 *     0.707x/0.897x — both orderings appear, and no load-invariant counter
 *     separates them either). Speed is not the reason.
 *   - It is FRAGILE. The blocking read made the frontend's scheduling the
 *     Octatrack's clock: if the frontend was preempted, the Octatrack
 *     stopped dead until it ran again, because nothing else could supply the
 *     DSP's input block. On a host that is always loaded, that is the whole
 *     problem.
 *
 * With the shim owning the pace (see ot-dsp-shim.cc), the ping-pong has no
 * remaining job. The ring decouples the two processes: the frontend consumes
 * whenever it runs, and an ordinary preemption costs nothing.
 *
 * SPSC, one writer and one reader per ring, so the indices need no lock: a
 * release store of the head publishes the payload written before it, and an
 * acquire load of the head orders the reader's payload reads after it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OT_AUDIO_SHM_H
#define OT_AUDIO_SHM_H

#include <stdint.h>

#define OT_SHM_FRAMES    16      /* audio frames per block                   */
#define OT_SHM_SLOTS      8      /* ESAI TX slots (DSP -> world)             */
#define OT_SHM_INS        4      /* ESAI RX slots (world -> DSP), IN A-D     */
#define OT_SHM_OUT_CAP  256      /* ring capacity in blocks (power of two)   */
#define OT_SHM_IN_CAP   256
/*
 * How far ahead of the consumer the producer will run before it waits.
 *
 * This is a BACKSTOP against a frontend that has stopped, not a throttle: the
 * shim paces itself, so steady-state occupancy is zero or one block. 64 blocks
 * is ~23 ms of Octatrack time — far beyond any scheduling hiccup, so an
 * ordinary preemption is absorbed silently and no sample is ever dropped.
 *
 * ☠ Do not treat this as a latency knob to tune down. Blocks are never
 * dropped, so the only thing a smaller value can do is re-introduce the
 * frontend into the Octatrack's critical path.
 */
#define OT_SHM_MAX_AHEAD 64
#define OT_SHM_MAGIC    0x4f544132u   /* "OTA2" */

/*
 * The Octatrack runs at REAL TIME and there is no divisor. `throttle=off` is
 * the one other state and it is a MEASUREMENT mode: capacity cannot be read
 * through a throttle, because any throttle is a cap that cannot measure above
 * itself.
 *
 * This used to be a divisor defaulting to 2, and 2 was a fossil from when
 * capacity was 0.784x and the emulator could not run real time at all. An
 * interleaved 8-per-arm sweep found real time and half speed both 8/8 clean;
 * see the note on octatrack_instance_init in ot-board.c. The canary for
 * loosening the throttle is KEY DELIVERY, not audio.
 */
#define OT_THROTTLE_DEFAULT 1

/*
 * Guest instructions per DSP slice — the ratio the DSP and the ColdFire run
 * at. The DSP is stepped from the vCPU on GUEST progress. 512 is calibrated, not
 * assumed, and measured rather than derived. It cannot
 * be interpolated — 768 measures two orders of magnitude worse than either 512
 * or 1024, because the cadence phase-locks with the guest's block sequence.
 */
#define OT_INTERLEAVE_DEFAULT 512

typedef struct {
    uint32_t magic;
    uint32_t throttled;                /* 1 = real time, 0 = flat out */
    /* Written by the producer, read by the consumer, and vice versa. Free
     * running; index with & (CAP - 1). */
    volatile uint64_t out_head, out_tail;
    volatile uint64_t in_head, in_tail;
    /* Consumer -> producer liveness, so the producer can tell "the frontend is
     * slow" from "the frontend is gone" without a socket read. */
    volatile uint32_t consumer_alive;
    uint32_t pad;
    int32_t out[OT_SHM_OUT_CAP][OT_SHM_FRAMES][OT_SHM_SLOTS];
    int32_t in[OT_SHM_IN_CAP][OT_SHM_FRAMES][OT_SHM_INS];
} OtAudioShm;

/* The one line sent over the retained Unix socket at connect:
 * "OTA2 <shm-name>\n". Nothing else is ever written to it. */
#define OT_SHM_HELLO "OTA2 "

#endif
