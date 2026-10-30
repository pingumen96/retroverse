#include <rv/xr/openxr_runtime.hpp>

// OpenXR's Win32 and OpenGL bindings need <windows.h> (HDC, HGLRC); NOMINMAX and WIN32_LEAN_AND_MEAN come from
// rv_set_target_defaults, so IUnknown (in the Win32 platform's declarations) needs <unknwn.h> of its own.
#include <windows.h>
#include <unknwn.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>
#include <rv/xr/swapchain_format.hpp>

namespace rv::xr {
namespace {

using namespace std::chrono_literals;

constexpr const char* kOpenGlExtension = "XR_KHR_opengl_enable";
constexpr const char* kRefreshRateExtension = "XR_FB_display_refresh_rate";
constexpr const char* kLocalFloorExtension = "XR_EXT_local_floor";
constexpr XrViewConfigurationType kStereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
/// LOCAL_FLOOR: core in OpenXR 1.1, XR_EXT_local_floor in 1.0 (the same value).
constexpr auto kLocalFloorSpace = static_cast<XrReferenceSpaceType>(1000426000);
/// The eye height LOCAL's origin is assumed to be at.
constexpr float kAssumedEyeHeight = 1.6f;
constexpr int kMinEyeSide = 16;

std::string resultName(XrInstance instance, XrResult result) {
    char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
        return buffer;
    }
    // Without an instance xrResultToString cannot run: the results the loader gives before one exists.
    switch (result) {
    case XR_ERROR_RUNTIME_UNAVAILABLE:
        return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_RUNTIME_FAILURE:
        return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_EXTENSION_NOT_PRESENT:
        return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_API_VERSION_UNSUPPORTED:
        return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED:
        return "XR_ERROR_INITIALIZATION_FAILED";
    default:
        return fmt::format("XrResult {}", static_cast<int>(result));
    }
}

Error xrError(XrInstance instance, XrResult result, std::string_view call, ErrorCode code = ErrorCode::PlatformError) {
    return Error{code, fmt::format("{} failed: {}", call, resultName(instance, result)), "openxr"};
}

std::string versionString(XrVersion version) {
    return fmt::format("{}.{}.{}", XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), XR_VERSION_PATCH(version));
}

SessionState toState(XrSessionState state) noexcept {
    switch (state) {
    case XR_SESSION_STATE_IDLE:
        return SessionState::Idle;
    case XR_SESSION_STATE_READY:
        return SessionState::Ready;
    case XR_SESSION_STATE_SYNCHRONIZED:
        return SessionState::Synchronized;
    case XR_SESSION_STATE_VISIBLE:
        return SessionState::Visible;
    case XR_SESSION_STATE_FOCUSED:
        return SessionState::Focused;
    case XR_SESSION_STATE_STOPPING:
        return SessionState::Stopping;
    case XR_SESSION_STATE_LOSS_PENDING:
        return SessionState::LossPending;
    case XR_SESSION_STATE_EXITING:
        return SessionState::Exiting;
    default:
        return SessionState::Unknown;
    }
}

std::string spaceName(XrReferenceSpaceType type) {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return "view";
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "local";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "stage";
    default:
        return type == kLocalFloorSpace ? "local-floor" : fmt::format("{}", static_cast<int>(type));
    }
}

/// A two-call enumeration (count, then fill), `make` building an element of the right XrStructureType.
template <class T, class Call, class Make>
XrResult enumerate(std::vector<T>& out, Call&& call, Make&& make) {
    std::uint32_t count = 0;
    XrResult result = call(0u, &count, static_cast<T*>(nullptr));
    if (XR_FAILED(result)) {
        return result;
    }
    out.assign(count, make());
    result = call(count, &count, out.data());
    out.resize(count);
    return result;
}

template <class T>
T typed(XrStructureType type) {
    T value{};
    value.type = type;
    return value;
}

