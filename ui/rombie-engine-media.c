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
#include "qemu/bitmap.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/memalign.h"
#include "qemu/thread.h"
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
/* Format 2 adds u32 disc runs and u32 0 to the header, and the runs after the
 * EEPROM: an image whose disc sectors each boot reads from the disc. */
#define BLOB_FORMAT_RUNS 2
#define BLOB_HEADER_RUNS 28
/* A handoff's state: u64 guest time (ns), u32 disc runs, u32 0. */
#define HANDOFF_HEADER 16
#define ZERO_GRAIN 4096 /* the disk copy leaves out zeros this finely */
#define DISC_SECTOR 2048
#define RUN_BYTES 24 /* u64 disk offset, u32 disc sector, u32 sectors, u64 check */
#define DISC_PIECE (8 << 20) /* a run is read from the disc this much at a time */
#define SECTORS_FILE "disc-sectors.bin"
#define SECTORS_MAGIC "RXDS"
#define SECTORS_FORMAT 1
#define SECTORS_HEADER 32 /* magic, u32 format, u64 sectors, u64 disc, u64 entries */
#define IDENTITY_SECTORS 512 /* the disc's first megabyte names it */

extern int64_t rombie_handoff_clock_ns; /* system/cpu-timers.c */

/* The hard disk's cache partitions, X, Y and Z: scratch space a game fills
 * from its disc and rebuilds when it must, as Halo 2 streams its maps through
 * them while it plays. A write there alone doesn't have the game's data sent
 * (it goes with the next send: a write elsewhere, a state change, a pause, the
 * exit), since a send reads the whole disk back, about 700 MB for Halo 2, and
 * in lockstep holds every PC's game while it does. */
#define HDD_CACHE_START 0x80000ULL
#define HDD_CACHE_END 0x8ca80000ULL

static bool hdd_written;

void rombie_media_hdd_write(uint64_t offset, uint64_t bytes)
{
    if (offset < HDD_CACHE_START || offset + bytes > HDD_CACHE_END) {
        qatomic_set(&hdd_written, true);
    }
}

