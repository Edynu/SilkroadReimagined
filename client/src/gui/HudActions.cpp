// CIFAction (ifaction.txt): the main window's Actions page (the key A). Its icons come from the
// client's actionwnddata.txt: each row's group and place name a slot of the layout (group 1, the
// commands, the slots from 100; group 4, the emotes, from 400). Using an icon goes to the game screen,
// which knows what each one does; those not built yet say so.

#include <algorithm>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {

namespace {

class ActionPage : public ui::Widget {
public:
    ActionPage(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}

    void Update(ui::Frame&) override {
        if (filled_ || context_.actions == nullptr) return;
        filled_ = true;
        for (const assets::ActionRow& row : *context_.actions) {
            auto* slot = ui::As<ui::Slot>(Child(row.group * 100 + row.slot));
            if (slot == nullptr) continue;
            slot->icon = "icon\\" + row.icon.substr(row.icon.rfind("icon\\", 0) == 0 ? 5 : 0);
            slot->tooltip = context_.Text(row.textKey);
            slot->pickable = true; // onto a quick slot: its shortcut (kind 74)
            ids_[slot] = row.id;
        }
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind != ui::Notice::Kind::Clicked && notice.kind != ui::Notice::Kind::RightClicked) return false;
        const auto it = ids_.find(notice.source);
        if (it == ids_.end()) return false;
        if (context_.useAction) context_.useAction(it->second);
        return true;
    }

    std::uint32_t ActionOf(const ui::Widget* w) const {
        const auto it = ids_.find(w);
        return it != ids_.end() ? it->second : 0;
    }

private:
    HudContext& context_;
    std::unordered_map<const ui::Widget*, std::uint32_t> ids_;
    bool filled_ = false;
};

} // namespace

std::uint32_t DraggedAction(const ui::Widget* source) {
    for (const ui::Widget* w = source; w != nullptr; w = w->parent()) {
        if (const auto* page = dynamic_cast<const ActionPage*>(w)) return page->ActionOf(source);
    }
    return 0;
}

void RegisterActionWindow(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFAction", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<ActionPage>(n, *c); });
}

} // namespace sr::client
