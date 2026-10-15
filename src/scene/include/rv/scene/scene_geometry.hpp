#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::scene {

/// Edge length in meters of the cube that stands in for an object whose geometry is unavailable.
inline constexpr float kPlaceholderSize = 0.5f;
/// Material role of placeholder cubes.
inline constexpr std::string_view kPlaceholderRole = "placeholder";

/// One mesh of a scene object with the material resolved for its role.
struct ScenePart {
    geometry::MeshData mesh;
    /// Prefab part role ("label", "screen"), the primitive kind for primitives ("box"), kPlaceholderRole for
    /// placeholders.
    std::string role;
    assets::MaterialDesc material;
    /// `material` is the library fallback: the role has no material, the named one is unknown, the inline material is
    /// invalid, or the part is a placeholder.
    bool fallbackMaterial = false;
};

/// CPU-side geometry of one SceneObject: every part is drawn at every transform in `instances` (local -> world,
/// meters).
struct SceneObjectGeometry {
    std::string id;
    std::vector<ScenePart> parts;
    std::vector<glm::mat4> instances;
    /// Collision boxes in object-local space, repeated at every instance: the prefab's colliders
    /// (geometry::ProceduralPrefab::colliders), the mesh bounds of a primitive, the placeholder cube. Empty when the
    /// object does not collide (SceneObject::collides is false).
    std::vector<geometry::PrefabCollider> colliders;
    /// The object's own geometry is unavailable (builder error, glTF model): `parts` holds one placeholder cube.
    bool placeholder = false;
};

/// CPU-side geometry of a scene, objects in file order.
struct SceneGeometry {
    std::vector<SceneObjectGeometry> objects;

    /// Meshes over all objects (one per part).
    [[nodiscard]] std::size_t meshCount() const noexcept;
    /// Object instances (grid cells count one each).
    [[nodiscard]] std::size_t instanceCount() const noexcept;
    /// Triangles drawn per frame: every part's triangles times its object's instances.
    [[nodiscard]] std::size_t drawnTriangleCount() const noexcept;
    [[nodiscard]] std::size_t placeholderCount() const noexcept;
    /// Parts drawn with the fallback material (placeholders included).
    [[nodiscard]] std::size_t fallbackMaterialCount() const noexcept;
};

/// Builds the geometry of every object of `scene` and resolves the material of every part. GPU-free: the application
/// uploads the meshes and draws each part at each instance transform.
///  - Prefab objects: geometry::buildPrefab(kind, params), one part per material role in the prefab's part order.
///  - Primitive objects: geometry::buildPrimitive(kind, params), one part whose role is the kind.
///  - Model objects (glTF rendering arrives with the model loader step) and objects whose builder fails (out-of-range
///    parameters, for example): one kPlaceholderSize cube standing on the object origin, with the fallback material.
///    The reason is logged as a warning.
/// Material of a part with role R: the library material named by SceneObject::materialNameForRole(R) (an unknown name
/// gives the library fallback and is logged once by the library); without a name, the object's inlineMaterial parsed
/// with assets::parseMaterialDesc (once per object; a parse error is logged and gives the fallback); without either,
/// the fallback, logged. Colliders: see SceneObjectGeometry::colliders (a placeholder blocks like the furniture it
/// stands in for). Never fails: missing or bad content becomes visible magenta (style §21).
[[nodiscard]] SceneGeometry buildSceneGeometry(const SceneDescription& scene, const assets::MaterialLibrary& materials);

/// Static collision world of a scene for first-person navigation: every collider of every object at every instance
/// transform (CollisionWorld::addBox with the transform: exact for objects turned about +Y by any angle, the
/// conservative world box for tilted ones), in object order. Tags name the source: the object id, then "[<instance>]"
/// for objects with several instances, then "/<collider name>" for named colliders or "#<index>" for unnamed ones of
/// objects with several, e.g. "shelf_main", "room/north", "boxes_walnut[17]".
[[nodiscard]] CollisionWorld buildCollisionWorld(const SceneGeometry& geometry);

} // namespace rv::scene