Pose toPose(const XrPosef& pose) noexcept {
    return Pose{glm::quat(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z),
                glm::vec3(pose.position.x, pose.position.y, pose.position.z)};
}

class OpenXrRuntime final : public Runtime {
public:
    OpenXrRuntime() = default;
    ~OpenXrRuntime() override;

    [[nodiscard]] Status createInstance(const OpenXrOptions& options);
    [[nodiscard]] Status findSystem(std::chrono::milliseconds wait);
    [[nodiscard]] Status createSession(const OpenXrOptions& options);

    [[nodiscard]] const RuntimeDescription& description() const noexcept override { return m_description; }
    [[nodiscard]] std::optional<RuntimeEvent> pollEvent() override;
    [[nodiscard]] Status beginSession() override;
    [[nodiscard]] Status endSession() override;
    void requestExit() override;
    [[nodiscard]] Expected<FrameTiming> waitFrame() override;
    [[nodiscard]] Status beginFrame() override;
    [[nodiscard]] Expected<bool> locateViews(std::int64_t displayTimeNs, EyeViews& eyes) override;
    [[nodiscard]] Expected<std::uint32_t> acquireImage(int eye) override;
    [[nodiscard]] Status releaseImage(int eye) override;
    [[nodiscard]] Status endFrame(const FrameTiming& timing, bool withViews) override;
    [[nodiscard]] std::span<const std::uint32_t> images(int eye) const override;
    [[nodiscard]] std::optional<float> currentRefreshRate() const override;

private:
    template <class Pfn>
    void load(const char* name, Pfn& pfn) const {
        PFN_xrVoidFunction function = nullptr;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(m_instance, name, &function))) {
            pfn = reinterpret_cast<Pfn>(function);
        }
    }

    RuntimeDescription m_description;
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_space = XR_NULL_HANDLE;
    XrEnvironmentBlendMode m_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    std::array<XrSwapchain, 2> m_swapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrViewConfigurationView, 2> m_configViews{};
    std::array<XrView, 2> m_views{};
    std::array<std::vector<std::uint32_t>, 2> m_images;
    bool m_refreshRateExtension = false;
    bool m_localFloorExtension = false;
    PFN_xrGetOpenGLGraphicsRequirementsKHR m_getGraphicsRequirements = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB m_enumerateRefreshRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB m_getRefreshRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB m_requestRefreshRate = nullptr;
};

OpenXrRuntime::~OpenXrRuntime() {
    for (XrSwapchain& swapchain : m_swapchains) {
        if (swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain);
        }
    }
    if (m_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_space);
    }
    if (m_session != XR_NULL_HANDLE) {
        xrDestroySession(m_session);
    }
    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
    }
}

