#include <algorithm>
#include <cstdio>
#include <memory>

#include <imgui.h>

#include <rv/devui/panels.hpp>
#include <rv/render/frame_stats.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/texture_cache.hpp>

#include "widgets.hpp"

namespace rv::devui {
namespace {

/// The graph's scale reaches at least two 60 Hz frames, so that a steady 16.7 ms sits in its middle.
constexpr float kGraphMinimumMilliseconds = 33.4f;

class StatsPanel final : public Panel {
public:
    explicit StatsPanel(const StatsPanelModel& model) : m_model(model) {}

    [[nodiscard]] const char* title() const noexcept override { return "Stats"; }

    void draw() override {
        drawFrameTimes();
        drawLastView();
        drawResources();
    }

private:
    void drawFrameTimes() const {
        const FrameTimeHistory& history = m_model.frameTimes;
        const FrameTimeHistory::Summary summary = history.summary();
        ImGui::Text("%.0f FPS  %.2f ms", summary.framesPerSecond, summary.averageMilliseconds);
        ImGui::SetItemTooltip("Average frame time of the frames in the graph; GPU times of the passes: Last view");
        char caption[64];
        std::snprintf(caption, sizeof(caption), "%.2f to %.2f ms, %zu frames", summary.minMilliseconds,
                      summary.maxMilliseconds, history.size());
        const float scaleMax = std::max(kGraphMinimumMilliseconds, summary.maxMilliseconds * 1.1f);
        ImGui::PlotLines("##frame_times", history.ringData(), static_cast<int>(history.size()),
                         static_cast<int>(history.ringOffset()), caption, 0.0f, scaleMax,
                         ImVec2(-1.0f, 3.0f * ImGui::GetTextLineHeightWithSpacing()));
    }

    void drawLastView() const {
        ImGui::SeparatorText("Last view");
        const gl::FrameStats& stats = m_model.renderStats;
        if (widgets::beginValueTable("view")) {
            widgets::millisecondsRow("GPU shadow", stats.shadowGpuMilliseconds);
            widgets::millisecondsRow("GPU depth", stats.depthGpuMilliseconds);
            widgets::millisecondsRow("GPU SSAO", stats.ssaoGpuMilliseconds);
            widgets::millisecondsRow("GPU scene", stats.sceneGpuMilliseconds);
            widgets::millisecondsRow("GPU post", stats.postGpuMilliseconds);
            widgets::valueRow("Draw calls", stats.drawCalls);
            widgets::valueRow("Shadow draw calls", stats.shadowDrawCalls);
            widgets::valueRow("Shadow instances", stats.shadowInstances);
            widgets::valueRow("Depth draw calls", stats.depthDrawCalls);
            widgets::valueRow("Batches", stats.batches);
            widgets::valueRow("Instances", stats.instances);
            widgets::valueRow("Triangles", stats.triangles);
            widgets::valueRow("Items", stats.items);
            widgets::valueRow("Culled items", stats.culledItems);
            widgets::valueRow("Shader switches", stats.shaderSwitches);
            widgets::valueRow("Material switches", stats.materialSwitches);
            widgets::valueRow("Debug lines", stats.debugLines);
            ImGui::EndTable();
        }
    }

    void drawResources() const {
        ImGui::SeparatorText("GPU resources");
        if (!ImGui::BeginTable("resources", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            return;
        }
        const auto row = [](const char* label, std::size_t count, std::size_t bytes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", count);
            ImGui::TableNextColumn();
            widgets::bytes(bytes);
        };
        row("Meshes", m_model.meshes.size(), m_model.meshes.gpuBytes());
        row("Materials", m_model.materials.size(), m_model.materials.gpuBytes());
        row("Textures", m_model.textures.textureCount(), m_model.textures.estimatedBytes());
        ImGui::EndTable();
        if (const std::size_t failed = m_model.textures.failedCount(); failed > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu texture source(s) unavailable (checker)", failed);
        }
    }

    StatsPanelModel m_model;
};

} // namespace

std::unique_ptr<Panel> makeStatsPanel(const StatsPanelModel& model) {
    return std::make_unique<StatsPanel>(model);
}

} // namespace rv::devui
