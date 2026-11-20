#pragma once

// The original's visual effects (.efp, assets/Effect.h): the game thread starts them, simulates them on the job system
// and writes what they show
// into the frame's EffectFrame, which the render thread draws (EffectPass).
//
// The simulation follows the format: 20 ticks a second; every node of the tree is a kind of
// element; an element of a node emits elements of each child node by the child's StaticEmit
// (start frame, duration, period, limit, count per emission) counted on the parent's age; each
// element runs its node's render program (commands on their scheduled frames: positions,
// velocities, cones, spheres, forces, rotations, random scale), moves by its velocity every tick
// and dies at the end of its node's frames (looping and NeverExtinct nodes, and a kept effect's
// fixed pieces, start over). A tick first ages every element (a death gives its place back to its
// parent's emitter), then the living run their commands and emit; the root node is emitted by its
// own StaticEmit on the effect's clock. An element shows as its node's plate (a unit square) or
// mesh, sized by SetGraphScale, coloured by SetGraphDiffuse (each frame's row held to the end of
// its life), its texture window from TextureSlide, facing the camera (ViewBillboard), turning about
// Y (ViewYBillboard), along its velocity (ViewVBillboard) or as it is turned (ViewNone), its shape's
// own turn inside that (SetShapeRot / SetShapeRotVel: spinning sprites); its blending and texture
// stages (MODULATE2X / 4X) from the node's resource. A ribbon node (RenderLinkPipe, LinkDPipe, LinkObj)
// draws no element alone: the elements one parent element made of it, oldest first, are strung into one
// strip: through their places facing the camera (LinkDPipe as they are,
// LinkPipe smoothed by a B-spline), or between two edges along each element's own up axis (LinkObj). Elements are carried every tick by the motion of
// the ancestors their node names (position, follow, matrix and velocity depths; past the root the
// effect's own place); one carried by nothing is let go into the world where it is born (trails,
// sparks left behind a moving weapon). Commands add to the element's own, its parent's or the root's
// place, in the frame their flags choose; SetBANPos / SetBANRot are keyed paths over the life.
//
// Built for crowds (300 effects, 10,000 particles in a fortress war):
// - programs are read, decoded and compiled once per file on a Streaming job, their textures and
//   meshes streamed by the asset store;
// - each frame the instances are stepped and built in parallel (ParallelFor, one instance per
//   item), each into its own reused buffers; the gather into the frame is a copy;
// - effects off the screen are not simulated (their clock runs, so they end on time); far ones
//   emit fewer elements (a level of detail by distance); the effect quality (the original's video
//   option 13: 0 none, 1 hits only, 2 all) filters what starts; caps bound elements per effect,
//   effects alive and what a frame draws.
// - each effect gives one light (its additive pieces' colour, centre and size) into the frame,
//   for the lighting of V2; nothing lights the scene with them yet.
//
// Thread: the game thread (Start, Stop, Update); Update uses the job system inside.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "sr/assets/Effect.h"
#include "sr/core/Jobs.h"
#include "sr/gfx/AssetStore.h"

namespace sr::assets {
class FileSystem;
}

