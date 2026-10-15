#include <array>
#include <cstddef>
#include <memory>

#include <imgui.h>
#include <spdlog/common.h>

#include <rv/devui/log_view.hpp>
#include <rv/devui/panels.hpp>

namespace rv::devui {
namespace {

/// Levels offered by the filter, and the names it shows.
constexpr std::array<spdlog::level::level_enum, 6> kLevels{spdlog::level::trace, spdlog::level::debug,
                                                           spdlog::level::info,  spdlog::level::warn,
                                                           spdlog::level::err,   spdlog::level::critical};
constexpr std::array<const char*, 6> kLevelNames{"trace", "debug", "info", "warning", "error", "critical"};

ImVec4 levelColor(spdlog::level::level_enum level) noexcept {
    switch (level) {
    case spdlog::level::trace:
    case spdlog::level::debug:
        return {0.6f, 0.6f, 0.6f, 1.0f};
    case spdlog::level::warn:
        return {1.0f, 0.8f, 0.35f, 1.0f};
    case spdlog::level::err:
        return {1.0f, 0.45f, 0.4f, 1.0f};
    case spdlog::level::critical:
        return {1.0f, 0.35f, 0.85f, 1.0f};
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

class LogPanel final : public Panel {
public:
    [[nodiscard]] const char* title() const noexcept override { return "Log"; }

    void draw() override {
        m_view.refresh();
        ImGui::SetNextItemWidth(8.0f * ImGui::GetFontSize());
        ImGui::Combo("Minimum level", &m_minimumLevel, kLevelNames.data(), static_cast<int>(kLevelNames.size()));
        ImGui::SameLine();
        ImGui::Checkbox("Autoscroll", &m_autoScroll);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu line(s)", m_view.size());

        if (ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            const spdlog::level::level_enum minimum = kLevels[static_cast<std::size_t>(m_minimumLevel)];
            for (std::size_t index = 0; index < m_view.size(); ++index) {
                const LogView::Line& line = m_view.line(index);
                if (line.level < minimum) {
                    continue;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, levelColor(line.level));
                ImGui::TextUnformatted(line.text.data(), line.text.data() + line.text.size());
                ImGui::PopStyleColor();
            }
            // Follow new lines while scrolled to the bottom; scrolling up stops following until back there.
            if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
    }

private:
    LogView m_view;
    /// Index into kLevels: info by default.
    int m_minimumLevel = 2;
    bool m_autoScroll = true;
};

} // namespace

std::unique_ptr<Panel> makeLogPanel() {
    return std::make_unique<LogPanel>();
}

} // namespace rv::devui
