#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/aabb.hpp>
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
///       "fillColor": "#FFFFFF", "fillIntensity": 0,  >= 0: an even fill added to the ambient whatever the normal
///                                                  (style §4.1's ambient/fill layer: the light an area's lamps
///                                                  bounce around it, which keeps upper walls and ceilings above
///                                                  black); occluded like the ambient
///       "exposure": 1.0,                           > 0, applied before tonemapping
///       "bloomStrength": 0.06,                     [0, 1], glow of screens, lamp shades, neon, highlights (0: none)
///       "bloomRadius": 0.7,                        [0, 1], how far the glow spreads (0: a few pixels)
///       "vignette": 0.05,                          [0, 1], darkening of the view's corners (0: none)
///       "ssao": {                                  optional: screen-space ambient occlusion of the ambient light
///         "enabled": true,
///         "radius": 0.4,                           (0, 5] meters: how far from a surface occluders darken it
///         "intensity": 1.0                         [0, 4]: exponent of the unoccluded share (0: no darkening)
///       }
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
///       "castsShadows": false,                     the first light that casts shadows is the key light: a spot
///                                                  or directional one is shadow-mapped, a point light casts none
///       "shadowStrength": 1.0,                     [0, 1]: share of the light its shadows take away
///       "area": "room:snes"                        the area it lights: required in a scene with "areas", an
///                                                  error in one without
///     }],
///     "areas": [{                                  optional (I4 step 3, ADR-0013 §9): parts of the scene that each
///                                                  hold their own lights, such as a floor's rooms and its hall
///       "id": "room:snes",                         required, non-empty, unique in the file
///       "boxes": [{"min": [x, y, z], "max": [x, y, z]}]   required, at least one; min < max on every axis
///       "environment": {                           optional (I6 step 1, ADR-0014 §4): the area's own lit terms,
///         "exposure": 1.5, "ambientSky": "#...",   each optional, the scene's "environment" for those it lacks;
///         "ambientGround": "#...",                 the views ease to the eye's area's over about half a second
///         "ambientIntensity": 0.3,                 (scene_areas.hpp's EnvironmentEase)
///         "fillColor": "#...", "fillIntensity": 0.1,
///         "bloomStrength": 0.28                    [0, 1] (I6 step 3: a theme pack's bloom, the arcade's)
///       }
///     }],
///     "objects": [{                                optional
///       "id": "shelf_main",                        required, non-empty, unique in the file
///       "prefab": "<kind>" | "primitive": "<kind>" | "model": "<asset path>",   exactly one
///       "params": {...},                           builder parameters, kept verbatim (shared parameter contract)
///       "position": [0, 0, 0], "rotationDeg": [0, 0, 0],  rotation R = Ry * Rx * Rz
///       "scale": [1, 1, 1] or 1,                   components > 0 (mirroring would flip the winding)
///       "material": "MAT_x",                       material of every role without an entry in "materials"
///       "materials": {"<role>": "MAT_x"},          per-role materials
///       "textures": {"<role>": {"pattern": "cover", "title": "...", "seed": 3} or {"file": "textures/x.png"} or
///                    {"procedural": "<surface>/<map>"}},   a procedural texture of rv/assets/surface_textures.hpp
///       "inlineMaterial": {...},                   material-library entry kept verbatim; excludes "material"
///       "grid": {"counts": [nx, ny, nz], "spacing": [sx, sy, sz]},   counts >= 1, both keys required
///       "castsShadows": true, "collides": true
///     }]
///   }
///
/// Areas: the area that holds the viewer's eye (the first whose boxes contain it, else the last one that held it) is the
/// one whose lights the views use: the renderer packs that area's lights nearest the eye, its key light (its first
/// light with castsShadows) shadowed, then a few lights of each neighbouring area (one whose boxes touch the eye's
/// area's, as a room's doorway passage touches its hall) near the face they share, never shadowed; the other lights
/// are left out (scene_areas.hpp). The ambient, fill and exposure are the eye's area's (its "environment"). A scene
/// without areas packs all its lights in list order, as it always did.
///
/// "model" objects are drawn with the materials and textures of their glTF file: their "material", "materials",
/// "textures" and "inlineMaterial" keys are ignored, with a warning that names them; "params" (builder parameters)
/// has no meaning for a model and is not read either.
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
/// Largest ambient occlusion radius (meters) and intensity a scene may ask for (SsaoSetup).
inline constexpr float kMaxSsaoRadius = 5.0f;
inline constexpr float kMaxSsaoIntensity = 4.0f;
/// Procedural texture patterns understood by the asset layer (assets::test patterns).
inline constexpr std::array<std::string_view, 12> kTexturePatternNames{
    "color_bars",     "checker",       "cover",          "sign",         "print",      "neon",
    "poster_memphis", "poster_lounge", "poster_console", "poster_flyer", "cork_board", "terminal"};

/// Initial camera of the scene (Camera conventions, angles in degrees).
struct CameraSetup {
    /// Eye position in meters.
    glm::vec3 position{0.0f, 1.65f, 0.0f};
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    CameraLens lens;
};

