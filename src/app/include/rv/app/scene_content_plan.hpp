#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::app {

/// A mesh the scene uploads once, shared by every part with its key.
struct PlannedMesh {
    /// scene::ScenePart::meshKey.
    std::string key;
    std::shared_ptr<const geometry::MeshData> mesh;
};

/// A material the scene registers on the GPU.
struct PlannedMaterial {
    /// Identity in the plan (and in the material registry): the material's name for a shared material;
    /// "<object id>/<role>" for the instance of an object's part with a scene texture, followed by
    /// "@<width>x<height>" when the texture is a pattern.
    std::string key;
    /// Name for logs and GPU debug labels: the material's name, or "<key> (<material name>)" for an instance.
    std::string label;
    assets::MaterialDesc material;
    scene::ShaderKind shader = scene::ShaderKind::Pbr; ///< gl::shaderKindOf(material)
    /// Instances only: the scene texture of the part (ScenePart::texture), which becomes the base color texture of the
    /// material. A file or a pattern is a single picture (cartridge label, poster, CRT screen), sampled clamped to its
    /// edges; a procedural surface texture repeats like the surface it is.
    std::optional<scene::TextureSource> picture;
    /// Pattern pictures: the size to generate them at, in pixels, with the aspect ratio of the surface as drawn
    /// (geometry::uvMetersPerUnit with the instance transform, then assets::patternSizeForAspect), so that a cover is
    /// not stretched on a label or on a poster scaled unevenly, and a resolution that follows the surface's size
    /// (assets::patternLongSideFor its longer side: a cartridge label gets about half the pixels of a poster). (0, 0)
    /// otherwise.
    glm::ivec2 patternSize{0};
};

/// One part of one object instance: a render item.
struct PlannedItem {
    std::size_t object = 0;   ///< index into SceneGeometry::objects
    std::size_t part = 0;     ///< index into the object's parts
    std::size_t mesh = 0;     ///< index into SceneContentPlan::meshes
    std::size_t material = 0; ///< index into SceneContentPlan::materials
    glm::mat4 model{1.0f};    ///< the object instance transform, local -> world
    bool castsShadow = true;  ///< SceneObjectGeometry::castsShadows
};

/// One instance of a model object, drawn with the model's own meshes and materials (gl::GpuModel of its modelPath).
struct PlannedModelInstance {
    std::size_t object = 0; ///< index into SceneGeometry::objects
    glm::mat4 model{1.0f};  ///< the object instance transform, model space -> world
    bool castsShadow = true;
};

/// What a scene registers on the GPU and draws: its distinct meshes and materials, a render item per part and object
/// instance, and the instances of its model objects.
struct SceneContentPlan {
    std::vector<PlannedMesh> meshes;
    std::vector<PlannedMaterial> materials;
    std::vector<PlannedItem> items;
    std::vector<PlannedModelInstance> modelInstances;

    /// Materials with a picture (instances).
    [[nodiscard]] std::size_t pictureMaterialCount() const noexcept;
};

/// Plans the content of `geometry`, deterministically, in scene order (objects, then parts, then instances):
///  - Meshes: one per distinct ScenePart::meshKey, in order of first use, so objects built from the same shape share
///    their meshes (the 24 cartridges of the test room: one shell mesh, one label mesh).
///  - Materials, in order of first use: one per material name for the parts without a scene texture; one instance per
///    object, role and pattern size for the parts with one (their textures differ, so do their materials). The
///    instances of a grid share their rotation and scale, hence one material instance.
///  - Items: one per part and object instance, with its mesh, material, transform and shadow flag. Drawing order and
///    batching are the renderer's (scene::BatchPlanner).
///  - Model instances: one per instance of an object with a loaded model.
[[nodiscard]] SceneContentPlan planSceneContent(const scene::SceneGeometry& geometry);

} // namespace rv::app
