#include "sr/gfx/FrameConstants.h"

#include <algorithm>
#include <cmath>

namespace sr::gfx {

namespace {
glm::vec3 Direction(const glm::vec3& v) {
    const float length = glm::length(v);
    return length > 0 ? v / length : glm::vec3(0, 1, 0);
}
} // namespace

FrameConstants MakeFrameConstants(const glm::mat4& viewProj, const glm::vec3& camera, const SceneLighting& lighting, double seconds,
                                  std::size_t pointLights) {
    FrameConstants f;
    f.viewProj = viewProj;
    f.camera = glm::vec4(camera, lighting.timeOfDay);
    f.terrainLight = glm::vec4(Direction(lighting.terrainLight), 0.0f);
    f.objectLight = glm::vec4(Direction(lighting.objectLight), 0.0f);
    f.diffuse = glm::vec4(lighting.diffuse, 1.0f);
    f.terrainAmbient = glm::vec4(lighting.terrainAmbient, 1.0f);
    f.objectAmbient = glm::vec4(lighting.objectAmbient, 1.0f);
    f.lift = glm::vec4(lighting.lift, lighting.lightmaps ? 1.0f : 0.0f);
    const bool fog = lighting.fog && lighting.fogEnd > lighting.fogStart;
    f.fogColor = glm::vec4(lighting.fogColor, fog ? 1.0f : 0.0f);
    f.terrainFogColor = glm::vec4(lighting.terrainFogColor, fog ? 1.0f : 0.0f);
    f.fogRange = glm::vec4(lighting.fogStart, lighting.fogEnd, fog ? 1.0f / (lighting.fogEnd - lighting.fogStart) : 0.0f, 0.0f);
    f.time = glm::vec4(static_cast<float>(std::fmod(seconds, 100000.0)), 0.0f, 0.0f, 0.0f);
    // The original passes it as a texture factor: each channel x 255, truncated.
    const glm::vec3 water = glm::floor(glm::clamp(lighting.waterColor, 0.0f, 1.0f) * 255.0f) / 255.0f;
    f.water = glm::vec4(water, 1.0f);
    f.objects = glm::vec4(lighting.objectFog == SceneLighting::ObjectFog::SquaredExponential ? 1.0f : 0.0f, lighting.fogDensity, 0.0f,
                          0.0f);
    f.lights = glm::vec4(static_cast<float>(pointLights), 0.0f, 0.0f, 0.0f);
    return f;
}

PointLightGpu MakePointLight(const PointLight& light) {
    return {glm::vec4(light.position, 1.0f), glm::vec4(light.diffuse, 1.0f), glm::vec4(light.ambient, 1.0f),
            glm::vec4(light.attenuation, 0.0f)};
}

} // namespace sr::gfx
