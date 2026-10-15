#include <cstddef>
#include <memory>
#include <string_view>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <rv/devui/panels.hpp>
#include <rv/devui/widgets.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/render/shader_library.hpp>
#include <rv/render/shadow_settings.hpp>
#include <rv/render/ssao_settings.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::devui {
namespace {

constexpr ImVec4 kOkColor{0.55f, 0.85f, 0.45f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr float kMinExposure = 0.01f;
constexpr float kMaxExposure = 16.0f;

class RenderingPanel final : public Panel {
public:
    explicit RenderingPanel(const RenderingPanelModel& model) : m_model(model) {}

    [[nodiscard]] const char* title() const noexcept override { return "Rendering"; }

    void draw() override {
        ImGui::SeparatorText("Debug lines");
        ImGui::Checkbox("Grid", &m_model.showGrid);
        ImGui::SetItemTooltip("World grid on the floor (y = 0)");
        ImGui::SameLine();
        ImGui::Checkbox("Bounds", &m_model.showBounds);
        ImGui::SetItemTooltip("Bounds of every object");
        ImGui::SameLine();
        ImGui::Checkbox("Sockets", &m_model.showSockets);
        ImGui::SetItemTooltip("Socket axes: X red, Y green, Z blue");

        ImGui::SeparatorText("Frame");
        ImGui::Checkbox("Wireframe", &m_model.wireframe);
        ImGui::SetItemTooltip("Draws the scene's triangles as edges (without ambient occlusion)");
        ImGui::SameLine();
        ImGui::Checkbox("FXAA", &m_model.antialiasing);
        ImGui::SetItemTooltip("Anti-aliasing: smooths the stair-steps of edges after the tonemap");
        ImGui::SameLine();
        drawVsync();

        drawShadows();
        drawAmbientOcclusion();
        drawPostProcessing();

        ImGui::SeparatorText("Shaders");
        if (ImGui::Button("Reload shaders (F5)")) {
            spdlog::info("Reloading shaders (developer overlay)");
            m_model.shaders.reloadAll();
        }
        drawShaderStates();
    }

private:
    /// The key light's shadow map: on or off, its biases (renderer tuning, the defaults until edited).
    void drawShadows() {
        ImGui::SeparatorText("Shadows");
        gl::ShadowSettings& shadows = m_model.shadows;
        ImGui::Checkbox("Key light shadow", &shadows.enabled);
        ImGui::SetItemTooltip("Shadow map of the first light that casts shadows (Lights panel)");
        widgets::pushLabelWidth("Normal offset"); // the widest label of the section
        ImGui::SliderFloat("Slope bias", &shadows.slopeBias, 0.0f, gl::kMaxShadowDepthBias, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Depth bias of the casters by their slope in the map (against acne on grazing light)");
        ImGui::SliderFloat("Constant bias", &shadows.constantBias, 0.0f, gl::kMaxShadowDepthBias, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Depth bias of the casters in the smallest depth steps of the map");
        ImGui::SliderFloat("Normal offset", &shadows.normalOffsetTexels, 0.0f, gl::kMaxShadowNormalOffsetTexels,
                           "%.2f texels", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Receivers look the map up this far along their normal (too much detaches the "
                              "shadows from their casters)");
        ImGui::PopItemWidth();
    }

    /// The ambient occlusion of the scene's environment (the scene file's values until edited).
    void drawAmbientOcclusion() {
        ImGui::SeparatorText("Ambient occlusion");
        scene::SsaoSetup& ssao = m_model.environment.ssao;
        ImGui::Checkbox("SSAO", &ssao.enabled);
        ImGui::SetItemTooltip("Darkens the ambient light in corners and where objects meet");
        widgets::pushLabelWidth("Intensity"); // the widest label of the section
        ImGui::SliderFloat("Radius", &ssao.radius, gl::kMinSsaoRadius, 2.0f, "%.2f m",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("How far from a surface occluders darken it");
        ImGui::SliderFloat("Intensity", &ssao.intensity, 0.0f, scene::kMaxSsaoIntensity, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Exponent of the unoccluded share of the ambient light (0: no darkening)");
        ImGui::PopItemWidth();
    }

    /// The post-processing of the scene's environment (the scene file's values until edited).
    void drawPostProcessing() {
        ImGui::SeparatorText("Post-processing");
        scene::EnvironmentSetup& environment = m_model.environment;
        widgets::pushLabelWidth("Bloom strength"); // the widest label of the section
        ImGui::DragFloat("Exposure", &environment.exposure, 0.01f, kMinExposure, kMaxExposure, "%.3f",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Multiplies the scene radiance before the ACES tonemap");
        ImGui::SliderFloat("Bloom strength", &environment.bloomStrength, 0.0f, 1.0f, "%.3f",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Share of the light of screens, lamp shades, neon and bright highlights that glows "
                              "around them (0: none)");
        ImGui::SliderFloat("Bloom radius", &environment.bloomRadius, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("How far the glow spreads: from a few pixels (0) to most of the view (1)");
        ImGui::SliderFloat("Vignette", &environment.vignette, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Darkening of the corners (0: none)");
        ImGui::PopItemWidth();
    }

    void drawVsync() {
        platform::GraphicsWindow& window = m_model.window;
        bool vsync = window.vsync() != platform::VsyncMode::Off;
        if (ImGui::Checkbox("Vsync", &vsync)) {
            if (auto mode = window.setVsync(vsync); mode) {
                spdlog::info("Vsync {}", platform::toString(*mode));
            } else {
                spdlog::warn("Vsync unchanged: {}", mode.error().toString());
            }
        }
        const std::string_view mode = platform::toString(window.vsync());
        ImGui::SetItemTooltip("Swap interval: %.*s", static_cast<int>(mode.size()), mode.data());
    }

    void drawShaderStates() const {
        const gl::ShaderLibrary& shaders = m_model.shaders;
        for (std::size_t index = 0; index < shaders.size(); ++index) {
            const gl::ShaderStatus status = shaders.status(index);
            const bool failed = !status.lastError.empty();
            ImGui::TextColored(failed ? kErrorColor : kOkColor, "%s", failed ? "failed" : "ok");
            ImGui::SameLine();
            ImGui::Text("%.*s", static_cast<int>(status.name.size()), status.name.data());
            if (failed) {
                if (status.valid) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the previous version draws)");
                }
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::TextWrapped("%.*s", static_cast<int>(status.lastError.size()), status.lastError.data());
                ImGui::PopStyleColor();
            }
        }
    }

    RenderingPanelModel m_model;
};

} // namespace

std::unique_ptr<Panel> makeRenderingPanel(const RenderingPanelModel& model) {
    return std::make_unique<RenderingPanel>(model);
}

} // namespace rv::devui
