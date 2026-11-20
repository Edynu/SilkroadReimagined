#include "screens/Screen.h"

#include "sr/core/Log.h"
#include "sr/core/Trace.h"

namespace sr::client {

void ScreenMachine::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    if (screen_) {
        SR_ZONE("screen: update");
        screen_->Update(context, snapshot);
        if (requested_.empty() && !screen_->next.empty()) requested_ = screen_->next;
    }
    if (requested_.empty()) return;
    const std::string name = std::move(requested_);
    requested_.clear();
    const auto factory = factories_.find(name);
    if (factory == factories_.end()) {
        core::LogWarning("no screen named '{}'", name);
        if (screen_) screen_->next.clear();
        return;
    }
    if (screen_) screen_->Exit(context, snapshot);
    screen_ = factory->second();
    screen_->Enter(context);
    ++switches_;
}

void ScreenMachine::Stop(ScreenContext& context, FrameSnapshot& snapshot) {
    if (screen_) screen_->Exit(context, snapshot);
    screen_.reset();
}

} // namespace sr::client