Status OpenXrRuntime::createInstance(const OpenXrOptions& options) {
    m_description.backend = "openxr";
    if (!options.runtimeJson.empty()) {
        // Read by the loader when it starts (here): this process alone uses the runtime, the system's choice stays.
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", options.runtimeJson.c_str());
        spdlog::info("OpenXR runtime manifest: {}", core::pathToUtf8(options.runtimeJson));
    }

    std::vector<XrExtensionProperties> extensions;
    const XrResult listed = enumerate(
        extensions,
        [](std::uint32_t capacity, std::uint32_t* count, XrExtensionProperties* out) {
            return xrEnumerateInstanceExtensionProperties(nullptr, capacity, count, out);
        },
        [] { return typed<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES); });
    if (XR_FAILED(listed)) {
        return tl::unexpected(xrError(XR_NULL_HANDLE, listed, "xrEnumerateInstanceExtensionProperties (no runtime?)",
                                      ErrorCode::Unavailable));
    }
    const auto offers = [&](std::string_view name) {
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const XrExtensionProperties& e) { return name == e.extensionName; });
    };
    if (!offers(kOpenGlExtension)) {
        return makeError(ErrorCode::Unsupported, "the runtime offers no XR_KHR_opengl_enable", "openxr");
    }

    std::vector<const char*> enabled{kOpenGlExtension};
    if (offers(kRefreshRateExtension)) {
        enabled.push_back(kRefreshRateExtension);
        m_refreshRateExtension = true;
    }
    if (offers(kLocalFloorExtension)) {
        enabled.push_back(kLocalFloorExtension);
        m_localFloorExtension = true;
    }
    for (const char* name : enabled) {
        m_description.extensions.emplace_back(name);
    }

    XrInstanceCreateInfo create = typed<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    std::strncpy(create.applicationInfo.applicationName, "Retroverse", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create.applicationInfo.engineName, "Retroverse", XR_MAX_ENGINE_NAME_SIZE - 1);
    create.applicationInfo.applicationVersion = 1;
    create.applicationInfo.engineVersion = 1;
    // 1.0: what every PC runtime accepts; LOCAL_FLOOR then comes from XR_EXT_local_floor.
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    create.enabledExtensionNames = enabled.data();
    if (const XrResult result = xrCreateInstance(&create, &m_instance); XR_FAILED(result)) {
        return tl::unexpected(xrError(XR_NULL_HANDLE, result, "xrCreateInstance"));
    }

    XrInstanceProperties properties = typed<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &properties))) {
        m_description.runtime = properties.runtimeName;
        m_description.runtimeVersion = versionString(properties.runtimeVersion);
    }
    spdlog::info("OpenXR runtime: {} {} (loader {}), extensions {}", m_description.runtime,
                 m_description.runtimeVersion, versionString(XR_CURRENT_API_VERSION),
                 fmt::join(m_description.extensions, ", "));

    load("xrGetOpenGLGraphicsRequirementsKHR", m_getGraphicsRequirements);
    if (m_refreshRateExtension) {
        load("xrEnumerateDisplayRefreshRatesFB", m_enumerateRefreshRates);
        load("xrGetDisplayRefreshRateFB", m_getRefreshRate);
        load("xrRequestDisplayRefreshRateFB", m_requestRefreshRate);
    }
    if (m_getGraphicsRequirements == nullptr) {
        return makeError(ErrorCode::Unsupported, "xrGetOpenGLGraphicsRequirementsKHR is missing", "openxr");
    }
    return ok();
}

