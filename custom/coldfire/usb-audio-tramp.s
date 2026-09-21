| SPDX-License-Identifier: MIT
| usb-audio-tramp.s — the in-image trampoline for the USB-audio payload.
|
| Linked by custom/usb-audio.py into the 408 B of image free space left after
| usb-midi's overlay (0x400d2b44). It is the ONLY new code that lives in the
| image; everything else runs from SDRAM scratch.
|
| Hooked into fs_card_detect_poll (0x4003f174), a task-context leaf polled by
| the card/UI task — task context is required because loading the payload does
| filesystem I/O (mutex, blocking reads), which an ISR could not do. Once the
| card is mounted it reads /USBAUDIO.BIN into scratch, invalidates the
| instruction cache, and jsr's the payload's stage2 (which installs the rest of
| the hooks). Run-once, and a no-op if the card file is absent — an image
| without /USBAUDIO.BIN then behaves exactly as the stock usb-midi composite.
|
| defsyms from usb-audio.py: PAYLOAD_BASE (0x48001000), PAYLOAD_MAGIC,
| PAYLOAD_MAX. The entry point comes from the payload's own header.

.ifndef HAVE_GUARD
.set HAVE_GUARD, 0
.endif
.set FS_MOUNT_KIND, 0x460d1cb8      | 1 = CF card mounted
.set FS_OPEN,   0x46c8242a          | fs vtable: open(path, mode) -> fid
.set FS_SIZE,   0x46c8241e          | fs vtable: size(fid) -> bytes
.set FS_READ,   0x46c82426          | fs vtable: read(fid, buf, len) -> got
.set FS_CLOSE,  0x46c82422          | fs vtable: close(fid)
.set MODE_R,    0x400b3289          | the "r" string the firmware opens with
.set CACR_ICINVA, 0xa40ce100        | steady-state CACR + ICINVA
| ☠ Hold NO while the card mounts and the payload is never loaded at all —
| recovery without pulling the card or deleting a file. panel_key_state
| (0x46100b18) is 8 debounced key-group bytes: group = id/8, bit = id%8, and
| NO is key id 50 -> byte +6, bit 2. MEASURED, not derived: holding NO reads
| 00 00 00 00 00 00 04 00 and all zero on release.
.set PANEL_KEY_NO,  0x46100b1e      | panel_key_state + 6
.set PANEL_KEY_NO_B, 2              | NO = id 50, 50 % 8
| ☠ The card mounts BEFORE the key subsystem is alive, so checking NO at
| mount time reads zero however hard the key is held (measured: the payload
| loaded anyway). Gate the whole load on the UI key handler list instead —
| BSS-cleared at boot, non-zero only once ui_task has registered handlers.
| That is observable guest state, not a tuned delay, and it also moves the
| copy to where it belongs: after the UI is up.
.set UI_KEY_LIST,   0x460d165c      | ui_key_handler_list; 0 until UI is up
| The firmware's own dismissible on-screen notification: popup(str, kind).
| Args pushed right-to-left (`pea kind; pea str; jsr`), kind 0x30 is the
| ordinary one used by 87 of its 225 call sites, e.g. "RECORDING CLEARED!".
| ☠ Without this the install declines SILENTLY — a healthy-looking machine
| that simply has no USB audio, which is indistinguishable from success
| unless you go and enumerate the USB interfaces from a host.
.set POPUP,         0x4005a2b8

    .text
    .global usbaudio_tramp
| fs_card_detect_poll displaced prologue: movel %d2,%sp@- ; movel 0x460d1cbc,%d1
usbaudio_tramp:
    movel   %d2,%sp@-               | displaced
    movel   0x460d1cbc,%d1          | displaced (d1 must survive to the rejoin)
    tstb    usbaudio_tramp_done
    beqs    0f
    | ☠ Already installed: this is the only TASK-context callback the feature
    | has, so it is where the hook guard reports that the payload went missing.
    | Raising a popup from the frame ISR at IPL 5 is not safe.
    .if HAVE_GUARD
    jsr     UA_GUARD_POLL
    .endif
    bra     9f
