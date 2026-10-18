#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <rv/devui/panels.hpp>
#include <rv/devui/widgets.hpp>
#include <rv/render/frame_stats.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/texture_cache.hpp>

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
        drawGpuMemory();
        drawVideo();
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
            if (stats.outlineGpuMilliseconds) {
                widgets::millisecondsRow("GPU outline", stats.outlineGpuMilliseconds);
            }
            widgets::valueRow("Draw calls", stats.drawCalls);
            widgets::valueRow("Shadow draw calls", stats.shadowDrawCalls);
            widgets::valueRow("Shadow instances", stats.shadowInstances);
            widgets::valueRow("Depth draw calls", stats.depthDrawCalls);
            widgets::valueRow("Batches", stats.batches);
            widgets::valueRow("Instances", stats.instances);
            widgets::valueRow("Triangles", stats.triangles);
            widgets::valueRow("Items", stats.items);
            widgets::valueRow("Culled items", stats.culledItems);
            if (stats.staticGroups > 0) {
                // The collection's static instance groups (shown only in views that have them).
                widgets::valueRow("Static draw calls", stats.staticDrawCalls);
                widgets::valueRow("Static instances", stats.staticInstances);
                widgets::valueRow("Clusters", stats.clusters);
                widgets::valueRow("Culled clusters", stats.culledClusters);
            }
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

    /// The renderer's accounting per kind (gl::GpuMemoryLedger), what the run report's gpuMemory records.
    void drawGpuMemory() const {
        ImGui::SeparatorText("GPU memory");
        ImGui::SetItemTooltip("The renderer's own accounting of what it allocates, from each allocation's format, size, "
                              "levels and layers (not the driver's figure; the overlay's and the product UI's own "
                              "textures are not counted)");
        if (!ImGui::BeginTable("gpu_memory", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            return;
        }
        // The numbers keep their width; the kind's name takes what is left. The allocations are in its tooltip.
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Now");
        ImGui::TableSetupColumn("Peak");
        ImGui::TableHeadersRow();
        const auto row = [](const char* label, const char* name, std::uint64_t count, std::uint64_t bytes,
                            std::uint64_t peak) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::SetItemTooltip("%s: %llu allocation(s) now", name, static_cast<unsigned long long>(count));
            ImGui::TableNextColumn();
            widgets::bytes(static_cast<std::size_t>(bytes));
            ImGui::TableNextColumn();
            widgets::bytes(static_cast<std::size_t>(peak));
        };
        std::uint64_t allocations = 0;
        for (const gl::GpuMemoryKind kind : gl::kGpuMemoryKinds) {
            const gl::GpuMemoryUsage& usage = m_model.gpuMemory.usage(kind);
            allocations += usage.allocations;
            row(gl::displayName(kind).data(), gl::toString(kind).data(), usage.allocations, usage.bytes,
                usage.peakBytes);
        }
        row("Total", "every kind", allocations, m_model.gpuMemory.totalBytes(), m_model.gpuMemory.peakTotalBytes());
        ImGui::EndTable();
    }

    /// The hero CRT's decoder and pictures (I3 step 11), what the run report's video section records.
    void drawVideo() const {
        if (!m_model.videoStatus) {
            return;
        }
        const std::vector<std::string> lines = m_model.videoStatus();
        if (lines.empty()) {
            return;
        }
        ImGui::SeparatorText("Video");
        for (const std::string& line : lines) {
            ImGui::TextWrapped("%s", line.c_str());
        }
    }

    StatsPanelModel m_model;
};

} // namespace

std::unique_ptr<Panel> makeStatsPanel(const StatsPanelModel& model) {
    return std::make_unique<StatsPanel>(model);
}

} // namespace rv::devui
