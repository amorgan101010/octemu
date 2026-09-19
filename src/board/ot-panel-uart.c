/*
 * UART1 — the Octatrack's panel link (312500 baud), with the version
 * handshake answered on the VIRTUAL clock.
 *
 * Derived from QEMU's hw/char/mcf_uart.c:
 *   Copyright (c) 2007 CodeSourcery.
 * and licensed, like it, under the GNU GPL version 2 or (at your option) any
 * later version. The register model came from that file function for function;
 * two things are ours:
 *
 *   1. A deeper RX staging queue in front of the 4-byte FIFO, so a multi-byte
 *      reply can be injected atomically and fed to the guest as it drains.
 *   2. The version handshake. The guest sends a two-byte class-0x70 query and
 *      then POLLS for a five-byte reply in a narrow window; bytes that miss
 *      that window fall into the deferred parser, which boots in a broken
 *      body-mode state and CLEARS instead of resyncing (0x40092350) — wedged
 *      for the rest of the run, every later key frame swallowed. Two readers
 *      want two answers (measured): the five bytes NOW, and the ten-byte
 *      class-0x70 frame ~250 ms LATER for the deferred parser. Ten bytes at
 *      once wedges it just as surely.
 *
 * Everything else on the link — display blocks, LED frames, key/encoder/
 * crossfader frames — passes through to the chardev untouched, because the
 * frontend is still the panel MCU. The query is forwarded too; a frontend that
 * also answers would be the wedge this device exists to remove, so the v2
 * frontend does not.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "qom/object.h"
#include "ot-qemu.h"
#include "ot-panel-wire.h"

#define FIFO_DEPTH 4
#define QUEUE_DEPTH 256

/* UART Status Register bits. */
#define U_RxRDY  0x01
#define U_FFULL  0x02
#define U_TxRDY  0x04
#define U_TxEMP  0x08

/* Interrupt flags. */
#define U_TxINT  0x01
#define U_RxINT  0x02
#define U_DBINT  0x04

#define U_RxIRQ  0x40                      /* UMR1: interrupt on FFULL */

/* The deferred ten-byte frame, in VIRTUAL time. */
#define OT_VERSION_REPLY_DELAY_NS 250000000ULL

#define TYPE_OT_PANEL_UART "octatrack-panel-uart"
OBJECT_DECLARE_SIMPLE_TYPE(OTPanelUart, OT_PANEL_UART)

struct OTPanelUart {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharFrontend chr;
    bool mk1;

    uint8_t mr[2];
    uint8_t sr, isr, imr, tb;
    int current_mr;
    uint8_t fifo[FIFO_DEPTH];
    int fifo_len;
    int tx_enabled, rx_enabled;

    /* RX staging: the guest-visible FIFO is four bytes deep, so a five- or
     * ten-byte reply cannot be pushed into it directly. */
    uint8_t q[QUEUE_DEPTH];
    int q_head, q_len;

    /* Outbound framing, so a 0x70 is only read as a class byte when it is
     * one. Frame lengths are the panel wire's, measured. */
    uint8_t pkt[2];
    int have, want;
    bool consuming;                    /* this frame is ours, not the panel's */

    QEMUTimer *reply_timer;
    uint8_t ver_query;
};

/* Key delivery is the largest open defect in the repo and every theory about
 * it has been about WHERE the tap is lost. These separate the candidates in
 * one run: `in` is bytes the frontend handed the device, `drop` is bytes the
 * staging queue could not hold, `out` is bytes the guest actually read. A
 * failing walk with in==out and no drops means the tap REACHED the guest and
 * the guest ignored it — which is a different bug from the device losing it. */
static uint64_t ot_panel_in, ot_panel_out, ot_panel_drop, ot_panel_keys;

void ot_panel_stats(char *buf, size_t len)
{
    snprintf(buf, len, "panel in=%llu out=%llu drop=%llu keyframes=%llu",
             (unsigned long long)ot_panel_in, (unsigned long long)ot_panel_out,
             (unsigned long long)ot_panel_drop,
             (unsigned long long)ot_panel_keys);
}

static void ot_panel_update(OTPanelUart *s)
{
    s->isr &= ~(U_TxINT | U_RxINT);
    if (s->sr & U_TxRDY) {
        s->isr |= U_TxINT;
    }
    if ((s->sr & ((s->mr[0] & U_RxIRQ) ? U_FFULL : U_RxRDY)) != 0) {
        s->isr |= U_RxINT;
    }
    qemu_set_irq(s->irq, (s->isr & s->imr) != 0);
}

