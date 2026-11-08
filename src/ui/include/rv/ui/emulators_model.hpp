#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The emulators screen (ADR-0018 §5; assets/ui/emulators.rml through ui::EmulatorsScreen): the systems with games, their
// readiness, what launches them and why, whether it was verified, the first blocker; a system opens every ES-DE command
// and Retroverse profile that serves it, the ready ones first; one of them chosen waits for its launch test, which only
// then stores it, or "Salva senza provare". The model holds what shows and the focus; everything else happens elsewhere
// (the catalog worker through the application): a command asks for it, the answers come back through the setters.
//
// | View    | Up, Down, pages     | Left, Right        | Activate                                 | Back          |
// |---------|---------------------|--------------------|------------------------------------------|---------------|
// | Systems | the systems         |                    | opens the system                         | closes        |
// | System  | the options         |                    | an option: Choice; "ES-DE": clears       | to Systems    |
// | Choice  | the buttons         | the game (Partita) | Prova, Programma, Salva, Annulla         | to System     |
// | Testing |                     |                    |                                          |               |
//
// Clicks press what the keys press. Every change of what the screen shows increments revision().
//
// For one title (I7 step 6, ADR-0018 §5, §11: Informazioni's Emulatore): openForGame() shows the title's system's options
// at once; a test runs that title, what is chosen is stored for it alone, and Back from the System view closes the screen.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace emulators {
inline constexpr std::string_view kDocumentPath = "ui/emulators.rml";
inline constexpr std::string_view kDataModel = "emulators";
inline constexpr std::string_view kPanelId = "emulators-panel";
inline constexpr std::string_view kBrowserPanelId = "folder-browser";
/// How many games a launch test offers (the first is the sample: ADR-0018 §5).
inline constexpr std::size_t kMaxSampleGames = 200;
/// The rows Page Up and Page Down move.
inline constexpr std::size_t kPageRows = 8;
} // namespace emulators

/// Where the table stands.
enum class EmulatorsState : std::uint8_t { Loading, Listed, Unavailable };
/// "loading", "listed", "unavailable".
[[nodiscard]] std::string_view toString(EmulatorsState state) noexcept;

enum class EmulatorsView : std::uint8_t { Systems, System, Choice, Testing };
/// "systems", "system", "choice", "testing".
[[nodiscard]] std::string_view toString(EmulatorsView view) noexcept;

/// A command or profile of a system.
struct EmulatorOption {
    /// A Retroverse profile (its id in `value`), else an ES-DE command (its label, maybe empty).
    bool profile = false;
    std::string value;
    /// What shows: the label, the profile's display name; empty: the system's one unlabelled command.
    std::string name;
    /// catalog::toString(LaunchReadiness), and the first blocker's code ("" when ready).
    std::string readiness = "ready";
    std::string blocker;
    /// The %EMULATOR_X% it runs, when it is one: a program can be chosen for it.
    std::string emulator;
    /// The program found (for people), and whether it is one chosen in Retroverse.
    std::string program;
    bool programChosen = false;
    /// It is what the system launches with now.
    bool chosen = false;
    /// A launch returned normally with it and this program.
    bool verified = false;

    bool operator==(const EmulatorOption&) const = default;
};

/// A system with games.
struct EmulatorSystem {
    std::string name;
    std::string fullName;
    /// I11 step 3: ES-DE's <fullname>, shown beside fullName when it differs (fullName is the identity's).
    std::string esdeName;
    std::size_t games = 0;
    /// What launches it now: its readiness, the first blocker's code, where the choice comes from
    /// (catalog::toString(ChoiceSource)), its name as an option shows it, whether it was verified.
    std::string readiness = "ready";
    std::string blocker;
    std::string source;
    std::string choice;
    bool verified = false;
    /// A choice of Retroverse's is stored for it (the catalog database's): "ES-DE" takes it away.
    bool stored = false;
    /// The ready options first (library::suggestedOptions()); empty until the system is opened.
    std::vector<EmulatorOption> options;

    bool operator==(const EmulatorSystem&) const = default;
};

/// A game a launch test can run.
struct EmulatorSample {
    std::string gameId;
    std::string title;

