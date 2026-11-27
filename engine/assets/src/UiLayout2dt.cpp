#include "sr/assets/UiLayout2dt.h"

#include <cstring>

namespace sr::assets {

namespace {

constexpr std::size_t kRecord = 976;

std::string Text(std::span<const std::byte> r, std::size_t at, std::size_t size) {
    const char* p = reinterpret_cast<const char*>(r.data() + at);
    return std::string(p, strnlen(p, size));
}

std::uint32_t U32(std::span<const std::byte> r, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, r.data() + at, 4);
    return v;
}

} // namespace

bool DecodeUi2dt(std::span<const std::byte> bytes, std::vector<Ui2dtRecord>& out, std::string& error) {
    out.clear();
    if (bytes.size() < 4) {
        error = "shorter than its count";
        return false;
    }
    const std::uint32_t count = U32(bytes, 0);
    if (bytes.size() != 4 + std::size_t(count) * kRecord) {
        error = "its size is not its count of 976-byte records";
        return false;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::span<const std::byte> r = bytes.subspan(4 + std::size_t(i) * kRecord, kRecord);
        Ui2dtRecord c;
        c.className = Text(r, 0x000, 64);
        c.frame = Text(r, 0x040, 256);
        c.art = Text(r, 0x140, 256);
        c.textKey = Text(r, 0x240, 128);
        c.kind = U32(r, 0x340);
        c.id = int(U32(r, 0x344));
        for (int k = 0; k < 3; ++k) c.parents[k] = int(U32(r, 0x348 + 4 * std::size_t(k)));
        const std::uint32_t argb = U32(r, 0x358);
        c.colour = {std::uint8_t(argb >> 24), std::uint8_t(argb >> 16), std::uint8_t(argb >> 8), std::uint8_t(argb)};
        c.rect = {float(std::int32_t(U32(r, 0x35C))), float(std::int32_t(U32(r, 0x360))), float(std::int32_t(U32(r, 0x364))),
                  float(std::int32_t(U32(r, 0x368)))};
        c.tabGroup = int(U32(r, 0x38C));
        c.tabPage = int(U32(r, 0x390));
        c.hAlign = int(std::to_integer<std::uint8_t>(r[0x3CD]));
        c.vAlign = int(std::to_integer<std::uint8_t>(r[0x3CE]));
        out.push_back(std::move(c));
    }
    return true;
}

UiLayoutNode Ui2dtNode(const Ui2dtRecord& c, float originX, float originY) {
    UiLayoutNode n;
    n.id = c.id;
    n.name = c.className;
    n.rect = {c.rect.x - originX, c.rect.y - originY, c.rect.w, c.rect.h};
    n.text = c.textKey;
    n.fontColor = c.colour;
    n.hasFontColor = true;
    n.hAlign = c.hAlign;
    n.vAlign = c.vAlign;
    switch (c.kind) {
    case 0: // the root: the window's chrome
        n.type = "CIFMainFrame";
        n.ddj = c.frame;
        break;
    case 1:  // a frame
    case 3:  // a black square (a frame of its own art)
    case 18: // a sub-window
        n.type = "CIFFrame";
        n.ddj = c.frame;
        break;
    case 2: // a tile
        n.type = "CIFNormalTile";
        n.ddj = c.frame;
        break;
    case 4:  // a button
    case 16: // a tab (its art the unselected face)
        n.type = "CIFButton";
        n.ddj = c.art;
        break;
    case 6:
        n.type = "CIFEdit";
        n.ddj = c.art;
        break;
    case 10:
        n.type = "CIFGauge";
        n.ddj = c.art;
        break;
    case 11:
        n.type = "CIFCheckBox";
        n.ddj = c.art;
        break;
    case 13: // the new interface's scroll bar (CNIFVerticalScroll): the classic one's parts, its own arrows
        n.type = "CIFVerticalScroll";
        if (n.name.empty()) n.name = "CNIFVerticalScroll";
        break;
    default: // statics, texts, slots, rows: their art and text
        n.type = "CIFStatic";
        n.ddj = c.art.empty() ? c.frame : c.art;
        break;
    }
    return n;
}

} // namespace sr::assets
