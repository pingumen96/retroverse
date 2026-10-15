#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/light.hpp>

namespace rv::scene {

/// Scene description files (assets/style_tests/reference_scenes/*.json), schema version 1.
///
/// Plain data for the application, which builds prefabs, primitives, models and materials from it. Lengths in
/// meters, angles in degrees, colors are opaque sRGB hex strings, exactly "#RRGGBB" (no alpha), converted to
/// linear RGB. Comments are allowed; unknown keys are errors everywhere (typos never pass silently). Optional keys
/// take the defaults listed below, which are also the defaults of the structs.
///
///   {
///     "schemaVersion": 1,                          required, must be 1
///     "name": "m0_testroom",                       required, non-empty
///     "camera": {                                  optional
///       "position": [0, 1.65, 0],                  EYE position (the walker's feet are eyeHeight below)
///       "yawDeg": 0, "pitchDeg": 0,                Camera conventions (yaw 0 looks at -Z); |pitchDeg| <= 89
///       "horizontalFovDeg": 75,                    [1, 179]
///       "near": 0.05, "far": 100                   0 < near < far
///     },
///     "environment": {                             optional
///       "clearColor": "#383735",                   background radiance where nothing is drawn (exposed and
///                                                  tonemapped like an unlit surface)
///       "ambientSky": "#FFFFFF", "ambientGround": "#808080",
///       "ambientIntensity": 0.2,                   >= 0, scales the hemispheric sky/ground ambient
///       "exposure": 1.0                            > 0, applied before tonemapping
///     },
///     "lights": [{                                 optional
///       "name": "ceiling",                         optional
///       "type": "point" | "spot" | "directional",  required
///       "position": [x, y, z],                     required for point and spot lights
///       "direction": [x, y, z] or "target": [x, y, z],  not both; required for spot and directional lights;
///                                                  normalized (the target must differ from the position)
///       "color": "#FFFFFF", "intensity": 1.0,      intensity >= 0
///       "range": 10.0,                             > 0 for point and spot lights
///       "innerConeDeg": 25, "outerConeDeg": 35,    0 <= inner <= outer < 90 (inner == outer: hard edge)
///       "castsShadows": false
///     }],
///     "objects": [{                                optional
///       "id": "shelf_main",                        required, non-empty, unique in the file
///       "prefab": "<kind>" | "primitive": "<kind>" | "model": "<asset path>",   exactly one
///       "params": {...},                           builder parameters, kept verbatim (shared parameter contract)
///       "position": [0, 0, 0], "rotationDeg": [0, 0, 0],  rotation R = Ry * Rx * Rz
///       "scale": [1, 1, 1] or 1,                   components > 0 (mirroring would flip the winding)
///       "material": "MAT_x",                       material of every role without an entry in "materials"
///       "materials": {"<role>": "MAT_x"},          per-role materials
///       "textures": {"<role>": {"pattern": "cover", "title": "...", "seed": 3} or {"file": "textures/x.png"}},
///       "inlineMaterial": {...},                   material-library entry kept verbatim; excludes "material"
///       "grid": {"counts": [nx, ny, nz], "spacing": [sx, sy, sz]},   counts >= 1, both keys required
///       "castsShadows": true, "collides": true
///     }]
///   }
///
/// Object transform = translate(position) * Ry * Rx * Rz * scale. A grid expands into nx * ny * nz instances
/// transform * translate(ix * sx, iy * sy, iz * sz) (spacing in the object's local space), x fastest, then y,
/// then z. The whole scene may hold at most kMaxSceneInstances instances (an object without grid counts as 1).
/// Asset paths (models, texture files) are relative, use '/' and never contain ".." or a drive/root.
///
/// Every validation error is rv::ErrorCode::ValidationFailed and carries "<context>:<JSON pointer>" as context,
/// e.g. "m0_testroom.json:/objects/3/params"; a cross-field error (near/far, inner/outer cone) points at the key
/// that is present in the file. loadSceneDescription also returns the I/O errors of rv::core::readJsonFile and
/// ParseError (context = file path) for malformed JSON, including number literals that overflow a double.

inline constexpr int kSceneSchemaVersion = 1;
inline constexpr std::size_t kMaxSceneInstances = 100000;
/// Procedural texture patterns understood by the asset layer (assets::test patterns).
inline constexpr std::array<std::string_view, 3> kTexturePatternNames{"color_bars", "checker", "cover"};

/// Initial camera of the scene (Camera conventions, angles in degrees).
struct CameraSetup {
    /// Eye position in meters.
    glm::vec3 position{0.0f, 1.65f, 0.0f};
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    CameraLens lens;
};

/// Linear-RGB environment terms for the renderer.
struct EnvironmentSetup {
    /// "#383735". Radiance of the background, seen where no surface is drawn: the renderer exposes and tonemaps it
    /// like the surfaces, so it reads on the same scale as the lit scene.
    glm::vec3 clearColor{0.0395462f, 0.0382044f, 0.0356013f};
    glm::vec3 ambientSky{1.0f};          ///< "#FFFFFF"
    glm::vec3 ambientGround{0.2158605f}; ///< "#808080"
    float ambientIntensity = 0.2f;
    float exposure = 1.0f;
};

/// Texture bound to one material role of an object (label of a cartridge, CRT screen, poster picture). The
/// application uses it as the base color texture of the material assigned to that role.
struct TextureSource {
    enum class Kind : std::uint8_t { File, Pattern };

