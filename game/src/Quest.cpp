#include "sr/game/Quest.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace sr::game {

namespace {

constexpr std::pair<std::string_view, std::string_view> kMoments[] = {
    {"greeting", "BASIC_MENUSTRING_GREETING"},
    {"offer", "BASIC_MENUSTRING_REQUEST_ACCEPT_QUEST"},
    {"offer-again", "BASIC_MENUSTRING_ACCEPT_AFTER_ONE_CLEAR"},
    {"accepted", "BASIC_MENUSTRING_AT_ACCEPT"},
    {"denied", "BASIC_MENUSTRING_AT_DENY"},
    {"not-done", "BASIC_MENUSTRING_NOT_ACHIEVED"},
    {"middle", "BASIC_MENUSTRING_MIDDLE_ACHIEVED"},
    {"bag-full", "BASIC_MENUSTRING_INVENTORY_FULL"},
    {"done", "BASIC_MENUSTRING_ACHIEVED"},
    {"done-now", "BASIC_MENUSTRING_ACHIEVED_NOW"},
    {"count-limit", "BASIC_MENUSTRING_COUNT_LIMIT"},
    {"not-party", "BASIC_MENUSTRING_NOT_PARTY"},
    {"time-out", "BASIC_MENUSTRING_TIME_LIMIT_AUTO_ABORT"},
    {"missing", "OURS_NEED_MISSING"}, // ours: what is needed at the hand-in not held (the original's coded quests say it)
    {"finished", "OURS_FINISHED"},    // ours: said once it is paid (the original's script sends it: QNO_WC_FERRY4_1's _08)
    {"accepted-again", "OURS_ACCEPTED_AGAIN"}, // ours: `accepted` when taken again (the coded quests' talk handlers: QNO_WC_WAREHOUSE_W_2's _08)
    {"resupply", "OURS_RESUPPLY"},             // ours: a `resupply` given at the giver (QNO_CA_TREASURE_5's "You used all Seal Keys")
    {"resupply-spent", "OURS_RESUPPLY_SPENT"}, // ours: the day's `resupply` given already ("come again tomorrow": EASTEU_5's _15)
    {"trial", "OURS_TRIAL"},                   // ours: a `trial`'s NPC's words before its row (the hwan tests' "I will teleport you...")
};

constexpr std::pair<MonsterClass, std::string_view> kClasses[] = {
    {MonsterClass::Normal, "normal"}, {MonsterClass::Champion, "champion"}, {MonsterClass::Giant, "giant"}, {MonsterClass::Unique, "unique"}};

constexpr std::pair<QuestGoalKind, std::string_view> kGoals[] = {
    {QuestGoalKind::Kill, "kill"},          {QuestGoalKind::Drop, "drop"},       {QuestGoalKind::Deliver, "deliver"},
    {QuestGoalKind::Talk, "talk"},          {QuestGoalKind::KillPlayer, "kill-player"}, {QuestGoalKind::Collect, "collect"},
    {QuestGoalKind::Capture, "capture"},    {QuestGoalKind::Record, "record"}};

// --- Writing -------------------------------------------------------------------------------------------

std::string Text(const QuestText& t) {
    if (!t.key.empty()) return "@" + t.key;
    std::string out = "\"";
    for (char c : t.literal) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string Join(const std::vector<std::string>& words) {
    std::string out;
    for (const std::string& w : words) out += " " + w;
    return out;
}

std::string Number(double d) { return std::format("{}", d); }

class Writer {
public:
    std::string out;
    void Line(std::string_view s) {
        out += s;
        out += '\n';
    }
    void TextLine(std::string_view name, const QuestText& t) {
        if (!t.Empty()) Line(std::format("{} {}", name, Text(t)));
    }
};

// on use ITEM [status S...] [first-time] [one-at-a-time [TEXT]] [refuse TEXT] [cap-first] [at REGION X Y Z R]... [away TEXT]
// [cap N ITEM [TEXT]] [full TEXT] [wait S] [chance N of M] [give ITEM] [failed TEXT] [got TEXT]
std::string UseLine(const QuestDef::UseRule& u) {
    std::string s = "on use " + u.item;
    if (u.trap) s += " trap";
    if (!u.statuses.empty()) {
        s += " status";
        for (const std::uint8_t st : u.statuses) s += std::format(" {}", st);
    }
    if (u.firstTime) s += " first-time";
    if (u.oneAtATime) s += " one-at-a-time" + (u.busy.Empty() ? std::string() : " " + Text(u.busy));
    if (!u.refuse.Empty()) s += " refuse " + Text(u.refuse);
    if (u.capFirst) s += " cap-first";
    for (const QuestDef::UseRule::Place& p : u.at) {
        s += std::format(" at {} {} {} {} {}", static_cast<std::int16_t>(p.region), Number(p.x), Number(p.y), Number(p.z), Number(p.radius));
    }
    if (!u.away.Empty()) s += " away " + Text(u.away);
    if (u.cap != 0) s += std::format(" cap {} {}{}", u.cap, u.capItem, u.capped.Empty() ? std::string() : " " + Text(u.capped));
    if (!u.full.Empty()) s += " full " + Text(u.full);
    if (u.seconds != 0) s += std::format(" wait {}", u.seconds);
    if (u.outOf != 0) s += std::format(" chance {} of {}", u.chance, u.outOf);
    if (!u.give.empty()) s += " give " + u.give;
    if (!u.failed.Empty()) s += " failed " + Text(u.failed);
    if (!u.got.Empty()) s += " got " + Text(u.got);
    // The original's other handlers.
    if (!u.met.Empty()) s += " met " + Text(u.met);
    if (u.party) s += " party" + (u.noParty.Empty() ? std::string() : " " + Text(u.noParty));
    if (!u.targets.empty()) s += " target" + Join(u.targets);
    if (u.targetRange != 0) s += " within " + Number(u.targetRange);
    if (!u.noTarget.Empty()) s += " none " + Text(u.noTarget);
    if (!u.tooFar.Empty()) s += " far " + Text(u.tooFar);
    if (!u.wrongTarget.Empty()) s += " wrong " + Text(u.wrongTarget);
    if (!u.take.empty()) s += " take " + u.take;
    if (!u.orGive.empty()) s += " or-give " + u.orGive;
    if (u.limit != 0) s += std::format(" limit {} {}", u.limit, u.limitItem);
    if (!u.keptIn.empty()) {
        s += " kept-in";
        for (const std::uint8_t st : u.keptIn) s += std::format(" {}", st);
    }
    for (const QuestDef::UseRule::Spawn& sp : u.spawns) {
        s += std::format(" spawn {} {} radius {} {}", sp.monster, sp.count, Number(sp.near), Number(sp.far));
        if (sp.seconds != 0) s += std::format(" life {}", sp.seconds);
    }
    if (u.capture) {
        const QuestDef::UseRule::Capture& c = *u.capture;
        if (!c.outcomes.empty()) s += " capture-outcomes";
        else if (!c.item.empty()) s += std::format(" capture-item {} minutes {} roll {} step {}", c.item, c.minutes, c.roll, c.step);
        else s += std::format(" capture {} minutes {} roll {} step {}", c.cos, c.minutes, c.roll, c.step);
        for (const QuestDef::UseRule::Capture::Outcome& o : c.outcomes) s += std::format(" outcome {} {} {}", o.item, o.below, Text(o.said));
        if (c.stays) s += " stays";
        if (!c.raise.empty()) s += std::format(" raise {} {} {}", c.raise, Number(c.raiseNear), Number(c.raiseFar));
        if (c.diedMinutes != 0) s += std::format(" died-wait {}", c.diedMinutes);
        if (c.below) s += " below";
        if (!c.already.Empty()) s += " already " + Text(c.already);
        for (const auto& [minutes, text] : c.warnings) s += std::format(" warn {} {}", minutes, Text(text));
        if (!c.ran.Empty()) s += " ran " + Text(c.ran);
        if (!c.ownerDied.Empty()) s += " owner-died " + Text(c.ownerDied);
        if (!c.cosDied.Empty()) s += " cos-died " + Text(c.cosDied);
    }
    for (const QuestDef::UseRule::Stage& st : u.stages) {
        s += std::format(" stage {} {} {} {} {} {} {}", static_cast<std::int16_t>(st.at.region), Number(st.at.x), Number(st.at.y), Number(st.at.z),
                         Number(st.at.radius), st.seconds, st.chance);
    }
    for (const QuestDef::UseRule::Site& site : u.sites) {
        s += std::format(" site {} {} {} {} {} {} {} radius {} {}", static_cast<std::int16_t>(site.at.region), Number(site.at.x), Number(site.at.y),
                         Number(site.at.z), Number(site.at.radius), site.item, site.spawn.monster, Number(site.spawn.near), Number(site.spawn.far));
        if (site.spawn.seconds != 0) s += std::format(" life {}", site.spawn.seconds);
    }
    if (!u.need.empty()) s += " need" + Join(u.need);
    if (u.room) s += " room";
    if (u.placeFirst) s += " place-first";
    if (u.instance) {
        const QuestDef::UseRule::World& w = *u.instance;
        s += " instance " + w.world;
        if (!w.unique.empty()) s += " unique " + w.unique;
        s += std::format(" after {} minutes {}", w.after, w.minutes);
        if (!w.notMaster.Empty()) s += " master " + Text(w.notMaster);
        if (!w.transport.Empty()) s += " transport " + Text(w.transport);
        for (const QuestText& t : w.comes) s += " comes " + Text(t);
        if (!w.begun.Empty()) s += " begun " + Text(w.begun);
        for (const auto& [minutes, text] : w.warnings) s += std::format(" warn {} {}", minutes, Text(text));
        if (!w.slain.Empty()) s += " slain " + Text(w.slain);
        if (!w.over.Empty()) s += " over " + Text(w.over);
        for (const QuestDef::UseRule::World::General& g : w.generals) {
            s += " general " + g.monster;
            if (!g.says.Empty()) s += " says " + Text(g.says);
            if (!g.dies.Empty()) s += " dies " + Text(g.dies);
            if (!g.last.Empty()) s += " last " + Text(g.last);
        }
        if (!w.generals.empty()) s += std::format(" next {}", w.next);
    }
    return s;
}

// trial WORLD at NPC row TEXT start REGION X Y Z [refuse TEXT] [no-job] after S boss MOB REGION X Y Z radius A B minutes M
// [warn M TEXT]... [adds MOB REGION X Y Z radius A B every S] [pass-on-time] [passed TEXT] [lost TEXT] [berserk] [take ITEM]
// [give ITEM] [failed TEXT] [out S]
std::string TrialLine(const QuestDef::Trial& t) {
    const auto place = [](const QuestDef::UseRule::Place& p) {
        return std::format("{} {} {} {}", static_cast<std::int16_t>(p.region), Number(p.x), Number(p.y), Number(p.z));
    };
    std::string s = std::format("trial {} at {} row {} start {}", t.world, t.npc, Text(t.row), place(t.start));
    if (!t.refuse.Empty()) s += " refuse " + Text(t.refuse);
    if (t.noJob) s += " no-job";
    s += std::format(" after {} boss {} {} radius {} {} minutes {}", t.after, t.boss, place(t.bossAt), Number(t.near), Number(t.far), t.minutes);
    for (const auto& [minutes, text] : t.warnings) s += std::format(" warn {} {}", minutes, Text(text));
    if (t.adds) s += std::format(" adds {} {} radius {} {} every {}", t.adds->monster, place(t.adds->at), Number(t.adds->near), Number(t.adds->far), t.adds->every);
    if (t.passOnTime) s += " pass-on-time";
    if (!t.passed.Empty()) s += " passed " + Text(t.passed);
    if (!t.lost.Empty()) s += " lost " + Text(t.lost);
    if (t.berserk) s += " berserk";
    if (!t.take.empty()) s += " take " + t.take;
    if (!t.give.empty()) s += " give " + t.give;
    if (!t.failed.Empty()) s += " failed " + Text(t.failed);
    if (t.out != 0) s += std::format(" out {}", t.out);
    return s;
}

std::string GoalLine(const QuestGoal& g) {
    std::string s = std::format("goal {}", QuestGoalName(g.kind));
    switch (g.kind) {
    case QuestGoalKind::Kill:
        s += std::format(" {}{}", g.count, Join(g.monsters));
        if (!g.classes.empty()) {
            s += " class";
            for (MonsterClass c : g.classes)
                for (const auto& [k, name] : kClasses)
                    if (k == c) s += " " + std::string(name);
        }
        if (g.berserk) s += " berserk";
        break;
    case QuestGoalKind::Drop:
        s += std::format(" {} {}{}", g.count, g.item, g.sources.empty() ? "" : " from");
        for (const QuestGoal::Source& src : g.sources) s += std::format(" {} {}%{}", src.monster, Number(src.percent), src.byLevel ? " by-level" : "");
        if (!g.npc.empty()) s += " at " + g.npc;
        if (g.apiece) {
            s += " apiece";
            if (g.apieceGold != 0) s += std::format(" gold {}", g.apieceGold);
            if (g.apieceExp != 0) s += std::format(" exp {}", g.apieceExp);
            if (g.apieceSp != 0) s += std::format(" sp {}", g.apieceSp);
        }
        break;
    case QuestGoalKind::Deliver:
        s += " " + g.npc;
        for (const QuestGoal::Items& i : g.items) s += std::format(" {} {}", i.item, i.count);
        for (const QuestGoal::Items& i : g.received) s += std::format(" get {} {}", i.item, i.count);
        if (!g.talk.Empty()) s += " talk " + Text(g.talk);
        break;
    case QuestGoalKind::Talk:
        s += " " + g.npc;
        for (const std::string& other : g.alsoAt) s += " " + other;
        if (!g.talk.Empty()) s += " talk " + Text(g.talk);
        break;
    case QuestGoalKind::KillPlayer:
        s += std::format(" {}", g.count);
        if (g.minLevel > 0) s += std::format(" level {}", g.minLevel);
        if (g.region != 0) s += std::format(" region {}", g.region);
        if (!g.jobs.empty()) s += " job" + Join(g.jobs);
        if (!g.npc.empty()) s += " at " + g.npc;
        break;
    case QuestGoalKind::Collect:
        s += " " + g.theme;
        if (!g.npc.empty()) s += " at " + g.npc;
        break;
    case QuestGoalKind::Record:
        for (std::size_t k = 0; k < g.records.size(); ++k) s += std::format("{} {} {}", k > 0 ? " or" : "", g.records[k].first, g.records[k].second);
        break;
    case QuestGoalKind::Capture:
        s += Join(g.monsters) + " at " + g.npc;
        break;
    }
    if (!g.text.Empty()) s += " text " + Text(g.text);
    if (g.hidden) s += " hidden";
    return s;
}

// --- Reading -------------------------------------------------------------------------------------------

struct Token {
    enum Kind { Word, Key, Literal } kind = Word;
    std::string value;
};

bool Tokenize(std::string_view line, std::vector<Token>& out, std::string& error) {
    out.clear();
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '#') break;
        if (c == '"') {
            Token t{Token::Literal, {}};
            ++i;
            bool closed = false;
            while (i < line.size()) {
                char d = line[i++];
                if (d == '"') {
                    closed = true;
                    break;
                }
                if (d == '\\' && i < line.size()) {
                    d = line[i++];
                    if (d == 'n') d = '\n';
                }
                t.value += d;
            }
            if (!closed) {
                error = "a text without its closing quote";
                return false;
            }
            out.push_back(std::move(t));
            continue;
        }
        std::size_t end = i;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r' && line[end] != '#') ++end;
        std::string_view word = line.substr(i, end - i);
        if (word.starts_with('@')) {
            out.push_back({Token::Key, std::string(word.substr(1))});
        } else {
            out.push_back({Token::Word, std::string(word)});
        }
        i = end;
    }
    return true;
}

