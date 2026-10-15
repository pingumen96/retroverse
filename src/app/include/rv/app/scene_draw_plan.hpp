#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::app {

/// Passes of a frame, in drawing order.
enum class DrawPass : std::uint8_t {
    Opaque, ///< assets::AlphaMode::Opaque
    Mask,   ///< assets::AlphaMode::Mask: alpha-tested, depth written
    Blend,  ///< assets::AlphaMode::Blend: blended over everything else, depth writes off
};

/// Pass of the materials with alpha mode `mode`.
[[nodiscard]] DrawPass drawPassOf(assets::AlphaMode mode) noexcept;

/// A material the scene view creates on the GPU.
struct PlannedMaterial {
    /// Identity in the plan: the material's name for a shared material; "<object id>/<role>" for the instance of an
    /// object's part with a scene texture, followed by "@<width>x<height>" when the texture is a pattern.
    std::string key;
    /// Name for logs and GPU debug labels: the material's name, or "<key> (<material name>)" for an instance.
    std::string label;
    assets::MaterialDesc material;
    scene::ShaderKind shader = scene::ShaderKind::Pbr; ///< gl::shaderKindOf(material)
    DrawPass pass = DrawPass::Opaque;                  ///< drawPassOf(material.alphaMode)
    /// Instances only: the scene texture of the part (ScenePart::texture), which becomes the base color texture of the
    /// material. It is a single picture (cartridge label, poster, CRT screen), so it is sampled clamped to its edges,
    /// never repeated.
    std::optional<scene::TextureSource> picture;
    /// Pattern pictures: the size to generate them at, in pixels, with the aspect ratio of the surface as drawn
    /// (geometry::uvMetersPerUnit with the instance transform, then assets::patternSizeForAspect), so that a cover is
    /// not stretched on a label or on a poster scaled unevenly. (0, 0) otherwise.
    glm::ivec2 patternSize{0};
};

/// One part of one object instance.
struct PlannedDraw {
    std::size_t object = 0;   ///< index into SceneGeometry::objects
    std::size_t part = 0;     ///< index into the object's parts
    std::size_t material = 0; ///< index into SceneDrawPlan::materials
    glm::mat4 model{1.0f};    ///< the object instance transform, local -> world
};

/// What the lit scene view creates and draws for a scene: its materials, and every part instance in drawing order.
struct SceneDrawPlan {
    std::vector<PlannedMaterial> materials;
    std::vector<PlannedDraw> draws;

    /// Materials with a picture (instances).
    [[nodiscard]] std::size_t pictureMaterialCount() const noexcept;
};

/// Plans the draws of `geometry`, deterministically:
///  - Materials, in order of first use (objects, parts, instances in scene order): one per material name for the parts
///    without a scene texture; one instance per object, role and pattern size for the parts with one. The instances
///    of a grid share their rotation and scale, hence one material instance.
///  - Draws: one per part and object instance. Opaque materials first, then alpha-masked ones, each sorted by shader,
///    material, object and part (fewer program and material switches); then alpha-blended ones in scene order
///    (object, part, instance), without depth sorting: no M0 scene uses one, sorting arrives with the batching
///    renderer. The instances of a part keep their order.
[[nodiscard]] SceneDrawPlan planSceneDraws(const scene::SceneGeometry& geometry);

} // namespace rv::app
