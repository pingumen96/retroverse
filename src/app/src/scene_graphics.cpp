#include <rv/app/scene_graphics.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/frame_stats_log.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/screenshot_name.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/render/debug_draw.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/render_state_cache.hpp>
#include <rv/render/shader_library.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_array_layout.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/view_targets.hpp>
#include <rv/scene/stereo.hpp>

namespace rv::app {
namespace {

#if defined(RV_DEV_ASSET_ROOT)
constexpr bool kShaderHotReload = true;
#else
constexpr bool kShaderHotReload = false;
#endif

#if defined(RV_DEV_UI)
/// Developer builds (with the developer overlay) log the frame statistics once per second.
constexpr bool kDeveloperBuild = true;
#else
constexpr bool kDeveloperBuild = false;
#endif

constexpr std::string_view kMaterialLibraryPath = "materials/base/materials.json";
/// Anisotropic filtering of textures and material samplers, and of the checker that replaces missing textures (plan
/// 5.4: 8x), clamped to the device limit.
constexpr float kTextureAnisotropy = 8.0f;
/// The inspection views, in the order of gl::kSurfaceViews without Lit.
constexpr std::array<gl::SurfaceView, 3> kInspectionViews{gl::SurfaceView::BaseColor, gl::SurfaceView::Normals,
                                                          gl::SurfaceView::UvChecker};

assets::MaterialLibrary loadMaterials(const core::Paths& paths) {
    auto library = assets::MaterialLibrary::loadFromFile(paths.assetRoot() / core::pathFromUtf8(kMaterialLibraryPath));
    if (library) {
        return std::move(*library);
    }
    assets::MaterialLibrary builtin;
    spdlog::error("Material library unavailable, every material falls back to {}: {}", builtin.fallback().name,
                  library.error().toString());
    return builtin;
}

/// The CRT presets (I5 step 6): assets/materials/crt_presets.json with the user's over it; a file that does not load
/// is said in the log (the user's left out, the shipped one's built-in copy used).
ScreenLooks loadScreenLooks(const core::Paths& paths) {
    assets::LoadedCrtPresets loaded = assets::loadCrtPresets(paths.assetRoot(), paths.configDir());
    for (const Error& issue : loaded.issues) {
        if (loaded.source == "built-in") {
            spdlog::error("CRT presets: the shipped crt_presets.json does not load, the built-in values are used: {}",
                          issue.toString());
        } else {
            spdlog::warn("CRT presets: the user's crt_presets.json is left out, the shipped values used: {}",
                         issue.toString());
        }
    }
    spdlog::info("CRT presets: the {} values", loaded.source);
    return ScreenLooks(loaded.presets, std::string(loaded.source));
}

/// The programs of the scene, its shadow and ambient occlusion, its post-processing and the hover outline
/// (ShaderLibrary handles), loaded at startup.
struct ScenePrograms {
    gl::ShaderHandle pbr;
    gl::ShaderHandle unlit;
    gl::ShaderHandle depth;
    gl::ShaderHandle depthMasked;
    gl::ShaderHandle ssao;
    gl::ShaderHandle ssaoBlur;
    gl::ShaderHandle bloomDownsampleFirst;
    gl::ShaderHandle bloomDownsample;
    gl::ShaderHandle bloomUpsample;
    gl::ShaderHandle post;
    gl::ShaderHandle antialiasing;
    gl::ShaderHandle outlineMark;
    gl::ShaderHandle outlineMarkMasked;
    gl::ShaderHandle outlineEdge;
    gl::ShaderHandle outlineDim;
};

/// Loads the programs of ScenePrograms. A program that does not build is logged and stays empty until a reload
/// succeeds (ShaderLibrary::load); errors only for an invalid description.
Expected<ScenePrograms> loadScenePrograms(gl::ShaderLibrary& library) {
    ScenePrograms programs;
    const std::pair<gl::ShaderHandle ScenePrograms::*, gl::ShaderDesc> descriptions[] = {
        {&ScenePrograms::pbr, {"pbr", "pbr/pbr.vert", "pbr/pbr.frag", {}}},
        {&ScenePrograms::unlit, {"unlit_emissive", "pbr/pbr.vert", "unlit/unlit_emissive.frag", {}}},
        {&ScenePrograms::depth, {"depth", "pbr/pbr.vert", "depth/depth.frag", {}}},
        {&ScenePrograms::depthMasked, {"depth_masked", "pbr/pbr.vert", "depth/depth.frag", {"RV_ALPHA_TEST"}}},
        {&ScenePrograms::ssao, {"ssao", "post/fullscreen.vert", "ssao/ssao.frag", {}}},
        {&ScenePrograms::ssaoBlur, {"ssao_blur", "post/fullscreen.vert", "ssao/ssao_blur.frag", {}}},
        {&ScenePrograms::bloomDownsampleFirst,
         {"bloom_downsample_karis", "post/fullscreen.vert", "post/bloom_downsample.frag", {"RV_BLOOM_KARIS"}}},
        {&ScenePrograms::bloomDownsample,
         {"bloom_downsample", "post/fullscreen.vert", "post/bloom_downsample.frag", {}}},
        {&ScenePrograms::bloomUpsample, {"bloom_upsample", "post/fullscreen.vert", "post/bloom_upsample.frag", {}}},
        {&ScenePrograms::post, {"post", "post/fullscreen.vert", "post/post.frag", {}}},
        {&ScenePrograms::antialiasing, {"fxaa", "post/fullscreen.vert", "post/fxaa.frag", {}}},
        {&ScenePrograms::outlineMark, {"outline_mark", "pbr/pbr.vert", "outline/outline_mark.frag", {}}},
        {&ScenePrograms::outlineMarkMasked,
         {"outline_mark_masked", "pbr/pbr.vert", "outline/outline_mark.frag", {"RV_ALPHA_TEST"}}},
        {&ScenePrograms::outlineEdge, {"outline_edge", "post/fullscreen.vert", "outline/outline_edge.frag", {}}},
        {&ScenePrograms::outlineDim, {"outline_dim", "post/fullscreen.vert", "outline/outline_dim.frag", {}}},
    };
    for (const auto& [handle, description] : descriptions) {
        auto loaded = library.load(description);
        if (!loaded) {
            return propagate(loaded.error());
        }
        programs.*handle = *loaded;
    }
    return programs;
}

/// Creates the render targets of an output (`targets`) at `size`, or resizes them. Failures are logged once per size so
/// a persistent one does not flood the log at the frame rate.
bool prepareTargets(gl::ViewTargets& targets, glm::ivec2 size, std::string_view debugName,
                    std::optional<glm::ivec2>& failedSize) {
    Status status = ok();
    if (!targets.valid()) {
        auto created = gl::ViewTargets::create(size, debugName);
        if (created) {
            targets = std::move(*created);
        } else {
            status = propagate(created.error());
        }
    } else {
        status = targets.resize(size);
    }
    if (status) {
        failedSize.reset();
        return true;
    }
    if (failedSize != size) {
        spdlog::error("Render targets '{}' unavailable at {}x{}: {}", debugName, size.x, size.y,
                      status.error().toString());
        failedSize = size;
    }
    return false;
}

/// Creates the side-by-side image of the stereo preview (`image`) at `size`, or resizes it; failures logged once per
/// size like prepareTargets().
bool prepareStereoImage(std::optional<gl::Framebuffer>& image, glm::ivec2 size, std::string_view debugName,
                        std::optional<glm::ivec2>& failedSize) {
    Status status = ok();
    if (!image) {
        // No depth: the eyes are copied in, nothing is drawn with a depth test (docs/i5/memory-margin.md).
        auto created =
            gl::Framebuffer::create(gl::FramebufferDesc{size, gl::ColorFormat::Srgb8Alpha8, false}, debugName);
        if (created) {
            image = std::move(*created);
        } else {
            status = propagate(created.error());
        }
    } else {
        status = image->resize(size);
    }
    if (status) {
        failedSize.reset();
        return true;
    }
    if (failedSize != size) {
        spdlog::error("Stereo image '{}' unavailable at {}x{}: {}", debugName, size.x, size.y,
                      status.error().toString());
        failedSize = size;
    }
    return false;
}

/// Index of an inspection view in kInspectionViews.
std::size_t inspectionIndex(gl::SurfaceView view) noexcept {
    for (std::size_t index = 0; index < kInspectionViews.size(); ++index) {
        if (kInspectionViews[index] == view) {
            return index;
        }
    }
    return 0;
}

} // namespace

gl::RenderView makeRenderView(const SceneInstance& scene, const scene::Camera& camera) {
    // The eye's area's exposure in a scene with areas, as eased so far (I6 step 1).
    const scene::EnvironmentSetup environment = scene.viewEnvironment();
    gl::RenderView view;
    view.camera = camera;
    view.items = scene.renderItems();
    view.lights = scene.lights();
    view.shadowLight = scene.shadowLight();
    view.shadowCasterBounds = scene.shadowCasterBounds();
    view.ssao = environment.ssao;
    view.clearColor = glm::vec4(environment.clearColor, 1.0f);
    view.post = gl::postSettingsFor(environment);
    return view;
}

struct SceneGraphics::State {
    explicit State(assets::MaterialLibrary materialLibrary) : library(std::move(materialLibrary)) {}

