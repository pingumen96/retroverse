// Cross-module integration checks (GPU-free) for the shader sources in assets/shaders: every program preprocesses
// with rv::assets::ShaderSourceLoader, and the std140 uniform blocks, material flags and texture units declared in
// GLSL match their C++ mirrors in rv/render/uniform_blocks.hpp (binding points, member types, offsets, sizes), so a
// change on one side cannot silently skew the other. The same holds for the display transform (tonemap.glsl and
// rv/render/tonemap.hpp); the material shaders are checked to write linear radiance and its emitted part to the
// attachments of the HDR target (exposure and tonemapping are the post pass's), fragment stages to sample their
// textures before they may discard, the post-processing passes to draw without vertex buffers, the bloom filters to
// keep the energy of the image and the first bloom level to take the Karis average of every tap. The shadow and
// ambient occlusion blocks, their texture units and constants match their mirrors too, the mesh programs share one
// invariant vertex stage (the depth pre-pass and the lit pass compute the same depth) and the ambient occlusion
// darkens the ambient light only.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rv/assets/shader_source.hpp>
#include <rv/core/file_io.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/core/text.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/tonemap.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/debug_lines.hpp>

namespace fs = std::filesystem;

namespace {

fs::path shaderRoot() {
    return rv::core::pathFromUtf8(RV_TEST_ASSET_DIR) / "shaders";
}

/// GLSL text with comments removed.
std::string withoutComments(const std::string& text) {
    std::string source = std::regex_replace(text, std::regex("/\\*[\\s\\S]*?\\*/"), " ");
    return std::regex_replace(source, std::regex("//[^\\n]*"), "");
}

/// GLSL text of a shader file with comments removed.
std::string glslWithoutComments(const std::string& relativePath) {
    const auto text = rv::core::readTextFile(shaderRoot() / rv::core::pathFromUtf8(relativePath));
    REQUIRE(text.has_value());
    return withoutComments(*text);
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
    // The programs of the application: pbr (PBR_Base), unlit_emissive (Unlit_Emissive) and the depth-only depth and
    // depth_masked (the shadow map, the depth pre-pass), which all share pbr.vert, debug_lines (--debug-draw), and the
    // fullscreen passes on post/fullscreen.vert: the ambient occlusion (ssao, ssao_blur) and the post-processing,
    // bloom_downsample (also with RV_BLOOM_KARIS for the first level), bloom_upsample and post.
    CHECK(stages == std::vector<std::string>{"debug/debug_lines.frag", "debug/debug_lines.vert", "depth/depth.frag",
                                             "pbr/pbr.frag", "pbr/pbr.vert", "post/bloom_downsample.frag",
                                             "post/bloom_upsample.frag", "post/fullscreen.vert", "post/post.frag",
                                             "ssao/ssao.frag", "ssao/ssao_blur.frag", "unlit/unlit_emissive.frag"});

    // The first bloom level is built with RV_BLOOM_KARIS, the depth program of alpha-masked batches with RV_ALPHA_TEST;
    // the self-checks use the programs of the views.
    const std::vector<std::string> karisDefines{"RV_BLOOM_KARIS"};
    const std::vector<std::string> alphaTestDefines{"RV_ALPHA_TEST"};
    for (const std::string& stage : stages) {
        for (const std::vector<std::string>& defines : {std::vector<std::string>{}, karisDefines, alphaTestDefines}) {
            INFO(stage << " with " << defines.size() << " define(s)");
            const auto source = loader.load(stage, defines);
            REQUIRE(source.has_value());
            CHECK(source->text.starts_with("#version 450 core"));
            // Every stage shares the frame block through the common include.
            CHECK(std::find(source->files.begin(), source->files.end(), fs::path("common/common.glsl")) !=
                  source->files.end());
        }
    }
    // The blocks and helpers each stage reads: the material shaders their material (and the lit one its lights), the
    // post-processing passes the post block, and only the post pass the display transform.
    const auto includes = [&loader](const char* stage, const char* include,
                                    const std::vector<std::string>& defines = {}) {
        const auto source = loader.load(stage, defines);
        REQUIRE(source.has_value());
        return std::find(source->files.begin(), source->files.end(), fs::path(include)) != source->files.end();
    };
    CHECK(includes("pbr/pbr.frag", "common/lights.glsl"));
    // The lit shader reads the key light's shadow and the ambient occlusion; the ambient occlusion passes their block.
    CHECK(includes("pbr/pbr.frag", "common/shadow.glsl"));
    CHECK(includes("pbr/pbr.frag", "common/occlusion.glsl"));
    for (const char* stage : {"ssao/ssao.frag", "ssao/ssao_blur.frag"}) {
        INFO(stage);
        CHECK(includes(stage, "common/ssao.glsl"));
        CHECK_FALSE(includes(stage, "common/tonemap.glsl"));
    }
    // The depth stage reads its material for the alpha test of alpha-masked batches.
    CHECK(includes("depth/depth.frag", "common/material.glsl", alphaTestDefines));
    for (const char* stage : {"pbr/pbr.frag", "unlit/unlit_emissive.frag"}) {
        INFO(stage);
        CHECK(includes(stage, "common/material.glsl"));
        CHECK(includes(stage, "common/scene_output.glsl"));
    }
    for (const char* stage : {"post/bloom_downsample.frag", "post/bloom_upsample.frag", "post/post.frag"}) {
        INFO(stage);
        CHECK(includes(stage, "common/post.glsl"));
    }
    CHECK(includes("post/post.frag", "common/tonemap.glsl"));
    for (const char* stage : {"pbr/pbr.frag", "unlit/unlit_emissive.frag", "post/bloom_downsample.frag",
                              "post/bloom_upsample.frag", "debug/debug_lines.frag"}) {
        INFO(stage);
        CHECK_FALSE(includes(stage, "common/tonemap.glsl"));
    }
}

TEST_CASE("integration/shaders: the material shaders write linear radiance for the post pass",
          "[integration][shader]") {
    // Both outputs at the locations of rv::gl::SceneOutput, the attachments of the HDR target: the radiance, and the
    // emitted part of it that the bloom always spreads.
    const std::string outputs = glslWithoutComments("common/scene_output.glsl");
    std::map<std::string, std::uint32_t> locations;
    const std::regex output(R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*out\s+vec4\s+(\w+)\s*;)");
    for (auto it = std::sregex_iterator(outputs.begin(), outputs.end(), output); it != std::sregex_iterator(); ++it) {
        locations[(*it)[2].str()] = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
    }
    using rv::gl::SceneOutput;
    CHECK(locations == std::map<std::string, std::uint32_t>{{"oColor", rv::gl::toIndex(SceneOutput::Radiance)},
                                                            {"oEmission", rv::gl::toIndex(SceneOutput::Emission)}});
    CHECK(locations.size() == rv::gl::kSceneOutputCount);

    // Exposure and tonemapping happen once, in the post pass: a material shader that applied them again would darken
    // and flatten its surfaces, and the bloom would see display values instead of radiance.
    const rv::assets::ShaderSourceLoader loader(shaderRoot());
    for (const char* stage : {"pbr/pbr.frag", "unlit/unlit_emissive.frag"}) {
        INFO(stage);
        const std::string own = glslWithoutComments(stage);
        // The outputs are the shared ones, written together.
        CHECK_FALSE(std::regex_search(own, std::regex(R"(\bout\s+vec4\b)")));
        CHECK(std::regex_search(own, std::regex(R"(\bwriteSceneOutput\s*\()")));
        const auto loaded = loader.load(stage);
        REQUIRE(loaded.has_value());
        const std::string source = withoutComments(loaded->text);
        CHECK_FALSE(std::regex_search(source, std::regex(R"(\btonemap\w*\s*\()")));
        CHECK_FALSE(std::regex_search(source, std::regex(R"(\buExposure\b)")));
    }
    // The emitted part: the emissive term of a lit material, all of an unlit one's radiance.
    CHECK(std::regex_search(glslWithoutComments("pbr/pbr.frag"),
                            std::regex(R"(writeSceneOutput\s*\(\s*radiance\s*,\s*emissive\s*,)")));
    CHECK(std::regex_search(glslWithoutComments("unlit/unlit_emissive.frag"),
                            std::regex(R"(writeSceneOutput\s*\(\s*radiance\s*,\s*radiance\s*,)")));
}

TEST_CASE("integration/shaders: mesh and debug line stages read the vertex layouts the renderer uploads",
          "[integration][shader]") {
    // Vertex inputs `layout(location = N) in <type> <name>;` of a stage.
    const auto inputsOf = [](const std::string& source) {
        std::map<int, std::string> inputs;
        const std::regex input(R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(\w+)\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(source.begin(), source.end(), input); it != std::sregex_iterator(); ++it) {
            inputs[std::stoi((*it)[1].str())] = (*it)[2].str() + " " + (*it)[3].str();
        }
        return inputs;
    };

    // Meshes: the standard 48-byte vertex at locations 0..3 (rv::geometry::VertexLayout), then the per-instance model
    // matrix, a mat4 whose columns take locations 4..7 (rv::gl::GpuMesh::kInstanceMatrixLocation). No draw sets a
    // model matrix uniform any more: every mesh is drawn instanced.
    const std::string mesh = glslWithoutComments("pbr/pbr.vert");
    const std::map<int, std::string> meshInputs = inputsOf(mesh);
    CHECK(meshInputs ==
          std::map<int, std::string>{
              {0, "vec3 aPosition"}, {1, "vec3 aNormal"}, {2, "vec4 aTangent"}, {3, "vec2 aUv0"}, {4, "mat4 aModel"}});
    const rv::geometry::VertexLayout& layout = rv::geometry::VertexLayout::standard();
    REQUIRE(layout.attributes.size() == 4);
    for (const rv::geometry::VertexAttribute& attribute : layout.attributes) {
        CHECK(meshInputs.contains(static_cast<int>(attribute.location)));
    }
    CHECK(mesh.find("uModel") == std::string::npos);

    // Debug lines: rv::scene::DebugVertex, a position (3 floats) then a linear RGBA color (4 floats).
    CHECK(inputsOf(glslWithoutComments("debug/debug_lines.vert")) ==
          std::map<int, std::string>{{0, "vec3 aPosition"}, {1, "vec4 aColor"}});
    CHECK(offsetof(rv::scene::DebugVertex, position) == 0);
    CHECK(offsetof(rv::scene::DebugVertex, color) == 12);
    CHECK(sizeof(rv::scene::DebugVertex) == 28);
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

namespace {

/// Value of `const float <name> = <literal>;` in a comment-free GLSL source.
float glslFloatConstant(const std::string& source, const std::string& name) {
    std::smatch match;
    REQUIRE(std::regex_search(source, match, std::regex(R"(const\s+float\s+)" + name + R"(\s*=\s*(-?\d+\.\d+)\s*;)")));
    return std::stof(match[1].str());
}

/// Texture units of `layout(binding = N) uniform sampler2D <name>;` (or sampler2DShadow) declarations: name -> unit.
std::map<std::string, std::uint32_t> samplerUnits(const std::string& source) {
    std::map<std::string, std::uint32_t> units;
    const std::regex sampler(
        R"(layout\s*\(\s*binding\s*=\s*(\d+)\s*\)\s*uniform\s+sampler2D(?:Shadow)?\s+(\w+)\s*;)");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), sampler); it != std::sregex_iterator(); ++it) {
        units[(*it)[2].str()] = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
    }
    return units;
}

} // namespace