class Reader {
public:
    Reader(const std::vector<Token>& t, std::string& error) : t_(t), error_(error) {}
    bool Done() const { return at_ >= t_.size(); }
    bool Is(std::string_view word) const { return !Done() && t_[at_].kind == Token::Word && t_[at_].value == word; }
    bool Take(std::string_view word) {
        if (!Is(word)) return false;
        ++at_;
        return true;
    }
    bool Word(std::string& out, std::string_view what) {
        if (Done() || t_[at_].kind != Token::Word) return Fail(std::format("{} expected", what));
        out = t_[at_++].value;
        return true;
    }
    template <typename T>
    bool Integer(T& out, std::string_view what) {
        std::string w;
        if (!Word(w, what)) return false;
        const auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), out);
        if (ec != std::errc() || p != w.data() + w.size()) return Fail(std::format("{}: '{}' is not a whole number", what, w));
        return true;
    }
    bool Percent(double& out, std::string_view what) {
        std::string w;
        if (!Word(w, what)) return false;
        if (w.ends_with('%')) w.pop_back();
        const auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), out);
        if (ec != std::errc() || p != w.data() + w.size()) return Fail(std::format("{}: '{}' is not a number", what, w));
        return true;
    }
    bool Real(float& out, std::string_view what) {
        std::string w;
        if (!Word(w, what)) return false;
        const auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), out);
        if (ec != std::errc() || p != w.data() + w.size()) return Fail(std::format("{}: '{}' is not a number", what, w));
        return true;
    }
    bool IsText() const { return !Done() && t_[at_].kind != Token::Word; }
    bool IsLower() const { return !Done() && t_[at_].kind == Token::Word && !t_[at_].value.empty() && std::islower(static_cast<unsigned char>(t_[at_].value[0])) != 0; }
    bool IsNumber() const {
        return !Done() && t_[at_].kind == Token::Word && !t_[at_].value.empty() && (std::isdigit(static_cast<unsigned char>(t_[at_].value[0])) != 0);
    }
    bool Text(QuestText& out, std::string_view what) {
        if (Done() || t_[at_].kind == Token::Word) return Fail(std::format("{}: a text expected (@KEY or \"words\")", what));
        const Token& t = t_[at_++];
        out = t.kind == Token::Key ? QuestText{t.value, {}} : QuestText{{}, t.value};
        return true;
    }
    // Codenames up to the next of `stops` (or the line's end).
    void Words(std::vector<std::string>& out, std::initializer_list<std::string_view> stops) {
        while (!Done() && t_[at_].kind == Token::Word) {
            for (std::string_view s : stops)
                if (t_[at_].value == s) return;
            out.push_back(t_[at_++].value);
        }
    }
    bool End() { return Done() || Fail(std::format("'{}' not understood here", t_[at_].value)); }
    bool Fail(std::string message) {
        error_ = std::move(message);
        return false;
    }

