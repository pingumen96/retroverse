#include <rv/ui/ui_view.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <SDL3/SDL_events.h>
#include <glad/gl.h>
#include <glm/common.hpp>
#include <spdlog/spdlog.h>

#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_GL3.h>

#include <rv/core/main_thread.hpp>
#include <rv/devui/overlay_capture.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_input_codes.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/ui/ui_system.hpp>

namespace rv::ui {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kMinTextScale = 0.5f;
constexpr float kMaxTextScale = 3.0f;

[[nodiscard]] double millisecondsSince(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] float sanitizedScale(float scale) noexcept {
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

[[nodiscard]] Rml::Input::KeyIdentifier keyOf(NavDirection direction) noexcept {
    switch (direction) {
    case NavDirection::Up:
        return Rml::Input::KI_UP;
    case NavDirection::Down:
        return Rml::Input::KI_DOWN;
    case NavDirection::Left:
        return Rml::Input::KI_LEFT;
    case NavDirection::Right:
        return Rml::Input::KI_RIGHT;
    }
    return Rml::Input::KI_UNKNOWN;
}

/// SDL events the context gets (keys aside, filtered by the command keys).
[[nodiscard]] bool isForwarded(Uint32 type) noexcept {
    switch (type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        return true;
    default:
        return false;
    }
}

} // namespace

std::string_view toString(NavOutcome outcome) noexcept {
    switch (outcome) {
    case NavOutcome::Moved:
        return "moved";
    case NavOutcome::Consumed:
        return "consumed";
    case NavOutcome::Unhandled:
        return "unhandled";
    }
    return "unknown";
}

UiView::UiView(UiSystem& system, Rml::Context& context, std::string name, gl::GpuTimer gpuTimer) noexcept
    : m_system(system), m_context(&context), m_name(std::move(name)), m_gpuTimer(std::move(gpuTimer)) {}

UiView::~UiView() {
    if (m_context != nullptr) {
        Rml::RemoveContext(m_name);
    }
}

Expected<std::unique_ptr<UiView>> UiView::create(UiSystem& system, std::string_view name, glm::ivec2 drawableSize,
                                                 float displayScale) {
    core::assertMainThread("UiView::create");
    const Rml::String contextName(name);
    if (contextName.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a UI view needs a name");
    }
    if (Rml::GetContext(contextName) != nullptr) {
        return makeError(ErrorCode::InvalidArgument, "a UI view with this name exists", contextName);
    }
    auto timer = gl::GpuTimer::create("UiView." + contextName);
    if (!timer) {
        return propagate(timer.error());
    }
    const glm::ivec2 size = glm::max(drawableSize, glm::ivec2(1));
    Rml::Context* context = Rml::CreateContext(contextName, Rml::Vector2i(size.x, size.y));
    if (context == nullptr) {
        return makeError(ErrorCode::GraphicsError, "Rml::CreateContext failed (see the log)", contextName);
    }
    auto view = std::unique_ptr<UiView>(new UiView(system, *context, contextName, std::move(*timer)));
    view->m_size = size;
    view->m_displayScale = sanitizedScale(displayScale);
    context->SetDensityIndependentPixelRatio(view->dpRatio());
    view->setCommandKeys(NavBindings::defaults());
    spdlog::info("UI view '{}': {}x{} pixels, dp ratio {:.2f}", contextName, size.x, size.y, view->dpRatio());
    return view;
}

bool UiView::resize(glm::ivec2 drawableSize, float displayScale) {
    if (drawableSize.x <= 0 || drawableSize.y <= 0) {
        return false;
    }
    bool changed = false;
    if (drawableSize != m_size) {
        m_size = drawableSize;
        m_context->SetDimensions(Rml::Vector2i(m_size.x, m_size.y));
        changed = true;
    }
    const float scale = sanitizedScale(displayScale);
    if (scale != m_displayScale) {
        m_displayScale = scale;
        m_context->SetDensityIndependentPixelRatio(dpRatio());
        changed = true;
    }
    if (changed) {
        spdlog::info("UI view '{}' resized: {}x{} pixels, dp ratio {:.2f}", m_name, m_size.x, m_size.y, dpRatio());
    }
    return changed;
}

void UiView::setTextScale(float scale) {
    const float clamped = std::clamp(sanitizedScale(scale), kMinTextScale, kMaxTextScale);
    if (clamped != m_textScale) {
        m_textScale = clamped;
        m_context->SetDensityIndependentPixelRatio(dpRatio());
        spdlog::info("UI view '{}': text scale {:.2f}, dp ratio {:.2f}", m_name, m_textScale, dpRatio());
    }
}

void UiView::setCommandKeys(const NavBindings& bindings) {
    m_commandKeys.reset();
    for (const auto& keys : bindings.keys) {
        for (const input::Key key : keys) {
            m_commandKeys.set(input::toIndex(key));
        }
    }
}

bool UiView::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
        if (const std::optional<input::Key> key = platform::keyFromScancode(event.key.scancode);
            key && m_commandKeys.test(input::toIndex(*key))) {
            return false; // a UI command: it arrives through NavInput
        }
    } else if (event.type == SDL_EVENT_TEXT_EDITING) {
        m_system.textInputEditor().HandleEdit(event.edit);
        return true;
    } else if (!isForwarded(event.type)) {
        return false; // window size and scale follow resize(); gamepads go through NavInput
    }
    SDL_Event copy = event; // the backend's handler takes a mutable event
    return !RmlSDL::InputEventHandler(m_context, m_system.window().handle(), copy);
}

