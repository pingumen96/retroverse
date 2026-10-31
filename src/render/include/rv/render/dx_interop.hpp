#pragma once

#include <memory>
#include <string_view>

#include <glad/gl.h>
#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>

namespace rv::gl {

class RenderStateCache;

/// A D3D11 texture's picture in OpenGL (I10 step 6, ADR-0017 §7: the captured game on the hero CRT), through
/// WGL_NV_DX_interop2 on the current context: the D3D11 texture registered as a GL texture, copied on copy() into a
/// texture of OpenGL's own (GL_SRGB8_ALPHA8, the display-encoded bytes as they are), which a material samples between
/// copies without any lock held. The D3D11 device and texture stay the caller's (platform_win::WindowCapture) and must
/// outlive this object. Windows only; main thread, GL context current.
class DxInteropTexture {
public:
    /// Whether the context offers WGL_NV_DX_interop2 (its entry points load).
    [[nodiscard]] static bool available() noexcept;
    /// Registers `d3dTexture` (an ID3D11Texture2D of `size`, B8G8R8A8) of `d3dDevice` (an ID3D11Device). Errors:
    /// Unsupported without the extension, GraphicsError when the device or the texture cannot be registered.
    [[nodiscard]] static Expected<std::unique_ptr<DxInteropTexture>> create(void* d3dDevice, void* d3dTexture,
                                                                            glm::ivec2 size,
                                                                            std::string_view debugName);
    ~DxInteropTexture();
    DxInteropTexture(const DxInteropTexture&) = delete;
    DxInteropTexture& operator=(const DxInteropTexture&) = delete;

    /// The D3D11 texture's current picture into texture() (locked for the copy; GL_FRAMEBUFFER_SRGB left off). Errors:
    /// the lock failed.
    [[nodiscard]] Status copy(RenderStateCache& state);
    [[nodiscard]] GLuint texture() const noexcept { return m_copy; }
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_size; }

private:
    DxInteropTexture() = default;

    void* m_device = nullptr;
    void* m_object = nullptr;
    GLuint m_shared = 0;
    GLuint m_copy = 0;
    GLuint m_readFramebuffer = 0;
    GLuint m_drawFramebuffer = 0;
    glm::ivec2 m_size{0};
};

} // namespace rv::gl
