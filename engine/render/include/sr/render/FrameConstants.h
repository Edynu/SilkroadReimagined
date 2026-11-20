#pragma once

// What every world shader reads once per frame (through a buffer address in its push
// constants): the light, the fog, the camera and the sky colours. Built by the scene from
// the original's environment (environment.ifo) and the time of day; the defaults are the
// look before environments (one fixed light, no fog, a plain sky).

#include <cmath>

#include <glm/glm.hpp>

namespace sr::render {

struct FrameConstants {
    // xyz: the direction toward the light. w: 1 lights one side (the original's
    // max(0, N.L)); 0 both sides (meshes) as before environments.
    glm::vec4 lightDirection = glm::vec4(glm::normalize(glm::vec3(0.4f, 1.0f, -0.3f)), 0.0f);
    glm::vec4 diffuse{0.55f, 0.55f, 0.55f, 0.0f};
    glm::vec4 ambient{0.45f, 0.45f, 0.45f, 0.0f};
    glm::vec4 fogColor{0.0f};          // a: 1 when fog is on
    glm::vec4 fog{0.0f};               // start, end (view depth), time of day (0..1), seconds
    glm::vec4 cameraPosition{0.0f};
    glm::vec4 cameraForward{0.0f, 0.0f, 1.0f, 0.0f};
    glm::vec4 zenith{0.55f, 0.70f, 0.85f, 0.0f};  // w: the sky gradient's rate
    glm::vec4 horizon{0.55f, 0.70f, 0.85f, 0.0f}; // w: the clouds' alpha
    glm::vec4 scatter{0.55f, 0.70f, 0.85f, 1.0f}; // w: the glow radius around the sun
    glm::vec4 sunTint{1.0f};                      // w: the stars' alpha
    // The terrain's light: rgb the lift of the original's lightmaps (the environment's
    // shadow floor); a: 1 when terrain is lit by its lightmaps (the original), 0 by the sun.
    glm::vec4 terrainLight{0.0f};
    // Cascaded sun shadows (ShadowCascades): world -> atlas (u, v, depth) per cascade.
    glm::mat4 shadowAtlas[4]{};
    glm::vec4 shadowSplits{0.0f}; // view depth where each cascade ends
    glm::vec4 shadowTexels{0.0f}; // world size of a texel per cascade
    glm::vec4 shadowParams{0.0f}; // atlas bindless index, cascades (0: none), 1 / atlas size, filter radius
    // Local lights (LightClusters): the grid (tiles x, y, slices, light count; 0 none),
    // tile size in pixels and the slices' near depth and slices / log(far / near), then the
    // buffer addresses as 32-bit halves: lights, clusters, indices, and 1 for highlights.
    glm::vec4 lightGrid{0.0f};
    glm::vec4 lightSlices{0.0f};
    glm::uvec4 lightBuffers{0u};
    glm::uvec4 lightIndices{0u};
    glm::vec4 shadowStrength{1.0f, 0.0f, 0.0f, 0.0f}; // x: how dark shadows get (the sun 1, the moon less)
    // The scene's screen-space inputs: the depth pre-pass and the ambient occlusion (bindless
    // indices, 0 none), 1 / width, 1 / height of the target.
    glm::vec4 screen{0.0f};
    // x: 1 modern character lighting; y: foliage sway strength (0 off); z: wind seconds.
    glm::vec4 shading{0.0f};
    // Modern water: the scene behind it (bindless index, 0: none), the projection's near and
    // far planes, caustics strength.
    glm::vec4 water{0.0f};
    // Weather: x wetness of the ground (0..1), y rain or snow (0..1), z lightning flash (0..1),
    // w overcast (0..1).
    glm::vec4 weather{0.0f};
    // The original's water colour (the environment's track; the water texture is times it).
    glm::vec4 waterColour{1.0f};
    // The player (origin-relative), w the radius grass and bushes bend away within (0: none).
    glm::vec4 player{0.0f};
    // The camera's view-projection (GLM column-major; screen-space reflections).
    glm::mat4 viewProj{1.0f};
};
static_assert(sizeof(FrameConstants) == 46 * 16);

} // namespace sr::render