TEST_CASE("integration/shaders: the GLSL post block, texture units and filters match their C++ mirrors",
          "[integration][shader]") {
    const std::string source = glslWithoutComments("common/post.glsl");
    const auto defines = parseDefines(source);
    int binding = -1;
    const std::vector<Member> members = parseBlock(source, "PostUniforms", binding, defines);
    CHECK(binding == static_cast<int>(rv::gl::toIndex(rv::gl::UniformBinding::Post)));
    using rv::gl::PostUniforms;
    checkLayout(members, {},
                {
                    {"float", "uExposure", offsetof(PostUniforms, exposure)},
                    {"float", "uBloomMix", offsetof(PostUniforms, bloomMix)},
                    {"float", "uBloomThreshold", offsetof(PostUniforms, bloomThreshold)},
                    {"float", "uBloomKnee", offsetof(PostUniforms, bloomKnee)},
                    {"float", "uVignette", offsetof(PostUniforms, vignette)},
                    {"float", "uDitherAmplitude", offsetof(PostUniforms, ditherAmplitude)},
                },
                sizeof(PostUniforms));
    CHECK(rv::gl::toIndex(rv::gl::UniformBinding::Shadow) == rv::gl::toIndex(rv::gl::UniformBinding::Post) + 1);

    // Every pass samples its source on PostTextureUnit::Source (and may read the emitted light of the scene); only the
    // post pass reads the bloom, so the bloom passes, which draw into the bloom levels, never sample one of them by
    // accident.
    using rv::gl::PostTextureUnit;
    CHECK(samplerUnits(source) ==
          std::map<std::string, std::uint32_t>{{"uSource", rv::gl::toIndex(PostTextureUnit::Source)},
                                               {"uEmission", rv::gl::toIndex(PostTextureUnit::Emission)}});
    CHECK(samplerUnits(glslWithoutComments("post/post.frag")) ==
          std::map<std::string, std::uint32_t>{{"uBloom", rv::gl::toIndex(PostTextureUnit::Bloom)}});
    for (const char* stage : {"post/bloom_downsample.frag", "post/bloom_upsample.frag"}) {
        INFO(stage);
        CHECK(samplerUnits(glslWithoutComments(stage)).empty());
    }
    // The material units come first.
    CHECK(rv::gl::toIndex(PostTextureUnit::Source) >= rv::gl::kMaterialTextureSlotCount);

    // Energy-conserving filters: the 13-tap downsample (a central box and four corner boxes) and the 3 x 3 tent.
    const float center = glslFloatConstant(source, "RV_BLOOM_CENTER_BOX_WEIGHT");
    const float corner = glslFloatConstant(source, "RV_BLOOM_CORNER_BOX_WEIGHT");
    CHECK(center + 4.0f * corner == 1.0f);
    const float tentCenter = glslFloatConstant(source, "RV_TENT_CENTER_WEIGHT");
    const float tentEdge = glslFloatConstant(source, "RV_TENT_EDGE_WEIGHT");
    const float tentCorner = glslFloatConstant(source, "RV_TENT_CORNER_WEIGHT");
    CHECK(tentCenter + 4.0f * tentEdge + 4.0f * tentCorner == 1.0f);
    // The HDR targets are half floats: radiance is limited where the CPU mirror of the tonemap limits it.
    CHECK(glslFloatConstant(source, "RV_MAX_RADIANCE") == rv::gl::kTonemapInputLimit);
}

