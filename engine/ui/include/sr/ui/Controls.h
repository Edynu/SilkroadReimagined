#pragma once

// The controls of the original's layouts, each named for what it is; the
// original's class it stands for is in its comment. Built by the Interface from layout nodes
// (RegisterControls); windows reach them by id and cast (`window->Find(5)->As<Gauge>()`).

#include <functional>
#include <string>
#include <vector>

#include "sr/ui/Widget.h"

namespace sr::ui {

class Interface;

// Every control class of the layouts, registered with the interface.
void RegisterControls(Interface& interface);

// The original's text styles (bits): a 1-pixel black drop shadow, bold (weight 600), italic.
enum TextStyle : std::uint8_t { kTextShadow = 1, kTextBold = 2, kTextItalic = 0x40 };

// The original's text lines: the font's point size + 5 apart (14 pixels for font 0).
float TextLineHeight(int fontIndex);
// Text in a rect as the original lays it out: lines at TextLineHeight, the block as wide as its
// widest line placed by the alignment, never clipped or cut; `style` of TextStyle bits.
void DrawLabel(Painter& paint, std::string_view text, Rect r, Color color, int fontIndex, int hAlign, int vAlign, int style = 0);

template <class T>
T* As(Widget* w) {
    return dynamic_cast<T*>(w);
}

// --- Images and text -------------------------------------------------------------------------

// CIFStatic: its texture (the UV_LT..UV_RB part) over its rect, its text in the client rect.
class Static : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Draw(Frame& frame) override;
    Color textColor = kWhite;
    int textStyle = 0;
    Color tint = kWhite; // its picture's colour (a transport's stars by their count)
    // Another texture in place of the node's (a window's code sets icons, faces).
    std::string texture;
    bool noArt = false; // neither picture (a window's code takes its art away: the quest list's goal rows)
    // Another part of the texture than the node's UV_LT..UV_RB (sprite frames, other looks).
    bool customUv = false;
    Vec2 uv0{0, 0}, uv1{1, 1};
};

// CIFDecoratedStatic: a static with an overlay its owner sets (normal, additive or pulsing).
class DecoratedStatic : public Static {
public:
    using Static::Static;
};

// CIFStaticWithPictureClip: a picture whose alpha comes from a mask its owner sets (faces). With a
// mask it draws the picture over its rect with the mask's alpha only (the picture's and the
// window's are ignored), nothing until the mask is in; without one, as a
// static.
class PictureClip : public Static {
public:
    using Static::Static;
    void Draw(Frame& frame) override;
    std::string mask;
};

// --- Buttons ---------------------------------------------------------------------------------

// CIFButton: its texture and the _focus / _press / _disable faces; a click on release over it.
// Style 0x20 repeats while held (a click on press, then every 50 ms after 1 s); style 1 tells its
// parent of every mouse move while held (a scroll thumb).
class Button : public Static {
public:
    using Static::Static;
    // Behaviour bits its owner sets (the original's button styles).
    enum : int { kDragNotify = 1, kNoFaces = 0x10, kRepeat = 0x20 };
    int behaviour = 0;
    void Created(Interface& interface) override;
    void Update(Frame& frame) override;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    bool down() const { return down_; }

private:
    std::string Face(Frame& frame) const;
    bool down_ = false;     // held with the mouse over it
    float repeat_ = -1.0f;  // seconds to the next repeated click
};

// CIFCloseButton: com_windowclose; hides the window it belongs to.
class CloseButton : public Button {
public:
    using Button::Button;
    void Created(Interface& interface) override;
    void Mouse(Frame& frame) override;
};

// CIFSelectableArea: a tab (the chat's, the block list's): its art and caption; a left press (not
// the release) tells its window; the selected one's caption is bold.
class SelectableArea : public Static {
public:
    using Static::Static;
    void Created(Interface& interface) override;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    bool selected = false;
};

// CIFCheckBox: on / off art (its own layout's static); a click flips it.
class CheckBox : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    bool checked = true; // boxes start checked
};

// CIFRadioButton: a group of choices, one checked.
class RadioButton : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    bool OnNotice(const Notice& notice) override;
    // The window's code makes the units: 16 x 16 buttons (ids 100 + i, at most 10) from `origin`,
    // `spacing` apart (down, or across with `row`), each with its label (id 200 + i).
    void SetUnits(Interface& interface, const std::vector<std::string>& labels, bool row = false);
    Vec2 origin{21, 43};
    float spacing = 30.0f, labelOffset = 28.0f, labelWidth = 92.0f;
    int selected = 0;
};

