# Rombie Xbox engine — notes (fork, build, spike, engine mode)

Fork of [xemu](https://github.com/xemu-project/xemu) for Rombie's Xbox
local engine, following the pcsx2-rombie conventions: work happens on
the `rombie` branch; releases tag as `rombie-xbox-engine-<version>`
(year-based). Upstream base: `dc10b4cea9` (v0.8.139).

## Building (Windows target)

xemu's documented Windows build is a cross-compile from Linux — there
is no native MSVC/MSYS2 path. From the directory containing the clone:

```
docker run --rm -v <clone>:/xemu -w /xemu -e CCACHE_DIR=/xemu/ccache \
    ghcr.io/xemu-project/xemu-win64-toolchain:latest \
    ./build.sh -p win64-cross
```

Output lands at `dist/xemu.exe` (statically linked, ~140 MB).

Gotchas hit on 2026-10-27:

- Clone with `core.autocrlf=false` so the container's shell scripts
  keep LF endings.
- Two of QEMU's deep test-data submodules (edk2 → pyca-cryptography)
  have filenames past Windows' 260-char limit; finish the checkout with
  `git -c core.longpaths=true submodule update --init --recursive`.
- The toolchain container has no `curl`, so meson's `dsp56300`
  subproject can't fetch its prebuilt library. Pre-place
  `dsp56300-0.1.3-x86_64-pc-windows-gnu.tar.gz` (from
  mborgerson/dsp56300 releases) in `subprojects/dsp56300/` and the
  download step is skipped.
- Build time on the 5950X: ~27 min wall / ~95 min sys through a Docker
  bind mount on `C:\` — the mount's file I/O is the bottleneck. For
  iterative work, keep a working copy on WSL2's native filesystem;
  ccache (`/xemu/ccache`) persists in the tree either way. `ccache/` is
  not in upstream's `.gitignore`; it's excluded locally via
  `.git/info/exclude` (add that line again on a fresh clone, or move
  the cache to a Docker volume).

## Boot test setup

Config via `-config_path <file>`; the relevant keys:

```toml
[general]
show_welcome = false   # first-boot welcome suppresses -bios entirely

[sys.files]
bootrom_path = '...mcpx_1.0.bin'       # 512 B, MD5 d49c52a4...
flashrom_path = '...Complex_4627v1.03.bin'
eeprom_path = '...eeprom.bin'          # auto-generated, 256 B
hdd_path = '...xbox_hdd.qcow2'
dvd_path = '...<game>.xiso.iso'
```

- `show_welcome = false` matters: while the welcome dialog is pending,
  vl.c skips the `-bios` argument and the machine sits at
  "Failed to load BIOS '(null)'".
- The EEPROM is auto-generated at `eeprom_path` on first start (256 B)
  — the engine never needs to ship or request one.
- Blank HDD: `xbox_hdd.qcow2` from xemu-dashboard releases (1.6 MB
  sparse, 8 GB virtual, no Microsoft code — safe for the engine to
  bundle).

## Disc forms

xemu boots XISO images only. Full redump dual-layer ISOs (~7 GB, video
partition + game partition) do not boot; the game partition must be
extracted first (xdvdfs or equivalent — the docs' `dd skip=387` trick).
Rombie's Docs page should say so; `romFolderCheck.ts` already detects
both header layouts.

Boot-tested 2026-10-27 with Burnout 2 — Point of Impact (USA)
`.xiso.iso` (3.07 GB): boots through the Complex 4627 BIOS straight
into the game. (Only one Xbox disc was on hand; the redump no-boot side
is per xemu's documentation, not locally reproduced.)

## Speed

Machine: Ryzen 9 5950X, RTX 5070 Ti, Windows 11 25H2. Burnout 2 at
native internal resolution (`surface_scale = 1`), OpenGL renderer:
emulation steady at roughly 2.5-3 cores CPU, ~700 MB RSS.
Full-speed-with-sound call: see the go/no-go record in Rombie's
`.docs/plans/xbox-support.md` Phase 1.

One rendering quirk on this machine: `win32_dxgi_present` fails with an
incomplete FBO and xemu falls back to `SDL_GL_SwapWindow` — cosmetic
for windowed use, and moot for the windowless engine, which will not
use the present path at all.

## Save states (the Phase 4 mechanism)

From `ui/xemu-snapshots.c`: xemu save states are QEMU internal
snapshots (`save_snapshot`/`load_snapshot`) stored **inside the game's
HDD qcow2** — each snapshot holds the vmstate stream plus a disk
snapshot, and xemu prefixes the vmstate with its own extra-data block
(magic/version, disc path, XBE title, PNG thumbnail;
`xemu_snapshots_save_extra_data`). The snapshot list is read from the
qcow2's snapshot table, with a `-dirty` read-only reopen for the
thumbnails.

Consequences for the engine:

- Stock xemu has **no standalone export**: a state cannot leave the
  qcow2 without custom code that extracts both the vmstate stream and
  the disk delta. The plan's "standalone blobs" assumption does not
  hold as-is; either slots manage named internal snapshots (the stored
  per-game HDD then carries the states and grows with each), or the
  engine grows a custom export/import path.
- Thumbnails come for free (PNG already embedded per snapshot).
- vmstate format is tied to the QEMU/xemu build, so states are fragile
  across engine versions — the "Made on <version>" load guard planned
  for Rombie's slots UI is load-bearing.
- Snapshot shortcuts F5-F8 exist upstream (`general.snapshots.*`) but
  are irrelevant to the windowless engine.

## Phase 2 spike: windowless frames and sound into a canvas

The spike was `ui/xui/rombie-spike.cc`, enabled by an environment
variable: the SDL window created hidden, the render loop paced to the
vblank interval (no vsync to lean on), DXGI present skipped, and a
localhost WebSocket streaming to one page in the PS2 engine's message
layout. Phase 3's engine mode (below) replaced it and kept its hooks.

Hook points (the Phase 3 engine keeps these):

- Video: in `gl_render_frame` (`ui/xemu.c`), the texture from
  `nv2a_get_framebuffer_surface()` is the game's framebuffer at its own
  resolution. Render it into a same-size FBO with the flip inverted
  (the `RenderFramebufferToPng` recipe in `ui/xui/gl-helpers.cc`), then
  `glReadPixels` — ~0.6-0.7 ms at 640x480 on the 5950X. Alpha must be
  forced opaque. Double-buffer to a sender thread; drop when it is
  mid-send.
- Audio: `mcpx_apu_monitor_frame` (`hw/xbox/mcpx/apu/monitor.c`) holds
  the final 48 kHz stereo S16 mix, 256 frames per push; skip opening
  the SDL audio device so the page is the only speaker.

Measured 2026-10-27, Burnout 2 title scene, headless Chrome driving the
test page on the 5950X / RTX 5070 Ti:

- 60 fps at 640x480, ~74 MB/s over localhost (PS2 engine: ~65 MB/s).
- Frame-ready to canvas-drawn delay 0.8-2.3 ms average (PS2: 2-3 ms).
- Audio at exactly 48 kHz, zero worklet underruns against a 100 ms
  prime buffer; ~0-1 frame dropped per second by the engine's
  drop-when-busy policy.

## Phase 3: engine mode

`ui/xui/rombie-engine.cc` (+ `ui/rombie-engine.h`,
`ui/rombie-engine-version.h`) turns the build into Rombie's engine when it
is started with `--link <url>` or `--key <key>`; with no engine argument it
is stock xemu. Launch modes, link registration, one-time key, origin check,
one page at a time and the port handoff follow `pcsx2-rombie/Main.cpp`; the
message layout is documented at the top of `rombie-engine.cc`. Releasing:
`rombie/RELEASING.md`; packaging: `rombie/package.ps1`.

- Port 47653 (the PS2 engine has 47652); data folder
  `%LOCALAPPDATA%\RombieXbox` (`-<port>` suffix for a dev engine on another
  port): `xemu.toml`, `eeprom.bin`, `hdd-scratch.qcow2`, and, for a link
  start, `engine.log` (one per run).
- Boot: QEMU starts only once the page's boot message arrives. Flash BIOS
  and MCPX ROM stay in memory (`hw/xbox/xbox.c`); the disc is the `rombie:`
  block driver (`block/rombie-disc.c`), which forwards each read to the page
  and completes it in a bottom half. The HDD starts from `xbox_hdd.qcow2`
  beside the exe until Phase 4 hands that to the page.
- Input: four `INPUT_DEVICE_ROMBIE` pads bound to the console's four ports
  (`ui/xemu-input.c`); none of the PC's keyboards or gamepads are opened.
- Fast-forward: `cpu_set_vm_speed()` (`system/cpu-timers.c`) scales the VM
  clock and ticks from an anchor so neither jumps; virtual-clock timer waits
  shrink to match (`util/qemu-timer.c`), and the two real-time pacers, guest
  vblanks (`ui/xemu.c`) and APU frames (`apu.c`), divide by the speed. The
  page still gets 48 kHz: the engine forwards 1/speed of the audio blocks.
- Stats (once a second) include `vbl` (guest vblanks), `speed`, `padMsgs`,
  `guestPolls` per port and `guestSeen` (buttons the guest read): the first
  place to look when input seems dead.

Gotchas found on the way (2026-10-28):

- **xemu's HUD swallows input.** `InputManager::Update` turns on "test mode"
  whenever its ImGui HUD has navigation focus, and test mode makes every
  pad report zeros to the guest. The hidden HUD can hold focus, so engine
  mode never sets test mode (`ui/xui/input-manager.cc`).
- **Browsers fragment big messages.** A boot message with a 1 MB flash BIOS
  arrives as continuation frames; the reader joins them.
- **No `exit()` from engine code.** It runs xemu's UI static destructors,
  which crash (0xC0000005) when the UI never started, as in `--version`,
  setup or a no-page link start. `QuitNow()` flushes and `_exit()`s.
- **Listen first, then hand off.** A bind probe before the real bind made
  link starts fail to listen; the engine now listens on the main thread
  (with `SO_EXCLUSIVEADDRUSE`) and only tries the handoff when that fails.
- **Burnout 2 ignores input for about 40 s after boot** while its intro
  runs; tests that press Start earlier see nothing happen.
- **`-monitor tcp:127.0.0.1:4555,server,nowait`** passes through to QEMU in
  engine mode too; `info usb` and `trace-event usb_ohci_* on` found the
  input bug.
- **No update check.** With `general.updates.check` on, xemu asks GitHub's
  API for its latest release at every start (`ui/xui/main.cc`,
  `ui/xui/update.cc`). Engine mode skips it: it has no window to show the
  answer, and the engine makes no internet connection. 2026.0.0 still made
  it (found 2026-11-12); 2026.0.1 doesn't.

Verified on the 5950X / RTX 5070 Ti with Burnout 2 through Rombie's
`engine/xbox/engine-test.html` driven by headless Chrome: boot from
page-supplied firmware and disc chunks; keyboard Start and a gamepad's A
reach the game (title to LOAD/SAVE); pause freezes picture and sound,
resume restarts them, exit ends the process; fast-forward x2 runs 120
guest vblanks/s against 60 at x1; foreign origin and no origin get 403, a
stale key 4001, a second tab 4002. From the release zip, unzipped fresh:
`--version` prints the version, a `--link` start plays, ends 10 s after
its page leaves, and ends after 60 s when no page comes.

## Phase 4: the game's media

`ui/rombie-engine-media.c` (+ `.h`) runs on the UI thread with the BQL
held, as xemu's own snapshot actions do.

- One blob per game, which the page stores: `RXBX`, u32 format (1), u32
  EEPROM size, u64 image size, the EEPROM, the qcow2 image. The page sends
  it before boot (0x08; empty means a new game: the bundled blank disk and
  a freshly generated EEPROM); the engine validates it (header sizes, qcow2
  magic) and refuses to boot on a bad one rather than hand back a blank
  disk as the game's data.
- The engine returns the blob (0x85) a second after the game's last disk
  write outside its cache partitions, after every state change, before a
  pause and on exit. Reading it: drain all I/O, flush, read the qcow2 file
  and the EEPROM, end the drain.
  - The writes are seen at the IDE controller (`hw/ide/core.c`'s DMA and PIO
    writes call `rombie_media_hdd_write`). One inside the cache partitions
    X, Y and Z (`0x80000` to `0x8CA80000`) alone sends nothing.
  - The caches are scratch space a game fills from its disc and rebuilds
    when it must: Halo 2 streams its maps through them while it plays.
  - Its disk is about 700 MB once they fill, and reading that back holds
    the game a second or two. Halo 2 also writes its data partition E (at
    about `0xABFD3000`, 1.3 MB in) every 7 s or so under random input, so
    the cache filter alone didn't stop the reads.
  - In netplay the blob waits (marked changed) for the session's end, the
    next handoff (which sends it anyway), a state change or the exit: every
    read held every PC, and the speed control took the host for behind and
    slowed the game to its cap (Rombie BRAIN-38, Phase 6). With the hold,
    Halo 2's netplay check stays between 6 and 12 ns. A crash mid-session
    loses what the game saved since the last handoff.
- Save states are xemu snapshots inside that image, one per slot, tagged
  `slot<N>@<engine version>`. A load of a state made on another version is
  refused with a `state-version` event. Thumbnails stay xemu's (inside the
  snapshot); Rombie's own slot records keep the page's thumbnail.
- Sizes on Burnout 2: blank disk 1.6 MB, 2.1 MB with one profile saved,
  52.4 MB with one state. Deleting a state frees its space inside the file
  but does not shrink the file.

## Lockstep (Rombie BRAIN-38, `.docs/plans/xbox-online.md` Phase 2)

Two engines given the same game data, clock and input stay identical at
every vblank. The protocol (0x0B, 0x0C, 0x86, 0x87, `lw`; protocol 2) is at
the top of `ui/xui/rombie-engine.cc`; the parts:

- **Guest time is the instruction count**: `-icount shift=N,sleep=off,
  align=off` and `-rtc base=<0x0B's clock>,clock=vm` (`system/vl.c`).
  `ROMBIE_ICOUNT_SHIFT` (development) sets N, 2^N ns an instruction; 0 by
  default for now, 1 is about a real Xbox's rate.
- **Vblanks on guest time** (`ui/xemu.c`): a `QEMU_CLOCK_VIRTUAL` timer on
  the CPU thread instead of the real-time vblank thread. It hashes, sends
  0x87, waits for that vblank's 0x0C input, keeps real time at fixed
  deadlines (unless unthrottled) and latches the pads; the guest's pad polls
  read only the latched pads (`ui/xemu-input.c`). Its messages go out on the
  sender thread: sent from the CPU thread, they waited on the socket behind
  1.2 MB frames and cost half of every second.
- **APU frames on the CPU thread** (`hw/xbox/mcpx/apu/apu.c`), one every
  2/3 ms of guest time, with the frame thread idle; four voice workers on
  every PC, their mixes added in worker order (`vp.c`).
- **The GPU runs only while the CPU waits for it** (`hw/xbox/nv2a/pfifo.c`,
  `nv2a.c`, `user.c`): after a PFIFO, PGRAPH or USER write; a DMA_PUT
  write only leaves work waiting, which runs before the next NV2A access
  (every block's MMIO goes through `nv2a_lockstep_read`/`write`), vblank,
  spin or surface access. Only those passes run the pusher (the display's
  syncs don't).
- **The hard disk's I/O completes at the guest time it started**
  (`hw/xbox/rombie-lockstep.c`, `hw/ide/core.c`, `hw/ide/pci.c`,
  `block/block-backend.c`): after an IDE register access the CPU waits,
  on a semaphore the block layer posts, until no request but the disc's is
  in flight. (A g_usleep poll slept a whole Windows tick, up to 15.6 ms.)
- **The disc's reads complete at a deadline** of guest time instead,
  50 ms for a seek or 2 ms for a read that follows the last, plus the
  transfer at 16 MB/s, in the order they were asked for. The guest runs on
  meanwhile; IDE's completion, the only thing that puts the data where the
  guest sees it, runs on the CPU thread at the deadline, which waits only
  if the page hasn't answered. The page took 33-60 ms a read, and a CPU
  that stood still for each ran loading at a third of real time.
- **The engine reads the disc ahead** (`ui/xui/rombie-engine.cc`, any
  mode): the page is asked for whole 1 MB chunks, every read also asks for
  the two after it, and 24 stay. A read pins its chunks until it has
  copied them: an evicted one handed a read waiting on its other chunk
  whatever was left, and the engines diverged.
- **The AC97 drains in guest time**: lockstep's default audio backend is
  `none` (`audio/audio.c`); the page hears the APU as before.
- **Spins skipped** (`accel/tcg/tcg-accel-ops-rr.c`): under icount every
  instruction costs host time, and the Xbox spins rather than halts. The
  kernel's idle loop is `sti; nop; nop; cli` and two compares (`8001b02e`
  on the 4627 BIOS); Burnout 2 waits on the GPU by reading a word in RAM
  (`0021b180`). Together they were 98% of the guest's instructions. Each
  timer interval now opens with two 840-instruction probes; when both leave
  the registers identical, guest time jumps to the next timer. Halts warp on
  the CPU thread too (`icount_warp_in_vcpu`), saving the main loop's two
  wake-ups. Two host-time leaks made engines skip differently (only the
  instruction counts differed, from vblank 3840): icount's budget also
  stopped at real-time timers (not in lockstep now, `icount_get_limit`),
  and a kick in an interval that had found no spin let it probe again (it
  now runs to the interval's end).
- **Hashes** (0x86): RAM, CPU, NV2A registers, APU registers and DSP
  memories, each drive's I/O counts, and guest time. The stats carry a
  `prof` line in lockstep (where the CPU thread's time went);
  `ROMBIE_LOCKSTEP_PROFILE=1` also prints the guest's hot places.

Measured on Burnout 2, two engines on the 5950X, flat out, the first
40-60 s from boot:

- Each sync off alone (0x10, 0x20, 0x40) diverges by vblank 60; all on,
  every hash matches.
- TCG under icount runs about 230 million guest instructions a second.
  Before the spin skip the guest needed 1,000 million a second at shift 0
  (500 million at shift 1), so it ran at about 10 vblanks a second.
- With the spin skip, shift 1, flat out: Burnout 2 runs at 100-111 vblanks
  a second once it is past the dashboard's boot animation. The animation
  is the heaviest GPU work (12 ms a frame solo, where it runs at 56 frames
  a second; 18 ms in lockstep, about 25 a second); netplay joins a running
  game and never plays it.
- Real time, shift 1, hashing every 60, measured from vblank 600: 7,200
  vblanks at 98.3% and 99.1% of those due, the lowest 30 s 93% and 96%,
  9-11 s short of sound, all in loading; the same run with the disc in
  Windows' file cache reached 100% and 98%. Solo (no lockstep), the same
  stretch plays at 60 frames a second.
- Flat out, hashing every vblank: 7,200 vblanks match (seed 2).

### Lockstep speed: four-player TimeSplitters 2 and Halo 2 (2026-11-14)

Burnout 2 idles; four-player split-screen games don't. Their first runs
held 57% of real time on one engine on the 5950X (TimeSplitters 2) and 18%
on the guest laptop (i7-8750H, GTX 1060). What fixed it, in order, each step
in step at every vblank on the main PC (TimeSplitters 2 and Halo 2 from
boot into their matches, flat out, hashed every vblank):

- **The guest's CPU speed** (`accel/tcg/icount-common.c`): guest time an
  instruction in sixteenths of a ns (`rombie_icount_set_scale`), finer than
  the shift, changed on the CPU thread at a vblank with the clock kept
  continuous (the bias takes up the change). The page sets it: 0x0B's start
  and 0x0F changes at a named vblank, which every engine gets before it gets
  there; `ROMBIE_ICOUNT_NS="vblank:ns,..."` schedules it for development.
  In lockstep a CPU-bound game slows as a whole where solo it drops frames,
  so the guest must be no faster than the slowest PC runs it. The test
  page's adaptive mode slows it by 15% when an engine falls behind and
  speeds it by 5% when every engine has more than 15% to spare (stats'
  `pace`, the time the CPU thread slept ahead of real time).
- **The GPU works ahead** (`hw/xbox/nv2a/pfifo.c`, `pgraph.c`, `nv2a.c`): a
  DMA_PUT starts the pusher at once; the CPU waits only at its old points.
  While the CPU runs, the GPU's semaphore and report writes are held
  (`nv2a_lockstep_gpu_write`) and land as the CPU starts waiting, and an
  interrupt parks the GPU thread until then (`nv2a_lockstep_park`). Its
  other writes (downloads, blits) land in surfaces' memory, whose every CPU
  access waits for the GPU first. The SDK polls USER's DMA_GET after kicks:
  a first read since the CPU last waited gets that wait's value, which the
  GPU has passed, and a second read with no wait between is a poll and
  waits. The PFB flush (WBC, `0x410`) it writes and polls doesn't wait; PFB
  reads never follow the GPU, though its tiles do, so other PFB writes
  still wait. Flag 0x08 keeps the serial GPU.
- **The display doesn't touch a surface's age** (`gl/display.c`,
  `vk/renderer.c`): the age decides eviction, which downloads into guest
  memory, so only the GPU's work sets it.
- **Voices read guest RAM straight from its block** (`vp.c` `vp_ldl` and
  kin): dozens of memory-API lookups a voice every frame, and a sample's
  each, halved the APU's time. Writes still go through the API, for its
  dirty tracking.
- **Voices resample linearly in lockstep** (xemu's note allows the hardware
  may be linear) instead of the sinc; **HRTF** walks its ring buffer without
  divisions (the same sums); the APU's threads **flush denormals** alike on
  every PC (the guest's floats are softfloat, unaffected).
- **An APU frame's voices run ahead** (`apu.c` `lockstep_launch`,
  `lockstep_commit_locked`): at a frame's tick its voices start on the frame
  thread from a copy of the voice descriptors, and the guest runs on. Their
  changes (only the words they changed), notifications, the DSPs' frame, the
  sound and interrupts land at the next APU access, vblank, spin or tick.
  Flag 0x80 keeps the serial APU.
- **Flag 0x04, a speed test**: one frame in ten and no game data.
  Playwright's DevTools connection carries every WebSocket frame (1.2 MB,
  60 a second); on the laptop that cost three threads beside the engine,
  and Halo 2's game data (700 MB, its cache partitions) crashed the driver.

Figures, one engine in real time in the match, flag 0x04:

| | main PC (5950X, RTX 5070 Ti) | laptop (i7-8750H, GTX 1060) |
|---|---|---|
| TimeSplitters 2, 6 ns | 100.6% (lowest 30 s 100.6%) | 49% |
| TimeSplitters 2, adaptive | | 100.7%, at 13.3 ns, 18 frames a second |
| Halo 2, 6 ns | 90.5% (GL), 100.4% (Vulkan) | |
| Halo 2, adaptive | | 100.4%, at 20.3 ns, 8 frames a second |

Two engines on the main PC, adaptive, TimeSplitters 2: 100% each, 5.4-6.9
ns, 41 frames a second, 150 hashes matching across nine speed changes. On
the laptop TCG runs 50-73 million instructions a second against the main
PC's 125-165, and with the APU and the GPU off the CPU thread that is its
limit. On the main PC Halo 2's limit is the GL renderer (the GPU thread
busy 660 ms a second); Vulkan cuts it to about 550, but two Vulkan engines
differ in RAM alone at 24 of 9,000 vblanks (transient, every other part
equal), from something that still writes guest RAM on host time; lockstep
stays on GL until that's found.

Since then (to `091df25a9c`):

- **The pusher's own writes to memory wait for the CPU** (`nv2a_lockstep_park_ram`,
  `gl/surface.c` `download_if_dirty`, both `blit.c`): a download or blit the
  pusher makes, ahead, could interleave with the CPU's writes once a surface
  was gone (Halo 2's RAM differed at 1,016 of 42,200 vblanks); it parks until
  the CPU waits, as an interrupt does. About 4% of Halo 2's speed.
- **Lockstep boots with the short startup animation** (`skip_boot_anim`;
  `ROMBIE_LOCKSTEP_FULL_ANIMATION=1` keeps the full one): every PC boots at a
  handoff, and the full one's render targets, left in memory, differ between
  GPU makers.
- **0x10** dumps RAM's page hashes and the GPU's surfaces at a vblank, and
  `ROMBIE_LOCKSTEP_TRACE_REPORTS=1` logs every occlusion count the guest gets,
  to compare two PCs.

Two PCs: the GTX 1060 replays the main PC's TimeSplitters 2 run at all 41,000
vblanks, every part; Halo 2 everything but RAM, which differs at 1,016
vblanks in a render target the two GPU generations draw differently and the
game doesn't read. The Intel UHD 630 diverges: 11 of 1,625 occlusion counts
differ (the first 36 pixels on NVIDIA, 40 on Intel), then the CPU. Rombie's
call (2026-11-14): integrated graphics are below Xbox netplay's minimum.

## Netplay's handoff (Rombie BRAIN-38, Phase 3)

A running engine can't go into lockstep (icount is fixed when QEMU starts),
so every PC, the host included, boots again at a handoff and loads one state
into a machine that has never run.

- **0x0D host** (`HandleNetplay`, `rombie_media_handoff`): the machine stops
  where it is (`vm_stop(RUN_STATE_SAVE_VM)`, as `save_snapshot` does: the
  GPU's surfaces go into memory, the APU's frame lands) and the engine
  writes the machine's state as a stream to a file (`rombie_savevm_file`,
  `migration/savevm.c`) and the disk as the guest sees it into a fresh qcow2
  (`copy_disk`: only clusters that hold data, zeros and the disc's runs left
  out, none of the player's save states). It sends its own game data (0x85,
  save states and all, which the host boots again with) and then 0x88: the
  guests' game data and the state, which is u64 guest time (ns), the disc
  runs and the stream. It stays stopped; the page restarts it.
- **Disc runs.** A game's disk can hold much of its disc: Halo 2 copies
  370 MB of its maps to its cache partitions in its first two minutes, 99.5%
  of its disk then, in 4 KB blocks that are two sectors of the disc each.
  Every guest has the disc, so `copy_disk` leaves out each block whose two
  halves hash as two consecutive sectors of the disc, and the state names
  them in runs (disk offset, disc sector, sectors, and a check: the sectors'
  hashes in order). The engine knows a sector by its hash once the page has
  sent its chunk (`rombie_media_disc_chunk`, from `HandleDiscData`), and
  keeps the hashes in its data folder (`disc-sectors.bin`, one disc, named
  by its size and its first megabyte), for the engines after it: a
  restarted host's resync, or a session whose cache an earlier one filled.
  On every PC, before the state loads, each run comes from that PC's own
  disc (`fill_from_disc`), unless the disk already holds it (the host's own,
  rebooted with its full disk); a check that doesn't match fails the join
  ("This disc isn't the same as the host's").
- **No BIOS in the state**: `xbox.bios` (the flash) and `xbox.mcpx` (the
  flash with the MCPX ROM over its end) are marked not migratable around the
  save, so the stream leaves them out, and around the load, so a stream
  holding them is refused. Each PC's come from its own files at boot.
- **0x04 op 4, restart** (`RestartSelf`): the engine closes the page's
  connection with a reset (a close from its end would leave the port in
  TIME_WAIT, which the exclusive bind refuses for minutes), closes its
  listener, and starts itself again with the same arguments plus
  `--restarted`, inheriting only stdout and stderr (an inherited socket
  would keep the port). The new process retries the port for 30 s.
- **0x0D join**, after 0x0B and 0x08 and before boot: the engine keeps the
  state, and once the machine is created and before it first runs
  (`rombie_engine_before_start`, called from `qmp_x_exit_preconfig` in
  `system/vl.c`) it plugs in the four pads (the host's machine had them;
  their hubs are in the state), loads the stream (`rombie_loadvm_file`, as
  `load_snapshot` does), and sets lockstep's vblank and input count to the
  vblank named.
- **Guest time goes on from the state's** (`system/cpu-timers.c`): the
  timers' section loads first, and its `post_load` sets icount's bias so the
  clock reads the handoff's guest time, whatever the source counted (no
  icount on a solo host, another guest speed on a resync), so every timer
  the devices restore comes due when it would have. The guest's TSC follows
  the clock (`hw/i386/x86-cpu.c`). Lockstep's vblanks and APU frames count
  from there (`xemu_lockstep_vblank_rebase`, `mcpx_apu_lockstep_rebase`), and
  0x0B's guest speed applies at once. The RTC starts again from 0x0B's date
  plus guest time (`rombie_rtc_restart_clock`): a solo host's RTC runs on
  the host's clock, which lockstep doesn't use.
