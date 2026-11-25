// Typed text, lists, text boxes, rich text, the combo box and its pop-up list.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <string>
#include <vector>

#include "sr/ui/Controls.h"
#include "sr/ui/Font.h"
#include "sr/ui/Interface.h"

namespace sr::ui {

namespace {

float FontPoints(int fontIndex) {
    constexpr float kPoints[] = {9.0f, 8.0f, 12.0f, 11.0f, 15.0f};
    return kPoints[std::clamp(fontIndex, 0, 4)];
}

std::size_t PreviousCharacter(const std::string& s, std::size_t at) {
    if (at == 0) return 0;
    --at;
    while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
    return at;
}

std::size_t NextCharacter(const std::string& s, std::size_t at) {
    if (at >= s.size()) return s.size();
    ++at;
    while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) ++at;
    return at;
}

std::size_t CountCharacters(const std::string& s) {
    std::size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

Rect Inset(const Widget& w) {
    Rect r = w.ScreenRect();
    if (w.node().hasClientRect) {
        const auto& c = w.node().clientRect;
        r = {r.x + c.x, r.y + c.y, r.w - c.x - c.w, r.h - c.y - c.h};
    }
    return r;
}

// Words of `text` broken into lines no wider than `width` (at spaces; '\n' and the two
// characters "\n" break too).
std::vector<std::string> Wrap(Painter& paint, std::string_view text, float width, int fontIndex) {
    std::vector<std::string> out;
    std::string line, word;
    const auto flushWord = [&] {
        if (word.empty()) return;
        const std::string candidate = line.empty() ? word : line + word;
        if (!line.empty() && width > 0.0f && paint.TextWidth(candidate, fontIndex) > width) {
            out.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool escaped = c == '\\' && i + 1 < text.size() && text[i + 1] == 'n';
        if (c == '\n' || escaped) {
            flushWord();
            out.push_back(line);
            line.clear();
            if (escaped) ++i;
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

// --- Edit ----------------------------------------------------------------------------------------

void Edit::Created(Interface& interface) {
    Static::Created(interface);
    interactive = true;
    password = (node().style & 0x2) != 0;
    // The chat's input line: Enter, Tab and Escape go to its window; 100 bytes at most.
    if (type() == "CIFEInputChatBox") {
        passKeys = true;
        limit = 100;
    }
    caret_ = text().size();
}

void Edit::Focus(Frame& frame) {
    frame.interface.Focus(this);
    caret_ = text().size();
}

void Edit::Mouse(Frame& frame) {
    // A click gives the focus; the caret stays where it was.
    if (!readOnly && frame.input.Pressed(MouseButton::Left) && frame.pointer.hovered == this) Focus(frame);
    if (multiline && frame.input.wheel() != 0.0f && frame.pointer.hovered == this) firstRow_ += frame.input.wheel() > 0.0f ? -1 : 1;
}

void Edit::WrapRows(Painter& paint) {
    const float width = Inset(*this).w - 4.0f;
    if (wrappedWidth_ == width && wrapped_ == text()) return;
    wrapped_ = text();
    wrappedWidth_ = width;
    rows_.clear();
    const int font = node().fontIndex;
    const std::string_view all = wrapped_;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t newline = all.find('\n', begin);
        const std::size_t lineEnd = newline == std::string_view::npos ? all.size() : newline;
        // Each row as long as it fits, broken after the last space in it (at the width when it has none).
        std::size_t at = begin;
        for (;;) {
            std::size_t i = at, space = std::string_view::npos;
            while (i < lineEnd) {
                const std::size_t next = NextCharacter(wrapped_, i);
                if (i > at && paint.TextWidth(all.substr(at, next - at), font) > width) break;
                if (all[i] == ' ') space = i;
                i = next;
            }
            if (i >= lineEnd) {
                rows_.push_back({at, lineEnd});
                break;
            }
            const std::size_t cut = space != std::string_view::npos && space > at ? space + 1 : i;
            rows_.push_back({at, cut});
            at = cut;
        }
        if (newline == std::string_view::npos) break;
        begin = newline + 1;
    }
}

int Edit::CaretRow() const {
    // Where a row wraps, the caret at the join stands at the start of the next one.
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (caret_ <= rows_[i].end && (i + 1 == rows_.size() || caret_ < rows_[i + 1].begin)) return int(i);
    }
    return rows_.empty() ? 0 : int(rows_.size()) - 1;
}

void Edit::DrawRows(Frame& frame, const std::string& saved) {
    WrapRows(frame.paint);
    const Rect client = Inset(*this);
    const int font = node().fontIndex;
    const float lineHeight = TextLineHeight(font);
    const int shown = std::max(1, int(client.h / lineHeight));
    const int caretRow = CaretRow();
    if (focused_ && !readOnly) {
        if (caretRow < firstRow_) firstRow_ = caretRow;
        if (caretRow >= firstRow_ + shown) firstRow_ = caretRow - shown + 1;
    }
    firstRow_ = std::clamp(firstRow_, 0, std::max(0, int(rows_.size()) - shown));
    const Color color = WithAlpha(enabled ? textColor : Rgba(180, 180, 180), Opacity());
    const std::string_view all = saved;
    frame.paint.PushClip(ScreenRect());
    float y = std::trunc(client.y);
    for (int i = firstRow_; i < int(rows_.size()) && i < firstRow_ + shown; ++i, y += lineHeight) {
        const TextRow& row = rows_[std::size_t(i)];
        frame.paint.Text(all.substr(row.begin, row.end - row.begin), {client.x, y, client.w, lineHeight}, color, font, 0, 0);
        if (i == caretRow && focused_ && !readOnly && std::fmod(frame.time, 0.4) < 0.2) {
            const float caretX = client.x + frame.paint.TextWidth(all.substr(row.begin, std::min(caret_, row.end) - row.begin), font);
            frame.paint.Fill({caretX, y, 2.0f, FontPoints(font) + 2.0f}, color);
        }
    }
    frame.paint.PopClip();
}

void Edit::Update(Frame& frame) { focused_ = frame.interface.focus() == this; }

void Edit::Keyboard(Frame& frame) {
    if (readOnly) return;
    const Input& in = frame.input;
    std::string value = text();
    caret_ = std::min(caret_, value.size());
    bool changed = false;
    // While an IME composes, its keys are its own (Backspace, the arrows, Enter pick in it).
    const bool composing = !in.composition().empty();
    for (std::size_t i = 0; i < in.text().size();) {
        const std::size_t start = i;
        i = NextCharacter(in.text(), i);
        const std::string ch = in.text().substr(start, i - start);
        if (static_cast<unsigned char>(ch[0]) < 0x20) continue;
        if (numeric && (ch[0] < '0' || ch[0] > '9')) continue;
        if (CountCharacters(value) >= limit) break;
        value.insert(caret_, ch);
        caret_ += ch.size();
        changed = true;
    }
    if (composing) {
        if (changed) {
            SetText(value);
            Notify(Notice::Kind::TextChanged);
        }
        return;
    }
    // Ours (the original's edits take neither): Ctrl+C copies the whole text, Ctrl+X cuts it, Ctrl+V pastes the clipboard's
    // text at the caret (one line unless multiline; a password's never copied; within the limit and the numeric rule).
    if (in.ctrl()) {
        Context* context = frame.interface.context();
        if ((in.KeyPressed(Key::C) || in.KeyPressed(Key::X)) && !password && context != nullptr && context->setClipboard) {
            context->setClipboard(value);
            if (in.KeyPressed(Key::X) && !value.empty()) {
                value.clear();
                caret_ = 0;
                changed = true;
            }
        }
        if (in.KeyPressed(Key::V) && context != nullptr && context->getClipboard) {
            const std::string pasted = context->getClipboard();
            for (std::size_t i = 0; i < pasted.size();) {
                const std::size_t start = i;
                i = NextCharacter(pasted, i);
                std::string ch = pasted.substr(start, i - start);
                if (ch == "\r") continue;
                if (ch == "\n" || ch == "\t") {
                    if (!multiline || ch == "\t") ch = " ";
                } else if (static_cast<unsigned char>(ch[0]) < 0x20) {
                    continue;
                }
                if (numeric && (ch[0] < '0' || ch[0] > '9')) continue;
                if (CountCharacters(value) >= limit) break;
                value.insert(caret_, ch);
                caret_ += ch.size();
                changed = true;
            }
        }
    }
    if (in.KeyPressed(Key::Backspace, true) && caret_ > 0) {
        const std::size_t from = PreviousCharacter(value, caret_);
        value.erase(from, caret_ - from);
        caret_ = from;
        changed = true;
    }
    if (in.KeyPressed(Key::Delete, true) && caret_ < value.size()) {
        value.erase(caret_, NextCharacter(value, caret_) - caret_);
        changed = true;
    }
    if (in.KeyPressed(Key::Left, true)) caret_ = PreviousCharacter(value, caret_);
    if (in.KeyPressed(Key::Right, true)) caret_ = NextCharacter(value, caret_);
    if (in.KeyPressed(Key::Home)) caret_ = 0;
    if (in.KeyPressed(Key::End)) caret_ = value.size();
    if (multiline && in.KeyPressed(Key::Enter, true) && CountCharacters(value) < limit) {
        value.insert(caret_, 1, '\n');
        ++caret_;
        changed = true;
    }
    if (numeric && value.empty() && changed && !emptyAllowed) value = "0";
    if (numeric && changed && maxValue >= 0 && !value.empty()) {
        // Read back as a number: over the highest it becomes the highest; "05" reads "5".
        std::int64_t number = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') continue;
            number = std::min(number * 10 + (c - '0'), maxValue + 1);
        }
        const std::string again = std::to_string(std::min(number, maxValue));
        if (again != value) {
            value = again;
            caret_ = value.size();
        }
    }
    if (changed) {
        SetText(value);
        Notify(Notice::Kind::TextChanged);
    } else if (multiline && (in.KeyPressed(Key::Up, true) || in.KeyPressed(Key::Down, true))) {
        // A row up or down, as near the same place along it as there is.
        WrapRows(frame.paint);
        const int row = CaretRow(), to = in.KeyPressed(Key::Up, true) ? row - 1 : row + 1;
        if (to >= 0 && to < int(rows_.size())) {
            const std::string_view all = value;
            const int font = node().fontIndex;
            const float x = frame.paint.TextWidth(all.substr(rows_[std::size_t(row)].begin, caret_ - rows_[std::size_t(row)].begin), font);
            const TextRow& target = rows_[std::size_t(to)];
            std::size_t at = target.begin;
            while (at < target.end) {
                const std::size_t next = NextCharacter(value, at);
                if (frame.paint.TextWidth(all.substr(target.begin, next - target.begin), font) > x) break;
                at = next;
            }
            caret_ = at;
        }
    }
    if (passKeys && in.KeyPressed(Key::Enter)) Notify(Notice::Kind::Submitted);
    if (passKeys && in.KeyPressed(Key::Escape)) Notify(Notice::Kind::Cancelled);
}

void Edit::Draw(Frame& frame) {
    // Its own art and background (most edits have none: the window's art draws the field).
    const std::string saved = text();
    SetText({});
    Static::Draw(frame);
    SetText(saved);
    if (multiline) return DrawRows(frame, saved);
    const Rect client = Inset(*this);
    const int font = node().fontIndex;
    // An IME's composition stands at the caret, as if typed, until it is.
    const std::string composition = focused_ && !password ? frame.input.composition() : std::string();
    std::string typed = saved;
    std::size_t caret = std::min(caret_, saved.size());
    if (!composition.empty()) {
        typed.insert(caret, composition);
        std::size_t at = caret;
        for (int k = 0; k < frame.input.compositionCursor() && at < caret + composition.size(); ++k) at = NextCharacter(typed, at);
        caret = at;
    }
    const std::string shown = password ? std::string(CountCharacters(typed), '*') : typed;
    const std::string beforeCaret = password ? std::string(CountCharacters(typed.substr(0, caret)), '*') : typed.substr(0, caret);
    const float width = frame.paint.TextWidth(shown, font), lineHeight = TextLineHeight(font);
    float x = client.x;
    if (node().hAlign == 1) x = client.x + (client.w - width) * 0.5f;
    if (node().hAlign == 2) x = client.Right() - width - 2.0f;
    float y = client.y;
    if (node().vAlign == 1) y = client.y + (client.h - lineHeight) * 0.5f;
    if (node().vAlign == 2) y = client.Bottom() - lineHeight;
    // Wider than the field: its end shows, 5 pixels in from the right; the caret keeps 40
    // pixels of text to its left in view.
    if (width > client.w - 5.0f) {
        x = client.Right() - 5.0f - width;
        const float caretX = x + frame.paint.TextWidth(beforeCaret, font);
        if (caretX < client.x + 40.0f) x = std::min(client.x, x + (client.x + 40.0f - caretX));
    }
    x = std::trunc(x);
    y = std::trunc(y);
    const float opacity = Opacity();
    const Color color = WithAlpha(enabled ? textColor : Rgba(180, 180, 180), opacity);
    frame.paint.PushClip(ScreenRect());
    frame.paint.Text(shown, {x, y, width + 2.0f, lineHeight}, color, font, 0, 0);
    caretX_ = x + frame.paint.TextWidth(beforeCaret, font);
    if (!composition.empty()) {
        // A 1-pixel line under the composition (the original draws its Chinese IME's this way).
        const std::size_t start = std::min(caret_, saved.size());
        const float from = x + frame.paint.TextWidth(std::string_view(typed).substr(0, start), font);
        const float to = from + frame.paint.TextWidth(composition, font);
        frame.paint.Fill({from, y + lineHeight - 2.0f, to - from, 1.0f}, color);
    }
    // The caret: 2 pixels wide, point size + 2 tall, blinking every 200 ms.
    if (focused_ && std::fmod(frame.time, 0.4) < 0.2) {
        const float caretX = x + frame.paint.TextWidth(beforeCaret, font);
        const float height = FontPoints(font) + 2.0f;
        const float top = saved.empty() ? std::trunc(client.y + (client.h - FontPixels(font) - 2.0f) * 0.5f) : y + 1.0f;
        frame.paint.Fill({caretX, top, 2.0f, height}, color);
    }
    frame.paint.PopClip();
}

// --- List ----------------------------------------------------------------------------------------

int ListView::RowAt(Vec2 p) const {
    const Rect client = Inset(*this);
    if (!client.Contains(p) || lineHeight <= 0.0f) return -1;
    const int row = first + int((p.y - client.y) / lineHeight);
    return row < int(rows.size()) ? row : -1;
}

void ListView::Mouse(Frame& frame) {
    const Input& in = frame.input;
    hovered = hoverTracking && frame.pointer.hovered == this ? RowAt(frame.pointer.position) : -1;
    if (in.Released(MouseButton::Left) && frame.pointer.held == this) {
        const int row = RowAt(frame.pointer.position);
        if (row >= 0) {
            selected = row;
            Notify(Notice::Kind::RowPicked, row);
        }
    }
    if (in.DoubleClicked(MouseButton::Left) && frame.pointer.hovered == this && selected >= 0) {
        Notify(Notice::Kind::RowDoubleClicked, selected);
    }
}

void ListView::Draw(Frame& frame) {
    Widget::Draw(frame);
    const Rect client = Inset(*this);
    const int font = node().fontIndex;
    const float textHeight = TextLineHeight(font);
    const float opacity = Opacity();
    float y = client.y;
    if (node().vAlign == 1) y += (lineHeight - textHeight) * 0.5f;
    if (node().vAlign == 2) y += lineHeight - textHeight;
    const int last = visibleRows > 0 ? std::min(int(rows.size()), first + visibleRows) : int(rows.size());
    for (int i = first; i < last; ++i) {
        const Row& row = rows[std::size_t(i)];
        const float width = frame.paint.TextWidth(row.text, font);
        float x = client.x;
        if (node().hAlign == 1) x = client.x + (client.w - width) * 0.5f;
        if (node().hAlign == 2) x = client.Right() - width;
        // The selected row and the hovered one sit on flat bars from the left edge.
        const Color bar = i == selected && showSelected ? selectedBar : i == hovered && showHover ? hoverBar : 0;
        if ((bar >> 24) != 0) frame.paint.Fill({client.x, y, client.w, textHeight}, WithAlpha(bar, float(bar >> 24) / 255.0f * opacity));
        Color color = row.color != 0 ? row.color : textColor;
        if (i == hovered && hoverColor != 0) color = hoverColor;
        frame.paint.Text(row.text, {std::trunc(x), std::trunc(y), width + 2.0f, textHeight}, WithAlpha(color, opacity), font, 0, 0);
        y += lineHeight;
    }
}

// --- Text box ------------------------------------------------------------------------------------

void TextBox::AddParagraph(std::string text, Color color, int style, Color hover) {
    paragraphs_.push_back({std::move(text), color, style, hover});
    if (maxParagraphs > 0 && paragraphs_.size() > maxParagraphs) {
        paragraphs_.erase(paragraphs_.begin(), paragraphs_.begin() + std::ptrdiff_t(paragraphs_.size() - maxParagraphs));
    }
    dirty_ = true;
    follow_ = true; // a new paragraph shows the end
}

void TextBox::ClearParagraphs() {
    paragraphs_.clear();
    rows_.clear();
    first_ = 0;
    dirty_ = true;
    hoveredRow_ = pressedRow_ = -1; // rows that are gone (a click's own handler may refill the box, the NPC talk's rows)
}

void TextBox::Layout(Painter& paint) {
    // Each paragraph wrapped at the client width; with the hanging indent its later rows start
    // as far in as the spaces that make 20 pixels.
    const int font = node().fontIndex;
    const float width = Inset(*this).w;
    const float space = std::max(1.0f, paint.TextWidth(" ", font));
    const float indent = hangingIndent ? std::ceil(20.0f / space) * space : 0.0f;
    rows_.clear();
    for (std::size_t k = 0; k < paragraphs_.size(); ++k) {
        const Paragraph& p = paragraphs_[k];
        // Each of its lines (broken by '\n' or the escape "\n") starts a row at the left; a line
        // wider than the box goes on in the narrower width.
        for (const std::string& line : Wrap(paint, p.text, 0.0f, font)) {
            const std::vector<std::string> first = Wrap(paint, line, width, font);
            if (first.empty()) continue;
            rows_.push_back({first[0], p.color, p.style, 0.0f, int(k)});
            if (first.size() == 1) continue;
            std::string rest;
            for (std::size_t i = 1; i < first.size(); ++i) rest += first[i];
            for (const std::string& part : Wrap(paint, rest, width - indent, font)) rows_.push_back({part, p.color, p.style, indent, int(k)});
        }
    }
    width_ = width;
    dirty_ = false;
}

void TextBox::ScrollBy(int rows) {
    follow_ = false;
    first_ += rows;
    if (bar != nullptr) bar->SetPosition(first_);
}

int TextBox::RowAt(Vec2 p) const {
    const Rect client = Inset(*this);
    const int shown = visibleRows > 0 ? visibleRows : std::max(1, int(rect.h / lineHeight));
    const int count = std::min(shown, int(rows_.size()) - first_);
    if (count <= 0 || p.x < client.x || p.x > client.Right()) return -1;
    const float top = client.y + (bottomAnchored ? float(shown - count) * lineHeight : 0.0f);
    if (p.y < top) return -1;
    const int row = first_ + int((p.y - top) / lineHeight);
    return row < first_ + count ? row : -1;
}

int TextBox::ParagraphAt(Vec2 p) const {
    const int row = RowAt(p);
    return row >= 0 ? rows_[std::size_t(row)].paragraph : -1;
}

void TextBox::Mouse(Frame& frame) {
    if (!hoverTracking) return;
    const int row = frame.pointer.hovered == this ? RowAt(frame.pointer.position) : -1;
    hoveredRow_ = row;
    pressedRow_ = frame.pointer.held == this && frame.input.Down(MouseButton::Left) ? row : -1;
    if (frame.input.Released(MouseButton::Left) && frame.pointer.held == this && row >= 0) {
        Notify(Notice::Kind::RowPicked, rows_[std::size_t(row)].paragraph);
    }
}

void TextBox::Update(Frame& frame) {
    interactive = hoverTracking;
    hoveredRow_ = pressedRow_ = -1; // the mouse says again (after the updates)
    if (paragraphs_.empty()) return;
    if (dirty_ || width_ != Inset(*this).w) Layout(frame.paint);
    const int shown = visibleRows > 0 ? visibleRows : std::max(1, int(rect.h / lineHeight));
    const int last = std::max(0, int(rows_.size()) - shown);
    if (bar != nullptr) {
        // The bar says where we are (its arrows, its thumb); a new paragraph moves it to the end.
        if (follow_ || last != barLast_) {
            const int at = follow_ ? last : bar->position();
            bar->SetRange(0, last);
            bar->SetPosition(at);
            barLast_ = last;
            follow_ = false;
        }
        first_ = bar->position();
    } else {
        if (follow_) first_ = last;
        follow_ = false;
    }
    first_ = std::clamp(first_, 0, last);
}

void TextBox::Draw(Frame& frame) {
    if (!paragraphs_.empty()) {
        Static::Draw(frame); // its art, no text
        if (dirty_ || width_ != Inset(*this).w) Layout(frame.paint);
        const Rect client = Inset(*this);
        const int font = node().fontIndex;
        const float opacity = Opacity();
        const int shown = visibleRows > 0 ? visibleRows : std::max(1, int(rect.h / lineHeight));
        const int count = std::min(shown, int(rows_.size()) - first_);
        // Bottom-anchored: fewer rows than the box holds sit at its bottom.
        float y = client.y + (bottomAnchored ? float(shown - count) * lineHeight : 0.0f);
        // A paragraph with a hover colour takes it on every row while the mouse is on one of them.
        const int hoveredParagraph = hoveredRow_ >= 0 && hoveredRow_ < int(rows_.size()) ? rows_[std::size_t(hoveredRow_)].paragraph : -1;
        for (int i = first_; i < first_ + count; ++i) {
            const Row& row = rows_[std::size_t(i)];
            if (i == hoveredRow_ && (hoverBar >> 24) != 0) {
                // The hover bar: as wide as the row's text (the chat), or the whole row (the
                // whisper list), the row's height.
                const float width = hoverFullRow ? client.w : frame.paint.TextWidth(row.text, font) + 1.0f;
                frame.paint.Fill({hoverFullRow ? client.x : client.x + row.indent, y, width, lineHeight},
                                 WithAlpha(hoverBar, float(hoverBar >> 24) / 255.0f * opacity));
            }
            const Color hover = row.paragraph == hoveredParagraph ? paragraphs_[std::size_t(row.paragraph)].hover : Color(0);
            const Color color = i == pressedRow_ ? kWhite : hover != 0 ? hover : row.color;
            DrawLabel(frame.paint, row.text, {client.x + row.indent, y, client.w - row.indent, lineHeight}, WithAlpha(color, opacity), font,
                      0, 0, row.style);
            y += lineHeight;
        }
        return;
    }
    const std::string saved = text();
    SetText({});
    Static::Draw(frame);
    SetText(saved);
    // Wrapped at the client width, a drop shadow (its text style 1), not clipped.
    const Rect client = Inset(*this);
    const int font = node().fontIndex;
    float y = client.y;
    const Color color = WithAlpha(enabled ? textColor : Argb(0xFFB4B4B4), Opacity());
    for (const std::string& line : Wrap(frame.paint, saved, client.w, font)) {
        DrawLabel(frame.paint, line, {client.x, y, client.w, lineHeight}, color, font, node().hAlign, 0, kTextShadow);
        y += lineHeight;
    }
}

// --- Rich text (SML2) -----------------------------------------------------------------------------

void RichText::Draw(Frame& frame) {
    const std::string saved = text();
    SetText({});
    Static::Draw(frame);
    SetText(saved);
    // The original takes only "<sml2>...</sml2>"; anything else keeps the old content (none here).
    std::string_view markup = saved;
    if (markup.size() < 14 || !markup.starts_with("<sml2>") || markup.find("</sml2>") == std::string_view::npos) return;
    const Rect client = Inset(*this);
    const int font = node().fontIndex;
    const float opacity = Opacity();
    struct Word {
        std::string text;
        Color color;
        bool newline = false;
    };
    std::vector<Word> words;
    std::vector<Color> colors{textColor};
    int align = node().hAlign;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) words.push_back({std::exchange(current, {}), colors.back()});
    };
    for (std::size_t i = 0; i < markup.size();) {
        if (markup[i] == '<') {
            const std::size_t end = markup.find('>', i);
            if (end == std::string_view::npos) break;
            std::string_view tag = markup.substr(i + 1, end - i - 1);
            flush();
            if (tag == "br" || tag == "br/" || tag.starts_with("p") || tag == "/p") {
                words.push_back({{}, colors.back(), true});
            } else if (tag == "center") {
                align = 1;
            } else if (tag == "right") {
                align = 2;
            } else if (tag == "left") {
                align = 0;
            } else if (tag.starts_with("font")) {
                Color c = colors.back();
                if (const std::size_t q = tag.find("color=\""); q != std::string_view::npos) {
                    int v[4] = {255, 255, 255, 255};
                    std::sscanf(std::string(tag.substr(q + 7)).c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]);
                    c = Rgba(std::uint8_t(v[1]), std::uint8_t(v[2]), std::uint8_t(v[3]), std::uint8_t(v[0]));
                }
                colors.push_back(c);
            } else if (tag == "/font" && colors.size() > 1) {
                colors.pop_back();
            }
            i = end + 1;
        } else {
            current += markup[i];
            if (markup[i] == ' ') flush();
            ++i;
        }
    }
    flush();
    const float lineHeight = TextLineHeight(font);
    std::vector<std::vector<const Word*>> lines(1);
    float x = 0.0f;
    for (const Word& w : words) {
        if (w.newline) {
            lines.emplace_back();
            x = 0.0f;
            continue;
        }
        const float width = frame.paint.TextWidth(w.text, font);
        if (x > 0.0f && x + width > client.w) {
            lines.emplace_back();
            x = 0.0f;
        }
        lines.back().push_back(&w);
        x += width;
    }
    float y = client.y;
    for (const auto& line : lines) {
        float width = 0.0f;
        for (const Word* w : line) width += frame.paint.TextWidth(w->text, font);
        float at = align == 1 ? client.x + (client.w - width) * 0.5f : align == 2 ? client.Right() - width : client.x;
        at = std::trunc(at);
        for (const Word* w : line) {
            const float ww = frame.paint.TextWidth(w->text, font);
            frame.paint.Text(w->text, {at, y, ww + 2.0f, lineHeight}, WithAlpha(w->color, opacity), font, 0, 0);
            at += ww;
        }
        y += lineHeight;
    }
}

