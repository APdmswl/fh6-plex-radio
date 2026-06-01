# One-shot build script. Configures CMake (Release, x64), compiles, then
# stages everything that needs to ship in dist\.
#
#   PS> .\scripts\build.ps1
#
# Output:
#   dist\version.dll            the proxy DLL (drops next to forzahorizon6.exe)
#   dist\fh6-radio\ui\          dashboard (mounted at http://localhost:<port>)
#   dist\fh6-radio\config.toml  seeded from config.example.toml

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$dist  = Join-Path $root "dist"

# Locate cmake.exe. Prefer the one on PATH; otherwise look inside any VS
# install (which always ships CMake when the C++ workload is selected),
# then fall back to the standalone CMake installer's default location.
function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoots = & $vswhere -all -products * -property installationPath
        foreach ($vs in $vsRoots) {
            $p = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $p) { return $p }
        }
    }
    foreach ($p in @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )) { if (Test-Path $p) { return $p } }

    throw @"
cmake.exe not found. Either:
  - install Visual Studio 2022/2026 with the "Desktop development with C++"
    workload (CMake is bundled), or
  - install CMake from https://cmake.org/download/ (tick "Add CMake to PATH").
"@
}

$cmake = Find-CMake
Write-Host "Using cmake: $cmake" -ForegroundColor DarkGray

if (-not (Test-Path (Join-Path $root "third_party\nlohmann\nlohmann\json.hpp"))) {
    Write-Host "third_party/ is empty -- running get-deps.ps1 first." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot "get-deps.ps1")
}

Write-Host "-> cmake configure" -ForegroundColor Cyan
& $cmake -S $root -B $build -A x64 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "-> cmake build (Release)" -ForegroundColor Cyan
& $cmake --build $build --config Release | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "fh6-radio") | Out-Null

Copy-Item (Join-Path $build "Release\version.dll") $dist
Copy-Item -Recurse (Join-Path $root "ui\dist") (Join-Path $dist "fh6-radio\ui")
Copy-Item (Join-Path $root "config.example.toml") (Join-Path $dist "fh6-radio\config.toml")
foreach ($doc in @(
    "LICENSE",
    "README.ko.md",
    "NOTICE.txt",
    "CHANGES.txt",
    "SOURCE_CODE.txt",
    "THIRD_PARTY_NOTICES.txt"
)) {
    Copy-Item (Join-Path $root $doc) $dist
}

$readme = @'
FH6 Plex Radio
--------------

Plex-focused radio integration for Forza Horizon 6. It exposes a local
web dashboard and routes Plex music through the in-game radio audio path.


Getting it running
~~~~~~~~~~~~~~~~~~

Make sure FH6 isn't open first. Then drop the contents of this archive
straight into the folder that contains forzahorizon6.exe. Depending on
where you installed the game, that'll look like one of:

    Steam      ->  ...\steamapps\common\ForzaHorizon6
    Xbox app   ->  ...\XboxGames\Forza Horizon 6\Content

Let Windows overwrite when it asks. Heads-up: some antivirus tools dunk
on the bundled version.dll because of how the mod hooks into the game.
If yours yeets the file, add the FH6 folder to its exclusions
list and re-extract.

Plex playback is decoded through ffmpeg. This archive does not bundle
ffmpeg, so install it separately. Either put ffmpeg.exe on PATH or enter
the full path in Settings > General > ffmpeg path. Settings > Plex >
ffmpeg path can still be used as a Plex-specific override. Portable ffmpeg
works fine, for example:

    E:\ffmpeg\bin\ffmpeg.exe

Once the files are in place, launch the game and head into
Settings -> Audio. Two switches matter:

    Streamer Mode  ->  ON     (the new station only shows up with
                                this enabled)
    Radio DJ       ->  OFF    (otherwise the in-game DJ chimes in
                                over your tracks)

Now cycle the radio stations in-game until you land on the new one.
The mod's audio only goes out while that station is the active one --
flip to another station and it stops broadcasting.


Configuring Plex
~~~~~~~~~~~~~~~~

With the game running, open the local dashboard:

    http://localhost:8420

Enter your Plex server URL and token, then hit Load. Pick a music
library, artist, album, or audio playlist, then Save & Play. The stream
is decoded through ffmpeg, so either keep ffmpeg on PATH or set its full
path from Settings > General. If ffmpeg is missing or the path is wrong,
Plex tracks may fail, skip immediately, or stop after a short burst of
audio.

For a local Plex server on your LAN, http://192.168.x.x:32400 is usually
more reliable than https://192.168.x.x:32400. Windows can reject local
HTTPS when the certificate is not trusted or does not match the IP address.
You can test a token directly with:

    /library/sections?X-Plex-Token=YOUR_TOKEN

on the same server URL.

Edits save the moment you change them -- no need to bounce the game.
Do not redistribute your private Plex token, runtime config, cookies, or logs.


Pulling it back out
~~~~~~~~~~~~~~~~~~~

Two things to remove from the FH6 install folder: version.dll, and the
fh6-radio/ folder sitting next to it. After that, hit "Verify integrity
of game files" (Steam) or "Repair" (Xbox app / MS Store) and the patched
game assets get pulled back to vanilla.


License
~~~~~~~

This project is a modified version of FH6 Universal Radio by g0ldyy.
Attribution and modification details are in NOTICE.txt.

This project is distributed under GPLv3. The full license text is in the
included LICENSE file.

Keep these files together when redistributing a binary build:

    LICENSE
    README.ko.md
    NOTICE.txt
    CHANGES.txt
    SOURCE_CODE.txt
    THIRD_PARTY_NOTICES.txt
    CORRESPONDING_SOURCE.zip

CORRESPONDING_SOURCE.zip is generated from the matching source tree during
build. If you modify and redistribute the binary, regenerate that source
archive from the modified source.

AI assistance
~~~~~~~~~~~~~

Codex 5.5 model was used as an AI development assistant while adapting and
packaging this Plex-focused version.

Nothing here is affiliated with, endorsed by, or connected to Turn 10
Studios, Playground Games, Xbox Game Studios, Microsoft, Plex, or Forza.
All trademarks belong to their respective owners. Provided as-is, no
warranty, use at your own risk.
'@
Set-Content -Path (Join-Path $dist "README.txt") -Value $readme -Encoding utf8

function New-CorrespondingSourceArchive {
    param(
        [Parameter(Mandatory = $true)] [string] $Root,
        [Parameter(Mandatory = $true)] [string] $Dist
    )

    $stage = Join-Path $Dist "corresponding-source"
    $archive = Join-Path $Dist "CORRESPONDING_SOURCE.zip"
    $excludedNames = @(".git", ".github", ".vs", "build", "dist", "node_modules")

    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    if (Test-Path $archive) { Remove-Item -Force $archive }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    Get-ChildItem -LiteralPath $Root -Force | Where-Object {
        $excludedNames -notcontains $_.Name
    } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage $_.Name) -Recurse -Force
    }

    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $archive -Force
    Remove-Item -Recurse -Force $stage
}

New-CorrespondingSourceArchive -Root $root -Dist $dist

Write-Host "`nBuilt + staged in $dist" -ForegroundColor Green
Get-ChildItem -Recurse -File $dist | ForEach-Object {
    "  $($_.FullName.Substring($dist.Length + 1))"
}
