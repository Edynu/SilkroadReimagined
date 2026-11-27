// Item and skill slots, the tooltip, and the registration of every control class.

#include <algorithm>
#include <cmath>
#include <string>

#include "sr/ui/ClassLayouts.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::ui {

namespace {

std::vector<std::string> WrapLines(Painter& paint, std::string_view text, float width, int fontIndex) {
    std::vector<std::string> out;
    std::string line, word;
    const auto flushWord = [&] {
        if (word.empty()) return;
        const std::string candidate = line + word;
        if (!line.empty() && paint.TextWidth(candidate, fontIndex) > width) {
            out.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') {
            flushWord();
            out.push_back(line);
            line.clear();
            continue;
        }
        word += c;
        if (c == ' ') flushWord();
    }
    flushWord();
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}

} // namespace

// --- Slot ----------------------------------------------------------------------------------------

void Slot::Created(Interface&) { interactive = true; }

bool Slot::Hit(Vec2 p) const {
    // The hit area reaches 2 pixels past the 32 x 32 icon on every side, so slots 36 apart touch.
    const Rect r = ScreenRect();
    return p.x >= r.x - 2 && p.y >= r.y - 2 && p.x <= r.Right() + 2 && p.y <= r.Bottom() + 2;
}

void Slot::Draw(Frame& frame) {
    // No art when empty: the window's lattice is the empty slot; only the white veil when it has one (the equipment's
    // attachment slot under a dress that takes none).
    const Rect r = ScreenRect();
    const float opacity = Opacity();
    if (icon.empty()) {
        if (ghost) frame.paint.Fill(r, WithAlpha(kWhite, 0.45f * opacity));
        return;
    }
    const std::string& path = frame.paint.HasTexture(icon) ? icon : std::string("icon\\icon_default.ddj");
    frame.paint.Image(path, r, {0, 0}, {1, 1}, WithAlpha(kWhite, opacity));
    if (!usable) // the red veil, or a broken piece's red frame
        frame.paint.Image(broken ? "icon\\icon_item_broken.ddj" : "icon\\icon_disable.ddj", r, {0, 0}, {1, 1}, WithAlpha(kWhite, opacity));
    if (worn && usable) {
        // Its pulse: the 17 steps of 80 ms read as frames 0..15 (the step x 16 / 17).
        const int f = int(frame.time / 0.08) % 17 * 16 / 17;
        const Vec2 uv0{float(f % 8) / 8.0f, float(f / 8) / 4.0f};
        frame.paint.Image("icon\\icon_item_warning.ddj", r, uv0, {uv0.x + 0.125f, uv0.y + 0.25f}, WithAlpha(kWhite, opacity));
    }
    if (ghost) frame.paint.Fill(r, WithAlpha(kWhite, 0.45f * opacity));
    if ((veil >> 24) != 0) frame.paint.Fill(r, WithAlpha(veil | 0xFF000000u, float(veil >> 24) / 255.0f * opacity));
    if (!overlay.empty()) frame.paint.Image(overlay, r, {0, 0}, {1, 1}, WithAlpha(kWhite, opacity));
    if (rare) {
        // The rare sparkle: an 8 x 4 sheet of 32-pixel frames, 40 ms each, looping from a start of its own.
        const int start = int((reinterpret_cast<std::uintptr_t>(this) >> 4) % 32);
        const int f = (int(frame.time * 25.0) + start) % 32;
        const Vec2 uv0{float(f % 8) / 8.0f, float(f / 8) / 4.0f};
        frame.paint.Image("icon\\item\\etc\\icon_edge_rare.ddj", r, uv0, {uv0.x + 0.125f, uv0.y + 0.25f}, WithAlpha(kWhite, opacity));
    }
    if (spiritAwake) {
        // An awakened Devil's Spirit (1.188's slot, its time left above 0): a strip of sixteen 32-pixel frames (512
        // wide) on the slot's frame counter (ours: 40 ms a frame, as the rare sparkle's).
        const int f = int(frame.time * 25.0) % 16;
        frame.paint.Image("icon\\item\\etc\\icon_edge_nasrun.ddj", r, {float(f) / 16.0f, 0.0f}, {float(f + 1) / 16.0f, 1.0f}, WithAlpha(kWhite, opacity));
    }
    if (petOut) {
        // Its pet out: a strip of nine 32-pixel frames (288 wide), 50 ms each, looping (1.188: the slot's timer 4,
        // drawn from its frame counter 9..1).
        const int f = int(frame.time * 20.0) % 9;
        frame.paint.Image("interface\\pet\\pt_edge_effect.ddj", r, {float(f) / 9.0f, 0.0f}, {float(f + 1) / 9.0f, 1.0f}, WithAlpha(kWhite, opacity));
    }
    // A wait: the clock wiping clear as it ends, then the ready flash; the time left on top.
    if (cooling > 0.0f) {
        const int f = std::clamp(int(239.0f - cooling * 239.0f), 0, 239);
        const Vec2 uv0{float(f % 16) / 16.0f, float(f / 16) / 16.0f};
        frame.paint.Image("interface\\skill\\skill_delay.ddj", r, uv0, {uv0.x + 1.0f / 16.0f, uv0.y + 1.0f / 16.0f}, WithAlpha(kWhite, opacity));
    } else if (charge > 0.0f) {
        const int f = std::clamp(int(15.0f - charge * 15.0f), 0, 15);
        const Vec2 uv0{float(f % 4) / 4.0f, float(f / 4) / 4.0f};
        frame.paint.Image("interface\\skill\\skill_charge.ddj", r, uv0, {uv0.x + 0.25f, uv0.y + 0.25f}, WithAlpha(kWhite, opacity));
    }
    if (cooling > 0.0f && seconds > 0.0) {
        const int s = int(std::ceil(seconds));
        const int shown = s <= 60 ? s : s <= 3600 ? (s + 59) / 60 : (s + 3599) / 3600;
        const Color colour = s <= 60 ? kWhite : s <= 3600 ? Argb(0xFFE1E700) : Argb(0xFF000CA9);
        const std::string digits = std::to_string(shown);
        float x = r.x + r.w / 2.0f - 4.0f * float(digits.size());
        for (const char d : digits) {
            frame.paint.Image("effect\\icon\\cool_time_" + std::string(1, d) + ".ddj", {x, r.y + r.h / 2.0f - 6.0f, 8, 12}, {0, 0}, {1, 1},
                              WithAlpha(colour, opacity));
            x += 8.0f;
        }
    }
    // The stack count: item_number digits, 8 x 8, 5 apart, from the top-left; the last digit at
    // x - 2 + 4n for n digits.
    if (count > 1 || (countAlways && count > 0)) {
        const std::string digits = std::to_string(count);
        const float n = float(digits.size());
        float x = r.x - 2.0f + 4.0f * n;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            frame.paint.Image("interface\\item_number\\item_number_" + std::string(1, *it) + ".ddj", {x, r.y + 2.0f, 8, 8}, {0, 0}, {1, 1},
                              WithAlpha(kWhite, opacity));
            x -= 5.0f;
        }
    }
    // The one-shot effects (the inventory's and the equipment's slots): frame 20 - n of the repair's
    // sparks (columns of 32 on a 256-wide sheet, rows on a 128-high one) from 20 pixels up and left of the slot, 72
    // across; frame 16 - n of the switch's swirl (4 x 4 on 128) from 8 pixels, 48 across.
    for (std::size_t k = 0; k < 2; ++k) {
        if (flashPending_[k]) {
            flashPending_[k] = false;
            flashStart_[k] = frame.time;
        }
        if (flashStart_[k] < 0.0) continue;
        const int frames = k == 0 ? 20 : 16, columns = k == 0 ? 8 : 4, rows = 4;
        const int f = int((frame.time - flashStart_[k]) / 0.05);
        if (f >= frames || f < 0) {
            flashStart_[k] = -1.0;
            continue;
        }
        const float grow = k == 0 ? 20.0f : 8.0f, size = k == 0 ? 72.0f : 48.0f;
        const Vec2 uv0{float(f % columns) / float(columns), float(f / columns) / float(rows)};
        frame.paint.Image(k == 0 ? "icon\\icon_mall_repair.ddj" : "icon\\icon_mall_transgender.ddj", {r.x - grow, r.y - grow, size, size}, uv0,
                          {uv0.x + 1.0f / float(columns), uv0.y + 1.0f / float(rows)}, WithAlpha(kWhite, opacity));
    }
}

