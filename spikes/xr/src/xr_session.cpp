#include "xr_session.hpp"

// OpenXR's Win32 and OpenGL bindings need <windows.h> (HDC, HGLRC); NOMINMAX and WIN32_LEAN_AND_MEAN come from
// rv_set_target_defaults, so IUnknown (in the Win32 platform's declarations) needs <unknwn.h> of its own.
#include <windows.h>
#include <unknwn.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <thread>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>
#include <spike_xr/swapchain_format.hpp>

namespace rv::spikes::xr {
namespace {

using namespace std::chrono_literals;

constexpr const char* kOpenGlExtension = "XR_KHR_opengl_enable";
constexpr const char* kRefreshRateExtension = "XR_FB_display_refresh_rate";
constexpr const char* kLocalFloorExtension = "XR_EXT_local_floor";
constexpr const char* kPerformanceMetricsExtension = "XR_META_performance_metrics";
constexpr XrViewConfigurationType kStereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
/// LOCAL_FLOOR: core in OpenXR 1.1, XR_EXT_local_floor in 1.0 (the same value).
constexpr auto kLocalFloorSpace = static_cast<XrReferenceSpaceType>(1000426000);
/// The eye height LOCAL's origin is assumed to be at.
constexpr float kAssumedEyeHeight = 1.6f;

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

rv::Error xrError(XrInstance instance, XrResult result, std::string_view call,
                  ErrorCode code = ErrorCode::PlatformError) {
    return rv::Error{code, fmt::format("{} failed: {}", call, resultName(instance, result)), "openxr"};
}

std::string versionString(XrVersion version) {
    return fmt::format("{}.{}.{}", XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), XR_VERSION_PATCH(version));
}

const char* stateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE:
        return "idle";
    case XR_SESSION_STATE_READY:
        return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "synchronized";
    case XR_SESSION_STATE_VISIBLE:
        return "visible";
    case XR_SESSION_STATE_FOCUSED:
        return "focused";
    case XR_SESSION_STATE_STOPPING:
        return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "loss-pending";
    case XR_SESSION_STATE_EXITING:
        return "exiting";
    default:
        return "unknown";
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

const char* blendModeName(XrEnvironmentBlendMode mode) {
    switch (mode) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return "opaque";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return "additive";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return "alpha-blend";
    default:
        return "unknown";
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

const char* counterUnitName(XrPerformanceMetricsCounterUnitMETA unit) {
    switch (unit) {
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META:
        return "percent";
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META:
        return "ms";
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_BYTES_META:
        return "bytes";
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_HERTZ_META:
        return "hz";
    default:
        return "generic";
    }
}

EyePose toPose(const XrPosef& pose) {
    return EyePose{glm::quat(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z),
                   glm::vec3(pose.position.x, pose.position.y, pose.position.z)};
}

} // namespace

struct OpenXrSession::Native {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    std::array<XrSwapchain, 2> swapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrViewConfigurationView, 2> configViews{};
    std::array<XrView, 2> views{};
    bool refreshRateExtension = false;
    bool localFloorExtension = false;
    bool performanceMetricsExtension = false;
    /// The runtime's performance counters, turned on for the session, and their paths as text.
    std::vector<XrPath> counterPaths;
    std::vector<std::string> counterNames;
    PFN_xrGetOpenGLGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRefreshRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;
    PFN_xrEnumeratePerformanceMetricsCounterPathsMETA enumerateCounterPaths = nullptr;
    PFN_xrSetPerformanceMetricsStateMETA setPerformanceMetricsState = nullptr;
    PFN_xrQueryPerformanceMetricsCounterMETA queryCounter = nullptr;

    template <class Pfn>
    void load(const char* name, Pfn& pfn) const {
        PFN_xrVoidFunction function = nullptr;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, &function))) {
            pfn = reinterpret_cast<Pfn>(function);
        }
    }
};

OpenXrSession::OpenXrSession() : m_native(std::make_unique<Native>()), m_origin(std::chrono::steady_clock::now()) {}

