// A quest scene on the game screen (the original's message 0x3CA2, which plays
// script\q_script\<name>.txt; the Roc's entrance): the script's commands at their times, the camera
// on the scene's paths (S_CameraTeleport, S_CameraCreate, S_CameraInsert, S_CameraStart, S_CameraRelease) and shaken
// (S_CameraShake), the player hidden (S_Start's INVISIBLE_USER) and the names left out, the cinema's bars (S_SubTitle
// STITLE_START / STITLE_FINISH), its sounds (S_SoundPlay), its characters (S_ObjectCreate in a free stage slot, its
// animation by S_ObjectAniToIndex, gone at S_ObjectRelease), over at S_End.
// The scene's music (S_SetBGM, and a scene sound naming a Music.pk2 track: the Roc's own track). The Roc's crowd of
// adventurers (S_ObjectCreateIndex): the players there in their own looks. Not shown yet (logged once): the berserk
// quests' shots (S_DecoSkillTarget).
// The screen washed with a colour: S_Lighting (a flash, a dip to black before a cut).
// Its texts on the screen: S_StaticCreateEx / S_StaticRelease (the dialogue's lines in the White Viper's scenes).
// Its characters, its own and the player (*player, over where the game has it until S_End): S_Teleport, S_ObjectTurn,
// S_ObjectMoveTo walking or running (S_ObjectMotionState; S_ObjectOptionSet's speeds, *SCR_OBJ_NODRAW / DRAW), a clip
// by its file (S_ObjectAniToName) or none (S_ObjectAniStopAll), effects on their bones (S_DecoEftAdd, S_DecoEftClear);
// effects in place (S_EffectCreate, S_EffectRelease) and pictures over the screen (S_TextureFade).

#include <algorithm>
#include <cmath>
#include <optional>

#include "audio/GameAudio.h"
#include "play/PlayScreen.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/SkillAnimation.h"
#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"
#include "title/CameraKeys.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr std::uint32_t kSceneStand = 0, kSceneWalk = 1, kSceneRun = 7; // animation states (STAND1, WALK, RUN)
constexpr float kDegrees = 3.14159265f / 180.0f;
constexpr float kSceneGroundReach = 30.0f; // how far over a scene character's height its floor is looked for (ours)

// A place of the script, <sectorX> <sectorY> <x> <y> <z> from argument `i`, against the world's origin region.
glm::vec3 ScenePlace(const assets::SceneCommand& c, std::size_t i, int originX, int originY) {
    return {c.Number(i + 2) + float(int(c.Number(i)) - originX) * assets::kRegionSize, c.Number(i + 3),
            c.Number(i + 4) + float(int(c.Number(i + 1)) - originY) * assets::kRegionSize};
}

// The region of the script's <sectorX> <sectorY> from argument `i`.
std::uint16_t SceneRegion(const assets::SceneCommand& c, std::size_t i) {
    return std::uint16_t((int(c.Number(i)) & 0xFF) | (int(c.Number(i + 1)) & 0xFF) << 8);
}

// The scene's POS_ anchors: POS_LEFT / TOP 0, POS_CENTER 1, POS_RIGHT / BOTTOM 2.
int SceneAnchor(const std::string& v) { return v == "POS_CENTER" ? 1 : (v == "POS_RIGHT" || v == "POS_BOTTOM") ? 2 : 0; }

} // namespace

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
    run.name = name;
    run.castsPlayers = std::any_of(run.commands.begin(), run.commands.end(), [](const auto& c) { return c.verb == "S_ObjectCreateIndex"; });
    scene_ = std::move(run);
    core::LogInfo("game: quest scene {} ({} commands)", name, scene_.commands.size());
}

