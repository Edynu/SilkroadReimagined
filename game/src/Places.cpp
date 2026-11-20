#include "sr/game/Places.h"

namespace sr::game {

PlaceKind PlaceKindOfWorld(std::string_view codename) {
    if (codename == "INS_DEFAULT") return PlaceKind::Field;
    if (codename.starts_with("INS_FORT_")) return PlaceKind::Fortress;
    if (codename.starts_with("INS_ARENA_")) return PlaceKind::Arena;
    if (codename.starts_with("INS_EVENT_")) return PlaceKind::EventWorld;
    if (codename.starts_with("INS_GOD_")) return PlaceKind::ForgottenWorld;
    return PlaceKind::Instance;
}

void PlaceTable::Add(std::uint16_t region, PlaceKind kind) {
    if (kind != PlaceKind::Field) regions_[region] = kind;
}

PlaceKind PlaceTable::Of(std::uint16_t region) const {
    const auto it = regions_.find(region);
    return it != regions_.end() ? it->second : PlaceKind::Field;
}

Refusal FreePvpRefusal(const ActivityState& s) {
    if (!s.alive) return Refusal::Dead;
    if (s.jobSuit) return Refusal::JobSuit;
    if (s.dealing) return Refusal::Dealing;
    if (s.casting) return Refusal::Casting;
    if (s.riding) return Refusal::Riding;
    if (s.fortressWar) return Refusal::FortressWar;
    if (s.place == PlaceKind::Arena) return Refusal::Arena;
    if (s.place == PlaceKind::EventWorld) return Refusal::EventWorld;
    if (s.place == PlaceKind::ForgottenWorld) return Refusal::ForgottenWorld;
    return Refusal::None;
}

const char* RefusalName(Refusal r) {
    switch (r) {
    case Refusal::None: return "allowed";
    case Refusal::Dead: return "dead";
    case Refusal::Dealing: return "dealing";
    case Refusal::Casting: return "casting";
    case Refusal::Riding: return "riding";
    case Refusal::JobSuit: return "in a job's suit";
    case Refusal::FortressWar: return "in a fortress's area at war";
    case Refusal::Arena: return "in the battle arena";
    case Refusal::EventWorld: return "in an event's map";
    case Refusal::ForgottenWorld: return "in the Forgotten World";
    }
    return "?";
}

} // namespace sr::game
