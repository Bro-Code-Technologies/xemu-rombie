/*
 * Rombie's Xbox engine: lockstep's view of the machine. The state's hashes
 * (0x86), and the CPU thread's wait for the I/O it starts.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "exec/icount.h"
#include "hw/core/cpu.h"
#include "hw/pci/pci.h"
#include "system/block-backend.h"
#include "system/cpus.h"
#include "block/accounting.h"
#include "cpu.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/mcpx/apu/apu.h"
#include "ui/rombie-engine.h"

/* XXH64's arithmetic. Two engines only need the same answer, not
 * xxHash's. */
#define H_P1 11400714785074694791ULL
#define H_P2 14029467366897019727ULL
#define H_P3 1609587929392839161ULL
#define H_P4 9650029242287828579ULL
#define H_P5 2870177450012600261ULL

static inline uint64_t h_rotl(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t h_round(uint64_t acc, uint64_t in)
{
    acc += in * H_P2;
    return h_rotl(acc, 31) * H_P1;
}

static inline uint64_t h_merge(uint64_t acc, uint64_t v)
{
    acc ^= h_round(0, v);
    return acc * H_P1 + H_P4;
}

uint64_t rombie_hash64(const void *data, size_t size, uint64_t seed)
{
    const uint8_t *p = data;
    const uint8_t *end = p + size;
    uint64_t h;

    if (size >= 32) {
        const uint8_t *limit = end - 32;
        uint64_t v1 = seed + H_P1 + H_P2, v2 = seed + H_P2, v3 = seed,
                 v4 = seed - H_P1;
        do {
            v1 = h_round(v1, ldq_le_p(p));
            v2 = h_round(v2, ldq_le_p(p + 8));
            v3 = h_round(v3, ldq_le_p(p + 16));
            v4 = h_round(v4, ldq_le_p(p + 24));
            p += 32;
        } while (p <= limit);
        h = h_rotl(v1, 1) + h_rotl(v2, 7) + h_rotl(v3, 12) + h_rotl(v4, 18);
        h = h_merge(h, v1);
        h = h_merge(h, v2);
        h = h_merge(h, v3);
        h = h_merge(h, v4);
    } else {
        h = seed + H_P5;
    }
    h += size;
    for (; p + 8 <= end; p += 8) {
        h ^= h_round(0, ldq_le_p(p));
        h = h_rotl(h, 27) * H_P1 + H_P4;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)ldl_le_p(p) * H_P1;
        h = h_rotl(h, 23) * H_P2 + H_P3;
        p += 4;
    }
    for (; p < end; p++) {
        h ^= *p * H_P5;
        h = h_rotl(h, 11) * H_P1;
    }
    h ^= h >> 33;
    h *= H_P2;
    h ^= h >> 29;
    h *= H_P3;
    h ^= h >> 32;
    return h;
}

static uint64_t hash_segment(const SegmentCache *s, uint64_t h)
{
    const uint64_t v[] = { s->selector, s->base, s->limit, s->flags };
    return rombie_hash64(v, sizeof(v), h);
}

/* The registers a program can see, field by field: CPUX86State also holds
 * host pointers and TCG's lazily computed flags. */
static uint64_t hash_cpu(void)
{
    if (!first_cpu) {
        return 0;
    }
    CPUX86State *env = &X86_CPU(first_cpu)->env;
    uint64_t h = rombie_hash64(env->regs, sizeof(env->regs), 3);
    const uint64_t v[] = {
        env->eip,     cpu_compute_eflags(env), env->cr[0], env->cr[2],
        env->cr[3],   env->cr[4],              env->fpstt, env->fpus,
        env->fpuc,    env->mxcsr,              env->a20_mask,
        first_cpu->interrupt_request,          first_cpu->halted,
    };
    h = rombie_hash64(v, sizeof(v), h);
    for (int i = 0; i < 6; i++) {
        h = hash_segment(&env->segs[i], h);
    }
    h = hash_segment(&env->ldt, h);
    h = hash_segment(&env->tr, h);
    h = hash_segment(&env->gdt, h);
    h = hash_segment(&env->idt, h);
    for (int i = 0; i < 8; i++) {
        const uint64_t fp[] = { env->fpregs[i].d.low, env->fpregs[i].d.high,
                                env->fptags[i], env->xmm_regs[i].ZMM_Q(0),
                                env->xmm_regs[i].ZMM_Q(1) };
        h = rombie_hash64(fp, sizeof(fp), h);
    }
    return h;
}

