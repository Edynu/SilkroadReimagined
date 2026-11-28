#include "gui/HudWindows.h"

#include "gui/Hud.h"
#include "gui/HudParty.h"

#include <memory>

#include "sr/game/SkillText.h"

namespace sr::client {

std::string FormatText(const std::string& format, std::initializer_list<std::string> args) { return game::FormatText(format, args); }

void MainPopup::Update(ui::Frame& frame) {
    ui::MainFrame::Update(frame);
    // The left strip (ifmainpopup.txt's round buttons 10..16 and their backing 4): 1.188 hides them when it makes the
    // window and never shows them again; its pages are reached by the under menu bar and the keys.
    for (int id : {4, 10, 11, 12, 13, 14, 15, 16}) {
        if (ui::Widget* w = Child(id)) w->visible = false;
    }
}

bool MainPopup::OnNotice(const ui::Notice& notice) { return ui::MainFrame::OnNotice(notice); }

void RegisterHudWindows(ui::Interface& ui, HudContext& context) {
    HudContext* c = &context;
    RegisterGuildWindows(ui, context);
    RegisterGuildMasterWindows(ui, context); // HudGuildMaster.cpp
    RegisterGuildBoxWindows(ui, context);    // HudGuildBoxes.cpp
    RegisterGuildWarWindows(ui, context);    // HudGuildWar.cpp
    RegisterFortressWindows(ui, context);    // HudFortress.cpp
    RegisterFortressWorkWindows(ui, context); // HudFortressWork.cpp
    RegisterActionWindow(ui, context);       // HudActions.cpp
    RegisterFortressMapWindows(ui, context); // HudFortressMap.cpp
    RegisterWorldMapWindow(ui, context);     // HudWorldMap.cpp
    RegisterJobWindows(ui, context);         // HudJobs.cpp
    RegisterTradeWindows(ui, context);       // HudTrade.cpp
    RegisterMagicPopWindows(ui, context);    // HudMagicPop.cpp
    RegisterMallWindows(ui, context);        // HudMall.cpp
    RegisterServiceWindows(ui, context);     // HudServices.cpp
    ui.Register("CIFMainPopup", [c](const assets::UiLayoutNode& n) { return std::make_unique<MainPopup>(n, *c); });
    ui.Register("CIFPlayerMiniInfo", [c](const assets::UiLayoutNode& n) { return std::make_unique<PlayerBar>(n, *c); });
    ui.Register("CIFMagicStateBoard", [c](const assets::UiLayoutNode& n) { return std::make_unique<BuffBoard>(n, *c); });
    ui.Register("CIFUnderBar", [c](const assets::UiLayoutNode& n) { return std::make_unique<UnderBar>(n, *c); });
    ui.Register("CNIFUnderMenuBar", [c](const assets::UiLayoutNode& n) { return std::make_unique<MenuBar>(n, *c); });
    ui.Register("CIFExtQuickSlot", [c](const assets::UiLayoutNode& n) { return std::make_unique<ExtraQuickSlots>(n, *c); });
    RegisterExtraBarOptions(ui, *c);
    ui.Register("CIFTargetWindow", [c](const assets::UiLayoutNode& n) { return std::make_unique<TargetWindow>(n, *c); });
    ui.Register("CIFBuffViewer", [c](const assets::UiLayoutNode& n) { return std::make_unique<TargetBuffs>(n, *c); });
    // The target window's kinds: their art, in other looks set by the window.
    for (const char* kind : {"CIFTargetWindowPlayer", "CIFTargetWindowJobPlayer", "CIFTargetWindowSpecialMob", "CIFTargetWindowCommonEnemy",
                             "CIFTargetWindowFortressStructure"}) {
        ui.Register(kind, [](const assets::UiLayoutNode& n) { return std::make_unique<ui::Static>(n); });
    }
    ui.Register("CIFMinimap", [c](const assets::UiLayoutNode& n) { return std::make_unique<Minimap>(n, *c); });
    ui.Register("CIFRegionView", [c](const assets::UiLayoutNode& n) { return std::make_unique<AreaBanner>(n, *c); });
    ui.Register("CIFSystemMessage", [c](const assets::UiLayoutNode& n) { return std::make_unique<SystemMessages>(n, *c); });
    ui.Register("CIFNotify", [c](const assets::UiLayoutNode& n) { return std::make_unique<NoticeBanner>(n, *c); });
    // Ours (no class of the original): the performance panel.
    ui.Register("SRPerformancePanel", [c](const assets::UiLayoutNode& n) { return std::make_unique<PerformancePanel>(n, *c); });
#ifdef SR_BUG_REPORTS
    ui.Register("SRBugReport", [c](const assets::UiLayoutNode& n) { return std::make_unique<BugReportWindow>(n, *c); });
    RegisterHelpRequestWindow(ui, context); // HudHelpRequest.cpp
#endif
    ui.Register("CIFSystemWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<SystemMenu>(n, *c); });
    ui.Register("CIFChatViewer", [c](const assets::UiLayoutNode& n) { return std::make_unique<ChatWindow>(n, *c); });
    ui.Register("CIFChatModeViewer", [c](const assets::UiLayoutNode& n) { return MakeChatModeViewer(n, *c); });
    ui.Register("CIFWholeChat", [c](const assets::UiLayoutNode& n) { return std::make_unique<GlobalChatWindow>(n, *c); });
    ui.Register("CIFInventory", [c](const assets::UiLayoutNode& n) { return std::make_unique<InventoryWindow>(n, *c); });
    ui.Register("CIFPlayerInfo", [c](const assets::UiLayoutNode& n) { return std::make_unique<PlayerInfo>(n, *c); });
    ui.Register("CIFSkill", [c](const assets::UiLayoutNode& n) { return MakeSkillWindow(n, *c); });
    ui.Register("CIFOption", [c](const assets::UiLayoutNode& n) { return MakeOptionWindow(n, *c); });
    // A skill row: its bar (skl_mastery_bar: the cells over the stone band) under its slots.
    ui.Register("CIFSkillGroup", [](const assets::UiLayoutNode& n) { return std::make_unique<ui::Static>(n); });
    // The board under the rows: a frame (a CIFFrame in the RTTI) of int_window_ parts, whose bottom
    // edge closes the list (left out, the 4 pixels under the list were see-through).
    ui.Register("CIFSkillBoard", [](const assets::UiLayoutNode& n) { return std::make_unique<ui::FrameBorder>(n); });
    ui.Register("CIFSkillPracticeBox", [c](const assets::UiLayoutNode& n) { return std::make_unique<SkillPracticeBox>(n, *c); });
    ui.Register("CIFEquipment", [c](const assets::UiLayoutNode& n) { return std::make_unique<EquipmentWindow>(n, *c); });
    ui.Register("CIFEquipSkillSlotWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<EquipSkillWindow>(n, *c); });
    ui.Register("CIFStoreForPackage", [c](const assets::UiLayoutNode& n) { return std::make_unique<StoreWindow>(n, *c); });
    ui.Register("CIFStoreDeal", [c](const assets::UiLayoutNode& n) { return std::make_unique<DealBox>(n, *c); });
    ui.Register("CIFStorageGold", [c](const assets::UiLayoutNode& n) { return std::make_unique<GoldBox>(n, *c); });
    ui.Register("CIFStorageRoom", [c](const assets::UiLayoutNode& n) { return std::make_unique<StorageWindow>(n, *c); });
    ui.Register("CIFDelayGaugeBoard", [c](const assets::UiLayoutNode& n) { return std::make_unique<DelayGaugeBoard>(n, *c); });
    ui.Register("CEquipStateImg", [c](const assets::UiLayoutNode& n) { return std::make_unique<EquipStateImage>(n, *c); });
    ui.Register("CIFDelayInfo", [](const assets::UiLayoutNode& n) { return std::make_unique<ui::Static>(n); });
    ui.Register("CIF_NPCWindow", [c](const assets::UiLayoutNode& n) { return std::make_unique<NpcWindow>(n, *c); });
    // The talk panel inside it: a frame of its own (npc_conversation_window_*) around the dark tile.
    ui.Register("CIF_NPCTalk", [](const assets::UiLayoutNode& n) { return std::make_unique<ui::FrameBorder>(n); });
    // The message boxes: the split box by its section, the Yes / No one otherwise.
    ui.Register("CIFMessageBox", [](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> {
        if (n.subSection == "MsgBoxDivideCount") return std::make_unique<DivideBox>(n);
        if (n.subSection == "Rebirth") return std::make_unique<RebirthBox>(n);
        if (n.subSection == "ReverseReturn") return std::make_unique<ReverseReturnBox>(n);
        if (n.subSection == "MsgBoxINIF") return std::make_unique<InformBox>(n); // the Subscription Card's (HudFlagWorld.cpp)
        return std::make_unique<ConfirmBox>(n);
    });
    RegisterPartyWindows(ui, context); // HudParty.cpp
    RegisterAcademyWindows(ui, context); // HudAcademy.cpp
    RegisterOpenMarketWindows(ui, context); // HudOpenMarket.cpp
    RegisterTempleWindows(ui, context);     // HudTemple.cpp
    ui.Register("CIFAutoPotion", [c](const assets::UiLayoutNode& n) { return MakeAutoPotionWindow(n, *c); }); // HudAutoPotion.cpp
    ui.Register("CIFExchange", [c](const assets::UiLayoutNode& n) { return MakeExchangeWindow(n, *c); });     // HudExchange.cpp
    RegisterStallWindows(ui, context); // HudStall.cpp
    RegisterFreePvpWindow(ui, context); // HudFreePvp.cpp
    RegisterAlchemyWindow(ui, context); // HudAlchemy.cpp
    RegisterQuestWindows(ui, context);  // HudQuest.cpp
    RegisterGameGuideWindows(ui, context); // HudGameGuide.cpp
    RegisterInstanceWindows(ui, context); // HudInstance.cpp
    RegisterCosWindows(ui, context);      // HudCos.cpp
    RegisterFlagWorldWindows(ui, context); // HudFlagWorld.cpp
    RegisterArenaWindows(ui, context);     // HudArena.cpp
}

} // namespace sr::client
