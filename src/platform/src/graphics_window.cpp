#include <rv/platform/graphics_window.hpp>

#include <string>
#include <utility>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_video.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/main_thread.hpp>

namespace rv::platform {
namespace {

GlProc loadGlProc(const char* name) {
    return SDL_GL_GetProcAddress(name);
}

std::string sdlError() {
    const char* error = SDL_GetError();
    return (error != nullptr && *error != '\0') ? std::string(error) : std::string("unknown SDL error");
}

struct GlAttribute {
    SDL_GLAttr attribute;
    const char* name;
    int value;
};

/// Applies every context and pixel format attribute; they only take effect for windows created afterwards.
Status applyContextAttributes(const GlContextDesc& desc) {
    SDL_GL_ResetAttributes();
    const GlAttribute attributes[] = {
        {SDL_GL_CONTEXT_MAJOR_VERSION, "SDL_GL_CONTEXT_MAJOR_VERSION", desc.versionMajor},
        {SDL_GL_CONTEXT_MINOR_VERSION, "SDL_GL_CONTEXT_MINOR_VERSION", desc.versionMinor},
        {SDL_GL_CONTEXT_PROFILE_MASK, "SDL_GL_CONTEXT_PROFILE_MASK", SDL_GL_CONTEXT_PROFILE_CORE},
        {SDL_GL_CONTEXT_FLAGS, "SDL_GL_CONTEXT_FLAGS", desc.debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0},
        {SDL_GL_RED_SIZE, "SDL_GL_RED_SIZE", 8},
        {SDL_GL_GREEN_SIZE, "SDL_GL_GREEN_SIZE", 8},
        {SDL_GL_BLUE_SIZE, "SDL_GL_BLUE_SIZE", 8},
        {SDL_GL_ALPHA_SIZE, "SDL_GL_ALPHA_SIZE", 8},
        {SDL_GL_DEPTH_SIZE, "SDL_GL_DEPTH_SIZE", desc.depthBits},
        {SDL_GL_STENCIL_SIZE, "SDL_GL_STENCIL_SIZE", desc.stencilBits},
        {SDL_GL_DOUBLEBUFFER, "SDL_GL_DOUBLEBUFFER", desc.doubleBuffer ? 1 : 0},
        {SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, "SDL_GL_FRAMEBUFFER_SRGB_CAPABLE", desc.srgbFramebuffer ? 1 : 0},
        {SDL_GL_ACCELERATED_VISUAL, "SDL_GL_ACCELERATED_VISUAL", 1},
    };
    for (const GlAttribute& attribute : attributes) {
        if (!SDL_GL_SetAttribute(attribute.attribute, attribute.value)) {
            return makeError(ErrorCode::PlatformError,
                             fmt::format("{} = {} was rejected: {}", attribute.name, attribute.value, sdlError()));
        }
    }
    return ok();
}

} // namespace

std::string_view toString(VsyncMode mode) noexcept {
    switch (mode) {
    case VsyncMode::Off:
        return "off";
    case VsyncMode::On:
        return "on";
    case VsyncMode::Adaptive:
        return "adaptive";
    }
    return "unknown";
}

GraphicsWindow::GraphicsWindow(SDL_Window* window, SDL_GLContextState* context, glm::ivec2 windowedSize) noexcept
    : m_window(window), m_context(context), m_windowedSize(windowedSize) {}

GraphicsWindow::GraphicsWindow(GraphicsWindow&& other) noexcept
    : m_window(std::exchange(other.m_window, nullptr)), m_context(std::exchange(other.m_context, nullptr)),
      m_windowedSize(other.m_windowedSize), m_vsync(other.m_vsync) {}

GraphicsWindow& GraphicsWindow::operator=(GraphicsWindow&& other) noexcept {
    if (this != &other) {
        destroy();
        m_window = std::exchange(other.m_window, nullptr);
        m_context = std::exchange(other.m_context, nullptr);
        m_windowedSize = other.m_windowedSize;
        m_vsync = other.m_vsync;
    }
    return *this;
}

GraphicsWindow::~GraphicsWindow() {
    destroy();
}

void GraphicsWindow::destroy() noexcept {
    if (m_context != nullptr) {
        SDL_GL_MakeCurrent(m_window, nullptr);
        SDL_GL_DestroyContext(m_context);
        m_context = nullptr;
    }
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

std::uint32_t GraphicsWindow::id() const noexcept {
    return m_window != nullptr ? SDL_GetWindowID(m_window) : 0u;
}

glm::ivec2 GraphicsWindow::drawableSize() const noexcept {
    int width = 0;
    int height = 0;
    if (m_window == nullptr || !SDL_GetWindowSizeInPixels(m_window, &width, &height)) {
        return glm::ivec2(0);
    }
    return {width, height};
}

glm::ivec2 GraphicsWindow::windowSize() const noexcept {
    int width = 0;
    int height = 0;
    if (m_window == nullptr || !SDL_GetWindowSize(m_window, &width, &height)) {
        return glm::ivec2(0);
    }
    return {width, height};
}

float GraphicsWindow::displayScale() const noexcept {
    const float scale = m_window != nullptr ? SDL_GetWindowDisplayScale(m_window) : 0.0f;
    return scale > 0.0f ? scale : 1.0f;
}

void GraphicsWindow::setTitle(std::string_view title) {
    if (m_window == nullptr) {
        return;
    }
    const std::string text(title);
    if (!SDL_SetWindowTitle(m_window, text.c_str())) {
        spdlog::debug("SDL_SetWindowTitle failed: {}", sdlError());
    }
}

void GraphicsWindow::show() {
    if (m_window != nullptr && !SDL_ShowWindow(m_window)) {
        spdlog::warn("SDL_ShowWindow failed: {}", sdlError());
    }
}

Status GraphicsWindow::setFullscreen(bool fullscreen) {
    if (m_window == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the window has been destroyed");
    }
    if (fullscreen == isFullscreen()) {
        return ok();
    }
    if (fullscreen) {
        m_windowedSize = windowSize();
        // A null display mode selects borderless fullscreen at the desktop resolution.
        if (!SDL_SetWindowFullscreenMode(m_window, nullptr) || !SDL_SetWindowFullscreen(m_window, true)) {
            return makeError(ErrorCode::PlatformError, "could not enter borderless fullscreen: " + sdlError());
        }
        return ok();
    }
    if (!SDL_SetWindowFullscreen(m_window, false)) {
        return makeError(ErrorCode::PlatformError, "could not leave fullscreen: " + sdlError());
    }
    // Size requests are ignored while the window is still fullscreen: wait for asynchronous window systems first.
    SDL_SyncWindow(m_window);
    if (m_windowedSize.x > 0 && m_windowedSize.y > 0 &&
        !SDL_SetWindowSize(m_window, m_windowedSize.x, m_windowedSize.y)) {
        spdlog::debug("Could not restore the windowed size {}x{}: {}", m_windowedSize.x, m_windowedSize.y, sdlError());
    }
    return ok();
}

Status GraphicsWindow::toggleBorderlessFullscreen() {
    return setFullscreen(!isFullscreen());
}

bool isFullscreenToggle(const SDL_Event& event) noexcept {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
           (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) && (event.key.mod & SDL_KMOD_ALT) != 0;
}

bool GraphicsWindow::isFullscreen() const noexcept {
    return m_window != nullptr && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

bool GraphicsWindow::isMinimized() const noexcept {
    return m_window != nullptr && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
}

Status GraphicsWindow::swap() {
    if (m_window == nullptr || !SDL_GL_SwapWindow(m_window)) {
        return makeError(ErrorCode::GraphicsError, "SDL_GL_SwapWindow failed: " + sdlError());
    }
    return ok();
}

Expected<VsyncMode> GraphicsWindow::setVsync(bool enabled) {
    if (m_context == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the OpenGL context has been destroyed");
    }
    if (!enabled) {
        if (!SDL_GL_SetSwapInterval(0)) {
            return makeError(ErrorCode::GraphicsError, "could not disable vsync: " + sdlError());
        }
        m_vsync = VsyncMode::Off;
        return m_vsync;
    }
    if (SDL_GL_SetSwapInterval(-1)) {
        m_vsync = VsyncMode::Adaptive;
        return m_vsync;
    }
    const std::string adaptiveError = sdlError();
    if (!SDL_GL_SetSwapInterval(1)) {
        return makeError(ErrorCode::GraphicsError, "could not enable vsync: " + sdlError());
    }
    spdlog::debug("Adaptive vsync is unavailable ({}); using regular vsync", adaptiveError);
    m_vsync = VsyncMode::On;
    return m_vsync;
}

GlProcLoader GraphicsWindow::glLoader() const noexcept {
    return &loadGlProc;
}

Expected<GraphicsWindow> createGraphicsWindow(const WindowDesc& window, const GlContextDesc& context) {
    core::assertMainThread("createGraphicsWindow");
    if (window.width <= 0 || window.height <= 0) {
        return makeError(ErrorCode::InvalidArgument, "window size must be positive",
                         fmt::format("{}x{}", window.width, window.height));
    }
    if (auto attributes = applyContextAttributes(context); !attributes) {
        return propagate(attributes.error());
    }

    // Created hidden so that it is shown only once the context exists and the window mode has been applied.
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;
    if (window.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (window.highDpi) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    SDL_Window* handle = SDL_CreateWindow(window.title.c_str(), window.width, window.height, flags);
    if (handle == nullptr) {
        return makeError(ErrorCode::PlatformError, "SDL_CreateWindow failed: " + sdlError(),
                         fmt::format("{}x{} OpenGL window", window.width, window.height));
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(handle);
    if (glContext == nullptr) {
        const std::string error = sdlError();
        SDL_DestroyWindow(handle);
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not create an OpenGL {}.{} core profile context: {}", context.versionMajor,
                                     context.versionMinor, error));
    }
    GraphicsWindow result(handle, glContext, glm::ivec2(window.width, window.height));
    if (!SDL_GL_MakeCurrent(handle, glContext)) {
        return makeError(ErrorCode::GraphicsError, "SDL_GL_MakeCurrent failed: " + sdlError());
    }

    if (auto vsync = result.setVsync(context.vsync); !vsync) {
        spdlog::warn("Vsync configuration failed: {}", vsync.error().toString());
    }
    if (window.mode == WindowMode::Borderless) {
        if (auto fullscreen = result.setFullscreen(true); !fullscreen) {
            spdlog::warn("Starting windowed: {}", fullscreen.error().toString());
        }
    }
    if (!window.hidden) {
        result.show();
    }
    return result;
}

} // namespace rv::platform
