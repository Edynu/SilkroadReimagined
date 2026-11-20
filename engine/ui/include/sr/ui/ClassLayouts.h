#pragma once

// Which resinfo layout each interface class builds itself from (the original names one file in
// each window class's create function).

#include <cstddef>
#include <span>
#include <string_view>

namespace sr::ui {

// "resinfo\if....txt" for a class name such as "CIFInventory"; empty when the class loads none.
std::string_view LayoutOfClass(std::string_view className);
std::size_t ClassLayoutCount();
// The class that builds itself from a layout ("resinfo\ifinventory.txt" -> "CIFInventory").
std::string_view ClassOfLayout(std::string_view layoutPath);
// The window classes whose chrome is the original's main frame (CIFMainFrame: a frame, a title,
// a strip to drag it by and a close button; RTTI).
bool IsMainFrameClass(std::string_view className);
std::span<const std::string_view> MainFrameClasses();

} // namespace sr::ui
