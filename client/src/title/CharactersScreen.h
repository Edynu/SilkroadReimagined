#pragma once

// The character lobby (C5e): the account's characters on the chosen game server, and creating,
// deleting, restoring and selecting them over the title session the title screen handed over.
// The original's character select: the characters on the wharf's dock,
// its buttons where the original puts them; choosing one brings the camera to it. Creating is still
// a plain form until the create screens.
//
// Automated runs: --create NAME creates a character (the first valid look of --race china|europe,
// --sex man|woman) once the list arrives, --select NAME selects one, --exit-on-select ends the
// program with 0 when the selection is accepted (1 on a refusal).

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "frame/GameUi.h"
#include "screens/Screen.h"
#include "screens/WorldScreen.h"
#include "sr/assets/GameData.h"
#include "sr/assets/IntroScript.h"
#include "sr/game/CharacterRules.h"
#include "sr/ui/Context.h"
#include "title/ScreenFade.h"
#include "title/TitleShared.h"
#include "title/UiKit.h"

namespace sr::client {

class CharactersScreen final : public Screen {
public:
    const char* Name() const override { return "characters"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void HandleEvents(ScreenContext& context);
    void Draw(ScreenContext& context, float width, float height);
    void Create(const std::string& name, game::CreationRules::Race race, bool male);
    std::string StatusText(game::proto::LobbyStatus status) const;

    std::shared_ptr<TitleShared> shared_;
    std::unique_ptr<UiTextures> textures_;
    std::unique_ptr<UiKit> kit_;
    ui::Context ui_;
    FontAtlas atlas_;
    std::shared_ptr<CharacterData> tables_; // the title's character tables (TitleShared::LoadCharacters)

    // The dock: the wharf of region 81,105, the characters on their slots.
    std::unique_ptr<WorldScreen> world_;
    std::vector<assets::CameraKey> track_; // the camera's current track: arriving, choosing, going back
    float trackTime_ = 0.0f;
    bool stageDirty_ = true;
    unsigned placed_ = 0;  // the dock slots given a character (bit per slot)
    bool shown_ = false;   // the wharf and its characters were in once: no black after that
    double enteredAt_ = -1.0; // the screen's first update (its time to show is logged)
    ScreenFade screenFade_; // in from black when shown; out to black before the title
    std::string leaving_;
    bool DockReady() const;
    // Choosing a character (empty: none) moves the camera to it; the buttons change with it.
    void Choose(const std::string& name);
    // Where the lobby is: the dock; on the way to the pedestal; at the
    // pedestal picking a race between its idols; a race picked (the camera closes in under a fade
    // to black); on the way back to the dock.
    enum class Stage { Dock, ToPedestal, Pedestal, Picked, ToDock };
    Stage stage_ = Stage::Dock;
    int hoveredIdol_ = -1; // 0 Europe, 1 China (the idols' order on the pedestal)
    int race_ = -1;        // the picked one
    float fade_ = 0.0f;    // 0..1: the black over the picked race's close-up
    void GoToPedestal();
    void GoToDock(float seconds);
    void PickRace(int race);
    void DrawPedestal(float width, float height, float T);
    void UpdateDock(ScreenContext& context, FrameSnapshot& snapshot);
    void PlaceCharacters(ScreenContext& context);
    // The weapons' and shields' effects (a SoX's glow, the sparks from +8: StageEffects::ItemGlows), as the game
    // screen's, on each dock slot once its model shows. They and the plus glow need the title's later tables
    // (CharacterData::Ready): characters placed before those are in are dressed again once they are.
    void StartItemGlows(ScreenContext& context);
    void StopItemGlows();
    bool dressedWithGlows_ = false;
    unsigned glowing_ = 0; // the dock slots whose effects were started (bit per slot)
    std::array<std::vector<gfx::EffectSystem::Handle>, 4> glows_;
    // A point of the dock (origin-relative) on the screen; false behind the camera.
    bool Project(float width, float height, float x, float y, float z, ui::Vec2& out) const;

    std::optional<game::proto::CharacterList> list_;
    std::string selected_;
    std::string message_;
    bool busy_ = false;
    // The deletion's or the restoration's question (the layout's Warning section, 1.188's popup): open, and which.
    bool warning_ = false, warningRestore_ = false;
    bool kicked_ = false; // the server said why it closes the connection (message_ keeps it)
    // Automated creation (--create with --race and --sex), straight to the server.
    bool europe_ = false, male_ = true;
    // Automated runs.
    std::string autoCreate_, autoSelect_;
    std::string autoAsk_; // --ask-delete NAME: that one chosen and its Delete (or Restore) pressed, for a look at its question
    bool askNow_ = false;
    bool autoPedestal_ = false; // --pedestal: press Create once the list is in (--pedestal-after-restart: back from a Restart)
    int autoPick_ = -1;         // --pick europe|china: pick that race at the pedestal
    bool exitOnSelect_ = false;
};

} // namespace sr::client
