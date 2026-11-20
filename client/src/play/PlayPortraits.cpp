// The HUD's portraits on the game screen: the player's head in its bar,
// dressed as it stands in the world, so a change of what it wears draws it again (as the original's
// re-dressing sets its redraw flag); and the other party members' heads in
// the quick party bars, their figures as created (the party's record names no worn items).

#include "gui/HudWindows.h"
#include "play/PlayScreen.h"

namespace sr::client {

void PlayScreen::UpdatePortraits(ScreenContext& context, FrameSnapshot& snapshot) {
    if (const game::WorldView::Entity* self = view_.Find(view_.Self())) {
        if (const WorldScreen::StageLook* look = LookOf(context, *self)) {
            portraits_.Show(kSelfPortrait, {look->model, look->stance, look->height, look->volume}, true);
        }
    }
    if (tables_ && tables_->Usable() && player_.party.InParty()) {
        for (const game::proto::PartyMemberRecord* m : player_.party.Others()) {
            if (m->model == 0) continue; // not told its figure
            portraits_.Show(m->id, portraits_.FigureLook(m->model, context.files, tables_->characters, tables_->items), false);
        }
    }
    // The Academy page's faces while it is up, the player's own its bar's.
    if (tables_ && tables_->Usable() && player_.academy.InAcademy() && hud_.Shown(hud::kMainPopup) && hud_.Page() == hud::kPageAcademy) {
        for (const game::proto::AcademyMember& m : player_.academy.Info().members) {
            if (m.model == 0 || m.name == player_.self.name) continue;
            portraits_.Show(AcademyPortraitKey(m.character), portraits_.FigureLook(m.model, context.files, tables_->characters, tables_->items), false);
        }
    }
    MallPreview(context); // the item mall's figure (PlayMall.cpp)
    portraits_.Update(context.assets, snapshot, gui_.textures());
    if (!portraits_.Settled()) context.holdCapture = true; // a capture shows them drawn
}

} // namespace sr::client
