#include "preview_panels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
#include <rv/render/material_registry.hpp>
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

/// "label: value" rows of a table, formatted once per subject.
using Rows = std::vector<std::pair<std::string, std::string>>;

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

/// The rows of `rows` in a value table with the given id.
void drawRows(const char* id, const Rows& rows) {
    if (devui::widgets::beginValueTable(id)) {
        for (const auto& [label, value] : rows) {
            devui::widgets::textRow(label.c_str(), value);
        }
        ImGui::EndTable();
    }
}

/// Tracks the subject a panel last formatted its text for.
class SubjectRevision {
public:
    explicit SubjectRevision(const std::uint64_t& current) noexcept : m_current(current) {}

    /// True once per new subject: the panel formats what it shows of it again.
    [[nodiscard]] bool changed() noexcept {
        if (m_seen == m_current) {
            return false;
        }
        m_seen = m_current;
        return true;
    }

private:
    const std::uint64_t& m_current;
    std::uint64_t m_seen = 0; // subjects count from 1
};

class AssetPanel final : public devui::Panel {
public:
    explicit AssetPanel(PreviewPanelModel model) : m_model(std::move(model)), m_revision(m_model.subjectRevision) {
        for (std::size_t index = 0; index < m_viewLabels.size(); ++index) {
            m_viewLabels[index] = std::string(toString(kViewModes[index]));
        }
    }

    [[nodiscard]] const char* title() const noexcept override { return "Asset"; }

    void draw() override {
        if (m_revision.changed()) {
            m_targetLabel = m_model.subject.target ? m_model.subject.target->label() : std::string();
        }
        drawSubject();
        drawView();
        drawOverlays();
        drawCamera();
        ImGui::SeparatorText("Controls");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Left drag orbits, middle or right drag pans, the wheel zooms (gamepad: right stick, left "
                            "stick, triggers). Drop a file on the window to preview it. F1 hides the panels (the "
                            "labels stay), F12 saves a screenshot.");
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
        ImGui::TextDisabled("%s: %s", kindName(subject.target->kind), m_targetLabel.c_str());
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
        for (std::size_t index = 0; index < m_viewLabels.size(); ++index) {
            const ViewMode mode = kViewModes[index];
            if (ImGui::RadioButton(m_viewLabels[index].c_str(), m_model.viewMode == mode)) {
                m_model.viewMode = mode;
            }
            ImGui::SetItemTooltip("Key %zu", index + 1);
            if (index % 2 == 0 && index + 1 < m_viewLabels.size()) {
                ImGui::SameLine(); // two per line
            }
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
        toggle("Floor grid", overlays.grid,
               "Lines every 10 cm (on subjects up to 6 m across), brighter every meter; X axis red, Z axis blue");
        toggle("Scale", overlays.scaleReferences, "A 1 m cube and a 1.70 m human-height marker beside the subject");
        toggle("Bounds (cm)", overlays.bounds, "The subject's bounds with its width, height and depth");
        toggle("Node bounds", overlays.nodeBounds, "The bounds of every drawn node (prefab part)");
        toggle("Pivot", overlays.pivot, "The axes of the subject's origin: X red, Y green, Z blue");
        toggle("Sockets", overlays.sockets,
               "Socket axes and names, screen outlines, slot and level volumes, collision proxies");
        toggle("Normals", overlays.normals, "A short line along the normal of every vertex");
        ImGui::EndTable();
    }

    void drawCamera() {
        ImGui::SeparatorText("Camera");
        if (ImGui::Button("Frame (F)")) {
            m_model.frameSubject();
        }
        ImGui::SetItemTooltip(
            "Frame the subject; again, before the camera moves: with its sockets and scale references");
        ImGui::SameLine();
        bool rotate = m_model.orbit.autoRotate();
        if (ImGui::Checkbox("Auto-rotate (R)", &rotate)) {
            m_model.orbit.setAutoRotate(rotate);
        }
        ImGui::TextDisabled("Distance %.3f m", m_model.orbit.distance());
    }

    PreviewPanelModel m_model;
    SubjectRevision m_revision;
    std::array<std::string, std::size(kViewModes)> m_viewLabels;
    std::string m_targetLabel;
};

class ModelPanel final : public devui::Panel {
public:
    explicit ModelPanel(PreviewPanelModel model) : m_model(std::move(model)), m_revision(m_model.subjectRevision) {}

    [[nodiscard]] const char* title() const noexcept override { return "Model"; }

