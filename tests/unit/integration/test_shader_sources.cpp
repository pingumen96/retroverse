// Cross-module integration checks (GPU-free) for the shader sources in assets/shaders: every program preprocesses
// with rv::assets::ShaderSourceLoader, and the std140 uniform blocks, material flags and texture units declared in
// GLSL match their C++ mirrors in rv/render/uniform_blocks.hpp (binding points, member types, offsets, sizes), so a
// change on one side cannot silently skew the other.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <rv/assets/shader_source.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/render/uniform_blocks.hpp>

namespace fs = std::filesystem;

namespace {

fs::path shaderRoot() {
    return rv::core::pathFromUtf8(RV_TEST_ASSET_DIR) / "shaders";
}

/// GLSL text of a shader file with comments removed.
std::string glslWithoutComments(const std::string& relativePath) {
    const auto text = rv::core::readTextFile(shaderRoot() / rv::core::pathFromUtf8(relativePath));
    REQUIRE(text.has_value());
    std::string source = std::regex_replace(*text, std::regex("/\\*[\\s\\S]*?\\*/"), " ");
    return std::regex_replace(source, std::regex("//[^\\n]*"), "");
}

// --- A small std140 layout engine: scalars, vectors, mat4, structs and arrays of them --------------------------------

struct Member {
    std::string type;
    std::string name;
    std::size_t arraySize = 0; ///< 0: not an array
};

using Structs = std::map<std::string, std::vector<Member>>;

/// Integer `#define NAME VALUE` constants.
std::map<std::string, std::size_t> parseDefines(const std::string& source) {
    std::map<std::string, std::size_t> defines;
    const std::regex define("#define\\s+(\\w+)\\s+(\\d+)");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), define); it != std::sregex_iterator(); ++it) {
        defines[(*it)[1].str()] = std::stoul((*it)[2].str());
    }
    return defines;
}

std::vector<Member> parseMembers(const std::string& body, const std::map<std::string, std::size_t>& defines) {
    std::vector<Member> members;
    const std::regex member("(\\w+)\\s+(\\w+)\\s*(?:\\[\\s*(\\w+)\\s*\\])?\\s*;");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), member); it != std::sregex_iterator(); ++it) {
        Member parsed{(*it)[1].str(), (*it)[2].str(), 0};
        if ((*it)[3].matched) {
            const std::string size = (*it)[3].str();
            const auto defined = defines.find(size);
            parsed.arraySize = defined != defines.end() ? defined->second : std::stoul(size);
        }
        members.push_back(parsed);
    }
    return members;
}

Structs parseStructs(const std::string& source, const std::map<std::string, std::size_t>& defines) {
    Structs structs;
    const std::regex declaration("struct\\s+(\\w+)\\s*\\{([^}]*)\\}");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), declaration); it != std::sregex_iterator();
         ++it) {
        structs[(*it)[1].str()] = parseMembers((*it)[2].str(), defines);
    }
    return structs;
}

/// Members of `layout(std140, binding = N) uniform <block> { ... };` and N.
std::vector<Member> parseBlock(const std::string& source, const std::string& block, int& binding,
                               const std::map<std::string, std::size_t>& defines) {
    const std::regex header("layout\\s*\\(\\s*std140\\s*,\\s*binding\\s*=\\s*(\\d+)\\s*\\)\\s*uniform\\s+" + block +
                            "\\s*\\{([^}]*)\\}");
    std::smatch match;
    REQUIRE(std::regex_search(source, match, header));
    binding = std::stoi(match[1].str());
    return parseMembers(match[2].str(), defines);
}

struct Std140Type {
    std::size_t alignment;
    std::size_t size;
};

