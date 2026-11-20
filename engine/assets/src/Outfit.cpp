#include "sr/assets/Outfit.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <unordered_set>

#include "sr/assets/FileSystem.h"

namespace sr::assets {

namespace {

using Quat = std::array<float, 4>; // x, y, z, w
using Vec3 = std::array<float, 3>;

Quat Multiply(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
    // v' = v + 2w (u x v) + 2 u x (u x v), u = q.xyz
    const Vec3 u{q[0], q[1], q[2]};
    const auto cross = [](const Vec3& a, const Vec3& b) {
        return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const Vec3 t = cross(u, v);
    const Vec3 tt = cross(u, t);
    return {v[0] + 2.0f * (q[3] * t[0] + tt[0]), v[1] + 2.0f * (q[3] * t[1] + tt[1]), v[2] + 2.0f * (q[3] * t[2] + tt[2])};
}

float Distance(const Vec3& a, const Vec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// A bone is the same in two skeletons only if its name AND bind pose agree
// (items reuse generic names such as "Bone01" for private bones); genuinely
// shared bones come from the same rig export, so the tolerance is just float noise.
bool SamePose(const BindPose& a, const BindPose& b) {
    if (Distance(a.position, b.position) > 0.1f) return false;
    const float dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] + a.rotation[2] * b.rotation[2] +
                      a.rotation[3] * b.rotation[3];
    return std::abs(dot) > 0.995f; // q and -q are the same rotation
}

} // namespace

std::vector<BindPose> BindWorld(const SkeletonData& skeleton) {
    std::vector<BindPose> world(skeleton.bones.size());
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        const Bone& bone = skeleton.bones[i];
        if (bone.parent < 0) {
            world[i] = {bone.local.translation, bone.local.rotation};
        } else {
            const BindPose& parent = world[static_cast<std::size_t>(bone.parent)];
            const Vec3 offset = Rotate(parent.rotation, bone.local.translation);
            world[i].rotation = Multiply(parent.rotation, bone.local.rotation);
            world[i].position = {parent.position[0] + offset[0], parent.position[1] + offset[1],
                                 parent.position[2] + offset[2]};
        }
    }
    return world;
}

std::vector<std::array<float, 3>> BindWorldPositions(const SkeletonData& skeleton) {
    std::vector<Vec3> positions;
    for (const BindPose& pose : BindWorld(skeleton)) positions.push_back(pose.position);
    return positions;
}

bool AssembleOutfit(const FileSystem& fs, const std::string& bodyPath, const std::vector<std::string>& itemPaths,
                    ModelData& out, OutfitReport& report, std::string& error) {
    report = {};
    if (!LoadModel(fs, bodyPath, out, error)) return false;
    for (const std::string& w : out.warnings) report.warnings.push_back(w);

    // Body cover table: key -> body mesh index.
    PartLink bodyLink;
    if (auto file = fs.Read(bodyPath)) {
        if (!FindPartLink(file->Bytes(), true, out.resource.meshPaths.size(), bodyLink)) {
            report.warnings.push_back(bodyPath + ": no cover table");
        }
    }
    const std::vector<BindPose> wearerBind = BindWorld(out.skeleton);
    std::unordered_set<int> hidden;

    int itemIndex = -1;
    for (const std::string& itemPath : itemPaths) {
        ++itemIndex;
        ModelData item;
        std::string itemError;
        if (!LoadModel(fs, itemPath, item, itemError)) {
            report.warnings.push_back(itemPath + ": " + itemError);
            continue;
        }
        // Covering.
        if (auto file = fs.Read(itemPath)) {
            PartLink link;
            if (FindPartLink(file->Bytes(), false, item.resource.meshPaths.size(), link) && link.c == 1) {
                for (const auto& pair : link.pairs) {
                    for (const auto& body : bodyLink.pairs) {
                        if (body[0] == pair[0]) hidden.insert(static_cast<int>(body[1]));
                    }
                }
            }
        }
        // Bones, one at a time:
        //  1. the wearer's bone of the same name, if it is the same bone (SamePose);
        //  2. else the nearest ancestor in the item's own skeleton that is shared;
        //  3. else the item's attach bone (.bsr skeleton section), with the vertices
        //     moved by its bind position: private branches are authored around it;
        //  4. else the wearer's root.
        const std::vector<BindPose> itemBind = BindWorld(item.skeleton);
        const std::string& attachName = item.resource.skeletonAttachBone;
        const int attach = attachName.empty() ? -1 : out.skeleton.Find(attachName);
        // The item's own bones become sockets on the attach bone (an arrow is held at a bow's).
        if (attach >= 0) {
            for (std::size_t b = 0; b < item.skeleton.bones.size(); ++b) {
                const std::string& name = item.skeleton.bones[b].name;
                if (out.skeleton.Find(name) >= 0) continue;
                ModelData::Socket socket;
                socket.name = name;
                socket.parentBone = attachName;
                socket.item = itemPath;
                socket.position = itemBind[b].position;
                socket.rotation = itemBind[b].rotation;
                out.sockets.push_back(std::move(socket));
            }
        }
        const auto shared = [&](int own, int wearer) {
            return wearer >= 0 && (own < 0 || SamePose(itemBind[static_cast<std::size_t>(own)],
                                                       wearerBind[static_cast<std::size_t>(wearer)]));
        };
        for (ModelPart& part : item.parts) {
            MeshData& mesh = part.mesh;
            std::string line = part.meshPath + " v" + std::to_string(mesh.version) + " " +
                               std::to_string(mesh.vertices.size()) + " verts, material flags 0x";
            {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "%X", part.hasMaterial ? part.material.flags : 0u);
                line += hex;
            }
            if (mesh.boneNames.empty()) {
                report.bindings.push_back(line + ": rigid -> " + (attach >= 0 ? "attach " + attachName + " (+bind position)" : "unskinned"));
                // A rigid piece (weapons, shields): it follows the attach bone as a whole.
                if (attach < 0 || mesh.vertices.empty()) {
                    part.meshIndex = -1;
                    part.allowAlphaTest = false;
                    part.outfitItem = itemIndex;
                    out.parts.push_back(std::move(part));
                    continue;
                }
                mesh.boneNames = {attachName};
                mesh.boneIndices.assign(mesh.vertices.size(), {0, 0});
                mesh.boneWeights.assign(mesh.vertices.size(), {1.0f, 0.0f});
                for (MeshVertex& v : mesh.vertices) {
                    for (int k = 0; k < 3; ++k) v.position[k] += wearerBind[static_cast<std::size_t>(attach)].position[static_cast<std::size_t>(k)];
                }
                part.meshIndex = -1;
                part.allowAlphaTest = false;
                part.outfitItem = itemIndex;
                out.parts.push_back(std::move(part));
                continue;
            }
            std::vector<bool> baked(mesh.boneNames.size(), false);
            line += ", bones:";
            for (std::size_t b = 0; b < mesh.boneNames.size(); ++b) {
                std::string& name = mesh.boneNames[b];
                const std::string original = name;
                const int own = item.skeleton.Find(name);
                const int wearerBone = out.skeleton.Find(name);
                if (shared(own, wearerBone)) {
                    line += " [" + original + (own < 0 ? " wearer]" : " shared]");
                    continue;
                }
                if (wearerBone >= 0 && own >= 0) {
                    const float d = Distance(itemBind[static_cast<std::size_t>(own)].position,
                                             wearerBind[static_cast<std::size_t>(wearerBone)].position);
                    line += " (" + original + " pose differs by " + std::to_string(d) + ")";
                }
                bool resolved = false;
                for (int cur = own >= 0 ? item.skeleton.bones[static_cast<std::size_t>(own)].parent : -1; cur >= 0;
                     cur = item.skeleton.bones[static_cast<std::size_t>(cur)].parent) {
                    const std::string& ancestor = item.skeleton.bones[static_cast<std::size_t>(cur)].name;
                    if (shared(cur, out.skeleton.Find(ancestor))) {
                        name = ancestor;
                        resolved = true;
                        break;
                    }
                }
                if (resolved) {
                    line += " [" + original + " -> ancestor " + name + "]";
                    continue;
                }
                if (attach >= 0) {
                    name = attachName;
                    baked[b] = true;
                    line += " [" + original + " -> attach " + name + "]";
                } else if (!out.skeleton.bones.empty()) {
                    report.warnings.push_back(itemPath + ": bone " + name + " has no wearer bone");
                    name = out.skeleton.bones.front().name;
                    line += " [" + original + " -> ROOT " + name + "]";
                }
            }
            report.bindings.push_back(line);
            if (attach >= 0 && std::find(baked.begin(), baked.end(), true) != baked.end()) {
                const Vec3& at = wearerBind[static_cast<std::size_t>(attach)].position;
                for (std::size_t v = 0; v < mesh.vertices.size() && v < mesh.boneIndices.size(); ++v) {
                    float weight = 0.0f;
                    for (int k = 0; k < 2; ++k) {
                        const std::size_t bone = mesh.boneIndices[v][static_cast<std::size_t>(k)];
                        if (bone < baked.size() && baked[bone]) weight += mesh.boneWeights[v][static_cast<std::size_t>(k)];
                    }
                    for (int k = 0; k < 3; ++k) mesh.vertices[v].position[k] += at[static_cast<std::size_t>(k)] * weight;
                }
            }
            part.meshIndex = -1; // not a body mesh
            part.allowAlphaTest = false;
            part.outfitItem = itemIndex;
            out.parts.push_back(std::move(part));
        }
        for (const std::string& w : item.warnings) report.warnings.push_back(itemPath + ": " + w);
    }

