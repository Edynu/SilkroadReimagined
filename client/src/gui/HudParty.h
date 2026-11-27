#pragma once

// The party's windows, views over game::PartyView: the main window's Party
// page (CIFParty: the master's row, the seven members' slots, the settings, Invite / Set / Party
// match), the quick party window down the left edge (CIFQuickPartyWnd: the other members' bars), the
// settings box (CIFSetPartyMode), the target menu of a player (the 1.188 client's CNIFTargetMenu,
// built in code from res_ui\targetmenu.2dt's places), and party matching: its window
// (CIFPartyMatch: the listings, pages, search), the listing's box (CIFPartyMatchRegister; ours: the
// exp and item settings chosen in it), the leader's join request (CIFPartyMatchReqJoin) and the
// applicant's waiting box (CIFPartyJoinProgress). What they ask goes to the HUD manager as requests.

#include <cstdint>
#include <functional>
#include <string>

#include "gui/HudWindows.h"

namespace sr::client {

void RegisterPartyWindows(ui::Interface& ui, HudContext& context);

// The settings box (CIFSetPartyMode), centred: exp shared or each its own, items the same, members
// may invite; `confirmed` hears the settings chosen (Yes).
void OpenSetPartyMode(ui::Interface& ui, std::uint8_t settings, std::function<void(std::uint8_t settings)> confirmed);
// A player's menu at a point (interface pixels): Whisper, Exchange, Add friend, Invite party, Trace.
void OpenTargetMenu(ui::Interface& ui, HudContext& context, std::uint32_t entity, const std::string& name, ui::Vec2 at);
void CloseTargetMenu(ui::Interface& ui);
// Asks to party with a player: in no party the settings box first (the next party's settings), in
// one at once.
void InviteToParty(ui::Interface& ui, HudContext& context, std::uint32_t entity);
// The matching windows: the listing's box (a new listing, or the player's own changed), the leader's
// join request box, the applicant's waiting box; each shown once, closed when its matter is over.
void OpenPartyMatchRegister(ui::Interface& ui, HudContext& context, bool modify);
void SyncPartyMatchBoxes(ui::Interface& ui, HudContext& context);
// The academy's uses of the party's boxes: the matching list's register box in its academy
// state, and the join request's progress box while the player waits for a guardian.
void OpenAcademyMatchRegister(ui::Interface& ui, HudContext& context, bool modify);
void SyncAcademyProgress(ui::Interface& ui, HudContext& context);

// The settings as the windows write them ("Exp Auto Share" / "Exp Free-For-All" and the items').
std::string PartyExpText(const HudContext& context, std::uint8_t settings);
std::string PartyItemText(const HudContext& context, std::uint8_t settings);
// A party invitation's box, the original's message box kind 6: who asks, the question, the two settings in black boxes,
// "anyone may invite" when set, Accept / Refuse.
ConfirmBox* OpenPartyInviteBox(HudContext& context, const std::string& from, std::uint8_t settings, std::function<void(bool yes)> answer);
// An academy join request to its guardian, the original's message box kind 0x0E: the request, the applicant's level
// (and highest) and race in black boxes.
ConfirmBox* OpenAcademyJoinBox(HudContext& context, const std::string& from, int level, int maxLevel, std::uint8_t race,
                               std::function<void(bool yes)> answer);
// A purpose's name (Hunting, Quest, Trade Union, Thief Union).
std::string PartyPurposeText(const HudContext& context, std::uint8_t purpose);

} // namespace sr::client
