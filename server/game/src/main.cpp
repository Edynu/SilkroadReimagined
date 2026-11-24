// SroGame.exe - a game server. Options: sr/server/Program.h, and
//   --game DIR            the client's game files (default: SR_GAME_DIR): the creation rules
//   --id N                this server's id (default 1)
//   --server-name NAME    as listed (default Silkroad)
//   --group N --group-name NAME
//   --public-host HOST    the address players are sent to (default 127.0.0.1)
//   --capacity N          players this game server takes at most (default: the shard's capacity, 3000)
//   --weather-speed X     the weather's clock X times the wall clock (tries; every server of a shard alike; default 1)
//   --shard-capacity N    the shard's players, all its game servers together (default: the settings' shard-capacity, 3000)
//   --max-characters N    per account (default 3, the v1.150 client's)
//   --delete-days N       how long a deleted character can be restored (default 7)
//   --tick-rate N         the world's ticks a second (default 20)
//   --shard-schema NAME this shard's schema in the database (default sro_vt_shard)
//   --tables database|client  where items, characters, skills, teleports, shops, levels and quests come
//                         from: the database's original tables when it holds them (default; the
//                         client's rows only), or the client's text files
//   --dev-commands        take developer commands (a teleport anywhere): development only
//   --stats-seconds N     how often the world's ticks are logged (default 10; 0: never)
//   --zone-regions N      a zone's width in regions (default 16)
//   --workers N           job threads for the zones and the navigation (default: the machine's)
//   --net-threads N       network threads (default 2)
//   --view-radius R       how far a player sees, in the minimap's units (default 600; a region is 192)
//   --view-players N --view-monsters N --view-items N   each kind's share of a view (300, 150, 50);
//                         a share's unused places go to the others' nearest
//   --view-tier1 N --view-tier2 N   the nearest N hear of a leg at once, the next N every 2nd tick,
//                         the rest every 4th (150, 150); --view-keep F the edge's hysteresis (0.1)
// The rates (each 1 = the original's; in SroGame.cfg as "rate-exp = 2" or on the command line):
//   --rate-exp X --rate-sp X          experience and skill experience from kills
//   --rate-gold X --rate-drop-gold X  the gold of a drop, how often gold drops
//   --rate-drop-equipment X --rate-drop-sox X --rate-drop-material X --rate-drop-elixir X --rate-drop-potion X
//   --level-cap N                     the highest level (default 110)
//   --pk-prohibited                   the PK switch: no player attacks an innocent nor a hostile job (0x3025; free PvP and
//                                     guild wars stay); off by default
//   --beta-quests                     the beta quests (game/BetaQuests.h): a Beta Helper beside So-Ok in each town, ten
//                                     quests of bound Seal of Sun weapons; --beta-helpers "REGION,X,Y,Z[,HEADING];..." its
//                                     places instead; off by default
//   --monster-drops FILE  the drop tables from a file; by default they come from the database's original tables
//                         (SroSetup), else monster_drops.tsv next to it; without either monsters drop nothing
//   --monster-nests FILE  the monsters' nest policies from a file; by default from the database, else
//                         monster_nests.tsv next to it; without either every point holds one peaceful monster
// The fortress war (its fortresses from the database's reference):
//   --fortress-cycle R,G,W  ours, for tries: from the start, R s of applications, G s, then W s of war,
//                         again (instead of `_Schedule`); --dev-commands also takes FortressDev (the
//                         GM's /SIEGE STARTREQ, ENDREQ, STARTWAR, ENDWAR)
//   --fortress-structure-hp N  ours, for tries: every structure's most health (default: characterdata's)
//   --bandit-seconds A,B  ours, for tries: the bandits' waves each A + up to B seconds (the original's 60,60)
// Capture the Flag (the shard's settings or the command line):
//   --event-ctf  on (the event's switch); --ctf-times HH:MM,... the plays' starts (registration 10 minutes before)
//   --ctf-cycle R,P[,W]   ours, for tries: R s of registration, P s of play, W s for the winners (60), again
//   --ctf-min-players N   a match's least players (2); --dev-commands also takes FlagWorldDev (/FLAGWORLD OPEN|START|END|CANCEL)
// The Battle Arena: --event-arena on (the original's _Schedule 16..23), --arena-cycle R,P ours for
//   tries (random mode: R s of registration, P s of play); --dev-commands also takes ArenaDev (/ARENA OPEN [MODE]|START|END|CANCEL)
// The Roc: --event-roc (on: SCHEDULE_DEF_MEET_ROC), --roc-cycle O,P ours for tries (the gate open O s every
//   P s), --roc-time-scale N (the Peak's times N times faster); --dev-commands also takes /ROC OPEN|SHUT|END|BREAK
// The job temples: --event-temples (on: `_Schedule` 9..11), --temple-cycle P ours for tries (each pair's
//   period P s, the opening a quarter in); --dev-commands also takes /TEMPLE OPEN|CLOSE 9|10|11, /TEMPLE REP ANUBIS|HAROERIS TRADER|THIEF N
//   --seed-teams "Guild:Name,Name;Guild:Name,Name" [--seed-level N] [--seed-gold N] [--seed-sp N] [--password P] [--seed-kit archer] [--seed-woman]
//   --seed-character "ACCOUNT:NAME[:thief|hunter|trader],..." [--seed-level N] [--seed-gold N] [--seed-sp N]: characters for
//     trying things on an existing account (the job's league, an alias and its suit in the bag); then it exits
//     [--seed-owner GUILD] [--seed-fortress N]  accounts, characters and guilds for a try of the war; then it exits
// Giving items (then it exits; the character must not be in the game):
//   --give NAME --items "CODE[:count][,CODE[:count]...]" [--gold N] [--plus N] [--perfect-whites] [--durability N]
//     each itemdata codename into the first free bag places (a count above the stack's most makes
//     several stacks; equipment gets its new durability, --plus its plus, --perfect-whites every
//     white stat at its best)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <array>
#include <sstream>
#include <tuple>

#include "sr/game/BetaQuests.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/OpenMarket.h"
#include "sr/assets/FileSystem.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "sr/assets/GameData.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Guild.h"
#include "sr/game/Items.h"
#include "sr/server/Database.h"
#include "sr/game/Jobs.h"
#include "sr/server/GameServer.h"
#include "sr/server/DatabaseText.h"
#include "sr/server/GuildStore.h"
#include "sr/server/Program.h"
#include "sr/server/ReferenceData.h"
#include "sr/server/SilkStore.h"
#include "sr/server/Store.h"

namespace {

// --give: items and gold for a character not in the game.
int Give(const sr::core::Config& c, const sr::server::ProgramOptions& options, const sr::game::CharacterRules& rules) {
    using namespace sr;
    std::string error;
    if (!rules.items) {
        core::LogError("SroGame: --give: no item table");
        return 1;
    }
    std::vector<server::StoredItem> items;
    const std::string list = c.GetString("items", "");
    const int plus = std::clamp(c.GetInt("plus", 0), 0, 255);
    const bool perfect = c.GetBool("perfect-whites", false);
    const long long worn = c.GetInt("durability", -1); // -1: new
    for (std::size_t at = 0; at < list.size();) {
        std::size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        std::string spec = list.substr(at, end - at);
        at = end + 1;
        int count = 1;
        if (const std::size_t colon = spec.find(':'); colon != std::string::npos) {
            count = std::max(1, std::atoi(spec.c_str() + colon + 1));
            spec.resize(colon);
        }
        const assets::ItemRow* row = rules.items->Find(spec);
        if (row == nullptr) {
            core::LogError("SroGame: --give: no item {}", spec);
            return 1;
        }
        const bool equipment = game::NewDurability(*row) > 0 || game::KindOf(*row) == game::ItemKind::Accessory;
        server::StoredItem item;
        item.refId = row->refObjId;
        item.plus = std::uint8_t(equipment ? plus : 0);
        item.variance = equipment && perfect ? 0x0FFFFFFFFFFFFFFFull : 0; // twelve slots of 31
        item.durability = game::NewDurability(*row);
        if (equipment && perfect && item.durability > 0) item.durability = std::uint32_t(row->Stat(64)); // the best durability
        if (equipment && worn >= 0 && item.durability > 0) item.durability = std::uint32_t(std::min<long long>(worn, item.durability)); // worn down (tries of a repair)
        item.counted = game::DataIsCount(*row); // _items.data: a count, but equipment's durability (and a mask's monster)
        const int most = game::MaxStack(*row);
        for (int left = count; left > 0; left -= most) {
            item.quantity = std::uint16_t(std::min(left, most));
            items.push_back(item);
        }
    }
    const std::uint64_t gold = std::uint64_t(std::max<long long>(0, std::atoll(c.GetString("gold", "0").c_str())));
    const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);
    auto db = server::Database::Connect(server::WithSearchPath(options.databaseUrl, shard + ", " + server::kAccountSchema), error);
    if (!db || !server::MigrateSchema(*db, shard, error)) {
        core::LogError("SroGame: --give: database: {}", error);
        return 1;
    }
    const std::string name = c.GetString("give");
    switch (server::GiveItems(*db, std::uint16_t(c.GetInt("id", 1)), name, items, gold, error)) {
    case server::GiveResult::Done: core::LogInfo("SroGame: gave {} {} item stacks and {} gold", name, items.size(), gold); return 0;
    case server::GiveResult::NoCharacter: core::LogError("SroGame: --give: no character {}", name); return 1;
    case server::GiveResult::Online: core::LogError("SroGame: --give: {} is in the game; give when the account is out", name); return 1;
    case server::GiveResult::BagFull: core::LogError("SroGame: --give: {}'s bag has no room for all of it (nothing given)", name); return 1;
    case server::GiveResult::Failed: break;
    }
    core::LogError("SroGame: --give: {}", error);
    return 1;
}