TEST_CASE("integration/shaders: the first bloom level takes the Karis average of every one of its 13 taps",
          "[integration][shader]") {
    // Weighting whole boxes would let a lone firefly through: it falls into all five boxes of the texel that covers it,
    // so their weights cancel out. Each tap gets its own Karis weight, times its share of the 13-tap filter.
    const std::string post = glslWithoutComments("common/post.glsl");
    const float center = glslFloatConstant(post, "RV_BLOOM_CENTER_BOX_WEIGHT");
    const float corner = glslFloatConstant(post, "RV_BLOOM_CORNER_BOX_WEIGHT");
    CHECK(std::regex_search(post, std::regex(R"(float\s+karisWeight\s*\([^)]*\)\s*\{\s*return\s+1\.0\s*/\s*\()"
                                             R"(\s*1\.0\s*\+\s*luminance\s*\(\s*\w+\s*\)\s*\*\s*uExposure\s*\)\s*;)")));

    const std::string downsample = glslWithoutComments("post/bloom_downsample.frag");
    const std::regex tap(R"(addKarisTap\s*\(\s*([a-m])\s*,\s*(?:(\d+\.\d+)\s*\*\s*)?(centerBoxTap|cornerBoxTap)\s*,)");
    std::map<char, float> weights;
    for (auto it = std::sregex_iterator(downsample.begin(), downsample.end(), tap); it != std::sregex_iterator();
         ++it) {
        const float factor = (*it)[2].matched ? std::stof((*it)[2].str()) : 1.0f;
        const float boxWeight = (*it)[3].str() == "centerBoxTap" ? center : corner;
        CHECK(weights.emplace((*it)[1].str()[0], factor * boxWeight * 0.25f).second); // each tap once
    }
    REQUIRE(weights.size() == 13);
    // A tap's share: a quarter of the weight of each box it belongs to.
    const std::map<char, int> cornerBoxes{{'a', 1}, {'b', 2}, {'c', 1}, {'d', 2}, {'e', 4},
                                          {'f', 2}, {'g', 1}, {'h', 2}, {'i', 1}};
    float total = 0.0f;
    for (const auto& [name, weight] : weights) {
        INFO(name);
        const auto boxes = cornerBoxes.find(name);
        const float expected = boxes != cornerBoxes.end() ? static_cast<float>(boxes->second) * corner * 0.25f
                                                          : center * 0.25f; // j, k, l, m: the central box
        CHECK(weight == expected);
        total += weight;
    }
    CHECK(total == 1.0f);
    // The bloom source of each tap (the emitted light and the highlights), with its Karis weight.
    CHECK(std::regex_search(downsample, std::regex(R"(bloomSource\s*\()")));
    CHECK(std::regex_search(downsample, std::regex(R"(karisWeight\s*\(\s*source\s*\))")));
}

