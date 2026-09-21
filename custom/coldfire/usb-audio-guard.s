| SPDX-License-Identifier: MIT
| usb-audio-guard.s — image-resident guard between a hook site and the payload.
|
| ☠ WHY THIS EXISTS. Every hook this feature installs was a bare `jmp` into
| payload memory. The instant that memory stops being our code — a project load
| wiping the heap, an allocation handed to a FLEX sample, any placement mistake
| — the machine executes whatever is there. On a real unit that is an illegal
| instruction at IPL 5 with the PC somewhere in the sample heap
| (VEC:04, ADDR:44A8E9xx), seen four times across three different payload
| locations, and it is unrecoverable without pulling the card.
|
| The guard makes that failure mode SILENCE instead of a crash: it checks the
| payload's 'OTPL' header before jumping, and if the payload is gone it runs the
| displaced instruction and rejoins the stock code. It lives in the IMAGE, so it
| cannot itself be erased by whatever erased the payload.
|
| ☠ It stays inert without a card payload: nothing points at the guard until
| stage2 installs the hook, so deleting /USBAUDIO.BIN leaves a stock machine.

.set POPUP,        0x4005a2b8
.set OTPL,         0x4F54504C       | payload header magic, written by the loader
.set FRAME_REJOIN, 0x4000d9a6       | after the displaced clrl in frame_isr
.set FRAME_DISPL,  0x46104d4e       | what the displaced instruction clears

    .text
    .global usbaudio_guard_frame
| Installed at the frame_isr hook site in place of a direct jump to the payload.
| ☠ The hook site is the last instruction before frame_isr's moveml epilogue, so
| every register is about to be reloaded from the stack: this may clobber freely,
| but must not touch %sp.
usbaudio_guard_frame:
    movel   #PAYLOAD_BASE,%a0
    movel   %a0@,%d0
    cmpil   #OTPL,%d0
    bnes    .Lgone
    movel   usbaudio_guard_shim,%d0 | where stage2 said the producer lives
    beqs    .Lgone                  | never installed: behave as if gone
    moveal  %d0,%a0
    jmp     %a0@                    | payload intact: run the producer
.Lgone:
    moveq   #1,%d0                  | latch it for the poll-context reporter
    moveb   %d0,usbaudio_guard_lost
    clrl    FRAME_DISPL             | the displaced instruction
    jmp     FRAME_REJOIN

    .global usbaudio_guard_poll
| Called from the trampoline on every card poll, i.e. TASK context — the popup
| must not be raised from the frame ISR at IPL 5. Reports once.
usbaudio_guard_poll:
    tstb    usbaudio_guard_lost
    beqs    1f
    tstb    usbaudio_guard_said
    bnes    1f
    moveq   #1,%d0
    moveb   %d0,usbaudio_guard_said
    movel   %d1,%sp@-
    movel   %a1,%sp@-
    pea     0x30
    pea     guard_msg
    movel   #POPUP,%a0
    jsr     %a0@
    lea     %sp@(8),%sp
    moveal  %sp@+,%a1
    movel   %sp@+,%d1
1:  rts

guard_msg:
    .asciz  "USBAUD LOST"
    .balign 2
| ☠ The shim's address is WRITTEN HERE BY STAGE2, not compiled in. Baking it
| into the guard made the IMAGE depend on the payload's internal layout, so
| adding a few instructions to the producer moved the shim and forced a
| reflash for what is otherwise a card-file-only change. The guard must stay
| payload-layout-independent or iteration costs a flash every time.
    .balign 4
    .global usbaudio_guard_shim
usbaudio_guard_shim: .long 0
usbaudio_guard_lost: .byte 0
usbaudio_guard_said: .byte 0
    .balign 2
