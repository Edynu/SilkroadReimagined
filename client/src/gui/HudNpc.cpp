// The NPC window and the message box (C7i): a gate's menu, the
// places it leads to, and the question before its point becomes the return point.

#include <algorithm>
#include <memory>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/OpenMarketProtocol.h"
#include "sr/game/EventProtocol.h"
#include "sr/game/JobProtocol.h"

namespace sr::client {

namespace {

namespace proto = game::proto;

// The menu's colours: white, orange under the mouse (a screenshot of the original the user
// supplied, 2026-10-30; passes 0xFFEFDAA4 with each destination, which the window shows
// white: what that colour is for is not traced).
// A row, one the guild's level does not allow yet, under the mouse.
const ui::Color kChoice = ui::Argb(0xFFEFDAA4), kChoiceRed = ui::Argb(0xFFFF4A4A), kChoiceHover = ui::Argb(0xFFFF8A00);

// Whether the fortress manager shows its rows: the player's guild holds a fortress, or a guild of its
// union does (the 1.188 client takes the fortress of the world the player stands in; a guild holds one at most).
bool FortressManagerServes(const HudContext& c) {
    if (c.guild == nullptr || !c.guild->InGuild() || c.fortress == nullptr) return false;
    for (const proto::FortressRecord& f : c.fortress->Fortresses()) {
        if (f.owner == 0) continue;
        if (f.owner == c.guild->Guild().id) return true;
        for (const proto::UnionGuildRecord& g : c.guild->Union().guilds) {
            if (g.id == f.owner) return true;
        }
    }
    return false;
}

// A gate's pulley (NPC function 0x1F): the gate it works, the gate
// structure in view nearest to it (they stand beside each other; inferred), and only for the members of the guild holding
// its fortress. Null: no row.
const proto::EntityStructure* PulleyGate(const HudContext& c, std::uint32_t npc) {
    if (c.fortress == nullptr || c.world == nullptr || c.guild == nullptr || !c.guild->InGuild()) return nullptr;
    const game::WorldView::Entity* at = c.world->Find(npc);
    if (at == nullptr) return nullptr;
    const proto::FlatPosition p = proto::ToFlat(at->position);
    const proto::EntityStructure* best = nullptr;
    double nearest = 300.0;
    for (const auto& [entity, s] : c.fortress->Structures()) {
        const game::WorldView::Entity* e = c.world->Find(entity);
        if (e == nullptr || s.kind != std::uint8_t(game::StructureKind::Gate)) continue;
        const proto::FlatPosition g = proto::ToFlat(e->position);
        const double d = std::hypot(g.x - p.x, g.z - p.z);
        if (d < nearest) {
            nearest = d;
            best = &s;
        }
    }
    const proto::FortressRecord* f = best != nullptr ? c.fortress->Fortress(best->fortress) : nullptr;
    if (f == nullptr) return nullptr;
    const std::uint32_t holder = f->holder != 0 ? f->holder : f->owner;
    return holder != 0 && holder == c.guild->Guild().id ? best : nullptr;
}

} // namespace

// --- The split box -------------------------------------------------------------------------------

void DivideBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifmessagebox.txt")) {
        for (const auto& s : layout->sections) {
            if (s.name != "MsgBoxDivideCount") continue;
            for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui.Make(*it));
        }
    }
    if (auto* edit = ui::As<ui::Edit>(Child(17))) {
        edit->numeric = true;
        edit->emptyAllowed = true; // deleted to nothing, a new count typed
        edit->limit = 5;
        edit->passKeys = true; // Enter: Confirm; Escape: Cancel
    }
}

void DivideBox::Ask(const std::string& icon, const std::string& name, int count, std::function<void(int)> answer) {
    count_ = count;
    answer_ = std::move(answer);
    if (auto* w = ui::As<ui::Static>(Child(11))) w->texture = icon; // the item's icon in its well
    if (ui::Widget* w = Child(12)) w->SetText(name);
    if (ui::Widget* w = Child(15)) w->SetText(std::to_string(count));
    if (auto* edit = ui::As<ui::Edit>(Child(17))) {
        edit->SetValue("1");
        if (ui_ != nullptr) ui_->Focus(edit);
    }
}

void DivideBox::Update(ui::Frame&) {
    // The retained number follows the typed one (inferred).
    const auto* edit = ui::As<ui::Edit>(Child(17));
    const int typed = edit != nullptr ? std::atoi(edit->text().c_str()) : 0;
    if (ui::Widget* w = Child(15)) w->SetText(std::to_string(std::max(0, count_ - std::clamp(typed, 0, count_))));
}

bool DivideBox::OnNotice(const ui::Notice& notice) {
    const bool mine = notice.source != nullptr && notice.source->parent() == this;
    const bool confirm = mine && ((notice.kind == ui::Notice::Kind::Clicked && notice.control == 220) || notice.kind == ui::Notice::Kind::Submitted);
    const bool cancel = mine && ((notice.kind == ui::Notice::Kind::Clicked && (notice.control == 221 || notice.control == 2)) ||
                                 notice.kind == ui::Notice::Kind::Cancelled);
    if (!confirm && !cancel) return ui::MainFrame::OnNotice(notice);
    const auto* edit = ui::As<ui::Edit>(Child(17));
    const int typed = edit != nullptr ? std::atoi(edit->text().c_str()) : 0;
    auto answer = std::move(answer_);
    answer_ = nullptr;
    if (ui_ != nullptr) ui_->Close(this);
    // 1 .. count - 1 moves; anything else does nothing (observed range 0 .. count - 1, 0 sends nothing).
    if (confirm && answer && typed >= 1 && typed < count_) answer(typed);
    return true;
}

// --- The casting gauges --------------------------------------------------------------------------

void DelayGaugeBoard::Created(ui::Interface& ui) {
    // The newest gauge at the board's y + 76; its id is the type (0, a return
    // scroll) + 100.
    assets::UiLayoutNode node;
    node.type = "CIFDelayInfo";
    node.name = "GDR_DELAY_INFO";
    node.id = 100;
    node.ddj = "interface\\ifcommon\\com_casting_window.ddj";
    node.rect = {0.0f, 76.0f, 192.0f, 36.0f};
    auto made = ui.Make(node);
    gauge_ = made.get();
    gauge_->visible = false;
    if (auto* bar = ui::As<ui::Gauge>(gauge_->Child(10))) {
        bar->texture = "interface\\ifcommon\\com_casting_gauge_return.ddj";
        bar->SetValue(0.0f, true);
    }
    Add(std::move(made));
}

void DelayGaugeBoard::Update(ui::Frame&) {
    if (gauge_ == nullptr || context_.state == nullptr) return;
    const game::PlayerState::Casting& c = context_.state->casting;
    gauge_->visible = c.item != 0;
    // Without a gauge the board is empty: every click goes through to what is under it (the user's report 2026-11-14:
    // the invisible board took clicks above the underbar); with one, it takes the mouse (its Cancel button).
    clickThrough = c.item == 0;
    if (c.item == 0) {
        shownItem_ = 0;
        return;
    }
    if (c.item != shownItem_) {
        shownItem_ = c.item;
        const assets::ItemRow* row = context_.items != nullptr ? context_.items->Find(c.item) : nullptr;
        if (ui::Widget* name = gauge_->Child(12)) name->SetText(row != nullptr ? context_.Text(row->nameKey) : std::string());
    }
    // No easing: the share of the time gone, full once it is over.
    const double done = c.seconds > 0 ? (context_.time - c.started) / c.seconds : 1.0;
    if (auto* bar = ui::As<ui::Gauge>(gauge_->Child(10))) bar->SetValue(float(std::clamp(done, 0.0, 1.0)), true);
}

bool DelayGaugeBoard::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.control != 11 || notice.source == nullptr || notice.source->parent() != gauge_) return false;
    if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::CancelCasting});
    return true;
}

// --- The NPC window ------------------------------------------------------------------------------

void NpcWindow::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    ui::Widget* talk = Child(10);
    box_ = talk != nullptr ? ui::As<ui::TextBox>(talk->Child(10)) : nullptr;
    if (talk != nullptr) {
        // A plain talk: the dark tile, without the quest's paper and title strip (inferred: the
        // quest window turns them on).
        for (int id : {11, 12}) {
            if (ui::Widget* w = talk->Child(id)) w->visible = false;
        }
    }
    if (box_ != nullptr) {
        box_->hoverTracking = true;
        box_->lineHeight = 18.0f; // as the original's screenshot shows the rows
        if (talk != nullptr) box_->bar = ui::As<ui::ScrollBar>(talk->Child(6));
    }
}

