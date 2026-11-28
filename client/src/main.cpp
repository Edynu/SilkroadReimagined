// SroReimagined - the rewritten client.
//
// Settings come from SroReimagined.cfg in the working directory (key = value lines), then the
// command line (--key value, or --flag). --game falls back to $SR_GAME_DIR.
//
//   --game <dir> --override <dir> --size WxH --log <file>
//   --cursors <sro_client.exe>: the executable the cursors are read from (default: the game folder's), once: they are kept
//   as PNG files in --cursor-dir DIR (default client/assets/cursors, git-ignored) and read there from then on
//   --window-mode 0|1|2           windowed, borderless or fullscreen for this run (default: the Option window's)
//   --screen logo|crowd|viewer|world|dungeon|scenebench|empty   the first screen (default logo; then --after-logo, default crowd)
//   --workers N                   job workers (default: hardware threads - 3; 0: run jobs inline)
//   --render-thread false         render on the game thread, through the same snapshots
//   --frame-limit N               frames a second (0: the display's rate, -1: none); --vsync false
//   --look classic|modern-low|modern-high|modern-ultra   this run's look (Classic or the Modern rows)
//   --video key=value,...         single video rows after the look (sunShadows=2,viewDistance=0, ...)
//   --game-rows key=0|1,...       the Game page's rows for this run (guide=1: the guide's tips every minute, ...)
//   --validation, --sync-validation   Vulkan validation layers (the second adds synchronisation checks)
//   --tools false                 hide the developer tools (F1)
//   --audio false                 no sound (the device is not opened)
//   --fixed-dt S                  advance S seconds a frame (repeatable captures)
//   --capture <file.png> [--capture-frame N]   save frame N (default 10; later while the screen is loading,
//                                              at most --capture-wait N frames) and exit
//   --run-seconds S [--bench-out <file>]       run S seconds, then print frame statistics
//   --screen-cycle N              switch screens N rounds and check for leaked resources
//   --window-test                 resize, change modes, minimise and restore, check rendering goes on
//   crowd benchmark: --characters N --burst-every-ticks N --burst-size N --record-chunks N
//                    --simulation-rate HZ --apply-budget-ms MS --hash-step N
//   viewer: --open <path> (.bsr .cpd .bms .ddj .dds) --animation N
//   world (C3 test scenes): --region X,Y (78,105) --world-radius R (2) --camera x,y,z,yaw,pitch
//                           --characters N [--character-models a.bsr,...] --clock H (12) --time-of-day t
//                           --day N --view-distance 0..4 (4) --glow false --lens-flare false --hud
//                           --weather rain|snow|clear --weather-amount 0..255 (128) --lightning-at S
//                           --teleport-to X,Y [--teleport-at S (5)] --capture-loading
//                           --effect x.efp [--effects N] [--effect-bone name|name@item.bsr] [--effect-scale S]
//                           the walkers (--characters N): --walker-players 0..1 --walker-skill-every S
//                           --walker-monsters K (K monster kinds from characterdata); a close-up:
//                           --walker-at X,Z (the first stands there, the second 60 east as its target)
//                           [--walker-items ITEM_..,ITEM_..[,woman][,height:N][,volume:N][,figure:N]] [--walker-skill SKILL_..] (its items, the one skill it casts)
//   dungeon (C4d): --dungeon 0x8001|0x8009 (or the dungeoninfo id) --block N --camera x,y,z,yaw,pitch --glow false
//   scene benchmark (GPU-driven world): --instances N --models a.bsr,b.bsr
//   --asset-check bsr,bms,ddj [--asset-window N] [--asset-delay S]   stream every file of those types, report, exit
//   --name-distance N             names over NPCs, monsters and items within N world units of the player (default 400)
//   guilds and unions in the world: --guild "step;step;..." one step each --guild-step S (2) from
//                           --guild-after S (3): walk-npc (to the nearest guild NPC), npc-menu, npc-choose:N, name:NAME
//                           (the name box), yes / no (the confirmation box), create:NAME, invite:PLAYER, target:PLAYER,
//                           notice:TITLE|BODY, kick:NAME, leave, disband, master:NAME, permit:NAME=BITS, donate:N,
//                           levelup, chat:TEXT, uchat:TEXT, union-invite:PLAYER, union-leave, union-kick:GUILD,
//                           window[:union], crest, crest-delete, union-crest, union-crest-delete (the bag's emblem
//                           item used; yes answers its box), npc-row:KEY (the NPC window's row of that text),
//                           promote-ok / promote-cancel (the guild promotion window), buy:TAB,SLOT,COUNT (the open store),
//                           hide:ID (a window closed), write-notice:TITLE|BODY (the editor), donate-window:N (the GP button's window),
//                           position:MEMBER,VALUE (a fortress position given, 0 takes it away),
//                           global:TEXT (a Global chatting item of the bag used to say it),
//                           pick:NAME / pick-confirm:NAME (the transfer or election window), war:GUILD,victory,days,
//                           hours,minutes,wager, war-end:GUILD, war-reward, strike:NAME,DAMAGE, attack:NAME,
//                           window:hostile, wait; --guild-accept | --guild-decline answers proposals (a war's too)
//                           without the box; --crest-dir DIR: guildmark.bmp / unionmark.bmp looked for there first
//   streaming memory, reserved at start: --staging-mib N (64) --texture-reserve-mib N (1024) --geometry-mib N (512)
//   --skin-arena-mib N (128): the frame's skinned vertices, 32 bytes each (shadows get half); parts past it are not drawn
//   parties in the game (PlayParty.cpp): --party-invite NAME [--party-settings exp,item,anyone]
//     --party-accept | --party-decline [--party-answer-after S (1)] --party-kick NAME [--party-kick-after S (3)]
//     --party-leave-after S --party-set exp,item [--party-set-after S (3)] --party-page --party-match --target-menu NAME
//     matching: --match-register TITLE [--match-purpose 0..3] [--match-levels A,B] [--match-settings exp,item]
//     [--match-after S (1)] --match-join LEADER --match-accept | --match-refuse
//   the gallery's party windows: --screen gallery --hud --page party|party-match|party-register|party-mode|party-request|party-invite|target-menu
//   the auto potion (PlayAutoPotion.cpp): --auto-potion "hp,70,ITEM_..;mp,50,ITEM_..;abnormal,ITEM_..;delay,1000"
//     (the window opened, each row's item dropped in from the bag, ticked at its percent, Confirm; "mp,off" unticks a row)
//     [--auto-potion-after S (2)] --auto-potion-window (left open) --attack-nearest S (the nearest monster from S s on,
//     again whenever it is gone) --follow NAME (an observer walking after a player, its camera on it)
//   the gallery's auto potion window: --screen gallery --hud --page auto-potion
//   the gallery's others: --screen gallery --hud --page inventory|practice|practice-mastery|rebirth (--mouse x,y for a hover)
//   the gallery's equipment: --hud --page equipment (every worn place filled) | dress (the dress side
//     with five avatars) | dress-plain (a dress that takes no attachment: its slot veiled); --mouse on a slot for its tooltip
//   the fortress war (PlayFortress.cpp): --fortress "step;step" [--fortress-after S (3)]
//     [--fortress-step S (2)], the steps: walk-official (to the nearest fortress official), walk:X,Z (in the region
//     it stands in), window (the official's window), press:F (its row's button for fortress F, the question
//     answered yes), apply:F, defend:F, cancel:F, dev:startreq|endreq|startwar|endwar (the GM's /SIEGE; the server
//     needs --dev-commands), goto-fortress:F (beside its heart; developer), goto:N (a teleport point; developer),
//     attack:heart|tower|gate, select:heart|tower|gate, attack-player:NAME, siege:F (each second the nearest
//     guard tower, then the heart, until the player's guild holds F), until-held:F, until-owner:F, action:ID (an
//     Action page icon), drop:PLACE, wait; a capture waits for the last step (--capture-wait frames at most)
//   the exchange, steps of --fortress and --guild: exchange:NAME (asks that player),
//     put:PLACE, take:SLOT, gold:N, confirm (the window's button: Confirm, then Approve), exchange-cancel; a request
//     is answered by --guild-accept | --guild-decline; the gallery: --hud --page exchange | exchange-confirmed
//   the gallery's fortress official's window: --screen gallery --hud --page fortress-apply

