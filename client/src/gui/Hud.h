#pragma once

// The HUD manager: the windows of ginterface.txt's Create section are made at the start and only
// shown or hidden afterwards; a window of its other sections is made when it opens and thrown away
// when it closes. It places them by the screen's size, keeps the always-on-top set in front, and
// turns the keys into their actions: windows it opens itself, the rest handed to the screen as
// requests (the game's own work: quick slots, sitting, the camera's view, leaving).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sr/assets/UiLayout.h"
#include "sr/game/PlayerState.h"
#include "sr/ui/Context.h"
#include "sr/ui/Hotkeys.h"
#include "sr/ui/Interface.h"
#include "gui/HudWindows.h"

namespace sr::client {

// The HUD's window ids (ginterface.txt).
namespace hud {
constexpr int kChat = 1, kMinimap = 10, kPlayerInfo = 11, kUnderBar = 12, kStore = 15, kTarget = 16, kStorage = 19, kGuildStorage = 145,
              kNotice = 20, kBuffs = 22, kCommunity = 23, kLetterAlarm = 55, kQuickParty = 24, kMainPopup = 25, kExchange = 26, kLoading = 27,
              kWorldMap = 28, kNpc = 30, kPractice = 31, kGameGuide = 32, kStall = 33, kDurability = 34, kWarning = 35,
              kPets = 39, kDelayGauges = 40, kEventGuide = 41, kQuestNotice = 42, kAlchemy = 44, kItemMall = 167, kGlobalChat = 56,
              kSystemMessages = 68, kRegionView = 83, kHelpBubble = 97, kSystemMenu = 98, kDragged = 99, kPetWindow = 120,
              kPartyMatch = 130, kExtQuickSlots = 133, kAutoPotion = 135, kStallNetwork = 150, kSupporterChat = 161,
              kFreePvp = 187, kMenuBar = 175, kEnchant = 168, kAlchemyAlert = 172, kQuestQuick = 173, kQuestReward = 188, kQuestList = 1076, kCollection = 210, kCollectionBox = 211, kCollectionStory = 212, kSummon = 214, kEventRank = 400, kTimer = 410, kQuickState = 420, kQuickStateHalf = 421, kEventGuideSecond = 30006,
              kTempleGuide = 165, kEquipState = 157;
// Ours: the performance panel (HudWindows.h PerformancePanel), an id no layout uses.
constexpr int kPerformance = 9000;
// Ours: the bug report window (HudWindows.h BugReportWindow; the build's SR_BUG_REPORTS).
constexpr int kBugReport = 9001;
// The main window's pages: the ids of the windows nested in it.
constexpr int kPageInventory = 70, kPageEquipment = 71, kPageParty = 72, kPageSkills = 73, kPageActions = 74,
              kPageCharacter = 75, kPageQuests = 76, kPageAcademy = 77;
} // namespace hud

// What the keys and the HUD's buttons ask of the game; the screen does them (or says they come
// with a later phase).
struct HudRequest {
    enum class Kind : std::uint8_t {
        UseQuickSlot,     // value: the quick slot (0 the right button's, 1..40 the pages', 41..50 the extra bar's)
        QuickSlotChanged, // value: the quick slot the player changed (the session keeps them)
        TargetSelf,       // the player's bar clicked
        CancelBuff,       // value: the effect's index in PlayerState::effects
        Berserk,
        SitStand,
        PickUp,
        ShowDroppedItems, // value: 1 while held, 0 released
        HideOthers,       // the same
        PetCommand,       // value: the command (0 info, 1 ride, 2 attack, 3 follow, 6 behaviour, 100 unsummon, 101 next pet)
        Reply,
        TargetEnemyPlayer,
        LastTarget,
        AssistTarget,
        FaceTarget,
        HideAllies,
        HideEnemies,
        TargetPartyMember, // value: 0 the player, 1..7 the party's members
        NextView,          // F9: the camera's next view
        ChatInput,         // Enter (the chat takes it; the screen when there is none)
        Chat,              // a line to send: value the kind (game::proto::ChatKind), text, target (a whisper's)
        ChatCommand,       // a "/" command typed in the chat: text (its feature does it)
        MoveItem,          // value: the place it is in, extra: the place it goes to | quantity << 8 (0: all)
        Restart,           // the system menu: back to the character select
        Quit,              // the system menu: to Windows
        UseItem,           // value: the inventory place (a right click on it)
        CancelCasting,     // the casting gauge's cancel button
        GateTeleport,      // the NPC window: value the gate, extra the place it leads to (a teleportdata id)
        GateDesignate,     // the NPC window: value the gate whose point would be the return point (asks first)
        OpenStorage,       // the NPC window: value the storage keeper's entity
        GuildStorage,      // the NPC window's guild row: value the NPC's entity
        StorageMove,       // value: from | to << 8 | fromStorage << 16 | toStorage << 17 | the guild's << 18
        StorageGold,       // text: the amount, value: 1 deposit (0 take) | 2 the guild's
        OpenShop,          // the NPC window: value the merchant's entity, extra the tab group
        ShopBuy,           // value: the tab | the good's slot << 8, extra: the count
        ShopSell,          // value: the inventory place, extra: the count (0: all)
        ShopBuyback,       // value: the buy-back entry
        DropItem,          // value: the inventory place let go over the world (asks first)
        DropGold,          // text: the amount
        RaiseStat,         // the character page's +: value 0 strength, 1 intelligence
        LearnSkill,        // the Skills page: value the skill's next level (its skilldata id)
        RaiseMastery,      // the Skills page: value the mastery
        // Parties and party matching (HudParty.cpp).
        PartyInvite,        // value: a player's entity (in no party: a new party with `extra`'s settings)
        PartyAnswer,        // value: 1 yes, 0 no (the request box)
        PartyLeave,         // the party window's leave, the master's dismiss
        PartyKick,          // value: the member's id
        PartySettings,      // value: the settings (the master's party's; in none: the next party's)
        PartyMatchList,     // value: the page, extra: the purpose (0xFF all) | levelMin << 8 | levelMax << 16, text: a name
        PartyMatchRegister, // value: settings | purpose << 8, extra: levelMin | levelMax << 8, text: the title
        PartyMatchDelete,
        PartyMatchJoin,     // value: the listing
        PartyMatchAnswer,   // value: 1 yes, 0 no (the leader's join request box)
        SelectEntity,       // value: an entity to select (a party member's slot)
        ToggleQuests,       // Q: the quest window (1.188's CNIFQuest, its own window)
        ToggleAlchemy,      // Y: the alchemy window (1.188's CNIFEnchantWnd 168, its own window)
        // The avatars (HudInventory.cpp).
        WearAvatar,    // value: the bag's place of the avatar put on (into the avatar place of its kind)
        TakeOffAvatar, // value: the avatar place (into the bag's first free place)
        UseSkill,      // value: the skill (a right click on a learned one in the S window), used as a quick slot's
    };
    Kind kind;
    int value = 0;
    std::string text, target;
    int extra = 0;
};

class Hud {
public:
    // Makes the Create section's windows (in the file's order of depth: its first node in front),
    // hides those that start hidden and places them for the screen (interface pixels).
    void Create(ui::Interface& ui, ui::Vec2 screen);
    bool Created() const { return ui_ != nullptr; }
    // The state the keys work on (the underbar's page, the quick slots) and the windows show.
    void Bind(game::PlayerState* state) { state_ = state; }
    // The start places by the screen's size (section 7), then every window pulled onto it.
    void Place(ui::Vec2 screen);
    ui::Vec2 Screen() const { return screen_; }

