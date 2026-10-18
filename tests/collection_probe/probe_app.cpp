#include "probe_app.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/mesh_builder.hpp>
#include <rv/render/collection_arrays.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/static_instance_group.hpp>
#include <rv/render/texture_array.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/instance_clusters.hpp>

namespace rv::probe {
namespace {

/// Layer pictures kept queued in the arrays, so that the per-frame budget, not the probe, limits the uploads.
constexpr std::size_t kQueuedPictures = 256;
constexpr std::string_view kKeyPrefix = "item/";

std::string itemKey(std::size_t item) {
    return fmt::format("{}{}", kKeyPrefix, item);
}

std::optional<std::size_t> itemOfKey(std::string_view key) {
    if (!key.starts_with(kKeyPrefix)) {
        return std::nullopt;
    }
    key.remove_prefix(kKeyPrefix.size());
    std::size_t item = 0;
    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), item);
    if (error != std::errc() || end != key.data() + key.size()) {
        return std::nullopt;
    }
    return item;
}

/// The scene around the items: a dark backdrop behind the wall, a uniform white ambient (sky and ground alike) and no
/// light, ambient occlusion, bloom or vignette, so that every item is lit alike and its colors depend on its pictures
/// alone. The groups scenario adds a spot key light from the upper left, whose shadows the shells cast on the backdrop
/// (and on each other), and the ambient occlusion, with a dimmer ambient: everything a static group must draw like
/// render items.
scene::SceneDescription probeScene(const Grid& grid, Scenario scenario) {
    scene::SceneDescription description;
    description.name = "collection_probe";
    description.camera = cameraFor(grid);
    scene::EnvironmentSetup& environment = description.environment;
    environment.clearColor = glm::vec3(0.005f);
    environment.ambientSky = glm::vec3(1.0f);
    environment.ambientGround = glm::vec3(1.0f);
    environment.ambientIntensity = 1.0f;
    environment.exposure = 1.0f;
    environment.bloomStrength = 0.0f;
    environment.vignette = 0.0f;
    environment.ssao.enabled = false;
    if (scenario == Scenario::Groups) {
        environment.ambientIntensity = 0.35f;
        environment.ssao.enabled = true;
        scene::Light key;
        key.name = "probe_key";
        key.type = scene::LightType::Spot;
        key.position = glm::vec3(-3.0f, 4.0f, 10.0f);
        key.direction = glm::normalize(glm::vec3(0.0f) - key.position);
        key.intensity = 110.0f;
        key.range = 40.0f;
        key.innerConeDeg = 30.0f;
        key.outerConeDeg = 38.0f;
        key.castsShadows = true;
        description.lights.push_back(key);
    }

    scene::SceneObject backdrop;
    backdrop.id = "backdrop";
    backdrop.source = scene::SceneObject::Source::Primitive;
    backdrop.kind = "box";
    const float width = static_cast<float>(grid.columns) * kCellSize.x * 1.3f;
    const float height = static_cast<float>(grid.rows) * kCellSize.y * 1.3f;
    backdrop.params = nlohmann::json{{"size", {width, height, 0.02f}}};
    backdrop.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -0.05f));
    backdrop.material = "MAT_Plastic_Black";
    backdrop.castsShadows = false;
    backdrop.collides = false;
    description.objects.push_back(std::move(backdrop));
    return description;
}

assets::MaterialDesc layerMaterialDesc() {
    assets::MaterialDesc desc;
    desc.name = "MAT_Probe_Layer";
    desc.baseColorFactor = glm::vec4(1.0f);
    desc.metallicFactor = 0.0f;
    desc.roughnessFactor = 1.0f;
    return desc;
}

} // namespace

