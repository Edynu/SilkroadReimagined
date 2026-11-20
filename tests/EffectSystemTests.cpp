// The effect system's programs and simulation (gfx/Effects.h): a
// made-up program run tick by tick, then every effect of the game data compiled and run.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#include "sr/assets/FileSystem.h"
#include "sr/core/Path.h"
#include "sr/gfx/Effects.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace sr;

namespace {

assets::EffectCommand Command(const std::string& name, std::vector<float> values = {}) {
    assets::EffectCommand c;
    c.name = name;
    c.values = std::move(values);
    return c;
}

// A root of `frames` frames emitting one plate child a tick (two at a time) for 5 ticks; each child
// lives 10 frames, flies up and fades.
assets::EffectData Fountain() {
    assets::EffectData data;
    data.version = 11;
    assets::EffectNode& root = data.root;
    root.frames = 20;
    root.life.name = "NormalTimeLife";
    assets::EffectNode child;
    child.frames = 10;
    child.life.name = "NormalTimeLife";
    child.emitterProgram.push_back(Command("StaticEmit", {0, 5, 1, 100, 2}));
    child.renderProgram.push_back(Command("SetVelocity", {0, 1, 0}));
    assets::EffectCommand colours = Command("SetGraphDiffuse");
    for (int i = 0; i < 10; ++i) colours.colors.push_back({255, 128, 0, static_cast<std::uint8_t>(255 - i * 20)});
    colours.rows = 10;
    child.renderProgram.push_back(colours);
    child.render.name = "RenderPlate";
    child.view.name = "ViewBillboard";
    child.resource.srcBlend = 5;
    child.resource.dstBlend = 2; // additive
    child.resource.meshes.push_back({"none", {"particles\\spark.ddj"}});
    root.children.push_back(child);
    return data;
}

} // namespace

TEST_CASE("effect blends from the resource's D3D blend states") {
    CHECK(gfx::EffectBlendOf(5, 6) == gfx::EffectBlend::Alpha);
    CHECK(gfx::EffectBlendOf(2, 6) == gfx::EffectBlend::Premultiplied);
    CHECK(gfx::EffectBlendOf(5, 2) == gfx::EffectBlend::Additive);
    CHECK(gfx::EffectBlendOf(2, 2) == gfx::EffectBlend::Additive);
}

TEST_CASE("an effect program compiles and runs by its StaticEmit, frames and tracks") {
    gfx::EffectProgram program;
    std::string error;
    REQUIRE_MESSAGE(gfx::CompileEffect(Fountain(), program, error), error);
    REQUIRE(program.nodes.size() == 2);
    CHECK(program.nodes[1].parent == 0);
    CHECK(program.nodes[1].emitter);
    CHECK(program.nodes[1].render == gfx::EffectProgram::Render::Plate);
    CHECK(program.nodes[1].blend == gfx::EffectBlend::Additive);
    CHECK(program.textures == std::vector<std::string>{"particles\\spark.ddj"});
    CHECK(program.nodes[1].colours.size() == 10);
    // The root's 20 frames, or its last child (born at 4) living 10.
    CHECK(program.lengthTicks == 20);

    gfx::EffectSimulation sim;
    sim.Start(program, 7, false);
    // Tick 0: the root, and its first two children (due at its age 0).
    CHECK(sim.Alive() == 3);
    for (int i = 0; i < 4; ++i) sim.Advance(gfx::EffectSimulation::kTick);
    CHECK(sim.Alive() == 1 + 10); // two a tick over ticks 0..4
    for (int i = 0; i < 6; ++i) sim.Advance(gfx::EffectSimulation::kTick);
    CHECK(sim.Alive() == 1 + 8); // the first two died at age 10

    // What it shows: additive plates once their texture is in, and a light.
    gfx::EffectSimulation::Look look;
    look.textures = {42};
    look.textureFlags = {0};
    gfx::EffectSimulation::Output out;
    const glm::mat3 camera(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1));
    sim.Build(glm::mat4(1.0f), 1.0f, look, camera, 1000, out);
    CHECK(out.sprites[2].empty()); // not in yet
    look.textureFlags = {gfx::EffectSimulation::Look::kReady};
    sim.Build(glm::mat4(1.0f), 1.0f, look, camera, 1000, out);
    REQUIRE(out.sprites[2].size() == 8);
    CHECK(out.sprites[0].empty());
    CHECK(out.sprites[2][0].texture == 42);
    CHECK(out.sprites[2][0].centre.y > 0.0f); // flown up
    CHECK(out.lit);
    CHECK(out.light.colour.r == doctest::Approx(1.0f));

    // It ends once its root's frames and the last children are done.
    for (int i = 0; i < 40 && !sim.Done(); ++i) sim.Advance(gfx::EffectSimulation::kTick);
    CHECK(sim.Done());
    CHECK(sim.Ticks() == 20);
}

