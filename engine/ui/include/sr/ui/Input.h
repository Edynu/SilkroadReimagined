#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "sr/ui/Types.h"

namespace sr::ui {

// The keys the game reads. Letters and digits keep their ASCII codes.
enum class Key : std::uint16_t {
    None = 0,
    Backspace = 8,
    Tab = 9,
    Enter = 13,
    Escape = 27,
    Space = 32,
    Num0 = '0', Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    A = 'A', B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Delete = 127,
    Left = 256, Right, Up, Down, Home, End, PageUp, PageDown, Insert,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Semicolon, Grave, Shift, Ctrl, Alt,
    Count
};

enum class MouseButton : std::uint8_t { Left = 0, Right = 1, Middle = 2 };

// One input event, translated from the OS by sr::platform.
struct InputEvent {
    enum class Type : std::uint8_t { None, MouseMove, MouseDown, MouseUp, Wheel, KeyDown, KeyUp, Text, Composition, FocusLost };
    Type type = Type::None;
    Vec2 position;          // MouseMove / MouseDown / MouseUp (pixels)
    MouseButton button = MouseButton::Left;
    std::uint8_t clicks = 0; // MouseDown: the OS's count of quick clicks (2 = a double click); 0 = unknown
    float wheel = 0.0f;     // Wheel: positive = away from the user
    Key key = Key::None;    // KeyDown / KeyUp
    bool repeat = false;    // KeyDown held down
    std::string text;       // Text: UTF-8 typed characters; Composition: what the IME is composing (empty: done)
    int cursor = 0;         // Composition: the IME's cursor in it (characters)
};

// The input of one frame: state plus what happened since the last frame.
class Input {
public:
    // Start of a frame: forgets the previous frame's presses, releases, text and wheel.
    void BeginFrame(double timeSeconds);
    void Feed(const InputEvent& event);

    Vec2 mouse() const { return mouse_; }
    Vec2 mouseDelta() const { return {mouse_.x - previousMouse_.x, mouse_.y - previousMouse_.y}; }
    bool Down(MouseButton b) const { return down_[Index(b)]; }
    bool Pressed(MouseButton b) const { return pressed_[Index(b)]; }
    bool Released(MouseButton b) const { return released_[Index(b)]; }
    Vec2 PressPosition(MouseButton b) const { return pressPosition_[Index(b)]; }
    // The second press of a double click went down this frame.
    bool DoubleClicked(MouseButton b) const { return doubleClicked_[Index(b)]; }
    float wheel() const { return wheel_; }

    bool KeyDown(Key k) const { return keyDown_[static_cast<std::size_t>(k)]; }
    // Went down this frame (auto-repeat counts when `repeat`).
    bool KeyPressed(Key k, bool repeat = false) const {
        return keyPressed_[static_cast<std::size_t>(k)] || (repeat && keyRepeated_[static_cast<std::size_t>(k)]);
    }
    bool ctrl() const { return KeyDown(Key::Ctrl); }
    bool shift() const { return KeyDown(Key::Shift); }
    const std::string& text() const { return text_; } // typed this frame
    // What an IME (Korean, Chinese, Japanese) is composing and not yet typed, shown in the edit at
    // its caret; it lasts across frames until the IME ends it.
    const std::string& composition() const { return composition_; }
    int compositionCursor() const { return compositionCursor_; }
    double time() const { return time_; }

private:
    static std::size_t Index(MouseButton b) { return static_cast<std::size_t>(b); }

    Vec2 mouse_{-1.0f, -1.0f}, previousMouse_{-1.0f, -1.0f};
    std::array<bool, 3> down_{}, pressed_{}, released_{}, doubleClicked_{};
    std::array<double, 3> lastPressTime_{-1.0, -1.0, -1.0};
    std::array<Vec2, 3> pressPosition_{};
    float wheel_ = 0.0f;
    std::array<bool, static_cast<std::size_t>(Key::Count)> keyDown_{}, keyPressed_{}, keyRepeated_{};
    std::string text_;
    std::string composition_;
    int compositionCursor_ = 0;
    double time_ = 0.0;
};

} // namespace sr::ui
