#include "scene_view.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/assets/test_patterns.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/render/texture_defaults.hpp>

namespace rv::app {
namespace {

/// Drawing pass of an alpha mode: opaque first, then alpha-tested, then blended.
int alphaPass(assets::AlphaMode mode) noexcept {
    switch (mode) {
    case assets::AlphaMode::Opaque:
        return 0;
    case assets::AlphaMode::Mask:
        return 1;
    case assets::AlphaMode::Blend:
        return 2;
    }
    return 0;
}

/// Texture of one material slot named by the material itself (an asset file), or null when the slot has none.
const gl::Texture2D* materialTexture(const assets::TextureRef& reference, gl::ColorSpace colorSpace,
                                     const assets::MaterialDesc& material, gl::TextureCache& textures,
                                     const gl::TextureDefaults& defaults) {
    if (reference.empty()) {
        return nullptr;
    }
    if (!reference.path.empty()) {
        return &textures.file(reference.path, colorSpace);
    }
    // Image indices refer to the images of a glTF model; scene parts carry library and inline materials only.
    spdlog::warn("Material '{}' refers to model image {}, which this scene does not provide; drawn as a checker",
                 material.name, *reference.imageIndex);
    return &defaults.checker();
}

/// The textures `material` names, by slot (sRGB color, linear data).
gl::MaterialTextures materialTextures(const assets::MaterialDesc& material, gl::TextureCache& textures,
                                      const gl::TextureDefaults& defaults) {
    gl::MaterialTextures slots{};
    const auto set = [&](gl::MaterialTextureSlot slot, const assets::TextureRef& reference, gl::ColorSpace space) {
        slots[gl::toIndex(slot)] = materialTexture(reference, space, material, textures, defaults);
    };
    set(gl::MaterialTextureSlot::BaseColor, material.baseColorTexture, gl::ColorSpace::Srgb);
    set(gl::MaterialTextureSlot::MetallicRoughness, material.metallicRoughnessTexture, gl::ColorSpace::Linear);
    set(gl::MaterialTextureSlot::Normal, material.normalTexture, gl::ColorSpace::Linear);
    set(gl::MaterialTextureSlot::Occlusion, material.occlusionTexture, gl::ColorSpace::Linear);
    set(gl::MaterialTextureSlot::Emissive, material.emissiveTexture, gl::ColorSpace::Srgb);
    return slots;
}

/// Base color texture of a part from its scene texture source: an asset file, or a pattern generated at the aspect
/// ratio of the part's surface so that covers and test cards are not stretched.
const gl::Texture2D& sceneTexture(const scene::TextureSource& source, const geometry::MeshData& surface,
                                  gl::TextureCache& textures) {
    if (source.kind == scene::TextureSource::Kind::File) {
        return textures.file(source.path, gl::ColorSpace::Srgb);
    }
    const glm::vec2 metersPerUnit = geometry::uvMetersPerUnit(surface);
    const float aspect = metersPerUnit.y > 0.0f ? metersPerUnit.x / metersPerUnit.y : 1.0f;
    return textures.pattern(source.pattern, source.title, source.seed, assets::patternSizeForAspect(aspect),
                            gl::ColorSpace::Srgb);
}

void logLights(const scene::SceneDescription& scene, const gl::PackedLights& packed) {
    for (const gl::DroppedLight& dropped : packed.dropped) {
        spdlog::warn("Scene '{}': light '{}' is not drawn: {}", scene.name, dropped.name, dropped.reason);
    }
    const gl::LightsUniforms& block = packed.uniforms;
    spdlog::info("Scene lights: {} point/spot, {} directional; ambient intensity {:g}, exposure {:g}",
                 block.punctualLightCount, block.directionalIntensity > 0.0f ? 1 : 0, block.ambientIntensity,
                 block.exposure);
}

} // namespace

Expected<SceneView> SceneView::create(const scene::SceneDescription& scene, const scene::SceneGeometry& geometry,
                                      gl::TextureCache& textures, const gl::TextureDefaults& defaults,
                                      const gl::GLCapabilities& caps) {
    SceneView view;
    view.m_camera.setLens(scene.camera.lens);
    view.m_camera.setPose(scene.camera.position, core::degreesToRadians(scene.camera.yawDeg),
                          core::degreesToRadians(scene.camera.pitchDeg));
    view.m_clearColor = glm::vec4(scene.environment.clearColor, 1.0f);

    auto materials = gl::MaterialSet::create(defaults, textures.anisotropy());
    if (!materials) {
        return propagate(materials.error());
    }
    view.m_materials = std::move(*materials);

    // One material per name; one instance per object and role with a scene texture of its own.
    std::map<std::string, scene::MaterialHandle, std::less<>> materialByKey;
    std::size_t texturedInstances = 0;
    for (const scene::SceneObjectGeometry& object : geometry.objects) {
        for (const scene::ScenePart& part : object.parts) {
            auto mesh = gl::GpuMesh::create(part.mesh, object.id + ":" + part.mesh.name);
            if (!mesh) {
                spdlog::error("Scene object '{}': part '{}' cannot be uploaded and is not drawn: {}", object.id,
                              part.role, mesh.error().toString());
                continue;
            }
            const std::string key = part.texture ? object.id + "/" + part.role : part.material.name;
            auto found = materialByKey.find(key);
            if (found == materialByKey.end()) {
                gl::MaterialTextures slots = materialTextures(part.material, textures, defaults);
                std::string name = part.material.name;
                if (part.texture) {
                    slots[gl::toIndex(gl::MaterialTextureSlot::BaseColor)] =
                        &sceneTexture(*part.texture, part.mesh, textures);
                    name = fmt::format("{} ({})", key, part.material.name);
                    ++texturedInstances;
                }
                found = materialByKey.emplace(key, view.m_materials.add(std::move(name), part.material, slots)).first;
            }
            const std::size_t meshIndex = view.m_meshes.size();
            view.m_meshes.push_back(std::move(*mesh));
            for (const glm::mat4& instance : object.instances) {
                view.m_draws.push_back(DrawItem{meshIndex, found->second, instance});
            }
        }
    }
    if (view.m_materials.size() > 0) {
        if (auto uploaded = view.m_materials.upload(caps.uniformBufferOffsetAlignment, "MaterialUniforms");
            !uploaded) {
            return propagate(uploaded.error());
        }
    }

    // Opaque, then masked (each by shader, material and mesh), then blended in scene order.
    const gl::MaterialSet& set = view.m_materials;
    std::stable_sort(view.m_draws.begin(), view.m_draws.end(), [&set](const DrawItem& a, const DrawItem& b) {
        const gl::GpuMaterial& left = set.material(a.material);
        const gl::GpuMaterial& right = set.material(b.material);
        const int leftPass = alphaPass(left.alphaMode);
        const int rightPass = alphaPass(right.alphaMode);
        if (leftPass != rightPass || leftPass == alphaPass(assets::AlphaMode::Blend)) {
            return leftPass < rightPass;
        }
        return std::tuple(left.shader, a.material, a.mesh) < std::tuple(right.shader, b.material, b.mesh);
    });

    const gl::PackedLights lights = gl::packLights(scene.lights, scene.environment);
    logLights(scene, lights);
    auto lightsBuffer = gl::UniformBuffer<gl::LightsUniforms>::create("LightsUniforms");
    if (!lightsBuffer) {
        return propagate(lightsBuffer.error());
    }
    if (auto updated = lightsBuffer->update(lights.uniforms); !updated) {
        return propagate(updated.error());
    }
    view.m_lights = std::move(*lightsBuffer);

    spdlog::info("Scene materials: {} ({} textured instance(s)); textures: {} ({:.1f} MiB), {} unavailable",
                 view.m_materials.size(), texturedInstances, textures.textureCount(),
                 static_cast<double>(textures.estimatedBytes()) / (1024.0 * 1024.0), textures.failedCount());
    return view;
}

void SceneView::render(gl::GLDevice& device, const SceneShaders& shaders,
                       gl::UniformBuffer<gl::FrameUniforms>& frameUniforms, const gl::Framebuffer& target,
                       float timeSeconds) {
    if (!target.valid()) {
        return;
    }
    const gl::ScopedDebugGroup group(device, "SceneView");
    device.beginPass(target, m_clearColor);

    const glm::ivec2 size = target.size();
    m_camera.setAspect(static_cast<float>(size.x) / static_cast<float>(size.y));
    gl::FrameUniforms frame;
    frame.view = m_camera.view();
    frame.projection = m_camera.projection();
    frame.viewProjection = frame.projection * frame.view;
    frame.cameraPosition = m_camera.position();
    frame.time = timeSeconds;
    if (auto updated = frameUniforms.update(frame); !updated) {
        spdlog::error("FrameUniforms update failed: {}", updated.error().toString());
        return;
    }
    frameUniforms.bind(gl::UniformBinding::Frame);
    m_lights.bind(gl::UniformBinding::Lights);

    gl::RenderStateCache& state = device.state();
    state.setDepthTest(true);
    state.setDepthFunc(GL_LESS);
    state.setPolygonMode(GL_FILL);
    state.setCullMode(GL_BACK);
    state.setBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const gl::ShaderProgram* currentProgram = nullptr;
    std::uint32_t currentMaterial = UINT32_MAX;
    for (const DrawItem& draw : m_draws) {
        const gl::GpuMaterial& material = m_materials.material(draw.material);
        const gl::ShaderProgram* program = material.shader == scene::ShaderKind::Unlit ? shaders.unlit : shaders.pbr;
        if (program == nullptr || !program->valid()) {
            continue;
        }
        if (program != currentProgram) {
            program->use();
            currentProgram = program;
        }
        if (draw.material.value != currentMaterial) {
            m_materials.bind(draw.material);
            const bool blend = material.alphaMode == assets::AlphaMode::Blend;
            state.setBlend(blend);
            state.setDepthWrite(!blend);
            state.setCullFace(!material.doubleSided);
            currentMaterial = draw.material.value;
        }
        program->setMat4("uModel", draw.model);
        m_meshes[draw.mesh].draw();
    }
    // Leave the default fixed-function state for whatever draws next (overlays, the next pass).
    state.setBlend(false);
    state.setDepthWrite(true);
    state.setCullFace(true);
}

} // namespace rv::app