Status OpenXrRuntime::findSystem(std::chrono::milliseconds wait) {
    XrSystemGetInfo info = typed<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const auto start = std::chrono::steady_clock::now();
    for (int attempt = 1;; ++attempt) {
        const XrResult result = xrGetSystem(m_instance, &info, &m_system);
        if (XR_SUCCEEDED(result)) {
            break;
        }
        const bool waiting = result == XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        if (!waiting || std::chrono::steady_clock::now() - start >= wait) {
            return tl::unexpected(xrError(m_instance, result, "xrGetSystem (headset connected, Link active?)",
                                          waiting ? ErrorCode::Unavailable : ErrorCode::PlatformError));
        }
        if (attempt == 1) {
            spdlog::info("No headset yet (start Link on the headset); waiting up to {} s",
                         std::chrono::duration_cast<std::chrono::seconds>(wait).count());
        }
        std::this_thread::sleep_for(500ms);
    }

    XrSystemProperties properties = typed<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_system, &properties))) {
        m_description.system = properties.systemName;
    }

    std::vector<XrViewConfigurationType> configurations;
    enumerate(
        configurations,
        [&](std::uint32_t capacity, std::uint32_t* count, XrViewConfigurationType* out) {
            return xrEnumerateViewConfigurations(m_instance, m_system, capacity, count, out);
        },
        [] { return XrViewConfigurationType{}; });
    if (std::find(configurations.begin(), configurations.end(), kStereo) == configurations.end()) {
        return makeError(ErrorCode::Unsupported, "the headset offers no primary stereo view configuration", "openxr");
    }

    std::vector<XrViewConfigurationView> views;
    const XrResult viewsListed = enumerate(
        views,
        [&](std::uint32_t capacity, std::uint32_t* count, XrViewConfigurationView* out) {
            return xrEnumerateViewConfigurationViews(m_instance, m_system, kStereo, capacity, count, out);
        },
        [] { return typed<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW); });
    if (XR_FAILED(viewsListed) || views.size() != 2) {
        return makeError(ErrorCode::Unsupported, "the stereo configuration does not have two views", "openxr");
    }
    m_configViews = {views[0], views[1]};
    m_description.recommendedEyeSize = glm::ivec2(static_cast<int>(views[0].recommendedImageRectWidth),
                                                  static_cast<int>(views[0].recommendedImageRectHeight));

    std::vector<XrEnvironmentBlendMode> blendModes;
    enumerate(
        blendModes,
        [&](std::uint32_t capacity, std::uint32_t* count, XrEnvironmentBlendMode* out) {
            return xrEnumerateEnvironmentBlendModes(m_instance, m_system, kStereo, capacity, count, out);
        },
        [] { return XrEnvironmentBlendMode{}; });
    const bool opaque =
        std::find(blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != blendModes.end();
    m_blendMode = opaque || blendModes.empty() ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : blendModes.front();

    // Required before xrCreateSession.
    XrGraphicsRequirementsOpenGLKHR requirements =
        typed<XrGraphicsRequirementsOpenGLKHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR);
    if (const XrResult result = m_getGraphicsRequirements(m_instance, m_system, &requirements); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrGetOpenGLGraphicsRequirementsKHR"));
    }
    if (XR_MAKE_VERSION(4, 5, 0) < requirements.minApiVersionSupported) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("the runtime needs OpenGL {} or later",
                                     versionString(requirements.minApiVersionSupported)),
                         "openxr");
    }
    spdlog::info("Headset: {}, eyes {}x{} recommended", m_description.system, m_description.recommendedEyeSize.x,
                 m_description.recommendedEyeSize.y);
    return ok();
}

