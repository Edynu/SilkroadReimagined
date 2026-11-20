// Item sets, 1:1 with the original's set operator: the group a count picks, the operator's play of moves, breaks, level
// changes and the entry (with
// the original's ways: a piece put on without the group changing keeps its whites, a piece taken off takes the set's
// resists away, a set of one piece takes the group found before it), the 1.188 data, and the sets on a game server
// (the pieces' total plus raised, the absorptions and the resists through DevSheet). The game files' case needs
// SR_GAME_DIR, the server's SR_DB_URL; otherwise skipped.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/core/Text.h"
#include "sr/game/Items.h"
#include "sr/game/Sets.h"
#include "sr/game/States.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;
using game::SetPiece;
using game::SetTable;
using game::SetWear;

namespace {

// The 1.188 sets this file plays with, as refsetitemgroup.txt and refmagicoptgroup.txt have them (tables of data):
// the ROC set 1 (no group), set A's Chinese men's heavy armour 13 (groups 2 / 3 / 4 at 2 / 4 / 6 pieces), its
// accessories 25 (5 / 6 at 2 / 4), set B's armour 26 (7 / 8 / 9) and accessories 38 (10 / 11), set A's weapons 39.
SetTable Table() {
    const auto set = [](int id, const char* code, std::uint32_t mask, std::array<std::uint32_t, 10> groups) {
        game::SetGroup s;
        s.id = id;
        s.codename = code;
        s.magicMask = mask;
        s.groups = groups;
        return s;
    };
    const std::vector<game::SetGroup> sets = {set(1, "ITEMSET_ROC_CH_M_RED_LIGHTNING", 0, {}),
                                              set(13, "ITEM_11_SET_A_CH_M_HEAVY", 21, {2, 0, 3, 0, 4}),
                                              set(25, "ITEM_11_SET_A_ACCESSORY", 5, {5, 0, 6}),
                                              set(26, "ITEM_11_SET_B_CH_M_HEAVY", 21, {7, 0, 8, 0, 9}),
                                              set(38, "ITEM_11_SET_B_ACCESSORY", 5, {10, 0, 11}),
                                              set(39, "ITEM_11_SET_A_WEAPON", 0, {})};
    const auto row = [](int link, int type, std::uint32_t option, int value) {
        assets::MagicOptGroupRow g;
        g.link = link;
        g.magicType = type;
        g.optId = option;
        g.optLevel = type == 1 ? 10 : 1;
        g.value = value;
        return g;
    };
    const std::vector<assets::MagicOptGroupRow> groups = {
        row(1, 1, 357, 6),  row(1, 1, 358, 6),   row(1, 1, 359, 60), row(1, 1, 362, 160), row(1, 1, 363, 6), // MAGIC_NOR_ROCSET_001
        row(2, 2, 389, 1),  row(3, 2, 389, 1),   row(3, 2, 390, 10), row(4, 2, 389, 2),   row(4, 2, 390, 10),
        row(5, 2, 389, 1),  row(6, 2, 389, 2),   row(7, 2, 389, 1),  row(8, 2, 389, 2),   row(8, 2, 390, 10),
        row(9, 2, 389, 4),  row(9, 2, 390, 10),  row(10, 2, 389, 2), row(11, 2, 389, 4)};
    const auto option = [](std::uint32_t id, const char* name, std::uint32_t code) {
        assets::MagicOptionRow o;
        o.paramId = id;
        o.optionName = name;
        o.attribute = code;
        return o;
    };
    const std::vector<assets::MagicOptionRow> options = {option(357, "MATTR_STR_SET", 0x737472), option(389, "MATTR_REINFORCE_ITEM_SET", game::kSetPlusCode),
                                                         option(390, "MATTR_RESIST_ALL_SET", game::kSetResistCode)};
    return game::MakeSetTable(sets, groups, options);
}

SetPiece Piece(std::int64_t serial, int place, int setId) {
    SetPiece p;
    p.serial = serial;
    p.refId = std::uint32_t(serial);
    p.place = place;
    p.setId = setId;
    p.stamp = std::uint64_t(serial) * 7;
    return p;
}

} // namespace