    if (!hidden.empty()) {
        std::vector<ModelPart> kept;
        for (ModelPart& part : out.parts) {
            if (part.meshIndex >= 0 && hidden.contains(part.meshIndex)) {
                report.hiddenMeshes.push_back(part.meshPath);
            } else {
                kept.push_back(std::move(part));
            }
        }
        out.parts = std::move(kept);
    }
    return true;
}

namespace {

using Point = std::array<float, 3>;

float Dot(const Point& a, const Point& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Point Sub(const Point& a, const Point& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

// The squared distance from p to the triangle abc (the closest point by its Voronoi region).
float TriangleDistance2(const Point& p, const Point& a, const Point& b, const Point& c) {
    const Point ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
    const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    const auto at = [&](const Point& q) { const Point d = Sub(p, q); return Dot(d, d); };
    if (d1 <= 0 && d2 <= 0) return at(a);
    const Point bp = Sub(p, b);
    const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return at(b);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const float v = d1 / (d1 - d3);
        return at({a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]});
    }
    const Point cp = Sub(p, c);
    const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return at(c);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const float w = d2 / (d2 - d6);
        return at({a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]});
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return at({b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2])});
    }
    const float denom = 1.0f / (va + vb + vc), v = vb * denom, w = vc * denom;
    return at({a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w});
}

} // namespace

