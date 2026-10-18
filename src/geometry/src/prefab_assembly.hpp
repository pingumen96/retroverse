#pragma once

// Internal helpers shared by the procedural prefab builders. Not installed, not part of the API.

#include <deque>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/mesh_builder.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>

namespace rv::geometry::detail {

/// Minimum distance between coplanar surfaces that face the same way (labels, screens, decals): keeps them
/// apart in a 24-bit depth buffer at room distances.
inline constexpr float kDecalOffset = 0.0005f;

/// Meters per uv unit of wooden parts, laid out with BoxUv::GrainMeters (u along each board): wood textures repeat
/// every meter at the same scale on every piece of furniture and trim.
inline constexpr float kWoodMetersPerTile = 1.0f;

/// Collects geometry per material role and the sockets of one prefab.
class PrefabAssembler {
public:
    /// `roles` fixes the order of the parts in the result.
    PrefabAssembler(std::string_view kind, std::initializer_list<std::string_view> roles);

    /// Builder that receives the geometry of `role` (an undeclared role is appended after the declared ones).
    /// References stay valid for the assembler's lifetime.
    MeshBuilder& part(std::string_view role);

    /// Adds a socket; the node name follows the glTF naming rules ("Anchor_<id>" for anchors, the id otherwise).
    /// A mesh-backed socket (ScreenSurface) names the role of its mesh in `meshRole`; finish() turns it into
    /// Socket::meshIndex, the index of that role's part.
    void addSocket(std::string id, SocketKind kind, const glm::mat4& transform, const core::Aabb& bounds = {},
                   std::string_view meshRole = {});

    /// Declares the prefab's collision boxes; an empty list declares a prefab that never blocks. Without a call,
    /// finish() makes the overall bounds the only collider.
    void setColliders(std::vector<PrefabCollider> colliders);

    /// Builds one mesh "<kind>/<role>" per non-empty role, the overall bounds, the socket mesh indices and the
    /// colliders.
    [[nodiscard]] ProceduralPrefab finish() &&;

private:
    std::string m_kind;
    std::deque<std::pair<std::string, MeshBuilder>> m_parts;
    std::vector<Socket> m_sockets;
    /// Mesh role of each socket of m_sockets, empty when the socket is not mesh-backed.
    std::vector<std::string> m_socketMeshRoles;
    /// Declared colliders; nothing declared means "the bounds".
    std::optional<std::vector<PrefabCollider>> m_colliders;
};

[[nodiscard]] glm::mat4 translation(const glm::vec3& offset);

/// Rigid frame at `position` whose +Z points along the horizontal unit vector `forward` and +Y up.
[[nodiscard]] glm::mat4 frameFacing(const glm::vec3& forward, const glm::vec3& position);

/// Rigid frame at `position` leaning back by `radians` about +X: +Y = (0, cos, -sin), +Z = (0, sin, cos), the frame of
/// a face-out item leaning against a panel or a wall behind it.
[[nodiscard]] glm::mat4 leaningBack(float radians, const glm::vec3& position);

/// `frame` applied to the point (0, y, z): where a point of a leaning frame lies.
[[nodiscard]] glm::vec3 framePoint(const glm::mat4& frame, float y, float z);

/// Shortest round-trip text of a float, for error messages ("0.125", "1e-05").
[[nodiscard]] std::string formatNumber(float value);

/// OutOfRange error with context "<kind>.<key>".
[[nodiscard]] tl::unexpected<Error> outOfRange(std::string_view kind, std::string_view key, std::string message);

/// Inclusive range checks (NaN always fails), reporting "<value> is outside [min, max]" for "<kind>.<key>".
[[nodiscard]] Status checkRange(std::string_view kind, std::string_view key, float value, float min, float max);
[[nodiscard]] Status checkRange(std::string_view kind, std::string_view key, int value, int min, int max);
[[nodiscard]] Status checkRange(std::string_view kind, std::string_view key, const glm::vec2& value,
                                const glm::vec2& min, const glm::vec2& max);
[[nodiscard]] Status checkRange(std::string_view kind, std::string_view key, const glm::vec3& value,
                                const glm::vec3& min, const glm::vec3& max);

/// The first failed status of `checks` in order (all of them are evaluated), or success.
[[nodiscard]] Status firstError(std::initializer_list<Status> checks);

} // namespace rv::geometry::detail
