// SroBots.exe - the bot benchmark (C6f): bots enter one game server's world
// and walk, so its zones' ticks can be measured under load (SroGame logs them, --stats-seconds).
//
// Each bot is an account `botN` with a character `BotN` (made in the database when missing,
// dressed as a template character; bots have no password: they log in with tickets issued in
// the database, as the login server issues them). Their places are spread over clusters of
// regions that straddle zone edges, so walking takes them from zone to zone; the server puts them
// down on the floor there. Then each connects over TLS, greets, enters the lobby with its ticket,
// selects its character, enters the world and every few seconds clicks a point around it.
//
//   --bots N               bots (default 5000)
//   --db URL               the database (default: SR_DB_URL)
//   --game DIR             the game files, for the regions that have navigation (default: SR_GAME_DIR)
//   --host H --port P      the game server (default 127.0.0.1:15781), --server-id N (default 1),
//                          --shard-schema NAME its shard's schema (default sro_vt_shard)
//   --server-certificate F the certificate the server's chain ends in (default dev\server.pem)
//   --tls-name NAME        the name it carries (default localhost)
//   --template NAME        the character the bots are dressed as (default Hero)
//   --clusters N           clusters of places (default 50); --zone-regions N the server's zone width (16)
//   --ramp N               bots connecting a second (default 250)
//   --walk-every S         seconds between a bot's clicks, on average (default 4); --walk-range R (300)
//   --run-seconds S        how long the bots stay in once all are in (default 120)
//   --threads N            network threads (default 4); --workers N the threads the bots are played
//                          on (default: the machine's); --seed N
//   --latency MS --jitter MS --loss P --rto MS   a simulated network for every bot, both ways: each
//                          message waits MS (+ up to the jitter), in order; a lost packet (P of them,
//                          0.02 = 2 %) holds back everything behind it for the retransmission (rto,
//                          default 250 ms), as TCP does. The report adds what a view holds by kind,
//                          the bytes a bot receives and the time from a click to its own new leg
//   --place X,Z[,LX,LZ]    all bots packed around one spot of region X,Z (local LX,LZ, default its
//                          middle) instead of the clusters; --place-radius R (default 400): they stand
//                          and walk within R of it (Hotan's gate: --place 135,92,1138,153). Several spots
//                          separated by ';' share the bots out in equal batches, each batch about its own
//   --actions              the bots also play: each gets a kit (gold, stacks in its bag, buffs it can
//                          cast on itself with any weapon, chosen from the game's skill table) and at
//                          random intervals casts a buff, says a line, drops an item (or gold when its
//                          bag is empty) and picks up what lies near it. The averages: --cast-every S
//                          (10), --chat-every S (25), --drop-every S (30), --pick-every S (8),
//                          --pick-range R (250); --buffs N buffs in the kit (default 6). Among
//                          monsters it also attacks the nearest within --attack-range R (400) every
//                          --attack-every S (6), and a dead bot rises where it fell after 3 to 8 s
//   --ctf Capture the Flag's players: each registers at the
//                          registration's opening line (state 2; So-Ok's row, sent from where it stands, which must be
//                          a town), follows the server's moves into the match and home (ServerMove: on to that server
//                          with its ticket) and rises at its camp when killed; they do not walk (--walk-every is ignored);
//                          the Battle Arena's too (a random score registration at its opening line)
//   --temples the job temples' scenario, a test with its verdict (exit 0 when every
//                          check passed): bot1 a game master and trader by the traders' and hunters' desert gate (171),
//                          --temple-traders N (30) traders there, --temple-hunters N (5) hunters there, --temple-thieves N
//                          (15) thieves by the thieves' (172), --temple-late N (10) more traders, and one thief at the
//                          traders' gate; all level 110 in their league with its suit on. In turn: the gates (the traders,
//                          the hunters, the thieves, the late traders, the thief at the wrong gate: "no right"; once
//                          --temple-least (30) are in, an alliance's share of --temple-share (70) % or more refused, the
//                          hunters counted with the traders, each answer checked against the rule), the board's icon and the
//                          board by it, the board's numbers again after the game master's /temple REP, Anubis's and Isis's
//                          opening for the thieves (/temple), --temple-doors N (3) of each league at Anubis's door (the
//                          thieves in, the traders and the hunters refused), the closing (the thieves back in the area).
//                          Connect them to the server holding the temples
//                          (the dev shard: --port 15782 --server-id 2)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Config.h"
#include "sr/core/Log.h"
#include "sr/core/Jobs.h"
#include "sr/game/LobbyProtocol.h"
#include "sr/game/NavLoader.h"
#include "sr/game/NavMesh.h"
#include "sr/game/Protocol.h"
#include "sr/game/EventProtocol.h"
#include "sr/game/Arena.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/net/Connection.h"
#include "sr/server/Database.h"
#include "sr/server/Program.h"
#include "sr/server/Store.h"

namespace {

using namespace sr;
namespace proto = game::proto;
using Clock = std::chrono::steady_clock;

// A simulated network between each bot and the server (--latency, --jitter, --loss): each way, a
// message waits its delay, never passing one sent before it (TCP keeps the order); a lost packet
// holds back everything behind it until it is sent again (--rto, TCP's retransmission wait).
struct NetSim {
    double latencyMs = 0, jitterMs = 0, loss = 0, rtoMs = 250;
    bool On() const { return latencyMs > 0 || jitterMs > 0 || loss > 0; }
};

struct Bot {
    std::int64_t account = 0, character = 0;
    std::string name;
    enum class Stage { Waiting, Greeting, Lobby, Selecting, Entering, InWorld, Closed } stage = Stage::Waiting;
    std::shared_ptr<net::Connection> connection;
    std::vector<std::uint8_t> ticket;
    std::uint32_t entity = 0;
    // --ctf: a move to another game server under way (its host and port; its ticket in `ticket`), and whether it
    // arrives from one (the server takes it straight into the world).
    std::optional<std::pair<std::string, std::uint16_t>> moveTo;
    bool arriving = false;
    proto::WorldPosition at;
    std::optional<proto::FlatPosition> home; // --place: the spot it stands and walks about
    Clock::time_point nextWalk;
    std::vector<net::Message> inbox;
    // --actions: what it holds and sees, and when it acts next.
    std::set<std::uint8_t> bag; // bag places that hold something (13 and up)
    std::uint64_t gold = 0;
    std::unordered_map<std::uint32_t, proto::FlatPosition> ground;   // the ground items it sees
    std::unordered_map<std::uint32_t, proto::FlatPosition> monsters; // the living monsters it sees
    Clock::time_point nextCast, nextChat, nextDrop, nextPick, nextAttack;
    bool dead = false;
    Clock::time_point riseAt; // dead: when it asks to rise where it fell
    // What it sees, by kind (0 player, 1 NPC, 2 monster, 3 ground item); the click it waits to hear
    // answered (its own new leg: the round trip a player feels).
    std::unordered_map<std::uint32_t, std::uint8_t> kinds;
    Clock::time_point clickAt{};
    bool clickPending = false;
    // --temples: its part (0 none, 1 the game master, 2 a trader, 3 a thief) and what it heard.
    int templeRole = 0;
    bool templeWaiting = false; // a gate or a door asked, no answer yet
    int templeAnswer = -1;      // the last one: 0 through (WorldEntered), else the GateStatus it was refused
    int templeIcon = -1;        // TempleIcon: 1 shown, 0 hidden, -1 none heard
    int templeBoard = -1;       // TempleReputation: 0 the board with two rows, else 100 + its code
    std::optional<proto::TemplePoints> templePoints; // the board's numbers (TemplePoints)
    std::vector<proto::TempleNotice> templeNotices;
    std::unordered_map<std::uint32_t, proto::FlatPosition> doors; // the temples' doors in sight (INS_TEMPLE_TELEPORT)
    // Its own randomness (bots are played on several threads) and its side of the simulated network.
    std::mt19937 rng;
    const NetSim* sim = nullptr;
    struct Delayed {
        Clock::time_point due;
        net::Message message;
    };
    std::deque<Delayed> outQueue, inQueue;
    Clock::time_point outLast{}, inLast{};