void PlayScreen::EndQuestScene() {
    if (world_) {
        for (const auto& [name, placed] : scene_.looks) {
            for (const std::uint32_t effect : placed.effects) world_->Effects().Stop(effect);
        }
        for (const auto& [name, effect] : scene_.effects) world_->Effects().Stop(effect);
        for (const auto& [name, slot] : scene_.objects) {
            world_->ClearStage(slot);
            if (slot < slotUsed_.size()) slotUsed_[slot] = false;
        }
        if (scene_.music != nullptr) scene_.music->SceneMusic({});
        for (const std::uint32_t person : scene_.crowd) {
            if (const std::size_t own = SlotOf(person); own != StageEffects::kNone) world_->SetStageOpacity(own, 1.0f);
        }
        // The player back where the game has it (PlaceWalkers puts it there again), shown.
        if (const std::size_t self = SlotOf(view_.Self()); self != StageEffects::kNone) world_->SetStageOpacity(self, 1.0f);
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
            // INVISIBLE_USER hides the player, but not in a scene that casts the players (its crowd): it stands in front.
            scene_.hideSelf = arg(1) == "INVISIBLE_USER" && !scene_.castsPlayers;
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
                scene_.trackStarted = false;
            }
        } else if (c.verb == "S_CameraCreate") {
            scene_.inserting.clear();
            scene_.trackStarted = false;
        } else if (c.verb == "S_CameraInsert") {
            // A key put in once the path runs joins it (the White Viper's 10th scene starts its path, then puts its key).
            assets::CameraKey key;
            if (assets::SceneCameraKey(c, true, key)) {
                scene_.inserting.push_back(key);
                if (scene_.trackStarted) {
                    std::stable_sort(scene_.inserting.begin(), scene_.inserting.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
                    scene_.track = scene_.inserting;
                    scene_.running = true;
                }
            }
        } else if (c.verb == "S_CameraStart") {
            scene_.trackStarted = true;
            scene_.trackFrom = c.time;
            if (!scene_.inserting.empty()) {
                std::stable_sort(scene_.inserting.begin(), scene_.inserting.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
                scene_.track = scene_.inserting;
                scene_.running = true;
            }
        } else if (c.verb == "S_CameraRelease") {
            // The camera stays where its path left it until the next teleport.
            if (scene_.running && !scene_.track.empty()) scene_.held = assets::SampleCameraTrack(scene_.track, t - scene_.trackFrom);
            scene_.running = false;
            scene_.trackStarted = false;
        } else if (c.verb == "S_CameraShake") {
            // <kind> <strength> <ms> <step ms> (ours: the strength in hundredths of a unit, as the skills' shakes).
            shake_.amplitude = std::max(shake_.amplitude * float(context.time < shake_.until), c.Number(1, 300.0f));
            shake_.from = context.time;
            shake_.until = context.time + c.Number(2, 500.0f) / 1000.0;
            shake_.step = std::max(0.005f, c.Number(3, 10.0f) / 1000.0f);
            shake_.next = context.time;
        } else if (c.verb == "S_SoundPlay") {
            // A music track the script names by a path the game lacks (the Roc's "Prim\Snd\Mob_Roc\roc_entrance.ogg" at
            // 3 s): the scene's music from then, as Music.pk2 has it.
            const std::string sound = arg(0);
            const std::size_t cut = sound.find_last_of("\\/");
            const std::string track = cut == std::string::npos ? sound : sound.substr(cut + 1);
            if (context.audio != nullptr && sound.size() > 4 && core::NormalizeAssetPath(sound).ends_with(".ogg") && !context.files.Exists(sound) &&
                context.files.Exists(track)) {
                context.audio->SceneMusic(track);
                scene_.music = context.audio;
            } else if (context.audio != nullptr && world_) {
                assets::CameraKey key;
                if (QuestSceneCamera(t, key)) {
                    const glm::vec3 at(key.position[0] + float(key.sectorX - originX_) * assets::kRegionSize, key.position[1],
                                       key.position[2] + float(key.sectorY - originY_) * assets::kRegionSize);
                    context.audio->PlayEffect(arg(0), at);
                }
            }
        } else if (c.verb == "S_ObjectCreate" || c.verb == "S_ObjectCreateIndex") {
            // <name> <codename> <sectorX> <sectorY> <x> <y> <z> <yaw (degrees)> <shown>. CreateIndex (the Roc's crowd
            // of adventurers; its index 1.188 takes as a RefObjID none of 1..50 is, so its crowd never shows): ours the
            // players there, each place the next of them in their own look and glows (its party first, then the others
            // in sight), their own figures hidden while the scene shows them; no one left, the place stays empty. The
            // player itself is shown then, at its own place in the scene (in front: S_Teleport *player), though the
            // script hides it (the user's call, 2026-11-25).
            const bool crowd = c.verb == "S_ObjectCreateIndex";
            const assets::ObjectRow* row = nullptr;
            std::optional<WorldScreen::StageLook> personLook;
            std::uint32_t person = 0;
            if (crowd) {
                if (!scene_.crowdListed) {
                    scene_.crowdListed = true;
                    const auto add = [&](std::uint32_t id) {
                        const game::WorldView::Entity* e = id != 0 ? view_.Find(id) : nullptr;
                        if (e != nullptr && id != view_.Self() && IsPlayer(*e) && std::find(scene_.crowd.begin(), scene_.crowd.end(), id) == scene_.crowd.end()) {
                            scene_.crowd.push_back(id);
                        }
                    };
                    for (const proto::PartyMemberRecord* m : player_.party.Others()) add(m->entity);
                    std::vector<std::uint32_t> others;
                    for (const auto& [id, e] : view_.Entities()) {
                        if (IsPlayer(e)) others.push_back(id);
                    }
                    std::sort(others.begin(), others.end());
                    for (const std::uint32_t id : others) add(id);
                }
                while (!personLook && scene_.crowdNext < scene_.crowd.size()) {
                    person = scene_.crowd[scene_.crowdNext++];
                    const game::WorldView::Entity* e = view_.Find(person);
                    if (const WorldScreen::StageLook* look = e != nullptr ? LookOf(context, *e) : nullptr) personLook = *look;
                }
                if (!personLook) {
                    unshown("S_ObjectCreateIndex (no one left for its place)");
                    continue;
                }
            } else if (tables_ && tables_->Usable()) {
                row = tables_->characters.Find(std::string_view(arg(1)));
            }
            const std::string model = crowd ? personLook->model : row != nullptr ? assets::CharacterTable::ResolveModel(context.files, *row) : std::string();
            const auto walkers = slotUsed_.begin() + std::ptrdiff_t(std::min(kWalkerSlots, slotUsed_.size()));
            const auto free = std::find(slotUsed_.begin(), walkers, false);
            if (model.empty() || free == walkers || !world_) {
                core::LogWarning("game: quest scene: {} ({}) not placed ({})", arg(0), arg(1),
                                 !crowd && row == nullptr ? "no such character" : model.empty() ? "no model" : "no free stage slot");
                continue;
            }
            *free = true;
            const std::size_t slot = std::size_t(free - slotUsed_.begin());
            WorldScreen::StageLook look{model};
            if (crowd) {
                look = *personLook;
                look.state = kSceneStand;
                look.clipSet.clear();
                look.cycleSet.clear();
                look.once = false;
            } else {
                look.height = float(row->scalePercent) / 100.0f;
            }
            const float x = c.Number(4) + float(int(c.Number(2)) - originX_) * assets::kRegionSize;
            const float z = c.Number(6) + float(int(c.Number(3)) - originY_) * assets::kRegionSize;
            const float yaw = c.Number(7) * 3.14159265f / 180.0f;
            world_->SetStage(slot, look, x, c.Number(5), z, yaw);
            scene_.objects.emplace_back(arg(0), slot);
            QuestSceneRun::Placed placed;
            placed.look = look;
            placed.look.cycle = kSceneStand;
            placed.x = x, placed.y = c.Number(5), placed.z = z, placed.yaw = yaw;
            placed.region = SceneRegion(c, 2);
            placed.placed = true;
            if (row != nullptr) placed.walkSpeed = row->walkSpeed, placed.runSpeed = row->runSpeed;
            placed.person = person;
            scene_.looks[arg(0)] = std::move(placed);
        } else if (c.verb == "S_ObjectEquipSetIndex") {
            // <name> <level> <armour kind> <weapon kind> <rare>: 1.188 dresses its crowd's figure in gear of that level and
            // those kinds; ours' crowd are the players there, in their own gear: nothing to change.
        } else if (c.verb == "S_ObjectAniToIndex") {
            // <name> <blend> <speed> *ANI_...: a state of its own set (*ANI_DIE1, *ANI_ATTREADY, ...); *ANI_EMOTION01..
            // the default set's animation 50 + n, as the emotes. Ours: at its own speed.
            std::string ani = arg(3);
            if (ani.starts_with('*')) ani.erase(0, 1);
            QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0));
            if (placed != nullptr && ani.starts_with("ANI_EMOTION")) {
                const auto emote = proto::kEmoteFirstState + std::uint32_t(std::max(0, std::atoi(ani.c_str() + 11) - 1));
                PlayClip(*placed, "default", emote, c.time);
            } else if (const int state = assets::AnimationState(ani); placed != nullptr && state >= 0) {
                PlayClip(*placed, std::string(), std::uint32_t(state), c.time);
            }
        } else if (c.verb == "S_ObjectAniToName") {
            // <name> *SCR_ANI_BLEND | *SCR_ANI_NOBLEND <speed> <.ban>: that clip, looping or once as the clip is (the
            // original asks the clip which; NOBLEND stops what plays first). Ours: at its own speed.
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr && !arg(3).empty()) {
                PlayClip(*placed, std::string(WorldScreen::kClipFile) + arg(3), kSceneStand, c.time);
            }
        } else if (c.verb == "S_ObjectAniStopAll") {
            // Its clips stopped: it stands.
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr) PlayClip(*placed, std::string(), kSceneStand, c.time);
        } else if (c.verb == "S_ObjectOptionSet") {
            // <name> *SCR_OBJ_...: NOIDLE (a clip played through holds its last frame, never back to standing),
            // NODRAWNAME (no names show in a scene), WALKSPEED / RUNSPEED <speed>, NODRAW / DRAW, NOFRUSTUM / FRUSTUM
            // (drawn out of the view's cull: ours always).
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr) {
                const std::string option = arg(1);
                if (option == "*SCR_OBJ_NOIDLE") placed->noIdle = true;
                if (option == "*SCR_OBJ_WALKSPEED") placed->walkSpeed = std::max(0.1f, c.Number(2, placed->walkSpeed));
                if (option == "*SCR_OBJ_RUNSPEED") placed->runSpeed = std::max(0.1f, c.Number(2, placed->runSpeed));
                if (option == "*SCR_OBJ_NODRAW") placed->hidden = true;
                if (option == "*SCR_OBJ_DRAW") placed->hidden = false;
            }
        } else if (c.verb == "S_ObjectMotionState") {
            // <name> 1 walking, 2 running (0 the original's own default: ours running).
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr) placed->walk = int(c.Number(1)) == 1;
        } else if (c.verb == "S_ObjectMoveTo") {
            // <name> <sectorX> <sectorY> <x> <y> <z>: on its way there, facing the way it goes.
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr) {
                const glm::vec3 to = ScenePlace(c, 1, originX_, originY_);
                placed->toX = to.x, placed->toY = to.y, placed->toZ = to.z;
                placed->region = SceneRegion(c, 1);
                placed->moving = true;
                placed->look.held = false;
                placed->placed = true;
            }
        } else if (c.verb == "S_ObjectTurn") {
            // <name> <yaw (degrees)> <its mount too>: turned at once.
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr) {
                placed->yaw = c.Number(1) * kDegrees;
                placed->placed = true;
            }
        } else if (c.verb == "S_Teleport") {
            // <name> <sectorX> <sectorY> <x> <y> <z> <yaw (degrees)>: there at once (the player only on the screen: the
            // server never moved it).
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr) {
                const glm::vec3 at = ScenePlace(c, 1, originX_, originY_);
                placed->x = at.x, placed->y = at.y, placed->z = at.z;
                placed->region = SceneRegion(c, 1);
                placed->yaw = c.Number(6) * kDegrees;
                placed->moving = false;
                placed->placed = true;
            }
        } else if (c.verb == "S_DecoEftAdd") {
            // <name> <x> <y> <z> <flag> <size> <ms> <.efp> <bone>: an effect on its bone, offset in it, kept until
            // S_DecoEftClear (ms 0) or that long. Ours: the flag (the original passes it on) not read; the number before
            // the time read as its size.
            QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0));
            const std::size_t slot = QuestSceneSlot(arg(0));
            if (placed != nullptr && slot != StageEffects::kNone && world_ && !arg(7).empty()) {
                gfx::EffectSystem::Request request;
                request.path = arg(7);
                request.anchor.target = WorldScreen::StageAnchor(slot);
                request.anchor.bone = arg(8);
                request.anchor.offset = glm::vec3(c.Number(1), c.Number(2), c.Number(3));
                request.scale = std::max(0.01f, c.Number(5, 1.0f));
                const float ms = c.Number(6);
                request.seconds = ms > 0.0f ? ms / 1000.0f : -1.0f;
                request.kind = gfx::EffectSystem::Kind::World;
                if (const auto handle = world_->Effects().Start(request); handle != 0) placed->effects.push_back(handle);
            }
        } else if (c.verb == "S_DecoEftClear") {
            if (QuestSceneRun::Placed* placed = QuestScenePlaced(arg(0)); placed != nullptr) {
                if (world_) {
                    for (const std::uint32_t effect : placed->effects) world_->Effects().Stop(effect);
                }
                placed->effects.clear();
            }
        } else if (c.verb == "S_EffectCreate") {
            // <name> <.efp> <sectorX> <sectorY> <x> <y> <z> <yaw (degrees)> <size> <kept>: an effect in place, played
            // through (kept 1: until S_EffectRelease).
            if (world_ && !arg(1).empty()) {
                const glm::vec3 at = ScenePlace(c, 2, originX_, originY_);
                gfx::EffectSystem::Request request;
                request.path = arg(1);
                request.at = glm::rotate(glm::translate(glm::mat4(1.0f), at), c.Number(7) * kDegrees, glm::vec3(0.0f, 1.0f, 0.0f));
                request.scale = std::max(0.01f, c.Number(8, 1.0f));
                request.seconds = c.Number(9) != 0.0f ? -1.0f : 0.0f;
                request.kind = gfx::EffectSystem::Kind::World;
                if (const auto old = scene_.effects.find(arg(0)); old != scene_.effects.end()) world_->Effects().Release(old->second);
                scene_.effects[arg(0)] = world_->Effects().Start(request);
            }
        } else if (c.verb == "S_EffectRelease") {
            if (const auto it = scene_.effects.find(arg(0)); it != scene_.effects.end()) {
                if (world_) world_->Effects().Release(it->second);
                scene_.effects.erase(it);
            }
        } else if (c.verb == "S_TextureFade") {
            // <name> <screen h> <screen v> <its own h> <its own v> <w> <h> <x> <y> <in s> <held s> <out s> <picture>:
            // placed as the statics, each a picture of its own (a later one of the same name over it: the original
            // keeps them all).
            QuestSceneRun::Fade fade;
            fade.screenH = SceneAnchor(arg(1)), fade.screenV = SceneAnchor(arg(2)), fade.ownH = SceneAnchor(arg(3)), fade.ownV = SceneAnchor(arg(4));
            fade.w = c.Number(5), fade.h = c.Number(6), fade.x = c.Number(7), fade.y = c.Number(8);
            fade.in = std::max(0.0f, c.Number(9)), fade.hold = std::max(0.0f, c.Number(10)), fade.out = std::max(0.0f, c.Number(11));
            fade.path = arg(12);
            fade.from = c.time;
            if (!fade.path.empty()) scene_.fades.push_back(std::move(fade));
        } else if (c.verb == "S_ObjectRelease") {
            const auto it = std::find_if(scene_.objects.begin(), scene_.objects.end(), [&](const auto& o) { return o.first == arg(0); });
            if (it != scene_.objects.end()) {
                if (world_) world_->ClearStage(it->second);
                if (it->second < slotUsed_.size()) slotUsed_[it->second] = false;
                scene_.objects.erase(it);
            }
            if (const auto placed = scene_.looks.find(arg(0)); placed != scene_.looks.end()) {
                if (world_) {
                    for (const std::uint32_t effect : placed->second.effects) world_->Effects().Stop(effect);
                }
                scene_.looks.erase(placed);
            }
        } else if (c.verb == "S_StaticCreateEx") {
            // <name> <screen h> <screen v> <its own h> <its own v> <w> <h> <x> <y> <text h> <text v> <r> <g> <b> <font> <text>
            // (the original's SC_StaticCreateEx: POS_LEFT / TOP 0, POS_CENTER 1, POS_RIGHT / BOTTOM 2; the text a key after
            // '*', else as written).
            const auto pos = [&](std::size_t i) { return SceneAnchor(arg(i)); };
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
        } else if (c.verb == "S_Lighting") {
            // <seconds> <alpha> <r> <g> <b> <kind> [<in> <held> <out>]: the screen washed with a colour for its time, then
            // gone: kind 0 from the colour to nothing, 1 from nothing to the colour, 3 in, held and out over its three
            // times, another the alpha as given.
            QuestSceneRun::Wash wash;
            wash.from = c.time;
            wash.seconds = std::max(0.001f, c.Number(0));
            const auto byte = [&](std::size_t i) { return std::uint32_t(std::clamp(int(c.Number(i)), 0, 255)); };
            wash.alpha = float(byte(1)) / 255.0f;
            wash.rgb = byte(2) << 16 | byte(3) << 8 | byte(4);
            wash.kind = int(c.Number(5));
            wash.in = std::max(0.0f, c.Number(6)), wash.hold = std::max(0.0f, c.Number(7)), wash.out = std::max(0.0f, c.Number(8));
            scene_.washes.push_back(wash);
        } else if (c.verb == "S_SetBGM") {
            // <name> <flag>: the scene's music over the area's until S_End (1.188 plays a Music.pk2 file by that name on
            // its own channel). The Roc's names a file the game does not have ("괴조로크"): ours plays the scene's own
            // track then, its name's .ogg (roc_entrance.ogg).
            std::string track;
            for (const std::string& name : {arg(0), arg(0) + ".ogg", scene_.name + ".ogg"}) {
                if (!name.empty() && context.files.Exists(name)) {
                    track = name;
                    break;
                }
            }
            if (!track.empty() && context.audio != nullptr) {
                context.audio->SceneMusic(track);
                scene_.music = context.audio;
            } else {
                unshown(c.verb);
            }
        } else if (c.verb == "S_DecoSkillTarget") {
            unshown(c.verb);
        }
        // S_ViewSightRanger, S_CameraSetState: nothing of ours to change.
    }
    // A scene whose script never ends: over 5 s after its last command.
    if (scene_.active && scene_.next >= scene_.commands.size() && t > scene_.commands.back().time + 5.0f) EndQuestScene();
}

