#include "sr/game/Guild.h"

#include <algorithm>

namespace sr::game {

std::size_t Utf8Length(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

namespace {
template <class... F>
struct Overloaded : F... {
    using F::operator()...;
};
template <class... F>
Overloaded(F...) -> Overloaded<F...>;
} // namespace

std::vector<GuildView::Line> GuildView::Apply(const proto::GuildEvent& event) {
    std::vector<Line> lines;
    ++revision_;
    const auto find = [&](std::uint32_t character) {
        return std::find_if(guild_.members.begin(), guild_.members.end(), [&](const auto& m) { return m.character == character; });
    };
    std::visit(Overloaded{
                   [&](const proto::GuildAnswer& a) {
                       answers_.push_back(a);
                       if (a.error != 0) return;
                       // The client's lines on its own success (the created guild's name).
                       if (a.action == proto::GuildAction::Create) lines.push_back({"UIIT_MSG_GUILD_CREATE_RESULT", guild_.name});
                       // An emblem registered: its line and the notice that it may take a while to show
                       // (registered); deleted: its line (the box answered).
                       const bool guildCrest = a.action == proto::GuildAction::CrestRegister;
                       if (guildCrest || a.action == proto::GuildAction::UnionCrestRegister) {
                           lines.push_back({guildCrest ? "UIIT_MSG_GUILD_CREST_REGISTER_COMPLETE" : "UIIT_MSG_UNION_CREST_REGISTER_COMPLETE", {}});
                           for (const char* key : {"UIIT_MSG_CREST_REGISTER_INFORMATION_01", "UIIT_MSG_CREST_REGISTER_INFORMATION_02",
                                                   "UIIT_MSG_CREST_REGISTER_INFORMATION_03", "UIIT_MSG_CREST_REGISTER_INFORMATION_04"}) {
                               lines.push_back({key, {}});
                           }
                       }
                       if (a.action == proto::GuildAction::CrestDelete) lines.push_back({"UIIT_MSG_GUILD_CREST_DELETE_COMPLETE", {}});
                       // The leadership handed over (the transfer window's box answered).
                       if (a.action == proto::GuildAction::MasterTransfer) lines.push_back({"UIIT_MSG_MLEAVE_SUCCESS", {}});
                       if (a.action == proto::GuildAction::UnionCrestDelete) lines.push_back({"UIIT_MSG_UNION_CREST_DELETE_COMPLETE", {}});
                   },
                   [&](const proto::GuildProposal& p) { proposal_ = p; },
                   [&](const proto::GuildInfo& g) {
                       guild_ = g;
                       if (g.id == 0) {
                           union_ = {};
                           election_ = {};
                       }
                       WantCrest(proto::kCrestGuild, g.id, g.crest);
                   },
                   [&](const proto::GuildMemberJoined& j) {
                       if (const auto it = find(j.member.character); it != guild_.members.end()) {
                           *it = j.member;
                       } else {
                           guild_.members.push_back(j.member);
                       }
                       lines.push_back({"UIIT_MSG_GUILD_JOIN_RESULT", j.member.name});
                   },
                   [&](const proto::GuildMemberRemoved& r) {
                       const bool self = r.name == selfName_;
                       if (r.reason == 2) {
                           lines.push_back({"UIIT_MSG_GUILD_EXPULSION_RESULT", r.name}); // everyone sees it
                       } else if (self) {
                           lines.push_back({"UIIT_MSG_GUILD_EXIT_RESULT", guild_.name}); // only the one who left
                       }
                       if (const auto it = find(r.character); it != guild_.members.end()) guild_.members.erase(it);
                   },
                   [&](const proto::GuildMemberChanged& c) {
                       const auto it = find(c.member.character);
                       if (it == guild_.members.end()) {
                           guild_.members.push_back(c.member);
                           return;
                       }
                       if (it->online != c.member.online && c.member.name != selfName_) {
                           lines.push_back({c.member.online ? "UIIT_MSG_GUILD_CONNECT" : "UIIT_MSG_GUILD_DISCONNECT", c.member.name});
                       }
                       // A title granted (only the master grants them): "You have designated [%s] as [%s]."
                       // on the master's side.
                       if (it->nickname != c.member.nickname && !c.member.nickname.empty() && Self() != nullptr && Self()->memberClass == kGuildMaster) {
                           lines.push_back({"UIIT_MSG_GUILD_NAME_GRANT_RESULT", c.member.name, c.member.nickname});
                       }
                       // A position given or taken away (the commander's goes with the master's place).
                       if (it->position != c.member.position && it->position != kPositionCommander && c.member.position != kPositionCommander) {
                           if (c.member.position != kPositionNone) {
                               Line line{"UIIT_MSG_FORT_POSITION_GRANT_COMPLETE", c.member.name};
                               line.secondKey = GuildPositionKey(c.member.position);
                               lines.push_back(std::move(line));
                           } else {
                               lines.push_back({"UIIT_MSG_FORT_POSITION_REMOVE_COMPLETE", c.member.name});
                           }
                       }
                       *it = c.member;
                   },
                   [&](const proto::GuildDisbanded& d) {
                       lines.push_back({"UIIT_MSG_GUILD_BREAK_RESULT", d.name});
                       guild_ = {};
                       union_ = {};
                       election_ = {};
                       wars_ = {};
                   },
                   [&](const proto::GuildNoticeChanged& n) {
                       guild_.noticeTitle = n.title;
                       guild_.notice = n.body;
                       lines.push_back({"UIIT_MSG_GUILD_COMMON_KNOW_REMIND_UPDATE", {}});
                   },
                   [&](const proto::UnionInfo& u) {
                       union_ = u;
                       guild_.unionId = u.id;
                       WantCrest(proto::kCrestUnion, u.id, u.crest);
                   },
                   [&](const proto::UnionChanged& c) {
                       static constexpr const char* kKeys[] = {"", "UIIT_MSG_GUILD_ALLY_JOIN_RESULT", "UIIT_MSG_GUILD_ALLY_EXIT_RESULT",
                                                               "UIIT_MSG_GUILD_RESPECT_ALLY_EXPEL_RESULT",
                                                               "UIIT_MSG_QUESTION_GUILD_RESPECT_ALLY_BREAKUP"};
                       if (c.event >= 1 && c.event <= 4) lines.push_back({kKeys[c.event], c.guild});
                   },
                   [&](const proto::GuildPoints& p) {
                       if (p.level > guild_.level && guild_.id != 0) lines.push_back({"UIIT_MSG_GUILD_LEVEL_UP_RESULT", std::to_string(p.level)});
                       guild_.level = p.level;
                       guild_.points = p.points;
                   },
                   [&](const proto::EntityGuild& e) {
                       if (e.guild == 0) {
                           entities_.erase(e.entity);
                       } else {
                           entities_[e.entity] = e;
                           WantCrest(proto::kCrestGuild, e.guild, e.crest);
                           WantCrest(proto::kCrestUnion, e.unionId, e.unionCrest);
                       }
                   },
                   [&](const proto::GuildElection& e) {
                       // Its start for every member: "The voting for the leader has just started."
                       if (e.active != 0 && election_.active == 0) lines.push_back({"UIIT_MSG_MRELEASE_VOTING", {}});
                       election_ = e;
                       if (e.show != 0) electionShow_ = true;
                   },
                   [&](const proto::GuildWarProposal& p) { warProposal_ = p; },
                   [&](const proto::GuildWars& w) { wars_ = w; },
                   [&](const proto::GuildWarReward& r) { warReward_ = r.gold; },
                   [&](const proto::GuildGpHistory& h) { gpHistory_ = h; },
                   [&](const proto::GuildRecallAsk& r) { recallAsk_ = r; }, // its box and countdown: the client's (not built yet)
                   [&](const proto::GuildWarNews& n) {
                       // The 1.188 client's lines (begun, a kill).
                       static constexpr const char* kKeys[] = {"", "UIIT_MSG_GUILDWAR_GOTOWAR", "UIIT_MSG_GUILDWAR_WARREFUSAL",
                                                               "UIIT_MSG_GUILDWAR_KILLNEWS", "UIIT_CTL_GUILDWAR_ENDCOUNT", "UIIT_CTL_GUILDWAR_ENDWAR",
                                                               "UIIT_MSG_GUILDWAR_WINERGUILD", "UIIT_MSG_GUILDWAR_LOSEGUILD"};
                       if (n.kind >= 1 && n.kind <= 7) lines.push_back({kKeys[n.kind], n.a, n.b, n.c});
                   },
                   [&](const proto::GuildElectionEnded& e) {
                       election_ = {};
                       if (e.winner.empty()) {
                           lines.push_back({"UIIT_MSG_MRELEASE_BROKEN", {}});
                       } else {
                           lines.push_back({"UIIT_MSG_MRELEASE_BEELETED", e.guild, e.winner});
                       }
                   },
                   [&](const proto::GuildCrest& c) {
                       if (c.pixels.size() != proto::kCrestBytes || c.revision == 0) return;
                       CrestPixels& kept = crests_[{c.kind, c.id}];
                       if (c.revision >= kept.revision) kept = {c.revision, c.pixels};
                   },
               },
               event);
    return lines;
}

const proto::GuildMemberRecord* GuildView::Member(std::uint32_t character) const {
    for (const auto& m : guild_.members) {
        if (m.character == character) return &m;
    }
    return nullptr;
}

const proto::GuildMemberRecord* GuildView::Master() const {
    for (const auto& m : guild_.members) {
        if (m.memberClass == kGuildMaster) return &m;
    }
    return nullptr;
}

const proto::GuildMemberRecord* GuildView::Self() const {
    for (const auto& m : guild_.members) {
        if (m.name == selfName_) return &m;
    }
    return nullptr;
}

bool GuildView::May(std::uint32_t permission) const {
    const proto::GuildMemberRecord* self = Self();
    return self != nullptr && (self->memberClass == kGuildMaster || (self->permission & permission) != 0);
}

int GuildView::Online() const {
    return int(std::count_if(guild_.members.begin(), guild_.members.end(), [](const auto& m) { return m.online != 0; }));
}

void GuildView::WantCrest(std::uint8_t kind, std::uint32_t id, std::uint32_t revision) {
    if (id == 0 || revision == 0 || Crest(kind, id, revision) != nullptr) return;
    if (crestsAsked_.insert({kind, id, revision}).second) crestRequests_.push_back({kind, id, revision});
}

const std::vector<std::uint8_t>* GuildView::Crest(std::uint8_t kind, std::uint32_t id, std::uint32_t revision) const {
    const auto it = crests_.find({kind, id});
    return it != crests_.end() && it->second.revision == revision && revision != 0 ? &it->second.pixels : nullptr;
}

std::optional<CrestItem> CrestItemOf(std::string_view codename) {
    if (codename == "ITEM_ETC_GUILD_CREST_USER") return CrestItem{proto::kCrestGuild, false};
    if (codename == "ITEM_ETC_GUILD_CREST_DELETE") return CrestItem{proto::kCrestGuild, true};
    if (codename == "ITEM_ETC_UNION_CREST_USER") return CrestItem{proto::kCrestUnion, false};
    if (codename == "ITEM_ETC_UNION_CREST_DELETE") return CrestItem{proto::kCrestUnion, true};
    return std::nullopt;
}

CrestFile ReadCrestBitmap(std::span<const std::uint8_t> file, std::vector<std::uint8_t>& pixels) {
    pixels.clear();
    const auto u16 = [&](std::size_t at) { return std::uint32_t(file[at]) | std::uint32_t(file[at + 1]) << 8; };
    const auto u32 = [&](std::size_t at) { return u16(at) | u16(at + 2) << 16; };
    // The file header (14 bytes), then the bitmap's: its width at 18, height at 22, bits at 28.
    if (file.size() < 54 || file[0] != 'B' || file[1] != 'M') return CrestFile::Format;
    if (u16(28) != 24 || std::int32_t(u32(18)) != kCrestSide || std::int32_t(u32(22)) != kCrestSide) return CrestFile::Format;
    constexpr std::size_t kRow = kCrestSide * 3; // 48: already a multiple of 4
    const std::size_t offset = u32(10);
    if (offset < 54 || offset > file.size() || file.size() - offset < kRow * kCrestSide) return CrestFile::Format;
    pixels.resize(proto::kCrestBytes);
    for (int y = 0; y < kCrestSide; ++y) {
        const std::uint8_t* row = file.data() + offset + kRow * std::size_t(kCrestSide - 1 - y); // bottom-up
        std::copy(row, row + kRow, pixels.begin() + std::ptrdiff_t(kRow * std::size_t(y)));
    }
    return CrestFile::Ok;
}

const proto::EntityGuild* GuildView::EntityTag(std::uint32_t entity) const {
    const auto it = entities_.find(entity);
    return it != entities_.end() ? &it->second : nullptr;
}

std::uint32_t GuildView::UnionOf(std::uint32_t guild) const {
    if (guild == 0) return 0;
    if (InGuild() && Guild().id == guild) return Guild().unionId;
    for (const auto& [entity, tag] : entities_) {
        if (tag.guild == guild) return tag.unionId;
    }
    return 0;
}

std::string GuildView::HeadLine(std::string_view guild, std::string_view nickname) {
    std::string line = "[";
    line += guild;
    if (!nickname.empty()) {
        line += " * ";
        line += nickname;
    }
    line += "]";
    return line;
}

bool GuildView::AtWarWith(std::uint32_t guild) const {
    return guild != 0 && std::any_of(wars_.wars.begin(), wars_.wars.end(), [&](const proto::GuildWarRecord& w) { return w.guild == guild; });
}

bool GuildWarTermsValid(const proto::GuildWarTerms& t) {
    if (t.victory >= kGuildWarVictoryPoints.size()) return false;
    if (t.unlimited != 0) return true;
    if (t.days > 30 || t.hours > 23 || t.minutes > 50) return false;
    return GuildWarSeconds(t) > 0 || t.victory != 0;
}

std::int64_t GuildWarSeconds(const proto::GuildWarTerms& t) {
    if (t.unlimited != 0) return 0;
    return std::int64_t(t.days) * 86'400 + std::int64_t(t.hours) * 3'600 + std::int64_t(t.minutes) * 60;
}

std::uint32_t GuildView::HeadColour(std::uint32_t guild, std::uint32_t unionId) const {
    if (guild != 0 && guild == guild_.id) return 0xFFFEAD2E;
    if (AtWarWith(guild)) return 0xFFFF0000; // at war
    if (unionId != 0 && unionId == union_.id) return 0xFFA0FE68;
    return 0xFF66B9FF;
}

} // namespace sr::game
