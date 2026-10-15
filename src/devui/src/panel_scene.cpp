#include <cstddef>
#include <memory>
#include <utility>

#include <imgui.h>

#include <rv/devui/panels.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::devui {
namespace {

const char* sourceName(scene::SceneObject::Source source) noexcept {
    switch (source) {
    case scene::SceneObject::Source::Prefab:
        return "prefab";
    case scene::SceneObject::Source::Primitive:
        return "primitive";
    case scene::SceneObject::Source::Model:
        return "model";
    }
    return "?";
}

class ScenePanel final : public Panel {
public:
    explicit ScenePanel(ScenePanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return "Scene"; }

    void draw() override {
        const scene::SceneDescription& description = m_model.scene;
        ImGui::Text("%zu objects", description.objects.size());
        ImGui::SetItemTooltip("Scene '%s': %zu objects, %zu lights", description.name.c_str(),
                              description.objects.size(), description.lights.size());
        // One row per object on two lines, "go to" and the id, then the kind and the source: each line has the
        // panel's whole width, so a narrow docked column still shows all of them.
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
        if (!ImGui::BeginTable("objects", 1, kFlags)) {
            return;
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(description.objects.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto index = static_cast<std::size_t>(row);
                const scene::SceneObject& object = description.objects[index];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Go to")) {
                    m_model.goTo(index);
                }
                ImGui::SetItemTooltip("Stand in front of '%s', looking at it", object.id.c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(object.id.c_str());
                const char* kind =
                    object.source == scene::SceneObject::Source::Model ? object.modelPath.c_str() : object.kind.c_str();
                const std::size_t instances = object.instanceTransforms().size();
                if (instances > 1) {
                    ImGui::TextDisabled("%s (%s, %zu instances)", kind, sourceName(object.source), instances);
                } else {
                    ImGui::TextDisabled("%s (%s)", kind, sourceName(object.source));
                }
                ImGui::SetItemTooltip("Kind %s, source %s", kind, sourceName(object.source));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

private:
    ScenePanelModel m_model;
};

} // namespace

std::unique_ptr<Panel> makeScenePanel(ScenePanelModel model) {
    return std::make_unique<ScenePanel>(std::move(model));
}

} // namespace rv::devui