// --- Combo box -----------------------------------------------------------------------------------

namespace {

// The open list: the black square's frame on an opaque fill, with a 1-pixel grey line around it so
// it stands apart from the black field above it and the rows under it.
class ComboList : public StretchBox {
public:
    using StretchBox::StretchBox;
    void Draw(Frame& frame) override {
        StretchBox::Draw(frame);
        const Rect r = ScreenRect();
        const Color line = WithAlpha(Argb(0xFF6B6B6B), Opacity());
        frame.paint.Fill({r.x, r.y, r.w, 1.0f}, line);
        frame.paint.Fill({r.x, r.Bottom() - 1.0f, r.w, 1.0f}, line);
        frame.paint.Fill({r.x, r.y, 1.0f, r.h}, line);
        frame.paint.Fill({r.Right() - 1.0f, r.y, 1.0f, r.h}, line);
    }
};

} // namespace

void ComboBox::Created(Interface& interface) {
    Static::Created(interface);
    // The arrow (1), the black field (2), the chosen text (3, white).
    assets::UiLayoutNode arrow;
    arrow.type = "CIFButton";
    arrow.id = 1;
    arrow.rect = {rect.w - 20, 0, 20, rect.h};
    arrow.ddj = "interface\\ifcommon\\com_qst_downarrow_button.ddj";
    Add(interface.Make(arrow));
    assets::UiLayoutNode field;
    field.type = "CIFStretchWnd";
    field.id = 2;
    field.rect = {0, 0, rect.w - 20, rect.h};
    field.ddj = "interface\\ifcommon\\com_blacksquare_";
    auto box = interface.Make(field);
    box->background = Argb(0xFF000000);
    Add(std::move(box));
    assets::UiLayoutNode chosen;
    chosen.type = "CIFStatic";
    chosen.id = 3;
    chosen.rect = {5, 0, rect.w - 30, rect.h};
    chosen.vAlign = 1;
    Add(interface.Make(chosen));
    interface_ = &interface;
    interactive = true; // the field opens the list too
}