void Slot::Mouse(Frame& frame) {
    const Input& in = frame.input;
    if (frame.pointer.hovered == this && !icon.empty() && tooltipLines) {
        frame.interface.ShowTooltip(tooltipLines(), ScreenRect(),
                                    !tooltipSigned ? std::string() : tooltipSign.empty() ? "interface\\ifcommon\\com_itemsign.ddj" : tooltipSign);
    } else if (frame.pointer.hovered == this && !tooltip.empty()) {
        frame.interface.ShowTooltip(tooltip, ScreenRect());
    }
    // A left release picks the item up (no press needed, no distance: the original's rule); the
    // window says what may move. A right click (press, then release on it) uses it.
    if (in.Released(MouseButton::Left) && frame.pointer.hovered == this && frame.interface.picked() == nullptr && !icon.empty() &&
        !in.ctrl() && !in.shift()) {
        Notify(Notice::Kind::Clicked);
        if (pickable) frame.interface.Pick(this);
    } else if (in.Released(MouseButton::Left) && frame.pointer.hovered == this && frame.interface.picked() == nullptr && !icon.empty() &&
               in.shift()) {
        Notify(Notice::Kind::Clicked, 1); // with Shift (the inventory's split)
    } else if (in.Released(MouseButton::Left) && frame.pointer.hovered == this && frame.interface.picked() == nullptr && !icon.empty() &&
               in.ctrl()) {
        Notify(Notice::Kind::Clicked, 2); // with Ctrl (the store's quick buy and sell)
    }
    if (in.Released(MouseButton::Right) && frame.pointer.held == this && Hit(frame.pointer.position)) Notify(Notice::Kind::RightClicked);
}

