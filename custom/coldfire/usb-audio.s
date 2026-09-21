| SPDX-License-Identifier: MIT
| usb-audio.s — the USB-audio payload, run from SDRAM scratch (0x48001000).
|
| Copied there at runtime from the CF card by custom/coldfire/usb-audio-tramp.s, this holds
| everything too large for the image's free space: the four grown UAC1 config
| descriptors (generated into usb-audio-cfg.s by custom/usb-audio.py), the
| SET/GET_INTERFACE + usb_isr + descriptor-clamp shims, and the EP3 iso packet
| builder that streams the emulated MAIN output (mirrored into guest SDRAM by
| -M octatrack,audio-tap=on) to the host at 44.1 kHz stereo 16-bit.
|
| stage2 installs every image hook AT RUNTIME (expect-guarded, D-cache pushed
| and I-cache invalidated), so an image whose card lacks /USBAUDIO.BIN is the
| stock usb-midi composite and nothing here ever runs.
|
| defsyms from usb-audio.py: USBMIDI_ISR_SHIM (chain target), CFG_LEN (grown
| config length), MIDI_CFG_{FS,HS,OS_FS,OS_HS} (the usb-midi config addresses
| the responder's pea sites currently hold — the expect() values).

| ---- image sites (all from disassembly of out/usb-midi.bin) ----
.ifndef AUD_CUSHION_OVR
.set AUD_CUSHION_OVR, 1
.endif
.ifndef AUD_SERVO
.set AUD_SERVO, 1
.endif
.ifndef GUARD_SHIM
.set GUARD_SHIM, 0
.endif
.ifndef EXPECT_BASE
.set EXPECT_BASE, 0
.endif
.ifndef EXPECT_STAGE2
.set EXPECT_STAGE2, 0
.endif
.ifndef GUARD_FRAME
.set GUARD_FRAME, 0
.endif
.ifndef HEAP_RESERVE
.set HEAP_RESERVE, 0
.endif
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
.set EPLISTADDR, 0xfc0b0158         | the controller's OWN dQH list base
.set ENDPTSTAT,  0xfc0b01b8
.set DCCPARAMS,  0xfc0b0124         | DEN[4:0] = endpoint pairs this core has
.set USBSTS,     0xfc0b0144
.set USBINTR,    0xfc0b0148
.set EPPRIME,    0xfc0b01b0
.set EPFLUSH,    0xfc0b01b4
.set EPCOMPLETE, 0xfc0b01bc
.set ENDPTCTRL3, 0xfc0b01cc
.set QH_EP3IN,   0x4ec949c0         | EP3 IN dQH as OBSERVED IN THE EMULATOR.
                                    | ☠ Only ever a fallback now. The real base
                                    | is read from ENDPTLISTADDR at run time:
                                    | hardcoding it assumed the firmware puts
                                    | its endpoint list where QEMU showed it,
                                    | and if silicon differs the controller
                                    | never sees our queue head at all -- which
                                    | presents as an endpoint that enumerates
                                    | and then transmits nothing, forever.
.set QH_EP3IN_OFF, 7*64             | EP3 IN is list entry (3*2)+1
.set EP3IN_BIT,  0x00080000         | ENDPTPRIME/STAT/COMPLETE bit for EP3 IN

| ---- the audio source: the post-FX readback arena, which EXISTS ON HARDWARE -
| ☠ This used to read a ring that only `-M octatrack,audio-tap=on` ever wrote
| (QEMU mirroring the DSP's ESAI TX, which no CPU on a real board can see). A
| unit running that build had no producer at all. The source here is
| readback_buf: the eDMA deposits every track's post-FX, pre-fader block into
| SRAM each frame, on silicon as in the emulator, and it is the same memory
| the RECEIVE machine sums — confirmed working on real hardware.
|
| Layout: RB_BASE + prev*1024 + track*128 + frame*8, two 32-bit words (L,R)
| per frame, 16 frames per block, 8 tracks. Reads use the PREVIOUS bank
| (the pipeline is one block deep).
|
| ☠ It is the summed track bus, NOT the master output: pre-fader, so track
| level, the crossfader and MAIN volume are not in it. It is real audio the
| hardware produces; it is not literally what the MAIN jacks emit.
.set FLEX_FREE_BASE, 0x8000691c     | live free-stack base cursor (index)
.set RB_BASE,      0x80003190
.set RB_PREV,      0x800000e4      | pingpong_prev: reads use prev
.set RB_TRACKS,    8
.set RB_SHIFT,     AUDIO_SHIFT     | 32-bit readback -> s16 (calibrated)

| The payload's own audio ring. ☠ It lives INSIDE the blob now, as reserved
| space, so it moves with the code when the loader relocates: there is no
| second address to verify, and it cannot collide with the sample heap because
| the heap itself handed us the memory. Shrunk from 16384 to 4096 frames — 93
| ms of buffer, ample between a ~2756/s producer and a 1 kHz consumer — which
| drops the whole footprint from 14 pages to 6 and so makes a contiguous
| allocation far likelier.
.set AUD_FRAMES,   4096
.set AUD_CUSHION,  2048         | ~46 ms buffered before the first packet. Big
                                    | enough that a momentary producer stall is
                                    | absorbed rather than becoming a dropout;
                                    | this stream is for recording, and latency
                                    | here costs nothing (Mark monitors from the
                                    | Octatrack's own outputs).
.set AUD_TARGET,   2048         | fill the servo steers towards
.set AUD_BAND,     512          | deadband, so it does not hunt

.set CACR_ICINVA,  0xa40ce100       | steady-state CACR + ICINVA (FREE-SPACE.md)
| The firmware's own device attach/detach — usb_attach(1) attaches,
| usb_attach(0) detaches; USB DISK MODE uses it on entry and exit.
.set USB_ATTACH,   0x4001eb44

    .text
| ---- self-relocating entry (POSITION INDEPENDENT) ------------------------
| ☠ The loader no longer picks a fixed address: it asks the firmware's page
| allocator for memory, so the payload cannot know at build time where it will
| run. This prologue is the only part that must work at any address, so it
| touches memory ONLY through a base register derived from its own PC. It then
| fixes up the self-references listed in the header and calls the real stage2,
| which may go back to plain absolute addressing.
|
| ☠ The heap is CACHEABLE COPYBACK (ACR0 covers 0x40000000-0x47ffffff), unlike
| the old cache-inhibited scratch. Relocation writes CODE, so the dirty data
| lines must be pushed and the instruction cache invalidated before any of it
| is executed, or the CPU fetches the pre-relocation bytes.
|
| Header (at base): +0 magic, +4 length, +8 checksum, +12 entry offset,
| +16 reloc table offset, +20 reloc count, +24 link base.
    .global usbaudio_entry
usbaudio_entry:
    lea     %sp@(-32),%sp
    moveml  %d0-%d4/%a0-%a2,%sp@
    lea     %pc@(usbaudio_entry),%a0
    lea     %a0@(-28),%a0           | a0 = load base (entry sits right after the header)
    movel   %a0@(16),%d0
    lea     %a0@(0,%d0:l),%a1       | a1 = relocation table
    movel   %a0@(20),%d1            | d1 = entry count
    movel   %a0,%d2
    addil   #28,%d2                 | runtime address of the code
    subl    %a0@(24),%d2            | minus the link base = fixup delta
    tstl    %d1
    beqs    2f
1:  movel   %a1@+,%d3               | offset of one self-reference
    lea     %a0@(0,%d3:l),%a2
    movel   %a2@,%d4
    addl    %d2,%d4
    movel   %d4,%a2@
    subql   #1,%d1
    bnes    1b
2:  movel   %a0@(4),%d1             | push the whole blob out of the D-cache
    lsrl    #4,%d1                  | one cpushl per 16-byte line
    addql   #1,%d1
    moveal  %a0,%a2
3:  cpushl  %bc,%a2@
    lea     %a2@(16),%a2
    subql   #1,%d1
    bnes    3b
    movel   #0xa40ce100,%d0         | steady-state CACR + ICINVA
    movec   %d0,%cacr
    nop
    moveml  %sp@,%d0-%d4/%a0-%a2
    lea     %sp@(32),%sp
    bra     usbaudio_stage2         | PC-relative: valid before and after

    .global usbaudio_stage2

| ---- stage2: install the image hooks, once, from scratch --------------------
| Table-driven: verify EVERY site matches its expected stock bytes before
| touching any, then apply all, D-cache-push each patched line, and invalidate
| the I-cache. A single mismatch aborts the whole install (the image stays the
| stock usb-midi composite). Runs in supervisor context (the fs poll task).
usbaudio_stage2:
    lea     %sp@(-48),%sp
    moveml  %d0-%d7/%a0-%a3,%sp@
    | ☠ Scratch holds whatever the SDRAM powered up with — it is NOT zero on
    | hardware, and QEMU's zeroed RAM hides that (--hw-faithful poisons it).
    | The producer's cursor must start from a known value, not from whatever
    | was in those four bytes.
    clrl    aud_produced
    | Same reason: clear EP3's queue head before anything can enumerate.
    bsr     audio_qh_resolve        | %a0 = EP3 dQH, from the controller
    moveq   #15,%d1
0:  clrl    %a0@+
    subql   #1,%d1
    bpls    0b
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
    bne     .Labort                 | mismatch -> abort, install nothing
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
    bra     4b                      | not bras: the diagnostic prefill pushed this out of range
6:  movel   #CACR_ICINVA,%d0        | invalidate the whole I + branch cache
    movec   %d0,%cacr
    nop
    | ☠ DO NOT RE-ATTACH USB FROM HERE. An earlier version called
    | usb_attach(0)/usb_attach(1) so a host that had already cached the stock
    | descriptors would re-read them. It CORRUPTED A REAL CF CARD.
    |
    | usb_attach is USB DISK MODE's routine, and DISK MODE's contract is that
    | the guest UNMOUNTS ITS FILESYSTEM FIRST — usb_diskmode_enter unmounts the
    | card and only then attaches. Calling it from here attaches the mass
    | storage device to the host while the machine still has the card mounted
    | and live, so two writers share one medium: on hardware whole directories
    | (PRESETS, the Sets) came back as zero-byte entries. It also wedges the
    | machine before the UI in the emulator, which is how the gate caught it.
    |
    | A host holding stale descriptors is a nuisance; replugging the cable
    | fixes it. Corrupting the user's card is not a trade worth making. If
    | this is ever revisited it must go through the firmware's own unmount
    | path, not around it.
    .if GUARD_SHIM
    | Tell the image-resident guard where the producer actually is. Must happen
    | BEFORE the hook is installed, or the first block interrupt could arrive
    | with the guard still holding zero.
    lea     %pc@(audio_frame_shim),%a0
    movel   %a0,GUARD_SHIM
    .endif
    .if EXPECT_BASE
    | ---- am I actually loaded where the build assumed? ---------------------
    | ☠ A payload built for a fixed base but loaded by an ALLOCATOR trampoline
    | runs fine, reserves the wrong pages, and is then wiped by the next
    | project load -- after which the hooks jump into its remains. That is the
    | VEC:04 illegal instruction at IPL 5 seen four times, and the crash PC was
    | always the frame shim's offset from the ALLOCATOR's base, not from the
    | base this build was compiled for. Refuse to install instead.
    | usbaudio_entry sits exactly at the header boundary (entry +0x1c ==
    | PAYLOAD_HDR), so its runtime address is the load base + 0x1c with no
    | dependency on any other payload symbol. stage2 is NOT at the body start
    | (it links at +0x84), and comparing against that is what made this check
    | abort every install while appearing to work.
    lea     %pc@(usbaudio_entry),%a0
    movel   #EXPECT_STAGE2,%d0
    cmpl    %a0,%d0
    beqs    .Lbase_ok
    moveq   #10,%d0                 | wrong load address for this build
    bra     .Lreport
.Lbase_ok:
    .endif
    .if HEAP_RESERVE
    | ---- take the reserved pages out of the LIVE free stack ----------------
    | ☠ Patching flex_heap_init's constants only changes FUTURE re-seeds. The
    | stack was already seeded at boot with pages 1..14602 before this payload
    | loaded, and FLEX sample loads consume it from the BASE -- pages 1, 2, 3
    | ... which is exactly where the payload sits. On hardware, changing
    | projects loaded samples straight over our code and the frame ISR then
    | executed sample data: VEC:04 illegal instruction at IPL 5, PC inside the
    | heap. Advancing the base cursor removes those pages from the live stack
    | immediately, so nothing can be handed them before the next re-seed.
    |
    | ☠ GUARDED: if the cursor has already moved, something has ALREADY been
    | allocated from the bottom of the heap and the payload may be sitting on
    | live sample data. Abort the install rather than corrupt a sample -- no
    | hooks are installed at this point, so aborting leaves a stock machine.
    movel   FLEX_FREE_BASE,%d0
    bnes    .Lheap_busy
    moveq   #HEAP_RESERVE,%d0
    movel   %d0,FLEX_FREE_BASE
    .endif
    .if HEAP_RESERVE
    bra     .Lheap_ok
.Lheap_busy:
    moveq   #9,%d0                  | "USBAUD E9" — heap bottom already in use
    bra     .Lreport
.Lheap_ok:
    .endif
    moveq   #0,%d0                  | "USBAUD E0" — hooks really installed
    bra     .Lreport
.Labort:
    moveq   #7,%d0                  | "USBAUD E7" — a patch site did not match
.Lreport:
    movel   #UA_REPORT,%a0
    jsr     %a0@
.Lstage2_done:
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
    moveq   #4,%d1                  | AudioStreaming moved 3 -> 4 (two functions)
    cmpl    %d0,%d1
    bne     5f                      | not iface 3 -> stock
    | ☠ usbaudio_alt is what gates the per-block producer, so it must be set
    | LAST on the way up: a block interrupt landing between the flag and the
    | cursor init would run the producer against stale consumed/acc state.
    | On the way DOWN it is cleared FIRST, for the same reason.
    mvzb    SETUP_ALT,%d0
    tstb    %d0
    beq     2f                      | alt 0 -> tear down
    | ☠ ZERO THE WHOLE 64-BYTE dQH FIRST. EP3's queue head sits past anything
    | the stock firmware ever initializes, so on hardware its TOKEN (+0x0C)
    | and buffer pointers (+0x10..+0x1C) hold power-on garbage. The device
    | controller is a real bus master on silicon: it acts on that token's
    | ACTIVE bit and those pointers, then writes transfer status back through
    | them — an arbitrary memory write, which lands wherever the garbage
    | points (image code included). The firmware's own EP0 setup at
    | 0x4001d656 clears the token for exactly this reason; clearing the whole
    | structure is the safe superset. QEMU's packet bench never showed this
    | because it does not fetch the dQH out of guest memory at all.
    bsr     audio_qh_resolve        | %a0 = EP3 dQH, from the controller
    moveq   #15,%d1
1:  clrl    %a0@+
    subql   #1,%d1
    bpls    1b
    moveal  qh_ep3,%a0              | resolved just above
    movel   #0x60b40000,%d0         | dQH cap: Mult 1, ZLT off, maxpkt 180
    movel   %d0,%a0@
    clrl    %a0@(4)                 | current dTD
    moveq   #1,%d0
    movel   %d0,%a0@(8)             | no dTD primed yet (terminate)
    clrl    %a0@(12)                | TOKEN — the field the firmware clears
    movel   #0x00840000,%d0         | ENDPTCTRL3 TXE + iso (cosmetic; bench ignores)
    movel   %d0,ENDPTCTRL3
    movel   aud_produced,%d0
    subil   #AUD_TARGET,%d0         | start a full cushion BEHIND the producer:
    bccs    .Lcons_ok               | the ring is already full, so there is no
    moveq   #0,%d0                  | priming gap and no startup underruns
.Lcons_ok:
    movel   %d0,usbaudio_consumed
    clrl    usbaudio_acc
    clrb    usbaudio_busy
    moveq   #1,%d0
    moveb   %d0,usbaudio_primed     | already full: no priming phase
    moveq   #1,%d0
    moveb   %d0,usbaudio_alt        | only now may the producer run
    bra     4f
2:  clrb    usbaudio_alt            | stop the producer BEFORE tearing down
    | ☠ FLUSH the endpoint. Clearing ENDPTCTRL3 disables it but does NOT
    | cancel a dTD that is already primed, so a packet queued microseconds
    | before alt 0 still goes out and the host sees audio after teardown.
    | Intermittent by nature — it depends on whether a prime was in flight —
    | which is exactly why it showed up as a flaky gate rather than a clean
    | failure.
    movel   #EP3IN_BIT,%d0
    movel   %d0,EPFLUSH
3:  movel   EPFLUSH,%d0             | flush is complete when the bit clears
    andil   #EP3IN_BIT,%d0
    bnes    3b
    clrl    ENDPTCTRL3
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
    moveq   #4,%d1                  | AudioStreaming moved 3 -> 4 (two functions)
    cmpl    %d0,%d1
    bnes    1f
    pea     usbaudio_alt
    jmp     GETIFACE_REJOIN
1:  pea     GETIFACE_STOCKP
    jmp     GETIFACE_REJOIN

| ---- usb_isr shim (installed at 0x4001e606, was jmp usbmidi_isr_shim) -------
| Services the block-clock SOF (kick the builder) and EP3 IN completions (free
| the dTD, kick again), then chains to the usb-midi ISR shim which handles EP2
| and runs the original displaced instruction.
    .global audio_isr_shim
audio_isr_shim:
    lea     %sp@(-40),%sp
    moveml  %d0-%d7/%a0-%a1,%sp@
    | ☠ Exactly ONE place primes EP3: the per-block producer in frame_isr.
    | This shim only retires the completion. Priming from both would race on
    | usbaudio_busy (test-then-set, and frame_isr can preempt usb_isr), and
    | the block clock at ~2756/s already outruns the 1 kHz packet rate, so
    | there is nothing to gain by kicking from here. The SOF is not used at
    | all any more — the firmware's own block clock is the send clock.
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
    movel   aud_produced,%d0
    movel   usbaudio_consumed,%d1
    movel   %d0,%d2
    subl    %d1,%d2                 | d2 = frames available
    cmpil   #AUD_FRAMES,%d2
    blss    3f                      | within the ring: fine
    addql   #1,usbaudio_overruns
    movel   %d0,%d1
    subil   #AUD_FRAMES,%d1
    movel   %d1,usbaudio_consumed   | resync to the ring's trailing edge
    movel   #AUD_FRAMES,%d2
    | ☠ STARTUP CUSHION. Measured on hardware: with the consumer started at
    | "now" (consumed = produced, zero frames buffered) the first ~1.4 s of the
    | stream carried a burst of discontinuities -- audible as a crackle that
    | then settled -- because the host pulls a packet every frame while the
    | producer only appends 16 frames per block, so the consumer repeatedly
    | caught up to the producer before the rate accumulator settled. Holding
    | the first packet back until a cushion exists costs a few ms of latency
    | once, and the host simply sees no data for that moment. The emulator
    | cannot show this: its host pulls in lockstep, so the race never happens
    | and the capture is clean from the first frame.

3:  | ☠ STARTUP CUSHION, as a one-shot gate. Two bugs lived here: it sat after
    | the overrun branch, which the normal path jumps over (blss 3f), so it
    | never ran except following an overrun -- telemetry from a unit showed the
    | ring pinned at a fill of 0..56 frames instead of 2048, underrunning 1624
    | times in 0.9 s. And it must NOT be a per-packet condition: requiring the
    | cushion on every packet stalls the stream the moment fill dips, turning a
    | click into a gap. Hold the first packet until the buffer is built, then
    | let the servo keep it there.
    .if AUD_CUSHION_OVR == 0
    bras    .Lprimed                | no cushion: send as soon as frames exist
    .endif
    tstb    usbaudio_primed
    bnes    .Lprimed
    cmpil   #AUD_CUSHION,%d2
    bcss    .Lnotyet
    moveq   #1,%d3
    moveb   %d3,usbaudio_primed
    bras    .Lprimed
.Lnotyet:
    addql   #1,usbaudio_underruns
    bra     9f                      | still filling: send nothing yet
.Lprimed:
    movel   usbaudio_acc,%d3
    | ☠ RATE SERVO. A fixed 44.1 frames per packet assumes the Octatrack's
    | audio clock and the host's USB frame clock agree. They do not: they drift,
    | the ring fill walks towards one end, and eventually the stream either
    | overruns (resync to the trailing edge -- a jump) or underruns (no packet
    | -- a gap). Measured on hardware as four bursts of 10-14 discontinuities
    | in 30 s, at irregular intervals, which is drift rather than anything
    | musical. Nudging the drain rate by +-0.1 frame/packet against a target
    | fill corrects drift continuously instead of letting it accumulate into a
    | click. This is what an asynchronous endpoint is supposed to do, and the
    | correction is a rate change of ~0.2%, far below anything audible.
    movel   #441,%d5                | nominal 44.1 frames per packet
    .if AUD_SERVO == 0
    bras    .Lsrv_done              | servo off: fixed 44.1, the best measured
    .endif
    cmpil   #(AUD_TARGET+AUD_BAND),%d2
    bcss    .Lsrv_low
    movel   #442,%d5                | ring filling: drain a touch faster
    bras    .Lsrv_done
.Lsrv_low:
    cmpil   #(AUD_TARGET-AUD_BAND),%d2
    bccs    .Lsrv_done
    movel   #440,%d5                | ring draining: drain a touch slower
.Lsrv_done:
    addl    %d5,%d3
    moveq   #10,%d7
    movel   %d3,%d4
    divu.l  %d7,%d4                 | d4 = n = (acc+441)/10
    movel   %d4,%d5
    mulu.l  %d7,%d5
    movel   %d3,%d6
    subl    %d5,%d6                 | d6 = new acc = (acc+441) - n*10
    cmpl    %d4,%d2
    bccs    .Lhave_frames
    addql   #1,usbaudio_underruns
    bra     9f                      | available < n: underrun, send nothing
.Lhave_frames:
    movel   %d6,usbaudio_acc
    | copy n frames, wrap-safe, into the packet buffer
    movel   %d4,%d3
    movel   usbaudio_consumed,%d5
    lea     usbaudio_pktbuf,%a1
4:  movel   %d5,%d1
    andil   #AUD_FRAMES-1,%d1
    lsll    #2,%d1
    movel   #aud_ring,%a0
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
    moveal  qh_ep3,%a1              | resolved dQH, not the hardcoded guess
    movel   %a0,%a1@(8)             | dQH next-dTD (the bench reads this on prime)
    | ☠ PUSH THE DIRTY LINES BEFORE PRIMING. The payload now runs from the
    | flex heap, which ACR0 maps CACHEABLE COPYBACK — the old scratch at
    | 0x48000000+ was cache-inhibited, so stores went straight to RAM and this
    | was not needed. The USB controller is a bus master and does NOT snoop
    | the CPU's data cache, so a dTD or packet buffer still sitting dirty in
    | the D-cache is read by the controller as whatever RAM held before.
    | ☠ QEMU models no caches at all, so no gate in this tree can see this;
    | it would present on hardware as silence or garbage audio.
    | ☠ ALL FOUR WAYS PER LINE. On ColdFire V4e the CPUSHL operand selects a
    | cache SET AND WAY (low address bits pick the way), not simply a line by
    | address -- so the obvious one-cpushl-per-line loop pushes way 0 only and
    | leaves a line living in any other way dirty. Issuing the four way
    | encodings of each line is correct under that reading AND harmless under
    | the plain address-indexed reading, where it just re-pushes the same line.
    | A stale dTD is the worst case and matches the observed hardware symptom
    | exactly: the controller reads a token whose ACTIVE bit never reached RAM,
    | transmits nothing, and the host records unbroken digital silence.
    lea     usbaudio_dtd,%a1
    moveq   #2,%d1                  | the dTD: 32 B = two lines
    bsr     audio_push_lines
    lea     usbaudio_pktbuf,%a1
    moveq   #12,%d1                 | 180 B of packet = 12 lines of 16
    bsr     audio_push_lines
    moveq   #1,%d1
    moveb   %d1,usbaudio_busy
    movel   #EP3IN_BIT,%d1
    movel   %d1,EPPRIME
9:  rts


| Push %d1 cache lines starting at %a1 (16-byte aligned) out of the copyback
| D-cache, covering all four ways of each set. Clobbers %a1 and %d1.
audio_push_lines:
.Lpush_line:
    cpushl  %dc,%a1@                | way 0
    addql   #1,%a1
    cpushl  %dc,%a1@                | way 1
    addql   #1,%a1
    cpushl  %dc,%a1@                | way 2
    addql   #1,%a1
    cpushl  %dc,%a1@                | way 3
    lea     %a1@(13),%a1            | +3 consumed, so +13 lands on the next line
    subql   #1,%d1
    bnes    .Lpush_line
    rts


| Resolve EP3's queue head from ENDPTLISTADDR. Returns it in %a0 and caches it
| in qh_ep3. The register is 2 KB aligned, so the low 11 bits are not address.
| Falls back to the emulator-observed constant if the register reads as zero,
| so a controller that has not been set up yet cannot send us to address 0.
audio_qh_resolve:
    | ☠ VALIDATE BEFORE TRUSTING. stage2 runs during the card poll, early
    | enough that the controller need not be set up yet, so this register can
    | hold power-on garbage rather than zero. An unchecked read sent a 64-byte
    | clear to a wild address on real hardware and glitched the audio out.
    | The firmware programs 0x4EC94800 here (a literal in the image at
    | 0x4001d63a), so anything outside SDRAM is not an endpoint list and the
    | known-good constant is used instead.
    movel   EPLISTADDR,%d0
    andil   #0xfffff800,%d0
    cmpil   #0x40000000,%d0
    blts    .Lqh_fallback           | below SDRAM: garbage
    cmpil   #0x50000000,%d0
    bges    .Lqh_fallback           | above SDRAM: garbage
    bras    .Lqh_have
.Lqh_fallback:
    movel   #(QH_EP3IN - QH_EP3IN_OFF),%d0
.Lqh_have:
    addil   #QH_EP3IN_OFF,%d0
    movel   %d0,qh_ep3
    moveal  %d0,%a0
    rts


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
    | ☠ The producer used to idle until the host opened the stream, so the ring
    | was empty at alt 1 and the first ~127 packets could not be filled --
    | measured on hardware as exactly 127 underruns, all at startup, and heard
    | as a burst of clicks in the first 1.5 s. Keeping it running costs one
    | block of summing per interrupt whether or not anyone is listening, and
    | buys a stream that starts instantly with a full buffer behind it.
    | usbaudio_alt still gates the SENDING, just not the producing.
audio_frame_shim_body:
    movel   RB_PREV,%d0
    lsll    #8,%d0
    lsll    #2,%d0                  | prev * 1024 (imm shift is 1-8)
    addil   #RB_BASE,%d0
    moveal  %d0,%a2                 | a2 = this bank's track 0
    movel   aud_produced,%d4
    movel   %d4,%d5
    andil   #AUD_FRAMES-1,%d5
    lsll    #2,%d5
    movel   #aud_ring,%a3
    addal   %d5,%a3                 | a3 = write cursor (block never wraps:
                                    | 16 divides the ring size)
    moveq   #RB_SHIFT,%d1           | asr.l immediate is 1-8 only: shift via d1
    moveq   #15,%d6                 | 16 frames
1:
    | ☠ NOT `A == 0 || A == 6`: gas does not evaluate that as logical OR here,
    | it silently takes the else branch and the build produces the synthetic
    | triangle while claiming to produce the track sum. Verified with a
    | standalone .if test after two hardware captures measured the wrong signal.
    moveq   #0,%d2                  | L accumulator
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
    | saturate to s16 and store little-endian. ☠ ColdFire has no rotate, so the
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
    bpl     1b                      | not bpls: the telemetry source is long
    addql   #8,%d4
    addql   #8,%d4                  | 16 frames produced
    movel   %d4,aud_produced
    | the block clock IS the send clock: prime the next packet now. usb_isr's
    | EP3-completion path also kicks; usbaudio_busy serializes the two.
    tstb    usbaudio_alt            | nobody listening: produce, but do not send
    beqs    9f
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
| ☠ PATCH_USB / PATCH_FRAME are build-time defsyms (both 1 normally). They
| exist so a payload can install a SUBSET of the hooks, which is how a
| hardware fault gets bisected by copying one file to the card instead of
| reflashing — the whole point of the self-describing payload header.
| tests/usb-audio-bisect.sh builds the set.
usbaudio_patches:
.if PATCH_USB
    .long 0x4001e606, exp_isr,      pat_isr,      6
    .long 0x4001dd04, exp_setiface, pat_setiface, 6
    .long 0x4001d824, exp_getiface, pat_getiface, 6
    .long 0x4001d882, exp_cfg_fs,   pat_cfg_fs,   4
    .long 0x4001d88a, exp_cfg_hs,   pat_cfg_hs,   4
    .long 0x4001d8c0, exp_cfg_os_hs,pat_cfg_os_hs,4
    .long 0x4001d8c8, exp_cfg_os_fs,pat_cfg_os_fs,4
    .long 0x4001d858, exp_clamp,    pat_clamp1,   6
    .long 0x4001d896, exp_clamp,    pat_clamp2,   6
.endif
.if PATCH_FRAME
    .long 0x4000d9a0, exp_frame,    pat_frame,    6
.endif
.if PATCH_USB
    .long 0x400e2004, exp_devcls,   pat_devcls,   3
    .if HEAP_RESERVE
    | ---- shrink the flex heap so the bottom pages are permanently ours ------
    | flex_heap_init memsets the WHOLE heap and re-seeds the free-page stack on
    | every project load, which is what destroys a heap-allocated payload. Four
    | immediates make it skip the lowest HEAP_RESERVE+1 pages (page 0 is
    | already reserved), so those pages are outside BOTH the memset and the
    | free list -- memory the firmware itself no longer believes exists. The
    | payload is loaded straight into them, so nothing ever has to relocate.
    |
    | ☠ Bottom, not top: the recorder reserve pops from the TOP of the free
    | stack, so the top pages are contended. The bottom sits at a fixed known
    | address (sample_heap_base) that nothing claims once it is out of the list.
    |
    | ☠ These are RUNTIME writes by the card payload, never flashed edits. With
    | no /USBAUDIO.BIN, or with NO held at boot, flex_heap_init keeps its stock
    | constants and the machine is untouched -- which is the whole recovery
    | contract.
    .long 0x40096f82, exp_heap_cnt, pat_heap_cnt, 4   | free_top seed
    .long 0x40096fa5, exp_heap_1st, pat_heap_1st, 1   | first seeded index
    .long 0x40097008, exp_heap_len, pat_heap_len, 4   | memset length
    .long 0x4009700e, exp_heap_base,pat_heap_base,4   | memset base
    .endif
.endif
    .long 0

exp_isr:       .byte 0x4e,0xf9
               .long USBMIDI_ISR_SHIM
    .if HEAP_RESERVE
.set HEAP_PAGES,  HEAP_RESERVE + 1      | page 0 is already out of the free list
.set HEAP_BYTES,  HEAP_PAGES * 6144
exp_heap_cnt:  .long 14602
pat_heap_cnt:  .long 14602 - HEAP_RESERVE
exp_heap_1st:  .byte 1
pat_heap_1st:  .byte 1 + HEAP_RESERVE
    .balign 2
exp_heap_len:  .long 89720832
pat_heap_len:  .long 89720832 - HEAP_BYTES
exp_heap_base: .long 0x40a955e0
pat_heap_base: .long 0x40a955e0 + HEAP_BYTES
    .endif

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
| ☠ The device-descriptor class triple. An Interface Association Descriptor
| is only honoured when the DEVICE declares Miscellaneous / Common Class /
| IAD; with 00/00/00 a host may ignore the association entirely and leave the
| audio function ungrouped. Interface-level classes are unaffected, so mass
| storage still binds exactly as before.
exp_devcls:    .byte 0x00,0x00,0x00
pat_devcls:    .byte 0xef,0x02,0x01
exp_frame:     .byte 0x42,0xb9,0x46,0x10,0x4d,0x4e   | clrl 0x46104d4e
pat_frame:     .byte 0x4e,0xf9
    .if GUARD_FRAME
               .long GUARD_FRAME    | via the image-resident guard, so a missing
                                    | payload is silence rather than an illegal
                                    | instruction at IPL 5
    .else
               .long audio_frame_shim
    .endif

| ---- payload state ----------------------------------------------------------
    .balign 4
    .global usbaudio_consumed, usbaudio_acc, usbaudio_overruns
    .global usbaudio_underruns
usbaudio_consumed: .long 0          | frames pulled from the tap ring
usbaudio_acc:      .long 0          | 44.1-frame accumulator (x10)
usbaudio_overruns: .long 0
usbaudio_underruns: .long 0    | packets we could not fill
aud_produced:      .long 0          | producer frame count (relocates with us)
aud_phase:         .long 0          | synthetic-source phase, 0..99
qh_ep3:            .long 0          | EP3 IN dQH, read from ENDPTLISTADDR
aud_ring:          .space AUD_FRAMES*4   | 4096 frames, 4 B each (s16 LE L,R)
usbaudio_alt:      .byte 0          | current AudioStreaming alt setting
usbaudio_busy:     .byte 0          | an EP3 IN transfer is in flight
usbaudio_primed:   .byte 0          | the startup cushion has been built
    .balign 4
.ifndef DMA_FIXED
.set DMA_FIXED, 0
.endif
.if DMA_FIXED
| ☠ The two structures the USB controller DMAs live OUTSIDE the payload blob,
| in cache-inhibited memory. The blob runs from flex heap pages, which ACR0
| maps cacheable copyback, and the controller is a bus master that does not
| snoop the CPU's data cache -- so a dTD written by the CPU can still be dirty
| in cache when the controller fetches it, and the controller then reads
| whatever RAM held before. That presents exactly as measured on hardware:
| ENDPTPRIME clears (the prime was consumed) while ENDPTSTAT never arms (the
| descriptor it found was not ACTIVE). Above ACR0's 0x40000000-0x47FFFFFF
| window the default CACR data mode is inhibited, so stores here reach RAM
| with no cache maintenance at all. 0x49000000 is write-verified clean on
| hardware (custom/coldfire/memtest-probe.s, two patterns).
.set usbaudio_dtd,    DMA_FIXED
.set usbaudio_pktbuf, DMA_FIXED + 64
.else
usbaudio_dtd:      .space 32        | the single EP3 IN dTD (0x20-aligned area)
usbaudio_pktbuf:   .space 192       | one iso packet (<= 180 B)
.endif

    .balign 4
    .include "usb-audio-cfg.s"
