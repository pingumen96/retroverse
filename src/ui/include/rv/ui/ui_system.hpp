#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/ui/ui_assets.hpp>

namespace rv::platform {
class GraphicsWindow;
} // namespace rv::platform

class RenderInterface_GL3;
class TextInputMethodEditor_SDL;

namespace rv::ui {

/// Settings of a UiSystem.
struct UiSystemConfig {
    /// Where documents, style sheets and fonts are read from. RmlUi names them by UTF-8 paths relative to it
    /// ("ui/quick_catalog.rml"); paths that are absolute or climb out of it ("..") are refused.
    std::filesystem::path assetRoot;
    /// Font faces loaded at start, relative to the asset root (no system font is ever used, style §17.3): kUiFontFaces
    /// by default. A face that cannot be loaded is logged; with none loaded creation fails.
    std::vector<std::string> fontFaces{kUiFontFaces.begin(), kUiFontFaces.end()};
};

/// Messages RmlUi logged since the UiSystem was created, by severity (reports, smoke checks).
struct UiLogCounts {
    std::uint64_t errors = 0;   ///< errors and failed assertions
    std::uint64_t warnings = 0;
};

/// RmlUi for the product screens (specification §13.1: RmlUi for the product, Dear ImGui for developers only): owns
/// RmlUi's global state from Rml::Initialise to Rml::Shutdown with
///  - the system interface of the SDL3 backend (cursors, clipboard, text input through the window), its time set by the
///    application (setTime: with --fixed-dt, simulated time makes transitions reproducible) and RmlUi's log routed
///    to spdlog;
///  - a file interface reading from the asset root with UTF-8 paths;
///  - the OpenGL 3 renderer of the backend (vendored in third_party/rmlui_backends on our glad loader: one loader, no
///    duplicate symbols), shared by every view;
///  - the SDL text input method editor (IME composition into text fields);
///  - the font faces of UiSystemConfig (Atkinson Hyperlegible, SIL OFL 1.1, assets/registry).
/// The views (UiView, one RmlUi context per output) are created from it and must be destroyed before it. Only one
/// UiSystem exists at a time (RmlUi's state is global). Main thread only; create and destroy it while the OpenGL
/// context is current.
class UiSystem {
public:
    /// Initializes RmlUi for `window`, which must outlive the system. Errors: InvalidArgument while another UiSystem is
    /// alive, GraphicsError when the renderer's shaders do not build, IoError when no font face can be loaded,
    /// PlatformError when RmlUi does not initialize; nothing is left behind.
    [[nodiscard]] static Expected<std::unique_ptr<UiSystem>> create(platform::GraphicsWindow& window,
                                                                    const UiSystemConfig& config);
    ~UiSystem();
    UiSystem(const UiSystem&) = delete;
    UiSystem& operator=(const UiSystem&) = delete;

    /// The time animations, transitions and the text caret follow, in seconds; set once per frame. Until the first
    /// call it is the time elapsed since creation.
    void setTime(double seconds) noexcept;
    [[nodiscard]] double time() const noexcept;

    [[nodiscard]] UiLogCounts logCounts() const noexcept;
    [[nodiscard]] const std::filesystem::path& assetRoot() const noexcept;
    [[nodiscard]] platform::GraphicsWindow& window() const noexcept;
    /// The font faces that loaded, relative to the asset root.
    [[nodiscard]] const std::vector<std::string>& loadedFontFaces() const noexcept;

    /// The renderer shared by the views (rv_ui internal).
    [[nodiscard]] RenderInterface_GL3& renderer() noexcept;
    /// Draws the renderer has issued since the system was created (geometry, clip masks, fullscreen passes).
    [[nodiscard]] std::uint64_t drawCalls() const noexcept;
    /// The text input method editor that SDL_EVENT_TEXT_EDITING events go to (rv_ui internal).
    [[nodiscard]] TextInputMethodEditor_SDL& textInputEditor() noexcept;

private:
    struct Impl;

    explicit UiSystem(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
