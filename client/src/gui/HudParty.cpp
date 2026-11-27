// The party's windows and party matching: views over
// game::PartyView, their requests to the HUD manager.

#include "gui/HudParty.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <format>
#include <functional>

#include "gui/Hud.h"
#include "gui/PortraitTexture.h"
#include "sr/game/Party.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kPartyArt = "interface\\party\\";
constexpr const char* kMatchLayout = "resinfo\\ifpartymatch.txt";
// Ours: the target menu's window id (it has none in ginterface.txt).
constexpr int kTargetMenuId = 30100;

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

void Show(ui::Widget* w, bool shown) {
    if (w != nullptr) w->visible = shown;
}

void Tooltip(ui::Frame& frame, ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->ShownOnScreen() && frame.pointer.hovered == w && !text.empty()) frame.interface.ShowTooltip(text, w->ScreenRect());
}

const char* RaceMark16(std::uint8_t race) {
    return race == 1 ? "interface\\ifcommon\\com_kindred_europe16.ddj" : "interface\\ifcommon\\com_kindred_china16.ddj";
}

std::string RaceText(const HudContext& context, std::uint8_t race) {
    return context.Text(race == 1 ? "UIIT_CTL_PARTYMATCH_AUTOMATCH_RACE_EU" : "UIIT_CTL_PARTYMATCH_AUTOMATCH_RACE_CH");
}

// The nodes of a layout's section added to a window (its first node in front, as the builder does).
void AddSection(ui::Interface& ui, ui::Widget& window, const std::string& layout, std::string_view section) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) window.Add(ui.Make(*it));
    }
}

const assets::UiLayoutNode* NodeIn(ui::Interface& ui, const std::string& layout, std::string_view section, int id) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return nullptr;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (const auto& n : s.nodes) {
            if (n.id == id) return &n;
        }
    }
    return nullptr;
}

template <class T>
T* FindWindow(ui::Interface& ui) {
    for (ui::Widget* w : ui.Windows()) {
        if (auto* t = dynamic_cast<T*>(w); t != nullptr && w->visible) return t;
    }
    return nullptr;
}

// A control made in code (the original's own, not in a layout).
ui::Widget* MakeNode(ui::Interface& ui, ui::Widget& parent, const std::string& type, int id, ui::Rect rect) {
    assets::UiLayoutNode n;
    n.type = type;
    n.id = id;
    n.rect = {rect.x, rect.y, rect.w, rect.h};
    auto made = ui.Make(n);
    ui::Widget* w = made.get();
    parent.Add(std::move(made));
    return w;
}

std::uint8_t ByteOf(const ui::Widget* edit) {
    return edit != nullptr ? std::uint8_t(std::clamp(std::atoi(edit->text().c_str()), 0, 255)) : 0;
}

// --- The Party page (CIFParty) -----------------------------------------------------------------------

// A member's row: the master's in the window itself, a slot's in its CIFPartySlot (ifparty.txt,
// ifpartyslot.txt).
struct RowIds {
    int name, label, level, guild, hp, mp, exit, race, picture;
};
constexpr RowIds kMasterRow{42, 43, 44, 45, 46, 47, 48, 15, 41};
constexpr RowIds kSlotRow{11, 12, 13, 14, 16, 17, 18, 10, 9};
constexpr int kSlots = 7, kFirstSlot = 30;

class PartyWindow : public ui::Widget {
public:
    PartyWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui_ = &ui;
        for (ui::Widget* row : Rows()) {
            const RowIds& ids = row == this ? kMasterRow : kSlotRow;
            Show(row->Child(ids.exit), false);
            if (auto* hp = ui::As<ui::Gauge>(row->Child(ids.hp))) hp->SetValue(0.0f, true);
            if (auto* mp = ui::As<ui::Gauge>(row->Child(ids.mp))) mp->SetValue(0.0f, true);
        }
        Show(Child(14), false); // the leader's crown
    }

    void Update(ui::Frame& frame) override {
        const game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        const game::PartyView& party = state->party;
        // The master's row: the party's master (none: "<Party leader doesn't exist>" over the
        // disabled bars); the slots: the other members in the order they joined.
        const proto::PartyMemberRecord* master = party.InParty() ? party.Member(party.leader) : nullptr;
        Fill(frame, *this, kMasterRow, master, true);
        Show(Child(13), master != nullptr && !master->guild.empty()); // the guild's plate
        Show(Child(14), master != nullptr);
        std::vector<const proto::PartyMemberRecord*> others;
        for (const proto::PartyMemberRecord& m : party.members) {
            if (m.id != party.leader) others.push_back(&m);
        }
        const std::uint32_t self = party.SelfId();
        for (int i = 0; i < kSlots; ++i) {
            ui::Widget* slot = Child(kFirstSlot + i);
            if (slot == nullptr) continue;
            const proto::PartyMemberRecord* m = std::size_t(i) < others.size() ? others[std::size_t(i)] : nullptr;
            slotMembers_[std::size_t(i)] = m != nullptr ? m->id : 0;
            Fill(frame, *slot, kSlotRow, m, false);
            Show(slot->Child(15), m != nullptr && !m->guild.empty());
            // The button: the master sends any member away, a member leaves from its own row.
            ui::Widget* exit = slot->Child(kSlotRow.exit);
            Show(exit, m != nullptr && (party.IsLeader() || m->id == self));
            Tooltip(frame, exit, context_.Text(party.IsLeader() && m != nullptr && m->id != self ? "UIIT_CTL_BAN_PARTY" : "UIIT_CTL_LEAVE_PARTY"));
        }
        // The master's own row: its button dismisses the party.
        ui::Widget* dismiss = Child(kMasterRow.exit);
        Show(dismiss, party.IsLeader());
        Tooltip(frame, dismiss, context_.Text("UIIT_STT_PARTY_DISSOLVE"));
        // The settings: the party's, or the next party's when in none.
        const std::uint8_t settings = party.InParty() ? party.settings : party.preferred;
        Put(Child(49), PartyExpText(context_, settings));
        Put(Child(50), PartyItemText(context_, settings));
        // Invite: anyone out of a party, its master, or every member when the party lets them; Set:
        // out of a party (the next party's) or its master (ours: the original changes no party).
        if (ui::Widget* b = Child(20)) b->enabled = !party.InParty() || party.IsLeader() || (party.settings & game::kPartyAnyoneInvites) != 0;
        if (ui::Widget* b = Child(21)) b->enabled = !party.InParty() || party.IsLeader();
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || context_.hud == nullptr || context_.state == nullptr) return false;
        game::PartyView& party = context_.state->party;
        if (notice.source->parent() == this) {
            switch (notice.control) {
            case 48: // the master's dismiss
                context_.hud->Request({HudRequest::Kind::PartyLeave});
                return true;
            case 20: { // Invite: the selected player
                const game::TargetStatus& t = context_.state->target;
                if (ui_ != nullptr && t.kind == game::TargetStatus::Kind::Player && t.entity != 0 && t.entity != context_.state->self.entity) {
                    InviteToParty(*ui_, context_, t.entity);
                }
                return true;
            }
            case 21: // Set: the settings box
                if (ui_ != nullptr) {
                    Hud* hud = context_.hud;
                    OpenSetPartyMode(*ui_, party.InParty() ? party.settings : party.preferred, [hud, &party](std::uint8_t s) {
                        if (!party.InParty()) party.preferred = s;
                        hud->Request({HudRequest::Kind::PartySettings, int(s)});
                    });
                }
                return true;
            case 22: context_.hud->Toggle(hud::kPartyMatch); return true;
            default: return false;
            }
        }
        // A slot's button: the master sends that member away (asked first), a member leaves.
        if (notice.control != kSlotRow.exit) return false;
        for (int i = 0; i < kSlots; ++i) {
            if (notice.source->parent() != Child(kFirstSlot + i)) continue;
            const std::uint32_t member = slotMembers_[std::size_t(i)];
            if (member == 0) return true;
            if (party.IsLeader() && member != party.SelfId()) {
                Hud* hud = context_.hud;
                hud->Confirm("UIIT_STT_CONFIRM_BOX", {context_.Text("UIIT_MSG_PARTY_BAN_REQUEST")}, [hud, member](bool yes) {
                    if (yes) hud->Request({HudRequest::Kind::PartyKick, int(member)});
                });
            } else {
                context_.hud->Request({HudRequest::Kind::PartyLeave});
            }
            return true;
        }
        return false;
    }

