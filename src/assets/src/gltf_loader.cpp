#include <rv/assets/gltf_loader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>

#include "gltf_geometry.hpp"
#include "path_util.hpp"

namespace rv::assets {
namespace {

namespace fg = fastgltf;

constexpr std::uint32_t kGlbMagic = 0x46546C67;     // "glTF"
constexpr std::uint32_t kGlbJsonChunk = 0x4E4F534A; // "JSON"
constexpr std::size_t kGlbHeaderSize = 12;
constexpr std::size_t kGlbChunkHeaderSize = 8;
constexpr float kMaxPlausibleExtent = 20.0f;  // meters
constexpr float kMinPlausibleExtent = 0.005f; // meters
constexpr std::string_view kSupportedRequiredExtensions[] = {"KHR_texture_transform",
                                                             "KHR_materials_emissive_strength"};

std::string toStdString(std::string_view text) {
    return std::string(text);
}

bool isSupportedRequiredExtension(std::string_view name) noexcept {
    return std::find(std::begin(kSupportedRequiredExtensions), std::end(kSupportedRequiredExtensions), name) !=
           std::end(kSupportedRequiredExtensions);
}

std::uint32_t readLittleEndian32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool isGlb(std::span<const std::byte> bytes) noexcept {
    return bytes.size() >= 4 && readLittleEndian32(bytes, 0) == kGlbMagic;
}

/// Precise diagnostics for the GLB container (fastgltf only reports "invalid GLB").
Status checkGlbContainer(std::span<const std::byte> bytes, const std::string& context) {
    if (bytes.size() < kGlbHeaderSize + kGlbChunkHeaderSize) {
        return makeError(ErrorCode::ParseError, "truncated GLB: the file ends inside the header", context);
    }
    const std::uint32_t version = readLittleEndian32(bytes, 4);
    if (version != 2) {
        return makeError(ErrorCode::Unsupported,
                         "GLB container version " + std::to_string(version) + " is not supported (expected 2)",
                         context);
    }
    const std::size_t declaredLength = readLittleEndian32(bytes, 8);
    if (declaredLength > bytes.size()) {
        return makeError(ErrorCode::ParseError,
                         "truncated GLB: the header declares " + std::to_string(declaredLength) + " bytes but only " +
                             std::to_string(bytes.size()) + " are available",
                         context);
    }
    const std::size_t jsonLength = readLittleEndian32(bytes, 12);
    if (readLittleEndian32(bytes, 16) != kGlbJsonChunk) {
        return makeError(ErrorCode::ParseError, "invalid GLB: the first chunk is not JSON", context);
    }
    const std::size_t jsonEnd = kGlbHeaderSize + kGlbChunkHeaderSize + jsonLength;
    if (jsonEnd > declaredLength) {
        return makeError(ErrorCode::ParseError, "truncated GLB: the JSON chunk extends past the end of the file",
                         context);
    }
    if (jsonEnd + kGlbChunkHeaderSize <= declaredLength) {
        const std::size_t binLength = readLittleEndian32(bytes, jsonEnd);
        if (binLength > declaredLength - jsonEnd - kGlbChunkHeaderSize) {
            return makeError(ErrorCode::ParseError, "truncated GLB: the binary chunk extends past the end of the file",
                             context);
        }
    }
    return ok();
}

/// JSON text of a .gltf file or of the first chunk of a (checked) GLB.
std::string_view jsonText(std::span<const std::byte> bytes) {
    if (isGlb(bytes)) {
        const std::size_t length = readLittleEndian32(bytes, 12);
        return {reinterpret_cast<const char*>(bytes.data()) + kGlbHeaderSize + kGlbChunkHeaderSize, length};
    }
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// fastgltf only reports that some required extension is missing; read "extensionsRequired" to name it.
std::optional<std::string> findUnsupportedRequiredExtension(std::string_view json) {
    const nlohmann::json document = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::nullopt;
    }
    const auto it = document.find("extensionsRequired");
    if (it == document.end() || !it->is_array()) {
        return std::nullopt;
    }
    for (const nlohmann::json& extension : *it) {
        if (extension.is_string() && !isSupportedRequiredExtension(extension.get<std::string>())) {
            return extension.get<std::string>();
        }
    }
    return std::nullopt;
}

struct SocketMatch {
    geometry::SocketKind kind = geometry::SocketKind::Other;
    std::string id;
};

/// Node naming conventions of style §9.2.
std::optional<SocketMatch> classifySocket(std::string_view name) {
    constexpr std::string_view kAnchor = "Anchor_";
    constexpr std::string_view kScreen = "ScreenSurface";
    if (name.starts_with(kAnchor) && name.size() > kAnchor.size()) {
        return SocketMatch{geometry::SocketKind::Anchor, std::string(name.substr(kAnchor.size()))};
    }
    if (name == kScreen) {
        return SocketMatch{geometry::SocketKind::ScreenSurface, std::string(name)};
    }
    if (name.starts_with(kScreen) && name.size() > kScreen.size() + 1 && name[kScreen.size()] == '_') {
        const std::string_view suffix = name.substr(kScreen.size() + 1);
        if (std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return SocketMatch{geometry::SocketKind::ScreenSurface, std::string(name)};
        }
    }
    if (name.starts_with("ColliderProxy")) {
        return SocketMatch{geometry::SocketKind::ColliderProxy, std::string(name)};
    }
    if (name == "PowerButton" || name.starts_with("Speaker")) {
        return SocketMatch{geometry::SocketKind::Other, std::string(name)};
    }
    return std::nullopt;
}

std::string formatVec3(const glm::vec3& v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%.4g x %.4g x %.4g", static_cast<double>(v.x), static_cast<double>(v.y),
                  static_cast<double>(v.z));
    return buffer;
}

std::optional<detail::UvTransform> transformOf(const fg::TextureInfo* info) {
    if (info == nullptr) {
        return std::nullopt;
    }
    detail::UvTransform transform;
    if (info->transform) {
        transform.offset = glm::vec2(info->transform->uvOffset[0], info->transform->uvOffset[1]);
        transform.rotation = info->transform->rotation;
        transform.scale = glm::vec2(info->transform->uvScale[0], info->transform->uvScale[1]);
    }
    return transform;
}

class Importer {
public:
    Importer(std::span<const std::byte> bytes, std::filesystem::path baseDirectory, const GltfLoadOptions& options,
             std::string context)
        : m_bytes(bytes), m_baseDirectory(std::move(baseDirectory)), m_options(options), m_context(std::move(context)),
          m_budget{static_cast<std::size_t>(options.maxFileBytes / 16),
                   static_cast<std::size_t>(options.maxFileBytes / 4)} {}