OpenXrSession::~OpenXrSession() {
    Native& n = *m_native;
    for (XrSwapchain& swapchain : n.swapchains) {
        if (swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain);
        }
    }
    if (n.space != XR_NULL_HANDLE) {
        xrDestroySpace(n.space);
    }
    if (n.session != XR_NULL_HANDLE) {
        xrDestroySession(n.session);
    }
    if (n.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(n.instance);
    }
}

Expected<std::unique_ptr<OpenXrSession>> OpenXrSession::createInstance(const InstanceOptions& options,
                                                                       nlohmann::json& report) {
    nlohmann::json& runtime = report["runtime"];
    if (!options.runtimeJson.empty()) {
        // Read by the loader when it starts (here): this process alone uses the runtime, the system's choice stays.
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", options.runtimeJson.c_str());
        runtime["manifest"] = rv::core::pathToUtf8(options.runtimeJson);
    }

    std::vector<XrExtensionProperties> extensions;
    const XrResult listed = enumerate(
        extensions,
        [](std::uint32_t capacity, std::uint32_t* count, XrExtensionProperties* out) {
            return xrEnumerateInstanceExtensionProperties(nullptr, capacity, count, out);
        },
        [] { return typed<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES); });
    if (XR_FAILED(listed)) {
        runtime["available"] = false;
        return tl::unexpected(xrError(XR_NULL_HANDLE, listed, "xrEnumerateInstanceExtensionProperties (no runtime?)",
                                      ErrorCode::Unavailable));
    }
    runtime["available"] = true;
    nlohmann::json offered = nlohmann::json::array();
    const auto offers = [&](std::string_view name) {
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const XrExtensionProperties& e) { return name == e.extensionName; });
    };
    for (const XrExtensionProperties& e : extensions) {
        offered.push_back(fmt::format("{} v{}", e.extensionName, e.extensionVersion));
    }
    runtime["extensions"] = std::move(offered);
    if (!offers(kOpenGlExtension)) {
        return makeError(ErrorCode::Unsupported, "the runtime offers no XR_KHR_opengl_enable", "openxr");
    }

    auto self = std::unique_ptr<OpenXrSession>(new OpenXrSession());
    Native& n = *self->m_native;
    std::vector<const char*> enabled{kOpenGlExtension};
    if (offers(kRefreshRateExtension)) {
        enabled.push_back(kRefreshRateExtension);
        n.refreshRateExtension = true;
    }
    if (offers(kLocalFloorExtension)) {
        enabled.push_back(kLocalFloorExtension);
        n.localFloorExtension = true;
    }
    if (offers(kPerformanceMetricsExtension)) {
        enabled.push_back(kPerformanceMetricsExtension);
        n.performanceMetricsExtension = true;
    }
    runtime["enabledExtensions"] = enabled;

    XrInstanceCreateInfo create = typed<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    std::strncpy(create.applicationInfo.applicationName, "Retroverse XR spike", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create.applicationInfo.engineName, "Retroverse", XR_MAX_ENGINE_NAME_SIZE - 1);
    create.applicationInfo.applicationVersion = 1;
    create.applicationInfo.engineVersion = 1;
    // 1.0: what every PC runtime accepts; LOCAL_FLOOR then comes from XR_EXT_local_floor.
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    create.enabledExtensionNames = enabled.data();
    if (const XrResult result = xrCreateInstance(&create, &n.instance); XR_FAILED(result)) {
        return tl::unexpected(xrError(XR_NULL_HANDLE, result, "xrCreateInstance"));
    }

    XrInstanceProperties properties = typed<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
    if (XR_SUCCEEDED(xrGetInstanceProperties(n.instance, &properties))) {
        runtime["name"] = properties.runtimeName;
        runtime["version"] = versionString(properties.runtimeVersion);
    }
    runtime["loaderApiVersion"] = versionString(XR_CURRENT_API_VERSION);
    spdlog::info("OpenXR runtime: {} {}", runtime.value("name", "?"), runtime.value("version", "?"));

    n.load("xrGetOpenGLGraphicsRequirementsKHR", n.getGraphicsRequirements);
    if (n.refreshRateExtension) {
        n.load("xrEnumerateDisplayRefreshRatesFB", n.enumerateRefreshRates);
        n.load("xrGetDisplayRefreshRateFB", n.getRefreshRate);
        n.load("xrRequestDisplayRefreshRateFB", n.requestRefreshRate);
    }
    if (n.performanceMetricsExtension) {
        n.load("xrEnumeratePerformanceMetricsCounterPathsMETA", n.enumerateCounterPaths);
        n.load("xrSetPerformanceMetricsStateMETA", n.setPerformanceMetricsState);
        n.load("xrQueryPerformanceMetricsCounterMETA", n.queryCounter);
    }
    if (n.getGraphicsRequirements == nullptr) {
        return makeError(ErrorCode::Unsupported, "xrGetOpenGLGraphicsRequirementsKHR is missing", "openxr");
    }
    return self;
}

