#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/material_library.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/socket.hpp>
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

/// What SceneInstance::replaceObjects() changes.
struct SceneObjectsChange {
    /// The objects and lights replaced: those whose id (a light's name) starts with it ("room:snes/").
    std::string prefix;
    /// Their replacements, in order: they take the place of the first object (light) replaced, else go last.
    std::vector<scene::SceneObject> objects;
    std::vector<scene::Light> lights;
};

/// What SceneInstance::replaceObjects() did: the new objects equal to one replaced (kept: their content as it was),
/// those built, and the old ones gone.
struct SceneObjectsChanged {
    std::size_t kept = 0;
    std::size_t built = 0;
    std::size_t removed = 0;
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
///  - Areas (I4 step 3, ADR-0013 §9; I6 step 1, ADR-0014 §4): in a scene with areas (a floor: its rooms and its
///    hall), the lights block holds the lights scene::AreaLighting selects for the area that holds the eye (the first
///    area holding it, else the last one held): its key light shadowed, its point and spot lights nearest the eye and
///    a reserve of its neighbours' seen through the doorways they share, never the key light, each faded in and out
///    over kLightFadeSeconds and clipped to its portal. The key light's shadow fades likewise: the shadow of the key
///    light the eye left fades out, then the next one's fades in. The area's ambient, fill and exposure are its own
///    (scene::AreaEnvironment over the scene's environment), eased over about half a second when the eye changes
///    areas (scene::EnvironmentEase). The application calls followEye() every frame with the camera's eye and the
///    frame's time, then advanceEnvironment(); everything is at once at the first frame, under reduceMotion and after
///    snapLighting() (a teleport). The developer overlay shows and edits the eye's area's terms in environment(). The
///    log names every area's key light at load (or why it casts none), and the area the eye enters. A scene without
///    areas packs its lights once, in list order, as before.
///  - Collision: scene::buildCollisionWorld of the content, for first-person navigation.
///  - Debug lines: the bounds of every object instance (scene::addSceneBoundsLines) and the axes of its sockets
///    (scene::addSceneSocketLines), kept apart so that each kind can be shown on its own.
///  - Objects: the local bounds of every object, for placing a viewer in front of one (the overlay's "go to").
///  - Objects replaced (I4b step 4, ADR-0015 §4: the editor rebuilds a room's static part alone): replaceObjects() puts
///    new objects and lights in place of those of an id prefix, building only what changed, the render items, the
///    collision world, the bounds, the debug lines and the packed lights following in the description's order.
/// Main thread only.
class SceneInstance {
public:
    /// Builds and registers `description`. Parts that cannot be uploaded are logged and not drawn. Errors: the material
    /// blocks cannot be uploaded (GPU allocation failure).
    [[nodiscard]] static Expected<SceneInstance> create(const scene::SceneDescription& description,
                                                        const assets::MaterialLibrary& library,
                                                        const scene::ModelLoader& loadModel,
                                                        const SceneResources& resources);

    /// `description`'s objects and lights of `change.prefix` replaced by `change`'s (I4b step 4): `description` is the
    /// one this instance shows (created from, kept in step since), changed in place. A new object equal to a replaced
    /// one of its id keeps what that one drew and collided with; the others are built and registered as create() does
    /// (deduplicated by key); the render items, the collision world, the object bounds and the debug lines follow the
    /// description's order as create() would make them; the lights are those of the new list (the others as
    /// sceneLights() holds them: the developer overlay's edits of them kept), the area that holds the eye choosing its
    /// lights again at once (as after snapLighting()). Errors: the material blocks cannot be uploaded
    /// (the content is then the new one, its new materials not drawable until an upload succeeds).
    [[nodiscard]] Expected<SceneObjectsChanged> replaceObjects(scene::SceneDescription& description,
                                                               const SceneObjectsChange& change,
                                                               const assets::MaterialLibrary& library,
                                                               const scene::ModelLoader& loadModel,
                                                               const SceneResources& resources);
    /// The indices into renderItems() of the items drawn for the objects whose ids `match` accepts (I4b step 4: a
    /// fixture held by the editor leaves its place), in order.
    [[nodiscard]] std::vector<std::size_t> itemsOf(const std::function<bool(std::string_view id)>& match) const;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] std::span<const scene::RenderItem> renderItems() const noexcept { return m_items; }
    [[nodiscard]] const gl::LightsUniforms& lights() const noexcept { return m_lights; }
    [[nodiscard]] const scene::CollisionWorld& collisionWorld() const noexcept { return m_collision; }
    /// The bounds of every object instance (plain lines), in world space.
    [[nodiscard]] const scene::DebugLines& boundsLines() const {
        rebuildObjectLines();
        return m_boundsLines;
    }
    /// The axes of every socket of every object instance (markers), in world space.
    [[nodiscard]] const scene::DebugLines& socketLines() const {
        rebuildObjectLines();
        return m_socketLines;
    }
    /// Object-local bounds of what each object draws (scene::SceneObjectGeometry::bounds), in the order of the scene
    /// description's objects.
    [[nodiscard]] std::span<const core::Aabb> objectBounds() const noexcept { return m_objectBounds; }

