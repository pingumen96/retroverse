#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>

namespace rv::geometry {

/// Semantic attachment points of a prefab (style guide §9.2). Procedural prefabs and glTF models expose the
/// same Socket type, so the world generator and the renderer treat both uniformly.
enum class SocketKind {
    Anchor,        ///< "Anchor_<Id>" nodes: placement and interaction targets (Anchor_TV_Stand, Anchor_ScreenLook)
    ScreenSurface, ///< "ScreenSurface" mesh that receives video or images
    ColliderProxy, ///< "ColliderProxy*" simplified collision geometry, never rendered
    ShelfSlot,     ///< procedural shelf slots "Slot_<row>_<index>" that hold a game's physical media
    Other,         ///< named parts such as PowerButton, SpeakerLeft
};

[[nodiscard]] std::string_view toString(SocketKind kind) noexcept;

struct Socket {
    /// Stable identifier: for "Anchor_TV_Stand" the id is "TV_Stand"; for "ScreenSurface" it is "ScreenSurface".
    std::string id;
    /// Source node name (glTF) or generated name (procedural).
    std::string nodeName;
    SocketKind kind = SocketKind::Other;
    /// Transform relative to the prefab/model root (meters).
    glm::mat4 transform{1.0f};
    /// Local extent of the socket when meaningful (screen quad, slot volume); may be empty.
    core::Aabb bounds;
    /// Mesh index for mesh-backed sockets (ScreenSurface, ColliderProxy).
    std::optional<std::uint32_t> meshIndex;
};

/// Linear lookup by id (prefabs carry a handful of sockets).
[[nodiscard]] const Socket* findSocket(std::span<const Socket> sockets, std::string_view id) noexcept;

} // namespace rv::geometry
