#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// Size of the socket's bounds in the prefab or model (meters): Socket::bounds is in the socket's own space, so a glTF
/// node scaled by 0.01 (a model exported in centimeters) has bounds 100 times its size; each side is scaled by the
/// length of the transform's axis along it. Zero for empty or non-finite bounds.
[[nodiscard]] glm::vec3 socketSize(const Socket& socket) noexcept;

/// Where a slot socket stands in its fixture: "Slot_<level>_<index>" (the shelf's, the display case's and the wall
/// shelf's slots), level 0 lowest (or frontmost), index 0 leftmost.
struct SlotAddress {
    int level = 0;
    int index = 0;
};

/// The address of a slot id: "Slot_" followed by two decimal numbers in int range joined by '_', without sign or
/// leading zeros ("Slot_0_12"); nullopt for any other id.
[[nodiscard]] std::optional<SlotAddress> parseSlotId(std::string_view id) noexcept;

/// The level of a level socket's id: "Level_" and a decimal number as in parseSlotId ("Level_3"); nullopt otherwise.
/// A fixture with slots has one Anchor socket "Level_<level>" per level (node "Anchor_Level_<level>"): its frame on
/// the level's surface, its bounds the volume free above it, where shelf dressing stands when no title needs the level
/// (ADR-0010 §5).
[[nodiscard]] std::optional<int> parseLevelId(std::string_view id) noexcept;

/// The slots of one level of a fixture.
struct SlotLevel {
    int level = 0;
    /// Slots of the level (their indices run from 0 to count - 1 in the prefabs of this module).
    int count = 0;
    /// socketSize() of the level's first slot (the lowest index): the item volume (w, h, d) the level's slots hold.
    glm::vec3 slotSize{0.0f};
    /// Height of the first slot's origin (its bottom center) in the prefab.
    float height = 0.0f;
};

/// The ShelfSlot sockets of `sockets` whose ids parse as slot ids, grouped by level in ascending level order (levels
/// without a slot are absent): what the world generator reads a fixture's capacity from (ADR-0010 §5), never from
/// constants of its own.
[[nodiscard]] std::vector<SlotLevel> slotLevels(std::span<const Socket> sockets);

} // namespace rv::geometry
