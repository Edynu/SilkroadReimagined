#pragma once

// !!! BETA ONLY: TO BE DELETED IN FULL BEFORE THE RELEASE (the user's call, 2026-11-19) !!!
// These quests are hard-coded for the beta period. Before a release remove all of it: this file and BetaQuests.cpp, the
// `beta-quests` / `beta-helpers` settings and --beta-quests, the Beta Helper spawns (server/game/src/main.cpp), the quests'
// loading (GameServer.cpp "The beta quests"), and the tests "the beta quests". Keep or remove the general pieces it brought
// (start record, reward bound, sr_bound_items, sr_char_records, the custom NPC name) as the release needs.

// The beta quests (ours, the user's design 2026-11-18): a test shard's way to hand its
// testers the equipment the late systems need. On a game server started with `beta-quests` (the shard's settings or
// --beta-quests) a Beta Helper stands beside So-Ok in each town; its ten quests, each once a character, pay a Seal of Sun
// weapon of a degree, the player's pick of the 14, bound to the character:
//   degrees 2..7  after 3, 10, 25, 50, 75 and 100 different quests finished (the beta ones not counted);
//   degree 8      after 10 Capture the Flag or Battle Arena matches played (the record `event-matches`);
//   degree 9      after the Roc's defeat witnessed in the Peak (`roc`);
//   degree 10     after the Pharaoh's tomb cleared at its hardest (`tomb-hardest`) or 5 temple quests (QNO_SD_GU_*) finished;
//   degree 11     after a fortress war fought (`fortress-war`).
// A custom NPC is one the server names: its codename the name's, upper case, spaces as underscores, after NPC_ (the
// client finds its quests' talk by it; its look a model of the game's, So-Ok's for the Beta Helper).

#include <string>
#include <string_view>
#include <vector>

namespace sr::game {

inline constexpr std::string_view kBetaHelperName = "Beta Helper";
inline constexpr std::string_view kBetaQuestPrefix = "QBETA_";

// "Beta Helper" -> "NPC_BETA_HELPER".
std::string CustomNpcCodename(std::string_view name);

// The ten quests' definitions in our format, one text.
std::string BetaQuestDefinitions();

// The 14 Seal of Sun weapons of a degree (2..11): China's five, then Europe's nine.
std::vector<std::string> SealOfSunWeapons(int degree);

} // namespace sr::game
