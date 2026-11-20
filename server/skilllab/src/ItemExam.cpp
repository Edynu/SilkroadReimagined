#include "sr/server/ItemExam.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <thread>
#include <tuple>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near // windows.h's old pointer words, names of fields in sr/game/Quest.h
#undef far
#endif

#include <nlohmann/json.hpp>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "sr/game/Alchemy.h"
#include "sr/game/AlchemyProtocol.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Cos.h"
#include "sr/game/CosProtocol.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/ItemCatalogue.h"
#include "sr/game/Mall.h"
#include "sr/game/MallServices.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/GuildProtocol.h"
#include "sr/game/Guild.h"
#include "sr/game/EventProtocol.h"
#include "sr/game/GuildRecall.h"
#include "sr/game/MagicPop.h"
#include "sr/server/DatabaseText.h"
#include "sr/assets/Gacha.h"
#include "sr/server/Fortress.h"
#include "sr/server/FortressStore.h"
#include "sr/game/FortressProtocol.h"
#include "sr/assets/Shop.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/Items.h"
#include "sr/game/Jobs.h"
#include "sr/game/Potions.h"
#include "sr/game/QuestItems.h"
#include "sr/game/Sets.h"
#include "sr/game/SpecialItems.h"
#include "sr/game/Spirit.h"
#include "sr/game/Combat.h"
#include "sr/game/Stats.h"
#include "sr/game/Protocol.h"
#include "sr/game/Skills.h"
#include "sr/game/States.h"
#include "sr/game/WorldProtocol.h"
#include "sr/net/Connection.h"
#include "sr/server/Database.h"
#include "sr/server/GameServer.h"
#include "sr/server/Import.h"
#include "sr/server/ItemLab.h"
#include "sr/server/Store.h"
#include "sr/server/QuestStore.h"

#include "ExamClient.h"

namespace sr::server {

namespace {

namespace proto = game::proto;
using json = nlohmann::json;
using game::ItemRoute;
using lab::Clock;
using lab::ExamClient;
using lab::Ms;
using lab::WithDatabase;
using namespace std::chrono_literals;

// The paths the exam knows, in the order it runs them (the order players meet them; L13).
constexpr ItemRoute kRoutes[] = {ItemRoute::Recovery,   ItemRoute::ReturnScroll, ItemRoute::ReverseReturn, ItemRoute::ThiefDen,
                                 ItemRoute::BuffScroll, ItemRoute::Equip,        ItemRoute::Avatar,        ItemRoute::Ammunition,
                                 ItemRoute::PetItem,  ItemRoute::Alchemy,      ItemRoute::Socket,        ItemRoute::OriginalRefuses,
                                 ItemRoute::QuestUse, ItemRoute::QuestItem,    ItemRoute::NotUsable,     ItemRoute::Declined,
                                 ItemRoute::Firework, ItemRoute::BerserkPotion, ItemRoute::BonusScroll,  ItemRoute::TimeTicket,
                                 ItemRoute::Premium,  ItemRoute::Expansion,    ItemRoute::JobSuit,       ItemRoute::CosSummon,
                                 ItemRoute::PetSummon, ItemRoute::Talisman,    ItemRoute::Resurrection,  ItemRoute::MagicCube,
                                 ItemRoute::GlobalChat, ItemRoute::GenderSwitch, ItemRoute::RepairHammer, ItemRoute::SpiritExtension,
                                 ItemRoute::SkinChange, ItemRoute::PetFood,    ItemRoute::PetRecovery,   ItemRoute::PetCure,
                                 ItemRoute::PetGrowth, ItemRoute::PetTreatment, ItemRoute::Gold,        ItemRoute::SkillWithdraw, ItemRoute::GuildCrest,
                                 ItemRoute::GuildRecall, ItemRoute::MonsterMask, ItemRoute::StallDecoration, ItemRoute::FlagCard,
                                 ItemRoute::MagicPop,  ItemRoute::Hole,        ItemRoute::Fortress,      ItemRoute::TradeGood};

// The stand-in figures the exam's players are made with (their race and sex are what count): [race][male].
constexpr std::uint32_t kFigure[2][2] = {{101, 100}, {201, 200}};
// In the field west of Constantinople (the exam's database holds no nests: nothing else is about).
constexpr proto::WorldPosition kPlace{assets::RegionId(76, 105), 600.0f, 140.0f, 900.0f};
constexpr int kHighLevel = 110;      // GameRates::levelCap's default: the 1.188 content's
constexpr std::uint16_t kServer = 1; // the exam's game server's id (its tickets)
constexpr float kNear = 60.0f;       // an arrival this near its place is there (the ground under it may move it)

struct Result {
    int scenario = 1;
    std::string variant, status, summary;
    json expected = json::object(), observed = json::object();
};
Result Verdict(int scenario, std::string variant, const std::vector<std::string>& failed, std::string text, json expected, json observed) {
    Result r{scenario, std::move(variant), failed.empty() ? "pass" : "fail", std::move(text), std::move(expected), std::move(observed)};
    if (!failed.empty()) {
        r.summary.clear();
        for (const std::string& f : failed) r.summary += (r.summary.empty() ? "" : "; ") + f;
    }
    return r;
}
Result Skip(int scenario, std::string variant, std::string why) { return {scenario, std::move(variant), "skip", std::move(why)}; }

int LevelOf(const assets::ItemRow& row) {
    int level = 0;
    for (int i = 0; i < 4; ++i) {
        if (row.requirementTypes[std::size_t(i)] == 1) level = std::max(level, row.requirementLevels[std::size_t(i)]);
    }
    return level;
}
// The exam's player for an item: its country's (both: China), its sex's (either: a man).
int RaceFor(const assets::ItemRow& row) { return row.country == 1 ? 1 : 0; }
bool MaleFor(const assets::ItemRow& row) { return row.sex != 0; }

float Distance2d(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    // Both in one region (the callers check it): its local metres.
    return std::hypot(a.x - b.x, a.z - b.z);
}
json Where(const proto::WorldPosition& p) { return {{"region", std::format("{},{}", p.region & 0xFF, p.region >> 8)}, {"x", std::lround(p.x)}, {"z", std::lround(p.z)}}; }

// What the exam reads from the game files, shared by its game server and its players.
struct Data {
    std::shared_ptr<ItemLabData> lab;
    std::shared_ptr<assets::ItemTable> items;
    std::shared_ptr<assets::SkillTable> skills;
    std::shared_ptr<assets::CharacterTable> characters;
    std::shared_ptr<assets::TeleportTable> teleports;
    std::shared_ptr<game::SpiritTable> spirits; // the Devil's Spirit's by-plus options and skills
    // magicoptionassign.txt: the avatars' lists at the smith
    std::shared_ptr<std::vector<assets::MagicOptionAssignRow>> assign;
    std::uint32_t smith = 0; // NPC_CH_SMITH's RefObjID: it stands beside the exam's place
    std::vector<game::ItemCatalogueEntry> catalogue;
    std::array<std::uint32_t, 2> mastery{}; // a mastery of each race (DevLearn raises the level with it)
    // Magic POP's tables (the database's) and its first machine and Gori's RefObjIDs, beside the exam's place
    std::shared_ptr<assets::GachaTable> gacha;
    std::uint32_t popMachine = 0, popGori = 0;
    // The fortress war's: the fortresses, their items' rows and the guards' variants; the holder's
    // and the attacker's players, made in the database before the server starts (profile -> account, character).
    std::vector<FortressSpec> fortresses;
    std::vector<FortressItemRow> fortItems;
    std::vector<SiegeVariantRow> fortGuards;
    std::map<int, std::pair<std::string, std::string>> prepared;
    // The trade's: the merchants' shops, the specialty NPCs beside the exam's place (codename -> RefObjID).
    std::shared_ptr<assets::ShopTable> shops;
    std::map<std::string, std::uint32_t> specials;

    bool Load(assets::FileSystem& files, std::string& error) {
        lab = std::make_shared<ItemLabData>();
        if (!LoadItemLabData(files, *lab, error)) return false;
        items = std::shared_ptr<assets::ItemTable>(lab, &lab->items);
        skills = std::shared_ptr<assets::SkillTable>(lab, &lab->skills);
        characters = std::shared_ptr<assets::CharacterTable>(lab, &lab->characters);
        teleports = std::make_shared<assets::TeleportTable>();
        if (!teleports->Load(files, error)) return (error = "the teleports: " + error), false;
        game::ItemRouteData route;
        route.skills = skills.get();
        route.characters = characters.get();
        route.talismans = &lab->talismans;
        catalogue = game::BuildItemCatalogue(*items, route);
        spirits = std::make_shared<game::SpiritTable>();
        if (!game::LoadSpiritTable(files, lab->options, *spirits, error)) return (error = "the spirits: " + error), false;
        assign = std::make_shared<std::vector<assets::MagicOptionAssignRow>>();
        const auto assignFile = files.Read("server_dep\\silkroad\\textdata\\magicoptionassign.txt");
        if (!assignFile || !assets::DecodeMagicOptionAssign(core::DecodeTextFile(assignFile->Bytes()), *assign, error)) {
            return (error = "magicoptionassign.txt: " + (assignFile ? error : std::string("missing"))), false;
        }
        if (const assets::ObjectRow* npc = characters->Find(std::string_view("NPC_CH_SMITH"))) smith = npc->refObjId;
        for (const int race : {0, 1}) {
            for (const assets::MasteryRow& m : skills->masteries()) {
                if (!skills->Board(m.id).empty() && game::MasteryOfRace(*skills, m.id, race)) {
                    mastery[std::size_t(race)] = m.id;
                    break;
                }
            }
        }
        if (mastery[0] == 0 || mastery[1] == 0) return (error = "no mastery of each race"), false;
        return true;
    }
};

// A player of the exam: its client, and what the server told it of its places, health and progress.
struct Tester {
    std::unique_ptr<ExamClient> c;
    std::string name;
    int race = 0;
    bool male = true;
    std::uint32_t entity = 0;
    std::map<std::uint8_t, proto::ItemRecord> places; // the worn places and the bag
    proto::Vitals vitals;
    proto::Progress progress;
    std::size_t seen = 0;
    std::map<std::uint32_t, Clock::time_point> lockedUntil; // a recovery kind's lock (its bit) runs until then
    std::map<std::uint32_t, std::uint32_t> models; // what it has seen: a model (characterdata RefObjID) -> its entity

    // A kind's lock waited out (DevReset leaves the items' locks: a potion right after another of its kind is refused).
    void WaitLock(std::uint32_t bit) {
        const auto it = lockedUntil.find(bit);
        if (bit == 0 || it == lockedUntil.end()) return;
        const auto now = Clock::now();
        if (now < it->second) c->Pump(std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now) + std::chrono::milliseconds(80));
    }

    void Sync() {
        const std::size_t end = c->Mark();
        for (auto& [at, m] : c->Since<proto::InventoryList>(seen)) {
            places.clear();
            for (const proto::ItemRecord& r : m.items) places[r.slot] = r;
        }
        for (auto& [at, m] : c->Since<proto::ItemMoved>(seen)) {
            for (const proto::ItemRecord& r : m.changed) places[r.slot] = r;
            for (const std::uint8_t s : m.emptied) places.erase(s);
        }
        for (auto& [at, m] : c->Since<proto::AvatarMoved>(seen)) {
            for (const proto::ItemRecord& r : m.bag) places[r.slot] = r;
            for (const std::uint8_t s : m.bagEmptied) places.erase(s);
        }
        for (auto& [at, m] : c->Since<proto::EntityAppeared>(seen)) {
            if (m.kind == proto::EntityKind::Character || m.kind == proto::EntityKind::Monster) models[m.character.model] = m.entity;
        }
        if (const auto v = c->Since<proto::Vitals>(seen); !v.empty()) vitals = v.back().second;
        if (const auto p = c->Since<proto::Progress>(seen); !p.empty()) progress = p.back().second;
        seen = end;
    }
    // The bag's count of an item (its every stack).
    int Count(std::uint32_t refId) const {
        int n = 0;
        for (const auto& [slot, r] : places) {
            if (slot >= game::slot::kWorn && r.item == refId) n += r.quantity;
        }
        return n;
    }
    // The log kept short: what came is read into the places first.
    void Trim() {
        if (c->Mark() < 4000) return;
        Sync();
        c->Forget();
        seen = 0;
    }
};

// One share of the items on players of its own, against a game server on `port` (the exam's own, in this process or
// in the exam's).
class Runner {
public:
    Runner(const ItemExamOptions& o, ItemExamReport& report, const Data& data, Database& results, Database& exam, net::NetService& net, std::uint16_t port,
           std::string pem, std::function<void()> pump)
        : options_(o), report_(report), data_(data), results_(results), exam_(exam), net_(net), port_(port), pem_(std::move(pem)), pump_(std::move(pump)) {
        std::string error;
        std::vector<OptionalTeleportRow> points;
        if (LoadOptionalTeleports(exam_, points, error)) points_ = std::move(points);
    }

    bool Run(std::string& error) {
        const std::vector<const game::ItemCatalogueEntry*> share = Share();
        core::LogInfo("SroSkillLab: item exam part {}/{}: {} items", options_.part, options_.parts, share.size());
        for (const game::ItemCatalogueEntry* e : share) {
            std::vector<Result> results;
            switch (e->routing.route) {
            case ItemRoute::Recovery: results = Recovery(*e->row, error); break;
            case ItemRoute::ReturnScroll: results = Return(*e->row, error); break;
            case ItemRoute::ReverseReturn: results = Reverse(*e->row, error); break;
            case ItemRoute::ThiefDen: results = ThiefDen(*e->row, error); break;
            case ItemRoute::BuffScroll: results = Buff(*e->row, error); break;
            case ItemRoute::Equip:
            case ItemRoute::Ammunition: results = Equip(*e->row, error); break;
            case ItemRoute::Avatar: results = Avatar(*e->row, error); break;
            case ItemRoute::PetItem: results = PetItem(*e->row, error); break;
            case ItemRoute::Alchemy:
            case ItemRoute::Socket: results = Alchemy(*e->row, error); break;
            case ItemRoute::OriginalRefuses:
            case ItemRoute::QuestUse:
            case ItemRoute::QuestItem:
            case ItemRoute::NotUsable:
            case ItemRoute::Declined:
            case ItemRoute::Firework:
            case ItemRoute::BerserkPotion:
            case ItemRoute::BonusScroll:
            case ItemRoute::TimeTicket:
            case ItemRoute::Premium:
            case ItemRoute::Expansion: results = BagUse(*e->row, e->routing.route, error); break;
            case ItemRoute::JobSuit: results = Suit(*e->row, error); break;
            case ItemRoute::CosSummon:
            case ItemRoute::PetSummon: results = Summon(*e->row, e->routing.route, error); break;
            case ItemRoute::Talisman: results = TalismanUse(*e->row, error); break;
            case ItemRoute::Resurrection: results = Rise(*e->row, error); break;
            case ItemRoute::MagicCube:
            case ItemRoute::GlobalChat:
            case ItemRoute::GenderSwitch:
            case ItemRoute::RepairHammer:
            case ItemRoute::SpiritExtension:
            case ItemRoute::SkinChange: results = {MallTool(*e->row, e->routing.route, error)}; break;
            case ItemRoute::PetFood:
            case ItemRoute::PetRecovery:
            case ItemRoute::PetCure:
            case ItemRoute::PetGrowth:
            case ItemRoute::PetTreatment: results = {PetCare(*e->row, e->routing.route, error)}; break;
            case ItemRoute::Gold: results = {GoldLook(*e->row, error)}; break;
            case ItemRoute::SkillWithdraw: results = {Withdrawal(*e->row, error)}; break;
            case ItemRoute::GuildCrest: results = {Crest(*e->row, error)}; break;
            case ItemRoute::GuildRecall: results = {Recall(*e->row, error)}; break;
            case ItemRoute::MonsterMask:
            case ItemRoute::StallDecoration:
            case ItemRoute::FlagCard: results = {PlainUse(*e->row, e->routing.route, error)}; break;
            case ItemRoute::MagicPop: results = {Pop(*e->row, error)}; break;
            case ItemRoute::Hole: results = {Hole(*e->row, error)}; break;
            case ItemRoute::Fortress: results = {FortressItem(*e->row, error)}; break;
            case ItemRoute::TradeGood: results = {TradeGood(*e->row, error)}; break;
            default: break;
            }
            if (!error.empty()) return false; // a player could not be made: the exam cannot go on
            for (const Result& r : results) Write(*e, r);
        }
        return true;
    }

private:
    // --- The share and the players ----------------------------------------------------------------------------

    std::vector<const game::ItemCatalogueEntry*> Share() const {
        std::vector<const game::ItemCatalogueEntry*> out;
        for (const ItemRoute route : kRoutes) {
            const std::string name = game::ItemRouteName(route);
            if (!options_.routes.empty() && std::find(options_.routes.begin(), options_.routes.end(), name) == options_.routes.end()) continue;
            int index = -1;
            // The fortress war's items all in the first part (its holder and attacker are one pair of players), those used
            // outside the war first: the war started once (its end spends the applications).
            const bool fortress = route == ItemRoute::Fortress;
            const std::size_t first = out.size();
            for (const game::ItemCatalogueEntry& e : data_.catalogue) {
                if (e.routing.route != route || (!options_.item.empty() && e.row->codename != options_.item)) continue;
                if (options_.limit > 0 && index + 1 >= options_.limit) break;
                ++index;
                if (fortress ? options_.part != 0 : index % std::max(1, options_.parts) != options_.part) continue;
                out.push_back(&e);
            }
            if (fortress) {
                const auto period = [&](const game::ItemCatalogueEntry* e) {
                    const auto r = std::find_if(data_.fortItems.begin(), data_.fortItems.end(), [&](const FortressItemRow& x) { return x.refObjId == e->row->refObjId; });
                    return r == data_.fortItems.end() ? 1 : r->time == 2 ? 0 : r->time == 0 ? 1 : 2;
                };
                std::stable_sort(out.begin() + std::ptrdiff_t(first), out.end(), [&](auto* a, auto* b) { return period(a) < period(b); });
            }
        }
        return out;
    }

    // The exam's kinds of player: at level 1; at the level cap with its race's masteries (a European the warrior's and
    // the wizard's: every European armour's mastery requirement met); at the cap with the
    // warrior's alone, or the cleric's alone (the mastery refusals).
    // kFortress: at the cap and its guild's master, the fortress war's Commander (position 1).
    // kThief: at the cap and in the thieves' league (its _CharTrijob row written before it enters).
    enum Profile : int { kLow = 0, kHigh = 1, kWarriorOnly = 2, kClericOnly = 3, kFortress = 4, kThief = 5, kTrader = 6, kHunter = 7, kKeeper = 8, kRaider = 9 };
    static constexpr std::uint32_t kWarrior = 0x201, kWizard = 0x202, kCleric = 0x204;
    // A player of its own made at `level` (no masteries): the `key`th of its kind.
    static Profile Banded(int key, int level) { return Profile(1000 * (key + 1) + std::clamp(level, 1, 999)); }

    // The player of that race and sex, at the level cap (`high`) or at level 1, made the first time it is asked for.
    Tester* Get(int race, bool male, bool high, std::string& error) { return Get(race, male, high ? kHigh : kLow, error); }
    Tester* Get(int race, bool male, Profile profile, std::string& error) {
        std::unique_ptr<Tester>& t = testers_[{race, male, int(profile)}];
        if (!t || !t->c->Open()) t = Make(race, male, profile, error);
        return t.get();
    }

    // A fresh player in the world at the exam's place, its bag empty, raised to the level cap with its profile's masteries.
    std::unique_ptr<Tester> Make(int race, bool male, bool high, std::string& error) { return Make(race, male, high ? kHigh : kLow, error); }
    std::unique_ptr<Tester> Make(int race, bool male, Profile profile, std::string& error) {
        const int band = int(profile) >= 1000 ? int(profile) % 1000 : 0; // a level-band player (Banded)
        const bool high = profile != kLow && band == 0;
        auto t = std::make_unique<Tester>();
        t->race = race;
        t->male = male;
        ++made_;
        t->name = std::format("I{}{}{}{}{}", options_.part, race == 0 ? "C" : "E", male ? "M" : "W", band != 0 ? 'B' : "LHWCFTRUKA"[int(profile)], made_);
        // The fortress's holder and attacker: their characters made in the database before the server started (their guilds,
        // the holding, the application).
        const auto preparedIt = data_.prepared.find(int(profile));
        const bool prepared = preparedIt != data_.prepared.end();
        if (prepared) t->name = preparedIt->second.second;
        const std::string account = prepared ? preparedIt->second.first : std::format("itemexam{}x{}", options_.part, made_);
        if (!prepared && CreateAccount(exam_, account, "itemexam-secret", error) != CreateAccountResult::Created) {
            return (error = "the account " + account + ": " + error), nullptr;
        }
        bool ok = false;
        const auto row = FindAccount(exam_, account, ok);
        if (!row) return (error = "the account " + account), nullptr;
        t->c = std::make_unique<ExamClient>(net_, port_, pem_, [this] {
            pump_();
            DrainAll();
        });
        ExamClient& c = *t->c;
        std::size_t mark = c.Mark();
        proto::Hello hello;
        hello.client = "SroSkillLab";
        c.Send(hello);
        const auto welcome = c.Next<proto::Welcome>(mark);
        if (!welcome || welcome->result != proto::WelcomeResult::Accepted) return (error = "the exam's game server did not greet"), nullptr;
        proto::LobbyEnter enter;
        enter.ticket = IssueTicket(exam_, row->id, kServer, 30, error);
        mark = c.Mark();
        c.Send(enter);
        if (!c.Next<proto::LobbyEntered>(mark)) return (error = "the lobby"), nullptr;
        proto::CharacterCreate m;
        m.name = t->name;
        m.model = kFigure[race][male ? 1 : 0];
        const std::uint32_t set = std::uint32_t(race * 6 + (male ? 0 : 3)); // the creation's stand-ins (ExamConfig)
        m.chest = set + 1;
        m.legs = set + 2;
        m.shoes = set + 3;
        m.weapon = race == 0 ? 30 : 31;
        mark = c.Mark();
        if (!prepared) c.Send(m);
        const auto created = prepared ? std::optional<proto::LobbyResult>(proto::LobbyResult{}) : c.Next<proto::LobbyResult>(mark);
        if (!created || created->status != proto::LobbyStatus::Ok) {
            return (error = std::format("the player {} was not made (status {})", t->name, created ? int(created->status) : -1)), nullptr;
        }
        const std::vector<CharacterRow> list = ListCharacters(exam_, row->id, kServer, 0, ok);
        // Its own character by its name: the imported shard's _User may link the same account id to the original's own
        // characters ([GM] Admin is account 1's), so the first listed is not always ours.
        const auto mine = std::find_if(list.begin(), list.end(), [&](const CharacterRow& c) { return c.name == t->name; });
        if (mine == list.end()) return (error = "the player " + t->name), nullptr;
        const std::int64_t character = mine->id;
        CharacterRow::Place here;
        here.region = kPlace.region;
        here.x = kPlace.x;
        here.y = kPlace.y;
        here.z = kPlace.z;
        SavePlaces(exam_, {{character, here}});
        exam_.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot < 13", {character}); // no stand-in worn
        exam_.Query("UPDATE _char SET remaingold = 100000000 WHERE charid = $1", {character});         // gold to drop and pay with
        if (band != 0) exam_.Query("UPDATE _char SET curlevel = $2, maxlevel = $2 WHERE charid = $1", {character, band});
        if (profile == kThief || profile == kTrader || profile == kHunter) {
            // Its league (_CharTrijob: 1 trader, 2 thief, 3 hunter) at job level 7, the most any suit asks (ReqLevel2).
            const int job = profile == kTrader ? 1 : profile == kThief ? 2 : 3;
            exam_.Query("INSERT INTO _chartrijob (charid, jobtype, level, exp, contribution, reward) VALUES ($1, $2, 7, 0, 0, 0) "
                        "ON CONFLICT (charid) DO UPDATE SET jobtype = $2, level = 7, exp = 0",
                        {character, job});
            // A trader's markets: the trade quests the specialty NPCs ask finished once.
            if (profile == kTrader) {
                exam_.Query("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                            "SELECT $1, q.id, 4, 1, now(), now(), 0, 0 FROM sr_quests q WHERE q.codename LIKE 'QNO_TRADE_%' ON CONFLICT DO NOTHING",
                            {character});
            }
            // Its alias: a league's suit is refused without one (0x189F).
            exam_.Query("UPDATE _char SET nickname16 = $2 WHERE charid = $1", {character, std::format("Thief{}x{}", options_.part, made_)});
        }
        mark = c.Mark();
        c.Send(proto::CharacterSelect{t->name});
        if (!c.Next<proto::LobbyResult>(mark)) return (error = "selecting " + t->name), nullptr;
        mark = c.Mark();
        c.Send(proto::WorldEnter{});
        const auto entered = c.Next<proto::WorldEntered>(mark);
        if (!entered || entered->status != proto::WorldStatus::Ok) return (error = t->name + " did not enter the world"), nullptr;
        t->entity = entered->entity;
        c.Pump(300ms);
        if (high) {
            std::vector<std::uint32_t> masteries;
            if (profile == kWarriorOnly) masteries = {kWarrior};
            else if (profile == kClericOnly) masteries = {kCleric};
            else if (race == 1) masteries = {kWarrior, kWizard};
            else masteries = {data_.mastery[std::size_t(race)]};
            for (const std::uint32_t m : masteries) {
                mark = c.Mark();
                c.Send(proto::DevLearn{m, std::uint8_t(kHighLevel), 1});
                if (!c.Wait<proto::SkillBookList>(mark, [&](const proto::SkillBookList& b) {
                        return std::any_of(b.masteries.begin(), b.masteries.end(), [&](const proto::MasteryRecord& x) { return x.mastery == m && x.level >= kHighLevel; });
                    })) {
                    return (error = std::format("{} did not learn mastery {}", t->name, m)), nullptr;
                }
            }
            c.Pump(100ms);
            c.Send(proto::DevReset{proto::DevReset::kAll});
            c.Pump(200ms);
        }
        if (profile == kFortress) {
            // Its own guild through the server (DevGuild): its master holds siege authority 1, the Commander.
            mark = c.Mark();
            c.Send(proto::DevGuild{std::format("IxGuild{}x{}", options_.part, made_), 5}); // level 5: crests, a union
            c.Pump(400ms);
        }
        t->Sync();
        return t;
    }

    // Every player's connection read while one waits: they stand together and see each other's broadcasts, and an idle
    // one's unread queue would fill and close its connection.
    void DrainAll() {
        for (auto& [key, t] : testers_) {
            if (t && t->c) t->c->Drain();
        }
    }
    // The players' logs kept short (between the steps, never inside a wait: the waits hold marks into them).
    void TrimAll() {
        for (auto& [key, t] : testers_) {
            if (t && t->c) t->Trim();
        }
    }

    // The player's own put back (proto::DevReset's bits); a bag emptied is emptied in its places too.
    void Reset(Tester& t, std::uint8_t what) {
        TrimAll();
        t.Trim();
        t.c->Send(proto::DevReset{what});
        t.c->Pump(40ms);
        t.Sync();
        if ((what & proto::DevReset::kBag) != 0) {
            for (auto it = t.places.begin(); it != t.places.end();) it = it->first >= game::slot::kWorn ? t.places.erase(it) : std::next(it);
        }
    }