std::size_t roundUp(std::size_t value, std::size_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

std::optional<Std140Type> std140(const std::string& type, const Structs& structs);

/// Offsets of `members` laid out one after the other, and the end of the last one.
std::vector<std::size_t> layoutMembers(const std::vector<Member>& members, const Structs& structs, std::size_t& end) {
    std::vector<std::size_t> offsets;
    std::size_t offset = 0;
    for (const Member& member : members) {
        INFO(member.type << " " << member.name);
        const auto layout = std140(member.type, structs);
        REQUIRE(layout.has_value());
        std::size_t alignment = layout->alignment;
        std::size_t size = layout->size;
        if (member.arraySize > 0) {
            // Array elements are aligned and padded like vec4s.
            alignment = roundUp(alignment, 16);
            size = roundUp(size, 16) * member.arraySize;
        }
        offset = roundUp(offset, alignment);
        offsets.push_back(offset);
        offset += size;
    }
    end = offset;
    return offsets;
}

std::optional<Std140Type> std140(const std::string& type, const Structs& structs) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool") {
        return Std140Type{4, 4};
    }
    if (type == "vec2") {
        return Std140Type{8, 8};
    }
    if (type == "vec3") {
        return Std140Type{16, 12};
    }
    if (type == "vec4") {
        return Std140Type{16, 16};
    }
    if (type == "mat4") {
        return Std140Type{16, 64};
    }
    const auto found = structs.find(type);
    if (found == structs.end()) {
        return std::nullopt;
    }
    // A struct is aligned like its most aligned member, rounded up to a vec4, and padded to that alignment.
    std::size_t alignment = 16;
    for (const Member& member : found->second) {
        const auto layout = std140(member.type, structs);
        if (!layout) {
            return std::nullopt;
        }
        alignment = std::max(alignment, roundUp(layout->alignment, 16));
    }
    std::size_t end = 0;
    layoutMembers(found->second, structs, end);
    return Std140Type{alignment, roundUp(end, alignment)};
}

struct ExpectedMember {
    const char* type;
    const char* name;
    std::size_t offset;
    std::size_t arraySize = 0;
};

/// Checks types, names, order and std140 offsets of `members` against the C++ mirror, and the padded size.
void checkLayout(const std::vector<Member>& members, const Structs& structs,
                 const std::vector<ExpectedMember>& expected, std::size_t cppSize) {
    REQUIRE(members.size() == expected.size());
    std::size_t end = 0;
    const std::vector<std::size_t> offsets = layoutMembers(members, structs, end);
    for (std::size_t i = 0; i < members.size(); ++i) {
        INFO(members[i].type << " " << members[i].name);
        CHECK(members[i].type == expected[i].type);
        CHECK(members[i].name == expected[i].name);
        CHECK(members[i].arraySize == expected[i].arraySize);
        CHECK(offsets[i] == expected[i].offset);
    }
    // std140 rounds block and struct sizes up to a multiple of 16 bytes; the C++ structs pad to the same size.
    CHECK(roundUp(end, 16) == cppSize);
}

} // namespace

TEST_CASE("integration/shaders: every program in assets/shaders preprocesses", "[integration][shader]") {
    const rv::assets::ShaderSourceLoader loader(shaderRoot());
    std::vector<std::string> stages;
    for (const auto& entry : fs::recursive_directory_iterator(shaderRoot())) {
        const fs::path extension = entry.path().extension();
        if (entry.is_regular_file() && (extension == ".vert" || extension == ".frag")) {
            stages.push_back(entry.path().lexically_relative(shaderRoot()).generic_string());
        }
    }
    std::sort(stages.begin(), stages.end());
    // The programs of the application: pbr (PBR_Base) and unlit_emissive (Unlit_Emissive), which shares pbr.vert.
    CHECK(stages == std::vector<std::string>{"pbr/pbr.frag", "pbr/pbr.vert", "unlit/unlit_emissive.frag"});

    // The sRGB self-check builds the unlit program with RV_UNLIT_LINEAR_OUTPUT.
    const std::vector<std::string> selfCheckDefines{"RV_UNLIT_LINEAR_OUTPUT"};
    for (const std::string& stage : stages) {
        for (const std::vector<std::string>& defines : {std::vector<std::string>{}, selfCheckDefines}) {
            INFO(stage << " with " << defines.size() << " define(s)");
            const auto source = loader.load(stage, defines);
            REQUIRE(source.has_value());
            CHECK(source->text.starts_with("#version 450 core"));
            // Every stage shares the frame block through the common include.
            CHECK(std::find(source->files.begin(), source->files.end(), fs::path("common/common.glsl")) !=
                  source->files.end());
        }
    }
    // Both fragment stages read the lights block (exposure) and the material block.
    for (const char* stage : {"pbr/pbr.frag", "unlit/unlit_emissive.frag"}) {
        INFO(stage);
        const auto source = loader.load(stage);
        REQUIRE(source.has_value());
        for (const char* include : {"common/lights.glsl", "common/tonemap.glsl", "common/material.glsl"}) {
            CHECK(std::find(source->files.begin(), source->files.end(), fs::path(include)) != source->files.end());
        }
    }
}

