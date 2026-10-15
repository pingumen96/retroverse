#include <rv/devui/dev_ui.hpp>

#include <array>
#include <cfloat>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <spdlog/spdlog.h>

#include <rv/core/main_thread.hpp>
#include <rv/core/text.hpp>
#include <rv/devui/overlay_input.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>

namespace rv::devui {
namespace {

/// Shaders of the OpenGL 3 backend: the GLSL of the OpenGL 4.5 core context.
constexpr const char* kGlslVersion = "#version 450";

/// Default layout, as shares of the space each split divides: the side columns, then the log row under the scene,
/// then the bottom part of each column.
constexpr float kLeftColumnShare = 0.23f;
constexpr float kRightColumnShare = 0.30f;
constexpr float kBottomRowShare = 0.24f;
constexpr float kLeftBottomShare = 0.39f;
constexpr float kRightBottomShare = 0.40f;

/// SDL hints the SDL3 backend changes for its own sake when it initializes (focus click-through, no automatic mouse
/// capture during drags, the style of borderless windows). They are restored afterwards, so that the game's input and
/// windows behave the same with or without the overlay.
constexpr std::array<const char*, 3> kBackendHints{SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, SDL_HINT_MOUSE_AUTO_CAPTURE,
                                                   "SDL_BORDERLESS_WINDOWED_STYLE"};

/// Saves the values of kBackendHints and restores them on destruction.
class ScopedBackendHints {
public:
    ScopedBackendHints() {
        for (std::size_t index = 0; index < kBackendHints.size(); ++index) {
            if (const char* value = SDL_GetHint(kBackendHints[index])) {
                m_values[index] = std::string(value);
            }
        }
    }
    ~ScopedBackendHints() {
        for (std::size_t index = 0; index < kBackendHints.size(); ++index) {
            if (m_values[index]) {
                SDL_SetHint(kBackendHints[index], m_values[index]->c_str());
            } else {
                SDL_ResetHint(kBackendHints[index]);
            }
        }
    }
    ScopedBackendHints(const ScopedBackendHints&) = delete;
    ScopedBackendHints& operator=(const ScopedBackendHints&) = delete;

private:
    std::array<std::optional<std::string>, kBackendHints.size()> m_values;
};

InputEventKind classify(const SDL_Event& event) noexcept {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        return InputEventKind::MouseMotion;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return InputEventKind::MouseButtonDown;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return InputEventKind::MouseButtonUp;
    case SDL_EVENT_MOUSE_WHEEL:
        return InputEventKind::MouseWheel;
    case SDL_EVENT_KEY_DOWN:
        return InputEventKind::KeyDown;
    case SDL_EVENT_KEY_UP:
        return InputEventKind::KeyUp;
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
        return InputEventKind::TextInput;
    default:
        return InputEventKind::Other;
    }
}

/// Releases every mouse button in Dear ImGui through the SDL3 backend, which also tracks held buttons itself (it keeps
/// the OS mouse capture on while it believes one is held). Queued after the events already received.
void releaseMouseButtons(std::uint32_t windowId) {
    SDL_Event release{};
    release.type = SDL_EVENT_MOUSE_BUTTON_UP;
    release.button.windowID = windowId;
    release.button.down = false;
    constexpr std::array<Uint8, 5> kButtons{SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT, SDL_BUTTON_X1,
                                            SDL_BUTTON_X2};
    for (const Uint8 button : kButtons) {
        release.button.button = button;
        ImGui_ImplSDL3_ProcessEvent(&release);
    }
}

/// Scale from `displaySize` (window coordinates) to `pixels`, nudged up by half a pixel so that the backend's
/// (int)(size * scale) truncation lands on the pixel size exactly.
ImVec2 scaleTo(const ImVec2& displaySize, const glm::ivec2& pixels) noexcept {
    return ImVec2((static_cast<float>(pixels.x) + 0.5f) / displaySize.x,
                  (static_cast<float>(pixels.y) + 0.5f) / displaySize.y);
}

} // namespace