0:
    movel   FS_MOUNT_KIND,%d0
    moveq   #1,%d2
    cmpl    %d0,%d2                 | mounted?
    bnes    9f                      | not yet — retry on the next poll
    movel   UI_KEY_LIST,%d0         | UI keys up yet?
    beq     9f                      | no — retry on a later poll
    moveq   #1,%d0
    moveb   %d0,usbaudio_tramp_done | attempt exactly once (even on failure)
    moveb   PANEL_KEY_NO,%d0        | NO held? skip the payload for this boot
    btst    #PANEL_KEY_NO_B,%d0
    beqs    4f
    moveq   #6,%d0                  | "USBAUD E6" — skipped on purpose
    jsr     UA_REPORT
    bra     9f
4:
    bsr     usbaudio_load
9:  jmp     0x4003f17c              | rejoin after the displaced instructions

| Load /USBAUDIO.BIN into scratch, I-cache invalidate, jsr stage2. Preserves
| every register the poll caller still needs (d1 = old detect state, d2 saved
| on the stack by the displaced push).
|
| ☠ EVERY reason to reject the card file must leave the machine as the stock
| usb-midi composite, because deleting the file from the card is the ONLY
| recovery path this feature is allowed to need.
|
| The payload is SELF-DESCRIBING: a 16-byte header (magic, total length,
| checksum, entry point) followed by the code. The gates are, in order:
|   1. the file opens at all;
|   2. fs_size is sane (>= 16, <= MAX, a whole number of longwords);
|   3. the header magic is ours;
|   4. the header's length equals the file's actual size — this is what
|      catches a TRUNCATED copy, which the magic alone cannot (the read takes
|      a sector count, so a short file still has a valid first longword);
|   5. the longword sum over the file equals the header's checksum.
| Only then is the header's entry point called.
|
| ☠ Deliberately NOT a constant baked into the image: an image that only
| accepts the one payload it was built with forces a REFLASH for every
| payload experiment, which is the slowest possible way to work on hardware.
| Self-consistency rejects truncation and corruption just as hard, while
| letting a flashed unit run any correctly built payload from the card.
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
    bne     4f
    moveq   #1,%d0                  | no file on the card
    bra     7f
4:
    movel   %d0,%d7                 | d7 = fid
    | gate 2: a sane size
    movel   %d7,%sp@-
    movel   FS_SIZE,%a0
    jsr     %a0@
    addql   #4,%sp
    movel   %d0,%d6                 | d6 = file size in bytes
    moveq   #16,%d1
    cmpl    %d0,%d1
    bles    4f
    moveq   #2,%d0
    bra     6f                      | bad size -> close, then report
4:
    movel   #PAYLOAD_MAX,%d1
    cmpl    %d0,%d1
    blt     7f                      | larger than the scratch window
    movel   %d0,%d1
    andil   #3,%d1
    bne     7f                      | not a whole number of longwords
    | ☠ ASK THE FIRMWARE FOR THE MEMORY. Two hand-picked addresses were
    | "verified free" earlier and both were wrong on a working unit — the
    | second was overwritten once a real project loaded, crashing inside our
    | own code. usbaudio_alloc pops contiguous pages off the flex heap's free
    | stack, so the heap itself knows the memory is taken.
.if UA_FIXED_BASE
    | ☠ TEST CONFIGURATION ONLY. A fixed load address, for probing whether a
    | candidate region is durable on real hardware. The payload is relocatable
    | so this costs nothing to support — but a fixed address is precisely what
    | failed twice, and must never be the shipping configuration without a
    | hardware measurement of that region UNDER LOAD.
    movel   #UA_FIXED_BASE,%d0
.else
    jsr     UA_ALLOC
.endif
    tstl    %d0
    bne     5f
    moveq   #8,%d0                  | "USBAUD E8" — no memory
    bra     6f