private:
    const std::vector<Token>& t_;
    std::size_t at_ = 0;
    std::string& error_;
};

bool ParseGoal(Reader& r, QuestGoal& g) {
    std::string kind;
    if (!r.Word(kind, "the goal's kind")) return false;
    bool known = false;
    for (const auto& [k, name] : kGoals)
        if (name == kind) {
            g.kind = k;
            known = true;
        }
    if (!known) return r.Fail(std::format("'{}' is no goal (kill, drop, deliver, talk, kill-player, collect, capture)", kind));
    switch (g.kind) {
    case QuestGoalKind::Kill:
        if (!r.Integer(g.count, "the kills")) return false;
        r.Words(g.monsters, {"class", "berserk", "text", "hidden"});
        if (g.monsters.empty()) return r.Fail("a kill names its monsters");
        if (r.Take("class")) {
            std::vector<std::string> names;
            r.Words(names, {"berserk", "text", "hidden"});
            for (const std::string& n : names) {
                bool found = false;
                for (const auto& [c, name] : kClasses)
                    if (name == n) {
                        g.classes.push_back(c);
                        found = true;
                    }
                if (!found) return r.Fail(std::format("'{}' is no monster class (normal, champion, giant, unique)", n));
            }
        }
        if (r.Take("berserk")) g.berserk = true;
        break;
    case QuestGoalKind::Drop:
        if (!r.Integer(g.count, "the items") || !r.Word(g.item, "the item") ) return false;
        // Without `from`: the item comes otherwise (an item's use, a kill handler, a capture), counted in the bag (the
        // original's mission kind 10: QNO_EU_EASTEU_1's Stable Filth from the holy water).
        if (r.Take("from")) {
            while (!r.Done() && !r.Is("at") && !r.Is("apiece") && !r.Is("text") && !r.Is("hidden")) {
                QuestGoal::Source s;
                if (!r.Word(s.monster, "a monster") || !r.Percent(s.percent, "its chance")) return false;
                if (r.Take("by-level")) s.byLevel = true;
                g.sources.push_back(std::move(s));
            }
            if (g.sources.empty()) return r.Fail("a drop names its monsters after 'from'");
        }
        if (r.Take("at") && !r.Word(g.npc, "the NPC")) return false;
        // apiece [gold G] [exp E] [sp S]: paid by the piece at the hand-in
        if (r.Take("apiece")) {
            g.apiece = true;
            for (;;) {
                if (r.Take("gold")) {
                    if (!r.Integer(g.apieceGold, "the gold a piece")) return false;
                } else if (r.Take("exp")) {
                    if (!r.Integer(g.apieceExp, "the experience a piece")) return false;
                } else if (r.Take("sp")) {
                    if (!r.Integer(g.apieceSp, "the skill experience a piece")) return false;
                } else {
                    break;
                }
            }
        }
        break;
    case QuestGoalKind::Deliver:
        if (!r.Word(g.npc, "the NPC")) return false;
        while (!r.Done() && !r.Is("talk") && !r.Is("text") && !r.Is("hidden")) {
            // get ITEM N: what the NPC hands back at the delivery
            const bool get = r.Take("get");
            QuestGoal::Items i;
            if (!r.Word(i.item, "an item") || !r.Integer(i.count, "its count")) return false;
            (get ? g.received : g.items).push_back(std::move(i));
        }
        if (r.Take("talk") && !r.Text(g.talk, "the talk")) return false;
        break;
    case QuestGoalKind::Talk:
        if (!r.Word(g.npc, "the NPC")) return false;
        while (!r.Done() && !r.Is("talk") && !r.Is("text") && !r.Is("hidden")) {
            std::string other;
            if (!r.Word(other, "an NPC")) return false;
            g.alsoAt.push_back(std::move(other));
        }
        if (r.Take("talk") && !r.Text(g.talk, "the talk")) return false;
        break;
    case QuestGoalKind::KillPlayer:
        if (!r.Integer(g.count, "the kills")) return false;
        for (;;) {
            if (r.Take("level")) {
                if (!r.Integer(g.minLevel, "the level")) return false;
            } else if (r.Take("region")) {
                if (!r.Integer(g.region, "the region")) return false;
            } else if (r.Take("job")) {
                r.Words(g.jobs, {"level", "region", "at", "text", "hidden"});
            } else if (r.Take("at")) {
                if (!r.Word(g.npc, "the NPC")) return false;
            } else {
                break;
            }
        }
        break;
    case QuestGoalKind::Collect:
        if (!r.Word(g.theme, "the theme")) return false;
        if (r.Take("at") && !r.Word(g.npc, "the NPC")) return false;
        break;
    case QuestGoalKind::Record:
        // goal record NAME N [or NAME N]...: its count the first's
        do {
            std::string name;
            int count = 0;
            if (!r.Word(name, "the record") || !r.Integer(count, "its count")) return false;
            g.records.emplace_back(std::move(name), std::max(count, 1));
        } while (r.Take("or"));
        g.count = std::uint32_t(g.records.front().second);
        break;
    case QuestGoalKind::Capture: {
        // goal capture COS at NPC: the COS a trap caught (the quest's `on use ... capture COS`) brought to the NPC.
        std::string cos;
        if (!r.Word(cos, "the caught COS") || !r.Take("at") || !r.Word(g.npc, "the NPC")) return r.Fail("'goal capture COS at NPC'");
        g.monsters = {std::move(cos)};
        break;
    }
    }
    if (r.Take("text") && !r.Text(g.text, "the goal's line")) return false;
    if (r.Take("hidden")) g.hidden = true;
    return r.End();
}

