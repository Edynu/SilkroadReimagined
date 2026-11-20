#include "title/UiKit.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

std::string Variant(const std::string& path, const char* suffix) {
    const std::size_t dot = path.rfind('.');
    return dot == std::string::npos ? path + suffix : path.substr(0, dot) + suffix + path.substr(dot);
}

void PopCharacter(std::string& s) {
    while (!s.empty()) {
        const auto c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

} // namespace

float UiKit::FontSize(int fontIndex) {
    constexpr float kSizes[] = {12.0f, 11.0f, 16.0f, 15.0f, 20.0f};
    return kSizes[std::clamp(fontIndex, 0, 4)];
}

ui::Rect UiKit::Screen(core::DesignRect r) const {
    const core::DesignRect s = grid_.ToScreen(r);
    return {s.x, s.y, s.w, s.h};
}

void UiKit::Image(const std::string& path, core::DesignRect r, float alpha) { ImageScreen(path, Screen(r), alpha); }

void UiKit::ImageScreen(const std::string& path, ui::Rect r, float alpha) {
    const UiTextures::Ref t = textures_.Get(path);
    if (!t.resident || alpha <= 0.0f || r.w <= 0.0f || r.h <= 0.0f) return;
    List().Image(t.texture, r, {0, 0}, {1, 1}, ui::WithAlpha(ui::kWhite, alpha));
}

ui::Vec2 UiKit::TextureSize(const std::string& path) {
    const UiTextures::Ref t = textures_.Get(path);
    return {t.width, t.height};
}

void UiKit::Fill(core::DesignRect r, ui::Color color) { List().Fill(Screen(r), color); }

float UiKit::TextWidth(std::string_view text, int fontIndex) {
    ui::Font* font = data_.Font();
    if (!font || text.empty()) return 0.0f;
    return font->Measure(text, FontSize(fontIndex) * grid_.scale()).x / grid_.scale();
}

void UiKit::Text(std::string_view text, core::DesignRect r, ui::Color color, int fontIndex, int hAlign, int vAlign) {
    ui::Font* font = data_.Font();
    if (text.empty() || !font) return;
    const float size = FontSize(fontIndex) * grid_.scale();
    const ui::Vec2 extent = font->Measure(text, size);
    const ui::Rect s = Screen(r);
    ui::Vec2 pos{s.x, s.y};
    if (hAlign == 1) pos.x = s.x + std::floor((s.w - extent.x) * 0.5f);
    if (hAlign == 2) pos.x = s.Right() - extent.x;
    if (vAlign == 1) pos.y = s.y + std::floor((s.h - extent.y) * 0.5f);
    if (vAlign == 2) pos.y = s.Bottom() - extent.y;
    List().PushClip(s);
    font->Draw(List(), text, pos, size, color);
    List().PopClip();
}

void UiKit::RichText(std::string_view markup, core::DesignRect r, ui::Color color, int fontIndex, int hAlign) {
    ui::Font* font = data_.Font();
    if (markup.empty() || !font) return;
    struct Word {
        std::string text; // with its trailing space
        ui::Color color;
        bool breaks = false; // a line break instead of a word
    };
    std::vector<Word> words;
    std::string current;
    ui::Color now = color;
    const auto flush = [&] {
        if (!current.empty()) words.push_back({std::exchange(current, {}), now});
    };
    for (std::size_t i = 0; i < markup.size();) {
        const char c = markup[i];
        if (c == '<') {
            const std::size_t end = markup.find('>', i);
            if (end == std::string_view::npos) break;
            const std::string_view tag = markup.substr(i + 1, end - i - 1);
            flush();
            if (tag.starts_with("font")) {
                // color="A,R,G,B"
                int v[4] = {255, 255, 255, 255};
                if (const std::size_t q = tag.find('"'); q != std::string_view::npos) {
                    std::sscanf(std::string(tag.substr(q + 1)).c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]);
                }
                now = ui::Rgba(std::uint8_t(v[1]), std::uint8_t(v[2]), std::uint8_t(v[3]), std::uint8_t(v[0]));
            } else if (tag.starts_with("/font")) {
                now = color;
            }
            i = end + 1;
        } else if (c == '\\' && i + 1 < markup.size() && markup[i + 1] == 'n') {
            flush();
            words.push_back({{}, now, true});
            i += 2;
        } else if (c == '\n') {
            flush();
            words.push_back({{}, now, true});
            ++i;
        } else {
            current += c;
            ++i;
            if (c == ' ') flush();
        }
    }
    flush();
    // Lines no wider than the rect (a word wider than it gets a line of its own).
    std::vector<std::vector<const Word*>> lines(1);
    float x = 0.0f;
    for (const Word& w : words) {
        if (w.breaks) {
            lines.emplace_back();
            x = 0.0f;
            continue;
        }
        const float width = TextWidth(w.text, fontIndex);
        if (x > 0.0f && x + TextWidth(std::string(w.text.substr(0, w.text.find_last_not_of(' ') + 1)), fontIndex) > r.w) {
            lines.emplace_back();
            x = 0.0f;
        }
        lines.back().push_back(&w);
        x += width;
    }
    const float lineHeight = font->LineHeight(FontSize(fontIndex) * grid_.scale()) / grid_.scale();
    float y = r.y;
    for (const auto& line : lines) {
        float width = 0.0f;
        for (const Word* w : line) width += TextWidth(w->text, fontIndex);
        float at = hAlign == 1 ? r.x + std::floor((r.w - width) * 0.5f) : r.x;
        for (const Word* w : line) {
            const float ww = TextWidth(w->text, fontIndex);
            Text(w->text, {at, y, ww + 2.0f, lineHeight}, w->color, fontIndex, 0, 0);
            at += ww;
        }
        y += lineHeight;
    }
}