/* Move staged bytes into the guest-visible FIFO while there is room. */
static void ot_panel_pump(OTPanelUart *s)
{
    while (s->rx_enabled && s->q_len > 0 && s->fifo_len < FIFO_DEPTH) {
        s->fifo[s->fifo_len++] = s->q[s->q_head];
        s->q_head = (s->q_head + 1) % QUEUE_DEPTH;
        s->q_len--;
        s->sr |= U_RxRDY;
        if (s->fifo_len == FIFO_DEPTH) {
            s->sr |= U_FFULL;
        }
    }
    ot_panel_update(s);
}

static void ot_panel_stage(OTPanelUart *s, const uint8_t *buf, int n)
{
    int i;

    for (i = 0; i < n && s->q_len < QUEUE_DEPTH; i++) {
        s->q[(s->q_head + s->q_len) % QUEUE_DEPTH] = buf[i];
        s->q_len++;
        if ((buf[i] & 0xF0) == 0x20) {
            ot_panel_keys++;               /* a key-group snapshot's class */
        }
    }
    ot_panel_in += i;
    ot_panel_drop += n - i;
    ot_panel_pump(s);
}

static void ot_panel_reply_later(void *opaque)
{
    OTPanelUart *s = opaque;
    const uint8_t f[10] = { (uint8_t)(0x70 | (s->ver_query & 0x0F)),
                            0x05, 0x08, 0x01, 0x02, 0, 0, 0, 0, 0 };

    ot_panel_stage(s, f, sizeof(f));
}

/* One complete outbound frame. Only the version query is ours. */
static void ot_panel_frame(OTPanelUart *s)
{
    static const uint8_t five[5] = { 0x70, 0x05, 0x08, 0x01, 0x02 };

    if (s->pkt[0] != 0x70 || s->mk1) {
        return;                            /* MK1 boots with no handshake */
    }
    ot_panel_stage(s, five, sizeof(five));
    s->ver_query = s->pkt[1];
    timer_mod_ns(s->reply_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                 + OT_VERSION_REPLY_DELAY_NS);
}

/* Returns true if the byte should also go out to the frontend. The version
 * query is CONSUMED here: this device owns that exchange end to end, and a
 * frontend that saw the query would answer it too — a second, host-timed reply
 * arriving late is precisely the wedge this device exists to remove. */
static bool ot_panel_tx_byte(OTPanelUart *s, uint8_t b)
{
    if (s->have == 0) {
        s->want = ot_panel_pktlen(b);
        if (!s->want) {
            return true;                   /* unframed: pass it through */
        }
        s->consuming = (b == 0x70) && !s->mk1;
    }
    if (s->have < (int)sizeof(s->pkt)) {
        s->pkt[s->have] = b;
    }
    if (++s->have >= s->want) {
        ot_panel_frame(s);
        s->have = 0;
        if (s->consuming) {
            s->consuming = false;
            return false;
        }
    }
    return !s->consuming;
}

static uint64_t ot_panel_read(void *opaque, hwaddr addr, unsigned size)
{
    OTPanelUart *s = opaque;

    switch (addr & 0x3f) {
    case 0x00:
        return s->mr[s->current_mr];
    case 0x04:
        return s->sr;
    case 0x0c: {
        uint8_t val;

        if (s->fifo_len == 0) {
            return 0;
        }
        val = s->fifo[0];
        ot_panel_out++;
        s->fifo_len--;
        for (int i = 0; i < s->fifo_len; i++) {
            s->fifo[i] = s->fifo[i + 1];
        }
        s->sr &= ~U_FFULL;
        if (s->fifo_len == 0) {
            s->sr &= ~U_RxRDY;
        }
        ot_panel_pump(s);
        qemu_chr_fe_accept_input(&s->chr);
        return val;
    }
    case 0x14:
        return s->isr;
    default:
        return 0;
    }
}

static void ot_panel_do_tx(OTPanelUart *s)
{
    if (s->tx_enabled && (s->sr & U_TxEMP) == 0) {
        if (ot_panel_tx_byte(s, s->tb)) {
            qemu_chr_fe_write_all(&s->chr, &s->tb, 1);
        }
        s->sr |= U_TxEMP;
    }
    if (s->tx_enabled) {
        s->sr |= U_TxRDY;
    } else {
        s->sr &= ~U_TxRDY;
    }
}

