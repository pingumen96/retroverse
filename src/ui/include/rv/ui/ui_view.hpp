#pragma once

#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/codes.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/ui/ui_command.hpp>

union SDL_Event;

namespace Rml {
class Context;
class Element;
} // namespace Rml

namespace rv::gl {
class Framebuffer;
class GLDevice;
} // namespace rv::gl

namespace rv::ui {

class UiSystem;

/// Keys a focused text field moves its caret with, which the screen sends for the UI's Home and End commands, and
/// Backspace, which the on-screen keyboard sends for delete.
enum class CaretKey : std::uint8_t { Home, End, Backspace };

/// What a focus move did.
enum class NavOutcome : std::uint8_t {
    Moved,     ///< the focus moved to another element
    Consumed,  ///< the focused element used the move itself (the caret of a text field moved)
    Unhandled, ///< nothing in that direction within the focused element's region: the screen decides
};

[[nodiscard]] std::string_view toString(NavOutcome outcome) noexcept;

/// The cost of the UI pass in the last frame.
struct UiPassTimes {
    double updateMs = 0.0; ///< CPU: Context::Update (events, data bindings, styles, layout, animations)
    double renderMs = 0.0; ///< CPU: the whole render, the three parts below and the GPU timer
    double beginFrameMs = 0.0;    ///< CPU: the renderer's BeginFrame (OpenGL state backup, layer set-up and clear)
    double contextRenderMs = 0.0; ///< CPU: Context::Render (the element tree's geometry, OpenGL draws)
    double endFrameMs = 0.0;      ///< CPU: the renderer's EndFrame (MSAA resolve, composite, state restore)
    std::uint32_t drawCalls = 0;  ///< draws the renderer issued (geometry, clip masks, fullscreen passes)
    /// GPU: the renderer's frame in the window (gl::GpuTimer: read a few frames later, nothing before the first).
    std::optional<double> gpuMs;
};

/// One RmlUi context: the UI of one output (the window), laid out for its drawable size in pixels with the density of
/// its display (dp ratio = SDL_GetWindowDisplayScale x the text scale of the accessibility settings, specification
/// §12.4), so that RCSS sizes in dp read the same at any resolution and scale.
///
///  - Layout themes: views less tall than kCompactBelowDisplayDp in dp of the display alone activate the RmlUi theme
///    kCompactTheme (compactLayout(): 1280x720 at 125%), which documents style with "@media (theme: compact)". The
///    text scale does not take part, so that a larger text scale always means larger text (a dp media query would see
///    1920x1080 at 125% with text scale 1.2 as 720 dp tall and swap in the compact sizes, about as much smaller).
///  - Input: handleEvent() forwards the mouse, the wheel, text input and IME composition and the keys that are not UI
///    commands (letters, Backspace, Delete, Tab) to the context through the SDL3 backend. The keys bound to the UI
///    actions of the action map (setCommandKeys(): arrows, Enter, Esc, Page Up and Down, Home, End by default) and
///    every gamepad event stay out: they reach the UI as UiCommands through the action map and NavInput, the same path
///    for the pad, the keyboard and --input-script. The screen turns commands into navigate(), activate() and
///    caretKey(): the arrow, Enter, Home and End keys of RmlUi, so its spatial navigation (nav-up/down/left/right
///    properties, confined to the focused element's scroll container), scrolling the focus into view and text editing
///    all work as with a keyboard.
///  - Frame: update() once per frame (after the commands), then render() into the window after the scene, with
///    GL_FRAMEBUFFER_SRGB disabled (RmlUi's colors are sRGB, blended in sRGB like a browser) and the RenderStateCache
///    invalidated afterwards, since the backend changes state behind it; renderTo() draws the same frame into a
///    screenshot target, scaled uniformly to fit and centered (devui::fitOverlayCapture), like the developer overlay.
///  - Times: CPU of the update and the render, GPU of the window's render (gl::GpuTimer).
/// Destroy every view before its UiSystem. Main thread only.
class UiView {
public:
    /// Creates the context `name` (unique among the views) for a drawable of `drawableSize` pixels on a display of
    /// content scale `displayScale`. Errors: InvalidArgument for an empty name or a name in use, GraphicsError when
    /// the GPU timer cannot be created.
    [[nodiscard]] static Expected<std::unique_ptr<UiView>> create(UiSystem& system, std::string_view name,
                                                                  glm::ivec2 drawableSize, float displayScale);
    ~UiView();
    UiView(const UiView&) = delete;
    UiView& operator=(const UiView&) = delete;

