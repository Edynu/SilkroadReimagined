#include "play/PlayScreen.h"
#include "gui/ErrorTexts.h"
#include "sr/game/BetaQuests.h"
#include "sr/game/TimedServices.h"
#include "sr/game/Cos.h"
#include "sr/game/Jobs.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include <set>
#include <format>
#include <utility>

#include <imgui.h>

#include "sr/assets/Dress.h"
#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Trace.h"
#include "sr/game/Loading.h"
#include "sr/game/Berserk.h"
#include "sr/game/SpecialItems.h"
#include "sr/game/SkillCatalogue.h"
#include "sr/game/Skills.h"
#include "gui/HudWindows.h"
#include "gui/UiWarmup.h"
#include "sr/game/Cos.h"
#include "audio/GameAudio.h"
#include "sr/audio/Decode.h"
#include "title/CameraKeys.h"

namespace sr::client {

const char* MallItemErrorKey(std::uint16_t code); // PlayMallItems.cpp

namespace proto = game::proto;

namespace {

// The original's entity pick: nine rays, the cursor's and eight about
// it at 1.6 % of the screen's width across and of its height down; the nearest on the cursor's own ray wins, the side
// rays count only when it meets nothing (`centre` says which; across the kinds too, ClickWorld). `pick(rx, ry, distance)`
// gives the nearest on one ray (0 none).
template <typename Pick>
std::uint32_t OverNineRays(float x, float y, float width, float height, Pick pick, float* outDistance, bool* centre = nullptr) {
    float best = 1e9f;
    std::uint32_t id = pick(x, y, best);
    if (centre != nullptr) *centre = id != 0;
    if (id == 0) {
        const float dx = width * 0.016f, dy = height * 0.016f;
        for (int i = -1; i <= 1; ++i) {
            for (int j = -1; j <= 1; ++j) {
                if (i == 0 && j == 0) continue;
                float d = 1e9f;
                const std::uint32_t got = pick(x + float(i) * dx, y + float(j) * dy, d);
                if (got != 0 && d < best) {
                    best = d;
                    id = got;
                }
            }
        }
    }
    if (outDistance != nullptr) *outDistance = id != 0 ? best : 1e9f;
    return id;
}


constexpr std::uint32_t kStand = 0, kAttReady = 6, kWalk = 1, kRun = 7, kPick = 38; // animation states (STAND1, ATTREADY, WALK, RUN, PICK)
constexpr std::uint32_t kDamage = 3, kDie = 4, kDieLoop = 36;   // DAMAGE1, DIE1, DIE1_RM
constexpr std::uint32_t kSitDown = 13, kSitting = 14, kStandUp = 15; // the clips *_a_sitground, *_a_sitbreath, *_a_sitstand
// States and knocks: STUN, DAMAGE2, DOWN, DOWN_RM, DOWN_DAMAGE, DOWN_UP, DOWN_DIE.
constexpr std::uint32_t kStun = 79, kDamage2 = 9, kDown = 62, kDownLoop = 63, kDownDamage = 64, kDownUp = 65, kDownDie = 66;
constexpr double kDownFallSeconds = 0.6, kDownRiseSeconds = 0.8; // ours: the clips' own lengths are not read yet
constexpr double kFadeInSeconds = 2.0, kFadeOutSeconds = 1.5; // CIDecoAppear, CIDecoDisappear

// A monster's size: its reference's times its grade's (champion 1.5, giant 3, elite 1.7; a party monster's by its grade).
float MonsterScale(const assets::ObjectRow* row, std::uint8_t rarity) {
    const float base = row != nullptr ? float(row->scalePercent) / 100.0f : 1.0f;
    const int grade = rarity & 0x0F;
    return base * (grade == 1 ? 1.5f : grade == 4 ? 3.0f : grade == 6 ? 1.7f : 1.0f);
}
constexpr double kPickSeconds = 1.0;
constexpr float kFadeSeconds = 0.5f;
// Ours until the original's are known: a click this near a gate's spot means the gate, or one on
// its crystal (an upright cylinder this wide and tall over the spot; the gate rows' click box,
// columns 48..50, is not read yet). A gate's menu opens from up to 800 away (the original's; the server takes
// a gate structure from 800); farther, a walk to a random point 20..640 from it first.
constexpr float kGateClick = 40.0f, kGateCrystalRadius = 30.0f, kGateCrystalHeight = 120.0f, kGateReach = 800.0f;
constexpr float kGateWalkNearest = 20.0f, kGateWalkFarthest = 640.0f;
// An NPC clicked farther than this is walked to first (to a random 20..240 from it), nearer talked to at once (1.188).
constexpr float kCameraFarthest = 300.0f; // the follow camera's farthest (ours; the original's 150)
constexpr float kCameraPitchLimit = 1.0707964f; // the original's: half pi less 0.5, both ways
constexpr float kNpcReach = 240.0f, kNpcWalkNearest = 20.0f;

// Gold (TID 3/3/5, not the TID4 1 tokens and coins sharing it:'s test).
bool IsGold(const assets::ItemRow& row) {
    return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 5 && row.typeId[3] != 1;
}
// Gold's coin shower (the 1.188 client's): a skeleton and a 1 s clip of its own.
constexpr const char* kGoldShower = "res\\item\\etc\\drop_ch_money_ing.bsr";
constexpr double kShowerWait = 3.0; // ours: a shower still not in by then gives way to its heap

} // namespace

void PlayScreen::Enter(ScreenContext& context) {
    netOff_ = false;
    quitNow_ = false;
    portraits_.Init(context.device);
    madeFiles_ = context.madeFiles;
    shared_ = TitleShared::From(context);
    context.handoff.erase(TitleShared::kHandoff);
    if (const std::string replay = context.config.GetString("replay", ""); !replay.empty() && !shared_) {
        // A recorded session instead of a server: no session, the tables still needed to dress.
        std::string error;
        if (!recording_.Load(replay, error)) {
            core::LogError("game: {}", error);
            context.quit = true;
            context.exitCode = 1;
            return;
        }
        core::LogInfo("game: replaying {} ({} messages, {:.1f} s)", replay, recording_.messages.size(), recording_.Duration());
        replay_ = std::make_unique<game::WorldReplay>(recording_);
        replayTime_ = recording_.messages.empty() ? 0.0 : recording_.messages.front().time;
        shared_ = std::make_shared<TitleShared>();
        shared_->LoadCharacters(context.jobs, context.files);
        tables_ = shared_->characters;
        slotUsed_.assign(kSlots, false);
        return;
    }
    if (const std::string filter = context.config.GetString("skill-bench", ""); !filter.empty() && !shared_) {
        // The skill bench: a made-up session, built once the tables are in (Update).
        benchFilter_ = filter;
        benchCaptures_ = context.config.GetString("bench-captures", "");
        benchVideo_ = context.config.GetString("bench-video", "");
        // --bench-view side: the camera at the caster's side, farther out (a flight crosses the picture).
        const std::string view = context.config.GetString("bench-view", "");
        benchView_ = view == "side" ? 1 : view == "close" ? 2 : 0;
        // --bench-burst N[,S]: N pictures of each skill instead, S seconds apart from its cast (0.05).
        if (const std::string burst = context.config.GetString("bench-burst", ""); !burst.empty()) {
            benchBurst_ = std::max(1, std::atoi(burst.c_str()));
            if (const std::size_t comma = burst.find(','); comma != std::string::npos) benchBurstStep_ = std::max(0.01, std::atof(burst.c_str() + comma + 1));
        }
        benchPending_ = true;
        shared_ = std::make_shared<TitleShared>();
        shared_->LoadCharacters(context.jobs, context.files);
        tables_ = shared_->characters;
        slotUsed_.assign(kSlots, false);
        return;
    }
    if (!shared_) {
        core::LogWarning("game: no title session handed over; back to the title");
        next = "title";
        return;
    }
    shared_->LoadCharacters(context.jobs, context.files);
    tables_ = shared_->characters;
    slotUsed_.assign(kSlots, false);
    gui_.Load(context);
    const std::string walk = context.config.GetString("walk", "");
    if (!walk.empty()) {
        walk_ = std::sscanf(walk.c_str(), "%f,%f", &walkX_, &walkZ_) == 2;
        if (!walk_) core::LogWarning("--walk {}: expected dx,dz", walk);
        walkWait_ = float(context.config.GetDouble("walk-after", 0.0)); // --walk-after S: it stands that long first
    }
    leaveAfter_ = float(context.config.GetDouble("leave-after", -1.0));
    logoutAfter_ = shared_ && shared_->restarted ? -1.0f : float(context.config.GetDouble("logout-after", -1.0)); // once a run
    dieAfter_ = float(context.config.GetDouble("die-after", -1.0));
    acceptRevival_ = context.config.GetBool("accept-revival", false);
    if (const std::string r = context.config.GetString("revive", ""); !r.empty()) {
        unsigned skill = 0;
        float seconds = 0.0f;
        if (std::sscanf(r.c_str(), "%u,%f", &skill, &seconds) == 2) {
            reviveSkill_ = skill;
            reviveAfter_ = seconds;
        } else {
            core::LogWarning("--revive {}: expected skill,seconds", r);
        }
    }
    if (const std::string list = context.config.GetString("cast", ""); !list.empty()) {
        for (std::size_t at = 0; at <= list.size();) {
            std::size_t end = list.find(';', at);
            if (end == std::string::npos) end = list.size();
            unsigned skill = 0;
            float seconds = 0.0f;
            if (std::sscanf(list.substr(at, end - at).c_str(), "%u,%f", &skill, &seconds) == 2) castScript_.push_back({skill, seconds});
            at = end + 1;
        }
    }
    selectPlayer_ = context.config.GetString("select-player", "");
    berserkAfter_ = float(context.config.GetDouble("berserk-after", -1.0));
    chatAt_ = context.config.GetDouble("chat-after", 2.0);
    GuildOptions(context);
    FortressOptions(context);
    nameDistance_ = float(context.config.GetDouble("name-distance", 400.0));
    openInventory_ = context.config.GetBool("inventory", false);
    if (const std::string at = context.config.GetString("mouse", ""); !at.empty()) std::sscanf(at.c_str(), "%f,%f", &mouse_.x, &mouse_.y);
    if (const std::string m = context.config.GetString("move-item", ""); !m.empty()) std::sscanf(m.c_str(), "%d,%d", &moveFrom_, &moveTo_);
    for (std::string lines = context.config.GetString("chat", ""); !lines.empty();) {
        const std::size_t bar = lines.find('|');
        chatScript_.push_back(lines.substr(0, bar));
        lines = bar == std::string::npos ? std::string() : lines.substr(bar + 1);
    }
    clickAround_ = float(context.config.GetDouble("click-around", 0.0));
    const std::string click = context.config.GetString("click", "");
    if (!click.empty() && std::sscanf(click.c_str(), "%f,%f", &clickX_, &clickY_) != 2) core::LogWarning("--click {}: expected fx,fy", click);
    clickAfter_ = context.config.GetDouble("click-after", 0.0);
    // --click-at region,x,z: a click on the ground there, as ClickWorld's walk takes it (WalkTo; tests of the movement).
    if (const std::string at = context.config.GetString("click-at", ""); !at.empty()) {
        unsigned region = 0;
        float x = 0, z = 0;
        if (std::sscanf(at.c_str(), "%u,%f,%f", &region, &x, &z) == 3) clickAt_ = proto::WorldPosition{std::uint16_t(region), x, 0.0f, z};
        else core::LogWarning("--click-at {}: expected region,x,z", at);
    }
    // --movement classic|pathfinding: the Option window's Beta choice for this run (tests).
    if (const std::string movement = context.config.GetString("movement", ""); !movement.empty() && context.options != nullptr) {
        context.options->beta.movement = movement == "pathfinding" ? core::BetaOptions::Movement::Pathfinding : core::BetaOptions::Movement::Classic;
    }
    // --names original|far: the Beta page's names over heads for this run (tries).
    if (const std::string names = context.config.GetString("names", ""); !names.empty() && context.options != nullptr) {
        context.options->beta.namesFar = names == "far";
    }
    if (const int gate = context.config.GetInt("use-gate", 0); gate > 0) {
        pendingGate_ = std::uint32_t(gate);
        gateTries_ = 20;
        holdForGate_ = true; // a capture waits for the far side
    }
    gateTo_ = std::uint32_t(std::max(0, context.config.GetInt("gate-to", 0)));
    goldBox_ = context.config.GetBool("gold-box", false);
    if (context.config.GetBool("gate-menu", false)) {
        talkAtGate_ = true;
        holdForGate_ = false;
        const std::string choose = context.config.GetString("gate-choose", "");
        for (std::size_t at = 0; at < choose.size();) {
            std::size_t end = choose.find(',', at);
            if (end == std::string::npos) end = choose.size();
            gateScript_.push_back(choose.substr(at, end - at));
            at = end + 1;
        }
    }
    gotoPoint_ = std::uint32_t(std::max(0, context.config.GetInt("goto", 0)));
    if (gotoPoint_ != 0) holdForGate_ = waitingForGate_ = true; // a capture waits for the far side
    recordPath_ = context.config.GetString("record", "");
    if (!recordPath_.empty()) shared_->session->Record(&recording_);
    PartyEnter(context); // the scripted party tries (PlayParty.cpp)
    AutoPotionEnter(context); // the auto potion's Confirm and tries (PlayAutoPotion.cpp)
    shared_->session->EnterWorld();
}

void PlayScreen::Script(ScreenContext& context) {
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr || leaving_ || replay_) return;
    GuildScript(context);
    CommunityTick(context.time);
    if (!textLater_.empty() && gui_.data().Ready()) {
        auto later = std::move(textLater_);
        textLater_.clear();
        for (auto& post : later) post(context.time);
    }
    FortressScript(context);
    AutoPotionScript(context);
    RouteFrame(context);
    if (logoutAfter_ >= 0.0f && (logoutAfter_ -= float(context.dt)) < 0.0f) {
        core::LogInfo("game: Restart (--logout-after)");
        if (shared_) shared_->restarted = true;
        hud_.Request({HudRequest::Kind::Restart});
    }
    if (dieAfter_ >= 0.0f && (dieAfter_ -= float(context.dt)) < 0.0f) {
        core::LogInfo("game: a developer's death (--die-after)");
        shared_->session->DevDie();
    }
    if (berserkAfter_ >= 0.0f && (berserkAfter_ -= float(context.dt)) < 0.0f) {
        core::LogInfo("game: --berserk-after: the request (gauge {})", player_.self.berserk);
        shared_->session->Berserk();
    }
    if (!selectPlayer_.empty()) {
        for (const auto& [id, e] : view_.Entities()) {
            if (id == view_.Self() || e.kind != proto::EntityKind::Character || e.look.name != selectPlayer_) continue;
            core::LogInfo("game: --select-player: {} ({})", selectPlayer_, id);
            SelectPlayer(id);
            selectPlayer_.clear();
            break;
        }
    }
    for (auto it = castScript_.begin(); it != castScript_.end();) {
        if ((it->second -= float(context.dt)) >= 0.0f) {
            ++it;
            continue;
        }
        core::LogInfo("game: --cast: skill {}", it->first);
        shared_->session->UseSkill(it->first, player_.target.Selected() && player_.target.entity != view_.Self() ? player_.target.entity : 0);
        it = castScript_.erase(it);
    }
    if (reviveSkill_ != 0 && (reviveAfter_ -= float(context.dt)) < 0.0f) {
        // The nearest dead player shown (no NPC, no living one).
        std::uint32_t nearest = 0;
        float best = 1e9f;
        for (const auto& [id, e] : view_.Entities()) {
            if (id == view_.Self() || e.kind != proto::EntityKind::Character || IsNpc(e) || !IsDead(e)) continue;
            const float d = std::hypot(e.position.x - self->position.x, e.position.z - self->position.z);
            if (e.position.region == self->position.region && d < best) {
                best = d;
                nearest = id;
            }
        }
        core::LogInfo("game: --revive: skill {} on {}", reviveSkill_, nearest);
        if (nearest != 0) shared_->session->UseSkill(reviveSkill_, nearest);
        reviveSkill_ = 0;
    }
    if (openInventory_ && hud_.Created()) {
        hud_.OpenPage(hud::kPageInventory);
        openInventory_ = false;
    }
    if (moveFrom_ >= 0 && hud_.Created() && (moveAt_ -= context.dt) < 0.0) {
        core::LogInfo("game: --move-item {} -> {}", moveFrom_, moveTo_);
        hud_.Request({HudRequest::Kind::MoveItem, moveFrom_, {}, {}, moveTo_});
        moveFrom_ = -1;
        chatHold_ = 3.0; // captures wait for the answer
    }
    if (!chatScript_.empty() && hud_.Created() && (chatAt_ -= context.dt) < 0.0) {
        // Typed into the chat's input line and sent with Enter, as the player would.
        if (auto* chat = dynamic_cast<ChatWindow*>(gui_.interface() ? gui_.interface()->Window(hud::kChat) : nullptr)) {
            chat->FocusInput();
            chat->input()->SetValue(chatScript_.front());
            chat->Submit(chatScript_.front());
        }
        chatScript_.erase(chatScript_.begin());
        chatAt_ = 2.0;
        chatHold_ = 3.0; // the answer, and what the others say back
    }
    if (!chatScript_.empty() || moveFrom_ >= 0 || (chatHold_ -= context.dt) > 0.0) context.holdCapture = true;
    if (leaveAfter_ >= 0.0f && (leaveAfter_ -= float(context.dt)) < 0.0f) {
        core::LogInfo("game: leaving the world (--leave-after)");
        leaving_ = true;
        shared_->session->LeaveWorld();
        return;
    }
    if (clickAt_ && (clickAfter_ -= context.dt) < 0.0) {
        proto::WorldPosition at = *clickAt_;
        world_->GroundHeight(at, at.y);
        core::LogInfo("game: --click-at: {} ({:.0f}, {:.0f}, {:.0f})", at.region, at.x, at.y, at.z);
        WalkTo(context, at, false);
        moveMarker_ = {at, context.time};
        clickAt_.reset();
    }
    if (clickX_ >= 0.0f && (clickAfter_ -= context.dt) < 0.0) {
        // --click fx,fy [--click-after S]: the player's click there (ClickWorld: the ground, the sky, whatever stands there), with the
        // Option window's movement (--movement classic|pathfinding sets it for the run).
        const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
        proto::WorldPosition target;
        bool sky = false;
        if (world_->Pick(clickX_ * width, clickY_ * height, width, height, target, true, &sky)) {
            core::LogInfo("game: --click: to {},{} ({:.0f}, {:.0f}, {:.0f}){}", target.region & 0xFF, target.region >> 8, target.x, target.y, target.z,
                          sky ? " (the sky: that way)" : "");
        }
        ClickWorld(context, clickX_ * width, clickY_ * height, false);
        clickX_ = -1.0f;
    }
    if (clickAround_ > 0.0f) {
        // --click-around S: a click every S seconds somewhere about the character, then whether the
        // server's walk toward it came (its leg changed) within a moment.
        if (aroundCheck_ > 0.0 && context.time >= aroundCheck_ && heldMove_) {
            aroundCheck_ = context.time + 0.6; // held for the swing's end: checked once it went
        } else if (aroundCheck_ > 0.0 && context.time >= aroundCheck_) {
            aroundCheck_ = 0.0;
            const bool taken = std::hypot(self->legTo.x - aroundLegTo_.x, self->legTo.z - aroundLegTo_.z) > 0.01f || self->legTo.region != aroundLegTo_.region;
            if (!taken) {
                ++aroundMissed_;
                core::LogWarning("game: --click-around: click {} at {:.0f},{:.0f} on {} was not answered with a walk", aroundClicks_, aroundAt_.x,
                                 aroundAt_.y, lastClick_);
            }
        }
        if ((aroundWait_ -= float(context.dt)) <= 0.0f) {
            aroundWait_ = clickAround_;
            const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            const float angle = unit(aroundRandom_) * 6.2831853f, radius = (0.08f + 0.3f * unit(aroundRandom_)) * height;
            aroundAt_ = {0.5f * width + std::cos(angle) * radius, 0.55f * height + std::sin(angle) * radius * 0.6f};
            aroundLegTo_ = self->legTo;
            aroundCheck_ = context.time + 0.6;
            ++aroundClicks_;
            ClickWorld(context, aroundAt_.x, aroundAt_.y, false);
        }
    }
    if (!walk_ || self->moving || (walkWait_ -= float(context.dt)) > 0.0f) return;
    if (walkHome_.region == 0) walkHome_ = self->position;
    // The next end of the walk, on the ground there when the navigation knows it.
    const proto::FlatPosition home = proto::ToFlat(walkHome_);
    proto::WorldPosition target = walkOut_ ? proto::FromFlat({home.x + walkX_, home.z + walkZ_}, walkHome_.y) : walkHome_;
    world_->GroundHeight(target, target.y);
    shared_->session->MoveTo(target);
    walkOut_ = !walkOut_;
    walkWait_ = 1.0f; // the server's answer starts the leg; until then, no new order
}

void PlayScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    if (aroundClicks_ > 0) core::LogInfo("game: --click-around: {} clicks, {} not answered with a walk", aroundClicks_, aroundMissed_);
    if (context.audio != nullptr) context.audio->Silence(); // the place's music and ambience end with the world
    if (shared_) context.jobs.Wait(shared_->charactersJob);
    if (!recordPath_.empty()) {
        if (shared_ && shared_->session) shared_->session->Record(nullptr);
        std::string error;
        if (recording_.Save(recordPath_, error)) {
            core::LogInfo("game: recorded {} messages ({:.1f} s) to {}", recording_.messages.size(), recording_.Duration(), recordPath_);
        } else {
            core::LogWarning("game: {}", error);
        }
    }
    loading_.End(context.assets);
    portraits_.Exit(context.assets, snapshot, gui_.textures());
    gui_.Exit(snapshot);
    if (world_) world_->Exit(context, snapshot);
    world_.reset();
    shared_.reset();
}

void PlayScreen::HandleEvents(ScreenContext& context) {
    for (game::TitleEvent& event : shared_->session->TakeEvents()) {
        HandleEvent(context, event);
        if (!next.empty()) return;
    }
}

void PlayScreen::StepReplay(ScreenContext& context, int steps) {
    // What arrived before each step goes in before it (as game::ReplayHash plays it).
    for (int i = 0; i < steps; ++i) {
        for (game::TitleEvent& event : replay_->TakeUntil(replayTime_)) HandleEvent(context, event);
        view_.Step(clock_.StepSeconds());
        replayTime_ += clock_.StepSeconds();
    }
    if (replay_->Finished() && !replayDone_) {
        replayDone_ = true;
        core::LogInfo("game: the replay is over (hash {:016x})", view_.Hash());
    }
}

void PlayScreen::Bench(ScreenContext& context, FrameSnapshot& snapshot) {
    // The step the replay is in: logged as it starts, a picture half a second into its swing.
    if (benchOver_) return;
    if (!benchStarted_) {
        benchStarted_ = true;
        core::LogInfo("bench: the world is in; casting");
    }
    while (benchStep_ < bench_.steps.size() && replayTime_ >= bench_.steps[benchStep_].at) {
        const game::SkillBenchStep& step = bench_.steps[benchStep_];
        const assets::ItemRow* weapon = step.weapon != 0 ? tables_->items.Find(step.weapon) : nullptr;
        core::LogInfo("bench: {}/{} {} by {} ({})", benchStep_ + 1, bench_.steps.size(), step.codename,
                      step.caster == game::SkillBench::kEuropean ? "the European" : "the Chinese", weapon != nullptr ? weapon->codename : "as held");
        ++benchStep_;
    }
    // A video of each step (--bench-video): its frames up to the next step's start, the first's from the bench's start.
    if (!benchVideo_.empty() && !bench_.steps.empty()) {
        while (benchClip_ < bench_.steps.size() && replayTime_ >= bench_.steps[benchClip_].until) ++benchClip_;
        if (benchClip_ < bench_.steps.size()) {
            // Named by its group's code, the developer pages' (game::SkillGroupCode: the chains' "_1S" rows too).
            const game::SkillBenchStep& step = bench_.steps[benchClip_];
            const assets::SkillRow* row = tables_ ? tables_->skills.Find(step.skill) : nullptr;
            snapshot.record = std::filesystem::path(benchVideo_) / ((row != nullptr ? game::SkillGroupCode(*row) : step.codename) + ".mp4");
        }
    }
    if (!benchCaptures_.empty() && benchShot_ < benchStep_) {
        const game::SkillBenchStep& step = bench_.steps[benchShot_];
        if (benchBurst_ > 0 && replayTime_ >= step.at + benchFrame_ * benchBurstStep_) {
            snapshot.capture = std::filesystem::path(benchCaptures_) / std::format("{:03}_{}_{:02}.png", benchShot_ + 1, step.codename, benchFrame_);
            snapshot.captureGoesOn = true;
            if (++benchFrame_ >= benchBurst_) {
                benchFrame_ = 0;
                ++benchShot_;
            }
        } else if (benchBurst_ == 0 && replayTime_ >= step.capture) {
            snapshot.capture = std::filesystem::path(benchCaptures_) / std::format("{:03}_{}.png", benchShot_ + 1, step.codename);
            snapshot.captureGoesOn = true;
            ++benchShot_;
        }
    }
    if (benchStep_ >= bench_.steps.size() && replayTime_ >= bench_.steps.back().until) {
        benchOver_ = true;
        const WorldScreen::ClipCounts& clips = world_->Clips();
        const std::size_t failed = context.assets.GetStats().failed - std::min(benchFailedBefore_, context.assets.GetStats().failed);
        core::LogInfo("bench: done: {} skills; clips {} in their set (the skill's group), {} from another, {} missing; {} files failed", bench_.steps.size(),
                      clips.inStance, clips.fromDefault, clips.missing, failed);
        // The stages' sounds it heard (K8): each file found and decoded (on jobs; the bench is ending).
        const std::vector<std::string> sounds(benchSounds_.begin(), benchSounds_.end());
        std::atomic<int> unplayable{0};
        context.jobs.ParallelFor(sounds.size(), 8, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const auto file = context.files.Read(sounds[i]);
                audio::Pcm pcm;
                std::string error;
                if (!file || !audio::DecodeSound(file->Bytes(), pcm, error) || pcm.samples.empty()) {
                    unplayable.fetch_add(1);
                    core::LogWarning("bench: sound {}: {}", sounds[i], file ? error : std::string("not in the game files"));
                }
            }
        });
        core::LogInfo("bench: sounds: {} of the stages' played, {} files, {} missing or not decoded", benchSoundCues_, sounds.size(), unplayable.load());
        core::LogInfo("bench: shots: {} started by their impact, {} by the fallback", shotsAtImpact_, shotsLate_);
        context.quit = true;
        context.exitCode = clips.missing > 0 || failed > 0 || unplayable.load() > 0 ? 3 : 0;
    }
}

void PlayScreen::ReleaseHits(ScreenContext& context) {
    for (std::size_t i = 0; i < heldHits_.size();) {
        if (heldHits_[i].at > context.time) {
            ++i;
            continue;
        }
        game::TitleEvent event = std::move(heldHits_[i].event);
        heldHits_.erase(heldHits_.begin() + std::ptrdiff_t(i));
        releasingHit_ = true;
        HandleEvent(context, event);
        releasingHit_ = false;
    }
    if (heldHits_.empty()) newestHitHp_.clear();
}

