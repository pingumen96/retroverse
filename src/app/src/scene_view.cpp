#include "scene_view.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/scene_draw_plan.hpp>
#include <rv/core/math.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/tonemap.hpp>

namespace rv::app {
namespace {

/// Texture of one material slot named by the material itself (an asset file of a tiling surface), or null when the
/// slot has none.
const gl::Texture2D* materialTexture(const assets::TextureRef& reference, gl::ColorSpace colorSpace,
                                     const assets::MaterialDesc& material, gl::TextureCache& textures,
                                     const gl::TextureDefaults& defaults) {
    if (reference.empty()) {
        return nullptr;
    }
    if (!reference.path.empty()) {
        return &textures.file(reference.path, colorSpace, gl::TextureWrap::Repeat);
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

/// Base color texture of a material instance from its picture (PlannedMaterial::picture): an asset file or a pattern
/// generated at the planned size, both clamped to their edges (single pictures).
const gl::Texture2D& pictureTexture(const PlannedMaterial& planned, gl::TextureCache& textures) {
    const scene::TextureSource& source = *planned.picture;
    if (source.kind == scene::TextureSource::Kind::File) {
        return textures.file(source.path, gl::ColorSpace::Srgb, gl::TextureWrap::ClampToEdge);
    }
    return textures.pattern(source.pattern, source.title, source.seed, planned.patternSize, gl::ColorSpace::Srgb);
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
    // The background is scene radiance like any surface: exposure and tonemapping apply before the sRGB encoding.
    view.m_clearColor = glm::vec4(gl::tonemap(scene.environment.clearColor, scene.environment.exposure), 1.0f);

    auto materials = gl::MaterialSet::create(defaults, textures.anisotropy());
    if (!materials) {
        return propagate(materials.error());
    }
    view.m_materials = std::move(*materials);

    // One GpuMesh per part, in scene order; a part that cannot be uploaded is logged and not drawn.
    std::vector<std::vector<std::optional<std::size_t>>> meshOfPart(geometry.objects.size());
    for (std::size_t objectIndex = 0; objectIndex < geometry.objects.size(); ++objectIndex) {
        const scene::SceneObjectGeometry& object = geometry.objects[objectIndex];
        meshOfPart[objectIndex].resize(object.parts.size());
        for (std::size_t partIndex = 0; partIndex < object.parts.size(); ++partIndex) {
            const scene::ScenePart& part = object.parts[partIndex];
            auto mesh = gl::GpuMesh::create(part.mesh, object.id + ":" + part.mesh.name);
            if (!mesh) {
                spdlog::error("Scene object '{}': part '{}' cannot be uploaded and is not drawn: {}", object.id,
                              part.role, mesh.error().toString());
                continue;
            }
            meshOfPart[objectIndex][partIndex] = view.m_meshes.size();
            view.m_meshes.push_back(std::move(*mesh));
        }
    }

    // Materials and drawing order come from the GPU-free plan (scene_draw_plan.hpp).
    const SceneDrawPlan plan = planSceneDraws(geometry);
    std::vector<scene::MaterialHandle> handles;
    handles.reserve(plan.materials.size());
    for (const PlannedMaterial& planned : plan.materials) {
        gl::MaterialTextures slots = materialTextures(planned.material, textures, defaults);
        if (planned.picture) {
            slots[gl::toIndex(gl::MaterialTextureSlot::BaseColor)] = &pictureTexture(planned, textures);
        }
        handles.push_back(view.m_materials.add(planned.label, planned.material, slots));
    }
    if (view.m_materials.size() > 0) {
        if (auto uploaded = view.m_materials.upload(caps.uniformBufferOffsetAlignment, "MaterialUniforms");
            !uploaded) {
            return propagate(uploaded.error());
        }
    }
    view.m_draws.reserve(plan.draws.size());
    for (const PlannedDraw& draw : plan.draws) {
        if (const std::optional<std::size_t> mesh = meshOfPart[draw.object][draw.part]) {
            view.m_draws.push_back(DrawItem{*mesh, handles[draw.material], draw.model});
        }
    }

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
                 view.m_materials.size(), plan.pictureMaterialCount(), textures.textureCount(),
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
