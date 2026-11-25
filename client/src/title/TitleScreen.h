#pragma once

// The title screen (C5e): the original's resinfo\pstitle.txt - the
// letterbox bars, the logo, the login window (account, password, the chosen server) and the
// server list - over our own title session (gateway, then login), in front of the intro flight
// (script\intro\constantinople.txt: the world seen from its camera keys, once the regions are
// in; then held on the last key). When the lobby is entered, the session goes on to the character
// screen.
//
// Options: --gateway HOST:PORT (default 127.0.0.1:15779), --server-certificate FILE (the
// certificate the servers' chains end in; default dev\server.pem), --server-name NAME (the name
// their certificate carries; default localhost). Automated runs: --account, --password, --server
// (an id) log in once the server list arrives; --intro-at S starts the intro flight S seconds in.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "frame/GameUi.h"
#include "screens/Screen.h"
#include "screens/WorldScreen.h"
#include "sr/assets/IntroScript.h"
#include "sr/ui/Context.h"
#include "title/ScreenFade.h"
#include "title/TitleShared.h"
#include "title/UiKit.h"
#include "world/LoadingScreen.h"

namespace sr::client {

class TitleScreen final : public Screen {
public:
    const char* Name() const override { return "title"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void HandleEvents();
    void Draw(ScreenContext& context, float width, float height);
    void DrawLoginWindow(const assets::UiLayout& layout, float cx, float cy);
    void DrawServerList(const assets::UiLayout& layout, float cx, float cy);
    std::uint16_t lobbyServer_ = 0;
    void Submit();
    void UpdateIntro(ScreenContext& context, FrameSnapshot& snapshot);

    std::shared_ptr<TitleShared> shared_;
    // The intro flight: its keys read on a job, the world behind them.
    struct Intro {
        std::atomic<bool> ready{false};
        std::vector<assets::CameraKey> keys;
    };
    std::shared_ptr<Intro> intro_;
    core::JobCounter introJob_;
    std::unique_ptr<WorldScreen> world_;
    float introTime_ = 0.0f;
    bool shown_ = false; // the world, interface data and art were in: shown since, never black again
    // Until then the game's loading screen (Constantinople's picture, the gauge: ours, inferred; the
    // original's start shows its launcher before the client).
    LoadingScreen loading_;
    ui::DrawList loadingList_;
    void HideUntilShown(ScreenContext& context, FrameSnapshot& snapshot);
    // Fading in from black when shown, out to black before the lobby.
    ScreenFade fade_;
    std::string leaving_;
    std::unique_ptr<UiTextures> textures_;
    std::unique_ptr<UiKit> kit_;
    ui::Context ui_;
    FontAtlas atlas_;

    std::string account_, password_;
    std::uint16_t draftServer_ = 0;
    bool serverListOpen_ = false;
    bool focusAccount_ = true;
    bool busy_ = false;
    bool quitAsked_ = false; // the login box's Cancel: the game ends
    bool autoLogin_ = false;
    std::string message_;
    // A login refusal waiting for the text table to be put in words (the original's, by its login answer's codes).
    struct Refusal {
        game::proto::LoginStatus status = game::proto::LoginStatus::Busy;
        std::uint8_t attempts = 0, attemptLimit = 0;
        std::string reason;
        std::int64_t blockedUntil = 0;
    };
    std::optional<Refusal> refusal_;
    static std::string RefusalWords(const UiData& d, const Refusal& r);
    // The shard turned this client away as out of date (its words, ClientGate.h on the servers): a box over the title
    // instead of the login, with the download page and Exit.
    std::string outdated_;
    void DrawOutdated(ScreenContext& context);
    bool connected_ = false;
};

} // namespace sr::client
