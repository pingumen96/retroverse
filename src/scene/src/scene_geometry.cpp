#include <rv/scene/scene_geometry.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <rv/geometry/mesh_builder.hpp>
#include <rv/geometry/prefabs.hpp>

namespace rv::scene {
namespace {

/// Magenta cube resting on the object origin, so a missing piece of furniture still shows where it should stand.
ScenePart placeholderPart(const assets::MaterialLibrary& materials) {
    const glm::mat4 standOnOrigin = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f * kPlaceholderSize, 0.0f));
    geometry::MeshBuilder builder;
    builder.box(glm::vec3(kPlaceholderSize), standOnOrigin);
    ScenePart part;
    part.mesh = std::move(builder).build("placeholder");
    part.role = std::string(kPlaceholderRole);
    part.material = materials.fallback();
    part.fallbackMaterial = true;
    return part;
}

/// Geometry of one object: (role, mesh) pairs and object-local colliders.
struct ObjectShapes {
    std::vector<std::pair<std::string, geometry::MeshData>> meshes;
    std::vector<geometry::PrefabCollider> colliders;
};

/// Builds the meshes and colliders of one object, or returns the builder error.
Expected<ObjectShapes> buildObjectShapes(const SceneObject& object) {
    ObjectShapes shapes;
    switch (object.source) {
    case SceneObject::Source::Prefab: {
        auto prefab = geometry::buildPrefab(object.kind, object.params);
        if (!prefab) {
            return propagate(prefab.error());
        }
        for (geometry::PrefabPart& part : prefab->parts) {
            shapes.meshes.emplace_back(std::move(part.materialRole), std::move(part.mesh));
        }
        shapes.colliders = std::move(prefab->colliders);
        return shapes;
    }
    case SceneObject::Source::Primitive: {
        auto mesh = geometry::buildPrimitive(object.kind, object.params);
        if (!mesh) {
            return propagate(mesh.error());
        }
        shapes.colliders.push_back(geometry::PrefabCollider{{}, mesh->bounds});
        shapes.meshes.emplace_back(object.kind, std::move(*mesh));
        return shapes;
    }
    case SceneObject::Source::Model:
        break;
    }
    return makeError(ErrorCode::Unsupported, "glTF models are not rendered yet", object.modelPath);
}

/// Collision tag of one world box, see buildCollisionWorld().
std::string colliderTag(const SceneObjectGeometry& object, std::size_t instance, std::size_t index) {
    std::string tag = object.id;
    if (object.instances.size() > 1) {
        tag += fmt::format("[{}]", instance);
    }
    const geometry::PrefabCollider& collider = object.colliders[index];
    if (!collider.name.empty()) {
        tag += "/" + collider.name;
    } else if (object.colliders.size() > 1) {
        tag += fmt::format("#{}", index);
    }
    return tag;
}

/// Resolves the materials of one object's parts; the inline material is parsed at most once.
class MaterialResolver {
public:
    MaterialResolver(const SceneObject& object, std::size_t objectIndex, std::string_view sceneName,
                     const assets::MaterialLibrary& materials)
        : m_object(object), m_objectIndex(objectIndex), m_sceneName(sceneName), m_materials(materials) {}

    void resolve(ScenePart& part) {
        const std::string_view name = m_object.materialNameForRole(part.role);
        if (!name.empty()) {
            const assets::MaterialDesc* found = m_materials.find(name);
            part.material = m_materials.getOrFallback(name); // logs an unknown name once
            part.fallbackMaterial = found == nullptr;
            return;
        }
        if (const assets::MaterialDesc* inlineMaterial = parsedInlineMaterial()) {
            part.material = *inlineMaterial;
            return;
        }
        part.material = m_materials.fallback();
        part.fallbackMaterial = true;
        if (!m_object.inlineMaterial.is_object()) {
            spdlog::warn("Scene object '{}' assigns no material to role '{}'; using {}", m_object.id, part.role,
                         part.material.name);
        }
    }

private:
    const assets::MaterialDesc* parsedInlineMaterial() {
        if (!m_object.inlineMaterial.is_object()) {
            return nullptr;
        }
        if (!m_inlineParsed) {
            m_inlineParsed = true;
            const std::string context = fmt::format("{}:/objects/{}/inlineMaterial", m_sceneName, m_objectIndex);
            auto parsed = assets::parseMaterialDesc(m_object.inlineMaterial, m_object.id + ".inlineMaterial", context);
            if (parsed) {
                m_inline = std::move(*parsed);
            } else {
                spdlog::warn("Scene object '{}' has an invalid inline material ({}); using {}", m_object.id,
                             parsed.error().toString(), m_materials.fallback().name);
            }
        }
        return m_inline ? &*m_inline : nullptr;
    }