bool rombie_media_hdd_written(void)
{
    return qatomic_xchg(&hdd_written, false);
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

typedef struct {
    uint64_t disk;
    uint64_t sector;
    uint64_t sectors;
    uint64_t check;
} DiscRun;

static void put_runs(uint8_t *out, const GArray *runs)
{
    for (guint i = 0; i < runs->len; i++) {
        const DiscRun *run = &g_array_index(runs, DiscRun, i);
        uint8_t *p = out + (size_t)i * RUN_BYTES;
        stq_le_p(p, run->disk);
        stl_le_p(p + 8, (uint32_t)run->sector);
        stl_le_p(p + 12, (uint32_t)run->sectors);
        stq_le_p(p + 16, run->check);
    }
}

/* The blob from an image file and the EEPROM file: format 2 with `runs`,
 * format 1 without. */
static bool pack_blob(const char *hdd_path, const GArray *runs, uint8_t **data,
                      size_t *size, char **error)
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

    const size_t header = runs ? BLOB_HEADER_RUNS : BLOB_HEADER;
    const size_t runs_size = runs ? (size_t)runs->len * RUN_BYTES : 0;
    *size = header + eeprom_size + runs_size + hdd_size;
    uint8_t *blob = g_malloc(*size);
    memcpy(blob, BLOB_MAGIC, 4);
    put_le32(blob + 4, runs ? BLOB_FORMAT_RUNS : BLOB_FORMAT);
    put_le32(blob + 8, (uint32_t)eeprom_size);
    put_le64(blob + 12, hdd_size);
    if (runs) {
        put_le32(blob + 20, runs->len);
        put_le32(blob + 24, 0);
    }
    memcpy(blob + header, eeprom, eeprom_size);
    if (runs) {
        put_runs(blob + header + eeprom_size, runs);
    }
    memcpy(blob + header + eeprom_size + runs_size, hdd, hdd_size);
    g_free(hdd);
    g_free(eeprom);
    *data = blob;
    return true;
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

/*
 * Netplay's disc runs. A game's disk can hold much of its disc (Halo 2 copies
 * 370 MB of its maps to its cache partitions in its first two minutes), and
 * every guest has the disc, so the handoff's disk leaves out each 4 KB block
 * that is two sectors of the disc and names them instead, in runs. The
 * engine knows a sector by its hash once the page has sent it (the game read
 * it), and keeps those in its data folder, for one disc, for the engines
 * after it: a restarted host's resync, a session whose cache an earlier one
 * filled.
 */
#define HASH_P1 0x9E3779B185EBCA87ULL
#define HASH_P2 0xC2B2AE3D27D4EB4FULL
#define HASH_P3 0x165667B19E3779F9ULL

static GMutex sectors_lock; /* the page's session thread records */
static uint64_t sectors_count;
static uint64_t *sector_hashes;
static unsigned long *sector_known;

typedef struct {
    uint64_t hash;
    uint64_t sector;
} SectorKey;

typedef struct {
    uint64_t count;
    uint64_t *hashes;
    unsigned long *known;
    SectorKey *keys; /* the known sectors, by hash */
    size_t nkeys;
} DiscIndex;

static inline uint64_t hash_round(uint64_t acc, uint64_t in)
{
    acc += in * HASH_P2;
    acc = rol64(acc, 31);
    return acc * HASH_P1;
}

static uint64_t sector_hash(const uint8_t *p)
{
    uint64_t v[4] = { HASH_P1 + HASH_P2, HASH_P2, 0, -HASH_P1 };
    for (int i = 0; i < DISC_SECTOR; i += 32) {
        for (int k = 0; k < 4; k++) {
            v[k] = hash_round(v[k], ldq_le_p(p + i + 8 * k));
        }
    }
    uint64_t h = rol64(v[0], 1) + rol64(v[1], 7) + rol64(v[2], 12) +
                 rol64(v[3], 18);
    h ^= h >> 33;
    h *= HASH_P2;
    h ^= h >> 29;
    h *= HASH_P3;
    h ^= h >> 32;
    return h;
}

/* A run's check: its sectors' hashes in order. */
static uint64_t check_sectors(uint64_t check, const uint8_t *data, size_t size)
{
    for (size_t at = 0; at + DISC_SECTOR <= size; at += DISC_SECTOR) {
        check = hash_round(check, sector_hash(data + at));
    }
    return check;
}

void rombie_media_disc_chunk(uint64_t offset, const uint8_t *data, size_t size)
{
    const uint64_t count = rombie_engine_disc_size() / DISC_SECTOR;
    if (offset % DISC_SECTOR || count == 0) {
        return;
    }
    const size_t n = size / DISC_SECTOR;
    g_autofree uint64_t *hashes = g_new(uint64_t, n);
    for (size_t i = 0; i < n; i++) {
        hashes[i] = sector_hash(data + i * DISC_SECTOR);
    }
    g_mutex_lock(&sectors_lock);
    if (sectors_count != count) {
        g_free(sector_hashes);
        g_free(sector_known);
        sectors_count = count;
        sector_hashes = g_new0(uint64_t, count);
        sector_known = bitmap_new(count);
    }
    const uint64_t first = offset / DISC_SECTOR;
    for (size_t i = 0; i < n && first + i < count; i++) {
        sector_hashes[first + i] = hashes[i];
        set_bit(first + i, sector_known);
    }
    g_mutex_unlock(&sectors_lock);
}

typedef struct {
    QemuEvent done;
    bool ok;
} DiscWait;

static void disc_read_done(void *opaque, bool ok)
{
    DiscWait *wait = opaque;
    wait->ok = ok;
    qemu_event_set(&wait->done);
}

/* From the page, waiting (its session thread answers without the BQL). */
static bool read_disc(uint64_t offset, uint32_t length, void *dst)
{
    DiscWait wait = { .ok = false };
    qemu_event_init(&wait.done, false);
    rombie_engine_disc_read(offset, length, dst, disc_read_done, &wait);
    qemu_event_wait(&wait.done);
    qemu_event_destroy(&wait.done);
    return wait.ok;
}

/* sectors_lock held: the disc's name, 0 if its first megabyte isn't known. */
static uint64_t disc_identity_locked(void)
{
    uint64_t identity = hash_round(HASH_P1, sectors_count);
    for (uint64_t s = 0; s < MIN(sectors_count, IDENTITY_SECTORS); s++) {
        if (!test_bit(s, sector_known)) {
            return 0;
        }
        identity = hash_round(identity, sector_hashes[s]);
    }
    return identity ? identity : 1;
}

/* sectors_lock held: the folder's sectors for this disc join this engine's. */
static void merge_sectors_locked(const char *path, uint64_t identity)
{
    gchar *bytes = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &bytes, &length, NULL)) {
        return;
    }
    const uint8_t *p = (const uint8_t *)bytes;
    if (length >= SECTORS_HEADER && !memcmp(p, SECTORS_MAGIC, 4) &&
        (uint32_t)ldl_le_p(p + 4) == SECTORS_FORMAT &&
        ldq_le_p(p + 8) == sectors_count && ldq_le_p(p + 16) == identity) {
        const uint64_t entries = ldq_le_p(p + 24);
        if (entries <= (length - SECTORS_HEADER) / 16) {
            for (uint64_t i = 0; i < entries; i++) {
                const uint8_t *e = p + SECTORS_HEADER + i * 16;
                const uint64_t s = (uint32_t)ldl_le_p(e);
                if (s < sectors_count && !test_bit(s, sector_known)) {
                    sector_hashes[s] = ldq_le_p(e + 8);
                    set_bit(s, sector_known);
                }
            }
        }
    }
    g_free(bytes);
}

