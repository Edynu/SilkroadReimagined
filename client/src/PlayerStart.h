#pragma once

// The players' build (SR_PLAYER_BUILD): how the client starts when a player opens it rather than a developer's command
// line. The exe's own folder is the working folder (its settings, the options file, the log and the cursors live beside
// it); the shard it plays on, its gateway and its servers' certificate, is built in (PlayerServer.h); the title screen
// follows the logo; the original game's folder is asked for on the first start (a folder picker), checked and
// remembered in SroReimagined.cfg, and again whenever it no longer holds the game's archives or the player holds Shift
// while the game starts (or passes --choose-game). There is no console: what goes wrong is shown in a message box and
// written to SroReimagined.log.

#include <string_view>

#include "sr/core/Config.h"

namespace sr::client {

// The exe's folder as the working folder; call first, before the settings are read.
void UseExeFolder();
// The player build's defaults (only where SroReimagined.cfg and the command line say nothing), then the game folder:
// false when there is none (the player cancelled the picker; already told).
bool PreparePlayerStart(core::Config& config);
// A message box (the players' build has no console).
void ShowStartError(std::string_view text);

} // namespace sr::client
