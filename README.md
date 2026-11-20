# Silkroad Reimagined

A new, open-source client and server for **Silkroad Online**, written from scratch in C++20:

- **The client** (`SroReimagined.exe`): our own engine on Vulkan 1.3, multithreaded, with the game's original
  interface rebuilt from its layout files. It reads the game's art, maps and text from your own copy of the vSRO 1.188
  client.
- **The servers**: gateway, login, game servers and the website's API, talking to the client over our own protocol on
  TLS 1.3, with their state in PostgreSQL. A shard runs one or more game servers that split the world between them.

The aim is the original game's rules, numbers and feel, built our way: a modern engine, a server that can grow with its
players, and code anyone can read and improve.

> **An unofficial fan project.** Silkroad Reimagined is not affiliated with, endorsed or sponsored by Joymax or any
> other rights holder of Silkroad Online. Product names and trademarks belong to their owners. It comes with no warranty
> and no liability, its network security included. See [NOTICE.md](NOTICE.md).

It runs on Windows with Vulkan today; Linux and macOS are planned (OpenGL and Metal backends of the engine's rendering
layer). Most of the code is written by an AI model (Claude Opus) under human direction and reviewed and tested by
people ([CONTRIBUTING.md](CONTRIBUTING.md)).

## What you need: your own original files

This repository contains **no original game files and no original data**: no client, no server, no database, no art.
Everyone brings their own copy, and the programs read it where it is (never changing it).

| Who | Needs | Where it is read |
|---|---|---|
| **Players** | a **vSRO 1.188** client folder (its `Media.pk2`, `Data.pk2`, `Map.pk2`, ...) | the launcher asks for the folder once |
| **Server operators** | the same **vSRO 1.188** client folder (the game servers read its maps and tables) | `game = ...` in the shard's settings |
| | **`SRO_VT_SHARD.bak`**, the original vSRO 1.188 server's shard database backup (SQL Server) | `data/` (read by `SroSetup`) |
| | **`Quest.sct`**, the original server's compiled quest script (`Script\VIETNAM\Quest.sct` in the server files) | `data/` (read by `SroSetup`) |

The client folder and the server files must be of the same version, **vSRO 1.188**: the database's ids must match the
client's. `SroSetup` checks this and tells you if they do not.

We do not provide these files and cannot tell you where to get them. You are responsible for having the right to use
the copies you use.

## Getting started

- **Play:** download the launcher from your server's website, point it at your vSRO 1.188 client folder, and play.
- **Run a server:** [docs/SERVER_SETUP.md](docs/SERVER_SETUP.md) takes you from an empty machine to a running shard:
  PostgreSQL, the settings file, `SroSetup` (it reads your `.bak` and `Quest.sct` into the database), then the servers.
- **Build from source:** below.
- **Contribute:** [CONTRIBUTING.md](CONTRIBUTING.md). AI agents start at [AGENTS.md](AGENTS.md).

## Building

Windows 10 or 11, x64. Install:

