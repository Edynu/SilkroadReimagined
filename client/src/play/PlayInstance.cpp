// The Forgotten World on the game screen: the Gap of Dimensions a party's hole
// opened (its model from teleportbuilding, its name board's lines as the 1.188 client's, the party
// master for the party's own gap, the levels, the grade), walked up to and entered after a question; the
// instance's exits; a talisman's registration asked first (UIIT_MSG_COLLECTION_WINDOW_RECORD_CONFIRM, as
// 1.188's collection book does); the triggers' notices on screen and in the chat; the refusals.

#include <format>

#include "audio/GameAudio.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr float kGapTalk = 90.0f;   // from how near the gap and an exit are used (the server's reach 100)
constexpr float kGapWalkTo = 50.0f; // a click walks this near first

} // namespace

const assets::TeleportGate* PlayScreen::GapGate(const game::WorldView::Entity& e) const {
    if (!tables_ || !tables_->Usable() || e.kind != proto::EntityKind::Character || !e.look.name.empty()) return nullptr;
    if (tables_->characters.Find(e.look.model) != nullptr) return nullptr;
    return tables_->teleports.Gate(e.look.model);
}

bool PlayScreen::IsExit(const game::WorldView::Entity& e) const {
    if (!IsNpc(e)) return false;
    const assets::ObjectRow* row = tables_->characters.Find(e.look.model);
    return row != nullptr && row->codename == "NPC_INS_EXIT_TELEPORT";
}

bool PlayScreen::IsPillar(const game::WorldView::Entity& e) const {
    if (!IsNpc(e)) return false;
    const assets::ObjectRow* row = tables_->characters.Find(e.look.model);
    return row != nullptr && row->codename == "NPC_FWORLD_RECALL_PARTY";
}

bool PlayScreen::UsedNear(std::uint32_t entity) const {
    const game::WorldView::Entity* e = view_.Find(entity);
    return e != nullptr && (GapGate(*e) != nullptr || IsExit(*e) || IsPillar(*e));
}

float PlayScreen::UseReach(std::uint32_t entity, float otherwise) const { return UsedNear(entity) ? kGapTalk : otherwise; }
float PlayScreen::UseWalkTo(std::uint32_t entity, float otherwise) const { return UsedNear(entity) ? kGapWalkTo : otherwise; }

// The gaps in sight: each one's facts asked once (its name board's lines).
void PlayScreen::AskGaps() {
    if (replay_ || !shared_ || !shared_->session) return;
    for (const auto& [id, e] : view_.Entities()) {
        if (GapGate(e) == nullptr || gapsAsked_.contains(id)) continue;
        gapsAsked_.insert(id);
        shared_->session->SendGame(proto::SelectGap::kId, proto::Encode(proto::SelectGap{id}));
    }
    std::erase_if(gapsAsked_, [&](std::uint32_t id) { return view_.Find(id) == nullptr; });
    std::erase_if(gaps_, [&](const auto& kv) { return view_.Find(kv.first) == nullptr; });
}

// The lines under a gap's name: "Party master: %s" for the player's party's own, "Level: %s"
// with the hole's levels, "Grade : %s" in stars.
std::vector<std::string> PlayScreen::GapLines(std::uint32_t entity) {
    std::vector<std::string> out;
    const auto it = gaps_.find(entity);
    if (it == gaps_.end()) return out;
    const UiData& data = gui_.data();
    const proto::GapInfo& g = it->second;
    const auto format = [&](const char* key, const std::string& value) {
        std::string pattern = data.Text(key);
        const std::size_t at = pattern.find("%s");
        return at == std::string::npos ? pattern + " " + value : pattern.replace(at, 2, value);
    };
    if (g.temple) {
        // A job temple's door: Seth's says its temple's players, the others nothing more.
        if (g.capacity > 0) {
            std::string line = data.Text("UIIT_STT_EGYPT_SHRINE_SETH_ENTRANCE_NUMBER");
            for (const int v : {int(g.inside), int(g.capacity)}) {
                if (const std::size_t at = line.find("%d"); at != std::string::npos) line.replace(at, 2, std::to_string(v));
            }
            out.push_back(line);
        }
        return out;
    }
    if (g.own) out.push_back(format("UIIT_STT_GOD_PARTY_MASTER", g.master));
    out.push_back(format("UIIT_STT_GOD_LV_EXTENT", std::format("{} - {}", g.minLevel, g.maxLevel)));
    if (g.grade >= 1 && g.grade <= 4) out.push_back(format("UIIT_STT_GOD_CLASS", data.Text("UIIT_STT_GOD_CLASS_" + std::to_string(g.grade))));
    return out;
}