// --give-silk NAME --amount N [--reason 2]: silk added to (N < 0: taken from) the account of the character or account NAME
// (SilkStore.h GrantSilk: the row, the original's logs, the account's bus line: a player of it on any game server hears
// it). Ours: the plan's offline grant until the website's administration grants silk.
int GiveSilk(const sr::core::Config& c, const sr::server::ProgramOptions& options) {
    using namespace sr;
    std::string error;
    const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);
    auto db = server::Database::Connect(server::WithSearchPath(options.databaseUrl, shard + ", " + server::kAccountSchema), error);
    if (!db || !server::MigrateSchema(*db, shard, error)) {
        core::LogError("SroGame: --give-silk: database: {}", error);
        return 1;
    }
    const std::string name = c.GetString("give-silk");
    server::QueryResult who = db->Query("SELECT u.userjid FROM _user u JOIN _char c ON c.charid = u.charid WHERE lower(c.charname16) = lower($1) "
                                        "UNION ALL SELECT jid FROM sro_vt_account.tb_user WHERE lower(struserid) = lower($1) LIMIT 1",
                                        {name});
    if (!who.Ok() || who.Rows() == 0) {
        core::LogError("SroGame: --give-silk: no character or account {}", name);
        return 1;
    }
    const std::int64_t amount = std::atoll(c.GetString("amount", "0").c_str());
    const int reason = std::clamp(c.GetInt("reason", int(server::SilkReason::GiftReceived)), 0, 5);
    server::SilkBalance after;
    const server::SilkGrant done = server::GrantSilk(*db, who.Int(0, 0), amount, server::SilkKind::Own, server::SilkReason(reason), "",
                                                     "SroGame --give-silk", after, error);
    if (done != server::SilkGrant::Done) {
        core::LogError("SroGame: --give-silk: {}", done == server::SilkGrant::Short ? std::string("too little silk") : error);
        return 1;
    }
    core::LogInfo("SroGame: {}'s account has {} silk now", name, after.own);
    return 0;
}

