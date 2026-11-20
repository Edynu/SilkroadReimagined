#pragma once

// The C2 asset check: `--asset-check bsr,bms,ddj` requests every
// file of those types through the asset store, a window of requests at a time, while frames keep
// rendering (the newest resident model is drawn). Each asset must become Resident or fail with a
// reason; the summary lists the failures by reason, load times and memory, then the program ends
// (exit code 5 if anything failed).

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "screens/Screen.h"
#include "sr/gfx/AssetStore.h"

namespace sr::client {

class AssetCheckScreen final : public Screen {
public:
    const char* Name() const override { return "assetcheck"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void Finish(ScreenContext& context);

    struct Listing {
        std::atomic<bool> ready{false};
        std::vector<std::string> paths;
    };
    std::shared_ptr<Listing> listing_;
    std::vector<std::string> types_;
    std::size_t window_ = 256; // requests in flight
    std::size_t next_ = 0;
    struct InFlight {
        gfx::AssetId id;
        std::size_t path = 0;
    };
    std::vector<InFlight> inFlight_;
    std::size_t resident_ = 0, failed_ = 0;
    std::map<std::string, std::vector<std::string>> failures_; // reason -> paths
    std::vector<double> loadMs_;
    std::uint64_t peakBytes_ = 0;
    double started_ = 0;
    double delay_ = 0;          // --asset-delay: plain frames before the first request
    double streamStart_ = -1;   // when the first request went out
    gfx::AssetId shown_; // the newest resident model, drawn while the check runs
    OrbitCamera camera_;
    bool done_ = false;
};

} // namespace sr::client
