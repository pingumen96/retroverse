#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/assets/model_data.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::scene {

/// Edge length in meters of the cube that stands in for an object whose geometry is unavailable.
inline constexpr float kPlaceholderSize = 0.5f;
/// Material role of placeholder cubes.
inline constexpr std::string_view kPlaceholderRole = "placeholder";
/// Mesh key of the placeholder cube, shared by every placeholder.
inline constexpr std::string_view kPlaceholderMeshKey = "placeholder";

/// Object-local bounds of the placeholder cube: kPlaceholderSize on a side, standing on the object origin.
[[nodiscard]] core::Aabb placeholderBounds() noexcept;

/// Loads the glTF model a scene object names (SceneObject::modelPath). The model is shared by every object that names
/// the same path: buildSceneGeometry calls the loader once per distinct path.
using ModelLoader = std::function<Expected<std::shared_ptr<const assets::ModelData>>(std::string_view modelPath)>;

/// The loader of scene files: assets::loadGltf(<assetRoot>/<modelPath>) with the default options (scene files only
/// hold asset-relative paths without "..", see SceneDescription).
[[nodiscard]] ModelLoader assetModelLoader(std::filesystem::path assetRoot);

/// One mesh of a scene object with the material resolved for its role.
struct ScenePart {
    /// The part's geometry, shared by the parts of every object built from the same shape: the same source, kind and
    /// parameters build once (24 cartridges with the same parameters share one shell mesh and one label mesh).
    std::shared_ptr<const geometry::MeshData> mesh;
    /// Identity of `mesh`: parts with equal keys share the same geometry, so a renderer uploads it once.
    /// "<prefab|primitive>:<kind>:<parameters as JSON>#<role>", or kPlaceholderMeshKey.
    std::string meshKey;
    /// Prefab part role ("label", "screen"), the primitive kind for primitives ("box"), kPlaceholderRole for
    /// placeholders.
    std::string role;
    assets::MaterialDesc material;
    /// `material` is the library fallback: the role has no material, the named one is unknown, the inline material is
    /// invalid, or the part is a placeholder.
    bool fallbackMaterial = false;
    /// Texture the scene binds to the part's role (SceneObject::texturesByRole): the renderer uses it as the base
    /// color texture of `material`, in a material instance of its own. Empty when the role has none, and for
    /// placeholders.
    std::optional<TextureSource> texture;
};

/// CPU-side content of one SceneObject, drawn at every transform in `instances` (local -> world, meters).
struct SceneObjectGeometry {
    std::string id;
    /// Meshes with their materials: the parts of a prefab, the mesh of a primitive, or one placeholder cube. Empty for
    /// a model object, which draws `model`.
    std::vector<ScenePart> parts;
    /// Model objects whose file loaded: the model, shared by every object that names the same path (loaded once) and
    /// drawn with its own meshes, materials and images. Null otherwise.
    std::shared_ptr<const assets::ModelData> model;
    /// Model objects: SceneObject::modelPath, the identity of `model` (also for placeholders standing in for it).
    std::string modelPath;
    std::vector<glm::mat4> instances;
    /// Object-local bounds of what is drawn: prefab bounds, primitive mesh bounds, model bounds or placeholderBounds().
    core::Aabb bounds;
    /// Object-local sockets of the prefab (shelf slots, CRT screen surface, anchors) or of the model (style §9.2 node
    /// names); none for primitives and placeholders.
    std::vector<geometry::Socket> sockets;
    /// Collision boxes in object-local space, repeated at every instance: the prefab's colliders
    /// (geometry::ProceduralPrefab::colliders), the mesh bounds of a primitive, the boxes of a model's ColliderProxy
    /// nodes (else the model bounds), the placeholder cube. Empty when the object does not collide
    /// (SceneObject::collides is false).
    std::vector<geometry::PrefabCollider> colliders;
    /// SceneObject::castsShadows.
    bool castsShadows = true;
    /// The object's own geometry is unavailable (builder error, model that cannot be loaded or has nothing to draw):
    /// `parts` holds one placeholder cube.
    bool placeholder = false;
};