// --- Tooltip -------------------------------------------------------------------------------------

void DrawTooltip(Painter& paint, std::string_view text, Rect owner, Vec2 screen) {
    DrawTooltip(paint, std::vector<TooltipLine>{{std::string(text), kWhite, kTextShadow}}, owner, screen, {});
}

void DrawTooltip(Painter& paint, const std::vector<TooltipLine>& paragraphs, Rect owner, Vec2 screen, const std::string& sign) {
    // Each paragraph wrapped at 200 pixels; one row is 14 high, more rows 18 each. The item sign
    // takes the first row's start: the name begins after spaces as wide as it, and one more.
    struct Row {
        std::string text;
        Color color;
        int style;
    };
    std::vector<Row> lines;
    for (std::size_t k = 0; k < paragraphs.size(); ++k) {
        std::string text = paragraphs[k].text.empty() ? std::string(" ") : paragraphs[k].text;
        if (k == 0 && !sign.empty()) {
            std::string pad;
            // (At most 12 spaces: a font that is not in yet measures nothing.)
            while (paint.TextWidth(pad, 0) < 12.0f && pad.size() < 12) pad += ' ';
            text = pad + " " + text;
        }
        for (const std::string& row : WrapLines(paint, text, 200.0f, 0)) lines.push_back({row, paragraphs[k].color, paragraphs[k].style});
    }
    if (lines.empty()) return;
    float width = 0.0f;
    for (const Row& line : lines) width = std::max(width, paint.TextWidth(line.text, 0) + ((line.style & kTextBold) ? 1.0f : 0.0f));
    const float lineHeight = lines.size() > 1 ? 18.0f : 14.0f;
    const float height = lineHeight * float(lines.size());
    // Beside the owner: 8 to its right (to its left when it would leave the screen), 8 below its
    // top, kept 15 above the screen's bottom.
    float x = owner.Right() + 8.0f;
    if (width + owner.w + 20.0f + x > screen.x) x = owner.x - width - 8.0f;
    float y = std::min(owner.y + 8.0f, screen.y - height - 15.0f);
    x = std::trunc(x);
    y = std::trunc(y);
    // An 8-pixel frame around a dark navy fill, then the text. The fill as opaque as a screenshot
    // of the original shows it (about 83 %; measured by the user's capture, 2026-10-12): the window
    // alpha 0xC4 seen in the code does not thin the fill and the text as we first read it.
    const float alpha = 1.0f;
    const Color tint = WithAlpha(kWhite, alpha);
    const std::string corner = "interface\\ifcommon\\com_tooltip_corner.ddj", edge = "interface\\ifcommon\\com_tooltip_edge.ddj";
    paint.Fill({x, y, width, height}, WithAlpha(Argb(0xD41A2345), float(0xD4) / 255.0f));
    const Vec2 tl[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, tr[4] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}}, br[4] = {{1, 1}, {0, 1}, {0, 0}, {1, 0}},
               bl[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    paint.ImageCorners(corner, {x - 8, y - 8, 8, 8}, tl, tint);
    paint.ImageCorners(corner, {x + width, y - 8, 8, 8}, tr, tint);
    paint.ImageCorners(corner, {x + width, y + height, 8, 8}, br, tint);
    paint.ImageCorners(corner, {x - 8, y + height, 8, 8}, bl, tint);
    // The edge piece (8 across, 4 along) stretched along each side, its outer side outward.
    const Vec2 left[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, right[4] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};
    const Vec2 top[4] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}}, bottom[4] = {{1, 0}, {1, 1}, {0, 1}, {0, 0}};
    paint.ImageCorners(edge, {x - 8, y, 8, height}, left, tint);
    paint.ImageCorners(edge, {x + width, y, 8, height}, right, tint);
    paint.ImageCorners(edge, {x, y - 8, width, 8}, top, tint);
    paint.ImageCorners(edge, {x, y + height, width, 8}, bottom, tint);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        DrawLabel(paint, lines[i].text, {x, y + lineHeight * float(i), width, lineHeight}, WithAlpha(lines[i].color, alpha), 0, 0, 1,
                  lines[i].style);
    }
    if (!sign.empty()) paint.Image(sign, {x, y + std::floor((lineHeight - 12.0f) / 2.0f), 12, 12}, {0, 0}, {1, 1}, tint);
}