struct ProbeApp::State {
    /// Declared first: destroyed last, after the arrays and the content that refer to its registries.
    std::unique_ptr<app::SceneGraphics> graphics;
    std::optional<app::SceneInstance> scene;
    gl::CollectionArrays arrays;
    /// The groups scenario's items on the static path (empty otherwise), and the view's list of groups.
    std::optional<gl::StaticInstanceGroup> group;
    std::array<const gl::StaticInstanceGroup*, 1> groupList{};
    ProbeOptions options;
    ScenarioCounts counts;
    Grid grid;
    scene::Camera camera;
    scene::MeshHandle shellMesh;
    scene::MeshHandle frontMesh;
    scene::MeshHandle spineMesh;
    scene::MaterialHandle shellMaterial;
    /// The layered material of each array, per class.
    std::array<std::vector<scene::MaterialHandle>, gl::kCollectionClassCount> layerMaterials;
    /// The scene's items, then three per placed item: its shell, front and spine.
    std::vector<scene::RenderItem> items;
    std::size_t sceneItems = 0;
    std::size_t placed = 0;
    /// Pictures still to submit, in order: the front and the spine of each item.
    std::deque<std::pair<gl::CollectionClass, std::size_t>> toSubmit;
    bool grown = false;
    bool growthWaitLogged = false;
    /// Every placed item's layers held their pictures when the last frame was drawn.
    bool everyLayerResident = false;
    std::size_t uploadedLayers = 0;
    /// The groups scenario: the highlight is set, and the outline's GPU times of the frames that drew it.
    bool highlighted = false;
    std::vector<double> outlineGpuMilliseconds;

    [[nodiscard]] bool groups() const noexcept { return options.scenario == Scenario::Groups; }
    [[nodiscard]] bool staticPath() const noexcept { return groups() && options.path == DrawPath::Static; }

    /// A layered material for every array that has none yet, and the material blocks uploaded again.
    Status ensureMaterials(int uniformBufferOffsetAlignment) {
        gl::MaterialRegistry& materials = graphics->materials();
        bool added = false;
        for (const gl::CollectionClass cls : gl::kCollectionClasses) {
            std::vector<scene::MaterialHandle>& list = layerMaterials[gl::toIndex(cls)];
            while (list.size() < arrays.arrayCount(cls)) {
                const std::string key = fmt::format("probe/{}[{}]", gl::toString(cls), list.size());
                list.push_back(materials.addLayered(
                    key, key, layerMaterialDesc(), gl::MaterialTextures{},
                    gl::LayeredBaseColor{&arrays.array(cls, list.size()), cls == gl::CollectionClass::Spine}));
                added = true;
            }
        }
        return added ? materials.upload(uniformBufferOffsetAlignment, "ProbeMaterials") : ok();
    }

    /// Places items up to `count`: their layers acquired, their pictures queued for submission, their render items
    /// added (on the groups scenario's static path, their static instances, made into the group: that scenario places
    /// its items once). The other scenarios show layer 0 until a picture is uploaded and then write its layer
    /// (applyUploads()); the groups scenario names each item's layers in its records from the start (a fresh array's
    /// layers hold the pending picture until theirs arrives), so that the group is never written while the pictures
    /// load.
    Status place(std::size_t count) {
        std::vector<scene::StaticInstance> staticInstances;
        for (std::size_t item = placed; item < count; ++item) {
            std::array<gl::LayerSlot, gl::kCollectionClassCount> slots{};
            for (const gl::CollectionClass cls : gl::kCollectionClasses) {
                auto slot = arrays.acquire(cls, itemKey(item));
                if (!slot) {
                    return propagate(slot.error());
                }
                slots[gl::toIndex(cls)] = *slot;
                toSubmit.emplace_back(cls, item);
            }
            scene::InstanceAttributes attributes;
            if (groups()) {
                attributes.frontLayer = slots[0].layer;
                attributes.spineLayer = slots[1].layer;
                attributes.pickId = static_cast<std::uint32_t>(item + 1);
            }
            const bool hidden = groups() && options.hide && item == hiddenItem(grid);
            const std::array<scene::RenderItem, 3> parts{
                scene::RenderItem{shellMesh, shellMaterial, scene::ShaderKind::Pbr,
                                  glm::translate(glm::mat4(1.0f), cellCenter(grid, item)), true, attributes},
                scene::RenderItem{frontMesh, layerMaterials[gl::toIndex(gl::CollectionClass::Front)][slots[0].array],
                                  scene::ShaderKind::Pbr, glm::translate(glm::mat4(1.0f), frontCenter(grid, item)),
                                  false, attributes},
                scene::RenderItem{spineMesh, layerMaterials[gl::toIndex(gl::CollectionClass::Spine)][slots[1].array],
                                  scene::ShaderKind::Pbr, glm::translate(glm::mat4(1.0f), spineCenter(grid, item)),
                                  false, attributes},
            };
            for (const scene::RenderItem& part : parts) {
                if (staticPath()) {
                    scene::StaticInstance instance{part.mesh,        part.material, part.shader,
                                                   part.model,       part.castsShadow, part.attributes,
                                                   clusterOf(grid, item)};
                    if (hidden) {
                        instance.attributes.flags |= scene::instance_flags::kHidden;
                    }
                    staticInstances.push_back(instance);
                } else if (!hidden) {
                    items.push_back(part);
                }
            }
        }
        placed = std::max(placed, count);
        if (staticPath()) {
            auto created = gl::StaticInstanceGroup::create(staticInstances, clusterCount(grid), graphics->meshes(),
                                                           graphics->materials(), "ProbeGroup");
            if (!created) {
                return propagate(created.error());
            }
            group = std::move(*created);
            groupList[0] = &*group;
            spdlog::info("Static instance group: {} instance(s) in {} batch(es) and {} cluster(s), {} bytes uploaded",
                         group->layout().instances().size(), group->layout().batches().size(),
                         group->layout().occupiedClusters(), group->uploadedBytes());
        }
        return ok();
    }

