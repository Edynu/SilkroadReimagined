#pragma once

// Camera keys of the title screens (C5e): the intro scripts' (script\intro\*.txt) and the character
// select's own, which the client keeps in code. A key looks at
// its point from `distance` behind it along (pitch, yaw); positive pitch looks down.

#include <cmath>
#include <span>
#include <vector>

#include "screens/WorldScreen.h"
#include "sr/assets/IntroScript.h"
#include "sr/assets/Terrain.h"

namespace sr::client {

// Sets the backdrop's view to `key`, relative to region (originX, originY).
inline void ApplyCameraKey(WorldScreen& world, const assets::CameraKey& key, int originX, int originY) {
    const float px = key.position[0] + float(key.sectorX - originX) * assets::kRegionSize;
    const float pz = key.position[2] + float(key.sectorY - originY) * assets::kRegionSize;
    const float pitch = key.rotation[0], yaw = key.rotation[1];
    const float fx = std::cos(pitch) * std::sin(yaw), fy = -std::sin(pitch), fz = std::cos(pitch) * std::cos(yaw);
    // Our fly camera is the eye, with positive pitch looking up.
    world.SetView(px - fx * key.distance, key.position[1] - fy * key.distance, pz - fz * key.distance, yaw, -pitch);
}

inline assets::CameraKey MakeKey(float time, int sectorX, int sectorY, float x, float y, float z, float pitch, float yaw, float distance) {
    assets::CameraKey k;
    k.time = time;
    k.sectorX = sectorX;
    k.sectorY = sectorY;
    k.position[0] = x;
    k.position[1] = y;
    k.position[2] = z;
    k.rotation[0] = pitch;
    k.rotation[1] = yaw;
    k.distance = distance;
    return k;
}

// The character select (region 0x6951 = 81,105).
inline constexpr int kSelectRegionX = 81, kSelectRegionY = 105;

// Arriving at the dock: two keys 2 s apart (key i at time 2 i).
inline std::vector<assets::CameraKey> DockKeys() {
    return {MakeKey(0.0f, kSelectRegionX, kSelectRegionY, 60.0f, -15.0f, 700.0f, 0.1f, 3.0f, 30.0f),
            MakeKey(2.0f, kSelectRegionX, kSelectRegionY, 60.0f, -15.0f, 660.0f, 0.1f, 3.0f, 30.0f)};
}

// From the dock to the pedestal: four keys 5/3 s apart.
inline std::vector<assets::CameraKey> CreateKeys() {
    constexpr float step = 5.0f / 3.0f;
    return {MakeKey(0 * step, kSelectRegionX, kSelectRegionY, 60.0f, -15.0f, 660.0f, 0.1f, 3.0f, 30.0f),
            MakeKey(1 * step, kSelectRegionX, kSelectRegionY, 100.0f, 0.0f, 625.0f, -0.1f, 2.2f, 30.0f),
            MakeKey(2 * step, kSelectRegionX, kSelectRegionY, 150.0f, -10.0f, 642.0f, -0.01f, 1.8f, 30.0f),
            MakeKey(3 * step, kSelectRegionX, kSelectRegionY, 165.2f, -33.9f, 640.3f, 0.62f, 2.53f, 30.0f)};
}

// The dock's slots: between two points, a full page of four at fifths of the way, a last page of
// r characters at (r + 1)ths (y keeps the fifths there, as the original). Facing yaw 0.
struct DockSlot {
    float x, y, z;
};
inline std::vector<DockSlot> DockSlots(int count) {
    constexpr DockSlot a{95.0f, -27.6f, 654.0f}, b{29.0f, -27.6f, 644.0f};
    std::vector<DockSlot> out;
    const int pages = count / 4;
    for (int page = 0; page <= pages; ++page) {
        const int onPage = page < pages ? 4 : count % 4;
        const float parts = page < pages ? 5.0f : float(onPage + 1);
        for (int i = 1; i <= onPage; ++i) {
            out.push_back({a.x + (b.x - a.x) * float(i) / parts, a.y + (b.y - a.y) * float(i) / 5.0f, a.z + (b.z - a.z) * float(i) / parts});
        }
    }
    return out;
}

// Choosing a character: from the current view to a close one in 2 s (going back: 1 s).
// The look-at point is 2 units off the slot on x and z, at a height from the character's scale
// (scale x 15 or 16 - 28.78; a character being deleted kneels: lower), level, distance 8. Its yaw
// is written as 0.3925 minus the slot's; we turn it by pi to look at the character's face
// (inferred: the camera's own yaw field and the track's differ by half a turn).
inline assets::CameraKey ChosenKey(float time, const DockSlot& slot, bool deleting, float scale = 1.0f) {
    constexpr float kPi = 3.14159265f;
    const float y = deleting ? scale * 9.0f - 29.0f : scale * 15.5f - 28.779173f;
    return MakeKey(time, kSelectRegionX, kSelectRegionY, slot.x + 2.0f, y, slot.z + 2.0f, 0.0f, kPi + 0.3925f, 8.0f);
}

// Picking a race at the pedestal: the camera keeps the pedestal's last rotation and
// distance and moves its point toward the idol in 1 s, under a fade to black. 0 Europe, 1 China.
inline assets::CameraKey PickKey(float time, int race) {
    return race == 0 ? MakeKey(time, kSelectRegionX, kSelectRegionY, 168.5f, -34.9f, 639.1f, 0.62f, 2.53f, 30.0f)
                     : MakeKey(time, kSelectRegionX, kSelectRegionY, 165.8f, -35.1f, 636.7f, 0.62f, 2.53f, 30.0f);
}

// The pedestal: the box (a map table) and, on it, the race idols and the lizard, each
// turned by its yaw (read as a yaw, inferred).
struct StageProp {
    const char* model;
    float x, y, z, yaw;
};
inline constexpr StageProp kStageProps[] = {
    {"res\\interface\\box.bsr", 155.0f, -20.0f, 652.0f, 3.0f},
    {"res\\interface\\interface_idol_europe.bsr", 157.0f, -20.0f, 652.6f, 3.31f},
    {"res\\interface\\interface_idol_china.bsr", 156.2f, -20.0f, 651.6f, 3.03f},
    {"res\\interface\\interface_lizard.bsr", 155.6f, -20.0f, 651.6f, 3.0f},
};

} // namespace sr::client
