#include "devtools/EngineWindow.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

#include "sr/assets/FileSystem.h"
#include "sr/core/Config.h"
#include "sr/core/Profiler.h"
#include "sr/core/Trace.h"

namespace sr::client {

EngineWindow::EngineWindow(Info info) : info_(std::move(info)), fileCount_(std::make_unique<FileCount>()) {
    if (info_.files != nullptr && core::JobSystem::Exists()) {
        core::JobSystem::Get().Run(
            [files = info_.files, count = fileCount_.get()] {
                SR_ZONE("engine window: count files");
                count->files.store(files->ListFiles().size(), std::memory_order_release);
            },
            core::JobPriority::Streaming, &fileCount_->done);
    }
}

EngineWindow::~EngineWindow() {
    // The counting job holds a pointer into fileCount_; it must finish first. It is a streaming
    // job, so poll instead of Wait() (which only helps with frame jobs).
    while (!fileCount_->done.Done()) std::this_thread::yield();
}

void EngineWindow::AddFrameTime(double seconds) {
    frameMs_[frameCursor_] = static_cast<float>(seconds * 1000.0);
    frameCursor_ = (frameCursor_ + 1) % frameMs_.size();
}

void EngineWindow::RunJobTest(core::JobSystem& jobs) {
    // Sums sqrt over 4 million numbers, once on one thread and once over all workers.
    constexpr std::size_t kCount = 4'000'000;
    constexpr std::size_t kBatch = 16'384;
    const auto work = [](std::size_t begin, std::size_t end) {
        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) sum += std::sqrt(static_cast<double>(i));
        return sum;
    };

    auto start = std::chrono::steady_clock::now();
    double serial = 0.0;
    {
        SR_ZONE("job test: one thread");
        serial = work(0, kCount);
    }
    jobTestSerialMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    std::vector<double> partial((kCount + kBatch - 1) / kBatch, 0.0);
    start = std::chrono::steady_clock::now();
    {
        SR_ZONE("job test: all workers");
        jobs.ParallelFor(kCount, kBatch, [&](std::size_t begin, std::size_t end) {
            SR_ZONE("job test: batch");
            partial[begin / kBatch] = work(begin, end);
        });
    }
    jobTestParallelMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    double parallel = 0.0;
    for (double p : partial) parallel += p;
    jobTestCorrect_ = std::abs(parallel - serial) <= 1e-6 * std::abs(serial);
    jobTestRan_ = true;
}