TEST_CASE("a kept effect starts its fixed pieces over; a released one finishes") {
    gfx::EffectProgram program;
    std::string error;
    REQUIRE(gfx::CompileEffect(Fountain(), program, error));
    gfx::EffectSimulation sim;
    sim.Start(program, 1, true);
    for (int i = 0; i < 100; ++i) sim.Advance(gfx::EffectSimulation::kTick);
    CHECK_FALSE(sim.Done());
    CHECK(sim.Alive() >= 1);
    sim.Release();
    for (int i = 0; i < 100 && !sim.Done(); ++i) sim.Advance(gfx::EffectSimulation::kTick);
    CHECK(sim.Done());
}

TEST_CASE("a weapon's kept glow holds steady: it does not pile up over a minute (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr || *dir == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    for (const char* path : {"system\\system_rarebow_c.efp", "system\\system_raretype_a_step1.efp", "system\\system_raretype_b_step1.efp",
                             "system\\system_raretype_c_step1.efp"}) {
        const auto file = fs.Read(path);
        if (!file) {
            MESSAGE(path << ": not in the game files");
            continue;
        }
        assets::EffectData data;
        REQUIRE_MESSAGE(assets::DecodeEfp(file->Bytes(), data, error), error);
        gfx::EffectProgram program;
        REQUIRE_MESSAGE(gfx::CompileEffect(data, program, error), error);
        gfx::EffectSimulation sim;
        sim.Start(program, 1, true);
        std::string counts;
        std::size_t at10 = 0, most = 0;
        for (int s = 1; s <= 60; ++s) {
            for (int i = 0; i < 20; ++i) sim.Advance(gfx::EffectSimulation::kTick);
            if (s == 10) at10 = sim.Alive();
            if (s > 10) most = std::max(most, sim.Alive());
            if (s % 5 == 0) counts += " " + std::to_string(sim.Alive());
        }
        MESSAGE(path << ": alive every 5 s:" << counts);
        // After its first rounds it only replaces what dies (some room for the emitters' rounds).
        CHECK(most <= at10 + at10 / 2 + 8);
    }
}

