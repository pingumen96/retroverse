#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <rv/app/scene_instance.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/post_processor.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/render/surface_view.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/debug_lines.hpp>

namespace rv::assets {
class MaterialLibrary;
} // namespace rv::assets

namespace rv::gl {
struct FrameStats;
class Framebuffer;
class GLDevice;
class MaterialRegistry;
class MeshRegistry;
class ShaderLibrary;
class ShaderProgram;
class TextureCache;
class TextureDefaults;
} // namespace rv::gl

namespace rv::app {

class AppContext;

/// The view of `scene` from `camera`: its render items, lights and key light, the bounds of its shadow casters, its
/// ambient occlusion, background and post-processing (gl::postSettingsFor its environment), with the default shadow
/// settings, at time 0. The caller sets what it controls itself (the time, the shadow settings, wireframe,
/// anti-aliasing).
[[nodiscard]] gl::RenderView makeRenderView(const SceneInstance& scene, const scene::Camera& camera);

/// What every view of a SceneInstance needs on the GPU, shared by the applications built on rv_app (RetroverseApp, the
/// asset preview tool), and the frame they render with it:
///  - the material library (assets/materials/base/materials.json, or the built-in fallback when it cannot be loaded);
///  - the shader library with the programs of the views (lit, unlit emissive, depth, ambient occlusion,
///    post-processing; the debug lines and the inspection views of gl::SurfaceView when first used), reloaded by
///    reloadShaders() (F5) and, in developer builds (RV_DEV_ASSET_ROOT), by pollShaderReload() when their files change;
///  - the texture defaults and cache and the mesh and material registries that a SceneInstance registers its content
///    in (resources()); clearContent() empties them for the next one (the cache keeps its procedural surfaces);
///  - the renderer (gl::SceneRenderer) and the debug lines drawn over every view (setDebugLines());
///  - the render targets of the window and of the screenshots, each kept at its size (a failure to allocate them is
///    logged once per size), and the frame statistics log (FrameStatsLog: once per second in developer builds, and for
///    the whole run when a frame-limited run ends).
/// Main thread only; create it in IAppLogic::onInit and destroy it in onShutdown, while the GL context is current.
class SceneGraphics {
public:
    /// Creates everything above for the run of `context`. Errors: GPU objects that cannot be created, invalid program
    /// descriptions. A program that does not build is logged and stays empty until a reload succeeds (its batches are
    /// not drawn; a frame-limited run then fails at finishRun()).
    [[nodiscard]] static Expected<std::unique_ptr<SceneGraphics>> create(AppContext& context);
    ~SceneGraphics();
    SceneGraphics(const SceneGraphics&) = delete;
    SceneGraphics& operator=(const SceneGraphics&) = delete;

    [[nodiscard]] const assets::MaterialLibrary& materialLibrary() const noexcept;
    /// Where a SceneInstance registers its content.
    [[nodiscard]] SceneResources resources() noexcept;
    /// Releases every mesh, material and texture registered so far except the procedural surface maps, which no
    /// content changes (gl::TextureCache::clearContent): the render items of the previous SceneInstance must not be
    /// drawn any more. For new content in the same run (the asset preview loads another file).
    void clearContent() noexcept;

    [[nodiscard]] gl::ShaderLibrary& shaders() noexcept;
    [[nodiscard]] gl::MeshRegistry& meshes() noexcept;
    [[nodiscard]] gl::MaterialRegistry& materials() noexcept;
    [[nodiscard]] gl::TextureCache& textures() noexcept;
    [[nodiscard]] const gl::TextureDefaults& textureDefaults() const noexcept;
    /// The statistics of the last view rendered (gl::SceneRenderer::stats()).
    [[nodiscard]] const gl::FrameStats& renderStats() const noexcept;
    /// The post-processing programs and the unlit program of the views, for the rendering self-checks.
    [[nodiscard]] gl::PostShaders postShaders() const noexcept;
    [[nodiscard]] const gl::ShaderProgram& unlitProgram() const noexcept;

    /// Lines drawn over every view from now on, depth-tested against the scene (gl::DebugDraw); empty lines draw none.
    /// The program and the line buffer are created when lines are first set. Errors: the program description or the
    /// buffer (no lines are drawn then).
    [[nodiscard]] Status setDebugLines(const scene::DebugLines& lines);
    /// What the surfaces of the views show from now on: their materials' shading (gl::SurfaceView::Lit), or an
    /// inspection view, drawn with its program (loaded the first time) and shown without tonemapping, bloom,
    /// anti-aliasing, ambient occlusion or shadows, on the background the lit view displays (gl::displayColor).
    /// Errors: the program description (the view is unchanged then).
    [[nodiscard]] Status setSurfaceView(gl::SurfaceView view);
    [[nodiscard]] gl::SurfaceView surfaceView() const noexcept;
    /// Draws an overlay (the developer overlay) over every window frame (`window`, after the scene) and over the
    /// screenshots (`screenshot`, given the screenshot's output; empty: screenshots show the scene alone). Set once.
    void setOverlay(std::function<void(gl::GLDevice&)> window,
                    std::function<void(gl::GLDevice&, const gl::Framebuffer&)> screenshot);

    /// Renders the frame of `view` (its camera's aspect follows each target):
    ///  1. the screenshots first, so that the frame ends with the window: with `screenshotRequested` (F12)
    ///     <user data>/screenshots/retroverse-<timestamp>.png, a failure logged; on the last frame of a frame-limited
    ///     run with --screenshot that file, a failure failing the run (AppContext::failRun). Both at
    ///     --screenshot-size, with the overlay when one is set for screenshots, written as RGB PNG (the window is
    ///     opaque);
    ///  2. the window: the view into the window's targets, copied to the window (black when the targets cannot be
    ///     allocated), then the overlay;
    ///  3. the frame statistics (and, in developer builds, their summary once per second).
    void renderFrame(AppContext& context, const gl::RenderView& view, bool screenshotRequested);

    /// F5: rebuilds every program.
    void reloadShaders();
    /// Rebuilds the programs whose files changed, in developer builds (RV_DEV_ASSET_ROOT); nothing otherwise. Cheap to
    /// call every frame.
    void pollShaderReload();

    /// The end of a run, from IAppLogic::onShutdown: adds to the run report (AppContext::report) the statistics of the
    /// last view rendered, the instance buffer's capacity and the --screenshot written. A frame-limited run also logs
    /// the frame statistics of the run, and fails when a program of the views never built (frames missed surfaces, the
    /// bloom or the tonemap) or when the --screenshot of its last frame was not written (the run ended before it).
    void finishRun(AppContext& context);

private:
    struct State;

    explicit SceneGraphics(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_state;
};

} // namespace rv::app
