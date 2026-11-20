#include <doctest/doctest.h>

// The recovery items and the auto potion: the shared rules (game/Potions.h)
// and the game server using a character's potions by its settings, in-process over the wire.

#include <chrono>
#include <cmath>
#include <memory>
#include <string>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Potions.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
using game::AutoPotion;

namespace {

assets::ItemRow Potion(std::uint32_t id, const char* code, int tid3, int tid4, std::array<int, 6> params) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = code;
    r.typeId = {3, 3, tid3, tid4};
    r.country = 3;
    r.maxStack = 50;
    for (std::size_t i = 0; i < params.size(); ++i) r.params[i] = params[i];
    for (std::size_t i = params.size(); i < r.params.size(); ++i) r.params[i] = -1;
    return r;
}

constexpr std::uint32_t kHpPotion = 50, kMpPotion = 51, kHpPercent = 52, kVigor = 53, kPill = 55, kPurify = 56;

std::shared_ptr<assets::ItemTable> PotionTable() {
    auto table = std::make_shared<assets::ItemTable>();
    // The creation's items (a Chinese chest, legs, shoes and a sword), then the 1.188 rows' numbers.
    const auto worn = [&](std::uint32_t id, const char* code, int tid3, int tid4) {
        assets::ItemRow r;
        r.refObjId = id;
        r.codename = code;
        r.typeId = {3, 1, tid3, tid4};
        r.sex = 2;
        r.requirementTypes = {1, -1, -1, -1};
        r.requirementLevels = {1, 0, 0, 0};
        r.stats.assign(55, 0.0f);
        r.stats[0] = 40;
        table->Add(r);
    };
    worn(1, "CHEST", 1, 3);
    worn(2, "LEGS", 1, 4);
    worn(3, "SHOES", 1, 6);
    worn(10, "SWORD", 6, 2);
    table->Add(Potion(kHpPotion, "ITEM_ETC_HP_POTION_01", 1, 1, {120, 0, 0, 0, -1, -1}));
    table->Add(Potion(kMpPotion, "ITEM_ETC_MP_POTION_01", 1, 2, {0, 0, 120, 0, -1, -1}));
    table->Add(Potion(kHpPercent, "ITEM_ETC_HP_SPOTION_01", 1, 1, {0, 25, 0, 0, -1, -1}));
    table->Add(Potion(kVigor, "ITEM_ETC_ALL_POTION_01", 1, 3, {120, 0, 120, 0, -1, -1}));
    table->Add(Potion(kPill, "ITEM_ETC_CURE_ALL_01", 2, 6, {36, 36, 36, 36, 36, 36}));
    table->Add(Potion(kPurify, "ITEM_ETC_CURE_RANDOM_01", 2, 1, {25145280, 3, 100, -1, -1, -1}));
    return table;
}

// Everything of T that arrived (and nothing more for a moment).
template <typename T>
void Drain(Client& c) {
    T m;
    while (c.Expect(m, 60ms)) {
    }
}

} // namespace

