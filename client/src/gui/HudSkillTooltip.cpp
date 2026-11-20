// A skill's tooltip, as the 1.188 client builds it for a skill slot: the name and level, active or passive, what it
// costs and needs, its
// description, what it does (and the line builder of its parameter tags, the lines
// themselves are game::SkillText's, shared with the skill lab's list), then the conditions of learning
// it or of its next level.

#include <algorithm>
#include <map>

#include "gui/HudWindows.h"
#include "sr/game/SkillText.h"

namespace sr::client {

namespace {

const ui::Color kBeige = ui::Argb(0xFFEFDAA4), kRed = ui::Argb(0xFFFF4A4A), kWhite = ui::Argb(0xFFFFFFFF);
constexpr int kShadow = ui::kTextShadow, kBoldShadow = ui::kTextShadow | ui::kTextBold;

std::string N(long long v) { return std::to_string(v); }

class Builder {
public:
    Builder(const assets::SkillRow& row, const HudContext& c) : row_(row), c_(c) {}
    std::vector<ui::TooltipLine> Build();
    // What the row does, its lines alone (a socket stone's tooltip shows each level's).
    std::vector<std::string> EffectTexts() const { return game::SkillRowEffectLines(row_, Lookup()); }

private:
    std::string T(const char* key) const { return c_.Text(key); }
    game::SkillTextLookup Lookup() const {
        return [c = &c_](const std::string& key) { return c->Text(key); };
    }
    std::string Name(const assets::SkillRow& s) const {
        std::string n = s.nameKey.empty() ? std::string() : c_.Text(s.nameKey);
        return n.empty() || n == s.nameKey ? s.codename : n;
    }
    // A condition of learning `s` (its text, and whether the player meets it: -1 not a test).
    struct Condition {
        std::string text;
        int met = -1;
    };
    std::vector<Condition> Conditions(const assets::SkillRow& s) const;
    bool Wears(int type, int sub) const;
    int MasteryLevel(std::uint32_t mastery) const;
    int KnownLevel(std::uint32_t group) const;