private:
    std::vector<ui::Widget*> Rows() {
        std::vector<ui::Widget*> rows{this};
        for (int i = 0; i < kSlots; ++i) {
            if (ui::Widget* slot = Child(kFirstSlot + i)) rows.push_back(slot);
        }
        return rows;
    }

    void Fill(ui::Frame& frame, ui::Widget& row, const RowIds& ids, const proto::PartyMemberRecord* m, bool masterRow) {
        Put(row.Child(ids.name), m != nullptr ? m->name : masterRow ? context_.Text("UIIT_STT_NO_PARTY_LEADER") : std::string());
        Show(row.Child(ids.label), m != nullptr);
        Put(row.Child(ids.level), m != nullptr ? std::to_string(m->level) : std::string());
        Put(row.Child(ids.guild), m != nullptr ? m->guild : std::string());
        if (auto* race = ui::As<ui::Static>(row.Child(ids.race))) {
            race->texture = m != nullptr ? RaceMark16(m->race) : std::string();
            race->visible = m != nullptr;
        }
        // The face (28 x 28 under pt_face.ddj): no portrait of its own, the member's quick party bar's,
        // the player bar's for the player.
        if (auto* face = ui::As<ui::PictureClip>(row.Child(ids.picture))) {
            const bool self = m != nullptr && context_.state != nullptr && m->id == context_.state->party.SelfId();
            face->mask = std::string(kPartyArt) + "pt_face.ddj";
            face->texture = m == nullptr ? std::string() : PortraitTexture(self ? kSelfPortrait : m->id);
            face->visible = m != nullptr;
        }
        // HP and MP by the tenth; none: the master's row's disabled bars, a slot's empty.
        for (const int id : {ids.hp, ids.mp}) {
            auto* g = ui::As<ui::Gauge>(row.Child(id));
            if (g == nullptr) continue;
            const bool hp = id == ids.hp;
            g->FitToTexture(frame.paint, std::string(kPartyArt) + (hp ? "pt_hp.ddj" : "pt_mp.ddj")); // the layout gives no size
            if (m != nullptr) {
                g->texture = std::string(kPartyArt) + (hp ? "pt_hp.ddj" : "pt_mp.ddj");
                g->SetValue(float(hp ? m->hpTenths : m->mpTenths) / 10.0f);
                g->visible = true;
            } else {
                g->texture = std::string(kPartyArt) + (hp ? "pt_hp_disable.ddj" : "pt_mp_disable.ddj");
                g->SetValue(1.0f, true);
                g->visible = masterRow;
            }
        }
    }

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::array<std::uint32_t, kSlots> slotMembers_{};
};

// --- The quick party window (CIFQuickPartyWnd) ---------------------------------------------------------

// The other members' bars down the left edge (the 1.188 client's slots show every member but the
// player), seven 122 x 40 slots 44 apart (ifquickpartywnd.txt's places; the 1.188 client makes them in code). A press
// on one selects that member.
class QuickPartyWindow : public ui::Widget {
public:
    QuickPartyWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        AddSection(ui, *this, "resinfo\\ifquickpartywnd.txt", "Create");
        rect.w = 122.0f;
        rect.h = Spacing() * kSlots;
        interactive = true;
        for (int i = 0; i < kSlots; ++i) {
            ui::Widget* slot = Child(1 + i);
            if (slot == nullptr) continue;
            slot->visible = false;
            for (int id : {5, 10, 20, 21, 100}) Show(slot->Child(id), false); // the status, buffs, warnings (Caution), selection
            // The member's two highest masteries, 17 x 17 under the face, made in code.
            Show(MakeNode(ui, *slot, "CIFStatic", kMastery1, {-3.0f, 39.0f, 17.0f, 17.0f}), false);
            Show(MakeNode(ui, *slot, "CIFStatic", kMastery2, {15.0f, 39.0f, 17.0f, 17.0f}), false);
            for (int id : {2, 3}) {
                if (auto* g = ui::As<ui::Gauge>(slot->Child(id))) g->SetValue(0.0f, true);
            }
            // The face (1): the member's portrait, 64 x 64, under the 28 x 28 qpt_face.ddj; the screen draws it
            // (frame/Portraits.h).
            if (auto* face = ui::As<ui::PictureClip>(slot->Child(1))) face->mask = "interface\\quickparty\\qpt_face.ddj";
        }
        // The lock (10): the slots' order changes only while it is open (the original's "Change Quick Party Window
        // Sequence"), shown in a party; 20 x 20 at 0, -7 (the layout gives it no size).
        if (ui::Widget* lock = Child(10)) {
            lock->rect = {0.0f, -7.0f, 20.0f, 20.0f};
            lock->visible = false;
        }
    }

    void Update(ui::Frame& frame) override {
        const game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        const game::PartyView& party = state->party;
        // The members in the player's order (a member new to it at the end).
        std::vector<const proto::PartyMemberRecord*> others = party.Others();
        for (const proto::PartyMemberRecord* m : others) {
            if (std::find(order_.begin(), order_.end(), m->id) == order_.end()) order_.push_back(m->id);
        }
        std::stable_sort(others.begin(), others.end(), [&](const proto::PartyMemberRecord* a, const proto::PartyMemberRecord* b) {
            return std::find(order_.begin(), order_.end(), a->id) < std::find(order_.begin(), order_.end(), b->id);
        });
        // The lock: its picture and its tooltip by its state.
        if (auto* lock = ui::As<ui::Static>(Child(10)); lock != nullptr) {
            lock->visible = party.InParty();
            lock->texture = unlocked_ ? "interface\\ifcommon\\quickparty_move_unlock.ddj" : "interface\\ifcommon\\quickparty_move_lock.ddj";
            Tooltip(frame, lock,
                    context_.Text("UIIT_STT_QUICKPARTY_MOVE") + "\n" +
                        FormatText(context_.Text("UIIT_STT_QUICKPARTY_MOVE_STATE"),
                                   {context_.Text(unlocked_ ? "UIIT_STT_QUICKPARTY_MOVE_UNLOCK" : "UIIT_STT_QUICKPARTY_MOVE_LOCK")}));
        }
        // Only the slots shown take the mouse; out of a party it lets every click through to the world.
        const int shown = party.InParty() ? std::min<int>(kSlots, int(others.size())) : 0;
        const float spacing = Spacing();
        rect.h = spacing * float(std::max(1, shown));
        clickThrough = shown == 0;
        phase_ = std::fmod(phase_ + double(frame.dt) * 1000.0, 800.0);
        for (int i = 0; i < kSlots; ++i) {
            ui::Widget* slot = Child(1 + i);
            if (slot == nullptr) continue;
            slot->rect.y = spacing * float(i);
            const proto::PartyMemberRecord* m = party.InParty() && std::size_t(i) < others.size() ? others[std::size_t(i)] : nullptr;
            entities_[std::size_t(i)] = m != nullptr ? m->entity : 0;
            if (i == 0) shownIds_.clear();
            if (m != nullptr) shownIds_.push_back(m->id);
            slot->visible = m != nullptr;
            if (m == nullptr) continue;
            Put(slot->Child(4), m->name);
            if (auto* g = ui::As<ui::Gauge>(slot->Child(2))) g->SetValue(float(m->hpTenths) / 10.0f);
            if (auto* g = ui::As<ui::Gauge>(slot->Child(3))) g->SetValue(float(m->mpTenths) / 10.0f);
            Caution(slot->Child(20), m->hpTenths, "hpWarning", "pmi_hp_quick_effect_caution.ddj");
            Caution(slot->Child(21), m->mpTenths, "mpWarning", "pmi_mp_quick_effect_caution.ddj");
            if (auto* race = ui::As<ui::Static>(slot->Child(6))) race->texture = RaceMark16(m->race);
            Show(slot->Child(7), m->id == party.leader);
            Show(slot->Child(100), m->entity != 0 && state->target.entity == m->entity);
            if (auto* face = ui::As<ui::PictureClip>(slot->Child(1))) face->texture = PortraitTexture(m->id);
            // Far away: a darker cover over the face by the distance.
            if (auto* status = ui::As<ui::Static>(slot->Child(5))) {
                const int grade = FarGrade(*m);
                status->visible = grade > 0;
                if (grade > 0) status->texture = std::format("interface\\quickparty\\qpt_face_faraway_{}.ddj", 50 + 10 * grade);
            }
            // Its masteries: each one's icon (skillmasterydata col 11), its name the tooltip; none hidden.
            const std::uint32_t masteries[2] = {m->mastery1, m->mastery2};
            for (int k = 0; k < 2; ++k) {
                auto* icon = ui::As<ui::Static>(slot->Child(k == 0 ? kMastery1 : kMastery2));
                const assets::MasteryRow* row = context_.skills != nullptr && masteries[k] != 0 ? context_.skills->Mastery(masteries[k]) : nullptr;
                if (icon == nullptr) continue;
                icon->visible = row != nullptr && !row->iconPath.empty();
                if (!icon->visible) continue;
                icon->texture = row->iconPath;
                Tooltip(frame, icon, context_.Text(row->nameKey));
            }
        }
    }

    void Mouse(ui::Frame& frame) override {
        // The lock toggled; with it open a slot dragged onto another swaps the two members' places.
        const auto slotAt = [&](ui::Vec2 at) {
            for (int i = 0; i < kSlots; ++i) {
                ui::Widget* slot = Child(1 + i);
                if (slot != nullptr && slot->visible && slot->Hit(at)) return i;
            }
            return -1;
        };
        if (frame.input.Released(ui::MouseButton::Left) && dragFrom_ >= 0) {
            const int to = slotAt(frame.pointer.position), from = std::exchange(dragFrom_, -1);
            if (unlocked_ && to >= 0 && to != from && std::size_t(std::max(from, to)) < shownIds_.size()) {
                auto a = std::find(order_.begin(), order_.end(), shownIds_[std::size_t(from)]);
                auto b = std::find(order_.begin(), order_.end(), shownIds_[std::size_t(to)]);
                if (a != order_.end() && b != order_.end()) std::iter_swap(a, b);
            }
            return;
        }
        if (!frame.input.Pressed(ui::MouseButton::Left) || context_.hud == nullptr) return;
        if (ui::Widget* lock = Child(10); lock != nullptr && lock->visible && lock->Hit(frame.pointer.position)) {
            unlocked_ = !unlocked_;
            return;
        }
        const int i = slotAt(frame.pointer.position);
        if (i < 0 || entities_[std::size_t(i)] == 0) {
            if (i >= 0 && unlocked_) dragFrom_ = i;
            return;
        }
        if (unlocked_) dragFrom_ = i;
        context_.hud->Request({HudRequest::Kind::SelectEntity, int(entities_[std::size_t(i)])});
    }

