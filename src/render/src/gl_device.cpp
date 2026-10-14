#include <rv/render/gl_device.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/main_thread.hpp>
#include <rv/core/math.hpp>

namespace rv::gl {
namespace {

/// No capability has this value: glEnable must reject it with GL_INVALID_ENUM.
constexpr GLenum kInvalidCapability = 0xFFFF;
/// Upper bound when clearing stale error flags (one per error kind is all GL keeps).
constexpr int kMaxStaleErrors = 16;

std::string glString(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value != nullptr ? std::string(reinterpret_cast<const char*>(value)) : std::string();
}

GLint glInteger(GLenum name) {
    GLint value = 0;
    glGetIntegerv(name, &value);
    return value;
}

/// Name of the first DSA / KHR_debug entry point used by rv_render that the driver did not export, or empty.
std::string_view firstMissingEntryPoint() {
    const std::pair<std::string_view, bool> entryPoints[] = {
        {"glCreateBuffers", glCreateBuffers != nullptr},
        {"glNamedBufferStorage", glNamedBufferStorage != nullptr},
        {"glNamedBufferSubData", glNamedBufferSubData != nullptr},
        {"glGetNamedBufferParameteriv", glGetNamedBufferParameteriv != nullptr},
        {"glCreateVertexArrays", glCreateVertexArrays != nullptr},
        {"glVertexArrayVertexBuffer", glVertexArrayVertexBuffer != nullptr},
        {"glVertexArrayElementBuffer", glVertexArrayElementBuffer != nullptr},
        {"glVertexArrayAttribFormat", glVertexArrayAttribFormat != nullptr},
        {"glVertexArrayAttribBinding", glVertexArrayAttribBinding != nullptr},
        {"glEnableVertexArrayAttrib", glEnableVertexArrayAttrib != nullptr},
        {"glProgramUniformMatrix4fv", glProgramUniformMatrix4fv != nullptr},
        {"glGetNamedFramebufferAttachmentParameteriv", glGetNamedFramebufferAttachmentParameteriv != nullptr},
        {"glObjectLabel", glObjectLabel != nullptr},
        {"glPushDebugGroup", glPushDebugGroup != nullptr},
        {"glPopDebugGroup", glPopDebugGroup != nullptr},
        {"glDebugMessageCallback", glDebugMessageCallback != nullptr},
        {"glDebugMessageControl", glDebugMessageControl != nullptr},
    };
    for (const auto& [name, present] : entryPoints) {
        if (!present) {
            return name;
        }
    }
    return {};
}

DebugSeverity toDebugSeverity(GLenum severity) noexcept {
    switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH:
        return DebugSeverity::High;
    case GL_DEBUG_SEVERITY_MEDIUM:
        return DebugSeverity::Medium;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
        return DebugSeverity::Notification;
    case GL_DEBUG_SEVERITY_LOW:
    default:
        return DebugSeverity::Low;
    }
}

std::string_view sourceName(GLenum source) noexcept {
    switch (source) {
    case GL_DEBUG_SOURCE_API:
        return "api";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
        return "window-system";
    case GL_DEBUG_SOURCE_SHADER_COMPILER:
        return "shader-compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY:
        return "third-party";
    case GL_DEBUG_SOURCE_APPLICATION:
        return "application";
    default:
        return "other-source";
    }
}

std::string_view typeName(GLenum type) noexcept {
    switch (type) {
    case GL_DEBUG_TYPE_ERROR:
        return "error";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        return "deprecated";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        return "undefined-behavior";
    case GL_DEBUG_TYPE_PORTABILITY:
        return "portability";
    case GL_DEBUG_TYPE_PERFORMANCE:
        return "performance";
    case GL_DEBUG_TYPE_MARKER:
        return "marker";
    case GL_DEBUG_TYPE_PUSH_GROUP:
        return "push-group";
    case GL_DEBUG_TYPE_POP_GROUP:
        return "pop-group";
    default:
        return "other";
    }
}

spdlog::level::level_enum logLevelFor(DebugSeverity severity) noexcept {
    switch (severity) {
    case DebugSeverity::High:
        return spdlog::level::err;
    case DebugSeverity::Medium:
        return spdlog::level::warn;
    case DebugSeverity::Low:
    case DebugSeverity::Notification:
        break;
    }
    return spdlog::level::info;
}

/// Drivers often end messages with a newline (and some count the terminator in `length`).
std::string_view trimMessage(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.back() == '\0' || text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    return text;
}

const char* yesNo(bool value) noexcept {
    return value ? "yes" : "no";
}

/// Some drivers (NVIDIA among them) report GL_LINEAR for the default framebuffer even though GL_FRAMEBUFFER_SRGB
/// converts writes to it. Clears one back buffer pixel to linear 0.5 with and without GL_FRAMEBUFFER_SRGB and reads it
/// back: the conversion is real only if both readings match (sRGB-encoded ~188, then ~128), so an undefined readback
/// cannot produce a false positive. Runs once at creation, before the first frame; restores the state it changes.
bool defaultFramebufferConvertsSrgb() {
    constexpr int kTolerance = 2;
    constexpr float kProbeValue = 0.5f;
    const auto expected = [](float encoded) { return static_cast<int>(std::lround(encoded * 255.0f)); };
    const int encodedExpected = expected(core::linearToSrgb(kProbeValue));
    const int linearExpected = expected(kProbeValue);

    GLint scissorBox[4] = {};
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_BACK);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glClearColor(kProbeValue, kProbeValue, kProbeValue, 1.0f);