// CIFDragableArea: dragging it moves its window.
class DragArea : public Widget {
public:
    using Widget::Widget;
    // Behind its siblings (they get clicks first); pressing it raises the window.
    void Created(Interface& interface) override;
    void Mouse(Frame& frame) override;
    bool fixed = false; // the window stays where it is (the extra quick bar's "Fixing position")

private:
    Vec2 grab_;
};

// --- Borders and backgrounds -------------------------------------------------------------------

// CIFFrame: four corners at their size, the edges tiled between them (the last tile cropped).
class FrameBorder : public Widget {
public:
    using Widget::Widget;
    void Draw(Frame& frame) override;
    // The parts' prefix (the node's DDJ: "interface\\frame\\mframe_wnd_").
    std::string prefix() const { return node().ddj; }
};

// CIFSubFrame: a frame with a caption at the top.
class SubFrame : public FrameBorder {
public:
    using FrameBorder::FrameBorder;
    void Created(Interface& interface) override;
};

// CIFMainFrame: a window's chrome: a frame, the title, the strip to drag it by, the close button.
class MainFrame : public FrameBorder {
public:
    using FrameBorder::FrameBorder;
    void Created(Interface& interface) override;
    // Its close button (id 2) hides the window and tells the interface (snd_window_close).
    bool OnNotice(const Notice& notice) override;

private:
    Interface* interface_ = nullptr;
};

// CIFNormalTile: its texture tiled at its size from the top-left, the last ones cropped.
class TileFill : public Widget {
public:
    using Widget::Widget;
    void Draw(Frame& frame) override;
};

// CIFStretchWnd: four corners at their size, the sides stretched (top and bottom are the side
// textures turned a quarter turn).
class StretchBox : public Widget {
public:
    using Widget::Widget;
    void Draw(Frame& frame) override;
    std::string prefix;
};

// CIFBarWnd: left and right caps, the middle tiled, the caption centred.
class Bar : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Draw(Frame& frame) override;
    std::string pieces; // another set of pieces than the layout's (a list row's selected look); empty: the layout's
};

// CIFLattice: a grid of 36-pixel cells (inner, last column, last row, last cell).
class Lattice : public Widget {
public:
    using Widget::Widget;
    void Draw(Frame& frame) override;
};

// CIFFade: a plain colour over its client rect (black unless its owner sets one).
class Fade : public Widget {
public:
    using Widget::Widget;
    void Draw(Frame& frame) override;
    Color color = Argb(0xFF000000);
};

// --- Values ---------------------------------------------------------------------------------------

// CIFGauge: fills left to right; the shown value eases to the target (10% of the gap per update).
class Gauge : public Static {
public:
    using Static::Static;
    void Update(Frame& frame) override;
    void Draw(Frame& frame) override;
    void SetValue(float value, bool instant = false);
    float value() const { return target_; }
    float speed = 0.01f;
    float backgroundValue = -1.0f; // drawn faintly when >= 0
    // A two-coloured gauge (2dt kind 17, the temples' reputation: green against orange): this texture fills the rest,
    // right of the value (none: nothing).
    std::string rest;

private:
    float shown_ = 0.0f, target_ = 0.0f;
};

// CIFVerticalScroll: an up arrow above, a fixed thumb on a stretched track, a down arrow below;
// tells its parent the new position.
class ScrollBar : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Update(Frame& frame) override;
    void Draw(Frame& frame) override;
    bool OnNotice(const Notice& notice) override;
    void SetRange(int min, int max, int step = 1);
    void SetPosition(int position);
    int position() const { return position_; }
    float track = 0.0f; // the thumb's travel in pixels

private:
    void PlaceChildren();
    int min_ = 0, max_ = 100, step_ = 1, position_ = 0;
};

// CIFScrollManager: a frame that lays out its items in rows and columns, with a scroll bar.
class ScrollManager : public FrameBorder {
public:
    using FrameBorder::FrameBorder;
    // Its bar (id 10, from its own layout) at the right edge: travel = height - 48.
    void Created(Interface& interface) override;
};