    // When `bytes` sent or received now get through: their delay, after anything before them. A
    // loss is rolled per TCP segment (1,460 bytes): one lost segment stalls the whole stream.
    Clock::time_point Due(Clock::time_point now, Clock::time_point& last, std::size_t bytes) {
        double ms = sim->latencyMs;
        if (sim->jitterMs > 0) ms += std::uniform_real_distribution<double>(0.0, sim->jitterMs)(rng);
        const double segments = std::max(1.0, std::ceil(double(bytes) / 1460.0));
        const double lost = 1.0 - std::pow(1.0 - sim->loss, segments);
        if (sim->loss > 0 && std::uniform_real_distribution<double>(0.0, 1.0)(rng) < lost) ms += sim->rtoMs;
        Clock::time_point due = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
        if (due < last) due = last;
        last = due;
        return due;
    }
    void Queue(net::MessageId id, std::vector<std::byte> payload) {
        if (sim == nullptr || !sim->On()) {
            connection->Send(id, payload);
            return;
        }
        const std::size_t bytes = payload.size() + 6;
        outQueue.push_back({Due(Clock::now(), outLast, bytes), {id, std::move(payload)}});
    }
    void FlushOut(Clock::time_point now) {
        while (!outQueue.empty() && outQueue.front().due <= now) {
            connection->Send(outQueue.front().message.id, outQueue.front().message.payload);
            outQueue.pop_front();
        }
    }
};

// What the bots get to play with (--actions).
struct Kit {
    std::vector<std::uint32_t> skills;                    // skilldata ids each bot casts on itself
    std::vector<std::pair<std::uint32_t, int>> masteries; // mastery id, level
    std::vector<std::pair<std::uint32_t, int>> groups;    // skill group, level learned
    std::vector<std::pair<std::uint32_t, int>> stacks;    // itemdata id, count: the bag's stacks
};

// Buffs a Chinese bot can cast on itself whatever it holds: in service, no target asked, a time it
// lasts (5 s or more), at most 60 MP, at most mastery level 20; each group's first level, the lowest
// masteries first.
void PickBuffs(const assets::SkillTable& skills, int count, Kit& kit) {
    std::vector<const assets::SkillRow*> found;
    for (const assets::MasteryRow& m : skills.masteries()) {
        for (const assets::SkillRow* r : skills.Board(m.id)) {
            if (r == nullptr || !r->inService || r->race != 0 || r->targetRequired || !r->hasDuration || r->activity == 0) continue;
            if (r->weaponKinds[0] != 255 || r->weaponKinds[1] != 255 || r->masteries[0] == 0) continue;
            if (r->mpCost > 60 || r->masteryLevels[0] > 20 || r->buffDurationMs < 5000) continue;
            found.push_back(r);
        }
    }
    std::sort(found.begin(), found.end(), [](const assets::SkillRow* a, const assets::SkillRow* b) {
        return a->masteryLevels[0] != b->masteryLevels[0] ? a->masteryLevels[0] < b->masteryLevels[0] : a->id < b->id;
    });
    std::map<std::uint32_t, int> masteries;
    for (const assets::SkillRow* r : found) {
        if (int(kit.skills.size()) >= count) break;
        kit.skills.push_back(r->id);
        kit.groups.push_back({r->group, std::max(1, r->level)});
        int& level = masteries[r->masteries[0]];
        level = std::max({level, r->masteryLevels[0], 1});
        core::LogInfo("SroBots: buff {} ({}, mastery {} level {}, {} MP, {} s)", r->codename, r->id, r->masteries[0], r->masteryLevels[0],
                      r->mpCost, r->buffDurationMs / 1000);
    }
    for (const auto& [m, level] : masteries) kit.masteries.push_back({m, level});
}

// The bots' characters (by name) among the first `count`, for the SQL below ($1 is `count`).
constexpr const char* kBots = "lower(c.charname16) ~ '^bot[0-9]+$' AND substr(c.charname16, 4)::int <= $1::int";

// New items for the rows of `need` (charid, slot, refitemid, optlevel, variance, data), in those
// places of _inventory: blank ids are not reused here (a bulk insert), each gets a serial, and the
// pool knows them in use, as the original's allocation leaves an item.
bool PlaceNewItems(server::Database& db, const std::string& need, std::vector<server::Param> params, const char* what, std::string& error) {
    const server::QueryResult r = db.Query(
        "WITH need AS (" + need + "), "
        "numbered AS (SELECT need.*, row_number() OVER () AS rn FROM need), "
        "s AS (UPDATE _latestitemserial SET latestitemserial = latestitemserial + (SELECT count(*) FROM numbered) "
        "      RETURNING latestitemserial - (SELECT count(*) FROM numbered) AS base), "
        "made AS (INSERT INTO _items (refitemid, optlevel, variance, data, magparamnum, serial64) "
        "         SELECT numbered.refitemid, numbered.optlevel, numbered.variance, numbered.data, 0, s.base + numbered.rn FROM numbered, s "
        "         RETURNING id64, serial64), "
        "pooled AS (INSERT INTO _itempool (itemid, inuse) SELECT id64, 1 FROM made) "
        "UPDATE _inventory i SET itemid = made.id64 FROM made, numbered, s "
        "WHERE made.serial64 = s.base + numbered.rn AND i.charid = numbered.charid AND i.slot = numbered.slot",
        params);
    if (!r.Ok()) error = std::string(what) + ": " + r.Error();
    return r.Ok();
}

// The kit in the database for the first `count` bots: gold, the bag's stacks where a place is free,
// the masteries and the skills (raised, never lowered).
bool PrepareKit(server::Database& db, int count, const Kit& kit, std::string& error) {
    const std::string bots = kBots;
    const auto run = [&](const std::string& sql, std::vector<server::Param> params, const char* what) {
        const server::QueryResult r = db.Query(sql, std::move(params));
        if (!r.Ok()) error = std::string(what) + ": " + r.Error();
        return r.Ok();
    };
    if (!run("UPDATE _char c SET remaingold = GREATEST(c.remaingold, 5000000) WHERE " + bots, {count}, "gold")) return false;
    for (std::size_t i = 0; i < kit.stacks.size(); ++i) {
        const int slot = 13 + int(i);
        if (!PlaceNewItems(db,
                           "SELECT c.charid, $2::smallint AS slot, $3::int AS refitemid, 0::smallint AS optlevel, 0::bigint AS variance, $4::int AS data "
                           "FROM _char c JOIN _inventory x ON x.charid = c.charid AND x.slot = $2::smallint AND x.itemid = 0 WHERE " + bots,
                           {count, slot, int(kit.stacks[i].first), kit.stacks[i].second}, "the bag", error)) {
            return false;
        }
    }
    for (const auto& [mastery, level] : kit.masteries) {
        if (!run("INSERT INTO _charskillmastery (charid, masteryid, level) SELECT c.charid, $2::int, $3::smallint FROM _char c WHERE " + bots +
                     " ON CONFLICT (charid, masteryid) DO UPDATE SET level = GREATEST(_charskillmastery.level, EXCLUDED.level)",
                 {count, int(mastery), level}, "masteries")) {
            return false;
        }
    }
    for (std::uint32_t skill : kit.skills) {
        if (!run("INSERT INTO _charskill (charid, skillid, enable) SELECT c.charid, $2::int, 1 FROM _char c WHERE " + bots + " ON CONFLICT DO NOTHING",
                 {count, std::int64_t(skill)}, "skills")) {
            return false;
        }
    }
    return true;
}

template <typename T>
void Send(Bot& bot, const T& message) {
    bot.Queue(T::kId, proto::Encode(message));
}

// What the bots did and heard since the last report (each job adds its own, then they are summed).
struct Tally {
    std::uint64_t received = 0, bytes = 0, clicks = 0, casts = 0, chats = 0, drops = 0, goldDrops = 0, picks = 0, chatLines = 0;
    std::uint64_t itemsOk = 0, itemsRefused = 0, goldChanges = 0, attacks = 0, deaths = 0, rises = 0, hitsSeen = 0;
    std::int64_t inWorld = 0; // entered minus left
    std::uint64_t closed = 0;
    std::map<int, std::uint64_t> castAnswers; // SkillUseStatus -> count
    std::vector<double> rtts;                 // a click to its own new leg, ms
    void Add(const Tally& o) {
        received += o.received;
        bytes += o.bytes;
        clicks += o.clicks;
        casts += o.casts;
        chats += o.chats;
        drops += o.drops;
        goldDrops += o.goldDrops;
        picks += o.picks;
        chatLines += o.chatLines;
        itemsOk += o.itemsOk;
        itemsRefused += o.itemsRefused;
        goldChanges += o.goldChanges;
        attacks += o.attacks;
        deaths += o.deaths;
        rises += o.rises;
        hitsSeen += o.hitsSeen;
        inWorld += o.inWorld;
        closed += o.closed;
        for (const auto& [k, n] : o.castAnswers) castAnswers[k] += n;
        rtts.insert(rtts.end(), o.rtts.begin(), o.rtts.end());
    }
};

double Percentile(std::vector<double>& values, double p) {
    if (values.empty()) return 0.0;
    const std::size_t k = std::min(values.size() - 1, std::size_t(p * double(values.size())));
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(k), values.end());
    return values[k];
}

// The regions that have navigation (navmesh\nv_XXXX.nvm).
std::set<std::uint16_t> NavRegions(const assets::FileSystem& fs) {
    std::set<std::uint16_t> out;
    for (const std::string& path : fs.ListFiles()) {
        unsigned id = 0;
        if (std::sscanf(path.c_str(), "navmesh\\nv_%4x.nvm", &id) == 1 && id < 0x8000) out.insert(std::uint16_t(id));
    }
    return out;
}