    const auto clearAndRead = [](bool srgb) {
        std::array<GLubyte, 4> pixel{};
        if (srgb) {
            glEnable(GL_FRAMEBUFFER_SRGB);
        } else {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        return pixel;
    };
    const std::array<GLubyte, 4> converted = clearAndRead(true);
    const std::array<GLubyte, 4> plain = clearAndRead(false);

    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    if (scissorEnabled == GL_FALSE) {
        glDisable(GL_SCISSOR_TEST);
    }
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(converted[channel] - encodedExpected) > kTolerance ||
            std::abs(plain[channel] - linearExpected) > kTolerance) {
            return false;
        }
    }
    return true;
}

void logCapabilities(const GLCapabilities& caps, bool srgbEncodingReported) {
    spdlog::info("GL_VENDOR: {}", caps.vendor);
    spdlog::info("GL_RENDERER: {}", caps.renderer);
    spdlog::info("GL_VERSION: {} (OpenGL {}.{} {} profile{})", caps.version, caps.versionMajor, caps.versionMinor,
                 caps.coreProfile ? "core" : "compatibility", caps.debugContext ? ", debug context" : "");
    spdlog::info("GL_SHADING_LANGUAGE_VERSION: {}", caps.glslVersion);
    spdlog::info("GL limits: max texture size {}, max anisotropy {:g}, UBO offset alignment {} B, max UBO size {} B, "
                 "max combined texture units {}",
                 caps.maxTextureSize, caps.maxAnisotropy, caps.uniformBufferOffsetAlignment, caps.maxUniformBlockSize,
                 caps.maxCombinedTextureImageUnits);
    spdlog::info("GL extensions: KHR_debug {}, ARB_clip_control {}", yesNo(caps.khrDebug), yesNo(caps.arbClipControl));
    if (caps.srgbDefaultFramebuffer && srgbEncodingReported) {
        spdlog::info("Default framebuffer: sRGB (GL_FRAMEBUFFER_SRGB enabled)");
    } else if (caps.srgbDefaultFramebuffer) {
        spdlog::info("Default framebuffer: sRGB (GL_FRAMEBUFFER_SRGB enabled; the driver reports GL_LINEAR encoding "
                     "but a readback confirmed the conversion)");
    } else {
        spdlog::warn("Default framebuffer: not sRGB; GL_FRAMEBUFFER_SRGB stays disabled and shaders writing to it "
                     "must encode sRGB themselves");
    }
}

} // namespace

