// Magic POP on the game screen: the script's steps for the live tests. The window and the
// answers are HudMagicPop.cpp's.
//
//   talk:NPC_CH_GACHA_MACHINE, then pop-choose:ENTRY   the machine's row ("Participate in the game."), the prize of that
//                                                      GachaID chosen through the selection window and the strip
//   pop-card:PLACE | pop-card:CODENAME                 a card of the bag on the card's slot (a codename: its first)
//   pop-play                                           PLAY (the request goes 4 s later)
//   talk:NPC_CH_GACHA_OPERATOR, then pop-exchange      Gori's row (every coupon exchanged)

#include <cctype>
#include <cstdlib>

#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

bool PlayScreen::PopScriptStep(const std::string& verb, const std::string& arg) {
    auto* npc = dynamic_cast<NpcWindow*>(hud_.Shown(hud::kNpc) ? hud_.Show(hud::kNpc) : nullptr);
    if (verb == "pop-choose") {
        if (npc != nullptr) npc->ChooseText(hudContext_.Text("SN_TALK_CH_GACHA_MACHINE_2"));
        if (!MagicPopChoose(hudContext_, std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10)))) core::LogWarning("game: pop-choose:{}: no such prize here", arg);
    } else if (verb == "pop-card") {
        const int place = !arg.empty() && std::isdigit(static_cast<unsigned char>(arg[0])) ? std::atoi(arg.c_str()) : BagPlaceOf(arg);
        if (!MagicPopCard(hudContext_, place)) core::LogWarning("game: pop-card:{}: no card there, or no prize chosen", arg);
    } else if (verb == "pop-play") {
        if (!MagicPopPlay(hudContext_)) core::LogWarning("game: pop-play: not ready");
    } else if (verb == "pop-exchange") {
        if (npc == nullptr || !npc->ChooseText(hudContext_.Text("SN_TALK_CH_GACHA_OPERATOR_2"))) core::LogWarning("game: pop-exchange: no Gori's window");
    } else {
        core::LogWarning("game: {}: not a Magic POP step", verb);
    }
    return true;
}

} // namespace sr::client
