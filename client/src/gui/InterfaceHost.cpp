#include "gui/InterfaceHost.h"

#include "sr/ui/Controls.h"

namespace sr::client {

void InterfaceHost::Load(ScreenContext& context) {
    layouts_ = AllLayouts(context.files);
    data_.Load(context.jobs, context.files, layouts_);
    textures_ = std::make_unique<UiTextures>(context.assets, context.uiWarmup);
    resources_ = std::make_unique<UiResources>(*textures_, data_);
}

ui::Interface* InterfaceHost::Prepare(const std::function<void(ui::Interface&)>& classes) {
    if (interface_) return interface_.get();
    if (!data_.Ready()) return nullptr;
    interface_ = std::make_unique<ui::Interface>(*resources_, [this](const std::string& path) { return data_.Layout(path); });
    ui::RegisterControls(*interface_);
    if (classes) classes(*interface_);
    return interface_.get();
}

void InterfaceHost::Frame(ScreenContext& context, FrameSnapshot& snapshot, const ui::Input& input, float scale,
                          const std::function<void(ui::Painter&)>& under, bool windows) {
    if (!interface_) return;
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    ui::Font* font = data_.Font();
    if (font) atlas_.Sync(context.device, *font);
    ui_.font = font;
    ui_.getClipboard = context.getClipboard;
    ui_.setClipboard = context.setClipboard;
    ui_.NewFrame(input, {width, height}, context.mouseBlocked, context.keyboardBlocked);
    if (under) {
        ui_.BeginLayer("under the interface", ui::LayerKind::Background);
        ui::Painter paint(ui_.draw(), interface_->resources(), scale);
        under(paint);
        ui_.EndLayer();
    }
    if (windows) interface_->Frame(ui_, scale, context.time, float(context.dt));
    ui_.EndFrame();
    if (font) atlas_.Sync(context.device, *font);
    for (const ui::DrawList* list : ui_.DrawOrder()) AppendUi(snapshot.ui, *list);
    if (ui_.typing()) {
        context.textInput = true;
        context.textArea = ui_.textArea();
        context.textCaret = ui_.textCaret();
    }
    shownFrames_ = interface_->Settled() ? shownFrames_ + 1 : 0;
}

void InterfaceHost::Exit(FrameSnapshot& snapshot) {
    interface_.reset();
    data_.Wait();
    if (const rhi::TextureHandle t = atlas_.Release()) snapshot.releaseTextures.push_back(t);
    resources_.reset();
    textures_.reset();
}

} // namespace sr::client