    bool operator==(const EmulatorSample&) const = default;
};

enum class EmulatorsButton : std::uint8_t { Sample, Test, Program, ClearProgram, Save, Cancel };
/// "sample", "test", "program", "clear-program", "save", "cancel" (the button event's argument).
[[nodiscard]] std::string_view toString(EmulatorsButton button) noexcept;
[[nodiscard]] std::optional<EmulatorsButton> emulatorsButtonFromName(std::string_view name) noexcept;
/// The button's element id ("em-sample" ...).
[[nodiscard]] std::string_view elementIdOf(EmulatorsButton button) noexcept;

/// What the screen asks of the application.
enum class EmulatorsCommandKind : std::uint8_t {
    Close,        ///< it closed itself
    Load,         ///< the table, again
    Samples,      ///< the games of `system`
    Test,         ///< a launch test of the choice with `game`: stored once it returned normally (`forGame`: for it alone)
    Save,         ///< "Salva senza provare": the choice stored as it is (`forGame`: for `game` alone)
    Clear,        ///< the system's stored choice removed: ES-DE's decides again (`forGame`: the title's)
    Browse,       ///< a program for `emulator`, from the folder browser
    ClearProgram, ///< the program chosen for `emulator` removed
};
/// "close", "load", "samples", "test", "save", "clear", "browse", "clear-program".
[[nodiscard]] std::string_view toString(EmulatorsCommandKind kind) noexcept;

struct EmulatorsCommand {
    EmulatorsCommandKind kind = EmulatorsCommandKind::Close;
    std::string system;
    bool profile = false;
    std::string value;
    std::string game;
    std::string gameTitle;
    std::string emulator;
    /// The screen stands for one title: the choice is `game`'s, not `system`'s.
    bool forGame = false;

    bool operator==(const EmulatorsCommand&) const = default;
};

/// A click (the ray's too).
struct EmulatorsClick {
    enum class Kind : std::uint8_t { System, Option, Clear, Button, Sample };
    Kind kind = Kind::System;
    /// The row's place (System, Option), the sample's step (Sample: -1, 1).
    int index = 0;
    EmulatorsButton button = EmulatorsButton::Cancel;

    bool operator==(const EmulatorsClick&) const = default;
};

class EmulatorsModel {
public:
    /// Opens on the systems, asking for the table (takeRequest()); `resumedFocus` keeps the system focused last.
    void open();
    void close() noexcept;
    /// Opens for the title `gameId` of `system`: on that system's options once the table came. `own`: what is stored for
    /// the title (the option "in uso"; "Torna alla scelta del sistema" takes it away).
    void openForGame(std::string system, std::string gameId, std::string title,
                     std::optional<std::pair<bool, std::string>> own);
    /// It stands for one title; its GameId and title.
    [[nodiscard]] bool forGame() const noexcept { return !m_game.empty(); }
    [[nodiscard]] const std::string& gameId() const noexcept { return m_game; }
    [[nodiscard]] const std::string& gameTitle() const noexcept { return m_gameTitle; }

    /// The table is wanted (the wizard's page 5 shows its counts before the screen ever opens).
    void prepare();

    /// What the application must do next, once: what a view entered asks.
    [[nodiscard]] std::optional<EmulatorsCommand> takeRequest() noexcept;

    /// A UI command while open, and the command for the application, if any.
    [[nodiscard]] std::optional<EmulatorsCommand> apply(UiCommand command);
    [[nodiscard]] std::optional<EmulatorsCommand> click(const EmulatorsClick& click);

