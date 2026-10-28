# Builds the Xbox engine's download: a zip with the engine, the blank hard disk image it starts
# every game from, the licenses and a README. Run from the fork's root after a build
# (dist\xemu.exe, see docs\rombie\notes.md).
#   powershell -File rombie\package.ps1 [-Dist dist] [-Out dist]
param(
	[string]$Dist = "dist",
	[string]$Out = "dist"
)
$ErrorActionPreference = 'Stop'

$version = (Select-String -Path ui\rombie-engine-version.h -Pattern '#define ROMBIE_XBOX_ENGINE_VERSION "(.+)"').Matches[0].Groups[1].Value
$name = "rombie-xbox-engine-$version-windows-x64"
$stage = Join-Path $Out $name
$zip = "$stage.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item "$Dist\xemu.exe" "$stage\rombie-xbox-engine.exe"

# The blank hard disk every game's own disk is copied from: xemu-dashboard's image (MIT; an open
# dashboard built with nxdk, no Microsoft code). Pinned to one release and checked by hash, so a
# release ships exactly the bytes that were tested. A new image is a deliberate change here.
$dashboard = 'https://github.com/xemu-project/xemu-dashboard/releases/download/v20260516-0955'
$pinned = @{
	'xbox_hdd.qcow2' = '00D7DF7A2BC235F8801764F00B7F40E194D1E392F7A9619D6B2396C89770F6DD'
	'LICENSE'        = '3C4D8C95FDCD47F61E106F1E597D8C9DB37A57193D0239346EDCBC9F4F2334F6'
}
$cache = Join-Path $Out 'xemu-dashboard-v20260516-0955'
New-Item -ItemType Directory -Force $cache | Out-Null
foreach ($file in $pinned.Keys) {
	$path = Join-Path $cache $file
	if (-not (Test-Path $path)) { Invoke-WebRequest -Uri "$dashboard/$file" -OutFile $path }
	$hash = (Get-FileHash $path -Algorithm SHA256).Hash
	if ($hash -ne $pinned[$file]) { throw "$file has SHA-256 $hash, expected $($pinned[$file]). Delete $cache and try again." }
}
Copy-Item (Join-Path $cache 'xbox_hdd.qcow2') "$stage\xbox_hdd.qcow2"

New-Item -ItemType Directory "$stage\licenses" | Out-Null
# xemu's own license with every library it links (written by the build's gen-license.py). Its
# header sends readers to upstream xemu for the source; this build's source is the fork, at the
# release's tag.
$license = [IO.File]::ReadAllText((Resolve-Path "$Dist\LICENSE.txt"))
$upstream = 'is available at:' + "`n`n" + (' ' * 32) + 'https://xemu.app'
if (-not $license.Contains($upstream)) { throw "LICENSE.txt's source line changed; update package.ps1 to match." }
$fork = 'is available at:' + "`n`n" + (' ' * 32) + "https://github.com/Bro-Code-Technologies/xemu-rombie/tree/rombie-xbox-engine-$version"
[IO.File]::WriteAllText((Join-Path (Resolve-Path $stage) 'LICENSE.txt'), $license.Replace($upstream, $fork))
Copy-Item (Join-Path $cache 'LICENSE') "$stage\licenses\xemu-dashboard.txt"

$readme = @"
Rombie Xbox Engine $version

Plays original Xbox games in Rombie (https://rombie.app). It runs xemu on this computer,
without a window of its own, and shows the game inside Rombie.

Setting it up
  1. Unzip this folder somewhere it can stay, for example Documents\Rombie Xbox Engine.
  2. Run rombie-xbox-engine.exe once. It says it's set up, and that's all.
  From then on Rombie starts the engine whenever you play an Xbox game. The first time, your
  browser asks whether to open "Rombie Xbox Engine": allow it, and tick "always allow" so it
  doesn't ask again.

Moving or removing it
  Moved the folder? Run rombie-xbox-engine.exe once more from its new place.
  To remove it, run "rombie-xbox-engine.exe --unregister", then delete the folder.

Privacy
  The engine only accepts connections from this computer, and only from Rombie. Your discs and
  firmware files stay on this computer: Rombie reads them from your ROM folder and hands them to
  the engine piece by piece. The engine doesn't save them anywhere.

Requirements
  64-bit Windows 10 or 11, and a graphics card with OpenGL 4.0.

License
  The engine is free software under the GNU General Public License v2 (LICENSE.txt, with the
  licenses of the libraries it includes). It's built from xemu (https://xemu.app).
  xbox_hdd.qcow2 is xemu-dashboard's blank hard disk (licenses\xemu-dashboard.txt).
  Source code: https://github.com/Bro-Code-Technologies/xemu-rombie
"@
Set-Content -Path "$stage\README.txt" -Value $readme -Encoding utf8

Compress-Archive -Path $stage -DestinationPath $zip
$size = (Get-Item $zip).Length
"{0} ({1:N1} MB)" -f $zip, ($size / 1MB)
