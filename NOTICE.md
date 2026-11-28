# Notice

## A fan project

Silkroad Reimagined is an independent, non-commercial **fan project**. Its aim is to keep Silkroad Online playable on
modern hardware: a new client and new servers, written from scratch. Today it runs on Windows with Vulkan; Linux and
macOS are planned, through more backends of the engine's rendering layer (OpenGL and Metal).

It is **not affiliated with, endorsed, sponsored or approved by** Joymax, any company affiliated with it, or any other
holder of rights in Silkroad Online. "Silkroad Online" and the related names and marks belong to their owners and are
used here only to say which game this project works with.

## No original game files or data

This repository contains **no** original client, server, database, art, sound, text or other files of Silkroad Online,
and none may be added to it: not whole files, not parts of them, not data extracted from them, not copies of their
code.

Players and server operators bring their own copies, which the programs read where they are. Each user is responsible
for having the right to use the copies they use. The source license below covers only the work of this project's
contributors; it grants no rights to the original game, its files, its data or its trademarks.

## License

Silkroad Reimagined is free software under the **GNU Affero General Public License, version 3 or (at your option) any
later version** ([LICENSE](LICENSE), `AGPL-3.0-or-later`).

In plain words: you may use, study, change and share it, including in forks. Anyone who distributes a changed version,
**or runs a changed version as a server that players use**, must make its complete source available under the same
license. Forks stay open source, servers included.

## No warranty, no liability

The software is provided **as is**, without warranty of any kind, and the contributors are not liable for any damage
arising from its use (sections 15 and 16 of the license). In particular, nothing here promises that the programs, their
network communication, the accounts they hold or the data they store are secure or fit for any purpose. Whoever runs a
server is responsible for running it safely, for its players' data, and for the laws that apply where they run it.

## Made mostly by AI, reviewed by people

Most of this project's code and documentation was written by an AI model (Anthropic's Claude) under human direction,
and reviewed and tested by people. See [CONTRIBUTING.md](CONTRIBUTING.md) for how that works for contributions.

## Third-party software

The libraries the build fetches (SDL3, Dear ImGui, Vulkan Memory Allocator, OpenAL Soft, Asio, OpenSSL, libpq,
cpp-httplib, nlohmann/json, stb_truetype, FreeType, Tracy, doctest, GLM and others) remain under their own licenses; their
versions are pinned in `cmake/`. Anyone distributing binaries must carry the notices those licenses require.
FreeType is used under the FreeType License: portions of this software are copyright © The FreeType Project
(www.freetype.org). All rights reserved.
