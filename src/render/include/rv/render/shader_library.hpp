#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/core/file_watch_set.hpp>
#include <rv/render/shader_program.hpp>

namespace rv::gl {

/// Source files and preprocessor defines of one program. Paths are relative to the shader root, with forward
/// slashes (UTF-8).
struct ShaderDesc {
    /// Unique within the library; names the program in logs and GL debug labels.
    std::string name;
    std::string vertexPath;   ///< e.g. "unlit/unlit.vert"
    std::string fragmentPath; ///< e.g. "unlit/unlit.frag"
    /// "NAME" or "NAME=VALUE", injected after #version in both stages (rv::assets::ShaderSourceLoader).
    std::vector<std::string> defines;
};

/// Program of a ShaderLibrary, returned by ShaderLibrary::load(). A default handle names no program.
struct ShaderHandle {
    std::uint32_t value = 0;

    auto operator<=>(const ShaderHandle&) const = default;
};

/// Outcome of a reload pass.
struct ShaderReloadSummary {
    std::size_t rebuilt = 0; ///< programs rebuilt and swapped in
    std::size_t failed = 0;  ///< programs whose rebuild failed; they keep their previous version
};

/// GLSL programs built from files under a shader root (<asset root>/shaders): sources are assembled by
/// rv::assets::ShaderSourceLoader (#include relative to the root, injected defines, #line directives) and compiled with
/// ShaderProgram, whose compiler messages name "<file>:<line>" (rv::assets::mapCompilerLog).
///
/// reloadAll() (F5) rebuilds every program; pollHotReload() checks the modification times of every file involved
/// (root files and includes) at most every 500 ms and rebuilds only the programs that use a changed file. A failed
/// build is logged with the mapped messages and leaves the previous working program in place, so rendering goes on;
/// a program that never built is empty (ShaderProgram::valid() is false) until a later reload succeeds. Rebuilding
/// replaces the GL program: uniform values set on the old one are lost, so callers set their uniforms before each
/// draw. Main thread only; every call, destruction included, needs the GL context current.
class ShaderLibrary {
public:
    /// `shaderRoot` is usually <asset root>/shaders. Nothing is read until load().
    explicit ShaderLibrary(std::filesystem::path shaderRoot);

    /// Registers `desc` and builds it. Errors only for an invalid descriptor (InvalidArgument: empty name or path,
    /// name already registered). A failed build is logged and still returns a handle, whose program stays empty until
    /// a reload succeeds, so that fixing the file brings the program in without a restart.
    [[nodiscard]] Expected<ShaderHandle> load(const ShaderDesc& desc);

    /// Current program of `handle`: the last successful build, or an empty program while it never built. Handles that
    /// this library did not return also give an empty program. The reference stays valid until the next load() and
    /// shows the new program after a rebuild.
    [[nodiscard]] const ShaderProgram& get(ShaderHandle handle) const noexcept;

    /// Rebuilds every program from its files (F5) and logs the outcome.
    ShaderReloadSummary reloadAll();
    /// Rebuilds the programs whose files changed since the previous check. Checks the file system at most every
    /// 500 ms (core::FileWatchSet); cheap to call every frame.
    ShaderReloadSummary pollHotReload();

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

private:
    struct Entry {
        ShaderDesc desc;
        ShaderProgram program;
        /// Absolute, lexically normal paths of the files the stages were last assembled from (root file first).
        std::vector<std::filesystem::path> vertexFiles;
        std::vector<std::filesystem::path> fragmentFiles;
    };

    /// Assembles and builds `entry`; swaps the program in on success, logs and keeps the previous one on failure.
    bool rebuild(Entry& entry, bool reload);
    /// Watches exactly the files of every entry.
    void updateWatchedFiles();

    std::filesystem::path m_root;
    std::vector<Entry> m_entries;
    core::FileWatchSet m_watch;
    ShaderProgram m_empty;
};

} // namespace rv::gl
