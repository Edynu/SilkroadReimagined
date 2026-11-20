#include "sr/game/SkillText.h"

#include <cstdio>
#include <span>
#include <string_view>
#include <utility>

#include "sr/assets/GameData.h"

namespace sr::game {

namespace {

using assets::SkillTag;

std::string N(long long v) { return std::to_string(v); }
std::string F1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
// Tenths of a metre / of a second as the original prints them ("%.1fm", "%.1f").
std::string Metres(std::uint32_t tenths) { return F1(double(tenths) / 10.0) + "m"; }
std::string Seconds1(std::uint32_t ms) { return F1(double(ms) / 1000.0); }

// The lines of the tags of a row (`owner`: the skill the tooltip is of, whose name a Fire Trap's area checks).
class Lines {
public:
    Lines(const assets::SkillRow& owner, const SkillTextLookup& text) : owner_(owner), text_(text) {}
    std::vector<std::string> out;

    void Effects(const assets::SkillRow& s);
    void ChainEffects(const assets::SkillTable& skills);

private:
    std::string T(const char* key) const { return text_ ? text_(key) : std::string(key); }
    void Add(std::string s) { out.push_back(std::move(s)); }
    // "{what} {n} {how}" and "{what} {n}% {how}": most of the effect lines.
    std::string Flat(const char* what, long long n, const char* how) const { return T(what) + " " + N(n) + " " + T(how); }
    std::string Pct(const char* what, long long n, const char* how) const { return T(what) + " " + N(n) + "% " + T(how); }
    void Area(const assets::SkillParam& p);