void NpcWindow::Begin(const std::string& title, const std::string& greetingKey) {
    choices_.clear();
    pages_.clear();
    page_ = 0;
    held_.clear();
    if (ui::Widget* t = Child(0)) t->SetText(title);
    if (box_ == nullptr) return;
    box_->ClearParagraphs();
    if (!greetingKey.empty() && context_.hasText && context_.hasText(greetingKey)) {
        box_->AddParagraph(context_.Text(greetingKey), ui::kWhite);
        box_->AddParagraph({}, ui::kWhite);
    }
}

// "The current tax rate of %s is %d%%." in green (0xFFD6FF7E) under the greeting of an NPC or a gate a
// fortress taxes (its rate as the fortress has it, waived or not).
void NpcWindow::AddTaxLine() {
    if (box_ == nullptr || context_.npcTax == nullptr || context_.npcTax->fortress == 0) return;
    const proto::FortressRecord* f = context_.fortress != nullptr ? context_.fortress->Fortress(context_.npcTax->fortress) : nullptr;
    const std::string name = f != nullptr ? context_.Text(f->nameKey) : std::string();
    box_->AddParagraph(FormatText(context_.Text("SN_FORTRESS_SHOP_TAX"), {name, std::to_string(context_.npcTax->rate)}), ui::Argb(0xFFD6FF7E));
    box_->AddParagraph({}, ui::kWhite);
}

void NpcWindow::AddSafeTradeLine() {
    if (box_ == nullptr || context_.npcSafe == nullptr || context_.npcSafe->option != 0) return;
    if (context_.state == nullptr || context_.state->job.type != game::JobType::Trader) return;
    box_->AddParagraph(FormatText(context_.Text("UIIT_MSG_SAFETRADE_PROGRESS_NUMBER"),
                                  {std::to_string(context_.npcSafe->count), std::to_string(context_.npcSafe->limit)}),
                       ui::Argb(0xFFD6FF7E));
    box_->AddParagraph({}, ui::kWhite);
}

void NpcWindow::AddChoice(const std::string& text, Choice choice, bool red) {
    if (box_ == nullptr) return;
    // "1. Select teleport area": numbered from 1 in the order added.
    choices_[int(box_->paragraphs().size())] = choice;
    const int number = int(choices_.size());
    box_->AddParagraph(std::to_string(number) + ". " + text, red ? kChoiceRed : kChoice, ui::kTextShadow, kChoiceHover);
}