    // `count` of the item into the bag (DevGive): the place it went to.
    std::optional<std::uint8_t> Give(Tester& t, const assets::ItemRow& row, std::uint32_t count = 1, std::uint32_t data = 0) {
        const std::size_t mark = t.c->Mark();
        proto::DevGive give{row.codename, count, false};
        give.data = data;
        t.c->Send(give);
        const auto moved = t.c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) {
            return std::any_of(m.changed.begin(), m.changed.end(), [&](const proto::ItemRecord& r) { return r.item == row.refObjId; });
        }, 3s);
        t.Sync();
        if (!moved) return std::nullopt;
        for (const proto::ItemRecord& r : moved->changed) {
            if (r.item == row.refObjId) return r.slot;
        }
        return std::nullopt;
    }

    // The bag's first place holding the item (a second use: an item that does not stack is in another place).
    static std::optional<std::uint8_t> SlotOf(Tester& t, std::uint32_t refId) {
        t.Sync();
        for (const auto& [slot, r] : t.places) {
            if (slot >= game::slot::kWorn && r.item == refId) return slot;
        }
        return std::nullopt;
    }
    std::optional<proto::ItemRefused> RefusedSince(Tester& t, std::size_t mark, std::uint32_t refId) const {
        for (auto& [at, m] : t.c->Since<proto::ItemRefused>(mark)) {
            if (m.item == refId) return m;
        }
        return std::nullopt;
    }

    // The player's numbers as the server holds them (DevSheet): by name ("strength", "maxHp", ...).
    std::map<std::string, float> Sheet(Tester& t) {
        std::map<std::string, float> out;
        const std::size_t mark = t.c->Mark();
        t.c->Send(proto::DevSheet{});
        if (const auto a = t.c->Next<proto::DevSheetAnswer>(mark, 2s)) {
            for (const proto::DevNumber& n : a->numbers) out[n.name] = n.value;
        }
        return out;
    }
    // A blue of that name for an item of that degree (the nearest degree under it when it has none of its own).
    const assets::MagicOptionRow* OptionFor(std::string_view name, int degree) const {
        const assets::MagicOptionRow* best = nullptr;
        for (const assets::MagicOptionRow& o : data_.lab->options) {
            if (o.optionName != name || o.degree > degree) continue;
            if (best == nullptr || o.degree > best->degree) best = &o;
        }
        return best;
    }

    // --- The pets' watches and skills ----------------------------------------
    // Its pet's item given and the pet summoned; the item (a watch's whole stack) used on it: its answer (0, the count
    // taken: a watch its minutes, at most the rental's), a watch's bag 28 places more, the skill on the pet; the same again
    // refused 0x1894; the other kind of pet 3.
    std::vector<Result> PetItem(const assets::ItemRow& row, std::string& error) {
        const game::PetItemJobs jobs = game::PetItemJobsOf(row);
        const bool grab = jobs.grabPet;
        const assets::ItemRow* petItem = data_.items->Find(std::string_view(grab ? "ITEM_COS_P_RABBIT_SCROLL_SILK" : "ITEM_COS_P_FLUTE"));
        const assets::ItemRow* otherItem = data_.items->Find(std::string_view(grab ? "ITEM_COS_P_FLUTE" : "ITEM_COS_P_RABBIT_SCROLL_SILK"));
        if (!jobs || petItem == nullptr || otherItem == nullptr) return {Skip(1, "use", "no jobs, or no pet item to try it on")};
        Tester* t = Get(0, true, true, error);
        if (t == nullptr) return {};
        Reset(*t, proto::DevReset::kAll);
        const auto petSlot = Give(*t, *petItem);
        const auto otherSlot = Give(*t, *otherItem);
        const int count = grab ? std::max(1, row.maxStack) : 2;
        const auto slot = Give(*t, row, std::uint32_t(count));
        if (!petSlot || !otherSlot || !slot) return {Verdict(1, "use", {"not given (DevGive)"}, "", {}, {})};
        const auto summon = [&](std::uint8_t place) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::UseItem{place});
            return t->c->Wait<proto::CosSummoned>(mark, [](const proto::CosSummoned&) { return true; }, 3s);
        };
        const auto useOn = [&](std::uint8_t place, std::uint8_t on) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::UseItemOn{place, on});
            return std::make_pair(mark, t->c->Wait<proto::ServiceAnswer>(mark, [](const proto::ServiceAnswer& a) { return a.request == proto::ServiceRequest::UseOn; }, 3s));
        };
        std::vector<Result> out;
        const auto pet = summon(*petSlot);
        const auto other = summon(*otherSlot);
        if (!pet || !other) {
            Reset(*t, proto::DevReset::kAll);
            return {Verdict(1, "use", {"its pets did not come out"}, "", {}, {})};
        }
        // The other kind of pet: 3.
        {
            const auto [mark, a] = useOn(*slot, *otherSlot);
            std::vector<std::string> f;
            if (!a) f.push_back("no answer");
            else if (a->code != 3) f.push_back(std::format("answered 0x{:X}, 3 expected", a->code));
            out.push_back(Verdict(1, "wrong pet", f, grab ? "on an attack pet: 3" : "on a grab pet: 3", {{"code", 3}}, {{"code", a ? json(a->code) : json(nullptr)}}));
        }
        // Its own: done, the count taken, the bag, the skill on the pet.
        const assets::SkillRow* skill = data_.skills->Find(std::string_view(game::PetSkillCodename(jobs, pet->level)));
        const auto [mark, a] = useOn(*slot, *petSlot);
        t->c->Pump(300ms);
        std::vector<std::string> failed;
        json observed = {{"code", a ? json(a->code) : json(nullptr)}, {"taken", a ? json(a->b) : json(nullptr)}};
        if (!a) failed.push_back("no answer");
        else if (a->code != 0) failed.push_back(std::format("refused with 0x{:X}", a->code));
        // A watch: as many as the stack holds, at most the rental's minutes (a fresh pet's 40,320 less the minute begun).
        const int want = grab ? std::min(count, int(pet->rentSeconds / 60)) : 1;
        if (a && a->code == 0 && int(a->b) != want) failed.push_back(std::format("{} taken, {} expected", a->b, want));
        if (grab) {
            std::optional<int> size;
            for (auto& [at, b] : t->c->Since<proto::CosBag>(mark)) {
                if (b.entity == pet->entity) size = b.size;
            }
            observed["bag"] = size ? json(*size) : json(nullptr);
            if (size != game::kGrabPetBag + jobs.bagPlaces) failed.push_back(std::format("its bag {}, {} expected", size.value_or(-1), game::kGrabPetBag + jobs.bagPlaces));
        }
        std::optional<std::vector<std::uint32_t>> buffs;
        for (auto& [at, b] : t->c->Since<proto::EntityBuffs>(mark)) {
            if (b.entity == pet->entity) buffs = b.skills;
        }
        const bool skillOn = skill != nullptr && buffs && std::find(buffs->begin(), buffs->end(), skill->id) != buffs->end();
        observed["skillOn"] = skillOn;
        if (!skillOn) failed.push_back("its skill not on the pet");
        out.push_back(Verdict(1, "use", failed,
                              grab ? std::format("the grab pet's bag {} and {} for {} minutes", game::kGrabPetBag + jobs.bagPlaces, jobs.skill, want)
                                   : std::format("{} on the attack pet for 30 minutes", skill != nullptr ? skill->codename : jobs.skill),
                              {{"code", 0}, {"taken", want}}, observed));
        // The same again while it runs: 0x1894 (a watch's whole stack is gone: one more given).
        if (a && a->code == 0) {
            const auto again = SlotOf(*t, row.refObjId) ? SlotOf(*t, row.refObjId) : Give(*t, row, 1);
            if (again) {
                const auto [m2, b] = useOn(*again, *petSlot);
                std::vector<std::string> f;
                if (!b) f.push_back("no answer");
                else if (b->code != 0x1894) f.push_back(std::format("answered 0x{:X}, 0x1894 expected", b->code));
                out.push_back(Verdict(1, "again", f, "a second while one runs: 0x1894", {{"code", 0x1894}}, {{"code", b ? json(b->code) : json(nullptr)}}));
            }
        }
        t->c->Send(proto::CosDismiss{pet->entity});
        t->c->Send(proto::CosDismiss{other->entity});
        t->c->Pump(200ms);
        Reset(*t, proto::DevReset::kAll);
        return out;
    }

    // --- Recovery: potions and pills ----------------------------------------------

    std::vector<Result> Recovery(const assets::ItemRow& row, std::string& error) {
        Tester* t = Get(RaceFor(row), MaleFor(row), true, error);
        if (t == nullptr) return {};
        if (game::IsPotion(row)) return Potion(*t, row);
        if (game::IsCure(row)) return Pill(*t, row);
        return {Skip(1, "use", "neither a potion nor a pill by its type ids")};
    }

    std::vector<Result> Potion(Tester& t, const assets::ItemRow& row) {
        Reset(t, proto::DevReset::kAll);
        std::size_t mark = t.c->Mark();
        t.c->Send(proto::DevVitals{10, 10, 0});
        if (!t.c->Wait<proto::Vitals>(mark, [](const proto::Vitals& v) { return v.hp <= v.maxHp / 10 + 1; }, 3s)) return {Skip(1, "use", "its health was not lowered (DevVitals)")};
        const auto slot = Give(t, row, 2);
        if (!slot) return {Verdict(1, "use", {"not given (DevGive)"}, "", {}, {})};
        const proto::Vitals before = t.vitals;
        const game::Recovery r = game::RecoveryOf(row, t.progress.level, t.progress.strength, t.progress.intelligence, before.maxHp, before.maxMp);
        const game::ReuseLock lock = game::ReuseLockOf(row, t.race == 1);
        t.WaitLock(lock.bit);
        t.Sync();
        const proto::Vitals start = t.vitals;
        mark = t.c->Mark();
        const auto usedAt = Clock::now();
        t.c->Send(proto::UseItem{*slot});
        if (lock.bit != 0) t.lockedUntil[lock.bit] = usedAt + std::chrono::milliseconds(lock.ms);
        const auto used = t.c->Wait<proto::ItemUsed>(mark, [&](const proto::ItemUsed& u) { return u.entity == t.entity && u.item == row.refObjId; }, 2s);
        // A second at once, inside the lock: refused (nothing used, nothing taken).
        std::optional<bool> secondRefused;
        if (used && lock.bit != 0 && lock.ms >= 1000) {
            const std::size_t second = t.c->Mark();
            t.c->Send(proto::UseItem{SlotOf(t, row.refObjId).value_or(*slot)});
            secondRefused = !t.c->Wait<proto::ItemUsed>(second, [&](const proto::ItemUsed& u) { return u.entity == t.entity && u.item == row.refObjId; }, 400ms);
        }
        t.c->Pump(r.flat ? 4700ms : 400ms);
        t.Sync();
        // What came: the cooldown, the steps.
        std::optional<std::uint32_t> cooldown;
        for (auto& [at, m] : t.c->Since<proto::Cooldown>(mark)) {
            if (m.kind == proto::CooldownKind::Item && m.id == row.refObjId) cooldown = m.ms;
        }
        const std::uint32_t regen = std::min(before.maxHp / 2, std::max(1u, std::uint32_t(double(before.maxHp) * 0.8 / 100.0)));
        const std::uint32_t regenMp = std::min(before.maxMp / 2, std::max(1u, std::uint32_t(double(before.maxMp) * 0.8 / 100.0)));
        struct Gauge {
            std::uint32_t start = 0, most = 0, amount = 0, regen = 0;
            std::vector<std::uint32_t> deltas;
            std::vector<int> times; // ms after the use
            std::uint32_t last = 0;
        };
        Gauge hp{start.hp, start.maxHp, r.hp, regen}, mp{start.mp, start.maxMp, r.mp, regenMp};
        hp.last = hp.start;
        mp.last = mp.start;
        for (auto& [at, v] : t.c->Since<proto::Vitals>(mark)) {
            for (auto [g, value] : {std::pair<Gauge*, std::uint32_t>{&hp, v.hp}, {&mp, v.mp}}) {
                if (value > g->last) {
                    g->deltas.push_back(value - g->last);
                    g->times.push_back(Ms(at - usedAt));
                }
                g->last = value;
            }
        }
        std::vector<std::string> failed;
        json observed = {{"used", used.has_value()}, {"cooldownMs", cooldown ? json(*cooldown) : json(nullptr)}};
        json expected = {{"flat", r.flat}, {"hp", r.hp}, {"mp", r.mp}, {"lockBit", lock.bit}, {"lockMs", lock.ms}};
        if (!used) failed.push_back("not used (no ItemUsed)");
        if (lock.bit != 0 && cooldown != std::uint32_t(lock.ms)) failed.push_back(std::format("its cooldown {} ms, the lock is {} ms", cooldown ? int(*cooldown) : -1, lock.ms));
        if (t.Count(row.refObjId) != 1) failed.push_back(std::format("{} left of 2", t.Count(row.refObjId)));
        observed["left"] = t.Count(row.refObjId);
        for (auto [name, g] : {std::pair<const char*, Gauge*>{"hp", &hp}, {"mp", &mp}}) {
            observed[name] = {{"start", g->start}, {"most", g->most}, {"deltas", g->deltas}, {"atMs", g->times}, {"end", g->last}};
            if (g->amount == 0) continue;
            if (r.flat) {
                // Five steps of a fifth, one at once and one a second (the regeneration's own steps beside them).
                const std::uint32_t step = game::PotionStep(g->amount);
                expected[std::string(name) + "Step"] = step;
                const int steps = int(std::count(g->deltas.begin(), g->deltas.end(), step) + std::count(g->deltas.begin(), g->deltas.end(), step + g->regen));
                const bool full = g->last >= g->most;
                if (steps < game::kPotionSteps && !full) failed.push_back(std::format("{}: {} steps of {} (5 expected)", name, steps, step));
                const int firstStep = g->times.empty() ? -1 : g->times.front();
                if (firstStep > 400) failed.push_back(std::format("{}: its first step {} ms after the use (at once expected)", name, firstStep));
            } else {
                // A percent at once.
                const std::uint32_t want = std::min(g->most, g->start + g->amount);
                const std::uint32_t first = g->deltas.empty() ? g->start : g->start + g->deltas.front();
                expected[std::string(name) + "After"] = want;
                if (first != want && !(first > want && first <= want + g->regen)) failed.push_back(std::format("{}: {} after it, {} expected", name, first, want));
            }
        }
        std::vector<Result> out;
        out.push_back(Verdict(1, "use", failed,
                              r.flat ? std::format("{} HP and {} MP in five steps; its kind locked {} ms", r.hp, r.mp, lock.ms)
                                     : std::format("{} HP and {} MP at once; its kind locked {} ms", r.hp, r.mp, lock.ms),
                              expected, observed));
        if (secondRefused) {
            out.push_back(Verdict(2, "lock", *secondRefused ? std::vector<std::string>{} : std::vector<std::string>{"a second was drunk inside the lock"},
                                  std::format("a second inside the {} ms lock refused", lock.ms), {{"lockMs", lock.ms}}, {{"refused", *secondRefused}}));
        }
        Reset(t, proto::DevReset::kAll);
        return out;
    }

    std::vector<Result> Pill(Tester& t, const assets::ItemRow& row) {
        const game::CureSpec cure = game::PillCureOf(row);
        // The state put on as a hit would put it: a curse at level 1 for 30 s; an element (the universal pill takes its
        // level's worth of time off: game::StateSet::Cure) at the pill's level, for that level's time: of the mask's
        // elements the one with the longest time a level (game/src/States.cpp's table, the original's).
        constexpr std::pair<game::StateKind, int> kElementMs[] = {{game::StateKind::Poison, 1000}, {game::StateKind::Burn, 750},
                                                                  {game::StateKind::Zombie, 750},  {game::StateKind::Shock, 500},
                                                                  {game::StateKind::Frostbite, 250}, {game::StateKind::Freeze, 97}};
        std::optional<game::StateKind> kind;
        int level = 1;
        std::uint32_t ms = 30000;
        for (const auto& [k, perLevel] : kElementMs) {
            if (!kind && (game::StateBit(k) & cure.elementMask) != 0) {
                kind = k;
                level = std::max(1, cure.elementLevel);
                ms = std::uint32_t(level * perLevel);
            }
        }
        for (int k = 0; k <= int(game::StateKind::TimeBomb) && !kind; ++k) {
            if (k == 23) continue;
            if ((game::StateBit(game::StateKind(k)) & cure.curseMask) != 0) kind = game::StateKind(k);
        }
        if (!kind) return {Skip(1, "use", "its cure names no state")};
        Reset(t, proto::DevReset::kAll);
        const auto has = [&](const proto::EntityStates& s) {
            return s.entity == t.entity && std::any_of(s.states.begin(), s.states.end(), [&](const proto::StateRecord& r) { return r.kind == std::uint8_t(*kind); });
        };
        // The pill given and its kind's lock (a previous pill's) waited out first: a short state (a level-1 poison, 1 s) put on
        // after it is still on when the pill is used.
        const auto slot = Give(t, row);
        if (!slot) return {Verdict(1, "use", {"not given (DevGive)"}, "", {}, {})};
        const game::ReuseLock lock = game::ReuseLockOf(row, t.race == 1);
        t.WaitLock(lock.bit);
        std::size_t mark = t.c->Mark();
        proto::DevAfflict afflict;
        afflict.target = t.entity;
        afflict.state = std::uint8_t(*kind);
        afflict.level = std::uint16_t(level);
        afflict.ms = ms;
        afflict.value = 1;
        t.c->Send(afflict);
        if (!t.c->Wait<proto::EntityStates>(mark, has, 2s)) return {Skip(1, "use", std::format("the {} state could not be put on (DevAfflict)", game::StateName(*kind)))};
        mark = t.c->Mark();
        t.c->Send(proto::UseItem{*slot});
        if (lock.bit != 0) t.lockedUntil[lock.bit] = Clock::now() + std::chrono::milliseconds(lock.ms);
        const auto used = t.c->Wait<proto::ItemUsed>(mark, [&](const proto::ItemUsed& u) { return u.entity == t.entity && u.item == row.refObjId; }, 2s);
        const auto cleared = t.c->Wait<proto::EntityStates>(mark, [&](const proto::EntityStates& s) { return s.entity == t.entity && !has(s); }, 1500ms);
        t.Sync();
        std::optional<std::uint32_t> cooldown;
        for (auto& [at, m] : t.c->Since<proto::Cooldown>(mark)) {
            if (m.kind == proto::CooldownKind::Item && m.id == row.refObjId) cooldown = m.ms;
        }
        std::vector<std::string> failed;
        if (!used) failed.push_back("not used (no ItemUsed)");
        if (!cleared) failed.push_back(std::format("the {} state stayed", game::StateName(*kind)));
        if (lock.bit != 0 && cooldown != std::uint32_t(lock.ms)) failed.push_back(std::format("its cooldown {} ms, the lock is {} ms", cooldown ? int(*cooldown) : -1, lock.ms));
        if (t.Count(row.refObjId) != 0) failed.push_back("not taken");
        Result r = Verdict(1, "use", failed, std::format("cures {} (level {}, {} ms); its kind locked {} ms", game::StateName(*kind), level, ms, lock.ms),
                           {{"state", game::StateName(*kind)}, {"level", level}, {"ms", ms}, {"elementLevel", cure.elementLevel}, {"elementMask", cure.elementMask}, {"curseMask", cure.curseMask}, {"curseChance", cure.curseChance},
                            {"lockMs", lock.ms}},
                           {{"used", used.has_value()}, {"cured", cleared.has_value()}, {"cooldownMs", cooldown ? json(*cooldown) : json(nullptr)}});
        Reset(t, proto::DevReset::kAll);
        return {r};
    }

    // --- The return scrolls ------------------------------------------

    // A return read at the exam's place: its gauge's time, then the arrival. Each on a fresh player (it travels).
    struct Travel {
        std::optional<proto::ReturnStarted> started;
        std::optional<proto::WorldEntered> arrived;
        int ms = -1; // from the gauge's start to the arrival
        std::optional<std::uint16_t> refused;
    };
    Travel Read(Tester& t, std::uint8_t slot, std::uint32_t refId, std::uint8_t option = 0, std::uint32_t point = 0) {
        Travel out;
        slot = SlotOf(t, refId).value_or(slot);
        const std::size_t mark = t.c->Mark();
        t.c->Send(proto::UseItem{slot, option, point});
        out.started = t.c->Wait<proto::ReturnStarted>(mark, [&](const proto::ReturnStarted& s) { return s.item == refId; }, 2s);
        if (!out.started) {
            if (const auto r = RefusedSince(t, mark, refId)) out.refused = r->code;
            return out;
        }
        out.arrived = t.c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t.entity; }, std::chrono::milliseconds(out.started->ms + 8000));
        if (out.arrived) {
            Clock::time_point began{}, came{};
            for (auto& [at, m] : t.c->Since<proto::ReturnStarted>(mark)) began = at;
            for (auto& [at, m] : t.c->Since<proto::WorldEntered>(mark)) came = at;
            out.ms = Ms(came - began);
        }
        t.Sync();
        return out;
    }
    std::vector<std::string> CheckTravel(const Travel& tr, const assets::ItemRow& row, const proto::WorldPosition& to, json& observed) {
        std::vector<std::string> failed;
        const int gauge = std::max(100, row.params[0]);
        observed["gaugeMs"] = tr.started ? json(tr.started->ms) : json(nullptr);
        observed["tookMs"] = tr.ms;
        if (tr.refused) observed["refused"] = std::format("0x{:X}", *tr.refused);
        if (!tr.started) return {tr.refused ? std::format("refused with 0x{:X}", *tr.refused) : "no gauge (ReturnStarted)"};
        if (tr.started->ms != std::uint32_t(gauge)) failed.push_back(std::format("its gauge {} ms, its Param1 says {}", tr.started->ms, gauge));
        if (!tr.arrived) return (failed.push_back("never arrived"), failed);
        observed["arrived"] = Where(tr.arrived->position);
        if (tr.ms < gauge - 50 || tr.ms > gauge + 2500) failed.push_back(std::format("arrived {} ms after the gauge began ({} expected)", tr.ms, gauge));
        if (tr.arrived->position.region != to.region || Distance2d(tr.arrived->position, to) > kNear) {
            failed.push_back(std::format("arrived at {} ({}, {}), not {} ({}, {})", tr.arrived->position.region, std::lround(tr.arrived->position.x),
                                         std::lround(tr.arrived->position.z), to.region, std::lround(to.x), std::lround(to.z)));
        }
        return failed;
    }
    std::optional<proto::WorldPosition> TownOf(int race) const {
        const assets::TeleportPoint* p = data_.teleports->Point(race == 1 ? 25u : 1u); // GATE_EU, GATE_CH: no return point designated
        if (p == nullptr || !p->HasArrival()) return std::nullopt;
        return proto::WorldPosition{p->region, p->x, p->y, p->z};
    }

    std::vector<Result> Return(const assets::ItemRow& row, std::string& error) {
        auto t = Make(RaceFor(row), true, true, error);
        if (!t) return {};
        const auto town = TownOf(t->race);
        if (!town) return {Skip(1, "travel", "no town gate in teleportdata")};
        const auto slot = Give(*t, row, 2);
        if (!slot) return {Verdict(1, "travel", {"not given (DevGive)"}, "", {}, {})};
        // The second, read while the first's gauge runs: refused (nothing taken).
        const std::size_t mark = t->c->Mark();
        t->c->Send(proto::UseItem{*slot});
        const auto started = t->c->Wait<proto::ReturnStarted>(mark, [&](const proto::ReturnStarted& s) { return s.item == row.refObjId; }, 2s);
        std::size_t second = t->c->Mark();
        if (started) t->c->Send(proto::UseItem{SlotOf(*t, row.refObjId).value_or(*slot)});
        Travel tr;
        tr.started = started;
        if (!started) {
            if (const auto r = RefusedSince(*t, mark, row.refObjId)) tr.refused = r->code;
        } else {
            tr.arrived = t->c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t->entity; }, std::chrono::milliseconds(started->ms + 8000));
            if (tr.arrived) {
                Clock::time_point began{}, came{};
                for (auto& [at, m] : t->c->Since<proto::ReturnStarted>(mark)) {
                    if (began == Clock::time_point{}) began = at;
                }
                for (auto& [at, m] : t->c->Since<proto::WorldEntered>(mark)) came = at;
                tr.ms = Ms(came - began);
            }
        }
        t->Sync();
        json observed;
        std::vector<std::string> failed = CheckTravel(tr, row, *town, observed);
        observed["left"] = t->Count(row.refObjId);
        if (t->Count(row.refObjId) != 1) failed.push_back(std::format("{} left of 2", t->Count(row.refObjId)));
        std::vector<Result> out;
        out.push_back(Verdict(1, "travel", failed, std::format("to {} after {} ms", t->race == 1 ? "GATE_EU" : "GATE_CH", std::max(100, row.params[0])),
                              {{"to", Where(*town)}, {"gaugeMs", std::max(100, row.params[0])}}, observed));
        if (started) {
            const bool again = t->c->Since<proto::ReturnStarted>(second).size() > 0;
            out.push_back(Verdict(2, "again", again ? std::vector<std::string>{"a second gauge began while the first ran"} : std::vector<std::string>{},
                                  "a second read while the first's gauge runs: refused", {}, {{"secondGauge", again}}));
        }
        return out;
    }

    std::vector<Result> Reverse(const assets::ItemRow& row, std::string& error) {
        auto t = Make(RaceFor(row), true, true, error);
        if (!t) return {};
        std::vector<Result> out;
        const auto slot = Give(*t, row, 6);
        if (!slot) return {Verdict(1, "recall", {"not given (DevGive)"}, "", {}, {})};
        // The refusals at the start: no option, no recall point yet, no place of death yet.
        for (const auto& [variant, option, code] : {std::tuple<const char*, std::uint8_t, std::uint16_t>{"no option", 0, 2}, {"no recall point", proto::kReverseToRecall, 0x1885},
                                                   {"no place of death", proto::kReverseToDeath, 0x1886}}) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::UseItem{*slot, option, 0});
            const auto refused = t->c->Wait<proto::ItemRefused>(mark, [&](const proto::ItemRefused& r) { return r.item == row.refObjId; }, 1500ms);
            const bool gauge = !t->c->Since<proto::ReturnStarted>(mark).empty();
            std::vector<std::string> failed;
            if (gauge) failed.push_back("its gauge began");
            if (!refused) failed.push_back("no refusal");
            else if (refused->code != code) failed.push_back(std::format("refused with 0x{:X}, 0x{:X} expected", refused->code, code));
            out.push_back(Verdict(2, variant, failed, std::format("refused with 0x{:X}", code), {{"code", std::format("0x{:X}", code)}},
                                  {{"code", refused ? json(std::format("0x{:X}", refused->code)) : json(nullptr)}, {"gauge", gauge}}));
        }
        // The recall point: a return scroll read here, the arrival in town, then back here.
        const game::ItemCatalogueEntry* scroll = nullptr;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            if (e.routing.route == ItemRoute::ReturnScroll && e.row->params[0] <= 10000) {
                scroll = &e;
                break;
            }
        }
        const auto town = TownOf(t->race);
        if (scroll == nullptr || !town) {
            out.push_back(Skip(1, "recall", "no return scroll or town gate to make a recall point with"));
        } else if (const auto s = Give(*t, *scroll->row); !s) {
            out.push_back(Verdict(1, "recall", {"the return scroll not given (DevGive)"}, "", {}, {}));
        } else {
            const Travel home = Read(*t, *s, scroll->row->refObjId);
            if (!home.arrived) {
                out.push_back(Verdict(1, "recall", {"the return scroll did not take it to town"}, "", {}, {}));
            } else {
                const Travel back = Read(*t, *slot, row.refObjId, proto::kReverseToRecall);
                json observed;
                const std::vector<std::string> failed = CheckTravel(back, row, kPlace, observed);
                out.push_back(Verdict(1, "recall", failed, "back to where the last return scroll was read", {{"to", Where(kPlace)}, {"gaugeMs", std::max(100, row.params[0])}},
                                      observed));
            }
        }
        // The place of death: where it stands now (its last arrival: the exam's place again, or town when the recall
        // failed), risen in town, back there.
        {
            const auto arrivals = t->c->Since<proto::WorldEntered>(0);
            const std::optional<proto::WorldPosition> died = arrivals.empty() ? std::nullopt : std::optional(arrivals.back().second.position);
            t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll & ~proto::DevReset::kBag)});
            t->c->Pump(40ms);
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::DevDie{});
            t->c->Pump(300ms);
            t->c->Send(proto::Resurrect{1});
            const auto risen = t->c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t->entity; }, 8s);
            t->Sync();
            if (!risen) {
                out.push_back(Skip(1, "death", "it did not rise in town (DevDie, Resurrect 1)"));
            } else {
                const Travel back = Read(*t, *slot, row.refObjId, proto::kReverseToDeath);
                json observed;
                const std::vector<std::string> failed = died ? CheckTravel(back, row, *died, observed) : std::vector<std::string>{"its place of death unknown"};
                out.push_back(Verdict(1, "death", failed, "back to where it died", {{"to", died ? Where(*died) : json(nullptr)}}, observed));
            }
        }
        // A map point (_RefOptionalTeleport) for its level.
        const OptionalTeleportRow* point = nullptr;
        for (const OptionalTeleportRow& p : points_) {
            if ((p.levelMin == 0 && p.levelMax == 0) || (kHighLevel >= p.levelMin && kHighLevel <= p.levelMax)) {
                point = &p;
                break;
            }
        }
        if (point == nullptr) {
            out.push_back(Skip(1, "point", "no map point for its level in the exam's database"));
        } else {
            const proto::WorldPosition to{point->region, point->x, point->y, point->z};
            const Travel go = Read(*t, *slot, row.refObjId, proto::kReverseToPoint, point->id);
            json observed;
            const std::vector<std::string> failed = CheckTravel(go, row, to, observed);
            out.push_back(Verdict(1, "point", failed, std::format("to map point {}", point->id), {{"point", point->id}, {"to", Where(to)}}, observed));
        }
        return out;
    }

    // A thief (the league's, its suit worn) reads it: the gauge of its Param1, then the point of the building its Param4
    // names (ITEM_ETC_SCROLL_RETURN_THIEFDEN_01: STORE_TD_GATE, its point GATE_TD).
    Result ThiefReads(const assets::ItemRow& row, std::string& error) {
        const int race = RaceFor(row) == 1 ? 1 : 0;
        const assets::TeleportPoint* to = nullptr;
        for (const assets::TeleportGate& g : data_.teleports->Gates()) {
            if (g.codename == row.paramTexts[3]) to = data_.teleports->PointOf(g.refObjId);
        }
        if (to == nullptr) return Skip(1, "thief", std::format("no teleport point for its building {}", row.paramTexts[3]));
        const assets::ItemRow* suit = data_.items->Find(std::string_view(race == 0 ? "ITEM_CH_M_TRADE_THIEF_02" : "ITEM_EU_M_TRADE_THIEF_01"));
        if (suit == nullptr) return Skip(1, "thief", "no thief suit in the data");
        Tester* t = Get(race, true, kThief, error);
        if (t == nullptr) return Skip(1, "thief", "no player");
        Reset(*t, proto::DevReset::kAll);
        // A league's suit goes on only in a town (0x18A2): to its race's town gate (GATE_CH 1, GATE_EU 20).
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DevGoto{race == 0 ? 1u : 20u});
        if (!t->c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t->entity; }, 8s)) {
            return Skip(1, "thief", "the town gate not reached (DevGoto)");
        }
        t->c->Pump(100ms);
        // On after its change (the exam's 0.2 s; the original's 10 s): the move's answer then.
        mark = t->c->Mark();
        t->c->Send(proto::DevGive{suit->codename, 1, true});
        const auto worn = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) {
            return m.to == game::slot::kJobSuit ? m.status != proto::ItemStatus::JobChange : m.from >= game::slot::kWorn && m.status != proto::ItemStatus::Ok;
        }, 4s);
        if (!worn || worn->status != proto::ItemStatus::Ok || worn->to != game::slot::kJobSuit) {
            Reset(*t, proto::DevReset::kAll);
            return Verdict(1, "thief", {std::format("its thief suit not worn ({}, 0x{:X})", worn ? int(worn->status) : -1, worn ? worn->code : 0)}, "", {}, {});
        }
        t->c->Pump(100ms);
        const auto slot = Give(*t, row);
        if (!slot) return Verdict(1, "thief", {"not given (DevGive)"}, "", {}, {});
        const Travel tr = Read(*t, *slot, row.refObjId);
        json observed = json::object();
        std::vector<std::string> failed = CheckTravel(tr, row, {to->region, to->x, to->y, to->z}, observed);
        if (tr.started && t->Count(row.refObjId) != 0) failed.push_back("not taken");
        // Back: the suit off, home.
        mark = t->c->Mark();
        t->c->Send(proto::ItemMove{std::uint8_t(game::slot::kJobSuit), std::uint8_t(game::slot::kWorn), 0});
        t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from == game::slot::kJobSuit; }, 2s);
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "thief", failed, std::format("a thief in its suit reads it: to {} (the point of {})", to->codename, row.paramTexts[3]),
                       {{"to", to->codename}, {"gaugeMs", std::max(100, row.params[0])}}, observed);
    }

    std::vector<Result> ThiefDen(const assets::ItemRow& row, std::string& error) {
        Tester* t = Get(RaceFor(row), true, true, error);
        if (t == nullptr) return {};
        Reset(*t, proto::DevReset::kAll);
        const auto slot = Give(*t, row);
        if (!slot) return {Verdict(2, "not a thief", {"not given (DevGive)"}, "", {}, {})};
        const std::size_t mark = t->c->Mark();
        t->c->Send(proto::UseItem{*slot});
        const auto refused = t->c->Wait<proto::ItemRefused>(mark, [&](const proto::ItemRefused& r) { return r.item == row.refObjId; }, 1500ms);
        std::vector<std::string> failed;
        if (!refused) failed.push_back("no refusal");
        else if (refused->code != 0x186A) failed.push_back(std::format("refused with 0x{:X}, 0x186A expected", refused->code));
        if (!t->c->Since<proto::ReturnStarted>(mark).empty()) failed.push_back("its gauge began");
        Reset(*t, proto::DevReset::kBag);
        return {Verdict(2, "not a thief", failed, "refused to one who is not a thief (0x186A)", {{"code", "0x186A"}},
                        {{"code", refused ? json(std::format("0x{:X}", refused->code)) : json(nullptr)}}),
                ThiefReads(row, error)};
    }

    // --- The scrolls that cast a buff ----------------------------------------------

    std::vector<Result> Buff(const assets::ItemRow& row, std::string& error) {
        Tester* t = Get(RaceFor(row), MaleFor(row), true, error);
        if (t == nullptr) return {};
        const assets::SkillRow* skill = data_.skills->Find(std::string_view(row.paramTexts[0]));
        if (skill == nullptr) return {Verdict(1, "buff", {"its Param1 names no skill: " + row.paramTexts[0]}, "", {}, {})};
        Reset(*t, proto::DevReset::kAll);
        const auto slot = Give(*t, row, 2);
        if (!slot) return {Verdict(1, "buff", {"not given (DevGive)"}, "", {}, {})};
        const std::uint32_t want = skill->hasDuration ? skill->buffDurationMs : 3'600'000u;
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::UseItem{*slot});
        const auto added = t->c->Wait<proto::BuffAdded>(mark, [&](const proto::BuffAdded& b) { return b.skill == skill->id; }, 2s);
        const auto used = t->c->Wait<proto::ItemUsed>(mark, [&](const proto::ItemUsed& u) { return u.entity == t->entity && u.item == row.refObjId; }, 300ms);
        t->Sync();
        std::vector<std::string> failed;
        json observed = {{"used", used.has_value()}, {"ms", added ? json(added->ms) : json(nullptr)}};
        // A quest's trap (its skill's 'trap' with 'qest'): no buff, the skill stands as an object where it was read
        // (Skills: a skill object, EntityAppeared kind SkillObject; the captures).
        std::uint32_t standing = 0;
        if (!added && (assets::FindSkillParam(*skill, assets::SkillTag("trap")) || assets::FindSkillParam(*skill, assets::SkillTag("efr")))) {
            for (const auto& [at, e] : t->c->Since<proto::EntityAppeared>(mark)) {
                if (e.kind == proto::EntityKind::SkillObject) standing = e.entity;
            }
            observed["object"] = standing;
        }
        if (standing != 0) {
            if (!used) failed.push_back("no ItemUsed");
            if (t->Count(row.refObjId) != 1) failed.push_back(std::format("{} left of 2", t->Count(row.refObjId)));
            Reset(*t, proto::DevReset::kAll);
            return {Verdict(1, "buff", failed, std::format("{} stands where it was read (a quest's trap: a skill object, no buff)", skill->codename),
                            {{"skill", skill->codename}}, observed)};
        }
        if (!added) {
            failed.push_back("no buff");
            if (const auto r = RefusedSince(*t, mark, row.refObjId)) failed.back() += std::format(" (refused with 0x{:X})", r->code);
        } else if (std::abs(int(added->ms) - int(want)) > 1000) {
            failed.push_back(std::format("{} ms of it, {} expected", added->ms, want));
        }
        if (!used) failed.push_back("no ItemUsed");
        if (t->Count(row.refObjId) != 1) failed.push_back(std::format("{} left of 2", t->Count(row.refObjId)));
        std::vector<Result> out;
        out.push_back(Verdict(1, "buff", failed, std::format("{} for {} s", skill->codename, want / 1000), {{"skill", skill->codename}, {"ms", want}}, observed));
        if (added) {
            // A second starts its time again (the same buff, its token kept).
            t->c->Pump(1200ms);
            mark = t->c->Mark();
            t->c->Send(proto::UseItem{SlotOf(*t, row.refObjId).value_or(*slot)});
            const auto again = t->c->Wait<proto::BuffAdded>(mark, [&](const proto::BuffAdded& b) { return b.skill == skill->id; }, 2s);
            std::vector<std::string> f;
            if (!again) f.push_back("no buff from the second");
            else if (std::abs(int(again->ms) - int(want)) > 1000) f.push_back(std::format("the second's {} ms, {} expected", again->ms, want));
            out.push_back(Verdict(1, "again", f, "a second starts its time again", {{"ms", want}}, {{"ms", again ? json(again->ms) : json(nullptr)}, {"sameToken", again && again->token == added->token}}));
        }
        Reset(*t, proto::DevReset::kAll);
        return out;
    }

    // --- The equipment -------------------------------------------------------

    // The item given and worn by the move's rules (DevGive's wear): the move's answer; then taken off again. `weapon`:
    // worn first (ammunition's bow or crossbow), and taken off after it.
    std::optional<proto::ItemMoved> Wear(Tester& t, const assets::ItemRow& row, int place, const assets::ItemRow* weapon = nullptr) {
        Reset(t, proto::DevReset::kBag);
        const auto on = [&](const assets::ItemRow& r) {
            const std::size_t mark = t.c->Mark();
            t.c->Send(proto::DevGive{r.codename, 1, true});
            const auto moved = t.c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from >= game::slot::kWorn; }, 3s);
            t.Sync();
            return moved;
        };
        const auto off = [&](int from) {
            std::uint8_t free = game::slot::kWorn;
            while (t.places.contains(free)) ++free;
            const std::size_t mark = t.c->Mark();
            t.c->Send(proto::ItemMove{std::uint8_t(from), free, 0});
            t.c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == from; }, 2s);
            t.Sync();
        };
        bool armed = false;
        if (weapon != nullptr) {
            const auto w = on(*weapon);
            armed = w && w->status == proto::ItemStatus::Ok;
        }
        const auto moved = on(row);
        if (moved && moved->status == proto::ItemStatus::Ok) off(place);
        if (armed) off(game::slot::kWeapon);
        Reset(t, proto::DevReset::kBag);
        return moved;
    }
    // A bow or crossbow the ammunition goes beside (of its country when it has one, either sex, within the level cap):
    // the ammunition is tried on that weapon's country's player (bolts are both countries', crossbows Europe's).
    const assets::ItemRow* WeaponFor(const assets::ItemRow& ammo) const {
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            const assets::ItemRow& w = *e.row;
            if (e.routing.route == ItemRoute::Equip && game::AmmunitionFor(w) == ammo.typeId[3] && w.sex == 2 && LevelOf(w) <= kHighLevel &&
                (ammo.country == 3 || w.country == ammo.country)) {
                return &w;
            }
        }
        return nullptr;
    }

    std::vector<Result> Equip(const assets::ItemRow& row, std::string& error) {
        const int place = game::EquipPlaceFor(row, false, false);
        if (place < 0) return {Verdict(1, "wear", {"no place for it (EquipPlaceFor)"}, "", {}, {})};
        const int level = LevelOf(row);
        std::vector<Result> out;
        // Ammunition goes beside its bow or crossbow (ours: Inventory's FitsBeside; the route's rule).
        const bool ammo = game::IsAmmunition(row);
        const assets::ItemRow* weapon = ammo ? WeaponFor(row) : nullptr;
        if (ammo && weapon == nullptr) return {Skip(1, "wear", "no bow or crossbow for it to go beside")};
        // A fortress weapon (TID4 16) is worn only by a guild member of a fortress position its Param5 takes (0x18E0,
        // checked before the rest: ApplyMove): the exam's players have none.
        const bool fortress = game::KindOf(row) == game::ItemKind::Weapon && row.typeId[3] == 16;
        // A European armour's mastery pairs: checked before the country.
        bool masteryPairs = false, cleric = false;
        for (const int type : row.requirementTypes) {
            masteryPairs = masteryPairs || (type >= 0x201 && type <= 0x206);
            cleric = cleric || type == int(kCleric);
        }
        const auto check = [&](int scenario, const char* variant, int race, bool male, Profile profile, proto::ItemStatus want, std::string text, bool armed = true) {
            Tester* t = Get(race, male, profile, error);
            if (t == nullptr) return;
            const auto moved = Wear(*t, row, place, armed ? weapon : nullptr);
            std::vector<std::string> failed;
            json observed = {{"status", moved ? json(int(moved->status)) : json(nullptr)}, {"code", moved ? json(moved->code) : json(nullptr)}};
            if (!moved) failed.push_back("no answer to the move");
            else if (moved->status != want) failed.push_back(std::format("its move answered {}, {} expected", int(moved->status), int(want)));
            else if (want == proto::ItemStatus::Ok && moved->to != place) failed.push_back(std::format("worn in place {}, {} expected", moved->to, place));
            out.push_back(Verdict(scenario, variant, failed, std::move(text),
                                  {{"status", int(want)}, {"place", place}, {"weapon", armed && weapon ? json(weapon->codename) : json(nullptr)}}, observed));
        };
        const int race = weapon != nullptr ? RaceFor(*weapon) : RaceFor(row);
        const bool male = MaleFor(row);
        if (fortress) {
            // Worn by a member whose fortress position is in its Param5 mask (35: Commander, Deputy, Military Engineer):
            // the guild's master, the Commander.
            check(1, "wear", race, male, kFortress, proto::ItemStatus::Ok, std::format("worn by a guild's Commander (position 1 in its mask {})", row.params[4]));
            check(2, "no fortress position", race, male, kHigh, proto::ItemStatus::FortressPosition, "refused with no fortress position (0x18E0)");
            // Its position is checked after the wear rules: below its level the level answers first.
            if (level > 1) check(2, "level", race, male, kLow, proto::ItemStatus::Level, std::format("refused at level 1 (it asks {}) before its position", level));
            return out;
        }
        if (level > kHighLevel) {
            out.push_back(Skip(1, "wear", std::format("it asks level {}, above the cap {}", level, kHighLevel)));
        } else {
            check(1, "wear", race, male, kHigh, proto::ItemStatus::Ok, std::format("worn in place {} at level {}", place, kHighLevel));
        }
        if (level > 1) check(2, "level", race, male, kLow, proto::ItemStatus::Level, std::format("refused at level 1 (it asks {})", level));
        if (row.country == 0 || row.country == 1) {
            // A Chinese has none of the European masteries: their pairs refuse it before the country (0x186C, not 0x182F).
            if (masteryPairs && row.country == 1) {
                check(2, "race", 0, male, kHigh, proto::ItemStatus::Mastery, "refused to a Chinese by its mastery requirement, checked before the country (0x186C)");
            } else {
                check(2, "race", 1 - row.country, male, kHigh, proto::ItemStatus::Race, std::format("refused to {}", row.country == 0 ? "a European" : "a Chinese"));
            }
        }
        if (row.sex == 0 || row.sex == 1) check(2, "sex", race, !male, kHigh, proto::ItemStatus::Sex, std::format("refused to a {}", male ? "woman" : "man"));
        // A European of the level but of no mastery it names: the robes ask the casters' (a warrior alone refused), the
        // light and heavy armour the warrior's or others (a cleric alone refused).
        if (masteryPairs && level <= kHighLevel) {
            check(2, "mastery", 1, male, cleric ? kWarriorOnly : kClericOnly, proto::ItemStatus::Mastery,
                  std::format("refused to a European of the {} mastery alone (0x186C)", cleric ? "warrior's" : "cleric's"));
        }
        if (ammo) check(2, "no weapon", race, male, kHigh, proto::ItemStatus::WrongSlot, "refused with no bow or crossbow worn", false);
        // An accessory's blues (STR, INT):
        // given on it, worn, the player's numbers before and after.
        if ((row.typeId[2] == 5 || row.typeId[2] == 12) && level <= kHighLevel) out.push_back(Blues(row, place, race, male, error));
        // An accessory set's earring (the plain one of each set and country): the set's four pieces worn one by one.
        if (row.typeId[2] == 5 || row.typeId[2] == 12) {
            const std::string& c = row.codename;
            for (const char* letter : {"A", "B"}) {
                if (c == std::format("ITEM_{}_EARRING_11_SET_{}", row.typeId[2] == 5 ? "CH" : "EU", letter)) out.push_back(SetCheck(row, letter, error));
            }
        }
        // A European piece whose masteries were lowered after it went on: on the first chest
        // of each sex and family whose mastery requirements are all above 1.
        if (IsMasteryTrigger(row)) out.push_back(MasteryLowered(row, error));
        // A shield's skill (game::EquipmentFits' reqi 4): on the first degree-1 shield of each country.
        if (IsShieldSkillTrigger(row)) out.push_back(ShieldSkill(row, error));
        // The complete armour set: on the first degree-1 chest of each country, sex and family.
        if (IsSetTrigger(row)) out.push_back(CompleteSet(row, error));
        // A harp: its bearer's most mana half again (param 4, percent 50).
        if (row.typeId == std::array<int, 4>{3, 1, 6, 14} && level <= kHighLevel) out.push_back(Harp(row, place, race, male, error));
        return out;
    }

    // The accessory set: earring, necklace, the left ring, the right ring worn in turn, the
    // player's numbers after each (DevSheet's setPlusN: the raise of the piece in worn place N), then the right ring off.
    // The group of the most pieces at or under the count: set A +1 at 2, +2 at 4 (B +2, +4); a piece added that does not
    // change the group gets nothing (the 3rd); taking one off with a change lowers the others.
    Result SetCheck(const assets::ItemRow& earring, std::string_view letter, std::string& error) {
        const std::string country = earring.typeId[2] == 5 ? "CH" : "EU";
        const int race = earring.typeId[2] == 5 ? 0 : 1;
        Tester* t = Get(race, true, true, error);
        if (t == nullptr) return Skip(1, "set", "no player");
        Reset(*t, proto::DevReset::kBag);
        const int step = letter == "A" ? 1 : 2;
        struct Piece {
            const char* kind;
            int place;
        };
        constexpr Piece kPieces[] = {{"EARRING", game::slot::kEarring}, {"NECKLACE", game::slot::kNecklace}, {"RING", game::slot::kRingLeft}, {"RING", game::slot::kRingRight}};
        const auto raises = [&]() {
            std::map<int, int> out;
            for (const auto& [name, value] : Sheet(*t)) {
                if (name.rfind("setPlus", 0) == 0) out[std::atoi(name.c_str() + 7)] = int(value);
            }
            return out;
        };
        // What each count should leave: the raise of every worn place (absent: none).
        const std::map<int, int> none;
        const std::map<int, int> expected[] = {none,
                                               {{9, step}, {10, step}},
                                               {{9, step}, {10, step}},
                                               {{9, 2 * step}, {10, 2 * step}, {11, 2 * step}, {12, 2 * step}}};
        std::vector<std::string> failed;
        json seen = json::array();
        for (int k = 0; k < 4; ++k) {
            const std::string code = std::format("ITEM_{}_{}_11_SET_{}", country, kPieces[k].kind, letter);
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::DevGive{code, 1, true});
            const auto moved = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from >= game::slot::kWorn; }, 3s);
            t->c->Pump(60ms);
            if (!moved || moved->status != proto::ItemStatus::Ok || moved->to != kPieces[k].place) {
                failed.push_back(std::format("{} not worn in place {}", code, kPieces[k].place));
                break;
            }
            const std::map<int, int> got = raises();
            seen.push_back({{"pieces", k + 1}, {"raises", got}});
            if (got != expected[k]) failed.push_back(std::format("at {} pieces the raises are not the group's", k + 1));
        }
        if (failed.empty()) {
            // The right ring off: the group of 2 again (the three left raised by its step).
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(game::slot::kRingRight), std::uint8_t(game::slot::kWorn), 0});
            t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from == game::slot::kRingRight; }, 2s);
            t->c->Pump(60ms);
            const std::map<int, int> got = raises();
            seen.push_back({{"pieces", 3}, {"after", "the right ring off"}, {"raises", got}});
            const std::map<int, int> want = {{9, step}, {10, step}, {11, step}};
            if (got != want) failed.push_back("with the right ring off the three left are not raised by the group of 2");
        }
        // Everything off and away.
        for (const Piece& p : kPieces) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(p.place), std::uint8_t(game::slot::kWorn), 0});
            t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == p.place; }, 1s);
            Reset(*t, proto::DevReset::kBag);
        }
        return Verdict(1, std::format("set {}", letter), failed,
                       std::format("set {}: +{} at 2 pieces, +{} at 4, none for the 3rd alone; the right ring off: back to +{}", letter, step, 2 * step, step),
                       {{"step", step}}, {{"raises", seen}});
    }

    // The Devil's Spirit's power: awake at +3 with an UMDU blue of 6, worn: its by-plus options
    // (HP from STR, MP from INT, the block) and skills, the damage on uniques; taken off: all of it gone. The expected from
    // the tables themselves (game::SpiritGiftOf): a spirit with no row for its plus (the events' 24971..24978) gives nothing.
    Result SpiritPower(const assets::ItemRow& row, int race, bool male, std::uint32_t awakeUntil, std::string& error) {
        Tester* t = Get(race, male, true, error);
        if (t == nullptr) return Skip(1, "power", "no player");
        const assets::MagicOptionRow* umdu = OptionFor("MATTR_NASRUN_UMDU", 1);
        if (umdu == nullptr) return Skip(1, "power", "no MATTR_NASRUN_UMDU row");
        constexpr int kPlus = 3, kUmdu = 6;
        game::ItemInstance item;
        item.refId = row.refObjId;
        item.plus = kPlus;
        item.durability = awakeUntil;
        item.options.push_back({umdu->paramId, std::uint32_t(kUmdu)});
        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const game::SpiritGift gift = game::SpiritGiftOf(*data_.spirits, row, item, &data_.lab->options, now);
        float hpna = 0, mpna = 0, block = 0;
        for (const game::StatChange& c : gift.changes) {
            if (c.stat == game::stat::kHpFromStr && c.kind == 1) hpna += c.value;
            if (c.stat == game::stat::kMpFromInt && c.kind == 1) mpna += c.value;
            if (c.stat == game::stat::kBlock && c.kind == 0) block += c.value;
        }
        Reset(*t, proto::DevReset::kBag);
        const std::map<std::string, float> before = Sheet(*t);
        // Given at +3 with its blue and awake, then put on (DevGive's wear is not an avatar's).
        std::size_t mark = t->c->Mark();
        proto::DevGive give{row.codename, 1, false};
        give.magic = {std::uint64_t(umdu->paramId) | (std::uint64_t(kUmdu) << 32)};
        give.plus = kPlus;
        give.data = awakeUntil;
        t->c->Send(give);
        const auto given = t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) {
            return std::any_of(m.changed.begin(), m.changed.end(), [&](const proto::ItemRecord& r) { return r.item == row.refObjId && r.plus == kPlus; });
        }, 3s);
        std::optional<std::uint8_t> slot;
        if (given) {
            for (const proto::ItemRecord& r : given->changed) {
                if (r.item == row.refObjId) slot = r.slot;
            }
        }
        if (!slot) return Verdict(1, "power", {"not given (DevGive)"}, "", {}, {});
        mark = t->c->Mark();
        t->c->Send(proto::AvatarMove{true, *slot});
        const auto worn = t->c->Next<proto::AvatarMoved>(mark, 2s);
        t->c->Pump(80ms);
        const std::map<std::string, float> on = Sheet(*t);
        mark = t->c->Mark();
        t->c->Send(proto::AvatarMove{false, std::uint8_t(game::avatar::kSpirit)});
        t->c->Next<proto::AvatarMoved>(mark, 2s);
        t->c->Pump(80ms);
        const std::map<std::string, float> off = Sheet(*t);
        Reset(*t, proto::DevReset::kBag);
        const auto at = [](const std::map<std::string, float>& m, const std::string& k) {
            const auto it = m.find(k);
            return it != m.end() ? double(it->second) : 0.0;
        };
        std::vector<std::string> failed;
        if (!worn || worn->status != proto::ItemStatus::Ok) failed.push_back("not worn");
        const double str = std::trunc(at(before, "strength")), intel = std::trunc(at(before, "intelligence"));
        const double wantHp = gift.found ? double(game::PlayerVitalFrom(kHighLevel, double(float(str * (100.0 + hpna) / 100.0)))) : at(before, "maxHp");
        const double wantMp = gift.found ? double(game::PlayerVitalFrom(kHighLevel, double(float(intel * (100.0 + mpna) / 100.0)))) : at(before, "maxMp");
        if (std::abs(at(on, "maxHp") - wantHp) > 1.0) failed.push_back(std::format("most HP {:.0f}, {:.0f} expected (HPNA {})", at(on, "maxHp"), wantHp, hpna));
        if (std::abs(at(on, "maxMp") - wantMp) > 1.0) failed.push_back(std::format("most MP {:.0f}, {:.0f} expected (MPNA {})", at(on, "maxMp"), wantMp, mpna));
        if (std::abs(at(on, "block") - at(before, "block") - (gift.found ? block : 0.0)) > 0.01) failed.push_back(std::format("block +{:.0f}, +{} expected", at(on, "block") - at(before, "block"), block));
        if (std::abs(at(on, "uniqueDamage") - (gift.found ? kUmdu : 0)) > 0.01) failed.push_back(std::format("damage on uniques {:.0f}, {} expected", at(on, "uniqueDamage"), gift.found ? kUmdu : 0));
        if (int(at(on, "spiritOn")) != (gift.found ? 1 : 0)) failed.push_back("its contributions' switch (spiritOn) not as its row says");
        for (const std::uint32_t skill : gift.skills) {
            if (at(on, std::format("itemSkill{}", skill)) != 1.0) failed.push_back(std::format("skill {} not given", skill));
        }
        // Taken off: everything as before.
        for (const char* k : {"maxHp", "maxMp", "block", "uniqueDamage", "spiritOn"}) {
            if (std::abs(at(off, k) - at(before, k)) > 0.01) failed.push_back(std::format("{} not back after taking it off", k));
        }
        for (const std::uint32_t skill : gift.skills) {
            if (at(off, std::format("itemSkill{}", skill)) != 0.0) failed.push_back(std::format("skill {} kept after taking it off", skill));
        }
        json skills = json::array();
        for (const std::uint32_t skill : gift.skills) skills.push_back(skill);
        return Verdict(1, "power",
                       failed, gift.found ? std::format("+{}: HP from STR +{}%, MP from INT +{}%, block +{}, {} skills, damage on uniques +{}%", kPlus, hpna, mpna, block, gift.skills.size(), kUmdu)
                                          : std::format("+{}: no row for it (nothing given), as the original", kPlus),
                       {{"found", gift.found}, {"hpna", hpna}, {"mpna", mpna}, {"block", block}, {"skills", skills}, {"uniqueDamage", gift.found ? kUmdu : 0}},
                       {{"maxHp", {at(before, "maxHp"), at(on, "maxHp"), at(off, "maxHp")}}, {"maxMp", {at(before, "maxMp"), at(on, "maxMp"), at(off, "maxMp")}},
                        {"block", {at(before, "block"), at(on, "block")}}, {"uniqueDamage", at(on, "uniqueDamage")}, {"spiritOn", at(on, "spiritOn")}});
    }

    // A degree-1 plain piece of the family for each armour place: the first in the catalogue (country, sex, TID3).
    std::array<const assets::ItemRow*, 6> FamilyPieces(int country, int sex, int family) const {
        std::array<const assets::ItemRow*, 6> out{};
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            const assets::ItemRow& r = *e.row;
            if (e.routing.route != ItemRoute::Equip || r.typeId[2] != family || r.country != country || r.sex != sex || r.itemClass != 1 || r.Rare()) continue;
            const int place = game::EquipPlaceFor(r, false, false);
            if (place >= game::slot::kHead && place <= game::slot::kFeet && out[std::size_t(place)] == nullptr) out[std::size_t(place)] = &r;
        }
        return out;
    }
    bool IsSetTrigger(const assets::ItemRow& row) const {
        if (game::KindOf(row) != game::ItemKind::Armour || row.itemClass != 1 || row.Rare() || game::EquipPlaceFor(row, false, false) != game::slot::kChest) return false;
        return FamilyPieces(row.country, row.sex, row.typeId[2])[std::size_t(game::slot::kChest)] == &row;
    }
    Result CompleteSet(const assets::ItemRow& chest, std::string& error) {
        const int family = chest.typeId[2];
        const std::array<const assets::ItemRow*, 6> pieces = FamilyPieces(chest.country, chest.sex, family);
        if (std::any_of(pieces.begin(), pieces.end(), [](const assets::ItemRow* r) { return r == nullptr; })) return Skip(1, "complete set", "a place with no piece of its family");
        Tester* t = Get(RaceFor(chest), MaleFor(chest), true, error);
        if (t == nullptr) return Skip(1, "complete set", "no player");
        Reset(*t, proto::DevReset::kBag);
        const double want = family == 1 || family == 9 ? 80 : family == 2 || family == 10 ? 90 : 100;
        std::vector<std::string> failed;
        json seen = json::array();
        for (int place = game::slot::kHead; place <= game::slot::kFeet; ++place) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::DevGive{pieces[std::size_t(place)]->codename, 1, true});
            const auto moved = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from >= game::slot::kWorn; }, 3s);
            if (!moved || moved->status != proto::ItemStatus::Ok) failed.push_back(std::format("{} not worn", pieces[std::size_t(place)]->codename));
            t->c->Pump(40ms);
            const auto sheet = Sheet(*t);
            const double cost = sheet.contains("mpCostPercent") ? sheet.at("mpCostPercent") : -1;
            seen.push_back({{"pieces", place + 1}, {"mpCostPercent", cost}});
            if (place < game::slot::kFeet && std::abs(cost - 100) > 0.01) failed.push_back(std::format("the MP cost {} at {} pieces", cost, place + 1));
            if (place == game::slot::kFeet && std::abs(cost - want) > 0.01) failed.push_back(std::format("the MP cost {} with all six, {} expected", cost, want));
        }
        // One piece off: no set.
        const std::size_t mark = t->c->Mark();
        t->c->Send(proto::ItemMove{std::uint8_t(game::slot::kHead), std::uint8_t(game::slot::kWorn), 0});
        t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from == game::slot::kHead; }, 2s);
        t->c->Pump(40ms);
        const auto sheet = Sheet(*t);
        const double after = sheet.contains("mpCostPercent") ? sheet.at("mpCostPercent") : -1;
        if (std::abs(after - 100) > 0.01) failed.push_back(std::format("the MP cost {} with the head off", after));
        for (int place = game::slot::kChest; place <= game::slot::kFeet; ++place) {
            const std::size_t m = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(place), std::uint8_t(game::slot::kWorn + 1), 0});
            t->c->Wait<proto::ItemMoved>(m, [&](const proto::ItemMoved& x) { return x.from == place; }, 2s);
            Reset(*t, proto::DevReset::kBag);
        }
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "complete set", failed, std::format("family {}: the MP cost {} % with all six pieces, 100 % else", family, want), {{"family", family}, {"mpCostPercent", want}},
                       {{"steps", seen}, {"headOff", after}});
    }

    // A dress's blues: AVATAR_STR 3 and AVATAR_STR_2 2 ('stra' both: summed) and
    // AVATAR_INT 4, worn: STR +5, INT +4; taken off: back.
    Result AvatarBlues(const assets::ItemRow& row, int race, bool male, std::string& error) {
        const assets::MagicOptionRow* str = OptionFor("MATTR_AVATAR_STR", 1);
        const assets::MagicOptionRow* str2 = OptionFor("MATTR_AVATAR_STR_2", 1);
        const assets::MagicOptionRow* intel = OptionFor("MATTR_AVATAR_INT", 1);
        if (str == nullptr || str2 == nullptr || intel == nullptr) return Skip(1, "blues", "no avatar STR / INT rows");
        Tester* t = Get(race, male, true, error);
        if (t == nullptr) return Skip(1, "blues", "no player");
        Reset(*t, proto::DevReset::kBag);
        const std::map<std::string, float> before = Sheet(*t);
        std::size_t mark = t->c->Mark();
        proto::DevGive give{row.codename, 1, false};
        give.magic = {std::uint64_t(str->paramId) | (3ull << 32), std::uint64_t(str2->paramId) | (2ull << 32), std::uint64_t(intel->paramId) | (4ull << 32)};
        t->c->Send(give);
        const auto given = t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) {
            return std::any_of(m.changed.begin(), m.changed.end(), [&](const proto::ItemRecord& r) { return r.item == row.refObjId && !r.magic.empty(); });
        }, 3s);
        std::optional<std::uint8_t> slot;
        if (given) {
            for (const proto::ItemRecord& r : given->changed) {
                if (r.item == row.refObjId) slot = r.slot;
            }
        }
        if (!slot) return Verdict(1, "blues", {"not given (DevGive)"}, "", {}, {});
        mark = t->c->Mark();
        t->c->Send(proto::AvatarMove{true, *slot});
        const auto worn = t->c->Next<proto::AvatarMoved>(mark, 2s);
        t->c->Pump(60ms);
        const std::map<std::string, float> on = Sheet(*t);
        mark = t->c->Mark();
        t->c->Send(proto::AvatarMove{false, std::uint8_t(game::avatar::kDress)});
        t->c->Next<proto::AvatarMoved>(mark, 2s);
        t->c->Pump(60ms);
        const std::map<std::string, float> off = Sheet(*t);
        Reset(*t, proto::DevReset::kBag);
        const auto at = [](const std::map<std::string, float>& m, const char* k) {
            const auto it = m.find(k);
            return it != m.end() ? double(it->second) : 0.0;
        };
        std::vector<std::string> failed;
        if (!worn || worn->status != proto::ItemStatus::Ok) failed.push_back("not worn");
        if (std::abs(at(on, "strength") - at(before, "strength") - 5.0) > 0.01) failed.push_back(std::format("STR +{:.0f}, +5 expected (3 + 2 summed)", at(on, "strength") - at(before, "strength")));
        if (std::abs(at(on, "intelligence") - at(before, "intelligence") - 4.0) > 0.01) failed.push_back(std::format("INT +{:.0f}, +4 expected", at(on, "intelligence") - at(before, "intelligence")));
        if (std::abs(at(off, "strength") - at(before, "strength")) > 0.01 || std::abs(at(off, "intelligence") - at(before, "intelligence")) > 0.01) failed.push_back("not back after taking it off");
        return Verdict(1, "blues", failed, "AVATAR_STR 3 and AVATAR_STR_2 2 summed (STR +5), AVATAR_INT 4", {{"strength", 5}, {"intelligence", 4}},
                       {{"strength", {at(before, "strength"), at(on, "strength"), at(off, "strength")}}, {"intelligence", {at(before, "intelligence"), at(on, "intelligence"), at(off, "intelligence")}}});
    }

    // The smith's blessing at NPC_CH_SMITH beside the exam's place: the avatar's list
    // (magicoptionassign.txt) granted in order up to what it may hold, each the option's degree-1 row and its value; one
    // more refused (2, full), one held written again (no new option), an option of no avatar's list refused (0xA). A flag
    // or a Devil's Spirit: refused (8).
    Result Grant(const assets::ItemRow& row, int race, bool male, std::string& error) {
        Tester* t = Get(race, male, true, error);
        if (t == nullptr) return Skip(1, "grant", "no player");
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kBag | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kBag);
        const auto npc = t->models.find(data_.smith);
        if (data_.smith == 0 || npc == t->models.end()) return Skip(1, "grant", "no smith in sight");
        const std::optional<std::uint8_t> slot = Give(*t, row);
        if (!slot) return Verdict(1, "grant", {"not given (DevGive)"}, "", {}, {});
        const auto grant = [&](const std::string& option) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::AvatarGrant{npc->second, *slot, option});
            return t->c->Wait<proto::AlchemyResult>(mark, [](const proto::AlchemyResult& r) { return r.operation == proto::AlchemyOperation::AvatarGrant; }, 2s);
        };
        const auto describe = [](const std::optional<proto::AlchemyResult>& r) {
            return !r ? std::string("no answer") : r->outcome == proto::AlchemyOutcome::Success ? std::format("granted ({} = {})", r->option, r->value)
                                                                                                : std::format("refused 0x{:X}", r->code);
        };
        std::vector<std::string> failed;
        json seen = json::array();
        const std::vector<std::string> list = game::alchemy::AvatarGrantList(*data_.assign, row);
        if (!game::alchemy::IsGrantAvatar(row)) {
            const auto r = grant("MATTR_AVATAR_STR");
            seen.push_back(describe(r));
            if (!r || r->outcome != proto::AlchemyOutcome::Refused || r->code != game::alchemy::code::kGrantNotAvatar) failed.push_back("not refused as no avatar (8): " + describe(r));
            Reset(*t, proto::DevReset::kBag);
            return Verdict(2, "grant", failed, "the smith refuses it: not an avatar it takes (8)", {{"code", 8}}, {{"answers", seen}});
        }
        const int capacity = std::min(12, row.maxMagicOptions);
        const int granted = std::min(capacity, int(list.size()));
        for (int i = 0; i < granted; ++i) {
            const auto r = grant(list[std::size_t(i)]);
            seen.push_back({{"option", list[std::size_t(i)]}, {"answer", describe(r)}});
            const assets::MagicOptionRow* m = OptionFor(list[std::size_t(i)], 1);
            if (!r || r->outcome != proto::AlchemyOutcome::Success) {
                failed.push_back(std::format("{}: {}", list[std::size_t(i)], describe(r)));
                continue;
            }
            if (m == nullptr || r->option != m->paramId) failed.push_back(std::format("{}: option {}, its level-1 row expected", list[std::size_t(i)], r->option));
            else if (const std::uint32_t low = m->rangeWords[1], high = m->rangeWords[2]; r->value < std::max(1u, low) || r->value > std::max(low, high)) {
                failed.push_back(std::format("{}: value {}, {}..{} expected", list[std::size_t(i)], r->value, low, high));
            }
        }
        if (int(list.size()) > capacity) {
            const auto r = grant(list[std::size_t(capacity)]);
            seen.push_back({{"option", list[std::size_t(capacity)]}, {"answer", describe(r)}});
            if (!r || r->outcome != proto::AlchemyOutcome::Refused || r->code != game::alchemy::code::kGrantFull) failed.push_back("one more not refused as full (2): " + describe(r));
        }
        if (granted > 0) {
            const auto r = grant(list[0]);
            seen.push_back({{"option", list[0] + " again"}, {"answer", describe(r)}});
            if (!r || r->outcome != proto::AlchemyOutcome::Success) failed.push_back("a held option not written again: " + describe(r));
        }
        const auto wrong = grant("MATTR_STR");
        seen.push_back({{"option", "MATTR_STR"}, {"answer", describe(wrong)}});
        if (!wrong || wrong->outcome != proto::AlchemyOutcome::Refused || wrong->code != game::alchemy::code::kGrantWrong) failed.push_back("MATTR_STR not refused (0xA): " + describe(wrong));
        t->Sync();
        const auto held = t->places.find(*slot);
        const int options = held != t->places.end() ? int(held->second.magic.size()) : -1;
        if (options != granted) failed.push_back(std::format("it holds {} options, {} expected", options, granted));
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "grant", failed, std::format("{} of its {} options granted (it holds {}), one more refused, MATTR_STR refused", granted, list.size(), capacity),
                       {{"capacity", capacity}, {"list", list}}, {{"answers", seen}, {"options", options}});
    }

    // A European chest whose mastery pairs (0x201..0x206) all ask above level 1, one of them the warrior's or the wizard's
    // (the high European tester's), at most the level cap.
    bool MasteryPiece(const assets::ItemRow& r) const {
        if (game::KindOf(r) != game::ItemKind::Armour || r.country != 1 || r.Rare() || game::EquipPlaceFor(r, false, false) != game::slot::kChest || LevelOf(r) > kHighLevel) {
            return false;
        }
        bool ours = false, any = false;
        for (std::size_t k = 0; k < 4; ++k) {
            const int type = r.requirementTypes[k];
            if (type < 0x201 || type > 0x206) continue;
            any = true;
            if (r.requirementLevels[k] < 2) return false;
            ours = ours || type == int(kWarrior) || type == int(kWizard);
        }
        return any && ours;
    }
    bool IsMasteryTrigger(const assets::ItemRow& row) const {
        if (!MasteryPiece(row)) return false;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            const assets::ItemRow& r = *e.row;
            if (e.routing.route == ItemRoute::Equip && r.sex == row.sex && r.typeId[2] == row.typeId[2] && MasteryPiece(r)) return &r == &row;
        }
        return false;
    }
    // Worn at the cap it gives its defence; its masteries lowered below what it asks (DevLearn, as a skill withdrawal
    // leaves them) it stays worn and gives nothing: the numbers those of the same character with it taken off
    // (the level and the masteries first). The masteries raised again after.
    Result MasteryLowered(const assets::ItemRow& chest, std::string& error) {
        Tester* t = Get(1, MaleFor(chest), true, error);
        if (t == nullptr) return Skip(1, "masteries lowered", "no player");
        Reset(*t, proto::DevReset::kBag);
        const auto defence = [&] {
            t->c->Pump(40ms);
            const auto sheet = Sheet(*t);
            return sheet.contains("phyDefence") ? double(sheet.at("phyDefence")) : -1.0;
        };
        const auto learn = [&](std::uint32_t mastery, int level) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::DevLearn{mastery, std::uint8_t(level), 1});
            t->c->Wait<proto::SkillBookList>(mark, [](const proto::SkillBookList&) { return true; }, 2s);
        };
        const auto off = [&] {
            std::uint8_t free = game::slot::kWorn;
            t->Sync();
            while (t->places.contains(free)) ++free;
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(game::slot::kChest), free, 0});
            return t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& x) { return x.from == game::slot::kChest; }, 2s);
        };
        std::vector<std::string> failed;
        const double bare = defence();
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DevGive{chest.codename, 1, true});
        const auto worn = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from >= game::slot::kWorn; }, 3s);
        if (!worn || worn->status != proto::ItemStatus::Ok || worn->to != game::slot::kChest) {
            Reset(*t, proto::DevReset::kBag);
            return Verdict(1, "masteries lowered", {"not worn at the cap"}, "", {}, {});
        }
        const double atCap = defence();
        if (atCap <= bare) failed.push_back(std::format("worn at the cap: defence {:.2f}, above {:.2f} expected", atCap, bare));
        // Lowered below it: each of the warrior's and the wizard's to 1.
        learn(kWarrior, 1);
        learn(kWizard, 1);
        const double lowered = defence();
        const auto taken = off();
        const double loweredBare = defence();
        if (!taken || taken->status != proto::ItemStatus::Ok) failed.push_back("not taken off with the masteries lowered");
        if (std::abs(lowered - loweredBare) > 0.001) failed.push_back(std::format("its masteries lowered it still gives {:.2f} defence", lowered - loweredBare));
        learn(kWarrior, kHighLevel);
        learn(kWizard, kHighLevel);
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "masteries lowered", failed, "worn at the cap it gives its defence; its masteries lowered below it, nothing (as taken off)",
                       {{"gives", "nothing"}}, {{"bare", bare}, {"atCap", atCap}, {"lowered", lowered}, {"loweredBare", loweredBare}});
    }

    // --- Alchemy ------------------------------------------------
    // Each item through its own request (AlchemyProtocol.h): the rules (game/Alchemy.h) pick what it fits, the server's
    // answer and the bag are read back. A strengthening's result comes on the 4th one-second tick.
    using AlOp = proto::AlchemyOperation;
    using AlOut = proto::AlchemyOutcome;

    template <typename F>
    const assets::ItemRow* FirstEquipment(F fits) const {
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            if (e.routing.route == ItemRoute::Equip && game::alchemy::KindOf(*e.row) == game::alchemy::Kind::Equipment && !e.row->Rare() && fits(*e.row)) {
                return e.row;
            }
        }
        return nullptr;
    }
    template <typename F>
    const assets::ItemRow* AnyTarget(F fits) const {
        if (const assets::ItemRow* r = FirstEquipment(fits)) return r;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            if ((e.routing.route == ItemRoute::Equip || e.routing.route == ItemRoute::Avatar) && fits(*e.row)) return e.row;
        }
        return nullptr;
    }
    // An item given with options on it (DevGive's magic: option id | value << 32): its place.
    std::optional<std::uint8_t> GiveWith(Tester& t, const assets::ItemRow& row, const std::vector<game::ItemInstance::Option>& options) {
        if (options.empty()) return Give(t, row);
        const std::size_t mark = t.c->Mark();
        proto::DevGive give{row.codename, 1, false};
        for (const auto& o : options) give.magic.push_back(std::uint64_t(o.id) | (std::uint64_t(o.value) << 32));
        t.c->Send(give);
        const auto moved = t.c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) {
            return std::any_of(m.changed.begin(), m.changed.end(), [&](const proto::ItemRecord& r) { return r.item == row.refObjId; });
        }, 3s);
        if (!moved) return std::nullopt;
        for (const proto::ItemRecord& r : moved->changed) {
            if (r.item == row.refObjId) return r.slot;
        }
        return std::nullopt;
    }
    std::vector<const assets::ItemRow*> AlchemyRows(game::alchemy::Kind kind) const {
        std::vector<const assets::ItemRow*> out;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            if (game::alchemy::KindOf(*e.row) == kind) out.push_back(e.row);
        }
        return out;
    }
    static game::ItemInstance Fresh(const assets::ItemRow& r) {
        game::ItemInstance i;
        i.refId = r.refObjId;
        return i;
    }
    template <typename M>
    std::optional<proto::AlchemyResult> Alchemize(Tester& t, const M& message, AlOp op, std::chrono::milliseconds wait) {
        const std::size_t mark = t.c->Mark();
        t.c->Send(message);
        const auto r = t.c->Wait<proto::AlchemyResult>(mark, [&](const proto::AlchemyResult& a) { return a.operation == op; }, wait);
        t.c->Pump(80ms);
        t.Sync();
        return r;
    }
    static std::string Said(const std::optional<proto::AlchemyResult>& r) {
        if (!r) return "no answer";
        const char* o = r->outcome == AlOut::Success ? "success" : r->outcome == AlOut::Failure ? "failure" : r->outcome == AlOut::Destroyed ? "destroyed"
                        : r->outcome == AlOut::Cancelled ? "cancelled" : "refused";
        return r->code != 0 ? std::format("{} (0x{:X})", o, r->code) : std::string(o);
    }
    const proto::ItemRecord* At(Tester& t, std::uint8_t place) const {
        const auto it = t.places.find(place);
        return it != t.places.end() ? &it->second : nullptr;
    }
    // A player with an empty bag, and the items given (their places), else the reason.
    Tester* AlchemyPlayer(std::string& error) {
        Tester* t = Get(0, true, true, error);
        if (t != nullptr) Reset(*t, proto::DevReset::kBag);
        return t;
    }

    std::vector<Result> Alchemy(const assets::ItemRow& row, std::string& error) {
        namespace al = game::alchemy;
        if (al::IsSocketStone(row) || al::IsSocketMaker(row) || al::IsSocketEraser(row)) return {Socket(row, error)};
        switch (al::KindOf(row)) {
        case al::Kind::Elixir: return {Strengthen(row, error)};
        case al::Kind::LuckyPowder: return {Powder(row, error)};
        case al::Kind::AdvancedElixir: return {Advanced(row, error)};
        case al::Kind::MagicStone:
        case al::Kind::AttrStone: return {Stone(row, error)};
        case al::Kind::Tablet: return {Manufacture(row, row, error)};
        case al::Kind::Element: return {ElementUse(row, error)};
        case al::Kind::ByProduct: return {Disjoint(row, error)};
        case al::Kind::Rondo: return {row.codename.ends_with("_02") ? Dismantle(row, error) : Disjoint(row, error)};
        default: break;
        }
        return {Skip(1, "alchemy", "a kind the exam has no check for")};
    }

    // An elixir on the first plain piece it fits, at +0: the result on the 4th tick, one elixir used, +1 on a success.
    Result Strengthen(const assets::ItemRow& elixir, std::string& error) {
        const assets::ItemRow* target = AnyTarget([&](const assets::ItemRow& r) { return game::alchemy::CheckReinforce(r, Fresh(r), elixir, nullptr) == 0; });
        if (target == nullptr) return Skip(1, "strengthen", "no equipment it fits");
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, "strengthen", "no player");
        const auto at = Give(*t, *target), el = Give(*t, elixir, 2);
        t->Sync();
        if (!at || !el) return Verdict(1, "strengthen", {"not given (DevGive)"}, "", {}, {});
        const auto r = Alchemize(*t, proto::AlchemyStrengthen{*at, *el, 0}, AlOp::Strengthen, 7s);
        std::vector<std::string> failed;
        if (!r || (r->outcome != AlOut::Success && r->outcome != AlOut::Failure && r->outcome != AlOut::Destroyed)) failed.push_back("strengthening: " + Said(r));
        if (t->Count(elixir.refObjId) != 1) failed.push_back(std::format("{} elixirs left, 1 expected", t->Count(elixir.refObjId)));
        if (r && r->outcome == AlOut::Success && r->plusAfter != r->plusBefore + 1) failed.push_back(std::format("+{} -> +{} on a success", r->plusBefore, r->plusAfter));
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "strengthen", failed, std::format("on {} at +0: {}, the elixir used", target->codename, Said(r)), {{"target", target->codename}},
                       {{"outcome", Said(r)}, {"plus", r ? json(r->plusAfter) : json(nullptr)}});
    }

    // A powder with an elixir on a piece of its degree.
    Result Powder(const assets::ItemRow& powder, std::string& error) {
        const int degree = game::alchemy::Degree(powder);
        const std::vector<const assets::ItemRow*> elixirs = AlchemyRows(game::alchemy::Kind::Elixir);
        const assets::ItemRow* elixir = nullptr;
        const assets::ItemRow* target = FirstEquipment([&](const assets::ItemRow& r) {
            if (game::alchemy::Degree(r) != degree) return false;
            for (const assets::ItemRow* e : elixirs) {
                if (game::alchemy::CheckReinforce(r, Fresh(r), *e, &powder) == 0) return (elixir = e), true;
            }
            return false;
        });
        if (target == nullptr) return Skip(1, "strengthen", std::format("no degree-{} piece an elixir fits", degree));
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, "strengthen", "no player");
        const auto at = Give(*t, *target), el = Give(*t, *elixir), pw = Give(*t, powder, 2);
        t->Sync();
        if (!at || !el || !pw) return Verdict(1, "strengthen", {"not given (DevGive)"}, "", {}, {});
        const auto r = Alchemize(*t, proto::AlchemyStrengthen{*at, *el, *pw}, AlOp::Strengthen, 7s);
        std::vector<std::string> failed;
        if (!r || (r->outcome != AlOut::Success && r->outcome != AlOut::Failure && r->outcome != AlOut::Destroyed)) failed.push_back("strengthening: " + Said(r));
        if (t->Count(powder.refObjId) != 1) failed.push_back(std::format("{} powders left, 1 expected", t->Count(powder.refObjId)));
        // Its degree checked: on a piece of another degree, refused (0x540A) and nothing used.
        const assets::ItemRow* other = FirstEquipment([&](const assets::ItemRow& r) {
            return game::alchemy::Degree(r) != degree && game::alchemy::ElixirFits(*elixir, r);
        });
        std::string wrong = "none";
        if (other != nullptr) {
            const auto ot = Give(*t, *other), el2 = Give(*t, *elixir);
            t->Sync();
            const auto w = ot && el2 ? Alchemize(*t, proto::AlchemyStrengthen{*ot, *el2, *SlotOf(*t, powder.refObjId)}, AlOp::Strengthen, 2s) : std::nullopt;
            wrong = Said(w);
            if (!w || w->outcome != AlOut::Refused || w->code != game::alchemy::code::kPowderDegree) failed.push_back("another degree's piece: " + wrong);
        }
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "strengthen", failed, std::format("with {} on {} (degree {}): {}; another degree refused", elixir->codename, target->codename, degree, Said(r)),
                       {{"degree", degree}}, {{"outcome", Said(r)}, {"otherDegree", wrong}});
    }

    // An advanced elixir: never fails; the piece's advanced plus its Param8.
    Result Advanced(const assets::ItemRow& recipe, std::string& error) {
        const assets::ItemRow* target = FirstEquipment([&](const assets::ItemRow& r) { return game::alchemy::CheckAdvanced(r, Fresh(r), recipe) == 0; });
        if (target == nullptr) {
            // A SoX recipe (Rarity 2) wants a SoX or rare set piece: the first rare one it fits.
            for (const game::ItemCatalogueEntry& e : data_.catalogue) {
                if (e.routing.route == ItemRoute::Equip && game::alchemy::CheckAdvanced(*e.row, Fresh(*e.row), recipe) == 0) {
                    target = e.row;
                    break;
                }
            }
        }
        if (target == nullptr) return Skip(1, "advanced", "no equipment it fits");
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, "advanced", "no player");
        const auto at = Give(*t, *target), rc = Give(*t, recipe);
        t->Sync();
        if (!at || !rc) return Verdict(1, "advanced", {"not given (DevGive)"}, "", {}, {});
        const auto r = Alchemize(*t, proto::AlchemyStrengthen{*at, *rc, 0}, AlOp::Strengthen, 7s);
        std::vector<std::string> failed;
        if (!r || r->outcome != AlOut::Success) failed.push_back("the elixir: " + Said(r));
        const proto::ItemRecord* piece = At(*t, *at);
        const int want = recipe.params[7];
        if (piece == nullptr || piece->advanced != want) failed.push_back(std::format("advanced +{}, +{} expected", piece ? int(piece->advanced) : -1, want));
        if (t->Count(recipe.refObjId) != 0) failed.push_back("the recipe not used");
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "advanced", failed, std::format("on {}: advanced +{}, never failing", target->codename, want), {{"advanced", want}},
                       {{"outcome", Said(r)}, {"advanced", piece ? json(piece->advanced) : json(nullptr)}});
    }

    // A magic, protection or attribute stone on the first plain piece it fits: a result on the 4th tick, the stone used.
    Result Stone(const assets::ItemRow& stone, std::string& error) {
        const bool attr = game::alchemy::KindOf(stone) == game::alchemy::Kind::AttrStone;
        const std::string_view name = stone.paramTexts[0];
        const int degree = stone.params[0];
        // What the piece must hold first (CheckMagicStone): an Astral stone more Immortal charges than Astral ones (0x5424), a
        // Repair stone the Repair option itself (0x5425, the honour items').
        std::vector<game::ItemInstance::Option> held;
        const auto hold = [&](std::string_view option, std::uint32_t value) {
            if (const assets::MagicOptionRow* m = game::alchemy::OptionRow(data_.lab->options, option, degree)) held.push_back({m->paramId, value});
        };
        if (!attr && name == "MATTR_ASTRAL") hold("MATTR_ATHANASIA", 3);
        if (!attr && name == "MATTR_REPAIR") hold("MATTR_REPAIR", 1);
        const auto fits = [&](const assets::ItemRow& r) {
            game::ItemInstance i = Fresh(r);
            i.options = held;
            return attr ? game::alchemy::CheckAttrStone(r, stone) == 0 : game::alchemy::CheckMagicStone(r, i, stone, data_.lab->options) == 0;
        };
        const assets::ItemRow* target = AnyTarget(fits);
        if (target == nullptr) {
            // Nothing it may go on in 1.188 (degree 12: only the fortress weapons, which take no option): refused on a piece of
            // its degree (else the one below), nothing used.
            const auto ofDegree = [&](int d) -> const assets::ItemRow* {
                for (const game::ItemCatalogueEntry& e : data_.catalogue) {
                    if (game::alchemy::KindOf(*e.row) == game::alchemy::Kind::Equipment && game::alchemy::Degree(*e.row) == d) return e.row;
                }
                return nullptr;
            };
            const assets::ItemRow* other = ofDegree(degree) != nullptr ? ofDegree(degree) : ofDegree(degree - 1);
            Tester* t = other != nullptr ? AlchemyPlayer(error) : nullptr;
            if (t == nullptr) return Skip(2, "stone", "no piece of the degree below");
            const auto at = Give(*t, *other), st = Give(*t, stone);
            t->Sync();
            const auto r = at && st ? Alchemize(*t, proto::AlchemyStrengthen{*at, *st, 0}, AlOp::Strengthen, 2s) : std::nullopt;
            std::vector<std::string> failed;
            if (!r || r->outcome != AlOut::Refused) failed.push_back("on a lower degree: " + Said(r));
            if (t->Count(stone.refObjId) != 1) failed.push_back("the stone used");
            Reset(*t, proto::DevReset::kBag);
            return Verdict(2, "stone", failed, std::format("no piece in 1.188 takes it: refused on {} (degree {}), the stone kept", other->codename, game::alchemy::Degree(*other)),
                           {{"degree", degree}}, {{"outcome", Said(r)}});
        }
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, "stone", "no player");
        const auto at = GiveWith(*t, *target, held), st = Give(*t, stone, 2);
        t->Sync();
        if (!at || !st) return Verdict(1, "stone", {"not given (DevGive)"}, "", {}, {});
        const auto r = Alchemize(*t, proto::AlchemyStrengthen{*at, *st, 0}, AlOp::Strengthen, 7s);
        std::vector<std::string> failed;
        if (!r || (r->outcome != AlOut::Success && r->outcome != AlOut::Failure)) failed.push_back("the stone: " + Said(r));
        if (t->Count(stone.refObjId) != 1) failed.push_back(std::format("{} stones left, 1 expected", t->Count(stone.refObjId)));
        if (r && r->outcome == AlOut::Success && !attr) {
            const proto::ItemRecord* piece = At(*t, *at);
            const bool has = piece != nullptr && std::any_of(piece->magic.begin(), piece->magic.end(), [&](std::uint64_t m) { return std::uint32_t(m) == r->option; });
            if (!has) failed.push_back(std::format("its option {} not on the piece", r->option));
        }
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "stone", failed, std::format("on {}: {}, the stone used", target->codename, Said(r)), {{"target", target->codename}},
                       {{"outcome", Said(r)}, {"option", r ? json(r->option) : json(nullptr)}, {"value", r ? json(r->value) : json(nullptr)}});
    }

    // A tablet with its recipe's elements: the stone made at once, the tablet and the listed counts used.
    Result Manufacture(const assets::ItemRow& tablet, const assets::ItemRow& examined, std::string& error) {
        const char* variant = &examined == &tablet ? "make" : "element";
        game::alchemy::Recipe recipe;
        if (!game::alchemy::TabletRecipe(*data_.items, tablet, recipe) || recipe.stone == nullptr) return Skip(1, variant, "its recipe names an item the table lacks");
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, variant, "no player");
        const auto tb = Give(*t, tablet);
        std::vector<std::uint8_t> places;
        for (const auto& [element, count] : recipe.elements) {
            if (const auto e = Give(*t, *element, std::uint32_t(count))) places.push_back(*e);
        }
        t->Sync();
        if (!tb || places.size() != recipe.elements.size()) return Verdict(1, variant, {"not given (DevGive)"}, "", {}, {});
        const auto r = Alchemize(*t, proto::AlchemyManufacture{*tb, places}, AlOp::Manufacture, 3s);
        std::vector<std::string> failed;
        if (!r || r->outcome != AlOut::Success) failed.push_back("making: " + Said(r));
        if (t->Count(tablet.refObjId) != 0) failed.push_back("the tablet not used");
        if (t->Count(recipe.stone->refObjId) != 1) failed.push_back(std::format("{} {} made, 1 expected", t->Count(recipe.stone->refObjId), recipe.stone->codename));
        for (const auto& [element, count] : recipe.elements) {
            if (t->Count(element->refObjId) != 0) failed.push_back(std::format("{} {} left", t->Count(element->refObjId), element->codename));
        }
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, variant, failed, std::format("{} and its {} elements: {}", tablet.codename, recipe.elements.size(), recipe.stone->codename),
                       {{"stone", recipe.stone->codename}}, {{"outcome", Said(r)}});
    }
    // An element: used up in a tablet's making (the first tablet whose recipe lists it).
    Result ElementUse(const assets::ItemRow& element, std::string& error) {
        for (const assets::ItemRow* tablet : AlchemyRows(game::alchemy::Kind::Tablet)) {
            game::alchemy::Recipe recipe;
            if (!game::alchemy::TabletRecipe(*data_.items, *tablet, recipe)) continue;
            if (std::any_of(recipe.elements.begin(), recipe.elements.end(), [&](const auto& e) { return e.first == &element; })) return Manufacture(*tablet, element, error);
        }
        return Skip(1, "element", "no tablet's recipe lists it");
    }

    // A monster's material broken down with Void Rondos (`material` a Rondo: the first material that takes them).
    Result Disjoint(const assets::ItemRow& examined, std::string& error) {
        const assets::ItemRow* rondo = data_.items->Find(std::string_view("ITEM_ETC_ARCHEMY_RONDO_01"));
        const assets::ItemRow* material = &examined;
        if (game::alchemy::KindOf(examined) == game::alchemy::Kind::Rondo) {
            material = nullptr;
            for (const assets::ItemRow* m : AlchemyRows(game::alchemy::Kind::ByProduct)) {
                if (game::alchemy::VoidRondos(*m, 1) > 0) {
                    material = m;
                    break;
                }
            }
        }
        if (rondo == nullptr || material == nullptr) return Skip(1, "disjoint", "no Void Rondo or material in the data");
        const int rondos = game::alchemy::VoidRondos(*material, 1);
        const auto yield = game::alchemy::DisjointYield(*data_.items, *material, 1);
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, "disjoint", "no player");
        const auto mt = Give(*t, *material);
        if (rondos > 0) Give(*t, *rondo, std::uint32_t(rondos));
        t->Sync();
        if (!mt) return Verdict(1, "disjoint", {"not given (DevGive)"}, "", {}, {});
        const auto r = Alchemize(*t, proto::AlchemyDisjoint{{*mt}}, AlOp::Disjoint, 3s);
        std::vector<std::string> failed;
        if (yield.empty()) {
            // Nothing comes of it: refused, the material kept.
            if (!r || r->outcome != AlOut::Refused) failed.push_back("a material that yields nothing: " + Said(r));
            if (t->Count(material->refObjId) != 1) failed.push_back("the material not kept");
        } else {
            if (!r || r->outcome != AlOut::Success) failed.push_back("disjoining: " + Said(r));
            if (t->Count(material->refObjId) != 0) failed.push_back("the material not used");
            if (t->Count(rondo->refObjId) != 0) failed.push_back(std::format("{} Rondos left of {}", t->Count(rondo->refObjId), rondos));
            const bool got = std::any_of(yield.begin(), yield.end(), [&](const auto& y) { return t->Count(y.first->refObjId) > 0; });
            if (!got) failed.push_back("none of its elements given");
        }
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "disjoint", failed, std::format("{} with {} Void Rondo(s): {} kinds of element", material->codename, rondos, yield.size()),
                       {{"rondos", rondos}, {"yields", yield.size()}}, {{"outcome", Said(r)}});
    }

    // A plain piece dismantled with its Destroyer Rondos: the piece and the Rondos used.
    Result Dismantle(const assets::ItemRow& rondo, std::string& error) {
        const assets::ItemRow* target = FirstEquipment([&](const assets::ItemRow& r) { return game::alchemy::DestroyerRondos(r) > 0; });
        if (target == nullptr) return Skip(1, "dismantle", "no equipment to dismantle");
        const int rondos = game::alchemy::DestroyerRondos(*target);
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, "dismantle", "no player");
        const auto at = Give(*t, *target);
        Give(*t, rondo, std::uint32_t(rondos));
        t->Sync();
        if (!at) return Verdict(1, "dismantle", {"not given (DevGive)"}, "", {}, {});
        const auto r = Alchemize(*t, proto::AlchemyDismantle{*at}, AlOp::Dismantle, 3s);
        std::vector<std::string> failed;
        if (!r || r->outcome != AlOut::Success) failed.push_back("dismantling: " + Said(r));
        if (t->Count(target->refObjId) != 0) failed.push_back("the piece not gone");
        if (t->Count(rondo.refObjId) != 0) failed.push_back(std::format("{} Rondos left of {}", t->Count(rondo.refObjId), rondos));
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "dismantle", failed, std::format("{} with {} Destroyer Rondo(s)", target->codename, rondos), {{"rondos", rondos}}, {{"outcome", Said(r)}});
    }

    // Sockets: a socket made with the maker, a stone put in (the 4th tick), erased with the eraser.
    Result Socket(const assets::ItemRow& row, std::string& error) {
        namespace al = game::alchemy;
        const std::vector<const assets::ItemRow*> all = [&] {
            std::vector<const assets::ItemRow*> v;
            for (const game::ItemCatalogueEntry& e : data_.catalogue) v.push_back(e.row);
            return v;
        }();
        const auto find = [&](bool (*kind)(const assets::ItemRow&)) -> const assets::ItemRow* {
            for (const assets::ItemRow* r : all) {
                if (kind(*r)) return r;
            }
            return nullptr;
        };
        const assets::ItemRow* maker = find(al::IsSocketMaker);
        const assets::ItemRow* eraser = find(al::IsSocketEraser);
        const assets::ItemRow* stone = al::IsSocketStone(row) ? &row : nullptr;
        if (stone == nullptr) {
            for (const assets::ItemRow* r : all) {
                if (al::IsSocketStone(*r) && !al::SocketStoneDisabled(*r)) {
                    stone = r;
                    break;
                }
            }
        }
        if (maker == nullptr || eraser == nullptr || stone == nullptr) return Skip(1, "socket", "no maker, eraser or stone in the data");
        const bool disabled = al::SocketStoneDisabled(*stone);
        // A piece that takes a socket and, with it made, the stone (any degree: the stones ask a degree).
        const assets::ItemRow* target = nullptr;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            const assets::ItemRow& r = *e.row;
            if (e.routing.route != ItemRoute::Equip || al::CheckSocketMake(r, Fresh(r), 0) != 0) continue;
            game::ItemInstance i = Fresh(r);
            al::MakeSocket(i, 0);
            if (disabled || al::CheckSocketInsert(r, i, *stone, 0) == 0) {
                target = &r;
                break;
            }
        }
        if (target == nullptr) return Skip(1, "socket", "no piece takes a socket and this stone");
        Tester* t = AlchemyPlayer(error);
        if (t == nullptr) return Skip(1, "socket", "no player");
        const auto at = Give(*t, *target), mk = Give(*t, *maker), st = Give(*t, *stone), er = Give(*t, *eraser);
        t->Sync();
        if (!at || !mk || !st || !er) return Verdict(1, "socket", {"not given (DevGive)"}, "", {}, {});
        std::vector<std::string> failed;
        json seen = json::object();
        const auto made = Alchemize(*t, proto::SocketMake{*at, 0}, AlOp::SocketMake, 3s);
        seen["make"] = Said(made);
        if (!made || made->outcome != AlOut::Success) failed.push_back("making the socket: " + Said(made));
        const auto in = Alchemize(*t, proto::SocketInsert{*at, *SlotOf(*t, stone->refObjId), 0}, AlOp::SocketInsert, 7s);
        seen["insert"] = Said(in);
        if (disabled) {
            if (!in || in->outcome != AlOut::Refused || in->code != al::code::kSocketStoneKind) failed.push_back("the disabled stone: " + Said(in));
        } else {
            if (!in || in->outcome != AlOut::Success) failed.push_back("the stone: " + Said(in));
            const auto out = Alchemize(*t, proto::SocketErase{*at, 0}, AlOp::SocketErase, 3s);
            seen["erase"] = Said(out);
            if (!out || out->outcome != AlOut::Success) failed.push_back("erasing: " + Said(out));
        }
        Reset(*t, proto::DevReset::kBag);
        return Verdict(1, "socket", failed,
                       disabled ? std::format("{}: refused (ours disables it: its effect never acts in the original)", stone->codename)
                                : std::format("on {}: a socket made, {} put in, erased", target->codename, stone->codename),
                       {{"target", target->codename}, {"stone", stone->codename}}, seen);
    }

    // --- A use from the bag ---------------------------------------
    struct UseSeen {
        bool used = false;                    // ItemUsed for it
        std::optional<std::uint16_t> refused; // ItemRefused's code
        int left = 0;                         // its count after
    };
    UseSeen UseOnce(Tester& t, const assets::ItemRow& row) {
        UseSeen s;
        const auto slot = SlotOf(t, row.refObjId);
        if (!slot) return s;
        const std::size_t mark = t.c->Mark();
        t.c->Send(proto::UseItem{*slot});
        s.used = t.c->Wait<proto::ItemUsed>(mark, [&](const proto::ItemUsed& u) { return u.entity == t.entity && u.item == row.refObjId; }, 700ms).has_value();
        if (const auto r = RefusedSince(t, mark, row.refObjId)) s.refused = r->code;
        t.c->Pump(80ms);
        t.Sync();
        s.left = t.Count(row.refObjId);
        return s;
    }
    static std::string Seen(const UseSeen& s) {
        return s.used ? std::format("used ({} left)", s.left) : s.refused ? std::format("refused 0x{:X} ({} left)", *s.refused, s.left) : std::format("no answer ({} left)", s.left);
    }

    // Two given, one used: what its path says should happen, then a second use where "one running" refuses it.
    std::vector<Result> BagUse(const assets::ItemRow& row, ItemRoute route, std::string& error) {
        Tester* t = Get(RaceFor(row), MaleFor(row), true, error);
        if (t == nullptr) return {Skip(1, "use", "no player")};
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kServices | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        if (!Give(*t, row, 2)) return {Verdict(1, "use", {"not given (DevGive)"}, "", {}, {})};
        t->Sync();
        const int given = t->Count(row.refObjId);
        std::vector<std::string> failed;
        const std::size_t useMark = t->c->Mark();
        const UseSeen first = UseOnce(*t, row);
        json seen = {{"first", Seen(first)}};
        std::string text;
        int scenario = 1;
        const auto keptRefused = [&](std::optional<std::uint16_t> code, const std::string& why) {
            scenario = 2;
            text = why;
            if (first.used) failed.push_back("used: " + Seen(first));
            if (first.left != given) failed.push_back(std::format("{} left of {}: not kept", first.left, given));
            if (code && first.refused != code) failed.push_back(std::format("{}, 0x{:X} expected", Seen(first), *code));
        };
        const auto usedOne = [&](const std::string& why) {
            text = why;
            if (!first.used) failed.push_back("not used: " + Seen(first));
            else if (first.left != given - 1) failed.push_back(std::format("{} left of {}: not one taken", first.left, given));
        };
        const auto secondRefused = [&](std::uint16_t code) {
            const UseSeen second = UseOnce(*t, row);
            seen["second"] = Seen(second);
            if (second.used || second.refused != code) failed.push_back(std::format("a second: {}, refused 0x{:X} expected", Seen(second), code));
        };
        switch (route) {
        case ItemRoute::OriginalRefuses: {
            const std::uint16_t code = game::OriginalUseRefusal(row);
            keptRefused(code, std::format("refused 0x{:X} as the original's, kept", code));
            break;
        }
        case ItemRoute::QuestUse: {
            const game::QuestItemUse u = game::QuestItemUseOf(row.codename);
            if (u.taker == game::QuestItemTaker::GuildSupport || u.taker == game::QuestItemTaker::Pandora) {
                usedOne("its event takes it (the summons), one used");
            } else {
                keptRefused(game::kNoQuestTakesIt, u.taker == game::QuestItemTaker::None ? "no handler in 1.188: refused 2, kept"
                                                                                           : "without its quest: refused 2, kept (as the original's)");
            }
            break;
        }
        case ItemRoute::QuestItem:
        case ItemRoute::NotUsable: keptRefused(std::nullopt, "not used from the bag: nothing happens, kept"); break;
        case ItemRoute::Declined: keptRefused(0x1894, "refused 0x1894 (ours declines it), kept"); break;
        case ItemRoute::Firework: {
            usedOne("used, one taken");
            if (game::CoolTimeOf(row).group != 0) secondRefused(game::kCoolTimeRefused);
            break;
        }
        case ItemRoute::BerserkPotion: usedOne("the gauge filled, one taken"); break;
        case ItemRoute::BonusScroll:
            usedOne("its bonus running, one taken");
            secondRefused(game::kBonusRunning);
            break;
        case ItemRoute::TimeTicket:
            usedOne("its service running, one taken");
            secondRefused(0x1894);
            break;
        case ItemRoute::Premium: usedOne("used, one taken"); break;
        case ItemRoute::Expansion: {
            // P1 the places, P2 0 the bag, 1 the storage (opened this
            // session first, else refused 2).
            if (row.params[1] == 1) {
                keptRefused(2, "the storage not opened this session: refused 2, kept");
                break;
            }
            text = std::format("the bag {} places larger, one taken", row.params[0]);
            const auto grown = t->c->Since<proto::StorageGrown>(useMark);
            seen["grown"] = grown.empty() ? json(nullptr) : json(grown.back().second.size);
            if (grown.empty() || grown.back().second.target != 1) failed.push_back("no StorageGrown for the bag");
            if (first.left != given - 1) failed.push_back(std::format("{} left of {}: not one taken", first.left, given));
            break;
        }
        default: break;
        }
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kServices)});
        Reset(*t, proto::DevReset::kAll);
        return {Verdict(scenario, "use", failed, text, {{"route", game::ItemRouteName(route)}}, seen)};
    }

    // --- Job suits: put on by a member of its league (an alias, in a town, no party), the change ----
    std::vector<Result> Suit(const assets::ItemRow& row, std::string& error) {
        const game::JobType job = game::SuitJob(row);
        const Profile profile = job == game::JobType::Trader ? kTrader : job == game::JobType::Thief ? kThief : kHunter;
        const int race = RaceFor(row);
        const bool male = MaleFor(row);
        const auto inTown = [&](Tester& t) {
            const std::size_t mark = t.c->Mark();
            t.c->Send(proto::DevGoto{race == 0 ? 1u : 20u}); // GATE_CH, GATE_EU
            return t.c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t.entity; }, 8s).has_value();
        };
        const auto putOn = [&](Tester& t) {
            const std::size_t mark = t.c->Mark();
            t.c->Send(proto::DevGive{row.codename, 1, true});
            return t.c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) {
                return m.to == game::slot::kJobSuit ? m.status != proto::ItemStatus::JobChange : m.from >= game::slot::kWorn && m.status != proto::ItemStatus::Ok;
            }, 4s);
        };
        std::vector<Result> out;
        Tester* t = Get(race, male, profile, error);
        if (t == nullptr) return {Skip(1, "wear", "no player")};
        Reset(*t, proto::DevReset::kAll);
        if (!inTown(*t)) return {Skip(1, "wear", "the town not reached (DevGoto)")};
        t->c->Pump(100ms);
        const auto worn = putOn(*t);
        std::vector<std::string> failed;
        if (!worn || worn->status != proto::ItemStatus::Ok || worn->to != game::slot::kJobSuit) {
            failed.push_back(std::format("not worn ({}, 0x{:X})", worn ? int(worn->status) : -1, worn ? worn->code : 0));
        } else {
            // Off again, in the town (its change too), its lock after the change waited out (jobLockSeconds).
            t->c->Pump(500ms);
            t->Sync();
            std::uint8_t free = game::slot::kWorn;
            while (t->places.contains(free)) ++free;
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(game::slot::kJobSuit), free, 0});
            const auto off = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) {
                return m.from == game::slot::kJobSuit && m.status != proto::ItemStatus::JobChange;
            }, 4s);
            if (!off || off->status != proto::ItemStatus::Ok) failed.push_back("not taken off in the town");
        }
        out.push_back(Verdict(1, "wear", failed, std::format("worn and taken off by a {} of its league (job level 7) in the town",
                                                             job == game::JobType::Trader ? "trader" : job == game::JobType::Thief ? "thief" : "hunter"),
                              {{"job", int(job)}}, {{"status", worn ? json(int(worn->status)) : json(nullptr)}}));
        Reset(*t, proto::DevReset::kAll);
        // One of no league: refused (0x189E).
        if (Tester* none = Get(race, male, true, error)) {
            Reset(*none, proto::DevReset::kAll);
            std::vector<std::string> f2;
            if (inTown(*none)) {
                none->c->Pump(100ms);
                const auto refused = putOn(*none);
                if (!refused || refused->status != proto::ItemStatus::JobSuit || refused->code != 0x189E) {
                    f2.push_back(std::format("{} / 0x{:X}, JobSuit 0x189E expected", refused ? int(refused->status) : -1, refused ? refused->code : 0));
                }
            } else {
                f2.push_back("the town not reached");
            }
            out.push_back(Verdict(2, "no league", f2, "refused to one of no league (0x189E)", {{"code", "0x189E"}}, {}));
            none->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
            Reset(*none, proto::DevReset::kAll);
        }
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        return out;
    }

    // --- A ride's, transport's or pet's summoning: CosSummoned; a scroll used up, a pet's item kept --
    std::vector<Result> Summon(const assets::ItemRow& row, ItemRoute route, std::string& error) {
        Tester* t = Get(RaceFor(row), true, true, error);
        if (t == nullptr) return {Skip(1, "summon", "no player")};
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        if (!Give(*t, row)) return {Verdict(1, "summon", {"not given (DevGive)"}, "", {}, {})};
        t->Sync();
        const auto slot = SlotOf(*t, row.refObjId);
        const std::size_t mark = t->c->Mark();
        t->c->Send(proto::UseItem{*slot});
        const auto summoned = t->c->Wait<proto::CosSummoned>(mark, [](const proto::CosSummoned&) { return true; }, 3s);
        t->c->Pump(100ms);
        t->Sync();
        std::vector<std::string> failed;
        json seen = json::object();
        const auto refusalsFirst = t->c->Since<proto::CosRefused>(mark);
        if (!summoned && !refusalsFirst.empty() && refusalsFirst.back().second.text == "UIIT_MSG_STRGERR_YOU_CANT_ACTIVATE_CART") {
            // A transport: refused to one of no league (as here); a trader in his suit summons it (Coses: a trader's or a
            // thief's, a thief's not in a town).
            Reset(*t, proto::DevReset::kAll);
            return {Verdict(2, "no league", {}, "a transport refused to one of no league (UIIT_MSG_STRGERR_YOU_CANT_ACTIVATE_CART)", {}, {}),
                    TraderSummons(row, error)};
        }
        if (!summoned) {
            const auto refusals = t->c->Since<proto::CosRefused>(mark);
            const auto item = RefusedSince(*t, mark, row.refObjId);
            failed.push_back(std::format("not summoned ({})", !refusals.empty() ? refusals.back().second.text
                                                              : item ? std::format("0x{:X}", item->code) : std::string("no answer")));
        } else {
            seen["model"] = summoned->model;
            const bool pet = route == ItemRoute::PetSummon;
            if (pet && t->Count(row.refObjId) != 1) failed.push_back("a pet's item not kept");
            if (!pet && t->Count(row.refObjId) != 0) failed.push_back("the scroll not used up");
            const std::size_t m2 = t->c->Mark();
            t->c->Send(proto::CosDismiss{summoned->entity});
            if (!t->c->Wait<proto::CosDismissed>(m2, [&](const proto::CosDismissed& d) { return d.entity == summoned->entity; }, 2s)) failed.push_back("not dismissed");
        }
        Reset(*t, proto::DevReset::kAll);
        return {Verdict(1, "summon", failed, route == ItemRoute::PetSummon ? "summoned, its item kept, dismissed" : "summoned, the scroll used up, dismissed",
                        {{"route", game::ItemRouteName(route)}}, seen)};
    }

    // --- A talisman: registered in the collection book once, then refused (0x1818) -------
    std::vector<Result> TalismanUse(const assets::ItemRow& row, std::string& error) {
        Tester* t = Get(RaceFor(row), true, true, error);
        if (t == nullptr) return {Skip(1, "register", "no player")};
        Reset(*t, proto::DevReset::kAll);
        // A tester registers each talisman once for the exam: a fresh player per item (the book is the character's).
        if (!Give(*t, row, 2)) return {Verdict(1, "register", {"not given (DevGive)"}, "", {}, {})};
        t->Sync();
        // Through the collection book's request (RegisterTalisman): the first taken in, the
        // second refused (UIIT_MSG_COLLECTION_WINDOW_RECORD_ERR) and kept.
        const auto registerOnce = [&]() -> std::optional<proto::TalismanRegistered> {
            const auto slot = SlotOf(*t, row.refObjId);
            if (!slot) return std::nullopt;
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::RegisterTalisman{*slot});
            const auto r = t->c->Wait<proto::TalismanRegistered>(mark, [](const proto::TalismanRegistered&) { return true; }, 2s);
            t->c->Pump(80ms);
            t->Sync();
            return r;
        };
        const auto first = registerOnce();
        const int afterFirst = t->Count(row.refObjId);
        const auto second = registerOnce();
        std::vector<std::string> failed;
        if (!first || !first->ok || first->text != "UIIT_MSG_COLLECTION_WINDOW_RECORD_FINISH") failed.push_back("first: " + (first ? first->text : std::string("no answer")));
        if (afterFirst != 1) failed.push_back(std::format("{} left after the first: not taken in", afterFirst));
        if (!second || second->ok || t->Count(row.refObjId) != 1) failed.push_back("second: " + (second ? second->text : std::string("no answer")) + ", refused and kept expected");
        Reset(*t, proto::DevReset::kAll);
        return {Verdict(1, "register", failed, "registered in the collection book once (RECORD_FINISH), a second refused (RECORD_ERR) and kept", {},
                        {{"first", first ? first->text : ""}, {"second", second ? second->text : ""}})};
    }

    // --- A resurrection scroll: refused alive (0x1887), used where it fell when dead ------
    std::vector<Result> Rise(const assets::ItemRow& row, std::string& error) {
        Tester* t = Get(RaceFor(row), true, true, error);
        if (t == nullptr) return {Skip(1, "rise", "no player")};
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        if (!Give(*t, row, 2)) return {Verdict(1, "rise", {"not given (DevGive)"}, "", {}, {})};
        t->Sync();
        std::vector<Result> out;
        const UseSeen alive = UseOnce(*t, row);
        out.push_back(Verdict(2, "alive", alive.used || alive.refused != 0x1887 || alive.left != 2 ? std::vector<std::string>{"alive: " + Seen(alive)} : std::vector<std::string>{},
                              "refused alive (0x1887), kept", {{"code", "0x1887"}}, {{"seen", Seen(alive)}}));
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DevDie{});
        t->c->Pump(400ms);
        mark = t->c->Mark();
        const auto slot = SlotOf(*t, row.refObjId);
        t->c->Send(proto::UseItem{*slot});
        const auto vitals = t->c->Wait<proto::Vitals>(mark, [](const proto::Vitals& v) { return v.hp > 0; }, 3s);
        t->c->Pump(100ms);
        t->Sync();
        std::vector<std::string> failed;
        if (!vitals) failed.push_back("not risen (no health back)");
        if (t->Count(row.refObjId) != 1) failed.push_back(std::format("{} left of 2: not one taken", t->Count(row.refObjId)));
        out.push_back(Verdict(1, "rise", failed, "dead: risen where it fell with its health, one taken", {}, {{"hp", vitals ? json(vitals->hp) : json(nullptr)}}));
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        return out;
    }

    // A trader of the row's race in his suit (in the town) summons the transport; it is dismissed and the suit taken off.
    Result TraderSummons(const assets::ItemRow& row, std::string& error) {
        const int race = RaceFor(row);
        const assets::ItemRow* suit = nullptr;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            const assets::ItemRow& r = *e.row;
            if (e.routing.route == ItemRoute::JobSuit && game::SuitJob(r) == game::JobType::Trader && RaceFor(r) == race && MaleFor(r) && LevelOf(r) <= kHighLevel) {
                suit = &r;
                break;
            }
        }
        if (suit == nullptr) return Skip(1, "summon", "no trader's suit of its race");
        Tester* t = Get(race, true, kTrader, error);
        if (t == nullptr) return Skip(1, "summon", "no trader");
        Reset(*t, proto::DevReset::kAll);
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DevGoto{race == 0 ? 1u : 20u});
        if (!t->c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t->entity; }, 8s)) return Skip(1, "summon", "the town not reached");
        t->c->Pump(100ms);
        mark = t->c->Mark();
        t->c->Send(proto::DevGive{suit->codename, 1, true});
        const auto worn = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) {
            return m.to == game::slot::kJobSuit ? m.status != proto::ItemStatus::JobChange : m.from >= game::slot::kWorn && m.status != proto::ItemStatus::Ok;
        }, 4s);
        std::vector<std::string> failed;
        if (!worn || worn->status != proto::ItemStatus::Ok) failed.push_back("the trader's suit not worn");
        t->c->Pump(500ms);
        Give(*t, row);
        t->Sync();
        mark = t->c->Mark();
        t->c->Send(proto::UseItem{*SlotOf(*t, row.refObjId)});
        const auto summoned = t->c->Wait<proto::CosSummoned>(mark, [](const proto::CosSummoned&) { return true; }, 3s);
        t->c->Pump(100ms);
        t->Sync();
        if (!summoned) {
            const auto refusals = t->c->Since<proto::CosRefused>(mark);
            failed.push_back("a trader in his suit: not summoned (" + (refusals.empty() ? std::string("no answer") : refusals.back().second.text) + ")");
        } else {
            if (t->Count(row.refObjId) != 0) failed.push_back("the scroll not used up");
            const std::size_t m2 = t->c->Mark();
            t->c->Send(proto::CosDismiss{summoned->entity});
            t->c->Wait<proto::CosDismissed>(m2, [&](const proto::CosDismissed& d) { return d.entity == summoned->entity; }, 2s);
        }
        // The suit off again.
        t->c->Pump(300ms);
        std::uint8_t free = game::slot::kWorn;
        while (t->places.contains(free)) ++free;
        mark = t->c->Mark();
        t->c->Send(proto::ItemMove{std::uint8_t(game::slot::kJobSuit), free, 0});
        t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from == game::slot::kJobSuit && m.status != proto::ItemStatus::JobChange; }, 4s);
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "summon", failed, std::format("summoned by a trader in {} (in the town), the scroll used up, dismissed", suit->codename), {}, {});
    }

    // --- The mall's tools: each on what it works on --------------------------------------------
    Result MallTool(const assets::ItemRow& row, ItemRoute route, std::string& error) {
        const int race = RaceFor(row);
        Tester* t = Get(race, true, true, error);
        if (t == nullptr) return Skip(1, "use", "no player");
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        std::vector<std::string> failed;
        json seen = json::object();
        std::string text;
        const auto done = [&]() {
            Reset(*t, proto::DevReset::kAll);
            return Verdict(1, "use", failed, text, {{"route", game::ItemRouteName(route)}}, seen);
        };
        if (!Give(*t, row)) return Verdict(1, "use", {"not given (DevGive)"}, "", {}, {});
        t->Sync();
        const std::uint8_t tool = *SlotOf(*t, row.refObjId);
        switch (route) {
        case ItemRoute::MagicCube: {
            // A B elixir of its kind in and out (CubeTakes; GameServer::MoveCube).
            const assets::ItemRow* elixir = nullptr;
            for (const game::ItemCatalogueEntry& e : data_.catalogue) {
                if (game::CubeTakes(row, *e.row)) {
                    elixir = e.row;
                    break;
                }
            }
            if (elixir == nullptr) return Skip(1, "use", "no item its kind takes");
            const auto el = Give(*t, *elixir);
            t->Sync();
            std::size_t mark = t->c->Mark();
            t->c->Send(proto::CubeMove{0, *el, tool, 0});
            bool moved = t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == *el; }, 1500ms).has_value();
            if (!moved) {
                // A cube new since the entry: its items read first (CubeContents), then the move again.
                t->c->Wait<proto::CubeContents>(mark, [](const proto::CubeContents&) { return true; }, 3s);
                seen["retried"] = true;
                mark = t->c->Mark();
                t->c->Send(proto::CubeMove{0, *el, tool, 0});
                t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == *el; }, 2s);
            }
            t->c->Pump(80ms);
            t->Sync();
            const int in = t->Count(elixir->refObjId);
            mark = t->c->Mark();
            t->c->Send(proto::CubeMove{1, 0, tool, 0});
            t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved&) { return true; }, 2s);
            t->c->Pump(80ms);
            t->Sync();
            const int out = t->Count(elixir->refObjId);
            seen["elixir"] = elixir->codename;
            seen["inBag"] = in;
            seen["backInBag"] = out;
            if (in != 0) failed.push_back("not put in the cube");
            if (out != 1) failed.push_back("not taken out to the bag");
            text = std::format("{} put in (of its {} places) and taken out", elixir->codename, game::CubePlaces(row));
            return done();
        }
        case ItemRoute::GlobalChat: {
            proto::ChatSend line;
            line.kind = proto::ChatKind::Global;
            line.text = "the item exam";
            line.slot = tool;
            t->c->Send(line);
            t->c->Pump(300ms);
            t->Sync();
            if (t->Count(row.refObjId) != 0) failed.push_back("not taken by the global line");
            text = "a global line takes it";
            return done();
        }
        case ItemRoute::GenderSwitch: {
            // A plain armour piece of degree up to its Param1 with its other sex's counterpart (same name, M <-> W / F).
            const assets::ItemRow* target = nullptr;
            const assets::ItemRow* other = nullptr;
            for (const game::ItemCatalogueEntry& e : data_.catalogue) {
                const assets::ItemRow& r = *e.row;
                if (e.routing.route != ItemRoute::Equip || game::KindOf(r) != game::ItemKind::Armour || r.sex != 1 || r.Rare()) continue;
                for (const char* to : {"_W_", "_F_"}) {
                    std::string name = r.codename;
                    if (const std::size_t at = name.find("_M_"); at != std::string::npos) name.replace(at, 3, to);
                    const assets::ItemRow* c = data_.items->Find(std::string_view(name));
                    if (c != nullptr && c != &r && game::GenderSwitchRefusal(row, game::slot::kWorn + 1, 109, &r, c) == 0) {
                        target = &r;
                        other = c;
                        break;
                    }
                }
                if (target != nullptr) break;
            }
            if (target == nullptr) return Skip(1, "use", "no armour it switches");
            const auto at = Give(*t, *target);
            t->Sync();
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::UseItemOn{tool, *at});
            t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) {
                return std::any_of(m.changed.begin(), m.changed.end(), [&](const proto::ItemRecord& r) { return r.slot == *at; });
            }, 2s);
            t->c->Pump(80ms);
            t->Sync();
            const proto::ItemRecord* now = At(*t, *at);
            seen = {{"from", target->codename}, {"to", other->codename}, {"now", now ? json(now->item) : json(nullptr)}};
            if (now == nullptr || now->item != other->refObjId) failed.push_back(std::format("{} not switched to {}", target->codename, other->codename));
            if (t->Count(row.refObjId) != 0) failed.push_back("the tool not taken");
            text = std::format("{} became {}", target->codename, other->codename);
            return done();
        }
        case ItemRoute::RepairHammer: {
            const assets::ItemRow* weapon = FirstPlain([](const assets::ItemRow& r) { return game::KindOf(r) == game::ItemKind::Weapon; });
            if (weapon == nullptr) return Skip(1, "use", "no weapon");
            const auto at = Give(*t, *weapon, 1, 1); // its durability 1
            t->Sync();
            const std::uint32_t before = 1;
            const std::size_t useMark = t->c->Mark();
            const UseSeen used = UseOnce(*t, row);
            t->c->Pump(100ms);
            std::uint32_t after = before;
            for (auto& [when, d] : t->c->Since<proto::DurabilityChanged>(useMark)) {
                if (d.place == *at) after = d.durability;
            }
            seen = {{"before", before}, {"after", after}, {"use", Seen(used)}};
            if (after <= before) failed.push_back(std::format("durability {} -> {}: not mended", before, after));
            if (used.left != 0) failed.push_back("the hammer not taken");
            text = std::format("{} mended from {} to {}", weapon->codename, before, after);
            return done();
        }
        case ItemRoute::SpiritExtension: {
            const assets::ItemRow* spirit = nullptr;
            for (const game::ItemCatalogueEntry& e : data_.catalogue) {
                if (game::IsDevilSpirit(*e.row) && RaceFor(*e.row) == race) {
                    spirit = e.row;
                    break;
                }
            }
            if (spirit == nullptr) return Skip(1, "use", "no Devil's Spirit");
            const std::uint32_t day = std::uint32_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 86400);
            const auto at = Give(*t, *spirit, 1, day);
            t->Sync();
            const std::uint32_t before = day; // its awakening's end as given
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::UseItemOn{tool, *at});
            t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) {
                return std::any_of(m.changed.begin(), m.changed.end(), [&](const proto::ItemRecord& r) { return r.slot == *at; });
            }, 2s);
            t->c->Pump(80ms);
            t->Sync();
            const std::uint32_t after = At(*t, *at) ? At(*t, *at)->durability : 0;
            const std::uint32_t want = before + std::uint32_t(std::max(0, row.params[0]));
            seen = {{"before", before}, {"after", after}, {"want", want}};
            if (after != want) failed.push_back(std::format("awakening's end {} -> {}, {} expected", before, after, want));
            if (t->Count(row.refObjId) != 0) failed.push_back("the tool not taken");
            text = std::format("{}'s awakening {} s longer", spirit->codename, row.params[0]);
            return done();
        }
        case ItemRoute::SkinChange: {
            // Another sex of the same race (nothing worn), its scale kept at 0.
            const std::uint32_t model = kFigure[race][0];
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::SkinChange{tool, model, 0});
            const auto answer = t->c->Wait<proto::ServiceAnswer>(mark, [](const proto::ServiceAnswer&) { return true; }, 2s);
            t->c->Pump(80ms);
            t->Sync();
            seen = {{"code", answer ? json(answer->code) : json(nullptr)}};
            if (!answer || answer->code != 0) failed.push_back(std::format("answered {}", answer ? std::format("0x{:X}", answer->code) : std::string("nothing")));
            if (t->Count(row.refObjId) != 0) failed.push_back("the scroll not taken");
            text = "a new model of its race (the other sex, nothing worn): the scroll taken";
            t->c->Send(proto::SkinChange{std::uint8_t(0), kFigure[race][1], 0}); // back (no scroll: refused, harmless)
            return done();
        }
        default: break;
        }
        return Skip(1, "use", "no check for its path");
    }

    // --- The pets' care: each on the pet in the state it is for ----------
    // The pet's row (_CharCOS, its item's data word) set while it is put away: hungry, hurt or dead.
    Result PetCare(const assets::ItemRow& row, ItemRoute route, std::string& error) {
        const bool clock = route == ItemRoute::PetTreatment && row.typeId[3] == 12; // a grab pet's time (3/3/13/12)
        const bool revival = route == ItemRoute::PetTreatment && !clock;           // a dead attack pet (3/3/1/6)
        const int kind = clock ? 2 : 1;                                            // 3/2/1/2 grab, 3/2/1/1 attack
        const assets::ItemRow* pet = nullptr;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            if (e.routing.route == ItemRoute::PetSummon && e.row->typeId == std::array<int, 4>{3, 2, 1, kind}) {
                pet = e.row;
                break;
            }
        }
        if (pet == nullptr) return Skip(1, "care", "no pet of its kind");
        Tester* t = Get(0, true, true, error);
        if (t == nullptr) return Skip(1, "care", "no player");
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace | proto::DevReset::kServices), kPlace});
        Reset(*t, proto::DevReset::kAll);
        const auto petAt = Give(*t, *pet);
        Give(*t, row, 2);
        t->Sync();
        if (!petAt) return Verdict(1, "care", {"not given (DevGive)"}, "", {}, {});
        const auto summon = [&]() -> std::optional<proto::CosSummoned> {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::UseItem{*petAt});
            const auto s = t->c->Wait<proto::CosSummoned>(mark, [](const proto::CosSummoned&) { return true; }, 3s);
            t->c->Pump(150ms);
            t->Sync();
            return s;
        };
        const auto dismiss = [&](std::uint32_t entity) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::CosDismiss{entity});
            t->c->Wait<proto::CosDismissed>(mark, [&](const proto::CosDismissed& d) { return d.entity == entity; }, 2s);
        };
        std::vector<std::string> failed;
        json seen = json::object();
        auto out = summon();
        if (!out) return Verdict(1, "care", {"its pet not summoned"}, "", {}, {});
        // Its row: the newest of the tester's pets (the item's data word is told in a message of its own).
        const std::int64_t cos = exam_.Query("SELECT c.id FROM _charcos c JOIN _char ch ON ch.charid = c.ownercharid WHERE ch.charname16 = $1 ORDER BY c.id DESC LIMIT 1",
                                             {t->name}).Int(0, 0);
        seen["cos"] = cos;
        // The state it is for, set on the row while the pet is put away (its save waited for first).
        const auto setRow = [&](const std::string& set) {
            dismiss(out->entity);
            for (int i = 0; i < 60 && exam_.Query("SELECT state FROM _charcos WHERE id = $1", {cos}).Int(0, 0) != std::int64_t(game::CosState::Inactive); ++i) {
                t->c->Pump(50ms);
            }
            exam_.Query("UPDATE _charcos SET " + set + " WHERE id = $1", {cos});
        };
        std::string text;
        if (route == ItemRoute::PetFood) {
            setRow("hgp = 1000");
            out = summon();
            text = "fed to the hungry attack pet out (HGP 1,000 of 10,000): one taken";
        } else if (route == ItemRoute::PetRecovery) {
            setRow("hp = 10");
            out = summon();
            text = "on the hurt pet out (10 HP): one taken";
        } else if (route == ItemRoute::PetCure) {
            proto::DevAfflict afflict;
            afflict.target = out->entity;
            afflict.state = std::uint8_t(game::StateKind::Poison);
            afflict.level = 1;
            afflict.ms = 20000;
            afflict.value = 1;
            t->c->Send(afflict);
            t->c->Pump(200ms);
            text = "on the poisoned pet out: one taken";
        } else if (revival) {
            setRow(std::format("state = {}, hp = 0", int(game::CosState::Dead)));
            out.reset();
            text = "on the dead attack pet's item: revived, one taken";
        } else if (clock) {
            // A grab pet whose time ran out (dead by its clock): renewed for the tool's minutes.
            setRow(std::format("state = {}, rentendtime = now() - interval '1 hour'", int(game::CosState::Dead)));
            out.reset();
            text = "on the grab pet's item, its time run out: renewed, one taken";
        } else {
            text = "on the attack pet's item, its pet out: one taken";
        }
        if (route != ItemRoute::PetTreatment && !out) failed.push_back("its pet not summoned again");
        const std::uint8_t tool = *SlotOf(*t, row.refObjId);
        const std::size_t mark = t->c->Mark();
        if (route == ItemRoute::PetTreatment) t->c->Send(proto::CosUseOnPet{tool, *petAt});
        else if (route == ItemRoute::PetGrowth) t->c->Send(proto::UseItemOn{tool, *petAt});
        else t->c->Send(proto::UseItem{tool});
        t->c->Pump(500ms);
        t->Sync();
        const auto refusals = t->c->Since<proto::CosRefused>(mark);
        const auto refused = RefusedSince(*t, mark, row.refObjId);
        seen["left"] = t->Count(row.refObjId);
        if (t->Count(row.refObjId) != 1) {
            failed.push_back(std::format("not taken ({})", !refusals.empty() ? refusals.back().second.text
                                                       : refused ? std::format("0x{:X}", refused->code) : std::string("no answer")));
        }
        if (revival || clock) {
            const std::int64_t state = exam_.Query("SELECT state FROM _charcos WHERE id = $1", {cos}).Int(0, 0);
            t->c->Pump(300ms);
            seen["stateAfter"] = exam_.Query("SELECT state FROM _charcos WHERE id = $1", {cos}).Int(0, 0);
            if (seen["stateAfter"] == int(game::CosState::Dead) && state == int(game::CosState::Dead)) failed.push_back("the pet still dead");
        }
        if (out) dismiss(out->entity);
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "care", failed, text, {{"route", game::ItemRouteName(route)}, {"pet", pet->codename}}, seen);
    }

    // --- Gold on the ground (GameServer::GoldModel): an amount of its band dropped wears its look; picked up, it comes back --
    Result GoldLook(const assets::ItemRow& row, std::string& error) {
        const std::uint64_t amount = row.codename.ends_with("_01") ? 500 : row.codename.ends_with("_02") ? 5000 : 50000;
        Tester* t = Get(0, true, true, error);
        if (t == nullptr) return Skip(1, "drop", "no player");
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DropGold{amount});
        const auto pile = t->c->Wait<proto::EntityAppeared>(mark, [&](const proto::EntityAppeared& e) {
            return e.kind == proto::EntityKind::GroundItem && e.character.model == row.refObjId;
        }, 2s);
        std::vector<std::string> failed;
        json seen = {{"amount", amount}};
        if (!pile) {
            failed.push_back(std::format("no pile with its look for {} gold", amount));
        } else {
            mark = t->c->Mark();
            t->c->Send(proto::PickUp{pile->entity});
            const auto back = t->c->Wait<proto::GoldChanged>(mark, [](const proto::GoldChanged&) { return true; }, 4s);
            seen["after"] = back ? json(back->gold) : json(nullptr);
            if (!back) failed.push_back("not picked up (no GoldChanged)");
        }
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "drop", failed, std::format("{} gold dropped wears its look, picked up again", amount), {{"look", row.codename}}, seen);
    }

    // --- The skill withdrawal: a skill one level down with its potion, the SP back ----------
    Result Withdrawal(const assets::ItemRow& potion, std::string& error) {
        const game::WithdrawMode mode = game::WithdrawModeOf(potion);
        if (mode == game::WithdrawMode::None) return Skip(1, "withdraw", "not a withdrawal potion");
        // The European warrior alone (its skills' levels matter to no other check), its warrior's skills raised to level 2.
        Tester* t = Get(1, true, kWarriorOnly, error);
        if (t == nullptr) return Skip(1, "withdraw", "no player");
        Reset(*t, proto::DevReset::kAll);
        const std::size_t learnMark = t->c->Mark();
        t->c->Send(proto::DevLearn{kWarrior, std::uint8_t(kHighLevel), 2});
        t->c->Wait<proto::SkillBookList>(learnMark, [](const proto::SkillBookList&) { return true; }, 3s);
        // Its book as the server last told it, and a skill the rule lets down one level.
        const auto lists = t->c->Since<proto::SkillBookList>(learnMark);
        if (lists.empty()) return Skip(1, "withdraw", "no skill book told");
        game::SkillBook book;
        for (const proto::MasteryRecord& m : lists.back().second.masteries) book.masteries[m.mastery] = m.level;
        for (const std::uint32_t id : lists.back().second.skills) {
            if (const assets::SkillRow* r = data_.skills->Find(id)) book.groups[r->group] = std::max(book.groups[r->group], r->level);
        }
        std::uint32_t skill = 0;
        int target = 0;
        for (const auto& [group, level] : book.groups) {
            if (level < 2) continue;
            const assets::SkillRow* r = data_.skills->GroupLevel(group, level);
            if (r == nullptr) continue;
            const game::Withdrawal w = game::WithdrawSkill(book, *data_.skills, r->id, level - 1, mode, 0);
            if (w.code == 0 && w.levels == 1) {
                skill = r->id;
                target = level - 1;
                break;
            }
        }
        if (skill == 0) return Skip(1, "withdraw", "no learned skill the rule lets down a level");
        if (!Give(*t, potion, 2)) return Verdict(1, "withdraw", {"not given (DevGive)"}, "", {}, {});
        t->Sync();
        const std::size_t mark = t->c->Mark();
        t->c->Send(proto::Withdraw{*SlotOf(*t, potion.refObjId), 0, skill, std::uint8_t(target)});
        const auto answer = t->c->Wait<proto::ServiceAnswer>(mark, [](const proto::ServiceAnswer& a) { return a.request == proto::ServiceRequest::Withdraw; }, 3s);
        t->c->Pump(100ms);
        t->Sync();
        std::vector<std::string> failed;
        if (!answer || answer->code != 0) failed.push_back(std::format("answered {}", answer ? std::format("0x{:X}", answer->code) : std::string("nothing")));
        if (t->Count(potion.refObjId) != 1) failed.push_back(std::format("{} potions left of 2: not one taken", t->Count(potion.refObjId)));
        // Back to where it was (the next check's book).
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "withdraw", failed, std::format("skill {} withdrawn to level {} (mode {}): one potion taken", skill, target, int(mode)),
                       {{"skill", skill}, {"to", target}}, {{"code", answer ? json(answer->code) : json(nullptr)}});
    }

    // --- Emblems: the 16 x 16 image beside a guild's or a union's name, by its master ----------
    bool unionMade_ = false;
    // Two masters of level-5 guilds (the fortress testers of each race) in one union: the first leads.
    bool MakeUnion(Tester& lead, std::string& error) {
        if (unionMade_) return true;
        Tester* other = Get(1, true, kFortress, error);
        if (other == nullptr) return false;
        for (Tester* t : {&lead, other}) {
            t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
            t->c->Pump(200ms);
        }
        const std::size_t mark = other->c->Mark();
        lead.c->Send(proto::UnionInvite{other->entity});
        if (!other->c->Wait<proto::GuildProposal>(mark, [](const proto::GuildProposal& p) { return p.kind == proto::kProposalUnion; }, 3s)) return false;
        const std::size_t joined = lead.c->Mark();
        other->c->Send(proto::GuildProposalAnswer{proto::kProposalUnion, 1});
        lead.c->Pump(800ms);
        unionMade_ = lead.c->Mark() > joined;
        return unionMade_;
    }
    Result Crest(const assets::ItemRow& row, std::string& error) {
        const std::optional<game::CrestItem> item = game::CrestItemOf(row.codename);
        if (!item) return Skip(1, "crest", "not a crest item");
        const bool forUnion = item->kind == proto::kCrestUnion;
        Tester* t = Get(0, true, kFortress, error);
        if (t == nullptr) return Skip(1, "crest", "no guild master");
        if (forUnion && !MakeUnion(*t, error)) return Skip(1, "crest", "no union made");
        Reset(*t, proto::DevReset::kAll);
        // This kind's register and delete items.
        const assets::ItemRow* reg = nullptr;
        const assets::ItemRow* del = nullptr;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            const auto c = game::CrestItemOf(e.row->codename);
            if (c && c->kind == item->kind) (c->remove ? del : reg) = e.row;
        }
        if (reg == nullptr || del == nullptr) return Skip(1, "crest", "no register or delete item of its kind");
        const std::vector<std::uint8_t> pixels(proto::kCrestBytes, 0x80);
        // One request with that item: the master's answer (GuildAnswer, its error) and whether the item went.
        struct Done {
            std::optional<std::uint16_t> error;
            bool taken = false;
        };
        const auto request = [&](const assets::ItemRow& by, bool remove) -> Done {
            Done d;
            if (!Give(*t, by)) return d;
            t->Sync();
            const std::uint8_t slot = *SlotOf(*t, by.refObjId);
            const proto::GuildAction action = remove ? (forUnion ? proto::GuildAction::UnionCrestDelete : proto::GuildAction::CrestDelete)
                                                     : (forUnion ? proto::GuildAction::UnionCrestRegister : proto::GuildAction::CrestRegister);
            const std::size_t mark = t->c->Mark();
            if (remove) t->c->Send(proto::GuildCrestDelete{item->kind, slot});
            else t->c->Send(proto::GuildCrestRegister{item->kind, slot, pixels});
            const auto answer = t->c->Wait<proto::GuildAnswer>(mark, [&](const proto::GuildAnswer& a) { return a.action == action; }, 3s);
            t->c->Pump(100ms);
            t->Sync();
            if (answer) d.error = answer->error;
            d.taken = t->Count(by.refObjId) == 0;
            return d;
        };
        const auto said = [](const Done& d) { return d.error ? std::format("0x{:X}", *d.error) : std::string("no answer"); };
        std::vector<std::string> failed;
        json seen = json::object();
        std::string text;
        if (!item->remove) {
            request(*del, true); // an earlier emblem away first (none: NoCrest, the item kept)
            Reset(*t, proto::DevReset::kAll);
            const Done d = request(row, false);
            seen["answer"] = d.error ? json(*d.error) : json(nullptr);
            if (d.error != std::uint16_t(0)) failed.push_back("not registered: " + said(d));
            if (!d.taken) failed.push_back("the item kept");
            // A second while it stands: refused CrestExists, the item kept (0x4C4E).
            Reset(*t, proto::DevReset::kAll);
            const Done again = request(row, false);
            seen["again"] = again.error ? json(*again.error) : json(nullptr);
            if (again.error != std::uint16_t(game::GuildError::CrestExists) || again.taken) failed.push_back("a second not refused CrestExists: " + said(again));
            text = std::format("a {} emblem registered by its master: the item taken; a second refused 0x4C4E", forUnion ? "union's" : "guild's");
        } else {
            const Done first = request(*reg, false);
            if (first.error != std::uint16_t(0) && first.error != std::uint16_t(game::GuildError::CrestExists)) failed.push_back("none registered first: " + said(first));
            Reset(*t, proto::DevReset::kAll);
            const Done d = request(row, true);
            seen["answer"] = d.error ? json(*d.error) : json(nullptr);
            if (d.error != std::uint16_t(0)) failed.push_back("not deleted: " + said(d));
            if (!d.taken) failed.push_back("the item kept");
            // Again with none: refused NoCrest (0x4C4F), the item kept.
            Reset(*t, proto::DevReset::kAll);
            const Done again = request(row, true);
            seen["again"] = again.error ? json(*again.error) : json(nullptr);
            if (again.error != std::uint16_t(game::GuildError::NoCrest) || again.taken) failed.push_back("a second not refused NoCrest: " + said(again));
            text = std::format("a {} emblem deleted by its master: the item taken; again with none refused 0x4C4F", forUnion ? "union's" : "guild's");
        }
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "crest", failed, text, {{"kind", forUnion ? "union" : "guild"}}, seen);
    }

    // --- The guild recall (GameServer::UseGuildRecall): by its guild's master in a town ---------
    Result Recall(const assets::ItemRow& row, std::string& error) {
        const int race = RaceFor(row);
        std::vector<std::string> failed;
        json seen = json::object();
        // Not a master (no guild): refused 0x18CB, kept.
        if (Tester* low = Get(race, true, kLow, error)) {
            Reset(*low, proto::DevReset::kAll);
            if (Give(*low, row)) {
                low->Sync();
                const UseSeen u = UseOnce(*low, row);
                seen["notMaster"] = Seen(u);
                if (u.refused != game::kRecallNotMaster || u.left != 1) failed.push_back("a player in no guild not refused 0x18CB");
            }
            Reset(*low, proto::DevReset::kAll);
        }
        Tester* t = Get(race, true, kFortress, error);
        if (t == nullptr) return Skip(1, "use", "no guild master");
        Reset(*t, proto::DevReset::kAll);
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DevGoto{race == 0 ? 1u : 20u}); // GATE_CH, GATE_EU: in town
        if (!t->c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t->entity; }, 8s)) {
            return Skip(1, "use", "the town not reached (DevGoto)");
        }
        t->c->Pump(100ms);
        if (!Give(*t, row)) return Verdict(1, "use", {"not given (DevGive)"}, "", {}, {});
        t->Sync();
        mark = t->c->Mark();
        t->c->Send(proto::UseItem{*SlotOf(*t, row.refObjId)});
        // Its wait in the database first, then the item taken and the use told.
        const bool used = t->c->Wait<proto::ItemUsed>(mark, [&](const proto::ItemUsed& u) { return u.entity == t->entity && u.item == row.refObjId; }, 4s).has_value();
        t->c->Pump(100ms);
        t->Sync();
        const auto refused = RefusedSince(*t, mark, row.refObjId);
        seen["master"] = {{"used", used}, {"left", t->Count(row.refObjId)}, {"refused", refused ? json(refused->code) : json(nullptr)}};
        if (!used || t->Count(row.refObjId) != 0) failed.push_back(std::format("not used by the master in town ({})", refused ? std::format("0x{:X}", refused->code) : std::string("no answer")));
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        t->c->Pump(300ms);
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "use", failed, "by its guild's master (guild level 5) in town: taken, the members called; by one in no guild refused 0x18CB", {}, seen);
    }

    // --- Uses that need only themselves (GameServer::UseItem): the monster mask with a monster in it, a stall's
    // decoration, So-Ok's Subscription Card ------------------------------------------------------------------------------
    Result PlainUse(const assets::ItemRow& row, ItemRoute route, std::string& error) {
        Tester* t = Get(RaceFor(row), true, true, error);
        if (t == nullptr) return Skip(1, "use", "no player");
        Reset(*t, proto::DevReset::kAll);
        std::uint32_t data = 0;
        std::string text;
        json seen = json::object();
        if (route == ItemRoute::MonsterMask) {
            const assets::ObjectRow* monster = data_.characters ? data_.characters->Find(std::string_view("MOB_CH_MANGNYANG")) : nullptr;
            if (monster == nullptr) return Skip(1, "use", "no monster to carry");
            data = monster->refObjId;
            seen["monster"] = monster->codename;
            text = std::format("carrying {} (level {}): its shape taken, the mask used up", monster->codename, monster->level);
        } else if (route == ItemRoute::StallDecoration) {
            text = "the stall's look from now on (_StaticAvatar.Param1): used up";
        } else {
            text = "So-Ok's Subscription Card: the account queued (_FlagWorld_EventParticipants), Subscribed told, used up";
        }
        if (!Give(*t, row, 1, data)) return Verdict(1, "use", {"not given (DevGive)"}, "", {}, {});
        t->Sync();
        const std::size_t mark = t->c->Mark();
        const UseSeen u = UseOnce(*t, row);
        t->c->Pump(300ms);
        seen["use"] = Seen(u);
        std::vector<std::string> failed;
        if (u.left != 0) failed.push_back(std::format("not used up ({})", u.refused ? std::format("0x{:X}", *u.refused) : std::string("no refusal")));
        const std::int64_t character = exam_.Query("SELECT charid FROM _char WHERE charname16 = $1", {t->name}).Int(0, 0);
        if (route == ItemRoute::StallDecoration) {
            const std::int64_t skin = exam_.Query("SELECT param1 FROM _staticavatar WHERE charid = $1", {character}).Int(0, 0);
            seen["skin"] = skin;
            if (skin != std::int64_t(row.refObjId)) failed.push_back(std::format("the stall's look {} saved, {} expected", skin, row.refObjId));
        } else if (route == ItemRoute::FlagCard) {
            const bool told = !t->c->Since<proto::FlagWorldNotice>(mark).empty();
            const std::int64_t count = exam_.Query("SELECT coalesce(sum(count), 0) FROM _flagworld_eventparticipants", {}).Int(0, 0); // its own database: this card's
            seen["told"] = told;
            seen["queued"] = count;
            if (!told) failed.push_back("no notice (Subscribed)");
            if (count < 1) failed.push_back("the account not queued");
        } else if (!u.used) {
            failed.push_back("the use not told (ItemUsed)");
        }
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "use", failed, text, {{"route", game::ItemRouteName(route)}}, seen);
    }

    // --- Magic POP: a card played at its machine becomes a coupon in place; Gori makes every
    // coupon its prize ------------------------------------------------------------------------------------------------
    Result Pop(const assets::ItemRow& row, std::string& error) {
        if (!data_.gacha || data_.popMachine == 0 || data_.popGori == 0) return Skip(1, "pop", "no machine or Gori (the gacha tables)");
        const assets::GachaMachine* machine = data_.gacha->Machine(data_.popMachine);
        const assets::GachaEntry* entry = nullptr;
        for (const assets::GachaEntry& e : data_.gacha->entries()) {
            const assets::ItemRow* prize = data_.items->Find(e.item);
            if (machine != nullptr && e.set == machine->selection && prize != nullptr && game::KindOf(*prize) != game::ItemKind::Weapon) {
                entry = &e;
                break;
            }
        }
        if (entry == nullptr) return Skip(1, "pop", "no entry of the machine's set");
        Tester* t = Get(RaceFor(row), true, true, error);
        if (t == nullptr) return Skip(1, "pop", "no player");
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        Reset(*t, proto::DevReset::kAll);
        t->c->Pump(300ms);
        const auto machineAt = t->models.find(data_.popMachine), goriAt = t->models.find(data_.popGori);
        if (machineAt == t->models.end() || goriAt == t->models.end()) return Skip(1, "pop", "the machine or Gori not in sight");
        std::vector<std::string> failed;
        json seen = json::object();
        std::optional<std::uint8_t> at;
        std::string text;
        if (game::pop::IsCard(row)) {
            at = Give(*t, row);
            t->Sync();
            if (!at) return Verdict(1, "pop", {"not given (DevGive)"}, "", {}, {});
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::PopPlay{machineAt->second, entry->id, *at});
            const auto played = t->c->Wait<proto::PopPlayed>(mark, [](const proto::PopPlayed&) { return true; }, 4s);
            t->c->Pump(100ms);
            t->Sync();
            // The coupon in the answer's record (the card's place, in place).
            if (played && !played->item.empty()) t->places[played->item[0].slot] = played->item[0];
            const proto::ItemRecord* now = At(*t, *at);
            const assets::ItemRow* coupon = now ? data_.items->Find(now->item) : nullptr;
            const std::string want = played ? row.paramTexts[played->won ? 1 : 0] : std::string();
            seen["played"] = played ? json{{"code", played->code}, {"won", played->won}, {"prize", played->prize}} : json(nullptr);
            seen["coupon"] = coupon ? json(coupon->codename) : json(nullptr);
            if (!played || played->code != 0) failed.push_back(std::format("played: {}", played ? std::format("0x{:X}", played->code) : std::string("no answer")));
            else if (coupon == nullptr || coupon->codename != want) failed.push_back(std::format("the card not the coupon {} in place", want));
            text = std::format("played at {} for entry {}: the card became its coupon in place (Desc1 lost / Desc2 won)", machineAt->first, entry->id);
        } else {
            proto::DevGive give{row.codename, 1, false};
            give.magic = game::pop::CouponParams(*entry);
            const std::size_t mark = t->c->Mark();
            t->c->Send(give);
            const auto moved = t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) {
                return std::any_of(m.changed.begin(), m.changed.end(), [&](const proto::ItemRecord& r) { return r.item == row.refObjId; });
            }, 3s);
            if (moved) {
                for (const proto::ItemRecord& r : moved->changed) {
                    if (r.item == row.refObjId) at = r.slot;
                }
            }
            t->Sync();
            if (!at) return Verdict(1, "pop", {"not given (DevGive)"}, "", {}, {});
            text = std::format("its prize (entry {}: item {} x{}) made at Gori in place", entry->id, entry->item, entry->count);
        }
        // Every coupon its prize at Gori.
        if (failed.empty()) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::PopExchange{goriAt->second});
            const auto exchanged = t->c->Wait<proto::PopExchanged>(mark, [](const proto::PopExchanged&) { return true; }, 4s);
            t->c->Pump(100ms);
            t->Sync();
            if (exchanged) {
                for (const proto::ItemRecord& r : exchanged->items) t->places[r.slot] = r;
            }
            const proto::ItemRecord* now = At(*t, *at);
            seen["exchanged"] = exchanged ? json{{"code", exchanged->code}, {"items", exchanged->items.size()}} : json(nullptr);
            seen["prize"] = now ? json(now->item) : json(nullptr);
            if (!exchanged || exchanged->code != 0 || exchanged->items.empty()) {
                failed.push_back(std::format("Gori: {}", exchanged ? std::format("0x{:X}, {} items", exchanged->code, exchanged->items.size()) : std::string("no answer")));
            } else if (game::pop::IsCard(row) ? (now == nullptr || data_.items->Find(now->item) == nullptr || game::pop::IsCoupon(*data_.items->Find(now->item)))
                                              : (now == nullptr || now->item != entry->item)) {
                failed.push_back("the coupon not its prize in place");
            }
            text += "; Gori: the prize in its place";
        }
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "pop", failed, text, {{"entry", entry->id}}, seen);
    }

    // --- The dimension holes: by a party's master in town in its level band, a gap to its
    // instance; the master through it into the instance's layer --------------------------------------------------------
    int holes_ = 0;
    Result Hole(const assets::ItemRow& row, std::string& error) {
        const int level = std::max(1, row.params[2]); // Param3..Param4 the band
        const int key = holes_++;
        // Its own two players at the band's foot (a hole's wait, a master's one gap and the world's count kept apart).
        Tester* master = Get(0, true, Banded(key, level), error);
        Tester* member = master ? Get(1, true, Banded(key, level), error) : nullptr;
        if (master == nullptr || member == nullptr) return Skip(1, "hole", "no players");
        const auto toTown = [&](Tester& t) {
            const std::size_t mark = t.c->Mark();
            t.c->Send(proto::DevGoto{1u}); // GATE_CH
            return t.c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t.entity; }, 8s).has_value();
        };
        if (!toTown(*master) || !toTown(*member)) return Skip(1, "hole", "the town not reached (DevGoto)");
        master->c->Pump(200ms);
        member->c->Pump(100ms);
        std::vector<std::string> failed;
        json seen = {{"level", level}};
        // Not yet a party's master: refused (UIIT_MGS_ERR_GOD_TELEPORT_HOLE_NOT_PARTY), kept.
        if (!Give(*master, row)) return Verdict(1, "hole", {"not given (DevGive)"}, "", {}, {});
        master->Sync();
        std::size_t mark = master->c->Mark();
        master->c->Send(proto::UseItem{*SlotOf(*master, row.refObjId)});
        const auto alone = master->c->Wait<proto::InstanceRefused>(mark, [](const proto::InstanceRefused&) { return true; }, 2s);
        seen["alone"] = alone ? json(alone->text) : json(nullptr);
        if (!alone || alone->text != "UIIT_MGS_ERR_GOD_TELEPORT_HOLE_NOT_PARTY") failed.push_back("used without a party");
        // The party: the master invites, the member answers yes.
        mark = member->c->Mark();
        master->c->Send(proto::PartyInvite{member->entity, 0});
        if (!member->c->Wait<proto::PartyInvited>(mark, [](const proto::PartyInvited&) { return true; }, 3s)) return Skip(1, "hole", "no party invitation");
        member->c->Send(proto::PartyInviteAnswer{1});
        master->c->Pump(400ms);
        member->c->Pump(100ms);
        master->Sync();
        mark = master->c->Mark();
        master->c->Send(proto::UseItem{*SlotOf(*master, row.refObjId)});
        master->c->Pump(600ms);
        master->Sync();
        const auto refused = master->c->Since<proto::InstanceRefused>(mark);
        if (!refused.empty()) failed.push_back("refused " + refused.back().second.text);
        if (master->Count(row.refObjId) != 0) failed.push_back("the hole not used up");
        // The gap (INS_QUEST_TELEPORT: the database's row, not in the client's characterdata) beside the master.
        const std::int64_t gapModel = exam_.Query("SELECT id FROM _refobjcommon WHERE codename128 = 'INS_QUEST_TELEPORT'", {}).Int(0, 0);
        // Its own (the workers' masters open theirs at the same gate): the gap whose information names its master.
        std::uint32_t gap = 0;
        for (const auto& [at, e] : master->c->Since<proto::EntityAppeared>(mark)) {
            if (gap != 0 || gapModel == 0 || e.character.model != std::uint32_t(gapModel)) continue;
            const std::size_t asked = master->c->Mark();
            master->c->Send(proto::SelectGap{e.entity});
            const auto info = master->c->Wait<proto::GapInfo>(asked, [&](const proto::GapInfo& g) { return g.entity == e.entity; }, 2s);
            if (info && info->master == master->name) gap = e.entity;
        }
        seen["gap"] = gap;
        if (gap == 0) {
            failed.push_back("no gap opened");
        } else {
            mark = master->c->Mark();
            master->c->Send(proto::EnterGap{gap});
            const auto in = master->c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == master->entity; }, 10s);
            master->c->Pump(300ms);
            const auto barred = master->c->Since<proto::InstanceRefused>(mark);
            seen["entered"] = in.has_value();
            if (!in) failed.push_back(std::format("the master not through the gap{}", barred.empty() ? "" : " (" + barred.back().second.text + ")"));
            else seen["region"] = in->position.region;
        }
        return Verdict(1, "hole", failed,
                       std::format("{} by a party's master in town at level {}: used up, a gap to {}, the master through it; alone refused NOT_PARTY",
                                   row.codename, level, row.paramTexts[1]),
                       {{"world", row.paramTexts[1]}}, seen);
    }

    // --- The fortress war's items (Fortresses::UseItem): each in its period and place by
    // the fortress's holder (Commander) or its attacker, through FortressUseItem -----------------------------------------
    std::optional<bool> warOn_;
    // The war on or off at every fortress (the developer's switch; the holder applied against at each).
    void SetWar(Tester& t, bool on) {
        if (warOn_ == on) return;
        for (const FortressSpec& f : data_.fortresses) {
            t.c->Send(proto::FortressDev{on ? proto::FortressDevAction::StartWar : proto::FortressDevAction::EndWar, f.id, 0});
            t.c->Pump(150ms);
        }
        t.c->Pump(500ms);
        warOn_ = on;
    }
    // Taken beside the fortress's heart (or the structure `entity`): its place.
    std::optional<proto::WorldPosition> Beside(Tester& t, std::uint32_t fortress, std::uint32_t entity = 0) {
        const std::size_t mark = t.c->Mark();
        t.c->Send(proto::FortressDev{proto::FortressDevAction::GoTo, fortress, entity});
        const auto in = t.c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t.entity; }, 8s);
        t.c->Pump(300ms);
        if (!in) return std::nullopt;
        return in->position;
    }
    // A structure of the fortress the player sees (of kind `want`, else any standing): its entity (_SiegeFortressStruct's
    // models: the database's rows, some not in the client's characterdata).
    std::uint32_t SeenStructure(Tester& t, const FortressSpec& spec, game::StructureKind want) {
        const QueryResult r = exam_.Query("SELECT refeventstructid, refobjid FROM _siegefortressstruct WHERE fortressid = $1", {std::int64_t(spec.id)});
        std::uint32_t any = 0;
        for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
            const auto it = t.models.find(std::uint32_t(r.Int(i, 1)));
            if (it == t.models.end()) continue;
            const auto z = std::find_if(spec.structures.begin(), spec.structures.end(), [&](const FortressSpec::Structure& x) { return x.eventId == std::uint32_t(r.Int(i, 0)); });
            if (z != spec.structures.end() && z->kind == want) return it->second;
            if (any == 0) any = it->second;
        }
        return any;
    }
    Result FortressItem(const assets::ItemRow& row, std::string& error) {
        const auto fr = std::find_if(data_.fortItems.begin(), data_.fortItems.end(), [&](const FortressItemRow& r) { return r.refObjId == row.refObjId; });
        if (fr == data_.fortItems.end()) return Skip(1, "fortress", "not a fortress item (_RefObjItem's rows)");
        if (data_.fortresses.empty() || data_.prepared.size() < 2) return Skip(1, "fortress", "no fortress or no holder");
        const int kind = row.typeId[3];
        const bool guardOrTablet = row.typeId[2] == 3 && (kind == 6 || kind == 7);
        const bool camp = row.typeId[2] == 3 && kind == 8 && fr->target.find("HEADQUARTERS") != std::string::npos;
        const bool bomb = row.typeId[2] == 6;
        const bool kit = row.typeId[2] == 1;
        // The fortress: a guard's or a tablet's the one whose guards take its group (none: Jangan's, refused 0x18E7).
        const std::string group = [&] {
            std::string g = fr->target;
            while (!g.empty() && (g.back() == ' ' || g.back() == '\0')) g.pop_back();
            return g;
        }();
        std::uint32_t fortress = data_.fortresses.front().id;
        bool groupTaken = true;
        if (guardOrTablet) {
            groupTaken = false;
            for (const FortressSpec& f : data_.fortresses) {
                const bool takes = std::any_of(data_.fortGuards.begin(), data_.fortGuards.end(),
                                               [&](const SiegeVariantRow& v) { return v.fortress == f.id && v.group == group; });
                if (takes) {
                    fortress = f.id;
                    groupTaken = true;
                    break;
                }
            }
        }
        const FortressSpec& spec = *std::find_if(data_.fortresses.begin(), data_.fortresses.end(), [&](const FortressSpec& f) { return f.id == fortress; });
        Tester* keeper = Get(0, true, kKeeper, error);
        Tester* raider = keeper ? Get(0, true, kRaider, error) : nullptr;
        if (keeper == nullptr || raider == nullptr) return Skip(1, "fortress", "the holder or the attacker not in");
        Tester& t = camp || bomb ? *raider : *keeper;
        Reset(t, proto::DevReset::kAll);
        // Its period (Param3: 1 the war, 2 outside it).
        if (fr->time != 0) SetWar(t, fr->time == 1);
        std::vector<std::string> failed;
        json seen = {{"fortress", spec.codename}, {"time", fr->time}, {"place", fr->place}, {"mask", fr->mask}};
        auto here = Beside(t, fortress);
        if (!here) return Skip(1, "fortress", "not taken to the fortress (FortressDev GoTo)");
        if (!Give(t, row)) return Verdict(1, "fortress", {"not given (DevGive)"}, "", {}, {});
        t.Sync();
        proto::FortressUseItem m;
        m.slot = *SlotOf(t, row.refObjId);
        m.fortress = fortress;
        std::string text;
        if (row.typeId[2] == 3 && kind == 8) {
            // A manual on a free zone of its kind (the first of the fortress's), from in front of it (the developer's go-to).
            const game::StructureKind want = camp ? game::StructureKind::Headquarters : game::StructureKind::Barricade;
            const auto spot = std::find_if(spec.structures.begin(), spec.structures.end(), [&](const FortressSpec::Structure& z) { return z.kind == want; });
            if (spot == spec.structures.end()) return Skip(1, "fortress", "no zone of its kind");
            const std::size_t gone = t.c->Mark();
            t.c->Send(proto::FortressDev{proto::FortressDevAction::GoTo, fortress, 0, spot->eventId});
            if (!t.c->Wait<proto::WorldEntered>(gone, [&](const proto::WorldEntered& w) { return w.entity == t.entity; }, 8s)) {
                return Skip(1, "fortress", "not taken to its zone (FortressDev GoTo)");
            }
            t.c->Pump(300ms);
            m.zone = spot->eventId;
            text = std::format("{} on {}'s zone {} by its {}: built", row.codename, spec.codename, spot->eventId, camp ? "attacker" : "holder");
        } else if (kit) {
            // A repair kit on the holder's guard tower, hurt first (a developer's hit).
            const std::uint32_t tower = SeenStructure(t, spec, game::StructureKind::GuardTower);
            if (tower == 0) return Skip(1, "fortress", "no structure in sight");
            here = Beside(t, fortress, tower);
            // Hurt by the attacker (the holder's own hit does nothing to it).
            Reset(*raider, proto::DevReset::kAll);
            Beside(*raider, fortress, tower);
            raider->c->Send(proto::DevStrike{tower, 60000}); // more than an earlier kit's repair still running gives back
            raider->c->Pump(300ms);
            t.c->Pump(100ms);
            m.target = tower;
            text = std::format("{} on the holder's hurt guard tower at {}: its repair begun", row.codename, spec.codename);
        } else if (bomb) {
            // The shock bomb by the attacker on the holder beside it (its skill: living enemies, players and monsters).
            Reset(*keeper, proto::DevReset::kAll);
            Beside(*keeper, fortress);
            here = Beside(t, fortress, keeper->entity);
            m.target = keeper->entity;
            text = std::format("{} by the attacker on the holder in {}'s war: its skill cast", row.codename, spec.codename);
        } else if (row.typeId[2] == 3 && kind == 11) {
            text = std::format("{} in {} outside the war: its unique at the reader's level", row.codename, spec.codename);
        } else {
            // A flag, a guard or a tablet's monster where the user stands.
            if (here) {
                m.region = here->region;
                m.x = here->x;
                m.y = here->y;
                m.z = here->z;
            }
            text = !groupTaken ? std::format("{}: no fortress of 1.188 takes its group {} (_RefSiegeFortressGuard): refused 0x18E7", row.codename, group)
                               : std::format("{} placed at {} by its holder ({})", row.codename, spec.codename, fr->time == 1 ? "in the war" : "outside the war");
        }
        // Whole again just before (the holder's guards and towers shoot an attacker beside its heart in the war).
        if (bomb) {
            keeper->c->Send(proto::DevReset{proto::DevReset::kVitals});
            keeper->c->Pump(50ms);
        }
        t.c->Send(proto::DevReset{proto::DevReset::kVitals});
        const std::size_t mark = t.c->Mark();
        t.c->Send(m);
        const auto answer = t.c->Wait<proto::FortressAnswer>(mark, [](const proto::FortressAnswer& a) { return a.action == proto::FortressAction::UseItem; }, 3s);
        t.c->Pump(150ms);
        t.Sync();
        seen["answer"] = answer ? json{{"ok", answer->ok}, {"error", answer->error}} : json(nullptr);
        seen["left"] = t.Count(row.refObjId);
        // A summon scroll's unique: seen, then felled (it has no owner: it would fight the next checks' players).
        if (row.typeId[2] == 3 && kind == 11) {
            std::uint32_t unique = 0;
            for (const auto& [at, e] : t.c->Since<proto::EntityAppeared>(mark)) {
                if (e.kind == proto::EntityKind::Monster) unique = e.entity;
            }
            seen["unique"] = unique;
            if (answer && answer->ok && unique == 0) failed.push_back("no unique appeared");
            if (unique != 0) {
                t.c->Send(proto::DevStrike{unique, 2'000'000'000u});
                t.c->Pump(200ms);
            }
        }
        const int scenario = groupTaken ? 1 : 2;
        if (groupTaken) {
            if (!answer || !answer->ok) failed.push_back(std::format("refused {}", answer ? std::format("0x{:X}", answer->error) : std::string("(no answer)")));
            if (t.Count(row.refObjId) != 0) failed.push_back("not used up");
        } else {
            if (!answer || answer->ok || answer->error != 0x18E7) failed.push_back(std::format("not refused 0x18E7 ({})", answer ? std::format("0x{:X}", answer->error) : std::string("no answer")));
            if (t.Count(row.refObjId) != 1) failed.push_back("taken");
        }
        Reset(t, proto::DevReset::kAll);
        return Verdict(scenario, "fortress", failed, text, {{"fortress", spec.codename}}, seen);
    }

    // --- The trade's goods: bought at their specialty NPC into a trader's transport, sold at another's --
    // The trader in his suit (put on in town) with a transport out at the exam's place, the specialty NPCs beside it.
    std::uint32_t cart_ = 0;
    Tester* Trader(std::string& error, std::string& why) {
        Tester* t = Get(0, true, kTrader, error);
        if (t == nullptr) return (why = "no trader"), nullptr;
        if (cart_ != 0) return t;
        const assets::ItemRow* suit = nullptr;
        const assets::ItemRow* scroll = nullptr;
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            const assets::ItemRow& r = *e.row;
            if (suit == nullptr && e.routing.route == ItemRoute::JobSuit && game::SuitJob(r) == game::JobType::Trader && RaceFor(r) == 0 && MaleFor(r) &&
                LevelOf(r) <= kHighLevel) {
                suit = &r;
            }
            if (scroll == nullptr && e.routing.route == ItemRoute::CosSummon && r.codename.starts_with("ITEM_COS_T_") && LevelOf(r) <= kHighLevel) scroll = &r;
        }
        if (suit == nullptr || scroll == nullptr) return (why = "no trader's suit or transport scroll"), nullptr;
        Reset(*t, proto::DevReset::kAll);
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DevGoto{1u}); // GATE_CH: the suit is put on in a town
        if (!t->c->Wait<proto::WorldEntered>(mark, [&](const proto::WorldEntered& w) { return w.entity == t->entity; }, 8s)) return (why = "the town not reached"), nullptr;
        t->c->Pump(100ms);
        mark = t->c->Mark();
        t->c->Send(proto::DevGive{suit->codename, 1, true});
        const auto worn = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) {
            return m.to == game::slot::kJobSuit ? m.status != proto::ItemStatus::JobChange : m.from >= game::slot::kWorn && m.status != proto::ItemStatus::Ok;
        }, 4s);
        if (!worn || worn->status != proto::ItemStatus::Ok) return (why = "the trader's suit not worn"), nullptr;
        t->c->Pump(500ms);
        Give(*t, *scroll);
        t->Sync();
        mark = t->c->Mark();
        t->c->Send(proto::UseItem{*SlotOf(*t, scroll->refObjId)});
        const auto summoned = t->c->Wait<proto::CosSummoned>(mark, [](const proto::CosSummoned&) { return true; }, 3s);
        t->c->Pump(300ms);
        t->Sync();
        if (!summoned) return (why = "no transport summoned"), nullptr;
        cart_ = summoned->entity;
        return t;
    }
    Result TradeGood(const assets::ItemRow& row, std::string& error) {
        std::vector<std::string> failed;
        json seen = json::object();
        // The events' foods (3/3/8/0): goods to the server, but no specialty NPC sells them (no _RefShopGoods row): they never
        // ride in a transport; in the bag they are never used.
        std::string seller;
        int tab = -1, slot = -1;
        if (data_.shops) {
            for (const auto& [code, ref] : data_.specials) {
                const assets::NpcShop* shop = data_.shops->Find(code);
                if (shop == nullptr) continue;
                for (const assets::ShopTabGroup& g : shop->groups) {
                    for (const assets::ShopTab& tb : g.tabs) {
                        for (const assets::ShopGood& good : tb.goods) {
                            if (good.itemCodename == row.codename && (seller.empty() || game::TradeQuestOf(seller).size() > game::TradeQuestOf(code).size())) {
                                seller = code;
                                tab = tb.index;
                                slot = good.slot;
                            }
                        }
                    }
                }
            }
        }
        if (seller.empty()) {
            Tester* t = Get(0, true, true, error);
            if (t == nullptr) return Skip(2, "trade", "no player");
            Reset(*t, proto::DevReset::kAll);
            if (!Give(*t, row)) return Verdict(2, "trade", {"not given (DevGive)"}, "", {}, {});
            t->Sync();
            const UseSeen u = UseOnce(*t, row);
            seen["use"] = Seen(u);
            if (u.used || u.left != 1) failed.push_back("used from the bag");
            Reset(*t, proto::DevReset::kAll);
            return Verdict(2, "trade", failed, "no specialty NPC sells it (no _RefShopGoods row): it never rides in a transport; in the bag never used", {}, seen);
        }
        std::string why;
        Tester* t = Trader(error, why);
        if (t == nullptr) return Skip(1, "trade", why);
        t->c->Send(proto::DevReset{std::uint8_t(proto::DevReset::kVitals | proto::DevReset::kWaits)});
        t->c->Pump(100ms);
        const auto npc = t->models.find(data_.specials.at(seller));
        if (npc == t->models.end()) return Skip(1, "trade", seller + " not in sight");
        seen["seller"] = seller;
        // Bought: one into the transport's bag.
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::ShopBuy{npc->second, std::uint8_t(tab), std::uint8_t(slot), 1});
        const auto bought = t->c->Wait<proto::TradeAnswer>(mark, [](const proto::TradeAnswer& a) { return a.request == proto::TradeRequest::Buy; }, 3s);
        seen["buy"] = bought ? json{{"code", std::uint16_t(bought->code)}, {"gold", bought->gold}} : json(nullptr);
        const std::string_view quest = game::TradeQuestOf(seller);
        if (!bought || bought->code != proto::TradeCode::Ok) {
            failed.push_back(std::format("not bought at {} ({}){}", seller, bought ? std::format("0x{:X}", std::uint16_t(bought->code)) : std::string("no answer"),
                                         quest.empty() ? "" : std::format(": its NPC asks {} done, a native quest not built yet", quest)));
            return Verdict(1, "trade", failed, "", {{"seller", seller}}, seen);
        }
        // Sold: at another town's specialty NPC (one asking no quest), all the transport carries.
        std::string buyer;
        for (const auto& [code, ref] : data_.specials) {
            if (code != seller && game::TradeQuestOf(code).empty() && code.find("_SD_") == std::string::npos && t->models.contains(ref)) {
                buyer = code;
                break;
            }
        }
        if (buyer.empty()) return Skip(1, "trade", "no other specialty NPC in sight");
        mark = t->c->Mark();
        t->c->Send(proto::TradeSellAll{t->models.at(data_.specials.at(buyer))});
        const auto sold = t->c->Wait<proto::TradeAnswer>(mark, [](const proto::TradeAnswer& a) { return a.request == proto::TradeRequest::Sell; }, 3s);
        seen["sell"] = sold ? json{{"code", std::uint16_t(sold->code)}, {"count", sold->count}, {"gold", sold->gold}} : json(nullptr);
        seen["buyer"] = buyer;
        if (!sold || sold->code != proto::TradeCode::Ok || sold->count < 1) {
            failed.push_back(std::format("not sold at {} ({})", buyer, sold ? std::format("0x{:X}", std::uint16_t(sold->code)) : std::string("no answer")));
        }
        return Verdict(1, "trade", failed,
                       std::format("bought at {} into the trader's transport{}, sold at {}", seller, game::IsBargainGood(row) ? " (in the bargain hours)" : "", buyer),
                       {{"seller", seller}, {"buyer", buyer}}, seen);
    }

    // The first plain degree-1 item of the catalogue that `fits`.
    template <typename F>
    const assets::ItemRow* FirstPlain(F fits) const {
        for (const game::ItemCatalogueEntry& e : data_.catalogue) {
            if (e.routing.route == ItemRoute::Equip && e.row->itemClass == 1 && !e.row->Rare() && fits(*e.row)) return e.row;
        }
        return nullptr;
    }
    bool IsShieldSkillTrigger(const assets::ItemRow& row) const {
        if (game::KindOf(row) != game::ItemKind::Shield || row.itemClass != 1 || row.Rare()) return false;
        return FirstPlain([&](const assets::ItemRow& r) { return game::KindOf(r) == game::ItemKind::Shield && r.country == row.country; }) == &row;
    }
    // Its country's shield skill (the Chinese sword's SKILL_CH_SWORD_SHIELD_A_01, the European warrior's
    // SKILL_EU_WARRIOR_ONEHANDA_SHIELD_A_01: reqi 4 1 / 4 2) with a one-handed weapon in hand: refused without the shield
    // (SkillUseStatus::Weapon), a buff with it, the buff ended when the shield is taken off (EndUnfitBuffs from the move).
    Result ShieldSkill(const assets::ItemRow& shield, std::string& error) {
        const bool china = shield.country == 0;
        const assets::SkillRow* skill = data_.skills->Find(std::string_view(china ? "SKILL_CH_SWORD_SHIELD_A_01" : "SKILL_EU_WARRIOR_ONEHANDA_SHIELD_A_01"));
        const assets::ItemRow* weapon = FirstPlain([&](const assets::ItemRow& r) {
            return game::KindOf(r) == game::ItemKind::Weapon && r.country == shield.country && !game::TwoHanded(r) && r.typeId[3] == (china ? 2 : 7);
        });
        if (skill == nullptr || weapon == nullptr) return Skip(1, "shield skill", "no shield skill or one-handed weapon in the data");
        Tester* t = Get(china ? 0 : 1, true, true, error);
        if (t == nullptr) return Skip(1, "shield skill", "no player");
        Reset(*t, proto::DevReset::kAll);
        const auto wear = [&](const assets::ItemRow& r, int place) {
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::DevGive{r.codename, 1, true});
            const auto m = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& x) { return x.from >= game::slot::kWorn; }, 3s);
            return m && m->status == proto::ItemStatus::Ok && m->to == place;
        };
        const auto off = [&](int place) {
            std::uint8_t free = game::slot::kWorn;
            t->Sync();
            while (t->places.contains(free)) ++free;
            const std::size_t mark = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(place), free, 0});
            t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& x) { return x.from == place; }, 2s);
        };
        std::vector<std::string> failed;
        json observed = json::object();
        if (!wear(*weapon, game::slot::kWeapon)) failed.push_back(weapon->codename + " not worn");
        // Without the shield: refused.
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::UseSkill{skill->id, t->entity});
        const auto refused = t->c->Wait<proto::SkillUseAnswer>(mark, [&](const proto::SkillUseAnswer& a) { return a.skill == skill->id; }, 2s);
        observed["withoutShield"] = refused ? json(int(refused->status)) : json(nullptr);
        if (!refused || refused->status != proto::SkillUseStatus::Weapon) failed.push_back("not refused without the shield");
        // With it: the buff (China's); Europe's two shield skills are attacks on an enemy (Target_Required): no longer
        // refused for the weapon (the cast on itself refused for the target instead).
        if (!wear(shield, game::slot::kShield)) failed.push_back(shield.codename + " not worn");
        t->c->Send(proto::DevReset{proto::DevReset::kWaits});
        t->c->Pump(40ms);
        mark = t->c->Mark();
        t->c->Send(proto::UseSkill{skill->id, t->entity});
        std::optional<proto::BuffAdded> added;
        if (skill->targetRequired) {
            const auto answer = t->c->Wait<proto::SkillUseAnswer>(mark, [&](const proto::SkillUseAnswer& a) { return a.skill == skill->id; }, 2s);
            observed["withShield"] = answer ? json(int(answer->status)) : json(nullptr);
            if (answer && answer->status == proto::SkillUseStatus::Weapon) failed.push_back("refused for the weapon with the shield worn");
        } else {
            added = t->c->Wait<proto::BuffAdded>(mark, [&](const proto::BuffAdded& b) { return b.skill == skill->id; }, 6s);
            observed["buff"] = added.has_value();
            if (!added) failed.push_back("no buff with the shield");
        }
        // The shield taken off: the buff ends.
        if (added) {
            mark = t->c->Mark();
            off(game::slot::kShield);
            const auto removed = t->c->Wait<proto::BuffRemoved>(mark, [&](const proto::BuffRemoved& b) { return b.token == added->token; }, 2s);
            observed["endedWithTheShield"] = removed.has_value();
            if (!removed) failed.push_back("the buff stayed with the shield taken off");
        }
        off(game::slot::kShield);
        off(game::slot::kWeapon);
        Reset(*t, proto::DevReset::kAll);
        return Verdict(1, "shield skill", failed,
                       skill->targetRequired ? std::format("{}: refused for the weapon without a shield, not with it", skill->codename)
                                             : std::format("{}: refused without a shield, a buff with it, ended when it comes off", skill->codename),
                       {{"skill", skill->codename}, {"weapon", weapon->codename}}, observed);
    }

    Result Harp(const assets::ItemRow& row, int place, int race, bool male, std::string& error) {
        Tester* t = Get(race, male, true, error);
        if (t == nullptr) return Skip(1, "harp", "no player");
        Reset(*t, proto::DevReset::kBag);
        const std::map<std::string, float> before = Sheet(*t);
        std::size_t mark = t->c->Mark();
        t->c->Send(proto::DevGive{row.codename, 1, true});
        const auto moved = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from >= game::slot::kWorn; }, 3s);
        t->c->Pump(60ms);
        const std::map<std::string, float> after = Sheet(*t);
        if (moved && moved->status == proto::ItemStatus::Ok) {
            mark = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(place), std::uint8_t(game::slot::kWorn), 0});
            t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == place; }, 2s);
        }
        Reset(*t, proto::DevReset::kBag);
        const double was = before.contains("maxMp") ? before.at("maxMp") : -1, now = after.contains("maxMp") ? after.at("maxMp") : -1;
        std::vector<std::string> failed;
        if (!moved || moved->status != proto::ItemStatus::Ok) failed.push_back("not worn");
        else if (std::abs(now - std::trunc(was * 1.5)) > 1.0) failed.push_back(std::format("its most mana {:.0f} to {:.0f}, {:.0f} expected", was, now, std::trunc(was * 1.5)));
        return Verdict(1, "harp", failed, "its bearer's most mana +50 %", {{"maxMp", std::trunc(was * 1.5)}}, {{"maxMp", {was, now}}});
    }

    Result Blues(const assets::ItemRow& row, int place, int race, bool male, std::string& error) {
        const int degree = std::max(1, (row.itemClass - 1) / 3 + 1);
        const assets::MagicOptionRow* str = OptionFor("MATTR_STR", degree);
        const assets::MagicOptionRow* intel = OptionFor("MATTR_INT", degree);
        if (str == nullptr || intel == nullptr) return Skip(1, "blues", std::format("no STR or INT blue of degree {}", degree));
        Tester* t = Get(race, male, true, error);
        if (t == nullptr) return Skip(1, "blues", "no player");
        Reset(*t, proto::DevReset::kBag);
        const std::map<std::string, float> before = Sheet(*t);
        constexpr std::uint32_t kStr = 5, kInt = 3;
        proto::DevGive give{row.codename, 1, true};
        give.magic = {std::uint64_t(str->paramId) | (std::uint64_t(kStr) << 32), std::uint64_t(intel->paramId) | (std::uint64_t(kInt) << 32)};
        std::size_t mark = t->c->Mark();
        t->c->Send(give);
        const auto moved = t->c->Wait<proto::ItemMoved>(mark, [](const proto::ItemMoved& m) { return m.from >= game::slot::kWorn; }, 3s);
        t->c->Pump(60ms);
        const std::map<std::string, float> after = Sheet(*t);
        t->Sync();
        if (moved && moved->status == proto::ItemStatus::Ok) {
            mark = t->c->Mark();
            t->c->Send(proto::ItemMove{std::uint8_t(place), std::uint8_t(game::slot::kWorn), 0});
            t->c->Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == place; }, 2s);
        }
        Reset(*t, proto::DevReset::kBag);
        const auto at = [](const std::map<std::string, float>& m, const char* k) {
            const auto it = m.find(k);
            return it != m.end() ? double(it->second) : -1.0;
        };
        std::vector<std::string> failed;
        if (!moved || moved->status != proto::ItemStatus::Ok) failed.push_back("not worn");
        const double dStr = at(after, "strength") - at(before, "strength"), dInt = at(after, "intelligence") - at(before, "intelligence");
        if (std::abs(dStr - kStr) > 0.01) failed.push_back(std::format("STR +{:.0f}, +{} expected", dStr, kStr));
        if (std::abs(dInt - kInt) > 0.01) failed.push_back(std::format("INT +{:.0f}, +{} expected", dInt, kInt));
        if (at(after, "maxHp") <= at(before, "maxHp")) failed.push_back("its health did not grow with its STR");
        return Verdict(1, "blues", failed, std::format("STR +{} and INT +{} from its blues ({}, {})", kStr, kInt, str->optionName, intel->optionName),
                       {{"strength", kStr}, {"intelligence", kInt}, {"degree", degree}},
                       {{"strength", {at(before, "strength"), at(after, "strength")}},
                        {"intelligence", {at(before, "intelligence"), at(after, "intelligence")}},
                        {"maxHp", {at(before, "maxHp"), at(after, "maxHp")}},
                        {"maxMp", {at(before, "maxMp"), at(after, "maxMp")}}});
    }

    // An avatar given and put on (AvatarMove): the answer; then taken off again. `dress`: a dress put on first (an
    // attachment's).
    std::optional<proto::AvatarMoved> WearAvatar(Tester& t, const assets::ItemRow& row, int place, const assets::ItemRow* dress,
                                                 std::optional<std::uint16_t>* refusal = nullptr, std::uint32_t data = 0) {
        Reset(t, proto::DevReset::kBag);
        const auto on = [&](const assets::ItemRow& r) -> std::optional<proto::AvatarMoved> {
            const auto slot = Give(t, r, 1, &r == &row ? data : 0);
            if (!slot) return std::nullopt;
            const std::size_t mark = t.c->Mark();
            t.c->Send(proto::AvatarMove{true, *slot});
            const auto moved = t.c->Next<proto::AvatarMoved>(mark, 2s);
            t.Sync();
            if (refusal != nullptr && &r == &row) {
                if (const auto code = RefusedSince(t, mark, r.refObjId)) *refusal = code->code;
            }
            return moved;
        };
        const auto off = [&](int p) {
            const std::size_t mark = t.c->Mark();
            t.c->Send(proto::AvatarMove{false, std::uint8_t(p)});
            t.c->Next<proto::AvatarMoved>(mark, 2s);
            t.Sync();
        };
        bool dressed = false;
        if (dress != nullptr) {
            const auto d = on(*dress);
            dressed = d && d->status == proto::ItemStatus::Ok;
        }
        const auto moved = on(row);
        if (moved && moved->status == proto::ItemStatus::Ok) off(place);
        if (dressed) off(game::avatar::kDress);
        Reset(t, proto::DevReset::kBag);
        return moved;
    }

    std::vector<Result> Avatar(const assets::ItemRow& row, std::string& error) {
        const int place = game::AvatarPlaceFor(row);
        if (place < 0) return {Verdict(1, "wear", {"no avatar place for it (AvatarPlaceFor)"}, "", {}, {})};
        const int level = LevelOf(row);
        const int race = RaceFor(row);
        const bool male = MaleFor(row);
        // An attachment goes on a dress: one that fits the same player.
        const assets::ItemRow* dress = nullptr;
        if (place == game::avatar::kAttachment) {
            for (const game::ItemCatalogueEntry& e : data_.catalogue) {
                const assets::ItemRow& d = *e.row;
                if (e.routing.route == ItemRoute::Avatar && game::AvatarPlaceFor(d) == game::avatar::kDress && RaceFor(d) == race && (d.country == row.country || d.country == 3) &&
                    (d.sex == 2 || d.sex == row.sex || row.sex == 2) && MaleFor(d) == male && LevelOf(d) <= kHighLevel) {
                    dress = &d;
                    break;
                }
            }
        }
        std::vector<Result> out;
        const auto check = [&](int scenario, const char* variant, int r, bool m, bool high, proto::ItemStatus want, const assets::ItemRow* withDress, std::string text,
                               std::uint16_t code = 0, std::uint32_t data = 0) {
            Tester* t = Get(r, m, high, error);
            if (t == nullptr) return;
            std::optional<std::uint16_t> refusal;
            const auto moved = WearAvatar(*t, row, place, withDress, &refusal, data);
            std::vector<std::string> failed;
            if (code != 0 && refusal != code) failed.push_back(std::format("refused with {}, 0x{:X} expected", refusal ? std::format("0x{:X}", *refusal) : "no code", code));
            if (!moved) failed.push_back("no answer to the move");
            else if (moved->status != want) failed.push_back(std::format("its move answered {}, {} expected", int(moved->status), int(want)));
            else if (want == proto::ItemStatus::Ok &&
                     !std::any_of(moved->avatars.begin(), moved->avatars.end(), [&](const proto::ItemRecord& a) { return a.slot == place && a.item == row.refObjId; })) {
                failed.push_back(std::format("not in avatar place {}", place));
            }
            out.push_back(Verdict(scenario, variant, failed, std::move(text), {{"status", int(want)}, {"place", place}, {"dress", withDress ? json(withDress->codename) : json(nullptr)}},
                                  {{"status", moved ? json(int(moved->status)) : json(nullptr)}}));
        };
        // A Devil's Spirit goes on only while awake (0x18F9, checked first); the mall's
        // purchase awakens it, DevGive's is asleep.
        if (game::IsDevilSpirit(row)) {
            // Awake: its data word the awakening's end (Unix seconds), a day from now.
            const auto day = std::uint32_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 86400);
            check(1, "wear", race, male, true, proto::ItemStatus::Ok, nullptr, "worn while awake (its awakening a day from now)", 0, day);
            check(2, "asleep", race, male, true, proto::ItemStatus::Failed, nullptr, "refused while asleep (0x18F9)", 0x18F9);
            out.push_back(SpiritPower(row, race, male, day, error));
            out.push_back(Grant(row, race, male, error));
            return out;
        }
        if (level > kHighLevel) {
            out.push_back(Skip(1, "wear", std::format("it asks level {}, above the cap {}", level, kHighLevel)));
        } else if (place == game::avatar::kAttachment && dress == nullptr) {
            out.push_back(Skip(1, "wear", "no dress for it to go on"));
        } else {
            check(1, "wear", race, male, true, proto::ItemStatus::Ok, dress, std::format("worn in avatar place {}{}", place, dress ? " on a dress" : ""));
            if (place == game::avatar::kDress) out.push_back(AvatarBlues(row, race, male, error));
        }
        if (place == game::avatar::kAttachment) check(2, "no dress", race, male, true, proto::ItemStatus::NeedsDress, nullptr, "refused with no dress worn");
        out.push_back(Grant(row, race, male, error));
        if (level > 1) check(2, "level", race, male, false, proto::ItemStatus::Level, dress, std::format("refused at level 1 (it asks {})", level));
        if (row.country == 0 || row.country == 1) check(2, "race", 1 - row.country, male, true, proto::ItemStatus::Race, nullptr, "refused to the other country");
        if (row.sex == 0 || row.sex == 1) check(2, "sex", race, !male, true, proto::ItemStatus::Sex, nullptr, std::format("refused to a {}", male ? "woman" : "man"));
        return out;
    }

    // --- The results ------------------------------------------------------------------------------------------------

    void Write(const game::ItemCatalogueEntry& e, const Result& r) {
        if (r.status == "pass") ++report_.passed;
        else if (r.status == "skip") ++report_.skipped;
        else ++report_.failed;
        core::LogInfo("SroSkillLab: {} {} ({} {}): {} {}", game::ItemRouteName(e.routing.route), e.row->codename, r.scenario, r.variant, r.status, r.summary);
        results_.Query("INSERT INTO sro_vt_account.sr_lab_results (run_id, code, scenario, variant, status, summary, expected, observed) "
                       "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb)",
                       {report_.run, e.row->codename, r.scenario, r.variant, r.status, r.summary, r.expected.dump(), r.observed.dump()});
    }

    const ItemExamOptions& options_;
    ItemExamReport& report_;
    const Data& data_;
    Database& results_;
    Database& exam_;
    net::NetService& net_;
    std::uint16_t port_;
    std::string pem_;
    std::function<void()> pump_;
    std::map<std::tuple<int, bool, int>, std::unique_ptr<Tester>> testers_; // (race, male, Profile)
    std::vector<OptionalTeleportRow> points_;
    int made_ = 0;
};