void PlayScreen::HandleEvent(ScreenContext& context, game::TitleEvent& event) {
    // Several attackers' hits wait for their own swings' moments, so they show out of the server's order:
    // none may bring its target above the health the server told last (a stale one after the killing
    // blow left the dead looking alive, their death window never opened).
    if (auto* hit = std::get_if<proto::EntityDamaged>(&event)) {
        if (!releasingHit_) {
            newestHitHp_[hit->target] = hit->hp;
        } else if (const auto newest = newestHitHp_.find(hit->target); newest != newestHitHp_.end()) {
            hit->hp = std::min(hit->hp, newest->second);
        }
    }
    // A hit waits for its swing's damage moment: its number, effect,
    // sound and flinch, and its target's fall when it kills, show together then; one that comes late
    // shows as it comes.
    if (const auto* hit = std::get_if<proto::EntityDamaged>(&event); hit != nullptr && (hit->flags & proto::kDamageCompanion) != 0) {
        // A companion's strike (the Bow's hawk): it flies there first; the hit shows as it attacks.
        if (!releasingHit_ && world_) {
            if (const double after = stageEffects_.Strike(*world_, SlotOf(hit->attacker), SlotOf(hit->target), context.time); after > 0.005) {
                heldHits_.push_back({context.time + after, hit->target, event});
                return;
            }
        }
    } else if (hit != nullptr && !releasingHit_ && (hit->flags & (proto::kDamageState | proto::kDamageReturn | proto::kDamageBurst)) == 0) {
        if (pendingShots_.contains(hit->attacker)) {
            ++shotsAtImpact_;
            StartShot(hit->attacker, context.time); // its impact (P7)
        }
        // (A hit whose moment came is not asked again: by then a next swing's moments may stand here,
        // and a bow's shots, each longer in the air than the time between them, would never show.)
        if (std::vector<std::uint32_t>& struck = swingTargets_[hit->attacker]; std::find(struck.begin(), struck.end(), hit->target) == struck.end()) {
            struck.push_back(hit->target);
        }
        if (const auto s = strikeTimes_.find(hit->attacker); s != strikeTimes_.end() && hit->strike < s->second.size()) {
            if (const double at = s->second[hit->strike]; at > context.time + 0.005) {
                heldHits_.push_back({at, hit->target, event});
                return;
            }
        } else if (const auto r = resultsAt_.find(hit->attacker); s == strikeTimes_.end() && r != resultsAt_.end() && r->second > context.time + 0.005) {
            heldHits_.push_back({r->second, hit->target, event}); // a presentation of no damage group: at its end (P22)
            return;
        }
    }
    // A knock or the states a hit gives come with it: they wait for the latest hit on their target
    // still held (the knock-down and the stun show as the blow lands).
    if (!releasingHit_) {
        std::uint32_t on = 0;
        if (const auto* knock = std::get_if<proto::EntityKnocked>(&event)) on = knock->entity;
        if (const auto* states = std::get_if<proto::EntityStates>(&event)) on = states->entity;
        if (on != 0) {
            double latest = -1.0;
            for (const HeldHit& h : heldHits_) {
                if (h.target == on && std::holds_alternative<proto::EntityDamaged>(h.event)) latest = std::max(latest, h.at);
            }
            if (latest > context.time + 0.005) {
                heldHits_.push_back({latest, on, event});
                return;
            }
        }
    }
    Sounds(context, event);
    if (const auto* change = std::get_if<proto::EquipmentChanged>(&event)) {
        const game::WorldView::Entity* e = view_.Find(change->entity);
        core::LogInfo("game: {} wears {} in place {} now (was {})", e ? e->look.name : std::string("?"), change->item, change->slot, change->previous);
    }
    if (const auto* entered = std::get_if<proto::WorldEntered>(&event)) {
        if (entered->status != proto::WorldStatus::Ok) {
            core::LogWarning("game: the world refused the character ({})", int(entered->status));
            if (replay_) return;
            context.handoff[TitleShared::kHandoff] = shared_;
            next = "characters";
            return;
        }
        arrival_ = *entered; // the world is built (again) for it before the next use
        arrived_ = true;
        enteredFrom_ = view_.Self(); // the view takes the new entity after this
        // The player's own view (Online options), asked for afresh with every entry.
        if (!replay_ && shared_ && shared_->session) {
            const core::OnlineOptions online = context.options != nullptr ? context.options->online : core::OnlineOptions{};
            shared_->session->SetViewLimits(std::uint16_t(online.players), std::uint16_t(online.monsters), std::uint16_t(online.radius));
            appliedView_ = {online.players, online.monsters, online.radius};
        }
    } else if (const auto* view = std::get_if<proto::ViewLimitsAnswer>(&event)) {
        viewAnswer_ = *view;
        haveViewAnswer_ = true;
        core::LogInfo("game: the server shows this player at most {} players and {} monsters (its most {} / {}, {} map units)", view->players,
                      view->monsters, view->maxPlayers, view->maxMonsters, view->radius);
    } else if (const auto* logout = std::get_if<proto::LogoutAnswer>(&event)) {
        Logout(*logout, context.time);
    } else if (const auto* refused = std::get_if<proto::GateRefused>(&event)) {
        core::LogWarning("game: the gate refused ({})", int(refused->status));
        waitingForGate_ = false;
        // The original's interaction errors (ours for the refusals it has no text for).
        const char* key = refused->status == proto::GateStatus::ServerDown    ? "UIO_SMERR_CANT_FIND_GAMESERVER"
                          : refused->status == proto::GateStatus::NotEnoughGold ? "UIIT_MSG_INTERACTION_FAIL_NOT_ENOUGH_MONEY"
                          : refused->status == proto::GateStatus::TooFar      ? "UIIT_MSG_INTERACTION_FAIL_TOO_FAR"
                          // A link's restrictions (the 1.188 client's texts for codes 0x1C15, 0x1C10, 0x1C23).
                          : refused->status == proto::GateStatus::WrongLevel ? "UIIT_MSG_INTERACTION_FAIL_OUT_OF_REQUIRED_LEVEL_FOR_TELEPORT"
                          : refused->status == proto::GateStatus::Transport  ? "UIIT_MSG_INTERACTION_FAIL_CANT_USE_TELEPORT_WITH_TRADECART"
                          : refused->status == proto::GateStatus::Condition  ? "UIIT_MSG_QUEST_ROC_CANNOT_ENTRANCE"
                          : refused->status == proto::GateStatus::TransportFar ? "UIIT_MSG_INTERACTION_FAIL_TOO_FAR_FROM_TRADECART"
                          // A murderer at a gate structure (0x1C16, banner + line; 0x1C19 has no text).
                          : refused->status == proto::GateStatus::Murderer ? "UIIT_MSG_INTERACTION_FAIL_MURDERER_CAN_USE_PORTAL_STRUCTURE"
                          // The Pharaoh's tomb's and the instances' (0x1C27, 0x1C28, 0x1C2A, 0x1C25, 0x1C29,
                          // 0x1C31, 0x1C30, 0x1C2B by the 1.188 error table).
                          : refused->status == proto::GateStatus::TombEntries ? "UIIT_MSG_EGYPT_TOMB_ENTRANCE_CONDITION_ERROR_01"
                          : refused->status == proto::GateStatus::TombMaster  ? "UIIT_MSG_EGYPT_TOMB_ENTRANCE_CONDITION_ERROR_02"
                          : refused->status == proto::GateStatus::TombParty   ? "UIIT_MSG_EGYPT_TOMB_ENTRANCE_CONDITION_ERROR_05"
                          : refused->status == proto::GateStatus::InstanceFull ? "UIIT_MSG_INTERACTION_FAIL_OUT_OF_REQUIRED_NUMBER_FOR_TELEPORT"
                          : refused->status == proto::GateStatus::InstanceNone ? "UIIT_MSG_INSTANCEDUNGEON_ERROR_MSG_01"
                          : refused->status == proto::GateStatus::InInstance  ? "UIIT_MSG_INSTANCEDUNGEON_ERROR_MSG_02"
                          : refused->status == proto::GateStatus::Riding      ? "UIIT_MSG_INTERACTION_FAIL_CANT_USE_TELEPORT_WITH_RIDE"
                          : refused->status == proto::GateStatus::JobSuit     ? "UIIT_MSG_FLAGWAR_GAME_JOB_NOT_ENTER"
                          // The job temples' (0x1C2C, 0x1C2F, 0x1C2E, 0x1C2D).
                          : refused->status == proto::GateStatus::ShrineLevel   ? "UIIT_MSG_EGYPT_SHRINE_ENTRANCE_CONDITION_ERROR_01"
                          : refused->status == proto::GateStatus::ShrineNoRight ? "UIIT_MSG_EGYPT_SHRINE_ENTRANCE_CONDITION_ERROR_02"
                          : refused->status == proto::GateStatus::ShrineJobSuit ? "UIIT_MSG_EGYPT_SHRINE_ENTRANCE_CONDITION_ERROR_03"
                          : refused->status == proto::GateStatus::ShrineRatio   ? "UIIT_MSG_EGYPT_SHRINE_ENTRANCE_CONDITION_ERROR_04"
                          // Into a fortress's world (0x1C1A..0x1C1E).
                          : refused->status == proto::GateStatus::FortressJobSuit        ? "UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_01"
                          : refused->status == proto::GateStatus::FortressFull           ? "UIIT_MSG_FORT_PORTALSTONE_FAIL_04"
                          : refused->status == proto::GateStatus::FortressNotParticipant ? "UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_02"
                          : refused->status == proto::GateStatus::FortressNotMember      ? "UIIT_MSG_FORT_NOT_POSSESS_FORTRESS_MEMBER"
                          : refused->status == proto::GateStatus::FortressClosed         ? "UIIT_MSG_FORT_ETC_ERR_CANT_ENTER_5MINBEFORE"
                          : refused->status == proto::GateStatus::Returning ? "UIIT_MSG_INTERACTION_FAIL_TELESCROLL_ALREADY_WORKING"
                          : refused->status == proto::GateStatus::ThiefDen  ? "UIIT_INTERACTION_FAIL_ONLY_ROBBER_CAN_TELEPORT_TO_THIEFDEN"
                          : refused->status == proto::GateStatus::Busy      ? "UIIT_MSG_INTERACTION_FAIL_IM_BUSY"
                                                                           : "UIIT_MSG_STATE_TELEPORT_FAIL";
        if (!replay_ && refused->status == proto::GateStatus::Murderer) player_.Notify(game::Banner::Kind::Warning, gui_.data().Text(key), context.time);
        if (!replay_) player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), context.time);
    } else if (const auto* menu = std::get_if<proto::GateMenu>(&event)) {
        // The NPC window on the gate's menu (C7i), once the HUD is made.
        if (!replay_) {
            gateMenu_ = *menu;
            npcTax_ = menu->tax; // its fortress tax: the fares shown with it
        }
    } else if (const auto* shop = std::get_if<proto::ShopAnswer>(&event); shop != nullptr && shop->opened) {
        if (!replay_) shopShow_ = shop->entity; // the store of the picked tab group, beside the inventory
    } else if (const auto* repaired = std::get_if<proto::NpcRepaired>(&event)) {
        // An NPC's repair: the refusals' texts by the original's table (ErrorTexts.h: 0x1C07 not enough money, 0x1C11
        // cannot be repaired, 0x1C12 cannot be revived); done prints nothing (the pieces' new durability is in the bag).
        core::LogInfo("game: repair: {} ({} pieces, {} gold, code 0x{:04x})", repaired->ok ? "done" : "refused", repaired->changed.size(), repaired->gold, repaired->code);
        if (!replay_ && !repaired->ok) {
            const char* key = OriginalErrorKey(repaired->code);
            if (key == nullptr) key = "UIIT_MSG_INTERACTION_FAIL_I_DONT_HAVE_ANYTHING_FOR_YOU";
            player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), context.time);
        }
    } else if (const auto* list = std::get_if<proto::StorageList>(&event)) {
        // The window and the inventory beside it, once the HUD is made (the guild's: its own window).
        if (!replay_) (list->guild != 0 ? guildStorageShow_ : storageShow_) = true;
    } else if (const auto* refused = std::get_if<proto::StorageRefused>(&event)) {
        // The guild's storage refused: in use by another, the guild's level, no
        // authority, no guild (their texts).
        if (!replay_) {
            const UiData& d = gui_.data();
            std::string line;
            if (refused->code == 1) {
                line = FormatText(d.Text("UIIT_MSG_GUILD_WAREHOUSE_USE"), {refused->name});
            } else if (refused->code == 0x4C4A) {
                line = d.Text("UIIT_MSG_GUILD_WAREHOUSE_LIMIT");
            } else {
                line = GuildErrorText(hudContext_, refused->code, 0);
            }
            if (!line.empty()) player_.Post(game::GameMessage::Kind::General, line, context.time);
        }
    } else if (const auto* menu = std::get_if<proto::NpcMenu>(&event)) {
        flag_.ClearDesk(); // So-Ok's desk comes after her menu (EventDesk)
        arena_.ClearDesk(); // and the arena manager's (ArenaDesk)
        if (!replay_) {
            npcMenu_ = *menu;
            npcTax_ = menu->tax; // its fortress tax: the shop's prices shown with it
            npcSafe_ = menu->safe;
            if (menu->safe.option != 0xFF) safeOption_ = menu->safe.option; // kept with limit and count
            hudContext_.npcServices = menu->services; // the store's Repair rows (HudServices.cpp)
            hudContext_.npcServicesOf = menu->entity;
        }
    } else if (std::holds_alternative<proto::EntitySwing>(event) || std::holds_alternative<proto::EntityDamaged>(event) ||
               std::holds_alternative<proto::EntityBlink>(event)) {
        if (const auto* hit = std::get_if<proto::EntityDamaged>(&event)) CosHit(*hit); // our own pet's health (PlayCos.cpp)
        Fight(context, event);
    } else if (const auto* buffs = std::get_if<proto::EntityBuffs>(&event)) {
        TakeBuffs(*buffs, context.time);
    } else if (const auto* struck = std::get_if<proto::SkillObjectStruck>(&event)) {
        ObjectStruck(*struck, context.time);
    } else if (const auto* revived = std::get_if<proto::SkillTook>(&event)) {
        // A skill taking hold (a heal, a cure, a resurrection's rise): its ACT_S (effect, sound) on whom it took.
        const assets::SkillRow* skill = tables_ && tables_->Usable() ? tables_->skills.Find(revived->skill) : nullptr;
        const auto* stages = skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr;
        const std::size_t slot = SlotOf(revived->entity);
        if (world_ && stages != nullptr && slot != StageEffects::kNone) stageEffects_.BuffStart(*world_, *stages, slot);
    } else if (const auto* states = std::get_if<proto::EntityStates>(&event)) {
        TakeStates(*states, context.time);
    } else if (const auto* knock = std::get_if<proto::EntityKnocked>(&event)) {
        knocked_[knock->entity] = {knock->kind, context.time, context.time + double(knock->ms) / 1000.0, ++takes_};
        // A knock-down ends what it played; a knock-back is a slide alone, its clip going on, with a player's flinch
        // (kind 5).
        if (knock->kind == proto::KnockKind::Down) {
            EndPresentation(knock->entity, context.time);
            motions_.erase(knock->entity);
        } else if (const game::WorldView::Entity* e = view_.Find(knock->entity); e != nullptr && IsPlayer(*e)) {
            flinches_[knock->entity] = ++takes_;
        }
    } else if (const auto* cancelled = std::get_if<proto::EntityCastCancelled>(&event)) {
        // A cast its server gave up (B071 mode 2): its clips end (ours: the
        // motion; its later stages' cancel is P3's).
        EndPresentation(cancelled->entity, context.time);
        motions_.erase(cancelled->entity);
    } else if (const auto* action = std::get_if<proto::EntityAction>(&event)) {
        if (action->action == proto::EntityActionKind::PickUp) motions_[action->entity] = {{{kPick, context.time + kPickSeconds, ++takes_}}};
    } else if (const auto* pvp = std::get_if<proto::EntityFreePvp>(&event)) {
        HandleFreePvp(context, *pvp);
    } else if (const auto* refused = std::get_if<proto::FreePvpAnswer>(&event)) {
        // The original's texts by code; a change given up says so.
        const char* key = nullptr;
        switch (refused->code) {
        case proto::FreePvpCode::CannotEnter: key = "UIIT_MGS_ERR_FRPVP_ON"; break;
        case proto::FreePvpCode::JobState: key = "UIIT_MGS_ERR_FRPVP_JOB_STATE"; break;
        case proto::FreePvpCode::CannotLeave: key = "UIIT_MGS_ERR_FRPVP_OFF"; break;
        case proto::FreePvpCode::CannotChange: key = "UIIT_MGS_ERR_FRPVP_CHANGE_TEAM"; break;
        case proto::FreePvpCode::Cancelled: // leaving, joining or changing teams given up
            key = pvpChangeTo_ == 0 ? "UIIT_MGS_ERR_FRPVP_OFF_CANCEL" : pvpChangeFrom_ == 0 ? "UIIT_MGS_ERR_FRPVP_ON_CANCEL" : "UIIT_MGS_ERR_FRPVP_CANCEL_CHANGE_TEAM";
            break;
        default: break;
        }
        if (key != nullptr) player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), context.time);
    } else if (const auto* emote = std::get_if<proto::EntityEmote>(&event)) {
        // A gesture (the 1.188 client's 0x3091: a player's emotion state): the default set's
        // animation type 50 + the gesture, once, as long as its clip (a walk cuts it short).
        if (emote->gesture < 8) {
            Motion m;
            m.steps = {{proto::kEmoteFirstState + emote->gesture, context.time + 5.0, ++takes_, "default"}};
            m.shotFrom = context.time; // its end once the clip's length is known
            motions_[emote->entity] = std::move(m);
        }
    } else if (const auto* cool = std::get_if<proto::Cooldown>(&event)) {
        // A wait began: the skill's group (and its cool-down group), or the item.
        const game::PlayerState::Cooling c{context.time, context.time + double(cool->ms) / 1000.0};
        if (cool->kind == proto::CooldownKind::Item) {
            player_.itemCooling[cool->id] = c;
            if (const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(cool->id) : nullptr) {
                if (const game::ReuseLock lock = game::ReuseLockOf(*row, player_.self.race == 1); lock.bit != 0) {
                    player_.itemCooling[game::PlayerState::KindCoolingKey(lock.bit)] = c;
                }
                if (row->cooldownGroup != 0) player_.itemCooling[game::PlayerState::GroupCoolingKey(std::uint32_t(row->cooldownGroup))] = c;
            }
        } else if (const assets::SkillRow* s = tables_ && tables_->Usable() ? tables_->skills.Find(cool->id) : nullptr) {
            player_.skillCooling[s->group] = c;
            if (s->cooldownGroup != 0) player_.groupCooling[std::uint32_t(s->cooldownGroup)] = c;
        }
    } else if (const auto* used = std::get_if<proto::SkillUseAnswer>(&event)) {
        // A refusal as a system line (the original's table of UIIT_SKILL_USE_FAIL_* texts).
        const char* key = nullptr;
        switch (used->status) {
        case proto::SkillUseStatus::Mana: key = "UIIT_SKILL_USE_FAIL_NOTENOUGHMP"; break;
        case proto::SkillUseStatus::Cooling: key = "UIIT_SKILL_USE_FAIL_TIMEDELAY_COOL_TIME"; break;
        case proto::SkillUseStatus::Weapon: key = "UIIT_SKILL_USE_FAIL_WRONGWEAPON"; break;
        case proto::SkillUseStatus::Obstacle: key = "UIIT_SKILL_USE_FAIL_PATH_INTERRUPTED"; break; // "Cannot attack due to an obstacle." (0x3010)
        case proto::SkillUseStatus::FortressPosition: key = "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"; break; // 0x3047
        case proto::SkillUseStatus::Music: key = "UIIT_STT_SKILL_USE_FAIL_MUSICAL_RANGE"; break; // 0x3032
        case proto::SkillUseStatus::Target: key = "UIIT_SKILL_USE_FAIL_NONESELECTETARGET"; break;
        case proto::SkillUseStatus::Dead: key = "UIIT_SKILL_USE_FAIL_CHAR_DEAD"; break;
        case proto::SkillUseStatus::Untouchable: key = "UIIT_MSG_SKILL_USE_FAIL_CANT_ATTACK_TEMPORARILY"; break;
        case proto::SkillUseStatus::Overlap: key = "UIIT_SKILL_USE_FAIL_OVERLAP"; break;
        // The original's codes' words (its table, ErrorTexts.h): 0x3029 a link's count, 0x3028 a battle state, 0x3030 a
        // skill in a monster's shape, 0x3031 a stealth or a transform in berserk.
        case proto::SkillUseStatus::Links: key = OriginalErrorKey(0x3029); break;
        case proto::SkillUseStatus::InBattle: key = OriginalErrorKey(0x3028); break;
        case proto::SkillUseStatus::Transformed: key = OriginalErrorKey(0x3030); break;
        case proto::SkillUseStatus::Berserk: key = OriginalErrorKey(0x3031); break;
        case proto::SkillUseStatus::ReviveLevel: key = "UIIT_MSG_SKILL_USE_FAIL_RESURRECT"; break;
        // The jobs' 0x3018, 0x3022, 0x3019, 0x3021, 0x3006.
        case proto::SkillUseStatus::NotBattlefield: key = "UIIT_MSG_SKILL_USE_FAIL_ITS_NOT_A_BATTLE_FIELD"; break;
        case proto::SkillUseStatus::PartyMember: key = "UIIT_MSG_SKILL_USE_FAIL_CANT_ATTACK_PARTY_MEMBER"; break;
        case proto::SkillUseStatus::JobLock: key = "UIIT_MSG_SKILL_USE_FAIL_WAITFOR_JOB_ACTIVATE"; break;
        case proto::SkillUseStatus::Riding: key = "UIIT_MSG_SKILL_USE_FAIL_CANT_USESKILL_IN_RIDESTATE"; break;
        case proto::SkillUseStatus::JobMonster: key = "UIIT_SKILL_USE_FAIL_WRONGTARGET"; break;
        case proto::SkillUseStatus::SafeTrade:
            // 0x3024: banner + line.
            key = "UIIT_MSG_SKILL_USE_FAIL_CANT_ATTACK_CARAVAN_LEVEL1";
            player_.Notify(game::Banner::Kind::Warning, gui_.data().Text(key), context.time);
            break;
        case proto::SkillUseStatus::Ammunition: key = player_.self.race == 1 ? "UIIT_SKILL_USE_FAIL_RUNOUT_AMMO_VOLT" : "UIIT_SKILL_USE_FAIL_RUNOUT_AMMO"; break;
        // The PK rules: 0x3014..0x3017, 0x3025, 0x3039 as lines.
        case proto::SkillUseStatus::DailyPkLimit:
        case proto::SkillUseStatus::TotalPkLimit:
        case proto::SkillUseStatus::YourLevelTooLow:
        case proto::SkillUseStatus::TargetLevelTooLow:
        case proto::SkillUseStatus::PkProhibited:
        case proto::SkillUseStatus::Duplicate: PkRefusal(used->status, context.time); break;
        case proto::SkillUseStatus::NotYet: core::LogInfo("game: skill {}: only attack skills are cast so far", used->skill); break;
        default: break;
        }
        if (key != nullptr) player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), context.time);
    } else if (const auto* learned = std::get_if<proto::SkillBookAnswer>(&event)) {
        const char* key = nullptr;
        switch (game::SkillCheck(learned->status)) {
        case game::SkillCheck::NoSkillPoints: key = learned->mastery != 0 ? "UIIT_STT_SKILL_POINT_INSUFFICIENCY_MASTERY" : "UIIT_STT_SKILL_POINT_INSUFFICIENCY"; break;
        case game::SkillCheck::MasteryLevel: key = "UIIT_STT_SKILL_LEARN_MASTERY_LIMIT"; break;
        case game::SkillCheck::MasteryTotal: key = "UIIT_STT_SKILL_LEARN_MASTERY_TOTAL_LIMIT"; break;
        case game::SkillCheck::Strength: key = "UIIT_STT_SKILL_LEARN_STR_INSUFFICIENCY"; break;
        case game::SkillCheck::Intelligence: key = "UIIT_STT_SKILL_LEARN_INT_INSUFFICIENCY"; break;
        default: break;
        }
        if (key != nullptr) player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), context.time);
    } else if (const auto* book = std::get_if<proto::SkillBookList>(&event); book != nullptr && tables_ && tables_->Usable()) {
        // A higher level learned: the quick slots holding a lower level of its group take it.
        for (std::size_t i = 0; i < player_.quickSlots.slots.size(); ++i) {
            game::QuickSlot& q = player_.quickSlots.slots[i];
            if (q.kind != game::QuickSlot::Kind::Skill) continue;
            const assets::SkillRow* old = tables_->skills.Find(q.value);
            if (old == nullptr) continue;
            for (const std::uint32_t id : book->skills) {
                const assets::SkillRow* now = tables_->skills.Find(id);
                if (now != nullptr && now->group == old->group && now->id != old->id) {
                    q.value = id;
                    hud_.Request({HudRequest::Kind::QuickSlotChanged, int(i)});
                }
            }
        }
    } else if (const auto* gained = std::get_if<proto::ExperienceGained>(&event)) {
        // A kill's experience: system lines of the Acquisition kind, raw numbers, and the orbs
        // flying to the player.
        core::LogInfo("game: gained {} experience, {} skill experience", gained->experience, gained->skillExperience);
        KillOrbs(*gained, context.time);
        const UiData& d = gui_.data();
        if (gained->experience > 0) {
            player_.Post(game::GameMessage::Kind::Acquisition, FormatText(d.Text("UIIT_MSG_STATE_GAIN_EXP"), {std::to_string(gained->experience)}), context.time);
        }
        if (gained->skillExperience > 0) {
            player_.Post(game::GameMessage::Kind::Acquisition, FormatText(d.Text("UIIT_MSG_STATE_GET_SKILL_EXP"), {std::to_string(gained->skillExperience)}),
                         context.time);
        }
    } else if (const auto* points = std::get_if<proto::BerserkPoints>(&event); points != nullptr && points->from != 0) {
        // Berserk points from a kill: an orb a point from the monster to the player (the original's hwan_v), landing in its
        // violet absorption with hyanget.wav.
        if (world_ && points->points > player_.self.berserk) {
            const std::size_t from = SlotOf(points->from), self = SlotOf(view_.Self());
            if (from != StageEffects::kNone && self != StageEffects::kNone) {
                for (int i = 0; i < int(points->points - player_.self.berserk); ++i) {
                    stageEffects_.Homing(*world_, from, self, "battle\\hwan_v.efp", context.time, orbsSent_++, "battle\\hwn_violet_indraft.efp",
                                         "prim\\snd\\ui\\hyanget.wav");
                }
            }
        }
    } else if (const auto* item = std::get_if<proto::ItemGained>(&event)) {
        // A pick-up's line in the system messages, the Acquisition kind (the player's own or its grab pet's): "[%d] gold
        // gained.", "item [%s %d pieces] gained." for an expendable (itemdata TID2 3), else "item [%s] gained.".
        const UiData& d = gui_.data();
        std::string line;
        if (item->refId == 0) {
            line = FormatText(d.Text("UIIT_MSG_STATE_GAIN_GOLD"), {std::to_string(item->gold)});
        } else if (const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item->refId) : nullptr) {
            const std::string name = d.Text(row->nameKey);
            line = row->typeId[1] == 3 ? FormatText(d.Text("UIIT_MSG_STATE_GET_ITEM_EXPENDABLE"), {name, std::to_string(item->count)})
                                       : FormatText(d.Text("UIIT_MSG_STATE_GET_ITEM_NONEXPENDABLE"), {name});
        }
        if (!line.empty()) player_.Post(game::GameMessage::Kind::Acquisition, line, context.time);
    } else if (const auto* progress = std::get_if<proto::Progress>(&event)) {
        // A new level: the warning banner "Promoted to level [%d]." (not a system line); skill points
        // gained: a system line only (section 2; the server's show flag for the banner is off for
        // ordinary gains; the level-up effect and sound come later).
        const UiData& d = gui_.data();
        if (player_.self.entity != 0 && progress->level > player_.self.level) {
            LevelUpEffect(player_.self.entity, context.time);
            player_.Notify(game::Banner::Kind::Warning, FormatText(d.Text("UIIT_MSG_STRGERR_LEVEL"), {std::to_string(progress->level)}), context.time);
        }
        if (player_.self.entity != 0 && progress->skillPoints > player_.self.skillPoints) {
            const std::string line = FormatText(d.Text("UIIT_STT_SKILL_POINT_RECOVER_RESULT"), {std::to_string(progress->skillPoints - player_.self.skillPoints)});
            player_.Post(game::GameMessage::Kind::Acquisition, line, context.time);
        }
        // Skill points taken (a murderer's death) and experience lost (a death's penalty), told as the progress they
        // leave: the original's lines for each.
        if (player_.self.entity != 0 && progress->skillPoints < player_.self.skillPoints) {
            const std::string line = FormatText(d.Text("UIIT_MSG_JSERR_SINCE_YOU_DIE_IN_MURDERER_SP_DEPRIVED_BY_SERVER"),
                                                {std::to_string(player_.self.skillPoints - progress->skillPoints)});
            player_.Post(game::GameMessage::Kind::Acquisition, line, context.time);
        }
        if (player_.self.entity != 0 && progress->level == player_.self.level && progress->experience < player_.self.experience) {
            const std::string line = FormatText(d.Text("UIIT_MSG_STATE_LOSE_EXP"), {std::to_string(player_.self.experience - progress->experience)});
            player_.Post(game::GameMessage::Kind::Acquisition, line, context.time);
        }
    } else if (const auto* offered = std::get_if<proto::RevivalOffered>(&event)) {
        // A resurrection offered (K9): the original's question (ASK_SKL_RESURRECTION 0..2), the
        // answer to the server (No when closed).
        const UiData& d = gui_.data();
        core::LogInfo("game: {} offers a resurrection (skill {})", offered->caster, offered->skill);
        hud_.Confirm("UIIT_STT_CONFIRM_BOX",
                     {d.Text("UIIT_MSG_MSGBOX_ASK_SKL_RESURRECTION_0"), d.Text("UIIT_MSG_MSGBOX_ASK_SKL_RESURRECTION_1"),
                      d.Text("UIIT_MSG_MSGBOX_ASK_SKL_RESURRECTION_2")},
                     [this](bool yes) { shared_->session->AnswerRevival(yes); });
        if (acceptRevival_) hud_.AnswerConfirm(true);
    } else if (const auto* buff = std::get_if<proto::BuffAdded>(&event)) {
        // After the state's own handling (an entry clears the list first): see below.
        pendingBuffs_.push_back(*buff);
        if (tables_ && tables_->Usable()) BuffEffects(buff->token, buff->skill, true);
    } else if (const auto* held = std::get_if<proto::ActionHeld>(&event)) {
        actionsHeld_ = held->count;
    } else if (const auto* punished = std::get_if<proto::AccountRestricted>(&event)) {
        // A GM's punishment stopped a request: when it ends, as the original's line words it (year, month, day, AM / PM
        // (ours: the words), hour, minute; local time).
        if (!replay_) {
            const UiData& d = gui_.data();
            const char* key = punished->kind == 0 ? "UIIT_MSG_GM_PUNISHMENT_CHAT_BLOCK" : "UIIT_MSG_GM_PUNISHMENT_TRADE_BLOCK";
            std::string line = d.Text(key);
            if (punished->until > 0) {
                const std::time_t end = std::time_t(punished->until);
                std::tm at{};
                localtime_s(&at, &end);
                const int hour = at.tm_hour % 12 == 0 ? 12 : at.tm_hour % 12;
                line = FormatText(line, {std::to_string(at.tm_year + 1900), std::to_string(at.tm_mon + 1), std::to_string(at.tm_mday),
                                         at.tm_hour < 12 ? "AM" : "PM", std::to_string(hour), std::to_string(at.tm_min)});
            }
            player_.Post(game::GameMessage::Kind::General, line, context.time);
        }
    } else if (const auto* clock = std::get_if<proto::WorldClock>(&event)) {
        // The server's game clock (the original's 0x3027): the client's restarts from it and runs on.
        clockHours_ = double(std::min<int>(clock->hour, 23)) + double(std::min<int>(clock->minute, 59)) / 60.0;
        clockDay_ = clock->day;
        if (world_) world_->SetClock(clockHours_, clockDay_);
        core::LogInfo("game: the world's clock day {} {:02}:{:02}", clock->day, clock->hour, clock->minute);
    } else if (const auto* weather = std::get_if<proto::WeatherNow>(&event)) {
        // The weather where it stands (its region's climate); the world eases into it over 5 s. Kept for the next
        // place's world too (a teleport makes a new one).
        const std::uint8_t kind = std::uint8_t(std::clamp<int>(weather->kind, 1, 3));
        if (kind != weather_.kind) core::LogInfo("game: weather {} (amount {})", kind == 1 ? "clear" : kind == 2 ? "rain" : "snow", weather->amount);
        weather_ = {kind, weather->amount};
        if (world_) world_->SetWeather(game::WeatherKind(weather_.kind), weather_.amount);
    } else if (const auto* gone = std::get_if<proto::BuffRemoved>(&event)) {
        // A buff's token: the states' entries (their token their kind) and the timed jobs' are not its (an imbue's token
        // 5 took the poison's icon off the board; the user's report 2026-11-13).
        std::erase_if(player_.effects, [&](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::Skill && e.token == gone->token; });
        std::erase_if(player_.marks, [&](const game::Mark& m) { return m.token == gone->token; });
        BuffEffects(gone->token, 0, false);
    } else if (const auto* rose = std::get_if<proto::EntityLevelUp>(&event)) {
        LevelUpEffect(rose->entity, context.time);
    } else if (const auto* unique = std::get_if<proto::UniqueNotice>(&event)) {
        // A unique's news (0x300C 0x0C05 / 0x0C06): five uniques have lines of their own, the others the
        // general one with their name; gone with a killer "[killer] has killed ...", without one "... has disappeared"
        // (Demon Shaitan has no line of that: the general one). A system line and the warning banner.
        const assets::ObjectRow* row = tables_ && tables_->Usable() ? tables_->characters.Find(unique->model) : nullptr;
        if (row != nullptr) {
            // By the monster's name key, as 1.188 compares.
            static constexpr std::pair<const char*, const char*> kOwn[] = {{"SN_MOB_CH_TIGERWOMAN", "TIGER_GIRL"}, {"SN_MOB_KK_ISYUTARU", "IYUTARU"},
                                                                           {"SN_MOB_OA_URUCHI", "URRUCHI"},         {"SN_MOB_TK_BONELORD", "BONELORD"},
                                                                           {"SN_MOB_RM_TAHOMET", "TAHOMET"}};
            std::string own;
            for (const auto& [key, line] : kOwn) {
                if (row->nameKey == key) own = line;
            }
            const bool killed = unique->kind == 2 && !unique->killer.empty();
            if (unique->kind == 2 && !killed && own == "TAHOMET") own.clear();
            const char* stem = unique->kind == 1 ? "UIIT_MSG_APPEAR_" : killed ? "UIIT_MSG_ANYONE_DEAD_" : "UIIT_MSG_DEAD_";
            std::string text = gui_.data().Text(std::string(stem) + (own.empty() ? "UNIC" : own));
            // Its %s in turn: the killer first, then the monster's name.
            std::vector<std::string> args;
            if (killed) args.push_back(unique->killer);
            if (own.empty()) args.push_back(gui_.data().Text(row->nameKey));
            for (const std::string& a : args) {
                if (const std::size_t at = text.find("%s"); at != std::string::npos) text.replace(at, 2, a);
            }
            player_.Post(game::GameMessage::Kind::General, text, context.time);
            player_.Notify(game::Banner::Kind::Warning, text, context.time);
            core::LogInfo("game: {}", text);
        }
    } else if (const auto* used = std::get_if<proto::ItemUsed>(&event)) {
        ItemUseEffect(used->entity, used->item);
        ++itemEffectsSeen_;
    } else if (const auto* mall = std::get_if<proto::MallEvent>(&event)) {
        // Magic POP's answers and the shard's announce (HudMagicPop.cpp).
        if (const auto* played = std::get_if<proto::PopPlayed>(mall); played != nullptr && !replay_) {
            MagicPopPlayed(hudContext_, *played);
        } else if (const auto* exchanged = std::get_if<proto::PopExchanged>(mall); exchanged != nullptr && !replay_) {
            MagicPopExchanged(hudContext_, *exchanged);
        } else if (const auto* announce = std::get_if<proto::PopAnnounce>(mall); announce != nullptr && !replay_) {
            MagicPopAnnounced(hudContext_, *announce);
        }
        HandleMallItems(context, *mall); // the mall's items and services (PlayMallItems.cpp)
        HandleMall(context, *mall);      // the item mall's window and silk (PlayMall.cpp)
        if (!replay_) ServiceEvent(hudContext_, *mall); // the services' answers, the cubes (HudServices.cpp)

    } else if (const auto* refused = std::get_if<proto::ItemRefused>(&event)) {
        // The original's code as its text (the 1.188 client's table, ErrorTexts.h); a code it has none for, the COS's
        // item line (ours).
        const char* key = OriginalErrorKey(refused->code);
        if (key == nullptr) key = "UIIT_MSG_COSPETERR_CANT_USEITEM";
        // A murderer's return scroll (0x1875): a banner too (category 5).
        if (refused->code == 0x1875) player_.Notify(game::Banner::Kind::Warning, gui_.data().Text(key), context.time);
        if (const char* mall = MallItemErrorKey(refused->code)) key = mall; // the mall's items' codes
        // A second experience scroll while one runs: the original's client refuses it itself with its own line.
        if (const assets::ItemRow* row = tables_ ? tables_->items.Find(refused->item) : nullptr;
            row != nullptr && refused->code == game::kBonusRunning && game::BonusScrollOf(*row).key == game::kBonusExperienceKey) {
            key = "UIIT_MSG_MOVE_SPEED_UP_FAIL_OVERLAP";
        }
        player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), context.time);
        core::LogInfo("game: item use refused (code 0x{:X})", refused->code);
    } else if (const auto* started = std::get_if<proto::ReturnStarted>(&event)) {
        player_.casting = {started->item, context.time, started->ms / 1000.0};
    } else if (std::holds_alternative<proto::ReturnCancelled>(event)) {
        player_.casting = {};
        if (!replay_) player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_TRANSITION_CANCEL_RESULT"), context.time);
    } else if (std::holds_alternative<proto::ReturnPointSet>(event)) {
        // Observed: posts it as a system line.
        if (!replay_) player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_STATE_REBIRTH_POINT_APPOINT"), context.time);
    } else if (std::holds_alternative<proto::WorldLeft>(event)) {
        if (replay_) return; // the last picture stays
        if (exitAfterLogout_) {
            core::LogInfo("game: the countdown is over; to Windows");
            context.quit = true;
            return;
        }
        context.handoff[TitleShared::kHandoff] = shared_;
        next = "characters";
    } else if (const auto* error = std::get_if<game::TitleError>(&event)) {
        core::LogWarning("game: {}", error->reason);
        // The game server's connection lost in the world: the original's box (its kind 1, "The connection to the server
        // has ended."); its button ends the game, as the original's does.
        if (error->where == game::TitleError::Where::Game && shown_ && gui_.interface() != nullptr && !netOff_ && !replay_) {
            netOff_ = true;
            hud_.Inform("UIIT_STT_CONFIRM_BOX", {gui_.data().Text("UIIT_MSG_MSGBOX_NETOFF")}, [this] { quitNow_ = true; });
            return;
        }
        next = "title";
        return;
    } else if (const auto* bye = std::get_if<proto::Disconnected>(&event)) {
        // The server is closing the connection and says why (a blocked account, a game master's kick): the title says it.
        const std::string why = TitleShared::FarewellOf(shared_->data, *bye);
        core::LogWarning("game: {}", why);
        context.handoff[TitleShared::kFarewell] = std::make_shared<std::string>(why);
        return;
    }
    // One going fades out where it was (one coming back stops fading).
    if (const auto* gone = std::get_if<proto::EntityGone>(&event)) {
        if (gone->entity != view_.Self()) entityJobs_.erase(gone->entity);
        jobChanges_.erase(gone->entity);
        flag_.Forget(gone->entity);
        const game::WorldView::Entity* e = view_.Find(gone->entity);
        if (e != nullptr && gone->entity != view_.Self() && e->kind != proto::EntityKind::GroundItem && slots_.contains(gone->entity)) {
            vanishing_[gone->entity] = {*e, context.time};
        }
    } else if (const auto* came = std::get_if<proto::EntityAppeared>(&event)) {
        vanishing_.erase(came->entity);
    } else if (const auto* entered = std::get_if<proto::WorldEntered>(&event)) {
        vanishing_.clear();
        flag_.Entered(entered->entity); // the others' teams told again; the timer hidden out of a match
    }
    if (const auto* flagEvent = std::get_if<proto::EventEvent>(&event)) HandleEvent(context, *flagEvent); // PlayFlagWorld.cpp
    if (const auto* fort = std::get_if<proto::FortressEvent>(&event)) {
        HandleFortress(context, *fort);
    } else if (std::holds_alternative<proto::WorldEntered>(event)) {
        fortress_.ClearEntities();
    } else if (const auto* gone = std::get_if<proto::EntityGone>(&event)) {
        fortress_.Forget(gone->entity);
    }
    if (const auto* community = std::get_if<proto::CommunityEvent>(&event)) HandleCommunity(context, *community);
    if (const auto* academy = std::get_if<proto::AcademyEvent>(&event)) HandleAcademy(context, *academy);
    if (const auto* market = std::get_if<proto::OpenMarketEvent>(&event)) HandleOpenMarket(context, *market);
    if (const auto* exchange = std::get_if<proto::ExchangeEvent>(&event)) HandleExchange(context, *exchange);
    if (const auto* alchemy = std::get_if<proto::AlchemyResult>(&event)) AlchemyResultArrived(hudContext_, *alchemy);
    if (const auto* stall = std::get_if<proto::StallEvent>(&event)) HandleStall(context, *stall);
    if (const auto* quest = std::get_if<proto::QuestEvent>(&event)) HandleQuest(context, *quest);
    if (const auto* instance = std::get_if<proto::InstanceEvent>(&event)) HandleInstance(context, *instance);
    if (const auto* cos = std::get_if<proto::CosEvent>(&event)) {
        HandleCos(context, *cos);
    } else if (std::holds_alternative<proto::WorldEntered>(event)) {
        riders_.clear(); // those in sight are told again (and ours come back with CosSummoned)
        ownCoses_.clear();
        SyncCosHud();
    } else if (const auto* gone = std::get_if<proto::EntityGone>(&event)) {
        CosForget(gone->entity);
    }
    if (const auto* guild = std::get_if<proto::GuildEvent>(&event)) {
        HandleGuild(context, *guild);
    } else if (const auto* entered = std::get_if<proto::WorldEntered>(&event)) {
        guild_.ClearEntities(); // those in sight are told again
        guild_.SetSelf(entered->character.name);
    } else if (const auto* gone = std::get_if<proto::EntityGone>(&event)) {
        guild_.Forget(gone->entity);
        if (const auto kept = berserkEffects_.find(gone->entity); kept != berserkEffects_.end()) {
            if (world_) {
                for (const gfx::EffectSystem::Handle h : kept->second) stageEffects_.Release(*world_, h);
            }
            berserkEffects_.erase(kept);
        }
    }
    // A character's line (chat kind 13, a text key): named here, as the original's client names it by its entity. A
    // player's (a coded quest's reply) is its own All line; a monster's or an NPC's stays a speech, with its name.
    if (auto* line = std::get_if<proto::ChatLine>(&event); line != nullptr && line->kind == proto::ChatKind::Speech) {
        line->text = gui_.data().Text(line->text);
        if (const game::WorldView::Entity* who = view_.Find(line->sender); who != nullptr && IsPlayer(*who)) {
            line->kind = proto::ChatKind::All;
            line->name = who->look.name;
            if (line->sender == view_.Self()) line->sender = 0; // shown, not taken for the player's own echo (its bubble by name)
        } else if (who != nullptr && tables_ && tables_->Usable()) {
            if (const assets::ObjectRow* row = tables_->characters.Find(who->look.model)) line->name = gui_.data().Text(row->nameKey);
        }
    }
    view_.Apply(event);
    player_.Apply(event);
    if (const auto* link = std::get_if<proto::LinkAdded>(&event)) {
        // A link the player made: on its own buffs board
        // unless its skill's 'lnks' fourth word is 0 (the DOTs; adds no entry then), its target's
        // name kept for the tooltip; a mark ('hntp') on the minimap where its target is.
        const assets::SkillRow* row = tables_ && tables_->Usable() ? tables_->skills.Find(link->skill) : nullptr;
        const game::WorldView::Entity* on = view_.Find(link->target);
        if (row != nullptr) {
            if (const assets::SkillParam lnks = assets::FindSkillParam(*row, assets::SkillTag("lnks")); !lnks || lnks.Int(3) != 0) {
                game::ActiveEffect e{game::ActiveEffect::Kind::Skill, link->skill, link->token, false, context.time, link->ms / 1000.0};
                if (on != nullptr) e.target = on->look.name;
                player_.effects.push_back(std::move(e));
            }
            if (assets::FindSkillParam(*row, assets::SkillTag("hntp"))) {
                player_.marks.push_back({link->token, link->target, on != nullptr ? on->position : proto::WorldPosition{}, on != nullptr ? on->yaw : 0.0f});
            }
        }
    } else if (const auto* motion = std::get_if<proto::EntityMotion>(&event)) {
        // Sitting, standing up, walking.
        if (motion->motion == proto::kMotionStand && motion->walking == 0) {
            entityMotions_.erase(motion->entity);
        } else {
            entityMotions_[motion->entity] = {motion->motion, motion->to, motion->walking != 0, context.time};
        }
    } else if (const auto* pk = std::get_if<proto::PkEvent>(&event)) {
        HandlePk(context, *pk); // murderers (PlayPk.cpp)
    } else if (const auto* jobs = std::get_if<proto::JobEvent>(&event)) {
        // Others' jobs: the alias over the head while the suit is on.
        if (const auto* seen = std::get_if<proto::EntityJob>(jobs)) {
            if (seen->state == game::kNoJob) {
                entityJobs_.erase(seen->entity);
            } else {
                entityJobs_[seen->entity] = *seen;
            }
        } else if (const auto* change = std::get_if<proto::EntityJobChange>(jobs)) {
            if (change->seconds == 0) {
                jobChanges_.erase(change->entity);
            } else {
                jobChanges_[change->entity] = {context.time, change->seconds};
            }
        } else if (const auto* trade = std::get_if<proto::TradeAnswer>(jobs)) {
            TradeAnswered(hudContext_, *trade); // HudTrade.cpp
        } else if (const auto* prices = std::get_if<proto::TradePrices>(jobs)) {
            hudContext_.tradePrices.clear();
            for (const proto::TradePrice& p : prices->prices) hudContext_.tradePrices[p.item] = p.price;
        } else if (const auto* scale = std::get_if<proto::TradeScale>(jobs)) {
            hudContext_.tradeStars = scale->stars;
        } else if (const auto* notice = std::get_if<proto::TradeBargainNotice>(jobs)) {
            TradeNotice(hudContext_, *notice);
        } else if (const auto* board = std::get_if<proto::TradeBoard>(jobs)) {
            hudContext_.tradeBoard = *board;
            ++hudContext_.tradeBoardVersion;
        } else if (const auto* answer = std::get_if<proto::JobAnswer>(jobs)) {
            JobAnswered(hudContext_, *answer); // the league's answers (HudJobs.cpp)
        } else if (const auto* lock = std::get_if<proto::TradeBargainLock>(jobs); lock != nullptr && !replay_) {
            // 0x320C {7, s}: kind 6 on for its seconds, off when they run out.
            bargainUntil_ = context.time + double(lock->seconds);
            std::erase_if(player_.effects, [](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::TimedJob && e.id == 0x42524749; });
            SyncSafeTradeIcon(context.time);
        } else if (const auto* safe = std::get_if<proto::SafeTradeState>(jobs); safe != nullptr && !replay_) {
            // 0x34D5: the icon on while active; begun (0x50) a banner when the option byte is 0; the limit
            // (0x41) a system line; 0x51 and 0x40 nothing said.
            safeTradeOn_ = safe->active != 0;
            SyncSafeTradeIcon(context.time);
            if (safe->active != 0 && safe->code == 0x50 && safeOption_ == 0) {
                player_.Notify(game::Banner::Kind::Warning,
                               FormatText(gui_.data().Text("UIIT_MSG_SAFETRADE_PROGRESS"), {std::to_string(safe->count), std::to_string(safe->limit)}),
                               context.time);
            }
            if (safe->active == 0 && safe->code == 0x41) {
                player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_SAFETRADE_NUMBER_ERR_LIMIT"), context.time);
            }
            core::LogInfo("game: safe trade {} (0x{:02X}, {}/{})", safe->active != 0 ? "on" : "off", safe->code, safe->count, safe->limit);
        } else if (const auto* ranking = std::get_if<proto::JobRanking>(jobs); ranking != nullptr && !replay_) {
            JobRankingArrived(hudContext_, *ranking); // HudJobs.cpp
        } else if (const auto* outcome = std::get_if<proto::JobOutcomeAnswer>(jobs); outcome != nullptr && !replay_) {
            JobOutcomeAnswered(hudContext_, *outcome);
        } else if (const auto* exp = std::get_if<proto::JobExpChanged>(jobs); exp != nullptr && !replay_) {
            // The job's experience (0x30E6): "Trader Experience Point [%d] gained." / "... lost.", the
            // grade on a level up, a trader's donation to the hunters' league.
            static constexpr const char* kWords[4] = {"", "MERCHANT", "THIEF", "HUNTER"};
            const char* word = kWords[std::min<std::uint8_t>(exp->type, 3)];
            const UiData& d = gui_.data();
            if (*word != 0 && exp->delta != 0) {
                const std::string key = std::format("UIIT_STT_JOB_EXP_{}_{}", word, exp->delta > 0 ? "GET" : "LOST");
                player_.Post(game::GameMessage::Kind::General, FormatText(d.Text(key), {std::to_string(exp->delta > 0 ? exp->delta : -exp->delta)}), context.time);
            }
            if (*word != 0 && exp->levelUp) {
                const std::string grade = JobClassName(hudContext_, exp->type, exp->level, player_.self.race);
                player_.Post(game::GameMessage::Kind::General, FormatText(d.Text(std::format("UIIT_STT_JOB_LVUP_{}_CLASS", word)), {grade}), context.time);
            }
            if (exp->donated > 0) {
                player_.Post(game::GameMessage::Kind::General, FormatText(d.Text("UIIT_MSG_SPECIALTY_CONTRIBUTION_HUNTERGUILD"), {GoldText(exp->donated)}), context.time);
            }
        } else if (const auto* previous = std::get_if<proto::JobPrevious>(jobs)) {
            ShowPreviousJobs(hudContext_, *previous);
        }
    } else if (const auto* marks = std::get_if<proto::EntityMarks>(&event)) {
        if (marks->marks == 0) {
            entityMarks_.erase(marks->entity);
        } else {
            entityMarks_[marks->entity] = marks->marks;
        }
    } else if (const auto* mode = std::get_if<proto::EntityBerserk>(&event)) {
        // The mode that ends is the one that was on (its stages' end).
        const auto was = berserkers_.find(mode->entity);
        const std::uint8_t variant = mode->on != 0 ? mode->variant : was != berserkers_.end() ? was->second : 0;
        if (mode->on != 0) {
            berserkers_[mode->entity] = mode->variant;
        } else if (was != berserkers_.end()) {
            berserkers_.erase(was);
        }
        looks_.erase(mode->entity); // its hair and horns on or off
        if (mode->entity == view_.Self()) berserkUntil_ = mode->on != 0 ? context.time + double(mode->ms) / 1000.0 : 0.0;
        // Told before it stands on the stage (just seen, or an arrival still to come: that one clears
        // the stage): its loops once it does.
        if (mode->on != 0 && (arrived_ || SlotOf(mode->entity) == StageEffects::kNone)) {
            berserkWaiting_.insert(mode->entity);
        } else {
            berserkWaiting_.erase(mode->entity);
            BerserkEffects(mode->entity, mode->on != 0, variant);
        }
    } else if (const auto* refused = std::get_if<proto::BerserkAnswer>(&event); refused && refused->status != proto::BerserkStatus::Ok) {
        core::LogInfo("game: berserk refused ({})", int(refused->status));
    } else if (const auto* where = std::get_if<proto::LinkPosition>(&event)) {
        for (game::Mark& m : player_.marks) {
            if (m.token != where->token) continue;
            m.position = where->position;
            m.yaw = where->yaw;
        }
    }
    for (const proto::BuffAdded& b : pendingBuffs_) {
        std::erase_if(player_.effects, [&](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::Skill && e.token == b.token; });
        player_.effects.push_back({game::ActiveEffect::Kind::Skill, b.skill, b.token, false, context.time, b.ms / 1000.0});
    }
    pendingBuffs_.clear();
}