// Standing by a gap or an exit: what it does, asked first.
void PlayScreen::UseAt(std::uint32_t entity) {
    const game::WorldView::Entity* e = view_.Find(entity);
    if (e == nullptr || replay_ || !shared_ || !shared_->session) return;
    const UiData& data = gui_.data();
    if (GapGate(*e) != nullptr) {
        // Ours: the question is the text table's UIIT_MSG_GOD_GATE_MOVE (1.188's own code does not use it; its gap
        // explains itself in its hover tooltip).
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", {data.Text("UIIT_MSG_GOD_GATE_MOVE")}, [this, entity](bool yes) {
            if (yes) shared_->session->SendGame(proto::EnterGap::kId, proto::Encode(proto::EnterGap{entity}));
        });
    } else if (IsExit(*e)) {
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", {data.Text("SN_INS_QUEST_TELEPORT_BS")}, [this, entity](bool yes) {
            if (yes) shared_->session->SendGame(proto::LeaveInstance::kId, proto::Encode(proto::LeaveInstance{entity}));
        });
    } else if (IsPillar(*e)) {
        // Its NPC window (the server tells its one service), "Summon Party member" asking for the list.
        shared_->session->TalkToNpc(entity);
    }
}

bool PlayScreen::UseTalisman(std::uint8_t place) {
    if (!tables_ || !tables_->Usable()) return false;
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
    if (row == nullptr || row->typeId != std::array<int, 4>{3, 3, 12, 5}) return false;
    if (replay_ || !shared_ || !shared_->session) return true;
    // the collection book's own box (its picture, its name, the question).
    hudContext_.collection = &tables_->collection;
    OpenCollectionBox(hudContext_, place, *row);
    return true;
}