TEST_CASE("Auto potion: the original's words, thresholds, locks, amounts and what fits a row") {
    // The 1.188 client's defaults written as words: HP off, 50 %, quick slot 1
    // (page 1, key 1); MP slot 2; the delay on at 1 s.
    CHECK(game::EncodeAutoPotionWord({false, 50, 0}, 1) == 0x3211);
    CHECK(game::EncodeAutoPotionWord({false, 50, 0}, 2) == 0x3212);
    CHECK(game::EncodeAutoPotionWord({false, 0, 0}, 3) == 0x0013);
    CHECK(game::EncodeAutoPotionWord({true, 70, 0}, 10) == (0x8000 | 70 << 8 | 0x1A)); // slot 10: page 1, key 10
    CHECK(game::EncodeAutoPotionWord({true, 70, 0}, 23) == (0x8000 | 70 << 8 | 0x33));
    CHECK(game::EncodeAutoPotionDelay(true, 1000) == 0x8A);
    int slot = 0;
    const AutoPotion::Entry e = game::DecodeAutoPotionWord(0xC623, &slot);
    CHECK(e.enabled);
    CHECK(e.percent == 70);
    CHECK(slot == 13);
    CHECK(game::DecodeAutoPotionWord(game::EncodeAutoPotionWord({true, 100, 0})).percent == 100);
    bool on = false;
    int ms = 0;
    game::DecodeAutoPotionDelay(0x8A, on, ms);
    CHECK(on);
    CHECK(ms == 1000);
    // Settings within the window's ranges; the repeat is the delay, or 500 ms without it.
    AutoPotion s;
    CHECK(s.RepeatMs() == 1000);
    s.rows[AutoPotion::kHp].percent = 0;
    s.delayMs = 9900;
    s.Clamp();
    CHECK(s.rows[AutoPotion::kHp].percent == 1);
    CHECK(s.delayMs == 9500);
    s.delayEnabled = false;
    CHECK(s.RepeatMs() == 500);
    CHECK(game::FromMessage(game::ToMessage(s)) == s);

    // A row holds at or under most x percent / 100, cut.
    CHECK(game::AutoPotionThreshold(333, 50) == 166);
    CHECK(game::AutoPotionThreshold(333, 100) == 333);

    const auto table = PotionTable();
    const assets::ItemRow& hp = *table->Find(kHpPotion);
    const assets::ItemRow& mp = *table->Find(kMpPotion);
    const assets::ItemRow& pct = *table->Find(kHpPercent);
    const assets::ItemRow& all = *table->Find(kVigor);
    const assets::ItemRow& pill = *table->Find(kPill);
    const assets::ItemRow& purify = *table->Find(kPurify);
    // The locks: a flat potion 1.1 s Chinese, 15.1 s European; a percent
    // one 4.1 s; HP, MP and both apart; the pills.
    CHECK(game::ReuseLockOf(hp, false).bit == 0x01);
    CHECK(game::ReuseLockOf(hp, false).ms == 1100);
    CHECK(game::ReuseLockOf(hp, true).ms == 15100);
    CHECK(game::ReuseLockOf(mp, false).bit == 0x02);
    CHECK(game::ReuseLockOf(all, false).bit == 0x04);
    CHECK(game::ReuseLockOf(pct, true).ms == 4100);
    CHECK(game::ReuseLockOf(pill, false).bit == 0x40);
    CHECK(game::ReuseLockOf(pill, false).ms == 1100);
    CHECK(game::ReuseLockOf(purify, false).bit == 0x100);
    CHECK(game::ReuseLockOf(purify, false).ms == 20100);
    // The amounts: flat by level and STR / INT; percent of the most.
    const game::Recovery r1 = game::RecoveryOf(hp, 1, 20, 20, 300, 300);
    CHECK(r1.flat);
    CHECK(r1.hp == 125); // 120 x (1 + 20 / 416)
    CHECK(r1.mp == 0);
    CHECK(game::RecoveryOf(hp, 11, 52, 20, 300, 300).hp == std::uint32_t(120.0 * std::pow(1.02, 10) * (1.0 + 52.0 / 416.0)));
    const game::Recovery r2 = game::RecoveryOf(pct, 10, 20, 20, 1000, 500);
    CHECK_FALSE(r2.flat);
    CHECK(r2.hp == 250);
    CHECK(game::PotionStep(125) == 25);
    CHECK(game::PotionStep(3) == 1);
    // What a row takes.
    CHECK(game::AutoPotionFits(AutoPotion::kHp, hp));
    CHECK_FALSE(game::AutoPotionFits(AutoPotion::kHp, mp));
    CHECK(game::AutoPotionFits(AutoPotion::kHp, all));
    CHECK(game::AutoPotionFits(AutoPotion::kMp, all));
    CHECK(game::AutoPotionFits(AutoPotion::kAbnormal, pill));
    CHECK(game::AutoPotionFits(AutoPotion::kAbnormal, purify));
    CHECK_FALSE(game::AutoPotionFits(AutoPotion::kAbnormal, hp));
    // The pills' cures.
    const game::CureSpec c = game::PillCureOf(pill);
    CHECK(c.elementMask == 0x3F);
    CHECK(c.elementLevel == 36);
    const game::CureSpec p = game::PillCureOf(purify);
    CHECK(p.curseMask == 25145280u);
    CHECK(p.curseLevel == 3);
    CHECK(p.curseChance == 100);
    CHECK(p.most == 1);
}

