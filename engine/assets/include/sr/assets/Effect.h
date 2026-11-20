#pragma once

// Visual effects: .efp ("JMXVEFF " + four-digit version, 0011 in v1.150), the particle
// programs of skills, hits and glowing objects. Paths inside are relative to the root of
// Particles.pk2. Layout (little-endian; str = u32 length + CP949 bytes):
//   "JMXVEFF " "00vv"; v >= 12: f32 scale; v == 13: 3 x i32; one node, then the end of the file.
//   node: u32, str name; i32 n controllers { str kind, payload by kind }; i32 frames (the
//     node's life at 20 frames a second); i32 n parameters { str kind, payload by kind };
//     programs pre, emitter, post-emitter; command life; program update; u8 keep matrix,
//     u8 keep origin, i32 position depth, i32 matrix depth, i32 velocity depth, u8 local
//     motion, i32 follow depth, u8 shape motion; command view; the resource; command render;
//     programs trailing, render; i32 n children { node }.
//   program: i32 n { command }. command: u8 present (0: nothing more), str name, u8 flags,
//     u8 schedule flags, f32 start, f32 spacing, f32 end, payload by name (see Effect.cpp).
//   resource: u32 back faces (1: both sides), u32 source blend, u32 destination blend
//     (D3DBLEND), u32 x 3 colour stage, u32 x 3 alpha stage, i32 n meshes { str .bms
//     (or a plate), i32 n textures { str .ddj } }.
// Payloads are decided by the command / parameter / controller name: an unknown name
// stops the decode (the rest cannot be found without it).
//
// Not read: 7 files in an older layout, "JMXVEFF 0000" (skill\china\lightning_ganggi_* and
// cold_ganggi_motion_a, etc_mirage_sword_normal). Measured over the 7: after the magic, u8 node
// count, then per node (parents first) u8 child count, that many u8 child indices, str name, u32
// frames; then each node's body, a fixed-size record without command names: a resource header as
// above (u32 back faces, D3D source / destination blend, 6 stage u32), then one keyframe per frame
// (a 4 x 4 matrix, a colour, scale vectors; 188 bytes in etc_mirage_sword_normal) and fields of
// mixed width with the texture / mesh / .ban paths further on. How the original reads it is not known;
// DecodeEfp reports "unsupported JMXVEFF version 0" for them.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace sr::assets {

struct EffectCommand {
    std::string name; // StaticEmit, SetGraphDiffuse, RenderPlate, ViewBillboard, ...
    std::uint8_t flags = 0;
    std::uint8_t schedule = 0;
    float start = 0, spacing = 0, end = 0;
    std::vector<float> values;                      // the payload's numbers in order (a list's count excluded)
    std::vector<std::array<std::uint8_t, 4>> colors; // SetGraphDiffuse: RGBA per frame
    int rows = 0;                                    // list commands: entries (vec3 / vec4 / matrix)
};

struct EffectResource {
    std::uint32_t backFace = 0;
    std::uint32_t srcBlend = 5, dstBlend = 6; // D3DBLEND: 5 SRCALPHA, 6 INVSRCALPHA, 2 ONE
    std::array<std::uint32_t, 6> stages{};
    struct Mesh {
        std::string path; // .bms, relative to the Particles root
        std::vector<std::string> textures;
    };
    std::vector<Mesh> meshes;
    bool additive() const { return dstBlend == 2; }
};

// The random-scale curve (BlendScaleGraph): points at normalised times.
struct EffectScaleGraph {
    struct Point {
        float t = 0;
        std::array<float, 3> value{};
    };
    std::vector<Point> points;
};

struct EffectNode {
    std::string name;
    int frames = 0; // life in frames (20 a second)
    // The node's controllers by kind (NormalTimeLife, NormalTimeLoopLife, StaticEmit, ...);
    // StaticEmit keeps its numbers in `values` as the command does.
    std::vector<EffectCommand> controllers;
    std::vector<EffectCommand> preProgram, emitterProgram, postEmitterProgram, updateProgram, trailingProgram,
        renderProgram;
    EffectCommand life, view, render;
    bool keepMatrix = false, keepOrigin = false, localMotion = false, shapeMotion = false;
    int positionDepth = 0, matrixDepth = 0, velocityDepth = 0, followDepth = 0;
    EffectResource resource;
    std::vector<EffectScaleGraph> scaleGraphs;
    std::vector<EffectNode> children;
    // The first command of a program with that name, or null.
    static const EffectCommand* Find(const std::vector<EffectCommand>& program, std::string_view name);
    // Whether the node starts over at the end of its frames: the life command NormalTimeLoop or
    // the controller NormalTimeLoopLife.
    bool Loops() const { return life.name == "NormalTimeLoop" || Find(controllers, "NormalTimeLoopLife") != nullptr; }
};

struct EffectData {
    int version = 0;
    float scale = 1.0f;
    EffectNode root;
};

bool DecodeEfp(std::span<const std::byte> file, EffectData& out, std::string& error);

} // namespace sr::assets