    /// The scene's lights as described (linear colors): the source of lights(). Edited in place by the developer
    /// overlay, then applyLighting().
    [[nodiscard]] std::span<scene::Light> sceneLights() noexcept { return m_sceneLights; }
    [[nodiscard]] std::span<const scene::Light> sceneLights() const noexcept { return m_sceneLights; }
    /// The scene's environment: the source of the ambient of lights(), the background radiance and the
    /// post-processing of every view, edited in place by the developer overlay. In a scene with areas its area terms
    /// (exposure, ambient, fill) are those of the area that holds the eye, and an edit of them is that area's.
    [[nodiscard]] scene::EnvironmentSetup& environment() noexcept { return m_environment; }
    [[nodiscard]] const scene::EnvironmentSetup& environment() const noexcept { return m_environment; }
    /// The environment the views use: environment(), its area terms (exposure, ambient, fill) in a scene with areas
    /// those of the area that holds the eye as eased so far.
    [[nodiscard]] scene::EnvironmentSetup viewEnvironment() const;
    /// Packs sceneLights() and the ambient of environment() into lights(); the lights left out are droppedLights(), the
    /// light that casts the shadows is shadowLight() (or shadowNote() says why none does). In a scene with areas, the
    /// lights the area that holds the eye selects (chosen again first, the lights' flags or places perhaps edited).
    void applyLighting();

    /// The scene's areas (empty: none).
    [[nodiscard]] std::span<const scene::SceneArea> areas() const noexcept { return m_areas; }
    /// The id of the area that holds the eye; empty in a scene without areas.
    [[nodiscard]] std::string_view areaId() const noexcept;
    /// The scene lights packed into lights() for the area that holds the eye, as indices into sceneLights(): its own,
    /// then its neighbours' (areaOwnLights() of them are its own); empty in a scene without areas (every light, in list
    /// order).
    [[nodiscard]] std::span<const std::uint32_t> areaSelection() const noexcept { return m_areaLighting.selection(); }
    /// How many of areaSelection()'s first lights are the eye's area's own.
    [[nodiscard]] std::size_t areaOwnLights() const noexcept { return m_areaLighting.ownCount(); }
    /// The weight of each light of areaSelection() (0 to 1).
    [[nodiscard]] std::span<const float> areaWeights() const noexcept { return m_areaLighting.weights(); }
    /// The point and spot lights of lights(), in its order: which scene light each is, its weight, whether a portal
    /// clips it.
    [[nodiscard]] std::span<const gl::PackedLightSource> packedLights() const noexcept { return m_packedLights; }
    /// True while a light or the key light's shadow fades in or out.
    [[nodiscard]] bool lightsFading() const noexcept {
        return m_areaLighting.fading() || (m_shadowSource.has_value() && m_shadowProgress < 1.0f);
    }
    /// Follows the eye (the camera's position, world space) in a scene with areas: the area that holds it, the lights
    /// chosen for it and their fades moved on by `seconds` (the frame's time), repacked when they change (cheap: a pass
    /// over the area's lights and its neighbours'), `forward` the view's unit direction (the lights wholly behind the
    /// eye are left out; zero: none are). Returns true when the area that holds the eye changed; nothing happens in a
    /// scene without areas.
    bool followEye(const glm::vec3& eye, const glm::vec3& forward = glm::vec3(0.0f), float seconds = 0.0f);
    /// The next followEye() cuts to its eye: the environment, the lights and the shadow at once (a teleport, the
    /// overlay's "go to").
    void snapLighting() noexcept { m_snapNext = true; }
    /// Moves the ease between two areas' environments on by `seconds` (the frame's time); also takes up an edit of
    /// environment() that the eye's area does not override. True when the views' environment changed; nothing happens
    /// in a scene without areas.
    bool advanceEnvironment(float seconds);
    /// The world's reduceMotion: a change of area switches the environment at once.
    void setReduceMotion(bool on) noexcept { m_reduceMotion = on; }
    /// True while the views ease from one area's environment to the next's.
    [[nodiscard]] bool environmentEasing() const noexcept { return m_environmentEase.easing(); }
    /// Scene lights that the last packing left out, with the reason.
    [[nodiscard]] const std::vector<gl::DroppedLight>& droppedLights() const noexcept { return m_droppedLights; }
    /// The key light of the last packing: the light that casts the views' shadow (casts() is false for none).
    [[nodiscard]] const gl::ShadowLight& shadowLight() const noexcept { return m_shadowLight; }
    /// The index in sceneLights() of the last packing's key light (gl::PackedLights::keyLight: the first light with
    /// castsShadows in the order packed; in a scene with areas, of the area that holds the eye), whether or not it
    /// casts; nothing when no light packed asks for shadows.
    [[nodiscard]] const std::optional<std::size_t>& keyLight() const noexcept { return m_keyLight; }
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
    /// The environment of the area that holds the eye: its terms over environment().
    [[nodiscard]] scene::EnvironmentSetup areaEnvironment() const;
    /// What each object of the description drew and collided with (I4b step 4: replaceObjects() keeps an unchanged
    /// object's): its id, its local bounds, instances and sockets (the debug lines), how many boxes of the collision
    /// world are its (the world lists them object after object).
    struct ObjectRecord {
        std::string id;
        core::Aabb bounds;
        std::vector<glm::mat4> instances;
        std::vector<geometry::Socket> sockets;
        std::size_t boxes = 0;
    };
    [[nodiscard]] static ObjectRecord recordOf(const scene::SceneObjectGeometry& object);
    /// The debug lines made again from m_objects when they were asked for since a replacement (lazily: a floor's
    /// sockets are many, the lines seldom shown).
    void rebuildObjectLines() const;
    /// Takes an edit of environment()'s area terms (the developer overlay's) into the area it showed them for, then
    /// shows the eye's area's there; true when the eye's area's terms changed.
    bool takeEnvironmentEdit();
    /// Moves the key light's shadow on by `seconds` (at once with `instant`); true when it changed.
    bool advanceShadow(float seconds, bool instant);

