#include "frame/FrameStats.h"

#include <algorithm>
#include <format>

namespace sr::client {

namespace {

struct Spread {
    double average = 0, p50 = 0, p99 = 0, worst = 0;
};

Spread SpreadOf(std::vector<double> values) {
    Spread s;
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (double v : values) sum += v;
    s.average = sum / static_cast<double>(values.size());
    s.p50 = values[values.size() / 2];
    s.p99 = values[std::min(values.size() - 1, values.size() * 99 / 100)];
    s.worst = values.back();
    return s;
}

} // namespace

std::string FrameRecorder::Summary() const {
    std::lock_guard lock(mutex_);
    // The first frames build pipelines and fill caches: leave them out.
    const std::size_t skip = std::min<std::size_t>(samples_.size() / 10, 60);
    std::vector<double> interval, render, game, streaming, burstIntervals;
    std::size_t sinceBurst = 1000;
    for (std::size_t i = skip; i < samples_.size(); ++i) {
        const Sample& s = samples_[i];
        interval.push_back(s.intervalMs);
        render.push_back(s.renderMs);
        game.push_back(s.gameMs);
        streaming.push_back(s.streamingMs);
        sinceBurst = s.burst ? 0 : sinceBurst + 1;
        if (sinceBurst < 10) burstIntervals.push_back(s.intervalMs); // the burst's frame and the 9 after
    }
    const Spread f = SpreadOf(interval), r = SpreadOf(render), g = SpreadOf(game), b = SpreadOf(burstIntervals),
                 st = SpreadOf(streaming);
    std::string out = std::format(
        "frames {} | frame ms avg {:.2f} ({:.1f} FPS) p50 {:.2f} p99 {:.2f} worst {:.2f}\n"
        "game thread ms avg {:.2f} p99 {:.2f} worst {:.2f} | render thread ms avg {:.2f} p99 {:.2f} worst {:.2f}\n"
        "overlap: game + render = {:.2f} ms against a {:.2f} ms frame",
        interval.size(), f.average, f.average > 0 ? 1000.0 / f.average : 0.0, f.p50, f.p99, f.worst, g.average, g.p99,
        g.worst, r.average, r.p99, r.worst, g.average + r.average, f.average);
    if (st.worst > 0) {
        out += std::format("\nstreaming (render thread) ms avg {:.3f} p99 {:.3f} worst {:.3f}", st.average, st.p99, st.worst);
    }
    if (!burstIntervals.empty()) {
        out += std::format("\nbursts: worst frame {:.2f} ms within 10 frames of a burst, {:.2f}x the median", b.worst,
                           f.p50 > 0 ? b.worst / f.p50 : 0.0);
    }
    // Long frames (the streaming budgets: none over 20 ms, none over twice the median), and the
    // worst few with what each thread spent, to tell loading from other causes.
    std::size_t over20 = 0, overTwice = 0;
    std::vector<std::size_t> order;
    for (std::size_t i = skip; i < samples_.size(); ++i) {
        over20 += samples_[i].intervalMs > 20.0 ? 1 : 0;
        overTwice += samples_[i].intervalMs > 2.0 * f.p50 ? 1 : 0;
        order.push_back(i);
    }
    out += std::format("\nlong frames: {} over 20 ms, {} over twice the median ({:.2f} ms)", over20, overTwice, 2.0 * f.p50);
    const std::size_t shown = std::min<std::size_t>(order.size(), 5);
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(shown), order.end(),
                      [&](std::size_t a, std::size_t b2) { return samples_[a].intervalMs > samples_[b2].intervalMs; });
    for (std::size_t k = 0; k < shown; ++k) {
        const Sample& s = samples_[order[k]];
        out += std::format("\n  frame {}: {:.2f} ms (game {:.2f}, render {:.2f}, streaming {:.3f})", order[k], s.intervalMs, s.gameMs,
                           s.renderMs, s.streamingMs);
    }
    return out;
}

} // namespace sr::client