namespace sr::gfx {

// --- What a frame draws (the snapshot's part; EffectPass reads it) --------------------------------

// The resource's D3D blending: SRCALPHA / INVSRCALPHA (and anything unusual), ONE / INVSRCALPHA,
// and destination ONE (additive, its source SRCALPHA or ONE).
enum class EffectBlend : std::uint8_t { Alpha, Premultiplied, Additive };
inline constexpr std::size_t kEffectBlends = 3;
EffectBlend EffectBlendOf(std::uint32_t srcBlend, std::uint32_t dstBlend);

// Sprite and mesh flags (Effect.hlsl).
inline constexpr std::uint32_t kEffectSourceOne = 4;     // D3DBLEND_ONE source: the colour adds as it is (alpha ignored)
inline constexpr std::uint32_t kEffectPremultiplied = 8; // the texture's colour is premultiplied by its alpha
inline constexpr std::uint32_t kEffectOwnTexture = 16;   // a mesh in its own texture (the node names none)
// The resource's texture stages (D3DTOP_MODULATE2X / 4X): the colour, the alpha times 2 or 4 (then clamped).
inline constexpr std::uint32_t kEffectColour2x = 32, kEffectColour4x = 64, kEffectAlpha2x = 128, kEffectAlpha4x = 256;

struct EffectSprite { // 64 bytes, Effect.hlsl's sprite record
    glm::vec3 centre{0.0f};
    std::uint32_t colour = 0xFFFFFFFFu; // RGBA8, red in the low byte
    glm::vec3 axisX{0.0f};              // half the plate is +-axisX * 0.5
    std::uint32_t texture = 0;          // bindless
    glm::vec3 axisY{0.0f};
    std::uint32_t flags = 0;
    glm::vec4 window{0.0f, 0.0f, 1.0f, 1.0f}; // uv offset, uv scale
};
static_assert(sizeof(EffectSprite) == 64);

struct EffectMeshInstance { // 80 bytes
    glm::vec4 rows[3]{};    // the mesh's world matrix (rows of 3 x 4)
    std::uint32_t colour = 0xFFFFFFFFu;
    std::uint32_t texture = 0;
    std::uint32_t flags = 0;
    std::uint32_t pad = 0;
    glm::vec4 window{0.0f, 0.0f, 1.0f, 1.0f};
};
static_assert(sizeof(EffectMeshInstance) == 80);

struct EffectRibbonVertex { // 32 bytes, Effect.hlsl's ribbon vertex (a triangle list, six a segment)
    glm::vec3 position{0.0f};
    std::uint32_t colour = 0xFFFFFFFFu; // RGBA8, red in the low byte
    glm::vec2 uv{0.0f};
    std::uint32_t texture = 0;          // bindless
    std::uint32_t flags = 0;
};
static_assert(sizeof(EffectRibbonVertex) == 32);

// Instances of one mesh part (geometry pool offsets) in one blend.
struct EffectMeshDraw {
    std::uint64_t vertexOffset = 0; // bytes into the geometry pool
    std::uint32_t firstIndex = 0;   // into the pool's 16-bit indices
    std::uint32_t indexCount = 0;
    std::uint32_t vertexFlags = 0;  // kVertex*
    std::uint32_t firstInstance = 0, instanceCount = 0;
    EffectBlend blend = EffectBlend::Alpha;
};

// The light an effect gives (V2 lights the scene with them).
struct EffectLight {
    glm::vec3 position{0.0f};
    float radius = 0.0f;
    glm::vec3 colour{1.0f}; // the hue (its brightest channel 1)
    float intensity = 0.0f;
    std::uint32_t id = 0;   // its effect's handle, kept from frame to frame (0: none, a window's)
};

struct EffectFrame {
    // Sprites by blend (Alpha, then Premultiplied, then Additive), the alpha ones far to near.
    std::vector<EffectSprite> sprites;
    std::array<std::uint32_t, kEffectBlends> spriteCounts{};
    std::vector<EffectMeshInstance> meshInstances;
    std::vector<EffectMeshDraw> meshDraws; // by blend, in the same order
    // Ribbons' vertices by blend (the alpha ones by effect, far to near), six a segment.
    std::vector<EffectRibbonVertex> ribbons;
    std::array<std::uint32_t, kEffectBlends> ribbonCounts{};
    std::vector<EffectLight> lights;       // one per visible effect that glows
    bool Empty() const { return sprites.empty() && meshDraws.empty() && ribbons.empty(); }
    void Clear() {
        sprites.clear(); // keeps the capacity: no allocation per frame
        spriteCounts = {};
        ribbons.clear();
        ribbonCounts = {};
        meshInstances.clear();
        meshDraws.clear();
        lights.clear();
    }
};

// --- Programs ---------------------------------------------------------------------------------

// An .efp ready to run: the node tree flattened (parents first), each node's commands by the frames
// they fire at, its tracks (colour, size, texture window) and its look. Immutable once compiled.
struct EffectProgram {
    enum class Op : std::uint8_t {
        Position, KeyedPosition, SpherePosition, ConePosition, ConeVelocity, ConeForce, Velocity, Force, Attraction, RandomScale,
        Turn, KeyedTurn, Spin, Shape, ShapeSpin
    };
    // A command's flags as the original's opcodes take them: positions start from `base` (0 the element's own place,
    // 1 its parent's, 2 the
    // previous element of its group's) and add a value turned by `frame` (0 none, 1 the element's own turn, 2 its
    // parent's, 3 the previous element's); velocities, forces and turns take the frame alone (turns: 0 set, else the
    // value times the frame's turn). A command naming the previous element does nothing on the group's first.
    struct Command {
        Op op = Op::Position;
        std::uint8_t flags = 0;
        std::uint8_t base = 0, frame = 0;
        glm::vec3 value{0.0f}; // position, velocity, force; cones: min speed, max speed, degrees
        float amount = 0.0f;   // Attraction
        glm::mat3 turn{1.0f};  // Turn, Spin, Shape, ShapeSpin
        std::vector<glm::vec3> positions; // KeyedPosition (SetBANPos): one per key, over the element's life
        std::vector<glm::mat3> turns;     // KeyedTurn (SetBANRot)
    };
    // The original's render kinds (by the node's kind: 1 plate, 2 mesh, 3 LinkDPipe, 4 LinkPipe,
    // 5 LinkObj).
    enum class Render : std::uint8_t { None, Plate, Mesh, LinkDPipe, LinkPipe, LinkObj };
    enum class View : std::uint8_t { Own, Camera, Upright, Velocity };
    struct Node {
        int frames = 1;
        bool loops = false, endless = false;
        // Whose motion carries its elements, as ancestors counted up (1 the parent; past the root the
        // effect's own place; 0 none): their place turned about that ancestor (positionDepth), moved
        // with it (followDepth), their turn (matrixDepth) and velocity (velocityDepth) turned with it.
        // An element carried by nothing stays where it was born in the world (a trail, a spark).
        int positionDepth = 1, matrixDepth = 1, velocityDepth = 0, followDepth = 1;
        bool localMotion = false; // its rotation velocity (SetRVelocity) turns it every tick
        bool shapeMotion = false; // its shape's rotation velocity (SetShapeRotVel) turns its plate or mesh every tick
        bool emitter = false; // a StaticEmit; without one, one element born with its parent
        float emitStart = 0, emitDuration = 0, emitPeriod = 1, emitLimit = 0, emitRate = 0;
        int parent = -1;
        int childSlot = 0;          // its place among its parent's children
        std::vector<int> children;
        std::vector<Command> commands;
        std::vector<std::uint32_t> firing; // frames + 1 offsets into firingCommands
        std::vector<std::uint16_t> firingCommands;
        std::vector<std::array<std::uint8_t, 4>> colours; // RGBA per frame
        std::vector<glm::vec3> sizes;                       // per frame
        std::vector<glm::vec4> windows;                     // texture windows over the life
        std::vector<assets::EffectScaleGraph::Point> randomScale;
        Render render = Render::None;
        View view = View::Own;
        int texture = -1, mesh = -1; // into the program's lists
        EffectBlend blend = EffectBlend::Alpha;
        bool sourceOne = false;
        bool glows = false; // additive: gives light
        std::uint32_t modulate = 0; // kEffectColour2x / 4x, kEffectAlpha2x / 4x
    };
    std::vector<Node> nodes;
    std::vector<std::string> textures, meshes; // paths under the Particles root
    float scale = 1.0f;
    int lengthTicks = 0; // the longest a one-shot runs (its root's emission, then the deepest chain)
};
bool CompileEffect(const assets::EffectData& data, EffectProgram& out, std::string& error);

// One effect's elements (the simulation alone: tests and the system use it).
class EffectSimulation {
public:
    static constexpr std::size_t kMaxElements = 2048;
    static constexpr float kTick = 0.05f; // 20 frames a second

