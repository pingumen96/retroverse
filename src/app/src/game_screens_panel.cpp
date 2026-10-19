#include "game_screens_panel.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/render/texture.hpp>
#include <rv/ui/game_info_screen.hpp>
#include <rv/ui/media_viewer_model.hpp>
#include <rv/ui/media_viewer_screen.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::app {
namespace {

/// Pictures uploaded in one frame, at most.
constexpr std::size_t kUploadsPerFrame = 2;

/// The viewer's pictures, read and decoded on a thread of their own (the main thread never reads the user's files).
class PictureLoader {
public:
    struct Job {
        std::uint64_t serial = 0;
        std::size_t index = 0;
        std::string relativePath;
    };
    struct Result {
        std::uint64_t serial = 0;
        std::size_t index = 0;
        std::optional<assets::ImageData> picture;
        std::optional<Error> error;
    };

    PictureLoader(std::optional<std::filesystem::path> libraryFile, std::function<void()> wake)
        : m_libraryFile(std::move(libraryFile)), m_wake(std::move(wake)), m_thread([this] { run(); }) {}

    ~PictureLoader() {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_jobs.clear();
        }
        m_changed.notify_all();
        m_thread.join();
    }

    PictureLoader(const PictureLoader&) = delete;
    PictureLoader& operator=(const PictureLoader&) = delete;

    /// Replaces the jobs waiting with `jobs` of a new opening (`serial`); the media root is resolved again for it.
    void request(std::uint64_t serial, std::vector<Job> jobs) {
        {
            const std::lock_guard lock(m_mutex);
            m_serial = serial;
            m_jobs.assign(std::make_move_iterator(jobs.begin()), std::make_move_iterator(jobs.end()));
            m_results.clear();
        }
        m_changed.notify_all();
    }

    /// Drops what waits (the viewer closed): a picture in flight goes nowhere.
    void cancel(std::uint64_t serial) {
        const std::lock_guard lock(m_mutex);
        m_serial = serial;
        m_jobs.clear();
        m_results.clear();
    }

    [[nodiscard]] std::vector<Result> take() {
        const std::lock_guard lock(m_mutex);
        std::vector<Result> taken(std::make_move_iterator(m_results.begin()), std::make_move_iterator(m_results.end()));
        m_results.clear();
        return taken;
    }

    [[nodiscard]] bool busy() const {
        const std::lock_guard lock(m_mutex);
        return !m_jobs.empty() || m_working;
    }

private:
    void run() {
        std::optional<std::uint64_t> rootSerial;
        std::optional<Expected<std::filesystem::path>> root;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
                if (m_stopping) {
                    return;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                m_working = true;
            }
            if (rootSerial != job.serial) {
                // The media root of each opening as ES-DE's settings give it now (an unplugged drive is seen).
                rootSerial = job.serial;
                root = m_libraryFile
                           ? viewerMediaRoot(*m_libraryFile)
                           : Expected<std::filesystem::path>(makeError(ErrorCode::NotFound, "no library.json"));
            }
            Result result;
            result.serial = job.serial;
            result.index = job.index;
            if (!*root) {
                result.error = root->error();
            } else if (auto picture = viewerPicture(**root, job.relativePath); !picture) {
                result.error = picture.error();
            } else {
                result.picture = std::move(*picture);
            }
            bool posted = false;
            {
                const std::lock_guard lock(m_mutex);
                m_working = false;
                if (job.serial == m_serial && !m_stopping) {
                    m_results.push_back(std::move(result));
                    posted = true;
                }
            }
            if (posted && m_wake) {
                m_wake();
            }
        }
    }

    const std::optional<std::filesystem::path> m_libraryFile;
    const std::function<void()> m_wake;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Job> m_jobs;
    std::deque<Result> m_results;
    std::uint64_t m_serial = 0;
    bool m_working = false;
    bool m_stopping = false;
    // Last: the thread starts once the rest is constructed.
    std::thread m_thread;
};

} // namespace

