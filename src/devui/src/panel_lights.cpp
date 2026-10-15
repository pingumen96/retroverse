#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include <glm/glm.hpp>
#include <imgui.h>

#include <rv/devui/panels.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/scene/light.hpp>
#include <rv/scene/scene_description.hpp>

#include "widgets.hpp"

namespace rv::devui {
namespace {

constexpr float kMaxIntensity = 1000.0f;
constexpr float kMinRange = 0.01f; ///< meters
constexpr float kMaxRange = 100.0f;
/// Spot cones stay below 90 degrees (scene::Light).
constexpr float kMaxConeDeg = 89.9f;
constexpr float kMaxAmbientIntensity = 10.0f;
constexpr float kMinExposure = 0.01f;
constexpr float kMaxExposure = 16.0f;
/// Indent of a light's editor under its name, in font sizes: narrow, the docked column has little width to spare.
constexpr float kLightIndentEms = 0.5f;
constexpr ImVec4 kWarningColor{1.0f, 0.75f, 0.3f, 1.0f};

class LightsPanel final : public Panel {
public:
    explicit LightsPanel(LightsPanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return "Lights"; }

    void draw() override {
        bool changed = drawEnvironment();
        ImGui::SeparatorText("Lights");
        if (m_model.lights.empty()) {
            ImGui::TextDisabled("The scene has no lights");
        }
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, kLightIndentEms * ImGui::GetFontSize());
        for (std::size_t index = 0; index < m_model.lights.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            changed |= drawLight(m_model.lights[index]);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        for (const gl::DroppedLight& dropped : m_model.dropped) {
            ImGui::TextColored(kWarningColor, "Not drawn: %s (%s)", dropped.name.c_str(), dropped.reason.c_str());
        }
        if (changed) {
            m_model.apply();
        }
    }

private:
    bool drawEnvironment() {
        if (!ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            return false;
        }
        scene::EnvironmentSetup& environment = m_model.environment;
        widgets::pushLabelWidth("Background"); // the widest label of the section
        bool changed = ImGui::DragFloat("Exposure", &environment.exposure, 0.01f, kMinExposure, kMaxExposure, "%.3f",
                                        ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        changed |= widgets::linearColor("Background", environment.clearColor);
        ImGui::SetItemTooltip("Radiance where nothing is drawn, exposed and tonemapped like the surfaces");
        ImGui::SeparatorText("Ambient");
        changed |= widgets::linearColor("Sky", environment.ambientSky);
        ImGui::SetItemTooltip("Hemispheric ambient: the color from above");
        changed |= widgets::linearColor("Ground", environment.ambientGround);
        ImGui::SetItemTooltip("Hemispheric ambient: the color from below");
        changed |= ImGui::DragFloat("Intensity", &environment.ambientIntensity, 0.005f, 0.0f, kMaxAmbientIntensity,
                                    "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopItemWidth();
        return changed;
    }

    static bool drawLight(scene::Light& light) {
        const std::string_view type = scene::toString(light.type);
        const char* name = light.name.empty() ? "(unnamed)" : light.name.c_str();
        if (!ImGui::TreeNode("light", "%s (%.*s)", name, static_cast<int>(type.size()), type.data())) {
            return false;
        }
        // Vectors take the whole width, their labels above them: three values beside a label do not fit a column.
        bool changed = false;
        if (light.type != scene::LightType::Directional) {
            widgets::labelAbove("Position (m)");
            changed |= ImGui::DragFloat3("##position", &light.position.x, 0.01f, -1000.0f, 1000.0f, "%.3f",
                                         ImGuiSliderFlags_AlwaysClamp);
        }
        if (light.type != scene::LightType::Point) {
            widgets::labelAbove("Direction");
            changed |= widgets::direction("##direction", light.direction);
        }
        widgets::pushLabelWidth("Outer cone"); // the widest label below
        changed |= widgets::linearColor("Color", light.color);
        changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, kMaxIntensity, "%.3f",
                                    ImGuiSliderFlags_AlwaysClamp);
        if (light.type != scene::LightType::Directional) {
            changed |= ImGui::DragFloat("Range (m)", &light.range, 0.05f, kMinRange, kMaxRange, "%.2f",
                                        ImGuiSliderFlags_AlwaysClamp);
        }
        if (light.type == scene::LightType::Spot) {
            changed |= ImGui::DragFloat("Inner cone", &light.innerConeDeg, 0.25f, 0.0f, light.outerConeDeg, "%.1f deg",
                                        ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("Outer cone", &light.outerConeDeg, 0.25f, light.innerConeDeg, kMaxConeDeg,
                                        "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::PopItemWidth();
        ImGui::Checkbox("Casts shadows", &light.castsShadows);
        ImGui::SetItemTooltip("Kept for the shadow pass (not drawn yet)");
        ImGui::TreePop();
        return changed;
    }

    LightsPanelModel m_model;
};

} // namespace

std::unique_ptr<Panel> makeLightsPanel(LightsPanelModel model) {
    return std::make_unique<LightsPanel>(std::move(model));
}

} // namespace rv::devui
