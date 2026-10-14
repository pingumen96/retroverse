#include "support/glb_builder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <rv/assets/image_io.hpp>

namespace rv::test {
namespace {

constexpr int kFloat = 5126;
constexpr int kUnsignedInt = 5125;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

class BinWriter {
public:
    /// Appends `size` bytes at a 4-byte aligned offset and returns that offset.
    std::size_t append(const void* data, std::size_t size) {
        while (m_bytes.size() % 4 != 0) {
            m_bytes.push_back(std::byte{0});
        }
        const std::size_t offset = m_bytes.size();
        m_bytes.resize(offset + size);
        if (size > 0) {
            std::memcpy(m_bytes.data() + offset, data, size);
        }
        return offset;
    }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(m_bytes); }
    [[nodiscard]] bool empty() const noexcept { return m_bytes.empty(); }

private:
    std::vector<std::byte> m_bytes;
};

struct JsonArrays {
    nlohmann::json bufferViews = nlohmann::json::array();
    nlohmann::json accessors = nlohmann::json::array();
};

int addBufferView(JsonArrays& arrays, BinWriter& bin, const void* data, std::size_t size, std::optional<int> target) {
    const std::size_t offset = bin.append(data, size);
    nlohmann::json view = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}};
    if (target) {
        view["target"] = *target;
    }
    arrays.bufferViews.push_back(view);
    return static_cast<int>(arrays.bufferViews.size()) - 1;
}

template <class Vector>
int addFloatAccessor(JsonArrays& arrays, BinWriter& bin, const std::vector<Vector>& values, const char* type,
                     bool withBounds) {
    const int view = addBufferView(arrays, bin, values.data(), values.size() * sizeof(Vector), kArrayBuffer);
    nlohmann::json accessor = {
        {"bufferView", view}, {"componentType", kFloat}, {"count", values.size()}, {"type", type}};
    if (withBounds && !values.empty()) {
        // Non-finite components (deliberately broken test data) are skipped: JSON cannot represent them.
        nlohmann::json minJson = nlohmann::json::array();
        nlohmann::json maxJson = nlohmann::json::array();
        for (int c = 0; c < Vector::length(); ++c) {
            float minimum = 0.0f;
            float maximum = 0.0f;
            bool first = true;
            for (const Vector& value : values) {
                if (!std::isfinite(value[c])) {
                    continue;
                }
                minimum = first ? value[c] : std::min(minimum, value[c]);
                maximum = first ? value[c] : std::max(maximum, value[c]);
                first = false;
            }
            minJson.push_back(minimum);
            maxJson.push_back(maximum);
        }
        accessor["min"] = minJson;
        accessor["max"] = maxJson;
    }
    arrays.accessors.push_back(accessor);
    return static_cast<int>(arrays.accessors.size()) - 1;
}

std::string base64(const std::vector<std::byte>& bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t b0 = static_cast<std::uint8_t>(bytes[i]);
        const std::uint32_t b1 = i + 1 < bytes.size() ? static_cast<std::uint8_t>(bytes[i + 1]) : 0u;
        const std::uint32_t b2 = i + 2 < bytes.size() ? static_cast<std::uint8_t>(bytes[i + 2]) : 0u;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out += kAlphabet[(triple >> 18) & 63u];
        out += kAlphabet[(triple >> 12) & 63u];
        out += i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 63u] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[triple & 63u] : '=';
    }
    return out;
}

void appendU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

} // namespace