// --- The exam's process: its database, its game server, its workers ---------------------------------------------------

class Exam {
public:
    Exam(const ItemExamOptions& o, ItemExamReport& r) : options_(o), report_(r) {}

    bool Run(std::string& error) {
        if (!assets::MountGameArchives(files_, options_.gameDir, error) || !data_.Load(files_, error)) return false;
        results_ = Database::Connect(options_.resultsUrl, error);
        if (!results_ || !MigrateSchema(*results_, error)) return false;
        if (options_.port != 0) return RunWorker(error);
        if (!OpenDatabase(error)) return false;
        if (!PrepareFortress(error)) return false;
        bool ok = StartServer(error);
        if (ok) ok = options_.workers > 1 ? RunWorkers(error) : RunHere(error);
        Finish();
        return ok;
    }

private:
    std::string Filter() const {
        std::string filter = "items";
        for (const std::string& r : options_.routes) filter += (filter == "items" ? ": " : ",") + r;
        if (!options_.item.empty()) filter += " " + options_.item;
        if (options_.limit > 0) filter += std::format(" (at most {} a path)", options_.limit);
        return filter;
    }

    bool OpenDatabase(std::string& error) {
        name_ = std::format("sro_lab_items_{}", std::chrono::system_clock::now().time_since_epoch().count() % 1000000000);
        const QueryResult made = results_->Query("CREATE DATABASE " + name_);
        if (!made.Ok()) return (error = "the exam's database: " + made.Error()), false;
        exam_ = Database::Connect(WithDatabase(options_.resultsUrl, name_), error);
        if (!exam_ || !MigrateSchema(*exam_, error)) return false;
        // The original's reference tables (the Forgotten World's worlds and talismans, the arena's worlds, the reverse return's
        // map points, ...): the operator's own shard backup when it is here (data\SRO_VT_SHARD.bak, as SroSetup reads it),
        // else an export of it made through SQL Server (SR_VSRO_EXPORT; SroDb --import).
        const std::filesystem::path backupFile = "data/SRO_VT_SHARD.bak";
        const char* exportEnv = std::getenv("SR_VSRO_EXPORT");
        const std::filesystem::path exportDir = exportEnv != nullptr ? exportEnv : "";
        const bool haveBackup = std::filesystem::exists(backupFile);
        if (haveBackup || (!exportDir.empty() && std::filesystem::exists(exportDir / "SRO_VT_SHARD" / "schema.tsv"))) {
            ImportReport ir;
            if (haveBackup) {
                SqlServerBackup backup;
                if (!backup.Open(backupFile, error)) return (error = "the shard backup: " + error), false;
                BackupImportOptions bo;
                bo.replace = true;
                if (!ImportBackup(*exam_, backup, bo, ir, error)) return (error = "the reference tables: " + error), false;
            } else {
                ImportOptions io;
                io.dir = exportDir;
                io.databases = {"SRO_VT_SHARD"};
                io.replace = true;
                if (!ImportDatabases(*exam_, io, ir, error)) return (error = "the reference tables: " + error), false;
            }
            core::LogInfo("SroSkillLab: the exam's database has the original's shard tables ({})",
                          haveBackup ? backupFile.string() : exportDir.string());
            // The quests (the script's and the ones the game server codes) into sr_quests, as SroSetup writes them.
            const std::filesystem::path script = "data/Quest.sct";
            if (std::ifstream in(script, std::ios::binary); in) {
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                QuestImportReport qr;
                std::string why;
                if (!ImportQuestScript(*exam_, bytes, qr, why)) core::LogWarning("SroSkillLab: the quests: {}", why);
            }
        } else {
        // Without them: the reverse return scroll's map points (_RefOptionalTeleport) copied from the results' database.
        exam_->Query("CREATE TABLE IF NOT EXISTS sro_vt_shard._refoptionalteleport (service integer NOT NULL, id integer PRIMARY KEY, regionid smallint NOT NULL, "
                     "pos_x smallint NOT NULL, pos_y smallint NOT NULL, pos_z smallint NOT NULL, levelmin smallint NOT NULL, levelmax smallint NOT NULL)");
        const QueryResult points = results_->Query("SELECT service, id, regionid, pos_x, pos_y, pos_z, levelmin, levelmax FROM sro_vt_shard._refoptionalteleport");
        for (int i = 0; points.Ok() && i < points.Rows(); ++i) {
            exam_->Query("INSERT INTO sro_vt_shard._refoptionalteleport (service, id, regionid, pos_x, pos_y, pos_z, levelmin, levelmax) VALUES ($1, $2, $3, $4, $5, $6, $7, $8) "
                         "ON CONFLICT (id) DO NOTHING",
                         {points.Int(i, 0), points.Int(i, 1), points.Int(i, 2), points.Int(i, 3), points.Int(i, 4), points.Int(i, 5), points.Int(i, 6), points.Int(i, 7)});
        }
        }
        const QueryResult run = results_->Query("INSERT INTO sro_vt_account.sr_lab_runs (build, filter, scenarios) VALUES ($1, $2, '1,2') RETURNING id", {options_.build, Filter()});
        if (!run.Ok() || run.Rows() != 1) return (error = "the run: " + run.Error()), false;
        report_.run = run.Int(0, 0);
        return true;
    }