void PlayScreen::ForgetEntities() {
    // Everything kept by entity id belongs to the old world: the view starts afresh, those in sight are told again
    // (with new look versions), and another game server's ids name other entities. Kept, a West monster's model
    // dressed the player who came East by a return scroll, a player's dress a monster (the user's report 2026-11-13).
    // Only the player's own entries stay, and only under the same id (a teleport on one server).
    const std::uint32_t keep = enteredFrom_ != 0 && enteredFrom_ == view_.Self() ? enteredFrom_ : 0;
    const auto prune = [keep](auto& kept) {
        std::erase_if(kept, [keep](const auto& item) {
            if constexpr (requires { item.first; }) {
                return item.first != keep;
            } else {
                return item != keep;
            }
        });
    };
    prune(blinks_);
    prune(motions_);
    prune(strikeTimes_);
    prune(swingTargets_);
    prune(newestHitHp_);
    prune(combatStance_);
    prune(killed_);
    prune(appeared_);
    prune(groundShown_);
    prune(staged_);
    prune(looks_);
    prune(lookVersions_);
    prune(structureStages_);
    prune(speech_);
    prune(gaps_);
    prune(gapsAsked_);
    prune(entityStalls_);
    prune(boothsShown_);
    prune(entityPvp_);
    prune(lastSkill_);
    prune(berserkWaiting_);
    prune(berserkers_);
    prune(leveledAt_);
    prune(buffed_);
    prune(objectEffects_);
    prune(afflicted_);
    prune(knocked_);
}

void PlayScreen::Arrive(ScreenContext& context, FrameSnapshot& snapshot) {
    arrived_ = false;
    player_.casting = {}; // a return scroll's gauge ends with the teleport
    const bool teleport = world_ != nullptr;
    if (teleport) holdForGate_ = waitingForGate_ = false;
    if (world_) {
        clockHours_ = world_->ClockHours(); // the clock runs on in the next place
        clockDay_ = world_->ClockDay();
        // Everything of the old place goes; the loading screen covers the new one coming in.
        view_.SetNavigation(nullptr);
        world_->Exit(context, snapshot);
        world_.reset();
    }
    slots_.clear();
    slotUsed_.assign(kSlots, false);
    showers_.clear(); // their slots and models went with the old world
    stageProps_.clear();
    stageEffects_.Clear(); // the effects went with the old world
    buffEffects_.clear();
    berserkEffects_.clear();
    itemGlows_.clear();
    markEffects_.clear(); // the effects went with the old world
    entityMotions_.clear();
    {
        // Its own marks stay, under its new entity after a move to another game server; the others' come again with them.
        const auto own = entityMarks_.find(enteredFrom_);
        const std::uint8_t mine = own != entityMarks_.end() ? own->second : 0;
        entityMarks_.clear();
        if (mine != 0) entityMarks_[view_.Self()] = mine;
        // The others' told again with their appearances; its own came with the entry (before this, in the same frame).
        const auto ownJob = entityJobs_.find(view_.Self());
        const std::optional<proto::EntityJob> myJob = ownJob != entityJobs_.end() ? std::optional<proto::EntityJob>(ownJob->second) : std::nullopt;
        entityJobs_.clear();
        if (myJob) entityJobs_[view_.Self()] = *myJob;
        jobChanges_.clear();
    }
    ForgetEntities();
    autoRun_ = false;
    shown_ = false;
    black_ = 1.0f;
    if (teleport) pendingGate_ = 0;
    turning_ = false;
    turnHeld_ = false;
    targetMenuArmed_ = false;
    // The world screen around the character's region, with stage slots for the walkers.
    const proto::WorldPosition& at = arrival_.position;
    originX_ = at.region & 0xFF;
    originY_ = at.region >> 8;
    world_ = std::make_unique<WorldScreen>(WorldScreen::Backdrop{originX_, originY_, kSlots + 1 + kMarkerSlots}); // + the zone markers
    world_->SetWeather(game::WeatherKind(weather_.kind), weather_.amount);
    stageEffects_.Clear();
    stageEffects_.SetCompanionSlots(kWalkerSlots, kCompanionSlots);
    stageEffects_.struckBy = [this](std::size_t casterSlot) {
        std::vector<std::size_t> out;
        for (const auto& [id, slot] : slots_) {
            if (slot != casterSlot) continue;
            if (const auto it = swingTargets_.find(id); it != swingTargets_.end()) {
                for (const std::uint32_t target : it->second) {
                    if (const std::size_t s = SlotOf(target); s != StageEffects::kNone) out.push_back(s);
                }
            }
            break;
        }
        return out;
    };
    world_->Enter(context);
    // The game clock: carried from the last place, else the client's own from 00:00 (--clock H for a run).
    if (clockHours_ < 0.0) clockHours_ = std::max(0.0, context.config.GetDouble("clock", 0.0));
    world_->SetClock(clockHours_, clockDay_);
    view_.SetNavigation(&world_->Navigation());
    std::mt19937 random(std::random_device{}());
    loading_.End(context.assets);
    loading_.Begin(context.assets,
                   game::LoadingPicture(teleport ? game::LoadingKind::Teleport : game::LoadingKind::FirstEntry, at.region, random()));
    cameraYaw_ = arrival_.yaw; // behind the character, looking where it faces
    core::LogInfo("game: {} {} {},{} ({:.0f}, {:.0f}, {:.0f})", arrival_.character.name, teleport ? "arrived at" : "entered the world at",
                  originX_, originY_, at.x, at.y, at.z);
}

bool PlayScreen::GateHere(const assets::TeleportGate& g) const {
    if (g.region == 0 || !tables_ || tables_->teleports.PointOf(g.refObjId) == nullptr) return false;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const std::uint16_t here = self != nullptr ? self->position.region : 0;
    if ((g.region & 0x8000) != 0) return g.region == here;
    return (here & 0x8000) == 0;
}

const assets::TeleportGate* PlayScreen::GateNear(const proto::WorldPosition& at, float reach) const {
    if (!tables_ || !tables_->Usable()) return nullptr;
    const proto::FlatPosition p = proto::ToFlat(at);
    const assets::TeleportGate* best = nullptr;
    double bestDistance = reach;
    for (const assets::TeleportGate& g : tables_->teleports.Gates()) {
        if (!GateHere(g)) continue;
        const proto::FlatPosition s = proto::ToFlat({g.region, g.x, g.y, g.z});
        const double d = std::hypot(s.x - p.x, s.z - p.z);
        if (d <= bestDistance) {
            best = &g;
            bestDistance = d;
        }
    }
    return best;
}

const assets::TeleportGate* PlayScreen::GateUnder(float x, float y, float width, float height, const proto::WorldPosition* ground) const {
    if (!tables_ || !tables_->Usable() || !world_) return nullptr;
    // The gate's box first (the nearest along the ray; the original's: its row's BCRadius across, its BCHeight up and down
    // about its spot; a row without them our crystal's cylinder), then the ground near a gate's spot.
    const assets::TeleportGate* best = nullptr;
    float bestDistance = 1e9f;
    for (const assets::TeleportGate& g : tables_->teleports.Gates()) {
        if (!GateHere(g)) continue;
        float distance = 0;
        const proto::WorldPosition at{g.region, g.x, g.y, g.z};
        const bool hit = g.radius > 0.0f && g.height > 0.0f ? world_->RayHitsBox(x, y, width, height, at, g.radius, g.height, distance)
                                                            : world_->RayHitsCylinder(x, y, width, height, at, kGateCrystalRadius, kGateCrystalHeight, distance);
        if (hit && distance < bestDistance) {
            best = &g;
            bestDistance = distance;
        }
    }
    if (best == nullptr && ground != nullptr) best = GateNear(*ground, kGateClick);
    return best;
}

// The Game page's name rows, as the original reads them: monsters' and COS' (the monster row), NPCs', the player's own,
// other players' (or, with the guild row on, those of the player's own guild). Gates keep theirs.
bool PlayScreen::NameShown(std::uint32_t id, const game::WorldView::Entity& e, ScreenContext& context) const {
    if (context.options == nullptr) return true;
    const core::GameOptions& rows = context.options->game;
    switch (e.kind) {
    case proto::EntityKind::Monster:
    case proto::EntityKind::Cos: return rows.Get("monsterNames");
    case proto::EntityKind::Character: {
        if (GapGate(e) != nullptr) return true;
        if (IsNpc(e)) return rows.Get("npcNames");
        if (id == view_.Self()) return rows.Get("ownName");
        if (rows.Get("playerNames")) return true;
        if (!rows.Get("guildNames") || !guild_.InGuild()) return false;
        const proto::EntityGuild* tag = guild_.EntityTag(id);
        return tag != nullptr && tag->guild == guild_.Guild().id;
    }
    default: return true;
    }
}

// The status gauges over heads, as the original chooses them by the Game page's rows: the player's own HP and MP (its own
// row), its own COS's HP (the COS row), a party member's HP and MP (the party row), the selected monster's HP (the
// monster row).
int PlayScreen::StatusGauge(std::uint32_t id, const game::WorldView::Entity& e, ScreenContext& context, float& hp, float& mp) const {
    if (context.options == nullptr) return 0;
    const core::GameOptions& rows = context.options->game;
    const auto share = [](std::uint32_t now, std::uint32_t most) { return most == 0 ? 0.0f : float(std::min(now, most)) / float(most); };
    if (id == view_.Self()) {
        if (!rows.Get("ownStatus")) return 0;
        hp = share(player_.self.hp, player_.self.maxHp);
        mp = share(player_.self.mp, player_.self.maxMp);
        return 2;
    }
    switch (e.kind) {
    case proto::EntityKind::Cos:
        if (!rows.Get("cosStatus")) return 0;
        for (const OwnCos& c : ownCoses_) {
            if (c.entity != id) continue;
            hp = share(c.hp, c.maxHp);
            return 1;
        }
        return 0;
    case proto::EntityKind::Character: {
        if (!rows.Get("partyStatus") || IsNpc(e) || !player_.party.InParty()) return 0;
        const proto::PartyMemberRecord* m = player_.party.MemberByEntity(id);
        if (m == nullptr) return 0;
        hp = std::min(1.0f, float(m->hpTenths) / 10.0f);
        mp = std::min(1.0f, float(m->mpTenths) / 10.0f);
        return 2;
    }
    case proto::EntityKind::Monster:
        if (!rows.Get("monsterStatus") || player_.target.entity != id) return 0;
        hp = share(player_.target.hp, player_.target.maxHp);
        return 1;
    default: return 0;
    }
}

// A random distance in [nearest, farthest] to stop short of what was clicked (the original's rand() between the two).
float PlayScreen::WalkUpDistance(float nearest, float farthest) {
    return std::uniform_real_distribution<float>(nearest, farthest)(walkRandom_);
}

bool PlayScreen::IsNpc(const game::WorldView::Entity& e) const {
    // By its model alone: a custom NPC has a name too (the server's, the Beta Helper; game/BetaQuests.h).
    if (!tables_ || !tables_->Usable() || e.kind != proto::EntityKind::Character) return false;
    const assets::ObjectRow* row = tables_->characters.Find(e.look.model);
    return row != nullptr && row->typeId[1] == 2 && row->typeId[2] == 2;
}

bool PlayScreen::IsPlayer(const game::WorldView::Entity& e) const {
    return e.kind == proto::EntityKind::Character && !e.look.name.empty() && !IsNpc(e);
}

bool PlayScreen::RayHitsEntity(float x, float y, float width, float height, std::uint32_t id, float& distance) const {
    // The model's box as it stands; one not on the stage is not drawn: not picked.
    const std::size_t slot = SlotOf(id);
    return slot != StageEffects::kNone && world_->RayHitsStage(x, y, width, height, slot, distance);
}

std::uint32_t PlayScreen::NpcUnder(float x, float y, float width, float height, float* outDistance, bool* centre) const {
    if (!world_) return 0;
    return OverNineRays(x, y, width, height, [&](float rx, float ry, float& bestDistance) {
        std::uint32_t best = 0;
        for (const auto& [id, e] : view_.Entities()) {
            float distance = 0;
            const bool gap = GapGate(e) != nullptr; // the Forgotten World's gap: its portal's size (ours)
            if (!IsNpc(e) && !gap) continue;
            const bool hit = gap ? world_->RayHitsCylinder(rx, ry, width, height, e.position, 30.0f, 120.0f, distance)
                                 : RayHitsEntity(rx, ry, width, height, id, distance);
            // Not behind a wall, a building or a hill drawn nearer under the cursor (ours: the user's report 2026-11-15;
            // the original picks through them).
            if (hit && distance < bestDistance && !world_->Occluded(x, y, width, height, distance)) {
                best = id;
                bestDistance = distance;
            }
        }
        return best;
    }, outDistance, centre);
}

std::uint32_t PlayScreen::MonsterUnder(float x, float y, float width, float height, float* outDistance, bool* centre) const {
    if (!world_ || !tables_ || !tables_->Usable()) return 0;
    // Its model's box as it stands, at its size (the original's pick); a fortress's structure too.
    return OverNineRays(x, y, width, height, [&](float rx, float ry, float& bestDistance) {
        std::uint32_t best = 0;
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind != proto::EntityKind::Monster || IsDead(e)) continue;
            float distance = 0;
            if (RayHitsEntity(rx, ry, width, height, id, distance) && distance < bestDistance && !world_->Occluded(x, y, width, height, distance)) {
                best = id;
                bestDistance = distance;
            }
        }
        return best;
    }, outDistance, centre);
}

std::uint32_t PlayScreen::PlayerUnder(float x, float y, float width, float height, float* outDistance, bool* centre) const {
    if (!world_) return 0;
    // Its model's box as it stands, living or dead (the original's: a body lying keeps its model's box).
    return OverNineRays(x, y, width, height, [&](float rx, float ry, float& bestDistance) {
        std::uint32_t best = 0;
        for (const auto& [id, e] : view_.Entities()) {
            if (id == view_.Self() || e.kind != proto::EntityKind::Character || IsNpc(e) || e.look.name.empty()) continue;
            float distance = 0;
            if (RayHitsEntity(rx, ry, width, height, id, distance) && distance < bestDistance && !world_->Occluded(x, y, width, height, distance)) {
                best = id;
                bestDistance = distance;
            }
        }
        return best;
    }, outDistance, centre);
}

void PlayScreen::SelectPlayer(std::uint32_t entity) {
    const game::WorldView::Entity* e = view_.Find(entity);
    if (e == nullptr) return;
    game::TargetStatus t;
    t.entity = entity;
    t.kind = game::TargetStatus::Kind::Player;
    t.model = e->look.model;
    t.name = e->look.name;
    t.level = std::uint8_t(std::clamp<int>(e->look.level, 0, 255));
    if (tables_ && tables_->Usable()) {
        if (const auto figure = tables_->rules.creation.FigureOf(e->look.model)) t.race = figure->race == game::CreationRules::Race::Europe ? 1 : 0;
    }
    t.hp = e->look.hp;
    player_.target = t;
}

void PlayScreen::SelectMonster(std::uint32_t entity) {
    const game::WorldView::Entity* e = view_.Find(entity);
    const assets::ObjectRow* row = e != nullptr && tables_->Usable() ? tables_->characters.Find(e->look.model) : nullptr;
    if (row == nullptr) return;
    // The client-side selection until targeting goes through the server (C10b).
    game::TargetStatus t;
    t.entity = entity;
    t.kind = fortress_.Structure(entity) != nullptr ? game::TargetStatus::Kind::Structure : game::TargetStatus::Kind::Monster; // its own window (iftw_fortressstructure.txt)
    t.model = e->look.model;
    t.name = gui_.data().Text(row->nameKey);
    if ((e->rarity & 0x0F) == 7) t.name = gui_.data().Text("UIIT_STT_MOB_STRONG") + " " + t.name; // an event's Strong one
    t.level = std::uint8_t(std::clamp(row->level, 0, 255));
    t.rarity = e->rarity & 0x0F;      // the grade
    t.party = (e->rarity >> 4) == 1;  // a party monster
    t.maxHp = proto::MonsterMaxHp(row->maxHp, e->rarity);
    if (const proto::EntityStructure* structure = fortress_.Structure(entity)) t.maxHp = structure->maxHp; // a fortress's (its server's)
    t.hp = e->look.hp;
    player_.target = t;
}

std::uint32_t PlayScreen::CosUnder(float x, float y, float width, float height, float* outDistance, bool* centre) const {
    if (!world_) return 0;
    const std::uint32_t own = OwnMount();
    return OverNineRays(x, y, width, height, [&](float rx, float ry, float& bestDistance) {
        std::uint32_t best = 0;
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind != proto::EntityKind::Cos || IsDead(e) || (own != 0 && id == own)) continue; // the player's own mount: never
            float distance = 0;
            if (RayHitsEntity(rx, ry, width, height, id, distance) && distance < bestDistance) {
                best = id;
                bestDistance = distance;
            }
        }
        return best;
    }, outDistance, centre);
}

void PlayScreen::SelectCos(std::uint32_t entity) {
    const game::WorldView::Entity* e = view_.Find(entity);
    const assets::ObjectRow* row = e != nullptr && tables_ && tables_->Usable() ? tables_->characters.Find(e->look.model) : nullptr;
    if (row == nullptr) return;
    game::TargetStatus t;
    t.entity = entity;
    t.kind = game::TargetStatus::Kind::Pet;
    t.model = e->look.model;
    // Its own name (a pet once named), else its kind's ("Wolf", "Horse").
    t.name = !e->look.name.empty() ? e->look.name : gui_.data().Text(row->nameKey);
    t.level = std::uint8_t(std::clamp(row->level, 0, 255));
    // A pet's window has no health bar; a horse's and a transport's has.
    const game::CosKind kind = game::CosKindOf(*row);
    t.noBar = kind == game::CosKind::AttackPet || kind == game::CosKind::GrabPet;
    t.maxHp = row->maxHp;
    t.hp = e->look.hp;
    player_.target = t;
}

bool PlayScreen::Hidden(std::uint32_t entity) const {
    if (!tables_ || !tables_->Usable()) return false;
    const auto hides = [&](std::uint32_t skill) {
        const assets::SkillRow* s = tables_->skills.Find(skill);
        const assets::SkillParam hide = s != nullptr ? assets::FindSkillParam(*s, assets::SkillTag("hide")) : assets::SkillParam{};
        return hide && (hide[0] == 1 || hide[0] == 2);
    };
    if (entity == view_.Self()) {
        return std::any_of(buffSkills_.begin(), buffSkills_.end(), [&](const auto& b) { return hides(b.second); });
    }
    const auto b = buffed_.find(entity);
    return b != buffed_.end() && std::any_of(b->second.skills.begin(), b->second.skills.end(), hides);
}

bool PlayScreen::IsDead(const game::WorldView::Entity& e) const {
    // Not while the killing hit waits for its moment.
    for (const HeldHit& h : heldHits_) {
        const auto* hit = std::get_if<proto::EntityDamaged>(&h.event);
        if (hit != nullptr && hit->target == e.id && (hit->flags & proto::kDamageKilled) != 0) return false;
    }
    // Monsters and players at no health (an NPC's summary carries none).
    if (e.kind == proto::EntityKind::Monster) return e.look.hp == 0;
    if (e.id == view_.Self()) return player_.self.hp == 0 && player_.self.maxHp > 0;
    return IsPlayer(e) && e.look.hp == 0;
}

void PlayScreen::Fight(ScreenContext& context, const game::TitleEvent& event) {
    if (!tables_ || !tables_->Usable()) return;
    if (const auto* blink = std::get_if<proto::EntityBlink>(&event)) {
        // Kept for its swing, which follows: a dash's time from where it stands.
        Blink b{blink->skill, 0.0};
        if (const game::WorldView::Entity* e = blink->kind == proto::BlinkKind::Dash ? view_.Find(blink->entity) : nullptr) {
            const proto::FlatPosition a = proto::ToFlat(e->position), z = proto::ToFlat(blink->to); // (the view takes the move after this)
            b.seconds = std::hypot(z.x - a.x, z.z - a.z) / double(proto::kBlinkDashSpeed);
        }
        blinks_[blink->entity] = b;
        return;
    }
    if (const auto* swing = std::get_if<proto::EntitySwing>(&event)) {
        // The skill's shot clips (skilleffect.txt by its base name, a monster's skill by its codename),
        // one after another swing by swing, for the skill's time.
        const assets::SkillRow* skill = tables_->skills.Find(swing->skill);
        if (skill == nullptr) return;
        const bool named = !skill->baseName.empty() && skill->baseName != "xxx";
        // Its clips and stages are those of the skill it copies when it names one (Shown: a bandit's attack plays its
        // player skill's), its times its own.
        const assets::SkillAnimation* anim = tables_->skillAnimations.Find(AnimationKey(Shown(*skill)));
        // A shot still waiting for its impact (P7) was fired all the same: a bolt's hit comes after its flight, and the
        // caster's next swing may come first. It goes now, the rest of its presentation ending (the user's report
        // 2026-11-22: a fire nuke sometimes showed no fireball).
        std::optional<PendingShot> fired;
        if (const auto p = pendingShots_.find(swing->attacker); p != pendingShots_.end()) {
            fired = std::move(p->second);
            pendingShots_.erase(p);
        }
        EndPresentation(swing->attacker, context.time); // the one running ends (a new B070 of its caster)
        if (fired) {
            pendingShots_[swing->attacker] = std::move(*fired);
            StartShot(swing->attacker, context.time);
        }
        swingTargets_[swing->attacker].clear(); // a new swing: its struck targets afresh
        // Its shot clip (one of its row's by a random byte of the cast, the ready and wait clips by
        // the same byte; ATTACK1 when it names none).
        std::uint32_t state = 2; // ATTACK1
        const std::uint32_t turn = clipRandom_() & 0xFFu;
        if (anim != nullptr && !anim->shot.empty()) state = std::uint32_t(anim->shot[turn % anim->shot.size()]);
        // A row with a wait clip and no shot (Ghost Walk's ANI_SKILL_7) holds its wait clip: never ATTACK1.
        else if (anim != nullptr && !anim->wait.empty()) state = std::uint32_t(anim->wait[turn % anim->wait.size()]);
        // A teleport's dash: its WAIT phase (clip and effect) is the run, then a short hold as it stops (the 1.188
        // client's 200 ms blend out).
        double dash = 0.0;
        if (const auto b = blinks_.find(swing->attacker); b != blinks_.end()) {
            if (b->second.skill == swing->skill) dash = std::max(0.05, b->second.seconds);
            blinks_.erase(b);
        }
        // A skill's clips in order: ready while it prepares, wait while it
        // casts, then its shot for its duration (skilldata cols 11, 12, 13; ours: the clips' own
        // lengths are the client's in the original). A phase with no clip of its row runs in the next
        // clip (StageEffects::Swing). A skill of no duration (a buff) shows its shot 0.8 s.
        // Frostbite and slow (its time percent, 200 and 125) stretch them all, its clips playing at 100 / percent, as
        // the original's.
        const double stretch = Stretch(swing->attacker);
        const double ready = std::max(0, skill->preparingMs) / 1000.0 * stretch, wait = dash > 0.0 ? dash : std::max(0, skill->castingMs) / 1000.0 * stretch;
        // The shot's time: its duration and those of the rows its chain adds (col 9; one clip shows
        // the whole chain), until its clip's own length is known.
        double seconds = dash > 0.0 ? 0.2 : skill->durationMs > 0 ? std::max(0.4, double(skill->durationMs) / 1000.0) : 0.8;
        int rows = 1;
        for (const assets::SkillRow* next = tables_->skills.Find(skill->chainNext); next != nullptr && rows < 16;
             next = tables_->skills.Find(next->chainNext), ++rows) {
            seconds += std::max(0, next->durationMs) / 1000.0;
        }
        if (dash <= 0.0) seconds *= stretch;
        // Its clips from its row's group (skillaniset2 col 5), whatever is in hand.
        const std::string set = anim != nullptr ? anim->group : std::string();
        // A skill whose row names no clips at all (an imbue: only its effect shows) moves nobody; its
        // stages keep the phases' own times.
        const bool moves = anim == nullptr || !anim->ready.empty() || !anim->wait.empty() || !anim->shot.empty();
        const bool readyClip = ready > 0.0 && anim != nullptr && !anim->ready.empty();
        const bool waitClip = wait > 0.0 && anim != nullptr && !anim->wait.empty();
        StageEffects::Swing times = StageEffects::Swing::Of(ready, wait, seconds, readyClip || !moves, waitClip || !moves);
        // The shot to its clip's end once its length is known (it may still be loading: see Motion).
        const std::size_t slot = SlotOf(swing->attacker);
        const double clip = moves && dash <= 0.0 && slot != StageEffects::kNone && world_ ? world_->StageClipSeconds(slot, set, state) * stretch : -1.0;
        if (clip > 0.0) times.end = times.shot + StageEffects::kClipHold + clip;
        // Its weapon's model (a throw flies it): the first model of the item in its weapon place.
        std::string weapon;
        if (const game::WorldView::Entity* e = view_.Find(swing->attacker)) {
            for (const proto::EquippedItem& item : e->look.equipment) {
                const assets::ItemRow* row = tables_->items.Find(item.item);
                if (row == nullptr || assets::EquipSlot(*row) != 6) continue;
                if (const std::vector<std::string> models = assets::WornModels(context.files, tables_->items, *row); !models.empty()) weapon = models.front();
            }
        }
        // Its sounds as its clips name them (the sound track of each clip; code 0, the
        // one-shot's hold before them), and none named: ours, at the swing.
        const std::size_t firstSound = clipSounds_.size();
        // Its sounds' key: a copy of another skill without rows of its own sounds as that one (Basic_Original: most
        // monster kins' attacks are their kin's, e.g. the Blood Sandman's ATTACK01 the Dark Sandman's).
        std::string soundKey = named ? skill->baseName : skill->codename;
        if (skill->original != 0 && context.audio != nullptr && !context.audio->HasSkillSounds(soundKey)) {
            if (const assets::SkillRow* original = tables_->skills.Find(skill->original)) {
                soundKey = !original->baseName.empty() && original->baseName != "xxx" ? original->baseName : original->codename;
            }
        }
        if (moves && slot != StageEffects::kNone && world_) {
            const game::WorldView::Entity* who = view_.Find(swing->attacker);
            const assets::ObjectRow* row = who != nullptr ? tables_->characters.Find(who->look.model) : nullptr;
            const bool isPlayer = who != nullptr && IsPlayer(*who);
            std::string hand;
            if (isPlayer) {
                hand = set.substr(0, set.find('_'));
                for (char& c : hand) c = char(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
                if (hand.empty() || hand == "DEFAULT") hand = "PUNCH";
            }
            const auto queue = [&](std::uint32_t clipState, double start) {
                for (const assets::ResourceData::SoundEvent& e : world_->StageSounds(slot, set, clipState)) {
                    clipSounds_.push_back({swing->attacker, 0, soundKey, hand, row != nullptr ? row->codename : std::string(),
                                           e.handle, e.wave, start + double(e.ms) / 1000.0 * stretch});
                }
            };
            if (readyClip) queue(std::uint32_t(anim->ready[turn % anim->ready.size()]), context.time + StageEffects::kClipHold);
            queue(state, context.time + times.shot + StageEffects::kClipHold);
        }
        const bool clipSounds = clipSounds_.size() > firstSound;
        if (context.audio != nullptr && world_) {
            if (const game::WorldView::Entity* who = view_.Find(swing->attacker)) {
                context.audio->Swing(swing->attacker, soundKey, world_->OriginRelative(who->position) + glm::vec3(0.0f, 10.0f, 0.0f), clipSounds);
            }
        }
        // A player that attacks (a skill of no wait clip) stands in its combat stance 5 s.
        if (const game::WorldView::Entity* who = view_.Find(swing->attacker);
            who != nullptr && IsPlayer(*who) && (anim == nullptr || anim->wait.empty())) {
            combatStance_[swing->attacker] = context.time + 5.0;
        }
        // Its effects (play/PlayEffects.cpp), on the times of the clips below and the shot's hit moments.
        // A wait clip's attack: its shot waits for its impact (the wait clip looping till then; P7, P19): its first hit's
        // message, or 400 ms past the data's time. Without a wait clip the clip runs from the cast, as the original's.
        const bool dot = assets::FindSkillParam(*skill, assets::SkillTag("lks2")) && assets::FindSkillParam(*skill, assets::SkillTag("puls"));
        const bool deferShot = dash <= 0.0 && waitClip && times.shot > 0.0 && game::TraitsOf(*skill).attack && !dot;
        SkillEffects(clipSounds, *skill, swing->attacker, swing->target, context.time, times, set, state, weapon, deferShot);
        if (!moves) return;
        Motion motion;
        motion.speed = float(1.0 / stretch);
        if (readyClip) motion.steps.push_back({std::uint32_t(anim->ready[turn % anim->ready.size()]), context.time + times.wait, ++takes_, set});
        if (waitClip) motion.steps.push_back({std::uint32_t(anim->wait[turn % anim->wait.size()]), context.time + times.shot, ++takes_, set});
        motion.steps.push_back({state, context.time + times.end, ++takes_, set});
        // A shot that waits for its impact (P7): its wait goes on until then (StartShot ends it; at the latest the
        // fallback's 400 ms), so its body's shot clip starts with its stages and ends with its motion.
        if (deferShot && motion.steps.size() >= 2) motion.steps[motion.steps.size() - 2].until = context.time + times.shot + 0.4;
        if (waitClip) {
            motion.waitLoop = true;
            motion.waitState = std::uint32_t(anim->wait[turn % anim->wait.size()]);
            motion.waitSet = set;
        }
        motion.locks = anim != nullptr && (!anim->ready.empty() || !anim->wait.empty() || !anim->shot.empty());
        motion.dash = dash > 0.0;
        if (motion.dash) {
            if (const auto* stages = StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill))) {
                for (const StageEffects::Stage& s : *stages) {
                    if (s.phase == StageEffects::kWait && s.action == "AT_LOOP" && s.object.ends_with(".efp")) motion.dashEffect = s.object;
                }
            }
        }
        for (std::size_t i = firstSound; i < clipSounds_.size(); ++i) clipSounds_[i].take = motion.steps.back().take;
        if (clip <= 0.0 && dash <= 0.0) motion.shotFrom = context.time + times.shot;
        // Its weapon hidden: from the cast (hide weapon 1), or as a WEAPON flight leaves the hand.
        if (anim != nullptr && anim->hideWeapon == 1) motion.hideFrom = context.time;
        if (const auto* stages = StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill))) {
            std::vector<double> events = slot != StageEffects::kNone ? world_->StageEvents(slot, set, state) : std::vector<double>{};
            for (double& t : events) t *= stretch;
            for (const StageEffects::Stage& s : *stages) {
                if (core::NormalizeAssetPath(s.object) != "weapon" || !s.action.starts_with("AT_MOV")) continue;
                const double launch = context.time + StageEffects::StageStart(s, times, events) + s.moveDelayMs / 1000.0;
                motion.hideFrom = motion.hideFrom < 0.0 ? launch : std::min(motion.hideFrom, launch);
            }
        }
        // Its weapon's trail, while the weapon shows (Hide Weapon 0): its clip's events
        // 5 and 6 turn it on and off.
        if (anim != nullptr && !anim->trailTexture.empty() && anim->hideWeapon == 0 && slot != StageEffects::kNone) {
            const std::vector<double> on = world_->StageEvents(slot, set, state, 5), off = world_->StageEvents(slot, set, state, 6);
            if (!on.empty()) {
                const double shot = context.time + times.shot + StageEffects::kClipHold;
                motion.trailOn = shot + on.front() * stretch;
                motion.trailOff = !off.empty() && off.front() > on.front() ? shot + off.front() * stretch : clip > 0.0 ? shot + clip : shot + on.front() * stretch + 0.4;
                motion.trailTexture = anim->trailTexture;
                motion.trailColour = anim->trailColour;
                motion.trailSeconds = anim->trailMs > 0 ? float(anim->trailMs) / 1000.0f : 0.2f;
            }
        }
        if (anim != nullptr) {
            motion.spineAxis = glm::vec3(anim->spineAxis[0], anim->spineAxis[1], anim->spineAxis[2]);
            motion.target = swing->target;
        }
        if (const auto old = motions_.find(swing->attacker); old != motions_.end() && old->second.trail != 0 && world_) {
            world_->Effects().EndTrail(old->second.trail);
        }
        motions_[swing->attacker] = std::move(motion);
        // Arrows and bolts are the skill's stages (SkillEffects above, world/StageEffects.h).
        return;
    }
    if (const auto* hit = std::get_if<proto::EntityDamaged>(&event)) {
        const bool tick = (hit->flags & proto::kDamageState) != 0; // a state's damage: no hit effect, no flinch
        // A player struck (not killed, not blocked) stands in its combat stance 5 s.
        if (const game::WorldView::Entity* struck = view_.Find(hit->target); struck != nullptr && !tick && (hit->flags & (proto::kDamageKilled | proto::kDamageBlocked)) == 0 &&
                                                                              IsPlayer(*struck)) {
            combatStance_[hit->target] = context.time + 5.0;
        }
        if (!tick) HitEffects(*hit);
        // Its number, the killing hit's too.
        if (hit->damage > 0) {
            const game::WorldView::Entity* target = view_.Find(hit->target);
            const game::WorldView::Entity* self = view_.Find(view_.Self());
            if (target != nullptr) {
                double distance = 0;
                if (self != nullptr) {
                    const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(target->position);
                    distance = std::hypot(a.x - b.x, a.z - b.z);
                }
                if (hit->attacker == view_.Self() || distance < 200.0) {
                    // A hit red over a player, white over the others; a state's tick (poison, burn, ...) yellow over a
                    // player, orange over the others (the original's).
                    const bool player = IsPlayer(*target);
                    const ui::Color colour = tick ? (player ? ui::Argb(0xFFFFD33A) : ui::Argb(0xFFFF713A)) : player ? ui::Argb(0xFFFF3A3A) : ui::Argb(0xFFFFFFFF);
                    damageTexts_.push_back({target->position, hit->damage, (hit->flags & proto::kDamageCritical) != 0, colour, context.time});
                }
            }
        } else if ((hit->flags & proto::kDamageBlocked) != 0) {
            // A blocked hit: the "blocking" mark over the struck one, rising as the numbers do (the original's kind 2).
            const game::WorldView::Entity* target = view_.Find(hit->target);
            const game::WorldView::Entity* self = view_.Find(view_.Self());
            if (target != nullptr && self != nullptr) {
                const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(target->position);
                if (hit->attacker == view_.Self() || hit->target == view_.Self() || std::hypot(a.x - b.x, a.z - b.z) < 200.0) {
                    damageTexts_.push_back({target->position, 0, false, IsPlayer(*target) ? ui::Argb(0xFFFF3A3A) : ui::Argb(0xFFFFFFFF), context.time, true});
                }
            }
        }
        if ((hit->flags & proto::kDamageKilled) != 0) {
            EndPresentation(hit->target, context.time); // a death ends what it was showing
            killed_[hit->target] = context.time;
            if (hit->target == view_.Self()) diedAt_ = context.time;
            motions_.erase(hit->target);
            if (hit->target == player_.target.entity && hit->target != view_.Self()) player_.target = {};
            return;
        }
        if (hit->hp > 0) killed_.erase(hit->target);

        // A hit flinches one that is not swinging itself (ours: the original's choice between the
        // light and the hard hit is not traced); one that lies plays DOWN_DAMAGE, one frozen, asleep or
        // stunned nothing.
        if (const auto k = knocked_.find(hit->target);
            !tick && hit->damage > 0 && k != knocked_.end() && k->second.kind == proto::KnockKind::Down && context.time < k->second.until) {
            motions_[hit->target] = {{{kDownDamage, context.time + 0.4, ++takes_}}};
            return;
        }
        const bool locked = InState(hit->target, game::StateKind::Freeze, context.time) || InState(hit->target, game::StateKind::Sleep, context.time) ||
                            InState(hit->target, game::StateKind::Stun, context.time);
        if (hit->damage > 0 && !tick && !locked) {
            // A player character: the 1.188 client's flinch, a layer of four bones over whatever it plays, its motion
            // untouched. Others: their DAMAGE1 when not swinging.
            const game::WorldView::Entity* struck = view_.Find(hit->target);
            if (struck != nullptr && IsPlayer(*struck)) {
                flinches_[hit->target] = ++takes_;
            } else if (const auto m = motions_.find(hit->target);
                       m == motions_.end() || m->second.End() <= context.time || m->second.steps.back().state == kDamage) {
                motions_[hit->target] = {{{kDamage, context.time + 0.5, ++takes_}}};
            }
        }
    }
}