bool ParseReward(Reader& r, QuestReward& w) {
    std::string what;
    if (!r.Word(what, "the reward")) return false;
    if (what == "exp") return r.Integer(w.exp, "the experience") && r.End();
    if (what == "skill-exp") return r.Integer(w.skillExp, "the skill experience") && r.End();
    if (what == "sp") return r.Integer(w.sp, "the skill points") && r.End();
    if (what == "gold") return r.Integer(w.gold, "the gold") && r.End();
    if (what == "inventory") return r.Integer(w.inventory, "the places") && r.End();
    if (what == "hwan") return r.Integer(w.hwan, "the hwan level") && r.End();
    if (what == "choose") return r.Integer(w.choose, "how many") && r.End();
    if (what == "by-country") return (w.byCountry = true, r.End());
    if (what == "bound") return (w.bound = true, r.End());
    if (what == "hidden") return (w.shown = false, r.End());
    if (what == "item") {
        QuestReward::Item i;
        if (!r.Word(i.item, "the item") || !r.Integer(i.count, "its count")) return false;
        w.items.push_back(std::move(i));
        return r.End();
    }
    if (what == "points") {
        QuestReward::Points p;
        if (!r.Integer(p.amount, "the points") || !r.Word(p.name, "their name")) return false;
        w.points.push_back(std::move(p));
        return r.End();
    }
    return r.Fail(std::format("'{}' is no reward (exp, skill-exp, sp, gold, item, choose, by-country, hidden, inventory, hwan, points)", what));
}

bool ParseStart(Reader& r, QuestDef& q) {
    std::string what;
    if (!r.Word(what, "the condition")) return false;
    if (what == "level") {
        std::string range;
        if (!r.Word(range, "the levels")) return false;
        const std::size_t dash = range.find('-');
        const std::string_view a = std::string_view(range).substr(0, dash);
        const std::string_view b = dash == std::string::npos ? std::string_view{} : std::string_view(range).substr(dash + 1);
        auto num = [&](std::string_view s, int& out) {
            const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
            return ec == std::errc() && p == s.data() + s.size();
        };
        q.maxLevel = 0;
        if (!num(a, q.minLevel) || (!b.empty() && !num(b, q.maxLevel)))
            return r.Fail(std::format("'{}' is no level range (A, A-B, or A- for no upper bound)", range));
        if (dash == std::string::npos) q.maxLevel = q.minLevel;
        return r.End();
    }
    if (what == "after") {
        std::vector<std::string>* list = r.Take("all") ? &q.afterAll : r.Take("one") ? &q.afterOne : nullptr;
        if (list == nullptr) return r.Fail("after all ... or after one ...");
        r.Words(*list, {});
        // Q*N: finished N times (after all only)
        for (std::string& code : *list) {
            const std::size_t star = code.find('*');
            if (star == std::string::npos) continue;
            int n = 0;
            const std::string count = code.substr(star + 1);
            const auto [end, ec] = std::from_chars(count.data(), count.data() + count.size(), n);
            if (list != &q.afterAll || ec != std::errc{} || end != count.data() + count.size() || n < 1 || n > 0xFFFF)
                return r.Fail(std::format("'{}' is no QUEST*TIMES of 'after all'", code));
            code.resize(star);
            if (n > 1) q.afterTimes[code] = std::uint16_t(n);
        }
        return r.End();
    }
    if (what == "record") {
        QuestDef::RecordNeed need;
        do {
            std::string name;
            int count = 0;
            if (!r.Word(name, "the record") || !r.Integer(count, "its count")) return false;
            need.any.emplace_back(std::move(name), count);
        } while (r.Take("or"));
        q.records.push_back(std::move(need));
        return r.End();
    }
    if (what == "before") {
        r.Words(q.before, {});
        return q.before.empty() ? r.Fail("'before' names the quests not yet finished") : r.End();
    }
    if (what == "never") {
        r.Words(q.never, {});
        return q.never.empty() ? r.Fail("'never' names the quests never taken") : r.End();
    }
    if (what == "during") {
        r.Words(q.during, {});
        return q.during.empty() ? r.Fail("'during' names the quests under way") : r.End();
    }
    if (what == "party") return (q.party = true, r.End());
    if (what == "race") return r.Word(q.race, "the race") && r.End();
    if (what == "job") {
        r.Words(q.jobs, {});
        return r.End();
    }
    if (what == "hwan") return r.Integer(q.hwan, "the hwan level") && r.End();
    if (what == "time") return r.Word(q.time, "day or night") && r.End();
    if (what == "quota") return r.Integer(q.quota, "the starts") && r.End();
    if (what == "real-clock") return (q.realClock = true, r.End());
    if (what == "given") return (q.given = true, r.End());
    if (what == "premium") return (q.premium = true, r.End());
    if (what == "holding-one") {
        r.Words(q.startHoldingOne, {});
        return q.startHoldingOne.empty() ? r.Fail("'holding-one' names the items one of which is held") : r.End();
    }
    if (what == "holding") {
        r.Words(q.startHolding, {});
        return q.startHolding.empty() ? r.Fail("'holding' names the items held") : r.End();
    }
    return r.Fail(std::format("'{}' is no start condition (level, after, during, party, race, job, hwan, time, quota, real-clock, given, premium, holding)",
                              what));
}

