// The Forgotten World: the catalog from the original's tables and the 1.188 files,
// the layers, an instance opened by a hole and entered through its gap, its triggers opening its doors, the
// collection book.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <thread>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Import.h"
#include "sr/server/InstanceStore.h"
#include "sr/server/Instances.h"
#include "sr/server/Monsters.h"
#include "sr/server/Temples.h"
#include "sr/server/World.h"

using namespace sr;
using namespace sr::test;
namespace proto = game::proto;

namespace {

std::filesystem::path InstanceExportDir() { return ExportDir(); }

struct InstanceRig : server::InstanceHost {
    server::World* world = nullptr;
    const assets::CharacterTable* characters = nullptr;
    std::map<std::uint32_t, std::uint32_t> partyOf; // entity -> party (none: 1)
    std::set<std::uint32_t> masters{1};             // the parties' masters
    std::map<std::uint8_t, std::uint32_t> bag;      // place -> item
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    std::vector<server::InstanceEntry> entries;
    std::vector<server::BookEntry> book;
    int bookChanged = 0;
    int level = 40; // every player's

    std::optional<Player> InstancePlayer(std::uint32_t entity) override {
        Player p;
        p.peer = entity;
        p.character = 1000 + entity;
        p.name = "P" + std::to_string(entity);
        p.level = level;
        p.party = partyOf.contains(entity) ? partyOf[entity] : 1;
        p.partyMaster = masters.contains(entity);
        p.inTown = true;
        return p;
    }
    void InstanceSend(std::uint64_t, proto::MessageId id, const std::vector<std::byte>& payload) override { sent.emplace_back(id, payload); }
    void InstanceTeleport(std::uint32_t entity, const proto::WorldPosition& to) override { world->Teleport(entity, to); }
    std::uint32_t InstanceItemAt(std::uint32_t, std::uint8_t slot) override { return bag.contains(slot) ? bag[slot] : 0u; }
    bool InstanceTakeOne(std::uint32_t, std::uint8_t slot) override { return bag.erase(slot) == 1; }
    void InstanceSaveEntry(std::int64_t, const server::InstanceEntry& e) override { entries.push_back(e); }
    void InstanceSaveBook(std::int64_t, const server::BookEntry& e) override { book.push_back(e); }
    void InstanceSaveWait(std::int64_t, const server::ItemWait& w) override { waits.push_back(w); }
    std::vector<server::ItemWait> waits;
    void InstanceBookChanged(std::uint32_t) override { ++bookChanged; }
    std::vector<Member> InstancePartyMembers(std::uint32_t entity) override {
        std::vector<Member> out;
        const std::uint32_t party = partyOf.contains(entity) ? partyOf[entity] : 1;
        for (std::uint32_t e = 1; e < 4000; ++e) {
            if (world->PeerOf(e) == 0) continue;
            const std::uint32_t theirs = partyOf.contains(e) ? partyOf[e] : 1;
            if (theirs == party) out.push_back({1000 + e, "P" + std::to_string(e), 40, 1, e});
        }
        return out;
    }
    std::uint32_t InstanceEntityOf(std::uint32_t character) override { return character > 1000 ? character - 1000 : 0; }
    std::uint16_t InstanceServerId() override { return 1; }
    void InstancePublish(std::string text) override { published.push_back(std::move(text)); }
    std::vector<std::string> published;
    int InstanceMonsterLevel(std::uint32_t refObjId) override {
        const assets::ObjectRow* row = characters->Find(refObjId);
        return row != nullptr ? row->level : 0;
    }
    template <typename T>
    std::vector<T> All() const {
        std::vector<T> out;
        for (const auto& [id, payload] : sent) {
            T m;
            if (id == T::kId && proto::Decode(payload, m)) out.push_back(m);
        }
        return out;
    }
};

// The job temples' host: each player's facts, the alliances' counts, what is told, what is written.
struct TempleRig : server::TempleHost {
    std::map<std::uint32_t, Facts> facts;
    std::pair<int, int> counts{0, 0};
    std::vector<proto::TempleNotice> told;
    std::array<int, 4> saved{};
    std::optional<Facts> TempleFacts(std::uint32_t entity) override {
        const auto it = facts.find(entity);
        return it != facts.end() ? std::optional<Facts>(it->second) : std::nullopt;
    }
    std::pair<int, int> TempleAllianceCounts() override { return counts; }
    void TempleTell(proto::MessageId id, const std::vector<std::byte>& payload) override {
        proto::TempleNotice n;
        if (id == proto::TempleNotice::kId && proto::Decode(payload, n)) told.push_back(n);
    }
    void TempleSaveReputation(const std::array<int, 4>& values) override { saved = values; }
};

} // namespace

