// The ways round walls, buildings and a dungeon's rooms (PlayScreen.h, Route): the scripted walkers' (the quests' tests,
// the hunt), and a player's click when the Option window's Beta page asks for Pathfinding (PlayScreen::WalkTo). Ours: by
// default a player's click walks straight, as the original's does.

#include <atomic>
#include <chrono>
#include <cmath>

#include "PlayScreen.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

struct PlayScreen::RoutePlan {
    game::NavWorld nav; // a copy: the game thread streams regions in and out meanwhile (they are shared, never changed)
    game::NavPosition from;
    proto::WorldPosition to;
    game::NavPathOptions options;
    bool best = false; // the best of several searches (a player's click)
    std::atomic<bool> done{false};
    bool found = false;
    std::vector<game::NavPosition> path;
    game::NavPathStats stats;
    double ms = 0;
};

namespace {

double FlatDistance(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const proto::FlatPosition p = proto::ToFlat(a), q = proto::ToFlat(b);
    return std::hypot(p.x - q.x, p.z - q.z);
}

constexpr double kLegReached = 10.0; // a leg's end this near: the next leg
constexpr int kLegTries = 3;         // a leg asked for this often without getting there: stuck
constexpr int kSearches = 4;         // the way searched at most this often (again after each place it stuck)
constexpr float kCornerClearance = 15.0f; // a way's legs, and its corners cut, keep this far off what stands beside them
constexpr float kWallCost = 0.5f;         // its search's cost of a step beside a wall (game::NavPathOptions::wallCost)
constexpr float kStuckAhead = 40.0f; // the place it could not pass: this far ahead of where it stands,
constexpr float kStuckRadius = 60.0f; // this wide

} // namespace

bool PlayScreen::RouteTo(const proto::WorldPosition& to, std::uint32_t target, float arrive, bool click) {
    route_ = {};
    route_.target = target;
    route_.goal = to;
    route_.arrive = arrive;
    route_.click = click;
    return RouteSearch();
}

bool PlayScreen::RouteSearch() {
    route_.legs.clear();
    route_.next = 0;
    route_.tries = 0;
    route_.failed = false;
    ++route_.searches;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr || world_ == nullptr || replay_ || !shared_ || !shared_->session) return false;
    const game::NavWorld& nav = world_->Navigation();
    game::NavPosition from = self->nav;
    if (!from.Valid()) from = nav.Drop(self->position.region, self->position.x, self->position.y, self->position.z);
    const proto::WorldPosition& to = route_.goal;
    // A straight walk there (and nowhere it stuck before): the click alone. With `arrive`, one stopped that near the
    // goal does too (at a counter): the leg is where it stops.
    game::NavPosition end;
    const std::uint32_t walked = from.Valid() ? nav.Walk(from, to.region, to.x, to.z, end) : game::nav_walk::kFailed;
    const proto::WorldPosition stop{end.region, end.x, end.y, end.z};
    const bool whole = (walked & (game::nav_walk::kBlocked | game::nav_walk::kFailed | game::nav_walk::kLimit)) == 0 && FlatDistance(stop, to) < kLegReached;
    const bool near = (walked & (game::nav_walk::kFailed | game::nav_walk::kLimit)) == 0 && route_.arrive > 0.0f && FlatDistance(stop, to) <= route_.arrive;
    if (!from.Valid() || (route_.avoid.empty() && (whole || near))) {
        route_.legs = {whole || !from.Valid() ? to : stop};
        route_.legAt = -1e9; // sent by the next frame
        return false;
    }
    auto plan = std::make_shared<RoutePlan>();
    plan->nav = nav;
    plan->from = from;
    plan->to = to;
    plan->options.avoid = route_.avoid;
    plan->options.arrive = route_.arrive;
    plan->best = route_.click;
    plan->options.closest = route_.click; // a click where no way leads: as near as the way goes
    // Its legs keep off the corners and its way off the walls (a click's and a script's walk-to alike).
    plan->options.clearance = kCornerClearance;
    plan->options.wallCost = kWallCost;
    route_.plan = plan;
    core::JobSystem::Get().Run([plan] {
        const auto start = std::chrono::steady_clock::now();
        plan->found = plan->best ? game::FindBestNavPath(plan->nav, plan->from, plan->to.region, plan->to.x, plan->to.z, plan->path, plan->options,
                                                         &plan->stats, &core::JobSystem::Get())
                                 : game::FindNavPath(plan->nav, plan->from, plan->to.region, plan->to.x, plan->to.z, plan->path, plan->options, &plan->stats);
        plan->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        plan->done.store(true, std::memory_order_release);
    });
    return true;
}

