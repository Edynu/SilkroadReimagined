#include "sr/server/SkillExam.h"

#include <tuple>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include <nlohmann/json.hpp>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/game/Heals.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Items.h"
#include "sr/game/Protocol.h"
#include "sr/game/SkillCatalogue.h"
#include "sr/game/Skills.h"
#include "sr/game/WorldProtocol.h"
#include "sr/net/Connection.h"
#include "sr/server/Database.h"
#include "sr/server/GameServer.h"
#include "sr/server/SkillLab.h"
#include "sr/server/Store.h"

#include "ExamClient.h"

namespace sr::server {

namespace {

namespace proto = game::proto;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

using lab::ExamClient;
using lab::Ms;
using lab::WithDatabase;

// What a skill wants worn: its weapon (and that weapon's ammunition), a shield, an armour set of a kind.
struct Outfit {
    std::vector<const assets::ItemRow*> items;
};

class Exam {
public:
    Exam(const ExamOptions& o, ExamReport& r) : options_(o), report_(r) {}

    bool Run(std::string& error) {
        if (!LoadData(error)) return false;
        if (!OpenDatabases(error)) return false;
        const bool ok = StartServer(error) && RunMasteries(error);
        Finish();
        return ok;
    }

private:
    // --- Set-up ---------------------------------------------------------------------------------------------------

    bool LoadData(std::string& error) {
        if (!assets::MountGameArchives(files_, options_.gameDir, error)) return false;
        characters_ = std::make_shared<assets::CharacterTable>();
        skills_ = std::make_shared<assets::SkillTable>();
        items_ = std::make_shared<assets::ItemTable>();
        if (!characters_->Load(files_, error) || !skills_->Load(files_, error) || !items_->Load(files_, error)) return false;
        if (!LoadLabData(files_, lab_, error)) return false;
        catalogue_ = game::BuildSkillCatalogue(*skills_);
        return true;
    }

    bool OpenDatabases(std::string& error) {
        results_ = Database::Connect(options_.resultsUrl, error);
        if (!results_ || !MigrateSchema(*results_, error)) return false;
        // The exam's own database (dropped at its end): our layout only, no reference tables, so no nests.
        name_ = std::format("sro_lab_exam_{}", std::chrono::system_clock::now().time_since_epoch().count() % 1000000000);
        const QueryResult made = results_->Query("CREATE DATABASE " + name_);
        if (!made.Ok()) return (error = "the exam's database: " + made.Error()), false;
        examUrl_ = WithDatabase(options_.resultsUrl, name_);
        exam_ = Database::Connect(examUrl_, error);
        if (!exam_ || !MigrateSchema(*exam_, error)) return false;
        std::string filter = options_.skill;
        for (const std::string& m : options_.masteries) filter += (filter.empty() ? "" : ",") + m;
        if (options_.race >= 0) filter += (filter.empty() ? "" : ",") + std::string(options_.race == 0 ? "china" : "europe");
        std::string scenarios;
        for (const int s : options_.scenarios) scenarios += (scenarios.empty() ? "" : ",") + std::to_string(s);
        if (options_.run > 0) {
            report_.run = options_.run; // a worker of a parallel exam: its run
            return true;
        }
        const QueryResult run = results_->Query("INSERT INTO sro_vt_account.sr_lab_runs (build, filter, scenarios) VALUES ($1, $2, $3) RETURNING id",
                                                {options_.build, filter, scenarios});
        if (!run.Ok() || run.Rows() != 1) return (error = "the run: " + run.Error()), false;
        report_.run = run.Int(0, 0);
        return true;
    }

    bool StartServer(std::string& error) {
        std::string key;
        if (!net::MakeSelfSignedCertificate("localhost", 1, pem_, key, error)) return false;
        config_.listen.address = "127.0.0.1";
        config_.listen.port = 0;
        config_.listen.tls = {pem_, key};
        config_.databaseUrl = examUrl_;
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{U'0', U'9'}, {U'A', U'Z'}, {U'a', U'z'}}) rules->names.Allow(first, last);
        // Two stand-in figures (their race is what counts): China and Europe, men.
        rules->creation.AddFigure(kChinaFigure, game::CreationRules::Race::China, true);
        rules->creation.AddFigure(kEuropeFigure, game::CreationRules::Race::Europe, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddArmourSet(game::CreationRules::Race::Europe, true, 4, 5, 6);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        rules->creation.AddWeapon(game::CreationRules::Race::Europe, 11);
        rules->items = items_;
        config_.rules = rules;
        config_.files = &files_;
        config_.characters = characters_;
        config_.skills = skills_;
        config_.levels.assign(150, 1000);
        config_.reportInterval = 500ms;
        config_.jobs = &jobs_;
        config_.databaseThreads = 1; // its connections few: many exams run at once (SroSkillLab --workers)
        config_.devCommands = true;
        config_.freePvpSeconds = 0.3; // scenario 4's players change their free PvP team at once (the original's 10 s)
        if (!game_.Start(config_, error)) return false;
        core::LogInfo("SroSkillLab: the exam's game server on {} (database {})", game_.Port(), name_);
        return true;
    }

    void Finish() {
        game_.Stop();
        if (results_) {
            results_->Query("UPDATE sro_vt_account.sr_lab_runs SET finished_at = now(), passed = $2, failed = $3, skipped = $4 WHERE id = $1",
                            {report_.run, report_.passed, report_.failed, report_.skipped});
        }
        exam_.reset();
        if (results_ && !name_.empty() && !options_.keepDatabase) results_->Query("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE)");
    }

    // --- The masteries --------------------------------------------------------------------------------------------

    std::vector<const assets::MasteryRow*> Masteries() const {
        std::vector<const assets::MasteryRow*> out;
        std::string skillMastery;
        for (const game::SkillCatalogueEntry& e : catalogue_) {
            if (!options_.skill.empty() && e.code == options_.skill) skillMastery = std::to_string(e.mastery->id);
        }
        for (const assets::MasteryRow& m : skills_->masteries()) {
            if (skills_->Board(m.id).empty()) continue;
            const int race = game::MasteryOfRace(*skills_, m.id, 0) ? 0 : 1;
            if (options_.race >= 0 && race != options_.race) continue;
            const auto name = lab_.text.find(m.nameKey);
            const std::string text = name != lab_.text.end() ? name->second : m.nameKey;
            bool wanted = options_.masteries.empty() && skillMastery.empty();
            for (const std::string& w : options_.masteries) {
                std::string a = w, b = text;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                wanted = wanted || w == std::to_string(m.id) || (!a.empty() && b.find(a) != std::string::npos);
            }
            if (!skillMastery.empty()) wanted = skillMastery == std::to_string(m.id);
            if (wanted) out.push_back(&m);
        }
        return out;
    }

    bool RunMasteries(std::string& error) {
        const std::vector<const assets::MasteryRow*> masteries = Masteries();
        if (masteries.empty()) return (error = "no mastery fits the filter"), false;
        for (const assets::MasteryRow* m : masteries) {
            if (!RunMastery(*m, error)) return false;
            ++masteryIndex_;
        }
        return true;
    }

    // The level a mastery's character is made at: what its board's last levels ask, within the level cap.
    int LevelFor(std::uint32_t mastery) const {
        int level = 1;
        for (const game::SkillCatalogueEntry& e : catalogue_) {
            if (e.mastery->id != mastery) continue;
            for (const assets::SkillRow* r : e.ranks) level = std::max(level, r->masteryLevels[0]);
        }
        return std::clamp(level, 1, config_.rates.levelCap);
    }

    bool RunMastery(const assets::MasteryRow& mastery, std::string& error) {
        const int race = game::MasteryOfRace(*skills_, mastery.id, 0) ? 0 : 1;
        const int level = LevelFor(mastery.id);
        const auto name = lab_.text.find(mastery.nameKey);
        masteryName_ = name != lab_.text.end() ? name->second : mastery.nameKey;
        core::LogInfo("SroSkillLab: {} ({}), a character of level {}", masteryName_, mastery.id, level);
        auto client = MakeCharacter(race, error);
        if (!client) return false;
        ExamClient& c = *client;
        std::size_t mark = c.Mark();
        c.Send(proto::DevLearn{mastery.id, std::uint8_t(level), 0});
        const auto book = c.Wait<proto::SkillBookList>(mark, [&](const proto::SkillBookList& b) {
            return std::any_of(b.masteries.begin(), b.masteries.end(), [&](const proto::MasteryRecord& x) { return x.mastery == mastery.id && x.level == level; });
        });
        if (!book) return (error = masteryName_ + ": the mastery was not learned"), false;
        std::map<std::uint32_t, const assets::SkillRow*> learned; // by group
        for (const std::uint32_t id : book->skills) {
            if (const assets::SkillRow* s = skills_->Find(id)) learned[s->group] = s;
        }
        // Its stat points as a player of the mastery spends them: all in INT for one whose attacks are magical (or that
        // has none: the healers, the bards), else all in STR.
        int magical = 0, physical = 0;
        for (const game::SkillCatalogueEntry& e : catalogue_) {
            if (e.mastery->id != mastery.id || !e.First().attack) continue;
            ((e.First().attackKind & 8) != 0 ? magical : physical) += 1;
        }
        const std::uint8_t stat = magical >= physical ? 1 : 0;
        c.Pump(300ms);
        if (const auto progress = c.Wait<proto::Progress>(0, [](const proto::Progress& p) { return p.statPoints > 0; }, 2s)) {
            for (int i = 0; i < progress->statPoints; ++i) c.Send(proto::RaiseStat{stat});
            c.Pump(500ms);
        }
        worn_.clear();
        int index = -1;
        for (const game::SkillCatalogueEntry& e : catalogue_) {
            if (e.mastery->id != mastery.id || (!options_.skill.empty() && e.code != options_.skill)) continue;
            if (++index % std::max(1, options_.parts) != options_.part) continue; // another worker's share
            const auto it = learned.find(e.First().group);
            for (const int scenario : options_.scenarios) {
                if (scenario == 3) {
                    if (it == learned.end()) {
                        Write(e, 3, Skip(e, "not learned at mastery level " + std::to_string(level)));
                        continue;
                    }
                    const std::vector<Result> cases = Special(c, e, *it->second);
                    if (cases.empty()) Write(e, 3, Skip(e, "no special case fits it"));
                    for (const Result& r : cases) Write(e, 3, r);
                    continue;
                }
                if (scenario == 4) {
                    if (it == learned.end()) {
                        Write(e, 4, Skip(e, "not learned at mastery level " + std::to_string(level)));
                        continue;
                    }
                    if (!casterTeamed_ && !SetTeam(c, me_, casterTeam_, 1)) core::LogWarning("SroSkillLab: the caster could not join free PvP team 1");
                    casterTeamed_ = true;
                    const std::vector<Result> cases = Versus(c, e, *it->second);
                    if (cases.empty()) Write(e, 4, Skip(e, "nothing against or for a player"));
                    for (const Result& r : cases) Write(e, 4, r);
                    continue;
                }
                if (scenario != 1 && scenario != 2) continue;
                Result r = it == learned.end() ? Skip(e, "not learned at mastery level " + std::to_string(level)) : Cast(c, e, *it->second, scenario);
                Write(e, scenario, r);
            }
            c.Forget();
        }
        // The armour comparison: each mastery's on one worker (spread over them by the masteries' order).
        if (std::find(options_.scenarios.begin(), options_.scenarios.end(), 4) != options_.scenarios.end() && options_.skill.empty() &&
            masteryIndex_ % std::max(1, options_.parts) == options_.part) {
            if (const std::optional<Result> armour = Armour(c, mastery, learned)) {
                const auto e = std::find_if(catalogue_.begin(), catalogue_.end(), [&](const game::SkillCatalogueEntry& x) { return x.code == armour->code; });
                if (e != catalogue_.end()) Write(*e, 4, *armour);
            }
        }
        // The second players' clients forget too; a new mastery's caster joins its team afresh.
        for (Target& t : targets_) {
            if (t.client) t.client->Forget();
        }
        casterTeamed_ = false;
        casterTeam_ = 0;
        return true;
    }

