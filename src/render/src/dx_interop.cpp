#include <rv/render/dx_interop.hpp>

// WGL_NV_DX_interop2's entry points come from wglGetProcAddress (glad loads OpenGL's alone).
#include <windows.h>

#include <string>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/render/render_state_cache.hpp>

namespace rv::gl {
namespace {

constexpr GLenum kAccessReadOnly = 0x0000; // WGL_ACCESS_READ_ONLY_NV

using OpenDevice = HANDLE(WINAPI*)(void* dxDevice);
using CloseDevice = BOOL(WINAPI*)(HANDLE device);
using RegisterObject = HANDLE(WINAPI*)(HANDLE device, void* dxObject, GLuint name, GLenum type, GLenum access);
using UnregisterObject = BOOL(WINAPI*)(HANDLE device, HANDLE object);
using LockObjects = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);
using UnlockObjects = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);

struct Entry {
    OpenDevice open = nullptr;
    CloseDevice close = nullptr;
    RegisterObject registerObject = nullptr;
    UnregisterObject unregisterObject = nullptr;
    LockObjects lock = nullptr;
    UnlockObjects unlock = nullptr;

    [[nodiscard]] bool loaded() const noexcept {
        return open && close && registerObject && unregisterObject && lock && unlock;
    }
};

template <class Pfn>
Pfn load(const char* name) noexcept {
    return reinterpret_cast<Pfn>(reinterpret_cast<void*>(wglGetProcAddress(name)));
}

const Entry& entry() noexcept {
    static const Entry loaded = [] {
        Entry e;
        e.open = load<OpenDevice>("wglDXOpenDeviceNV");
        e.close = load<CloseDevice>("wglDXCloseDeviceNV");
        e.registerObject = load<RegisterObject>("wglDXRegisterObjectNV");
        e.unregisterObject = load<UnregisterObject>("wglDXUnregisterObjectNV");
        e.lock = load<LockObjects>("wglDXLockObjectsNV");
        e.unlock = load<UnlockObjects>("wglDXUnlockObjectsNV");
        return e;
    }();
    return loaded;
}

std::string lastError() {
    return fmt::format("GetLastError {}", GetLastError());
}

} // namespace

bool DxInteropTexture::available() noexcept {
    return entry().loaded();
}

Expected<std::unique_ptr<DxInteropTexture>> DxInteropTexture::create(void* d3dDevice, void* d3dTexture,
                                                                     glm::ivec2 size, std::string_view debugName) {
    const Entry& e = entry();
    if (!e.loaded()) {
        return makeError(ErrorCode::Unsupported, "the OpenGL context offers no WGL_NV_DX_interop2", "dx interop");
    }
    if (d3dDevice == nullptr || d3dTexture == nullptr || size.x <= 0 || size.y <= 0) {
        return makeError(ErrorCode::InvalidArgument, "no D3D11 device, texture or size", "dx interop");
    }
    std::unique_ptr<DxInteropTexture> interop(new DxInteropTexture());
    interop->m_size = size;
    interop->m_device = e.open(d3dDevice);
    if (interop->m_device == nullptr) {
        return makeError(ErrorCode::GraphicsError, "wglDXOpenDeviceNV failed: " + lastError(), "dx interop");
    }
    // The registered name comes from glGenTextures, as the extension asks.
    glGenTextures(1, &interop->m_shared);
    interop->m_object = e.registerObject(interop->m_device, d3dTexture, interop->m_shared, GL_TEXTURE_2D,
                                         kAccessReadOnly);
    if (interop->m_object == nullptr) {
        return makeError(ErrorCode::GraphicsError, "wglDXRegisterObjectNV failed: " + lastError(), "dx interop");
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &interop->m_copy);
    glTextureStorage2D(interop->m_copy, 1, GL_SRGB8_ALPHA8, size.x, size.y);
    glTextureParameteri(interop->m_copy, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(interop->m_copy, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(interop->m_copy, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(interop->m_copy, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const std::string label(debugName);
    glObjectLabel(GL_TEXTURE, interop->m_copy, static_cast<GLsizei>(label.size()), label.c_str());
    glCreateFramebuffers(1, &interop->m_readFramebuffer);
    glCreateFramebuffers(1, &interop->m_drawFramebuffer);
    glNamedFramebufferTexture(interop->m_drawFramebuffer, GL_COLOR_ATTACHMENT0, interop->m_copy, 0);
    return interop;
}

DxInteropTexture::~DxInteropTexture() {
    const Entry& e = entry();
    if (m_readFramebuffer != 0) {
        glDeleteFramebuffers(1, &m_readFramebuffer);
    }
    if (m_drawFramebuffer != 0) {
        glDeleteFramebuffers(1, &m_drawFramebuffer);
    }
    if (m_copy != 0) {
        glDeleteTextures(1, &m_copy);
    }
    if (m_object != nullptr && e.loaded()) {
        e.unregisterObject(m_device, m_object);
    }
    if (m_shared != 0) {
        glDeleteTextures(1, &m_shared);
    }
    if (m_device != nullptr && e.loaded()) {
        e.close(m_device);
    }
}

Status DxInteropTexture::copy(RenderStateCache& state) {
    const Entry& e = entry();
    HANDLE object = m_object;
    if (e.lock(m_device, 1, &object) == FALSE) {
        return makeError(ErrorCode::GraphicsError, "wglDXLockObjectsNV failed: " + lastError(), "dx interop");
    }
    // The registered texture's first row is the window's top (D3D's), as an uploaded image's and a decoded frame's
    // first row are (ADR-0002's top-left texture origin): copied as it is.
    glNamedFramebufferTexture(m_readFramebuffer, GL_COLOR_ATTACHMENT0, m_shared, 0);
    state.setFramebufferSrgb(false);
    glBlitNamedFramebuffer(m_readFramebuffer, m_drawFramebuffer, 0, 0, m_size.x, m_size.y, 0, 0, m_size.x, m_size.y,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glNamedFramebufferTexture(m_readFramebuffer, GL_COLOR_ATTACHMENT0, 0, 0);
    e.unlock(m_device, 1, &object);
    return ok();
}

} // namespace rv::gl
