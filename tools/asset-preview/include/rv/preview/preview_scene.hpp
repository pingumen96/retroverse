#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/preview/preview_subject.hpp>
#include <rv/preview/view_mode.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::preview {

/// The library material of the screens of the preview, and the test pattern they show (style §19.3: a media applied to
/// the prefab): the CRT picture tube of the M0 test room with its color bars.
inline constexpr std::string_view kScreenMaterialName = "MAT_Screen_CRT";
inline constexpr std::string_view kScreenPattern = "color_bars";

/// The library materials the preview gives the part roles of the procedural prefab `kind`: those of the M0 test room
/// (assets/style_tests/reference_scenes/m0_testroom.json), so a prefab looks as it does there. Empty for an unknown
/// kind (its parts then show the fallback material).
[[nodiscard]] std::map<std::string, std::string> prefabRoleMaterials(std::string_view kind);
/// The pictures the preview shows on the picture roles of the prefab `kind`: kScreenPattern on a screen, a cover on a
/// cartridge label or a poster.
[[nodiscard]] std::map<std::string, scene::TextureSource> prefabRoleTextures(std::string_view kind);

/// The scene of `subject` (app::makeInspectionScene: the neutral studio of the model viewer, its three-point light rig
/// and background, sized to the subject's bounds): the model at the origin under the key of its file
/// (subject.modelFile), or the prefab with its parameters, role materials and pictures, or the placeholder of what
/// could not be loaded; no object for an empty preview.
[[nodiscard]] scene::SceneDescription makePreviewScene(const PreviewSubject& subject);
/// The model loader of makePreviewScene: serves the subject's model, which it already holds; fails without one (the
/// scene then shows the placeholder).
[[nodiscard]] scene::ModelLoader previewModelLoader(const PreviewSubject& subject);

/// A ScreenSurface socket of the subject and how the preview shows its test pattern.
struct ScreenSurface {
    std::string socketId;
    std::string nodeName;
    /// The screen rectangle in socket space (geometry::Socket::bounds).
    core::Aabb bounds;
    /// The socket's transform in the subject's space.
    glm::mat4 transform{1.0f};
    /// Size of the screen rectangle in the subject's space, meters (geometry::socketSize: with the node's scale, so a
    /// model exported in centimeters shows its real size).
    glm::vec3 size{0.0f};
    /// Size of the pattern for the screen's aspect as drawn (app::patternSizeFor): the texture the scene shows on it.
    glm::ivec2 patternSize{0};
    /// glTF models: the primitives of the screen's node (indices into assets::ModelData::meshes), drawn with the screen
    /// material and the pattern instead of their own. Empty for a prefab, whose "screen" role has them already
    /// (prefabRoleTextures).
    std::vector<std::uint32_t> modelMeshes;
};

/// The ScreenSurface sockets of `subject` that have a mesh to show a picture on.
[[nodiscard]] std::vector<ScreenSurface> screenSurfaces(const PreviewSubject& subject);

/// The name of a screen aspect ratio (width / height): the nearest of 1:1, 5:4, 4:3, 3:2, 16:10, 16:9 and 21:9 within
/// 1.5%, else "<aspect>:1" with two decimals ("1.42:1"); "-" when not finite and positive.
[[nodiscard]] std::string aspectRatioName(float aspect);
/// The width / height of a manifest's "<w>:<h>" aspect ratio (assets::VideoSurfaceInfo::aspectRatio); nothing for
/// another form or numbers that are not positive.
[[nodiscard]] std::optional<float> parseAspectRatio(std::string_view text);

} // namespace rv::preview