void EngineWindow::Draw(core::JobSystem& jobs, const RenderFeedback& feedback, const char* screen, bool renderThread) {
    ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({460, 560}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Engine")) {
        ImGui::End();
        return;
    }

    // Frame times, oldest to newest.
    std::array<float, 240> ordered{};
    float worst = 0.0f, sum = 0.0f;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        ordered[i] = frameMs_[(frameCursor_ + i) % frameMs_.size()];
        worst = std::max(worst, ordered[i]);
        sum += ordered[i];
    }
    const float average = sum / static_cast<float>(ordered.size());
    ImGui::Text("frame %.2f ms (%.0f FPS), worst of the last %zu: %.2f ms", average,
                average > 0.0f ? 1000.0f / average : 0.0f, ordered.size(), worst);
    ImGui::PlotLines("##frames", ordered.data(), static_cast<int>(ordered.size()), 0, nullptr, 0.0f,
                     std::max(worst, 16.7f) * 1.1f, {-1.0f, 60.0f});
    ImGui::Text("screen: %s | %s", screen, renderThread ? "game + render threads" : "one thread (--render-thread off)");
    if (ImGui::CollapsingHeader("Render thread", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("work %.2f ms, waiting for the GPU %.2f ms, GPU %.2f ms", feedback.renderWorkMs, feedback.gpuWaitMs,
                    feedback.device.gpuMs);
        ImGui::Text("draws %u, instances %u, triangles %llu, passes %u", feedback.device.draws, feedback.device.instances,
                    static_cast<unsigned long long>(feedback.device.primitives), feedback.device.passes);
        ImGui::Text("live: %u textures, %u buffers, %u pipelines (%u waiting for the GPU)", feedback.resources.textures,
                    feedback.resources.buffers, feedback.resources.pipelines, feedback.resources.pendingDeletions);
    }
    if (!feedback.gpuTimers.empty() && ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The render graph's passes (one timer each), passes of the same name added up.
        std::vector<std::pair<const char*, float>> merged;
        for (const auto& t : feedback.gpuTimers) {
            auto it = std::find_if(merged.begin(), merged.end(), [&](const auto& e) { return std::strcmp(e.first, t.name) == 0; });
            if (it == merged.end()) {
                merged.push_back({t.name, t.ms});
            } else {
                it->second += t.ms;
            }
        }
        std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [name, ms] : merged) ImGui::Text("%7.3f ms  %s", ms, name);
    }
    if (ImGui::CollapsingHeader("Streaming")) {
        // the asset store, the staging ring and the transfer queue.
        const auto mib = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
        ImGui::Text("render thread: %.3f ms a frame; %u blocks (%.2f MiB) sent this frame", feedback.streamingMs,
                    feedback.device.streamed, mib(feedback.device.streamedBytes));
        ImGui::Text("staging %.1f / %.0f MiB; uploads %llu submitted, %llu visible; %s transfer queue",
                    mib(feedback.streaming.stagingUsed), mib(feedback.streaming.stagingCapacity),
                    static_cast<unsigned long long>(feedback.streaming.submitted), static_cast<unsigned long long>(feedback.streaming.uploaded),
                    feedback.streaming.dedicatedTransferQueue ? "dedicated" : "shared");
        if (feedback.memory.budget > 0) {
            ImGui::Text("GPU memory %.0f / %.0f MiB (driver budget)", mib(feedback.memory.usage), mib(feedback.memory.budget));
        }
        if (info_.assets != nullptr) {
            const gfx::AssetStore::Stats s = info_.assets->GetStats();
            ImGui::Text("assets %zu (%zu kept unreferenced): %zu resident, %zu loading, %zu failed, %llu evicted", s.live,
                        s.unreferenced, s.resident, s.queued + s.loading + s.waiting + s.uploading, s.failed,
                        static_cast<unsigned long long>(s.evicted));
            ImGui::Text("textures %.0f / %.0f MiB; geometry pool %.0f / %.0f MiB, largest free %.0f MiB", mib(s.textureBytes),
                        mib(s.textureBudgetBytes), mib(s.geometry.used), mib(s.geometry.capacity), mib(s.geometry.largestFree));
        }
    }
    if (ImGui::CollapsingHeader("Game thread", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const core::Profiler::Entry& entry : core::Profiler::Get().Last()) {
            if (entry.time) {
                ImGui::Text("%-28s %7.3f ms", entry.name, entry.value);
            } else {
                ImGui::Text("%-28s %7.0f", entry.name, entry.value);
            }
        }
    }

    if (ImGui::CollapsingHeader("Jobs", ImGuiTreeNodeFlags_DefaultOpen)) {
        const core::JobSystem::Stats stats = jobs.GetStats();
        ImGui::Text("workers: %u", jobs.WorkerCount());
        ImGui::Text("jobs run: %llu (stolen %llu, run while waiting %llu)", static_cast<unsigned long long>(stats.executed),
                    static_cast<unsigned long long>(stats.stolen), static_cast<unsigned long long>(stats.helped));
        if (ImGui::Button("Job test (4M square roots)")) RunJobTest(jobs);
        if (jobTestRan_) {
            ImGui::Text("one thread %.2f ms, all workers %.2f ms (x%.1f)%s", jobTestSerialMs_, jobTestParallelMs_,
                        jobTestParallelMs_ > 0.0 ? jobTestSerialMs_ / jobTestParallelMs_ : 0.0,
                        jobTestCorrect_ ? "" : "  RESULTS DIFFER");
        }
    }

    if (ImGui::CollapsingHeader("Devices", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("GPU: %s", info_.gpu.c_str());
        ImGui::Text("audio: %s", info_.audio.c_str());
        ImGui::Text("profiler: %s", core::ProfilerConnected() ? "Tracy connected" :
#ifdef SR_TRACY
                                                                 "Tracy waiting (connect the 0.14.1 profiler)"
#else
                                                                 "built without Tracy"
#endif
        );
    }

    if (info_.files != nullptr && ImGui::CollapsingHeader("Files", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& source : info_.files->Sources()) {
            ImGui::BulletText("%.*s", static_cast<int>(source->Name().size()), source->Name().data());
        }
        if (fileCount_->done.Done()) {
            ImGui::Text("%zu files", fileCount_->files.load(std::memory_order_acquire));
        } else {
            ImGui::TextDisabled("counting files...");
        }
    }

    if (info_.config != nullptr && ImGui::CollapsingHeader("Settings")) {
        for (const auto& [key, value] : info_.config->Values()) ImGui::Text("%s = %s", key.c_str(), value.c_str());
    }
    ImGui::End();
}

} // namespace sr::client