    assets::MaterialLibrary library;
    /// The screens' CRT looks (I5 step 6), which the scene instances' screens take.
    ScreenLooks looks;
    std::unique_ptr<gl::ShaderLibrary> shaders;
    ScenePrograms programs;
    std::optional<gl::ShaderHandle> debugShader; ///< loaded when debug lines are first set
    std::array<std::optional<gl::ShaderHandle>, kInspectionViews.size()> inspectionShaders; ///< loaded when first shown
    gl::SurfaceView surfaceView = gl::SurfaceView::Lit;
    // Declared before the registries and the renderer, which refer to them, so that they are destroyed after.
    std::unique_ptr<gl::TextureDefaults> textureDefaults;
    std::unique_ptr<gl::TextureCache> textures;
    gl::MaterialRegistry materials;
    gl::MeshRegistry meshes;
    std::optional<gl::SceneRenderer> renderer;
    int uniformBufferOffsetAlignment = 256;
    /// The storage of the collection's texture arrays in this run (ADR-0011 §4).
    gl::CollectionArrayFormat collectionArrayFormat = gl::CollectionArrayFormat::Bc1Srgb;
    std::optional<gl::DebugDraw> debugDraw; ///< created when debug lines are first set
    bool debugLinesShown = false;
    std::function<void(gl::GLDevice&)> windowOverlay;
    std::function<void(gl::GLDevice&, const gl::Framebuffer&)> screenshotOverlay;
    /// Render targets of the window's view, kept at its size (allocated on first use), and of a screenshot of another
    /// size, made for it alone: one set is held at any time (captureScreenshot()).
    gl::ViewTargets windowTargets;
    gl::ViewTargets screenshotTargets;
    std::optional<glm::ivec2> failedWindowSize;
    std::optional<glm::ivec2> failedScreenshotSize;
    /// --stereo-preview: the eyes' targets (the window's and the screenshots' alike, at the eye size of the image being
    /// drawn) and the side-by-side images of the window and the screenshots.
    std::array<gl::ViewTargets, 2> eyeTargets;
    std::array<std::optional<glm::ivec2>, 2> failedEyeSize;
    std::optional<gl::Framebuffer> windowStereo;
    std::optional<gl::Framebuffer> screenshotStereo;
    std::optional<glm::ivec2> failedWindowStereoSize;
    std::optional<glm::ivec2> failedScreenshotStereoSize;
    /// VR (I10 step 2): the eyes render into eyeTargets too; framebuffers over the swapchains' images, by texture name;
    /// the side-by-side image of the screenshots.
    std::map<std::uint32_t, gl::TextureTarget> swapchainTargets;
    /// The VR mode's panels (I10 step 5): their program and pass, loaded when first drawn; a failure is logged once.
    std::optional<gl::ShaderHandle> panelShader;
    std::optional<gl::PanelPass> panelPass;
    bool panelFailed = false;
    std::optional<gl::Framebuffer> xrScreenshot;
    std::optional<glm::ivec2> failedXrScreenshotSize;
    bool swapchainTargetFailed = false;
    /// The size of the last screenshot written (the VR mode's differs from --screenshot-size).
    glm::ivec2 lastScreenshotSize{0};
    FrameStatsLog statsLog;
    bool lastFrameCaptured = false; ///< --screenshot written
    /// The targets the last screenshot was drawn through (kScreenshotWindowTargets, kScreenshotOwnTargets or
    /// kScreenshotEyesTargets).
    std::string_view screenshotTargetsUsed;
    std::string_view lastFrameTargets; ///< those of the --screenshot
    bool viewRendered = false;      ///< a view was rendered during the current frame (window or screenshot)
    bool anyViewRendered = false;   ///< a view was rendered during the run

