/*
 * Rombie's Xbox engine mode: xemu without a window, driven by one Rombie page
 * over a local WebSocket (the pcsx2-rombie protocol surface). The page
 * supplies the firmware and the disc; the engine sends back frames and audio.
 *
 * How it's started:
 *   (no arguments)       first run: registers the rombie-xbox:// link for
 *                        this Windows user, then exits
 *   --link <url>         Rombie opened rombie-xbox://start?key=<key>; serve
 *                        that page, exit when it's gone
 *   --unregister         removes the link registration
 *   --key <key> [...]    development: serve pages presenting <key>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef ROMBIE_ENGINE_H
#define ROMBIE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when the arguments are a link start (--link), before they are parsed:
 * such a run has no console and an arbitrary working folder, so it logs to
 * its data folder (set up by rombie_engine_early_init) instead. */
bool rombie_engine_is_link_launch(int argc, char **argv);

/* Parse engine arguments (consumed entries are nulled out). Setup and
 * unregister runs do their registry work and exit inside this call. */
void rombie_engine_early_init(int argc, char **argv);

/* True when this run is an engine run (--link or --key): the window is
 * hidden, audio goes to the page, and the boot comes over the socket. */
bool rombie_engine_enabled(void);

/* The engine's private config file, under its own data folder, so an xemu
 * the user installed themselves keeps its settings. Engine mode only. */
const char *rombie_engine_config_path(void);

/* Main thread, after settings load and window creation, before QEMU starts:
 * override the file settings, start the server, and block until a page's
 * boot message arrives. Exits the process when no page comes. */
void rombie_engine_wait_for_boot(void);

/* UI thread, inside the main-loop lock (next to the HUD update): run
 * queued control work (pause, resume, exit) safely. */
void rombie_engine_run_pending(void);

/* UI thread, GL context current: capture the framebuffer texture. */
void rombie_engine_capture(uint32_t tex, bool flip);

/* APU thread: one EP frame of final mixed output, 48 kHz stereo S16. */
void rombie_engine_submit_audio(const int16_t frames[][2], int count);

/* Input polling: player 0-3's pad as the page last sent it, in xemu's
 * ControllerState form (button mask, axes: triggers 0..32767, sticks with
 * +y up). */
void rombie_engine_read_pad(int player, uint16_t *buttons, int16_t axis[6]);

/* How many times real time the console runs (1 unless fast-forwarding). */
double rombie_engine_speed(void);

/* Vblank thread: one guest vblank raised (for the stats line). */
void rombie_engine_on_vblank(void);

/* hw/xbox/xid.c: the guest read port 0-3's pad (for the stats line). */
void rombie_engine_on_pad_poll(int port, uint16_t buttons);

/* hw/xbox/xbox.c: the page-supplied firmware, when the engine is running.
 * Returns false when not in engine mode (load from files as usual). */
bool rombie_engine_get_flash(const uint8_t **data, uint32_t *size);
bool rombie_engine_get_mcpx(const uint8_t **data, uint32_t *size);

/* block/rombie-disc.c: the disc the page serves in chunks. */
uint64_t rombie_engine_disc_size(void);
typedef void (*rombie_disc_read_cb)(void *opaque, bool ok);
void rombie_engine_disc_read(uint64_t offset, uint32_t length, void *dst,
                             rombie_disc_read_cb cb, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
