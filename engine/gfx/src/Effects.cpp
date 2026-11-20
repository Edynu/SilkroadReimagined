#include "sr/gfx/Effects.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

#include "sr/assets/FileSystem.h"
#include "sr/core/Path.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

constexpr float kDegrees = std::numbers::pi_v<float> / 180.0f;
constexpr int kMaxFrames = 1 << 16;
constexpr float kLoadPatience = 3.0f; // seconds an effect waits for its program before it is dropped
constexpr float kKeptPieces = 64.0f;  // an emitter of no more than this many is a kept effect's fixed pieces (ours)

bool None(std::string_view path) {
    const std::string key = core::NormalizeAssetPath(path);
    return key.empty() || key == "none" || key == "xxx";
}

// The ages a command fires at, by its schedule flags over a node of `t` frames: bit 0 the start in
// percent of the life, bit 1 the spacing in percent, bits 2.. how the end is given (an age, a
// percent, a length after the start, a percent counted back, a number of repeats).
std::vector<int> FiringFrames(const assets::EffectCommand& c, int t) {
    const int f = c.schedule;
    const double first = (f & 1) ? std::trunc(c.start / 100.0) * t : std::trunc(c.start);
    const double period = (f & 2) ? t * c.spacing / 100.0 : c.spacing;
    double last = 0;
    switch ((f >> 2) % 5) {
    case 0: last = std::trunc(c.end); break;
    case 1: last = std::trunc(c.end / 100.0) * t; break;
    case 2: last = first + std::trunc(c.end); break;
    case 3: last = first - std::trunc(c.end / -100.0) * t; break;
    default:
        if (c.end < 1.0f) return {};
        last = first + std::trunc(c.end * period);
        break;
    }
    last = std::min(last, static_cast<double>(t - 1));
    std::vector<int> out;
    if (period <= 0.0) {
        out.push_back(static_cast<int>(first));
        return out;
    }
    for (int i = 0; first + i * period <= last + 1e-6 && out.size() < 1024; ++i) {
        out.push_back(static_cast<int>(std::trunc(first + i * period)));
    }
    return out;
}

// A D3D row-major 4 x 4 (row vectors) as a column-major rotation, from `at` in the list.
glm::mat3 RowMajor(const std::vector<float>& v, std::size_t at = 0) {
    glm::mat3 m;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m[r][c] = v[at + static_cast<std::size_t>(r) * 4 + static_cast<std::size_t>(c)];
    }
    return m;
}

// The texture stages' colour and alpha operations (D3DTOP: 4 MODULATE, 5 MODULATE2X, 6 MODULATE4X).
std::uint32_t Modulate(const std::array<std::uint32_t, 6>& stages) {
    std::uint32_t flags = 0;
    if (stages[2] == 5) flags |= kEffectColour2x;
    if (stages[2] == 6) flags |= kEffectColour4x;
    if (stages[5] == 5) flags |= kEffectAlpha2x;
    if (stages[5] == 6) flags |= kEffectAlpha4x;
    return flags;
}

std::uint32_t Pack(const glm::vec4& c) {
    const auto b = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

// --- Ribbons (the 1.188 client's) -------------

// One element of a ribbon: where it is, its colour, half the strip's width there, and (LinkObj) the unit axis its
// two edges lie along.
struct RibbonPoint {
    int node = 0, parent = -1;
    glm::vec3 centre{0.0f}, axis{0.0f};
    glm::vec4 colour{1.0f};
    float size = 0.0f;
};

// The uniform cubic B-spline of four points at t (0..1 between the middle two).
glm::vec3 BSpline(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, float t) {
    const float s = 1.0f - t, t2 = t * t, t3 = t2 * t;
    return a * (s * s * s / 6.0f) + b * ((3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f) + c * ((-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f) +
           d * (t3 / 6.0f);
}

// A strip from its cross-sections (two edges, a colour and a u each) as triangles, six vertices a segment.
struct RibbonSection {
    glm::vec3 a{0.0f}, b{0.0f};
    glm::vec4 colour{1.0f};
    float u = 0.0f;
};
void EmitStrip(const std::vector<RibbonSection>& sections, std::uint32_t texture, std::uint32_t flags, std::vector<EffectRibbonVertex>& out) {
    if (sections.size() < 2) return;
    const auto vertex = [&](const glm::vec3& p, const RibbonSection& s, float v) {
        EffectRibbonVertex& r = out.emplace_back();
        r.position = p;
        r.colour = Pack(s.colour);
        r.uv = glm::vec2(s.u, v);
        r.texture = texture;
        r.flags = flags;
    };
    for (std::size_t i = 0; i + 1 < sections.size(); ++i) {
        const RibbonSection& s = sections[i];
        const RibbonSection& n = sections[i + 1];
        vertex(s.a, s, 0.0f);
        vertex(n.a, n, 0.0f);
        vertex(n.b, n, 1.0f);
        vertex(s.a, s, 0.0f);
        vertex(n.b, n, 1.0f);
        vertex(s.b, s, 1.0f);
    }
}

// One group's ribbon (its points oldest first, repeats dropped). The pipes face the camera: each point's side is
// across the line as the screen shows it (its neighbours' direction there), its edges that side times its size
// either way, u from 0 at the first to 1 at the last, v 0 and 1 on the edges. LinkPipe first pads the line with its
// end points and samples a uniform cubic B-spline three times a segment (colour and size between the segment's
// middle two points); LinkObj does the same with each point's two edges (its own up axis times its size), u from
// 1 at the first to 0 at the last.
void BuildRibbon(EffectProgram::Render render, std::vector<RibbonPoint>& points, const glm::vec3& right, const glm::vec3& up,
                 std::uint32_t texture, std::uint32_t flags, std::vector<RibbonSection>& sections, std::vector<EffectRibbonVertex>& out) {
    sections.clear();
    if (points.size() < 2) return;
    const std::size_t n = points.size();
    const auto at = [&](std::ptrdiff_t i) -> const RibbonPoint& { return points[std::size_t(std::clamp<std::ptrdiff_t>(i, 0, std::ptrdiff_t(n) - 1))]; };
    if (render == EffectProgram::Render::LinkObj) {
        // Both edges smoothed: n - 1 segments of three samples, then the last pair.
        const std::size_t samples = (n - 1) * 3;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const RibbonPoint &p0 = at(std::ptrdiff_t(i) - 1), &p1 = at(std::ptrdiff_t(i)), &p2 = at(std::ptrdiff_t(i) + 1), &p3 = at(std::ptrdiff_t(i) + 2);
            for (int k = 0; k < 3; ++k) {
                const float t = float(k) / 3.0f;
                RibbonSection& s = sections.emplace_back();
                s.a = BSpline(p0.centre + p0.axis * p0.size, p1.centre + p1.axis * p1.size, p2.centre + p2.axis * p2.size, p3.centre + p3.axis * p3.size, t);
                s.b = BSpline(p0.centre - p0.axis * p0.size, p1.centre - p1.axis * p1.size, p2.centre - p2.axis * p2.size, p3.centre - p3.axis * p3.size, t);
                s.colour = glm::mix(p1.colour, p2.colour, t);
                s.u = 1.0f - float(sections.size() - 1) / float(samples);
            }
        }
        const RibbonPoint& last = points.back();
        RibbonSection& s = sections.emplace_back();
        s.a = last.centre + last.axis * last.size;
        s.b = last.centre - last.axis * last.size;
        s.colour = last.colour;
        s.u = 0.0f;
        EmitStrip(sections, texture, flags, out);
        return;
    }
    // The line: as it is (LinkDPipe), or smoothed (LinkPipe).
    std::vector<RibbonPoint> smooth;
    const std::vector<RibbonPoint>* line = &points;
    if (render == EffectProgram::Render::LinkPipe) {
        smooth.reserve((n - 1) * 3 + 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const RibbonPoint &p0 = at(std::ptrdiff_t(i) - 1), &p1 = at(std::ptrdiff_t(i)), &p2 = at(std::ptrdiff_t(i) + 1), &p3 = at(std::ptrdiff_t(i) + 2);
            for (int k = 0; k < 3; ++k) {
                const float t = float(k) / 3.0f;
                RibbonPoint& q = smooth.emplace_back();
                q.centre = BSpline(p0.centre, p1.centre, p2.centre, p3.centre, t);
                q.colour = glm::mix(p1.colour, p2.colour, t);
                q.size = glm::mix(p1.size, p2.size, t);
            }
        }
        smooth.push_back(points.back());
        line = &smooth;
    }
    const std::vector<RibbonPoint>& l = *line;
    const std::size_t count = l.size();
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 along = i == 0 ? l[1].centre - l[0].centre : i + 1 == count ? l[i].centre - l[i - 1].centre : l[i + 1].centre - l[i - 1].centre;
        // Across the line on the screen: its direction in the camera's plane turned a quarter.
        glm::vec2 d(glm::dot(along, right), glm::dot(along, up));
        const float length = glm::length(d);
        d = length > 1e-6f ? d / length : glm::vec2(0.0f, 1.0f);
        const glm::vec3 side = (right * d.y - up * d.x) * l[i].size;
        RibbonSection& s = sections.emplace_back();
        s.a = l[i].centre + side;
        s.b = l[i].centre - side;
        s.colour = l[i].colour;
        s.u = float(i) / float(count - 1);
    }
    EmitStrip(sections, texture, flags, out);
}

// The node's longest run in ticks: its life, or its children's last births and theirs.
int Length(const EffectProgram& p, int index) {
    const EffectProgram::Node& n = p.nodes[static_cast<std::size_t>(index)];
    int longest = n.frames;
    for (const int c : n.children) {
        const EffectProgram::Node& child = p.nodes[static_cast<std::size_t>(c)];
        const float lastBirth = child.emitter ? std::min(child.emitStart + child.emitDuration, static_cast<float>(n.frames)) : 0.0f;
        longest = std::max(longest, static_cast<int>(lastBirth) + Length(p, c));
    }
    return std::min(longest, kMaxFrames);
}

// Six planes of a view-projection (Vulkan clip space, depth 0..1), inward.
struct Frustum {
    glm::vec4 planes[6];
    explicit Frustum(const glm::mat4& m) {
        const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]), r1(m[0][1], m[1][1], m[2][1], m[3][1]),
            r2(m[0][2], m[1][2], m[2][2], m[3][2]), r3(m[0][3], m[1][3], m[2][3], m[3][3]);
        planes[0] = r3 + r0;
        planes[1] = r3 - r0;
        planes[2] = r3 + r1;
        planes[3] = r3 - r1;
        planes[4] = r2;
        planes[5] = r3 - r2;
        for (glm::vec4& p : planes) p /= std::max(glm::length(glm::vec3(p)), 1e-6f);
    }
    bool Sphere(const glm::vec3& c, float r) const {
        for (const glm::vec4& p : planes) {
            if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
        }
        return true;
    }
};

} // namespace