    // `kept`: the effect stays (an aura, a buff): its fixed pieces start over instead of ending.
    void Start(const EffectProgram& program, std::uint32_t seed, bool kept);
    // Where the effect stands now (its place times its scale): elements carried by nothing are born
    // into the world there and stay when it moves on. Before Start and each Advance.
    void Place(const glm::mat4& space) { space_ = space; }
    // Advances by `dt` seconds (at most 8 ticks a call). `lod` 0..1: the share of emissions kept.
    void Advance(float dt, float lod = 1.0f);
    // No more emission; what is alive finishes.
    void Release() { releasing_ = true; }
    bool Done() const;
    std::size_t Alive() const { return alive_; }
    int Ticks() const { return ticks_; }
    float TickFraction() const { return tickTime_ / kTick; }

    // What it shows now, in the space `at` (scaled by `scale`), appended to the outputs.
    struct Look {
        static constexpr std::uint8_t kReady = 1, kPremultiplied = 2, kFailed = 4;
        std::vector<std::uint32_t> textures;     // bindless per program texture
        std::vector<std::uint8_t> textureFlags;  // k*; a node whose texture is not ready is not drawn
        std::vector<std::uint8_t> meshReady;     // per program mesh
    };
    struct MeshItem {
        int mesh = 0;
        EffectBlend blend = EffectBlend::Alpha;
        EffectMeshInstance instance;
    };
    struct Output {
        std::array<std::vector<EffectSprite>, kEffectBlends> sprites;
        std::vector<MeshItem> meshes;
        std::array<std::vector<EffectRibbonVertex>, kEffectBlends> ribbons;
        EffectLight light;
        bool lit = false;
        void Clear() {
            for (auto& s : sprites) s.clear();
            for (auto& r : ribbons) r.clear();
            meshes.clear();
            lit = false;
        }
    };
    // `camera`: its right, up and forward (into the scene) as columns; at most `budget` pieces.
    // `pull`: units every piece is drawn nearer the camera (Request::pull).
    // `opacity` (0..1) fades the whole effect: its pieces' alpha, an additive piece's colour.
    void Build(const glm::mat4& at, float scale, const Look& look, const glm::mat3& camera, std::size_t budget, Output& out,
               float pull = 0.0f, float opacity = 1.0f) const;

private:
    struct Element {
        glm::vec3 position{0.0f}, previous{0.0f}, velocity{0.0f}, origin{0.0f};
        glm::vec3 randomScale{1.0f};
        glm::mat3 turn{1.0f}, spin{1.0f}, previousTurn{1.0f};
        glm::mat3 shape{1.0f}, shapeSpin{1.0f}; // the plate's or mesh's own turn and its rotation velocity
        glm::mat4 space{1.0f}; // a loose element's: the effect's place when it was let go (its coordinates are in it)
        int node = 0;
        int parent = -1;
        int age = 0;
        std::uint32_t emitted = 0; // its children's running totals, from here in emitted_
        bool alive = true;
        bool loose = false; // in `space` rather than the effect's current place
    };
    float Random();
    bool Loops(const Element& e, const EffectProgram::Node& node) const;
    // The ancestor `depth` levels up (1 the parent); null past the root (the effect's own place) or
    // when one on the way is gone. `top`: the topmost living ancestor (the element itself at the root).
    const Element* Ancestor(const Element& e, int depth) const;
    const Element& Top(const Element& e) const;
    void Carry(Element& e, const EffectProgram::Node& node);
    // `before`: e's place in elements_ (its size for one being born): the previous element of its group is searched
    // before it.
    void Run(Element& e, int frame, std::size_t before);
    // The previous element of e's group (its node's elements made by the same parent), alive, before `before`.
    const Element* Previous(const Element& e, std::size_t before) const;
    void Birth(int node, int parent);
    void Emit(std::size_t index);
    void EmitRoots();
    void Tick();
    void Compact();

