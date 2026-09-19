/*
 * The panel link's frame framing, shared by both ends of the wire.
 *
 * The QEMU panel-uart device (ot-panel-uart.c) parses OUTBOUND frames to find
 * the version query it owns; the frontend (src/panel.c) parses INBOUND frames
 * to build its PanelState. Both need the same class-byte table, and a table
 * that disagreed between the two ends would desynchronise the link in a way
 * that looks like a lost keypress — so it lives here once.
 *
 * Frame lengths are measured off the wire, not documented anywhere upstream.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OT_PANEL_WIRE_H
#define OT_PANEL_WIRE_H

#include <stdint.h>

/*
 * Bytes in the frame introduced by class byte `b`, including `b` itself.
 * 0 means the byte is not a class byte — the reader is out of frame and must
 * resync by discarding it.
 *
 *   0xB5        6   palette write: [0xB5][id hi][id lo][r][g][b]
 *   0x1n       10   display block: [0x1n][column][8 bytes of pixels]
 *   0x2n/0xAn   2   LED-bit group snapshot (0xAn is group 16+n)
 *   0x3n        2   4-bit brightness for one bit id  / encoder delta outbound
 *   0x5n/0x6n   2   reserved classes seen on the wire
 *   0x7n        2   version handshake class
 */
static inline int ot_panel_pktlen(uint8_t b)
{
    if (b == 0xB5) {
        return 6;
    }
    switch (b & 0xF0) {
    case 0x10: return 10;
    case 0x20: case 0xA0: case 0x30:
    case 0x50: case 0x60: case 0x70: return 2;
    default: return 0;
    }
}

#endif