TEST_CASE("Sets: a count picks one group, the most pieces at or under it") {
    const SetTable t = Table();
    // Set A's accessories: none at 1, +1 at 2 and 3, +2 at 4.
    const std::uint32_t accessoryA[] = {0, 0, 5, 5, 6, 6};
    for (int n = 0; n < 6; ++n) CHECK(t.GroupFor(25, n) == accessoryA[n]);
    CHECK(t.PlusOf(5) == 1);
    CHECK(t.PlusOf(6) == 2);
    CHECK(t.ResistOf(6) == 0);
    // Set B's: +2, +4.
    CHECK(t.PlusOf(t.GroupFor(38, 2)) == 2);
    CHECK(t.PlusOf(t.GroupFor(38, 4)) == 4);
    // Armour A: +1 at 2 and 3; +1 and resists 10 at 4 and 5; +2 and 10 at 6 and over.
    const int plusA[] = {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2};
    const int resistA[] = {0, 0, 0, 0, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};
    for (int n = 0; n < 14; ++n) {
        CHECK(t.PlusOf(t.GroupFor(13, n)) == plusA[n]);
        CHECK(t.ResistOf(t.GroupFor(13, n)) == resistA[n]);
    }
    // Armour B: +1, then +2 and 10, then +4 and 10.
    CHECK(t.PlusOf(t.GroupFor(26, 3)) == 1);
    CHECK(t.PlusOf(t.GroupFor(26, 5)) == 2);
    CHECK(t.PlusOf(t.GroupFor(26, 6)) == 4);
    CHECK(t.ResistOf(t.GroupFor(26, 6)) == 10);
    // The ROC sets and the weapons' have no group; an unknown set none; MagicType 1 (the ROC drop's blues) is no set row.
    for (int n = 0; n < 8; ++n) {
        CHECK(t.GroupFor(1, n) == 0);
        CHECK(t.GroupFor(39, n) == 0);
        CHECK(t.GroupFor(99, n) == 0);
    }
    CHECK(t.OptionsOf(1).empty());
}