TEST_CASE("Forgotten World: the catalog, a hole's instance in its own layer, its triggers opening its doors, the book (game files, export)") {
    const char* gameDir = std::getenv("SR_GAME_DIR");
    const std::filesystem::path dir = InstanceExportDir();
    if (!gameDir || !std::filesystem::exists(dir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_GAME_DIR or SR_VSRO_EXPORT missing - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::ImportOptions options;
    options.dir = dir;
    options.databases = {"SRO_VT_SHARD"};
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, error), error);
    REQUIRE(report.Problems() == 0);

    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::CharacterTable characters;
    assets::ItemTable items;
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    REQUIRE_MESSAGE(skills->Load(fs, error), error);

    // The catalog: 22 worlds, 22 holes, 4 themes of 8 talismans, world 1's 7 pillar triggers.
    auto catalog = std::make_shared<server::GameWorldCatalog>();
    std::vector<std::string> notes;
    REQUIRE_MESSAGE(server::LoadGameWorlds(t.Db(), fs, characters, items, *catalog, notes, error), error);
    for (const std::string& n : notes) MESSAGE(n);
    CHECK(catalog->worlds.size() == 22);
    CHECK(catalog->holes.size() == 22);
    CHECK(catalog->themes.size() == 4);
    CHECK(catalog->talismans.size() == 32);
    CHECK(catalog->sharedTriggers.size() == 7);
    CHECK(catalog->reentrySeconds == 300 * 60); // ENTER_WORLD_GROUP_COOL_TIME 300, in minutes (five hours)
    const server::GameWorldSpec* togui = catalog->World(63);
    REQUIRE(togui != nullptr);
    CHECK(togui->codename == "INS_GOD_TOGUI_35_50_LEVEL_1");
    CHECK(togui->start.region == 32484);
    CHECK(togui->minLevel == 35);
    CHECK(togui->maxLevel == 50);
    CHECK(togui->maxUsers == 4);
    CHECK(togui->doors.size() == 15); // three big, twelve small
    CHECK(togui->npcs.size() == 6);   // four recall pillars, two exits
    CHECK(togui->triggers.size() == 17);
    std::size_t waiting = 0;
    for (const server::MonsterNest& n : togui->nests) waiting += n.waiting ? 1 : 0;
    CHECK(waiting == 36);
    const assets::ItemRow* holeRow = items.Find(std::string_view("ITEM_ETC_TELEPORT_HOLE_TOGUI_35_50_LEVEL_1"));
    REQUIRE(holeRow != nullptr);
    REQUIRE(catalog->holes.contains(holeRow->refObjId));
    const server::HoleSpec& hole = catalog->holes.at(holeRow->refObjId);
    CHECK(hole.world == 63);
    CHECK(hole.grade == 1);
    CHECK(hole.minLevel == 35);
    CHECK(hole.maxLevel == 50);
    CHECK(hole.theme == "UIIT_STT_GOD_TOGUI");
    CHECK(hole.gapModel == 19556);
    // Flame Mountain's doors: its rooms' navmeshes name them, all 19 placed in the dungeon.
    const server::GameWorldSpec* flame = catalog->World(69);
    REQUIRE(flame != nullptr);
    CHECK(flame->doors.size() == 19);
    for (const auto& d : flame->doors) CHECK(d.position.region == 0x8012);

    // The world with its navigation, the monsters, the instances.
    core::JobSystem jobs(2);
    server::WorldConfig config;
    server::World world(config, jobs, &fs);
    server::Monsters monsters(world, characters, skills.get(), {}, 7);
    InstanceRig rig;
    rig.world = &world;
    rig.characters = &characters;
    server::Instances instances(rig, world, monsters, characters, catalog, 11);
    std::vector<server::WorldMessage> out;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 3000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, {}, events);
            instances.Tick(now);
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    // The master stands at the world's start place in the shared world (the rig says it is a town) with a hole.
    server::WorldPlayer master;
    master.peer = 1;
    master.character = 1001;
    master.position = togui->start;
    master.dropOnGround = true;
    const std::uint32_t me = world.Enter(master);
    REQUIRE(me == 1);
    instances.Enter(me, 1001, {}, {});
    REQUIRE(pump([&] { return world.Where(me).has_value(); }));
    rig.bag[20] = hole.item;
    CHECK(instances.UseItem(me, 20, *holeRow));
    REQUIRE(instances.Count() == 1);
    CHECK(rig.bag.empty()); // the hole taken
    // The holes' wait (COOLTIME 0x71, 5 hours) began, kept and told.
    REQUIRE(rig.waits.size() == 1);
    CHECK(rig.waits.front().group == 0x71);
    REQUIRE(!rig.All<proto::Cooldown>().empty());
    CHECK(rig.All<proto::Cooldown>().back().id == hole.item);
    CHECK(rig.All<proto::Cooldown>().back().ms == 18'000'000u);
    const std::uint32_t gap = instances.GapOf(me);
    REQUIRE(gap != 0);
    // Its window's facts.
    instances.Handle(me, proto::SelectGap::kId, proto::Encode(proto::SelectGap{gap}));
    REQUIRE(rig.All<proto::GapInfo>().size() == 1);
    const proto::GapInfo info = rig.All<proto::GapInfo>().front();
    CHECK(info.world == 63);
    CHECK(info.master == "P1");
    CHECK(info.grade == 1);
    CHECK(info.secondsLeft > 290);
    // A stranger of another party is refused at the gap.
    server::WorldPlayer stranger = master;
    stranger.peer = 2;
    stranger.character = 1002;
    const std::uint32_t other = world.Enter(stranger);
    rig.partyOf[other] = 2;
    instances.Enter(other, 1002, {}, {});
    REQUIRE(pump([&] { return world.Where(other).has_value(); }));
    instances.Handle(other, proto::EnterGap::kId, proto::Encode(proto::EnterGap{gap}));
    REQUIRE(!rig.All<proto::InstanceRefused>().empty());
    CHECK(rig.All<proto::InstanceRefused>().back().text == "UIIT_MGS_ERR_GOD_TELEPORT_HOLE_NOT_PARTY");
    CHECK(instances.LayerOf(other) == 0);

    // The master enters: taken to the start in the instance's layer, told the instance's state.
    instances.Handle(me, proto::EnterGap::kId, proto::Encode(proto::EnterGap{gap}));
    const std::uint32_t layer = instances.LayerOf(me);
    REQUIRE(layer != 0);
    REQUIRE(pump([&] { return !rig.All<proto::InstanceState>().empty(); }));
    CHECK(rig.All<proto::InstanceState>().back().world == 63);
    // The re-entry limit's timed job (TIMEDJOB_*_FORCLIENT, in minutes).
    CHECK(rig.All<proto::InstanceState>().back().jobSeconds == 300 * 60);
    CHECK(rig.All<proto::InstanceState>().back().jobIcon == "icon\\item\\etc\\teleprot_hole.ddj");
    CHECK(rig.All<proto::InstanceState>().back().jobSubject == "UIIT_STT_PARTY_LOST_WOLRD_SUBJECT");
    CHECK(world.Where(me)->position.layer == layer);
    REQUIRE(!rig.entries.empty());
    CHECK(rig.entries.back().active);
    CHECK(rig.entries.back().world == 63);
    REQUIRE(instances.ReturnPlace(me).has_value());
    CHECK(instances.ReturnPlace(me)->layer == 0);

    // The party recall pillar: the master at a pillar lists its party; a member in a town declines, then
    // (after the button's 5 s) another accepts and is taken in.
    const std::vector<std::uint32_t> pillars = instances.PillarsOf(layer);
    REQUIRE(pillars.size() == 4);
    world.Teleport(me, world.Where(pillars.front())->position);
    REQUIRE(pump([&] { return world.Where(me) && world.Where(me)->position.layer == layer; }));
    server::WorldPlayer member = master;
    member.peer = 5;
    member.character = 1005;
    const std::uint32_t five = world.Enter(member);
    member.peer = 6;
    member.character = 1006;
    const std::uint32_t six = world.Enter(member);
    instances.Enter(five, 1000 + five, {}, {});
    instances.Enter(six, 1000 + six, {}, {});
    REQUIRE(pump([&] { return world.Where(five) && world.Where(six); }));
    instances.Handle(me, proto::OpenSummon::kId, proto::Encode(proto::OpenSummon{pillars.front()}));
    REQUIRE(!rig.All<proto::SummonList>().empty());
    const proto::SummonList list = rig.All<proto::SummonList>().back();
    CHECK(list.maxUsers == 4);
    const auto row = [&](std::uint32_t character) {
        const auto it = std::find_if(list.rows.begin(), list.rows.end(), [&](const auto& r) { return r.character == character; });
        return it != list.rows.end() ? std::optional<proto::SummonList::Row>(*it) : std::nullopt;
    };
    REQUIRE(row(1000 + five));
    CHECK(row(1000 + five)->state == 2);
    CHECK(row(1000 + five)->minutes >= 119);
    CHECK(!row(1001));         // not itself
    CHECK(!row(1000 + other)); // not another party's
    instances.Handle(me, proto::SummonMember::kId, proto::Encode(proto::SummonMember{1000 + six}));
    REQUIRE(!rig.All<proto::SummonAsked>().empty());
    CHECK(rig.All<proto::SummonAsked>().back().master == "P1");
    instances.Handle(six, proto::SummonReply::kId, proto::Encode(proto::SummonReply{false}));
    REQUIRE(!rig.All<proto::SummonResult>().empty());
    CHECK(rig.All<proto::SummonResult>().back().text == "UIIT_STT_GNGWC_PARTY_MEMBER_CALL_REFUSAL");
    CHECK(rig.All<proto::SummonResult>().back().name == "P" + std::to_string(six));
    CHECK(instances.LayerOf(six) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5100)); // the button's wait
    instances.Handle(me, proto::SummonMember::kId, proto::Encode(proto::SummonMember{1000 + five}));
    instances.Handle(five, proto::SummonReply::kId, proto::Encode(proto::SummonReply{true}));
    CHECK(instances.LayerOf(five) == layer);
    REQUIRE(pump([&] { return world.Where(five) && world.Where(five)->position.layer == layer; }));
    CHECK(instances.ReturnPlace(five)->layer == 0);
    // Listed again: inside already.
    instances.Handle(me, proto::OpenSummon::kId, proto::Encode(proto::OpenSummon{pillars.front()}));
    const auto again = rig.All<proto::SummonList>().back();
    const auto fiveRow = std::find_if(again.rows.begin(), again.rows.end(), [&](const auto& r) { return r.character == 1000 + five; });
    REQUIRE(fiveRow != again.rows.end());
    CHECK(fiveRow->state == 1);
    instances.Leave(six);
    world.Leave(six);
    // The instance's first monsters stand in its layer; the shared world's same place has none (npcpos.txt has no
    // monster there).
    REQUIRE(pump([&] { return !monsters.Near(world.Where(me)->position, 3000.0f).empty(); }));
    CHECK(monsters.Near(world.Where(other)->position, 3000.0f).empty());

    // A shut door stands in the way of walks in its layer only: BIG_GATE_TOGUI_1, walked through either way.
    const server::GameWorldSpec::Door* door = nullptr;
    for (const auto& d : togui->doors) {
        if (d.zone == "POS_STRUCTURE_GOD_BIG_GATE_TOGUI_1") door = &d;
    }
    REQUIRE(door != nullptr);
    {
        proto::WorldPosition near = door->position;
        near.layer = layer;
        world.Teleport(me, near);
        proto::WorldPosition shared = door->position;
        world.Teleport(other, shared);
        REQUIRE(pump([&] { return world.Where(me) && world.Where(other); }));
        bool blockedOnlyInLayer = false;
        for (const float angle : {door->yaw, door->yaw + 1.5707964f}) {
            const proto::FlatPosition c = proto::ToFlat(door->position);
            const float dx = std::sin(angle) * 250.0f, dz = std::cos(angle) * 250.0f; // from outside its footprint
            const proto::WorldPosition from = proto::FromFlat({c.x - dx, c.z - dz}, door->position.y);
            const proto::WorldPosition to = proto::FromFlat({c.x + dx, c.z + dz}, door->position.y);
            // Both put on the near side, then a blink to the far side as a walk would go.
            proto::WorldPosition fromLayer = from;
            fromLayer.layer = layer;
            world.Teleport(me, fromLayer);
            world.Teleport(other, from);
            REQUIRE(pump([&] { return world.Where(me) && world.Where(other); }));
            const auto inLayer = world.Blink(me, to, 700.0f);
            const auto inShared = world.Blink(other, to, 700.0f);
            if (!inLayer || !inShared) continue;
            const proto::FlatPosition a = proto::ToFlat(*inLayer), b = proto::ToFlat(*inShared), goal = proto::ToFlat(to);
            const double leftInLayer = std::hypot(a.x - goal.x, a.z - (goal.z + double(layer) * proto::kLayerFlatSpan));
            const double leftShared = std::hypot(b.x - goal.x, b.z - goal.z);
            MESSAGE("door walk at " << angle << ": " << leftInLayer << " short in the layer, " << leftShared << " in the shared world");
            if (leftShared < 5.0 && leftInLayer > 30.0) blockedOnlyInLayer = true;
        }
        CHECK(blockedOnlyInLayer);
    }

    // Camp A: the Envy (hive 679, waiting) raised, then each hive cleared raises the next (17: 680; 18: 683 and the four
    // small doors; 19: 684; 20: the big door 1 with its notice).
    const proto::FlatPosition startFlat = proto::ToFlat(togui->start);
    proto::WorldPosition camp = proto::FromFlat({startFlat.x - 450.0, startFlat.z - 2900.0}, togui->start.y);
    camp.layer = layer;
    world.Teleport(me, camp);
    REQUIRE(pump([&] { return world.Where(me) && world.Where(me)->position.layer == layer; }));
    const std::size_t envy = instances.NestOf(layer, 30121);
    REQUIRE(envy != SIZE_MAX);
    CHECK(monsters.NestSpec(envy)->waiting);
    monsters.ActivateNest(envy); // as trigger 14 does once hive 675 is cleared
    const auto clear = [&](std::uint32_t nestId) {
        const std::size_t n = instances.NestOf(layer, nestId);
        REQUIRE(n != SIZE_MAX);
        REQUIRE(pump([&] { return !monsters.NestSpec(n)->waiting; }, 200));
        for (int round = 0; round < 1000 && !monsters.NestCleared(n); ++round) {
            pump([] { return true; }, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // the monsters wake their nests once a second
            const auto spec = monsters.NestSpec(n);
            for (const std::uint32_t m : monsters.Near(spec->position, spec->spawnRadius + 400.0f)) {
                const auto where = world.Where(m);
                const server::Monsters::Damaged d = monsters.Damage(m, me, 100'000'000, server::Monsters::Clock::now());
                if (d.killed && where) instances.Killed(me, d, where->position);
            }
        }
        if (!monsters.NestCleared(n)) {
            const auto [alive, left] = monsters.NestCounts(n);
            MESSAGE("nest " << nestId << " not cleared: " << alive << " standing, " << left << " to come; at " << monsters.NestSpec(n)->position.region << " "
                            << monsters.NestSpec(n)->position.x << "," << monsters.NestSpec(n)->position.z);
        }
        CHECK(monsters.NestCleared(n));
    };
    clear(30121);
    for (const std::uint32_t id : {30115u, 30116u, 30117u, 30118u, 30119u, 30120u}) CHECK(!monsters.NestSpec(instances.NestOf(layer, id))->waiting);
    for (const std::uint32_t id : {30115u, 30116u, 30117u, 30118u, 30119u, 30120u}) clear(id);
    CHECK(instances.DoorOpen(layer, "POS_STRUCTURE_GOD_SMALL_GATE_TOGUI_A_1"));
    CHECK(instances.DoorOpen(layer, "POS_STRUCTURE_GOD_SMALL_GATE_TOGUI_A_4"));
    CHECK(!instances.DoorOpen(layer, "POS_STRUCTURE_GOD_BIG_GATE_TOGUI_1"));
    for (const std::uint32_t id : {30122u, 30123u, 30124u, 30125u}) clear(id);
    clear(30126);
    CHECK(instances.DoorOpen(layer, "POS_STRUCTURE_GOD_BIG_GATE_TOGUI_1"));
    bool openNotice = false;
    for (const proto::InstanceNotice& n : rig.All<proto::InstanceNotice>()) openNotice |= n.text == "UIIT_MGS_GOD_TRIGGER_NOTICE_COMMON_OPEN_DOOR" && n.notify;
    CHECK(openNotice);

    // Out by an exit: back where it entered from, its entry no longer active.
    const std::vector<std::uint32_t> exits = instances.ExitsOf(layer);
    REQUIRE(exits.size() == 2);
    {
        proto::WorldPosition at = world.Where(exits.front())->position;
        world.Teleport(me, at);
        REQUIRE(pump([&] { return world.Where(me) && world.Where(me)->position.layer == layer; }));
    }
    instances.Handle(me, proto::LeaveInstance::kId, proto::Encode(proto::LeaveInstance{exits.front()}));
    CHECK(instances.LayerOf(me) == 0);
    REQUIRE(pump([&] { return world.Where(me) && world.Where(me)->position.layer == 0; }));
    CHECK(!rig.entries.back().active);
    CHECK(rig.All<proto::InstanceState>().back().world == 0);
    // The re-entry limit: a third player's party opens another instance; the first, joined to that party, is refused
    // at its gap within 300 s of its last entry.
    server::WorldPlayer third = master;
    third.peer = 3;
    third.character = 1003;
    third.position = world.Where(me)->position;
    const std::uint32_t thirdEntity = world.Enter(third);
    rig.partyOf[thirdEntity] = 3;
    rig.masters.insert(thirdEntity);
    instances.Enter(thirdEntity, 1003, {}, {});
    REQUIRE(pump([&] { return world.Where(thirdEntity).has_value(); }));
    rig.bag[21] = hole.item;
    CHECK(instances.UseItem(thirdEntity, 21, *holeRow));
    const std::uint32_t gap2 = instances.GapOf(thirdEntity);
    REQUIRE(gap2 != 0);
    rig.partyOf[me] = 3;
    rig.masters.erase(me);
    instances.Handle(me, proto::EnterGap::kId, proto::Encode(proto::EnterGap{gap2}));
    CHECK(rig.All<proto::InstanceRefused>().back().text == "UIIT_MGS_ERR_GOD_LIMIT_TIME");
    CHECK(instances.LayerOf(me) == 0);
    // A master who used a hole an hour ago: told the wait left at its entry, and its next hole refused (kept in the bag).
    server::WorldPlayer fourth = master;
    fourth.peer = 4;
    fourth.character = 1004;
    fourth.position = world.Where(me)->position;
    const std::uint32_t fourthEntity = world.Enter(fourth);
    rig.partyOf[fourthEntity] = 4;
    rig.masters.insert(fourthEntity);
    const std::int64_t anHourAgo = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 3600;
    rig.sent.clear();
    instances.Enter(fourthEntity, 1004, {}, {}, {{0x71, anHourAgo}});
    REQUIRE(!rig.All<proto::Cooldown>().empty());
    CHECK(rig.All<proto::Cooldown>().back().ms == doctest::Approx(4.0 * 3600 * 1000).epsilon(0.001));
    REQUIRE(pump([&] { return world.Where(fourthEntity).has_value(); }));
    const std::size_t open = instances.Count();
    rig.bag[22] = hole.item;
    CHECK(instances.UseItem(fourthEntity, 22, *holeRow));
    CHECK(instances.Count() == open);
    CHECK(rig.bag.contains(22));

    // The book: a talisman registered once, refused the second time; the theme complete with its eight.
    const server::CollectionTheme* theme = catalog->Theme(std::string_view("THEME_GOD_TOGUI_RED_BLOOD"));
    REQUIRE(theme != nullptr);
    std::vector<const assets::ItemRow*> talismans;
    for (const auto& [item, spec] : catalog->talismans) {
        if (spec.theme == theme->id) talismans.push_back(items.Find(item));
    }
    REQUIRE(talismans.size() == 8);
    CHECK(!instances.ThemeComplete(me, "THEME_GOD_TOGUI_RED_BLOOD"));
    rig.bag[30] = talismans[0]->refObjId;
    CHECK(instances.UseItem(me, 30, *talismans[0]));
    CHECK(rig.All<proto::TalismanRegistered>().back().ok);
    CHECK(rig.All<proto::TalismanRegistered>().back().text == "UIIT_MSG_COLLECTION_WINDOW_RECORD_FINISH");
    rig.bag[31] = talismans[0]->refObjId;
    CHECK(instances.UseItem(me, 31, *talismans[0]));
    CHECK(!rig.All<proto::TalismanRegistered>().back().ok);
    CHECK(rig.All<proto::TalismanRegistered>().back().text == "UIIT_MSG_COLLECTION_WINDOW_RECORD_ERR");
    CHECK(rig.bag.contains(31)); // not taken
    for (std::size_t i = 1; i < talismans.size(); ++i) {
        rig.bag[std::uint8_t(40 + i)] = talismans[i]->refObjId;
        instances.Handle(me, proto::RegisterTalisman::kId, proto::Encode(proto::RegisterTalisman{std::uint8_t(40 + i)}));
    }
    CHECK(rig.book.size() == 8);
    CHECK(rig.bookChanged == 8);
    CHECK(instances.ThemeComplete(me, "THEME_GOD_TOGUI_RED_BLOOD"));

    // A shut Flame Mountain door (big gate 1, its doorway's cells in the room's floor) stops walks in its layer only, from
    // the room's side and from the corridor's.
    const server::GameWorldSpec::Door* gate = nullptr;
    for (const auto& d : flame->doors) {
        if (d.zone == "POS_STRUCTURE_GOD_BIG_GATE_FLAME_1") gate = &d;
    }
    REQUIRE(gate != nullptr);
    constexpr std::uint32_t kFlameLayer = 77;
    world.SetNavZone(gate->navKey, gate->navName, true, kFlameLayer);
    server::WorldPlayer walker = master;
    walker.peer = 8;
    walker.character = 1008;
    walker.position = gate->position;
    walker.position.layer = kFlameLayer;
    const std::uint32_t inLayer = world.Enter(walker);
    walker.peer = 9;
    walker.character = 1009;
    walker.position.layer = 0;
    const std::uint32_t inShared = world.Enter(walker);
    REQUIRE(pump([&] { return world.Where(inLayer) && world.Where(inShared); }));
    int blockedWays = 0;
    for (const float angle : {gate->yaw, gate->yaw + 1.5707964f}) {
        for (const float side : {1.0f, -1.0f}) {
            const proto::FlatPosition c = proto::ToFlat(gate->position);
            const float dx = std::sin(angle) * 220.0f * side, dz = std::cos(angle) * 220.0f * side;
            const proto::WorldPosition from = proto::FromFlat({c.x - dx, c.z - dz}, gate->position.y);
            const proto::WorldPosition to = proto::FromFlat({c.x + dx, c.z + dz}, gate->position.y);
            proto::WorldPosition fromLayer = from;
            fromLayer.layer = kFlameLayer;
            world.Teleport(inLayer, fromLayer);
            world.Teleport(inShared, from);
            REQUIRE(pump([&] { return world.Where(inLayer) && world.Where(inShared); }));
            const auto a = world.Blink(inLayer, to, 600.0f);
            const auto b = world.Blink(inShared, to, 600.0f);
            if (!a || !b) continue;
            const proto::FlatPosition pa = proto::ToFlat(*a), pb = proto::ToFlat(*b), goal = proto::ToFlat(to);
            const double leftInLayer = std::hypot(pa.x - goal.x, pa.z - (goal.z + double(kFlameLayer) * proto::kLayerFlatSpan));
            const double leftShared = std::hypot(pb.x - goal.x, pb.z - goal.z);
            MESSAGE("flame gate walk " << angle << " " << side << ": " << leftInLayer << " short in the layer, " << leftShared << " shared");
            if (leftShared < 5.0 && leftInLayer > 30.0) ++blockedWays;
        }
    }
    CHECK(blockedWays == 2); // through the doorway either way
    CHECK(!instances.ThemeComplete(me, "THEME_GOD_FLAME_BURNING_ABYSS"));

    // The Pharaoh's tomb: its three worlds by their gates, their nests and rules; the party's master
    // opens one and his party is told the difficulty, a member joins it, the other difficulty and a party of one refused,
    // a free re-entry, two entries a day.
    REQUIRE(catalog->tombs.size() == 3);
    const server::GameWorldSpec* novice = catalog->TombOfPoint(165);
    const server::GameWorldSpec* expert = catalog->TombOfPoint(166);
    REQUIRE(novice != nullptr);
    REQUIRE(expert != nullptr);
    CHECK(catalog->TombOfPoint(167) != nullptr);
    CHECK(novice->codename == "INS_PHARAOH_NOVICE");
    CHECK(novice->party);
    CHECK(novice->entriesPerDay == 2);
    CHECK(novice->minLevel == 100);
    CHECK(novice->maxUsers == 8);
    CHECK(novice->nests.size() == 30);
    CHECK(novice->nameKey == "SN_ZONE_21836_10");
    CHECK(novice->warnMs == std::vector<std::uint32_t>{1'800'000, 600'000, 300'000});
    CHECK(novice->start.region == 29631);
    rig.level = 105;
    server::WorldPlayer tomber = master;
    tomber.position = {19019, 202.0f, 398.0f, 1782.0f};
    std::uint32_t tp[3]{};
    for (int k = 0; k < 3; ++k) {
        tomber.peer = 20 + std::uint64_t(k);
        tomber.character = 1020 + k;
        tp[k] = world.Enter(tomber);
        instances.Enter(tp[k], 1020 + k, {}, {});
    }
    REQUIRE(pump([&] { return world.Where(tp[0]) && world.Where(tp[1]) && world.Where(tp[2]); }));
    rig.partyOf[tp[0]] = rig.partyOf[tp[1]] = 7;
    rig.partyOf[tp[2]] = 9; // alone
    rig.masters.insert(tp[0]);
    rig.sent.clear();
    CHECK(instances.EnterTomb(tp[1], *novice) == proto::GateStatus::TombMaster); // his master not in yet
    CHECK(instances.EnterTomb(tp[2], *novice) == proto::GateStatus::TombParty);
    REQUIRE(instances.EnterTomb(tp[0], *novice) == proto::GateStatus::Ok);
    const std::uint32_t tombLayer = instances.LayerOf(tp[0]);
    CHECK(tombLayer != 0);
    CHECK(instances.WorldOf(tp[0]) == 57);
    bool told = false;
    for (const proto::InstanceNotice& n : rig.All<proto::InstanceNotice>()) {
        told |= n.text == "UIIT_MSG_EGYPT_TOMB_ENTRANCE_CONDITION_ERROR_04" && n.arg == "SN_ZONE_21836_10";
    }
    CHECK(told);
    CHECK(instances.EnterTomb(tp[1], *expert) == proto::GateStatus::TombMaster); // another difficulty
    REQUIRE(instances.EnterTomb(tp[1], *novice) == proto::GateStatus::Ok);
    CHECK(instances.LayerOf(tp[1]) == tombLayer);
    // Out and back in while it stands: free.
    instances.ExitSiege(tp[0]);
    CHECK(instances.LayerOf(tp[0]) == 0);
    REQUIRE(pump([&] { return world.Where(tp[0]).has_value(); }));
    REQUIRE(instances.EnterTomb(tp[0], *novice) == proto::GateStatus::Ok);
    CHECK(instances.LayerOf(tp[0]) == tombLayer);
    // Two entries a day: the second opening counts, a third is refused.
    instances.CloseWorld(tombLayer);
    REQUIRE(pump([&] { return world.Where(tp[0]).has_value(); }));
    REQUIRE(instances.EnterTomb(tp[0], *novice) == proto::GateStatus::Ok);
    instances.CloseWorld(instances.LayerOf(tp[0]));
    REQUIRE(pump([&] { return world.Where(tp[0]).has_value(); }));
    CHECK(instances.EnterTomb(tp[0], *novice) == proto::GateStatus::TombEntries);
    CHECK(instances.EnterTomb(tp[0], *expert) == proto::GateStatus::Ok); // each difficulty its own count

    // The job temples: their six worlds, the area's rules; Anubis's and Isis's opened, the
    // reputation's alliance alone at Anubis's door, the others refused; Haroeris's shut to all on equal reputations; Haroeris
    // killed: Seth's temple and its door; closed, everyone back.
    REQUIRE(catalog->temples.size() == 6);
    CHECK(catalog->templeArea.at("ENTER_LIMIT_CONDITION_LEVEL_MINIMUM_NUM") == "105");
    const server::GameWorldSpec* anubis = nullptr;
    for (const server::GameWorldSpec& t : catalog->temples) {
        if (t.codename == "INS_TEMPLE_ANUB") anubis = &t;
    }
    REQUIRE(anubis != nullptr);
    CHECK(anubis->nests.size() == 5);
    CHECK(anubis->start.region == 0x800C);
    TempleRig temple;
    server::TempleConfig templeConfig;
    templeConfig.enabled = false; // opened by hand
    server::Temples temples(temple, instances, monsters, world, catalog, templeConfig, {0, 7, 0, 0}); // the thieves ahead at Anubis's
    REQUIRE(temples.Open(10));
    REQUIRE(temples.LayerOf(53) != 0);
    REQUIRE(temples.LayerOf(54) != 0);
    CHECK(temples.AllowedOf(53) == proto::TempleNotice::kThief);
    CHECK(temples.AllowedOf(54) == proto::TempleNotice::kEveryone);
    CHECK(temples.Reputation() == std::array<int, 4>{0, 0, 0, 0}); // Anubis's back to 0, written
    CHECK(temple.saved == std::array<int, 4>{0, 0, 0, 0});
    const std::uint32_t anubisDoor = temples.DoorOf(53);
    REQUIRE(anubisDoor != 0);
    REQUIRE(pump([&] { return world.Where(anubisDoor).has_value(); }));
    server::WorldPlayer pilgrim = master;
    pilgrim.position = world.Where(anubisDoor)->position;
    std::uint32_t pg[3]{};
    for (int k = 0; k < 3; ++k) {
        pilgrim.peer = 40 + std::uint64_t(k);
        pilgrim.character = 1040 + k;
        pg[k] = world.Enter(pilgrim);
        instances.Enter(pg[k], 1040 + k, {}, {});
    }
    REQUIRE(pump([&] { return world.Where(pg[0]) && world.Where(pg[1]) && world.Where(pg[2]); }));
    temple.facts[pg[0]] = {110, 2, false, false, false, 2}; // a thief in his suit
    temple.facts[pg[1]] = {110, 1, false, false, false, 1}; // a trader in his
    temple.facts[pg[2]] = {110, 4, false, false, false, 2}; // the thief league, no suit on
    // The door tests the league, then the world's admission the suit.
    CHECK(temples.UseDoor(pg[2], anubisDoor) == proto::GateStatus::ShrineJobSuit);
    CHECK(temples.UseDoor(pg[1], anubisDoor) == proto::GateStatus::ShrineNoRight);
    temple.counts = {10, 40};          // the thieves 80 % of 50
    CHECK(temples.UseDoor(pg[0], anubisDoor) == proto::GateStatus::ShrineRatio);
    temple.counts = {20, 20};
    REQUIRE(temples.UseDoor(pg[0], anubisDoor) == proto::GateStatus::Ok);
    CHECK(instances.LayerOf(pg[0]) == temples.LayerOf(53));
    // The area's gates by the suit of their alliance.
    CHECK(temples.AreaRefusal(pg[2], server::Temples::kAreaGateThief) == proto::GateStatus::ShrineNoRight);
    CHECK(temples.AreaRefusal(pg[1], server::Temples::kAreaGateTraderHunter) == proto::GateStatus::Ok);
    CHECK(temples.AreaRefusal(pg[1], server::Temples::kAreaGateThief) == proto::GateStatus::ShrineNoRight);
    CHECK(temples.AreaRefusal(pg[0], server::Temples::kAreaGateTraderHunter) == proto::GateStatus::ShrineNoRight);
    CHECK(temples.AreaRefusal(pg[0], server::Temples::kAreaGateThief) == proto::GateStatus::Ok);
    // Haroeris's: equal reputations (0, 0): nobody, so no instance and no door; the traders ahead next time.
    REQUIRE(temples.Open(11));
    CHECK(temples.AllowedOf(55) == proto::TempleNotice::kNobody);
    CHECK(temples.LayerOf(55) == 0);
    CHECK(temples.DoorOf(55) == 0);
    temples.Close(11);
    temples.AddReputation(1, 1, 3);
    REQUIRE(temples.Open(11));
    CHECK(temples.AllowedOf(55) == proto::TempleNotice::kTraderHunter);
    REQUIRE(temples.LayerOf(55) != 0);
    CHECK(temples.LayerOf(56) == 0); // Seth's shut until Haroeris dies
    server::Monsters::Damaged haroeris;
    haroeris.killed = true;
    haroeris.refObjId = 26736;
    proto::WorldPosition fell{0x800E, 9919.0f, 100.0f, 24.0f};
    fell.layer = temples.LayerOf(55);
    temples.Killed(haroeris, fell);
    CHECK(temples.LayerOf(56) != 0);
    CHECK(temples.DoorOf(56) != 0);
    // The doors' selection: Seth's says its temple's players and its WorldMaxUserCount, the others nothing.
    const auto sethDoor = temples.DoorInfo(temples.DoorOf(56));
    REQUIRE(sethDoor.has_value());
    CHECK(sethDoor->temple);
    CHECK(sethDoor->world == 56);
    CHECK(sethDoor->capacity == 32);
    CHECK(sethDoor->inside == 0);
    const auto anubisDoorInfo = temples.DoorInfo(anubisDoor);
    REQUIRE(anubisDoorInfo.has_value());
    CHECK(anubisDoorInfo->capacity == 0);
    CHECK_FALSE(temples.DoorInfo(pg[0]).has_value());
    // The board's numbers again (0xB4E0): Anubis's, then Haroeris's.
    temples.AddReputation(1, 1, 4);
    temples.AddReputation(1, 2, 9);
    const proto::TemplePoints points = server::Temples::PointsOf(temples.Board());
    CHECK(points.anubisTraderHunter == 0);
    CHECK(points.anubisThief == 0);
    CHECK(points.haroerisTraderHunter == 4);
    CHECK(points.haroerisThief == 9);
    bool sethTold = false;
    for (const proto::TempleNotice& n : temple.told) sethTold |= n.kind == proto::TempleNotice::kOpened && n.region == 0x800F;
    CHECK(sethTold);
    // Closed: the pair's instances gone, the player inside back in the area.
    temples.Close(10);
    CHECK(temples.LayerOf(53) == 0);
    CHECK(temples.DoorOf(53) == 0);
    CHECK(instances.LayerOf(pg[0]) == 0);
    temples.Close(11);
    CHECK(temples.LayerOf(56) == 0);
}