Expected<std::unique_ptr<GLDevice>> GLDevice::create(GlLoaderFn loader, const GLDeviceConfig& config) {
    core::assertMainThread("GLDevice::create");
    if (loader == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "no OpenGL function loader was provided");
    }
    if (gladLoadGL(loader) == 0) {
        return makeError(ErrorCode::GraphicsError, "could not load the OpenGL entry points (no current context?)");
    }

    GLCapabilities caps;
    caps.vendor = glString(GL_VENDOR);
    caps.renderer = glString(GL_RENDERER);
    caps.version = glString(GL_VERSION);
    caps.glslVersion = glString(GL_SHADING_LANGUAGE_VERSION);
    caps.versionMajor = glInteger(GL_MAJOR_VERSION);
    caps.versionMinor = glInteger(GL_MINOR_VERSION);
    const std::string driver = caps.renderer + ", OpenGL " + caps.version;
    if (caps.versionMajor < 4 || (caps.versionMajor == 4 && caps.versionMinor < 5)) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("OpenGL 4.5 is required but the driver provides {}.{}", caps.versionMajor,
                                     caps.versionMinor),
                         driver);
    }
    caps.coreProfile = (glInteger(GL_CONTEXT_PROFILE_MASK) & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    if (!caps.coreProfile) {
        return makeError(ErrorCode::GraphicsError, "an OpenGL core profile context is required", driver);
    }
    if (const std::string_view missing = firstMissingEntryPoint(); !missing.empty()) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("the driver reports OpenGL {}.{} but does not export {}", caps.versionMajor,
                                     caps.versionMinor, missing),
                         driver);
    }
    caps.debugContext = (glInteger(GL_CONTEXT_FLAGS) & GL_CONTEXT_FLAG_DEBUG_BIT) != 0;
    caps.maxTextureSize = glInteger(GL_MAX_TEXTURE_SIZE);
    caps.uniformBufferOffsetAlignment = glInteger(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT);
    caps.maxUniformBlockSize = glInteger(GL_MAX_UNIFORM_BLOCK_SIZE);
    caps.maxCombinedTextureImageUnits = glInteger(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    caps.khrDebug = GLAD_GL_KHR_debug != 0;
    caps.arbClipControl = GLAD_GL_ARB_clip_control != 0;
    const bool anisotropy = GLAD_GL_ARB_texture_filter_anisotropic != 0 ||
                            GLAD_GL_EXT_texture_filter_anisotropic != 0 ||
                            (caps.versionMajor == 4 && caps.versionMinor >= 6) || caps.versionMajor > 4;
    if (anisotropy) {
        GLfloat maxAnisotropy = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAnisotropy);
        caps.maxAnisotropy = std::max(maxAnisotropy, 1.0f);
    }
    GLint encoding = GL_LINEAR;
    glGetNamedFramebufferAttachmentParameteriv(0, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
    const bool srgbEncodingReported = encoding == GL_SRGB;
    caps.srgbDefaultFramebuffer = srgbEncodingReported || defaultFramebufferConvertsSrgb();
    logCapabilities(caps, srgbEncodingReported);

    // The informational ids are NVIDIA's: other vendors may use the same numbers for messages that matter.
    const bool nvidia = caps.vendor.find("NVIDIA") != std::string::npos;
    const std::span<const std::uint32_t> ignoredIds =
        nvidia ? std::span<const std::uint32_t>(kNvidiaInformationalIds) : std::span<const std::uint32_t>();

    std::unique_ptr<GLDevice> device(new GLDevice(std::move(caps), ignoredIds));
    if (config.debugOutput) {
        device->enableDebugOutput();
    }
    device->applyDefaultState();
    return device;
}

GLDevice::GLDevice(GLCapabilities caps, std::span<const std::uint32_t> ignoredDebugIds)
    : m_caps(std::move(caps)), m_debugFilter(ignoredDebugIds) {
    // The stack starts with the default group; the message length limit includes the terminator.
    m_maxDebugGroupDepth = static_cast<std::uint32_t>(std::max(glInteger(GL_MAX_DEBUG_GROUP_STACK_DEPTH) - 1, 0));
    m_maxDebugMessageLength = static_cast<std::uint32_t>(std::max(glInteger(GL_MAX_DEBUG_MESSAGE_LENGTH) - 1, 0));
}

GLDevice::~GLDevice() {
    if (!m_debugOutput) {
        return;
    }
    glDebugMessageCallback(nullptr, nullptr);
    glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDisable(GL_DEBUG_OUTPUT);
    try {
        logDebugSummary();
    } catch (...) {
        // Logging must never turn shutdown into a crash.
    }
}

void GLDevice::enableDebugOutput() {
    glEnable(GL_DEBUG_OUTPUT);
    // Synchronous delivery: the callback runs inside the offending call, on this thread, with a useful call stack.
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(&GLDevice::onDebugMessage, this);
    // Notifications (including debug group markers) would be dropped by the filter anyway: stop them at the source.
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
    m_debugOutput = true;
    if (!m_caps.debugContext) {
        spdlog::warn("GL debug output enabled on a non-debug context: the driver may report fewer messages");
    }
    spdlog::info("GL debug output enabled: synchronous, notifications and known informational ids dropped, "
                 "repeats suppressed after {} occurrences",
                 m_debugFilter.repeatLimit());
}

void GLDevice::applyDefaultState() {
    m_state.invalidate();
    m_state.setDepthTest(true);
    m_state.setDepthFunc(GL_LESS);
    m_state.setDepthWrite(true);
    m_state.setCullFace(true);
    m_state.setCullMode(GL_BACK);
    m_state.setBlend(false);
    m_state.setBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_state.setFramebufferSrgb(m_caps.srgbDefaultFramebuffer);
    m_state.setPolygonMode(GL_FILL);
}

