#include "sr/ui/Input.h"

namespace sr::ui {

void Input::BeginFrame(double timeSeconds) {
    time_ = timeSeconds;
    previousMouse_ = mouse_;
    pressed_ = {};
    released_ = {};
    doubleClicked_ = {};
    keyPressed_ = {};
    keyRepeated_ = {};
    wheel_ = 0.0f;
    text_.clear();
}

void Input::Feed(const InputEvent& e) {
    switch (e.type) {
    case InputEvent::Type::MouseMove: mouse_ = e.position; break;
    case InputEvent::Type::MouseDown:
        mouse_ = e.position;
        down_[Index(e.button)] = true;
        pressed_[Index(e.button)] = true;
        {
            // Two presses within half a second, a few pixels apart (like the OS default).
            const std::size_t b = Index(e.button);
            const Vec2 last = pressPosition_[b];
            const bool near = (e.position.x - last.x) * (e.position.x - last.x) + (e.position.y - last.y) * (e.position.y - last.y) < 36.0f;
            // The OS's double click when it says (its own time and distance, as the original's
            // WM_LBUTTONDBLCLK: every second click of a run); else our own measure.
            doubleClicked_[b] = e.clicks > 0 ? e.clicks % 2 == 0
                                                 : lastPressTime_[b] >= 0.0 && time_ - lastPressTime_[b] < 0.5 && near;
            lastPressTime_[b] = doubleClicked_[b] ? -1.0 : time_;
        }
        pressPosition_[Index(e.button)] = e.position;
        break;
    case InputEvent::Type::MouseUp:
        mouse_ = e.position;
        down_[Index(e.button)] = false;
        released_[Index(e.button)] = true;
        break;
    case InputEvent::Type::Wheel: wheel_ += e.wheel; break;
    case InputEvent::Type::KeyDown: {
        const auto k = static_cast<std::size_t>(e.key);
        if (k == 0 || k >= keyDown_.size()) break;
        if (e.repeat) {
            keyRepeated_[k] = true;
        } else {
            keyPressed_[k] = true;
        }
        keyDown_[k] = true;
        break;
    }
    case InputEvent::Type::KeyUp: {
        const auto k = static_cast<std::size_t>(e.key);
        if (k != 0 && k < keyDown_.size()) keyDown_[k] = false;
        break;
    }
    case InputEvent::Type::Text:
        text_ += e.text;
        composition_.clear(); // typed: the composition is done
        break;
    case InputEvent::Type::Composition:
        composition_ = e.text;
        compositionCursor_ = e.cursor;
        break;
    case InputEvent::Type::FocusLost:
        keyDown_ = {};
        composition_.clear();
        for (std::size_t b = 0; b < down_.size(); ++b) {
            if (down_[b]) released_[b] = true;
        }
        down_ = {};
        break;
    case InputEvent::Type::None: break;
    }
}

} // namespace sr::ui
