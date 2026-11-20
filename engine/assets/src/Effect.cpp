#include "sr/assets/Effect.h"

#include <algorithm>
#include <cstring>

#include "sr/core/BinaryReader.h"

namespace sr::assets {

// Payloads by name (counts are i32; colours are stored B, G, R, A):
//   commands
//     StaticEmit: u32 start, u32 duration, u32 period, u32 limit, f32 per emission
//     Attraction, SetGraphRandomScale: f32
//     SetPosition, SetSpherePos, SetVelocity, Force: 3 f32
//     SetRotationMat, SetRVelocityMat: 16 f32
//     SetRotation, SetRVelocity: 3 f32 (pitch, yaw, roll degrees) + 16 f32
//     SetRotationAxis, SetRVelocityAxis, SetShapeRot, SetShapeRotVel: 4 f32 (axis, degrees) + 16 f32
//     SetConePos, SetConeVel, ConeForce: 3 f32 (min, max, degrees) + 3 f32
//     SetGraphScale: n x 3 f32; SetGraphDiffuse: n x colour
//     SetBANRot: f32, n x 16 f32; SetBANPos: f32, n x 3 f32; TextureSlide: 3 f32, n x 4 f32
//     life / view / render kinds and ProgramUpdate: nothing
//   parameters: float f32; Vector 3 f32; Matrix 16 f32; SEFStaticEmit 20 bytes; AxisVector4
//     20 f32; RotVector 19 f32; AngleVector1 6 f32; FrameScale n x 3 f32; BlendScaleGraph a
//     curve of 3 f32; BlendScaleGraphPointer f32; FrameDiffuse n x colour; BlendDiffuseGraph a
//     curve of colours; FrameBANRotation f32, n x 16 f32; FrameBANPosition f32, n x 3 f32;
//     BSAnimation n x str; FrameTextureSlide 3 f32, n x 4 f32
//     (curve: f32 begin, f32 end, n x { f32 time 0..1, value })
//   controllers (kept by kind): NormalTimeLife, NormalTimeLoopLife nothing; StaticEmit as the command; LinkMode
//     4 u32; BAN n x str; ViewMode str; Shape str + a resource; ScaleGraph 3 curves of f32 +
//     2 f32; DiffuseGraph a curve of u8 + a curve of colours; Program a program

namespace {

constexpr std::int32_t kMaxCount = 1 << 16;
constexpr int kMaxDepth = 64;

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> file) : r_(file) {}

    core::BinaryReader r_;
    std::string error_;

    bool Fail(std::string message) {
        if (error_.empty()) error_ = std::move(message) + " at " + std::to_string(r_.Offset());
        return false;
    }
    bool Count(std::int32_t& n) {
        n = static_cast<std::int32_t>(r_.U32());
        if (!r_.Ok() || n < 0 || n > kMaxCount) return Fail("implausible count");
        return true;
    }
    void Floats(std::vector<float>& out, std::size_t n) {
        for (std::size_t i = 0; i < n && r_.Ok(); ++i) out.push_back(r_.F32());
    }
    std::array<std::uint8_t, 4> Colour() {
        const std::uint8_t b = r_.U8(), g = r_.U8(), red = r_.U8(), a = r_.U8();
        return {red, g, b, a};
    }
    // A list of `width` floats per entry, after its count.
    bool List(EffectCommand& c, std::size_t width) {
        std::int32_t n = 0;
        if (!Count(n)) return false;
        c.rows = n;
        Floats(c.values, static_cast<std::size_t>(n) * width);
        return r_.Ok();
    }
    bool SkipCurve(std::size_t valueBytes) {
        r_.F32();
        r_.F32();
        std::int32_t n = 0;
        if (!Count(n)) return false;
        r_.Bytes(static_cast<std::size_t>(n) * (4 + valueBytes));
        return r_.Ok();
    }