    // A fresh character of the race, at the exam's place, in the world.
    std::unique_ptr<ExamClient> MakeCharacter(int race, std::string& error) {
        const std::string name = std::format("Lab{}{}", race == 0 ? "C" : "E", ++made_);
        const std::string account = "labexam" + std::to_string(made_);
        if (CreateAccount(*exam_, account, "labexam-secret", error) != CreateAccountResult::Created) return nullptr;
        bool ok = false;
        const auto row = FindAccount(*exam_, account, ok);
        if (!row) return (error = "the exam's account"), nullptr;
        auto c = std::make_unique<ExamClient>(net_, game_.Port(), pem_, [this] { game_.Poll(2ms); });
        std::size_t mark = c->Mark();
        proto::Hello hello;
        hello.client = "SroSkillLab";
        c->Send(hello);
        const auto welcome = c->Next<proto::Welcome>(mark);
        if (!welcome || welcome->result != proto::WelcomeResult::Accepted) return (error = "the exam's game server did not greet"), nullptr;
        proto::LobbyEnter enter;
        enter.ticket = IssueTicket(*exam_, row->id, config_.id, 30, error);
        mark = c->Mark();
        c->Send(enter);
        if (!c->Next<proto::LobbyEntered>(mark)) return (error = "the lobby"), nullptr;
        proto::CharacterCreate m;
        m.name = name;
        m.model = race == 0 ? kChinaFigure : kEuropeFigure;
        m.chest = race == 0 ? 1 : 4;
        m.legs = race == 0 ? 2 : 5;
        m.shoes = race == 0 ? 3 : 6;
        m.weapon = race == 0 ? 10 : 11;
        mark = c->Mark();
        c->Send(m);
        const auto made = c->Next<proto::LobbyResult>(mark);
        if (!made || made->status != proto::LobbyStatus::Ok) {
            return (error = std::format("the character {} was not made (status {})", name, made ? int(made->status) : -1)), nullptr;
        }
        const std::vector<CharacterRow> list = ListCharacters(*exam_, row->id, config_.id, 0, ok);
        if (list.empty()) return (error = "the character " + name), nullptr;
        CharacterRow::Place here;
        here.region = kPlace.region;
        here.x = kPlace.x;
        here.y = kPlace.y;
        here.z = kPlace.z;
        SavePlaces(*exam_, {{list[0].id, here}});
        // The creation's stand-ins (ids 1..6, 10, 11: no items of the game files) out of their places: the skills' own go there.
        exam_->Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot < 13", {list[0].id});
        mark = c->Mark();
        c->Send(proto::CharacterSelect{name});
        if (!c->Next<proto::LobbyResult>(mark)) return (error = "selecting " + name), nullptr;
        mark = c->Mark();
        c->Send(proto::WorldEnter{});
        const auto entered = c->Next<proto::WorldEntered>(mark);
        if (!entered || entered->status != proto::WorldStatus::Ok) return (error = name + " did not enter the world"), nullptr;
        me_ = entered->entity;
        c->Pump(500ms); // the ground about it
        return c;
    }

    // --- What to wear ---------------------------------------------------------------------------------------------

