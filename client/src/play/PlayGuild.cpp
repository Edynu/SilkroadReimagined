// Guilds and unions on the game screen: the session's guild events into
// game::GuildView, their system lines (the 1.188 client's texts) and the proposals' yes/no boxes,
// the guild line over members' heads, and the test options that drive guild requests.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near
#undef far
#endif

#include "play/PlayScreen.h"
#include "sr/game/BetaQuests.h"
#include "sr/game/Cos.h"
#include "sr/game/JobProtocol.h"
#include "sr/core/Log.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

// An emblem as a 16 x 16 DDS of 32-bit pixels (A8R8G8B8), for the asset store to load like any
// texture.
std::vector<std::byte> CrestDds(const std::vector<std::uint8_t>& pixels) {
    constexpr std::uint32_t kSide = game::kCrestSide;
    std::vector<std::byte> out(128 + kSide * kSide * 4);
    const auto put = [&](std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
    std::memcpy(out.data(), "DDS ", 4);
    put(4, 124);                  // the header's size
    put(8, 0x1 | 0x2 | 0x4 | 0x1000); // caps, height, width, pixel format
    put(12, kSide);
    put(16, kSide);
    put(76, 32);                  // the pixel format's size
    put(80, 0x40 | 0x1);          // RGB with alpha
    put(88, 32);
    put(92, 0x00FF0000u);
    put(96, 0x0000FF00u);
    put(100, 0x000000FFu);
    put(104, 0xFF000000u);
    put(108, 0x1000);             // a texture
    for (std::size_t i = 0; i < std::size_t(kSide * kSide); ++i) {
        std::byte* p = out.data() + 128 + i * 4;
        p[0] = std::byte(pixels[i * 3 + 0]);
        p[1] = std::byte(pixels[i * 3 + 1]);
        p[2] = std::byte(pixels[i * 3 + 2]);
        p[3] = std::byte(0xFF);
    }
    return out;
}

} // namespace

void PlayScreen::GuildOptions(ScreenContext& context) {
    // --guild "step;step;..." (create:NAME, walk-npc, invite:NAME, target:NAME, notice:TITLE|BODY,
    // kick:NAME, leave, disband, master:NAME, permit:NAME=BITS, donate:N, levelup, chat:TEXT,
    // uchat:TEXT, union-invite:NAME, union-leave, union-kick:GUILD, window[:union], wait), one each
    // --guild-step seconds from --guild-after seconds in the world.
    // Back in the world after --logout-after's Restart (--reselect): --guild-again's steps instead (none by default).
    const bool again = shared_ && shared_->restarted;
    for (std::string steps = context.config.GetString(again ? "guild-again" : "guild", ""); !steps.empty();) {
        const std::size_t semi = steps.find(';');
        guildScript_.push_back(steps.substr(0, semi));
        steps = semi == std::string::npos ? std::string() : steps.substr(semi + 1);
    }
    guildAfter_ = context.config.GetDouble("guild-after", 3.0);
    guildStep_ = context.config.GetDouble("guild-step", 2.0);
    guildAnswer_ = context.config.GetBool("guild-accept", false) ? 1 : context.config.GetBool("guild-decline", false) ? -1 : 0;
    // --crest-dir DIR: guildmark.bmp / unionmark.bmp looked for there first, then in the game's
    // folder (the original's "install path"), then in the current one.
    crestDir_ = context.config.GetString("crest-dir", "");
    gameDir_ = context.config.GetString("game", "");
}

// A Global chatting item (TID 3/3/3/5) opens "Chat to all"; a line sent there uses one up.
bool PlayScreen::UseGlobalChatItem(std::uint8_t place) {
    if (!tables_ || !tables_->Usable()) return false;
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
    if (row == nullptr || row->typeId != std::array<int, 4>{3, 3, 3, 5}) return false;
    if (auto* window = dynamic_cast<GlobalChatWindow*>(hud_.Show(hud::kGlobalChat))) window->Open(place, item->refId);
    return true;
}

bool PlayScreen::UseCrestItem(std::uint8_t place) {
    if (!tables_ || !tables_->Usable()) return false;
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
    const std::optional<game::CrestItem> crest = row != nullptr ? game::CrestItemOf(row->codename) : std::nullopt;
    if (!crest) return false;
    if (replay_ || !shared_ || !shared_->session) return true;
    const auto text = [this](const std::string& key) { return gui_.data().Text(key); };
    const bool ofGuild = crest->kind == proto::kCrestGuild;
    const std::uint8_t kind = crest->kind;
    if (crest->remove) {
        // "Delete guild emblem?"
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", {text(ofGuild ? "UIIT_MSG_GUILD_CREST_DELETE" : "UIIT_MSG_UNION_CREST_DELETE")}, [this, kind, place](bool yes) {
            if (yes) shared_->session->SendGuild(proto::GuildCrestDelete{kind, place});
        });
        return true;
    }
    // The question (the original's box 0x15 / 0x18: the format, where the file goes, then "Register the ... emblem?").
    OpenCrestBox(hudContext_, ofGuild, false, [this, kind, place](bool yes) {
        if (yes) RegisterCrest(kind, place);
    });
    return true;
}

void PlayScreen::RegisterCrest(std::uint8_t kind, std::uint8_t place) {
    const bool ofGuild = kind == proto::kCrestGuild;
    const auto post = [this](const char* key) {
        player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), hudContext_.time);
        core::LogInfo("game: emblem: {}", key);
    };
    // an emblem registered already is refused before the file is read.
    if ((ofGuild ? guild_.Guild().crest : guild_.Union().crest) != 0) return post("UIIT_MSG_CREST_REGISTER_OVERLAP");
    // the file (none: "There is no file", the wrong kind: "does not follow the format").
    const char* name = ofGuild ? "guildmark.bmp" : "unionmark.bmp";
    std::vector<std::filesystem::path> folders;
    for (const std::string& dir : {crestDir_, gameDir_}) {
        if (!dir.empty()) folders.emplace_back(dir);
    }
    folders.push_back(std::filesystem::current_path());
