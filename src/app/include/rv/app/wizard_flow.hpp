#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The wizard's flow (ADR-0018 §4): what a page of ui::WizardModel asks that may wait on a disk, on a thread of its own, so
// that a slow or vanished volume never blocks a frame: ES-DE's discovery and the folders' access test, the library's
// preview, the folder browser's listings, library.json's writing. Each answer wakes the host and is collected by poll()
// on the main thread, by serial, so one that comes after the user went elsewhere is dropped. The threads hold nothing of
// this object but a shared box; the destructor cancels the read, waits for them within a bound, and leaves one a dead
// volume holds to end by itself. ES-DE's settings go through esde::readSettings(): the scraper's credentials are never
// read. Nothing is written under ES-DE's, the ROM or the media folders: library.json goes under the data directory.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <rv/app/command_line.hpp>
#include <rv/core/paths.hpp>
#include <rv/library/library_creation.hpp>
#include <rv/library/library_preview.hpp>
#include <rv/ui/folder_browser_model.hpp>
#include <rv/ui/wizard_model.hpp>

namespace rv::app {

/// How a run's wizard starts.
enum class WizardStart : std::uint8_t {
    None,   ///< no wizard
    Fresh,  ///< on its first page
    Resume, ///< on its preview: library.json stands, no import ever finished
};
/// "none", "fresh", "resume".
[[nodiscard]] std::string_view toString(WizardStart start) noexcept;

/// ADR-0018 §4: an interactive room run whose data directory has no library or no catalog starts the wizard; a
/// frame-limited one never does by itself (no automated run may discover the machine's ES-DE): only with --wizard, which
/// needs --esde-home. `room`: the run shows the generated building.
[[nodiscard]] WizardStart wizardStart(bool room, const CommandLineOptions& options, library::LibraryStage stage) noexcept;

/// The preview's figures the run report keeps beside the page's.
struct WizardPreviewFigures {
    std::size_t entries = 0;
    std::size_t esdeGames = 0;
    std::size_t missing = 0;
    std::size_t members = 0;
    std::size_t issues = 0;
};

class WizardFlow {
public:
    struct Options {
        core::Paths paths;
        /// --esde-home, --esde-install: discovery stands on them and the system probe is never asked.
        std::optional<std::filesystem::path> esdeHome;
        std::optional<std::filesystem::path> esdeInstall;
        /// Called on a job's thread once it answered (AppContext::wakeCallback(): thread-safe).
        std::function<void()> wake;
    };

    /// How long the destructor waits for the jobs at most.
    static constexpr std::chrono::milliseconds kStopBound{5000};

    explicit WizardFlow(Options options);
    ~WizardFlow();
    WizardFlow(const WizardFlow&) = delete;
    WizardFlow& operator=(const WizardFlow&) = delete;

    /// Opens the wizard. Resume reads library.json first (the file says where it stands), and a library.json that cannot
    /// be read leaves page 3 its failure.
    void open(WizardStart start);

    [[nodiscard]] ui::WizardModel& model() noexcept { return m_model; }
    [[nodiscard]] ui::FolderBrowserModel& browser() noexcept { return m_browser; }

    /// Starts what `command` asks of the flow: Detect, Browse, Read. The others are the application's.
    void command(const ui::WizardCommand& command);
    /// The folder browser closed: a folder chosen replaces the page's and the detection runs again.
    void browserClosed(const ui::FolderBrowserResult& result);

    /// Page 4 begins: library.json is written on a job (with what the user changed, the file before it kept aside first:
    /// a backup that fails leaves it as it is). takePrepared() answers.
    void prepareImport();
    /// How prepareImport() ended.
    struct Prepared {
        /// A key of the document when it failed: "library-exists", "library-backup", "library-write".
        std::string failure;
        /// library.json is another than the last one a catalog could have opened: a new catalog must open it.
        bool changed = false;
    };
    [[nodiscard]] std::optional<Prepared> takePrepared();

    /// Once per frame and per wake: the browser's listing asked, the jobs' answers given to the models.
    void poll();
    /// A job of the flow is awaited (lockstep runs wait for it).
    [[nodiscard]] bool busy() const noexcept;

    [[nodiscard]] const WizardPreviewFigures& figures() const noexcept { return m_figures; }
    /// Folders chosen in the browser.
    [[nodiscard]] std::uint64_t browsed() const noexcept { return m_browsed; }

private:
    struct Box;

    [[nodiscard]] library::LibraryDraftRequest draftRequest() const;
    void detect();
    void read();
    void listing(const ui::FolderListingRequest& request);

    Options m_options;
    ui::WizardModel m_model;
    ui::FolderBrowserModel m_browser;
    std::shared_ptr<Box> m_box;
    /// What the user chose over what discovery finds (--esde-home and --esde-install begin them).
    std::optional<std::filesystem::path> m_home;
    std::optional<std::filesystem::path> m_install;
    std::optional<std::filesystem::path> m_rom;
    std::optional<std::filesystem::path> m_media;
    catalog::LibraryId m_id;
    /// The library as far as it is known: resumed from library.json, or the last detection's.
    std::optional<library::LibraryDraft> m_draft;
    std::optional<catalog::LibraryProfile> m_profile;
    std::optional<ui::WizardFolder> m_browsing;
    /// The folders differ from library.json's; set until it was written.
    bool m_changed = false;
    std::uint64_t m_detectSerial = 0;
    std::uint64_t m_readSerial = 0;
    bool m_detecting = false;
    bool m_reading = false;
    bool m_preparing = false;
    bool m_listing = false;
    std::shared_ptr<std::atomic<bool>> m_readCancel;
    WizardPreviewFigures m_figures;
    std::uint64_t m_browsed = 0;
};

} // namespace rv::app
