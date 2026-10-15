#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <glad/gl.h>
#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/debug_message_filter.hpp>
#include <rv/render/render_state_cache.hpp>

namespace rv::gl {

class Framebuffer;

/// OpenGL entry point loader: returns the address of the named function for the current context. Same signature as
/// glad's GLADloadfunc; the platform layer provides one (rv::platform::GraphicsWindow::glLoader) so that this module
/// never includes SDL.
using GlProc = void (*)();
using GlLoaderFn = GlProc (*)(const char* name);

struct GLDeviceConfig {
    /// Install a synchronous GL_DEBUG_OUTPUT callback that routes driver messages to the log by severity. Pair it with
    /// a debug context: without one, drivers may report fewer messages.
    bool debugOutput = false;
};

/// Facts about the context, queried once when the device is created.
struct GLCapabilities {
    std::string vendor;      ///< GL_VENDOR
    std::string renderer;    ///< GL_RENDERER
    std::string version;     ///< GL_VERSION
    std::string glslVersion; ///< GL_SHADING_LANGUAGE_VERSION
    int versionMajor = 0;
    int versionMinor = 0;
    bool coreProfile = false;
    bool debugContext = false;
    int maxTextureSize = 0;               ///< texels per side
    float maxAnisotropy = 1.0f;           ///< 1 when anisotropic filtering is unavailable
    int uniformBufferOffsetAlignment = 0; ///< bytes
    int maxUniformBlockSize = 0;          ///< bytes
    int maxCombinedTextureImageUnits = 0;
    bool khrDebug = false;       ///< GL_KHR_debug advertised (debug output is also core since OpenGL 4.3)
    bool arbClipControl = false; ///< GL_ARB_clip_control advertised (also core since OpenGL 4.5)
    /// GL_FRAMEBUFFER_SRGB converts linear output written to the default framebuffer. Taken from the back buffer's
    /// GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING; when the driver reports GL_LINEAR (NVIDIA does even for sRGB-capable
    /// pixel formats), a one-pixel clear and readback at creation checks whether the conversion happens anyway.
    /// Informational: frames are rendered into sRGB Framebuffers and copied to the window byte for byte
    /// (Framebuffer::blitToDefault), which displays correctly either way.
    bool srgbDefaultFramebuffer = false;
};

/// Outcome of GLDevice::runDebugSelfTest().
struct DebugSelfTestResult {
    bool errorFlagRaised = false;            ///< glGetError() returned GL_INVALID_ENUM after the invalid call
    bool debugOutputActive = false;          ///< the debug callback is installed
    std::uint64_t messagesCaptured = 0;      ///< counted debug messages produced by the invalid call
    std::uint64_t highSeverityCaptured = 0;  ///< the High-severity part of messagesCaptured
};

/// The OpenGL 4.5 core device: entry points, capabilities, debug output, frame setup and the render state cache.
///
/// It never touches SDL: the window and context belong to rv::platform::GraphicsWindow, which must outlive the device
/// and keep its context current on the main thread. All member functions are main-thread only. Not movable: the debug
/// callback refers to the device by address.
class GLDevice {
public:
    /// Loads the GL entry points through `loader` (the context must be current on the calling thread), requires an
    /// OpenGL 4.5 core profile context, logs vendor, renderer, version and capabilities, installs the debug callback
    /// when requested and puts the GL state into the defaults documented on state().
    [[nodiscard]] static Expected<std::unique_ptr<GLDevice>> create(GlLoaderFn loader, const GLDeviceConfig& config);

    GLDevice(const GLDevice&) = delete;
    GLDevice& operator=(const GLDevice&) = delete;
    GLDevice(GLDevice&&) = delete;
    GLDevice& operator=(GLDevice&&) = delete;
    /// Removes the debug callback and logs a summary of the debug messages (counts, suppressed repeats).
    ~GLDevice();

    [[nodiscard]] const GLCapabilities& caps() const noexcept { return m_caps; }

    /// Fixed-function state. Defaults after create(): depth test on with GL_LESS and depth writes, back faces culled,
    /// blending off with (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), filled polygons, color writes on, polygon offset off,
    /// GL_FRAMEBUFFER_SRGB as in GLCapabilities::srgbDefaultFramebuffer.
    [[nodiscard]] RenderStateCache& state() noexcept { return m_state; }

    /// Starts rendering into `target`: binds it, sets the viewport to (0, 0, target.size()), enables
    /// GL_FRAMEBUFFER_SRGB (linear shader output is encoded to sRGB by sRGB color attachments) and clears color to
    /// `clearColorLinear` (linear RGBA), depth to 1 and stencil to 0. Depth writes are enabled because clears honor
    /// the depth mask. An empty target is left unbound.
    void beginPass(const Framebuffer& target, const glm::vec4& clearColorLinear);

    /// True when the debug callback is installed (GLDeviceConfig::debugOutput).
    [[nodiscard]] bool debugOutputEnabled() const noexcept { return m_debugOutput; }
    /// Debug messages received with severity >= `atLeast`, de-duplicated repeats included, dropped ones (notifications,
    /// known informational ids) excluded. Always 0 without debug output.
    [[nodiscard]] std::uint64_t debugMessageCount(DebugSeverity atLeast) const noexcept;

    /// Opens a named debug group (visible in RenderDoc / Nsight captures and in debug messages). Names longer than the
    /// GL limit are truncated. Groups nested deeper than GL allows are skipped, keeping push/pop balanced.
    void pushDebugGroup(std::string_view name);
    /// Closes the innermost group opened by pushDebugGroup(); an unmatched pop is logged and ignored.
    void popDebugGroup();

    /// For --gl-selftest: calls glEnable(0xFFFF), which must raise GL_INVALID_ENUM, and reports whether the error was
    /// flagged and delivered to the debug callback. The provoked message is logged and counted like any other.
    [[nodiscard]] DebugSelfTestResult runDebugSelfTest();

private:
    GLDevice(GLCapabilities caps, std::span<const std::uint32_t> ignoredDebugIds);

    static void GLAD_API_PTR onDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                            const GLchar* message, const void* userParam);
    void handleDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, std::string_view message);
    void enableDebugOutput();
    void applyDefaultState();
    void logDebugSummary() const;

    GLCapabilities m_caps;
    RenderStateCache m_state;
    DebugMessageFilter m_debugFilter;
    bool m_debugOutput = false;
    std::uint32_t m_maxDebugGroupDepth = 0;   // pushes allowed on top of the default group
    std::uint32_t m_maxDebugMessageLength = 0; // characters, terminator excluded
    std::uint32_t m_debugGroupDepth = 0;
    std::uint32_t m_skippedDebugGroups = 0;
};

/// RAII debug group: GLDevice::pushDebugGroup() on construction, popDebugGroup() on destruction.
class ScopedDebugGroup {
public:
    ScopedDebugGroup(GLDevice& device, std::string_view name);
    ~ScopedDebugGroup();
    ScopedDebugGroup(const ScopedDebugGroup&) = delete;
    ScopedDebugGroup& operator=(const ScopedDebugGroup&) = delete;

private:
    GLDevice& m_device;
};

} // namespace rv::gl
