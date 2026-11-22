#pragma once

// Engine-side model data decoded from the original JMXV formats:
//   .bms  mesh            -> MeshData
//   .bmt  material set    -> std::vector<MaterialData>
//   .bsr  resource        -> ResourceData (which meshes and material sets form one object)
// and LoadModel(), which assembles a .bsr into renderable parts. Remaster
// formats (glTF, ...) are meant to produce these same types.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/assets/Animation.h"

namespace sr::assets {

class FileSystem;

struct Bounds {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
};

struct MeshVertex {
    float position[3];
    float normal[3];
    float uv[2];
};

// The walkable surface of a static object (.bms section 7): triangles
// ("cells") over their own vertices in the object's frame, with the perimeter ("outline") and
// cell-to-cell ("internal") edges. Characters stand on these where they are nearer than the
// terrain (bridges, stairs, decks, floors). One navmesh is shared by every instance of an object.
struct ObjectNavMesh {
    // Edge flags. Internal edges: 0x01 blocks crossing from cell B to A, 0x02 from A to B, 0x04 marks
    // the edge internal. Outline edges: 0x01 a wall from outside, 0x02 blocks leaving, 0x08 a link to
    // another object (the region's .nvm says which), 0x10 a ledge (not an entry from outside, blocks
    // leaving: the raised sides of bridges and decks). 0x20 and 0x80 only mark the walk's result.
    struct Edge {
        std::uint16_t a = 0, b = 0;       // vertices
        std::uint16_t cell = 0;           // cell A
        std::uint16_t neighbour = 0xFFFF; // cell B, 0xFFFF = none (always none on the outline)
        std::uint8_t flags = 0;
        std::uint8_t event = 0;           // (flags & 1) bits 6-7: an event on crossing
    };
    // The edge grid: 100 x 100 unit tiles from (originX, originZ), row-major with x fastest, each
    // listing the outline edges that pass through it.
    struct Grid {
        float originX = 0, originZ = 0;
        std::uint32_t width = 0, height = 0;
        std::vector<std::uint32_t> start; // width * height + 1 offsets into `edges`
        std::vector<std::uint16_t> edges; // outline edge indices
    };
    static constexpr float kGridTile = 100.0f;

    std::uint32_t flags = 0; // header word 11: 1 edge event bytes, 2 cell event bytes, 4 event names
    std::vector<std::array<float, 3>> vertices;
    // Per vertex: on the outline, its outward 2D normal as an angle index i: (cos t, -sin t) in
    // (x, z), t = i * 2 pi / 256 (the client pushes a walker out of a wall along it); 0 elsewhere.
    std::vector<std::uint8_t> directions;
    std::vector<std::array<std::uint16_t, 3>> cells;
    std::vector<std::uint8_t> cellEvents; // (flags & 2) per cell: bits 6-7 an event zone, bits 0-5 its name
    std::vector<Edge> outline;
    std::vector<Edge> internal;
    std::vector<std::string> events; // (flags & 4) event or gate names
    Grid grid;
    Bounds bounds;

    bool empty() const { return cells.empty(); }
    // One name starting with "event" and two cells: an event zone the walk treats as terrain
    // (fortress gates, guard towers).
    bool IsEventMarker() const;
};

struct MeshData {
    std::string name;
    std::string materialName; // a material inside one of the resource's .bmt sets
    std::uint32_t version = 0; // 109 or 110
    std::uint32_t flags = 0;   // header flags; 0x400 = lightmapped (second UV set)
    std::uint32_t sortKey = 0; // u32 after the material name; meaning not yet known
    std::vector<MeshVertex> vertices;
    std::vector<std::array<float, 2>> lightmapUvs; // only when lightmapped
    std::string lightmapPath;                      // only when lightmapped
    std::vector<std::uint16_t> indices;            // triangle list
    Bounds bounds;
    // Skinning (filled for 0110 meshes; M4 uses it). Two bones per vertex.
    std::vector<std::string> boneNames;
    std::vector<std::array<std::uint8_t, 2>> boneIndices;
    std::vector<std::array<float, 2>> boneWeights;
    ObjectNavMesh navMesh; // empty for most meshes