#ifdef _WIN32
    // Ours: beside the client's own program too (where a player puts it when the game's folder is not ours).
    {
        wchar_t self[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, self, MAX_PATH) != 0) folders.push_back(std::filesystem::path(self).parent_path());
    }
#endif
    std::vector<std::uint8_t> file;
    bool found = false;
    for (const std::filesystem::path& dir : folders) {
        std::ifstream in(dir / name, std::ios::binary);
        if (!in) continue;
        found = true;
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        core::LogInfo("game: emblem: {} ({} bytes)", (dir / name).string(), file.size());
        break;
    }
    if (!found) return post("UIIT_MSG_CREST_REGISTER_FAIL");
    std::vector<std::uint8_t> pixels;
    if (game::ReadCrestBitmap(file, pixels) != game::CrestFile::Ok) return post("UIIT_MSG_CREST_REGISTER_ERROR");
    shared_->session->SendGuild(proto::GuildCrestRegister{kind, place, std::move(pixels)});
}

std::string PlayScreen::CrestTexture(std::uint8_t kind, std::uint32_t id, std::uint32_t revision) {
    const std::vector<std::uint8_t>* pixels = revision != 0 ? guild_.Crest(kind, id, revision) : nullptr;
    if (pixels == nullptr || madeFiles_ == nullptr) return {};
    std::string path = std::format("crest\\{}{}_{}.dds", kind == proto::kCrestGuild ? 'g' : 'u', id, revision);
    if (crestFiles_.insert(path).second) madeFiles_->Put(path, CrestDds(*pixels));
    return path;
}

