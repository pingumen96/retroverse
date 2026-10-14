#include <rv/render/shader_library.hpp>

#include <algorithm>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/assets/shader_source.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/text.hpp>

namespace rv::gl {
namespace {

std::filesystem::path absoluteFile(const std::filesystem::path& root, const std::filesystem::path& relative) {
    return (root / relative).lexically_normal();
}

std::vector<std::filesystem::path> absoluteFiles(const std::filesystem::path& root,
                                                 const std::vector<std::filesystem::path>& relativeFiles) {
    std::vector<std::filesystem::path> files;
    files.reserve(relativeFiles.size());
    for (const std::filesystem::path& file : relativeFiles) {
        files.push_back(absoluteFile(root, file));
    }
    return files;
}

/// Preprocessing errors carry their location ("unlit/unlit.frag:3") as context: put it first, like compiler messages.
std::string describeSourceError(const Error& error) {
    return error.context.empty() ? error.message : fmt::format("{}: {}", error.context, error.message);
}

bool usesAny(const std::vector<std::filesystem::path>& files, const std::vector<std::filesystem::path>& changed) {
    return std::any_of(files.begin(), files.end(), [&changed](const std::filesystem::path& file) {
        return std::find(changed.begin(), changed.end(), file) != changed.end();
    });
}

} // namespace

ShaderLibrary::ShaderLibrary(std::filesystem::path shaderRoot) : m_root(std::move(shaderRoot)) {}

Expected<ShaderHandle> ShaderLibrary::load(const ShaderDesc& desc) {
    core::assertMainThread("ShaderLibrary::load");
    if (desc.name.empty() || desc.vertexPath.empty() || desc.fragmentPath.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a shader needs a name, a vertex path and a fragment path",
                         desc.name);
    }
    const bool duplicate = std::any_of(m_entries.begin(), m_entries.end(),
                                       [&desc](const Entry& entry) { return entry.desc.name == desc.name; });
    if (duplicate) {
        return makeError(ErrorCode::InvalidArgument, "a shader with this name is already loaded", desc.name);
    }

    Entry entry;
    entry.desc = desc;
    m_entries.push_back(std::move(entry));
    rebuild(m_entries.back(), false);
    updateWatchedFiles();
    return ShaderHandle{static_cast<std::uint32_t>(m_entries.size())};
}

const ShaderProgram& ShaderLibrary::get(ShaderHandle handle) const noexcept {
    if (handle.value == 0 || handle.value > m_entries.size()) {
        return m_empty;
    }
    return m_entries[handle.value - 1].program;
}

ShaderReloadSummary ShaderLibrary::reloadAll() {
    core::assertMainThread("ShaderLibrary::reloadAll");
    ShaderReloadSummary summary;
    for (Entry& entry : m_entries) {
        ++(rebuild(entry, true) ? summary.rebuilt : summary.failed);
    }
    updateWatchedFiles();
    spdlog::info("Shaders reloaded: {} program(s) rebuilt, {} failed", summary.rebuilt, summary.failed);
    return summary;
}

ShaderReloadSummary ShaderLibrary::pollHotReload() {
    const std::vector<std::filesystem::path> changed = m_watch.changed();
    if (changed.empty()) {
        return {};
    }
    core::assertMainThread("ShaderLibrary::pollHotReload");
    for (const std::filesystem::path& file : changed) {
        spdlog::info("Shader source changed: {}", core::pathToUtf8(file));
    }
    ShaderReloadSummary summary;
    for (Entry& entry : m_entries) {
        if (usesAny(entry.vertexFiles, changed) || usesAny(entry.fragmentFiles, changed)) {
            ++(rebuild(entry, true) ? summary.rebuilt : summary.failed);
        }
    }
    updateWatchedFiles();
    return summary;
}

bool ShaderLibrary::rebuild(Entry& entry, bool reload) {
    // Each stage watches the files its assembly read or tried to read, also when it failed: a missing include is
    // watched until it is created, and an edit of any file read before the error can fix it.
    const assets::ShaderSourceLoader loader(m_root);
    std::vector<std::filesystem::path> dependencies;
    const auto vertex = loader.load(entry.desc.vertexPath, entry.desc.defines, dependencies);
    entry.vertexFiles = absoluteFiles(m_root, dependencies);
    const auto fragment = loader.load(entry.desc.fragmentPath, entry.desc.defines, dependencies);
    entry.fragmentFiles = absoluteFiles(m_root, dependencies);

    std::string failure;
    if (!vertex) {
        failure = describeSourceError(vertex.error());
    }
    if (!fragment) {
        failure += failure.empty() ? "" : "\n";
        failure += describeSourceError(fragment.error());
    }
    if (failure.empty()) {
        auto program = ShaderProgram::build(*vertex, *fragment, entry.desc.name);
        if (program) {
            entry.program = std::move(*program);
            std::set<std::filesystem::path> files(entry.vertexFiles.begin(), entry.vertexFiles.end());
            files.insert(entry.fragmentFiles.begin(), entry.fragmentFiles.end());
            spdlog::info("Shader '{}' {} from {} + {} ({} file(s) with includes)", entry.desc.name,
                         reload ? "rebuilt" : "built", entry.desc.vertexPath, entry.desc.fragmentPath, files.size());
            return true;
        }
        failure = program.error().message;
    }

    if (entry.program.valid()) {
        spdlog::error("Shader '{}' failed to build; the previous version stays in use:\n{}", entry.desc.name, failure);
    } else {
        spdlog::error("Shader '{}' failed to build; nothing is drawn with it until a reload succeeds:\n{}",
                      entry.desc.name, failure);
    }
    return false;
}

void ShaderLibrary::updateWatchedFiles() {
    std::vector<std::filesystem::path> files;
    for (const Entry& entry : m_entries) {
        files.insert(files.end(), entry.vertexFiles.begin(), entry.vertexFiles.end());
        files.insert(files.end(), entry.fragmentFiles.begin(), entry.fragmentFiles.end());
    }
    m_watch.assign(files);
}

} // namespace rv::gl
