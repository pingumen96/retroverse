#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/assets/shader_source.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

using rv::ErrorCode;
using rv::assets::ShaderSource;
using rv::assets::ShaderSourceLoader;

namespace {

struct ShaderTree {
    rv::test::TempDir dir{"rv-shaders"};

    void write(std::string_view relative, std::string_view text) const {
        REQUIRE(rv::core::writeTextFileAtomic(dir.file(relative), text).has_value());
    }
    [[nodiscard]] ShaderSourceLoader loader() const { return ShaderSourceLoader(dir.path()); }
};

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    for (const std::string_view line : rv::core::split(text, '\n')) {
        result.emplace_back(line);
    }
    if (!result.empty() && result.back().empty()) {
        result.pop_back();
    }
    return result;
}

std::string fileName(const ShaderSource& source, std::size_t index) {
    return rv::core::pathToUtf8(source.files.at(index));
}

/// Replays GLSL #line semantics (3.30+: "#line n s" makes the NEXT line number n of source string s) and returns,
/// for every output line containing `marker`, the (file, line) a compiler would report.
std::optional<std::pair<std::string, int>> locate(const ShaderSource& source, std::string_view marker) {
    int line = 1;
    std::size_t file = 0;
    for (const std::string& text : lines(source.text)) {
        if (text.starts_with("#line ")) {
            const std::size_t space = text.find(' ', 6);
            line = std::stoi(text.substr(6, space - 6));
            file = static_cast<std::size_t>(std::stoul(text.substr(space + 1)));
            continue;
        }
        if (text.find(marker) != std::string::npos) {
            return std::make_pair(fileName(source, file), line);
        }
        ++line;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("assets/shader_source: defines are injected right after #version", "[assets][shader]") {
    ShaderTree tree;
    tree.write("unlit/unlit.frag", "// Unlit shader\n/* multi-line\n   header */\n#version 450 core\nvoid main() {}\n");
    const std::vector<std::string> defines = {"USE_SHADOWS", "LIGHT_COUNT=4", " SPACED = 1.5 "};
    const auto source = tree.loader().load("unlit/unlit.frag", defines);
    REQUIRE(source.has_value());
    CHECK(lines(source->text) == std::vector<std::string>{"// Unlit shader", "/* multi-line", "   header */",
                                                          "#version 450 core", "#define USE_SHADOWS",
                                                          "#define LIGHT_COUNT 4", "#define SPACED 1.5", "#line 5 0",
                                                          "void main() {}"});
    REQUIRE(source->files.size() == 1);
    CHECK(fileName(*source, 0) == "unlit/unlit.frag");

    const auto plain = tree.loader().load("unlit/unlit.frag");
    REQUIRE(plain.has_value());
    CHECK(plain->text.find("#line") == std::string::npos);
    CHECK(plain->text.find("#define") == std::string::npos);
}

TEST_CASE("assets/shader_source: invalid defines are rejected", "[assets][shader]") {
    ShaderTree tree;
    tree.write("a.frag", "#version 450\n");
    for (const std::string define : {"1ABC", "", "=3", "GL_RESERVED", "MY__NAME", "BAD-NAME", "NAME=1\n2"}) {
        CAPTURE(define);
        const std::vector<std::string> defines = {define};
        const auto source = tree.loader().load("a.frag", defines);
        REQUIRE_FALSE(source.has_value());
        CHECK(source.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("assets/shader_source: nested includes with #line directives", "[assets][shader]") {
    ShaderTree tree;
    tree.write("pbr/pbr.frag", "#version 450\n#include \"common/a.glsl\"\nvoid main() {}\n");
    tree.write("common/a.glsl", "// a\n#include \"common/b.glsl\"\nfloat a() { return b(); }\n");
    tree.write("common/b.glsl", "float b() { return 1.0; }\n");
    const auto source = tree.loader().load("pbr/pbr.frag");
    REQUIRE(source.has_value());
    CHECK(lines(source->text) == std::vector<std::string>{"#version 450", "#line 1 1", "// a", "#line 1 2",
                                                          "float b() { return 1.0; }", "#line 3 1",
                                                          "float a() { return b(); }", "#line 3 0", "void main() {}"});
    REQUIRE(source->files.size() == 3);
    CHECK(fileName(*source, 0) == "pbr/pbr.frag");
    CHECK(fileName(*source, 1) == "common/a.glsl");
    CHECK(fileName(*source, 2) == "common/b.glsl");
}

TEST_CASE("assets/shader_source: #line numbers map every line back to its file", "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "// header\n#version 450\n#include \"lib/one.glsl\"\nconst int MAIN_A = 1;\n"
                            "#include \"lib/two.glsl\"\n\nconst int MAIN_B = 2;\n");
    tree.write("lib/one.glsl", "#include \"lib/shared.glsl\"\nconst int ONE_A = 1;\n\nconst int ONE_B = 2;\n");
    tree.write("lib/two.glsl", "// two\n#include \"lib/shared.glsl\"\nconst int TWO_A = 1;\n");
    tree.write("lib/shared.glsl", "\nconst int SHARED_A = 1;\n");
    const std::vector<std::string> defines = {"A", "B=2"};
    const auto source = tree.loader().load("main.frag", defines);
    REQUIRE(source.has_value());

    const std::map<std::string, std::pair<std::string, int>> expected = {
        {"MAIN_A", {"main.frag", 4}},   {"MAIN_B", {"main.frag", 7}},   {"ONE_A", {"lib/one.glsl", 2}},
        {"ONE_B", {"lib/one.glsl", 4}}, {"TWO_A", {"lib/two.glsl", 3}}, {"SHARED_A", {"lib/shared.glsl", 2}},
    };
    for (const auto& [marker, location] : expected) {
        CAPTURE(marker);
        const auto found = locate(*source, marker);
        REQUIRE(found.has_value());
        CHECK(*found == location);
    }
}

TEST_CASE("assets/shader_source: each file is included at most once", "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "#version 450\n#include \"a.glsl\"\n#include \"b.glsl\"\n#include \"a.glsl\"\n"
                            "void main() {}\n");
    tree.write("a.glsl", "#include \"b.glsl\"\nfloat a() { return b(); }\n");
    tree.write("b.glsl", "float b() { return 1.0; }\n");
    const auto source = tree.loader().load("main.frag");
    REQUIRE(source.has_value());
    std::size_t count = 0;
    for (std::size_t pos = source->text.find("float b()"); pos != std::string::npos;
         pos = source->text.find("float b()", pos + 1)) {
        ++count;
    }
    CHECK(count == 1);
    CHECK(source->files.size() == 3);
    CHECK(locate(*source, "void main()") == std::make_pair(std::string("main.frag"), 5));
}

TEST_CASE("assets/shader_source: include cycles are errors naming the chain", "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "#version 450\n#include \"common/a.glsl\"\n");
    tree.write("common/a.glsl", "#include \"common/b.glsl\"\n");
    tree.write("common/b.glsl", "\n#include \"common/a.glsl\"\n");
    const auto source = tree.loader().load("main.frag");
    REQUIRE_FALSE(source.has_value());
    CHECK(source.error().code == ErrorCode::ParseError);
    CHECK(source.error().message.find("main.frag -> common/a.glsl -> common/b.glsl -> common/a.glsl") !=
          std::string::npos);
    CHECK(source.error().context == "common/b.glsl:2");

    tree.write("self.frag", "#version 450\n#include \"self.frag\"\n");
    const auto self = tree.loader().load("self.frag");
    REQUIRE_FALSE(self.has_value());
    CHECK(self.error().message.find("self.frag -> self.frag") != std::string::npos);
}

TEST_CASE("assets/shader_source: a missing include reports the including file and line", "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "#version 450\n\n#include \"common/missing.glsl\"\n");
    const auto source = tree.loader().load("main.frag");
    REQUIRE_FALSE(source.has_value());
    CHECK(source.error().code == ErrorCode::NotFound);
    CHECK(source.error().context == "main.frag:3");
    CHECK(source.error().message.find("common/missing.glsl") != std::string::npos);

    const auto root = tree.loader().load("does/not/exist.frag");
    REQUIRE_FALSE(root.has_value());
    CHECK(root.error().code == ErrorCode::NotFound);
}

TEST_CASE("assets/shader_source: directive errors", "[assets][shader]") {
    ShaderTree tree;
    tree.write("no_version.frag", "void main() {}\n#version 450\n");
    tree.write("empty.frag", "// only a comment\n");
    tree.write("twice.frag", "#version 450\n#version 450\n");
    tree.write("version_in_include.frag", "#version 450\n#include \"inc.glsl\"\n");
    tree.write("inc.glsl", "#version 330\n");
    tree.write("angle.frag", "#version 450\n#include <inc.glsl>\n");
    tree.write("unquoted.frag", "#version 450\n#include inc.glsl\n");
    tree.write("trailing.frag", "#version 450\n#include \"inc.glsl\" extra\n");
    tree.write("escape.frag", "#version 450\n#include \"../outside.glsl\"\n");
    tree.write("absolute.frag", "#version 450\n#include \"/abs.glsl\"\n");
    tree.write("open_comment.frag", "#version 450\n#include \"open.glsl\"\nvoid main() {}\n");
    tree.write("open.glsl", "/* never closed\n");

    const auto check = [&tree](std::string_view file, ErrorCode code, std::string_view context) {
        CAPTURE(file);
        const auto source = tree.loader().load(file);
        REQUIRE_FALSE(source.has_value());
        CHECK(source.error().code == code);
        CHECK(source.error().context == context);
    };
    check("no_version.frag", ErrorCode::ParseError, "no_version.frag:1");
    check("empty.frag", ErrorCode::ParseError, "empty.frag");
    check("twice.frag", ErrorCode::ParseError, "twice.frag:2");
    check("version_in_include.frag", ErrorCode::ParseError, "inc.glsl:1");
    check("angle.frag", ErrorCode::ParseError, "angle.frag:2");
    check("unquoted.frag", ErrorCode::ParseError, "unquoted.frag:2");
    check("trailing.frag", ErrorCode::ParseError, "trailing.frag:2");
    check("escape.frag", ErrorCode::InvalidArgument, "escape.frag:2");
    check("absolute.frag", ErrorCode::InvalidArgument, "absolute.frag:2");
    check("open_comment.frag", ErrorCode::ParseError, "open.glsl");
    CHECK(tree.loader().load("../escape.frag").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("assets/shader_source: block comments must not cross the #version or an #include line",
          "[assets][shader]") {
    ShaderTree tree;
    // The injected defines and #line would land inside the comment opened on the #version line.
    tree.write("version_open.frag", "#version 450 /* note\n   continued */\nvoid main() {}\n");
    // Replacing the include line would drop the "*/" and leave the comment open over the included text.
    tree.write("include_close.frag", "#version 450\n/* start\nend */ #include \"inc.glsl\"\nvoid main() {}\n");
    tree.write("inc.glsl", "float inc() { return 1.0; }\n");
    const std::vector<std::string> defines = {"USE_SHADOWS"};
    for (const auto& [file, context] : {std::pair{"version_open.frag", "version_open.frag:1"},
                                        std::pair{"include_close.frag", "include_close.frag:3"}}) {
        CAPTURE(file);
        const auto source = tree.loader().load(file, defines);
        REQUIRE_FALSE(source.has_value());
        CHECK(source.error().code == ErrorCode::ParseError);
        CHECK(source.error().context == context);
    }

    // Comments closed on the same line, or before the directive, are fine.
    tree.write("closed.frag", "/* header\n*/ #version 450 /* inline */\n#include \"inc.glsl\" /* inline */\n");
    const auto closed = tree.loader().load("closed.frag", defines);
    REQUIRE(closed.has_value());
    CHECK(lines(closed->text) == std::vector<std::string>{"/* header", "*/ #version 450 /* inline */",
                                                          "#define USE_SHADOWS", "#line 3 0", "#line 1 1",
                                                          "float inc() { return 1.0; }", "#line 4 0"});
}

TEST_CASE("assets/shader_source: the include guard ignores letter case like Windows file names",
          "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "#version 450\n#include \"common/brdf.glsl\"\n#include \"common/BRDF.glsl\"\n"
                            "void main() {}\n");
    tree.write("common/brdf.glsl", "float brdf() { return 1.0; }\n");
    const auto source = tree.loader().load("main.frag");
    REQUIRE(source.has_value());
    CHECK(source->text.find("float brdf()") == source->text.rfind("float brdf()")); // expanded once
    REQUIRE(source->files.size() == 2);
    CHECK(fileName(*source, 1) == "common/brdf.glsl");
    CHECK(locate(*source, "void main()") == std::make_pair(std::string("main.frag"), 4));

    // A cycle through another spelling of an open file is reported where it closes.
    tree.write("cycle.frag", "#version 450\n#include \"a.glsl\"\n");
    tree.write("a.glsl", "#include \"A.glsl\"\n");
    const auto cycle = tree.loader().load("cycle.frag");
    REQUIRE_FALSE(cycle.has_value());
    CHECK(cycle.error().code == ErrorCode::ParseError);
    CHECK(cycle.error().context == "a.glsl:1");
    CHECK(cycle.error().message.find("cycle.frag -> a.glsl -> A.glsl") != std::string::npos);
}

TEST_CASE("assets/shader_source: paths that are not valid UTF-8 are errors, not exceptions", "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "#version 450\n#include \"caf\xE9.glsl\"\nvoid main() {}\n"); // Latin-1, not UTF-8
    const auto include = tree.loader().load("main.frag");
    REQUIRE_FALSE(include.has_value());
    CHECK(include.error().code == ErrorCode::InvalidArgument);
    CHECK(include.error().context == "main.frag:2");
    CHECK(include.error().message.find("caf\\xE9.glsl") != std::string::npos); // escaped: the message stays UTF-8

    for (const std::string_view bad : {"caf\xE9.frag", "over\xC0\xAFlong.frag", "surrogate\xED\xA0\x80.frag",
                                       "cut\xE2\x82.frag"}) {
        CAPTURE(bad);
        const auto root = tree.loader().load(bad);
        REQUIRE_FALSE(root.has_value());
        CHECK(root.error().code == ErrorCode::InvalidArgument);
    }

    // A shader root without a UTF-8 form (unpaired UTF-16 surrogate) fails cleanly as well.
    const ShaderSourceLoader broken(tree.dir.path() / std::wstring(L"bad\xD800name"));
    CHECK_FALSE(broken.load("main.frag").has_value());

    // Valid UTF-8 names work.
    tree.write("café.frag", "#version 450\nvoid main() {}\n");
    CHECK(tree.loader().load("café.frag").has_value());
}

TEST_CASE("assets/shader_source: directives inside comments are ignored", "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "#version 450\n// #include \"missing.glsl\"\n/*\n#include \"missing.glsl\"\n*/\n"
                            "  #  include \"real.glsl\" // trailing comment\nvoid main() {}\n");
    tree.write("real.glsl", "float real() { return 2.0; }\n");
    const auto source = tree.loader().load("main.frag");
    REQUIRE(source.has_value());
    CHECK(source->files.size() == 2);
    CHECK(source->text.find("float real()") != std::string::npos);
    CHECK(source->text.find("// #include \"missing.glsl\"") != std::string::npos);
}

TEST_CASE("assets/shader_source: CRLF line endings and a UTF-8 BOM are accepted", "[assets][shader]") {
    ShaderTree tree;
    tree.write("main.frag", "\xEF\xBB\xBF#version 450\r\n#include \"inc.glsl\"\r\nvoid main() {}\r\n");
    tree.write("inc.glsl", "\xEF\xBB\xBF"
                           "float inc() { return 1.0; }\r\n");
    const auto source = tree.loader().load("main.frag");
    REQUIRE(source.has_value());
    CHECK(source->text.find('\r') == std::string::npos);
    CHECK(source->text.find("\xEF\xBB\xBF") == std::string::npos);
    CHECK(lines(source->text) == std::vector<std::string>{"#version 450", "#line 1 1", "float inc() { return 1.0; }",
                                                          "#line 3 0", "void main() {}"});
}

TEST_CASE("assets/shader_source: compiler logs are remapped to file:line", "[assets][shader]") {
    ShaderSource source;
    source.files = {rv::core::pathFromUtf8("pbr/pbr_base.frag"), rv::core::pathFromUtf8("common/brdf.glsl"),
                    rv::core::pathFromUtf8("common/lights.glsl")};

    // NVIDIA
    CHECK(rv::assets::mapCompilerLog("0(12) : error C0000: syntax error, unexpected '}'", source) ==
          "pbr/pbr_base.frag:12: error C0000: syntax error, unexpected '}'");
    CHECK(rv::assets::mapCompilerLog("2(3) : warning C7050: \"x\" might be used before being initialized", source) ==
          "common/lights.glsl:3: warning C7050: \"x\" might be used before being initialized");
    // AMD / Intel
    CHECK(rv::assets::mapCompilerLog("ERROR: 1:7: 'roughness' : undeclared identifier", source) ==
          "common/brdf.glsl:7: error: 'roughness' : undeclared identifier");
    CHECK(rv::assets::mapCompilerLog("WARNING: 0:2: extension not supported", source) ==
          "pbr/pbr_base.frag:2: warning: extension not supported");
    // Mesa
    CHECK(rv::assets::mapCompilerLog("0:12(5): error: `foo' undeclared", source) ==
          "pbr/pbr_base.frag:12:5: error: `foo' undeclared");

    // Multi-line logs keep line structure; unknown formats and unknown source strings are left alone.
    const std::string log = "0(1) : error C1: a\r\nERROR: 2:4: b\nLink failed.\n9(1) : error C2: c\nERROR: 3 errors\n";
    CHECK(rv::assets::mapCompilerLog(log, source) ==
          "pbr/pbr_base.frag:1: error C1: a\r\ncommon/lights.glsl:4: error: b\nLink failed.\n9(1) : error C2: c\n"
          "ERROR: 3 errors\n");
    CHECK(rv::assets::mapCompilerLog("", source).empty());
}

TEST_CASE("assets/shader_source: a loaded source and its compiler log work together", "[assets][shader]") {
    ShaderTree tree;
    tree.write("unlit/unlit_emissive.frag", "#version 450\n#include \"common/common.glsl\"\nout vec4 color;\n"
                                            "void main() { color = vec4(1.0) }\n");
    tree.write("common/common.glsl", "const float PI = 3.14159;\n");
    const auto source = tree.loader().load("unlit/unlit_emissive.frag");
    REQUIRE(source.has_value());
    const auto location = locate(*source, "color = vec4(1.0)");
    REQUIRE(location.has_value());
    CHECK(location->second == 4);
    // A driver reports the error with the source-string number and line it derived from the #line directives.
    CHECK(rv::assets::mapCompilerLog("0(4) : error C0000: syntax error, unexpected '}'", *source) ==
          "unlit/unlit_emissive.frag:4: error C0000: syntax error, unexpected '}'");
}