    Kind kind = Kind::Pattern;
    /// Kind::File: asset-relative UTF-8 path.
    std::string path;
    /// Kind::Pattern: one of kTexturePatternNames.
    std::string pattern;
    /// Text drawn by patterns that show one (the "cover" title); may be empty.
    std::string title;
    /// Seed of the pattern's deterministic variations.
    std::uint32_t seed = 0;
};

/// One entry of "objects": a procedural prefab, a primitive or a glTF model, placed once or as a grid.
struct SceneObject {
    enum class Source : std::uint8_t { Prefab, Primitive, Model };

    std::string id;
    Source source = Source::Primitive;
    /// Prefab or primitive kind ("shelf", "box", ...); empty for models.
    std::string kind;
    /// Source::Model: asset-relative glTF path.
    std::string modelPath;
    /// Builder parameters, verbatim (always an object).
    nlohmann::json params = nlohmann::json::object();
    /// Local -> world transform of the object (or of the grid origin).
    glm::mat4 transform{1.0f};
    /// Expanded grid instances; empty = a single instance at `transform`.
    std::vector<glm::mat4> instances;
    /// Library material ("MAT_x") of every role without an entry in materialsByRole; may be empty.
    std::string material;
    /// Library material per material role (prefab part roles such as "label" or "screen").
    std::map<std::string, std::string> materialsByRole;
    std::map<std::string, TextureSource> texturesByRole;
    /// Material-library entry (an object) or null when absent.
    nlohmann::json inlineMaterial;
    bool castsShadows = true;
    /// The application derives collider boxes from the built geometry (per wall for a room shell).
    bool collides = true;

    /// Every instance transform: `instances`, or `transform` alone when there is no grid.
    [[nodiscard]] std::span<const glm::mat4> instanceTransforms() const noexcept;
    /// Named material of a role: materialsByRole[role], else `material`; empty when neither is set (the
    /// application then uses inlineMaterial when present, else its fallback material).
    [[nodiscard]] std::string_view materialNameForRole(std::string_view role) const;
};

/// Validated content of one scene file; objects and lights keep the file order.
struct SceneDescription {
    std::string name;
    CameraSetup camera;
    EnvironmentSetup environment;
    std::vector<Light> lights;
    std::vector<SceneObject> objects;

    /// Object with the given id, or nullptr.
    [[nodiscard]] const SceneObject* findObject(std::string_view id) const noexcept;
};

/// Validates and converts a parsed document. `context` prefixes every error location (usually the file name).
[[nodiscard]] Expected<SceneDescription> parseSceneDescription(const nlohmann::json& document,
                                                               std::string_view context);

/// Reads, parses and validates a scene file; error locations are prefixed with the file name.
[[nodiscard]] Expected<SceneDescription> loadSceneDescription(const std::filesystem::path& path);

} // namespace rv::scene