static void save_sectors_locked(const char *path, uint64_t identity)
{
    const uint64_t entries = bitmap_count_one(sector_known, sectors_count);
    const size_t size = SECTORS_HEADER + entries * 16;
    g_autofree uint8_t *out = g_malloc0(size);
    memcpy(out, SECTORS_MAGIC, 4);
    stl_le_p(out + 4, SECTORS_FORMAT);
    stq_le_p(out + 8, sectors_count);
    stq_le_p(out + 16, identity);
    stq_le_p(out + 24, entries);
    uint8_t *e = out + SECTORS_HEADER;
    for (uint64_t s = find_first_bit(sector_known, sectors_count);
         s < sectors_count; s = find_next_bit(sector_known, sectors_count, s + 1)) {
        stl_le_p(e, (uint32_t)s);
        stq_le_p(e + 8, sector_hashes[s]);
        e += 16;
    }
    g_file_set_contents(path, (const gchar *)out, (gssize)size, NULL);
}

static int compare_keys(const void *a, const void *b)
{
    const SectorKey *x = a, *y = b;
    if (x->hash != y->hash) {
        return x->hash < y->hash ? -1 : 1;
    }
    return x->sector < y->sector ? -1 : x->sector > y->sector;
}

/* Every sector known, this engine's and the folder's, which gets them all. */
static void disc_index_open(const char *dir, DiscIndex *index)
{
    memset(index, 0, sizeof(*index));
    if (rombie_engine_disc_size() < DISC_SECTOR) {
        return;
    }
    /* Its chunk comes, and is known, if it wasn't. */
    g_autofree uint8_t *first = g_malloc(DISC_SECTOR);
    read_disc(0, DISC_SECTOR, first);
    g_autofree char *path = g_build_filename(dir, SECTORS_FILE, NULL);
    g_mutex_lock(&sectors_lock);
    const uint64_t identity = sectors_count ? disc_identity_locked() : 0;
    if (identity) {
        merge_sectors_locked(path, identity);
        save_sectors_locked(path, identity);
        index->count = sectors_count;
        index->hashes = g_memdup2(sector_hashes, sectors_count * 8);
        index->known = g_memdup2(sector_known,
                                 BITS_TO_LONGS(sectors_count) * sizeof(long));
    }
    g_mutex_unlock(&sectors_lock);
    if (!index->count) {
        return;
    }
    index->nkeys = bitmap_count_one(index->known, index->count);
    index->keys = g_new(SectorKey, index->nkeys);
    size_t k = 0;
    for (uint64_t s = find_first_bit(index->known, index->count);
         s < index->count; s = find_next_bit(index->known, index->count, s + 1)) {
        index->keys[k++] = (SectorKey){ index->hashes[s], s };
    }
    qsort(index->keys, index->nkeys, sizeof(SectorKey), compare_keys);
}

static void disc_index_close(DiscIndex *index)
{
    g_free(index->hashes);
    g_free(index->known);
    g_free(index->keys);
}

