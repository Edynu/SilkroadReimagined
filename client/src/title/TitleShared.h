#pragma once

// What the title screens share (C5e): the connection service, the title session (gateway, login,
// lobby) and the interface data. The title screen makes it; the lobby screens take it over through
// the screen context's handoff, so the session lives on from one screen to the next.

#include <atomic>
#include <chrono>
#include <iterator>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "screens/Screen.h"
#include "sr/assets/CollectionBook.h"
#include "sr/assets/Dress.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/assets/ItemGlow.h"
#include "sr/assets/Shop.h"
#include "sr/assets/SkillAnimation.h"
#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Skills.h"
#include "sr/game/TitleSession.h"
#include "sr/net/Connection.h"
#include "title/UiKit.h"

namespace sr::client {

// The character tables the lobby and the create screens read (loaded once, on jobs): the
// creation rules (the server checks them too), and characterdata / itemdata to dress characters;
// then the game screen's tables. Each table on a job of its own: the lobby needs only the first
// three (Dressable), the game screen all (Usable).
struct CharacterData {
    assets::AtStructTable atStructs;     // a fortress structure's models by damage (the game screen)
    std::atomic<bool> dressReady{false}; // characters, items and the rules are in
    std::atomic<bool> ready{false};      // every table is in
    bool ok = false;                     // characters, items and the rules loaded (set before dressReady)
    game::CharacterRules rules;
    assets::CharacterTable characters;
    assets::ItemTable items;
    assets::TeleportTable teleports; // the gates (the game screen; empty when it failed to load)
    std::vector<assets::NpcPosition> npcPlaces; // npcpos.txt: where the NPCs and nests stand (the F1 Teleport panel's NPCs)
    assets::SkillTable skills;       // skilldata (the game screen's tooltips; empty when it failed to load)
    assets::ShopTable shops;         // the merchants' goods (the store window; empty when it failed to load)
    assets::SkillAnimationTable skillAnimations; // skilleffect.txt: which clips a skill plays (fights; empty when it failed to load)
    assets::ItemGlowTable itemGlows; // itemrare.txt, itemoptionefp.txt: the effects of weapons and shields (empty when it failed to load)
    assets::EquipGlowTable equipGlows; // itemoption.txt, itemtypenumber.txt: the plus glow (empty when it failed to load)
    assets::VisualChangeTable visuals; // charactervisualchange.txt: what a worn visual hides (a job suit; empty when it failed to load)
    assets::BanditBodies banditBodies; // usableresobjiddata.txt: the figures the jobs' bandits take
    game::BanditImbues banditImbues;   // the imbue each bandit skill shows (from the skills, once they are in)
    std::vector<std::uint32_t> monsterExperience; // leveldata.txt: a general monster's experience by level (how many orbs a kill sends)
    std::vector<std::uint64_t> levels; // leveldata.txt: the EXP each level needs (the HUD; empty when missing)
    std::vector<std::uint32_t> masteryCosts; // leveldata.txt col 2: a mastery's next level's skill points
    std::vector<std::array<std::int64_t, 3>> jobExp; // leveldata.txt cols 6..8: a job level's experience (the job windows)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> levelGold; // levelgold.txt: GoldMin, GoldMax by level (the quest potion's price)
    assets::CollectionBookData collection;   // the talisman collections (empty when it failed to load)
    bool Ready() const { return ready.load(std::memory_order_acquire); }
    bool Usable() const { return Ready() && ok; }
    // The lobby's part: enough to dress characters and check a new one.
    bool DressReady() const { return dressReady.load(std::memory_order_acquire); }
    bool Dressable() const { return DressReady() && ok; }

    // The loading jobs' bookkeeping.
    std::atomic<int> partsLeft{0}, dressPartsLeft{0};
    bool charactersOk = false, itemsOk = false;
};

struct TitleShared {
    std::unique_ptr<net::NetService> net;
    std::unique_ptr<game::TitleSession> session;
    UiData data;
    std::string account;   // remembered for the next login
    std::uint16_t server = 0;
    std::string serverName;
    // Loaded by the first lobby screen; the counter is waited for before the file system goes.
    std::shared_ptr<CharacterData> characters;
    core::JobCounter charactersJob;
    int race = 0; // the race the pedestal picked for the create screen (0 Europe, 1 China)
    bool scripted = false; // --create / --select done (once a run)
    bool restarted = false; // --logout-after's Restart done (once a run; with --reselect the select comes again)
    bool reselected = false; // that select made
    void LoadCharacters(core::JobSystem& jobs, const assets::FileSystem& files);