Panel::~Panel() = default;

struct DevUi::Impl {
    struct DockedPanel {
        std::unique_ptr<Panel> panel;
        DockSlot slot = DockSlot::Right;
    };

    explicit Impl(platform::GraphicsWindow& window) noexcept : window(window) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (rendererReady) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        if (platformReady) {
            ImGui_ImplSDL3_Shutdown();
        }
        if (context != nullptr) {
            ImGui::DestroyContext(context); // saves imgui.ini
        }
    }

    /// Scales fonts and sizes for a display content scale, from the unscaled style.
    void applyDisplayScale(float scale) {
        ImGuiStyle& style = ImGui::GetStyle();
        style = baseStyle;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
        displayScale = scale;
    }

    /// The default layout: the side columns, the log row under the scene, and the panels docked by slot.
    void buildDefaultLayout(ImGuiID dockSpace, const ImVec2& size) {
        ImGui::DockBuilderRemoveNode(dockSpace);
        // The flags come from two enums (public and internal): combined as the ImGuiDockNodeFlags integer they are.
        const ImGuiDockNodeFlags flags =
            static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode;
        ImGui::DockBuilderAddNode(dockSpace, flags);
        ImGui::DockBuilderSetNodeSize(dockSpace, size);
        ImGuiID center = dockSpace;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, kLeftColumnShare, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, kRightColumnShare, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, kBottomRowShare, nullptr, &center);
        const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, kLeftBottomShare, nullptr, &left);
        const ImGuiID rightBottom =
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, kRightBottomShare, nullptr, &right);
        for (const DockedPanel& docked : panels) {
            ImGuiID node = right;
            switch (docked.slot) {
            case DockSlot::Left:
                node = left;
                break;
            case DockSlot::LeftBottom:
                node = leftBottom;
                break;
            case DockSlot::Right:
                node = right;
                break;
            case DockSlot::RightBottom:
                node = rightBottom;
                break;
            case DockSlot::Bottom:
                node = bottom;
                break;
            }
            ImGui::DockBuilderDockWindow(docked.panel->title(), node);
        }
        ImGui::DockBuilderFinish(dockSpace);
    }

    platform::GraphicsWindow& window;
    ImGuiContext* context = nullptr;
    bool platformReady = false;
    bool rendererReady = false;
    /// imgui.ini as UTF-8: io.IniFilename points into it.
    std::string iniFile;
    ImGuiStyle baseStyle;
    float displayScale = 1.0f;
    std::vector<DockedPanel> panels;

    bool visible = false;
    bool mouseCaptured = false;
    /// io.WantCaptureMouse and io.WantCaptureKeyboard of the last frame built while visible.
    bool wantsMouse = false;
    bool wantsKeyboard = false;
    /// The dock space had no layout when the overlay was first shown (no imgui.ini yet): build the default one.
    std::optional<bool> needsDefaultLayout;
    /// A frame was built since the overlay was shown: its draw data can be rendered.
    bool frameReady = false;
    /// io.DisplayFramebufferScale of the window, before buildFrame() raised it for a screenshot.
    ImVec2 windowFramebufferScale{1.0f, 1.0f};
};

