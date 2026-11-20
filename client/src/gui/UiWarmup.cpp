#include "gui/UiWarmup.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "sr/core/Log.h"
#include "sr/core/Path.h"

namespace sr::client {

namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
} // namespace

void UiWarmup::Ask(Batch& batch, const std::string& path) {
    if (!asked_.insert(core::NormalizeAssetPath(path)).second) return;
    const gfx::AssetId id = assets_.RequestTexture(path, kUiWarmupPriority);
    if (!id) return;
    if (batch.done) {
        batch.done = false;
        batch.started = Now();
    }
    batch.ids.push_back(id);
    ++batch.total;
}

bool UiWarmup::Follow(Batch& batch) {
    if (batch.done) return true;
    while (batch.settled < batch.ids.size()) {
        const gfx::AssetState s = assets_.State(batch.ids[batch.settled]);
        if (s != gfx::AssetState::Resident && s != gfx::AssetState::Failed) return false;
        ++batch.settled;
    }
    core::LogInfo("ui warm-up: {} {} in after {:.2f} s", batch.total, batch.name, Now() - batch.started);
    Release(batch); // in: the store keeps them, under its budget
    batch.done = true;
    return true;
}

void UiWarmup::Release(Batch& batch) {
    for (const gfx::AssetId id : batch.ids) assets_.Release(id);
    batch.ids.clear();
    batch.settled = 0;
}

void UiWarmup::Load(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        core::LogInfo("ui warm-up: no {} yet (made at the end of this run)", file);
        return;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    list_.Decode(text);
    const std::vector<std::string> paths = list_.Paths();
    for (const std::string& p : paths) Ask(fromList_, p);
    core::LogInfo("ui warm-up: {} textures from {}", paths.size(), file);
}

void UiWarmup::Preload(std::span<const std::string> paths) {
    for (const std::string& p : paths) Ask(preloaded_, p);
}

bool UiWarmup::ListReady() {
    Follow(preloaded_);
    return Follow(fromList_);
}

bool UiWarmup::Save(const std::string& file) const {
    // Written beside, then put in place: a run that ends half-way never leaves half a list. The temporary file is this
    // run's own (two clients on one machine end at their own times).
    const std::string temporary =
        file + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string text = list_.Encode();
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::string named;
    for (const std::string& n : coldNames_) named += (named.empty() ? "" : ", ") + n;
    core::LogInfo("ui warm-up: {} textures drawn this run, {} of them not in when first asked for{}{}; list written to {}", list_.drawnThisRun(),
                  cold_, named.empty() ? "" : ": ", named, file);
    return true;
}

void UiWarmup::ReleaseAll() {
    Release(fromList_);
    Release(preloaded_);
    fromList_.done = preloaded_.done = true;
    asked_.clear();
}

} // namespace sr::client
