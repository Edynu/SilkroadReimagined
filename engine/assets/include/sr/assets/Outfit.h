#pragma once

// A dressed character: the body resource plus worn item resources merged into one
// model on the body's skeleton, the way the original attaches items:
//
// - Item meshes are skinned to the wearer's bones by name, bone by bone (see
//   AssembleOutfit in Outfit.cpp): a name match counts only when both bind poses
//   agree; private bones fall back to their nearest shared ancestor, then to the
//   item's attach bone (vertices on it moved by the attach bone's bind position),
//   then the root. Rigid pieces follow the
//   attach bone as a whole.
// - An item whose part link replaces the skin (cover mode 1) hides the body meshes
//   whose cover keys it lists (see PartLink in Model.h).

#include <cstdint>
#include <string>
#include <vector>

#include "sr/assets/Model.h"

namespace sr::assets {

class FileSystem;

struct OutfitReport {
    std::vector<std::string> hiddenMeshes; // body meshes covered by items
    std::vector<std::string> warnings;
    // One line per item mesh: how each of its bones was bound (for diagnostics).
    std::vector<std::string> bindings;
};

// `itemPaths`: item .bsr resources in wear order. Fails only when the body cannot load.
bool AssembleOutfit(const FileSystem& fs, const std::string& bodyPath, const std::vector<std::string>& itemPaths,
                    ModelData& out, OutfitReport& report, std::string& error);

// Bind-pose world transform of every bone (forward kinematics over the local transforms).
struct BindPose {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1}; // x, y, z, w
};
std::vector<BindPose> BindWorld(const SkeletonData& skeleton);
std::vector<std::array<float, 3>> BindWorldPositions(const SkeletonData& skeleton);

// The share (0..1) of a mesh's vertices that lie within `reach` of a triangle of the others (bind pose, model space):
// how much of a body part the worn pieces lie on.
float CoveredShare(const MeshData& part, const std::vector<const MeshData*>& over, float reach);

// The body's own parts under the regions the shown pieces cover (DressedCharacter::covered: 0x02 the torso, 0x04 the
// upper arm, 0x08 the lower arm, 0x10 the pelvis and thigh, 0x20 the calf) left out of an assembled outfit, each only
// where the pieces lie on it: kCoverShare of its vertices within kCoverReach of their triangles (ours,
// a suit tighter than the body, the thief's, would let it show through; a short top leaves the waist bare, so it stays).
// Returns the parts left out.
constexpr float kCoverReach = 0.3f; // model units (a character stands about 18 tall)
constexpr float kCoverShare = 0.9f;
std::vector<std::string> LeaveOutCoveredBody(ModelData& model, std::uint32_t covered);

} // namespace sr::assets
