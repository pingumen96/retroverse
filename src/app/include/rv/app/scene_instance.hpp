#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/material_library.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/debug_lines.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_areas.hpp>
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
///    beyond its capacity are logged and left out), with the key light that casts the shadows (the first light with
///    castsShadows, when it can: a spot light, or the directional light when some item casts shadows,
///    gl::requireShadowCasters) and the world bounds of what the shadow pass draws (scene::shadowCasterBounds). The
///    environment also holds the background radiance, the ambient occlusion and the post-processing (exposure, bloom,
///    vignette), which the renderer reads per view (gl::postSettingsFor). The developer overlay edits the lights and
///    the environment in place (sceneLights(), environment()) and repacks the lights with applyLighting().
///  - Areas (I4 step 3, ADR-0013 §9): in a scene with areas (a floor: its rooms and its hall), the lights block holds
///    the lights of the area that holds the eye (scene::AreaLighting: the first area holding it, else the last one
///    held), its key light shadowed and its point and spot lights nearest the eye up to the block's capacity; the
///    application calls followEye() every frame with the camera's eye. A scene without areas packs its lights once, in
///    list order, as before.
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
    [[nodiscard]] std::span<const scene::Light> sceneLights() const noexcept { return m_sceneLights; }
    /// The scene's environment: the source of the ambient of lights(), the background radiance and the
    /// post-processing of every view, edited in place by the developer overlay.
    [[nodiscard]] scene::EnvironmentSetup& environment() noexcept { return m_environment; }
    [[nodiscard]] const scene::EnvironmentSetup& environment() const noexcept { return m_environment; }
    /// Packs sceneLights() and the ambient of environment() into lights(); the lights left out are droppedLights(), the
    /// light that casts the shadows is shadowLight() (or shadowNote() says why none does). In a scene with areas, the
    /// lights the area that holds the eye selects (chosen again first, the lights' flags or places perhaps edited).
    void applyLighting();

    /// The scene's areas (empty: none).
    [[nodiscard]] std::span<const scene::SceneArea> areas() const noexcept { return m_areas; }
    /// The id of the area that holds the eye; empty in a scene without areas.
    [[nodiscard]] std::string_view areaId() const noexcept;
    /// The scene lights packed into lights() for the area that holds the eye, as indices into sceneLights() in their
    /// order; empty in a scene without areas (every light, in list order).
    [[nodiscard]] std::span<const std::uint32_t> areaSelection() const noexcept { return m_areaLighting.selection(); }
    /// Follows the eye (the camera's position, world space) in a scene with areas: the area that holds it, and the
    /// lights packed for it when they change (cheap: a pass over the area's lights). Returns true when the area that
    /// holds the eye changed; nothing happens in a scene without areas.
    bool followEye(const glm::vec3& eye);
    /// Scene lights that the last packing left out, with the reason.
    [[nodiscard]] const std::vector<gl::DroppedLight>& droppedLights() const noexcept { return m_droppedLights; }
    /// The key light of the last packing: the light that casts the views' shadow (casts() is false for none).
    [[nodiscard]] const gl::ShadowLight& shadowLight() const noexcept { return m_shadowLight; }
    /// Why the first light that asks for shadows casts none in the last packing (a point light, a dropped light, a
    /// directional light while no item casts shadows); empty otherwise.
    [[nodiscard]] const std::string& shadowNote() const noexcept { return m_shadowNote; }
    /// World bounds of the render items the shadow pass draws (those that cast shadows and are not alpha-blended,
    /// scene::shadowCasterBounds): the orthographic shadow map of a directional key light is fitted to them. Empty when
    /// nothing casts.
    [[nodiscard]] const core::Aabb& shadowCasterBounds() const noexcept { return m_shadowCasterBounds; }

private:
    /// Packs the lights (all, or the area's selection) into m_lights with the key light and what was left out.
    void packLighting();

    std::string m_name;
    std::vector<scene::RenderItem> m_items;
    std::vector<scene::Light> m_sceneLights;
    scene::EnvironmentSetup m_environment;
    gl::LightsUniforms m_lights;
    std::vector<gl::DroppedLight> m_droppedLights;
    gl::ShadowLight m_shadowLight;
    std::string m_shadowNote;
    core::Aabb m_shadowCasterBounds;
    std::vector<scene::SceneArea> m_areas;
    scene::AreaLighting m_areaLighting;
    /// The eye the area lighting last followed.
    glm::vec3 m_eye{0.0f};
    scene::CollisionWorld m_collision;
    scene::DebugLines m_boundsLines;
    scene::DebugLines m_socketLines;
    std::vector<core::Aabb> m_objectBounds;
};

} // namespace rv::app
