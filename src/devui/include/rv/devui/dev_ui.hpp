#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/scene/viewpoint.hpp>

union SDL_Event;

namespace rv::platform {
class GraphicsWindow;
} // namespace rv::platform

namespace rv::gl {
class Framebuffer;
class GLDevice;
} // namespace rv::gl

namespace rv::devui {

/// One dockable window of the developer overlay: a thin view of state that lives elsewhere (rv/devui/panels.hpp).
class Panel {
public:
    virtual ~Panel();

    /// Window title, unique within the overlay: also the window's Dear ImGui id and its key in imgui.ini.
    [[nodiscard]] virtual const char* title() const noexcept = 0;
    /// Draws the window's contents; called between ImGui::Begin and ImGui::End while the window is shown.
    virtual void draw() = 0;
    /// Called once per frame by DevUi::buildFrame() before any panel is drawn, also while this one is not (a tab behind
    /// another, the overlay hidden): for what the panel must keep in step with the state it shows even unseen, such as
    /// releasing the thumbnails of textures that are gone (TexturePreviews::sync). Nothing by default.
    virtual void update() {}
};

/// Where the default layout docks a panel; the layout imgui.ini keeps replaces it once the user rearranges the panels.
/// Panels sharing a slot become tabs of one dock node, the first added in front.
enum class DockSlot : std::uint8_t {
    Left,        ///< left column, top
    LeftBottom,  ///< left column, bottom
    Right,       ///< right column, top
    RightBottom, ///< right column, bottom
    Bottom,      ///< below the scene, between the columns
};

/// Settings of a DevUi.
struct DevUiConfig {
    /// Where the window layout is kept (Dear ImGui's ini file), normally <user data>/config/imgui.ini; its directory is
    /// created when missing. Empty keeps the layout for the session only. Never the working directory.
    std::filesystem::path iniFile;
};

/// The Dear ImGui developer overlay (developer builds, RV_DEV_UI): owns the ImGui context with its SDL3 platform and
/// OpenGL 3 renderer backends (GLSL "#version 450", docking enabled), and draws the dockable panels added to it over
/// the scene, around a see-through central area (a dock space with a pass-through central node).
///
///  - Visibility: hidden at creation; ToggleDevUI (F1) shows and hides it through setVisible(). While hidden no panel
///    is drawn and every event goes to the game; an ImGui frame is built only for the scene decoration, when one is set
///    (setSceneDecoration), so that labels placed in the scene stay over it.
///  - Input: every SDL event goes through handleEvent(), which feeds Dear ImGui first and decides what the game still
///    gets (routeOverlayEvent): nothing the overlay wants (a click on a panel, keys while a text field or a dragged
///    widget is active, so Esc there never quits), but always the releases, and nothing at all while the game holds
///    the mouse for first-person look (setMouseCaptured). Gamepads are left to the game. Keyboard navigation is off
///    (it would take the game's keys), so Dear ImGui leaves Esc alone: the game's Back closes the topmost popup
///    (closePopup()) before it closes the overlay.
///  - Scale: fonts and sizes follow the content scale of the window's display (SDL_GetWindowDisplayScale), also when
///    the window moves to another display. The font is Dear ImGui's scalable default (ProggyForever).
///  - Rendering: buildFrame() builds the frame (once per frame, before the scene renders, so that edits apply at
///    once), renderToWindow() draws it last into the default framebuffer, renderTo() into a screenshot target, scaled
///    uniformly to fit it and centered (fitOverlayCapture). GL_FRAMEBUFFER_SRGB is disabled while Dear ImGui draws
///    (its colors are sRGB already) and the RenderStateCache is invalidated afterwards, since the backend changes GL
///    state behind it.
/// Only one DevUi exists at a time (Dear ImGui's current context). Main thread only; create and destroy it while the
/// OpenGL context is current. Destruction shuts the backends down and saves the layout.
class DevUi {
public:
    /// Creates the context and initializes both backends for `window`, which must outlive the overlay. Errors:
    /// InvalidArgument while another DevUi is alive, GraphicsError when a backend cannot initialize (the overlay's
    /// shaders did not build); nothing is left behind.
    [[nodiscard]] static Expected<std::unique_ptr<DevUi>> create(platform::GraphicsWindow& window,
                                                                 const DevUiConfig& config);
    ~DevUi();
    DevUi(const DevUi&) = delete;
    DevUi& operator=(const DevUi&) = delete;