void NpcWindow::ShowGate(std::uint32_t gate, std::uint32_t services, std::vector<std::uint32_t> opened) {
    gate_ = gate;
    opened_ = std::move(opened);
    const assets::TeleportGate* g = context_.teleports != nullptr ? context_.teleports->Gate(gate) : nullptr;
    Begin(g != nullptr ? context_.Text(g->nameKey) : std::string(), g != nullptr && context_.npcGreeting ? context_.npcGreeting(g->codename) : "");
    AddTaxLine();
    // The menu by the services, in the original's order: the return point
    // (0x40, row 9), then the teleport (0x80, row 10).
    if ((services & proto::kGateRecallPoint) != 0) AddChoice(context_.Text("UIIT_CTL_RECALL_POSITION"), {Choice::Kind::RecallPoint, 0});
    if ((services & proto::kGateTeleport) != 0) AddChoice(context_.Text("UIIT_CTL_TELEPORT_TARGET"), {Choice::Kind::Destinations, 0});
    // Ours: an end to the talk on the first menu too (the original's is closed by its button).
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

void NpcWindow::ShowNpc(std::uint32_t entity, const std::string& codename, const std::string& nameKey, std::uint32_t services) {
    npc_ = entity;
    gate_ = 0;
    opened_.clear();
    npcCodename_ = codename;
    npcNameKey_ = nameKey;
    Begin(context_.Text(nameKey), context_.npcGreeting ? context_.npcGreeting(codename) : "");
    AddSafeTradeLine();
    AddTaxLine();
    // The original's order: the talk (0x2, row 4 "Start to converse."), the
    // shop (0x1), the storage (0x4), ...; ours ends with the end of the talk.
    if ((services & proto::kNpcTalk) != 0) AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKSTART"), {Choice::Kind::Talk, 0});
    // The shop: one row per tab group, its label.
    if ((services & proto::kNpcShop) != 0 && context_.shops != nullptr) {
        if (const assets::NpcShop* shop = context_.shops->Find(codename)) {
            // A guild NPC's shop is red below guild level 3.
            const bool low = (services & proto::kNpcGuild) != 0 && (context_.guild == nullptr || !context_.guild->InGuild() || context_.guild->Guild().level < 3);
            for (std::size_t g = 0; g < shop->groups.size(); ++g) AddChoice(context_.Text(shop->groups[g].labelKey), {Choice::Kind::Shop, std::uint32_t(g)}, low);
        }
    }
    if ((services & proto::kNpcStorage) != 0) {
        AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_STOREHOUSE"), {Choice::Kind::Storage, 0});
        // "Academy Establishment" after the storage, at level 60 out of an academy, then "Honor ranking" always
        // (rows 0x2D and 0x2E).
        if (context_.state != nullptr && !context_.state->academy.InAcademy() && context_.state->self.level >= game::academy::kFoundLevel) {
            AddChoice(context_.Text("UIIT_STT_TC_OPEN"), {Choice::Kind::AcademyFound, 0});
        }
        AddChoice(context_.Text("UIIT_STT_TC_HONOR_RANKING"), {Choice::Kind::AcademyRanking, 0});
    }
    // The Open Market's merchant: its row opens the market (ours, "Search Items": 1.188 carries
    // no text of its own for the row; its server answers 0x7506 with the market's state).
    if (codename == game::openmarket::kNpcCodename) AddChoice(context_.Text("UIIT_STT_OPEN_MARKET_ITEM_SEARCH"), {Choice::Kind::OpenMarket, 0});
    if ((services & proto::kNpcGuild) != 0) {
        for (const GuildNpcRowInfo& row : GuildNpcRows(context_)) AddChoice(row.text, {Choice::Kind::Guild, std::uint32_t(row.choice)}, row.red);
    }
    // A fortress's official: "Application / cancellation of fortress war" opens the war's window once the server
    // sends its data.
    if ((services & proto::kNpcFortressOfficial) != 0) AddChoice(context_.Text("SN_FORTRESS_OFFICIAL_WARAPPLY"), {Choice::Kind::Fortress, 0});
    // A battle aide: "Structure management" opens the structure map once the server sends the structures.
    if ((services & proto::kNpcFortressBattleAide) != 0) AddChoice(context_.Text("SN_FORTRESS_BATTLEAIDE_STRUCTURE"), {Choice::Kind::Fortress, 1});
    // The fortress manager (function 0x33 rows): only for the holder's guild and its union; ours has its
    // first row, "Tax management" (0x705E action 0; the window opens with the answer). Its other rows (hiring, the jobs,
    // the blessing, the guild's introduction, the GP history, the Cave of Discipline) come with their features.
    if ((services & proto::kNpcFortressManager) != 0 && FortressManagerServes(context_)) {
        // Rows 1-7: tax, employing (red for a union member), the jobs, the
        // blessing, about the guild, the GP history, the Cave of Discipline.
        const bool ownGuild = context_.guild != nullptr && context_.guild->InGuild() && context_.fortress != nullptr &&
                              std::any_of(context_.fortress->Fortresses().begin(), context_.fortress->Fortresses().end(), [&](const proto::FortressRecord& f) {
                                  return (f.holder != 0 ? f.holder : f.owner) == context_.guild->Guild().id;
                              });
        AddChoice(context_.Text("SN_FORTRESS_MANAGER_TAX"), {Choice::Kind::Fortress, 2});
        AddChoice(context_.Text("SN_FORTRESS_MANAGER_NPC"), {Choice::Kind::Fortress, 20}, !ownGuild);
        AddChoice(context_.Text("SN_FORTRESS_MANAGER_SERVICE"), {Choice::Kind::Fortress, 30});
        AddChoice(context_.Text("UIIT_STT_FORTRESS_BUFF"), {Choice::Kind::Fortress, 40});
        AddChoice(context_.Text("UIIT_PAG_GUILD_INTRODUCE"), {Choice::Kind::Fortress, 50});
        AddChoice(context_.Text("UIIT_STT_GUILD_GP_HISTORY"), {Choice::Kind::Fortress, 60});
        AddChoice(context_.Text("UIIT_STT_FORTRESS_DUNGEON"), {Choice::Kind::Fortress, 70});
    }
    // The smith's "Item production" and "Item repairing" and the trainer's "Training" (rows 0x36 1 and 2,
    // 0x37: the holder's members): the queries open CIFFortressMakeItemWnd (HudFortressWork.cpp); the repair asks first.
    if ((services & (proto::kNpcFortressSmith | proto::kNpcFortressTrainer)) != 0 && FortressManagerServes(context_)) {
        const bool trainer = (services & proto::kNpcFortressTrainer) != 0;
        AddChoice(context_.Text(trainer ? "UIIT_STT_FORT_ETC_TRAINING" : "SN_FORTRESS_SMITH_PRODUCT"), {Choice::Kind::Fortress, trainer ? 81u : 80u});
        if (!trainer) AddChoice(context_.Text("SN_FORTRESS_SMITH_REPAIR"), {Choice::Kind::Fortress, 82});
    }
    // The Cave of Discipline's teleporter inside (NPC function 0x42): "Exit" for anyone; it sends the leave
    // (0x705E 0x20), the fortress being the one kept from the manager's talk (1.188's global).
    if ((services & proto::kNpcFortressDungeon) != 0) AddChoice(context_.Text("UIIT_STT_FORTRESS_DUNGEON_EXIT"), {Choice::Kind::Fortress, 73});
    // A gate's pulley: "Open" while its gate is shut (state 0), "Close" while it is open (2), nothing else (broken).
    if ((services & proto::kNpcFortressPulley) != 0) {
        if (const proto::EntityStructure* gate = PulleyGate(context_, npc_)) {
            if (gate->state == 0) AddChoice(context_.Text("SN_FORTRESS_STRUCTURE_OPERATE_OPEN"), {Choice::Kind::Fortress, 3});
            if (gate->state == 2) AddChoice(context_.Text("SN_FORTRESS_STRUCTURE_OPERATE_CLOSE"), {Choice::Kind::Fortress, 4});
        }
    }
    // A league's keeper (functions 0x14 / 0x15 / 0x16): its rows (HudJobs.cpp).
    services_ = services;
    for (const JobNpcRowInfo& row : JobNpcRows(context_, services)) AddChoice(row.text, {Choice::Kind::Job, std::uint32_t(row.row)});
    // After the job rows, Egypt's three alliance NPCs: "Temple area Point" (row 0x3C).
    if (codename == "NPC_SD_M_AREA_MERCHANT" || codename == "NPC_SD_M_AREA_HUNTER" || codename == "NPC_SD_T_AREA_THIEF") {
        AddChoice(context_.Text("UIIT_PAG_EGYPT_SHRINE_REPUTATION"), {Choice::Kind::TempleReputation, 0});
    }
    // A specialty NPC: "Trade Export Details" (row 0x39; the price board, nothing sent).
    if ((services & proto::kNpcTrade) != 0) AddChoice(context_.Text("UIIT_PAG_SPECIAL_PRICE_CONDITION_INFO"), {Choice::Kind::Job, std::uint32_t(JobNpcRow::TradeBoard)});
    // Magic POP (rows 0x27, 0x28): the machine's "Participate in the game.", Gori's
    // "Exchange Item Exchange Coupon to Item".
    if ((services & proto::kNpcPopMachine) != 0) AddChoice(context_.Text("SN_TALK_CH_GACHA_MACHINE_2"), {Choice::Kind::MagicPop, 0});
    if ((services & proto::kNpcPopExchange) != 0) AddChoice(context_.Text("SN_TALK_CH_GACHA_OPERATOR_2"), {Choice::Kind::MagicPop, 1});
    // A party recall pillar: "Summon Party member" (NPC function 0x27 -> row 0x3D).
    if ((services & proto::kNpcPartySummon) != 0) AddChoice(context_.Text("UIIT_PAG_PARTY_SUMMON"), {Choice::Kind::PartySummon, 0});
    // An NPC with a teleport point of its own (a ferry's ticket seller): a gate's rows, the return point then the teleport.
    if ((services & proto::kGateRecallPoint) != 0) AddChoice(context_.Text("UIIT_CTL_RECALL_POSITION"), {Choice::Kind::RecallPoint, 0});
    if ((services & proto::kGateTeleport) != 0) AddChoice(context_.Text("UIIT_CTL_TELEPORT_TARGET"), {Choice::Kind::Destinations, 0});
    // Event So-Ok's registration row after the NPC's ordinary rows, by the state her desk told: 1 "Capture the Flag
    // Match Registration", 2 its cancel, 0 none; drawing it sets which answer is expected. Its colours are the rows'
    // (0xFFEFDAA4, 0xFFFF8A00 under the mouse).
    if (context_.flag != nullptr && context_.flag->Desk().npc == entity) {
        const std::uint8_t state = context_.flag->Desk().flagState;
        if (const std::string_view key = game::FlagView::DeskRowKey(state); !key.empty()) {
            context_.flag->RowDrawn(state);
            AddChoice(context_.Text(std::string(key)), {Choice::Kind::FlagDesk, state});
        }
    }
    // The Battle Arena manager's row by the state its desk told: registered "Cancel Registration", else
    // the open mode's name; drawn as the rows are (0xFFEFDAA4, 0xFFFF8A00 under the mouse).
    if (context_.arena != nullptr && context_.arena->Desk().npc == entity) {
        if (const auto row = game::ArenaView::DeskRowOf(context_.arena->Desk().state)) {
            AddChoice(context_.Text(row->key), {Choice::Kind::ArenaDesk, context_.arena->Desk().state});
        }
    }
    // No "End conversation." row on the first menu: the original's ends the talk by the window's button; its sub-menus
    // (a talk, the destinations, ...) end with that row.
}

void NpcWindow::ShowEventDesk() {
    if (npc_ != 0 && gate_ == 0 && context_.flag != nullptr && context_.flag->Desk().npc == npc_) ShowNpc(npc_, npcCodename_, npcNameKey_, services_);
}

void NpcWindow::ShowArenaDesk() {
    if (npc_ != 0 && gate_ == 0 && context_.arena != nullptr && context_.arena->Desk().npc == npc_) ShowNpc(npc_, npcCodename_, npcNameKey_, services_);
}

void NpcWindow::ShowEventSays(const proto::EventSays& says) {
    if (says.npc != npc_ || box_ == nullptr) return;
    // The original's dialogue: its text, then its keys as rows "%d. %s" (a key the text
    // tables lack shows empty: 070919's row, SN_TALK_QEV_CH_EVENT_KISAENG_070910_B).
    const auto said = [&](const std::string& key) { return context_.hasText && context_.hasText(key) ? context_.Text(key) : std::string(); };
    choices_.clear();
    pages_.clear();
    page_ = 0;
    held_.clear();
    if (ui::Widget* title = Child(0)) title->SetText(context_.Text(npcNameKey_));
    box_->ClearParagraphs();
    if (!says.text.empty()) {
        box_->AddParagraph(said(says.text), ui::kWhite);
        box_->AddParagraph({}, ui::kWhite);
    }
    for (std::size_t i = 0; i < says.rows.size(); ++i) {
        if (says.rows[i] == game::kTalkExit) AddChoice(said(says.rows[i]), {Choice::Kind::EventExit, says.quest});
        else AddChoice(said(says.rows[i]), {Choice::Kind::EventRow, std::uint32_t(says.quest) << 8 | std::uint32_t(i)});
    }
}

void NpcWindow::ShowTalk() {
    // What the NPC says when talked to, then its quests: in 1.188 the talk is the server's
    // dialogue: a text, then a numbered list ("%d. %s", picked as row + 5). Ours: the
    // quests' greeting at this NPC (their `greeting` talk, SN_NPC_*_QS; else npcchat.txt's second text), each quest
    // of the NPC's rows by its name, then the end of the talk.
    const game::QuestView* qv = context_.quests;
    const bool listed = qv != nullptr && qv->NpcRows().npc == npc_ && !qv->NpcRows().rows.empty();
    std::string greeting;
    if (listed) {
        for (const auto& row : qv->NpcRows().rows) {
            if (const game::QuestDef* q = qv->Find(row.quest)) {
                const game::QuestText t = game::QuestView::TalkOf(*q, npcCodename_, "greeting");
                if (!t.Empty()) {
                    greeting = QuestTextOf(t);
                    break;
                }
            }
        }
    }
    choices_.clear();
    if (ui::Widget* title = Child(0)) title->SetText(context_.Text(npcNameKey_));
    if (box_ == nullptr) return;
    box_->ClearParagraphs();
    if (greeting.empty() && context_.npcTalk) {
        const std::string key = context_.npcTalk(npcCodename_);
        if (!key.empty() && context_.hasText && context_.hasText(key)) greeting = context_.Text(key);
    }
    if (!greeting.empty()) {
        box_->AddParagraph(greeting, ui::kWhite);
        box_->AddParagraph({}, ui::kWhite);
    }
    if (listed) {
        // Each row coloured by the player's level against the quest's: under it red, 10 or more
        // above it 0xFF65AFA2, else the plain rows' colour.
        const int level = context_.state != nullptr ? context_.state->self.level : 1;
        for (const auto& row : qv->NpcRows().rows) {
            const game::QuestDef* q = qv->Find(row.quest);
            if (q == nullptr || box_ == nullptr) continue;
            const int gap = level - q->level;
            choices_[int(box_->paragraphs().size())] = {Choice::Kind::Quest, row.quest};
            const ui::Color colour = gap < 0 ? kChoiceRed : gap >= 10 ? ui::Argb(0xFF65AFA2) : kChoice;
            box_->AddParagraph(std::to_string(choices_.size()) + ". " + QuestTextOf(q->name), colour, ui::kTextShadow, kChoiceHover);
        }
    }
    // Event So-Ok's coded quests: their titles (textevent.txt) in her list, as the original's dialogue
    // lists keys; no quest id travels in the original, ours names the quest in EventTalk.
    if (context_.flag != nullptr && context_.flag->Desk().npc == npc_) {
        for (const proto::EventDesk::Row& row : context_.flag->Desk().quests) AddChoice(context_.Text(row.title), {Choice::Kind::EventQuest, row.quest});
    }
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

std::string NpcWindow::QuestTextOf(const game::QuestText& t) const {
    if (!t.key.empty()) return context_.hasText && context_.hasText(t.key) ? context_.Text(t.key) : t.key;
    return t.literal;
}

void NpcWindow::ShowOutcome(const std::string& text, bool collect) {
    choices_.clear();
    if (box_ == nullptr) return;
    box_->ClearParagraphs();
    box_->AddParagraph(text, ui::kWhite);
    box_->AddParagraph({}, ui::kWhite);
    // Row 0x25 "Collect this week's outcome" (only when N is not 0), then the end.
    if (collect) AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_HUNTERMENU_OUTCOME"), {Choice::Kind::Job, std::uint32_t(JobNpcRow::OutcomeCollect)});
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

void NpcWindow::ShowQuestSays(const proto::QuestSays& says) {
    const game::QuestView* qv = context_.quests;
    const game::QuestDef* q = qv != nullptr ? qv->Find(says.quest) : nullptr;
    if (q == nullptr || says.npc != npc_) return;
    // A talk of several pages still being read: this one waits for its end.
    if (page_ + 1 < pages_.size()) {
        held_.push_back(says);
        return;
    }
    quest_ = says.quest;
    picked_.clear();
    // The quest's talk for the moment (each of its lines a page), or the goal's own (a delivery's, a talk's); when it
    // has none for an offer, the quest's name says what it is.
    std::vector<game::QuestText> said;
    if (!says.text.empty()) {
        said.push_back(game::QuestText{says.text, {}}); // a talk game's own words
    } else if (says.moment.empty()) {
        if (says.goal < q->goals.size() && !q->goals[says.goal].talk.Empty()) said.push_back(q->goals[says.goal].talk);
    } else {
        said = game::QuestView::TalksOf(*q, npcCodename_, says.moment);
        if (said.empty() && says.moment == "offer-again") said = game::QuestView::TalksOf(*q, npcCodename_, "offer");
        if (said.empty() && (says.moment == "done-now" || says.moment == "finished")) said.push_back(game::QuestText{"SN_TALK_COMMON_END", {}});
    }
    if (said.empty() && says.moment.rfind("offer", 0) == 0) said.push_back(q->name);
    paging_ = says;
    pages_.clear();
    for (const game::QuestText& t : said) pages_.push_back(QuestTextOf(t));
    page_ = 0;
    ShowQuestPage();
}

void NpcWindow::ShowQuestPage() {
    const proto::QuestSays& says = paging_;
    choices_.clear();
    if (ui::Widget* title = Child(0)) title->SetText(context_.Text(npcNameKey_));
    if (box_ == nullptr) return;
    box_->ClearParagraphs();
    if (page_ < pages_.size()) {
        box_->AddParagraph(pages_[page_], ui::kWhite);
        box_->AddParagraph({}, ui::kWhite);
    }
    // Pages to come, or a talk held meanwhile: "Next" (the original's SN_TALK_COMMON_NEXT).
    if (page_ + 1 < pages_.size() || (!held_.empty() && says.choice == proto::QuestChoice::None)) {
        AddChoice(context_.Text("SN_TALK_COMMON_NEXT"), {Choice::Kind::QuestNext, 0});
        return;
    }
    // The answers as the original's talk lists them (numbers the server's list of text keys; its own
    // UIIT_STT_YES / _NO / _CONFIRM are in none of 1.188's tables): the quest tables' common words, an offer
    // "Accept" / "Refuse", a finish "Receive reward.", a goal handed in "OK", else "End conversation.".
    switch (says.choice) {
    case proto::QuestChoice::Accept:
        AddChoice(context_.Text("SN_TALK_COMMON_ACCEPT"), {Choice::Kind::QuestYes, says.quest});
        AddChoice(context_.Text("SN_TALK_COMMON_DENY"), {Choice::Kind::QuestNo, says.quest});
        break;
    case proto::QuestChoice::HandIn: {
        // An exchange (QSP_ALL_POTION_3): its rows instead of "Receive reward" (the original's five lines, 1.188 talk rows
        // from QSP_ALL_POTION_3's dialogue), then the end of the talk.
        const game::QuestDef* q = context_.quests != nullptr ? context_.quests->Find(says.quest) : nullptr;
        if (q != nullptr && !q->tiers.empty() && says.moment == "done") {
            for (std::size_t k = 0; k < q->tiers.size(); ++k) {
                const game::QuestDef::Tier& t = q->tiers[k];
                std::string text = QuestTextOf(t.talk);
                if (text.empty()) {
                    const assets::ItemRow* row = context_.items != nullptr ? context_.items->Find(std::string_view(t.item)) : nullptr;
                    text = std::format("{} {}   {} SP", row != nullptr ? context_.Text(row->nameKey) : t.item, t.count, t.sp);
                }
                AddChoice(text, {Choice::Kind::QuestTier, std::uint32_t(k)});
            }
            AddChoice(context_.Text("SN_TALK_COMMON_EXIT"), {Choice::Kind::End, 0});
            break;
        }
        AddChoice(context_.Text(says.moment == "done" ? "SN_TALK_COMMON_REWARD" : "SN_TALK_COMMON_OK"), {Choice::Kind::QuestHandIn, says.quest});
        break;
    }
    case proto::QuestChoice::None:
        AddChoice(context_.Text("SN_TALK_COMMON_EXIT"), {Choice::Kind::End, 0});
        break;
    case proto::QuestChoice::Rows:
        // A talk game's rows (the original's own lists, e.g. QNO_CH_BIGMAN_1's drinks): the one picked goes back by index.
        for (std::size_t k = 0; k < says.rows.size(); ++k) AddChoice(context_.Text(says.rows[k]), {Choice::Kind::QuestTier, std::uint32_t(k)});
        break;
    }
}

// The reward's items to pick from (ours; 1.188's choice window, "You must select your reward.", is not read yet):
// each item a row, picked until the quest's count.
void NpcWindow::ShowRewardChoice() {
    const game::QuestDef* q = context_.quests != nullptr ? context_.quests->Find(quest_) : nullptr;
    if (q == nullptr || box_ == nullptr) return;
    choices_.clear();
    box_->ClearParagraphs();
    box_->AddParagraph(context_.Text("UIIT_STT_QUESTREWARD_MSG01"), ui::kWhite);
    box_->AddParagraph({}, ui::kWhite);
    for (std::size_t i = 0; i < q->reward.items.size(); ++i) {
        if (std::find(picked_.begin(), picked_.end(), std::uint8_t(i)) != picked_.end()) continue;
        const auto& item = q->reward.items[i];
        const assets::ItemRow* row = context_.items != nullptr ? context_.items->Find(std::string_view(item.item)) : nullptr;
        std::string name = row != nullptr ? context_.Text(row->nameKey) : item.item;
        if (item.count > 1) name += " x" + std::to_string(item.count);
        AddChoice(name, {Choice::Kind::QuestReward, std::uint32_t(i)});
    }
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

void NpcWindow::ShowHire() {
    Begin(context_.Text("SN_FORTRESS_MANAGER_NPC"), "");
    if (box_ != nullptr) {
        box_->AddParagraph(context_.Text("UIIT_STT_FORT_MANAGER_HIRE"), ui::kWhite);
        box_->AddParagraph({}, ui::kWhite);
    }
    std::uint32_t fortress = 0;
    if (context_.fortress != nullptr && context_.guild != nullptr && context_.guild->InGuild()) {
        for (const proto::FortressRecord& f : context_.fortress->Fortresses()) {
            if ((f.holder != 0 ? f.holder : f.owner) == context_.guild->Guild().id) fortress = f.id;
        }
    }
    const std::uint8_t hired = context_.fortress != nullptr ? context_.fortress->Hired(fortress) : 0;
    const std::string fee = FormatText(context_.Text("SN_FORTRESS_MANAGER_EMPLOY_FEE"), {"30000", "3000"});
    const std::pair<const char*, std::uint32_t> rows[] = {{"SN_FORTRESS_MANAGER_EMPLOY_BATTLEAIDE", 1}, {"SN_FORTRESS_MANAGER_EMPLOY_SMITH", 2}, {"SN_FORTRESS_MANAGER_EMPLOY_TRAINER", 4}};
    for (std::size_t i = 0; i < 3; ++i) {
        AddChoice(context_.Text(rows[i].first) + " " + fee, {Choice::Kind::Fortress, std::uint32_t(21 + i)}, (hired & rows[i].second) != 0);
    }
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

void NpcWindow::ShowDungeon() {
    // The Cave of Discipline's page (1.188's rows UIIT_STT_FORTRESS_DUNGEON_*; the state as the last 0x1D answer told it).
    Begin(context_.Text("UIIT_STT_FORTRESS_DUNGEON"), "");
    const proto::FortressDungeon* d = context_.fortress != nullptr ? &context_.fortress->Dungeon() : nullptr;
    if (box_ != nullptr) {
        box_->AddParagraph(context_.Text("UIIT_MSG_FORTRESS_DUNGEON_INTRODUCE"), ui::kWhite);
        box_->AddParagraph(context_.Text(d != nullptr && d->open ? "UIIT_MSG_FORTRESS_DUNGEON_INTRODUCE_CREATED" : "UIIT_MSG_FORTRESS_DUNGEON_INTRODUCE_NOT_CREATED"),
                           ui::kWhite);
        box_->AddParagraph({}, ui::kWhite);
    }
    AddChoice(context_.Text("UIIT_STT_FORTRESS_DUNGEON_CREATE"), {Choice::Kind::Fortress, 71});
    AddChoice(FormatText(context_.Text("UIIT_STT_FORTRESS_DUNGEON_ENTER"), {std::to_string(d != nullptr ? d->players : 0), "30"}), {Choice::Kind::Fortress, 72});
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

void NpcWindow::ShowBlessings() {
    // "Which blessing buff will you receive?" and the fortress's rows "%s Buff (%d Gold, %d GP req'd)" (the client's own
    // refsiegeblessbuff.txt; ours: the rows the server named in the list's blessings, BlessID 1..).
    Begin(context_.Text("UIIT_STT_FORTRESS_BUFF"), "");
    if (box_ != nullptr) {
        box_->AddParagraph(context_.Text("UIIT_MSG_FORTRESS_BUFF_SELECT"), ui::kWhite);
        box_->AddParagraph({}, ui::kWhite);
    }
    std::uint32_t fortress = 0;
    if (context_.fortress != nullptr && context_.guild != nullptr && context_.guild->InGuild()) {
        for (const proto::FortressRecord& f : context_.fortress->Fortresses()) {
            if ((f.holder != 0 ? f.holder : f.owner) == context_.guild->Guild().id) fortress = f.id;
        }
    }
    if (context_.worldMap != nullptr) {
        for (const assets::FortressBlessRow& b : context_.worldMap->blessings) {
            if (std::uint32_t(b.fortress) != fortress || b.id <= 0 || b.id > 9) continue;
            const assets::SkillRow* skill = context_.skills != nullptr ? context_.skills->Find(b.skill) : nullptr;
            const std::string name = skill != nullptr ? context_.Text(skill->nameKey) : std::to_string(b.skill);
            AddChoice(FormatText(context_.Text("UIIT_STT_FORTRESS_BUFF_NAME"), {name, std::to_string(b.gold), std::to_string(b.guildPoints)}),
                      {Choice::Kind::Fortress, std::uint32_t(40 + b.id)});
        }
    }
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

void NpcWindow::ShowDestinations() {
    const assets::TeleportGate* g = context_.teleports != nullptr && gate_ != 0 ? context_.teleports->Gate(gate_) : nullptr;
    const assets::TeleportPoint* from = g != nullptr ? context_.teleports->PointOf(g->refObjId) : nullptr;
    if (g != nullptr) {
        Begin(context_.Text(g->nameKey), context_.npcGreeting ? context_.npcGreeting(g->codename) : "");
    } else {
        // An NPC's own point (a ferry's seller): its model's.
        const assets::ObjectRow* row = context_.characters != nullptr ? context_.characters->Find(std::string_view(npcCodename_)) : nullptr;
        from = row != nullptr && context_.teleports != nullptr ? context_.teleports->PointOf(row->refObjId) : nullptr;
        Begin(context_.Text(npcNameKey_), context_.npcGreeting ? context_.npcGreeting(npcCodename_) : "");
    }
    if (from == nullptr) return;
    // Every place the point leads to with its fee, then the end of the talk:
    // "Teleport to [Donwhang]area. [5,000]gold", or without the fee when it is free.
    for (const assets::TeleportLink* link : context_.teleports->LinksFrom(from->id)) {
        const assets::TeleportPoint* to = context_.teleports->Point(link->to);
        if (to == nullptr) continue;
        const std::string place = context_.Text(to->nameKey);
        // The fare with the gate's fortress tax; a free one untaxed.
        const std::string text = link->fee > 0 ? FormatText(context_.Text("UIIT_CTL_TELEPORT_RESULT"), {place, GoldText(context_.TaxedBuy(link->fee))})
                                               : FormatText(context_.Text("UIIT_CTL_TELEPORT_FREE_RESULT"), {place});
        AddChoice(text, {Choice::Kind::Destination, link->to});
    }
    // Then those the server opened for the gate at run time, free (the snake gate's into the tomb's 5th floor).
    for (const std::uint32_t id : g != nullptr ? opened_ : std::vector<std::uint32_t>{}) {
        const assets::TeleportPoint* to = context_.teleports->Point(id);
        if (to != nullptr) AddChoice(FormatText(context_.Text("UIIT_CTL_TELEPORT_FREE_RESULT"), {context_.Text(to->nameKey)}), {Choice::Kind::Destination, id});
    }
    AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
}

bool NpcWindow::Choose(int number) {
    if (box_ == nullptr || number < 1) return false;
    int n = 0;
    for (const auto& [paragraph, choice] : choices_) {
        if (++n == number) return OnNotice({ui::Notice::Kind::RowPicked, box_->id(), box_, paragraph});
    }
    return false;
}

bool NpcWindow::ChooseJob(JobNpcRow row) {
    int n = 0;
    for (const auto& [paragraph, choice] : choices_) {
        ++n;
        if (choice.kind == Choice::Kind::Job && choice.destination == std::uint32_t(row)) return Choose(n);
    }
    return false;
}

bool NpcWindow::ChooseText(const std::string& text) {
    if (box_ == nullptr) return false;
    // The rows read "N. text": the one whose text follows its number.
    int n = 0;
    for (const auto& [paragraph, choice] : choices_) {
        ++n;
        const std::string row = box_->paragraphs()[std::size_t(paragraph)].text;
        const std::string prefix = std::to_string(n) + ". ";
        if (row == prefix + text) return Choose(n);
    }
    return false;
}

bool NpcWindow::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::RowPicked || box_ == nullptr || notice.source != box_) return ui::MainFrame::OnNotice(notice);
    const auto it = choices_.find(notice.value);
    if (it == choices_.end()) return true;
    const Choice choice = it->second;
    const auto close = [&] {
        if (context_.hud != nullptr) context_.hud->Hide(hud::kNpc);
    };
    switch (choice.kind) {
    case Choice::Kind::Destinations: ShowDestinations(); break;
    case Choice::Kind::Talk: ShowTalk(); break;
    case Choice::Kind::Quest:
        if (context_.questRequest) context_.questRequest(proto::QuestTalk::kId, proto::Encode(proto::QuestTalk{npc_, choice.destination}));
        break;
    case Choice::Kind::FlagDesk:
        // Either row (the toggle): under level 20 its line and nothing sent, else the request.
        close();
        if (const auto refusal = game::FlagView::RowRefusal(context_.state != nullptr ? int(context_.state->self.level) : 1)) {
            if (context_.flagLine) context_.flagLine(*refusal);
        } else if (context_.eventRequest) {
            context_.eventRequest(proto::FlagWorldRegister::kId, proto::Encode(proto::FlagWorldRegister{}));
        }
        break;
    case Choice::Kind::ArenaDesk: {
        // Registered: the cancel at once (0x74D3 {2}). Else the open mode's talk with its one row (1.188:
        // action 0x3A).
        const auto row = game::ArenaView::DeskRowOf(std::uint16_t(choice.destination));
        if (!row) break;
        if (row->cancel) {
            close();
            proto::ArenaRegister cancel;
            cancel.action = 2;
            if (context_.eventRequest) context_.eventRequest(proto::ArenaRegister::kId, proto::Encode(cancel));
            break;
        }
        Begin(context_.Text(npcNameKey_), std::string(game::ArenaView::ModeTalk(row->mode)));
        AddChoice(context_.Text("UIIT_STT_ARENA_NPC_JOIN"), {Choice::Kind::ArenaJoin, choice.destination});
        AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
        break;
    }
    case Choice::Kind::ArenaJoin: {
        // "Arena Registration" (action 0x3B): the guild window for the guild mode, else the rule window, with the kinds.
        const auto row = game::ArenaView::DeskRowOf(std::uint16_t(choice.destination));
        close();
        if (!row) break;
        if (row->mode == std::uint8_t(game::arena::Mode::Guild)) OpenArenaGuild(context_, row->kinds);
        else OpenArenaRule(context_, row->mode, row->kinds);
        break;
    }
    case Choice::Kind::EventQuest:
        if (context_.eventRequest) context_.eventRequest(proto::EventTalk::kId, proto::Encode(proto::EventTalk{npc_, std::uint16_t(choice.destination), 0xFF}));
        break;
    case Choice::Kind::EventRow:
        if (context_.eventRequest) {
            context_.eventRequest(proto::EventTalk::kId, proto::Encode(proto::EventTalk{npc_, std::uint16_t(choice.destination >> 8), std::uint8_t(choice.destination & 0xFF)}));
        }
        break;
    case Choice::Kind::EventExit:
        if (context_.eventRequest) context_.eventRequest(proto::EventTalk::kId, proto::Encode(proto::EventTalk{npc_, std::uint16_t(choice.destination), 1}));
        close();
        break;
    case Choice::Kind::QuestYes:
        if (context_.questRequest) context_.questRequest(proto::QuestAccept::kId, proto::Encode(proto::QuestAccept{npc_, choice.destination}));
        break;
    case Choice::Kind::QuestNext:
        if (page_ + 1 < pages_.size()) {
            ++page_;
            ShowQuestPage();
        } else if (!held_.empty()) {
            const proto::QuestSays next = held_.front();
            held_.pop_front();
            pages_.clear();
            page_ = 0;
            ShowQuestSays(next);
        }
        break;
    case Choice::Kind::QuestNo: {
        // The NPC's words for a refusal, then the end of the talk.
        proto::QuestSays denied{npc_, choice.destination, "denied", 0, proto::QuestChoice::None};
        ShowQuestSays(denied);
        break;
    }
    case Choice::Kind::QuestHandIn: {
        // Finishing it here with a reward to choose: the choice first.
        const game::QuestDef* q = context_.quests != nullptr ? context_.quests->Find(choice.destination) : nullptr;
        bool finishing = false;
        if (q != nullptr && game::QuestFinishesAt(*q, npcCodename_)) {
            const auto under = context_.quests->Active().find(choice.destination);
            finishing = under != context_.quests->Active().end() && under->second.status == proto::QuestStatus::Achieved;
        }
        if (finishing && q->reward.choose > 0) {
            // The reward to choose: 1.188's reward window with its items and its "Reward" button (CNIFQuestReward).
            OpenQuestReward(context_, choice.destination, npc_);
            break;
        }
        if (context_.questRequest) context_.questRequest(proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{npc_, choice.destination, picked_}));
        break;
    }
    case Choice::Kind::QuestTier: {
        // The row picked: the hand-in with its index (the server takes its items and pays its SP, or says it is short).
        const std::vector<std::uint8_t> row{std::uint8_t(choice.destination)};
        if (context_.questRequest) context_.questRequest(proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{npc_, quest_, row}));
        break;
    }
    case Choice::Kind::QuestReward: {
        picked_.push_back(std::uint8_t(choice.destination));
        const game::QuestDef* q = context_.quests != nullptr ? context_.quests->Find(quest_) : nullptr;
        if (q != nullptr && picked_.size() >= q->reward.choose) {
            if (context_.questRequest) context_.questRequest(proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{npc_, quest_, picked_}));
        } else {
            ShowRewardChoice();
        }
        break;
    }
    case Choice::Kind::Shop:
        // The store of that tab group, beside the inventory, once the server says it is open.
        close();
        if (context_.hud != nullptr) {
            HudRequest r{HudRequest::Kind::OpenShop, int(npc_)};
            r.extra = int(choice.destination);
            context_.hud->Request(r);
        }
        break;
    case Choice::Kind::Destination:
        // At once, the fee in the line being the price (row 10).
        close();
        if (context_.hud != nullptr) {
            HudRequest r{HudRequest::Kind::GateTeleport, int(gate_ != 0 ? gate_ : npc_)}; // a gate, or an NPC's point by its entity
            r.extra = int(choice.destination);
            context_.hud->Request(r);
        }
        break;
    case Choice::Kind::RecallPoint:
        // The window closes and the question comes up (row 9, box kind 5).
        close();
        if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::GateDesignate, int(gate_ != 0 ? gate_ : npc_)});
        break;
    case Choice::Kind::Storage:
        close();
        if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::OpenStorage, int(npc_)});
        break;
    case Choice::Kind::Guild:
        close();
        if (ui_ != nullptr) GuildNpcChoice(*ui_, context_, int(choice.destination), npc_);
        break;
    case Choice::Kind::AcademyRanking:
        // The list asked with the NPC (0x7478); its answer opens the ranking window.
        close();
        if (context_.academyRequest) context_.academyRequest(proto::AcademyRankingAsk::kId, proto::Encode(proto::AcademyRankingAsk{npc_}));
        break;
    case Choice::Kind::TempleReputation:
        // Asked with the NPC (0x74DF); the answer opens CNIFConfirmReputation.
        close();
        if (context_.instanceRequest) context_.instanceRequest(proto::TempleReputationAsk::kId, proto::Encode(proto::TempleReputationAsk{npc_}));
        break;
    case Choice::Kind::OpenMarket:
        // Asked with the NPC (0x7506); the state's answer opens the window.
        close();
        if (context_.openMarketRequest) context_.openMarketRequest(proto::OpenMarketOpen::kId, proto::Encode(proto::OpenMarketOpen{npc_}));
        break;
    case Choice::Kind::AcademyFound:
        // The fee's question (box 6: "The fee is 100,000 gold..."), then the founding.
        close();
        if (context_.hud != nullptr) {
            HudContext* c = &context_;
            const std::uint32_t keeper = npc_;
            context_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {context_.Text("UIIT_MSG_TC_TRAININGCAMP_OPEN")}, [c, keeper](bool yes) {
                if (yes && c->academyRequest) c->academyRequest(proto::AcademyFound::kId, proto::Encode(proto::AcademyFound{keeper}));
            });
        }
        break;
    case Choice::Kind::Job:
        // The outcome's rows keep the talk open for its answer (it replaces the talk or closes it).
        if (const auto row = JobNpcRow(choice.destination); row != JobNpcRow::Outcome && row != JobNpcRow::OutcomeCollect) close();
        if (ui_ != nullptr) JobNpcChoice(*ui_, context_, JobNpcRow(choice.destination), npc_, services_);
        break;
    case Choice::Kind::MagicPop:
        if (choice.destination == 0) {
            // The machine: its window (the original asks first with the NPC function 0x11 and opens on the answer; ours opens at
            // once, the play's request carries the checks).
            close();
            OpenMagicPop(context_, npc_);
        } else if (MagicPopHasCoupon(context_)) {
            // Gori with a coupon in the bag: the window closes and the exchange goes (0x7119).
            close();
            if (context_.popRequest) context_.popRequest(proto::PopExchange::kId, proto::Encode(proto::PopExchange{npc_}));
        } else if (box_ != nullptr) {
            // Without one: his words for it, nothing sent.
            choices_.clear();
            box_->ClearParagraphs();
            box_->AddParagraph(context_.Text("SN_TALK_CH_GACHA_OPERATOR_3"), ui::kWhite);
            box_->AddParagraph({}, ui::kWhite);
            AddChoice(context_.Text("UIIT_STT_NPC_CHATTING_WND_TALKEND"), {Choice::Kind::End, 0});
        }
        break;
    case Choice::Kind::PartySummon:
        close();
        if (context_.instanceRequest) context_.instanceRequest(proto::OpenSummon::kId, proto::Encode(proto::OpenSummon{npc_}));
        break;
    case Choice::Kind::Fortress: {
        // A fortress NPC's rows: the request goes with the NPC; a row that opens a window opens it beside
        // the NPC's window, which stays (the NPC window's close closes them, Hud::Sync).
        context_.fortressNpc = npc_;
        const auto send = [&](proto::FortressAction action, std::uint32_t value = 0) {
            if (context_.fortressRequest) context_.fortressRequest(proto::FortressManage::kId, proto::Encode(proto::FortressManage{npc_, action, 0, value}));
        };
        HudContext* ctx = &context_;
        const std::uint32_t npc = npc_;
        const auto ask = [&](const std::string& text, proto::FortressAction action, std::uint32_t value) {
            // The NPC window's question box: "Confirmation window" with its text, then the request.
            if (context_.hud == nullptr) return;
            std::vector<std::string> lines;
            std::string rest = text;
            for (std::size_t at; (at = rest.find('\n')) != std::string::npos;) rest.replace(at, 1, "\\n"); // a real break as the table's
            for (std::size_t at; (at = rest.find("\\n")) != std::string::npos;) {
                lines.push_back(rest.substr(0, at));
                rest = rest.substr(at + 2);
            }
            lines.push_back(rest);
            context_.hud->Confirm("UIIT_STT_CONFIRM_BOX", lines, [ctx, npc, action, value](bool yes) {
                if (yes && ctx->fortressRequest) ctx->fortressRequest(proto::FortressManage::kId, proto::Encode(proto::FortressManage{npc, action, 0, value}));
            });
        };
        if (choice.destination == 20) {
            // "Who will you employ?": the three, a hired one drawn as not offered.
            send(proto::FortressAction::Hired);
            ShowHire();
            return true;
        }
        if (choice.destination == 40) {
            ShowBlessings();
            return true;
        }
        if (choice.destination == 70) {
            send(proto::FortressAction::DungeonQuery);
            ShowDungeon();
            return true;
        }
        if (choice.destination >= 21 && choice.destination <= 23) {
            // Only the Commander's click asks, and only for one not hired: "Do you want to employ [%s]? [%d]gold
            // and [%d]GP are needed for employing." with the NPC's name, 30,000 and 3,000 (kinds 8, 9, 10).
            const std::uint32_t bit = choice.destination == 21 ? 1u : choice.destination == 22 ? 2u : 4u;
            const proto::GuildMemberRecord* self = context_.guild != nullptr ? context_.guild->Self() : nullptr;
            std::uint32_t fortress = 0;
            if (context_.fortress != nullptr && context_.guild != nullptr && context_.guild->InGuild()) {
                for (const proto::FortressRecord& f : context_.fortress->Fortresses()) {
                    if ((f.holder != 0 ? f.holder : f.owner) == context_.guild->Guild().id) fortress = f.id;
                }
            }
            const bool hired = context_.fortress != nullptr && (context_.fortress->Hired(fortress) & bit) != 0;
            if (self == nullptr || self->position != game::kPositionCommander || hired) return true;
            const char* name = bit == 1 ? "SN_FORTRESS_MANAGER_NPC_NAME_BATTLEAID" : bit == 2 ? "SN_FORTRESS_MANAGER_NPC_NAME_SMITH" : "SN_FORTRESS_MANAGER_NPC_NAME_TRAINER";
            ask(FormatText(context_.Text("UIIT_MSG_FORT_MANAGER_EMPLOY_WINDOW"), {context_.Text(name), "30000", "3000"}), proto::FortressAction::Hire, bit);
            return true;
        }
        if (choice.destination > 40 && choice.destination < 50) {
            // A blessing's row asks with its own text, "%s Buff (%d Gold, %d GP req'd)" (box kind 0x0B), then 0x1C.
            const std::string row = box_ != nullptr ? box_->paragraphs()[std::size_t(notice.value)].text : std::string();
            const std::size_t dot = row.find(". ");
            ask(dot != std::string::npos ? row.substr(dot + 2) : row, proto::FortressAction::Bless, choice.destination - 40);
            return true;
        }
        if (choice.destination == 71 || choice.destination == 72 || choice.destination == 73) {
            const proto::FortressDungeon* d = context_.fortress != nullptr ? &context_.fortress->Dungeon() : nullptr;
            if (choice.destination == 71) {
                // Create: sent at once, only while none is open.
                if (d != nullptr && d->open) return true;
                close();
                send(proto::FortressAction::DungeonCreate);
                return true;
            }
            if (choice.destination == 72) {
                // Enter, while one is open: a first entry asks for its price (UIIT_STT_GUILD_DUNGEON_CREATE_GP_GOLD_CONSUME with
                // refsiegedungeon.txt's EntryGold and EntryGP, box kind 0x0C); one admitted goes in at once.
                if (d == nullptr || !d->open) return true;
                if (d->admitted) {
                    close();
                    send(proto::FortressAction::DungeonEnter);
                    return true;
                }
                std::uint64_t gold = 0;
                std::uint32_t points = 0;
                if (context_.worldMap != nullptr) {
                    for (const assets::FortressDungeonRow& r : context_.worldMap->caves) {
                        if (std::uint32_t(r.fortress) == d->fortress) {
                            gold = r.gold;
                            points = r.guildPoints;
                        }
                    }
                }
                ask(FormatText(context_.Text("UIIT_STT_GUILD_DUNGEON_CREATE_GP_GOLD_CONSUME"), {std::to_string(gold), std::to_string(points)}),
                    proto::FortressAction::DungeonEnter, 0);
                return true;
            }
            close();
            send(proto::FortressAction::DungeonLeave);
            return true;
        }
        if (!context_.fortressRequest) break;
        if (choice.destination >= 30 && choice.destination < 70) {
            // The jobs (5), the introduction (0x19), the GP history (0x1B): their windows open beside this one.
            send(choice.destination == 30 ? proto::FortressAction::Schedule : choice.destination == 50 ? proto::FortressAction::Introduction : proto::FortressAction::GpHistory);
            break;
        }
        if (choice.destination == 82) {
            // "Item repairing": the player's guild the holder's again (else 0x4C1E's line), the make window
            // closed, the price of the worn places 0..7 and the bag, each piece's; nothing to
            // mend: "No item needs repairing."; else box kind 0x0C, "Repair all" with the price, whose yes sends the repair
            // of all (0x703E {NPC, 2}).
            if (!FortressManagerServes(context_)) {
                if (context_.systemLine) context_.systemLine(context_.Text("UIIT_MSG_GUILDERR_PERMISSION_DENIED"));
                return true;
            }
            if (context_.hud != nullptr) context_.hud->Hide(304); // CIFFortressMakeItemWnd
            std::uint64_t price = 0;
            if (context_.state != nullptr && context_.items != nullptr) {
                for (const auto& [place, item] : context_.state->inventory.places) {
                    if (place > game::slot::kShield && place < game::slot::kWorn) continue; // the job suit and the accessories' places
                    const assets::ItemRow* row = context_.items->Find(item.refId);
                    if (row == nullptr || !game::Repairable(*row, item, context_.magicOptions)) continue;
                    const std::uint32_t most = std::uint32_t(game::ComputeStats(*row, item, context_.magicOptions).durability.high);
                    price += game::PriceRepair(*row, item.durability, most, 0, true).gold;
                }
            }
            if (price == 0) {
                if (context_.systemLine) context_.systemLine(context_.Text("UIIT_MSG_STRGERR_THERE_IS_NO_ITEM_TO_REPAIR"));
                return true;
            }
            if (context_.hud != nullptr) {
                context_.hud->Confirm("UIIT_CTL_REPAIR_ALL", {context_.Text("UIIT_MSG_MSGBOX_REPAIR_ITEM"), std::to_string(price)}, [ctx, npc](bool yes) {
                    if (yes && ctx->fortressRequest) ctx->fortressRequest(proto::NpcRepair::kId, proto::Encode(proto::NpcRepair{npc, 2, 0}));
                });
            }
            return true;
        }
        if (choice.destination == 80 || choice.destination == 81) {
            // "Item production" / "Training": the query, whose answer opens.
            context_.forgeAsked = true;
            send(choice.destination == 80 ? proto::FortressAction::ForgeQuery : proto::FortressAction::TrainQuery);
            break;
        }
        if (choice.destination == 1) { // the field carries the row: 1 the battle aide's structures, 2 the manager's tax
            context_.fortressRequest(proto::FortressStructuresQuery::kId, proto::Encode(proto::FortressStructuresQuery{npc_, 0}));
        } else if (choice.destination == 2) {
            context_.fortressRequest(proto::FortressTaxQuery::kId, proto::Encode(proto::FortressTaxQuery{npc_}));
        } else if (choice.destination == 3 || choice.destination == 4) {
            // The pulley: op 2 opens, 0 closes (0x705E 0x15 {fortress, op}).
            close();
            if (const proto::EntityStructure* gate = PulleyGate(context_, npc_)) {
                context_.fortressRequest(proto::FortressPulley::kId,
                                         proto::Encode(proto::FortressPulley{npc_, gate->fortress, std::uint16_t(choice.destination == 3 ? 2 : 0)}));
            }
        } else {
            context_.fortressRequest(proto::FortressApplyOpen::kId, proto::Encode(proto::FortressApplyOpen{npc_}));
        }
        break;
    }
    case Choice::Kind::End: close(); break;
    }
    return true;
}