    /// Follows the drawable size and the display's content scale, and the compact theme with them; true when either
    /// changed (the context lays its documents out again at the next update). A drawable without area (a minimized
    /// window) keeps the last size.
    bool resize(glm::ivec2 drawableSize, float displayScale);
    /// Text scaling (specification §12.4), clamped to [0.5, 3]: multiplies the dp ratio, never changes the theme.
    void setTextScale(float scale);
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_size; }
    [[nodiscard]] float displayScale() const noexcept { return m_displayScale; }
    [[nodiscard]] float textScale() const noexcept { return m_textScale; }
    [[nodiscard]] float dpRatio() const noexcept { return m_displayScale * m_textScale; }
    /// The compact theme is active (see the class comment).
    [[nodiscard]] bool compact() const noexcept { return m_compact; }

    [[nodiscard]] Rml::Context& context() noexcept { return *m_context; }
    [[nodiscard]] UiSystem& system() noexcept { return m_system; }

    /// The keys handleEvent() leaves out because they reach the UI as commands: the keys bound to the UI actions
    /// (ui::commandKeys() of the bindings in use; those of input::ActionMap::defaults() until set).
    void setCommandKeys(const std::bitset<input::kKeyCount>& keys) noexcept { m_commandKeys = keys; }
    /// Forwards one SDL event to the context (see the class comment). True when the UI used it: the game must not see
    /// it.
    bool handleEvent(const SDL_Event& event);

    /// Moves the focus like the arrow key of `direction` (the keyboard's modifiers apply: Shift selects in a text
    /// field).
    NavOutcome navigate(NavDirection direction);
    /// Presses the focused element like Enter: a click on a focusable element, a line break in a text field. True
    /// when an element used it.
    bool activate();
    /// Sends RmlUi's Home, End or Backspace key to the focused element: the caret of a text field moves to its start or
    /// end, the character before it goes. True when an element used it.
    bool caretKey(CaretKey key);

    /// The element with id `id` in any document of the view, or null.
    [[nodiscard]] Rml::Element* element(std::string_view id) const;
    /// The border box of the element with id `id`: x, y from the view's top-left corner, width, height, in pixels.
    [[nodiscard]] std::optional<glm::vec4> elementBox(std::string_view id) const;
    /// The focused element, null when only a document (or nothing) has the focus.
    [[nodiscard]] Rml::Element* focusedElement() const;

    /// Context::Update, timed. Once per frame, after the commands of the frame.
    void update();
    /// Draws the context into the window (the default framebuffer, over the scene), timed on the CPU and the GPU.
    void render(gl::GLDevice& device);
    /// Draws the context into `target` (a screenshot of the window's frame), scaled uniformly from the view's size to
    /// fit and centered.
    void renderTo(gl::GLDevice& device, const gl::Framebuffer& target);

    [[nodiscard]] const UiPassTimes& times() const noexcept { return m_times; }

private:
    UiView(UiSystem& system, Rml::Context& context, std::string name, gl::GpuTimer gpuTimer) noexcept;

    /// Sends key down and up to the context; true when the key down was not consumed (it propagated to the end).
    bool pressKey(int keyIdentifier);
    /// Activates or deactivates the compact theme for the current size and display scale.
    void applyLayoutTheme();

    UiSystem& m_system;
    Rml::Context* m_context = nullptr;
    std::string m_name;
    gl::GpuTimer m_gpuTimer;
    glm::ivec2 m_size{0};
    float m_displayScale = 1.0f;
    float m_textScale = 1.0f;
    bool m_compact = false;
    std::bitset<input::kKeyCount> m_commandKeys;
    UiPassTimes m_times;
};

} // namespace rv::ui
