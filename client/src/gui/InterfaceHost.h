#pragma once

// The original's interface on a screen (C7): its data (the
// layouts, the text table and the font, read on a job), its textures, the window manager with
// every control registered, and the UI context its windows draw into. The gallery and the game
// screen each own one.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "frame/GameUi.h"
#include "gui/UiData.h"
#include "gui/UiResources.h"
#include "screens/Screen.h"
#include "sr/ui/Context.h"
#include "sr/ui/Interface.h"

namespace sr::client {

class InterfaceHost {
public:
    // Starts loading every layout (resinfo\*.txt) with the text table and the font.
    void Load(ScreenContext& context);
    // The interface, made the first time the data is in (null until then), with the controls
    // and whatever window classes `classes` registers.
    ui::Interface* Prepare(const std::function<void(ui::Interface&)>& classes = {});
    ui::Interface* interface() { return interface_.get(); }

    // One frame: the windows take `input` (the mouse and the keys), update and draw; their draw
    // lists go into the snapshot. Afterwards context().mouseOverUi() says whether the mouse was
    // over a window (the world takes clicks only where none is).
    // `under`: drawn under every window, in a layer that takes no mouse (the speech over heads).
    // `windows` false: the windows left out this frame (neither drawn nor given the input; a quest scene's cinema), the
    // layer under them drawn still.
    void Frame(ScreenContext& context, FrameSnapshot& snapshot, const ui::Input& input, float scale,
               const std::function<void(ui::Painter& paint)>& under = {}, bool windows = true);
    // Releases the font atlas through the snapshot, the windows and the textures.
    void Exit(FrameSnapshot& snapshot);

    UiData& data() { return data_; }
    // Its textures (null before Load): the program's own go in as external ones (portraits).
    UiTextures* textures() { return textures_.get(); }
    ui::Context& context() { return ui_; }
    const std::vector<std::string>& layouts() const { return layouts_; }
    // Every texture the windows asked for has streamed in, for a few frames (captures wait).
    bool Settled() const { return shownFrames_ >= 3; }

private:
    UiData data_;
    std::unique_ptr<UiTextures> textures_;
    std::unique_ptr<UiResources> resources_;
    std::unique_ptr<ui::Interface> interface_;
    ui::Context ui_;
    FontAtlas atlas_;
    std::vector<std::string> layouts_;
    int shownFrames_ = 0;
};

} // namespace sr::client