TEST_CASE("Sets: the operator plays the moves as the original does (CSetMagicOptionOP)") {
    const SetTable t = Table();
    SetWear w;
    std::vector<SetPiece> worn;
    const auto raise = [&](std::int64_t serial) {
        for (const SetPiece& p : worn) {
            if (p.serial == serial) return w.RaiseOf(p);
        }
        return w.RaiseOf(serial, std::uint64_t(serial) * 7);
    };
    int level = 101;
    const auto put = [&](const SetPiece& p) {
        worn.push_back(p);
        w.Update(t, worn, level);
    };
    const auto take = [&](std::int64_t serial) {
        std::erase_if(worn, [&](const SetPiece& p) { return p.serial == serial; });
        w.Update(t, worn, level);
    };
    w.Update(t, worn, level); // the entry, nothing worn

    SUBCASE("set A's accessories one by one: the third keeps its whites, the fourth raises them all") {
        put(Piece(1, 9, 25)); // the earring
        CHECK(raise(1) == 0);
        put(Piece(2, 10, 25)); // the necklace: 2 pieces, +1 on both
        CHECK(raise(1) == 1);
        CHECK(raise(2) == 1);
        CHECK(w.Group(25) == 5);
        put(Piece(3, 11, 25)); // a ring: 3 pieces, the group of 2 still: not looked at again, the ring's whites its own
        CHECK(raise(3) == 0);
        CHECK(raise(1) == 1);
        put(Piece(4, 12, 25)); // the other ring: 4 pieces, +2 on all four
        for (int s = 1; s <= 4; ++s) CHECK(raise(s) == 2);
        CHECK(w.ResistAll() == 0);
        // A ring off: 3 pieces, the group of 2 again: the three worn made again with +1; the ring off keeps its +2.
        take(4);
        CHECK(w.Pieces(25) == 3);
        CHECK(raise(1) == 1);
        CHECK(raise(3) == 1);
        CHECK(w.RaiseOf(4, 28) == 2);
        // Its whites made again by alchemy (another stamp): no raise.
        CHECK(w.RaiseOf(4, 29) == 0);
        // All off: the set forgotten.
        take(3);
        take(2);
        take(1);
        CHECK(w.Pieces(25) == 0);
        CHECK(w.Group(25) == 0);
    }
    SUBCASE("armour A's resists: on with the group of 4, gone with a piece taken off until the group changes") {
        for (int k = 0; k < 4; ++k) put(Piece(10 + k, k, 13));
        CHECK(w.ResistAll() == 10);
        for (int k = 0; k < 4; ++k) CHECK(raise(10 + k) == 1);
        game::StateDefence d;
        w.AddResists(d);
        for (int e = 0; e < 6; ++e) CHECK(d.elementResist[std::size_t(e)] == 10);
        CHECK(d.percent[15] == 10); // combustion
        CHECK(d.percent[8] == 10);  // disease
        CHECK(d.percent[0] == 10);  // sleep
        CHECK(d.percent[3] == 10);  // fear
        CHECK(d.percent[7] == 0);   // stun: not among 'rals''s
        // A fifth: the same group, nothing changes.
        put(Piece(14, 4, 13));
        CHECK(w.ResistAll() == 10);
        CHECK(raise(14) == 0);
        // The fifth off: 4 pieces, the same group: the set's resists taken off with the piece and not put back.
        take(14);
        CHECK(w.ResistAll() == 0);
        CHECK(raise(10) == 1);
        // Two off and one back: 3 (the group of 2), then 4 (the group of 4 again: its resists back).
        take(13);
        CHECK(w.Group(13) == 2);
        put(Piece(13, 3, 13));
        CHECK(w.ResistAll() == 10);
        // A level changed: every piece taken out and installed again: the resists gone.
        level = 102;
        w.Update(t, worn, level);
        CHECK(w.ResistAll() == 0);
        CHECK(raise(10) == 1);
        // A piece breaking takes them away too (back with the next change of group).
        take(13);
        put(Piece(13, 3, 13));
        CHECK(w.ResistAll() == 10);
        worn[0].broken = true;
        w.Update(t, worn, level);
        CHECK(w.ResistAll() == 0);
    }
    SUBCASE("a swap within the set: the new piece keeps its own whites") {
        put(Piece(20, 9, 25));
        put(Piece(21, 11, 25));
        CHECK(raise(21) == 1);
        // The ring replaced by another of the set in one move: the count stays, the group too.
        worn[1] = Piece(22, 11, 25);
        w.Update(t, worn, level);
        CHECK(w.Pieces(25) == 2);
        CHECK(raise(22) == 0);
        CHECK(raise(20) == 1);
    }
    SUBCASE("a broken piece ends the pass of the raise") {
        SetPiece broken = Piece(31, 10, 25);
        broken.broken = true;
        worn = {Piece(30, 9, 25), broken, Piece(32, 11, 25)};
        w.Reset();
        w.Update(t, worn, level); // the entry: 3 pieces, the group of 2
        CHECK(raise(30) == 1);
        CHECK(raise(31) == 1); // made with it, then installs nothing
        CHECK(raise(32) == 0); // never reached
    }
}

TEST_CASE("Sets: a set's change installs an unfit worn piece anyway, until it is next taken out") {
    const SetTable t = Table();
    SetWear w;
    std::vector<SetPiece> worn;
    w.Update(t, worn, 101); // the entry, nothing worn
    // The earring worn, then its masteries lowered (a skill withdrawal): worn, unfit, nothing of it counted.
    worn.push_back(Piece(1, 9, 25));
    w.Update(t, worn, 101);
    worn[0].fits = false;
    w.Update(t, worn, 101);
    CHECK_FALSE(w.Forced(worn[0]));
    // The necklace put on: set 25 at 2 pieces, its group made each worn piece of the set again and installed it, the
    // unfit earring too (no requirements test there): it counts from now on.
    worn.push_back(Piece(2, 10, 25));
    w.Update(t, worn, 101);
    CHECK(w.Forced(worn[0]));
    CHECK_FALSE(w.Forced(worn[1]));
    // A level changed: every worn piece taken out and installed again, its requirements tested: the earring no more.
    w.Update(t, worn, 102);
    CHECK_FALSE(w.Forced(worn[0]));
}

