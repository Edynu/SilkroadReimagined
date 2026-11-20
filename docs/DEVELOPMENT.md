# Developing SroReimagined

How the developers' builds differ from the players', what the developer tools are and how to use them. For building
from scratch see the [README](../README.md#building); for running a shard, [SERVER_SETUP.md](SERVER_SETUP.md); for the
rules every change keeps, [AGENTS.md](../AGENTS.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).

## The builds

| Preset | Compiler | For | What is in it |
|---|---|---|---|
| `clang` (`debug`, `release`) | clang + lld | developers | everything: the client with the developer tools, the servers with the developer commands, the tests, the tools |
| `msvc` (`msvc-debug`, `msvc-release`) | MSVC | developers | the same as `clang`, built with Visual Studio's compiler (run through `scripts\dev.cmd`) |
| `client-release` | MSVC | players | the client alone: no developer tools, no profiler, no console; one shard built in (README "Building the client for players") |
| `server-release` | clang | a public shard | the servers alone: no developer commands, no test seeds, no tests |

The switches behind them (CMake options; a preset sets them, `-D` overrides them):

| Option | Default | What it does |
|---|---|---|
| `SR_DEV_TOOLS` | ON | The client's developer tools: the ImGui windows, F1 and F5. Off: none of them, and no screen draws its ImGui panel. |
| `SR_DEV_COMMANDS` | ON | The game server's developer commands (teleports, items, experience, the event switches, the test seeds). Off: the server has no way to turn them on, whatever its settings say; the tests need them, so they are left out too. |
| `SR_PLAYER_BUILD` | OFF | The players' client: a window program, everything beside the exe, the game folder asked for, the shard built in (`SR_PLAYER_GATEWAY`, `SR_PLAYER_CERTIFICATE`). |
| `SR_TRACY` | ON | Tracy profiler zones, sent only while a Tracy 0.14.1 profiler is connected. |
| `SR_BUILD_CLIENT`, `SR_BUILD_SERVERS`, `SR_BUILD_TESTS`, `SR_BUILD_TOOLS` | ON | Which parts are built. |

A public shard runs `server-release` and players run `client-release`: a developer's build is never what players or a
public server get.

## The developers' client

`build\clang\client\Debug\SroReimagined.exe` (or `RelWithDebInfo`). Its settings come from `SroReimagined.cfg` in the
working folder (`key = value` lines), then the command line (`--key value`, `--flag`); every option is listed at the top
of `client/src/main.cpp`. The ones used every day:

| Option | What it does |
|---|---|
| `--game DIR` (or `SR_GAME_DIR`) | the vSRO 1.188 client's folder |
| `--gateway HOST:PORT` | the shard's gateway (default `127.0.0.1:15779`, the dev shard) |
| `--server-certificate FILE` | the servers' certificate to trust (default `dev\server.pem`) |
| `--screen NAME` | the first screen: `logo`, `title`, `world`, `dungeon`, `viewer`, `crowd`, `gallery`, `scenebench`, ... |
| `--tools false` | start with the developer tools hidden |
| `--validation` | Vulkan's validation layers (exit code 2 when they reported anything) |
| `--capture FILE.png` | render, save a picture and exit (what automated checks look at) |

A developer's client reaches the title screen through `--screen title` (the default goes on from the logo to the crowd
benchmark).

### The developer tools (ImGui)

The developer tools are drawn with [Dear ImGui](https://github.com/ocornut/imgui). ImGui is for developers only: the
game's own interface (the HUD, the windows, the title screens) is our own UI layer, `sr::ui` (`engine/ui`), built from the
original's layouts and drawn like the original's. A players' build has no ImGui window at all.

- **F1** shows or hides every developer window. They start shown (hidden with `--tools false` or during a capture).
- **F5** cycles the screens `crowd`, `empty`, `logo` (the resource leak check walks the same circle with `--screen-cycle`).
- **Alt+Enter** switches windowed and borderless: the game's own key, in every build.

While the mouse is over an ImGui window the game does not see it (and the cursor is ImGui's); window places are kept in
`sr_client.ini`.

The windows:

| Window | Where | What it shows |
|---|---|---|
| **Engine** | always (`client/src/devtools/EngineWindow.cpp`) | the render thread and the GPU's passes, the asset streaming, the game thread, the job system's workers, the devices, the mounted archives and every setting |
| **Network** | the game screen | the game connection live: bytes and messages a second each way, the totals, a walk's round trip, what the view holds by kind against what the server allows, the messages by kind |
| **Teleport (developer)** | the game screen | a teleport to a point, a place by the minimap's coordinates, an NPC or a monster's nest; works only with a server whose developer commands are on |
| **World** | the world screen | the regions streamed, the navigation, the weather, the camera, the characters, the effects, the game clock |
| **Dungeon** | the dungeon screen | the block shown, the fog, the camera |
| **Crowd** | the crowd benchmark | the characters, the simulation's steps and messages, a burst of spawns (B), the state hash |
| **Scene bench** | the scene benchmark | its scenes and their timings |
| **Assets**, **Effect** | the viewer | the archives' models and textures to open (with a filter), the open model's, texture's or effect's details |
| **Asset check** | `--asset-check` | every file of the asked types streamed, and which failed |
| **Interface gallery** | the gallery screen | every interface layout, opened as a window on sample data |

How the tools are made: ImGui draws through our renderer like everything else (`engine/devui`, its shader
`shaders/ImGui.hlsl`), on the game thread, into the frame's snapshot. A screen draws its own panel inside
`if (context.toolsVisible) { ... }` in its `Update`; the Engine window is drawn by `client/src/App.cpp`. A panel never
waits: what takes time runs as a job and the panel shows its result when it is there (`EngineWindow`'s archive count is
the pattern). Code that only a developer's build has goes inside `#ifdef SR_DEV_TOOLS`.

### The game's own interface

The HUD and the windows are `sr::ui` widgets (`engine/ui/include/sr/ui/Widget.h`), most of them built from the
original's layouts (`resinfo\*.txt`, `res_ui\*.2dt`) and registered by their class name (`client/src/gui/HudWindows.cpp`).
A window of our own is a widget made in code: the performance panel is the example (`PerformancePanel` in
`client/src/gui/HudWindows.h`, `HudPerformance.cpp`): a class registered under a name, opened by `Hud::Create` from a
node made in code, placed by `Hud::Place`, reading what it shows from the `HudContext` the game screen fills.

## The developer commands (the servers)

A developers' server build (`SR_DEV_COMMANDS` on) takes `--dev-commands` (or `dev-commands = true` in the shard's
settings): then the client's Teleport panel works and a GM's chat commands are on (`/siege`, `/flagworld`, `/arena`,
`/roc`, `/temple`, ...; every one in `server/game/src/main.cpp`'s header). `start-dev`-style runs use it; a public shard
never does, and its `server-release` build cannot.

## Measuring

- **Tracy**: `SR_ZONE("name")` marks a zone on any thread (`engine/core/include/sr/core/Trace.h`); connect the Tracy 0.14.1
  profiler to a running client or server.
- **The crowd benchmark** (release build): `--screen crowd --characters 1000 --run-seconds 10 --frame-limit -1 --vsync
  false --tools false` prints the frame, game-thread and render-thread statistics; `--run-seconds S` does the same on any
  screen.
- **The performance panel** in the game (every build): the frames a second, the frame times, the latency and the
  traffic.

## Tests

`build\clang\tests\Debug\sr_tests.exe` (doctest; `--test-case="*name*"` picks some). The tests that need the game files,
a database or the original's data skip without them: the environment variables are in the README.