// The bots' accounts and characters (made when missing), dressed as the template: in the
// original's tables, with every row a new character gets there.
bool Prepare(server::Database& db, int count, const std::string& templateName, std::vector<Bot>& bots, std::string& error) {
    const server::QueryResult t = db.Query("SELECT charid FROM _char WHERE lower(charname16) = lower($1) AND deleted = 0 LIMIT 1", {templateName});
    if (!t.Ok() || t.Rows() == 0) {
        error = std::format("no template character '{}' ({})", templateName, t.Error());
        return false;
    }
    const std::int64_t templateId = t.Int(0, 0);
    const std::string mine = kBots; // the bots' characters, in SQL
    const auto run = [&](const std::string& sql, std::vector<server::Param> params, const char* what) {
        const server::QueryResult r = db.Query(sql, std::move(params));
        if (!r.Ok()) error = std::string(what) + ": " + r.Error();
        return r.Ok();
    };
    // Bots never log in with a password: they have neither ours nor the original's.
    if (!run("INSERT INTO sro_vt_account.tb_user (struserid, password, regtime) SELECT 'bot' || g, '', localtimestamp "
             "FROM generate_series(1, $1::int) g ON CONFLICT DO NOTHING",
             {count}, "accounts") ||
        // Their starting silk, as every account's (SilkStore.h kStartingSilk; none logged: they are no players).
        !run("INSERT INTO sro_vt_account.sk_silk (jid, silk_own, silk_gift, silk_point) SELECT u.jid, 10000, 0, 0 FROM sro_vt_account.tb_user u "
             "WHERE lower(u.struserid) ~ '^bot[0-9]+$' ON CONFLICT (jid) DO NOTHING",
             {}, "silk") ||
        !run("WITH a AS (SELECT u.jid, substr(u.struserid, 4) AS n FROM sro_vt_account.tb_user u "
             "           WHERE lower(u.struserid) ~ '^bot[0-9]+$' AND substr(u.struserid, 4)::int <= $1::int "
             "           AND NOT EXISTS (SELECT 1 FROM _user x WHERE x.userjid = u.jid)), "
             "c AS (INSERT INTO _char (refobjid, charname16, scale, strength, intellect, hp, mp, posx, posy, posz, inventorysize, lastlogout) "
             "      SELECT t.refobjid, 'Bot' || a.n, t.scale, t.strength, t.intellect, t.hp, t.mp, 0, 0, 0, 45, localtimestamp "
             "      FROM a, _char t WHERE t.charid = $2 ON CONFLICT DO NOTHING RETURNING charid, charname16) "
             "INSERT INTO _user (userjid, charid) SELECT u.jid, c.charid FROM c JOIN sro_vt_account.tb_user u ON lower(u.struserid) = lower(c.charname16)",
             {count, templateId}, "characters") ||
        !run("INSERT INTO _charnamelist (charname16, charid) SELECT left(c.charname16, 17), c.charid FROM _char c WHERE " + mine + " ON CONFLICT DO NOTHING",
             {count}, "names") ||
        !run("INSERT INTO _inventory (charid, slot, itemid) SELECT c.charid, g, 0 FROM _char c CROSS JOIN generate_series(0, 108) g WHERE " + mine +
                 " ON CONFLICT DO NOTHING",
             {count}, "inventory") ||
        !run("INSERT INTO _inventoryforavatar (charid, slot, itemid) SELECT c.charid, g, 0 FROM _char c CROSS JOIN generate_series(0, 4) g WHERE " + mine +
                 " ON CONFLICT DO NOTHING",
             {count}, "avatar places") ||
        !run("INSERT INTO _clientconfig (charid, configtype, slotseq, slottype, data) "
             "SELECT c.charid, k.t, k.q, 0, 0 FROM _char c CROSS JOIN (SELECT 0 AS t, 0 AS q UNION ALL SELECT 1, g FROM generate_series(0, 50) g "
             "  UNION ALL SELECT t, 0 FROM generate_series(11, 14) t) k WHERE " + mine + " ON CONFLICT DO NOTHING",
             {count}, "quick slots") ||
        !run("INSERT INTO _staticavatar (charid) SELECT c.charid FROM _char c WHERE " + mine + " ON CONFLICT DO NOTHING", {count}, "avatar") ||
        !run("INSERT INTO _chartrijob (charid) SELECT c.charid FROM _char c WHERE " + mine + " ON CONFLICT DO NOTHING", {count}, "jobs") ||
        !run("INSERT INTO sr_char_state (charid) SELECT c.charid FROM _char c WHERE " + mine + " ON CONFLICT DO NOTHING", {count}, "state")) {
        return false;
    }
    // What the template wears, new items for the bots that wear nothing.
    if (!PlaceNewItems(db,
                       "SELECT c.charid, w.slot, w.refitemid, coalesce(w.optlevel, 0) AS optlevel, coalesce(w.variance, 0) AS variance, w.data "
                       "FROM _char c, (SELECT i.slot, it.refitemid, it.optlevel, it.variance, it.data FROM _inventory i JOIN _items it ON it.id64 = i.itemid "
                       "               WHERE i.charid = $2 AND i.slot < 13 AND i.itemid <> 0) w "
                       "WHERE " + mine + " AND c.charid <> $2 "
                       "AND NOT EXISTS (SELECT 1 FROM _inventory x WHERE x.charid = c.charid AND x.slot < 13 AND x.itemid <> 0)",
                       {count, templateId}, "items", error)) {
        return false;
    }
    const server::QueryResult r = db.Query(
        "SELECT u.jid, c.charid, c.charname16 FROM sro_vt_account.tb_user u JOIN _user x ON x.userjid = u.jid JOIN _char c ON c.charid = x.charid "
        "WHERE lower(u.struserid) ~ '^bot[0-9]+$' AND substr(u.struserid, 4)::int <= $1::int ORDER BY substr(u.struserid, 4)::int",
        {count});
    if (!r.Ok()) {
        error = "the bots: " + r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        Bot b;
        b.account = r.Int(i, 0);
        b.character = r.Int(i, 1);
        b.name = r.Text(i, 2);
        bots.push_back(std::move(b));
    }
    // A run that ended badly may have left sessions behind.
    db.Query("DELETE FROM sro_vt_account.sr_online_sessions s USING sro_vt_account.tb_user u WHERE s.jid = u.jid AND lower(u.struserid) ~ '^bot[0-9]+$'");
    return true;
}

// Places in clusters of 2 x 3 regions across a zone's west edge (regions x - 1 and x, x a
// multiple of the zone width), on regions with navigation, each on a floor a walker can stand on
// (the topmost there, not a blocked tile: checked on the navigation, loaded for the clusters).
bool Spread(server::Database& db, const assets::FileSystem& fs, std::vector<Bot>& bots, const std::set<std::uint16_t>& regions,
            int clusters, int zoneRegions, std::mt19937& random, std::string& error) {
    std::vector<std::uint16_t> centres;
    for (const std::uint16_t r : regions) {
        const int x = r & 0xFF, z = r >> 8;
        if (x % zoneRegions != 0 || x == 0) continue;
        bool all = true;
        for (int dz = -1; dz <= 1 && all; ++dz) {
            for (int dx = -1; dx <= 0 && all; ++dx) all = regions.contains(assets::RegionId(x + dx, z + dz));
        }
        if (all) centres.push_back(r);
    }
    if (centres.empty()) {
        error = "no region pairs across a zone edge with navigation";
        return false;
    }
    std::shuffle(centres.begin(), centres.end(), random);
    centres.resize(std::min<std::size_t>(centres.size(), std::size_t(std::max(1, clusters))));
    // The clusters' navigation.
    core::JobSystem jobs(4);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    std::size_t wanted = 0;
    for (const std::uint16_t c : centres) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 0; ++dx) {
                loader.Request(assets::RegionId((c & 0xFF) + dx, (c >> 8) + dz));
                ++wanted;
            }
        }
    }
    for (int i = 0; i < 6000 && nav.Size() < wanted; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::uniform_real_distribution<float> local(150.0f, 1770.0f);
    std::uniform_int_distribution<int> pickX(-1, 0), pickZ(-1, 1);
    // A place a walker can stand on around a cluster's centre (false after enough tries: mountains,
    // water, a region not loaded).
    const auto standable = [&](std::uint16_t c, proto::WorldPosition& out) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            const std::uint16_t region = assets::RegionId((c & 0xFF) + pickX(random), (c >> 8) + pickZ(random));
            const float x = local(random), z = local(random);
            const float above[3] = {x, 5000.0f, z}, down[3] = {0.0f, -1.0f, 0.0f};
            game::NavHit top;
            if (!nav.Ray(region, above, down, 10000.0f, top) || top.region != region) continue;
            const game::NavPosition at = nav.Drop(region, x, top.y, z);
            if (!at.Valid()) continue; // a blocked tile
            out = {region, x, at.y, z};
            return true;
        }
        return false;
    };
    std::vector<std::uint16_t> usable;
    for (const std::uint16_t c : centres) {
        proto::WorldPosition probe;
        if (standable(c, probe)) usable.push_back(c);
    }
    if (usable.empty()) {
        error = "no cluster has a place to stand";
        return false;
    }
    centres = std::move(usable);
    std::vector<server::CharacterPlace> places;
    for (std::size_t i = 0; i < bots.size(); ++i) {
        const std::uint16_t c = centres[i % centres.size()];
        if (!standable(c, bots[i].at)) {
            error = std::format("no standable place found around region {},{}", c & 0xFF, c >> 8);
            return false;
        }
        places.push_back({bots[i].character, {bots[i].at.region, bots[i].at.x, bots[i].at.y, bots[i].at.z, 0.0f}});
    }
    if (!server::SavePlaces(db, places)) {
        error = "saving the bots' places failed";
        return false;
    }
    core::LogInfo("SroBots: {} bots in {} clusters across zone edges (zones {} regions wide)", bots.size(), centres.size(), zoneRegions);
    return true;
}

