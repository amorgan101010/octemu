| SPDX-License-Identifier: MIT
| usb-audio-report.s — on-screen status for the payload install.
|
| Lives in its own image free zone (docs/FREE-SPACE.md: 199 B at 0x400c14d5)
| because the trampoline's zone has only tens of bytes left.
|
| ☠ WHY THIS EXISTS. Every reason the trampoline declines a card payload was
| SILENT: the machine boots perfectly and simply has no USB audio, which is
| indistinguishable from success without enumerating USB interfaces from a
| host. That cost a full diagnostic round trip. An install that can decline
| must say so on the device.
|
| Uses the firmware's own dismissible notification, popup(str, kind), the one
| that shows "RECORDING CLEARED!" and friends; kind 0x30 is the ordinary form
| used by 87 of its 225 call sites.

.set POPUP, 0x4005a2b8

    .text
    .global usbaudio_report
| d0 = reason code 0..9. Clobbers d0/a0 only; the caller has already saved
| everything the fs poll needs.
|   0 installed      1 no file        2 bad size     3 bad magic
|   4 bad length     5 bad checksum   6 skipped (NO held)
usbaudio_report:
    movel   %d1,%sp@-
    movel   %a1,%sp@-
    addil   #0x30,%d0
    moveb   %d0,ua_msg+8
    pea     0x30
    pea     ua_msg
    movel   #POPUP,%a0
    jsr     %a0@
    lea     %sp@(8),%sp
    moveal  %sp@+,%a1
    movel   %sp@+,%d1
    rts

ua_msg:
    .asciz  "USBAUD E0"
    .balign 2
