// The world screen's Escape menu (WorldScreen.h, "The menu"): the original's system menu and its
// Option window over the test scenes (the Hotan test), so the look can be changed while the crowd
// runs. The HUD manager makes its windows as the game screen's does, all hidden; Escape shows the
// system menu (or closes what is open), its Option button the Option window, its Exit quits.

#include "screens/WorldScreen.h"

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "gui/InterfaceHost.h"
#include "audio/GameAudio.h"
#include "sr/core/Log.h"
#include "sr/game/PlayerState.h"
#include "sr/game/WorldView.h"

namespace sr::client {

struct WorldScreen::Menu {
    InterfaceHost host;
    Hud hud;
    HudContext hudContext;
    game::PlayerState state;
    game::WorldView view;
    ui::Interface* ui = nullptr;
};

void WorldScreen::StartMenu(ScreenContext& context) {
    menu_ = std::make_shared<Menu>();
    menu_->host.Load(context);
}

void WorldScreen::ExitMenu(FrameSnapshot& snapshot) {
    if (!menu_) return;
    menu_->ui = nullptr;
    menu_->host.Exit(snapshot);
    menu_.reset();
}

bool WorldScreen::MenuHasMouse() const { return menu_ && menu_->ui != nullptr && menu_->host.context().mouseOverUi(); }

void WorldScreen::UpdateMenu(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!menu_ || context.input == nullptr) return;
    Menu& m = *menu_;
    HudContext& h = m.hudContext;
    h.state = &m.state;
    h.world = &m.view;
    h.hud = &m.hud;
    h.time = context.time;
    h.text = [this](const std::string& key) { return menu_->host.data().Text(key); };
    h.hasText = [this](const std::string& key) { return menu_->host.data().HasText(key); };
    h.regionKey = [this](std::uint16_t region) { return menu_->host.data().RegionKey(region); };
    h.options = context.options;
    h.optionsChanged = [&context] { context.optionsChanged = true; };
    h.audioPreview = [&context](const core::AudioOptions& levels) {
        if (context.audio != nullptr) context.audio->SetLevels(levels);
    };
    const ui::Vec2 screen{float(context.device.SwapchainWidth()), float(context.device.SwapchainHeight())};
    if (m.ui == nullptr) {
        m.ui = m.host.Prepare([&h](ui::Interface& made) { RegisterHudWindows(made, h); });
        if (m.ui == nullptr) return; // the layouts are still loading
        m.hud.Create(*m.ui, screen);
        for (ui::Widget* w : m.ui->Windows()) w->visible = false; // nothing until Escape
        // --menu system|options: open at the start (captures).
        const std::string open = context.config.GetString("menu", "");
        if (open == "system") m.hud.Escape();
        if (open == "options") m.hud.OpenOptions();
    }
    m.host.Frame(context, snapshot, *context.input, 1.0f);
    if (context.input->KeyPressed(ui::Key::Escape) && !context.keyboardBlocked && !m.host.context().typing()) m.hud.Escape();
    for (const HudRequest& r : m.hud.TakeRequests()) {
        if (r.kind == HudRequest::Kind::Quit) {
            core::LogInfo("world: Exit from the menu");
            context.quit = true;
        }
    }
}

} // namespace sr::client