void PlayScreen::HandleInstance(ScreenContext& context, const proto::InstanceEvent& event) {
    const UiData& data = gui_.data();
    const auto text = [&](const std::string& key) { return data.HasText(key) ? data.Text(key) : key; };
    const auto post = [&](const std::string& line) { player_.Post(game::GameMessage::Kind::General, line, context.time); };
    if (const auto* g = std::get_if<proto::GapInfo>(&event)) {
        gaps_[g->entity] = *g;
        if (g->temple && g->capacity > 0) core::LogInfo("game: temple door {} (world {}): {} / {} inside", g->entity, g->world, g->inside, g->capacity);
    } else if (const auto* r = std::get_if<proto::InstanceRefused>(&event)) {
        post(text(r->text));
        core::LogInfo("game: forgotten world: refused ({})", r->text);
    } else if (const auto* n = std::get_if<proto::InstanceNotice>(&event)) {
        // Its %s (the tomb's difficulty: a text key, or words).
        std::string said = text(n->text);
        if (const std::size_t at = said.find("%s"); at != std::string::npos && !n->arg.empty()) said.replace(at, 2, text(n->arg));
        if (n->notify) player_.Notify(game::Banner::Kind::Notice, said, context.time);
        if (n->warning) player_.Notify(game::Banner::Kind::Warning, said, context.time);
        if (n->system) post(said);
        if (n->quest) {
            player_.Notify(game::Banner::Kind::Quest, text(n->text), context.time);
            if (n->text == "SN_TALK_COMMON_END" && context.audio != nullptr) context.audio->PlayUi("SND_QUEST_END");
        }
        core::LogInfo("game: forgotten world: {}", n->text);
    } else if (const auto* s = std::get_if<proto::InstanceState>(&event)) {
        instance_ = *s;
        // The re-entry limit's timed job on the buff board, told again after each WorldEntered cleared it.
        constexpr std::uint32_t kReentryJob = 0x46475752; // ours: the board's key for it
        auto& effects = player_.effects;
        std::erase_if(effects, [](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::TimedJob && e.id == kReentryJob; });
        if (s->jobSeconds > 0) {
            game::ActiveEffect job;
            job.kind = game::ActiveEffect::Kind::TimedJob;
            job.id = kReentryJob;
            job.started = context.time;
            job.duration = double(s->jobSeconds);
            job.icon = s->jobIcon;
            job.subject = s->jobSubject;
            job.text = s->jobText;
            effects.push_back(std::move(job));
        }
        core::LogInfo("game: forgotten world: {}", s->world != 0 ? std::format("in world {} ({} s left)", s->world, s->secondsLeft) : "out");
    } else if (const auto* b = std::get_if<proto::CollectionBook>(&event)) {
        book_.clear();
        for (const auto& entry : b->entries) book_.insert(entry.theme << 8 | entry.slot);
    } else if (const auto* t = std::get_if<proto::TalismanRegistered>(&event)) {
        if (t->ok) {
            book_.insert(t->theme << 8 | t->slot);
            ShowTalismanRegistered(hudContext_, t->theme, t->slot); // the book shown, the slot's flip-book
        }
        if (!t->text.empty()) post(text(t->text));
        core::LogInfo("game: talisman {}: theme {} slot {}", t->ok ? "registered" : "refused", t->theme, t->slot);
    } else if (const auto* list = std::get_if<proto::SummonList>(&event)) {
        summonList_ = *list;
        ++hudContext_.summonRevision;
        OpenSummonWindow(hudContext_);
        core::LogInfo("game: summon list: {} members", list->rows.size());
    } else if (const auto* asked = std::get_if<proto::SummonAsked>(&event)) {
        // "%s is summoning you. Do you wish to accept the summon?" (20 s; the server lets it lapse).
        std::string line = data.Text("UIIT_STT_GNGWC_PARTY_MEMBER_CALL");
        if (const std::size_t at = line.find("%s"); at != std::string::npos) line.replace(at, 2, asked->master);
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", {line}, [this](bool yes) {
            if (shared_ && shared_->session) shared_->session->SendGame(proto::SummonReply::kId, proto::Encode(proto::SummonReply{yes}));
        });
        core::LogInfo("game: summoned by {}", asked->master);
    } else if (const auto* roc = std::get_if<proto::RocNotice>(&event)) {
        // The Roc's countdowns (0x300C 0x0C14 / 0x0C15): the line, then in the chat and on the
        // warning banner.
        std::string line;
        if (roc->kind == proto::RocNotice::kBerserk && roc->minutes == proto::RocNotice::kStarted) {
            line = text("UIIT_MSG_QUEST_ROC_BERSERKER_START");
        } else if (roc->kind == proto::RocNotice::kBerserk && roc->minutes == proto::RocNotice::kAdvanced) {
            line = text("UIIT_MSG_QUEST_ROC_BERSERKER_AGAIN");
        } else {
            line = text(roc->kind == proto::RocNotice::kBerserk ? "UIIT_MSG_QUEST_ROC_BERSERKER" : "UIIT_MSG_QUEST_ROC_BYEBYE");
            if (const std::size_t at = line.find("%d"); at != std::string::npos) line.replace(at, 2, std::to_string(roc->minutes));
        }
        post(line);
        player_.Notify(game::Banner::Kind::Warning, line, context.time);
        core::LogInfo("game: roc: {}", line);
    } else if (const auto* alert = std::get_if<proto::InstanceAlert>(&event)) {
        // The original's 0x300C alarm (1.188's Msg_300C: a byte, the minutes left): "After %d minute, instance dungeon will
        // vanish. When it vanish, user will move to saved return place.", in the chat and on the warning banner.
        std::string line = text("UIIT_MSG_EGYPT_DUNGEON_DESTROY_ALARM");
        if (const std::size_t at = line.find("%d"); at != std::string::npos) line.replace(at, 2, std::to_string((alert->seconds + 30) / 60));
        post(line);
        player_.Notify(game::Banner::Kind::Warning, line, context.time);
    } else if (const auto* temple = std::get_if<proto::TempleNotice>(&event)) {
        // The job temples' news (0x300C 0x0C17 / 0x0C18): the temple named by its dungeon's zone, the
        // alliance by its guild's name (the traders' and the hunters' together), a line and the warning banner.
        const std::string name = text(data.RegionKey(temple->region));
        const auto fill = [](std::string line, const std::vector<std::string>& args) {
            for (const std::string& a : args) {
                const std::size_t s = line.find("%s"), d = line.find("%d");
                const std::size_t at = std::min(s, d);
                if (at == std::string::npos) break;
                line.replace(at, 2, a);
            }
            return line;
        };
        std::string line;
        if (temple->kind == proto::TempleNotice::kBefore) {
            line = fill(text("UIIT_MSG_EGYPT_SHRINE_ENTRANCE_ALARM"), {name, std::to_string(temple->minutes)});
        } else if (temple->kind == proto::TempleNotice::kOpened) {
            const std::string who = temple->allowed == proto::TempleNotice::kTraderHunter
                                        ? text("UIIT_PAG_COMPANY_MERCHANT") + ", " + text("UIIT_PAG_COMPANY_HENTER")
                                        : temple->allowed == proto::TempleNotice::kThief ? text("UIIT_PAG_COMPANY_THIEF")
                                                                                         : text("UIIT_STT_EGYPT_SHRINE_ENTRANCE_UNION_ALL");
            if (temple->allowed == proto::TempleNotice::kNobody) {
                line = fill(text("UIIT_MSG_EGYPT_SHRINE_ENTRANCE_UNION_ALARM_04"), {who, name});
            } else if (temple->allowed == proto::TempleNotice::kEveryone) {
                line = fill(text("UIIT_MSG_EGYPT_SHRINE_ENTRANCE_UNION_ALARM_03"), {name});
            } else {
                line = fill(text("UIIT_MSG_EGYPT_SHRINE_ENTRANCE_UNION_ALARM_01"), {who, name});
            }
        }
        if (!line.empty()) {
            post(line);
            player_.Notify(game::Banner::Kind::Warning, line, context.time);
        }
        core::LogInfo("game: temple {} ({:#06x}): {}", int(temple->kind), temple->region, line);
    } else if (const auto* board = std::get_if<proto::TempleReputation>(&event)) {
        // "Temple area Point"'s answer (0xB4DF): the board, or for code 1 alone (the other alliance's NPC)
        // 0x1C26 "Only members are able to use this."; the other codes say nothing.
        if (board->ok) ShowTempleReputation(hudContext_, board->rows);
        else if (board->code == proto::TempleReputation::kOtherAlliance) post(text("UIIT_MSG_EGYPT_MENU_ERROR_UNIONMEMBER"));
    } else if (const auto* points = std::get_if<proto::TemplePoints>(&event)) {
        UpdateTemplePoints(hudContext_, *points); // 0xB4E0: the open board's numbers
    } else if (const auto* icon = std::get_if<proto::TempleIcon>(&event)) {
        ShowTempleGuide(hudContext_, icon->show); // 0x34E1: the side icon 0xA5
    } else if (const auto* scene = std::get_if<proto::QuestScene>(&event)) {
        PlayQuestScene(context, scene->name);
    } else if (const auto* result = std::get_if<proto::SummonResult>(&event)) {
        std::string line = text(result->text);
        if (const std::size_t at = line.find("%s"); at != std::string::npos) line.replace(at, 2, result->name);
        post(line);
        core::LogInfo("game: summon: {} ({})", result->text, result->name);
    }
}