EffectBlend EffectBlendOf(std::uint32_t srcBlend, std::uint32_t dstBlend) {
    // D3DBLEND: 2 ONE, 5 SRCALPHA, 6 INVSRCALPHA.
    if (dstBlend == 2) return EffectBlend::Additive;
    if (srcBlend == 2 && dstBlend == 6) return EffectBlend::Premultiplied;
    return EffectBlend::Alpha;
}

bool CompileEffect(const assets::EffectData& data, EffectProgram& out, std::string& error) {
    out = EffectProgram{};
    out.scale = data.scale;
    std::vector<const assets::EffectNode*> sources;
    const auto flatten = [&](auto& self, const assets::EffectNode& node, int parent, int slot) -> void {
        const int index = static_cast<int>(sources.size());
        sources.push_back(&node);
        out.nodes.emplace_back();
        out.nodes.back().parent = parent;
        out.nodes.back().childSlot = slot;
        if (parent >= 0) out.nodes[static_cast<std::size_t>(parent)].children.push_back(index);
        for (std::size_t i = 0; i < node.children.size(); ++i) self(self, node.children[i], index, static_cast<int>(i));
    };
    flatten(flatten, data.root, -1, 0);
    if (out.nodes.size() > 0xFFFF) {
        error = "too many effect nodes";
        return false;
    }
    const auto listed = [](std::vector<std::string>& list, const std::string& path) {
        const auto it = std::find(list.begin(), list.end(), path);
        if (it != list.end()) return static_cast<int>(it - list.begin());
        list.push_back(path);
        return static_cast<int>(list.size()) - 1;
    };
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const assets::EffectNode& s = *sources[i];
        EffectProgram::Node& n = out.nodes[i];
        n.frames = std::clamp(s.frames, 1, kMaxFrames);
        n.loops = s.Loops();
        n.endless = s.life.name == "NeverExtinct";
        n.positionDepth = std::clamp(s.positionDepth, 0, 64);
        n.matrixDepth = std::clamp(s.matrixDepth, 0, 64);
        n.velocityDepth = std::clamp(s.velocityDepth, 0, 64);
        n.followDepth = std::clamp(s.followDepth, 0, 64);
        n.localMotion = s.localMotion;
        n.shapeMotion = s.shapeMotion;
        if (const assets::EffectCommand* emit = assets::EffectNode::Find(s.emitterProgram, "StaticEmit");
            emit != nullptr && emit->values.size() >= 5) {
            n.emitter = true;
            n.emitStart = emit->values[0];
            n.emitDuration = emit->values[1];
            n.emitPeriod = emit->values[2];
            n.emitLimit = emit->values[3];
            n.emitRate = emit->values[4];
        }
        // The render program: its tracks, and its commands by the frames they fire at.
        std::vector<std::vector<std::uint16_t>> byFrame(static_cast<std::size_t>(n.frames));
        for (const assets::EffectCommand& c : s.renderProgram) {
            const std::vector<float>& v = c.values;
            if (c.name == "SetGraphDiffuse") {
                n.colours = c.colors;
                continue;
            }
            if (c.name == "SetGraphScale") {
                for (std::size_t r = 0; r + 2 < v.size(); r += 3) n.sizes.emplace_back(v[r], v[r + 1], v[r + 2]);
                continue;
            }
            if (c.name == "TextureSlide") {
                for (std::size_t r = 3; r + 3 < v.size(); r += 4) n.windows.emplace_back(v[r], v[r + 1], v[r + 2], v[r + 3]);
                continue;
            }
            EffectProgram::Command command;
            command.flags = c.flags;
            const auto vec = [&](std::size_t at) { return v.size() >= at + 3 ? glm::vec3(v[at], v[at + 1], v[at + 2]) : glm::vec3(0.0f); };
            const std::string& name = c.name;
            using Op = EffectProgram::Op;
            // The flags as the original's opcodes take them (12 places, 3 spheres, 4 frames).
            const std::uint8_t f = c.flags;
            const auto placed = [&] {
                command.base = static_cast<std::uint8_t>(f % 3);
                command.frame = static_cast<std::uint8_t>(std::min(f / 3, 3));
            };
            const auto framed = [&] { command.frame = static_cast<std::uint8_t>(std::min<int>(f, 3)); };
            if (name == "SetPosition") {
                command.op = Op::Position;
                command.value = vec(0);
                placed();
            } else if (name == "SetBANPos" && v.size() >= 4) {
                command.op = Op::KeyedPosition;
                for (std::size_t r = 1; r + 2 < v.size(); r += 3) command.positions.emplace_back(v[r], v[r + 1], v[r + 2]);
                placed();
            } else if (name == "SetSpherePos") {
                command.op = Op::SpherePosition;
                command.value = vec(0);
                command.base = static_cast<std::uint8_t>(std::min<int>(f, 2));
            } else if (name == "SetConePos" || name == "SetConeVel" || name == "ConeForce") {
                command.op = name == "SetConePos" ? Op::ConePosition : name == "SetConeVel" ? Op::ConeVelocity : Op::ConeForce;
                command.value = vec(0);
                if (command.op == Op::ConePosition) {
                    placed();
                } else {
                    framed();
                }
            } else if (name == "SetVelocity" || name == "Force") {
                command.op = name == "SetVelocity" ? Op::Velocity : Op::Force;
                command.value = vec(0);
                framed();
            } else if (name == "Attraction" && !v.empty()) {
                command.op = Op::Attraction;
                command.amount = v[0];
                command.base = static_cast<std::uint8_t>(std::min<int>(f, 1) + 1); // towards the parent, or the top ancestor
            } else if (name == "SetGraphRandomScale") {
                command.op = Op::RandomScale;
            } else if ((name == "SetRotationMat" || name == "SetRVelocityMat") && v.size() >= 16) {
                // The rotations carry their matrix after their angles: the turn the original uses.
                command.op = name == "SetRotationMat" ? Op::Turn : Op::Spin;
                command.turn = RowMajor(v);
                framed();
            } else if ((name == "SetRotation" || name == "SetRVelocity") && v.size() >= 3 + 16) {
                command.op = name == "SetRotation" ? Op::Turn : Op::Spin;
                command.turn = RowMajor(v, 3);
                framed();
            } else if ((name == "SetRotationAxis" || name == "SetRVelocityAxis" || name == "SetShapeRot" || name == "SetShapeRotVel") &&
                       v.size() >= 4 + 16) {
                command.op = name == "SetRotationAxis" ? Op::Turn : name == "SetRVelocityAxis" ? Op::Spin : name == "SetShapeRot" ? Op::Shape : Op::ShapeSpin;
                command.turn = RowMajor(v, 4);
                framed();
            } else if (name == "SetBANRot" && v.size() >= 1 + 16) {
                command.op = Op::KeyedTurn;
                for (std::size_t r = 1; r + 15 < v.size(); r += 16) command.turns.push_back(RowMajor(v, r));
                framed();
            } else {
                continue; // a command with nothing to do here
            }
            const auto index = static_cast<std::uint16_t>(n.commands.size());
            n.commands.push_back(command);
            for (const int frame : FiringFrames(c, n.frames)) {
                if (frame >= 0 && frame < n.frames) byFrame[static_cast<std::size_t>(frame)].push_back(index);
            }
        }
        n.firing.reserve(byFrame.size() + 1);
        for (const auto& frame : byFrame) {
            n.firing.push_back(static_cast<std::uint32_t>(n.firingCommands.size()));
            n.firingCommands.insert(n.firingCommands.end(), frame.begin(), frame.end());
        }
        n.firing.push_back(static_cast<std::uint32_t>(n.firingCommands.size()));
        if (!s.scaleGraphs.empty()) n.randomScale = s.scaleGraphs.front().points;

        n.render = s.render.name == "RenderPlate"       ? EffectProgram::Render::Plate
                   : s.render.name == "RenderMesh"      ? EffectProgram::Render::Mesh
                   : s.render.name == "RenderLinkDPipe" ? EffectProgram::Render::LinkDPipe
                   : s.render.name == "RenderLinkPipe"  ? EffectProgram::Render::LinkPipe
                   : s.render.name == "RenderLinkObj"   ? EffectProgram::Render::LinkObj
                                                        : EffectProgram::Render::None;
        n.view = s.view.name == "ViewBillboard"    ? EffectProgram::View::Camera
                 : s.view.name == "ViewYBillboard" ? EffectProgram::View::Upright
                 : s.view.name == "ViewVBillboard" ? EffectProgram::View::Velocity
                                                   : EffectProgram::View::Own;
        if (!s.resource.meshes.empty()) {
            const auto& mesh = s.resource.meshes.front();
            if (!mesh.textures.empty() && !None(mesh.textures.front())) n.texture = listed(out.textures, mesh.textures.front());
            if (n.render == EffectProgram::Render::Mesh && !None(mesh.path)) n.mesh = listed(out.meshes, mesh.path);
        }
        if (n.render == EffectProgram::Render::Mesh && n.mesh < 0) n.render = EffectProgram::Render::None;
        n.blend = EffectBlendOf(s.resource.srcBlend, s.resource.dstBlend);
        n.sourceOne = s.resource.srcBlend == 2;
        n.glows = s.resource.dstBlend == 2 || n.sourceOne;
        n.modulate = Modulate(s.resource.stages);
    }
    const EffectProgram::Node& root = out.nodes.front();
    const int rootEnd = root.emitter ? static_cast<int>(root.emitStart + root.emitDuration) : 0;
    out.lengthTicks = std::min(rootEnd, kMaxFrames) + Length(out, 0);
    return true;
}

