#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::assets {

/// A preprocessed GLSL source ready for glShaderSource, plus the files it was assembled from.
struct ShaderSource {
    /// Complete GLSL text: the root file with defines injected after #version and every #include expanded.
    std::string text;
    /// Files that make up `text`, relative to the shader root with forward slashes ("pbr/pbr_base.frag",
    /// "common/brdf.glsl"). The index is the GLSL source-string number used in the emitted `#line` directives;
    /// files[0] is the root file. The renderer's hot reload watches root / files[i].
    std::vector<std::filesystem::path> files;
};

/// GPU-free GLSL preprocessing for the renderer and its hot reload (M0 step 4).
///
/// Rules:
/// - The root file must start with a `#version` line; only comments and blank lines may precede it.
///   `#version` anywhere else (later in the root file or in an included file) is an error.
/// - Defines are injected right after the `#version` line: "NAME" becomes `#define NAME`, "NAME=VALUE"
///   becomes `#define NAME VALUE`. Names must be identifiers and must not be reserved (`GL_` prefix, `__`).
/// - `#include "path"` resolves relative to the shader root (not to the including file), with forward slashes;
///   ".." and absolute paths are rejected. Includes nest. Each file is included at most once per load (implicit
///   include guard: later includes of the same path expand to an empty line); an include cycle is an error
///   naming the chain. A missing include is an error with "<including file>:<line>" context.
/// - After every include boundary a `#line <n> <fileIndex>` directive is emitted (GLSL 3.30+ semantics: the
///   next line is line n), so compiler messages map back to the right file and line (see mapCompilerLog).
/// - Directives inside block or line comments are ignored. Includes are expanded regardless of `#if`
///   conditions. CRLF line endings and a UTF-8 byte-order mark are accepted; the output uses LF.
class ShaderSourceLoader {
public:
    explicit ShaderSourceLoader(std::filesystem::path shaderRoot);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    /// Loads `relativePath` (relative to the shader root, forward slashes) and expands it. Errors: NotFound
    /// (root file or include missing), InvalidArgument (bad path or define), ParseError (missing #version,
    /// malformed #include, include cycle, unterminated block comment in an include), OutOfRange (file too large,
    /// include nesting deeper than 32).
    [[nodiscard]] Expected<ShaderSource> load(std::string_view relativePath,
                                              std::span<const std::string> defines = {}) const;

private:
    std::filesystem::path m_root;
};

/// Rewrites driver compile logs so locations name the source file instead of the source-string number:
///   NVIDIA            "0(12) : error C0000: ..."   -> "<files[0]>:12: error C0000: ..."
///   AMD / Intel       "ERROR: 1:7: 'x' : ..."      -> "<files[1]>:7: error: 'x' : ..."   (WARNING -> warning)
///   Mesa              "0:12(5): error: ..."        -> "<files[0]>:12:5: error: ..."
/// Lines in other formats, or naming a source string that `source` does not have, are kept unchanged.
[[nodiscard]] std::string mapCompilerLog(std::string_view log, const ShaderSource& source);

} // namespace rv::assets