// --- The message box -----------------------------------------------------------------------------

// --- The reverse return scroll's box ---------------------------------------------------------------

void ReverseReturnBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    // The message box's tile laid out for this size (ours: as the death window's).
    for (const auto& c : children()) {
        if (c->node().type == "CIFNormalTile") c->rect = {16.0f, 40.0f, rect.w - 32.0f, rect.h - 56.0f};
    }
    assets::UiLayoutNode question;
    question.type = "CIFStatic";
    question.name = "GDR_REVERSE_QUESTION";
    question.id = 300;
    question.text = "UIIT_MSG_QUESTION_SILKMALL_ITEM_USE_REVERSE_PORTAL";
    question.hAlign = 1;
    question.rect = {27.0f, 48.0f, 246.0f, 20.0f};
    Add(ui.Make(question));
    static constexpr struct {
        int id;
        float y;
        const char* text;
    } kButtons[] = {{201, 76.0f, "UIIT_MSG_ITEM_USE_REVERSE_PORTAL_RETRUN_TO_LAST_RETURN"},
                    {202, 107.0f, "UIIT_MSG_ITEM_USE_REVERSE_PORTAL_RETRUN_TO_LAST_DEATH"},
                    {203, 138.0f, "UIIT_CLT_REVERSE_RETRUN_SCROLL_TO_APPOINT_CHANGE"},
                    {200, 169.0f, "UIO_COMMON_CTL_CANCEL"}};
    for (const auto& b : kButtons) {
        assets::UiLayoutNode button;
        button.type = "CIFButton";
        button.name = "GDR_REVERSE_BUTTON";
        button.id = b.id;
        button.ddj = "interface\\ifcommon\\com_return_button.ddj";
        button.text = b.text;
        button.hAlign = 1;
        button.rect = {25.0f, b.y, 252.0f, 24.0f};
        Add(ui.Make(button));
    }
}

