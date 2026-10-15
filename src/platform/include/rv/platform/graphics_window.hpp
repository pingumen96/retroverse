#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>

struct SDL_Window;
struct SDL_GLContextState;
union SDL_Event;

namespace rv::platform {

/// OpenGL entry point and loader signatures; identical to glad's GLADapiproc / GLADloadfunc and to rv::gl::GlLoaderFn.
using GlProc = void (*)();
using GlProcLoader = GlProc (*)(const char* name);

/// How the window covers the screen (specification §5.3).
enum class WindowMode : std::uint8_t {
    Windowed,   ///< decorated, movable window of WindowDesc::width x WindowDesc::height
    Borderless, ///< borderless fullscreen at the desktop resolution (no display mode change)
};

/// Window creation parameters. Sizes are in window coordinates (SDL "screen" units); see
/// GraphicsWindow::drawableSize() for the size in pixels.
struct WindowDesc {
    std::string title = "Retroverse";
    int width = 1280;
    int height = 720;
    bool resizable = true;
    bool highDpi = true; ///< request a full-resolution drawable on high pixel density displays
    bool hidden = false; ///< never show the window (automated runs); rendering still works
    WindowMode mode = WindowMode::Windowed;
};

/// OpenGL context and default framebuffer requested from the driver. The core profile is always requested; every
/// attribute is applied before the window is created.
struct GlContextDesc {
    int versionMajor = 4;
    int versionMinor = 5;
    bool debug = false;          ///< debug context (KHR_debug delivers every message)
    bool srgbFramebuffer = true; ///< sRGB-capable default framebuffer (a request: verify with the GL device)
    int depthBits = 24;
    int stencilBits = 8;
    bool doubleBuffer = true;
    bool vsync = true; ///< see GraphicsWindow::setVsync()
};

/// Swap interval in effect.
enum class VsyncMode : std::uint8_t {
    Off,      ///< swap interval 0
    On,       ///< swap interval 1
    Adaptive, ///< swap interval -1: synchronized, but late frames are presented immediately (tearing over stutter)
};

[[nodiscard]] std::string_view toString(VsyncMode mode) noexcept;

/// An SDL window with its OpenGL context, current on the creating (main) thread. Move-only RAII: destruction
/// releases and destroys the context first, then the window.
class GraphicsWindow {
public:
    GraphicsWindow(GraphicsWindow&& other) noexcept;
    GraphicsWindow& operator=(GraphicsWindow&& other) noexcept;
    GraphicsWindow(const GraphicsWindow&) = delete;
    GraphicsWindow& operator=(const GraphicsWindow&) = delete;
    ~GraphicsWindow();

    /// Underlying SDL window (owned by this object).
    [[nodiscard]] SDL_Window* handle() const noexcept { return m_window; }
    /// SDL window id, as found in SDL_WindowEvent::windowID.
    [[nodiscard]] std::uint32_t id() const noexcept;

    /// Default framebuffer size in pixels: the viewport size to render with.
    [[nodiscard]] glm::ivec2 drawableSize() const noexcept;
    /// Client area size in window coordinates.
    [[nodiscard]] glm::ivec2 windowSize() const noexcept;
    /// Content scale of the display the window is on (1.0 at 100% scaling).
    [[nodiscard]] float displayScale() const noexcept;

    /// Sets the title bar text (UTF-8).
    void setTitle(std::string_view title);
    /// Makes a window created hidden visible.
    void show();

    /// Enters borderless fullscreen at the desktop resolution, or returns to the windowed size it had before.
    [[nodiscard]] Status setFullscreen(bool fullscreen);
    /// Alt+Enter behavior: switches between borderless fullscreen and the previous windowed size.
    [[nodiscard]] Status toggleBorderlessFullscreen();
    [[nodiscard]] bool isFullscreen() const noexcept;
    [[nodiscard]] bool isMinimized() const noexcept;

    /// Presents the back buffer (SDL_GL_SwapWindow); blocks for vsync when enabled.
    [[nodiscard]] Status swap();
    /// Enables vsync (adaptive when the driver supports it, regular otherwise) or disables it. Returns the mode in
    /// effect; on error the previous mode stays active.
    [[nodiscard]] Expected<VsyncMode> setVsync(bool enabled);
    [[nodiscard]] VsyncMode vsync() const noexcept { return m_vsync; }

    /// OpenGL entry point loader for this window's context (wraps SDL_GL_GetProcAddress); pass it to gladLoadGL
    /// through rv::gl::GLDevice::create.
    [[nodiscard]] GlProcLoader glLoader() const noexcept;

private:
    friend Expected<GraphicsWindow> createGraphicsWindow(const WindowDesc& window, const GlContextDesc& context);

    GraphicsWindow(SDL_Window* window, SDL_GLContextState* context, glm::ivec2 windowedSize) noexcept;
    void destroy() noexcept;

    SDL_Window* m_window = nullptr;
    SDL_GLContextState* m_context = nullptr;
    glm::ivec2 m_windowedSize{0}; ///< window size to restore when leaving fullscreen
    VsyncMode m_vsync = VsyncMode::Off;
};

/// Creates the window and an OpenGL core profile context (GlContextDesc), makes the context current, applies vsync
/// and the window mode, and shows the window unless WindowDesc::hidden. Requires a live SdlPlatform; main thread only.
/// Fails with GraphicsError (and SDL_GetError() in the message) when the requested context version is unavailable.
[[nodiscard]] Expected<GraphicsWindow> createGraphicsWindow(const WindowDesc& window, const GlContextDesc& context);

/// True for the press of the fullscreen shortcut, Alt+Enter: a key-down event (not a repeat) of the main or keypad
/// Enter key with either Alt key held. Pure function of the event, no SDL call.
[[nodiscard]] bool isFullscreenToggle(const SDL_Event& event) noexcept;

} // namespace rv::platform
