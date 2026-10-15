#include <cstddef>
#include <memory>
#include <string_view>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <rv/devui/panels.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/render/shader_library.hpp>
#include <rv/scene/scene_description.hpp>

#include "widgets.hpp"

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
        ImGui::SetItemTooltip("Draws the scene's triangles as edges");
        ImGui::SameLine();
        drawVsync();

        drawPostProcessing();

        ImGui::SeparatorText("Shaders");
        if (ImGui::Button("Reload shaders (F5)")) {
            spdlog::info("Reloading shaders (developer overlay)");
            m_model.shaders.reloadAll();
        }
        drawShaderStates();
    }

private:
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
        ImGui::SetItemTooltip(
            "Glow of the light above the bloom threshold: screens, lamp shades, highlights (0: none)");
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