bool ReverseReturnBox::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source->parent() != this) return ui::MainFrame::OnNotice(notice);
    int result = -1;
    if (notice.control >= 201 && notice.control <= 203) result = notice.control - 200;
    else if (notice.control == 200 || notice.control == 2) result = 0; // Cancel, the frame's close
    if (result < 0) return ui::MainFrame::OnNotice(notice);
    auto answer = std::move(answer_);
    answer_ = nullptr;
    if (ui_ != nullptr) ui_->Close(this);
    if (answer) answer(result);
    return true;
}

// --- The death window ------------------------------------------------------------------------------

void RebirthBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    // The message box's tile is laid out for its usual size: this box is 400 x 210.
    for (const auto& c : children()) {
        if (c->node().type == "CIFNormalTile") c->rect = {16.0f, 40.0f, rect.w - 32.0f, rect.h - 56.0f};
    }
    assets::UiLayoutNode picture;
    picture.type = "CIFStatic";
    picture.name = "GDR_REBIRTH_PICTURE";
    picture.ddj = "interface\\messagebox\\msgbox_rebirth.ddj";
    picture.rect = {128.0f, 43.0f, 148.0f, 52.0f};
    Add(ui.Make(picture));
    assets::UiLayoutNode text;
    text.type = "CIFPML";
    text.name = "GDR_REBIRTH_TEXT";
    text.id = 300;
    text.text = "UIIT_MSG_MSGBOX_ASK_SELF_REBIRTH_1";
    text.hAlign = 1;
    text.rect = {0.0f, 117.0f, 400.0f, 16.0f};
    Add(ui.Make(text));
    for (int id : {200, 201}) {
        assets::UiLayoutNode button;
        button.type = "CIFButton";
        button.name = id == 200 ? "GDR_REBIRTH_POINT" : "GDR_REBIRTH_HERE";
        button.id = id;
        button.ddj = "interface\\messagebox\\msgbox_rebirth_button.ddj";
        button.hAlign = 1;
        button.rect = {id == 200 ? 22.0f : 202.0f, 168.0f, 176.0f, 24.0f};
        Add(ui.Make(button));
    }
}