    Expected<ModelData> run() {
        if (auto status = parse(); !status) {
            return propagate(status.error());
        }
        if (auto status = resolveBuffers(); !status) {
            return propagate(status.error());
        }
        decodeImages();
        convertMaterials();
        if (auto status = convertMeshes(); !status) {
            return propagate(status.error());
        }
        if (auto status = buildNodes(); !status) {
            return propagate(status.error());
        }
        if (auto status = buildScene(); !status) {
            return propagate(status.error());
        }
        computeBoundsAndStats();
        return std::move(m_model);
    }

private:
    struct MeshRange {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    [[nodiscard]] tl::unexpected<Error> fail(ErrorCode code, std::string message) const {
        return makeError(code, std::move(message), m_context);
    }

    void warn(std::string message) { m_model.warnings.push_back(std::move(message)); }

    Status parse() {
        if (m_bytes.size() > m_options.maxFileBytes) {
            return fail(ErrorCode::OutOfRange,
                        "glTF file is larger than the allowed " + std::to_string(m_options.maxFileBytes) + " bytes");
        }
        if (isGlb(m_bytes)) {
            if (auto status = checkGlbContainer(m_bytes, m_context); !status) {
                return status;
            }
        } else {
            const std::string_view text = core::trim(jsonText(m_bytes));
            if (text.empty() || text.front() != '{') {
                return fail(ErrorCode::ParseError, "not a glTF file (neither GLB nor a JSON object)");
            }
        }

        auto data = fg::GltfDataBuffer::FromBytes(m_bytes.data(), m_bytes.size());
        if (data.error() != fg::Error::None) {
            return fail(ErrorCode::OutOfRange,
                        "cannot buffer the glTF data (" + toStdString(fg::getErrorMessage(data.error())) + ")");
        }
        fg::Parser parser(fg::Extensions::KHR_texture_transform | fg::Extensions::KHR_materials_emissive_strength |
                          fg::Extensions::KHR_materials_unlit);
        auto asset = parser.loadGltf(data.get(), m_baseDirectory, fg::Options::None, fg::Category::OnlyRenderable);
        const fg::Error error = asset.error();
        if (error == fg::Error::MissingExtensions || error == fg::Error::UnknownRequiredExtension) {
            const auto name = findUnsupportedRequiredExtension(jsonText(m_bytes));
            return fail(ErrorCode::Unsupported, name ? "glTF requires the unsupported extension \"" + *name + "\""
                                                     : "glTF requires an unsupported extension");
        }
        if (error != fg::Error::None) {
            return fail(ErrorCode::ParseError, "invalid glTF (" + toStdString(fg::getErrorName(error)) + ": " +
                                                   toStdString(fg::getErrorMessage(error)) + ")");
        }
        m_asset = std::move(asset.get());
        for (const auto& extension : m_asset.extensionsRequired) {
            if (!isSupportedRequiredExtension(extension)) {
                return fail(ErrorCode::Unsupported,
                            "glTF requires the unsupported extension \"" + toStdString(extension) + "\"");
            }
        }
        return ok();
    }