#include <cstdlib>
#include <string>

#include "App.h"
#ifdef SR_PLAYER_BUILD
#include "PlayerStart.h"
#endif
#include "sr/core/Config.h"
#include "sr/core/Log.h"

int main(int argc, char** argv) {
#ifdef SR_PLAYER_BUILD
    // A player's start (PlayerStart.h): beside the exe, its game folder asked for once, errors in a message box.
    sr::client::UseExeFolder();
#endif
    sr::core::Config config;
    std::string error;
    if (!config.LoadFile("SroReimagined.cfg", error) || !config.ParseCommandLine(argc, argv, error)) {
#ifdef SR_PLAYER_BUILD
        sr::client::ShowStartError("SroReimagined.cfg: " + error);
#else
        sr::core::LogError("{}", error);
#endif
        return 2;
    }
#ifdef SR_PLAYER_BUILD
    if (!sr::client::PreparePlayerStart(config)) return 2;
    const int code = sr::client::Run(config);
    if (code != 0) {
        sr::client::ShowStartError("SroReimagined stopped (code " + std::to_string(code) +
                                   "). What happened is written in SroReimagined.log beside it.");
    }
    return code;
#else
    if (!config.Has("game")) {
        if (const char* env = std::getenv("SR_GAME_DIR")) config.Set("game", env);
    }
    if (!config.Has("game")) {
        sr::core::LogError("no game folder: pass --game <dir>, set SR_GAME_DIR, or add game = <dir> to SroReimagined.cfg");
        return 2;
    }
    return sr::client::Run(config);
#endif
}
