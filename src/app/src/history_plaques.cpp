#include <rv/app/history_plaques.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

namespace rv::app {

namespace {

constexpr std::string_view kHeroSign = "/fx:hero.sign";

/// The plaque's box in its own frame: centred on its pivot across, from the wall (z 0) out to its depth.
constexpr glm::vec3 kLocalMin{-0.5F * kPlaqueWidth, -0.5F * kPlaqueHeight, 0.0F};
constexpr glm::vec3 kLocalMax{0.5F * kPlaqueWidth, 0.5F * kPlaqueHeight, kPlaqueDepth};

} // namespace

std::vector<PlaqueSite> plaqueSites(const scene::SceneDescription& description) {
    std::vector<PlaqueSite> sites;
    for (const scene::SceneObject& object : description.objects) {
        if (!object.id.ends_with(kHeroSign)) {
            continue;
        }
        const std::string_view roomText = std::string_view(object.id).substr(0, object.id.size() - kHeroSign.size());
        auto room = world::RoomId::parse(roomText);
        if (!room || room->kind() != world::RoomIdKind::System) {
            continue;
        }
        sites.push_back(PlaqueSite{*room, glm::translate(object.transform, kPlaqueOffset)});
    }
    return sites;
}

std::string plaqueId(const world::RoomId& room) {
    return std::string(kPlaquePrefix) + room.toString();
}

std::optional<world::RoomId> plaqueRoom(std::string_view objectId) {
    if (!objectId.starts_with(kPlaquePrefix)) {
        return std::nullopt;
    }
    auto room = world::RoomId::parse(objectId.substr(kPlaquePrefix.size()));
    if (!room) {
        return std::nullopt;
    }
    return *room;
}

scene::SceneObject plaqueObject(const PlaqueSite& site) {
    scene::SceneObject object;
    object.id = plaqueId(site.room);
    object.source = scene::SceneObject::Source::Prefab;
    object.kind = "poster_frame";
    object.params = nlohmann::json{
        {"width", kPlaqueWidth}, {"height", kPlaqueHeight}, {"frameWidth", kPlaqueFrame}, {"depth", kPlaqueDepth}};
    object.transform = site.transform;
    object.materialsByRole = {{"frame", "MAT_Metal_Brass"}, {"picture", "MAT_Paper_Print"}};
    scene::TextureSource picture;
    picture.kind = scene::TextureSource::Kind::Pattern;
    picture.pattern = "cover";
    picture.title = "PLAQUE";
    object.texturesByRole.emplace("picture", std::move(picture));
    object.castsShadows = false;
    object.collides = false;
    return object;
}

std::optional<float> plaqueHit(const glm::mat4& transform, const core::Ray& ray) {
    const glm::mat4 toLocal = glm::inverse(transform);
    const glm::vec3 origin(toLocal * glm::vec4(ray.origin, 1.0F));
    const glm::vec3 direction(toLocal * glm::vec4(ray.direction, 0.0F));
    // The slabs.
    float near = 0.0F;
    float far = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1.0e-8F) {
            if (origin[axis] < kLocalMin[axis] || origin[axis] > kLocalMax[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float t0 = (kLocalMin[axis] - origin[axis]) / direction[axis];
        float t1 = (kLocalMax[axis] - origin[axis]) / direction[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        near = std::max(near, t0);
        far = std::min(far, t1);
        if (near > far) {
            return std::nullopt;
        }
    }
    // The transform is rigid: the local distance is the scene's.
    if (near > kPlaquePickDistance) {
        return std::nullopt;
    }
    return near;
}

core::Aabb plaqueBounds(const glm::mat4& transform) {
    std::array<glm::vec3, 8> corners{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const glm::vec3 local((i & 1U) != 0 ? kLocalMax.x : kLocalMin.x, (i & 2U) != 0 ? kLocalMax.y : kLocalMin.y,
                              (i & 4U) != 0 ? kLocalMax.z : kLocalMin.z);
        corners[i] = glm::vec3(transform * glm::vec4(local, 1.0F));
    }
    return core::Aabb::fromPoints(corners);
}

std::optional<assets::PlaqueText> plaqueText(const SignSystem& system) {
    if (system.description.empty()) {
        return std::nullopt;
    }
    return assets::PlaqueText{system.name, system.maker, system.year, system.type, system.description};
}

} // namespace rv::app