- **Stops in lockstep.** Lockstep waits inside QEMU_CLOCK_VIRTUAL timers'
  callbacks with the BQL let go (the vblank for input, its GPU sync, a disc
  read), and a stop or a reset (`pause_all_vcpus`) holds the BQL while it
  disables that clock, which waits for the callbacks in flight: every pause,
  save, exit or guest reboot there deadlocked. While a virtual-clock
  callback has let go of the BQL its list now says it has none in flight
  (`bql_unlock` and `bql_lock` call `rombie_timers_bql_released` and
  `rombie_timers_bql_taken`, `util/qemu-timer.c`), and the vblank sees the
  stop by the run state (`rombie_lockstep_stopping`), which a stop sets
  first.
- **The guest's own reset** (Halo 2 reboots in its opening) comes at the
  same guest point on every PC. Lockstep's APU frame in flight belongs to
  the machine before it, so the reset waits for it and drops it (its voice
  writes would land on the reset's registers and run the DSP on garbage),
  and a GPU running ahead first does all it was given, as if the CPU waited
  for it (`pfifo_lockstep_reset`), so its held writes land before the
  registers go.
- **What only the host does.** In lockstep a save, a load, a pause, the
  host's handoff and exit run on the UI thread while the CPU holds at a vblank
  (`QueueAtVblank`), where the GPU has done all it was given: stopped
  anywhere else its thread may be parked on an interrupt for a CPU that no
  longer runs. A slot save there leaves the GPU's surfaces out of memory
  (`nv2a_save_keeps_surfaces`): copying them in showed the host's game what
  the guests' didn't (RAM differed until the next resync). Such a state
  holds memory as the game left it; a surface the game doesn't draw again
  after a load shows its old pixels. A state saved in lockstep carries its
  guest time in `cpu_clock_offset`, where an engine running alone reads it.
  The game data read-back (0x85: a drain, a flush and the files read, at
  the engine's own moments) changes nothing the game sees: both engines of a
  check read theirs back every 0.7-2 s (`ROMBIE_LOCKSTEP_READBACK_MS`) and
  stay identical.
- **0x0D end**: the vblanks stop waiting for input and the guest reads the
  page's own pads again, at the guest speed reached.
- **Development**: with `ROMBIE_HANDOFF_DIR` set the host writes the
  handoff there and sends 0x88 empty, and a join with no state reads it from
  there, so the test page's DevTools connection never carries Halo 2's disk.
  With `ROMBIE_LOCKSTEP_READBACK_MS` set, a speed test (which sends no game
  data) still reads its game data back that often while it runs, and drops
  it.

Figures (main PC): TimeSplitters 2's state is 61-62 MB (8.1-8.5 MB
gzipped), the guests' game data 3.1 MB (0.9 MB), the host's stop and copy
0.3 s, a restart 0.9 s, a join 1.3-2.0 s. Halo 2's state is 48 MB (16 MB
gzipped); its disk had 370 MB of data at vblank 7200, 367 MB of it in 234
disc runs, so the guests' game data is 7.3 MB (391 MB without the runs,
280 MB gzipped); the host's stop 0.4 s, a guest's join 4 s, reading the
runs from a USB hard drive at 94 MB/s. `lockstep-test.html`'s
`xboxLockstep.handoff` (`lockstep-run.py --handoff`) checks it: A plays alone
or in lockstep, hosts and restarts, B joins fresh, A saves a state on the
way and then hosts again (a resync), hashes compared every vblank.