    void draw() override {
        if (m_revision.changed()) {
            refresh();
        }
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
    /// The tree of the model (children of every node, labels with socket kinds, primitive lines with their materials)
    /// or the prefab's part materials and socket positions, formatted for the new subject.
    void refresh() {
        const PreviewSubject& subject = m_model.subject;
        m_children.clear();
        m_roots.clear();
        m_nodeLabels.clear();
        m_nodeOrigins.clear();
        m_primitiveLines.clear();
        m_partMaterials.clear();
        m_socketPositions.clear();
        if (subject.model) {
            refreshModel(*subject.model);
        } else if (subject.prefab) {
            const auto materials = prefabRoleMaterials(subject.prefab->kind);
            for (const geometry::PrefabPart& part : subject.prefab->parts) {
                const auto found = materials.find(part.materialRole);
                m_partMaterials.push_back(found != materials.end() ? found->second : std::string("-"));
            }
            for (const geometry::Socket& socket : subject.prefab->sockets) {
                // Where the socket is and, for a slot or a screen, its extent (a slot: the item it holds).
                std::string text = formatPosition(glm::vec3(socket.transform[3]));
                if (!socket.bounds.isEmpty()) {
                    text += ", " + formatDimensions(geometry::socketSize(socket));
                }
                m_socketPositions.push_back(std::move(text));
            }
        }
    }

    void refreshModel(const assets::ModelData& model) {
        m_children.assign(model.nodes.size(), {});
        for (std::size_t index = 0; index < model.nodes.size(); ++index) {
            const assets::ModelNode& node = model.nodes[index];
            const int parent = node.parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < model.nodes.size()) {
                m_children[static_cast<std::size_t>(parent)].push_back(index);
            } else {
                m_roots.push_back(index);
            }
            const geometry::Socket* socket = nullptr;
            for (const geometry::Socket& candidate : m_model.subject.sockets) {
                if (candidate.nodeName == node.name) {
                    socket = &candidate;
                    break;
                }
            }
            m_nodeLabels.push_back(
                socket != nullptr ? fmt::format("{}  [{}]", node.name, geometry::toString(socket->kind)) : node.name);
            m_nodeOrigins.push_back(formatPosition(glm::vec3(node.world[3])));
        }
        // The material each primitive is drawn with; primitives that are not drawn (a collision proxy's) have none.
        std::vector<const assets::ModelDrawItem*> drawnAs(model.meshes.size(), nullptr);
        for (const assets::ModelDrawItem& item : model.drawItems) {
            if (item.meshIndex < drawnAs.size()) {
                drawnAs[item.meshIndex] = &item;
            }
        }
        for (std::size_t mesh = 0; mesh < model.meshes.size(); ++mesh) {
            const geometry::MeshData& data = model.meshes[mesh];
            std::string_view materialName = "not drawn";
            if (const assets::ModelDrawItem* item = drawnAs[mesh]) {
                materialName = item->material && *item->material < model.materials.size()
                                   ? std::string_view(model.materials[*item->material].name)
                                   : std::string_view("glTF default material");
            }
            m_primitiveLines.push_back(fmt::format("{}: {} tri, {} vert\n{}", data.name, data.triangleCount(),
                                                   data.vertices.size(), materialName));
        }
    }

    void drawModel(const assets::ModelData& model) {
        ImGui::Text("%zu node(s), %zu primitive(s)", model.nodes.size(), model.meshes.size());
        ImGui::SetItemTooltip("Nodes, their primitives (triangles, vertices, material) and socket kinds");
        for (const std::size_t root : m_roots) {
            drawNode(model, root);
        }
    }

