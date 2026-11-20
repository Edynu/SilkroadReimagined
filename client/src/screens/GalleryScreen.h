#pragma once

// The interface gallery (C7b): any of the original's layouts opened as a window
// of the game's interface (sr::ui::Interface), on sample data, to compare with the original.
//
// Options: --open a,b,... (layout files: "ifunderbar" or "ifunderbar.txt", optionally ":Section"),
// --ui-scale S (default 1, the original's pixels), --samples false (the layouts bare), --mouse x,y
// (the mouse held there, for captures of hover states), --click x,y[;x,y...] (clicks there, in
// turn, a few frames apart), --hud (the HUD as the game starts it, at
// the screen's size, on a sample character: C7c), --backdrop r,g,b (the colour behind the windows,
// 0..255: a bright one shows what a window leaves transparent). The F1 tools list every layout to open.

#include <memory>
#include <string>
#include <optional>
#include <set>
#include <vector>

#include "frame/Portraits.h"
#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "gui/InterfaceHost.h"
#include "screens/Screen.h"
#include "sr/assets/Terrain.h"
#include "sr/assets/Gacha.h"
#include "sr/core/Jobs.h"

namespace sr::client {

struct CharacterData;

class GalleryScreen final : public Screen {
public:
    const char* Name() const override { return "gallery"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void OpenLayout(const std::string& spec);
    void DevTools();
    // --hud: the sample character's portrait, dressed from the game's
    // tables (read on a job).
    void SamplePortraits(ScreenContext& context, FrameSnapshot& snapshot);
    Portraits portraits_;
    std::shared_ptr<CharacterData> tables_;
    core::JobCounter tablesJob_;
    bool dressed_ = false;
    PortraitLook selfLook_;

    InterfaceHost host_;
    ui::Interface* interface_ = nullptr;
    // --hud: the HUD on a sample character.
    bool hudMode_ = false;
    int useSlot_ = 0; // --use-slot N: quick slot N used once the windows are in (the keys' mark)
    Hud hud_;
    game::PlayerState sample_;
    // --page skills (with --hud): the main window on that page; the Skills page's tables.
    std::string page_;
    assets::SkillTable skills_;
    assets::StallNetworkData stallNetwork_; // the stall network window's classes and types
    std::vector<std::uint32_t> masteryCosts_;
    game::WorldView sampleWorld_;
    HudContext hudContext_;
    game::CommunityView sampleCommunity_; // the Community pages' sample friends, blocks and letters
    game::GuildView sampleGuild_; // the guild window's sample guild and union
    game::FortressView sampleFortress_; // the fortress official's window on sample data
    bool autoPotionShown_ = false; // --page auto-potion: the window shown on the sample bag
    int flashAt_ = 0; // --page item-flash: the frames since its effects started
    std::set<std::uint32_t> sampleBook_; // --page collection-flip: the sample character's collection book
    bool exchangeShown_ = false;   // --page exchange: the exchange window on a sample exchange
    bool mallShown_ = false;       // --page mall[:...]: the item mall (HudMall.cpp)
    assets::MallTable mallTable_;
    MallView mall_;
    bool popShown_ = false;        // --page magic-pop[:STATE]: Magic POP's window (HudMagicPop.cpp)
    assets::GachaTable gacha_;
    bool cosShown_ = false;        // --page cos | cos-two: the COS manager on sample COS
    game::ExchangeView sampleExchange_;
    game::FlagView sampleFlag_;  // --page ctf-*: Capture the Flag's pieces on a sample match (HudFlagWorld.cpp)
    bool ctfShown_ = false;
    game::ArenaView sampleArena_; // --page arena-*: the Battle Arena's windows on a sample match (HudArena.cpp)
    bool arenaShown_ = false;
    bool templeShown_ = false; // --page temple-reputation
    bool fortressWorkShown_ = false; // --page fortress-business, -intro, -make...: the manager's and the forges' windows
    bool fortressMapShown_ = false; // --page fortress-map: the battle aide's map shown on Jangan's places
    std::vector<assets::NamedSpot> spots_; // navmesh\objectstring.ifo (--page fortress-map)
    std::vector<std::string> pending_; // --open, once the data is in
    float scale_ = 1.0f;
    bool samples_ = true;
    float backdrop_[3] = {0.17f, 0.18f, 0.20f};
    ui::Vec2 mouse_{-1.0f, -1.0f};
    // --click x,y[;x,y...]: left clicks there, one after the other, once the windows are in (then
    // the mouse stays at the last).
    std::vector<ui::Vec2> clicks_;
    std::vector<std::optional<ui::Vec2>> dragsTo_; // a click's drag to there (d:x0,y0,x1,y1), else none
    ui::Input script_; // the clicks' input, kept from frame to frame while they run
    bool scripting_ = false;
    std::size_t clickIndex_ = 0;
    int clickStep_ = 0;
    ui::Vec2 flow_{12.0f, 12.0f}; // where the next window goes
    float rowHeight_ = 0.0f, screenWidth_ = 1024.0f;
    int opened_ = 0;
    char filter_[64] = {};
};

} // namespace sr::client
