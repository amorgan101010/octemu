| SPDX-License-Identifier: MIT
| usb-audio.s — the USB-audio payload, run from SDRAM scratch (0x48001000).
|
| Copied there at runtime from the CF card by custom/coldfire/usb-audio-tramp.s, this holds
| everything too large for the image's free space: the four grown UAC1 config
| descriptors (generated into usb-audio-cfg.s by custom/usb-audio.py), the
| SET/GET_INTERFACE + usb_isr + descriptor-clamp shims, and the EP3 iso packet
| builder that sums the per-track readback blocks and streams them to the host
| at 44.1 kHz stereo 16-bit.
|
| stage2 installs every image hook AT RUNTIME (expect-guarded, D-cache pushed
| and I-cache invalidated), so an image whose card lacks /USBAUDIO.BIN is the
| stock usb-midi composite and nothing here ever runs.
|
| defsyms from usb-audio.py: USBMIDI_ISR_SHIM (chain target), CFG_LEN (grown
| config length), MIDI_CFG_{FS,HS,OS_FS,OS_HS} (the usb-midi config addresses
| the responder's pea sites currently hold — the expect() values).

| ---- image sites (all from disassembly of out/usb-midi.bin) ----
.set SETUP_ALT,      0x46c8ce0a     | SETUP wValue low = alt setting
.set SETUP_IFACE,    0x46c8ce0c     | SETUP wIndex low = interface number
.set EP0_STATUS_IN,  0x4001d524     | zero-length EP0 IN status (ACK)
.set SETIFACE_DONE,  0x4001de74     | control-request-done
.set SETIFACE_STOCK, 0x4001dd0a     | rejoin after the displaced movel
.set GETIFACE_REJOIN,0x4001d81e     | GET_INTERFACE send tail (expects pea'd ptr)
.set GETIFACE_STOCKP,0x400e20a1     | the stock 1-byte "00" the send reads
.set CLAMP1_REJOIN,  0x4001d864     | after the CONFIG length clamp
.set CLAMP2_REJOIN,  0x4001d8a2     | after the OTHER_SPEED length clamp

| ---- USB controller registers ----
.set USBSTS,     0xfc0b0144
.set USBINTR,    0xfc0b0148
.set EPPRIME,    0xfc0b01b0
.set EPCOMPLETE, 0xfc0b01bc
.set ENDPTCTRL3, 0xfc0b01cc
.set QH_EP3IN,   0x4ec949c0         | EP3 IN dQH (base 0x4ec94800 + 7*0x40)
.set EP3IN_BIT,  0x00080000         | ENDPTPRIME/STAT/COMPLETE bit for EP3 IN

| ---- the audio source: the post-FX readback arena, which EXISTS ON HARDWARE -
| ☠ Do NOT source this from the QEMU audio tap (-M octatrack,audio-tap=on):
| that ring is QEMU mirroring the DSP's ESAI TX, which no CPU on a real board
| can see, so a unit would have no producer at all. readback_buf is real: the
| eDMA deposits every track's post-FX, pre-fader block into SRAM each frame, on
| silicon as in the emulator, and it is the same memory RECEIVE sums.
|
| Layout: RB_BASE + prev*1024 + track*128 + frame*8, two 32-bit words (L,R)
| per frame, 16 frames per block, 8 tracks. Reads use the PREVIOUS bank
| (the pipeline is one block deep).
|
| ☠ It is the summed track bus, NOT the master output: pre-fader, so track
| level, the crossfader and MAIN volume are not in it. It is real audio the
| hardware produces; it is not literally what the MAIN jacks emit.
.set RB_BASE,      0x80003190
.set RB_PREV,      0x800000e4      | pingpong_prev: reads use prev
.set RB_TRACKS,    8
.set RB_SHIFT,     AUDIO_SHIFT     | 32-bit readback -> s16 (calibrated)

| The payload's own ring, in scratch above the blob. Scratch is NOT zero on
| hardware, so stage2 initializes this explicitly.
.set AUD_PRODUCED, 0x48020000       | u32 producer frame count
.set AUD_RING,     0x48020040       | 16384 frames, 4 B each (s16 LE L,R)
.set TAP_FRAMES,   16384

.set CACR_ICINVA,  0xa40ce100       | steady-state CACR + ICINVA

    .text
    .global usbaudio_stage2

| ---- stage2: install the image hooks, once, from scratch --------------------
| Table-driven: verify EVERY site matches its expected stock bytes before
| touching any, then apply all, D-cache-push each patched line, and invalidate
| the I-cache. A single mismatch aborts the whole install. Runs in supervisor
| context (the fs poll task).
usbaudio_stage2:
    lea     %sp@(-48),%sp
    moveml  %d0-%d7/%a0-%a3,%sp@
    | ☠ Scratch holds whatever the SDRAM powered up with — it is NOT zero on
    | hardware, and QEMU's zeroed RAM hides that (--hw-faithful poisons it).
    | The producer's cursor must start from a known value, not from whatever
    | was in those four bytes.
    clrl    AUD_PRODUCED
    | ---- verify pass ----
    lea     usbaudio_patches,%a0
1:  movel   %a0@,%d0                | target va
    beqs    3f
    moveal  %d0,%a1
    moveal  %a0@(4),%a2             | expect ptr
    movel   %a0@(12),%d1           | len
2:  mvzb    %a1@+,%d2               | zero-extend: moveb would leave stale
    mvzb    %a2@+,%d3               | upper bytes and cmpl would mis-compare
    cmpl    %d2,%d3
    bnes    8f                      | mismatch -> abort, install nothing
    subql   #1,%d1
    bnes    2b
    lea     %a0@(16),%a0
    bras    1b
    | ---- apply pass ----
3:  lea     usbaudio_patches,%a0
4:  movel   %a0@,%d0
    beqs    6f
    moveal  %d0,%a1                 | dest
    moveal  %a0@(8),%a2             | patch ptr
    movel   %a0@(12),%d1           | len
5:  moveb   %a2@+,%a1@+
    subql   #1,%d1
    bnes    5b
    | push the patched line(s) out of the copyback D-cache so the I-fetch sees
    | them (the image region is data-cacheable; QEMU no-ops this).
    moveal  %a0@,%a3
    cpushl  %bc,%a3@
    movel   %a0@(12),%d1
    addal   %d1,%a3
    subql   #1,%a3
    cpushl  %bc,%a3@
    lea     %a0@(16),%a0
    bras    4b
6:  movel   #CACR_ICINVA,%d0        | invalidate the whole I + branch cache
    movec   %d0,%cacr
    nop
8:  moveml  %sp@,%d0-%d7/%a0-%a3
    lea     %sp@(48),%sp
    rts

| ---- SET_INTERFACE shim (installed at 0x4001dd04) ---------------------------
| Displaced: movel 0xfc0b01c4,%d0. Interface 3 (AudioStreaming) brings the EP3
| iso IN up on alt 1 and tears it down on alt 0; any other interface falls
| through to the stock handler unchanged.
    .global audio_setiface_shim
audio_setiface_shim:
    mvzb    SETUP_IFACE,%d0
    moveq   #3,%d1
    cmpl    %d0,%d1
    bne     5f                      | not iface 3 -> stock
    mvzb    SETUP_ALT,%d0
    moveb   %d0,usbaudio_alt
    tstb    %d0
    beq     2f                      | alt 0 -> tear down
    movel   #0x60b40000,%d0         | dQH cap: Mult 1, ZLT off, maxpkt 180
    movel   %d0,QH_EP3IN
    clrl    QH_EP3IN+4
    moveq   #1,%d0
    movel   %d0,QH_EP3IN+8          | no dTD primed yet (terminate)
    movel   #0x00840000,%d0         | ENDPTCTRL3 TXE + iso (cosmetic; bench ignores)
    movel   %d0,ENDPTCTRL3
    movel   AUD_PRODUCED,%d0
    movel   %d0,usbaudio_consumed   | start streaming from "now"
    clrl    usbaudio_acc
    clrb    usbaudio_busy
    bra     4f
2:  clrl    ENDPTCTRL3
    clrb    usbaudio_busy
4:  jsr     EP0_STATUS_IN
    jmp     SETIFACE_DONE
5:  movel   0xfc0b01c4,%d0          | displaced
    jmp     SETIFACE_STOCK

| ---- GET_INTERFACE shim (installed at 0x4001d824) ---------------------------
| Displaced: pea 0x400e20a1 (the stock 1-byte "00"). Interface 3 reports the
| live alt setting; every other interface keeps the stock answer.
    .global audio_getiface_shim
audio_getiface_shim:
    mvzb    SETUP_IFACE,%d0
    moveq   #3,%d1
    cmpl    %d0,%d1
    bnes    1f
    pea     usbaudio_alt
    jmp     GETIFACE_REJOIN
1:  pea     GETIFACE_STOCKP
    jmp     GETIFACE_REJOIN

| ---- usb_isr shim (installed at 0x4001e606, was jmp usbmidi_isr_shim) -------
| Retires EP3 IN completions (clears the busy flag, so the next block can
| prime), then chains to the usb-midi ISR shim which handles EP2 and runs the
| original displaced instruction.
    .global audio_isr_shim
audio_isr_shim:
    lea     %sp@(-40),%sp
    moveml  %d0-%d7/%a0-%a1,%sp@
    | ☠ Exactly ONE place primes EP3: the per-block producer in frame_isr.
    | This shim only retires the completion. Priming from both would race on
    | usbaudio_busy (test-then-set, and frame_isr can preempt usb_isr), and
    | the block clock at ~2756/s already outruns the 1 kHz packet rate, so
    | there is nothing to gain by kicking from here. The SOF is not used.
    movel   EPCOMPLETE,%d0
    movel   #EP3IN_BIT,%d1
    andl    %d1,%d0
    beqs    2f
    movel   #EP3IN_BIT,%d1
    movel   %d1,EPCOMPLETE          | W1C EP3 IN
    clrb    usbaudio_busy
2:  moveml  %sp@,%d0-%d7/%a0-%a1
    lea     %sp@(40),%sp
    jmp     USBMIDI_ISR_SHIM

| ---- the iso packet builder -------------------------------------------------
| One packet in flight (busy flag). Sends the next 44/45-frame packet only when
| the tap ring holds that many un-sent frames, so the emitted stream is an
| exact, gap-free substring of what --recording captures. Underrun sends
| nothing (the host's IN blocks); a producer that laps the ring (host stopped
| draining) resyncs and counts an overrun. Caller saved d0-d7/a0-a1.
usbaudio_kick:
    tstb    usbaudio_busy
    bne     9f
    movel   AUD_PRODUCED,%d0
    movel   usbaudio_consumed,%d1
    movel   %d0,%d2
    subl    %d1,%d2                 | d2 = frames available
    cmpil   #TAP_FRAMES,%d2
    blss    3f                      | within the ring: fine
    addql   #1,usbaudio_overruns
    movel   %d0,%d1
    subil   #TAP_FRAMES,%d1
    movel   %d1,usbaudio_consumed   | resync to the ring's trailing edge
    movel   #TAP_FRAMES,%d2
3:  movel   usbaudio_acc,%d3
    addil   #441,%d3                | 44.1 frames/ms: 9x44 + 45 per 10 packets
    moveq   #10,%d7
    movel   %d3,%d4
    divu.l  %d7,%d4                 | d4 = n = (acc+441)/10
    movel   %d4,%d5
    mulu.l  %d7,%d5
    movel   %d3,%d6
    subl    %d5,%d6                 | d6 = new acc = (acc+441) - n*10
    cmpl    %d4,%d2
    bcs     9f                      | available < n: underrun, send nothing
    movel   %d6,usbaudio_acc
    | copy n frames, wrap-safe, into the packet buffer
    movel   %d4,%d3
    movel   usbaudio_consumed,%d5
    lea     usbaudio_pktbuf,%a1
4:  movel   %d5,%d1
    andil   #TAP_FRAMES-1,%d1
    lsll    #2,%d1
    movel   #AUD_RING,%a0
    addal   %d1,%a0
    movel   %a0@,%a1@
    addql   #4,%a1
    addql   #1,%d5
    subql   #1,%d3
    bnes    4b
    movel   %d5,usbaudio_consumed
    | build the dTD and prime EP3 IN
    movel   %d4,%d6
    lsll    #2,%d6                  | nbytes = n * 4
    lea     usbaudio_dtd,%a0
    moveq   #1,%d1
    movel   %d1,%a0@                | next dTD = terminate
    movel   %d6,%d1
    swap    %d1                     | nbytes << 16
    oril    #0x8080,%d1             | IOC + ACTIVE
    movel   %d1,%a0@(4)             | token
    lea     usbaudio_pktbuf,%a1
    movel   %a1,%a0@(8)             | buffer page 0
    movel   %a0,QH_EP3IN+8          | dQH next-dTD (the bench reads this on prime)
    moveq   #1,%d1
    moveb   %d1,usbaudio_busy
    movel   #EP3IN_BIT,%d1
    movel   %d1,EPPRIME
9:  rts

| ---- the per-block producer (installed at 0x4000d9a0, inside frame_isr) ----
| frame_isr runs once per 16-frame block on real hardware. This is the whole
| reason the feature can work on a unit: the block clock, the audio and the
| trigger to send are all firmware events, with nothing supplied by the
| emulator.
|
| ☠ The hook site is the LAST instruction before frame_isr's
| `moveml %sp@,%d0-%fp` epilogue, so every register is about to be reloaded
| from the stack — this shim may clobber d0-a6 freely. It must not touch %sp.
|
| Displaced: clrl 0x46104d4e (6 bytes), rejoin 0x4000d9a6.
    .global audio_frame_shim
audio_frame_shim:
    tstb    usbaudio_alt            | host has not opened the stream: cost 0
    beq     9f
    movel   RB_PREV,%d0
    lsll    #8,%d0
    lsll    #2,%d0                  | prev * 1024 (imm shift is 1-8)
    addil   #RB_BASE,%d0
    moveal  %d0,%a2                 | a2 = this bank's track 0
    movel   AUD_PRODUCED,%d4
    movel   %d4,%d5
    andil   #TAP_FRAMES-1,%d5
    lsll    #2,%d5
    movel   #AUD_RING,%a3
    addal   %d5,%a3                 | a3 = write cursor (block never wraps:
                                    | 16 divides the ring size)
    moveq   #RB_SHIFT,%d1           | asr.l immediate is 1-8 only: shift via d1
    moveq   #15,%d6                 | 16 frames
1:  moveq   #0,%d2                  | L accumulator
    moveq   #0,%d3                  | R accumulator
    moveal  %a2,%a0
    moveq   #RB_TRACKS-1,%d7
2:  movel   %a0@,%d0                | track L
    asrl    %d1,%d0
    addl    %d0,%d2
    movel   %a0@(4),%d0             | track R
    asrl    %d1,%d0
    addl    %d0,%d3
    lea     %a0@(128),%a0           | next track, same frame
    subql   #1,%d7
    bpls    2b
    | saturate to s16 and store little-endian. ColdFire has no rotate, so the
    | byte order is built by hand rather than by swapping.
    bsr     audio_sat16
    moveb   %d2,%a3@                | L low byte
    lsrl    #8,%d2
    moveb   %d2,%a3@(1)             | L high byte
    movel   %d3,%d2
    bsr     audio_sat16
    moveb   %d2,%a3@(2)             | R low byte
    lsrl    #8,%d2
    moveb   %d2,%a3@(3)             | R high byte
    lea     %a3@(4),%a3
    lea     %a2@(8),%a2             | next frame
    subql   #1,%d6
    bpls    1b
    addql   #8,%d4
    addql   #8,%d4                  | 16 frames produced
    movel   %d4,AUD_PRODUCED
    | the block clock IS the send clock: prime the next packet now. This is the
    | only place that primes -- usb_isr only retires the completion.
    bsr     usbaudio_kick
9:  clrl    0x46104d4e              | displaced
    jmp     0x4000d9a6

| d2 -> saturated int16 in the low word of d2. Clobbers d0.
audio_sat16:
    movel   #32767,%d0
    cmpl    %d0,%d2
    bles    1f
    movel   %d0,%d2
    rts
1:  movel   #-32768,%d0
    cmpl    %d0,%d2
    bges    2f
    movel   %d0,%d2
2:  rts

| ---- descriptor length-clamp shims -----------------------------------------
| The stock responder clamps GET_DESCRIPTOR(CONFIG / OTHER_SPEED) replies to a
| hardcoded length via a moveq that cannot hold the grown config; each shim
| computes min(wLength, CFG_LEN) and rejoins. d2 = wLength (host order).
    .global audio_clamp1_shim, audio_clamp2_shim
audio_clamp1_shim:
    movel   %d2,%d1
    cmpil   #CFG_LEN,%d1
    blss    1f
    movel   #CFG_LEN,%d1
1:  jmp     CLAMP1_REJOIN
audio_clamp2_shim:
    movel   %d2,%d1
    cmpil   #CFG_LEN,%d1
    blss    1f
    movel   #CFG_LEN,%d1
1:  jmp     CLAMP2_REJOIN

| ---- runtime patch table ---------------------------------------------------
| Each record: target va, expected-bytes ptr, patch-bytes ptr, length.
    .data
    .balign 4
usbaudio_patches:
    .long 0x4001e606, exp_isr,      pat_isr,      6
    .long 0x4001dd04, exp_setiface, pat_setiface, 6
    .long 0x4001d824, exp_getiface, pat_getiface, 6
    .long 0x4001d882, exp_cfg_fs,   pat_cfg_fs,   4
    .long 0x4001d88a, exp_cfg_hs,   pat_cfg_hs,   4
    .long 0x4001d8c0, exp_cfg_os_hs,pat_cfg_os_hs,4
    .long 0x4001d8c8, exp_cfg_os_fs,pat_cfg_os_fs,4
    .long 0x4001d858, exp_clamp,    pat_clamp1,   6
    .long 0x4001d896, exp_clamp,    pat_clamp2,   6
    .long 0x4000d9a0, exp_frame,    pat_frame,    6
    .long 0

exp_isr:       .byte 0x4e,0xf9
               .long USBMIDI_ISR_SHIM
pat_isr:       .byte 0x4e,0xf9
               .long audio_isr_shim
exp_setiface:  .byte 0x20,0x39,0xfc,0x0b,0x01,0xc4
pat_setiface:  .byte 0x4e,0xf9
               .long audio_setiface_shim
exp_getiface:  .byte 0x48,0x79,0x40,0x0e,0x20,0xa1
pat_getiface:  .byte 0x4e,0xf9
               .long audio_getiface_shim
exp_clamp:     .byte 0x70,0x7c,0xb0,0x82,0x65,0x04
pat_clamp1:    .byte 0x4e,0xf9
               .long audio_clamp1_shim
pat_clamp2:    .byte 0x4e,0xf9
               .long audio_clamp2_shim
exp_cfg_fs:    .long MIDI_CFG_FS
pat_cfg_fs:    .long cfg_fs
exp_cfg_hs:    .long MIDI_CFG_HS
pat_cfg_hs:    .long cfg_hs
exp_cfg_os_hs: .long MIDI_CFG_OS_HS
pat_cfg_os_hs: .long cfg_os_hs
exp_cfg_os_fs: .long MIDI_CFG_OS_FS
pat_cfg_os_fs: .long cfg_os_fs
exp_frame:     .byte 0x42,0xb9,0x46,0x10,0x4d,0x4e   | clrl 0x46104d4e
pat_frame:     .byte 0x4e,0xf9
               .long audio_frame_shim

| ---- payload state ----------------------------------------------------------
    .balign 4
    .global usbaudio_consumed, usbaudio_acc, usbaudio_overruns
usbaudio_consumed: .long 0          | frames pulled from the tap ring
usbaudio_acc:      .long 0          | 44.1-frame accumulator (x10)
usbaudio_overruns: .long 0          | host-drain-stall resyncs (counted, not silent)
usbaudio_alt:      .byte 0          | current AudioStreaming alt setting
usbaudio_busy:     .byte 0          | an EP3 IN transfer is in flight
    .balign 4
usbaudio_dtd:      .space 32        | the single EP3 IN dTD (0x20-aligned area)
usbaudio_pktbuf:   .space 192       | one iso packet (<= 180 B)

    .balign 4
    .include "usb-audio-cfg.s"