static void ot_panel_command(OTPanelUart *s, uint8_t cmd)
{
    switch ((cmd >> 4) & 7) {
    case 1: s->current_mr = 0; break;      /* reset mode register pointer */
    case 2:                                /* reset receiver */
        s->rx_enabled = 0;
        s->fifo_len = 0;
        s->sr &= ~(U_RxRDY | U_FFULL);
        break;
    case 3:                                /* reset transmitter */
        s->tx_enabled = 0;
        s->sr |= U_TxEMP;
        s->sr &= ~U_TxRDY;
        break;
    case 5: s->isr &= ~U_DBINT; break;     /* reset break-change interrupt */
    default: break;
    }
    switch ((cmd >> 2) & 3) {
    case 1: s->tx_enabled = 1; ot_panel_do_tx(s); break;
    case 2: s->tx_enabled = 0; ot_panel_do_tx(s); break;
    default: break;
    }
    switch (cmd & 3) {
    case 1: s->rx_enabled = 1; ot_panel_pump(s); break;
    case 2: s->rx_enabled = 0; break;
    default: break;
    }
}

static void ot_panel_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    OTPanelUart *s = opaque;

    switch (addr & 0x3f) {
    case 0x00:
        s->mr[s->current_mr] = val;
        s->current_mr = 1;
        break;
    case 0x08:
        ot_panel_command(s, val);
        break;
    case 0x0c:                             /* transmit buffer */
        s->sr &= ~U_TxEMP;
        s->tb = val;
        ot_panel_do_tx(s);
        break;
    case 0x14:
        s->imr = val;
        break;
    default:
        break;                             /* CSR and ACR are ignored */
    }
    ot_panel_update(s);
}

static const MemoryRegionOps ot_panel_ops = {
    .read = ot_panel_read,
    .write = ot_panel_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int ot_panel_can_receive(void *opaque)
{
    OTPanelUart *s = opaque;

    return QUEUE_DEPTH - s->q_len;
}

static void ot_panel_receive(void *opaque, const uint8_t *buf, int size)
{
    ot_panel_stage(opaque, buf, size);
}

static void ot_panel_event(void *opaque, QEMUChrEvent event)
{
    OTPanelUart *s = opaque;
    const uint8_t zero = 0;

    if (event == CHR_EVENT_BREAK) {
        s->isr |= U_DBINT;
        ot_panel_stage(s, &zero, 1);
    }
}

static void ot_panel_reset(DeviceState *dev)
{
    OTPanelUart *s = OT_PANEL_UART(dev);

    s->fifo_len = 0;
    s->q_head = s->q_len = 0;
    s->have = s->want = 0;
    s->consuming = false;
    s->mr[0] = s->mr[1] = 0;
    s->sr = U_TxEMP;
    s->tx_enabled = s->rx_enabled = 0;
    s->isr = s->imr = 0;
}

static void ot_panel_instance_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    OTPanelUart *s = OT_PANEL_UART(dev);

    memory_region_init_io(&s->iomem, obj, &ot_panel_ops, s, "octatrack.panel",
                          0x40);
    sysbus_init_mmio(dev, &s->iomem);
    sysbus_init_irq(dev, &s->irq);
}

static void ot_panel_realize(DeviceState *dev, Error **errp)
{
    OTPanelUart *s = OT_PANEL_UART(dev);

    s->reply_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ot_panel_reply_later, s);
    qemu_chr_fe_set_handlers(&s->chr, ot_panel_can_receive, ot_panel_receive,
                             ot_panel_event, NULL, s, NULL, true);
}

static const Property ot_panel_properties[] = {
    DEFINE_PROP_CHR("chardev", OTPanelUart, chr),
    DEFINE_PROP_BOOL("mk1", OTPanelUart, mk1, false),
};

static void ot_panel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ot_panel_realize;
    device_class_set_legacy_reset(dc, ot_panel_reset);
    device_class_set_props(dc, ot_panel_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo ot_panel_info = {
    .name          = TYPE_OT_PANEL_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OTPanelUart),
    .instance_init = ot_panel_instance_init,
    .class_init    = ot_panel_class_init,
};

static void ot_panel_register_types(void)
{
    type_register_static(&ot_panel_info);
}

type_init(ot_panel_register_types)

void ot_panel_uart_create(MemoryRegion *sysmem, uint64_t base,
                           struct IRQState *irq, struct Chardev *chr, int mk1)
{
    DeviceState *dev = qdev_new(TYPE_OT_PANEL_UART);

    if (chr) {
        qdev_prop_set_chr(dev, "chardev", (Chardev *)chr);
    }
    qdev_prop_set_bit(dev, "mk1", mk1 != 0);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, (qemu_irq)irq);
    memory_region_add_subregion(sysmem, base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
}