// on use ITEM ... (UseLine's words, in any order).
bool ParseUse(Reader& r, QuestDef& q) {
    QuestDef::UseRule u;
    if (!r.Word(u.item, "the item used")) return false;
    while (!r.Done()) {
        std::string what;
        if (!r.Word(what, "a part of the use")) return false;
        bool ok = true;
        if (what == "status") {
            while (ok && r.IsNumber()) {
                int st = 0;
                ok = r.Integer(st, "a state");
                if (ok && st != 1 && st != 7 && st != 8) ok = r.Fail("a use's states are 1 (under way), 7 (progressed) and 8 (goals met)");
                if (ok) u.statuses.push_back(std::uint8_t(st));
            }
            if (ok && u.statuses.empty()) ok = r.Fail("'status' takes the states (1, 7, 8)");
        } else if (what == "first-time") {
            u.firstTime = true;
        } else if (what == "one-at-a-time") {
            u.oneAtATime = true;
            if (r.IsText()) ok = r.Text(u.busy, "what is said while it runs");
        } else if (what == "cap-first") {
            u.capFirst = true;
        } else if (what == "refuse") {
            ok = r.Text(u.refuse, "what is said when refused");
        } else if (what == "at") {
            QuestDef::UseRule::Place p;
            int region = 0;
            ok = r.Integer(region, "the region") && r.Real(p.x, "x") && r.Real(p.y, "y") && r.Real(p.z, "z") && r.Real(p.radius, "the radius");
            p.region = static_cast<std::uint16_t>(region);
            if (ok) u.at.push_back(p);
        } else if (what == "away") {
            ok = r.Text(u.away, "what is said elsewhere");
        } else if (what == "cap") {
            ok = r.Integer(u.cap, "the cap") && r.Word(u.capItem, "the item capped");
            if (ok && r.IsText()) ok = r.Text(u.capped, "what is said at the cap");
        } else if (what == "full") {
            ok = r.Text(u.full, "what is said with a full bag");
        } else if (what == "wait") {
            ok = r.Integer(u.seconds, "the seconds");
        } else if (what == "chance") {
            ok = r.Integer(u.chance, "the chance") && r.Take("of") && r.Integer(u.outOf, "out of what");
            if (ok && (u.outOf == 0 || u.chance > u.outOf)) ok = r.Fail("'chance N of M' takes 0 < M and N <= M");
        } else if (what == "give") {
            ok = r.Word(u.give, "the item given");
        } else if (what == "failed") {
            ok = r.Text(u.failed, "what is said when the roll loses");
        } else if (what == "got") {
            ok = r.Text(u.got, "what is said when given");
        } else if (what == "met") {
            ok = r.Text(u.met, "what is said with its goals met");
        } else if (what == "party") {
            u.party = true;
            if (r.IsText()) ok = r.Text(u.noParty, "what is said out of such a party");
        } else if (what == "target") {
            // The monsters' codenames up to the next part (the parts are lower case).
            while (!r.Done() && !r.IsText() && !r.IsLower()) {
                std::string m;
                ok = r.Word(m, "a monster");
                u.targets.push_back(std::move(m));
            }
            if (ok && u.targets.empty()) ok = r.Fail("'target' names the monsters it is used on");
        } else if (what == "within") {
            ok = r.Real(u.targetRange, "the target's reach");
        } else if (what == "none") {
            ok = r.Text(u.noTarget, "what is said with nothing selected");
        } else if (what == "far") {
            ok = r.Text(u.tooFar, "what is said when the target is too far");
        } else if (what == "wrong") {
            ok = r.Text(u.wrongTarget, "what is said on another monster");
        } else if (what == "take") {
            ok = r.Word(u.take, "the item taken");
        } else if (what == "or-give") {
            ok = r.Word(u.orGive, "the item given when the roll loses");
        } else if (what == "limit") {
            ok = r.Integer(u.limit, "the limit") && r.Word(u.limitItem, "the item limited");
        } else if (what == "kept-in") {
            while (ok && r.IsNumber()) {
                int st = 0;
                ok = r.Integer(st, "a state");
                if (ok) u.keptIn.push_back(std::uint8_t(st));
            }
            if (ok && u.keptIn.empty()) ok = r.Fail("'kept-in' takes the states");
        } else if (what == "spawn") {
            QuestDef::UseRule::Spawn sp;
            ok = r.Word(sp.monster, "the monster raised");
            if (ok && r.IsNumber()) ok = r.Integer(sp.count, "how many");
            if (ok && r.Take("radius")) ok = r.Real(sp.near, "the nearest radius") && r.Real(sp.far, "the farthest radius");
            if (ok && r.Take("life")) ok = r.Integer(sp.seconds, "its seconds");
            if (ok && (sp.count == 0 || sp.near > sp.far)) ok = r.Fail("'spawn MOB N radius A B' takes N > 0 and A <= B");
            if (ok) u.spawns.push_back(std::move(sp));
        } else if (what == "capture") {
            u.capture = QuestDef::UseRule::Capture{};
            ok = r.Word(u.capture->cos, "the caught COS");
        } else if (what == "capture-item") {
            u.capture = QuestDef::UseRule::Capture{};
            ok = r.Word(u.capture->item, "the caught item");
        } else if (what == "capture-outcomes") {
            u.capture = QuestDef::UseRule::Capture{};
        } else if (what == "outcome" || what == "stays") {
            if (!u.capture) {
                ok = r.Fail(std::format("'{}' belongs to a 'capture-outcomes'", what));
            } else if (what == "stays") {
                u.capture->stays = true;
            } else {
                QuestDef::UseRule::Capture::Outcome o;
                ok = r.Word(o.item, "the item it gives") && r.Integer(o.below, "the bound in 101") && r.Text(o.said, "what is said then");
                if (ok) u.capture->outcomes.push_back(std::move(o));
            }
        } else if (what == "trap") {
            u.trap = true;
        } else if (((what == "minutes" || what == "warn") && !u.instance) || what == "roll" || what == "step" || what == "below" || what == "already" || what == "ran" ||
                   what == "owner-died" || what == "cos-died" || what == "raise" || what == "died-wait") {
            if (!u.capture) {
                ok = r.Fail(std::format("'{}' belongs to a 'capture'", what));
            } else if (what == "raise") {
                ok = r.Word(u.capture->raise, "the monster raised each minute") && r.Real(u.capture->raiseNear, "the nearest radius") &&
                     r.Real(u.capture->raiseFar, "the farthest radius");
                if (ok && u.capture->raiseNear > u.capture->raiseFar) ok = r.Fail("'raise MOB A B' takes A <= B");
            } else if (what == "died-wait") {
                ok = r.Integer(u.capture->diedMinutes, "the minutes left after its owner's death");
            } else if (what == "minutes") {
                ok = r.Integer(u.capture->minutes, "the minutes");
            } else if (what == "roll") {
                ok = r.Integer(u.capture->roll, "the roll");
            } else if (what == "step") {
                ok = r.Integer(u.capture->step, "the step a level");
            } else if (what == "below") {
                u.capture->below = true;
            } else if (what == "already") {
                ok = r.Text(u.capture->already, "what is said with one caught");
            } else if (what == "warn") {
                std::pair<std::uint32_t, QuestText> w;
                ok = r.Integer(w.first, "the minutes left") && r.Text(w.second, "what is said then");
                if (ok) u.capture->warnings.push_back(std::move(w));
            } else if (what == "ran") {
                ok = r.Text(u.capture->ran, "what is said when its time is over");
            } else if (what == "owner-died") {
                ok = r.Text(u.capture->ownerDied, "what is said when its owner dies");
            } else {
                ok = r.Text(u.capture->cosDied, "what is said when the COS dies");
            }
        } else if (what == "stage") {
            QuestDef::UseRule::Stage st;
            int region = 0;
            ok = r.Integer(region, "the region") && r.Real(st.at.x, "x") && r.Real(st.at.y, "y") && r.Real(st.at.z, "z") && r.Real(st.at.radius, "the radius") &&
                 r.Integer(st.seconds, "the stage's wait") && r.Integer(st.chance, "the stage's chance in 101");
            st.at.region = static_cast<std::uint16_t>(region);
            if (ok && st.chance > 101) ok = r.Fail("a stage's chance is in 101");
            if (ok) u.stages.push_back(st);
        } else if (what == "site") {
            QuestDef::UseRule::Site site;
            int region = 0;
            ok = r.Integer(region, "the region") && r.Real(site.at.x, "x") && r.Real(site.at.y, "y") && r.Real(site.at.z, "z") &&
                 r.Real(site.at.radius, "the radius") && r.Word(site.item, "the site's item") && r.Word(site.spawn.monster, "the site's monster");
            site.at.region = static_cast<std::uint16_t>(region);
            if (ok && r.Take("radius")) ok = r.Real(site.spawn.near, "the nearest radius") && r.Real(site.spawn.far, "the farthest radius");
            if (ok && r.Take("life")) ok = r.Integer(site.spawn.seconds, "its seconds");
            if (ok && site.spawn.near > site.spawn.far) ok = r.Fail("a site's 'radius A B' takes A <= B");
            if (ok) u.sites.push_back(std::move(site));
        } else if (what == "need") {
            while (!r.Done() && !r.IsText() && !r.IsLower()) {
                std::string item;
                ok = r.Word(item, "an item needed");
                u.need.push_back(std::move(item));
            }
            if (ok && u.need.empty()) ok = r.Fail("'need' names the items held");
        } else if (what == "room") {
            u.room = true;
        } else if (what == "place-first") {
            u.placeFirst = true;
        } else if (what == "instance") {
            u.instance = QuestDef::UseRule::World{};
            ok = r.Word(u.instance->world, "the instance world");
        } else if (u.instance && (what == "general" || what == "says" || what == "dies" || what == "last" || what == "next")) {
            // The uniques in turn (World::generals): `general MOB [says TEXT] [dies TEXT] [last TEXT]`, `next S`.
            auto& generals = u.instance->generals;
            if (what == "general") {
                generals.emplace_back();
                ok = r.Word(generals.back().monster, "the general");
            } else if (what == "next") {
                ok = r.Integer(u.instance->next, "the seconds before the next one");
            } else if (generals.empty()) {
                ok = r.Fail(std::format("'{}' belongs to a 'general'", what));
            } else {
                ok = r.Text(what == "says" ? generals.back().says : what == "dies" ? generals.back().dies : generals.back().last, "what is said");
            }
        } else if (what == "unique" || what == "after" || what == "master" || what == "transport" || what == "comes" || what == "begun" ||
                   what == "slain" || what == "over" || ((what == "minutes" || what == "warn") && !u.capture && u.instance)) {
            if (!u.instance) {
                ok = r.Fail(std::format("'{}' belongs to an 'instance'", what));
            } else if (what == "unique") {
                ok = r.Word(u.instance->unique, "the instance's unique");
            } else if (what == "after") {
                ok = r.Integer(u.instance->after, "the seconds before it comes");
            } else if (what == "minutes") {
                ok = r.Integer(u.instance->minutes, "the fight's minutes");
            } else if (what == "warn") {
                std::pair<std::uint32_t, QuestText> w;
                ok = r.Integer(w.first, "the minutes left") && r.Text(w.second, "what is said then");
                if (ok) u.instance->warnings.push_back(std::move(w));
            } else {
                QuestText t;
                ok = r.Text(t, "what is said");
                if (!ok) {
                } else if (what == "master") {
                    u.instance->notMaster = std::move(t);
                } else if (what == "transport") {
                    u.instance->transport = std::move(t);
                } else if (what == "comes") {
                    u.instance->comes.push_back(std::move(t));
                } else if (what == "begun") {
                    u.instance->begun = std::move(t);
                } else if (what == "slain") {
                    u.instance->slain = std::move(t);
                } else {
                    u.instance->over = std::move(t);
                }
            }
        } else {
            ok = r.Fail(std::format("'{}' is no part of 'on use' (status, first-time, one-at-a-time, refuse, cap-first, at, away, cap, full, wait, "
                                    "chance, give, failed, got, met, party, target, within, none, far, wrong, take, or-give, limit, kept-in, spawn, "
                                    "capture, capture-item, trap, stage, site, need, room, place-first, instance)",
                                    what));
        }
        if (!ok) return false;
    }
    q.onUse.push_back(std::move(u));
    return true;
}

