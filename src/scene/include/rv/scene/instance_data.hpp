#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace rv::scene {

/// Bits of InstanceAttributes::flags (ADR-0011 §2). Mirrored by the RV_INSTANCE_* constants of
/// assets/shaders/common/common.glsl, which a unit test keeps equal. Items that are not collection items set none.
namespace instance_flags {
/// The item is highlighted (focus, hover).
inline constexpr std::uint32_t kHighlighted = 1u << 0;
/// The item's static instance is not drawn: it is pulled out or inspected and drawn by the dynamic path instead.
inline constexpr std::uint32_t kHidden = 1u << 1;
/// The item's game is unavailable: drawn dimmed.
inline constexpr std::uint32_t kUnavailable = 1u << 2;
} // namespace instance_flags

/// The per-instance values beside the model matrix (ADR-0011 §2), read by the mesh shaders as one `uvec4` at attribute
/// location 8 (pbr/pbr.vert `aInstance`: x frontLayer, y spineLayer, z flags, w pickId). Zero for every item that is
/// not a collection item: layer 0 of each array is the pending picture, no flag is set and pick id 0 names nothing.
struct InstanceAttributes {
    /// Layer of the item's front label in its media family's front array.
    std::uint32_t frontLayer = 0;
    /// Layer of the item's spine in its media family's spine array.
    std::uint32_t spineLayer = 0;
    /// instance_flags bits.
    std::uint32_t flags = 0;
    /// The item's pick id; 0 for none.
    std::uint32_t pickId = 0;

    bool operator==(const InstanceAttributes&) const = default;
};

/// The 80-byte instance record of every instanced draw (ADR-0011 §2): the model matrix (local -> world, column-major,
/// attribute locations 4..7), then InstanceAttributes (location 8). scene::BatchPlan lists one per planned item,
/// gl::InstanceBuffer stores them as they are and gl::GpuMesh reads them with a stride of sizeof(InstanceData).
struct InstanceData {
    glm::mat4 model{1.0f};
    InstanceAttributes attributes;

    bool operator==(const InstanceData&) const = default;
};

static_assert(sizeof(InstanceAttributes) == 16, "InstanceAttributes is the uvec4 at attribute location 8");
static_assert(sizeof(InstanceData) == 80, "the instance record is 80 bytes (ADR-0011 §2)");
static_assert(offsetof(InstanceData, model) == 0 && offsetof(InstanceData, attributes) == 64,
              "the matrix comes first, its attributes right after it");
static_assert(offsetof(InstanceAttributes, frontLayer) == 0 && offsetof(InstanceAttributes, spineLayer) == 4 &&
                  offsetof(InstanceAttributes, flags) == 8 && offsetof(InstanceAttributes, pickId) == 12,
              "x frontLayer, y spineLayer, z flags, w pickId");

} // namespace rv::scene
