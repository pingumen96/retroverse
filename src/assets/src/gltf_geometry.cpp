#include "gltf_geometry.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>

#include <rv/core/math.hpp>

namespace rv::assets::detail {
namespace {

namespace fg = fastgltf;

constexpr std::size_t kMaxVerticesPerPrimitive = std::size_t{1} << 24;
constexpr std::size_t kMaxIndicesPerPrimitive = std::size_t{1} << 26;

/// Element location of an accessor after bounds checks. `data` is null for accessors without a buffer view,
/// whose elements are all zero (glTF 2.0 §3.6.2.3).
struct AccessorView {
    const std::byte* data = nullptr;
    std::size_t count = 0;
    std::size_t stride = 0;
    fg::AccessorType type = fg::AccessorType::Invalid;
    fg::ComponentType componentType = fg::ComponentType::Invalid;
    bool normalized = false;
};

template <class T>
T loadUnaligned(const std::byte* bytes) noexcept {
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

std::string_view componentName(fg::ComponentType type) noexcept {
    switch (type) {
    case fg::ComponentType::Byte:
        return "BYTE";
    case fg::ComponentType::UnsignedByte:
        return "UNSIGNED_BYTE";
    case fg::ComponentType::Short:
        return "SHORT";
    case fg::ComponentType::UnsignedShort:
        return "UNSIGNED_SHORT";
    case fg::ComponentType::Int:
        return "INT";
    case fg::ComponentType::UnsignedInt:
        return "UNSIGNED_INT";
    case fg::ComponentType::Float:
        return "FLOAT";
    case fg::ComponentType::Double:
        return "DOUBLE";
    case fg::ComponentType::Invalid:
        break;
    }
    return "INVALID";
}

Expected<AccessorView> viewAccessor(const fg::Asset& asset, BufferSpans buffers, std::size_t accessorIndex,
                                    const std::string& what) {
    if (accessorIndex >= asset.accessors.size()) {
        return makeError(ErrorCode::ParseError,
                         what + " references accessor " + std::to_string(accessorIndex) + ", which does not exist");
    }
    const fg::Accessor& accessor = asset.accessors[accessorIndex];
    if (accessor.sparse.has_value()) {
        return makeError(ErrorCode::Unsupported, what + " uses a sparse accessor, which is not supported");
    }
    const std::size_t elementSize = fg::getElementByteSize(accessor.type, accessor.componentType);
    if (elementSize == 0) {
        return makeError(ErrorCode::ParseError, what + " uses an accessor with an invalid type");
    }
    AccessorView view;
    view.count = accessor.count;
    view.stride = elementSize;
    view.type = accessor.type;
    view.componentType = accessor.componentType;
    view.normalized = accessor.normalized;
    if (!accessor.bufferViewIndex.has_value()) {
        return view;
    }

    const std::size_t viewIndex = accessor.bufferViewIndex.value();
    auto bytes = bufferViewBytes(asset, buffers, viewIndex, what);
    if (!bytes) {
        return propagate(bytes.error());
    }
    const fg::BufferView& bufferView = asset.bufferViews[viewIndex];
    const std::size_t stride = bufferView.byteStride.has_value() ? bufferView.byteStride.value() : elementSize;
    if (stride < elementSize) {
        return makeError(ErrorCode::ParseError, what + ": the byte stride of buffer view " + std::to_string(viewIndex) +
                                                    " is smaller than an element");
    }
    if (accessor.count > 0) {
        const std::size_t length = bytes->size();
        const bool fits = accessor.byteOffset <= length && elementSize <= length - accessor.byteOffset &&
                          (accessor.count - 1) <= (length - accessor.byteOffset - elementSize) / stride;
        if (!fits) {
            return makeError(ErrorCode::ParseError, what + ": accessor " + std::to_string(accessorIndex) +
                                                        " reads past the end of buffer view " +
                                                        std::to_string(viewIndex));
        }
        view.data = bytes->data() + accessor.byteOffset;
    }
    view.stride = stride;
    return view;
}

Status checkCount(const AccessorView& view, std::size_t limit, const std::string& what) {
    if (view.count > limit) {
        return makeError(ErrorCode::OutOfRange, what + " has " + std::to_string(view.count) +
                                                    " elements; the limit is " + std::to_string(limit));
    }
    return ok();
}

Expected<std::vector<glm::vec3>> readVec3(const fg::Asset& asset, BufferSpans buffers, std::size_t accessorIndex,
                                          const std::string& what) {
    auto view = viewAccessor(asset, buffers, accessorIndex, what);
    if (!view) {
        return propagate(view.error());
    }
    if (view->type != fg::AccessorType::Vec3 || view->componentType != fg::ComponentType::Float) {
        return makeError(ErrorCode::Unsupported, what + " must be a FLOAT VEC3 accessor (found " +
                                                     std::string(componentName(view->componentType)) + ")");
    }
    if (auto status = checkCount(*view, kMaxVerticesPerPrimitive, what); !status) {
        return propagate(status.error());
    }
    std::vector<glm::vec3> values(view->count, glm::vec3(0.0f));
    if (view->data != nullptr) {
        for (std::size_t i = 0; i < view->count; ++i) {
            const std::byte* element = view->data + i * view->stride;
            values[i] = glm::vec3(loadUnaligned<float>(element), loadUnaligned<float>(element + 4),
                                  loadUnaligned<float>(element + 8));
        }
    }
    return values;
}

Expected<std::vector<glm::vec4>> readVec4(const fg::Asset& asset, BufferSpans buffers, std::size_t accessorIndex,
                                          const std::string& what) {
    auto view = viewAccessor(asset, buffers, accessorIndex, what);
    if (!view) {
        return propagate(view.error());
    }
    if (view->type != fg::AccessorType::Vec4 || view->componentType != fg::ComponentType::Float) {
        return makeError(ErrorCode::Unsupported, what + " must be a FLOAT VEC4 accessor (found " +
                                                     std::string(componentName(view->componentType)) + ")");
    }
    if (auto status = checkCount(*view, kMaxVerticesPerPrimitive, what); !status) {
        return propagate(status.error());
    }
    std::vector<glm::vec4> values(view->count, glm::vec4(0.0f));
    if (view->data != nullptr) {
        for (std::size_t i = 0; i < view->count; ++i) {
            const std::byte* element = view->data + i * view->stride;
            values[i] = glm::vec4(loadUnaligned<float>(element), loadUnaligned<float>(element + 4),
                                  loadUnaligned<float>(element + 8), loadUnaligned<float>(element + 12));
        }
    }
    return values;
}

/// TEXCOORD_n: FLOAT, or normalized UNSIGNED_BYTE / UNSIGNED_SHORT (glTF 2.0 §3.7.2.1).
Expected<std::vector<glm::vec2>> readTexCoords(const fg::Asset& asset, BufferSpans buffers, std::size_t accessorIndex,
                                               const std::string& what) {
    auto view = viewAccessor(asset, buffers, accessorIndex, what);
    if (!view) {
        return propagate(view.error());
    }
    const bool isFloat = view->componentType == fg::ComponentType::Float;
    const bool isUnorm8 = view->componentType == fg::ComponentType::UnsignedByte && view->normalized;
    const bool isUnorm16 = view->componentType == fg::ComponentType::UnsignedShort && view->normalized;
    if (view->type != fg::AccessorType::Vec2 || !(isFloat || isUnorm8 || isUnorm16)) {
        return makeError(ErrorCode::Unsupported,
                         what + " must be a VEC2 accessor of FLOAT or normalized UNSIGNED_BYTE/UNSIGNED_SHORT");
    }
    if (auto status = checkCount(*view, kMaxVerticesPerPrimitive, what); !status) {
        return propagate(status.error());
    }
    std::vector<glm::vec2> values(view->count, glm::vec2(0.0f));
    if (view->data != nullptr) {
        for (std::size_t i = 0; i < view->count; ++i) {
            const std::byte* element = view->data + i * view->stride;
            if (isFloat) {
                values[i] = glm::vec2(loadUnaligned<float>(element), loadUnaligned<float>(element + 4));
            } else if (isUnorm8) {
                values[i] = glm::vec2(static_cast<float>(loadUnaligned<std::uint8_t>(element)),
                                      static_cast<float>(loadUnaligned<std::uint8_t>(element + 1))) /
                            255.0f;
            } else {
                values[i] = glm::vec2(static_cast<float>(loadUnaligned<std::uint16_t>(element)),
                                      static_cast<float>(loadUnaligned<std::uint16_t>(element + 2))) /
                            65535.0f;
            }
        }
    }
    return values;
}

Expected<std::vector<std::uint32_t>> readIndices(const fg::Asset& asset, BufferSpans buffers, std::size_t accessorIndex,
                                                 const std::string& what) {
    auto view = viewAccessor(asset, buffers, accessorIndex, what);
    if (!view) {
        return propagate(view.error());
    }
    const fg::ComponentType component = view->componentType;
    if (view->type != fg::AccessorType::Scalar ||
        (component != fg::ComponentType::UnsignedByte && component != fg::ComponentType::UnsignedShort &&
         component != fg::ComponentType::UnsignedInt)) {
        return makeError(ErrorCode::Unsupported,
                         what + " must be a SCALAR accessor of UNSIGNED_BYTE, UNSIGNED_SHORT or UNSIGNED_INT");
    }
    if (auto status = checkCount(*view, kMaxIndicesPerPrimitive, what); !status) {
        return propagate(status.error());
    }
    std::vector<std::uint32_t> indices(view->count, 0u);
    if (view->data != nullptr) {
        for (std::size_t i = 0; i < view->count; ++i) {
            const std::byte* element = view->data + i * view->stride;
            if (component == fg::ComponentType::UnsignedByte) {
                indices[i] = loadUnaligned<std::uint8_t>(element);
            } else if (component == fg::ComponentType::UnsignedShort) {
                indices[i] = loadUnaligned<std::uint16_t>(element);
            } else {
                indices[i] = loadUnaligned<std::uint32_t>(element);
            }
        }
    }
    return indices;
}

std::string_view modeName(fg::PrimitiveType type) noexcept {
    switch (type) {
    case fg::PrimitiveType::Points:
        return "POINTS";
    case fg::PrimitiveType::Lines:
        return "LINES";
    case fg::PrimitiveType::LineLoop:
        return "LINE_LOOP";
    case fg::PrimitiveType::LineStrip:
        return "LINE_STRIP";
    case fg::PrimitiveType::Triangles:
        return "TRIANGLES";
    case fg::PrimitiveType::TriangleStrip:
        return "TRIANGLE_STRIP";
    case fg::PrimitiveType::TriangleFan:
        return "TRIANGLE_FAN";
    }
    return "UNKNOWN";
}

/// Triangle strips and fans become lists, keeping the glTF winding rules (glTF 2.0 §3.7.2.1).
Expected<std::vector<std::uint32_t>> toTriangleList(fg::PrimitiveType type, std::vector<std::uint32_t> indices,
                                                    const std::string& what) {
    if (type == fg::PrimitiveType::Triangles) {
        if (indices.size() % 3 != 0) {
            return makeError(ErrorCode::ParseError,
                             what + " has " + std::to_string(indices.size()) + " indices, not a multiple of 3");
        }
        return indices;
    }
    std::vector<std::uint32_t> list;
    if (indices.size() < 3) {
        return list;
    }
    list.reserve((indices.size() - 2) * 3);
    for (std::size_t i = 0; i + 2 < indices.size(); ++i) {
        if (type == fg::PrimitiveType::TriangleStrip) {
            const bool odd = (i % 2) == 1;
            list.push_back(indices[i]);
            list.push_back(indices[odd ? i + 2 : i + 1]);
            list.push_back(indices[odd ? i + 1 : i + 2]);
        } else { // fan
            list.push_back(indices[i + 1]);
            list.push_back(indices[i + 2]);
            list.push_back(indices[0]);
        }
    }
    return list;
}

template <class Vector>
bool allFinite(const std::vector<Vector>& values) noexcept {
    for (const Vector& value : values) {
        for (int c = 0; c < Vector::length(); ++c) {
            if (!std::isfinite(value[c])) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool UvTransform::isIdentity() const noexcept {
    return offset == glm::vec2(0.0f) && rotation == 0.0f && scale == glm::vec2(1.0f);
}

glm::vec2 UvTransform::apply(const glm::vec2& uv) const noexcept {
    const glm::vec2 scaled = scale * uv;
    const float c = std::cos(rotation);
    const float s = std::sin(rotation);
    return offset + glm::vec2(c * scaled.x + s * scaled.y, -s * scaled.x + c * scaled.y);
}

Expected<std::span<const std::byte>> bufferViewBytes(const fg::Asset& asset, BufferSpans buffers, std::size_t viewIndex,
                                                     const std::string& what) {
    if (viewIndex >= asset.bufferViews.size()) {
        return makeError(ErrorCode::ParseError,
                         what + " references buffer view " + std::to_string(viewIndex) + ", which does not exist");
    }
    const fg::BufferView& view = asset.bufferViews[viewIndex];
    if (view.meshoptCompression) {
        return makeError(ErrorCode::Unsupported, what + " uses meshopt compression, which is not supported");
    }
    if (view.bufferIndex >= buffers.size()) {
        return makeError(ErrorCode::ParseError, what + ": buffer view " + std::to_string(viewIndex) +
                                                    " references a buffer that does not exist");
    }
    const std::span<const std::byte> buffer = buffers[view.bufferIndex];
    if (view.byteOffset > buffer.size() || view.byteLength > buffer.size() - view.byteOffset) {
        return makeError(ErrorCode::ParseError, what + ": buffer view " + std::to_string(viewIndex) +
                                                    " extends past the end of buffer " +
                                                    std::to_string(view.bufferIndex));
    }
    return buffer.subspan(view.byteOffset, view.byteLength);
}

Expected<std::optional<geometry::MeshData>>
convertPrimitive(const fg::Asset& asset, BufferSpans buffers, const fg::Primitive& primitive,
                 const PrimitiveConversion& conversion, const std::string& label, std::vector<std::string>& warnings) {
    const fg::PrimitiveType type = primitive.type;
    if (type != fg::PrimitiveType::Triangles && type != fg::PrimitiveType::TriangleStrip &&
        type != fg::PrimitiveType::TriangleFan) {
        warnings.push_back(label + ": " + std::string(modeName(type)) + " primitives are not supported; skipped");
        return std::optional<geometry::MeshData>{};
    }
    if (!primitive.targets.empty()) {
        warnings.push_back(label + ": morph targets are ignored");
    }

    const auto positionAttribute = primitive.findAttribute("POSITION");
    if (positionAttribute == primitive.attributes.end()) {
        return makeError(ErrorCode::ParseError, label + " has no POSITION attribute");
    }
    auto positions = readVec3(asset, buffers, positionAttribute->accessorIndex, label + " POSITION");
    if (!positions) {
        return propagate(positions.error());
    }
    const std::size_t vertexCount = positions->size();
    const auto sameCount = [&](std::size_t count, std::string_view attribute) -> Status {
        if (count != vertexCount) {
            return makeError(ErrorCode::ParseError, label + " " + std::string(attribute) + " has " +
                                                        std::to_string(count) + " elements but POSITION has " +
                                                        std::to_string(vertexCount));
        }
        return ok();
    };

    std::vector<glm::vec3> normals;
    if (const auto attribute = primitive.findAttribute("NORMAL"); attribute != primitive.attributes.end()) {
        auto values = readVec3(asset, buffers, attribute->accessorIndex, label + " NORMAL");
        if (!values) {
            return propagate(values.error());
        }
        if (auto status = sameCount(values->size(), "NORMAL"); !status) {
            return propagate(status.error());
        }
        normals = std::move(*values);
    }
    std::vector<glm::vec4> tangents;
    if (const auto attribute = primitive.findAttribute("TANGENT"); attribute != primitive.attributes.end()) {
        auto values = readVec4(asset, buffers, attribute->accessorIndex, label + " TANGENT");
        if (!values) {
            return propagate(values.error());
        }
        if (auto status = sameCount(values->size(), "TANGENT"); !status) {
            return propagate(status.error());
        }
        tangents = std::move(*values);
    }
    std::vector<glm::vec2> uvs(vertexCount, glm::vec2(0.0f));
    if (const auto attribute = primitive.findAttribute("TEXCOORD_0"); attribute != primitive.attributes.end()) {
        auto values = readTexCoords(asset, buffers, attribute->accessorIndex, label + " TEXCOORD_0");
        if (!values) {
            return propagate(values.error());
        }
        if (auto status = sameCount(values->size(), "TEXCOORD_0"); !status) {
            return propagate(status.error());
        }
        uvs = std::move(*values);
    }
    if (!allFinite(*positions) || !allFinite(normals) || !allFinite(tangents) || !allFinite(uvs)) {
        return makeError(ErrorCode::ParseError, label + " has NaN or infinite vertex data");
    }

    std::vector<std::uint32_t> indices;
    if (primitive.indicesAccessor.has_value()) {
        auto values = readIndices(asset, buffers, primitive.indicesAccessor.value(), label + " indices");
        if (!values) {
            return propagate(values.error());
        }
        for (const std::uint32_t index : *values) {
            if (index >= vertexCount) {
                return makeError(ErrorCode::ParseError, label + " has index " + std::to_string(index) + " but only " +
                                                            std::to_string(vertexCount) + " vertices");
            }
        }
        indices = std::move(*values);
    } else {
        indices.resize(vertexCount);
        std::iota(indices.begin(), indices.end(), 0u);
    }
    auto triangles = toTriangleList(type, std::move(indices), label);
    if (!triangles) {
        return propagate(triangles.error());
    }
    if (triangles->empty()) {
        warnings.push_back(label + " has no triangles; skipped");
        return std::optional<geometry::MeshData>{};
    }

    if (conversion.uvTransform != nullptr && !conversion.uvTransform->isIdentity()) {
        for (glm::vec2& uv : uvs) {
            uv = conversion.uvTransform->apply(uv);
        }
    }

    geometry::MeshData mesh;
    mesh.name = label;
    bool regenerateTangents = conversion.generateTangents;
    if (normals.empty()) {
        // glTF 2.0 §3.7.2.1: without normals, use flat normals and ignore the provided tangents.
        warnings.push_back(label + " has no normals; flat normals were generated");
        mesh.vertices.reserve(triangles->size());
        for (std::size_t t = 0; t + 2 < triangles->size(); t += 3) {
            const std::uint32_t corners[3] = {(*triangles)[t], (*triangles)[t + 1], (*triangles)[t + 2]};
            const glm::vec3 p0 = (*positions)[corners[0]];
            glm::vec3 normal = glm::cross((*positions)[corners[1]] - p0, (*positions)[corners[2]] - p0);
            const float length = glm::length(normal);
            normal = length > 1e-20f ? normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
            for (const std::uint32_t corner : corners) {
                geometry::Vertex vertex;
                vertex.position = (*positions)[corner];
                vertex.normal = normal;
                vertex.uv0 = uvs[corner];
                mesh.vertices.push_back(vertex);
            }
        }
        mesh.indices.resize(mesh.vertices.size());
        std::iota(mesh.indices.begin(), mesh.indices.end(), 0u);
    } else {
        std::size_t degenerateNormals = 0;
        mesh.vertices.resize(vertexCount);
        for (std::size_t v = 0; v < vertexCount; ++v) {
            geometry::Vertex& vertex = mesh.vertices[v];
            vertex.position = (*positions)[v];
            const float length = glm::length(normals[v]);
            if (length > 1e-12f) {
                vertex.normal = normals[v] / length;
            } else {
                vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                ++degenerateNormals;
            }
            vertex.uv0 = uvs[v];
        }
        if (degenerateNormals > 0) {
            warnings.push_back(label + " has " + std::to_string(degenerateNormals) +
                               " zero-length normals; replaced with +Y");
        }
        if (!tangents.empty()) {
            regenerateTangents = false;
            for (std::size_t v = 0; v < vertexCount; ++v) {
                const glm::vec3 direction(tangents[v]);
                const float length = glm::length(direction);
                if (length <= 1e-12f) {
                    regenerateTangents = true;
                    break;
                }
                mesh.vertices[v].tangent = glm::vec4(direction / length, tangents[v].w < 0.0f ? -1.0f : 1.0f);
            }
            if (regenerateTangents) {
                warnings.push_back(label + " has zero-length tangents; tangents were regenerated");
            }
        }
        mesh.indices = std::move(*triangles);
    }
    if (regenerateTangents) {
        geometry::computeTangents(mesh);
    }
    geometry::computeBounds(mesh);
    if (auto status = geometry::validateMesh(mesh); !status) {
        return makeError(ErrorCode::ParseError, label + ": " + status.error().message);
    }
    return std::optional<geometry::MeshData>(std::move(mesh));
}

} // namespace rv::assets::detail
