#include <rv/app/wizard_flow.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <rv/core/backups.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/library/folder_listing.hpp>
#include <rv/platform_win/system_probe.hpp>

namespace rv::app {
namespace {

using ui::WizardFolder;
using ui::WizardWork;

[[nodiscard]] std::string stateOf(const esde::FolderCheck& check) {
    if (check.availability == catalog::Availability::Available) {
        return "ok";
    }
    switch (check.reason) {
    case catalog::SourceUnavailableReason::FolderMissing:
        return "missing";
    case catalog::SourceUnavailableReason::Unreachable:
        return "unreachable";
    default:
        return "unreadable";
    }
}

[[nodiscard]] ui::WizardFolderRow rowOf(const std::optional<std::filesystem::path>& folder) {
    ui::WizardFolderRow row;
    if (!folder) {
        row.state = "unknown";
        return row;
    }
    row.path = core::pathToUtf8Lossy(*folder);
    row.state = stateOf(library::checkAccess(*folder));
    return row;
}

/// What page 2 shows of `draft`.
[[nodiscard]] ui::WizardDetection detectionOf(const library::LibraryDraft& draft) {
    ui::WizardDetection detection;
    detection.problem = std::string(library::toString(draft.problem));
    detection.version = draft.discovery.version;
    const auto rootPath = [](const std::optional<esde::ResolvedRoot>& root) {
        return root ? root->path : std::nullopt;
    };
    auto& rows = detection.folders;
    rows[static_cast<std::size_t>(WizardFolder::Home)] = rowOf(draft.discovery.esdeHome);
    rows[static_cast<std::size_t>(WizardFolder::Install)] = rowOf(draft.discovery.installFolder);
    rows[static_cast<std::size_t>(WizardFolder::Rom)] = rowOf(rootPath(draft.roots.rom));
    rows[static_cast<std::size_t>(WizardFolder::Media)] = rowOf(rootPath(draft.roots.media));
    // The ROM folder out of reach stops the library here: a preview would read nothing. A media folder may simply not
    // exist yet (nothing scraped), and the preview says what it finds.
    if (draft.problem == library::DraftProblem::None && rows[static_cast<std::size_t>(WizardFolder::Rom)].state != "ok") {
        detection.problem = "rom-root-unavailable";
    }
    return detection;
}

[[nodiscard]] ui::FolderBrowserFailure failureOf(const Error& error) noexcept {
    switch (error.code) {
    case ErrorCode::NotFound:
        return ui::FolderBrowserFailure::Missing;
    case ErrorCode::Unavailable:
        return ui::FolderBrowserFailure::Unreachable;
    default:
        return ui::FolderBrowserFailure::Unreadable;
    }
}

[[nodiscard]] bool sameFolder(const std::optional<std::filesystem::path>& left,
                              const std::optional<std::filesystem::path>& right) {
    const auto tidy = [](std::filesystem::path path) { return path.make_preferred().lexically_normal(); };
    return left.has_value() == right.has_value() && (!left || tidy(*left) == tidy(*right));
}

[[nodiscard]] bool sameFolders(const catalog::LibraryProfile& left, const catalog::LibraryProfile& right) {
    return sameFolder(left.esdeHome, right.esdeHome) && sameFolder(left.installFolder, right.installFolder) &&
           sameFolder(left.romRoot, right.romRoot) && sameFolder(left.mediaRoot, right.mediaRoot);
}

/// A folder chosen for the ES-DE home: it, or the folder that holds its ES-DE folder as ES-DE's own --home names it.
void setHome(library::LibraryDraftRequest& request, const std::filesystem::path& folder) {
    std::error_code ignored;
    if (std::filesystem::is_directory(folder / "ES-DE", ignored)) {
        request.homeFolder = folder;
    } else {
        request.esdeHome = folder;
    }
}

} // namespace

std::string_view toString(WizardStart start) noexcept {
    switch (start) {
    case WizardStart::None:
        return "none";
    case WizardStart::Fresh:
        return "fresh";
    case WizardStart::Resume:
        return "resume";
    }
    return "unknown";
}

WizardStart wizardStart(bool room, const CommandLineOptions& options, library::LibraryStage stage) noexcept {
    if (!room || stage == library::LibraryStage::Ready || (options.frameLimit && !options.wizard)) {
        return WizardStart::None;
    }
    return stage == library::LibraryStage::NoLibrary ? WizardStart::Fresh : WizardStart::Resume;
}

/// What the jobs' threads and the main thread share.
struct WizardFlow::Box {
    struct Detected {
        std::uint64_t serial = 0;
        Expected<library::LibraryDraft> draft = makeError(ErrorCode::Unknown, "not read", {});
        ui::WizardDetection detection;
    };
    struct Read {
        std::uint64_t serial = 0;
        ui::WizardPreview preview;
        WizardPreviewFigures figures;
        std::string failure;
    };
    struct Listed {
        std::uint64_t serial = 0;
        std::vector<ui::FolderRow> rows;
        std::optional<ui::FolderBrowserFailure> failure;
    };