// --- The simulation ---------------------------------------------------------------------------

float EffectSimulation::Random() {
    // A linear congruential sequence of 15 bits, as the CRT's rand().
    random_ = random_ * 214013u + 2531011u;
    return static_cast<float>((random_ >> 16) & 0x7FFF) / 32767.0f;
}

void EffectSimulation::Start(const EffectProgram& program, std::uint32_t seed, bool kept) {
    program_ = &program;
    elements_.clear();
    emitted_.clear();
    alive_ = 0;
    rootEmitted_ = 0.0f;
    ticks_ = 0;
    tickTime_ = 0.0f;
    lod_ = 1.0f;
    random_ = seed != 0 ? seed : 1u;
    kept_ = kept;
    releasing_ = false;
    if (!program.nodes.empty()) EmitRoots(); // tick 0
}

bool EffectSimulation::Loops(const Element& e, const EffectProgram::Node& node) const {
    if (releasing_) return false;
    if (node.loops) return true;
    // A kept effect (an object's glow, an aura) keeps its fixed pieces going: the root and every
    // piece born with its parent rather than emitted, so nothing fades and waits for the next round.
    // A fixed piece goes on only while the element that made it lives: one made by an emitted piece
    // (each round's) ends with its round, else every round adds another that never goes (the rare
    // bow's system_rarebow_c.efp grew without end). Judged by its node, not its parent index: Compact
    // leaves an orphan with parent -1, which is no root.
    if (!kept_) return false;
    if (e.node == 0) return true;
    if (node.emitter || e.parent < 0) return false;
    return elements_[static_cast<std::size_t>(e.parent)].alive;
}

const EffectSimulation::Element* EffectSimulation::Ancestor(const Element& e, int depth) const {
    const Element* at = &e;
    for (int i = 0; i < depth; ++i) {
        if (at->parent < 0) return nullptr; // past the root: the effect's own place
        at = &elements_[static_cast<std::size_t>(at->parent)];
        if (!at->alive) return nullptr;
    }
    return at;
}

const EffectSimulation::Element& EffectSimulation::Top(const Element& e) const {
    const Element* at = &e;
    while (at->parent >= 0 && elements_[static_cast<std::size_t>(at->parent)].alive) at = &elements_[static_cast<std::size_t>(at->parent)];
    return *at;
}

void EffectSimulation::Carry(Element& e, const EffectProgram::Node& node) {
    // The original moves every element by the motion of the ancestors its node names, each tick: its place turned
    // about one (positionDepth) and moved
    // with another (followDepth), its turn and velocity turned with others. An ancestor past the root
    // is the effect's own place, which does not move in these coordinates (the place is put on them
    // as they are drawn). Ancestors come first in the list, so theirs is this tick's motion.
    const auto delta = [](const Element& a) { return a.turn * glm::transpose(a.previousTurn); };
    if (const Element* p = node.positionDepth > 0 ? Ancestor(e, node.positionDepth) : nullptr; p != nullptr && p->loose == e.loose) {
        e.position = p->previous + delta(*p) * (e.position - p->previous);
    }
    if (const Element* f = node.followDepth > 0 ? Ancestor(e, node.followDepth) : nullptr; f != nullptr && f->loose == e.loose) {
        e.position += f->position - f->previous;
    }
    if (const Element* m = node.matrixDepth > 0 ? Ancestor(e, node.matrixDepth) : nullptr) e.turn = delta(*m) * e.turn;
    if (const Element* v = node.velocityDepth > 0 ? Ancestor(e, node.velocityDepth) : nullptr) e.velocity = delta(*v) * e.velocity;
}

const EffectSimulation::Element* EffectSimulation::Previous(const Element& e, std::size_t before) const {
    for (std::size_t i = std::min(before, elements_.size()); i-- > 0;) {
        const Element& p = elements_[i];
        if (p.alive && p.node == e.node && p.parent == e.parent && &p != &e) return &p;
    }
    return nullptr;
}