void UiKit::TextBox(std::string_view text, core::DesignRect r, ui::Color color, int fontIndex) {
    ui::Font* font = data_.Font();
    if (text.empty() || !font) return;
    const float size = FontSize(fontIndex) * grid_.scale();
    const ui::Rect s = Screen(r);
    List().PushClip(s);
    float y = s.y;
    for (const std::string& line : font->Wrap(text, size, s.w)) {
        font->Draw(List(), line, {s.x, y}, size, color);
        y += font->LineHeight(size);
    }
    List().PopClip();
}

void UiKit::NodeText(const assets::UiLayoutNode& node, std::string_view text, core::DesignRect r, float alpha) {
    const assets::UiLayoutColor c = node.fontColor;
    std::string shown(text);
    // A little too wide runs over its rect; more is cut with "..." (inferred from screenshots of the
    // original: in Arial "EXP" 5 over its 28, "Level", "Height" whole; "Volume" 10 over its 45 and
    // "Weapon" cut: "Vo...", "Wea..").
    constexpr float kSlack = 8.0f;
    if (TextWidth(shown, node.fontIndex) > r.w + kSlack) {
        while (!shown.empty() && TextWidth(shown + "...", node.fontIndex) > r.w + kSlack) {
            PopCharacter(shown);
        }
        shown += "...";
    }
    if (TextWidth(shown, node.fontIndex) > r.w) {
        r.x -= node.hAlign == 1 ? kSlack / 2 : node.hAlign == 2 ? kSlack : 0.0f;
        r.w += kSlack;
    }
    Text(shown, r, ui::WithAlpha(ui::Rgba(c.r, c.g, c.b), alpha * float(c.a) / 255.0f), node.fontIndex, node.hAlign, 1);
}

core::DesignRect UiKit::Inset(const assets::UiLayoutNode& node, core::DesignRect r) {
    // ClientRect holds insets: left, top, right, bottom.
    if (!node.hasClientRect) return r;
    return {r.x + node.clientRect.x, r.y + node.clientRect.y, r.w - node.clientRect.x - node.clientRect.w,
            r.h - node.clientRect.y - node.clientRect.h};
}

ui::Context::ItemState UiKit::Item(std::string_view id, core::DesignRect r, bool enabled) {
    const ui::Rect s = Screen(r);
    ui_.Solid(s);
    return ui_.Item(id, s, enabled);
}

bool UiKit::Button(std::string_view id, const assets::UiLayoutNode& node, core::DesignRect r, bool enabled) {
    return ButtonState(id, node, r, enabled).clicked;
}