TEST_CASE("integration/shaders: the GLSL FrameUniforms block matches rv::gl::FrameUniforms", "[integration][shader]") {
    const std::string source = glslWithoutComments("common/common.glsl");
    const auto defines = parseDefines(source);
    int binding = -1;
    const std::vector<Member> members = parseBlock(source, "FrameUniforms", binding, defines);
    CHECK(binding == static_cast<int>(rv::gl::toIndex(rv::gl::UniformBinding::Frame)));
    checkLayout(members, {},
                {
                    {"mat4", "uView", offsetof(rv::gl::FrameUniforms, view)},
                    {"mat4", "uProjection", offsetof(rv::gl::FrameUniforms, projection)},
                    {"mat4", "uViewProjection", offsetof(rv::gl::FrameUniforms, viewProjection)},
                    {"vec3", "uCameraPosition", offsetof(rv::gl::FrameUniforms, cameraPosition)},
                    {"float", "uTime", offsetof(rv::gl::FrameUniforms, time)},
                },
                sizeof(rv::gl::FrameUniforms));
}

TEST_CASE("integration/shaders: the GLSL LightsUniforms block matches rv::gl::LightsUniforms",
          "[integration][shader]") {
    const std::string source = glslWithoutComments("common/lights.glsl");
    const auto defines = parseDefines(source);
    REQUIRE(defines.contains("RV_MAX_PUNCTUAL_LIGHTS"));
    CHECK(defines.at("RV_MAX_PUNCTUAL_LIGHTS") == rv::gl::kMaxPunctualLights);
    const Structs structs = parseStructs(source, defines);
    REQUIRE(structs.contains("PunctualLight"));

    using rv::gl::PunctualLightUniforms;
    checkLayout(structs.at("PunctualLight"), structs,
                {
                    {"vec3", "position", offsetof(PunctualLightUniforms, position)},
                    {"float", "range", offsetof(PunctualLightUniforms, range)},
                    {"vec3", "color", offsetof(PunctualLightUniforms, color)},
                    {"float", "intensity", offsetof(PunctualLightUniforms, intensity)},
                    {"vec3", "direction", offsetof(PunctualLightUniforms, direction)},
                    {"float", "spotScale", offsetof(PunctualLightUniforms, spotScale)},
                    {"float", "spotOffset", offsetof(PunctualLightUniforms, spotOffset)},
                },
                sizeof(PunctualLightUniforms));
    // Array elements of the struct are exactly one C++ struct apart.
    CHECK(std140("PunctualLight", structs)->size == sizeof(PunctualLightUniforms));

    int binding = -1;
    const std::vector<Member> members = parseBlock(source, "LightsUniforms", binding, defines);
    CHECK(binding == static_cast<int>(rv::gl::toIndex(rv::gl::UniformBinding::Lights)));
    using rv::gl::LightsUniforms;
    checkLayout(members, structs,
                {
                    {"PunctualLight", "uPunctualLights", offsetof(LightsUniforms, punctualLights),
                     rv::gl::kMaxPunctualLights},
                    {"vec3", "uDirectionalDirection", offsetof(LightsUniforms, directionalDirection)},
                    {"float", "uDirectionalIntensity", offsetof(LightsUniforms, directionalIntensity)},
                    {"vec3", "uDirectionalColor", offsetof(LightsUniforms, directionalColor)},
                    {"int", "uPunctualLightCount", offsetof(LightsUniforms, punctualLightCount)},
                    {"vec3", "uAmbientSky", offsetof(LightsUniforms, ambientSky)},
                    {"float", "uAmbientIntensity", offsetof(LightsUniforms, ambientIntensity)},
                    {"vec3", "uAmbientGround", offsetof(LightsUniforms, ambientGround)},
                    {"float", "uExposure", offsetof(LightsUniforms, exposure)},
                },
                sizeof(LightsUniforms));
}