// --seed-teams "Guild:Name,Name,...;Guild:Name,...": teams for a try of the fortress war: an account for each name
// (the name in lower case, --password), a
// Chinese character of --seed-level with --seed-gold and --seed-sp, beside Jangan's official; the first name
// founds the guild, the others join it, the guild at the top level. What is there already is kept, so
// this runs before every try (and brings everyone back to Jangan). --seed-owner GUILD gives it Jangan
// (--seed-fortress), without an application standing from the last try. Only while the game servers
// are down: they read the guilds and the fortresses when they start.
[[maybe_unused]] int SeedTeams(const sr::core::Config& c, const sr::server::ProgramOptions& options, const sr::game::CharacterRules& rules) {
    using namespace sr;
    std::string error;
    const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);
    auto db = server::Database::Connect(server::WithSearchPath(options.databaseUrl, shard + ", " + server::kAccountSchema), error);
    if (!db || !server::MigrateSchema(*db, shard, error)) {
        core::LogError("SroGame: --seed-teams: database: {}", error);
        return 1;
    }
    const auto fail = [](std::string_view what, std::string_view detail) {
        core::LogError("SroGame: --seed-teams: {}: {}", what, detail);
        return 1;
    };
    // --seed-woman: women (a woman's body has a Bone01 of its own: the seal bow's glow test).
    const bool man = !c.GetBool("seed-woman", false);
    const std::vector<std::uint32_t> figures = rules.creation.FiguresOf(game::CreationRules::Race::China, man);
    const auto armour = rules.creation.ArmourOf(game::CreationRules::Race::China, man);
    const std::vector<std::uint32_t> weapons = rules.creation.WeaponsOf(game::CreationRules::Race::China);
    if (figures.empty() || armour.empty() || weapons.empty()) return fail("creation rules", "no Chinese figure, armour or weapon");

    const int level = std::clamp(c.GetInt("seed-level", 80), 1, 110);
    const std::int64_t gold = std::max<std::int64_t>(0, std::atoll(c.GetString("seed-gold", "100000000").c_str()));
    const std::int64_t sp = std::max<std::int64_t>(0, std::atoll(c.GetString("seed-sp", "1000000").c_str()));
    const std::string password = c.GetString("password", "secret123");
    const std::uint16_t server = std::uint16_t(c.GetInt("id", 1));
    // The character's body at its level as a player who put every point in strength would have it; health
    // and mana are the game server's cap on entering.
    const int strength = game::StartingStats::kStrength + (level - 1) * 4, intelligence = game::StartingStats::kIntelligence + (level - 1);

    const server::CharacterRow::Place jangan{25256, 1529.0f, 0.0f, 1115.0f, 0.0f}; // beside Jangan's fortress official (its nest: 1569, 1115)
    // --seed-kit archer (the quests' tests, the user's call): a Chinese archer: from level 101 degree 11's Seal of Star
    // (A_RARE: 1.188's degree 11 has the Star and its sets only), below it the last degree with a Seal of Sun (10,
    // C_RARE), its bow and garment set +9, the quiver in the shield's place, the biggest potions and spare arrows in the
    // bag, bow, fire and force at the level (330 at 110) with every skill they teach up to it (the hunt casts the imbue
    // and the strongest attacks).
    const std::string kit = c.GetString("seed-kit", "");
    // --seed-kit blade: the same with a blade and a shield, the blade (sword), fire and force masteries.
    const auto archer = [&](std::int64_t character, const std::string& name, bool bow) -> bool {
        struct Piece {
            server::EquipSlot slot;
            const char* code;
        };
        // Each piece the best its level wears (the highest degree whose level it meets, a Seal of Star from 11): a
        // piece above it would give nothing (game::LevelFits; the user's call 2026-11-13).
        const auto best = [&](const std::function<std::string(const std::string&, const std::string&)>& code) {
            for (int t = 11; t >= 1; --t) {
                const std::string tt = std::format("{:02}", t);
                for (const char* seal : {"A_RARE", "C"}) {
                    const std::string c = code(tt, seal);
                    const assets::ItemRow* r = rules.items ? rules.items->Find(std::string_view(c)) : nullptr;
                    if (r != nullptr && game::LevelFits(*r, level)) return c;
                }
            }
            return code("01", "C");
        };
        const auto clothes = [&](const char* part) {
            return best([&](const std::string& t, const std::string& s) { return std::string(man ? "ITEM_CH_M_CLOTHES_" : "ITEM_CH_W_CLOTHES_") + t + "_" + part + "_" + s; });
        };
        const std::string codes[] = {best([&](const std::string& t, const std::string& s) { return std::string(bow ? "ITEM_CH_BOW_" : "ITEM_CH_BLADE_") + t + "_" + s; }),
                                     bow ? std::string("ITEM_ETC_AMMO_ARROW_01")
                                         : best([&](const std::string& t, const std::string& s) { return "ITEM_CH_SHIELD_" + t + "_" + s; }),
                                     clothes("HA"), clothes("BA"), clothes("SA"), clothes("AA"), clothes("LA"), clothes("FA")};
        const Piece pieces[] = {{server::EquipSlot::Weapon, codes[0].c_str()}, {server::EquipSlot::Shield, codes[1].c_str()},
                                {server::EquipSlot::Head, codes[2].c_str()},   {server::EquipSlot::Chest, codes[3].c_str()},
                                {server::EquipSlot::Shoulders, codes[4].c_str()}, {server::EquipSlot::Hands, codes[5].c_str()},
                                {server::EquipSlot::Legs, codes[6].c_str()},   {server::EquipSlot::Feet, codes[7].c_str()}};
        const std::vector<std::int64_t> ids = server::ReserveItemIds(*db, int(std::size(pieces)));
        if (ids.size() < std::size(pieces)) return error = "no item ids", false;
        std::vector<server::StoredItem> worn;
        for (std::size_t i = 0; i < std::size(pieces); ++i) {
            const assets::ItemRow* itemRow = rules.items ? rules.items->Find(std::string_view(pieces[i].code)) : nullptr;
            if (itemRow == nullptr) return error = std::string("no item ") + pieces[i].code, false;
            server::StoredItem item;
            item.id = ids[i];
            item.slot = std::uint8_t(pieces[i].slot);
            item.refId = itemRow->refObjId;
            item.durability = game::NewDurability(*itemRow);
            item.plus = std::uint8_t(item.durability > 0 ? 9 : 0);
            item.counted = game::DataIsCount(*itemRow);
            item.quantity = std::uint16_t(item.durability > 0 ? 1 : game::MaxStack(*itemRow));
            worn.push_back(item);
        }
        if (!server::SaveItems(*db, character, worn, {}, {})) return error = "could not dress it", false;
        std::vector<server::StoredItem> bag;
        for (const auto& [code, count] : {std::pair{"ITEM_ETC_HP_POTION_05", 250}, std::pair{"ITEM_ETC_MP_POTION_05", 250}, std::pair{"ITEM_ETC_AMMO_ARROW_01", bow ? 1000 : 0}}) {
            if (count == 0) continue;
            const assets::ItemRow* itemRow = rules.items ? rules.items->Find(std::string_view(code)) : nullptr;
            if (itemRow == nullptr) return error = std::string("no item ") + code, false;
            server::StoredItem item;
            item.refId = itemRow->refObjId;
            item.counted = game::DataIsCount(*itemRow);
            const int most = game::MaxStack(*itemRow);
            for (int left = count; left > 0; left -= most) {
                item.quantity = std::uint16_t(std::min(left, most));
                bag.push_back(item);
            }
        }
        if (server::GiveItems(*db, server, name, bag, 0, error) != server::GiveResult::Done) return error = "the bag: " + error, false;
        // bow (Pacheon) or sword (Bicheon: the blade's too), fire, force (_RefSkill's ReqCommon_Mastery1)
        const std::int64_t weaponMastery = bow ? 259 : 257;
        const std::uint32_t kMasteries[] = {std::uint32_t(weaponMastery), 275, 276};
        db->Query("UPDATE _charskillmastery SET level = 0 WHERE charid = $1 AND masteryid NOT IN ($2, 275, 276)", {character, weaponMastery});
        for (const std::uint32_t mastery : kMasteries) {
            const auto trained = db->Query("UPDATE _charskillmastery SET level = $3 WHERE charid = $1 AND masteryid = $2", {character, std::int64_t(mastery), level});
            if (trained.Ok() && trained.Affected() == 0) db->Query("INSERT INTO _charskillmastery (charid, masteryid, level) VALUES ($1, $2, $3)", {character, std::int64_t(mastery), level});
        }
        const auto learned = db->Query(
            "INSERT INTO _charskill (charid, skillid, enable) SELECT $1, s.id, 1 FROM "
            "(SELECT DISTINCT ON (basic_group) id FROM _refskill WHERE service = 1 AND reqcommon_mastery1 IN ($3, 275, 276) AND reqcommon_masterylevel1 <= $2 "
            " ORDER BY basic_group, reqcommon_masterylevel1 DESC, id DESC) s "
            "ON CONFLICT (charid, skillid) DO UPDATE SET enable = 1",
            {character, level, weaponMastery});
        if (!learned.Ok()) return error = learned.Error(), false;
        return true;
    };
    std::vector<server::CharacterPlace> places;
    std::map<std::string, std::uint32_t> guilds;

    const std::string list = c.GetString("seed-teams");
    for (std::size_t at = 0; at < list.size();) {
        std::size_t end = list.find(';', at);
        if (end == std::string::npos) end = list.size();
        const std::string team = list.substr(at, end - at);
        at = end + 1;
        const std::size_t colon = team.find(':');
        if (colon == std::string::npos) return fail("a team without its names", team);
        const std::string guildName = team.substr(0, colon);
        std::vector<std::string> names;
        for (std::size_t n = colon + 1; n <= team.size();) {
            std::size_t comma = team.find(',', n);
            if (comma == std::string::npos) comma = team.size();
            if (comma > n) names.push_back(team.substr(n, comma - n));
            n = comma + 1;
        }
        if (names.empty()) return fail("a team without its names", team);

        std::uint32_t guild = 0;
        if (const auto found = db->Query("SELECT id FROM _guild WHERE lower(name) = lower($1)", {guildName}); found.Ok() && found.Rows() > 0) {
            guild = std::uint32_t(found.Int(0, 0));
        }
        for (const std::string& name : names) {
            std::string account = name;
            std::transform(account.begin(), account.end(), account.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            const auto made = server::CreateAccount(*db, account, password, error);
            if (made != server::CreateAccountResult::Created && made != server::CreateAccountResult::NameTaken) return fail("account " + account, error);
            bool ok = false;
            const auto row = server::FindAccount(*db, account, ok);
            if (!ok || !row) return fail("account " + account, "not found after making it");

            const auto findCharacter = [&]() -> std::int64_t {
                const auto q = db->Query("SELECT charid FROM _char WHERE lower(charname16) = lower($1) AND deleted = 0", {name});
                return q.Ok() && q.Rows() > 0 ? q.Int(0, 0) : 0;
            };
            std::int64_t character = findCharacter();
            if (character == 0) {
                server::NewCharacter n;
                n.name = name;
                n.model = figures.front();
                n.height = 34; // the middle of the create screen's slider
                n.level = game::StartingStats::kLevel;
                n.strength = game::StartingStats::kStrength;
                n.intelligence = game::StartingStats::kIntelligence;
                n.hp = game::StartingStats::kHp;
                n.mp = game::StartingStats::kMp;
                const auto& set = armour.front();
                const std::uint32_t weapon = weapons.front();
                const std::pair<server::EquipSlot, std::uint32_t> worn[] = {{server::EquipSlot::Chest, set.chest},
                                                                           {server::EquipSlot::Legs, set.legs},
                                                                           {server::EquipSlot::Feet, set.shoes},
                                                                           {server::EquipSlot::Weapon, weapon},
                                                                           {server::EquipSlot::Shield, rules.creation.ShieldFor(weapon)}};
                for (const auto& [slot, item] : worn) {
                    if (item == 0) continue;
                    const assets::ItemRow* itemRow = rules.items ? rules.items->Find(item) : nullptr;
                    n.equipment.push_back({slot, item, 0, itemRow ? game::NewDurability(*itemRow) : 0u});
                }
                if (server::CreateCharacter(*db, row->id, server, n, 4, error) != server::CreateCharacterResult::Created) return fail("character " + name, error);
                character = findCharacter();
                if (character == 0) return fail("character " + name, "not found after making it");
            }
            if (!db->Query("UPDATE _char SET curlevel = $2, maxlevel = GREATEST(maxlevel, $2), strength = $3, intellect = $4, hp = 2000000, mp = 2000000, "
                           "remaingold = $5, remainskillpoint = $6 WHERE charid = $1",
                           {character, level, strength, intelligence, gold, sp})
                     .Ok()) {
                return fail("character " + name, "could not raise it");
            }
            if (kit == "archer" || kit == "blade") {
                if (!archer(character, name, kit == "archer")) return fail("the " + kit + "'s kit for " + name, error);
            } else {
            // A sword of its level and all the sword mastery teaches up to it (the bots cast at random).
            const auto sword = db->Query("SELECT id FROM _refobjcommon WHERE codename128 ~ '^ITEM_CH_SWORD_[0-9]+_C$' AND service = 1 AND reqlevel1 <= $1 "
                                         "ORDER BY reqlevel1 DESC LIMIT 1",
                                         {level});
            if (sword.Ok() && sword.Rows() > 0) {
                const assets::ItemRow* swordRow = rules.items ? rules.items->Find(std::uint32_t(sword.Int(0, 0))) : nullptr;
                db->Query("UPDATE _items SET refitemid = $2, data = $3 WHERE id64 = (SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $4)",
                          {character, sword.Int(0, 0), std::int64_t(swordRow ? game::NewDurability(*swordRow) : 0u), int(server::EquipSlot::Weapon)});
            }
            // The guild's master, its commander, carries a fortress hammer (two-handed: no shield).
            if (&name == &names.front()) {
                const auto hammer = db->Query("SELECT id FROM _refobjcommon WHERE codename128 ~ '^ITEM_FORT_FORTRESS_HAMMER_[0-9]+$' AND service = 1 AND reqlevel1 <= $1 AND reqlevel1 > 10 "
                                              "ORDER BY reqlevel1 DESC LIMIT 1",
                                              {level});
                if (hammer.Ok() && hammer.Rows() > 0) {
                    const assets::ItemRow* hammerRow = rules.items ? rules.items->Find(std::uint32_t(hammer.Int(0, 0))) : nullptr;
                    db->Query("UPDATE _items SET refitemid = $2, data = $3 WHERE id64 = (SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $4)",
                              {character, hammer.Int(0, 0), std::int64_t(hammerRow ? game::NewDurability(*hammerRow) : 0u), int(server::EquipSlot::Weapon)});
                    db->Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = $2", {character, int(server::EquipSlot::Shield)});
                }
            }
            constexpr int kSwordMastery = 257; // _RefSkill's ReqCommon_Mastery1 of SKILL_CH_SWORD_*
            const auto trained = db->Query("UPDATE _charskillmastery SET level = $3 WHERE charid = $1 AND masteryid = $2", {character, kSwordMastery, level});
            if (trained.Ok() && trained.Affected() == 0) {
                db->Query("INSERT INTO _charskillmastery (charid, masteryid, level) VALUES ($1, $2, $3)", {character, kSwordMastery, level});
            }
            const auto learned = db->Query(
                "INSERT INTO _charskill (charid, skillid, enable) SELECT $1, s.id, 1 FROM "
                "(SELECT DISTINCT ON (basic_group) id FROM _refskill WHERE service = 1 AND reqcommon_mastery1 = $2 AND reqcommon_masterylevel1 <= $3 "
                " ORDER BY basic_group, reqcommon_masterylevel1 DESC, id DESC) s "
                "ON CONFLICT (charid, skillid) DO UPDATE SET enable = 1",
                {character, kSwordMastery, level});
            if (!learned.Ok()) return fail("skills of " + name, learned.Error());
            }
            places.push_back({character, jangan});

            const auto member = db->Query("SELECT guildid FROM _guildmember WHERE charid = $1 AND guildid <> 0", {character});
            if (!member.Ok()) return fail("guild member " + name, member.Error());
            const std::uint32_t in = member.Rows() > 0 ? std::uint32_t(member.Int(0, 0)) : 0;
            if (in != 0 && guild != 0 && in != guild) return fail(name, "is in another guild already");
            if (in == 0 && guild == 0) {
                bool nameTaken = false;
                guild = server::CreateGuild(*db, guildName, character, nameTaken, error);
                if (guild == 0) return fail("guild " + guildName, nameTaken ? "the name is taken" : error);
            } else if (in == 0) {
                if (!server::AddGuildMember(*db, guild, character, game::kGuildMember, 0)) return fail("guild member " + name, "could not add");
            } else {
                guild = in;
            }
            db->Query("UPDATE _guildmember SET charlevel = $2 WHERE charid = $1", {character, level});
        }
        // The top level (the war asks for 3) and guild points for the fortress's upgrades and hiring.
        db->Query("UPDATE _guild SET lvl = $2, gatheredsp = GREATEST(gatheredsp, 1000000) WHERE id = $1", {std::int64_t(guild), game::kGuildMaxLevel});
        guilds[guildName] = guild;
        core::LogInfo("SroGame: --seed-teams: {} (guild {}): {} members", guildName, guild, names.size());
    }
    if (!server::SavePlaces(*db, places)) return fail("places", "could not save them");

    // A clean fortress for the try: no application, no damage and no holder left from the last.
    const std::int64_t fortress = c.GetInt("seed-fortress", 1);
    const std::string owner = c.GetString("seed-owner", "");
    const auto ownerGuild = guilds.find(owner);
    if (!owner.empty() && ownerGuild == guilds.end()) return fail("--seed-owner", "not one of the teams: " + owner);
    if (c.Has("seed-owner") || c.Has("seed-fortress")) {
        const std::int64_t id = ownerGuild != guilds.end() ? std::int64_t(ownerGuild->second) : 0;
        const bool reset = db->Transaction(
            [&](server::Database& tx) {
                return tx.Query("DELETE FROM _siegefortressrequest WHERE fortressid = $1", {fortress}).Ok() &&
                       tx.Query("DELETE FROM _siegefortressstonestate WHERE fortressid = $1", {fortress}).Ok() &&
                       tx.Query("UPDATE _siegefortress SET guildid = $2, tempguildid = 0, npchired = 0 WHERE fortressid = $1", {fortress, id}).Ok() &&
                       tx.Query("UPDATE _siegefortressstruct SET ownerguildid = $2 WHERE fortressid = $1", {fortress, id}).Ok();
            },
            error);
        if (!reset) return fail("the fortress", error);
        core::LogInfo("SroGame: --seed-teams: fortress {} held by {}", fortress, owner.empty() ? "nobody" : owner);
    }
    return 0;
}