void PlayScreen::PlayClip(QuestSceneRun::Placed& placed, std::string set, std::uint32_t state, float at) {
    placed.look.clipSet = std::move(set);
    placed.look.state = state;
    placed.look.take += 1;
    placed.look.held = false;
    placed.look.cycle = kSceneStand;
    placed.look.cycleSet.clear();
    placed.clipFrom = at;
}

PlayScreen::QuestSceneRun::Placed* PlayScreen::QuestScenePlaced(const std::string& name) {
    if (const auto it = scene_.looks.find(name); it != scene_.looks.end()) return &it->second;
    if (name != "*player" || !world_) return nullptr;
    // The player, first named: from where it stands.
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr) return nullptr;
    QuestSceneRun::Placed placed;
    const glm::vec3 at = world_->OriginRelative(self->position);
    placed.x = at.x, placed.y = at.y, placed.z = at.z, placed.yaw = self->yaw;
    placed.region = self->position.region;
    placed.look.cycle = kSceneStand;
    return &scene_.looks.emplace(name, std::move(placed)).first->second;
}

bool PlayScreen::SceneHides(std::uint32_t entity) const {
    if (!scene_.active) return false;
    if (entity == view_.Self()) return scene_.hideSelf;
    for (const auto& [name, placed] : scene_.looks) {
        if (placed.person == entity) return true;
    }
    return false;
}