    const assets::SkillRow& row_;
    const HudContext& c_;
};

int Builder::MasteryLevel(std::uint32_t mastery) const {
    if (c_.state == nullptr) return 0;
    for (const auto& [id, level] : c_.state->masteries) {
        if (id == mastery) return level;
    }
    return 0;
}

int Builder::KnownLevel(std::uint32_t group) const {
    if (c_.state == nullptr || c_.skills == nullptr) return 0;
    int best = 0;
    for (const std::uint32_t id : c_.state->skills) {
        if (const assets::SkillRow* s = c_.skills->Find(id); s != nullptr && s->group == group) best = std::max(best, s->level);
    }
    return best;
}

bool Builder::Wears(int type, int sub) const {
    // An item worn of that class (TID3) and kind (TID4; 0 any).
    if (c_.state == nullptr || c_.items == nullptr) return false;
    for (int place = 0; place < game::slot::kWorn; ++place) {
        const game::ItemInstance* item = c_.state->inventory.At(std::uint8_t(place));
        const assets::ItemRow* r = item != nullptr ? c_.items->Find(item->refId) : nullptr;
        if (r != nullptr && r->typeId[2] == type && (sub == 0 || r->typeId[3] == sub)) return true;
    }
    return false;
}

std::vector<Builder::Condition> Builder::Conditions(const assets::SkillRow& s) const {
    // the mastery's level, STR, INT, the other groups needed, the skill points.
    std::vector<Condition> out;
    const game::CharacterStatus* self = c_.state != nullptr ? &c_.state->self : nullptr;
    if (s.masteries[0] != 0 && c_.skills != nullptr) {
        if (const assets::MasteryRow* m = c_.skills->Mastery(s.masteries[0])) {
            out.push_back({T("PARAM_MASTERY_LEVEL") + " : " + c_.Text(m->nameKey) + " " + T("UIIT_STT_LEVEL_LV") + " " + N(s.masteryLevels[0]),
                           MasteryLevel(s.masteries[0]) >= s.masteryLevels[0] ? 1 : 0});
        }
    }
    if (s.requiredStr != 0) out.push_back({T("PARAM_STR") + " : " + N(s.requiredStr), self != nullptr && self->strength >= s.requiredStr ? 1 : 0});
    if (s.requiredInt != 0) out.push_back({T("PARAM_INT") + " : " + N(s.requiredInt), self != nullptr && self->intelligence >= s.requiredInt ? 1 : 0});
    int n = 1;
    for (std::size_t i = 0; i < s.prerequisiteGroups.size(); ++i) {
        const std::uint32_t group = s.prerequisiteGroups[i];
        if (group == 0 || group == s.group || c_.skills == nullptr) continue;
        const assets::SkillRow* need = c_.skills->GroupLevel(group, s.prerequisiteLevels[i]);
        const std::string name = need != nullptr ? Name(*need) : std::string();
        out.push_back({T("PARAM_REQ_PREV_SKILL") + N(n++) + " : " + name + " " + T("UIIT_STT_LEVEL_LV") + " " + N(s.prerequisiteLevels[i]),
                       KnownLevel(group) >= s.prerequisiteLevels[i] ? 1 : 0});
    }
    if (s.spCost != 0) out.push_back({T("PARAM_REQ_SP") + " : " + N(s.spCost), self != nullptr && self->skillPoints >= std::uint32_t(s.spCost) ? 1 : 0});
    return out;
}

std::vector<ui::TooltipLine> Builder::Build() {
    std::vector<ui::TooltipLine> out;
    // The name and level, then a blank line ("%s %s %d\n ").
    out.push_back({Name(row_) + " " + T("UIIT_STT_LEVEL_LV") + " " + N(row_.level), kWhite, kBoldShadow});
    out.push_back({{}, kWhite, kShadow});
    out.push_back({T(row_.activity != 0 ? "PARAM_ACTIVE_SKILL" : "PARAM_PASSIVE_SKILL"), kBeige, kShadow});
    const game::SkillTextLookup lookup = Lookup();
    for (const std::string& l : game::SkillCostLines(row_, lookup)) out.push_back({l, kBeige, kShadow});
    if (const game::SkillItemNeeds needs = game::SkillItemLine(row_, lookup); !needs.line.empty()) {
        // Beige when worn: any one of them, or with "Essential Item" every one.
        bool worn = needs.essential;
        for (const std::uint16_t k : needs.kinds) {
            const bool w = Wears(k >> 8, k & 0xFF);
            if (needs.essential && !w) worn = false;
            if (!needs.essential && w) worn = true;
        }
        out.push_back({needs.line, worn ? kBeige : kRed, kShadow});
    }
    if (!row_.descKey.empty() && c_.hasText && c_.hasText(row_.descKey) && !c_.Text(row_.descKey).empty()) {
        AddTooltipDescription(out, c_.Text(row_.descKey));
    }
    out.push_back({{}, kWhite, kShadow});
    for (const std::string& l : game::SkillEffectLines(row_, c_.skills, lookup)) out.push_back({l, kBeige, kShadow});
    // What learning it asks (a group's first level, not known yet, its mastery listed), or what its
    // next level does (a known level below the group's last); nothing at the last level.
    if (c_.skills == nullptr || c_.state == nullptr) return out;
    const auto conditions = [&](const char* header, const assets::SkillRow& s) {
        out.push_back({T(header), kWhite, kBoldShadow});
        for (const Condition& cond : Conditions(s)) out.push_back({cond.text, cond.met < 0 ? kWhite : cond.met != 0 ? kBeige : kRed, kShadow});
    };
    const bool known = KnownLevel(row_.group) >= row_.level;
    if (row_.level == 1 && !known) {
        bool listed = false;
        for (const auto& [id, level] : c_.state->masteries) listed = listed || id == row_.masteries[0];
        if (listed) conditions("PARAM_CONDITION_OF_LEARN", row_);
        return out;
    }
    if (const assets::SkillRow* next = c_.skills->GroupLevel(row_.group, row_.level + 1)) {
        out.push_back({{}, kWhite, kShadow});
        conditions("PARAM_CONDITION_OF_NEXT_LEVEL", *next);
    }
    return out;
}

} // namespace

std::vector<std::string> SkillEffectTexts(const assets::SkillRow& row, const HudContext& context) { return Builder(row, context).EffectTexts(); }

std::vector<ui::TooltipLine> SkillTooltip(const assets::SkillRow& row, const HudContext& context) {
    // Effects and conditions keep their trailing blanks out: a line of only a space is a blank.
    std::vector<ui::TooltipLine> lines = Builder(row, context).Build();
    for (ui::TooltipLine& l : lines) {
        if (l.text == " ") l.text.clear();
    }
    while (!lines.empty() && lines.back().text.empty()) lines.pop_back();
    return lines;
}

} // namespace sr::client
