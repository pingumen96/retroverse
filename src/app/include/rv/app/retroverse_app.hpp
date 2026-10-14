#pragma once

#include <memory>

#include <glm/vec4.hpp>

#include <rv/app/app_logic.hpp>

namespace rv::app {

class TriangleDemo;

/// The Retroverse application logic. M0 step 3 content: clears to the wall color of style §3 (#383735, converted to
/// linear) and draws one rotating triangle that proves buffers, vertex arrays, shaders and the sRGB output path. The
/// scene renderer replaces the triangle in later steps.
class RetroverseApp final : public IAppLogic {
public:
    RetroverseApp();
    ~RetroverseApp() override;
    RetroverseApp(const RetroverseApp&) = delete;
    RetroverseApp& operator=(const RetroverseApp&) = delete;

    [[nodiscard]] Status onInit(AppContext& context) override;
    void onUpdate(AppContext& context, double deltaSeconds) override;
    void onRender(AppContext& context) override;
    void onShutdown(AppContext& context) override;

private:
    glm::vec4 m_clearColorLinear{0.0f, 0.0f, 0.0f, 1.0f};
    std::unique_ptr<TriangleDemo> m_triangle;
};

} // namespace rv::app