bool UiView::pressKey(int keyIdentifier) {
    const auto key = static_cast<Rml::Input::KeyIdentifier>(keyIdentifier);
    const int modifiers = RmlSDL::GetKeyModifierState();
    const bool propagated = m_context->ProcessKeyDown(key, modifiers);
    m_context->ProcessKeyUp(key, modifiers);
    return propagated;
}

NavOutcome UiView::navigate(NavDirection direction) {
    const Rml::Element* before = m_context->GetFocusElement();
    const bool propagated = pressKey(keyOf(direction));
    if (m_context->GetFocusElement() != before) {
        return NavOutcome::Moved;
    }
    return propagated ? NavOutcome::Unhandled : NavOutcome::Consumed;
}

bool UiView::activate() {
    return !pressKey(Rml::Input::KI_RETURN);
}

Rml::Element* UiView::element(std::string_view id) const {
    const Rml::String name(id);
    for (int index = 0; index < m_context->GetNumDocuments(); ++index) {
        if (Rml::Element* found = m_context->GetDocument(index)->GetElementById(name)) {
            return found;
        }
    }
    return nullptr;
}

std::optional<glm::vec4> UiView::elementBox(std::string_view id) const {
    Rml::Element* found = element(id);
    if (found == nullptr) {
        return std::nullopt;
    }
    const Rml::Vector2f offset = found->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = found->GetBox().GetSize(Rml::BoxArea::Border);
    return glm::vec4(offset.x, offset.y, size.x, size.y);
}

Rml::Element* UiView::focusedElement() const {
    Rml::Element* focus = m_context->GetFocusElement();
    if (focus == nullptr || focus == m_context->GetRootElement() || focus == focus->GetOwnerDocument()) {
        return nullptr;
    }
    return focus;
}

void UiView::update() {
    const Clock::time_point start = Clock::now();
    m_context->Update();
    m_times.updateMs = millisecondsSince(start);
}

void UiView::render(gl::GLDevice& device) {
    const Clock::time_point start = Clock::now();
    const std::uint64_t drawsBefore = m_system.drawCalls();
    {
        const gl::ScopedDebugGroup group(device, "UI");
        RenderInterface_GL3& renderer = m_system.renderer();
        gl::Framebuffer::unbind();
        // RmlUi's colors are sRGB already: written as they are, not encoded a second time.
        device.state().setFramebufferSrgb(false);
        renderer.SetViewport(m_size.x, m_size.y);
        renderer.SetOutput(0, 0, 0, 0, 0);
        m_gpuTimer.begin();
        const Clock::time_point beginStart = Clock::now();
        renderer.BeginFrame();
        const Clock::time_point renderStart = Clock::now();
        m_context->Render();
        const Clock::time_point endStart = Clock::now();
        renderer.EndFrame();
        const Clock::time_point endDone = Clock::now();
        m_gpuTimer.end();
        device.state().invalidate(); // the backend changed state behind the cache
        m_times.beginFrameMs = std::chrono::duration<double, std::milli>(renderStart - beginStart).count();
        m_times.contextRenderMs = std::chrono::duration<double, std::milli>(endStart - renderStart).count();
        m_times.endFrameMs = std::chrono::duration<double, std::milli>(endDone - endStart).count();
    }
    m_times.drawCalls = static_cast<std::uint32_t>(m_system.drawCalls() - drawsBefore);
    m_times.renderMs = millisecondsSince(start);
    m_times.gpuMs = m_gpuTimer.latestMilliseconds();
}

void UiView::renderTo(gl::GLDevice& device, const gl::Framebuffer& target) {
    if (!target.valid()) {
        return;
    }
    const glm::ivec2 pixels = target.size();
    const devui::CaptureFit fit = devui::fitOverlayCapture(glm::vec2(m_size), pixels);
    const glm::ivec2 drawn = glm::ivec2(glm::round(glm::vec2(m_size) * fit.scale));
    const glm::ivec2 offset(fit.offset);
    const gl::ScopedDebugGroup group(device, "UI.Capture");
    RenderInterface_GL3& renderer = m_system.renderer();
    target.bind();
    GLint framebuffer = 0; // one state query per screenshot: Framebuffer keeps its name to itself
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    device.state().setFramebufferSrgb(false);
    renderer.SetViewport(m_size.x, m_size.y);
    // The output rectangle counts from the bottom-left corner, the fit's offset from the top-left one.
    renderer.SetOutput(static_cast<unsigned int>(framebuffer), offset.x, pixels.y - offset.y - drawn.y, drawn.x,
                       drawn.y);
    renderer.BeginFrame();
    m_context->Render();
    renderer.EndFrame();
    renderer.SetOutput(0, 0, 0, 0, 0);
    device.state().invalidate();
}

} // namespace rv::ui
