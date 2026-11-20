#pragma once

// A dungeon's navigation (C4d): no terrain and no region grid. The walkable
// surface is each block's floor navmesh (the object navmesh its .bsr names) placed in the dungeon's
// own space; a walk goes triangle by triangle as on an outdoor object and passes from
// block to block where a doorway edge of one (outline flag 8) meets a doorway edge of a neighbouring
// block (the .dof's neighbour lists). Immutable once built, like a NavRegion.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/assets/Dungeon.h"
#include "sr/game/NavMesh.h"

namespace sr::game {

class NavDungeon {
public:
    struct Block {
        std::shared_ptr<const NavObjectMesh> mesh; // its floor; null: none
        float x = 0, y = 0, z = 0;                 // dungeon space
        float cos = 1, sin = 0;                    // of the yaw (x' = x cos - z sin, as outdoor objects; inferred)
        std::vector<std::uint32_t> neighbours;
        // Per outline edge of the mesh: the block and outline edge a doorway leads to (-1: none).
        std::vector<std::int32_t> doorBlock;
        std::vector<std::uint32_t> doorEdge;
        float min[3]{}, max[3]{}; // its floor's box in dungeon space (rays)
        void ToLocal(float wx, float wz, float& lx, float& lz) const;
        void ToWorld(float lx, float lz, float& wx, float& wz) const;
    };

    // `meshFor(blockIndex)` gives each block its floor's navmesh (or null).
    template <typename MeshFor>
    static std::shared_ptr<const NavDungeon> Build(const assets::DungeonData& data, MeshFor&& meshFor) {
        auto dungeon = std::make_shared<NavDungeon>();
        dungeon->blocks_.resize(data.blocks.size());
        for (std::size_t i = 0; i < data.blocks.size(); ++i) dungeon->blocks_[i].mesh = meshFor(static_cast<std::uint32_t>(i));
        dungeon->Link(data);
        return dungeon;
    }

    std::uint16_t Region() const { return region_; }
    const std::vector<Block>& Blocks() const { return blocks_; }
    std::size_t Doors() const { return doors_; } // doorway edges matched (each side counts)

    // The floor under (x, z) nearest in height to y, among the blocks of the point's 200-unit grid
    // cell: instance = the block, cell = its triangle. Invalid outside the grid or over no floor.
    NavPosition Drop(float x, float y, float z) const;
    // Walks from `from` towards (x, z) across the floors and through doorways (result bit kLink
    // when it changed block); stopped 0.2 back at blocking edges and at outline edges without a door.
    // `zones`: the event zones it enters (NavZonesEntered), in order. `blocked`: the zones standing in the way (their
    // names in lower case without the turn: an instance's shut doors); null: none.
    std::uint32_t Walk(const NavPosition& from, float x, float z, NavPosition& to, std::uint32_t maxCrossings = 100000,
                       std::vector<std::string>* zones = nullptr, const std::unordered_set<std::string>* blocked = nullptr) const;
    // The nearest floor the ray meets within `maxLength` (dungeon space; `direction` of unit length):
    // hit.instance the block.
    bool Ray(const float origin[3], const float direction[3], float maxLength, NavHit& hit) const;

private:
    void Link(const assets::DungeonData& data);

    std::uint16_t region_ = 0;
    float boundsMin_[3]{};
    std::uint32_t gridSize_[3]{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> grid_; // key (y << 20) | (z << 10) | x
    std::vector<Block> blocks_;
    std::size_t doors_ = 0;
};

} // namespace sr::game
