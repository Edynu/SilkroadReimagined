#include "sr/server/QuestScript.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>

namespace sr::server {

namespace {

using Value = LuaCall::Value;
using Kind = Value::Kind;

// --- The chunk ---------------------------------------------------------------------------------------------

struct Proto {
    std::vector<std::uint32_t> code;
    std::vector<Value> constants;
    std::vector<Proto> protos;
    std::uint8_t upvalues = 0;
};

class ChunkReader {
public:
    explicit ChunkReader(std::span<const std::uint8_t> d) : d_(d) {}
    bool Header(std::string& error) {
        static constexpr std::uint8_t kHeader[] = {0x1B, 'L', 'u', 'a', 0x51, 0, 1, 4, 4, 4, 8, 0};
        if (d_.size() < sizeof(kHeader) || std::memcmp(d_.data(), kHeader, sizeof(kHeader)) != 0) {
            error = "not a Lua 5.1 chunk of 4-byte integers and 8-byte numbers";
            return false;
        }
        at_ = sizeof(kHeader);
        return true;
    }
    bool Function(Proto& p, int depth = 0) {
        if (depth > 64) return false;
        std::string ignored;
        std::uint32_t n = 0;
        if (!String(ignored) || !Skip(8) || !Byte(p.upvalues) || !Skip(3)) return false; // lines; params, vararg, stack
        if (!U32(n) || n > Left() / 4) return false;
        p.code.resize(n);
        for (std::uint32_t& i : p.code) U32(i);
        if (!U32(n)) return false;
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint8_t type = 0;
            if (!Byte(type)) return false;
            Value v;
            if (type == 0) {
                v.kind = Kind::Nil;
            } else if (type == 1) {
                std::uint8_t b = 0;
                if (!Byte(b)) return false;
                v.kind = Kind::Boolean;
                v.number = b;
            } else if (type == 3) {
                if (Left() < 8) return false;
                std::memcpy(&v.number, d_.data() + at_, 8);
                at_ += 8;
                v.kind = Kind::Number;
            } else if (type == 4) {
                if (!String(v.text)) return false;
                v.kind = Kind::String;
            } else {
                return false;
            }
            p.constants.push_back(std::move(v));
        }
        if (!U32(n) || n > Left()) return false;
        p.protos.resize(n);
        for (Proto& child : p.protos)
            if (!Function(child, depth + 1)) return false;
        if (!U32(n) || !Skip(std::size_t(n) * 4)) return false; // line info
        if (!U32(n)) return false;
        for (std::uint32_t k = 0; k < n; ++k)
            if (!String(ignored) || !Skip(8)) return false; // locals
        if (!U32(n)) return false;
        for (std::uint32_t k = 0; k < n; ++k)
            if (!String(ignored)) return false; // upvalue names
        return true;
    }
    bool AtEnd() const { return at_ == d_.size(); }

private:
    std::size_t Left() const { return d_.size() - at_; }
    bool Skip(std::size_t n) {
        if (Left() < n) return false;
        at_ += n;
        return true;
    }
    bool Byte(std::uint8_t& b) {
        if (Left() < 1) return false;
        b = d_[at_++];
        return true;
    }
    bool U32(std::uint32_t& v) {
        if (Left() < 4) return false;
        std::memcpy(&v, d_.data() + at_, 4);
        at_ += 4;
        return true;
    }
    // The script's strings are the original's code page; its names are ASCII, kept as bytes.
    bool String(std::string& out) {
        std::uint32_t n = 0;
        if (!U32(n) || n > Left()) return false;
        out.assign(reinterpret_cast<const char*>(d_.data() + at_), n == 0 ? 0 : n - 1);
        at_ += n;
        return true;
    }
    std::span<const std::uint8_t> d_;
    std::size_t at_ = 0;
};

// --- The calls ---------------------------------------------------------------------------------------------

// Lua 5.1's opcodes that are read; every other one that writes its A register makes it unknown.
enum Op : std::uint32_t {
    kMove = 0, kLoadK = 1, kLoadBool = 2, kLoadNil = 3, kGetGlobal = 5, kSetGlobal = 7, kSelf = 11, kJmp = 22, kEq = 23, kLt = 24,
    kLe = 25, kTest = 26, kCall = 28, kTailCall = 29, kReturn = 30, kForLoop = 31, kForPrep = 32, kTForLoop = 33, kSetList = 34,
    kClose = 35, kClosure = 36, kSetTable = 9, kSetUpval = 8,
};

void Walk(const Proto& p, std::string name, std::vector<LuaFunction>& out) {
    struct Reg {
        Value v;
        int closure = -1;
    };
    std::map<std::uint32_t, Reg> reg;
    std::vector<std::string> childNames(p.protos.size());
    LuaFunction f;
    f.name = std::move(name);
    const auto constant = [&](std::uint32_t k) { return k < p.constants.size() ? p.constants[k] : Value{}; };
    const auto unknown = [&](std::uint32_t a) { reg[a] = Reg{}; };
    for (std::size_t pc = 0; pc < p.code.size(); ++pc) {
        const std::uint32_t i = p.code[pc];
        const std::uint32_t op = i & 0x3F, a = (i >> 6) & 0xFF, b = (i >> 23) & 0x1FF, bx = i >> 14;
        switch (op) {
        case kMove: reg[a] = reg[b]; break;
        case kLoadK: reg[a] = Reg{constant(bx)}; break;
        case kLoadBool: reg[a] = Reg{Value{Kind::Boolean, double(b != 0), {}}}; break;
        case kLoadNil:
            for (std::uint32_t r = a; r <= b; ++r) reg[r] = Reg{Value{Kind::Nil, 0, {}}};
            break;
        case kGetGlobal: reg[a] = Reg{Value{Kind::Global, 0, constant(bx).text}}; break;
        case kSetGlobal:
            if (const Reg& r = reg[a]; r.closure >= 0 && std::size_t(r.closure) < childNames.size()) childNames[std::size_t(r.closure)] = constant(bx).text;
            break;
        case kSelf:
            unknown(a);
            unknown(a + 1);
            break;
        case kCall:
        case kTailCall: {
            LuaCall call;
            if (const Reg& fn = reg[a]; fn.v.kind == Kind::Global) call.function = fn.v.text;
            for (std::uint32_t k = 1; b != 0 && k < b; ++k) call.args.push_back(reg[a + k].v);
            const std::string called = call.function;
            if (!called.empty()) f.calls.push_back(std::move(call));
            reg[a] = Reg{Value{Kind::Result, 0, called}};
            for (std::uint32_t k = a + 1; k < a + 256 && k < 512; ++k) reg.erase(k);
            break;
        }
        case kClosure:
            reg[a] = Reg{Value{}, int(bx)};
            if (bx < p.protos.size()) pc += p.protos[bx].upvalues; // the pseudo-instructions naming its upvalues
            break;
        case kJmp: case kEq: case kLt: case kLe: case kTest: case kReturn: case kSetList: case kClose: case kSetTable: case kSetUpval:
            break;
        case kForLoop: case kForPrep: case kTForLoop:
            for (std::uint32_t k = a; k < a + 4; ++k) unknown(k);
            break;
        default: unknown(a); break;
        }
    }
    out.push_back(std::move(f));
    for (std::size_t k = 0; k < p.protos.size(); ++k) Walk(p.protos[k], childNames[k], out);
}

// --- The quests --------------------------------------------------------------------------------------------

std::string Str(const std::vector<Value>& a, std::size_t i) { return i < a.size() && a[i].IsString() ? a[i].text : std::string(); }
std::string Global(const std::vector<Value>& a, std::size_t i) { return i < a.size() && a[i].kind == Kind::Global ? a[i].text : std::string(); }
double Num(const std::vector<Value>& a, std::size_t i) { return i < a.size() && a[i].IsNumber() ? a[i].number : 0.0; }
// A count as the original's callbacks take it: truncated (__ftol2: 1.5 is 1).
std::uint32_t Count(const std::vector<Value>& a, std::size_t i) {
    const double d = Num(a, i);
    return d <= 0 ? 0 : std::uint32_t(d);
}
game::QuestText Key(const std::string& s) { return s.empty() ? game::QuestText{} : game::TextKey(s); }

std::string JobOf(const std::string& global) {
    if (global == "OBJ_JOBSTATE_TRADER") return "trader";
    if (global == "OBJ_JOBSTATE_HUNTER") return "hunter";
    if (global == "OBJ_JOBSTATE_ROBBER") return "thief";
    return {};
}

bool ClassOf(const std::string& global, game::MonsterClass& out) {
    if (global == "MONSTER_CLASS_NORMAL") out = game::MonsterClass::Normal;
    else if (global == "MONSTER_CLASS_CHAMPION") out = game::MonsterClass::Champion;
    else if (global == "MONSTER_CLASS_GIANT") out = game::MonsterClass::Giant;
    else if (global == "MONSTER_CLASS_UNIQUE") out = game::MonsterClass::Unique;
    else return false;
    return true;
}

void AddUnique(std::vector<std::string>& list, const std::string& s) {
    if (!s.empty() && std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
}

// A goal from one of the mission calls (arguments: the quest, its index, the type, the line's key, then the type's own).
bool Goal(const LuaCall& call, game::QuestGoal& g, std::string& note) {
    const std::vector<Value>& a = call.args;
    g.text = Key(Str(a, 3));
    const std::string& fn = call.function;
    if (fn == "LuaSetMissionData") { // n targets, the targets, (a value), the class, the kills, (a value)
        g.kind = game::QuestGoalKind::Kill;
        const std::size_t n = Count(a, 4);
        for (std::size_t k = 0; k < n; ++k) g.monsters.push_back(Str(a, 5 + k));
        if (game::MonsterClass c; ClassOf(Global(a, 6 + n), c)) g.classes.push_back(c);
        g.count = Count(a, 7 + n);
        return true;
    }
    if (fn == "LuaSetMissionData_EX") { // n targets, the targets, m classes, the classes, the kills, two values
        g.kind = game::QuestGoalKind::Kill;
        const std::size_t n = Count(a, 4);
        for (std::size_t k = 0; k < n; ++k) g.monsters.push_back(Str(a, 5 + k));
        const std::size_t m = Count(a, 5 + n);
        for (std::size_t k = 0; k < m; ++k)
            if (game::MonsterClass c; ClassOf(Global(a, 6 + n + k), c)) g.classes.push_back(c);
        g.count = Count(a, 6 + n + m);
        return true;
    }
    if (fn == "LuaSetCollectionItemMissionData" || fn == "LuaSetCollectionItemMissionData_EX") {
        // n sources, the NPC, (a value), the items, the item, n pairs of monster and percent; _EX: six controls, the first
        // whether the goal is shown
        g.kind = game::QuestGoalKind::Drop;
        const std::size_t n = Count(a, 4);
        g.npc = Str(a, 5);
        g.count = Count(a, 7);
        g.item = Str(a, 8);
        for (std::size_t k = 0; k < n; ++k) g.sources.push_back({Str(a, 9 + 2 * k), Num(a, 10 + 2 * k)});
        if (fn.ends_with("_EX")) g.hidden = Num(a, 9 + 2 * n) == 0;
        return true;
    }
    if (fn == "LuaSetDeliverMissionData") { // the NPC, n items, n pairs of count and item, (a value), the NPC's talk
        g.kind = game::QuestGoalKind::Deliver;
        g.npc = Str(a, 4);
        const std::size_t n = Count(a, 5);
        for (std::size_t k = 0; k < n; ++k) g.items.push_back({Str(a, 7 + 2 * k), Count(a, 6 + 2 * k)});
        g.talk = Key(Str(a, 7 + 2 * n));
        return true;
    }
    if (fn == "LuaSetDeliverMissionData_EX") {
        // the NPC, (a value), n talks, the talks, then the item lists: its one use (QNO_CH_SMITH_1) hands over the item
        // given on starting the quest
        g.kind = game::QuestGoalKind::Deliver;
        g.npc = Str(a, 4);
        const std::size_t n = Count(a, 6);
        g.talk = Key(Str(a, 7));
        for (std::size_t k = 7 + n; k < a.size(); ++k)
            if (Str(a, k).starts_with("ITEM_")) {
                g.items.push_back({Str(a, k), 1});
                break;
            }
        return true;
    }
    if (fn == "LuaSetDialLogMissionData") { // n talks, the talks (the NPC is the one whose lines hold the talk)
        g.kind = game::QuestGoalKind::Talk;
        g.talk = Key(Str(a, 5));
        return true;
    }
    if (fn == "LuaSetKillPlayerMissionData") {
        // (a value), the NPC, the kills, n filters, the filters (MKP_*), the region, the level, n jobs, the jobs
        g.kind = game::QuestGoalKind::KillPlayer;
        g.npc = Str(a, 5);
        g.count = Count(a, 6);
        const std::size_t n = Count(a, 7);
        bool region = false, level = false, job = false;
        for (std::size_t k = 0; k < n; ++k) {
            const std::string f = Global(a, 8 + k);
            region |= f == "MKP_REGION";
            level |= f == "MKP_LEVEL";
            job |= f == "MKP_JOB";
            if (f != "MKP_REGION" && f != "MKP_LEVEL" && f != "MKP_JOB") note = "a kill-player filter not read: " + f;
        }
        std::size_t at = 8 + n;
        if (region) g.region = std::int32_t(Num(a, at++));
        if (level) g.minLevel = int(Num(a, at++));
        if (job) {
            const std::size_t m = Count(a, at++);
            for (std::size_t k = 0; k < m; ++k) g.jobs.push_back(JobOf(Global(a, at + k)));
        }
        return true;
    }
    if (fn == "LuaSetTalismanMissionData") { // the NPC, the theme, (a value)
        g.kind = game::QuestGoalKind::Collect;
        g.npc = Str(a, 4);
        g.theme = Str(a, 5);
        return true;
    }
    return false;
}

std::string SpecialOf(const std::string& event) {
    if (event == "CONVERSATION_SINGLE") return "conversation";
    if (event == "UNIQUE_DEAD") return "unique-dead";
    if (event == "STARTQUEST") return "start";
    if (event == "ENDQUEST") return "end";
    if (event == "ABORTQUEST") return "abort";
    if (event == "RESTARTQUEST") return "restart";
    std::string s = event;
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return c == '_' ? '-' : char(std::tolower(static_cast<unsigned char>(c))); });
    return s;
}

