// The world screen's crowd (WorldScreen.h, "The crowd"): the test walkers as a stand-in for a crowded
// game. The character tables load on a job, then a second job plans every walker (its figure and
// random equipment dressed as the game screen dresses players, or a monster row; its skills and
// buff); the game thread then gives each walker its look and, every frame, its skill's clips and the
// effects of world/StageEffects.h (as the game screen's), the hits on a neighbour, the buff loops and
// the glows of rare weapons and shields.

#include "screens/WorldScreen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <sstream>

#include <imgui.h>

#include "sr/assets/Dress.h"
#include "sr/assets/GameData.h"
#include "sr/assets/ItemGlow.h"
#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Trace.h"
#include "title/TitleShared.h"
#include "world/StageEffects.h"

namespace sr::client {

namespace {

constexpr std::uint32_t kAttack = 2, kDamage = 3, kRun = 7; // .bsr animation states

struct Rng {
    std::uint32_t state;
    std::uint32_t Next() {
        std::uint32_t x = state += 0x9e3779b9U;
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }
    float Unit() { return static_cast<float>(Next() & 0xFFFFFF) / static_cast<float>(0x1000000); }
    bool Chance(float p) { return Unit() < p; }
    template <typename T>
    const T& Pick(const std::vector<T>& v) { return v[Next() % v.size()]; }
};

// One walker's part, decided on the plan job.
struct WalkerPlan {
    bool player = false;
    WorldScreen::StageLook look;
    std::vector<std::uint32_t> worn;   // itemdata ids (players)
    std::vector<std::uint32_t> skills; // skilldata ids it swings
    std::uint32_t buff = 0;            // a buff it wears (its looping effects), 0 none
    std::string codename;              // characterdata (where hits land, its blood)
    int rare = 0, plus = 0;            // how many worn items are rare, enhanced (+N: data only)
};

struct Plan {
    std::atomic<bool> ready{false};
    std::vector<WalkerPlan> walkers;
};

int Degree(const assets::ItemRow& row) { return row.itemClass > 0 ? (row.itemClass - 1) / 3 + 1 : 1; }

// The equipment and skills to pick from, by race (0 China, 1 Europe).
struct Catalogue {
    // Armour by race, sex, place (0..5) and class (1 clothes, 2 light, 3 heavy).
    std::map<std::tuple<int, bool, int, int>, std::vector<const assets::ItemRow*>> armour;
    std::vector<const assets::ItemRow*> weapons[2], rareWeapons[2], shields[2], rareShields[2];
    std::map<std::pair<int, int>, std::vector<std::uint32_t>> attacks; // race, weapon kind (TID4) -> skills
    std::vector<std::uint32_t> buffs[2];                                  // skills with a looping effect

