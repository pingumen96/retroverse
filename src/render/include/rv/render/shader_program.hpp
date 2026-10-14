#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <rv/assets/shader_source.hpp>
#include <rv/core/expected.hpp>

namespace rv::gl {

/// Move-only RAII linked GLSL program (vertex + fragment stages). Uniforms are written with glProgramUniform*, so the
/// program does not need to be bound; locations are looked up once per name and cached. Setting a uniform the
/// linker removed (location -1) is a no-op, as in GL. Uniform values are state of the GL program object, not of this
/// wrapper, which is why the setters are const. A default-constructed or moved-from program is empty (id 0).
/// Main thread only.
class ShaderProgram {
public:
    ShaderProgram() noexcept = default;
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ~ShaderProgram();

    /// Compiles both stages and links them. On failure the error (GraphicsError, context = `debugName`) carries the
    /// stage and the driver's complete compile or link log; nothing is leaked.
    [[nodiscard]] static Expected<ShaderProgram> build(std::string_view vertexSource, std::string_view fragmentSource,
                                                       std::string_view debugName);
    /// Same for sources assembled by rv::assets::ShaderSourceLoader: every compiler message is rewritten with
    /// rv::assets::mapCompilerLog, so it names "<file>:<line>" (relative to the shader root) instead of a source-string
    /// number. Link messages refer to no source string (they name variables and stages), so a link failure is reported
    /// as the driver wrote it, headed by the two root files.
    [[nodiscard]] static Expected<ShaderProgram>
    build(const assets::ShaderSource& vertex, const assets::ShaderSource& fragment, std::string_view debugName);

    /// glUseProgram.
    void use() const;

    /// Location of an active uniform, -1 when the program has none with that name (cached either way).
    [[nodiscard]] GLint uniformLocation(std::string_view name) const;

    /// Assigns uniform block `blockName` to uniform buffer binding point `binding`. NotFound when the block is not
    /// active in the program.
    [[nodiscard]] Status bindUniformBlock(std::string_view blockName, GLuint binding);

    void setInt(std::string_view name, int value) const;
    void setFloat(std::string_view name, float value) const;
    void setVec2(std::string_view name, const glm::vec2& value) const;
    void setVec3(std::string_view name, const glm::vec3& value) const;
    void setVec4(std::string_view name, const glm::vec4& value) const;
    /// Column-major, as stored by glm.
    void setMat4(std::string_view name, const glm::mat4& value) const;

    [[nodiscard]] GLuint id() const noexcept { return m_id; }
    [[nodiscard]] bool valid() const noexcept { return m_id != 0; }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    /// Rewrites one stage's compile log before it is reported (identity for plain sources).
    using LogMapper = std::function<std::string(std::string_view log)>;

    ShaderProgram(GLuint id, std::string debugName) noexcept;
    /// `stageFiles` names the sources in link failures ("unlit/unlit.vert + unlit/unlit.frag"); may be empty.
    [[nodiscard]] static Expected<ShaderProgram>
    compileAndLink(std::string_view vertexSource, std::string_view fragmentSource, std::string_view debugName,
                   const LogMapper& mapVertexLog, const LogMapper& mapFragmentLog, std::string_view stageFiles);
    void release() noexcept;

    GLuint m_id = 0;
    std::string m_debugName;
    mutable std::map<std::string, GLint, std::less<>> m_uniformLocations;
};

} // namespace rv::gl
