#pragma once

// What a player's place and state allow: the kinds of place the original's database names
// (_RefGame_World's worlds and their regions, _RefInstance_World_Region), and the rules that read them. Free PvP is the
// first to ask (the original refuses it in a fortress's world during its war,
// in a timed event's world, in the battle arena, while riding, in a job's suit, while dealing); what comes later (the
// arenas, the event maps, the Forgotten World, riding) only has to mark its place or its state.

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace sr::game {

enum class PlaceKind : std::uint8_t {
    Field,          // INS_DEFAULT: the open world
    Fortress,       // INS_FORT_*: a fortress's area
    Arena,          // INS_ARENA_*: the battle arena's maps
    EventWorld,     // INS_EVENT_*: an event's map (the random match, ...)
    ForgottenWorld, // INS_GOD_*: the Forgotten World's levels
    Instance,       // any other instance world (quests' rooms, temples, Pharaoh's tomb, ...)
};

// A world's kind by its code name (_RefGame_World.WorldCodeName128).
PlaceKind PlaceKindOfWorld(std::string_view codename);

// The regions of the worlds that are not the open field; a region in none of them is the field.
class PlaceTable {
public:
    void Add(std::uint16_t region, PlaceKind kind);
    PlaceKind Of(std::uint16_t region) const;
    std::size_t Size() const { return regions_.size(); }

private:
    std::unordered_map<std::uint16_t, PlaceKind> regions_;
};

// A player as the rules see it.
struct ActivityState {
    PlaceKind place = PlaceKind::Field;
    bool fortressWar = false; // its place is a fortress's area while that fortress is at war
    bool alive = true;
    bool dealing = false;     // at a stall (its own or visiting), in an exchange
    bool casting = false;     // a skill's cast under way
    bool riding = false;      // on a ride or a transport
    bool jobSuit = false;     // a job's suit worn
};

// Why something is refused (None: it is allowed).
enum class Refusal : std::uint8_t { None, Dead, Dealing, Casting, Riding, JobSuit, FortressWar, Arena, EventWorld, ForgottenWorld };

// Free PvP's team asked for: the original's own refusals, and (ours, the user's call) the Forgotten World, whose
// fights are the monsters'.
Refusal FreePvpRefusal(const ActivityState& s);
const char* RefusalName(Refusal r);

} // namespace sr::game