bool DefinitionOf(const LuaFunction& f, game::QuestDef& q, std::vector<std::string>& notes) {
    q = game::QuestDef{};
    q.repeat = 0; // the original's limit when none is set: none
    std::map<std::uint32_t, game::QuestGoal> goals;
    static const std::set<std::string> kIgnored = {"LuaSetMissionDataSize", "LuaSetMissionCompleteNum"};
    for (const LuaCall& call : f.calls) {
        const std::vector<Value>& a = call.args;
        const std::string& fn = call.function;
        if (fn == "LuaGetQuestID") {
            q.codename = Str(a, 0);
        } else if (fn == "LuaSetStartCodition") { // n flags, the flags (QSC_*), the quest's level and the lowest level
            // Only the lowest is a condition: the original refuses a player under it and has no upper bound (a higher
            // player takes the quest; the 1.188 client colours its row).
            if (a.size() >= 2) {
                q.maxLevel = 0;
                q.minLevel = int(Num(a, a.size() - 1));
            }
        } else if (fn == "LuaSetStartMethod") { // the method, n NPCs, the NPCs
            for (std::size_t k = 0; k < Count(a, 1); ++k) AddUnique(q.givers, Str(a, 2 + k));
        } else if (fn == "LuaQuestInsertNpc") {
            for (std::size_t k = 0; k < Count(a, 0); ++k) AddUnique(q.npcs, Str(a, 1 + k));
        } else if (fn == "LuaInsertMissionNPC") {
            AddUnique(q.npcs, Str(a, 0));
        } else if (fn == "LuaInsertMissionOrCompleteNpc") {
            q.finisher = Str(a, 0);
        } else if (fn == "LuaSetAchievementLimit") {
            q.repeat = Count(a, 0);
        } else if (fn == "LuaSetDivideType") {
            q.divide = Count(a, 0);
        } else if (fn == "LuaSetRealTime") {
            q.realClock = true;
        } else if (fn == "LuaSetDifferentString") {
            q.differentString = Count(a, 0);
        } else if (fn == "LuaSetDisableQuest") {
            q.disabled = Num(a, 0) != 0;
        } else if (fn == "LuaInsertDependancyQuests") {
            for (std::size_t k = 0; k < Count(a, 0); ++k) AddUnique(q.afterAll, Str(a, 1 + k));
        } else if (fn == "LuaInsertDependancyQuestsOneClear") {
            for (std::size_t k = 0; k < Count(a, 0); ++k) AddUnique(q.afterOne, Str(a, 1 + k));
        } else if (fn == "LuaSetJobTypeStartCondition") {
            AddUnique(q.jobs, JobOf(Global(a, 1)));
        } else if (fn == "LuaSetHwanLevel") {
            q.hwan = Count(a, 0);
        } else if (fn == "LuaSetRaceStartCodition") {
            const std::string c = Global(a, 1);
            q.race = c == "COUNTRY_CHINA" ? "china" : c == "COUNTRY_EUROPE" ? "europe" : std::string();
        } else if (fn == "LuaSetLimitTimeStartCondition") {
            q.timerSeconds = Count(a, 0);
        } else if (fn == "LuaSetDayOrNight") { // 1 night (hours 20 to 4), 2 day
            q.time = Num(a, 0) == 1 ? "night" : "day";
        } else if (fn == "LuaStartDayLimit") {
            q.quota = Count(a, 0);
        } else if (fn == "LuaInsertQuestFunctionStringList") { // n pairs of event and handler
            for (std::size_t k = 0; k < Count(a, 0); ++k) AddUnique(q.special, SpecialOf(Str(a, 1 + 2 * k)));
        } else if (fn == "InsertQuestMenuStringList") { // the NPC, n pairs of moment and text
            const std::string npc = Str(a, 0);
            for (std::size_t k = 0; k < Count(a, 1); ++k) {
                if (Str(a, 2 + 2 * k).empty()) continue; // a count beyond the pairs written (the original's slip, once)
                const std::string_view moment = game::QuestMomentOf(Str(a, 2 + 2 * k));
                if (moment.empty()) {
                    notes.push_back(std::format("{}: {} not read", q.codename, Str(a, 2 + 2 * k)));
                    continue;
                }
                q.talks.push_back({npc, std::string(moment), Key(Str(a, 3 + 2 * k))});
            }
        } else if (fn == "LuaSetAchievedItem") { // n items, experience, skill experience, gold, n (a value, item, count)
            q.reward.exp = std::uint64_t(Num(a, 1));
            q.reward.skillExp = std::uint64_t(Num(a, 2));
            q.reward.gold = std::uint64_t(Num(a, 3));
            for (std::size_t k = 0; k < Count(a, 0); ++k) q.reward.items.push_back({Str(a, 5 + 3 * k), Count(a, 6 + 3 * k)});
        } else if (fn == "LuaSetAchievedSkillPont") {
            q.reward.sp = Count(a, 0);
        } else if (fn == "LuaSetAchievedPoint") {
            q.reward.points.push_back({Count(a, 0), Str(a, 1)});
        } else if (kIgnored.contains(fn)) {
        } else if (fn.starts_with("LuaSet") && (fn.find("MissionData") != std::string::npos)) {
            game::QuestGoal g;
            std::string note;
            if (Goal(call, g, note)) {
                goals[Count(a, 1)] = std::move(g);
            } else {
                notes.push_back(std::format("{}: {} not read", q.codename, fn));
            }
            if (!note.empty()) notes.push_back(q.codename + ": " + note);
        } else {
            notes.push_back(std::format("{}: {} not read", q.codename, fn));
        }
    }
    for (auto& [index, g] : goals) {
        // A talk's NPC: the one whose lines hold the talk; a delivery made with the extended call hands over what the
        // quest gave on starting.
        if (g.kind == game::QuestGoalKind::Talk && g.npc.empty())
            for (const game::QuestTalk& t : q.talks)
                if (t.text == g.talk) g.npc = t.npc;
        q.goals.push_back(std::move(g));
    }
    // Who offers it: of the start's NPCs (LuaSetStartMethod names every NPC the quest involves), the ones with an
    // offer to say (BASIC_MENUSTRING_REQUEST_ACCEPT_QUEST); the first when none has (QNO_AM_FERRY1_2: the ferryman
    // offers, the general takes the delivery).
    {
        std::vector<std::string> offering;
        for (const std::string& npc : q.givers)
            for (const game::QuestTalk& t : q.talks)
                if (t.npc == npc && t.moment == "offer") AddUnique(offering, npc);
        if (offering.empty() && !q.givers.empty()) offering.push_back(q.givers.front());
        q.givers = std::move(offering);
    }
    // A delivery's items are given on starting the quest, the plain call's as the extended one's (the original's start
    // grants every deliver mission's items when its first count is above 0; then its MIDDLE_ACHIEVED is told,
    // "Received Blue Pearl from Toson", QNO_CA_THIEF_6_1's _07; no room: its INVENTORY_FULL, _05).
    for (const game::QuestGoal& g : q.goals)
        if (g.kind == game::QuestGoalKind::Deliver && !g.items.empty() && g.items.front().count > 0)
            for (const auto& i : g.items) q.startItems.push_back(i);
    if (q.minLevel <= 0) q.minLevel = 1;
    return !q.codename.empty();
}

} // namespace

bool ReadLuaCalls(std::span<const std::uint8_t> chunk, std::vector<LuaFunction>& out, std::string& error) {
    out.clear();
    ChunkReader reader(chunk);
    if (!reader.Header(error)) return false;
    Proto main;
    if (!reader.Function(main) || !reader.AtEnd()) {
        error = "the chunk does not read to its end as Lua 5.1 functions";
        return false;
    }
    Walk(main, {}, out);
    return true;
}

bool DecodeQuestScript(std::span<const std::uint8_t> chunk, std::vector<game::QuestDef>& out, std::vector<std::string>& notes,
                       std::string& error) {
    out.clear();
    std::vector<LuaFunction> functions;
    if (!ReadLuaCalls(chunk, functions, error)) return false;
    for (const LuaFunction& f : functions) {
        bool id = false, start = false;
        for (const LuaCall& c : f.calls) {
            id |= c.function == "LuaGetQuestID";
            start |= c.function == "LuaSetStartCodition";
        }
        if (!id || !start) continue;
        game::QuestDef q;
        if (DefinitionOf(f, q, notes)) out.push_back(std::move(q));
    }
    return true;
}

} // namespace sr::server
