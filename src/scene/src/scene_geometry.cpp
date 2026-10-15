#include <rv/scene/scene_geometry.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/assets/gltf_loader.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/mesh_builder.hpp>
#include <rv/geometry/prefabs.hpp>

namespace rv::scene {
namespace {

using SharedMesh = std::shared_ptr<const geometry::MeshData>;
using SharedModel = std::shared_ptr<const assets::ModelData>;

/// Built content of one shape (a source, kind and parameter set), shared by every object that uses it.
struct Shape {
    /// (role, mesh) in the prefab's part order.
    std::vector<std::pair<std::string, SharedMesh>> meshes;
    std::vector<geometry::PrefabCollider> colliders;
    std::vector<geometry::Socket> sockets;
    core::Aabb bounds;
};

/// "<prefab|primitive>:<kind>:<parameters as JSON>". JSON objects keep their keys sorted, so equal parameter sets
/// print equally (a value written as 1 or as 1.0 prints differently, which only costs a second build).
std::string shapeKey(const SceneObject& object) {
    const std::string_view source = object.source == SceneObject::Source::Prefab ? "prefab" : "primitive";
    return fmt::format("{}:{}:{}", source, object.kind,
                       object.params.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

/// Builds the meshes, colliders and sockets of a prefab or primitive object, or returns the builder error.
Expected<Shape> buildShape(const SceneObject& object) {
    Shape shape;
    if (object.source == SceneObject::Source::Prefab) {
        auto prefab = geometry::buildPrefab(object.kind, object.params);
        if (!prefab) {
            return propagate(prefab.error());
        }
        for (geometry::PrefabPart& part : prefab->parts) {
            shape.meshes.emplace_back(std::move(part.materialRole),
                                      std::make_shared<const geometry::MeshData>(std::move(part.mesh)));
        }
        shape.colliders = std::move(prefab->colliders);
        shape.sockets = std::move(prefab->sockets);
        shape.bounds = prefab->bounds;
        return shape;
    }
    auto mesh = geometry::buildPrimitive(object.kind, object.params);
    if (!mesh) {
        return propagate(mesh.error());
    }
    shape.bounds = mesh->bounds;
    shape.colliders.push_back(geometry::PrefabCollider{{}, mesh->bounds});
    shape.meshes.emplace_back(object.kind, std::make_shared<const geometry::MeshData>(std::move(*mesh)));
    return shape;
}

/// Models by path, each loaded once: the model, or the reason it is unavailable.
class ModelCache {
public:
    explicit ModelCache(const ModelLoader& loader) : m_loader(loader) {}

    const Expected<SharedModel>& get(const std::string& path) {
        if (const auto found = m_models.find(path); found != m_models.end()) {
            return found->second;
        }
        return m_models.emplace(path, load(path)).first->second;
    }

private:
    Expected<SharedModel> load(const std::string& path) const {
        if (!m_loader) {
            return makeError(ErrorCode::Unsupported, "no model loader was given", path);
        }
        auto model = m_loader(path);
        if (!model) {
            return propagate(model.error());
        }
        if (!*model) {
            return makeError(ErrorCode::Unknown, "the model loader returned no model", path);
        }
        // Before the check below: the warnings of an empty model say why it is empty (skipped primitives).
        for (const std::string& warning : (*model)->warnings) {
            spdlog::warn("Model '{}': {}", path, warning);
        }
        if ((*model)->drawItems.empty()) {
            return makeError(ErrorCode::ValidationFailed, "the model has no triangles to draw", path);
        }
        return std::move(*model);
    }

    const ModelLoader& m_loader;
    std::map<std::string, Expected<SharedModel>, std::less<>> m_models;
};

/// Colliders of a model: the boxes of its ColliderProxy nodes (model space), else its bounds.
std::vector<geometry::PrefabCollider> modelColliders(const assets::ModelData& model) {
    std::vector<geometry::PrefabCollider> colliders;
    for (const geometry::Socket& socket : model.sockets) {
        if (socket.kind == geometry::SocketKind::ColliderProxy && !socket.bounds.isEmpty()) {
            colliders.push_back(geometry::PrefabCollider{socket.id, socket.bounds.transformed(socket.transform)});
        }
    }
    if (colliders.empty()) {
        colliders.push_back(geometry::PrefabCollider{{}, model.bounds});
    }
    return colliders;
}

/// Magenta cube resting on the object origin, so a missing piece of furniture still shows where it should stand.
SharedMesh makePlaceholderMesh() {
    const glm::mat4 standOnOrigin = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f * kPlaceholderSize, 0.0f));
    geometry::MeshBuilder builder;
    builder.box(glm::vec3(kPlaceholderSize), standOnOrigin);
    return std::make_shared<const geometry::MeshData>(std::move(builder).build("placeholder"));
}

/// Turns `built` into a placeholder cube (one mesh shared by every placeholder of the scene) and logs why.
void usePlaceholder(SceneObjectGeometry& built, const SceneObject& object, const Error& reason,
                    const assets::MaterialLibrary& materials, SharedMesh& sharedMesh) {
    spdlog::warn("Scene object '{}' is drawn as a placeholder: {}", object.id, reason.toString());
    if (!sharedMesh) {
        sharedMesh = makePlaceholderMesh();
    }
    ScenePart& part = built.parts.emplace_back();
    part.mesh = sharedMesh;
    part.meshKey = std::string(kPlaceholderMeshKey);
    part.role = std::string(kPlaceholderRole);
    part.material = materials.fallback();
    part.fallbackMaterial = true;
    built.placeholder = true;
    built.bounds = sharedMesh->bounds;
    if (object.collides) {
        built.colliders.push_back(geometry::PrefabCollider{{}, built.bounds});
    }
}

/// A model object draws its own materials: material assignments of the scene would silently do nothing.
void warnAboutModelMaterials(const SceneObject& object) {
    if (!object.material.empty() || !object.materialsByRole.empty() || !object.texturesByRole.empty() ||
        object.inlineMaterial.is_object()) {
        spdlog::warn("Scene object '{}' is a glTF model, drawn with its own materials: its material, materials, "
                     "textures and inlineMaterial entries are ignored",
                     object.id);
    }
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

core::Aabb placeholderBounds() noexcept {
    const float half = 0.5f * kPlaceholderSize;
    return core::Aabb::fromMinMax(glm::vec3(-half, 0.0f, -half), glm::vec3(half, kPlaceholderSize, half));
}

ModelLoader assetModelLoader(std::filesystem::path assetRoot) {
    return [root = std::move(assetRoot)](std::string_view modelPath) -> Expected<SharedModel> {
        auto model = assets::loadGltf(root / core::pathFromUtf8(modelPath));
        if (!model) {
            return propagate(model.error());
        }
        return std::make_shared<const assets::ModelData>(std::move(*model));
    };
}

std::size_t SceneGeometry::partCount() const noexcept {
    std::size_t count = 0;
    for (const SceneObjectGeometry& object : objects) {
        count += object.parts.size();
    }
    return count;
}

std::size_t SceneGeometry::uniqueMeshCount() const noexcept {
    std::set<std::string_view> meshKeys;
    std::set<std::string_view> models;
    std::size_t modelMeshes = 0;
    for (const SceneObjectGeometry& object : objects) {
        for (const ScenePart& part : object.parts) {
            meshKeys.insert(part.meshKey);
        }
        if (object.model && models.insert(object.modelPath).second) {
            // Only the meshes the model draws: collider proxies and meshes outside its active scene are never uploaded.
            std::vector<bool> drawn(object.model->meshes.size(), false);
            for (const assets::ModelDrawItem& item : object.model->drawItems) {
                if (item.meshIndex < drawn.size() && !drawn[item.meshIndex]) {
                    drawn[item.meshIndex] = true;
                    ++modelMeshes;
                }
            }
        }
    }
    return meshKeys.size() + modelMeshes;
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
        std::size_t perInstance = 0;
        for (const ScenePart& part : object.parts) {
            perInstance += part.mesh ? part.mesh->triangleCount() : 0;
        }
        if (object.model) {
            for (const assets::ModelDrawItem& item : object.model->drawItems) {
                perInstance += object.model->meshes[item.meshIndex].triangleCount();
            }
        }
        count += perInstance * object.instances.size();
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

std::size_t SceneGeometry::modelObjectCount() const noexcept {
    std::size_t count = 0;
    for (const SceneObjectGeometry& object : objects) {
        count += object.model ? 1u : 0u;
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

SceneGeometry buildSceneGeometry(const SceneDescription& scene, const assets::MaterialLibrary& materials,
                                 const ModelLoader& loadModel) {
    SceneGeometry geometry;
    geometry.objects.reserve(scene.objects.size());
    std::map<std::string, Expected<Shape>, std::less<>> shapes;
    ModelCache models(loadModel);
    SharedMesh placeholderMesh;
    for (std::size_t index = 0; index < scene.objects.size(); ++index) {
        const SceneObject& object = scene.objects[index];
        SceneObjectGeometry& built = geometry.objects.emplace_back();
        built.id = object.id;
        built.castsShadows = object.castsShadows;
        const std::span<const glm::mat4> transforms = object.instanceTransforms();
        built.instances.assign(transforms.begin(), transforms.end());

        if (object.source == SceneObject::Source::Model) {
            built.modelPath = object.modelPath;
            warnAboutModelMaterials(object);
            const Expected<SharedModel>& model = models.get(object.modelPath);
            if (!model) {
                usePlaceholder(built, object, model.error(), materials, placeholderMesh);
                continue;
            }
            built.model = *model;
            built.bounds = (*model)->bounds;
            built.sockets = (*model)->sockets;
            if (object.collides) {
                built.colliders = modelColliders(**model);
            }
            continue;
        }

        const std::string key = shapeKey(object);
        auto shape = shapes.find(key);
        if (shape == shapes.end()) {
            shape = shapes.emplace(key, buildShape(object)).first;
        }
        if (!shape->second) {
            usePlaceholder(built, object, shape->second.error(), materials, placeholderMesh);
            continue;
        }
        MaterialResolver resolver(object, index, scene.name, materials);
        for (const auto& [role, mesh] : shape->second->meshes) {
            ScenePart& part = built.parts.emplace_back();
            part.mesh = mesh;
            part.meshKey = key + "#" + role;
            part.role = role;
            resolver.resolve(part);
            if (const auto texture = object.texturesByRole.find(part.role); texture != object.texturesByRole.end()) {
                part.texture = texture->second;
            }
        }
        built.bounds = shape->second->bounds;
        built.sockets = shape->second->sockets;
        if (object.collides) {
            built.colliders = shape->second->colliders;
        }
    }
    return geometry;
}

CollisionWorld buildCollisionWorld(const SceneGeometry& geometry) {
    CollisionWorld world;
    for (const SceneObjectGeometry& object : geometry.objects) {
        for (std::size_t instance = 0; instance < object.instances.size(); ++instance) {
            for (std::size_t index = 0; index < object.colliders.size(); ++index) {
                world.addBox(object.colliders[index].box, object.instances[instance],
                             colliderTag(object, instance, index));
            }
        }
    }
    return world;
}

} // namespace rv::scene
