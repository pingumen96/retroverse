// Cross-module integration checks (GPU-free) for the shader sources in assets/shaders: every program preprocesses
// with rv::assets::ShaderSourceLoader, and the std140 FrameUniforms block of common.glsl matches its C++ mirror
// rv::gl::FrameUniforms (binding point and member offsets), so a change on one side cannot silently skew the other.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
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

struct BlockMember {
    std::string type;
    std::string name;
};

/// std140 base alignment and size of the scalar, vector and matrix types used in uniform blocks.
struct Std140Type {
    std::size_t alignment;
    std::size_t size;
};

std::optional<Std140Type> std140(const std::string& type) {
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
    return std::nullopt;
}

/// Members of `layout(std140, binding = N) uniform <block> { ... };` in `source`, and N.
std::vector<BlockMember> parseBlock(const std::string& source, const std::string& block, int& binding) {
    const std::regex header("layout\\s*\\(\\s*std140\\s*,\\s*binding\\s*=\\s*(\\d+)\\s*\\)\\s*uniform\\s+" + block +
                            "\\s*\\{([^}]*)\\}");
    std::smatch match;
    REQUIRE(std::regex_search(source, match, header));
    binding = std::stoi(match[1].str());
    std::string body = match[2].str();
    body = std::regex_replace(body, std::regex("//[^\\n]*"), ""); // strip line comments
    std::vector<BlockMember> members;
    const std::regex member("(\\w+)\\s+(\\w+)\\s*;");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), member); it != std::sregex_iterator(); ++it) {
        members.push_back({(*it)[1].str(), (*it)[2].str()});
    }
    return members;
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
    REQUIRE(std::find(stages.begin(), stages.end(), "unlit/unlit.vert") != stages.end());
    REQUIRE(std::find(stages.begin(), stages.end(), "unlit/unlit.frag") != stages.end());

    for (const std::string& stage : stages) {
        INFO(stage);
        const auto source = loader.load(stage);
        REQUIRE(source.has_value());
        CHECK(source->text.starts_with("#version 450 core"));
        // Every stage shares the frame block through the common include.
        CHECK(std::find(source->files.begin(), source->files.end(), fs::path("common/common.glsl")) !=
              source->files.end());
    }
}

TEST_CASE("integration/shaders: the GLSL FrameUniforms block matches rv::gl::FrameUniforms", "[integration][shader]") {
    const auto common = rv::core::readTextFile(shaderRoot() / "common" / "common.glsl");
    REQUIRE(common.has_value());
    int binding = -1;
    const std::vector<BlockMember> members = parseBlock(*common, "FrameUniforms", binding);
    CHECK(binding == static_cast<int>(rv::gl::toIndex(rv::gl::UniformBinding::Frame)));

    struct ExpectedMember {
        const char* type;
        const char* name;
        std::size_t offset;
    };
    const std::vector<ExpectedMember> expected{
        {"mat4", "uView", offsetof(rv::gl::FrameUniforms, view)},
        {"mat4", "uProjection", offsetof(rv::gl::FrameUniforms, projection)},
        {"mat4", "uViewProjection", offsetof(rv::gl::FrameUniforms, viewProjection)},
        {"vec3", "uCameraPosition", offsetof(rv::gl::FrameUniforms, cameraPosition)},
        {"float", "uTime", offsetof(rv::gl::FrameUniforms, time)},
        {"float", "uExposure", offsetof(rv::gl::FrameUniforms, exposure)},
    };
    REQUIRE(members.size() == expected.size());

    std::size_t offset = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        INFO(members[i].type << " " << members[i].name);
        CHECK(members[i].type == expected[i].type);
        CHECK(members[i].name == expected[i].name);
        const auto layout = std140(members[i].type);
        REQUIRE(layout.has_value());
        offset = (offset + layout->alignment - 1) / layout->alignment * layout->alignment;
        CHECK(offset == expected[i].offset);
        offset += layout->size;
    }
    // std140 rounds the block size up to a multiple of 16 bytes; the C++ struct pads to the same size.
    CHECK((offset + 15) / 16 * 16 == sizeof(rv::gl::FrameUniforms));
}
