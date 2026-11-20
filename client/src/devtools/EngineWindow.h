#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "frame/FrameStats.h"
#include "sr/core/Jobs.h"
#include "sr/gfx/AssetStore.h"

namespace sr::core {
class Config;
}
namespace sr::assets {
class FileSystem;
}

namespace sr::client {

// The developer tools' "Engine" window (F1): frame times, the job system, the devices, the
// mounted archives and the settings. It also counts the archives' files in a background job,
// polled from the frame (the pattern every loader follows: never wait from the frame).
class EngineWindow {
public:
    struct Info {
        std::string gpu;
        std::string audio;
        const core::Config* config = nullptr;
        const assets::FileSystem* files = nullptr;
        const gfx::AssetStore* assets = nullptr;
    };

    explicit EngineWindow(Info info);
    ~EngineWindow();
    void AddFrameTime(double seconds);
    // `feedback`: the render thread's latest numbers; `screen`: the active screen.
    void Draw(core::JobSystem& jobs, const RenderFeedback& feedback, const char* screen, bool renderThread);

private:
    void RunJobTest(core::JobSystem& jobs);

    Info info_;
    std::array<float, 240> frameMs_{};
    std::size_t frameCursor_ = 0;

    struct FileCount {
        core::JobCounter done;
        std::atomic<std::size_t> files{0};
    };
    std::unique_ptr<FileCount> fileCount_;

    double jobTestParallelMs_ = 0.0;
    double jobTestSerialMs_ = 0.0;
    bool jobTestRan_ = false;
    bool jobTestCorrect_ = false;
};

} // namespace sr::client
