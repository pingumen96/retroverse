#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The first-run wizard (ADR-0018 §4; assets/ui/wizard.rml through ui::WizardScreen): seven pages from the pad alone. The
// model holds the pages, what each shows and the focus; what a page does happens elsewhere (app::WizardFlow on workers,
// the app for the catalog and the world): a command asks for it, and the answers come back through the setters, so that
// a page never waits for anything. Which folder a row shows, and the folder browser over the page, are the flow's.
//
// | # | Page       | Way forward              | Way back                  |
// |---|------------|--------------------------|---------------------------|
// | 1 | Welcome    | Avanti                   | Back rests on Esci        |
// | 2 | Detection  | Avanti, no problem left  | to 1                      |
// | 3 | Preview    | Avanti, read             | to 2                      |
// | 4 | Import     | Avanti, imported         | to 3, once it failed only |
// | 5 | Emulators  | Avanti (skippable)       | none: the import stands   |
// | 6 | Appearance | Avanti                   | to 5                      |
// | 7 | World      | Entra, the world ready   | none: the world starts    |
//
// A resumed wizard (library.json, no catalog yet) opens on page 3. Clicks press what the keys press. Every change of what
// the panel shows increments revision().

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace wizard {
inline constexpr std::string_view kDocumentPath = "ui/wizard.rml";
inline constexpr std::string_view kDataModel = "wizard";
inline constexpr std::string_view kButtonEvent = "button";
inline constexpr std::string_view kStepEvent = "step";
inline constexpr std::string_view kFolderEvent = "folder";
inline constexpr std::string_view kPanelId = "wizard-panel";
inline constexpr std::string_view kBrowserPanelId = "folder-browser";
} // namespace wizard

enum class WizardPage : std::uint8_t { Welcome, Detection, Preview, Import, Emulators, Appearance, World };
inline constexpr std::size_t kWizardPageCount = 7;
/// "welcome", "detection", "preview", "import", "emulators", "appearance", "world".
[[nodiscard]] std::string_view toString(WizardPage page) noexcept;

/// A focusable element of a page.
enum class WizardButton : std::uint8_t {
    Next,
    Previous,
    Quit,
    Retry,
    Enter,
    FolderHome,
    FolderInstall,
    FolderRom,
    FolderMedia,
    Lighting,
    ReduceMotion,
};
/// "next", "previous", "quit", "retry", "enter", "folder-home", "folder-install", "folder-rom", "folder-media",
/// "lighting", "reduce-motion" (the button event's argument).
[[nodiscard]] std::string_view toString(WizardButton button) noexcept;
[[nodiscard]] std::optional<WizardButton> wizardButtonFromName(std::string_view name) noexcept;
/// The button's element id ("wz-next" ...).
[[nodiscard]] std::string_view elementIdOf(WizardButton button) noexcept;

/// The folders page 2 shows (and the browser changes).
enum class WizardFolder : std::uint8_t { Home, Install, Rom, Media };
inline constexpr std::size_t kWizardFolderCount = 4;
/// "home", "install", "rom", "media".
[[nodiscard]] std::string_view toString(WizardFolder folder) noexcept;

/// What a job of a page is doing.
enum class WizardWork : std::uint8_t { Idle, Running, Done, Failed };
/// "idle", "running", "done", "failed".
[[nodiscard]] std::string_view toString(WizardWork work) noexcept;

/// A folder as page 2 shows it.
struct WizardFolderRow {
    std::string path;
    /// "ok", "missing", "unreachable", "unreadable", "unknown" (not found; an optional install), "checking".
    std::string state = "checking";

    bool operator==(const WizardFolderRow&) const = default;
};

/// Page 2's content.
struct WizardDetection {
    std::array<WizardFolderRow, kWizardFolderCount> folders;
    /// library::DraftProblem's name: "none", or what stops the library ("home-unknown", ...).
    std::string problem = "none";
    /// A version of ES-DE, when known.
    std::string version;

    bool operator==(const WizardDetection&) const = default;
};

/// Page 3's content: `import --dry-run`'s reading (library::PreviewCounts' totals).
struct WizardPreview {
    std::size_t systems = 0;
    std::size_t systemsWithGames = 0;
    std::size_t games = 0;
    std::size_t media = 0;
    /// Share of the games with media, percent (nothing: unknown).
    std::optional<int> coveragePercent;
    std::size_t unrecognized = 0;
    std::size_t errors = 0;

    bool operator==(const WizardPreview&) const = default;
};