TEST_CASE("Sets: the entry counts every piece, then installs each; a set of one takes the group found before it") {
    const SetTable t = Table();
    SetWear w;
    // Armour A whole and one accessory of set A: the armour's group of 6 (+2, resists 10) is found first and kept for the
    // pass; the accessory's set (one piece, no group of its own) takes it: its ring +2 and resists 10 more.
    std::vector<SetPiece> worn;
    for (int k = 0; k < 6; ++k) worn.push_back(Piece(40 + k, k, 13));
    worn.push_back(Piece(50, 11, 25));
    w.Update(t, worn, 101);
    CHECK(w.Group(13) == 4);
    for (int k = 0; k < 6; ++k) CHECK(w.RaiseOf(worn[std::size_t(k)]) == 2);
    CHECK(w.Group(25) == 4);
    CHECK(w.RaiseOf(worn[6]) == 2);
    CHECK(w.ResistAll() == 20);
    // A second ring of the set: its own group of 2 (+1 on both rings), its resists gone.
    worn.push_back(Piece(51, 12, 25));
    w.Update(t, worn, 101);
    CHECK(w.Group(25) == 5);
    CHECK(w.RaiseOf(worn[6]) == 1);
    CHECK(w.RaiseOf(worn[7]) == 1);
    CHECK(w.ResistAll() == 10);
    // The set alone, entered again (a relog): no group borrowed.
    SetWear alone;
    alone.Update(t, {Piece(50, 11, 25)}, 101);
    CHECK(alone.Group(25) == 0);
    CHECK(alone.RaiseOf(Piece(50, 11, 25)) == 0);
}

TEST_CASE("Sets: the 1.188 data (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE(assets::MountGameArchives(fs, dir, error));
    std::vector<assets::MagicOptionRow> options;
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
    REQUIRE(file);
    REQUIRE(assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), options, error));
    SetTable t;
    REQUIRE_MESSAGE(game::LoadSetTable(fs, options, nullptr, t, error), error);
    CHECK(t.Size() == 42);
    // Every set row and its groups as the hand-made table above has them (and the database: measured equal).
    const SetTable mine = Table();
    for (const int id : {1, 13, 25, 26, 38, 39}) {
        REQUIRE(t.Find(id) != nullptr);
        CHECK(t.Find(id)->groups == mine.Find(id)->groups);
        for (int n = 0; n < 8; ++n) {
            CHECK(t.PlusOf(t.GroupFor(id, n)) == mine.PlusOf(mine.GroupFor(id, n)));
            CHECK(t.ResistOf(t.GroupFor(id, n)) == mine.ResistOf(mine.GroupFor(id, n)));
        }
    }
    // The twelve ROC sets have no group, the armour's 2 / 4 / 6 and the accessories' 2 / 4, the weapons' and shields' none.
    int roc = 0, armour = 0, accessory = 0, none = 0;
    for (const auto& [id, s] : t.Sets()) {
        const bool any = s.groups[0] != 0 || s.groups[2] != 0 || s.groups[4] != 0;
        if (s.codename.starts_with("ITEMSET_ROC")) roc += any ? 100 : 1;
        else if (s.codename.ends_with("ACCESSORY")) accessory += s.groups[0] != 0 && s.groups[2] != 0 && s.groups[4] == 0 ? 1 : 100;
        else if (s.codename.ends_with("WEAPON") || s.codename.ends_with("SHIELD")) none += any ? 100 : 1;
        else armour += s.groups[0] != 0 && s.groups[2] != 0 && s.groups[4] != 0 ? 1 : 100;
        // The mask the loader checks: a bit for each group.
        for (std::size_t i = 0; i < s.groups.size(); ++i) CHECK(((s.magicMask >> i) & 1u) == (s.groups[i] != 0 ? 1u : 0u));
    }
    CHECK(roc == 12);
    CHECK(armour == 24);
    CHECK(accessory == 2);
    CHECK(none == 4);
    // Every item with a set names one the table has; set A's accessories are China's and Europe's, plain and sealed.
    assets::ItemTable items;
    REQUIRE(items.Load(fs, error));
    std::map<int, int> bySet;
    for (const auto& [code, row] : items.rows()) {
        if (row.setId == 0) continue;
        CHECK_MESSAGE(t.Find(row.setId) != nullptr, row.codename);
        ++bySet[row.setId];
    }
    CHECK(bySet[25] == 12);
    CHECK(bySet[13] == 12);
    // A ring's whites with its set's raise are its whites at that much more plus (the total plus's extra).
    const assets::ItemRow* ring = items.Find("ITEM_CH_RING_11_SET_A");
    REQUIRE(ring != nullptr);
    CHECK(ring->setId == 25);
    game::ItemInstance plain;
    plain.refId = ring->refObjId;
    plain.variance = 0x3FFFFFFFFFFFFFFFull;
    game::ItemInstance plus2 = plain;
    plus2.plus = 2;
    CHECK(game::ComputeStats(*ring, plain, &options, 2).phyAbsorption.low == doctest::Approx(game::ComputeStats(*ring, plus2, &options).phyAbsorption.low));
    CHECK(game::ComputeStats(*ring, plain, &options, 2).phyAbsorption.low > game::ComputeStats(*ring, plain, &options).phyAbsorption.low);
}

