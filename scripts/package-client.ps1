# Makes the players' client package from the client-release build: a folder and a .zip with SroReimagined.exe and the DLLs
# it needs (the Visual C++ runtime beside it, so nothing has to be installed). The shard it plays on (its gateway and
# its servers' certificate) is built into the exe at configure time (README "Building the client for players"):
#
#   scripts\dev.cmd cmake --preset client-release -DSR_PLAYER_GATEWAY=HOST:15779 -DSR_PLAYER_CERTIFICATE=PATH\server.pem
#   scripts\dev.cmd cmake --build --preset client-release --target SroReimagined
#   powershell -ExecutionPolicy Bypass -File scripts\package-client.ps1
#
# The game folder is not in the package: the client asks the player for it on the first start (src/PlayerStart.h).
param(
    [string]$Name = 'SroReimagined',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build\client-release\client\Release'
$exe = Join-Path $build 'SroReimagined.exe'
if (-not (Test-Path $exe)) { throw "No client-release build: $exe" }

$version = (Get-Item $exe).VersionInfo.ProductVersion
if (-not $Out) { $Out = Join-Path $root 'build\client-release\package' }
$folder = Join-Path $Out "$Name-$version"
if (Test-Path $folder) { Remove-Item $folder -Recurse -Force }
New-Item -ItemType Directory -Force $folder | Out-Null

# The exe and the DLLs the build put beside it (OpenAL, OpenSSL).
Copy-Item $exe $folder
Get-ChildItem $build -Filter *.dll | Copy-Item -Destination $folder

# The Visual C++ runtime from Visual Studio's redistributable folder (Microsoft allows it beside the program).
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory | Where-Object Name -match '^\d' | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' } | Select-Object -First 1
if (-not $crt) { throw 'The Visual C++ redistributable folder was not found' }
foreach ($dll in 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crt.FullName $dll) $folder
}

@"
# SroReimagined's settings (key = value). The game folder is added here on the first start; the server it plays on is
# built into SroReimagined.exe.
"@ | Set-Content (Join-Path $folder 'SroReimagined.cfg') -Encoding ascii

@"
SroReimagined $version
=====================

An unofficial, open-source client for Silkroad Online, played on the files of your own vSRO 1.188 client.
A fan project: not connected to Joymax or any company of Silkroad Online.

Starting
  1. Unpack this folder anywhere you like (not inside your Silkroad folder).
  2. Start SroReimagined.exe. The first time it asks for your vSRO 1.188 client's folder (the one with Media.pk2).
  3. Log in with an account from the website.

  Another game folder later: hold Shift while you start SroReimagined.exe (or delete the game line in
  SroReimagined.cfg). It also asks again by itself when the folder no longer holds the game.

Needs
  Windows 10 or 11 (64-bit) and a graphics card with Vulkan 1.3 (current drivers).

Files
  SroReimagined.cfg   your settings (the game folder)
  SroReimagined.log   what happened in the last run: send it along with a bug report

License: AGPL-3.0-or-later. It is free software: its source code is open to everyone.
"@ | Set-Content (Join-Path $folder 'README.txt') -Encoding ascii

$zip = "$folder.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip
Write-Host "Package: $folder"
Write-Host "Zip:     $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
