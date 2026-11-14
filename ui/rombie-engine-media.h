/*
 * Rombie's Xbox engine: the game's writable media (rombie-engine-media.c).
 * UI thread, BQL held.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef ROMBIE_ENGINE_MEDIA_H
#define ROMBIE_ENGINE_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if the guest wrote its hard disk since the last call. */
bool rombie_media_hdd_written(void);

/* The game's stored object: "RXBX", u32 format (1), u32 EEPROM size,
 * u64 image size, the EEPROM, then the qcow2 image (with its save states).
 * Little-endian. g_free() the data. */
bool rombie_media_read_blob(uint8_t **data, size_t *size, char **error);

/* Page session thread: a chunk of the disc came, so its sectors are known
 * (netplay's disc runs). */
void rombie_media_disc_chunk(uint64_t offset, const uint8_t *data,
                             size_t size);

/* Netplay's handoff, on the host: stops the machine where it is (it stays
 * stopped) and makes what every PC boots with, in `dir`'s scratch files:
 *   the state: u64 guest time (ns), u32 run count, u32 0, the disc runs
 *     (u64 disk offset, u32 disc sector, u32 sectors, u64 check: disk that
 *     is the disc's sectors, which every PC reads from its own disc), then
 *     the machine's state stream, with neither BIOS in it (each PC has its
 *     own);
 *   the game data: a blob as above whose image is the disk as it stands
 *     without the runs, in a fresh image without the player's save states.
 * g_free() both. */
bool rombie_media_handoff(const char *dir, uint8_t **state, size_t *state_size,
                          uint8_t **game_data, size_t *game_data_size,
                          char **error);

/* On every PC, the host's own included: the machine, created and not yet
 * run, takes a handoff's state, its disk's runs read from the disc first
 * (where the disk doesn't hold them already). *clock_ns is the guest time
 * it goes on from. Main thread, BQL held. */
bool rombie_media_load_handoff(const char *dir, const uint8_t *state,
                               size_t size, int64_t *clock_ns, char **error);

/* Slot states. *error is g_strdup'd on failure; a load refused because the
 * state was made on another engine version reports "version:<version>". */
bool rombie_media_save_state(int slot, const char *engine_version,
                             char **error);
bool rombie_media_load_state(int slot, const char *engine_version,
                             char **error);
bool rombie_media_delete_state(int slot, char **error);

/* [{"slot":1,"engine":"2026.0.0","date":<unix ms>}, ...]. g_free() it. */
char *rombie_media_slots_json(void);

#ifdef __cplusplus
}
#endif

#endif