// trial WORLD ... (TrialLine's words, in any order).
bool ParseTrial(Reader& r, QuestDef& q) {
    QuestDef::Trial t;
    if (!r.Word(t.world, "the trial's world")) return false;
    const auto place = [&](QuestDef::UseRule::Place& p) {
        int region = 0;
        const bool ok = r.Integer(region, "the region") && r.Real(p.x, "x") && r.Real(p.y, "y") && r.Real(p.z, "z");
        p.region = static_cast<std::uint16_t>(region);
        return ok;
    };
    bool boss = false;
    while (!r.Done()) {
        std::string what;
        if (!r.Word(what, "a part of the trial")) return false;
        bool ok = true;
        if (what == "at") ok = r.Word(t.npc, "the trial's NPC");
        else if (what == "row") ok = r.Text(t.row, "the row's words");
        else if (what == "start") ok = place(t.start);
        else if (what == "refuse") ok = r.Text(t.refuse, "what is said when refused");
        else if (what == "no-job") t.noJob = true;
        else if (what == "after") ok = r.Integer(t.after, "the seconds before the boss");
        else if (what == "boss") ok = boss = r.Word(t.boss, "the boss") && place(t.bossAt) && r.Take("radius") && r.Real(t.near, "the nearest radius") &&
                                             r.Real(t.far, "the farthest radius");
        else if (what == "minutes") ok = r.Integer(t.minutes, "the trial's minutes");
        else if (what == "warn") {
            std::pair<std::uint32_t, QuestText> w;
            ok = r.Integer(w.first, "the minutes left") && r.Text(w.second, "what is said then");
            if (ok) t.warnings.push_back(std::move(w));
        } else if (what == "adds") {
            QuestDef::Trial::Adds a;
            ok = r.Word(a.monster, "the adds") && place(a.at) && r.Take("radius") && r.Real(a.near, "the nearest radius") &&
                 r.Real(a.far, "the farthest radius") && r.Take("every") && r.Integer(a.every, "the seconds between them");
            if (ok && a.every == 0) ok = r.Fail("'adds ... every S' takes S > 0");
            if (ok) t.adds = std::move(a);
        } else if (what == "pass-on-time") t.passOnTime = true;
        else if (what == "passed") ok = r.Text(t.passed, "what is said when passed");
        else if (what == "lost") ok = r.Text(t.lost, "what is said when lost");
        else if (what == "berserk") t.berserk = true;
        else if (what == "take") ok = r.Word(t.take, "the item taken");
        else if (what == "give") ok = r.Word(t.give, "the item given");
        else if (what == "failed") ok = r.Text(t.failed, "what is said when failed");
        else if (what == "out") ok = r.Integer(t.out, "the seconds before all go out");
        else ok = r.Fail(std::format("'{}' is no part of 'trial' (at, row, start, refuse, no-job, after, boss, minutes, warn, adds, pass-on-time, "
                                     "passed, lost, berserk, take, give, failed, out)",
                                     what));
        if (!ok) return false;
    }
    if (t.npc.empty() || !boss) return r.Fail("a trial names its NPC ('at NPC') and its boss ('boss MOB REGION X Y Z radius A B')");
    if (t.near > t.far) return r.Fail("the boss's 'radius A B' takes A <= B");
    q.trial = std::move(t);
    return true;
}

} // namespace

const char* QuestGoalName(QuestGoalKind k) {
    for (const auto& [kind, name] : kGoals)
        if (kind == k) return name.data();
    return "?";
}

std::string_view QuestMomentOf(std::string_view basicMenuString) {
    for (const auto& [moment, key] : kMoments)
        if (key == basicMenuString) return moment;
    return {};
}

std::string_view BasicMenuStringOf(std::string_view moment) {
    for (const auto& [m, key] : kMoments)
        if (m == moment) return key;
    return {};
}