TEST_CASE("integration/shaders: the post-processing passes draw a fullscreen triangle without vertex buffers",
          "[integration][shader]") {
    // PostProcessor binds an empty vertex array and draws three vertices: the vertex stage reads no attribute.
    const std::string vertex = glslWithoutComments("post/fullscreen.vert");
    CHECK_FALSE(std::regex_search(vertex, std::regex(R"(\bin\s+\w+\s+\w+\s*;)")));
    CHECK(std::regex_search(vertex, std::regex(R"(\bgl_VertexID\b)")));
    // The fragment stages read the uv it writes at location 0.
    CHECK(std::regex_search(vertex, std::regex(R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*out\s+vec2\s+vUv\s*;)")));
    for (const char* stage : {"post/bloom_downsample.frag", "post/bloom_upsample.frag", "post/post.frag"}) {
        INFO(stage);
        CHECK(std::regex_search(glslWithoutComments(stage),
                                std::regex(R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*in\s+vec2\s+vUv\s*;)")));
    }
}

namespace {

/// Decimal literals of `text` ("0.0245786", "-0.53108"), in order.
std::vector<float> floatsIn(const std::string& text) {
    std::vector<float> values;
    const std::regex number(R"(-?\d+\.\d+)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), number); it != std::sregex_iterator(); ++it) {
        values.push_back(std::stof(it->str()));
    }
    return values;
}

/// Function definitions at file scope of a comment-free GLSL source: name -> body (between its braces).
std::map<std::string, std::string> functionBodies(const std::string& source) {
    std::map<std::string, std::string> bodies;
    const std::regex header(R"(\b\w+\s+(\w+)\s*\([^;{}]*\)\s*\{)");
    int depth = 0;
    std::size_t scanned = 0;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), header); it != std::sregex_iterator(); ++it) {
        const auto start = static_cast<std::size_t>(it->position(0));
        for (; scanned < start; ++scanned) {
            depth += source[scanned] == '{' ? 1 : (source[scanned] == '}' ? -1 : 0);
        }
        if (depth != 0) {
            continue; // a statement such as `if (...) {` inside a function
        }
        const std::size_t open = start + static_cast<std::size_t>(it->length(0)) - 1;
        int nested = 0;
        std::size_t close = open;
        for (; close < source.size(); ++close) {
            nested += source[close] == '{' ? 1 : (source[close] == '}' ? -1 : 0);
            if (nested == 0) {
                break;
            }
        }
        REQUIRE(close < source.size());
        bodies[(*it)[1].str()] = source.substr(open + 1, close - open - 1);
    }
    return bodies;
}

/// A call of the function `name`.
std::regex callOf(const std::string& name) {
    return std::regex(R"(\b)" + name + R"(\s*\()");
}

/// Functions whose body matches `direct`, or calls one that does (transitively).
std::set<std::string> functionsReaching(const std::map<std::string, std::string>& bodies, const std::regex& direct) {
    std::set<std::string> reaching;
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [name, body] : bodies) {
            if (reaching.contains(name)) {
                continue;
            }
            bool reaches = std::regex_search(body, direct);
            for (const std::string& callee : reaching) {
                reaches = reaches || std::regex_search(body, callOf(callee));
            }
            if (reaches) {
                reaching.insert(name);
                changed = true;
            }
        }
    }
    return reaching;
}