// Every bot within `radius` of one spot (--place), each on a floor a walker can stand on (the
// topmost there, not a blocked tile; the navigation of the 3 x 3 regions around it is loaded).
bool Pack(server::Database& db, const assets::FileSystem& fs, std::span<Bot> bots, proto::FlatPosition centre, float radius,
          std::mt19937& random, std::string& error) {
    const proto::WorldPosition middle = proto::FromFlat(centre, 0.0f);
    core::JobSystem jobs(4);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    std::size_t wanted = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            loader.Request(assets::RegionId((middle.region & 0xFF) + dx, (middle.region >> 8) + dz));
            ++wanted;
        }
    }
    for (int i = 0; i < 6000 && nav.Size() < wanted; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::uniform_real_distribution<float> unit(0.0f, 1.0f), angle(0.0f, 6.2831853f);
    std::vector<server::CharacterPlace> places;
    for (Bot& b : bots) {
        bool found = false;
        for (int attempt = 0; attempt < 400 && !found; ++attempt) {
            const float d = radius * std::sqrt(unit(random)), a = angle(random);
            const proto::WorldPosition p = proto::FromFlat({centre.x + d * std::sin(a), centre.z + d * std::cos(a)}, 0.0f);
            const float above[3] = {p.x, 5000.0f, p.z}, down[3] = {0.0f, -1.0f, 0.0f};
            game::NavHit top;
            if (!nav.Ray(p.region, above, down, 10000.0f, top) || top.region != p.region) continue;
            const game::NavPosition at = nav.Drop(p.region, p.x, top.y, p.z);
            if (!at.Valid()) continue; // a blocked tile
            b.at = {p.region, p.x, at.y, p.z};
            b.home = centre;
            found = true;
        }
        if (!found) {
            error = std::format("no standable place within {} of the spot (region {},{})", radius, middle.region & 0xFF, middle.region >> 8);
            return false;
        }
        places.push_back({b.character, {b.at.region, b.at.x, b.at.y, b.at.z, 0.0f}});
    }
    if (!server::SavePlaces(db, places)) {
        error = "saving the bots' places failed";
        return false;
    }
    core::LogInfo("SroBots: {} bots packed within {} of region {},{} ({:.0f}, {:.0f})", bots.size(), radius, middle.region & 0xFF,
                  middle.region >> 8, middle.x, middle.z);
    return true;
}

// --temples: the bots made level 110 members of their league with its suit on (slot 8: ITEM_CH_M_TRADE_TRADER_02, _HUNTER_02
// or _THIEF_02; the template is a Chinese man), bot1 a game master (tb_user.sec_primary 1). `thief` and `hunter` are SQL tests
// of the bots' numbers; the rest are traders.
bool PrepareTemples(server::Database& db, int count, const std::string& thief, const std::string& hunter, std::string& error) {
    const std::string mine = kBots;
    const auto run = [&](const std::string& sql, std::vector<server::Param> params, const char* what) {
        const server::QueryResult r = db.Query(sql, std::move(params));
        if (!r.Ok()) error = std::string(what) + ": " + r.Error();
        return r.Ok();
    };
    return run("UPDATE _char c SET curlevel = 110, maxlevel = 110, expoffset = 0, sexpoffset = 0 WHERE " + mine, {count}, "levels") &&
           run("UPDATE _chartrijob j SET jobtype = CASE WHEN " + thief + " THEN 2 WHEN " + hunter + " THEN 3 ELSE 1 END, level = 7 FROM _char c "
               "WHERE j.charid = c.charid AND " + mine,
               {count}, "leagues") &&
           run("UPDATE _inventory i SET itemid = 0 FROM _char c WHERE i.charid = c.charid AND i.slot = 8 AND " + mine, {count}, "the suits' place") &&
           PlaceNewItems(db,
                         "SELECT c.charid, 8 AS slot, (SELECT id FROM _refobjcommon WHERE codename128 = CASE WHEN " + thief +
                             " THEN 'ITEM_CH_M_TRADE_THIEF_02' WHEN " + hunter + " THEN 'ITEM_CH_M_TRADE_HUNTER_02' ELSE 'ITEM_CH_M_TRADE_TRADER_02' END) "
                             "AS refitemid, 0 AS optlevel, 0 AS variance, 0 AS data "
                             "FROM _char c WHERE " + mine,
                         {count}, "the suits", error) &&
           run("UPDATE sro_vt_account.tb_user SET sec_primary = 1, sec_content = 1 WHERE lower(struserid) = 'bot1'", {}, "the game master");
}

// --temples: the scenario's steps over the bots (played between their turns, on one thread) and its checks.
struct TempleRun {
    static constexpr std::uint16_t kArea = 0x8010, kAnubis = 0x800C;
    static constexpr std::uint32_t kTraderGate = 27751, kTraderTo = 173, kThiefGate = 27752, kThiefTo = 174;
    int traders = 30, hunters = 5, thieves = 15, late = 10, doorEach = 3, least = 30, share = 70;
    // Roles: 1 the game master, 2 a trader, 3 a thief, 4 a hunter, 5 a thief at the traders' gate.
    static bool Thief(int role) { return role == 3 || role == 5; }
    std::optional<proto::TemplePoints> before;
    enum class Phase { Settle, Gates, Board, Open, Doors, Close, Done } phase = Phase::Settle;
    Clock::time_point due{};
    std::vector<std::size_t> queue; // the bots in turn (the gates, the doors)
    std::size_t next = 0;
    Clock::time_point askedAt{};
    int step = 0;
    std::vector<std::size_t> doorBots;
    int failures = 0, passes = 0;

    void Check(const std::string& what, bool ok, const std::string& detail) {
        ++(ok ? passes : failures);
        if (ok) core::LogInfo("SroBots: temples: PASS {}{}", what, detail.empty() ? "" : " (" + detail + ")");
        else core::LogError("SroBots: temples: FAIL {}{}", what, detail.empty() ? "" : " (" + detail + ")");
    }
    static void Say(Bot& b, const std::string& line) {
        proto::ChatSend say;
        say.kind = proto::ChatKind::All;
        say.text = line;
        Send(b, say);
    }
    static bool InArea(const Bot& b) { return b.stage == Bot::Stage::InWorld && b.at.region == kArea; }