    const EffectProgram* program_ = nullptr;
    std::vector<Element> elements_;
    std::vector<float> emitted_;
    std::size_t alive_ = 0;
    float rootEmitted_ = 0.0f;
    glm::mat4 space_{1.0f};
    int ticks_ = 0;
    float tickTime_ = 0.0f;
    float lod_ = 1.0f;
    std::uint32_t random_ = 1;
    bool kept_ = false, releasing_ = false;
};

// --- The game's effects -----------------------------------------------------------------------

struct EffectLimits {
    std::size_t effects = 2048;      // alive at once (more are not started)
    std::size_t sprites = 16384;     // drawn in a frame (quality 2; quality 1 a quarter)
    std::size_t meshInstances = 8192;
    std::size_t lights = 1024;
    float lodNear = 800.0f, lodFar = 2500.0f; // full emission nearer, a quarter at lodFar, none beyond 1.5 x
};

class EffectSystem {
public:
    using Handle = std::uint32_t; // 0: none

    // Where an effect stands. A target of 0 is a fixed place (the request's `at`, Move changes it);
    // otherwise the owner's key, placed every frame by the Place callback (false: the target is gone
    // and the effect is released).
    struct Anchor {
        std::uint32_t target = 0;
        std::string bone;      // empty: the model's root
        bool atHeight = false; // the root raised to the body's height (the top of its bounding box)
        bool atMiddle = false; // the root raised to half of it
        bool keepTurn = true;  // false: the bone's place with no turn
        bool inBone = false;   // the offset in the bone's frame, a point with no turn
        glm::vec3 offset{0.0f}; // in the root's turned frame (inBone: the bone's)
        // Another target whose root's turn the effect takes, its offset in that frame too (0: its own):
        // an effect on the target turned by the caster (A 5).
        std::uint32_t turnOf = 0;
        bool follow = true;    // false: placed where the target is when it starts, then left there
    };
    using Place = std::function<bool(const Anchor&, glm::mat4&)>;
    // Skill effects follow the effect quality fully, hits (the hiteffect folder) down to 1, the
    // world's own (glowing objects) always.
    enum class Kind : std::uint8_t { Skill, Hit, World };
    struct Request {
        std::string path;    // .efp under the Particles root
        glm::mat4 at{1.0f};  // a fixed effect's place
        Anchor anchor;
        glm::mat4 local{1.0f}; // put on the effect in its anchor's space (a stage's own turn)
        float scale = 1.0f;
        float seconds = 0.0f;  // 0: until its program ends; > 0: kept that long; < 0: kept until Stop / Release
        float delay = 0.0f;    // seconds before it starts
        Kind kind = Kind::Skill;
        // Units its pieces are drawn nearer the camera than they are: a glow on a bone inside a
        // model (Yeoha's eyes in its skull) shows on its surface instead of behind it.
        float pull = 0.0f;
        // Fades of the whole effect (a stage's col 12, A 5): from
        // nothing to whole over `fadeIn` seconds as it starts, to nothing over `fadeOut` once released
        // (it is gone then).
        float fadeIn = 0.0f, fadeOut = 0.0f;
    };