/// Position of the first match in `text` of `direct` or of a call to one of `functions`, or npos.
std::size_t firstUse(const std::string& text, const std::regex& direct, const std::set<std::string>& functions) {
    std::size_t first = std::string::npos;
    std::smatch match;
    if (std::regex_search(text, match, direct)) {
        first = static_cast<std::size_t>(match.position(0));
    }
    for (const std::string& function : functions) {
        if (std::regex_search(text, match, callOf(function))) {
            first = std::min(first, static_cast<std::size_t>(match.position(0)));
        }
    }
    return first;
}

} // namespace

TEST_CASE("integration/shaders: the display transform of tonemap.glsl matches its CPU mirror rv::gl::tonemap",
          "[integration][shader]") {
    const std::string source = glslWithoutComments("common/tonemap.glsl");
    const auto matrix = [&source](const std::string& name) {
        std::smatch match;
        REQUIRE(std::regex_search(source, match,
                                  std::regex(R"(const\s+mat3\s+)" + name + R"(\s*=\s*mat3\s*\(([^)]*)\)\s*;)")));
        return floatsIn(match[1].str());
    };
    const auto checkValues = [](const std::vector<float>& glsl, const std::vector<float>& cpp) {
        REQUIRE(glsl.size() == cpp.size());
        for (std::size_t i = 0; i < glsl.size(); ++i) {
            INFO("value " << i);
            CHECK(glsl[i] == cpp[i]);
        }
    };
    checkValues(matrix("RV_ACES_INPUT"),
                std::vector<float>(rv::gl::kAcesInputColumns.begin(), rv::gl::kAcesInputColumns.end()));
    checkValues(matrix("RV_ACES_OUTPUT"),
                std::vector<float>(rv::gl::kAcesOutputColumns.begin(), rv::gl::kAcesOutputColumns.end()));

    const auto bodies = functionBodies(source);
    REQUIRE(bodies.contains("acesRrtOdtFit"));
    REQUIRE(bodies.contains("tonemap"));
    // (v * (v + a) - b) / (v * (c * v + d) + e), constants in that order.
    const rv::gl::AcesCurveFit& fit = rv::gl::kAcesCurveFit;
    checkValues(floatsIn(bodies.at("acesRrtOdtFit")), {fit.a, fit.b, fit.c, fit.d, fit.e});
    // The exposed radiance is clamped at 0 (NaN included) and limited before the curve.
    const std::string& tonemap = bodies.at("tonemap");
    const std::regex clampedAtZero(R"(max\s*\(\s*radiance\s*\*\s*exposure\s*,\s*vec3\s*\(\s*0\.0\s*\)\s*\))");
    CHECK(std::regex_search(tonemap, clampedAtZero));
    CHECK(std::regex_search(tonemap, std::regex(R"(isnan\s*\(\s*exposed\s*\))")));
    checkValues(floatsIn(tonemap), {0.0f, 0.0f, rv::gl::kTonemapInputLimit});
}

