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
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "block/accounting.h"
#include "block/block-global-state.h"
#include "block/block-io.h"
#include "block/graph-lock.h"
#include "exec/cpu-common.h"
#include "migration/global_state.h"
#include "migration/snapshot.h"
#include "qemu/cutils.h"
#include "qemu/memalign.h"
#include "qemu/timer.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "system/runstate.h"

#include "rombie-engine.h"
#include "rombie-engine-media.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "xemu-settings.h"
#include "xemu-snapshots.h"

#define HDD_DEVICE "ide0-hd0"
#define BLOB_MAGIC "RXBX"
#define BLOB_FORMAT 1
#define BLOB_HEADER 20 /* magic, u32 format, u32 eeprom size, u64 hdd size */
#define HANDOFF_HEADER 8 /* a handoff's state: u64 guest time (ns) */
#define ZERO_GRAIN 4096 /* the disk copy leaves out zeros this finely */

extern int64_t rombie_handoff_clock_ns; /* system/cpu-timers.c */

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

/* The blob from an image file and the EEPROM file. */
static bool pack_blob(const char *hdd_path, uint8_t **data, size_t *size,
                      char **error)
{
    gchar *hdd = NULL, *eeprom = NULL;
    gsize hdd_size = 0, eeprom_size = 0;
    GError *gerr = NULL;
    const bool ok =
        g_file_get_contents(hdd_path, &hdd, &hdd_size, &gerr) &&
        g_file_get_contents(g_config.sys.files.eeprom_path, &eeprom,
                            &eeprom_size, &gerr);
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

bool rombie_media_read_blob(uint8_t **data, size_t *size, char **error)
{
    /* Nothing in flight and every cache on disk, so the file is the disk. */
    bdrv_drain_all_begin();
    bdrv_flush_all();
    const bool ok =
        pack_blob(g_config.sys.files.hdd_path, data, size, error);
    bdrv_drain_all_end();
    return ok;
}

/* Neither BIOS goes in a handoff's state: each PC has its own (the flash,
 * and the flash with the MCPX ROM over its end, hw/xbox/xbox.c). A load with
 * them unmigratable refuses a state that holds them. */
static void set_bios_migratable(bool on)
{
    static const char *const names[] = { "xbox.bios", "xbox.mcpx" };
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        RAMBlock *rb = qemu_ram_block_by_name(names[i]);
        if (rb && on) {
            qemu_ram_set_migratable(rb);
        } else if (rb) {
            qemu_ram_unset_migratable(rb);
        }
    }
}

/* The hard disk as the guest sees it, into a new image with no snapshots:
 * only the clusters that hold data (the player's save states stay out). The
 * machine is stopped, so nothing else touches the disk. */
static bool copy_disk(const char *path, uint64_t *data_bytes, Error **errp)
{
    BlockBackend *src = blk_by_name(HDD_DEVICE);
    if (!src) {
        error_setg(errp, "The game has no hard disk.");
        return false;
    }
    BlockDriverState *bs = blk_bs(src);
    const int64_t size = blk_getlength(src);
    if (size < 0) {
        error_setg_errno(errp, (int)-size, "The hard disk's size is unknown");
        return false;
    }
    bdrv_img_create(path, "qcow2", NULL, NULL, NULL, size, 0, true, errp);
    if (*errp) {
        return false;
    }
    QDict *options = qdict_new();
    qdict_put_str(options, "driver", "qcow2");
    BlockBackend *dst = blk_new_open(path, NULL, options, BDRV_O_RDWR, errp);
    if (!dst) {
        return false;
    }

    const int64_t chunk = 1 << 20;
    uint8_t *buf = qemu_blockalign(bs, chunk);
    bool ok = true;
    for (int64_t offset = 0; ok && offset < size;) {
        int64_t count = 0;
        int ret;
        {
            GRAPH_RDLOCK_GUARD_MAINLOOP();
            ret = bdrv_block_status_above(bs, NULL, offset, size - offset,
                                          &count, NULL, NULL);
        }
        if (ret < 0 || count <= 0) {
            error_setg_errno(errp, ret < 0 ? -ret : EIO,
                             "Reading the hard disk's layout failed");
            ok = false;
            break;
        }
        if ((ret & BDRV_BLOCK_DATA) && !(ret & BDRV_BLOCK_ZERO)) {
            for (int64_t at = offset; ok && at < offset + count; at += chunk) {
                const int64_t n = MIN(chunk, offset + count - at);
                ok = blk_pread(src, at, n, buf, 0) >= 0;
                /* A game's zero-filled files read the same unwritten. */
                for (int64_t part = 0; ok && part < n; part += ZERO_GRAIN) {
                    const int64_t m = MIN(ZERO_GRAIN, n - part);
                    if (!buffer_is_zero(buf + part, m)) {
                        ok = blk_pwrite(dst, at + part, m, buf + part, 0) >= 0;
                        *data_bytes += m;
                    }
                }
            }
            if (!ok) {
                error_setg(errp, "Copying the hard disk failed");
            }
        }
        offset += count;
    }
    if (ok && blk_flush(dst) < 0) {
        error_setg(errp, "Writing the hard disk's copy failed");
        ok = false;
    }
    qemu_vfree(buf);
    blk_unref(dst);
    return ok;
}