namespace {

constexpr std::uint32_t kChest = 932001, kLegs = 932002, kShoes = 932003, kSword = 932010;
constexpr std::uint32_t kEarring = 932101, kNecklace = 932102, kRing = 932103;
constexpr std::uint32_t kHelm = 932201, kShoulders = 932202, kMail = 932203, kGloves = 932204, kPants = 932205;

assets::ItemRow Equipment(std::uint32_t id, const char* codename, std::array<int, 4> tid, int setId) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = tid;
    r.inService = true;
    r.maxStack = 1;
    r.country = 3;
    r.sex = 2;
    r.requirementTypes = {-1, -1, -1, -1};
    r.setId = setId;
    r.stats.assign(117 - assets::ItemRow::kFirstStatColumn + 1, 0.0f);
    return r;
}

// Set A's Chinese accessories' absorptions (cols 71..73, 79..81: low, high, per plus) as the 1.188 rows have them.
assets::ItemRow Accessory(std::uint32_t id, const char* codename, int part, float low, float high, float inc) {
    assets::ItemRow r = Equipment(id, codename, {3, 1, 5, part}, 25);
    for (const int base : {71, 79}) {
        r.stats[std::size_t(base - assets::ItemRow::kFirstStatColumn)] = low;
        r.stats[std::size_t(base + 1 - assets::ItemRow::kFirstStatColumn)] = high;
        r.stats[std::size_t(base + 2 - assets::ItemRow::kFirstStatColumn)] = inc;
    }
    return r;
}

std::shared_ptr<assets::ItemTable> SetItems() {
    auto items = std::make_shared<assets::ItemTable>();
    // The starting armour heavy too (one family worn at a time: MixedArmour).
    items->Add(Equipment(kChest, "ITEM_CH_M_HEAVY_01_BA_A_DEF", {3, 1, 3, 3}, 0));
    items->Add(Equipment(kLegs, "ITEM_CH_M_HEAVY_01_LA_A_DEF", {3, 1, 3, 4}, 0));
    items->Add(Equipment(kShoes, "ITEM_CH_M_HEAVY_01_FA_A_DEF", {3, 1, 3, 6}, 0));
    items->Add(Equipment(kSword, "ITEM_CH_SWORD_01_A_DEF", {3, 1, 6, 2}, 0));
    items->Add(Accessory(kEarring, "ITEM_CH_EARRING_11_SET_A", 1, 25.3f, 25.8f, 0.25f));
    items->Add(Accessory(kNecklace, "ITEM_CH_NECKLACE_11_SET_A", 2, 29.6f, 30.2f, 0.29f));
    items->Add(Accessory(kRing, "ITEM_CH_RING_11_SET_A", 3, 23.0f, 23.5f, 0.23f));
    items->Add(Equipment(kHelm, "ITEM_CH_M_HEAVY_11_HA_SET_A", {3, 1, 3, 1}, 13));
    items->Add(Equipment(kShoulders, "ITEM_CH_M_HEAVY_11_SA_SET_A", {3, 1, 3, 2}, 13));
    items->Add(Equipment(kMail, "ITEM_CH_M_HEAVY_11_BA_SET_A", {3, 1, 3, 3}, 13));
    items->Add(Equipment(kGloves, "ITEM_CH_M_HEAVY_11_AA_SET_A", {3, 1, 3, 5}, 13));
    items->Add(Equipment(kPants, "ITEM_CH_M_HEAVY_11_LA_SET_A", {3, 1, 3, 4}, 13));
    return items;
}