    // The application's answers.
    void setTable(std::vector<EmulatorSystem> systems);
    /// No table can be had: no catalog of ES-DE's, or it cannot be read.
    void setUnavailable();
    /// What a system opened asked for: its options (the list holds none: translating them all takes seconds), its games.
    void setOptions(const std::string& system, std::vector<EmulatorOption> options);
    void setSamples(const std::string& system, std::vector<EmulatorSample> samples);
    /// A launch test began: Testing shows, until testEnded().
    void testStarted();
    /// How a launch test ended: "verified" goes back to the system (the table asked again); any other key of the document
    /// stays on the choice, not stored, saying so.
    void testEnded(std::string_view verdict);
    /// A change the application made ended: "" went well (the table asked again; `toSystem`: back to the system), else a
    /// key of the document.
    void changeEnded(std::string_view problem, bool toSystem);

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] EmulatorsState state() const noexcept { return m_state; }
    [[nodiscard]] EmulatorsView view() const noexcept { return m_view; }
    [[nodiscard]] const std::vector<EmulatorSystem>& systems() const noexcept { return m_systems; }
    /// The system the focus stands on (Systems) or that shows; null when none.
    [[nodiscard]] const EmulatorSystem* system() const noexcept;
    /// The option a choice stands on (Choice, Testing); null when none.
    [[nodiscard]] const EmulatorOption* option() const noexcept;
    [[nodiscard]] std::size_t focusedSystem() const noexcept { return m_system; }
    [[nodiscard]] std::size_t focusedOption() const noexcept { return m_option; }
    /// The rows of the System view: the options, then "ES-DE" when a stored choice can go.
    [[nodiscard]] std::size_t optionRows() const noexcept;
    [[nodiscard]] bool offersClear() const noexcept;
    /// The buttons of the Choice view, in focus order.
    [[nodiscard]] std::vector<EmulatorsButton> buttons() const;
    [[nodiscard]] EmulatorsButton focusedButton() const noexcept { return m_button; }
    [[nodiscard]] const std::vector<EmulatorSample>& samples() const noexcept { return m_samples; }
    [[nodiscard]] std::size_t sample() const noexcept { return m_sample; }
    /// The samples of the system were answered.
    [[nodiscard]] bool samplesKnown() const noexcept { return m_samplesKnown; }
    /// How the last test ended ("" none, "running"), or what a change met ("" none).
    [[nodiscard]] const std::string& verdict() const noexcept { return m_verdict; }
    [[nodiscard]] const std::string& problem() const noexcept { return m_problem; }
    /// A choice was stored without its test since the system opened.
    [[nodiscard]] bool savedUntested() const noexcept { return m_saved; }
    /// How many systems launch as they are, and how many do not.
    [[nodiscard]] std::size_t readySystems() const noexcept;
    [[nodiscard]] std::size_t blockedSystems() const noexcept;
    /// The element the focus stands on.
    [[nodiscard]] std::string focusedElement() const;
    [[nodiscard]] static std::string systemElement(std::size_t index);
    [[nodiscard]] static std::string optionElement(std::size_t index);
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void show(EmulatorsView view);
    void moveRow(std::ptrdiff_t delta);
    void stepSample(int direction);
    void chooseOption(std::size_t index);
    [[nodiscard]] std::optional<EmulatorsCommand> press(EmulatorsButton button);
    [[nodiscard]] std::optional<EmulatorsCommand> enter();
    [[nodiscard]] EmulatorsCommand commandOf(EmulatorsCommandKind kind) const;
    void settleButton();
    /// What the choice stands on as it is now: a program chosen makes an option ready.
    void syncChosen();
    /// A title's "in uso" is its own choice, or what the system has when it has none.
    void markOwn();

    bool m_open = false;
    EmulatorsState m_state = EmulatorsState::Loading;
    EmulatorsView m_view = EmulatorsView::Systems;
    std::vector<EmulatorSystem> m_systems;
    std::size_t m_system = 0;
    std::size_t m_option = 0;
    /// What a choice stands on: the option's identity, since a table can come back in another order.
    std::optional<EmulatorOption> m_chosen;
    EmulatorsButton m_button = EmulatorsButton::Cancel;
    std::vector<EmulatorSample> m_samples;
    std::size_t m_sample = 0;
    bool m_samplesKnown = false;
    std::string m_verdict;
    std::string m_problem;
    bool m_saved = false;
    /// For one title: its system, GameId, title, and what is stored for it (profile, value).
    std::string m_gameSystem;
    std::string m_game;
    std::string m_gameTitle;
    std::optional<std::pair<bool, std::string>> m_own;
    std::optional<EmulatorsCommand> m_request;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
