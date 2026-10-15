#include <rv/preview/preview_overlay.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <span>
#include <utility>

#include <fmt/format.h>

#include <rv/core/math.hpp>

namespace rv::preview {
namespace {

/// sRGB colors of the labels.
constexpr glm::vec4 kBoundsLabel{1.0f, 0.82f, 0.35f, 1.0f};
constexpr glm::vec4 kReferenceLabel{0.86f, 0.86f, 0.82f, 1.0f};
constexpr glm::vec4 kAnchorLabel{1.0f, 1.0f, 1.0f, 1.0f};
constexpr glm::vec4 kScreenLabel{0.55f, 0.97f, 1.0f, 1.0f};
constexpr glm::vec4 kColliderLabel{1.0f, 0.62f, 0.86f, 1.0f};
constexpr glm::vec4 kOtherLabel{0.8f, 0.8f, 0.8f, 1.0f};

/// Gap between the subject and its scale references, meters.
constexpr float kMinReferenceGap = 0.25f;
/// Human-height marker: this far left of the reference cube, with ticks this wide every half meter.
constexpr float kMarkerGap = 0.5f;
constexpr float kMarkerTick = 0.1f;
constexpr float kMinGridHalfExtent = 2.0f;
constexpr float kMaxGridHalfExtent = 50.0f;

/// The subject's bounds when they can be measured, else a 1 m cube standing on the origin.
core::Aabb usableBounds(const core::Aabb& bounds) noexcept {
    const bool usable = !bounds.isEmpty() && core::isFinite(bounds.min) && core::isFinite(bounds.max);
    return usable ? bounds : core::Aabb::fromMinMax(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
}

float largestExtent(const core::Aabb& bounds) noexcept {
    const glm::vec3 size = bounds.size();
    return std::max({size.x, size.y, size.z});
}

/// The floor grid: the minor lines first, then the major lines and the axes of DebugLines::addGrid over them.
void addFloorGrid(float halfExtent, scene::DebugLines& lines) {
    const int steps = static_cast<int>(std::lround(halfExtent / kGridMinorSpacing));
    const int perMajor = static_cast<int>(std::lround(kGridMajorSpacing / kGridMinorSpacing));
    for (int step = -steps; step <= steps; ++step) {
        if (step % perMajor == 0) {
            continue;
        }
        const float offset = static_cast<float>(step) * kGridMinorSpacing;
        lines.addLine(glm::vec3(-halfExtent, 0.0f, offset), glm::vec3(halfExtent, 0.0f, offset),
                      overlay_colors::kGridMinor);
        lines.addLine(glm::vec3(offset, 0.0f, -halfExtent), glm::vec3(offset, 0.0f, halfExtent),
                      overlay_colors::kGridMinor);
    }
    lines.addGrid(halfExtent, kGridMajorSpacing);
}

/// The 1 m cube and the 1.70 m marker left of the subject.
void addScaleReferences(const core::Aabb& bounds, PreviewOverlay& overlay) {
    const core::Aabb cube = referenceCubeBounds(bounds);
    overlay.lines.addBox(cube, glm::mat4(1.0f), overlay_colors::kScaleReference);
    overlay.labels.push_back({glm::vec3(cube.center().x, cube.max.y, cube.max.z), "1 m cube", kReferenceLabel});

    const glm::vec3 foot(cube.min.x - kMarkerGap, 0.0f, cube.center().z);
    const glm::vec3 head = foot + glm::vec3(0.0f, kHumanHeight, 0.0f);
    overlay.lines.addLine(foot, head, overlay_colors::kScaleReference);
    for (const float height : {0.0f, 0.5f, 1.0f, 1.5f}) {
        const glm::vec3 at = foot + glm::vec3(0.0f, height, 0.0f);
        const glm::vec3 halfTick(0.5f * kMarkerTick, 0.0f, 0.0f);
        overlay.lines.addLine(at - halfTick, at + halfTick, overlay_colors::kScaleReference);
    }
    overlay.lines.addLine(head - glm::vec3(kMarkerTick, 0.0f, 0.0f), head + glm::vec3(kMarkerTick, 0.0f, 0.0f),
                          overlay_colors::kScaleReference);
    overlay.labels.push_back({head, "1.70 m", kReferenceLabel});
}

/// The bounds box and, with `dimensions`, its dimensions at the middle of three of its edges.
void addBounds(const core::Aabb& bounds, bool dimensions, PreviewOverlay& overlay) {
    overlay.lines.addBox(bounds, glm::mat4(1.0f), scene::debug_colors::kObjectBounds);
    if (!dimensions) {
        return;
    }
    const glm::vec3 size = bounds.size();
    const glm::vec3 center = bounds.center();
    overlay.labels.push_back(
        {glm::vec3(center.x, bounds.min.y, bounds.max.z), "W " + formatCentimeters(size.x), kBoundsLabel});
    overlay.labels.push_back(
        {glm::vec3(bounds.max.x, center.y, bounds.max.z), "H " + formatCentimeters(size.y), kBoundsLabel});
    overlay.labels.push_back(
        {glm::vec3(bounds.max.x, bounds.min.y, center.z), "D " + formatCentimeters(size.z), kBoundsLabel});
}

/// A drawn mesh of the subject and where it is drawn.
struct PlacedMesh {
    const geometry::MeshData* mesh = nullptr;
    glm::mat4 transform{1.0f};
};

/// The meshes the subject draws: a model's draw items, a prefab's parts.
std::vector<PlacedMesh> drawnMeshes(const PreviewSubject& subject) {
    std::vector<PlacedMesh> meshes;
    if (subject.model) {
        for (const assets::ModelDrawItem& item : subject.model->drawItems) {
            if (item.meshIndex < subject.model->meshes.size()) {
                meshes.push_back({&subject.model->meshes[item.meshIndex], item.world});
            }
        }
    } else if (subject.prefab) {
        for (const geometry::PrefabPart& part : subject.prefab->parts) {
            meshes.push_back({&part.mesh, glm::mat4(1.0f)});
        }
    }
    return meshes;
}

/// A point as a sortable key.
using PointKey = std::array<float, 3>;

PointKey keyOf(const glm::vec3& point) noexcept {
    return {point.x, point.y, point.z};
}

/// The triangles that share an edge: how many, and whether they all lie in one plane.
struct EdgeFaces {
    glm::vec3 firstNormal{0.0f};
    int faces = 0;
    bool coplanar = true;
};

/// The outline of the triangles of `mesh` placed by `transform`: every edge once, edges matched by position (also
/// between triangles that do not share vertices, as the flat faces of a box), except those between two triangles of one
/// plane (the diagonals of quads), so a box proxy shows its 12 sides.
void addMeshOutline(const geometry::MeshData& mesh, const glm::mat4& transform, const glm::vec4& color,
                    std::size_t& budget, scene::DebugLines& lines) {
    std::map<std::pair<PointKey, PointKey>, EdgeFaces> edges;
    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const std::array<std::uint32_t, 3> corners{mesh.indices[index], mesh.indices[index + 1],
                                                   mesh.indices[index + 2]};
        if (std::any_of(corners.begin(), corners.end(), [&](std::uint32_t c) { return c >= mesh.vertices.size(); })) {
            continue;
        }
        const glm::vec3 p0 = mesh.vertices[corners[0]].position;
        const glm::vec3 cross =
            glm::cross(mesh.vertices[corners[1]].position - p0, mesh.vertices[corners[2]].position - p0);
        const float area = glm::length(cross);
        if (!std::isfinite(area) || area < 1e-12f) {
            continue; // degenerate: no plane
        }
        const glm::vec3 normal = cross / area;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const PointKey from = keyOf(mesh.vertices[corners[corner]].position);
            const PointKey to = keyOf(mesh.vertices[corners[(corner + 1) % 3]].position);
            EdgeFaces& faces = edges[{std::min(from, to), std::max(from, to)}];
            if (faces.faces == 0) {
                faces.firstNormal = normal;
            } else if (glm::dot(faces.firstNormal, normal) < 0.999f) {
                faces.coplanar = false;
            }
            ++faces.faces;
        }
    }
    for (const auto& [edge, faces] : edges) {
        if (faces.faces == 2 && faces.coplanar) {
            continue; // inside a flat face
        }
        if (budget == 0) {
            return;
        }
        --budget;
        const auto& [from, to] = edge;
        lines.addLine(glm::vec3(transform * glm::vec4(from[0], from[1], from[2], 1.0f)),
                      glm::vec3(transform * glm::vec4(to[0], to[1], to[2], 1.0f)), color);
    }
}

/// The primitives of the model node a mesh-backed socket belongs to.
std::span<const geometry::MeshData> socketMeshes(const assets::ModelData& model, const geometry::Socket& socket) {
    const auto node = std::find_if(model.nodes.begin(), model.nodes.end(), [&](const assets::ModelNode& n) {
        return n.name == socket.nodeName && n.mesh == socket.meshIndex;
    });
    if (node == model.nodes.end() || !socket.meshIndex || *socket.meshIndex >= model.meshes.size()) {
        return {};
    }
    const std::size_t first = *socket.meshIndex;
    const std::size_t count = std::min<std::size_t>(node->meshCount, model.meshes.size() - first);
    return std::span<const geometry::MeshData>(model.meshes).subspan(first, count);
}

glm::vec4 labelColor(geometry::SocketKind kind) noexcept {
    switch (kind) {
    case geometry::SocketKind::Anchor:
        return kAnchorLabel;
    case geometry::SocketKind::ScreenSurface:
        return kScreenLabel;
    case geometry::SocketKind::ColliderProxy:
        return kColliderLabel;
    case geometry::SocketKind::ShelfSlot:
    case geometry::SocketKind::Other:
        break;
    }
    return kOtherLabel;
}

/// Socket axes and names, screen outlines, collision proxies (a model's ColliderProxy meshes, a prefab's collider
/// boxes) as wireframes.
void addSockets(const PreviewSubject& subject, const core::Aabb& bounds, PreviewOverlay& overlay) {
    std::size_t edgeBudget = kMaxColliderEdges;
    for (const geometry::Socket& socket : subject.sockets) {
        overlay.lines.addAxes(socket.transform, scene::socketAxisLength(socket, bounds));
        if (socket.kind == geometry::SocketKind::ScreenSurface) {
            overlay.lines.addBox(socket.bounds, socket.transform, overlay_colors::kScreen);
        }
        if (socket.kind == geometry::SocketKind::ColliderProxy && subject.model) {
            for (const geometry::MeshData& mesh : socketMeshes(*subject.model, socket)) {
                addMeshOutline(mesh, socket.transform, overlay_colors::kCollider, edgeBudget, overlay.lines);
            }
        }
        if (socket.kind != geometry::SocketKind::ShelfSlot) {
            overlay.labels.push_back({glm::vec3(socket.transform[3]), socket.nodeName, labelColor(socket.kind)});
        }
    }
    if (subject.prefab) {
        for (const geometry::PrefabCollider& collider : subject.prefab->colliders) {
            overlay.lines.addBox(collider.box, glm::mat4(1.0f), overlay_colors::kCollider);
        }
    }
}

/// A short line along the normal of every vertex (of every n-th one beyond kMaxNormalLines).
void addNormals(const PreviewSubject& subject, const core::Aabb& bounds, PreviewOverlay& overlay) {
    const std::vector<PlacedMesh> meshes = drawnMeshes(subject);
    std::size_t vertices = 0;
    for (const PlacedMesh& placed : meshes) {
        vertices += placed.mesh->vertices.size();
    }
    const std::size_t stride = std::max<std::size_t>(1, (vertices + kMaxNormalLines - 1) / kMaxNormalLines);
    const float length = std::clamp(0.03f * largestExtent(bounds), 0.002f, 0.05f);
    std::size_t counter = 0;
    for (const PlacedMesh& placed : meshes) {
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(placed.transform)));
        for (const geometry::Vertex& vertex : placed.mesh->vertices) {
            if (counter++ % stride != 0) {
                continue;
            }
            const glm::vec3 position(placed.transform * glm::vec4(vertex.position, 1.0f));
            const glm::vec3 normal = normalMatrix * vertex.normal;
            const float normalLength = glm::length(normal);
            if (std::isfinite(normalLength) && normalLength > 1e-6f) {
                overlay.lines.addLine(position, position + normal * (length / normalLength), overlay_colors::kNormals);
            }
        }
    }
}

} // namespace

