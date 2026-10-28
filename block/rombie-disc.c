/*
 * Rombie disc driver: the game disc, read in chunks from the Rombie page
 * that drives the engine (ui/rombie-engine.h). The page reads the player's
 * own file; nothing is copied to disk. Used as the DVD drive's medium in
 * engine mode ("rombie:"), read-only.
 *
 * A read sends one request over the engine's socket and yields; the reply
 * completes it through a bottom half in this driver's AioContext, which can
 * only run once the coroutine has yielded, so the completion cannot race
 * the request.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "block/block-io.h"
#include "block/block_int.h"

#include "ui/rombie-engine.h"

typedef struct {
    Coroutine *co;
    AioContext *ctx;
    bool done;
    bool ok;
} RombieDiscRead;

static void rombie_disc_read_bh(void *opaque)
{
    RombieDiscRead *read = opaque;
    read->done = true;
    aio_co_wake(read->co);
}

/* Engine session thread (or inline on failure). */
static void rombie_disc_read_done(void *opaque, bool ok)
{
    RombieDiscRead *read = opaque;
    read->ok = ok;
    aio_bh_schedule_oneshot(read->ctx, rombie_disc_read_bh, read);
}

static void rombie_disc_parse_filename(const char *filename, QDict *options,
                                       Error **errp)
{
    if (strcmp(filename, "rombie:")) {
        error_setg(errp, "The only allowed filename for this driver is "
                         "'rombie:'");
    }
}

static int rombie_disc_open(BlockDriverState *bs, QDict *options, int flags,
                            Error **errp)
{
    if (rombie_engine_disc_size() == 0) {
        error_setg(errp, "No Rombie page has supplied a disc");
        return -EINVAL;
    }
    return 0;
}

static int64_t coroutine_fn rombie_disc_co_getlength(BlockDriverState *bs)
{
    return rombie_engine_disc_size();
}

static int coroutine_fn rombie_disc_co_preadv(BlockDriverState *bs,
                                              int64_t offset, int64_t bytes,
                                              QEMUIOVector *qiov,
                                              BdrvRequestFlags flags)
{
    RombieDiscRead read = {
        .co = qemu_coroutine_self(),
        .ctx = bdrv_get_aio_context(bs),
    };

    void *dst;
    void *bounce = NULL;
    if (qiov->niov == 1 && qiov->iov[0].iov_len >= (size_t)bytes) {
        dst = qiov->iov[0].iov_base;
    } else {
        bounce = g_malloc(bytes);
        dst = bounce;
    }

    rombie_engine_disc_read(offset, bytes, dst, rombie_disc_read_done, &read);
    while (!read.done) {
        qemu_coroutine_yield();
    }

    if (read.ok && bounce != NULL) {
        qemu_iovec_from_buf(qiov, 0, bounce, bytes);
    }
    g_free(bounce);
    return read.ok ? 0 : -EIO;
}

static BlockDriver bdrv_rombie_disc = {
    .format_name            = "rombie",
    .protocol_name          = "rombie",
    .instance_size          = 0,

    .bdrv_open              = rombie_disc_open,
    .bdrv_parse_filename    = rombie_disc_parse_filename,
    .bdrv_co_getlength      = rombie_disc_co_getlength,

    .bdrv_co_preadv         = rombie_disc_co_preadv,
};

static void bdrv_rombie_disc_init(void)
{
    bdrv_register(&bdrv_rombie_disc);
}

block_init(bdrv_rombie_disc_init);