5:  moveal  %d0,%a4                 | a4 = load base, for the rest of this routine
    | Read the blob into the allocated base. ☠ The fs read vtable counts SECTORS
    | (512 B), not bytes — the same ABI fs_copy_file/os_upgrade_stage use.
    |
    | ☠ IN CHUNKS OF AT MOST READ_CHUNK SECTORS. Measured on hardware: a read
    | of 8 sectors lands at PAYLOAD_BASE correctly, while 24 and 34 sectors
    | write over the OS image at 0x40001000 instead — the damage extent
    | tracking the transfer size, and the machine dying on an illegal
    | instruction wherever the overwrite reached live code. The emulator never
    | shows this because its ATA model advertises no DMA (ata_mode_bytes reads
    | pio 1, mdma -1, udma -1), so the firmware always takes the PIO loops;
    | a real CF card reports DMA and multi-sector reads go down the eDMA
    | channel-14 path instead. Small reads stay on the working path.
    movel   %d6,%d5
    addil   #511,%d5
    lsrl    #8,%d5
    lsrl    #1,%d5                  | d5 = sectors remaining
    moveal  %a4,%a3                 | a3 = destination cursor
1:  tstl    %d5
    beqs    2f
    movel   %d5,%d4
    cmpil   #READ_CHUNK,%d4
    blss    3f
    movel   #READ_CHUNK,%d4         | d4 = sectors this pass
3:  movel   %d4,%sp@-
    movel   %a3,%sp@-
    movel   %d7,%sp@-
    movel   FS_READ,%a0
    jsr     %a0@
    lea     %sp@(12),%sp
    movel   %d4,%d0
    lsll    #8,%d0
    lsll    #1,%d0                  | bytes = sectors * 512
    addal   %d0,%a3
    subl    %d4,%d5
    bras    1b
2:
    | close(fid)
    movel   %d7,%sp@-
    movel   FS_CLOSE,%a0
    jsr     %a0@
    addql   #4,%sp
    | gates 3-5: header magic, declared length == actual size, checksum.
    movel   %a4@,%d0
    cmpl    #PAYLOAD_MAGIC,%d0
    beqs    4f
    moveq   #3,%d0
    bra     7f
4:
    movel   %a4@(4),%d0
    cmpl    %d6,%d0
    beqs    4f
    moveq   #4,%d0                  | declared length != actual size
    bra     7f
4:
    | sum every longword, then subtract the stored checksum: what remains
    | must equal it (the checksum field is built to make that hold).
    moveal  %a4,%a0
    movel   %d6,%d1
    lsrl    #2,%d1                  | longword count
    clrl    %d0
6:  addl    %a0@+,%d0
    subql   #1,%d1
    bnes    6b
    movel   %a4@(8),%d1
    subl    %d1,%d0
    cmpl    %d1,%d0
    beqs    4f
    moveq   #5,%d0                  | checksum mismatch
    bra     7f
4:
    | ☠ The payload relocates ITSELF and does its own cache maintenance (it
    | now runs from cacheable heap memory, not cache-inhibited scratch), so
    | the invalidate that used to live here belongs to it.
    | ☠ A header entry offset of 0 means LOAD BUT DO NOT EXECUTE.
    | That is the only way to separate "the ICINVA broke something" from
    | "fetching instructions out of 0x48000000+ broke something" without a
    | reflash per experiment — the two suspects left after hardware bisection
    | showed that writing scratch is fine and executing it is not.
    movel   %a4@(12),%d0            | header entry OFFSET from the load base
    beq     8f                      | 0 -> loaded, never run
    lea     %a4@(0,%d0:l),%a0
    jsr     %a0@                    | stage2 installs the hooks AND reports
    bras    8f                      | its own outcome (E0 or E7) — not ours
6:  movel   %d0,%d3                 | reject after open: close the file first
    movel   %d7,%sp@-
    movel   FS_CLOSE,%a0
    jsr     %a0@
    addql   #4,%sp
    movel   %d3,%d0
7:  jsr     UA_REPORT               | say what happened, on screen
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
