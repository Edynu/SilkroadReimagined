#pragma once

// Intro scripts (Media.pk2 script\intro\*.txt) drive the camera flight behind the
// title / login screen. Each "S_CameraInsert" line adds one camera key:
//
//   <n> S_CameraInsert <time> <sectorX> <sectorY> <x> <y> <z> <pitch> <yaw> <roll> <distance>
//
// Position is relative to the sector (region) named on the line. The camera looks
// at that point from `distance` behind it, along the direction the angles give:
// forward = (cos pitch * sin yaw, -sin pitch, cos pitch * cos yaw); positive pitch
// looks down. Keys are interpolated with a Catmull-Rom spline.

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sr::assets {

struct CameraKey {
    float time = 0.0f;
    int sectorX = 0;
    int sectorY = 0;
    float position[3]{};
    float rotation[3]{}; // pitch, yaw, roll (radians)
    float distance = 0.0f;
};

// Text already decoded to UTF-8. Keys must have increasing times.
bool DecodeIntroCamera(std::string_view utf8Text, std::vector<CameraKey>& out, std::string& error);

// The camera at `time` (clamped to the track). The result is in the sector of the
// key at or before `time`; keys in other sectors are moved into it first.
CameraKey SampleCameraTrack(std::span<const CameraKey> keys, float time);

// The quest scene scripts (Media.pk2 script\q_script\*.txt: the Roc's entrance, the White Viper's; played on the
// original's message 0x3CA2) are the intro's lines with every command of the scene player:
//   <time> <command> <arguments...>          '//' starts a comment; an argument in double quotes is one
// Times are seconds from the scene's start; lines are not in time order.
struct SceneCommand {
    float time = 0.0f;
    std::string verb;              // S_CameraInsert, S_SoundPlay, ...
    std::vector<std::string> args; // as written (quotes off)
    float Number(std::size_t i, float otherwise = 0.0f) const;
};
// The commands in time order (lines of one time keep the file's order). False when a line has no time.
bool DecodeSceneScript(std::string_view utf8Text, std::vector<SceneCommand>& out, std::string& error);
// A camera key of S_CameraInsert (`timed`: its first argument the key's time) or S_CameraTeleport (none):
// <sectorX> <sectorY> <x> <y> <z> <pitch> <yaw> <roll> <distance>.
bool SceneCameraKey(const SceneCommand& command, bool timed, CameraKey& out);

} // namespace sr::assets
