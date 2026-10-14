#include <rv/render/framebuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/type_ptr.hpp>

#include <rv/assets/image_io.hpp>
#include <rv/render/render_state_cache.hpp>

#include "gl_object_label.hpp"

namespace rv::gl {
namespace {

GLint glInteger(GLenum name) {
    GLint value = 0;
    glGetIntegerv(name, &value);
    return value;
}

/// Largest width and height every attachment can have.
glm::ivec2 maxFramebufferSize() {
    const GLint attachmentLimit = std::min(glInteger(GL_MAX_TEXTURE_SIZE), glInteger(GL_MAX_RENDERBUFFER_SIZE));
    return {std::min(glInteger(GL_MAX_FRAMEBUFFER_WIDTH), attachmentLimit),
            std::min(glInteger(GL_MAX_FRAMEBUFFER_HEIGHT), attachmentLimit)};
}

GLenum internalFormat(ColorFormat format) noexcept {
    return format == ColorFormat::Rgba16F ? GL_RGBA16F : GL_SRGB8_ALPHA8;
}

std::string_view statusName(GLenum status) noexcept {
    switch (status) {
    case GL_FRAMEBUFFER_UNDEFINED:
        return "GL_FRAMEBUFFER_UNDEFINED";
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
        return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
        return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
        return "GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER";
    case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
        return "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER";
    case GL_FRAMEBUFFER_UNSUPPORTED:
        return "GL_FRAMEBUFFER_UNSUPPORTED";
    case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
        return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
    case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
        return "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS";
    default:
        return "unknown status";
    }
}

struct Attachments {
    TextureObject color;
    RenderbufferObject depthStencil;
    FramebufferObject framebuffer;
};

Expected<Attachments> allocate(const FramebufferDesc& desc, const std::string& debugName) {
    const glm::ivec2 limit = maxFramebufferSize();
    if (desc.size.x < 1 || desc.size.y < 1 || desc.size.x > limit.x || desc.size.y > limit.y) {
        return makeError(
            ErrorCode::OutOfRange,
            fmt::format("framebuffer size {}x{} is outside 1x1..{}x{}", desc.size.x, desc.size.y, limit.x, limit.y),
            debugName);
    }
    Attachments attachments;

    GLuint color = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &color);
    attachments.color = TextureObject(color);
    if (color == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateTextures returned no texture", debugName);
    }
    glTextureStorage2D(color, 1, internalFormat(desc.colorFormat), desc.size.x, desc.size.y);
    // Storage only becomes immutable when the allocation succeeded (GL_OUT_OF_MEMORY otherwise).
    GLint immutable = GL_FALSE;
    glGetTextureParameteriv(color, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    if (immutable == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not allocate a {}x{} {} color texture", desc.size.x, desc.size.y,
                                     toString(desc.colorFormat)),
                         debugName);
    }
    glTextureParameteri(color, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(color, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(color, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(color, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    detail::setObjectLabel(GL_TEXTURE, color, debugName + ".color");

    GLuint framebuffer = 0;
    glCreateFramebuffers(1, &framebuffer);
    attachments.framebuffer = FramebufferObject(framebuffer);
    if (framebuffer == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateFramebuffers returned no framebuffer", debugName);
    }
    glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, color, 0);

    if (desc.depthStencil) {
        GLuint depthStencil = 0;
        glCreateRenderbuffers(1, &depthStencil);
        attachments.depthStencil = RenderbufferObject(depthStencil);
        if (depthStencil == 0) {
            return makeError(ErrorCode::GraphicsError, "glCreateRenderbuffers returned no renderbuffer", debugName);
        }
        glNamedRenderbufferStorage(depthStencil, GL_DEPTH24_STENCIL8, desc.size.x, desc.size.y);
        GLint width = 0;
        glGetNamedRenderbufferParameteriv(depthStencil, GL_RENDERBUFFER_WIDTH, &width);
        if (width != desc.size.x) {
            return makeError(ErrorCode::GraphicsError,
                             fmt::format("could not allocate a {}x{} depth/stencil buffer", desc.size.x, desc.size.y),
                             debugName);
        }
        detail::setObjectLabel(GL_RENDERBUFFER, depthStencil, debugName + ".depthStencil");
        glNamedFramebufferRenderbuffer(framebuffer, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthStencil);
    }

    glNamedFramebufferDrawBuffer(framebuffer, GL_COLOR_ATTACHMENT0);
    glNamedFramebufferReadBuffer(framebuffer, GL_COLOR_ATTACHMENT0);
    const GLenum status = glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("framebuffer is incomplete: {} (0x{:04X})", statusName(status), status),
                         debugName);
    }
    detail::setObjectLabel(GL_FRAMEBUFFER, framebuffer, debugName);
    return attachments;
}

} // namespace