    static constexpr const char* kHandoff = "title";
    // Why the server closed the connection (proto::Disconnected), for the title screen to say (a std::string).
    static constexpr const char* kFarewell = "farewell";
    static std::string FarewellOf(const game::proto::Disconnected& bye) {
        if (bye.reason == game::proto::DisconnectReason::Blocked) return BlockedWords(bye.text, bye.until);
        if (bye.reason == game::proto::DisconnectReason::Replaced) return "The character entered the game from another connection.";
        return "Disconnected by a game master" + (bye.text.empty() ? std::string(".") : " (" + bye.text + ").");
    }
    // A blocked account's words (the login's refusal, a kick's): until when (local time; 0: no end) and why.
    static std::string BlockedWords(const std::string& reason, std::int64_t until) {
        std::string words = "The account is blocked";
        if (until > 0) {
            const std::time_t t = static_cast<std::time_t>(until);
            std::tm local{};
            localtime_s(&local, &t);
            char date[32] = {};
            std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", &local);
            words += std::string(" until ") + date;
        }
        return words + (reason.empty() ? std::string(".") : ": " + reason + ".");
    }
    static std::shared_ptr<TitleShared> From(ScreenContext& context) {
        const auto it = context.handoff.find(kHandoff);
        return it == context.handoff.end() ? nullptr : std::static_pointer_cast<TitleShared>(it->second);
    }
};

// Reads the character tables on jobs, each table on its own (the counter is waited for before the
// file system goes). Characters and items first and ahead of the scenery's streaming (the lobby
// waits for them), the rules once both are in; the game screen's tables behind them.
inline std::shared_ptr<CharacterData> LoadCharacterData(core::JobSystem& jobs, const assets::FileSystem& files, core::JobCounter& counter) {
    auto characters = std::make_shared<CharacterData>();
    const auto start = std::chrono::steady_clock::now();
    const auto finished = [start](CharacterData& data) {
        // The last part to finish says so (each part's writes come before its count goes down).
        if (data.partsLeft.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        data.ready.store(true, std::memory_order_release);
        core::LogInfo("title: game tables in {:.2f} s",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    };
    // Characters and items; the second of them to finish loads the rules (they need both).
    const auto dressPart = [start, finished](CharacterData& data, const assets::FileSystem& fs) {
        if (data.dressPartsLeft.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::string error = "characterdata or itemdata did not load";
            data.ok = data.charactersOk && data.itemsOk && data.rules.Load(fs, data.characters, data.items, error);
            if (!data.ok) core::LogWarning("title: character tables: {}", error);
            // The jobs' bandits' figures: the character rows usableresobjiddata.txt lists.
            if (std::string e; data.charactersOk && !data.banditBodies.Load(fs, data.characters, e)) core::LogWarning("title: the bandits' figures: {}", e);
            data.dressReady.store(true, std::memory_order_release);
            core::LogInfo("title: character tables in {:.2f} s",
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        finished(data);
    };
    using Part = void (*)(CharacterData&, const assets::FileSystem&);
    const Part rest[] = {
        [](CharacterData& d, const assets::FileSystem& fs) {
            if (std::string e; !d.teleports.Load(fs, e)) core::LogWarning("title: teleports: {}", e);
            if (const auto file = fs.Read("server_dep\\silkroad\\textdata\\npcpos.txt")) {
                if (!assets::DecodeNpcPositions(core::DecodeTextFile(file->Bytes()), d.npcPlaces)) core::LogWarning("title: npcpos.txt did not decode");
            }
        },
        [](CharacterData& d, const assets::FileSystem& fs) {
            if (std::string e; !d.collection.Load(fs, e)) core::LogWarning("title: collection book: {}", e);
        },
        [](CharacterData& d, const assets::FileSystem& fs) {
            if (const auto file = fs.Read("server_dep\\silkroad\\textdata\\atstructeffect.txt")) d.atStructs.Decode(core::DecodeTextFile(file->Bytes()));
        },
        [](CharacterData& d, const assets::FileSystem& fs) {
            if (std::string e; !d.skills.Load(fs, e)) core::LogWarning("title: skills: {}", e);
            d.banditImbues.Build(d.skills); // the imbues the bandits' skills show (game::BanditImbues)
        },
        [](CharacterData& d, const assets::FileSystem& fs) {
            if (std::string e; !d.shops.Load(fs, e)) core::LogWarning("title: shops: {}", e);
        },
        [](CharacterData& d, const assets::FileSystem& fs) {
            if (std::string e; !d.skillAnimations.Load(fs, e)) core::LogWarning("title: skill animations: {}", e);
            if (std::string e; !d.itemGlows.Load(fs, e)) core::LogWarning("title: item glows: {}", e);
            if (std::string e; !d.equipGlows.Load(fs, e)) core::LogWarning("title: plus glows: {}", e);
            if (std::string e; !d.visuals.Load(fs, e)) core::LogWarning("title: visual changes: {}", e);
        },
        [](CharacterData& d, const assets::FileSystem& fs) {
            if (auto file = fs.Read("server_dep\\silkroad\\textdata\\leveldata.txt")) {
                std::string e;
                const std::string text = core::DecodeTextFile(file->Bytes());
                if (!assets::DecodeLevelTable(text, d.levels, e)) core::LogWarning("title: levels: {}", e);
                if (!assets::DecodeMasteryCosts(text, d.masteryCosts, e)) core::LogWarning("title: mastery costs: {}", e);
                if (!assets::DecodeMonsterExperience(text, d.monsterExperience, e)) core::LogWarning("title: monster experience: {}", e);
                if (!assets::DecodeJobExperience(text, d.jobExp, e)) core::LogWarning("title: job experience: {}", e);
            }
            if (auto file = fs.Read("server_dep\\silkroad\\textdata\\levelgold.txt")) {
                std::string e;
                if (!assets::DecodeLevelGold(core::DecodeTextFile(file->Bytes()), d.levelGold, e)) core::LogWarning("title: level gold: {}", e);
            }
        },
    };
    characters->dressPartsLeft.store(2, std::memory_order_relaxed);
    characters->partsLeft.store(2 + int(std::size(rest)), std::memory_order_relaxed);
    jobs.Run(
        [data = characters, fs = &files, dressPart] {
            std::string e;
            data->charactersOk = data->characters.Load(*fs, e);
            if (!data->charactersOk) core::LogWarning("title: characterdata: {}", e);
            dressPart(*data, *fs);
        },
        core::JobPriority::Normal, &counter);
    jobs.Run(
        [data = characters, fs = &files, dressPart] {
            std::string e;
            data->itemsOk = data->items.Load(*fs, e);
            if (!data->itemsOk) core::LogWarning("title: itemdata: {}", e);
            dressPart(*data, *fs);
        },
        core::JobPriority::Normal, &counter);
    for (const Part part : rest) {
        jobs.Run(
            [data = characters, fs = &files, part, finished] {
                part(*data, *fs);
                finished(*data);
            },
            core::JobPriority::Streaming, &counter);
    }
    return characters;
}

inline void TitleShared::LoadCharacters(core::JobSystem& jobs, const assets::FileSystem& files) {
    if (!characters) characters = LoadCharacterData(jobs, files, charactersJob);
}

// A lobby refusal as the original's text.
inline std::string LobbyStatusText(const UiData& d, game::proto::LobbyStatus status) {
    using game::proto::LobbyStatus;
    switch (status) {
    case LobbyStatus::Ok: return {};
    case LobbyStatus::InvalidCreation: return d.Text("UIO_SMERR_INVALID_CHARGEN_INFO");
    case LobbyStatus::NoWeapon: return d.Text("UIO_MSG_ERROR_CHARACTER_SELECTWEAPON");
    case LobbyStatus::TooManyCharacters: return d.Text("UIO_MSG_ERROR_CHARACTER_OVER");
    case LobbyStatus::Failed: return d.Text("UIO_SMERR_FAILED_TO_CREATE_CHARACTER");
    case LobbyStatus::BadName: return d.Text("UIO_MSG_ERROR_CHARACTER_NAME_STRING");
    case LobbyStatus::NameNotAllowed: return d.Text("UIO_SMERR_NOT_ALLOWED_CHARNAME");
    case LobbyStatus::NameTaken: return d.Text("UIO_MSG_ERROR_ID");
    case LobbyStatus::NotFound: return "No such character.";
    case LobbyStatus::Deleting: return "The character is being deleted: restore it first.";
    case LobbyStatus::NotDeleting: return "The character is not being deleted.";
    case LobbyStatus::Busy: return d.Text("UIO_MSG_ERROR_SEVER_CONNECT");
    case LobbyStatus::GuildMaster: return "A guild's master cannot be deleted: leave the guild or hand it on first.";
    }
    return {};
}

// The layouts the title screens read.
inline constexpr const char* kTitleLayout = "resinfo\\pstitle.txt";
inline constexpr const char* kSelectLayout = "resinfo\\pscharacterselect.txt";
inline constexpr const char* kCreateChinaLayout = "resinfo\\pscharactercreatechina.txt";
inline constexpr const char* kCreateEuropeLayout = "resinfo\\pscharactercreate_europe.txt";

} // namespace sr::client