    bool CommandPayload(EffectCommand& c) {
        const std::string& n = c.name;
        const auto is = [&](std::initializer_list<const char*> names) {
            return std::any_of(names.begin(), names.end(), [&](const char* s) { return n == s; });
        };
        if (n == "StaticEmit") {
            for (int i = 0; i < 4; ++i) c.values.push_back(static_cast<float>(r_.U32()));
            c.values.push_back(r_.F32());
        } else if (is({"Attraction", "SetGraphRandomScale"})) {
            Floats(c.values, 1);
        } else if (is({"SetPosition", "SetSpherePos", "SetVelocity", "Force"})) {
            Floats(c.values, 3);
        } else if (is({"SetRotationMat", "SetRVelocityMat"})) {
            Floats(c.values, 16);
        } else if (is({"SetRotation", "SetRVelocity"})) {
            Floats(c.values, 3 + 16);
        } else if (is({"SetRotationAxis", "SetRVelocityAxis", "SetShapeRot", "SetShapeRotVel"})) {
            Floats(c.values, 4 + 16);
        } else if (is({"SetConePos", "SetConeVel", "ConeForce"})) {
            Floats(c.values, 3 + 3);
        } else if (n == "SetGraphScale") {
            return List(c, 3);
        } else if (n == "SetGraphDiffuse") {
            std::int32_t count = 0;
            if (!Count(count)) return false;
            c.rows = count;
            for (std::int32_t i = 0; i < count && r_.Ok(); ++i) c.colors.push_back(Colour());
        } else if (n == "SetBANRot") {
            Floats(c.values, 1);
            return List(c, 16);
        } else if (n == "SetBANPos") {
            Floats(c.values, 1);
            return List(c, 3);
        } else if (n == "TextureSlide") {
            Floats(c.values, 3);
            return List(c, 4);
        } else if (!is({"NeverExtinct", "NormalTimeExtinct", "NormalTimeLoop", "NormalTimeLife", "ViewNone", "ViewBillboard",
                        "ViewVBillboard", "ViewYBillboard", "RenderNone", "RenderMesh", "RenderPlate", "RenderLinkObj",
                        "RenderLinkPipe", "RenderLinkDPipe", "ProgramUpdate"})) {
            return Fail("unknown command " + n);
        }
        return r_.Ok();
    }

    bool Command(EffectCommand& c) {
        c = EffectCommand{};
        if (r_.U8() == 0) return r_.Ok();
        c.name = r_.CountedString(256);
        c.flags = r_.U8();
        c.schedule = r_.U8();
        c.start = r_.F32();
        c.spacing = r_.F32();
        c.end = r_.F32();
        return r_.Ok() && CommandPayload(c);
    }

    bool Program(std::vector<EffectCommand>& out) {
        std::int32_t n = 0;
        if (!Count(n)) return false;
        out.resize(static_cast<std::size_t>(n));
        for (EffectCommand& c : out) {
            if (!Command(c)) return false;
        }
        // Absent commands are dropped.
        std::erase_if(out, [](const EffectCommand& c) { return c.name.empty(); });
        return true;
    }

    bool Resource(EffectResource& out) {
        out.backFace = r_.U32();
        out.srcBlend = r_.U32();
        out.dstBlend = r_.U32();
        for (auto& s : out.stages) s = r_.U32();
        std::int32_t n = 0;
        if (!Count(n)) return false;
        for (std::int32_t i = 0; i < n && r_.Ok(); ++i) {
            EffectResource::Mesh mesh;
            mesh.path = r_.CountedString();
            std::int32_t textures = 0;
            if (!Count(textures)) return false;
            for (std::int32_t t = 0; t < textures && r_.Ok(); ++t) mesh.textures.push_back(r_.CountedString());
            out.meshes.push_back(std::move(mesh));
        }
        return r_.Ok();
    }

    bool Parameter(EffectNode& node) {
        const std::string kind = r_.CountedString(256);
        std::vector<float> scratch;
        std::int32_t n = 0;
        if (kind == "float" || kind == "BlendScaleGraphPointer") {
            r_.F32();
        } else if (kind == "Vector") {
            Floats(scratch, 3);
        } else if (kind == "Matrix") {
            Floats(scratch, 16);
        } else if (kind == "SEFStaticEmit") {
            r_.Bytes(20);
        } else if (kind == "AxisVector4") {
            Floats(scratch, 20);
        } else if (kind == "RotVector") {
            Floats(scratch, 19);
        } else if (kind == "AngleVector1") {
            Floats(scratch, 6);
        } else if (kind == "FrameScale") {
            if (!Count(n)) return false;
            r_.Bytes(static_cast<std::size_t>(n) * 12);
        } else if (kind == "BlendScaleGraph") {
            r_.F32();
            r_.F32();
            if (!Count(n)) return false;
            EffectScaleGraph graph;
            for (std::int32_t i = 0; i < n && r_.Ok(); ++i) {
                EffectScaleGraph::Point p;
                p.t = r_.F32();
                for (float& v : p.value) v = r_.F32();
                graph.points.push_back(p);
            }
            node.scaleGraphs.push_back(std::move(graph));
        } else if (kind == "FrameDiffuse") {
            if (!Count(n)) return false;
            r_.Bytes(static_cast<std::size_t>(n) * 4);
        } else if (kind == "BlendDiffuseGraph") {
            return SkipCurve(4);
        } else if (kind == "FrameBANRotation" || kind == "FrameBANPosition") {
            r_.F32();
            if (!Count(n)) return false;
            r_.Bytes(static_cast<std::size_t>(n) * (kind == "FrameBANRotation" ? 64 : 12));
        } else if (kind == "BSAnimation") {
            if (!Count(n)) return false;
            for (std::int32_t i = 0; i < n && r_.Ok(); ++i) r_.CountedString();
        } else if (kind == "FrameTextureSlide") {
            Floats(scratch, 3);
            if (!Count(n)) return false;
            r_.Bytes(static_cast<std::size_t>(n) * 16);
        } else {
            return Fail("unknown parameter " + kind);
        }
        return r_.Ok();
    }