float CoveredShare(const MeshData& part, const std::vector<const MeshData*>& over, float reach) {
    if (part.vertices.empty()) return 0.0f;
    // The others' triangles with their boxes widened by the reach.
    struct Triangle {
        Point a, b, c, low, high;
    };
    std::vector<Triangle> triangles;
    for (const MeshData* mesh : over) {
        if (mesh == nullptr) continue;
        for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            if (mesh->indices[i] >= mesh->vertices.size() || mesh->indices[i + 1] >= mesh->vertices.size() || mesh->indices[i + 2] >= mesh->vertices.size()) continue;
            Triangle t;
            const auto& va = mesh->vertices[mesh->indices[i]].position;
            const auto& vb = mesh->vertices[mesh->indices[i + 1]].position;
            const auto& vc = mesh->vertices[mesh->indices[i + 2]].position;
            t.a = {va[0], va[1], va[2]};
            t.b = {vb[0], vb[1], vb[2]};
            t.c = {vc[0], vc[1], vc[2]};
            for (int k = 0; k < 3; ++k) {
                t.low[k] = std::min({t.a[k], t.b[k], t.c[k]}) - reach;
                t.high[k] = std::max({t.a[k], t.b[k], t.c[k]}) + reach;
            }
            triangles.push_back(t);
        }
    }
    const float reach2 = reach * reach;
    std::size_t near = 0;
    for (const MeshVertex& v : part.vertices) {
        const Point p{v.position[0], v.position[1], v.position[2]};
        for (const Triangle& t : triangles) {
            if (p[0] < t.low[0] || p[0] > t.high[0] || p[1] < t.low[1] || p[1] > t.high[1] || p[2] < t.low[2] || p[2] > t.high[2]) continue;
            if (TriangleDistance2(p, t.a, t.b, t.c) <= reach2) {
                ++near;
                break;
            }
        }
    }
    return float(near) / float(part.vertices.size());
}

std::vector<std::string> LeaveOutCoveredBody(ModelData& model, std::uint32_t covered) {
    std::vector<std::string> left;
    if (covered == 0) return left;
    // A body part and the region over it: the torso the mail's (0x02), the upper arm the shoulders' (0x04), the lower arm
    // the hands' (0x08), the pelvis and thigh the pants' (0x10), the calf the boots' (0x20).
    static constexpr std::pair<const char*, std::uint32_t> kParts[] = {
        {"_torso_upper.bms", 0x02}, {"_torso_lower.bms", 0x02}, {"_arm_upper.bms", 0x04}, {"_arm_lower.bms", 0x08},
        {"_arm_fore.bms", 0x08},    {"_pelvis.bms", 0x10},      {"_thigh.bms", 0x10},     {"_calf.bms", 0x20}};
    std::vector<const MeshData*> over;
    for (const ModelPart& part : model.parts) {
        if (part.outfitItem >= 0) over.push_back(&part.mesh);
    }
    const auto underPieces = [&](const ModelPart& part) {
        if (part.outfitItem >= 0) return false;
        std::string path = part.meshPath;
        std::replace(path.begin(), path.end(), '/', '\\');
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (path.find("\\char\\") == std::string::npos) return false;
        for (const auto& [suffix, region] : kParts) {
            if (path.ends_with(suffix)) return (covered & region) != 0 && CoveredShare(part.mesh, over, kCoverReach) >= kCoverShare;
        }
        return false;
    };
    std::erase_if(model.parts, [&](const ModelPart& part) {
        if (!underPieces(part)) return false;
        left.push_back(part.meshPath);
        return true;
    });
    return left;
}

} // namespace sr::assets