    // One step; true when it is done.
    bool Step(std::vector<Bot>& bots, Clock::time_point now) {
        Bot& gm = bots[0];
        const auto after = [&](double seconds) { due = now + std::chrono::milliseconds(int(seconds * 1000.0)); };
        if (now < due) return false;
        switch (phase) {
        case Phase::Settle:
            if (step++ == 0) {
                Say(gm, "/temple CLOSE 10"); // a run before may have left them open
                after(3.0);
                return false;
            }
            // The hunters first (so some are in before the share closes on the traders' alliance), then the traders, the thieves,
            // the late traders and the thief at the wrong gate.
            for (std::size_t i = 1; i < bots.size(); ++i) {
                if (bots[i].templeRole == 4) queue.push_back(i);
            }
            for (std::size_t i = 1; i < bots.size(); ++i) {
                if (bots[i].templeRole != 4) queue.push_back(i);
            }
            phase = Phase::Gates;
            core::LogInfo("SroBots: temples: the gates: {} traders, {} hunters, {} thieves, {} late traders, a thief at the traders' gate", traders,
                          hunters, thieves, late);
            return false;
        case Phase::Gates: {
            if (next < queue.size()) {
                Bot& b = bots[queue[next]];
                if (askedAt == Clock::time_point{}) {
                    b.templeAnswer = -1;
                    b.templeWaiting = true;
                    askedAt = now;
                    Send(b, proto::UseGate{b.templeRole == 3 ? kThiefGate : kTraderGate, b.templeRole == 3 ? kThiefTo : kTraderTo});
                    return false;
                }
                if (b.templeWaiting && now - askedAt < std::chrono::seconds(10)) return false;
                b.templeWaiting = false;
                askedAt = {};
                ++next;
                after(0.25);
                return false;
            }
            // Each answer against the rule (once `least` are in, an alliance's share of `share` % or more refused).
            int inTh = 0, inThief = 0, wrong = 0, refused = 0;
            std::string firstWrong;
            for (const std::size_t i : queue) {
                const Bot& b = bots[i];
                const bool isThief = Thief(b.templeRole);
                const int sum = inTh + inThief, mine = isThief ? inThief : inTh;
                const bool expectRefused = sum >= least && mine * 100 / sum >= share;
                // The thief at the traders' gate: the gate's suit first, "no right".
                const int expected = b.templeRole == 5 ? int(proto::GateStatus::ShrineNoRight) : expectRefused ? int(proto::GateStatus::ShrineRatio) : 0;
                if (b.templeAnswer != expected) {
                    ++wrong;
                    if (firstWrong.empty()) firstWrong = std::format("{}: answered {}, expected {} with {} / {} in", b.name, b.templeAnswer, expected, inTh, inThief);
                }
                if (b.templeAnswer == 0) ++(isThief ? inThief : inTh);
                if (b.templeAnswer == int(proto::GateStatus::ShrineRatio)) ++refused;
            }
            Check("the gates' answers follow the alliances' share rule", wrong == 0,
                  std::format("{} traders and {} thieves in, {} refused for the share{}", inTh, inThief, refused, firstWrong.empty() ? "" : "; " + firstWrong));
            Check("the share refused some late traders", refused > 0, std::format("{} refused", refused));
            Check("the thief at the traders' gate has no right", bots[queue.back()].templeAnswer == int(proto::GateStatus::ShrineNoRight),
                  std::format("answer {}", bots[queue.back()].templeAnswer));
            phase = Phase::Board;
            after(3.0);
            return false;
        }
        case Phase::Board:
            if (step < 100) {
                step = 100;
                for (Bot& b : bots) {
                    if (InArea(b) || &b == &gm) Send(b, proto::TempleReputationAsk{0}); // the icon's board (no NPC)
                }
                bots[1].templePoints.reset();
                Send(bots[1], proto::TemplePointsAsk{}); // the numbers before
                after(4.0);
                return false;
            } else if (step == 101) {
                Say(gm, "/temple REP HAROERIS TRADER 7");
                step = 102;
                after(1.0);
                return false;
            } else if (step == 102) {
                Say(gm, "/temple REP HAROERIS THIEF 2");
                step = 103;
                after(2.5); // the board kept every second on the temples' server
                return false;
            } else if (step == 103) {
                bots[1].templePoints.reset();
                Send(bots[1], proto::TemplePointsAsk{});
                step = 104;
                after(2.0);
                return false;
            } else if (step == 104) {
                const auto& now = bots[1].templePoints;
                const bool ok = before && now && now->haroerisTraderHunter == before->haroerisTraderHunter + 7 &&
                                now->haroerisThief == before->haroerisThief + 2 && now->anubisTraderHunter == before->anubisTraderHunter &&
                                now->anubisThief == before->anubisThief;
                Check("the board's numbers again show the reputation added (0xB4E0)", ok,
                      before && now ? std::format("Haroeris {} / {} -> {} / {}", before->haroerisTraderHunter, before->haroerisThief,
                                                  now->haroerisTraderHunter, now->haroerisThief)
                                    : "no answer");
                phase = Phase::Open;
                step = 0;
                return false;
            } else {
                before = bots[1].templePoints;
                int inside = 0, icons = 0, boards = 0;
                for (const Bot& b : bots) {
                    if (!InArea(b)) continue;
                    ++inside;
                    icons += b.templeIcon == 1 ? 1 : 0;
                    boards += b.templeBoard == 0 ? 1 : 0;
                }
                Check("everyone in the area has the board's icon", inside > 0 && icons == inside, std::format("{} of {}", icons, inside));
                Check("everyone in the area gets the board by the icon", inside > 0 && boards == inside, std::format("{} of {}", boards, inside));
                Check("the game master outside, with no job quest, has no icon", gm.templeIcon != 1, std::format("icon {}", gm.templeIcon));
                Check("the game master outside gets the board by the icon too", gm.templeBoard == 0, std::format("answer {}", gm.templeBoard));
                step = 101;
                return false;
            }
        case Phase::Open:
            if (step == 0) {
                for (Bot& b : bots) b.templeNotices.clear();
                Say(gm, "/temple REP ANUBIS THIEF 5");
                step = 1;
                after(1.0);
                return false;
            }
            if (step == 1) {
                Say(gm, "/temple OPEN 10");
                step = 2;
                after(4.0);
                return false;
            } else {
                int inside = 0, told = 0;
                for (const Bot& b : bots) {
                    if (!InArea(b)) continue;
                    ++inside;
                    for (const proto::TempleNotice& n : b.templeNotices) {
                        if (n.kind == proto::TempleNotice::kOpened && n.region == kAnubis && n.allowed == proto::TempleNotice::kThief) {
                            ++told;
                            break;
                        }
                    }
                }
                Check("the area hears Anubis's temple open to the thieves alone", inside > 0 && told == inside, std::format("{} of {}", told, inside));
                // Some of each league to Anubis's door (a developer's teleport beside it).
                std::map<int, int> sent;
                for (std::size_t i = 1; i < bots.size(); ++i) {
                    Bot& b = bots[i];
                    if (!InArea(b) || b.templeRole == 5) continue;
                    int& n = sent[b.templeRole];
                    if (n >= doorEach) continue;
                    ++n;
                    doorBots.push_back(i);
                    Send(b, proto::DevGotoPlace{kArea, 292.0f, -1985.0f, 41.0f});
                }
                phase = Phase::Doors;
                step = 0;
                next = 0;
                after(5.0);
                return false;
            }
        case Phase::Doors: {
            if (next < doorBots.size()) {
                Bot& b = bots[doorBots[next]];
                if (askedAt == Clock::time_point{}) {
                    std::uint32_t door = 0;
                    float best = 1e9f;
                    const proto::FlatPosition here = proto::ToFlat(b.at);
                    for (const auto& [entity, where] : b.doors) {
                        if (const float d = std::hypot(where.x - here.x, where.z - here.z); d < best) {
                            best = d;
                            door = entity;
                        }
                    }
                    b.templeAnswer = -1;
                    if (door == 0) {
                        core::LogWarning("SroBots: temples: {} sees no door", b.name);
                        ++next;
                        return false;
                    }
                    b.templeWaiting = true;
                    askedAt = now;
                    Send(b, proto::EnterGap{door});
                    return false;
                }
                if (b.templeWaiting && now - askedAt < std::chrono::seconds(8)) return false;
                b.templeWaiting = false;
                askedAt = {};
                ++next;
                after(0.5);
                return false;
            }
            int thievesIn = 0, tradersRefused = 0, thiefCount = 0, traderCount = 0, huntersRefused = 0, hunterCount = 0;
            std::string answers;
            for (const std::size_t i : doorBots) {
                const Bot& b = bots[i];
                answers += std::format("{}{} {}", answers.empty() ? "" : ", ", b.name, b.templeAnswer);
                if (b.templeRole == 3) {
                    ++thiefCount;
                    if (b.templeAnswer == 0 && b.at.region == kAnubis) ++thievesIn;
                } else if (b.templeRole == 4) {
                    ++hunterCount;
                    if (b.templeAnswer == int(proto::GateStatus::ShrineNoRight)) ++huntersRefused;
                } else {
                    ++traderCount;
                    if (b.templeAnswer == int(proto::GateStatus::ShrineNoRight)) ++tradersRefused;
                }
            }
            Check("the thieves go through Anubis's door", thiefCount > 0 && thievesIn == thiefCount, std::format("{} of {}", thievesIn, thiefCount));
            Check("the traders are refused at it (no right)", traderCount > 0 && tradersRefused == traderCount,
                  std::format("{} of {}; answers: {}", tradersRefused, traderCount, answers));
            Check("the hunters are refused at it (no right)", hunterCount > 0 && huntersRefused == hunterCount,
                  std::format("{} of {}", huntersRefused, hunterCount));
            Say(gm, "/temple CLOSE 10");
            phase = Phase::Close;
            step = 0;
            after(4.0);
            return false;
        }
        case Phase::Close: {
            // The run's own reputation taken back (Anubis's went with its decision), then the numbers checked against before.
            if (step == 1) {
                Say(gm, "/temple REP HAROERIS THIEF -2");
                step = 2;
                after(2.5);
                return false;
            }
            if (step == 2) {
                bots[1].templePoints.reset();
                Send(bots[1], proto::TemplePointsAsk{});
                step = 3;
                after(2.0);
                return false;
            }
            if (step == 3) {
                const auto& now = bots[1].templePoints;
                Check("the run's Haroeris reputation taken back", before && now && now->haroerisTraderHunter == before->haroerisTraderHunter &&
                                                                   now->haroerisThief == before->haroerisThief,
                      before && now ? std::format("{} / {}", now->haroerisTraderHunter, now->haroerisThief) : "no answer");
                core::LogInfo("SroBots: temples: {} passed, {} failed", passes, failures);
                phase = Phase::Done;
                return true;
            }
            int back = 0, thiefCount = 0;
            for (const std::size_t i : doorBots) {
                const Bot& b = bots[i];
                if (b.templeRole != 3) continue;
                ++thiefCount;
                if (b.at.region == kArea) ++back;
            }
            Check("the closing sends the thieves inside back to the area", thiefCount > 0 && back == thiefCount, std::format("{} of {}", back, thiefCount));
            Say(gm, "/temple REP HAROERIS TRADER -7");
            step = 1;
            after(1.0);
            return false;
        }
        case Phase::Done: return true;
        }
        return true;
    }
};

const std::array<const char*, 14> kChatLines = {
    "hi Hotan", "anyone for the tomb?", "WTS elixirs, pm me", "LF party Roc", "lol", "brb", "who sells arrows here?",
    "trade?", "nice gear", "is the fortress war today?", "need a healer", "WTB Seal of Sun sword", "gg", "where is the stable?"};

} // namespace