std::uint32_t PlayScreen::NearestNpc(bool guildOnly) const {
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr) return 0;
    const proto::FlatPosition me = proto::ToFlat(self->position);
    std::uint32_t best = 0;
    double nearest = 1e18;
    for (const auto& [id, e] : view_.Entities()) {
        if (id == view_.Self() || e.kind != proto::EntityKind::Character || !IsNpc(e)) continue;
        if (guildOnly) {
            const assets::ObjectRow* row = tables_ && tables_->Usable() ? tables_->characters.Find(e.look.model) : nullptr;
            if (row == nullptr || !game::IsGuildNpcCodename(row->codename)) continue;
        }
        const proto::FlatPosition p = proto::ToFlat(e.position);
        const double d = std::hypot(p.x - me.x, p.z - me.z);
        if (d < nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

void PlayScreen::HandleGuild(ScreenContext& context, const proto::GuildEvent& event) {
    const auto text = [this](const std::string& key) { return gui_.data().Text(key); };
    if (const auto* e = std::get_if<proto::EntityGuild>(&event)) {
        core::LogInfo("game: guild: {} is in {} '{}'{}", e->entity, e->guild, e->name, e->nickname.empty() ? "" : " as " + e->nickname);
    }
    for (const game::GuildView::Line& line : guild_.Apply(event)) {
        const std::string shown = FormatText(text(line.key), {line.argument, line.secondKey.empty() ? line.second : text(line.secondKey), line.third});
        core::LogInfo("game: guild: {}", shown);
        // A war's end: won or lost, in the 1.188 client's "Alarm" box (its message box, UIIT_STT_EVENTGUIDE), not a line.
        if (line.key == "UIIT_MSG_GUILDWAR_WINERGUILD" || line.key == "UIIT_MSG_GUILDWAR_LOSEGUILD") {
            hud_.Inform("UIIT_STT_EVENTGUIDE", {shown}, {});
            continue;
        }
        player_.Post(game::GameMessage::Kind::General, shown, context.time);
        // A surrender: the war closes in a minute, its seconds counted on the warning banner (the original's 60 s timer).
        if (line.key == "UIIT_CTL_GUILDWAR_ENDCOUNT") {
            guildWarEndAt_ = context.time + double(game::kGuildWarEndSeconds);
            guildWarEndShown_ = -1;
        }
        // A war begun: on the warning banner too.
        if (line.key == "UIIT_MSG_GUILDWAR_GOTOWAR") player_.Notify(game::Banner::Kind::Warning, shown, context.time);
    }
    player_.self.guild = guild_.Guild().id;
    // The election's window asked open (its row at the guild NPC).
    if (guild_.TakeElectionShow()) OpenGuildElection(hudContext_);
    if (const auto history = guild_.TakeGpHistory()) OpenGuildGpHistory(hudContext_, *history);
    // A war declared on the player's guild: its master's agreement box (or the test options' answer).
    if (const proto::GuildWarProposal* p = guild_.WarProposal()) {
        const proto::GuildWarProposal war = *p;
        guild_.ClearWarProposal();
        core::LogInfo("game: guild: '{}' ({}) declares war: {} gold", war.guildName, war.fromName, war.terms.wager);
        const auto answer = [this](bool yes) {
            core::LogInfo("game: guild: the war answered {}", yes ? "yes" : "no");
            shared_->session->SendGuild(proto::GuildProposalAnswer{proto::kProposalWar, std::uint8_t(yes ? 1 : 0)});
        };
        if (guildAnswer_ != 0) {
            answer(guildAnswer_ > 0);
        } else {
            AskWarAnswer(hudContext_, war, answer);
        }
    }
    // The war's reward to collect: its box (yes takes it).
    if (const auto gold = guild_.TakeWarReward()) AskWarReward(hudContext_, *gold);
    // The master's recall: "Guild master has recalled guild members. Agree to recall?" with Yes and No (the original's
    // member box CIFGuildSummonReq: ifmessagebox.txt's MsgBoxSimple titled "Agreement window", 252 / 253), closed
    // unanswered after its own 3 minutes (the original's timer; a yes after the server's seconds is refused 0x18C1);
    // --guild-accept or --guild-decline answer it.
    if (const auto ask = guild_.TakeRecallAsk()) {
        core::LogInfo("game: guild: {} recalls the members ({} s)", ask->master, ask->seconds);
        const auto answer = [this](bool yes) {
            recallBox_ = nullptr;
            core::LogInfo("game: guild: the recall answered {}", yes ? "yes" : "no");
            if (!replay_ && shared_ && shared_->session) shared_->session->SendGuild(proto::GuildRecallAnswer{std::uint8_t(yes ? 1 : 0)});
        };
        if (guildAnswer_ != 0) {
            answer(guildAnswer_ > 0);
        } else if (ConfirmBox* box = hud_.Confirm("UIIT_STT_AGREEMENT_BOX", {text("UIIT_MSG_GUILD_RECALL_ANSWER")}, answer)) {
            recallBox_ = box;
            recallUntil_ = context.time + 180.0;
        }
    }
    // The emblems seen and not yet here, asked of the server.
    for (const proto::GuildCrestRequest& r : guild_.TakeCrestRequests()) {
        if (!replay_ && shared_ && shared_->session) shared_->session->SendGuild(r);
    }
    for (const proto::GuildAnswer& a : guild_.TakeAnswers()) {
        core::LogInfo("game: guild: request {} answered 0x{:04x}", int(a.action), a.error);
        // "Contributed [%d]GP.".
        if (a.action == proto::GuildAction::Donate && a.error == 0 && hudContext_.text) {
            player_.Post(game::GameMessage::Kind::General, FormatText(text("UIIT_MSG_GUILD_GP_SUBSCRIPION_RESULT"), {std::to_string(a.value)}), context.time);
        }
        // An emblem registered: the original's box of its success and the notice that it shows in a few minutes, one
        // Confirm button.
        if ((a.action == proto::GuildAction::CrestRegister || a.action == proto::GuildAction::UnionCrestRegister) && a.error == 0 && hudContext_.text) {
            OpenCrestBox(hudContext_, a.action == proto::GuildAction::CrestRegister, true, nullptr);
        }
        if (a.error == 0 || !hudContext_.text) continue;
        // The war's answers the client words itself: no answer in time, nothing to collect, not at war.
        if (a.action == proto::GuildAction::WarDeclare && a.error == std::uint16_t(game::GuildError::TimedOut)) {
            player_.Post(game::GameMessage::Kind::General, text("UIIT_MSG_GUILDWARERR_REQUISITION_TIME_OUT"), context.time);
            continue;
        }
        if ((a.action == proto::GuildAction::WarReward || a.action == proto::GuildAction::WarClaim) &&
            a.error == std::uint16_t(game::GuildError::NoWarReward)) {
            player_.Post(game::GameMessage::Kind::General, text("UIIT_CTL_GUILDWAR_NOTCOMPENSATION"), context.time);
            continue;
        }
        if (a.action == proto::GuildAction::WarEnd && a.error == std::uint16_t(game::GuildError::NotEnemies)) {
            player_.Post(game::GameMessage::Kind::General, text("UIIT_MSG_GUILDWARERR_NOTENEMYGUILD"), context.time);
            continue;
        }
        if ((a.action == proto::GuildAction::WarDeclare || a.action == proto::GuildAction::WarAnswer) && a.error == std::uint16_t(game::GuildError::JoinRefused)) {
            continue; // its line is the war's news
        }
        // The election refused: "Elections cannot be called during this period.".
        if (a.action == proto::GuildAction::ElectionStart && a.error == std::uint16_t(game::GuildError::NoElection)) {
            player_.Post(game::GameMessage::Kind::General, text("UIIT_MSG_MRELEASEERR_NOTVOTETIME"), context.time);
            continue;
        }
        // An emblem there already (0x4C4E; the 1.188 client checks it itself and says so).
        const bool crest = a.action == proto::GuildAction::CrestRegister || a.action == proto::GuildAction::UnionCrestRegister;
        if (crest && a.error == std::uint16_t(game::GuildError::CrestExists)) {
            player_.Post(game::GameMessage::Kind::General, text("UIIT_MSG_CREST_REGISTER_OVERLAP"), context.time);
            continue;
        }
        if (const std::string why = GuildErrorText(hudContext_, a.error, a.value); !why.empty()) {
            player_.Post(game::GameMessage::Kind::General, why, context.time);
        }
    }
    if (const proto::GuildProposal* p = guild_.Proposal()) {
        const proto::GuildProposal proposal = *p;
        guild_.ClearProposal();
        core::LogInfo("game: guild: {} proposes {} ('{}')", proposal.fromName, proposal.kind == proto::kProposalGuild ? "joining its guild" : "a union",
                      proposal.guildName);
        const auto answer = [this, kind = proposal.kind](bool yes) {
            core::LogInfo("game: guild: the proposal answered {}", yes ? "yes" : "no");
            shared_->session->SendGuild(proto::GuildProposalAnswer{kind, std::uint8_t(yes ? 1 : 0)});
        };
        if (guildAnswer_ != 0) {
            answer(guildAnswer_ > 0);
        } else if (proposal.kind == proto::kProposalGuild) {
            // the agreement box, who asks and which guild.
            hud_.Confirm("UIIT_STT_AGREEMENT_BOX",
                         {FormatText(text("UIIT_MSG_GUILD_JOIN_REQUEST"), {proposal.fromName}), FormatText(text("UIIT_MSG_GUILD_QUESTION_JOIN"), {proposal.guildName})},
                         answer);
        } else {
            hud_.Confirm("UIIT_STT_GUILD_RESPECT_ALLY_JOIN", {FormatText(text("UIIT_MSG_QUESTION_GUILD_RESPECT_ALLY_JOIN"), {proposal.guildName})}, answer);
        }
    }
}

void PlayScreen::GuildScript(ScreenContext& context) {
    if (guildScript_.empty()) return;
    context.holdCapture = true; // a capture waits for the last step (--capture-wait frames at most), as --fortress's
    if (guildNext_ < 0.0) guildNext_ = context.time + guildAfter_;
    if (context.time < guildNext_) return;
    guildNext_ = context.time + guildStep_;
    const std::string step = guildScript_.front();
    guildScript_.erase(guildScript_.begin());
    const std::size_t colon = step.find(':');
    const std::string verb = step.substr(0, colon), arg = colon == std::string::npos ? std::string() : step.substr(colon + 1);
    if (CommunityScriptStep(verb, arg, step)) return;
    if (AcademyScriptStep(verb, arg)) return;
    if (OpenMarketScriptStep(verb, arg)) return;
    if (ExchangeScriptStep(verb, arg)) return;
    if (StallScriptStep(context, verb, arg)) return;
    if (InstanceScriptStep(context, verb, arg)) return;
    if (AlchemyScriptStep(verb, arg)) return;
    if (CosScriptStep(verb, arg)) return;
    if (ServiceScriptStep(verb, arg)) return;
    if (FlagScriptStep(context, verb, arg)) return;
    const auto player = [&](const std::string& name) -> std::uint32_t {
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind == proto::EntityKind::Character && e.look.name == name) return id;
        }
        core::LogWarning("game: --guild {}: no player '{}' in sight", step, name);
        return 0;
    };
    const auto member = [&](const std::string& name) -> std::uint32_t {
        for (const auto& m : guild_.Guild().members) {
            if (m.name == name) return m.character;
        }
        return 0;
    };
    auto& session = *shared_->session;
    core::LogInfo("game: --guild {}", step);
    if (verb == "create") {
        session.SendGuild(proto::GuildCreate{NearestNpc(true), arg});
    } else if (verb == "walk-npc") {
        // To the nearest guild NPC, until within its reach (300; the step comes again each second).
        const game::WorldView::Entity* npc = view_.Find(NearestNpc(true));
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (npc != nullptr && self != nullptr) {
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(npc->position);
            const double d = std::hypot(a.x - b.x, a.z - b.z);
            if (d > 250.0) {
                core::LogInfo("game: --guild walk-npc: {:.0f} from the guild NPC", d);
                proto::WorldPosition to = npc->position;
                to.x += 12.0f;
                session.MoveTo(to);
                guildScript_.insert(guildScript_.begin(), step);
                guildNext_ = context.time + 1.0;
            }
        } else {
            core::LogWarning("game: --guild walk-npc: no guild NPC in sight");
        }
    } else if (verb == "invite") {
        if (const std::uint32_t target = player(arg)) session.SendGuild(proto::GuildInvite{target});
    } else if (verb == "target") {
        if (const std::uint32_t target = player(arg)) SelectPlayer(target);
    } else if (verb == "notice") {
        const std::size_t bar = arg.find('|');
        session.SendGuild(proto::GuildNoticeEdit{arg.substr(0, bar), bar == std::string::npos ? std::string() : arg.substr(bar + 1)});
    } else if (verb == "kick") {
        session.SendGuild(proto::GuildKick{arg});
    } else if (verb == "leave") {
        session.SendGuild(proto::GuildLeave{NearestNpc(false)});
    } else if (verb == "disband") {
        session.SendGuild(proto::GuildDisband{NearestNpc(true)});
    } else if (verb == "master") {
        session.SendGuild(proto::GuildMasterTransfer{NearestNpc(true), member(arg)});
    } else if (verb == "permit") {
        const std::size_t eq = arg.find('=');
        session.SendGuild(proto::GuildPermissions{{{member(arg.substr(0, eq)), std::uint32_t(std::strtoul(arg.substr(eq + 1).c_str(), nullptr, 0))}}});
    } else if (verb == "donate") {
        session.SendGuild(proto::GuildDonate{std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10))});
    } else if (verb == "levelup") {
        session.SendGuild(proto::GuildLevelUp{NearestNpc(true)});
    } else if (verb == "chat" || verb == "uchat") {
        hud_.Request({HudRequest::Kind::Chat, int(verb == "chat" ? proto::ChatKind::Guild : proto::ChatKind::Union), arg});
    } else if (verb == "union-invite") {
        if (const std::uint32_t target = player(arg)) session.SendGuild(proto::UnionInvite{target});
    } else if (verb == "union-leave") {
        session.SendGuild(proto::UnionLeave{});
    } else if (verb == "union-kick") {
        for (const auto& g : guild_.Union().guilds) {
            if (g.name == arg) session.SendGuild(proto::UnionKick{g.id});
        }
    } else if (verb == "walk-to" || verb == "talk") {
        // The quests' steps: walk-to:CODENAME walks to that NPC until within its reach (the step
        // comes again each second); talk:CODENAME opens its window, as a click on it.
        std::uint32_t npc = 0;
        for (const auto& [id, e] : view_.Entities()) {
            const assets::ObjectRow* row = e.kind == proto::EntityKind::Character && tables_ && tables_->Usable() ? tables_->characters.Find(e.look.model) : nullptr;
            // a custom NPC by its own codename (its look's name: the Beta Helper, game/BetaQuests.h)
            const std::string code = row == nullptr ? std::string() : e.look.name.empty() ? row->codename : game::CustomNpcCodename(e.look.name);
            if (row != nullptr && code == arg) npc = id;
        }
        const game::WorldView::Entity* there = view_.Find(npc);
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (there == nullptr || self == nullptr) {
            core::LogWarning("game: --guild {}: {} not in sight", step, arg);
        } else if (verb == "talk") {
            session.TalkToNpc(npc);
        } else {
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(there->position);
            const double d = std::hypot(a.x - b.x, a.z - b.z);
            if (d > 60.0 && route_.failed && route_.target == npc) {
                core::LogWarning("game: --guild walk-to: no way to {} ({:.0f} away)", arg, d);
                route_ = {};
            } else if (d > 60.0) {
                // Round what stands between (PlayRoute.cpp): the way asked for once, the step again each second until
                // the walker is there.
                if (!Routing() || route_.target != npc) {
                    core::LogInfo("game: --guild walk-to: {:.0f} from {}", d, arg);
                    proto::WorldPosition to = there->position;
                    to.x += 12.0f;
                    RouteTo(to, npc, 45.0f); // within talking reach (an NPC behind a counter: its side)
                }
                guildScript_.insert(guildScript_.begin(), step);
                guildNext_ = context.time + 1.0;
            }
        }
    } else if (verb == "quest-row") {
        // quest-row:CODENAME: the NPC window's row of that quest (its name).
        const game::QuestDef* q = nullptr;
        for (const auto& row : quests_.NpcRows().rows)
            if (const game::QuestDef* d = quests_.Find(row.quest); d != nullptr && d->codename == arg) q = d;
        auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc));
        const std::string name = q == nullptr ? std::string() : !q->name.key.empty() ? gui_.data().Text(q->name.key) : q->name.literal;
        if (npc == nullptr || q == nullptr || !npc->ChooseText(name)) core::LogWarning("game: --guild {}: no such quest row", step);
    } else if (verb == "quest-window") {
        ToggleQuestWindow(hudContext_);
    } else if (verb == "quest-view") {
        // quest-view:CODENAME: the reward window of that quest under way, as its content button.
        for (const auto& [id, s] : quests_.Active())
            if (const game::QuestDef* d = quests_.Find(id); d != nullptr && d->codename == arg) OpenQuestReward(hudContext_, id, 0);
    } else if (verb == "quest-abandon") {
        for (const auto& [id, s] : quests_.Active())
            if (const game::QuestDef* d = quests_.Find(id); d != nullptr && d->codename == arg) session.SendGame(proto::QuestAbandon::kId, proto::Encode(proto::QuestAbandon{id}));
    } else if (verb == "npc-menu") {
        // The NPC window of the nearest guild NPC (its guild rows), as a click on it.
        if (const std::uint32_t npc = NearestNpc(true)) session.TalkToNpc(npc);
    } else if (verb == "shop-close") {
        // The store window put away (a specialty NPC's: the server decides the safe trade).
        if (hud_.Shown(hud::kStore)) hud_.Show(hud::kStore)->visible = false;
        else core::LogWarning("game: --guild {}: no store open", step);
    } else if (verb == "npc-choose") {
        if (auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc))) npc->Choose(std::atoi(arg.c_str()));
    } else if (verb == "goto") {
        session.DevGoto(std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10))); // a developer's teleport to that point
    } else if (verb == "gate") {
        // gate:GATE,TO: the gate (its structure's id) walked to and used toward point TO, as --use-gate / --gate-to.
        unsigned gate = 0, to = 0;
        if (std::sscanf(arg.c_str(), "%u,%u", &gate, &to) >= 1) {
            pendingGate_ = gate;
            gateTo_ = to;
            gateTries_ = 20;
        }
    } else if (verb == "use") {
        // use:CODENAME: the bag's first item of that codename used (a hole, a scroll).
        bool used = false;
        for (const auto& [place, item] : player_.inventory.places) {
            const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (row == nullptr || row->codename != arg || place < game::slot::kWorn) continue;
            session.SendGame(proto::UseItem::kId, proto::Encode(proto::UseItem{place}));
            used = true;
            break;
        }
        if (!used) core::LogWarning("game: --guild {}: no {} in the bag", step, arg);
    } else if (verb == "mall-use") {
        // mall-use:CODENAME: the bag's first of it used as a right click would (the mall's items ask first; PlayMallItems.cpp).
        for (const auto& [place, item] : player_.inventory.places) {
            const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (row == nullptr || row->codename != arg || place < game::slot::kWorn) continue;
            hud_.Request({HudRequest::Kind::UseItem, place});
            break;
        }
    } else if (verb == "command") {
        // command:TEXT: a "/" command typed in the chat (the premium's /Return, /Reverse Return, /Resurrection).
        hud_.Request({HudRequest::Kind::ChatCommand, 0, arg});
    } else if (verb == "die") {
        session.SendGame(proto::DevDie::kId, proto::Encode(proto::DevDie{})); // a developer's death (SroGame --dev-commands)
    } else if (verb == "reward-pick") {
        // reward-pick:ITEM: the open reward window's item of that codename chosen and taken.
        if (!PickQuestReward(hudContext_, arg)) core::LogWarning("game: --guild {}: no reward window with that item", step);
    } else if (verb == "exp") {
        // exp:N: a developer's experience (SroGame --dev-commands; a level reached for the quests given by level).
        session.SendGame(proto::DevExperience::kId, proto::Encode(proto::DevExperience{std::strtoull(arg.c_str(), nullptr, 10), 0}));
    } else if (verb == "goto-place") {
        // goto-place:REGION,X,Z[,Y]: a developer's teleport to that place (REGION decimal or 0x..; a dungeon's needs Y).
        char region[16]{};
        float x = 0.0f, z = 0.0f, y = proto::DevGotoPlace::kNoHeight;
        if (std::sscanf(arg.c_str(), "%15[^,],%f,%f,%f", region, &x, &z, &y) >= 3) {
            const auto r = std::uint16_t(std::strtol(region, nullptr, 0));
            session.SendGame(proto::DevGotoPlace::kId, proto::Encode(proto::DevGotoPlace{r, x, z, y}));
        } else {
            core::LogWarning("game: goto-place: '{}' is not REGION,X,Z[,Y]", arg);
        }
    } else if (verb == "quick-action" || verb == "use-quick") {
        // quick-action:SLOT,ID: an Action page icon dropped on quick slot SLOT (its shortcut, kind 74); use-quick:SLOT:
        // that slot pressed (as its key).
        int slot = -1;
        unsigned action = 0;
        if (verb == "quick-action" && std::sscanf(arg.c_str(), "%d,%u", &slot, &action) == 2 && slot >= 0 && slot < game::QuickSlots::kCount) {
            player_.quickSlots.slots[std::size_t(slot)] = {game::QuickSlot::Kind::Action, action};
            hud_.Request({HudRequest::Kind::QuickSlotChanged, slot});
            core::LogInfo("game: --guild {}: action {} on quick slot {}", step, action, slot);
        } else if (verb == "use-quick" && std::sscanf(arg.c_str(), "%d", &slot) == 1 && slot >= 0 && slot < game::QuickSlots::kCount) {
            hud_.UseQuickSlot(slot, context.time, true);
            const game::QuickSlot& q = player_.quickSlots.slots[std::size_t(slot)];
            core::LogInfo("game: --guild {}: used (its icon now {})", step, hudContext_.slotIcon ? hudContext_.slotIcon(q) : std::string());
        } else {
            core::LogWarning("game: --guild {}: expected quick-action:SLOT,ID or use-quick:SLOT", step);
        }
    } else if (verb == "click-gate") {
        // click-gate:REFOBJID: the player's click on the screen where that gate stands (its spot projected), through
        // the pick as a mouse's (tries of the gates' pick).
        const assets::TeleportGate* gate = nullptr;
        if (tables_ && tables_->Usable()) {
            for (const assets::TeleportGate& g : tables_->teleports.Gates()) {
                if (g.refObjId == std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10))) gate = &g;
            }
        }
        const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
        float x = 0.0f, y = 0.0f;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (gate == nullptr || !world_ || self == nullptr) {
            core::LogWarning("game: --guild {}: no such gate", step);
        } else if (!world_->Project({gate->region, gate->x, gate->y, gate->z}, 0.0f, width, height, x, y) || x < 0.0f || y < 0.0f || x > width ||
                   y > height) {
            // Out of view: the camera turned toward it (the step again clicks).
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat({gate->region, gate->x, gate->y, gate->z});
            cameraYaw_ = float(std::atan2(b.x - a.x, b.z - a.z));
            const double flat = std::max(1.0, std::hypot(b.x - a.x, b.z - a.z));
            cameraPitch_ = std::clamp(-float(std::atan2(double(gate->y - self->position.y), flat)), -1.07f, 1.07f); // within the pitch's limit
            core::LogInfo("game: --guild {}: out of view, the camera turned toward it", step);
        } else {
            ClickWorld(context, x, y, false);
            core::LogInfo("game: --guild {}: clicked at {:.0f},{:.0f}: {}", step, x, y, lastClick_);
        }
    } else if (verb == "goto-mob") {
        // A developer's teleport to a nest of that monster (SroGame --dev-commands; the quests' hunts).
        session.SendGame(proto::DevGotoMonster::kId, proto::Encode(proto::DevGotoMonster{arg}));
    } else if (verb == "give") {
        // give:ITEM,N: a developer's item into the bag (SroGame --dev-commands).
        const std::size_t comma = arg.find(',');
        const std::uint32_t count = comma == std::string::npos ? 1u : std::uint32_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 10));
        session.SendGame(proto::DevGive::kId, proto::Encode(proto::DevGive{arg.substr(0, comma), count}));
    } else if (verb == "hunt") {
        // hunt[:MOB]: fights the nearest monster (of that codename) one after another, until hunt-stop; round walls
        // (PlayRoute.cpp), leaving one it finds no way to.
        autoPotionTries_.attackAfter = 0.0f;
        autoPotionTries_.hunt = arg;
    } else if (verb == "hunt-stop") {
        autoPotionTries_.attackAfter = -1.0f;
        autoPotionTries_.attacking = 0;
        route_ = {};
    } else if (verb == "loot") {
        UseAction(context, 1008); // the Action page's Auto Looting: the drops around picked up
    } else if (verb == "job-rank" || verb == "job-donation" || verb == "job-outcome" || verb == "job-collect") {
        // The keeper's rankings and the week's outcome: job-rank the activity ranking, job-donation
        // the donation or contribution one, job-outcome asks the outcome, job-collect the talk's collect row then.
        auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc));
        bool chosen = false;
        if (npc != nullptr) {
            if (verb == "job-rank") chosen = npc->ChooseJob(JobNpcRow::ActivityRank);
            else if (verb == "job-donation") chosen = npc->ChooseJob(JobNpcRow::DonationRank) || npc->ChooseJob(JobNpcRow::ContributionRank);
            else if (verb == "job-outcome") chosen = npc->ChooseJob(JobNpcRow::Outcome);
            else chosen = npc->ChooseJob(JobNpcRow::OutcomeCollect);
        }
        if (!chosen) core::LogWarning("game: --guild {}: no such row on the NPC's window", step);
    } else if (verb == "rank-page") {
        // rank-page:N: the open ranking window's spinner at page N (0 the first).
        if (!PageJobRanking(hudContext_, std::atoi(arg.c_str()))) core::LogWarning("game: --guild {}: no ranking open", step);
    } else if (verb == "job-join" || verb == "job-leave" || verb == "job-previous" || verb == "job-alias") {
        // The league keeper's rows (HudJobs.cpp) on its open NPC window (talk:NPC first): join and
        // leave ask first ("yes" answers); job-alias:NAME opens the alias window, types NAME and checks it.
        auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc));
        bool chosen = false;
        if (npc != nullptr) {
            if (verb == "job-join") chosen = npc->ChooseJob(JobNpcRow::Join);
            else if (verb == "job-leave") chosen = npc->ChooseJob(JobNpcRow::Leave);
            else if (verb == "job-previous") chosen = npc->ChooseJob(JobNpcRow::Previous);
            else chosen = (npc->ChooseJob(JobNpcRow::AliasCreate) || npc->ChooseJob(JobNpcRow::AliasModify)) && TypeJobAlias(hudContext_, arg);
        }
        if (!chosen) core::LogWarning("game: --guild {}: no such row on the NPC's window", step);
    } else if (verb == "cart-buy" || verb == "cart-sell" || verb == "cart-sell-all" || verb == "trade-board") {
        // The trade (HudTrade.cpp): cart-buy:SLOT,COUNT a good of the open specialty shop's first
        // tab into the transport through the deal window; cart-sell:PLACE[,COUNT] a good of the transport's bag;
        // cart-sell-all; trade-board:N the price board, its trade centre N picked and refreshed.
        const std::size_t comma = arg.find(',');
        const int first = std::atoi(arg.c_str());
        const int second = comma == std::string::npos ? -1 : std::atoi(arg.c_str() + comma + 1);
        bool done = false;
        if (verb == "trade-board") {
            OpenTradeBoard(hudContext_);
            done = RefreshTradeBoard(hudContext_, first);
        } else if (verb == "cart-sell-all") {
            if (hudContext_.tradeNpc != 0) session.SendGame(proto::TradeSellAll::kId, proto::Encode(proto::TradeSellAll{hudContext_.tradeNpc}));
            done = hudContext_.tradeNpc != 0;
        } else if (verb == "cart-sell") {
            for (const HudContext::CosShown& c : hudContext_.coses) {
                if (c.kind != std::uint8_t(game::CosKind::Transport)) continue;
                if (const auto it = c.bag.find(std::uint8_t(first)); it != c.bag.end() && SellFromCart(hudContext_, it->second, first)) {
                    done = ConfirmTradeDeal(hudContext_, second);
                }
            }
        } else if (const game::WorldView::Entity* npc = view_.Find(hudContext_.tradeNpc); npc != nullptr && tables_ && tables_->Usable()) {
            const assets::ObjectRow* row = tables_->characters.Find(npc->look.model);
            const assets::NpcShop* shop = row != nullptr ? tables_->shops.Find(row->codename) : nullptr;
            if (shop != nullptr && !shop->groups.empty() && !shop->groups.front().tabs.empty()) {
                const assets::ShopTab& tab = shop->groups.front().tabs.front();
                for (const assets::ShopGood& good : tab.goods) {
                    if (good.slot == first && BuyIntoCart(hudContext_, good, tab.index)) done = ConfirmTradeDeal(hudContext_, second);
                }
            }
        }
        if (!done) core::LogWarning("game: --guild {}: nothing to do", step);
    } else if (verb == "character") {
        hud_.OpenPage(hud::kPageCharacter); // the main window's Character page (its job rows)
    } else if (verb == "job-alias-ok") {
        // The alias window's OK once its name was checked free (then "yes").
        if (!ConfirmJobAlias(hudContext_)) core::LogWarning("game: --guild {}: no alias checked", step);
    } else if (verb == "npc-row") {
        // The NPC window's row of that text key (UIIT_CTL_MASTERRELEASE, ...).
        auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc));
        if (npc == nullptr || !npc->ChooseText(gui_.data().Text(arg))) core::LogWarning("game: --guild {}: no such row", step);
    } else if (verb == "write-notice") {
        // write-notice:TITLE|BODY: the editor opened by its pencil, typed in and confirmed.
        OpenGuildNoticeWrite(hudContext_);
        const std::size_t bar = arg.find('|');
        if (!FillGuildNotice(hudContext_, arg.substr(0, bar), bar == std::string::npos ? std::string() : arg.substr(bar + 1))) core::LogWarning("game: --guild {}: no editor", step);
    } else if (verb == "donate-window") {
        // donate-window:N: the GP button's window, N typed, its OK pressed.
        OpenGuildDonate(hudContext_);
        if (!FillGuildDonate(hudContext_, std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10)))) core::LogWarning("game: --guild {}: no donate window", step);
    } else if (verb == "position") {
        // position:MEMBER,VALUE: the window for that member, the box of that value (kPosition*), confirmed.
        const std::size_t comma = arg.find(',');
        OpenGuildPosition(hudContext_, member(arg.substr(0, comma)));
        if (!PickGuildPosition(hudContext_, std::uint8_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 0)), true)) core::LogWarning("game: --guild {}: no position window", step);
    } else if (verb == "global") {
        // global:TEXT: the first Global chatting item of the bag used, the line typed and sent.
        bool used = false;
        for (const auto& [place, item] : player_.inventory.places) {
            if (place >= game::slot::kWorn && UseGlobalChatItem(place)) {
                used = true;
                if (auto* window = dynamic_cast<GlobalChatWindow*>(hud_.Show(hud::kGlobalChat))) window->Say(arg);
                break;
            }
        }
        if (!used) core::LogWarning("game: --guild {}: no Global chatting item in the bag", step);
    } else if (verb == "hide") {
        // hide:ID: that window closed (what it covers seen).
        if (ui::Widget* window = hud_.Show(std::atoi(arg.c_str()))) window->visible = false;
    } else if (verb == "promote-ok" || verb == "promote-cancel") {
        if (!PressGuildLevelUp(hudContext_, verb == "promote-ok")) core::LogWarning("game: --guild {}: no promotion window", step);
    } else if (verb == "buy") {
        // buy:TAB,SLOT,COUNT: a good of the open store, as its buy box confirmed.
        int tab = 0, slot = 0, count = 1;
        std::sscanf(arg.c_str(), "%d,%d,%d", &tab, &slot, &count);
        HudRequest request{HudRequest::Kind::ShopBuy, tab | slot << 8};
        request.extra = count;
        hud_.Request(request);
    } else if (verb == "name") {
        if (ui::Interface* ui = gui_.interface(); ui == nullptr || !AnswerGuildNameBox(*ui, arg)) core::LogWarning("game: --guild {}: no name box", step);
    } else if (verb == "yes" || verb == "no") {
        if (!AnswerWarTerms(hudContext_, verb == "yes")) hud_.AnswerConfirm(verb == "yes");
    } else if (verb == "window") {
        ui::Widget* window = hud_.Show(hud::kCommunity);
        ShowGuildPage(window, arg == "union" ? 1 : arg == "hostile" ? 2 : 0);
    } else if (verb == "war") {
        // war:GUILD,victory,days,hours,minutes,wager: the request window filled and confirmed (then "yes").
        std::vector<std::string> f;
        for (std::string rest = arg; !rest.empty();) {
            const std::size_t comma = rest.find(',');
            f.push_back(rest.substr(0, comma));
            rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
        }
        proto::GuildWarTerms terms;
        const auto number = [&](std::size_t i) { return i < f.size() ? std::strtoul(f[i].c_str(), nullptr, 10) : 0ul; };
        terms.victory = std::uint8_t(number(1));
        terms.days = std::uint8_t(number(2));
        terms.hours = std::uint8_t(number(3));
        terms.minutes = std::uint8_t(number(4));
        terms.wager = std::uint32_t(number(5));
        if (f.empty() || !FillGuildWarRequest(hudContext_, f[0], terms, true)) core::LogWarning("game: --guild {}: no request window", step);
    } else if (verb == "war-end") {
        for (const auto& w : guild_.Wars().wars) {
            if (w.name == arg) session.SendGuild(proto::GuildWarEnd{w.guild});
        }
    } else if (verb == "war-reward") {
        hudContext_.guildNpc = NearestNpc(true);
        session.SendGuild(proto::GuildWarRewardQuery{hudContext_.guildNpc});
    } else if (verb == "strike") {
        // strike:NAME,DAMAGE: a developer's hit on an enemy player (the war's kills).
        const std::size_t comma = arg.find(',');
        if (const std::uint32_t target = player(arg.substr(0, comma))) {
            session.SendGuild(proto::DevStrike{target, comma == std::string::npos ? 1000u : std::uint32_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 10))});
        }
    } else if (verb == "smite") {
        // smite:MOB[,DAMAGE]: a developer's hit on the nearest monster of that codename in sight (the quests' tests: a
        // unique felled; SroGame --dev-commands), by default enough to kill it.
        const std::size_t comma = arg.find(',');
        const std::string codename = arg.substr(0, comma);
        const std::uint32_t damage = comma == std::string::npos ? 2000000000u : std::uint32_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 10));
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        std::uint32_t best = 0;
        double bestDistance = 1e18;
        for (const auto& [id, e] : view_.Entities()) {
            const assets::ObjectRow* row = e.kind == proto::EntityKind::Monster && tables_ && tables_->Usable() ? tables_->characters.Find(e.look.model) : nullptr;
            if (row == nullptr || row->codename != codename || self == nullptr || killed_.contains(id)) continue;
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(e.position);
            if (const double d = std::hypot(a.x - b.x, a.z - b.z); d < bestDistance) {
                best = id;
                bestDistance = d;
            }
        }
        if (best == 0) {
            core::LogWarning("game: --guild {}: no {} in sight", step, codename);
        } else {
            core::LogInfo("game: --guild smite: {} ({}) at {:.0f}", codename, best, bestDistance);
            session.SendGuild(proto::DevStrike{best, damage});
        }
    } else if (verb == "attack") {
        if (const std::uint32_t target = player(arg)) {
            SelectPlayer(target);
            session.Attack(target);
        }
    } else if (verb == "force-attack" || verb == "click-player") {
        // force-attack:NAME: a click on that player with Alt held (the force PK); click-player:NAME
        // a plain click (the first selects, the next on the selected one attacks an enemy).
        if (const std::uint32_t target = player(arg)) ClickPlayer(context, target, false, verb == "force-attack");
    } else if (verb == "pick" || verb == "pick-confirm") {
        // The open transfer or election window: that member's row checked (and Confirm pressed).
        if (!PickGuildMasterRow(hudContext_, arg, verb == "pick-confirm")) core::LogWarning("game: --guild {}: no such row in an open window", step);
    } else if (verb == "crest" || verb == "crest-delete" || verb == "union-crest" || verb == "union-crest-delete") {
        // The first emblem item of that kind in the bag, used as a right click would.
        const std::string code = verb == "crest" ? "ITEM_ETC_GUILD_CREST_USER" : verb == "crest-delete" ? "ITEM_ETC_GUILD_CREST_DELETE"
                                 : verb == "union-crest" ? "ITEM_ETC_UNION_CREST_USER" : "ITEM_ETC_UNION_CREST_DELETE";
        bool used = false;
        for (const auto& [place, item] : player_.inventory.places) {
            const assets::ItemRow* row = place >= game::slot::kWorn && tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (row != nullptr && row->codename == code) {
                used = UseCrestItem(place);
                break;
            }
        }
        if (!used) core::LogWarning("game: --guild {}: no {} in the bag", step, code);
    } else if (verb != "wait") {
        core::LogWarning("game: --guild {}: not a step", step);
    }
}