    /// The highlight of highlightItem() (its three parts), from kHighlightFrame on.
    Status highlight() {
        const std::size_t item = highlightItem(grid);
        for (std::size_t part = 0; part < 3; ++part) {
            if (staticPath()) {
                scene::InstanceAttributes attributes = group->layout().instances()[group->layout().positionOf(
                                                                                       3 * item + part)]
                                                           .attributes;
                attributes.flags |= scene::instance_flags::kHighlighted;
                if (Status set = group->setAttributes(3 * item + part, attributes); !set) {
                    return set;
                }
            } else {
                // The render items of the items before the hidden one are three per item; it has none.
                const std::size_t hiddenBefore = options.hide && hiddenItem(grid) < item ? 3 : 0;
                items[sceneItems + 3 * item + part - hiddenBefore].attributes.flags |=
                    scene::instance_flags::kHighlighted;
            }
        }
        highlighted = true;
        return ok();
    }

    /// Keeps kQueuedPictures pictures queued.
    Status submitPictures() {
        while (arrays.pendingUploads() < kQueuedPictures && !toSubmit.empty()) {
            const auto [cls, item] = toSubmit.front();
            toSubmit.pop_front();
            const gl::Rgb8 color = cls == gl::CollectionClass::Front ? frontColor(item) : spineColor(item);
            if (auto submitted = arrays.submit(cls, itemKey(item), gl::makeSolidLayer(cls, arrays.format(), color));
                !submitted) {
                return submitted;
            }
        }
        return ok();
    }

    /// Every placed item's two layers hold their pictures.
    [[nodiscard]] bool allResident() const {
        return toSubmit.empty() && arrays.pendingUploads() == 0 &&
               arrays.residentLayers(gl::CollectionClass::Front) == placed &&
               arrays.residentLayers(gl::CollectionClass::Spine) == placed;
    }

    /// The instance records of the items whose pictures went up now name their layers.
    void applyUploads(const gl::LayerUploadResult& result) {
        uploadedLayers += result.completed.size();
        if (groups()) {
            return; // the records name their layers from the start
        }
        for (const gl::LayerUpload& upload : result.completed) {
            const std::optional<std::size_t> item = itemOfKey(upload.key);
            if (!item || *item >= placed) {
                continue;
            }
            for (std::size_t part = 0; part < 3; ++part) {
                scene::InstanceAttributes& attributes = items[sceneItems + 3 * *item + part].attributes;
                (upload.cls == gl::CollectionClass::Front ? attributes.frontLayer : attributes.spineLayer) =
                    upload.layer;
            }
        }
    }

    /// Places the camera where its mode stands at `frame`.
    void placeCamera(std::uint64_t frame) {
        const scene::CameraSetup setup = cameraAt(grid, options.camera, frame);
        camera.setLens(setup.lens);
        camera.setPose(setup.position, core::degreesToRadians(setup.yawDeg), core::degreesToRadians(setup.pitchDeg));
    }
};