- **CMake 3.29+** and **Ninja**
- **LLVM** (clang++ and lld), and Visual Studio 2026 (or 2022) with "Desktop development with C++" for the MSVC
  presets (the players' client is built with it)
- **Vulkan SDK 1.3+** (the client's shaders are compiled with its DXC)
- **OpenSSL 3** (`C:\Program Files\OpenSSL-Win64` is found by itself)
- **PostgreSQL 14+** (the servers link its `libpq`; its installer's default folder is found by itself)

The other libraries (SDL3, Dear ImGui, VMA, OpenAL Soft, Asio, doctest, ...) are fetched by CMake at pinned versions.

```
cmake --preset clang                 # configure (clang++ + lld + Ninja), from a plain shell
cmake --build --preset debug         # or --preset release
.\build\clang\tests\Debug\sr_tests.exe
```

The servers for a public server are built without the developer commands:

```
cmake --preset server-release
cmake --build --preset server-release
```

The client for players (the developers' client is in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)) is described next.

### Building the client for players

A players' client is built for one shard: the address it connects to and the certificate it trusts are built into the
exe, so neither a settings file nor the command line can point it anywhere else. It is built with MSVC (Visual Studio's
"Desktop development with C++"), without the developer tools: a window without a console, no ImGui window, no F1 or
F5, the logo and then the title screen.

```
scripts\dev.cmd cmake --preset client-release -DSR_PLAYER_GATEWAY=play.example.com:15779 -DSR_PLAYER_CERTIFICATE=C:\SroServer\bin\dev\server.pem
scripts\dev.cmd cmake --build --preset client-release --target SroReimagined
powershell -ExecutionPolicy Bypass -File scripts\package-client.ps1
```

CMake keeps the values: later builds need only the second and third lines. The package
(`build\client-release\package\SroReimagined-<version>.zip`) holds the exe, its DLLs and the Visual C++ runtime, so
players install nothing. A new version: `-DSR_CLIENT_VERSION=0.1.1` on the first line (the exe's version resource and
the package's name).

**The shard it plays on** (the configure line):

| Setting | What it is |
|---|---|
| `SR_PLAYER_GATEWAY` | The gateway players connect to first, `host:port` (shard.cfg's `gateway-port`; the host the public address of the front machine or a DNS name for it). The gateway then sends players on to the login server (`login-host`) and the game servers (`server.N.public-host`): those come from the shard's settings, not from the client. If the site is behind Cloudflare, the game's name must be a DNS-only record: Cloudflare's proxy carries web traffic, not the game's. |
| `SR_PLAYER_CERTIFICATE` | The shard's certificate, `server.pem`: the servers make it on their first start (`bin\dev\server.pem` beside them) for the name in shard.cfg's `tls-name`. The client trusts this certificate and no other, so it cannot be sent to another server pretending to be yours. Only the certificate goes into a build: `server.key` beside it is the servers' private key, it never leaves the server machine (the build refuses a file that holds a private key). |
| `SR_PLAYER_SERVER_NAME` | The name the certificate was made for (`tls-name`; default `localhost`, as the servers make it by default). |

A new certificate (made again on the server, or a real one later) means a new client build: the old clients stop
connecting to the shard, which is what a certificate change is for.

**The player's game folder.** SroReimagined plays on the player's own vSRO 1.188 client files (the five archives:
`Media.pk2`, `Map.pk2`, `Data.pk2`, `Music.pk2`, `Particles.pk2`); nothing of the original game is in the package.

- On the first start the client asks for the folder with Windows' own folder picker, checks the five archives are there
  and keeps the folder in `SroReimagined.cfg` beside the exe (`game = ...`).
- It asks again by itself when that folder no longer holds them (moved or uninstalled).
- To choose another folder: hold **Shift** while starting SroReimagined.exe (Cancel keeps the current one), start it
  with `--choose-game`, or delete the `game` line from `SroReimagined.cfg`.
- A folder name outside ASCII is kept by its short (8.3) Windows name.

**Beside the exe** the client keeps its settings (`SroReimagined.cfg`), the player's options (`sro_options.cfg`, the
Option window's), its log (`SroReimagined.log`: what a bug report needs), the mouse cursors it takes from the player's
`sro_client.exe` on the first start (`cursors\`, the original's art, never in the package) and the interface textures it
warms up (`sro_ui_warmup.txt`). It never writes into the game folder.

**The performance panel.** At the top right of the game, left of the minimap: the frames a second and the latency (a
walk's round trip to the game server); a click on it opens the frame times and the connection's traffic. It is our own
interface, drawn like the game's windows (no ImGui), in every build.

**Windows' warnings.** Until the client is signed, Windows SmartScreen shows "Windows protected your PC" for a new
download: "More info", then "Run anyway". Signing it (a code-signing certificate) is the cure.

Some tests need more, and skip without it:

- `SR_GAME_DIR`: a vSRO 1.188 client folder (the game files' tests);
- `SR_DB_URL`: a PostgreSQL database whose user may create test databases beside it (the servers' tests);
- `SR_VSRO_DATA`: the folder with your `SRO_VT_SHARD.bak` and `Quest.sct` (the backup reader's, the setup's and
  the quests' tests); `SR_QUEST_SCRIPT` names the `Quest.sct` when it lies elsewhere;
- `SR_VSRO_EXPORT`: an export of the same backup made through SQL Server (`<db>/schema.tsv`, `<db>/data/<table>.tsv`):
  the reader is checked against it row by row, and the tests that need the original's tables in a database import it.

## The programs

| Program | What it is |
|---|---|
| `SroReimagined.exe` | the client |
| `SroSetup.exe` | a server's one-time setup: makes the database's tables and imports your `.bak` and `Quest.sct` |
| `SroControl.exe` | the front machine: gateway, login and the shard's watch in one process |
| `SroGame.exe` | a game server (`--id N`); a shard runs one or more |
| `SroWeb.exe` | the website's API (registration, rankings, status, administration) |
| `SroDb.exe` | database tools for developers (migrations, counts) |
| `SroBots.exe` | the load test: bots that log in and walk |
| `pk2tool.exe` | inspects the client's `.pk2` archives |

## The source

| Folder | What is in it |
|---|---|
| `engine/` | the engine's modules: `core` (jobs, log, settings), `assets` (the game's file formats), `net` (our protocol on TLS), `ui`, `platform`, `rhi` (Vulkan), `gfx` (the renderer), `anim`, `audio`, `devui` (developer tools) |
| `game/` | the game's rules and the protocol, shared by the client and the servers; never depends on rendering, audio or UI |
| `client/` | the client: its screens, the interface, the world |
| `server/` | the servers and their shared library (`server/common`: the database, the stores) |
| `tests/` | the unit and integration tests (doctest) |
| `tools/` | `pk2tool` |
| `deploy/` | the shard's settings files |

## License

[AGPL-3.0-or-later](LICENSE): you may use, study, change and share this software. Anyone who distributes a changed
version, or runs one as a server that players use, must make its complete source available under the same license.
The license covers our code only; it grants no rights to the original game's files, data or trademarks
([NOTICE.md](NOTICE.md)).
