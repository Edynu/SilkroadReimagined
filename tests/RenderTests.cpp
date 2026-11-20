#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include "sr/render/Frustum.h"

using sr::render::Frustum;

TEST_CASE("frustum culls spheres and boxes outside a left-handed 0..1 camera") {
    // Looking down +Z (left-handed) from the origin, 90 degrees, near 1, far 100.
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 1.0f, 100.0f);
    const Frustum f(proj * view);

    CHECK(f.Sphere({0.0f, 0.0f, 50.0f}, 1.0f));
    CHECK_FALSE(f.Sphere({0.0f, 0.0f, -50.0f}, 1.0f));   // behind
    CHECK_FALSE(f.Sphere({0.0f, 0.0f, 150.0f}, 1.0f));   // past the far plane
    CHECK(f.Sphere({0.0f, 0.0f, 100.5f}, 1.0f));         // touching the far plane
    CHECK_FALSE(f.Sphere({60.0f, 0.0f, 50.0f}, 5.0f));   // right of the 45 degree side
    CHECK(f.Sphere({52.0f, 0.0f, 50.0f}, 5.0f));         // crossing it
    CHECK_FALSE(f.Sphere({0.0f, 70.0f, 50.0f}, 5.0f));   // above
    CHECK_FALSE(f.Sphere({0.0f, 0.0f, 0.2f}, 0.5f));     // before the near plane

    CHECK(f.Box({-1.0f, -1.0f, 10.0f}, {1.0f, 1.0f, 12.0f}));
    CHECK(f.Box({-500.0f, -1.0f, 10.0f}, {500.0f, 1.0f, 12.0f})); // wider than the view
    CHECK_FALSE(f.Box({20.0f, -1.0f, 10.0f}, {30.0f, 1.0f, 12.0f}));
    CHECK_FALSE(f.Box({-1.0f, -1.0f, -12.0f}, {1.0f, 1.0f, -10.0f}));

    const Frustum all; // default: nothing culled
    CHECK(all.Sphere({0.0f, 0.0f, -1000.0f}, 0.0f));
    CHECK(all.Box({1e6f, 1e6f, 1e6f}, {1e6f + 1.0f, 1e6f + 1.0f, 1e6f + 1.0f}));
}

#include "sr/render/ShadowCascades.h"

TEST_CASE("shadow cascades cover the view, nearest first, each in its atlas quarter") {
    const glm::vec3 eye(100.0f, 50.0f, -200.0f);
    const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.3f, -0.2f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 1.0f, 6000.0f);
    const glm::vec3 toLight = glm::normalize(glm::vec3(0.70710678f, 0.70710678f, 0.0f));
    const auto c = sr::render::BuildShadowCascades(proj * view, eye, toLight, 3, 1500.0f, 4096);
    REQUIRE(c.count == 3);
    CHECK(c.splits[0] < c.splits[1]);
    CHECK(c.splits[1] < c.splits[2]);
    CHECK(c.splits[2] == doctest::Approx(1500.0f).epsilon(0.01));
    CHECK(c.texels[0] < c.texels[2]);
    // A point in front of the camera, inside the first slice, lands in the first quarter.
    const glm::vec3 forward = glm::normalize(glm::vec3(0.3f, -0.2f, 1.0f));
    const glm::vec3 p = eye + forward * (c.splits[0] * 0.5f);
    const glm::vec4 a = c.atlas[0] * glm::vec4(p, 1.0f);
    CHECK(a.x > 0.0f);
    CHECK(a.x < 0.5f);
    CHECK(a.y > 0.0f);
    CHECK(a.y < 0.5f);
    CHECK(a.z > 0.0f);
    CHECK(a.z < 1.0f);
    // The same point in the third cascade sits in the bottom-left quarter.
    const glm::vec4 b = c.atlas[2] * glm::vec4(p, 1.0f);
    CHECK(b.x < 0.5f);
    CHECK(b.y > 0.5f);
    // Toward the light is nearer (smaller depth).
    const glm::vec4 above = c.atlas[0] * glm::vec4(p + toLight * 10.0f, 1.0f);
    CHECK(above.z < a.z);
    // A small camera move keeps the projection on whole texels (no shimmer).
    const auto moved = sr::render::BuildShadowCascades(proj * glm::translate(view, glm::vec3(-0.01f, 0.0f, 0.0f)), eye + glm::vec3(0.01f, 0, 0),
                                                       toLight, 3, 1500.0f, 4096);
    const glm::vec4 a2 = moved.atlas[0] * glm::vec4(p, 1.0f);
    const float texelUv = 0.5f / 2048.0f;
    const float shift = (a2.x - a.x) / texelUv;
    CHECK(std::abs(shift - std::round(shift)) < 0.01f);
}

#include "sr/render/LightClusters.h"

TEST_CASE("local lights land in the clusters their sphere reaches") {
    using sr::render::LightClusters;
    using sr::render::PointLight;
    const glm::vec3 eye(0.0f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 16.0f / 9.0f, 1.0f, 5000.0f);
    proj[1][1] *= -1.0f; // as the game's camera (Vulkan y down)
    const glm::mat4 vp = proj * view;
    const std::vector<PointLight> lights = {
        {{0.0f, 0.0f, 100.0f}, 20.0f, {1, 0.5f, 0}, 1.0f},  // straight ahead
        {{0.0f, 0.0f, -100.0f}, 20.0f, {1, 1, 1}, 1.0f},    // behind: dropped
        {{0.0f, 0.0f, 9000.0f}, 20.0f, {1, 1, 1}, 1.0f},    // too far: dropped
    };
    LightClusters c;
    c.Build(lights, vp, eye, glm::vec3(0, 0, 1), 64);
    REQUIRE(c.count() == 1);
    CHECK(c.lights[0].z == doctest::Approx(100.0f));
    CHECK(c.lights[1].x == doctest::Approx(1.0f));
    const auto at = [&](int x, int y, int s) {
        const std::size_t i = static_cast<std::size_t>((s * LightClusters::kTilesY + y) * LightClusters::kTilesX + x);
        return c.clusters[i * 2 + 1];
    };
    const int slice = c.Slice(100.0f);
    CHECK(at(LightClusters::kTilesX / 2, LightClusters::kTilesY / 2, slice) == 1); // the middle of the screen
    CHECK(at(0, 0, slice) == 0);                                                   // a corner
    CHECK(at(LightClusters::kTilesX / 2, LightClusters::kTilesY / 2, LightClusters::kSlices - 1) == 0); // far away
    // A light the camera stands in covers the whole screen at its depths.
    const std::vector<PointLight> around = {{{0.0f, 0.0f, 5.0f}, 50.0f, {1, 1, 1}, 1.0f}};
    c.Build(around, vp, eye, glm::vec3(0, 0, 1), 64);
    CHECK(at(0, 0, 0) == 1);
    CHECK(at(LightClusters::kTilesX - 1, LightClusters::kTilesY - 1, 0) == 1);
    // The cap keeps the strongest.
    std::vector<PointLight> many(10, PointLight{{0.0f, 0.0f, 200.0f}, 30.0f, {1, 1, 1}, 0.1f});
    many[7].intensity = 5.0f;
    c.Build(many, vp, eye, glm::vec3(0, 0, 1), 1);
    REQUIRE(c.count() == 1);
    CHECK(c.lights[1].w == doctest::Approx(5.0f));
}