    // The keys of this frame (none while a text field has the keyboard); call after the
    // interface's frame, with the clock the state's times are on. Requests for the game collect
    // until TakeRequests.
    void Keys(const ui::Context& context, double now);
    // The windows the state shows or hides: the target window when a target is chosen.
    void Sync();
    // A quick slot used (a key, a click): the request, and the underbar's frame on its column
    // when it is one of the page's ten.
    void UseQuickSlot(int index, double now, bool frame = true);
    std::vector<HudRequest> TakeRequests() { return std::move(requests_); }
    void Request(HudRequest request) { requests_.push_back(request); }

    // A window by id: shown (made from its ginterface.txt section when it is made on demand),
    // hidden (thrown away when made on demand), or the one or the other.
    ui::Widget* Show(int id);
    void Hide(int id);
    // A side icon made in code (no ginterface.txt node; the temples' board icon CIFConfirmReputationGuide 0xA5): shown in
    // the side icons' row (below), or the one already made.
    ui::Widget* ShowGuide(const assets::UiLayoutNode& node);
    // A window the game puts up by itself (the pet bar on a summon): while F12 hides the interface it
    // waits in F12's list and comes up with the rest on the next F12.
    void ShowWithInterface(int id);
    // F12's: hidden the moment, or whether a window is shown or waits for F12's return.
    bool InterfaceHidden() const { return f12Hidden_; }
    bool ShownOrWaiting(int id) const;
    ui::Widget* Toggle(int id);
    bool Shown(int id) const;
    // The main window on one of its pages (hud::kPage*), or hidden if it shows that page already.
    void TogglePage(int page);
    int Page() const { return page_; } // the main window's page (shown or last shown)
    // The main window shown on a page, in front.
    void OpenPage(int page);
    // The Option window (ifsystemwnd.txt's OptionWnd section), centred; raised when it is open.
    void OpenOptions();
    // The underbar's Menu: the main window shown on its last page, or hidden.
    void ToggleMain();
    // Escape: closes one thing (section 6.4); with nothing to close, opens the system menu.
    void Escape();
    // A message box (CIFMessageBox) with a title key, lines and Yes / No, centred; `answer` hears
    // which (No when closed). The box made (none without an interface).
    ConfirmBox* Confirm(const std::string& titleKey, const std::vector<std::string>& lines, std::function<void(bool yes)> answer);
    // A message box that only tells (one button, Confirm); `done` hears its button or its closing.
    void Inform(const std::string& titleKey, const std::vector<std::string>& lines, std::function<void()> done);
    // The reverse return scroll's box (ReverseReturnBox), centred: `answer` hears 1 the last recall point, 2 the place
    // of death, 3 a point on the map, 0 cancelled.
    void ReverseReturn(std::function<void(int result)> answer);
    // The split box (MsgBoxDivideCount) for a stack: `answer` hears the number to move off (none
    // when cancelled).
    void Divide(const std::string& icon, const std::string& name, int count, std::function<void(int)> answer);
    // A store's box (buy, sell, buy back), centred.
    void Deal(const DealBox::Deal& deal);
    // The storage's gold box, centred: into the storage (the inventory's coin) or out of it (the
    // storage's coin).
    void StorageGold(bool deposit, bool guild = false);
    // The same box to drop gold on the ground ("Amount"), and for the gold offered in an exchange.
    void DropGoldBox();
    void ExchangeGoldBox();
    // The open message box answered as by a click (scripts); false when none is open.
    bool AnswerConfirm(bool yes);
    // The Skills page's question (CIFSkillPracticeBox, ginterface 31).
    void Practice(const SkillPracticeBox::Question& q, std::function<void()> confirmed);
    // The death window (RebirthBox), placed as the original's message boxes: at (W/2 - 154, H/2 - 74)
    // of the screen; and closed again (back to life: every message box goes).
    // `firstButton`: another text key for its first button (a Capture the Flag match's "Resurrect at your camp.").
    void Rebirth(int level, std::function<void(int kind)> answer, const std::vector<std::string>& warLines = {}, double riseAt = 0.0, std::string countdown = {},
                 const std::string& firstButton = {});
    void CloseRebirth();
    // Its button (200 the first, 201 the second) pressed as by a click (scripts); false when it is not shown.
    bool PressRebirth(int button);
    bool RebirthShown() const;