    /// Reads a file referenced by a relative URI next to the glTF (never outside `baseDirectory`), recording it in
    /// ModelData::externalFiles, also when it cannot be read.
    Expected<std::vector<std::byte>> readExternal(const fg::URI& uri, const std::string& what) {
        if (!uri.scheme().empty()) {
            return fail(ErrorCode::Unsupported,
                        what + ": only relative file references are supported (\"" + toStdString(uri.string()) + "\")");
        }
        if (m_baseDirectory.empty()) {
            return fail(ErrorCode::NotFound, what + ": external file \"" + toStdString(uri.path()) +
                                                 "\" cannot be resolved without a base directory");
        }
        auto relative = detail::normalizeRelativePath(uri.path(), true, m_context);
        if (!relative) {
            return fail(ErrorCode::InvalidArgument, what + ": " + relative.error().message);
        }
        const std::filesystem::path file = m_baseDirectory / core::pathFromUtf8(*relative);
        std::vector<std::filesystem::path>& files = m_model.externalFiles;
        if (std::find(files.begin(), files.end(), file) == files.end()) {
            files.push_back(file);
        }
        auto bytes = core::readBinaryFile(file, m_options.maxFileBytes);
        if (!bytes) {
            return fail(bytes.error().code, what + " (\"" + *relative + "\"): " + bytes.error().message);
        }
        return bytes;
    }

    Status resolveBuffers() {
        m_buffers.resize(m_asset.buffers.size());
        for (std::size_t i = 0; i < m_asset.buffers.size(); ++i) {
            const fg::Buffer& buffer = m_asset.buffers[i];
            const std::string what = "buffer " + std::to_string(i);
            std::span<const std::byte> bytes;
            if (const auto* array = std::get_if<fg::sources::Array>(&buffer.data)) {
                bytes = std::span<const std::byte>(array->bytes.data(), array->bytes.size());
            } else if (const auto* vector = std::get_if<fg::sources::Vector>(&buffer.data)) {
                bytes = vector->bytes;
            } else if (const auto* view = std::get_if<fg::sources::ByteView>(&buffer.data)) {
                bytes = std::span<const std::byte>(view->bytes.data(), view->bytes.size());
            } else if (const auto* uri = std::get_if<fg::sources::URI>(&buffer.data)) {
                auto file = readExternal(uri->uri, what);
                if (!file) {
                    return propagate(file.error());
                }
                if (uri->fileByteOffset > file->size()) {
                    return fail(ErrorCode::ParseError, what + ": byte offset is past the end of the file");
                }
                m_externalData.push_back(std::move(*file));
                bytes = std::span<const std::byte>(m_externalData.back()).subspan(uri->fileByteOffset);
            } else {
                return fail(ErrorCode::ParseError, what + " has no data");
            }
            if (bytes.size() < buffer.byteLength) {
                return fail(ErrorCode::ParseError, "truncated data: " + what + " declares " +
                                                       std::to_string(buffer.byteLength) + " bytes but holds " +
                                                       std::to_string(bytes.size()));
            }
            m_buffers[i] = bytes.first(buffer.byteLength);
        }
        return ok();
    }

