#include "sr/platform/Window.h"

#include <SDL3/SDL.h>

namespace sr::platform {

namespace {
SDL_Window* Sdl(void* w) { return static_cast<SDL_Window*>(w); }

ui::Key ToKey(SDL_Keycode k) {
    if (k >= SDLK_A && k <= SDLK_Z) return static_cast<ui::Key>('A' + (k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9) return static_cast<ui::Key>('0' + (k - SDLK_0));
    if (k >= SDLK_F1 && k <= SDLK_F12) return static_cast<ui::Key>(static_cast<int>(ui::Key::F1) + (k - SDLK_F1));
    switch (k) {
    case SDLK_BACKSPACE: return ui::Key::Backspace;
    case SDLK_TAB: return ui::Key::Tab;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return ui::Key::Enter;
    case SDLK_ESCAPE: return ui::Key::Escape;
    case SDLK_SPACE: return ui::Key::Space;
    case SDLK_DELETE: return ui::Key::Delete;
    case SDLK_LEFT: return ui::Key::Left;
    case SDLK_RIGHT: return ui::Key::Right;
    case SDLK_UP: return ui::Key::Up;
    case SDLK_DOWN: return ui::Key::Down;
    case SDLK_HOME: return ui::Key::Home;
    case SDLK_END: return ui::Key::End;
    case SDLK_PAGEUP: return ui::Key::PageUp;
    case SDLK_PAGEDOWN: return ui::Key::PageDown;
    case SDLK_INSERT: return ui::Key::Insert;
    case SDLK_SEMICOLON: return ui::Key::Semicolon;
    case SDLK_GRAVE: return ui::Key::Grave;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return ui::Key::Shift;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return ui::Key::Ctrl;
    case SDLK_LALT:
    case SDLK_RALT: return ui::Key::Alt;
    default: return ui::Key::None;
    }
}
} // namespace

Window::~Window() {
    for (auto& [key, cursor] : cursors_) SDL_DestroyCursor(static_cast<SDL_Cursor*>(cursor));
    cursors_.clear();
    if (window_ != nullptr) {
        SDL_DestroyWindow(Sdl(window_));
        SDL_Quit();
    }
}

bool Window::Create(const char* title, int width, int height, std::string& error) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
    window_ = SDL_CreateWindow(title, width, height, flags);
    if (window_ == nullptr) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        SDL_Quit();
        return false;
    }
    return true;
}

bool Window::PumpEvents(const std::function<void(const void* sdlEvent)>& onEvent) {
    bool keepRunning = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (onEvent) {
            onEvent(&event);
        }
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            keepRunning = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resized_ = true;
            break;
        default:
            break;
        }
    }
    return keepRunning;
}

std::uint32_t Window::PixelWidth() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(Sdl(window_), &w, &h);
    return static_cast<std::uint32_t>(w);
}

std::uint32_t Window::PixelHeight() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(Sdl(window_), &w, &h);
    return static_cast<std::uint32_t>(h);
}

float Window::DisplayScale() const {
    const float scale = SDL_GetWindowDisplayScale(Sdl(window_));
    return scale > 0.0f ? scale : 1.0f;
}

bool Window::IsMinimized() const { return (SDL_GetWindowFlags(Sdl(window_)) & SDL_WINDOW_MINIMIZED) != 0; }

bool Window::ConsumeResized() {
    const bool was = resized_;
    resized_ = false;
    return was;
}

void Window::Minimize() { SDL_MinimizeWindow(Sdl(window_)); }

void Window::Restore() { SDL_RestoreWindow(Sdl(window_)); }

bool Window::Resize(int width, int height) {
    if (window_ == nullptr || width <= 0 || height <= 0) return false;
    if (SDL_GetWindowFlags(Sdl(window_)) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) {
        SDL_SetWindowFullscreen(Sdl(window_), false);
        SDL_RestoreWindow(Sdl(window_));
    }
    return SDL_SetWindowSize(Sdl(window_), width, height);
}

std::string Window::LastError() const { return SDL_GetError(); }

float Window::RefreshRate() const {
    if (window_ == nullptr) return 0.0f;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(Sdl(window_)));
    return mode != nullptr ? mode->refresh_rate : 0.0f;
}

bool Window::SetMode(Mode mode) {
    if (window_ == nullptr) return false;
    SDL_Window* w = Sdl(window_);
    switch (mode) {
    case Mode::Windowed: return SDL_SetWindowFullscreen(w, false);
    case Mode::Borderless:
        // A null mode is the desktop's own: borderless, no display mode change.
        return SDL_SetWindowFullscreenMode(w, nullptr) && SDL_SetWindowFullscreen(w, true);
    case Mode::Fullscreen: {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(w);
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
        SDL_DisplayMode closest{};
        int width = 0, height = 0;
        SDL_GetWindowSize(w, &width, &height);
        if (desktop == nullptr ||
            !SDL_GetClosestFullscreenDisplayMode(display, width, height, desktop->refresh_rate, true, &closest)) {
            return SDL_SetWindowFullscreenMode(w, nullptr) && SDL_SetWindowFullscreen(w, true);
        }
        return SDL_SetWindowFullscreenMode(w, &closest) && SDL_SetWindowFullscreen(w, true);
    }
    }
    return false;
}

