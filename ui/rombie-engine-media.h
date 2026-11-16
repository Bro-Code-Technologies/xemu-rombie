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

/* True if the guest wrote its hard disk outside its cache partitions since the
 * last call. */
bool rombie_media_hdd_written(void);

/* The game's stored object, little-endian. g_free() the data.
 *   Format 1, an image with save states: "RXBX", u32 1, u32 EEPROM size,
 *     u64 image size, the EEPROM, then the qcow2 image as it is.
 *   Format 2, one without: "RXBX", u32 2, u32 EEPROM size, u64 image size,
 *     u32 disc runs, u32 0, the EEPROM, the runs (as the handoff's state's),
 *     then a fresh qcow2 image without them, made from a copy of the image
 *     file taken with the disk's I/O drained while the game runs on. */
bool rombie_media_read_blob(uint8_t **data, size_t *size, char **error);

/* The same blob, made while the game runs: a copy-before-write filter over
 * the disk keeps it as it stood when this was called (the game's I/O drained
 * only to put the filter in; format 1 copies the image file with it drained),
 * then a coroutine in the main loop and a worker thread make the blob from
 * that. `done` gets it on that worker thread (the data, g_free() it) or an
 * error (g_free() it), or on the main loop if the copy failed. False, and
 * *error, if it couldn't start. One at a time: the scratch files are fixed. */
typedef void (*RombieBlobDone)(uint8_t *data, size_t size, char *error,
                               void *opaque);
bool rombie_media_read_blob_async(RombieBlobDone done, void *opaque,
                                  char **error);

/* Holds until a running save's images are closed (its packing may go on):
 * before a save state, a load, a delete or the handoff, which take every
 * writable disk. */
void rombie_media_settle_blob(void);

/* Before the machine runs: a format 2 blob's runs read from the disc into
 * the disk, checked. */
bool rombie_media_fill_save_runs(const uint8_t *runs, uint32_t count,
                                 char **error);

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
