# Contributing

Thank you for helping keep Silkroad Online alive. This page says how work gets into the project. Read it, the
[README](README.md) and [AGENTS.md](AGENTS.md) (the engineering rules: they apply to people as much as to AI agents)
before you start.

## How this project is made

Most of the code is written by an AI model, **Anthropic's Claude Opus**, under human direction, and every change is
reviewed and tested by people. That shapes the rules below:

- **Code in `engine/` and `game/` is written with Claude Opus only.** One model keeps one style across the engine; code
  from other AI tools is not accepted there. Elsewhere (the client's screens, the servers, tools, tests, docs) other
  tools and hand-written code are welcome, as long as they follow [AGENTS.md](AGENTS.md).
- **Every change is tested by a person once it is finished.** Passing tests are not enough: someone runs the client and
  the servers and checks the change in the game (see "Done" below).
- **You answer for what you submit**, whoever or whatever wrote it: you have read it, you understand it, and you can
  explain it in review.

## The rules that are never broken

1. **No original game files or data in the repository.** No `.pk2`, no extracted files, no database backups or exports,
   no `Quest.sct`, no executables, no screenshots of the original's text tables, no tables of values copied out of them.
   Users bring their own copies ([NOTICE.md](NOTICE.md)).
2. **No code that is not ours.** No decompiled or disassembled code, from the original game or anything else; no code
   copied from other Silkroad projects (whatever their license); no code pasted from places whose license does not let
   it become AGPL.
3. **No reverse-engineering trail in the source.** Behaviour learned by studying the original is written in our own
   words: no addresses, no function names from a disassembler, no references to decompiled output or to private notes.
   A comment says what the game does and why our code does it so, not where in a binary that was found.
4. **Never guess what the original does.** If you do not know, find out (AGENTS.md "When the game is missing
   something"), or say plainly in the pull request that it is unverified.
5. **Never run the original's server or client executables** as part of working on this project; study them statically
   only. Server packs found online have carried malware.

## Before you start

- For anything bigger than a small fix, open an issue first and say what you plan, so work is not done twice and the
  design is agreed before code is written.
- Build and run the tests ([README](README.md#building)). Set `SR_GAME_DIR`, `SR_DB_URL` and `SR_VSRO_DATA` so the tests
  that need game files, a database and the original's backup run too.

## Done

A change is done when all of these hold:

- **It builds** with no new warnings (`cmake --build --preset debug`), and the server release build still builds
  (`cmake --preset server-release; cmake --build --preset server-release`).
- **The tests pass**, and the change has its own: a doctest case for new logic; a case that decodes every file of a type
  for a new format parser; a case in `tests/CrossServerTests.cpp` for anything players share.
- **A person has tested it in the game.** For anything players share (chat, parties, guilds, trades, friends, ...):
  with two game servers of one shard up and a client on each. For rendering: the Vulkan validation layers report nothing
  (`SroReimagined.exe --validation`). Say in the pull request what was tested and how; add screenshots for anything
  on screen.
- **The documentation says what changed**: the README or `docs/` for anything players, operators or contributors see.

## Pull requests

- One topic per pull request; small is better.
- Describe what it does, why, how it was tested, and which AI model (if any) wrote which part.
- Keep commits readable: a commit message says what changed and why, in plain words.
- Review comments are answered in the pull request; a reviewer may ask for a live test of their own.

## Security problems

Do not open a public issue for a security problem (a way to crash a server, take another player's items, read
someone's account, ...). Report it privately through GitHub's "Report a vulnerability" on this repository, with the
steps to reproduce it. It is fixed before it is made public.

## License of contributions

What you contribute is licensed under the project's license, AGPL-3.0-or-later ([LICENSE](LICENSE)). By opening a pull
request you confirm that you have the right to contribute it under that license.