ProbeApp::ProbeApp(ProbeOptions options) : m_options(options) {}

ProbeApp::~ProbeApp() = default;

Status ProbeApp::onInit(app::AppContext& context) {
    auto state = std::make_unique<State>();
    state->options = m_options;
    state->counts = countsOf(m_options.scenario);
    state->grid = gridFor(state->counts.final);
    auto graphics = app::SceneGraphics::create(context);
    if (!graphics) {
        return propagate(graphics.error());
    }
    state->graphics = std::move(*graphics);
    app::SceneGraphics& graphicsRef = *state->graphics;
    const int alignment = context.device().caps().uniformBufferOffsetAlignment;

    const scene::SceneDescription description = probeScene(state->grid, m_options.scenario);
    auto sceneInstance =
        app::SceneInstance::create(description, graphicsRef.materialLibrary(), {}, graphicsRef.resources());
    if (!sceneInstance) {
        return propagate(sceneInstance.error());
    }
    state->scene.emplace(std::move(*sceneInstance));
    const std::span<const scene::RenderItem> sceneItems = state->scene->renderItems();
    state->items.assign(sceneItems.begin(), sceneItems.end());
    state->sceneItems = state->items.size();

    state->placeCamera(0);

    gl::CollectionArrays::Desc desc;
    desc.format = graphicsRef.collectionArrayFormat();
    desc.pendingColors = {kPendingFront, kPendingSpine};
    desc.debugName = "ProbeArrays";
    auto arrays = gl::CollectionArrays::create(desc, {state->counts.initial, state->counts.initial});
    if (!arrays) {
        return propagate(arrays.error());
    }
    state->arrays = std::move(*arrays);

    gl::MeshRegistry& meshes = graphicsRef.meshes();
    geometry::MeshBuilder shellBuilder;
    shellBuilder.box(kShellSize);
    geometry::MeshBuilder frontBuilder;
    frontBuilder.quad(kFrontSize);
    geometry::MeshBuilder spineBuilder;
    spineBuilder.quad(kSpineSize);
    const std::pair<scene::MeshHandle State::*, geometry::MeshData> parts[] = {
        {&State::shellMesh, std::move(shellBuilder).build("probe_shell")},
        {&State::frontMesh, std::move(frontBuilder).build("probe_front")},
        {&State::spineMesh, std::move(spineBuilder).build("probe_spine")},
    };
    for (const auto& [member, mesh] : parts) {
        auto handle = meshes.add(fmt::format("probe/{}", mesh.name), mesh);
        if (!handle) {
            return propagate(handle.error());
        }
        (*state).*member = *handle;
    }
    const assets::MaterialDesc& shell = graphicsRef.materialLibrary().getOrFallback("MAT_Plastic_Gray");
    state->shellMaterial = graphicsRef.materials().add("probe/shell", "probe/shell", shell,
                                                       gl::resolveMaterialTextures(shell, graphicsRef.textures()));
    if (auto materials = state->ensureMaterials(alignment); !materials) {
        return propagate(materials.error());
    }
    if (auto placed = state->place(state->counts.initial); !placed) {
        return propagate(placed.error());
    }
    if (m_options.view != gl::SurfaceView::Lit) {
        if (auto shown = graphicsRef.setSurfaceView(m_options.view); !shown) {
            return propagate(shown.error());
        }
    }
    spdlog::info("Collection probe '{}': {} item(s) on a {}x{} wall, {} at the end; arrays {}, {} view, {} camera",
                 toString(m_options.scenario), state->counts.initial, state->grid.columns, state->grid.rows,
                 state->counts.final, gl::toString(desc.format), gl::toString(m_options.view),
                 toString(m_options.camera));
    if (state->groups()) {
        spdlog::info("Groups: {} path, {} cluster(s), highlight {}, hidden {}", toString(m_options.path),
                     clusterCount(state->grid),
                     m_options.highlight ? fmt::format("item {} from frame {}", highlightItem(state->grid),
                                                       kHighlightFrame)
                                         : std::string("none"),
                     m_options.hide ? fmt::format("item {}", hiddenItem(state->grid)) : std::string("none"));
    }
    m_state = std::move(state);
    return ok();
}

void ProbeApp::onUpdate(app::AppContext& /*context*/, double /*deltaSeconds*/) {}

