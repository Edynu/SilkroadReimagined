#pragma once

// The HUD's windows (C7c): the original's window classes as views over the
// player's state (game::PlayerState) and the world around it (game::WorldView), registered with the
// interface by their class names. What a window asks of the game goes to the HUD manager as a
// request; what the player arranges (the quick slots, the underbar's page) the window changes in
// the state itself.

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "sr/assets/CollectionBook.h"
#include "sr/assets/Gacha.h"
#include "sr/assets/GameGuide.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Options.h"
#include "sr/assets/Shop.h"
#include "sr/assets/UiLayout2dt.h"
#include "sr/assets/WorldMap.h"
#include "sr/game/Fortress.h"
#include "sr/game/Community.h"
#include "sr/game/AlchemyProtocol.h"
#include "sr/game/Exchange.h"
#include "sr/game/ArenaView.h"
#include "sr/game/FlagView.h"
#include "sr/game/Stall.h"
#include "sr/game/QuestView.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/game/Guild.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Inventory.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/PlayerState.h"
#include "sr/game/Spirit.h"
#include "sr/game/WorldProtocol.h"
#include "sr/game/WorldView.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Hotkeys.h"
#include "sr/ui/Interface.h"

namespace sr::client {

class Hud;
class ConfirmBox;

// What the windows read and whom they ask: the screen that owns the HUD keeps it.
// The item mall as the window keeps it (HudMall.cpp): the catalogue, the stock the server told,
// the basket (the client's own, emptied when the mall closes), the avatars tried on the preview by their kind
// (game::MallAvatarKind: the good's tab and slot), a purchase under way, the history's last answer.
struct MallView {
    const assets::MallTable* catalog = nullptr;
    std::map<std::uint32_t, int> stock;
    std::vector<std::pair<std::uint32_t, int>> basket;
    std::map<int, std::pair<std::uint32_t, int>> worn;
    int dealing = 0;
    float previewYaw = 0.0f;
    game::proto::SilkHistory history;
    int historyState = 0; // 0 none asked, 1 waiting, 2 answered
    std::uint64_t historyVersion = 0, version = 0;
};

// What the performance panel shows (ours; the game screen fills it every frame, PlayScreen::SamplePerformance): the
// frames of the last second and the game connection's traffic over it.
struct HudPerformance {
    double fps = 0;            // frames shown in the last second
    double frameMs = 0;        // their average time
    double worstMs = 0;        // the longest of them
    double latencyMs = -1;     // a walk's round trip to the game server, the last one (-1: none measured yet)
    double bytesIn = 0, bytesOut = 0;       // the game connection, a second
    double messagesIn = 0, messagesOut = 0; // the same in messages
    std::uint64_t totalIn = 0, totalOut = 0; // bytes since the connection was made
    bool connected = false;
};

// A bug report as the report window sends it (ours; the build's SR_BUG_REPORTS): what the player wrote and how the
// screenshot is to be taken (the report window is never in it).
struct BugReportForm {
    int category = 0; // game::proto::kBugCategories
    std::string subject, text;
    bool screenshot = true;
    bool withInterface = true; // the HUD in the screenshot (else the scene alone)
};

struct HudContext {
    // The performance panel's numbers (null: none, e.g. the gallery).
    const HudPerformance* performance = nullptr;
    // The bug report's sending (null: no reports, the performance panel shows no report button).
    std::function<void(BugReportForm)> bugReport;
    game::PlayerState* state = nullptr;
    // The player's guild and union (the guild window; null: none yet), its requests to the server
    // (GuildProtocol.h, encoded; null: offline) and the 1.188 windows' binary layouts by path.
    const game::GuildView* guild = nullptr;
    // Friends, the block list and letters (the guild window's Community side; null: none yet).
    const game::CommunityView* community = nullptr;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> communityRequest;
    // The Academy's requests (AcademyProtocol.h).
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> academyRequest;
    // The Open Market's requests (OpenMarketProtocol.h).
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> openMarketRequest;
    // The exchange with another player (the exchange window; null: none yet), its requests
    // (ExchangeProtocol.h, encoded; null: offline) and its window's Confirm (confirm, else approve).
    const game::ExchangeView* exchange = nullptr;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> exchangeRequest;
    std::function<void()> exchangeConfirm;
    // Alchemy (HudAlchemy.cpp): its requests (AlchemyProtocol.h, encoded; null: offline); its
    // checks read the magic options below.
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> alchemyRequest;
    // Stalls and the stall network (HudStall.cpp): the player's stall and the network's
    // pages (null: none yet), the network's classes and types, the requests (StallProtocol.h, encoded; null:
    // offline) and a town's name (regioninfo.txt's area index).
    const game::StallView* stall = nullptr;
    // The quests: the player's as the server told them (null: none yet) and the requests
    // (QuestProtocol.h, encoded; null: offline).
    const game::QuestView* quests = nullptr;
    // The level-up's quest guide (proto::QuestGuide): the rows its window lists.
    std::vector<game::proto::QuestGuide::Row> questGuide;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> questRequest;
    // The Forgotten World (HudInstance.cpp): the collection book (theme << 8 | slot) and the
    // collections' tables (null: none yet), the recall pillar's list and its revision, the requests (InstanceProtocol.h,
    // encoded; null: offline).
    const std::set<std::uint32_t>* book = nullptr;
    const assets::CollectionBookData* collection = nullptr;
    const game::proto::SummonList* summonList = nullptr;
    int summonRevision = 0;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> instanceRequest;
    // Magic POP (HudMagicPop.cpp): the sets, the machines and the selection's tree (null: not read
    // yet) and the requests (MallProtocol.h's PopPlay, PopExchange; encoded; null: offline).
    const assets::GachaTable* gacha = nullptr;
    // The item mall (HudMall.cpp): its view (null: not loaded) and its requests (MallProtocol.h, encoded; null: offline).
    MallView* mall = nullptr;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> mallRequest;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> popRequest;
    // The jobs' leagues (HudJobs.cpp): the requests (JobProtocol.h, encoded; null: offline).
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> jobRequest;
    // The jobs' trade (HudTrade.cpp): its requests (TradeSell, TradeSellAll, TradeBoardQuery);
    // the stars (TradeScale); the specialty NPC whose shop is open (its entity; 0 none), the thieves' buyer's, its prices
    // (TradePrices: item -> gold); a purchase's steps still to send (its good's tab and slot); the trade centres
    // (specialnpcdata.txt's NPCs) and the price board's last answer.
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> tradeRequest;
    std::uint8_t tradeStars = 0;
    game::proto::TradeScale tradeScale; // the last told, with its rule (the deal window's scale of a count typed)
    std::uint32_t tradeNpc = 0;
    bool tradeStolenBuyer = false;
    std::map<std::uint32_t, std::uint32_t> tradePrices;
    int tradeBuyLeft = 0, tradeBuyTab = 0, tradeBuySlot = 0;
    // A purchase of more than a stack: the original's box saying it runs ("You are currently purchasing special goods."),
    // its Cancel stopping the rest; closed when the last step is answered.
    ConfirmBox* tradeBuyBox = nullptr;
    std::vector<std::uint32_t> tradeCentres;
    game::proto::TradeBoard tradeBoard;
    std::uint64_t tradeBoardVersion = 0;
    // A line for the system messages (the windows' own warnings).
    std::function<void(const std::string& line)> systemLine;
    // Whether an entity in sight is an NPC with something about the player's quests (its minimap sign).
    std::function<bool(std::uint32_t entity)> questNpc;
    // The quest chosen by its row's map button (the 1.188 client's: its points on the minimap; 0 none), and where its
    // NPCs stand now (game::QuestTaskNpcs at npcpos.txt's places).
    std::uint32_t trackedQuest = 0;
    std::function<std::vector<game::proto::WorldPosition>(std::uint32_t quest)> questPlaces;
    const assets::StallNetworkData* stallNetwork = nullptr;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> stallRequest;
    std::function<std::string(int town)> townName;
    // COS (HudCos.cpp): the player's own, in the order of their status icons; the one the command
    // bar orders (0: none, the manager picks the first); a command on one (kCos*, the Pet hotkeys' numbers).
    struct CosShown {
        std::uint32_t entity = 0, model = 0;
        std::uint8_t kind = 0; // game::CosKind
        std::uint32_t hp = 0, maxHp = 0;
        bool ridden = false;
        std::string name;      // a pet's (empty: none given)
        std::uint8_t level = 0;
        std::uint16_t hgp = 0; // an attack pet's hunger points (0..10000)
        std::uint32_t option = 0; // a grab pet's settings (proto::kGrab*)
        // A grab pet's bag (CosBag): its places and what is in them; its version counts its changes.
        std::uint8_t bagSize = 0;
        std::map<std::uint8_t, game::ItemInstance> bag;
        std::uint64_t bagVersion = 0;
        std::uint64_t experience = 0; // an attack pet's, in its level (the window's EXP row)
        std::int64_t rentUntil = 0;   // a grab pet's rental's end (Unix seconds; 0: none; the window's Rental period)
        bool aggressive = true;       // an attack pet's stance (its command's icon)
    };
    std::vector<CosShown> coses;
    // The pets of the summoning items in the bag (CosPetItems), by the _CharCOS row an item's durability names: what
    // its tooltip says (name, level, Normal / Dead, a grab pet's rental) and its slot shows (an edge running while out).
    struct PetItemShown {
        std::uint32_t model = 0;
        std::uint8_t state = 0; // game::CosState
        std::string name;
        std::uint8_t level = 0;
        std::int64_t rentUntil = 0; // Unix seconds on this machine's clock (0: none, or over)
        double lifeFrom = -1;       // back from dead at this HUD time: its slot's pt_life_effect (-1 none)
    };
    std::map<std::int64_t, PetItemShown> petItems;
    std::uint32_t cosSelected = 0;
    int cosTab = -1; // a page of the COS window asked for (scripts: 0 Basic Info, 1 Inventory, 2 Setting; -1 none)
    std::function<void(int command, std::uint32_t entity)> cosCommand;
    // Its requests to the server (CosProtocol.h, encoded; null: offline): a bag move, a grab pet's settings.
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> cosRequest;
    // Free the player's team (0 none) and whether a change runs, the request.
    std::uint8_t freePvpTeam = 0;
    bool freePvpChanging = false;
    std::function<void(std::uint8_t team)> freePvpRequest;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> guildRequest;
    std::uint32_t guildNpc = 0; // the guild NPC whose row was chosen last (the master windows' requests go with it)
    // The fortresses and the official's window as the server told them (null: none yet), and the
    // fortress war's requests to the server (FortressProtocol.h, encoded; null: offline).
    const game::FortressView* fortress = nullptr;
    // The fortress NPC whose row was chosen last (the manager's windows' requests go with it), and whether a smith's or a
    // trainer's row asked its window open (its query's answer opens it).
    std::uint32_t fortressNpc = 0;
    bool forgeAsked = false;
    // A guild's emblem at that revision as a texture path (empty: not here yet; asked for then).
    std::function<std::string(std::uint32_t guild, std::uint32_t revision)> guildCrest;
    // A game picture's key pixels (32-bit ARGB, one of `keys`) in `colour`, the rest clear: a texture made once
    // (KeyedPicture); its path ("" when it cannot be made). The world map's monster areas.
    std::function<std::string(const std::string& source, const std::vector<std::uint32_t>& keys, std::uint32_t colour)> keyedPicture;
    // The Action window's icons (actionwnddata.txt; null: not loaded yet) and an icon used.
    const std::vector<assets::ActionRow>* actions = nullptr;
    // The world map's maps, floors and labels (null: not loaded yet).
    const assets::WorldMapData* worldMap = nullptr;
    // Where the player stands in a dungeon (region 0: not in one, or not known yet):
    // the minimap's folder (regioninfo.txt), and the .dof's floor and room names of the block of its floor
    // (text keys: "DH_A01_FLOOR02" is "2F", "QT_A01_F01_ROOM_01" "Audience Chamber").
    struct DungeonPlace {
        std::uint16_t region = 0;
        std::string folder, floor, room;
    };
    DungeonPlace dungeon;
    std::function<void(std::uint32_t action)> useAction;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> fortressRequest;
    std::function<const std::vector<assets::Ui2dtRecord>*(const std::string& path)> layout2dt;
    // The game guide's topics and events (null until the interface's data is in).
    std::function<const assets::GameGuideData*()> gameGuide;
    std::uint64_t eventGuides = 0;     // the event guide's pages met (proto::EventGuideState's mask)
    std::uint32_t eventGuidePage = 0;  // the page its side icon opens
    std::function<void(int kind, const std::string& value)> guideEvent; // an event of the guide met (the screen's GuideEvent)
    // The premium package's entry (its item) used, as its chat command (the composite window's buttons).
    std::function<void(std::uint32_t item)> premiumUse;
    const game::WorldView* world = nullptr; // the minimap's dots (null: none)
    Hud* hud = nullptr;
    double time = 0; // the clock the state's times are on (seconds)
    // The interface's text table (UIIT_... keys, zone names); the key itself when it has none.
    std::function<std::string(const std::string& key)> text;
    std::function<bool(const std::string& key)> hasText;
    // The area a region is in (regioncode.txt), for the area banner; empty when none.
    std::function<std::string(std::uint16_t region)> regionKey;
    // Icons (paths under the archive's root) and names; empty when unknown.
    std::function<std::string(const game::QuickSlot& slot)> slotIcon;
    // An action shortcut's tooltip (what its next use does: sit or stand, walk or run, free PvP on or off).
    std::function<std::string(const game::QuickSlot& slot)> slotText;
    // A sound file (a prim\snd\ path) played at the player (a buff's end: buf_disappear.wav).
    std::function<void(const std::string& path)> playAtPlayer;
    // A quick slot's wait (its clock, the ready flash after it, the seconds left; all 0 / -1 when none).
    struct Cooling {
        float cooling = 0.0f, charge = 0.0f;
        double seconds = -1.0;
    };
    // One place for every wait shown: an item's (its refId) and a skill's (its skilldata id); a
    // quick slot's comes from them (the underbar, the extra bar, the inventory, the Skills page).
    std::function<Cooling(std::uint32_t refId)> itemCooling;
    std::function<Cooling(std::uint32_t skill)> skillCooling;
    std::function<Cooling(const game::QuickSlot& slot)> slotCooling;
    std::function<std::string(const game::ActiveEffect& effect)> effectIcon;
    std::function<std::string(const game::ActiveEffect& effect)> effectName;
    // abusefilter.txt's words: the chat refuses a line with one before sending it (null: none).
    const game::NameRules* words = nullptr;
    // itemdata and magicoption.txt: the inventory's icons and tooltips (null: not loaded yet).
    const assets::ItemTable* items = nullptr;
    const assets::CharacterTable* characters = nullptr; // characterdata (a fortress structure's name, icon, level)
    const std::vector<assets::MagicOptionRow>* magicOptions = nullptr;
    const game::SpiritTable* spirits = nullptr; // the Devil's Spirit's options by plus (its tooltip)
    // refmagicoptgroup.txt: the socket stones' skills by level (a stone's tooltip, the Socket page; null: not loaded yet).
    const std::vector<assets::MagicOptGroupRow>* magicOptGroups = nullptr;
    // magicoptionassign.txt: the avatar blessing's option lists (null: not loaded yet).
    const std::vector<assets::MagicOptionAssignRow>* magicOptionAssign = nullptr;
    // A window's pick mode wants this mouse cursor (the original's cursor resource id; 0: the screen's own): the Socket
    // page's making (174) and removing (175),
    int cursor = 0;
    const assets::SkillTable* skills = nullptr; // a scroll's skill (the tooltip's effect line), the Skills page
    const std::vector<std::uint32_t>* masteryCosts = nullptr; // leveldata col 2: a mastery's next level's skill points
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>* levelGold = nullptr; // levelgold.txt: GoldMin, GoldMax by level
    // leveldata cols 6..8 (_RefLevel.JobExp_Trader / _Robber / _Hunter): a job level's experience by level (-1 past the
    // table; the job windows' gauges).
    const std::vector<std::array<std::int64_t, 3>>* jobExp = nullptr;
    // The leagues' rankings as last told, by type x 2 + kind (asked once a session, emptied
    // only by a new answer; an empty one is asked again).
    std::map<int, game::proto::JobRanking> jobRanks;
    // The gates and where they lead (the NPC window's menus; null: not loaded yet), and what NPCs
    // and gates say (their greeting's text key by codename; empty: none).
    const assets::TeleportTable* teleports = nullptr;
    // The merchants' goods (the NPC window's shop rows, the store); null: not loaded yet.
    const assets::ShopTable* shops = nullptr;
    // A merchant's store is open: items' tooltips end with what the merchant pays (its "Price").
    bool shopPrices = false;
    // The fortress tax at the NPC or gate last talked to (null: none told): purchases
    // show with it added, specialty sales with it taken off (TaxedBuy, TaxedSale).
    const game::proto::NpcTax* npcTax = nullptr;
    // The NPC last talked to's safe trade tail (a trader league member at a specialty NPC).
    const game::proto::SafeTradeTalk* npcSafe = nullptr;
    std::uint64_t TaxedBuy(std::uint64_t price) const {
        const std::int64_t tax = npcTax != nullptr && npcTax->fortress != 0 ? game::FortressTax(price, npcTax->rate, npcTax->exempt) : 0;
        return tax >= 0 ? price + std::uint64_t(tax) : price - std::min<std::uint64_t>(price, std::uint64_t(-tax));
    }
    std::uint64_t TaxedSale(std::uint64_t price) const {
        const std::int64_t tax = npcTax != nullptr && npcTax->fortress != 0 ? game::FortressTax(price, npcTax->rate, npcTax->exempt) : 0;
        return tax >= 0 ? price - std::min<std::uint64_t>(price, std::uint64_t(tax)) : price + std::uint64_t(-tax);
    }
    std::function<std::string(const std::string& codename)> npcGreeting, npcTalk;
    // The player's options (the Option window edits a draft of them); optionsChanged after the
    // window kept its draft (the game saves and applies them); audioPreview while its bars move.
    core::Options* options = nullptr;
    std::function<void()> optionsChanged;
    std::function<void(const core::AudioOptions&)> audioPreview;
    // The auto potion window's Confirm: the settings to keep (the game
    // sends them; the server answers with what it kept). Null: nobody keeps them (kept in the state).
    std::function<void(const game::AutoPotion&)> autoPotionSave;
    // What the server applies to the player's view and its own most (the Online page's sliders;
    // null until the server answered).
    const game::proto::ViewLimitsAnswer* view = nullptr;
    // Who wears the items (the tooltip's requirement lines in red when not met).
    game::Wearer wearer;
    // The equipment window's 3D character, as the window last drew it: the screen opens a view of
    // the player's character in that rect (pixels) turned by `yaw`.
    struct CharacterView {
        bool shown = false;
        ui::Rect rect; // pixels
        float yaw = 0;
        // The skin change window's chosen look (HudServices.cpp; 0: the character's own): its model and shape byte.
        std::uint32_t model = 0;
        std::uint8_t shape = 0;
    } characterView;
    // The item mall's services (HudServices.cpp): their requests; the last NPC menu's
    // services and NPC (the store's Repair rows); a pick mode (1 the store's Repair: the next item clicked is mended at
    // `servicePickNpc`; 2 the gender switch tool in `servicePickTool`: the next armour clicked); the withdrawal window's
    // potion's place (-1 none); the magic cubes' items by the cube's bag place; the creation's figures (the skin change).
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> serviceRequest;
    std::uint32_t npcServices = 0, npcServicesOf = 0;
    int servicePick = 0;
    std::uint32_t servicePickNpc = 0;
    int servicePickTool = -1;
    int withdrawPotion = -1;
    std::map<std::uint8_t, std::vector<game::proto::ItemRecord>> cubes;
    const game::CreationRules* creation = nullptr;
    // Capture the Flag and Event So-Ok (55; HudFlagWorld.cpp): the client's view (the timer, the teams, So-Ok's
    // desk; null: none), the requests (EventProtocol.h, encoded; null: offline) and a line of the event (the green
    // banner and the system messages, or the system line alone).
    game::FlagView* flag = nullptr;
    std::function<void(game::proto::MessageId id, std::vector<std::byte> payload)> eventRequest;
    std::function<void(const game::FlagView::Line& line)> flagLine;
    // The Battle Arena (HudArena.cpp): the client's view (the state, the scores, the result, the
    // timer, the manager's desk; null: none); its requests go by eventRequest; a line of it (the system messages, or the
    // big notice).
    game::ArenaView* arena = nullptr;
    std::function<void(const game::ArenaView::Line& line)> arenaLine;