/// CPU-side content of a scene, objects in file order.
struct SceneGeometry {
    std::vector<SceneObjectGeometry> objects;

    /// Parts over all objects (model primitives not included).
    [[nodiscard]] std::size_t partCount() const noexcept;
    /// Distinct meshes a renderer uploads: distinct part mesh keys plus, for every distinct model, the meshes its draw
    /// items use (not its ColliderProxy meshes, which are not drawn, nor meshes outside its active scene).
    [[nodiscard]] std::size_t uniqueMeshCount() const noexcept;
    /// Object instances (grid cells count one each).
    [[nodiscard]] std::size_t instanceCount() const noexcept;
    /// Triangles drawn per frame: every part's and every model primitive's triangles times its object's instances.
    [[nodiscard]] std::size_t drawnTriangleCount() const noexcept;
    [[nodiscard]] std::size_t placeholderCount() const noexcept;
    /// Objects drawn with a loaded model.
    [[nodiscard]] std::size_t modelObjectCount() const noexcept;
    /// Parts drawn with the fallback material (placeholders included).
    [[nodiscard]] std::size_t fallbackMaterialCount() const noexcept;
};

/// Builds the content of every object of `scene` and resolves the material of every part. GPU-free: the application
/// uploads the meshes and draws each part at each instance transform.
///  - Prefab objects: geometry::buildPrefab(kind, params), one part per material role in the prefab's part order, with
///    the prefab's sockets.
///  - Primitive objects: geometry::buildPrimitive(kind, params), one part whose role is the kind.
///    Objects with the same source, kind and parameters share the result: each distinct shape is built once and its
///    meshes are shared (ScenePart::mesh, ScenePart::meshKey).
///  - Model objects: `loadModel(modelPath)`, called once per distinct path; the object keeps the model (drawn with its
///    own materials: material, materials, textures and inlineMaterial of a model object are ignored, with a warning)
///    and its sockets. Model warnings are logged once per model, also for a model that has nothing to draw (they say
///    why: skipped primitives).
///  - Objects whose geometry is unavailable (builder error such as out-of-range parameters, model that cannot be
///    loaded or has nothing to draw, no `loadModel`): one kPlaceholderSize cube standing on the object origin, with
///    the fallback material. The reason is logged as a warning.
/// Material of a part with role R: the library material named by SceneObject::materialNameForRole(R) (an unknown name
/// gives the library fallback and is logged once by the library); without a name, the object's inlineMaterial parsed
/// with assets::parseMaterialDesc (once per object; a parse error is logged and gives the fallback); without either,
/// the fallback, logged. Textures: ScenePart::texture is SceneObject::texturesByRole[R], when present. Colliders: see
/// SceneObjectGeometry::colliders (a placeholder blocks like the furniture it stands in for). Never fails: missing or
/// bad content becomes visible magenta (style §21).
[[nodiscard]] SceneGeometry buildSceneGeometry(const SceneDescription& scene, const assets::MaterialLibrary& materials,
                                               const ModelLoader& loadModel = {});

/// Static collision world of a scene for first-person navigation: every collider of every object at every instance
/// transform (CollisionWorld::addBox with the transform: exact for objects turned about +Y by any angle, the
/// conservative world box for tilted ones), in object order. Tags name the source: the object id, then "[<instance>]"
/// for objects with several instances, then "/<collider name>" for named colliders or "#<index>" for unnamed ones of
/// objects with several, e.g. "shelf_main", "room/north", "boxes_walnut[17]".
[[nodiscard]] CollisionWorld buildCollisionWorld(const SceneGeometry& geometry);
/// The colliders of `object` added to `world` as buildCollisionWorld() adds them (it adds every object's in turn).
void addObjectColliders(CollisionWorld& world, const SceneObjectGeometry& object);

} // namespace rv::scene