// --seed-character "ACCOUNT:NAME[:JOB],...": characters on an existing account (a tester's), for trying things: a Chinese
// character of --seed-level with --seed-gold and --seed-sp, a sword with the sword mastery and its skills; JOB thief, hunter
// or trader: its league at job level 7 (_CharTrijob), an alias (its suit is refused without one) and the best suit of the
// job its level wears in the bag. What is there already is kept (raised again). Only while the game servers are down.
[[maybe_unused]] int SeedCharacters(const sr::core::Config& c, const sr::server::ProgramOptions& options, const sr::game::CharacterRules& rules) {
    using namespace sr;
    std::string error;
    const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);
    auto db = server::Database::Connect(server::WithSearchPath(options.databaseUrl, shard + ", " + server::kAccountSchema), error);
    if (!db || !server::MigrateSchema(*db, shard, error)) {
        core::LogError("SroGame: --seed-character: database: {}", error);
        return 1;
    }
    const auto fail = [](std::string_view what, std::string_view detail) {
        core::LogError("SroGame: --seed-character: {}: {}", what, detail);
        return 1;
    };
    const std::vector<std::uint32_t> figures = rules.creation.FiguresOf(game::CreationRules::Race::China, true);
    const auto armour = rules.creation.ArmourOf(game::CreationRules::Race::China, true);
    const std::vector<std::uint32_t> weapons = rules.creation.WeaponsOf(game::CreationRules::Race::China);
    if (figures.empty() || armour.empty() || weapons.empty()) return fail("creation rules", "no Chinese figure, armour or weapon");
    const int level = std::clamp(c.GetInt("seed-level", 110), 1, 110);
    const std::int64_t gold = std::max<std::int64_t>(0, std::atoll(c.GetString("seed-gold", "100000000").c_str()));
    const std::int64_t sp = std::max<std::int64_t>(0, std::atoll(c.GetString("seed-sp", "1000000").c_str()));
    const std::uint16_t server = std::uint16_t(c.GetInt("id", 1));
    const int strength = game::StartingStats::kStrength + (level - 1) * 4, intelligence = game::StartingStats::kIntelligence + (level - 1);
    const server::CharacterRow::Place jangan{25256, 1529.0f, 0.0f, 1115.0f, 0.0f};
    std::vector<server::CharacterPlace> places;
    const std::string list = c.GetString("seed-character");
    for (std::size_t at = 0; at < list.size();) {
        std::size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        const std::string entry = list.substr(at, end - at);
        at = end + 1;
        const std::size_t first = entry.find(':');
        if (first == std::string::npos) return fail(entry, "ACCOUNT:NAME[:JOB]");
        const std::size_t second = entry.find(':', first + 1);
        const std::string account = entry.substr(0, first);
        const std::string name = entry.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
        const std::string job = second == std::string::npos ? std::string() : entry.substr(second + 1);
        bool ok = false;
        const auto row = server::FindAccount(*db, account, ok);
        if (!row) return fail("account " + account, "not found");
        const auto findCharacter = [&]() -> std::int64_t {
            const auto q = db->Query("SELECT charid FROM _char WHERE lower(charname16) = lower($1) AND deleted = 0", {name});
            return q.Ok() && q.Rows() > 0 ? q.Int(0, 0) : 0;
        };
        std::int64_t character = findCharacter();
        if (character == 0) {
            server::NewCharacter n;
            n.name = name;
            n.model = figures.front();
            n.height = 34;
            n.level = game::StartingStats::kLevel;
            n.strength = game::StartingStats::kStrength;
            n.intelligence = game::StartingStats::kIntelligence;
            n.hp = game::StartingStats::kHp;
            n.mp = game::StartingStats::kMp;
            const auto& set = armour.front();
            const std::uint32_t weapon = weapons.front();
            const std::pair<server::EquipSlot, std::uint32_t> worn[] = {{server::EquipSlot::Chest, set.chest}, {server::EquipSlot::Legs, set.legs},
                                                                       {server::EquipSlot::Feet, set.shoes}, {server::EquipSlot::Weapon, weapon},
                                                                       {server::EquipSlot::Shield, rules.creation.ShieldFor(weapon)}};
            for (const auto& [slot, item] : worn) {
                if (item == 0) continue;
                const assets::ItemRow* itemRow = rules.items ? rules.items->Find(item) : nullptr;
                n.equipment.push_back({slot, item, 0, itemRow ? game::NewDurability(*itemRow) : 0u});
            }
            if (server::CreateCharacter(*db, row->id, server, n, 4, error) != server::CreateCharacterResult::Created) return fail("character " + name, error);
            character = findCharacter();
            if (character == 0) return fail("character " + name, "not found after making it");
        }
        if (!db->Query("UPDATE _char SET curlevel = $2, maxlevel = GREATEST(maxlevel, $2), strength = $3, intellect = $4, hp = 2000000, mp = 2000000, "
                       "remaingold = $5, remainskillpoint = $6 WHERE charid = $1",
                       {character, level, strength, intelligence, gold, sp})
                 .Ok()) {
            return fail("character " + name, "could not raise it");
        }
        // A sword of its level, the sword mastery at it and every skill it teaches up to it.
        const auto sword = db->Query("SELECT id FROM _refobjcommon WHERE codename128 ~ '^ITEM_CH_SWORD_[0-9]+_C$' AND service = 1 AND reqlevel1 <= $1 "
                                     "ORDER BY reqlevel1 DESC LIMIT 1",
                                     {level});
        if (sword.Ok() && sword.Rows() > 0) {
            const assets::ItemRow* swordRow = rules.items ? rules.items->Find(std::uint32_t(sword.Int(0, 0))) : nullptr;
            db->Query("UPDATE _items SET refitemid = $2, data = $3 WHERE id64 = (SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $4)",
                      {character, sword.Int(0, 0), std::int64_t(swordRow ? game::NewDurability(*swordRow) : 0u), int(server::EquipSlot::Weapon)});
        }
        constexpr int kSwordMastery = 257;
        if (const auto trained = db->Query("UPDATE _charskillmastery SET level = $3 WHERE charid = $1 AND masteryid = $2", {character, kSwordMastery, level});
            trained.Ok() && trained.Affected() == 0) {
            db->Query("INSERT INTO _charskillmastery (charid, masteryid, level) VALUES ($1, $2, $3)", {character, kSwordMastery, level});
        }
        db->Query("INSERT INTO _charskill (charid, skillid, enable) SELECT $1, s.id, 1 FROM "
                  "(SELECT DISTINCT ON (basic_group) id FROM _refskill WHERE service = 1 AND reqcommon_mastery1 = $2 AND reqcommon_masterylevel1 <= $3 "
                  " ORDER BY basic_group, reqcommon_masterylevel1 DESC, id DESC) s "
                  "ON CONFLICT (charid, skillid) DO UPDATE SET enable = 1",
                  {character, kSwordMastery, level});
        // The job: the league at job level 7, an alias, the best suit its level wears.
        if (!job.empty()) {
            const game::JobType type = job == "trader" ? game::JobType::Trader : job == "thief" ? game::JobType::Thief : job == "hunter" ? game::JobType::Hunter
                                                                                                                                         : game::JobType::None;
            if (type == game::JobType::None) return fail(entry, "JOB is trader, thief or hunter");
            db->Query("INSERT INTO _chartrijob (charid, jobtype, level, exp, contribution, reward) VALUES ($1, $2, 7, 0, 0, 0) "
                      "ON CONFLICT (charid) DO UPDATE SET jobtype = $2, level = 7, exp = 0",
                      {character, int(type)});
            db->Query("UPDATE _char SET nickname16 = $2 WHERE charid = $1 AND (nickname16 IS NULL OR nickname16 = '')", {character, name + "Job"});
            const assets::ItemRow* suit = nullptr;
            int best = -1;
            if (rules.items) {
                for (const auto& [code, r] : rules.items->rows()) {
                    if (game::SuitJob(r) != type || r.country != 0 || r.sex != 1) continue;
                    const int need = r.requirementLevels[0];
                    if (need <= level && need > best) {
                        best = need;
                        suit = &r;
                    }
                }
            }
            if (suit != nullptr) {
                server::StoredItem item;
                item.refId = suit->refObjId;
                item.durability = game::NewDurability(*suit);
                if (server::GiveItems(*db, server, name, {item}, 0, error) != server::GiveResult::Done) return fail("the suit for " + name, error);
            }
            core::LogInfo("SroGame: --seed-character: {} the {} (league level 7, alias {}Job){}", name, job, name, suit ? ", " + suit->codename + " in the bag" : "");
        }
        places.push_back({character, jangan});
        core::LogInfo("SroGame: --seed-character: {} on {} at level {} with {} gold", name, account, level, gold);
    }
    if (!server::SavePlaces(*db, places)) return fail("places", "could not save them");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    using namespace sr;
    server::ProgramOptions options;
    std::string error;
    if (!server::ReadProgramOptions(argc, argv, "SroGame", 15781, options, error)) {
        core::LogError("SroGame: {}", error);
        return 1;
    }
    const core::Config& c = options.config;
    if (!c.GetString("give-silk", "").empty()) return GiveSilk(c, options); // no game files needed
    std::string gameDir = c.GetString("game");
    if (gameDir.empty()) {
        if (const char* env = std::getenv("SR_GAME_DIR")) gameDir = env;
    }
    if (gameDir.empty()) {
        core::LogError("SroGame: no game files: pass --game DIR or set SR_GAME_DIR");
        return 1;
    }
    auto rules = std::make_shared<game::CharacterRules>();
    // The archives stay mounted: the world loads its navigation from them.
    assets::FileSystem fs;
    if (!assets::MountGameArchives(fs, gameDir, error)) {
        core::LogError("SroGame: game files: {}", error);
        return 1;
    }
    // The original's reference tables in the database, when it holds them: the
    // client's text tables rebuilt from them, mounted over the game files, so every table below is
    // the database's (the client's rows only).
    const std::string shardSchema = c.GetString("shard-schema", server::kDefaultShardSchema);
    auto referenceDb = server::Database::Connect(server::WithSearchPath(options.databaseUrl, shardSchema + ", " + server::kAccountSchema), error);
    const bool reference = referenceDb && server::HasReferenceTables(*referenceDb);
    core::LogInfo("SroGame: the database {} the original's reference tables", reference ? "holds" : "does not hold");
    // A database SroSetup has not prepared: no world to run (tables = client runs on the client's files alone, for tries).
    if (!reference && c.GetString("tables", "database") != "client") {
        core::LogError("SroGame: the database has no game data: run SroSetup first (docs/SERVER_SETUP.md 5)");
        return 1;
    }
    if (reference && c.GetString("tables", "database") != "client") {
        server::DatabaseTextCounts counts;
        if (auto text = server::MakeDatabaseText(*referenceDb, fs, counts, error)) {
            fs.Mount(std::move(text), assets::FileSystem::kOverridePriority);
            core::LogInfo("SroGame: the tables from the database: {} files, {} rows ({} values differ from the client's: the database's taken); "
                          "{} rows the client lacks left out, {} of the client's not in the database (kept)",
                          counts.files, counts.rows, counts.changed, counts.databaseOnly, counts.clientOnly);
        } else {
            core::LogWarning("SroGame: the tables from the database: {} (the client's files serve them)", error);
        }
    }
    if (!rules->Load(fs, error)) {
        core::LogError("SroGame: game files: {}", error);
        return 1;
    }
    core::LogInfo("SroGame: {} creatable figures, {} forbidden words", rules->creation.Figures(), rules->names.ForbiddenWords());
    if (!c.GetString("give", "").empty()) return Give(c, options, *rules);
