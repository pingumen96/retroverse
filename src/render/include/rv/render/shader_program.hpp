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

#include <rv/core/expected.hpp>

namespace rv::gl {

/// Move-only RAII linked GLSL program (vertex + fragment stages). Uniforms are written with glProgramUniform*, so the
/// program does not need to be bound; locations are looked up once per name and cached. Setting a uniform the
/// linker removed (location -1) is a no-op, as in GL. A default-constructed or moved-from program is empty (id 0).
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

    /// glUseProgram.
    void use() const;

    /// Location of an active uniform, -1 when the program has none with that name (cached either way).
    [[nodiscard]] GLint uniformLocation(std::string_view name) const;

    /// Assigns uniform block `blockName` to uniform buffer binding point `binding`. NotFound when the block is not
    /// active in the program.
    [[nodiscard]] Status bindUniformBlock(std::string_view blockName, GLuint binding);

    void setInt(std::string_view name, int value);
    void setFloat(std::string_view name, float value);
    void setVec2(std::string_view name, const glm::vec2& value);
    void setVec3(std::string_view name, const glm::vec3& value);
    void setVec4(std::string_view name, const glm::vec4& value);
    /// Column-major, as stored by glm.
    void setMat4(std::string_view name, const glm::mat4& value);

    [[nodiscard]] GLuint id() const noexcept { return m_id; }
    [[nodiscard]] bool valid() const noexcept { return m_id != 0; }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    ShaderProgram(GLuint id, std::string debugName) noexcept;
    void release() noexcept;

    GLuint m_id = 0;
    std::string m_debugName;
    mutable std::map<std::string, GLint, std::less<>> m_uniformLocations;
};

} // namespace rv::gl