// CIFSliderCtrl (ifsliderctrl.txt): its art (the node's DDJ) over its rect, the back (id 1) and
// forward (id 2) buttons, repeating, a step each, the thumb (id 3) on the track between them; a press
// or a drag on the track puts the thumb under the mouse. Tells its value (ValueChanged). Without an
// owner's places the forward button mirrors the back one at the right end; the owner may set the
// buttons' textures and places (the auto potion's).
class Slider : public Static {
public:
    using Static::Static;
    void Created(Interface& interface) override;
    void Update(Frame& frame) override;
    void Mouse(Frame& frame) override;
    bool OnNotice(const Notice& notice) override;
    void SetValue(int v);
    int min = 0, max = 100, step = 1, value = 0;
    bool placeForward = true; // false: the owner placed the buttons

private:
    bool dragging_ = false;
};

// CIFVerticalSpinCtrl: a number in a black box (com_blacksquare_) with the up and down arrows of the
// underbar (ub_up_arrow, ub_down_arrow; 20 x 12) at its right, made in code. Its
// value is shown divided by `divider` with `decimals` places (the auto potion's delay: ms as
// seconds). As the original's numeric box: up takes a step off, down adds one. Tells its value (ValueChanged).
class VerticalSpin : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Update(Frame& frame) override;
    bool OnNotice(const Notice& notice) override;
    void SetValue(int v);
    int min = 0, max = 100, step = 1, value = 0;
    float divider = 1.0f;
    int decimals = 0;
};

// CIFHScroll_Option: the Option window's volume bar: a thumb along the bar, pressed or dragged
// anywhere on it to a value min..max (ValueChanged while it moves).
class HSlider : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Update(Frame& frame) override;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    // Its arrows (ids 0 and 1, from its own layout) move it a step.
    bool OnNotice(const Notice& notice) override;
    void SetValue(int v);
    int min = 0, max = 100, value = 0;

private:
    bool dragging_ = false;
};

// CIFSpinButtonCtrl: left / right arrows around a page number (the value + 1).
class Spin : public Widget {
public:
    using Widget::Widget;
    void Draw(Frame& frame) override;
    bool OnNotice(const Notice& notice) override;
    void Update(Frame& frame) override;
    int value = 0, max = 0;
};

// One paragraph of a tooltip: its text (wrapped at 200 pixels), colour and TextStyle bits; an
// empty text is a blank line.
struct TooltipLine {
    std::string text;
    Color color = kWhite;
    int style = kTextShadow;
};

// --- Item slots and the tooltip -----------------------------------------------------------------

// CIFSlotWithHelp: an icon (no art when empty: the window's lattice is behind it), under the red
// icon_disable when not usable, the stack count in item_number digits when above 1. Its hit area is 2 pixels larger on
// every side. A left click picks it up (the window decides), a right click uses it; the tooltip
// shows while the mouse is over it.
class Slot : public Widget {
public:
    using Widget::Widget;
    void Created(Interface& interface) override;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    bool Hit(Vec2 p) const override;
    std::string icon;      // empty: an empty slot
    bool pickable = true;  // a left click picks the item up (the window sets it)
    int count = 0;         // shown when above 1 (any count with countAlways: stackable items)
    bool countAlways = false;
    bool usable = true;    // icon_disable's red veil when not
    bool ghost = false;    // a white veil (the two-handed weapon's copy in the shield place)
    Color veil = 0;        // a coloured veil over the item, 0xAARRGGBB as the original's slot keeps it:
                           // a dead or expired pet's item, an expired timed item 0x80004B7E
    std::string overlay;   // a picture over the item (the exchange's blue com_item_exchange.ddj once a side confirmed)
    bool rare = false;     // the rare items' sparkle running round the edge (icon_edge_rare)
    bool petOut = false;   // a summoned pet's item: interface\pet\pt_edge_effect running round it
    double lifeFrom = -1;  // a pet's item back from dead: interface\pet\pt_life_effect played once from this time (-1 none)
    bool spiritAwake = false; // an awakened Devil's Spirit: icon\item\etc\icon_edge_nasrun running round it
    // A piece near breaking (durability 1..6): icon_item_warning's red pulse over it, 16 frames of 16 pixels (an 8-column
    // sheet), 80 ms each. Broken (durability 0, nothing else keeping it off): icon_item_broken's red frame in place of
    // icon_disable's veil.
    bool worn = false, broken = false;
    std::string tooltip;   // the help text
    std::function<std::vector<TooltipLine>()> tooltipLines; // an item's (or a skill's) tooltip, built when shown
    std::string tooltipSign; // its sign before the name (com_itemsign... ; empty: the plain one)
    bool tooltipSigned = true; // an item's sign at all (a skill's tooltip has none)
    // A wait running: the part left (0 none) as the dark
    // clock of skill_delay (16 x 16 frames), then the half-second "ready" flash of skill_charge (4 x 4
    // frames, `charge` the part of it left); `seconds` >= 0 also writes the time left on the icon
    // (underbar slots: white seconds, yellow minutes, blue hours).
    float cooling = 0.0f, charge = 0.0f;
    double seconds = -1.0;
    // A one-shot effect over the item, drawn past its edges (1.188's slot timers 1 and 2, a frame each 50 ms): Repair,
    // icon_mall_repair's 20 sparks (8 x 4 frames of 32) on 72 x 72 about the
    // slot, when its durability rises; Switch, icon_mall_transgender's 16 swirls (4 x 4) on 48 x 48, when the item
    // becomes another (the gender switch). Started at the next draw.
    enum class Flash : std::uint8_t { Repair, Switch };
    void Play(Flash kind) { flashPending_[std::size_t(kind)] = true; }

private:
    bool flashPending_[2] = {};
    double flashStart_[2] = {-1.0, -1.0};
};

