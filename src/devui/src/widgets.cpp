#include "widgets.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <imgui.h>

#include <rv/core/math.hpp>

namespace rv::devui::widgets {

bool linearColor(const char* label, glm::vec3& linear) {
    glm::vec3 encoded = core::linearToSrgb(glm::clamp(linear, glm::vec3(0.0f), glm::vec3(1.0f)));
    if (!ImGui::ColorEdit3(label, &encoded.x, ImGuiColorEditFlags_DisplayHex)) {
        return false;
    }
    linear = core::srgbToLinear(encoded);
    return true;
}

bool linearColor(const char* label, glm::vec4& linear) {
    glm::vec3 rgb(linear);
    glm::vec4 encoded(core::linearToSrgb(glm::clamp(rgb, glm::vec3(0.0f), glm::vec3(1.0f))), linear.a);
    if (!ImGui::ColorEdit4(label, &encoded.x, ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaBar)) {
        return false;
    }
    linear = glm::vec4(core::srgbToLinear(glm::vec3(encoded)), encoded.a);
    return true;
}

bool direction(const char* label, glm::vec3& direction) {
    glm::vec3 edited = direction;
    if (!ImGui::DragFloat3(label, &edited.x, 0.01f, -1.0f, 1.0f, "%.3f")) {
        return false;
    }
    const float length = glm::length(edited);
    if (!std::isfinite(length) || length < 1e-3f) {
        return false;
    }
    direction = edited / length;
    return true;
}

void pushLabelWidth(float labelEms) {
    ImGui::PushItemWidth(-labelEms * ImGui::GetFontSize());
}

bool beginValueTable(const char* id) {
    return ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg);
}

void valueRow(const char* label, std::size_t value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::Text("%zu", value);
}

void bytes(std::size_t bytes) {
    constexpr double kKiB = 1024.0;
    const auto value = static_cast<double>(bytes);
    if (value >= kKiB * kKiB) {
        ImGui::Text("%.1f MiB", value / (kKiB * kKiB));
    } else if (value >= kKiB) {
        ImGui::Text("%.1f KiB", value / kKiB);
    } else {
        ImGui::Text("%zu B", bytes);
    }
}

} // namespace rv::devui::widgets