ComboBox::~ComboBox() {
    if (interface_ != nullptr) interface_->Disown(this);
}

void ComboBox::Select(int index, bool notify) {
    selected = index >= 0 && index < int(items.size()) ? index : -1;
    if (Widget* chosen = Child(3)) chosen->SetText(selected >= 0 ? items[std::size_t(selected)] : std::string());
    if (notify) Notify(Notice::Kind::ComboPicked, selected);
}

bool ComboBox::open() const { return interface_ != nullptr && interface_->popup() != nullptr && interface_->popupOwner() == this; }

void ComboBox::Close() {
    if (open()) interface_->ClosePopup();
}

void ComboBox::Open() {
    if (interface_ == nullptr || items.empty()) return;
    // The list below the field: a pop-up window of its own (the black square's frame on an opaque
    // black fill: the original's half black lets the rows under it show through), rows at point
    // size + 4, a scroll bar beside them when there are more than it shows.
    const int shown = std::max(1, std::min(rowsShown, int(items.size())));
    const bool scrolls = int(items.size()) > shown;
    const float lineHeight = 9.0f + 4.0f;
    assets::UiLayoutNode popupNode;
    popupNode.type = "CIFStretchWnd";
    popupNode.name = "GDR_COMBO_LIST";
    popupNode.ddj = "interface\\ifcommon\\com_blacksquare_";
    const Vec2 o = Origin();
    popupNode.rect = {o.x, o.y + rect.h, scrolls ? rect.w : rect.w - 20.0f, lineHeight * float(shown) + 8.0f};
    std::unique_ptr<Widget> popup = std::make_unique<ComboList>(popupNode);
    popup->background = Argb(0xFF000000);
    assets::UiLayoutNode listNode;
    listNode.type = "CIFListCtrl";
    listNode.id = 4;
    listNode.rect = {4, 4, popupNode.rect.w - 8 - (scrolls ? 18.0f : 0.0f), popupNode.rect.h - 8};
    ListView* list = nullptr;
    if (auto made = interface_->Make(listNode); (list = As<ListView>(made.get())) != nullptr) {
        list->lineHeight = lineHeight;
        list->visibleRows = shown;
        list->hoverTracking = true;
        list->showHover = true;
        list->showSelected = false;
        list->hoverBar = Argb(0xFF8080FF);
        list->hoverColor = Argb(0xFFFFFF80);
        for (const std::string& item : items) list->rows.push_back({item});
        popup->Add(std::move(made));
    }
    if (scrolls) {
        // The bar's arrows sit 16 above and below its travel (its own layout's parts).
        assets::UiLayoutNode barNode;
        barNode.type = "CIFVerticalScroll";
        barNode.id = 5;
        barNode.rect = {popupNode.rect.w - 18.0f, 18.0f, 16.0f, popupNode.rect.h - 52.0f};
        if (auto made = interface_->Make(barNode); auto* bar = As<ScrollBar>(made.get())) {
            bar->SetRange(0, int(items.size()) - shown);
            bar->SetPosition(std::max(0, selected)); // the chosen row in view
            if (list != nullptr) list->first = bar->position();
            popup->Add(std::move(made));
        }
    }
    Interface* ui = interface_;
    Widget* window = popup.get();
    popup->onNotice = [this, ui, window](const Notice& n) {
        // (Only while this combo still owns the list: the interface forgets an owner going away.)
        if (ui->popup() != window || ui->popupOwner() != this) return;
        if (n.kind == Notice::Kind::RowPicked || n.kind == Notice::Kind::RowDoubleClicked) {
            ui->ClosePopup();
            Select(n.value, true);
        }
    };
    interface_->OpenPopup(*this, std::move(popup));
}