// CIFHelperBubbleWindow: the tooltip beside a slot (8-pixel tooltip frame, dark navy fill); rows 14
// high for one, 18 for more. `sign`: the item sign's texture (com_itemsign...) before the first
// line, an item's name; empty: none.
void DrawTooltip(Painter& paint, std::string_view text, Rect beside, Vec2 screen);
// Returns the box it drew (its text's rect, the 8-pixel frame outside it; empty when nothing was drawn).
Rect DrawTooltip(Painter& paint, const std::vector<TooltipLine>& lines, Rect beside, Vec2 screen, const std::string& sign = {});

// --- Text input and lists ---------------------------------------------------------------------

// CIFEdit: one line of typed text (512 characters unless set), a 2-pixel caret blinking every
// 200 ms; when wider than the field its end shows. A click gives it the keyboard. An IME's
// composition shows at the caret, underlined, until it is typed.
class Edit : public Static {
public:
    using Static::Static;
    void Created(Interface& interface) override;
    void Update(Frame& frame) override;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    void Keyboard(Frame& frame) override;
    bool focused() const { return focused_; }
    void Focus(Frame& frame);
    // Text its window puts in (the chat's prefixes, a whisper's address): the caret at its end.
    void SetValue(std::string value) {
        SetText(std::move(value));
        caret_ = text().size();
    }
    // The caret's x on the screen (interface pixels), as last drawn.
    float caretX() const { return caretX_; }
    std::size_t limit = 512; // characters
    bool password = false;   // the layout's style 2: one '*' a character
    bool numeric = false;
    // A numeric box left empty when its digits are all deleted (else it reads 0): a count typed afresh.
    bool emptyAllowed = false;
    // A numeric box's highest number (-1: none): a larger one typed becomes it, and its leading zeros go (the original's
    // numeric boxes reread what was typed as a number after each key).
    std::int64_t maxValue = -1;
    bool passKeys = false;   // Enter and Escape go to the window (Submitted, Cancelled: the chat input)
    bool multiline = false;  // rows wrapped at its width, Enter a new line, the wheel scrolls it (a guild notice)
    bool readOnly = false;   // shown, never typed in

private:
    struct TextRow {
        std::size_t begin = 0, end = 0; // byte offsets of the row's text
    };
    void WrapRows(Painter& paint);
    int CaretRow() const;
    void DrawRows(Frame& frame, const std::string& saved);
    std::size_t caret_ = 0; // byte offset
    bool focused_ = false;
    float caretX_ = 0.0f;
    std::vector<TextRow> rows_; // a multi-line edit's, for the text and width they were made from
    std::string wrapped_;
    float wrappedWidth_ = -1.0f;
    int firstRow_ = 0;
};