    std::string Text(const std::string& key) const { return text ? text(key) : key; }
};

void RegisterHudWindows(ui::Interface& ui, HudContext& context);

// The COS manager (HudCos.cpp): status icons and the command bar. Its commands, as the Pet hotkeys number them
// (HudRequest::PetCommand).
// 100 is PgUp's: a pet's Unsummon, a ride's or a transport's Terminate.
constexpr int kCosInfo = 0, kCosRide = 1, kCosAttack = 2, kCosFollow = 3, kCosStance = 6, kCosTerminate = 100;
void RegisterCosWindows(ui::Interface& ui, HudContext& context);
// The place in the picked pet's bag a slot of the COS window stands for (-1: none): the inventory window takes drops
// from it (a CosBagMove).
int CosBagPlaceOf(const ui::Widget* slot);

// A text key's text with printf-style numbers put in ("Lv %d"); the numbers come formatted.
std::string FormatText(const std::string& format, std::initializer_list<std::string> args);
// The guild window (1.188's CNIFGuildWnd from res_ui\guild.2dt; HudGuild.cpp) on the community key,
// a guild refusal's system line (empty: the client prints none) and the window's page (0 the guild,
// 1 the union).
void RegisterGuildWindows(ui::Interface& ui, HudContext& context);
// The community pages (HudCommunity.cpp): the guild window's Community button's
// pages; `tab` 0 Friend, 1 Block, 2 Msg Box.
void RegisterCommunityWindows(ui::Interface& ui, HudContext& context);
// The Academy page (HudAcademy.cpp): the main window's L page over the player's
// game::AcademyView; its members' portraits' keys (frame/Portraits.h); a news or refusal line worded.
void RegisterAcademyWindows(ui::Interface& ui, HudContext& context);
std::uint32_t AcademyPortraitKey(std::uint32_t character);
// The honour ranking's window (154) opened over the last ranking the server sent.
void OpenAcademyRanking(HudContext& context);
std::string AcademyLineText(const std::function<std::string(const std::string&)>& text, const game::AcademyView::Line& line);
ui::Widget* AddCommunityPanel(ui::Interface& ui, HudContext& context, ui::Widget& window);
void ShowCommunityTab(ui::Widget* window, int tab);
bool CloseCommunityLetter(ui::Widget* window); // Escape: a letter being read or written first
void CommunityReadLetter(HudContext& context, int index); // the test options': a letter's Read pressed
void ShowCommunityWriter(ui::Widget* window, int many);    // the gallery's: 0 a letter's pane, 1 the guild's, 2 the union's
// The window on its Community side (the underbar's button: shows it, or hides it if it is there).
void ToggleCommunityPage(HudContext& context, int tab = -1);
// The window shown on its guild side or its Community side (the underbar menu's Guild and Community rows).
void ShowGuildSide(HudContext& context, bool community);
// A chat line's command that is the community's (true: taken).
bool CommunityChatCommand(HudContext& context, const std::string& line);
std::string CommunityLineText(const HudContext& context, const game::CommunityView::Line& line);
std::string GuildErrorText(const HudContext& context, std::uint16_t error, std::uint32_t value);
void ShowGuildPage(ui::Widget* window, int page);
// The guild window's Grant name box: a title for a member, confirmed, sent (HudGuild.cpp).
void AskGuildTitle(HudContext& c, std::uint32_t character, const std::string& name);
// A guild NPC's rows by the player's guild (create without one; the master promote,
// disband and transfer the leadership; a member leave, call the election and, while one runs, vote),
// each a choice number for GuildNpcChoice; and that choice done: the
// name asked (an input box, then the name and the cost confirmed) or the action confirmed, then the
// request sent with the NPC.
struct GuildNpcRowInfo {
    std::string text;
    int choice = 0;
    bool red = false; // what the guild's level does not allow yet
};
std::vector<GuildNpcRowInfo> GuildNpcRows(const HudContext& context);
// The guild NPC's mercenary rows (a shard that runs them, GuildInfo::mercenaries): "Grant additional ability to mercenary"
// opens the NPC window's soldier page (NpcWindow::ShowSoldiers); its rows are kGuildRowSoldierAbility + the ability's bit
// (0: release them all), done by GuildNpcChoice.
inline constexpr int kGuildRowSoldier = 11;
inline constexpr int kGuildRowSoldierAbility = 0x100;
// A league keeper's rows (HudJobs.cpp): by the NPC's league (proto::kNpcJob*), each a
// JobNpcRow; and that row done (a confirmation, the alias window, a request with the NPC).
enum class JobNpcRow : std::uint8_t { Join, Leave, AliasCreate, AliasModify, ActivityRank, DonationRank, ContributionRank, Outcome, Previous, TradeBoard,
                                       OutcomeCollect };
struct JobNpcRowInfo {
    std::string text;
    JobNpcRow row = JobNpcRow::Join;
};
std::vector<JobNpcRowInfo> JobNpcRows(const HudContext& context, std::uint32_t services);
void JobNpcChoice(ui::Interface& ui, HudContext& context, JobNpcRow row, std::uint32_t npc, std::uint32_t services);
// A league's grade name (UIIT_STT_CLASS_[EU_]<job>_<level>, the European words for race 1) and a job's experience line
// ("%d%% (%d)" of the level's requirement, "100%" past the table) with its gauge's fill.
std::string JobClassName(const HudContext& context, int type, int level, int race);
std::string JobExpLine(const HudContext& context, int type, int level, std::uint64_t exp, float& fill);
// The server's answers: a refusal's or a success's system line, the alias window's check; the previous information's
// window.
void JobAnswered(HudContext& context, const game::proto::JobAnswer& answer);
void ShowPreviousJobs(HudContext& context, const game::proto::JobPrevious& previous);
// The test options' way: a name typed into the open alias window and checked; its OK pressed once checked.
bool TypeJobAlias(HudContext& context, const std::string& name);
bool ConfirmJobAlias(HudContext& context);
void RegisterJobWindows(ui::Interface& ui, HudContext& context);
// A ranking told (JobRanking): kept, and its window opened (not refilled when one is open already).
void JobRankingArrived(HudContext& context, const game::proto::JobRanking& ranking);
// The open ranking window's page (its spinner) set to `page` (the script's rank-page); false when none is open.
bool PageJobRanking(HudContext& context, int page);
// The week's outcome answered (JobOutcomeAnswer): asked, the NPC's talk shows it; collected, the talk closes, the line.
void JobOutcomeAnswered(HudContext& context, const game::proto::JobOutcomeAnswer& answer);
// The trade's windows (HudTrade.cpp): the specialty deal window (a purchase into the transport,
// a sale from it), the price board; the answers' lines, the bargain hours' notices.
struct TradeDeal {
    bool buy = true;
    std::string name;
    std::uint64_t unitPrice = 0; // a purchase's per good; a sale's per good
    int count = 0, most = 0;
    int tab = 0, slot = 0; // a purchase's good
    int place = 0;         // a sale's place in the transport's bag
};
void OpenTradeDeal(HudContext& context, const TradeDeal& deal);
// A shop good dropped on the transport's bag (false: not a specialty good) / a good of its bag dropped on the shop.
bool BuyIntoCart(HudContext& context, const assets::ShopGood& good, int tab);
bool SellFromCart(HudContext& context, const game::ItemInstance& item, int place);
void TradeBuyStep(HudContext& context);
void TradeAnswered(HudContext& context, const game::proto::TradeAnswer& answer);
void TradeNotice(HudContext& context, const game::proto::TradeBargainNotice& notice);
const char* TradeCodeKey(std::uint16_t code, bool* banner);
void OpenTradeBoard(HudContext& context);
// The test options' way: the board's trade centre `centre` picked and refreshed; the deal window's OK (count -1: as typed).
bool RefreshTradeBoard(HudContext& context, int centre);
bool ConfirmTradeDeal(HudContext& context, int count, bool confirm = true);
void RegisterTradeWindows(ui::Interface& ui, HudContext& context);
// Magic POP (HudMagicPop.cpp): the machine's window (CIFGhaCha, 140) opened for the machine
// `npc`; whether the bag holds a coupon (Gori's row); the answers; the announce; a coupon's tooltip line; the script's
// steps (a prize chosen by its GachaID through the selection, a card registered, Play).
void RegisterMagicPopWindows(ui::Interface& ui, HudContext& context);
// The item mall (HudMall.cpp): F10 and the underbar's button ask "Enter Item Mall" (or close it);
// the window opened (its stock asked) and closed; the server's answers; a script's step (false: not a mall step, or
// no window); the preview's turn.
void RegisterMallWindows(ui::Interface& ui, HudContext& context);
void AskItemMall(HudContext& context);
void OpenItemMall(HudContext& context);
void CloseItemMall(HudContext& context);
void ItemMallAnswer(HudContext& context, const game::proto::MallEvent& event);
bool ItemMallStep(HudContext& context, const std::string& step);
float ItemMallPreviewYaw(const HudContext& context);
void OpenMagicPop(HudContext& context, std::uint32_t npc);
bool MagicPopHasCoupon(const HudContext& context);
void MagicPopPlayed(HudContext& context, const game::proto::PopPlayed& answer);
void MagicPopExchanged(HudContext& context, const game::proto::PopExchanged& answer);
void MagicPopAnnounced(HudContext& context, const game::proto::PopAnnounce& announce);
std::vector<std::string> MagicPopCouponLine(const HudContext& context, const assets::ItemRow& row, const game::ItemInstance& item);
bool MagicPopChoose(HudContext& context, std::uint32_t entry, bool pick = true); // pick false: the filter only
bool MagicPopCard(HudContext& context, int place);
bool MagicPopPlay(HudContext& context);
bool MagicPopRolling(HudContext& context);

// The item mall's services (HudServices.cpp).
void RegisterServiceWindows(ui::Interface& ui, HudContext& context);
// The premium package's uses window (interface 36), filled from the player's premium uses and shown.
void ShowCompositeItems(HudContext& context);
// An item used whose use is a window, a pick or a question first (the skin change scroll, the restoration potion, the
// gender switch tool, the repair hammer, a magic cube): true when handled here.
bool UseServiceItem(HudContext& context, int place);
// A pick mode (the store's Repair, the gender switch tool): the next item clicked goes to it.
bool ServicePicking(const HudContext& context);
bool ServicePick(HudContext& context, int place);
// The store's Repair all: its box with the price.
std::uint64_t RepairAllPrice(const HudContext& context);
void AskRepairAll(HudContext& context, std::uint32_t npc);
// A bag item dropped on a magic cube (true: the cube takes it).
bool CubeDrop(HudContext& context, int from, int to);
// A cube's tooltip lines: its places used and its items.
std::vector<std::string> CubeLines(const HudContext& context, int place);
// The withdrawal window holds this widget (the skill board in its withdrawal mode); a level of a learned skill or the
// mastery asked to be withdrawn.
bool InWithdrawalWindow(const ui::Widget* w);
// The removal box (CIFSkillRemovalBox) for that skill or mastery: its arrows choose how many levels go.
void AskWithdraw(HudContext& context, const assets::SkillRow* skill, std::uint32_t mastery);
// Scripts: the box's down arrow pressed `down` times, then (with `answer`) OK or Cancel; false when it is not shown.
bool WithdrawBoxShown(HudContext& context);
bool WithdrawBoxStep(HudContext& context, int down, bool answer, bool ok);
// The services' answers (their system lines) and the cubes' items.
void ServiceEvent(HudContext& context, const game::proto::MallEvent& event);
// The skin change window's choice (the script's steps): the sex, the figure's index, the height and volume; `confirm`
// presses OK. False when the window is not up.
bool ChooseSkin(HudContext& context, bool male, int figure, int height, int volume, bool confirm);
// The open confirmation box made taller by `extra` (its text and the buttons below; HudGuild.cpp).
void GrowConfirm(ui::Interface& ui, float extra);
void GrowConfirm(ConfirmBox& box, float extra);
// A simple box shaped as one of the original's kinds (HudParty.cpp): its size (centred again), its text hidden, the frame,
// background and close button following the edges, Yes and No placed; and a line added to it (`ddj` a picture behind it,
// a black box; `align` 0 left, 1 centre, 2 right).
void ShapeBox(ConfirmBox& box, float width, float height, float yesX, float noX, float buttonsY);
void PutLine(ui::Interface& ui, ConfirmBox& box, const std::string& text, float x, float y, float w, float h, const char* ddj, int align);
// The emblem item's boxes (the original's message box kinds 0x15 / 0x18 the question, 0x1A / 0x1B the success, one
// Confirm button): the guild's or the union's lines.
ConfirmBox* OpenCrestBox(HudContext& c, bool ofGuild, bool complete, std::function<void(bool yes)> answer);
void GuildNpcChoice(ui::Interface& ui, HudContext& context, int choice, std::uint32_t npc);
// The open name box answered with `name` (the test options); false when none is open.
bool AnswerGuildNameBox(ui::Interface& ui, const std::string& name);
// The guild master's windows (HudGuildMaster.cpp): the leader transfer
// window (58) and the election's (57), opened; the row of a member or candidate checked in the open
// one (and Confirm pressed): the test options' way.
void RegisterGuildMasterWindows(ui::Interface& ui, HudContext& context);
void RegisterGuildBoxWindows(ui::Interface& ui, HudContext& context); // HudGuildBoxes.cpp
void OpenGuildNoticeWrite(HudContext& context);             // the notice's pencil
void OpenAcademyNoticeWrite(HudContext& context);           // the Academy page's pencil (the same window, its academy state)
void OpenGuildDonate(HudContext& context);                  // the GP button
// The war's confirmation or agreement box with the terms (HudGuildWar.cpp); the test options' answer.
void AskWarTerms(HudContext& context, const char* layout, const char* titleKey, const std::string& first, const std::string& second, const game::proto::GuildWarTerms& terms, std::function<void(bool)> answer);
bool AnswerWarTerms(HudContext& context, bool yes);
void OpenGuildPosition(HudContext& context, std::uint32_t character); // Position allocating, for that member
bool PickGuildPosition(HudContext& context, std::uint8_t position, bool confirm); // the test options': a box chosen
void OpenGuildGpHistory(HudContext& context, const game::proto::GuildGpHistory& history); // the server's answer
// A layout of the 1.188 client's res_ui\*.2dt built under `window` (its rects relative to their parents).
void BuildGuild2dt(ui::Interface& ui, const std::vector<assets::Ui2dtRecord>& records, ui::Widget& window, float ox, float oy);
bool FillGuildNotice(HudContext& context, const std::string& title, const std::string& body); // the test options': typed and confirmed
bool FillGuildDonate(HudContext& context, std::uint32_t amount);
void OpenGuildLevelUp(HudContext& context);                 // the NPC's "Promote the guild."
bool PressGuildLevelUp(HudContext& context, bool ok);       // the test options' way: its OK or Cancel
void OpenGuildMasterTransfer(HudContext& context);
void OpenGuildElection(HudContext& context);
bool PickGuildMasterRow(HudContext& context, const std::string& name, bool confirm);
// A line typed in the guild's input box (the prompt across its top, at most `limit` bytes).
void AskGuildText(HudContext& c, const std::string& prompt, std::size_t limit, std::function<void(std::string)> answer,
                  const std::string& initial = {});
// The fortress war (HudFortress.cpp): the official's window (303) opened,
// a fortress's row's button pressed (its question asked; the test options' way), and a war's line as
// the 1.188 client words it.
void RegisterFortressWindows(ui::Interface& ui, HudContext& context);
void RegisterActionWindow(ui::Interface& ui, HudContext& context); // CIFAction: the main window's Actions page (HudActions.cpp)
// The world map (M; HudWorldMap.cpp): 1.188's CNIFWorldMap in the CIFWorldMap window's place.
void RegisterWorldMapWindow(ui::Interface& ui, HudContext& context);
void ShowWorldMap(HudContext& context, int map); // the window open on that map (0 the world, 2001.. a dungeon floor)
// The world map's guide shown (the window opened first) on tab `tab` (0 the places, 1 the monsters), its heading `heading`
// opened and that heading's entry `entry` picked (-1: none): the gallery's and the scripts' way.
void WorldMapGuideStep(HudContext& context, int tab, int heading, int entry);
// The quest list's map button: the world map on the quest's NPC, its monsters' tiles tinted, its places' quest icons.
void WorldMapQuest(HudContext& context, std::uint32_t quest);
// The world map's monster box (31) ticked or not: the monsters 0..6 levels over the player's marked.
void WorldMapMonsterBox(HudContext& context, bool on);
// `source` (a game .ddj) read, its pixels whose ARGB is one of `keys` drawn in `colour` and the rest clear, written once
// into `made` as a 32-bit DDS; its path ("" when the picture cannot be read).
std::string KeyedPicture(const assets::FileSystem& files, assets::MemorySource& made, const std::string& source,
                         const std::vector<std::uint32_t>& keys, std::uint32_t colour);
// The world map in the reverse return scroll's choosing mode: `chosen` hears the
// picked map point's ID (_RefOptionalTeleport's); closing the map hears nothing. `listMarker` (the gallery's) lists
// that marker's destinations at once.
void ChooseReturnPoint(HudContext& context, std::function<void(std::uint32_t)> chosen, int listMarker = -1);
// The battle aide's structure map (CIFFortressMap, 301) and its upgrade-type window (CIFCheckConfirmWnd, 101): HudFortressMap.cpp.
void RegisterFortressMapWindows(ui::Interface& ui, HudContext& context);
void OpenFortressMap(HudContext& context);
// icon\ + a fortress structure's icon: whole, _r damaged, _d destroyed (HudFortressMap.cpp).
std::string FortressStructureIcon(const HudContext& context, const game::proto::FortressStructureRecord& record);
void OpenFortressApply(HudContext& context);
bool PressFortressApply(HudContext& context, std::uint32_t fortress);
// The fortress manager's tax management (CIFTaxManagement, 300; HudFortress.cpp): opened
// with the server's FortressTaxInfo; the test options' way: "rate" (the slider at `value`, then Change's box) or "levy"
// (Levying's box), and the box answered (`amount` >= 0: the levy's number typed first).
void OpenTaxManagement(HudContext& context);
bool PressTaxManagement(HudContext& context, const std::string& action, std::int64_t value);
bool AnswerTaxBox(HudContext& context, bool yes, std::int64_t amount = -1);
std::string FortressNoticeText(const HudContext& context, const game::FortressView::Line& line);
// The fortress manager's and the forges' windows (HudFortressWork.cpp): "Jobs for Fortress
// War" (CIFFortressBusiness, 302) with the schedule's answer; the guild's introduction (CNIFGuildIntroWnd, 185) with its
// answer, the NPC it goes back to, and typed (the test options); the smith's or the trainer's window
// (CIFFortressMakeItemWnd, 304): opened by the query's answer, refilled by the later ones; its buttons pressed (the test
// options): "make" (the item's row, its box answered with `count`), "cancel" (its box answered), "complete", "tab" (item:
// the tab).
void RegisterFortressWorkWindows(ui::Interface& ui, HudContext& context);
void OpenFortressBusiness(HudContext& context, const game::proto::FortressSchedule& schedule);
void OpenGuildIntroduction(HudContext& context, const game::proto::FortressIntroduction& intro, std::uint32_t npc);
bool TypeGuildIntroduction(HudContext& context, const std::string& text, bool confirm);
void OpenFortressMakeItem(HudContext& context, const game::proto::FortressForge& work, bool open);
bool PressFortressMakeItem(HudContext& context, const std::string& action, std::uint32_t item, int count);
// Guild wars (HudGuildWar.cpp): the request window (59) opened or filled
// and confirmed (the test options); the terms' lines; the other master's agreement box; the reward's box.
void RegisterGuildWarWindows(ui::Interface& ui, HudContext& context);
void OpenGuildWarRequest(HudContext& context);
bool FillGuildWarRequest(HudContext& context, const std::string& name, const game::proto::GuildWarTerms& terms, bool confirm);
std::vector<std::string> GuildWarTermsLines(const HudContext& context, const game::proto::GuildWarTerms& terms);
void AskWarAnswer(HudContext& context, const game::proto::GuildWarProposal& proposal, std::function<void(bool)> answer);
void AskWarReward(HudContext& context, std::uint64_t gold);
// A chat line's text as shown: without its line feeds and "\n" escapes.
std::string ChatText(std::string_view text);

// --- The player (section 1) ---------------------------------------------------------------------

// CIFPlayerMiniInfo (11): the name, level, HP and MP with their gauges, the berserk orbs, the race
// mark, the stat panel; a press on it selects the player.
class PlayerBar : public ui::Static {
public:
    PlayerBar(const assets::UiLayoutNode& node, HudContext& context) : Static(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Mouse(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;

private:
    void ShowPanel(bool shown);
    void Warning(ui::Frame& frame, int id, float ratio);
    float burnClock_ = 0.0f; // the berserk orbs' flames
    HudContext& context_;
    bool panel_ = false;
    double phase_ = 0; // the low HP / MP glow's sheet (ms)
};

// CIFMagicStateBoard (22): the buffs in rows of eight, the debuffs and states under them, each an
// icon over a time gauge; the tooltip names it, a right click asks to cancel a buff.
class BuffBoard : public ui::Widget {
public:
    BuffBoard(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;
    void Mouse(ui::Frame& frame) override;

private:
    // Where each effect's slot is (board pixels), laid out as the original.
    void Layout();
    HudContext& context_;
    std::vector<ui::Vec2> places_;
    int hovered_ = -1;
    // A skill buff's end (the original's): icon\buf_effect.ddj's ten frames over where its slot was, one
    // each 100 ms, and buf_disappear.wav at the player. The last frame's skill buffs (token -> place) find the ended.
    struct Ending {
        ui::Vec2 place;
        double started = 0.0;
    };
    std::vector<Ending> endings_;
    std::unordered_map<std::uint32_t, ui::Vec2> lastBuffs_;
};

// The abnormal states by their bit (game::StateBit): the icon (icon\stateodd\s_<name>_icon.ddj) and
// the title key PARAM_<code>, which also names the state's effect list in
// skilleffect.txt; empty for a bit of none. A buff's or state's tooltip:
// its name, then the time left.
std::string StateIcon(std::uint32_t bit);
std::string StateTitle(std::uint32_t bit);
std::string EffectTooltip(const HudContext& context, const game::ActiveEffect& e);

// CIFBuffViewer (the target window's row, id 100; section 4): the target's states, eight icons a row
// 23 apart, under the target's look (K3: states only; its buffs when the server sends others').
class TargetBuffs : public ui::Widget {
public:
    TargetBuffs(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;

private:
    HudContext& context_;
    int hovered_ = -1;
};

// --- The underbar and quick slots (sections 2, 3) ------------------------------------------------

// CIFUnderBar (12): the experience cells and percentage, the skill experience gauge and points,
// the page of ten quick slots and the `M` slot, the menu buttons.
class UnderBar : public ui::Widget {
public:
    UnderBar(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The quick slot a control shows (-1: not a quick slot).
    static int SlotIndex(int controlId, int page);

private:
    HudContext& context_;
};

// CNIFUnderMenuBar (175, res_ui\nifundermenubar.2dt): 1.188's underbar, shown in place of CIFUnderBar (which 1.188
// still makes from ginterface.txt but no longer shows): the level and its experience
// (ten cells of a percent each within the current tenth), the skill experience gauge and points, the page of ten quick
// slots and the mouse slot, the Character / Inventory / Skill / Item Mall buttons, and the Menu button's popup of the
// other windows (the collection book among them).
class MenuBar : public ui::Static {
public:
    MenuBar(const assets::UiLayoutNode& node, HudContext& context) : Static(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The quick slot a control shows on a page (-1: none): 50 the mouse slot's (0), 40..49 the page's ten.
    static int SlotIndex(int controlId, int page);
    // A button of the bar or a row of its menu pressed; the menu closes.
    void Choose(int control);
    void ToggleMenu();
    bool MenuShown() const { return menu_ != nullptr && menu_->visible; }

private:
    void FitMenu(ui::Painter& paint);
    HudContext& context_;
    ui::Widget* menu_ = nullptr;
    bool menuFitted_ = false;
};

// CIFExtQuickSlot (133): ten more quick slots (41..50) on a bar of its own, by default two lines
// down the right edge, see-through.
class ExtraQuickSlots : public ui::Widget {
public:
    ExtraQuickSlots(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The options window's four (CIFExtQuickSlotOption: see-through 25, lock 26, fixing position 27, two lines 28).
    struct Settings {
        bool seeThrough, locked, fixed, twoLines;
    };
    Settings Current() const { return {seeThrough_, locked_, fixed_, twoLines_}; }
    void Apply(const Settings& s);
    // The options window opened beside the bar (on the screen's wider side), or closed when it is open.
    void ToggleOptions();

private:
    void Shape(ui::Interface& ui);
    void ShowOpen(); // the tiles, slots and end piece as open_ says, the on/off button's face to match
    void Save();     // into the player's options (the original's SRExtQSOption.dat)
    HudContext& context_;
    bool open_ = true, vertical_ = true, twoLines_ = true, seeThrough_ = true, locked_ = false, fixed_ = false, reshape_ = false;
};

// Dropping and letting go of quick slots, shared by the underbar and the extra bar: a slot dropped
// on another swaps with it, one let go anywhere else is cleared.
void LinkQuickSlotDrops(ui::Widget& window, HudContext& context);
// The extra quick bar's options window (CIFExtQuickSlotOption, HudUnderBar.cpp).
void RegisterExtraBarOptions(ui::Interface& ui, HudContext& context);

// --- The target (section 4) -----------------------------------------------------------------------

// CIFTargetWindow (16): the selected monster, NPC, pet or player, in the look of its kind, centred
// at the top.
// CIFMainPopup: the main window; the round buttons down its left go to its pages and say which.
class MainPopup : public ui::MainFrame {
public:
    MainPopup(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;

private:
    HudContext& context_;
};

class TargetWindow : public ui::Widget {
public:
    TargetWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;

private:
    HudContext& context_;
};

// --- The minimap and the area banner (section 5) ----------------------------------------------------

// CIFMinimap (10): the map around the player, north up, cut to a disc; the dots of who is around;
// the player's arrow; the area name and the coordinates; zoom and the world map's button.
class Minimap : public ui::Static {
public:
    Minimap(const assets::UiLayoutNode& node, HudContext& context) : Static(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The coordinates the original shows for a place (X:%3d / Y:%3d).
    static int CoordinateX(std::uint16_t region, float x) { return ((region & 0xFF) - 135) * 192 + int(x / 10.0f); }
    static int CoordinateY(std::uint16_t region, float z) { return ((region >> 8) - 92) * 192 + int(z / 10.0f); }

private:
    HudContext& context_;
    float scale_ = 160.0f, target_ = 160.0f; // pixels a region (1,920 units) is drawn
};

// The performance panel (ours, no counterpart in the original; hud::kPerformance): a small box at the top right, left
// of the minimap. Collapsed it is one line, the frames a second and the latency; a click on its header expands it to the
// frame times and the connection's traffic, another collapses it. Drawn in the game's own font, no ImGui.
class PerformancePanel : public ui::Widget {
public:
    PerformancePanel(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {
        interactive = true;
    }
    void Created(ui::Interface& ui) override { ui_ = &ui; }
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;
    void Mouse(ui::Frame& frame) override;
    bool expanded = false;
    // Its size for its state (the lines it shows); the bug report's button (with HudContext::bugReport) at its left.
    static constexpr float kWidth = 184.0f, kHeader = 16.0f, kLine = 14.0f;
    static constexpr float kButton = 18.0f, kButtonGap = 4.0f;
    static constexpr int kExpandedLines = 6;
    // Its right edge from the screen's right: left of the minimap, and of the COS's status icons when any are out
    // (HudCos.cpp: the first 172 from the right, 48 a further one, the picked one's outline 11 out).
    static float RightGap(std::size_t coses);

private:
    bool HasButton() const { return static_cast<bool>(context_.bugReport); }
    ui::Rect ButtonRect() const; // on the screen
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
};

#ifdef SR_BUG_REPORTS
// The bug report window (ours; HudBugReport.cpp, hud::kBugReport): what kind of bug, a subject and the description, and the
// screenshot (with or without the interface; the window itself never in it); Send gives it to HudContext::bugReport.
// Drawn as the performance panel is (a dark box, the game's gold edge, the game's font), the controls the game's own.
class BugReportWindow : public ui::Widget {
public:
    BugReportWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) { interactive = true; }
    void Created(ui::Interface& ui) override;
    void Draw(ui::Frame& frame) override;
    void Mouse(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    static constexpr float kWidth = 420.0f, kHeight = 372.0f;

private:
    void Send();
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    int category_ = 0;
    std::string problem_;   // why Send was refused (shown over the buttons)
    double problemAt_ = -1; // when (shown a few seconds)
    double now_ = 0;
};
// Opens the report window (centred; a second press closes it).
void ToggleBugReport(ui::Interface& ui, HudContext& context);
#endif

// CIFRegionView (83): the area's name, description and monster levels across the top when the
// player comes into another area: 0.5 s in, 3 s, 0.5 s out.
class AreaBanner : public ui::Widget {
public:
    AreaBanner(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;

private:
    HudContext& context_;
    std::string key_; // the area shown last
    double shownAt_ = -100;
    std::uint16_t region_ = 0;
};

// --- Notices (section 6) ----------------------------------------------------------------------

// CIFSystemMessage (68): the game's lines over the bottom right, drop-shadowed; its frame (the
// background, the filter and size buttons, the scroll bar) shows only while the mouse is over it.
class SystemMessages : public ui::Widget {
public:
    SystemMessages(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // 1..6: 3 lines a level; placed so its bottom stays 90 above the screen's.
    void SetLevel(int level);
    int level() const { return level_; }
    // The filter board's boxes: Acquisition, Combat, Status, Party, Game.
    std::array<bool, 5> filter{true, false, true, true, true};
    bool autohide = true; // the option "System UI Autohide"
    bool Passes(game::GameMessage::Kind kind) const;

private:
    void ShowFrame(bool shown);
    HudContext& context_;
    ui::TextBox* box_ = nullptr;
    ui::ScrollBar* bar_ = nullptr;
    ui::Widget* board_ = nullptr;
    int level_ = 2;
    bool boardOpen_ = false, frameShown_ = true;
    std::uint64_t seen_ = 0;
    ui::Vec2 placedFor_{-1, -1};
};

// CIFNotify (20, 35, 42): a banner across the top with the latest notice, warning or quest text;
// bold white on a band of its colour, 5 seconds, then 2 fading.
class NoticeBanner : public ui::Widget {
public:
    NoticeBanner(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;

private:
    HudContext& context_;
    game::Banner::Kind kind_ = game::Banner::Kind::Notice;
    double shownTime_ = -1;
    std::string art_; // the frame's prefix: com_notice_ / com_warning_ / com_quest_
    ui::Color colour_ = 0;
    float top_ = 100;
};

// --- The chat -----------------------------------------------------------------

// CIFChatViewer (1): the chat over the underbar at the left edge: the lines of four tabs (All,
// Party, Guild, Union) over the input line, in seven sizes (0: the input line only; 2 at the
// start); the whisper list above it. The player's lines go to the game as requests; the lines the
// state receives come in with their colours and tags.
// The chat window's command list (CIFChatModeViewer).
std::unique_ptr<ui::Widget> MakeChatModeViewer(const assets::UiLayoutNode& node, HudContext& context);

class ChatWindow : public ui::Widget {
public:
    ChatWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;

    // The original's chat types, as the window sorts its lines (2.2: every line in All; a party,
    // guild or union line also in its tab; whispers, game masters' and notices in every tab).
    enum : int { kAll = 1, kWhisper = 2, kGm = 3, kParty = 4, kGuild = 5, kGlobal = 6, kUnion = 11, kAcademy = 16, kSystem = 0xFF };
    // A line as the window shows it: its type, colour and text ("Name(FROM):text").
    struct Shown {
        int type = kAll;
        ui::Color colour = 0;
        std::string text;
    };
    static Shown Format(const game::ChatEntry& entry, const HudContext& context);

    // 0..6: 4 rows a level, standing on the underbar; 0 leaves the input line only.
    void SetLevel(int level);
    int level() const { return level_; }
    // 0 All, 1 Party, 2 Guild, 3 Union: its lines and its lamp; an empty input takes its prefix.
    void SelectTab(int tab);
    int tab() const { return tab_; }
    // Enter: the keyboard to the input line.
    void FocusInput();
    // "$name " in the input line, a whisper ready (with the keyboard when `focus`).
    void Address(const std::string& name, bool focus);
    // The reply key: the last whisper partner addressed.
    void Reply();
    // A line into the tabs by its type (`allOnly`: the All tab only, as the filter's refusal).
    void AddLine(int type, ui::Color colour, const std::string& text, bool allOnly = false);
    // The input line's text as typed and sent with Enter (tests): what it asks of the game and
    // what it leaves in the input line.
    void Submit(const std::string& line);

    ui::Edit* input() const { return input_; }
    ui::TextBox* box(int tab) const { return boxes_[std::size_t(tab)]; }
    const std::deque<std::string>& partners() const { return partners_; }
    const std::string& replyTarget() const { return replyTarget_; }
    bool restricted() const { return restricted_; }
    bool stripShown() const { return !stripHidden_; }

    static constexpr std::size_t kPartners = 30;

private:
    void ShowStrip(bool shown);
    void ShowWhispers(bool shown);
    void ShowModes(bool shown);
    void PickMode(int row);
    void Restrict(int seconds);
    void TakeEntries();
    void NotePartner(const std::string& name);
    bool Command(const std::string& line); // false: not the client's own (sent: a game master's command)
    void Release();
    void Post(game::GameMessage::Kind kind, const std::string& text);
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::array<ui::TextBox*, 4> boxes_{};
    std::array<ui::Widget*, 4> lamps_{};
    std::array<ui::SelectableArea*, 4> tabs_{};
    ui::Edit* input_ = nullptr;
    ui::ScrollBar* bar_ = nullptr;
    ui::Widget* whispers_ = nullptr; // CIFWhisperList (51)
    ui::Widget* modes_ = nullptr;    // CIFChatModeViewer (42): the command list of the button 41
    ui::TextBox* whisperBox_ = nullptr;
    ui::Static* penalty_ = nullptr; // "Chat Restricted: N seconds"
    int level_ = 2, tab_ = 0;
    bool stripHidden_ = false, whispersOpen_ = false, welcomed_ = false;
    std::string replyTarget_;
    std::deque<std::string> partners_;
    std::uint64_t seen_ = 0;
    std::deque<double> sends_; // when the last lines to the people around were sent (the flood rule)
    bool restricted_ = false;
    int restrictLeft_ = 0;
    double restrictTick_ = 0;
    ui::Vec2 placedFor_{-1, -1};
};

// CIFWholeChat (56): "Chat to all": a line to every player on the server, paid with a Global
// chatting item; the count of those left, Use and Exit (Enter and Escape).
class GlobalChatWindow : public ui::FrameBorder {
public:
    GlobalChatWindow(const assets::UiLayoutNode& node, HudContext& context) : FrameBorder(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    int count = 0; // the items left of the one it was opened with (the inventory says)
    // The item used: its bag place (and what it is, so another put there does not count).
    void Open(std::uint8_t place, std::uint32_t refId) {
        place_ = place;
        refId_ = refId;
    }
    void Say(const std::string& text); // the test options': typed and sent

private:
    void Recount();
    void Use();
    std::uint8_t place_ = 0;
    std::uint32_t refId_ = 0;
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
};

// CIFDelayGaugeBoard (40): the casting gauge above the underbar, the original's five kinds (proto::CastingKind): a
// CIFDelayInfo (192 x 36, com_casting_window.ddj) at the board's bottom, its line (the item's name; a search's quest; the
// guild recall's), its bar filling left to right over its time (the collection's art for a search, the return's for the
// rest; full until a teleport), a cancel button.
class DelayGaugeBoard : public ui::Widget {
public:
    DelayGaugeBoard(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;

private:
    HudContext& context_;
    ui::Widget* gauge_ = nullptr;
    double shownStart_ = -1.0; // the gauge whose line and art are set (its start)
};

// CEquipStateImg (ginterface 157, 68 x 96, at (W - 200, 74)): the worn pieces near breaking, a paper doll of eight
// pictures over the same place, one per place 0..7 (head, chest, shoulders, hands, legs, feet, weapon, shield): its
// part yellow at durability 1..6, red at 0 (interface\ifcommon\com_re_*_y / _r); accessories never; empty while all are
// sound. 1.188 shows it in place of the older durability warning (34), which stays hidden.
class EquipStateImage : public ui::Widget {
public:
    EquipStateImage(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;

private:
    HudContext& context_;
    std::array<ui::Static*, 8> parts_{};
};

// --- NPCs and gates (C7i) ---------------------------------------

// CIF_NPCWindow (30): the talk window (if_npcwindow.txt's CIF_NPCTalk: a text box over a dark tile,
// with its scroll bar). The title is the NPC's name; the box holds its greeting and the numbered
// choices, beige and orange under the mouse; a click on one does it. For now the gates' menu: the
// return point, the places it leads to with their fees.
class NpcWindow : public ui::MainFrame {
public:
    NpcWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    bool OnNotice(const ui::Notice& notice) override;
    // A gate's menu by the services the server offers (proto::kGate*), and the destinations it opened at run time (the
    // snake gate's floor 5: proto::GateMenu::opened).
    void ShowGate(std::uint32_t gate, std::uint32_t services, std::vector<std::uint32_t> opened = {});
    // An NPC's: its name, its greeting (npcchat.txt by codename), what it offers (proto::kNpc*).
    void ShowNpc(std::uint32_t entity, const std::string& codename, const std::string& nameKey, std::uint32_t services);
    // The guild's mercenaries' page (the 1.188 client's: the abilities held in a line, then the four abilities' rows and
    // the release of them all, each numbered).
    void ShowSoldiers();
    std::uint32_t gate() const { return gate_; }
    // The choice numbered `number` (1..), as a click on it; false when there is none (scripts).
    bool Choose(int number);
    // The choice whose text is `text` (without its number), as a click on it.
    bool ChooseText(const std::string& text);
    // A league keeper's row of that kind, when it has one (the test options).
    bool ChooseJob(JobNpcRow row);
    // What the NPC says about a quest (the server's QuestSays): its talk, then the player's answers.
    void ShowQuestSays(const game::proto::QuestSays& says);
    // "Safe Trade Attempts [count]/[limit]" at the top of the talk: a trader league member's, the option 0.
    void AddSafeTradeLine();
    // The week's outcome in the talk: its text replaced by UIIT_STT_OUTCOME_WINDOW, the rows by the
    // collect row (when there is something) and the end.
    void ShowOutcome(const std::string& text, bool collect);
    // Event So-Ok's desk come after her menu: the menu again with the registration row by her state.
    void ShowEventDesk();
    // What she says in a coded quest's talk (EventSays): its text and its numbered rows.
    void ShowEventSays(const game::proto::EventSays& says);
    // The Battle Arena manager's desk come after its menu: the menu again with its row by the state.
    void ShowArenaDesk();
    std::uint32_t npc() const { return npc_; }

private:
    struct Choice {
        enum class Kind : std::uint8_t {
            End, RecallPoint, Destinations, Destination, Storage, Talk, Shop, Guild, Fortress,
            AcademyFound,   // a storage keeper's "Academy Establishment"
            AcademyRanking, // its "Honor ranking"
            OpenMarket,     // Juel's market (the original's row 0x3F)
            AvatarGrant,    // a smith's "Grant magic option on avatar" (row 0x2F): the avatar blessing's window
            TempleReputation, // "Temple area Point" (row 0x3C): the temples' reputation board asked
            PartySummon, // a party recall pillar's "Summon Party member" (the summon list asked for)
            Quest,       // a quest of the NPC's list (destination: the quest)
            QuestYes,    // the offer accepted
            QuestNo,     // the offer declined (the NPC's words for it, then the end)
            QuestHandIn, // what is due handed in, or the quest finished (with the reward's choice)
            QuestReward, // a reward item picked (destination: its index)
            QuestTier,   // an exchange's row picked (destination: its index; the premium's purification seeds)
            QuestNext,   // the talk's next page (or, past its last, the talk held meanwhile)
            Job,         // a league keeper's row (destination: JobNpcRow)
            MagicPop,    // the machine's "Participate in the game." (0) or Gori's exchange (1)
            FlagDesk,    // So-Ok's "Capture the Flag Match Registration" or its cancel
            EventQuest,  // one of So-Ok's coded quests (destination: the quest; EventTalk 0xFF)
            EventRow,    // a row of her talk (destination: quest << 8 | the row's index; EventTalk with it)
            EventExit,   // her talk's "End conversation." (destination: the quest; EventTalk 1, the window closed)
            ArenaDesk,   // the arena manager's row (destination: its state): the cancel, or the open mode's talk
            ArenaJoin,   // the mode's talk's "Arena Registration" (destination: the state): the guild or the rule window
        };
        Kind kind = Kind::End;
        std::uint32_t destination = 0; // a teleportdata id
    };
    void Begin(const std::string& title, const std::string& greetingKey);
    void AddTaxLine();
    void AddChoice(const std::string& text, Choice choice, bool red = false);
    void ShowDestinations();
    // The fortress manager's "Who will you employ?" rows and its blessings.
    void ShowHire();
    void ShowBlessings();
    void ShowDungeon();
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    ui::TextBox* box_ = nullptr;
    std::map<int, Choice> choices_; // by the box's paragraph
    std::uint32_t gate_ = 0;
    std::vector<std::uint32_t> opened_; // the gate's destinations opened at run time
    std::uint32_t npc_ = 0; // the NPC talked to (its entity)
    std::string npcCodename_, npcNameKey_;
    std::uint32_t services_ = 0; // what it offers (proto::kNpc*)
    void ShowTalk();
    // A quest's text in the player's language (a key of the text tables, or the words themselves).
    std::string QuestTextOf(const game::QuestText& t) const;
    void ShowRewardChoice();
    std::uint32_t quest_ = 0;              // the quest being talked about
    std::vector<std::uint8_t> picked_;     // the reward's items chosen so far
    // A talk of several pages (its moment's lines): the page shown, and what the server said meanwhile (a story's
    // "finished" right after its "accepted"), shown once the pages are read.
    void ShowQuestPage();
    game::proto::QuestSays paging_;
    std::vector<std::string> pages_;
    std::vector<std::string> pageRows_; // each page's row, the player's answer (empty: "Next", or the moment's words)
    std::size_t page_ = 0;
    std::deque<game::proto::QuestSays> held_;
};

// CIFMessageBox with ifmessagebox.txt's MsgBoxINIF and one button, Confirm (202), which closes it and sends nothing:
// the Subscription Card's box (kind 0x2A). The Yes (200)
// and No (201) buttons are hidden; the text is its first static (1; 301 in the box, the frame's drag strip being 1).
class InformBox : public ui::MainFrame {
public:
    using MainFrame::MainFrame;
    void Created(ui::Interface& ui) override;
    bool OnNotice(const ui::Notice& notice) override;
    void Say(const std::string& text);

private:
    ui::Interface* ui_ = nullptr;
};

// CIFMessageBox with ifmessagebox.txt's MsgBoxSimple: lines of text, Yes and No (the close button
// says No). Made by Hud::Confirm, gone once answered.
class ConfirmBox : public ui::MainFrame {
public:
    using MainFrame::MainFrame;
    void Created(ui::Interface& ui) override;
    bool OnNotice(const ui::Notice& notice) override;
    void Ask(const std::vector<std::string>& lines, std::function<void(bool yes)> answer);
    void Answer(bool yes); // as a click on Yes or No
    // One button only, in the middle, labelled `label` (the original's boxes that only tell: the lost connection's).
    void OkOnly(const std::string& label);
    // The name strip over the text (interface\messagebox\msgbox_skillname.ddj, 212 x 26, centred, its text `key`): the
    // original's boxes of a repair (the smith's Repair All, kind 0x0C; the battle aide's structures, 0x27..0x29). The
    // box grows by it.
    void Header(const std::string& key);

private:
    ui::Interface* ui_ = nullptr;
    std::function<void(bool)> answer_;
};

// The death window: CIFMessageBox kind 3, made in
// code, 400 x 210 in the message box's frame titled "Confirmation window": the picture
// msgbox_rebirth.ddj at 128,43, "the character is now paralyzed." across it at y 117, two buttons
// (msgbox_rebirth_button.ddj, 176 x 24) at 22,168 (id 200: "Resurrect at the specified point.",
// answer 1) and 202,168 (id 201: up to level 10 "Resurrect at the present point.", answer 2; above it
// "Rescued by other player", which only closes the box). Made by Hud::Rebirth.
class RebirthBox : public ui::MainFrame {
public:
    using MainFrame::MainFrame;
    void Created(ui::Interface& ui) override;
    bool OnNotice(const ui::Notice& notice) override;
    // `answer` gets 1 (the return point), 2 (where it lies) or 0 (closed: waiting to be raised).
    void Ask(int level, std::function<void(int kind)> answer);
    // In a fortress's war: its lines under the question, and no rising before `riseAt` (the box's clock; the count
    // itself is the warning banner's, PlayScreen::DeathWindow).
    void WarLines(const std::vector<std::string>& lines, double riseAt);
    void Update(ui::Frame& frame) override;

private:
    ui::Interface* ui_ = nullptr;
    int level_ = 1;
    double riseAt_ = 0.0, now_ = 0.0;
    std::function<void(int)> answer_;
};

// The reverse return scroll's box (1.188 message box kind 0x21): a
// CIFMessageBox of 300 x 204, "Confirmation window", the question, and four buttons (com_return_button.ddj, 252 x 24
// at x 25): the last recall point (1), the place of death (2), a point on the map (3), Cancel (0). None is greyed: the
// server refuses. Its answer closes it.
class ReverseReturnBox : public ui::MainFrame {
public:
    using MainFrame::MainFrame;
    void Created(ui::Interface& ui) override;
    bool OnNotice(const ui::Notice& notice) override;
    void Ask(std::function<void(int result)> answer) { answer_ = std::move(answer); }

private:
    ui::Interface* ui_ = nullptr;
    std::function<void(int)> answer_;
};

// CIFSkillPracticeBox (ginterface.txt 31): the skill's (or mastery's) icon,
// name and level, its mastery's name, its description, "Learn [%s]skill." / "reinforce [%s]skill upto
// level [%d].", the skill points it needs; Confirm sends (Enter too), Cancel and Escape close.
class SkillPracticeBox : public ui::MainFrame {
public:
    SkillPracticeBox(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    bool OnNotice(const ui::Notice& notice) override;
    struct Question {
        std::string icon, name, mastery, description, next;
        int level = 1;
        std::uint32_t skillPoints = 0;
        bool isMastery = false; // a mastery's level: its name in the box, no icon or level
    };
    void Ask(const Question& q, std::function<void()> confirmed);
    void Answer(bool yes);

private:
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::function<void()> confirmed_;
    std::uint32_t skillPoints_ = 0; // what the asked level costs
};

// CIFMessageBox with MsgBoxDivideCount: an item's icon and
// name, "Retained number" with its count, "Distributed number" with an edit starting at 1 (0 ..
// count - 1), Confirm / Cancel. Made by Hud::Divide, gone once answered.
class DivideBox : public ui::MainFrame {
public:
    using MainFrame::MainFrame;
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    void Ask(const std::string& icon, const std::string& name, int count, std::function<void(int)> answer);

private:
    ui::Interface* ui_ = nullptr;
    int count_ = 0;
    std::function<void(int)> answer_;
};

// --- The inventory and the equipment ------------------------------------

// The storage's gold box (MsgBoxStoreMoney: the coin, "Amount", the number, Confirm / Cancel), one
// way each: into the storage from the inventory's coin, out of it from the storage's. Made by
// Hud::StorageGold, gone once used.
class GoldBox : public ui::MainFrame {
public:
    GoldBox(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;

private:
    // A control of the section by its layout name (its ids 1 and 2 are the frame's drag strip and
    // close button too).
    ui::Widget* Named(std::string_view name) const;
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    ui::Edit* edit_ = nullptr;
    bool focused_ = false;
};

// CIFStoreForPackage (15, ifstore.txt): a merchant's store, one of its
// tab groups: up to 4 tabs (200..203, made in code), 30 goods a page (100..129) with the page
// spinner (14), the 5 sold items that can be bought back (50..54, oldest left). A good dragged onto
// the inventory opens the buy box; an item dropped on the store the sell box; a sold one dragged onto
// the inventory the buy-back box; Ctrl + click buys at once, sells once asked.
class StoreWindow : public ui::MainFrame {
public:
    StoreWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The store of `npc`'s shop, tab group `group`, titled `title`.
    void Open(std::uint32_t npc, const assets::NpcShop& shop, int group, const std::string& title);
    // The good a slot of this window shows (null: none), and the buy-back index one stands for (-1).
    static const assets::ShopGood* GoodOf(const ui::Widget* slot, int* tabIndex = nullptr);
    static int SoldOf(const ui::Widget* slot);
    // The item a good makes (its package's plus, white stats, durability; a stack of one).
    static game::ItemInstance ItemOf(const assets::ShopGood& good, const assets::ItemTable& items);

private:
    void SelectTab(int tab);
    void Refresh();
    HudContext& context_;
    const assets::ShopTabGroup* group_ = nullptr;
    std::uint32_t npc_ = 0;
    int tab_ = 0, page_ = 0;
    std::uint64_t seen_ = ~0ull;
};

// The store's boxes (C7i): buying (the item mall's ConfirmBuy with a price row), selling and buying
// back (ifmessagebox.txt's MsgBoxStoreConfirm). Made by Hud::Deal, gone once answered.
class DealBox : public ui::MainFrame {
public:
    DealBox(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    struct Deal {
        enum class Kind : std::uint8_t { Buy, Sell, Buyback, StallRegister, Deposit, StallBuy };
        Kind kind = Kind::Buy;
        std::string icon, name;
        std::uint64_t unitPrice = 0; // a buy's per piece; a sale's per piece; a buy-back's total
        int count = 1, most = 1;     // the starting count and its maximum
        int tab = 0, slot = 0;       // a buy's good
        int place = 0;               // a sale's inventory place
        int index = 0;               // a buy-back's entry
        int stallSlot = 0;           // a stall's register (`place` the bag's; 0 a price change) or buy: the stall's slot
        int storageMove = 0;         // a deposit's: its StorageMove request (`unitPrice` the whole fee)
    };
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    void Set(const Deal& deal);

private:
    ui::Widget* Named(std::string_view name) const;
    int Count() const;
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    Deal deal_;
    ui::Edit* count_ = nullptr;
    ui::Widget* price_ = nullptr;
};

// CIFStorageRoom (19, ifstorageroom.txt): the account's storage at a keeper (C7i). 30 places a page
// (6 x 5, ids 100..129), the page spinner (13) through size / 30 pages, the stored gold (10). Items
// are dragged between it and the inventory, or within it; the server answers (StorageMoved).
class StorageWindow : public ui::MainFrame {
public:
    StorageWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The storage place a slot of this window stands for (-1: none).
    static int PlaceOf(const ui::Widget* slot);
    int page() const { return page_; }

private:
    void Refresh();
    // Which storage it shows: the account's, or (the GuildStorageRoom section, 145) the guild's.
    game::Storage& Store() const { return guild_ ? context_.state->guildStorage : context_.state->storage; }
    HudContext& context_;
    bool guild_ = false;
    int page_ = 0;
    std::uint64_t seen_ = ~0ull;
    // What is held: a hidden slot carrying a copy of the item and its place, so it stays in the
    // hand when the page turns (the original's CIFDraggedStatic proxy).
    ui::Slot* proxy_ = nullptr;
    int proxyPlace_ = -1;
};

// The place a slot of the inventory or the equipment stands for: 13.. the bag, 0..12 the worn
// ones; -1 for any other slot (the underbar's shortcuts take them).
int InventoryPlaceOf(const ui::Widget* slot);
// The player's own offered place a slot of the exchange window is (0..11), -1 another (HudExchange.cpp).
int ExchangeOfferOf(const ui::Widget* slot);
// The avatar place an avatar slot of the equipment's dress side shows; -1 otherwise.
int AvatarPlaceOf(const ui::Widget* slot);

// The item tooltip's lines, as the original builds them (name with its plus and colour, kind,
// degree, white stats with their "(+X%)", requirements in red when not met, seal, blues, a stack's
// quantity).
std::vector<ui::TooltipLine> ItemTooltip(const assets::ItemRow& row, const game::ItemInstance& item, const HudContext& context);
// An item's slot: its icon, a stackable's count, red when it cannot be used (none: empty). `wornMarks`: the bag's and the
// equipment's slots, where a piece near breaking pulses and a broken one is framed (1.188 marks no other window's).
void ShowItem(ui::Slot& slot, const game::ItemInstance* item, const HudContext& context, bool wornMarks = false);
// A description as tooltip lines: plain text as it is, SML2 markup cut at its line breaks in its colours.
void AddTooltipDescription(std::vector<ui::TooltipLine>& out, std::string_view text);
// A skill's tooltip: its name and level, active or
// passive, what it costs and the weapon and items it needs, its description, what it does (from its
// parameter tags), then what learning it or its next level asks, each condition red while unmet.
std::vector<ui::TooltipLine> SkillTooltip(const assets::SkillRow& row, const HudContext& context);
// What a skill does, its effect lines alone (a socket stone's tooltip: "Lv1 : ..." per level).
std::vector<std::string> SkillEffectTexts(const assets::SkillRow& row, const HudContext& context);
// The gold as the inventory writes it (a comma every three digits) and its colour by amount.
std::string GoldText(std::uint64_t gold);
ui::Color GoldColor(std::uint64_t gold);

// The skill a dragged widget is (a learned skill's icon on the Skills page): its skilldata id, 0 none.
std::uint32_t DraggedSkill(const ui::Widget* source);
// The action a dragged widget is (an icon of the Action page): its actionwnddata id, 0 none.
std::uint32_t DraggedAction(const ui::Widget* source);
// The Input page's keys (Windows key codes, in the original's order of the 34 actions) as the HUD's
// bindings (HudOptions.cpp).
void ApplyKeyOptions(const core::InputOptions& input, ui::KeyBindings& bindings);
// The name of the key bound to an action, as the key options show it (empty: none).
std::string BoundKeyName(const ui::KeyBindings& bindings, ui::KeyAction action);
// CIFOption (the system menu's Option window; HudOptions.cpp).
std::unique_ptr<ui::Widget> MakeOptionWindow(const assets::UiLayoutNode& node, HudContext& context);
// The page the Option window opens on next (captures: --menu options:N); once, then the video page again.
void SetOptionStartPage(int page);
// CIFSkill (73, the main window's Skills page; HudSkills.cpp).
std::unique_ptr<ui::Widget> MakeSkillWindow(const assets::UiLayoutNode& node, HudContext& context);
// CIFAutoPotion (135, the T key; HudAutoPotion.cpp): the original's layout
// with a drop-in slot in each row in place of its quick slot's combo boxes.
std::unique_ptr<ui::Widget> MakeAutoPotionWindow(const assets::UiLayoutNode& node, HudContext& context);
// The exchange window (CIFExchange, resinfo\ifexchange.txt; HudExchange.cpp).
std::unique_ptr<ui::Widget> MakeExchangeWindow(const assets::UiLayoutNode& node, HudContext& context);
// A line in the open exchange window's message box; the client's text for an exchange's code (null: none).
void ExchangeNote(ui::Interface& ui, const std::string& text);
const char* ExchangeCodeKey(game::proto::ExchangeCode code);
// The stall window (CIFStall 33, resinfo\ifstall.txt) and the stall network's (CNIFStallNetwork 150 from
// res_ui\stallnetwork.2dt; HudStall.cpp).
void RegisterStallWindows(ui::Interface& ui, HudContext& context);
// The stall network's row and page numbers as other windows use them (the Open Market's): a row lit as chosen, the
// pages' numbers (four at a time; a press notifies Clicked with the page).
void StallRowChosen(ui::Widget& row, bool chosen);
void StallPages(ui::Widget& pages, int page, int count);
// The Open Market (HudOpenMarket.cpp): 1.188's CNIFOpenMarket (190, res_ui\open_market_main.2dt)
// with its Search, Register and Settle pages, and the registration's price box (194, open_market_1_1.2dt). Shown or closed;
// whether its Register page takes the bag's items (a right click), and one put in (its price asked); the script steps
// (om-page:search|register|settle, om-search:CLASS,TYPE[,DEGREE], om-choose:N, om-buy:N, om-register, om-cancel:N, om-settle;
// false: not a step of it, or the window is not made).
void RegisterOpenMarketWindows(ui::Interface& ui, HudContext& context);
// The temples' reputation board (HudTemple.cpp): CNIFConfirmReputation with the server's two rows.
void RegisterTempleWindows(ui::Interface& ui, HudContext& context);
void ShowTempleReputation(HudContext& context, const std::vector<game::proto::TempleReputationRow>& rows);
// The board's numbers again (proto::TemplePoints, asked every 10 s while it is shown): its rows' numbers and gauges.
void UpdateTemplePoints(HudContext& context, const game::proto::TemplePoints& points);
// The board's icon in the side icons' row (proto::TempleIcon; CIFConfirmReputationGuide 0xA5): shown or hidden.
void ShowTempleGuide(HudContext& context, bool show);
void ShowOpenMarket(HudContext& context, bool shown);
// The consignment notification window (197, open_market_2.2dt) and its side icon (198) while a ticket runs; the icon blinks
// after a sale's or an expiry's notice.
void ToggleMarketAlarm(HudContext& context);
void ShowMarketAlarmGuide(HudContext& context, bool shown);
void MarketAlarmNews(HudContext& context);
bool OpenMarketTakesItems(HudContext& context);
void OpenMarketPut(HudContext& context, int place);
bool OpenMarketScript(HudContext& context, const std::string& verb, const std::string& arg);
bool OpenMarketPrice(HudContext& context, std::uint64_t price); // the price box's price typed and confirmed; false: not shown
void OpenMarketSearchAgain(HudContext& context);               // the window's last search asked again (a purchase made)
// The free PvP window (CNIFFreeBattlePvp from res_ui\\frpvp.2dt; HudFreePvp.cpp): opened under the minimap.
void RegisterFreePvpWindow(ui::Interface& ui, HudContext& context);
// The alchemy window (CNIFEnchantWnd 168 from res_ui\\nifenchantwnd.2dt; HudAlchemy.cpp): opened
// and closed (Y, the Menu's 114, the Action page's 1010); shown (the bag is held then); a bag item put in (a right
// click); the server's answer.
void RegisterAlchemyWindow(ui::Interface& ui, HudContext& context);
void ToggleAlchemyWindow(HudContext& context);
bool AlchemyShown(HudContext& context);
void AlchemyPut(HudContext& context, int place);
void AlchemyResultArrived(HudContext& context, const game::proto::AlchemyResult& result);
// The smith's avatar blessing (1.188's CIFGrantMagicAttributeWnd, window 156): opened by a smith's row (`npc` its entity);
// a script's or the gallery's steps on it: an avatar put in from a bag place, a row picked, Confirm (false: not open).
void ShowAvatarGrant(HudContext& context, std::uint32_t npc);
bool AvatarGrantStep(HudContext& context, int put, int pick, bool confirm);
// A magic option's tooltip line as 1.188 writes it (HudItemOptions.cpp): empty when the client
// shows none; red for a penalty. And the names alchemy's stone lines use: the text of an option
// granted, its name in a change line, and whether its values read as percents.
struct MagicOptionText {
    std::string text;
    bool negative = false;
};
MagicOptionText MagicOptionLine(const HudContext& context, const assets::ItemRow& item, const game::ItemInstance::Option& option);
struct MagicOptionNames {
    std::string append, change;
    bool percent = false;
};
MagicOptionNames MagicOptionAlchemyNames(const HudContext& context, std::string_view optionName, std::uint32_t value);
// The live tests' steps: a tab shown (0 Disjoint, 1 Dismantle, 2 Manufacture, 3 Strengthen), the button pressed, the
// alert box answered (false: none shown).
void AlchemyShowTab(HudContext& context, int tab);
void AlchemyPress(HudContext& context);
bool AlchemyAnswer(HudContext& context, bool yes);
// The Socket page's: shown (else the Alchemy page), Make then a click on socket `index`, a
// stone (its bag place) dropped on a socket, Remove then a click on one, an insertion's Cancel; the pick mode (1
// making, 2 removing; 0 none).
void AlchemyShowSocket(HudContext& context, bool socket);
void AlchemySocketMake(HudContext& context, int index);
void AlchemySocketInsert(HudContext& context, int stonePlace, int index);
void AlchemySocketErase(HudContext& context, int index);
void AlchemySocketCancel(HudContext& context);
int AlchemySocketPickMode(HudContext& context);
// The equipment window's sockets' skill window (500) for a worn place, as a right click on it fills it (false: the item
// has no filled socket); the skill shown at `index` (0..3), or (dragged) what dragging it to a quick slot gives (an
// active's id; 0 a passive or none).
bool ShowWornSocketSkills(HudContext& context, int place);
// A one-shot effect on the slot of an inventory place (worn 0..12 in the equipment window, the bag's in the inventory):
// the repair's sparks or the gender switch's swirl (ui::Slot::Flash).
void FlashItemSlot(HudContext& context, int place, ui::Slot::Flash kind);
std::uint32_t WornSocketSkill(HudContext& context, int index, bool dragged);
// The quest windows (HudQuest.cpp): the quest list (Q), the reward window (a quest's page,
// its reward and its button; `npc` the finisher talked to, 0 to look), the quick window.
void RegisterQuestWindows(ui::Interface& ui, HudContext& context);
// The game guide (HudGameGuide.cpp).
void RegisterGameGuideWindows(ui::Interface& ui, HudContext& context);
// The guide open on that topic (its tab, its heading opened; 0: as it was).
void OpenGameGuideAt(HudContext& context, std::uint32_t topic);
// The event guide's side icon up for that page (snd_quest), its click opening the guide there.
void ShowEventGuide(HudContext& context, std::uint32_t topic);
// The academy's guide icon (CIFEventGuideSecond) up: a level gained under 40 outside an academy, as the original's.
void ShowMentorGuide(HudContext& context);
void ToggleQuestWindow(HudContext& context);
// The level-up's quest guide: the rows kept, the side icon up (none: put away).
void ShowQuestGuide(HudContext& context, std::vector<game::proto::QuestGuide::Row> rows);
void OpenQuestReward(HudContext& context, std::uint32_t quest, std::uint32_t npc);
// The reward window's item of that codename picked and its button pressed (the quests' tests; false: no such item).
bool PickQuestReward(HudContext& context, const std::string& item);
// The reward's confirm (res_ui\corfim.2dt) on those picked items, as "Reward" opens it (the gallery's page).
void OpenQuestRewardConfirm(HudContext& context, std::uint32_t quest, std::uint32_t npc, const std::vector<std::uint8_t>& picked);
void ShowQuestQuick(HudContext& context);
// A 1.188 CNIF window's layout (res_ui\*.2dt) as widgets under `root` (HudQuest.cpp), and its root node.
std::vector<ui::Widget*> Build2dt(ui::Interface& ui, ui::Widget& root, const std::vector<assets::Ui2dtRecord>& records,
                                  const std::function<void(const assets::Ui2dtRecord&, assets::UiLayoutNode&)>& adjust = {});
std::optional<assets::UiLayoutNode> RootNode2dt(const HudContext& c, const char* layout, const char* type, int id);
// The Forgotten World's windows (HudInstance.cpp): the party recall pillar's summon
// window, the collection book, a talisman's story and its register box.
void RegisterInstanceWindows(ui::Interface& ui, HudContext& context);
void OpenSummonWindow(HudContext& context);
void ToggleCollectionBook(HudContext& context);
void ShowCollectionBook(HudContext& context); // shown and raised (the underbar menu's row, case 104)
// A talisman registered: the book opened on its theme, the slot's flip-book played.
void ShowTalismanRegistered(HudContext& context, std::uint32_t theme, int slot);
void OpenCollectionBox(HudContext& context, std::uint8_t place, const assets::ItemRow& item);
// Script steps: the summon window's row of that member chosen and summoned; the book's theme (its row) and a slot's story.
bool SummonByName(HudContext& context, const std::string& name);
bool PickCollectionTheme(HudContext& context, int row);
bool OpenCollectionStory(HudContext& context, int slot);
bool AnswerCollectionBox(HudContext& context, bool yes); // false: not shown
void OpenFreePvpWindow(HudContext& context);
// Capture the Flag's windows (HudFlagWorld.cpp): the event timer (CIFTimerWnd 410, from
// the flag view), the camp countdown (CIFTimeDrivenWnd 0x7538, 10 s, one at a time), the Subscription Card's box.
void RegisterFlagWorldWindows(ui::Interface& ui, HudContext& context);
void OpenCampCountdown(HudContext& context);
bool CampCountdownShown(const HudContext& context);
void ShowSubscribedBox(HudContext& context);
bool AnswerInformBox(HudContext& context); // its Confirm pressed (scripts); false: none shown
// The 48 x 2 yellow bar under a head: a job suit's change, a flag
// pole's action. `x`, `y`: the point 2 over the body on the screen; the bar shrinks to the left over `seconds`.
void DrawActionBar(ui::Painter& paint, float x, float y, double elapsed, int seconds);
// The Battle Arena's windows (HudArena.cpp): the rule window (162) for a mode and the kinds'
// bits (the guild mode with its chosen members), the guild window (160), both closed; the score (200), rank (163) and
// result (181) windows kept as the view has them, once a frame; a line's text (its arguments put in).
void RegisterArenaWindows(ui::Interface& ui, HudContext& context);
void OpenArenaRule(HudContext& context, std::uint8_t mode, std::uint16_t kinds, std::vector<std::uint32_t> members = {});
void OpenArenaGuild(HudContext& context, std::uint16_t kinds);
void CloseArenaRegistration(HudContext& context);
void ArenaWindows(HudContext& context);
std::string ArenaLineText(const HudContext& context, const game::ArenaView::Line& line);
// Script steps: the rule window's kind row picked (and Register pressed: its confirm box asked); the guild window's
// members of those names moved right (then Confirm); the result window closed. False: not shown or not found.
bool ArenaRuleShown(const HudContext& context);
bool ArenaPickKind(HudContext& context, std::uint8_t kind);
bool ArenaRegisterKind(HudContext& context, std::uint8_t kind);
bool ArenaGuildChoose(HudContext& context, const std::vector<std::string>& names, bool confirm);
bool CloseArenaResult(HudContext& context);
// The client's text for a stall's code (null: none); the register box for an item of the bag put up at a
// slot of the player's own stall (the inventory's drop on a slot, the scripts).
const char* StallCodeKey(game::proto::StallCode code);
void StallRegister(HudContext& context, int slot, int place);
// A visitor's buy of a stall's slot (its item dragged to the bag): the agreement box, its icon, name, count and
// price, Buy and Cancel (the original's store money box for a stall's purchase).
void StallBuyBox(HudContext& context, int slot);
// The stall's slot (0..9) a widget of the stall window is, -1 another (an item dragged out of it).
int StallSlotOf(const ui::Widget* slot);
// The network window's rows as shown and a row chosen (the scripts and tests): its seller and listing.
struct StallNetworkPick {
    std::string seller;
    std::uint64_t listing = 0;
};
std::vector<StallNetworkPick> StallNetworkRows(ui::Interface& ui);
// The open network window's search with the combos set by index (class, type; degree 0 any) and a row of the
// page shown bought (its question asked).
void StallNetworkSearch(ui::Interface& ui, int classIndex, int typeIndex, int degree);
void StallNetworkBuy(ui::Interface& ui, int row);
// Its rows' drop slots (0 HP, 1 MP, 2 the abnormal state), for scripts and tests; null when not there.
ui::Slot* AutoPotionSlot(ui::Widget* window, int row);
// An item of the inventory (its bag place) dropped on a row's slot, as a drop does: taken when it fits
// the row (game::AutoPotionFits); false when it does not.
bool AutoPotionDrop(ui::Widget* window, int row, int place);
// A row's check box and percent, the delay's box and value, as the player sets them (scripts, tests).
void AutoPotionSetRow(ui::Widget* window, int row, bool enabled, int percent);
void AutoPotionSetDelay(ui::Widget* window, bool enabled, int ms);
// The window's Confirm, as a click on it (scripts and tests).
void AutoPotionConfirm(ui::Widget* window);
// Its tab (ours: 0 Character, 1 Vehicle, 2 Pet).
void AutoPotionTab(ui::Widget* window, int tab);

// CIFPlayerInfo (75, the main window's Character page; ifplayerinfo_trijob2.txt): "Lv %d", the
// experience now and what the level needs, the stat points and the + beside STR and INT (pressable
// while there are any; each spends one), HP and MP with their gauges, the attack, defence, balance,
// hit and parry numbers.
class PlayerInfo : public ui::Widget {
public:
    PlayerInfo(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;

private:
    HudContext& context_;
};

// CIFInventory (70, page of the main window): 32 places a tab (4 x 8, made in code), one tab
// "Equipment" (two, "Expand", beyond 32 places), places beyond the size locked; the gold below.
// Dropping an item on a place moves it there (the server says how it went).
class InventoryWindow : public ui::Widget {
public:
    InventoryWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The slot of an inventory place (13 ..), null when none.
    ui::Slot* SlotOf(int place) const;
    void SelectTab(int tab);

private:
    void Build(ui::Interface& ui, int places);
    void Refresh();
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::vector<ui::Slot*> slots_;
    std::vector<ui::SelectableArea*> tabs_;
    int tab_ = 0, places_ = -1;
    std::uint64_t seen_ = ~0ull, exchangeSeen_ = ~0ull;
    bool picking_ = false; // a services pick mode was on at the last refresh (HudServices.cpp)
};

// CIFEquipSkillSlotWnd (500, the equipment window's child at (0, 240), its own ifequipskillslotwnd.txt): four skill
// slots (1..4 over pt_block02 frames 21..24). A right click on a worn item with filled sockets fills it with their
// skills: an active one may be dragged to the quick slots, a passive one is veiled.
class EquipSkillWindow : public ui::Widget {
public:
    EquipSkillWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    // The skills shown (skilldata ids, at most four; none: hidden) and the worn place they come from (-1 none).
    void Show(const std::vector<std::uint32_t>& skills, int place);
    int place() const { return place_; }
    const std::array<std::uint32_t, 4>& skills() const { return skills_; }
    // The active skill a slot of it shows (a drag's source), 0 none.
    std::uint32_t SkillOf(const ui::Widget* w) const;

private:
    HudContext& context_;
    std::array<ui::Slot*, 4> slots_{};
    std::array<std::uint32_t, 4> skills_{};
    int place_ = -1;
};

// CIFEquipment (71, beside the inventory): the 13 places worn (head .. right ring) around the
// player's character turning in 3D over the stone background, their part pictures always under
// them; the turn buttons. Its dress side: button 13 shows the five avatar
// slots 113..117 (hat, dress, attachment, flag, Devil's Spirit) with the job suit's, button 14 the
// worn places again; an avatar dropped on the window is put on, one taken to the bag off.
class EquipmentWindow : public ui::Widget {
public:
    EquipmentWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    void Draw(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    ui::Slot* SlotOf(int place) const { return place >= 0 && place < 13 ? slots_[std::size_t(place)] : nullptr; }
    // The avatar slot of a kind (game::avatar::kHat .. kSpirit; slots 113..117).
    ui::Slot* AvatarSlotOf(int kind) const { return kind >= 0 && kind < 5 ? avatarSlots_[std::size_t(kind)] : nullptr; }
    // The avatar place whose item a slot of the dress side shows; -1 for another slot or an empty one.
    int AvatarPlaceShown(const ui::Widget* slot) const;
    // The dress side shown (button 13) or the worn places (button 14).
    bool AvatarView() const { return avatars_; }
    void ShowView(bool avatars);
    float yaw() const { return yaw_; }
    // A right click on a worn place: its item's sockets' skills in the skill window (500), or the window hidden when it
    // has none (the scripts' and the tests' way too).
    void ShowSocketSkills(int place);
    EquipSkillWindow* SkillWindow() const;

private:
    void Refresh();
    HudContext& context_;
    std::array<ui::Slot*, 13> slots_{};
    std::array<ui::Slot*, 5> avatarSlots_{};
    std::array<int, 5> avatarShown_{-1, -1, -1, -1, -1}; // the avatar place each avatar slot shows
    bool avatars_ = false;
    float yaw_ = 0.0f, frameMs_ = 16.0f;
    std::uint64_t seen_ = ~0ull;
    bool repairing_ = false; // the store's Repair pick was on at the last refresh (HudServices.cpp)
};

// CIFSystemWnd (98): Option, Help, Restart and Exit, stacked 34 apart.
class SystemMenu : public ui::MainFrame {
public:
    SystemMenu(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    bool OnNotice(const ui::Notice& notice) override;

private:
    HudContext& context_;
};

} // namespace sr::client