struct GameScreensPanel::Impl {
    Impl(ui::UiView& view, ui::UiSystem& system, std::optional<std::filesystem::path> libraryFile,
         std::function<void()> wake)
        : view(view), system(system), loader(std::move(libraryFile), std::move(wake)) {}

    ~Impl() { dropTextures(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// The page shown's texture registered for the document (none: the key emptied).
    void registerPicture() {
        unsigned id = 0;
        glm::ivec2 size{0, 0};
        const std::size_t index = model.index();
        if (viewer && viewer->visible() && index < textures.size() && textures[index] && textures[index]->valid()) {
            id = textures[index]->id();
            size = textures[index]->size();
        }
        if (id != registered) {
            system.setTexture(ui::media_viewer::kPictureTexture, id, size);
            registered = id;
        }
    }

    void dropTextures() {
        if (registered != 0) {
            system.setTexture(ui::media_viewer::kPictureTexture, 0, {});
            registered = 0;
        }
        textures.clear();
        waiting.clear();
    }

    ui::UiView& view;
    ui::UiSystem& system;
    std::unique_ptr<ui::MediaViewerScreen> viewer;
    std::unique_ptr<ui::GameInfoScreen> info;
    ui::MediaViewerModel model;
    std::optional<ui::GameInfoContent> infoContent;
    /// The viewer's opening (its loader jobs carry it) and the pictures waiting for their upload.
    std::uint64_t serial = 0;
    std::vector<std::optional<gl::Texture2D>> textures;
    std::deque<PictureLoader::Result> waiting;
    unsigned registered = 0;
    /// The page whose "page" event was recorded last.
    std::optional<std::size_t> recordedPage;
    PictureLoader loader;
};

GameScreensPanel::GameScreensPanel(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

GameScreensPanel::~GameScreensPanel() = default;

Expected<std::unique_ptr<GameScreensPanel>> GameScreensPanel::create(ui::UiView& view, ui::UiSystem& system,
                                                                     std::optional<std::filesystem::path> libraryFile,
                                                                     std::function<void()> wake) {
    auto viewer = ui::MediaViewerScreen::create(view);
    if (!viewer) {
        return propagate(viewer.error());
    }
    auto info = ui::GameInfoScreen::create(view);
    if (!info) {
        return propagate(info.error());
    }
    auto impl = std::make_unique<Impl>(view, system, std::move(libraryFile), std::move(wake));
    impl->viewer = std::move(*viewer);
    impl->info = std::move(*info);
    auto panel = std::unique_ptr<GameScreensPanel>(new GameScreensPanel(std::move(impl)));
    panel->m_record.bound = true;
    return panel;
}

bool GameScreensPanel::visible() const noexcept {
    return viewerOpen() || infoOpen();
}

bool GameScreensPanel::viewerOpen() const noexcept {
    return m_impl->model.isOpen();
}

bool GameScreensPanel::infoOpen() const noexcept {
    return m_impl->infoContent.has_value();
}

bool GameScreensPanel::loading() const noexcept {
    return viewerOpen() && m_impl->loader.busy();
}

void GameScreensPanel::openMedia(MediaRequest request, std::uint64_t frame) {
    Impl& impl = *m_impl;
    if (infoOpen()) {
        close(frame);
    }
    if (viewerOpen()) {
        close(frame);
    }
    std::vector<std::string> kinds;
    std::string listed;
    for (const MediaPageFile& page : request.pages) {
        kinds.push_back(page.kind);
    }
    impl.model.open(request.gameId, request.title, request.platform, kinds);
    std::vector<PictureLoader::Job> jobs;
    for (std::size_t index = 0; index < impl.model.pages().size(); ++index) {
        const std::string& kind = impl.model.pages()[index].kind;
        const auto file = std::find_if(request.pages.begin(), request.pages.end(),
                                       [&](const MediaPageFile& page) { return page.kind == kind; });
        jobs.push_back(PictureLoader::Job{impl.serial + 1, index, file->relativePath});
        listed += (listed.empty() ? "" : ",") + kind;
    }
    ++impl.serial;
    impl.textures.clear();
    impl.textures.resize(impl.model.pages().size()); // move-only textures: no fill by copy
    impl.waiting.clear();
    impl.recordedPage.reset();
    impl.loader.request(impl.serial, std::move(jobs));
    m_gameId = request.gameId;
    m_from = request.from;
    addEvent(frame, "media", {}, listed);
    spdlog::info("Frame {}: Vedi media of {} (from the {}): {} page(s)", frame, request.gameId, request.from,
                 impl.model.pages().size());
    impl.viewer->show(impl.model);
    showPage(frame);
}

void GameScreensPanel::openInfo(ui::GameInfoContent content, std::string from, std::uint64_t frame) {
    Impl& impl = *m_impl;
    if (viewerOpen()) {
        close(frame);
    }
    m_gameId = content.gameId;
    m_from = std::move(from);
    std::string versions;
    for (const ui::OtherVersion& version : content.versions) {
        versions += (versions.empty() ? "" : "; ") + version.title + " (" + version.region + ")";
    }
    impl.infoContent = std::move(content);
    impl.info->show(*impl.infoContent);
    addEvent(frame, "info", {}, versions);
    spdlog::info("Frame {}: Informazioni of {} (from the {}): {} other version(s)", frame, m_gameId, m_from,
                 impl.infoContent->versionCount);
}

void GameScreensPanel::close(std::uint64_t frame) {
    Impl& impl = *m_impl;
    if (viewerOpen()) {
        impl.model.close();
        impl.viewer->hide();
        ++impl.serial;
        impl.loader.cancel(impl.serial);
        impl.dropTextures();
        addEvent(frame, "closed", {}, "media");
        spdlog::info("Frame {}: the media viewer closed", frame);
    }
    if (infoOpen()) {
        impl.infoContent.reset();
        impl.info->hide();
        addEvent(frame, "closed", {}, "info");
        spdlog::info("Frame {}: Informazioni closed", frame);
    }
}

GameScreensPanel::Switch GameScreensPanel::apply(ui::UiCommand command, std::uint64_t frame) {
    Impl& impl = *m_impl;
    if (viewerOpen()) {
        if (command == ui::UiCommand::Info) {
            return Switch::Info;
        }
        if (command != ui::UiCommand::Back && impl.model.apply(command) == ui::ViewerStep::Moved) {
            showPage(frame);
        }
        return Switch::None;
    }
    if (infoOpen()) {
        const ui::InfoStep step = ui::infoStepOf(command);
        if (step == ui::InfoStep::Media) {
            return Switch::Media;
        }
        if (step != ui::InfoStep::Close && step != ui::InfoStep::None && impl.info->scroll(step)) {
            addEvent(frame, "scroll", {}, std::string(ui::toString(step)));
        }
    }
    return Switch::None;
}

GameScreensPanel::Switch GameScreensPanel::update(std::uint64_t frame) {
    Impl& impl = *m_impl;
    Switch asked = Switch::None;
    if (const std::optional<ui::ViewerClick> click = impl.viewer->takeClicked(); click && viewerOpen()) {
        if (*click == ui::ViewerClick::Close) {
            close(frame);
        } else if (impl.model.apply(*click == ui::ViewerClick::Previous ? ui::UiCommand::Left
                                                                        : ui::UiCommand::Right) ==
                   ui::ViewerStep::Moved) {
            showPage(frame);
        }
    }
    if (const std::optional<ui::InfoStep> click = impl.info->takeClicked(); click && infoOpen()) {
        if (*click == ui::InfoStep::Close) {
            close(frame);
        } else if (*click == ui::InfoStep::Media) {
            asked = Switch::Media;
        }
    }
    if (viewerOpen()) {
        for (PictureLoader::Result& result : impl.loader.take()) {
            if (result.serial == impl.serial) {
                impl.waiting.push_back(std::move(result));
            }
        }
        // The page shown first, then in page order.
        std::stable_sort(impl.waiting.begin(), impl.waiting.end(), [&](const auto& a, const auto& b) {
            return (a.index == impl.model.index()) > (b.index == impl.model.index());
        });
        for (std::size_t uploads = 0; uploads < kUploadsPerFrame && !impl.waiting.empty(); ++uploads) {
            PictureLoader::Result result = std::move(impl.waiting.front());
            impl.waiting.pop_front();
            if (result.index >= impl.model.pages().size()) {
                continue;
            }
            const std::string kind = impl.model.pages()[result.index].kind;
            std::optional<Error> failure = result.error;
            if (result.picture) {
                gl::TextureDesc desc;
                // The sRGB values as encoded: the UI composites sRGB values (GL_FRAMEBUFFER_SRGB off).
                desc.colorSpace = gl::ColorSpace::Linear;
                desc.mipmaps = true;
                desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
                desc.sampling.filter = gl::TextureFilter::Trilinear;
                desc.memoryKind = gl::GpuMemoryKind::HeroTextures;
                auto texture =
                    gl::Texture2D::create(*result.picture, desc, fmt::format("MediaViewer {} {}", m_gameId, kind));
                if (texture) {
                    const glm::ivec2 size = texture->size();
                    impl.textures[result.index] = std::move(*texture);
                    impl.model.setPicture(result.index, ui::PictureState::Shown, size.x, size.y);
                    ++m_record.pictures;
                    addEvent(frame, "picture", kind, fmt::format("shown {}x{}", size.x, size.y));
                    continue;
                }
                failure = texture.error();
            }
            impl.model.setPicture(result.index, ui::PictureState::Failed);
            ++m_record.failures;
            addEvent(frame, "picture", kind, "failed");
            // The error names one of the user's files: at debug level only.
            spdlog::debug("Media viewer: the {} of {} cannot be shown: {}", kind, m_gameId,
                          failure ? failure->toString() : std::string("unknown"));
            spdlog::info("Frame {}: the media viewer's {} of {} cannot be shown (its fallback shows)", frame, kind,
                         m_gameId);
        }
        impl.viewer->show(impl.model);
        impl.registerPicture();
        ++m_record.viewerFrames;
    }
    if (infoOpen()) {
        impl.info->show(*impl.infoContent);
        ++m_record.infoFrames;
    }
    return asked;
}

void GameScreensPanel::afterUpdate() {
    Impl& impl = *m_impl;
    if (!viewerOpen()) {
        return;
    }
    impl.viewer->afterUpdate();
    if (const std::optional<glm::vec4> box = impl.viewer->pictureBox()) {
        m_record.pictureBox = {box->x, box->y, box->z, box->w};
        if (const std::optional<glm::vec4> stage = impl.viewer->stageBox()) {
            m_record.stageBox = {stage->x, stage->y, stage->z, stage->w};
        }
    }
}

void GameScreensPanel::showPage(std::uint64_t frame) {
    Impl& impl = *m_impl;
    const ui::MediaPage* page = impl.model.current();
    if (page == nullptr || impl.recordedPage == impl.model.index()) {
        return;
    }
    impl.recordedPage = impl.model.index();
    addEvent(frame, "page", page->kind, std::to_string(impl.model.index() + 1));
    impl.viewer->show(impl.model);
    impl.registerPicture();
}

void GameScreensPanel::addEvent(std::uint64_t frame, std::string event, std::string kind, std::string detail) {
    if (m_record.events.size() >= kMaxInteractionEvents) {
        ++m_record.eventsDropped;
        return;
    }
    m_record.events.push_back(
        GameScreenEventRecord{frame, std::move(event), m_gameId, m_from, std::move(kind), std::move(detail)});
}

} // namespace rv::app
