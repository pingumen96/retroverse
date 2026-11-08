#include <rv/assets/console_content.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/assets/bc1.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/geometry/socket.hpp>

#include "json_util.hpp"

namespace rv::assets {

namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 3> kSocketRoles{"CartridgeSlot", "DiscTray", "Front"};

/// A file name alone: no folder, no "..", no drive.
bool plainFileName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name.find(':') == std::string_view::npos;
}

/// The raw values' box-filtered chain (linear maps: normals, roughness): each level halves each side (at least 1), a
/// pixel the mean of the 2 x 2 (or 2 x 1) it covers.
std::vector<ImageData> rawMipChain(const ImageData& image) {
    std::vector<ImageData> levels{image};
    while (levels.back().width > 1 || levels.back().height > 1) {
        const ImageData& from = levels.back();
        ImageData to;
        to.width = std::max(1, from.width / 2);
        to.height = std::max(1, from.height / 2);
        to.channels = 4;
        to.pixels.resize(static_cast<std::size_t>(to.width) * static_cast<std::size_t>(to.height) * 4);
        for (int y = 0; y < to.height; ++y) {
            for (int x = 0; x < to.width; ++x) {
                for (std::size_t c = 0; c < 4; ++c) {
                    unsigned sum = 0;
                    unsigned count = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(from.width - 1, x * 2 + dx);
                            const int sy = std::min(from.height - 1, y * 2 + dy);
                            sum += from.pixels[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(from.width) +
                                                static_cast<std::size_t>(sx)) *
                                                   4 +
                                               c];
                            ++count;
                        }
                    }
                    to.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(to.width) +
                               static_cast<std::size_t>(x)) *
                                  4 +
                              c] = static_cast<std::uint8_t>((sum + count / 2) / count);
                }
            }
        }
        levels.push_back(std::move(to));
    }
    return levels;
}

std::size_t chainBytes(int width, int height, bool bc3) {
    std::size_t bytes = 0;
    for (;;) {
        bytes += bc3 ? bc3LevelBytes(width, height) : bc1LevelBytes(width, height);
        if (width <= 1 && height <= 1) {
            return bytes;
        }
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
    }
}

/// A sampled image's blocks: BC3 for a base colour (sRGB) with transparency, else BC1.
bool usesBc3(const ImageData& image, bool srgb) {
    return srgb && image.channels == 4 && !isOpaque(image);
}

ImageData asRgba(const ImageData& image) {
    return image.channels == 4 ? image : convertChannels(image, 4);
}

const geometry::Socket* socketFor(const ModelData& model, const ConsoleManifest& manifest, std::string_view role) {
    if (const auto named = manifest.sockets.find(std::string(role)); named != manifest.sockets.end()) {
        for (const geometry::Socket& socket : model.sockets) {
            if (socket.nodeName == named->second) {
                return &socket;
            }
        }
        // A node without a socket's name: a plain node of the model.
        return nullptr;
    }
    return geometry::findSocket(model.sockets, role);
}

} // namespace

std::string_view toString(ConsoleKind kind) noexcept {
    switch (kind) {
    case ConsoleKind::Home:
        return "home";
    case ConsoleKind::Handheld:
        return "handheld";
    case ConsoleKind::Computer:
        return "computer";
    }
    return "home";
}

std::string_view toString(ConsoleFront front) noexcept {
    switch (front) {
    case ConsoleFront::PlusZ:
        return "+z";
    case ConsoleFront::MinusZ:
        return "-z";
    case ConsoleFront::PlusX:
        return "+x";
    case ConsoleFront::MinusX:
        return "-x";
    }
    return "+z";
}