#ifdef SR_DEV_COMMANDS
    if (!c.GetString("seed-teams", "").empty()) return SeedTeams(c, options, *rules);
    if (!c.GetString("seed-character", "").empty()) return SeedCharacters(c, options, *rules);
#else
    // The test seeds make accounts with known passwords: not in a release build (CMake SR_DEV_COMMANDS).
    if (c.Has("seed-teams") || c.Has("seed-character")) {
        core::LogError("SroGame: --seed-teams and --seed-character are not in this build (a release)");
        return 1;
    }
#endif
    core::JobSystem jobs(c.GetInt("workers", core::JobSystem::kDefaultWorkers));
    if (!server::LoadProgramCertificate(options, error)) {
        core::LogError("SroGame: {}", error);
        return 1;
    }

    server::GameServerConfig config;
    config.listen = options.listen;
    config.peers.messagesPerSecond = options.messagesPerSecond;
    config.peers.messageBurst = options.messageBurst;
    config.peers.clientGate = options.clientGate;
    config.databaseUrl = options.databaseUrl;
    config.shardSchema = c.GetString("shard-schema", server::kDefaultShardSchema);
    config.rules = rules;
    // The Open Market: on unless "open-market = false"; its fees' percents by the price's bracket
    // ("open-market-listing-fees = 0,5,5,5", "open-market-sale-fees = 0,10,10,10": the original's server.cfg).
    config.openMarket = c.GetBool("open-market", true);
    const auto fees = [&](const char* key, std::array<int, 4>& out) {
        std::istringstream in(c.GetString(key, ""));
        std::string part;
        for (std::size_t k = 0; k < out.size() && std::getline(in, part, ','); ++k) out[k] = std::clamp(std::atoi(part.c_str()), 0, 100);
    };
    fees("open-market-listing-fees", config.openMarketFees.listing);
    fees("open-market-sale-fees", config.openMarketFees.sale);
    auto teleports = std::make_shared<assets::TeleportTable>();
    if (teleports->Load(fs, error)) {
        core::LogInfo("SroGame: {} teleport gates, {} links", teleports->Gates().size(), teleports->Links().size());
        config.teleports = teleports;
    } else {
        core::LogWarning("SroGame: teleports: {} (no gate leads anywhere)", error);
    }
    // The teleport zones: walked into, they take a player through their point's link.
    if (reference && server::LoadTeleportZones(*referenceDb, config.teleportZones, error)) {
        core::LogInfo("SroGame: {} teleport zones", config.teleportZones.size());
    } else if (reference) {
        core::LogWarning("SroGame: the teleport zones: {} (none takes anyone anywhere)", error);
    }
    // The NPCs (characterdata rows of type 1/2/2) where npcpos.txt puts them (dungeons' too: their regions have bit 15 set).
    auto characterTable = std::make_shared<assets::CharacterTable>();
    {
        assets::CharacterTable& characters = *characterTable;
        std::vector<assets::NpcPosition> places;
        const auto file = fs.Read("server_dep\\silkroad\\textdata\\npcpos.txt");
        if (characters.Load(fs, error) && file && assets::DecodeNpcPositions(core::DecodeTextFile(file->Bytes()), places)) {
            // Which way each faces (npcpos.txt has no column for it): npc_headings.tsv, written by
            // tools/scripts/npc_headings.py, by codename, region and the place in tenths; a heading
            // word turns from +x toward +z (65536 a full turn), our yaw from +z toward +x.
            std::map<std::tuple<std::string, int, long long, long long, long long>, int> headings;
            if (std::ifstream in(c.GetString("npc-headings", "npc_headings.tsv")); in) {
                std::string code;
                int region = 0, heading = 0;
                long long x = 0, y = 0, z = 0;
                for (std::string line; std::getline(in, line);) {
                    if (line.empty() || line[0] == '#') continue;
                    std::istringstream row(line);
                    if (row >> code >> region >> x >> y >> z >> heading) headings[{code, region, x, y, z}] = heading;
                }
            }
            const auto tenths = [](float v) { return std::llround(double(v) * 10.0); };
            std::size_t turned = 0;
            for (const assets::NpcPosition& p : places) {
                const assets::ObjectRow* row = characters.Find(p.refObjId);
                if (row == nullptr || row->typeId[1] != 2 || row->typeId[2] != 2) continue;
                float yaw = 0.0f;
                if (const auto it = headings.find({row->codename, int(p.region), tenths(p.x), tenths(p.y), tenths(p.z)}); it != headings.end()) {
                    yaw = float(std::numbers::pi / 2.0 - double(it->second) / 65536.0 * 2.0 * std::numbers::pi);
                    ++turned;
                }
                config.npcs.push_back({p.refObjId, row->codename, {p.region, p.x, p.y, p.z}, yaw});
            }
            // The Open Market's NPC (out of service in vSRO 1.188) at its nests when the market is on.
            if (config.openMarket) {
                if (const assets::ObjectRow* row = characters.Find(game::openmarket::kNpcRefObjId)) {
                    for (const game::openmarket::NpcSpot& s : game::openmarket::kNpcSpots) {
                        const float yaw = float(std::numbers::pi / 2.0 - double(s.heading) / 65536.0 * 2.0 * std::numbers::pi);
                        config.npcs.push_back({row->refObjId, row->codename, {s.region, s.x, s.y, s.z}, yaw});
                    }
                }
            }
            // The beta quests' Beta Helper (game/BetaQuests.h; ours): So-Ok's look, beside each So-Ok (8 to its east), or at
            // the places `beta-helpers` gives ("REGION,X,Y,Z[,HEADING]" separated by ';', the heading in degrees).
            config.betaQuests = c.GetBool("beta-quests", false);
            if (config.betaQuests) {
                if (const assets::ObjectRow* soOk = characters.Find(std::string_view(game::flag::kSoOk))) {
                    const std::string name(game::kBetaHelperName), codename = game::CustomNpcCodename(game::kBetaHelperName);
                    std::vector<server::GameServerConfig::NpcSpawn> helpers;
                    if (const std::string places = c.GetString("beta-helpers", ""); !places.empty()) {
                        std::istringstream list(places);
                        for (std::string one; std::getline(list, one, ';');) {
                            int region = 0;
                            float x = 0, y = 0, z = 0, heading = 0;
                            if (std::sscanf(one.c_str(), " %d , %f , %f , %f , %f", &region, &x, &y, &z, &heading) >= 4)
                                helpers.push_back({soOk->refObjId, codename, {std::uint16_t(region), x, y, z},
                                                   float(std::numbers::pi / 2.0 - double(heading) * std::numbers::pi / 180.0), name});
                        }
                    } else {
                        for (const server::GameServerConfig::NpcSpawn& n : config.npcs)
                            if (n.codename == game::flag::kSoOk) helpers.push_back({n.refObjId, codename, {n.position.region, n.position.x + 8.0f, n.position.y, n.position.z}, n.yaw, name});
                    }
                    config.npcs.insert(config.npcs.end(), helpers.begin(), helpers.end());
                    core::LogInfo("SroGame: the beta quests are on: {} Beta Helpers", helpers.size());
                }
            }
            core::LogInfo("SroGame: {} NPCs, {} of them facing their way (npc_headings.tsv)", config.npcs.size(), turned);
            // The monsters' points (characterdata TID3 1), dungeons' too, with their policy
            // from monster_nests.tsv (tools/scripts/monster_nests.py) by codename, region and the
            // place in whole units; a point without one holds one peaceful monster.
            std::map<std::tuple<std::string, int, long long, long long>, server::MonsterNest> policies;
            std::string nestText, nestSource;
            server::ReferenceCounts nestCounts;
            if (!c.Has("monster-nests") && reference && server::NestRecords(*referenceDb, nestText, nestCounts, error)) {
                nestSource = "the database";
            } else if (std::ifstream file(c.GetString("monster-nests", "monster_nests.tsv"), std::ios::binary); file) {
                std::stringstream all;
                all << file.rdbuf();
                nestText = all.str();
                nestSource = c.GetString("monster-nests", "monster_nests.tsv");
            }
            // An instance world's own point ("!" lines: the temples' and the tomb's; ReferenceData.cpp NestRecords): raised by its
            // instances in their layers, never in the shared world.
            std::set<std::tuple<std::string, int, long long, long long>> instanceOnly;
            if (std::istringstream in(nestText); !nestText.empty()) {
                std::string code;
                int region = 0, champion = 0, delayMin = 0, delayMax = 0, count = 0, aggressive = 0;
                long long x = 0, z = 0;
                float radius = 0, spawnRadius = 0, sight = 0;
                for (std::string line; std::getline(in, line);) {
                    if (line.empty() || line[0] == '#') continue;
                    if (line[0] == '!') {
                        std::istringstream row(line.substr(1));
                        if (row >> code >> region >> x >> z) instanceOnly.insert({code, region, x, z});
                        continue;
                    }
                    std::istringstream row(line);
                    if (!(row >> code >> region >> x >> z >> radius >> spawnRadius >> champion >> delayMin >> delayMax >> count >> aggressive >> sight)) continue;
                    // The database's records go on with the nest's id, its hive and the hive's cap (a unique's hive: one
                    // at a time over its nests; Monsters.h MonsterNest::hiveMax); monster_nests.tsv stops before them.
                    std::uint32_t nestId = 0, hive = 0;
                    int hiveMax = 0;
                    if (!(row >> nestId >> hive >> hiveMax)) nestId = hive = 0, hiveMax = 0;
                    // Then its tactics' trace and boundary (records made before them: 500 and 1).
                    int trace = 500, boundary = 1;
                    if (!(row >> trace >> boundary)) trace = 500, boundary = 1;
                    // Then the champion row's sight (0: its tactics names none, so no champion nor giant; records made
                    // before it: the nest's share as it is).
                    float championSight = 0;
                    if (!(row >> championSight)) championSight = -1;
                    if (championSight == 0) champion = 0;
                    championSight = std::max(0.0f, championSight);
                    // Then the crowded hunting ground's players, step and most (0: not one).
                    int crowdPlayers = 0, crowdStep = 0, crowdMax = 0;
                    if (!(row >> crowdPlayers >> crowdStep >> crowdMax)) crowdPlayers = crowdStep = crowdMax = 0;
                    server::MonsterNest& n = policies[{code, region, x, z}];
                    n.radius = radius;
                    n.spawnRadius = spawnRadius;
                    n.championPercent = champion;
                    n.delayMin = float(delayMin);
                    n.delayMax = float(delayMax);
                    n.maxCount = count;
                    n.aggressive = aggressive != 0;
                    n.sight = sight;
                    n.nestId = nestId;
                    n.hive = hive;
                    n.hiveMax = hiveMax;
                    n.trace = float(trace);
                    n.traceBoundary = boundary;
                    n.championSight = championSight;
                    n.crowdPlayers = crowdPlayers;
                    n.crowdStep = crowdStep;
                    n.crowdMax = crowdMax;
                }
            }
            std::size_t matched = 0, instanceOwn = 0;
            for (const assets::NpcPosition& p : places) {
                const assets::ObjectRow* row = characters.Find(p.refObjId);
                if (row == nullptr || row->typeId[1] != 2 || row->typeId[2] != 1) continue;
                if (instanceOnly.contains({row->codename, int(p.region), std::llround(p.x), std::llround(p.z)})) {
                    ++instanceOwn;
                    continue;
                }
                server::MonsterNest nest;
                if (const auto it = policies.find({row->codename, int(p.region), std::llround(p.x), std::llround(p.z)}); it != policies.end()) {
                    nest = it->second;
                    ++matched;
                }
                nest.refObjId = p.refObjId;
                nest.position = {p.region, p.x, p.y, p.z};
                config.monsterNests.push_back(nest);
            }
            config.characters = characterTable;
            core::LogInfo("SroGame: {} monster points, {} with their nest's policy ({}); {} left to the instances", config.monsterNests.size(), matched,
                          nestSource.empty() ? "none found" : nestSource, instanceOwn);
            // What their states roll against (K3): the database's Resist columns.
            if (auto resistances = std::make_shared<server::MonsterResistances>();
                reference && server::MonsterResistanceRecords(*referenceDb, *resistances, error)) {
                core::LogInfo("SroGame: the resistances of {} monsters", resistances->size());
                config.monsterResistances = std::move(resistances);
            } else if (reference) {
                core::LogWarning("SroGame: the monsters' resistances: {} (none resists a state)", error);
            }
            // The berserk modes by hwan level.
            if (reference && server::HwanLevelRecords(*referenceDb, config.hwanLevels, error)) {
                core::LogInfo("SroGame: {} hwan levels", config.hwanLevels.size());
            } else if (reference) {
                core::LogWarning("SroGame: the hwan levels: {} (the berserk mode's speed alone)", error);
            }
            // The socket stones' skills by level.
            if (reference && server::MagicOptGroupRecords(*referenceDb, config.magicOptGroups, error)) {
                core::LogInfo("SroGame: {} magic option group rows", config.magicOptGroups.size());
            } else if (reference) {
                core::LogWarning("SroGame: the magic option groups: {} (no socket stone goes in)", error);
            }
        } else {
            core::LogWarning("SroGame: npcpos.txt: {} (no NPCs)", error.empty() ? "missing" : error);
        }
    }
    // The fortress war: the fortresses in service from the database's
    // reference, their structures' places from the client's navmesh\objectstring.ifo, the schedule.
    if (referenceDb && config.characters) {
        if (!server::LoadBattleRanks(*referenceDb, config.fortress.battleRanks, error)) core::LogWarning("SroGame: the battle ranks: {}", error);
        if (!server::LoadFortressItems(*referenceDb, config.fortress.items, error)) core::LogWarning("SroGame: the fortress items: {}", error);
        if (!server::LoadFortressGuards(*referenceDb, config.fortress.guards, error)) core::LogWarning("SroGame: the fortress guards: {}", error);
        if (!server::LoadSiegeSummons(*referenceDb, config.fortress.summons, error)) core::LogWarning("SroGame: the summon scrolls' uniques: {}", error);
        if (!server::LoadBlessBuffs(*referenceDb, config.fortress.blesses, error)) core::LogWarning("SroGame: the fortress blessings: {}", error);
        if (!server::LoadForgeReference(*referenceDb, config.fortress.forges, error)) core::LogWarning("SroGame: the fortress forges: {}", error);
        if (!server::LoadSiegeQuests(*referenceDb, config.fortress.quests, config.fortress.rewards, error)) core::LogWarning("SroGame: the siege quests: {}", error);
        if (!server::LoadSiegeDungeons(*referenceDb, config.fortress.dungeons, error)) core::LogWarning("SroGame: the Cave of Discipline: {}", error);

        if (!server::LoadStructureReferences(*referenceDb, config.fortress.structureReferences, error)) {
            core::LogWarning("SroGame: the structures' reference: {}", error);
        }
        if (server::LoadFortressSpecs(*referenceDb, fs, *config.characters, config.fortresses, error)) {
            std::size_t structures = 0;
            for (const server::FortressSpec& f : config.fortresses) structures += f.structures.size();
            core::LogInfo("SroGame: {} fortresses, {} structure places", config.fortresses.size(), structures);
        } else {
            core::LogWarning("SroGame: the fortresses: {} (no fortress war)", error);
            config.fortresses.clear();
        }
        if (!server::LoadFortressSchedule(*referenceDb, config.fortress.schedule, error)) core::LogWarning("SroGame: the fortress schedule: {}", error);
        if (!server::LoadSchedules(*referenceDb, {game::kScheduleSpecialGoods}, config.bargainSchedule, error)) {
            core::LogWarning("SroGame: the bargain goods' hours: {}", error);
        }
    }
    if (const std::string cycle = c.GetString("fortress-cycle", ""); !cycle.empty()) {
        int requestSeconds = 0, gapSeconds = 0, warSeconds = 0;
        if (std::sscanf(cycle.c_str(), "%d,%d,%d", &requestSeconds, &gapSeconds, &warSeconds) == 3 && warSeconds > 0) {
            config.fortress.requestSeconds = std::max(0, requestSeconds);
            config.fortress.gapSeconds = std::max(0, gapSeconds);
            config.fortress.warSeconds = warSeconds;
            core::LogInfo("SroGame: the fortress war's cycle: {} s of applications, {} s, {} s of war", requestSeconds, gapSeconds, warSeconds);
        }
    }
    config.fortress.structureHp = std::uint32_t(std::max(0, c.GetInt("fortress-structure-hp", 0)));
    // --bandit-seconds A,B: the bandits' waves each A + up to B seconds (ours, for tries; the original's 60,60).
    if (const std::string b = c.GetString("bandit-seconds", ""); !b.empty()) {
        double a = 60.0, span = 60.0;
        if (std::sscanf(b.c_str(), "%lf,%lf", &a, &span) >= 1) {
            config.banditMinSeconds = a;
            config.banditSpanSeconds = span;
        }
    }
    auto shops = std::make_shared<assets::ShopTable>();
    if (shops->Load(fs, error)) {
        core::LogInfo("SroGame: {} merchants' shops", shops->size());
        config.shops = shops;
    } else {
        core::LogWarning("SroGame: shops: {} (no NPC sells anything)", error);
    }
    // The item mall's catalogue (Mall.h): GROUP_MALL's shops from the same tables.
    auto mall = std::make_shared<assets::MallTable>();
    if (mall->Load(fs, error)) {
        core::LogInfo("SroGame: the item mall: {} shops, {} goods", mall->shops().size(), mall->Goods());
        config.mall = mall;
    } else {
        core::LogWarning("SroGame: the item mall: {} (no mall, no silk)", error);
    }
    // Magic POP (MagicPop.h): the sets and the machines from the same tables.
    auto gacha = std::make_shared<assets::GachaTable>();
    if (gacha->Load(fs, error, false)) {
        core::LogInfo("SroGame: Magic POP: {} entries, {} machines", gacha->entries().size(), gacha->machines().size());
        config.gacha = gacha;
    } else {
        core::LogWarning("SroGame: Magic POP: {} (no machine plays)", error);
    }
    auto skills = std::make_shared<assets::SkillTable>();
    if (skills->Load(fs, error)) {
        config.skills = skills;
    } else {
        core::LogWarning("SroGame: skills: {} (no scroll casts anything)", error);
    }
    // Levels and gold by level (leveldata.txt, levelgold.txt).
    if (const auto file = fs.Read("server_dep\\silkroad\\textdata\\leveldata.txt")) {
        const std::string text = core::DecodeTextFile(file->Bytes());
        if (!assets::DecodeLevelTable(text, config.levels, error) || !assets::DecodeMonsterExperience(text, config.monsterExperience, error) ||
            !assets::DecodeMasteryCosts(text, config.masteryCosts, error)) {
            core::LogWarning("SroGame: leveldata.txt: {} (no experience)", error);
        }
    }
    if (const auto file = fs.Read("server_dep\\silkroad\\textdata\\levelgold.txt")) {
        if (!assets::DecodeLevelGold(core::DecodeTextFile(file->Bytes()), config.levelGold, error)) core::LogWarning("SroGame: levelgold.txt: {}", error);
    }
    // The magic options (alchemy's stones and protections).
    if (const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt")) {
        auto options = std::make_shared<std::vector<assets::MagicOptionRow>>();
        if (assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), *options, error)) config.magicOptions = std::move(options);
        else core::LogWarning("SroGame: magicoption.txt: {} (no alchemy)", error);
    }
    // The options each kind of item may get (magicoptionassign.txt: the database's _RefMagicOptAssign through
    // DatabaseText): the drops' random blues and the avatars' lists at the smith.
    std::string assignError;
    if (const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoptionassign.txt")) {
        auto assign = std::make_shared<std::vector<assets::MagicOptionAssignRow>>();
        if (assets::DecodeMagicOptionAssign(core::DecodeTextFile(file->Bytes()), *assign, assignError)) config.magicOptionAssign = std::move(assign);
    } else {
        assignError = "missing";
    }
    if (!config.magicOptionAssign) core::LogWarning("SroGame: magicoptionassign.txt: {} (no random blues on drops, no avatar granted an option)", assignError);
    // The item sets: refsetitemgroup.txt (the database's _RefSetItemGroup through DatabaseText)
    // and their groups' MagicType 2 rows (the database's _RefMagicOptGroup when read, else the client's file).
    if (config.magicOptions) {
        auto sets = std::make_shared<game::SetTable>();
        if (game::LoadSetTable(fs, *config.magicOptions, &config.magicOptGroups, *sets, error)) {
            core::LogInfo("SroGame: {} item sets", sets->Size());
            config.sets = std::move(sets);
        } else {
            core::LogWarning("SroGame: the item sets: {} (no set gives anything)", error);
        }
        // The Devil's Spirit's options and skills by plus: the database's
        // _Ref*ByItemOptLevel tables through DatabaseText, else the client's files (the same rows).
        auto spirits = std::make_shared<game::SpiritTable>();
        if (game::LoadSpiritTable(fs, *config.magicOptions, *spirits, error)) {
            core::LogInfo("SroGame: {} Devil's Spirit levels", spirits->Size());
            config.spirits = std::move(spirits);
        } else {
            core::LogWarning("SroGame: the Devil's Spirit's levels: {} (a worn spirit gives nothing)", error);
        }
    }
    // The rates (SroGame.cfg or the command line; 1 = the original's).
    config.rates.experience = std::max(0.0, c.GetDouble("rate-exp", 1.0));
    config.rates.skillExperience = std::max(0.0, c.GetDouble("rate-sp", 1.0));
    config.rates.gold = std::max(0.0, c.GetDouble("rate-gold", 1.0));
    config.rates.goldChance = std::max(0.0, c.GetDouble("rate-drop-gold", 1.0));
    config.rates.equipment = std::max(0.0, c.GetDouble("rate-drop-equipment", 1.0));
    config.rates.rare = std::max(0.0, c.GetDouble("rate-drop-sox", 1.0));
    config.rates.materials = std::max(0.0, c.GetDouble("rate-drop-material", 1.0));
    config.rates.elixirs = std::max(0.0, c.GetDouble("rate-drop-elixir", 1.0));
    config.rates.potions = std::max(0.0, c.GetDouble("rate-drop-potion", 1.0));
    config.rates.levelCap = std::clamp(c.GetInt("level-cap", 110), 1, 140);
    // Capture the Flag (ours: the original's times were its shard manager's): the shard's
    // event-ctf = on, its plays' starts ctf-times = 20:00,22:00 (registration opens 10 minutes before each, define 12's
    // own 10 minutes), --ctf-cycle R,P[,W] for tries (R s of registration, P s of play, W s for the winners, again from
    // the Unix epoch, so every server agrees), ctf-min-players N (a match's least, default 2).
    config.flagWorld.enabled = c.GetBool("event-ctf", false);
    if (const std::string times = c.GetString("ctf-times", "20:00,22:00"); !times.empty()) {
        std::size_t at = 0;
        while (at < times.size()) {
            const std::size_t comma = times.find(',', at);
            const std::string one = times.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            int h = 0, m = 0;
            if (std::sscanf(one.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) config.flagWorld.schedule.times.push_back(h * 60 + m);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
    }
    if (const std::string cycle = c.GetString("ctf-cycle", ""); !cycle.empty()) {
        int r = 0, p = 0, w = 60;
        if (std::sscanf(cycle.c_str(), "%d,%d,%d", &r, &p, &w) >= 2 && r > 0 && p > 0) {
            config.flagWorld.enabled = true;
            config.flagWorld.schedule.cycle = true;
            config.flagWorld.schedule.registerSeconds = r;
            config.flagWorld.schedule.playSeconds = p;
            config.flagWorld.schedule.winnersSeconds = std::max(0, w);
            core::LogInfo("SroGame: Capture the Flag's cycle: {} s of registration, {} s of play, {} s for the winners", r, p, w);
        }
    }
    // 0 (unset): the shard manager's fewest by the match's group (4, or 8 from group 3); a number: that (tries).
    if (const int fewest = c.GetInt("ctf-min-players", 0); fewest > 0) config.flagWorld.minPlayers = std::max(2, fewest);
    // The Battle Arena: the shard's event-arena = on runs the original's _Schedule rows 16..23;
    // --arena-cycle R,P for tries (ours: random mode, R s of registration, P s of play, again from the Unix epoch).
    config.arena.enabled = c.GetBool("event-arena", false);
    // 0 (unset): the shard manager's fewest a random match by its group (4, or 8 from group 3); a number: that (tries).
    if (const int fewest = c.GetInt("arena-min-players", 0); fewest > 0) config.arena.minPlayers = std::max(2, fewest);
    if (const std::string cycle = c.GetString("arena-cycle", ""); !cycle.empty()) {
        int r = 0, p = 0;
        if (std::sscanf(cycle.c_str(), "%d,%d", &r, &p) == 2 && r > 0 && p > 0) {
            config.arena.enabled = true;
            config.arena.cycleRegister = r;
            config.arena.cyclePlay = p;
        }
    }
    // The Roc: event-roc (on) runs SCHEDULE_DEF_MEET_ROC (Mondays 15:30, 10 minutes); --roc-cycle O,P for
    // tries (ours: the gate open O s every P s), --roc-time-scale N (the Peak's times N times faster).
    config.roc.enabled = c.GetBool("event-roc", true);
    if (const std::string cycle = c.GetString("roc-cycle", ""); !cycle.empty()) {
        int o = 0, p = 0;
        if (std::sscanf(cycle.c_str(), "%d,%d", &o, &p) == 2 && o > 0 && p > o) {
            config.roc.enabled = true;
            config.roc.cycleOpen = o;
            config.roc.cyclePeriod = p;
        }
    }
    if (const std::string scale = c.GetString("roc-time-scale", ""); !scale.empty()) config.roc.timeScale = std::max(0.01, std::atof(scale.c_str()));
    // The job temples: event-temples (on) runs `_Schedule` 9..11; --temple-cycle P for tries (ours: every
    // pair's period P seconds, the opening a quarter in).
    config.temples.enabled = c.GetBool("event-temples", true);
    if (const int period = c.GetInt("temple-cycle", 0); period > 0) {
        config.temples.enabled = true;
        config.temples.cyclePeriod = std::max(60, period);
    }
    config.pkProhibited = c.GetBool("pk-prohibited", false);
    // What monsters drop: the database's original tables, or a file.
    if (rules->items) {
        auto drops = std::make_shared<server::DropTable>();
        std::string text;
        server::ReferenceCounts counts;
        bool loaded = false;
        if (!c.Has("monster-drops") && reference && config.characters &&
            server::DropRecords(*referenceDb, *rules->items, *config.characters, text, counts, error) && drops->LoadText(text, *rules->items, error)) {
            loaded = true;
            core::LogInfo("SroGame: monster drops from the database: {} records ({} items, {} monsters the client lacks left out)", counts.records,
                          counts.missingItems, counts.missingMonsters);
        } else if (drops->Load(c.GetString("monster-drops", "monster_drops.tsv"), *rules->items, error)) {
            loaded = true;
            core::LogInfo("SroGame: monster drops loaded ({})", c.GetString("monster-drops", "monster_drops.tsv"));
        }
        if (loaded) {
            drops->SetLevelGold(config.levelGold);
            // The pieces' random blues: the option rows and the
            // lists by kind (magicoptionassign.txt: the database's _RefMagicOptAssign through DatabaseText).
            if (config.magicOptions && config.magicOptionAssign) {
                drops->SetMagicOptions(config.magicOptions, *config.magicOptionAssign);
                core::LogInfo("SroGame: the drops' random blues: {} lists of options", config.magicOptionAssign->size());
            } else {
                core::LogWarning("SroGame: the drops' random blues: {} (no dropped piece gets blues, a unique's extra pieces are lost)",
                                 !config.magicOptions ? std::string("no magic options") : std::string("no magicoptionassign.txt"));
            }
            config.drops = drops;
        } else {
            core::LogWarning("SroGame: monster drops: {} (monsters drop nothing; SroDb --import puts the tables in the database)", error);
        }
    }
    core::LogInfo("SroGame: rates: exp x{}, sp x{}, gold x{} (drops x{}), equipment x{}, SoX x{}, materials x{}, elixirs x{}, potions x{}; "
                  "level cap {}",
                  config.rates.experience, config.rates.skillExperience, config.rates.gold, config.rates.goldChance, config.rates.equipment,
                  config.rates.rare, config.rates.materials, config.rates.elixirs, config.rates.potions, config.rates.levelCap);
    config.id = std::uint16_t(c.GetInt("id", 1));
    config.serverName = c.GetString("server-name", "Silkroad");
    config.group = std::uint8_t(c.GetInt("group", 1));
    config.groupName = c.GetString("group-name", "Silkroad");
    config.publicHost = c.GetString("public-host", "127.0.0.1");
    config.capacity = std::uint32_t(c.GetInt("capacity", int(server::ShardLayout::kDefaultShardCapacity)));
    config.weatherSpeed = c.GetDouble("weather-speed", 1.0);
    config.shardCapacity = std::uint32_t(c.GetInt("shard-capacity", int(server::ShardLayout::kDefaultShardCapacity)));
    // The shard's settings (ShardLayout.h): this server's row of them; the command line still wins.
    auto layout = std::make_shared<server::ShardLayout>();
    if (!server::ShardLayout::Read(c, *layout, error)) {
        core::LogError("SroGame: {}", error);
        return 1;
    }
    if (!layout->Empty()) {
        const server::ShardServer* self = layout->Server(config.id);
        if (self == nullptr) {
            core::LogError("SroGame: the settings name no game server {}: --id one of those they list", config.id);
            return 1;
        }
        config.layout = layout;
        if (!c.Has("server-name")) config.serverName = layout->Name();
        if (!c.Has("public-host")) config.publicHost = self->publicHost;
        if (!c.Has("capacity")) config.capacity = self->capacity;
        if (!c.Has("shard-capacity")) config.shardCapacity = layout->Capacity();
        if (!c.Has("port") && self->port != 0) config.listen.port = self->port;
        core::LogInfo("SroGame: {} of shard '{}'", self->name, layout->Name());
    }
    config.maxCharacters = c.GetInt("max-characters", 3);
    config.deleteSeconds = c.GetInt("delete-days", 7) * 24 * 3600;
    config.world.tickRate = c.GetDouble("tick-rate", 20.0);
    config.devCommands = c.GetBool("dev-commands", false);
    config.guildElectionSeconds = c.GetInt("guild-election-seconds", config.guildElectionSeconds);
    config.guildWarEndSeconds = c.GetInt("guild-war-end-seconds", config.guildWarEndSeconds);
    config.statsInterval = std::chrono::seconds(std::max(0, c.GetInt("stats-seconds", 10)));
    if (config.devCommands) core::LogWarning("SroGame: developer commands are on (development only)");
    config.world.zoneRegions = c.GetInt("zone-regions", 16);
    // Who sees whom (World.h): the radius in the minimap's units (a tenth of the world's), each
    // kind's share, the tiers' sizes.
    config.world.viewRadius = float(10.0 * c.GetDouble("view-radius", 600.0));
    config.world.viewPlayers = std::size_t(std::max(0, c.GetInt("view-players", 300)));
    config.world.viewMonsters = std::size_t(std::max(0, c.GetInt("view-monsters", 150)));
    config.world.viewItems = std::size_t(std::max(0, c.GetInt("view-items", 50)));
    config.world.viewTier1 = std::size_t(std::max(0, c.GetInt("view-tier1", 150)));
    config.world.viewTier2 = std::size_t(std::max(0, c.GetInt("view-tier2", 150)));
    config.world.viewKeep = float(std::clamp(c.GetDouble("view-keep", 0.1), 0.0, 0.9));
    config.networkThreads = unsigned(std::clamp(c.GetInt("net-threads", 2), 1, 64));
    core::LogInfo("SroGame: views: radius {:.0f} (map units), shares {} players / {} monsters / {} items, tiers {} + {} + the rest; "
                  "{} network threads, {} job workers",
                  config.world.viewRadius / 10.0f, config.world.viewPlayers, config.world.viewMonsters, config.world.viewItems,
                  config.world.viewTier1, config.world.viewTier2, config.networkThreads, jobs.WorkerCount());
    config.jobs = &jobs;
    config.files = &fs;
    server::GameServer game;
    if (!game.Start(config, error)) {
        core::LogError("SroGame: {}", error);
        return 1;
    }
    server::InstallStopHandler();
    game.Run(server::StopRequested());
    game.Stop();
    core::LogInfo("SroGame: stopped");
    return 0;
}
