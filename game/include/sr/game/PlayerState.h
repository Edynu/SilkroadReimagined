#pragma once

// The player's own state as the interface shows it (C7c): the
// character's numbers, its target, the quick slots, what acts on it (buffs and states), the
// game's messages, the banners and the chat. The session's world events feed it; what the server does not
// send yet keeps its defaults until its feature comes (C9 the HUD working, C10 combat). The HUD's
// windows are views over it. Headless.

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/game/AcademyView.h"
#include "sr/game/OpenMarketView.h"
#include "sr/game/Inventory.h"
#include "sr/game/Party.h"
#include "sr/game/Jobs.h"
#include "sr/game/Potions.h"
#include "sr/game/Storage.h"
#include "sr/game/TitleSession.h"

namespace sr::game {

// The player's character (the player's bar, the underbar).
struct CharacterStatus {
    std::uint32_t entity = 0;
    std::uint32_t model = 0; // characterdata RefObjID (the portrait)
    std::uint8_t race = 0;   // 0 China, 1 Europe (the race mark)
    std::string name;
    std::uint8_t level = 1, highestLevel = 1;
    std::uint32_t hp = 0, maxHp = 0, mp = 0, maxMp = 0;
    std::uint64_t experience = 0;       // gained in this level
    std::uint64_t experienceToNext = 0; // what the level needs (leveldata.txt); 0 unknown
    std::uint32_t skillExperience = 0;  // toward the next skill point (0..kSkillExperiencePerPoint - 1)
    std::uint32_t skillPoints = 0;
    std::uint16_t strength = 0, intelligence = 0, statPoints = 0;
    // The stat panel's numbers (the server's word on them comes with C9).
    std::uint32_t physicalAttack[2]{}, magicalAttack[2]{};
    std::uint16_t physicalDefence = 0, magicalDefence = 0, hitRatio = 0, parryRatio = 0;
    std::uint8_t berserk = 0; // the berserk (Hwan) level: lit orbs, 0..kBerserkBars
    std::uint8_t berserkLit = 0; // in the mode: the orbs still burning (all five at its start, one going each fifth of its time)
    bool partyLeader = false;
    std::uint32_t party = 0, guild = 0; // the party and guild it is in (0: none; parties come with C7g, guilds C7j)

