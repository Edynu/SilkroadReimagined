#pragma once

// The world after the select (C6c): the session asks the
// game server to enter, the loading screen shows until the town around the character and the
// character itself are in, then the world fades in. The server's word on who is where comes as
// events into game::WorldView, which walks everyone along their legs in fixed steps (30 a
// second); each walker stands on a stage slot of the world screen, dressed (assets::Dress),
// running while it walks. A left click on the ground asks the server to go there; the camera follows the character
// (right mouse drag turns it, the wheel zooms; its numbers are ours until the original's follow camera is known). Over
// the world, the HUD (C7c,
// gui/Hud.h): the original's windows; the world takes clicks only where no window is; Escape goes
// through the HUD (the system menu's Restart leaves the world for the character select).
//
// Test options: --walk dx,dz (back and forth from the arrival), --click fx,fy (a click on that
// point of the screen, in fractions), --leave-after S, --logout-after S (the system menu's Restart:
// the server's countdown, then the select), --chat "line|line" [--chat-after S] (typed into the
// chat and sent, 2 s apart; "$Name text" whispers), --inventory (the inventory page open),
// --mouse x,y (the mouse held there), --move-item from,to (one item move; places 0..12 worn,
// 13.. the bag), --use-gate ID (walk to that gate and use
// it), --gate-to N (the point a gate is asked for; otherwise the first it leads to), --gate-menu
// (with --use-gate: the gate's menu instead, C7i) [--gate-choose "2,1,y"], --goto N
// (a developer's teleport to point N, as the F1 tools' Teleport panel; servers started with
// --dev-commands only). K9's tries: --die-after S (a developer's death, those servers only),
// --accept-revival (a resurrection offered is accepted), --revive "skill,S" (that skill cast on the
// nearest other player after S seconds).
//
// Teleports (C6e): a click near a gate walks to it; standing there, the character
// asks to go where the gate leads (the original's destination window comes with the interface,
// C7); the server's new WorldEntered rebuilds the world at the destination behind the teleport's
// loading screen.
//
// The session recorder (C6d): --record FILE keeps the world's messages and saves them on leaving;
// --replay FILE (on --screen game) plays one back without a server, the camera on its character.
//
// The skill bench (K1): --skill-bench FILTER (all, a mastery's name key, part of
// a codename) plays a made-up session of those skills cast one after another (game/SkillBench.h),
// each with its weapon in hand; --bench-captures DIR saves a picture of each (--bench-burst N[,S]: N
// pictures S seconds apart from its cast, to see an effect move); at its end it reports
// how the clips were found and the files that failed, and quits (exit 3 when anything was missing).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frame/GameUi.h"
#include "frame/Portraits.h"
#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "gui/InterfaceHost.h"
#include "screens/Screen.h"
#include "screens/WorldScreen.h"
#include "sr/assets/Gacha.h"
#include "sr/assets/IntroScript.h"
#include "sr/assets/RegionInfo.h"
#include "sr/game/FlagView.h"
#include "sr/game/NavPath.h"
#include "sr/game/Mall.h"
#include "sr/game/PlayerState.h"
#include "sr/game/SimulationClock.h"
#include "sr/game/SkillBench.h"
#include "sr/game/States.h"
#include "sr/game/WorldRecording.h"
#include "sr/game/WorldView.h"
#include "sr/ui/DrawList.h"
#include "title/TitleShared.h"
#include "world/LoadingScreen.h"
#include "world/StageEffects.h"

