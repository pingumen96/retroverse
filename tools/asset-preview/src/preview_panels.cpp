#include "preview_panels.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rv/core/text.hpp>
#include <rv/devui/texture_previews.hpp>
#include <rv/devui/widgets.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_cache.hpp>

namespace rv::preview {
namespace {

constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImVec4 kWarningColor{1.0f, 0.75f, 0.3f, 1.0f};
constexpr ImVec4 kOkColor{0.55f, 0.85f, 0.45f, 1.0f};
constexpr ImVec4 kInfoColor{0.65f, 0.75f, 0.9f, 1.0f};
/// Side of the screen's test pattern, in font sizes.
constexpr float kPatternEms = 13.0f;

const char* kindName(PreviewTarget::Kind kind) noexcept {
    switch (kind) {
    case PreviewTarget::Kind::Model:
        return "glTF model";
    case PreviewTarget::Kind::Manifest:
        return "prefab manifest";
    case PreviewTarget::Kind::Prefab:
        return "procedural prefab";
    }
    return "?";
}

/// "error", "warning" or "info" in its color.
void severityTag(PreviewMessage::Severity severity) {
    switch (severity) {
    case PreviewMessage::Severity::Error:
        ImGui::TextColored(kErrorColor, "error");
        break;
    case PreviewMessage::Severity::Warning:
        ImGui::TextColored(kWarningColor, "warning");
        break;
    case PreviewMessage::Severity::Info:
        ImGui::TextColored(kInfoColor, "info");
        break;
    }
}

void issueTag(assets::ValidationIssue::Severity severity) {
    severityTag(severity == assets::ValidationIssue::Severity::Error ? PreviewMessage::Severity::Error
                                                                     : PreviewMessage::Severity::Warning);
}

/// `text` wrapped on the rest of the line.
void wrappedOnLine(std::string_view text) {
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

/// "x, y, z" in meters.
std::string formatPosition(const glm::vec3& position) {
    return fmt::format("{:.3f}, {:.3f}, {:.3f} m", position.x, position.y, position.z);
}

class AssetPanel final : public devui::Panel {
public:
    explicit AssetPanel(PreviewPanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return "Asset"; }

    void draw() override {
        drawSubject();
        drawView();
        drawOverlays();
        drawCamera();
        ImGui::SeparatorText("Controls");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Left drag orbits, middle or right drag pans, the wheel zooms (gamepad: right stick, left "
                            "stick, triggers). Drop a file on the window to preview it. F1 hides the panels, F12 saves "
                            "a screenshot.");
        ImGui::PopTextWrapPos();
    }

private:
    void drawSubject() {
        const PreviewSubject& subject = m_model.subject;
        if (!subject.target) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted("Drop a .glb, .gltf or manifest .json file on the window to preview it.");
            ImGui::PopTextWrapPos();
            return;
        }
        ImGui::TextUnformatted(subject.title.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s: %s", kindName(subject.target->kind), subject.target->label().c_str());
        ImGui::PopTextWrapPos();
        constexpr PreviewMessage::Topic kAsset = PreviewMessage::Topic::Asset;
        const std::size_t warnings = subject.count(PreviewMessage::Severity::Warning, kAsset);
        if (subject.placeholder) {
            ImGui::TextColored(kErrorColor, "Not loaded: shown as the magenta placeholder");
        } else if (warnings == 0) {
            ImGui::TextColored(kOkColor, "Loaded");
        } else {
            ImGui::TextColored(kWarningColor, "Loaded with %zu warning(s)", warnings);
        }
        ImGui::SetItemTooltip("See the Warnings panel");
        if (subject.manifest) {
            const std::size_t issues[] = {subject.manifest->count(assets::ValidationIssue::Severity::Error),
                                          subject.manifest->count(assets::ValidationIssue::Severity::Warning)};
            const bool unreadable = !subject.manifest->manifest;
            if (unreadable) {
                ImGui::TextColored(kErrorColor, "Manifest unreadable");
            } else if (issues[0] + issues[1] == 0) {
                ImGui::TextColored(kOkColor, "Manifest: no issues");
            } else {
                ImGui::TextColored(issues[0] > 0 ? kErrorColor : kWarningColor,
                                   "Manifest: %zu error(s), %zu warning(s)", issues[0], issues[1]);
            }
            ImGui::SetItemTooltip("See the Manifest panel");
        }
        if (ImGui::Button("Reload")) {
            m_model.reload();
        }
        ImGui::SetItemTooltip("Load the target again (it reloads by itself when its files change on disk)");
    }

    void drawView() {
        ImGui::SeparatorText("View");
        int key = 1;
        for (const ViewMode mode : kViewModes) {
            const std::string label = std::string(toString(mode));
            if (ImGui::RadioButton(label.c_str(), m_model.viewMode == mode)) {
                m_model.viewMode = mode;
            }
            ImGui::SetItemTooltip("Key %d", key);
            if (key % 2 == 1 && mode != ViewMode::Wireframe) {
                ImGui::SameLine(); // two per line
            }
            ++key;
        }
    }

    void drawOverlays() {
        ImGui::SeparatorText("Overlays");
        OverlaySelection& overlays = m_model.overlays;
        if (!ImGui::BeginTable("overlays", 2, ImGuiTableFlags_SizingStretchSame)) {
            return;
        }
        const auto toggle = [](const char* label, bool& value, const char* tooltip) {
            ImGui::TableNextColumn();
            ImGui::Checkbox(label, &value);
            ImGui::SetItemTooltip("%s", tooltip);
        };
        toggle("Floor grid", overlays.grid, "Lines every 10 cm, brighter every meter; X axis red, Z axis blue");
        toggle("Scale", overlays.scaleReferences, "A 1 m cube and a 1.70 m human-height marker beside the subject");
        toggle("Bounds (cm)", overlays.bounds, "The subject's bounds with its width, height and depth");
        toggle("Node bounds", overlays.nodeBounds, "The bounds of every drawn node (prefab part)");
        toggle("Pivot", overlays.pivot, "The axes of the subject's origin: X red, Y green, Z blue");
        toggle("Sockets", overlays.sockets, "Socket axes and names, screen outlines, collision proxies");
        toggle("Normals", overlays.normals, "A short line along the normal of every vertex");
        ImGui::EndTable();
    }

    void drawCamera() {
        ImGui::SeparatorText("Camera");
        if (ImGui::Button("Frame (F)")) {
            m_model.frameSubject();
        }
        ImGui::SameLine();
        bool rotate = m_model.orbit.autoRotate();
        if (ImGui::Checkbox("Auto-rotate (R)", &rotate)) {
            m_model.orbit.setAutoRotate(rotate);
        }
        ImGui::TextDisabled("Distance %.3f m", m_model.orbit.distance());
    }

    PreviewPanelModel m_model;
};

class ModelPanel final : public devui::Panel {
public:
    explicit ModelPanel(PreviewPanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return "Model"; }

    void draw() override {
        const PreviewSubject& subject = m_model.subject;
        if (subject.model) {
            drawModel(*subject.model);
        } else if (subject.prefab) {
            drawPrefab(*subject.prefab);
        } else {
            ImGui::TextDisabled("Nothing loaded");
        }
    }

private:
    /// The children of every node and the material of every mesh, rebuilt for each subject.
    void refresh(const assets::ModelData& model) {
        if (m_revision == m_model.subjectRevision) {
            return;
        }
        m_revision = m_model.subjectRevision;
        m_children.assign(model.nodes.size(), {});
        m_roots.clear();
        for (std::size_t index = 0; index < model.nodes.size(); ++index) {
            const int parent = model.nodes[index].parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < model.nodes.size()) {
                m_children[static_cast<std::size_t>(parent)].push_back(index);
            } else {
                m_roots.push_back(index);
            }
        }
        m_meshMaterials.assign(model.meshes.size(), std::nullopt);
        for (const assets::ModelDrawItem& item : model.drawItems) {
            if (item.meshIndex < m_meshMaterials.size()) {
                m_meshMaterials[item.meshIndex] = item.material;
            }
        }
    }

    void drawModel(const assets::ModelData& model) {
        refresh(model);
        ImGui::Text("%zu node(s), %zu primitive(s)", model.nodes.size(), model.meshes.size());
        for (const std::size_t root : m_roots) {
            drawNode(model, root);
        }
    }

    void drawNode(const assets::ModelData& model, std::size_t index) {
        const assets::ModelNode& node = model.nodes[index];
        const auto socket = std::find_if(m_model.subject.sockets.begin(), m_model.subject.sockets.end(),
                                         [&node](const geometry::Socket& s) { return s.nodeName == node.name; });
        std::string label = node.name;
        if (socket != m_model.subject.sockets.end()) {
            label += fmt::format("  [{}]", geometry::toString(socket->kind));
        }
        const bool leaf = m_children[index].empty() && !node.mesh;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (leaf) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        ImGui::PushID(static_cast<int>(index));
        const bool open = ImGui::TreeNodeEx("node", flags, "%s", label.c_str());
        if (ImGui::BeginItemTooltip()) {
            const glm::vec3 position(node.world[3]);
            ImGui::Text("Origin %s", formatPosition(position).c_str());
            ImGui::EndTooltip();
        }
        if (open && !leaf) {
            if (node.mesh) {
                for (std::uint32_t mesh = *node.mesh; mesh < *node.mesh + node.meshCount && mesh < model.meshes.size();
                     ++mesh) {
                    drawPrimitive(model, mesh);
                }
            }
            for (const std::size_t child : m_children[index]) {
                drawNode(model, child);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void drawPrimitive(const assets::ModelData& model, std::uint32_t mesh) {
        const geometry::MeshData& data = model.meshes[mesh];
        const std::optional<std::uint32_t> material =
            mesh < m_meshMaterials.size() ? m_meshMaterials[mesh] : std::nullopt;
        const std::string materialName = material && *material < model.materials.size()
                                             ? model.materials[*material].name
                                             : std::string("glTF default material");
        ImGui::BulletText("%s: %zu tri, %zu vert", data.name.c_str(), data.triangleCount(), data.vertices.size());
        ImGui::SetItemTooltip("Mesh %u, material %s", mesh, materialName.c_str());
    }

    void drawPrefab(const geometry::ProceduralPrefab& prefab) {
        const auto materials = prefabRoleMaterials(prefab.kind);
        ImGui::SeparatorText("Parts");
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("parts", 3, kFlags)) {
            ImGui::TableSetupColumn("Role");
            ImGui::TableSetupColumn("Triangles");
            ImGui::TableSetupColumn("Material");
            ImGui::TableHeadersRow();
            for (const geometry::PrefabPart& part : prefab.parts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(part.materialRole.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", part.mesh.triangleCount());
                ImGui::TableNextColumn();
                const auto found = materials.find(part.materialRole);
                ImGui::TextUnformatted(found != materials.end() ? found->second.c_str() : "-");
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Sockets");
        if (ImGui::BeginTable("sockets", 2, kFlags)) {
            for (const geometry::Socket& socket : prefab.sockets) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(socket.nodeName.c_str());
                ImGui::TableNextColumn();
                const std::string_view kind = geometry::toString(socket.kind);
                ImGui::TextDisabled("%.*s", static_cast<int>(kind.size()), kind.data());
                ImGui::SetItemTooltip("%s", formatPosition(glm::vec3(socket.transform[3])).c_str());
            }
            ImGui::EndTable();
        }
    }

    PreviewPanelModel m_model;
    std::uint64_t m_revision = 0;
    std::vector<std::vector<std::size_t>> m_children;
    std::vector<std::size_t> m_roots;
    std::vector<std::optional<std::uint32_t>> m_meshMaterials;
};

class StatisticsPanel final : public devui::Panel {
public:
    explicit StatisticsPanel(PreviewPanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return "Statistics"; }

    void draw() override {
        const AssetReport& report = m_model.report;
        if (!m_model.subject.target) {
            ImGui::TextDisabled("Nothing loaded");
            return;
        }
        ImGui::SeparatorText("Bounds");
        if (report.size == glm::vec3(0.0f)) {
            ImGui::TextDisabled("None (placeholder)");
        } else {
            ImGui::TextUnformatted(formatDimensions(report.size).c_str());
            ImGui::SetItemTooltip("Width (x) x height (y) x depth (z)");
        }
        ImGui::SeparatorText("Geometry");
        if (devui::widgets::beginValueTable("geometry")) {
            devui::widgets::valueRow("Nodes", report.nodes);
            devui::widgets::valueRow("Meshes", report.meshes);
            devui::widgets::valueRow("Primitives (draw calls)", report.primitives);
            devui::widgets::valueRow("Triangles drawn", report.triangles);
            devui::widgets::valueRow("Triangles in meshes", report.uniqueTriangles);
            devui::widgets::valueRow("Vertices", report.vertices);
            devui::widgets::valueRow("Materials", report.materials);
            devui::widgets::valueRow("Sockets", report.sockets);
            ImGui::EndTable();
        }
        drawTextures(report);
        ImGui::SeparatorText("glTF extensions");
        if (report.extensions.empty()) {
            ImGui::TextDisabled("None");
        }
        for (const std::string& extension : report.extensions) {
            ImGui::BulletText("%s", extension.c_str());
        }
    }

private:
    void drawTextures(const AssetReport& report) {
        ImGui::SeparatorText("Textures");
        if (!m_model.subject.model) {
            // Prefabs draw procedural surfaces and patterns: what the cache holds.
            ImGui::Text("%zu on the GPU, ", m_model.textures.textureCount());
            ImGui::SameLine(0.0f, 0.0f);
            devui::widgets::bytes(m_model.textures.estimatedBytes());
            ImGui::TextDisabled("Procedural surfaces and patterns of the materials");
            return;
        }
        if (report.textures.empty()) {
            ImGui::TextDisabled("No images");
            return;
        }
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("textures", 3, kFlags)) {
            ImGui::TableSetupColumn("Image");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("GPU (est.)");
            ImGui::TableHeadersRow();
            for (const TextureReport& texture : report.textures) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("#%u", texture.image);
                ImGui::SetItemTooltip("%s", texture.usage.empty() ? "unused" : texture.usage.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d x %d x %d", texture.size.x, texture.size.y, texture.channels);
                ImGui::TableNextColumn();
                devui::widgets::bytes(texture.gpuBytes);
            }
            ImGui::EndTable();
        }
        ImGui::Text("Total ");
        ImGui::SameLine(0.0f, 0.0f);
        devui::widgets::bytes(report.textureBytes);
        ImGui::SetItemTooltip("RGBA8 with full mip chains, once per color space an image is used in");
    }

    PreviewPanelModel m_model;
};

class ManifestPanel final : public devui::Panel {
public:
    explicit ManifestPanel(PreviewPanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return kManifestPanelTitle; }

    void draw() override {
        const std::optional<ManifestCheck>& check = m_model.subject.manifest;
        if (!check) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("No manifest. A manifest next to a model (<name>.manifest.json or <name>.json) is "
                                "checked against it; open a manifest .json to preview the prefab it describes.");
            ImGui::PopTextWrapPos();
            return;
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", core::pathToUtf8(check->path).c_str());
        ImGui::PopTextWrapPos();
        if (check->error) {
            ImGui::TextColored(kErrorColor, "Cannot be read");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(check->error->toString().c_str());
            ImGui::PopTextWrapPos();
            return;
        }
        drawChecks(*check);
        drawManifest(*check->manifest);
    }

private:
    static void drawChecks(const ManifestCheck& check) {
        ImGui::SeparatorText("Checks");
        if (check.issues.empty()) {
            ImGui::TextColored(kOkColor, "No issues");
            return;
        }
        ImGui::Text("%zu error(s), %zu warning(s)", check.count(assets::ValidationIssue::Severity::Error),
                    check.count(assets::ValidationIssue::Severity::Warning));
        for (const assets::ValidationIssue& issue : check.issues) {
            issueTag(issue.severity);
            wrappedOnLine(issue.message);
        }
    }

    void drawManifest(const assets::PrefabManifest& manifest) const {
        using devui::widgets::textRow;
        ImGui::SeparatorText("Identity");
        if (devui::widgets::beginValueTable("identity")) {
            textRow("Id", manifest.id);
            textRow("Asset type", manifest.assetType);
            textRow("Category", manifest.category);
            textRow("Role", manifest.role);
            textRow("Tags", join(manifest.tags));
            textRow("Fallback", manifest.fallbackPrefab.empty() ? "-" : manifest.fallbackPrefab);
            textRow("Hash", manifest.hash);
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Source and license");
        const assets::SourceInfo& source = manifest.source;
        if (devui::widgets::beginValueTable("source")) {
            textRow("Kind", source.kind);
            textRow("Author", source.author);
            textRow("URL", source.url.empty() ? "-" : source.url);
            textRow("License", source.license);
            textRow("Snapshot", source.licenseSnapshot.empty() ? "-" : source.licenseSnapshot);
            textRow("Acquired", source.acquiredAt);
            textRow("Attribution", source.attributionRequired ? "required" : "not required");
            textRow("Redistribution", source.redistributionApproved ? "approved" : "not approved");
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Runtime");
        const assets::RuntimeInfo& runtime = manifest.runtime;
        if (devui::widgets::beginValueTable("runtime")) {
            if (runtime.model) {
                textRow("Model", *runtime.model);
            }
            if (runtime.generator) {
                textRow("Generator", *runtime.generator);
                textRow("Parameters", runtime.params.empty() ? std::string("defaults") : runtime.params.dump());
            }
            if (runtime.scaleMeters) {
                textRow("Size (nominal)", formatDimensions(*runtime.scaleMeters));
            }
            if (m_model.report.size != glm::vec3(0.0f)) {
                textRow("Size (measured)", formatDimensions(m_model.report.size));
            }
            for (const auto& [role, socket] : runtime.sockets) {
                textRow(("Socket " + role).c_str(), socket);
            }
            if (runtime.videoSurface) {
                textRow("Video surface", runtime.videoSurface->aspectRatio + " on " + runtime.videoSurface->meshNode);
            }
            textRow("Collision", runtime.collision.empty() ? "-" : runtime.collision);
            textRow("LODs", runtime.lods.empty() ? "-" : join(runtime.lods));
            ImGui::EndTable();
        }
    }

    static std::string join(const std::vector<std::string>& values) {
        std::string text;
        for (const std::string& value : values) {
            text += text.empty() ? "" : ", ";
            text += value;
        }
        return text.empty() ? "-" : text;
    }

    PreviewPanelModel m_model;
};

class ScreenPanel final : public devui::Panel {
public:
    explicit ScreenPanel(PreviewPanelModel model) : m_model(std::move(model)), m_previews(m_model.textures) {}

    [[nodiscard]] const char* title() const noexcept override { return "Screen"; }

    void draw() override {
        if (m_model.screens.empty()) {
            ImGui::TextDisabled("No ScreenSurface with a mesh");
            return;
        }
        for (const ScreenSurface& screen : m_model.screens) {
            ImGui::PushID(screen.socketId.c_str());
            drawScreen(screen);
            ImGui::PopID();
        }
    }

private:
    void drawScreen(const ScreenSurface& screen) {
        ImGui::SeparatorText(screen.nodeName.c_str());
        const glm::vec3 size = screen.bounds.size();
        const float aspect = size.y > 0.0f ? size.x / size.y : 0.0f;
        ImGui::Text("%.1f x %.1f cm, %s", 100.0f * size.x, 100.0f * size.y, aspectRatioName(aspect).c_str());
        ImGui::SetItemTooltip("The screen rectangle and its aspect ratio (width / height %.3f)", aspect);
        drawVideoSurface(screen, aspect);
        const gl::Texture2D& pattern =
            m_model.textures.pattern(kScreenPattern, "", 0, screen.patternSize, gl::ColorSpace::Srgb);
        m_previews.thumbnail(pattern, kPatternEms * ImGui::GetFontSize(), "Test pattern");
        ImGui::TextDisabled("%.*s %d x %d on %.*s", static_cast<int>(kScreenPattern.size()), kScreenPattern.data(),
                            screen.patternSize.x, screen.patternSize.y, static_cast<int>(kScreenMaterialName.size()),
                            kScreenMaterialName.data());
    }

    /// The manifest's video surface on this screen, and whether its aspect ratio matches the mesh.
    void drawVideoSurface(const ScreenSurface& screen, float aspect) const {
        const std::optional<ManifestCheck>& check = m_model.subject.manifest;
        if (!check || !check->manifest || !check->manifest->runtime.videoSurface) {
            return;
        }
        const assets::VideoSurfaceInfo& surface = *check->manifest->runtime.videoSurface;
        if (surface.meshNode != screen.socketId && surface.meshNode != screen.nodeName) {
            return;
        }
        const std::optional<float> declared = parseAspectRatio(surface.aspectRatio);
        const bool matches = declared && aspect > 0.0f && std::abs(*declared - aspect) <= 0.015f * *declared;
        ImGui::TextColored(matches ? kOkColor : kWarningColor, "Manifest video surface %s%s",
                           surface.aspectRatio.c_str(), matches ? "" : ": the mesh's aspect differs");
    }

    PreviewPanelModel m_model;
    devui::TexturePreviews m_previews;
};

class WarningsPanel final : public devui::Panel {
public:
    explicit WarningsPanel(PreviewPanelModel model) : m_model(std::move(model)) {}

    [[nodiscard]] const char* title() const noexcept override { return "Warnings"; }

    void draw() override {
        const std::vector<PreviewMessage>& messages = m_model.subject.messages;
        if (messages.empty()) {
            ImGui::TextColored(kOkColor, m_model.subject.target ? "No warnings" : "Nothing loaded");
            return;
        }
        for (const PreviewMessage& message : messages) {
            severityTag(message.severity);
            wrappedOnLine(message.text);
        }
    }

private:
    PreviewPanelModel m_model;
};

} // namespace

std::unique_ptr<devui::Panel> makeAssetPanel(PreviewPanelModel model) {
    return std::make_unique<AssetPanel>(std::move(model));
}

std::unique_ptr<devui::Panel> makeModelPanel(PreviewPanelModel model) {
    return std::make_unique<ModelPanel>(std::move(model));
}

std::unique_ptr<devui::Panel> makeStatisticsPanel(PreviewPanelModel model) {
    return std::make_unique<StatisticsPanel>(std::move(model));
}

std::unique_ptr<devui::Panel> makeManifestPanel(PreviewPanelModel model) {
    return std::make_unique<ManifestPanel>(std::move(model));
}

std::unique_ptr<devui::Panel> makeScreenPanel(PreviewPanelModel model) {
    return std::make_unique<ScreenPanel>(std::move(model));
}

std::unique_ptr<devui::Panel> makeWarningsPanel(PreviewPanelModel model) {
    return std::make_unique<WarningsPanel>(std::move(model));
}

} // namespace rv::preview
