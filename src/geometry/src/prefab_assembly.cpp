#include "prefab_assembly.hpp"

#include <cstddef>
#include <cstdint>
#include <format>

namespace rv::geometry::detail {

PrefabAssembler::PrefabAssembler(std::string_view kind, std::initializer_list<std::string_view> roles) : m_kind(kind) {
    for (const std::string_view role : roles) {
        m_parts.emplace_back(std::string(role), MeshBuilder{});
    }
}

MeshBuilder& PrefabAssembler::part(std::string_view role) {
    for (auto& [name, builder] : m_parts) {
        if (name == role) {
            return builder;
        }
    }
    return m_parts.emplace_back(std::string(role), MeshBuilder{}).second;
}

void PrefabAssembler::addSocket(std::string id, SocketKind kind, const glm::mat4& transform, const core::Aabb& bounds,
                                std::string_view meshRole) {
    Socket socket;
    socket.nodeName = kind == SocketKind::Anchor ? "Anchor_" + id : id;
    socket.id = std::move(id);
    socket.kind = kind;
    socket.transform = transform;
    socket.bounds = bounds;
    m_sockets.push_back(std::move(socket));
    m_socketMeshRoles.emplace_back(meshRole);
}

void PrefabAssembler::setColliders(std::vector<PrefabCollider> colliders) {
    m_colliders = std::move(colliders);
}

ProceduralPrefab PrefabAssembler::finish() && {
    ProceduralPrefab prefab;
    prefab.kind = m_kind;
    for (auto& [role, builder] : m_parts) {
        if (builder.empty()) {
            continue;
        }
        PrefabPart part;
        part.mesh = std::move(builder).build(m_kind + "/" + role);
        part.materialRole = role;
        prefab.bounds.expand(part.mesh.bounds);
        prefab.parts.push_back(std::move(part));
    }
    // Mesh indices refer to the final part list, after empty parts were dropped.
    for (std::size_t i = 0; i < m_sockets.size(); ++i) {
        const std::string& meshRole = m_socketMeshRoles[i];
        for (std::size_t index = 0; !meshRole.empty() && index < prefab.parts.size(); ++index) {
            if (prefab.parts[index].materialRole == meshRole) {
                m_sockets[i].meshIndex = static_cast<std::uint32_t>(index);
                break;
            }
        }
    }
    prefab.sockets = std::move(m_sockets);
    if (m_colliders) {
        prefab.colliders = std::move(*m_colliders);
    } else if (!prefab.bounds.isEmpty()) {
        prefab.colliders.push_back(PrefabCollider{{}, prefab.bounds});
    }
    return prefab;
}

glm::mat4 translation(const glm::vec3& offset) {
    glm::mat4 m(1.0f);
    m[3] = glm::vec4(offset, 1.0f);
    return m;
}

glm::mat4 frameFacing(const glm::vec3& forward, const glm::vec3& position) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::cross(up, forward);
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(right, 0.0f);
    m[1] = glm::vec4(up, 0.0f);
    m[2] = glm::vec4(forward, 0.0f);
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

std::string formatNumber(float value) {
    return std::format("{}", value);
}

tl::unexpected<Error> outOfRange(std::string_view kind, std::string_view key, std::string message) {
    std::string context(kind);
    context += '.';
    context += key;
    return makeError(ErrorCode::OutOfRange, std::move(message), std::move(context));
}

Status checkRange(std::string_view kind, std::string_view key, float value, float min, float max) {
    if (value >= min && value <= max) {
        return ok();
    }
    return outOfRange(kind, key,
                      formatNumber(value) + " is outside [" + formatNumber(min) + ", " + formatNumber(max) + "]");
}

Status checkRange(std::string_view kind, std::string_view key, int value, int min, int max) {
    if (value >= min && value <= max) {
        return ok();
    }
    return outOfRange(kind, key,
                      std::to_string(value) + " is outside [" + std::to_string(min) + ", " + std::to_string(max) + "]");
}

Status checkRange(std::string_view kind, std::string_view key, const glm::vec2& value, const glm::vec2& min,
                  const glm::vec2& max) {
    for (int i = 0; i < 2; ++i) {
        if (!(value[i] >= min[i] && value[i] <= max[i])) {
            return outOfRange(kind, key,
                              "component " + std::to_string(i) + " = " + formatNumber(value[i]) + " is outside [" +
                                  formatNumber(min[i]) + ", " + formatNumber(max[i]) + "]");
        }
    }
    return ok();
}

Status checkRange(std::string_view kind, std::string_view key, const glm::vec3& value, const glm::vec3& min,
                  const glm::vec3& max) {
    for (int i = 0; i < 3; ++i) {
        if (!(value[i] >= min[i] && value[i] <= max[i])) {
            return outOfRange(kind, key,
                              "component " + std::to_string(i) + " = " + formatNumber(value[i]) + " is outside [" +
                                  formatNumber(min[i]) + ", " + formatNumber(max[i]) + "]");
        }
    }
    return ok();
}

Status firstError(std::initializer_list<Status> checks) {
    for (const Status& check : checks) {
        if (!check) {
            return check;
        }
    }
    return ok();
}

} // namespace rv::geometry::detail
