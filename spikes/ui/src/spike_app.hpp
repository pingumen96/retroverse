#pragma once

#include <memory>

#include <rv/app/app_logic.hpp>

#include "spike_options.hpp"

namespace rv::spike_ui {

/// The application of spike_ui on app::AppHost: the test room through app::SceneGraphics (unchanged rv_app), the quick
/// catalog screen of rv_ui over it (ui::UiSystem, ui::UiView, ui::QuickCatalogScreen on the mock catalog), input from a
/// platform::SdlInputCollector through ui::NavInput, and, unless --interactive, the scripted scenario driving a
/// virtual gamepad (defaultScenario) with its checks; scenario runs hear the virtual pad only, never a real one.
/// Records the UI pass of every frame (CPU and GPU times, whether the UI changed, heap allocations through
/// AllocationCounter), every focus change, the actions, the layouts and the screenshots, and writes the report
/// (--report) when the run ends; a failed check, a check the run ended before, or an RmlUi error or warning fails the
/// run (exit code 3).
class SpikeApp final : public app::IAppLogic {
public:
    explicit SpikeApp(SpikeOptions options);
    ~SpikeApp() override;
    SpikeApp(const SpikeApp&) = delete;
    SpikeApp& operator=(const SpikeApp&) = delete;

    [[nodiscard]] Status onInit(app::AppContext& context) override;
    void onEvent(app::AppContext& context, const SDL_Event& event) override;
    void onUpdate(app::AppContext& context, double deltaSeconds) override;
    void onRender(app::AppContext& context) override;
    void onShutdown(app::AppContext& context) override;

private:
    struct State;

    SpikeOptions m_options;
    std::unique_ptr<State> m_state;
};

} // namespace rv::spike_ui
