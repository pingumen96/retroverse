#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// Material returned for unknown names (style §21: missing assets never break the scene).
inline constexpr std::string_view kFallbackMaterialName = "MAT_Fallback_Magenta";

/// Parses one material object (library entries and inline materials of scene files). `name` becomes
/// MaterialDesc::name; `context` is the JSON path of the object (e.g. "materials.json:/materials/MAT_Wood_Oak")
/// and errors append the offending key ("<context>/roughness").
///
/// Keys (all optional; unknown keys are errors):
///   "shading": "pbr" | "unlit"                       (default "pbr")
///   "baseColor": "#RRGGBB"                           sRGB hex art-direction color, converted to linear
///   "baseColorLinear": [r, g, b]                     linear components in [0, 1]; exclusive with "baseColor"
///   "alpha": 1.0                                     base color alpha in [0, 1]
///   "metallic", "roughness", "occlusionStrength"     in [0, 1]
///   "emissive": "#RRGGBB"                            sRGB hex, converted to linear (default black)
///   "emissiveStrength": >= 0, "normalScale": finite number
///   "alphaMode": "opaque" | "mask" | "blend", "alphaCutoff": [0, 1], "doubleSided": bool
///   "baseColorTexture", "metallicRoughnessTexture", "normalTexture", "occlusionTexture", "emissiveTexture":
///       asset-relative paths with forward slashes (no "..", not absolute), stored in TextureRef::path, or a
///       procedural texture "procedural:<surface>/<map>" of the surface catalog (rv/assets/surface_textures.hpp; the
///       surface and the map must exist), stored in TextureRef::procedural as "<surface>/<map>".
///   "crt": {"lineCount", "scanlineStrength", "maskTriadCount", "maskStrength", "curvature", "vignette",
///       "glassReflection"}: the CRT_Subtle effect of an unlit screen (CrtEffect, its ranges; missing keys keep its
///       defaults), only with "shading": "unlit".
/// Defaults are those of MaterialDesc. Shared convention for "unlit" presets (screens, neon): the renderer outputs
/// the base color (times its texture) scaled by emissiveStrength, so MAT_Screen_Emissive (white, strength 2)
/// shows its video or test pattern at twice its nominal brightness.
/// Errors: ParseError (type, unknown key or value, an unknown procedural texture, "crt" on a lit material),
/// OutOfRange (number out of range), InvalidArgument (texture path).
[[nodiscard]] Expected<MaterialDesc> parseMaterialDesc(const nlohmann::json& object, std::string_view name,
                                                       std::string_view context);

/// Named material presets (style §3.2), loaded from assets/materials/base/materials.json:
///   {"schemaVersion": 1, "materials": {"MAT_Wood_Oak": {...}, ...}}
/// Lookups are case-sensitive. The fallback is the file's MAT_Fallback_Magenta entry, or a built-in unlit
/// magenta material when the file does not define it. A default-constructed library is empty and only has the
/// built-in fallback (useful when the file cannot be loaded).
class MaterialLibrary {
public:
    MaterialLibrary();

    /// Reads and parses a library file. Error contexts start with the UTF-8 path of the file.
    [[nodiscard]] static Expected<MaterialLibrary> loadFromFile(const std::filesystem::path& path);
    /// Parses a library document; `context` names its source (usually the file path).
    [[nodiscard]] static Expected<MaterialLibrary> fromJson(const nlohmann::json& document, std::string_view context);

    /// The material called `name`, or nullptr. The pointer stays valid as long as the library is alive.
    [[nodiscard]] const MaterialDesc* find(std::string_view name) const;
    /// The material called `name`, or fallback(). A missing name is logged as a warning once per name
    /// (thread-safe; copies of a library share the record of reported names).
    [[nodiscard]] const MaterialDesc& getOrFallback(std::string_view name) const;
    /// The library's MAT_Fallback_Magenta entry, or the built-in unlit magenta material.
    [[nodiscard]] const MaterialDesc& fallback() const;
    /// Material names in ascending order.
    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] std::size_t size() const noexcept { return m_materials.size(); }

private:
    struct MissingNames;

    std::map<std::string, MaterialDesc, std::less<>> m_materials;
    std::shared_ptr<MissingNames> m_missing; // names already reported by getOrFallback()
};

} // namespace rv::assets
