# Rombie Xbox engine — Phase 1 notes (fork, build, boot test)

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