TEST_CASE("Auto potion on the game server: below the threshold, the lock, the next stack, death, kept, seen by others") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    rules->items = PotionTable();
    config.rules = rules;
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.logoutCountdown = 0s;
    config.databaseThreads = 1; // saves before the next entry's load, in order
    config.devCommands = true;  // DevDie
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    REQUIRE_MESSAGE(game.Start(config, error), error);

    // An account's client in the lobby (the account made first when `make`).
    const auto connect = [&](const std::string& account, bool make) {
        if (make) REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
        return c;
    };
    const auto lobby = [&](const std::string& account, const std::string& name) {
        auto c = connect(account, true);
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        return c;
    };
    const auto select = [&](Client& c, const std::string& name) {
        c.Send(proto::CharacterSelect{name});
        proto::LobbyResult r;
        REQUIRE(c.Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
    };
    auto a = lobby("autoa", "Alpha");
    auto b = lobby("autob", "Beta");
    // Alpha's potions (given while its account is away: the game server keeps a player's items):
    // one HP potion alone (place 13), then a stack of five (14), and MP ones (15).
    a->Connection().Close();
    a.reset();
    {
        bool ok = false;
        const std::int64_t jid = server::FindAccount(t.Db(), "autoa", ok)->id;
        for (int i = 0; i < 2000 && t.Db().Query("SELECT 1 FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Rows() > 0; ++i) pump();
    }
    const auto give = [&](std::uint32_t ref, std::uint16_t count) {
        server::StoredItem item;
        item.refId = ref;
        item.quantity = count;
        item.counted = true;
        REQUIRE(server::GiveItems(t.Db(), config.id, "Alpha", {item}, 0, error) == server::GiveResult::Done);
    };
    give(kHpPotion, 1);
    give(kHpPotion, 5);
    give(kMpPotion, 10);
    // Alpha comes in hurt: 10 HP.
    REQUIRE(t.Db().Query("UPDATE _char SET hp = 10 WHERE charname16 = 'Alpha'").Ok());
    a = connect("autoa", false);

    select(*a, "Alpha");
    a->Send(proto::WorldEnter{});
    proto::AutoPotionSettings settings;
    REQUIRE(a->Expect(settings));
    // A new character's are the original's defaults: everything off, 50 %, the delay on at 1 s.
    CHECK_FALSE(settings.hp.enabled);
    CHECK(settings.hp.percent == 50);
    CHECK(settings.hp.item == 0);
    CHECK(settings.delayEnabled);
    CHECK(settings.delayMs == 1000);
    proto::WorldEntered ea;
    REQUIRE(a->Expect(ea));
    proto::Vitals vitals;
    REQUIRE(a->Expect(vitals));
    CHECK(vitals.hp == 10);
    const std::uint32_t maxHp = vitals.maxHp;
    REQUIRE(maxHp > 100);
    select(*b, "Beta");
    b->Send(proto::WorldEnter{});
    proto::WorldEntered eb;
    REQUIRE(b->Expect(eb));
    proto::EntityAppeared seen;
    REQUIRE(b->Expect(seen)); // Alpha, beside Beta

    // Above its threshold (1 %: 10 HP is more than its most / 100 only when the most is under 1,000;
    // here 10 > trunc(most x 1 / 100) for a most under 1,000): nothing is used.
    REQUIRE(maxHp < 1000);
    proto::AutoPotionSettings set;
    set.hp = {true, 1, kHpPotion};
    set.mp = {false, 50, kHpPotion}; // does not fit the MP row: left out
    set.abnormal = {false, 0, kPill};
    set.delayEnabled = true;
    set.delayMs = 1000;
    a->Send(set);
    proto::AutoPotionSettings kept;
    REQUIRE(a->Expect(kept));
    CHECK(kept.hp.enabled);
    CHECK(kept.hp.percent == 1);
    CHECK(kept.hp.item == kHpPotion);
    CHECK(kept.mp.item == 0);
    CHECK(kept.abnormal.item == kPill);
    proto::ItemMoved moved;
    CHECK_FALSE(a->Expect(moved, 600ms));

    // Under it (100 %: always): the first stack (place 13, one potion) at once; its kind locked for
    // 1.1 s (a Chinese character's flat potion); the first step of its 125 HP; Alpha and Beta told.
    Drain<proto::Vitals>(*a);
    set.hp.percent = 100;
    a->Send(set);
    REQUIRE(a->Expect(kept));
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(a->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Ok);
    REQUIRE(moved.emptied.size() == 1);
    CHECK(moved.emptied[0] == 13);
    proto::Cooldown cool;
    REQUIRE(a->Expect(cool));
    CHECK(cool.kind == proto::CooldownKind::Item);
    CHECK(cool.id == kHpPotion);
    CHECK(cool.ms == 1100);
    REQUIRE(a->Expect(vitals));
    CHECK(vitals.hp >= 10 + 25); // its first step (and the regeneration out of fights, 3 % every 3 s)
    CHECK(vitals.hp < maxHp);
    proto::ItemUsed used;
    REQUIRE(a->Expect(used));
    CHECK(used.entity == ea.entity);
    CHECK(used.item == kHpPotion);
    REQUIRE(b->Expect(used));
    CHECK(used.entity == ea.entity);
    CHECK(used.item == kHpPotion);

    // The next one comes from the next stack (place 14: four left), not before the lock is over.
    REQUIRE(a->Expect(moved, 5s));
    const double gap = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(gap >= 1.05);
    CHECK(moved.emptied.empty());
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].slot == 14);
    CHECK(moved.changed[0].quantity == 4);

    // Dead: nothing is used, though the row would hold.
    a->Send(proto::DevDie{});
    for (int i = 0; i < 50; ++i) pump();
    while (a->Expect(moved, 100ms)) {
    } // a use that was on its way
    CHECK_FALSE(a->Expect(moved, 2500ms));

    // Kept: the original's words in _clientconfig, the item in sr_auto_potion, and the settings back
    // when the character comes in again.
    a->Send(proto::WorldLeave{});
    proto::WorldLeft left;
    REQUIRE(a->Expect(left));
    for (int i = 0; i < 50; ++i) pump();
    const server::QueryResult words = t.Db().Query(
        "SELECT c.configtype, c.data FROM _clientconfig c JOIN _char ch ON ch.charid = c.charid "
        "WHERE ch.charname16 = 'Alpha' AND c.configtype BETWEEN 11 AND 14 ORDER BY c.configtype");
    REQUIRE(words.Rows() == 4);
    CHECK(words.Int(0, 1) == game::EncodeAutoPotionWord({true, 100, 0})); // 0xE400
    CHECK(words.Int(0, 1) == 0xE400);
    CHECK(words.Int(3, 1) == 0x8A);
    const server::QueryResult items = t.Db().Query(
        "SELECT p.configtype, p.refobjid FROM sr_auto_potion p JOIN _char ch ON ch.charid = p.charid WHERE ch.charname16 = 'Alpha' ORDER BY p.configtype");
    REQUIRE(items.Rows() == 2);
    CHECK(items.Int(0, 1) == kHpPotion);
    CHECK(items.Int(1, 0) == 13);
    CHECK(items.Int(1, 1) == kPill);
    select(*a, "Alpha");
    a->Send(proto::WorldEnter{});
    REQUIRE(a->Expect(settings));
    CHECK(settings.hp.enabled);
    CHECK(settings.hp.percent == 100);
    CHECK(settings.hp.item == kHpPotion);
    CHECK(settings.abnormal.item == kPill);
    CHECK(settings.delayMs == 1000);

    // The manual rules hold for a hand's use too: the MP potion (its own kind) while HP's is locked.
    REQUIRE(a->Expect(ea));
    set.hp.enabled = false;
    a->Send(set);
    REQUIRE(a->Expect(kept));
    for (int i = 0; i < 100; ++i) pump(); // what was on its way (a use as it came in at full HP: 100 % holds)
    Drain<proto::ItemMoved>(*a);
    Drain<proto::Cooldown>(*a);
    Drain<proto::ItemUsed>(*b);
    a->Send(proto::UseItem{15});
    REQUIRE(a->Expect(moved));
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].slot == 15);
    CHECK(moved.changed[0].quantity == 9);
    REQUIRE(a->Expect(cool));
    CHECK(cool.id == kMpPotion);
    REQUIRE(b->Expect(used));
    CHECK(used.item == kMpPotion);
    game.Stop();
}
