#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include <rv/app/app_logic.hpp>

namespace rv::probe {

/// What the probe's own options choose (the rest of its command line is retroverse.exe's).
struct UiProbeOptions {
    /// --probe-shots <dir>: the screenshots of the fixture's checkpoints, written there by name.
    std::optional<std::filesystem::path> shots;
    /// --probe-report <json>: the checkpoints, the commands, the screenshots and RmlUi's log counts.
    std::optional<std::filesystem::path> report;
};

/// rv_ui_probe's logic: the directory (rv::ui::DirectoryScreen on assets/ui/directory.rml) over the scene of --scene
/// (the test room by default), from the fixed model of directory_fixture.hpp opened on its floors at frame 0, driven
/// by --input-script's UI actions through the action map and NavInput as retroverse.exe reads them (devices are not
/// read: the script alone drives the run); a game's card filled as the app would; at each checkpoint frame, the model's
/// and the screen's state recorded and the screenshot taken. A frame-limited run fails (exit 3) when a screenshot was
/// not written.
class UiProbeApp final : public app::IAppLogic {
public:
    explicit UiProbeApp(UiProbeOptions options);
    ~UiProbeApp() override;

    [[nodiscard]] Status onInit(app::AppContext& context) override;
    void onEvent(app::AppContext& context, const SDL_Event& event) override;
    void onUpdate(app::AppContext& context, double deltaSeconds) override;
    void onRender(app::AppContext& context) override;
    void onShutdown(app::AppContext& context) override;

private:
    struct State;

    UiProbeOptions m_options;
    std::unique_ptr<State> m_state;
};

} // namespace rv::probe