void ProbeApp::onRender(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& state = *m_state;
    const std::uint64_t frame = context.frameIndex();
    if (!state.grown && state.counts.final > state.counts.initial && frame >= kGrowthFrame) {
        if (!state.allResident()) {
            if (!state.growthWaitLogged) {
                spdlog::info("Frame {}: the growth waits for the first items' layers", frame);
                state.growthWaitLogged = true;
            }
        } else {
            Status grown = ok();
            for (const gl::CollectionClass cls : gl::kCollectionClasses) {
                if (grown) {
                    grown = state.arrays.reserve(cls, state.counts.final);
                }
            }
            if (grown) {
                grown = state.ensureMaterials(context.device().caps().uniformBufferOffsetAlignment);
            }
            if (grown) {
                grown = state.place(state.counts.final);
            }
            if (!grown) {
                context.failRun(grown.error());
                return;
            }
            state.grown = true;
            spdlog::info("Frame {}: {} item(s) placed, the arrays grown ({:.1f} MiB)", frame, state.placed,
                         static_cast<double>(state.arrays.estimatedBytes()) / (1024.0 * 1024.0));
        }
    }
    if (auto submitted = state.submitPictures(); !submitted) {
        context.failRun(submitted.error());
        return;
    }
    const gl::LayerUploadResult uploads = state.arrays.uploadPending();
    if (uploads.elapsed > std::chrono::microseconds(1500)) {
        // The budget's overruns, with the frame they happened in (preemption, a driver stall): for the measurements.
        spdlog::info("Frame {}: {} layer(s) uploaded in {:.2f} ms, over the budget", frame, uploads.completed.size(),
                     std::chrono::duration<double, std::milli>(uploads.elapsed).count());
    }
    state.applyUploads(uploads);
    const bool resident = state.allResident();
    if (resident && !state.everyLayerResident) {
        spdlog::info("Frame {}: every layer of the {} item(s) is resident ({} uploaded)", frame, state.placed,
                     state.uploadedLayers);
    }
    state.everyLayerResident = resident;
    if (state.groups() && state.options.highlight && !state.highlighted && frame >= kHighlightFrame) {
        if (Status set = state.highlight(); !set) {
            context.failRun(set.error());
            return;
        }
        spdlog::info("Frame {}: item {} highlighted ({} instance bytes written since the group was created)", frame,
                     highlightItem(state.grid), state.group ? state.group->writtenBytes() : 0);
    }
    state.placeCamera(frame);
    gl::RenderView view = app::makeRenderView(*state.scene, state.camera);
    view.items = state.items;
    if (state.group) {
        view.staticGroups = state.groupList;
    }
    state.graphics->renderFrame(context, view, false);
    if (const std::optional<double> outline = state.graphics->renderStats().outlineGpuMilliseconds) {
        state.outlineGpuMilliseconds.push_back(*outline);
    }
}

void ProbeApp::onShutdown(app::AppContext& context) {
    if (m_state) {
        State& state = *m_state;
        spdlog::info("Collection probe: {} item(s), fronts {} resident, spines {} resident, {} upload(s) queued",
                     state.placed, state.arrays.residentLayers(gl::CollectionClass::Front),
                     state.arrays.residentLayers(gl::CollectionClass::Spine), state.arrays.pendingUploads());
        if (state.group) {
            spdlog::info("Static instance group: {} bytes uploaded at its creation, {} bytes written since",
                         state.group->uploadedBytes(), state.group->writtenBytes());
        }
        if (!state.outlineGpuMilliseconds.empty()) {
            std::vector<double> times = state.outlineGpuMilliseconds;
            std::sort(times.begin(), times.end());
            spdlog::info("Outline GPU time over {} frame(s): median {:.4f} ms, max {:.4f} ms", times.size(),
                         times[times.size() / 2], times.back());
        }
        if (context.options().frameLimit && !state.everyLayerResident) {
            context.failRun(Error{ErrorCode::ValidationFailed,
                                  "a layer was still pending at the last frame: the screenshot would not show every "
                                  "item's pictures",
                                  "collection probe"});
        }
        state.graphics->finishRun(context);
    }
    m_state.reset();
}

} // namespace rv::probe