void PlayScreen::RouteFrame(ScreenContext& context) {
    if (route_.plan != nullptr && route_.plan->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<RoutePlan> plan = std::move(route_.plan);
        route_.plan = nullptr;
        core::LogInfo("game: route: {} to {}, {:.0f}, {:.0f}: {} legs ({:.0f} ms, {} points, {} walks, {} places avoided)",
                      !plan->found ? "no way" : plan->stats.partial ? "the nearest way" : "a way", plan->to.region, plan->to.x, plan->to.z,
                      plan->path.size(), plan->ms,
                      plan->stats.expanded, plan->stats.walks, plan->options.avoid.size());
        if (!plan->found) {
            route_.failed = true;
            // A player's click finds no way (walled in, too far): it walks straight there all the same, as Classic.
            if (route_.click && shared_ && shared_->session) shared_->session->MoveTo(plan->to);
            return;
        }
        for (const game::NavPosition& p : plan->path) route_.legs.push_back({p.region, p.x, p.y, p.z});
        route_.next = 0;
        route_.legAt = -1e9;
    }
    if (route_.next >= route_.legs.size() || !shared_ || !shared_->session) return;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr) return;
    const proto::WorldPosition& leg = route_.legs[route_.next];
    const double distance = FlatDistance(self->position, leg);
    if (distance < kLegReached) {
        // Still walking, short of the leg's end: the next leg only when a clear straight walk reaches it from here; else
        // the leg's end first (a corner's: the next leg sent from before it clipped the corner and the server's walk
        // stopped there, the user's report 2026-11-14).
        if (self->moving && distance > 1.0 && route_.next + 1 < route_.legs.size() && world_ && self->nav.Valid()) {
            const proto::WorldPosition& after = route_.legs[route_.next + 1];
            if (!game::NavWalkClear(world_->Navigation(), self->nav, after.region, after.x, after.z, kCornerClearance)) return;
        }
        ++route_.next;
        route_.tries = 0;
        route_.legAt = -1e9;
        if (route_.click && route_.next == route_.legs.size()) core::LogInfo("game: route: the click's way walked ({} legs)", route_.legs.size());
        return;
    }
    // A player's way cuts its corners: while on a leg, a straight walk from where it is that reaches the leg after the
    // next one's end (on the client's navigation, as the server walks it) goes there at once, a few times a second.
    if (route_.click && route_.next + 1 < route_.legs.size() && self->moving && self->nav.Valid() && world_ && context.time - route_.cutAt > 0.2) {
        route_.cutAt = context.time;
        const proto::WorldPosition& after = route_.legs[route_.next + 1];
        game::NavPosition end;
        const std::uint32_t walked = world_->Navigation().Walk(self->nav, after.region, after.x, after.z, end);
        if ((walked & (game::nav_walk::kBlocked | game::nav_walk::kFailed | game::nav_walk::kLimit)) == 0 &&
            FlatDistance({end.region, end.x, end.y, end.z}, after) < 1.0 &&
            game::NavWalkClear(world_->Navigation(), self->nav, after.region, after.x, after.z, kCornerClearance)) {
            ++route_.next;
            route_.tries = 0;
            shared_->session->MoveTo(after);
            route_.legAt = context.time;
            return;
        }
    }
    // A leg asked for again when the walker stands short of it (the server's walk stopped it); after a few, the place
    // ahead of it is something the client's navigation does not know (a door shut in the instance): kept out of, the
    // way searched again, until it has been searched often enough.
    const bool standing = !self->moving;
    if (route_.legAt < 0.0 || (standing && context.time - route_.legAt > 1.5)) {
        if (route_.tries++ >= kLegTries) {
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(leg);
            const double length = std::max(1.0, std::hypot(b.x - a.x, b.z - a.z));
            const proto::WorldPosition ahead = proto::FromFlat({a.x + (b.x - a.x) / length * kStuckAhead, a.z + (b.z - a.z) / length * kStuckAhead}, 0.0f);
            core::LogInfo("game: route: stuck {:.0f} short of leg {} of {}", distance, route_.next + 1, route_.legs.size());
            if (route_.searches >= kSearches) {
                route_.legs.clear();
                route_.next = 0;
                route_.failed = true;
                return;
            }
            route_.avoid.push_back({ahead.region, ahead.x, ahead.z, kStuckRadius});
            RouteSearch();
            return;
        }
        shared_->session->MoveTo(leg);
        route_.legAt = context.time;
    }
}

} // namespace sr::client