    // The fortress war's two sides (as FortressTests' rig): a holder of every fortress and an
    // attacker applied against each, both guilds of level 5 with six members more, their masters Commanders; made in the
    // database before the server starts (it reads the guilds and the fortresses then).
    bool PrepareFortress(std::string& error) {
        const std::int64_t firstGuild = exam_->Query("SELECT coalesce(max(id), 0) + 1000 FROM _guild", {}).Int(0, 0);
        const std::uint32_t model = kFigure[0][1];
        std::int64_t guild[2] = {firstGuild, firstGuild + 1};
        for (int side = 0; side < 2; ++side) {
            const std::string account = side == 0 ? "itemexamkeeper" : "itemexamraider"; // as LoadShared names them
            const std::string name = side == 0 ? "IxCKeeper" : "IxCRaider";
            if (CreateAccount(*exam_, account, "itemexam-secret", error) != CreateAccountResult::Created) return (error = "the account " + account + ": " + error), false;
            bool ok = false;
            const auto row = FindAccount(*exam_, account, ok);
            if (!row) return (error = "the account " + account), false;
            NewCharacter nc;
            nc.name = name;
            nc.model = model;
            nc.strength = 20;
            nc.intelligence = 20;
            nc.hp = 200;
            nc.mp = 200;
            nc.level = 1;
            if (CreateCharacter(*exam_, row->id, kServer, nc, 4, error) != CreateCharacterResult::Created) return (error = "the character " + name + ": " + error), false;
            const std::int64_t character = exam_->Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Int(0, 0);
            const std::string guildName = std::format("Ix{}{}", options_.part, side == 0 ? "Keepers" : "Raiders");
            exam_->Query("INSERT INTO _guild (id, name, lvl, gatheredsp, foundationdate, alliance, booty) OVERRIDING SYSTEM VALUE VALUES ($1, $2, 5, 0, now(), 0, 0)",
                         {guild[side], guildName});
            const auto add = [&](std::int64_t ch, const std::string& n, int memberClass) {
                exam_->Query("INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission, siegeauthority) "
                             "VALUES ($1, $2, $3, $4, 30, 0, now(), $5, $6)",
                             {guild[side], ch, n, memberClass, memberClass == 0 ? -1 : 0, memberClass == 0 ? 1 : 0});
            };
            add(character, name, 0);
            exam_->Query("UPDATE _char SET guildid = $1 WHERE charid = $2", {guild[side], character});
            for (int i = 0; i < 6; ++i) add(9'000'000 + guild[side] * 10 + i, guildName + std::to_string(i), 10);
        }
        exam_->Query("DELETE FROM _siegefortressrequest", {});
        exam_->Query("DELETE FROM _siegefortressobject", {});
        exam_->Query("UPDATE _siegefortress SET guildid = $1, tempguildid = 0, npchired = 0", {guild[0]});
        exam_->Query("UPDATE _siegefortressstruct SET ownerguildid = $1", {guild[0]});
        for (const std::int64_t f : {1, 3, 6}) exam_->Query("INSERT INTO _siegefortressrequest (fortressid, guildid, requesttype) VALUES ($1, $2, 0)", {f, guild[1]});
        core::LogInfo("SroSkillLab: the fortresses held by guild {}, applied against by guild {}", guild[0], guild[1]);
        return true;
    }

