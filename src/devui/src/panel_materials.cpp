#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <rv/devui/panels.hpp>
#include <rv/devui/texture_previews.hpp>
#include <rv/devui/widgets.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/material_uniforms.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::devui {
namespace {

constexpr float kMaxEmissiveStrength = 100.0f;
constexpr std::uint32_t kTextureBits =
    gl::material_flags::kBaseColorTexture | gl::material_flags::kMetallicRoughnessTexture |
    gl::material_flags::kNormalTexture | gl::material_flags::kOcclusionTexture | gl::material_flags::kEmissiveTexture;

const char* shaderName(scene::ShaderKind shader) noexcept {
    return shader == scene::ShaderKind::Unlit ? "unlit" : "pbr";
}

const char* alphaModeName(assets::AlphaMode mode) noexcept {
    switch (mode) {
    case assets::AlphaMode::Opaque:
        return "opaque";
    case assets::AlphaMode::Mask:
        return "mask";
    case assets::AlphaMode::Blend:
        return "blend";
    }
    return "?";
}

/// The factors a material draws with now (its uniforms), for editing.
gl::MaterialFactors currentFactors(const gl::MaterialUniforms& uniforms) noexcept {
    return gl::MaterialFactors{uniforms.baseColorFactor, uniforms.metallic, uniforms.roughness,
                               uniforms.emissiveStrength};
}

/// Names of the material texture slots, in gl::MaterialTextureSlot order.
constexpr std::array<const char*, gl::kMaterialTextureSlotCount> kSlotNames{"Base color", "Metallic-roughness",
                                                                            "Normal", "Occlusion", "Emissive"};
/// Side of a texture thumbnail, in font sizes.
constexpr float kThumbnailEms = 3.0f;

class MaterialsPanel final : public Panel {
public:
    explicit MaterialsPanel(const MaterialsPanelModel& model) : m_model(model), m_previews(model.textures) {}

    [[nodiscard]] const char* title() const noexcept override { return "Materials"; }

    void draw() override {
        gl::MaterialRegistry& materials = m_model.materials;
        countUsers();
        ImGui::Text("%zu materials", materials.size());
        ImGui::SetItemTooltip("Edits apply to the GPU at once and are never saved");
        if (materials.size() == 0) {
            return;
        }
        drawList();
        if (m_selected < materials.size()) {
            drawEditor(m_selected);
        }
    }

private:
    /// Every material with the number of render items that use it; a click selects one for the editor.
    void drawList() {
        const gl::MaterialRegistry& materials = m_model.materials;
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("list", 2, kFlags, ImVec2(0.0f, 8.0f * ImGui::GetTextLineHeightWithSpacing()))) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(materials.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto index = static_cast<std::size_t>(row);
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const gl::GpuMaterial& material = materials.material(handle(index));
                if (ImGui::Selectable(material.name.c_str(), index == m_selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    m_selected = index;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", m_users[index]);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    static scene::MaterialHandle handle(std::size_t index) noexcept {
        return scene::MaterialHandle{static_cast<std::uint32_t>(index)};
    }

    /// Render items per material (the registry's contents and the items may change from frame to frame: the asset
    /// preview reloads its model).
    void countUsers() {
        m_users.assign(m_model.materials.size(), 0);
        for (const scene::RenderItem& item : m_model.items()) {
            if (item.material.value < m_users.size()) {
                ++m_users[item.material.value];
            }
        }
    }

    void drawEditor(std::size_t index) {
        gl::MaterialRegistry& materials = m_model.materials;
        const gl::GpuMaterial& material = materials.material(handle(index));
        ImGui::SeparatorText(material.name.c_str());
        ImGui::TextDisabled("%s, %s%s", shaderName(material.shader), alphaModeName(material.alphaMode),
                            material.doubleSided ? ", double-sided" : "");
        drawTextures(material);
        gl::MaterialFactors factors = currentFactors(material.uniforms);
        widgets::pushLabelWidth("Base color"); // the widest label below
        bool changed = widgets::linearColor("Base color", factors.baseColor);
        changed |= ImGui::SliderFloat("Metallic", &factors.metallic, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        changed |=
            ImGui::SliderFloat("Roughness", &factors.roughness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::DragFloat("Emissive", &factors.emissiveStrength, 0.01f, 0.0f, kMaxEmissiveStrength, "x%.3f",
                                    ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Emissive strength: scales the emissive color (pbr) or the color (unlit)");
        ImGui::PopItemWidth();
        if (ImGui::Button("Revert")) {
            factors = gl::materialFactors(material.source);
            changed = true;
        }
        ImGui::SetItemTooltip("Back to the values the material was loaded with");
        if (changed) {
            if (auto applied = materials.setFactors(handle(index), factors); !applied) {
                spdlog::warn("Material '{}' cannot be edited: {}", material.name, applied.error().toString());
            }
        }
    }

    /// A thumbnail of every texture of `material` with its slot and size.
    void drawTextures(const gl::GpuMaterial& material) {
        if ((material.uniforms.flags & kTextureBits) == 0) {
            ImGui::TextDisabled("No textures");
            return;
        }
        if (!ImGui::BeginTable("textures", 2, ImGuiTableFlags_SizingFixedFit)) {
            return;
        }
        const float side = kThumbnailEms * ImGui::GetFontSize();
        for (std::size_t slot = 0; slot < gl::kMaterialTextureSlotCount; ++slot) {
            const gl::Texture2D* texture = material.textureSources[slot];
            if (texture == nullptr) {
                continue;
            }
            ImGui::PushID(static_cast<int>(slot));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            m_previews.thumbnail(*texture, side, kSlotNames[slot]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kSlotNames[slot]);
            const glm::ivec2 size = texture->size();
            ImGui::TextDisabled("%d x %d %s", size.x, size.y,
                                texture->desc().colorSpace == gl::ColorSpace::Srgb ? "sRGB" : "linear");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    MaterialsPanelModel m_model;
    TexturePreviews m_previews;
    std::vector<std::size_t> m_users;
    std::size_t m_selected = 0;
};

} // namespace

std::unique_ptr<Panel> makeMaterialsPanel(const MaterialsPanelModel& model) {
    return std::make_unique<MaterialsPanel>(model);
}

} // namespace rv::devui
