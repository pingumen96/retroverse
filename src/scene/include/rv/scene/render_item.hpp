#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <glm/glm.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/scene/instance_data.hpp>

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

/// Passes of a view, in drawing order.
enum class DrawPass : std::uint8_t {
    Opaque, ///< assets::AlphaMode::Opaque
    Mask,   ///< assets::AlphaMode::Mask: alpha-tested, writes depth
    Blend,  ///< assets::AlphaMode::Blend: blended over the other passes, back to front, without depth writes
};

/// Pass of the materials with alpha mode `mode`.
[[nodiscard]] constexpr DrawPass drawPassOf(assets::AlphaMode mode) noexcept {
    switch (mode) {
    case assets::AlphaMode::Opaque:
        return DrawPass::Opaque;
    case assets::AlphaMode::Mask:
        return DrawPass::Mask;
    case assets::AlphaMode::Blend:
        return DrawPass::Blend;
    }
    return DrawPass::Opaque;
}

/// One mesh instance to draw this frame. `model` is the local -> world transform (meters, column vectors); with
/// `attributes` it makes the item's instance record (InstanceData, which the planner lists for the renderer).
/// `castsShadow` is not part of the batch key: renderers plan the shadow pass from the subset of items that
/// cast shadows (with the light's frustum). `attributes` is not part of it either: items of one batch differ in their
/// layers, flags and pick ids; items that are not collection items leave them zero.
struct RenderItem {
    MeshHandle mesh;
    MaterialHandle material;
    ShaderKind shader = ShaderKind::Pbr;
    glm::mat4 model{1.0f};
    bool castsShadow = true;
    InstanceAttributes attributes;
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