    std::string m_name;
    std::vector<scene::RenderItem> m_items;
    std::vector<scene::Light> m_sceneLights;
    scene::EnvironmentSetup m_environment;
    gl::LightsUniforms m_lights;
    std::vector<gl::DroppedLight> m_droppedLights;
    gl::ShadowLight m_shadowLight;
    std::optional<std::size_t> m_keyLight;
    std::string m_shadowNote;
    core::Aabb m_shadowCasterBounds;
    std::vector<scene::SceneArea> m_areas;
    scene::AreaLighting m_areaLighting;
    /// Each area's environment (its terms over the scene's), edited by the overlay through environment().
    std::vector<scene::EnvironmentSetup> m_areaEnvironments;
    /// The area whose terms environment() shows, and what it showed (takeEnvironmentEdit()).
    std::optional<std::size_t> m_shownArea;
    scene::EnvironmentSetup m_shown;
    scene::EnvironmentEase m_environmentEase;
    /// The light whose shadow the views draw (the eye's area's key light, or the one it left while its shadow fades),
    /// and the progress of its fade (advanceShadow()).
    std::optional<std::uint32_t> m_shadowSource;
    float m_shadowProgress = 0.0f;
    std::vector<gl::PackedLightSource> m_packedLights;
    bool m_snapNext = false;
    bool m_reduceMotion = false;
    /// followEye() was called: the first call switches the environment at once.
    bool m_eyeFollowed = false;
    /// The eye the area lighting last followed, and the view's direction then (zero: none).
    glm::vec3 m_eye{0.0f};
    glm::vec3 m_forward{0.0f};
    scene::CollisionWorld m_collision;
    /// Per description object (I4b step 4), and the object of each render item and whether it is a model's (create()
    /// lists the prefabs' and primitives' items first, in object order, then the models').
    std::vector<ObjectRecord> m_objects;
    std::vector<std::uint32_t> m_itemObjects;
    std::vector<bool> m_itemModels;
    mutable scene::DebugLines m_boundsLines;
    mutable scene::DebugLines m_socketLines;
    mutable bool m_linesStale = false;
    std::vector<core::Aabb> m_objectBounds;
};

} // namespace rv::app