    // The best item of a type the character may wear: in service, its race's, of a level it has, the highest such
    // (a normal one before a rare one).
    const assets::ItemRow* BestItem(int race, int level, int tid3, int tid4) const {
        const assets::ItemRow* best = nullptr;
        int bestLevel = -1;
        for (const auto& [code, row] : items_->rows()) {
            if (!row.inService || row.typeId[0] != 3 || row.typeId[1] != 1 || row.typeId[2] != tid3 || (tid4 != 0 && row.typeId[3] != tid4)) continue;
            if (row.country != race || row.sex == 0 || !game::LevelFits(row, level) || row.Rare()) continue;
            // Its tier: the highest level it asks, of the character (type 1) or of a mastery (the Europeans' armour: a Rogue's
            // or a Cleric's 91 for the light set's tenth).
            int needs = 0;
            for (int i = 0; i < 4; ++i) {
                if (row.requirementTypes[std::size_t(i)] > 0) needs = std::max(needs, row.requirementLevels[std::size_t(i)]);
            }
            if (needs > level) continue;
            if (needs > bestLevel || (needs == bestLevel && best != nullptr && code < best->codename)) {
                best = &row;
                bestLevel = needs;
            }
        }
        return best;
    }
    const assets::ItemRow* Ammunition(int kind) const {
        for (const auto& [code, row] : items_->rows()) {
            if (row.inService && row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 4 && row.typeId[3] == kind) return &row;
        }
        return nullptr;
    }
    Outfit OutfitFor(const assets::SkillRow& skill, int race, int level) const {
        Outfit o;
        int weapon = skill.weaponKinds[0] != 255 ? skill.weaponKinds[0] : -1;
        // A shield asked for ('reqi' 4): a one-handed weapon of its kinds (a two-handed one takes the shield's place).
        bool shield = false;
        assets::ForEachSkillParam(skill, [&](std::uint32_t code, std::span<const std::uint32_t> a) {
            shield = shield || (code == assets::SkillTag("reqi") && !a.empty() && a[0] == 4);
        });
        if (shield) {
            for (const std::uint8_t k : skill.weaponKinds) {
                if (k == 255 || k == 0) continue;
                if (const assets::ItemRow* w = BestItem(race, level, 6, k); w != nullptr && !game::TwoHanded(*w)) {
                    weapon = k;
                    break;
                }
            }
        }
        if (weapon > 0) {
            if (const assets::ItemRow* w = BestItem(race, level, 6, weapon)) {
                o.items.push_back(w);
                if (const int ammo = game::AmmunitionFor(*w); ammo != 0) {
                    if (const assets::ItemRow* a = Ammunition(ammo)) o.items.push_back(a);
                }
            }
        }
        // Its conditions on what is worn ('reqi'): a shield, a weapon of a sort, an armour kind on its six parts.
        assets::ForEachSkillParam(skill, [&](std::uint32_t code, std::span<const std::uint32_t> a) {
            if (code != assets::SkillTag("reqi") || a.size() < 2) return;
            const int type = int(a[0]), sort = int(a[1]);
            if (type == 4) {
                if (const assets::ItemRow* s = BestItem(race, level, 4, sort)) o.items.push_back(s);
            } else if (type == 6 && sort != 0 && weapon <= 0) {
                if (const assets::ItemRow* w = BestItem(race, level, 6, sort)) o.items.push_back(w);
            } else if ((type >= 1 && type <= 3) || (type >= 9 && type <= 11)) {
                for (int part = 1; part <= 6; ++part) {
                    if (sort != 0 && sort != part) continue;
                    if (const assets::ItemRow* p = BestItem(race, level, type, part)) o.items.push_back(p);
                }
            }
        });
        return o;
    }
    // Worn: each item of the outfit not worn yet given and put on (the move's own rules).
    std::vector<std::string> Wear(ExamClient& c, const Outfit& o) { return Wear(c, o, worn_); }
    std::vector<std::string> Wear(ExamClient& c, const Outfit& o, std::map<int, std::uint32_t>& worn_) {
        std::vector<std::string> problems;
        // Armour of another kind off first: a robe and armour do not mix (the move's MixedArmour, as the original's), so
        // the old set goes to the bag (emptied by the reset) before the new one goes on.
        int wantKind = 0;
        for (const assets::ItemRow* item : o.items) {
            const int place = game::EquipPlaceFor(*item, false, false);
            if (place >= game::slot::kHead && place <= game::slot::kFeet) wantKind = item->typeId[2];
        }
        std::uint8_t bag = std::uint8_t(game::slot::kWorn);
        // A shield asked for beside a two-handed weapon in hand (an earlier skill's): the weapon off first (the move
        // refuses a shield beside it).
        const bool wantsShield = std::any_of(o.items.begin(), o.items.end(), [](const assets::ItemRow* i) { return game::EquipPlaceFor(*i, false, false) == game::slot::kShield && i->typeId[2] == 4; });
        const bool newWeapon = std::any_of(o.items.begin(), o.items.end(), [](const assets::ItemRow* i) { return i->typeId[2] == 6; });
        if (const auto w = worn_.find(game::slot::kWeapon); wantsShield && !newWeapon && w != worn_.end()) {
            if (const assets::ItemRow* row = items_->Find(w->second); row != nullptr && game::TwoHanded(*row)) {
                const std::size_t mark = c.Mark();
                c.Send(proto::ItemMove{std::uint8_t(game::slot::kWeapon), bag++, 0});
                c.Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == game::slot::kWeapon; }, 3s);
                worn_.erase(w);
            }
        }
        if (wantKind != 0) {
            for (int place = game::slot::kHead; place <= game::slot::kFeet; ++place) {
                const auto it = worn_.find(place);
                if (it == worn_.end()) continue;
                const assets::ItemRow* row = items_->Find(it->second);
                if (row != nullptr && row->typeId[2] == wantKind) continue;
                const std::size_t mark = c.Mark();
                c.Send(proto::ItemMove{std::uint8_t(place), bag++, 0});
                c.Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.from == place; }, 3s);
                worn_.erase(it);
            }
        }
        for (const assets::ItemRow* item : o.items) {
            const int place = game::EquipPlaceFor(*item, false, false);
            if (place < 0 || worn_[place] == item->refObjId) continue;
            const std::size_t mark = c.Mark();
            c.Send(proto::DevGive{item->codename, std::uint32_t(game::IsAmmunition(*item) ? 1000 : 1), true});
            const auto moved = c.Wait<proto::ItemMoved>(mark, [&](const proto::ItemMoved& m) { return m.to == place; }, 3s);
            if (!moved || moved->status != proto::ItemStatus::Ok) {
                problems.push_back(std::format("{} not worn (status {})", item->codename, moved ? int(moved->status) : -1));
                continue;
            }
            worn_[place] = item->refObjId;
            // A two-handed weapon takes the shield's place too.
            if (place == game::slot::kWeapon && game::TwoHanded(*item)) worn_.erase(game::slot::kShield);
        }
        return problems;
    }

    // --- Scenarios 1 (still) and 2 (fleeing) --------------------------------------------------------------------------

    struct Result {
        std::string status; // pass, fail, skip, error
        std::string summary;
        std::string variant; // a scenario's case (scenario 3's: down, chain-death, ...)
        std::string code;    // written on another group than the one it came with (scenario 4's armour)
        json expected = json::object(), observed = json::object();
    };

    Result Skip(const game::SkillCatalogueEntry&, std::string why) {
        Result r;
        r.status = "skip";
        r.summary = std::move(why);
        return r;
    }

    // A move that goes somewhere (a stop is told as a move to where it stands: the walk it ended came before the swing).
    static bool Walks(const proto::EntityMoved& m) {
        const proto::FlatPosition a = proto::ToFlat(m.from), b = proto::ToFlat(m.to);
        return std::hypot(a.x - b.x, a.z - b.z) > 1.0;
    }

    // A skill's range as the server takes it for its walk into reach: its own Action_Range, else its weapon's and its
    // 'ru' (Skills::Fight; the passives' additions aside).
    static float RangeOf(const assets::SkillRow& skill, const assets::ItemRow* weapon) {
        if (skill.range > 0) return float(skill.range);
        float range = std::max(weapon != nullptr ? weapon->Stat(94) : 0.0f, 6.0f);
        if (const assets::SkillParam ru = assets::FindSkillParam(skill, assets::SkillTag("ru")); ru && skill.attack) range += float(ru.Int(0));
        return range;
    }

    // Scenario 1: the monster stands; scenario 2: it walks away from the caster, the cast starting at the edge of
    // reach: the caster stands through the skill's hold, its hits land wherever the target went, any
    // chase only after the hold (the testers' bow volley: the caster walked after its target between the shots).
    Result Cast(ExamClient& c, const game::SkillCatalogueEntry& e, const assets::SkillRow& skill, int scenario) {
        Result r;
        const game::SkillRoute route = game::SkillRouteOf(skill);
        const game::SkillTimes times = game::TimesOf(*skills_, skill);
        const game::SkillMotion motion = game::SkillMotionOf(skill);
        const int hits = game::HitsOf(*skills_, skill);
        // A damage over time (a link with 'lks2' and a 'puls': the Warlock's): no hit of its swing, its link's pulses.
        const bool dot = assets::FindSkillParam(skill, assets::SkillTag("lks2")) && assets::FindSkillParam(skill, assets::SkillTag("lnks")) &&
                         assets::FindSkillParam(skill, assets::SkillTag("puls")).Int(0) > 0;
        r.expected = {{"dot", dot},{"skill", skill.codename},       {"route", game::SkillRouteName(route)},  {"motion", game::SkillMotionName(motion)},
                      {"holdMs", times.HoldMs()},      {"cycleMs", times.CycleMs(skill.autoAttack != 0)}, {"hits", hits}, {"mpCost", skill.mpCost},
                      {"reuseMs", skill.cooldownMs},   {"autoAttack", skill.autoAttack}};
        switch (route) {
        case game::SkillRoute::Passive: return Skip(e, "a passive: its numbers count in the stats (checked with the sheet later)");
        case game::SkillRoute::Resurrect: return Skip(e, "a resurrection: needs a dead player (scenario 4)");
        case game::SkillRoute::Duplicate: return Skip(e, "a duplicate: needs another player (scenario 4)");
        case game::SkillRoute::Mask: return Skip(e, "a mask: needs a monster's body (scenario 3)");
        case game::SkillRoute::EnemyPlayerBuff: return Skip(e, "for enemy players alone (scenario 4)");
        default: break;
        }
        // On another player alone (a party member, an ally: a guard, a bless, a mana transfer, a link; or an enemy player):
        // scenario 4's.
        if (skill.targetRequired && !skill.targetSelf && !skill.targetEnemyMonster) return Skip(e, "on another player alone (scenario 4)");
        const bool fleeing = scenario == 2;
        if (fleeing && route != game::SkillRoute::Attack) return Skip(e, "scenario 2 is an attack on a fleeing target: this skill takes none");
        const int race = skill.race == 1 ? 1 : 0;
        const int level = LevelFor(e.mastery->id);
        // Fresh: no waits, health and mana full, no states, no buffs, back at the exam's place (a chase after a fleeing
        // monster leads it away); then what it wants worn.
        std::size_t mark = c.Mark();
        c.Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        const auto vitals = c.Next<proto::Vitals>(mark, 3s);
        const Outfit want = OutfitFor(skill, race, level);
        const std::vector<std::string> outfit = Wear(c, want);
        const assets::ItemRow* weapon = nullptr;
        for (const assets::ItemRow* item : want.items) {
            if (weapon == nullptr && item->typeId[2] == 6) weapon = item;
        }
        c.Pump(150ms);
        const std::uint32_t maxMp = vitals ? vitals->maxMp : 0;
        // Its target: a still monster ahead for what strikes, itself for the rest.
        const bool onEnemy = route == game::SkillRoute::Attack;
        // A taunt: the still monster beside it, its target when it takes one.
        const bool taunt = route == game::SkillRoute::Taunt;
        const bool besideEnemy = route == game::SkillRoute::Around || route == game::SkillRoute::Object || route == game::SkillRoute::Unhandled || taunt;
        std::uint32_t monster = 0;
        // Fleeing: within its reach, nine tenths of its range away (30 at least: a melee skill walks to it first), walking
        // away from the caster as the cast is asked for.
        const float range = RangeOf(skill, weapon);
        const float distance = besideEnemy ? 15.0f : fleeing ? std::max(30.0f, 0.9f * range) : 30.0f;
        if (onEnemy || besideEnemy) {
            mark = c.Mark();
            c.Send(proto::DevSpawn{MonsterFor(level), distance, 0, fleeing ? proto::LabBehaviour::FleeWalk : proto::LabBehaviour::Still, 10'000'000, 1});
            const auto spawned = c.Next<proto::DevSpawned>(mark, 3s);
            monster = spawned ? spawned->entity : 0;
            if (monster == 0) {
                r.status = "error";
                r.summary = "the monster was not made";
                return r;
            }
            const auto appeared = c.Wait<proto::EntityAppeared>(mark, [&](const proto::EntityAppeared& a) { return a.entity == monster; }, 3s);
            if (fleeing && !(appeared && appeared->moving) && !c.Wait<proto::EntityMoved>(mark, [&](const proto::EntityMoved& m) { return m.entity == monster; }, 3s)) {
                c.Send(proto::DevRemove{0});
                r.status = "error";
                std::string where = "not seen";
                if (appeared) {
                    where = std::format("at {:.0f}, {:.0f} of region {:X}", appeared->position.x, appeared->position.z, appeared->position.region);
                }
                r.summary = std::format("the monster did not flee: {} away asked, appeared {}", distance, where);
                return r;
            }
        }
        r.expected["rangeUsed"] = range;
        r.expected["distance"] = distance;
        // A bard's dance plays only while one of its songs does ('reqc' 0x20; U3-04): a song of its masteries started
        // first (the lowest of the battle songs, 'scls').
        if ((assets::FindSkillParam(skill, assets::SkillTag("reqc")).Int(0) & 0x20) != 0) {
            const assets::SkillRow* song = nullptr;
            for (const auto& [id, row] : skills_->rows()) {
                if (!row.inService || !assets::FindSkillParam(row, assets::SkillTag("scls")) || row.masteries != skill.masteries) continue;
                if (song == nullptr || row.level < song->level) song = &row;
            }
            if (song != nullptr) {
                std::size_t m = c.Mark();
                c.Send(proto::UseSkill{song->id, 0});
                c.Wait<proto::BuffAdded>(m, [&](const proto::BuffAdded& b) { return b.skill == song->id; }, 5s);
                r.expected["song"] = song->codename;
            }
        }
        // The cast.
        mark = c.Mark();
        const auto t0 = Clock::now();
        proto::UseSkill use{skill.id, onEnemy || (taunt && skill.targetRequired && skill.targetEnemyMonster) ? monster : 0u};
        if (route == game::SkillRoute::Teleport) {
            use.hasPoint = true;
            use.point = {kPlace.region, kPlace.x + 120.0f, kPlace.y, kPlace.z};
        }
        c.Send(use);
        const auto answer = c.Wait<proto::SkillUseAnswer>(mark, [&](const proto::SkillUseAnswer& a) { return a.skill == skill.id; }, 3s);
        const auto swing = answer && answer->status == proto::SkillUseStatus::Ok
                               ? c.Wait<proto::EntitySwing>(mark, [&](const proto::EntitySwing& s) { return s.attacker == me_ && s.skill == skill.id; },
                                                            fleeing ? 10s : 4s)
                               : std::nullopt;
        Clock::time_point ts = Clock::now();
        if (swing) {
            for (const auto& [at, s] : c.Since<proto::EntitySwing>(mark)) {
                if (s.attacker == me_ && s.skill == skill.id) {
                    ts = at;
                    break;
                }
            }
        }
        c.Pump(std::chrono::milliseconds(times.HoldMs() + (fleeing ? 2500 : 1200)));
        if (monster != 0) {
            const std::size_t m = c.Mark();
            c.Send(proto::DevRemove{0});
            c.Wait<proto::EntityGone>(m, [&](const proto::EntityGone& g) { return g.entity == monster; }, 2s);
        }

        // What was seen.
        json o = json::object();
        o["answer"] = answer ? int(answer->status) : -1;
        o["swing"] = bool(swing);
        if (swing) o["approachMs"] = Ms(ts - t0);
        if (!outfit.empty()) o["outfit"] = outfit;
        // The hits on its target from the swing until the next swing of another skill (the basic attack after it).
        Clock::time_point next = Clock::time_point::max();
        for (const auto& [at, s] : c.Since<proto::EntitySwing>(mark)) {
            if (s.attacker == me_ && s.skill != skill.id && at > ts) {
                next = std::min(next, at);
                o["basicAfter"] = true;
            }
        }
        int seen = 0, ticks = 0;
        std::uint64_t damage = 0;
        json hitTimes = json::array();
        for (const auto& [at, h] : c.Since<proto::EntityDamaged>(mark)) {
            if (h.attacker != me_ || monster == 0 || h.target != monster || at < ts || at >= next) continue;
            if ((h.flags & proto::kDamageState) != 0) { // a burn's, a poison's, a bleed's tick: the state's, not a hit
                ++ticks;
                continue;
            }
            ++seen;
            damage += h.damage;
            hitTimes.push_back(Ms(at - ts));
        }
        o["hits"] = seen;
        if (ticks > 0) o["stateTicks"] = ticks;
        o["damage"] = damage;
        o["hitTimesMs"] = std::move(hitTimes);
        std::set<int> states;
        for (const auto& [at, s] : c.Since<proto::EntityStates>(mark)) {
            if (s.entity != monster || monster == 0) continue;
            for (const proto::StateRecord& x : s.states) states.insert(x.kind);
        }
        o["states"] = states;
        json buffs = json::array();
        for (const auto& [at, b] : c.Since<proto::BuffAdded>(mark)) buffs.push_back(b.skill);
        o["buffs"] = buffs;
        std::optional<std::uint32_t> mpAfter;
        for (const auto& [at, v] : c.Since<proto::Vitals>(mark)) mpAfter = std::min(mpAfter.value_or(v.mp), v.mp);
        if (mpAfter && maxMp > 0) o["mpSpent"] = std::int64_t(maxMp) - std::int64_t(*mpAfter);
        for (const auto& [at, cool] : c.Since<proto::Cooldown>(mark)) {
            if (cool.kind == proto::CooldownKind::Skill && cool.id == skill.id) o["reuseMs"] = cool.ms;
        }
        // The caster's own moves from its swing to its hold's end (the walk into reach comes before the swing).
        json moved = json::array();
        if (swing) {
            // The original's hold (its chain's rows on the 500 ms budget: SkillTimes::CycleMs), its earliest end.
            const auto holdEnd = ts + std::chrono::milliseconds(times.CycleMs(skill.autoAttack != 0));
            for (const auto& [at, mv] : c.Since<proto::EntityMoved>(mark)) {
                if (mv.entity == me_ && at > ts + 50ms && at < holdEnd && Walks(mv)) moved.push_back(Ms(at - ts));
            }
        }
        o["movedDuringHoldMs"] = moved;
        // Fleeing, with the basic attack after it: the hold's end seen, the caster's first step after its hold (the chase)
        // or the basic attack's swing, whichever first.
        std::optional<int> holdSeen;
        if (fleeing && swing && skill.autoAttack == 1) {
            for (const auto& [at, mv] : c.Since<proto::EntityMoved>(mark)) {
                if (mv.entity == me_ && at > ts + 50ms && Walks(mv)) {
                    holdSeen = Ms(at - ts);
                    o["chaseAfterMs"] = *holdSeen;
                    break;
                }
            }
            if (next != Clock::time_point::max() && (!holdSeen || Ms(next - ts) < *holdSeen)) holdSeen = Ms(next - ts);
            if (holdSeen) o["holdSeenMs"] = *holdSeen;
        }
        bool blinked = false;
        for (const auto& [at, b] : c.Since<proto::EntityBlink>(mark)) blinked = blinked || b.entity == me_;
        if (route == game::SkillRoute::Teleport) o["blinked"] = blinked;

        // Against what the data says.
        std::vector<std::string> failed, notes;
        if (!answer) {
            failed.push_back("no answer");
        } else if (answer->status != proto::SkillUseStatus::Ok) {
            failed.push_back(std::format("refused (status {})", int(answer->status)));
        }
        if (answer && answer->status == proto::SkillUseStatus::Ok) {
            // Where the chase stood: the caster's moves (from, to, speed) since the cast, ms from the ask.
            const auto chase = [&] {
                std::string out;
                int shown = 0;
                for (const auto& [at, mv] : c.Since<proto::EntityMoved>(mark)) {
                    if ((mv.entity != me_ && mv.entity != monster) || ++shown > 12) continue;
                    out += std::format(" [{} {} ms: {:.0f},{:.0f} -> {:.0f},{:.0f} at {:.0f}]", mv.entity == me_ ? "me" : "it", Ms(at - t0), mv.from.x, mv.from.z,
                                       mv.to.x, mv.to.z, mv.speed);
                }
                return out;
            };
            if (!swing && route != game::SkillRoute::Teleport) failed.push_back("no swing" + (fleeing ? ";" + chase() : std::string()));
            if (onEnemy && dot) {
                if (seen == 0) failed.push_back("no pulse of its damage over time");
            } else if (onEnemy) {
                if (seen != hits) failed.push_back(std::format("{} of {} hits", seen, hits));
                if (seen > 0 && damage == 0) failed.push_back("no damage");
            }
            const bool buff = route == game::SkillRoute::SelfBuff || route == game::SkillRoute::PlayerBuff;
            if (buff && buffs.empty()) failed.push_back("no buff");
            if (route == game::SkillRoute::Teleport && !blinked) failed.push_back("no teleport");
            if (taunt) {
                // Its threat on the monster beside it, told as a skill that took hold there (B0BD).
                bool took = false;
                for (const auto& [at, s] : c.Since<proto::SkillTook>(mark)) took = took || (s.entity == monster && s.skill == skill.id);
                if (!took) failed.push_back("no threat on the monster");
                o["threatTold"] = took;
            }
            if (motion == game::SkillMotion::Stands && !moved.empty()) {
                failed.push_back(std::format("moved during the hold (at {} ms after the swing){}", moved[0].get<int>(), fleeing ? ";" + chase() : std::string()));
            }
            // The hold measured against its data: over 450 ms past it is a hold of another length (the skill tick's
            // rounding and the next update allowed).
            // A release may come a skill tick late (300 ms a row with a casting); the next update and the chase's step allowed.
            if (const int cycle = times.CycleMs(true); holdSeen && motion == game::SkillMotion::Stands && *holdSeen > cycle + 300 * times.LeadRows() + 450) {
                failed.push_back(std::format("held {} ms, the original's cycle {} ms", *holdSeen, cycle));
            }
            if (skill.cooldownMs > 0 && o.contains("reuseMs") && o["reuseMs"] != skill.cooldownMs) failed.push_back("another wait");
            if (skill.cooldownMs > 0 && !o.contains("reuseMs")) notes.push_back("no wait told");
            if (skill.mpCost > 0 && (!o.contains("mpSpent") || o["mpSpent"].get<std::int64_t>() <= 0)) notes.push_back("no MP spent seen");
        }
        if (!outfit.empty()) notes.push_back(outfit.front());
        r.observed = std::move(o);
        r.status = failed.empty() ? "pass" : "fail";
        std::string text;
        if (onEnemy) text = dot ? std::format("{} pulses", seen) : std::format("{} of {} hits", seen, hits);
        if (ticks > 0) text += std::format(", {} state ticks", ticks);
        if (r.observed.contains("mpSpent")) text += std::format("{}MP {}", text.empty() ? "" : ", ", r.observed["mpSpent"].get<std::int64_t>());
        if (r.observed.contains("reuseMs")) text += std::format("{}wait {} ms", text.empty() ? "" : ", ", r.observed["reuseMs"].get<std::uint32_t>());
        if (motion == game::SkillMotion::Stands) text += std::format("{}held {} ms", text.empty() ? "" : ", ", times.CycleMs(skill.autoAttack != 0));
        if (holdSeen) text += std::format(" (seen {} ms)", *holdSeen);
        if (fleeing && swing && r.observed.contains("approachMs")) text += std::format(", swung {} ms after the ask", r.observed["approachMs"].get<int>());
        if (!buffs.empty()) text += std::format("{}buff", text.empty() ? "" : ", ");
        if (!states.empty()) text += std::format("{}{} state kinds seen", text.empty() ? "" : ", ", states.size());
        std::string reasons;
        for (const std::string& f : failed) reasons += (reasons.empty() ? "" : "; ") + f;
        for (const std::string& n : notes) text += "; " + n;
        r.summary = failed.empty() ? text : reasons + (text.empty() ? "" : " (" + text + ")");
        return r;
    }

    // --- Scenario 3: special cases ------------------------------------------------------------------------------------

    // Fresh, back at the place, what it wants worn (`instead`: another weapon of that kind in its place); its MP most.
    std::vector<std::string> wearProblems_; // the last Prepare's
    std::uint32_t Prepare(ExamClient& c, const assets::SkillRow& skill, int race, int level, int instead = 0) {
        const std::size_t mark = c.Mark();
        c.Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kPlace});
        const auto vitals = c.Next<proto::Vitals>(mark, 3s);
        Outfit o = OutfitFor(skill, race, level);
        if (instead != 0) {
            std::erase_if(o.items, [](const assets::ItemRow* i) { return i->typeId[2] == 6 || (i->typeId[1] == 3 && i->typeId[2] == 4); });
            if (const assets::ItemRow* w = BestItem(race, level, 6, instead)) o.items.push_back(w);
        }
        wearProblems_ = Wear(c, o);
        c.Pump(150ms);
        return vitals ? vitals->maxMp : 0;
    }
    std::uint32_t Spawn(ExamClient& c, int level, float distance, proto::LabBehaviour behaviour, std::uint32_t hp) {
        const std::size_t mark = c.Mark();
        c.Send(proto::DevSpawn{MonsterFor(level), distance, 0, behaviour, hp, 1});
        const auto spawned = c.Next<proto::DevSpawned>(mark, 3s);
        const std::uint32_t monster = spawned ? spawned->entity : 0;
        if (monster != 0) c.Wait<proto::EntityAppeared>(mark, [&](const proto::EntityAppeared& a) { return a.entity == monster; }, 3s);
        return monster;
    }
    void Unspawn(ExamClient& c, std::uint32_t monster) {
        if (monster == 0) return;
        const std::size_t m = c.Mark();
        c.Send(proto::DevRemove{0});
        c.Wait<proto::EntityGone>(m, [&](const proto::EntityGone& g) { return g.entity == monster; }, 2s);
    }
    // The caster's hits on `monster` since `mark` (a state's ticks aside) until its next swing of another skill (the
    // basic attack after it), and when each came.
    std::vector<Clock::time_point> HitsOn(ExamClient& c, std::size_t mark, std::uint32_t monster, std::uint32_t skill, bool* killed = nullptr) {
        std::vector<Clock::time_point> out;
        Clock::time_point swung = Clock::time_point::max(), next = Clock::time_point::max();
        for (const auto& [at, s] : c.Since<proto::EntitySwing>(mark)) {
            if (s.attacker != me_) continue;
            if (s.skill == skill && swung == Clock::time_point::max()) swung = at;
            else if (s.skill != skill && at > swung) next = std::min(next, at);
        }
        for (const auto& [at, h] : c.Since<proto::EntityDamaged>(mark)) {
            if (h.attacker != me_ || h.target != monster || (h.flags & proto::kDamageState) != 0 || at >= next) continue;
            out.push_back(at);
            if (killed != nullptr && (h.flags & proto::kDamageKilled) != 0) *killed = true;
        }
        return out;
    }
    static Result Verdict(std::string variant, std::vector<std::string> failed, std::string text, json observed) {
        Result r;
        r.variant = std::move(variant);
        r.status = failed.empty() ? "pass" : "fail";
        std::string reasons;
        for (const std::string& f : failed) reasons += (reasons.empty() ? "" : "; ") + f;
        r.summary = failed.empty() ? text : reasons + (text.empty() ? "" : " (" + text + ")");
        r.observed = std::move(observed);
        return r;
    }
    static Result SkipVariant(std::string variant, std::string why) {
        Result r;
        r.variant = std::move(variant);
        r.status = "skip";
        r.summary = std::move(why);
        return r;
    }
    // A weapon kind the skill does not take, of the race's (the Chinese sword, spear or bow; the Europeans' one-handed
    // sword, crossbow or dagger).
    static int OtherWeapon(const assets::SkillRow& skill, int race) {
        const std::array<int, 3> kinds = race == 0 ? std::array<int, 3>{2, 4, 6} : std::array<int, 3>{7, 12, 13};
        for (const int k : kinds) {
            if (std::find(skill.weaponKinds.begin(), skill.weaponKinds.end(), std::uint8_t(k)) == skill.weaponKinds.end()) return k;
        }
        return 0;
    }

    // Scenario 3 (L5): each case that fits the skill, a result of its own (its variant).
    std::vector<Result> Special(ExamClient& c, const game::SkillCatalogueEntry& e, const assets::SkillRow& skill) {
        std::vector<Result> out;
        const game::SkillRoute route = game::SkillRouteOf(skill);
        const game::SkillTimes times = game::TimesOf(*skills_, skill);
        const int race = skill.race == 1 ? 1 : 0;
        const int level = LevelFor(e.mastery->id);
        const int hits = game::HitsOf(*skills_, skill);
        const int lead = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs);
        const bool dot = assets::FindSkillParam(skill, assets::SkillTag("lks2")) && assets::FindSkillParam(skill, assets::SkillTag("lnks")) &&
                         assets::FindSkillParam(skill, assets::SkillTag("puls")).Int(0) > 0;
        const game::SkillTraits traits = game::TraitsOf(skill);

        // A passive: active in the server's sheet with what it needs worn; with another weapon kind, not.
        if (route == game::SkillRoute::Passive) {
            const auto sheet = [&](const char* what) -> std::optional<proto::DevPassive> {
                const std::size_t m = c.Mark();
                c.Send(proto::DevSheet{});
                const auto a = c.Next<proto::DevSheetAnswer>(m, 3s);
                (void)what;
                if (!a) return std::nullopt;
                for (const proto::DevPassive& p : a->passives) {
                    if (p.skill == skill.id) return p;
                }
                return proto::DevPassive{};
            };
            Prepare(c, skill, race, level);
            const auto worn = sheet("worn");
            std::vector<std::string> failed;
            json o = json::object();
            if (!worn) {
                failed.push_back("no sheet");
            } else if (worn->skill == 0) {
                failed.push_back("not among its passives");
            } else {
                o["active"] = worn->active;
                o["changes"] = worn->changes;
                if (!worn->active) {
                    std::string problems;
                    for (const std::string& p : wearProblems_) problems += "; " + p;
                    failed.push_back("not active with what it needs worn" + problems);
                }
            }
            std::string text = worn && worn->skill != 0 ? std::format("{}, {} changes on the sheet", worn->active ? "active" : "inactive", worn->changes) : std::string();
            if (const int other = skill.weaponKinds[0] != 255 ? OtherWeapon(skill, race) : 0; other != 0 && worn && worn->active) {
                Prepare(c, skill, race, level, other);
                const auto bare = sheet("other weapon");
                o["activeWithOtherWeapon"] = bare && bare->active;
                if (bare && bare->active) failed.push_back(std::format("still active with weapon kind {}", other));
                text += std::format("; off with weapon kind {}", other);
            }
            out.push_back(Verdict("passive", std::move(failed), text, std::move(o)));
            return out;
        }

        // The wrong weapon: refused (0x300D's text, our Weapon).
        if (skill.weaponKinds[0] != 255 && skill.activity != 0) {
            if (const int other = OtherWeapon(skill, race); other != 0) {
                Prepare(c, skill, race, level, other);
                const std::size_t m = c.Mark();
                c.Send(proto::UseSkill{skill.id, 0});
                const auto a = c.Wait<proto::SkillUseAnswer>(m, [&](const proto::SkillUseAnswer& x) { return x.skill == skill.id; }, 3s);
                std::vector<std::string> failed;
                if (!a) failed.push_back("no answer");
                else if (a->status != proto::SkillUseStatus::Weapon) failed.push_back(std::format("answered {} (the weapon's refusal is {})", int(a->status), int(proto::SkillUseStatus::Weapon)));
                out.push_back(Verdict("wrong-weapon", std::move(failed), std::format("refused with weapon kind {}", other), json::object()));
                c.Pump(200ms);
            }
        }

        // A buff on itself cast again: the new one replaces it; a toggle cast again is turned off.
        if (route == game::SkillRoute::SelfBuff || (route == game::SkillRoute::PlayerBuff && skill.targetSelf)) {
            Prepare(c, skill, race, level);
            std::size_t m = c.Mark();
            c.Send(proto::UseSkill{skill.id, 0});
            const auto first = c.Wait<proto::BuffAdded>(m, [&](const proto::BuffAdded& b) { return b.skill == skill.id; }, std::chrono::milliseconds(lead + 3000));
            std::vector<std::string> failed;
            json o = json::object();
            if (!first) {
                failed.push_back("no buff the first time");
            } else if (traits.toggle) {
                c.Pump(300ms);
                m = c.Mark();
                c.Send(proto::UseSkill{skill.id, 0});
                const auto off = c.Wait<proto::BuffRemoved>(m, [&](const proto::BuffRemoved& b) { return b.token == first->token; }, 3s);
                if (!off) failed.push_back("not turned off by a second cast");
                out.push_back(Verdict("toggle-off", std::move(failed), "on, then off", std::move(o)));
            } else {
                c.Send(proto::DevReset{std::uint8_t(proto::DevReset::kWaits | proto::DevReset::kVitals)});
                c.Pump(std::chrono::milliseconds(std::max(300, times.HoldMs() + 100)));
                m = c.Mark();
                c.Send(proto::UseSkill{skill.id, 0});
                const auto second = c.Wait<proto::BuffAdded>(m, [&](const proto::BuffAdded& b) { return b.skill == skill.id; }, std::chrono::milliseconds(lead + 3000));
                bool removed = false;
                for (const auto& [at, b] : c.Since<proto::BuffRemoved>(m)) removed = removed || b.token == first->token;
                const auto answer = c.Wait<proto::SkillUseAnswer>(m, [&](const proto::SkillUseAnswer& x) { return x.skill == skill.id; }, 1s);
                o["secondAnswer"] = answer ? int(answer->status) : -1;
                if (!second) failed.push_back(std::format("no buff the second time (answer {})", answer ? int(answer->status) : -1));
                else if (!removed && second->token != first->token) failed.push_back("the first one not replaced (two of it)");
                out.push_back(Verdict("again", std::move(failed), "the second replaces the first", std::move(o)));
            }
            return out;
        }

        if (route != game::SkillRoute::Attack) return out;
        // One for enemy players alone (the Wizard's Mana Burn, the KB stone's knock-back): its cases are scenario 4's.
        if (skill.targetRequired && !skill.targetEnemyMonster) return out;
        const auto castOn = [&](std::uint32_t monster, std::size_t& mark) {
            mark = c.Mark();
            c.Send(proto::UseSkill{skill.id, monster});
            const auto answer = c.Wait<proto::SkillUseAnswer>(mark, [&](const proto::SkillUseAnswer& a) { return a.skill == skill.id; }, 3s);
            const auto swing = answer && answer->status == proto::SkillUseStatus::Ok
                                   ? c.Wait<proto::EntitySwing>(mark, [&](const proto::EntitySwing& s) { return s.attacker == me_ && s.skill == skill.id; }, 6s)
                                   : std::nullopt;
            return std::pair{answer, swing.has_value()};
        };

        // A stunned target: every hit lands.
        {
            Prepare(c, skill, race, level);
            const std::uint32_t monster = Spawn(c, level, 30.0f, proto::LabBehaviour::Still, 10'000'000);
            std::size_t m = c.Mark();
            c.Send(proto::DevAfflict{monster, std::uint8_t(game::StateKind::Stun), 1, 8000, 0, 0});
            const bool stunned = c.Wait<proto::EntityStates>(m, [&](const proto::EntityStates& s) {
                return s.entity == monster && std::any_of(s.states.begin(), s.states.end(), [](const proto::StateRecord& x) { return x.kind == std::uint8_t(game::StateKind::Stun); });
            }, 3s).has_value();
            const auto [answer, swung] = castOn(monster, m);
            c.Pump(std::chrono::milliseconds(times.HoldMs() + 1200));
            const int seen = int(HitsOn(c, m, monster, skill.id).size());
            std::vector<std::string> failed;
            if (!stunned) failed.push_back("the monster was not stunned");
            if (!answer || answer->status != proto::SkillUseStatus::Ok) failed.push_back(std::format("answered {}", answer ? int(answer->status) : -1));
            else if (!swung) failed.push_back("no swing");
            else if (dot ? seen == 0 : seen != hits) failed.push_back(std::format("{} of {} hits", seen, hits));
            Unspawn(c, monster);
            out.push_back(Verdict("stunned-target", std::move(failed), std::format("{} of {} hits on a stunned target", seen, hits), {{"hits", seen}}));
        }

        // A target lying down: a down attack ('da') strikes it there; any other waits for it to rise.
        {
            const bool downAttack = game::AttackOf(skill).downPercent > 0;
            Prepare(c, skill, race, level);
            const std::uint32_t monster = Spawn(c, level, 30.0f, proto::LabBehaviour::Still, 10'000'000);
            std::size_t m = c.Mark();
            c.Send(proto::DevAfflict{monster, proto::DevAfflict::kNoState, 1, 0, 0, 1});
            const auto knocked = c.Wait<proto::EntityKnocked>(m, [&](const proto::EntityKnocked& k) { return k.entity == monster; }, 3s);
            if (!knocked || knocked->kind != proto::KnockKind::Down) {
                out.push_back(SkipVariant("down", "the monster could not be knocked down"));
            } else {
                const Clock::time_point up = Clock::now() + std::chrono::milliseconds(knocked->ms);
                const auto [answer, swung] = castOn(monster, m);
                c.Pump(std::chrono::milliseconds(std::max<int>(times.HoldMs() + 1200, int(knocked->ms) + 600)));
                int whileDown = 0, after = 0;
                for (const Clock::time_point at : HitsOn(c, m, monster, skill.id)) (at < up - 100ms ? whileDown : after) += 1;
                std::vector<std::string> failed;
                if (!answer || answer->status != proto::SkillUseStatus::Ok) failed.push_back(std::format("answered {}", answer ? int(answer->status) : -1));
                else if (downAttack && whileDown == 0) failed.push_back("a down attack did not strike the target lying");
                else if (!downAttack && whileDown > 0) failed.push_back(std::format("{} hits on the target lying (not a down attack)", whileDown));
                Unspawn(c, monster);
                out.push_back(Verdict("down", std::move(failed),
                                      std::format("{}: {} hits while it lay, {} after", downAttack ? "a down attack" : "not a down attack", whileDown, after),
                                      {{"whileDown", whileDown}, {"after", after}, {"downAttack", downAttack}, {"downMs", knocked->ms}}));
            }
        }

        // A chain whose target dies at its first hit: no later row.
        if (!game::ChainOf(*skills_, skill).empty() && !dot) {
            Prepare(c, skill, race, level);
            const std::uint32_t monster = Spawn(c, level, 30.0f, proto::LabBehaviour::Still, 1);
            std::size_t m = c.Mark();
            const auto [answer, swung] = castOn(monster, m);
            c.Pump(std::chrono::milliseconds(times.HoldMs() + 800));
            bool killed = false;
            const int seen = int(HitsOn(c, m, monster, skill.id, &killed).size());
            std::vector<std::string> failed;
            if (!answer || answer->status != proto::SkillUseStatus::Ok) failed.push_back(std::format("answered {}", answer ? int(answer->status) : -1));
            else if (!killed) failed.push_back("the target did not die");
            else if (seen != 1) failed.push_back(std::format("{} hits on a target dead at the first", seen));
            Unspawn(c, monster);
            out.push_back(Verdict("chain-death", std::move(failed), std::format("{} hit, the chain of {} rows ended", seen, game::ChainOf(*skills_, skill).size() + 1),
                                  {{"hits", seen}, {"killed", killed}}));
        }

        // Its caster stunned during its casting: given up, told, nothing struck or paid (B071 2).
        if (lead >= 200) {
            const std::uint32_t maxMp = Prepare(c, skill, race, level);
            const std::uint32_t monster = Spawn(c, level, 30.0f, proto::LabBehaviour::Still, 10'000'000);
            std::size_t m = c.Mark();
            const auto [answer, swung] = castOn(monster, m);
            if (swung) c.Send(proto::DevAfflict{me_, std::uint8_t(game::StateKind::Stun), 1, 1500, 0, 0});
            c.Pump(std::chrono::milliseconds(times.HoldMs() + 800));
            bool cancelled = false;
            for (const auto& [at, x] : c.Since<proto::EntityCastCancelled>(m)) cancelled = cancelled || x.entity == me_;
            const int seen = int(HitsOn(c, m, monster, skill.id).size());
            std::optional<std::uint32_t> mpAfter;
            for (const auto& [at, v] : c.Since<proto::Vitals>(m)) mpAfter = std::min(mpAfter.value_or(v.mp), v.mp);
            const bool paid = mpAfter && maxMp > 0 && *mpAfter < maxMp;
            std::vector<std::string> failed;
            if (!answer || answer->status != proto::SkillUseStatus::Ok) failed.push_back(std::format("answered {}", answer ? int(answer->status) : -1));
            else if (!swung) failed.push_back("no swing");
            else {
                if (!cancelled) failed.push_back("no cancel told");
                if (seen > 0) failed.push_back(std::format("{} hits after its caster was stunned", seen));
                if (paid) failed.push_back("its MP paid");
            }
            Unspawn(c, monster);
            c.Send(proto::DevReset{proto::DevReset::kStates});
            out.push_back(Verdict("caster-stunned", std::move(failed), std::format("stunned during its casting of {} ms: given up", lead),
                                  {{"cancelled", cancelled}, {"hits", seen}, {"paid", paid}}));
        }
        return out;
    }

    // --- Scenario 4: against a player ---------------------------------------------------------------------------------

    // The second exam player, one per race: its client, its entity, what it wears, its free PvP team (0 none: a friend
    // of the caster's team 1; 2: its foe).
    struct Target {
        std::unique_ptr<ExamClient> client;
        std::uint32_t entity = 0;
        std::map<int, std::uint32_t> worn;
        std::uint8_t team = 0;
        int level = 1;
    };
    Target* TargetFor(int race, int level, std::string& error) {
        Target& t = targets_[race == 1 ? 1 : 0];
        if (t.client) return &t;
        const std::uint32_t me = me_;
        t.client = MakeCharacter(race, error);
        t.entity = me_;
        me_ = me;
        if (!t.client) return nullptr;
        // Of the caster's level (the PvP rules' levels): a mastery of its race learned at it raises it there.
        const std::uint32_t mastery = race == 1 ? 513u : 257u;
        const std::size_t m = t.client->Mark();
        t.client->Send(proto::DevLearn{mastery, std::uint8_t(level), 0});
        t.client->Wait<proto::SkillBookList>(m, [&](const proto::SkillBookList& b) { return !b.masteries.empty(); }, 5s);
        t.client->Pump(300ms);
        t.level = level;
        return &t;
    }
    // A player's free PvP team, waited for (the exam's server changes it in 0.3 s).
    bool SetTeam(ExamClient& c, std::uint32_t entity, std::uint8_t& current, std::uint8_t team) {
        if (current == team) return true;
        const std::size_t m = c.Mark();
        c.Send(proto::FreePvp{team});
        const auto done = c.Wait<proto::EntityFreePvp>(m, [&](const proto::EntityFreePvp& f) { return f.entity == entity && f.team == team && f.changing == 0; }, 4s);
        if (done) current = team;
        return done.has_value();
    }
    // The target fresh, back at its place, in `team`, its health and mana at those percents.
    bool ReadyTarget(Target& t, std::uint8_t team, int hpPercent = 100, int mpPercent = 100) {
        std::size_t m = t.client->Mark();
        t.client->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kTargetPlace});
        t.client->Next<proto::Vitals>(m, 2s);
        if (!SetTeam(*t.client, t.entity, t.team, team)) return false;
        // A foe: 10 million most health (no hit kills it: every hit of a chain lands); a friend: its own (a heal's rise
        // measured on it).
        m = t.client->Mark();
        t.client->Send(proto::DevVitals{std::uint8_t(hpPercent), std::uint8_t(mpPercent), team != 0 ? 10'000'000u : 0u});
        t.client->Next<proto::Vitals>(m, 2s);
        t.client->Pump(100ms);
        return true;
    }
    // The cast on `target` by the caster: its answer, whether it swung.
    std::pair<std::optional<proto::SkillUseAnswer>, bool> CastAt(ExamClient& c, const assets::SkillRow& skill, std::uint32_t target, std::size_t& mark) {
        mark = c.Mark();
        c.Send(proto::UseSkill{skill.id, target});
        const auto answer = c.Wait<proto::SkillUseAnswer>(mark, [&](const proto::SkillUseAnswer& a) { return a.skill == skill.id; }, 3s);
        const bool swung = answer && answer->status == proto::SkillUseStatus::Ok &&
                           c.Wait<proto::EntitySwing>(mark, [&](const proto::EntitySwing& s) { return s.attacker == me_ && s.skill == skill.id; }, 6s).has_value();
        return {answer, swung};
    }
    static std::string Answered(const std::optional<proto::SkillUseAnswer>& a) { return std::format("answered {}", a ? int(a->status) : -1); }

    // Scenario 4 (L6): the cases against or for another player that fit the skill.
    std::vector<Result> Versus(ExamClient& c, const game::SkillCatalogueEntry& e, const assets::SkillRow& skill) {
        std::vector<Result> out;
        const game::SkillRoute route = game::SkillRouteOf(skill);
        const game::SkillTimes times = game::TimesOf(*skills_, skill);
        const game::SkillTraits traits = game::TraitsOf(skill);
        const int race = skill.race == 1 ? 1 : 0;
        const int level = LevelFor(e.mastery->id);
        const int hits = game::HitsOf(*skills_, skill);
        const int lead = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs);
        const bool dot = assets::FindSkillParam(skill, assets::SkillTag("lks2")) && assets::FindSkillParam(skill, assets::SkillTag("lnks")) &&
                         assets::FindSkillParam(skill, assets::SkillTag("puls")).Int(0) > 0;
        const bool manaSwitch = route == game::SkillRoute::Attack && traits.buff && (skill.targetAlly || skill.targetParty) &&
                                assets::FindSkillParam(skill, assets::SkillTag("lnks"));
        const bool onEnemy = route == game::SkillRoute::Attack && skill.targetEnemyPlayer && !manaSwitch;
        // (A taunt moves monsters' threat alone: nothing of it on a player.)
        const bool onFriend = skill.targetRequired && (skill.targetAlly || skill.targetParty) &&
                              (route == game::SkillRoute::Support || route == game::SkillRoute::PlayerBuff || route == game::SkillRoute::Resurrect || manaSwitch);
        if (!onEnemy && !onFriend && route != game::SkillRoute::Duplicate && route != game::SkillRoute::EnemyPlayerBuff) return out;
        std::string error;
        Target* t = TargetFor(race, level, error);
        if (t == nullptr) {
            Result r;
            r.status = "error";
            r.summary = "the second player was not made: " + error;
            out.push_back(r);
            return out;
        }
        ExamClient& tc = *t->client;
        const std::uint32_t target = t->entity;
        const auto after = std::chrono::milliseconds(std::max(times.HoldMs(), lead) + 1500);

        // On an enemy player: struck (its hits, its health or mana down, or states on it); on a player not its foe: refused.
        if (onEnemy) {
            Prepare(c, skill, race, level);
            if (!ReadyTarget(*t, 2)) {
                out.push_back(SkipVariant("pvp", "the second player could not join a free PvP team"));
            } else {
                std::size_t m = 0;
                std::size_t tm = tc.Mark();
                auto [answer, swung] = CastAt(c, skill, target, m);
                c.Pump(after);
                tc.Pump(50ms);
                // A skill of states alone: its roll may miss; up to three casts.
                for (int again = 0; again < 2 && hits == 0 && answer && answer->status == proto::SkillUseStatus::Ok; ++again) {
                    bool any = false;
                    for (const auto& [at, s] : c.Since<proto::EntityStates>(m)) any = any || (s.entity == target && !s.states.empty());
                    if (any) break;
                    Prepare(c, skill, race, level);
                    ReadyTarget(*t, 2);
                    tm = tc.Mark();
                    std::tie(answer, swung) = CastAt(c, skill, target, m);
                    c.Pump(after);
                    tc.Pump(50ms);
                }
                const int seen = int(HitsOn(c, m, target, skill.id).size());
                std::uint64_t damage = 0;
                for (const auto& [at, h] : c.Since<proto::EntityDamaged>(m)) damage += h.attacker == me_ && h.target == target ? h.damage : 0;
                std::optional<std::uint32_t> lowHp, lowMp;
                for (const auto& [at, v] : tc.Since<proto::Vitals>(tm)) {
                    lowHp = std::min(lowHp.value_or(v.hp), v.hp);
                    lowMp = std::min(lowMp.value_or(v.mp), v.mp);
                }
                bool states = false;
                for (const auto& [at, s] : c.Since<proto::EntityStates>(m)) states = states || (s.entity == target && !s.states.empty());
                // Killed by a hit (a level's hits outweigh a player's health): the hits up to it, none after (the rest of a
                // chain has no target, as a monster's death ends it).
                bool killed = false;
                const int upToKill = int(HitsOn(c, m, target, skill.id, &killed).size());
                std::vector<std::string> failed;
                if (!answer || answer->status != proto::SkillUseStatus::Ok) failed.push_back(Answered(answer));
                else if (!swung) failed.push_back("no swing");
                else if (hits > 0 && !dot && !killed && seen != hits) failed.push_back(std::format("{} of {} hits", seen, hits));
                else if (hits > 0 && !dot && killed && (upToKill == 0 || upToKill > hits)) failed.push_back(std::format("{} of {} hits, the player killed", upToKill, hits));
                else if (hits == 0 && traits.states && !states) failed.push_back("no state on the player (three casts)");
                out.push_back(Verdict("pvp", std::move(failed),
                                      std::format("{} of {} hits, {} damage on a player{}{}", seen, hits, damage, states ? ", states on it" : "",
                                                  killed ? ", killed by them" : ""),
                                      {{"hits", seen}, {"damage", damage}, {"states", states}, {"killed", killed}}));
            }
            // (An attack on a player outside PvP is no refusal: a murder, the PK rules' own, PkTests.)
            return out;
        }

        // For another player: its heal, its cure, its buff, its revival, its link.
        if (onFriend || route == game::SkillRoute::Duplicate || route == game::SkillRoute::EnemyPlayerBuff) {
            const bool scorn = route == game::SkillRoute::EnemyPlayerBuff;
            const std::uint8_t team = scorn ? 2 : 0;
            const bool heal = route == game::SkillRoute::Support && traits.heal;
            const bool cure = route == game::SkillRoute::Support && traits.cure && !traits.heal;
            const char* variant = scorn ? "scorn" : route == game::SkillRoute::Duplicate ? "duplicate" : route == game::SkillRoute::Resurrect ? "revive-other"
                                : heal ? "heal-other" : cure ? "cure-other" : route == game::SkillRoute::Taunt ? "taunt-other" : "buff-other";
            Prepare(c, skill, race, level);
            if (!ReadyTarget(*t, team, heal ? 30 : 100, heal ? 30 : 100)) {
                out.push_back(SkipVariant(variant, "the second player could not change its free PvP team"));
                return out;
            }
            // A cure's state: the first its masks take, put on the player (a level 1, 8 s).
            std::uint8_t curable = 0xFF;
            if (cure) {
                const game::CureSpec spec = game::CureOf(skill);
                for (int k = 0; k < 32 && curable == 0xFF; ++k) {
                    const std::uint32_t bit = game::StateBit(game::StateKind(k));
                    if (bit != 0 && ((spec.elementMask | spec.curseMask) & bit) != 0) curable = std::uint8_t(k);
                }
                if (curable == 0xFF) {
                    out.push_back(SkipVariant(variant, "no state its masks take"));
                    return out;
                }
                const std::size_t m = c.Mark();
                c.Send(proto::DevAfflict{target, curable, 1, 8000, 0, 0});
                c.Wait<proto::EntityStates>(m, [&](const proto::EntityStates& s) { return s.entity == target && !s.states.empty(); }, 3s);
            }
            if (route == game::SkillRoute::Resurrect) {
                const std::size_t m = tc.Mark();
                tc.Send(proto::DevDie{});
                tc.Wait<proto::Vitals>(m, [](const proto::Vitals& v) { return v.hp == 0; }, 3s);
            }
            tc.Pump(50ms);
            std::uint32_t hpBefore = 0, mpBefore = 0, maxHp = 0;
            for (const auto& [at, v] : tc.Since<proto::Vitals>(0)) hpBefore = v.hp, mpBefore = v.mp, maxHp = v.maxHp;
            const std::size_t tm = tc.Mark();
            std::size_t m = 0;
            const auto [answer, swung] = CastAt(c, skill, target, m);
            c.Pump(after);
            tc.Pump(100ms);
            std::vector<std::string> failed;
            std::string text;
            const bool aboveRevive = route == game::SkillRoute::Resurrect && t->level > game::ReviveOf(skill).maxLevel;
            const int copyCap = route == game::SkillRoute::Duplicate ? assets::FindSkillParam(skill, assets::SkillTag("msch")).Int(1) : 0;
            if (route == game::SkillRoute::Duplicate && t->level > copyCap) {
                // A player above what it copies: refused.
                if (!answer || answer->status != proto::SkillUseStatus::Target) failed.push_back(Answered(answer) + " (the refusal for a level above it expected)");
                text = std::format("refused on a player of level {} (it copies to {})", t->level, copyCap);
            } else if (aboveRevive) {
                // The dead player above what it raises: refused.
                if (!answer || answer->status != proto::SkillUseStatus::ReviveLevel) failed.push_back(Answered(answer) + " (the refusal for a level above it expected)");
                text = std::format("refused on a dead player of level {} (it raises to {})", t->level, game::ReviveOf(skill).maxLevel);
                tc.Send(proto::DevReset{proto::DevReset::kVitals});
            } else if (!answer || answer->status != proto::SkillUseStatus::Ok) {
                failed.push_back(Answered(answer));
            } else if (heal) {
                std::uint32_t hp1 = hpBefore, mp1 = mpBefore;
                for (const auto& [at, v] : tc.Since<proto::Vitals>(tm)) hp1 = std::max(hp1, v.hp), mp1 = std::max(mp1, v.mp);
                text = std::format("the player's HP {} -> {}, MP {} -> {}", hpBefore, hp1, mpBefore, mp1);
                // More than a regeneration's tick: 3 % of its most.
                const std::uint32_t step = std::max<std::uint32_t>(1, maxHp * 3 / 100);
                if (hp1 < hpBefore + step && mp1 < mpBefore + step) failed.push_back("neither its HP nor its MP rose (more than a regeneration)");
            } else if (cure) {
                bool gone = false;
                for (const auto& [at, s] : c.Since<proto::EntityStates>(m)) {
                    if (s.entity != target) continue;
                    const bool still = std::any_of(s.states.begin(), s.states.end(), [&](const proto::StateRecord& r) { return r.kind == curable; });
                    gone = gone || !still;
                }
                text = std::format("state {} put on the player, {}", int(curable), gone ? "cured" : "still on it");
                if (!gone) failed.push_back("the state was not cured");
            } else if (route == game::SkillRoute::Resurrect) {
                const bool offered = tc.Wait<proto::RevivalOffered>(tm, [&](const proto::RevivalOffered& o) { return o.skill == skill.id || o.caster == me_; }, 2s).has_value();
                text = offered ? "the revival offered to the dead player" : "no revival offered";
                if (!offered) failed.push_back("no revival offered");
                tc.Send(proto::RevivalAnswer{1});
                tc.Pump(200ms);
                tc.Send(proto::DevReset{proto::DevReset::kVitals}); // risen anyway (the lab's reset raises the dead)
            } else if (route == game::SkillRoute::Duplicate) {
                bool on = false;
                for (const auto& [at, b] : c.Since<proto::BuffAdded>(m)) on = on || b.skill == skill.id;
                text = on ? "the caster in the player's look" : "no buff on the caster";
                if (!on) failed.push_back("no buff on the caster");
            } else if (route == game::SkillRoute::Taunt) {
                text = "cast on the player";
            } else {
                bool on = false;
                for (const auto& [at, b] : tc.Since<proto::BuffAdded>(tm)) on = on || b.skill == skill.id;
                text = on ? "its buff on the player" : "no buff on the player";
                if (!on) failed.push_back("no buff on the player");
            }
            out.push_back(Verdict(variant, std::move(failed), text, json::object()));
            if (scorn) ReadyTarget(*t, 0);
        }
        return out;
    }

    // The armour kinds against a mastery's first attack on players (L6): the second player in its
    // race's three kinds (garment, protector, armour; the Europeans' robe, light, heavy), the attack three times on each:
    // the less of the defence its lane meets (physical or magical), the more its hits take (criticals and blocks aside).
    std::optional<Result> Armour(ExamClient& c, const assets::MasteryRow& mastery, const std::map<std::uint32_t, const assets::SkillRow*>& learned) {
        // Its hardest-hitting attack on players (the greatest 'att' power of its rank learned): its damage well above
        // the noise of the attack's own range, the state skills' small hits aside.
        const game::SkillCatalogueEntry* pick = nullptr;
        int best = 0;
        for (const game::SkillCatalogueEntry& e : catalogue_) {
            if (e.mastery->id != mastery.id) continue;
            const assets::SkillRow& s = e.First();
            if (game::SkillRouteOf(s) != game::SkillRoute::Attack || !s.targetEnemyPlayer || !s.attack || game::HitsOf(*skills_, s) == 0) continue;
            if (assets::FindSkillParam(s, assets::SkillTag("lks2"))) continue;
            const auto rank = learned.find(s.group);
            const int power = game::AttackOf(rank != learned.end() ? *rank->second : s).power;
            if (pick == nullptr || power > best) pick = &e, best = power;
        }
        if (pick == nullptr) return std::nullopt;
        // Its rank learned (the strongest the character has): the casts' damage well above the noise of the first rank's.
        const auto rank = learned.find(pick->First().group);
        const assets::SkillRow& skill = rank != learned.end() ? *rank->second : pick->First();
        const int race = skill.race == 1 ? 1 : 0;
        const int level = LevelFor(mastery.id);
        std::string error;
        Target* t = TargetFor(race, level, error);
        if (t == nullptr) return std::nullopt;
        const bool magical = (game::AttackOf(skill).kind & 8) != 0;
        const std::array<int, 3> kinds = race == 1 ? std::array<int, 3>{9, 10, 11} : std::array<int, 3>{1, 2, 3};
        json rows = json::array();
        std::vector<std::pair<double, double>> seen; // defence, mean damage
        for (const int kind : kinds) {
            Outfit o;
            for (int part = 1; part <= 6; ++part) {
                if (const assets::ItemRow* p = BestItem(race, level, kind, part)) o.items.push_back(p);
            }
            t->client->Send(proto::DevReset{std::uint8_t(proto::DevReset::kAll | proto::DevReset::kPlace), kTargetPlace});
            t->client->Pump(150ms);
            Wear(*t->client, o, t->worn);
            std::size_t m = t->client->Mark();
            t->client->Send(proto::DevSheet{});
            const auto sheet = t->client->Next<proto::DevSheetAnswer>(m, 3s);
            double defence = 0;
            for (const proto::DevNumber& n : sheet ? sheet->numbers : std::vector<proto::DevNumber>{}) {
                if (n.name == (magical ? "magDefence" : "phyDefence")) defence = n.value;
            }
            std::uint64_t sum = 0;
            int counted = 0;
            for (int i = 0; i < 6; ++i) {
                Prepare(c, skill, race, level);
                if (!ReadyTarget(*t, 2)) break;
                std::size_t cm = 0;
                CastAt(c, skill, t->entity, cm);
                c.Pump(std::chrono::milliseconds(game::TimesOf(*skills_, skill).HoldMs() + 800));
                for (const auto& [at, h] : c.Since<proto::EntityDamaged>(cm)) {
                    if (h.attacker != me_ || h.target != t->entity || (h.flags & (proto::kDamageCritical | proto::kDamageBlocked | proto::kDamageState)) != 0) continue;
                    sum += h.damage;
                    ++counted;
                }
            }
            const double mean = counted > 0 ? double(sum) / counted : 0.0;
            seen.push_back({defence, mean});
            rows.push_back({{"kind", kind}, {"defence", defence}, {"meanDamage", mean}, {"hits", counted}});
        }
        ReadyTarget(*t, 0);
        std::vector<std::string> failed;
        for (std::size_t i = 0; i < seen.size(); ++i) {
            for (std::size_t j = 0; j < seen.size(); ++j) {
                if (seen[i].first > seen[j].first * 1.1 && seen[i].second > seen[j].second * 1.15 && seen[j].second > 0) {
                    failed.push_back(std::format("{} defence took {:.0f} a hit, more than {} defence's {:.0f}", seen[i].first, seen[i].second, seen[j].first, seen[j].second));
                }
            }
        }
        std::string text = std::format("{} with {}:", magical ? "magical" : "physical", skill.codename);
        for (std::size_t i = 0; i < seen.size(); ++i) text += std::format(" kind {} defence {:.0f} -> {:.0f} a hit;", kinds[i], seen[i].first, seen[i].second);
        Result r = Verdict("armour", std::move(failed), text, {{"kinds", rows}});
        r.code = pick->code;
        return r;
    }

    // A normal monster about the character's level (the nearest below it first): a field monster of no job.
    std::string MonsterFor(int level) {
        if (const auto it = monsterFor_.find(level); it != monsterFor_.end()) return it->second;
        const assets::ObjectRow* best = nullptr;
        for (const auto& [code, r] : characters_->rows()) {
            if (r.typeId[0] != 1 || r.typeId[1] != 2 || r.typeId[2] != 1 || r.typeId[3] != 1 || r.rarity != 0 || r.level < 1) continue;
            if (!code.starts_with("MOB_CH_") && !code.starts_with("MOB_EU_") && !code.starts_with("MOB_AM_") && !code.starts_with("MOB_KK_") &&
                !code.starts_with("MOB_OA_") && !code.starts_with("MOB_RM_") && !code.starts_with("MOB_TK_")) {
                continue;
            }
            if (code.find("_CLON") != std::string::npos || code.find("_STRONG") != std::string::npos) continue;
            if (r.walkSpeed <= 0.0f && r.runSpeed <= 0.0f) continue; // one that never moves (a seal stone) cannot flee
            const int d = std::abs(r.level - level), bd = best != nullptr ? std::abs(best->level - level) : 1 << 30;
            if (best == nullptr || d < bd || (d == bd && code < best->codename)) best = &r;
        }
        const std::string code = best != nullptr ? best->codename : "MOB_CH_MANGNYANG";
        monsterFor_[level] = code;
        return code;
    }

    void Write(const game::SkillCatalogueEntry& e, int scenario, const Result& r) {
        if (r.status == "pass") ++report_.passed;
        else if (r.status == "skip") ++report_.skipped;
        else ++report_.failed;
        core::LogInfo("SroSkillLab: {} {} ({}{}{}): {} {}", masteryName_, e.code, scenario, r.variant.empty() ? "" : " ", r.variant, r.status, r.summary);
        results_->Query("INSERT INTO sro_vt_account.sr_lab_results (run_id, code, scenario, variant, status, summary, expected, observed) "
                        "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb)",
                        {report_.run, e.code, scenario, r.variant, r.status, r.summary, r.expected.dump(), r.observed.dump()});
    }

    static constexpr std::uint32_t kChinaFigure = 100, kEuropeFigure = 200;
    // In the field west of Constantinople (its navigation in; the town itself, 77..81 x 103..107, is safe ground).
    static constexpr proto::WorldPosition kPlace{assets::RegionId(76, 105), 600.0f, 140.0f, 900.0f};
    static constexpr proto::WorldPosition kTargetPlace{kPlace.region, kPlace.x + 30.0f, kPlace.y, kPlace.z}; // scenario 4's second player

    const ExamOptions& options_;
    ExamReport& report_;
    assets::FileSystem files_;
    std::shared_ptr<assets::CharacterTable> characters_;
    std::shared_ptr<assets::SkillTable> skills_;
    std::shared_ptr<assets::ItemTable> items_;
    LabData lab_;
    std::vector<game::SkillCatalogueEntry> catalogue_;
    std::unique_ptr<Database> results_, exam_;
    std::string name_, examUrl_, pem_;
    core::JobSystem jobs_{2};
    net::NetService net_{1};
    GameServer game_;
    GameServerConfig config_;
    int made_ = 0;
    std::uint32_t me_ = 0;
    std::string masteryName_;
    std::map<int, std::uint32_t> worn_; // place -> item worn there now (by the exam)
    std::array<Target, 2> targets_;     // scenario 4's second players, by race
    int masteryIndex_ = 0;              // the mastery being run, in the order they run
    std::uint8_t casterTeam_ = 0;
    bool casterTeamed_ = false;
    std::map<int, std::string> monsterFor_;
};

} // namespace

std::int64_t NewExamRun(const std::string& resultsUrl, const std::string& build, const std::string& filter, const std::string& scenarios, std::string& error) {
    auto db = Database::Connect(resultsUrl, error);
    if (!db) return 0;
    const QueryResult run = db->Query("INSERT INTO sro_vt_account.sr_lab_runs (build, filter, scenarios) VALUES ($1, $2, $3) RETURNING id", {build, filter, scenarios});
    if (!run.Ok() || run.Rows() != 1) return (error = "the run: " + run.Error()), 0;
    return run.Int(0, 0);
}

bool RunExam(const ExamOptions& options, ExamReport& report, std::string& error) {
    Exam exam(options, report);
    return exam.Run(error);
}

} // namespace sr::server