DevUi::DevUi(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

DevUi::~DevUi() = default;

Expected<std::unique_ptr<DevUi>> DevUi::create(platform::GraphicsWindow& window, const DevUiConfig& config) {
    core::assertMainThread("DevUi::create");
    if (ImGui::GetCurrentContext() != nullptr) {
        return makeError(ErrorCode::InvalidArgument, "a developer overlay already exists");
    }
    IMGUI_CHECKVERSION();
    auto impl = std::make_unique<Impl>(window);
    impl->context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true; // dragging a value never moves its window
    // Nothing is ever written to the working directory: the layout goes to the ini file given (set below, once the
    // overlay works, so that a failed start writes nothing), logs nowhere.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();
    impl->baseStyle = ImGui::GetStyle();
    impl->applyDisplayScale(window.displayScale());
    io.Fonts->AddFontDefaultVector();

    {
        const ScopedBackendHints hints;
        impl->platformReady = ImGui_ImplSDL3_InitForOpenGL(window.handle(), window.glContext());
    }
    if (!impl->platformReady) {
        return makeError(ErrorCode::GraphicsError, "the Dear ImGui SDL3 backend could not initialize");
    }
    // The overlay is driven by mouse and keyboard; gamepads stay the game's.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    impl->rendererReady = ImGui_ImplOpenGL3_Init(kGlslVersion);
    if (!impl->rendererReady) {
        return makeError(ErrorCode::GraphicsError, "the Dear ImGui OpenGL 3 backend could not initialize");
    }
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        return makeError(ErrorCode::GraphicsError, "the Dear ImGui OpenGL 3 backend could not build its shaders");
    }
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // hidden: leave the cursor alone

    if (!config.iniFile.empty()) {
        std::error_code error;
        std::filesystem::create_directories(config.iniFile.parent_path(), error);
        if (error) {
            spdlog::warn("Developer overlay: the layout is not kept, {} cannot be created: {}",
                         core::pathToUtf8(config.iniFile.parent_path()), error.message());
        } else {
            impl->iniFile = core::pathToUtf8(config.iniFile);
            io.IniFilename = impl->iniFile.c_str();
        }
    }
    spdlog::info("Developer overlay: Dear ImGui {} (docking), display scale {:.2f}, layout {}", IMGUI_VERSION,
                 impl->displayScale, impl->iniFile.empty() ? std::string("not kept") : impl->iniFile);
    return std::unique_ptr<DevUi>(new DevUi(std::move(impl)));
}

void DevUi::addPanel(std::unique_ptr<Panel> panel, DockSlot slot) {
    if (panel != nullptr) {
        m_impl->panels.push_back(Impl::DockedPanel{std::move(panel), slot});
    }
}

bool DevUi::visible() const noexcept {
    return m_impl->visible;
}

void DevUi::setVisible(bool visible) {
    Impl& impl = *m_impl;
    if (visible == impl.visible) {
        return;
    }
    impl.visible = visible;
    impl.frameReady = false;
    impl.wantsMouse = false;
    impl.wantsKeyboard = false;
    ImGuiIO& io = ImGui::GetIO();
    if (visible) {
        // Keys and buttons may have been released while hidden, without Dear ImGui seeing it.
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.AddFocusEvent((SDL_GetWindowFlags(impl.window.handle()) & SDL_WINDOW_INPUT_FOCUS) != 0);
        if (!impl.mouseCaptured) {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        }
        spdlog::info("Developer overlay shown (F1 hides it)");
    } else {
        // Leave nothing of the overlay behind: buttons it saw held (and the OS mouse capture of a drag), text input,
        // its cursor shape.
        releaseMouseButtons(impl.window.id());
        SDL_CaptureMouse(false);
        SDL_Window* handle = impl.window.handle();
        if (SDL_TextInputActive(handle)) {
            SDL_StopTextInput(handle);
        }
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_ShowCursor();
        spdlog::info("Developer overlay hidden");
    }
}

bool DevUi::handleEvent(const SDL_Event& event, bool mouseCaptured) {
    Impl& impl = *m_impl;
    setMouseCaptured(mouseCaptured);
    if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED && event.window.windowID == impl.window.id() &&
        impl.window.displayScale() != impl.displayScale) {
        impl.applyDisplayScale(impl.window.displayScale());
        spdlog::info("Developer overlay: display scale {:.2f}", impl.displayScale);
    }
    const OverlayInputState state{impl.visible, impl.wantsMouse, impl.wantsKeyboard, impl.mouseCaptured};
    const EventRoute route = routeOverlayEvent(classify(event), state);
    if (route.toOverlay) {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
    return !route.toGame;
}