    Expected<ImageData> decodeOneImage(const fg::Image& image, const std::string& what) {
        const std::string context = m_context + " (" + what + ")";
        if (const auto* view = std::get_if<fg::sources::BufferView>(&image.data)) {
            auto bytes = detail::bufferViewBytes(m_asset, m_buffers, view->bufferViewIndex, what);
            if (!bytes) {
                return propagate(bytes.error());
            }
            return decodeImage(*bytes, 0, context);
        }
        if (const auto* array = std::get_if<fg::sources::Array>(&image.data)) {
            return decodeImage(std::span<const std::byte>(array->bytes.data(), array->bytes.size()), 0, context);
        }
        if (const auto* vector = std::get_if<fg::sources::Vector>(&image.data)) {
            return decodeImage(vector->bytes, 0, context);
        }
        if (const auto* byteView = std::get_if<fg::sources::ByteView>(&image.data)) {
            return decodeImage(std::span<const std::byte>(byteView->bytes.data(), byteView->bytes.size()), 0, context);
        }
        if (const auto* uri = std::get_if<fg::sources::URI>(&image.data)) {
            auto file = readExternal(uri->uri, what);
            if (!file) {
                return propagate(file.error());
            }
            return decodeImage(*file, 0, context);
        }
        return fail(ErrorCode::ParseError, what + " has no data");
    }

    void decodeImages() {
        m_imageMap.assign(m_asset.images.size(), std::nullopt);
        if (!m_options.loadImages) {
            return;
        }
        for (std::size_t i = 0; i < m_asset.images.size(); ++i) {
            const fg::Image& image = m_asset.images[i];
            const std::string what =
                "image " + std::to_string(i) + (image.name.empty() ? "" : " \"" + toStdString(image.name) + "\"");
            // A failure, even an unexpected one, only costs this image (and the textures using it).
            auto decoded = [&]() -> Expected<ImageData> {
                try {
                    return decodeOneImage(image, what);
                } catch (const std::exception& e) {
                    return fail(ErrorCode::Unknown, std::string("unexpected failure: ") + e.what());
                }
            }();
            if (!decoded) {
                warn(what + " could not be loaded and was dropped: " + decoded.error().message);
                continue;
            }
            m_imageMap[i] = static_cast<std::uint32_t>(m_model.images.size());
            m_model.images.push_back(std::move(*decoded));
        }
    }

    TextureRef resolveTexture(const fg::TextureInfo* info, const std::string& material, std::string_view slot) {
        TextureRef reference;
        if (info == nullptr || !m_options.loadImages) {
            return reference;
        }
        const std::string what = "material \"" + material + "\" " + std::string(slot);
        if (info->textureIndex >= m_asset.textures.size()) {
            warn(what + " references a missing texture; texture dropped");
            return reference;
        }
        const fg::Texture& texture = m_asset.textures[info->textureIndex];
        if (!texture.imageIndex.has_value() || texture.imageIndex.value() >= m_asset.images.size()) {
            warn(what + " uses a texture without a usable image; texture dropped");
            return reference;
        }
        const auto image = m_imageMap[texture.imageIndex.value()];
        if (!image) {
            return reference; // the decoding failure was already reported
        }
        reference.imageIndex = *image;
        reference.texCoord = static_cast<std::uint32_t>(info->texCoordIndex);
        const bool overridesTexCoord = info->transform && info->transform->texCoordIndex.has_value() &&
                                       info->transform->texCoordIndex.value() != 0;
        if (reference.texCoord != 0 || overridesTexCoord) {
            warn(what + " uses a texture coordinate set other than TEXCOORD_0, which is not loaded");
        }
        return reference;
    }

    static glm::vec3 finiteOr(const glm::vec3& value, const glm::vec3& fallback) {
        return core::isFinite(value) ? value : fallback;
    }

    static float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

