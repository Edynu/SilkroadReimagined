#include "sr/assets/Outfit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    for (const std::string& passed : itemPaths) {
        ++itemIndex;
        // "item.bsr#setN": the piece in its resource's material set N (the battle dress's colours).
        std::string itemPath = passed;
        std::uint32_t materialSet = 0;
        if (const std::size_t mark = itemPath.find("#set"); mark != std::string::npos) {
            materialSet = static_cast<std::uint32_t>(std::strtoul(itemPath.c_str() + mark + 4, nullptr, 10));
            itemPath.resize(mark);
        }
        ModelData item;
        std::string itemError;
        if (!LoadModel(fs, itemPath, item, itemError)) {
            report.warnings.push_back(itemPath + ": " + itemError);
            continue;
        }
        if (materialSet != 0 && !UseMaterialSet(fs, item, materialSet)) {
            report.warnings.push_back(itemPath + ": no material set " + std::to_string(materialSet));
        }
        // Covering: a mode 1 piece hides the body's meshes of its keys; a mode 2 piece is drawn over them.
        bool overSkin = false;
        if (auto file = fs.Read(itemPath)) {
            PartLink link;
            const bool found = FindPartLink(file->Bytes(), false, item.resource.meshPaths.size(), link);
            overSkin = found && link.c == 2 && link.b <= 5; // the armour's and the garments' parts, not a weapon
            if (found && link.c == 1) {
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
        // The item's own bones become sockets on the attach bone (an arrow is held at a bow's). One the
        // wearer has by name is a socket too unless it is the same bone: a woman's body has a Bone01 of
        // its own, not the bow's, where the bow's glow hangs.
        if (attach >= 0) {
            for (std::size_t b = 0; b < item.skeleton.bones.size(); ++b) {
                const std::string& name = item.skeleton.bones[b].name;
                if (const int wearer = out.skeleton.Find(name);
                    wearer >= 0 && SamePose(itemBind[b], wearerBind[static_cast<std::size_t>(wearer)])) {
                    continue;
                }
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
            part.overSkin = overSkin;
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

} // namespace sr::assets