    bool IsSkinned() const { return !boneNames.empty(); }
};

struct MaterialData {
    std::string name;
    float ambient[4]{};
    float diffuse[4]{};
    float specular[4]{};
    float emissive[4]{};
    float power = 0.0f;    // specular power or alpha reference; not yet confirmed
    std::uint32_t flags = 0;
    std::string texturePath; // resolved full path ("" when the material has none)
    float textureScale = 1.0f;
    std::uint8_t extra[3]{};
};

struct ResourceData {
    struct MaterialSet {
        std::uint32_t id = 0;
        std::string path; // .bmt
    };
    std::string name;
    std::uint16_t kind0 = 0;
    std::uint16_t kind1 = 0;
    std::vector<MaterialSet> materialSets;
    std::vector<std::string> meshPaths;      // .bms
    std::string navMeshPath;                 // header word 7: the .bms whose object navmesh it walks on
    std::string skeletonPath;                // .bsk, empty for static objects
    std::string skeletonAttachBone;          // wearer bone an item's skeleton hangs from
    std::vector<std::string> animationPaths; // .ban
    // Animation sets (section 5): per weapon stance ("default", "sword", "bow", ...)
    // the state records mapping a state id to an animation (index into animationPaths).
    // State 0 is the stance's idle.
    struct AnimationEvent {
        int timeMs = 0; // into the animation
        int code = 0;   // 1: an effect / hit moment
    };
    struct AnimationState {
        std::uint32_t id = 0;
        int animation = -1;
        std::vector<AnimationEvent> events;
    };
    struct AnimationSet {
        std::string name;
        std::vector<AnimationState> states;
    };
    std::vector<AnimationSet> animationSets;
    // Modifiers (section 6, "ModData"): sets of them, always on ("ambient", kind 2) or
    // while an animation state plays (kind 1). Each modifier starts with 28 bytes
    // (u32 x 6: [2] flags (0x10 its clock runs, 0x100 applied), [3] the material index it
    // changes (0xFFFFFFFF all), [4] overrides the ambient set; u8 x 4: a flip-book's start,
    // end, flags), then by its u32 type:
    //   0x00000 material: u32 duration ms, u32 flags (1 ambient, 2 diffuse take the colour;
    //     4 a scalar list follows), u32 mode (0 loop, 1 back and forth, 2 hold), u32 n
    //     { u32 ms, f32 r, g, b, a }, (4: u32 n { u32 ms, f32 }), u32 x 4 enables (alpha
    //     test, blending, no depth writes, texture-factor pulse), u8 x 16 (source / destination
    //     blend, colour op / args, alpha op / args, alpha ref, alpha func, pulse low / high,
    //     f32 pulse rate), u32 (1: both sides)
    //   0x10000 texture animation: u32 x 5, f32 x 16 ([0] 1: the second texture stage's
    //     coordinates; [1] 0: a flip-book of [2] fps, [3] rows, [4] columns, frames from the start
    //     to the end byte; [1] 1: scrolling, the matrix's rates per second (AmbientLook))
    //   0x10001, 0x10002: u32, str, u32. 0x30000 particles: u32 n { u32 (1: at the root),
    //     str .efp, str bone, f32 x 3 offset, u32 start ms, u8 x 3, u8 turned (1: f32 x 3) }
    //   0x40000: 16 bytes. 0x40001: 40 bytes, u32 n { u8 (1: str) }. 0x50000 sounds: u32 n
    //     (n > 0: 44 bytes, n x { str, u32 tracks { u32 (1: str, i32, str) } }).
    //   0x60000 - 0x60002, 0x70000: nothing more.
    // The section: u32 n sets, the sets, u32 n sets, the sets; a set: u32 kind, i32 state,
    // str animation set, u32 n { u32 type, the modifier }.
    struct MaterialModifier {
        std::uint32_t flags = 0, material = 0xFFFFFFFF;
        std::uint32_t durationMs = 0, colourFlags = 0, mode = 0;
        struct Key {
            std::uint32_t ms = 0;
            float rgba[4]{};
        };
        std::vector<Key> keys;
        std::uint32_t enables[4]{};
        std::uint8_t states[12]{}; // source blend, destination blend, ...
        float pulseRate = 0;
        bool twoSided = false;
    };
    struct TextureModifier {
        std::uint32_t flags = 0, material = 0xFFFFFFFF;
        std::uint32_t words[5]{};
        float matrix[16]{};
        std::uint8_t start = 0, end = 0, bookFlags = 0;
    };
    // An environment map (type 0x40000, the 1.188 client's ModDataEnvMap; its draw setup): the shine of
    // worn items. Its material's alpha is the shine's mask, so the part is drawn without the alpha test, or, with
    // flag 2, testing only for alpha 0 (the reference 1, GREATEREQUAL), whatever the material's own flags say.
    struct EnvMapModifier {
        std::uint32_t flags = 0, material = 0xFFFFFFFF; // the modifier's base words 2 and 3 (0xFFFFFFFF: every material)
        std::uint32_t alphaFlags = 0;                   // base word 5: 2 the alpha test at reference 1
        std::uint32_t words[4]{};                       // its own 16 bytes (not read further yet)
    };
    struct ParticleModifier {
        bool atRoot = false;
        std::string effect; // .efp under the Particles root
        std::string bone;
        float offset[3]{};
        std::uint32_t startMs = 0;
        std::uint8_t flags[3]{};
        bool turned = false;
        float turn[3]{}; // radians
    };
    // A clip's sounds (type 0x50000, the 1.188 client's sound track): at `ms` into the clip, a name
    // effectsound.txt answers (snd_swing_s1, voc_shout1, snd_run, ...), and a wave of its own.
    struct SoundEvent {
        std::uint32_t ms = 0;
        std::string handle;
        std::string wave;
    };
    struct SoundTrack {
        std::string group; // "default", ...
        std::vector<SoundEvent> events;
    };
    struct ModifierSet {
        std::uint32_t kind = 0; // 1 an animation state's, 2 ambient, 0 a clip's by its name (animationSet)
        std::int32_t state = -1;
        std::string animationSet;
        std::vector<MaterialModifier> materials;
        std::vector<TextureModifier> textures;
        std::vector<EnvMapModifier> envMaps;
        std::vector<ParticleModifier> particles;
        std::vector<SoundTrack> sounds;
    };
    std::vector<ModifierSet> modifierSets;
    bool modifiersUnread = false; // section 6 has something DecodeBsr does not know
    // The animation (.ban path) of `state` in the set named `set` (case-insensitive); empty if none.
    std::string StateAnimation(std::string_view set, std::uint32_t state) const;
    // The times (ms into the clip) of `state`'s events of a code in that set (code 1: the hit and effect
    // moments a skill stage's "start event" counts), in order; empty if none.
    std::vector<int> StateEvents(std::string_view set, std::uint32_t state, int code = 1) const;
    // The sounds of `state`'s clip in that set, in order of their moments (the state's own modifier
    // set, else one under its clip's name); empty if none.
    std::vector<SoundEvent> StateSounds(std::string_view set, std::uint32_t state) const;
};

// .cpd: an object assembled from several .bsr parts (NPC body + armour + weapon, ...).
struct CompoundData {
    std::string name;
    // The compound's collision resource (.bsr, usually one of its parts; empty when none): a compound
    // placed in the world walks on this resource's object navmesh (1.188's wrecks, LoadObjectNavMesh).
    std::string collisionPath;
    std::vector<std::string> partPaths; // .bsr
};

bool DecodeBms(std::span<const std::byte> file, MeshData& out, std::string& error);
// Only the object navmesh of a .bms (true with an empty mesh when it has none).
bool DecodeBmsNavMesh(std::span<const std::byte> file, ObjectNavMesh& out, std::string& error);
bool DecodeCpd(std::span<const std::byte> file, CompoundData& out, std::string& error);
// `bmtPath` resolves texture names, which are relative to the .bmt's folder.
bool DecodeBmt(std::span<const std::byte> file, const std::string& bmtPath, std::vector<MaterialData>& out,
               std::string& error);
bool DecodeBsr(std::span<const std::byte> file, ResourceData& out, std::string& error);

// What a resource's always-on ("ambient", kind 2) modifiers do to the parts drawn with one of its
// materials ("Object materials and texture animation").
struct AmbientLook {
    // Texture animation, scrolling: the texture coordinates are multiplied, as the row (u, v, 1),
    // by a 3 x 3 matrix that starts as the identity and grows by these rates every second (the
    // original adds rate x elapsed time each tick, never wrapping): m11, m12, m21, m22, then the
    // offsets m31 (u) and m32 (v). The corpus has no flip-books among the ambient sets.
    bool scrolls = false;
    float uvRate[6]{};
    // Material: blending without the alpha test, or with depth writes off, makes the part
    // translucent (waterfalls, glows): drawn after the solid parts, not writing depth. Blending
    // with the alpha test and depth writes (trees, the Hotan fountain) stays with the solid parts.
    bool blended = false;
    bool alphaTest = false;                              // (blended) below the reference: dropped
    std::uint8_t sourceBlend = 5, destinationBlend = 6;  // D3DBLEND (5 source alpha, 6 its inverse, 2 one)
    bool twoSided = false;
    // (blended) the colour stage's factor on the lit texture: the material's key colour (grey),
    // doubled for "modulate 2x".
    float tint = 1.0f;
    // An environment map (ResourceData::EnvMapModifier; worn items' shine) covers the material: its alpha is the
    // shine's mask, so the material's own cut-out flag does not count; the part is cut out only where its alpha is 0
    // when the modifier asks for it (ALPHAREF 1).
    bool envMap = false;
    bool envAlphaZero = false;
};
// The ambient look of `materialIndex` (a part's place in its .bmt) under `resource`'s modifiers.
AmbientLook PartAmbientLook(const ResourceData& resource, int materialIndex);

// One mesh with the material it is drawn with.
struct ModelPart {
    std::string meshPath;
    int meshIndex = -1; // position in its resource's mesh list (the cover table's "prim")
    // False for worn items in the prototype's renderer (engine/render): their materials may carry the
    // 0x200 "alpha is transparency" flag yet keep an environment-map (shine) mask in alpha. The
    // rewrite's asset store decides by AmbientLook::envMap instead and ignores this.
    bool allowAlphaTest = true;
    // The outfit's item it comes from (its place in AssembleOutfit's itemPaths); -1: the body, or not an outfit.
    int outfitItem = -1;
    // A worn piece of cover mode 2 (PartLink::c): drawn over the body's skin, which stays under it.
    bool overSkin = false;
    MeshData mesh;
    bool hasMaterial = false;
    MaterialData material;
    int materialIndex = -1; // its place in its .bmt (modifiers name materials by it)
    AmbientLook ambient;    // its resource's always-on modifiers on its material
};

struct ModelData {
    std::string sourcePath;
    ResourceData resource; // the main resource; empty when loaded from a bare .bms
    std::vector<ModelPart> parts;
    std::vector<std::string> warnings; // missing files, unknown materials, ...
    Bounds bounds;
    SkeletonData skeleton;                   // empty for static objects
    std::vector<std::string> animationPaths; // from the resource(s) that own the skeleton
    std::vector<ResourceData::AnimationSet> animationSets; // from the same resource
    // Bones of worn items the wearer lacks (a bow's marker nodes: where an arrow is held; a rare
    // weapon's ai_start / ai_end, where its glow hangs), each as it stands in the item's own space,
    // the wearer bone the item hangs from and the item resource it came from (items share names).
    struct Socket {
        std::string name;
        std::string parentBone;
        std::string item; // the item's .bsr, as passed to AssembleOutfit
        std::array<float, 3> position{};
        std::array<float, 4> rotation{0, 0, 0, 1}; // x, y, z, w
    };
    std::vector<Socket> sockets;
};

// Loads a .bsr (all meshes, materials from the first set that has them, the
// skeleton), a .cpd (all its .bsr parts merged, skeleton from the first part
// that has one) or a bare .bms (no material). Returns false only when nothing
// could be loaded.
bool LoadModel(const FileSystem& fs, const std::string& path, ModelData& out, std::string& error);
// The object navmesh a navmesh\object.ifo entry walks on: a .bsr names its .bms; a .cpd's collision
// resource does. True with an empty mesh when the object has none (an effect, a compound without a
// collision resource, a resource without a navmesh).
bool LoadObjectNavMesh(const FileSystem& fs, const std::string& objectPath, ObjectNavMesh& out, std::string& error);
// Dresses the main resource's meshes in its material set `setId` (section 0 ids: 0 the
// default look, 2 the champion's). False when the resource has no such set.
bool UseMaterialSet(const FileSystem& fs, ModelData& model, std::uint32_t setId);

// The attachment table at the very end of a .bsr (after every other section):
//   u32 a, u32 b, u32 c, u32 count, count x { u32 key, u32 value }   [+ u32 0 on characters]
// On a character, pairs map a cover key to the mesh (prim) index it hides; on an
// item, `b` is the equipment part (HA 0, BA 1, LA 2, FA 3, SA 4, AA 5, a weapon 6 / 7,
// a job suit 16), `c` the cover mode (1: the wearer's meshes with the same keys are hidden
// while it is worn; 2: nothing is hidden, the piece is drawn over the skin) and the pair
// keys are the body regions it covers (their values are not read: 0 or 1).
// Found by scanning back from the end of the file (there is no pointer to it):
// the largest `count` whose record lines up exactly and passes the checks.
struct PartLink {
    std::uint32_t a = 0, b = 0, c = 0;
    std::vector<std::array<std::uint32_t, 2>> pairs; // key, value
};
bool FindPartLink(std::span<const std::byte> bsr, bool character, std::size_t meshCount, PartLink& out);

} // namespace sr::assets