void EffectSimulation::Run(Element& e, int frame, std::size_t before) {
    const EffectProgram::Node& node = program_->nodes[static_cast<std::size_t>(e.node)];
    if (frame < 0 || frame >= node.frames) return;
    const std::uint32_t begin = node.firing[static_cast<std::size_t>(frame)], end = node.firing[static_cast<std::size_t>(frame) + 1];
    if (begin == end) return;
    using Op = EffectProgram::Op;
    // Its parent as it is now (a gone parent: where it was when this was born), and the previous element of its group
    // (the "third" place and turn of the commands; a command that names it does nothing on the group's first element).
    const Element* parent = e.parent >= 0 && elements_[static_cast<std::size_t>(e.parent)].alive ? &elements_[static_cast<std::size_t>(e.parent)] : nullptr;
    const Element* previousElement = Previous(e, before);
    // A loose element reads an ancestor still on the effect's place in its own coordinates.
    const auto own = [&](const Element& a) { return e.loose && !a.loose ? glm::inverse(e.space) * space_ : glm::mat4(1.0f); };
    const auto frameTurn = [&](std::uint8_t f) -> glm::mat3 {
        const Element* a = f == 2 ? parent : f == 3 ? previousElement : nullptr;
        if (f == 1) return e.turn;
        if (a == nullptr) return glm::mat3(1.0f);
        if (!e.loose || a->loose) return a->turn;
        const glm::mat3 m(own(*a));
        return m * a->turn / std::max(glm::length(m[0]), 1e-6f);
    };
    const auto basePosition = [&](std::uint8_t b) -> glm::vec3 {
        const Element* a = b == 1 ? parent : b == 2 ? previousElement : nullptr;
        if (b == 0) return e.position; // adds to its own place
        if (a == nullptr) return e.origin;
        return glm::vec3(own(*a) * glm::vec4(a->position, 1.0f));
    };
    for (std::uint32_t k = begin; k < end; ++k) {
        const EffectProgram::Command& c = node.commands[node.firingCommands[k]];
        const auto cone = [&] {
            const float speed = c.value.x + Random() * (c.value.y - c.value.x);
            const float polar = Random() * c.value.z * kDegrees;
            const float azimuth = Random() * 2.0f * std::numbers::pi_v<float>;
            return glm::vec3(-std::sin(polar) * speed * std::cos(azimuth), std::cos(polar) * speed, -std::sin(polar) * speed * std::sin(azimuth));
        };
        // A keyed command's key at this frame (its keys spread over the element's life).
        const auto key = [&](std::size_t keys) { return std::min(keys - 1, static_cast<std::size_t>(frame) * keys / static_cast<std::size_t>(node.frames)); };
        // Naming the previous element (a place from it, or its turn) on the group's first: nothing.
        if (previousElement == nullptr) {
            const bool placed = c.op == Op::Position || c.op == Op::KeyedPosition || c.op == Op::SpherePosition || c.op == Op::ConePosition ||
                                c.op == Op::Attraction;
            if ((placed && c.base == 2) || c.frame == 3) continue;
        }
        switch (c.op) {
        case Op::Position: e.position = basePosition(c.base) + frameTurn(c.frame) * c.value; break;
        case Op::KeyedPosition:
            if (!c.positions.empty()) e.position = basePosition(c.base) + frameTurn(c.frame) * c.positions[key(c.positions.size())];
            break;
        case Op::SpherePosition: {
            glm::vec3 p(0.0f);
            for (int tries = 0; tries < 16; ++tries) {
                p = glm::vec3(Random() - 0.5f, Random() - 0.5f, Random() - 0.5f) * 2.0f;
                if (glm::dot(p, p) <= 1.0f) break;
            }
            e.position = basePosition(c.base) + p * c.value;
            break;
        }
        case Op::ConePosition: e.position = basePosition(c.base) + frameTurn(c.frame) * cone(); break;
        case Op::ConeVelocity: e.velocity = frameTurn(c.frame) * cone(); break;
        case Op::ConeForce: e.velocity += frameTurn(c.frame) * cone(); break;
        case Op::Velocity: e.velocity = frameTurn(c.frame) * c.value; break;
        case Op::Force: e.velocity += frameTurn(c.frame) * c.value; break;
        case Op::Attraction: {
            const glm::vec3 d = basePosition(c.base) - e.position;
            const float length = glm::length(d);
            if (length > 1e-4f) e.velocity += d * c.amount / length;
            break;
        }
        case Op::RandomScale: {
            const auto& points = node.randomScale;
            if (points.empty()) break;
            const float r = Random();
            std::array<float, 3> s = points.back().value;
            for (std::size_t i = 1; i < points.size(); ++i) {
                if (r > points[i].t) continue;
                const float span = points[i].t - points[i - 1].t;
                const float w = span > 0.0f ? (r - points[i - 1].t) / span : 0.0f;
                for (int k3 = 0; k3 < 3; ++k3) s[k3] = points[i - 1].value[k3] + (points[i].value[k3] - points[i - 1].value[k3]) * w;
                break;
            }
            e.randomScale = glm::vec3(s[0], s[1], s[2]);
            break;
        }
        case Op::Turn: e.turn = c.frame == 0 ? c.turn : frameTurn(c.frame) * c.turn; break;
        case Op::KeyedTurn:
            if (!c.turns.empty()) {
                const glm::mat3& t = c.turns[key(c.turns.size())];
                e.turn = c.frame == 0 ? t : frameTurn(c.frame) * t;
            }
            break;
        case Op::Spin: e.spin = c.turn; break;
        case Op::Shape: e.shape = c.turn; break;
        case Op::ShapeSpin: e.shapeSpin = c.turn; break;
        }
    }
}

void EffectSimulation::Birth(int node, int parent) {
    if (alive_ >= kMaxElements) return;
    Element e;
    e.node = node;
    e.parent = parent;
    const EffectProgram::Node& n = program_->nodes[static_cast<std::size_t>(node)];
    if (parent >= 0) {
        // Born where its parent is, turned as it is, with its velocity less that of the ancestor it
        // follows (the original's element start).
        const Element& p = elements_[static_cast<std::size_t>(parent)];
        e.origin = e.position = p.position;
        e.turn = p.turn;
        e.velocity = p.velocity;
        if (n.followDepth > 0) {
            if (const Element* f = Ancestor(e, n.followDepth)) e.velocity -= f->velocity;
        }
        // In the world from now on when nothing carries it, or when its parent already is.
        e.loose = p.loose || (n.positionDepth == 0 && n.followDepth == 0);
        e.space = p.loose ? p.space : space_;
    } else {
        e.loose = n.positionDepth == 0 && n.followDepth == 0;
        e.space = space_;
    }
    e.emitted = static_cast<std::uint32_t>(emitted_.size());
    emitted_.resize(emitted_.size() + program_->nodes[static_cast<std::size_t>(node)].children.size(), 0.0f);
    Run(e, 0, elements_.size());
    e.previous = e.position;
    elements_.push_back(e);
    ++alive_;
    Emit(elements_.size() - 1); // children due at age 0 come at once
}

void EffectSimulation::EmitRoots() {
    const EffectProgram::Node& root = program_->nodes.front();
    if (!root.emitter) {
        if (ticks_ == 0 && rootEmitted_ == 0.0f) {
            rootEmitted_ = 1.0f;
            Birth(0, -1);
        }
        return;
    }
    if (releasing_) return;
    const float age = static_cast<float>(ticks_) - root.emitStart;
    if (age < 0.0f || age >= root.emitDuration || root.emitPeriod <= 0.0f || std::fmod(age, root.emitPeriod) != 0.0f) return;
    const float next = std::min(root.emitLimit, rootEmitted_ + root.emitRate);
    const int births = static_cast<int>(std::trunc(next)) - static_cast<int>(std::trunc(rootEmitted_));
    rootEmitted_ = next;
    for (int b = 0; b < births; ++b) Birth(0, -1);
}

void EffectSimulation::Emit(std::size_t index) {
    // Each child node's StaticEmit, counted on this element's age.
    const auto& children = program_->nodes[static_cast<std::size_t>(elements_[index].node)].children;
    for (std::size_t c = 0; c < children.size(); ++c) {
        const EffectProgram::Node& child = program_->nodes[static_cast<std::size_t>(children[c])];
        if (releasing_ && child.emitter) continue;
        const Element& e = elements_[index];
        float& total = emitted_[e.emitted + c];
        if (!child.emitter) {
            // One element, born with its parent (a fixed piece: never thinned by distance).
            if (e.age == 0 && total == 0.0f) {
                total = 1.0f;
                Birth(children[c], static_cast<int>(index));
            }
            continue;
        }
        const float age = static_cast<float>(e.age) - child.emitStart;
        if (age < 0.0f || age >= child.emitDuration || child.emitPeriod <= 0.0f || std::fmod(age, child.emitPeriod) != 0.0f) continue;
        const float next = std::min(child.emitLimit, total + child.emitRate);
        const int births = static_cast<int>(std::trunc(next)) - static_cast<int>(std::trunc(total));
        total = next;
        for (int b = 0; b < births; ++b) {
            // Far away, a share of the births only (they still count, so nothing catches up later);
            // but not of the few pieces a kept effect makes once and keeps (a monster's glow: one
            // first seen from afar would stay without them when it comes close).
            if (lod_ < 1.0f && !(kept_ && child.emitLimit <= kKeptPieces) && Random() > lod_) continue;
            Birth(children[c], static_cast<int>(index));
        }
    }
}

void EffectSimulation::Tick() {
    // First every element ages, starts over or dies (a death gives its place back to its parent's
    // emitter); only then do the living run their commands and emit. A parent starting over thus
    // finds the places its last round's children left this very tick, and nothing goes dark between.
    const std::size_t count = elements_.size();
    for (std::size_t i = 0; i < count; ++i) {
        Element& e = elements_[i];
        if (!e.alive) continue;
        const EffectProgram::Node& node = program_->nodes[static_cast<std::size_t>(e.node)];
        e.previous = e.position;
        e.previousTurn = e.turn;
        e.turn = e.turn * e.spin;
        if (node.shapeMotion) e.shape = e.shape * e.shapeSpin;
        e.position += e.velocity;
        if (++e.age < node.frames) continue;
        // NeverExtinct keeps an element; its clock still goes round (its commands repeat).
        if (Loops(e, node) || (node.endless && !releasing_)) {
            e.age = 0;
            continue;
        }
        e.alive = false;
        --alive_;
        if (e.parent >= 0) {
            emitted_[elements_[static_cast<std::size_t>(e.parent)].emitted + static_cast<std::size_t>(node.childSlot)] -= 1.0f;
        } else {
            rootEmitted_ -= 1.0f;
        }
    }
    ++ticks_;
    EmitRoots();
    for (std::size_t i = 0; i < count; ++i) {
        Element& e = elements_[i];
        if (!e.alive) continue;
        Carry(e, program_->nodes[static_cast<std::size_t>(e.node)]);
        Run(e, e.age, i);
        Emit(i);
    }
    Compact();
}