void Window::WarpMouse(float x, float y) {
    // The UI's pixels back to the window's coordinates SDL takes.
    int windowW = 0, windowH = 0, pixelW = 0, pixelH = 0;
    SDL_GetWindowSize(Sdl(window_), &windowW, &windowH);
    SDL_GetWindowSizeInPixels(Sdl(window_), &pixelW, &pixelH);
    const float sx = pixelW > 0 ? static_cast<float>(windowW) / static_cast<float>(pixelW) : 1.0f;
    const float sy = pixelH > 0 ? static_cast<float>(windowH) / static_cast<float>(pixelH) : 1.0f;
    SDL_WarpMouseInWindow(Sdl(window_), x * sx, y * sy);
}

void Window::HideCursor(bool hide) {
    if (hide == cursorHidden_) return;
    cursorHidden_ = hide;
    if (hide) {
        SDL_HideCursor();
    } else {
        SDL_ShowCursor();
    }
}

bool Window::Translate(const void* sdlEvent, ui::InputEvent& out) const {
    const SDL_Event& e = *static_cast<const SDL_Event*>(sdlEvent);
    // SDL reports the mouse in window coordinates; the UI works in pixels.
    int windowW = 0, windowH = 0, pixelW = 0, pixelH = 0;
    SDL_GetWindowSize(Sdl(window_), &windowW, &windowH);
    SDL_GetWindowSizeInPixels(Sdl(window_), &pixelW, &pixelH);
    const float sx = windowW > 0 ? static_cast<float>(pixelW) / static_cast<float>(windowW) : 1.0f;
    const float sy = windowH > 0 ? static_cast<float>(pixelH) / static_cast<float>(windowH) : 1.0f;
    out = ui::InputEvent{};
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        out.type = ui::InputEvent::Type::MouseMove;
        out.position = {e.motion.x * sx, e.motion.y * sy};
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button > SDL_BUTTON_RIGHT) return false;
        out.type = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? ui::InputEvent::Type::MouseDown : ui::InputEvent::Type::MouseUp;
        out.position = {e.button.x * sx, e.button.y * sy};
        out.button = e.button.button == SDL_BUTTON_LEFT    ? ui::MouseButton::Left
                     : e.button.button == SDL_BUTTON_RIGHT ? ui::MouseButton::Right
                                                           : ui::MouseButton::Middle;
        out.clicks = e.button.clicks;
        // While a button is held the window keeps the mouse even outside it (the original's
        // SetCapture: a drag or a scroll thumb follows the mouse past the window's edge).
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            SDL_CaptureMouse(true);
        } else if ((SDL_GetMouseState(nullptr, nullptr) & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK)) == 0) {
            SDL_CaptureMouse(false);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        out.type = ui::InputEvent::Type::Wheel;
        out.wheel = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        out.key = ToKey(e.key.key);
        if (out.key == ui::Key::None) return false;
        out.type = e.type == SDL_EVENT_KEY_DOWN ? ui::InputEvent::Type::KeyDown : ui::InputEvent::Type::KeyUp;
        out.repeat = e.key.repeat;
        return true;
    case SDL_EVENT_TEXT_INPUT:
        out.type = ui::InputEvent::Type::Text;
        out.text = e.text.text != nullptr ? e.text.text : "";
        return true;
    case SDL_EVENT_TEXT_EDITING:
        out.type = ui::InputEvent::Type::Composition;
        out.text = e.edit.text != nullptr ? e.edit.text : "";
        out.cursor = e.edit.start;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        out.type = ui::InputEvent::Type::FocusLost;
        return true;
    default: return false;
    }
}

void Window::AddCursor(int key, const std::uint8_t* rgba, int width, int height, int hotX, int hotY) {
    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, const_cast<std::uint8_t*>(rgba), width * 4);
    if (surface == nullptr) return;
    SDL_Cursor* cursor = SDL_CreateColorCursor(surface, hotX, hotY);
    SDL_DestroySurface(surface);
    if (cursor == nullptr) return;
    if (const auto it = cursors_.find(key); it != cursors_.end()) SDL_DestroyCursor(static_cast<SDL_Cursor*>(it->second));
    cursors_[key] = cursor;
}

void Window::UseCursor(int key) {
    if (key == cursor_) return;
    cursor_ = key;
    const auto it = cursors_.find(key);
    SDL_SetCursor(it != cursors_.end() ? static_cast<SDL_Cursor*>(it->second) : SDL_GetDefaultCursor());
}

void Window::SetTextInput(bool on) {
    if (on == SDL_TextInputActive(Sdl(window_))) return;
    if (on) {
        SDL_StartTextInput(Sdl(window_));
    } else {
        SDL_StopTextInput(Sdl(window_));
    }
}

void Window::SetTextInputArea(float x, float y, float w, float h, float caretX) {
    const SDL_Rect area{int(x), int(y), int(w), int(h)};
    SDL_SetTextInputArea(Sdl(window_), &area, int(caretX - x));
}

std::string Window::ClipboardText() const {
    char* text = SDL_GetClipboardText();
    std::string out = text != nullptr ? text : "";
    SDL_free(text);
    return out;
}

void Window::SetClipboardText(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

bool OpenUrl(const std::string& url) {
    if (!url.starts_with("https://") && !url.starts_with("http://")) return false;
    return SDL_OpenURL(url.c_str());
}

double Window::Time() const { return static_cast<double>(SDL_GetTicksNS()) / 1e9; }

Window::NativeHandles Window::Native() const {
    NativeHandles handles;
#ifdef _WIN32
    const SDL_PropertiesID props = SDL_GetWindowProperties(Sdl(window_));
    handles.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    handles.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
#endif
    return handles;
}

} // namespace sr::platform