private:
    static constexpr int kMastery1 = 0x65, kMastery2 = 0x66;
    // The slots' spacing: 56, or 73 with the members' buffs shown (the game option
    // "party buffs": a row under the masteries). Ours: one column (the original starts another 133 to
    // the right when the next slot would come within 230 of the screen's bottom).
    float Spacing() const { return context_.options != nullptr && !context_.options->game.Get("partyBuffs") ? 56.0f : 73.0f; }

    // The member's distance grade: 0 under 600 from the player, 1..5 from 600, 700, 800, 900 and 1000
    // (the original's squared distances 360,000 .. 1,000,000); its place the one seen
    // when it is in view, else the party's last word. Ours: across the ground (the original also
    // counts the height when the member is in view).
    int FarGrade(const proto::PartyMemberRecord& m) const {
        const game::WorldView* world = context_.world;
        const game::WorldView::Entity* self = world != nullptr ? world->Find(world->Self()) : nullptr;
        if (self == nullptr) return 0;
        const game::WorldView::Entity* seen = m.entity != 0 ? world->Find(m.entity) : nullptr;
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(seen != nullptr ? seen->position : m.position);
        const double d2 = (a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z);
        return d2 >= 1000000.0 ? 5 : d2 >= 810000.0 ? 4 : d2 >= 640000.0 ? 3 : d2 >= 490000.0 ? 2 : d2 >= 360000.0 ? 1 : 0;
    }

    // A member's HP or MP under 30 % (tenths 1 and 2; never at 0, a dead one): that bar's caution glow, its sheet's
    // eight 64 x 32 cells 100 ms each, as the Game page's HP / MP warning row allows (the original's quick party slot).
    void Caution(ui::Widget* w, std::uint8_t tenths, const char* row, const char* sheet) {
        auto* glow = ui::As<ui::Static>(w);
        if (glow == nullptr) return;
        glow->visible = tenths > 0 && tenths < 3 && context_.options != nullptr && context_.options->game.Get(row);
        if (!glow->visible) return;
        glow->texture = std::string("interface\\playerminiinfo\\") + sheet;
        const int cell = int(phase_ / 100.0) % 8;
        glow->customUv = true;
        glow->uv0 = {float(cell % 4) * 0.25f, float(cell / 4) * 0.5f};
        glow->uv1 = {glow->uv0.x + 0.25f, glow->uv0.y + 0.5f};
    }

    HudContext& context_;
    std::array<std::uint32_t, kSlots> entities_{};
    double phase_ = 0.0; // the caution glows' sheet (ms)
    bool unlocked_ = false;              // the lock open: the slots may be reordered
    std::vector<std::uint32_t> order_;   // the members' order (character ids)
    std::vector<std::uint32_t> shownIds_; // the members shown, slot by slot
    int dragFrom_ = -1;                  // the slot a drag began on
};

// --- The settings box (CIFSetPartyMode) ----------------------------------------------------------------

// ifsetpartymode.txt: two radio groups (13 the exp, 14 the items: Share, then each its own), the box "Can invite
// without master status" (17), Yes / No.
class SetPartyModeBox : public ui::MainFrame {
public:
    SetPartyModeBox(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        const auto group = [&](int id, const char* share, const char* self) {
            auto* r = ui::As<ui::RadioButton>(Child(id));
            if (r == nullptr) return;
            // Two units 20 apart inside the group's box (ours: the original's group places them
            // with its own numbers).
            r->origin = {6.0f, 4.0f};
            r->spacing = 20.0f;
            r->labelOffset = 20.0f;
            r->labelWidth = 96.0f;
            r->SetUnits(ui, {context_.Text(share), context_.Text(self)});
        };
        group(13, "UIIT_STT_PARTY_EXP_SHARE", "UIIT_STT_PARTY_EXP_SELF");
        group(14, "UIIT_STT_PARTY_ITEM_SHARE", "UIIT_STT_PARTY_ITEM_SELF");
    }

    void Ask(std::uint8_t settings, std::function<void(std::uint8_t)> confirmed) {
        confirmed_ = std::move(confirmed);
        if (auto* r = ui::As<ui::RadioButton>(Child(13))) r->selected = (settings & game::kPartyExpShare) != 0 ? 0 : 1;
        if (auto* r = ui::As<ui::RadioButton>(Child(14))) r->selected = (settings & game::kPartyItemShare) != 0 ? 0 : 1;
        if (auto* c = ui::As<ui::CheckBox>(Child(17))) c->checked = (settings & game::kPartyAnyoneInvites) != 0;
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this;
        if (!mine || (notice.control != 20 && notice.control != 21 && notice.control != 2)) return ui::MainFrame::OnNotice(notice);
        std::uint8_t settings = 0;
        if (const auto* r = ui::As<ui::RadioButton>(Child(13)); r != nullptr && r->selected == 0) settings |= game::kPartyExpShare;
        if (const auto* r = ui::As<ui::RadioButton>(Child(14)); r != nullptr && r->selected == 0) settings |= game::kPartyItemShare;
        if (const auto* c = ui::As<ui::CheckBox>(Child(17)); c != nullptr && c->checked) settings |= game::kPartyAnyoneInvites;
        auto confirmed = std::move(confirmed_);
        confirmed_ = nullptr;
        if (ui_ != nullptr) ui_->Close(this);
        if (notice.control == 20 && confirmed) confirmed(settings);
        return true;
    }

private:
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::function<void(std::uint8_t)> confirmed_;
};

