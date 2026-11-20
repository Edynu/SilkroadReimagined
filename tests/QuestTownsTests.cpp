// The original's coded quests found missing (server/common OriginalQuestsTowns.cpp): the definitions
// read back the same, and every NPC, monster, item and text they name is in the 1.188 client's files.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <set>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/UiLayout.h"
#include "sr/game/BetaQuests.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/Quest.h"
#include "sr/server/QuestScript.h"

using namespace sr;

namespace {

// Whether the towns' file defines the quest (its line "quest CODE", the name ending at a space or the line's end).
bool InTowns(const std::string& code) {
    const std::string_view text = server::CodedQuestsTowns();
    const std::string line = "\nquest " + code;
    for (std::size_t at = text.find(line); at != std::string_view::npos; at = text.find(line, at + 1)) {
        const std::size_t end = at + line.size();
        if (end == text.size() || text[end] == ' ' || text[end] == '\r' || text[end] == '\n') return true;
    }
    return false;
}

std::vector<game::QuestDef> Towns() {
    std::vector<game::QuestDef> coded;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    std::vector<game::QuestDef> out;
    for (game::QuestDef& q : coded)
        if (InTowns(q.codename)) out.push_back(std::move(q));
    return out;
}

} // namespace

TEST_CASE("Coded quests (towns): our definitions read back the same, each with a giver and a finisher") {
    const std::vector<game::QuestDef> towns = Towns();
    CHECK(towns.size() >= 34);
    std::set<std::string> seen;
    std::string error;
    for (const game::QuestDef& q : towns) {
        CAPTURE(q.codename);
        CHECK(seen.insert(q.codename).second);
        game::QuestDef back;
        REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(q), back, error), error);
        CHECK(back == q);
        CHECK(q.givers.size() == 1);
        // A finisher, or paid where its delivery is handed over ; a story (no goal) or a talk game neither.
        const bool story = q.goals.empty() || !q.talkGame.empty();
        CHECK((story || !q.finisher.empty() || std::any_of(q.goals.begin(), q.goals.end(), [](const game::QuestGoal& g) { return g.kind == game::QuestGoalKind::Deliver; })));
    }
    // QNO_CH_PRIEST_1: eight lithography pieces, each paid by the piece (its setup's loop).
    const auto priest = std::find_if(towns.begin(), towns.end(), [](const game::QuestDef& q) { return q.codename == "QNO_CH_PRIEST_1"; });
    REQUIRE(priest != towns.end());
    REQUIRE(priest->goals.size() == 8);
    for (const game::QuestGoal& g : priest->goals) {
        CHECK(g.apiece);
        CHECK(g.apieceGold == 125);
        CHECK(g.apieceExp == 100);
    }
}

TEST_CASE("Coded quests (towns): every NPC, monster, item and text they name is the 1.188 client's") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr || !std::filesystem::exists(dir)) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    assets::TextCatalog texts;
    REQUIRE_MESSAGE(assets::LoadTextCatalog(fs, "server_dep\\silkroad\\textdata\\textquest.txt", texts, error), error);
    const auto object = [&](const std::string& code) {
        CAPTURE(code);
        CHECK(characters.Find(std::string_view(code)) != nullptr);
    };
    const auto item = [&](const std::string& code) {
        CAPTURE(code);
        CHECK(items.Find(std::string_view(code)) != nullptr);
    };
    const auto text = [&](const game::QuestText& t) {
        if (t.key.empty()) return;
        CAPTURE(t.key);
        CHECK(texts.contains(t.key));
    };
    // The beta quests' weapons (game/BetaQuests.h): every Seal of Sun weapon of degrees 2..11, and So-Ok's look for the helper.
    for (int degree = 2; degree <= 11; ++degree)
        for (const std::string& w : game::SealOfSunWeapons(degree)) item(w);
    object(std::string(game::flag::kSoOk));
    for (const game::QuestDef& q : Towns()) {
        CAPTURE(q.codename);
        for (const std::string& n : q.givers) object(n);
        for (const std::string& n : q.npcs) object(n);
        if (!q.finisher.empty()) object(q.finisher);
        for (const game::QuestDef::Raise& rz : q.raises) object(rz.monster);
        for (const auto& i : q.startItems) item(i.item);
        for (const game::QuestGoal& g : q.goals) {
            for (const std::string& m : g.monsters) object(m);
            for (const auto& s : g.sources) object(s.monster);
            if (!g.item.empty()) item(g.item);
            for (const auto& i : g.items) item(i.item);
            if (!g.npc.empty()) object(g.npc);
            text(g.talk);
            text(g.text);
        }
        for (const game::QuestTalk& t : q.talks) {
            object(t.npc);
            text(t.text);
        }
    }
}