std::string WriteQuest(const QuestDef& q) {
    Writer w;
    w.Line("quest " + q.codename);
    if (q.id != 0) w.Line(std::format("id {}", q.id));
    w.Line(std::format("level {}", q.level));
    w.TextLine("name", q.name);
    w.TextLine("reward-title", q.rewardTitle);
    w.TextLine("reward-text", q.rewardText);
    w.TextLine("guide-npc", q.guideNpc);
    w.TextLine("guide-condition", q.guideCondition);
    w.Line(q.maxLevel == 0 ? std::format("start level {}-", q.minLevel) : q.maxLevel == q.minLevel ? std::format("start level {}", q.minLevel)
                                                                                                     : std::format("start level {}-{}", q.minLevel, q.maxLevel));
    if (!q.afterAll.empty()) {
        std::string line = "start after all";
        for (const std::string& code : q.afterAll) {
            line += " " + code;
            if (const auto t = q.afterTimes.find(code); t != q.afterTimes.end()) line += std::format("*{}", t->second);
        }
        w.Line(line);
    }
    if (!q.never.empty()) w.Line("start never" + Join(q.never));
    if (!q.before.empty()) w.Line("start before" + Join(q.before));
    for (const QuestDef::RecordNeed& need : q.records) {
        std::string line = "start record";
        for (std::size_t k = 0; k < need.any.size(); ++k) line += std::format("{} {} {}", k > 0 ? " or" : "", need.any[k].first, need.any[k].second);
        w.Line(line);
    }
    if (!q.afterOne.empty()) w.Line("start after one" + Join(q.afterOne));
    if (!q.during.empty()) w.Line("start during" + Join(q.during));
    if (q.party) w.Line("start party");
    if (!q.race.empty()) w.Line("start race " + q.race);
    if (!q.jobs.empty()) w.Line("start job" + Join(q.jobs));
    if (q.hwan != 0) w.Line(std::format("start hwan {}", q.hwan));
    if (!q.time.empty()) w.Line("start time " + q.time);
    if (q.quota != 0) w.Line(std::format("start quota {}", q.quota));
    if (q.realClock) w.Line("start real-clock");
    if (q.given) w.Line("start given");
    if (q.premium) w.Line("start premium");
    if (!q.startHolding.empty()) w.Line("start holding" + Join(q.startHolding));
    if (!q.startHoldingOne.empty()) w.Line("start holding-one" + Join(q.startHoldingOne));
    if (q.timerSeconds != 0) w.Line(std::format("timer {}", q.timerSeconds));
    w.Line(q.repeat == 0 ? std::string("repeat any") : std::format("repeat {}", q.repeat));
    if (q.giveUpLocks) w.Line("give-up locks");
    if (q.giveUpKeepsItems) w.Line("give-up keeps-items");
    for (const std::string& code : q.finishResets) w.Line("finish-reset " + code);
    if (!q.givers.empty()) w.Line("giver" + Join(q.givers));
    if (!q.npcs.empty()) w.Line("npc" + Join(q.npcs));
    if (!q.finisher.empty()) w.Line("finish " + q.finisher);
    for (const QuestGoal::Items& i : q.startItems) w.Line(std::format("start-item {} {}", i.item, i.count));
    for (const QuestGoal::Items& i : q.needItems) w.Line(std::format("need item {} {}", i.item, i.count));
    if (q.needGold != 0) w.Line(std::format("need gold {}", q.needGold));
    for (const QuestDef::RecordNeed& need : q.needRecords) {
        std::string line = "need record";
        for (std::size_t k = 0; k < need.any.size(); ++k) line += std::format("{} {} {}", k > 0 ? " or" : "", need.any[k].first, need.any[k].second);
        w.Line(line);
    }
    if (q.divide != 0) w.Line(std::format("divide {}", q.divide));
    if (q.differentString != 0) w.Line(std::format("different-string {}", q.differentString));
    if (q.disabled) w.Line("disabled");
    if (!q.special.empty()) w.Line("special" + Join(q.special));
    for (const QuestDef::UniqueDead& u : q.onUniqueDead) {
        w.Line(std::format("on unique-dead {}{} take {} give {}", u.monster,
                           u.region ? std::format(" region {}", static_cast<std::int16_t>(*u.region)) : std::string(), u.take, u.give));
    }
    for (const QuestDef::KillRule& k : q.onKill) {
        std::string s = "on kill " + k.monster;
        if (!k.need.empty()) s += " need " + k.need;
        if (!k.take.empty()) s += " take " + k.take;
        if (!k.clear.empty()) s += " clear" + Join(k.clear);
        if (!k.give.empty()) s += " give " + k.give;
        if (k.cap != 0) s += std::format(" cap {}", k.cap);
        if (!k.full.Empty()) s += " full " + Text(k.full);
        w.Line(s);
    }
    for (const QuestGoal::Items& i : q.resupply) w.Line(std::format("resupply {} {}", i.item, i.count));
    if (!q.talkGame.empty()) w.Line("talk-game " + q.talkGame);
    for (const QuestDef::Raise& rz : q.raises) w.Line(std::format("raise {} every {} radius {} {}", rz.monster, rz.seconds, Number(rz.nearest), Number(rz.farthest)));
    if (q.resupplyDaily) w.Line(q.resupplyFromStart ? "resupply-daily start" : q.resupplyAfterFinish ? "resupply-daily after" : "resupply-daily");
    for (const std::string& item : q.payTake) w.Line("pay-take " + item);
    for (const QuestDef::UseRule& u : q.onUse) w.Line(UseLine(u));
    if (q.trial) w.Line(TrialLine(*q.trial));
    for (const QuestGoal& g : q.goals) w.Line(GoalLine(g));
    const QuestReward& r = q.reward;
    if (r.exp != 0) w.Line(std::format("reward exp {}", r.exp));
    if (r.skillExp != 0) w.Line(std::format("reward skill-exp {}", r.skillExp));
    if (r.sp != 0) w.Line(std::format("reward sp {}", r.sp));
    if (r.gold != 0) w.Line(std::format("reward gold {}", r.gold));
    if (r.inventory != 0) w.Line(std::format("reward inventory {}", r.inventory));
    if (r.hwan != 0) w.Line(std::format("reward hwan {}", r.hwan));
    for (const QuestReward::Item& i : r.items) w.Line(std::format("reward item {} {}", i.item, i.count));
    if (r.choose != 0) w.Line(std::format("reward choose {}", r.choose));
    if (r.byCountry) w.Line("reward by-country");
    if (!r.shown) w.Line("reward hidden");
    if (r.bound) w.Line("reward bound");
    for (const QuestReward::Points& p : r.points) w.Line(std::format("reward points {} {}", p.amount, p.name));
    for (const QuestDef::Tier& t : q.tiers) {
        w.Line(std::format("tier {} {} sp {}{}", t.item, t.count, t.sp, t.talk.Empty() ? std::string() : " talk " + Text(t.talk)));
    }
    for (const QuestTalk& t : q.talks) w.Line(std::format("talk {} {} {}", t.npc, t.moment, Text(t.text)));
    return w.out;
}

double QuestLevelScale(int playerLevel, int monsterLevel) {
    const int gap = monsterLevel - playerLevel;
    double scale = 1.0;
    if (gap > 10) scale = 1.0 - 0.02 * double(gap - 10);
    else if (gap <= -7) scale = 1.0 - 0.15 * double(-gap - 3);
    else if (gap < -3) scale = 0.9;
    return std::max(0.1, scale);
}