// --- The target menu (CNIFTargetMenu) ------------------------------------------------------------------

// The 1.188 client's menu of a player (res_ui\targetmenu.2dt, read for its places: a 102 x 142
// ub_new_wnd_ window, the name in exc_box at 5,6, five 86 x 20 ub_new_menu_button buttons at x 8).
// Whisper, Exchange, Add friend and Invite party work; Trace comes with its feature.
class TargetMenu : public ui::FrameBorder {
public:
    TargetMenu(const assets::UiLayoutNode& node, HudContext& context) : FrameBorder(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui_ = &ui;
        interactive = true;
        assets::UiLayoutNode tile;
        tile.type = "CIFNormalTile";
        tile.ddj = "interface\\ifcommon\\bg_tile\\com_bg_tile_u.ddj";
        tile.rect = {4.0f, 4.0f, 94.0f, 134.0f};
        Add(ui.Make(tile));
        assets::UiLayoutNode box;
        box.type = "CIFStatic";
        box.id = 8;
        box.ddj = "interface\\exchange\\exc_box.ddj";
        box.rect = {5.0f, 6.0f, 93.0f, 20.0f};
        box.hAlign = 1;
        box.vAlign = 1;
        Add(ui.Make(box));
        // The 2dt's ids and places (its y order: whisper, exchange, friend, party, trace).
        constexpr std::pair<int, float> kButtons[] = {{14, 32}, {15, 52}, {17, 71}, {16, 91}, {18, 110}};
        constexpr std::pair<int, const char*> kCaptions[] = {
            {14, "UIIT_STT_GET_WHISPER"}, {15, "UIIT_CTL_EXCHANGE_TT"}, {16, "UIIT_STT_INVITE_PARTY"}, {17, "UIIT_CTL_FRIENDADD"}, {18, "UIIT_CTL_AUTOTRACE_TT"}};
        for (const auto& [id, y] : kButtons) {
            assets::UiLayoutNode b;
            b.type = "CIFButton";
            b.id = id;
            b.ddj = "interface\\underbar\\ub_new_menu_button.ddj";
            b.rect = {8.0f, y, 86.0f, 20.0f};
            b.hAlign = 1;
            b.vAlign = 1;
            for (const auto& [cid, key] : kCaptions) {
                if (cid == id) b.text = key;
            }
            ui::Widget* made = Add(ui.Make(b));
            made->enabled = id == 14 || id == 15 || id == 16 || id == 17;
        }
    }

    void Set(std::uint32_t entity, const std::string& name) {
        entity_ = entity;
        name_ = name;
        Put(Child(8), name);
    }

    void Update(ui::Frame& frame) override {
        // Gone with a press anywhere else (ours), or when its player is no longer selected.
        const bool pressed = frame.input.Pressed(ui::MouseButton::Left) || frame.input.Pressed(ui::MouseButton::Right);
        const bool selected = context_.state != nullptr && context_.state->target.entity == entity_;
        if ((pressed && !Hit(frame.pointer.position) && opened_) || !selected) {
            if (ui_ != nullptr) ui_->Close(this);
            return;
        }
        opened_ = true;
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || notice.source->parent() != this || ui_ == nullptr) return false;
        if (notice.control == 14) {
            if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->Address(name_, true);
        } else if (notice.control == 15) {
            if (context_.useAction) context_.useAction(1006); // the Action page's Exchange, on the selected player
        } else if (notice.control == 16) {
            InviteToParty(*ui_, context_, entity_);
        } else if (notice.control == 17) {
            if (context_.communityRequest) context_.communityRequest(game::proto::FriendAsk::kId, game::proto::Encode(game::proto::FriendAsk{name_}));
        } else {
            return true;
        }
        ui_->Close(this);
        return true;
    }

private:
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::uint32_t entity_ = 0;
    std::string name_;
    bool opened_ = false;
};

// --- Party matching (CIFPartyMatch) ---------------------------------------------------------------------

constexpr int kMatchRows = 12, kFirstMatchRow = 70;

class PartyMatchWindow : public ui::MainFrame {
public:
    PartyMatchWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        // The search line, the column heads and the twelve rows (the layout's other sections).
        for (const char* section : {"SearchInfo", "SlotListButton", "SlotList"}) AddSection(ui, *this, kMatchLayout, section);
        // The list's black square is black inside (no tile lies under the rows; as the combo box's
        // own black square is filled).
        if (ui::Widget* square = Child(6)) square->background = ui::Argb(0xFF000000);
        if (auto* combo = ui::As<ui::ComboBox>(Child(45))) {
            combo->items = {context_.Text("UIIT_CTL_PARTYMATCH_PSEARCH_OBJECTALL")};
            for (int p = 0; p < game::kPartyPurposes; ++p) combo->items.push_back(PartyPurposeText(context_, std::uint8_t(p)));
            combo->Select(0);
        }
        for (int id : {49, 50}) {
            if (auto* e = ui::As<ui::Edit>(Child(id))) {
                e->numeric = true;
                e->limit = 3;
                e->SetText(id == 49 ? "1" : "110"); // every level, to begin with
            }
        }
        if (auto* e = ui::As<ui::Edit>(Child(47))) e->limit = 12;
        if (ui::Widget* b = Child(17)) b->enabled = false; // auto match: not ours yet
        Show(Child(26), false);                            // twelve rows a page: no scrolling
        Ask(0);
    }

    void Update(ui::Frame& frame) override {
        const game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        const game::PartyView& party = state->party;
        const proto::PartyMatchListing& listing = party.listing;
        if (auto* spin = ui::As<ui::Spin>(Child(25))) {
            spin->max = std::max(0, int(listing.pages) - 1);
            if (spin->value != page_ && seen_ == party.version) {
                page_ = spin->value;
                Ask(page_);
            } else if (seen_ != party.version) {
                spin->value = listing.page;
                page_ = listing.page;
            }
        }
        if (seen_ != party.version) {
            seen_ = party.version;
            if (selected_ >= int(listing.entries.size())) selected_ = -1;
        }
        for (int i = 0; i < kMatchRows; ++i) {
            ui::Widget* row = Child(kFirstMatchRow + i);
            if (row == nullptr) continue;
            const proto::PartyMatchEntry* e = std::size_t(i) < listing.entries.size() ? &listing.entries[std::size_t(i)] : nullptr;
            for (int id = 10; id <= 17; ++id) Show(row->Child(id), e != nullptr);
            // Every row is a bar (its top and bottom pixels are the lines between the rows); the
            // chosen one in the selected pieces.
            Show(row->Child(5), true);
            if (auto* bar = ui::As<ui::Bar>(row->Child(5))) {
                bar->pieces = e != nullptr && i == selected_ ? "interface\\ifcommon\\com_bar01select_" : "";
            }
            if (e == nullptr) continue;
            Put(row->Child(10), std::to_string(e->entry & 0xFFFFF)); // above: which server keeps it
            // The party's kind as a mark: shared exp, or each its own (ours: which mark is which is
            // inferred from their names, pt_association / pt_eachone).
            if (auto* mark = ui::As<ui::Static>(row->Child(11))) {
                mark->texture = std::string(kPartyArt) + ((e->form.settings & game::kPartyExpShare) != 0 ? "pt_association.ddj" : "pt_eachone.ddj");
            }
            Put(row->Child(12), RaceText(context_, e->race));
            Put(row->Child(13), e->name);
            Put(row->Child(14), e->form.title);
            Put(row->Child(15), PartyPurposeText(context_, e->form.purpose));
            Put(row->Child(16), std::format("{}/{}", e->members, e->capacity));
            Put(row->Child(17), std::format("{}~{}", e->form.levelMin, e->form.levelMax));
        }
        const bool own = party.ownEntry != 0;
        if (ui::Widget* b = Child(18)) b->enabled = !own;
        if (ui::Widget* b = Child(19)) b->enabled = own;
        if (ui::Widget* b = Child(20)) b->enabled = own;
        (void)frame;
    }

    void Mouse(ui::Frame& frame) override {
        if (!frame.input.Pressed(ui::MouseButton::Left)) return;
        for (int i = 0; i < kMatchRows; ++i) {
            ui::Widget* row = Child(kFirstMatchRow + i);
            if (row != nullptr && row->Hit(frame.pointer.position)) selected_ = i;
        }
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.source == nullptr || notice.source->parent() != this || context_.state == nullptr || context_.hud == nullptr || ui_ == nullptr) {
            return ui::MainFrame::OnNotice(notice);
        }
        if (notice.kind != ui::Notice::Kind::Clicked) return ui::MainFrame::OnNotice(notice);
        game::PartyView& party = context_.state->party;
        const proto::PartyMatchEntry* chosen =
            selected_ >= 0 && std::size_t(selected_) < party.listing.entries.size() ? &party.listing.entries[std::size_t(selected_)] : nullptr;
        switch (notice.control) {
        case 15: // Join party
            if (chosen == nullptr) return true;
            if (party.InParty()) {
                party.notices.push_back({"UIIT_MSG_PARTYMATCH_JOIN_ERROR_PARTY", {}});
                return true;
            }
            context_.hud->Request({HudRequest::Kind::PartyMatchJoin, int(chosen->entry)});
            return true;
        case 16: // Whisper its leader
            if (chosen != nullptr) {
                if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->Address(chosen->name, true);
            }
            return true;
        case 18: { // Form party (the original's checks before its box)
            const char* refused = nullptr;
            if (party.ownEntry != 0) refused = "UIIT_MSG_PARTYMATCH_RECORD_ERROR_AGAIN";
            else if (!party.InParty() && context_.state->self.level < game::kPartyMinLevel) refused = "UIIT_MSG_PARTYMATCH_JOIN_ERROR_LEVEL";
            else if (party.InParty() && !party.IsLeader()) refused = "UIIT_MSG_PARTYMATCH_JOIN_ERROR_PARTY";
            else if (party.InParty() && int(party.members.size()) >= party.Capacity()) refused = "UIIT_MSG_PARTYERR_ALREADY_FULL";
            if (refused != nullptr) {
                party.notices.push_back({refused, {}});
                return true;
            }
            OpenPartyMatchRegister(*ui_, context_, false);
            return true;
        }
        case 19: OpenPartyMatchRegister(*ui_, context_, true); return true;
        case 20: { // Delete entry, asked first
            Hud* hud = context_.hud;
            hud->Confirm("UIIT_STT_CONFIRM_BOX", {context_.Text("UIIT_STT_PARTYMATCH_DELETE_CONFIRM")}, [hud](bool yes) {
                if (yes) hud->Request({HudRequest::Kind::PartyMatchDelete});
            });
            return true;
        }
        case 55: Ask(0); return true;     // Search
        case 56: Ask(page_); return true; // Refresh
        default: return ui::MainFrame::OnNotice(notice);
        }
    }

