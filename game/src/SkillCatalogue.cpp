#include "sr/game/SkillCatalogue.h"

#include <algorithm>
#include <cctype>

#include "sr/assets/GameData.h"

namespace sr::game {

namespace {

using assets::FindSkillParam;
using assets::SkillTag;

} // namespace

SkillRoute SkillRouteOf(const assets::SkillRow& s) {
    // Skills::Use's checks in its order (server/game/src/Skills.cpp): what each needs of the data alone; the runtime's
    // refusals (dead, waiting, no mana, the target's kind and reach) come around them there.
    if (s.activity == 0) return SkillRoute::Passive;
    const SkillTraits t = TraitsOf(s);
    if (FindSkillParam(s, SkillTag("dttp"))) return SkillRoute::Detection;
    assets::SkillParam tele = FindSkillParam(s, SkillTag("tele"));
    if (!tele) tele = FindSkillParam(s, SkillTag("tel2"));
    if (tele && tele.Int(1) > 0) return SkillRoute::Teleport;
    if (FindSkillParam(s, SkillTag("mcap"))) return SkillRoute::Mask;
    if (const assets::SkillParam change = FindSkillParam(s, SkillTag("msch")); change && change[0] == 2) return SkillRoute::Duplicate;
    if (FindSkillParam(s, SkillTag("efr"), 3)) return SkillRoute::Object;
    if (!s.targetRequired && (s.hasDuration || t.toggle)) return SkillRoute::SelfBuff;
    // One whose targets are players and never monsters (the Rogue's Tag Point: a party member, an ally or an enemy
    // player) is a player's buff; one on enemy players alone has no target.
    const bool onPlayers = t.buff && !s.targetEnemyMonster && (s.targetAlly || s.targetParty);
    if (t.buff && t.target == SkillTraits::Target::Enemy && !s.targetEnemyMonster && !onPlayers) return SkillRoute::EnemyPlayerBuff;
    if (t.buff && (t.target != SkillTraits::Target::Enemy || onPlayers)) return SkillRoute::PlayerBuff;
    if (t.resurrect) return SkillRoute::Resurrect;
    if ((t.heal || t.cure) && !t.attack) return SkillRoute::Support;
    if (!t.attack && !t.debuff && !t.buff) return t.taunt ? SkillRoute::Taunt : SkillRoute::Unhandled;
    if (!t.attack && !s.targetRequired) return SkillRoute::Around;
    return SkillRoute::Attack;
}

const char* SkillRouteName(SkillRoute route) {
    switch (route) {
    case SkillRoute::Passive: return "passive";
    case SkillRoute::Detection: return "detection";
    case SkillRoute::Teleport: return "teleport";
    case SkillRoute::Mask: return "mask";
    case SkillRoute::Duplicate: return "duplicate";
    case SkillRoute::Object: return "object";
    case SkillRoute::SelfBuff: return "self-buff";
    case SkillRoute::EnemyPlayerBuff: return "enemy-player-buff";
    case SkillRoute::PlayerBuff: return "player-buff";
    case SkillRoute::Resurrect: return "resurrect";
    case SkillRoute::Support: return "support";
    case SkillRoute::Taunt: return "taunt";
    case SkillRoute::Unhandled: return "unhandled";
    case SkillRoute::Around: return "around";
    case SkillRoute::Attack: return "attack";
    }
    return "?";
}

const char* SkillRouteText(SkillRoute route) {
    switch (route) {
    case SkillRoute::Passive: return "Never cast: its numbers count in the stats while it is learned and what it needs is worn.";
    case SkillRoute::Detection: return "A copy of its buff on every player in its area, revealing the hidden to all while it lasts.";
    case SkillRoute::Teleport: return "To the point asked at once, as far as it reaches and the way allows; no preparing or casting.";
    case SkillRoute::Mask: return "On a normal monster's body of no higher level: an item carrying that monster at the caster's feet.";
    case SkillRoute::Duplicate: return "On another player of no higher level: the caster takes its look while the buff lasts.";
    case SkillRoute::Object: return "A skill object stands at the caster's feet from its impact and acts on those in its area.";
    case SkillRoute::SelfBuff: return "A buff on the caster at once (an imbue too: its force joins the caster's hits).";
    case SkillRoute::EnemyPlayerBuff: return "On an enemy player alone (walked into its reach): its copy and link on it; while it lasts that player aims only at the caster.";
    case SkillRoute::PlayerBuff: return "A buff on the caster or on the player selected (walked into its reach first).";
    case SkillRoute::Resurrect: return "Offered to the dead player selected (walked into its reach first); the revival on its yes.";
    case SkillRoute::Support: return "A heal or a cure on its target (itself, the selected player walked into reach first, the party).";
    case SkillRoute::Taunt: return "No damage: a threat raised or cut on the monsters its area takes (around the caster or its target).";
    case SkillRoute::Unhandled: return "Nothing it does is known to our server: refused.";
    case SkillRoute::Around: return "States on those around the caster, at once, no target asked.";
    case SkillRoute::Attack: return "On the selected enemy: walked into its reach, then cast as the next swing, its chain's rows one after another.";
    }
    return "";
}

SkillMotion SkillMotionOf(const assets::SkillRow& s) {
    if (s.activity == 0) return SkillMotion::None;
    if (s.activity == 1) return SkillMotion::Free;
    assets::SkillParam tele = FindSkillParam(s, SkillTag("tele"));
    if (!tele) tele = FindSkillParam(s, SkillTag("tel2"));
    if ((tele && tele.Int(1) > 0) || FindSkillParam(s, SkillTag("tel3"))) return SkillMotion::Moves;
    return SkillMotion::Stands;
}

const char* SkillMotionName(SkillMotion motion) {
    switch (motion) {
    case SkillMotion::None: return "none";
    case SkillMotion::Stands: return "stands";
    case SkillMotion::Free: return "free";
    case SkillMotion::Moves: return "moves";
    }
    return "?";
}

int SkillTimes::HoldMs() const {
    int ms = std::max(0, preparingMs) + std::max(0, castingMs);
    for (const int r : rowMs) ms += std::max(0, r);
    return ms;
}

int SkillTimes::CycleMs(bool handsOn) const {
    int budget = 500, used = 0, rowAt = 0, hold = 0;
    for (std::size_t i = 0; i < rowMs.size(); ++i) {
        const int lead = i < rowLeadMs.size() ? std::max(0, rowLeadMs[i]) : (i == 0 ? std::max(0, preparingMs) + std::max(0, castingMs) : 0);
        const int duration = std::max(0, rowMs[i]);
        if (i + 1 < rowMs.size()) {
            int wait = 0;
            if (duration > budget) {
                wait = duration - budget;
                used += budget;
                budget = 0;
            } else {
                budget -= duration;
            }
            rowAt = std::max(rowAt + lead, rowAt + wait);
        } else {
            hold = rowAt + lead + duration;
        }
    }
    return std::max(hold, rowAt) + (handsOn ? used : 0);
}

int SkillTimes::LeadRows() const {
    int n = 0;
    for (std::size_t i = 0; i < rowMs.size(); ++i) {
        const int lead = i < rowLeadMs.size() ? rowLeadMs[i] : (i == 0 ? preparingMs + castingMs : 0);
        n += lead > 0 ? 1 : 0;
    }
    return n;
}

std::vector<const assets::SkillRow*> ChainOf(const assets::SkillTable& skills, const assets::SkillRow& skill) {
    std::vector<const assets::SkillRow*> out;
    for (const assets::SkillRow* r = skill.chainNext != 0 ? skills.Find(skill.chainNext) : nullptr; r != nullptr && out.size() < 15;
         r = r->chainNext != 0 ? skills.Find(r->chainNext) : nullptr) {
        out.push_back(r);
    }
    return out;
}

SkillTimes TimesOf(const assets::SkillTable& skills, const assets::SkillRow& skill) {
    SkillTimes t;
    t.preparingMs = skill.preparingMs;
    t.castingMs = skill.castingMs;
    t.rowMs.push_back(skill.durationMs);
    t.rowLeadMs.push_back(std::max(0, skill.preparingMs) + std::max(0, skill.castingMs));
    for (const assets::SkillRow* r : ChainOf(skills, skill)) {
        t.rowMs.push_back(r->durationMs);
        t.rowLeadMs.push_back(std::max(0, r->preparingMs) + std::max(0, r->castingMs));
    }
    t.reuseMs = skill.cooldownMs;
    t.cooldownGroup = skill.cooldownGroup;
    return t;
}

int HitsOf(const assets::SkillTable& skills, const assets::SkillRow& skill) {
    const auto hits = [](const assets::SkillRow& r) {
        if (!r.attack && !FindSkillParam(r, SkillTag("pdmg")) && !FindSkillParam(r, SkillTag("lfst"))) return 0; // its own damage, a life steal
        return std::max(1, r.hits);
    };
    int n = hits(skill);
    for (const assets::SkillRow* r : ChainOf(skills, skill)) n += hits(*r);
    return n;
}

std::string SkillGroupCode(const assets::SkillRow& skill) {
    if (!skill.baseName.empty() && skill.baseName != "xxx") return skill.baseName;
    std::string code = skill.codename;
    const std::size_t cut = code.rfind('_');
    if (cut != std::string::npos && cut + 1 < code.size() &&
        std::all_of(code.begin() + std::ptrdiff_t(cut + 1), code.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        code.resize(cut);
    }
    return code;
}

std::vector<SkillCatalogueEntry> BuildSkillCatalogue(const assets::SkillTable& skills) {
    std::vector<SkillCatalogueEntry> out;
    for (const assets::MasteryRow& m : skills.masteries()) {
        for (const assets::SkillRow* first : skills.Board(m.id)) {
            if (first == nullptr || !first->inService) continue;
            SkillCatalogueEntry e;
            e.code = SkillGroupCode(*first);
            e.mastery = &m;
            for (int level = first->level; level < 256; ++level) {
                const assets::SkillRow* r = skills.GroupLevel(first->group, level);
                if (r == nullptr) break;
                if (r->inService) e.ranks.push_back(r);
            }
            if (e.ranks.empty()) e.ranks.push_back(first);
            e.traits = TraitsOf(*first);
            e.route = SkillRouteOf(*first);
            e.motion = SkillMotionOf(*first);
            out.push_back(std::move(e));
        }
    }
    return out;
}

} // namespace sr::game
