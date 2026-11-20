// The render graph's plan without a device (C3a): which passes survive and
// which barriers go in front of each.

#include <doctest/doctest.h>

#include <string>

#include "sr/gfx/RenderGraph.h"

using sr::gfx::PassKind;
using sr::gfx::RenderGraph;
using sr::gfx::Use;

namespace {

RenderGraph::TextureDesc Target(std::uint32_t w = 64, std::uint32_t h = 64) {
    RenderGraph::TextureDesc d;
    d.width = w;
    d.height = h;
    return d;
}

const RenderGraph::PlannedPass& Find(const RenderGraph& g, const std::string& name) {
    for (const auto& p : g.Plan()) {
        if (p.name == name) return p;
    }
    FAIL("no pass " << name);
    return g.Plan().front();
}

} // namespace

TEST_CASE("RenderGraph: passes nothing needs are dropped") {
    RenderGraph g;
    g.Reset();
    const auto unused = g.CreateTexture(Target(), "unused");
    const auto scene = g.CreateTexture(Target(), "scene");
    g.AddPass("lonely", PassKind::Graphics, [&](RenderGraph::Builder& b) { b.Write(unused, Use::ColorWrite); }, {});
    g.AddPass("scene", PassKind::Graphics, [&](RenderGraph::Builder& b) { b.Write(scene, Use::ColorWrite); }, {});
    g.AddPass("present", PassKind::Graphics, [&](RenderGraph::Builder& b) { b.Read(scene, Use::Sampled).SideEffect(); }, {});
    g.AddPass("overwritten", PassKind::Graphics, [&](RenderGraph::Builder& b) { b.Write(scene, Use::ColorWrite); }, {});
    g.Compile();
    CHECK(Find(g, "lonely").culled);
    CHECK(Find(g, "overwritten").culled); // after the last reader: nothing reads what it writes
    CHECK_FALSE(Find(g, "scene").culled);
    CHECK_FALSE(Find(g, "present").culled);
}

TEST_CASE("RenderGraph: writing an imported resource keeps the pass") {
    RenderGraph g;
    g.Reset();
    const auto history = g.ImportTexture({}, "history");
    g.AddPass("keep history", PassKind::Graphics, [&](RenderGraph::Builder& b) { b.Write(history, Use::ColorWrite); }, {});
    g.Compile();
    CHECK_FALSE(Find(g, "keep history").culled);
}

TEST_CASE("RenderGraph: buffer barriers follow the declared uses") {
    RenderGraph g;
    g.Reset();
    const auto draws = g.ImportBuffer({}, "draw list");
    const auto target = g.CreateTexture(Target(), "target");
    g.AddPass("clear", PassKind::Transfer, [&](RenderGraph::Builder& b) { b.Write(draws, Use::TransferWrite); }, {});
    g.AddPass("cull", PassKind::Compute, [&](RenderGraph::Builder& b) { b.Write(draws, Use::ComputeWrite); }, {});
    g.AddPass("draw", PassKind::Graphics, [&](RenderGraph::Builder& b) {
        b.Read(draws, Use::IndirectRead).Read(draws, Use::VertexRead).Write(target, Use::ColorWrite);
    }, {});
    g.AddPass("draw again", PassKind::Graphics, [&](RenderGraph::Builder& b) {
        // Draws over the first pass's result: it loads the target (a read) as well as writing it.
        b.Read(draws, Use::IndirectRead).Read(target, Use::ColorWrite).Write(target, Use::ColorWrite).SideEffect();
    }, {});
    g.AddPass("cull again", PassKind::Compute, [&](RenderGraph::Builder& b) { b.Write(draws, Use::ComputeWrite); }, {});
    g.Compile();

    // The work before the frame, then the clear.
    const auto& clear = Find(g, "clear");
    REQUIRE(clear.barriers.size() == 1);
    CHECK(clear.barriers[0].from == Use::External);
    CHECK(clear.barriers[0].to == Use::TransferWrite);
    // Write after write.
    const auto& cull = Find(g, "cull");
    REQUIRE(cull.barriers.size() == 1);
    CHECK(cull.barriers[0].from == Use::TransferWrite);
    CHECK(cull.barriers[0].to == Use::ComputeWrite);
    // Read after write: one barrier per kind of reader.
    const auto& draw = Find(g, "draw");
    REQUIRE(draw.barriers.size() == 2);
    CHECK(draw.barriers[0].from == Use::ComputeWrite);
    CHECK(draw.barriers[0].to == Use::IndirectRead);
    CHECK(draw.barriers[1].to == Use::VertexRead);
    // The same kind of read again needs nothing.
    CHECK(Find(g, "draw again").barriers.empty());
    // Write after the reads: wait for both readers.
    const auto& again = Find(g, "cull again");
    REQUIRE(again.barriers.size() == 2);
    CHECK(again.barriers[0].from == Use::IndirectRead);
    CHECK(again.barriers[1].from == Use::VertexRead);
    CHECK(again.barriers[0].to == Use::ComputeWrite);
}

TEST_CASE("RenderGraph: culled passes place no barriers") {
    RenderGraph g;
    g.Reset();
    const auto list = g.ImportBuffer({}, "list");
    const auto scratch = g.CreateTexture(Target(), "scratch");
    g.AddPass("unused reader", PassKind::Graphics, [&](RenderGraph::Builder& b) {
        b.Read(list, Use::IndirectRead).Write(scratch, Use::ColorWrite);
    }, {});
    g.AddPass("writer", PassKind::Compute, [&](RenderGraph::Builder& b) { b.Write(list, Use::ComputeWrite); }, {});
    g.Compile();
    CHECK(Find(g, "unused reader").culled);
    const auto& writer = Find(g, "writer");
    REQUIRE(writer.barriers.size() == 1);
    CHECK(writer.barriers[0].from == Use::External); // not the culled reader's read
}

TEST_CASE("RenderGraph: transient targets are reused across frames") {
    RenderGraph g;
    for (int frame = 0; frame < 5; ++frame) {
        g.Reset();
        const auto a = g.CreateTexture(Target(1600, 900), "colour");
        const auto b = g.CreateTexture(Target(1600, 900), "colour 2");
        CHECK(a != b);
        g.AddPass("p", PassKind::Graphics, [&](RenderGraph::Builder& builder) {
            builder.Write(a, Use::ColorWrite).Write(b, Use::ColorWrite).SideEffect();
        }, {});
        g.Compile();
    }
    CHECK(g.PooledTextures() == 2); // two of the same description, made once
    g.Reset();
    g.CreateTexture(Target(1280, 720), "resized");
    CHECK(g.PooledTextures() == 3); // a new size makes a new one; the old ones go when unused long enough
}