bool ParseQuest(std::string_view text, QuestDef& out, std::string& error) {
    out = QuestDef{};
    std::vector<Token> tokens;
    int number = 0;
    bool started = false;
    for (std::size_t start = 0; start <= text.size();) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++number;
        std::string why;
        const auto fail = [&](const std::string& message) {
            error = std::format("line {}: {}", number, message);
            return false;
        };
        if (!Tokenize(line, tokens, why)) return fail(why);
        if (tokens.empty()) continue;
        if (tokens[0].kind != Token::Word) return fail("a line starts with a word");
        const std::string head = tokens[0].value;
        const std::vector<Token> rest(tokens.begin() + 1, tokens.end());
        Reader r(rest, why);
        if (!started) {
            if (head != "quest") return fail("a quest starts with 'quest CODENAME'");
            if (!r.Word(out.codename, "the codename") || !r.End()) return fail(why);
            started = true;
            continue;
        }
        bool ok = true;
        if (head == "quest") {
            return fail("one quest a file");
        } else if (head == "id") {
            ok = r.Integer(out.id, "the id") && r.End();
        } else if (head == "level") {
            ok = r.Integer(out.level, "the level") && r.End();
        } else if (head == "name") {
            ok = r.Text(out.name, "the name") && r.End();
        } else if (head == "reward-title") {
            ok = r.Text(out.rewardTitle, "the reward's title") && r.End();
        } else if (head == "reward-text") {
            ok = r.Text(out.rewardText, "the reward's text") && r.End();
        } else if (head == "guide-npc") {
            ok = r.Text(out.guideNpc, "the guide's NPC line") && r.End();
        } else if (head == "guide-condition") {
            ok = r.Text(out.guideCondition, "the guide's condition line") && r.End();
        } else if (head == "start") {
            ok = ParseStart(r, out);
        } else if (head == "timer") {
            ok = r.Integer(out.timerSeconds, "the seconds") && r.End();
        } else if (head == "repeat") {
            ok = r.Take("any") ? (out.repeat = 0, r.End()) : r.Integer(out.repeat, "how often") && r.End();
        } else if (head == "give-up") {
            if (r.Take("locks")) ok = (out.giveUpLocks = true, r.End());
            else if (r.Take("keeps-items")) ok = (out.giveUpKeepsItems = true, r.End());
            else ok = r.Fail("give-up locks or give-up keeps-items");
        } else if (head == "finish-reset") {
            std::string code;
            ok = r.Word(code, "the quest cleared at the finish") && r.End();
            if (ok) out.finishResets.push_back(std::move(code));
        } else if (head == "giver") {
            r.Words(out.givers, {});
            ok = r.End();
        } else if (head == "npc") {
            r.Words(out.npcs, {});
            ok = r.End();
        } else if (head == "finish") {
            ok = r.Word(out.finisher, "the NPC") && r.End();
        } else if (head == "start-item") {
            QuestGoal::Items i;
            ok = r.Word(i.item, "the item") && r.Integer(i.count, "its count") && r.End();
            if (ok) out.startItems.push_back(std::move(i));
        } else if (head == "need") {
            if (r.Take("gold")) {
                ok = r.Integer(out.needGold, "the gold") && r.End();
            } else if (r.Take("item")) {
                QuestGoal::Items i;
                ok = r.Word(i.item, "the item") && r.Integer(i.count, "its count") && r.End();
                if (ok) out.needItems.push_back(std::move(i));
            } else if (r.Take("record")) {
                QuestDef::RecordNeed need;
                do {
                    std::string name;
                    int count = 0;
                    ok = r.Word(name, "the record") && r.Integer(count, "its count");
                    if (ok) need.any.emplace_back(std::move(name), count);
                } while (ok && r.Take("or"));
                ok = ok && r.End();
                if (ok) out.needRecords.push_back(std::move(need));
            } else {
                ok = false;
                why = "'need' takes 'item ITEM N', 'gold N' or 'record NAME N [or NAME N]...'";
            }
        } else if (head == "divide") {
            ok = r.Integer(out.divide, "the divide type") && r.End();
        } else if (head == "different-string") {
            ok = r.Integer(out.differentString, "the value") && r.End();
        } else if (head == "disabled") {
            out.disabled = true;
            ok = r.End();
        } else if (head == "on" && r.Take("use")) {
            ok = ParseUse(r, out);
        } else if (head == "on" && r.Take("kill")) {
            // on kill MOB [need ITEM] [take ITEM] [clear ITEM...] [give ITEM] [cap N] [full TEXT]
            QuestDef::KillRule k;
            ok = r.Word(k.monster, "the monster");
            while (ok && !r.Done()) {
                if (r.Take("need")) ok = r.Word(k.need, "the item needed");
                else if (r.Take("take")) ok = r.Word(k.take, "the item taken");
                else if (r.Take("clear")) r.Words(k.clear, {"need", "take", "give", "cap", "full"}), ok = !k.clear.empty();
                else if (r.Take("give")) ok = r.Word(k.give, "the item given");
                else if (r.Take("cap")) ok = r.Integer(k.cap, "the cap");
                else if (r.Take("full")) ok = r.Text(k.full, "the full bag's words");
                else ok = false;
            }
            if (!ok && why.empty()) why = "'on kill' takes 'MOB [need ITEM] [take ITEM] [clear ITEM...] [give ITEM] [cap N] [full TEXT]'";
            if (ok) out.onKill.push_back(std::move(k));
        } else if (head == "resupply") {
            QuestGoal::Items i;
            ok = r.Word(i.item, "the item") && r.Integer(i.count, "its count") && r.End();
            if (ok) out.resupply.push_back(std::move(i));
        } else if (head == "raise") {
            QuestDef::Raise rz;
            ok = r.Word(rz.monster, "the monster") && r.Take("every") && r.Integer(rz.seconds, "the seconds") && rz.seconds > 0;
            if (ok && r.Take("radius")) {
                int a = 0, b = 0;
                ok = r.Integer(a, "the nearest") && r.Integer(b, "the farthest") && a <= b;
                rz.nearest = float(a), rz.farthest = float(b);
            }
            ok = ok && r.End();
            if (!ok && why.empty()) why = "'raise MOB every S [radius A B]'";
            if (ok) out.raises.push_back(std::move(rz));
        } else if (head == "talk-game") {
            ok = r.Word(out.talkGame, "the game") && r.End();
        } else if (head == "resupply-daily") {
            out.resupplyDaily = true;
            out.resupplyFromStart = r.Take("start");
            out.resupplyAfterFinish = !out.resupplyFromStart && r.Take("after");
            ok = r.End();
        } else if (head == "pay-take") {
            std::string item;
            ok = r.Word(item, "the item taken at the hand-in") && r.End();
            if (ok) out.payTake.push_back(std::move(item));
        } else if (head == "trial") {
            ok = ParseTrial(r, out);
        } else if (head == "on") {
            // on unique-dead MOB [region R] take ITEM give ITEM
            QuestDef::UniqueDead u;
            int region = 0;
            ok = r.Take("unique-dead") && r.Word(u.monster, "the unique");
            if (ok && r.Take("region")) {
                ok = r.Integer(region, "the region");
                u.region = static_cast<std::uint16_t>(region);
            }
            ok = ok && r.Take("take") && r.Word(u.take, "the item taken") && r.Take("give") && r.Word(u.give, "the item given") && r.End();
            if (!ok && why.empty()) why = "'on' takes 'unique-dead MOB [region R] take ITEM give ITEM' or 'use ITEM ...'";
            if (ok) out.onUniqueDead.push_back(std::move(u));
        } else if (head == "special") {
            r.Words(out.special, {});
            ok = r.End();
        } else if (head == "goal") {
            QuestGoal g;
            ok = ParseGoal(r, g);
            if (ok) out.goals.push_back(std::move(g));
        } else if (head == "reward") {
            ok = ParseReward(r, out.reward);
        } else if (head == "tier") {
            // tier ITEM N sp M [talk TEXT]
            QuestDef::Tier t;
            ok = r.Word(t.item, "the item") && r.Integer(t.count, "its count") && r.Take("sp") && r.Integer(t.sp, "the skill points");
            if (ok && r.Take("talk")) ok = r.Text(t.talk, "the row's words");
            ok = ok && r.End();
            if (!ok && why.empty()) why = "'tier' takes 'ITEM N sp M [talk TEXT]'";
            if (ok) out.tiers.push_back(std::move(t));
        } else if (head == "talk") {
            QuestTalk t;
            ok = r.Word(t.npc, "the NPC") && r.Word(t.moment, "the moment") && r.Text(t.text, "what is said") && r.End();
            if (ok && BasicMenuStringOf(t.moment).empty()) {
                ok = false;
                why = std::format("'{}' is no moment (greeting, offer, offer-again, accepted, denied, not-done, middle, bag-full, done, "
                                  "done-now, count-limit, not-party, time-out, missing)",
                                  t.moment);
            }
            if (ok) out.talks.push_back(std::move(t));
        } else {
            return fail(std::format("'{}' is not understood", head));
        }
        if (!ok) return fail(why);
    }
    if (!started) {
        error = "no 'quest CODENAME' line";
        return false;
    }
    return true;
}

bool QuestFinishesAt(const QuestDef& q, std::string_view npc) {
    const auto named = [&](const std::vector<std::string>& list) { return std::find(list.begin(), list.end(), npc) != list.end(); };
    if (!q.tiers.empty() && named(q.givers)) return true;
    if (!q.finisher.empty()) return q.finisher == npc;
    return std::any_of(q.goals.begin(), q.goals.end(), [&](const QuestGoal& g) {
        if (g.kind != QuestGoalKind::Deliver && g.kind != QuestGoalKind::Talk && g.kind != QuestGoalKind::Capture) return false;
        return g.npc == npc || named(g.alsoAt);
    });
}

} // namespace sr::game
