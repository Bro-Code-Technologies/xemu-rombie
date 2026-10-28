/*
 * Rombie's Xbox engine: the game's writable media. The per-game hard disk
 * image (with its EEPROM) goes back to the page whenever the game writes it,
 * and save states are xemu's own snapshots inside that image, one per slot,
 * tagged "slot<N>@<engine version>" so a state made on another engine
 * version is never loaded (QEMU's machine state format changes between
 * versions).
 *
 * Everything here runs on the UI thread with the BQL held
 * (rombie_engine_run_pending), as xemu's own snapshot actions do.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/accounting.h"
#include "block/block-global-state.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"

#include "rombie-engine-media.h"
#include "xemu-settings.h"
#include "xemu-snapshots.h"

#define HDD_DEVICE "ide0-hd0"
#define BLOB_MAGIC "RXBX"
#define BLOB_FORMAT 1
#define BLOB_HEADER 20 /* magic, u32 format, u32 eeprom size, u64 hdd size */

static uint64_t last_write_ops;

bool rombie_media_hdd_written(void)
{
    BlockBackend *blk = blk_by_name(HDD_DEVICE);
    if (!blk) {
        return false;
    }
    const uint64_t ops = blk_get_stats(blk)->nr_ops[BLOCK_ACCT_WRITE];
    const bool written = ops != last_write_ops;
    last_write_ops = ops;
    return written;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

bool rombie_media_read_blob(uint8_t **data, size_t *size, char **error)
{
    gchar *hdd = NULL, *eeprom = NULL;
    gsize hdd_size = 0, eeprom_size = 0;
    GError *gerr = NULL;

    /* Nothing in flight and every cache on disk, so the file is the disk. */
    bdrv_drain_all_begin();
    bdrv_flush_all();
    const bool ok =
        g_file_get_contents(g_config.sys.files.hdd_path, &hdd, &hdd_size,
                            &gerr) &&
        g_file_get_contents(g_config.sys.files.eeprom_path, &eeprom,
                            &eeprom_size, &gerr);
    bdrv_drain_all_end();

    if (!ok) {
        *error = g_strdup_printf("Reading the hard disk image failed: %s",
                                 gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(hdd);
        g_free(eeprom);
        return false;
    }

    *size = BLOB_HEADER + eeprom_size + hdd_size;
    uint8_t *blob = g_malloc(*size);
    memcpy(blob, BLOB_MAGIC, 4);
    put_le32(blob + 4, BLOB_FORMAT);
    put_le32(blob + 8, (uint32_t)eeprom_size);
    put_le64(blob + 12, hdd_size);
    memcpy(blob + BLOB_HEADER, eeprom, eeprom_size);
    memcpy(blob + BLOB_HEADER + eeprom_size, hdd, hdd_size);
    g_free(hdd);
    g_free(eeprom);
    *data = blob;
    return true;
}

/* The tag of `slot`'s snapshot, or NULL. The caller frees it. */
static char *find_slot_tag(int slot)
{
    Error *err = NULL;
    QEMUSnapshotInfo *info = NULL;
    const int count = xemu_snapshots_list(&info, NULL, &err);
    if (err) {
        error_free(err);
        return NULL;
    }
    char *prefix = g_strdup_printf("slot%d@", slot);
    char *found = NULL;
    for (int i = 0; i < count && !found; i++) {
        if (g_str_has_prefix(info[i].name, prefix)) {
            found = g_strdup(info[i].name);
        }
    }
    g_free(prefix);
    return found;
}

bool rombie_media_save_state(int slot, const char *engine_version,
                             char **error)
{
    Error *err = NULL;
    /* A slot holds one state: the old one goes, whatever version made it. */
    char *old = find_slot_tag(slot);
    if (old) {
        xemu_snapshots_delete(old, &err);
        xemu_snapshots_mark_dirty();
        g_free(old);
        if (err) {
            *error = g_strdup(error_get_pretty(err));
            error_free(err);
            return false;
        }
    }

    char *tag = g_strdup_printf("slot%d@%s", slot, engine_version);
    xemu_snapshots_save(tag, &err);
    g_free(tag);
    if (err) {
        *error = g_strdup(error_get_pretty(err));
        error_free(err);
        return false;
    }
    return true;
}

bool rombie_media_load_state(int slot, const char *engine_version,
                             char **error)
{
    char *tag = find_slot_tag(slot);
    if (!tag) {
        *error = g_strdup_printf("Slot %d has no save state.", slot);
        return false;
    }
    const char *made_on = strchr(tag, '@') + 1;
    if (strcmp(made_on, engine_version) != 0) {
        *error = g_strdup_printf("version:%s", made_on);
        g_free(tag);
        return false;
    }

    Error *err = NULL;
    xemu_snapshots_load(tag, &err);
    xemu_snapshots_mark_dirty();
    g_free(tag);
    if (err) {
        *error = g_strdup(error_get_pretty(err));
        error_free(err);
        return false;
    }
    return true;
}

bool rombie_media_delete_state(int slot, char **error)
{
    char *tag = find_slot_tag(slot);
    if (!tag) {
        return true;
    }
    Error *err = NULL;
    xemu_snapshots_delete(tag, &err);
    xemu_snapshots_mark_dirty();
    g_free(tag);
    if (err) {
        *error = g_strdup(error_get_pretty(err));
        error_free(err);
        return false;
    }
    return true;
}

char *rombie_media_slots_json(void)
{
    Error *err = NULL;
    QEMUSnapshotInfo *info = NULL;
    const int count = xemu_snapshots_list(&info, NULL, &err);
    if (err) {
        error_free(err);
        return g_strdup("[]");
    }

    GString *json = g_string_new("[");
    bool first = true;
    for (int i = 0; i < count; i++) {
        int slot;
        char version[64];
        if (sscanf(info[i].name, "slot%d@%63s", &slot, version) != 2) {
            continue;
        }
        g_string_append_printf(json,
                               "%s{\"slot\":%d,\"engine\":\"%s\","
                               "\"date\":%" PRIu64 "}",
                               first ? "" : ",", slot, version,
                               (uint64_t)info[i].date_sec * 1000 +
                                   info[i].date_nsec / 1000000);
        first = false;
    }
    g_string_append(json, "]");
    return g_string_free(json, false);
}