namespace sr::client {

class PlayScreen final : public Screen {
public:
    const char* Name() const override { return "game"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void HandleEvents(ScreenContext& context);
    void HandleEvent(ScreenContext& context, game::TitleEvent& event);
    // The world screen around where the server put the character (an entry or a teleport).
    void Arrive(ScreenContext& context, FrameSnapshot& snapshot);
    // A click near a gate: walk there, then ask for one of the places it leads to.
    void Gates(ScreenContext& context);
    const assets::TeleportGate* GateNear(const game::proto::WorldPosition& at, float reach) const;
    // A gate the player can meet where it is: outdoors the outdoor ones, in a dungeon those of its own region (the
    // original has only the gates its server spawns about the player).
    bool GateHere(const assets::TeleportGate& g) const;
    void StepReplay(ScreenContext& context, int steps);
    void Bench(ScreenContext& context, FrameSnapshot& snapshot);
    void HandleInput(ScreenContext& context);
    void ClickWorld(ScreenContext& context, float x, float y, bool ctrl, bool alt = false);
    // The HUD over the world: its frame, its keys and what they ask of the game.
    void Interface(ScreenContext& context, FrameSnapshot& snapshot);
    void HudRequests(ScreenContext& context);
    // A skill used (its quick slot, its right click in the S window): its wait, its target, the action list's hold.
    void UseSkill(ScreenContext& context, std::uint32_t skill);
    void PlaceWalkers(ScreenContext& context);
    // The follow camera in the player's sight: Free; Third person (it swings
    // behind the character while it walks); Quarter view (the pitch held). Pulled in where the
    // world stands between the character and the camera (the original's ray, 5 units short).
    void FollowCamera(); // once a frame (it eases the swing and the pull-in), after CameraInput
    // A quest scene (PlayScene.cpp; the original's 0x3CA2): started, stepped once a frame (before the
    // camera), its camera at `t` seconds in (false: none, the follow camera), ended, its cinema bars drawn.
    void PlayQuestScene(ScreenContext& context, const std::string& name);
    void StepQuestScene(ScreenContext& context);
    bool QuestSceneCamera(float t, assets::CameraKey& key) const;
    void EndQuestScene();
    void DrawQuestSceneBars(ScreenContext& context, ui::Painter& paint);
    void CameraInput(ScreenContext& context);
    core::Sight Sight() const { return sight_ != nullptr ? *sight_ : core::Sight::Free; }
    const WorldScreen::StageLook* LookOf(ScreenContext& context, const game::WorldView::Entity& e);

    std::shared_ptr<TitleShared> shared_;
    std::shared_ptr<CharacterData> tables_;
    std::uint32_t orbsSent_ = 0; // varies each orb's pause and curve
    std::unique_ptr<WorldScreen> world_;
    int originX_ = 0, originY_ = 0; // the world screen's origin region
    game::proto::WorldEntered arrival_; // the last entry, until Arrive builds the world for it
    bool arrived_ = false;
    // The interface's warm-up (gui/UiWarmup.h): the loading screen waits for it this long at most once the world is in.
    static constexpr double kWarmupWaitSeconds = 3.0;
    double worldInAt_ = -1.0;      // when the world was in while the warm-up was not (-1: not waiting)
    bool worldMapWarmed_ = false;  // the world map's tiles were handed to the warm-up
    // The gate the character walks to (its reference id), and the destination asked of it.
    std::uint32_t pendingGate_ = 0;
    std::uint32_t gateTo_ = 0; // --gate-to N: a teleportdata id (0: the first the gate leads to)
    int gateTries_ = 0;        // --use-gate: walks toward the gate before giving up
    // The gate under a screen point: its crystal, or the ground near its spot (`ground`: the ground
    // under the point, null when none).
    const assets::TeleportGate* GateUnder(float x, float y, float width, float height, const game::proto::WorldPosition* ground) const;
    bool talkAtGate_ = false;  // a click: at the gate, its menu (C7i); --use-gate goes through at once
    std::optional<game::proto::GateMenu> gateMenu_;
    std::optional<game::proto::NpcMenu> npcMenu_;
    // NPCs (C7i): the one clicked, talked to once the character stands near it.
    std::uint32_t pendingNpc_ = 0;
    bool storageShow_ = false;
    // The guild's storage: its window to show, and held (told when put away).
    bool guildStorageShow_ = false, guildStorageHeld_ = false;
    std::uint32_t shopShow_ = 0; // a merchant whose store opens once the HUD is made
    int shopGroup_ = 0;          // the tab group picked in its menu
    bool goldBox_ = false; // --gold-box
    bool IsNpc(const game::WorldView::Entity& e) const;
    bool IsPlayer(const game::WorldView::Entity& e) const; // a character with a name that is no NPC
    // Another player under (x, y), living or dead (its model's box, as the original's), and its selection. The picks
    // below cast the original's nine rays; `centre` says the hit is the cursor's own ray's (it beats the others' rays).
    std::uint32_t PlayerUnder(float x, float y, float width, float height, float* distance = nullptr, bool* centre = nullptr) const;
    void SelectPlayer(std::uint32_t entity);
    // Monsters (C10a): the one under a point (its model's box; a fortress structure's too), selected into the
    // target window (its health kept up to date while it is in view).
    std::uint32_t MonsterUnder(float x, float y, float width, float height, float* distance = nullptr, bool* centre = nullptr) const;
    // The camera ray through a screen point against an entity's model box on the stage (none on it: no).
    bool RayHitsEntity(float x, float y, float width, float height, std::uint32_t id, float& distance) const;
    void SelectMonster(std::uint32_t entity);
    // A COS (a pet, horse or transport; anyone's) under a point, and its selection: the target window's common look
    // (1.188 CIFTargetWindowCommonEnemy, a pet without a health bar, a horse or transport with
    // one). The player's own mount is not picked; another's is, and a click on its rider takes it (the original's).
    std::uint32_t CosUnder(float x, float y, float width, float height, float* distance = nullptr, bool* centre = nullptr) const;
    void SelectCos(std::uint32_t entity);
    void RefreshTarget(double now);
    // Fights (C10b): what each walker plays for a while (a swing's clip, a hit's flinch), each swing
    // taking the skill's next shot clip; the killed lie down (their die clip held), the ones that
    // came into view dead already lying (the die loop).
    // A motion: its clips one after another (a skill's ready, wait and shot), each until its time.
    struct Motion {
        struct Step {
            std::uint32_t state = 0;
            double until = 0;
            std::uint32_t take = 0;
            std::string set; // the set its clip comes from (a skill's group; empty: the stance)
        };
        std::vector<Step> steps;
        // A swing's shot ends with its clip (the 1.188 client steps on at the clip's end):
        // when it starts, until the clip's length is known (the last step's time till then: the data's).
        double shotFrom = -1.0;
        // From when its weapon is hidden until the swing ends (negative: never).
        double hideFrom = -1.0;
        // Its weapon's trail (the skill's cols 14..17): from its shot clip's event 5 to its event 6 (or
        // the clip's end); its texture, colour and length; the trail drawn (0: none yet).
        double trailOn = -1.0, trailOff = -1.0;
        std::string trailTexture;
        std::uint32_t trailColour = 0xFFFFFFFFu;
        float trailSeconds = 0.2f;
        gfx::EffectSystem::Handle trail = 0;
        // Its spine aim (the skill's col 23 axis; zero none) at its target.
        glm::vec3 spineAxis{0.0f};
        std::uint32_t target = 0;
        // A skill whose presentation has a READY, WAIT or SHOT clip: its caster's own walk waits for its
        // end (enters the skill state only for those; the imbues have none).
        bool locks = false;
        // A teleport skill's dash (EntityBlink): its clips play while it runs; a copy of its WAIT stage's effect left
        // every kDashCopyStep units it covers, from
        // where the last was left.
        bool dash = false;
        std::string dashEffect;
        glm::vec3 dashLast{0.0f};
        bool dashStarted = false;
        // A skill's wait loop: the cyclic layer from the cast's start, under its READY one-shot, until its shot.
        std::uint32_t waitState = 0;
        std::string waitSet;
        bool waitLoop = false;
        double End() const { return steps.empty() ? 0.0 : steps.back().until; }
    };
    // A teleport skill's move told before its swing (EntityBlink), by its caster: its skill and how long its dash runs
    // (0: a put).
    struct Blink {
        std::uint32_t skill = 0;
        double seconds = 0;
    };
    std::unordered_map<std::uint32_t, Blink> blinks_;
    std::unordered_map<std::uint32_t, Motion> motions_;
    // A player character's latest flinch (its take; WorldScreen::StageLook::flinch).
    std::unordered_map<std::uint32_t, std::uint32_t> flinches_;
    // A click on the ground while the player's own skill swings or it lies knocked: kept, its marker
    // shown, and sent the moment that ends (keeps it, sends it once the state
    // bits 0x2476 are clear, tries each frame); any other click or a skill replaces it.
    std::optional<game::proto::WorldPosition> heldMove_;
    bool heldSky_ = false; // the held click was on the sky
    // A teleport skill's point kept the same way (its skill; 0: the held point is a walk): the 1.188 client keeps it in
    // its walk keeper too and sends it as the skill.
    std::uint32_t heldSkill_ = 0;
    double heldUntil_ = 0; // the latest it waits (5 s: a skill that hands on to the attack would hold it for ever)
    double HeldUntil(double now) const;
    // The weather the server last told (proto::WeatherNow), given to each new place's world.
    game::proto::WeatherNow weather_;
    // The game clock carried from place to place (the original's runs on through teleports): hours and day; < 0 hours: not
    // yet (the client's own from 00:00 at its first place, as the original's starts, until the server's WorldClock).
    double clockHours_ = -1.0;
    int clockDay_ = 0;
    // The action list's count the server told (proto::ActionHeld): two held, an attack
    // click and a queued skill are not sent, a ground click sends no stop; and when the player last
    // sent an attack or a skill (a walk kept waits 500 ms after it).
    std::uint8_t actionsHeld_ = 0;
    double actionSentAt_ = -1e9;
    double clickAfter_ = 0.0; // --click-after S: the scripted click waits so long in the world
    std::optional<game::proto::WorldPosition> clickAt_; // --click-at region,x,z
    double heldSince_ = 0.0;
    // A click's walk (the Option window's Beta page, core::BetaOptions): Pathfinding sends a ground click through the way
    // search (RouteTo: legs of straight walks, each checked by the server as a click); Classic, and a click on the sky,
    // walk straight there, as the original's click does.
    // A click's walk (the ground's, a far NPC's or gate's): a stop first with one command held, kept while a swing
    // runs (true: kept, sent by Update).
    bool ClickWalk(ScreenContext& context, const game::proto::WorldPosition& to, bool sky);
    void WalkTo(ScreenContext& context, const game::proto::WorldPosition& to, bool sky);
    bool OwnMoveHeld(double now) const;
    // A hit waits for its swing's damage moment of its number (proto::EntityDamaged::strike): the
    // moments of each attacker's latest swing (the game's clock), the hits held till theirs, and what
    // the server sends with a hit (its knock, the states it gives) held with it.
    std::unordered_map<std::uint32_t, std::vector<double>> strikeTimes_;
    // The targets each attacker's latest swing struck, in the order their hits came (a splash's hops,
    // AT_SOURCE's flights back: StageEffects::struckBy).
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> swingTargets_;
    struct HeldHit {
        double at = 0;
        std::uint32_t target = 0;
        game::TitleEvent event;
    };
    std::vector<HeldHit> heldHits_;
    // A swing whose presentation has no damage group: its results held to its end, by its caster.
    std::unordered_map<std::uint32_t, double> resultsAt_;
    std::unordered_map<std::uint32_t, std::uint32_t> newestHitHp_; // a target -> the health the server's last hit on it told
    bool releasingHit_ = false; // a held hit going on now (a companion's is not flown again)
    void ReleaseHits(ScreenContext& context);
    std::minstd_rand clipRandom_{std::random_device{}()}; // the byte a cast picks its clips by
    // Until when a player stands in its combat stance (ATTREADY; 5 s from an attack or a hit).
    std::unordered_map<std::uint32_t, double> combatStance_;
    std::unordered_map<std::uint32_t, double> killed_;       // seen dying (when)
    std::unordered_map<std::uint32_t, double> appeared_;     // walkers fading in since then
    // Walkers the server took away, fading out where they were until their time is up.
    struct Vanishing {
        game::WorldView::Entity entity;
        double since = 0;
    };
    std::unordered_map<std::uint32_t, Vanishing> vanishing_;
    std::uint32_t takes_ = 0;
    void Fight(ScreenContext& context, const game::TitleEvent& event);
    // An entity's running presentation ended (a new swing of its own, a knock-down, its death, the server's cancel):
    // its later stages dropped, its held effects ended, its hits not shown yet
    // shown at once.
    void EndPresentation(std::uint32_t entity, double now);
    // The sound (O3, audio/GameAudio.h): an event's sound, heard before the state takes it in; and
    // each frame the ear at the camera and the music and ambience of the player's place.
    void Sounds(ScreenContext& context, const game::TitleEvent& event);
    void Listen(ScreenContext& context);
    // The player's death: the death window 3 s after the fall,
    // again on a click on the own bar while it lies; gone when the character is up again.
    double diedAt_ = -1;
    void DeathWindow(double now, bool reopen);
    bool IsDead(const game::WorldView::Entity& e) const;
    // Under a stealth or an invisibility (its buffs: the player's own, others' EntityBuffs).
    bool Hidden(std::uint32_t entity) const;
    std::uint32_t NpcUnder(float x, float y, float width, float height, float* distance = nullptr, bool* centre = nullptr) const;
    void Npcs(double now);
    bool StandingNear(std::uint32_t entity, float reach) const;
    // The player turned where it stands toward what it acts on in reach with no walk and no swing (an NPC's talk, an
    // item's pick-up): at once on its own screen, and told to the server (proto::Turn) from a degree on, as the original's.
    void FaceToward(std::uint32_t target);
    double pendingUntil_ = 0; // a clicked NPC or item is given up after this
    // Items on the ground (C7i): their models kept with the view, the one clicked picked up once the
    // character stands near it, their names while the key is held.
    void SyncGround(double now);
    std::uint32_t GroundUnder(float x, float y, float width, float height, float* distance = nullptr) const;
    std::uint32_t pendingPick_ = 0;
    bool showDropNames_ = false;
    // The items' names as the last frame drew them (screen pixels, depth): a click on one is a click
    // on its item (the pouch under it is small from afar).
    struct ItemLabel {
        std::uint32_t id = 0;
        ui::Rect rect;
        float depth = 0;
    };
    std::vector<ItemLabel> itemLabels_;

    std::unordered_set<std::uint32_t> groundShown_;
    // Gold seen falling (WorldView::Entity::fresh): its coin shower on a walker's stage slot, its clip
    // played once from when it shows, then its heap in its place (as the 1.188 client shows it).
    struct Shower {
        std::size_t slot = 0;
        double since = 0;     // asked for
        double shownAt = -1;  // its model in and its clip started (-1: not yet)
        double seconds = 0;   // its clip's length
    };
    std::unordered_map<std::uint32_t, Shower> showers_;
    void EndShower(std::uint32_t entity);
    // A skill stage's model at its target (StageEffects::TargetModel: a monster's roots out of the ground): on a
    // walker's free slot at the target's feet, its clip played once (as the coin shower), the slot freed at its end.
    std::vector<Shower> stageProps_;
    void StageProps(double now);
    std::vector<game::proto::BuffAdded> pendingBuffs_; // put on the board after the event's own handling // the server's answer, shown once the HUD is made
    // --gate-choose "2,1,y": the gate menu's choices picked one a second once it is open ("y" / "n":
    // the message box answered).
    std::vector<std::string> gateScript_;
    double gateScriptAt_ = 0;
    void GateScript(double now);
    bool waitingForGate_ = false; // asked; the server's answer has not come
    bool holdForGate_ = false;    // --use-gate: captures wait for the teleport
    // Developer teleports (F1 tools; --goto N once in the world): a teleport point's arrival.
    void DevTools(ScreenContext& context);
    void NetworkPanel();
    // The game connection's rates over the last second (netRates_), sampled every frame: the performance panel and the
    // F1 Network panel read them.
    void SampleNetwork();
    // The performance panel's numbers (performance_), every frame.
    void SamplePerformance(const ScreenContext& context);
#ifdef SR_BUG_REPORTS
    // A bug report from the HUD's window (PlayBugReport.cpp): its screenshot taken on the next frame (the report window
    // closed, the interface left out unless asked for), read back once written, then sent with the report in parts.
    void StartBugReport(BugReportForm form);
    void BugReportFrame(ScreenContext& context, FrameSnapshot& snapshot);
    struct BugReportOut {
        enum class Stage : std::uint8_t { Capture, Waiting, Sending } stage = Stage::Capture;
        BugReportForm form;
        std::filesystem::path shot; // the screenshot's file, written by the render thread
        double since = 0;           // when it was asked for (the wait's limit)
        std::vector<std::uint8_t> image;
        std::size_t sent = 0;       // the image's bytes sent
        double nextPart = 0;        // the parts' pace
    };
    std::optional<BugReportOut> bugReport_;
#endif
    std::uint32_t gotoPoint_ = 0;
    int devPoint_ = 0;
    // The F1 Teleport panel's other ways: a place by the minimap's X / Y, an NPC (npcpos.txt's, by its name or codename), a
    // monster's nest (its codename); the lists' filters.
    char devFilter_[64] = {}, devNpcFilter_[64] = {}, devMonster_[96] = {};
    int devX_ = 0, devY_ = 0;
    std::uint32_t devNpc_ = 0; // the NPC row picked (characterdata id)
    game::WorldView view_;
    game::SimulationClock clock_{game::SimulationClock::kDefaultRate};
    // The walkers on the world screen's stage slots.
    // Enough for the server's biggest view so far (600 players, 400 monsters) and its NPCs (8 KB of
    // bone palette each, a frame in flight); a view holding more stages its nearest (PlaceWalkers).
    static constexpr std::size_t kSlots = 1280;
    // The last of them set aside for the models a buff's stages carry (StageEffects::SetCompanionSlots).
    static constexpr std::size_t kCompanionSlots = 32;
    static constexpr std::size_t kWalkerSlots = kSlots - kCompanionSlots;
    std::unordered_set<std::uint32_t> staged_; // the nearest kSlots, when the view holds more walkers
    double stagedAt_ = -1.0;                   // when staged_ was chosen (every half second)
    // The equipment window's character: one more stage slot, in the scene's overlay, seen through a
    // camera of its own aimed into the window.
    static constexpr std::size_t kViewSlot = kSlots;
    static constexpr std::uint32_t kPreviewId = 0xFFFFFFF0u; // the skin change window's look (its cache's entry)
    void CharacterView(ScreenContext& context);
    void CharacterViewCamera(ScreenContext& context, FrameSnapshot& snapshot);
    bool viewShown_ = false;
    std::unordered_map<std::uint32_t, std::size_t> slots_;
    std::vector<bool> slotUsed_;
    std::unordered_map<std::uint32_t, WorldScreen::StageLook> looks_; // dressed, per entity
    std::uint32_t enteredFrom_ = 0; // the player's entity before the latest WorldEntered (Arrive: the same id or another server's)
    void ForgetEntities(); // Arrive: the old world's entries by entity id
    std::unordered_map<std::uint32_t, std::uint32_t> lookVersions_;  // the entity's look they were dressed for
    std::unordered_map<std::uint32_t, int> structureStages_;         // a structure's model (0 whole, 1 damaged, 2 rubble) they were dressed for
    // The follow camera (ours: the original's limits are not traced yet).
    float cameraYaw_ = 0.0f, cameraPitch_ = 0.35f, cameraDistance_ = 60.0f;
    // A camera shake (the player's own skills' SCT_SHAKECAM): its amplitude (the original's units; ours
    // a hundredth of a world unit), how long it lasts, how often its jolt changes; the jolt now.
    struct QuestSceneRun {
        bool active = false;
        std::vector<assets::SceneCommand> commands;
        std::size_t next = 0;
        double started = 0;
        bool hideSelf = false;
        bool bars = false;
        float barsSeconds = 2.0f, barsHeight = 120.0f;
        double barsShown = 0; // 0..1, sliding
        std::vector<assets::CameraKey> inserting, track;
        float trackFrom = 0;
        bool running = false;
        std::optional<assets::CameraKey> held;
        std::vector<std::pair<std::string, std::size_t>> objects; // its characters' names -> stage slots
        struct Placed {
            WorldScreen::StageLook look;
            float x = 0, y = 0, z = 0, yaw = 0;
        };
        std::map<std::string, Placed> looks;
        std::set<std::string> unshown; // commands logged once as not shown
    } scene_;
    struct CameraShake {
        double from = 0, until = 0, next = 0;
        float amplitude = 0, step = 0.02f;
        float offset[3] = {0, 0, 0};
        std::uint32_t random = 12345;
    } shake_;
    const core::Sight* sight_ = nullptr;       // the options' (null: free)
    std::array<int, core::InputOptions::kBindings> appliedKeys_{}; // the key options the HUD took last
    std::array<int, 3> appliedView_{-1, -1, -1}; // the Online options' players, monsters and radius the server was told last
    game::proto::ViewLimitsAnswer viewAnswer_; // what the server applies to this player's view
    bool haveViewAnswer_ = false;
    // The performance panel (HudWindows.h): what it shows, and the frames of the second being counted.
    HudPerformance performance_;
    struct FrameWindow {
        double start = -1;
        int frames = 0;
        double sumMs = 0, worstMs = 0;
    } frameWindow_;
    // The game connection: the session's counts at the last sample, and the rates since.
    struct NetSample {
        double time = -1;
        std::uint64_t bytesIn = 0, bytesOut = 0, messagesIn = 0, messagesOut = 0;
        std::map<game::proto::MessageId, std::uint64_t> byId;
    } netSample_;
    struct NetRates {
        double bytesIn = 0, bytesOut = 0, messagesIn = 0, messagesOut = 0;
        std::vector<std::pair<game::proto::MessageId, double>> byId; // messages a second, the busiest first
    } netRates_;
    core::Sight lastSight_ = core::Sight::Free;
    float cameraDt_ = 0.0f;                    // this frame's seconds, for the third person's swing
    double cameraNow_ = 0.0;                   // this frame's time (the camera shake)
    float cameraShown_ = 60.0f;                // the distance after the collision, eased back out
    float ownFade_ = 1.0f;                     // the player's own model, faded out while looking steeply up
    std::uint32_t markedTarget_ = 0;            // the selection the ground marker last saw, and when it came
    double markedTargetAt_ = -1.0;
    glm::dvec3 cameraFocus_{0.0};              // the point the camera looks at, in world units, eased after the character
    bool cameraFocusSet_ = false;
    bool turning_ = false;
    bool netOff_ = false; // the lost connection's box is up
    bool quitNow_ = false; // its button pressed: the game ends
    ConfirmBox* recallBox_ = nullptr; // the guild master's recall asked, unanswered (closed at recallUntil_)
    ConfirmBox* questEnterBox_ = nullptr; // a quest instance's entry asked (QuestEnterAsk), unanswered (closed at questEnterUntil_)
    double questEnterUntil_ = 0.0;
    double recallUntil_ = 0.0;
    bool hpLow_ = false, mpLow_ = false; // under 30 % at the last frame (the warning's alarm)
    bool turnHeld_ = false;   // this turn holds the cursor (the Game page's camera method 2)
    ui::Vec2 turnFrom_;       // where the turn's button was pressed
    bool targetMenuArmed_ = false; // the right press may still open a player's menu at its release
    InterfaceHost gui_;
    Hud hud_;
    game::PlayerState player_; // what the HUD shows
    HudContext hudContext_;
    // The HUD's portraits (PlayPortraits.cpp): the player's head in its bar.
    Portraits portraits_;
    void UpdatePortraits(ScreenContext& context, FrameSnapshot& snapshot);
    // The system menu's Restart / Exit: the server's countdown, shown a second at a time.
    void Logout(const game::proto::LogoutAnswer& answer, double now);
    void LogoutTick(double now);
    bool exitAfterLogout_ = false;
    // Speech over heads: the last line each character said to the
    // people around (or to everyone), for 10 s, drawn under the windows.
    struct Speech {
        std::string text;
        ui::Color colour = 0;
        double until = 0;
    };
    std::unordered_map<std::uint32_t, Speech> speech_;
    std::uint64_t speechSeen_ = 0;
    void TakeSpeech(double now);
    // Guilds and unions (PlayGuild.cpp): the player's guild and the guilds of
    // those it sees; their lines, the proposals' boxes, the guild line over heads; the test options
    // --guild "step;step" [--guild-after S] [--guild-step S] [--guild-accept | --guild-decline].
    game::GuildView guild_;
    // Friends, the block list and letters (PlayCommunity.cpp): the Community pages'
    // picture, its system lines, the friend proposal's box.
    game::CommunityView community_;
    void HandleCommunity(ScreenContext& context, const game::proto::CommunityEvent& event);
    // The Academy (PlayAcademy.cpp): its events into player_.academy, their lines, the
    // invitation's box, the test steps.
    void HandleAcademy(ScreenContext& context, const game::proto::AcademyEvent& event);
    bool AcademyScriptStep(const std::string& verb, const std::string& arg);
    bool academyAskOpen_ = false;
    // The Open Market (PlayOpenMarket.cpp): its events into player_.openMarket, their lines,
    // the window opened and closed with the market, the test steps (om-*).
    void HandleOpenMarket(ScreenContext& context, const game::proto::OpenMarketEvent& event);
    bool OpenMarketScriptStep(const std::string& verb, const std::string& arg);
    // The exchange with another player (PlayExchange.cpp): its window's picture,
    // the request's box, the window's Confirm, the test steps.
    game::ExchangeView exchange_;
    bool exchangeAsking_ = false, exchangeAskDropped_ = false; // the request box is open; its request went
    void AskExchange(ScreenContext& context);
    void HandleExchange(ScreenContext& context, const game::proto::ExchangeEvent& event);
    void ExchangeConfirm();
    bool ExchangeScriptStep(const std::string& verb, const std::string& arg);
    // Stalls and the stall network (PlayStall.cpp): the player's stall and the network's
    // pages, the booths of those around (by entity) and the ones shown, a booth clicked (walked to, then visited),
    // the network's tables and the towns (read on first use).
    game::StallView stall_;
    // The quests (PlayQuest.cpp): the player's as the server told them.
    game::QuestView quests_;
    double lastTime_ = 0.0; // the clock's second of the last quest message (the windows' own lines)
    void HandleQuest(ScreenContext& context, const game::proto::QuestEvent& event);
    std::optional<game::proto::QuestMark> QuestMarkOf(std::uint32_t entity) const;
    // The Forgotten World (PlayInstance.cpp): the gaps' facts, the instance the
    // character is in, its collection book (theme << 8 | slot).
    std::unordered_map<std::uint32_t, game::proto::GapInfo> gaps_;
    std::set<std::uint32_t> gapsAsked_;
    game::proto::InstanceState instance_;
    std::set<std::uint32_t> book_;
    game::proto::SummonList summonList_;
    std::uint32_t hoveredGap_ = 0; // the gap under the mouse (its tooltip)
    std::uint32_t hoveredEntity_ = 0; // the NPC, monster or player under the mouse (its name shows at any distance)
    int beginnerOption_ = -1;         // the Game page's beginner mark last seen (-1: not yet; a change is told to the server)
    bool NameShown(std::uint32_t id, const game::WorldView::Entity& e, ScreenContext& context) const;
    // The status gauge over one's head (the Game page's status rows): 0 none, 1 HP alone, 2 HP and MP; the shares filled.
    int StatusGauge(std::uint32_t id, const game::WorldView::Entity& e, ScreenContext& context, float& hp, float& mp) const;
    double gapHoverAskedAt_ = -1e9; // a temple door's facts last asked under the mouse
    bool IsPillar(const game::WorldView::Entity& e) const;
    void HandleInstance(ScreenContext& context, const game::proto::InstanceEvent& event);
    const assets::TeleportGate* GapGate(const game::WorldView::Entity& e) const; // a gap: its gate (null: not one)
    bool IsExit(const game::WorldView::Entity& e) const;
    bool UsedNear(std::uint32_t entity) const; // a gap or an exit: used from nearer than an NPC is talked to
    float UseReach(std::uint32_t entity, float otherwise) const;
    float UseWalkTo(std::uint32_t entity, float otherwise) const;
    void AskGaps();
    std::vector<std::string> GapLines(std::uint32_t entity);
    void UseAt(std::uint32_t entity);
    bool UseTalisman(std::uint8_t place);
    bool InstanceScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg);
    // Alchemy's (PlayAlchemy.cpp): alchemy (the window), alchemy-tab:N, alchemy-put:CODENAME (the bag's first of it),
    // alchemy-go (the button), alchemy-yes / alchemy-no (the alert box).
    bool AlchemyScriptStep(const std::string& verb, const std::string& arg);
    bool SocketScriptStep(const std::string& verb, const std::string& arg);
    int BagPlaceOf(const std::string& code) const; // the bag's first item of that codename, -1 none
    std::unordered_map<std::uint32_t, game::proto::EntityStall> entityStalls_;
    std::unordered_map<std::uint32_t, std::uint32_t> boothsShown_; // entity -> the skin shown
    std::uint32_t pendingStall_ = 0;
    double pendingStallUntil_ = 0.0;
    assets::StallNetworkData stallTables_;
    // Magic POP's tables (gachaitemset, gachanpcmap, the selection's tree), read once; its script steps (PlayMagicPop.cpp):
    // pop-choose:ENTRY (the machine's row, then the prize through the selection), pop-card:PLACE|CODENAME, pop-play,
    // pop-exchange (Gori's row).
    assets::GachaTable gacha_;
    bool gachaRead_ = false;
    bool PopScriptStep(const std::string& verb, const std::string& arg);
    assets::RegionAreaTable towns_;
    bool stallTablesRead_ = false;
    void OpenStall(ScreenContext& context);
    void OpenStallNetwork(ScreenContext& context);
    void HandleStall(ScreenContext& context, const game::proto::StallEvent& event);
    void SyncBooths();
    bool ClickBooth(ScreenContext& context, std::uint32_t entity);
    void StallVisits(double now);
    bool StallScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg);
    // COS: rides, transports and pets (PlayCos.cpp): our own (CosSummoned), who rides what
    // (CosRiding, ours and others'), riders seated on their mounts' saddles, the commands and the test steps.
    struct OwnCos {
        std::uint32_t entity = 0, model = 0, item = 0;
        std::uint8_t kind = 0; // game::CosKind
        std::uint32_t hp = 0, maxHp = 0;
        std::string name;      // a pet's (empty: none given)
        std::uint8_t level = 0;
        std::uint16_t hgp = 0; // an attack pet's hunger points
        std::uint32_t option = 0; // a grab pet's settings
        std::uint8_t bagSize = 0; // a grab pet's bag (CosBag)
        std::map<std::uint8_t, game::ItemInstance> bag;
        std::uint64_t bagVersion = 0;
        std::uint64_t experience = 0; // an attack pet's, in its level
        std::int64_t rentUntil = 0;   // a grab pet's rental's end (Unix seconds on this machine's clock; 0: none)
        bool aggressive = true;       // an attack pet's stance (CosStance)
    };
    void CosHit(const game::proto::EntityDamaged& hit); // a hit on our own COS: its health (PlayCos.cpp)
    std::vector<OwnCos> ownCoses_;
    std::unordered_map<std::uint32_t, std::uint32_t> riders_; // rider -> its mount
    // The order the walkers are staged in each frame: riders after their mounts (kept: no allocation per frame).
    std::vector<const std::pair<const std::uint32_t, game::WorldView::Entity>*> stageOrder_;
    void HandleCos(ScreenContext& context, const game::proto::CosEvent& event);
    void CosForget(std::uint32_t entity); // gone from sight
    // Where a rider sits: its mount's saddle bone as last drawn, the mount's way; false: on foot (or its mount
    // not staged and posed yet).
    bool SeatOf(std::uint32_t rider, glm::vec3& at, float& yaw) const;
    std::uint32_t OwnMount() const;       // what carries the player (0: on foot)
    void CosRide(bool ride);              // the player's ride or transport: board or leave
    void CosDismiss();                    // send it away
    void SyncCosHud();                    // the HUD's COS (status icons, command bar) as they are now
    void CosCommand(int command, std::uint32_t entity); // the command bar's, the Pet hotkeys'
    bool CosScriptStep(const std::string& verb, const std::string& arg);
    // The item mall's services' steps (PlayServices.cpp).
    bool ServiceScriptStep(const std::string& verb, const std::string& arg);
    bool CommunityScriptStep(const std::string& verb, const std::string& arg, const std::string& step);
    void CommunityTick(double now);
    // System lines that came before the interface's texts were in (the world entered while they load): posted once they
    // are (their keys would show otherwise).
    std::vector<std::function<void(double)>> textLater_;
    bool friendAskOpen_ = false;
    double friendAskUntil_ = 0.0;
    // The fortress war (PlayFortress.cpp): the fortresses and their
    // structures as the server tells them, the war's lines, the official's window; the test options
    // --fortress "step;step" [--fortress-after S] [--fortress-step S].
    game::FortressView fortress_;
    game::proto::NpcTax npcTax_; // the fortress tax at the NPC or gate last talked to
    // Safe trade: the NPC last talked to's tail, the option byte kept from it (0 until told), and whether it is on (the
    // buff board's icon while it is).
    game::proto::SafeTradeTalk npcSafe_;
    std::uint8_t safeOption_ = 0;
    bool safeTradeOn_ = false;
    double bargainUntil_ = 0; // the bargain goods' lock-out's end on the client's clock (its icon until then)
    void SyncSafeTradeIcon(double now);
    void FortressOptions(ScreenContext& context);
    void HandleFortress(ScreenContext& context, const game::proto::FortressEvent& event);
    void FortressScript(ScreenContext& context);
    void UseAction(ScreenContext& context, std::uint32_t action); // an icon of the Action window (PlayActions.cpp)
    // A chat command (its text table key and the line typed): what it names done (PlayActions.cpp).
    void ChatCommand(ScreenContext& context, const std::string& key, const std::string& line);
    // An action shortcut's icon and text key as the next use would do it (PlayActions.cpp).
    std::pair<std::string, std::string> ActionSlotLook(std::uint32_t action) const;
    // The Action page: each walker's motion and marks as the server told them
    // (EntityMotion, EntityMarks), the helper marks' effects, auto run.
    struct ActionMotion {
        std::uint8_t motion = 0, to = 0;
        bool walking = false;
        double since = 0.0;
    };
    std::unordered_map<std::uint32_t, ActionMotion> entityMotions_;
    std::unordered_map<std::uint32_t, std::uint8_t> entityMarks_;
    // The players in a job's suit as they are told (EntityJob): their state, job level, alias.
    std::unordered_map<std::uint32_t, game::proto::EntityJob> entityJobs_;
    // Job suits going on or coming off (EntityJobChange; the original's 0x3041 sub 2): when the change began, its
    // seconds; the 48 x 2 yellow bar over the head shrinks with them.
    struct JobChangeBar {
        double started = 0;
        int seconds = 0;
    };
    std::unordered_map<std::uint32_t, JobChangeBar> jobChanges_;
    struct MarkEffect {
        std::size_t slot = 0;
        std::string list; // the effect list shown (SYSTEM_HELPERMARK, SYSTEM_FREEPVP_RED, ...)
        std::vector<gfx::EffectSystem::Handle> effects;
    };
    // Free PvP (PlayActions.cpp): each player's team and whether a change runs, the window.
    struct FreePvpState {
        std::uint8_t team = 0, to = 0;
        bool changing = false;
    };
    std::unordered_map<std::uint32_t, FreePvpState> entityPvp_;
    // The player's last change under way: its team then and the one asked (a cancel's line by them, the original's).
    std::uint8_t pvpChangeFrom_ = 0, pvpChangeTo_ = 0;
    void ToggleFreePvpWindow();
    bool FreePvpFoe(std::uint32_t other) const;
    bool JobFoe(std::uint32_t other) const; // a job's foe (PlayFortress.cpp)
    void HandleFreePvp(ScreenContext& context, const game::proto::EntityFreePvp& e);
    // Murderers (PlayPk.cpp): each player's PvP state (EntityPvpState), the click on a player (select, then a second click or Alt; enemies without Alt), the return scrolls a
    // murderer may not read, the texts of the refusals.
    std::unordered_map<std::uint32_t, std::uint8_t> entityPk_;
    std::uint8_t PvpStateOf(std::uint32_t entity) const;
    void HandlePk(ScreenContext& context, const game::proto::PkEvent& e);
    // `mount`: the ride the player sits on, the one selected (the original's pick gives a rider's mount).
    void ClickPlayer(ScreenContext& context, std::uint32_t other, bool ctrl, bool alt, std::uint32_t mount = 0);
    // What one click at a point picks (the original's single pick over every kind): at most one set; `mount` with
    // `player` when he rides (the mount is the one selected).
    struct Picked {
        std::uint32_t item = 0, npc = 0, monster = 0, player = 0, cos = 0, mount = 0;
    };
    Picked PickUnder(float x, float y, float width, float height) const;
    // The cursor over another player or a COS's owner (the original's 250 ms choice): the red sword when he may be
    // attacked (or Alt is held) and is not of the party, a booth's bag, the fortress war's and the events' sides.
    int PlayerCursor(std::uint32_t player, bool alt) const;
    bool MurdererScrollRefused(std::uint8_t place, double time);
    // The berserk time's scroll ('pht') in berserk: refused by the client ("Cannot be used in berzerk mode.", the original's).
    bool BerserkScrollRefused(std::uint8_t place, double time);
    void PkRefusal(game::proto::SkillUseStatus status, double time);
    ui::Color PlayerNameColour(std::uint32_t id, const game::WorldView::Entity& e) const;
    // Capture the Flag and Event So-Ok (55; PlayFlagWorld.cpp): the client's view of the event (the notices'
    // lines, the timer, the teams, the doors, So-Ok's desk), the timer window's state, a script step waiting for So-Ok's
    // window, the death box's camp button, the player's own action bar (a job suit's change, a pole's action) and what it
    // refuses.
    game::FlagView flag_;
    bool flagTimerShown_ = false;
    enum class FlagScript : std::uint8_t { None, Register, Swap, SwapRow };
    FlagScript flagScript_ = FlagScript::None;
    double flagScriptUntil_ = 0.0;
    int flagWalkTries_ = 0; // ctf-walk's repeats
    std::string snapName_; // the script's snap:NAME: a picture of the next frame, the run going on (--snap-dir)
    void HandleEvent(ScreenContext& context, const game::proto::EventEvent& event);
    void FlagLine(const game::FlagView::Line& line, double time);
    bool FlagFoe(std::uint32_t other) const;
    void FlagFrame(ScreenContext& context);
    void FlagDeath(double now);
    bool FlagScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg);
    std::uint32_t NearestByCodename(const std::function<bool(std::string_view)>& match) const;
    bool ActionBarRuns(double now) const;
    bool ActionBarRefuses(double now); // running: its system line, true
    // The Battle Arena (PlayArena.cpp): the client's view (the lines, the state and its windows, the
    // scores, the result, the timer, the manager's desk), a script step waiting for the manager's window and the rule
    // window, and the steps.
    game::ArenaView arena_;
    enum class ArenaScript : std::uint8_t { None, Register, Cancel };
    ArenaScript arenaScript_ = ArenaScript::None;
    std::uint8_t arenaScriptKind_ = game::arena::kScore;
    double arenaScriptUntil_ = 0.0;
    bool HandleArena(ScreenContext& context, const game::proto::EventEvent& event);
    void ArenaLine(const game::ArenaView::Line& line, double time);
    void ArenaFrame(ScreenContext& context);
    bool ArenaScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg);
    bool TalkToNearest(ScreenContext& context, std::string_view codename, const std::string& verb);
    std::unordered_map<std::uint32_t, MarkEffect> markEffects_;
    bool autoRun_ = false;
    double autoRunNext_ = 0.0;
    void AutoLoot(ScreenContext& context);
    void AutoRun(ScreenContext& context);
    void ActionMarks(ScreenContext& context);
    std::unordered_map<std::string, std::vector<assets::ResourceData::ParticleModifier>> carriedEffects_; // a carried model's own effects
    const std::vector<assets::ResourceData::ParticleModifier>& CarriedEffects(ScreenContext& context, const std::string& model);
    // Another player the fortress war makes an enemy (both in its area; not of the guild or its union).
    bool FortressFoe(std::uint32_t entity, const game::WorldView::Entity& e) const;
    // A fortress's structure of the player's guild or union (never hit by them).
    bool OwnStructure(std::uint32_t entity) const;
    // The test bots: a random attack skill known and off its wait (0: none).
    std::uint32_t RandomAttackSkill(double now) const;
    void BotSkill(ScreenContext& context, std::uint32_t target);
    double botSkillAt_ = -1.0;
    std::vector<std::string> fortressScript_;
    double fortressNext_ = -1.0, fortressAfter_ = 3.0, fortressStep_ = 2.0;
    int walkTries_ = 0;
    std::int64_t levyAmount_ = -1; // --fortress tax-levy:N: the amount typed into the levy box
    std::uint16_t walkRegion_ = 0; // the --fortress walk step's region (another: a teleport zone took the walker)
    std::uint32_t siegeTarget_ = 0; // the structure the siege step last attacked, and when
    double siegeAttackAt_ = -1.0;
    void GuildOptions(ScreenContext& context);
    void HandleGuild(ScreenContext& context, const game::proto::GuildEvent& event);
    void GuildScript(ScreenContext& context);
    bool DrawGuildLine(ui::Painter& paint, std::uint32_t entity, const ui::Rect& nameAt); // false: no guild line
    std::uint32_t NearestNpc(bool guildOnly) const; // the nearest NPC in sight (a guild NPC), 0: none
    std::vector<std::string> guildScript_;
    double guildNext_ = -1.0, guildAfter_ = 3.0, guildStep_ = 2.0;
    int guildAnswer_ = 0; // proposals: 1 yes, -1 no, 0 asked in a box
    // Emblems: an emblem item used (its box, then the file read and sent;
    // true when the item was one), the emblems over heads as textures made into `madeFiles_`.
    bool UseCrestItem(std::uint8_t place);
    // A reverse return scroll in that bag place: its box opened instead of a use;
    // false for any other item. ReverseChoice sends what the box (or the map) chose.
    bool UseReverseReturn(std::uint8_t place);
    // A fortress war's item used from the bag or a quick slot (PlayFortress.cpp): the stock
    // client's checks, then the selected target (repair kits, the shock bomb), the nearest free zone (the
    // manuals) or a ground point picked by the next click (flags, guards, tablets: fortressPick_); false: not one.
    bool UseFortressItem(std::uint8_t place);
    std::optional<std::uint8_t> fortressPick_;
    bool FortressPicked(const game::proto::WorldPosition& at);
    // A manual's zone markers (the click):
    // the free barricade or war camp zones of the fortress within the manual's Range - 5, each shown by its structure's model
    // (the client's CImitationObj) on a stage slot of its own after kViewSlot; a click on one uses the manual on its zone
    // (the others go, it stays until the answer), a click elsewhere puts them all away.
    struct ZoneMarker {
        std::uint32_t zone = 0;
        game::proto::WorldPosition at;
        float yaw = 0;
        std::string model;
        float height = 1.0f;
    };
    static constexpr std::size_t kMarkerSlots = 32;
    std::vector<ZoneMarker> zoneMarkers_;
    std::uint8_t zonePlace_ = 0;      // the manual's bag place
    std::uint32_t zoneFortress_ = 0;
    bool zoneWaiting_ = false;        // a marker clicked: the use sent, its answer awaited
    double zoneSince_ = 0.0, zoneNow_ = 0.0;
    std::size_t zoneShown_ = 0;       // marker slots set last frame
    const assets::FileSystem* markerFiles_ = nullptr; // the game's files (the markers' models), kept from the frame
    bool ShowZoneMarkers(std::uint8_t place, const assets::ItemRow& manual, std::uint32_t fortress);
    void PlaceZoneMarkers(ScreenContext& context);
    void ClearZoneMarkers();
    // A click while markers show: true when it hit one (used), else they are put away and the click goes on.
    bool ClickZoneMarker(float x, float y, float width, float height);

    void ReverseChoice(std::uint8_t place, int result);
    // The bag's first reverse return scroll (0: none; the test steps').
    std::uint8_t ReverseScrollPlace() const;
    bool UseGlobalChatItem(std::uint8_t place);
    // The item mall's items (PlayMallItems.cpp): the uses that ask first or need a target (true:
    // one of them), the premium's chat commands, the services' answers.
    bool UseMallItem(std::uint8_t place);
    bool PremiumCommand(const std::string& line);
    void HandleMallItems(ScreenContext& context, const game::proto::MallEvent& event);
    // The item mall (PlayMall.cpp): its catalogue and the window's view, the answers, the steps, the preview.
    void SetupMall(ScreenContext& context);
    void HandleMall(ScreenContext& context, const game::proto::MallEvent& event);
    bool MallScriptStep(const std::string& verb, const std::string& arg);
    void MallPreview(ScreenContext& context);
    assets::MallTable mallTable_;
    MallView mall_;
    bool mallRead_ = false;
    bool premiumBoothAsked_ = false;
    void RegisterCrest(std::uint8_t kind, std::uint8_t place);
    std::string CrestTexture(std::uint8_t kind, std::uint32_t id, std::uint32_t revision);
    assets::MemorySource* madeFiles_ = nullptr;
    std::unordered_set<std::string> crestFiles_;
    std::string crestDir_, gameDir_; // where guildmark.bmp / unionmark.bmp are looked for (--crest-dir first)
    void DrawSpeech(ScreenContext& context, ui::Painter& paint);
    void DrawNames(ScreenContext& context, ui::Painter& paint);
    // Damage numbers: the hitcount digits over where the target
    // was hit (20 up), rising 50 pixels in a second, whole for half a second then fading; criticals
    // bigger with the word under them; red on players, white on the rest; shown for the player's own
    // hits and for hits nearer than 200.
    // Where a click on the ground sends the character (ours: a blue ring there for a moment).
    struct MoveMarker {
        game::proto::WorldPosition at;
        double born = -1.0;
    };
    MoveMarker moveMarker_;
    void GroundDecals(ScreenContext& context); // the move marker and the selection ring, laid on the ground
    struct DamageText {
        game::proto::WorldPosition at;
        std::uint32_t value = 0;
        bool critical = false;
        ui::Color colour = 0;
        double born = 0;
        bool blocked = false; // the "blocking" mark alone, no number (the original's hit kind 2)
    };
    std::vector<DamageText> damageTexts_;
    // Effects (V1; play/PlayEffects.cpp): a skill's stages on its caster and target (with its arrows
    // and bolts: released 0.3 of the way into the swing as the
    // server's hits, flying at the stage's speed to where the target stood then), hits, buffs while
    // they last, level ups, and the stage effects and models that fly.
    std::size_t SlotOf(std::uint32_t entity) const; // StageEffects::kNone when not shown
    // `deferShot`: its shot's stages wait for the impact (StartShot; P7).
    void SkillEffects(bool clipSounds, const assets::SkillRow& skill, std::uint32_t caster, std::uint32_t target, double now, const StageEffects::Swing& swing,
                      std::string_view set, std::uint32_t shotState, std::string_view weapon, bool deferShot = false);
    // What a cast's stages need of its caster and target now (its shot clip's events, its weapon, the target's hit point).
    StageEffects::Cast CastOf(std::uint32_t caster, std::uint32_t target, std::string_view set, std::uint32_t shotState, std::string_view weapon,
                              std::vector<double>& events);
    // A casting attack's shot waits for its impact (the 1.188 client starts SHOT at the server's impact message): its
    // first hit's message, or 400 ms past the data's time.
    struct PendingShot {
        std::uint32_t skill = 0, target = 0, state = 0, castNo = 0;
        StageEffects::Swing swing;
        double start = 0, planned = 0; // the swing's start and its shot by the data
        std::string set, weapon;
        bool actStart = false;
    };
    std::unordered_map<std::uint32_t, PendingShot> pendingShots_;
    // Its shot from `at`: its stages, its body's shot clip, its clip sounds, its trail and its hit moments moved by the
    // impact's delay (an early impact cuts its READY or WAIT short).
    void StartShot(std::uint32_t caster, double at);
    // The skill whose presentation (skilleffect.txt's stages, skillaniset's clips) a skill shows: the one it copies
    // (skilldata col 6, Basic_Original) when it names one, as the 1.188 client looks it up; else itself. The monsters'
    // and the bandits' rows copy a kin's or a player's (MOB_EU_* bandits a wizard's bolt, a warrior's strike). Its own
    // row keeps its times and its hits.
    const assets::SkillRow& Shown(const assets::SkillRow& skill) const;
    static std::string AnimationKey(const assets::SkillRow& skill) {
        return !skill.baseName.empty() && skill.baseName != "xxx" ? skill.baseName : skill.codename;
    }
    std::size_t shotsAtImpact_ = 0, shotsLate_ = 0; // the shots its hit started, and those the fallback did (the bench says)
    void HitEffects(const game::proto::EntityDamaged& hit);
    void BuffEffects(std::uint32_t token, std::uint32_t skill, bool added);
    void LevelUpEffect(std::uint32_t entity, double now);
    void KillOrbs(const game::proto::ExperienceGained& gained, double now);