TEST_CASE("integration/shaders: fragment stages sample every texture before the alpha test may discard",
          "[integration][shader]") {
    // After a discard that only some fragments of a 2x2 quad take, implicit derivatives are undefined (GLSL 4.50
    // §6.4): a texture() after it could pick a wrong mip level along cut-out edges. Lookups with an explicit level of
    // detail or none (textureLod: the shadow map; texelFetch: the ambient occlusion) need no derivatives and may
    // follow.
    const rv::assets::ShaderSourceLoader loader(shaderRoot());
    const std::regex sampling(R"(\btexture(?:Offset|Proj|ProjOffset)?\s*\()");
    const std::regex discarding(R"(\bdiscard\b)");
    const std::pair<const char*, std::vector<std::string>> stages[] = {
        {"pbr/pbr.frag", {}}, {"unlit/unlit_emissive.frag", {}}, {"depth/depth.frag", {"RV_ALPHA_TEST"}}};
    for (const auto& [stage, defines] : stages) {
        INFO(stage);
        const auto loaded = loader.load(stage, defines);
        REQUIRE(loaded.has_value());
        const auto bodies = functionBodies(withoutComments(loaded->text));
        REQUIRE(bodies.contains("main"));
        const std::set<std::string> samplers = functionsReaching(bodies, sampling);
        const std::set<std::string> discarders = functionsReaching(bodies, discarding);
        CHECK(discarders.contains("materialAlphaTest"));

        // main honors the alpha mode, and samples nothing after the first point where it may discard.
        const std::string& main = bodies.at("main");
        const std::size_t discardAt = firstUse(main, discarding, discarders);
        REQUIRE(discardAt != std::string::npos);
        CHECK(firstUse(main, sampling, samplers) < discardAt);
        CHECK(firstUse(main.substr(discardAt), sampling, samplers) == std::string::npos);
    }
}

namespace {

/// Value of `const int <name> = <literal>;` in a comment-free GLSL source.
int glslIntConstant(const std::string& source, const std::string& name) {
    std::smatch match;
    REQUIRE(std::regex_search(source, match, std::regex(R"(const\s+int\s+)" + name + R"(\s*=\s*(-?\d+)\s*;)")));
    return std::stoi(match[1].str());
}

/// The statements (the text between semicolons) of `body` that contain `needle`.
std::vector<std::string> statementsWith(const std::string& body, const std::string& needle) {
    std::vector<std::string> statements;
    std::size_t start = 0;
    while (start < body.size()) {
        const std::size_t end = std::min(body.find(';', start), body.size());
        const std::string statement = body.substr(start, end - start);
        if (statement.find(needle) != std::string::npos) {
            statements.push_back(statement);
        }
        start = end + 1;
    }
    return statements;
}

} // namespace