void EffectSimulation::Compact() {
    // Drops the dead now and then, keeping parents' indices and the emitters' totals right.
    const std::size_t dead = elements_.size() - alive_;
    if (dead <= 256 || dead * 2 <= elements_.size()) return;
    std::vector<int> remap(elements_.size(), -1);
    std::size_t kept = 0;
    std::vector<float> totals;
    totals.reserve(emitted_.size());
    for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i].alive) continue;
        remap[i] = static_cast<int>(kept);
        Element e = elements_[i];
        const std::size_t children = program_->nodes[static_cast<std::size_t>(e.node)].children.size();
        const std::uint32_t at = static_cast<std::uint32_t>(totals.size());
        totals.insert(totals.end(), emitted_.begin() + e.emitted, emitted_.begin() + e.emitted + static_cast<std::ptrdiff_t>(children));
        e.emitted = at;
        elements_[kept++] = e;
    }
    elements_.resize(kept);
    for (Element& e : elements_) e.parent = e.parent >= 0 ? remap[static_cast<std::size_t>(e.parent)] : -1;
    emitted_ = std::move(totals);
}

void EffectSimulation::Advance(float dt, float lod) {
    if (program_ == nullptr || program_->nodes.empty()) return;
    lod_ = std::clamp(lod, 0.0f, 1.0f);
    tickTime_ += dt;
    for (int ticks = 0; tickTime_ >= kTick && ticks < 8; ++ticks) {
        Tick();
        tickTime_ -= kTick;
    }
    tickTime_ = std::min(tickTime_, kTick);
}

bool EffectSimulation::Done() const {
    if (program_ == nullptr || program_->nodes.empty()) return true;
    const EffectProgram::Node& root = program_->nodes.front();
    const bool rootsPending = root.emitter && !releasing_ && static_cast<float>(ticks_) - root.emitStart < root.emitDuration;
    return !rootsPending && alive_ == 0;
}

void EffectSimulation::Build(const glm::mat4& at, float scale, const Look& look, const glm::mat3& camera, std::size_t budget,
                             Output& out, float pull, float opacity) const {
    if (program_ == nullptr) return;
    const glm::vec3 right = camera[0], up = camera[1], forward = camera[2];
    const glm::mat4 current = at * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    const float frac = std::clamp(tickTime_ / kTick, 0.0f, 1.0f);
    const glm::vec3 level = glm::length(glm::vec2(right.x, right.z)) > 1e-4f ? glm::normalize(glm::vec3(right.x, 0.0f, right.z)) : glm::vec3(1, 0, 0);
    // The light it gives: its glowing pieces, by colour and brightness.
    glm::vec3 glowColour(0.0f), glowCentre(0.0f), glowLow(1e9f), glowHigh(-1e9f);
    float glowWeight = 0.0f, glowSize = 0.0f;
    std::size_t made = 0;
    // The ribbon nodes' elements, gathered and strung after the rest (their look per node: texture and flags).
    thread_local std::vector<RibbonPoint> ribbonPoints;
    thread_local std::vector<RibbonPoint> ribbonGroup;
    thread_local std::vector<RibbonSection> ribbonSections;
    ribbonPoints.clear();
    for (const Element& e : elements_) {
        if (!e.alive) continue;
        const EffectProgram::Node& node = program_->nodes[static_cast<std::size_t>(e.node)];
        if (node.render == EffectProgram::Render::None || made >= budget) continue;
        // Where its coordinates are: the effect's place now, or where it was when this one was let go.
        const glm::mat4& space = e.loose ? e.space : current;
        // A ONE source adds its colour as it is; in the premultiplied blend the pipeline does that already.
        std::uint32_t texture = 0, flags = (node.sourceOne && node.blend == EffectBlend::Additive ? kEffectSourceOne : 0u) | node.modulate;
        if (node.texture >= 0) {
            const auto t = static_cast<std::size_t>(node.texture);
            if ((look.textureFlags[t] & Look::kReady) == 0) continue; // not in yet (or failed): not drawn
            texture = look.textures[t];
            if ((look.textureFlags[t] & Look::kPremultiplied) != 0) flags |= kEffectPremultiplied;
        }
        if (node.render == EffectProgram::Render::Mesh && look.meshReady[static_cast<std::size_t>(node.mesh)] == 0) continue;
        // The colour of this frame, held for the whole tick (an element shows its last row to its end).
        glm::vec4 colour(1.0f);
        // A track's rows spread over the element's life (a row a frame, or a row a percent of it when
        // its command fires by percent: 97 to 100 rows on a node of 30 frames).
        const float lived = (static_cast<float>(std::min(e.age, node.frames - 1)) + frac) / static_cast<float>(std::max(1, node.frames));
        if (!node.colours.empty()) {
            const int rows = static_cast<int>(node.colours.size());
            const auto row = static_cast<std::size_t>(std::min(rows - 1, std::min(e.age, node.frames - 1) * rows / std::max(1, node.frames)));
            const auto& rgba = node.colours[row];
            colour = glm::vec4(rgba[0], rgba[1], rgba[2], rgba[3]) / 255.0f;
        }
        if (opacity < 1.0f) {
            colour.a *= opacity;
            if (node.sourceOne) colour = glm::vec4(glm::vec3(colour) * opacity, colour.a);
        }
        const bool ribbon = node.render == EffectProgram::Render::LinkDPipe || node.render == EffectProgram::Render::LinkPipe ||
                            node.render == EffectProgram::Render::LinkObj;
        // (A ribbon keeps its faded points: the strip runs through every element of its group.)
        if (!ribbon && (node.sourceOne ? std::max({colour.r, colour.g, colour.b}) <= 0.002f : colour.a <= 0.002f)) continue;
        glm::vec3 size(1.0f);
        if (!node.sizes.empty()) {
            const int last = static_cast<int>(node.sizes.size()) - 1;
            const float at = lived * static_cast<float>(node.sizes.size());
            const int row = std::min(static_cast<int>(at), last);
            size = glm::mix(node.sizes[static_cast<std::size_t>(row)], node.sizes[static_cast<std::size_t>(std::min(row + 1, last))],
                            std::clamp(at - static_cast<float>(row), 0.0f, 1.0f));
        }
        size *= e.randomScale * scale;
        glm::vec4 window(0.0f, 0.0f, 1.0f, 1.0f);
        if (!node.windows.empty()) {
            const int rows = static_cast<int>(node.windows.size());
            window = node.windows[static_cast<std::size_t>(std::min(rows - 1, e.age * rows / std::max(1, node.frames)))];
        }
        const glm::vec3 centre = glm::vec3(space * glm::vec4(glm::mix(e.previous, e.position, frac), 1.0f)) - forward * pull;
        // The element's axes in the world.
        glm::vec3 ax, ay, az;
        switch (node.view) {
        case EffectProgram::View::Camera:
            ax = right;
            ay = up;
            az = forward;
            break;
        case EffectProgram::View::Upright:
            ax = level;
            ay = glm::vec3(0, 1, 0);
            az = glm::cross(ax, ay);
            break;
        case EffectProgram::View::Velocity: {
            const glm::vec3 v = glm::mat3(space) * e.velocity;
            if (glm::length(v) > 1e-5f) {
                ay = glm::normalize(v);
                const glm::vec3 side = glm::cross(ay, forward);
                ax = glm::length(side) > 1e-5f ? glm::normalize(side) : right;
                az = glm::cross(ax, ay);
                break;
            }
            [[fallthrough]];
        }
        case EffectProgram::View::Own: {
            const glm::mat3 m = (e.loose ? glm::mat3(e.space) / std::max(scale, 1e-6f) : glm::mat3(at)) * e.turn;
            ax = m[0];
            ay = m[1];
            az = m[2];
            break;
        }
        }
        // Its shape's own turn (SetShapeRot, SetShapeRotVel) inside that: a spinning plate, a turning mesh.
        if (e.shape != glm::mat3(1.0f)) {
            const glm::mat3 m = glm::mat3(ax, ay, az) * e.shape;
            ax = m[0];
            ay = m[1];
            az = m[2];
        }
        ax *= size.x;
        ay *= size.y;
        az *= size.z;
        if (node.glows) {
            const float w = colour.a * std::max({colour.r, colour.g, colour.b});
            if (w > 0.01f) {
                glowColour += glm::vec3(colour) * w;
                glowCentre += centre * w;
                glowWeight += w;
                glowLow = glm::min(glowLow, centre);
                glowHigh = glm::max(glowHigh, centre);
                glowSize = std::max({glowSize, glm::length(ax), glm::length(ay)});
            }
        }
        ++made;
        if (ribbon) {
            RibbonPoint& p = ribbonPoints.emplace_back();
            p.node = e.node;
            p.parent = e.parent;
            p.centre = centre;
            p.colour = colour;
            p.size = glm::length(ax);
            const float ly = glm::length(ay);
            p.axis = ly > 1e-6f ? ay / ly : up;
            continue;
        }
        if (node.render == EffectProgram::Render::Plate) {
            EffectSprite& s = out.sprites[static_cast<std::size_t>(node.blend)].emplace_back();
            s.centre = centre;
            s.colour = Pack(colour);
            s.axisX = ax;
            s.texture = texture;
            s.axisY = ay;
            s.flags = flags;
            s.window = window;
        } else {
            MeshItem& m = out.meshes.emplace_back();
            m.mesh = node.mesh;
            m.blend = node.blend;
            m.instance.rows[0] = glm::vec4(ax.x, ay.x, az.x, centre.x);
            m.instance.rows[1] = glm::vec4(ax.y, ay.y, az.y, centre.y);
            m.instance.rows[2] = glm::vec4(ax.z, ay.z, az.z, centre.z);
            m.instance.colour = Pack(colour);
            m.instance.texture = texture;
            m.instance.flags = flags | (node.texture >= 0 ? 0u : kEffectOwnTexture);
            m.instance.window = window;
        }
    }
    // The ribbons: each group (a node's elements by the one that made them) oldest first, repeats dropped.
    std::stable_sort(ribbonPoints.begin(), ribbonPoints.end(),
                     [](const RibbonPoint& a, const RibbonPoint& b) { return a.node != b.node ? a.node < b.node : a.parent < b.parent; });
    for (std::size_t i = 0; i < ribbonPoints.size();) {
        std::size_t j = i;
        ribbonGroup.clear();
        for (; j < ribbonPoints.size() && ribbonPoints[j].node == ribbonPoints[i].node && ribbonPoints[j].parent == ribbonPoints[i].parent; ++j) {
            const RibbonPoint& p = ribbonPoints[j];
            if (!ribbonGroup.empty() && glm::all(glm::lessThanEqual(glm::abs(ribbonGroup.back().centre - p.centre), glm::vec3(1e-6f)))) continue;
            ribbonGroup.push_back(p);
        }
        const EffectProgram::Node& node = program_->nodes[static_cast<std::size_t>(ribbonPoints[i].node)];
        std::uint32_t texture = 0, flags = (node.sourceOne && node.blend == EffectBlend::Additive ? kEffectSourceOne : 0u) | node.modulate;
        if (node.texture >= 0) {
            const auto t = static_cast<std::size_t>(node.texture);
            texture = look.textures[t];
            if ((look.textureFlags[t] & Look::kPremultiplied) != 0) flags |= kEffectPremultiplied;
        }
        BuildRibbon(node.render, ribbonGroup, right, up, texture, flags, ribbonSections, out.ribbons[static_cast<std::size_t>(node.blend)]);
        i = j;
    }
    if (glowWeight > 0.05f) {
        out.lit = true;
        out.light.position = glowCentre / glowWeight;
        const glm::vec3 c = glowColour / glowWeight;
        const float bright = std::max({c.r, c.g, c.b, 1e-3f});
        out.light.colour = c / bright;
        // A quarter softer than at first (2026-11-13, the user: "less strong"; halving it and shortening its reach left
        // the effects with almost no light): the reach as it was.
        out.light.intensity = std::clamp(glowWeight * 0.11f, 0.11f, 0.9f) * bright;
        out.light.radius = std::clamp(glm::length(glowHigh - glowLow) * 0.5f + glowSize * 2.0f + 30.0f, 50.0f, 400.0f);
    }
}

