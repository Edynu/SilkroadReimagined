// A quest scene on the game screen (the original's message 0x3CA2, which plays
// script\q_script\<name>.txt; the Roc's entrance): the script's commands at their times, the camera
// on the scene's paths (S_CameraTeleport, S_CameraCreate, S_CameraInsert, S_CameraStart, S_CameraRelease) and shaken
// (S_CameraShake), the player hidden (S_Start's INVISIBLE_USER) and the names left out, the cinema's bars (S_SubTitle
// STITLE_START / STITLE_FINISH), its sounds (S_SoundPlay), its characters (S_ObjectCreate in a free stage slot, its
// animation by S_ObjectAniToIndex, gone at S_ObjectRelease), over at S_End.
// Not shown yet (logged once): the music by its name (S_SetBGM), the light's fades (S_Lighting), the crowd of random
// adventurers (S_ObjectCreateIndex, S_ObjectEquipSetIndex) and the player's own place in it (S_Teleport *player).
// Its texts on the screen: S_StaticCreateEx / S_StaticRelease (the dialogue's lines in the White Viper's scenes).

#include <algorithm>
#include <cmath>

#include "audio/GameAudio.h"
#include "play/PlayScreen.h"
#include "sr/assets/FileSystem.h"
#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "title/CameraKeys.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::PlayQuestScene(ScreenContext& context, const std::string& name) {
    if (scene_.active) EndQuestScene();
    const std::string path = "script\\q_script\\" + name + ".txt";
    const auto file = context.files.Read(path);
    std::string error;
    QuestSceneRun run;
    if (!file || !assets::DecodeSceneScript(core::DecodeTextFile(file->Bytes()), run.commands, error)) {
        core::LogWarning("game: quest scene {}: {}", path, file ? error : std::string("no such file"));
        return;
    }
    run.active = true;
    run.started = context.time;
    scene_ = std::move(run);
    core::LogInfo("game: quest scene {} ({} commands)", name, scene_.commands.size());
}

void PlayScreen::EndQuestScene() {
    if (world_) {
        for (const auto& [name, slot] : scene_.objects) {
            world_->ClearStage(slot);
            if (slot < slotUsed_.size()) slotUsed_[slot] = false;
        }
    }
    const double bars = scene_.barsShown;
    scene_ = {};
    scene_.barsShown = bars; // the bars go out after the scene
    core::LogInfo("game: quest scene over");
}

