#pragma once

#include <span>
#include <string>
#include <vector>

#include <rv/assets/material_library.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/light_packing.hpp>
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
///  - Lights and environment: the scene lights and ambient packed into the lights block (gl::packLights; lights
///    beyond its capacity are logged and left out). The environment also holds the background radiance and the
///    post-processing (exposure, bloom, vignette), which the renderer reads per view (gl::postSettingsFor). The
///    developer overlay edits the lights and the environment in place (sceneLights(), environment()) and repacks the
///    lights with applyLighting().
///  - Collision: scene::buildCollisionWorld of the content, for first-person navigation.
///  - Debug lines: the bounds of every object instance (scene::addSceneBoundsLines) and the axes of its sockets
///    (scene::addSceneSocketLines), kept apart so that each kind can be shown on its own.
///  - Objects: the local bounds of every object, for placing a viewer in front of one (the overlay's "go to").
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
    [[nodiscard]] const scene::CollisionWorld& collisionWorld() const noexcept { return m_collision; }
    /// The bounds of every object instance (plain lines), in world space.
    [[nodiscard]] const scene::DebugLines& boundsLines() const noexcept { return m_boundsLines; }
    /// The axes of every socket of every object instance (markers), in world space.
    [[nodiscard]] const scene::DebugLines& socketLines() const noexcept { return m_socketLines; }
    /// Object-local bounds of what each object draws (scene::SceneObjectGeometry::bounds), in the order of the scene
    /// description's objects.
    [[nodiscard]] std::span<const core::Aabb> objectBounds() const noexcept { return m_objectBounds; }

    /// The scene's lights as described (linear colors): the source of lights(). Edited in place by the developer
    /// overlay, then applyLighting().
    [[nodiscard]] std::span<scene::Light> sceneLights() noexcept { return m_sceneLights; }
    /// The scene's environment: the source of the ambient of lights(), the background radiance and the
    /// post-processing of every view, edited in place by the developer overlay.
    [[nodiscard]] scene::EnvironmentSetup& environment() noexcept { return m_environment; }
    /// Packs sceneLights() and the ambient of environment() into lights(); the lights left out are droppedLights().
    void applyLighting();
    /// Scene lights that the last packing left out, with the reason.
    [[nodiscard]] const std::vector<gl::DroppedLight>& droppedLights() const noexcept { return m_droppedLights; }

private:
    std::string m_name;
    std::vector<scene::RenderItem> m_items;
    std::vector<scene::Light> m_sceneLights;
    scene::EnvironmentSetup m_environment;
    gl::LightsUniforms m_lights;
    std::vector<gl::DroppedLight> m_droppedLights;
    scene::CollisionWorld m_collision;
    scene::DebugLines m_boundsLines;
    scene::DebugLines m_socketLines;
    std::vector<core::Aabb> m_objectBounds;
};

} // namespace rv::app
