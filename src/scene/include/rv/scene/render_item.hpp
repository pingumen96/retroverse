#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <glm/glm.hpp>

namespace rv::scene {

/// Opaque index of a mesh in the renderer's resource registry. The scene layer only compares and hashes it.
struct MeshHandle {
    std::uint32_t value = 0;

    auto operator<=>(const MeshHandle&) const = default;
};

/// Opaque index of a material in the renderer's resource registry.
struct MaterialHandle {
    std::uint32_t value = 0;

    auto operator<=>(const MaterialHandle&) const = default;
};

/// Shader family an item is drawn with (style §25: PBR_Base and Unlit_Emissive in M0).
enum class ShaderKind : std::uint8_t { Pbr, Unlit };

/// One mesh instance to draw this frame. `model` is the local -> world transform (meters, column vectors).
/// `castsShadow` is not part of the batch key: renderers plan the shadow pass from the subset of items that
/// cast shadows (with the light's frustum).
struct RenderItem {
    MeshHandle mesh;
    MaterialHandle material;
    ShaderKind shader = ShaderKind::Pbr;
    glm::mat4 model{1.0f};
    bool castsShadow = true;
};

} // namespace rv::scene

template <>
struct std::hash<rv::scene::MeshHandle> {
    std::size_t operator()(const rv::scene::MeshHandle& handle) const noexcept {
        return std::hash<std::uint32_t>{}(handle.value);
    }
};

template <>
struct std::hash<rv::scene::MaterialHandle> {
    std::size_t operator()(const rv::scene::MaterialHandle& handle) const noexcept {
        return std::hash<std::uint32_t>{}(handle.value);
    }
};