/* What the disc and the hard disk have done: each drive's reads and
 * writes, in operations and bytes. */
static uint64_t hash_io(void)
{
    uint64_t h = 4;
    for (BlockBackend *blk = blk_next(NULL); blk; blk = blk_next(blk)) {
        BlockAcctStats *stats = blk_get_stats(blk);
        const uint64_t v[] = {
            stats->nr_ops[BLOCK_ACCT_READ],   stats->nr_ops[BLOCK_ACCT_WRITE],
            stats->nr_bytes[BLOCK_ACCT_READ], stats->nr_bytes[BLOCK_ACCT_WRITE],
        };
        h = rombie_hash64(v, sizeof(v), h);
    }
    return h;
}

static uint64_t hash_time(void)
{
    const uint64_t v[] = { icount_get_raw(),
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) };
    return rombie_hash64(v, sizeof(v), 5);
}

void rombie_lockstep_hash(uint64_t out[ROMBIE_HASH_PARTS])
{
    const uint64_t from = get_clock();
    out[0] = nv2a_lockstep_ram_hash();
    out[1] = hash_cpu();
    out[2] = nv2a_lockstep_hash();
    out[3] = mcpx_apu_lockstep_hash();
    out[4] = hash_io();
    out[5] = hash_time();
    rombie_prof_add(ROMBIE_PROF_HASH_NS, get_clock() - from);
}

static uint64_t prof[ROMBIE_PROF_COUNT];

void rombie_prof_add(RombieProf what, uint64_t amount)
{
    qatomic_add(&prof[what], amount);
}

uint64_t rombie_prof_take(RombieProf what)
{
    return qatomic_xchg(&prof[what], 0);
}

typedef struct SpinState {
    uint32_t regs[8];
    uint32_t pc, eflags, fpstt, fpus, fpuc, mxcsr;
    uint64_t fp[8][2];
    uint64_t xmm[8][2];
} SpinState;
static SpinState spin_states[2];

void rombie_spin_capture(void *opaque, int slot)
{
    CPUX86State *env = &X86_CPU((CPUState *)opaque)->env;
    SpinState *s = &spin_states[slot & 1];
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++) {
        s->regs[i] = (uint32_t)env->regs[i];
        s->fp[i][0] = env->fpregs[i].d.low;
        s->fp[i][1] = env->fpregs[i].d.high;
        s->xmm[i][0] = env->xmm_regs[i].ZMM_Q(0);
        s->xmm[i][1] = env->xmm_regs[i].ZMM_Q(1);
    }
    s->pc = (uint32_t)(env->eip + env->segs[R_CS].base);
    s->eflags = cpu_compute_eflags(env);
    s->fpstt = env->fpstt;
    s->fpus = env->fpus;
    s->fpuc = env->fpuc;
    s->mxcsr = env->mxcsr;
}

bool rombie_lockstep_catch_up(void)
{
    return nv2a_lockstep_catch_up();
}

bool rombie_spin_same(void)
{
    return memcmp(&spin_states[0], &spin_states[1], sizeof(SpinState)) == 0;
}

/* Development only (ROMBIE_LOCKSTEP_PROFILE=1): the guest's hot places. */
static bool profiling(void)
{
    static int on = -1;
    if (on < 0) {
        const char *env = getenv("ROMBIE_LOCKSTEP_PROFILE");
        on = env && env[0] == '1';
    }
    return on;
}

#define PC_SLOTS 4096
#define PC_REPORT_EVERY 3000
static struct {
    uint32_t pc;
    uint32_t count;
} pc_slots[PC_SLOTS];
static uint32_t pc_samples;

static int compare_pc_counts(const void *a, const void *b)
{
    const uint32_t ca = ((const typeof(pc_slots[0]) *)a)->count;
    const uint32_t cb = ((const typeof(pc_slots[0]) *)b)->count;
    return ca < cb ? 1 : (ca > cb ? -1 : 0);
}