void RebirthBox::Ask(int level, std::function<void(int)> answer) {
    level_ = level;
    answer_ = std::move(answer);
    if (ui_ == nullptr) return;
    if (ui::Widget* b = Child(200)) b->SetText(ui_->resources().Text("UIIT_MSG_MSGBOX_REBIRTH_POINT_BUTTON"));
    if (ui::Widget* b = Child(201)) {
        b->SetText(ui_->resources().Text(level <= 10 ? "UIIT_MSG_MSGBOX_REBIRTH_STANDING_BUTTON" : "UIIT_MSG_MSGBOX_REBIRTH_HELP_BUTTON"));
    }
}

void RebirthBox::WarLines(const std::vector<std::string>& lines, double riseAt, std::string countdown) {
    riseAt_ = riseAt;
    countdown_ = std::move(countdown);
    if (ui_ == nullptr) return;
    for (std::size_t i = 0; i < lines.size() && i < 4; ++i) {
        assets::UiLayoutNode text;
        text.type = "CIFPML";
        text.id = 310 + int(i);
        text.hAlign = 1;
        text.rect = {0.0f, 134.0f + float(i) * 16.0f, 400.0f, 16.0f};
        ui::Widget* line = Add(ui_->Make(text));
        if (line != nullptr) line->SetText(lines[i]);
    }
    // Lower, under the lines (the box is 32 taller).
    for (int id : {200, 201}) {
        if (ui::Widget* b = Child(id)) b->rect.y = 200.0f;
    }
}