private:
    // A page of the listings that fit the search line.
    void Ask(int page) {
        if (context_.hud == nullptr) return;
        int purpose = 0xFF;
        if (const auto* combo = ui::As<ui::ComboBox>(Child(45)); combo != nullptr && combo->selected > 0) purpose = combo->selected - 1;
        const int levelMin = ByteOf(Child(49)), levelMax = ByteOf(Child(50));
        const std::string name = Child(47) != nullptr ? Child(47)->text() : std::string();
        context_.hud->Request({HudRequest::Kind::PartyMatchList, page, name, {}, purpose | levelMin << 8 | levelMax << 16});
        page_ = page;
    }

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = ~0ull;
    int page_ = 0, selected_ = -1;
};

// The listing's box (CIFPartyMatchRegister): its purpose (four radio buttons in a row), the levels,
// the title (each purpose's default title), and the party's type. Ours: the type is
// chosen here with two boxes where the original only wrote the party's (statics 45, 47).
class PartyMatchRegisterBox : public ui::MainFrame {
public:
    PartyMatchRegisterBox(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        if (auto* r = ui::As<ui::RadioButton>(Child(40))) {
            // Four across the row's 272 (ours: the original's places are set in code).
            r->origin = {0.0f, 0.0f};
            r->spacing = 72.0f;
            r->labelOffset = 17.0f;
            r->labelWidth = 54.0f;
            std::vector<std::string> labels;
            for (int p = 0; p < game::kPartyPurposes; ++p) labels.push_back(PartyPurposeText(context_, std::uint8_t(p)));
            r->SetUnits(ui, labels, true);
        }
        for (int id : {42, 43}) {
            if (auto* e = ui::As<ui::Edit>(Child(id))) {
                e->numeric = true;
                e->limit = 3;
            }
        }
        if (auto* e = ui::As<ui::Edit>(Child(49))) e->limit = 40; // the field's width (ours)
        // Our two boxes in the diamonds' places: shared exp, shared items.
        for (ui::Widget* w : {Find("GDR_PTY_OPTION_EXP_DECO"), Find("GDR_PTY_OPTION_ITEM_DECO")}) Show(w, false);
        for (const auto& [id, x] : {std::pair{70, 20.0f}, std::pair{71, 157.0f}}) {
            assets::UiLayoutNode box;
            box.type = "CIFCheckBox";
            box.id = id;
            box.rect = {x, 224.0f, 16.0f, 16.0f};
            Add(ui.Make(box));
        }
    }

    // The academy's register box (the same window in the "register" / "modify" state of the matching
    // window): the subject and whom it takes, the first row's choice of two (Recruit apprentices, Recruit guardian
    // assistants); the levels and the party's type put away (ours: which controls 1.188 hides is not read).
    void SetupAcademy(bool modify) {
        academy_ = true;
        modify_ = modify;
        if (auto* r = ui::As<ui::RadioButton>(Child(40)); r != nullptr && ui_ != nullptr) {
            r->spacing = 136.0f;
            r->labelWidth = 116.0f;
            r->SetUnits(*ui_, {context_.Text("UIIT_STT_TC_STUDENT_INVITATION"), context_.Text("UIIT_STT_TC_SECOND_GUARDIAN_INVITATION")}, true);
        }
        for (int id : {6, 7, 11, 12, 15, 16, 20, 21, 31, 32, 35, 36, 42, 43, 45, 47, 70, 71}) Show(Child(id), false);
        for (ui::Widget* w : {Find("GDR_PTY_OPTION_EXP_DECO"), Find("GDR_PTY_OPTION_ITEM_DECO")}) Show(w, false);
        const game::AcademyView* a = context_.state != nullptr ? &context_.state->academy : nullptr;
        if (auto* r = ui::As<ui::RadioButton>(Child(40))) r->selected = a != nullptr && modify && a->Info().matchKind == 2 ? 1 : 0;
        if (auto* e = ui::As<ui::Edit>(Child(49))) e->limit = int(game::academy::kMatchTitleMax);
        Put(Child(49), a != nullptr && modify ? a->Info().matchTitle : std::string());
        if (ui::Widget* title = Child(0); title != nullptr && ui::As<ui::Static>(title) != nullptr) {
            title->SetText(context_.Text(modify ? "UIIT_PAG_PARTYMATCH_MODIFY" : "UIIT_PAG_PARTYMATCH_RECORD"));
        }
    }