Status OpenXrSession::findSystem(std::chrono::milliseconds wait, nlohmann::json& report) {
    Native& n = *m_native;
    nlohmann::json& system = report["system"];
    XrSystemGetInfo info = typed<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const auto start = std::chrono::steady_clock::now();
    int attempts = 0;
    for (;;) {
        ++attempts;
        const XrResult result = xrGetSystem(n.instance, &info, &n.system);
        if (XR_SUCCEEDED(result)) {
            break;
        }
        const bool waiting = result == XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        if (!waiting || std::chrono::steady_clock::now() - start >= wait) {
            system["found"] = false;
            system["attempts"] = attempts;
            system["error"] = resultName(n.instance, result);
            return tl::unexpected(
                xrError(n.instance, result, "xrGetSystem (headset connected, Link active?)",
                        waiting ? ErrorCode::Unavailable : ErrorCode::PlatformError));
        }
        if (attempts == 1) {
            spdlog::info("No headset yet (start Link on the headset); waiting up to {} s",
                         std::chrono::duration_cast<std::chrono::seconds>(wait).count());
        }
        std::this_thread::sleep_for(500ms);
    }
    system["found"] = true;
    system["attempts"] = attempts;

    XrSystemProperties properties = typed<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    if (XR_SUCCEEDED(xrGetSystemProperties(n.instance, n.system, &properties))) {
        system["name"] = properties.systemName;
        system["vendorId"] = properties.vendorId;
        system["maxSwapchainSize"] = {properties.graphicsProperties.maxSwapchainImageWidth,
                                      properties.graphicsProperties.maxSwapchainImageHeight};
        system["maxLayerCount"] = properties.graphicsProperties.maxLayerCount;
        system["orientationTracking"] = properties.trackingProperties.orientationTracking == XR_TRUE;
        system["positionTracking"] = properties.trackingProperties.positionTracking == XR_TRUE;
        spdlog::info("Headset: {}", properties.systemName);
    }

    std::vector<XrViewConfigurationType> configurations;
    enumerate(
        configurations,
        [&](std::uint32_t capacity, std::uint32_t* count, XrViewConfigurationType* out) {
            return xrEnumerateViewConfigurations(n.instance, n.system, capacity, count, out);
        },
        [] { return XrViewConfigurationType{}; });
    if (std::find(configurations.begin(), configurations.end(), kStereo) == configurations.end()) {
        return makeError(ErrorCode::Unsupported, "the headset offers no primary stereo view configuration", "openxr");
    }

    std::vector<XrViewConfigurationView> views;
    const XrResult viewsListed = enumerate(
        views,
        [&](std::uint32_t capacity, std::uint32_t* count, XrViewConfigurationView* out) {
            return xrEnumerateViewConfigurationViews(n.instance, n.system, kStereo, capacity, count, out);
        },
        [] { return typed<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW); });
    if (XR_FAILED(viewsListed) || views.size() != 2) {
        return makeError(ErrorCode::Unsupported, "the stereo configuration does not have two views", "openxr");
    }
    nlohmann::json eyes = nlohmann::json::array();
    for (std::size_t i = 0; i < 2; ++i) {
        n.configViews[i] = views[i];
        eyes.push_back({{"recommended", {views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight}},
                        {"max", {views[i].maxImageRectWidth, views[i].maxImageRectHeight}},
                        {"recommendedSamples", views[i].recommendedSwapchainSampleCount}});
    }
    system["eyes"] = std::move(eyes);

    std::vector<XrEnvironmentBlendMode> blendModes;
    enumerate(
        blendModes,
        [&](std::uint32_t capacity, std::uint32_t* count, XrEnvironmentBlendMode* out) {
            return xrEnumerateEnvironmentBlendModes(n.instance, n.system, kStereo, capacity, count, out);
        },
        [] { return XrEnvironmentBlendMode{}; });
    nlohmann::json modes = nlohmann::json::array();
    for (const XrEnvironmentBlendMode mode : blendModes) {
        modes.push_back(blendModeName(mode));
    }
    system["blendModes"] = std::move(modes);
    const bool opaque = std::find(blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) !=
                        blendModes.end();
    n.blendMode = opaque || blendModes.empty() ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : blendModes.front();

    // Required before xrCreateSession.
    XrGraphicsRequirementsOpenGLKHR requirements =
        typed<XrGraphicsRequirementsOpenGLKHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR);
    if (const XrResult result = n.getGraphicsRequirements(n.instance, n.system, &requirements); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrGetOpenGLGraphicsRequirementsKHR"));
    }
    system["openGl"] = {{"minApiVersion", versionString(requirements.minApiVersionSupported)},
                        {"maxApiVersion", versionString(requirements.maxApiVersionSupported)}};
    const XrVersion ours = XR_MAKE_VERSION(4, 5, 0);
    if (ours < requirements.minApiVersionSupported) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("the runtime needs OpenGL {} or later",
                                     versionString(requirements.minApiVersionSupported)),
                         "openxr");
    }
    return {};
}

