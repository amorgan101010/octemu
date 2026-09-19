| SPDX-License-Identifier: MIT
| RECEIVE machine -- ColdFire V4e audio handler for machine id 3.
| Overwrites stock NEIGHBOR prep+audio at 0x4000463c..0x4000472b (240 bytes)
| and absorbs the prep's voice-marking, so 0x400d6454[3] can be NULLed.
|
| Six source slots.  The LEVELS live on page 1 because only page-1 parameters
| can be LFO destinations or scene/p-lock targets -- PMTR decomposes /6 %6 over
| five pages of six.  The ROUTING lives on page 2, where static things belong.
|
|   p0..p5   LV1..LV6    0..127 attenuator, 0 = off
|   p6..p11  SRC1..SRC6  0 = OFF, 1..8 = take that track
|
| Page-1 values reach us at record offset i*2 (staged from the voice working
| buffer).  Page-2 values are staged separately, from 0x80000830 + track*72,
| and the frame builder copies twelve bytes of that -- six u16, exactly the
| page-2 count -- to record offsets 24..35.  Hardware says those are BYTES,
| not u16: reading them at stride 2 made SRC1/SRC3/SRC5 answer for LV1/LV2/LV3
| and left LV4..LV6 dead.  So SRC is at 24 + k, stride 1.
|
| Called TWICE per block by the dispatcher:
|   0x4000d43e  (track, READ sel,  0,     split)   <- previous block's machine
|   0x4000d52c  (track, WRITE sel, split, 16)      <- current machine
| n == 0 is normal and must still emit the 16-byte header.
|
| Appends to *0x80001c80:  [n][0][0x04000000][0] + n*8 bytes stereo.
| Sources are read from 0x80003190 + rsel*1024 + src*128 + start*8 -- post-FX
| and pre-fader, both confirmed on hardware (FX carry through, mute does not).
| Parameters are at 0x80000510 + (sel*8+track)*48 + i*2, value in the HIGH byte.

        .text
        .globl  recv
recv:
        lea     -28(%sp),%sp
        movem.l %d2-%d7/%a2,(%sp)
        | track=32(sp) sel=36(sp) start=40(sp) end=44(sp)
        move.l  44(%sp),%d1
        sub.l   40(%sp),%d1             | d1 = n frames
        move.l  0x80001c80,%a0
        move.l  %d1,(%a0)+              | header: count
        clr.l   (%a0)+                  |         position
        move.l  #0x04000000,(%a0)+      |         rate = 1.0 Q6.26
        clr.l   (%a0)+                  |         fraction
        move.l  %a0,%d6                 | d6 = payload base
        tst.l   %d1
        ble     9f                      | n == 0 (split == 0): header only

        move.l  %a0,%a1                 | zero the payload
        move.l  %d1,%d0
2:      clr.l   (%a1)+
        clr.l   (%a1)+
        subq.l  #1,%d0
        bnes    2b

        move.l  32(%sp),%d2             | d2 = track
        move.l  %d2,%d0                 | -- absorbed prep: mark the voice
        moveq   #42,%d3
        lsl.l   #2,%d3                  | 168
        mulsl   %d3,%d0
        movea.l %d0,%a0
        adda.l  #0x800049d8,%a0
        move.b  #-1,(%a0)               | voice[0x00] = active
        moveq   #3,%d0
        move.b  %d0,20(%a0)             | voice[0x14] = machine id
        clr.l   48(%a0)                 | voice[0x30] = 0   generic voice code
        moveq   #64,%d0                 | voice[0x34] = 64  reads both
        move.l  %d0,52(%a0)

        move.l  36(%sp),%d0             | rec = 0x80000510 + (sel*8+track)*48
        lsl.l   #3,%d0
        add.l   %d2,%d0
        moveq   #48,%d3
        mulsl   %d3,%d0
        add.l   #0x80000510,%d0
        movea.l %d0,%a2

        move.l  0x800000e4,%d4          | source base for track 0
        lsl.l   #8,%d4
        lsl.l   #2,%d4
        move.l  40(%sp),%d0
        lsl.l   #3,%d0
        add.l   %d0,%d4
        add.l   #0x80003190,%d4

        move.l  #0xa0,%macsr            | signed fractional, saturating
        moveq   #5,%d5                  | slot 5..0
3:      move.l  %d5,%d0
        add.l   %d0,%d0                 | k*2
        mvz.b   %a2@(24,%d5:l),%d7      | SRCk : byte, stride 1
        beqs    8f
        mvz.b   %a2@(0,%d0:l),%d3       | LVk  : 0..127 attenuator
        beqs    8f
        subq.l  #1,%d7                  | source track index 0..7 (count=9 bounds it)
        lsl.l   #7,%d7
        movea.l %d4,%a0
        add.l   %d7,%a0                 | that track's slot
        movea.l %d6,%a1
        move.l  %d1,%d7
        add.l   %d7,%d7                 | two words per frame
        | LV 127 COPIES, and that is what makes RECEIVE a GENERALISATION of
        | NEIGHBOR rather than an approximation of it.  A Q31 fraction cannot
        | be 1.0 -- LV<<24 tops out at 0x7f000000 -- so the MAC path scales by
        | 127/128 (0.992188) at full level, short of a copy on every frame.
        | So the top of the range takes the same straight copy NEIGHBOR does
        | (0x400046fe), and SRC = the preceding track at LV 127 is NEIGHBOR,
        | bit for bit.
        moveq   #127,%d0
        cmp.l   %d0,%d3
        bnes    5f
7:      move.l  (%a0)+,%d0
        add.l   %d0,(%a1)+
        subq.l  #1,%d7
        bnes    7b
        bras    8f
5:      swap    %d3
        lsl.l   #8,%d3                  | level as a Q31 fraction
4:      move.l  (%a0)+,%d0
        mac.l   %d0,%d3,%acc0
        movclr.l %acc0,%d0
        add.l   %d0,(%a1)+
        subq.l  #1,%d7
        bnes    4b
8:      subq.l  #1,%d5
        bpls    3b

9:      move.l  %d1,%d0                 | advance the stream pointer
        lsl.l   #3,%d0
        add.l   %d6,%d0
        move.l  %d0,0x80001c80
        movem.l (%sp),%d2-%d7/%a2
        lea     28(%sp),%sp
        rts