TEST_CASE("every effect compiles and runs within its caps") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr || *dir == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    int compiled = 0, oldFormat = 0, failed = 0;
    std::size_t mostElements = 0;
    std::string busiest;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> blends;
    for (const std::string& path : fs.ListFiles()) {
        if (!core::NormalizeAssetPath(path).ends_with(".efp")) continue;
        const auto file = fs.Read(path);
        REQUIRE(file.has_value());
        assets::EffectData data;
        if (!assets::DecodeEfp(file->Bytes(), data, error)) {
            ++(data.version == 0 ? oldFormat : failed); // JMXVEFF 0000: not decoded (Effect.cpp)
            continue;
        }
        gfx::EffectProgram program;
        if (!gfx::CompileEffect(data, program, error)) {
            if (++failed <= 5) MESSAGE(path << ": " << error);
            continue;
        }
        ++compiled;
        const auto count = [&](const assets::EffectNode& n, auto& self) -> void {
            ++blends[{n.resource.srcBlend, n.resource.dstBlend}];
            for (const auto& c : n.children) self(c, self);
        };
        count(data.root, count);
        // Three seconds, kept (the most it can grow).
        gfx::EffectSimulation sim;
        sim.Start(program, 1, true);
        for (int i = 0; i < 60; ++i) sim.Advance(gfx::EffectSimulation::kTick);
        CHECK(sim.Alive() <= gfx::EffectSimulation::kMaxElements);
        if (sim.Alive() > mostElements) {
            mostElements = sim.Alive();
            busiest = path;
        }
    }
    MESSAGE(compiled << " effects compile and run, " << oldFormat << " in the old JMXVEFF 0000 layout, " << failed << " fail; busiest "
                     << busiest << " with " << mostElements << " elements after 3 s");
    for (const auto& [blend, n] : blends) MESSAGE("blend " << blend.first << " / " << blend.second << ": " << n << " nodes");
    CHECK(compiled > 1600);
    CHECK(failed == 0);
}

TEST_CASE("every ribbon effect strings its elements into strips (game files)") {
    // RenderLinkPipe / LinkDPipe / each effect with a ribbon node, run a second,
    // gives triangles (six vertices a segment) of finite places, their u across 0..1, once its textures are in.
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr || *dir == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    int withRibbons = 0, drawn = 0;
    std::size_t mostVertices = 0;
    const glm::mat3 camera(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1));
    for (const std::string& path : fs.ListFiles()) {
        if (!core::NormalizeAssetPath(path).ends_with(".efp")) continue;
        const auto file = fs.Read(path);
        assets::EffectData data;
        if (!file || !assets::DecodeEfp(file->Bytes(), data, error)) continue;
        gfx::EffectProgram program;
        if (!gfx::CompileEffect(data, program, error)) continue;
        const bool ribbon = std::any_of(program.nodes.begin(), program.nodes.end(), [](const gfx::EffectProgram::Node& n) {
            return n.render == gfx::EffectProgram::Render::LinkPipe || n.render == gfx::EffectProgram::Render::LinkDPipe ||
                   n.render == gfx::EffectProgram::Render::LinkObj;
        });
        if (!ribbon) continue;
        ++withRibbons;
        gfx::EffectSimulation::Look look;
        look.textures.assign(program.textures.size(), 7);
        look.textureFlags.assign(program.textures.size(), gfx::EffectSimulation::Look::kReady);
        look.meshReady.assign(program.meshes.size(), 1);
        gfx::EffectSimulation sim;
        sim.Start(program, 3, false);
        // Moving, so a trail is left: a unit a tick along x.
        std::size_t vertices = 0;
        for (int i = 0; i < 20; ++i) {
            sim.Place(glm::translate(glm::mat4(1.0f), glm::vec3(float(i), 0.0f, 0.0f)));
            sim.Advance(gfx::EffectSimulation::kTick);
            gfx::EffectSimulation::Output out;
            sim.Build(glm::translate(glm::mat4(1.0f), glm::vec3(float(i), 0.0f, 0.0f)), 1.0f, look, camera, 4096, out);
            for (const auto& list : out.ribbons) {
                CHECK(list.size() % 6 == 0);
                for (const gfx::EffectRibbonVertex& v : list) {
                    CHECK(std::isfinite(v.position.x));
                    CHECK(std::isfinite(v.position.y));
                    CHECK(std::isfinite(v.position.z));
                    CHECK(v.uv.x >= -1e-4f);
                    CHECK(v.uv.x <= 1.0001f);
                }
                vertices += list.size();
            }
        }
        if (vertices > 0) ++drawn;
        mostVertices = std::max(mostVertices, vertices);
    }
    MESSAGE(withRibbons << " effects have ribbons, " << drawn << " drew strips in their first second; most vertices " << mostVertices);
    CHECK(withRibbons > 200);
    CHECK(drawn * 10 >= withRibbons * 8); // (a few start theirs later than a second)
}
