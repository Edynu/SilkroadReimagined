#pragma once

// The client's picture of the world (C6c): what the
// server said is around, each walker carried along its last leg by the client itself (dead
// reckoning, as the original's CNavigationDeadreckon) in fixed steps (SimulationClock); drawing
// interpolates between the last two steps. Headless: the screens draw it.

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "sr/game/NavMesh.h"
#include "sr/game/TitleSession.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game {

class WorldView {
public:
    struct Entity {
        std::uint32_t id = 0;
        proto::CharacterSummary look;
        proto::WorldPosition position; // after the last step
        proto::WorldPosition previous; // before it
        float yaw = 0;
        bool moving = false;
        proto::WorldPosition legTo;
        float speed = 0;
        NavPosition nav;      // where it stands on the navigation (its cell and object)
        proto::WorldPosition legFrom;
        NavPosition legStart; // where the leg starts on the navigation (found at its first step)
        bool settled = false; // standing where it appeared: put down on the navigation once it was in (Step)
        // A swing heard while it still walked (the server stops it and swings in one tick; the stop's
        // leg can come after the swing): whom it turns to once it stands.
        std::uint32_t turnTo = 0;
        // A teleport skill's dash running (EntityBlink): its end, where the server's stand there changes nothing.
        bool dashing = false;
        proto::WorldPosition dashTo;
        // Which look it has: a number never given twice by this view (its appearance and each change of what it wears
        // take a new one, Clear never starts it again), so a screen's cache by entity and look version can never hand an
        // entity another's look, even when a new world (another game server) gives an id a new owner.
        std::uint32_t lookVersion = 0;
        proto::EntityKind kind = proto::EntityKind::Character;
        std::uint32_t amount = 0; // a ground item's count or gold; a skill object's owner
        std::uint32_t owner = 0;  // a ground item kept for one player: his entity (0 anyone's)
        std::uint8_t plus = 0;
        std::uint8_t rarity = 0;  // a monster's kind (EntityAppeared)
        bool fresh = false;       // a ground item seen falling (EntityAppeared::fresh)
    };

    // The client's navigation, for the walkers' heights: each step walks the leg from its start
    // to the step's point (the floor the walker is on carried along, as the original's entities
    // walk), so a walker stays on stairs and bridges instead of the ground
    // under them, and every step agrees with the server's walk of the whole leg. Null: heights as
    // the server told them, in a straight line along a leg.
    void SetNavigation(const NavWorld* nav) { nav_ = nav; }

    // A world event (others are ignored). WorldEntered starts the picture afresh.
    void Apply(const TitleEvent& event);
    // One fixed step of `seconds`.
    void Step(double seconds);
    void Clear();

    bool Entered() const { return self_ != 0; }
    std::uint32_t Self() const { return self_; }
    const proto::WorldEntered& SelfData() const { return selfData_; }
    const Entity* Find(std::uint32_t id) const;
    const std::unordered_map<std::uint32_t, Entity>& Entities() const { return entities_; }
    // Where it is drawn: `alpha` of the way from its last step to its current (SimulationClock).
    static proto::WorldPosition Drawn(const Entity& e, double alpha);
    // A digest of every entity's place (replays: the same messages give the same picture).
    std::uint64_t Hash() const;

private:
    void Leg(Entity& e, const proto::WorldPosition& from, const proto::WorldPosition& to, float speed);
    void Face(Entity& e, std::uint32_t target); // turned toward another entity
    std::unordered_map<std::uint32_t, Entity> entities_;
    std::uint32_t self_ = 0;
    proto::WorldEntered selfData_;
    std::uint32_t nextLook_ = 0; // the last look version given (never reset)
    void Walk(Entity& e, proto::WorldPosition& to) const;
    const NavWorld* nav_ = nullptr;
};

} // namespace sr::game