void rombie_prof_sample_pc(void *opaque)
{
    if (!profiling()) {
        return;
    }
    CPUX86State *env = &X86_CPU((CPUState *)opaque)->env;
    const uint32_t pc = (uint32_t)(env->eip + env->segs[R_CS].base);
    /* Samples land where the instruction budget ran out, so they spread
     * over the guest code as its time does. */
    uint32_t slot = (pc * 2654435761u) % PC_SLOTS;
    for (int probe = 0; probe < 16; probe++) {
        uint32_t s = (slot + probe) % PC_SLOTS;
        if (pc_slots[s].count == 0 || pc_slots[s].pc == pc) {
            pc_slots[s].pc = pc;
            pc_slots[s].count++;
            break;
        }
    }
    if (++pc_samples < PC_REPORT_EVERY) {
        return;
    }
    qsort(pc_slots, PC_SLOTS, sizeof(pc_slots[0]), compare_pc_counts);
    fprintf(stderr, "rombie-lockstep: pcs over %u samples:", pc_samples);
    for (int i = 0; i < 12 && pc_slots[i].count; i++) {
        fprintf(stderr, " %08x:%u", pc_slots[i].pc, pc_slots[i].count);
    }
    fprintf(stderr, "\n");
    /* The code around the hottest place, to tell a spin loop from work. */
    uint32_t lowest = pc_slots[0].pc;
    for (int i = 1; i < 8 && pc_slots[i].count; i++) {
        if (pc_slots[i].pc < lowest && pc_slots[0].pc - pc_slots[i].pc < 64) {
            lowest = pc_slots[i].pc;
        }
    }
    uint8_t code[48];
    if (cpu_memory_rw_debug((CPUState *)opaque, lowest - 16, code, sizeof(code),
                            false) == 0) {
        fprintf(stderr, "rombie-lockstep: code at %08x:", lowest - 16);
        for (size_t i = 0; i < sizeof(code); i++) {
            fprintf(stderr, "%s%02x", i == 16 ? " | " : " ", code[i]);
        }
        fprintf(stderr, " eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n",
                (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_ECX],
                (uint32_t)env->regs[R_EDX], (uint32_t)env->regs[R_EBX],
                (uint32_t)env->regs[R_ESI], (uint32_t)env->regs[R_EDI]);
    }
    memset(pc_slots, 0, sizeof(pc_slots));
    pc_samples = 0;
}

uint64_t rombie_prof_now(void)
{
    return get_clock();
}

uint64_t rombie_prof_insns(void)
{
    return icount_enabled() ? icount_get_raw() : 0;
}

bool rombie_lockstep_stopping(void)
{
    return first_cpu && qatomic_read(&first_cpu->stop);
}

static __thread bool unlocked;

void rombie_lockstep_unlock(void)
{
    unlocked = bql_locked();
    if (unlocked) {
        bql_unlock();
    }
}

void rombie_lockstep_relock(void)
{
    if (unlocked) {
        unlocked = false;
        bql_lock();
    }
}

/* Posted when the block layer's last request completes, or a disc read's
 * data arrives. A wait that polled with g_usleep slept a whole Windows
 * timer tick (up to 15.6 ms) each time. */
static QemuSemaphore io_idle;

/*
 * The disc's reads. The page answers each in tens of milliseconds, and a
 * CPU that stood still for every one ran loading at a third of real time.
 * So each read gets a deadline of guest time, as a drive's seek and
 * transfer would take; the guest runs on meanwhile, and IDE's completion,
 * the only thing that puts the data where the guest can see it, runs at
 * the deadline (disc_deliver), in the order the reads were asked for.
 */
#define DISC_SEEK_NS 50000000LL /* a read that doesn't follow the last */
#define DISC_NEXT_NS 2000000LL  /* one that does */
#define DISC_BYTES_PER_S (16 * 1024 * 1024)

typedef struct DiscRead {
    QSIMPLEQ_ENTRY(DiscRead) next;
    int64_t deadline;
    bool arrived;
    void (*finish)(void *, int);
    void *opaque;
    int ret;
} DiscRead;

static QemuMutex disc_lock;
static QSIMPLEQ_HEAD(, DiscRead) disc_reads =
    QSIMPLEQ_HEAD_INITIALIZER(disc_reads);
static unsigned int disc_in_flight; /* asked for, not arrived */
static QEMUTimer *disc_timer;
static uint64_t disc_next_offset;

static void __attribute__((constructor)) io_idle_init(void)
{
    qemu_sem_init(&io_idle, 0);
    qemu_mutex_init(&disc_lock);
}

static void disc_rearm(void *opaque)
{
    qemu_mutex_lock(&disc_lock);
    DiscRead *head = QSIMPLEQ_FIRST(&disc_reads);
    const int64_t deadline = head ? head->deadline : -1;
    qemu_mutex_unlock(&disc_lock);
    if (deadline >= 0) {
        timer_mod_ns(disc_timer, deadline);
    }
}