    void Setup(bool modify) {
        modify_ = modify;
        const game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        const game::PartyView& party = state->party;
        proto::PartyMatchForm form;
        if (modify && party.ownEntry != 0) {
            for (const proto::PartyMatchEntry& e : party.listing.entries) {
                if (e.entry == party.ownEntry) form = e.form;
            }
            if (form.title.empty()) form = party.ownForm;
        }
        if (form.title.empty()) {
            // Ours: the levels about the player's, the party's (or the next party's) settings.
            const int level = state->self.level;
            form.levelMin = std::uint8_t(std::clamp(level - 10, 1, 255));
            form.levelMax = std::uint8_t(std::clamp(level + 10, 1, 255));
            form.settings = party.InParty() ? party.settings : party.preferred;
            form.purpose = 0;
        }
        if (auto* r = ui::As<ui::RadioButton>(Child(40))) r->selected = form.purpose;
        Put(Child(42), std::to_string(form.levelMin));
        Put(Child(43), std::to_string(form.levelMax));
        if (auto* c = ui::As<ui::CheckBox>(Child(70))) c->checked = (form.settings & game::kPartyExpShare) != 0;
        if (auto* c = ui::As<ui::CheckBox>(Child(71))) c->checked = (form.settings & game::kPartyItemShare) != 0;
        anyone_ = std::uint8_t(form.settings & game::kPartyAnyoneInvites);
        purpose_ = -1;
        Put(Child(49), form.title);
        if (ui::Widget* title = Child(0); title != nullptr && ui::As<ui::Static>(title) != nullptr) {
            title->SetText(context_.Text(modify ? "UIIT_PAG_PARTYMATCH_MODIFY" : "UIIT_PAG_PARTYMATCH_RECORD"));
        }
    }

    void Update(ui::Frame&) override {
        if (academy_) return;
        Put(Child(45), PartyExpText(context_, Settings()));
        Put(Child(47), PartyItemText(context_, Settings()));
        // A purpose chosen: its default title, unless the player wrote another.
        const auto* r = ui::As<ui::RadioButton>(Child(40));
        if (r == nullptr || r->selected == purpose_) return;
        purpose_ = r->selected;
        ui::Widget* title = Child(49);
        if (title == nullptr) return;
        bool isDefault = title->text().empty();
        for (int p = 0; p < game::kPartyPurposes && !isDefault; ++p) isDefault = title->text() == DefaultTitle(p);
        if (isDefault) Put(title, DefaultTitle(purpose_));
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this;
        if (!mine || (notice.control != 60 && notice.control != 61 && notice.control != 2)) return ui::MainFrame::OnNotice(notice);
        if (notice.control == 60 && academy_ && context_.state != nullptr) {
            //'s checks: a subject; the chosen class with a place left.
            const std::string title = Child(49) != nullptr ? Child(49)->text() : std::string();
            const auto* r = ui::As<ui::RadioButton>(Child(40));
            const std::uint8_t kind = std::uint8_t(r != nullptr && r->selected == 1 ? 2 : 1);
            const auto post = [&](const char* key) { context_.state->Post(game::GameMessage::Kind::General, context_.Text(key), context_.time); };
            if (title.empty()) {
                post("UIIT_STT_TC_MACHING_TITLE_ERROR");
                return true;
            }
            const game::AcademyView& a = context_.state->academy;
            if (kind == 2 && a.OfClass(game::academy::Class::Assistant).size() >= std::size_t(game::academy::kMostAssistants)) {
                post("UIIT_MSG_TC_ERROR_SECOND_GUARDIAN_COUNT_OVER");
                return true;
            }
            if (kind == 1 && a.OfClass(game::academy::Class::Apprentice).size() >= std::size_t(game::academy::kMostApprentices)) {
                post("UIIT_MSG_TC_ERROR_STUDENT_COUNT_OVER");
                return true;
            }
            if (context_.academyRequest) {
                if (modify_) context_.academyRequest(proto::AcademyMatchModify::kId, proto::Encode(proto::AcademyMatchModify{kind, title}));
                else context_.academyRequest(proto::AcademyMatchRegister::kId, proto::Encode(proto::AcademyMatchRegister{kind, title}));
            }
        } else if (notice.control == 60 && context_.state != nullptr && context_.hud != nullptr) {
            // The client's checks: a level range from 1, a title.
            const int levelMin = ByteOf(Child(42)), levelMax = ByteOf(Child(43));
            const std::string title = Child(49) != nullptr ? Child(49)->text() : std::string();
            game::PartyView& party = context_.state->party;
            if (levelMin < 1 || levelMax < levelMin) {
                party.notices.push_back({"UIIT_MSG_PARTYMATCH_RECORD_ERROR_LEVEL", {}});
                return true;
            }
            if (title.empty()) {
                party.notices.push_back({"UIIT_MSG_PARTYMATCH_RECORD_ERROR_TITLE", {}});
                return true;
            }
            const int purpose = std::max(0, ui::As<ui::RadioButton>(Child(40)) != nullptr ? ui::As<ui::RadioButton>(Child(40))->selected : 0);
            party.ownForm = {Settings(), std::uint8_t(purpose), std::uint8_t(levelMin), std::uint8_t(levelMax), title};
            if (!party.InParty()) party.preferred = Settings();
            context_.hud->Request({HudRequest::Kind::PartyMatchRegister, Settings() | purpose << 8, title, {}, levelMin | levelMax << 8});
        }
        if (ui_ != nullptr) ui_->Close(this);
        return true;
    }

private:
    std::uint8_t Settings() const {
        std::uint8_t s = anyone_;
        if (const auto* c = ui::As<ui::CheckBox>(Child(70)); c != nullptr && c->checked) s |= game::kPartyExpShare;
        if (const auto* c = ui::As<ui::CheckBox>(Child(71)); c != nullptr && c->checked) s |= game::kPartyItemShare;
        return s;
    }
    std::string DefaultTitle(int purpose) const {
        // Hunting the first, trade the second, quests the third, thieves the fourth.
        static constexpr const char* kTitles[] = {"UIIT_MSG_PARTYMATCH_RECORD_DEFAULT1", "UIIT_MSG_PARTYMATCH_RECORD_DEFAULT3",
                                                  "UIIT_MSG_PARTYMATCH_RECORD_DEFAULT2", "UIIT_MSG_PARTYMATCH_RECORD_DEFAULT4"};
        return purpose >= 0 && purpose < game::kPartyPurposes ? context_.Text(kTitles[purpose]) : std::string();
    }

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    bool modify_ = false, academy_ = false;
    int purpose_ = -1;
    std::uint8_t anyone_ = 0;
};

// The leader's box for a join request (CIFPartyMatchReqJoin, ginterface 131): who asks, its level and
// race; Confirm or Cancel (closing it says no).
class PartyMatchRequestBox : public ui::MainFrame {
public:
    PartyMatchRequestBox(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        if (ui::Widget* title = Child(0)) title->SetText(context_.Text("UIIT_PAG_PARTYMATCH_JOINREQUEST"));
        for (int id : {34, 35}) Show(Child(id), false); // the masteries: not told (ours)
    }

    void Update(ui::Frame&) override {
        const game::PlayerState* state = context_.state;
        if (state == nullptr || !state->party.joinRequest) return;
        const proto::PartyMatchJoinRequest& r = *state->party.joinRequest;
        Put(Child(21), FormatText(context_.Text("UIIT_STT_PARTYMATCH_JOIN_REQUEST"), {r.name}));
        Put(Child(30), std::to_string(r.level));
        Put(Child(31), RaceText(context_, r.race));
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this;
        if (!mine || (notice.control != 45 && notice.control != 46 && notice.control != 2)) return ui::MainFrame::OnNotice(notice);
        if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::PartyMatchAnswer, notice.control == 45 ? 1 : 0});
        if (context_.state != nullptr) context_.state->party.joinRequest.reset();
        if (ui_ != nullptr) ui_->Close(this);
        return true;
    }

private:
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
};

// The applicant's box while the leader decides (CIFPartyJoinProgress, ifpartymatch.txt's
// JoinProgress): the request's line and the time going by.
class PartyJoinProgressBox : public ui::MainFrame {
public:
    PartyJoinProgressBox(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        if (ui::Widget* title = Child(0)) title->SetText(context_.Text("UIIT_PAG_PARTYMATCH_JOINPROGRESS"));
        if (auto* g = ui::As<ui::Gauge>(Child(11))) g->SetValue(0.0f, true);
    }