    EffectSystem() = default;
    ~EffectSystem() { Shutdown(); }
    EffectSystem(const EffectSystem&) = delete;
    EffectSystem& operator=(const EffectSystem&) = delete;

    void Init(AssetStore& assets, const assets::FileSystem& files, core::JobSystem& jobs, const EffectLimits& limits = EffectLimits{});
    // Ends every effect and gives the programs' textures and meshes back (load jobs still running
    // give theirs back themselves).
    void Shutdown();

    // 0 when the quality filters it out, the path is none, or too many effects are alive.
    Handle Start(const Request& request);
    void Stop(Handle handle);    // gone at once
    void Release(Handle handle); // emits no more, stays where it is; what it made finishes
    void Move(Handle handle, const glm::mat4& at); // a fixed effect's new place
    bool Alive(Handle handle) const;
    void SetQuality(int quality) { quality_ = quality; }
    int Quality() const { return quality_; }

    struct View {
        glm::mat4 viewProj{1.0f};
        glm::vec3 eye{0.0f};
        glm::vec3 forward{0.0f, 0.0f, 1.0f}; // the camera's direction, into the scene
    };
    // A ribbon behind a moving line (a weapon's trail, the 1.188 client's "mirage"
    // 31-skill-presentation.md A 9): the line's places over its last `seconds`, drawn additively in
    // `texture` (under the Particles root) tinted `colour` (RGBA8), fading along its length. Point adds
    // the line's place now (from `a` to `b`); End lets what is drawn fade out, then it is gone.
    Handle StartTrail(const std::string& texture, std::uint32_t colour, float seconds);
    void TrailPoint(Handle handle, const glm::vec3& a, const glm::vec3& b);
    void EndTrail(Handle handle);