// CIFListCtrl: rows of text at a line height (23 unless set), the selected row on a blue bar,
// the hovered one on its own when the owner turns that on. A click picks a row.
class ListView : public Widget {
public:
    using Widget::Widget;
    void Draw(Frame& frame) override;
    void Mouse(Frame& frame) override;
    void Created(Interface&) override { interactive = true; }
    struct Row {
        std::string text;
        Color color = 0; // 0: the list's
        int value = 0;
    };
    // The row under a point (interface pixels), -1 for none.
    int RowAt(Vec2 p) const;
    std::vector<Row> rows;
    int first = 0, selected = -1, hovered = -1;
    int visibleRows = 0; // 0: as many as there are
    float lineHeight = 23.0f;
    Color textColor = kWhite, hoverColor = 0;
    Color selectedBar = Argb(0xFF0686C2), hoverBar = 0;
    bool showSelected = true, showHover = false, hoverTracking = false;
};

// CIFTextBox: its text wrapped at the client width, line by line (the list's 23 pixels unless
// set), with a drop shadow; not clipped. Or, as the chat and the system messages use it, a list of
// paragraphs added one by one, each with its colour: kept up to a number (the oldest go first),
// wrapped into rows, `visibleRows` of them shown from the scroll position; bottom-anchored, the
// newest row at the bottom and a new paragraph scrolling to the end; a linked scroll bar follows.
class TextBox : public Static {
public:
    using Static::Static;
    void Update(Frame& frame) override;
    void Draw(Frame& frame) override;
    float lineHeight = 23.0f;

    struct Paragraph {
        std::string text;
        Color color = kWhite;
        int style = kTextShadow;
        Color hover = 0; // its colour under the mouse (the NPC window's choices); 0: its own
    };
    void AddParagraph(std::string text, Color color, int style = kTextShadow, Color hover = 0);
    void ClearParagraphs();
    const std::vector<Paragraph>& paragraphs() const { return paragraphs_; }
    std::size_t maxParagraphs = 0; // 0: no limit
    int visibleRows = 0;           // 0: as many as fit the height
    bool bottomAnchored = false;
    bool hangingIndent = false;    // wrapped rows of a paragraph start 20 pixels in
    ScrollBar* bar = nullptr;      // follows the rows; its position is the first row shown
    // One row up (-1) or down (+1), as the wheel does.
    void ScrollBy(int rows);
    void ScrollToEnd() { follow_ = true; }
    // The chat's lines: the row under the mouse on a bar (`hoverBar`, as wide as its text unless
    // `hoverFullRow`), white while the button is held on it; a release over a row tells the window
    // its paragraph (RowPicked, the paragraph's index). Off unless the window turns it on.
    bool hoverTracking = false, hoverFullRow = false;
    Color hoverBar = 0;
    void Mouse(Frame& frame) override;
    // The paragraph a point is on (interface pixels), -1 for none.
    int ParagraphAt(Vec2 p) const;

private:
    struct Row {
        std::string text;
        Color color;
        int style;
        float indent;
        int paragraph;
    };
    int RowAt(Vec2 p) const;
    int hoveredRow_ = -1, pressedRow_ = -1;
    void Layout(Painter& paint);
    std::vector<Paragraph> paragraphs_;
    std::vector<Row> rows_;
    bool dirty_ = true, follow_ = true;
    int first_ = 0, barLast_ = -1;
    float width_ = -1.0f;
};

// CIFPML: rich text in the original's SML2 markup (<sml2>, <br>, <p>, <center>, <font color>, ...).
class RichText : public Static {
public:
    using Static::Static;
    void Draw(Frame& frame) override;
    // The height its content took when last drawn (a page's scroll).
    float contentHeight = 0.0f;
};

// CIFComboBox: a black field with the chosen text and a down arrow that opens the list below it.
// The arrow or the field opens the list and closes it again; the list is the interface's pop-up
// (in front of every window, with a scroll bar when it has more rows than it shows).
class ComboBox : public Static {
public:
    using Static::Static;
    ~ComboBox() override;
    void Created(Interface& interface) override;
    void Update(Frame& frame) override;
    void Mouse(Frame& frame) override;
    bool OnNotice(const Notice& notice) override;
    // Chooses an item (-1 for none); with `notify` the window hears of it.
    void Select(int index, bool notify = false);
    bool open() const;
    void Open();
    void Close();
    std::vector<std::string> items;
    int selected = -1;
    int rowsShown = 5;

private:
    Interface* interface_ = nullptr;
};

} // namespace sr::ui