/* The sector where two known sectors hash to a and b, trying `hint` first. */
static bool disc_index_find(const DiscIndex *index, uint64_t a, uint64_t b,
                            uint64_t hint, uint64_t *sector)
{
    if (hint + 1 < index->count && test_bit(hint, index->known) &&
        test_bit(hint + 1, index->known) && index->hashes[hint] == a &&
        index->hashes[hint + 1] == b) {
        *sector = hint;
        return true;
    }
    size_t lo = 0, hi = index->nkeys;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (index->keys[mid].hash < a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    /* A sector the disc repeats (a fill pattern) is anywhere it is. */
    for (int tries = 0; lo < index->nkeys && index->keys[lo].hash == a &&
                        tries < 64; lo++, tries++) {
        const uint64_t s = index->keys[lo].sector;
        if (s + 1 < index->count && test_bit(s + 1, index->known) &&
            index->hashes[s + 1] == b) {
            *sector = s;
            return true;
        }
    }
    return false;
}

/* A 4 KB block of the disk that is two sectors of the disc joins `runs`. */
static bool add_disc_run(const DiscIndex *index, GArray *runs, uint64_t disk,
                         const uint8_t *block, uint64_t *hint)
{
    if (!index->nkeys) {
        return false;
    }
    const uint64_t a = sector_hash(block);
    const uint64_t b = sector_hash(block + DISC_SECTOR);
    uint64_t sector;
    if (!disc_index_find(index, a, b, *hint, &sector)) {
        return false;
    }
    DiscRun *last =
        runs->len ? &g_array_index(runs, DiscRun, runs->len - 1) : NULL;
    if (last && last->disk + last->sectors * DISC_SECTOR == disk &&
        last->sector + last->sectors == sector) {
        last->sectors += 2;
        last->check = hash_round(hash_round(last->check, a), b);
    } else {
        const DiscRun run = { disk, sector, 2,
                              hash_round(hash_round(HASH_P1, a), b) };
        g_array_append_val(runs, run);
    }
    *hint = sector + 2;
    return true;
}

/* Before the machine runs (a save's runs) or the state loads (netplay's, on
 * every PC): each run's disk comes from this PC's own disc, unless the disk
 * already holds it (the host's own). */
static bool fill_from_disc(const uint8_t *runs, uint32_t count, bool save,
                           char **error)
{
    if (count == 0) {
        return true;
    }
    BlockBackend *hdd = blk_by_name(HDD_DEVICE);
    const int64_t hdd_size = hdd ? blk_getlength(hdd) : -1;
    if (hdd_size < 0) {
        *error = g_strdup("The game has no hard disk.");
        return false;
    }
    const uint64_t disc_sectors = rombie_engine_disc_size() / DISC_SECTOR;
    uint8_t *buf = qemu_blockalign(blk_bs(hdd), DISC_PIECE);
    uint64_t filled = 0, kept = 0;
    const int64_t from = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *r = runs + (size_t)i * RUN_BYTES;
        const uint64_t disk = ldq_le_p(r);
        const uint64_t sector = (uint32_t)ldl_le_p(r + 8);
        const uint64_t sectors = (uint32_t)ldl_le_p(r + 12);
        const uint64_t check = ldq_le_p(r + 16);
        const uint64_t bytes = sectors * DISC_SECTOR;
        if (sectors == 0 || sector + sectors > disc_sectors ||
            disk % DISC_SECTOR || disk + bytes > (uint64_t)hdd_size) {
            *error = g_strdup(save ? "The game's saved data names parts of "
                                     "the disc that this disc doesn't have."
                                   : "The netplay state's disc runs aren't "
                                     "valid.");
            ok = false;
            break;
        }
        uint64_t have = HASH_P1;
        for (uint64_t at = 0; ok && at < bytes; at += DISC_PIECE) {
            const uint64_t n = MIN(DISC_PIECE, bytes - at);
            ok = blk_pread(hdd, disk + at, n, buf, 0) >= 0;
            have = check_sectors(have, buf, n);
        }
        if (ok && have == check) {
            kept += bytes;
            continue;
        }
        uint64_t got = HASH_P1;
        for (uint64_t at = 0; ok && at < bytes; at += DISC_PIECE) {
            const uint64_t n = MIN(DISC_PIECE, bytes - at);
            ok = read_disc(sector * DISC_SECTOR + at, (uint32_t)n, buf) &&
                 blk_pwrite(hdd, disk + at, n, buf, 0) >= 0;
            got = check_sectors(got, buf, n);
        }
        if (!ok) {
            *error = g_strdup("Reading the disc for the game's hard disk "
                              "failed.");
        } else if (got != check) {
            *error = g_strdup(save ? "This disc isn't the one the game's data "
                                     "was saved with."
                                   : "This disc isn't the same as the host's.");
            ok = false;
        }
        filled += bytes;
    }
    qemu_vfree(buf);
    if (ok && blk_flush(hdd) < 0) {
        *error = g_strdup("Writing the game's hard disk failed.");
        ok = false;
    }
    if (ok) {
        fprintf(stderr,
                "rombie-engine: %s: %.1f MB from the disc, "
                "%.1f MB already there (%u runs), in %" PRId64 " ms\n",
                save ? "the saved disk" : "netplay's disk", filled / 1048576.0,
                kept / 1048576.0, count,
                qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - from);
    }
    return ok;
}