bool ComboBox::OnNotice(const Notice& notice) {
    // The arrow opens the list, or closes it when it is open.
    if (notice.kind != Notice::Kind::Clicked || notice.source->parent() != this || notice.control != 1) return false;
    if (open()) Close();
    else Open();
    return true;
}

void ComboBox::Mouse(Frame& frame) {
    // A click on the field does what the arrow does.
    if (frame.pointer.held == this && frame.input.Released(MouseButton::Left) && Hit(frame.pointer.position)) {
        if (open()) Close();
        else Open();
    }
}

void ComboBox::Update(Frame& frame) {
    if (!open()) return;
    Widget* popup = interface_->popup();
    // The list follows the field (a window dragged, a list scrolled), above it when it would leave
    // the screen below.
    const Vec2 o = Origin();
    popup->rect.x = o.x;
    popup->rect.y = o.y + rect.h;
    if (popup->rect.Bottom() > frame.interface.screen().y && o.y - popup->rect.h >= 0.0f) popup->rect.y = o.y - popup->rect.h;
    // The wheel over it scrolls it; the rows follow the bar.
    auto* bar = As<ScrollBar>(popup->Child(5));
    auto* list = As<ListView>(popup->Child(4));
    if (bar == nullptr || list == nullptr) return;
    if (frame.input.wheel() != 0.0f && popup->ScreenRect().Contains(frame.pointer.position)) {
        bar->SetPosition(bar->position() + (frame.input.wheel() > 0.0f ? -1 : 1));
    }
    list->first = bar->position();
}

} // namespace sr::ui
