#pragma once

// The item lab's list (L12): every item row in service of the 1.188 client's item table, with the
// kind it is (by its type ids and its data) and the path our game server takes for it (ItemRouteOf: the checks of
// GameServer::UseItem in its order, then the other messages that take an item: the equipment's moves, the alchemy
// window, the shops, ...). Headless: the skill lab's tool writes it to the database for the site's /dev; the item exam
// (to come) reads its expectations from it.

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace sr::assets {
struct ItemRow;
class ItemTable;
class SkillTable;
class CharacterTable;
} // namespace sr::assets

namespace sr::game {

// The path our server takes for an item. The use routes follow GameServer::UseItem's checks in their order (the
// first that fits wins); the rest are the other messages that take an item.
enum class ItemRoute : std::uint8_t {
    Equip,           // worn: an item move into its place (Inventory::Move, CanEquip)
    JobSuit,         // worn in the job place: the job's rules (SuitRefusal), then the 10 s change
    Avatar,          // worn in the avatar places (MoveAvatar)
    Ammunition,      // worn beside the bow or the crossbow, one taken at each shot (TakeAmmunition)
    TimeTicket,      // UseMallService: a gold, silver or skill time ticket
    Premium,         // UseMallService: the premium package
    Expansion,       // UseMallService: the bag or the storage grows
    Resurrection,    // UseMallService: rising while dead
    SpiritExtension, // UseMallItemOn: the Devil's Spirit's awakening longer
    PetGrowth,       // UseMallItemOn: the potion of growth on a growth pet's item
    PetItem,         // UseMallItemOn -> Coses::UseMallItemOnPet: a watch or a pet skill's jobs on its pet
    Hole,            // Instances::UseItem: a Forgotten World instance opened
    Talisman,        // Instances::UseItem: registered in the collection book
    FlagCard,        // FlagWorlds::UseItem: So-Ok's Subscription Card
    PetSummon,       // Coses::UsePet: a pet's item
    PetFood,         // Coses::UseItem: the attack pet's hunger
    CosSummon,       // Coses::UseItem: a ride's or a transport's scroll
    Fortress,        // FortressUseItem: a fortress war item with its place or target
    BuffScroll,      // UseItem: the skill Desc1 names, as a buff on the reader
    MonsterMask,     // UseItem: the monster it carries, as the user's shape
    Recovery,        // UseItem -> UseRecovery: a potion or a pill
    PetRecovery,     // UseItem -> UsePetRecovery: the COS's HP
    PetCure,         // UseItem -> UsePetCure: the COS's bad states (the pets' universal pill)
    StallDecoration, // UseItem: the stall's look
    RepairHammer,    // UseItem -> UseHammer
    ReverseReturn,   // UseItem: the reverse return scroll's three ways
    ThiefDen,        // UseItem: the thieves' town scroll
    ReturnScroll,    // UseItem: to the return point
    Firework,        // UseItem: used up, its show on the screens around
    BerserkPotion,   // UseItem: the berserk gauge full
    BonusScroll,     // UseItem: a timed bonus by its key (experience, skill experience, the berserk time)
    QuestUse,        // UseItem: taken by its quest's handler (the quest manager), else refused
    GuildRecall,     // UseItem -> UseGuildRecall: the guild's members asked over the shard to come to its master
    GenderSwitch,    // UseItemOn -> UseItemOnService: the armour's other sex
    PetTreatment,    // CosUseOnPet: a dead attack pet revived, a pick pet's time extended
    SkinChange,      // SkinChange: its window's new model
    SkillWithdraw,   // Withdraw: taken by the skill and mastery withdrawal
    MagicCube,       // CubeMove: a store of B elixirs
    GlobalChat,      // a global chat line takes one
    GuildCrest,      // the guild window's crest takes one
    Alchemy,         // the alchemy window: strengthening, making a stone, breaking down
    Socket,          // the alchemy window's Socket page
    MagicPop,        // Magic POP: a card at its NPC, a coupon its prize
    TradeGood,       // the trade: bought into the transport, sold at a specialty NPC
    QuestItem,       // never used: its quest gives it, counts it and takes it
    Gold,            // gold's look on the ground
    Declined,        // ours refuses it on purpose (`why` says why)
    OriginalRefuses, // the original's server refuses its use too: ours answers the same code (`why` says which)
    Unhandled,       // usable by its data (CanUse), but nothing of ours takes it: refused
    NotUsable,       // CanUse 0 and nothing takes it: carried, stored, sold, traded
};
inline constexpr int kItemRoutes = int(ItemRoute::NotUsable) + 1;
const char* ItemRouteName(ItemRoute route);    // "equip", "recovery", ...: the list's and the site's
const char* ItemRouteText(ItemRoute route);    // what our server does with it, in a line
const char* ItemRouteHandler(ItemRoute route); // where in our server (the function, the message)
const char* ItemRouteNote(ItemRoute route);    // the topic of its behaviour ("" none)

// What the route needs beyond the row (each may be null: its check is left out then): the skills (a scroll's skill),
// the characters (what a COS scroll summons), the collection book's talismans (their codenames).
struct ItemRouteData {
    const assets::SkillTable* skills = nullptr;
    const assets::CharacterTable* characters = nullptr;
    const std::unordered_set<std::string>* talismans = nullptr;
};
struct ItemRouting {
    ItemRoute route = ItemRoute::NotUsable;
    std::string why;  // a refusal's reason, a rule of the route, or what the route leaves out
    bool gap = false; // `why` names a known gap: the route takes it, not as the original does
};
ItemRouting ItemRouteOf(const assets::ItemRow& row, const ItemRouteData& data);

// How far our server goes with an item, before any exam (the list's status; the exam's results come over it on the
// site: tested, failed): its route takes it, takes it with a known gap, refuses it though usable, refuses it on
// purpose, or there is nothing to use (carried, stored, sold, traded).
enum class ItemState : std::uint8_t { Implemented, Partly, Unhandled, Declined, Carried };
ItemState ItemStateOf(const ItemRouting& routing);
const char* ItemStateName(ItemState state); // "implemented", "partly", "unhandled", "declined", "carried"

// The kind of an item, by its type ids and its data: a group ("weapon", "armour", "potion", "alchemy", ...) and its
// kind within it ("sword", "garment", "hp", "elixir", ...). Every in-service row has one ("other" / "unknown" none).
struct CatalogueKind {
    const char* group = "other";
    const char* kind = "unknown";
};
CatalogueKind CatalogueKindOf(const assets::ItemRow& row);

// Where it comes from, by its flags and its codename: "mall" (CashItem, ITEM_MALL_), "event" (ITEM_EVENT_, ITEM_ETC_E<date>_),
// "rental" (ITEM_EVENT_RENT_), "quest" (ITEM_QNO_, ITEM_QSP_, ITEM_QCX_, ITEM_QTUTORIAL_), "gm" (ITEM_GM_), else "game".
const char* ItemSourceOf(const assets::ItemRow& row);

// A requirement pair's name (cols 32..39): 1 the level, 2..4 a job's level (trader, thief, hunter); others as written
// ("type N"; the masteries' ids are named by the caller).
const char* RequirementName(int type);

struct ItemCatalogueEntry {
    const assets::ItemRow* row = nullptr;
    CatalogueKind kind;
    ItemRouting routing;
    const char* source = "game";
};

// Every in-service row of the table, ordered by its type ids then its id.
std::vector<ItemCatalogueEntry> BuildItemCatalogue(const assets::ItemTable& items, const ItemRouteData& data);

} // namespace sr::game
