/*
 * Rombie's Xbox engine: System Link. The network card's (nvnet) Ethernet
 * frames go to the page over the engine's WebSocket (0x89), and the page's
 * come back (0x11), so the page can be the switch between consoles. Nothing
 * else is connected: xemu's NAT, UDP and pcap backends are never made in
 * engine mode. The cable is QEMU's link state on the card, which the card
 * reports to the game as a PHY link change, so it plugs in and out while a
 * game runs.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-net.h"
#include "net/net.h"
#include "net/hub.h"
#include "ui/rombie-engine.h"

/* Ethernet's header, and its largest frame with a VLAN tag. */
#define LINK_FRAME_MIN 14
#define LINK_FRAME_MAX 1518
/* Frames from the page waiting for the main loop; a burst past this is
 * dropped, as a switch would. */
#define LINK_QUEUE_MAX 512

static NetClientState *link_nc;
static QEMUBH *link_bh;
static QemuMutex link_mutex;
static GQueue link_in = G_QUEUE_INIT;
static bool link_cable;

static ssize_t rombie_link_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t size)
{
    rombie_engine_link_to_page(buf, size);
    return size;
}

static NetClientInfo rombie_link_info = {
    .type = NET_CLIENT_DRIVER_NONE,
    .size = sizeof(NetClientState),
    .receive = rombie_link_receive,
};

/* Main loop, BQL held. */
static void rombie_link_deliver(void *opaque)
{
    for (;;) {
        qemu_mutex_lock(&link_mutex);
        GBytes *frame = g_queue_pop_head(&link_in);
        qemu_mutex_unlock(&link_mutex);
        if (!frame) {
            break;
        }
        gsize size;
        const uint8_t *data = g_bytes_get_data(frame, &size);
        if (link_cable) {
            qemu_send_packet(link_nc, data, size);
        }
        g_bytes_unref(frame);
    }
}

void rombie_link_init(void)
{
    qemu_mutex_init(&link_mutex);
    /* The card is on hub 0 (the machine's default NIC); this joins it. */
    link_nc = qemu_new_net_client(&rombie_link_info, NULL, "rombie-link",
                                  "rombie-link");
    net_hub_add_port(0, "rombie-link-hubport", link_nc);
    qatomic_set(&link_bh, qemu_bh_new(rombie_link_deliver, NULL));
    rombie_link_set_cable(rombie_engine_link_cable());
}

void rombie_link_set_cable(bool in)
{
    if (!link_nc) {
        return;
    }
    link_cable = in;
    qmp_set_link("nvnet.0", in, NULL);
    if (!in) {
        qemu_mutex_lock(&link_mutex);
        g_queue_clear_full(&link_in, (GDestroyNotify)g_bytes_unref);
        qemu_mutex_unlock(&link_mutex);
    }
}

void rombie_link_from_page(const uint8_t *frame, size_t size)
{
    QEMUBH *bh = qatomic_read(&link_bh);
    if (!bh || size < LINK_FRAME_MIN || size > LINK_FRAME_MAX) {
        return;
    }
    qemu_mutex_lock(&link_mutex);
    const bool queued = g_queue_get_length(&link_in) < LINK_QUEUE_MAX;
    if (queued) {
        g_queue_push_tail(&link_in, g_bytes_new(frame, size));
    }
    qemu_mutex_unlock(&link_mutex);
    if (queued) {
        qemu_bh_schedule(bh);
    }
}