    bool Controller(EffectNode& node) {
        const std::string kind = r_.CountedString(256);
        node.controllers.push_back(EffectCommand{});
        EffectCommand& c = node.controllers.back();
        c.name = kind;
        std::int32_t n = 0;
        if (kind == "NormalTimeLife" || kind == "NormalTimeLoopLife") return true;
        if (kind == "StaticEmit") {
            for (int i = 0; i < 4; ++i) c.values.push_back(static_cast<float>(r_.U32()));
            c.values.push_back(r_.F32());
        } else if (kind == "LinkMode") {
            // Four counts of ancestors up (reads them, marks those ancestors): kept as numbers.
            for (int i = 0; i < 4; ++i) c.values.push_back(static_cast<float>(static_cast<std::int32_t>(r_.U32())));
        } else if (kind == "BAN") {
            if (!Count(n)) return false;
            for (std::int32_t i = 0; i < n && r_.Ok(); ++i) r_.CountedString();
        } else if (kind == "ViewMode") {
            r_.CountedString();
        } else if (kind == "Shape") {
            r_.CountedString();
            EffectResource ignored;
            return Resource(ignored);
        } else if (kind == "ScaleGraph") {
            for (int i = 0; i < 3; ++i) {
                if (!SkipCurve(4)) return false;
            }
            r_.F32();
            r_.F32();
        } else if (kind == "DiffuseGraph") {
            return SkipCurve(1) && SkipCurve(4);
        } else if (kind == "Program") {
            std::vector<EffectCommand> ignored;
            return Program(ignored);
        } else {
            return Fail("unknown controller " + kind);
        }
        return r_.Ok();
    }

    bool Node(EffectNode& node, int depth) {
        if (depth > kMaxDepth) return Fail("effect nodes nest too deep");
        r_.U32();
        node.name = r_.CountedString();
        std::int32_t n = 0;
        if (!Count(n)) return false;
        for (std::int32_t i = 0; i < n; ++i) {
            if (!Controller(node)) return false;
        }
        node.frames = static_cast<std::int32_t>(r_.U32());
        if (!Count(n)) return false;
        for (std::int32_t i = 0; i < n; ++i) {
            if (!Parameter(node)) return false;
        }
        if (!Program(node.preProgram) || !Program(node.emitterProgram) || !Program(node.postEmitterProgram) ||
            !Command(node.life) || !Program(node.updateProgram)) {
            return false;
        }
        node.keepMatrix = r_.U8() != 0;
        node.keepOrigin = r_.U8() != 0;
        node.positionDepth = static_cast<std::int32_t>(r_.U32());
        node.matrixDepth = static_cast<std::int32_t>(r_.U32());
        node.velocityDepth = static_cast<std::int32_t>(r_.U32());
        node.localMotion = r_.U8() != 0;
        node.followDepth = static_cast<std::int32_t>(r_.U32());
        node.shapeMotion = r_.U8() != 0;
        if (!Command(node.view) || !Resource(node.resource) || !Command(node.render) || !Program(node.trailingProgram) ||
            !Program(node.renderProgram)) {
            return false;
        }
        if (!Count(n)) return false;
        node.children.resize(static_cast<std::size_t>(n));
        for (EffectNode& child : node.children) {
            if (!Node(child, depth + 1)) return false;
        }
        return r_.Ok() || Fail("truncated");
    }
};

} // namespace

const EffectCommand* EffectNode::Find(const std::vector<EffectCommand>& program, std::string_view name) {
    const auto it = std::find_if(program.begin(), program.end(), [&](const EffectCommand& c) { return c.name == name; });
    return it != program.end() ? &*it : nullptr;
}

bool DecodeEfp(std::span<const std::byte> file, EffectData& out, std::string& error) {
    out = EffectData{};
    if (file.size() < 12 || std::memcmp(file.data(), "JMXVEFF ", 8) != 0) {
        error = "not a JMXVEFF file";
        return false;
    }
    const char* digits = reinterpret_cast<const char*>(file.data()) + 8;
    out.version = 0;
    for (int i = 0; i < 4; ++i) {
        if (digits[i] < '0' || digits[i] > '9') {
            error = "bad JMXVEFF version";
            return false;
        }
        out.version = out.version * 10 + (digits[i] - '0');
    }
    if (out.version < 10 || out.version > 13) {
        error = "unsupported JMXVEFF version " + std::to_string(out.version);
        return false;
    }
    Decoder d(file);
    d.r_.Seek(12);
    if (out.version >= 12) out.scale = d.r_.F32();
    if (out.version == 13) d.r_.Bytes(12);
    if (!d.Node(out.root, 0)) {
        error = d.error_.empty() ? "truncated" : d.error_;
        return false;
    }
    if (d.r_.Offset() != file.size()) {
        error = std::to_string(file.size() - d.r_.Offset()) + " bytes after the effect";
        return false;
    }
    return true;
}

} // namespace sr::assets
