#include <rv/render/shader_program.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "gl_object_label.hpp"

namespace rv::gl {
namespace {

/// Owns a shader object until the program has been linked.
class ShaderObject {
public:
    explicit ShaderObject(GLenum stage) : m_id(glCreateShader(stage)) {}
    ShaderObject(const ShaderObject&) = delete;
    ShaderObject& operator=(const ShaderObject&) = delete;
    ~ShaderObject() {
        if (m_id != 0) {
            glDeleteShader(m_id);
        }
    }

    [[nodiscard]] GLuint id() const noexcept { return m_id; }

private:
    GLuint m_id;
};

std::string_view stageName(GLenum stage) noexcept {
    switch (stage) {
    case GL_VERTEX_SHADER:
        return "vertex";
    case GL_FRAGMENT_SHADER:
        return "fragment";
    default:
        return "unknown";
    }
}

std::string trimLog(std::string log) {
    while (!log.empty() && (log.back() == '\0' || log.back() == '\n' || log.back() == '\r' || log.back() == ' ')) {
        log.pop_back();
    }
    return log;
}

std::string shaderInfoLog(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 0) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, length, &written, log.data());
    log.resize(static_cast<std::size_t>(std::clamp<GLsizei>(written, 0, length)));
    return trimLog(std::move(log));
}

std::string programInfoLog(GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length <= 0) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(program, length, &written, log.data());
    log.resize(static_cast<std::size_t>(std::clamp<GLsizei>(written, 0, length)));
    return trimLog(std::move(log));
}

/// Compiles one stage. Returns the failure description ("<stage> shader ...:\n<log>") or nothing on success.
std::optional<std::string> compileStage(const ShaderObject& shader, GLenum stage, std::string_view source,
                                        std::string_view debugName) {
    if (shader.id() == 0) {
        return fmt::format("{} shader: glCreateShader failed", stageName(stage));
    }
    if (source.empty()) {
        return fmt::format("{} shader: source is empty", stageName(stage));
    }
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        return fmt::format("{} shader: source of {} bytes is too large", stageName(stage), source.size());
    }
    const GLchar* text = source.data();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader.id(), 1, &text, &length);
    glCompileShader(shader.id());

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader.id(), GL_COMPILE_STATUS, &compiled);
    const std::string log = shaderInfoLog(shader.id());
    if (compiled == GL_FALSE) {
        return fmt::format("{} shader compilation failed:\n{}", stageName(stage), log.empty() ? "(empty log)" : log);
    }
    if (!log.empty()) {
        spdlog::debug("{} shader of '{}' compiled with messages:\n{}", stageName(stage), debugName, log);
    }
    return std::nullopt;
}

} // namespace

ShaderProgram::ShaderProgram(GLuint id, std::string debugName) noexcept : m_id(id), m_debugName(std::move(debugName)) {}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : m_id(std::exchange(other.m_id, 0u)), m_debugName(std::move(other.m_debugName)),
      m_uniformLocations(std::move(other.m_uniformLocations)) {
    other.m_uniformLocations.clear();
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        release();
        m_id = std::exchange(other.m_id, 0u);
        m_debugName = std::move(other.m_debugName);
        m_uniformLocations = std::move(other.m_uniformLocations);
        other.m_uniformLocations.clear();
    }
    return *this;
}

ShaderProgram::~ShaderProgram() {
    release();
}

void ShaderProgram::release() noexcept {
    if (m_id != 0) {
        glDeleteProgram(m_id);
        m_id = 0;
    }
    m_uniformLocations.clear();
}

Expected<ShaderProgram> ShaderProgram::build(std::string_view vertexSource, std::string_view fragmentSource,
                                             std::string_view debugName) {
    // Both stages are compiled before reporting, so one error report covers every broken stage.
    const ShaderObject vertex(GL_VERTEX_SHADER);
    const ShaderObject fragment(GL_FRAGMENT_SHADER);
    const std::optional<std::string> vertexError = compileStage(vertex, GL_VERTEX_SHADER, vertexSource, debugName);
    const std::optional<std::string> fragmentError =
        compileStage(fragment, GL_FRAGMENT_SHADER, fragmentSource, debugName);
    if (vertexError || fragmentError) {
        std::string message = vertexError.value_or("");
        if (fragmentError) {
            message += message.empty() ? "" : "\n";
            message += *fragmentError;
        }
        return makeError(ErrorCode::GraphicsError, std::move(message), std::string(debugName));
    }

    const GLuint id = glCreateProgram();
    if (id == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateProgram failed", std::string(debugName));
    }
    ShaderProgram program(id, std::string(debugName));
    glAttachShader(id, vertex.id());
    glAttachShader(id, fragment.id());
    glLinkProgram(id);
    glDetachShader(id, vertex.id());
    glDetachShader(id, fragment.id());

    GLint linked = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &linked);
    const std::string log = programInfoLog(id);
    if (linked == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("program link failed:\n{}", log.empty() ? "(empty log)" : log),
                         std::string(debugName));
    }
    if (!log.empty()) {
        spdlog::debug("Program '{}' linked with messages:\n{}", debugName, log);
    }
    detail::setObjectLabel(GL_PROGRAM, id, debugName);
    return program;
}

void ShaderProgram::use() const {
    glUseProgram(m_id);
}

GLint ShaderProgram::uniformLocation(std::string_view name) const {
    if (m_id == 0) {
        return -1;
    }
    if (const auto cached = m_uniformLocations.find(name); cached != m_uniformLocations.end()) {
        return cached->second;
    }
    std::string key(name);
    const GLint location = glGetUniformLocation(m_id, key.c_str());
    if (location < 0) {
        spdlog::debug("Uniform '{}' is not active in program '{}'", key, m_debugName);
    }
    m_uniformLocations.emplace(std::move(key), location);
    return location;
}

Status ShaderProgram::bindUniformBlock(std::string_view blockName, GLuint binding) {
    if (m_id == 0) {
        return makeError(ErrorCode::InvalidArgument, "program is empty", std::string(blockName));
    }
    const std::string name(blockName);
    const GLuint index = glGetUniformBlockIndex(m_id, name.c_str());
    if (index == GL_INVALID_INDEX) {
        return makeError(ErrorCode::NotFound, fmt::format("uniform block '{}' is not active", name), m_debugName);
    }
    glUniformBlockBinding(m_id, index, binding);
    return ok();
}

void ShaderProgram::setInt(std::string_view name, int value) {
    if (const GLint location = uniformLocation(name); location >= 0) {
        glProgramUniform1i(m_id, location, value);
    }
}

void ShaderProgram::setFloat(std::string_view name, float value) {
    if (const GLint location = uniformLocation(name); location >= 0) {
        glProgramUniform1f(m_id, location, value);
    }
}

void ShaderProgram::setVec2(std::string_view name, const glm::vec2& value) {
    if (const GLint location = uniformLocation(name); location >= 0) {
        glProgramUniform2fv(m_id, location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setVec3(std::string_view name, const glm::vec3& value) {
    if (const GLint location = uniformLocation(name); location >= 0) {
        glProgramUniform3fv(m_id, location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setVec4(std::string_view name, const glm::vec4& value) {
    if (const GLint location = uniformLocation(name); location >= 0) {
        glProgramUniform4fv(m_id, location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setMat4(std::string_view name, const glm::mat4& value) {
    if (const GLint location = uniformLocation(name); location >= 0) {
        glProgramUniformMatrix4fv(m_id, location, 1, GL_FALSE, glm::value_ptr(value));
    }
}

} // namespace rv::gl