    // F10 and the underbar's Item Mall button (the screen asks "Enter Item Mall" first; none: the window at once).
    std::function<void()> onItemMall;

    ui::KeyBindings& bindings() { return bindings_; }
    ui::Interface* interface() { return ui_; }

private:
    const assets::UiLayoutNode* NodeOf(int id, std::string* section = nullptr) const;
    void ShowPage(ui::Widget& main, int page);
    void Sound(bool opened); // snd_window_open / snd_window_close

    ui::Interface* ui_ = nullptr;
    game::PlayerState* state_ = nullptr;
    std::uint32_t shownTarget_ = 0;
    bool npcShown_ = false; // the NPC window was open at the last Sync
    const assets::UiLayout* master_ = nullptr;
    ui::Vec2 screen_{1024, 768};
    ui::KeyBindings bindings_;
    std::vector<HudRequest> requests_;
    int page_ = hud::kPageCharacter; // the main window's last page
    std::vector<int> hiddenByF12_;   // F12: the windows it hid, shown again by the next F12
    // The side icons (CAlramGuideMgrWnd, ginterface 158; adds one at the end, lays them out): in the
    // order they came up.
    std::vector<int> guides_;
    static bool IsGuide(int id);
    void LayoutGuides();
    bool f12Hidden_ = false;
    bool showingDropped_ = false, hidingOthers_ = false; // the held keys' state
};

} // namespace sr::client