std::size_t PlayScreen::QuestSceneSlot(const std::string& name) const {
    if (name == "*player") return SlotOf(view_.Self());
    const auto it = std::find_if(scene_.objects.begin(), scene_.objects.end(), [&](const auto& o) { return o.first == name; });
    return it != scene_.objects.end() ? it->second : StageEffects::kNone;
}

void PlayScreen::PlaceQuestSceneActors(ScreenContext& context) {
    if (!scene_.active || !world_) return;
    const float t = float(context.time - scene_.started);
    for (auto& [name, placed] : scene_.looks) {
        const std::size_t slot = QuestSceneSlot(name);
        if (slot == StageEffects::kNone) continue;
        const bool player = name == "*player";
        if (player && !placed.placed) continue; // named but never moved: where the game has it
        if (placed.moving) {
            const float dx = placed.toX - placed.x, dz = placed.toZ - placed.z;
            const float distance = std::hypot(dx, dz);
            const float step = (placed.walk ? placed.walkSpeed : placed.runSpeed) * float(context.dt);
            if (distance > 0.01f) placed.yaw = std::atan2(dx, dz);
            if (step >= distance) {
                placed.x = placed.toX, placed.y = placed.toY, placed.z = placed.toZ;
                placed.moving = false;
            } else {
                const float f = step / distance;
                placed.x += dx * f, placed.z += dz * f;
                placed.y += (placed.toY - placed.y) * f;
            }
        }
        // A clip played through, not a loop: held on its last frame (*SCR_OBJ_NOIDLE), else it stands again. (A
        // clip of a named set holds its first frame through its 200 ms blend first.)
        if (!placed.look.held && (placed.look.state != kSceneStand || !placed.look.clipSet.empty())) {
            bool loops = false;
            const double seconds = world_->StageClipSeconds(slot, placed.look.clipSet, placed.look.state, &loops);
            const double ends = placed.clipFrom + seconds + (placed.look.clipSet.empty() ? 0.0 : 0.2);
            if (seconds > 0.0 && !loops && placed.noIdle && t >= ends - 0.03) {
                placed.look.held = true;
            } else if (seconds > 0.0 && !loops && !placed.noIdle && t > ends + 0.2) {
                placed.look.clipSet.clear();
                placed.look.state = kSceneStand;
                placed.look.take += 1;
            }
        }
        WorldScreen::StageLook look = placed.look;
        if (player) {
            const game::WorldView::Entity* self = view_.Find(view_.Self());
            const WorldScreen::StageLook* dressed = self != nullptr ? LookOf(context, *self) : nullptr;
            if (dressed == nullptr) continue;
            look = *dressed;
            look.state = placed.look.state;
            look.clipSet = placed.look.clipSet;
            look.take = placed.look.take;
            look.held = placed.look.held;
            look.cycle = placed.look.cycle;
            look.cycleSet = placed.look.cycleSet;
        }
        // On its way: walking or running under whatever clip the script gave it (a lunge's), else the walk or run alone.
        if (placed.moving) {
            look.cycle = placed.walk ? kSceneWalk : kSceneRun;
            look.cycleSet.clear();
            if (look.clipSet.empty()) look.state = look.cycle;
        }
        // On the ground under the script's height (the original's characters stand on their navigation; the script's
        // heights are near it, a little above or below): the floor dropped onto from just over it.
        float y = placed.y;
        game::proto::WorldPosition ground;
        int regionX = placed.region & 0xFF, regionY = placed.region >> 8;
        if ((placed.region & 0x8000) == 0) {
            regionX = int(std::floor(placed.x / assets::kRegionSize)) + originX_;
            regionY = int(std::floor(placed.z / assets::kRegionSize)) + originY_;
        }
        ground.region = std::uint16_t((regionX & 0xFF) | (regionY & 0xFF) << 8);
        ground.x = placed.x - float(regionX - originX_) * assets::kRegionSize;
        ground.z = placed.z - float(regionY - originY_) * assets::kRegionSize;
        ground.y = placed.y;
        if (float floor = 0.0f; world_->GroundHeight(ground, floor)) {
            y = floor;
        } else if (ground.y = placed.y + kSceneGroundReach; world_->GroundHeight(ground, floor)) {
            y = floor;
        }
        world_->SetStage(slot, look, placed.x, y, placed.z, placed.yaw);
        world_->SetStageOpacity(slot, placed.hidden ? 0.0f : 1.0f);
        // A player in the crowd: its own figure hidden while its copy stands there, the copy wearing its glows once
        // its model is in.
        if (placed.person != 0) {
            if (const std::size_t own = SlotOf(placed.person); own != StageEffects::kNone) world_->SetStageOpacity(own, 0.0f);
            const game::WorldView::Entity* e = !placed.glowing && tables_ ? view_.Find(placed.person) : nullptr;
            if (e != nullptr && world_->StageReady(slot)) {
                placed.glowing = true;
                std::vector<std::uint32_t> worn;
                std::vector<std::uint8_t> plus;
                for (const proto::EquippedItem& item : e->look.equipment) {
                    worn.push_back(item.item);
                    plus.push_back(item.plus);
                }
                for (const auto h : StageEffects::ItemGlows(*world_, slot, context.files, tables_->items, tables_->itemGlows, worn, plus)) placed.effects.push_back(h);
            }
        }
    }
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
    // Its texts and pictures: the screen's anchor (none, half, all of its width or height), less its own share of its
    // size, plus its offset (the original's place of a scene's static), a text aligned in it.
    const auto along = [](int anchor, float full) { return anchor == 2 ? full : anchor == 1 ? std::floor(full / 2.0f) : 0.0f; };
    const auto own = [](int anchor, float size) { return anchor == 2 ? size : anchor == 1 ? std::floor(size / 2.0f) : 0.0f; };
    // The pictures in, held and out (over the bars, the texts over them), each gone once out.
    const float t = float(context.time - scene_.started);
    std::erase_if(scene_.fades, [&](const QuestSceneRun::Fade& f) { return t > f.from + f.in + f.hold + f.out; });
    for (const QuestSceneRun::Fade& f : scene_.fades) {
        const float into = t - f.from;
        if (into < 0.0f) continue;
        const float alpha = into < f.in ? into / std::max(0.001f, f.in)
                            : into < f.in + f.hold ? 1.0f
                                                   : std::max(0.0f, 1.0f - (into - f.in - f.hold) / std::max(0.001f, f.out));
        const ui::Rect r{along(f.screenH, w) - own(f.ownH, f.w) + f.x, along(f.screenV, h) - own(f.ownV, f.h) + f.y, f.w, f.h};
        paint.Image(f.path, r, {0, 0}, {1, 1}, ui::Argb((std::uint32_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << 24) | 0xFFFFFFu));
    }
    // The colour washes over the scene, the bars and the pictures (the texts over them), each gone after its time.
    std::erase_if(scene_.washes, [&](const QuestSceneRun::Wash& wash) { return t > wash.from + wash.seconds; });
    for (const QuestSceneRun::Wash& wash : scene_.washes) {
        const float into = t - wash.from;
        if (into < 0.0f) continue;
        float alpha = wash.alpha;
        if (wash.kind == 0) alpha = 1.0f - into / wash.seconds;
        if (wash.kind == 1) alpha = into / wash.seconds;
        if (wash.kind == 3) {
            alpha = into < wash.in ? into / std::max(0.001f, wash.in)
                    : into < wash.in + wash.hold ? 1.0f
                                                 : 1.0f - (into - wash.in - wash.hold) / std::max(0.001f, wash.out);
        }
        const std::uint32_t a = std::uint32_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
        paint.Fill({0.0f, 0.0f, w, h}, ui::Argb(a << 24 | wash.rgb));
    }
    for (const QuestSceneRun::Static& st : scene_.statics) {
        const ui::Rect r{along(st.screenH, w) - own(st.ownH, st.w) + st.x, along(st.screenV, h) - own(st.ownV, st.h) + st.y, st.w, st.h};
        ui::DrawLabel(paint, st.text, r, ui::Argb(st.color), st.font, st.textH, st.textV, ui::kTextShadow);
    }
}

} // namespace sr::client
