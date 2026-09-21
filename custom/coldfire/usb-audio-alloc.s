| SPDX-License-Identifier: MIT
| usb-audio-alloc.s — get memory from the firmware's own page allocator.
|
| ☠ WHY: twice this feature used a hand-picked address (0x48001000, then
| 0x49000000) "verified free" by a probe. Both were verified against a machine
| that did not match how the unit is actually used — first the emulator's
| memory map, then a unit with no project loaded — and the second overwrote
| the payload once a real 1.8 GB set was in memory, crashing inside our own
| code. The heap knows what is in use; we do not. So ask it.
|
| The flex sample heap is a paged allocator: 6144-byte pages, page N at
| sample_heap_base + N*6144. Free page indices live on a stack; allocation
| pops from the top. recorder_reserve_alloc (0x400948cc) is the firmware's own
| user of it and the idiom is copied from there, including the IPL7 mask.
|
| ☠ Pages are popped highest-first, so consecutive pops are NORMALLY
| descending-contiguous — but fragmentation can break that, and a payload
| split across a gap would execute garbage. Contiguity is CHECKED, never
| assumed; on failure the pages are returned and the caller is told.
|
| Returns d0 = base address of NPAGES contiguous pages, or 0 on failure.

.set FLEX_TOP,   0x80006920         | free-stack cursor (index)
.set FLEX_BASE,  0x8000691c         | free-stack base   (index)
.set FLEX_STACK, 0x46c2e9c0         | the stack itself, word entries
.set FLEX_SHADOW,14602              | alloc clears entry and entry+14602
.set HEAP_BASE,  0x40a955e0         | page N -> HEAP_BASE + N*6144
.set PAGE_SZ,    6144

    .text
    .global usbaudio_alloc
usbaudio_alloc:
    lea     %sp@(-24),%sp
    moveml  %d1-%d5/%a0,%sp@
    movew   %sr,%d5                 | save SR, mask to IPL7 (as the firmware does)
    movew   #0x2700,%sr
    movel   FLEX_TOP,%d0
    movel   FLEX_BASE,%d1
    subl    %d1,%d0                 | pages available
    cmpil   #NPAGES,%d0
    blt     8f                      | not enough free pages at all
    movel   FLEX_TOP,%d0            | d0 = cursor
    lea     FLEX_STACK,%a0
    moveq   #NPAGES,%d3             | pages still to take
    moveq   #0,%d4                  | previous page index (0 = none yet)
1:  subql   #1,%d0
    mvzw    %a0@(0,%d0:l:2),%d1     | pop one page index
    tstl    %d4
    beqs    2f
    movel   %d4,%d2
    subql   #1,%d2
    cmpl    %d1,%d2
    bnes    7f                      | not contiguous with the last one -> undo
2:  movel   %d1,%d4                 | remember it
    clrw    %a0@(0,%d0:l:2)         | clear the entry, and its shadow
    movel   %d0,%d2
    addil   #FLEX_SHADOW,%d2
    clrw    %a0@(0,%d2:l:2)
    subql   #1,%d3
    bnes    1b
    movel   %d0,FLEX_TOP            | commit the new cursor
    movel   %d4,%d0                 | lowest page index -> base address
    movel   #PAGE_SZ,%d1            | ColdFire mulsl needs a register source
    mulsl   %d1,%d0
    addil   #HEAP_BASE,%d0
    bras    9f
7:  addql   #1,%d0                  | put back the one that broke contiguity
8:  moveq   #0,%d0                  | failure
9:  movew   %d5,%sr
    moveml  %sp@,%d1-%d5/%a0
    lea     %sp@(24),%sp
    rts