TEST_CASE("integration/shaders: the GLSL shadow block, constants and shadow map unit match their C++ mirrors",
          "[integration][shader]") {
    const std::string source = glslWithoutComments("common/shadow.glsl");
    const auto defines = parseDefines(source);
    int binding = -1;
    const std::vector<Member> members = parseBlock(source, "ShadowUniforms", binding, defines);
    CHECK(binding == static_cast<int>(rv::gl::toIndex(rv::gl::UniformBinding::Shadow)));
    using rv::gl::ShadowUniforms;
    checkLayout(members, {},
                {
                    {"mat4", "uShadowViewProjection", offsetof(ShadowUniforms, viewProjection)},
                    {"vec3", "uShadowLightPosition", offsetof(ShadowUniforms, lightPosition)},
                    {"int", "uShadowLightIndex", offsetof(ShadowUniforms, lightIndex)},
                    {"vec2", "uShadowNormalOffset", offsetof(ShadowUniforms, normalOffset)},
                    {"float", "uShadowStrength", offsetof(ShadowUniforms, strength)},
                    {"float", "uShadowTexelSize", offsetof(ShadowUniforms, texelSize)},
                },
                sizeof(ShadowUniforms));
    CHECK(glslIntConstant(source, "RV_NO_SHADOW_LIGHT") == rv::gl::kNoShadowLight);
    CHECK(glslIntConstant(source, "RV_DIRECTIONAL_SHADOW_LIGHT") == rv::gl::kDirectionalShadowLight);
    // The directional light's index follows the punctual lights of the lights block.
    const auto lightDefines = parseDefines(glslWithoutComments("common/lights.glsl"));
    REQUIRE(lightDefines.contains("RV_MAX_PUNCTUAL_LIGHTS"));
    CHECK(static_cast<std::size_t>(glslIntConstant(source, "RV_DIRECTIONAL_SHADOW_LIGHT")) ==
          lightDefines.at("RV_MAX_PUNCTUAL_LIGHTS"));

    // The depth comparison is the hardware's (a shadow sampler) on the scene unit of the shadow map.
    CHECK(samplerUnits(source) == std::map<std::string, std::uint32_t>{
                                      {"uShadowMap", rv::gl::toIndex(rv::gl::SceneTextureUnit::ShadowMap)}});
    CHECK(std::regex_search(source, std::regex(R"(uniform\s+sampler2DShadow\s+uShadowMap\b)")));
    // A 3 x 3 tent (1 2 1) of filtered fetches, whose weights add up to 16, mixed towards 1 by the strength; receivers
    // are moved along their normal before the lookup (normal offset).
    const auto bodies = functionBodies(source);
    REQUIRE(bodies.contains("keyLightVisibility"));
    const std::string& visibility = bodies.at("keyLightVisibility");
    CHECK(std::regex_search(visibility, std::regex(R"(textureLod\s*\(\s*uShadowMap\b)")));
    CHECK(std::regex_search(visibility,
                            std::regex(R"(2\s*-\s*abs\s*\(\s*x\s*\)\s*\)\s*\*\s*\(\s*2\s*-\s*abs\s*\(\s*y)")));
    CHECK(std::regex_search(visibility,
                            std::regex(R"(mix\s*\(\s*1\.0\s*,\s*visibility\s*/\s*16\.0\s*,\s*uShadowStrength\s*\))")));
    CHECK(std::regex_search(visibility, std::regex(R"(worldPosition\s*\+\s*normal\s*\*\s*offset)")));
}

TEST_CASE("integration/shaders: the GLSL ambient occlusion block and texture units match their C++ mirrors",
          "[integration][shader]") {
    const std::string source = glslWithoutComments("common/ssao.glsl");
    const auto defines = parseDefines(source);
    REQUIRE(defines.contains("RV_SSAO_SAMPLE_COUNT"));
    CHECK(defines.at("RV_SSAO_SAMPLE_COUNT") == rv::gl::kSsaoSampleCount);
    int binding = -1;
    const std::vector<Member> members = parseBlock(source, "SsaoUniforms", binding, defines);
    CHECK(binding == static_cast<int>(rv::gl::toIndex(rv::gl::UniformBinding::Ssao)));
    using rv::gl::SsaoUniforms;
    checkLayout(members, {},
                {
                    {"vec4", "uSsaoKernel", offsetof(SsaoUniforms, kernel), rv::gl::kSsaoSampleCount},
                    {"float", "uSsaoRadius", offsetof(SsaoUniforms, radius)},
                    {"float", "uSsaoIntensity", offsetof(SsaoUniforms, intensity)},
                    {"float", "uSsaoBias", offsetof(SsaoUniforms, bias)},
                    {"float", "uSsaoDepthTolerance", offsetof(SsaoUniforms, depthTolerance)},
                },
                sizeof(SsaoUniforms));
    CHECK(rv::gl::kFirstReservedUniformBinding == rv::gl::toIndex(rv::gl::UniformBinding::Ssao) + 1);

    using rv::gl::SsaoTextureUnit;
    CHECK(samplerUnits(source) ==
          std::map<std::string, std::uint32_t>{{"uSceneDepth", rv::gl::toIndex(SsaoTextureUnit::SceneDepth)},
                                               {"uRawOcclusion", rv::gl::toIndex(SsaoTextureUnit::RawOcclusion)}});
    // The lit shader reads the result on a unit of its own.
    CHECK(samplerUnits(glslWithoutComments("common/occlusion.glsl")) ==
          std::map<std::string, std::uint32_t>{
              {"uAmbientOcclusion", rv::gl::toIndex(rv::gl::SceneTextureUnit::AmbientOcclusion)}});

    // Both passes draw the fullscreen triangle of post/fullscreen.vert, read nothing from it and write one output; the
    // estimate places its samples with the camera's projection.
    for (const char* stage : {"ssao/ssao.frag", "ssao/ssao_blur.frag"}) {
        INFO(stage);
        const std::string own = glslWithoutComments(stage);
        CHECK(std::regex_search(own,
                                std::regex(R"(layout\s*\(\s*location\s*=\s*0\s*\)\s*out\s+vec4\s+oOcclusion\s*;)")));
        CHECK_FALSE(std::regex_search(own, std::regex(R"(\bin\s+\w+\s+\w+\s*;)")));
    }
    CHECK(std::regex_search(glslWithoutComments("ssao/ssao.frag"), std::regex(R"(uProjection\s*\*)")));
}