Status OpenXrSession::createSession(const SessionOptions& options, nlohmann::json& report) {
    Native& n = *m_native;
    nlohmann::json& session = report["session"];

    XrGraphicsBindingOpenGLWin32KHR binding =
        typed<XrGraphicsBindingOpenGLWin32KHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR);
    binding.hDC = wglGetCurrentDC();
    binding.hGLRC = wglGetCurrentContext();
    if (binding.hDC == nullptr || binding.hGLRC == nullptr) {
        return makeError(ErrorCode::GraphicsError, "no OpenGL context is current on this thread", "openxr");
    }
    XrSessionCreateInfo create = typed<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    create.next = &binding;
    create.systemId = n.system;
    if (const XrResult result = xrCreateSession(n.instance, &create, &n.session); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrCreateSession"));
    }

    // The reference space.
    std::vector<XrReferenceSpaceType> spaces;
    enumerate(
        spaces,
        [&](std::uint32_t capacity, std::uint32_t* count, XrReferenceSpaceType* out) {
            return xrEnumerateReferenceSpaces(n.session, capacity, count, out);
        },
        [] { return XrReferenceSpaceType{}; });
    nlohmann::json spaceNames = nlohmann::json::array();
    for (const XrReferenceSpaceType type : spaces) {
        spaceNames.push_back(spaceName(type));
    }
    session["referenceSpaces"] = std::move(spaceNames);
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
    if (const XrResult result = xrCreateReferenceSpace(n.session, &spaceInfo, &n.space); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrCreateReferenceSpace"));
    }
    m_floorOffset = chosen == XR_REFERENCE_SPACE_TYPE_LOCAL ? kAssumedEyeHeight : 0.0f;
    session["space"] = spaceName(chosen);

    // The eyes' format and size.
    std::vector<std::int64_t> formats;
    enumerate(
        formats,
        [&](std::uint32_t capacity, std::uint32_t* count, std::int64_t* out) {
            return xrEnumerateSwapchainFormats(n.session, capacity, count, out);
        },
        [] { return std::int64_t{0}; });
    nlohmann::json formatNames = nlohmann::json::array();
    for (const std::int64_t format : formats) {
        formatNames.push_back(glFormatName(format));
    }
    session["swapchainFormats"] = std::move(formatNames);
    const auto format = chooseColorFormat(formats);
    if (!format) {
        return makeError(ErrorCode::Unsupported, "the runtime offers neither GL_SRGB8_ALPHA8 nor GL_RGBA8", "openxr");
    }
    m_srgb = format->srgb;
    session["format"] = glFormatName(format->format);

    const XrViewConfigurationView& left = n.configViews[0];
    const auto scaled = [&](std::uint32_t recommended, std::uint32_t maximum) {
        const double size = std::round(static_cast<double>(recommended) * options.renderScale);
        return static_cast<int>(std::clamp(size, 16.0, static_cast<double>(maximum)));
    };
    m_eyeSize = {scaled(left.recommendedImageRectWidth, left.maxImageRectWidth),
                 scaled(left.recommendedImageRectHeight, left.maxImageRectHeight)};
    session["eyeSize"] = {m_eyeSize.x, m_eyeSize.y};
    session["renderScale"] = options.renderScale;

    nlohmann::json imageCounts = nlohmann::json::array();
    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo info = typed<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format->format;
        info.sampleCount = 1;
        info.width = static_cast<std::uint32_t>(m_eyeSize.x);
        info.height = static_cast<std::uint32_t>(m_eyeSize.y);
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (const XrResult result = xrCreateSwapchain(n.session, &info, &n.swapchains[eye]); XR_FAILED(result)) {
            return tl::unexpected(xrError(n.instance, result, "xrCreateSwapchain"));
        }
        std::vector<XrSwapchainImageOpenGLKHR> images;
        const XrResult listed = enumerate(
            images,
            [&](std::uint32_t capacity, std::uint32_t* count, XrSwapchainImageOpenGLKHR* out) {
                return xrEnumerateSwapchainImages(n.swapchains[eye], capacity, count,
                                                  reinterpret_cast<XrSwapchainImageBaseHeader*>(out));
            },
            [] { return typed<XrSwapchainImageOpenGLKHR>(XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR); });
        if (XR_FAILED(listed) || images.empty()) {
            return tl::unexpected(xrError(n.instance, listed, "xrEnumerateSwapchainImages"));
        }
        m_images[eye].clear();
        for (const XrSwapchainImageOpenGLKHR& image : images) {
            m_images[eye].push_back(image.image);
        }
        imageCounts.push_back(images.size());
    }
    session["imagesPerEye"] = std::move(imageCounts);

    // The refresh rate.
    if (n.enumerateRefreshRates != nullptr) {
        std::vector<float> rates;
        enumerate(
            rates,
            [&](std::uint32_t capacity, std::uint32_t* count, float* out) {
                return n.enumerateRefreshRates(n.session, capacity, count, out);
            },
            [] { return 0.0f; });
        session["refreshRates"] = rates;
        if (options.refreshRate && n.requestRefreshRate != nullptr) {
            const XrResult result = n.requestRefreshRate(n.session, *options.refreshRate);
            session["refreshRateRequested"] = *options.refreshRate;
            session["refreshRateRequest"] = resultName(n.instance, result);
        }
    } else if (options.refreshRate) {
        session["refreshRateRequest"] = "XR_FB_display_refresh_rate not offered";
    }
    if (const auto rate = currentRefreshRate()) {
        session["refreshRateAtStart"] = *rate;
    }
    // The runtime's own counters: what its compositor dropped, its view of the app's frame times.
    if (n.enumerateCounterPaths != nullptr && n.setPerformanceMetricsState != nullptr && n.queryCounter != nullptr) {
        enumerate(
            n.counterPaths,
            [&](std::uint32_t capacity, std::uint32_t* count, XrPath* out) {
                return n.enumerateCounterPaths(n.instance, capacity, count, out);
            },
            [] { return XrPath{XR_NULL_PATH}; });
        for (const XrPath path : n.counterPaths) {
            char name[XR_MAX_PATH_LENGTH] = {};
            std::uint32_t length = 0;
            n.counterNames.emplace_back(
                XR_SUCCEEDED(xrPathToString(n.instance, path, XR_MAX_PATH_LENGTH, &length, name)) ? name : "?");
        }
        XrPerformanceMetricsStateMETA state =
            typed<XrPerformanceMetricsStateMETA>(XR_TYPE_PERFORMANCE_METRICS_STATE_META);
        state.enabled = XR_TRUE;
        const XrResult result = n.setPerformanceMetricsState(n.session, &state);
        session["runtimeCounters"] = {{"enabled", resultName(n.instance, result)}, {"paths", n.counterNames}};
        if (XR_FAILED(result)) {
            n.counterPaths.clear();
            n.counterNames.clear();
        }
    } else {
        session["runtimeCounters"] = {{"enabled", "XR_META_performance_metrics not offered"}};
    }
    spdlog::info("Session: {} space, eyes {}x{} {}", spaceName(chosen), m_eyeSize.x, m_eyeSize.y,
                 glFormatName(format->format));
    return {};
}