core::Aabb referenceCubeBounds(const core::Aabb& bounds) noexcept {
    const core::Aabb subject = usableBounds(bounds);
    const float gap = std::max(kMinReferenceGap, 0.1f * subject.size().x);
    const float right = subject.min.x - gap;
    const float depthCenter = subject.center().z;
    return core::Aabb::fromMinMax(glm::vec3(right - 1.0f, 0.0f, depthCenter - 0.5f),
                                  glm::vec3(right, 1.0f, depthCenter + 0.5f));
}

float gridHalfExtent(const core::Aabb& bounds) noexcept {
    const core::Aabb subject = usableBounds(bounds);
    const core::Aabb cube = referenceCubeBounds(subject);
    const float reach = std::max({std::abs(subject.min.x), std::abs(subject.max.x), std::abs(subject.min.z),
                                  std::abs(subject.max.z), std::abs(cube.min.x) + kMarkerGap + kMarkerTick});
    return std::clamp(std::ceil(reach + 0.5f), kMinGridHalfExtent, kMaxGridHalfExtent);
}

PreviewOverlay buildPreviewOverlay(const PreviewSubject& subject, const OverlaySelection& selection) {
    PreviewOverlay overlay;
    const core::Aabb bounds = usableBounds(subject.bounds);
    if (selection.grid) {
        addFloorGrid(gridHalfExtent(bounds), overlay.lines);
    }
    if (selection.scaleReferences) {
        addScaleReferences(bounds, overlay);
    }
    if (!subject.target) {
        return overlay; // an empty preview: the studio only
    }
    if (selection.nodeBounds) {
        for (const PlacedMesh& placed : drawnMeshes(subject)) {
            overlay.lines.addBox(placed.mesh->bounds, placed.transform, overlay_colors::kNodeBounds);
        }
    }
    if (selection.bounds) {
        addBounds(bounds, !subject.placeholder, overlay);
    }
    if (selection.normals) {
        addNormals(subject, bounds, overlay);
    }
    if (selection.pivot) {
        overlay.lines.addAxes(glm::mat4(1.0f), std::clamp(0.25f * largestExtent(bounds), 0.001f, 0.5f));
        overlay.labels.push_back({glm::vec3(0.0f), "pivot", kAnchorLabel});
    }
    if (selection.sockets) {
        addSockets(subject, bounds, overlay);
    }
    return overlay;
}

std::string formatCentimeters(float meters) {
    return fmt::format("{:.1f} cm", 100.0f * meters);
}

std::string formatDimensions(const glm::vec3& size) {
    return fmt::format("{:.1f} x {:.1f} x {:.1f} cm", 100.0f * size.x, 100.0f * size.y, 100.0f * size.z);
}

std::optional<glm::vec2> projectToViewport(const glm::mat4& viewProjection, const glm::vec3& point,
                                           const glm::vec2& size) noexcept {
    const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
    if (!(clip.w > 1e-6f)) {
        return std::nullopt; // behind the eye (or a degenerate matrix)
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (!core::isFinite(ndc) || std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f || std::abs(ndc.z) > 1.0f) {
        return std::nullopt;
    }
    return glm::vec2((ndc.x + 1.0f) * 0.5f * size.x, (1.0f - ndc.y) * 0.5f * size.y);
}

} // namespace rv::preview