    const SceneObject& m_object;
    std::size_t m_objectIndex;
    std::string_view m_sceneName;
    const assets::MaterialLibrary& m_materials;
    std::optional<assets::MaterialDesc> m_inline;
    bool m_inlineParsed = false;
};

} // namespace

std::size_t SceneGeometry::meshCount() const noexcept {
    std::size_t count = 0;
    for (const SceneObjectGeometry& object : objects) {
        count += object.parts.size();
    }
    return count;
}

std::size_t SceneGeometry::instanceCount() const noexcept {
    std::size_t count = 0;
    for (const SceneObjectGeometry& object : objects) {
        count += object.instances.size();
    }
    return count;
}

std::size_t SceneGeometry::drawnTriangleCount() const noexcept {
    std::size_t count = 0;
    for (const SceneObjectGeometry& object : objects) {
        for (const ScenePart& part : object.parts) {
            count += part.mesh.triangleCount() * object.instances.size();
        }
    }
    return count;
}

std::size_t SceneGeometry::placeholderCount() const noexcept {
    std::size_t count = 0;
    for (const SceneObjectGeometry& object : objects) {
        count += object.placeholder ? 1u : 0u;
    }
    return count;
}

std::size_t SceneGeometry::fallbackMaterialCount() const noexcept {
    std::size_t count = 0;
    for (const SceneObjectGeometry& object : objects) {
        for (const ScenePart& part : object.parts) {
            count += part.fallbackMaterial ? 1u : 0u;
        }
    }
    return count;
}

SceneGeometry buildSceneGeometry(const SceneDescription& scene, const assets::MaterialLibrary& materials) {
    SceneGeometry geometry;
    geometry.objects.reserve(scene.objects.size());
    for (std::size_t index = 0; index < scene.objects.size(); ++index) {
        const SceneObject& object = scene.objects[index];
        SceneObjectGeometry& built = geometry.objects.emplace_back();
        built.id = object.id;
        const std::span<const glm::mat4> transforms = object.instanceTransforms();
        built.instances.assign(transforms.begin(), transforms.end());

        auto shapes = buildObjectShapes(object);
        if (!shapes) {
            spdlog::warn("Scene object '{}' is drawn as a placeholder: {}", object.id, shapes.error().toString());
            ScenePart& placeholder = built.parts.emplace_back(placeholderPart(materials));
            built.placeholder = true;
            if (object.collides) {
                built.colliders.push_back(geometry::PrefabCollider{{}, placeholder.mesh.bounds});
            }
            continue;
        }
        MaterialResolver resolver(object, index, scene.name, materials);
        for (auto& [role, mesh] : shapes->meshes) {
            ScenePart& part = built.parts.emplace_back();
            part.mesh = std::move(mesh);
            part.role = std::move(role);
            resolver.resolve(part);
        }
        if (object.collides) {
            built.colliders = std::move(shapes->colliders);
        }
    }
    return geometry;
}

CollisionWorld buildCollisionWorld(const SceneGeometry& geometry) {
    CollisionWorld world;
    for (const SceneObjectGeometry& object : geometry.objects) {
        for (std::size_t instance = 0; instance < object.instances.size(); ++instance) {
            for (std::size_t index = 0; index < object.colliders.size(); ++index) {
                world.addBox(object.colliders[index].box.transformed(object.instances[instance]),
                             colliderTag(object, instance, index));
            }
        }
    }
    return world;
}

} // namespace rv::scene