TEST_CASE("integration/shaders: the mesh programs share an invariant vertex stage", "[integration][shader]") {
    // The depth pre-pass and the lit pass draw with different programs over the same vertex stage: an invariant
    // gl_Position gives both the same depth, so the lit pass passes GL_LEQUAL exactly where the pre-pass drew.
    CHECK(std::regex_search(glslWithoutComments("pbr/pbr.vert"), std::regex(R"(\binvariant\s+gl_Position\s*;)")));
    // The depth stage writes nothing but depth, and discards like the lit pass when it alpha-tests.
    const std::string depth = glslWithoutComments("depth/depth.frag");
    CHECK_FALSE(std::regex_search(depth, std::regex(R"(\bout\s+\w+\s+\w+\s*;)")));
    CHECK_FALSE(std::regex_search(depth, std::regex(R"(\bgl_FragDepth\b)")));
    // Opaque batches draw their depth without a material bound: main reads one (the alpha test) only with
    // RV_ALPHA_TEST, and calls nothing otherwise.
    const auto bodies = functionBodies(depth);
    REQUIRE(bodies.contains("main"));
    std::smatch alphaTest;
    REQUIRE(std::regex_search(bodies.at("main"), alphaTest,
                              std::regex(R"(#if\s+defined\s*\(\s*RV_ALPHA_TEST\s*\)([\s\S]*?)#endif)")));
    CHECK(std::regex_search(alphaTest[1].str(), std::regex(R"(materialAlphaTest\s*\()")));
    CHECK_FALSE(std::regex_search(alphaTest.prefix().str() + alphaTest.suffix().str(), std::regex(R"(\w+\s*\()")));
}

TEST_CASE("integration/shaders: the lit shader shadows the key light and occludes the ambient light only",
          "[integration][shader]") {
    const auto bodies = functionBodies(glslWithoutComments("pbr/pbr.frag"));
    REQUIRE(bodies.contains("main"));
    const std::string& main = bodies.at("main");
    // The shadow darkens the light the block names: the punctual light with its index, or the directional light.
    CHECK(std::regex_search(main, std::regex(R"(i\s*==\s*uShadowLightIndex)")));
    CHECK(std::regex_search(main, std::regex(R"(uShadowLightIndex\s*==\s*RV_DIRECTIONAL_SHADOW_LIGHT)")));
    // The ambient occlusion is read once and reaches the two ambient terms, never a light's.
    CHECK(std::regex_search(main, std::regex(R"(ambientVisibility\s*=[^;]*screenAmbientOcclusion\s*\(\s*\))")));
    CHECK(statementsWith(main, "ambientVisibility").size() == 3); // read, then the diffuse and specular ambient
    const std::vector<std::string> ambient = statementsWith(main, "hemisphereAmbient");
    CHECK(ambient.size() == 2);
    for (const std::string& statement : ambient) {
        INFO(statement);
        CHECK(statement.find("ambientVisibility") != std::string::npos);
    }
    for (const std::string& statement : statementsWith(main, "evaluateLight")) {
        INFO(statement);
        CHECK(statement.find("ambientVisibility") == std::string::npos);
    }
    // The specular ambient is occluded through the specular occlusion of the ambient occlusion.
    CHECK(std::regex_search(main, std::regex(R"(specularOcclusion\s*\(\s*NoV\s*,\s*ambientVisibility\s*,)")));
}