void OpenXrSession::recordEvent(nlohmann::json event) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_origin).count();
    event["atMs"] = std::round(ms * 10.0) / 10.0;
    m_events.push_back(std::move(event));
}

void OpenXrSession::pollEvents() {
    Native& n = *m_native;
    XrEventDataBuffer event = typed<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
    while (xrPollEvent(n.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            recordEvent({{"state", stateName(changed.state)}});
            spdlog::info("Session state: {}", stateName(changed.state));
            switch (changed.state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo begin = typed<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
                begin.primaryViewConfigurationType = kStereo;
                const XrResult result = xrBeginSession(n.session, &begin);
                if (XR_SUCCEEDED(result)) {
                    m_running = true;
                } else {
                    recordEvent({{"error", fmt::format("xrBeginSession: {}", resultName(n.instance, result))}});
                    m_ended = true;
                }
                break;
            }
            case XR_SESSION_STATE_FOCUSED:
                m_reachedFocused = true;
                break;
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(n.session);
                m_running = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                m_running = false;
                m_ended = true;
                break;
            default:
                break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            recordEvent({{"event", "instance-loss-pending"}});
            m_running = false;
            m_ended = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& changed = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
            recordEvent({{"event", "reference-space-change"}, {"space", spaceName(changed.referenceSpaceType)}});
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& changed = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(event);
            recordEvent({{"event", "refresh-rate"}, {"from", changed.fromDisplayRefreshRate},
                         {"to", changed.toDisplayRefreshRate}});
            break;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
            const auto& lost = reinterpret_cast<const XrEventDataEventsLost&>(event);
            recordEvent({{"event", "events-lost"}, {"count", lost.lostEventCount}});
            break;
        }
        default:
            break;
        }
        event = typed<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
    }
}