int main(int argc, char** argv) {
    core::Config c;
    std::string error;
    if (!c.ParseCommandLine(argc, argv, error)) {
        core::LogError("SroBots: {}", error);
        return 1;
    }
    std::string dbUrl = c.GetString("db");
    if (dbUrl.empty()) {
        if (const char* env = std::getenv("SR_DB_URL")) dbUrl = env;
    }
    std::string gameDir = c.GetString("game");
    if (gameDir.empty()) {
        if (const char* env = std::getenv("SR_GAME_DIR")) gameDir = env;
    }
    // --temples: its own cast (bot1 the game master, then the traders, the thieves, the late traders).
    const bool temples = c.GetBool("temples", false);
    TempleRun templeRun;
    templeRun.traders = std::max(1, c.GetInt("temple-traders", 30));
    templeRun.thieves = std::max(1, c.GetInt("temple-thieves", 15));
    templeRun.hunters = std::max(1, c.GetInt("temple-hunters", 5));
    templeRun.late = std::max(0, c.GetInt("temple-late", 10));
    templeRun.doorEach = std::max(1, c.GetInt("temple-doors", 3));
    templeRun.least = std::max(1, c.GetInt("temple-least", 30));
    templeRun.share = std::clamp(c.GetInt("temple-share", 70), 1, 100);
    const int count = temples ? 1 + templeRun.traders + templeRun.hunters + templeRun.thieves + templeRun.late + 1 : std::max(1, c.GetInt("bots", 5000));
    const std::uint16_t serverId = std::uint16_t(c.GetInt("server-id", 1));
    const double ramp = std::max(1.0, c.GetDouble("ramp", 250.0));
    const double walkEvery = std::max(0.5, c.GetDouble("walk-every", 4.0));
    const float walkRange = float(c.GetDouble("walk-range", 300.0));
    const double runSeconds = c.GetDouble("run-seconds", 120.0);
    std::mt19937 random(unsigned(c.GetInt("seed", 1)));

    // The shard's tables first, then the accounts'.
    const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);
    auto db = server::Database::Connect(server::WithSearchPath(dbUrl, shard + ", " + server::kAccountSchema), error);
    if (!db) {
        core::LogError("SroBots: the database: {}", error);
        return 1;
    }
    assets::FileSystem fs;
    if (gameDir.empty() || !assets::MountGameArchives(fs, gameDir, error)) {
        core::LogError("SroBots: the game files: {}", gameDir.empty() ? "pass --game DIR or set SR_GAME_DIR" : error);
        return 1;
    }
    std::vector<Bot> bots;
    if (!Prepare(*db, count, c.GetString("template", "Hero"), bots, error)) {
        core::LogError("SroBots: {}", error);
        return 1;
    }
    // --place X,Z[,LX,LZ][;X,Z...]: spots for them (equal batches, in order), else the clusters across zone edges.
    const float packRadius = float(std::max(10.0, c.GetDouble("place-radius", 400.0)));
    if (temples) {
        // The game master and the traders by the desert's traders' and hunters' gate (its building at region 22347 (1627, 554)),
        // the thieves by the thieves' (region 21067 (1470, 301)).
        // Their numbers: 1 the game master, the traders, the hunters, the thieves, the late traders, the thief at the wrong gate.
        const int hunterFrom = 2 + templeRun.traders, hunterTo = hunterFrom + templeRun.hunters - 1;
        const int thiefFrom = hunterTo + 1, thiefTo = thiefFrom + templeRun.thieves - 1;
        const std::string n = "substr(c.charname16, 4)::int";
        const std::string thiefSql = std::format("({} BETWEEN {} AND {} OR {} = {})", n, thiefFrom, thiefTo, n, count);
        const std::string hunterSql = std::format("{} BETWEEN {} AND {}", n, hunterFrom, hunterTo);
        if (!PrepareTemples(*db, count, thiefSql, hunterSql, error)) {
            core::LogError("SroBots: the temples' bots: {}", error);
            return 1;
        }
        std::vector<Bot> traders, thieves;
        for (std::size_t i = 0; i < bots.size(); ++i) {
            const int k = int(i) + 1;
            bots[i].templeRole = k == 1 ? 1 : k == count ? 5 : (k >= thiefFrom && k <= thiefTo) ? 3 : (k >= hunterFrom && k <= hunterTo) ? 4 : 2;
        }
        // Packed in two groups (the thieves by their gate, everyone else by the traders'), then put back in their order.
        std::vector<std::size_t> traderIndex, thiefIndex;
        for (std::size_t i = 0; i < bots.size(); ++i) (bots[i].templeRole == 3 ? thiefIndex : traderIndex).push_back(i);
        for (const std::size_t i : traderIndex) traders.push_back(std::move(bots[i]));
        for (const std::size_t i : thiefIndex) thieves.push_back(std::move(bots[i]));
        if (!Pack(*db, fs, traders, proto::ToFlat({assets::RegionId(75, 87), 1612.0f, 0.0f, 548.0f}), 20.0f, random, error) ||
            !Pack(*db, fs, thieves, proto::ToFlat({assets::RegionId(75, 82), 1460.0f, 0.0f, 300.0f}), 20.0f, random, error)) {
            core::LogError("SroBots: {}", error);
            return 1;
        }
        for (std::size_t k = 0; k < traderIndex.size(); ++k) bots[traderIndex[k]] = std::move(traders[k]);
        for (std::size_t k = 0; k < thiefIndex.size(); ++k) bots[thiefIndex[k]] = std::move(thieves[k]);
    } else if (c.Has("place")) {
        std::vector<proto::FlatPosition> spots;
        const std::string list = c.GetString("place");
        for (std::size_t at = 0; at <= list.size();) {
            std::size_t end = list.find(';', at);
            if (end == std::string::npos) end = list.size();
            if (end > at) {
                float v[4] = {0, 0, 960.0f, 960.0f};
                const int n = std::sscanf(list.substr(at, end - at).c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
                if (n != 2 && n != 4) {
                    core::LogError("SroBots: --place wants X,Z or X,Z,LX,LZ (several separated by ';')");
                    return 1;
                }
                spots.push_back(proto::ToFlat({assets::RegionId(int(v[0]), int(v[1])), v[2], 0.0f, v[3]}));
            }
            at = end + 1;
        }
        for (std::size_t i = 0; i < spots.size(); ++i) {
            const std::size_t from = bots.size() * i / spots.size(), to = bots.size() * (i + 1) / spots.size();
            if (!Pack(*db, fs, std::span<Bot>(bots.data() + from, to - from), spots[i], packRadius, random, error)) {
                core::LogError("SroBots: {}", error);
                return 1;
            }
        }
    } else if (!Spread(*db, fs, bots, NavRegions(fs), c.GetInt("clusters", 50), std::max(1, c.GetInt("zone-regions", 16)), random, error)) {
        core::LogError("SroBots: {}", error);
        return 1;
    }
    // --actions: the kit, from the game's own tables (codenames and skill columns, so the ids are
    // the client's).
    const bool actions = c.GetBool("actions", false);
    const bool ctf = c.GetBool("ctf", false);
    Kit kit;
    if (actions) {
        assets::SkillTable skills;
        assets::ItemTable items;
        if (!skills.Load(fs, error) || !items.Load(fs, error)) {
            core::LogError("SroBots: the game's tables: {}", error);
            return 1;
        }
        PickBuffs(skills, std::max(1, c.GetInt("buffs", 6)), kit);
        if (kit.skills.empty()) core::LogWarning("SroBots: no buff fits the kit's rules; the bots will not cast");
        for (const char* code : {"ITEM_ETC_HP_POTION_01", "ITEM_ETC_MP_POTION_01", "ITEM_ETC_HP_POTION_02", "ITEM_ETC_MP_POTION_02",
                                 "ITEM_ETC_AMMO_ARROW_01", "ITEM_ETC_CURE_ALL_01"}) {
            if (const assets::ItemRow* row = items.Find(std::string_view(code))) {
                kit.stacks.push_back({row->refObjId, std::clamp(row->maxStack, 1, 20)});
            } else {
                core::LogWarning("SroBots: no {} in itemdata; left out of the bag", code);
            }
        }
        if (!PrepareKit(*db, count, kit, error)) {
            core::LogError("SroBots: the kit: {}", error);
            return 1;
        }
        core::LogInfo("SroBots: the kit: {} buffs on {} masteries, {} stacks in the bag, gold", kit.skills.size(), kit.masteries.size(), kit.stacks.size());
    }
    const double castEvery = std::max(0.5, c.GetDouble("cast-every", 10.0)), chatEvery = std::max(1.0, c.GetDouble("chat-every", 25.0));
    const double dropEvery = std::max(1.0, c.GetDouble("drop-every", 30.0)), pickEvery = std::max(0.5, c.GetDouble("pick-every", 8.0));
    const float pickRange = float(c.GetDouble("pick-range", 250.0));
    const double attackEvery = std::max(0.5, c.GetDouble("attack-every", 6.0));
    const float attackRange = float(c.GetDouble("attack-range", 400.0));

    std::string pem;
    {
        std::ifstream file(c.GetString("server-certificate", "dev\\server.pem"), std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        pem = text.str();
    }
    if (pem.empty()) {
        core::LogError("SroBots: no server certificate (--server-certificate FILE)");
        return 1;
    }
    net::ConnectConfig connect;
    connect.host = c.GetString("host", "127.0.0.1");
    connect.port = std::uint16_t(c.GetInt("port", 15781));
    connect.tls = {pem, c.GetString("tls-name", "localhost")};
    net::NetService net(unsigned(std::max(1, c.GetInt("threads", 4))));
    server::InstallStopHandler();

    // The simulated network (none unless asked) and the threads the bots are played on.
    NetSim sim;
    sim.latencyMs = std::max(0.0, c.GetDouble("latency", 0.0));
    sim.jitterMs = std::max(0.0, c.GetDouble("jitter", 0.0));
    sim.loss = std::clamp(c.GetDouble("loss", 0.0), 0.0, 0.5);
    sim.rtoMs = std::max(1.0, c.GetDouble("rto", 250.0));
    core::JobSystem play(c.GetInt("workers", core::JobSystem::kDefaultWorkers));
    for (std::size_t i = 0; i < bots.size(); ++i) {
        bots[i].sim = &sim;
        bots[i].rng.seed(unsigned(c.GetInt("seed", 1)) * 7919u + unsigned(i));
    }
    if (sim.On()) {
        core::LogInfo("SroBots: the network: {:.0f} ms each way (+ up to {:.0f} ms), {:.1f} % of packets lost (each holds the stream {:.0f} ms)",
                      sim.latencyMs, sim.jitterMs, 100.0 * sim.loss, sim.rtoMs);
    }

    const auto after = [](Bot& b, Clock::time_point now, double mean) {
        return now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::exponential_distribution<double>(1.0 / mean)(b.rng)));
    };
    const auto start = Clock::now();
    std::size_t started = 0, inWorld = 0, closed = 0, peakInWorld = 0;
    Tally total;
    std::mutex totalMutex;
    Clock::time_point allIn{}, nextReport = start + std::chrono::seconds(10);

    // One bot's turn: what got through to it, then what it does.
    const auto turn = [&](Bot& b, Clock::time_point now, Tally& t) {
        std::uniform_real_distribution<float> angle(0.0f, 6.2831853f), reach(0.3f, 1.0f), unit(0.0f, 1.0f);
        b.inbox.clear();
        t.received += b.connection->Receive(b.inbox);
        for (const net::Message& m : b.inbox) t.bytes += m.payload.size() + 6;
        // Through the simulated network: what arrived together waits its delay together (its loss
        // rolled per segment of its bytes).
        std::vector<net::Message> ready;
        if (sim.On()) {
            std::size_t arrived = 0;
            for (const net::Message& m : b.inbox) arrived += m.payload.size() + 6;
            const Clock::time_point due = b.inbox.empty() ? now : b.Due(now, b.inLast, arrived);
            for (net::Message& m : b.inbox) b.inQueue.push_back({due, std::move(m)});
            while (!b.inQueue.empty() && b.inQueue.front().due <= now) {
                ready.push_back(std::move(b.inQueue.front().message));
                b.inQueue.pop_front();
            }
        } else {
            ready.swap(b.inbox);
        }
        const auto died = [&] {
            if (b.dead) return;
            b.dead = true;
            b.riseAt = now + std::chrono::milliseconds(3000 + b.rng() % 5000);
            ++t.deaths;
        };
        for (const net::Message& m : ready) {
            switch (m.id) {
            case proto::Welcome::kId:
                if (proto::Welcome w; proto::Decode(m.payload, w) && w.result == proto::WelcomeResult::Accepted) {
                    Send(b, proto::LobbyEnter{b.ticket});
                    b.stage = Bot::Stage::Lobby;
                }
                break;
            case proto::LobbyEntered::kId:
                if (proto::LobbyEntered e; proto::Decode(m.payload, e) && e.status == proto::LobbyEnterStatus::Accepted) {
                    if (b.arriving) { // come over: the server takes it into the world itself
                        b.stage = Bot::Stage::Entering;
                        break;
                    }
                    Send(b, proto::CharacterSelect{b.name});
                    b.stage = Bot::Stage::Selecting;
                } else {
                    core::LogWarning("SroBots: {}: the lobby refused ({})", b.name, int(e.status));
                }
                break;
            case proto::LobbyResult::kId:
                if (proto::LobbyResult r; proto::Decode(m.payload, r) && r.action == proto::LobbyAction::Select && r.status == proto::LobbyStatus::Ok &&
                                          b.stage == Bot::Stage::Selecting) {
                    Send(b, proto::WorldEnter{});
                    b.stage = Bot::Stage::Entering;
                }
                break;
            case proto::WorldEntered::kId:
                if (proto::WorldEntered e; proto::Decode(m.payload, e) && e.status == proto::WorldStatus::Ok) {
                    if (b.stage != Bot::Stage::InWorld) ++t.inWorld;
                    b.stage = Bot::Stage::InWorld;
                    b.entity = e.entity;
                    b.at = e.position;
                    if (b.templeWaiting) { // --temples: through the gate or the door
                        b.templeWaiting = false;
                        b.templeAnswer = 0;
                    }
                    b.doors.clear();
                    b.kinds.clear();
                    b.nextWalk = after(b, now, walkEvery);
                    b.nextCast = after(b, now, castEvery);
                    b.nextChat = after(b, now, chatEvery);
                    b.nextDrop = after(b, now, dropEvery);
                    b.nextPick = after(b, now, pickEvery);
                    b.nextAttack = after(b, now, attackEvery);
                }
                break;
            case proto::EntityMoved::kId:
                if (proto::EntityMoved e; proto::Decode(m.payload, e)) {
                    if (e.entity == b.entity) {
                        b.at = e.to; // where it is going
                        if (b.clickPending) {
                            t.rtts.push_back(std::chrono::duration<double, std::milli>(now - b.clickAt).count());
                            b.clickPending = false;
                        }
                    } else if (const auto it = b.monsters.find(e.entity); it != b.monsters.end()) {
                        it->second = proto::ToFlat(e.to);
                    }
                }
                break;
            case proto::Vitals::kId:
                if (proto::Vitals v; proto::Decode(m.payload, v)) {
                    if (v.hp == 0) died();
                    else b.dead = false;
                }
                break;
            case proto::EntityDamaged::kId:
                if (proto::EntityDamaged d; proto::Decode(m.payload, d)) {
                    ++t.hitsSeen;
                    if (d.hp == 0) b.monsters.erase(d.target); // a monster fell
                    if (d.target == b.entity && d.hp == 0) died();
                }
                break;
            case proto::InventoryList::kId:
                if (proto::InventoryList l; proto::Decode(m.payload, l)) {
                    b.gold = l.gold;
                    b.bag.clear();
                    for (const proto::ItemRecord& r : l.items) {
                        if (r.slot >= 13) b.bag.insert(r.slot);
                    }
                }
                break;
            case proto::ItemMoved::kId:
                if (proto::ItemMoved i; proto::Decode(m.payload, i)) {
                    ++(i.status == proto::ItemStatus::Ok ? t.itemsOk : t.itemsRefused);
                    for (const proto::ItemRecord& r : i.changed) {
                        if (r.slot >= 13) b.bag.insert(r.slot);
                    }
                    for (const std::uint8_t place : i.emptied) b.bag.erase(place);
                }
                break;
            case proto::GoldChanged::kId:
                if (proto::GoldChanged g; proto::Decode(m.payload, g)) {
                    b.gold = g.gold;
                    ++t.goldChanges;
                }
                break;
            case proto::EntityAppeared::kId:
                if (proto::EntityAppeared e; proto::Decode(m.payload, e)) {
                    std::uint8_t kind = 0;
                    if (e.kind == proto::EntityKind::GroundItem) {
                        kind = 3;
                        b.ground[e.entity] = proto::ToFlat(e.position);
                    } else if (e.kind == proto::EntityKind::Monster) {
                        kind = 2;
                        b.monsters[e.entity] = proto::ToFlat(e.moving ? e.to : e.position);
                    } else if (e.character.name.empty()) {
                        kind = 1; // an NPC
                        if (temples && (e.character.model == 27760 || e.character.model == 27759)) b.doors[e.entity] = proto::ToFlat(e.position);
                    }
                    b.kinds[e.entity] = kind;
                }
                break;
            case proto::EntityGone::kId:
                if (proto::EntityGone e; proto::Decode(m.payload, e)) {
                    b.ground.erase(e.entity);
                    b.monsters.erase(e.entity);
                    b.kinds.erase(e.entity);
                    b.doors.erase(e.entity);
                }
                break;
            case proto::SkillUseAnswer::kId:
                if (proto::SkillUseAnswer a; proto::Decode(m.payload, a)) ++t.castAnswers[int(a.status)];
                break;
            case proto::ChatLine::kId:
                ++t.chatLines;
                if (proto::ChatLine l; temples && proto::Decode(m.payload, l) && l.kind == proto::ChatKind::Gm) {
                    core::LogInfo("SroBots: {}: GM: {}", b.name, l.text);
                }
                break;
            case proto::GateRefused::kId:
                if (proto::GateRefused r; temples && proto::Decode(m.payload, r)) {
                    b.templeWaiting = false;
                    b.templeAnswer = int(r.status);
                }
                break;
            case proto::TempleIcon::kId:
                if (proto::TempleIcon i; temples && proto::Decode(m.payload, i)) b.templeIcon = i.show ? 1 : 0;
                break;
            case proto::TempleReputation::kId:
                if (proto::TempleReputation r; temples && proto::Decode(m.payload, r)) b.templeBoard = r.ok && r.rows.size() == 2 ? 0 : 100 + r.code;
                break;
            case proto::TemplePoints::kId:
                if (proto::TemplePoints p; temples && proto::Decode(m.payload, p)) b.templePoints = p;
                break;
            case proto::TempleNotice::kId:
                if (proto::TempleNotice n; temples && proto::Decode(m.payload, n)) b.templeNotices.push_back(n);
                break;
            case proto::FlagWorldNotice::kId:
                if (proto::FlagWorldNotice n; ctf && proto::Decode(m.payload, n)) {
                    if (n.personal == 0 && n.code == std::uint8_t(game::FlagState::Open)) {
                        Send(b, proto::FlagWorldRegister{});
                        core::LogInfo("SroBots: {} registers for Capture the Flag", b.name);
                    } else if (n.personal != 0) {
                        core::LogInfo("SroBots: {}: Capture the Flag's code 0x{:02X}{}", b.name, n.code, n.name.empty() ? "" : " " + n.name);
                    } else if (n.code != std::uint8_t(game::FlagState::Action)) {
                        core::LogInfo("SroBots: {}: Capture the Flag's state {}", b.name, n.code);
                    }
                }
                break;
            case proto::ArenaNotice::kId:
                if (proto::ArenaNotice n; ctf && proto::Decode(m.payload, n)) {
                    if (n.personal == 0 && n.code == std::uint8_t(game::ArenaState::Open)) {
                        Send(b, proto::ArenaRegister{1, n.mode, 1, {}}); // the score kind, as a lone player (random mode)
                        core::LogInfo("SroBots: {} registers for the Battle Arena (mode {})", b.name, n.mode);
                    } else if (n.personal != 0 && n.code != std::uint8_t(game::ArenaCode::Clock)) {
                        core::LogInfo("SroBots: {}: the arena's code 0x{:02X}", b.name, n.code);
                    }
                }
                break;
            case proto::ServerMove::kId:
                if (proto::ServerMove mv; ctf && proto::Decode(m.payload, mv)) {
                    b.ticket = mv.ticket;
                    b.moveTo = std::pair{mv.host, mv.port};
                    core::LogInfo("SroBots: {} moves to {}:{}", b.name, mv.host, mv.port);
                }
                break;
            default: break;
            }
        }
        if (b.moveTo) return; // on to the other server (the main loop connects it there)
        if (ctf && b.stage == Bot::Stage::InWorld && b.dead && now >= b.riseAt) {
            Send(b, proto::Resurrect{1}); // the match's camp (its own 10 s; the server's 30 s otherwise)
            b.riseAt = now + std::chrono::seconds(15);
        }
        if (b.connection->State() == net::ConnectionState::Closed) {
            if (b.stage == Bot::Stage::InWorld) --t.inWorld;
            core::LogWarning("SroBots: {} closed: {}", b.name, b.connection->CloseReason());
            b.stage = Bot::Stage::Closed;
            ++t.closed;
            return;
        }
        // A click around it.
        if (!ctf && !temples && b.stage == Bot::Stage::InWorld && now >= b.nextWalk) {
            const float a = angle(b.rng), d = walkRange * reach(b.rng);
            const proto::FlatPosition from = proto::ToFlat(b.at);
            proto::FlatPosition to{from.x + d * std::sin(a), from.z + d * std::cos(a)};
            // Packed (--place): a walk that would leave its spot heads back into it instead.
            if (b.home && std::hypot(to.x - b.home->x, to.z - b.home->z) > packRadius) {
                const float r = 0.5f * packRadius * std::sqrt(unit(b.rng)), back = angle(b.rng);
                to = {b.home->x + r * std::sin(back), b.home->z + r * std::cos(back)};
            }
            proto::MoveTo move;
            move.target = proto::FromFlat(to, b.at.y);
            Send(b, move);
            ++t.clicks;
            if (!b.clickPending) {
                b.clickAt = now;
                b.clickPending = true;
            }
            b.nextWalk = after(b, now, walkEvery);
        }
        if (actions && b.stage == Bot::Stage::InWorld && b.dead) {
            if (now >= b.riseAt) {
                Send(b, proto::Resurrect{2}); // where it fell (a bot is below level 10)
                ++t.rises;
                b.riseAt = now + std::chrono::seconds(10); // asked again if no answer comes
            }
        } else if (actions && b.stage == Bot::Stage::InWorld) {
            const auto nearest = [&](const std::unordered_map<std::uint32_t, proto::FlatPosition>& things, float range) {
                const proto::FlatPosition here = proto::ToFlat(b.at);
                std::uint32_t best = 0;
                float bestDistance = range;
                for (const auto& [entity, where] : things) {
                    const float d = std::hypot(where.x - here.x, where.z - here.z);
                    if (d < bestDistance) {
                        best = entity;
                        bestDistance = d;
                    }
                }
                return best;
            };
            if (now >= b.nextAttack) {
                if (const std::uint32_t target = nearest(b.monsters, attackRange); target != 0) {
                    Send(b, proto::AttackTarget{target});
                    ++t.attacks;
                }
                b.nextAttack = after(b, now, attackEvery);
            }
            if (now >= b.nextCast && !kit.skills.empty()) {
                Send(b, proto::UseSkill{kit.skills[std::size_t(b.rng() % kit.skills.size())], 0});
                ++t.casts;
                b.nextCast = after(b, now, castEvery);
            }
            if (now >= b.nextChat) {
                proto::ChatSend say;
                say.kind = proto::ChatKind::All;
                say.text = kChatLines[std::size_t(b.rng() % kChatLines.size())];
                Send(b, say);
                ++t.chats;
                b.nextChat = after(b, now, chatEvery);
            }
            if (now >= b.nextDrop) {
                // An item from the bag, else some gold (the bag refills from what is picked up).
                if (!b.bag.empty()) {
                    auto it = b.bag.begin();
                    std::advance(it, std::ptrdiff_t(b.rng() % b.bag.size()));
                    Send(b, proto::DropItem{*it});
                    b.bag.erase(it);
                    ++t.drops;
                } else if (b.gold > 100) {
                    const std::uint64_t amount = 10 + b.rng() % std::min<std::uint64_t>(b.gold - 10, 5000);
                    Send(b, proto::DropGold{amount});
                    b.gold -= amount;
                    ++t.goldDrops;
                }
                b.nextDrop = after(b, now, dropEvery);
            }
            if (now >= b.nextPick) {
                // The nearest thing on the ground within reach of where it is heading.
                if (const std::uint32_t item = nearest(b.ground, pickRange); item != 0) {
                    Send(b, proto::PickUp{item});
                    b.ground.erase(item);
                    ++t.picks;
                }
                b.nextPick = after(b, now, pickEvery);
            }
        }
        b.FlushOut(now);
    };

    for (;;) {
        if (server::StopRequested()) break;
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        // Connecting at the ramp's pace: a ticket, then the connection and the greeting.
        while (started < bots.size() && double(started) < elapsed * ramp) {
            Bot& b = bots[started++];
            b.ticket = server::IssueTicket(*db, b.account, serverId, 120, error);
            if (b.ticket.empty()) {
                core::LogWarning("SroBots: {}: no ticket: {}", b.name, error);
                b.stage = Bot::Stage::Closed;
                ++closed;
                continue;
            }
            b.connection = net.Connect(connect);
            proto::Hello hello;
            hello.client = "SroBots";
            Send(b, hello);
            b.stage = Bot::Stage::Greeting;
        }
        // Every bot's turn, the bots in parallel (each touches only itself; the tallies are summed).
        play.ParallelFor(started, 16, [&](std::size_t begin, std::size_t end) {
            Tally t;
            for (std::size_t i = begin; i < end; ++i) {
                Bot& b = bots[i];
                if (b.stage == Bot::Stage::Waiting || b.stage == Bot::Stage::Closed) continue;
                turn(b, now, t);
            }
            const std::lock_guard lock(totalMutex);
            total.Add(t);
        });
        // --ctf: a bot told to move goes on to that server with its ticket.
        for (std::size_t i = 0; i < started; ++i) {
            Bot& b = bots[i];
            if (!b.moveTo) continue;
            net::ConnectConfig to = connect;
            to.host = b.moveTo->first;
            to.port = b.moveTo->second;
            b.moveTo.reset();
            b.connection->Close();
            if (b.stage == Bot::Stage::InWorld) --inWorld;
            b.connection = net.Connect(to);
            proto::Hello hello;
            hello.client = "SroBots";
            Send(b, hello);
            b.stage = Bot::Stage::Greeting;
            b.arriving = true;
            b.dead = false;
        }
        inWorld = std::size_t(std::int64_t(inWorld) + total.inWorld);
        closed += total.closed;
        total.inWorld = 0;
        total.closed = 0;
        peakInWorld = std::max(peakInWorld, inWorld);
        if (allIn == Clock::time_point{} && inWorld + closed >= bots.size()) {
            allIn = now;
            core::LogInfo("SroBots: {} bots in the world after {:.1f} s ({} closed)", inWorld, elapsed, closed);
        }
        if (now >= nextReport) {
            const double span = 10.0;
            // What the bots see, by kind, and the round trip a click takes.
            std::size_t views = 0, players = 0, npcs = 0, monsters = 0, items = 0, most = 0;
            for (const Bot& o : bots) {
                if (o.stage != Bot::Stage::InWorld) continue;
                ++views;
                most = std::max(most, o.kinds.size());
                for (const auto& [entity, kind] : o.kinds) {
                    if (kind == 0) ++players;
                    else if (kind == 1) ++npcs;
                    else if (kind == 2) ++monsters;
                    else ++items;
                }
            }
            const double v = double(std::max<std::size_t>(1, views));
            core::LogInfo("SroBots: {:.0f} s: {} started, {} in the world, {} closed; in: {:.0f} messages/s, {:.1f} MB/s ({:.1f} KB/s a bot); "
                          "{:.0f} clicks/s",
                          elapsed, started, inWorld, closed, double(total.received) / span, double(total.bytes) / span / 1e6,
                          double(total.bytes) / span / 1e3 / v, double(total.clicks) / span);
            core::LogInfo("SroBots:   a view holds {:.0f} on average ({:.0f} players, {:.0f} monsters, {:.0f} items, {:.0f} NPCs), {} at most; "
                          "click to its leg: {:.0f} ms median, {:.0f} ms p95, {:.0f} ms p99 ({} clicks)",
                          double(players + npcs + monsters + items) / v, double(players) / v, double(monsters) / v, double(items) / v,
                          double(npcs) / v, most, Percentile(total.rtts, 0.5), Percentile(total.rtts, 0.95), Percentile(total.rtts, 0.99),
                          total.rtts.size());
            if (actions) {
                std::string answers;
                for (const auto& [status, n] : total.castAnswers) answers += std::format("{}{}:{}", answers.empty() ? "" : " ", status, n);
                core::LogInfo("SroBots:   in 10 s: {} casts (answers by status: {}), {} chat lines sent ({} heard), {} item drops, {} gold drops, "
                              "{} pickups asked; item answers {} ok, {} refused; gold changed {} times; {} attacks asked, {} hits seen, "
                              "{} deaths, {} rises asked",
                              total.casts, answers.empty() ? "none" : answers, total.chats, total.chatLines, total.drops, total.goldDrops, total.picks,
                              total.itemsOk, total.itemsRefused, total.goldChanges, total.attacks, total.hitsSeen, total.deaths, total.rises);
            }
            total = Tally{};
            nextReport = now + std::chrono::seconds(10);
        }
        // --temples: the scenario once everyone is in (a few seconds to settle), to its end.
        if (temples && allIn != Clock::time_point{} && now - allIn >= std::chrono::seconds(3) && templeRun.Step(bots, now)) break;
        if (!temples && allIn != Clock::time_point{} && std::chrono::duration<double>(now - allIn).count() >= runSeconds) break;
        net.WaitForActivity(std::chrono::milliseconds(sim.On() ? 2 : 10));
    }
    core::LogInfo("SroBots: done: at most {} in the world, {} closed", peakInWorld, closed);
    for (Bot& b : bots) {
        if (b.connection) b.connection->Close();
    }
    if (temples) return closed == 0 && templeRun.failures == 0 && templeRun.phase == TempleRun::Phase::Done ? 0 : 2;
    return closed == 0 ? 0 : 1;
}