    // The data the checks share with the server's setup (the trade's shops and NPCs, the fortresses, Magic POP's tables, the
    // prepared holder and attacker): the server's process fills config_ with it too, a worker only data_.
    void LoadShared() {
        // The trade: the merchants' shops, every specialty NPC in a ring beside the exam's place (their
        // markets load for the NPCs that stand here), the bargain goods' hours all day.
        {
            std::string why;
            auto shops = std::make_shared<assets::ShopTable>();
            if (shops->Load(files_, why)) {
                config_.shops = shops;
                data_.shops = shops;
            } else {
                core::LogWarning("SroSkillLab: shops: {}", why);
            }
            const char* specials[] = {"NPC_CH_SPECIAL", "NPC_CH_SPECIAL2", "NPC_WC_SPECIAL", "NPC_WC_SPECIAL2", "NPC_KT_SPECIAL",
                                      "NPC_EU_SPECIAL", "NPC_CA_SPECIAL", "NPC_TK_SPECIAL", "NPC_RM_SPECIAL", "NPC_AM_SPECIAL",
                                      "NPC_SD_M_AREA_SPECIAL", "NPC_SD_M_AREA_SPECIAL2", "NPC_SD_M_AREA_SPECIAL3"};
            // Around Jangan's gate (teleport point 1, GATE_CH): the trader's suit is put on in a town and his transport stays by him.
            const assets::TeleportPoint* gate = data_.teleports ? data_.teleports->Point(1) : nullptr;
            const proto::WorldPosition ring = gate != nullptr && gate->HasArrival() ? proto::WorldPosition{gate->region, gate->x, gate->y, gate->z} : kPlace;
            int i = 0;
            for (const char* code : specials) {
                const assets::ObjectRow* npc = data_.characters->Find(std::string_view(code));
                if (npc == nullptr) continue;
                const double a = 2.0 * 3.14159265 * double(i++) / double(std::size(specials));
                config_.npcs.push_back({npc->refObjId, code, {ring.region, ring.x + float(15.0 * std::cos(a)), ring.y, ring.z + float(15.0 * std::sin(a))}, 0.0f});
                data_.specials[code] = npc->refObjId;
            }
            game::ScheduleRow allDay;
            allDay.define = game::kScheduleSpecialGoods;
            allDay.dateStart = 0;
            allDay.dateEnd = 4102444800; // 2100
            allDay.mainType = 1;
            allDay.durationSeconds = 86400;
            config_.bargainSchedule = {allDay};
        }
        // The fortress war (as SroGame loads it; no schedule: the periods are the exam's developer switches).
        {
            std::string why;
            LoadBattleRanks(*exam_, config_.fortress.battleRanks, why);
            LoadFortressItems(*exam_, config_.fortress.items, why);
            LoadFortressGuards(*exam_, config_.fortress.guards, why);
            LoadSiegeSummons(*exam_, config_.fortress.summons, why);
            LoadBlessBuffs(*exam_, config_.fortress.blesses, why);
            LoadForgeReference(*exam_, config_.fortress.forges, why);
            LoadStructureReferences(*exam_, config_.fortress.structureReferences, why);
            if (LoadFortressSpecs(*exam_, files_, *data_.characters, config_.fortresses, why)) {
                data_.fortresses = config_.fortresses;
                data_.fortItems = config_.fortress.items;
                data_.fortGuards = config_.fortress.guards;
                core::LogInfo("SroSkillLab: {} fortresses, {} fortress items", config_.fortresses.size(), config_.fortress.items.size());
            } else {
                core::LogWarning("SroSkillLab: the fortresses: {}", why);
                config_.fortresses.clear();
            }
        }
        // Magic POP: its sets and machines from the database's tables as SroGame mounts them
        // (DatabaseText), its first machine and Gori beside the exam's place.
        {
            server::DatabaseTextCounts counts;
            std::string why;
            if (auto text = server::MakeDatabaseText(*exam_, files_, counts, why)) {
                popFiles_.Mount(std::move(text), assets::FileSystem::kOverridePriority);
                auto gacha = std::make_shared<assets::GachaTable>();
                if (gacha->Load(popFiles_, why, false) && !gacha->machines().empty()) {
                    config_.gacha = gacha;
                    data_.gacha = gacha;
                    const assets::ObjectRow* machine = data_.characters->Find(gacha->machines()[0].npc);
                    const assets::ObjectRow* gori = data_.characters->Find(std::string_view("NPC_CH_GACHA_OPERATOR"));
                    if (machine != nullptr) config_.npcs.push_back({machine->refObjId, machine->codename, {kPlace.region, kPlace.x + 14.0f, kPlace.y, kPlace.z}, 0.0f});
                    if (gori != nullptr) config_.npcs.push_back({gori->refObjId, gori->codename, {kPlace.region, kPlace.x + 16.0f, kPlace.y, kPlace.z}, 0.0f});
                    data_.popMachine = machine ? machine->refObjId : 0;
                    data_.popGori = gori ? gori->refObjId : 0;
                }
            }
            if (!why.empty()) core::LogWarning("SroSkillLab: Magic POP: {}", why);
        }
        data_.prepared[8] = {"itemexamkeeper", "IxCKeeper"};
        data_.prepared[9] = {"itemexamraider", "IxCRaider"};
    }