void DevUi::setMouseCaptured(bool captured) {
    Impl& impl = *m_impl;
    if (captured == impl.mouseCaptured) {
        return;
    }
    impl.mouseCaptured = captured;
    if (!impl.visible) {
        return; // setVisible() starts the overlay from a clean input state
    }
    ImGuiIO& io = ImGui::GetIO();
    if (captured) {
        // Queued after what Dear ImGui already received (the click that captured, which ends a text edit in a panel),
        // so that no button stays pressed in the overlay while first-person look has the mouse.
        releaseMouseButtons(impl.window.id());
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        impl.wantsMouse = false;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }
}

bool DevUi::hasKeyboard() const noexcept {
    return m_impl->visible && m_impl->wantsKeyboard;
}

void DevUi::buildFrame(std::optional<glm::ivec2> captureSize) {
    Impl& impl = *m_impl;
    if (!impl.visible) {
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    impl.windowFramebufferScale = io.DisplayFramebufferScale;
    if (captureSize && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        // Glyphs are rasterized at the density of the frame: a screenshot larger than the window gets its own.
        const ImVec2 captureScale = scaleTo(io.DisplaySize, *captureSize);
        if (captureScale.x > io.DisplayFramebufferScale.x) {
            io.DisplayFramebufferScale = captureScale;
        }
    }
    ImGui::NewFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockSpace = ImGui::GetID("RetroverseDockSpace");
    if (!impl.needsDefaultLayout) {
        impl.needsDefaultLayout = ImGui::DockBuilderGetNode(dockSpace) == nullptr; // imgui.ini has none
    }
    if (*impl.needsDefaultLayout && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        impl.buildDefaultLayout(dockSpace, viewport->WorkSize);
        impl.needsDefaultLayout = false;
    }
    ImGui::DockSpaceOverViewport(dockSpace, viewport, ImGuiDockNodeFlags_PassthruCentralNode);
    for (const Impl::DockedPanel& docked : impl.panels) {
        if (ImGui::Begin(docked.panel->title())) {
            docked.panel->draw();
        }
        ImGui::End();
    }
    ImGui::Render();
    impl.wantsMouse = io.WantCaptureMouse && !impl.mouseCaptured;
    impl.wantsKeyboard = io.WantCaptureKeyboard;
    impl.frameReady = true;
}

void DevUi::renderToWindow(gl::GLDevice& device) {
    Impl& impl = *m_impl;
    ImDrawData* data = ImGui::GetDrawData();
    if (!impl.visible || !impl.frameReady || data == nullptr || !data->Valid) {
        return;
    }
    const gl::ScopedDebugGroup group(device, "DevUi");
    gl::Framebuffer::unbind();
    // Dear ImGui's colors are sRGB already: written as they are, not encoded a second time.
    device.state().setFramebufferSrgb(false);
    const ImVec2 frameScale = data->FramebufferScale;
    data->FramebufferScale = impl.windowFramebufferScale;
    ImGui_ImplOpenGL3_RenderDrawData(data);
    data->FramebufferScale = frameScale;
    device.state().invalidate(); // the backend changed state behind the cache
}

void DevUi::renderTo(gl::GLDevice& device, const gl::Framebuffer& target) {
    Impl& impl = *m_impl;
    ImDrawData* data = ImGui::GetDrawData();
    if (!impl.visible || !impl.frameReady || data == nullptr || !data->Valid || !target.valid() ||
        data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) {
        return;
    }
    const gl::ScopedDebugGroup group(device, "DevUi.Capture");
    target.bind();
    // The target is sRGB: with GL_FRAMEBUFFER_SRGB off the overlay's sRGB colors are stored as they are, and blend
    // like they do in the window.
    device.state().setFramebufferSrgb(false);
    const ImVec2 frameScale = data->FramebufferScale;
    data->FramebufferScale = scaleTo(data->DisplaySize, target.size());
    ImGui_ImplOpenGL3_RenderDrawData(data);
    data->FramebufferScale = frameScale;
    device.state().invalidate();
}

} // namespace rv::devui