Status OpenXrRuntime::createSession(const OpenXrOptions& options) {
    XrGraphicsBindingOpenGLWin32KHR binding =
        typed<XrGraphicsBindingOpenGLWin32KHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR);
    binding.hDC = wglGetCurrentDC();
    binding.hGLRC = wglGetCurrentContext();
    if (binding.hDC == nullptr || binding.hGLRC == nullptr) {
        return makeError(ErrorCode::GraphicsError, "no OpenGL context is current on this thread", "openxr");
    }
    XrSessionCreateInfo create = typed<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    create.next = &binding;
    create.systemId = m_system;
    if (const XrResult result = xrCreateSession(m_instance, &create, &m_session); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrCreateSession"));
    }

    // The reference space.
    std::vector<XrReferenceSpaceType> spaces;
    enumerate(
        spaces,
        [&](std::uint32_t capacity, std::uint32_t* count, XrReferenceSpaceType* out) {
            return xrEnumerateReferenceSpaces(m_session, capacity, count, out);
        },
        [] { return XrReferenceSpaceType{}; });
    const auto offered = [&](XrReferenceSpaceType type) {
        return std::find(spaces.begin(), spaces.end(), type) != spaces.end();
    };
    XrReferenceSpaceType chosen = XR_REFERENCE_SPACE_TYPE_LOCAL;
    switch (options.space) {
    case SpaceChoice::Auto:
        chosen = offered(kLocalFloorSpace)                 ? kLocalFloorSpace
                 : offered(XR_REFERENCE_SPACE_TYPE_STAGE) ? XR_REFERENCE_SPACE_TYPE_STAGE
                                                          : XR_REFERENCE_SPACE_TYPE_LOCAL;
        break;
    case SpaceChoice::LocalFloor:
        chosen = kLocalFloorSpace;
        break;
    case SpaceChoice::Stage:
        chosen = XR_REFERENCE_SPACE_TYPE_STAGE;
        break;
    case SpaceChoice::Local:
        chosen = XR_REFERENCE_SPACE_TYPE_LOCAL;
        break;
    }
    if (!offered(chosen)) {
        return makeError(ErrorCode::Unsupported, fmt::format("the runtime offers no {} space", spaceName(chosen)),
                         "openxr");
    }
    XrReferenceSpaceCreateInfo spaceInfo = typed<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    spaceInfo.referenceSpaceType = chosen;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (const XrResult result = xrCreateReferenceSpace(m_session, &spaceInfo, &m_space); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrCreateReferenceSpace"));
    }
    m_description.space = spaceName(chosen);
    m_description.floorOffset = chosen == XR_REFERENCE_SPACE_TYPE_LOCAL ? kAssumedEyeHeight : 0.0f;

    // The eyes' format and size.
    std::vector<std::int64_t> formats;
    enumerate(
        formats,
        [&](std::uint32_t capacity, std::uint32_t* count, std::int64_t* out) {
            return xrEnumerateSwapchainFormats(m_session, capacity, count, out);
        },
        [] { return std::int64_t{0}; });
    const auto format = chooseColorFormat(formats);
    if (!format) {
        return makeError(ErrorCode::Unsupported, "the runtime offers neither GL_SRGB8_ALPHA8 nor GL_RGBA8", "openxr");
    }
    m_description.format = glFormatName(format->format);
    m_description.srgb = format->srgb;

    const XrViewConfigurationView& left = m_configViews[0];
    const auto scaled = [&](std::uint32_t recommended, std::uint32_t maximum) {
        const double side = std::round(static_cast<double>(recommended) * options.renderScale);
        return static_cast<int>(std::clamp(side, static_cast<double>(kMinEyeSide), static_cast<double>(maximum)));
    };
    m_description.renderScale = options.renderScale;
    m_description.eyeSize = glm::ivec2(scaled(left.recommendedImageRectWidth, left.maxImageRectWidth),
                                       scaled(left.recommendedImageRectHeight, left.maxImageRectHeight));

    for (std::size_t eye = 0; eye < m_swapchains.size(); ++eye) {
        XrSwapchainCreateInfo info = typed<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = format->format;
        info.sampleCount = 1;
        info.width = static_cast<std::uint32_t>(m_description.eyeSize.x);
        info.height = static_cast<std::uint32_t>(m_description.eyeSize.y);
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (const XrResult result = xrCreateSwapchain(m_session, &info, &m_swapchains[eye]); XR_FAILED(result)) {
            return tl::unexpected(xrError(m_instance, result, "xrCreateSwapchain"));
        }
        std::vector<XrSwapchainImageOpenGLKHR> images;
        const XrResult imagesListed = enumerate(
            images,
            [&](std::uint32_t capacity, std::uint32_t* count, XrSwapchainImageOpenGLKHR* out) {
                return xrEnumerateSwapchainImages(m_swapchains[eye], capacity, count,
                                                  reinterpret_cast<XrSwapchainImageBaseHeader*>(out));
            },
            [] { return typed<XrSwapchainImageOpenGLKHR>(XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR); });
        if (XR_FAILED(imagesListed) || images.empty()) {
            return tl::unexpected(xrError(m_instance, imagesListed, "xrEnumerateSwapchainImages"));
        }
        for (const XrSwapchainImageOpenGLKHR& image : images) {
            m_images[eye].push_back(image.image);
        }
    }
    m_description.imagesPerEye = m_images[0].size();

    // The refresh rate.
    if (m_enumerateRefreshRates != nullptr) {
        enumerate(
            m_description.refreshRates,
            [&](std::uint32_t capacity, std::uint32_t* count, float* out) {
                return m_enumerateRefreshRates(m_session, capacity, count, out);
            },
            [] { return 0.0f; });
        if (options.refreshRate && m_requestRefreshRate != nullptr) {
            m_description.refreshRateRequested = options.refreshRate;
            if (const XrResult result = m_requestRefreshRate(m_session, *options.refreshRate); XR_FAILED(result)) {
                spdlog::warn("OpenXR refresh rate {:g} Hz refused: {}", *options.refreshRate,
                             resultName(m_instance, result));
            }
        }
    } else if (options.refreshRate) {
        spdlog::warn("OpenXR refresh rate {:g} Hz not asked for: the runtime offers no XR_FB_display_refresh_rate",
                     *options.refreshRate);
    }
    spdlog::info("XR session: {} space, eyes {}x{} {} ({} image(s) each), refresh rates [{}]", m_description.space,
                 m_description.eyeSize.x, m_description.eyeSize.y, m_description.format, m_description.imagesPerEye,
                 fmt::join(m_description.refreshRates, ", "));
    return ok();
}