// The script's steps (--guild / --fortress): gap (the nearest gap walked to and its question shown), exit (the
// nearest exit's), talisman:PLACE (the talisman in that place used: its question).
bool PlayScreen::InstanceScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg) {
    if (verb != "gap" && verb != "exit" && verb != "pillar" && verb != "talisman" && verb != "summon" && verb != "collection" &&
        verb != "collection-theme" && verb != "collection-story" && verb != "talisman-yes" && verb != "pillar-summon" && verb != "talismans") {
        return false;
    }
    hudContext_.collection = tables_ && tables_->Usable() ? &tables_->collection : nullptr;
    if (verb == "talisman") {
        if (!UseTalisman(std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10)))) core::LogWarning("game: talisman: no talisman in place {}", arg);
        return true;
    }
    if (verb == "talisman-yes") {
        if (!AnswerCollectionBox(hudContext_, true)) core::LogWarning("game: talisman-yes: no box");
        return true;
    }
    if (verb == "summon") {
        if (!SummonByName(hudContext_, arg)) core::LogWarning("game: summon: no row of {}", arg);
        return true;
    }
    if (verb == "collection") {
        ToggleCollectionBook(hudContext_);
        return true;
    }
    if (verb == "collection-theme") {
        if (!PickCollectionTheme(hudContext_, std::atoi(arg.c_str()))) core::LogWarning("game: collection-theme: no row {}", arg);
        return true;
    }
    if (verb == "talismans") {
        // Every talisman in the bag registered (the quests' tests: a theme completed), as their boxes confirmed.
        int sent = 0;
        for (const auto& [place, item] : player_.inventory.places) {
            const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (row == nullptr || row->typeId != std::array<int, 4>{3, 3, 12, 5} || replay_ || !shared_ || !shared_->session) continue;
            shared_->session->SendGame(proto::RegisterTalisman::kId, proto::Encode(proto::RegisterTalisman{place}));
            ++sent;
        }
        core::LogInfo("game: talismans: {} registered", sent);
        return true;
    }
    if (verb == "pillar-summon") {
        // The pillar's NPC window's "Summon Party member" row picked.
        auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc));
        if (npc == nullptr || !npc->ChooseText(gui_.data().Text("UIIT_PAG_PARTY_SUMMON"))) core::LogWarning("game: pillar-summon: no such row");
        return true;
    }
    if (verb == "collection-story") {
        if (!OpenCollectionStory(hudContext_, std::atoi(arg.c_str()))) core::LogWarning("game: collection-story: slot {} not registered", arg);
        return true;
    }
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    std::uint32_t best = 0;
    double bestDistance = 1e18;
    for (const auto& [id, e] : view_.Entities()) {
        if (!(verb == "gap" ? GapGate(e) != nullptr : verb == "pillar" ? IsPillar(e) : IsExit(e)) || self == nullptr) continue;
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(e.position);
        const double d = std::hypot(a.x - b.x, a.z - b.z);
        if (d < bestDistance) {
            best = id;
            bestDistance = d;
        }
    }
    if (best == 0) {
        core::LogWarning("game: {}: none in sight", verb);
        return true;
    }
    // As a click on it.
    pendingNpc_ = best;
    pendingUntil_ = context.time + 10.0;
    pendingGate_ = 0;
    if (bestDistance > kGapTalk) {
        const game::WorldView::Entity* it = view_.Find(best);
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(it->position);
        const double length = std::max(1.0, bestDistance);
        shared_->session->MoveTo(proto::FromFlat({b.x + (a.x - b.x) / length * kGapWalkTo, b.z + (a.z - b.z) / length * kGapWalkTo}, it->position.y));
    }
    return true;
}

} // namespace sr::client