    /// Renders `view` into `targets` with the programs of the views, the debug lines and the surface view.
    void render(gl::GLDevice& device, const gl::RenderView& view, const gl::ViewTargets& targets) {
        gl::SceneShaders shaderSet;
        shaderSet.pbr = &shaders->get(programs.pbr);
        shaderSet.unlit = &shaders->get(programs.unlit);
        shaderSet.depth = &shaders->get(programs.depth);
        shaderSet.depthMasked = &shaders->get(programs.depthMasked);
        shaderSet.debugLines = debugShader ? &shaders->get(*debugShader) : nullptr;
        shaderSet.ssao.occlusion = &shaders->get(programs.ssao);
        shaderSet.ssao.blur = &shaders->get(programs.ssaoBlur);
        shaderSet.post = postShaders();
        shaderSet.outline.mark = &shaders->get(programs.outlineMark);
        shaderSet.outline.markMasked = &shaders->get(programs.outlineMarkMasked);
        shaderSet.outline.edge = &shaders->get(programs.outlineEdge);
        shaderSet.outline.dim = &shaders->get(programs.outlineDim);
        gl::RenderView shown = view;
        shown.debugDraw = debugDraw && debugLinesShown ? &*debugDraw : nullptr;
        if (surfaceView != gl::SurfaceView::Lit) {
            // The inspection programs write the colors to display: nothing may darken, shift or tonemap them. The
            // background is the one the lit view displays, so that switching views keeps the backdrop.
            shaderSet.inspection = &shaders->get(*inspectionShaders[inspectionIndex(surfaceView)]);
            shown.clearColor = glm::vec4(gl::displayColor(glm::vec3(view.clearColor), view.post), view.clearColor.a);
            shown.post.tonemap = false;
            shown.ssao.enabled = false;
            shown.shadowSettings.enabled = false;
        }
        {
            const core::CpuSectionScope timing(core::cpu_section::kSceneSubmit);
            renderer->render(device, shaderSet, shown, targets);
        }
        viewRendered = true;
        anyViewRendered = true;
    }

