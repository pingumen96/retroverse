#pragma once

#include <span>
#include <string>
#include <vector>

#include <glm/vec4.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/debug_lines.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::gl {
class MaterialRegistry;
class MeshRegistry;
class TextureCache;
} // namespace rv::gl

namespace rv::app {

/// Where a SceneInstance registers its content. Everything must outlive the instance.
struct SceneResources {
    gl::MeshRegistry& meshes;
    gl::MaterialRegistry& materials;
    gl::TextureCache& textures;
    /// GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT (GLCapabilities), for the material blocks.
    int uniformBufferOffsetAlignment = 256;
};

/// A scene description turned into drawable content: the reusable step between a scene file (or the model viewer) and
/// gl::SceneRenderer.
///  - Content: scene::buildSceneGeometry (prefabs and primitives built once per kind and parameters, glTF models loaded
///    once per path through the model loader, magenta placeholders for what is missing or broken).
///  - Meshes and materials: planned by planSceneContent (scene_content_plan.hpp) and registered in the registries,
///    deduplicated by key: one mesh per distinct shape part, one material per material name and one instance per
///    object and role with a scene texture (cover patterns on labels and posters, color bars on screens, generated at
///    the aspect ratio of their surface; pictures clamped to their edges); textures named by the materials repeat and
///    missing ones are drawn as the checker. Model objects register their meshes, materials and images through one
///    gl::GpuModel per distinct model.
///  - Render items: one per part and object instance, and one per model primitive and model instance.
///  - Lights and background: the scene lights and environment packed into the lights block (gl::packLights; lights
///    beyond its capacity are logged and left out) and the clear color exposed and tonemapped like the surfaces
///    (gl::tonemap), so the void beyond a doorway sits on the scale of the lit room.
///  - Collision: scene::buildCollisionWorld of the content, for first-person navigation.
///  - Debug lines: the bounds of every object instance and the axes of its sockets (scene::addSceneDebugLines).
/// Main thread only.
class SceneInstance {
public:
    /// Builds and registers `description`. Parts that cannot be uploaded are logged and not drawn. Errors: the material
    /// blocks cannot be uploaded (GPU allocation failure).
    [[nodiscard]] static Expected<SceneInstance> create(const scene::SceneDescription& description,
                                                        const assets::MaterialLibrary& library,
                                                        const scene::ModelLoader& loadModel,
                                                        const SceneResources& resources);

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] std::span<const scene::RenderItem> renderItems() const noexcept { return m_items; }
    [[nodiscard]] const gl::LightsUniforms& lights() const noexcept { return m_lights; }
    /// Linear display value of the background (exposed and tonemapped), encoded by the sRGB targets.
    [[nodiscard]] const glm::vec4& clearColor() const noexcept { return m_clearColor; }
    [[nodiscard]] const scene::CollisionWorld& collisionWorld() const noexcept { return m_collision; }
    /// Object bounds and socket axes, in world space.
    [[nodiscard]] const scene::DebugLines& debugLines() const noexcept { return m_debugLines; }

private:
    std::string m_name;
    std::vector<scene::RenderItem> m_items;
    gl::LightsUniforms m_lights;
    glm::vec4 m_clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    scene::CollisionWorld m_collision;
    scene::DebugLines m_debugLines;
};

} // namespace rv::app
