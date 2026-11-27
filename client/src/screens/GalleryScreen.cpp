#include "screens/GalleryScreen.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <tuple>

#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "sr/ui/Controls.h"
#include "gui/HudParty.h"
#include "sr/game/BetaQuests.h"
#include "title/TitleShared.h"

namespace sr::client {

namespace {

// The widget of a type somewhere under `w` (depth first).
bool EquipmentPage(const std::string& page) { return page == "equipment" || page == "dress" || page == "dress-plain" || page == "worn-state"; }

template <typename T>
T* FindUnder(ui::Widget* w) {
    if (w == nullptr) return nullptr;
    if (auto* t = dynamic_cast<T*>(w)) return t;
    for (const auto& c : w->children()) {
        if (T* t = FindUnder<T>(c.get())) return t;
    }
    return nullptr;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The box around a window and everything it shows, relative to the window's place (its
// controls may stick out above or left of it: the underbar's ornaments, the main window's tabs).
void Bounds(const ui::Widget& w, ui::Vec2 origin, ui::Vec2& lo, ui::Vec2& hi) {
    const ui::Rect r = w.ScreenRect();
    lo = {std::min(lo.x, r.x - origin.x), std::min(lo.y, r.y - origin.y)};
    hi = {std::max(hi.x, r.Right() - origin.x), std::max(hi.y, r.Bottom() - origin.y)};
    for (const auto& c : w.children()) Bounds(*c, origin, lo, hi);
}

// Sample data, so every control shows something (the gallery has no game behind it).
void Sample(ui::Interface& interface, ui::Widget& w, int& n) {
    ++n;
    if (auto* gauge = ui::As<ui::Gauge>(&w)) gauge->SetValue(0.65f, true);
    if (auto* edit = ui::As<ui::Edit>(&w)) edit->SetText("Sample");
    if (auto* list = ui::As<ui::ListView>(&w)) {
        for (int i = 1; i <= 5; ++i) list->rows.push_back({"Row " + std::to_string(i)});
        list->selected = 1;
    }
    if (auto* box = ui::As<ui::TextBox>(&w); box && box->text().empty()) {
        box->SetText("A text box wraps its text at its width, line by line, with a drop shadow.");
    }
    if (auto* rich = ui::As<ui::RichText>(&w); rich && rich->text().empty()) {
        rich->SetText("<sml2><center><font color=\"255,255,200,80\">Rich text</font></center><br>SML2 markup: lines, "
                      "<font color=\"255,120,200,255\">colours</font> and alignment.</sml2>");
    }
    if (auto* combo = ui::As<ui::ComboBox>(&w)) {
        combo->items = {"First", "Second", "Third", "Fourth", "Fifth", "Sixth"};
        combo->Select(0);
    }
    if (auto* slot = ui::As<ui::Slot>(&w)) {
        slot->icon = n % 3 == 0 ? "icon\\skillmastery\\china\\mastery_sword.ddj" : "icon\\item\\fort\\healing_flag.ddj";
        slot->count = n % 3 == 1 ? 25 : 1;
        slot->tooltip = "Sample item\nA tooltip of two lines";
    }
    if (auto* radio = ui::As<ui::RadioButton>(&w); radio && radio->children().size() <= 1) {
        radio->SetUnits(interface, {"Option one", "Option two"});
    }
    if (auto* spin = ui::As<ui::Spin>(&w)) spin->max = 4;
    if (auto* bar = ui::As<ui::ScrollBar>(&w)) bar->SetRange(0, 10);
    for (std::size_t i = 0; i < w.children().size(); ++i) Sample(interface, *w.children()[i], n);
}

// A character to show the HUD with: a level-7 character in a field by Jangan, a few players
// around, a monster selected, potions and skills on the bar, two buffs and a state.
void SampleCharacter(game::PlayerState& s, game::WorldView& world, double now, const game::proto::WorldPosition* at = nullptr) {
    namespace proto = game::proto;
    proto::WorldEntered entered;
    entered.entity = 1;
    entered.character.model = 1907; // CHAR_CH_MAN_ADVENTURER
    entered.character.name = "Altay";
    entered.character.level = 7;
    entered.character.hp = 495;
    entered.character.mp = 292;
    entered.character.strength = 32;
    entered.character.intelligence = 26;
    entered.position = at != nullptr ? *at : game::proto::WorldPosition{std::uint16_t((97 << 8) | 168), 960.0f, 0.0f, 1260.0f};
    entered.yaw = 0.6f;
    world.Apply(game::TitleEvent{entered});
    s.Apply(game::TitleEvent{entered});
    for (std::uint32_t i = 0; i < 4; ++i) {
        proto::EntityAppeared other;
        other.entity = 10 + i;
        other.character.name = "Walker";
        other.position = {entered.position.region, 960.0f + 90.0f * float(i) - 120.0f, 0.0f, 1260.0f + 60.0f * float(i % 2) - 140.0f};
        world.Apply(game::TitleEvent{other});
    }
    game::CharacterStatus& c = s.self;
    c.hp = 126;
    c.maxHp = 495;
    s.SetLevels({0, 118, 274, 498, 910, 1650, 2865, 4805, 7735}); // leveldata.txt's first levels
    c.experience = c.experienceToNext * 7961 / 10000;
    c.skillPoints = 24;
    c.skillExperience = 260;
    c.berserk = 2;
    c.physicalAttack[0] = 24;
    c.physicalAttack[1] = 31;
    c.magicalAttack[0] = 19;
    c.magicalAttack[1] = 25;
    c.physicalDefence = 18;
    c.magicalDefence = 16;
    c.hitRatio = 21;
    c.parryRatio = 14;
    s.target.entity = 2;
    s.target.kind = game::TargetStatus::Kind::Monster;
    s.target.name = "Barus Wolf";
    s.target.level = 6;
    s.target.maxHp = 168;
    s.target.hp = 130;
    // Its states (the target window's row, K3): stunned, burning, frostbitten.
    for (const std::uint32_t bit : {0x4000u, 0x8u, 0x2u}) s.target.states.push_back({game::ActiveEffect::Kind::State, bit, 0, true, now, 10.0});
    // The bar's samples: the gallery's own icon names (the tables give them in the game).
    using K = game::QuickSlot::Kind;
    auto& q = s.quickSlots.slots;
    q[0] = {K::Skill, 1};
    q[1] = {K::Item, 13};
    q[2] = {K::Item, 14};
    q[3] = {K::Skill, 2};
    q[4] = {K::Skill, 3};
    q[6] = {K::Action, 1000};
    q[41] = {K::Item, 13};
    q[42] = {K::Item, 14};
    s.effects.push_back({game::ActiveEffect::Kind::Skill, 2, 0, false, now, 300.0});
    s.effects.push_back({game::ActiveEffect::Kind::Skill, 3, 0, false, now - 200.0, 300.0});
    s.effects.push_back({game::ActiveEffect::Kind::State, 0x10, 0, true, now, 20.0});
    // A link the character made (proto::LinkAdded): its board's entry names whom it is on; and two
    // marks (Tag Point) on its minimap: a walker in sight, and one far east (held on the edge).
    s.effects.push_back({game::ActiveEffect::Kind::Skill, 4, 9, false, now, 900.0, "Walker"});
    s.marks.push_back({9, 11, {}, 0.0f});
    s.marks.push_back({10, 99, {entered.position.region, 1900.0f, 0.0f, 900.0f}, 1.2f});
    s.Post(game::GameMessage::Kind::Guide, "[Guide] If you don't want to see Guide, Go to [Game Setting] in the Option menu then turn off [Guide] function.", now);
    s.Post(game::GameMessage::Kind::Acquisition, "[149]Experience Points gained.", now);
    s.Post(game::GameMessage::Kind::Acquisition, "[90]skill experience point gained", now);
    // The chat: a line of every kind, the player's own among them (they come with the server's answer).
    const auto line = [&](proto::ChatKind kind, std::uint32_t sender, const char* name, const char* text) {
        proto::ChatLine l;
        l.kind = kind;
        l.sender = sender;
        l.name = name;
        l.text = text;
        s.Apply(game::TitleEvent{l});
    };
    const auto own = [&](proto::ChatKind kind, const char* target, const char* text) {
        const std::uint8_t index = s.QueueChat(kind, target, text);
        s.Apply(game::TitleEvent{proto::ChatAnswer{proto::ChatResult::Ok, kind, index, 0}});
    };
    line(proto::ChatKind::All, 10, "Walker", "anyone selling a blade?");
    own(proto::ChatKind::All, "", "I have one, 20k");
    line(proto::ChatKind::Whisper, 0, "Kira", "where are you?");
    own(proto::ChatKind::Whisper, "Kira", "Jangan, west gate");
    line(proto::ChatKind::Party, 0, "Ann", "heal please");
    line(proto::ChatKind::Guild, 0, "Bo", "guild meeting at 8");
    line(proto::ChatKind::Union, 0, "Lee", "union war tonight");
    line(proto::ChatKind::Global, 0, "Merchant", "Selling elixirs, whisper me");
    line(proto::ChatKind::Speech, 0, "Demon Shaitan", "You will not leave this place."); // a monster's line (as named by PlayScreen)
    line(proto::ChatKind::Notice, 0, "", "The server will be checked in 10 minutes.");
    s.Notify(game::Banner::Kind::Notice, "The server will be checked in 10 minutes.", now);
    s.Notify(game::Banner::Kind::Warning, "[Tiger Girl]has appeared.", now);
    s.Notify(game::Banner::Kind::Quest, "Quest completed: Barus Wolf hunt", now);
}

// A party for the party's windows: Ann leads, the sample character and two
// more in it, shared exp and items; a page of party matching listings (the player's own among them).
// A sample academy (--page academy): the sample character its guardian, two assistants (one
// away), four apprentices of every grade, one past 40 (its row offers Graduate), its honour rank 2.
void SampleAcademy(game::PlayerState& s) {
    namespace proto = game::proto;
    namespace academy = game::academy;
    proto::AcademyInfo info;
    info.id = 3;
    info.rank = 2;
    info.graduates = 14;
    info.evaluation = 260;
    info.noticeTitle = "Hunting at the Tarim Basin tonight";
    const auto member = [](std::uint32_t character, std::string name, academy::Class c, int level, int maxLevel, std::uint32_t model, bool online) {
        proto::AcademyMember m;
        m.character = character;
        m.name = std::move(name);
        m.model = model;
        m.memberClass = std::uint8_t(c);
        m.grade = academy::GradeOf(c, maxLevel);
        m.level = std::uint8_t(level);
        m.maxLevel = std::uint8_t(maxLevel);
        m.online = online ? 1 : 0;
        return m;
    };
    info.members = {member(1, s.self.name, academy::Class::Guardian, 95, 96, 1907, true),
                    member(2, "Ann", academy::Class::Assistant, 52, 52, 1920, true),
                    member(3, "Bo", academy::Class::Assistant, 44, 45, 14880, false),
                    member(4, "Kira", academy::Class::Apprentice, 8, 8, 14896, true),
                    member(5, "Lee", academy::Class::Apprentice, 17, 17, 1907, true),
                    member(6, "Mei", academy::Class::Apprentice, 29, 29, 1920, false),
                    member(7, "Tom", academy::Class::Apprentice, 41, 41, 14880, true)};
    info.matchEntry = 7;
    info.matchKind = 1;
    info.matchTitle = "Learn with us";
    s.academy.Apply(info, s.self.name);
    // The honour ranking's first places and a page of the matching list.
    proto::AcademyRanking ranking;
    const char* guardians[] = {"Moon", "Altay", "Sakura", "Drake", "Ivy", "Hakan", "Lune", "Brann", "Pip", "Rook", "Zed", "Kael"};
    for (int i = 0; i < 12; ++i) {
        ranking.rows.push_back({std::uint8_t(i + 1), academy::RankOfPlace(i + 1), guardians[i], std::uint8_t(110 - i * 3), std::uint32_t(60 - i * 4),
                                i % 3 == 0 ? std::string() : std::string("Dragons")});
    }
    s.academy.Apply(ranking, s.self.name);
    proto::AcademyMatchList list;
    list.page = 0;
    list.pages = 2;
    for (int i = 0; i < 9; ++i) {
        list.rows.push_back({std::uint32_t(3 + i * 4), std::uint8_t(95 - i * 4), std::uint8_t(i % 2), guardians[i],
                             i % 2 == 0 ? std::string("Hunting every evening, all welcome") : std::string("Trade runs and quests"),
                             std::uint32_t(30 - i * 3), std::uint8_t(1 + i % 8), std::uint8_t(1 + i % 5), std::uint8_t(1 + i % 2)});
    }
    s.academy.Apply(list, s.self.name);
}

void SampleParty(game::PlayerState& s) {
    namespace proto = game::proto;
    proto::PartyInfo info;
    info.party = 7;
    info.settings = game::kPartyExpShare | game::kPartyItemShare;
    info.leader = 101;
    // Their figures (characterdata ids: the portraits): a Chinese woman, the
    // sample character, a European man and woman, and a Chinese man. Kira is out of sight 750 away (a
    // darker face), Lee 1,100 away (past the experience's share: blue). Their two highest masteries.
    const auto member = [](std::uint32_t id, std::uint32_t entity, const char* name, int level, std::uint8_t race, int hp, int mp, std::uint32_t model,
                           std::uint32_t mastery1, std::uint32_t mastery2) {
        proto::PartyMemberRecord m;
        m.id = id;
        m.entity = entity;
        m.name = name;
        m.level = std::uint8_t(level);
        m.race = race;
        m.hpTenths = std::uint8_t(hp);
        m.mpTenths = std::uint8_t(mp);
        m.model = model;
        m.mastery1 = mastery1;
        m.mastery2 = mastery2;
        m.position = {std::uint16_t((97 << 8) | 168), 960.0f, 0.0f, 1260.0f};
        return m;
    };
    info.members = {member(101, 10, "Ann", 9, 0, 10, 8, 1920, 257, 275), member(102, 1, "Altay", 7, 0, 3, 6, 1907, 259, 0),
                    member(103, 11, "Bo", 8, 1, 7, 10, 14880, 513, 518), member(104, 0, "Kira", 6, 1, 5, 2, 14896, 515, 0),
                    member(105, 0, "Lee", 12, 0, 9, 9, 1907, 258, 273)};
    info.members[3].position.x -= 750.0f;
    info.members[4].position.z += 1100.0f;
    s.Apply(game::TitleEvent{info});
    proto::PartyMatchListing listing;
    listing.pages = 2;
    listing.own = 0;
    const auto entry = [](std::uint32_t id, const char* name, std::uint8_t race, std::uint8_t settings, std::uint8_t purpose, int low, int high,
                          int members, const char* title) {
        proto::PartyMatchEntry e;
        e.entry = id;
        e.leader = 200 + id;
        e.name = name;
        e.race = race;
        e.form = {settings, purpose, std::uint8_t(low), std::uint8_t(high), title};
        e.members = std::uint8_t(members);
        e.capacity = std::uint8_t(game::PartyCapacity(settings));
        return e;
    };
    listing.entries = {entry(3, "Ann", 0, 3, 0, 5, 15, 4, "For open hunting on the Silkroad!"),
                       entry(5, "Merchant", 0, 2, 2, 20, 40, 2, "Until the day the Silkroad is all covered with money!"),
                       entry(8, "Bo", 1, 1, 1, 1, 30, 3, "When will the quests end on the Silkroad?"),
                       entry(11, "Shade", 1, 0, 3, 30, 60, 1, "No need for blood or tears for thieves! Robbery is the only way to go!"),
                       entry(14, "Lee", 0, 3, 0, 40, 50, 6, "Tiger Girl tonight, healers welcome")};
    s.Apply(game::TitleEvent{listing});
}

std::string SampleIcon(const game::QuickSlot& q) {
    using K = game::QuickSlot::Kind;
    if (q.kind == K::Item) return q.value == 13 ? "icon\\item\\etc\\hp_potion_01.ddj" : "icon\\item\\etc\\mp_potion_01.ddj";
    if (q.kind == K::Action) return "icon\\action\\icon_cha_sit.ddj";
    if (q.kind == K::Skill) {
        if (q.value == 4) return "icon\\skill\\europe\\rog_stealtha_point_a.ddj";
        return q.value == 1 ? "icon\\skill\\china\\cold_bingpan_a.ddj" : q.value == 2 ? "icon\\skill\\china\\cold_ganggi_a.ddj" : "icon\\skill\\china\\cold_bingbyeok_a.ddj";
    }
    return {};
}

} // namespace

void GalleryScreen::Enter(ScreenContext& context) {
    host_.Load(context);
    scale_ = std::max(0.25f, static_cast<float>(context.config.GetDouble("ui-scale", 1.0)));
    samples_ = context.config.GetBool("samples", true);
    hudMode_ = context.config.GetBool("hud", false);
    page_ = context.config.GetString("page", "");
    useSlot_ = context.config.GetInt("use-slot", 0);
    {
        std::string error;
        if (!stallNetwork_.Load(context.files, error)) core::LogWarning("gallery: stall network: {}", error);
        hudContext_.stallNetwork = &stallNetwork_;
    }
    if (page_ == "skills" || page_.starts_with("party")) {
        // The Skills page needs skilldata and leveldata's mastery costs; the party's bars the masteries' icons.
        std::string error;
        if (!skills_.Load(context.files, error)) core::LogWarning("gallery: skills: {}", error);
        if (const auto file = context.files.Read("server_dep\\silkroad\\textdata\\leveldata.txt")) {
            assets::DecodeMasteryCosts(core::DecodeTextFile(file->Bytes()), masteryCosts_, error);
        }
    }
    if (page_ == "fortress-map") {
        std::string error;
        if (const auto file = context.files.Read("navmesh\\objectstring.ifo"); !file || !assets::DecodeObjectStrings(file->Bytes(), spots_, error)) {
            core::LogWarning("gallery: objectstring.ifo: {}", error);
        }
    }
    if (const std::string c = context.config.GetString("backdrop", ""); !c.empty()) {
        int r = 0, g = 0, b = 0;
        if (std::sscanf(c.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
            backdrop_[0] = float(std::clamp(r, 0, 255)) / 255.0f;
            backdrop_[1] = float(std::clamp(g, 0, 255)) / 255.0f;
            backdrop_[2] = float(std::clamp(b, 0, 255)) / 255.0f;
        }
    }
    if (const std::string at = context.config.GetString("mouse", ""); !at.empty()) {
        std::sscanf(at.c_str(), "%f,%f", &mouse_.x, &mouse_.y);
    }
    std::stringstream clicks(context.config.GetString("click", ""));
    for (std::string item; std::getline(clicks, item, ';');) {
        ui::Vec2 at, to;
        if (std::sscanf(item.c_str(), "d:%f,%f,%f,%f", &at.x, &at.y, &to.x, &to.y) == 4) {
            clicks_.push_back(at);
            dragsTo_.push_back(to);
        } else if (std::sscanf(item.c_str(), "%f,%f", &at.x, &at.y) == 2) {
            clicks_.push_back(at);
            dragsTo_.push_back(std::nullopt);
        }
    }
    std::stringstream open(context.config.GetString("open", ""));
    for (std::string item; std::getline(open, item, ',');) {
        if (!item.empty()) pending_.push_back(item);
    }
    if (hudMode_) {
        portraits_.Init(context.device);
        tables_ = LoadCharacterData(context.jobs, context.files, tablesJob_);
    }
}

void GalleryScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    context.jobs.Wait(tablesJob_); // it reads the file system
    portraits_.Exit(context.assets, snapshot, host_.textures());
    interface_ = nullptr;
    host_.Exit(snapshot);
}

void GalleryScreen::SamplePortraits(ScreenContext& context, FrameSnapshot& snapshot) {
    // The sample character dressed as the game dresses it, once the tables are in.
    if (!dressed_ && tables_ && tables_->Ready()) {
        dressed_ = true;
        const game::WorldView::Entity* self = sampleWorld_.Find(sampleWorld_.Self());
        std::vector<std::uint32_t> worn;
        if (self != nullptr) {
            for (const game::proto::EquippedItem& item : self->look.equipment) worn.push_back(item.item);
        }
        std::string error;
        if (!tables_->Usable() || self == nullptr ||
            !DressPortrait(context.files, tables_->characters, tables_->items, self->look.model, worn, self->look.height, selfLook_, error)) {
            core::LogWarning("gallery: no portrait: {}", tables_->Usable() ? error : std::string("the character tables did not load"));
        }
    }
    portraits_.Show(kSelfPortrait, selfLook_, true);
    // The item mall's preview: the sample character with the avatars tried on (PlayMall.cpp does the same in the game).
    if (mallShown_ && hud_.Created() && hud_.Shown(hud::kItemMall) && tables_->Usable()) {
        if (const game::WorldView::Entity* self = sampleWorld_.Find(sampleWorld_.Self())) {
            std::vector<std::uint32_t> worn;
            for (const game::proto::EquippedItem& item : self->look.equipment) worn.push_back(item.item);
            for (const auto& [kind, good] : mall_.worn) {
                const assets::MallGood* g = mallTable_.Find(good.first, good.second);
                const assets::ItemRow* row = g != nullptr ? tables_->items.Find(std::string_view(g->itemCodename)) : nullptr;
                if (row != nullptr) worn.push_back(row->refObjId);
            }
            PortraitLook look;
            std::string error;
            if (DressPortrait(context.files, tables_->characters, tables_->items, self->look.model, worn, self->look.height, look, error,
                              tables_->visuals.Size() > 0 ? &tables_->visuals : nullptr)) {
                portraits_.ShowFigure(kMallPreview, look, ItemMallPreviewYaw(hudContext_));
            }
        }
    }
    // The sample party's members (--page party): their figures as created, as the party bars show them.
    if (dressed_ && tables_->Usable() && sample_.party.InParty()) {
        for (const game::proto::PartyMemberRecord* m : sample_.party.Others()) {
            portraits_.Show(m->id, portraits_.FigureLook(m->model, context.files, tables_->characters, tables_->items), false);
        }
    }
    // The sample academy's members (--page academy).
    if (dressed_ && tables_->Usable() && sample_.academy.InAcademy()) {
        for (const game::proto::AcademyMember& m : sample_.academy.Info().members) {
            if (m.name == sample_.self.name) continue;
            portraits_.Show(AcademyPortraitKey(m.character), portraits_.FigureLook(m.model, context.files, tables_->characters, tables_->items), false);
        }
    }
    portraits_.Update(context.assets, snapshot, host_.textures());
    if (!dressed_ || !portraits_.Settled()) context.holdCapture = true; // a capture shows the faces
}

void GalleryScreen::OpenLayout(const std::string& spec) {
    std::string file = spec, section;
    if (const std::size_t colon = spec.find(':'); colon != std::string::npos) {
        file = spec.substr(0, colon);
        section = spec.substr(colon + 1);
    }
    file = Lower(file);
    if (!file.ends_with(".txt")) file += ".txt";
    if (!file.starts_with("resinfo\\")) file = "resinfo\\" + file;
    ui::Widget* window = interface_->Open(file, section, 0, {});
    if (window == nullptr) {
        core::LogWarning("gallery: no layout {} (section '{}')", file, section);
        return;
    }
    ++opened_;
    // Side by side in rows across the screen, 12 pixels apart, each with all it shows.
    ui::Vec2 lo{0, 0}, hi{0, 0};
    Bounds(*window, window->Origin(), lo, hi);
    const float w = hi.x - lo.x, h = hi.y - lo.y;
    if (flow_.x > 12.0f && flow_.x + w > screenWidth_) {
        flow_.x = 12.0f;
        flow_.y += rowHeight_ + 12.0f;
        rowHeight_ = 0.0f;
    }
    window->rect.x = flow_.x - lo.x;
    window->rect.y = flow_.y - lo.y;
    flow_.x += w + 12.0f;
    rowHeight_ = std::max(rowHeight_, h);
    if (!samples_) return;
    int n = 0;
    Sample(*interface_, *window, n);
    // Dropping a picked-up item on another slot of the window swaps them (a window's own rule;
    // the gallery's sample of the path a drop takes).
    window->onDrop = [](ui::Widget* source, ui::Widget* target) {
        auto* from = ui::As<ui::Slot>(source);
        auto* to = ui::As<ui::Slot>(target);
        if (from == nullptr || to == nullptr || from == to) return false;
        std::swap(from->icon, to->icon);
        std::swap(from->count, to->count);
        std::swap(from->tooltip, to->tooltip);
        return true;
    };
}

void GalleryScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    const float width = float(context.device.SwapchainWidth());
    snapshot.scene.active = false;
    snapshot.clearColor = {backdrop_[0], backdrop_[1], backdrop_[2], 1.0f};
    hudContext_.state = &sample_;
    hudContext_.options = context.options; // the windows as the options file has them (the system lines' autohide, ...)
    if (tables_ && tables_->Usable() && hudContext_.characters == nullptr) hudContext_.characters = &tables_->characters;
    hudContext_.keyedPicture = [files = &context.files, made = context.madeFiles](const std::string& source, const std::vector<std::uint32_t>& keys,
                                                                                  std::uint32_t colour) {
        return made != nullptr ? KeyedPicture(*files, *made, source, keys, colour) : std::string();
    };
#ifdef SR_BUG_REPORTS
    // The bug report's button and window on sample data: Send only logs what it would send.
    hudContext_.bugReport = [](BugReportForm form) {
        core::LogInfo("gallery: bug report '{}' ({} characters), category {}, screenshot {}, interface {}", form.subject, form.text.size(), form.category,
                      form.screenshot, form.withInterface);
    };
#endif
    if (!sampleGuild_.InGuild()) {
        // A guild of eight with a notice, in a union of three (the guild window's sample).
        sampleGuild_.SetSelf("Gallery");
        game::proto::GuildInfo g;
        g.id = 1;
        g.name = "Dragons";
        g.level = 4; // titles from level 4 (the Grant name button)
        g.points = 41'250;
        g.noticeTitle = "Hunting at Donwhang tonight";
        g.notice = "Meet at the east gate at nine.";
        g.unionId = 1;
        const char* names[] = {"Gallery", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta"};
        const char* titles[] = {"Founder", "Guard", "", "Scout", "", "", "", ""};
        const std::uint8_t positions[] = {game::kPositionCommander, game::kPositionDeputy, 0, game::kPositionEngineer, 0, 0, game::kPositionEngineer, 0};
        const std::uint32_t warPoints[] = {360, 600, 120, 360, 0, 240, 0, 0}; // the war page's ranks: two share one
        for (std::uint32_t i = 0; i < 8; ++i) {
            g.members.push_back({i + 1, names[i], std::uint8_t(i == 0 ? game::kGuildMaster : game::kGuildMember), std::uint8_t(70 - i * 6),
                                 (8 - i) * 1'250, i == 0 ? game::kGuildPermAll : i == 1 ? 0x13u : 0u, titles[i], i % 2 ? 14875u : 1907u,
                                 std::uint8_t(i < 5 ? 1 : 0), positions[i], warPoints[i]});
        }
        sampleGuild_.Apply(game::proto::GuildEvent{g});
        game::proto::UnionInfo u;
        u.id = 1;
        u.guilds = {{1, "Dragons", 3, "Gallery", 1907, 8}, {2, "Wolves", 2, "Fenrir", 14875, 17}, {3, "Eagles", 4, "Aquila", 1907, 31}};
        sampleGuild_.Apply(game::proto::GuildEvent{u});
    }
    hudContext_.guild = &sampleGuild_;
    if (sampleCommunity_.Friends().empty()) {
        game::proto::CommunityLists lists;
        lists.friends = {{2, "Beta", 1907, 1}, {3, "Gamma", 14875, 0}, {4, "Delta", 1907, 1}, {5, "Epsilon", 14875, 0}};
        lists.blocked = {"Rude", "Spammer"};
        lists.unread = 1;
        sampleCommunity_.Apply(game::proto::CommunityEvent{lists});
        game::proto::LetterListing mail;
        mail.rows = {{1, "Beta", 0, 2026, 11, 9, 20, 15}, {2, "Gamma", 1, 2026, 11, 8, 9, 5}};
        sampleCommunity_.Apply(game::proto::CommunityEvent{mail});
    }
    hudContext_.community = &sampleCommunity_;
    hudContext_.exchange = &sampleExchange_;
    hudContext_.communityRequest = [](game::proto::MessageId, std::vector<std::byte>) {};
    hudContext_.fortress = &sampleFortress_;
    hudContext_.layout2dt = [this](const std::string& path) { return host_.data().Layout2dt(path); };
    hudContext_.gameGuide = [this] { return host_.data().GameGuide(); };
    hudContext_.world = &sampleWorld_;
    hudContext_.hud = &hud_;
    hudContext_.time = context.time;
    hudContext_.text = [this](const std::string& key) { return host_.data().Text(key); };
    hudContext_.hasText = [this](const std::string& key) { return host_.data().HasText(key); };
    hudContext_.regionKey = [this](std::uint16_t region) { return host_.data().RegionKey(region); };
    hudContext_.slotIcon = SampleIcon;
    hudContext_.effectIcon = [](const game::ActiveEffect& e) { return SampleIcon({game::QuickSlot::Kind::Skill, e.id}); };
    hudContext_.effectName = [](const game::ActiveEffect& e) {
        return e.id == 2 ? std::string("Snow Shield Lv 1") : e.id == 4 ? std::string("Tag Point Lv 1") : std::string("Ice Flower Lv 1");
    };
    if (skills_.size() > 0) {
        hudContext_.skills = &skills_;
        hudContext_.masteryCosts = &masteryCosts_;
    }
    if (!interface_) {
        interface_ = host_.Prepare([this](ui::Interface& made) { RegisterHudWindows(made, hudContext_); });
        if (!interface_) {
            context.holdCapture = true;
            return;
        }
        if (hudMode_) {
            // --sample-dungeon REGION,x,z,FLOOR,ROOM: the sample character in a dungeon,
            // on the .dof's floor and room of those names (the minimap's tiles and names, the world map's floor).
            const std::string inDungeon = context.config.GetString("sample-dungeon", "");
            game::proto::WorldPosition dungeonAt;
            if (!inDungeon.empty()) {
                unsigned region = 0;
                char floor[64] = {}, room[64] = {};
                if (std::sscanf(inDungeon.c_str(), "%x,%f,%f,%63[^,],%63s", &region, &dungeonAt.x, &dungeonAt.z, floor, room) >= 4) {
                    dungeonAt.region = std::uint16_t(region);
                    hudContext_.dungeon = {dungeonAt.region, host_.data().DungeonFolder(dungeonAt.region), floor, room};
                } else {
                    core::LogWarning("--sample-dungeon {}: expected REGION(hex),x,z,FLOOR[,ROOM]", inDungeon);
                }
            }
            SampleCharacter(sample_, sampleWorld_, context.time, dungeonAt.region != 0 ? &dungeonAt : nullptr);
            // --page pk: the sample character a murderer after his first murder (the bar's PK
            // icon and, with --mouse over it, its tooltip).
            if (page_ == "pk") {
                sample_.pkDaily = 5;
                sample_.pkTotal = 1;
                sample_.pkPoints = 1'200;
            }
            hud_.Bind(&sample_);
            hud_.Create(*interface_, {width / scale_, float(context.device.SwapchainHeight()) / scale_});
            if (page_ == "skills" && skills_.size() > 0) {
                // A Chinese character's masteries (Bicheon 30, Pacheon 20) and a few skills learned.
                sample_.self.race = 0;
                sample_.self.level = 40;
                sample_.self.skillPoints = 1234;
                game::proto::SkillBookList book;
                for (std::uint32_t id : {257u, 258u, 259u, 273u, 274u, 275u, 276u}) {
                    book.masteries.push_back({id, std::uint8_t(id == 257 ? 30 : id == 259 ? 20 : 0)});
                }
                for (const char* code : {"SKILL_CH_SWORD_SMASH_A_05", "SKILL_CH_SWORD_SHIELD_A_03", "SKILL_CH_SWORD_SMASH_B_02"}) {
                    if (const assets::SkillRow* s = skills_.Find(std::string_view(code))) book.skills.push_back(s->id);
                }
                sample_.Apply(game::TitleEvent{book});
                hud_.OpenPage(hud::kPageSkills);
            }
            // The guild window on its sample guild, and its Grant name box.
            if (page_ == "guild" || page_ == "guild-title" || page_ == "guild-grant") {
                ShowGuildPage(hud_.Show(hud::kCommunity), 0);
                if (page_ == "guild-title") AskGuildTitle(hudContext_, 2, "Beta");
                // The fortress positions' grant window (--mouse on a row: its help).
                if (page_ == "guild-grant") OpenGuildPosition(hudContext_, 2);
            }
            // The Community side of the same window.
            if (page_ == "community" || page_ == "community-block" || page_ == "community-letter") {
                ShowGuildPage(hud_.Show(hud::kCommunity), page_ == "community" ? 10 : page_ == "community-block" ? 11 : 12);
            }
            if (page_ == "community-write" || page_ == "community-many") ShowCommunityWriter((ShowGuildPage(hud_.Show(hud::kCommunity), 12), hud_.Show(hud::kCommunity)), page_ == "community-many" ? 1 : 0);
            // The master's windows: the transfer with a member checked,
            // the election with three candidates, a ballot cast and its time running.
            // The guild's wars: the hostile guilds, the request window.
            if (page_ == "guild-gp") {
                game::proto::GuildGpHistory history;
                history.rows = {{"Gallery", 54'000, game::kGpReasonGrowth, 2026, 11, 9, 20, 15}, {"Gallery", 20'000, game::kGpReasonGuildItems, 2026, 11, 8, 18, 3}, {"Beta", 1'000, 1, 2026, 11, 2, 9, 41}};
                OpenGuildGpHistory(hudContext_, history);
            }
            if (page_ == "guild-hostile" || page_ == "guild-war-request" || page_ == "guild-combat") {
                game::proto::GuildWars wars;
                game::proto::GuildWarRecord w;
                w.guild = 7;
                w.name = "Ravens";
                w.master = "Corvus";
                w.level = 3;
                w.members = 21;
                w.ours = 240;
                w.theirs = 180;
                w.terms.victory = 1;
                w.terms.wager = 1'000'000;
                w.terms.unlimited = 1;
                wars.wars.push_back(w);
                w.guild = 8;
                w.name = "Vipers";
                w.master = "Naga";
                w.level = 2;
                w.members = 12;
                wars.wars.push_back(w);
                sampleGuild_.Apply(game::proto::GuildEvent{wars});
            }
            if (page_ == "guild-hostile") ShowGuildPage(hud_.Show(hud::kCommunity), 2);
            if (page_ == "guild-war-confirm" || page_ == "guild-war-agree") {
                game::proto::GuildWarTerms terms;
                terms.victory = 2;
                terms.days = 1;
                terms.hours = 6;
                terms.wager = 1'000'000;
                const bool confirm = page_ == "guild-war-confirm";
                AskWarTerms(hudContext_, confirm ? "resinfo\\ifguildwarconfirm.txt" : "resinfo\\ifguildwaragree.txt", confirm ? "UIIT_STT_CONFIRM_BOX" : "UIIT_STT_AGREEMENT_BOX",
                            FormatText(hudContext_.Text(confirm ? "UIIT_MSG_GUILDWAR_SANCTIONWAR_01" : "UIIT_MSG_GUILDWAR_CONSENTWAR_01"), {"Ravens"}),
                            hudContext_.Text(confirm ? "UIIT_MSG_GUILDWAR_SANCTIONWAR_02" : "UIIT_MSG_GUILDWAR_CONSENTWAR_02"), terms, [](bool) {});
            }
            if (page_ == "guild-combat") {
                ShowGuildPage(hud_.Show(hud::kCommunity), 3);
            }
            if (page_ == "guild-war-request") {
                game::proto::GuildWarTerms terms;
                terms.victory = 2;
                terms.days = 1;
                terms.hours = 12;
                terms.minutes = 30;
                terms.wager = 2'500'000;
                FillGuildWarRequest(hudContext_, "Ravens", terms, false);
            }
            // The fortress official's window: Jangan held by a guild, the
            // player's guild applied to take Bijeokdan, the periods of a day.
            if (page_ == "fortress-apply") {
                namespace proto = game::proto;
                proto::FortressList list;
                list.requestsOpen = true;
                list.fortresses = {{1, "SN_FORTRESS_JANGAN", 7, "Wolves", 0, {}, 10, false}, {6, "SN_FORTRESS_BIJEOKDAN", 0, {}, 0, {}, 0, false}};
                sampleFortress_.Apply(proto::FortressEvent{list});
                proto::FortressApplyInfo info;
                info.npc = 1;
                info.rows = {{1, "SN_FORTRESS_JANGAN", "Wolves", 1'000'000, false}, {6, "SN_FORTRESS_BIJEOKDAN", "", 500'000, false}};
                const std::int64_t day = std::int64_t(std::time(nullptr)) / 86400 * 86400;
                info.requestStart = day;
                info.requestEnd = day + 43200;
                info.warStart = day + 48600;
                info.warEnd = day + 55800;
                info.requestsOpen = true;
                info.applied = 6;
                info.side = proto::FortressSide::Attack;
                sampleFortress_.Apply(proto::FortressEvent{info});
                OpenFortressApply(hudContext_);
            }
            if (page_ == "guild-transfer") {
                OpenGuildMasterTransfer(hudContext_);
                PickGuildMasterRow(hudContext_, "Gamma", false);
            }
            if (page_ == "guild-election") {
                game::proto::GuildElection e;
                e.active = 1;
                e.secondsLeft = 437;
                e.candidates = {{2, "Beta", 3}, {3, "Gamma", 1}, {5, "Epsilon", 0}};
                e.ballot = 2;
                e.mayVote = 1;
                sampleGuild_.Apply(game::proto::GuildEvent{e});
                OpenGuildElection(hudContext_);
            }
            // The world map: following the sample character (Jangan), or a map by its id
            // (worldmap:0 the world, worldmap:2007 the tomb's third floor).
            // (worldmap-guide:TAB,HEADING,ENTRY: its guide on a tab (0 the places, 1 the monsters), a heading open, an entry
            // picked.)
            if (page_.rfind("worldmap-guide", 0) == 0) {
                hudContext_.worldMap = host_.data().WorldMap();
                int step[3] = {0, -1, -1};
                if (page_.size() > 15) std::sscanf(page_.c_str() + 15, "%d,%d,%d", &step[0], &step[1], &step[2]);
                hud_.Show(hud::kWorldMap);
                WorldMapGuideStep(hudContext_, step[0], step[1], step[2]);
            }
            // (worldmap-level: the world with the monster box ticked: the monsters 0..6 levels over the sample's marked.)
            if (page_ == "worldmap-level") {
                hudContext_.worldMap = host_.data().WorldMap();
                ShowWorldMap(hudContext_, 0);
                WorldMapMonsterBox(hudContext_, true);
            }
            if (page_ == "worldmap" || page_.rfind("worldmap:", 0) == 0) {
                hudContext_.worldMap = host_.data().WorldMap();
                // Jangan held by a sample guild: its crest box on the world.
                game::proto::FortressList forts;
                forts.fortresses = {{1, "SN_FORTRESS_JANGAN", 7, "Wolves", 0, {}, 10, false, {}, "Wulf", "The wolves keep the gate."}};
                sampleFortress_.Apply(game::proto::FortressEvent{forts});
                if (page_ == "worldmap") {
                    hud_.Show(hud::kWorldMap);
                } else {
                    ShowWorldMap(hudContext_, std::atoi(page_.substr(9).c_str()));
                }
            }
            // The reverse return scroll: its box, or the world map's choosing mode
            // (reverse-map[:N]: marker N's destinations listed).
            if (page_ == "reverse-return") hud_.ReverseReturn([](int result) { core::LogInfo("gallery: the reverse return box answers {}", result); });
            if (page_ == "reverse-map" || page_.rfind("reverse-map:", 0) == 0) {
                hudContext_.worldMap = host_.data().WorldMap();
                ChooseReturnPoint(hudContext_, [](std::uint32_t id) { core::LogInfo("gallery: the map point {} chosen", id); },
                                  page_.size() > 12 ? std::atoi(page_.substr(12).c_str()) : -1);
            }
            // The main window's Inventory page (its side buttons), the skill question, the death window.
            if (page_ == "inventory") hud_.OpenPage(hud::kPageInventory);
            if (page_ == "actions") {
                hudContext_.actions = host_.data().Actions();
                hudContext_.worldMap = host_.data().WorldMap();
                hud_.OpenPage(hud::kPageActions);
            }
            if (page_ == "practice" || page_ == "practice-mastery" || page_ == "practice-short") {
                SkillPracticeBox::Question q;
                q.isMastery = page_ == "practice-mastery";
                q.mastery = "Bicheon";
                q.icon = "icon\\skill\\china\\sword_smash_a.ddj";
                q.name = q.isMastery ? "Bicheon" : "Bicheon Slash";
                q.level = 1;
                q.description = "You will soar high into the sky mastering swords.";
                q.next = "Learn [Bicheon]skill.";
                q.skillPoints = 50;
                hud_.Practice(q, [] {});
                // practice-short: Learn pressed without the points: the original's box kind 9.
                if (auto* box = page_ == "practice-short" ? dynamic_cast<SkillPracticeBox*>(hud_.Show(hud::kPractice)) : nullptr) {
                    sample_.self.skillPoints = 24;
                    box->Answer(true);
                }
            }
            if (page_ == "rebirth") hud_.Rebirth(20, [](int) {});
            if (page_ == "repair-all") AskRepairAll(hudContext_, 0); // the smith's Repair All box (its name strip)
            // the emblem item's boxes: crest-box[:union] the question, crest-done[:union] the success.
            if (page_.starts_with("crest-box") || page_.starts_with("crest-done")) {
                OpenCrestBox(hudContext_, !page_.ends_with(":union"), page_.starts_with("crest-done"), nullptr);
            }
            if (page_ == "rebirth-war") { // an attacker's in a fortress war: its four lines and the 15 s count
                const auto text = [this](const char* key) { return host_.data().Text(key); };
                hud_.Rebirth(20, [](int) {},
                             {text("UIIT_MSG_FORT_REVIVE_TEXT_01"), text("UIIT_MSG_FORT_REVIVE_TEXT_NEW_01"), text("UIIT_MSG_FORT_REVIVE_PENALTY_TEXT_01"),
                              text("UIIT_MSG_FORT_REVIVE_PENALTY_TEXT_02")},
                             context.time + 15.0);
            }
            if (page_.starts_with("academy")) {
                SampleAcademy(sample_);
                hud_.OpenPage(hud::kPageAcademy);
                if (page_ == "academy-judge") hud_.Show(155);
                if (page_ == "academy-ranking") hud_.Show(154);
                if (page_ == "academy-match") hud_.Show(153);
            }
            // The party's windows on a sample party.
            if (page_.rfind("party", 0) == 0 || page_ == "target-menu") {
                SampleParty(sample_);
                namespace proto = game::proto;
                if (page_ == "party") hud_.OpenPage(hud::kPageParty);
                if (page_ == "party-match") hud_.Show(hud::kPartyMatch);
                if (page_ == "party-register") {
                    hud_.Show(hud::kPartyMatch);
                    OpenPartyMatchRegister(*interface_, hudContext_, false);
                }
                if (page_ == "party-mode") OpenSetPartyMode(*interface_, sample_.party.settings, [](std::uint8_t) {});
                if (page_ == "party-request") {
                    sample_.Apply(game::TitleEvent{proto::PartyMatchJoinRequest{3, 105, "Delta", 12, 1}});
                    hud_.Show(131);
                }
                if (page_ == "party-invite") OpenPartyInviteBox(hudContext_, "Ann", 3 | game::kPartyAnyoneInvites, [](bool) {});
                if (page_ == "target-menu") {
                    sample_.target = {};
                    sample_.target.entity = 11;
                    sample_.target.kind = game::TargetStatus::Kind::Player;
                    sample_.target.name = "Bo";
                    sample_.target.race = 1;
                    OpenTargetMenu(*interface_, hudContext_, 11, "Bo", {width / scale_ * 0.5f, 300.0f});
                }
            }
        }
        screenWidth_ = width / scale_;
        for (const std::string& spec : pending_) OpenLayout(spec);
        pending_.clear();
    }
    screenWidth_ = width / scale_;
    // --mouse x,y holds the mouse there (captures of hover states: faces, tooltips). While --click's steps run, their
    // input lasts from frame to frame (a button pressed stays down until released: a drag).
    if (clickIndex_ < clicks_.size()) {
        if (!scripting_) script_ = *context.input;
        scripting_ = true;
        script_.BeginFrame(context.time);
    }
    ui::Input input = scripting_ ? script_ : *context.input;
    if (mouse_.x >= 0.0f) {
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = mouse_;
        input.Feed(move);
    }
    // --click x,y[;x,y...]: each moved there, pressed, released, on three frames in a row once the
    // windows are in, then a few frames for what it did to show; the mouse stays at the last. A drag
    // (d:x0,y0,x1,y1) is pressed at the first point, moved to the second over eight frames, released there.
    if (clickIndex_ < clicks_.size()) {
        const ui::Vec2 at = clicks_[clickIndex_];
        const std::optional<ui::Vec2> to = dragsTo_[clickIndex_];
        if (to && clickStep_ >= 2 && clickStep_ < 10) {
            ui::InputEvent e;
            const float t = float(clickStep_ - 1) / 8.0f;
            e.position = {at.x + (to->x - at.x) * t, at.y + (to->y - at.y) * t};
            e.type = ui::InputEvent::Type::MouseMove;
            input.Feed(e);
            if (++clickStep_ == 10) clickStep_ = 2, dragsTo_[clickIndex_].reset(), clicks_[clickIndex_] = *to;
        } else if (clickStep_ < 3 && (clickStep_ > 0 || (host_.Settled() && (page_ != "fortress-map" || fortressMapShown_) &&
                                                         ((page_ != "inventory-full" && !EquipmentPage(page_)) || autoPotionShown_) &&
                                                         ((page_ != "cos" && page_ != "cos-two" && page_ != "cos-pets") || cosShown_) &&
                                                         ((!page_.starts_with("exchange") && !page_.starts_with("stall") && !page_.starts_with("open-market")) || exchangeFrames_ >= 10)))) {
            ui::InputEvent e;
            e.position = at;
            e.type = clickStep_ == 0 ? ui::InputEvent::Type::MouseMove : clickStep_ == 1 ? ui::InputEvent::Type::MouseDown : ui::InputEvent::Type::MouseUp;
            input.Feed(e);
            ++clickStep_;
        } else if (clickStep_ >= 3 && ++clickStep_ == 6) {
            clickStep_ = 0;
            ++clickIndex_;
            if (clickIndex_ == clicks_.size() && mouse_.x < 0.0f) mouse_ = at;
        }
        context.holdCapture = true;
    }
    if (scripting_) script_ = input;
    // --page ctf-bar: a player's name over his head and the action bar under it a second into a pole's action (the game
    // screen's DrawActionBar over a projected head; here at a fixed point).
    const auto under = [this](ui::Painter& paint) {
        if (page_ != "ctf-bar") return;
        const float x = std::floor(screenWidth_ / 2.0f), y = 300.0f, pitch = ui::TextLineHeight(0);
        const std::string name = sample_.self.name.empty() ? std::string("Altay") : sample_.self.name;
        const float w = paint.TextWidth(name, 0) + 2.0f;
        ui::DrawLabel(paint, name, {std::floor(x - w / 2.0f), std::floor(y - pitch - 2.0f), w, pitch}, ui::Argb(0xFFFFFFFF), 0, 1, 1, ui::kTextShadow);
        DrawActionBar(paint, x, y - 2.0f, 1.0, game::FlagView::kActionSeconds);
    };
    host_.Frame(context, snapshot, input, scale_, under);
    if (hudMode_) {
        if (useSlot_ > 0 && host_.Settled()) {
            hud_.UseQuickSlot(useSlot_, context.time);
            useSlot_ = 0;
        }
        hud_.Keys(host_.context(), context.time);
        for (const HudRequest& r : hud_.TakeRequests()) core::LogInfo("gallery: the HUD asks for {} ({})", int(r.kind), r.value);
        hud_.Sync();
        SamplePortraits(context, snapshot);
        // --page inventory-full: the bag at the 1.188 window's most (three tabs of 32), a potion
        // stack in each tab's first place; --click on a tab shows it.
        if (page_ == "inventory-full" && !autoPotionShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            autoPotionShown_ = true;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                hudContext_.magicOptions = host_.data().MagicOptions(); // the tooltips' blues and the stones' targets
                hudContext_.spirits = host_.data().Spirits();           // the Devil's Spirit's options by plus
                hudContext_.characters = &tables_->characters;          // a ride's or a transport's scroll's animal
                // --in-fortress N: the sample character's region in fortress N's area (the fortress items' place line, the
                // forges' making lines).
                if (const int fortress = context.config.GetInt("in-fortress", 0); fortress > 0) {
                    if (const game::WorldView::Entity* self = sampleWorld_.Find(sampleWorld_.Self())) {
                        game::proto::FortressList forts;
                        game::proto::FortressRecord f;
                        f.id = std::uint32_t(fortress);
                        f.nameKey = "SN_FORTRESS_JANGAN";
                        f.regions = {self->position.region};
                        forts.fortresses = {f};
                        sampleFortress_.Apply(game::proto::FortressEvent{forts});
                        hudContext_.worldMap = host_.data().WorldMap();
                    }
                }
                game::proto::InventoryList bag;
                bag.size = game::Inventory::kMaxSize;
                if (const assets::ItemRow* row = tables_->items.Find(std::string_view("ITEM_ETC_HP_POTION_02")))
                    for (std::uint8_t place : {std::uint8_t(13), std::uint8_t(45), std::uint8_t(77), std::uint8_t(108)})
                        bag.items.push_back({place, row->refObjId, 0, 0, 0, std::uint16_t(place), {}});
                // --bag-items CODE,CODE...: those from the bag's second place on (an item's tooltip with --mouse over it).
                std::uint8_t next = 14;
                const std::string codes = context.config.GetString("bag-items", "");
                for (std::size_t from = 0; from < codes.size();) {
                    const std::size_t comma = std::min(codes.find(',', from), codes.size());
                    if (const assets::ItemRow* row = tables_->items.Find(std::string_view(codes).substr(from, comma - from))) {
                        bag.items.push_back({next++, row->refObjId, 0, 0, game::NewDurability(*row), 1, {}});
                    }
                    from = comma + 1;
                }
                sample_.Apply(game::TitleEvent{bag});
                hud_.OpenPage(hud::kPageInventory);
            }
        }
        // --page equipment | dress | dress-plain: the sample character in a Chinese man's set, every
        // worn place (the accessories too) and a hat and a dress in the bag; dress: the equipment's dress side with a hat, a
        // dress that takes an attachment, its attachment, a flag and a Devil's Spirit; dress-plain: a dress that takes none
        // (the attachment's slot veiled); worn-state: the equipment with some pieces worn down (the head 3, the chest 0, the
        // hands 6, the feet 0, the weapon 2, the shield 0: the paper doll of CEquipStateImg). --mouse on a slot holds its tooltip.
        if (EquipmentPage(page_) && !autoPotionShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            autoPotionShown_ = true;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                const auto record = [&](std::uint8_t place, const char* code, std::uint8_t plus = 0) {
                    game::proto::ItemRecord r;
                    r.slot = place;
                    r.plus = plus;
                    if (const assets::ItemRow* row = tables_->items.Find(std::string_view(code))) {
                        r.item = row->refObjId;
                        r.durability = game::NewDurability(*row);
                    }
                    return r;
                };
                constexpr const char* kWorn[13] = {"ITEM_CH_M_HEAVY_01_HA_A", "ITEM_CH_M_HEAVY_01_BA_A", "ITEM_CH_M_HEAVY_01_SA_A",
                                                   "ITEM_CH_M_HEAVY_01_AA_A", "ITEM_CH_M_HEAVY_01_LA_A", "ITEM_CH_M_HEAVY_01_FA_A",
                                                   "ITEM_CH_SWORD_01_A",      "ITEM_CH_SHIELD_01_A",     "ITEM_CH_M_TRADE_TRADER_02",
                                                   "ITEM_CH_EARRING_01_A",    "ITEM_CH_NECKLACE_01_A",   "ITEM_CH_RING_01_A",
                                                   "ITEM_CH_RING_01_A"};
                game::proto::InventoryList bag;
                bag.size = 45;
                for (std::uint8_t place = 0; place < 13; ++place) bag.items.push_back(record(place, kWorn[place], place == 6 ? 3 : 0));
                if (page_ == "worn-state") {
                    constexpr std::uint32_t kLeft[8] = {3, 0, 200, 6, 200, 0, 2, 0}; // 200: sound
                    for (auto& r : bag.items)
                        if (r.slot < 8 && kLeft[r.slot] < 200) r.durability = kLeft[r.slot];
                }
                bag.items.push_back(record(13, "ITEM_MALL_AVATAR_M_PIRATE_HAT"));
                bag.items.push_back(record(14, page_ == "dress-plain" ? "ITEM_MALL_AVATAR_M_CH_WEDDING" : "ITEM_MALL_AVATAR_M_ANGEL_WING_DRESS"));
                std::erase_if(bag.items, [](const game::proto::ItemRecord& r) { return r.item == 0; });
                sample_.Apply(game::TitleEvent{bag});
                game::proto::AvatarList avatars;
                if (page_ != "equipment" && page_ != "worn-state") {
                    avatars.items.push_back(record(game::avatar::kHat, "ITEM_MALL_AVATAR_M_CH_WEDDING_HAT"));
                    if (page_ == "dress-plain") {
                        avatars.items.push_back(record(game::avatar::kDress, "ITEM_MALL_AVATAR_M_ANGEL_WING_DRESS"));
                    } else {
                        avatars.items.push_back(record(game::avatar::kDress, "ITEM_MALL_AVATAR_M_HALLOWEEN"));
                        avatars.items.push_back(record(game::avatar::kAttachment, "ITEM_MALL_AVATAR_M_HALLOWEEN_ATTACH"));
                    }
                    avatars.items.push_back(record(game::avatar::kFlag, "ITEM_ETC_E050815_KOREAFLAG_M"));
                    avatars.items.push_back(record(game::avatar::kSpirit, "ITEM_MALL_AVATAR_M_NASRUN"));
                    std::erase_if(avatars.items, [](const game::proto::ItemRecord& r) { return r.item == 0; });
                }
                sample_.Apply(game::TitleEvent{avatars});
                hud_.OpenPage(hud::kPageInventory);
                if (auto* equipment = FindUnder<EquipmentWindow>(interface_->Window(hud::kMainPopup)); equipment && page_ != "equipment" && page_ != "worn-state") {
                    equipment->ShowView(true);
                }
            }
        }
        // --page auto-potion: the auto potion window beside the inventory, on
        // a bag of potions and pills, once the item tables are in; its rows filled as a player would
        // (the items dropped in from the bag, the rows ticked).
        if (page_.starts_with("auto-potion") && !autoPotionShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            autoPotionShown_ = true;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                game::proto::InventoryList bag;
                bag.size = 45;
                const std::pair<const char*, std::uint16_t> kept[] = {{"ITEM_ETC_HP_POTION_02", 37}, {"ITEM_ETC_HP_POTION_02", 50},
                                                                      {"ITEM_ETC_MP_POTION_02", 42}, {"ITEM_ETC_CURE_ALL_02", 12},
                                                                      {"ITEM_ETC_HP_POTION_01", 8}, {"ITEM_ETC_ALL_POTION_02", 5},
                                                                      {"ITEM_ETC_COS_HP_POTION_02", 24}};
                std::uint8_t place = 13;
                for (const auto& [code, count] : kept) {
                    if (const assets::ItemRow* row = tables_->items.Find(std::string_view(code))) bag.items.push_back({place, row->refObjId, 0, 0, 0, count, {}});
                    ++place;
                }
                sample_.Apply(game::TitleEvent{bag});
                hud_.OpenPage(hud::kPageInventory);
                if (ui::Widget* w = hud_.Show(hud::kAutoPotion)) {
                    AutoPotionDrop(w, game::AutoPotion::kHp, 13);
                    AutoPotionDrop(w, game::AutoPotion::kMp, 15);
                    AutoPotionDrop(w, game::AutoPotion::kAbnormal, 16);
                    AutoPotionSetRow(w, game::AutoPotion::kHp, true, 70);
                    AutoPotionSetRow(w, game::AutoPotion::kMp, true, 45);
                    AutoPotionSetRow(w, game::AutoPotion::kAbnormal, true, 0);
                    AutoPotionSetDelay(w, true, 1500);
                    // Ours: the vehicle's and the pet's rows, and the tab the page names.
                    AutoPotionDrop(w, game::AutoPotion::kVehicle, 19);
                    AutoPotionDrop(w, game::AutoPotion::kAttackPet, 19);
                    AutoPotionSetRow(w, game::AutoPotion::kVehicle, true, 60);
                    AutoPotionSetRow(w, game::AutoPotion::kAttackPet, false, 40);
                    if (const auto colon = page_.find(':'); colon != std::string::npos) AutoPotionTab(w, std::atoi(page_.c_str() + colon + 1));
                }
            }
        }
        if (page_.starts_with("auto-potion") && !autoPotionShown_) context.holdCapture = true;
        // --page collection-flip: the collection book of a character with the first theme's talismans 1
        // and 2, then the third registered as the server's answer shows it: the book on its theme, the third slot's
        // flip-book playing (with --fixed-dt and --capture-frame, a frame of it).
        if (page_ == "collection-flip" && !autoPotionShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            autoPotionShown_ = true;
            if (tables_->Usable() && !tables_->collection.Themes().empty()) {
                const std::uint32_t theme = tables_->collection.Themes().front().id;
                sampleBook_ = {theme << 8 | 1u, theme << 8 | 2u, theme << 8 | 3u};
                hudContext_.collection = &tables_->collection;
                hudContext_.book = &sampleBook_;
                ShowTalismanRegistered(hudContext_, theme, 3);
            }
        }
        if (page_ == "collection-flip" && !autoPotionShown_) context.holdCapture = true;
        // --page item-flash: the inventory with the repair's sparks on the worn weapon and the bag's first place, the
        // gender switch's swirl on its second (ui::Slot::Flash; with --fixed-dt 0.05 --capture-frame N, a frame of them).
        if (page_ == "item-flash" && !autoPotionShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            autoPotionShown_ = true;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                game::proto::InventoryList bag;
                bag.size = 45;
                const std::pair<const char*, std::uint8_t> kept[] = {
                    {"ITEM_CH_SWORD_01_A", 6}, {"ITEM_CH_M_HEAVY_01_BA_A", 13}, {"ITEM_CH_M_HEAVY_01_LA_A", 14}};
                for (const auto& [code, place] : kept) {
                    if (const assets::ItemRow* row = tables_->items.Find(std::string_view(code))) bag.items.push_back({place, row->refObjId, 0, 0, 20, 1, {}});
                }
                sample_.Apply(game::TitleEvent{bag});
            }
            hud_.OpenPage(hud::kPageInventory);
            FlashItemSlot(hudContext_, 6, ui::Slot::Flash::Repair);
            FlashItemSlot(hudContext_, 13, ui::Slot::Flash::Repair);
            FlashItemSlot(hudContext_, 14, ui::Slot::Flash::Switch);
            flashAt_ = 0;
        }
        // Captured --flash-frames frames into them (with --fixed-dt 0.05 a frame of theirs each).
        if (page_ == "item-flash" && (!autoPotionShown_ || flashAt_++ < context.config.GetInt("flash-frames", 3))) context.holdCapture = true;
        // --page exchange: the exchange window beside the inventory, with Beta: the
        // player's blade, potions and gold offered (its places veiled in the bag), Beta's offer; both still
        // open (--page exchange-confirmed: both confirmed, the blue over each side's items, "Approve").
        const bool exchangePage = page_ == "exchange" || page_ == "exchange-confirmed";
        if (exchangePage && !exchangeShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            exchangeShown_ = true;
            namespace proto = game::proto;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                const auto record = [&](const char* code, std::uint8_t slot, std::uint16_t count, std::uint8_t plus = 0) {
                    const assets::ItemRow* row = tables_->items.Find(std::string_view(code));
                    return proto::ItemRecord{slot, row != nullptr ? row->refObjId : 0u, plus, 0, 40, count, {}};
                };
                proto::InventoryList bag;
                bag.size = 45;
                bag.gold = 1250000;
                bag.items = {record("ITEM_CH_BLADE_02_A_RARE", 13, 1, 5), record("ITEM_ETC_HP_POTION_02", 14, 50), record("ITEM_ETC_MP_POTION_02", 15, 42),
                             record("ITEM_CH_BLADE_01_B", 16, 1)};
                sample_.Apply(game::TitleEvent{bag});
                // A stranger on the open page (the name warning: not a friend, not of the guild or union), Beta (a
                // friend of the sample's) on the confirmed one.
                sampleExchange_.Apply(proto::ExchangeOpened{2, page_ == "exchange" ? "Zeta" : "Beta"});
                proto::ExchangeSide mine;
                mine.mine = 1;
                mine.gold = 25000;
                if (page_ == "exchange-confirmed") mine.step = proto::ExchangeStep::Confirmed;
                mine.items = {record("ITEM_CH_BLADE_02_A_RARE", 0, 1, 5), record("ITEM_ETC_HP_POTION_02", 1, 50)};
                mine.places = {13, 14};
                sampleExchange_.Apply(mine);
                proto::ExchangeSide theirs;
                if (page_ == "exchange-confirmed") theirs.step = proto::ExchangeStep::Confirmed;
                theirs.gold = 120000;
                theirs.items = {record("ITEM_CH_BLADE_01_C_RARE", 0, 1, 3), record("ITEM_ETC_MP_POTION_02", 1, 30), record("ITEM_CH_BLADE_02_B", 2, 1)};
                sampleExchange_.Apply(theirs);
                hud_.OpenPage(hud::kPageInventory);
                hud_.Show(hud::kExchange);
                // What the window would send, logged (an offer dragged back to the bag: ExchangeTake).
                hudContext_.exchangeRequest = [](proto::MessageId id, std::vector<std::byte>) { core::LogInfo("gallery: exchange request 0x{:04X}", id); };
            }
        }
        if (exchangePage && !exchangeShown_) context.holdCapture = true;
        if (exchangeShown_) ++exchangeFrames_; // its windows placed before --click's first
        // --page stall | stall-buy | stall-price: Zeta's stall open for business beside the bag (a visitor's view; stall-buy:
        // its second item dragged to the bag, the agreement box), or the player's own being modified with its first item's
        // price asked (stall-price: its edit button).
        // --page open-market-register: the Open Market's Register page with a potion stack waiting, Register pressed (its
        // confirm box: the deposit, the warning and its check box).
        if (page_ == "open-market-register" && !stallShown_ && interface_ != nullptr && tables_ && tables_->Ready() && hud_.Created()) {
            stallShown_ = true;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                if (const assets::ItemRow* r = tables_->items.Find(std::string_view("ITEM_ETC_HP_POTION_02"))) {
                    sample_.openMarket.pending.push_back({std::uint8_t(13), r->refObjId, 50, 25000});
                    ++sample_.openMarket.version;
                }
                hudContext_.openMarketRequest = [](game::proto::MessageId id, std::vector<std::byte>) { core::LogInfo("gallery: open market request 0x{:04X}", id); };
                ShowOpenMarket(hudContext_, true);
                OpenMarketScript(hudContext_, "om-page", "register");
                OpenMarketScript(hudContext_, "om-register", "");
            }
        }
        // --page open-market-alarm: the consignment notification's side icon and its window on three sample listings (one on
        // sale, one sold, one expired).
        if (page_ == "open-market-alarm" && !stallShown_ && interface_ != nullptr && tables_ && tables_->Ready() && hud_.Created()) {
            stallShown_ = true;
            if (tables_->Usable()) {
                namespace proto = game::proto;
                hudContext_.items = &tables_->items;
                const auto listing = [&](std::uint32_t id, const char* code, game::openmarket::Status status, std::uint32_t seconds) {
                    proto::OpenMarketListing l;
                    l.listing = id;
                    l.status = std::uint8_t(status);
                    if (const assets::ItemRow* r = tables_->items.Find(std::string_view(code))) l.item = {0, r->refObjId, 0, 0, 40, 1, {}};
                    l.price = 450000;
                    l.secondsLeft = seconds;
                    return l;
                };
                proto::OpenMarketListings mine;
                mine.listings = {listing(1, "ITEM_CH_BLADE_02_A_RARE", game::openmarket::Status::OnSale, 2 * 86400 + 3600),
                                 listing(2, "ITEM_ETC_HP_POTION_02", game::openmarket::Status::Sold, 27 * 86400),
                                 listing(3, "ITEM_CH_BOW_01_B", game::openmarket::Status::Expired, 5 * 3600)};
                sample_.openMarket.Apply(proto::OpenMarketEvent{mine}, context.time);
                hudContext_.openMarketRequest = [](proto::MessageId id, std::vector<std::byte>) { core::LogInfo("gallery: open market request 0x{:04X}", id); };
                ShowMarketAlarmGuide(hudContext_, true);
                ToggleMarketAlarm(hudContext_);
            }
        }
        // --page open-market-buy: the Open Market's Search page with one result, chosen and bought (its purchase box).
        if (page_ == "open-market-buy" && !stallShown_ && interface_ != nullptr && tables_ && tables_->Ready() && hud_.Created()) {
            stallShown_ = true;
            if (tables_->Usable()) {
                namespace proto = game::proto;
                hudContext_.items = &tables_->items;
                proto::OpenMarketResults results;
                results.pages = 1;
                results.total = 1;
                proto::OpenMarketRow row;
                row.account = 7;
                row.listing = 1;
                row.seller = "Zeta";
                row.price = 450000;
                row.secondsLeft = 86400 * 3;
                if (const assets::ItemRow* r = tables_->items.Find(std::string_view("ITEM_CH_BLADE_02_A_RARE"))) row.item = {0, r->refObjId, 5, 0, 40, 1, {}};
                results.rows = {row};
                sample_.openMarket.Apply(proto::OpenMarketEvent{results}, context.time);
                hudContext_.openMarketRequest = [](proto::MessageId id, std::vector<std::byte>) { core::LogInfo("gallery: open market request 0x{:04X}", id); };
                ShowOpenMarket(hudContext_, true);
                OpenMarketScript(hudContext_, "om-page", "search");
                OpenMarketScript(hudContext_, "om-buy", "0");
            }
        }
        // --page stall-network-buy: the stall network's window on a page of one row, its Purchase pressed (the box).
        if (page_ == "stall-network-buy" && !stallShown_ && interface_ != nullptr && tables_ && tables_->Ready() && hud_.Created()) {
            stallShown_ = true;
            namespace proto = game::proto;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                hudContext_.stall = &sampleStall_;
                proto::NetworkPage page;
                page.pages = 1;
                page.total = 1;
                proto::NetworkRow row;
                row.listing = 7ull << 8 | 1;
                row.owner = "Zeta";
                row.price = 12500;
                if (const assets::ItemRow* r = tables_->items.Find(std::string_view("ITEM_ETC_HP_POTION_02"))) row.item = {0, r->refObjId, 0, 0, 0, 50, {}};
                page.rows = {row};
                sampleStall_.NewSearch(0, 0);
                sampleStall_.Apply(proto::StallEvent{page}, 1);
                hudContext_.stallRequest = [](proto::MessageId id, std::vector<std::byte>) { core::LogInfo("gallery: stall request 0x{:04X}", id); };
                hud_.Show(hud::kStallNetwork);
                if (ui::Interface* ui = hud_.interface()) StallNetworkBuy(*ui, 0);
            }
        }
        const bool stallPage = page_ == "stall" || page_ == "stall-buy" || page_ == "stall-price";
        if (stallPage && !stallShown_ && interface_ != nullptr && tables_ && tables_->Ready() && hud_.Created()) {
            stallShown_ = true;
            namespace proto = game::proto;
            if (tables_->Usable()) {
                hudContext_.items = &tables_->items;
                hudContext_.stall = &sampleStall_;
                const auto record = [&](const char* code, std::uint8_t slot, std::uint16_t count, std::uint8_t plus = 0) {
                    const assets::ItemRow* row = tables_->items.Find(std::string_view(code));
                    return proto::ItemRecord{slot, row != nullptr ? row->refObjId : 0u, plus, 0, 40, count, {}};
                };
                const bool mine = page_ == "stall-price";
                proto::StallView view;
                view.owner = mine ? 1 : 7;
                view.ownerName = mine ? "Gallery" : "Zeta";
                view.name = mine ? "Gallery's goods" : "Blades and potions";
                view.open = mine ? 0 : 1;
                view.items = {{0, record("ITEM_CH_BLADE_02_A_RARE", 0, 1, 5), 450000}, {1, record("ITEM_ETC_HP_POTION_02", 0, 50), 12500},
                              {3, record("ITEM_CH_BLADE_01_B", 0, 1), 8000}};
                view.people = {{9, "Beta"}};
                sampleStall_.Apply(proto::StallEvent{view}, 1);
                hudContext_.stallRequest = [](proto::MessageId id, std::vector<std::byte>) { core::LogInfo("gallery: stall request 0x{:04X}", id); };
                hud_.OpenPage(hud::kPageInventory);
                hud_.Show(hud::kStall);
                if (page_ == "stall-buy") StallBuyBox(hudContext_, 1);
                if (page_ == "stall-price") StallRegister(hudContext_, 0, 0);
            }
        }
        if (stallPage && !stallShown_) context.holdCapture = true;
        if (stallShown_) ++exchangeFrames_; // as the exchange's: its windows placed before --click's first
        // --page magic-pop[:STATE] (HudMagicPop.cpp): the machine's window on a bag of two cards and a
        // coupon. STATE: select (the selection window, the default), choose (the strip of the first prize's kind, nothing
        // chosen: state 0), card (a prize chosen: 1), ready (a card registered: 2), rolling (PLAY pressed), won / lost (the
        // answer arrived; capture after the 4.1 s roll, e.g. --fixed-dt 0.05 --capture-frame 120).
        if (page_.starts_with("magic-pop") && !popShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            popShown_ = true;
            namespace proto = game::proto;
            std::string why;
            if (tables_->Usable() && gacha_.Load(context.files, why) && !gacha_.entries().empty()) {
                hudContext_.items = &tables_->items;
                hudContext_.gacha = &gacha_;
                const std::string state = page_.size() > 10 ? page_.substr(10) : "select";
                const auto record = [&](const char* code, std::uint8_t slot, std::uint16_t count) {
                    const assets::ItemRow* row = tables_->items.Find(std::string_view(code));
                    return proto::ItemRecord{slot, row != nullptr ? row->refObjId : 0u, 0, 0, 0, count, {}};
                };
                proto::InventoryList bag;
                bag.size = 45;
                bag.gold = 1250000;
                // The coupon holds the first prize (a sword of the strip: params [0] the item, [1] the count).
                const assets::GachaEntry& first = gacha_.entries().front();
                proto::ItemRecord coupon = record("ITEM_MALL_GACHA_CARD_WIN", 15, 1);
                coupon.magic = {first.item, 1};
                bag.items = {record("ITEM_MALL_GACHA_CARD", 13, 1), record("ITEM_MALL_GACHA_CARD", 14, 1), coupon, record("ITEM_ETC_HP_POTION_02", 16, 50)};
                sample_.Apply(game::TitleEvent{bag});
                OpenMagicPop(hudContext_, 0);
                if (state != "select") MagicPopChoose(hudContext_, first.id, state != "choose");
                if (state == "ready" || state == "rolling" || state == "won" || state == "lost") MagicPopCard(hudContext_, 13);
                if (state == "rolling" || state == "won" || state == "lost") {
                    MagicPopPlay(hudContext_);
                    if (state != "rolling") {
                        proto::PopPlayed played;
                        played.won = state == "won" ? 1 : 0;
                        played.prize = first.item;
                        played.count = 1;
                        MagicPopPlayed(hudContext_, played);
                    }
                }
            } else {
                core::LogWarning("gallery: Magic POP: {}", why);
            }
        }
        // --page mall[:STEP;STEP...] (HudMall.cpp): the item mall on a sample character with 9,870 silk, 120
        // gift silk and 35 points; each STEP a script step of PlayMall.cpp without its "mall-" (shop:MALL_AVATAR, tab:1, popup:CODE,
        // buy:CODE,COUNT, wear:CODE, buy-worn, reserve:CODE, shop:BASKET, basket-buy, history, ...). --page mall-enter: the
        // "Enter Item Mall" box.
        if (page_.starts_with("mall") && !mallShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            mallShown_ = true;
            std::string why;
            if (tables_->Usable() && mallTable_.Load(context.files, why)) {
                hudContext_.items = &tables_->items;
                mall_.catalog = &mallTable_;
                hudContext_.mall = &mall_;
                sample_.Apply(game::TitleEvent{game::proto::MallEvent{game::proto::SilkBalanceMessage{9870, 120, 35}}});
                // Each purchase logged and answered "bought" (the next frame): a lump buy's one-at-a-time sends show.
                hudContext_.mallRequest = [this](game::proto::MessageId id, std::vector<std::byte>) {
                    core::LogInfo("gallery: mall request 0x{:04X}", id);
                    if (id == game::proto::MallBuy::kId) ++mallAnswers_;
                };
                if (page_ == "mall-enter") {
                    AskItemMall(hudContext_);
                } else {
                    OpenItemMall(hudContext_);
                    const std::string steps = page_.size() > 5 ? page_.substr(5) : std::string();
                    for (std::size_t at = 0; at < steps.size();) {
                        const std::size_t end = std::min(steps.find(';', at), steps.size());
                        const std::string step = steps.substr(at, end - at);
                        if (!step.empty() && !ItemMallStep(hudContext_, "mall-" + step)) core::LogWarning("gallery: mall step {} not done", step);
                        at = end + 1;
                    }
                    // The history's answer (an inquiry asked): the starting silk and two charges of the last month.
                    if (steps.find("inquire") != std::string::npos) {
                        game::proto::SilkHistory h;
                        h.list = 1;
                        h.rows = {{10000, game::proto::PackSilkDate(2026, 11, 14, 9, 30)}, {500, game::proto::PackSilkDate(2026, 11, 2, 21, 5)},
                                  {1200, game::proto::PackSilkDate(2026, 10, 28, 13, 41)}};
                        ItemMallAnswer(hudContext_, game::proto::MallEvent{h});
                    }
                }
            } else {
                core::LogWarning("gallery: the item mall: {}", why);
            }
        }
        if (page_.starts_with("mall") && !mallShown_) context.holdCapture = true;
        if (mallAnswers_ > 0) {
            --mallAnswers_;
            ItemMallAnswer(hudContext_, game::proto::MallEvent{game::proto::MallBought{}});
        }
        // The won and lost pages wait for the roll's end (its result shown).
        const bool popResult = page_ == "magic-pop:won" || page_ == "magic-pop:lost";
        if (page_.starts_with("magic-pop") && (!popShown_ || (popResult && MagicPopRolling(hudContext_)))) context.holdCapture = true;
        // --page cos | cos-two | cos-pets: the COS manager (HudCos.cpp) on a ridden horse (its status
        // icon, the command bar: Info, Dismount, Terminate); cos-two adds a transport left standing beside it (two icons,
        // one outlined); cos-pets an attack pet (picked: its taller frame with the HGP gauge, its six commands) and a grab pet.
        const bool cosPage = page_ == "cos" || page_ == "cos-two" || page_ == "cos-pets";
        if (cosPage && !cosShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            cosShown_ = true;
            hudContext_.characters = tables_->Usable() ? &tables_->characters : nullptr;
            hudContext_.skills = tables_->Usable() ? &tables_->skills : nullptr;
            const auto add = [&](const char* codename, std::uint32_t entity, std::uint8_t kind, std::uint32_t hp, bool ridden, std::string name = {},
                                 std::uint16_t hgp = 0) -> HudContext::CosShown* {
                const assets::ObjectRow* row = hudContext_.characters != nullptr ? hudContext_.characters->Find(std::string_view(codename)) : nullptr;
                if (row == nullptr) return nullptr;
                hudContext_.coses.push_back({entity, row->refObjId, kind, hp, row->maxHp, ridden, std::move(name), std::uint8_t(row->level), hgp});
                return &hudContext_.coses.back();
            };
            add("COS_C_HORSE1", 901, 1, 700, page_ != "cos-pets");
            if (page_ == "cos-two") add("COS_T_CAMEL1", 902, 2, 5000, false);
            if (page_ == "cos-pets") {
                // The wolf at level 6 (the sample's level table ends at 8), its EXP a third of the way; the rabbit's
                // rental 27 days and some hours left.
                if (HudContext::CosShown* wolf = add("COS_P_WOLF_006", 903, 3, 700, false, "Fang", 6200)) wolf->experience = 965;
                if (HudContext::CosShown* rabbit = add("COS_P_RABBIT", 904, 4, 109, false)) {
                    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    rabbit->rentUntil = std::int64_t(now) + 27 * 86400 + 5 * 3600 + 12 * 60;
                }
                hudContext_.cosSelected = 903;
            }
            // Info opens the COS window on the picked one (the game screen's PlayScreen::CosCommand does the same).
            hudContext_.cosCommand = [this](int command, std::uint32_t entity) {
                if (command != kCosInfo) return;
                hudContext_.cosSelected = entity;
                hud_.Toggle(hud::kPetWindow);
            };
            hud_.Show(hud::kPets);
        }
        if (cosPage && !cosShown_) context.holdCapture = true;
        // --page ctf-timer | ctf-card | ctf-camp | ctf-desk[:2] | ctf-talk | ctf-swap | ctf-bar: Capture the Flag's pieces
        // (55 section 4) on a sample match, the player on team 0.
        if (page_.starts_with("ctf-") && interface_ != nullptr && tables_ && tables_->Ready()) {
            namespace proto = game::proto;
            hudContext_.flag = &sampleFlag_;
            hudContext_.npcGreeting = [this](const std::string& codename) { return host_.data().NpcGreeting(codename); };
            hudContext_.npcTalk = [this](const std::string& codename) { return host_.data().NpcTalk(codename); };
            constexpr std::uint32_t kSoOk = 950;
            if (!ctfShown_) {
                ctfShown_ = true;
                sampleFlag_.Apply(proto::EntityFlagTeam{sample_.self.entity, 0});
                if (page_ == "ctf-card") ShowSubscribedBox(hudContext_);
                if (page_ == "ctf-camp") {
                    hud_.Rebirth(sample_.self.level, [](int) {}, {}, 0.0, "UIIT_MSG_FLAGWAR_GO_TO_REVIVAL");
                    OpenCampCountdown(hudContext_);
                }
                if (page_.starts_with("ctf-desk") || page_ == "ctf-talk" || page_ == "ctf-swap") {
                    proto::EventDesk desk;
                    desk.npc = kSoOk;
                    desk.flagState = page_ == "ctf-desk:2" ? 2 : 1;
                    desk.quests.push_back({game::SwapIceTrophies().id, std::string(game::SwapIceTrophies().title)});
                    sampleFlag_.Apply(desk);
                    const assets::ObjectRow* row = tables_->Usable() ? tables_->characters.Find(game::flag::kSoOk) : nullptr;
                    if (auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc))) {
                        npc->ShowNpc(kSoOk, std::string(game::flag::kSoOk), row != nullptr ? row->nameKey : std::string("SN_NPC_CH_EVENT_KISAENG1"), proto::kNpcTalk);
                        if (page_ == "ctf-talk") npc->Choose(1);
                        if (page_ == "ctf-swap") {
                            npc->ShowEventSays({kSoOk, game::SwapIceTrophies().id, std::string(game::kSwapTalkA),
                                                {std::string(game::kSwapTalkB), std::string(game::kTalkExit)}});
                        }
                    }
                }
                if (page_ == "ctf-timer") sampleFlag_.Apply(proto::FlagWorldNotice{0, std::uint8_t(game::FlagState::MatchWorld), game::flag::kWorld, 0, {}}, sample_.self.entity, context.time);
            }
            if (page_ == "ctf-timer") {
                // Held at 19:47 (13 s played; FF 0E's resynchronisation each frame).
                sampleFlag_.Apply(proto::FlagWorldNotice{1, std::uint8_t(game::FlagCode::Clock), 13'000, 0, {}}, sample_.self.entity, context.time);
                hud_.Show(hud::kTimer);
            }
        }
        if (page_.starts_with("ctf-") && !ctfShown_) context.holdCapture = true;
        // --page temple-reputation: the temples' reputation board on sample rows: the thieves ahead at
        // Anubis's (they alone enter), Haroeris's even (nobody); its icon in the side icons' row beside the letters' envelope.
        if (page_ == "temple-reputation" && !templeShown_ && interface_ != nullptr && tables_ && tables_->Ready() && host_.data().Ready()) {
            templeShown_ = true;
            namespace proto = game::proto;
            const std::int64_t now = std::int64_t(std::time(nullptr));
            std::vector<proto::TempleReputationRow> rows(2);
            rows[0] = {proto::TempleNotice::kThief, now + 3 * 3600, now - 86400, 1200, 3400};
            rows[1] = {proto::TempleNotice::kNobody, now + 5 * 3600, now - 2 * 86400, 500, 500};
            ShowTempleReputation(hudContext_, rows);
            hud_.Show(hud::kLetterAlarm);
            ShowTempleGuide(hudContext_, true);
        }
        // --page quest-guide[:window]: the level-up's quest guide: its side icon on sample rows (and its window).
        if (page_.starts_with("quest-guide") && !templeShown_ && interface_ != nullptr && tables_ && tables_->Ready() && host_.data().Ready()) {
            templeShown_ = true;
            ShowQuestGuide(hudContext_, {{1, "SN_QNO_CH_SMITH_1", "SN_NN_QNO_CH_SMITH_1", "SN_NC_QNO_CH_SMITH_1"},
                                         {2, "SN_QNO_CH_POTION_1", "SN_NN_QNO_CH_POTION_1", "SN_NC_QNO_CH_POTION_1"}});
            if (page_ == "quest-guide:window") hud_.Show(45);
        }
        // --page composite-item: the premium package's uses window (its right click on the buff board) on sample uses.
        if (page_ == "composite-item" && !compositeShown_ && interface_ != nullptr && hud_.Created() && tables_ && tables_->Ready() && tables_->Usable()) {
            compositeShown_ = true;
            hudContext_.items = &tables_->items;
            sample_.timedServices.premium.clear();
            for (const auto& [code, left, perDay] : {std::tuple{"ITEM_MALL_RETURN_SCROLL_HIGH_SPEED", 3, 5}, std::tuple{"ITEM_MALL_REVERSE_RETURN_SCROLL", 0, 3},
                                                     std::tuple{"ITEM_MALL_RESURRECTION_100P_SCROLL", 2, 2}}) {
                if (const assets::ItemRow* row = tables_->items.Find(std::string_view(code))) {
                    sample_.timedServices.premium.push_back({row->refObjId, std::uint8_t(left), std::uint8_t(perDay)});
                }
            }
            ShowCompositeItems(hudContext_);
        }
        // --page game-guide: the Help key's guide (its start page and its menu; --click picks a heading and a topic).
        if (page_ == "game-guide" && !guideShown_ && interface_ != nullptr && host_.data().Ready()) {
            guideShown_ = true;
            hud_.Show(hud::kGameGuide);
        }
        // --page quest-reward[:confirm]: the reward window at its finisher on a beta quest's definition (one weapon to
        // choose), and with its first item picked and "Reward" pressed, the confirm (res_ui\corfim.2dt).
        if (page_.starts_with("quest-reward") && !rewardShown_ && interface_ != nullptr && tables_ && tables_->Ready() && host_.data().Ready() &&
            host_.data().Layout2dt("res_ui\\corfim.2dt") != nullptr) {
            rewardShown_ = true;
            const std::string text = game::BetaQuestDefinitions();
            const std::size_t next = text.find("\nquest ");
            sampleQuests_.Apply(game::proto::QuestDefinition{text.substr(0, next == std::string::npos ? std::string::npos : next + 1)});
            hudContext_.quests = &sampleQuests_;
            if (tables_->Usable()) hudContext_.items = &tables_->items;
            const game::QuestDef* q = sampleQuests_.Defs().empty() ? nullptr : &sampleQuests_.Defs().begin()->second;
            core::LogInfo("gallery: quest-reward: {} definitions, {} reward items, text '{}'", sampleQuests_.Defs().size(),
                          q != nullptr ? q->reward.items.size() : 0, q != nullptr ? q->rewardText.literal : std::string());
            if (q != nullptr) {
                OpenQuestReward(hudContext_, q->id, 1);
                if (page_ == "quest-reward:confirm") OpenQuestRewardConfirm(hudContext_, q->id, 1, {0});
            }
        }
        // --page alchemy[:TAB]: the alchemy window on a tab (0 Disjoint, 1 Dismantle, 2 Manufacture, 3 Strengthen; --mouse on a
        // slot or the button: its tip).
        if (page_.starts_with("alchemy") && !alchemyShown_ && interface_ != nullptr && tables_ && tables_->Ready() && host_.data().Ready() &&
            hud_.Created()) {
            alchemyShown_ = true;
            if (tables_->Usable()) hudContext_.items = &tables_->items;
            hudContext_.magicOptions = host_.data().MagicOptions();
            ToggleAlchemyWindow(hudContext_);
            if (page_.size() > 8) AlchemyShowTab(hudContext_, std::atoi(page_.c_str() + 8));
        }
        // --page avatar-grant: the smith's avatar blessing (window 156) with an avatar dress from the bag put in and its second
        // option picked (the bag's avatar: --bag-items' first, else the first dress of the data that takes magic options).
        if (page_ == "avatar-grant" && !alchemyShown_ && interface_ != nullptr && tables_ && tables_->Usable() && host_.data().Ready() &&
            hud_.Created()) {
            alchemyShown_ = true;
            hudContext_.items = &tables_->items;
            hudContext_.magicOptions = host_.data().MagicOptions();
            hudContext_.magicOptionAssign = host_.data().MagicOptionAssign();
            std::string code = context.config.GetString("bag-items", "");
            if (code.empty()) {
                for (const auto& [name, row] : tables_->items.rows()) {
                    if (row.typeId == std::array<int, 4>{3, 1, 13, 2} && row.maxMagicOptions > 0 && (code.empty() || name < code)) code = name;
                }
            }
            if (const assets::ItemRow* row = tables_->items.Find(std::string_view(code))) {
                game::proto::InventoryList bag;
                bag.size = game::Inventory::kMaxSize;
                bag.items.push_back({14, row->refObjId, 0, 0, game::NewDurability(*row), 1, {}});
                sample_.Apply(game::TitleEvent{bag});
                ShowAvatarGrant(hudContext_, 0);
                AvatarGrantStep(hudContext_, 14, 1, false);
            }
        }
        // --page guild-keeper: a keeper of guilds' rows on a shard with mercenaries (the master's "Grant additional ability to
        // mercenary"); --page guild-soldier[:BITS]: its soldier page, the guild holding the abilities BITS (game::kMerc*).
        if ((page_ == "guild-keeper" || page_.starts_with("guild-soldier")) && !soldierShown_ && tables_ && tables_->Ready() && host_.data().Ready() &&
            hud_.Created()) {
            soldierShown_ = true;
            namespace proto = game::proto;
            proto::GuildInfo g = sampleGuild_.Guild();
            g.mercenaries = true;
            g.mercenaryAttr = page_.size() > 14 ? std::uint8_t(std::atoi(page_.c_str() + 14)) : 0;
            sampleGuild_.Apply(proto::GuildEvent{g});
            const std::string npcCode = "NPC_CH_GENARAL_SP";
            const assets::ObjectRow* row = tables_->Usable() ? tables_->characters.Find(npcCode) : nullptr;
            if (auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc))) {
                npc->ShowNpc(952, npcCode, row != nullptr ? row->nameKey : std::string("SN_NPC_CH_GENARAL_SP"), proto::kNpcGuild);
                if (page_ != "guild-keeper") npc->ShowSoldiers();
            }
            core::LogInfo("gallery: {}: abilities {:#04x}", page_, int(g.mercenaryAttr));
        }
        // --page casting:KIND: the casting gauge of that kind (proto::CastingKind 0..4) on the sample character, 40 % run: a
        // return scroll, a quest's search (QNO_CA_WAREHOUSE_1's), the guild recall, a fortress flag placed, a repair kit.
        if (page_.starts_with("casting") && !castingShown_ && tables_ && tables_->Ready() && host_.data().Ready() && hud_.Created()) {
            castingShown_ = true;
            namespace proto = game::proto;
            const int kind = page_.size() > 8 ? std::atoi(page_.c_str() + 8) : 0;
            const auto item = [&](std::string_view code) {
                const assets::ItemRow* row = tables_->items.Find(code);
                return row != nullptr ? row->refObjId : 0u;
            };
            game::PlayerState::Casting c;
            c.kind = proto::CastingKind(std::clamp(kind, 0, 4));
            c.seconds = 10.0;
            c.started = context.time - 4.0;
            if (c.kind == proto::CastingKind::Return) c.item = item("ITEM_ETC_SCROLL_RETURN_01");
            if (c.kind == proto::CastingKind::PlacedItem) c.item = item("ITEM_FORT_DEFENSIVE_FLAG");
            if (c.kind == proto::CastingKind::SkillItem) c.item = item("ITEM_FORT_REPAIR_KIT_01");
            if (c.kind == proto::CastingKind::QuestSearch) {
                sampleQuests_.Apply(proto::QuestDefinition{"quest QNO_CA_WAREHOUSE_1\nid 90010\nlevel 30\nname \"\"\nstart level 1-\n"});
                hudContext_.quests = &sampleQuests_;
                c.quest = 90010;
            }
            hudContext_.items = &tables_->items; // the gauge's line names the item
            sample_.casting = c;
            core::LogInfo("gallery: casting: kind {} (item {})", int(c.kind), c.item);
        }
        // --page quest-list: the quest window (L) on two quests under way: each quest's row and its goals' rows under it
        // (one goal met: "Completed" in its colour).
        if (page_ == "quest-list" && !questListShown_ && interface_ != nullptr && tables_ && tables_->Ready() && host_.data().Ready() &&
            hud_.Created()) {
            questListShown_ = true;
            namespace proto = game::proto;
            sampleQuests_.Apply(proto::QuestDefinition{"quest QNO_GALLERY_HUNT\nid 90002\nlevel 5\nname \"Wolves of Jangan\"\nstart level 1-\n"
                                                       "giver NPC_CH_SMITH\nfinish NPC_CH_SMITH\nrepeat 1\n"
                                                       "goal kill 10 MOB_CH_WOLF text \"Hunt 10 wolves (%d)\"\n"
                                                       "goal kill 5 MOB_CH_WOLF_L text \"Hunt 5 wolf leaders (%d)\"\n"});
            sampleQuests_.Apply(proto::QuestDefinition{"quest QNO_GALLERY_FETCH\nid 90003\nlevel 12\nname \"The smith's ore\"\nstart level 1-\n"
                                                       "giver NPC_CH_SMITH\nfinish NPC_CH_SMITH\n"
                                                       "goal kill 20 MOB_CH_TIGER text \"Hunt 20 tigers (%d)\"\n"});
            proto::QuestList list;
            list.active = {{90002, proto::QuestStatus::Achieving, {10, 2}, 0}, {90003, proto::QuestStatus::Achieving, {7}, 0}};
            sampleQuests_.Apply(list);
            hudContext_.quests = &sampleQuests_;
            core::LogInfo("gallery: quest-list: {} definitions, {} under way", sampleQuests_.Defs().size(), sampleQuests_.Active().size());
            ToggleQuestWindow(hudContext_);
        }
        // --page quest-talk[:N]: a finisher's talk of several pages with the player's rows under them (the White Viper's
        // return to Leebaek: _09 with the row _10, _11 with _12, _13 with "Receive reward."); N rows picked first.
        if (page_.starts_with("quest-talk") && !questTalkShown_ && interface_ != nullptr && tables_ && tables_->Ready() && host_.data().Ready() &&
            hud_.Created()) {
            questTalkShown_ = true;
            namespace proto = game::proto;
            const std::string npcCode = "NPC_CH_GENARAL_SP";
            sampleQuests_.Apply(proto::QuestDefinition{
                "quest QNO_GALLERY_TALK\nid 90001\nlevel 88\nname @SN_QNO_TQ_BAEKRYOUNG_10\nstart level 88-\ngiver " + npcCode + "\nfinish " + npcCode +
                "\ntalk " + npcCode + " done @SN_TALK_QNO_TQ_BAEKRYEONG_10_09 row @SN_TALK_QNO_TQ_BAEKRYEONG_10_10\ntalk " + npcCode +
                " done @SN_TALK_QNO_TQ_BAEKRYEONG_10_11 row @SN_TALK_QNO_TQ_BAEKRYEONG_10_12\ntalk " + npcCode + " done @SN_TALK_QNO_TQ_BAEKRYEONG_10_13\n"});
            hudContext_.quests = &sampleQuests_;
            core::LogInfo("gallery: quest-talk: {} definitions", sampleQuests_.Defs().size());
            constexpr std::uint32_t kLeebaek = 952;
            const assets::ObjectRow* row = tables_->Usable() ? tables_->characters.Find(npcCode) : nullptr;
            if (auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc))) {
                npc->ShowNpc(kLeebaek, npcCode, row != nullptr ? row->nameKey : std::string("SN_NPC_CH_GENARAL_SP"), proto::kNpcTalk);
                proto::QuestSays says;
                says.npc = kLeebaek;
                says.quest = 90001;
                says.moment = "done";
                says.choice = proto::QuestChoice::HandIn;
                npc->ShowQuestSays(says);
                const int picks = page_.size() > 11 ? std::atoi(page_.c_str() + 11) : 0;
                for (int k = 0; k < picks; ++k) npc->Choose(1);
            }
        }
        // --page arena-rule | arena-guild | arena-score | arena-flag | arena-result | arena-desk[:cancel]: the Battle Arena's
        // windows on a sample match: the rule window with both kinds (score selected), the
        // guild window on the sample guild (two chosen), the score and rank windows on a sample board with the
        // timer, the score window alone in a flag map, a won result, the manager's NPC window with the random mode's row
        // (or the cancel row).
        if (page_.starts_with("arena-") && interface_ != nullptr && tables_ && tables_->Ready()) {
            namespace proto = game::proto;
            hudContext_.arena = &sampleArena_;
            hudContext_.npcGreeting = [this](const std::string& codename) { return host_.data().NpcGreeting(codename); };
            hudContext_.npcTalk = [this](const std::string& codename) { return host_.data().NpcTalk(codename); };
            constexpr std::uint32_t kManager = 951;
            if (!arenaShown_) {
                arenaShown_ = true;
                const auto broadcast = [&](std::uint8_t code, std::uint8_t kind, std::uint32_t value) {
                    proto::ArenaNotice n;
                    n.code = code;
                    n.kind = kind;
                    n.value = value;
                    sampleArena_.Apply(n, context.time);
                };
                if (page_ == "arena-rule") {
                    OpenArenaRule(hudContext_, 0, game::arena::kScoreBit | game::arena::kFlagBit);
                    ArenaPickKind(hudContext_, game::arena::kScore);
                }
                if (page_ == "arena-guild") {
                    OpenArenaGuild(hudContext_, game::arena::kScoreBit | game::arena::kFlagBit);
                    const auto& members = sampleGuild_.Guild().members;
                    std::vector<std::string> names;
                    for (std::size_t i = 1; i < members.size() && names.size() < 2; ++i) names.push_back(members[i].name);
                    ArenaGuildChoose(hudContext_, names, false);
                }
                if (page_ == "arena-score" || page_ == "arena-result") {
                    broadcast(4, game::arena::kScore, 0);
                    broadcast(8, game::arena::kScore, 600'000);
                    sampleArena_.Apply(proto::ArenaScores{128, 47, {{0, "Walker", 66}, {0, sample_.self.name.empty() ? "Altay" : sample_.self.name, 24},
                                                                 {1, "Fenrir", 21}, {0, "Beta", 9}, {1, "Aquila", 6}}});
                }
                if (page_ == "arena-flag") {
                    broadcast(4, game::arena::kFlag, 0);
                    broadcast(8, game::arena::kFlag, 600'000);
                    sampleArena_.Apply(proto::ArenaScores{1003, 2, {}});
                }
                if (page_ == "arena-result") sampleArena_.Apply(proto::ArenaResult{0, 1, 5, 186'760, 128, 47});
                if (page_.starts_with("arena-desk")) {
                    sampleArena_.Apply(proto::ArenaDesk{kManager, std::uint16_t(page_ == "arena-desk:cancel" ? game::arena::kRegistered : 0x60)});
                    const std::string codename(game::arena::kManager);
                    const assets::ObjectRow* row = tables_->Usable() ? tables_->characters.Find(codename) : nullptr;
                    if (auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc))) {
                        npc->ShowNpc(kManager, codename, row != nullptr ? row->nameKey : std::string("SN_NPC_BATTLE_ARENA_MANAGER"), proto::kNpcTalk);
                        if (page_ == "arena-desk:talk") npc->Choose(2); // the mode's row: its talk
                    }
                }
            }
            if (page_ == "arena-score") {
                // Held at 07:47 (2:13 played; the clock's resynchronisation each frame).
                proto::ArenaNotice clock;
                clock.personal = 1;
                clock.code = std::uint8_t(game::ArenaCode::Clock);
                clock.kind = game::arena::kScore;
                clock.value = 600'000;
                clock.value2 = 133'000;
                sampleArena_.Apply(clock, context.time);
                hud_.Show(hud::kTimer);
            }
            ArenaWindows(hudContext_);
        }
        if (page_.starts_with("arena-") && !arenaShown_) context.holdCapture = true;
        // --page fortress-map: the battle aide's structure map on Jangan's places of
        // objectstring.ifo (the heart, its towers and gates; one tower damaged, one gate destroyed), once the tables are in.
        if (page_ == "fortress-map" && !fortressMapShown_ && interface_ != nullptr && tables_ && tables_->Ready()) {
            fortressMapShown_ = true;
            namespace proto = game::proto;
            hudContext_.characters = tables_->Usable() ? &tables_->characters : nullptr;
            hudContext_.worldMap = host_.data().WorldMap();
            proto::FortressStructureList list;
            list.npc = 1;
            list.fortress = 1;
            std::uint32_t zone = 84;
            for (const assets::NamedSpot& s : spots_) {
                if (s.name.find("STRUCTURE_POS_JA_") == std::string::npos) continue;
                proto::FortressStructureRecord r;
                const bool heart = s.name.find("FORT_STONE") != std::string::npos, tower = s.name.find("GUARD_TOWER") != std::string::npos;
                const bool gate = s.name.find("GATE") != std::string::npos;
                if (!heart && !tower && !gate) continue;
                r.zone = zone++;
                r.region = s.region;
                r.x = s.x;
                r.z = s.z;
                r.refObjId = heart ? 19538 : tower ? 19521 : 19504;
                r.kind = std::uint8_t(heart ? game::StructureKind::Heart : tower ? game::StructureKind::GuardTower : game::StructureKind::Gate);
                r.maxHp = 5000;
                r.hp = r.zone == 86 ? 3100 : r.zone == 89 ? 0 : 5000;
                r.state = r.hp == 0 ? 1 : 0;
                r.repairCost = r.maxHp - r.hp;
                list.structures.push_back(r);
            }
            sampleFortress_.Apply(proto::FortressEvent{list});
            OpenFortressMap(hudContext_);
        }
        if (page_ == "fortress-map" && !fortressMapShown_) context.holdCapture = true;
        // The fortress manager's and the forges' windows (HudFortressWork.cpp), Jangan held by
        // the sample guild (the Dragons, the player its Commander): fortress-business[-none] (the jobs, an attacker applied or
        // none), fortress-intro[-member] (About Guild, the Commander's or a member's), fortress-make[:trainer|running|done|member]
        // (the smith's or the trainer's window, no work, one running, one done, as a member without the position),
        // fortress-make-box / fortress-make-cancel (the Make and Cancel boxes), fortress-hire / fortress-cave-enter (the
        // manager's questions).
        if (page_.starts_with("fortress-") && page_ != "fortress-map" && page_ != "fortress-apply" && !fortressWorkShown_ && interface_ != nullptr && tables_ &&
            tables_->Ready()) {
            fortressWorkShown_ = true;
            namespace proto = game::proto;
            hudContext_.items = tables_->Usable() ? &tables_->items : nullptr;
            hudContext_.worldMap = host_.data().WorldMap();
            proto::FortressList list;
            list.fortresses = {{1, "SN_FORTRESS_JANGAN", 1, "Dragons", 0, {}, 5, false, {}, "Gallery", "Dragons keep Jangan."}};
            sampleFortress_.Apply(proto::FortressEvent{list});
            if (page_.ends_with("-member")) sampleGuild_.SetSelf("Gamma"); // a member without a position
            if (page_.starts_with("fortress-business")) {
                proto::FortressSchedule s;
                s.fortress = 1;
                s.last = {11, 7, 6, 20, 0};
                s.next = {11, 14, 6, 20, 0};
                s.applied = page_ == "fortress-business";
                OpenFortressBusiness(hudContext_, s);
            } else if (page_.starts_with("fortress-intro")) {
                OpenGuildIntroduction(hudContext_, proto::FortressIntroduction{1, "Dragons keep Jangan. Apply at the clerk before Saturday."}, 1);
            } else if (page_.starts_with("fortress-make")) {
                proto::FortressForge w;
                w.npc = 1;
                w.side = page_ == "fortress-make:trainer" ? 1 : 0;
                w.fortress = 1;
                const std::int64_t now = std::int64_t(std::time(nullptr));
                if (page_ == "fortress-make:running" || page_ == "fortress-make-cancel") {
                    w.has = true;
                    w.item = 10593; // three repair kits: 9 minutes, 4 of them left
                    w.count = 3;
                    w.finishes = now + 4 * 60 + 20;
                } else if (page_ == "fortress-make:done") {
                    w.has = true;
                    w.item = 10593;
                    w.count = 3;
                    w.done = true;
                    w.finishes = now - 1;
                }
                OpenFortressMakeItem(hudContext_, w, true);
                if (page_ == "fortress-make-box") PressFortressMakeItem(hudContext_, "ask-make", 10593, 3);
                if (page_ == "fortress-make-cancel") PressFortressMakeItem(hudContext_, "ask-cancel", 0, 0);
            } else if (page_ == "fortress-hire") {
                hud_.Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(hudContext_.Text("UIIT_MSG_FORT_MANAGER_EMPLOY_WINDOW"), {hudContext_.Text("SN_FORTRESS_MANAGER_NPC_NAME_SMITH"), "30000", "3000"})},
                             [](bool) {});
            } else if (page_ == "fortress-cave-enter") {
                hud_.Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(hudContext_.Text("UIIT_STT_GUILD_DUNGEON_CREATE_GP_GOLD_CONSUME"), {"600000", "3000"})}, [](bool) {});
            }
        }
        if (page_.starts_with("fortress-") && page_ != "fortress-map" && page_ != "fortress-apply" && !fortressWorkShown_) context.holdCapture = true;
    }
    if (context.toolsVisible) DevTools();
    // A capture waits for the windows' art (and a few frames for it to show).
    if (!host_.Settled()) context.holdCapture = true;
}

void GalleryScreen::DevTools() {
    ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({320, 520}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Interface gallery")) {
        ImGui::SliderFloat("scale", &scale_, 0.5f, 3.0f, "%.2f");
        if (ImGui::Button("Close all")) {
            interface_->CloseAll();
            opened_ = 0;
            flow_ = {12.0f, 12.0f};
            rowHeight_ = 0.0f;
        }
        ImGui::InputText("filter", filter_, sizeof filter_);
        const std::string filter = Lower(filter_);
        if (ImGui::BeginChild("layouts")) {
            for (const std::string& path : host_.layouts()) {
                if (!filter.empty() && path.find(filter) == std::string::npos) continue;
                if (ImGui::Selectable(path.c_str() + 8)) OpenLayout(path.substr(8));
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace sr::client