public:
    // An item's use (ItemUsed: a potion, a pill, a scroll): the
    // effect list the 1.188 client plays on the user, by the item's kind (empty: none).
    static std::string ItemUseEffectName(const assets::ItemRow& row);
private:
    void ItemUseEffect(std::uint32_t entity, std::uint32_t item);
    std::uint64_t itemEffectsSeen_ = 0; // ItemUsed messages taken (the scripted tries log them)
    // The flights and held models, then the sounds due (K8): the stages'
    // where they are now, the swings' second ones at their release.
    void FlyEffects(ScreenContext& context);
    StageEffects stageEffects_;
    struct SecondSwing {
        std::uint32_t entity = 0;
        std::string skill; // its effectsound key (the base name, a monster's codename)
        double at = 0;
    };
    std::vector<SecondSwing> secondSwings_;
    // A hit's sound, due 100 ms after it shows (the 1.188 client's damage effect plays it on its tick once more than
    // 100 ms passed), where its target is then.
    struct HitSound {
        std::uint32_t attacker = 0, target = 0;
        bool critical = false;
        double at = 0;
    };
    std::vector<HitSound> hitSounds_;
    // A sound its clip names, due at `at` (the clip's sound track); not if its clip was cut short.
    struct ClipSound {
        std::uint32_t entity = 0;
        std::uint32_t take = 0;
        std::string skill, weapon, codename, handle, wave;
        double at = 0;
    };
    std::vector<ClipSound> clipSounds_;
    std::unordered_map<std::uint32_t, std::uint32_t> buffSkills_; // buff token -> its skill (its end's stages)
    std::unordered_set<std::string> benchSounds_;                 // the stage sounds the bench heard
    std::size_t benchSoundCues_ = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> lastSkill_; // attacker -> its last swing's skill (its hits' effects)
    std::unordered_map<std::uint32_t, std::vector<gfx::EffectSystem::Handle>> buffEffects_; // buff token -> its kept effects
    // Berserk's mode on an entity: its loops while it lasts.
    std::unordered_map<std::uint32_t, std::vector<gfx::EffectSystem::Handle>> berserkEffects_;
    // A body growing or shrinking (berserk's SCT_CHAR_SCALE: to 1.1 or 1.2 over its milliseconds as the mode starts; back
    // to 1 over the same as it ends, ours: the end's way is not traced), and the frame's time it is read at.
    struct Grow {
        float from = 1.0f, to = 1.0f;
        double start = 0.0, seconds = 1.0;
        float At(double now) const {
            const double t = seconds > 0.0 ? std::clamp((now - start) / seconds, 0.0, 1.0) : 1.0;
            return from + (to - from) * float(t);
        }
    };
    std::unordered_map<std::uint32_t, Grow> grows_;
    double now_ = 0.0;
    // `burst`: its start's burst and sound too (not when it was already in the mode, only just shown).
    void BerserkEffects(std::uint32_t entity, bool on, std::uint8_t variant, bool burst = true);
    std::unordered_set<std::uint32_t> berserkWaiting_; // in the mode, not on the stage yet
    std::unordered_map<std::uint32_t, std::uint8_t> berserkers_; // in the mode (its variant): they wear its hair and horns (LookOf)
    double berserkUntil_ = 0.0;                        // when the player's own mode ends (its orbs: 0 not in it)
    std::unordered_map<std::uint32_t, double> leveledAt_;
    // The kept effects, matched every frame to what is (PlayEffects.cpp): the loops of the player's
    // buffs while they last (those it entered the world with too), and the glow of the rare weapons
    // and shields everyone shown wears (resinfo\itemrare.txt).
    void KeptEffects(ScreenContext& context);
    void BuffLoops();
    // Others' buffs (EntityBuffs: the player's own come with BuffAdded / BuffRemoved): their skills,
    // the slot their loops are on, the loops by skill (started once the walker shows).
    struct Buffed {
        std::vector<std::uint32_t> skills;
        std::size_t slot = StageEffects::kNone;
        std::unordered_map<std::uint32_t, std::vector<StageEffects::Handle>> loops;
    };
    std::unordered_map<std::uint32_t, Buffed> buffed_;
    // The bandits shown and the imbue each wears (game::BanditImbues: its first skill's), kept among its buffs while it
    // is in view (OthersBuffLoops), whatever its buff list says (TakeBuffs).
    std::unordered_map<std::uint32_t, std::uint32_t> banditImbues_;
    void TakeBuffs(const game::proto::EntityBuffs& message, double now);
    void OthersBuffLoops();
    // The skill objects in view (proto::EntityKind::SkillObject): their effects by entity, matched every
    // frame to the view; a strike's stages where one stands.
    struct ShownObject {
        StageEffects::Handle effect = 0;
        glm::vec3 at{0.0f};
    };
    std::unordered_map<std::uint32_t, ShownObject> objectEffects_;
    void SkillObjects();
    glm::vec3 ObjectPoint(const game::WorldView::Entity& e) const;
    void ObjectStruck(const game::proto::SkillObjectStruck& message, double now);
    void ItemGlows(ScreenContext& context);
    struct ItemGlow {
        std::uint32_t lookVersion = 0;
        std::size_t slot = 0;
        std::vector<gfx::EffectSystem::Handle> effects;
    };
    std::unordered_map<std::uint32_t, ItemGlow> itemGlows_; // entity -> its items' glows (gone with the world)
    // States (K3): what each walker is in and until when (the server's
    // EntityStates), the loops of their effects (skilleffect.txt's PARAM_<code>) while they last, their
    // end effect when they end; the target's for the target window, the player's own on its buff
    // board. The knocks (EntityKnocked) and when they end, for the motions.
    struct ShownState {
        game::StateKind kind = game::StateKind::Freeze;
        double since = 0, until = 0;
    };
    struct Afflicted {
        std::vector<ShownState> states;
        std::unordered_map<std::uint8_t, std::vector<gfx::EffectSystem::Handle>> loops; // by kind
        std::size_t slot = StageEffects::kNone; // the slot the loops hang on
    };
    std::unordered_map<std::uint32_t, Afflicted> afflicted_;
    struct Knocked {
        game::proto::KnockKind kind = game::proto::KnockKind::Down;
        double since = 0, until = 0;
        std::uint32_t take = 0;
    };
    std::unordered_map<std::uint32_t, Knocked> knocked_;
    void TakeStates(const game::proto::EntityStates& states, double now);
    void StateLoops(double now);
    bool InState(std::uint32_t entity, game::StateKind kind, double now) const;
    std::vector<game::ActiveEffect> StatesOf(std::uint32_t entity, double now) const;
    void DrawDamage(ScreenContext& context, ui::Painter& paint);
    // How near NPCs, monsters and items must be for their names to show (world units; --name-distance,
    // ours until the options window has it); players' names always show.
    float nameDistance_ = 400.0f;
    // --chat "line|line": typed into the chat's input line once in the world, 2 s apart, from
    // --chat-after S (2); captures wait until 3 s after the last.
    std::vector<std::string> chatScript_;
    double chatAt_ = 2.0, chatHold_ = 0.0;
    // --inventory: the main window on its inventory page once in the world; --mouse x,y: the mouse
    // held there (captures of tooltips); --move-item from,to: one item move, 3 s in.
    bool openInventory_ = false;
    ui::Vec2 mouse_{-1, -1};
    int moveFrom_ = -1, moveTo_ = -1;
    double moveAt_ = 3.0;
    // The cursor, picked every 250 ms by what is under the mouse.
    void PickCursor(ScreenContext& context);
    int cursor_ = 0x95;
    double cursorPicked_ = -1;
    int logoutSeconds_ = 0;
    double logoutTick_ = 0;
    ui::Vec2 hudScreen_{0, 0}; // the screen the HUD was placed for
    // Loading, then a fade in from black.
    LoadingScreen loading_;
    ui::DrawList list_;
    bool shown_ = false;
    float black_ = 1.0f;
    bool leaving_ = false;
    // The test options (above).
    void Script(ScreenContext& context);
    bool walk_ = false, walkOut_ = true;
    float walkX_ = 0, walkZ_ = 0, walkWait_ = 0, leaveAfter_ = -1, logoutAfter_ = -1;
    // K9's tries: a developer's death after so many seconds (--die-after), a resurrection offered
    // accepted at once (--accept-revival), a resurrection cast on a player after so many seconds
    // (--revive "skill,seconds": the nearest other player).
    float dieAfter_ = -1, reviveAfter_ = -1;
    bool acceptRevival_ = false;
    std::uint32_t reviveSkill_ = 0;
    // --cast "skill,S;skill,S": skills used on the selection (none: itself) so many seconds into the
    // world, in turn (tries of others' sight of a player's skills).
    std::vector<std::pair<std::uint32_t, float>> castScript_;
    std::string selectPlayer_; // --select-player NAME: selected once in sight (the scripted casts' target)
    float berserkAfter_ = -1.0f; // --berserk-after S: the berserk request then
    float clickX_ = -1, clickY_ = -1; // --click fx,fy: one click there (fractions of the screen)
    // --click-around S: a click about the character every S seconds, each checked for its walk.
    float clickAround_ = 0, aroundWait_ = 0;
    double aroundCheck_ = 0;
    ui::Vec2 aroundAt_;
    game::proto::WorldPosition aroundLegTo_;
    int aroundClicks_ = 0, aroundMissed_ = 0;
    const char* lastClick_ = "nothing"; // what the last click on the world hit
    std::minstd_rand aroundRandom_{7};
    std::minstd_rand walkRandom_{std::random_device{}()}; // where a click's walk stops short (WalkUpDistance)
    float WalkUpDistance(float nearest, float farthest);
    game::proto::WorldPosition walkHome_;
    // Recording and replaying (C6d).
    game::WorldRecording recording_;
    std::string recordPath_;
    std::unique_ptr<game::WorldReplay> replay_;
    double replayTime_ = 0.0;
    bool replayDone_ = false;
    // The skill bench: what it casts (built once the tables are in), where it is, its pictures.
    std::string benchFilter_, benchCaptures_;
    std::string benchVideo_;    // --bench-video DIR: each skill's frames into DIR/<codename>.mp4 (frame/VideoWriter.h)
    std::size_t benchClip_ = 0; // the step whose video is being written
    int benchView_ = 0; // --bench-view side (1) | close (2)
    bool benchPending_ = false;
    game::SkillBench bench_;
    std::size_t benchStep_ = 0, benchShot_ = 0;
    int benchBurst_ = 0, benchFrame_ = 0; // --bench-burst
    double benchBurstStep_ = 0.05;
    bool benchStarted_ = false, benchOver_ = false;
    std::size_t benchFailedBefore_ = 0;

    // The auto potion (PlayAutoPotion.cpp): the window's Confirm sent, and
    // the scripted tries: --auto-potion (the window filled and confirmed), --auto-potion-window, and
    // --attack-nearest S (a fight to get hurt in; how often it was hurt is counted for the log),
    // --follow NAME (an observer walking after a player).
    void AutoPotionEnter(ScreenContext& context);
    void AutoPotionScript(ScreenContext& context);
    // The scripted walkers' ways round what stands in the way (the quests' tests, the hunt; PlayRoute.cpp,
    // game/NavPath.h): a way asked for is found on a job over a copy of the navigation, then walked a leg at a time,
    // each leg a click (one straight walk, as the server checks it).
    struct RoutePlan;
    struct Route {
        std::shared_ptr<RoutePlan> plan; // the job's, while it searches
        std::vector<game::proto::WorldPosition> legs;
        std::size_t next = 0;
        std::uint32_t target = 0; // the entity the way leads to (0: a place)
        double legAt = 0;         // when the current leg was asked for
        int tries = 0;            // the leg asked again (the walker stopped short of it)
        bool failed = false;      // no way found, or the walker stuck on it
        game::proto::WorldPosition goal;
        std::vector<game::NavAvoid> avoid; // where it stood stuck: kept out of when it searches again
        int searches = 0;
        float arrive = 0.0f; // a point this near the goal will do (NavPathOptions::arrive)
        bool click = false;  // the player's click (Pathfinding): no way found, it walks straight there all the same
        double cutAt = 0;    // when a corner was last tried to be cut
    } route_;
    // A way to `to` (to `target`, an entity, 0 a place): a straight walk is one click, sent by the next frame; else
    // the way is searched for (true). Replaces the way under way.
    // `click`: the player's click (Pathfinding): the best of several searches (game::FindBestNavPath), a corner cut on
    // the way when a straight walk reaches past it, and no way found a straight walk all the same.
    bool RouteTo(const game::proto::WorldPosition& to, std::uint32_t target, float arrive = 0.0f, bool click = false);
    bool Routing() const { return route_.plan != nullptr || route_.next < route_.legs.size(); }
    void RouteFrame(ScreenContext& context);
    bool RouteSearch(); // the way to route_.goal from where the walker stands (false: a straight walk, one leg)
    struct AutoPotionTries {
        std::string script, follow;
        std::string hunt; // only monsters of this codename (empty: any)
        std::map<std::uint32_t, int> pickTries; // a hunt's tries at each ground item (one it cannot take is left after 3)
        std::uint32_t rotation = 0;             // the hunt's next attack skill
        double imbueTried = -1e9;               // when the hunt last asked for its imbue
        float after = 2.0f, attackAfter = -1.0f, followWait = 0.0f;
        bool window = false, reopen = false, shown = false;
        std::uint32_t attacking = 0, lastHp = 0;
        int hurt = 0;
        double targetSince = 0;               // when the hunt chose its monster (one not dead in 30 s is left)
        std::set<std::uint32_t> unreachable;  // monsters the hunt found no way to, or left
    } autoPotionTries_;

    // Parties and party matching (PlayParty.cpp): the scripted tries read at
    // the start; each frame the party's lines, its request box and the matching boxes; the HUD's
    // party requests (and Ctrl + 2..8, a member's slot); a right click on a player (its menu).
    void PartyEnter(ScreenContext& context);
    void PartyFrame(ScreenContext& context);
    bool PartyRequest(const HudRequest& request);
    void PartyRightClick(ScreenContext& context, const ui::Input& in);
    void PartyScript(ScreenContext& context);
    struct PartyTries {
        std::string invite, kick, matchJoin, targetMenu; // player names
        std::uint8_t inviteSettings = 3, setSettings = 0;
        int answer = -1, matchAnswer = -1; // 1 accept, 0 refuse, -1 leave it to the player
        float answerAfter = 1.0f, kickAfter = -1.0f, leaveAfter = -1.0f, setAfter = -1.0f, matchAfter = 1.0f;
        bool page = false, match = false;
        bool registerListing = false;
        game::proto::PartyMatchForm form;
        double nextTry = 0, answerAt = -1, matchAnswerAt = -1;
    } partyTries_;
    bool partyAsking_ = false, partyAskDropped_ = false; // the request box is open; its request went
    std::uint32_t partyOwnSeen_ = 0, partySeen_ = 0;
};

} // namespace sr::client
