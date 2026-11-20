#pragma once

// The 1.188 interface's newer layouts (Media.pk2 res_ui\*.2dt):
// the windows of the CNIF* classes (the guild window res_ui\guild.2dt, its relations page
// guild_r.2dt, the community pages, the target menu, ...) are lists of controls in a binary file
// instead of resinfo's text.
//
// A file is a u32 count, then that many 976-byte records, each a control with its parent before it
// (measured over all 42 files, 2,312 records):
//
//   +0x000  char[64]   the class (CNIFGuildWnd, CNIFGuildUserSlot; empty for plain controls)
//   +0x040  char[256]  the frame's art (a prefix completed by left_up.ddj ..., or a tile's .ddj)
//   +0x140  char[256]  the control's art (.ddj)
//   +0x240  char[128]  its text key (UIIT_...)
//   +0x2C0  char[64]   the designer's name for it (CP949)
//   +0x300  char[64]   sample text (CP949; the code fills the real one)
//   +0x340  u32        the kind (0 the root, 1 a frame, 2 a tile, 3 a black square, 4 a button,
//                      5 a static, 6 an edit, 7 a text, 8 a slot, 10 a gauge, 11 a check box,
//                      13 a scroll bar, 15 a row, 16 a tab, 18 a sub-window, ...)
//   +0x344  u32        its id
//   +0x348  u32[3]     its parent's id, the grandparent's, the next (0: none)
//   +0x358  u32        the text's ARGB colour
//   +0x35C  i32[4]     x, y, width, height (every control in the file's own space)
//   +0x38C  u32[2]     a tab's group and the page it shows
//   +0x3CC  u8[4]      byte 1 the text's horizontal alignment (0 left, 1 centre, 2 right), byte 2
//                      the vertical (0 top, 1 centre, 2 bottom)

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sr/assets/UiLayout.h"

namespace sr::assets {

struct Ui2dtRecord {
    std::string className, frame, art, textKey;
    std::uint32_t kind = 0;
    int id = 0;
    int parents[3] = {0, 0, 0};
    UiLayoutColor colour;
    UiLayoutRect rect; // in the file's space
    int tabGroup = 0, tabPage = 0;
    int hAlign = 1, vAlign = 1;
};

bool DecodeUi2dt(std::span<const std::byte> bytes, std::vector<Ui2dtRecord>& out, std::string& error);

// The record as a resinfo-style node of the control class our interface makes for its kind
// (CIFFrame, CIFNormalTile, CIFButton, CIFStatic, ...), its rect relative to `origin`.
UiLayoutNode Ui2dtNode(const Ui2dtRecord& record, float originX, float originY);

} // namespace sr::assets