    const assets::SkillRow& owner_;
    const SkillTextLookup& text_;
};

void Lines::Area(const assets::SkillParam& p) {
    // 'efr': where it strikes (its second argument) and how far (tenths of a metre), how many.
    // A Fire Trap's kind-3 area is not described (its name is checked).
    if (p[0] == 3 && (owner_.nameKey == "SN_SKILL_EU_WIZARD_FIREA_TRAP_A" || owner_.nameKey == "SN_SKILL_EU_WIZARD_FIREA_TRAP_B")) return;
    const int shape = p.Int(1);
    if (shape == 1 || shape == 2) {
        std::string line = T(shape == 1 ? "UIIT_STT_NEAR_RANGE" : "UIIT_STT_FRONT_RANGE") + " " + T("UIIT_STT_RADIUS") + " " + Metres(p[2]);
        if (p[3] != 0) line += " (" + T("UIIT_STT_SAME_TIME_ATTACK") + " " + N(p[3]) + ")";
        Add(line);
    } else if (shape == 3 || shape == 4) {
        std::string line = T("UIIT_STT_PIERCING_RANGE") + " (" + T("UIIT_STT_PIERCING_NUM") + " " + N(p[3]) + ")";
        if (p[4] != 0) line += ", " + T("UIIT_STT_DAMAGE") + " " + T("PARAM_DECREASE") + " -" + N(p[4]) + "%";
        Add(line);
    } else if (shape == 5) {
        Add(" ");
    } else if (shape == 6) {
        std::string line = T("UIIT_STT_TRANSITION_RANGE") + " " + T("UIIT_STT_DISTANCE") + " " + Metres(p[2]);
        if (p[3] != 0) line += " (" + T("UIIT_STT_TRANSITION_COUNT") + " " + N(p[3]) + ")";
        Add(line);
    }
}

void Lines::Effects(const assets::SkillRow& s) {
    // In the order of the original's line builder, one block per tag.
    const auto P = [&](const char* tag) { return assets::FindSkillParam(s, SkillTag(tag)); };
    if (const auto p = P("dura")) {
        const std::uint32_t ms = p[0];
        if (ms < 10000) {
            Add(T("PARAM_DURA") + " " + Seconds1(ms) + " " + T("PARAM_SECOND"));
        } else if (ms <= 86400000u) {
            Add(T("PARAM_DURA") + " " + N(ms / 1000) + " " + T("PARAM_SECOND"));
        } else {
            Add(T("PARAM_DURA") + " " + N(ms / 86400000u) + " " + T("PARAM_DAY"));
        }
    }
    if (const auto p = P("lnks")) {
        if (p[1] != 0) Add(T("UIIT_STT_EU_LINK_INTERVAL") + " " + N(p[1] / 10) + T("UIIT_STT_DISTANCE_UNIT"));
        if (p[2] != 0) Add(T("UIIT_STT_EU_LINK_AMOUNT") + " " + N(p[2]));
    }
    if (const auto p = P("att"); p && p[2] != 0 && p[3] != 0 && (p[0] & 0xC) != 0) {
        // The kind's bits: 4 physical, 8 magical; min ~ max (power %).
        Add(T((p[0] & 4) != 0 ? "PARAM_PA" : "PARAM_MA") + " " + N(p[2]) + "~" + N(p[3]) + " (" + N(p[1]) + "%)");
    }
    if (const auto p = P("ko")) Add(T("PARAM_KO") + " " + N(p[1]) + "%(" + T("UIIT_STT_LEVEL_LV") + " " + N(p[0]) + ")");
    if (const auto p = P("da"); p && p[0] != 100) {
        std::string line = Pct("PARAM_DA", p.Int(0) - 100, "PARAM_INCREASE");
        if (const auto r = P("reqc"); r && r[0] != 0) line += " " + T("PARAM_SKILL_ABILITY_NST");
        Add(line);
    }
    if (const auto p = P("cr")) {
        if (p[0] != 0) Add(Flat("PARAM_CRITICAL", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Pct("PARAM_CRITICAL", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("ck")) Add(T("PARAM_CK") + " " + N(p[0]) + "%");
    if (const auto p = P("ru")) Add(T("PARAM_RU") + " " + Metres(p[0]) + " " + T("PARAM_INCREASE"));
    if (const auto p = P("kb")) Add(T("PARAM_KB") + " " + N(p[0]) + "%");
    if (const auto p = P("defp")) {
        if (p[0] != 0) Add(Flat("PARAM_PD", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Flat("PARAM_MD", p.Int(1), "PARAM_INCREASE"));
        if (p[2] != 0) Add(FormatText(T("UIIT_STT_EU_PSKILL_LIMIT_APPLY"), {N(p[2])}));
    }
    if (const auto p = P("defr")) {
        if (p[0] != 0) Add(Pct("PARAM_PD", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Pct("PARAM_MD", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("ar")) {
        if (p[0] != 0) Add(Flat("PARAM_PR", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Flat("PARAM_MR", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("dar")) {
        if ((p[0] & 4) != 0) Add(T("UIIT_STT_PHY_DAMAGE") + " " + T("PARAM_DECREASE_RATIO") + " " + N(p[1]));
        if ((p[0] & 8) != 0) Add(T("UIIT_STT_MAG_DAMAGE") + " " + T("PARAM_DECREASE_RATIO") + " " + N(p[1]));
    }
    if (const auto p = P("odar")) {
        if ((p[0] & 4) != 0) Add(Pct("UIIT_STT_PHY_DAMAGE", p.Int(1), "PARAM_ABSORB"));
        if ((p[0] & 8) != 0) Add(Pct("UIIT_STT_MAG_DAMAGE", p.Int(1), "PARAM_ABSORB"));
    }
    if (const auto p = P("dru")) {
        if (p[0] != 0) Add(Pct("UIIT_STT_PHY_DAMAGE", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Pct("UIIT_STT_MAG_DAMAGE", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("br"); p && p[1] != 0) Add(Pct("PARAM_BLOCKING", p.Int(1), "PARAM_INCREASE"));
    if (const auto p = P("hr")) {
        if (p[0] != 0) Add(Flat("PARAM_HR", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Pct("PARAM_HR", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("hwir"); p && p[0] != 0) Add(Pct("UIIT_CTL_TC_ACQUISITION_WHAN", p.Int(0), "PARAM_INCREASE"));
    if (const auto p = P("er")) {
        if (p[0] != 0) Add(Flat("PARAM_ER", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Pct("PARAM_ER", p.Int(1), "PARAM_INCREASE"));
    }
    // A socket stone's damage return on a block ('bldr' range, physical %, magical %; ours: the original's line for it
    // is not traced).
    if (const auto p = P("bldr")) {
        if (p[1] != 0) Add(T("UIIT_STT_REFLECT_EFFECT") + " " + T("UIIT_STT_PHY_DAMAGE") + " " + N(p[1]) + "%");
        if (p[2] != 0) Add(T("UIIT_STT_REFLECT_EFFECT") + " " + T("UIIT_STT_MAG_DAMAGE") + " " + N(p[2]) + "%");
    }
    if (const auto p = P("onff"); p && p[1] != 0) Add(FormatText(T("PARAM_ONFF_CONSUME_MP"), {N(p[0] / 1000), N(p[1])}));
    if (const auto p = P("mc"); p && p[0] == 2 && p[1] > 1) Add(T("PARAM_MC_COUNT") + " " + N(p[1]) + " " + T("UIIT_STT_COUNT"));
    for (const std::uint32_t kind : {1u, 2u, 3u}) {
        if (const auto p = assets::FindSkillParam(s, SkillTag("efr"), kind)) Area(p);
    }
    const auto maxLine = [&](const char* tag, const char* what, const char* how, std::size_t flat, std::size_t pct) {
        if (const auto p = P(tag)) {
            if (p[flat] != 0) Add(T("PARAM_MAX") + " " + T(what) + " " + N(p.Int(flat)) + " " + T(how));
            if (p[pct] != 0) Add(T("PARAM_MAX") + " " + T(what) + " " + N(p.Int(pct)) + "% " + T(how));
        }
    };
    maxLine("hpi", "PARAM_HP", "PARAM_INCREASE", 0, 1);
    maxLine("mpi", "PARAM_MP", "PARAM_INCREASE", 0, 1);
    if (const auto p = P("rhru"); p && p[0] != 0) {
        Add(T("PARAM_HP") + " " + T("UIIT_STT_EU_HEAL") + " " + T("UIIT_STT_EU_RECEIVE_AMOUNT") + " " + N(p[0]) + "% " + T("PARAM_INCREASE"));
    }
    if (const auto p = P("dcmp"); p && p[0] != 0) Add(Pct("PARAM_PSKILL_CONSUME_MP", p.Int(0), "PARAM_DECREASE"));
    if (const auto p = P("pw")) {
        if (p[1] != 0) Add(T("PARAM_HP") + " " + N(p[1]));
        if (p[2] != 0 && (p[0] & 4) != 0) Add(T("PARAM_PD") + " " + N(p[2]));
        if (p[2] != 0 && (p[0] & 8) != 0) Add(T("PARAM_MD") + " " + N(p[2]));
        if (p[3] != 0 && (p[0] & 4) != 0) Add(T("PARAM_PR") + " " + N(p[3]));
        if (p[3] != 0 && (p[0] & 8) != 0) Add(T("PARAM_MR") + " " + N(p[3]));
    }
    for (const char* tag : {"hste", "hst2", "hst3"}) {
        if (const auto p = P(tag); p && p[0] != 0) Add(Pct("PARAM_HSTE", p.Int(0), "PARAM_INCREASE"));
    }
    if (const auto p = P("irgc")) {
        if (p[0] != 0) Add(T("PARAM_HP") + " " + T("PARAM_IRGC") + " " + N(p[0]) + "% " + T("PARAM_INCREASE"));
        if (p[1] != 0) Add(T("PARAM_MP") + " " + T("PARAM_IRGC") + " " + N(p[1]) + "% " + T("PARAM_INCREASE"));
    }
    if (const auto p = P("apru")) {
        if (p[0] != 0) Add(Pct("PARAM_PA", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Pct("PARAM_MA", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("apau")) {
        if (p[0] != 0) Add(Flat("PARAM_PA", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Flat("PARAM_MA", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("spda")) {
        if (p[0] != 0) Add(Pct("PARAM_SPD", p.Int(0), "PARAM_DECREASE"));
        if (p[1] != 0) Add(Flat("PARAM_PA", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("terd"); p && p[0] != 0) Add(Flat("PARAM_ER", p.Int(0), "PARAM_DECREASE"));
    if (const auto p = P("thrd"); p && p[0] != 0) Add(Flat("PARAM_HR", p.Int(0), "PARAM_DECREASE"));
    if (const auto p = P("tele"); p && p[0] != 0) Add(T("PARAM_TELE") + " " + Metres(p[1]));
    for (const char* tag : {"tel2", "tel3"}) {
        if (const auto p = P(tag); p && p[1] != 0) Add(T("PARAM_TELE") + " " + Metres(p[1]));
    }
    if (const auto p = P("pola")) Add(T("PARAM_POLA") + "(" + T("UIIT_STT_LEVEL_LV") + " " + N(p[1]) + ")");
    if (const auto p = P("summ")) {
        Add(T("PARAM_DURA") + " " + N(p[0] / 1000) + " " + T("PARAM_SECOND"));
        if (p[3] != 0) Add(T("PARAM_PA") + " " + N(p[3]));
        if (p[4] != 0) Add(T("PARAM_MA") + " " + N(p[4]));
    }
    // The states a hit may give: power and probability; burn with its level.
    const auto chance = [&](const char* tag, const char* what) {
        if (const auto p = P(tag)) Add(T(what) + " " + T("UIIT_STT_PROBABILITY") + " " + N(p[1]) + "% (" + T("PARAM_POWER") + " " + N(p[0]) + ")");
    };
    chance("fz", "PARAM_FZ");
    chance("fb", "PARAM_FB");
    chance("es", "PARAM_ES");
    if (const auto p = P("bu")) {
        Add(T("PARAM_BU") + " " + T("UIIT_STT_PROBABILITY") + " " + N(p[1]) + "% (" + T("PARAM_POWER") + " " + N(p[0]) + ", " + T("UIIT_STT_LEVEL_LV") + " " +
            N(p[2]) + ")");
    }
    chance("ps", "PARAM_PS");
    // ... and grade, probability and time.
    const auto timed = [&](const char* tag, const char* what) {
        if (const auto p = P(tag)) {
            std::string line = T(what) + " " + N(p[2]) + " " + T("UIIT_STT_GRADE") + " (" + T("UIIT_STT_PROBABILITY") + " " + N(p[1]) + "%";
            // A bleed of a skill that strikes several times says how many (its 'mc').
            if (std::string_view(tag) == "bl") {
                if (const auto m = P("mc"); m && m[0] == 2 && m[1] >= 2) line += " " + N(m[1]) + " " + T("UIIT_STT_COUNT");
            }
            Add(line + ", " + Seconds1(p[0]) + " " + T("PARAM_SECOND") + ")");
        }
    };
    for (const auto& [tag, what] : std::initializer_list<std::pair<const char*, const char*>>{
             {"se", "PARAM_SLEEP"}, {"rt", "PARAM_ROOT"}, {"sl", "PARAM_SLOW"}, {"fe", "PARAM_FEAR"}, {"my", "PARAM_MYOPIA"},
             {"bl", "PARAM_BLOOD"}, {"dn", "PARAM_DN"}, {"st", "PARAM_STUN"}, {"ds", "PARAM_DISEASE"}, {"ca", "PARAM_CHAOS"},
             {"cssr", "PARAM_CURSE_STR"}, {"csit", "PARAM_CURSE_INT"}, {"cspd", "PARAM_CURSE_PD"}, {"csmd", "PARAM_CURSE_MD"},
             {"cshp", "PARAM_CURSE_HP"}, {"csmp", "PARAM_CURSIE_MP"}}) {
        timed(tag, what);
    }
    if (const auto p = P("tb")) {
        Add(T("PARAM_TIME_BOMB") + " " + N(p[1]) + " " + T("UIIT_STT_GRADE") + " (" + T("UIIT_STT_PROBABILITY") + " " + N(p[0]) + "%)");
    }
    if (const auto p = P("lkdr")) {
        if ((p[0] & 4) != 0) Add(Pct("UIIT_STT_PHY_DAMAGE", p.Int(1), "PARAM_ABSORB"));
        if ((p[0] & 8) != 0) Add(Pct("UIIT_STT_MAG_DAMAGE", p.Int(1), "PARAM_ABSORB"));
    }
    if (const auto p = P("lkag")) Add(Pct("PARAM_AGGRO", p.Int(0), "PARAM_ABSORB"));
    // Reflected damage, by the weapon's attack it is reckoned from.
    for (const auto& [tag, what] : std::initializer_list<std::pair<const char*, const char*>>{
             {"pwtt", "PARAM_WEAPON_PA_RATIO"}, {"mwtt", "PARAM_WEAPON_MA_RATIO"}, {"pwdt", "PARAM_WEAPON_PA_RATIO"}, {"mwdt", "PARAM_WEAPON_MA_RATIO"},
             {"mwhh", "PARAM_WEAPON_MA_RATIO"}, {"mwmh", "PARAM_WEAPON_MA_RATIO"}, {"mwhs", "PARAM_WEAPON_MA_RATIO"}}) {
        if (const auto p = P(tag)) Add(Pct(what, p.Int(0), "PARAM_REFLECT"));
    }
    if (const auto p = P("pmhp")) {
        if (p[1] != 0) Add(T("PARAM_MAX") + " " + T("PARAM_HP") + " " + N(p[1]) + " " + T("PARAM_DECREASE"));
        if (p[2] != 0) Add(T("PARAM_MAX") + " " + T("PARAM_HP") + " " + N(p[2]) + "% " + T("PARAM_DECREASE"));
    }
    if (const auto p = P("pmmp")) {
        if (p[1] != 0) Add(T("PARAM_MAX") + " " + T("PARAM_MP") + " " + N(p[1]) + " " + T("PARAM_DECREASE"));
        if (p[2] != 0) Add(T("PARAM_MAX") + " " + T("PARAM_MP") + " " + N(p[2]) + "% " + T("PARAM_DECREASE"));
    }
    if (const auto p = P("pmdp")) {
        if (p[1] != 0) Add(T("PARAM_MAX") + " " + T("PARAM_PD") + " " + N(p[1]) + "% " + T("PARAM_DECREASE"));
        if (p[2] != 0) Add(T("PARAM_MAX") + " " + T("PARAM_MD") + " " + N(p[2]) + "% " + T("PARAM_DECREASE"));
    }
    if (const auto p = P("dgmp")) Add(T("UIIT_STT_DAMAGE") + " " + N(p[0]) + "% " + T("PARAM_MP") + " " + T("PARAM_ABSORB"));
    for (const auto& [tag, what] : std::initializer_list<std::pair<const char*, const char*>>{{"chcr", "PARAM_HP"}, {"cmcr", "PARAM_MP"}}) {
        if (const auto p = P(tag)) Add(T(what) + " " + T("UIIT_STT_EU_END_BUFF") + " " + FormatText(T("PARAM_TO_DECREASE_RATIO"), {N(p[0])}));
    }
    if (const auto p = P("heal")) {
        if (p[0] != 0) Add(Flat("PARAM_HP", p.Int(0), "PARAM_RECOVERY"));
        if (p[1] != 0) Add(Pct("PARAM_HP", p.Int(1), "PARAM_RECOVERY"));
        if (p[2] != 0) Add(Flat("PARAM_MP", p.Int(2), "PARAM_RECOVERY"));
        if (p[3] != 0) Add(Pct("PARAM_MP", p.Int(3), "PARAM_RECOVERY"));
    }
    if (const auto p = P("resu")) Add(T("PARAM_RESU") + "(" + T("UIIT_STT_LEVEL_LV") + " " + N(p[0]) + ")");
    if (const auto p = P("cura")) Add(T("UIIT_STT_CURE_ALL") + " " + N(p[0]));
    if (const auto p = P("alcu")) Add(Pct("PARAM_ALCU", p.Int(0), "PARAM_INCREASE"));
    if (const auto p = P("luck")) Add(Pct("PARAM_LUCK", p.Int(0), "PARAM_INCREASE"));
    if (const auto p = P("pdmg")) Add(T("UIIT_STT_EU_ABSOLUTE") + " " + T("UIIT_STT_DAMAGE") + " " + N(p[0]));
    if (const auto p = P("dmgt")) {
        if (p[0] != 0) Add(T("UIIT_STT_EU_ABSORB_RATIO") + " " + N(p[0]) + "%");
        if (p[1] != 0) Add(T("UIIT_STT_EU_ABSORB_RATIO") + " " + N(p[1]) + "%");
    }
    if (const auto p = P("tnt2")) {
        if (p[0] != 0) Add(T("UIIT_STT_EU_TAUNT") + " " + N(p[0]));
        if (p[1] != 0) Add(Pct("PARAM_AGGRO", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = P("dtnt")) {
        if (p[0] != 0) Add(Flat("PARAM_AGGRO", p.Int(0), "PARAM_DECREASE"));
        if (p[1] != 0) Add(Pct("PARAM_AGGRO", p.Int(1), "PARAM_DECREASE"));
    }
    for (const auto& [tag, what] : std::initializer_list<std::pair<const char*, const char*>>{{"stri", "PARAM_STR"}, {"inti", "PARAM_INT"}}) {
        if (const auto p = P(tag)) {
            if (p[0] != 0) Add(Flat(what, p.Int(0), "PARAM_INCREASE"));
            if (p[1] != 0) Add(FormatText(T("UIIT_STT_EU_PSKILL_LIMIT_APPLY"), {N(p[1])}));
        }
    }
    if (const auto p = P("lfst")) Add(T("PARAM_HP") + " " + T("PARAM_ABSORB") + " " + N(p[0]));
    if (const auto p = P("tran")) {
        if (p[0] != 0) Add(T("PARAM_HP") + " " + T("PARAM_TRANS") + " " + N(p[0]));
        if (p[1] != 0) Add(T("PARAM_MP") + " " + T("PARAM_TRANS") + " " + N(p[1]));
    }
    for (const auto& [tag, what] : std::initializer_list<std::pair<const char*, const char*>>{
             {"spi", "PARAM_SKILL_POINT_AMOUNT"}, {"hwit", "PARAM_HWAN_TIME_INCREASE"}, {"msr", "PARAM_MP_ABSORB_RATE"}, {"gdr", "PARAM_GOLD_DROP_RATE"}}) {
        if (const auto p = P(tag)) Add(FormatText(T(what), {N(p[0])}));
    }
    if (const auto p = P("abnb")) Add(T("UIIT_STT_EU_ABNORMAL_VALID_RANGE") + " " + N(p[0] / 10) + T("UIIT_STT_DISTANCE_UNIT"));
    if (const auto p = P("dcri")) Add(T("PARAM_CRITICAL") + " " + T("PARAM_ER") + " " + N(p[0]) + " " + T("PARAM_INCREASE"));
    if (const auto p = P("stns")) {
        Add(FormatText(T("UIIT_TOOLTIP_CURSING_STONE_01"), {N(p[0] / 1000)}));
        Add(FormatText(T("UIIT_TOOLTIP_CURSING_STONE_02"), {N(p[2] / 1000), N(p[1])}));
    }
    if (const auto p = P("expu")) Add(T("PARAM_EXPU") + " " + N(p[0]) + " %");
}

void Lines::ChainEffects(const assets::SkillTable& skills) {
    // A skill of several hits (col 9 names the next): the attack averaged over its hits, the states
    // they may give (each with how many hits give it), the hits, the area, the down attack, the
    // critical, the taunt of the last.
    std::vector<const assets::SkillRow*> hits;
    for (const assets::SkillRow* s = &owner_; s != nullptr && hits.size() < 32; s = s->chainNext != 0 ? skills.Find(s->chainNext) : nullptr) hits.push_back(s);
    long long pct = 0, lo = 0, hi = 0;
    std::uint32_t kind = 0;
    int counted = 0;
    for (const assets::SkillRow* s : hits) {
        if (const auto a = assets::FindSkillParam(*s, SkillTag("att"))) {
            kind = a[0];
            pct += a[1];
            lo += a[2];
            hi += a[3];
            ++counted;
        }
    }
    if (counted > 0 && (kind & 0xC) != 0) {
        Add(T((kind & 4) != 0 ? "PARAM_PA" : "PARAM_MA") + " " + N(lo / counted) + "~" + N(hi / counted) + " (" + N(pct / counted) + "%)");
    }
    // The states, in the states builder's order, with the hits that may give them (inferred: the
    // first hit's numbers; the builder reads its own summary of the hits).
    const auto first = [&](const char* tag, int& count) {
        assets::SkillParam found;
        count = 0;
        for (const assets::SkillRow* s : hits) {
            if (const auto p = assets::FindSkillParam(*s, SkillTag(tag))) {
                if (count++ == 0) found = p;
            }
        }
        return found;
    };
    for (const auto& [tag, what] : std::initializer_list<std::pair<const char*, const char*>>{{"fz", "PARAM_FZ"}, {"fb", "PARAM_FB"}, {"es", "PARAM_ES"}, {"bu", "PARAM_BU"}, {"ps", "PARAM_PS"}}) {
        int count = 0;
        if (const auto p = first(tag, count)) {
            std::string line = T(what) + " " + T("UIIT_STT_PROBABILITY") + " " + N(p[1]) + "% (" + N(count) + " " + T("UIIT_STT_COUNT") + " " + T("PARAM_POWER") + " " + N(p[0]);
            if (std::string_view(tag) == "bu") line += ", " + T("UIIT_STT_LEVEL_LV") + " " + N(p[2]);
            Add(line + ")");
        }
    }
    for (const auto& [tag, what] : std::initializer_list<std::pair<const char*, const char*>>{
             {"se", "PARAM_SLEEP"}, {"rt", "PARAM_ROOT"}, {"sl", "PARAM_SLOW"}, {"fe", "PARAM_FEAR"}, {"my", "PARAM_MYOPIA"},
             {"bl", "PARAM_BLOOD"}, {"dn", "PARAM_DN"}, {"st", "PARAM_STUN"}, {"ds", "PARAM_DISEASE"}, {"ca", "PARAM_CHAOS"},
             {"cssr", "PARAM_CURSE_STR"}, {"csit", "PARAM_CURSE_INT"}, {"cspd", "PARAM_CURSE_PD"}, {"csmd", "PARAM_CURSE_MD"},
             {"cshp", "PARAM_CURSE_HP"}, {"csmp", "PARAM_CURSIE_MP"}}) {
        int count = 0;
        if (const auto p = first(tag, count)) {
            Add(T(what) + " " + N(p[2]) + " " + T("UIIT_STT_GRADE") + " (" + T("UIIT_STT_PROBABILITY") + " " + N(p[1]) + "% " + N(count) + " " +
                T("UIIT_STT_COUNT") + ", " + Seconds1(p[0]) + " " + T("PARAM_SECOND") + ")");
        }
    }
    // The hits: one per hit of the chain, or its 'mc' count; "Maximum" when a hit knocks down or back.
    int shots = 0;
    bool knocks = false;
    const bool multi = bool(assets::FindSkillParam(owner_, SkillTag("mc")));
    for (const assets::SkillRow* s : hits) {
        if (!multi) {
            ++shots;
        } else if (const auto m = assets::FindSkillParam(*s, SkillTag("mc"))) {
            if (assets::FindSkillParam(*s, SkillTag("kb")) || assets::FindSkillParam(*s, SkillTag("ko"))) knocks = true;
            shots += m.Int(1);
        }
    }
    if (shots != 0) {
        Add(knocks ? T("PARAM_MAX") + " " + T("PARAM_MC_COUNT") + " " + N(shots) + " " + T("UIIT_STT_COUNT")
                   : T("PARAM_MC_COUNT") + " " + N(shots) + " " + T("UIIT_STT_COUNT"));
    }
    const assets::SkillRow& last = *hits.back();
    if (const auto p = assets::FindSkillParam(last, SkillTag("efr"), 1)) Area(p);
    if (const auto p = assets::FindSkillParam(last, SkillTag("da")); p && p[0] != 100) {
        std::string line = Pct("PARAM_DA", p.Int(0) - 100, "PARAM_INCREASE");
        if (const auto r = assets::FindSkillParam(owner_, SkillTag("reqc")); r && r[0] != 0) line += " " + T("PARAM_SKILL_ABILITY_NST");
        Add(line);
    }
    if (const auto p = assets::FindSkillParam(last, SkillTag("cr"))) {
        if (p[0] != 0) Add(Flat("PARAM_CRITICAL", p.Int(0), "PARAM_INCREASE"));
        if (p[1] != 0) Add(Pct("PARAM_CRITICAL", p.Int(1), "PARAM_INCREASE"));
    }
    if (const auto p = assets::FindSkillParam(owner_, SkillTag("tnt2"))) {
        if (p[0] != 0) Add(T("UIIT_STT_EU_TAUNT") + " " + N(p[0]));
        if (p[1] != 0) Add(Pct("PARAM_AGGRO", p.Int(1), "PARAM_INCREASE"));
    }
}

} // namespace

std::string FormatText(const std::string& format, std::initializer_list<std::string> args) {
    std::string out;
    auto next = args.begin();
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size()) {
            out += format[i];
            continue;
        }
        if (format[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        // Flags, width, precision and length, then the conversion.
        std::size_t j = i + 1;
        while (j < format.size() && std::string_view("-+ #0123456789.lhI").find(format[j]) != std::string_view::npos) ++j;
        if (j >= format.size()) break;
        if (next != args.end()) out += *next++;
        i = j;
    }
    return out;
}

const char* WeaponTextKey(int kind) {
    switch (kind) {
    case 2: return "PARAM_WEAPON_SWORD";
    case 3: return "PARAM_WEAPON_BLADE";
    case 4: return "PARAM_WEAPON_SPEAR";
    case 5: return "PARAM_WEAPON_TBLADE";
    case 6: return "PARAM_WEAPON_BOW";
    case 7: return "UIO_NEWCHAR_STT_EU_ONEHANDSWORD";
    case 8: return "UIO_NEWCHAR_STT_EU_TWOHANDSWORD";
    case 9: return "UIO_NEWCHAR_STT_EU_DUELAXE";
    case 10: return "UIO_NEWCHAR_STT_EU_DARKSTAFF";
    case 11: return "UIO_NEWCHAR_STT_EU_TWOHANDSTAFF";
    case 12: return "UIO_NEWCHAR_STT_EU_CROSSBOW";
    case 13: return "UIO_NEWCHAR_STT_EU_DAGGER";
    case 14: return "UIO_NEWCHAR_STT_EU_HARP";
    case 15: return "UIO_NEWCHAR_STT_EU_ONEHANDSTAFF";
    default: return nullptr;
    }
}

std::vector<std::string> SkillCostLines(const assets::SkillRow& row, const SkillTextLookup& text) {
    // What a use costs, then the weapon it is used with.
    const auto T = [&](const char* key) { return text ? text(key) : std::string(key); };
    std::vector<std::string> out;
    if (row.hpCost > 0) out.push_back(FormatText(T("PARAM_REQ_HP"), {N(row.hpCost)}));
    if (row.mpCost > 0) out.push_back(FormatText(T("PARAM_REQ_MP"), {N(row.mpCost)}));
    if (row.hpCostRatio != 0) out.push_back(FormatText(T("PARAM_REQ_HP_RATIO"), {N(row.hpCostRatio)}));
    if (row.mpCostRatio != 0) out.push_back(FormatText(T("PARAM_REQ_MP_RATIO"), {N(row.mpCostRatio)}));
    if (row.weaponKinds[0] != 255) {
        const auto name = [&](int kind) { const char* k = WeaponTextKey(kind); return k != nullptr ? T(k) : std::string(); };
        std::string line = T("PARAM_REQ_WEAPON_TYPE") + ": " + name(row.weaponKinds[0]);
        if (row.weaponKinds[1] != 255) line += ", " + name(row.weaponKinds[1]);
        out.push_back(line);
    }
    return out;
}

SkillItemNeeds SkillItemLine(const assets::SkillRow& row, const SkillTextLookup& text) {
    const auto T = [&](const char* key) { return text ? text(key) : std::string(key); };
    SkillItemNeeds needs;
    std::vector<std::pair<int, int>> wanted;
    assets::ForEachSkillParam(row, [&](std::uint32_t code, std::span<const std::uint32_t> a) {
        if (code == SkillTag("reqi") && wanted.size() < 5) wanted.push_back({a.size() > 0 ? int(a[0]) : 0, a.size() > 1 ? int(a[1]) : 0});
    });
    if (wanted.empty()) return needs;
    needs.essential = bool(assets::FindSkillParam(row, SkillTag("reqn")));
    std::string line = T(needs.essential ? "UIIT_STT_EU_ESSENTIAL_ITEM" : "UIIT_STT_EU_REQUIRE_ITEM") + ": ";
    bool first = true;
    for (const auto& [type, sub] : wanted) {
        needs.kinds.push_back(std::uint16_t(type << 8 | (sub & 0xFF)));
        std::string what;
        const char* key = nullptr;
        switch (type) {
        case 1: key = sub == 0 ? "UIO_NEWCHAR_STT_CLOTHES" : nullptr; break;
        case 2: key = sub == 0 ? "UIO_NEWCHAR_STT_LIGHT_ARMOR" : nullptr; break;
        case 3: key = sub == 0 ? "UIO_NEWCHAR_STT_HEAVY_ARMOR" : nullptr; break;
        case 4: key = sub == 0 ? "SN_TAB_SHIELD" : (sub == 1 || sub == 2) ? "UIIT_STT_SHIELD" : nullptr; break;
        case 5: key = sub == 0 ? "UIIT_CTL_WNETWORK_CH_ACCESSORY" : nullptr; break;
        case 6: key = sub == 0 ? "SN_TAB_WEAPON" : WeaponTextKey(sub); break;
        case 9: key = sub == 0 ? "UIO_NEWCHAR_STT_EU_ROBE" : nullptr; break;
        case 10: key = sub == 0 ? "UIO_NEWCHAR_STT_EU_LIGHT_ARMOR" : nullptr; break;
        case 11: key = sub == 0 ? "UIO_NEWCHAR_STT_EU_HEAVY_ARMOR" : nullptr; break;
        case 12: key = sub == 0 ? "UIIT_CTL_WNETWORK_EU_ACCESSORY" : nullptr; break;
        default: break;
        }
        if (key != nullptr) what = T(key);
        if (!first) line += ", ";
        line += what;
        first = false;
    }
    needs.line = std::move(line);
    return needs;
}

std::vector<std::string> SkillRowEffectLines(const assets::SkillRow& row, const SkillTextLookup& text) {
    Lines lines(row, text);
    lines.Effects(row);
    return std::move(lines.out);
}

std::vector<std::string> SkillEffectLines(const assets::SkillRow& row, const assets::SkillTable* skills, const SkillTextLookup& text) {
    Lines lines(row, text);
    if (row.chainNext != 0 && skills != nullptr) {
        lines.ChainEffects(*skills);
    } else {
        lines.Effects(row);
        // A skill whose effect is another's ('rmut': that skill's id) shows that one's too.
        if (const auto m = assets::FindSkillParam(row, SkillTag("rmut")); m && m[0] != 0 && skills != nullptr) {
            if (const assets::SkillRow* other = skills->Find(m[0])) lines.Effects(*other);
        }
    }
    return std::move(lines.out);
}

} // namespace sr::game