TEST_CASE("integration/shaders: the GLSL material block, flags and texture units match their C++ mirrors",
          "[integration][shader]") {
    const std::string source = glslWithoutComments("common/material.glsl");
    const auto defines = parseDefines(source);
    int binding = -1;
    const std::vector<Member> members = parseBlock(source, "MaterialUniforms", binding, defines);
    CHECK(binding == static_cast<int>(rv::gl::toIndex(rv::gl::UniformBinding::Material)));
    using rv::gl::MaterialUniforms;
    checkLayout(members, {},
                {
                    {"vec4", "uBaseColorFactor", offsetof(MaterialUniforms, baseColorFactor)},
                    {"vec3", "uEmissive", offsetof(MaterialUniforms, emissive)},
                    {"float", "uMetallic", offsetof(MaterialUniforms, metallic)},
                    {"float", "uRoughness", offsetof(MaterialUniforms, roughness)},
                    {"float", "uNormalScale", offsetof(MaterialUniforms, normalScale)},
                    {"float", "uOcclusionStrength", offsetof(MaterialUniforms, occlusionStrength)},
                    {"float", "uAlphaCutoff", offsetof(MaterialUniforms, alphaCutoff)},
                    {"float", "uEmissiveStrength", offsetof(MaterialUniforms, emissiveStrength)},
                    {"uint", "uMaterialFlags", offsetof(MaterialUniforms, flags)},
                },
                sizeof(MaterialUniforms));

    namespace flags = rv::gl::material_flags;
    const std::map<std::string, std::uint32_t> expectedFlags{
        {"RV_MATERIAL_BASE_COLOR_TEXTURE", flags::kBaseColorTexture},
        {"RV_MATERIAL_METALLIC_ROUGHNESS_TEXTURE", flags::kMetallicRoughnessTexture},
        {"RV_MATERIAL_NORMAL_TEXTURE", flags::kNormalTexture},
        {"RV_MATERIAL_OCCLUSION_TEXTURE", flags::kOcclusionTexture},
        {"RV_MATERIAL_EMISSIVE_TEXTURE", flags::kEmissiveTexture},
        {"RV_MATERIAL_ALPHA_MASK", flags::kAlphaMask},
        {"RV_MATERIAL_ALPHA_BLEND", flags::kAlphaBlend},
        {"RV_MATERIAL_DOUBLE_SIDED", flags::kDoubleSided},
    };
    std::map<std::string, std::uint32_t> glslFlags;
    const std::regex flag("const\\s+uint\\s+(RV_MATERIAL_\\w+)\\s*=\\s*(\\d+)u\\s*;");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), flag); it != std::sregex_iterator(); ++it) {
        glslFlags[(*it)[1].str()] = static_cast<std::uint32_t>(std::stoul((*it)[2].str()));
    }
    CHECK(glslFlags == expectedFlags);

    using rv::gl::MaterialTextureSlot;
    const std::map<std::string, std::uint32_t> expectedUnits{
        {"uBaseColorTexture", rv::gl::toIndex(MaterialTextureSlot::BaseColor)},
        {"uMetallicRoughnessTexture", rv::gl::toIndex(MaterialTextureSlot::MetallicRoughness)},
        {"uNormalTexture", rv::gl::toIndex(MaterialTextureSlot::Normal)},
        {"uOcclusionTexture", rv::gl::toIndex(MaterialTextureSlot::Occlusion)},
        {"uEmissiveTexture", rv::gl::toIndex(MaterialTextureSlot::Emissive)},
    };
    std::map<std::string, std::uint32_t> glslUnits;
    const std::regex sampler("layout\\s*\\(\\s*binding\\s*=\\s*(\\d+)\\s*\\)\\s*uniform\\s+sampler2D\\s+(\\w+)\\s*;");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), sampler); it != std::sregex_iterator(); ++it) {
        glslUnits[(*it)[2].str()] = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
    }
    CHECK(glslUnits == expectedUnits);
    CHECK(expectedUnits.size() == rv::gl::kMaterialTextureSlotCount);
}