    void convertMaterials() {
        m_model.materials.reserve(m_asset.materials.size());
        m_uvTransforms.assign(m_asset.materials.size(), std::nullopt);
        for (std::size_t i = 0; i < m_asset.materials.size(); ++i) {
            const fg::Material& source = m_asset.materials[i];
            MaterialDesc material;
            material.name = source.name.empty() ? "material_" + std::to_string(i) : toStdString(source.name);
            material.shading = source.unlit ? ShadingModel::Unlit : ShadingModel::Pbr;

            const auto& pbr = source.pbrData;
            const glm::vec4 baseColor(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2],
                                      pbr.baseColorFactor[3]);
            const glm::vec3 emissive(source.emissiveFactor[0], source.emissiveFactor[1], source.emissiveFactor[2]);
            const bool finite = core::isFinite(baseColor) && core::isFinite(emissive) &&
                                std::isfinite(pbr.metallicFactor) && std::isfinite(pbr.roughnessFactor) &&
                                std::isfinite(source.emissiveStrength) && std::isfinite(source.alphaCutoff);
            if (!finite) {
                warn("material \"" + material.name + "\" has non-finite factors; defaults were used for them");
            }
            material.baseColorFactor = core::isFinite(baseColor) ? baseColor : glm::vec4(1.0f);
            material.metallicFactor = finiteOr(pbr.metallicFactor, 1.0f);
            material.roughnessFactor = finiteOr(pbr.roughnessFactor, 1.0f);
            material.emissiveFactor = finiteOr(emissive, glm::vec3(0.0f));
            material.emissiveStrength = finiteOr(source.emissiveStrength, 1.0f);
            material.alphaCutoff = finiteOr(source.alphaCutoff, 0.5f);
            material.doubleSided = source.doubleSided;
            switch (source.alphaMode) {
            case fg::AlphaMode::Opaque:
                material.alphaMode = AlphaMode::Opaque;
                break;
            case fg::AlphaMode::Mask:
                material.alphaMode = AlphaMode::Mask;
                break;
            case fg::AlphaMode::Blend:
                material.alphaMode = AlphaMode::Blend;
                break;
            }

            const fg::TextureInfo* baseColorInfo =
                pbr.baseColorTexture.has_value() ? &pbr.baseColorTexture.value() : nullptr;
            const fg::TextureInfo* metallicRoughnessInfo =
                pbr.metallicRoughnessTexture.has_value() ? &pbr.metallicRoughnessTexture.value() : nullptr;
            const fg::TextureInfo* normalInfo =
                source.normalTexture.has_value() ? &source.normalTexture.value() : nullptr;
            const fg::TextureInfo* occlusionInfo =
                source.occlusionTexture.has_value() ? &source.occlusionTexture.value() : nullptr;
            const fg::TextureInfo* emissiveInfo =
                source.emissiveTexture.has_value() ? &source.emissiveTexture.value() : nullptr;
            if (source.normalTexture.has_value()) {
                material.normalScale = finiteOr(source.normalTexture.value().scale, 1.0f);
            }
            if (source.occlusionTexture.has_value()) {
                material.occlusionStrength = finiteOr(source.occlusionTexture.value().strength, 1.0f);
            }
            material.baseColorTexture = resolveTexture(baseColorInfo, material.name, "baseColorTexture");
            material.metallicRoughnessTexture =
                resolveTexture(metallicRoughnessInfo, material.name, "metallicRoughnessTexture");
            material.normalTexture = resolveTexture(normalInfo, material.name, "normalTexture");
            material.occlusionTexture = resolveTexture(occlusionInfo, material.name, "occlusionTexture");
            material.emissiveTexture = resolveTexture(emissiveInfo, material.name, "emissiveTexture");

            selectUvTransform(i, material.name,
                              {baseColorInfo, metallicRoughnessInfo, normalInfo, occlusionInfo, emissiveInfo});
            m_model.materials.push_back(std::move(material));
        }
    }

    /// Vertices carry a single UV set, so one KHR_texture_transform per material is baked into TEXCOORD_0. When
    /// texture slots disagree, the base color transform (or the first textured slot's) wins.
    void selectUvTransform(std::size_t materialIndex, const std::string& name,
                           std::initializer_list<const fg::TextureInfo*> slots) {
        std::optional<detail::UvTransform> chosen;
        bool conflicting = false;
        for (const fg::TextureInfo* info : slots) {
            const auto transform = transformOf(info);
            if (!transform) {
                continue;
            }
            if (!transform->isIdentity() &&
                !std::isfinite(transform->rotation + transform->offset.x + transform->offset.y + transform->scale.x +
                               transform->scale.y)) {
                warn("material \"" + name + "\" has a non-finite KHR_texture_transform; ignored");
                return;
            }
            if (!chosen) {
                chosen = transform;
            } else if (!(*chosen == *transform)) {
                conflicting = true;
            }
        }
        if (conflicting) {
            warn("material \"" + name +
                 "\" uses different KHR_texture_transform values per texture; only the first (base color) one is "
                 "applied");
        }
        if (chosen && !chosen->isIdentity()) {
            m_uvTransforms[materialIndex] = chosen;
        }
    }