// --- The system -------------------------------------------------------------------------------

struct EffectSystem::Program {
    std::string path;
    // Written by the load job under the mutex, then published by `state` (0 loading, 1 ready, 2 failed).
    std::atomic<int> state{0};
    std::mutex mutex;
    bool abandoned = false; // the system shut down: the job gives back what it asked for
    std::shared_ptr<const EffectProgram> program;
    std::string error;
    std::vector<AssetId> textureIds, meshIds;
    // The game thread's, once ready.
    EffectSimulation::Look look;
    std::vector<std::shared_ptr<const ModelInfo>> meshes;
    bool resolved = false, reported = false;
};

void EffectSystem::Init(AssetStore& assets, const assets::FileSystem& files, core::JobSystem& jobs, const EffectLimits& limits) {
    assets_ = &assets;
    files_ = &files;
    jobs_ = &jobs;
    limits_ = limits;
    instances_.reserve(256);
    visible_.reserve(256);
}

void EffectSystem::Shutdown() {
    instances_.clear();
    byHandle_.clear();
    visible_.clear();
    if (assets_ != nullptr) {
        for (const Trail& t : trails_) assets_->Release(t.texture);
        for (const auto& [path, t] : decalTextures_) assets_->Release(t.id);
    }
    trails_.clear();
    decals_.clear();
    decalTextures_.clear();
    for (auto& [key, entry] : library_) {
        std::lock_guard lock(entry->mutex);
        entry->abandoned = true;
        if (assets_ != nullptr) {
            for (AssetId id : entry->textureIds) assets_->Release(id);
            for (AssetId id : entry->meshIds) assets_->Release(id);
        }
        entry->textureIds.clear();
        entry->meshIds.clear();
        entry->meshes.clear();
    }
    library_.clear();
    assets_ = nullptr;
}

std::shared_ptr<EffectSystem::Program> EffectSystem::Find(const std::string& key) {
    auto& entry = library_[key];
    if (entry) return entry;
    entry = std::make_shared<Program>();
    entry->path = key;
    // Read, decode and compile on a Streaming job; its textures and meshes asked of the store there.
    jobs_->Run(
        [entry, files = files_, assets = assets_] {
            SR_ZONE("effects: load");
            std::string error;
            auto program = std::make_shared<EffectProgram>();
            const auto file = files->Read(entry->path);
            assets::EffectData data;
            const bool ok = file && assets::DecodeEfp(file->Bytes(), data, error) && CompileEffect(data, *program, error);
            std::lock_guard lock(entry->mutex);
            if (!ok) {
                entry->error = file ? error : "not found";
                entry->state.store(2, std::memory_order_release);
                return;
            }
            if (entry->abandoned) return;
            for (const std::string& path : program->textures) entry->textureIds.push_back(assets->RequestTexture(path));
            for (const std::string& path : program->meshes) entry->meshIds.push_back(assets->RequestModel(path));
            entry->program = std::move(program);
            entry->state.store(1, std::memory_order_release);
        },
        core::JobPriority::Streaming);
    return entry;
}

void EffectSystem::Resolve(Program& p) {
    // Its textures' bindless indices and its meshes, as they come in (a failed one is never drawn).
    std::lock_guard lock(p.mutex);
    const EffectProgram& program = *p.program;
    if (p.look.textures.empty()) {
        p.look.textures.assign(program.textures.size(), 0);
        p.look.textureFlags.assign(program.textures.size(), 0);
        p.look.meshReady.assign(program.meshes.size(), 0);
        p.meshes.resize(program.meshes.size());
    }
    bool all = true;
    for (std::size_t i = 0; i < p.textureIds.size(); ++i) {
        if ((p.look.textureFlags[i] & (EffectSimulation::Look::kReady | EffectSimulation::Look::kFailed)) != 0) continue;
        if (const auto texture = assets_->Texture(p.textureIds[i])) {
            p.look.textures[i] = texture->bindless;
            p.look.textureFlags[i] = EffectSimulation::Look::kReady | (texture->premultipliedAlpha ? EffectSimulation::Look::kPremultiplied : 0);
        } else if (assets_->State(p.textureIds[i]) == AssetState::Failed) {
            p.look.textureFlags[i] = EffectSimulation::Look::kFailed;
        } else {
            all = false;
        }
    }
    for (std::size_t i = 0; i < p.meshIds.size(); ++i) {
        if (p.meshes[i] || p.look.meshReady[i] != 0) continue;
        if (auto model = assets_->Model(p.meshIds[i])) {
            p.meshes[i] = std::move(model);
            p.look.meshReady[i] = 1;
        } else if (assets_->State(p.meshIds[i]) == AssetState::Failed) {
            p.look.meshReady[i] = 0;
            p.meshes[i].reset();
        } else {
            all = false;
        }
    }
    p.resolved = all;
}