GltfParts buildGltfParts(const GlbDescription& description) {
    BinWriter bin;
    JsonArrays arrays;
    nlohmann::json root;
    root["asset"] = {{"version", "2.0"}, {"generator", "rv glb_builder"}};
    if (!description.extensionsUsed.empty()) {
        root["extensionsUsed"] = description.extensionsUsed;
    }
    if (!description.extensionsRequired.empty()) {
        root["extensionsRequired"] = description.extensionsRequired;
    }

    nlohmann::json nodes = nlohmann::json::array();
    std::vector<bool> isChild(description.nodes.size(), false);
    for (const GlbNode& node : description.nodes) {
        nlohmann::json json = {{"name", node.name}};
        if (node.matrix) {
            nlohmann::json matrix = nlohmann::json::array();
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    matrix.push_back((*node.matrix)[column][row]);
                }
            }
            json["matrix"] = matrix;
        } else {
            json["translation"] = {node.translation.x, node.translation.y, node.translation.z};
            json["rotation"] = {node.rotation.x, node.rotation.y, node.rotation.z, node.rotation.w};
            json["scale"] = {node.scale.x, node.scale.y, node.scale.z};
        }
        if (!node.children.empty()) {
            json["children"] = node.children;
            for (const int child : node.children) {
                if (child >= 0 && static_cast<std::size_t>(child) < isChild.size()) {
                    isChild[static_cast<std::size_t>(child)] = true;
                }
            }
        }
        if (node.mesh) {
            json["mesh"] = *node.mesh;
        }
        nodes.push_back(json);
    }
    if (!description.nodes.empty()) {
        root["nodes"] = nodes;
        std::vector<int> roots = description.sceneRoots;
        if (roots.empty()) {
            for (std::size_t n = 0; n < description.nodes.size(); ++n) {
                if (!isChild[n]) {
                    roots.push_back(static_cast<int>(n));
                }
            }
        }
        root["scenes"] = nlohmann::json::array({{{"nodes", roots}}});
        root["scene"] = 0;
    }

    nlohmann::json meshes = nlohmann::json::array();
    for (const GlbMesh& mesh : description.meshes) {
        nlohmann::json attributes;
        attributes["POSITION"] = addFloatAccessor(arrays, bin, mesh.positions, "VEC3", true);
        if (!mesh.normals.empty()) {
            attributes["NORMAL"] = addFloatAccessor(arrays, bin, mesh.normals, "VEC3", false);
        }
        if (!mesh.tangents.empty()) {
            attributes["TANGENT"] = addFloatAccessor(arrays, bin, mesh.tangents, "VEC4", false);
        }
        if (!mesh.uvs.empty()) {
            attributes["TEXCOORD_0"] = addFloatAccessor(arrays, bin, mesh.uvs, "VEC2", false);
        }
        nlohmann::json primitive = {{"attributes", attributes}, {"mode", mesh.mode}};
        if (!mesh.indices.empty()) {
            const int view = addBufferView(arrays, bin, mesh.indices.data(),
                                           mesh.indices.size() * sizeof(std::uint32_t), kElementArrayBuffer);
            arrays.accessors.push_back({{"bufferView", view},
                                        {"componentType", kUnsignedInt},
                                        {"count", mesh.indices.size()},
                                        {"type", "SCALAR"}});
            primitive["indices"] = static_cast<int>(arrays.accessors.size()) - 1;
        }
        if (mesh.material) {
            primitive["material"] = *mesh.material;
        }
        meshes.push_back({{"name", mesh.name}, {"primitives", nlohmann::json::array({primitive})}});
    }
    if (!meshes.empty()) {
        root["meshes"] = meshes;
    }

    nlohmann::json materials = nlohmann::json::array();
    nlohmann::json images = nlohmann::json::array();
    nlohmann::json textures = nlohmann::json::array();
    for (const GlbMaterial& material : description.materials) {
        const glm::vec4& color = material.baseColorFactor;
        nlohmann::json pbr = {{"baseColorFactor", {color.r, color.g, color.b, color.a}},
                              {"metallicFactor", material.metallicFactor},
                              {"roughnessFactor", material.roughnessFactor}};
        std::vector<std::byte> imageBytes = material.baseColorImageBytes;
        if (imageBytes.empty() && material.baseColorImage) {
            auto png = assets::encodePng(*material.baseColorImage);
            if (!png) {
                throw std::runtime_error("glb_builder: cannot encode the base color image: " + png.error().toString());
            }
            imageBytes = std::move(*png);
        }
        if (!imageBytes.empty()) {
            const int view = addBufferView(arrays, bin, imageBytes.data(), imageBytes.size(), std::nullopt);
            images.push_back({{"bufferView", view}, {"mimeType", "image/png"}});
            textures.push_back({{"source", static_cast<int>(images.size()) - 1}});
            pbr["baseColorTexture"] = {{"index", static_cast<int>(textures.size()) - 1}};
        }
        materials.push_back({{"name", material.name}, {"pbrMetallicRoughness", pbr}});
    }
    if (!materials.empty()) {
        root["materials"] = materials;
    }
    if (!images.empty()) {
        root["images"] = images;
        root["textures"] = textures;
    }
    if (!arrays.accessors.empty()) {
        root["accessors"] = arrays.accessors;
    }
    if (!arrays.bufferViews.empty()) {
        root["bufferViews"] = arrays.bufferViews;
    }

    GltfParts parts;
    parts.bin = std::move(bin).take();
    if (!parts.bin.empty()) {
        root["buffers"] = nlohmann::json::array({{{"byteLength", parts.bin.size()}}});
    }
    if (description.patchJson) {
        description.patchJson(root);
    }
    parts.json = std::move(root);
    return parts;
}