// --- Registration --------------------------------------------------------------------------------

namespace {

template <class T>
Interface::Factory Make() {
    return [](const assets::UiLayoutNode& node) -> std::unique_ptr<Widget> { return std::make_unique<T>(node); };
}

} // namespace

void RegisterControls(Interface& interface) {
    interface.Register("CIFStatic", Make<Static>());
    interface.Register("CIFDecoratedStatic", Make<DecoratedStatic>());
    interface.Register("CIFDraggedStatic", Make<Static>()); // the storage's invisible pick proxy
    interface.Register("CIFStaticWithPictureClip", Make<PictureClip>());
    interface.Register("CIFButton", Make<Button>());
    interface.Register("CIFCloseButton", Make<CloseButton>());
    interface.Register("CIFDropDownButton", Make<Button>());
    interface.Register("CIFCheckBox", Make<CheckBox>());
    interface.Register("CIFSelectableArea", Make<SelectableArea>());
    interface.Register("CIFRadioButton", Make<RadioButton>());
    interface.Register("CIFDragableArea", Make<DragArea>());
    interface.Register("CIFFrame", Make<FrameBorder>());
    interface.Register("CIFSubFrame", Make<SubFrame>());
    interface.Register("CIFMainFrame", Make<MainFrame>());
    interface.Register("CIFNormalTile", Make<TileFill>());
    interface.Register("CIFStretchWnd", Make<StretchBox>());
    interface.Register("CIFBarWnd", Make<Bar>());
    interface.Register("CIFLattice", Make<Lattice>());
    interface.Register("CIFFade", Make<Fade>());
    interface.Register("CIFGauge", Make<Gauge>());
    interface.Register("CIFVerticalScroll", Make<ScrollBar>());
    interface.Register("CIFScrollManager", Make<ScrollManager>());
    interface.Register("CIFSliderCtrl", Make<Slider>());
    interface.Register("CIFHScroll_Option", Make<HSlider>());
    interface.Register("CIFSpinButtonCtrl", Make<Spin>());
    interface.Register("CIFVerticalSpinCtrl", Make<VerticalSpin>());
    interface.Register("CIFEdit", Make<Edit>());
    interface.Register("CIFEInputChatBox", Make<Edit>());
    interface.Register("CIFListCtrl", Make<ListView>());
    interface.Register("CIFTextBox", Make<TextBox>());
    interface.Register("CIFPML", Make<RichText>());
    interface.Register("CIFComboBox", Make<ComboBox>());
    interface.Register("CIFSlotWithHelp", Make<Slot>());
    interface.Register("CIFSlotWithHelpForPackage", Make<Slot>());
    interface.Register("CIFExtQuickSlotSlot", Make<Slot>());
    // The window classes whose chrome is the main frame.
    for (std::string_view name : MainFrameClasses()) interface.Register(std::string(name), Make<MainFrame>());
}

} // namespace sr::ui