TEST_CASE("Forgotten World: the layer travels in a position's world-wide coordinates and back") {
    for (const std::uint32_t layer : {0u, 1u, 7u, 499'999u}) {
        for (const proto::WorldPosition p : {proto::WorldPosition{32484, 1063.25f, 693.0f, 1125.5f, layer}, proto::WorldPosition{0x8012, 10897.5f, 165.0f, -6110.25f, layer},
                                             proto::WorldPosition{25000, 0.5f, 0.0f, 1919.5f, layer}}) {
            const proto::WorldPosition back = proto::FromFlat(proto::ToFlat(p), p.y);
            CHECK(back.layer == layer);
            CHECK(back.region == p.region);
            CHECK(back.x == doctest::Approx(p.x).epsilon(0.0001));
            CHECK(back.z == doctest::Approx(p.z).epsilon(0.0001));
        }
    }
    // Two layers' same place are far apart.
    const proto::FlatPosition a = proto::ToFlat({32484, 100, 0, 100, 1}), b = proto::ToFlat({32484, 100, 0, 100, 2});
    CHECK(std::hypot(a.x - b.x, a.z - b.z) >= proto::kLayerFlatSpan);
}

TEST_CASE("Forgotten World: an entry saved active leaves the character's others inactive, in one transaction (the original's _CharInstanceWorldDataUpdate)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::InstanceEntry first;
    first.key = 1;
    first.world = 10;
    first.layer = 1;
    first.active = true;
    first.count = 1;
    server::InstanceEntry second = first;
    second.key = 2;
    second.world = 11;
    const auto active = [&](std::int64_t character) {
        return t.Db().Query("SELECT count(*), coalesce(max(worldid), 0) FROM _charinstanceworlddata WHERE charid = $1 AND isactivated <> 0", {character});
    };
    REQUIRE_MESSAGE(server::SaveInstanceEntry(t.Db(), 1, first, error), error);
    REQUIRE_MESSAGE(server::SaveInstanceEntry(t.Db(), 1, second, error), error);
    CHECK(active(1).Int(0, 0) == 1);
    CHECK(active(1).Int(0, 1) == 11);
    // Another character's entries are its own.
    REQUIRE_MESSAGE(server::SaveInstanceEntry(t.Db(), 2, first, error), error);
    CHECK(active(1).Int(0, 0) == 1);
    CHECK(active(2).Int(0, 1) == 10);
    // Left (inactive): the others stay as they are.
    second.active = false;
    REQUIRE_MESSAGE(server::SaveInstanceEntry(t.Db(), 1, second, error), error);
    CHECK(active(1).Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _charinstanceworlddata WHERE charid = 1").Int(0, 0) == 2);
}