#pragma once

// A placed model's own effects: its .bsr's ambient particle modifiers (kind 2 sets, gfx::ModelInfo::
// ambientParticles), kept while the model stands there. Map objects and dungeon blocks carry them: the
// dungeons' teleport circles (`system\system_dungeonpotal.efp` on each zone of the tomb's floors, its door
// outside Jangan, Donhwang's entrance room), Jangan's portal stone's glow, lamps,
// flames. Every one the 1.188 files hold hangs at the model's root with an offset in its own space.

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "sr/gfx/AssetStore.h"
#include "sr/gfx/Effects.h"

namespace sr::client {

// Starts `info`'s attached effects where an instance stands (`world`: rows of its 3 x 4 transform);
// their handles, for Stop when it goes.
inline std::vector<std::uint32_t> StartAttachedEffects(gfx::EffectSystem& effects, const gfx::ModelInfo& info, const std::array<float, 12>& world) {
    std::vector<std::uint32_t> handles;
    if (info.ambientParticles.empty()) return handles;
    // glm is column-major: m[column][row].
    glm::mat4 placed(1.0f);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) placed[column][row] = world[std::size_t(row * 4 + column)];
    }
    for (const assets::ResourceData::ParticleModifier& p : info.ambientParticles) {
        gfx::EffectSystem::Request request;
        request.path = p.effect;
        request.kind = gfx::EffectSystem::Kind::World;
        request.seconds = -1.0f; // while the model stands there
        glm::mat4 at = glm::translate(placed, glm::vec3(p.offset[0], p.offset[1], p.offset[2]));
        if (p.turned) { // ours: about x, y, then z (no file of 1.188 turns one)
            at = glm::rotate(at, p.turn[0], glm::vec3(1, 0, 0));
            at = glm::rotate(at, p.turn[1], glm::vec3(0, 1, 0));
            at = glm::rotate(at, p.turn[2], glm::vec3(0, 0, 1));
        }
        request.at = at;
        if (const std::uint32_t h = effects.Start(request); h != 0) handles.push_back(h);
    }
    return handles;
}

inline void StopAttachedEffects(gfx::EffectSystem& effects, std::vector<std::uint32_t>& handles) {
    for (const std::uint32_t h : handles) effects.Stop(h);
    handles.clear();
}

} // namespace sr::client