ui::Context::ItemState UiKit::ButtonState(std::string_view id, const assets::UiLayoutNode& node, core::DesignRect r, bool enabled) {
    const ui::Context::ItemState state = Item(id, r, enabled);
    const bool pressed = state.held && state.hovered;
    std::string path = node.ddj;
    const char* suffix = !enabled ? "_disable" : pressed ? "_press" : state.hovered ? "_focus" : nullptr;
    if (suffix && textures_.Exists(Variant(node.ddj, suffix))) path = Variant(node.ddj, suffix);
    Image(path, r);
    if (!node.text.empty()) {
        const float shift = pressed ? 1.0f : 0.0f; // the caption moves with the pressed face
        NodeText(node, data_.Text(node.text), Inset(node, {r.x + shift, r.y + shift, r.w, r.h}), enabled ? 1.0f : 0.5f);
    }
    return state;
}

bool UiKit::TextButton(std::string_view id, std::string_view label, core::DesignRect r, bool enabled, bool selected) {
    const ui::Context::ItemState state = Item(id, r, enabled);
    const ui::Color fill = !enabled       ? ui::Rgba(20, 22, 26, 200)
                           : state.held   ? ui::Rgba(70, 58, 30, 230)
                           : selected     ? ui::Rgba(90, 72, 32, 230)
                           : state.hovered ? ui::Rgba(45, 40, 28, 230)
                                           : ui::Rgba(24, 26, 32, 220);
    Fill(r, fill);
    List().Outline(Screen(r), selected ? ui::Rgba(240, 200, 110) : ui::Rgba(150, 125, 70, 220), std::max(1.0f, grid_.scale()));
    Text(label, r, enabled ? ui::Rgba(240, 230, 200) : ui::Rgba(120, 120, 120), 0, 1, 1);
    return state.clicked;
}

bool UiKit::Edit(std::string_view id, const assets::UiLayoutNode& node, core::DesignRect r, std::string& value, std::size_t capacity,
                 bool password, bool takeFocus) {
    r = Inset(node, r);
    const ui::Id key = ui_.GetId(id);
    const ui::Rect s = Screen(r);
    ui_.Solid(s);
    const ui::Context::ItemState state = ui_.Item(key, s, true);
    if (state.pressed || takeFocus) ui_.SetFocus(key);
    const ui::Input& in = ui_.input();
    bool entered = false;
    if (ui_.HasFocus(key)) {
        if (in.Pressed(ui::MouseButton::Left) && !s.Contains(in.mouse())) {
            ui_.ClearFocus(); // clicked elsewhere
        } else if (!ui_.keyboardBlocked()) {
            for (char c : in.text()) {
                if (static_cast<unsigned char>(c) >= 0x20 && value.size() < capacity) value += c;
            }
            if (in.KeyPressed(ui::Key::Backspace, true)) PopCharacter(value);
            if (in.ctrl() && in.KeyPressed(ui::Key::V) && ui_.getClipboard) {
                for (char c : ui_.getClipboard()) {
                    if (static_cast<unsigned char>(c) >= 0x20 && value.size() < capacity) value += c;
                }
            }
            entered = in.KeyPressed(ui::Key::Enter);
        }
    }
    const std::string shown = password ? std::string(value.size(), '*') : value;
    const assets::UiLayoutColor c = node.fontColor;
    const ui::Color colour = node.hasFontColor ? ui::Rgba(c.r, c.g, c.b, c.a) : ui::kWhite;
    const core::DesignRect text{r.x + 2, r.y, r.w - 4, r.h};
    const float width = TextWidth(shown, node.fontIndex);
    const float shift = std::max(0.0f, width - text.w + 2); // long text scrolls; the end stays visible
    List().PushClip(Screen(text));
    Text(shown, {text.x - shift, text.y, text.w + shift, text.h}, colour, node.fontIndex, 0, 1);
    if (ui_.HasFocus(key) && std::fmod(in.time(), 1.0) < 0.5) {
        const float x = text.x - shift + width + 1, h = FontSize(node.fontIndex);
        Fill({x, r.y + (r.h - h) * 0.5f, 1, h}, colour);
    }
    List().PopClip();
    return entered;
}

} // namespace sr::client