std::string_view toString(ColorFormat format) noexcept {
    switch (format) {
    case ColorFormat::Srgb8Alpha8:
        return "srgb8_alpha8";
    case ColorFormat::Rgba16F:
        return "rgba16f";
    }
    return "unknown";
}

Expected<Framebuffer> Framebuffer::create(const FramebufferDesc& desc, std::string_view debugName) {
    std::string name(debugName);
    auto attachments = allocate(desc, name);
    if (!attachments) {
        return propagate(attachments.error());
    }
    Framebuffer framebuffer;
    framebuffer.m_desc = desc;
    framebuffer.m_debugName = std::move(name);
    framebuffer.m_color = std::move(attachments->color);
    framebuffer.m_depthStencil = std::move(attachments->depthStencil);
    framebuffer.m_framebuffer = std::move(attachments->framebuffer);
    return framebuffer;
}

Status Framebuffer::resize(glm::ivec2 size) {
    if (!valid()) {
        return makeError(ErrorCode::InvalidArgument, "cannot resize an empty framebuffer", m_debugName);
    }
    if (size == m_desc.size) {
        return ok();
    }
    FramebufferDesc desc = m_desc;
    desc.size = size;
    auto attachments = allocate(desc, m_debugName);
    if (!attachments) {
        return propagate(attachments.error());
    }
    m_desc = desc;
    m_framebuffer = std::move(attachments->framebuffer);
    m_depthStencil = std::move(attachments->depthStencil);
    m_color = std::move(attachments->color);
    return ok();
}

void Framebuffer::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer.id());
}

void Framebuffer::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Framebuffer::clear(const glm::vec4& colorLinear) const {
    if (!valid()) {
        return;
    }
    glClearNamedFramebufferfv(m_framebuffer.id(), GL_COLOR, 0, glm::value_ptr(colorLinear));
    if (m_depthStencil.valid()) {
        glClearNamedFramebufferfi(m_framebuffer.id(), GL_DEPTH_STENCIL, 0, 1.0f, 0);
    }
}

void Framebuffer::blitToDefault(RenderStateCache& state, glm::ivec2 windowSize) const {
    if (!valid() || windowSize.x <= 0 || windowSize.y <= 0) {
        return;
    }
    // Raw copy: with GL_FRAMEBUFFER_SRGB enabled a blit would decode the sRGB source and might or might not re-encode
    // into the window, depending on how the driver reports the default framebuffer.
    state.setFramebufferSrgb(false);
    const GLenum filter = windowSize == m_desc.size ? GL_NEAREST : GL_LINEAR;
    glBlitNamedFramebuffer(m_framebuffer.id(), 0, 0, 0, m_desc.size.x, m_desc.size.y, 0, 0, windowSize.x, windowSize.y,
                           GL_COLOR_BUFFER_BIT, filter);
    unbind();
    state.setViewport(glm::ivec4(0, 0, windowSize.x, windowSize.y));
}

Expected<assets::ImageData> Framebuffer::readPixels() const {
    if (!valid()) {
        return makeError(ErrorCode::InvalidArgument, "cannot read an empty framebuffer", m_debugName);
    }
    const std::size_t byteCount =
        static_cast<std::size_t>(m_desc.size.x) * static_cast<std::size_t>(m_desc.size.y) * 4u;
    if (byteCount > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("{}x{} pixels do not fit one readback", m_desc.size.x, m_desc.size.y),
                         m_debugName);
    }
    assets::ImageData image;
    image.width = m_desc.size.x;
    image.height = m_desc.size.y;
    image.channels = 4;
    image.pixels.resize(byteCount);

    // The sRGB question of texture queries is answered differently by drivers; a GL_RGBA8 view of the same storage
    // has no sRGB encoding, so its texels are returned exactly as stored.
    GLuint source = m_color.id();
    TextureObject view;
    if (m_desc.colorFormat == ColorFormat::Srgb8Alpha8) {
        GLuint viewId = 0;
        glGenTextures(1, &viewId); // glTextureView needs a name that has never been bound: not glCreateTextures
        view = TextureObject(viewId);
        glTextureView(viewId, GL_TEXTURE_2D, m_color.id(), GL_RGBA8, 0, 1, 0, 1);
        source = viewId;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4); // RGBA8 rows are always a multiple of 4 bytes: no padding
    glGetTextureImage(source, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(byteCount), image.pixels.data());
    // Texture rows start at the bottom of the framebuffer; images are stored top row first.
    assets::flipRowsInPlace(image);
    return image;
}

} // namespace rv::gl