    bool StartServer(std::string& error) {
        std::string key;
        if (!net::MakeSelfSignedCertificate("localhost", 1, pem_, key, error)) return false;
        config_.listen.address = "127.0.0.1";
        config_.listen.port = 0;
        config_.listen.tls = {pem_, key};
        config_.databaseUrl = WithDatabase(options_.resultsUrl, name_);
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{U'0', U'9'}, {U'A', U'Z'}, {U'a', U'z'}}) rules->names.Allow(first, last);
        // The four stand-in figures, each with its creation's stand-in armour (ids 1..12) and weapon (30, 31).
        for (const int race : {0, 1}) {
            const auto r = race == 0 ? game::CreationRules::Race::China : game::CreationRules::Race::Europe;
            for (const bool male : {true, false}) {
                rules->creation.AddFigure(kFigure[race][male ? 1 : 0], r, male);
                const int set = race * 6 + (male ? 0 : 3);
                rules->creation.AddArmourSet(r, male, std::uint32_t(set + 1), std::uint32_t(set + 2), std::uint32_t(set + 3));
            }
            rules->creation.AddWeapon(r, race == 0 ? 30u : 31u);
        }
        rules->items = data_.items;
        config_.rules = rules;
        config_.files = &files_;
        config_.characters = data_.characters;
        config_.skills = data_.skills;
        config_.teleports = data_.teleports;
        config_.magicOptions = std::shared_ptr<const std::vector<assets::MagicOptionRow>>(data_.lab, &data_.lab->options);
        // The item sets from the game files (refsetitemgroup.txt, refmagicoptgroup.txt).
        auto sets = std::make_shared<game::SetTable>();
        if (!game::LoadSetTable(files_, data_.lab->options, nullptr, *sets, error)) return (error = "the sets: " + error), false;
        config_.sets = sets;
        config_.spirits = data_.spirits;
        config_.magicOptionAssign = data_.assign;
        config_.magicOptGroups = data_.lab->optionGroups; // the socket stones' skills by level
        config_.jobSuitSeconds = 0.2; // a league suit's change (the original's 10 s), as the tests shorten it
        config_.jobLockSeconds = 0.2;
        if (data_.smith != 0) config_.npcs.push_back({data_.smith, "NPC_CH_SMITH", {kPlace.region, kPlace.x + 12.0f, kPlace.y, kPlace.z}, 0.0f});
        LoadShared();
        config_.levels.assign(150, 1000);
        config_.reportInterval = 500ms;
        config_.jobs = &jobs_;
        config_.databaseThreads = 2;
        config_.devCommands = true;
        if (!game_.Start(config_, error)) return false;
        core::LogInfo("SroSkillLab: the item exam's game server on {} (database {})", game_.Port(), name_);
        return true;
    }

    // One player at a time, on this thread (it turns the server's loop while it waits).
    bool RunHere(std::string& error) {
        Runner runner(options_, report_, data_, *results_, *exam_, net_, game_.Port(), pem_, [this] { game_.Poll(2ms); });
        return runner.Run(error);
    }

    // A worker of an exam: players on the exam's game server (in its process), its share of the items.
    bool RunWorker(std::string& error) {
        report_.run = options_.run;
        std::ifstream in(options_.pemFile, std::ios::binary);
        pem_.assign(std::istreambuf_iterator<char>(in), {});
        if (pem_.empty()) return (error = "the certificate " + options_.pemFile), false;
        exam_ = Database::Connect(WithDatabase(options_.resultsUrl, options_.examDatabase), error);
        if (!exam_) return false;
        LoadShared(); // the data the checks read (the server's process placed the NPCs and prepared the players)
        Runner runner(options_, report_, data_, *results_, *exam_, net_, options_.port, pem_, [] { std::this_thread::sleep_for(1ms); });
        return runner.Run(error);
    }

    // The workers: this program again, each a share of the items on its own players, all on this game server, whose loop
    // this thread turns until they are done; their counts read from their logs' last lines.
    bool RunWorkers(std::string& error) {
#ifdef _WIN32
        const std::filesystem::path logDir = options_.logDir.empty() ? std::filesystem::path("lab-logs") : std::filesystem::path(options_.logDir);
        std::filesystem::create_directories(logDir);
        const std::filesystem::path pemFile = logDir / std::format("item-exam-{}.pem", report_.run);
        {
            std::ofstream out(pemFile, std::ios::binary);
            out << pem_;
        }
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
        struct Worker {
            PROCESS_INFORMATION process{};
            std::filesystem::path log;
            bool done = false;
        };
        std::vector<Worker> list(static_cast<std::size_t>(options_.workers));
        for (int k = 0; k < options_.workers; ++k) {
            Worker& w = list[std::size_t(k)];
            w.log = logDir / std::format("item-exam-{}-{:02}.log", report_.run, k);
            std::string args = std::format(" --item-exam --part {}/{} --run {} --worker-port {} --pem \"{}\" --exam-db {} --game-dir \"{}\" --db \"{}\" --build {}", k,
                                           options_.workers, report_.run, game_.Port(), pemFile.string(), name_, options_.gameDir, options_.resultsUrl, options_.build);
            std::string routes;
            for (const std::string& r : options_.routes) routes += (routes.empty() ? "" : ",") + r;
            if (!routes.empty()) args += " --routes " + routes;
            if (!options_.item.empty()) args += " --item " + options_.item;
            if (options_.limit > 0) args += std::format(" --limit {}", options_.limit);
            std::wstring line = L"\"" + std::wstring(self) + L"\"" + widen(args);
            SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE out = CreateFileW(w.log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            STARTUPINFOW start{};
            start.cb = sizeof(start);
            start.dwFlags = STARTF_USESTDHANDLES;
            start.hStdOutput = out;
            start.hStdError = out;
            start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &w.process)) {
                core::LogError("SroSkillLab: item exam worker {} could not start (error {})", k, GetLastError());
                w.done = true;
            }
            CloseHandle(out);
        }
        core::LogInfo("SroSkillLab: item exam run {}: {} workers on one game server, their logs in {}", report_.run, options_.workers, logDir.string());
        const std::regex summary(R"((\d+) passed, (\d+) failed, (\d+) skipped)");
        int broken = 0;
        for (;;) {
            game_.Poll(5ms);
            bool running = false;
            for (Worker& w : list) {
                if (w.done) continue;
                if (WaitForSingleObject(w.process.hProcess, 0) != WAIT_OBJECT_0) {
                    running = true;
                    continue;
                }
                w.done = true;
                DWORD code = 0;
                GetExitCodeProcess(w.process.hProcess, &code);
                CloseHandle(w.process.hProcess);
                CloseHandle(w.process.hThread);
                std::ifstream in(w.log);
                bool counted = false;
                for (std::string text; std::getline(in, text);) {
                    std::smatch m;
                    if (text.find("SroSkillLab: item exam worker") != std::string::npos && std::regex_search(text, m, summary)) {
                        report_.passed += std::stoi(m[1]);
                        report_.failed += std::stoi(m[2]);
                        report_.skipped += std::stoi(m[3]);
                        counted = true;
                    }
                }
                if (code != 0 || !counted) ++broken;
                core::LogInfo("SroSkillLab: {} done (exit {}){}", w.log.filename().string(), code, counted ? "" : ", no result line");
            }
            if (!running) break;
        }
        std::filesystem::remove(pemFile);
        if (broken > 0) error = std::format("{} of the workers broke off (their logs in {})", broken, logDir.string());
        return broken == 0;