std::vector<std::byte> buildGlb(const GlbDescription& description) {
    GltfParts parts = buildGltfParts(description);
    std::string json = parts.json.dump();
    while (json.size() % 4 != 0) {
        json.push_back(' ');
    }
    std::vector<std::byte> bin = std::move(parts.bin);
    while (bin.size() % 4 != 0) {
        bin.push_back(std::byte{0});
    }
    const std::size_t total = 12 + 8 + json.size() + (bin.empty() ? 0 : 8 + bin.size());

    std::vector<std::byte> glb;
    glb.reserve(total);
    appendU32(glb, 0x46546C67u); // "glTF"
    appendU32(glb, 2u);
    appendU32(glb, static_cast<std::uint32_t>(total));
    appendU32(glb, static_cast<std::uint32_t>(json.size()));
    appendU32(glb, 0x4E4F534Au); // "JSON"
    for (const char c : json) {
        glb.push_back(static_cast<std::byte>(c));
    }
    if (!bin.empty()) {
        appendU32(glb, static_cast<std::uint32_t>(bin.size()));
        appendU32(glb, 0x004E4942u); // "BIN\0"
        glb.insert(glb.end(), bin.begin(), bin.end());
    }
    return glb;
}

std::string buildGltfWithEmbeddedBuffer(const GlbDescription& description) {
    GltfParts parts = buildGltfParts(description);
    if (!parts.bin.empty()) {
        parts.json["buffers"][0]["uri"] = "data:application/octet-stream;base64," + base64(parts.bin);
    }
    return parts.json.dump(2);
}

GltfFiles buildGltfWithExternalBuffer(const GlbDescription& description, std::string_view binUri) {
    GltfParts parts = buildGltfParts(description);
    if (!parts.bin.empty()) {
        parts.json["buffers"][0]["uri"] = std::string(binUri);
    }
    return GltfFiles{parts.json.dump(2), std::move(parts.bin)};
}

GlbMesh makeQuadMesh(std::string name, float width, float height, bool withNormals) {
    GlbMesh mesh;
    mesh.name = std::move(name);
    const float x = width * 0.5f;
    const float y = height * 0.5f;
    mesh.positions = {{-x, y, 0.0f}, {-x, -y, 0.0f}, {x, -y, 0.0f}, {x, y, 0.0f}};
    mesh.uvs = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
    if (withNormals) {
        mesh.normals.assign(4, glm::vec3(0.0f, 0.0f, 1.0f));
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

} // namespace rv::test
