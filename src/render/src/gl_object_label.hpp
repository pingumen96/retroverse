#pragma once

#include <string_view>

#include <glad/gl.h>

namespace rv::gl::detail {

/// glObjectLabel for debuggers (RenderDoc, Nsight) and debug messages. The label is truncated to 255 characters, the
/// smallest GL_MAX_LABEL_LENGTH an implementation may have; empty labels are skipped.
void setObjectLabel(GLenum identifier, GLuint object, std::string_view label) noexcept;

} // namespace rv::gl::detail