    void drawNode(const assets::ModelData& model, std::size_t index) {
        const assets::ModelNode& node = model.nodes[index];
        const bool leaf = m_children[index].empty() && !node.mesh;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (leaf) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        ImGui::PushID(static_cast<int>(index));
        const bool open = ImGui::TreeNodeEx("node", flags, "%s", m_nodeLabels[index].c_str());
        ImGui::SetItemTooltip("Origin %s", m_nodeOrigins[index].c_str());
        if (open && !leaf) {
            if (node.mesh) {
                for (std::uint32_t mesh = *node.mesh; mesh < *node.mesh + node.meshCount && mesh < model.meshes.size();
                     ++mesh) {
                    // The material on a line of its own; both wrap in a narrow column.
                    ImGui::Bullet();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(m_primitiveLines[mesh].c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::SetItemTooltip("Primitive %u: %s", mesh, m_primitiveLines[mesh].c_str());
                }
            }
            for (const std::size_t child : m_children[index]) {
                drawNode(model, child);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void drawPrefab(const geometry::ProceduralPrefab& prefab) {
        ImGui::SeparatorText("Parts");
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("parts", 3, kFlags)) {
            ImGui::TableSetupColumn("Role");
            ImGui::TableSetupColumn("Triangles");
            ImGui::TableSetupColumn("Material");
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < prefab.parts.size() && index < m_partMaterials.size(); ++index) {
                const geometry::PrefabPart& part = prefab.parts[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(part.materialRole.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", part.mesh.triangleCount());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m_partMaterials[index].c_str());
                ImGui::SetItemTooltip("%s", m_partMaterials[index].c_str());
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Sockets");
        if (ImGui::BeginTable("sockets", 2, kFlags)) {
            for (std::size_t index = 0; index < prefab.sockets.size() && index < m_socketPositions.size(); ++index) {
                const geometry::Socket& socket = prefab.sockets[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(socket.nodeName.c_str());
                ImGui::TableNextColumn();
                const std::string_view kind = geometry::toString(socket.kind);
                ImGui::TextDisabled("%.*s", static_cast<int>(kind.size()), kind.data());
                ImGui::SetItemTooltip("%s", m_socketPositions[index].c_str());
            }
            ImGui::EndTable();
        }
    }

    PreviewPanelModel m_model;
    SubjectRevision m_revision;
    std::vector<std::vector<std::size_t>> m_children;
    std::vector<std::size_t> m_roots;
    std::vector<std::string> m_nodeLabels;
    std::vector<std::string> m_nodeOrigins;
    /// Per primitive of the model (assets::ModelData::meshes).
    std::vector<std::string> m_primitiveLines;
    /// Per part and per socket of the prefab.
    std::vector<std::string> m_partMaterials;
    std::vector<std::string> m_socketPositions;
};

class StatisticsPanel final : public devui::Panel {
public:
    explicit StatisticsPanel(PreviewPanelModel model)
        : m_model(std::move(model)), m_revision(m_model.subjectRevision) {}

    [[nodiscard]] const char* title() const noexcept override { return "Statistics"; }

    void draw() override {
        if (m_revision.changed()) {
            refresh();
        }
        const AssetReport& report = m_model.report;
        if (!m_model.subject.target) {
            ImGui::TextDisabled("Nothing loaded");
            return;
        }
        ImGui::SeparatorText("Bounds");
        if (report.size == glm::vec3(0.0f)) {
            ImGui::TextDisabled("None (placeholder)");
        } else {
            ImGui::TextUnformatted(m_dimensions.c_str());
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
        if (!report.slots.empty()) {
            ImGui::SeparatorText("Slots");
            for (const std::string& line : m_slotLines) {
                ImGui::Bullet(); // keeps the cursor on its line: the text wraps beside the bullet
                ImGui::TextWrapped("%s", line.c_str());
                ImGui::SetItemTooltip("Slot sockets (Slot_<level>_<index>) by the volume an item fills in them");
            }
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
    /// The size of the new subject, and for a prefab the textures its materials use (the registry holds the subject's
    /// materials only; the texture cache also keeps the procedural surfaces of earlier subjects).
    void refresh() {
        m_dimensions = formatDimensions(m_model.report.size);
        m_slotLines.clear();
        for (const SlotReport& slots : m_model.report.slots) {
            m_slotLines.push_back(fmt::format("{}: {} in {} level{}", formatDimensions(slots.size), slots.slots,
                                              slots.levels, slots.levels == 1 ? "" : "s"));
        }
        m_prefabTextures = 0;
        m_prefabTextureBytes = 0;
        if (!m_model.subject.prefab) {
            return;
        }
        std::vector<const gl::Texture2D*> seen;
        const gl::MaterialRegistry& materials = m_model.materials;
        for (std::size_t index = 0; index < materials.size(); ++index) {
            const gl::GpuMaterial& material =
                materials.material(scene::MaterialHandle{static_cast<std::uint32_t>(index)});
            for (const gl::Texture2D* texture : material.textureSources) {
                if (texture != nullptr && std::find(seen.begin(), seen.end(), texture) == seen.end()) {
                    seen.push_back(texture);
                    m_prefabTextureBytes += texture->estimatedBytes();
                }
            }
        }
        m_prefabTextures = seen.size();
    }

    void drawTextures(const AssetReport& report) {
        ImGui::SeparatorText("Textures");
        if (m_model.subject.prefab) {
            ImGui::Text("%zu on the GPU, ", m_prefabTextures);
            ImGui::SameLine(0.0f, 0.0f);
            devui::widgets::bytes(m_prefabTextureBytes);
            ImGui::TextDisabled("Procedural surfaces and patterns of the materials");
            return;
        }
        if (!m_model.subject.model) {
            ImGui::TextDisabled("None (not loaded)");
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
    SubjectRevision m_revision;
    std::string m_dimensions;
    /// One line per AssetReport::slots entry: "2.0 x 12.5 x 8.5 cm: 231 in 7 levels".
    std::vector<std::string> m_slotLines;
    std::size_t m_prefabTextures = 0;
    std::size_t m_prefabTextureBytes = 0;
};

class ManifestPanel final : public devui::Panel {
public:
    explicit ManifestPanel(PreviewPanelModel model) : m_model(std::move(model)), m_revision(m_model.subjectRevision) {}

    [[nodiscard]] const char* title() const noexcept override { return kManifestPanelTitle; }

    void draw() override {
        const std::optional<ManifestCheck>& check = m_model.subject.manifest;
        if (m_revision.changed()) {
            refresh(check);
        }
        if (!check) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("No manifest. A manifest next to a model (<name>.manifest.json or <name>.json) is "
                                "checked against it; open a manifest .json to preview the prefab it describes.");
            ImGui::PopTextWrapPos();
            return;
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", m_path.c_str());
        ImGui::PopTextWrapPos();
        if (check->error) {
            ImGui::TextColored(kErrorColor, "Cannot be read");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(m_error.c_str());
            ImGui::PopTextWrapPos();
            return;
        }
        drawChecks(*check);
        ImGui::SeparatorText("Identity");
        drawRows("identity", m_identity);
        ImGui::SeparatorText("Source and license");
        drawRows("source", m_source);
        ImGui::SeparatorText("Runtime");
        drawRows("runtime", m_runtime);
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

    /// The manifest's fields as rows, for the new subject.
    void refresh(const std::optional<ManifestCheck>& check) {
        m_path.clear();
        m_error.clear();
        m_identity.clear();
        m_source.clear();
        m_runtime.clear();
        if (!check) {
            return;
        }
        m_path = core::pathToGenericUtf8(check->path);
        if (check->error) {
            m_error = check->error->toString();
        }
        if (!check->manifest) {
            return;
        }
        const assets::PrefabManifest& manifest = *check->manifest;
        m_identity = {{"Id", manifest.id},
                      {"Asset type", manifest.assetType},
                      {"Category", manifest.category},
                      {"Role", manifest.role},
                      {"Tags", join(manifest.tags)},
                      {"Fallback", manifest.fallbackPrefab.empty() ? "-" : manifest.fallbackPrefab},
                      {"Hash", manifest.hash}};
        const assets::SourceInfo& source = manifest.source;
        m_source = {{"Kind", source.kind},
                    {"Author", source.author},
                    {"URL", source.url.empty() ? "-" : source.url},
                    {"License", source.license},
                    {"Snapshot", source.licenseSnapshot.empty() ? "-" : source.licenseSnapshot},
                    {"Acquired", source.acquiredAt},
                    {"Attribution", source.attributionRequired ? "required" : "not required"},
                    {"Redistribution", source.redistributionApproved ? "approved" : "not approved"}};
        const assets::RuntimeInfo& runtime = manifest.runtime;
        if (runtime.model) {
            m_runtime.emplace_back("Model", *runtime.model);
        }
        if (runtime.generator) {
            m_runtime.emplace_back("Generator", *runtime.generator);
            m_runtime.emplace_back("Parameters",
                                   runtime.params.empty() ? std::string("defaults") : runtime.params.dump());
        }
        if (runtime.scaleMeters) {
            m_runtime.emplace_back("Size (nominal)", formatDimensions(*runtime.scaleMeters));
        }
        if (m_model.report.size != glm::vec3(0.0f)) {
            m_runtime.emplace_back("Size (measured)", formatDimensions(m_model.report.size));
        }
        for (const auto& [role, socket] : runtime.sockets) {
            m_runtime.emplace_back("Socket " + role, socket);
        }
        if (runtime.videoSurface) {
            m_runtime.emplace_back("Video surface",
                                   runtime.videoSurface->aspectRatio + " on " + runtime.videoSurface->meshNode);
        }
        m_runtime.emplace_back("Collision", runtime.collision.empty() ? "-" : runtime.collision);
        m_runtime.emplace_back("LODs", runtime.lods.empty() ? "-" : join(runtime.lods));
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
    SubjectRevision m_revision;
    std::string m_path;
    std::string m_error;
    Rows m_identity;
    Rows m_source;
    Rows m_runtime;
};

class ScreenPanel final : public devui::Panel {
public:
    explicit ScreenPanel(PreviewPanelModel model)
        : m_model(std::move(model)), m_revision(m_model.subjectRevision), m_previews(m_model.textures) {}

    [[nodiscard]] const char* title() const noexcept override { return "Screen"; }

    void update() override { m_previews.sync(); }

    void draw() override {
        // The pattern textures belong to the cache, which releases them with the subject's content.
        if (m_revision.changed() || m_cacheRevision != m_model.textures.revision()) {
            refresh();
        }
        if (m_screens.empty()) {
            ImGui::TextDisabled("No ScreenSurface with a mesh");
            return;
        }
        for (const Shown& screen : m_screens) {
            ImGui::PushID(screen.id.c_str());
            ImGui::SeparatorText(screen.name.c_str());
            ImGui::TextUnformatted(screen.size.c_str());
            ImGui::SetItemTooltip("The screen rectangle as placed in the subject, and its aspect ratio (width / height "
                                  "%.3f)",
                                  screen.aspect);
            if (!screen.videoSurface.empty()) {
                ImGui::TextColored(screen.videoSurfaceMatches ? kOkColor : kWarningColor, "%s",
                                   screen.videoSurface.c_str());
            }
            if (screen.pattern != nullptr) {
                m_previews.thumbnail(*screen.pattern, kPatternEms * ImGui::GetFontSize(), "Test pattern");
            }
            ImGui::TextDisabled("%s", screen.caption.c_str());
            ImGui::PopID();
        }
    }

private:
    /// A screen as the panel shows it, formatted once per subject.
    struct Shown {
        std::string id;
        std::string name;
        std::string size;
        float aspect = 0.0f;
        std::string videoSurface;
        bool videoSurfaceMatches = false;
        const gl::Texture2D* pattern = nullptr;
        std::string caption;
    };

    void refresh() {
        m_cacheRevision = m_model.textures.revision();
        m_screens.clear();
        for (const ScreenSurface& screen : m_model.screens) {
            Shown shown;
            shown.id = screen.socketId;
            shown.name = screen.nodeName;
            shown.aspect = screen.size.y > 0.0f ? screen.size.x / screen.size.y : 0.0f;
            shown.size = fmt::format("{:.1f} x {:.1f} cm, {}", 100.0f * screen.size.x, 100.0f * screen.size.y,
                                     aspectRatioName(shown.aspect));
            describeVideoSurface(screen, shown);
            shown.pattern = &m_model.textures.pattern(kScreenPattern, "", 0, screen.patternSize, gl::ColorSpace::Srgb);
            shown.caption = fmt::format("{} {} x {} on {}", kScreenPattern, screen.patternSize.x, screen.patternSize.y,
                                        kScreenMaterialName);
            m_screens.push_back(std::move(shown));
        }
    }

    /// The manifest's video surface on this screen, and whether its aspect ratio matches the mesh.
    void describeVideoSurface(const ScreenSurface& screen, Shown& shown) const {
        const std::optional<ManifestCheck>& check = m_model.subject.manifest;
        if (!check || !check->manifest || !check->manifest->runtime.videoSurface) {
            return;
        }
        const assets::VideoSurfaceInfo& surface = *check->manifest->runtime.videoSurface;
        if (surface.meshNode != screen.socketId && surface.meshNode != screen.nodeName) {
            return;
        }
        const std::optional<float> declared = parseAspectRatio(surface.aspectRatio);
        shown.videoSurfaceMatches =
            declared && shown.aspect > 0.0f && std::abs(*declared - shown.aspect) <= 0.015f * *declared;
        shown.videoSurface = fmt::format("Manifest video surface {}{}", surface.aspectRatio,
                                         shown.videoSurfaceMatches ? "" : ": the mesh's aspect differs");
    }

    PreviewPanelModel m_model;
    SubjectRevision m_revision;
    std::uint64_t m_cacheRevision = 0;
    std::vector<Shown> m_screens;
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
