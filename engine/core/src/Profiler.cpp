#include "sr/core/Profiler.h"

namespace sr::core {

Profiler& Profiler::Get() {
    static Profiler profiler;
    return profiler;
}

void Profiler::NextFrame() {
    last_ = current_;
    // Keep the names (and their order) so a quiet frame still lists them, at zero.
    for (Entry& e : current_) e.value = 0.0;
}

Profiler::Entry& Profiler::Slot(const char* name, bool time) {
    for (Entry& e : current_) {
        if (e.name == name || std::string_view(e.name) == name) return e;
    }
    current_.push_back({name, 0.0, time});
    return current_.back();
}

} // namespace sr::core