EffectSystem::Handle EffectSystem::Start(const Request& request) {
    if (jobs_ == nullptr || None(request.path) || instances_.size() >= limits_.effects) return 0;
    if (request.kind != Kind::World && quality_ < (request.kind == Kind::Hit ? 1 : 2)) return 0;
    auto instance = std::make_unique<Instance>();
    instance->handle = nextHandle_++;
    if (nextHandle_ == 0) nextHandle_ = 1;
    instance->program = Find(core::NormalizeAssetPath(request.path));
    instance->request = request;
    instance->at = request.at * request.local;
    const Handle handle = instance->handle;
    byHandle_[handle] = instance.get();
    instances_.push_back(std::move(instance));
    return handle;
}

void EffectSystem::Stop(Handle handle) {
    if (const auto it = byHandle_.find(handle); it != byHandle_.end()) it->second->stopped = true;
}

void EffectSystem::Release(Handle handle) {
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end()) return;
    Instance& i = *it->second;
    if (!i.started) {
        i.stopped = true; // never began: nothing to finish
        return;
    }
    i.released = true;
    i.releasedAt = i.age;
    i.simulation.Release();
}

void EffectSystem::Move(Handle handle, const glm::mat4& at) {
    if (const auto it = byHandle_.find(handle); it != byHandle_.end()) it->second->request.at = at;
}

bool EffectSystem::Alive(Handle handle) const { return byHandle_.contains(handle); }

std::vector<std::string> EffectSystem::TakeFailures() { return std::exchange(failures_, {}); }

EffectSystem::Handle EffectSystem::StartTrail(const std::string& texture, std::uint32_t colour, float seconds) {
    if (assets_ == nullptr || texture.empty() || quality_ < 1) return 0;
    Trail t;
    t.handle = nextHandle_++;
    t.texture = assets_->RequestTexture(texture);
    t.colour = colour;
    t.seconds = std::max(0.02f, seconds);
    trails_.push_back(std::move(t));
    return trails_.back().handle;
}

void EffectSystem::GroundDecal(const std::string& texture, int cells, std::span<const glm::vec3> corners, std::uint32_t colour) {
    if (assets_ == nullptr || texture.empty() || cells < 1 || corners.size() != std::size_t((cells + 1) * (cells + 1))) return;
    auto [it, fresh] = decalTextures_.try_emplace(texture);
    if (fresh) it->second.id = assets_->RequestTexture(texture);
    decals_.push_back({&it->second, cells, colour, std::vector<glm::vec3>(corners.begin(), corners.end())});
}

void EffectSystem::TrailPoint(Handle handle, const glm::vec3& a, const glm::vec3& b) {
    for (Trail& t : trails_) {
        if (t.handle != handle || t.ended) continue;
        t.points.push_back({a, b, clock_});
        return;
    }
}

void EffectSystem::EndTrail(Handle handle) {
    for (Trail& t : trails_) {
        if (t.handle == handle) t.ended = true;
    }
}