void PlayScreen::StepQuestScene(ScreenContext& context) {
    // The bars slide in and out over their time (ours: the original's 2.0 s and 120 interface pixels as given).
    const float barsTarget = scene_.active && scene_.bars ? 1.0f : 0.0f;
    const float barsStep = float(context.dt) / std::max(0.1f, scene_.barsSeconds);
    scene_.barsShown = barsTarget > scene_.barsShown ? std::min<double>(barsTarget, scene_.barsShown + barsStep)
                                                     : std::max<double>(barsTarget, scene_.barsShown - barsStep);
    if (!scene_.active) return;
    const float t = float(context.time - scene_.started);
    const auto unshown = [&](const std::string& verb) {
        if (scene_.unshown.insert(verb).second) core::LogInfo("game: quest scene: {} not shown (ours)", verb);
    };
    while (scene_.active && scene_.next < scene_.commands.size() && scene_.commands[scene_.next].time <= t) {
        const assets::SceneCommand& c = scene_.commands[scene_.next++];
        const auto arg = [&](std::size_t i) { return i < c.args.size() ? c.args[i] : std::string(); };
        if (c.verb == "S_Start") {
            scene_.hideSelf = arg(1) == "INVISIBLE_USER";
        } else if (c.verb == "S_End") {
            EndQuestScene();
            return;
        } else if (c.verb == "S_SubTitle") {
            scene_.bars = arg(0) == "STITLE_START";
            scene_.barsSeconds = c.Number(1, 2.0f);
            scene_.barsHeight = c.Number(2, 120.0f);
        } else if (c.verb == "S_CameraTeleport") {
            assets::CameraKey key;
            if (assets::SceneCameraKey(c, false, key)) {
                scene_.held = key;
                scene_.running = false;
            }
        } else if (c.verb == "S_CameraCreate") {
            scene_.inserting.clear();
        } else if (c.verb == "S_CameraInsert") {
            assets::CameraKey key;
            if (assets::SceneCameraKey(c, true, key)) scene_.inserting.push_back(key);
        } else if (c.verb == "S_CameraStart") {
            if (!scene_.inserting.empty()) {
                std::stable_sort(scene_.inserting.begin(), scene_.inserting.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
                scene_.track = scene_.inserting;
                scene_.trackFrom = c.time;
                scene_.running = true;
            }
        } else if (c.verb == "S_CameraRelease") {
            // The camera stays where its path left it until the next teleport.
            if (scene_.running && !scene_.track.empty()) scene_.held = assets::SampleCameraTrack(scene_.track, t - scene_.trackFrom);
            scene_.running = false;
        } else if (c.verb == "S_CameraShake") {
            // <kind> <strength> <ms> <step ms> (ours: the strength in hundredths of a unit, as the skills' shakes).
            shake_.amplitude = std::max(shake_.amplitude * float(context.time < shake_.until), c.Number(1, 300.0f));
            shake_.from = context.time;
            shake_.until = context.time + c.Number(2, 500.0f) / 1000.0;
            shake_.step = std::max(0.005f, c.Number(3, 10.0f) / 1000.0f);
            shake_.next = context.time;
        } else if (c.verb == "S_SoundPlay") {
            if (context.audio != nullptr && world_) {
                assets::CameraKey key;
                if (QuestSceneCamera(t, key)) {
                    const glm::vec3 at(key.position[0] + float(key.sectorX - originX_) * assets::kRegionSize, key.position[1],
                                       key.position[2] + float(key.sectorY - originY_) * assets::kRegionSize);
                    context.audio->PlayEffect(arg(0), at);
                }
            }
        } else if (c.verb == "S_ObjectCreate") {
            // <name> <codename> <sectorX> <sectorY> <x> <y> <z> <yaw (degrees)> <shown>
            const assets::ObjectRow* row = tables_ && tables_->Usable() ? tables_->characters.Find(std::string_view(arg(1))) : nullptr;
            const std::string model = row != nullptr ? assets::CharacterTable::ResolveModel(context.files, *row) : std::string();
            const auto walkers = slotUsed_.begin() + std::ptrdiff_t(std::min(kWalkerSlots, slotUsed_.size()));
            const auto free = std::find(slotUsed_.begin(), walkers, false);
            if (model.empty() || free == walkers || !world_) {
                core::LogWarning("game: quest scene: {} ({}) not placed", arg(0), arg(1));
                continue;
            }
            *free = true;
            const std::size_t slot = std::size_t(free - slotUsed_.begin());
            WorldScreen::StageLook look{model};
            look.height = float(row->scalePercent) / 100.0f;
            const float x = c.Number(4) + float(int(c.Number(2)) - originX_) * assets::kRegionSize;
            const float z = c.Number(6) + float(int(c.Number(3)) - originY_) * assets::kRegionSize;
            const float yaw = c.Number(7) * 3.14159265f / 180.0f;
            world_->SetStage(slot, look, x, c.Number(5), z, yaw);
            scene_.objects.emplace_back(arg(0), slot);
            scene_.looks[arg(0)] = {look, x, c.Number(5), z, yaw};
        } else if (c.verb == "S_ObjectAniToIndex") {
            // <name> <blend> <speed> *ANI_EMOTION01..: the default set's animation 50 + n, as the emotes (once).
            const auto it = std::find_if(scene_.objects.begin(), scene_.objects.end(), [&](const auto& o) { return o.first == arg(0); });
            const std::string ani = arg(3);
            if (it != scene_.objects.end() && ani.starts_with("*ANI_EMOTION") && world_) {
                QuestSceneRun::Placed& placed = scene_.looks[arg(0)];
                placed.look.clipSet = "default";
                placed.look.state = proto::kEmoteFirstState + std::uint32_t(std::max(0, std::atoi(ani.c_str() + 12) - 1));
                placed.look.take += 1;
                placed.look.once = true;
                world_->SetStage(it->second, placed.look, placed.x, placed.y, placed.z, placed.yaw);
            }
        } else if (c.verb == "S_ObjectRelease") {
            const auto it = std::find_if(scene_.objects.begin(), scene_.objects.end(), [&](const auto& o) { return o.first == arg(0); });
            if (it != scene_.objects.end()) {
                if (world_) world_->ClearStage(it->second);
                if (it->second < slotUsed_.size()) slotUsed_[it->second] = false;
                scene_.objects.erase(it);
            }
        } else if (c.verb == "S_StaticCreateEx") {
            // <name> <screen h> <screen v> <its own h> <its own v> <w> <h> <x> <y> <text h> <text v> <r> <g> <b> <font> <text>
            // (the original's SC_StaticCreateEx: POS_LEFT / TOP 0, POS_CENTER 1, POS_RIGHT / BOTTOM 2; the text a key after
            // '*', else as written).
            const auto pos = [&](std::size_t i) {
                const std::string v = arg(i);
                return v == "POS_CENTER" ? 1 : (v == "POS_RIGHT" || v == "POS_BOTTOM") ? 2 : 0;
            };
            QuestSceneRun::Static st;
            st.name = arg(0);
            st.screenH = pos(1), st.screenV = pos(2), st.ownH = pos(3), st.ownV = pos(4);
            st.w = c.Number(5), st.h = c.Number(6), st.x = c.Number(7), st.y = c.Number(8);
            st.textH = pos(9), st.textV = pos(10);
            const auto byte = [&](std::size_t i) { return std::uint32_t(std::clamp(int(c.Number(i, 255.0f)), 0, 255)); };
            st.color = 0xFF000000u | byte(11) << 16 | byte(12) << 8 | byte(13);
            st.font = int(c.Number(14));
            std::string text = arg(15);
            st.text = text.starts_with('*') ? gui_.data().Text(text.substr(1)) : text;
            std::erase_if(scene_.statics, [&](const QuestSceneRun::Static& s) { return s.name == st.name; });
            scene_.statics.push_back(std::move(st));
        } else if (c.verb == "S_StaticRelease") {
            std::erase_if(scene_.statics, [&](const QuestSceneRun::Static& s) { return s.name == arg(0); });
        } else if (c.verb == "S_SetBGM" || c.verb == "S_Lighting" || c.verb == "S_ObjectCreateIndex" || c.verb == "S_ObjectEquipSetIndex" ||
                   c.verb == "S_Teleport") {
            unshown(c.verb);
        }
        // S_ViewSightRanger, S_CameraSetState, S_ObjectOptionSet: nothing of ours to change.
    }
    // A scene whose script never ends: over 5 s after its last command.
    if (scene_.active && scene_.next >= scene_.commands.size() && t > scene_.commands.back().time + 5.0f) EndQuestScene();
}

bool PlayScreen::QuestSceneCamera(float t, assets::CameraKey& key) const {
    if (!scene_.active) return false;
    if (scene_.running && !scene_.track.empty()) {
        key = assets::SampleCameraTrack(scene_.track, t - scene_.trackFrom);
        return true;
    }
    if (scene_.held) {
        key = *scene_.held;
        return true;
    }
    return false;
}

void PlayScreen::DrawQuestSceneBars(ScreenContext& context, ui::Painter& paint) {
    const float w = float(context.device.SwapchainWidth()) / paint.scale(), h = float(context.device.SwapchainHeight()) / paint.scale();
    if (scene_.barsShown > 0.0) {
        const float bar = scene_.barsHeight * float(scene_.barsShown) * h / 768.0f; // the original's interface is 768 high
        paint.Fill({0.0f, 0.0f, w, bar}, ui::Argb(0xFF000000));
        paint.Fill({0.0f, h - bar, w, bar}, ui::Argb(0xFF000000));
    }
    // Its texts: the screen's anchor (none, half, all of its width or height), less its own share of its size, plus its
    // offset (the original's 0x503F10), its text aligned in it.
    for (const QuestSceneRun::Static& st : scene_.statics) {
        const auto along = [](int anchor, float full) { return anchor == 2 ? full : anchor == 1 ? std::floor(full / 2.0f) : 0.0f; };
        const auto own = [](int anchor, float size) { return anchor == 2 ? size : anchor == 1 ? std::floor(size / 2.0f) : 0.0f; };
        const ui::Rect r{along(st.screenH, w) - own(st.ownH, st.w) + st.x, along(st.screenV, h) - own(st.ownV, st.h) + st.y, st.w, st.h};
        ui::DrawLabel(paint, st.text, r, ui::Argb(st.color), st.font, st.textH, st.textV, ui::kTextShadow);
    }
}

} // namespace sr::client
