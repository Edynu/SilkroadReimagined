#pragma once

// The title screens' interface kit (C5e): the original's layouts (resinfo\*.txt), its interface
// text table and font, read on a job; interface textures from the asset store; and a painter
// that draws the original's controls on the 1024 x 768 design grid into a draw list.
//
// Ported from the prototype's UiPainter (checked: it is plain drawing; the numbers it carries
// are marked where they are not from our RE).

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/assets/UiLayout.h"
#include "gui/UiData.h"
#include "sr/core/DesignGrid.h"
#include "sr/ui/Context.h"
#include "sr/ui/Font.h"

namespace sr::client {

class UiKit {
public:
    UiKit(ui::Context& context, UiTextures& textures, const UiData& data) : ui_(context), textures_(textures), data_(data) {}

    core::DesignGrid& grid() { return grid_; }
    ui::Context& context() { return ui_; }
    const UiData& data() const { return data_; }
    ui::DrawList& List() { return ui_.draw(); }

    ui::Rect Screen(core::DesignRect r) const;
    void Image(const std::string& path, core::DesignRect r, float alpha = 1.0f);
    void ImageScreen(const std::string& path, ui::Rect r, float alpha = 1.0f);
    ui::Vec2 TextureSize(const std::string& path);
    void Fill(core::DesignRect r, ui::Color color);
    void FillScreen(ui::Rect r, ui::Color color) { List().Fill(r, color); }

    // The original's interface font sizes in pixels at scale 1 (the prototype's: Arial 9 / 8 /
    // 12 / 11 / 15 point at 96 dpi; not yet from our RE).
    static float FontSize(int fontIndex);
    float TextWidth(std::string_view text, int fontIndex = 0);
    void Text(std::string_view text, core::DesignRect r, ui::Color color, int fontIndex = 0, int hAlign = 0, int vAlign = 1);
    // Text with the interface's markup ("<font color=\"A,R,G,B\">..</font>"; other tags dropped;
    // "\n" written out breaks the line), broken into lines inside `r` from its top, each aligned
    // by `hAlign` (0 left, 1 centre).
    void RichText(std::string_view markup, core::DesignRect r, ui::Color color, int fontIndex = 0, int hAlign = 0);
    // A CIFTextBox: text broken into lines inside `r`, from its top-left.
    void TextBox(std::string_view text, core::DesignRect r, ui::Color color, int fontIndex = 0);
    // A layout node's text with its font, colour and alignment.
    void NodeText(const assets::UiLayoutNode& node, std::string_view text, core::DesignRect r, float alpha = 1.0f);

    // A control's text rect: inside its ClientRect insets (left, top, right, bottom).
    static core::DesignRect Inset(const assets::UiLayoutNode& node, core::DesignRect r);
    // A control rect relative to its window's origin (design units).
    static core::DesignRect Local(const assets::UiLayoutNode& node, core::DesignRect window) {
        return {window.x + node.rect.x, window.y + node.rect.y, node.rect.w, node.rect.h};
    }

    ui::Context::ItemState Item(std::string_view id, core::DesignRect r, bool enabled = true);
    // A CIFButton: the texture, then its _focus / _press / _disable faces; true when clicked.
    bool Button(std::string_view id, const assets::UiLayoutNode& node, core::DesignRect r, bool enabled = true);
    // The same, with its state (held, hovered) for buttons that act while held.
    ui::Context::ItemState ButtonState(std::string_view id, const assets::UiLayoutNode& node, core::DesignRect r, bool enabled = true);
    // A plain button of our own (screens not yet laid out from the original's art); true when clicked.
    bool TextButton(std::string_view id, std::string_view label, core::DesignRect r, bool enabled = true, bool selected = false);
    // A CIFEdit: typed text into `value` (at most `capacity` bytes); true when Enter was pressed.
    bool Edit(std::string_view id, const assets::UiLayoutNode& node, core::DesignRect r, std::string& value, std::size_t capacity,
              bool password, bool takeFocus);

private:
    ui::Context& ui_;
    UiTextures& textures_;
    const UiData& data_;
    core::DesignGrid grid_;
};

} // namespace sr::client
