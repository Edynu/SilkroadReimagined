#include "sr/game/BetaQuests.h"

#include <cctype>
#include <format>

namespace sr::game {

namespace {

// China's weapons, then Europe's (itemdata: ITEM_<RACE>_<KIND>_<DEGREE>_A_RARE, every degree 2..11 in 1.188).
constexpr std::string_view kWeapons[] = {"CH_SWORD", "CH_BLADE",  "CH_SPEAR",     "CH_TBLADE", "CH_BOW",  "EU_DAGGER", "EU_SWORD",
                                         "EU_TSWORD", "EU_AXE",   "EU_CROSSBOW",  "EU_DARKSTAFF", "EU_TSTAFF", "EU_HARP", "EU_STAFF"};

struct Step {
    int degree;
    std::string_view need;  // the `start record` line's rest
    std::string_view title; // its name in the quest list
    std::string_view goal;  // what the offer says it asks
};
constexpr Step kSteps[] = {
    {2, "finished 3", "Beta reward: degree 2", "Finish 3 quests."},
    {3, "finished 10", "Beta reward: degree 3", "Finish 10 quests."},
    {4, "finished 25", "Beta reward: degree 4", "Finish 25 quests."},
    {5, "finished 50", "Beta reward: degree 5", "Finish 50 quests."},
    {6, "finished 75", "Beta reward: degree 6", "Finish 75 quests."},
    {7, "finished 100", "Beta reward: degree 7", "Finish 100 quests."},
    {8, "event-matches 10", "Beta reward: degree 8", "Play 10 matches of Capture the Flag or the Battle Arena."},
    {9, "roc 1", "Beta reward: degree 9", "Be in the Peak when the Roc is defeated."},
    {10, "tomb-hardest 1 or finished:QNO_SD_GU_ 5", "Beta reward: degree 10",
     "Clear the Pharaoh's tomb at its hardest (Master), or finish 5 temple quests."},
    {11, "fortress-war 1", "Beta reward: degree 11", "Fight in a fortress war."},
};

} // namespace

std::string CustomNpcCodename(std::string_view name) {
    std::string out = "NPC_";
    for (char c : name) out += c == ' ' ? '_' : char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> SealOfSunWeapons(int degree) {
    std::vector<std::string> out;
    // Seal of Sun is _C_RARE (Star _A_, Moon _B_); degree 11 has none in service in 1.188 (_RefObjCommon Service 0): Seal of
    // Star there, the best it has.
    const char seal = degree >= 11 ? 'A' : 'C';
    for (std::string_view w : kWeapons) out.push_back(std::format("ITEM_{}_{:02}_{}_RARE", w, degree, seal));
    return out;
}

std::string BetaQuestDefinitions() {
    const std::string npc = CustomNpcCodename(kBetaHelperName);
    std::string out;
    for (const Step& s : kSteps) {
        out += std::format("quest {}SUN_{:02}\n", kBetaQuestPrefix, s.degree);
        out += std::format("id {}\n", 40000 + s.degree);
        out += std::format("level {}\n", s.degree * 10);
        out += std::format("name \"{}\"\n", s.title);
        out += std::format("reward-title \"{}\"\n", s.title);
        out += std::format("reward-text \"Choose one {} weapon of degree {}. It is bound to you: it cannot be traded, sold in a "
                           "stall or the Open Market, dropped or put in a guild's storage.\"\n",
                           s.degree >= 11 ? "Seal of Star" : "Seal of Sun", s.degree);
        out += std::format("guide-npc \"{}\"\n", kBetaHelperName);
        out += std::format("guide-condition \"{}\"\n", s.goal);
        out += "repeat 1\n";
        out += std::format("giver {}\nnpc {}\nfinish {}\n", npc, npc, npc);
        out += std::format("goal record {} text \"{} (%d)\"\n", s.need, s.goal);
        for (const std::string& w : SealOfSunWeapons(s.degree)) out += std::format("reward item {} 1\n", w);
        out += "reward choose 1\nreward bound\n";
        out += std::format("talk {} greeting \"Welcome, tester. Every reward here helps us test the game: each can be taken once.\"\n", npc);
        out += std::format("talk {} offer \"{} A {} weapon of degree {} is yours to choose, bound to you.\"\n", npc, s.goal,
                           s.degree >= 11 ? "Seal of Star" : "Seal of Sun", s.degree);
        out += std::format("talk {} accepted \"Your progress is in your quest list. Come back to me when it is done.\"\n", npc);
        out += std::format("talk {} denied \"Come back when you want it.\"\n", npc);
        out += std::format("talk {} done \"Thank you for testing. Choose one weapon.\"\n", npc);
        out += std::format("talk {} not-done \"Not yet. {} Come back when it is done.\"\n", npc, s.goal);
        out += std::format("talk {} bag-full \"Your bag is full. Make room and come back.\"\n", npc);
        out += "\n";
    }
    return out;
}

} // namespace sr::game
