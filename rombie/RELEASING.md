# Releasing the Rombie Xbox engine

How a new engine version gets from this fork to a download on GitHub. The engine
is built and released from `github.com/Bro-Code-Technologies/xemu-rombie`
(branch `rombie`, checked out at `C:\Users\Michael\Projects\xemu-rombie`); the
build itself is in [docs/rombie/notes.md](../docs/rombie/notes.md#building-windows-target).
This file covers what comes after the code works: the version, the zip, the
tag, the GitHub release and promoting it.

A person publishes the release. Nothing here is automated.

## Names

| What | Format | 2026.0.0 |
| --- | --- | --- |
| Version | `YYYY.N.patch` | `2026.0.0` |
| Git tag | `rombie-xbox-engine-<version>` | `rombie-xbox-engine-2026.0.0` |
| Release name | `Rombie Xbox Engine <version>` | `Rombie Xbox Engine 2026.0.0` |
| Zip (from `package.ps1`) | `rombie-xbox-engine-<version>-windows-x64.zip` | `rombie-xbox-engine-2026.0.0-windows-x64.zip` |
| Commit message | `Rombie Xbox engine <version>: <what changed>` | `Rombie Xbox engine 2026.0.0: first release` |

Versions follow Rombie's other engines: `N` counts up through the year and goes
back to 0 in January. Raise `N` when the engine's behavior changes and `patch`
for fixes. The engine releases on its own tag, never with Rombie's version. The
wire protocol has its own number (`PROTOCOL_VERSION` in
`ui/xui/rombie-engine.cc`), and a protocol change is the point to raise
`XBOX_MIN_ENGINE_VERSION` in Rombie.

## Pre-release and official

Rombie's "Get the Xbox engine" link points at
`https://github.com/Bro-Code-Technologies/xemu-rombie/releases/latest`, and
GitHub's "latest" skips pre-releases. So:

- A **pre-release** is invisible to players. Staging testers get its direct link.
- An **official release** (the "latest release") is what every Rombie player is
  sent to. Promote one only after it has been checked on staging, and before the
  `apps.rombie.sys-xbox` flag is switched on in production.

A bug found after a release has gone out is fixed in a new version (2026.0.1),
never by changing the files of the old one.

## Steps

1. **Make the change** on the `rombie` branch.
   - `ui/rombie-engine-version.h`: `ROMBIE_XBOX_ENGINE_VERSION`.
   - `ALLOWED_ORIGINS` in `ui/xui/rombie-engine.cc`, if the pages allowed to
     drive the engine change. Then update Rombie's `.docs/architecture.md`
     (Xbox, the Security bullet) to match.
2. **Commit and push the branch.** Build from a clean tree (`git status` shows
   nothing), so the exe really is built from the commit you are about to tag.
3. **Close any running engine.** A running `xemu.exe` or
   `rombie-xbox-engine.exe` holds `dist\xemu.exe`, and the build's packaging
   step cannot replace it. Check Task Manager.
4. **Build** (from the directory that contains the clone, Docker running):
   ```powershell
   docker run --rm -v C:\Users\Michael\Projects\xemu-rombie:/xemu -w /xemu `
     -e CCACHE_DIR=/xemu/ccache ghcr.io/xemu-project/xemu-win64-toolchain:latest `
     ./build.sh -p win64-cross
   .\dist\xemu.exe --version | Out-String   # must print the new version
   ```
5. **Package** (from the fork's root):
   ```powershell
   powershell -File rombie\package.ps1
   ```
   It writes `dist\rombie-xbox-engine-<version>-windows-x64.zip` (about 50 MB;
   the name is taken from `ui/rombie-engine-version.h`). It downloads the blank
   hard disk image and its license from the pinned xemu-dashboard release and
   stops if either file's SHA-256 differs. `dist\` is not tracked by git.
6. **Try the zip the way a player would.** Unzip it into a new folder outside
   the build tree, then:
   - `.\rombie-xbox-engine.exe --version | Out-String` prints the new version.
     The engine has no console window, so PowerShell only shows its output when
     it is piped, as here.
   - Run the exe once. It says it is set up. This also re-registers the
     `rombie-xbox://` link to this folder, so a tester who had an older engine
     installed must do it once, or the browser keeps starting the old one.
   - Play an Xbox game from Rombie's dev server (`http://localhost:5173`). The
     browser's prompts ("Open Rombie Xbox Engine?", tick Always allow, and access
     to other apps and services) should appear the first time.
7. **Tag the release commit and push the tag:**
   ```powershell
   git tag rombie-xbox-engine-<version>
   git push origin rombie-xbox-engine-<version>
   ```
8. **Publish on GitHub.** Repository, Releases, Draft a new release:
   1. Choose the tag you just pushed.
   2. Release title: `Rombie Xbox Engine <version>`.
   3. Paste the description from the template below.
   4. Attach `rombie-xbox-engine-<version>-windows-x64.zip`.
   5. For a release that still needs staging: tick **Set as a pre-release**.
      Otherwise tick **Set as the latest release**.
   6. Publish release.
9. **Check the release page.** The zip is attached with the right name, the tag
   points at the commit you meant, and the download works. For a pre-release,
   also open `.../releases/latest` and confirm it did **not** change.

## Promoting a pre-release to the official release

After staging has signed it off: open the release, Edit, untick **Set as a
pre-release**, tick **Set as the latest release**, Update release. Then open
`.../releases/latest` and confirm it lands on this release, and follow the "Get
the Xbox engine" link on rombie.app's Docs page. Do this **before** the Xbox
feature flag is turned on for everyone.

To roll back, open the previous good release and make it the latest.

## Description template

Everything between the lines goes into the GitHub release description. Replace
`<version>`, and change the "New in" line for each release: one plain sentence
about what is different for the player.

```markdown
## Rombie Xbox Engine <version>

The Xbox engine is the small Windows program that runs original Xbox games for [Rombie](https://rombie.app). You install it once. After that, Rombie starts it whenever you play an Xbox game, and the game plays on Rombie's page like any other console.

### Install

1. Download `rombie-xbox-engine-<version>-windows-x64.zip` below and unzip it into a folder you'll keep.
2. Run `rombie-xbox-engine.exe` once. It isn't code-signed, so if Windows says it protected your PC, choose **More info**, then **Run anyway**. The engine tells you when it's set up.
3. Play an Xbox game in Rombie. The first time, your browser asks to open Rombie Xbox Engine: tick **Always allow** and open it. It may also ask whether the site can access other apps and services on your device: allow that too.

If you move the folder, run the engine once more from its new place. To remove it, run `rombie-xbox-engine.exe --unregister`, then delete the folder.

### What you need

- Windows 10 (22H2) or Windows 11, 64-bit
- A graphics card with OpenGL 4.0
- Chrome or Edge on the same computer
- Your own Xbox MCPX boot ROM, flash BIOS and game discs (`.iso`, in XISO form). None are included, and none leave your computer.

Setup details are in Rombie's Docs page.

### About this release

- New in <version>: <one plain sentence about what is different for the player>
- Built from xemu, with Rombie's engine mode and a few small hooks, all on the `rombie` branch of this repository.
- Runs without a window and talks to Rombie only over your own computer.
- Includes xemu-dashboard's blank hard disk image, which every game's own disk starts from. It contains no Microsoft code.
- Rombie shows an "update the Xbox engine" message if your version is too old for it. There is no auto-updater.

### License and source

The Xbox engine is free software under the GNU GPL v2 or later, as xemu is. The source for this release is the commit tagged `rombie-xbox-engine-<version>` in this repository.
```

### Ready to paste for 2026.0.0

```markdown
## Rombie Xbox Engine 2026.0.0

The Xbox engine is the small Windows program that runs original Xbox games for [Rombie](https://rombie.app). You install it once. After that, Rombie starts it whenever you play an Xbox game, and the game plays on Rombie's page like any other console.

### Install

1. Download `rombie-xbox-engine-2026.0.0-windows-x64.zip` below and unzip it into a folder you'll keep.
2. Run `rombie-xbox-engine.exe` once. It isn't code-signed, so if Windows says it protected your PC, choose **More info**, then **Run anyway**. The engine tells you when it's set up.
3. Play an Xbox game in Rombie. The first time, your browser asks to open Rombie Xbox Engine: tick **Always allow** and open it. It may also ask whether the site can access other apps and services on your device: allow that too.

If you move the folder, run the engine once more from its new place. To remove it, run `rombie-xbox-engine.exe --unregister`, then delete the folder.

### What you need

- Windows 10 (22H2) or Windows 11, 64-bit
- A graphics card with OpenGL 4.0
- Chrome or Edge on the same computer
- Your own Xbox MCPX boot ROM, flash BIOS and game discs (`.iso`, in XISO form). None are included, and none leave your computer.

Setup details are in Rombie's Docs page.

### About this release

- New in 2026.0.0: the first release. Plays original Xbox games in Rombie with four players, pause and fast-forward.
- Built from xemu, with Rombie's engine mode and a few small hooks, all on the `rombie` branch of this repository.
- Runs without a window and talks to Rombie only over your own computer.
- Includes xemu-dashboard's blank hard disk image, which every game's own disk starts from. It contains no Microsoft code.
- Rombie shows an "update the Xbox engine" message if your version is too old for it. There is no auto-updater.

### License and source

The Xbox engine is free software under the GNU GPL v2 or later, as xemu is. The source for this release is the commit tagged `rombie-xbox-engine-2026.0.0` in this repository.
```