/// Screen-space ambient occlusion of a view (style §4.1 contact cues): the ambient light is darkened in corners, in
/// creases and where objects meet, estimated per pixel from the depth of the view. It never darkens the direct light
/// (that is the key light's shadow).
struct SsaoSetup {
    bool enabled = true;
    /// Meters, (0, kMaxSsaoRadius]: how far from a surface occluders darken it; 0.4 m reaches into the corners of a
    /// room and under furniture without darkening whole walls.
    float radius = 0.4f;
    /// [0, kMaxSsaoIntensity]: exponent applied to the unoccluded share of the ambient light (1 as estimated, larger
    /// darker, 0 no darkening).
    float intensity = 1.0f;

    bool operator==(const SsaoSetup&) const = default;
};

/// Linear-RGB environment terms and the post-processing of the view for the renderer.
struct EnvironmentSetup {
    /// "#383735". Radiance of the background, seen where no surface is drawn: the renderer exposes and tonemaps it
    /// like the surfaces, so it reads on the same scale as the lit scene.
    glm::vec3 clearColor{0.0395462f, 0.0382044f, 0.0356013f};
    glm::vec3 ambientSky{1.0f};          ///< "#FFFFFF"
    glm::vec3 ambientGround{0.2158605f}; ///< "#808080"
    float ambientIntensity = 0.2f;
    /// Linear RGB and its scale: an even fill added to the hemispheric ambient whatever the surface's normal (0: none).
    glm::vec3 fillColor{1.0f};
    float fillIntensity = 0.0f;
    float exposure = 1.0f;
    /// How much of the light that blooms spreads into a soft glow around it (style §4.3: localized bloom): all the
    /// light emissive surfaces give off (screens, lamp shades, neon) and the highlights of the lights on glossy
    /// surfaces, moved from them into the glow; 0.06 is subtle. Lit paper, wood and walls never bloom.
    float bloomStrength = 0.06f;
    /// How far the glow spreads: the weight the bloom chain gives its wider levels, from a few pixels around a bright
    /// surface (0) to most of the view (1).
    float bloomRadius = 0.7f;
    /// Darkening of the view's corners (style §18.2 postprocess_vignette): 0.05 takes 5% of the radiance there.
    float vignette = 0.05f;
    /// Ambient occlusion of the views ("ssao").
    SsaoSetup ssao;
};

/// Texture bound to one material role of an object (label of a cartridge, CRT screen, poster picture, a surface). The
/// application uses it as the base color texture of the material assigned to that role: files and patterns as single
/// pictures (clamped to their edges), procedural textures as the tiling surfaces they are (repeated).
struct TextureSource {
    enum class Kind : std::uint8_t { File, Pattern, Procedural };

    Kind kind = Kind::Pattern;
    /// Kind::File: asset-relative UTF-8 path.
    std::string path;
    /// Kind::Pattern: one of kTexturePatternNames.
    std::string pattern;
    /// Text drawn by patterns that show one (the "cover" title, the "sign" lettering); may be empty.
    std::string title;
    /// Seed of the pattern's deterministic variations.
    std::uint32_t seed = 0;
    /// Kind::Procedural: "<surface>/<map>" of the surface catalog (assets::parseProceduralTextureName).
    std::string procedural;
};

/// One entry of "areas": a part of the scene that holds its own lights (a room extended through its doorway passage, a
/// floor's hall), as the boxes it spans in world space.
/// The lit terms of one area that may differ from the scene's environment (each absent one is the scene's): what the
/// views ease between when the eye changes areas (I6 step 1, ADR-0014 §4).
struct AreaEnvironment {
    std::optional<float> exposure;
    std::optional<glm::vec3> ambientSky;
    std::optional<glm::vec3> ambientGround;
    std::optional<float> ambientIntensity;
    std::optional<glm::vec3> fillColor;
    std::optional<float> fillIntensity;
    std::optional<float> bloomStrength;

    /// `scene` with these terms in place of its own.
    [[nodiscard]] EnvironmentSetup over(const EnvironmentSetup& scene) const;

    bool operator==(const AreaEnvironment&) const = default;
};

struct SceneArea {
    std::string id;
    /// World space, meters; each with min < max on every axis.
    std::vector<core::Aabb> boxes;
    /// Its lit terms where they differ from the scene's.
    AreaEnvironment environment;

    /// True when one of the boxes holds `point` (its faces included).
    [[nodiscard]] bool contains(const glm::vec3& point) const noexcept;

    bool operator==(const SceneArea&) const = default;
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
    /// Empty: every light lights the whole scene. Otherwise every light names one of them (Light::area).
    std::vector<SceneArea> areas;

    /// Object with the given id, or nullptr.
    [[nodiscard]] const SceneObject* findObject(std::string_view id) const noexcept;
    /// The index of the area `id` in `areas`, or nothing.
    [[nodiscard]] std::optional<std::size_t> findArea(std::string_view id) const noexcept;
};

/// Validates and converts a parsed document. `context` prefixes every error location (usually the file name).
[[nodiscard]] Expected<SceneDescription> parseSceneDescription(const nlohmann::json& document,
                                                               std::string_view context);

/// Reads, parses and validates a scene file; error locations are prefixed with the file name.
[[nodiscard]] Expected<SceneDescription> loadSceneDescription(const std::filesystem::path& path);

} // namespace rv::scene