void RebirthBox::Update(ui::Frame& frame) {
    ui::MainFrame::Update(frame);
    if (countdown_.empty()) return;
    ui::Widget* question = Child(300);
    if (question == nullptr) return;
    const double left = riseAt_ - frame.time;
    if (left > 0.0) {
        char text[128];
        std::snprintf(text, sizeof text, countdown_.c_str(), int(std::ceil(left)));
        question->SetText(text);
    } else if (ui_ != nullptr) {
        question->SetText(ui_->resources().Text("UIIT_MSG_MSGBOX_ASK_SELF_REBIRTH_1"));
        countdown_.clear();
    }
}

bool RebirthBox::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source->parent() != this) return ui::MainFrame::OnNotice(notice);
    if (notice.control == 200 && !countdown_.empty()) return true; // not yet: the server would refuse it silently
    int kind = -1;
    if (notice.control == 200) kind = 1;
    else if (notice.control == 201 || notice.control == 2) kind = level_ <= 10 && notice.control == 201 ? 2 : 0;
    if (kind < 0) return ui::MainFrame::OnNotice(notice);
    auto answer = std::move(answer_);
    answer_ = nullptr;
    if (ui_ != nullptr) ui_->Close(this);
    if (answer) answer(kind);
    return true;
}

void ConfirmBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    // The Yes / No box (ifmessagebox.txt's MsgBoxSimple) below the frame's title, over the dark
    // tile of its Create section (ours: where the section sits in the box is not traced).
    if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifmessagebox.txt")) {
        for (const auto& s : layout->sections) {
            if (s.name != "MsgBoxSimple") continue;
            for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) {
                assets::UiLayoutNode n = *it;
                n.rect.x += 8.0f;
                n.rect.y += 40.0f;
                if (n.id == 251) n.rect = {24.0f, 52.0f, rect.w - 48.0f, 60.0f};
                Add(ui.Make(n));
            }
        }
    }
    if (auto* text = ui::As<ui::TextBox>(Child(251))) text->lineHeight = 18.0f;
}