server::StoredItem Stored(std::uint32_t refId) {
    server::StoredItem i;
    i.refId = refId;
    return i;
}

struct SetServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit SetServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, kChest, kLegs, kShoes);
        rules->creation.AddWeapon(game::CreationRules::Race::China, kSword);
        rules->items = SetItems();
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.logoutCountdown = std::chrono::seconds(0);
        config.databaseThreads = 2;
        config.devCommands = true; // DevSheet
        config.sets = std::make_shared<SetTable>(Table());
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::string name;
        std::int64_t account = 0, character = 0;
        proto::InventoryList bag;
    };
    Player Make(const std::string& name, const std::vector<server::StoredItem>& items) {
        Player p;
        p.name = name;
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        p.account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(2ms); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), p.account, config.id, 30, error);
        p.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.client->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = kChest;
        m.legs = kLegs;
        m.shoes = kShoes;
        m.weapon = kSword;
        p.client->Send(m);
        proto::LobbyResult r;
        REQUIRE(p.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        p.character = server::ListCharacters(t.Db(), p.account, config.id, 0, ok)[0].id;
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = 1000;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
        REQUIRE(server::GiveItems(t.Db(), config.id, name, items, 0, error) == server::GiveResult::Done);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {p.account, int(config.id)}).Ok());
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        REQUIRE(p.client->Expect(p.bag));
        return p;
    }
    // The bag's places of an item (in the order they come).
    std::vector<std::uint8_t> PlacesOf(const Player& p, std::uint32_t refId) {
        std::vector<std::uint8_t> out;
        for (const proto::ItemRecord& i : p.bag.items) {
            if (i.item == refId && i.slot >= game::slot::kWorn) out.push_back(i.slot);
        }
        return out;
    }
    bool Move(Player& p, std::uint8_t from, std::uint8_t to) {
        p.client->Send(proto::ItemMove{from, to, 0});
        proto::ItemMoved moved;
        return p.client->Expect(moved) && moved.status == proto::ItemStatus::Ok;
    }
    std::map<std::string, float> Sheet(Player& p) {
        p.client->Send(proto::DevSheet{});
        proto::DevSheetAnswer a;
        std::map<std::string, float> out;
        if (p.client->Expect(a)) {
            for (const proto::DevNumber& n : a.numbers) out[n.name] = n.value;
        }
        return out;
    }
};

} // namespace

