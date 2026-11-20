# AGENTS.md

How to work in this repository: for AI coding agents first, and for every person too. Read it whole before the first
change. [CONTRIBUTING.md](CONTRIBUTING.md) has the project's rules for contributions; [NOTICE.md](NOTICE.md) what the
project is and is not; [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) the builds, their switches and the developer tools
(what a players' build leaves out: the developer tools and commands never reach players or a public shard).

## What this is

A new client and new servers for **Silkroad Online (vSRO 1.188)**, written from scratch in C++20: the original game's
rules, numbers, timings and feel, **built our way**: multithreaded, modern, and better optimised. The client reads the
player's own copy of the original client's files; the servers read the operator's own copy of the original's database
backup and quest script (`SroSetup`). The repository holds none of those files.

## Never

- **Never add original game files or data** to the repository: archives, extracted files, database rows, scripts,
  executables, tables of values copied out of them.
- **Never paste decompiled or disassembled code**, or code from other Silkroad projects, or code whose license cannot
  become AGPL-3.0-or-later. Write our own.
- **Never leave a reverse-engineering trail in the source**: no addresses, no disassembler function names, no references
  to decompiled output or private notes. Comments say what the game does, in our words.
- **Never run the original's executables** (client, game server, shard manager, ...). Static analysis only.
- **Never guess the original's behaviour.** See the next section.
- **Never bypass the architecture below** to make something work sooner: no ad-hoc threads, no loading on the game or
  render thread, no server-to-server connections, no Vulkan outside `engine/rhi`.

## When the game is missing something

A feature, a window, a rule, a number we do not have yet. Do not invent it and do not guess from memory or from other
projects: **find out from the original yourself**, then build it our way.

1. **The data first.** Most answers are in the files: the client's text tables and layouts (read with `pk2tool` or the
   engine's loaders from your own vSRO 1.188 client), the original's database (your own `SRO_VT_SHARD.bak`, through
   `SqlServerBackup`), the quest script. Measure a hypothesis over every file of its kind before relying on it.
2. **Then the original programs, statically.** Use your own copy of the vSRO 1.188 client and server executables in a
   disassembler (static analysis only, never run). Find the behaviour: its states, its numbers, its order of checks, its
   messages to the player.
3. **Write down what you learned in our words**, where the code that uses it lives: what happens and why. Keep your own
   notes, addresses and disassembly **outside this repository**.
4. **Tables of numbers are data.** A table of values the original uses (rates, curves, limits) may be taken as it is,
   written as our own constant with a comment saying what it is (not where in a binary it was found).
5. **Then build it the way this codebase builds things** (the next sections): on the job system, through our protocol,
   working across two game servers, tested.
6. If you cannot find the answer, **say so**: stop and ask, or mark the code and the pull request plainly as unverified.
   A guess that looks like fact is worse than a gap.

The original is the reference for **what** the game does. It is never the model for **how** our code does it.

## The architecture (keep it in mind in every change)

### Threads: the job system, always

- All parallel work goes through the job system (`engine/core` `Jobs.h`: work stealing, priorities, counters,
  `ParallelFor`). No `std::thread`, no `std::async`, no thread of your own.
- The client runs a **game thread** and a **render thread**; they meet only in the frame snapshot
  (`client/src/frame`). Nothing loads on either: files, decoding and uploads go through the asset store's jobs and come
  back when ready.
- A game server's world runs its zones on jobs at the tick rate; database work goes to its database workers and comes
  back through the loop's completions. Nothing waits on the database inside a tick.
- Work that touches shared state says which thread owns it. When unsure, make it a job with a clear owner.

### Layers

- `game/` is the game's rules and the protocol, shared by the client and the servers. It **never** depends on
  rendering, audio, UI, the window or the platform. `cmake/DependencyRules.cmake` stops the configure if a layer rule is
  broken; do not loosen it.
- Only `engine/rhi` sees Vulkan; only `engine/platform` sees SDL; only `engine/audio` sees OpenAL; only `engine/net`
  sees Asio and OpenSSL. Linux and macOS are planned with OpenGL and Metal backends of `engine/rhi`: **no Vulkan type,
  call or assumption above `rhi`**, and nothing Windows-only outside a platform layer.
- The servers never depend on rendering, audio, UI or the window.

### The network: ours, not the original's

- The client talks only to **our** servers over **our** protocol: frames of a u32 length and a u16 id, reliable and
  latest-only lanes, TLS 1.3 (`engine/net`). The original's packets are not used.
- Each feature has its own message id range in its `game/include/sr/game/*Protocol.h`; a new message goes in its
  feature's range. A change an older peer cannot read raises `kProtocolVersion` (`game/Protocol.h`).
- **The server decides.** A message is a request, checked against the server's own state: never trust a position, a
  distance, a price, a count, an index or an id from a client. Every string is UTF-8 within its limit; every index is
  bounds-checked; anything costly is rate-limited. A handler must not be able to crash the server.
- Views by radius and tiers; what is sent is what the player can see.

### The servers: a shard of game servers

- A shard runs **one or more game servers that split the world by area** (`deploy/*.cfg`, `ShardLayout`). Every test
  and every development run of a server feature has **two** game servers up.
- **Anything players share** (chat, parties, guilds, unions, trades, friends, letters, events, ...) is done only when it
  works with the players on different game servers: a case in `tests/CrossServerTests.cpp` and a live run with a client
  on each.
- Game servers **never connect to each other**. They meet in the database: shared state in tables, messages over the
  database's LISTEN / NOTIFY, one channel per feature.
- The database keeps the original's layout (its table and column names in lower case, one schema per original
  database), with **our own queries** (never the original's procedures) and our own extra state in `sr_` tables. Player
  values reach SQL only as parameters, never formatted into the text.

### Modernising

Match the original's behaviour; do not copy its limits. Prefer the design that scales and stays readable: data-oriented
where it is hot, streamed instead of loaded up front, measured (Tracy zones: `SR_ZONE`) instead of assumed. When the
original does something one way because of 2008 hardware or a single-threaded server, do it the modern way and keep the
result the player sees the same.

## Style

- C++20. Namespace `sr::<module>`. PascalCase types and functions, camelCase locals, a trailing `_` for members.
- Errors are returned as `bool` plus `std::string& error`; no exceptions across module boundaries.
- Comments are full sentences in plain words: what the code does and why, the game's rule it follows. Each source file
  that reads a format documents that format's layout.
- Follow the code around you: match its structure, naming and comment voice before adding your own.
- Code in `engine/` and `game/` is written with Claude Opus only ([CONTRIBUTING.md](CONTRIBUTING.md)).

## Done means checked

Every task has a concrete check. Run it before saying it is done, and say what you ran.

```
cmake --preset clang
cmake --build --preset debug
.\build\clang\tests\Debug\sr_tests.exe
```

- New logic has a doctest case; a new format parser has a case that decodes **every** file of that type in the game.
- Rendering: `SroReimagined.exe --validation --capture out.png` renders, saves a picture and exits (exit code 2 if the
  Vulkan validation layers reported anything); look at the picture, and expect "validation messages: 0".
- Servers: the change works with two game servers and a client on each when players share it.
- Then a person tests it in the game ([CONTRIBUTING.md](CONTRIBUTING.md)).

Report the outcome as it is: if a test fails, say so with its output; if a step was skipped, say which and why.

## Where things are

| Folder | What is in it |
|---|---|
| `engine/core` | jobs, log, settings, profiling zones, memory-mapped files, text conversion |
| `engine/assets` | the game's file formats: `.pk2` archives, textures, models, animations, terrain, navigation meshes, interface layouts, text tables |
| `engine/net` | our protocol: frames, lanes, TLS connections |
| `engine/rhi` | the rendering hardware interface (Vulkan 1.3 today) |
| `engine/gfx`, `engine/render`, `engine/anim` | the renderer, the older renderer still used by its tests, CPU skinning |
| `engine/ui`, `engine/platform`, `engine/audio`, `engine/devui` | the game's own UI layer, the window and input (SDL3), sound (OpenAL), developer tools (ImGui) |
| `game/` | the rules and the protocol, shared by client and servers |
| `client/` | the client: screens, the interface rebuilt from the original's layouts, the world |
| `server/common` | the servers' shared library: the database, the stores, the original's backup reader and import |
| `server/{gateway,login,game,control,web,setup,dbtool,bots,skilllab}` | the programs |
| `tests/` | doctest cases; those needing game files, a database or the original's backup skip without them |
| `deploy/` | the shard's settings files |