std::optional<RuntimeEvent> OpenXrRuntime::pollEvent() {
    for (;;) {
        XrEventDataBuffer event = typed<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        if (xrPollEvent(m_instance, &event) != XR_SUCCESS) {
            return std::nullopt;
        }
        RuntimeEvent out;
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            out.kind = RuntimeEvent::Kind::StateChanged;
            out.state = toState(reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
            return out;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            out.kind = RuntimeEvent::Kind::InstanceLossPending;
            return out;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            out.kind = RuntimeEvent::Kind::ReferenceSpaceChangePending;
            return out;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& changed = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(event);
            out.kind = RuntimeEvent::Kind::RefreshRateChanged;
            out.fromHz = changed.fromDisplayRefreshRate;
            out.toHz = changed.toDisplayRefreshRate;
            return out;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            out.kind = RuntimeEvent::Kind::EventsLost;
            out.count = reinterpret_cast<const XrEventDataEventsLost&>(event).lostEventCount;
            return out;
        default:
            break; // events the session does not follow (interaction profiles: step 4)
        }
    }
}

Status OpenXrRuntime::beginSession() {
    XrSessionBeginInfo begin = typed<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
    begin.primaryViewConfigurationType = kStereo;
    if (const XrResult result = xrBeginSession(m_session, &begin); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrBeginSession"));
    }
    return ok();
}

Status OpenXrRuntime::endSession() {
    if (const XrResult result = xrEndSession(m_session); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrEndSession"));
    }
    return ok();
}

void OpenXrRuntime::requestExit() {
    if (const XrResult result = xrRequestExitSession(m_session); XR_FAILED(result)) {
        spdlog::warn("xrRequestExitSession failed: {}", resultName(m_instance, result));
    }
}

Expected<FrameTiming> OpenXrRuntime::waitFrame() {
    XrFrameWaitInfo wait = typed<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    XrFrameState state = typed<XrFrameState>(XR_TYPE_FRAME_STATE);
    if (const XrResult result = xrWaitFrame(m_session, &wait, &state); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrWaitFrame"));
    }
    return FrameTiming{state.shouldRender == XR_TRUE, state.predictedDisplayTime, state.predictedDisplayPeriod};
}

Status OpenXrRuntime::beginFrame() {
    XrFrameBeginInfo begin = typed<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
    // XR_FRAME_DISCARDED (a success code) only says the previous frame was never ended.
    if (const XrResult result = xrBeginFrame(m_session, &begin); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrBeginFrame"));
    }
    return ok();
}

