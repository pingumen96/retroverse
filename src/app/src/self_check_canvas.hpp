#pragma once

#include <memory>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>

namespace rv::gl {
class GLDevice;
class ShaderProgram;
class TextureDefaults;
struct PostSettings;
struct PostShaders;
} // namespace rv::gl

namespace rv::app {

/// What the sRGB and FXAA self-checks draw: one unlit quad of a known radiance over black, drawn with the scene's unlit
/// program into the HDR target of view targets like the window's and the screenshots' (gl::ViewTargets), taken through
/// the post-processing of the views (gl::PostProcessor) to their sRGB output and read back like a screenshot.
///
/// The quad is the 2 x 2 "quad" primitive facing +Z, one instance of a given transform, seen through identity view and
/// projection matrices: with the identity transform it covers the whole target. Its material is unlit (all of its light
/// is emitted, so it all blooms); no light and no ambient are bound. Move-only; main thread only, with the context
/// current.
class SelfCheckCanvas {
public:
    /// Creates the quad mesh and its instance, the material of linear `radiance`, the uniform blocks the unlit program
    /// reads, view targets of `size` pixels and a post-processor; `name` labels them. Errors: GPU objects that cannot
    /// be created.
    [[nodiscard]] static Expected<SelfCheckCanvas> create(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                                          glm::ivec2 size, const glm::mat4& quadTransform,
                                                          glm::vec3 radiance, std::string_view name);

    SelfCheckCanvas(SelfCheckCanvas&&) noexcept;
    SelfCheckCanvas& operator=(SelfCheckCanvas&&) noexcept;
    SelfCheckCanvas(const SelfCheckCanvas&) = delete;
    SelfCheckCanvas& operator=(const SelfCheckCanvas&) = delete;
    ~SelfCheckCanvas();

    /// Clears the HDR target to black, draws the quad with `unlit`, post-processes it with `post` and `settings` into
    /// the output and reads the output back (gl::Framebuffer::readPixels: RGBA8, sRGB-encoded bytes, top row first).
    /// The default framebuffer is bound afterwards. Errors: the read-back.
    [[nodiscard]] Expected<assets::ImageData> render(gl::GLDevice& device, const gl::ShaderProgram& unlit,
                                                     const gl::PostShaders& post, const gl::PostSettings& settings);
    /// Reads back, like render(), the tonemapped image the last render() wrote before its anti-aliasing
    /// (gl::ViewTargets::tonemapped(): written over the emitted light's storage, docs/i5/memory-margin.md). Meaningful
    /// only after a render() with anti-aliasing. Errors: the read-back.
    [[nodiscard]] Expected<assets::ImageData> readTonemapped() const;

private:
    struct State;

    explicit SelfCheckCanvas(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_state;
};

} // namespace rv::app