    /// Renders the eyes of the camera of `view` (scene::stereoEyesFor: the preview's field, 64 mm apart) side by side
    /// into `image`, on black: both culled against one frustum enclosing them, the second reading the first's shadow
    /// map. False when the eyes' targets are unavailable (logged).
    bool renderStereo(gl::GLDevice& device, const gl::RenderView& view, const gl::Framebuffer& image) {
        const scene::EyeFov field = scene::stereoPreviewLeftField();
        const scene::StereoLayout layout = scene::stereoLayout(image.size(), field);
        if (layout.eyeSize.x < 1 || layout.eyeSize.y < 1) {
            return false;
        }
        constexpr std::array<std::string_view, 2> kNames{"StereoLeftTargets", "StereoRightTargets"};
        for (std::size_t eye = 0; eye < eyeTargets.size(); ++eye) {
            if (!prepareTargets(eyeTargets[eye], layout.eyeSize, kNames[eye], failedEyeSize[eye])) {
                return false;
            }
        }
        const scene::StereoEyes eyes = scene::stereoEyesFor(view.camera, scene::kStereoPreviewIpdMeters, field);
        const std::array<glm::mat4, 2> viewProjections{eyes.left.viewProjection(), eyes.right.viewProjection()};
        gl::RenderView eyeView = view;
        eyeView.cullFrustum = scene::Frustum::enclosing(viewProjections);
        eyeView.camera = eyes.left;
        render(device, eyeView, eyeTargets[0]);
        eyeView.camera = eyes.right;
        render(device, eyeView, eyeTargets[1]);

        gl::RenderStateCache& state = device.state();
        device.beginPass(image, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        // The eyes' bytes copied as they are: the image is display-ready like each eye's output.
        state.setFramebufferSrgb(false);
        eyeTargets[0].output().copyColorTo(image, layout.leftOffset);
        eyeTargets[1].output().copyColorTo(image, layout.rightOffset);
        image.bind();
        state.setViewport(glm::ivec4(0, 0, image.size().x, image.size().y));
        state.setFramebufferSrgb(true);
        return true;
    }

    /// The framebuffer over the swapchain image `image` of `size`, created when first used (and again for another
    /// size); null when it cannot be (logged once).
    gl::TextureTarget* swapchainTarget(std::uint32_t image, glm::ivec2 size) {
        auto found = swapchainTargets.find(image);
        if (found != swapchainTargets.end() && found->second.size() == size) {
            return &found->second;
        }
        auto created = gl::TextureTarget::create(image, size, fmt::format("XrSwapchainImage{}", image));
        if (!created) {
            if (!swapchainTargetFailed) {
                spdlog::error("VR swapchain image {} cannot be drawn into: {}", image, created.error().toString());
                swapchainTargetFailed = true;
            }
            return nullptr;
        }
        return &(swapchainTargets[image] = std::move(*created));
    }

    /// The panels' program and pass, loaded once; false (logged once) when they cannot be.
    bool preparePanels() {
        if (panelShader && panelPass) {
            return true;
        }
        if (panelFailed) {
            return false;
        }
        auto program = shaders->load(gl::ShaderDesc{"xr_panel", "xr/panel.vert", "xr/panel.frag", {}});
        auto pass = gl::PanelPass::create();
        if (!program || !pass) {
            spdlog::error("The VR mode's UI panel cannot be drawn: {}",
                          !program ? program.error().toString() : pass.error().toString());
            panelFailed = true;
            return false;
        }
        panelShader = *program;
        panelPass = std::move(*pass);
        return true;
    }

    /// Renders both eyes of a VR frame into their targets (eyeTargets) and copies each into its swapchain image. False
    /// when a target or an image cannot be drawn into (logged).
    bool renderEyes(gl::GLDevice& device, const gl::RenderView& view, const XrEyeTargets& eyes) {
        constexpr std::array<std::string_view, 2> kNames{"XrLeftTargets", "XrRightTargets"};
        // I10 step 7: the eyes at the dynamic render scale, scaled up into the images (copyColorTo filters then).
        const float scale = std::clamp(std::isfinite(eyes.renderScale) ? eyes.renderScale : 1.0f, 0.1f, 1.0f);
        const glm::ivec2 rendered =
            scale < 1.0f ? glm::max(glm::ivec2(glm::round(glm::vec2(eyes.size) * scale)), glm::ivec2(16)) : eyes.size;
        for (std::size_t eye = 0; eye < eyeTargets.size(); ++eye) {
            if (!prepareTargets(eyeTargets[eye], rendered, kNames[eye], failedEyeSize[eye])) {
                return false;
            }
        }
        const std::array<glm::mat4, 2> viewProjections{eyes.cameras[0].viewProjection(),
                                                       eyes.cameras[1].viewProjection()};
        gl::RenderView eyeView = view;
        eyeView.cullFrustum = scene::Frustum::enclosing(viewProjections);
        // ADR-0017 §4: no vignette in a headset (the lenses darken their edges already).
        eyeView.post.vignette = 0.0f;
        // I10 step 9: the fast profile (120 Hz) leaves out the passes that cost the most for the least seen.
        if (eyes.fast) {
            eyeView.ssao.enabled = false;
            eyeView.post.antialiasing = false;
        }
        const bool panels = !eyes.panels.empty() && preparePanels();
        for (std::size_t eye = 0; eye < eyeTargets.size(); ++eye) {
            eyeView.camera = eyes.cameras[eye];
            // The comfort vignette closes in on where the eye looks straight ahead (I10 step 3).
            const scene::EyeFov& field = eyes.fields[eye];
            eyeView.post.tunnelCenter =
                glm::vec2(std::tan(-field.left) / (std::tan(field.right) - std::tan(field.left)),
                          std::tan(-field.down) / (std::tan(field.up) - std::tan(field.down)));
            render(device, eyeView, eyeTargets[eye]);
            // The UI's panel over the eye's display-ready picture (I10 step 5).
            if (panels) {
                panelPass->draw(shaders->get(*panelShader), device.state(), eyeTargets[eye].output(),
                                eyes.cameras[eye].viewProjection(), eyes.panels);
            }
        }
        gl::RenderStateCache& state = device.state();
        // The eyes' display-ready bytes copied as they are into the sRGB images.
        state.setFramebufferSrgb(false);
        bool copied = true;
        for (std::size_t eye = 0; eye < eyeTargets.size(); ++eye) {
            gl::TextureTarget* target = swapchainTarget(eyes.images[eye], eyes.size);
            if (target == nullptr) {
                copied = false;
                continue;
            }
            eyeTargets[eye].output().copyColorTo(*target);
        }
        state.setFramebufferSrgb(true);
        return copied;
    }

    /// Back on the desktop (a VR session that ended, I10 step 2): the eyes' targets and the framebuffers over the
    /// swapchains' images are let go of, so that the window's targets are the only set held.
    /// `vr`: the session still runs (a frame without its eyes, tracking lost), and they are kept for the next one.
    void releaseEyes(bool vr) {
        if (vr || (!eyeTargets[0].valid() && !eyeTargets[1].valid() && swapchainTargets.empty())) {
            return;
        }
        eyeTargets = {};
        swapchainTargets.clear();
    }

    /// The VR screenshot: both eyes, as renderEyes() left them, side by side at their size. False when the image cannot
    /// be allocated (logged).
    bool composeEyes(gl::GLDevice& device) {
        const glm::ivec2 eye = eyeTargets[0].output().size();
        if (!prepareStereoImage(xrScreenshot, glm::ivec2(2 * eye.x, eye.y), "XrScreenshot", failedXrScreenshotSize)) {
            return false;
        }
        gl::RenderStateCache& state = device.state();
        device.beginPass(*xrScreenshot, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        state.setFramebufferSrgb(false);
        eyeTargets[0].output().copyColorTo(*xrScreenshot, glm::ivec2(0));
        eyeTargets[1].output().copyColorTo(*xrScreenshot, glm::ivec2(eye.x, 0));
        xrScreenshot->bind();
        state.setViewport(glm::ivec4(0, 0, xrScreenshot->size().x, xrScreenshot->size().y));
        state.setFramebufferSrgb(true);
        return true;
    }

    [[nodiscard]] gl::PostShaders postShaders() const {
        gl::PostShaders post;
        post.bloomDownsampleFirst = &shaders->get(programs.bloomDownsampleFirst);
        post.bloomDownsample = &shaders->get(programs.bloomDownsample);
        post.bloomUpsample = &shaders->get(programs.bloomUpsample);
        post.post = &shaders->get(programs.post);
        post.antialiasing = &shaders->get(programs.antialiasing);
        return post;
    }

    /// Renders `view` at --screenshot-size, with the overlay when it shows in screenshots, and writes it as an RGB PNG.
    /// At the window's size it is drawn through the window's targets, which the window's view draws anew right after;
    /// at another size through targets made for it alone, with the window's let go of first (the window's view makes
    /// them again): one set of targets is held at any time (docs/i5/memory-margin.md). A VR frame's (`xrEyes`: its
    /// eyes rendered, I10 step 2) is both eyes side by side at their size, and the stereo preview's (I10 step 1) both
    /// preview eyes: each from the eyes' targets, the side-by-side image let go of once written.
    Status captureScreenshot(AppContext& context, const gl::RenderView& view, const std::filesystem::path& path,
                             bool xrEyes) {
        const CommandLineOptions& options = context.options();
        if (xrEyes || options.stereoPreview) {
            screenshotTargetsUsed = kScreenshotEyesTargets;
            Status captured = captureEyes(context, view, path, xrEyes);
            xrScreenshot.reset();
            screenshotStereo.reset();
            return captured;
        }
        const glm::ivec2 size(options.screenshotWidth, options.screenshotHeight);
        const bool windowSized = size == context.framebufferSize();
        screenshotTargetsUsed = windowSized ? kScreenshotWindowTargets : kScreenshotOwnTargets;
        if (!windowSized) {
            windowTargets = gl::ViewTargets{};
        }
        Status captured = captureInto(context, view, path, windowSized);
        if (!windowSized) {
            screenshotTargets = gl::ViewTargets{};
        }
        return captured;
    }

    /// captureScreenshot() through the window's targets (`windowSized`) or the screenshot's.
    Status captureInto(AppContext& context, const gl::RenderView& view, const std::filesystem::path& path,
                       bool windowSized) {
        const CommandLineOptions& options = context.options();
        const glm::ivec2 size(options.screenshotWidth, options.screenshotHeight);
        gl::ViewTargets& targets = windowSized ? windowTargets : screenshotTargets;
        const bool prepared = windowSized
                                  ? prepareTargets(windowTargets, size, "WindowTargets", failedWindowSize)
                                  : prepareTargets(screenshotTargets, size, "ScreenshotTargets", failedScreenshotSize);
        if (!prepared) {
            return makeError(ErrorCode::GraphicsError, "no render targets for the screenshot", core::pathToUtf8(path));
        }
        render(context.device(), view, targets);
        return writeScreenshot(context, targets.output(), path, size);
    }

    /// captureScreenshot() of both eyes: a VR frame's as renderEyes() left them (`xrEyes`), else the stereo preview's
    /// at --screenshot-size.
    Status captureEyes(AppContext& context, const gl::RenderView& view, const std::filesystem::path& path,
                       bool xrEyes) {
        const CommandLineOptions& options = context.options();
        if (xrEyes) {
            if (!composeEyes(context.device())) {
                return makeError(ErrorCode::GraphicsError, "no image for the VR screenshot", core::pathToUtf8(path));
            }
            return writeScreenshot(context, *xrScreenshot, path, xrScreenshot->size());
        }
        const glm::ivec2 size(options.screenshotWidth, options.screenshotHeight);
        if (!prepareStereoImage(screenshotStereo, size, "ScreenshotStereo", failedScreenshotStereoSize) ||
            !renderStereo(context.device(), view, *screenshotStereo)) {
            return makeError(ErrorCode::GraphicsError, "no render targets for the stereo screenshot",
                             core::pathToUtf8(path));
        }
        return writeScreenshot(context, *screenshotStereo, path, size);
    }

    /// The screenshot's last part: the overlay over `output` when it shows in screenshots, then `output` read back and
    /// written as an RGB PNG.
    Status writeScreenshot(AppContext& context, const gl::Framebuffer& output, const std::filesystem::path& path,
                           glm::ivec2 size) {
        if (screenshotOverlay) {
            screenshotOverlay(context.device(), output);
        }
        gl::Framebuffer::unbind(); // the frame leaves the default framebuffer bound, also when the window is not drawn
        auto image = output.readPixels();
        if (!image) {
            return propagate(image.error());
        }
        // The window is opaque: it shows the stored color whatever alpha the passes wrote (a material's alpha, say).
        // Dropping the alpha channel makes the PNG show the same pixels.
        if (auto written = assets::writePng(path, assets::convertChannels(*image, 3)); !written) {
            return propagate(written.error());
        }
        spdlog::info("Screenshot written: {} ({}x{})", core::pathToUtf8(path), size.x, size.y);
        lastScreenshotSize = size;
        return ok();
    }

    /// The bytes of every texture the cache and the defaults hold, each from its format, size and levels
    /// (gl::Texture2D::estimatedBytes).
    [[nodiscard]] std::uint64_t heldTextureBytes() const {
        std::uint64_t bytes = textures->estimatedBytes();
        for (const gl::Texture2D* texture : {&textureDefaults->white(), &textureDefaults->black(),
                                             &textureDefaults->flatNormal(), &textureDefaults->checker()}) {
            bytes += texture->estimatedBytes();
        }
        return bytes;
    }

    /// Name of a program of the views that never built, or empty when all did.
    [[nodiscard]] std::string_view missingProgram() const {
        for (std::size_t index = 0; index < shaders->size(); ++index) {
            if (const gl::ShaderStatus status = shaders->status(index); !status.valid) {
                return status.name;
            }
        }
        return {};
    }
};

SceneGraphics::SceneGraphics(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}

SceneGraphics::~SceneGraphics() = default;

Expected<std::unique_ptr<SceneGraphics>> SceneGraphics::create(AppContext& context) {
    const core::Paths& paths = context.paths();
    auto state = std::make_unique<State>(loadMaterials(paths));
    state->looks = loadScreenLooks(paths);
    state->shaders = std::make_unique<gl::ShaderLibrary>(paths.assetRoot() / "shaders");
    auto programs = loadScenePrograms(*state->shaders);
    if (!programs) {
        return propagate(programs.error());
    }
    state->programs = *programs;
    spdlog::info("Shader hot reload {}; F5 reloads every shader",
                 kShaderHotReload ? "on (sources checked every 500 ms)" : "off in this build");

    const gl::GLCapabilities& caps = context.device().caps();
    const float anisotropy = gl::clampAnisotropy(kTextureAnisotropy, caps.maxAnisotropy);
    auto textureDefaults = gl::TextureDefaults::create(anisotropy);
    if (!textureDefaults) {
        return propagate(textureDefaults.error());
    }
    state->textureDefaults = std::make_unique<gl::TextureDefaults>(std::move(*textureDefaults));
    state->textures = std::make_unique<gl::TextureCache>(paths.assetRoot(), *state->textureDefaults, anisotropy);
    auto materials = gl::MaterialRegistry::create(*state->textureDefaults, anisotropy);
    if (!materials) {
        return propagate(materials.error());
    }
    state->materials = std::move(*materials);
    auto renderer = gl::SceneRenderer::create(state->meshes, state->materials);
    if (!renderer) {
        return propagate(renderer.error());
    }
    state->renderer.emplace(std::move(*renderer));
    state->uniformBufferOffsetAlignment = caps.uniformBufferOffsetAlignment;

    const CommandLineOptions& options = context.options();
    const gl::CollectionFormatChoice arrays =
        gl::chooseCollectionArrayFormat(caps.s3tcSrgb(), options.noTextureCompression);
    state->collectionArrayFormat = arrays.format;
    const glm::ivec2 front = gl::layerSize(gl::CollectionClass::Front, arrays.format);
    const glm::ivec2 spine = gl::layerSize(gl::CollectionClass::Spine, arrays.format);
    spdlog::info("Collection texture arrays: {} ({}x{} fronts, {}x{} spines): {}", gl::toString(arrays.format), front.x,
                 front.y, spine.x, spine.y, arrays.reason);
    if (options.screenshot && options.frameLimit && options.xr.enabled) {
        spdlog::info("Screenshot of the last frame (frame {}, counted from 0) will be written to {}: both eyes side by "
                     "side in VR, else {}x{}",
                     *options.frameLimit - 1, core::pathToUtf8(*options.screenshot), options.screenshotWidth,
                     options.screenshotHeight);
    } else if (options.screenshot && options.frameLimit) {
        spdlog::info("Screenshot of the last frame (frame {}, counted from 0) at {}x{} will be written to {}",
                     *options.frameLimit - 1, options.screenshotWidth, options.screenshotHeight,
                     core::pathToUtf8(*options.screenshot));
    }
    return std::unique_ptr<SceneGraphics>(new SceneGraphics(std::move(state)));
}

const assets::MaterialLibrary& SceneGraphics::materialLibrary() const noexcept {
    return m_state->library;
}

SceneResources SceneGraphics::resources() noexcept {
    return SceneResources{m_state->meshes, m_state->materials, *m_state->textures,
                          m_state->uniformBufferOffsetAlignment, &m_state->looks};
}

ScreenLooks& SceneGraphics::screenLooks() noexcept {
    return m_state->looks;
}

void SceneGraphics::clearContent() noexcept {
    // Materials first: they name the textures.
    m_state->materials.clear();
    m_state->meshes.clear();
    m_state->textures->clearContent();
}

gl::ShaderLibrary& SceneGraphics::shaders() noexcept {
    return *m_state->shaders;
}

gl::MeshRegistry& SceneGraphics::meshes() noexcept {
    return m_state->meshes;
}

gl::MaterialRegistry& SceneGraphics::materials() noexcept {
    return m_state->materials;
}

gl::TextureCache& SceneGraphics::textures() noexcept {
    return *m_state->textures;
}

const gl::TextureDefaults& SceneGraphics::textureDefaults() const noexcept {
    return *m_state->textureDefaults;
}

gl::CollectionArrayFormat SceneGraphics::collectionArrayFormat() const noexcept {
    return m_state->collectionArrayFormat;
}

const gl::FrameStats& SceneGraphics::renderStats() const noexcept {
    return m_state->renderer->stats();
}

gl::PostShaders SceneGraphics::postShaders() const noexcept {
    return m_state->postShaders();
}

const gl::ShaderProgram& SceneGraphics::unlitProgram() const noexcept {
    return m_state->shaders->get(m_state->programs.unlit);
}

Status SceneGraphics::setDebugLines(const scene::DebugLines& lines) {
    State& state = *m_state;
    state.debugLinesShown = false;
    if (lines.empty()) {
        return ok();
    }
    if (!state.debugShader) {
        auto debug =
            state.shaders->load(gl::ShaderDesc{"debug_lines", "debug/debug_lines.vert", "debug/debug_lines.frag", {}});
        if (!debug) {
            return propagate(debug.error());
        }
        state.debugShader = *debug;
    }
    if (!state.debugDraw) {
        auto draw = gl::DebugDraw::create("DebugLines");
        if (!draw) {
            return propagate(draw.error());
        }
        state.debugDraw = std::move(*draw);
    }
    if (auto uploaded = state.debugDraw->setLines(lines); !uploaded) {
        return propagate(uploaded.error());
    }
    state.debugLinesShown = true;
    return ok();
}

Status SceneGraphics::setSurfaceView(gl::SurfaceView view) {
    State& state = *m_state;
    if (view != gl::SurfaceView::Lit) {
        std::optional<gl::ShaderHandle>& handle = state.inspectionShaders[inspectionIndex(view)];
        if (!handle) {
            auto loaded = state.shaders->load(gl::ShaderDesc{std::string(gl::inspectionProgramName(view)),
                                                             "pbr/pbr.vert",
                                                             "debug/inspect.frag",
                                                             {std::string(gl::inspectionDefine(view))}});
            if (!loaded) {
                return propagate(loaded.error());
            }
            handle = *loaded;
        }
    }
    if (view != state.surfaceView) {
        spdlog::info("Surfaces show: {}", gl::toString(view));
    }
    state.surfaceView = view;
    return ok();
}

gl::SurfaceView SceneGraphics::surfaceView() const noexcept {
    return m_state->surfaceView;
}

void SceneGraphics::setOverlay(std::function<void(gl::GLDevice&)> window,
                               std::function<void(gl::GLDevice&, const gl::Framebuffer&)> screenshot) {
    m_state->windowOverlay = std::move(window);
    m_state->screenshotOverlay = std::move(screenshot);
}

void SceneGraphics::renderFrame(AppContext& context, const gl::RenderView& view, bool screenshotRequested,
                                const XrEyeTargets* eyes) {
    State& state = *m_state;
    state.viewRendered = false;
    // The frame's views (a screenshot, the window, the stereo preview's or the headset's eyes) share one shadow map.
    state.renderer->beginFrame();
    // A VR frame's eyes first: the screenshots and the window show them.
    bool eyesRendered = false;
    if (eyes != nullptr) {
        eyesRendered = state.renderEyes(context.device(), view, *eyes);
        if (eyesRendered) {
            context.setXrViewsRendered();
            // A session's frames before its first views (a real runtime's first ones have shouldRender false) went the
            // desktop's way below and made the window's targets: the mirror is the left eye, so they go.
            if (state.windowTargets.valid()) {
                state.windowTargets = gl::ViewTargets{};
            }
        }
    }
    // Screenshots are rendered before the window view, so the frame ends with the window blit and the state it
    // documents (default framebuffer with the window's viewport, GL_FRAMEBUFFER_SRGB off) for whatever draws next.
    if (screenshotRequested) {
        const std::filesystem::path path =
            context.paths().screenshotsDir() / screenshotFileName(currentLocalTimestamp());
        if (auto written = state.captureScreenshot(context, view, path, eyesRendered); !written) {
            spdlog::error("Screenshot failed: {}", written.error().toString());
        }
    }
    const CommandLineOptions& options = context.options();
    const bool lastFrame = options.frameLimit && context.frameIndex() + 1 == *options.frameLimit;
    if (options.screenshot && lastFrame) {
        if (auto written = state.captureScreenshot(context, view, *options.screenshot, eyesRendered); written) {
            state.lastFrameCaptured = true;
            state.lastFrameTargets = state.screenshotTargetsUsed;
        } else {
            Error error = written.error();
            error.message = "--screenshot failed: " + error.message;
            context.failRun(std::move(error));
        }
    }

    const glm::ivec2 windowSize = context.framebufferSize();
    if (windowSize.x > 0 && windowSize.y > 0) {
        if (eyes != nullptr) {
            // The mirror: the left eye, already rendered.
            if (eyesRendered) {
                state.eyeTargets[0].output().blitToDefaultFilled(context.device().state(), windowSize);
            } else {
                gl::Framebuffer::clearDefault(context.device().state(), windowSize);
            }
        } else if (options.stereoPreview) {
            if (prepareStereoImage(state.windowStereo, windowSize, "WindowStereo", state.failedWindowStereoSize) &&
                state.renderStereo(context.device(), view, *state.windowStereo)) {
                state.windowStereo->blitToDefault(context.device().state(), windowSize);
            } else {
                gl::Framebuffer::clearDefault(context.device().state(), windowSize);
            }
        } else {
            state.releaseEyes(context.vr());
            if (prepareTargets(state.windowTargets, windowSize, "WindowTargets", state.failedWindowSize)) {
                state.render(context.device(), view, state.windowTargets);
                state.windowTargets.output().blitToDefault(context.device().state(), windowSize);
            } else {
                // No scene this frame (logged): a black window, with the overlay and its log still over it.
                gl::Framebuffer::clearDefault(context.device().state(), windowSize);
            }
        }
        // Last, over the scene in the window.
        if (state.windowOverlay) {
            state.windowOverlay(context.device());
        }
    }

    // The statistics of the last view rendered this frame: the window's, or the screenshot's when the window is empty.
    // A frame that rendered no view (a minimized window) counts its time only: the renderer's statistics still
    // describe an earlier view.
    if (state.viewRendered) {
        state.statsLog.record(context.clock().deltaSeconds(), state.renderer->stats());
        // The run report's GPU times per pass, over the run and its phases (I3 step 17b).
        context.frameTiming().recordGpuPasses(state.renderer->stats());
    } else {
        state.statsLog.recordWithoutView(context.clock().deltaSeconds());
    }
    if constexpr (kDeveloperBuild) {
        if (const std::optional<std::string> summary = state.statsLog.takeIntervalSummary()) {
            spdlog::info("{}", *summary);
        }
    }
}

void SceneGraphics::reloadShaders() {
    spdlog::info("Reloading shaders");
    m_state->shaders->reloadAll();
}

void SceneGraphics::pollShaderReload() {
    if constexpr (kShaderHotReload) {
        m_state->shaders->pollHotReload();
    }
}

void SceneGraphics::finishRun(AppContext& context) {
    const CommandLineOptions& options = context.options();
    const State& state = *m_state;
    RunReport& report = context.report();
    if (state.anyViewRendered) {
        report.lastView = state.renderer->stats();
    }
    report.instanceCapacity = state.renderer->instanceCapacity();
    if (options.screenshot && state.lastFrameCaptured) {
        report.screenshot = ScreenshotRecord{*options.screenshot, state.lastScreenshotSize.x,
                                             state.lastScreenshotSize.y, std::string(state.lastFrameTargets)};
    }
    // The GPU memory accounting (gl::gpuMemoryLedger) records every texture of the cache and the defaults, and nothing
    // else under their kinds: the run report's procedural surfaces and scene textures are what they hold.
    const gl::GpuMemoryLedger& ledger = gl::gpuMemoryLedger();
    const std::uint64_t held = state.heldTextureBytes();
    const std::uint64_t recorded = ledger.usage(gl::GpuMemoryKind::ProceduralSurfaces).bytes +
                                   ledger.usage(gl::GpuMemoryKind::SceneTextures).bytes;
    if (held != recorded) {
        spdlog::error("GPU memory accounting: the texture cache and the defaults hold {} bytes, the ledger records {} "
                      "for procedural surfaces and scene textures",
                      held, recorded);
    }
    if (!options.frameLimit) {
        return;
    }
    if (const std::optional<std::string> summary = state.statsLog.runSummary("Frame stats of the run")) {
        spdlog::info("{}", *summary);
    }
    // An accounting that misses textures, or counts others, makes the run report's gpuMemory wrong: unattended runs,
    // which read it against the budgets, must not pass.
    if (held != recorded) {
        context.failRun(Error{ErrorCode::ValidationFailed,
                              fmt::format("the GPU memory accounting records {} bytes of textures, the texture cache "
                                          "and the defaults hold {}",
                                          recorded, held),
                              "gpuMemory"});
    }
    // An unattended run must not pass when it could never draw its scene completely (its frames and screenshot miss
    // surfaces, the bloom or the tonemap).
    if (const std::string_view missing = state.missingProgram(); !missing.empty()) {
        context.failRun(Error{ErrorCode::GraphicsError,
                              fmt::format("the scene was never drawn completely: shader '{}' did not build (see the "
                                          "errors above)",
                                          missing),
                              core::pathToUtf8(state.shaders->root())});
    }
    // Nor when it ended (Esc, window closed, quit request) before the last frame, which --screenshot captures: a file
    // left at that path by an earlier run must not pass for this run's result.
    if (options.screenshot && !state.lastFrameCaptured && !context.failure()) {
        context.failRun(Error{ErrorCode::ValidationFailed,
                              fmt::format("--screenshot was not written: the run ended after {} of {} frame(s)",
                                          context.frameIndex(), options.frameLimit.value_or(0)),
                              core::pathToUtf8(*options.screenshot)});
    }
}

} // namespace rv::app
