| SPDX-License-Identifier: MIT
| usb-audio-tramp.s — the in-image trampoline for the USB-audio payload.
|
| Linked by custom/usb-audio.py into what usb-midi leaves of the in-image free
| zone (0x400d2b44). It is the ONLY new code that lives in the image;
| everything else runs from SDRAM scratch.
|
| Hooked into fs_card_detect_poll (0x4003f174), a task-context leaf polled by
| the card/UI task — task context is required because loading the payload does
| filesystem I/O (mutex, blocking reads), which an ISR could not do. Once the
| card is mounted it reads /USBAUDIO.BIN into scratch, invalidates the
| instruction cache, and jsr's the payload's stage2 (which installs the rest of
| the hooks). Run-once, and a no-op if the card file is absent — an image
| without /USBAUDIO.BIN then behaves exactly as the stock usb-midi composite.
|
| defsyms from usb-audio.py: STAGE2 (payload stage2 entry), STAGE2_MAGIC (its
| first longword), PAYLOAD_BASE (0x48001000), PAYLOAD_LEN (blob size),
| PAYLOAD_SECTORS (its size in 512 B sectors), and PAYLOAD_WORDS / PAYLOAD_SUM
| (longword count and sum, for gate 3).

.set FS_MOUNT_KIND, 0x460d1cb8      | 1 = CF card mounted
.set FS_OPEN,   0x46c8242a          | fs vtable: open(path, mode) -> fid
.set FS_SIZE,   0x46c8241e          | fs vtable: size(fid) -> bytes
.set FS_READ,   0x46c82426          | fs vtable: read(fid, buf, len) -> got
.set FS_CLOSE,  0x46c82422          | fs vtable: close(fid)
.set MODE_R,    0x400b3289          | the "r" string the firmware opens with
.set CACR_ICINVA, 0xa40ce100        | steady-state CACR + ICINVA

    .text
    .global usbaudio_tramp
| fs_card_detect_poll displaced prologue: movel %d2,%sp@- ; movel 0x460d1cbc,%d1
usbaudio_tramp:
    movel   %d2,%sp@-               | displaced
    movel   0x460d1cbc,%d1          | displaced (d1 must survive to the rejoin)
    tstb    usbaudio_tramp_done
    bnes    9f
    movel   FS_MOUNT_KIND,%d0
    moveq   #1,%d2
    cmpl    %d0,%d2                 | mounted?
    bnes    9f                      | not yet — retry on the next poll
    moveq   #1,%d0
    moveb   %d0,usbaudio_tramp_done | attempt exactly once (even on failure)
    bsr     usbaudio_load
9:  jmp     0x4003f17c              | rejoin after the displaced instructions

| Load /USBAUDIO.BIN into scratch, I-cache invalidate, jsr stage2. Preserves
| every register the poll caller still needs (d1 = old detect state, d2 saved
| on the stack by the displaced push).
|
| ☠ EVERY reason to reject the card file must leave the Octatrack as the stock
| usb-midi composite, because deleting the file from the card is the ONLY
| recovery path this feature is allowed to need. Three gates, in order:
|   1. the file opens at all;
|   2. its size is EXACTLY the blob this image was built against (a short or
|      truncated copy is the likely real-world corruption, and the fixed
|      sector-count read below cannot detect one on its own);
|   3. the longword sum of what actually landed matches what was built.
| Only then is stage2 entered. A mismatch at any gate installs NOTHING.
usbaudio_load:
    lea     %sp@(-64),%sp
    moveml  %d0-%d7/%a0-%a6,%sp@
    | fid = open("/USBAUDIO.BIN", "r")
    pea     MODE_R
    pea     usbaudio_path
    movel   FS_OPEN,%a0
    jsr     %a0@
    lea     %sp@(8),%sp
    moveq   #-1,%d1
    cmpl    %d0,%d1
    beq     8f                      | open failed -> no audio (stock behaviour)
    movel   %d0,%d7                 | d7 = fid
    | gate 2: size(fid) must be the exact built length
    movel   %d7,%sp@-
    movel   FS_SIZE,%a0
    jsr     %a0@
    addql   #4,%sp
    cmpil   #PAYLOAD_LEN,%d0
    bnes    7f                      | wrong size -> close, install nothing
    | read the blob into PAYLOAD_BASE. ☠ The fs read vtable counts SECTORS
    | (512 B), not bytes — the same ABI fs_copy_file/os_upgrade_stage use.
    pea     PAYLOAD_SECTORS
    pea     PAYLOAD_BASE
    movel   %d7,%sp@-
    movel   FS_READ,%a0
    jsr     %a0@
    lea     %sp@(12),%sp
    | close(fid)
    movel   %d7,%sp@-
    movel   FS_CLOSE,%a0
    jsr     %a0@
    addql   #4,%sp
    | gate 3: the blob's first long is stage2's prologue AND the longword sum
    | of the whole loaded image matches the one built into this firmware.
    movel   PAYLOAD_BASE,%d0
    cmpl    #STAGE2_MAGIC,%d0
    bnes    8f
    movel   #PAYLOAD_BASE,%a0
    movel   #PAYLOAD_WORDS,%d1
    clrl    %d0
6:  addl    %a0@+,%d0
    subql   #1,%d1
    bnes    6b
    cmpil   #PAYLOAD_SUM,%d0
    bnes    8f                      | corrupt copy -> install nothing
    | invalidate I-cache + branch cache before the first fetch from scratch
    movel   #CACR_ICINVA,%d0
    movec   %d0,%cacr
    nop
    jsr     STAGE2                  | installs the rest of the hooks
    bras    8f
7:  movel   %d7,%sp@-               | reject after open: close the file first
    movel   FS_CLOSE,%a0
    jsr     %a0@
    addql   #4,%sp
8:  moveml  %sp@,%d0-%d7/%a0-%a6
    lea     %sp@(64),%sp
    rts

| kept in .text so the linked blob is contiguous (the slack is writable RAM at
| runtime, so the run-once flag lives here too).
usbaudio_path:
    .asciz  "/USBAUDIO.BIN"
    .balign 2
usbaudio_tramp_done:
    .byte   0
    .balign 2
