#pragma once

// The original server's quest script (Script/VIETNAM/Quest.sct: compiled Lua 5.1) read as
// data: never run. Each quest is a function of calls with constant arguments (LuaGetQuestID, LuaSetStartCodition,
// LuaSetMissionData, ...); the reader follows each function's instructions without branching, keeps what each
// register was last given (a constant, a global's name, a call's result) and reads the calls' arguments from them,
// then turns a quest's calls into our definition (game::QuestDef). The rewards it reads are the script's own; the
// import takes the database's (_RefQuestReward) over them.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sr/game/Quest.h"

namespace sr::server {

// One call a function makes, its arguments as written: numbers, strings, globals ("$NAME") and results ("=NAME").
struct LuaCall {
    struct Value {
        enum class Kind : std::uint8_t { Nil, Boolean, Number, String, Global, Result, Unknown } kind = Kind::Unknown;
        double number = 0;
        std::string text; // a string, a global's or a called function's name
        bool IsString() const { return kind == Kind::String; }
        bool IsNumber() const { return kind == Kind::Number; }
    };
    std::string function; // the global called
    std::vector<Value> args;
};
struct LuaFunction {
    std::string name; // the global it is stored in (empty: none)
    std::vector<LuaCall> calls;
};

// Every function of a Lua 5.1 chunk (little-endian, 4-byte int, size_t and instruction, 8-byte numbers) with its calls.
bool ReadLuaCalls(std::span<const std::uint8_t> chunk, std::vector<LuaFunction>& out, std::string& error);

// The quests the script defines (a quest's function calls LuaGetQuestID and LuaSetStartCodition); `notes` gets what
// was read but not understood.
bool DecodeQuestScript(std::span<const std::uint8_t> chunk, std::vector<game::QuestDef>& out, std::vector<std::string>& notes,
                       std::string& error);

// The quests the original codes in its game server (OriginalQuests.cpp: our definitions of them, read from its classes),
// without the numbers, levels, texts and rewards the database gives at the import.
bool DecodeCodedQuests(std::vector<game::QuestDef>& out, std::string& error);
// The families' blocks of them, each in a file of its own (the same format; read by DecodeCodedQuests): Samarkand,
// Donwhang's warehouse, the Tomb of Qin Shi and Roc Mountain (OriginalQuestsAsia.cpp); Constantinople's East Europe
// chain and the hwan army tests (OriginalQuestsEurope.cpp).
std::string_view CodedQuestsAsia();
std::string_view CodedQuestsEurope();
std::string_view CodedQuestsTowns(); // OriginalQuestsTowns.cpp: the ones found missing

} // namespace sr::server
