// Alchemy on the game screen: the script's steps for the live tests. The window itself and its
// answers are HudAlchemy.cpp's; its requests go out through the HUD context.

#include "sr/core/Log.h"
#include "play/PlayScreen.h"

#include <cstdlib>
#include <utility>

#include "gui/HudWindows.h"

namespace sr::client {

namespace {
// "CODE,N" as the codename and the number after its last comma (-1 none).
std::pair<std::string, int> CodeAndNumber(const std::string& arg) {
    const std::size_t comma = arg.rfind(',');
    if (comma == std::string::npos) return {arg, -1};
    return {arg.substr(0, comma), std::atoi(arg.c_str() + comma + 1)};
}
} // namespace

bool PlayScreen::AlchemyScriptStep(const std::string& verb, const std::string& arg) {
    if (verb.starts_with("socket-") || verb == "alchemy-page" || verb == "wear") return SocketScriptStep(verb, arg);
    if (verb.starts_with("pop-")) return PopScriptStep(verb, arg); // Magic POP's (PlayMagicPop.cpp)
    if (MallScriptStep(verb, arg)) return true;                    // the item mall's (PlayMall.cpp)
    if (verb != "alchemy" && verb != "alchemy-tab" && verb != "alchemy-put" && verb != "alchemy-go" && verb != "alchemy-yes" && verb != "alchemy-no") {
        return false;
    }
    if (verb == "alchemy") ToggleAlchemyWindow(hudContext_);
    if (verb == "alchemy-tab") AlchemyShowTab(hudContext_, std::atoi(arg.c_str()));
    if (verb == "alchemy-go") AlchemyPress(hudContext_);
    if ((verb == "alchemy-yes" || verb == "alchemy-no") && !AlchemyAnswer(hudContext_, verb == "alchemy-yes")) core::LogWarning("game: {}: no box", verb);
    if (verb == "alchemy-put") {
        // The bag's first item of that codename.
        const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(std::string_view(arg)) : nullptr;
        int place = -1;
        for (const auto& [at, item] : player_.inventory.places) {
            if (at >= game::slot::kWorn && row != nullptr && item.refId == row->refObjId) {
                place = at;
                break;
            }
        }
        if (place < 0) core::LogWarning("game: alchemy-put: no {} in the bag", arg);
        else AlchemyPut(hudContext_, place);
    }
    return true;
}

int PlayScreen::BagPlaceOf(const std::string& code) const {
    const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(std::string_view(code)) : nullptr;
    for (const auto& [at, item] : player_.inventory.places) {
        if (at >= game::slot::kWorn && row != nullptr && item.refId == row->refObjId) return at;
    }
    return -1;
}

// The Socket page's steps: alchemy-page:socket|alchemy, socket-put:CODE (the bag's first of
// it into the page), socket-make:N (Make, then socket N clicked), socket-insert:CODE,N (that stone dropped on socket N;
// "alchemy-yes" answers the box), socket-erase:N (Remove, then socket N; "alchemy-yes"), socket-cancel (an
// insertion's Cancel), socket-wear:CODE (the bag's first of it worn); the worn item's skills (window 500):
// socket-skills:PLACE (a right click on that worn place),
// socket-quick:N,SLOT (its skill N dragged to quick slot SLOT), socket-cast:N (its skill N used, on the selection).
bool PlayScreen::SocketScriptStep(const std::string& verb, const std::string& arg) {
    if (verb == "alchemy-page") {
        AlchemyShowSocket(hudContext_, arg == "socket");
    } else if (verb == "socket-put") {
        const int place = BagPlaceOf(arg);
        if (place < 0) core::LogWarning("game: socket-put: no {} in the bag", arg);
        else AlchemyPut(hudContext_, place);
    } else if (verb == "socket-make") {
        AlchemySocketMake(hudContext_, std::atoi(arg.c_str()));
    } else if (verb == "socket-insert") {
        const auto [code, index] = CodeAndNumber(arg);
        const int place = BagPlaceOf(code);
        if (place < 0) core::LogWarning("game: socket-insert: no {} in the bag", code);
        else AlchemySocketInsert(hudContext_, place, index);
    } else if (verb == "socket-erase") {
        AlchemySocketErase(hudContext_, std::atoi(arg.c_str()));
    } else if (verb == "socket-cancel") {
        AlchemySocketCancel(hudContext_);
    } else if (verb == "wear") {
        // wear:CODE: the bag's first of it dropped on its equipment place, through the HUD's own request (as a drag: the
        // equipment's checks and warnings, e.g. a PvP suit with a growth pet out).
        const int place = BagPlaceOf(arg);
        const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(std::string_view(arg)) : nullptr;
        const int wornAt = row != nullptr ? game::EquipPlaceFor(*row, player_.inventory.At(game::slot::kRingLeft) != nullptr,
                                                                player_.inventory.At(game::slot::kRingRight) != nullptr)
                                          : -1;
        if (place < 0 || wornAt < 0) core::LogWarning("game: wear: no {} to wear", arg);
        else hud_.Request({HudRequest::Kind::MoveItem, place, {}, {}, wornAt});
    } else if (verb == "socket-wear") {
        // The bag's first of it worn, as dropped on the equipment (the alchemy window closed first: it holds the bag).
        const int place = BagPlaceOf(arg);
        const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(std::string_view(arg)) : nullptr;
        const int wornAt = row != nullptr ? game::EquipPlaceFor(*row, player_.inventory.At(game::slot::kRingLeft) != nullptr,
                                                                player_.inventory.At(game::slot::kRingRight) != nullptr)
                                          : -1;
        if (place < 0 || wornAt < 0) core::LogWarning("game: socket-wear: no {} to wear", arg);
        else shared_->session->MoveItem({std::uint8_t(place), std::uint8_t(wornAt), 0});
    } else if (verb == "socket-skills") {
        const bool shown = ShowWornSocketSkills(hudContext_, std::atoi(arg.c_str()));
        core::LogInfo("game: socket-skills: place {}: {}", arg, shown ? "shown" : "no socket skill");
        for (int k = 0; k < 4; ++k) {
            if (const std::uint32_t id = WornSocketSkill(hudContext_, k, false); id != 0) {
                const assets::SkillRow* row = tables_ && tables_->Usable() ? tables_->skills.Find(id) : nullptr;
                core::LogInfo("game: socket-skills: {} {} ({})", k, id, row != nullptr ? row->codename : std::string("?"));
            }
        }
    } else if (verb == "socket-quick") {
        const auto [first, slot] = CodeAndNumber(arg);
        const std::uint32_t id = WornSocketSkill(hudContext_, std::atoi(first.c_str()), true);
        if (id == 0 || slot < 0 || slot >= game::QuickSlots::kCount) {
            core::LogWarning("game: socket-quick: {}: nothing to drag", arg);
        } else {
            player_.quickSlots.slots[std::size_t(slot)] = {game::QuickSlot::Kind::Skill, id};
            hud_.Request({HudRequest::Kind::QuickSlotChanged, slot});
            core::LogInfo("game: socket-quick: skill {} into quick slot {}", id, slot);
        }
    } else if (verb == "socket-cast") {
        const std::uint32_t id = WornSocketSkill(hudContext_, std::atoi(arg.c_str()), false);
        if (id == 0) {
            core::LogWarning("game: socket-cast: {}: no skill", arg);
        } else {
            core::LogInfo("game: socket-cast: skill {}", id);
            shared_->session->UseSkill(id, player_.target.Selected() && player_.target.entity != view_.Self() ? player_.target.entity : 0);
        }
    } else {
        return false;
    }
    return true;
}

} // namespace sr::client
