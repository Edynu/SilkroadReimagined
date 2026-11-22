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
// - An item of cover mode 1 hides the body meshes whose cover keys it lists, as the
//   original's compound character does when it attaches one; a mode 2 piece hides
//   nothing (see PartLink in Model.h). Nothing else leaves the body out.

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

} // namespace sr::assets