void ConfirmBox::Ask(const std::vector<std::string>& lines, std::function<void(bool)> answer) {
    answer_ = std::move(answer);
    if (auto* text = ui::As<ui::TextBox>(Child(251))) {
        text->ClearParagraphs();
        for (const std::string& line : lines) text->AddParagraph(line, ui::kWhite);
    }
}

bool ConfirmBox::OnNotice(const ui::Notice& notice) {
    const bool yes = notice.kind == ui::Notice::Kind::Clicked && notice.control == 252 && notice.source->parent() == this;
    const bool no = (notice.kind == ui::Notice::Kind::Clicked && notice.control == 253 && notice.source->parent() == this) ||
                    (notice.kind == ui::Notice::Kind::Clicked && notice.control == 2 && notice.source->parent() == this);
    if (!yes && !no) return ui::MainFrame::OnNotice(notice);
    Answer(yes);
    return true;
}

void ConfirmBox::OkOnly(const std::string& label) {
    ui::Widget* yes = Child(252);
    if (ui::Widget* no = Child(253)) no->visible = false;
    if (yes == nullptr) return;
    yes->SetText(label);
    yes->rect.x = std::floor((rect.w - yes->rect.w) / 2.0f);
}

void ConfirmBox::Answer(bool yes) {
    auto answer = std::move(answer_);
    answer_ = nullptr;
    if (ui_ != nullptr) ui_->Close(this);
    if (answer) answer(yes);
}

} // namespace sr::client