    std::mutex mutex;
    std::condition_variable ended;
    int running = 0;
    std::function<void()> wake;
    std::optional<Detected> detected;
    std::optional<Read> read;
    std::optional<Listed> listed;
    std::optional<Prepared> prepared;

    /// `slot` takes `answer` unless a newer one stands there: a job that ends late never hides a later job's answer.
    template <typename Answer>
    void post(std::optional<Answer>& slot, Answer answer) {
        const std::lock_guard lock(mutex);
        if (!slot || slot->serial < answer.serial) {
            slot = std::move(answer);
        }
    }

    /// Runs `job` on a thread that holds only this box.
    template <typename Job>
    static void run(const std::shared_ptr<Box>& box, Job job) {
        {
            const std::lock_guard lock(box->mutex);
            ++box->running;
        }
        std::thread([box, job = std::move(job)]() mutable {
            try {
                job(*box);
            } catch (const std::exception& error) {
                spdlog::error("A job of the wizard failed: {}", error.what());
            } catch (...) {
                spdlog::error("A job of the wizard failed");
            }
            // The wake under the lock: once the destructor cleared it, none is on its way to a host that is gone.
            const std::lock_guard lock(box->mutex);
            --box->running;
            if (box->wake) {
                box->wake();
            }
            box->ended.notify_all();
        }).detach();
    }
};

WizardFlow::WizardFlow(Options options) : m_options(std::move(options)), m_box(std::make_shared<Box>()) {
    m_box->wake = m_options.wake;
    m_home = m_options.esdeHome;
    m_install = m_options.esdeInstall;
    m_id = catalog::LibraryId::parse(catalog::LibraryId::kDefault).value();
}

WizardFlow::~WizardFlow() {
    if (m_readCancel) {
        m_readCancel->store(true);
    }
    std::unique_lock lock(m_box->mutex);
    if (!m_box->ended.wait_for(lock, m_options.stopBound, [this] { return m_box->running == 0; })) {
        spdlog::warn("A job of the wizard did not end within {} ms; it is left to end by itself",
                     m_options.stopBound.count());
    }
    m_box->wake = nullptr;
}

void WizardFlow::open(WizardStart start) {
    if (start == WizardStart::Resume) {
        const auto profile = esde::readLibraryFile(m_options.paths.configDir() / esde::kLibraryFileName);
        if (profile) {
            m_profile = *profile;
            m_id = profile->id;
            // The folders of the file stand, but what the command line gave, which only a detection can turn into a
            // library: page 3 then reads that, and page 4 writes it when it differs from the file.
            m_detectBeforeRead = m_home.has_value() || m_install.has_value();
            if (!m_home) {
                m_home = profile->esdeHome;
            }
            if (!m_install) {
                m_install = profile->installFolder;
            }
            m_rom = profile->romRoot;
            m_media = profile->mediaRoot;
        } else {
            spdlog::warn("Wizard: library.json cannot be read: {}", profile.error().toString());
            m_changed = true; // whatever the detection finds replaces it
        }
    }
    m_model.open(start == WizardStart::Resume);
}

library::LibraryDraftRequest WizardFlow::draftRequest() const {
    library::LibraryDraftRequest request;
    request.id = m_id;
    if (m_home) {
        setHome(request, *m_home);
    }
    request.installFolder = m_install;
    request.romRoot = m_rom;
    request.mediaRoot = m_media;
    request.userData = m_options.paths.userDataRoot();
    return request;
}

void WizardFlow::detect() {
    const std::uint64_t serial = ++m_detectSerial;
    m_detecting = true;
    ui::WizardDetection checking = m_model.detection();
    m_model.setDetection(WizardWork::Running, std::move(checking));
    // The system probe answers only an interactive run's discovery: with --esde-home the machine is never asked.
    Box::run(m_box, [serial, home = m_home, install = m_install, rom = m_rom, media = m_media, id = m_id,
                     userData = m_options.paths.userDataRoot(), noProbe = m_options.esdeHome.has_value()](Box& box) {
        library::LibraryDraftRequest request;
        request.id = id;
        if (home) {
            setHome(request, *home);
        }
        request.installFolder = install;
        request.romRoot = rom;
        request.mediaRoot = media;
        request.userData = userData;
        Box::Detected answer;
        answer.serial = serial;
        const library::NoSystemProbe none;
        const platform_win::Win32SystemProbe machine;
        answer.draft = library::draftLibrary(request, noProbe ? static_cast<const core::ISystemProbe&>(none)
                                                              : static_cast<const core::ISystemProbe&>(machine));
        if (answer.draft) {
            answer.detection = detectionOf(*answer.draft);
        } else {
            answer.detection.problem = "home-unknown";
        }
        box.post(box.detected, std::move(answer));
    });
}

void WizardFlow::cancelRead() {
    if (m_readCancel) {
        m_readCancel->store(true);
    }
    ++m_readSerial; // what it still answers is dropped
    if (m_reading || std::exchange(m_readAfterDetect, false)) {
        m_reading = false;
        m_model.setPreview(WizardWork::Idle, {});
    }
}

void WizardFlow::read() {
    cancelRead(); // one still running, for what the user left
    if (m_detectBeforeRead && !m_draft) {
        // The answer of this detection asks for the read again (poll()).
        m_detectBeforeRead = false;
        m_readAfterDetect = true;
        m_model.setPreview(WizardWork::Running, {});
        detect();
        return;
    }
    const std::uint64_t serial = m_readSerial;
    m_readCancel = std::make_shared<std::atomic<bool>>(false);
    // A detection's library, once there is one: what page 2 shows. library.json's only until then.
    const std::optional<catalog::LibraryProfile>& profile = m_draft ? m_draft->profile : m_profile;
    if (!profile) {
        m_model.setPreview(WizardWork::Failed, {}, "library-unreadable");
        return;
    }
    m_reading = true;
    m_model.setPreview(WizardWork::Running, {});
    Box::run(m_box, [serial, profile = *profile, cancel = m_readCancel, readLibrary = m_options.readLibrary](Box& box) {
        Box::Read answer;
        answer.serial = serial;
        library::PreviewOptions options;
        options.cancel = cancel.get();
        const auto preview = readLibrary ? readLibrary(profile, options) : library::previewLibrary(profile, options);
        if (preview) {
            const library::PreviewCounts& total = preview->total;
            answer.preview.systems = preview->snapshot.systems.size();
            answer.preview.systemsWithGames =
                static_cast<std::size_t>(std::count_if(preview->counts.begin(), preview->counts.end(), [](const auto& row) {
                    return row.second.games > 0;
                }));
            answer.preview.games = total.games;
            answer.preview.unrecognized = total.notAGame;
            answer.preview.errors = static_cast<std::size_t>(
                std::count_if(preview->snapshot.issues.begin(), preview->snapshot.issues.end(),
                              [](const catalog::Issue& issue) { return issue.severity == catalog::IssueSeverity::Error; }));
            answer.figures = WizardPreviewFigures{total.entries, total.esde, total.missing, total.members,
                                                  preview->snapshot.issues.size()};
            if (!cancel->load()) {
                if (const auto coverage = library::previewCoverage(*preview)) {
                    answer.preview.media = coverage->withAny;
                    if (const std::optional<double> share = coverage->anyShare()) {
                        answer.preview.coveragePercent = static_cast<int>(std::lround(*share * 100.0));
                    }
                } else {
                    spdlog::warn("Wizard: the media coverage cannot be read: {}", coverage.error().toString());
                }
            }
        } else if (preview.error().code != ErrorCode::Cancelled) {
            // The error names the user's folders: at debug level only.
            spdlog::debug("Wizard: the library cannot be read: {}", preview.error().toString());
            answer.failure = "library-unreadable";
        }
        box.post(box.read, std::move(answer));
    });
}

void WizardFlow::listing(const ui::FolderListingRequest& request) {
    m_listing = true;
    m_listingSerial = request.serial;
    Box::run(m_box, [request, listFolders = m_options.listFolders](Box& box) {
        Box::Listed answer;
        answer.serial = request.serial;
        const auto toRows = [&](const std::vector<library::ListedFolder>& folders) {
            answer.rows.reserve(folders.size());
            for (const library::ListedFolder& folder : folders) {
                answer.rows.push_back(ui::FolderRow{folder.path, folder.name, false});
            }
        };
        if (request.folder.empty()) {
            toRows(library::listDrives());
        } else if (const auto folders = listFolders ? listFolders(request.folder) : library::listFolders(request.folder)) {
            toRows(*folders);
        } else {
            answer.failure = failureOf(folders.error());
        }
        box.post(box.listed, std::move(answer));
    });
}

void WizardFlow::command(const ui::WizardCommand& command) {
    switch (command.kind) {
    case ui::WizardCommandKind::Detect:
        cancelRead(); // Back from page 3 comes this way
        detect();
        break;
    case ui::WizardCommandKind::Read:
        read();
        break;
    case ui::WizardCommandKind::Browse: {
        m_browsing = command.folder;
        const std::string& shown = m_model.detection().folders[static_cast<std::size_t>(command.folder)].path;
        std::optional<std::filesystem::path> start;
        if (auto folder = core::tryPathFromUtf8(shown); folder && !shown.empty()) {
            start = std::move(*folder);
        }
        m_browser.open(start);
        break;
    }
    default:
        break;
    }
}

void WizardFlow::browserClosed(const ui::FolderBrowserResult& result) {
    const std::optional<WizardFolder> folder = std::exchange(m_browsing, std::nullopt);
    if (!folder || !result.chosen) {
        return;
    }
    ++m_browsed;
    m_changed = true;
    switch (*folder) {
    case WizardFolder::Home:
        m_home = result.chosen;
        break;
    case WizardFolder::Install:
        m_install = result.chosen;
        break;
    case WizardFolder::Rom:
        m_rom = result.chosen;
        break;
    case WizardFolder::Media:
        m_media = result.chosen;
        break;
    }
    detect();
}

void WizardFlow::prepareImport() {
    m_preparing = true;
    const std::filesystem::path file = m_options.paths.configDir() / esde::kLibraryFileName;
    Box::run(m_box, [file, draft = m_draft, changed = m_changed](Box& box) {
        Prepared answer;
        std::error_code ignored;
        const bool present = std::filesystem::exists(file, ignored);
        if (present && !changed) {
            // library.json stands as the wizard resumed it, or as the last try wrote it.
        } else if (!draft) {
            answer.failure = "library-write";
        } else {
            if (present) {
                // Never replaced without its copy (ADR-0018 §2's rule for the user's files).
                if (const auto backup = core::backUpFile(file, file.parent_path() / "backups"); !backup) {
                    spdlog::warn("Wizard: library.json is kept: no copy of it can be made: {}",
                                 backup.error().toString());
                    answer.failure = "library-backup";
                }
            }
            if (answer.failure.empty()) {
                const Status written =
                    library::createLibrary(*draft, file, present ? esde::ExistingFile::Replace : esde::ExistingFile::Keep);
                if (written) {
                    answer.changed = true;
                } else {
                    spdlog::warn("Wizard: library.json cannot be written: {}", written.error().toString());
                    answer.failure = written.error().code == ErrorCode::Conflict ? "library-exists" : "library-write";
                }
            }
        }
        const std::lock_guard lock(box.mutex);
        box.prepared = std::move(answer);
    });
}

std::optional<WizardFlow::Prepared> WizardFlow::takePrepared() {
    std::optional<Prepared> prepared;
    {
        const std::lock_guard lock(m_box->mutex);
        prepared = std::exchange(m_box->prepared, std::nullopt);
    }
    if (prepared) {
        m_preparing = false;
        if (prepared->failure.empty()) {
            m_changed = false; // only now: a write that failed keeps what the user chose to be written
        }
    }
    return prepared;
}

void WizardFlow::poll() {
    if (const std::optional<ui::FolderListingRequest> request = m_browser.takeRequest()) {
        listing(*request);
    }
    std::optional<Box::Detected> detected;
    std::optional<Box::Read> reading;
    std::optional<Box::Listed> listed;
    {
        const std::lock_guard lock(m_box->mutex);
        detected = std::exchange(m_box->detected, std::nullopt);
        reading = std::exchange(m_box->read, std::nullopt);
        listed = std::exchange(m_box->listed, std::nullopt);
    }
    if (detected && detected->serial == m_detectSerial) {
        m_detecting = false;
        if (detected->draft) {
            m_draft = std::move(*detected->draft);
        } else {
            m_draft.reset();
            spdlog::warn("Wizard: discovery failed: {}", detected->draft.error().toString());
        }
        // What the command line gave differs from the file: page 4 writes it.
        if (m_profile && m_draft && m_draft->profile && !sameFolders(*m_profile, *m_draft->profile)) {
            m_changed = true;
        }
        m_model.setDetection(WizardWork::Done, std::move(detected->detection));
        if (std::exchange(m_readAfterDetect, false)) {
            read();
        }
    }
    if (reading && reading->serial == m_readSerial) {
        m_reading = false;
        if (reading->failure.empty()) {
            m_figures = reading->figures;
            m_model.setPreview(WizardWork::Done, reading->preview);
        } else {
            m_model.setPreview(WizardWork::Failed, {}, std::move(reading->failure));
        }
    }
    if (listed) {
        if (listed->serial == m_listingSerial) {
            m_listing = false; // a stale job ending says nothing of the one awaited
        }
        if (listed->failure) {
            m_browser.setFailure(listed->serial, *listed->failure);
        } else {
            m_browser.setListing(listed->serial, std::move(listed->rows));
        }
    }
}

int WizardFlow::runningJobs() const {
    const std::lock_guard lock(m_box->mutex);
    return m_box->running;
}

bool WizardFlow::busy() const noexcept {
    return m_detecting || m_reading || m_preparing || (m_listing && m_browser.isOpen());
}

} // namespace rv::app