void GLDevice::beginFrame(glm::ivec2 framebufferSize, const glm::vec4& clearColorLinear) {
    const glm::ivec2 size = glm::max(framebufferSize, glm::ivec2(0));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_state.setViewport(glm::ivec4(0, 0, size.x, size.y));
    m_state.setFramebufferSrgb(m_caps.srgbDefaultFramebuffer);
    m_state.setDepthWrite(true);
    const glm::vec3 linear(clearColorLinear);
    const glm::vec3 color = m_caps.srgbDefaultFramebuffer ? linear : core::linearToSrgb(linear);
    glClearColor(color.r, color.g, color.b, clearColorLinear.a);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

std::uint64_t GLDevice::debugMessageCount(DebugSeverity atLeast) const noexcept {
    return m_debugFilter.count(atLeast);
}

void GLDevice::pushDebugGroup(std::string_view name) {
    if (m_debugGroupDepth >= m_maxDebugGroupDepth) {
        ++m_skippedDebugGroups;
        return;
    }
    const std::string_view label = name.substr(0, m_maxDebugMessageLength);
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, static_cast<GLsizei>(label.size()), label.data());
    ++m_debugGroupDepth;
}

void GLDevice::popDebugGroup() {
    if (m_skippedDebugGroups > 0) {
        --m_skippedDebugGroups;
        return;
    }
    if (m_debugGroupDepth == 0) {
        spdlog::warn("popDebugGroup() without a matching pushDebugGroup() ignored");
        return;
    }
    glPopDebugGroup();
    --m_debugGroupDepth;
}

DebugSelfTestResult GLDevice::runDebugSelfTest() {
    DebugSelfTestResult result;
    result.debugOutputActive = m_debugOutput;
    // Clear stale error flags so that the check below only sees the provoked error.
    int stale = 0;
    while (glGetError() != GL_NO_ERROR && ++stale < kMaxStaleErrors) {
    }
    const std::uint64_t messagesBefore = m_debugFilter.count(DebugSeverity::Low);
    const std::uint64_t highBefore = m_debugFilter.count(DebugSeverity::High);
    glEnable(kInvalidCapability);
    result.errorFlagRaised = glGetError() == GL_INVALID_ENUM;
    result.messagesCaptured = m_debugFilter.count(DebugSeverity::Low) - messagesBefore;
    result.highSeverityCaptured = m_debugFilter.count(DebugSeverity::High) - highBefore;
    return result;
}

void GLAD_API_PTR GLDevice::onDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                           const GLchar* message, const void* userParam) {
    if (userParam == nullptr || message == nullptr) {
        return;
    }
    try {
        // userParam is the device registered in enableDebugOutput(); KHR_debug passes it back as const void*.
        auto* device = static_cast<GLDevice*>(const_cast<void*>(userParam));
        const std::size_t size = length >= 0 ? static_cast<std::size_t>(length) : std::strlen(message);
        device->handleDebugMessage(source, type, id, severity, std::string_view(message, size));
    } catch (...) {
        // Called from the driver: an exception must never escape.
    }
}

void GLDevice::handleDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, std::string_view message) {
    const DebugSeverity level = toDebugSeverity(severity);
    const DebugMessageKey key{source, type, id};
    switch (m_debugFilter.classify(key, level)) {
    case DebugMessageAction::Drop:
    case DebugMessageAction::Suppress:
        return;
    case DebugMessageAction::Log:
        spdlog::log(logLevelFor(level), "GL {} {} [{}] #{}: {}", sourceName(source), typeName(type), toString(level),
                    id, trimMessage(message));
        return;
    case DebugMessageAction::StartSuppressing:
        spdlog::log(
            logLevelFor(level), "GL {} {} [{}] #{} repeated more than {} times, further occurrences suppressed: {}",
            sourceName(source), typeName(type), toString(level), id, m_debugFilter.repeatLimit(), trimMessage(message));
        return;
    }
}

void GLDevice::logDebugSummary() const {
    const std::uint64_t high = m_debugFilter.count(DebugSeverity::High);
    const std::uint64_t mediumOrMore = m_debugFilter.count(DebugSeverity::Medium);
    const std::uint64_t lowOrMore = m_debugFilter.count(DebugSeverity::Low);
    spdlog::info("GL debug output summary: {} high, {} medium, {} low severity message(s)", high, mediumOrMore - high,
                 lowOrMore - mediumOrMore);
    // entry.count includes the occurrence that was logged with the "repeated more than N times" notice.
    for (const DebugMessageFilter::SuppressedMessages& entry : m_debugFilter.suppressed()) {
        spdlog::info("GL {} {} #{}: {} occurrence(s) beyond the repeat limit of {}", sourceName(entry.key.source),
                     typeName(entry.key.type), entry.key.id, entry.count, m_debugFilter.repeatLimit());
    }
}

ScopedDebugGroup::ScopedDebugGroup(GLDevice& device, std::string_view name) : m_device(device) {
    m_device.pushDebugGroup(name);
}

ScopedDebugGroup::~ScopedDebugGroup() {
    m_device.popDebugGroup();
}

} // namespace rv::gl
