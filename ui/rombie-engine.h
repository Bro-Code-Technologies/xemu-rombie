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
#include <stddef.h>
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

/* Lockstep (netplay): set by the page's 0x0B before boot, fixed after. The
 * guest's time is its instruction count, and everything it can see happens
 * at a point of that time, so two engines given the same inputs stay
 * identical. */
bool rombie_engine_lockstep(void);

/* The parts of the determinism work, each on in lockstep unless the page
 * turned it off for a development test. */
typedef enum {
    ROMBIE_SYNC_APU = 1, /* APU frames run on the CPU thread, on guest time */
    ROMBIE_SYNC_GPU = 2, /* the CPU waits for PFIFO after every kick */
    ROMBIE_SYNC_IO = 4,  /* the CPU waits for disc and disk I/O it started */
} RombieSync;
bool rombie_engine_lockstep_sync(RombieSync part);

/* QEMU's -icount and -rtc values for lockstep (system/vl.c). */
const char *rombie_engine_icount_option(void);
const char *rombie_engine_rtc_option(void);

/* Lockstep's vblank, on the CPU thread from a guest-time timer: hashes,
 * announces, waits for this vblank's input, keeps real time and applies
 * the input. False when the machine is stopping instead; the caller runs
 * the vblank again at the same guest time. */
bool rombie_engine_lockstep_vblank(void);

/* block/block-backend.c: every BlockBackend's requests in flight; it calls
 * rombie_lockstep_io_idle when the last one completes. */
unsigned int blk_lockstep_in_flight(void);
void rombie_lockstep_io_idle(void);

/* hw/xbox/rombie-lockstep.c */
/* The CPU thread waits here, after an IDE register access, until the I/O it
 * started has completed, so it completes at the guest time it started. The
 * disc's reads are the exception: */
void rombie_lockstep_io_settle(void);
/* A disc read joins a queue (NULL when not in lockstep): its data reaches
 * the guest at a deadline of guest time, the guest running meanwhile as on
 * a real drive, and the CPU waits there only if the page hasn't sent it. */
void *rombie_lockstep_disc_read(uint64_t offset, uint64_t bytes);
/* The read's data is in (any thread); `finish` runs at its deadline. */
void rombie_lockstep_disc_arrived(void *read, void (*finish)(void *, int),
                                  void *opaque, int ret);
/* The state's hashes, in the order 0x86 sends them. */
#define ROMBIE_HASH_PARTS 6
void rombie_lockstep_hash(uint64_t out[ROMBIE_HASH_PARTS]);
uint64_t rombie_hash64(const void *data, size_t size, uint64_t seed);
/* For the CPU thread's waits: whether the machine is being stopped, and the
 * main-loop lock let go and taken again. */
bool rombie_lockstep_stopping(void);
void rombie_lockstep_unlock(void);
void rombie_lockstep_relock(void);

/* Where lockstep's time goes, for the stats: host nanoseconds (or counts)
 * since the stats last took them. */
typedef enum {
    ROMBIE_PROF_EXEC_NS,     /* the CPU thread running guest code */
    ROMBIE_PROF_LOOPS,       /* its trips round the TCG loop */
    ROMBIE_PROF_EMPTY_LOOPS, /* trips with no instructions to run */
    ROMBIE_PROF_GPU_NS,      /* waiting for PFIFO */
    ROMBIE_PROF_APU_NS,      /* APU frames */
    ROMBIE_PROF_IO_NS,       /* waiting for I/O */
    ROMBIE_PROF_HASH_NS,     /* hashing */
    ROMBIE_PROF_IDLE_NS,     /* waiting with every CPU idle */
    ROMBIE_PROF_BQL_NS,      /* taking the main-loop lock */
    ROMBIE_PROF_TIMERS_NS,   /* guest-time timers, warps included */
    ROMBIE_PROF_WARPS,       /* warps over idle time */
    ROMBIE_PROF_VBLANK_NS,   /* lockstep's vblank, waits included */
    ROMBIE_PROF_DISPLAY_NS,  /* its display update (process_vblank) */
    ROMBIE_PROF_SEND_NS,     /* its messages to the page */
    ROMBIE_PROF_SPINS,       /* spins skipped to the next timer */
    ROMBIE_PROF_GPU_SYNCS,   /* PFIFO waits */
    ROMBIE_PROF_GPU_WORK_NS, /* the PFIFO thread's passes the CPU waited for */
    ROMBIE_PROF_GPU_PENDING_NS, /* ... in their pending work (display, ...) */
    ROMBIE_PROF_GPU_REPORTS_NS, /* ... in their reports */
    ROMBIE_PROF_FLIPS,       /* the game's frames (flip stalls), any mode */
    ROMBIE_PROF_PUSHER_NS,   /* the PFIFO thread's pusher, any mode */
    ROMBIE_PROF_COUNT
} RombieProf;
void rombie_prof_add(RombieProf what, uint64_t amount);
/* Development (ROMBIE_LOCKSTEP_PROFILE=1): where the guest was when the CPU
 * thread left guest code, reported to stderr every so many samples, with
 * the code at the hottest place. */
void rombie_prof_sample_pc(void *cpu);
/* Lockstep's spin finder (accel/tcg/tcg-accel-ops-rr.c): the registers a
 * program can see after a probe (slot 0 or 1), and whether two probes left
 * them the same. */
void rombie_spin_capture(void *cpu, int slot);
bool rombie_spin_same(void);
/* Lockstep: the GPU does the work the guest left waiting, with the CPU
 * waiting for it. False if there was none. */
bool rombie_lockstep_catch_up(void);
uint64_t rombie_prof_take(RombieProf what);
uint64_t rombie_prof_now(void);
uint64_t rombie_prof_insns(void);

#ifdef __cplusplus
}
#endif

#endif
