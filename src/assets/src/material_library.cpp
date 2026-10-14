#include <rv/assets/material_library.hpp>

#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>

#include "json_util.hpp"
#include "path_util.hpp"

namespace rv::assets {

struct MaterialLibrary::MissingNames {
    std::mutex mutex;
    std::set<std::string, std::less<>> names;
};

namespace {

const MaterialDesc& builtinFallback() {
    static const MaterialDesc material = [] {
        MaterialDesc fallback;
        fallback.name = std::string(kFallbackMaterialName);
        fallback.shading = ShadingModel::Unlit;
        fallback.baseColorFactor = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f); // #FF00FF is linear (1, 0, 1)
        fallback.roughnessFactor = 1.0f;
        return fallback;
    }();
    return material;
}

/// "#RRGGBB" (the '#' is optional) converted from sRGB to linear. Alpha digits are refused on purpose: material
/// transparency is authored with "alpha".
Expected<glm::vec3> readHexColor(const nlohmann::json& value, std::string_view context) {
    auto text = detail::readString(value, context);
    if (!text) {
        return propagate(text.error());
    }
    const std::string_view digits = std::string_view(*text).substr(text->starts_with('#') ? 1 : 0);
    if (digits.size() != 6) {
        return makeError(ErrorCode::ParseError, "expected an sRGB color \"#RRGGBB\", found \"" + *text + "\"",
                         std::string(context));
    }
    auto color = core::hexToLinearRgb(*text);
    if (!color) {
        return makeError(ErrorCode::ParseError, color.error().message + ": \"" + *text + "\"", std::string(context));
    }
    return color;
}

Expected<std::string> readEnum(const nlohmann::json& value, std::initializer_list<std::string_view> allowed,
                               std::string_view context) {
    auto text = detail::readString(value, context);
    if (!text) {
        return text;
    }
    std::string list;
    for (const std::string_view option : allowed) {
        if (*text == option) {
            return text;
        }
        list += list.empty() ? "" : ", ";
        list += "\"" + std::string(option) + "\"";
    }
    return makeError(ErrorCode::ParseError, "unknown value \"" + *text + "\" (expected " + list + ")",
                     std::string(context));
}

Status readTexture(const nlohmann::json& object, std::string_view key, std::string_view context, TextureRef& out) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return ok();
    }
    const std::string where = detail::pointerChild(context, key);
    auto text = detail::readString(*it, where);
    if (!text) {
        return propagate(text.error());
    }
    auto path = detail::normalizeRelativePath(*text, false, where);
    if (!path) {
        return propagate(path.error());
    }
    out.path = std::move(*path);
    return ok();
}

using detail::readOptional;

Expected<MaterialDesc> parseMaterial(const nlohmann::json& object, std::string_view name, std::string_view context) {
    if (auto status = detail::requireObject(object, context); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(
            object,
            {"shading", "baseColor", "baseColorLinear", "alpha", "metallic", "roughness", "emissive",
             "emissiveStrength", "normalScale", "occlusionStrength", "alphaMode", "alphaCutoff", "doubleSided",
             "baseColorTexture", "metallicRoughnessTexture", "normalTexture", "occlusionTexture", "emissiveTexture"},
            context);
        !status) {
        return propagate(status.error());
    }

    MaterialDesc material;
    material.name = std::string(name);
    const auto unit = [](const nlohmann::json& value, const std::string& where) {
        return detail::readNumberInRange(value, 0.0f, 1.0f, where);
    };
    const auto nonNegative = [](const nlohmann::json& value, const std::string& where) {
        return detail::readNumberInRange(value, 0.0f, std::numeric_limits<float>::max(), where);
    };
    const auto finite = [](const nlohmann::json& value, const std::string& where) {
        return detail::readNumber(value, where);
    };

    std::string shading = "pbr";
    const auto shadingReader = [](const nlohmann::json& value, const std::string& where) {
        return readEnum(value, {"pbr", "unlit"}, where);
    };
    if (auto status = readOptional(object, "shading", context, shadingReader, shading); !status) {
        return propagate(status.error());
    }
    material.shading = shading == "unlit" ? ShadingModel::Unlit : ShadingModel::Pbr;

    if (object.contains("baseColor") && object.contains("baseColorLinear")) {
        return makeError(ErrorCode::ParseError, "\"baseColor\" and \"baseColorLinear\" are mutually exclusive",
                         detail::pointerChild(context, "baseColorLinear"));
    }
    glm::vec3 baseColor = glm::vec3(material.baseColorFactor);
    if (auto status = readOptional(object, "baseColor", context, readHexColor, baseColor); !status) {
        return propagate(status.error());
    }
    if (const auto it = object.find("baseColorLinear"); it != object.end()) {
        const std::string where = detail::pointerChild(context, "baseColorLinear");
        auto linear = core::jsonToVec3(*it, where);
        if (!linear) {
            return propagate(linear.error());
        }
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite((*linear)[i]) || (*linear)[i] < 0.0f || (*linear)[i] > 1.0f) {
                return makeError(ErrorCode::OutOfRange, "linear color components must be in [0, 1]", where);
            }
        }
        baseColor = *linear;
    }
    float alpha = material.baseColorFactor.a;
    if (auto status = readOptional(object, "alpha", context, unit, alpha); !status) {
        return propagate(status.error());
    }
    material.baseColorFactor = glm::vec4(baseColor, alpha);

    if (auto status = readOptional(object, "metallic", context, unit, material.metallicFactor); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "roughness", context, unit, material.roughnessFactor); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "emissive", context, readHexColor, material.emissiveFactor); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "emissiveStrength", context, nonNegative, material.emissiveStrength);
        !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "normalScale", context, finite, material.normalScale); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "occlusionStrength", context, unit, material.occlusionStrength); !status) {
        return propagate(status.error());
    }

    std::string alphaMode = "opaque";
    const auto alphaModeReader = [](const nlohmann::json& value, const std::string& where) {
        return readEnum(value, {"opaque", "mask", "blend"}, where);
    };
    if (auto status = readOptional(object, "alphaMode", context, alphaModeReader, alphaMode); !status) {
        return propagate(status.error());
    }
    material.alphaMode = alphaMode == "mask"    ? AlphaMode::Mask
                         : alphaMode == "blend" ? AlphaMode::Blend
                                                : AlphaMode::Opaque;
    if (auto status = readOptional(object, "alphaCutoff", context, unit, material.alphaCutoff); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "doubleSided", context, detail::readBool, material.doubleSided); !status) {
        return propagate(status.error());
    }

    const std::pair<std::string_view, TextureRef*> textures[] = {
        {"baseColorTexture", &material.baseColorTexture},
        {"metallicRoughnessTexture", &material.metallicRoughnessTexture},
        {"normalTexture", &material.normalTexture},
        {"occlusionTexture", &material.occlusionTexture},
        {"emissiveTexture", &material.emissiveTexture},
    };
    for (const auto& [key, texture] : textures) {
        if (auto status = readTexture(object, key, context, *texture); !status) {
            return propagate(status.error());
        }
    }
    return material;
}

} // namespace