bool PlayScreen::DrawGuildLine(ui::Painter& paint, std::uint32_t entity, const ui::Rect& nameAt) {
    // [guild] or [guild * title], 20 pixels over the name, centred, in the guild's
    // colour (the player's own orange, its union's light green, the others light blue).
    std::string guild, nickname;
    std::uint32_t id = 0, unionId = 0, crest = 0, unionCrest = 0;
    if (entity == view_.Self()) {
        if (!guild_.InGuild()) return false;
        guild = guild_.Guild().name;
        id = guild_.Guild().id;
        unionId = guild_.Guild().unionId;
        crest = guild_.Guild().crest;
        unionCrest = unionId != 0 && guild_.Union().id == unionId ? guild_.Union().crest : 0;
        if (const proto::GuildMemberRecord* self = guild_.Self()) nickname = self->nickname;
    } else if (const proto::EntityGuild* tag = guild_.EntityTag(entity)) {
        guild = tag->name;
        nickname = tag->nickname;
        id = tag->guild;
        unionId = tag->unionId;
        crest = tag->crest;
        unionCrest = tag->unionCrest;
    } else {
        return false;
    }
    const std::string line = game::GuildView::HeadLine(guild, nickname);
    const float w = paint.TextWidth(line, 0) + 2.0f;
    const ui::Rect at{std::floor(nameAt.x + (nameAt.w - w) / 2.0f), nameAt.y - 20.0f, w, nameAt.h};
    const game::WorldView::Entity* body = view_.Find(entity);
    const bool foe = body != nullptr && entity != view_.Self() && FortressFoe(entity, *body);
    ui::DrawLabel(paint, line, at, ui::Argb(foe ? 0xFFFF0000 : guild_.HeadColour(id, unionId)), 0, 1, 1, ui::kTextShadow);
    // the emblems, 16 x 16, left of the line: the guild's 20 pixels before it, the
    // union's 36 (the size).
    const float side = float(game::kCrestSide), top = std::floor(at.y + (at.h - side) / 2.0f);
    if (const std::string path = CrestTexture(proto::kCrestGuild, id, crest); !path.empty()) paint.Image(path, {at.x - 20.0f, top, side, side});
    if (const std::string path = CrestTexture(proto::kCrestUnion, unionId, unionCrest); !path.empty()) paint.Image(path, {at.x - 36.0f, top, side, side});
    return true;
}

} // namespace sr::client
