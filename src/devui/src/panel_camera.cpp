#include <memory>
#include <utility>

#include <glm/glm.hpp>
#include <imgui.h>

#include <rv/core/math.hpp>
#include <rv/devui/panels.hpp>
#include <rv/input/input_system.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/first_person_controller.hpp>

#include "widgets.hpp"

namespace rv::devui {
namespace {

/// Field of view range offered: wider than style §6.2's 70-85 degrees, for inspecting.
constexpr float kMinFovDeg = 30.0f;
constexpr float kMaxFovDeg = 120.0f;
constexpr float kMaxSpeed = 30.0f; ///< m/s
constexpr float kMinSensitivity = 0.1f;
constexpr float kMaxSensitivity = 10.0f;

class CameraPanel final : public Panel {
public:
    explicit CameraPanel(CameraPanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return "Camera"; }

    void draw() override {
        widgets::pushLabelWidth("Speed (m/s)"); // the widest label of the panel
        drawPose();
        drawLens();
        drawWalker();
        drawInput();
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Checkbox("Collision", &m_model.collision);
        ImGui::SetItemTooltip("Off: walk through walls and furniture (noclip)");
        if (ImGui::Button("Reset to scene camera")) {
            m_model.resetToSceneCamera();
        }
    }

private:
    void drawPose() {
        scene::FirstPersonController& walker = m_model.walker;
        glm::vec3 feet = walker.feetPosition();
        const float limit = walker.settings().pitchLimitDeg;
        float yawDeg = core::radiansToDegrees(walker.yaw());
        float pitchDeg = core::radiansToDegrees(walker.pitch());
        bool moved =
            ImGui::DragFloat3("Feet (m)", &feet.x, 0.01f, -1000.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        moved |= ImGui::DragFloat("Yaw (deg)", &yawDeg, 0.5f, -180.0f, 180.0f, "%.2f", ImGuiSliderFlags_WrapAround);
        moved |= ImGui::DragFloat("Pitch (deg)", &pitchDeg, 0.25f, -limit, limit, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (moved) {
            walker.teleport(feet, core::degreesToRadians(yawDeg), core::degreesToRadians(pitchDeg));
        }
        const glm::vec3& eye = m_model.camera.position();
        ImGui::TextDisabled("Eye at (%.2f, %.2f, %.2f) m", eye.x, eye.y, eye.z);
    }

    void drawLens() {
        scene::CameraLens lens = m_model.camera.lens();
        if (ImGui::SliderFloat("FOV (deg)", &lens.horizontalFovDeg, kMinFovDeg, kMaxFovDeg, "%.1f")) {
            m_model.camera.setLens(lens);
        }
        ImGui::SetItemTooltip("Horizontal field of view");
    }

    void drawWalker() {
        scene::FirstPersonSettings settings = m_model.walker.settings();
        float speeds[2] = {settings.walkSpeed, settings.sprintSpeed};
        if (ImGui::DragFloat2("Speed (m/s)", speeds, 0.05f, 0.0f, kMaxSpeed, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            settings.walkSpeed = speeds[0];
            settings.sprintSpeed = speeds[1];
            m_model.walker.setSettings(settings);
        }
        ImGui::SetItemTooltip("Walking and sprinting (Left Shift)");
    }

    void drawInput() {
        input::InputSettings settings = m_model.input.settings();
        bool changed = ImGui::SliderFloat("Mouse", &settings.mouseSensitivityScale, kMinSensitivity, kMaxSensitivity,
                                          "x%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Mouse look sensitivity");
        changed |= ImGui::Checkbox("Invert Y", &settings.invertLookY);
        if (changed) {
            m_model.input.setSettings(settings);
        }
    }

    CameraPanelModel m_model;
};

} // namespace

std::unique_ptr<Panel> makeCameraPanel(CameraPanelModel model) {
    return std::make_unique<CameraPanel>(std::move(model));
}

} // namespace rv::devui
