| SPDX-License-Identifier: MIT
| RECEIVE amp -- give the RECEIVE machine the stock DSP amp envelope.
|
| Lives in the 338 free bytes at 0x400c45b0.  Two entry points:
|
|   hook  -- jsr'd from the record builder, replacing the id-3 word-30
|            marker block at 0x40004d4a..0x40004d65.  Runs once per block
|            per track, with the builder's registers live:
|              a1 = &record word 30 (the half being built)
|              a2 = record base for this track
|              d4 = track index 0..7
|              d5 = &published_machine_ids[sel*8 + track]
|            d0/d1/d2/a0 are free (the replaced code used them; the code
|            after 0x40004d66 reloads everything it needs).
|
|   prep  -- installed at prep vtable B[3] (0x400d6460).  The dispatcher
|            calls it with (track, byte, flags) when bit 4 of the track's
|            event byte at 0x46104d0c is set, and ORs the RETURN VALUE
|            into record word 30.  Here it only marks a trig pending for
|            the hook and returns 0.
|
| WHY:
|
|  - Word-30 bit 9, the builder's "machine id is 3" marker, is delivered
|    <<8 to the DSP as bit 17, and core 1's amp-env render skips the whole
|    envelope + VOL/BAL/XVOL stage when it is set (P:0x38c brset #17).
|    With the marker gone the voice behaves like a sample voice: NOT MIXED
|    at all while word 30 sits at 0, opened by a note-on strobe.
|  - The staging already publishes the track's REAL AMP page into record
|    words 0-5 for this machine -- page-1 bytes 24-35, the AMP knobs, land
|    there knob<<8.  The LV params never reach the DSP record; they live in
|    the 0x80000510 staging records only, which is what the audio handler
|    reads.  So the knobs need no help -- the ONLY thing missing is the
|    lifecycle.
|  - The DSP acts on one-record word-30 strobes: 0x01d0 note-on opens the
|    attack, 0x0060 starts the word-2 release, 0x0040 sustains.  A bare
|    0x0020 is ignored.  The hook drives these from a tiny per-track state
|    machine: a pending trig emits the note-on and arms a CPU-side HOLD
|    countdown (the DSP never reads word 1 -- HOLD is the CPU's job); its
|    expiry emits the gate-off and the DSP releases at the REL knob's rate.
|    REL 127 = INF never decays, so ONE TRIG with REL=INF is exactly
|    NEIGHBOR's forever-drone.
|  - At rest (never trigged) word 30 stays 0 and the voice is silent for
|    free -- the DSP does not mix a voice that has never noted on.
|
| HOLD mapping: blocks = knob<<4 (+1), 127 = INF.  One block = 16 frames
| at 44.1 kHz = 0.36 ms, so HOLD 64 sustains ~0.37 s.

STATE = 0x400d24d0                | 8 tracks x 4: phase, pend, holdctr.w.
                                  | 32 bytes at the head of the 2060-byte free
                                  | zone, not in the 338-byte code zone, which
                                  | they would leave with 2 bytes of slack.
                                  | Zero at boot, so phase starts idle.
                                  | phase: 0 idle, 1 gate, 2 released

        .text
        .globl  hook
hook:
        movea.l %d5,%a0
        mvs.b   %a0@,%d0
        moveq   #3,%d2
        cmp.l   %d0,%d2
        beqs    1f
        move.w  %a1@,%d0          | other machines: the replaced bclr #9
        bclr    #9,%d0
        move.w  %d0,%a1@
        rts

1:      moveq   #0x20,%d0         | record word 29, the 4th gain-chain stage,
        lsl.l   #8,%d0            | 0x0800 stock: x4 here restores NEIGHBOR
        move.w  %d0,%a2@(58)      | parity — once the marker no longer skips it,
                                  | the VOL/BAL/XVOL chain runs x0.25 (-12 dB)
                                  | at default knobs, where the skipped chain
                                  | was unity
        move.l  %d4,%d0           | a0 = page-1 param array for this track
        lsl.l   #6,%d0
        add.l   #0x80000a50,%d0
        movea.l %d0,%a0
        mvz.b   %a0@(26),%d2      | d2 = HOLD knob, before a0 is reused
        move.l  %d4,%d0
        lsl.l   #2,%d0
        add.l   #STATE,%d0
        movea.l %d0,%a0           | a0 = this track's state

        tst.b   %a0@(1)           | trig pending?
        beqs    2f
        clr.b   %a0@(1)
        move.b  #1,%a0@           | phase = gate
        moveq   #127,%d0          | holdctr = INF or knob<<4 + 1
        cmp.l   %d0,%d2
        bnes    11f
        move.l  #0xffff,%d2
        bras    12f
11:     lsl.l   #4,%d2
        addq.l  #1,%d2
12:     move.w  %d2,%a0@(2)
        move.w  #0x01d0,%a1@      | NOTE-ON
        rts

2:      mvz.b   %a0@,%d0          | phase
        tst.l   %d0
        bnes    3f
        clr.w   %a1@              | idle: word 30 = 0 — the DSP does not mix
        rts                       | a voice that has never noted on

3:      moveq   #2,%d1
        cmp.l   %d1,%d0
        beqs    5f                | released
        mvz.w   %a0@(2),%d0       | gate: run the hold countdown
        move.l  #0xffff,%d1
        cmp.l   %d1,%d0
        beqs    4f                | INF hold
        subq.l  #1,%d0
        move.w  %d0,%a0@(2)
        tst.l   %d0
        bnes    4f
        move.b  #2,%a0@           | hold expired: phase = released
        move.w  #0x0060,%a1@      | GATE-OFF -- the DSP starts the release
        rts
4:      move.w  #0x0040,%a1@      | sustaining
        rts
5:      move.w  #0x0040,%a1@      | releasing (or done) at the REL rate
        rts

        .globl  prep
prep:                             | (track, byte, flags) -> word-30 OR value
        move.l  %sp@(4),%d0
        lsl.l   #2,%d0
        add.l   #STATE,%d0
        movea.l %d0,%a0
        move.b  #1,%a0@(1)        | pend = 1; the hook emits the strobe
        clr.l   %d0
        rts