Expected<MaterialDesc> parseMaterialDesc(const nlohmann::json& object, std::string_view name,
                                         std::string_view context) {
    try {
        return parseMaterial(object, name, context);
    } catch (const std::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("unexpected JSON failure: ") + e.what(),
                         std::string(context));
    }
}

MaterialLibrary::MaterialLibrary() : m_missing(std::make_shared<MissingNames>()) {}

Expected<MaterialLibrary> MaterialLibrary::loadFromFile(const std::filesystem::path& path) {
    auto document = core::readJsonFile(path);
    if (!document) {
        return propagate(document.error());
    }
    return fromJson(*document, core::pathToUtf8(path));
}

Expected<MaterialLibrary> MaterialLibrary::fromJson(const nlohmann::json& document, std::string_view context) {
    const std::string root = std::string(context) + ":";
    if (auto status = detail::requireObject(document, root + "/"); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(document, {"schemaVersion", "materials"}, root); !status) {
        return propagate(status.error());
    }
    const std::string versionContext = detail::pointerChild(root, "schemaVersion");
    if (!document.contains("schemaVersion")) {
        return makeError(ErrorCode::ParseError, "missing \"schemaVersion\"", versionContext);
    }
    auto version = detail::readInteger(document["schemaVersion"], versionContext);
    if (!version) {
        return propagate(version.error());
    }
    if (*version != 1) {
        return makeError(ErrorCode::Unsupported, "unsupported schemaVersion " + std::to_string(*version),
                         versionContext);
    }
    const std::string materialsContext = detail::pointerChild(root, "materials");
    if (!document.contains("materials")) {
        return makeError(ErrorCode::ParseError, "missing \"materials\"", materialsContext);
    }
    const nlohmann::json& materials = document["materials"];
    if (auto status = detail::requireObject(materials, materialsContext); !status) {
        return propagate(status.error());
    }

    MaterialLibrary library;
    for (const auto& item : materials.items()) {
        const std::string where = detail::pointerChild(materialsContext, item.key());
        if (item.key().empty()) {
            return makeError(ErrorCode::ParseError, "material names must not be empty", where);
        }
        auto material = parseMaterialDesc(item.value(), item.key(), where);
        if (!material) {
            return propagate(material.error());
        }
        library.m_materials.emplace(item.key(), std::move(*material));
    }
    return library;
}

const MaterialDesc* MaterialLibrary::find(std::string_view name) const {
    const auto it = m_materials.find(name);
    return it == m_materials.end() ? nullptr : &it->second;
}

const MaterialDesc& MaterialLibrary::fallback() const {
    const MaterialDesc* own = find(kFallbackMaterialName);
    return own != nullptr ? *own : builtinFallback();
}

const MaterialDesc& MaterialLibrary::getOrFallback(std::string_view name) const {
    if (const MaterialDesc* material = find(name)) {
        return *material;
    }
    bool firstTime = true;
    if (m_missing) {
        const std::lock_guard lock(m_missing->mutex);
        firstTime = m_missing->names.emplace(name).second;
    }
    const MaterialDesc& substitute = fallback();
    if (firstTime) {
        spdlog::warn("Material '{}' not found; using {}", name, substitute.name);
    }
    return substitute;
}

std::vector<std::string> MaterialLibrary::names() const {
    std::vector<std::string> result;
    result.reserve(m_materials.size());
    for (const auto& entry : m_materials) {
        result.push_back(entry.first);
    }
    return result;
}

} // namespace rv::assets