void PlayScreen::EndPresentation(std::uint32_t entity, double now) {
    pendingShots_.erase(entity); // its shot never runs
    if (world_) stageEffects_.Cancel(*world_, SlotOf(entity), now);
    resultsAt_.erase(entity);
    for (HeldHit& h : heldHits_) {
        const auto* hit = std::get_if<proto::EntityDamaged>(&h.event);
        if (hit != nullptr && hit->attacker == entity && h.at > now) h.at = now; // its unshown hits show at once
    }
}

void PlayScreen::Sounds(ScreenContext& context, const game::TitleEvent& event) {
    GameAudio* audio = context.audio;
    if (audio == nullptr || !world_ || !tables_ || !tables_->Usable()) return;
    // Where one is, about its middle (ours: the original's sound points are not known yet).
    const auto at = [&](std::uint32_t entity, glm::vec3& out) {
        const game::WorldView::Entity* e = view_.Find(entity);
        if (e == nullptr) return false;
        out = world_->OriginRelative(e->position) + glm::vec3(0.0f, 10.0f, 0.0f);
        return true;
    };
    glm::vec3 p{0.0f};
    if (const auto* hit = std::get_if<proto::EntityDamaged>(&event)) { // (a swing's sounds: Fight)
        if (!at(hit->target, p)) return;
        if (hit->damage > 0 && hit->attacker != 0 && (hit->flags & proto::kDamageState) == 0) {
            const bool critical = (hit->flags & proto::kDamageCritical) != 0;
            hitSounds_.push_back({hit->attacker, hit->target, critical, context.time + 0.1}); // P12: 100 ms after
            // One's own critical shakes the camera 50 for 500 ms in steps of 20; in berserk (the original's character
            // mode 1) every hit of one's own does, a critical 200 in steps of 25.
            const bool berserk = context.time < berserkUntil_;
            if (hit->attacker == view_.Self() && (critical || berserk)) {
                const float amplitude = berserk && critical ? 200.0f : 50.0f;
                shake_.amplitude = std::max(shake_.amplitude * float(context.time < shake_.until), amplitude); // a running one's kept
                shake_.from = context.time;
                shake_.until = context.time + 0.5;
                shake_.step = berserk && critical ? 0.025f : 0.02f;
                shake_.next = context.time;
            }
            // The struck one's voice (effectsound's VOC_MOAN of its object; a killing hit has its death's instead).
            if ((hit->flags & proto::kDamageKilled) == 0) {
                if (const game::WorldView::Entity* struck = view_.Find(hit->target)) {
                    if (const assets::ObjectRow* row = tables_->characters.Find(struck->look.model)) audio->Moan(row->codename, critical, p);
                }
            }
        }
        if ((hit->flags & proto::kDamageBlocked) != 0) {
            // Its attacker's bow or crossbow has its own (effectsound's SND_BLOCKING rows).
            std::string_view weapon;
            if (const game::WorldView::Entity* a = view_.Find(hit->attacker)) {
                for (const proto::EquippedItem& item : a->look.equipment) {
                    const assets::ItemRow* row = tables_->items.Find(item.item);
                    if (row == nullptr || assets::EquipSlot(*row) != 6) continue;
                    weapon = row->typeId[3] == 6 ? "BOW" : row->typeId[3] == 12 ? "CROSSBOW" : "";
                }
            }
            audio->Blocked(weapon, (hit->flags & proto::kDamageCritical) != 0, p);
        }
        if ((hit->flags & proto::kDamageKilled) != 0 && !killed_.contains(hit->target)) {
            if (const assets::ObjectRow* row = tables_->characters.Find(view_.Find(hit->target)->look.model)) audio->Death(row->codename, p);
        }
    } else if (const auto* level = std::get_if<proto::EntityLevelUp>(&event)) {
        // Only the player's own; others see the effect in silence.
        if (level->entity == view_.Self()) audio->PlayUi("SND_LEVUP");
    } else if (const auto* action = std::get_if<proto::EntityAction>(&event)) {
        if (action->action == proto::EntityActionKind::PickUp && action->entity == view_.Self()) audio->PlayUi("SND_PICKUP");
    } else if (const auto* worn = std::get_if<proto::AvatarMoved>(&event)) {
        // An avatar put on: its equip sound, as an item worn (none for a kind effectsound.txt has none of).
        if (worn->status != proto::ItemStatus::Ok) return;
        for (const proto::ItemRecord& r : worn->avatars) {
            if (const assets::ItemRow* row = tables_->items.Find(r.item)) audio->Equip(*row);
        }
    } else if (const auto* moved = std::get_if<proto::ItemMoved>(&event)) {
        // A move the player made (a pick-up comes back with from = to: it has its own sound): the
        // item now in the place it went to (snd_equip by its type).
        if (moved->status == proto::ItemStatus::Ok && moved->from == moved->to) {
            // An item become another in its place (the gender switch): its slot's swirl (by the new item id: worn or in
            // the bag). Read before the state takes it.
            for (const proto::ItemRecord& r : moved->changed) {
                const game::ItemInstance* was = r.slot == moved->to ? player_.inventory.At(r.slot) : nullptr;
                if (was != nullptr && r.item != 0 && was->refId != r.item) FlashItemSlot(hudContext_, r.slot, ui::Slot::Flash::Switch);
            }
            return;
        }
        if (moved->status != proto::ItemStatus::Ok) return;
        for (const proto::ItemRecord& r : moved->changed) {
            if (r.slot != moved->to) continue;
            if (const assets::ItemRow* row = tables_->items.Find(r.item)) audio->Equip(*row);
        }
    } else if (const auto* cool = std::get_if<proto::Cooldown>(&event)) {
        // A potion drunk: the server starts its wait (TID 3/3/1).
        const assets::ItemRow* row = cool->kind == proto::CooldownKind::Item ? tables_->items.Find(cool->id) : nullptr;
        if (row != nullptr && row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 1) audio->PlayUi("SND_POTION");
    } else if (const auto* came = std::get_if<proto::EntityAppeared>(&event)) {
        // An item just fallen plays its type's snd_dropitem; effectsound.txt's only live row of
        // it is gold's: every other item lands in silence.
        if (came->kind != proto::EntityKind::GroundItem || !came->fresh) return;
        const assets::ItemRow* row = tables_->items.Find(came->character.model);
        if (row != nullptr && IsGold(*row)) audio->GoldDropped(world_->OriginRelative(came->position));
    } else if (std::holds_alternative<proto::WorldLeft>(event)) {
        audio->Silence();
    }
}

void PlayScreen::Listen(ScreenContext& context) {
    GameAudio* audio = context.audio;
    if (audio == nullptr || !world_ || !shown_) return;
    // The ear at the camera's eye (as SceneRenderer's view: `distance` from the target along the
    // pitch and yaw), looking at the target.
    const OrbitCamera& c = world_->Camera();
    const glm::vec3 target(c.targetX, c.targetY, c.targetZ);
    const glm::vec3 dir(std::cos(c.pitch) * std::sin(c.yaw), std::sin(c.pitch), std::cos(c.pitch) * std::cos(c.yaw));
    audio->SetListener(c.fly ? target : target + dir * c.distance, c.fly ? dir : -dir, glm::vec3(0.0f, 1.0f, 0.0f));
    // The music and ambience of the area the character stands in; night from 18:00 to 6:00 (ours).
    if (const game::WorldView::Entity* self = view_.Find(view_.Self())) {
        const float t = world_->TimeOfDay();
        audio->SetPlace(self->position.region, self->position.x, self->position.z, t < 0.25f || t >= 0.75f);
    }
}

void PlayScreen::DeathWindow(double now, bool reopen) {
    const bool dead = player_.self.hp == 0 && player_.self.maxHp > 0;
    if (!dead) {
        if (diedAt_ != -1) hud_.CloseRebirth(); // up again: every message box goes (as the original's)
        diedAt_ = -1;
        warCountAt_ = -1;
        warCountShown_ = -1;
        return;
    }
    // Dead in a fortress's area during its war: "%d seconds till resurrection." on the warning banner from the death,
    // once a second from 15 down to 0 (the original's interface timer).
    if (warCountAt_ == -1) {
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        warCountAt_ = self != nullptr && fortress_.WarArea(self->position.region) != 0 ? now : -2;
    }
    if (warCountAt_ >= 0 && warCountShown_ != 0) {
        const int left = std::max(0, 15 - int(now - warCountAt_));
        if (left != warCountShown_) {
            warCountShown_ = left;
            char text[128];
            std::snprintf(text, sizeof text, gui_.data().Text("UIIT_MSG_FORT_REVIVE_COUNT").c_str(), left);
            player_.Notify(game::Banner::Kind::Warning, text, now);
        }
    }
    if (reopen) {
        diedAt_ = now - 3.0;
    } else if (diedAt_ == -2) {
        return; // answered or closed: waits for the server, or for a click on its own bar
    } else if (diedAt_ < 0) {
        diedAt_ = now; // came into the world dead
    }
    if (hud_.RebirthShown() || now - diedAt_ < 3.0) return;
    // In a fortress's area during its war: where this side rises (the box opens 3 s after the death, rising is the
    // server's from 15 s on). The holder's guild by the Gate of Resurrection, its union's by the Gate of Fortress; the
    // others at their command post's tent (its two lines), else before the Gate of Fortress.
    std::vector<std::string> warLines;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const std::uint32_t war = self != nullptr ? fortress_.WarArea(self->position.region) : 0;
    if (const proto::FortressRecord* f = war != 0 ? fortress_.Fortress(war) : nullptr) {
        const std::uint32_t mine = guild_.InGuild() ? guild_.Guild().id : 0, holder = f->holder != 0 ? f->holder : f->owner;
        const auto text = [&](const char* key) { return gui_.data().Text(key); };
        if (mine != 0 && mine == holder) {
            warLines.push_back(text("UIIT_MSG_FORT_REVIVE_TEXT_03"));
        } else if (mine != 0 && guild_.Guild().unionId != 0 && guild_.Union().id == guild_.Guild().unionId && std::any_of(guild_.Union().guilds.begin(), guild_.Union().guilds.end(), [&](const auto& g) { return g.id == holder; })) {
            warLines.push_back(text("UIIT_MSG_FORT_REVIVE_TEXT_04"));
        } else {
            warLines.push_back(text("UIIT_MSG_FORT_REVIVE_TEXT_01"));
            warLines.push_back(text("UIIT_MSG_FORT_REVIVE_TEXT_NEW_01"));
        }
        warLines.push_back(text("UIIT_MSG_FORT_REVIVE_PENALTY_TEXT_01"));
        warLines.push_back(text("UIIT_MSG_FORT_REVIVE_PENALTY_TEXT_02"));
    }
    // In a Capture the Flag match its first button reads "Resurrect at your camp.".
    const bool match = flag_.InMatch(view_.Self()) && !arena_.InArena(); // an arena match revives by itself (FF F1)
    hud_.Rebirth(player_.self.level, [this, now](int kind) {
        if (kind > 0 && !replay_) shared_->session->Resurrect(std::uint8_t(kind));
        if (kind == 1) FlagDeath(now); // the camp countdown (PlayFlagWorld.cpp)
        diedAt_ = -2;
    }, warLines, warLines.empty() ? 0.0 : (warCountAt_ >= 0 ? warCountAt_ : diedAt_) + 15.0, match ? "UIIT_MSG_FLAGWAR_GO_TO_REVIVAL" : "");
}

void PlayScreen::RefreshTarget(double now) {
    game::TargetStatus& t = player_.target;
    if (!t.Selected() || (t.kind != game::TargetStatus::Kind::Monster && t.kind != game::TargetStatus::Kind::Pet)) return;
    if (const game::WorldView::Entity* e = view_.Find(t.entity)) {
        t.hp = e->look.hp;
        t.states = StatesOf(t.entity, now);
        if (t.kind == game::TargetStatus::Kind::Pet && !e->look.name.empty()) t.name = e->look.name; // named meanwhile
    } else {
        t = {}; // out of view: the window hides
    }
}

std::uint32_t PlayScreen::GroundUnder(float x, float y, float width, float height, float* outDistance) const {
    if (outDistance != nullptr) *outDistance = 0.0f; // its name under the mouse: in front of everything
    if (!world_) return 0;
    // Its name first (the nearest of the ones under the mouse), then a small upright cylinder over
    // each item (ours: the original picks the model).
    std::uint32_t best = 0;
    float bestDepth = 2.0f;
    for (const ItemLabel& l : itemLabels_) {
        if (x >= l.rect.x && x <= l.rect.Right() && y >= l.rect.y && y <= l.rect.Bottom() && l.depth < bestDepth && view_.Find(l.id) != nullptr) {
            best = l.id;
            bestDepth = l.depth;
        }
    }
    if (best != 0) return best;
    float bestDistance = 1e9f;
    for (const auto& [id, e] : view_.Entities()) {
        float distance = 0;
        if (e.kind == proto::EntityKind::GroundItem && world_->RayHitsCylinder(x, y, width, height, e.position, 7.0f, 8.0f, distance) &&
            distance < bestDistance) {
            best = id;
            bestDistance = distance;
        }
    }
    if (outDistance != nullptr) *outDistance = bestDistance;
    return best;
}

void PlayScreen::SyncGround(double now) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    // New ones: the item's ground model (itemdata col 53: a pouch, a box, coins); gone ones taken away.
    for (const auto& [id, e] : view_.Entities()) {
        if (e.kind != proto::EntityKind::GroundItem || world_->HasGroundItem(id)) continue;
        const assets::ItemRow* row = tables_->items.Find(e.look.model);
        if (row == nullptr || row->dropModelPath.empty()) continue;
        const glm::vec3 p = world_->OriginRelative(e.position);
        // Gold just fallen: its coin shower first, where its heap will lie (the heap asked for now, hidden until
        // then); without a free slot (a crowd fills them all) the heap at once.
        bool shower = false;
        if (e.fresh && IsGold(*row)) {
            const auto walkers = slotUsed_.begin() + std::ptrdiff_t(std::min(kWalkerSlots, slotUsed_.size()));
            if (const auto free = std::find(slotUsed_.begin(), walkers, false); free != walkers) {
                *free = true;
                const std::size_t slot = std::size_t(free - slotUsed_.begin());
                WorldScreen::StageLook look{kGoldShower};
                look.shadow = false;
                look.once = true;
                world_->SetStage(slot, look, p.x, p.y, p.z, 0.0f);
                world_->SetStageTransform(slot, WorldScreen::GroundPlace(p.x, p.y, p.z, e.yaw));
                showers_[id] = {slot, now};
                shower = true;
            }
        }
        world_->SetGroundItem(id, "res\\" + row->dropModelPath, p.x, p.y, p.z, e.yaw, !shower);
        groundShown_.insert(id);
    }
    // A shower's clip played to its end (from when its model came in): the heap in its place, in the same frame.
    for (auto it = showers_.begin(); it != showers_.end();) {
        Shower& s = it->second;
        if (s.shownAt < 0.0 && world_->StageReady(s.slot)) {
            s.shownAt = now;
            s.seconds = std::max(0.0, world_->StageClipSeconds(s.slot, "", 0));
        }
        const bool over = s.shownAt >= 0.0 ? now - s.shownAt >= s.seconds : now - s.since >= kShowerWait;
        const std::uint32_t entity = it->first;
        ++it;
        if (!over) continue;
        core::LogInfo("game: gold {}: its coin shower {} after {:.2f} s, its heap shown", entity, s.shownAt >= 0.0 ? "played" : "not in", now - s.since);
        EndShower(entity);
    }
    for (auto it = groundShown_.begin(); it != groundShown_.end();) {
        if (view_.Find(*it) == nullptr) {
            EndShower(*it);
            world_->ClearGroundItem(*it);
            if (pendingPick_ == *it) pendingPick_ = 0;
            it = groundShown_.erase(it);
        } else {
            ++it;
        }
    }
}

void PlayScreen::StageProps(double now) {
    if (!world_) return;
    for (const StageEffects::TargetModel& m : stageEffects_.TakeTargetModels(now)) {
        glm::mat4 root(1.0f);
        if (!world_->StageBone(m.target, {}, root)) continue;
        const auto walkers = slotUsed_.begin() + std::ptrdiff_t(std::min(kWalkerSlots, slotUsed_.size()));
        const auto free = std::find(slotUsed_.begin(), walkers, false);
        if (free == walkers) continue; // a crowd fills them all: none
        *free = true;
        const std::size_t slot = std::size_t(free - slotUsed_.begin());
        WorldScreen::StageLook look{m.model};
        look.shadow = false;
        look.once = true;
        const glm::vec3 p(root[3]);
        world_->SetStage(slot, look, p.x, p.y, p.z, 0.0f);
        world_->SetStageTransform(slot, WorldScreen::GroundPlace(p.x, p.y, p.z, 0.0f));
        stageProps_.push_back({slot, now});
    }
    // Each to its clip's end (from when its model came in), else gone after the shower's wait.
    std::erase_if(stageProps_, [&](Shower& s) {
        if (s.shownAt < 0.0 && world_->StageReady(s.slot)) {
            s.shownAt = now;
            s.seconds = std::max(0.3, world_->StageClipSeconds(s.slot, "", 0));
        }
        const bool over = s.shownAt >= 0.0 ? now - s.shownAt >= s.seconds : now - s.since >= kShowerWait;
        if (!over) return false;
        world_->ClearStage(s.slot);
        slotUsed_[s.slot] = false;
        return true;
    });
}

// The shower goes and the heap shows (picked up meanwhile: both go).
void PlayScreen::EndShower(std::uint32_t entity) {
    const auto it = showers_.find(entity);
    if (it == showers_.end()) return;
    if (world_) {
        world_->ClearStage(it->second.slot);
        world_->ShowGroundItem(entity);
    }
    slotUsed_[it->second.slot] = false;
    showers_.erase(it);
}

// Whether the character stands within `reach` of `entity`: what a click on it waits for (the walk's
// answer comes a moment after the click, so "not moving" alone is not "arrived").
bool PlayScreen::StandingNear(std::uint32_t entity, float reach) const {
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const game::WorldView::Entity* it = view_.Find(entity);
    if (self == nullptr || it == nullptr || self->moving) return false;
    const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(it->position);
    return std::hypot(a.x - b.x, a.z - b.z) <= reach;
}

void PlayScreen::FaceToward(std::uint32_t target) {
    if (replay_ || !shared_ || !shared_->session || OwnMount() != 0) return; // a rider turns with his mount
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const game::WorldView::Entity* it = view_.Find(target);
    if (self == nullptr || it == nullptr || self->moving || IsDead(*self)) return;
    const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(it->position);
    if (std::hypot(b.x - a.x, b.z - a.z) < 0.5) return;
    const float yaw = float(std::atan2(b.x - a.x, b.z - a.z));
    float diff = std::fmod(std::abs(yaw - self->yaw), 6.2831853f);
    if (diff > 3.1415926f) diff = 6.2831853f - diff;
    if (diff < 0.017453292f) return; // under a degree: as it stands
    view_.Apply(proto::EntityTurned{view_.Self(), proto::HeadingOf(yaw)});
    shared_->session->Turn(yaw);
}

void PlayScreen::Npcs(double now) {
    AskGaps();
    if (pendingNpc_ == 0 || replay_ || heldMove_) return; // its walk kept through a swing: not there yet
    // Near and standing: its menu (a gap's or an exit's question); given up when the walk did not get there.
    if (StandingNear(pendingNpc_, UseReach(pendingNpc_, kNpcReach))) {
        FaceToward(pendingNpc_);
        if (UsedNear(pendingNpc_)) UseAt(pendingNpc_);
        else shared_->session->TalkToNpc(pendingNpc_);
        pendingNpc_ = 0;
    } else if (now > pendingUntil_) {
        pendingNpc_ = 0;
    }
}

void PlayScreen::Gates(ScreenContext& context) {
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    // A walk kept through a swing (ClickWalk): not there yet, nor given up.
    if (pendingGate_ == 0 || self == nullptr || self->moving || heldMove_ || replay_ || !tables_ || !tables_->Usable()) return;
    const assets::TeleportGate* gate = tables_->teleports.Gate(pendingGate_);
    const assets::TeleportPoint* from = gate ? tables_->teleports.PointOf(gate->refObjId) : nullptr;
    if (from == nullptr) {
        pendingGate_ = 0;
        return;
    }
    if (GateNear(self->position, kGateReach) != gate) {
        // Not there yet: a scripted use walks on toward it a few times; a click gives up.
        if (gateTries_ <= 0) {
            core::LogWarning("game: {} is out of reach", gate->codename);
            pendingGate_ = 0;
        } else if ((walkWait_ -= float(context.dt)) <= 0.0f) {
            // Straight at it; when a straight walk stops short, a step aside (left, then right,
            // farther each time) before trying again (clicks walk straight, as the original's).
            const int attempt = 20 - gateTries_--;
            // A ground click by the building (as a player's: the building's own height may stand far above the ground,
            // e.g. the desert gate 27751's 300 over ground at 49, which the server refuses as off the navigation).
            proto::WorldPosition target{gate->region, gate->x, gate->region == self->position.region ? self->position.y : gate->y, gate->z};
            if (attempt % 2 == 1) {
                const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(target);
                const double dx = b.x - a.x, dz = b.z - a.z, length = std::max(1.0, std::hypot(dx, dz));
                const double side = ((attempt / 2) % 2 == 0 ? 1.0 : -1.0) * 60.0 * double(1 + attempt / 4);
                target = proto::FromFlat({a.x - dz / length * side, a.z + dx / length * side}, self->position.y);
            }
            shared_->session->MoveTo(target);
            walkWait_ = 1.0f; // the server's answer starts the leg; until then, no new order
        }
        return;
    }
    if (talkAtGate_) {
        // A click: what the gate offers (the server answers GateMenu; the NPC window lists it).
        pendingGate_ = 0;
        talkAtGate_ = false;
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat({gate->region, gate->x, gate->y, gate->z});
        core::LogInfo("game: talking to {} from {:.0f} away", gate->codename, std::hypot(a.x - b.x, a.z - b.z));
        shared_->session->TalkToGate(gate->refObjId);
        return;
    }
    const auto links = tables_->teleports.LinksFrom(from->id);
    const auto chosen =
        std::find_if(links.begin(), links.end(), [&](const assets::TeleportLink* l) { return gateTo_ == 0 || l->to == gateTo_; });
    pendingGate_ = 0;
    if (chosen == links.end()) {
        // No link there: a destination the server may have opened for it at run time (the snake gate's), asked as it is.
        if (gateTo_ != 0) {
            core::LogInfo("game: through {} to point {} (no link of the game files)", gate->codename, gateTo_);
            shared_->session->UseGate(gate->refObjId, gateTo_);
            waitingForGate_ = true;
            return;
        }
        core::LogWarning("game: {} does not lead there", gate->codename);
        return;
    }
    const assets::TeleportPoint* to = tables_->teleports.Point((*chosen)->to);
    core::LogInfo("game: through {} to {} ({} gold)", gate->codename, to ? to->codename : "?", (*chosen)->fee);
    shared_->session->UseGate(gate->refObjId, (*chosen)->to);
    waitingForGate_ = true;
}