Expected<FrameTiming> OpenXrSession::waitFrame() {
    Native& n = *m_native;
    XrFrameWaitInfo wait = typed<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    XrFrameState state = typed<XrFrameState>(XR_TYPE_FRAME_STATE);
    if (const XrResult result = xrWaitFrame(n.session, &wait, &state); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrWaitFrame"));
    }
    return FrameTiming{state.shouldRender == XR_TRUE, state.predictedDisplayTime, state.predictedDisplayPeriod};
}

Status OpenXrSession::beginFrame() {
    Native& n = *m_native;
    XrFrameBeginInfo begin = typed<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
    // XR_FRAME_DISCARDED (a success code) only says the previous frame was never ended.
    if (const XrResult result = xrBeginFrame(n.session, &begin); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrBeginFrame"));
    }
    return {};
}

Expected<bool> OpenXrSession::locateViews(std::int64_t displayTimeNs, std::array<EyeView, 2>& eyes) {
    Native& n = *m_native;
    XrViewLocateInfo locate = typed<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
    locate.viewConfigurationType = kStereo;
    locate.displayTime = displayTimeNs;
    locate.space = n.space;
    XrViewState state = typed<XrViewState>(XR_TYPE_VIEW_STATE);
    n.views = {typed<XrView>(XR_TYPE_VIEW), typed<XrView>(XR_TYPE_VIEW)};
    std::uint32_t count = 0;
    if (const XrResult result = xrLocateViews(n.session, &locate, &state, 2, &count, n.views.data());
        XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrLocateViews"));
    }
    constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (count != 2 || (state.viewStateFlags & kValid) != kValid) {
        return false;
    }
    for (std::size_t i = 0; i < 2; ++i) {
        eyes[i].pose = toPose(n.views[i].pose);
        const XrFovf& fov = n.views[i].fov;
        eyes[i].fov = EyeFov{fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
    }
    return true;
}

Expected<std::uint32_t> OpenXrSession::acquireImage(int eye) {
    Native& n = *m_native;
    XrSwapchainImageAcquireInfo acquire = typed<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    std::uint32_t index = 0;
    if (const XrResult result = xrAcquireSwapchainImage(n.swapchains[eye], &acquire, &index); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrAcquireSwapchainImage"));
    }
    XrSwapchainImageWaitInfo wait = typed<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    wait.timeout = XR_INFINITE_DURATION;
    if (const XrResult result = xrWaitSwapchainImage(n.swapchains[eye], &wait); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrWaitSwapchainImage"));
    }
    return index;
}

Status OpenXrSession::releaseImage(int eye) {
    Native& n = *m_native;
    XrSwapchainImageReleaseInfo release = typed<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    if (const XrResult result = xrReleaseSwapchainImage(n.swapchains[eye], &release); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrReleaseSwapchainImage"));
    }
    return {};
}

Status OpenXrSession::endFrame(const FrameTiming& timing, bool withViews) {
    Native& n = *m_native;
    std::array<XrCompositionLayerProjectionView, 2> projected{};
    for (std::size_t i = 0; i < 2; ++i) {
        projected[i] = typed<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
        projected[i].pose = n.views[i].pose;
        projected[i].fov = n.views[i].fov;
        projected[i].subImage.swapchain = n.swapchains[i];
        projected[i].subImage.imageRect.offset = {0, 0};
        projected[i].subImage.imageRect.extent = {m_eyeSize.x, m_eyeSize.y};
        projected[i].subImage.imageArrayIndex = 0;
    }
    XrCompositionLayerProjection layer = typed<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    layer.space = n.space;
    layer.viewCount = 2;
    layer.views = projected.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo end = typed<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    end.displayTime = timing.displayTimeNs;
    end.environmentBlendMode = n.blendMode;
    end.layerCount = withViews ? 1u : 0u;
    end.layers = withViews ? layers : nullptr;
    if (const XrResult result = xrEndFrame(n.session, &end); XR_FAILED(result)) {
        return tl::unexpected(xrError(n.instance, result, "xrEndFrame"));
    }
    return {};
}

void OpenXrSession::requestExit() {
    Native& n = *m_native;
    if (n.session != XR_NULL_HANDLE && m_running) {
        const XrResult result = xrRequestExitSession(n.session);
        recordEvent({{"event", "exit-requested"}, {"result", resultName(n.instance, result)}});
    }
}

std::optional<float> OpenXrSession::currentRefreshRate() const {
    const Native& n = *m_native;
    float rate = 0.0f;
    if (n.getRefreshRate != nullptr && n.session != XR_NULL_HANDLE &&
        XR_SUCCEEDED(n.getRefreshRate(n.session, &rate))) {
        return rate;
    }
    return std::nullopt;
}

std::vector<RuntimeCounter> OpenXrSession::queryCounters() const {
    const Native& n = *m_native;
    std::vector<RuntimeCounter> counters;
    for (std::size_t i = 0; i < n.counterPaths.size(); ++i) {
        XrPerformanceMetricsCounterMETA counter =
            typed<XrPerformanceMetricsCounterMETA>(XR_TYPE_PERFORMANCE_METRICS_COUNTER_META);
        if (XR_FAILED(n.queryCounter(n.session, n.counterPaths[i], &counter))) {
            continue;
        }
        double value = 0.0;
        if ((counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) != 0) {
            value = counter.floatValue;
        } else if ((counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) != 0) {
            value = counter.uintValue;
        } else {
            continue;
        }
        counters.push_back({n.counterNames[i], counterUnitName(counter.counterUnit), value});
    }
    return counters;
}

const char* toString(SpaceChoice space) noexcept {
    switch (space) {
    case SpaceChoice::Auto:
        return "auto";
    case SpaceChoice::LocalFloor:
        return "local-floor";
    case SpaceChoice::Stage:
        return "stage";
    case SpaceChoice::Local:
        return "local";
    }
    return "?";
}

} // namespace rv::spikes::xr