static bool read_file(const char *path, uint8_t **data, size_t *size,
                      char **error)
{
    gchar *bytes = NULL;
    gsize length = 0;
    GError *gerr = NULL;
    if (!g_file_get_contents(path, &bytes, &length, &gerr)) {
        *error = g_strdup_printf("Reading %s failed: %s", path,
                                 gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        return false;
    }
    *data = (uint8_t *)bytes;
    *size = length;
    return true;
}

static void set_error(char **error, Error *err, const char *fallback)
{
    *error = g_strdup(err ? error_get_pretty(err) : fallback);
    error_free(err);
}

bool rombie_media_handoff(const char *dir, uint8_t **state, size_t *state_size,
                          uint8_t **game_data, size_t *game_data_size,
                          char **error)
{
    g_autofree char *state_path =
        g_build_filename(dir, "handoff-out-state.bin", NULL);
    g_autofree char *disk_path =
        g_build_filename(dir, "handoff-out-hdd.qcow2", NULL);
    Error *err = NULL;

    /* As save_snapshot stops it: the GPU's surfaces go into memory and the
     * APU's frame lands. */
    global_state_store();
    vm_stop(RUN_STATE_SAVE_VM);
    const int64_t clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    bdrv_drain_all_begin();
    set_bios_migratable(false);
    bool ok = rombie_savevm_file(state_path, &err);
    set_bios_migratable(true);
    bdrv_drain_all_end();
    if (!ok) {
        set_error(error, err, "Saving the netplay state failed.");
        g_remove(state_path);
        return false;
    }
    uint64_t disk_bytes = 0;
    if (!copy_disk(disk_path, &disk_bytes, &err)) {
        set_error(error, err, "Copying the hard disk failed.");
        g_remove(state_path);
        g_remove(disk_path);
        return false;
    }

    fprintf(stderr, "rombie-engine: netplay's disk: %.1f MB of data\n",
            disk_bytes / 1048576.0);
    uint8_t *stream = NULL;
    size_t stream_size = 0;
    ok = read_file(state_path, &stream, &stream_size, error) &&
         pack_blob(disk_path, game_data, game_data_size, error);
    g_remove(state_path);
    g_remove(disk_path);
    if (!ok) {
        g_free(stream);
        return false;
    }
    *state_size = HANDOFF_HEADER + stream_size;
    *state = g_malloc(*state_size);
    put_le64(*state, (uint64_t)clock);
    memcpy(*state + HANDOFF_HEADER, stream, stream_size);
    g_free(stream);
    return true;
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

bool rombie_media_load_handoff(const char *dir, const uint8_t *state,
                               size_t size, int64_t *clock_ns, char **error)
{
    if (size <= HANDOFF_HEADER) {
        *error = g_strdup("The netplay state is empty.");
        return false;
    }
    const int64_t clock = (int64_t)get_le64(state);
    if (clock < 0) {
        *error = g_strdup("The netplay state's guest time isn't valid.");
        return false;
    }
    g_autofree char *path = g_build_filename(dir, "handoff-in-state.bin", NULL);
    GError *gerr = NULL;
    if (!g_file_set_contents(path, (const gchar *)state + HANDOFF_HEADER,
                             (gssize)(size - HANDOFF_HEADER), &gerr)) {
        *error = g_strdup_printf("Writing the netplay state failed: %s",
                                 gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        return false;
    }

    Error *err = NULL;
    rombie_handoff_clock_ns = clock;
    set_bios_migratable(false);
    const bool ok = rombie_loadvm_file(path, &err);
    set_bios_migratable(true);
    rombie_handoff_clock_ns = -1;
    g_remove(path);
    if (!ok) {
        set_error(error, err, "Loading the netplay state failed.");
        return false;
    }
    *clock_ns = clock;
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
    /* In lockstep the save is the host's alone: copying the GPU's surfaces
     * into memory would show its game what the guests' doesn't see. The
     * state then holds memory as the game left it, and a surface the game
     * doesn't draw again after a load shows its old pixels. */
    nv2a_save_keeps_surfaces = rombie_engine_lockstep();
    xemu_snapshots_save(tag, &err);
    nv2a_save_keeps_surfaces = false;
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