    Status convertMeshes() {
        m_meshRanges.resize(m_asset.meshes.size());
        for (std::size_t m = 0; m < m_asset.meshes.size(); ++m) {
            const fg::Mesh& mesh = m_asset.meshes[m];
            const std::string meshName = mesh.name.empty() ? "mesh_" + std::to_string(m) : toStdString(mesh.name);
            m_meshRanges[m].first = static_cast<std::uint32_t>(m_model.meshes.size());
            for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
                const fg::Primitive& primitive = mesh.primitives[p];
                const std::string label =
                    "mesh \"" + meshName + "\"" + (mesh.primitives.size() > 1 ? " primitive " + std::to_string(p) : "");
                std::optional<std::uint32_t> material;
                if (primitive.materialIndex.has_value()) {
                    if (primitive.materialIndex.value() < m_asset.materials.size()) {
                        material = static_cast<std::uint32_t>(primitive.materialIndex.value());
                    } else {
                        warn(label + " references a missing material; the default material is used");
                    }
                }
                detail::PrimitiveConversion conversion;
                conversion.generateTangents = m_options.generateTangents;
                if (material && m_uvTransforms[*material]) {
                    conversion.uvTransform = &*m_uvTransforms[*material];
                }
                auto converted = detail::convertPrimitive(m_asset, m_buffers, primitive, conversion, m_budget, label,
                                                          m_model.warnings);
                if (!converted) {
                    return fail(converted.error().code, converted.error().message);
                }
                if (!*converted) {
                    continue;
                }
                (*converted)->name = mesh.primitives.size() > 1 ? meshName + "/" + std::to_string(p) : meshName;
                m_model.meshes.push_back(std::move(**converted));
                m_primitiveMaterials.push_back(material);
            }
            m_meshRanges[m].count = static_cast<std::uint32_t>(m_model.meshes.size()) - m_meshRanges[m].first;
        }
        return ok();
    }