void PlayScreen::DevTools(ScreenContext&) {
    if (!tables_ || !tables_->Usable() || replay_) return;
    ImGui::SetNextWindowPos({16, 420}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Teleport (developer)")) {
        ImGui::TextUnformatted("To a teleport point's arrival (the server needs --dev-commands).");
        const auto& points = tables_->teleports.Points();
        // The outdoor ones by their region's x and y, a dungeon's by its region (the tomb's floors 0x8002..0x8007, ...).
        const auto label = [&](const assets::TeleportPoint& p) {
            if ((p.region & 0x8000) != 0) return std::format("{} {} (dungeon 0x{:04X})", p.id, p.codename, p.region);
            return std::format("{} {} ({},{})", p.id, p.codename, p.region & 0xFF, p.region >> 8);
        };
        // Case-blind "contains" for the lists' filters.
        const auto matches = [](std::string_view text, const char* filter) {
            if (filter[0] == '\0') return true;
            const std::string_view f(filter);
            const auto it = std::search(text.begin(), text.end(), f.begin(), f.end(),
                                        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
            return it != text.end();
        };
        ImGui::TextUnformatted("Its ride or transport and pets come along (on the same game server).");
        if (devPoint_ >= int(points.size())) devPoint_ = 0;
        ImGui::InputText("filter##point", devFilter_, sizeof devFilter_);
        if (!points.empty() && ImGui::BeginCombo("point", label(points[std::size_t(devPoint_)]).c_str())) {
            for (std::size_t i = 0; i < points.size(); ++i) {
                const assets::TeleportPoint& p = points[i];
                if (!p.HasArrival()) continue;
                const std::string text = label(p);
                if (!matches(text, devFilter_)) continue;
                if (ImGui::Selectable(text.c_str(), int(i) == devPoint_)) devPoint_ = int(i);
            }
            ImGui::EndCombo();
        }
        if (!points.empty() && ImGui::Button("Go##point")) {
            core::LogInfo("game: developer teleport to {}", points[std::size_t(devPoint_)].codename);
            shared_->session->DevGoto(points[std::size_t(devPoint_)].id);
        }

        // A place by the minimap's numbers (X, Y: tenths of a region's 192 steps; HudMinimap's CoordinateX / Y turned back).
        ImGui::Separator();
        ImGui::TextUnformatted("To a place (the minimap's X, Y):");
        if (const game::WorldView::Entity* self = view_.Find(view_.Self()); self != nullptr && ImGui::Button("Here")) {
            devX_ = Minimap::CoordinateX(self->position.region, self->position.x);
            devY_ = Minimap::CoordinateY(self->position.region, self->position.z);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("X", &devX_, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("Y", &devY_, 0);
        ImGui::SameLine();
        if (ImGui::Button("Go##place")) {
            // X = (rx - 135) x 192 + x / 10, Y = (rz - 92) x 192 + z / 10 (floored: a negative X is west of region 135).
            const auto split = [](int v, int origin, int& region, float& at) {
                const int q = v >= 0 ? v / 192 : -((-v + 191) / 192);
                region = origin + q;
                at = float(v - q * 192) * 10.0f + 5.0f;
            };
            int rx = 0, rz = 0;
            float x = 0, z = 0;
            split(devX_, 135, rx, x);
            split(devY_, 92, rz, z);
            if (rx >= 0 && rx < 256 && rz >= 0 && rz < 128) {
                core::LogInfo("game: developer teleport to X {} Y {} ({},{})", devX_, devY_, rx, rz);
                shared_->session->SendGame(proto::DevGotoPlace::kId,
                                           proto::Encode(proto::DevGotoPlace{std::uint16_t(rx | (rz << 8)), x, z}));
            }
        }

        // An NPC where npcpos.txt puts it (the server's first of that codename), by its name or codename.
        ImGui::Separator();
        ImGui::InputText("filter##npc", devNpcFilter_, sizeof devNpcFilter_);
        const auto npcLabel = [&](const assets::ObjectRow& row) {
            const std::string name = row.nameKey.empty() ? std::string() : gui_.data().Text(row.nameKey);
            return std::format("{} ({})", name.empty() ? row.codename : name, row.codename);
        };
        const assets::ObjectRow* picked = devNpc_ != 0 ? tables_->characters.Find(devNpc_) : nullptr;
        if (ImGui::BeginCombo("NPC", picked != nullptr ? npcLabel(*picked).c_str() : "")) {
            std::set<std::uint32_t> shown;
            for (const assets::NpcPosition& p : tables_->npcPlaces) {
                if ((p.region & 0x8000) != 0 || !shown.insert(p.refObjId).second) continue;
                const assets::ObjectRow* row = tables_->characters.Find(p.refObjId);
                if (row == nullptr || row->typeId[1] != 2 || row->typeId[2] != 2) continue;
                const std::string text = npcLabel(*row);
                if (!matches(text, devNpcFilter_)) continue;
                if (ImGui::Selectable(text.c_str(), row->refObjId == devNpc_)) devNpc_ = row->refObjId;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (picked != nullptr && ImGui::Button("Go##npc")) {
            core::LogInfo("game: developer teleport to {}", picked->codename);
            shared_->session->SendGame(proto::DevGotoMonster::kId, proto::Encode(proto::DevGotoMonster{picked->codename}));
        }

        // A monster's nearest nest (its codename, e.g. MOB_CH_TIGERWOMAN).
        ImGui::InputText("monster", devMonster_, sizeof devMonster_);
        ImGui::SameLine();
        if (devMonster_[0] != '\0' && ImGui::Button("Go##monster")) {
            core::LogInfo("game: developer teleport to a nest of {}", devMonster_);
            shared_->session->SendGame(proto::DevGotoMonster::kId, proto::Encode(proto::DevGotoMonster{devMonster_}));
        }
    }
    ImGui::End();
    NetworkPanel();
}

// The game connection's rates over the last second, taken once a second from the session's counts.
void PlayScreen::SampleNetwork() {
    if (!shared_ || !shared_->session) return;
    const game::SessionNetworkStats& net = shared_->session->Network();
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (netSample_.time < 0 || now - netSample_.time >= 1.0) {
        const double span = netSample_.time < 0 ? 0.0 : now - netSample_.time;
        if (span > 0) {
            netRates_.bytesIn = double(net.game.bytesReceived - netSample_.bytesIn) / span;
            netRates_.bytesOut = double(net.game.bytesSent - netSample_.bytesOut) / span;
            netRates_.messagesIn = double(net.game.messagesReceived - netSample_.messagesIn) / span;
            netRates_.messagesOut = double(net.game.messagesSent - netSample_.messagesOut) / span;
            netRates_.byId.clear();
            for (const auto& [id, kind] : net.received) {
                const auto before = netSample_.byId.find(id);
                const double rate = double(kind.messages - (before != netSample_.byId.end() ? before->second : 0)) / span;
                if (rate > 0) netRates_.byId.push_back({id, rate});
            }
            std::sort(netRates_.byId.begin(), netRates_.byId.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        }
        netSample_.time = now;
        netSample_.bytesIn = net.game.bytesReceived;
        netSample_.bytesOut = net.game.bytesSent;
        netSample_.messagesIn = net.game.messagesReceived;
        netSample_.messagesOut = net.game.messagesSent;
        netSample_.byId.clear();
        for (const auto& [id, kind] : net.received) netSample_.byId[id] = kind.messages;
    }
}

// The performance panel's numbers: the frames of the last second (counted from this screen's frames), the latency (a
// walk's round trip, the only one the protocol has) and the connection's rates.
void PlayScreen::SamplePerformance(const ScreenContext& context) {
    const double ms = context.dt * 1000.0;
    if (frameWindow_.start < 0) frameWindow_.start = context.time;
    ++frameWindow_.frames;
    frameWindow_.sumMs += ms;
    frameWindow_.worstMs = std::max(frameWindow_.worstMs, ms);
    if (context.time - frameWindow_.start >= 1.0) {
        const double span = context.time - frameWindow_.start;
        performance_.fps = double(frameWindow_.frames) / span;
        performance_.frameMs = frameWindow_.sumMs / double(frameWindow_.frames);
        performance_.worstMs = frameWindow_.worstMs;
        frameWindow_ = {context.time, 0, 0.0, 0.0};
    }
    SampleNetwork();
    performance_.connected = shared_ && shared_->session && !replay_;
    if (performance_.connected) {
        const game::SessionNetworkStats& net = shared_->session->Network();
        if (!net.moveTrips.empty()) performance_.latencyMs = net.moveTrips.back();
        performance_.totalIn = net.game.bytesReceived;
        performance_.totalOut = net.game.bytesSent;
    }
    performance_.bytesIn = netRates_.bytesIn;
    performance_.bytesOut = netRates_.bytesOut;
    performance_.messagesIn = netRates_.messagesIn;
    performance_.messagesOut = netRates_.messagesOut;
}

// The F1 Network panel: the game connection live (rates over the last second), what the view holds
// by kind against what the server allows, a walk's round trip, and the messages by kind.
void PlayScreen::NetworkPanel() {
    if (!shared_ || !shared_->session) return;
    const game::SessionNetworkStats& net = shared_->session->Network();
    ImGui::SetNextWindowPos({16, 560}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Network")) {
        ImGui::Text("in  %8.1f KB/s  %7.0f messages/s", netRates_.bytesIn / 1000.0, netRates_.messagesIn);
        ImGui::Text("out %8.1f KB/s  %7.0f messages/s", netRates_.bytesOut / 1000.0, netRates_.messagesOut);
        ImGui::Text("total in %.1f MB, out %.1f MB; latest-lane replaced %llu", double(net.game.bytesReceived) / 1e6, double(net.game.bytesSent) / 1e6,
                    static_cast<unsigned long long>(net.game.latestReplaced));
        // A walk's round trip: a click to the server's word on the player's own new leg.
        if (!net.moveTrips.empty()) {
            std::vector<double> trips(net.moveTrips.begin(), net.moveTrips.end());
            std::sort(trips.begin(), trips.end());
            ImGui::Text("walk round trip: %.0f ms last, %.0f ms median, %.0f ms worst (last %zu)", net.moveTrips.back(), trips[trips.size() / 2],
                        trips.back(), trips.size());
        } else {
            ImGui::TextUnformatted("walk round trip: no walk yet");
        }
        // What the view holds, by kind, against what the server allows this player.
        int players = 0, monsters = 0, items = 0, npcs = 0;
        for (const auto& [id, e] : view_.Entities()) {
            if (id == view_.Self()) continue;
            if (e.kind == proto::EntityKind::Monster) ++monsters;
            else if (e.kind == proto::EntityKind::GroundItem) ++items;
            else if (e.look.name.empty()) ++npcs;
            else ++players;
        }
        if (haveViewAnswer_) {
            ImGui::Text("view: %d / %u players, %d / %u monsters, %d items, %d NPCs (server's most %u / %u, %u map units)", players,
                        unsigned(viewAnswer_.players), monsters, unsigned(viewAnswer_.monsters), items, npcs, unsigned(viewAnswer_.maxPlayers),
                        unsigned(viewAnswer_.maxMonsters), unsigned(viewAnswer_.radius));
        } else {
            ImGui::Text("view: %d players, %d monsters, %d items, %d NPCs (the server has not said its limits)", players, monsters, items, npcs);
        }
        if (ImGui::BeginTable("messages", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("message");
            ImGui::TableSetupColumn("per second");
            ImGui::TableSetupColumn("total");
            ImGui::TableHeadersRow();
            for (const auto& [id, rate] : netRates_.byId) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (const char* name = game::WorldMessageName(id)) ImGui::TextUnformatted(name);
                else ImGui::Text("0x%04x", unsigned(id));
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", rate);
                ImGui::TableNextColumn();
                const auto it = net.received.find(id);
                ImGui::Text("%llu", static_cast<unsigned long long>(it != net.received.end() ? it->second.messages : 0));
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// The camera's own input, before anything this frame is drawn against the camera (the names, the hit
// numbers, the speech, the ground decals, the scene): the turn button (the right one, or the wheel button by the
// Input page's choice) turns it with the mouse (a turn begun over the world goes on over the windows), the wheel
// brings it nearer or farther. Whether the mouse is over a window is the interface's word from the last frame (this
// frame's is not out yet). The Game page's camera rows, as the original's: method 1 turns as the cursor moves;
// method 2 hides the cursor and holds it where the button was pressed, turns by how far it was pushed each frame,
// and leaves it there at the release.
void PlayScreen::CameraInput(ScreenContext& context) {
    cameraDt_ = static_cast<float>(context.dt);
    cameraNow_ = context.time;
    if (!shown_ || context.input == nullptr) return;
    const ui::Input& in = *context.input;
    const bool overWindow = gui_.interface() != nullptr && (gui_.context().mouseOverUi() || gui_.interface()->pointer().held);
    const bool blocked = context.mouseBlocked || overWindow || (gui_.interface() != nullptr && gui_.interface()->pickEnded());
    const bool wheelTurns = context.options != nullptr && context.options->input.mouseMode == 1;
    const ui::MouseButton button = wheelTurns ? ui::MouseButton::Middle : ui::MouseButton::Right;
    if (in.Pressed(button) && !blocked) {
        turning_ = true;
        turnFrom_ = in.PressPosition(button);
        turnHeld_ = context.options != nullptr && context.options->game.Get("cameraMethod2");
    }
    if (turning_ && !in.Down(button)) {
        // Method 2's cursor back where the button was pressed.
        if (turnHeld_) {
            context.warpMouse = true;
            context.warpTo = turnFrom_;
        }
        turning_ = false;
        turnHeld_ = false;
    }
    if (turning_) {
        ui::Vec2 d = in.mouseDelta();
        if (turnHeld_) {
            d = {in.mouse().x - turnFrom_.x, in.mouse().y - turnFrom_.y};
            context.hideCursor = true;
            context.warpMouse = true;
            context.warpTo = turnFrom_;
            // Pushed over 5 pixels in a frame: the turn's release opens no player's menu.
            if (!wheelTurns && (std::abs(d.x) > 5.0f || std::abs(d.y) > 5.0f)) targetMenuArmed_ = false;
        }
        // 0.005 radians a pixel both ways, the pitch within the original's +-1.0708 (half pi less 0.5; positive looks
        // down); the quarter view holds its pitch (the original's leaves it alone in view 2).
        cameraYaw_ += d.x * 0.005f;
        if (Sight() != core::Sight::QuarterView) cameraPitch_ = std::clamp(cameraPitch_ + d.y * 0.005f, -kCameraPitchLimit, kCameraPitchLimit);
    }
    // The wheel: 6 units a notch (the original's), from 10 out to 300 (ours: the original's farthest is 150, M-13).
    if (!blocked && in.wheel() != 0.0f) cameraDistance_ = std::clamp(cameraDistance_ - in.wheel() * 6.0f, 10.0f, kCameraFarthest);
}

void PlayScreen::HandleInput(ScreenContext& context) {
    sight_ = context.options != nullptr ? &context.options->sight : nullptr;
    // The Input page's keys, when they changed.
    if (context.options != nullptr && context.options->input.keys != appliedKeys_) {
        appliedKeys_ = context.options->input.keys;
        ApplyKeyOptions(context.options->input, hud_.bindings());
    }
    // The Online page's view, when it changed (in the world only; entering asks for it anyway).
    if (context.options != nullptr && world_ && !arrived_ && !replay_ && shared_ && shared_->session) {
        const std::array<int, 3> want{context.options->online.players, context.options->online.monsters, context.options->online.radius};
        if (want != appliedView_) {
            appliedView_ = want;
            shared_->session->SetViewLimits(std::uint16_t(want[0]), std::uint16_t(want[1]), std::uint16_t(want[2]));
        }
    }
    // The Game page's beginner mark, told to the server when the Option window changes it and it differs from the mark
    // the server keeps (the original's 0x7402 from the window's apply; the server takes it up to level 19 only).
    if (context.options != nullptr && world_ && !replay_ && shared_ && shared_->session) {
        const bool wanted = context.options->game.beginnerMark;
        if (beginnerOption_ != int(wanted)) {
            const bool first = beginnerOption_ < 0;
            beginnerOption_ = int(wanted);
            const auto own = entityMarks_.find(view_.Self());
            const std::uint8_t marks = own != entityMarks_.end() ? own->second : 0;
            if (!first && ((marks & proto::kMarkBeginner) != 0) != wanted) {
                shared_->session->SendGame(proto::SetMarks::kId, proto::Encode(proto::SetMarks{std::uint8_t(marks ^ proto::kMarkBeginner)}));
            }
        }
    }
    // The player's orbs in berserk: all five at its start, one going out each fifth of its time.
    {
        const double left = berserkUntil_ - context.time, part = double(game::kBerserkMs) / 1000.0 / 5.0;
        player_.self.berserkLit = left > 0.0 ? std::uint8_t(std::clamp(int(std::ceil(left / part)), 1, 5)) : 0;
    }
    // The guild master's recall unanswered when its time ran out: its box closes (PlayGuild.cpp).
    if (recallBox_ != nullptr && context.time >= recallUntil_) {
        if (ui::Interface* ui = gui_.interface()) ui->Close(recallBox_);
        recallBox_ = nullptr;
    }
    // A quest instance's entry unanswered when its time ran out: its box closes (the server forgets it: PlayQuest.cpp).
    if (questEnterBox_ != nullptr && context.time >= questEnterUntil_) {
        if (ui::Interface* ui = gui_.interface()) ui->Close(questEnterBox_);
        questEnterBox_ = nullptr;
    }
    // HP or MP falling under 30 % (not to 0): the alarm once at the player, with the Game page's warning row for it and
    // its sound row on (the original's CIFPlayerMiniInfo).
    if (world_ && !replay_ && context.options != nullptr) {
        const auto low = [](std::uint32_t now, std::uint32_t most) { return most != 0 && now != 0 && float(now) / float(most) < 0.3f; };
        const bool hpLow = low(player_.self.hp, player_.self.maxHp), mpLow = low(player_.self.mp, player_.self.maxMp);
        const core::GameOptions& rows = context.options->game;
        const bool alarm = rows.Get("warningSound") && ((hpLow && !hpLow_ && rows.Get("hpWarning")) || (mpLow && !mpLow_ && rows.Get("mpWarning")));
        if (alarm && context.audio != nullptr) {
            if (const game::WorldView::Entity* self = view_.Find(view_.Self())) {
                context.audio->PlayEffect("prim\\snd\\ui\\alarm_sound.wav", world_->OriginRelative(self->position));
            }
        }
        hpLow_ = hpLow;
        mpLow_ = mpLow;
    }
    if (!shown_ || context.input == nullptr) return;
    const ui::Input& in = *context.input;
    // The windows take the mouse where they are (and hold it while a button is down on one).
    const bool overWindow = gui_.interface() != nullptr && (gui_.context().mouseOverUi() || gui_.interface()->pointer().held);
    // A held item let go over the world is a drop, not a walk.
    const bool blocked = context.mouseBlocked || overWindow || (gui_.interface() != nullptr && gui_.interface()->pickEnded());
    // Which window takes a click meant for the world (logged, to find windows that cover it).
    if (overWindow && (in.Pressed(ui::MouseButton::Left) || in.Pressed(ui::MouseButton::Right)) && gui_.interface() != nullptr) {
        if (ui::Widget* w = gui_.interface()->under()) {
            ui::Widget* root = w->Root();
            const ui::Rect r = root->ScreenRect();
            core::LogInfo("interface: the click at {:.0f},{:.0f} went to {} ({}) in window {} at {:.0f},{:.0f} {:.0f}x{:.0f}", in.mouse().x,
                          in.mouse().y, w->node().name, w->node().type, root->node().type, r.x, r.y, r.w, r.h);
        }
    }
    if (blocked) return;
    // A right press on the world readies a player's menu when the Game page's "Activate action short cut" is on.
    if (in.Pressed(ui::MouseButton::Right)) targetMenuArmed_ = context.options != nullptr && context.options->game.Get("actionShortcut");
    PartyRightClick(context, in); // a player's menu (PlayParty.cpp)
    // The button that does not turn the camera uses the underbar's M slot, as the X key does: the wheel button while the
    // right one turns, the right one while the wheel button turns (the original's mouse choice).
    const bool wheelTurns = context.options != nullptr && context.options->input.mouseMode == 1;
    if (in.Pressed(wheelTurns ? ui::MouseButton::Right : ui::MouseButton::Middle) && !leaving_) {
        hud_.UseQuickSlot(game::QuickSlots::kRightButton, context.time, false);
    }
    // A left press on the world: what is there, where it was pressed. On the press, not the
    // release: clicking from place to place the mouse is still moving, and a release a few pixels
    // on used to be taken for a drag and lost (the left button drags nothing on the world). A
    // press while an item hangs on the mouse starts its drop instead.
    const bool holdingItem = gui_.interface() != nullptr && gui_.interface()->picked() != nullptr;
    if (in.Pressed(ui::MouseButton::Left) && !leaving_ && !holdingItem) {
        const ui::Vec2 at = in.PressPosition(ui::MouseButton::Left);
        ClickWorld(context, at.x, at.y, in.ctrl(), in.KeyDown(ui::Key::Alt));
    }
}

// What one click at (x, y) picks: the nearest along the ray of every kind (the original's single pick over them all; it
// orders no kind before another), an item's name under the mouse first. At most one of them set; `mount` with `player`
// when he rides. The click (ClickWorld) and the cursor (PickCursor) both go by it.
PlayScreen::Picked PlayScreen::PickUnder(float x, float y, float width, float height) const {
    Picked p;
    if (!world_ || replay_) return p;
    float itemAt = 1e9f, npcAt = 1e9f, monsterAt = 1e9f, playerAt = 1e9f, cosAt = 1e9f;
    bool npcCentre = false, monsterCentre = false, playerCentre = false, cosCentre = false;
    std::uint32_t item = GroundUnder(x, y, width, height, &itemAt);
    std::uint32_t npc = NpcUnder(x, y, width, height, &npcAt, &npcCentre);
    std::uint32_t nearestMonster = MonsterUnder(x, y, width, height, &monsterAt, &monsterCentre);
    std::uint32_t nearestPlayer = PlayerUnder(x, y, width, height, &playerAt, &playerCentre);
    std::uint32_t nearestCos = CosUnder(x, y, width, height, &cosAt, &cosCentre);
    {
        // The cursor's own ray's hit beats the side rays' across the kinds too (the original casts the nine over every
        // character at once).
        if (npcCentre || monsterCentre || playerCentre || cosCentre) {
            if (!npcCentre) npc = 0;
            if (!monsterCentre) nearestMonster = 0;
            if (!playerCentre) nearestPlayer = 0;
            if (!cosCentre) nearestCos = 0;
        }
        const float nearest = std::min({item != 0 ? itemAt : 1e9f, npc != 0 ? npcAt : 1e9f, nearestMonster != 0 ? monsterAt : 1e9f,
                                        nearestPlayer != 0 ? playerAt : 1e9f, nearestCos != 0 ? cosAt : 1e9f});
        if (item != 0 && itemAt > nearest) item = 0;
        if (npc != 0 && npcAt > nearest) npc = 0;
        if (nearestMonster != 0 && monsterAt > nearest) nearestMonster = 0;
        if (nearestPlayer != 0 && playerAt > nearest) nearestPlayer = 0;
        if (nearestCos != 0 && cosAt > nearest) nearestCos = 0;
        if (item != 0) npc = nearestMonster = nearestPlayer = nearestCos = 0; // a tie: the item, the NPC, the monster, the player, a COS
        if (npc != 0) nearestMonster = nearestPlayer = nearestCos = 0;
        if (nearestMonster != 0) nearestPlayer = nearestCos = 0;
        if (nearestPlayer != 0) nearestCos = 0;
    }
    // A rider, or the ride under him: his mount is the one picked (the original's), the click the rider's (who may be
    // attacked is his question; the server's hit on a rider lands on his mount).
    std::uint32_t mount = 0;
    if (nearestPlayer != 0) {
        if (const auto r = riders_.find(nearestPlayer); r != riders_.end() && view_.Find(r->second) != nullptr) mount = r->second;
    } else if (nearestCos != 0) {
        for (const auto& [rider, ride] : riders_) {
            if (ride == nearestCos && rider != view_.Self() && view_.Find(rider) != nullptr) {
                nearestPlayer = rider;
                mount = ride;
                nearestCos = 0;
                break;
            }
        }
    }
    p.item = item;
    p.npc = npc;
    p.monster = nearestMonster;
    p.player = nearestPlayer;
    p.cos = nearestCos;
    p.mount = mount;
    return p;
}

// A left click at (x, y) on the world: an item, an NPC, a monster, a gate or the ground there.
void PlayScreen::ClickWorld(ScreenContext& context, float x, float y, bool ctrl, bool alt) {
    // A click in the world while the logout counts stops it first (the only sender of 0x7006).
    if (logoutSeconds_ > 0 && !replay_ && shared_ && shared_->session) shared_->session->CancelLogout();
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    // A manual's zone markers first (a marker under the cursor uses the manual; elsewhere they go).
    if (!replay_ && ClickZoneMarker(x, y, width, height)) return;
    const ui::Vec2 up{x, y};
    proto::WorldPosition target;
    bool sky = false;
    const bool ground = !replay_ && world_->Pick(up.x, up.y, width, height, target, true, &sky);
    // What is under the cursor (PickUnder).
    const Picked picked = PickUnder(up.x, up.y, width, height);
    std::uint32_t item = picked.item, npc = picked.npc, nearestMonster = picked.monster, nearestPlayer = picked.player,
                  nearestCos = picked.cos;
    const std::uint32_t mount = picked.mount;
    if (nearestCos != 0) {
        // A COS: selected, its target window (no walk, as a player's click); a job foe's transport or pet clicked again
        // (or with Ctrl) attacked, as its owner would be.
        const bool again = player_.target.entity == nearestCos || ctrl;
        SelectCos(nearestCos);
        if (const game::WorldView::Entity* c = view_.Find(nearestCos); again && c != nullptr && JobFoe(c->amount) && actionsHeld_ < 2 && !replay_) {
            shared_->session->Attack(nearestCos);
            actionSentAt_ = context.time;
        }
        pendingNpc_ = 0;
        pendingGate_ = 0;
        lastClick_ = "a cos";
        autoRun_ = false;
        heldMove_.reset();
        route_ = {};
        return;
    }
    autoRun_ = false; // a click of its own ends auto run
    heldMove_.reset(); // and a walk held for the swing's end
    route_ = {};       // and a way under way
    const std::uint32_t monster = nearestMonster;
    const std::uint32_t other = nearestPlayer;
    if (item != 0) {
        // An item on the ground: asked for at once; the server walks the character there and picks it up on arrival
        // (as the original's does), behind a swing or a cast running. Two commands held: not sent.
        pendingNpc_ = 0;
        pendingGate_ = 0;
        // Kept for another (a drop for its killer's first 30 s, a quest item for good), unless both are of a party
        // sharing items: refused at once with the original's words (0x1812), nothing sent.
        const game::WorldView::Entity* lying = view_.Find(item);
        const bool shared = player_.party.InParty() && (player_.party.settings & game::kPartyItemShare) != 0 && lying != nullptr &&
                            player_.party.MemberByEntity(lying->owner) != nullptr;
        if (lying != nullptr && lying->owner != 0 && lying->owner != view_.Self() && !shared) {
            player_.itemRefusals.push_back(proto::ItemStatus::NotYours);
        } else if (actionsHeld_ < 2) {
            if (StandingNear(item, 15.0f)) FaceToward(item); // picked up from where it stands (the server's reach)
            shared_->session->PickUp(item);
        }
    }
    const assets::TeleportGate* gate = replay_ || npc != 0 || monster != 0 || other != 0 ? nullptr : GateUnder(up.x, up.y, width, height, ground ? &target : nullptr);
    lastClick_ = item != 0 ? "an item" : monster != 0 ? "a monster" : npc != 0 ? "an NPC" : other != 0 ? "a player" : gate != nullptr ? "a gate" : ground ? "the ground" : "nothing";
    if (other != 0) {
        ClickPlayer(context, other, ctrl, alt, mount); // PlayPk.cpp
        return;
    }
    if (item != 0) {
    } else if (monster != 0) {
        // A monster: selected; the selected one clicked again attacked (the server walks the
        // character into reach and swings until it dies).
        const bool again = player_.target.entity == monster || ctrl; // Ctrl: select and attack at once
        SelectMonster(monster);
        pendingNpc_ = 0;
        pendingGate_ = 0;
        if (again) {
            const game::WorldView::Entity* it = view_.Find(monster);
            // Two commands held: not sent.
            if (it != nullptr && !IsDead(*it) && actionsHeld_ < 2) {
                shared_->session->Attack(monster);
                actionSentAt_ = context.time;
            }
        }
    } else if (npc != 0) {
        // An NPC: talked to from where the character stands when near enough, else after a
        // walk toward it (as a gate).
        pendingNpc_ = npc;
        pendingUntil_ = context.time + 8.0;
        pendingGate_ = 0;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        const game::WorldView::Entity* it = view_.Find(npc);
        if (self != nullptr && it != nullptr) {
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(it->position);
            const double dx = a.x - b.x, dz = a.z - b.z, length = std::max(1.0, std::hypot(dx, dz));
            // The original's: a random stop 20..240 from it, so players do not all stand on one spot.
            const double walkTo = UsedNear(npc) ? double(UseWalkTo(npc, kNpcReach)) : double(WalkUpDistance(kNpcWalkNearest, kNpcReach));
            // Far: the ground click's walk (its stop, kept through a swing), the talk on arrival; near: at once.
            if (length > UseReach(npc, kNpcReach)) {
                if (ClickWalk(context, proto::FromFlat({b.x + dx / length * walkTo, b.z + dz / length * walkTo}, it->position.y), false)) {
                    pendingUntil_ = heldUntil_ + 8.0;
                }
            }
        }
    } else if (gate != nullptr) {
        pendingNpc_ = 0;
        // On a gate: its menu from where the character stands when near enough, else a walk
        // toward it first.
        pendingGate_ = gate->refObjId;
        gateTries_ = 0;
        talkAtGate_ = true;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (self != nullptr && GateNear(self->position, kGateReach) != gate) {
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat({gate->region, gate->x, gate->y, gate->z});
            const double dx = a.x - b.x, dz = a.z - b.z, length = std::max(1.0, std::hypot(dx, dz));
            const double walkTo = WalkUpDistance(kGateWalkNearest, kGateWalkFarthest);
            ClickWalk(context, proto::FromFlat({b.x + dx / length * walkTo, b.z + dz / length * walkTo}, gate->y), false); // as an NPC's
        }
    } else if (ground && fortressPick_ && !sky) {
        FortressPicked(target); // a fortress item's ground point (a flag, a guard, a tablet's monster), not a walk
    } else if (ground) {
        pendingGate_ = 0;
        pendingNpc_ = 0;
        ClickWalk(context, target, sky);

        moveMarker_ = {target, context.time};
    } else {
        core::LogInfo("game: a click at {:.0f},{:.0f} found nothing to go to", x, y);
    }
}

bool PlayScreen::ClickWalk(ScreenContext& context, const proto::WorldPosition& to, bool sky) {
    // One command held: a stop first (its attack ends now, a cast gone off runs on); two: none, the waiting one goes
    // before the walk.
    if (actionsHeld_ == 1 && !replay_) shared_->session->StopAction();
    if (OwnMoveHeld(context.time) || context.time < actionSentAt_ + 0.5) {
        if (!heldMove_) heldSince_ = context.time;
        heldUntil_ = context.time + 5.0;
        heldMove_ = to; // sent once that swing ends (Update); the walk then ends the attack on the server
        heldSky_ = sky;
        heldSkill_ = 0;
        return true;
    }
    WalkTo(context, to, sky);
    return false;
}

void PlayScreen::WalkTo(ScreenContext& context, const proto::WorldPosition& to, bool sky) {
    const bool pathfinding = context.options != nullptr && context.options->beta.movement == core::BetaOptions::Movement::Pathfinding;
    if (pathfinding && !sky && world_ && !replay_) {
        RouteTo(to, 0, 0.0f, true);
        return;
    }
    route_ = {};
    // The sky: that way, on and on till something stops the walker (the original's; the server walks it leg by leg).
    shared_->session->MoveTo(to, sky);
}

double PlayScreen::HeldUntil(double now) const {
    double until = now;
    const std::uint32_t self = view_.Self();
    if (const auto m = motions_.find(self); m != motions_.end() && m->second.locks) until = std::max(until, m->second.End());
    if (const auto k = knocked_.find(self); k != knocked_.end()) until = std::max(until, k->second.until);
    return until;
}

bool PlayScreen::OwnMoveHeld(double now) const {
    const std::uint32_t self = view_.Self();
    if (const auto m = motions_.find(self); m != motions_.end() && m->second.locks && now < m->second.End()) return true;
    if (const auto k = knocked_.find(self); k != knocked_.end() && now < k->second.until) return true;
    return false;
}

void PlayScreen::Interface(ScreenContext& context, FrameSnapshot& snapshot) {
    const ui::Vec2 screen{float(context.device.SwapchainWidth()), float(context.device.SwapchainHeight())};
    SamplePerformance(context);
    hudContext_.performance = &performance_;
#ifdef SR_BUG_REPORTS
    // The bug report window's Send (PlayBugReport.cpp); none in a replay, or while one is still going out.
    if (!replay_ && !bugReport_) {
        hudContext_.bugReport = [this](BugReportForm form) { StartBugReport(std::move(form)); };
    } else {
        hudContext_.bugReport = nullptr;
    }
#endif
    hudContext_.state = &player_;
    hudContext_.guild = &guild_;
    hudContext_.community = &community_;
    hudContext_.communityRequest = [this](game::proto::MessageId id, std::vector<std::byte> payload) {
        if (!replay_) shared_->session->SendGame(id, payload);
    };
    hudContext_.guildRequest = [this](proto::MessageId id, std::vector<std::byte> payload) {
        if (!replay_) shared_->session->SendGame(id, payload);
    };
    hudContext_.academyRequest = hudContext_.guildRequest;
    hudContext_.openMarketRequest = hudContext_.guildRequest;
    hudContext_.fortress = &fortress_;
    hudContext_.guildCrest = [this](std::uint32_t guild, std::uint32_t revision) { return CrestTexture(proto::kCrestGuild, guild, revision); };
    hudContext_.keyedPicture = [this, files = &context.files](const std::string& source, const std::vector<std::uint32_t>& keys, std::uint32_t colour) {
        return madeFiles_ != nullptr ? KeyedPicture(*files, *madeFiles_, source, keys, colour) : std::string();
    };
    hudContext_.npcTax = &npcTax_;
    hudContext_.npcSafe = &npcSafe_;
    SyncSafeTradeIcon(context.time); // put back after a world entry cleared the board
    hudContext_.fortressRequest = hudContext_.guildRequest;
    hudContext_.exchange = &exchange_;
    hudContext_.exchangeRequest = hudContext_.guildRequest;
    hudContext_.exchangeConfirm = [this] { ExchangeConfirm(); };
    hudContext_.alchemyRequest = hudContext_.guildRequest;
    hudContext_.cosCommand = [this](int command, std::uint32_t entity) { CosCommand(command, entity); }; // PlayCos.cpp
    hudContext_.cosRequest = [this](proto::MessageId id, std::vector<std::byte> payload) {
        if (!replay_) shared_->session->SendGame(id, std::move(payload));
    };
    hudContext_.freePvpRequest = [this](std::uint8_t team) {
        if (!replay_) shared_->session->SendGame(proto::FreePvp::kId, proto::Encode(proto::FreePvp{team}));
    };
    // Stalls and the network (PlayStall.cpp): the network's classes and the towns read once.
    if (!stallTablesRead_) {
        stallTablesRead_ = true;
        std::string why;
        if (!stallTables_.Load(context.files, why) || !towns_.Load(context.files, why)) core::LogWarning("game: stall network: {}", why);
    }
    hudContext_.stall = &stall_;
    hudContext_.stallNetwork = &stallTables_;
    hudContext_.stallRequest = hudContext_.guildRequest;
    hudContext_.quests = &quests_;
    hudContext_.questRequest = hudContext_.guildRequest;
    hudContext_.instanceRequest = hudContext_.guildRequest;
    hudContext_.jobRequest = hudContext_.guildRequest;
    hudContext_.flag = &flag_;
    hudContext_.eventRequest = hudContext_.guildRequest;
    hudContext_.flagLine = [this](const game::FlagView::Line& line) { FlagLine(line, hudContext_.time); };
    hudContext_.arena = &arena_;
    hudContext_.arenaLine = [this](const game::ArenaView::Line& line) { ArenaLine(line, hudContext_.time); };
    hudContext_.popRequest = hudContext_.guildRequest;
    SetupMall(context);
    if (!gachaRead_) {
        gachaRead_ = true;
        std::string why;
        if (!gacha_.Load(context.files, why)) core::LogWarning("game: Magic POP: {}", why);
    }
    hudContext_.gacha = &gacha_;
    hudContext_.tradeRequest = hudContext_.guildRequest;
    hudContext_.serviceRequest = hudContext_.guildRequest; // the item mall's services (HudServices.cpp)
    // The price board's trade centres (specialnpcdata.txt: RefObjID and codename a line), read once.
    if (hudContext_.tradeCentres.empty()) {
        if (const auto file = context.files.Read("server_dep\\silkroad\\textdata\\specialnpcdata.txt")) {
            const std::string text = core::DecodeTextFile(file->Bytes());
            for (std::size_t at = 0; at < text.size();) {
                const std::size_t end = std::min(text.find('\n', at), text.size());
                const std::uint32_t id = std::uint32_t(std::strtoul(text.c_str() + at, nullptr, 10));
                if (id != 0) hudContext_.tradeCentres.push_back(id);
                at = end + 1;
            }
        }
    }
    hudContext_.book = &book_;
    hudContext_.summonList = &summonList_;
    hudContext_.questNpc = [this](std::uint32_t entity) { return QuestMarkOf(entity).has_value(); };
    hudContext_.systemLine = [this](const std::string& line) { player_.Post(game::GameMessage::Kind::General, line, lastTime_); };
    hudContext_.townName = [this](int town) {
        const auto& areas = towns_.Areas();
        if (town < 0 || std::size_t(town) >= areas.size()) return std::string();
        // The area's music name is Korean (regioninfo.txt); the zone's text by its region is the HUD's own.
        return areas[std::size_t(town)].name;
    };
    hudContext_.actions = gui_.data().Actions();
    hudContext_.worldMap = gui_.data().WorldMap();
    // Where the player stands in a dungeon: the block of its floor's floor and room (the minimap's tiles and
    // names, the world map's floor).
    hudContext_.dungeon = {};
    if (world_ && world_->InDungeon() && world_->DungeonBlock() >= 0) {
        if (const assets::DungeonData* d = world_->Dungeon(); d != nullptr && std::size_t(world_->DungeonBlock()) < d->blocks.size()) {
            const assets::DungeonData::Block& block = d->blocks[std::size_t(world_->DungeonBlock())];
            hudContext_.dungeon.region = world_->DungeonRegion();
            hudContext_.dungeon.folder = gui_.data().DungeonFolder(world_->DungeonRegion());
            if (block.floor >= 0 && std::size_t(block.floor) < d->floorNames.size()) hudContext_.dungeon.floor = d->floorNames[std::size_t(block.floor)];
            if (block.room >= 0 && std::size_t(block.room) < d->roomNames.size()) hudContext_.dungeon.room = d->roomNames[std::size_t(block.room)];
        }
    }
    hudContext_.useAction = [this, &context](std::uint32_t action) { UseAction(context, action); };
    hudContext_.playAtPlayer = [this, &context](const std::string& path) {
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (context.audio != nullptr && world_ && self != nullptr) context.audio->PlayEffect(path, world_->OriginRelative(self->position));
    };
    hudContext_.layout2dt = [this](const std::string& path) { return gui_.data().Layout2dt(path); };
    hudContext_.gameGuide = [this] { return gui_.data().GameGuide(); };
    hudContext_.premiumUse = [this](std::uint32_t item) { UsePremium(item); };
    hudContext_.world = &view_;
    hudContext_.hud = &hud_;
    hudContext_.time = context.time;
    hudContext_.text = [this](const std::string& key) { return gui_.data().Text(key); };
    hudContext_.hasText = [this](const std::string& key) { return gui_.data().HasText(key); };
    hudContext_.regionKey = [this](std::uint16_t region) { return gui_.data().RegionKey(region); };
    hudContext_.words = tables_ && tables_->Usable() ? &tables_->rules.names : nullptr;
    hudContext_.items = tables_ && tables_->Usable() ? &tables_->items : nullptr;
    hudContext_.characters = tables_ && tables_->Usable() ? &tables_->characters : nullptr;
    hudContext_.creation = tables_ && tables_->Usable() ? &tables_->rules.creation : nullptr;
    hudContext_.magicOptions = gui_.data().MagicOptions();
    hudContext_.spirits = gui_.data().Spirits();
    hudContext_.magicOptGroups = gui_.data().MagicOptGroups();
    hudContext_.magicOptionAssign = gui_.data().MagicOptionAssign();
    hudContext_.skills = tables_ && tables_->Usable() ? &tables_->skills : nullptr;
    hudContext_.masteryCosts = tables_ && tables_->Usable() ? &tables_->masteryCosts : nullptr;
    hudContext_.levelGold = tables_ && tables_->Usable() ? &tables_->levelGold : nullptr;
    hudContext_.jobExp = tables_ && tables_->Usable() ? &tables_->jobExp : nullptr;
    hudContext_.options = context.options;
    hudContext_.view = haveViewAnswer_ ? &viewAnswer_ : nullptr;
    hudContext_.optionsChanged = [&context] { context.optionsChanged = true; };
    hudContext_.audioPreview = [&context](const core::AudioOptions& levels) {
        if (context.audio != nullptr) context.audio->SetLevels(levels);
    };
    hudContext_.teleports = tables_ && tables_->Usable() ? &tables_->teleports : nullptr;
    hudContext_.collection = tables_ && tables_->Usable() ? &tables_->collection : nullptr;
    hudContext_.shops = tables_ && tables_->Usable() ? &tables_->shops : nullptr;
    hudContext_.npcGreeting = [this](const std::string& codename) { return gui_.data().NpcGreeting(codename); };
    hudContext_.npcTalk = [this](const std::string& codename) { return gui_.data().NpcTalk(codename); };
    // The underbar's shortcuts: an item's icon (what is in that place now).
    hudContext_.slotText = [this](const game::QuickSlot& q) {
        return q.kind == game::QuickSlot::Kind::Action ? gui_.data().Text(ActionSlotLook(q.value).second) : std::string();
    };
    hudContext_.slotIcon = [this](const game::QuickSlot& q) {
        if (q.kind == game::QuickSlot::Kind::Action) return ActionSlotLook(q.value).first;
        if (q.kind == game::QuickSlot::Kind::Skill) {
            const assets::SkillRow* s = tables_ && tables_->Usable() ? tables_->skills.Find(q.value) : nullptr;
            return s != nullptr && !s->iconPath.empty() ? "icon\\" + s->iconPath : std::string();
        }
        if (q.kind != game::QuickSlot::Kind::Item && q.kind != game::QuickSlot::Kind::Equipped) return std::string();
        const game::ItemInstance* item = player_.inventory.At(std::uint8_t(q.value));
        const assets::ItemRow* row = item != nullptr && tables_ && tables_->Usable() ? tables_->items.Find(item->refId) : nullptr;
        if (row == nullptr) return std::string();
        return row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
    };
    // A shortcut's wait: a skill's (its group's, its cool-down group's), an item's; the half-second
    // ready flash after it.
    const auto lookOf = [this](const game::PlayerState::Cooling* found) {
        HudContext::Cooling look;
        if (found == nullptr) return look;
        const double now = hudContext_.time, total = std::max(1e-3, found->end - found->start);
        if (now < found->end) {
            look.cooling = float((found->end - now) / total);
            look.seconds = found->end - now;
        } else if (now < found->end + 0.5) {
            look.charge = float(1.0 - (now - found->end) / 0.5);
        }
        return look;
    };
    hudContext_.itemCooling = [this, lookOf](std::uint32_t refId) {
        // Its own wait, or its kind's (every HP potion waits with the one drunk: game::ReuseLockOf).
        const game::PlayerState::Cooling* found = nullptr;
        if (const auto it = player_.itemCooling.find(refId); it != player_.itemCooling.end()) found = &it->second;
        if (const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(refId) : nullptr) {
            const game::ReuseLock lock = game::ReuseLockOf(*row, player_.self.race == 1);
            const auto it = lock.bit != 0 ? player_.itemCooling.find(game::PlayerState::KindCoolingKey(lock.bit)) : player_.itemCooling.end();
            if (it != player_.itemCooling.end() && (found == nullptr || it->second.end > found->end)) found = &it->second;
            const auto group = row->cooldownGroup != 0 ? player_.itemCooling.find(game::PlayerState::GroupCoolingKey(std::uint32_t(row->cooldownGroup)))
                                                       : player_.itemCooling.end();
            if (group != player_.itemCooling.end() && (found == nullptr || group->second.end > found->end)) found = &group->second;
        }
        return lookOf(found);
    };
    hudContext_.skillCooling = [this, lookOf](std::uint32_t skill) {
        const game::PlayerState::Cooling* found = nullptr;
        const auto take = [&](const std::unordered_map<std::uint32_t, game::PlayerState::Cooling>& map, std::uint32_t key) {
            if (const auto it = map.find(key); it != map.end() && (found == nullptr || it->second.end > found->end)) found = &it->second;
        };
        if (const assets::SkillRow* s = tables_ && tables_->Usable() ? tables_->skills.Find(skill) : nullptr) {
            take(player_.skillCooling, s->group);
            if (s->cooldownGroup != 0) take(player_.groupCooling, std::uint32_t(s->cooldownGroup));
        }
        return lookOf(found);
    };
    hudContext_.slotCooling = [this](const game::QuickSlot& q) {
        if (q.kind == game::QuickSlot::Kind::Skill) return hudContext_.skillCooling(q.value);
        if (q.kind == game::QuickSlot::Kind::Item) {
            if (const game::ItemInstance* item = player_.inventory.At(std::uint8_t(q.value))) return hudContext_.itemCooling(item->refId);
        }
        return HudContext::Cooling{};
    };
    // The buff board: a skill's icon and name (skilldata columns 61, 62).
    hudContext_.effectIcon = [this](const game::ActiveEffect& e) {
        // A time service's entry (PlayerState::ShowTimedServices): its item's icon.
        if (e.kind == game::ActiveEffect::Kind::TimedJob && e.icon.empty() && tables_ && tables_->Usable()) {
            const assets::ItemRow* r = tables_->items.Find(e.id);
            return r != nullptr && !r->iconPath.empty() ? "icon\\" + r->iconPath : std::string();
        }
        if (e.kind == game::ActiveEffect::Kind::TimedJob) return e.icon;
        const assets::SkillRow* s = tables_ && tables_->Usable() ? tables_->skills.Find(e.id) : nullptr;
        return s != nullptr && !s->iconPath.empty() ? "icon\\" + s->iconPath : std::string();
    };
    hudContext_.effectName = [this](const game::ActiveEffect& e) {
        if (e.kind == game::ActiveEffect::Kind::TimedJob && e.subject.empty() && tables_ && tables_->Usable()) {
            const assets::ItemRow* r = tables_->items.Find(e.id);
            if (r == nullptr) return std::string();
            std::string tip = gui_.data().Text(r->nameKey);
            // Its description when the text tables have one (the premium's gold time ticket, ITEM_PRE_GOLTAM_4W, has a key
            // and no text: the user's report 2026-11-14 showed the bare key), its SML2 markup laid out as lines.
            if (!r->descKey.empty() && hudContext_.hasText && hudContext_.hasText(r->descKey)) {
                std::vector<ui::TooltipLine> lines;
                AddTooltipDescription(lines, gui_.data().Text(r->descKey));
                for (const ui::TooltipLine& l : lines) {
                    // The text tables' "\n" before a <br> would make a second break: one is enough.
                    std::string text = l.text;
                    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
                    if (text.size() >= 2 && text.ends_with("\\n")) text.resize(text.size() - 2);
                    tip += "\n" + text;
                }
            }
            // What the service has left (ours: the original's premium list window shows its entries, its lines not traced).
            const std::uint32_t category = e.token & 0xFFFFu;
            if ((e.token & 0xFFFF0000u) == game::PlayerState::kTimedServiceToken) {
                for (const auto& s : player_.timedServices.services) {
                    if (s.item != e.id || s.category != category) continue;
                    if (category == 3 || category == 9) {
                        const std::uint32_t bonus = (s.bonus >> 16) + (s.bonus & 0xFFFFu);
                        tip += std::format("\nBonus time: {}{} {}{}", bonus / 3600, gui_.data().Text("PARAM_HOUR"), bonus / 60 % 60,
                                           gui_.data().Text("PARAM_MINUTE"));
                        if (s.paused != 0) tip += "\n" + gui_.data().Text("UIIT_MSG_GOLDTIME_PAUSE_START");
                    }
                }
                if (category == 10) {
                    for (const auto& u : player_.timedServices.premium) {
                        const assets::ItemRow* part = tables_->items.Find(u.item);
                        if (part != nullptr) tip += std::format("\n{} {} / {}", gui_.data().Text(part->nameKey), u.left, u.perDay);
                    }
                }
            }
            return tip;
        }
        if (e.kind == game::ActiveEffect::Kind::TimedJob) {
            return e.text.empty() ? gui_.data().Text(e.subject) : gui_.data().Text(e.subject) + "\n" + gui_.data().Text(e.text);
        }
        const assets::SkillRow* s = tables_ && tables_->Usable() ? tables_->skills.Find(e.id) : nullptr;
        return s != nullptr ? gui_.data().Text(s->nameKey) : std::string();
    };
    hudContext_.wearer.level = player_.self.level;
    hudContext_.wearer.country = player_.self.race;
    if (tables_ && tables_->Usable()) {
        // The figure as seen now (a skin change's new sex: EntityLook), else the one it entered with (a monster's shape is
        // no figure: the sex stays).
        const game::WorldView::Entity* me = view_.Find(view_.Self());
        auto figure = me != nullptr ? tables_->rules.creation.FigureOf(me->look.model) : std::nullopt;
        if (!figure) figure = tables_->rules.creation.FigureOf(player_.self.model);
        if (figure) hudContext_.wearer.male = figure->male;
    }
    hud_.Bind(&player_);
    // The race mark: the figure's race in the creation rules.
    if (tables_ && tables_->Usable() && player_.self.entity != 0) {
        if (const auto figure = tables_->rules.creation.FigureOf(player_.self.model)) {
            player_.self.race = figure->race == game::CreationRules::Race::Europe ? 1 : 0;
        }
    }
    if (!player_.HasLevels() && tables_ && tables_->Usable()) player_.SetLevels(tables_->levels);
    ui::Interface* ui = gui_.Prepare([this](ui::Interface& made) { RegisterHudWindows(made, hudContext_); });
    if (ui == nullptr) {
        context.holdCapture = true; // the interface's data is still loading
        return;
    }
    if (!hud_.Created()) {
        hud_.Create(*ui, screen);
        hudScreen_ = screen;
        // COS told before the HUD was made (pets coming back at the entry, behind the loading screen): their icons now
        // (the user's report 2026-11-14: after a relog the pets' frames beside the minimap came only with a new summon).
        SyncCosHud();
        // The interface's clicks and windows heard (O3).
        if (GameAudio* audio = context.audio) ui->onSound = [audio](std::string_view handle) { audio->PlayUi(handle); };
    } else if (screen.x != hudScreen_.x || screen.y != hudScreen_.y) {
        hud_.Place(screen); // a new screen size: the start places again
        hudScreen_ = screen;
    }
    if (gateMenu_) {
        if (auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc))) npc->ShowGate(gateMenu_->gate, gateMenu_->services, gateMenu_->opened);
        gateMenu_.reset();
    }
    if (goldBox_) {
        // --gold-box: the storage's gold box up, as at a keeper (captures).
        goldBox_ = false;
        player_.storageOpen = true;
        hud_.StorageGold(true);
    }
    if (shopShow_ != 0) {
        const game::WorldView::Entity* e = view_.Find(shopShow_);
        const assets::ObjectRow* row = e != nullptr && tables_ && tables_->Usable() ? tables_->characters.Find(e->look.model) : nullptr;
        const assets::NpcShop* shop = row != nullptr ? tables_->shops.Find(row->codename) : nullptr;
        if (shop != nullptr) {
            if (auto* store = dynamic_cast<StoreWindow*>(hud_.Show(hud::kStore))) store->Open(shopShow_, *shop, shopGroup_, gui_.data().Text(row->nameKey));
            // A specialty NPC or the thieves' buyer: the transport's bag beside it, not the inventory.
            const bool specialty = game::IsSpecialtyNpc(row->codename) || game::IsStolenGoodsBuyer(row->codename);
            hudContext_.tradeNpc = specialty ? shopShow_ : 0;
            hudContext_.tradeStolenBuyer = game::IsStolenGoodsBuyer(row->codename);
            if (!specialty) hudContext_.tradePrices.clear();
            const HudContext::CosShown* cart = nullptr;
            for (const HudContext::CosShown& c : hudContext_.coses) {
                if (c.kind == std::uint8_t(game::CosKind::Transport)) cart = &c;
            }
            if (specialty && cart != nullptr) {
                hudContext_.cosSelected = cart->entity;
                hudContext_.cosTab = 1;
                ui::Widget* bag = hud_.Show(120); // the COS window (CIFCOS), beside the store as the inventory would be
                const ui::Widget* store = hud_.Shown(hud::kStore) ? hud_.Show(hud::kStore) : nullptr;
                if (bag != nullptr && store != nullptr) {
                    bag->rect.x = store->rect.x + store->rect.w;
                    bag->rect.y = store->rect.y;
                }
            } else {
                hud_.OpenPage(hud::kPageInventory);
            }
        }
        shopShow_ = 0;
    }
    // A store or storage window closed (its button, Escape): put away.
    if (player_.shopNpc != 0 && !hud_.Shown(hud::kStore)) player_.shopNpc = 0;
    if (player_.shopNpc == 0 && hudContext_.tradeNpc != 0) {
        // A specialty shop put away: the server decides the safe trade (the original's 0x704B).
        if (!replay_ && shared_ && shared_->session) shared_->session->CloseShop(hudContext_.tradeNpc);
        hudContext_.tradeNpc = 0;
        hudContext_.tradePrices.clear();
    }
    if (player_.storageOpen && !hud_.Shown(hud::kStorage)) player_.storageOpen = false;
    // The guild's storage put away (its window closed): freed for the other members.
    if (player_.guildStorageOpen && !hud_.Shown(hud::kGuildStorage)) player_.guildStorageOpen = false;
    if (guildStorageHeld_ && !player_.guildStorageOpen) {
        guildStorageHeld_ = false;
        if (!replay_) shared_->session->CloseGuildStorage();
    }
    if (guildStorageShow_) {
        guildStorageShow_ = false;
        guildStorageHeld_ = true;
        hud_.Show(hud::kGuildStorage);
        hud_.OpenPage(hud::kPageInventory);
    }
    hudContext_.shopPrices = player_.shopNpc != 0;
    if (storageShow_) {
        storageShow_ = false;
        hud_.Show(hud::kStorage);
        hud_.OpenPage(hud::kPageInventory);
    }
    if (npcMenu_) {
        const game::WorldView::Entity* e = view_.Find(npcMenu_->entity);
        const assets::ObjectRow* row = e != nullptr && tables_ && tables_->Usable() ? tables_->characters.Find(e->look.model) : nullptr;
        if (row != nullptr) {
            // A custom NPC the server names (ours: the beta quests' Beta Helper, game/BetaQuests.h): its codename the name's.
            const bool custom = !e->look.name.empty();
            const std::string codename = custom ? game::CustomNpcCodename(e->look.name) : row->codename;
            if (auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc)))
                npc->ShowNpc(npcMenu_->entity, codename, custom ? e->look.name : row->nameKey, npcMenu_->services);
        }
        npcMenu_.reset();
    }
    LogoutTick(context.time);
    TakeSpeech(context.time);
    // The quick window (1.188's CNIFMiniQuestWnd) once the player has quests and its layout is read.
    if (!quests_.Active().empty() && !hud_.Shown(hud::kQuestQuick)) ShowQuestQuick(hudContext_);
    ui::Input input = *context.input;
    if (mouse_.x >= 0.0f) {
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = mouse_;
        input.Feed(move);
    }
    gui_.Frame(context, snapshot, input, 1.0f, [&](ui::Painter& paint) {
        // A gap under the mouse: its tooltip.
        if (hoveredGap_ != 0 && gui_.interface() != nullptr && !gui_.context().mouseOverUi()) {
            std::vector<ui::TooltipLine> lines{{""}, {gui_.data().Text("UIIT_STT_GOD_TELTOPRT_GATE_1")}, {gui_.data().Text("UIIT_STT_GOD_TELTOPRT_GATE_2")}};
            for (const std::string& line : GapLines(hoveredGap_)) lines.push_back({line});
            lines.back().color = ui::Argb(0xFFD6FF7E);
            const ui::Vec2 p = gui_.interface()->pointer().position;
            gui_.interface()->HoldTooltip(std::move(lines), {p.x, p.y, 1.0f, 1.0f});
        }
        if (!scene_.active) { // a quest scene shows no names, numbers nor speech
            DrawNames(context, paint);
            DrawDamage(context, paint);
            DrawSpeech(context, paint);
        }
        DrawQuestSceneBars(context, paint);
    }, !scene_.active); // a quest scene shows no interface (its cinema's bars alone)
    hud_.Keys(gui_.context(), context.time);
    GateScript(context.time);
    HudRequests(context);
    hud_.Sync();
    PartyFrame(context); // the party's lines and boxes (PlayParty.cpp)
    if (!gui_.Settled()) context.holdCapture = true;
}

void PlayScreen::GateScript(double now) {
    // One step a second once the menu is open: a choice by its number, or the message box's answer.
    if (gateScript_.empty()) return;
    if (gateScriptAt_ == 0) {
        if (!hud_.Shown(hud::kNpc)) return;
        gateScriptAt_ = now + 1.0;
    }
    if (now < gateScriptAt_) return;
    gateScriptAt_ = now + 1.0;
    const std::string step = gateScript_.front();
    gateScript_.erase(gateScript_.begin());
    if (step == "y" || step == "n") {
        hud_.AnswerConfirm(step == "y");
    } else if (auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() ? hud_.interface()->Window(hud::kNpc) : nullptr)) {
        npc->Choose(std::atoi(step.c_str()));
    }
}

std::uint8_t PlayScreen::ReverseScrollPlace() const {
    if (!tables_ || !tables_->Usable()) return 0;
    for (const auto& [place, item] : player_.inventory.places) {
        const assets::ItemRow* row = place >= game::slot::kWorn ? tables_->items.Find(item.refId) : nullptr;
        if (row != nullptr && row->typeId == std::array<int, 4>{3, 3, 3, 3} && row->codename.find("REVERSE_RETURN") != std::string::npos) return place;
    }
    return 0;
}

bool PlayScreen::UseReverseReturn(std::uint8_t place) {
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr && tables_ && tables_->Usable() ? tables_->items.Find(item->refId) : nullptr;
    // The client knows it by its TID alone; ours also by its name (a quest's ferry ticket shares it).
    if (row == nullptr || row->typeId != std::array<int, 4>{3, 3, 3, 3} || row->codename.find("REVERSE_RETURN") == std::string::npos) return false;
    hud_.ReverseReturn([this, place](int result) { ReverseChoice(place, result); });
    return true;
}

void PlayScreen::ReverseChoice(std::uint8_t place, int result) {
    if (replay_ || !shared_ || !shared_->session) return;
    // 1 the last recall point (option 2), 2 the place of death (3), 3 a point on the world map (7 with its ID), 0
    // nothing.
    if (result == 1) {
        shared_->session->UseItem(place, proto::kReverseToRecall);
    } else if (result == 2) {
        shared_->session->UseItem(place, proto::kReverseToDeath);
    } else if (result == 3) {
        ChooseReturnPoint(hudContext_, [this, place](std::uint32_t id) {
            if (!replay_ && shared_ && shared_->session) shared_->session->UseItem(place, proto::kReverseToPoint, id);
        });
    }
}

void PlayScreen::UseSkill(ScreenContext& context, std::uint32_t skill) {
    if (replay_ || !tables_ || !tables_->Usable()) return;
    // Not while its wait runs (the client's own check, "Still have time to reuse the skill."); on the selection (the
    // server checks the rest).
    const assets::SkillRow* s = tables_->skills.Find(skill);
    if (s == nullptr || s->activity == 0) return;
    if (hudContext_.slotCooling && hudContext_.slotCooling({game::QuickSlot::Kind::Skill, skill}).cooling > 0.0f) {
        player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_SKILL_USE_FAIL_TIMEDELAY_COOL_TIME"), context.time);
        return;
    }
    // The selection, a monster or a player (the server takes what fits the skill: an attack a monster, a heal, a buff
    // or a resurrection a player).
    const std::uint32_t target = player_.target.Selected() && player_.target.entity != view_.Self() ? player_.target.entity : 0;
    // Two commands held: a skill that queues is not sent; an activity-1 one (an imbue) is.
    if (s->activity != 1 && actionsHeld_ >= 2) return;
    // A teleport goes toward the ground under the cursor, or along the cursor's heading when its ray meets nothing
    // (the ground click's own pick); the server goes as far as the
    // skill reaches and the way allows.
    if (assets::FindSkillParam(*s, assets::SkillTag("tele")) || assets::FindSkillParam(*s, assets::SkillTag("tel2"))) {
        const ui::Vec2 m = context.input->mouse();
        proto::WorldPosition point;
        if (world_ && world_->Pick(m.x, m.y, float(context.device.SwapchainWidth()), float(context.device.SwapchainHeight()), point, true)) {
            // During its own swing (or within 500 ms of a command sent) kept, sent when that ends; a later ground
            // click replaces it (the 1.188 walk keeper).
            if (OwnMoveHeld(context.time) || context.time < actionSentAt_ + 0.5) {
                if (!heldMove_) heldSince_ = context.time;
                heldUntil_ = context.time + 5.0;
                heldMove_ = point;
                heldSky_ = false;
                heldSkill_ = skill;
                return;
            }
            shared_->session->UseSkill(skill, 0, &point);
            actionSentAt_ = context.time;
        }
        return;
    }
    shared_->session->UseSkill(skill, target);
    actionSentAt_ = context.time;
}

void PlayScreen::HudRequests(ScreenContext& context) {
    for (const HudRequest& r : hud_.TakeRequests()) {
        switch (r.kind) {
        case HudRequest::Kind::Restart:
        case HudRequest::Kind::Quit:
            // The server counts down, then the character leaves (to the select, or the game ends).
            if (replay_) {
                context.quit = true;
                break;
            }
            exitAfterLogout_ = r.kind == HudRequest::Kind::Quit;
            shared_->session->RequestLogout(exitAfterLogout_ ? proto::LogoutKind::Exit : proto::LogoutKind::Restart);
            break;
        case HudRequest::Kind::Berserk:
            if (!replay_ && player_.self.berserk >= game::CharacterStatus::kBerserkBars) shared_->session->Berserk();
            break;
        case HudRequest::Kind::PetCommand: CosCommand(r.value, hudContext_.cosSelected); break; // Insert, Home, PgUp ...
        case HudRequest::Kind::SitStand: UseAction(context, 1000); break; // N: the Action page's Sit / Stand
        case HudRequest::Kind::PickUp: UseAction(context, 1008); break;   // G: its Auto Looting
        case HudRequest::Kind::NextView:
            // F9: free, third person, quarter view, round; a system line
            // names the new one.
            if (context.options != nullptr) {
                context.options->sight = static_cast<core::Sight>((static_cast<int>(context.options->sight) + 1) % 3);
                context.optionsChanged = true;
                static constexpr const char* kSaid[3] = {"UIIT_STT_CHANGED_SIGHT_FREE_DESC", "UIIT_STT_CHANGED_SIGHT_THIRD_PERSON_DESC",
                                                         "UIIT_STT_CHANGED_SIGHT_QUARTER_VIEW_DESC"};
                player_.Post(game::GameMessage::Kind::General, std::string(gui_.data().Text(kSaid[static_cast<int>(context.options->sight)])),
                             context.time);
            }
            break;
        case HudRequest::Kind::QuickSlotChanged:
            // The server keeps the bar (the original's 0x7541).
            if (!replay_ && r.value >= 0 && r.value < game::QuickSlots::kCount) {
                const game::QuickSlot& q = player_.quickSlots.slots[std::size_t(r.value)];
                shared_->session->SetQuickSlot({std::uint8_t(r.value), std::uint8_t(q.kind), q.value});
            }
            break;
        case HudRequest::Kind::ToggleQuests: ToggleQuestWindow(hudContext_); break;
        case HudRequest::Kind::ToggleAlchemy: ToggleAlchemyWindow(hudContext_); break;
        case HudRequest::Kind::Chat: {
            // Numbered and kept until the server's answer prints it.
            if (replay_) break;
            proto::ChatSend line;
            line.kind = proto::ChatKind(r.value);
            line.target = r.target;
            line.text = r.text;
            // A game master's command ("/..."): not kept, so its answer prints nothing of it (the GM line answers it).
            line.index = !line.text.empty() && line.text.front() == '/' ? std::uint8_t(0xFF) : player_.QueueChat(line.kind, line.target, line.text);
            line.slot = std::uint8_t(r.extra);
            shared_->session->SendChat(line);
            break;
        }
        case HudRequest::Kind::CancelCasting: {
            // Asked first (ours: the original's text for it, its box not traced).
            if (replay_ || player_.casting.item == 0) break;
            hud_.Confirm("UIIT_STT_CONFIRM_BOX", {gui_.data().Text("UIIT_STT_TRANSITION_CANCEL")}, [this](bool yes) {
                if (yes && shared_ && shared_->session && player_.casting.item != 0) shared_->session->CancelReturn();
            });
            break;
        }
        case HudRequest::Kind::GuildStorage:
            if (!replay_) shared_->session->OpenGuildStorage(std::uint32_t(r.value));
            break;
        case HudRequest::Kind::OpenStorage:
            if (!replay_) shared_->session->OpenStorage(std::uint32_t(r.value));
            break;
        case HudRequest::Kind::CancelBuff:
            // A right click on a skill's buff on the board: the server ends it (K4; BuffRemoved answers).
            if (!replay_ && r.value >= 0 && std::size_t(r.value) < player_.effects.size()) {
                const game::ActiveEffect& e = player_.effects[std::size_t(r.value)];
                if (e.kind == game::ActiveEffect::Kind::Skill && !e.debuff) shared_->session->CancelBuff(e.token);
                // A gold, silver or skill time service: "Pausing takes 20 seconds. Are you sure ...?" or the pause's end
                // (the confirm box, UIIT_STT_GOLDTIME_PAUSE_START / _STOP), then the original's 0x74BF.
                const std::uint8_t category = std::uint8_t(e.token & 0xFF);
                if (e.kind == game::ActiveEffect::Kind::TimedJob && (e.token & 0xFFFF0000u) == game::PlayerState::kTimedServiceToken &&
                    (category == game::kTimedGoldSilver || category == game::kTimedSkill)) {
                    bool paused = false;
                    for (const proto::TimedService& s : player_.timedServices.services) {
                        if (s.category == category) paused = s.paused != 0;
                    }
                    hud_.Confirm("UIIT_STT_CONFIRM_BOX", {gui_.data().Text(paused ? "UIIT_STT_GOLDTIME_PAUSE_STOP" : "UIIT_STT_GOLDTIME_PAUSE_START")},
                                 [this, category, paused](bool yes) {
                                     if (yes && !replay_ && shared_ && shared_->session) shared_->session->SendGuild(proto::TimedPause{category, std::uint8_t(!paused)});
                                 });
                }
                // A bonus scroll's run (experience, skill points, ...): "Will you cancel the use of the selected item?" /
                // "... its effect will be lost.", then TimedCancel (ours: the original's message not traced).
                if (e.kind == game::ActiveEffect::Kind::TimedJob && (e.token & 0xFFFF0000u) == game::PlayerState::kTimedServiceToken &&
                    game::IsBonusCategory(category)) {
                    hud_.Confirm("UIIT_STT_CONFIRM_BOX",
                                 {gui_.data().Text("UIIT_MSG_QUESTION_SILKMALL_ITEM_USE_CANCLE_1"), gui_.data().Text("UIIT_MSG_QUESTION_SILKMALL_ITEM_USE_CANCLE_2")},
                                 [this, category](bool yes) {
                                     if (yes && !replay_ && shared_ && shared_->session) shared_->session->SendGuild(proto::TimedCancel{category});
                                 });
                }
                // The academy's accumulation: "Are you sure to transfer cumulated experience points to the Guardian?",
                // then the original's 0x7483.
                if (e.kind == game::ActiveEffect::Kind::TimedJob && e.token == game::PlayerState::kAcademyToken) {
                    hud_.Confirm("UIIT_STT_CONFIRM_BOX", {gui_.data().Text("UIIT_CTL_TC_PASS_BUFF")}, [this](bool yes) {
                        if (yes && !replay_ && shared_ && shared_->session) shared_->session->SendGame(proto::AcademyPassBuff::kId, proto::Encode(proto::AcademyPassBuff{}));
                    });
                }
            }
            break;
        case HudRequest::Kind::ShowDroppedItems: showDropNames_ = r.value != 0; break;
        case HudRequest::Kind::DropItem: {
            // Asked first: ownership lost, anyone may take it.
            if (replay_) break;
            const UiData& d = gui_.data();
            const std::uint8_t place = std::uint8_t(r.value);
            const std::uint32_t was = player_.inventory.At(place) != nullptr ? player_.inventory.At(place)->refId : 0u;
            hud_.Confirm("UIIT_STT_CONFIRM_BOX", {d.Text("UIIT_MSG_DROP_WARNING_1"), d.Text("UIIT_MSG_DROP_WARNING_2")}, [this, place, was](bool yes) {
                const game::ItemInstance* item = player_.inventory.At(place);
                if (yes && shared_ && shared_->session && item != nullptr && item->refId == was) shared_->session->DropItem(place);
            });
            break;
        }
        case HudRequest::Kind::DropGold:
            if (!replay_) shared_->session->DropGold(std::strtoull(r.text.c_str(), nullptr, 10));
            break;
        case HudRequest::Kind::OpenShop:
            if (!replay_) {
                shopGroup_ = r.extra;
                shared_->session->Shop(proto::ShopOpen{std::uint32_t(r.value)});
            }
            break;
        case HudRequest::Kind::ShopBuy:
            if (!replay_ && player_.shopNpc != 0) {
                shared_->session->Shop(proto::ShopBuy{player_.shopNpc, std::uint8_t(r.value & 0xFF), std::uint8_t((r.value >> 8) & 0xFF), std::uint16_t(r.extra)});
            }
            break;
        case HudRequest::Kind::ShopSell:
            if (!replay_ && player_.shopNpc != 0) shared_->session->Shop(proto::ShopSell{player_.shopNpc, std::uint8_t(r.value), std::uint16_t(r.extra)});
            break;
        case HudRequest::Kind::ShopBuyback:
            if (!replay_ && player_.shopNpc != 0) shared_->session->Shop(proto::ShopBuyback{player_.shopNpc, std::uint8_t(r.value)});
            break;
        case HudRequest::Kind::StorageGold:
            if (!replay_) shared_->session->MoveStorageGold((r.value & 1) != 0, std::strtoull(r.text.c_str(), nullptr, 10), (r.value & 2) != 0);
            break;
        case HudRequest::Kind::StorageMove:
            if (!replay_) {
                shared_->session->MoveStorageItem({(r.value & (1 << 16)) != 0, (r.value & (1 << 17)) != 0, std::uint8_t(r.value & 0xFF),
                                                   std::uint8_t((r.value >> 8) & 0xFF), 0, std::uint8_t((r.value & (1 << 18)) != 0 ? 1 : 0)});
            }
            break;
        case HudRequest::Kind::UseSkill:
            if (!replay_ && r.value > 0) UseSkill(context, std::uint32_t(r.value));
            break;
        case HudRequest::Kind::UseQuickSlot: {
            // An item shortcut: equipment and ammunition are worn, as if dropped
            // on the equipment; other items are used. Worn-item shortcuts do nothing.
            if (replay_ || r.value < 0 || r.value >= game::QuickSlots::kCount) break;
            const game::QuickSlot& q = player_.quickSlots.slots[std::size_t(r.value)];
            if (q.kind == game::QuickSlot::Kind::Skill) {
                UseSkill(context, q.value);
                break;
            }
            if (q.kind == game::QuickSlot::Kind::Action) { // as a click on its icon of the Action page
                UseAction(context, q.value);
                break;
            }
            if (q.kind != game::QuickSlot::Kind::Item || !tables_ || !tables_->Usable()) break;
            const std::uint8_t place = std::uint8_t(q.value);
            const game::ItemInstance* item = player_.inventory.At(place);
            const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
            if (row == nullptr) break;
            const int wornAt = game::EquipPlaceFor(*row, player_.inventory.At(game::slot::kRingLeft) != nullptr,
                                                   player_.inventory.At(game::slot::kRingRight) != nullptr);
            if (wornAt >= 0) {
                shared_->session->MoveItem({place, std::uint8_t(wornAt), 0});
            } else if (ActionBarRuns(context.time)) {
                // Refused silently while the player's action bar runs.
            } else if (!MurdererScrollRefused(place, context.time) && !BerserkScrollRefused(place, context.time) && !UseCrestItem(place) && !UseGlobalChatItem(place) && !UseReverseReturn(place) && !UseFortressItem(place) && !UseMallItem(place) &&
                       !UseServiceItem(hudContext_, place)) {
                // With the selection (a quest's trap or branch takes it).
                shared_->session->UseItem(place, 0, 0, player_.target.Selected() && player_.target.entity != view_.Self() ? player_.target.entity : 0);
            }
            break;
        }
        case HudRequest::Kind::UseItem:
            if (!replay_ && !ActionBarRuns(context.time) && !MurdererScrollRefused(std::uint8_t(r.value), context.time) && !BerserkScrollRefused(std::uint8_t(r.value), context.time) && !UseCrestItem(std::uint8_t(r.value)) && !UseGlobalChatItem(std::uint8_t(r.value)) && !UseTalisman(std::uint8_t(r.value)) &&
                !UseReverseReturn(std::uint8_t(r.value)) && !UseFortressItem(std::uint8_t(r.value)) && !UseMallItem(std::uint8_t(r.value)) &&
                !UseServiceItem(hudContext_, int(r.value))) {
                shared_->session->UseItem(std::uint8_t(r.value), 0, 0, player_.target.Selected() && player_.target.entity != view_.Self() ? player_.target.entity : 0);
            }
            break;
        case HudRequest::Kind::GateTeleport:
            if (replay_) break;
            shared_->session->UseGate(std::uint32_t(r.value), std::uint32_t(r.extra));
            waitingForGate_ = true;
            break;
        case HudRequest::Kind::GateDesignate: {
            // The agreement box first (kind 5): where the character is now, then
            // the question; Yes asks the server.
            if (replay_) break;
            std::string here;
            if (const game::WorldView::Entity* self = view_.Find(view_.Self())) here = gui_.data().Text(std::to_string(self->position.region)); // the zone by its region (textzonename)
            const UiData& d = gui_.data();
            const std::uint32_t gate = std::uint32_t(r.value);
            hud_.Confirm("UIIT_STT_AGREEMENT_BOX", {FormatText(d.Text("UIIT_MSG_MSGBOX_SETREBIRTH_CURPOS"), {here}), d.Text("UIIT_MSG_MSGBOX_SETREBIRTH_ASKCERTIFY")},
                         [this, gate](bool yes) {
                             if (yes && shared_ && shared_->session) shared_->session->DesignateReturn(gate);
                         });
            break;
        }
        case HudRequest::Kind::MoveItem:
            // The server decides and answers (ItemMoved); what others see follows (EquipmentChanged).
            if (!replay_) shared_->session->MoveItem({std::uint8_t(r.value), std::uint8_t(r.extra & 0xFF), std::uint16_t(r.extra >> 8)});
            break;
        case HudRequest::Kind::WearAvatar:
        case HudRequest::Kind::TakeOffAvatar:
            // The server decides and answers (AvatarMoved).
            if (!replay_) shared_->session->MoveAvatar({r.kind == HudRequest::Kind::WearAvatar, std::uint8_t(r.value)});
            break;
        case HudRequest::Kind::ChatCommand:
            if (PremiumCommand(r.text)) break; // the premium's /Return, /Reverse Return, /Resurrection (PlayMallItems.cpp)
            ChatCommand(context, r.target, r.text); // the command's key (r.target) and its whole line
            break;
        case HudRequest::Kind::LearnSkill:
            if (!replay_) shared_->session->LearnSkill(std::uint32_t(r.value));
            break;
        case HudRequest::Kind::RaiseMastery:
            if (!replay_) shared_->session->RaiseMastery(std::uint32_t(r.value));
            break;
        case HudRequest::Kind::RaiseStat:
            if (!replay_) shared_->session->RaiseStat(std::uint8_t(r.value));
            break;
        case HudRequest::Kind::TargetSelf: {
            if (player_.self.hp == 0 && player_.self.maxHp > 0) DeathWindow(context.time, true);
            // The player's own bar: the target window on him (the server's selection comes with
            // targeting, C10).
            const game::CharacterStatus& s = player_.self;
            game::TargetStatus t;
            t.entity = s.entity;
            t.kind = game::TargetStatus::Kind::Player;
            t.model = s.model;
            t.name = s.name;
            t.race = s.race;
            player_.target = t;
            break;
        }
        default:
            PartyRequest(r); // the party's (PlayParty.cpp); the game's other actions come with their features
            break;
        }
    }
}

void PlayScreen::TakeSpeech(double now) {
    // Lines to the people around, a game master's and global lines make speech over the speaker
    // (found by its entity, else by name among those in view: the player's own lines too); the
    // others none.
    const std::uint64_t fresh = std::min<std::uint64_t>(player_.chatPosted - speechSeen_, player_.chat.size());
    for (std::size_t i = player_.chat.size() - std::size_t(fresh); i < player_.chat.size(); ++i) {
        const game::ChatEntry& e = player_.chat[i];
        if (e.result != proto::ChatResult::Ok) continue;
        std::string text = ChatText(e.text);
        ui::Color colour = ui::Argb(0xFFFFFFFF);
        switch (e.kind) {
        case proto::ChatKind::All: break;
        case proto::ChatKind::Gm:
            if (text.starts_with(';')) {
                text.erase(0, 1);
            } else {
                colour = ui::Argb(0xFFFFAEC3);
            }
            break;
        case proto::ChatKind::Global: colour = ui::Argb(0xFFFFFF00); break;
        case proto::ChatKind::Speech: colour = ui::Argb(0xFFDBADF8); break; // a monster's or an NPC's line
        default: continue;
        }
        std::uint32_t who = e.sender != 0 && view_.Find(e.sender) != nullptr ? e.sender : 0;
        for (const auto& [id, entity] : view_.Entities()) {
            if (who != 0) break;
            if (!e.name.empty() && entity.kind == proto::EntityKind::Character && entity.look.name == e.name) who = id;
        }
        if (who != 0) speech_[who] = {std::move(text), colour, now + 10.0}; // a new line replaces the last, 10 s again
    }
    speechSeen_ = player_.chatPosted;
}

void PlayScreen::DrawNames(ScreenContext& context, ui::Painter& paint) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    // Each one's name just over its head (the speech's point, 22 up), centred, with a shadow:
    // players white (the player's own gold-orange), NPCs light blue, monsters by the level difference.
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    const double alpha = clock_.Alpha();
    const float pitch = ui::TextLineHeight(0);
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const proto::FlatPosition me = self != nullptr ? proto::ToFlat(self->position) : proto::FlatPosition{};
    const bool namesFar = context.options != nullptr && context.options->beta.namesFar;
    itemLabels_.clear();
    for (const auto& [id, e] : view_.Entities()) {
        // Only over a body on the stage (not one still waiting for a slot or its model).
        if (e.kind != proto::EntityKind::GroundItem && !slots_.contains(id)) continue;
        // Who gets a name. The original's: within 300 of the player (the body's distance, height included), and the one
        // under the mouse at any distance; then the Game page's rows by kind (NameShown). Ours (the Beta page's choice):
        // players' names at any distance, the others' within the name distance. An item on the ground: within 300 both ways.
        const bool hovered = id == hoveredEntity_;
        if (self != nullptr && !hovered) {
            const proto::FlatPosition p = proto::ToFlat(e.position);
            const double flat = std::hypot(p.x - me.x, p.z - me.z);
            if (e.kind == proto::EntityKind::GroundItem) {
                if (flat > double(std::min(nameDistance_, 300.0f))) continue;
            } else if (namesFar) {
                if ((e.kind != proto::EntityKind::Character || e.look.name.empty()) && flat > double(nameDistance_)) continue;
            } else if (std::hypot(flat, double(e.position.y - self->position.y)) >= 300.0) {
                continue;
            }
        }
        // The name by the Game page's rows; its other lines (a guild's, a pet's) and its status gauge stand anyway.
        const bool nameShown = hovered || NameShown(id, e, context);
        float gaugeHp = 0.0f, gaugeMp = 0.0f;
        const int gauge = StatusGauge(id, e, context, gaugeHp, gaugeMp);
        std::string name = e.look.name;
        std::string petLine; // a COS's own name, under its owner's
        // A player in a job's suit shows "*" and its alias instead, and no guild line.
        const auto job = e.kind == proto::EntityKind::Character ? entityJobs_.find(id) : entityJobs_.end();
        const bool inJob = job != entityJobs_.end() && !job->second.alias.empty();
        if (inJob) name = "*" + job->second.alias;
        ui::Color colour = ui::Argb(0xFFFFFFFF);
        float lift = 22.0f; // over the head
        if (e.kind == proto::EntityKind::GroundItem) {
            // An item on the ground shows its name only (no plus, no count; gold its amount), light
            // blue when the server's byte is 1 (an ordinary item with magic options), 5 above it.
            const assets::ItemRow* row = tables_->items.Find(e.look.model);
            if (row == nullptr) continue;
            const bool gold = row->typeId[2] == 5 && row->typeId[1] == 3; // ITEM_ETC_GOLD_*
            name = gold ? GoldText(e.amount) + " " + gui_.data().Text("UIIT_STT_GOLD") : gui_.data().Text(row->nameKey);
            colour = e.rarity == 1 ? ui::Argb(0xFF72BFFF) : ui::Argb(0xFFFFFFFF);
            lift = 5.0f;
            // A name after the item's, as the 1.188 client does: specialty goods (3/3/8) "Name (*Alias)", their buyer's
            // alias; a quest or event item (3/3/9) "Name (Owner)" (upstream's review 2026-11-22).
            if (!e.look.name.empty() && row->typeId[0] == 3 && row->typeId[1] == 3) {
                if (row->typeId[2] == 8) name += " (*" + e.look.name + ")";
                else if (row->typeId[2] == 9) name += " (" + e.look.name + ")";
            }
        } else if (name.empty() && IsNpc(e)) {
            if (const assets::ObjectRow* row = tables_->characters.Find(e.look.model)) name = gui_.data().Text(row->nameKey);
            colour = ui::Argb(0xFF9ED0FF); // the original's NPC light blue
        } else if (const assets::TeleportGate* gate = name.empty() ? GapGate(e) : nullptr) {
            name = gui_.data().Text(gate->nameKey); // "Gap of Dimensions"; its lines under it
            colour = ui::Argb(0xFFEFDAA4);
            lift = 40.0f;
        } else if (e.kind == proto::EntityKind::Cos) {
            // A COS: its owner's name over it (white), a pet's own line under that (light yellow, below). Its owner
            // unseen: its own line alone. A mount under its rider: the rider's name is over them both already.
            if (const auto rides = riders_.find(e.amount); rides != riders_.end() && rides->second == id) continue;
            // Its owner in a job's suit: the alias it goes by, never its name (ours, as over the owner itself).
            const game::WorldView::Entity* owner = view_.Find(e.amount);
            name = owner != nullptr ? owner->look.name : std::string();
            const auto ownerJob = entityJobs_.find(e.amount);
            const bool ownerInJob = ownerJob != entityJobs_.end() && !ownerJob->second.alias.empty();
            if (ownerInJob) name = ownerJob->second.alias;
            // Its own line, "< Name >" (a CICCos's name between "< " and " >"). A pet's only: a ride or a
            // transport has no name. Not while its owner is in a job: the original server sends no name then
            // (only when the owner's job type is 4), so the suit's wearer
            // is not known by it. An unnamed pet's "< No name >" (UIIT_STT_COSNEWUI_TITLE) as the original looks in play
            // (the user's capture 2026-11-14; ours: the stock server and client send and build no line for an empty name).
            const assets::ObjectRow* cosRow = tables_->characters.Find(e.look.model);
            const game::CosKind cosKind = cosRow != nullptr ? game::CosKindOf(*cosRow) : game::CosKind::None;
            const bool pet = cosKind == game::CosKind::AttackPet || cosKind == game::CosKind::GrabPet;
            if (pet && !ownerInJob) {
                petLine = "< " + (!e.look.name.empty() ? e.look.name : gui_.data().Text("UIIT_STT_COSNEWUI_TITLE")) + " >";
            }
            if (name.empty()) name = std::exchange(petLine, std::string());
        } else if (e.kind == proto::EntityKind::Monster) {
            // Monsters in the target window's colour for the level difference (over their heads ours).
            const assets::ObjectRow* row = tables_->characters.Find(e.look.model);
            if (row == nullptr) continue;
            name = gui_.data().Text(row->nameKey);
            if ((e.rarity & 0x0F) == 7) name = gui_.data().Text("UIIT_STT_MOB_STRONG") + " " + name; // an event's Strong one
            const int gap = row->level - int(player_.self.level);
            colour = ui::Argb(gap <= -7 ? 0xFF87D2FF : gap <= -4 ? 0xFFA5E0CE : gap <= 0 ? 0xFFFFFFFF : gap <= 5 ? 0xFFFFB387 : 0xFFFF8787);
            lift = 22.0f * MonsterScale(row, e.rarity);
        } else if (e.kind == proto::EntityKind::Character) {
            colour = PlayerNameColour(id, e); // the PvP relations and states (PlayPk.cpp)
        }
        // A character's name stands at its body's height (ours; the original's name height is not traced), so a
        // quest's mark (the original's "*" with 5 more, at the body's height + 5) floats over the name.
        if ((e.kind == proto::EntityKind::Character && !GapGate(e)) || e.kind == proto::EntityKind::Cos) {
            if (const auto slot = slots_.find(id); slot != slots_.end()) {
                if (const float body = world_->StageHeight(slot->second); body > 1.0f) lift = body;
            }
            // A rider sits on its mount's saddle, above its own ground point: its name over its head up there. Seated,
            // its head is about half its standing height over the saddle (ours: 0.6 of it, a little over the head).
            glm::vec3 seat(0.0f);
            float seatYaw = 0.0f;
            if (e.kind == proto::EntityKind::Character && SeatOf(id, seat, seatYaw)) {
                lift = lift * 0.6f + std::max(0.0f, seat.y - world_->OriginRelative(game::WorldView::Drawn(e, alpha)).y);
            }
        }
        float x = 0, y = 0, depth = 0;
        if (name.empty() || !world_->Project(game::WorldView::Drawn(e, alpha), lift, width, height, x, y, depth)) continue;
        const float w = paint.TextWidth(name, 0) + 2.0f;
        const ui::Rect at{std::floor(x - w / 2.0f), std::floor(y - pitch - 2.0f), w, pitch};
        if (e.kind == proto::EntityKind::GroundItem) itemLabels_.push_back({id, at, depth});
        // At the head's depth: hidden behind walls, as the original's.
        paint.PushDepth(depth);
        if (nameShown) ui::DrawLabel(paint, name, at, colour, 0, 1, 1, ui::kTextShadow);
        bool guildLine = false; // a guild line over the name (the status gauge goes over it)
        // The marks beside a player's name, 16 x 16 at the name's top, as the original draws them (and, as it does, only
        // with the Game page's own-name row on): its job's icon 4 right of the name while in a job suit (the trader's,
        // the thief's, the hunter's), the beginner mark 20 left of it while the player wears one.
        if (e.kind == proto::EntityKind::Character && !IsNpc(e) && GapGate(e) == nullptr &&
            (context.options == nullptr || context.options->game.Get("ownName"))) {
            static constexpr const char* kJobIcons[3] = {"interface\\ifcommon\\com_job_merchant.ddj", "interface\\ifcommon\\com_job_thief.ddj",
                                                         "interface\\ifcommon\\com_job_hunter.ddj"};
            if (job != entityJobs_.end() && job->second.state >= 1 && job->second.state <= 3) {
                paint.Image(kJobIcons[job->second.state - 1], {at.x + at.w + 3.0f, at.y, 16.0f, 16.0f});
            }
            if (const auto marks = entityMarks_.find(id); marks != entityMarks_.end() && (marks->second & proto::kMarkBeginner) != 0) {
                paint.Image("icon\\etc\\icon_rudiment.ddj", {at.x + 1.0f - 20.0f, at.y, 16.0f, 16.0f});
            }
        }
        // A party monster's mark (europe_partymob, 16 x 16) 20 left of its name, as the original's monsters draw it.
        if (e.kind == proto::EntityKind::Monster && (e.rarity >> 4) == 1) {
            paint.Image("icon\\item\\etc\\europe_partymob.ddj", {at.x + 1.0f - 20.0f, at.y, 16.0f, 16.0f});
        }
        // A job suit's change or a flag pole's action: a 48 x 2 bar (4.8 a second) 10 below the point 2 over the body,
        // its right end coming in a tenth at a time, yellow.
        if (const auto bar = jobChanges_.find(id); bar != jobChanges_.end()) {
            float bx = 0, by = 0, bd = 0;
            if (context.time - bar->second.started < double(bar->second.seconds) &&
                world_->Project(game::WorldView::Drawn(e, alpha), lift + 2.0f, width, height, bx, by, bd)) {
                DrawActionBar(paint, bx, by, context.time - bar->second.started, bar->second.seconds);
            }
        }
        // A gap's lines under its name (a job temple's door has none there: Seth's count is its tooltip's).
        if (const auto info = gaps_.find(id); GapGate(e) != nullptr && (info == gaps_.end() || !info->second.temple)) {
            float below = at.y;
            for (const std::string& line : GapLines(id)) {
                below += pitch;
                const float lw = paint.TextWidth(line, 0) + 2.0f;
                ui::DrawLabel(paint, line, {std::floor(x - lw / 2.0f), below, lw, pitch}, ui::Argb(0xFFFFFFFF), 0, 1, 1, ui::kTextShadow);
            }
        } else if (e.kind == proto::EntityKind::Character && !IsNpc(e) && !inJob) {
            if (context.options == nullptr || context.options->game.Get("guildNames")) guildLine = DrawGuildLine(paint, id, at); // the guild row
        } else if (!petLine.empty()) {
            // Light yellow (1.188 CICCos's setup, its name line's colour 0xFFFFFF55).
            const float pw = paint.TextWidth(petLine, 0) + 2.0f;
            ui::DrawLabel(paint, petLine, {std::floor(x - pw / 2.0f), at.y + pitch, pw, pitch}, ui::Argb(0xFFFFFF55), 0, 1, 1, ui::kTextShadow);
        }
        // A stall's name over its booth, in the booth's colour (0xFFFEB5FF); ours a line above.
        if (const auto booth = entityStalls_.find(id); booth != entityStalls_.end()) {
            const float bw = paint.TextWidth(booth->second.name, 0) + 2.0f;
            ui::DrawLabel(paint, booth->second.name, {std::floor(x - bw / 2.0f), at.y - pitch - 4.0f, bw, pitch}, ui::Argb(0xFFFEB5FF), 0, 1, 1, ui::kTextShadow);
        }
        // The status gauge 25 over the name, past the guild line when one shows (the original lifts the name's point 20 for
        // that line, then 25 for the gauge), centred: a player's quick_hpmp_form (74 x 12: the HP bar at 2,1 and the MP bar at
        // 2,7, each 70 x 4), a monster's or a COS's quick_hp_form (74 x 6, the HP bar).
        if (gauge != 0) {
            const float top = std::floor(at.y - 25.0f - (guildLine ? 20.0f : 0.0f)), left = std::floor(x - 37.0f);
            paint.Image(gauge == 2 ? "interface\\ifcommon\\quick_hpmp_form.ddj" : "interface\\ifcommon\\quick_hp_form.ddj",
                        {left, top, 74.0f, gauge == 2 ? 12.0f : 6.0f});
            paint.Image("interface\\ifcommon\\quick_hp.ddj", {left + 2.0f, top + 1.0f, 70.0f * gaugeHp, 4.0f}, {0.0f, 0.0f}, {gaugeHp, 1.0f});
            if (gauge == 2) {
                paint.Image("interface\\ifcommon\\quick_mp.ddj", {left + 2.0f, top + 7.0f, 70.0f * gaugeMp, 4.0f}, {0.0f, 0.0f}, {gaugeMp, 1.0f});
            }
        }
        paint.PopDepth();
    }
}

void PlayScreen::GroundDecals(ScreenContext& context) {
    // The original's selection decal (CIODecal, made with the HUD at Media effect\select_01
    // .. 04.ddj in slots 0..3, 12 units across a side; shown by the selection): laid on the
    // ground under what the player chose. Slot 0 where a click sends the character (until it gets
    // there), 3 under a selected monster or pet, 2 under a player, 1 under anything else (NPCs).
    // A 6 x 6 grid, each corner dropped onto the ground under it (terrain, an object's floor, a
    // dungeon's), drawn in the scene with its camera (the effects' plates): it lies on slopes and stays
    // put however fast the camera turns; what stands there (the character's feet) hides it.
    if (!world_) return;
    constexpr int kCells = 6;
    constexpr float kHalf = 12.0f, kLift = 0.75f; // just over the ground (no flicker with it)
    std::array<glm::vec3, (kCells + 1) * (kCells + 1)> corners;
    const auto decal = [&](const proto::WorldPosition& centre, float yaw, int slot) {
        const float c = std::cos(yaw), sn = std::sin(yaw);
        const proto::FlatPosition flat = proto::ToFlat(centre);
        for (int j = 0; j <= kCells; ++j) {
            for (int i = 0; i <= kCells; ++i) {
                const float lx = (float(i) / kCells * 2.0f - 1.0f) * kHalf, lz = (float(j) / kCells * 2.0f - 1.0f) * kHalf;
                proto::WorldPosition p = proto::FromFlat({flat.x + double(lx * c + lz * sn), flat.z + double(-lx * sn + lz * c)}, centre.y);
                float y = centre.y;
                if (world_->GroundHeight(p, y)) p.y = y;
                corners[std::size_t(j * (kCells + 1) + i)] = world_->OriginRelative(p) + glm::vec3(0.0f, kLift, 0.0f);
            }
        }
        world_->Effects().GroundDecal(std::string("effect\\select_0") + char('1' + slot) + ".ddj", kCells, corners);
    };
    // One marker, as the original's: the latest of a ground click (at the destination, until the character gets there) and a
    // selection (under the selected one) has it.
    if (player_.target.entity != markedTarget_) {
        markedTarget_ = player_.target.entity;
        markedTargetAt_ = context.time;
    }
    bool destination = false;
    if (moveMarker_.born >= 0.0) {
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        const bool arrived = self != nullptr && !self->moving && context.time - moveMarker_.born > 0.5;
        if (arrived) {
            moveMarker_.born = -1.0;
        } else if (player_.target.entity == 0 || moveMarker_.born >= markedTargetAt_) {
            decal(moveMarker_.at, 0.0f, 0);
            destination = true;
        }
    }
    // Under the selected one, turned with it.
    if (player_.target.entity != 0 && !destination) {
        if (const game::WorldView::Entity* e = view_.Find(player_.target.entity)) {
            using Kind = game::TargetStatus::Kind;
            const Kind kind = player_.target.kind;
            const int slot = kind == Kind::Monster || kind == Kind::Pet ? 3 : kind == Kind::Player ? 2 : 1;
            decal(game::WorldView::Drawn(*e, clock_.Alpha()), e->yaw, slot);
        }
    }
}

void PlayScreen::DrawDamage(ScreenContext& context, ui::Painter& paint) {
    std::erase_if(damageTexts_, [&](const DamageText& d) { return context.time - d.born >= 1.0; });
    if (!world_ || damageTexts_.empty()) return;
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    // The original's sizes are for 1600 x 1200, scaled to the screen.
    const float sx = width / 1600.0f, sy = height / 1200.0f;
    for (const DamageText& d : damageTexts_) {
        float x = 0, y = 0, depth = 0;
        if (!world_->Project(d.at, 20.0f, width, height, x, y, depth)) continue;
        const double t = context.time - d.born;
        const float alpha = t < 0.5 ? 1.0f : float(std::max(0.0, 1.0 - (t - 0.5) / 0.5));
        y -= 50.0f * sy * float(std::min(1.0, t));
        const std::string digits = std::to_string(d.value);
        const float w = (d.critical ? 36.0f : 24.0f) * sx, h = (d.critical ? 60.0f : 40.0f) * sy, step = (d.critical ? 33.0f : 22.0f) * sx;
        const float left = std::floor(x - (step * float(digits.size() - 1) + w) / 2.0f), top = std::floor(y - h);
        const auto tint = [&](ui::Color c) { return (c & 0x00FFFFFFu) | (std::uint32_t(std::lround(alpha * 255.0f)) << 24); };
        if (d.blocked) {
            const ui::Rect r{std::floor(x - 52.0f * sx), std::floor(y - 40.0f * sy), 104.0f * sx, 40.0f * sy}; // its art's 104 x 40
            paint.Image("interface\\hitcount\\blocking_shadow.ddj", r, {0, 0}, {1, 1}, tint(0xFF000000u));
            paint.Image("interface\\hitcount\\blocking.ddj", r, {0, 0}, {1, 1}, tint(d.colour));
            continue;
        }
        for (std::size_t i = 0; i < digits.size(); ++i) {
            const ui::Rect r{left + step * float(i), top, w, h};
            const std::string base = std::string("interface\\hitcount\\hitcount_") + digits[i];
            paint.Image(base + "_shadow.ddj", r, {0, 0}, {1, 1}, tint(0xFF000000u));
            paint.Image(base + ".ddj", r, {0, 0}, {1, 1}, tint(d.colour));
        }
        if (d.critical) {
            const ui::Rect r{std::floor(x - 48.0f * sx), top + 45.0f * sy, 96.0f * sx, 24.0f * sy};
            paint.Image("interface\\hitcount\\critical_shadow.ddj", r, {0, 0}, {1, 1}, tint(0xFF000000u));
            paint.Image("interface\\hitcount\\critical.ddj", r, {0, 0}, {1, 1}, tint(d.colour));
        }
    }
}

void PlayScreen::DrawSpeech(ScreenContext& context, ui::Painter& paint) {
    if (!world_) return;
    // Over the head (its height, 20, + 2; observed): the text alone in the line's colour,
    // wrapped at 200 pixels a glyph at a time, on a 25 % black box a pixel larger, its bottom 10
    // pixels above that point; at the head's depth (the original's carries it), hidden behind walls.
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    const double alpha = clock_.Alpha();
    const float pitch = ui::TextLineHeight(0);
    for (auto it = speech_.begin(); it != speech_.end();) {
        const game::WorldView::Entity* e = view_.Find(it->first);
        if (e == nullptr || context.time >= it->second.until) {
            it = speech_.erase(it);
            continue;
        }
        float x = 0, y = 0, depth = 0;
        if (!world_->Project(game::WorldView::Drawn(*e, alpha), 22.0f, width, height, x, y, depth)) {
            ++it;
            continue;
        }
        std::vector<std::string> lines(1);
        const std::string& text = it->second.text;
        for (std::size_t i = 0; i < text.size();) {
            std::size_t next = i + 1;
            while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
            const std::string glyph = text.substr(i, next - i);
            if (!lines.back().empty() && paint.TextWidth(lines.back() + glyph, 0) > 200.0f) lines.emplace_back();
            lines.back() += glyph;
            i = next;
        }
        float w = 0.0f;
        for (const std::string& line : lines) w = std::max(w, paint.TextWidth(line, 0));
        const float h = pitch * float(lines.size());
        const float left = std::floor(x - w / 2.0f), top = std::floor(y - 10.0f - h);
        paint.PushDepth(depth); // hidden behind walls, as the names
        paint.Fill({left - 1.0f, top - 1.0f, w + 2.0f, h + 2.0f}, ui::Argb(0x40000000));
        for (std::size_t k = 0; k < lines.size(); ++k) {
            paint.Text(lines[k], {left, top + pitch * float(k), w + 2.0f, pitch}, it->second.colour, 0, 0, 0);
        }
        paint.PopDepth();
        ++it;
    }
}

void PlayScreen::PickCursor(ScreenContext& context) {
    // The scene's depth under the mouse, read back for the clicks' occlusion (ours; WorldScreen::Occluded).
    if (world_ && context.input != nullptr) {
        const ui::Vec2 m = mouse_.x >= 0.0f ? mouse_ : context.input->mouse();
        world_->SetProbePoint(m.x, m.y);
    }
    // Every 250 ms (the HUD's timer 1): over a window the arrow; else by what one click there would pick (the
    // original's): an item the open hand (the fist while the left button is held), a gate its own, an NPC the speech
    // bubble, a monster the sword when it may be fought, another player or a COS by PlayerCursor.
    if (context.time - cursorPicked_ >= 0.25 && context.input != nullptr) {
        cursorPicked_ = context.time;
        cursor_ = 0x95;
        hoveredGap_ = 0;
        hoveredEntity_ = 0;
        const bool overWindow = gui_.interface() != nullptr && gui_.context().mouseOverUi();
        if (!overWindow && world_ && !replay_) {
            const ui::Vec2 m = mouse_.x >= 0.0f ? mouse_ : context.input->mouse(); // --mouse: held there
            const float w = float(context.device.SwapchainWidth()), h = float(context.device.SwapchainHeight());
            proto::WorldPosition at;
            const bool ground = world_->Pick(m.x, m.y, w, h, at);
            const Picked picked = PickUnder(m.x, m.y, w, h);
            const bool alt = context.input->KeyDown(ui::Key::Alt);
            if (picked.item != 0) {
                cursor_ = context.input->Down(ui::MouseButton::Left) ? 0x9A : 0x99; // the fist (154) / the open hand (153)
            } else if (const std::uint32_t npc = picked.npc; npc != 0) {
                cursor_ = 0x98; // the speech bubble (152)
                hoveredEntity_ = npc;
                if (const game::WorldView::Entity* e = view_.Find(npc); e != nullptr && GapGate(*e) != nullptr) {
                    hoveredGap_ = npc;
                    // A job temple's door asked again under the mouse, every 2 s: Seth's players (the original's come with
                    // each selection).
                    if (const auto g = gaps_.find(npc); g != gaps_.end() && g->second.temple && context.time - gapHoverAskedAt_ >= 2.0 && shared_ &&
                                                        shared_->session) {
                        gapHoverAskedAt_ = context.time;
                        shared_->session->SendGame(proto::SelectGap::kId, proto::Encode(proto::SelectGap{npc}));
                    }
                }
            } else if (const std::uint32_t monster = picked.monster; monster != 0) {
                if (!OwnStructure(monster)) cursor_ = 0x97; // the sword (151)
                hoveredEntity_ = monster;
            } else if (picked.player != 0) {
                // A rider's: the pick gives his mount; the cursor is the COS's, by its rider's relation as his owner's.
                cursor_ = PlayerCursor(picked.player, alt);
                hoveredEntity_ = picked.player; // his name shown
            } else if (picked.cos != 0) {
                // A COS: the red sword by its owner's relation, the fortress war's side by his guild (the original's).
                const game::WorldView::Entity* c = view_.Find(picked.cos);
                cursor_ = c != nullptr && c->amount != 0 && c->amount != view_.Self() ? PlayerCursor(c->amount, alt) : 0x95;
                if (cursor_ == 0xA3) cursor_ = 0x95; // its owner's booth is no COS's
                hoveredEntity_ = picked.cos;
            } else if (GateUnder(m.x, m.y, w, h, ground ? &at : nullptr) != nullptr) {
                cursor_ = 0xA1;
            }
        }
    }
    // A window's pick mode (the Socket page's making and removing: 174, 175) wins, at once,
    // while its window shows (Escape hides it without a word).
    context.cursor = hudContext_.cursor != 0 && AlchemyShown(hudContext_) ? hudContext_.cursor : cursor_;
    // A service's pick: the cursor mode the original sets : the gender
    // switch tool's 0xA6 (pick a target item), the store's Repair 0x96 (the hammer);
    if (hudContext_.servicePick == 2) context.cursor = 0xA6;
    else if (hudContext_.servicePick == 1) context.cursor = 0x96;
}

// The safe trade's icon on the buff board while on (HUD icon kind 0x0B, icon_safetrade_progress.ddj, its one-line tooltip
// "Safe trade is in progress."); a world entry clears the board, so it is put back each frame.
void PlayScreen::SyncSafeTradeIcon(double now) {
    constexpr std::uint32_t kSafeTradeIcon = 0x53465452; // ours: the board's key for it
    auto& effects = player_.effects;
    // The bargain goods' lock-out (kind 6, icon_trade_timer.ddj, "Repurchase Time For Bargain Goods"; its gauge of
    // 600 s): on until its seconds run out (ours: the client's clock).
    {
        constexpr std::uint32_t kBargainIcon = 0x42524749; // ours: the board's key for it
        const auto b = std::find_if(effects.begin(), effects.end(),
                                    [](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::TimedJob && e.id == kBargainIcon; });
        if (now >= bargainUntil_) {
            if (b != effects.end()) effects.erase(b);
        } else if (b == effects.end()) {
            game::ActiveEffect icon;
            icon.kind = game::ActiveEffect::Kind::TimedJob;
            icon.id = kBargainIcon;
            icon.duration = 600.0;
            icon.started = bargainUntil_ - 600.0;
            icon.icon = "icon\\etc\\icon_trade_timer.ddj";
            icon.subject = "UIIT_STT_ICON_SPECIALITY_TIME";
            effects.push_back(std::move(icon));
        }
    }
    const auto it = std::find_if(effects.begin(), effects.end(),
                                 [](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::TimedJob && e.id == kSafeTradeIcon; });
    if (!safeTradeOn_) {
        if (it != effects.end()) effects.erase(it);
        return;
    }
    if (it != effects.end()) return;
    game::ActiveEffect icon;
    icon.kind = game::ActiveEffect::Kind::TimedJob;
    icon.id = kSafeTradeIcon;
    icon.icon = "icon\\etc\\icon_safetrade_progress.ddj";
    icon.subject = "UIIT_STT_SAFETRADE_PROGRESS";
    effects.push_back(std::move(icon));
}

void PlayScreen::Logout(const proto::LogoutAnswer& answer, double now) {
    const auto say = [&](const std::string& text) {
        // The warning banner and a system line, as the original posts its countdown.
        player_.Notify(game::Banner::Kind::Warning, text, now);
        player_.Post(game::GameMessage::Kind::General, text, now);
    };
    const UiData& d = gui_.data();
    switch (answer.status) {
    case proto::LogoutStatus::Counting:
        core::LogInfo("game: leaving in {} s", answer.seconds);
        exitAfterLogout_ = answer.kind == proto::LogoutKind::Exit;
        logoutSeconds_ = answer.seconds;
        logoutTick_ = now + 1.0;
        say(FormatText(d.Text("UIIT_MSG_LOGOUT_REMAIN_TIME"), {std::to_string(logoutSeconds_)}));
        break;
    case proto::LogoutStatus::Cancelled:
        logoutSeconds_ = 0;
        exitAfterLogout_ = false;
        say(d.Text("UIIT_MSG_LOGOUT_REMAIN_TIME_CANCLE"));
        break;
    case proto::LogoutStatus::InCombat:
        // 0x801: the banner only (error group 0x0F).
        exitAfterLogout_ = false;
        player_.Notify(game::Banner::Kind::Warning, d.Text("UIIT_MSG_LOGOUT_ERR_CANT_LOGOUT_IN_BATTLE_STATE"), now);
        break;
    case proto::LogoutStatus::Teleporting:
        exitAfterLogout_ = false;
        say(d.Text("UIIT_MSG_LOGOUT_ERR_CANT_LOGOUT_WHILE_TELEPORT_WORKING"));
        break;
    }
}

void PlayScreen::LogoutTick(double now) {
    // One less each second, said until it reaches nothing.
    if (logoutSeconds_ <= 0 || now < logoutTick_) return;
    logoutTick_ += 1.0;
    if (--logoutSeconds_ <= 0) return;
    const std::string text = FormatText(gui_.data().Text("UIIT_MSG_LOGOUT_REMAIN_TIME"), {std::to_string(logoutSeconds_)});
    player_.Notify(game::Banner::Kind::Warning, text, now);
    player_.Post(game::GameMessage::Kind::General, text, now);
}

const WorldScreen::StageLook* PlayScreen::LookOf(ScreenContext& context, const game::WorldView::Entity& e) {
    // A fortress's structure wears a model by its damage: destroyed
    // the third, whole the first, at 60 % of its health or less the second.
    int structureStage = -1;
    if (const proto::EntityStructure* s = fortress_.Structure(e.id)) {
        const float ratio = s->maxHp > 0 ? float(e.look.hp) / float(s->maxHp) : 0.0f;
        structureStage = (s->state & 1) != 0 || s->maxHp == 0 ? 2 : e.look.hp >= s->maxHp ? 0 : ratio <= 0.6f ? 1 : 0;
    }
    // Dressed again when what it wears changed.
    if (const auto it = looks_.find(e.id); it != looks_.end()) {
        if (lookVersions_[e.id] == e.lookVersion && (structureStage < 0 || structureStages_[e.id] == structureStage)) return &it->second;
        looks_.erase(it);
    }
    lookVersions_[e.id] = e.lookVersion;
    if (structureStage >= 0) structureStages_[e.id] = structureStage;
    if (!tables_->Usable()) return nullptr;
    std::vector<std::uint32_t> worn;
    std::vector<std::uint8_t> plus;
    for (const proto::EquippedItem& item : e.look.equipment) {
        worn.push_back(item.item);
        plus.push_back(item.plus);
    }
    // The dress side's avatars, drawn over the armour their visuals cover (assets::AvatarSlot).
    for (const proto::EquippedItem& item : e.look.avatars) {
        worn.push_back(item.item);
        plus.push_back(0);
    }
    // An NPC or a monster, or a player in a monster's shape: its
    // characterdata model as it is (dressed in it).
    const assets::ObjectRow* shape = tables_->characters.Find(e.look.model);
    // A gate standing as an object (the Forgotten World's gap): its teleportbuilding model.
    if (const assets::TeleportGate* gate = GapGate(e)) {
        assets::ObjectRow asRow;
        asRow.modelPath = gate->model;
        WorldScreen::StageLook look;
        look.model = assets::CharacterTable::ResolveModel(context.files, asRow);
        if (look.model.empty()) return nullptr;
        return &(looks_[e.id] = look);
    }
    // A bandit: a player's figure dressed by its spawn's byte, its row's size (a champion not
    // enlarged), without default garments.
    if (e.kind == proto::EntityKind::Monster && shape != nullptr && assets::IsBandit(*shape) && !tables_->banditBodies.Empty()) {
        const assets::SkillRow* skill = shape->skills[0] != 0 ? tables_->skills.Find(shape->skills[0]) : nullptr;
        const int weaponKind = skill != nullptr && skill->weaponKinds[0] != 255 ? skill->weaponKinds[0] : 2;
        // The imbue its first skill shows (a "magic" thief's cold or fire): worn as a buff while it is in view.
        if (const std::uint32_t imbue = skill != nullptr ? tables_->banditImbues.For(*skill) : 0u; imbue != 0) banditImbues_[e.id] = imbue;
        const assets::BanditOutfit outfit =
            assets::OutfitOfBandit(*shape, std::uint8_t(e.amount), weaponKind, tables_->banditBodies, tables_->characters, tables_->items);
        assets::DressedCharacter dressed;
        std::string error;
        if (outfit.body != 0 &&
            assets::Dress(context.files, tables_->characters, tables_->items, outfit.body, outfit.worn, 0, dressed, error, {}, nullptr, nullptr, false)) {
            WorldScreen::StageLook look;
            look.model = dressed.Key();
            look.stance = dressed.stance;
            look.height = float(shape->scalePercent) / 100.0f;
            look.volume = dressed.volume;
            return &(looks_[e.id] = look);
        }
        core::LogWarning("game: bandit {}: {}", shape->codename, error);
    }
    if (IsNpc(e) || e.kind == proto::EntityKind::Monster || (shape != nullptr && shape->typeId[1] == 2)) {
        const assets::ObjectRow* row = shape;
        std::string model = row != nullptr ? assets::CharacterTable::ResolveModel(context.files, *row) : std::string();
        if (structureStage >= 0 && row != nullptr) {
            if (const assets::AtStructLook* staged = tables_->atStructs.Find(row->codename); staged != nullptr && !staged->models[structureStage].empty()) {
                model = staged->models[structureStage];
            }
        }
        if (model.empty()) return nullptr;
        WorldScreen::StageLook look;
        look.model = model;
        look.height = float(row->scalePercent) / 100.0f;
        if (e.kind == proto::EntityKind::Monster) {
            // A monster's kind: a champion 1.5 times as big and in
            // material set 2 (when its own is 0 or 1), a giant 3 times, an elite 1.7; its own material
            // set otherwise (characterdata col 109; 2 counts as 0).
            const int own = row->materialKind == 2 ? 0 : row->materialKind;
            const int grade = e.rarity & 0x0F; // a party monster's by its grade
            const int set = grade == 1 && own <= 1 ? 2 : own;
            if (set != 0) look.model += "#set" + std::to_string(set);
            look.height *= grade == 1 ? 1.5f : grade == 4 ? 3.0f : grade == 6 ? 1.7f : 1.0f;
        }
        return &(looks_[e.id] = look);
    }
    assets::DressedCharacter dressed;
    std::string error;
    if (!assets::Dress(context.files, tables_->characters, tables_->items, e.look.model, worn, e.look.height, dressed, error, plus,
                       &tables_->equipGlows, tables_->visuals.Size() > 0 ? &tables_->visuals : nullptr)) {
        core::LogWarning("game: {}: {}", e.look.name, error);
        return nullptr;
    }
    WorldScreen::StageLook look;
    look.model = dressed.Key();
    // In berserk: the red hair and the horns, on any player's body (only China's files).
    if (berserkers_.contains(e.id)) {
        const bool woman = shape != nullptr && shape->codename.find("WOMAN") != std::string::npos;
        look.model += woman ? "|res\\char\\china\\chinawoman_hwan_hair.bsr" : "|res\\char\\china\\chinaman_hwan_hair.bsr";
    }
    look.stance = dressed.stance;
    look.height = dressed.height;
    look.volume = dressed.volume;
    return &(looks_[e.id] = look);
}

void PlayScreen::PlaceWalkers(ScreenContext& context) {
    std::erase_if(vanishing_, [&](const auto& v) { return context.time - v.second.since >= kFadeOutSeconds; });
    // Walkers gone (and faded): their slots free.
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (view_.Find(it->first) == nullptr && !vanishing_.contains(it->first)) {
            world_->ClearStage(it->second);
            slotUsed_[it->second] = false;
            looks_.erase(it->first);
            motions_.erase(it->first);
            combatStance_.erase(it->first);
            berserkers_.erase(it->first);
            killed_.erase(it->first);
            appeared_.erase(it->first);
            knocked_.erase(it->first);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    const double alpha = clock_.Alpha();
    // More walkers in view than slots (a server whose shares outgrow kSlots): the nearest take them,
    // chosen every half second; one already staged ranks 10% nearer, so the edge does not flicker.
    std::size_t walkers = 0;
    for (const auto& [id, e] : view_.Entities()) walkers += e.kind != proto::EntityKind::GroundItem && e.kind != proto::EntityKind::SkillObject;
    if (walkers + vanishing_.size() <= kWalkerSlots) {
        staged_.clear();
        stagedAt_ = -1.0;
    } else if (stagedAt_ < 0.0 || context.time - stagedAt_ >= 0.5) {
        stagedAt_ = context.time;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        const proto::FlatPosition me = self != nullptr ? proto::ToFlat(self->position) : proto::FlatPosition{};
        std::vector<std::pair<double, std::uint32_t>> ranked;
        ranked.reserve(walkers);
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind == proto::EntityKind::GroundItem || e.kind == proto::EntityKind::SkillObject) continue;
            const proto::FlatPosition p = proto::ToFlat(e.position);
            double d = std::hypot(p.x - me.x, p.z - me.z);
            if (slots_.contains(id)) d *= 0.9;
            ranked.push_back({id == view_.Self() ? -1.0 : d, id});
        }
        const std::size_t keep = kWalkerSlots - std::min(kWalkerSlots, vanishing_.size());
        std::nth_element(ranked.begin(), ranked.begin() + std::ptrdiff_t(keep), ranked.end());
        staged_.clear();
        for (std::size_t i = 0; i < keep; ++i) staged_.insert(ranked[i].second);
        // The ones that lost their place give it up at once (no fade: a nearer one takes it).
        for (auto it = slots_.begin(); it != slots_.end();) {
            if (!staged_.contains(it->first) && !vanishing_.contains(it->first)) {
                world_->ClearStage(it->second);
                slotUsed_[it->second] = false;
                appeared_.erase(it->first);
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto it = berserkWaiting_.begin(); it != berserkWaiting_.end();) {
        if (SlotOf(*it) == StageEffects::kNone) {
            it = view_.Find(*it) == nullptr ? berserkWaiting_.erase(it) : std::next(it);
            continue;
        }
        const std::uint32_t entity = *it;
        it = berserkWaiting_.erase(it);
        const auto mode = berserkers_.find(entity);
        BerserkEffects(entity, true, mode != berserkers_.end() ? mode->second : 0, false);
    }
    // Riders after everyone else: a rider sits on its mount's saddle as the mount stands this frame (SeatOf reads
    // the mount's slot, which SetStage moves at once), so its mount is staged first. In the map's own order a rider
    // took last frame's saddle whenever it came before its mount: a frame behind the horse, by turns (a stutter).
    stageOrder_.clear();
    for (const auto& entry : view_.Entities()) {
        if (!riders_.contains(entry.first)) stageOrder_.push_back(&entry);
    }
    for (const auto& entry : view_.Entities()) {
        if (riders_.contains(entry.first)) stageOrder_.push_back(&entry);
    }
    for (const auto* entry : stageOrder_) {
        const auto& [id, e] = *entry;
        if (e.kind == proto::EntityKind::GroundItem || e.kind == proto::EntityKind::SkillObject) continue; // SyncGround; SkillObjects
        if (!staged_.empty() && !staged_.contains(id)) continue;
        const WorldScreen::StageLook* dressed = LookOf(context, e);
        if (dressed == nullptr) continue;
        auto slot = slots_.find(id);
        if (slot == slots_.end()) {
            const auto walkers = slotUsed_.begin() + std::ptrdiff_t(std::min(kWalkerSlots, slotUsed_.size()));
            const auto free = std::find(slotUsed_.begin(), walkers, false);
            if (free == walkers) continue; // more walkers than slots: the rest wait
            *free = true;
            slot = slots_.emplace(id, std::size_t(free - slotUsed_.begin())).first;
            if (id != view_.Self()) appeared_[id] = context.time; // fades in (the player's own at once)
        }
        WorldScreen::StageLook look = *dressed;
        // Standing: its combat stance while that lasts (200 ms into either clip).
        const auto stance = combatStance_.find(id);
        // One in a monster's body (a mask) has no ATTREADY in it: it stands (the 1.188 client finds no clip in the body's
        // group and keeps what it plays).
        const bool monsterBody = look.model.find("\\mob\\") != std::string::npos || look.model.find("/mob/") != std::string::npos;
        look.state = e.moving ? kRun : !monsterBody && stance != combatStance_.end() && context.time < stance->second ? kAttReady : kStand;
        // A monster walks at its walk speed (wandering) and runs at its run speed (characterdata
        // cols 46, 47): the speed of its leg says which.
        if (e.moving && e.kind == proto::EntityKind::Monster) {
            if (const assets::ObjectRow* row = tables_->characters.Find(e.look.model); row != nullptr && e.speed <= (row->walkSpeed + row->runSpeed) * 0.5f) {
                look.state = kWalk;
            }
        }
        // A player's motion: its walk pace while it moves; still, sitting down (13),
        // sitting (14) or standing up (15).
        if (const auto m = entityMotions_.find(id); m != entityMotions_.end()) {
            if (e.moving) {
                if (m->second.walking) look.state = kWalk;
            } else if (m->second.motion == proto::kMotionSit) {
                look.state = kSitting;
            } else if (m->second.motion == proto::kMotionChanging) {
                look.state = m->second.to == proto::kMotionSit ? kSitDown : kStandUp;
            }
        }
        // A stall's owner in its booth: the vendor's clip (animation state 80 of the default set).
        if (entityStalls_.contains(id) && !e.moving) {
            look.state = proto::kStallVendorState;
            look.clipSet = "default";
        }
        // The player picking an item up: the PICK state's clip while it lasts (ours: its length is not
        // read from the clip yet).

        // The cyclic clip under whatever plays (the mixer's cyclic layer): standing,
        // the combat stance, running or walking, sitting, a booth's vendor; below, a stunned one's reel, a body knocked
        // down or dead its lying loop (a fall's or a death's one-shot ends on it, never on a standing pose).
        look.cycle = look.state;
        // Fighting: a swing while it lasts, over a walk or a slide too (the 1.188 client's one-shot owns the bones it
        // animates over the cyclic run, its own clock going on); the dead lie down.
        if (const auto m = motions_.find(id); m != motions_.end()) {
            Motion& motion = m->second;
            if (motion.shotFrom >= 0.0 && !motion.steps.empty()) {
                // The shot's clip in now: the swing lasts to its end.
                const Motion::Step& shot = motion.steps.back();
                if (const double clip = world_->StageClipSeconds(slot->second, shot.set, shot.state); clip > 0.0) {
                    motion.steps.back().until = motion.shotFrom + StageEffects::kClipHold + clip / double(motion.speed);
                    motion.shotFrom = -1.0;
                }
            }
            // Its trail: the hilt and the tip of what it holds (its ai_start / ai_end) each frame while on.
            if (motion.trailOn >= 0.0 && context.time >= motion.trailOn && context.time < motion.trailOff && !e.moving) {
                glm::mat4 hilt(1.0f), tip(1.0f);
                if (world_->StageBone(slot->second, "ai_start", hilt) && world_->StageBone(slot->second, "ai_end", tip)) {
                    if (motion.trail == 0) motion.trail = world_->Effects().StartTrail(motion.trailTexture, motion.trailColour, motion.trailSeconds);
                    if (motion.trail != 0) world_->Effects().TrailPoint(motion.trail, glm::vec3(hilt[3]), glm::vec3(tip[3]));
                }
            } else if (motion.trail != 0 && (context.time >= motion.trailOff || e.moving)) {
                world_->Effects().EndTrail(motion.trail);
                motion.trail = 0;
                motion.trailOn = -1.0;
            }
            if (context.time >= motion.End()) {
                if (motion.trail != 0) world_->Effects().EndTrail(motion.trail);
                motions_.erase(m);
            } else {
                // A dash's copies of its effect along its way, each at the place and heading reached (a short
                // one-shot of the loop, played out where it was left).
                if (motion.dash && !motion.dashEffect.empty()) {
                    const glm::vec3 here = world_->OriginRelative(e.position);
                    if (!motion.dashStarted) {
                        motion.dashLast = here;
                        motion.dashStarted = true;
                    }
                    constexpr float kDashCopyStep = 20.0f;
                    glm::vec3 way = here - motion.dashLast;
                    way.y = 0.0f;
                    const float length = glm::length(way);
                    for (float d = kDashCopyStep; d <= length; d += kDashCopyStep) {
                        const glm::vec3 at = motion.dashLast + way * (d / length);
                        gfx::EffectSystem::Request copy;
                        copy.path = motion.dashEffect;
                        copy.at = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(at.x, here.y, at.z)), e.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                        copy.seconds = 0.3f;
                        world_->Effects().Start(copy);
                    }
                    if (length >= kDashCopyStep) motion.dashLast += way * (std::floor(length / kDashCopyStep) * kDashCopyStep / length);
                }
                look.hideWeapon = motion.hideFrom >= 0.0 && context.time >= motion.hideFrom;
                // Its spine towards a target above or below: atan(height / distance), none within 15,
                // at most 30 degrees (ours: the roots' heights).
                if (const game::WorldView::Entity* t = motion.target != 0 && glm::length(motion.spineAxis) > 0.0f ? view_.Find(motion.target) : nullptr) {
                    const proto::FlatPosition a = proto::ToFlat(e.position), b = proto::ToFlat(t->position);
                    const double d = std::hypot(b.x - a.x, b.z - a.z);
                    const float angle = d < 15.0 ? 0.0f : std::clamp(float(std::atan(double(t->position.y - e.position.y) / d)), -0.5236f, 0.5236f);
                    look.spine = motion.spineAxis * angle;
                }
                for (std::size_t i = 0; i < motion.steps.size(); ++i) {
                    const Motion::Step& step = motion.steps[i];
                    if (context.time < step.until) {
                        look.state = step.state;
                        look.clipSet = step.set;
                        look.take = step.take;
                        look.speed = motion.speed;
                        // Standing, its wait loop is the cycle from the cast (P18: under READY, which blends out onto it), and
                        // under the shot while the shot blends in (no dip toward the stance); once the shot covers the body
                        // the cycle goes back to the stance under it, unseen, so the shot's end blends out onto the stance
                        // (the user saw the wait pose come back for a moment after Wolf's Thunderbolt's shot).
                        const bool shotCovers = i + 1 == motion.steps.size() && i > 0 &&
                                                context.time >= motion.steps[i - 1].until + StageEffects::kClipHold + 0.05;
                        if (motion.waitLoop && !e.moving && !shotCovers) {
                            look.cycle = motion.waitState;
                            look.cycleSet = motion.waitSet;
                        }
                        break;
                    }
                }
            }
        }
        // States and knocks: stunned it reels (STUN); frozen its pose holds;
        // knocked down it falls (DOWN), lies (DOWN_RM) and rises (DOWN_UP) by the end of its time;
        // knocked back it staggers (DAMAGE2) while it slides. A knock shows over its slide's run.
        if (InState(id, game::StateKind::Stun, context.time)) {
            look.state = kStun;
            look.take = 0;
            look.cycle = kStun;
            look.cycleSet.clear();
        }
        look.held = InState(id, game::StateKind::Freeze, context.time);
        // Its growth (berserk's SCT_CHAR_SCALE), gone once back to its size.
        if (const auto g = grows_.find(id); g != grows_.end()) {
            look.grow = g->second.At(context.time);
            if (g->second.to == 1.0f && look.grow == 1.0f) grows_.erase(g);
        }
        bool down = false;
        if (const auto k = knocked_.find(id); k != knocked_.end()) {
            const Knocked& kn = k->second;
            if (context.time >= kn.until) {
                knocked_.erase(k);
            } else if (kn.kind == proto::KnockKind::Down) {
                down = true;
                const bool hurt = motions_.contains(id) && motions_[id].steps.back().state == kDownDamage && context.time < motions_[id].End();
                look.state = context.time - kn.since < kDownFallSeconds ? kDown : context.time >= kn.until - kDownRiseSeconds ? kDownUp : hurt ? kDownDamage : kDownLoop;
                look.take = kn.take;
                look.cycle = kDownLoop;
                look.cycleSet.clear();
            } else if (const game::WorldView::Entity* who = view_.Find(id); who == nullptr || who->kind != proto::EntityKind::Character || who->look.name.empty()) {
                // A monster knocked back staggers (DAMAGE2) while it slides when nothing else plays (ours: its 4-bone
                // flinch is a player character's); a player character's clip goes on (P5).
                if (!motions_.contains(id)) {
                    look.state = context.time - kn.since < 0.5 ? kDamage2 : kStand;
                    look.take = kn.take;
                }
            }
        }
        if (IsDead(e)) {
            // DIE1 when seen falling, then resting in DIE1_RM (as the original's); DIE1_RM alone for
            // one already lying when it came into view; DOWN_DIE for one that died lying.
            const auto fell = killed_.find(id);
            look.state = fell != killed_.end() && context.time - fell->second < 3.0 ? (down ? kDownDie : kDie) : kDieLoop;
            look.take = 0;
            look.held = false;
            look.cycle = kDieLoop;
            look.cycleSet.clear();
        }
        glm::vec3 at = world_->OriginRelative(game::WorldView::Drawn(e, alpha));
        float yaw = e.yaw;
        // A rider: on its mount's saddle, in its riding pose (PlayCos.cpp).
        if (SeatOf(id, at, yaw)) {
            look.clipSet = "cart";
            look.state = 0;
            look.take = 0;
        }
        // Its flinch (P1): a one-shot the mixer averages with a swing's (half and half on its bones).
        if (const auto f = flinches_.find(id); f != flinches_.end()) look.flinch = f->second;
        world_->SetStage(slot->second, look, at.x, at.y, at.z, yaw);
        // Fading in as it appears, from transparent over 2 s, as the original's characters do (the player's own at once).
        float opacity = 1.0f;
        if (const auto a = appeared_.find(id); a != appeared_.end()) {
            opacity = float(std::clamp((context.time - a->second) / kFadeInSeconds, 0.0, 1.0));
            if (opacity >= 1.0f) appeared_.erase(a);
        }
        // Hidden (a stealth, an invisibility) yet shown here (itself, or seen by a detector): see-through
        // (ours: the original draws it by its body mode).
        if (Hidden(id)) opacity = std::min(opacity, 0.45f);
        if (id == view_.Self()) opacity = std::min(opacity, ownFade_); // looking steeply up
        world_->SetStageOpacity(slot->second, opacity);
    }
    // The ones fading out: standing (or lying) where they were.
    for (const auto& [id, v] : vanishing_) {
        const auto slot = slots_.find(id);
        const WorldScreen::StageLook* dressed = slot != slots_.end() ? LookOf(context, v.entity) : nullptr;
        if (dressed == nullptr) continue;
        WorldScreen::StageLook look = *dressed;
        look.state = IsDead(v.entity) ? kDieLoop : kStand;
        const glm::vec3 at = world_->OriginRelative(v.entity.position);
        world_->SetStage(slot->second, look, at.x, at.y, at.z, v.entity.yaw);
        world_->SetStageOpacity(slot->second, float(std::clamp(1.0 - (context.time - v.since) / kFadeOutSeconds, 0.0, 1.0)));
    }
}

void PlayScreen::CharacterView(ScreenContext& context) {
    // The player's character, dressed as in the world, standing at its own place in the overlay
    // (near the camera: the fog stays away), turned by the window's buttons; a yaw of 0 faces the
    // view's camera (inferred: the original's light shines from the camera's side).
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const WorldScreen::StageLook* dressed = self && shown_ ? LookOf(context, *self) : nullptr;
    viewShown_ = hudContext_.characterView.shown && dressed != nullptr;
    if (!viewShown_) {
        world_->ClearStage(kViewSlot);
        return;
    }
    WorldScreen::StageLook look = *dressed;
    // The skin change window's chosen look (HudServices.cpp): the character as another figure and shape; undressed when
    // of the other sex (its armour would not fit).
    if (hudContext_.characterView.model != 0) {
        game::WorldView::Entity preview = *self;
        preview.id = kPreviewId;
        preview.look.model = hudContext_.characterView.model;
        preview.look.height = hudContext_.characterView.shape;
        const auto mine = tables_ && tables_->Usable() ? tables_->rules.creation.FigureOf(self->look.model) : std::nullopt;
        const auto theirs = tables_ && tables_->Usable() ? tables_->rules.creation.FigureOf(preview.look.model) : std::nullopt;
        if (mine && theirs && mine->male != theirs->male) preview.look.equipment.clear(), preview.look.avatars.clear();
        preview.lookVersion = (preview.look.model * 2654435761u) ^ preview.look.height ^ (preview.look.equipment.empty() ? 0x80000000u : 0u);
        if (const WorldScreen::StageLook* other = LookOf(context, preview)) look = *other;
    }
    look.overlay = true;
    look.shadow = false;
    look.state = kStand;
    const glm::vec3 at = world_->OriginRelative(game::WorldView::Drawn(*self, clock_.Alpha()));
    world_->SetStage(kViewSlot, look, at.x, at.y, at.z, 3.14159265f + hudContext_.characterView.yaw);
}

void PlayScreen::CharacterViewCamera(ScreenContext& context, FrameSnapshot& snapshot) {
    // The original's view: looking at 9 above the feet from 40 away, a 30 degree
    // field of view, into the window's rect; our overlay target is the screen's, so the projection is
    // squeezed into the rect and the window draws that part of it.
    snapshot.scene.overlayOwnCamera = true;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const ui::Rect r = hudContext_.characterView.rect;
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    if (!viewShown_ || self == nullptr || r.w <= 0 || r.h <= 0 || width <= 0 || height <= 0) return;
    const glm::vec3 at = world_->OriginRelative(game::WorldView::Drawn(*self, clock_.Alpha()));
    const glm::vec3 target = at + glm::vec3(0.0f, 9.0f, 0.0f), eye = target + glm::vec3(0.0f, 0.0f, -40.0f);
    const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(30.0f * 3.14159265f / 180.0f, r.w / r.h, 1.0f, 5000.0f);
    proj[1][1] *= -1.0f;
    glm::mat4 into(1.0f);
    into[0][0] = r.w / width;
    into[1][1] = r.h / height;
    into[3][0] = (2.0f * r.x + r.w) / width - 1.0f;
    into[3][1] = (2.0f * r.y + r.h) / height - 1.0f;
    const glm::mat4 viewProj = into * proj * view;
    std::copy(glm::value_ptr(viewProj), glm::value_ptr(viewProj) + 16, snapshot.scene.overlayViewProj.begin());
}

// How fast the camera's point closes on the character, per second: at a run of about 50 units a
// second it trails some 8 units behind, and settles in under a second once the walk stops.
constexpr double kCameraFollowRate = 6.0;

void PlayScreen::FollowCamera() {
    // A quest scene's camera (PlayScene.cpp), shaken as the follow camera is.
    if (assets::CameraKey key; world_ && QuestSceneCamera(float(cameraNow_ - scene_.started), key)) {
        if (const double now = cameraNow_; now < shake_.until && shake_.amplitude > 0.0f) {
            if (now >= shake_.next) {
                shake_.next = now + shake_.step;
                const float left = float((shake_.until - now) / std::max(1e-3, shake_.until - shake_.from));
                for (float& o : shake_.offset) {
                    shake_.random = shake_.random * 1664525u + 1013904223u;
                    o = (float(shake_.random >> 8) / float(1u << 24) * 2.0f - 1.0f) * shake_.amplitude * 0.01f * left;
                }
            }
            for (int i = 0; i < 3; ++i) key.position[i] += shake_.offset[i];
        }
        ApplyCameraKey(*world_, key, originX_, originY_);
        return;
    }
    // The skill bench follows the caster of the skill being shown (the European's too), from the end of the one before.
    std::uint32_t subject = view_.Self();
    if (replay_ && !bench_.steps.empty()) {
        std::size_t i = 0;
        while (i + 1 < bench_.steps.size() && replayTime_ >= bench_.steps[i].until) ++i;
        subject = bench_.steps[i].caster;
    }
    const game::WorldView::Entity* self = view_.Find(subject);
    if (self == nullptr) self = view_.Find(view_.Self());
    if (self == nullptr) return;
    proto::WorldPosition at = game::WorldView::Drawn(*self, clock_.Alpha());
    if (world_) world_->SetFocus(at); // a dungeon shows the block of the player's floor
    // The camera's point eases after the character (ours): pinned to it, every start, stop and
    // correction of the walk reached the view in the same frame and read as a stutter. A jump
    // (a teleport, the first frame) puts it there at once.
    {
        const glm::dvec3 want(double(at.region & 0xFF) * assets::kRegionSize + at.x, double(at.y),
                              double(at.region >> 8) * assets::kRegionSize + at.z);
        const glm::dvec3 gap = want - cameraFocus_;
        if (!cameraFocusSet_ || glm::dot(gap, gap) > 150.0 * 150.0 || benchView_ != 0) {
            cameraFocus_ = want;
            cameraFocusSet_ = true;
        } else {
            cameraFocus_ += gap * (1.0 - std::exp(-double(cameraDt_) * kCameraFollowRate));
        }
        const glm::dvec3 lag = cameraFocus_ - want;
        at.x += float(lag.x);
        at.y += float(lag.y);
        at.z += float(lag.z);
    }
    // A shake: a jolt of the point the camera looks from, changed every step, fading over its time.
    if (const double now = cameraNow_; now < shake_.until && shake_.amplitude > 0.0f) {
        if (now >= shake_.next) {
            shake_.next = now + shake_.step;
            const float left = float((shake_.until - now) / std::max(1e-3, shake_.until - shake_.from));
            for (float& o : shake_.offset) {
                shake_.random = shake_.random * 1664525u + 1013904223u;
                o = (float(shake_.random >> 8) / float(1u << 24) * 2.0f - 1.0f) * shake_.amplitude * 0.01f * left;
            }
        }
        at.x += shake_.offset[0];
        at.y += shake_.offset[1];
        at.z += shake_.offset[2];
    }
    const core::Sight sight = Sight();
    lastSight_ = sight; // the quarter view keeps the pitch it had (the original's leaves it)
    // Third person: the camera behind the character's heading at all times (the original's view 1).
    if (sight == core::Sight::ThirdPerson) cameraYaw_ = self->yaw;
    // Looking steeply up (under -0.9) the player's own character fades out, back in above (the original's, over 0.5 s),
    // so it never hides the sky.
    {
        const float want = cameraPitch_ < -0.9f ? 0.0f : 1.0f;
        const float step = cameraDt_ * 2.0f;
        ownFade_ = want < ownFade_ ? std::max(want, ownFade_ - step) : std::min(want, ownFade_ + step);
    }
    if (benchView_ == 1) {
        cameraYaw_ = self->yaw - 1.5708f; // the open side of the bench's spot
        cameraDistance_ = 130.0f;
        cameraPitch_ = 0.45f;
    } else if (benchView_ == 2) {
        cameraYaw_ = self->yaw - 1.1f; // near, before its right side: the hands and what they hold
        cameraDistance_ = 32.0f;
        cameraPitch_ = 0.12f;
    }
    // Where it looks (the original's): 14.2 over the feet at 150, rising to the head as it comes near (10), by the
    // line through both (on to our 300 the same way); the head is the model's height less 3.8. A rider's is its
    // mount's: the mount's height less 13 over its feet, and a quarter of that height for the head.
    float lookBase = at.y, head = 14.2f;
    if (world_) {
        if (const auto mount = riders_.find(subject); mount != riders_.end()) {
            if (const auto m = slots_.find(mount->second); m != slots_.end()) {
                if (const float height = world_->StageHeight(m->second); height > 1.0f) {
                    lookBase = at.y + height - 13.0f;
                    head = height * 0.25f;
                }
            }
        } else if (const auto s = slots_.find(subject); s != slots_.end()) {
            if (const float height = world_->StageHeight(s->second); height > 1.0f) head = height - 3.8f;
        }
    }
    const float lookY = lookBase + 14.2f + (cameraDistance_ - 150.0f) * ((14.2f - head) / 140.0f);
    // Looking up (a negative pitch) brings it in toward 40 straight up (the original's: from 150 at level; ours from the
    // wheel's distance when that is farther).
    float wanted = cameraDistance_;
    if (cameraPitch_ < 0.0f) {
        const float u = std::min(1.0f, -cameraPitch_ / kCameraPitchLimit);
        const float from = std::max(150.0f, cameraDistance_);
        wanted = std::min(cameraDistance_, (1.0f - std::sin(1.5707964f * u)) * (from - 40.0f) + 40.0f);
    }
    // The world between the point and the camera pulls it in, 5 units short of the hit (the
    // original's); it eases back out when the way clears.
    float distance = wanted;
    if (world_) {
        const float fx = std::cos(cameraPitch_) * std::sin(cameraYaw_), fy = -std::sin(cameraPitch_), fz = std::cos(cameraPitch_) * std::cos(cameraYaw_);
        const float origin[3] = {at.x, lookY, at.z};
        const float back[3] = {-fx, -fy, -fz};
        game::NavHit hit;
        if (world_->Navigation().Ray(at.region, origin, back, wanted, hit)) distance = std::clamp(hit.distance - 5.0f, 5.0f, wanted);
        // The spring (ours): never inside an object either; in front of the box it would stand in, 5 units short, and
        // back out (cameraShown_ below) when the way clears.
        const glm::vec3 point(at.x + float((at.region & 0xFF) - originX_) * assets::kRegionSize, lookY,
                              at.z + float((at.region >> 8) - originY_) * assets::kRegionSize);
        const glm::vec3 eye = point + glm::vec3(back[0], back[1], back[2]) * distance;
        if (const float limit = world_->CameraLimit(point, eye); limit < 1.0f) distance = std::clamp(limit * distance - 5.0f, 5.0f, distance);
    }
    cameraShown_ = distance < cameraShown_ ? distance : cameraShown_ + (distance - cameraShown_) * std::min(1.0f, cameraDt_ * 4.0f);
    // A key at the look point, `distance` behind it along (pitch, yaw); positive pitch looks down.
    const assets::CameraKey key = MakeKey(0.0f, at.region & 0xFF, at.region >> 8, at.x, lookY, at.z, cameraPitch_, cameraYaw_, cameraShown_);
    ApplyCameraKey(*world_, key, originX_, originY_);

}

void PlayScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!shared_) {
        context.holdCapture = true;
        return;
    }
    SR_ZONE("game: update");
    now_ = context.time;
    if (quitNow_) {
        // The lost connection's box answered: the game ends there (the original's).
        context.quit = true;
        return;
    }
    if (!snapName_.empty()) {
        // A script's snap:NAME (ours, for live runs): this frame saved, the run going on.
        snapshot.capture = std::filesystem::path(context.config.GetString("snap-dir", ".")) / (snapName_ + ".png");
        snapshot.captureGoesOn = true;
        snapName_.clear();
    }
    if (benchPending_) {
        if (!tables_ || !tables_->Ready()) {
            context.holdCapture = true;
            return;
        }
        benchPending_ = false;
        std::string error;
        game::SkillBenchOptions benchOptions;
        benchOptions.weaponKind = context.config.GetInt("bench-weapon", 0);
        benchOptions.reach = float(context.config.GetInt("bench-reach", 0));
        benchOptions.state = context.config.GetInt("bench-state", 0);
        benchOptions.casterState = context.config.GetInt("bench-caster-state", 0);
        benchOptions.plus = static_cast<std::uint8_t>(std::clamp(context.config.GetInt("bench-plus", 0), 0, 255));
        benchOptions.weapon = context.config.GetString("bench-item", "");
        benchOptions.impactDelayMs = context.config.GetInt("bench-impact-delay", 0);
        // A video's skill rests 1.5 s after its time, so what it leaves (effects, the body's settling) is in its video.
        benchOptions.pauseSeconds = context.config.GetDouble("bench-pause", benchVideo_.empty() ? 0.0 : 1.5);
        // --bench-part K/N: the K-th of N parts (1-based), every N-th skill.
        if (const std::string part = context.config.GetString("bench-part", ""); !part.empty()) {
            const std::size_t slash = part.find('/');
            const int k = std::atoi(part.c_str()), n = slash != std::string::npos ? std::atoi(part.c_str() + slash + 1) : 1;
            if (n >= 1 && k >= 1 && k <= n) {
                benchOptions.part = k - 1;
                benchOptions.parts = n;
            }
        }
        if (!tables_->Usable() || !game::MakeSkillBench(tables_->skills, tables_->items, tables_->characters, benchFilter_, bench_, error, benchOptions)) {
            core::LogError("bench: {}", tables_->Usable() ? error : std::string("the tables did not load"));
            context.quit = true;
            context.exitCode = 1;
            return;
        }
        recording_ = bench_.recording;
        replay_ = std::make_unique<game::WorldReplay>(recording_);
        replayTime_ = 0.0;
        benchFailedBefore_ = context.assets.GetStats().failed;
        core::LogInfo("bench: {} skills ({}), {:.0f} s", bench_.steps.size(), benchFilter_, recording_.Duration());
    }
    if (replay_) {
        if (!world_) {
            for (game::TitleEvent& event : replay_->TakeUntil(replayTime_ + 1e-6)) HandleEvent(context, event); // the entry
        }
    } else {
        shared_->session->Update();
        HandleEvents(context);
        // A walk clicked during the player's own swing goes the moment it ends.
        if (heldMove_ && ((!OwnMoveHeld(context.time) && context.time >= actionSentAt_ + 0.5) || context.time >= heldUntil_)) {
            if (heldSkill_ != 0) {
                core::LogInfo("game: the teleport chosen during the swing goes now ({:.2f} s held)", context.time - heldSince_);
                shared_->session->UseSkill(heldSkill_, 0, &*heldMove_);
                actionSentAt_ = context.time;
                heldSkill_ = 0;
            } else {
                core::LogInfo("game: the walk clicked during the swing goes now ({:.2f} s held)", context.time - heldSince_);
                WalkTo(context, *heldMove_, heldSky_);
            }
            heldMove_.reset();
        }
    }
    if (arrived_ && next.empty()) Arrive(context, snapshot);
    if (!next.empty()) {
        context.holdCapture = true;
        return;
    }
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    list_.Clear();
    if (!world_) {
        // Waiting for the server's word on the character: black.
        snapshot.clearColor = {0, 0, 0, 1};
        context.holdCapture = true;
        return;
    }
    // The simulation in fixed steps; drawing between the last two. A replay waits for the world.
    if (replay_) {
        if (shown_) StepReplay(context, clock_.Advance(context.dt));
        if (arrived_) Arrive(context, snapshot);
        if (shown_ && !bench_.steps.empty()) Bench(context, snapshot);
    } else {
        const int steps = clock_.Advance(context.dt);
        for (int i = 0; i < steps; ++i) view_.Step(clock_.StepSeconds());
    }
    ReleaseHits(context);
    // The camera once a frame, final before anything is drawn against it: the names, hit numbers and
    // speech the interface lays over the scene project with the same camera the scene renders with.
    StepQuestScene(context);
    CameraInput(context);
    FollowCamera();
    if (shown_) Interface(context, snapshot);
    if (shown_) PickCursor(context);
    HandleInput(context);
    if (shown_) Script(context);
    if (shown_) {
        Gates(context);
        Npcs(context.time);
        StallVisits(context.time);
        SyncGround(context.time);
        StageProps(context.time);
        SyncBooths();
    }
    if (shown_ && gotoPoint_ != 0 && !replay_) {
        core::LogInfo("game: --goto {}", gotoPoint_);
        shared_->session->DevGoto(std::exchange(gotoPoint_, 0u));
    }
    if (shown_ && context.toolsVisible) DevTools(context);
    if (holdForGate_ && pendingGate_ == 0 && !arrived_) holdForGate_ = waitingForGate_; // given up: nothing to wait for
    if (holdForGate_) context.holdCapture = true;
    PlaceWalkers(context);
    PlaceQuestSceneActors(context);
    // A quest scene's INVISIBLE_USER: the player's own figure hidden while it plays.
    if (scene_.active && scene_.hideSelf && world_) {
        if (const std::size_t self = SlotOf(view_.Self()); self != StageEffects::kNone) world_->SetStageOpacity(self, 0.0f);
    }
    PlaceZoneMarkers(context);
    FlyEffects(context);
    KeptEffects(context);
    AutoRun(context);
    RefreshTarget(context.time);
    DeathWindow(context.time, false);
    // A guild war's last minute: "Until the end of war (%d seconds left)" on the warning banner each second (60 first).
    if (guildWarEndAt_ >= 0.0) {
        const int left = int(std::ceil(guildWarEndAt_ - context.time));
        if (left <= 0) {
            guildWarEndAt_ = -1.0;
        } else if (left != guildWarEndShown_) {
            guildWarEndShown_ = left;
            player_.Notify(game::Banner::Kind::Warning, FormatText(gui_.data().Text("UIIT_MSG_GUILDWAR_END_COUNTDOWN"), {std::to_string(left)}), context.time);
        }
    }
    FlagFrame(context);
    Listen(context);
    CharacterView(context);
    if (shown_) GroundDecals(context); // the walkers placed: the ring follows the selection's drawn place
    world_->Update(context, snapshot);
    CharacterViewCamera(context, snapshot);
    UpdatePortraits(context, snapshot);

    // The world map's tiles and pictures warmed up (gui/UiWarmup.h; ~22 MB, then kept by the store under its budget):
    // dragged about, the map never shows a tile still loading. Once, as soon as the map's table is in; the loading
    // screen does not wait for them.
    if (!worldMapWarmed_ && context.uiWarmup != nullptr) {
        if (const assets::WorldMapData* maps = gui_.data().WorldMap()) {
            std::vector<std::string> paths;
            for (const assets::WorldMapRow& m : maps->maps) {
                if (!m.tiled) {
                    if (!m.picture.empty()) paths.push_back(m.picture);
                    continue;
                }
                for (int i = 0; i < m.TilesAcross(); ++i) {
                    for (int j = 0; j < m.TilesDown(); ++j) paths.push_back(m.Tile(i, j));
                }
            }
            // Its pictures on the maps (the shops' and gates' signs, the places' art) and the view's corners
            // (HudMapView.cpp): the window's first opening drew them late.
            for (const assets::WorldMapLabel& l : maps->labels) {
                if (l.picture && !l.what.empty()) paths.push_back(l.what);
            }
            for (const char* corner : {"interface\\worldmap\\wmap_window_edge_1.ddj", "interface\\worldmap\\wmap_window_edge_2.ddj",
                                       "interface\\worldmap\\wmap_window_edge_3.ddj", "interface\\worldmap\\wmap_window_edge_4.ddj"}) {
                paths.emplace_back(corner);
            }
            context.uiWarmup->Preload(paths);
            worldMapWarmed_ = true;
        }
    }
    const auto self = slots_.find(view_.Self());
    const bool worldIn = world_->Settled() && self != slots_.end() && world_->StageReady(self->second);
    if (!shown_ && worldIn) {
        // The interface's warm-up in too, so the HUD shows whole from its first frame; a few seconds at most past the
        // world (it began at the program's start: it is nearly always in long before).
        if (worldInAt_ < 0.0) worldInAt_ = context.time;
        const bool uiIn = context.uiWarmup == nullptr || context.uiWarmup->ListReady() || context.time - worldInAt_ >= kWarmupWaitSeconds;
        if (uiIn) {
            shown_ = true; // the town, the character and the interface in: the world fades in
            worldInAt_ = -1.0;
            loading_.End(context.assets);
        }
    }
    if (!shown_) {
        // The loading screen over a black frame; the world streams behind it.
        snapshot.scene.active = false;
        snapshot.clearColor = {0, 0, 0, 1};
        loading_.SetProgress(world_->StreamProgress(context.assets));
        loading_.Draw(context.assets, list_, width, height);
        AppendUi(snapshot.ui, list_);
        context.holdCapture = true;
        return;
    }
    black_ = std::max(0.0f, black_ - float(context.dt) / kFadeSeconds);
    if (black_ > 0.0f) {
        list_.SetWhite(ui::TextureRef{0, false, false}, {0.5f, 0.5f});
        list_.Fill({0, 0, width, height}, ui::Rgba(0, 0, 0, std::uint8_t(255.0f * black_)));
        AppendUi(snapshot.ui, list_);
        context.holdCapture = true;
    }
#ifdef SR_BUG_REPORTS
    BugReportFrame(context, snapshot); // last: its screenshot's frame may leave the interface out
#endif
}

} // namespace sr::client