    // A picture laid on the ground for the next Update only (the original's CIODecal: where a click
    // sends the character, the ring under the selection): `cells` x `cells` plates over a grid of
    // (cells + 1)^2 `corners` (scene space, row by row, u along a row, v down the rows), each corner
    // on the ground under it, so the picture lies on slopes and floors and moves with the scene's own
    // camera. Alpha-blended, under the effects, whatever their quality.
    void GroundDecal(const std::string& texture, int cells, std::span<const glm::vec3> corners, std::uint32_t colour = 0xFFFFFFFFu);

    // Starts what loaded, places the anchored, steps the visible on the workers and writes the frame.
    void Update(double dt, const View& view, const Place& place, EffectFrame& out);

    struct Stats {
        std::size_t effects = 0, visible = 0, elements = 0, sprites = 0, meshInstances = 0, lights = 0;
        std::size_t programs = 0, loading = 0, failed = 0;
    };
    const Stats& GetStats() const { return stats_; }
    // Effects that could not be read since the last call ("path: why").
    std::vector<std::string> TakeFailures();

private:
    struct Program; // a library entry: the compiled program, its assets, its state
    struct Instance {
        Handle handle = 0;
        std::shared_ptr<Program> program;
        Request request;
        EffectSimulation simulation;
        EffectSimulation::Output output;
        glm::mat4 at{1.0f};
        float waited = 0.0f, age = 0.0f, distance = 0.0f;
        float releasedAt = -1.0f, opacity = 1.0f; // its age when released; the fades' product now
        bool started = false, placed = false, released = false, stopped = false, visible = false;
        // The light it gives as shown: eased toward each frame's (its particles' count and place change frame to
        // frame: taken as they are, the light flickered; 2026-11-13).
        EffectLight shownLight;
        bool lightShown = false;
    };
    std::shared_ptr<Program> Find(const std::string& key);
    void Resolve(Program& program);

    AssetStore* assets_ = nullptr;
    const assets::FileSystem* files_ = nullptr;
    core::JobSystem* jobs_ = nullptr;
    EffectLimits limits_;
    std::unordered_map<std::string, std::shared_ptr<Program>> library_;
    std::vector<std::unique_ptr<Instance>> instances_;
    std::unordered_map<Handle, Instance*> byHandle_;
    std::vector<Instance*> visible_;
    struct MeshItem {
        std::uint64_t key = 0; // blend, then the part: instances of one part in one blend draw together
        const ModelPartInfo* part = nullptr;
        EffectBlend blend = EffectBlend::Alpha;
        EffectMeshInstance instance;
    };
    std::vector<MeshItem> meshScratch_;
    struct Trail {
        Handle handle = 0;
        AssetId texture;
        std::uint32_t bindless = 0, colour = 0xFFFFFFFFu;
        bool ready = false, ended = false;
        float seconds = 0.2f;
        struct Point {
            glm::vec3 a{0.0f}, b{0.0f};
            double at = 0;
        };
        std::vector<Point> points;
    };
    std::vector<Trail> trails_;
    // The ground decals' textures (kept once asked for: a handful) and this frame's decals.
    struct DecalTexture {
        AssetId id;
        std::uint32_t bindless = 0;
        bool ready = false;
    };
    std::unordered_map<std::string, DecalTexture> decalTextures_;
    struct Decal {
        DecalTexture* texture = nullptr;
        int cells = 1;
        std::uint32_t colour = 0xFFFFFFFFu;
        std::vector<glm::vec3> corners;
    };
    std::vector<Decal> decals_;
    double clock_ = 0.0;
    Handle nextHandle_ = 1;
    int quality_ = 2;
    Stats stats_;
    std::vector<std::string> failures_;
};

} // namespace sr::gfx