TEST_CASE("Game server: a set's pieces worn raise their plus and its numbers") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SetServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto number = [](const std::map<std::string, float>& m, const char* name) {
        const auto it = m.find(name);
        return it != m.end() ? double(it->second) : 0.0;
    };

    SUBCASE("set A's accessories: +1 at 2 pieces, the third as it is, +2 at 4; a ring off, +1 on the three") {
        auto p = s.Make("Jewels", {Stored(kEarring), Stored(kNecklace), Stored(kRing), Stored(kRing)});
        const std::uint8_t earring = s.PlacesOf(p, kEarring).at(0), necklace = s.PlacesOf(p, kNecklace).at(0);
        const std::vector<std::uint8_t> rings = s.PlacesOf(p, kRing);
        REQUIRE(rings.size() == 2);
        CHECK(number(s.Sheet(p), "phyAbsorption") == doctest::Approx(0.0));
        REQUIRE(s.Move(p, earring, game::slot::kEarring));
        auto sheet = s.Sheet(p);
        CHECK(number(sheet, "phyAbsorption") == doctest::Approx(25.3).epsilon(1e-4));
        CHECK(number(sheet, "setPlus9") == 0);
        REQUIRE(s.Move(p, necklace, game::slot::kNecklace));
        sheet = s.Sheet(p);
        CHECK(number(sheet, "setPlus9") == 1);
        CHECK(number(sheet, "setPlus10") == 1);
        CHECK(number(sheet, "phyAbsorption") == doctest::Approx(25.3 + 0.25 + 29.6 + 0.29).epsilon(1e-4));
        CHECK(number(sheet, "magAbsorption") == doctest::Approx(25.3 + 0.25 + 29.6 + 0.29).epsilon(1e-4));
        REQUIRE(s.Move(p, rings[0], game::slot::kRingLeft));
        sheet = s.Sheet(p);
        CHECK(number(sheet, "setPlus11") == 0); // the group of 2 still: the ring as it is
        CHECK(number(sheet, "phyAbsorption") == doctest::Approx(25.3 + 0.25 + 29.6 + 0.29 + 23.0).epsilon(1e-4));
        REQUIRE(s.Move(p, rings[1], game::slot::kRingRight));
        sheet = s.Sheet(p);
        for (const char* place : {"setPlus9", "setPlus10", "setPlus11", "setPlus12"}) CHECK(number(sheet, place) == 2);
        CHECK(number(sheet, "phyAbsorption") == doctest::Approx(25.3 + 0.5 + 29.6 + 0.58 + 2 * (23.0 + 0.46)).epsilon(1e-4));
        // The right ring off into the bag: 3 pieces, the group of 2 again, +1 on the three.
        REQUIRE(s.Move(p, game::slot::kRingRight, rings[1]));
        sheet = s.Sheet(p);
        CHECK(number(sheet, "setPlus12") == 0);
        CHECK(number(sheet, "setPlus11") == 1);
        CHECK(number(sheet, "phyAbsorption") == doctest::Approx(25.3 + 0.25 + 29.6 + 0.29 + 23.0 + 0.23).epsilon(1e-4));
        CHECK(number(sheet, "resistBurn") == 0); // no resists on the accessories' groups
    }
    SUBCASE("armour A's resists at 4 pieces; a fifth taken off takes them away (the group unchanged)") {
        auto p = s.Make("Plates", {Stored(kHelm), Stored(kShoulders), Stored(kGloves), Stored(kMail), Stored(kPants)});
        REQUIRE(s.Move(p, s.PlacesOf(p, kHelm).at(0), game::slot::kHead));
        REQUIRE(s.Move(p, s.PlacesOf(p, kShoulders).at(0), game::slot::kShoulders));
        REQUIRE(s.Move(p, s.PlacesOf(p, kGloves).at(0), game::slot::kHands));
        auto sheet = s.Sheet(p);
        CHECK(number(sheet, "setPlus0") == 1);
        CHECK(number(sheet, "resistBurn") == 0);
        // The mail over the starting chest (a swap): 4 pieces, the group of 4: +1 and the resists.
        REQUIRE(s.Move(p, s.PlacesOf(p, kMail).at(0), game::slot::kChest));
        sheet = s.Sheet(p);
        for (const char* e : {"resistFreeze", "resistFrostbite", "resistShock", "resistBurn", "resistPoison", "resistZombie", "resistSleep", "resistFear",
                              "resistDisease", "resistCombustion"}) {
            CHECK_MESSAGE(number(sheet, e) == 10, e);
        }
        CHECK(number(sheet, "resistStun") == 0);
        CHECK(number(sheet, "setPlus1") == 1);
        // The pants over the starting legs: 5 pieces, the same group: nothing changes, the pants as they are.
        REQUIRE(s.Move(p, s.PlacesOf(p, kPants).at(0), game::slot::kLegs));
        sheet = s.Sheet(p);
        CHECK(number(sheet, "resistBurn") == 10);
        CHECK(number(sheet, "setPlus4") == 0);
        // The pants off into an empty place (the bag's 30 and 31 are: five items came in at 13..): 4 pieces, the same
        // group; the set's resists went with the piece.
        constexpr std::uint8_t kFree = 30;
        REQUIRE(s.Move(p, game::slot::kLegs, kFree));
        sheet = s.Sheet(p);
        CHECK(number(sheet, "resistBurn") == 0);
        CHECK(number(sheet, "setPlus1") == 1);
        // The helm off and on again: 3, then 4 pieces: the group changes twice, the resists back.
        REQUIRE(s.Move(p, game::slot::kHead, kFree + 1));
        CHECK(number(s.Sheet(p), "resistBurn") == 0);
        REQUIRE(s.Move(p, kFree + 1, game::slot::kHead));
        CHECK(number(s.Sheet(p), "resistBurn") == 10);
    }
}