Expected<bool> OpenXrRuntime::locateViews(std::int64_t displayTimeNs, EyeViews& eyes) {
    XrViewLocateInfo locate = typed<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
    locate.viewConfigurationType = kStereo;
    locate.displayTime = displayTimeNs;
    locate.space = m_space;
    XrViewState state = typed<XrViewState>(XR_TYPE_VIEW_STATE);
    m_views = {typed<XrView>(XR_TYPE_VIEW), typed<XrView>(XR_TYPE_VIEW)};
    std::uint32_t count = 0;
    if (const XrResult result = xrLocateViews(m_session, &locate, &state, 2, &count, m_views.data());
        XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrLocateViews"));
    }
    constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (count != 2 || (state.viewStateFlags & kValid) != kValid) {
        return false;
    }
    for (std::size_t i = 0; i < eyes.size(); ++i) {
        eyes[i].pose = toPose(m_views[i].pose);
        const XrFovf& fov = m_views[i].fov;
        eyes[i].fov = scene::EyeFov{fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
    }
    return true;
}

Expected<std::uint32_t> OpenXrRuntime::acquireImage(int eye) {
    const XrSwapchain swapchain = m_swapchains[static_cast<std::size_t>(eye)];
    XrSwapchainImageAcquireInfo acquire = typed<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    std::uint32_t index = 0;
    if (const XrResult result = xrAcquireSwapchainImage(swapchain, &acquire, &index); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrAcquireSwapchainImage"));
    }
    XrSwapchainImageWaitInfo wait = typed<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    wait.timeout = XR_INFINITE_DURATION;
    if (const XrResult result = xrWaitSwapchainImage(swapchain, &wait); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrWaitSwapchainImage"));
    }
    return index;
}

Status OpenXrRuntime::releaseImage(int eye) {
    XrSwapchainImageReleaseInfo release = typed<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    if (const XrResult result = xrReleaseSwapchainImage(m_swapchains[static_cast<std::size_t>(eye)], &release);
        XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrReleaseSwapchainImage"));
    }
    return ok();
}

Status OpenXrRuntime::endFrame(const FrameTiming& timing, bool withViews) {
    std::array<XrCompositionLayerProjectionView, 2> projected{};
    for (std::size_t i = 0; i < projected.size(); ++i) {
        projected[i] = typed<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
        projected[i].pose = m_views[i].pose;
        projected[i].fov = m_views[i].fov;
        projected[i].subImage.swapchain = m_swapchains[i];
        projected[i].subImage.imageRect.offset = {0, 0};
        projected[i].subImage.imageRect.extent = {m_description.eyeSize.x, m_description.eyeSize.y};
        projected[i].subImage.imageArrayIndex = 0;
    }
    XrCompositionLayerProjection layer = typed<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    layer.space = m_space;
    layer.viewCount = static_cast<std::uint32_t>(projected.size());
    layer.views = projected.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo end = typed<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    end.displayTime = timing.displayTimeNs;
    end.environmentBlendMode = m_blendMode;
    end.layerCount = withViews ? 1u : 0u;
    end.layers = withViews ? layers : nullptr;
    if (const XrResult result = xrEndFrame(m_session, &end); XR_FAILED(result)) {
        return tl::unexpected(xrError(m_instance, result, "xrEndFrame"));
    }
    return ok();
}

std::span<const std::uint32_t> OpenXrRuntime::images(int eye) const {
    return m_images[static_cast<std::size_t>(eye)];
}

std::optional<float> OpenXrRuntime::currentRefreshRate() const {
    float rate = 0.0f;
    if (m_getRefreshRate != nullptr && m_session != XR_NULL_HANDLE &&
        XR_SUCCEEDED(m_getRefreshRate(m_session, &rate))) {
        return rate;
    }
    return std::nullopt;
}

} // namespace

Expected<std::unique_ptr<Runtime>> createOpenXrRuntime(const OpenXrOptions& options) {
    auto runtime = std::make_unique<OpenXrRuntime>();
    if (auto created = runtime->createInstance(options); !created) {
        return propagate(created.error());
    }
    if (auto found = runtime->findSystem(options.headsetWait); !found) {
        return propagate(found.error());
    }
    if (auto session = runtime->createSession(options); !session) {
        return propagate(session.error());
    }
    return std::unique_ptr<Runtime>(std::move(runtime));
}

} // namespace rv::xr