/// Page 4's content.
struct WizardImport {
    /// RefreshPhase's name while it runs ("" before its first progress).
    std::string phase;
    std::size_t done = 0;
    std::size_t total = 0;
    std::size_t entries = 0;
    std::size_t added = 0;

    bool operator==(const WizardImport&) const = default;
};

/// What the wizard asks of the flow.
enum class WizardCommandKind : std::uint8_t {
    Quit,      ///< Esci
    Detect,    ///< page 2 entered, or Riprova on it: discover and check the folders
    Browse,    ///< a folder of page 2 to change
    Read,      ///< page 3 entered, or Riprova on it: read the library
    Import,    ///< page 4 entered, or Riprova on it: library.json, the database, the first import
    Lighting,  ///< the lighting profile, a step (`direction`)
    ReduceMotion, ///< reduceMotion toggled
    StartWorld,   ///< page 7 entered: the building job
    Enter,        ///< Entra: the wizard ends, in the world
};
/// "quit", "detect", "browse", "read", "import", "lighting", "reduce-motion", "start-world", "enter".
[[nodiscard]] std::string_view toString(WizardCommandKind kind) noexcept;

struct WizardCommand {
    WizardCommandKind kind = WizardCommandKind::Quit;
    /// Browse's folder.
    WizardFolder folder = WizardFolder::Home;
    int direction = 1;

    bool operator==(const WizardCommand&) const = default;
};

class WizardModel {
public:
    /// Opens on Welcome, or (resumed) on Preview; the page's command comes from takeEntered().
    void open(bool resumed);
    void close() noexcept;

    /// The command of a page entered (the first page's included), once.
    [[nodiscard]] std::optional<WizardCommand> takeEntered() noexcept;

    /// A UI command while open, and the command for the flow, if any.
    [[nodiscard]] std::optional<WizardCommand> apply(UiCommand command);
    /// A click on `button` (shown): it is focused and pressed; `direction` for a choice's arrows.
    [[nodiscard]] std::optional<WizardCommand> click(WizardButton button, int direction = 1);

    // The flow's answers.
    void setDetection(WizardWork work, WizardDetection detection);
    void setPreview(WizardWork work, WizardPreview preview, std::string failure = {});
    void setImport(WizardWork work, WizardImport progress, std::string failure = {});
    void setAppearance(std::string lightingProfile, bool reduceMotion);
    void setWorld(WizardWork work);

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] bool resumed() const noexcept { return m_resumed; }
    [[nodiscard]] WizardPage page() const noexcept { return m_page; }
    [[nodiscard]] WizardWork detectionWork() const noexcept { return m_detectionWork; }
    [[nodiscard]] WizardWork previewWork() const noexcept { return m_previewWork; }
    [[nodiscard]] WizardWork importWork() const noexcept { return m_importWork; }
    [[nodiscard]] WizardWork worldWork() const noexcept { return m_worldWork; }
    [[nodiscard]] const WizardDetection& detection() const noexcept { return m_detection; }
    [[nodiscard]] const WizardPreview& preview() const noexcept { return m_preview; }
    [[nodiscard]] const WizardImport& importing() const noexcept { return m_import; }
    /// Why the preview or the import failed: a key of the document.
    [[nodiscard]] const std::string& failure() const noexcept { return m_failure; }
    [[nodiscard]] const std::string& lightingProfile() const noexcept { return m_lighting; }
    [[nodiscard]] bool reduceMotion() const noexcept { return m_reduceMotion; }
    /// The elements the page offers now, in focus order.
    [[nodiscard]] std::vector<WizardButton> buttons() const;
    [[nodiscard]] WizardButton focused() const noexcept { return m_focused; }
    [[nodiscard]] std::string focusedElement() const;
    /// Avanti (Entra on the last page) can be pressed.
    [[nodiscard]] bool canGoOn() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void show(WizardPage page);
    void settleFocus();
    void focus(WizardButton button) noexcept;
    [[nodiscard]] std::optional<WizardCommand> press(WizardButton button, int direction);

    bool m_open = false;
    bool m_resumed = false;
    WizardPage m_page = WizardPage::Welcome;
    WizardButton m_focused = WizardButton::Next;
    std::optional<WizardCommand> m_entered;
    WizardWork m_detectionWork = WizardWork::Idle;
    WizardWork m_previewWork = WizardWork::Idle;
    WizardWork m_importWork = WizardWork::Idle;
    WizardWork m_worldWork = WizardWork::Idle;
    WizardDetection m_detection;
    WizardPreview m_preview;
    WizardImport m_import;
    std::string m_failure;
    std::string m_lighting;
    bool m_reduceMotion = false;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