#else
        return (error = "workers run on Windows only"), false;
#endif
    }

    void Finish() {
        game_.Stop();
        if (results_ && report_.run > 0) {
            results_->Query("UPDATE sro_vt_account.sr_lab_runs SET finished_at = now(), passed = $2, failed = $3, skipped = $4 WHERE id = $1",
                            {report_.run, report_.passed, report_.failed, report_.skipped});
        }
        exam_.reset();
        if (results_ && !name_.empty() && !options_.keepDatabase) results_->Query("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE)");
    }

    const ItemExamOptions& options_;
    ItemExamReport& report_;
    assets::FileSystem files_;
    assets::FileSystem popFiles_; // the database's tables (DatabaseText) for Magic POP's
    Data data_;
    std::unique_ptr<Database> results_, exam_;
    std::string name_, pem_;
    core::JobSystem jobs_{2};
    net::NetService net_{2};
    GameServer game_;
    GameServerConfig config_;
};

} // namespace

std::vector<std::string> ItemExamRoutes() {
    std::vector<std::string> out;
    for (const ItemRoute r : kRoutes) out.push_back(game::ItemRouteName(r));
    return out;
}

bool RunItemExam(const ItemExamOptions& options, ItemExamReport& report, std::string& error) {
    Exam exam(options, report);
    return exam.Run(error);
}

} // namespace sr::server