void EffectSystem::Update(double dtSeconds, const View& view, const Place& place, EffectFrame& out) {
    SR_ZONE("effects: update");
    out.Clear();
    stats_ = {};
    if (jobs_ == nullptr) return;
    const float dt = static_cast<float>(dtSeconds);
    clock_ += dtSeconds;
    for (auto& [key, entry] : library_) {
        const int state = entry->state.load(std::memory_order_acquire);
        if (state == 1) {
            ++stats_.programs;
            if (!entry->resolved) Resolve(*entry);
        } else if (state == 2) {
            ++stats_.failed;
            if (!entry->reported) {
                entry->reported = true;
                std::lock_guard lock(entry->mutex);
                failures_.push_back(key + ": " + entry->error);
            }
        } else {
            ++stats_.loading;
        }
    }

    // Start, place and choose what is seen (in order: cheap per effect).
    const Frustum frustum(view.viewProj);
    visible_.clear();
    for (const auto& owned : instances_) {
        Instance& i = *owned;
        i.visible = false;
        if (i.stopped) continue;
        if (!i.started) {
            if (i.request.delay > 0.0f) {
                i.request.delay -= dt;
                continue;
            }
            const int state = i.program->state.load(std::memory_order_acquire);
            if (state == 2 || (state == 0 && (i.waited += dt) > kLoadPatience)) {
                i.stopped = true;
                continue;
            }
            if (state == 0) continue;
        }
        const EffectSystem::Anchor& anchor = i.request.anchor;
        if (anchor.target == 0) {
            i.at = i.request.at * i.request.local;
        } else if (!i.released && (anchor.follow || !i.placed)) {
            glm::mat4 m(1.0f);
            if (place && place(anchor, m)) {
                i.at = m * i.request.local;
                i.placed = true;
            } else if (!i.placed) {
                i.stopped = true; // its target is not there
                continue;
            } else {
                i.released = true; // its target went: what it made finishes where it was
                i.simulation.Release();
            }
        }
        const float effectScale = i.request.scale * i.program->program->scale;
        if (!i.started) {
            // Started where it stands (what it lets go at once is born there). Compiled since the
            // library's pass above: its look sized before anything is built.
            if (!i.program->resolved) Resolve(*i.program);
            i.simulation.Place(i.at * glm::scale(glm::mat4(1.0f), glm::vec3(effectScale)));
            i.simulation.Start(*i.program->program, i.handle * 2654435761u + 1u, i.request.seconds != 0.0f);
            i.started = true;
        }
        i.age += dt;
        if (i.request.seconds > 0.0f && i.age >= i.request.seconds && !i.released) {
            i.released = true;
            i.releasedAt = i.age;
            i.simulation.Release();
        }
        // Its fades: in from its start, out from its release (gone when faded).
        i.opacity = i.request.fadeIn > 0.0f ? std::clamp(i.age / i.request.fadeIn, 0.0f, 1.0f) : 1.0f;
        if (i.released && i.request.fadeOut > 0.0f) {
            if (i.releasedAt < 0.0f) i.releasedAt = i.age;
            const float left = 1.0f - (i.age - i.releasedAt) / i.request.fadeOut;
            if (left <= 0.0f) {
                i.stopped = true;
                continue;
            }
            i.opacity *= left;
        }
        const glm::vec3 centre(i.at[3]);
        i.distance = glm::length(centre - view.eye);
        const float radius = 150.0f * std::max(i.request.scale * i.program->program->scale, 0.1f);
        if (i.distance > limits_.lodFar * 1.5f + radius || !frustum.Sphere(centre, radius)) {
            // Not simulated off the screen; a one-shot still ends on time.
            const bool kept = i.request.seconds < 0.0f || (i.request.seconds > 0.0f && !i.released);
            const float length = static_cast<float>(i.program->program->lengthTicks) * EffectSimulation::kTick;
            if (i.released || (!kept && i.age > length + 1.0f)) i.stopped = true;
            continue;
        }
        i.visible = true;
        visible_.push_back(&i);
    }

    // Step and build every visible effect on the workers, each into its own buffers.
    const std::size_t spriteBudget = quality_ >= 2 ? limits_.sprites : limits_.sprites / 4;
    const std::size_t perEffect = std::max<std::size_t>(64, spriteBudget / std::max<std::size_t>(1, visible_.size()) * 4);
    // The camera's axes as the scene's view has them (left-handed: right = up x forward).
    const glm::vec3 forward = glm::normalize(view.forward);
    glm::vec3 right = glm::cross(glm::vec3(0, 1, 0), forward);
    right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1, 0, 0);
    const glm::mat3 camera(right, glm::normalize(glm::cross(forward, right)), forward);
    const float lodNear = limits_.lodNear, lodFar = limits_.lodFar;
    jobs_->ParallelFor(visible_.size(), 4, [&](std::size_t begin, std::size_t end) {
        SR_ZONE("effects: step");
        for (std::size_t k = begin; k < end; ++k) {
            Instance& i = *visible_[k];
            const float far = std::clamp((i.distance - lodNear) / std::max(lodFar - lodNear, 1.0f), 0.0f, 1.0f);
            const float scale = i.request.scale * i.program->program->scale;
            i.simulation.Place(i.at * glm::scale(glm::mat4(1.0f), glm::vec3(scale)));
            i.simulation.Advance(dt, 1.0f - 0.75f * far);
            i.output.Clear();
            if (i.simulation.Done()) continue;
            i.simulation.Build(i.at, scale, i.program->look, camera, perEffect, i.output, i.request.pull, i.opacity);
        }
    });

    // The ground decals first, under every alpha-blended effect: a plate a cell, its corners the grid's
    // (the plate's second axis from the lower row to the upper, as the plate's texture runs).
    for (const Decal& d : decals_) {
        DecalTexture& t = *d.texture;
        if (!t.ready) {
            if (const auto texture = assets_->Texture(t.id)) {
                t.bindless = texture->bindless;
                t.ready = true;
            }
        }
        if (!t.ready) continue;
        const int row = d.cells + 1;
        const float step = 1.0f / float(d.cells);
        for (int j = 0; j < d.cells; ++j) {
            for (int i = 0; i < d.cells; ++i) {
                const glm::vec3& p00 = d.corners[std::size_t(j * row + i)];
                const glm::vec3& p10 = d.corners[std::size_t(j * row + i + 1)];
                const glm::vec3& p01 = d.corners[std::size_t((j + 1) * row + i)];
                const glm::vec3& p11 = d.corners[std::size_t((j + 1) * row + i + 1)];
                EffectSprite& s = out.sprites.emplace_back();
                s.centre = (p00 + p10 + p01 + p11) * 0.25f;
                s.axisX = ((p10 + p11) - (p00 + p01)) * 0.5f;
                s.axisY = ((p00 + p10) - (p01 + p11)) * 0.5f;
                s.colour = d.colour;
                s.texture = t.bindless;
                s.flags = 0;
                s.window = {float(i) * step, float(j) * step, step, step};
                ++out.spriteCounts[0];
            }
        }
    }
    decals_.clear();

    // Gather: the alpha-blended far to near (by effect), then the rest.
    std::sort(visible_.begin(), visible_.end(), [](const Instance* a, const Instance* b) { return a->distance > b->distance; });
    for (std::size_t blend = 0; blend < kEffectBlends; ++blend) {
        for (const Instance* i : visible_) {
            const auto& sprites = i->output.sprites[blend];
            const std::size_t room = spriteBudget - std::min(spriteBudget, out.sprites.size());
            const std::size_t n = std::min(room, sprites.size());
            out.sprites.insert(out.sprites.end(), sprites.begin(), sprites.begin() + static_cast<std::ptrdiff_t>(n));
            out.spriteCounts[blend] += static_cast<std::uint32_t>(n);
        }
    }
    // The ribbons, as the sprites: by blend, the alpha-blended by effect far to near (each counts as so many sprites
    // against the frame's budget: a segment's six vertices one).
    for (std::size_t blend = 0; blend < kEffectBlends; ++blend) {
        for (const Instance* i : visible_) {
            const auto& ribbons = i->output.ribbons[blend];
            const std::size_t room = (spriteBudget - std::min(spriteBudget, out.sprites.size() + out.ribbons.size() / 6)) * 6;
            const std::size_t n = std::min(room, ribbons.size()) / 6 * 6;
            out.ribbons.insert(out.ribbons.end(), ribbons.begin(), ribbons.begin() + static_cast<std::ptrdiff_t>(n));
            out.ribbonCounts[blend] += static_cast<std::uint32_t>(n);
        }
    }
    // The trails, additive (the last blend): each step between two of the line's places a plate, its
    // texture's width over the trail's length, fading with its age (ours: the 1.188 engine's ribbon
    // is not read; Length its seconds in milliseconds).
    for (auto it = trails_.begin(); it != trails_.end();) {
        Trail& t = *it;
        std::erase_if(t.points, [&](const Trail::Point& p) { return clock_ - p.at > double(t.seconds); });
        if (!t.ready && assets_ != nullptr) {
            if (const auto texture = assets_->Texture(t.texture)) {
                t.bindless = texture->bindless;
                t.ready = true;
            }
        }
        if (t.ended && t.points.size() < 2) {
            if (assets_ != nullptr) assets_->Release(t.texture);
            it = trails_.erase(it);
            continue;
        }
        for (std::size_t k = 1; t.ready && k < t.points.size() && out.sprites.size() < spriteBudget; ++k) {
            const Trail::Point& p0 = t.points[k - 1];
            const Trail::Point& p1 = t.points[k];
            const float age0 = float(clock_ - p0.at) / t.seconds, age1 = float(clock_ - p1.at) / t.seconds;
            EffectSprite& s = out.sprites.emplace_back();
            s.centre = (p0.a + p0.b + p1.a + p1.b) * 0.25f;
            s.axisX = ((p1.a + p1.b) - (p0.a + p0.b)) * 0.5f; // along the trail, older to newer
            s.axisY = ((p0.b + p1.b) - (p0.a + p1.a)) * 0.5f; // across it, hilt to tip
            const float fade = std::clamp(1.0f - 0.5f * (age0 + age1), 0.0f, 1.0f);
            const std::uint32_t alpha = std::uint32_t(float(t.colour >> 24) * fade);
            s.colour = (t.colour & 0x00FFFFFFu) | (alpha << 24);
            s.texture = t.bindless;
            s.flags = 0;
            s.window = {std::clamp(age0, 0.0f, 1.0f), 0.0f, std::clamp(age1, 0.0f, 1.0f) - std::clamp(age0, 0.0f, 1.0f), 1.0f}; // u its age
            ++out.spriteCounts[kEffectBlends - 1];
        }
        ++it;
    }
    meshScratch_.clear();
    for (const Instance* i : visible_) {
        for (const EffectSimulation::MeshItem& m : i->output.meshes) {
            const auto& model = i->program->meshes[static_cast<std::size_t>(m.mesh)];
            if (!model) continue;
            for (const ModelPartInfo& part : model->parts) {
                if (meshScratch_.size() >= limits_.meshInstances) break;
                MeshItem item{static_cast<std::uint64_t>(m.blend) << 56 | (part.vertexOffset & 0x00FFFFFFFFFFFFFFull), &part, m.blend, m.instance};
                if ((item.instance.flags & kEffectOwnTexture) != 0) item.instance.texture = part.textureBindless;
                meshScratch_.push_back(item);
            }
        }
    }
    std::stable_sort(meshScratch_.begin(), meshScratch_.end(), [](const MeshItem& a, const MeshItem& b) { return a.key < b.key; });
    for (const MeshItem& item : meshScratch_) {
        if (out.meshDraws.empty() || out.meshDraws.back().vertexOffset != item.part->vertexOffset || out.meshDraws.back().blend != item.blend) {
            EffectMeshDraw draw;
            draw.vertexOffset = item.part->vertexOffset;
            draw.firstIndex = static_cast<std::uint32_t>(item.part->indexOffset / 2);
            draw.indexCount = item.part->indexCount;
            draw.vertexFlags = item.part->vertexFlags;
            draw.firstInstance = static_cast<std::uint32_t>(out.meshInstances.size());
            draw.blend = item.blend;
            out.meshDraws.push_back(draw);
        }
        out.meshInstances.push_back(item.instance);
        ++out.meshDraws.back().instanceCount;
    }
    // The lights eased (about a tenth of a second): one coming in fades up from dark, one whose glow thins fades out.
    const float ease = 1.0f - std::exp(-static_cast<float>(std::max(dtSeconds, 0.0)) * 10.0f);
    for (Instance* i : visible_) {
        EffectLight& shown = i->shownLight;
        if (i->output.lit) {
            const EffectLight& now = i->output.light;
            if (!i->lightShown) {
                shown = now;
                shown.intensity = 0.0f;
                i->lightShown = true;
            }
            shown.position += (now.position - shown.position) * ease;
            shown.colour += (now.colour - shown.colour) * ease;
            shown.radius += (now.radius - shown.radius) * ease;
            shown.intensity += (now.intensity - shown.intensity) * ease;
        } else if (i->lightShown) {
            shown.intensity -= shown.intensity * ease;
        }
        shown.id = i->handle;
        if (i->lightShown && shown.intensity > 0.005f && out.lights.size() < limits_.lights) out.lights.push_back(shown);
        stats_.elements += i->simulation.Alive();
    }

    // The ended go.
    for (const auto& owned : instances_) {
        Instance& i = *owned;
        if (i.started && !i.stopped && i.visible && i.simulation.Done()) {
            const bool kept = i.request.seconds < 0.0f || (i.request.seconds > 0.0f && !i.released);
            if (!kept || i.released) i.stopped = true;
        }
        if (i.stopped) byHandle_.erase(i.handle);
    }
    std::erase_if(instances_, [](const std::unique_ptr<Instance>& i) { return i->stopped; });
    stats_.effects = instances_.size();
    stats_.visible = visible_.size();
    stats_.sprites = out.sprites.size();
    stats_.meshInstances = out.meshInstances.size();
    stats_.lights = out.lights.size();
    visible_.clear(); // its pointers may be gone now
}

} // namespace sr::gfx