    static constexpr std::uint32_t kSkillExperiencePerPoint = 400;
    static constexpr std::uint8_t kBerserkBars = 5;
    // 0..1 of the level done (0 when the table does not know the level).
    double ExperienceRatio() const {
        return experienceToNext == 0 ? 0.0 : double(experience) / double(experienceToNext);
    }
};

// A buff, or a state the character is in, on the buffs board.
struct ActiveEffect {
    enum class Kind : std::uint8_t { Skill = 1, State = 2, Item = 3, TimedJob = 4 };
    Kind kind = Kind::Skill;
    std::uint32_t id = 0;    // the skill or item RefObjID, or the state's bit
    std::uint32_t token = 0; // the server's instance of it
    bool debuff = false;     // the lower rows (curses, states)
    double started = 0;      // the clock's second it began at
    double duration = 0;     // seconds; 0: no end
    // A link the player made (proto::LinkAdded): whom it is on (its tooltip's "Target : name").
    std::string target;
    // A timed job (the Forgotten World's re-entry limit): its icon and its tooltip's subject and text keys.
    std::string icon, subject, text;
    // A time service's daily bonus as last told (proto::TimedService::bonus: yesterday's << 16 | today's), spent in battle.
    std::uint32_t bonus = 0;
};

// A mark the player put on someone (the Rogue's Tag Point, 'hntp'):
// its link's token, whom, and where the server last said it was (its minimap shows it there when it
// is out of sight).
struct Mark {
    std::uint32_t token = 0;
    std::uint32_t entity = 0;
    proto::WorldPosition position;
    float yaw = 0;
};

// What the player has selected (the target window).
struct TargetStatus {
    enum class Kind : std::uint8_t {
        None,
        Monster,   // the monster look (every rarity)
        Pet,       // pets, horses, transports, guild soldiers (the common look)
        Npc,       // NPCs (guards show their health)
        Player,
        Structure, // fortress guards, war monsters, barricades
    };
    std::uint32_t entity = 0; // 0: nothing selected
    Kind kind = Kind::None;
    std::uint32_t model = 0;
    std::string name;
    std::uint8_t level = 0;  // the reference level (monsters)
    std::uint8_t rarity = 0; // monsters: 0 general, 1 champion, 3 unique, 4 giant, 5 titan, 6 elite (2: none shown)
    bool party = false;      // a party monster
    bool guard = false;      // an NPC whose health shows
    bool noBar = false;      // pets and guild soldiers: no health bar
    std::uint8_t race = 0;   // players: the race mark
    std::uint8_t job = 0;    // players: 0 none, 1 trader, 2 thief, 3 hunter
    std::vector<ActiveEffect> states; // the states it is in (the buff row; K3)
    std::uint8_t jobLevel = 0;
    std::uint32_t hp = 0, maxHp = 0;
    bool Selected() const { return entity != 0; }
};

// A quick slot (the underbar's 51: 0 the right button's `M`, 1..40 the four pages of ten, 41..50
// the extra bar); it holds a shortcut. The kinds keep the original's
// numbers.
struct QuickSlot {
    enum class Kind : std::uint8_t { Empty = 0, PetCommand = 37, Item = 70, Equipped = 71, Skill = 73, Action = 74, Avatar = 78 };
    Kind kind = Kind::Empty;
    // The pet command or skill id, the inventory / equipment / avatar place, or the action
    // (its type in the top 8 bits).
    std::uint32_t value = 0;
    bool operator==(const QuickSlot&) const = default;
};

struct QuickSlots {
    static constexpr int kCount = 51, kPages = 4, kPerPage = 10, kRightButton = 0, kExtraFirst = 41;
    std::array<QuickSlot, kCount> slots{};
    int page = 0; // the underbar's page, 0..3
    // The underbar's "used" frame: the column (0..9) last used and when (-1: never).
    int usedColumn = -1;
    double usedAt = -1;
    static int Index(int page, int column) { return 1 + page * kPerPage + column; }
};

// A line of the game's messages (the system messages window), with its category: the filter's
// boxes take Acquisition to Game; General and Guide always show.
struct GameMessage {
    enum class Kind : std::uint8_t { General = 0, Acquisition = 1, Combat = 2, Status = 3, Party = 4, Game = 5, Guide = 6 };
    Kind kind = Kind::General;
    std::string text;
    double time = 0; // the clock's second it came at
};

// The three notice banners across the top (GM notices, warnings, quests): the latest text of each.
struct Banner {
    enum class Kind : std::uint8_t { Notice, Warning, Quest };
    std::string text;
    double time = -1; // the clock's second it was posted at; -1 never
};

// A line for the chat window: someone's, or the player's own once the server
// took it; or the player's line the server refused (`result`), which the window reports.
struct ChatEntry {
    proto::ChatKind kind = proto::ChatKind::All;
    std::uint32_t sender = 0; // the speaker's entity when known (its speech bubble)
    std::string name;         // the speaker; the player's own whisper: the one it went to
    std::string text;
    bool own = false;
    proto::ChatResult result = proto::ChatResult::Ok;
    std::uint16_t seconds = 0; // a refusal for chatting too fast: how long it lasts
};

class PlayerState {
public:
    // leveldata.txt's EXP per level (index: the level), for the experience gauge.
    void SetLevels(std::vector<std::uint64_t> experienceToNext) {
        levels_ = std::move(experienceToNext);
        SetLevel(self.level);
    }
    bool HasLevels() const { return !levels_.empty(); }
    // The EXP a level needs to the next (0: past the table, or none read).
    std::uint64_t ExperienceToNext(int level) const {
        return level > 0 && std::size_t(level) < levels_.size() ? levels_[std::size_t(level)] : 0;
    }
    // A world event: WorldEntered starts the character afresh (the quick slots stay: a teleport
    // enters again); the target goes when its entity does.
    void Apply(const TitleEvent& event);
    void Post(GameMessage::Kind kind, std::string text, double time);
    // A line the player sends: numbered (0..250, then 0 again, as the original's counter) and kept
    // until the server answers; send it with that index.
    std::uint8_t QueueChat(proto::ChatKind kind, std::string target, std::string text);
    // A banner's text (replacing what it showed).
    void Notify(Banner::Kind kind, std::string text, double time) { banners[std::size_t(kind)] = {std::move(text), time}; }

