#pragma once

// What every world pass reads about the frame: the camera, the light, the fog,
// the terrain lightmap's lift and the water colour, from the time of day (C3e, Environment.h).
// The game fills SceneLighting in its snapshot; the scene renderer writes FrameConstants once a
// frame into transient memory, followed by the frame's point lights (PointLight), and hands its
// address to the passes (shaders: Frame.hlsli).

#include <cstdint>

#include <glm/glm.hpp>

namespace sr::gfx {

// The game's side: the environment at this moment. The original's light is one directional light
// whose direction never changes: from +X low over the terrain, at 45 degrees over the
// objects; its ambient differs between the two.
struct SceneLighting {
    glm::vec3 terrainLight{5.0f, 1.0f, 0.0f};   // towards the light (normalised by the renderer)
    glm::vec3 objectLight{1.0f, 1.0f, 0.0f};
    glm::vec3 diffuse{0.3f};                     // the light's diffuse
    glm::vec3 terrainAmbient{0.25f};             // ambient while the terrain draws
    glm::vec3 objectAmbient{0.45f};              // ambient while objects draw
    glm::vec3 lift{0.3f};                        // added to the terrain lightmap before it multiplies
    bool lightmaps = true;
    bool fog = false;
    glm::vec3 fogColor{0.6f, 0.7f, 0.8f};        // everything but the near terrain
    glm::vec3 terrainFogColor{0.6f, 0.7f, 0.8f}; // the near terrain (the original: sqrt of the fog colour)
    float fogStart = 0.0f, fogEnd = 0.0f;        // linear, by view depth
    glm::vec3 waterColor{1.0f};                  // multiplies the water's frames
    float timeOfDay = 0.5f;                      // 0..1
    // Objects (C4d): they take the linear fog outdoors and in dungeons the squared
    // exponential 2^-(depth x density)^2.
    enum class ObjectFog : std::uint8_t { Linear, SquaredExponential };
    ObjectFog objectFog = ObjectFog::Linear;
    float fogDensity = 0.0f;
};

// A point light some instances take (InstanceRecord::flags bits 16-31 = 1 + its index): the
// original's vertex shader adds the light's ambient and its diffuse x N.L / (a0 + a1 d + a2 d^2).
struct PointLight {
    glm::vec3 position{0.0f};
    glm::vec3 diffuse{0.0f};
    glm::vec3 ambient{0.0f};
    glm::vec3 attenuation{1.0f, 0.0f, 0.0f}; // constant, linear, quadratic
};
struct PointLightGpu {
    glm::vec4 position, diffuse, ambient, attenuation;
};
static_assert(sizeof(PointLightGpu) == 64);

// The GPU's side (std430-like; offsets in Frame.hlsli).
struct FrameConstants {
    glm::mat4 viewProj{1.0f};       // 0
    glm::vec4 camera{0.0f};         // 64: world position; w: time of day
    glm::vec4 terrainLight{0.0f};   // 80: towards the light
    glm::vec4 objectLight{0.0f};    // 96
    glm::vec4 diffuse{0.0f};        // 112
    glm::vec4 terrainAmbient{0.0f}; // 128
    glm::vec4 objectAmbient{0.0f};  // 144
    glm::vec4 lift{0.0f};           // 160: rgb; w: 1 with lightmaps
    glm::vec4 fogColor{0.0f};       // 176: rgb; w: 1 with fog
    glm::vec4 terrainFogColor{0.0f}; // 192
    glm::vec4 fogRange{0.0f};       // 208: start, end, 1 / (end - start)
    glm::vec4 time{0.0f};           // 224: seconds since start
    glm::vec4 water{0.0f};          // 240: the water colour, quantised to bytes as the original's
    glm::vec4 objects{0.0f};        // 256: fog kind (0 linear, 1 squared exponential), density
    glm::vec4 lights{0.0f};         // 272: x the point lights following the constants
    // The sun's cascaded shadows (the Modern look's, G1; params.x 0: none).
    glm::mat4 shadowAtlas[4]{};     // 288: world -> atlas u, v, depth of cascade i
    glm::vec4 shadowSplits{0.0f};   // 544: view depth where cascade i ends
    glm::vec4 shadowTexels{0.0f};   // 560: world size of a texel of cascade i
    glm::vec4 shadowParams{0.0f};   // 576: cascades, atlas (bindless), strength, filter (0 low .. 3 ultra)
    glm::vec4 shadowLight{0.0f};    // 592: towards the light casting them; w 1: objects are lit from it
    glm::vec4 skyZenith{0.0f};      // 608: the sky overhead (the modern water's reflection); w 1: a sky
    glm::vec4 skyHorizon{0.0f};     // 624: the sky at the horizon
    glm::vec4 sun{0.0f};            // 640: towards the sun (w its strength, 0 below the horizon)
    glm::vec4 sunColour{0.0f};      // 656
    // 672: the Modern rows: x character lighting (wrap and rim), y foliage sway, z wet ground (0..1),
    // w glowing windows (night-light materials above 1, for the bloom)
    glm::vec4 modern{0.0f};
    // 688: the scene copied before the modern water (x its bindless index; rgb the colour, a the
    // view depth), for its reflections and refraction; y 1 when there is one. z: the effects'
    // lights (V2) following the point lights, 32 bytes each (position, radius; colour x intensity).
    glm::vec4 waterCopy{0.0f};
    // 720: the effects' lights by screen tile: x tiles across, y the tile's size in pixels, z where
    // the tiles' masks start (bytes from the constants; a uint2 per tile, bit i: effect light i).
    glm::vec4 lightTiles{0.0f};
};
static_assert(sizeof(FrameConstants) == 720);

FrameConstants MakeFrameConstants(const glm::mat4& viewProj, const glm::vec3& camera, const SceneLighting& lighting, double seconds,
                                  std::size_t pointLights = 0);
PointLightGpu MakePointLight(const PointLight& light);

} // namespace sr::gfx