    void Update(ui::Frame&) override {
        const game::PlayerState* state = context_.state;
        if (state != nullptr && state->party.joining == 0 && state->academy.joining) {
            // The academy's join request (the matching window's JoinProgress section).
            Put(Child(5), FormatText(context_.Text("UIIT_STT_PARTYMATCH_JOIN_PROGRESS"), {state->academy.joiningGuardian, state->self.name}));
            const double gone = (context_.time - state->academy.joiningAt) / double(game::academy::kInviteSeconds);
            if (auto* g = ui::As<ui::Gauge>(Child(11))) g->SetValue(float(std::clamp(gone, 0.0, 1.0)), true);
            return;
        }
        if (state == nullptr || state->party.joining == 0) return;
        const game::PartyView& party = state->party;
        std::string leader;
        for (const proto::PartyMatchEntry& e : party.listing.entries) {
            if (e.entry == party.joining) leader = e.name;
        }
        Put(Child(5), FormatText(context_.Text("UIIT_STT_PARTYMATCH_JOIN_PROGRESS"), {leader, state->self.name}));
        const double gone = (context_.time - party.joiningAt) / double(game::kPartyProposalSeconds);
        if (auto* g = ui::As<ui::Gauge>(Child(11))) g->SetValue(float(std::clamp(gone, 0.0, 1.0)), true);
    }

    void Draw(ui::Frame& frame) override {
        if (auto* g = ui::As<ui::Gauge>(Child(11))) g->FitToTexture(frame.paint, std::string(kPartyArt) + "pt_progress.ddj");
        ui::MainFrame::Draw(frame);
    }

private:
    HudContext& context_;
};

} // namespace

// A simple box made to the size of one of the original's kinds (centred again): its text hidden (the kind's own lines are
// placed), the frame's pieces and background tile reaching the right or the bottom edge following it, the close button
// with the right edge, Yes and No at their places.
void ShapeBox(ConfirmBox& box, float width, float height, float yesX, float noX, float buttonsY) {
    const float oldW = box.rect.w, oldH = box.rect.h, dw = width - oldW, dh = height - oldH;
    box.rect.x += std::floor(-dw / 2.0f);
    box.rect.y += std::floor(-dh / 2.0f);
    box.rect.w = width;
    box.rect.h = height;
    for (const auto& child : box.children()) {
        const int id = child->id();
        if (id == 251) {
            child->visible = false;
        } else if (id == 2) {
            child->rect.x += dw;
        } else if (id == 252 || id == 253) {
            child->rect.x = id == 252 ? yesX : noX;
            child->rect.y = buttonsY;
        } else {
            if (child->rect.x + child->rect.w >= oldW - 40.0f) child->rect.w += dw;
            if (child->rect.y + child->rect.h >= oldH - 40.0f) child->rect.h += dh;
        }
    }
}

// A line of the box (a static; `ddj` its picture behind, a black box), its text aligned (0 left, 1 centre).
void PutLine(ui::Interface& ui, ConfirmBox& box, const std::string& text, float x, float y, float w, float h, const char* ddj, int align) {
    assets::UiLayoutNode n;
    n.type = "CIFStatic";
    n.name = "GDR_MSGBOX_LINE";
    if (ddj != nullptr) n.ddj = ddj;
    n.hAlign = align;
    n.rect = {x, y, w, h};
    if (ui::Widget* line = box.Add(ui.Make(n))) line->SetText(text);
}


ConfirmBox* OpenPartyInviteBox(HudContext& c, const std::string& from, std::uint8_t settings, std::function<void(bool yes)> answer) {
    if (c.hud == nullptr || c.hud->interface() == nullptr) return nullptr;
    ui::Interface& ui = *c.hud->interface();
    ConfirmBox* box = c.hud->Confirm("UIIT_STT_CONFIRM_BOX", {}, std::move(answer));
    if (box == nullptr) return nullptr;
    // The original's kind 6 (8 the same): 300 x 176, the lines at 16, 48 and 16, 64, the settings in two black boxes
    // (msgbox_blackbox.ddj, 100 x 24) at 44, 93 and 156, 93, "anyone may invite" at 6, 121, Accept and Refuse at
    // 72, 139 and 152, 139.
    ShapeBox(*box, 300.0f, 176.0f, 72.0f, 152.0f, 139.0f);
    if (ui::Widget* yes = box->Child(252)) yes->SetText(c.Text("UIIT_STT_ACCEPT"));
    if (ui::Widget* no = box->Child(253)) no->SetText(c.Text("UIIT_STT_REFUSE"));
    const auto put = [&](const std::string& text, float x, float y, float w, float h, const char* ddj, int align) {
        PutLine(ui, *box, text, x, y, w, h, ddj, align);
    };
    put(FormatText(c.Text("UIIT_STT_PARTY_SOMEUSER"), {from}), 16.0f, 48.0f, 268.0f, 16.0f, nullptr, 0);
    put(c.Text("UIIT_STT_PARTY_PROPOSAL_ASK"), 16.0f, 64.0f, 268.0f, 16.0f, nullptr, 0);
    put(PartyExpText(c, settings), 44.0f, 93.0f, 100.0f, 24.0f, "interface\\messagebox\\msgbox_blackbox.ddj", 1);
    put(PartyItemText(c, settings), 156.0f, 93.0f, 100.0f, 24.0f, "interface\\messagebox\\msgbox_blackbox.ddj", 1);
    if ((settings & game::kPartyAnyoneInvites) != 0) put(c.Text("UIIT_STT_PARTY_JOIN_ANYONE"), 6.0f, 121.0f, 288.0f, 16.0f, nullptr, 1);
    return box;
}

ConfirmBox* OpenAcademyJoinBox(HudContext& c, const std::string& from, int level, int maxLevel, std::uint8_t race, std::function<void(bool yes)> answer) {
    if (c.hud == nullptr || c.hud->interface() == nullptr) return nullptr;
    ui::Interface& ui = *c.hud->interface();
    ConfirmBox* box = c.hud->Confirm("UIIT_PAG_PARTYMATCH_JOINREQUEST", {}, std::move(answer));
    if (box == nullptr) return nullptr;
    // The original's kind 0x0E: 360 x 215, the request at 14, 59 (300 wide), "Level" at 15, 121 and "Race" at 181, 121
    // (58 x 11), their values in black boxes (58 x 20) at 98, 121 and 264, 121 ("%d(%d)": the level and the highest),
    // the buttons at 101, 174 and 189, 174. Ours: the request's markup as plain lines (its name's colour not kept).
    ShapeBox(*box, 360.0f, 215.0f, 101.0f, 189.0f, 174.0f);
    std::string text = FormatText(c.Text("UIIT_STT_TC_JOIN_REQUEST"), {from});
    std::vector<std::string> lines(1);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '<') {
            lines.back() += text[i++];
            continue;
        }
        const std::size_t end = text.find('>', i);
        if (end == std::string::npos) break;
        if (text.compare(i, end - i + 1, "<br>") == 0) lines.emplace_back();
        i = end + 1;
    }
    for (std::size_t k = 0; k < lines.size(); ++k) PutLine(ui, *box, lines[k], 14.0f, 59.0f + 16.0f * float(k), 300.0f, 16.0f, nullptr, 0);
    PutLine(ui, *box, c.Text("UIIT_CTL_WARENETWORK_RESULT_LEVEL"), 15.0f, 121.0f, 58.0f, 20.0f, nullptr, 0);
    PutLine(ui, *box, c.Text("UIIT_CTL_PARTYMATCH_PSEARCH_LIST_RACE"), 181.0f, 121.0f, 58.0f, 20.0f, nullptr, 0);
    PutLine(ui, *box, std::to_string(level) + "(" + std::to_string(maxLevel) + ")", 98.0f, 121.0f, 58.0f, 20.0f, "interface\\messagebox\\msgbox_blackbox.ddj", 1);
    PutLine(ui, *box, RaceText(c, race), 264.0f, 121.0f, 58.0f, 20.0f, "interface\\messagebox\\msgbox_blackbox.ddj", 1);
    return box;
}