    CharacterStatus self;
    TargetStatus target;
    QuickSlots quickSlots;
    std::vector<ActiveEffect> effects; // in arrival order
    std::vector<Mark> marks;
    std::deque<GameMessage> messages;  // oldest first, at most kKeptMessages
    std::uint64_t messagesPosted = 0;  // every one ever posted (views take the new ones)
    std::array<Banner, 3> banners;
    // The inventory as the server says (InventoryList, then each ItemMoved); `inventoryVersion`
    // counts its changes (views refresh), `itemRefusals` the moves the server refused (the window
    // says why).
    Inventory inventory;
    std::uint64_t inventoryVersion = 0;
    // The account's storage once a keeper opened it (StorageList, then each StorageMoved; its moves
    // count in inventoryVersion too).
    Storage storage;
    bool storageOpen = false;
    // Its guild's storage while it holds it (StorageList with guild 1).
    Storage guildStorage;
    bool guildStorageOpen = false;
    // The merchant whose shop is open (its entity; 0: none) and what can be bought back there.
    std::uint32_t shopNpc = 0;
    std::vector<proto::BuybackRecord> buyback;
    std::vector<proto::ItemStatus> itemRefusals;
    std::vector<std::uint16_t> itemRefusalCodes; // a job suit's refusal's original code (ItemMoved::code; 0 none), with each
    // The casting gauge (the original's five kinds, proto::CastingKind): what it is for (the item of kinds 0, 3 and 4, the
    // quest of kind 1), when it began (the HUD's clock) and how long it takes; none while `seconds` is 0.
    struct Casting {
        std::uint32_t item = 0;
        double started = 0, seconds = 0;
        proto::CastingKind kind = proto::CastingKind::Return;
        std::uint32_t quest = 0;
        bool Active() const { return seconds > 0; }
    } casting;
    // Skills (C7f): the masteries the server lists (every one the race may train, in its order, with
    // its level; 0 not trained) and the skills known (skilldata ids, one per group: its level's);
    // `skillsVersion` counts the changes (the Skills page rebuilds).
    std::vector<std::pair<std::uint32_t, int>> masteries;
    std::vector<std::uint32_t> skills;
    std::uint64_t skillsVersion = 0;
    // Waits running for the quick slots' clocks, on the HUD's clock: by skill group, by cool-down
    // group (skilldata col 18's top byte), by item (its refId).
    struct Cooling {
        double start = 0, end = 0;
    };
    std::unordered_map<std::uint32_t, Cooling> skillCooling, groupCooling, itemCooling;
    // An item's wait is its kind's too (game::ReuseLockOf: every HP potion waits with the one drunk):
    // kept in itemCooling under this key with the kind's bit.
    static constexpr std::uint32_t KindCoolingKey(std::uint32_t lockBit) { return 0x80000000u | lockBit; }
    // An item's COOLTIME group (itemdata's "COOLTIME:0x.." pair: every Dimension Hole waits with the one used).
    static constexpr std::uint32_t GroupCoolingKey(std::uint32_t group) { return 0x40000000u | group; }
    // The auto potion as the server keeps it (AutoPotionSettings), and its
    // changes (the window shows them).
    AutoPotion autoPotion;
    std::uint64_t autoPotionVersion = 0;
    // The player's job: its league and alias (JobInfoMessage), and the state its suit gives
    // (EntityJob on itself; kNoJob without the suit).
    JobInfo job;
    std::string jobAlias;
    // The week's contribution as the character data had it on entering the world (as the 1.188 client keeps it,
    // never updated in a session: the contribution ranking's own entry shows this).
    std::uint32_t jobContributionAtEntry = 0;
    std::uint8_t jobState = kNoJob;
    // The player's PK counts (PkCounters: DailyPK, TotalPK, the points) and his
    // own PvP state (game::PvpState): the bar's PK icon while any count is not 0.
    std::uint8_t pkDaily = 0;
    std::uint16_t pkTotal = 0;
    std::uint32_t pkPoints = 0;
    std::uint8_t pvpState = 0;
    // The account's silk (SilkBalanceMessage): bought, gift, points; `silkKnown` once told.
    std::uint32_t silk = 0, giftSilk = 0, silkPoints = 0;
    bool silkKnown = false;
    // The item mall's time services and premium as last told (TimedServices), when (the HUD's
    // clock: their seconds count down from it) and how often.
    proto::TimedServices timedServices;
    double timedServicesAt = 0;
    std::uint64_t timedServicesVersion = 0;
    // The buff board's key of a time service's entry (| its category; its id the item).
    static constexpr std::uint32_t kTimedServiceToken = 0x4D430000u;
    void ShowTimedServices();
    // The player's party and the party matching it sees; `now` the HUD's clock
    // the screen keeps here, for when the party's requests came.
    PartyView party;
    // The player's academy: the Academy page's rows.
    AcademyView academy;
    // The Open Market: its window's rows.
    OpenMarketView openMarket;
    // The board's academy experience buffs (| 0 the accumulation, | 1 + n the n-th received), from `academy`.
    static constexpr std::uint32_t kAcademyToken = 0x41430000u;
    void ShowAcademyBuffs();
    double now = 0;
    std::deque<ChatEntry> chat;   // oldest first, at most kKeptChat
    std::uint64_t chatPosted = 0; // every one ever added (views take the new ones)
    // The names blocked for all chat, kept on the PC: their lines are dropped.
    std::vector<std::string> chatBlocked;
    // /BlockFriendInvitation: a friend proposal is refused without asking.
    bool friendInvitesBlocked = false;

    static constexpr std::size_t kKeptMessages = 100, kKeptChat = 100;

private:
    void SetLevel(std::uint8_t level);
    void AddChat(ChatEntry entry);
    std::vector<std::uint64_t> levels_;
    struct PendingChat {
        proto::ChatKind kind = proto::ChatKind::All;
        std::string target, text;
        bool waiting = false;
    };
    std::array<PendingChat, 251> pendingChat_{};
    std::uint8_t nextChat_ = 0;
};

} // namespace sr::game