std::optional<ConsoleKind> consoleKindFromString(std::string_view text) noexcept {
    for (const ConsoleKind kind : {ConsoleKind::Home, ConsoleKind::Handheld, ConsoleKind::Computer}) {
        if (toString(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

std::optional<ConsoleFront> consoleFrontFromString(std::string_view text) noexcept {
    for (const ConsoleFront front : {ConsoleFront::PlusZ, ConsoleFront::MinusZ, ConsoleFront::PlusX, ConsoleFront::MinusX}) {
        if (toString(front) == text) {
            return front;
        }
    }
    return std::nullopt;
}

Expected<ConsoleManifest> parseConsoleManifest(std::string_view text, std::string_view context) {
    auto document = core::parseJson(text, context);
    if (!document) {
        return propagate(document.error());
    }
    const std::string at = std::string(context) + ":";
    try {
        if (Status object = detail::requireObject(*document, at); !object) {
            return propagate(object.error());
        }
        if (Status keys = detail::rejectUnknownKeys(
                *document, {"schemaVersion", "model", "kind", "front", "sockets", "scaleCm", "source"}, at);
            !keys) {
            return propagate(keys.error());
        }
        ConsoleManifest manifest;
        const auto schema = document->find("schemaVersion");
        if (schema == document->end() || !schema->is_number_integer() || schema->get<int>() != kConsoleManifestSchema) {
            return makeError(ErrorCode::ValidationFailed, "expected schemaVersion 1",
                             detail::pointerChild(at, "schemaVersion"));
        }
        if (Status model = detail::readOptional(*document, "model", at, detail::readString, manifest.model); !model) {
            return propagate(model.error());
        }
        if (!plainFileName(manifest.model)) {
            return makeError(ErrorCode::ValidationFailed, "expected a file name in the console's folder",
                             detail::pointerChild(at, "model"));
        }
        std::string kind(toString(manifest.kind));
        if (Status read = detail::readOptional(*document, "kind", at, detail::readString, kind); !read) {
            return propagate(read.error());
        }
        const std::optional<ConsoleKind> parsedKind = consoleKindFromString(kind);
        if (!parsedKind) {
            return makeError(ErrorCode::ValidationFailed, "expected \"home\", \"handheld\" or \"computer\"",
                             detail::pointerChild(at, "kind"));
        }
        manifest.kind = *parsedKind;
        std::string front(toString(manifest.front));
        if (Status read = detail::readOptional(*document, "front", at, detail::readString, front); !read) {
            return propagate(read.error());
        }
        const std::optional<ConsoleFront> parsedFront = consoleFrontFromString(front);
        if (!parsedFront) {
            return makeError(ErrorCode::ValidationFailed, "expected \"+z\", \"-z\", \"+x\" or \"-x\"",
                             detail::pointerChild(at, "front"));
        }
        manifest.front = *parsedFront;
        if (const auto sockets = document->find("sockets"); sockets != document->end()) {
            const std::string where = detail::pointerChild(at, "sockets");
            if (Status object = detail::requireObject(*sockets, where); !object) {
                return propagate(object.error());
            }
            if (Status keys = detail::rejectUnknownKeys(*sockets, {"CartridgeSlot", "DiscTray", "Front"}, where); !keys) {
                return propagate(keys.error());
            }
            for (const auto& [role, node] : sockets->items()) {
                auto name = detail::readString(node, detail::pointerChild(where, role));
                if (!name) {
                    return propagate(name.error());
                }
                manifest.sockets.emplace(role, std::move(*name));
            }
        }
        if (const auto scale = document->find("scaleCm"); scale != document->end()) {
            const std::string where = detail::pointerChild(at, "scaleCm");
            if (!scale->is_array() || scale->size() != 3) {
                return makeError(ErrorCode::ValidationFailed, "expected [width, height, depth] in centimetres", where);
            }
            glm::vec3 cm(0.0F);
            for (int axis = 0; axis < 3; ++axis) {
                auto value = detail::readNumberInRange((*scale)[static_cast<std::size_t>(axis)], 0.1F, 1000.0F,
                                                       detail::pointerChild(where, static_cast<std::size_t>(axis)));
                if (!value) {
                    return propagate(value.error());
                }
                cm[axis] = *value;
            }
            manifest.scaleCm = cm;
        }
        if (const auto source = document->find("source"); source != document->end()) {
            const std::string where = detail::pointerChild(at, "source");
            if (Status object = detail::requireObject(*source, where); !object) {
                return propagate(object.error());
            }
            if (Status keys =
                    detail::rejectUnknownKeys(*source, {"url", "author", "license", "date", "modifications"}, where);
                !keys) {
                return propagate(keys.error());
            }
            ConsoleSourceInfo& info = manifest.source;
            for (auto [key, field] : std::array<std::pair<std::string_view, std::string*>, 5>{
                     {{"url", &info.url},
                      {"author", &info.author},
                      {"license", &info.license},
                      {"date", &info.date},
                      {"modifications", &info.modifications}}}) {
                if (Status read = detail::readOptional(*source, key, where, detail::readNullableString, *field); !read) {
                    return propagate(read.error());
                }
            }
        }
        return manifest;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("unexpected JSON failure: ") + e.what(),
                         std::string(context));
    }
}

std::string writeConsoleManifest(const ConsoleManifest& manifest) {
    json document = json::object();
    document["schemaVersion"] = kConsoleManifestSchema;
    document["model"] = manifest.model;
    document["kind"] = std::string(toString(manifest.kind));
    document["front"] = std::string(toString(manifest.front));
    json sockets = json::object();
    for (const auto& [role, node] : manifest.sockets) {
        sockets[role] = node;
    }
    document["sockets"] = std::move(sockets);
    if (manifest.scaleCm) {
        document["scaleCm"] = json::array({manifest.scaleCm->x, manifest.scaleCm->y, manifest.scaleCm->z});
    }
    document["source"] = json{{"url", manifest.source.url},
                              {"author", manifest.source.author},
                              {"license", manifest.source.license},
                              {"date", manifest.source.date},
                              {"modifications", manifest.source.modifications}};
    return document.dump(2) + "\n";
}

std::filesystem::path consoleFolder(const std::filesystem::path& contentDir, std::string_view system) {
    return contentDir / core::pathFromUtf8(kConsolesFolder) / core::pathFromUtf8(system);
}

glm::mat4 consoleFrontRotation(ConsoleFront front) noexcept {
    float degrees = 0.0F;
    switch (front) {
    case ConsoleFront::PlusZ:
        degrees = 0.0F;
        break;
    case ConsoleFront::MinusZ:
        degrees = 180.0F;
        break;
    case ConsoleFront::PlusX:
        degrees = -90.0F;
        break;
    case ConsoleFront::MinusX:
        degrees = 90.0F;
        break;
    }
    return glm::rotate(glm::mat4(1.0F), glm::radians(degrees), glm::vec3(0.0F, 1.0F, 0.0F));
}

glm::mat4 consolePlacement(const ModelData& model, ConsoleFront front) noexcept {
    if (model.bounds.isEmpty()) {
        return consoleFrontRotation(front);
    }
    const glm::vec3 bottom(0.5F * (model.bounds.min.x + model.bounds.max.x), model.bounds.min.y,
                           0.5F * (model.bounds.min.z + model.bounds.max.z));
    return consoleFrontRotation(front) * glm::translate(glm::mat4(1.0F), -bottom);
}

std::size_t CompressedTexture::bytes() const noexcept {
    std::size_t total = 0;
    for (const CompressedLevel& level : levels) {
        total += level.blocks.size();
    }
    return total;
}

std::vector<std::pair<std::uint32_t, bool>> sampledImages(const ModelData& model) {
    std::vector<std::pair<std::uint32_t, bool>> sampled;
    std::set<std::pair<std::uint32_t, bool>> seen;
    const auto add = [&](const TextureRef& ref, bool srgb) {
        if (!ref.imageIndex || *ref.imageIndex >= model.images.size()) {
            return;
        }
        const std::pair<std::uint32_t, bool> key{*ref.imageIndex, srgb};
        if (seen.insert(key).second) {
            sampled.push_back(key);
        }
    };
    for (const MaterialDesc& material : model.materials) {
        add(material.baseColorTexture, true);
        add(material.metallicRoughnessTexture, false);
        add(material.normalTexture, false);
        add(material.occlusionTexture, false);
        add(material.emissiveTexture, true);
    }
    return sampled;
}

std::size_t compressedTextureBytes(const ModelData& model) {
    std::size_t bytes = 0;
    for (const auto& [image, srgb] : sampledImages(model)) {
        const ImageData& data = model.images[image];
        bytes += chainBytes(data.width, data.height, usesBc3(data, srgb));
    }
    return bytes;
}

std::vector<CompressedTexture> compressConsoleTextures(const ModelData& model) {
    std::vector<CompressedTexture> textures;
    for (const auto& [image, srgb] : sampledImages(model)) {
        const ImageData rgba = asRgba(model.images[image]);
        if (!rgba.isValid()) {
            continue;
        }
        CompressedTexture texture;
        texture.image = image;
        texture.srgb = srgb;
        const bool bc3 = usesBc3(rgba, srgb);
        texture.format = bc3 ? (srgb ? TextureBlocks::Bc3Srgb : TextureBlocks::Bc3Linear)
                             : (srgb ? TextureBlocks::Bc1Srgb : TextureBlocks::Bc1Linear);
        const std::vector<ImageData> chain = srgb ? buildMipChain(rgba) : rawMipChain(rgba);
        for (const ImageData& level : chain) {
            texture.levels.push_back(
                CompressedLevel{level.width, level.height, bc3 ? encodeBc3(level) : encodeBc1(level)});
        }
        textures.push_back(std::move(texture));
    }
    return textures;
}

ConsoleCheck checkConsoleModel(const ModelData& model, const ConsoleManifest& manifest) {
    ConsoleCheck check;
    if (model.bounds.isEmpty()) {
        check.problems.emplace_back("the model draws nothing");
        return check;
    }
    const glm::vec3 raw = model.bounds.max - model.bounds.min;
    const bool turned = manifest.front == ConsoleFront::PlusX || manifest.front == ConsoleFront::MinusX;
    check.size = turned ? glm::vec3(raw.z, raw.y, raw.x) : raw;
    for (const auto& [axis, name] : std::array<std::pair<int, std::string_view>, 3>{{{0, "wide"}, {1, "high"}, {2, "deep"}}}) {
        const float side = check.size[axis];
        if (side < kConsoleMinSide || side > kConsoleMaxSide) {
            check.problems.push_back(fmt::format("it is {:.1f} cm {}: each side must be 5 to 45 cm", side * 100.0F, name));
        }
    }
    if (check.size.x > kConsoleMaxWidth || check.size.z > kConsoleMaxDepth) {
        check.problems.push_back(fmt::format("its footprint of {:.1f} x {:.1f} cm does not fit the cabinet beside the "
                                             "CRT ({:.1f} x {:.1f} cm at most)",
                                             check.size.x * 100.0F, check.size.z * 100.0F, kConsoleMaxWidth * 100.0F,
                                             kConsoleMaxDepth * 100.0F));
    }
    if (manifest.scaleCm) {
        for (int axis = 0; axis < 3; ++axis) {
            const float real = (*manifest.scaleCm)[axis] / 100.0F;
            if (std::abs(check.size[axis] - real) > kConsoleScaleTolerance * real) {
                check.problems.push_back(fmt::format("its size {:.1f} x {:.1f} x {:.1f} cm is not scaleCm's {:.1f} x "
                                                     "{:.1f} x {:.1f}",
                                                     check.size.x * 100.0F, check.size.y * 100.0F,
                                                     check.size.z * 100.0F, manifest.scaleCm->x, manifest.scaleCm->y,
                                                     manifest.scaleCm->z));
                break;
            }
        }
    }
    for (const ModelDrawItem& item : model.drawItems) {
        if (item.meshIndex < model.meshes.size()) {
            check.triangles += model.meshes[item.meshIndex].triangleCount();
        }
    }
    if (check.triangles > kConsoleMaxTriangles) {
        check.problems.push_back(
            fmt::format("it draws {} triangles: {} at most", check.triangles, kConsoleMaxTriangles));
    }
    check.textureBytes = compressedTextureBytes(model);
    if (check.textureBytes > kConsoleMaxTextureBytes) {
        check.problems.push_back(fmt::format("its textures take {:.2f} MiB once compressed: 2 MiB at most (1024 x "
                                             "1024 for the base colour, 512 x 512 for the other maps)",
                                             static_cast<double>(check.textureBytes) / (1024.0 * 1024.0)));
    }
    for (const auto& [role, node] : manifest.sockets) {
        if (socketFor(model, manifest, role) == nullptr) {
            check.problems.push_back(fmt::format("its {} names \"{}\", which is no Anchor_ node of the model", role, node));
        }
    }
    return check;
}

ConsoleMediaSocket consoleMediaSocket(const ModelData& model, const ConsoleManifest& manifest) {
    const glm::mat4 placement = consolePlacement(model, manifest.front);
    for (const std::string_view role : {std::string_view("CartridgeSlot"), std::string_view("DiscTray")}) {
        if (const geometry::Socket* socket = socketFor(model, manifest, role)) {
            const glm::vec3 size = geometry::socketSize(*socket);
            // The socket's frame without its node's scale: the item is scaled by the opening, never by the node.
            glm::mat4 frame = socket->transform;
            for (int axis = 0; axis < 3; ++axis) {
                const float length = glm::length(glm::vec3(frame[axis]));
                if (length > 0.0F) {
                    frame[axis] /= length;
                }
            }
            return ConsoleMediaSocket{placement * frame, glm::vec2(size.x, size.z), std::string(role)};
        }
    }
    const glm::vec3 top(0.5F * (model.bounds.min.x + model.bounds.max.x), model.bounds.max.y,
                        0.5F * (model.bounds.min.z + model.bounds.max.z));
    return ConsoleMediaSocket{placement * glm::translate(glm::mat4(1.0F), top), glm::vec2(0.0F), "top"};
}

Expected<ConsoleContent> loadConsoleContent(const std::filesystem::path& folder) {
    const std::filesystem::path manifestPath = folder / core::pathFromUtf8(kConsoleManifestName);
    auto text = core::readTextFile(manifestPath);
    if (!text) {
        return propagate(text.error());
    }
    auto manifest = parseConsoleManifest(*text, core::pathToUtf8Lossy(manifestPath));
    if (!manifest) {
        return propagate(manifest.error());
    }
    // Content is the user's: a model of at most 64 MiB, its geometry bounded by it.
    GltfLoadOptions options;
    options.maxFileBytes = 64ull << 20;
    auto model = loadGltf(folder / core::pathFromUtf8(manifest->model), options);
    if (!model) {
        return propagate(model.error());
    }
    return ConsoleContent{folder, std::move(*manifest), std::move(*model)};
}

} // namespace rv::assets