/* On the CPU thread at the first read's deadline. */
static void disc_deliver(void *opaque)
{
    const int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (;;) {
        qemu_mutex_lock(&disc_lock);
        DiscRead *r = QSIMPLEQ_FIRST(&disc_reads);
        if (!r || r->deadline > now) {
            qemu_mutex_unlock(&disc_lock);
            break;
        }
        if (!r->arrived) {
            qemu_mutex_unlock(&disc_lock);
            /* The page is late: guest time waits for it. */
            const uint64_t from = get_clock();
            bool arrived = false;
            rombie_lockstep_unlock();
            while (!rombie_lockstep_stopping()) {
                qemu_mutex_lock(&disc_lock);
                arrived = r->arrived;
                qemu_mutex_unlock(&disc_lock);
                if (arrived) {
                    break;
                }
                qemu_sem_timedwait(&io_idle, 10);
            }
            rombie_lockstep_relock();
            rombie_prof_add(ROMBIE_PROF_IO_NS, get_clock() - from);
            if (!arrived) {
                /* Stopping: deliver at the same guest time once it goes
                 * on, armed after the timer list is done. */
                aio_bh_schedule_oneshot(qemu_get_aio_context(), disc_rearm,
                                        NULL);
                return;
            }
            continue;
        }
        QSIMPLEQ_REMOVE_HEAD(&disc_reads, next);
        qemu_mutex_unlock(&disc_lock);
        r->finish(r->opaque, r->ret);
        g_free(r);
    }
    disc_rearm(NULL);
}

void *rombie_lockstep_disc_read(uint64_t offset, uint64_t bytes)
{
    if (!rombie_engine_lockstep_sync(ROMBIE_SYNC_IO)) {
        return NULL;
    }
    if (!disc_timer) {
        disc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, disc_deliver, NULL);
    }
    DiscRead *r = g_new0(DiscRead, 1);
    r->deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (offset == disc_next_offset ? DISC_NEXT_NS : DISC_SEEK_NS) +
                  (int64_t)(bytes * 1000000000ULL / DISC_BYTES_PER_S);
    disc_next_offset = offset + bytes;

    qemu_mutex_lock(&disc_lock);
    DiscRead *last = QSIMPLEQ_LAST(&disc_reads, DiscRead, next);
    if (last && last->deadline > r->deadline) {
        r->deadline = last->deadline; /* the drive serves them in order */
    }
    QSIMPLEQ_INSERT_TAIL(&disc_reads, r, next);
    qatomic_inc(&disc_in_flight);
    const bool first = QSIMPLEQ_FIRST(&disc_reads) == r;
    qemu_mutex_unlock(&disc_lock);
    if (first) {
        timer_mod_ns(disc_timer, r->deadline);
    }
    return r;
}

void rombie_lockstep_disc_arrived(void *read, void (*finish)(void *, int),
                                  void *opaque, int ret)
{
    DiscRead *r = read;
    qemu_mutex_lock(&disc_lock);
    r->finish = finish;
    r->opaque = opaque;
    r->ret = ret;
    r->arrived = true;
    qatomic_dec(&disc_in_flight);
    qemu_mutex_unlock(&disc_lock);
    qemu_sem_post(&io_idle);
}

/* What the CPU waits for after an IDE access: the block layer's requests
 * but the disc reads still out. */
static unsigned int io_waiting(void)
{
    const unsigned int all = blk_lockstep_in_flight();
    const unsigned int disc = qatomic_read(&disc_in_flight);
    return all > disc ? all - disc : 0;
}

void rombie_lockstep_io_idle(void)
{
    if (rombie_engine_lockstep_sync(ROMBIE_SYNC_IO)) {
        qemu_sem_post(&io_idle);
    }
}

/*
 * With the CPU waiting here, guest time stands still, so the I/O completes
 * (its data in memory, its interrupt raised) at the guest time it started,
 * however long the disc's page or the PC's disk takes. The main loop runs
 * the completions meanwhile, so the lock is let go.
 */
void rombie_lockstep_io_settle(void)
{
    if (!rombie_engine_lockstep_sync(ROMBIE_SYNC_IO) ||
        !qemu_in_vcpu_thread() || !io_waiting()) {
        return;
    }
    const uint64_t from = get_clock();
    rombie_lockstep_unlock();
    while (io_waiting() && !rombie_lockstep_stopping()) {
        qemu_sem_timedwait(&io_idle, 10);
    }
    rombie_lockstep_relock();
    rombie_prof_add(ROMBIE_PROF_IO_NS, get_clock() - from);
}