    /// Adds a dockable panel, placed at `slot` by the default layout. Panels are drawn in the order they were added.
    void addPanel(std::unique_ptr<Panel> panel, DockSlot slot);
    /// Brings the panel titled `title` to the front of its dock node (selects its tab) when the next frame is built.
    void focusPanel(std::string_view title);
    /// Draws over the scene and under the panels in every frame built (Dear ImGui's background draw list), after the
    /// panels, also while the overlay is hidden (then over the scene alone): labels placed in the scene, such as the
    /// asset preview's socket names (sceneViewArea() is the part the panels leave visible). Empty: nothing.
    void setSceneDecoration(std::function<void()> draw);

    [[nodiscard]] bool visible() const noexcept;
    /// Shows or hides the overlay. Showing it forgets the keys and buttons Dear ImGui last saw held (they may have been
    /// released while it was hidden); hiding it closes its popups, ends text input and puts the default mouse cursor
    /// back.
    void setVisible(bool visible);

    /// Routes one SDL event, also while hidden; `mouseCaptured` tells whether the game holds the mouse for first-person
    /// look. Returns true when the game must not see the event (see the class comment).
    [[nodiscard]] bool handleEvent(const SDL_Event& event, bool mouseCaptured);
    /// The game captured or released the mouse (relative mode for first-person look). On capture the overlay loses its
    /// pointer: the buttons it saw pressed are released (after the click that captured, which still ends a text edit
    /// there) and it stops changing the cursor.
    void setMouseCaptured(bool captured);

    /// A widget of the visible overlay has the keyboard (a text field being edited, a value being dragged).
    [[nodiscard]] bool hasKeyboard() const noexcept;
    /// A popup of the visible overlay is open (a combo's list, a color picker, a dock tab menu) in the frame built last.
    [[nodiscard]] bool popupOpen() const noexcept;
    /// Closes the topmost popup of the visible overlay unless it is modal (Back: one level at a time, as Dear ImGui's
    /// keyboard navigation does on Escape). Call it between frames, not from a panel.
    void closePopup();

    /// The part of the window the visible overlay leaves see-through over the scene (the central node of its dock
    /// space), in normalized device coordinates, as laid out by the frame being built or built last: panels drawing
    /// in buildFrame() see the current layout. Nullopt while hidden, before the first frame is built and when a panel
    /// is docked over the center.
    [[nodiscard]] std::optional<scene::ViewArea> sceneViewArea() const noexcept;

    /// Builds the overlay frame: Panel::update() of every panel, then the dock space and every panel shown (they may
    /// change the state they show) and the scene decoration. While hidden only the updates and, when set, the scene
    /// decoration. When this frame is also drawn into a screenshot of `captureSize` pixels, glyphs are rasterized for
    /// that size so that the scaled-up text stays sharp.
    void buildFrame(std::optional<glm::ivec2> captureSize = std::nullopt);
    /// Draws the frame built last into the default framebuffer (the window); nothing when no frame was built since the
    /// overlay was shown or hidden.
    void renderToWindow(gl::GLDevice& device);
    /// Draws the frame built last into `target` (screenshots), over what the target holds: scaled uniformly from the
    /// window to fit the target and centered, so that another aspect than the window's leaves bands of the target
    /// uncovered instead of stretching the panels (fitOverlayCapture).
    void renderTo(gl::GLDevice& device, const gl::Framebuffer& target);

private:
    struct Impl;

    explicit DevUi(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::devui
