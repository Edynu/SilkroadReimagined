#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "sr/ui/Input.h"

namespace sr::platform {

// OS window and event pump (SDL3 underneath). SDL is used only for the window
// and input; rendering goes through sr::rhi.
class Window {
public:
    struct NativeHandles {
        void* hinstance = nullptr; // Win32
        void* hwnd = nullptr;      // Win32
    };

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const char* title, int width, int height, std::string& error);

    // Processes pending OS events. Each raw event is passed to `onEvent` as an
    // `const SDL_Event*` (for the ImGui backend). Returns false once the user
    // asked to close the window.
    bool PumpEvents(const std::function<void(const void* sdlEvent)>& onEvent);

    // Size of the drawable area in pixels.
    std::uint32_t PixelWidth() const;
    std::uint32_t PixelHeight() const;
    // UI scale factor of the display the window is on (1.0 = 96 DPI).
    float DisplayScale() const;
    bool IsMinimized() const;
    // True once after the drawable size changed.
    bool ConsumeResized();
    // Sets the size of the window's content area (a maximized or full-screen window is first
    // restored to a window). False when the OS refuses.
    bool Resize(int width, int height);
    // Minimises or restores the window (tests; the player does it from the title bar).
    void Minimize();
    void Restore();
    // How the window covers the screen: a normal window, borderless over the whole desktop,
    // or exclusive fullscreen (the display mode nearest the window's size at the desktop's
    // refresh rate).
    enum class Mode { Windowed, Borderless, Fullscreen };
    bool SetMode(Mode mode);
    // The OS's reason for the last failed call.
    std::string LastError() const;
    // The refresh rate of the display the window is on (Hz; 0 when unknown).
    float RefreshRate() const;

    // The game UI's view of an OS event (mouse in pixels); false for events it does not use.
    bool Translate(const void* sdlEvent, ui::InputEvent& out) const;
    // Text input (typed characters, IME) on or off; the game turns it on while a text field
    // has the keyboard.
    void SetTextInput(bool on);
    // Where the focused text field is (pixels) and its caret's x: the IME's candidate list opens
    // under it.
    void SetTextInputArea(float x, float y, float w, float h, float caretX);
    std::string ClipboardText() const;
    void SetClipboardText(const std::string& text);
    // Seconds since the window was created.
    double Time() const;

    NativeHandles Native() const;
    void* SdlWindow() const { return window_; } // SDL_Window*, for the ImGui platform backend

    // Mouse cursors: an RGBA image (top-down) with its hotspot under `key`; UseCursor
    // shows one (an unknown key shows the system arrow).
    void AddCursor(int key, const std::uint8_t* rgba, int width, int height, int hotX, int hotY);
    void UseCursor(int key);
    // Someone else (the developer tools) set the cursor: the next UseCursor sets it again.
    void ForgetCursor() { cursor_ = -2; }
    // Puts the mouse at a place in the window, in pixels (the UI's units).
    void WarpMouse(float x, float y);
    // Hides the cursor (the camera's turn with the cursor held) or shows it again.
    void HideCursor(bool hide);

private:
    void* window_ = nullptr;
    std::unordered_map<int, void*> cursors_; // SDL_Cursor*
    int cursor_ = -1;
    bool cursorHidden_ = false;
    bool resized_ = false;
};

// Opens an http(s) address in the player's browser (an update's download page); false for anything else or a failure.
bool OpenUrl(const std::string& url);

} // namespace sr::platform
