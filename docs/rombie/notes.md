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
  write (the HDD BlockBackend's write counter, `ide0-hd0`), after every
  state change, before a pause and on exit. Reading it: drain all I/O,
  flush, read the qcow2 file and the EEPROM, end the drain.
- Save states are xemu snapshots inside that image, one per slot, tagged
  `slot<N>@<engine version>`. A load of a state made on another version is
  refused with a `state-version` event. Thumbnails stay xemu's (inside the
  snapshot); Rombie's own slot records keep the page's thumbnail.
- Sizes on Burnout 2: blank disk 1.6 MB, 2.1 MB with one profile saved,
  52.4 MB with one state. Deleting a state frees its space inside the file
  but does not shrink the file.