bool rombie_media_fill_save_runs(const uint8_t *runs, uint32_t count,
                                 char **error)
{
    return fill_from_disc(runs, count, true, error);
}

/* A hard disk as the guest sees it (`src`: the machine's, stopped, or a copy
 * of its file), into a new image with no snapshots: only the clusters that
 * hold data (the player's save states stay out), and of those only what isn't
 * the disc's (`runs` names that). */
static bool copy_disk(BlockBackend *src, const char *path,
                      const DiscIndex *index, GArray *runs,
                      uint64_t *data_bytes, uint64_t *disc_bytes, Error **errp)
{
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
    uint64_t hint = 0;
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
                    if (buffer_is_zero(buf + part, m)) {
                        continue;
                    }
                    *data_bytes += m;
                    if (m == 2 * DISC_SECTOR &&
                        add_disc_run(index, runs, at + part, buf + part,
                                     &hint)) {
                        *disc_bytes += m;
                    } else {
                        ok = blk_pwrite(dst, at + part, m, buf + part, 0) >= 0;
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

static bool copy_file(const char *from, const char *to, char **error)
{
    FILE *in = g_fopen(from, "rb");
    FILE *out = in ? g_fopen(to, "wb") : NULL;
    bool ok = in && out;
    const size_t piece = 8 << 20;
    uint8_t *buf = ok ? g_malloc(piece) : NULL;
    while (ok) {
        const size_t n = fread(buf, 1, piece, in);
        if (n == 0) {
            ok = !ferror(in);
            break;
        }
        ok = fwrite(buf, 1, n, out) == n;
    }
    g_free(buf);
    if (out && fclose(out) != 0) {
        ok = false;
    }
    if (in) {
        fclose(in);
    }
    if (!ok) {
        *error = g_strdup("Copying the hard disk image failed.");
        g_remove(to);
    }
    return ok;
}

/* With no save states in the image, what a save keeps is the disk as the
 * guest sees it in a fresh image, the disc's own sectors left out as runs:
 * a game's cache partitions (Halo 2's 700 MB) are nearly all copies of its
 * disc (the user's call, Rombie BRAIN-38). The image file is copied with the
 * game's disk I/O drained, as format 1 reads it, and the blob is made from
 * the copy while the game runs on: stopping the machine for it, the first
 * way, once left a game that never ran again. */
static bool read_compact_blob(uint8_t **data, size_t *size, char **error)
{
    g_autofree char *dir = g_path_get_dirname(g_config.sys.files.hdd_path);
    g_autofree char *copy_path =
        g_build_filename(dir, "save-in-hdd.qcow2", NULL);
    g_autofree char *disk_path =
        g_build_filename(dir, "save-out-hdd.qcow2", NULL);
    const int64_t from = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    bdrv_drain_all_begin();
    bdrv_flush_all();
    bool ok = copy_file(g_config.sys.files.hdd_path, copy_path, error);
    bdrv_drain_all_end();
    if (!ok) {
        return false;
    }

    Error *err = NULL;
    QDict *options = qdict_new();
    qdict_put_str(options, "driver", "qcow2");
    BlockBackend *src = blk_new_open(copy_path, NULL, options, 0, &err);
    if (!src) {
        set_error(error, err, "Opening the hard disk's copy failed.");
        g_remove(copy_path);
        return false;
    }
    DiscIndex index;
    disc_index_open(dir, &index);
    GArray *runs = g_array_new(false, false, sizeof(DiscRun));
    uint64_t disk_bytes = 0, disc_bytes = 0;
    ok = copy_disk(src, disk_path, &index, runs, &disk_bytes, &disc_bytes,
                   &err);
    disc_index_close(&index);
    blk_unref(src);
    g_remove(copy_path);
    if (!ok) {
        set_error(error, err, "Copying the hard disk failed.");
    } else {
        ok = pack_blob(disk_path, runs, data, size, error);
        fprintf(stderr,
                "rombie-engine: game data: %.1f MB of disk, %.1f MB of it "
                "the disc's (%u runs), %zu bytes, in %" PRId64 " ms\n",
                disk_bytes / 1048576.0, disc_bytes / 1048576.0, runs->len,
                ok ? *size : (size_t)0,
                qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - from);
    }
    g_remove(disk_path);
    g_array_free(runs, true);
    return ok;
}

bool rombie_media_read_blob(uint8_t **data, size_t *size, char **error)
{
    Error *err = NULL;
    QEMUSnapshotInfo *info = NULL;
    const int states = xemu_snapshots_list(&info, NULL, &err);
    if (!err && states == 0) {
        return read_compact_blob(data, size, error);
    }
    error_free(err);
    /* A save state holds the caches as they were, and an image can't be
     * copied with its snapshots: it goes as it is. Nothing in flight and
     * every cache on disk, so the file is the disk. */
    bdrv_drain_all_begin();
    bdrv_flush_all();
    const bool ok =
        pack_blob(g_config.sys.files.hdd_path, NULL, data, size, error);
    bdrv_drain_all_end();
    return ok;
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
    const int64_t index_from = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    DiscIndex index;
    disc_index_open(dir, &index);
    const int64_t index_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - index_from;
    GArray *runs = g_array_new(false, false, sizeof(DiscRun));
    uint64_t disk_bytes = 0, disc_bytes = 0;
    ok = copy_disk(blk_by_name(HDD_DEVICE), disk_path, &index, runs,
                   &disk_bytes, &disc_bytes, &err);
    fprintf(stderr,
            "rombie-engine: netplay's disk: %.1f MB of data, %.1f MB of it "
            "the disc's (%u runs; %zu of %" PRIu64 " disc sectors known, "
            "in %" PRId64 " ms)\n",
            disk_bytes / 1048576.0, disc_bytes / 1048576.0, runs->len,
            index.nkeys, index.count, index_ms);
    disc_index_close(&index);
    if (!ok) {
        set_error(error, err, "Copying the hard disk failed.");
        g_array_free(runs, true);
        g_remove(state_path);
        g_remove(disk_path);
        return false;
    }

    uint8_t *stream = NULL;
    size_t stream_size = 0;
    ok = read_file(state_path, &stream, &stream_size, error) &&
         pack_blob(disk_path, NULL, game_data, game_data_size, error);
    g_remove(state_path);
    g_remove(disk_path);
    if (!ok) {
        g_free(stream);
        g_array_free(runs, true);
        return false;
    }
    const size_t runs_size = (size_t)runs->len * RUN_BYTES;
    *state_size = HANDOFF_HEADER + runs_size + stream_size;
    *state = g_malloc(*state_size);
    stq_le_p(*state, (uint64_t)clock);
    stl_le_p(*state + 8, runs->len);
    stl_le_p(*state + 12, 0);
    put_runs(*state + HANDOFF_HEADER, runs);
    memcpy(*state + HANDOFF_HEADER + runs_size, stream, stream_size);
    g_free(stream);
    g_array_free(runs, true);
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
    const uint32_t run_count = (uint32_t)ldl_le_p(state + 8);
    const size_t runs_size = (size_t)run_count * RUN_BYTES;
    if (runs_size >= size - HANDOFF_HEADER) {
        *error = g_strdup("The netplay state is cut short.");
        return false;
    }
    if (!fill_from_disc(state + HANDOFF_HEADER, run_count, false, error)) {
        return false;
    }
    g_autofree char *path = g_build_filename(dir, "handoff-in-state.bin", NULL);
    GError *gerr = NULL;
    if (!g_file_set_contents(path,
                             (const gchar *)state + HANDOFF_HEADER + runs_size,
                             (gssize)(size - HANDOFF_HEADER - runs_size),
                             &gerr)) {
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
    rombie_lockstep_trace_time(0, "save begins");
    xemu_snapshots_save(tag, &err);
    rombie_lockstep_trace_time(0, "save ends");
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