std::string PartyExpText(const HudContext& context, std::uint8_t settings) {
    return context.Text((settings & game::kPartyExpShare) != 0 ? "UIIT_STT_PARTY_EXP_SHARE" : "UIIT_STT_PARTY_EXP_SELF");
}

std::string PartyItemText(const HudContext& context, std::uint8_t settings) {
    return context.Text((settings & game::kPartyItemShare) != 0 ? "UIIT_STT_PARTY_ITEM_SHARE" : "UIIT_STT_PARTY_ITEM_SELF");
}

std::string PartyPurposeText(const HudContext& context, std::uint8_t purpose) {
    switch (purpose) {
    case 0: return context.Text("UIIT_CTL_PARTYMATCH_PSEARCH_OBJECTCOMBAT");
    case 1: return context.Text("UIIT_STT_QUEST");
    case 2: return context.Text("UIIT_CTL_PARTYMATCH_PSEARCH_FIND_OBJECTTRADER");
    case 3: return context.Text("UIIT_CTL_PARTYMATCH_PSEARCH_FIND_OBJECTTHIEF");
    default: return {};
    }
}

void RegisterPartyWindows(ui::Interface& ui, HudContext& context) {
    HudContext* c = &context;
    ui.Register("CIFParty", [c](const assets::UiLayoutNode& n) { return std::make_unique<PartyWindow>(n, *c); });
    ui.Register("CIFQuickPartyWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<QuickPartyWindow>(n, *c); });
    ui.Register("CIFSetPartyMode", [c](const assets::UiLayoutNode& n) { return std::make_unique<SetPartyModeBox>(n, *c); });
    ui.Register("CNIFTargetMenu", [c](const assets::UiLayoutNode& n) { return std::make_unique<TargetMenu>(n, *c); });
    ui.Register("CIFPartyMatch", [c](const assets::UiLayoutNode& n) { return std::make_unique<PartyMatchWindow>(n, *c); });
    ui.Register("CIFPartyMatchRegister", [c](const assets::UiLayoutNode& n) { return std::make_unique<PartyMatchRegisterBox>(n, *c); });
    ui.Register("CIFPartyMatchReqJoin", [c](const assets::UiLayoutNode& n) { return std::make_unique<PartyMatchRequestBox>(n, *c); });
    ui.Register("CIFPartyJoinProgress", [c](const assets::UiLayoutNode& n) { return std::make_unique<PartyJoinProgressBox>(n, *c); });
}

void OpenSetPartyMode(ui::Interface& ui, std::uint8_t settings, std::function<void(std::uint8_t)> confirmed) {
    for (ui::Widget* w : ui.Windows()) {
        if (dynamic_cast<SetPartyModeBox*>(w) != nullptr && w->visible) ui.Close(w); // one at a time
    }
    assets::UiLayoutNode node;
    node.type = "CIFSetPartyMode";
    node.name = "GDR_SETPARTYMODE";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.rect = {0.0f, 0.0f, 300.0f, 196.0f}; // ours: the layout's content with the frame (its size is set in code)
    auto* box = dynamic_cast<SetPartyModeBox*>(ui.Open(node));
    if (box == nullptr) return;
    ui.Centre(*box, ui.screen());
    box->Ask(settings, std::move(confirmed));
}

void OpenTargetMenu(ui::Interface& ui, HudContext& context, std::uint32_t entity, const std::string& name, ui::Vec2 at) {
    CloseTargetMenu(ui);
    assets::UiLayoutNode node;
    node.type = "CNIFTargetMenu";
    node.name = "GDR_TARGET_MENU";
    node.id = kTargetMenuId;
    node.ddj = "interface\\frame\\ub_new_wnd_";
    const ui::Vec2 screen = ui.screen();
    node.rect = {std::clamp(at.x, 0.0f, std::max(0.0f, screen.x - 102.0f)), std::clamp(at.y, 0.0f, std::max(0.0f, screen.y - 142.0f)), 102.0f, 142.0f};
    if (auto* menu = dynamic_cast<TargetMenu*>(ui.Open(node))) menu->Set(entity, name);
    (void)context;
}

void CloseTargetMenu(ui::Interface& ui) {
    if (ui::Widget* menu = ui.Window(kTargetMenuId)) ui.Close(menu);
}

void InviteToParty(ui::Interface& ui, HudContext& context, std::uint32_t entity) {
    if (context.state == nullptr || context.hud == nullptr || entity == 0) return;
    game::PartyView& party = context.state->party;
    Hud* hud = context.hud;
    if (party.InParty()) {
        hud->Request({HudRequest::Kind::PartyInvite, int(entity), {}, {}, party.settings});
        return;
    }
    // A new party: its settings first (the Set window's box, then the request with them).
    OpenSetPartyMode(ui, party.preferred, [hud, &party, entity](std::uint8_t s) {
        party.preferred = s;
        hud->Request({HudRequest::Kind::PartyInvite, int(entity), {}, {}, s});
    });
}

void OpenPartyMatchRegister(ui::Interface& ui, HudContext& context, bool modify) {
    if (auto* open = FindWindow<PartyMatchRegisterBox>(ui)) ui.Close(open);
    const assets::UiLayoutNode* node = NodeIn(ui, kMatchLayout, "PartyRegister", 105);
    if (node == nullptr) return;
    auto* box = dynamic_cast<PartyMatchRegisterBox*>(ui.Open(*node));
    if (box == nullptr) return;
    ui.Centre(*box, ui.screen());
    box->Setup(modify);
    (void)context;
}

void OpenAcademyMatchRegister(ui::Interface& ui, HudContext& context, bool modify) {
    if (auto* open = FindWindow<PartyMatchRegisterBox>(ui)) ui.Close(open);
    const assets::UiLayoutNode* node = NodeIn(ui, "resinfo\\ifmentormatch.txt", "MentoRegister", 105);
    if (node == nullptr) return;
    auto* box = dynamic_cast<PartyMatchRegisterBox*>(ui.Open(*node));
    if (box == nullptr) return;
    ui.Centre(*box, ui.screen());
    box->SetupAcademy(modify);
    (void)context;
}

void SyncAcademyProgress(ui::Interface& ui, HudContext& context) {
    if (context.state == nullptr) return;
    const bool joining = context.state->academy.joining;
    PartyJoinProgressBox* progress = FindWindow<PartyJoinProgressBox>(ui);
    if (joining && progress == nullptr) {
        if (const assets::UiLayoutNode* node = NodeIn(ui, "resinfo\\ifmentormatch.txt", "JoinProgress", 200)) {
            if (ui::Widget* w = ui.Open(*node)) ui.Centre(*w, ui.screen());
        }
    } else if (!joining && progress != nullptr && context.state->party.joining == 0) {
        ui.Close(progress);
    }
}

void SyncPartyMatchBoxes(ui::Interface& ui, HudContext& context) {
    if (context.state == nullptr || context.hud == nullptr) return;
    const game::PartyView& party = context.state->party;
    // The leader's request box while a request waits.
    const bool asked = party.joinRequest.has_value();
    if (asked && !context.hud->Shown(131)) context.hud->Show(131);
    if (!asked && context.hud->Shown(131)) context.hud->Hide(131);
    // The applicant's box while it waits.
    PartyJoinProgressBox* progress = FindWindow<PartyJoinProgressBox>(ui);
    if (party.joining != 0 && progress == nullptr) {
        if (const assets::UiLayoutNode* node = NodeIn(ui, kMatchLayout, "JoinProgress", 100)) {
            if (ui::Widget* w = ui.Open(*node)) ui.Centre(*w, ui.screen());
        }
    } else if (party.joining == 0 && progress != nullptr && !context.state->academy.joining) {
        ui.Close(progress);
    }
}

} // namespace sr::client