    void Build(const CharacterData& t) {
        for (const auto& [code, row] : t.items.rows()) {
            if (!row.inService || row.country < 0 || row.country > 1 || (row.modelPath.empty() && row.linkedCodename.empty())) continue;
            if (code.find("_DEF") != std::string::npos) continue; // the create screen's samples
            const int place = assets::EquipSlot(row);
            const int race = row.country;
            const bool rare = row.Rare();
            if (place >= 0 && place <= 5) {
                const int kind = row.typeId[2] > 8 ? row.typeId[2] - 8 : row.typeId[2];
                for (const bool male : {false, true}) {
                    if (row.sex == 2 || (row.sex == 1) == male) armour[{race, male, place, kind}].push_back(&row);
                }
            } else if (place == 6) {
                (rare ? rareWeapons : weapons)[race].push_back(&row);
            } else if (place == 7) {
                (rare ? rareShields : shields)[race].push_back(&row);
            }
        }
        for (const auto& [id, s] : t.skills.rows()) {
            if (!s.inService || s.activity == 0) continue;
            const int race = s.codename.starts_with("SKILL_CH_") ? 0 : s.codename.starts_with("SKILL_EU_") ? 1 : -1;
            if (race < 0) continue;
            const bool named = !s.baseName.empty() && s.baseName != "xxx";
            const assets::SkillAnimation* anim = t.skillAnimations.Find(named ? s.baseName : s.codename);
            if (s.attack && anim != nullptr && !anim->shot.empty()) {
                for (const int kind : s.weaponKinds) {
                    if (kind != 255) attacks[{race, kind}].push_back(id);
                }
            }
            const auto* stages = StageEffects::StagesOf(t.skillAnimations, s);
            if (!s.attack && stages != nullptr && StageEffects::HasLoop(*stages)) buffs[race].push_back(id);
        }
        // A stable order (the tables are hash maps): the same run picks the same.
        const auto byId = [](const assets::ItemRow* a, const assets::ItemRow* b) { return a->refObjId < b->refObjId; };
        for (auto& [key, list] : armour) std::sort(list.begin(), list.end(), byId);
        for (int r = 0; r < 2; ++r) {
            for (auto* list : {&weapons[r], &rareWeapons[r], &shields[r], &rareShields[r]}) std::sort(list->begin(), list->end(), byId);
            std::sort(buffs[r].begin(), buffs[r].end());
        }
        for (auto& [key, list] : attacks) std::sort(list.begin(), list.end());
    }