    glm::mat4 localTransform(const fg::Node& node, const std::string& name) {
        if (const auto* trs = std::get_if<fg::TRS>(&node.transform)) {
            const glm::vec3 translation(trs->translation[0], trs->translation[1], trs->translation[2]);
            glm::quat rotation(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]);
            const glm::vec3 scale(trs->scale[0], trs->scale[1], trs->scale[2]);
            const float length = glm::length(rotation);
            if (std::isfinite(length) && length > 1e-8f) {
                rotation /= length;
            } else {
                warn("node \"" + name + "\" has an invalid rotation quaternion; identity used");
                rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) *
                   glm::scale(glm::mat4(1.0f), scale);
        }
        const auto& matrix = std::get<fg::math::fmat4x4>(node.transform);
        return glm::make_mat4(matrix.data());
    }

    Status buildNodes() {
        const std::size_t count = m_asset.nodes.size();
        m_model.nodes.resize(count);
        for (std::size_t n = 0; n < count; ++n) {
            for (const std::size_t child : m_asset.nodes[n].children) {
                if (child >= count || child == n) {
                    return fail(ErrorCode::ParseError,
                                "node " + std::to_string(n) + " has an invalid child " + std::to_string(child));
                }
                if (m_model.nodes[child].parent != -1) {
                    return fail(ErrorCode::ParseError, "node " + std::to_string(child) + " has more than one parent");
                }
                m_model.nodes[child].parent = static_cast<int>(n);
            }
        }

        for (std::size_t n = 0; n < count; ++n) {
            const fg::Node& source = m_asset.nodes[n];
            ModelNode& node = m_model.nodes[n];
            node.name = source.name.empty() ? "node_" + std::to_string(n) : toStdString(source.name);
            node.local = localTransform(source, node.name);
            if (source.meshIndex.has_value()) {
                const std::size_t meshIndex = source.meshIndex.value();
                if (meshIndex >= m_meshRanges.size()) {
                    return fail(ErrorCode::ParseError, "node \"" + node.name + "\" references mesh " +
                                                           std::to_string(meshIndex) + ", which does not exist");
                }
                if (m_meshRanges[meshIndex].count > 0) {
                    node.mesh = m_meshRanges[meshIndex].first;
                    node.meshCount = m_meshRanges[meshIndex].count;
                }
            }
        }

        // Parents before children: depth-first from every root. Every node has at most one parent, so the walk
        // never enters a cycle, and the nodes it does not reach are exactly those on or below a cycle.
        std::vector<char> reached(count, 0);
        std::vector<std::size_t> stack;
        for (std::size_t root = 0; root < count; ++root) {
            if (m_model.nodes[root].parent != -1) {
                continue;
            }
            stack.push_back(root);
            while (!stack.empty()) {
                const std::size_t n = stack.back();
                stack.pop_back();
                reached[n] = 1;
                ModelNode& node = m_model.nodes[n];
                node.world = node.parent == -1
                                 ? node.local
                                 : m_model.nodes[static_cast<std::size_t>(node.parent)].world * node.local;
                if (!core::isFinite(node.world)) {
                    return fail(ErrorCode::ParseError, "node \"" + node.name + "\" has a non-finite transform");
                }
                for (const std::size_t child : m_asset.nodes[n].children) {
                    stack.push_back(child);
                }
            }
        }
        if (const auto unreached = std::find(reached.begin(), reached.end(), 0); unreached != reached.end()) {
            return fail(ErrorCode::ParseError, "the node hierarchy contains a cycle (node " +
                                                   std::to_string(unreached - reached.begin()) +
                                                   " is not below any root node)");
        }
        return ok();
    }

    Expected<std::vector<std::size_t>> sceneRoots() const {
        std::vector<std::size_t> roots;
        if (m_asset.scenes.empty()) {
            for (std::size_t n = 0; n < m_model.nodes.size(); ++n) {
                if (m_model.nodes[n].parent == -1) {
                    roots.push_back(n);
                }
            }
            return roots;
        }
        const std::size_t sceneIndex = m_asset.defaultScene.has_value() ? m_asset.defaultScene.value() : 0;
        if (sceneIndex >= m_asset.scenes.size()) {
            return fail(ErrorCode::ParseError, "default scene " + std::to_string(sceneIndex) + " does not exist");
        }
        std::vector<char> listed(m_model.nodes.size(), 0);
        for (const std::size_t n : m_asset.scenes[sceneIndex].nodeIndices) {
            if (n >= m_model.nodes.size()) {
                return fail(ErrorCode::ParseError,
                            "scene references node " + std::to_string(n) + ", which does not exist");
            }
            if (m_model.nodes[n].parent != -1) {
                return fail(ErrorCode::ParseError,
                            "scene root node " + std::to_string(n) + " is also the child of another node");
            }
            if (listed[n] == 0) {
                listed[n] = 1;
                roots.push_back(n);
            }
        }
        return roots;
    }

    void checkNodeScale(const ModelNode& node) {
        const glm::mat3 basis(node.world);
        const glm::vec3 scale(glm::length(basis[0]), glm::length(basis[1]), glm::length(basis[2]));
        const float largest = std::max({scale.x, scale.y, scale.z});
        const float smallest = std::min({scale.x, scale.y, scale.z});
        if (smallest <= 1e-8f) {
            warn("node \"" + node.name + "\" has a zero scale; its mesh is degenerate");
            return;
        }
        if (largest - smallest > 1e-3f * largest) {
            warn("node \"" + node.name + "\" has a non-uniform scale (" + formatVec3(scale) +
                 "); apply the scale to the mesh before export (normals assume uniform scale)");
        }
        if (glm::determinant(basis) < 0.0f) {
            warn("node \"" + node.name + "\" has a mirroring (negative) scale; faces would render inside out");
        }
    }

    Status buildScene() {
        auto roots = sceneRoots();
        if (!roots) {
            return propagate(roots.error());
        }
        std::vector<std::size_t> stack(roots->rbegin(), roots->rend());
        while (!stack.empty()) {
            const std::size_t n = stack.back();
            stack.pop_back();
            const auto& children = m_asset.nodes[n].children;
            for (auto child = children.rbegin(); child != children.rend(); ++child) {
                stack.push_back(*child);
            }
            visitSceneNode(n);
        }
        return ok();
    }

    void visitSceneNode(std::size_t n) {
        const ModelNode& node = m_model.nodes[n];
        if (node.name == "Anchor_") {
            warn("node \"Anchor_\" has no anchor id; it is not a socket");
        }
        const std::optional<SocketMatch> match = classifySocket(node.name);
        const bool collider = match && match->kind == geometry::SocketKind::ColliderProxy;
        if (node.mesh && !(collider && m_options.excludeColliderProxies)) {
            checkNodeScale(node); // only drawn geometry: collision shapes may be stretched freely
            for (std::uint32_t k = 0; k < node.meshCount; ++k) {
                ModelDrawItem item;
                item.meshIndex = *node.mesh + k;
                item.material = m_primitiveMaterials[item.meshIndex];
                item.node = static_cast<std::uint32_t>(n);
                item.world = node.world;
                m_model.drawItems.push_back(item);
            }
        }
        if (!match) {
            return;
        }
        geometry::Socket socket;
        socket.id = match->id;
        socket.nodeName = node.name;
        socket.kind = match->kind;
        socket.transform = node.world;
        if (node.mesh) {
            socket.meshIndex = *node.mesh;
            for (std::uint32_t k = 0; k < node.meshCount; ++k) {
                socket.bounds.expand(m_model.meshes[*node.mesh + k].bounds);
            }
        } else if (match->kind == geometry::SocketKind::ScreenSurface) {
            warn("ScreenSurface node \"" + node.name + "\" has no mesh; video and images cannot be shown on it");
        } else if (match->kind == geometry::SocketKind::ColliderProxy) {
            warn("ColliderProxy node \"" + node.name + "\" has no mesh; it defines no collision shape");
        }
        if (!m_socketIds.insert(socket.id).second) {
            warn("duplicate socket id \"" + socket.id + "\" (node \"" + node.name + "\"); lookups return the first");
        }
        m_model.sockets.push_back(std::move(socket));
    }

    void computeBoundsAndStats() {
        for (const ModelDrawItem& item : m_model.drawItems) {
            for (const geometry::Vertex& vertex : m_model.meshes[item.meshIndex].vertices) {
                m_model.bounds.expand(glm::vec3(item.world * glm::vec4(vertex.position, 1.0f)));
            }
        }
        if (!m_model.bounds.isEmpty()) {
            const glm::vec3 size = m_model.bounds.size();
            const float largest = std::max({size.x, size.y, size.z});
            if (largest > kMaxPlausibleExtent) {
                warn("model bounds are " + formatVec3(size) +
                     " m, larger than 20 m: probably exported in centimeters or millimeters");
            } else if (largest < kMinPlausibleExtent) {
                warn("model bounds are " + formatVec3(size) +
                     " m, smaller than 5 mm: probably exported with a wrong unit scale");
            }
        }

        ModelStats& stats = m_model.stats;
        stats.meshes = m_asset.meshes.size();
        stats.primitives = m_model.meshes.size();
        for (const geometry::MeshData& mesh : m_model.meshes) {
            stats.triangles += mesh.triangleCount();
            stats.vertices += mesh.vertices.size();
        }
        stats.materials = m_model.materials.size();
        stats.images = m_model.images.size();
        stats.nodes = m_model.nodes.size();
        for (const auto& extension : m_asset.extensionsUsed) {
            stats.extensionsUsed.push_back(toStdString(extension));
        }
    }

    std::span<const std::byte> m_bytes;
    std::filesystem::path m_baseDirectory;
    GltfLoadOptions m_options;
    std::string m_context;
    detail::GeometryBudget m_budget; // what the remaining primitives may still decode (see GltfLoadOptions)

    fg::Asset m_asset;
    std::vector<std::vector<std::byte>> m_externalData;             // owns external buffer files
    std::vector<std::span<const std::byte>> m_buffers;              // per glTF buffer, byteLength bytes
    std::vector<std::optional<std::uint32_t>> m_imageMap;           // glTF image -> ModelData::images
    std::vector<std::optional<detail::UvTransform>> m_uvTransforms; // per glTF material
    std::vector<MeshRange> m_meshRanges;                            // per glTF mesh -> ModelData::meshes
    std::vector<std::optional<std::uint32_t>> m_primitiveMaterials; // per ModelData::meshes entry
    std::set<std::string, std::less<>> m_socketIds;                 // ids in ModelData::sockets
    ModelData m_model;
};

Expected<ModelData> importModel(std::span<const std::byte> bytes, const std::filesystem::path& baseDirectory,
                                const GltfLoadOptions& options, std::string context) {
    try {
        Importer importer(bytes, baseDirectory, options, context);
        return importer.run();
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while loading the model", context);
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Unknown, std::string("unexpected failure while loading the model: ") + e.what(),
                         context);
    }
}

} // namespace

Expected<ModelData> loadGltf(const std::filesystem::path& path, const GltfLoadOptions& options) {
    auto bytes = core::readBinaryFile(path, options.maxFileBytes);
    if (!bytes) {
        return propagate(bytes.error());
    }
    // A bare file name ("crt.gltf") is relative to the working directory, like its external references.
    const std::filesystem::path baseDirectory =
        path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    return importModel(*bytes, baseDirectory, options, core::pathToUtf8(path));
}

Expected<ModelData> loadGltfFromMemory(std::span<const std::byte> bytes, const std::filesystem::path& baseDirectory,
                                       const GltfLoadOptions& options) {
    return importModel(bytes, baseDirectory, options, "<memory glTF>");
}

} // namespace rv::assets
