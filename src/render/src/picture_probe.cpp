#include <rv/render/picture_probe.hpp>

#include <string>

#include <rv/render/render_state_cache.hpp>

namespace rv::gl {

namespace {

constexpr std::size_t kBytes = static_cast<std::size_t>(PictureProbe::kSize.x) * PictureProbe::kSize.y * 4;

} // namespace

Expected<std::unique_ptr<PictureProbe>> PictureProbe::create(std::string_view debugName) {
    std::unique_ptr<PictureProbe> probe(new PictureProbe());
    glCreateTextures(GL_TEXTURE_2D, 1, &probe->m_small);
    glTextureStorage2D(probe->m_small, 1, GL_RGBA8, kSize.x, kSize.y);
    const std::string label(debugName);
    glObjectLabel(GL_TEXTURE, probe->m_small, static_cast<GLsizei>(label.size()), label.c_str());
    glCreateFramebuffers(1, &probe->m_readFramebuffer);
    glCreateFramebuffers(1, &probe->m_drawFramebuffer);
    glNamedFramebufferTexture(probe->m_drawFramebuffer, GL_COLOR_ATTACHMENT0, probe->m_small, 0);
    if (glCheckNamedFramebufferStatus(probe->m_drawFramebuffer, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return makeError(ErrorCode::GraphicsError, "the probe's framebuffer is incomplete", label);
    }
    glCreateBuffers(1, &probe->m_buffer);
    glNamedBufferStorage(probe->m_buffer, static_cast<GLsizeiptr>(kBytes), nullptr, GL_CLIENT_STORAGE_BIT);
    probe->m_pixels.resize(kBytes);
    return probe;
}

PictureProbe::~PictureProbe() {
    if (m_fence != nullptr) {
        glDeleteSync(m_fence);
    }
    if (m_buffer != 0) {
        glDeleteBuffers(1, &m_buffer);
    }
    if (m_readFramebuffer != 0) {
        glDeleteFramebuffers(1, &m_readFramebuffer);
    }
    if (m_drawFramebuffer != 0) {
        glDeleteFramebuffers(1, &m_drawFramebuffer);
    }
    if (m_small != 0) {
        glDeleteTextures(1, &m_small);
    }
}

bool PictureProbe::request(GLuint texture, const glm::ivec4& area, RenderStateCache& state) {
    if (m_fence != nullptr || texture == 0 || area.z <= 0 || area.w <= 0) {
        return false;
    }
    glNamedFramebufferTexture(m_readFramebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    state.setFramebufferSrgb(false); // the bytes as they are: black is black in either encoding
    // The texture's first row is the image's top, as is the framebuffer's row 0: the area's rows as they are.
    glBlitNamedFramebuffer(m_readFramebuffer, m_drawFramebuffer, area.x, area.y, area.x + area.z, area.y + area.w, 0, 0,
                           kSize.x, kSize.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glNamedFramebufferTexture(m_readFramebuffer, GL_COLOR_ATTACHMENT0, 0, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTextureImage(m_small, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(kBytes), nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    return true;
}

std::optional<std::span<const std::uint8_t>> PictureProbe::poll() {
    if (m_fence == nullptr) {
        return std::nullopt;
    }
    const GLenum waited = glClientWaitSync(m_fence, 0, 0);
    if (waited != GL_ALREADY_SIGNALED && waited != GL_CONDITION_SATISFIED) {
        return std::nullopt;
    }
    glDeleteSync(m_fence);
    m_fence = nullptr;
    glGetNamedBufferSubData(m_buffer, 0, static_cast<GLsizeiptr>(kBytes), m_pixels.data());
    return std::span<const std::uint8_t>(m_pixels);
}

} // namespace rv::gl