    // One of `list` near a degree (its own degree's when there is one).
    static const assets::ItemRow* Near(Rng& rng, const std::vector<const assets::ItemRow*>& list, int degree) {
        if (list.empty()) return nullptr;
        std::vector<const assets::ItemRow*> same;
        for (const assets::ItemRow* row : list) {
            if (Degree(*row) == degree) same.push_back(row);
        }
        return same.empty() ? rng.Pick(list) : rng.Pick(same);
    }
};

void PlanPlayer(Rng& rng, const CharacterData& t, const assets::FileSystem& fs, const Catalogue& cat, WalkerPlan& out) {
    using Race = game::CreationRules::Race;
    const int race = rng.Chance(0.5f) ? 1 : 0;
    const bool male = rng.Chance(0.5f);
    const std::vector<std::uint32_t> figures = t.rules.creation.FiguresOf(race == 0 ? Race::China : Race::Europe, male);
    if (figures.empty()) return;
    const std::uint32_t figure = rng.Pick(figures);
    const int degree = 1 + static_cast<int>(rng.Next() % 10);
    const auto wearable = [&](const assets::ItemRow* row) { return row != nullptr && !assets::WornModels(fs, t.items, *row).empty(); };
    // The weapon (a third of them rare), a shield in the free hand of most one-handed ones.
    const assets::ItemRow* weapon = nullptr;
    for (int attempt = 0; attempt < 6 && !wearable(weapon); ++attempt) {
        const bool rare = rng.Chance(0.35f) && !cat.rareWeapons[race].empty();
        weapon = Catalogue::Near(rng, rare ? cat.rareWeapons[race] : cat.weapons[race], degree);
    }
    std::vector<std::uint32_t> worn;
    if (wearable(weapon)) {
        worn.push_back(weapon->refObjId);
        const int kind = weapon->typeId[3];
        if (assets::OneHanded(*weapon) && kind != 9 && kind != 13 && kind != 6 && kind != 12 && rng.Chance(0.7f)) {
            const bool rare = rng.Chance(0.35f) && !cat.rareShields[race].empty();
            const assets::ItemRow* shield = Catalogue::Near(rng, rare ? cat.rareShields[race] : cat.shields[race], degree);
            if (wearable(shield)) worn.push_back(shield->refObjId);
        }
        const auto attacks = cat.attacks.find({race, kind});
        if (attacks != cat.attacks.end()) out.skills = attacks->second;
    }
    // Armour of one class, most places filled.
    const int armourClass = 1 + static_cast<int>(rng.Next() % 3);
    for (int place = 0; place <= 5; ++place) {
        if (!rng.Chance(0.85f)) continue;
        const auto list = cat.armour.find({race, male, place, armourClass});
        if (list == cat.armour.end()) continue;
        const assets::ItemRow* row = Catalogue::Near(rng, list->second, degree);
        if (wearable(row)) worn.push_back(row->refObjId);
    }
    assets::DressedCharacter dressed;
    std::string error;
    const std::uint8_t shape = assets::PackShape(static_cast<int>(rng.Next() % 5), static_cast<int>(rng.Next() % 5));
    if (!assets::Dress(fs, t.characters, t.items, figure, worn, shape, dressed, error)) return;
    out.player = true;
    out.look.model = dressed.Key();
    out.look.stance = dressed.stance;
    out.look.height = dressed.height;
    out.look.volume = dressed.volume;
    out.look.state = kRun;
    out.worn = std::move(worn);
    for (const std::uint32_t id : out.worn) {
        const assets::ItemRow* row = t.items.Find(id);
        out.rare += row != nullptr && row->Rare();
        out.plus += rng.Chance(0.3f); // +N carries no look of its own yet (the plus glow is not read)
    }
    if (const assets::ObjectRow* row = t.characters.Find(figure)) out.codename = row->codename;
    if (!cat.buffs[race].empty() && rng.Chance(0.35f)) out.buff = rng.Pick(cat.buffs[race]);
}

// --walker-items: the first walker wears these items (itemdata codenames), on a figure of the first
// one's race. False when none of them can be worn.
bool PlanChosen(const CharacterData& t, const assets::FileSystem& fs, const std::vector<std::string>& items, WalkerPlan& out) {
    std::vector<std::uint32_t> worn;
    int race = -1;
    // Besides the codenames: "woman", "height:N", "volume:N" (0..4) for the wearer's body.
    bool male = true;
    int height = 2, volume = 2;
    std::size_t figureIndex = 0;
    for (const std::string& code : items) {
        if (code.starts_with("figure:")) {
            figureIndex = std::size_t(std::max(0, std::atoi(code.c_str() + 7)));
            continue;
        }
        if (code == "woman") {
            male = false;
            continue;
        }
        if (code.starts_with("height:") || code.starts_with("volume:")) {
            const int n = std::clamp(std::atoi(code.c_str() + 7), 0, 4);
            (code[0] == 'h' ? height : volume) = n;
            continue;
        }
        const assets::ItemRow* row = t.items.Find(std::string_view(code));
        if (row == nullptr || assets::WornModels(fs, t.items, *row).empty()) {
            core::LogWarning("world: crowd: --walker-items: {} cannot be worn", code);
            continue;
        }
        if (race < 0) race = row->country == 1 ? 1 : 0;
        worn.push_back(row->refObjId);
    }
    if (worn.empty()) return false;
    using Race = game::CreationRules::Race;
    const std::vector<std::uint32_t> figures = t.rules.creation.FiguresOf(race == 0 ? Race::China : Race::Europe, male);
    if (figures.empty()) return false;
    assets::DressedCharacter dressed;
    std::string error;
    if (!assets::Dress(fs, t.characters, t.items, figures[std::min(figureIndex, figures.size() - 1)], worn, assets::PackShape(height, volume), dressed, error)) {
        core::LogWarning("world: crowd: --walker-items: {}", error);
        return false;
    }
    out.player = true;
    out.look.model = dressed.Key();
    out.look.stance = dressed.stance;
    out.look.height = dressed.height;
    out.look.volume = dressed.volume;
    out.look.state = kRun;
    out.worn = std::move(worn);
    for (const std::uint32_t id : out.worn) {
        const assets::ItemRow* row = t.items.Find(id);
        out.rare += row != nullptr && row->Rare();
    }
    if (const assets::ObjectRow* row = t.characters.Find(figures.front())) out.codename = row->codename;
    return true;
}

// `kinds` different general monsters (in service, with skills, their model in the game files), one
// row a model, drawn at random but the same every run.
std::vector<std::string> DrawMonsterKinds(const CharacterData& t, const assets::FileSystem& fs, int kinds) {
    std::map<std::string, std::string> byModel; // normalized model -> its path as the game files spell it
    for (const auto& [code, row] : t.characters.rows()) {
        if (!row.inService || !code.starts_with("MOB_") || row.modelPath.empty() || row.rarity != 0 || row.skills[0] == 0) continue;
        std::string path = core::NormalizeAssetPath(row.modelPath);
        if (!path.starts_with("res\\")) path = "res\\" + path;
        if (!byModel.contains(path) && fs.Exists(path)) byModel.emplace(path, path);
    }
    std::vector<std::string> all;
    for (auto& [model, path] : byModel) all.push_back(path);
    Rng rng{20261104u};
    for (std::size_t i = all.size(); i > 1; --i) std::swap(all[i - 1], all[rng.Next() % i]);
    all.resize(std::min(all.size(), static_cast<std::size_t>(std::max(0, kinds))));
    core::LogInfo("world: crowd: {} monster kinds drawn from {} general monster models", all.size(), byModel.size());
    return all;
}

void BuildPlan(const CharacterData& t, const assets::FileSystem& fs, std::size_t count, float playerShare,
               std::vector<std::string> monsterPaths, int monsterKinds, const std::vector<std::string>& chosenItems,
               const std::string& chosenSkill, Plan& out) {
    SR_ZONE("world: crowd plan");
    Catalogue cat;
    cat.Build(t);
    if (monsterKinds > 0) monsterPaths = DrawMonsterKinds(t, fs, monsterKinds);
    // The monsters of the list as characterdata rows (by their model; one with skills first).
    std::vector<const assets::ObjectRow*> monsters(monsterPaths.size(), nullptr);
    for (const auto& [code, row] : t.characters.rows()) {
        if (!row.inService || !code.starts_with("MOB_") || row.modelPath.empty()) continue;
        const std::string model = core::NormalizeAssetPath(row.modelPath);
        for (std::size_t i = 0; i < monsterPaths.size(); ++i) {
            const std::string want = core::NormalizeAssetPath(monsterPaths[i]);
            if (want != model && want != core::NormalizeAssetPath("res\\" + row.modelPath)) continue;
            const assets::ObjectRow*& best = monsters[i];
            const auto score = [](const assets::ObjectRow* r) { return (r->skills[0] != 0 ? 2 : 0) + (r->rarity == 0 ? 1 : 0); };
            if (best == nullptr || score(&row) > score(best) || (score(&row) == score(best) && row.refObjId < best->refObjId)) best = &row;
        }
    }
    out.walkers.resize(count);
    const std::size_t players = static_cast<std::size_t>(std::lround(double(count) * std::clamp(playerShare, 0.0f, 1.0f)));
    std::size_t monsterCount = 0; // the monsters take the kinds in turn (not by the walker's index: players sit between them)
    for (std::size_t i = 0; i < count; ++i) {
        Rng rng{static_cast<std::uint32_t>(i) * 7919u + 17u};
        WalkerPlan& w = out.walkers[i];
        // Players and monsters mixed along the list (every walker's place is random anyway).
        const bool player = (i * players) / count != ((i + 1) * players) / count;
        if (i == 0 && !chosenItems.empty() && PlanChosen(t, fs, chosenItems, w)) {
            // The attacks of its weapon, as a planned player's.
            for (const std::uint32_t id : w.worn) {
                const assets::ItemRow* row = t.items.Find(id);
                if (row == nullptr || assets::EquipSlot(*row) != 6) continue;
                const auto attacks = cat.attacks.find({row->country == 1 ? 1 : 0, row->typeId[3]});
                if (attacks != cat.attacks.end()) w.skills = attacks->second;
            }
        } else if (player) {
            PlanPlayer(rng, t, fs, cat, w);
        }
        if (i == 1 && !chosenSkill.empty()) w.skills.clear(); // its target stands still
        if (i == 0 && !chosenSkill.empty()) {
            // --walker-skill: the first walker casts that one only.
            if (const assets::SkillRow* skill = t.skills.Find(std::string_view(chosenSkill))) {
                w.skills.assign(1, skill->id);
            } else {
                core::LogWarning("world: crowd: --walker-skill: no skill {}", chosenSkill);
            }
        }
        if (w.player || monsterPaths.empty()) continue;
        const std::size_t m = monsterCount++ % monsterPaths.size();
        w.look.model = monsterPaths[m];
        w.look.state = kRun;
        if (const assets::ObjectRow* row = monsters[m]) {
            w.look.height = float(row->scalePercent) / 100.0f;
            w.codename = row->codename;
            for (const std::uint32_t id : row->skills) {
                if (id != 0 && t.skills.Find(id) != nullptr) w.skills.push_back(id);
            }
        }
    }
}

} // namespace

struct WorldScreen::Crowd {
    std::shared_ptr<CharacterData> tables;
    core::JobCounter tablesJob, planJob;
    std::shared_ptr<Plan> plan;
    bool planning = false, dressed = false;
    double skillEvery = 6.0;
    float playerShare = 0.5f;
    int monsterKinds = 0;                 // --walker-monsters: drawn from characterdata (0: the list's)
    std::vector<std::string> monsters;
    std::vector<std::string> chosenItems; // --walker-items
    std::string chosenSkill;              // --walker-skill
    struct Step {
        std::uint32_t state = 0;
        double until = 0;
        std::uint32_t take = 0;
        std::string set; // the set its clip comes from (a skill's group; empty: the stance)
    };
    struct Walker {
        WalkerPlan plan;
        double nextSkill = 0;
        std::vector<Step> steps;
        std::uint32_t runTake = 0;
        std::uint32_t lastSkill = 0;
        bool started = false;               // its glows and buff loops
        std::vector<StageEffects::Handle> kept; // those effects (stopped when it is hidden)
    };
    std::vector<Walker> walkers;
    // The pool's slots by kind, and how many of each are shown (the World tools' sliders).
    std::vector<std::size_t> playerSlots, monsterSlots;
    int shownPlayers = 0, shownMonsters = 0;
    std::size_t shown = 0; // --characters: shown at first
    struct Hit {
        std::size_t attacker = 0, target = 0;
        std::uint32_t skill = 0;
        double at = 0;
    };
    std::vector<Hit> hits;
    StageEffects effects;
    std::uint32_t takes = 1;
    Rng rng{12345u};
    std::size_t casts = 0, landed = 0;
};

void WorldScreen::StartCrowd(ScreenContext& context, std::size_t shown) {
    auto k = std::make_shared<Crowd>();
    k->shown = shown;
    k->skillEvery = std::max(0.5, context.config.GetDouble("walker-skill-every", 6.0));
    k->playerShare = static_cast<float>(context.config.GetDouble("walker-players", 0.5));
    k->monsterKinds = context.config.GetInt("walker-monsters", 0);
    // Developer close-ups (with --walker-at): the first walker's items and the one skill it casts.
    std::stringstream items(context.config.GetString("walker-items", ""));
    for (std::string code; std::getline(items, code, ',');) {
        if (!code.empty()) k->chosenItems.push_back(code);
    }
    k->chosenSkill = context.config.GetString("walker-skill", "");
    // The list's monsters stay (as characterdata rows); its player models give way to the game's
    // figures. The list's models go back; the walkers wait for their looks.
    for (CharacterModel& m : characterModels_) {
        if (!core::NormalizeAssetPath(m.path).starts_with("res\\char\\")) k->monsters.push_back(m.path);
        context.assets.Release(m.model);
    }
    characterModels_.clear();
    for (Character& c : characters_) c.active = false;
    assets_ = &context.assets;
    k->tables = LoadCharacterData(context.jobs, context.files, k->tablesJob);
    crowd_ = std::move(k);
}

bool WorldScreen::CrowdWaiting() const { return crowd_ != nullptr && !crowd_->dressed; }

void WorldScreen::ExitCrowd(ScreenContext& context) {
    if (!crowd_) return;
    // Both jobs read the file system.
    context.jobs.Wait(crowd_->tablesJob);
    context.jobs.Wait(crowd_->planJob);
    core::LogInfo("world: crowd: {} skills cast, {} hits landed", crowd_->casts, crowd_->landed);
    crowd_.reset();
}

void WorldScreen::UpdateCrowd(ScreenContext& context) {
    if (!crowd_) return;
    SR_ZONE("world: crowd");
    Crowd& k = *crowd_;
    const double now = context.time;
    if (!k.dressed) {
        if (!k.planning) {
            if (!k.tables->Ready()) return;
            if (!k.tables->Usable()) {
                core::LogWarning("world: crowd: no character tables; the walkers stay out");
                k.dressed = true;
                return;
            }
            k.plan = std::make_shared<Plan>();
            context.jobs.Run(
                [plan = k.plan, tables = k.tables, fs = &context.files, count = characters_.size(), share = k.playerShare,
                 monsters = k.monsters, kinds = k.monsterKinds, items = k.chosenItems, skill = k.chosenSkill] {
                    BuildPlan(*tables, *fs, count, share, monsters, kinds, items, skill, *plan);
                    plan->ready.store(true, std::memory_order_release);
                },
                core::JobPriority::Streaming, &k.planJob);
            k.planning = true;
            return;
        }
        if (!k.plan->ready.load(std::memory_order_acquire)) return;
        // Every walker its look (one with none, a player whose figure failed, stays out).
        k.walkers.resize(characters_.size());
        std::size_t rare = 0, plus = 0, buffs = 0;
        for (std::size_t i = 0; i < characters_.size(); ++i) {
            Crowd::Walker& w = k.walkers[i];
            w.plan = std::move(k.plan->walkers[i]);
            if (w.plan.look.model.empty()) continue;
            (w.plan.player ? k.playerSlots : k.monsterSlots).push_back(i);
            rare += w.plan.rare;
            plus += w.plan.plus;
            buffs += w.plan.buff != 0;
        }
        k.plan.reset();
        k.dressed = true;
        // The first --characters of the pool shown, players and monsters in its share.
        const std::size_t players = std::min(k.playerSlots.size(), static_cast<std::size_t>(std::lround(double(k.shown) * k.playerShare)));
        k.shownPlayers = static_cast<int>(players);
        k.shownMonsters = static_cast<int>(std::min(k.monsterSlots.size(), k.shown - std::min(k.shown, players)));
        core::LogInfo("world: crowd: a pool of {} walkers, {} players ({} rare items, {} enhanced, {} buffed); {} players and {} monsters shown",
                      characters_.size(), k.playerSlots.size(), rare, plus, buffs, k.shownPlayers, k.shownMonsters);
        return;
    }
    if (k.walkers.empty()) return;
    const CharacterData& t = *k.tables;

    // The sliders: the first N of each kind shown, the rest hidden (their effects stopped).
    bool hid = false;
    const auto showFirst = [&](const std::vector<std::size_t>& slots, int count) {
        for (std::size_t j = 0; j < slots.size(); ++j) {
            const std::size_t i = slots[j];
            Character& c = characters_[i];
            Crowd::Walker& w = k.walkers[i];
            const bool want = static_cast<int>(j) < count;
            if (want && !c.active) {
                c.look = w.plan.look;
                c.type = StageModel(c.look);
                c.nextType = kNoType;
                c.dressed = true;
                c.still = false;
                c.active = true;
                w.started = false;
                w.steps.clear();
                w.nextSkill = now + 1.0 + k.rng.Unit() * k.skillEvery;
            } else if (!want && c.active) {
                for (const StageEffects::Handle h : w.kept) effects_.Stop(h);
                w.kept.clear();
                w.steps.clear();
                w.started = false;
                std::erase_if(k.hits, [&](const Crowd::Hit& hit) { return hit.attacker == i || hit.target == i; });
                ClearStage(i);
                c.dressed = false;
                c.still = false;
                hid = true;
            }
        }
    };
    showFirst(k.playerSlots, k.shownPlayers);
    showFirst(k.monsterSlots, k.shownMonsters);
    if (hid) ReleaseUnused(context);
    k.effects.Fly(*this, now);

    // One walker's skill: its clips in order (as the game screen's), its stages, a hit on the
    // nearest walker within reach.
    const auto cast = [&](std::size_t i) {
        Crowd::Walker& w = k.walkers[i];
        Character& c = characters_[i];
        w.nextSkill = now + k.skillEvery * (0.5 + k.rng.Unit());
        if (w.plan.skills.empty()) return;
        const assets::SkillRow* skill = t.skills.Find(k.rng.Pick(w.plan.skills));
        if (skill == nullptr) return;
        std::size_t target = StageEffects::kNone;
        float nearest = 120.0f;
        for (std::size_t j = 0; j < characters_.size(); ++j) {
            const Character& o = characters_[j];
            if (j == i || !o.spawned || !o.dressed) continue;
            const float d = std::hypot(o.x - c.x, o.z - c.z);
            if (d < nearest) {
                nearest = d;
                target = j;
            }
        }
        if (target != StageEffects::kNone && nearest > 1.0f) {
            c.dirX = (characters_[target].x - c.x) / nearest;
            c.dirZ = (characters_[target].z - c.z) / nearest;
        }
        const bool named = !skill->baseName.empty() && skill->baseName != "xxx";
        const assets::SkillAnimation* anim = t.skillAnimations.Find(named ? skill->baseName : skill->codename);
        // Its phases (skilldata cols 11, 12, 13), kept short for a crowd that keeps moving.
        const double ready = std::min(1.0, std::max(0, skill->preparingMs) / 1000.0);
        const double wait = std::min(1.5, std::max(0, skill->castingMs) / 1000.0);
        const double shot = std::clamp(skill->durationMs > 0 ? double(skill->durationMs) / 1000.0 : 0.8, 0.4, 1.6);
        const std::uint32_t turn = k.rng.Next();
        // The clips as the game screen plays them (a phase with no clip runs in the next one).
        const bool readyClip = ready > 0.0 && anim != nullptr && !anim->ready.empty();
        const bool waitClip = wait > 0.0 && anim != nullptr && !anim->wait.empty();
        const StageEffects::Swing times = StageEffects::Swing::Of(ready, wait, shot, readyClip, waitClip);
        w.steps.clear();
        const std::string set = anim != nullptr ? anim->group : std::string();
        if (readyClip) w.steps.push_back({std::uint32_t(anim->ready[turn % anim->ready.size()]), now + times.wait, ++k.takes, set});
        if (waitClip) w.steps.push_back({std::uint32_t(anim->wait[turn % anim->wait.size()]), now + times.shot, ++k.takes, set});
        const std::uint32_t state = anim != nullptr && !anim->shot.empty() ? std::uint32_t(anim->shot[turn % anim->shot.size()]) : kAttack;
        w.steps.push_back({state, now + times.end, ++k.takes, set});
        w.lastSkill = skill->id;
        w.nextSkill += times.end;
        ++k.casts;
        const std::vector<double> events = StageEvents(i, set, state);
        if (const auto* stages = StageEffects::StagesOf(t.skillAnimations, *skill)) {
            StageEffects::Cast cast;
            cast.shotEvents = events;
            k.effects.Skill(*this, *stages, i, target, now, times, true, anim, cast);
        }
        const double blow = events.empty() ? 0.3 * (times.end - times.shot) : events.front();
        if (target != StageEffects::kNone && skill->attack) k.hits.push_back({i, target, skill->id, now + times.shot + blow});
    };

    for (std::size_t i = 0; i < characters_.size() && i < k.walkers.size(); ++i) {
        Character& c = characters_[i];
        Crowd::Walker& w = k.walkers[i];
        if (!c.spawned || !c.dressed) continue;
        if (!w.started && c.nextType == kNoType) {
            // Shown: the glows of what it wears, the loops of its buff.
            w.started = true;
            if (!w.plan.worn.empty()) w.kept = StageEffects::ItemGlows(*this, i, context.files, t.items, t.itemGlows, w.plan.worn);
            if (const assets::SkillRow* buff = w.plan.buff != 0 ? t.skills.Find(w.plan.buff) : nullptr) {
                if (const auto* stages = StageEffects::StagesOf(t.skillAnimations, *buff)) {
                    for (const StageEffects::Handle h : k.effects.BuffLoops(*this, *stages, i)) w.kept.push_back(h);
                }
            }
            w.runTake = c.look.take;
        }
        while (!w.steps.empty() && now >= w.steps.front().until) w.steps.erase(w.steps.begin());
        if (w.steps.empty() && now >= w.nextSkill) cast(i);
        if (!w.steps.empty()) {
            c.look.state = w.steps.front().state;
            c.look.clipSet = w.steps.front().set;
            c.look.take = w.steps.front().take;
            c.still = true;
        } else {
            c.look.state = kRun;
            c.look.clipSet.clear();
            c.look.take = w.runTake;
            c.still = false;
        }
    }

    // Hits landing: the swing's hit (or critical) effect and the target's blood where hits land on
    // its body, and a flinch when it is not swinging itself.
    for (auto it = k.hits.begin(); it != k.hits.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const Crowd::Hit hit = *it;
        it = k.hits.erase(it);
        if (hit.target >= k.walkers.size() || !characters_[hit.target].spawned) continue;
        Crowd::Walker& w = k.walkers[hit.target];
        const assets::CharacterEffectInfo* info = w.plan.codename.empty() ? nullptr : t.skillAnimations.Character(w.plan.codename);
        const StageEffects::Stage::Anchor at = StageEffects::HitAnchor(info);
        if (const assets::SkillRow* s = t.skills.Find(hit.skill)) {
            const bool named = !s->baseName.empty() && s->baseName != "xxx";
            if (const assets::SkillAnimation* anim = t.skillAnimations.Find(named ? s->baseName : s->codename)) {
                const bool critical = k.rng.Chance(0.15f) && !anim->criticalEffect.empty();
                k.effects.Play(*this, hit.target, critical ? anim->criticalEffect : anim->damageEffect, at, gfx::EffectSystem::Kind::Hit);
            }
        }
        if (info != nullptr) k.effects.Play(*this, hit.target, info->blood, at, gfx::EffectSystem::Kind::Hit);
        if (w.steps.empty()) w.steps.push_back({kDamage, now + 0.5, ++k.takes});
        ++k.landed;
    }
}

void WorldScreen::CrowdTools() {
    if (!crowd_ || !crowd_->dressed) return;
    Crowd& k = *crowd_;
    ImGui::SliderInt("players", &k.shownPlayers, 0, static_cast<int>(k.playerSlots.size()));
    ImGui::SliderInt("monsters", &k.shownMonsters, 0, static_cast<int>(k.monsterSlots.size()));
    ImGui::Text("%zu skills cast, %zu hits landed", k.casts, k.landed);
}

} // namespace sr::client
