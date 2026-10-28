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
