#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <rv/devui/panels.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/material_uniforms.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/render_item.hpp>

#include "widgets.hpp"

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

class MaterialsPanel final : public Panel {
public:
    explicit MaterialsPanel(const MaterialsPanelModel& model) : m_model(model) {}

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

    /// Render items per material; counted again only when the registry has grown.
    void countUsers() {
        if (m_users.size() == m_model.materials.size()) {
            return;
        }
        m_users.assign(m_model.materials.size(), 0);
        for (const scene::RenderItem& item : m_model.items) {
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
        ImGui::TextDisabled("Textures:%s%s%s%s%s%s",
                            textureFlag(material, gl::material_flags::kBaseColorTexture, " base color"),
                            textureFlag(material, gl::material_flags::kMetallicRoughnessTexture, " metal-rough"),
                            textureFlag(material, gl::material_flags::kNormalTexture, " normal"),
                            textureFlag(material, gl::material_flags::kOcclusionTexture, " occlusion"),
                            textureFlag(material, gl::material_flags::kEmissiveTexture, " emissive"),
                            (material.uniforms.flags & kTextureBits) == 0 ? " none" : "");
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

    static const char* textureFlag(const gl::GpuMaterial& material, std::uint32_t flag, const char* name) noexcept {
        return (material.uniforms.flags & flag) != 0 ? name : "";
    }

    MaterialsPanelModel m_model;
    std::vector<std::size_t> m_users;
    std::size_t m_selected = 0;
};

} // namespace

std::unique_ptr<Panel> makeMaterialsPanel(const MaterialsPanelModel& model) {
    return std::make_unique<MaterialsPanel>(model);
}

} // namespace rv::devui
