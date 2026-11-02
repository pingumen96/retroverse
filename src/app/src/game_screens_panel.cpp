#include "game_screens_panel.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/launcher/manual_open.hpp>
#include <rv/platform_win/system_probe.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
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

/// The media viewer model's clock for the manual's second Activate (wall time: it guards against another explorer.exe,
/// whatever the run's simulated time).
[[nodiscard]] double monotonicSeconds() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// What a loader job reads: a page's picture, or (I5 step 8) where the clip's or the manual's file is.
enum class JobKind : std::uint8_t { Picture, Video, Manual };

[[nodiscard]] JobKind jobKindOf(std::string_view kind) noexcept {
    return kind == ui::media_viewer::kVideosKind    ? JobKind::Video
           : kind == ui::media_viewer::kManualsKind ? JobKind::Manual
                                                    : JobKind::Picture;
}

/// The last element of a '/'-separated path below the media root (the manual's file name, shown on its page).
[[nodiscard]] std::string fileNameOf(std::string_view relativePath) {
    const std::size_t slash = relativePath.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1));
}

/// The viewer's pictures, read and decoded on a thread of their own (the main thread never reads the user's files).
class PictureLoader {
public:
    struct Job {
        std::uint64_t serial = 0;
        std::size_t index = 0;
        std::string relativePath;
        JobKind kind = JobKind::Picture;
    };
    struct Result {
        std::uint64_t serial = 0;
        std::size_t index = 0;
        JobKind kind = JobKind::Picture;
        std::optional<assets::ImageData> picture;
        /// I5 step 8: the clip's or the manual's file below the media root (a \\?\ path).
        std::filesystem::path file;
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
            result.kind = job.kind;
            if (!*root) {
                result.error = root->error();
            } else if (job.kind != JobKind::Picture) {
                // I5 step 8: the clip's file (the decoder says what it finds there) or the manual's (there or not).
                if (auto file = core::extendedLengthPathBelow(**root, job.relativePath); !file) {
                    result.error = file.error();
                } else {
                    std::error_code error;
                    if (job.kind == JobKind::Manual && !std::filesystem::is_regular_file(*file, error)) {
                        result.error = Error{ErrorCode::NotFound, "the manual's file is not there", "manual"};
                    }
                    result.file = std::move(*file);
                }
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

    ~Impl() {
        dropTextures();
        registerVideo(0, {});
    }

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

    /// I5 step 8: the clip's texture (the service's view of it) registered for the document (0: the key emptied).
    void registerVideo(unsigned id, glm::ivec2 size) {
        if (id != registeredVideo || (id != 0 && size != registeredVideoSize)) {
            system.setTexture(ui::media_viewer::kVideoTexture, id, size);
            registeredVideo = id;
            registeredVideoSize = size;
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

    /// The page of `kind`, if the game has one.
    [[nodiscard]] std::optional<std::size_t> pageOf(std::string_view kind) const { return model.indexOf(kind); }
    [[nodiscard]] bool showing(std::string_view kind) const {
        const ui::MediaPage* page = model.current();
        return viewer && viewer->visible() && page != nullptr && page->kind == kind;
    }

    ui::UiView& view;
    ui::UiSystem& system;
    std::unique_ptr<ui::MediaViewerScreen> viewer;
    std::unique_ptr<ui::GameInfoScreen> info;
    ui::MediaViewerModel model;
    std::optional<ui::GameInfoContent> infoContent;
    /// I7 step 6: the focused row of Informazioni (nothing: the text has the input).
    std::optional<ui::InfoRow> infoRow;
    /// The viewer's opening (its loader jobs carry it) and the pictures waiting for their upload.
    std::uint64_t serial = 0;
    std::vector<std::optional<gl::Texture2D>> textures;
    std::deque<PictureLoader::Result> waiting;
    unsigned registered = 0;
    /// The page whose "page" event was recorded last.
    std::optional<std::size_t> recordedPage;

    // I5 step 8: the clip's file and its key below the media root (the loader found it), the clip's state the service
    // gave last (its "video" event recorded once per change) and its registered view; the manual's file; the manual's
    // runner and the machine's Windows folder (explorer.exe).
    std::filesystem::path videoFile;
    std::string videoKey;
    ViewerVideo lastVideo;
    std::string lastVideoEvent;
    unsigned registeredVideo = 0;
    glm::ivec2 registeredVideoSize{0, 0};
    std::filesystem::path manualFile;
    platform_win::Win32ProcessRunner manualRunner;
    platform_win::Win32SystemProbe probe;

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
    impl.videoFile.clear();
    impl.videoKey.clear();
    impl.manualFile.clear();
    impl.lastVideo = ViewerVideo{};
    impl.lastVideoEvent.clear();
    for (std::size_t index = 0; index < impl.model.pages().size(); ++index) {
        const std::string& kind = impl.model.pages()[index].kind;
        const auto file = std::find_if(request.pages.begin(), request.pages.end(),
                                       [&](const MediaPageFile& page) { return page.kind == kind; });
        jobs.push_back(PictureLoader::Job{impl.serial + 1, index, file->relativePath, jobKindOf(kind)});
        if (kind == ui::media_viewer::kVideosKind) {
            impl.videoKey = file->relativePath;
        } else if (kind == ui::media_viewer::kManualsKind) {
            impl.model.setManualFile(index, ui::PictureState::Loading, fileNameOf(file->relativePath));
        }
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
    impl.infoRow.reset();
    impl.info->setFocusedRow(std::nullopt);
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
        // I5 step 8: the clip's view is the service's; its key is emptied before the service lets it go.
        impl.registerVideo(0, {});
        impl.videoFile.clear();
        impl.videoKey.clear();
        impl.manualFile.clear();
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
        if (command == ui::UiCommand::Back) {
            return Switch::None;
        }
        switch (impl.model.apply(command, monotonicSeconds())) {
        case ui::ViewerStep::Moved:
            showPage(frame);
            break;
        case ui::ViewerStep::TogglePause:
            videoPauseToggled(frame);
            break;
        case ui::ViewerStep::OpenManual:
            openManual(frame);
            break;
        case ui::ViewerStep::None:
        case ui::ViewerStep::Close:
            break;
        }
        return Switch::None;
    }
    if (infoOpen()) {
        const ui::InfoStep step = ui::infoStepOf(command);
        if (step == ui::InfoStep::Media) {
            return Switch::Media;
        }
        const std::vector<ui::InfoRow> rows = ui::infoRowsOf(*impl.infoContent);
        const auto at = impl.infoRow ? std::find(rows.begin(), rows.end(), *impl.infoRow) : rows.end();
        if (at != rows.end()) {
            switch (command) {
            case ui::UiCommand::Up:
                setInfoRow(at == rows.begin() ? std::nullopt : std::optional<ui::InfoRow>(*(at - 1)), frame);
                return Switch::None;
            case ui::UiCommand::Down:
                if (at + 1 != rows.end()) {
                    setInfoRow(*(at + 1), frame);
                }
                return Switch::None;
            case ui::UiCommand::Activate:
                return pressInfoRow(*at, frame);
            case ui::UiCommand::Left:
            case ui::UiCommand::Right:
                if (at->kind == ui::InfoRow::Kind::Status) {
                    return command == ui::UiCommand::Left ? Switch::StatusPrevious : Switch::StatusNext;
                }
                return Switch::None;
            default:
                break;
            }
        }
        if (step != ui::InfoStep::Close && step != ui::InfoStep::None) {
            if (impl.info->scroll(step)) {
                addEvent(frame, "scroll", {}, std::string(ui::toString(step)));
            } else if (command == ui::UiCommand::Down && !rows.empty() && at == rows.end()) {
                setInfoRow(rows.front(), frame); // at the text's end, on to the rows
            }
        }
    }
    return Switch::None;
}

void GameScreensPanel::setInfoRow(std::optional<ui::InfoRow> row, std::uint64_t frame) {
    Impl& impl = *m_impl;
    if (impl.infoRow == row) {
        return;
    }
    impl.infoRow = row;
    impl.info->setFocusedRow(row);
    addEvent(frame, "row", {}, !row                                     ? std::string("text")
                               : row->kind == ui::InfoRow::Kind::Status  ? std::string("status")
                               : row->kind == ui::InfoRow::Kind::Emulator ? std::string("emulator")
                                                                          : fmt::format("version {}", row->version));
}

GameScreensPanel::Switch GameScreensPanel::pressInfoRow(const ui::InfoRow& row, std::uint64_t frame) {
    setInfoRow(row, frame);
    switch (row.kind) {
    case ui::InfoRow::Kind::Status:
        return Switch::StatusNext;
    case ui::InfoRow::Kind::Emulator:
        return Switch::EmulatorList;
    case ui::InfoRow::Kind::Version:
        return Switch::VersionLaunch;
    }
    return Switch::None;
}

const ui::GameInfoContent* GameScreensPanel::infoContent() const noexcept {
    const Impl& impl = *m_impl;
    return impl.infoContent ? &*impl.infoContent : nullptr;
}

std::optional<ui::InfoRow> GameScreensPanel::infoRow() const noexcept {
    return m_impl->infoRow;
}

void GameScreensPanel::setInfoControls(ui::GameInfoControls controls, std::uint64_t frame) {
    Impl& impl = *m_impl;
    if (!impl.infoContent) {
        return;
    }
    addEvent(frame, "controls", {}, fmt::format("{} {}", controls.status, controls.own ? "own" : controls.source));
    impl.infoContent->controls = std::move(controls);
    impl.info->show(*impl.infoContent);
}

void GameScreensPanel::noteInfo(std::uint64_t frame, std::string event, std::string detail) {
    addEvent(frame, std::move(event), {}, std::move(detail));
}

GameScreensPanel::Switch GameScreensPanel::update(std::uint64_t frame) {
    Impl& impl = *m_impl;
    Switch asked = Switch::None;
    if (const std::optional<ui::ViewerClick> click = impl.viewer->takeClicked(); click && viewerOpen()) {
        switch (*click) {
        case ui::ViewerClick::Close:
            close(frame);
            break;
        case ui::ViewerClick::Previous:
        case ui::ViewerClick::Next:
            if (impl.model.apply(*click == ui::ViewerClick::Previous ? ui::UiCommand::Left : ui::UiCommand::Right) ==
                ui::ViewerStep::Moved) {
                showPage(frame);
            }
            break;
        case ui::ViewerClick::Pause:
        case ui::ViewerClick::Manual:
            // The page's own button: Activate's step on it.
            if (const ui::ViewerStep step = impl.model.apply(ui::UiCommand::Activate, monotonicSeconds());
                step == ui::ViewerStep::TogglePause) {
                videoPauseToggled(frame);
            } else if (step == ui::ViewerStep::OpenManual) {
                openManual(frame);
            }
            break;
        }
    }
    if (const std::optional<ui::InfoStep> click = impl.info->takeClicked(); click && infoOpen()) {
        if (*click == ui::InfoStep::Close) {
            close(frame);
        } else if (*click == ui::InfoStep::Media) {
            asked = Switch::Media;
        }
    }
    if (const std::optional<ui::InfoRow> click = impl.info->takeClickedRow(); click && infoOpen()) {
        asked = pressInfoRow(*click, frame);
    }
    if (viewerOpen()) {
        for (PictureLoader::Result& result : impl.loader.take()) {
            if (result.serial != impl.serial || result.index >= impl.model.pages().size()) {
                continue;
            }
            if (result.kind == JobKind::Video) {
                // I5 step 8: the clip's file: the service opens it once the page shows.
                if (result.error) {
                    impl.model.setVideo(result.index, ui::PictureState::Failed, 0, 0, "file-not-found");
                    addEvent(frame, "video", std::string(ui::media_viewer::kVideosKind), "failed file-not-found");
                    spdlog::debug("Media viewer: the clip of {} cannot be found: {}", m_gameId,
                                  result.error->toString());
                } else {
                    impl.videoFile = std::move(result.file);
                }
            } else if (result.kind == JobKind::Manual) {
                const ui::MediaPage& page = impl.model.pages()[result.index];
                if (result.error) {
                    impl.model.setManualFile(result.index, ui::PictureState::Failed, page.name, "file-not-found");
                    addEvent(frame, "manual", std::string(ui::media_viewer::kManualsKind), "missing file-not-found");
                    spdlog::debug("Media viewer: the manual of {} cannot be found: {}", m_gameId,
                                  result.error->toString());
                } else {
                    impl.manualFile = std::move(result.file);
                    impl.model.setManualFile(result.index, ui::PictureState::Shown, page.name);
                    addEvent(frame, "manual", std::string(ui::media_viewer::kManualsKind), "found " + page.name);
                }
            } else {
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
    if (infoOpen()) {
        impl.info->afterUpdate();
        m_record.infoTextCut = std::max(m_record.infoTextCut, impl.info->textCut());
    }
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
    if (const std::optional<glm::vec4> box = impl.viewer->videoBox()) {
        m_record.videoBox = {box->x, box->y, box->z, box->w};
    }
}

ViewerRequest GameScreensPanel::viewerRequest() const {
    const Impl& impl = *m_impl;
    ViewerRequest request;
    request.open = viewerOpen();
    const ui::MediaPage* page = impl.model.current();
    if (!request.open || page == nullptr || page->kind != ui::media_viewer::kVideosKind ||
        page->state == ui::PictureState::Failed || impl.videoFile.empty()) {
        return request;
    }
    request.clip = ViewerClip{m_gameId, impl.videoFile, impl.videoKey, page->paused, impl.serial};
    return request;
}

void GameScreensPanel::setViewerVideo(const ViewerVideo& video, std::uint64_t frame) {
    Impl& impl = *m_impl;
    const std::optional<std::size_t> index = impl.pageOf(ui::media_viewer::kVideosKind);
    const bool shown = viewerOpen() && index && impl.showing(ui::media_viewer::kVideosKind);
    if (!shown) {
        impl.registerVideo(0, {});
        return;
    }
    const ui::MediaPage& page = impl.model.pages()[*index];
    std::string event;
    switch (video.state) {
    case ViewerVideoState::Playing:
    case ViewerVideoState::Paused:
        impl.model.setVideo(*index, ui::PictureState::Shown, video.width, video.height);
        impl.registerVideo(video.texture, {video.width, video.height});
        event = video.state == ViewerVideoState::Paused ? std::string("paused")
                                                         : fmt::format("playing {}x{}", video.width, video.height);
        ++m_record.videoFrames;
        break;
    case ViewerVideoState::Failed:
        impl.model.setVideo(*index, ui::PictureState::Failed, 0, 0, video.reason);
        impl.registerVideo(0, {});
        event = (video.fromVerdict ? "verdict " : "failed ") + video.reason;
        break;
    case ViewerVideoState::Waiting:
    case ViewerVideoState::None:
        if (page.state != ui::PictureState::Failed) {
            impl.model.setVideo(*index, ui::PictureState::Loading);
            event = "waiting";
        }
        impl.registerVideo(0, {});
        break;
    }
    if (!event.empty() && event != impl.lastVideoEvent) {
        impl.lastVideoEvent = event;
        addEvent(frame, "video", std::string(ui::media_viewer::kVideosKind), event);
        if (video.state == ViewerVideoState::Failed) {
            spdlog::info("Frame {}: the media viewer's clip of {} cannot play ({}): its fallback shows", frame,
                         m_gameId, video.reason);
        }
    }
    impl.lastVideo = video;
    impl.viewer->show(impl.model);
}

void GameScreensPanel::videoPauseToggled(std::uint64_t frame) {
    const Impl& impl = *m_impl;
    const ui::MediaPage* page = impl.model.current();
    if (page == nullptr) {
        return;
    }
    spdlog::info("Frame {}: the media viewer's clip of {} {}", frame, m_gameId,
                 page->paused ? "pauses" : "plays again");
    impl.viewer->show(impl.model);
}

void GameScreensPanel::openManual(std::uint64_t frame) {
    Impl& impl = *m_impl;
    const std::size_t index = impl.model.index();
    if (impl.manualFile.empty()) {
        impl.model.setManualOutcome(index, ui::ManualState::Ready); // asked again once its file is there
        return;
    }
    const std::string label = fmt::format("the manual of {}", m_gameId);
    auto request = launcher::planManualOpen(impl.manualFile, impl.probe, label);
    if (!request) {
        // I5 step 8's review: a path the plan refuses (Unsupported, or not UTF-8) is no failure of Windows: its own
        // sentence; explorer.exe missing (the Windows folder unknown, the program not there) is a failure.
        const ErrorCode code = request.error().code;
        const bool unsupported = code == ErrorCode::Unsupported || code == ErrorCode::InvalidArgument;
        impl.model.setManualOutcome(index, unsupported ? ui::ManualState::Unsupported : ui::ManualState::Failed,
                                    std::string(toString(code)));
        addEvent(frame, "manual", std::string(ui::media_viewer::kManualsKind),
                 (unsupported ? "unsupported " : "failed ") + std::string(toString(code)));
        // The message may name the user's file: at debug level only.
        spdlog::debug("Media viewer: the manual of {} cannot be opened: {}", m_gameId, request.error().toString());
        spdlog::info("Frame {}: the manual of {} cannot be opened by explorer.exe", frame, m_gameId);
        impl.viewer->show(impl.model);
        return;
    }
    // Started, never followed: no game session, Retroverse keeps running (ADR-0016 §10).
    auto started = impl.manualRunner.start(*request, {});
    if (!started) {
        const bool refused = launcher::isGuardRefusal(started.error());
        impl.model.setManualOutcome(index, refused ? ui::ManualState::Refused : ui::ManualState::Failed,
                                    refused ? std::string("guard") : std::string(toString(started.error().code)));
        addEvent(frame, "manual", std::string(ui::media_viewer::kManualsKind),
                 refused ? std::string("refused") : "failed " + std::string(toString(started.error().code)));
        if (refused) {
            spdlog::info("Frame {}: the manual of {} was not opened: the automation guard refused explorer.exe ({})",
                         frame, m_gameId, launcher::kLaunchAllowListVariable);
        } else {
            spdlog::debug("Media viewer: the manual of {} did not open: {}", m_gameId, started.error().toString());
            spdlog::warn("Frame {}: the manual of {} did not open ({})", frame, m_gameId,
                         toString(started.error().code));
        }
        impl.viewer->show(impl.model);
        return;
    }
    (*started)->stopWatching();
    impl.model.setManualOutcome(index, ui::ManualState::Opened, {}, monotonicSeconds());
    addEvent(frame, "manual", std::string(ui::media_viewer::kManualsKind), "opened");
    spdlog::info("Frame {}: the manual of {} opens in Windows' default program (explorer.exe, process {})", frame,
                 m_gameId, (*started)->processId());
    impl.viewer->show(impl.model);
}

void GameScreensPanel::showPage(std::uint64_t frame) {
    Impl& impl = *m_impl;
    const ui::MediaPage* page = impl.model.current();
    if (page == nullptr || impl.recordedPage == impl.model.index()) {
        return;
    }
    impl.recordedPage = impl.model.index();
    addEvent(frame, "page", page->kind, std::to_string(impl.model.index() + 1));
    if (page->kind != ui::media_viewer::kVideosKind) {
        impl.registerVideo(0, {}); // the service lets the clip go at its next frame
        impl.lastVideoEvent.clear();
    }
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
