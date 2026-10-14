#include "gl_object_label.hpp"

#include <cstddef>

namespace rv::gl::detail {
namespace {

/// GL_MAX_LABEL_LENGTH is at least 256, terminator included.
constexpr std::size_t kMaxPortableLabelLength = 255;

} // namespace

void setObjectLabel(GLenum identifier, GLuint object, std::string_view label) noexcept {
    if (object == 0 || label.empty() || glObjectLabel == nullptr) {
        return;
    }
    const std::string_view truncated = label.substr(0, kMaxPortableLabelLength);
    glObjectLabel(identifier, object, static_cast<GLsizei>(truncated.size()), truncated.data());
}

} // namespace rv::gl::detail
