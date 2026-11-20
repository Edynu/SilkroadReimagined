// The item mall's services on the game screen: the script's steps for the live tests.
// The windows, the picks and the answers are HudServices.cpp's; the requests go out through the HUD context.
//
// Steps (of --guild and --fortress):
//   service-use:CODE         the bag's first CODE used as a right click uses it (the skin change scroll's window, the
//                            restoration potion's withdrawal window, the gender switch tool's pick, the hammer's box,
//                            a magic cube's question)
//   service-pick:CODE        the bag's first CODE clicked in the pick mode (the gender switch's armour, the store's Repair)
//   service-yes, service-no  the box answered
//   repair                   the open store's Repair (its pick); repair-all: its Repair all (the box)
//   skin:SEX,FIGURE,HEIGHT,VOLUME   the skin change window's choice (SEX m or w; the figure's index from 0); skin-ok: OK
//   withdraw:SKILLCODE[,N]   the removal box for that learned skill's group, its down arrow pressed N times (1 by
//                            default); withdraw-mastery:ID[,N]; service-yes / service-no its OK / Cancel
//   cube-put:CODE            the bag's first CODE dropped on the bag's first magic cube

#include <algorithm>
#include <cstdlib>
#include <string>

#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"
#include "sr/game/MallServices.h"

namespace sr::client {

bool PlayScreen::ServiceScriptStep(const std::string& verb, const std::string& arg) {
    if (verb == "service-use") {
        const int place = BagPlaceOf(arg);
        if (place < 0 || !UseServiceItem(hudContext_, place)) core::LogWarning("game: service-use: no {} to use", arg);
        return true;
    }
    if (verb == "service-pick") {
        const int place = BagPlaceOf(arg);
        if (place < 0 || !ServicePick(hudContext_, place)) core::LogWarning("game: service-pick: {} not picked", arg);
        return true;
    }
    if (verb == "service-yes" || verb == "service-no") {
        // The removal box's OK / Cancel when it is up, else the open question.
        if (!WithdrawBoxStep(hudContext_, 0, true, verb == "service-yes")) hud_.AnswerConfirm(verb == "service-yes");
        return true;
    }
    if (verb == "repair") {
        hudContext_.servicePick = 1;
        hudContext_.servicePickNpc = player_.shopNpc;
        return true;
    }
    if (verb == "repair-all") {
        if (player_.shopNpc == 0) core::LogWarning("game: repair-all: no store open");
        else AskRepairAll(hudContext_, player_.shopNpc);
        return true;
    }
    if (verb == "skin" || verb == "skin-ok") {
        int values[4] = {0, 0, 2, 2};
        bool male = true;
        if (verb == "skin") {
            std::size_t at = 0;
            for (int k = 0; k < 4 && at <= arg.size(); ++k) {
                const std::size_t comma = std::min(arg.find(',', at), arg.size());
                const std::string part = arg.substr(at, comma - at);
                if (k == 0) male = part != "w";
                else values[k] = std::atoi(part.c_str());
                at = comma + 1;
            }
        }
        if (!ChooseSkin(hudContext_, male, values[1], values[2], values[3], verb == "skin-ok")) core::LogWarning("game: {}: no skin change window", verb);
        return true;
    }
    if (verb == "withdraw") {
        // withdraw:SKILLCODE[,N]: the removal box for that skill's group, its down arrow pressed N times (1 by default).
        const std::size_t comma = arg.find(',');
        const std::string code = arg.substr(0, comma);
        const int levels = comma == std::string::npos ? 1 : std::max(0, std::atoi(arg.c_str() + comma + 1));
        const assets::SkillRow* skill = tables_ && tables_->Usable() ? tables_->skills.Find(std::string_view(code)) : nullptr;
        if (skill == nullptr) {
            core::LogWarning("game: withdraw: no skill {}", code);
            return true;
        }
        // The group's learned row.
        const assets::SkillRow* learned = nullptr;
        for (const std::uint32_t id : player_.skills) {
            const assets::SkillRow* row = tables_->skills.Find(id);
            if (row != nullptr && row->group == skill->group) learned = row;
        }
        if (learned == nullptr) {
            core::LogWarning("game: withdraw: {} not learned", code);
        } else {
            AskWithdraw(hudContext_, learned, 0);
            WithdrawBoxStep(hudContext_, levels, false, false);
        }
        return true;
    }
    if (verb == "withdraw-mastery") {
        // withdraw-mastery:ID[,N]
        const std::size_t comma = arg.find(',');
        const int levels = comma == std::string::npos ? 1 : std::max(0, std::atoi(arg.c_str() + comma + 1));
        AskWithdraw(hudContext_, nullptr, std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10)));
        WithdrawBoxStep(hudContext_, levels, false, false);
        return true;
    }
    if (verb == "cube-put") {
        int cube = -1;
        for (const auto& [at, item] : player_.inventory.places) {
            const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (at >= game::slot::kWorn && row != nullptr && game::IsMagicCube(*row)) {
                cube = at;
                break;
            }
        }
        const int from = BagPlaceOf(arg);
        if (cube < 0 || from < 0 || !CubeDrop(hudContext_, from, cube)) core::LogWarning("game: cube-put: {} not put", arg);
        return true;
    }
    return false;
}

} // namespace sr::client
