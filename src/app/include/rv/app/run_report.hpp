#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/app/self_check.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/frame_stats.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::app {

/// Version of the run report format. parseRunReport() refuses other versions; fields are only ever added within one.
inline constexpr int kRunReportSchemaVersion = 1;

/// The OpenGL context a run rendered with, as the driver names it (gl::GLCapabilities).
struct RunReportContext {
    std::string vendor;      ///< GL_VENDOR
    std::string renderer;    ///< GL_RENDERER, e.g. "NVIDIA GeForce RTX 2080 SUPER/PCIe/SSE2"
    std::string version;     ///< GL_VERSION, which names the driver too, e.g. "4.5.0 NVIDIA 617.42"
    std::string glslVersion; ///< GL_SHADING_LANGUAGE_VERSION

    bool operator==(const RunReportContext&) const = default;
};

/// OpenGL debug messages a run received, by severity (gl::GLDevice::debugMessageCount: repeats the log suppressed
/// included, notifications and known informational ids excluded), those --gl-selftest provoked included
/// (RunReport::glSelfTest).
struct DebugMessageCounts {
    std::uint64_t high = 0;
    std::uint64_t medium = 0;
    std::uint64_t low = 0;

    bool operator==(const DebugMessageCounts&) const = default;
};

/// The render targets a screenshot was drawn through (ScreenshotRecord::targets, docs/i5/memory-margin.md).
inline constexpr std::string_view kScreenshotWindowTargets = "window"; ///< the window's: the screenshot has its size
inline constexpr std::string_view kScreenshotOwnTargets = "own";       ///< a set made for the screenshot alone

/// The --screenshot a run wrote.
struct ScreenshotRecord {
    std::filesystem::path file; ///< as given on the command line
    int width = 0;
    int height = 0;
    /// kScreenshotWindowTargets or kScreenshotOwnTargets.
    std::string targets;

    bool operator==(const ScreenshotRecord&) const = default;
};

/// The window of a run: its drawable size in pixels when the run ended (with display scaling, the size the window
/// was asked for times the display's scale: what the window's render targets are allocated for).
struct WindowRecord {
    int drawableWidth = 0;
    int drawableHeight = 0;

    bool operator==(const WindowRecord&) const = default;
};

/// The outcome of a --self-check.
struct SelfCheckRecord {
    SelfCheck check = SelfCheck::Srgb;
    bool passed = false;
    /// The measurements as logged, or why the check could not run.
    std::string summary;

    bool operator==(const SelfCheckRecord&) const = default;
};

/// What the quick catalog and the walker showed at one moment of a launch (LaunchRecord::before, ::after; I2 step 13:
/// a launch and its return leave them as they were).
struct CatalogView {
    /// The selected game's id and position; empty and none while nothing is selected.
    std::string selectedId;
    std::optional<std::size_t> selectedPosition;
    /// The selected game's launch readiness and first blocker as its card showed them (ui::CatalogEntry::readiness,
    /// ::blocker; empty for the mock catalog's games and a ready game's blocker).
    std::string readiness;
    std::string blocker;
    /// The search text and the active filter's key (empty: every game; else the ES-DE system's name, or the mock's
    /// platform).
    std::string search;
    std::string filter;
    /// The walker's pose as --spawn takes it.
    std::optional<SpawnPoint> camera;

    bool operator==(const CatalogView&) const = default;
};

/// Durations in milliseconds: how many, their percentiles (nearest rank: core::summarizeDurations) and the longest
/// (CatalogRecord::pages, ::frames; FrameTimesRecord, PhaseRecord, CpuSectionRecord).
using TimeSummary = core::DurationPercentiles;

/// The summary of `milliseconds` (sorted in place); nothing for none.
[[nodiscard]] std::optional<TimeSummary> summarizeTimes(std::vector<double>& milliseconds);

/// One refresh of the catalog from ES-DE during the run (I3 step 13r: "Aggiorna libreria", --refresh-library): the
/// import on the catalog worker, in slices.
struct RefreshRecord {
    /// What started it: "command-line" (--refresh-library), "user" ("Aggiorna libreria").
    std::string trigger;
    /// How it ended (app::toString(RefreshOutcome): "done", "cancelled", "busy", "failed"), "running" when the run
    /// ended before the worker answered, "unavailable" for a --refresh-library the catalog could not take (not open,
    /// the mock catalog: nothing ran); its error.
    std::string outcome;
    std::string error;
    /// The frame whose update asked for it, and the frame whose update learned its end.
    std::uint64_t requestFrame = 0;
    std::optional<std::uint64_t> endFrame;
    /// On the worker, from the job's start to its result; absent when it did not answer.
    std::optional<double> ms;
    /// The snapshot's entries and what the import changed (app::RefreshSummary).
    std::size_t entries = 0;
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t changed = 0;
    std::size_t moved = 0;
    std::size_t reviews = 0;
    std::size_t guarded = 0;
    /// The slices (app::RefreshSlicing): boundaries, requests answered at them, the longest stretch without one.
    std::uint64_t slices = 0;
    std::uint64_t served = 0;
    std::optional<double> longestSliceMs;
    /// The lists the quick catalog asked for while it ran, from the request to the answer on the worker
    /// (CatalogList::waitedMs: lists answered within 100 ms during an import); absent when none was asked.
    std::optional<TimeSummary> lists;
    /// The run ended while it ran: how long the catalog worker took to stop (the import cancelled, its transaction
    /// rolled back), in milliseconds.
    std::optional<double> stopMs;
    /// The quick catalog and the walker when it was asked for, and once its list ran again after the commit (or when
    /// it ended otherwise); absent when the run ended first.
    std::optional<CatalogView> before;
    std::optional<CatalogView> after;

    bool operator==(const RefreshRecord&) const = default;
};

/// The quick catalog of a run of retroverse.exe: absent only in runs without the product UI (the model viewer).
struct CatalogRecord {
    /// Where its games come from: "esde" (the catalog database of the user data directory, I2 step 13), "mock"
    /// (--mock-catalog).
    std::string source;
    /// The --mock-catalog file, as given; for "esde", the catalog database.
    std::filesystem::path file;
    /// Games the source lists (the whole catalog's default list: the available games).
    std::size_t games = 0;
    /// Times the catalog was opened, and whether it was open when the run ended.
    std::uint64_t opened = 0;
    bool open = false;
    /// The selected game when the run ended: its position in the list and its id; none while the list is empty.
    std::optional<std::size_t> selectedPosition;
    std::string selectedId;
    /// "esde": what the list says instead of games (ui::quick_catalog::kNotice*: "no-library", "no-database",
    /// "data-directory", "library-mismatch", "unavailable", "opening" when the run ended before the catalog opened);
    /// empty when the catalog opened.
    std::string notice;
    /// "esde": the library (library.json's id) and the systems with games the chips show.
    std::string library;
    std::size_t systems = 0;
    /// The search text and the active filter's key when the run ended.
    std::string search;
    std::string filter;
    /// Games of the list the model held when the run ended (the search and the filter applied), and the count the
    /// screen showed (ui::QuickCatalogScreen::shownCount; absent while the catalog was not open): equal once the list a
    /// query brought from the catalog's worker is shown.
    std::size_t listed = 0;
    std::optional<std::size_t> shownCount;
    /// "esde", milliseconds from the process's creation (NFR-01): the catalog opened on the worker (the database, the
    /// library's configuration, the systems' readiness), its first list materialized, and the first frame that showed
    /// a page of it (the quick catalog open with rows of the catalog bound); absent when it did not happen.
    std::optional<double> openedMs;
    std::optional<double> firstListMs;
    std::optional<double> firstPageMs;
    /// "esde": the page fetches on the main thread (ui::CatalogSource::rows: ADR-0008 §5's 5 ms budget); absent when
    /// there was none.
    std::optional<TimeSummary> pages;
    /// The frames rendered while the catalog showed games (open, its list not empty): their wall-clock times (the
    /// measured frame time, so not with --fixed-dt, which replaces it); absent when there was none.
    std::optional<TimeSummary> frames;
    /// "esde": every list the worker answered, from the request to the answer (CatalogList::waitedMs: the time in its
    /// queue, behind a refresh's slice or the room's snapshot, and the materialization; I3 step 10b); absent when there
    /// was none.
    std::optional<TimeSummary> lists;
    /// "esde": play sessions closed as interrupted when the catalog opened (their owner was gone: ADR-0008 §9).
    std::size_t interruptedSessions = 0;
    /// The card's blocks cut by the layout when the run ended (ui::QuickCatalogScreen::squeezedCardBlocks: a line or a
    /// row shows only in part); empty when the card showed its text whole, or was not shown.
    std::vector<std::string> cardSqueezed;
    /// The refreshes of the run, in order (I3 step 13r).
    std::vector<RefreshRecord> refreshes;

    bool operator==(const CatalogRecord&) const = default;
};

/// One launch from the quick catalog (LaunchFlow fills it): what was asked, what the session did, how the host waited.
struct LaunchRecord {
    std::string gameId;
    /// The emulator profile's id.
    std::string profile;
    /// The session's final state (launcher::toString(SessionState): "Returned", "Crashed", "FailedToStart"; a run that
    /// ends while the game runs ends its session too, as Returned with quitDuringGame), "Blocked" for a launch the plan
    /// refused (nothing started), "Pending" when the run ended before it could start.
    std::string state;
    /// The outcome as the product UI tells it (LaunchMessage: launch_outcome::k*, its reason and detail).
    std::string outcome;
    std::string reason;
    std::string detail;
    /// The authoritative exit code, when something exited.
    std::optional<std::uint32_t> exitCode;
    /// The wait strategy in effect (launcher::toString(WaitStrategy)); empty when nothing started.
    std::string strategy;
    /// The started process's id (0 when nothing started).
    std::uint32_t processId = 0;
    /// launcher::LaunchOutcome's flags; confirmedByUser only for the user's own confirmation (false when quitting or the
    /// wait limit ended the session, which LaunchSession::confirmReturn records as confirmed).
    bool quickExit = false;
    bool processesRemained = false;
    bool confirmedByUser = false;
    bool wasStillRunning = false;
    /// The run ended (its window closed, a quit request) while the emulator ran: it was left running.
    bool quitDuringGame = false;
    /// A frame-limited run stopped waiting for the game after its --launch-wait-limit (LaunchFlow::setWaitLimit): the
    /// session ended as left running.
    bool waitLimitReached = false;
    /// The frame whose input asked for the launch; the frame index when the emulator started and when its session
    /// ended (the next frame to render: every frame before it had been presented). Equal start and end frames: no
    /// frame was rendered while the game ran.
    std::uint64_t requestFrame = 0;
    std::optional<std::uint64_t> startFrame;
    std::optional<std::uint64_t> endFrame;
    /// Frames rendered during the session: for a question of the game-session panel (StillRunning, a confirmation, the
    /// user back in Retroverse), and while the game had the screen: none (the host waits instead), but the frame after a
    /// choice to go back to the game (it shows the "Gioco in corso" notice, which the window keeps behind it).
    std::uint64_t framesWithPanel = 0;
    std::uint64_t framesWhileRunning = 0;
    /// Times the host woke from its wait while the session ran (the runner's wake events, other events, the polling
    /// interval).
    std::uint64_t idleWakes = 0;
    /// Milliseconds: from the start to the process running (the window's hand-over included), the process's run (to
    /// the authoritative exit, or to the end of the session when nothing exited), and from that exit to the end of the
    /// session.
    std::optional<double> startMs;
    std::optional<double> runMs;
    std::optional<double> returnMs;
    /// Milliseconds from that exit (the end of the session when nothing exited) to Retroverse's window back in the
    /// foreground (platform::WindowHandover); absent for hidden windows, which have no hand-over, and while it is not.
    std::optional<double> foregroundMs;
    /// The panels the launch showed, in order (toString(SessionPanel): "still-running", "confirm", "playing",
    /// "outcome").
    std::vector<std::string> panels;
    /// The quick catalog and the walker when Avvia asked for the launch, and on the first frame after its end (the
    /// return, or the refusal); absent when the run ended first.
    std::optional<CatalogView> before;
    std::optional<CatalogView> after;
    /// "esde" catalogs: the play session recorded for the launch (catalog_db::SessionStore, on the catalog worker),
    /// absent while it was not; the error that kept it from being recorded; a launch verification recorded with it
    /// (a normal return, ADR-0008 §8).
    std::optional<std::int64_t> sessionId;
    std::string sessionError;
    bool verified = false;

    bool operator==(const LaunchRecord&) const = default;
};

// The sections of I3, declared by I3 step 0a and empty until the step that fills each: every report holds them (an
// empty object, or an empty array of phases), so that a reader can tell a run that had nothing to report from one that
// did not know the section.

/// One change of the world's interaction (I3 step 13a), on the frame it happened.
struct InteractionEventRecord {
    std::uint64_t frame = 0;
    /// "hover" (the hovered item changed; pickId "" for none), "pull-out" (an item starts to leave its slot: its
    /// static records hidden, a dynamic copy drawn), "pulled" (fully out), "in" (back in its slot, drawn by its static
    /// records again), "state" (the interaction's state changed: `state` is the new one), "focus" (the focused item
    /// changed in Focus: a D-pad step or a look), "step" (a D-pad step moved the focus).
    std::string event;
    std::string pickId;
    std::string state;

    bool operator==(const InteractionEventRecord&) const = default;
};

/// The inspection (I3 step 13b; ADR-0012 §4): the item lifted into the hand, turned and zoomed, the scene behind it
/// dimmed. Under InteractionRecord::inspect.
struct InspectRecord {
    /// Inspections entered; frames drawn with the item on its way into or out of the hand (0 < lift < 1) and held
    /// (lift 1); frames drawn with the scene behind it dimmed.
    std::uint64_t inspections = 0;
    std::uint64_t liftFrames = 0;
    std::uint64_t heldFrames = 0;
    std::uint64_t dimmedFrames = 0;
    /// Vedi media and Informazioni asked for in Inspect (I3 step 14b opens them), and the quick catalog opened while
    /// in Inspect: never (the pad's north button is Vedi media there).
    std::uint64_t mediaRequests = 0;
    std::uint64_t infoRequests = 0;
    std::uint64_t catalogOpenedInInspect = 0;
    /// Over the frames drawn held: the zoom's distance (the interaction's, 0.4 to 1.0 m) and the distance drawn
    /// (heldDistance(): kept out of its fixture), at their nearest and farthest (meters); nothing before any.
    std::optional<double> minDistance;
    std::optional<double> maxDistance;
    std::optional<double> minDrawnDistance;
    std::optional<double> maxDrawnDistance;
    /// The last frame drawn with the item off its pulled-out pose (lift above 0): the item, the lift, the turn in
    /// degrees, the zoom's distance and the distance drawn; the held item's centre, its front (its prefab's +Z) and up
    /// (+Y) in the camera's frame (x right, y up, z towards the viewer); whether its front showed the hero texture.
    std::string pickId;
    double lift = 0.0;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
    double distance = 0.0;
    double drawnDistance = 0.0;
    glm::vec3 heldCenter{0.0f};
    glm::vec3 heldFront{0.0f};
    glm::vec3 heldUp{0.0f};
    bool heroFront = false;

    bool operator==(const InspectRecord&) const = default;
};

/// The world's interaction (I3 step 13a; ADR-0012 §1-§3): the hover with its outline and label, the pull-out on the
/// dynamic path, Focus and its D-pad steps; the inspection (I3 step 13b). Empty ({}) in runs without a room's
/// collection.
struct InteractionRecord {
    bool bound = false;
    /// The world's reduceMotion (world.json) as the run had it.
    bool reduceMotion = false;
    /// At the end: app::toString(WorldState), the items hovered, focused and highlighted (pick ids in hexadecimal, ""
    /// none), the focused item's GameId, the items off their static pose.
    std::string state;
    std::string hoveredPickId;
    std::string focusedPickId;
    std::string highlightedPickId;
    std::string focusedGame;
    std::vector<std::string> pulled;
    /// Counts over the run: hover changes, focus changes in Focus and the D-pad steps among them, pull-outs started.
    std::uint64_t hoverChanges = 0;
    std::uint64_t focusChanges = 0;
    std::uint64_t steps = 0;
    std::uint64_t pullOuts = 0;
    /// Frames with an item off its static pose, the most such items in one frame, and the frames in which an item
    /// off its pose still had a static record drawn (not flagged hidden) or a hidden record had no dynamic copy: 0.
    std::uint64_t pulledFrames = 0;
    std::uint64_t maxPulled = 0;
    std::uint64_t staticDrawnWhilePulledFrames = 0;
    /// Frames during which an animation of the interaction ran (the "animation" phase), the walker's eases among them.
    std::uint64_t animationFrames = 0;
    std::uint64_t walkerEaseFrames = 0;
    /// Frames whose pick ray came from the cursor (the mouse free: the developer overlay open), and those that showed
    /// the hover's title label; the label's title at the end ("" none).
    std::uint64_t cursorFrames = 0;
    std::uint64_t labelFrames = 0;
    std::string label;
    /// The changes, in order (at most app::kMaxInteractionEvents; `eventsDropped` past them).
    std::vector<InteractionEventRecord> events;
    std::uint64_t eventsDropped = 0;
    InspectRecord inspect;

    bool operator==(const InteractionRecord&) const = default;
};

/// The most interaction events a run report keeps.
inline constexpr std::size_t kMaxInteractionEvents = 512;

/// A change of the world's card (I3 step 14a).
struct CardEventRecord {
    std::uint64_t frame = 0;
    /// "shown" (the card shows `game`), "hidden", "focus" (the focus moved to `button`), "press" (`button` pressed
    /// `via` "activate": Enter or the pad's south button on the focused one, or "click"), and what the interaction
    /// answered to the card or its shortcuts: "launch", "media", "info".
    std::string event;
    std::string button;
    std::string via;
    std::string game;

    bool operator==(const CardEventRecord&) const = default;
};

/// The world's card beside the inspected item (I3 step 14a; ADR-0012 §5). Empty ({}) in runs without a room's
/// collection.
struct CardRecord {
    bool bound = false;
    /// Frames it showed, with its cover's texture, and with its content cut by its maximum height (0: never squeezed).
    std::uint64_t shownFrames = 0;
    std::uint64_t coverFrames = 0;
    std::uint64_t squeezedFrames = 0;
    /// What it showed last: the game (GameId), its title, platform, year (0 unknown), readiness and blocker keys, the
    /// catalog's details known, the other versions, the focused button (ui::toString(CardButton); "" while hidden).
    std::string game;
    std::string title;
    std::string platform;
    std::uint64_t year = 0;
    std::string readiness;
    std::string blocker;
    bool details = false;
    std::uint64_t otherVersions = 0;
    std::string focused;
    /// The cover's box as last laid out with its texture: x, y, width, height in pixels of the window's view (empty:
    /// none).
    std::vector<double> coverBox;
    /// The changes, in order (at most kMaxInteractionEvents; `eventsDropped` past them).
    std::vector<CardEventRecord> events;
    std::uint64_t eventsDropped = 0;

    bool operator==(const CardRecord&) const = default;
};
/// An event of the launches from the room and of "Mostra nella stanza" (I3 step 15a; ADR-0012 §6, §8).
struct RoomLaunchEventRecord {
    std::uint64_t frame = 0;
    /// "launch" (Avvia asked: game, pickId, walker), "refused" (the product UI took no launch now: outcome, reason),
    /// "ended" (the launch's outcome, reason), "restored" (the frame the interaction came back: state, pickId, walker),
    /// "show-in-room" (game, pickId), "show-in-room-failed" (game; reason: the card's room key, "item-unavailable" or
    /// "state").
    std::string event;
    std::string game;
    std::string pickId;
    std::string outcome;
    std::string reason;
    std::string state;
    std::optional<SpawnPoint> walker;

    bool operator==(const RoomLaunchEventRecord&) const = default;
};

/// The launches from the room and "Mostra nella stanza" (I3 step 15a). Empty ({}) in runs that never showed a room's
/// plan.
struct RoomLaunchRecord {
    bool bound = false;
    /// Avvia asked, launches that ended returned (launch_outcome::kReturned) and otherwise (refused, failed, ...);
    /// "Mostra nella stanza" taken to an item and refused.
    std::uint64_t launches = 0;
    std::uint64_t returned = 0;
    std::uint64_t notReturned = 0;
    std::uint64_t showInRoom = 0;
    std::uint64_t showInRoomFailed = 0;
    /// The last launch: the walker when Avvia asked for it and once the interaction came back, and whether they are
    /// equal (exactly: the restore places the recorded pose).
    std::optional<SpawnPoint> walkerBefore;
    std::optional<SpawnPoint> walkerAfter;
    bool poseRestored = false;
    /// The quick catalog's card asked where games stand this many times; the last game asked and its room key
    /// (ui::quick_catalog's kRoomPlaced or why not).
    std::uint64_t placementQueries = 0;
    std::string placementGame;
    std::string placement;
    /// In order, at most kMaxInteractionEvents (`eventsDropped` past them).
    std::vector<RoomLaunchEventRecord> events;
    std::uint64_t eventsDropped = 0;

    bool operator==(const RoomLaunchRecord&) const = default;
};


/// A change of the media viewer or of Informazioni (I3 step 14b).
struct GameScreenEventRecord {
    std::uint64_t frame = 0;
    /// "media" (the viewer opened on `game` from `from`, "card" or "catalog"; `detail` its pages' kinds joined by ','),
    /// "page" (the page of `kind` shows; `detail` its position from 1), "picture" (`kind`'s picture: `detail`
    /// "shown <width>x<height>" or "failed"), "info" (Informazioni opened on `game` from `from`; `detail` its other
    /// versions, "<title> (<region>)" joined by "; "), "scroll" (`detail` the step), "closed" (`detail` "media" or
    /// "info").
    std::string event;
    std::string game;
    std::string from;
    std::string kind;
    std::string detail;

    bool operator==(const GameScreenEventRecord&) const = default;
};

/// The media viewer and Informazioni (I3 step 14b; ADR-0012 §5), from the world's card and the quick catalog. Empty
/// ({}) in runs without the product UI.
struct GameScreensRecord {
    bool bound = false;
    /// Frames each showed; pictures uploaded and pictures that could not be read or decoded.
    std::uint64_t viewerFrames = 0;
    std::uint64_t infoFrames = 0;
    std::uint64_t pictures = 0;
    std::uint64_t failures = 0;
    /// The viewer's picture and stage as last laid out with a picture: x, y, width, height in pixels of the window's
    /// view (empty: none).
    std::vector<double> pictureBox;
    std::vector<double> stageBox;
    /// The changes, in order (at most kMaxInteractionEvents; `eventsDropped` past them).
    std::vector<GameScreenEventRecord> events;
    std::uint64_t eventsDropped = 0;

    bool operator==(const GameScreensRecord&) const = default;
};

/// A game of the directory's favourites and recent games as the run listed it last (I4 step 7b).
struct DirectoryGameRecord {
    std::string gameId;
    std::string title;
    std::string system;
    bool favorite = false;
    /// The local date it was last played, "YYYY-MM-DD"; "" for a favourite never played.
    std::string played;

    bool operator==(const DirectoryGameRecord&) const = default;
};

/// A change of the directory (I4 step 7b).
struct DirectoryEventRecord {
    std::uint64_t frame = 0;
    /// "open" (`id` the tab, `detail` "elevator" or "map"), "command" (`id` ui::toString(DirectoryCommandKind), `detail`
    /// its floor, room or game), "refused" (`id` the notice, `detail` the command refused), "games" (`id` the list's
    /// state, `detail` its count), "card" (`id` the game, `detail` its room key), "launch-ended" (`id` the game, `detail`
    /// the outcome's code), "closed".
    std::string event;
    std::string id;
    std::string detail;

    bool operator==(const DirectoryEventRecord&) const = default;
};

/// The directory (I4 step 7b, ADR-0013 §10): how often it opened, what it showed at the end, its favourites and recent
/// games as listed last, its card, its changes. Empty ({}) in runs without the generated building.
/// A range of a room of a chain as the directory shows it (I4b step 8): its section's system and the initials it runs
/// between ("A", "Mo").
struct DirectoryRangeRecord {
    std::string system;
    std::string first;
    std::string last;

    bool operator==(const DirectoryRangeRecord&) const = default;
};

/// A room of a chain as the directory's "Piani" tab lists it (I4b step 8): its id, its titles' ranges (none until they
/// are known) and whether they interleave with another room's ("in ordine sparso").
struct DirectoryChainRoomRecord {
    std::string room;
    std::vector<DirectoryRangeRecord> ranges;
    bool scattered = false;

    bool operator==(const DirectoryChainRoomRecord&) const = default;
};

struct DirectoryRecord {
    bool bound = false;
    std::uint64_t opened = 0;
    /// At the end: open, its tab and view (ui::toString), its notice, the games list's state.
    bool open = false;
    std::string tab;
    std::string view;
    std::string notice;
    std::string gamesState;
    /// Times the games list was asked of the catalog, and the list as it was last listed.
    std::uint64_t gamesRequests = 0;
    std::vector<DirectoryGameRecord> games;
    /// The card last shown: its game, its room key, the outcome of its last launch ("" none).
    std::string cardGame;
    std::string cardRoom;
    std::string cardOutcome;
    /// In order, at most kMaxInteractionEvents (`eventsDropped` past them).
    std::vector<DirectoryEventRecord> events;
    std::uint64_t eventsDropped = 0;
    /// I4b step 8: the rooms of chains as the building given to the directory last listed them, floor by floor.
    std::vector<DirectoryChainRoomRecord> chainRooms;

    bool operator==(const DirectoryRecord&) const = default;
};

/// The persisted start (I3 step 10c): the stored plan's room shown before the catalog's snapshot answered.
struct WorldPreviewRecord {
    /// The frame it first showed in, when it was committed (from the process's creation), its job on the pool and its
    /// commit on the main thread (milliseconds).
    std::uint64_t frame = 0;
    std::optional<double> committedAtMs;
    std::optional<double> buildMs;
    std::optional<double> commitMs;
    /// Its plan's digest, its items, those the room's items of the last start knew (served from the cache at once).
    std::string planDigest;
    std::uint64_t items = 0;
    std::uint64_t itemsKnown = 0;
    /// Its fronts and spines resident when that frame was rendered (none until it was).
    std::optional<std::uint64_t> frontsResident;
    std::optional<std::uint64_t> spinesResident;

    bool operator==(const WorldPreviewRecord&) const = default;
};

/// How one collection room of the floor shown took a building's result (I4 step 6b, ADR-0013 §9: the per-room
/// reconcile, by content digest): its room id, app::toString(RoomUpdateKind) ("unchanged": its instances and uploads
/// kept; "textures": its covers served again; "items": its items applied in place; "added", "removed": a room the floor
/// gained or lost; "rebuilt": the floor committed again around it, its pictures kept), and its items compared in its
/// own frame.
struct RoomUpdateRecord {
    std::string room;
    std::string kind;
    std::uint64_t kept = 0;
    std::uint64_t moved = 0;
    std::uint64_t flagsChanged = 0;
    std::uint64_t added = 0;
    std::uint64_t removed = 0;
    /// Items whose cover request changed (a cover added, replaced or gone, a title changed).
    std::uint64_t coversChanged = 0;

    bool operator==(const RoomUpdateRecord&) const = default;
};

/// Where a floor committed again put the walker (I4 step 6b, ADR-0013 §7: kept in the frame of the area that holds it,
/// moved with that area): the area ("room:snes", "hall:gen4") and how far it moved in the scene's frame (meters; a
/// half turn of the frame turns it too).
struct WalkerKeptRecord {
    std::string area;
    glm::vec3 shift{0.0f};
    bool halfTurn = false;

    bool operator==(const WalkerKeptRecord&) const = default;
};

/// What the snapshot's result did to the stored plan's room shown (I3 step 10c, world_reconcile.hpp).
struct WorldReconcileRecord {
    /// app::toString(Reconciliation): "textures", "rebuild", "shell", or "items" (I3 step 15b: another collection of
    /// the same static part, its items applied in place); the frame it was committed in.
    std::string kind;
    std::uint64_t frame = 0;
    /// The collection shown against the result's, by game (LayoutDifference).
    std::uint64_t kept = 0;
    std::uint64_t moved = 0;
    std::uint64_t flagsChanged = 0;
    std::uint64_t added = 0;
    std::uint64_t removed = 0;
    /// "textures": the covers were served again (they changed, or the media root is known: their sources checked).
    bool coversReloaded = false;
    /// Each collection room's part (I4 step 6b: room by room), and where a floor committed again kept the walker.
    std::vector<RoomUpdateRecord> rooms;
    std::optional<WalkerKeptRecord> walker;

    bool operator==(const WorldReconcileRecord&) const = default;
};

/// A live update of the room shown (I3 step 15b, world_live_plan.hpp): the room's job run again after a refresh of the
/// catalog, and what its result did to the room.
struct WorldLiveUpdateRecord {
    /// What started it ("refresh"), the frame it was asked in and the frame its result was applied in.
    std::string trigger;
    std::uint64_t requestFrame = 0;
    std::uint64_t frame = 0;
    /// app::toString(LiveUpdateKind): "unchanged", "textures", "items", "rebuild", "kept"; why a rebuild or kept.
    std::string kind;
    std::string reason;
    /// app::toString(WorldOutcome) of the job ("updated", "unchanged", "kept", ...), the change report's rebuild level,
    /// whether the plan was written.
    std::string outcome;
    std::string rebuild;
    bool planWritten = false;
    /// The room shown against the result's collection, by game (LayoutDifference), and the items whose cover requests
    /// changed.
    std::uint64_t kept = 0;
    std::uint64_t moved = 0;
    std::uint64_t flagsChanged = 0;
    std::uint64_t added = 0;
    std::uint64_t removed = 0;
    std::uint64_t coversChanged = 0;
    /// Items: the arrays grew, an array was added; the items and static records after it.
    bool arraysGrown = false;
    bool arrayAdded = false;
    std::uint64_t items = 0;
    std::uint64_t staticRecords = 0;
    /// Milliseconds: from the request to the result's application, the room's job on the pool, the application on the
    /// main thread.
    std::optional<double> requestToApplyMs;
    std::optional<double> buildMs;
    std::optional<double> applyMs;
    /// Each collection room's part (I4 step 6b: room by room), and where a floor committed again kept the walker.
    std::vector<RoomUpdateRecord> rooms;
    std::optional<WalkerKeptRecord> walker;

    bool operator==(const WorldLiveUpdateRecord&) const = default;
};

/// A prefab of the room that fell back to its family's (style §21's object fallbacks).
struct ObjectFallbackRecord {
    /// The manifest the plan names, what replaced it ("family generic", "family procedural") and why.
    std::string prefab;
    std::string source;
    std::string reason;

    bool operator==(const ObjectFallbackRecord&) const = default;
};

/// A room of the floor shown (I4 step 5a, ADR-0013 §7): its id, its floor and its place on the floor (the plan's: x and
/// z of its frame's origin in millimetres in the floor's frame, its yaw in half turns).
struct WorldRoomRecord {
    std::string id;
    std::string floor;
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;
    std::uint32_t yawHalfTurns = 0;

    bool operator==(const WorldRoomRecord&) const = default;
};

/// A floor of the plan shown (I4 step 7b: world.floors): its id, its order (building.json's index: 0 the ground floor),
/// its rooms and the games they place.
struct WorldFloorRecord {
    std::string id;
    std::uint32_t order = 0;
    std::uint64_t rooms = 0;
    std::uint64_t placed = 0;

    bool operator==(const WorldFloorRecord&) const = default;
};

/// An elevator ride (I4 step 7b, ADR-0013 §10: world.rides): from the floor shown to another, the view faded out, the
/// destination's floor built on the pool, swapped at a frame boundary, faded in once the eye's area is resident (since
/// I4 step 8 the covers in view of the room that holds the eye, or by 3 s after the request; under lockstep the waits
/// replace the timeout).
struct RideRecord {
    std::string from;
    std::string to;
    /// app::toString(RideReason): "floor" (its elevator), "room" (a room's doorway), "showInRoom" (a game's Focus pose).
    std::string reason;
    /// The room (reasons room and showInRoom) and the game (showInRoom).
    std::string room;
    std::string game;
    /// The frame the ride was asked in, the one its fade-out ended in, the one the floor was swapped in, the one its
    /// fade-in ended in.
    std::uint64_t requestedFrame = 0;
    std::optional<std::uint64_t> fadedOutFrame;
    std::optional<std::uint64_t> swappedFrame;
    std::optional<std::uint64_t> fadedInFrame;
    /// Milliseconds from the request: to the destination's floor built (its job's result taken), to the eye's area
    /// resident (since I4 step 8 the covers in view of the room that holds the eye), to the view back (faded in: the
    /// acceptable row's measure, ADR-0013 §13); the floor-build job's own time.
    std::optional<double> builtMs;
    std::optional<double> residentMs;
    std::optional<double> totalMs;
    std::optional<double> jobMs;
    /// I4 step 8: the items of the room that holds the eye in the view when it came back, and those of them still
    /// pending (a ride that timed out); none in a hall.
    std::optional<std::uint64_t> viewItems;
    std::optional<std::uint64_t> viewPending;
    /// I4 step 8: milliseconds from the request to every item of the destination room resident (reasons room and
    /// showInRoom), followed after the view came back while the walker's floor stays (ADR-0013 §13's row of the room
    /// that holds the walker: 30 s cold, 3 s warm); none for a hall or while it is not.
    std::optional<double> roomResidentMs;
    /// "arrived"; "timed-out" (faded in after the timeout without the eye's area resident); "failed" (the destination
    /// could not be built: the walker where it was); "running" (the run ended during the ride).
    std::string outcome;
    /// Where the walker stood when the view came back (the destination's frame).
    std::optional<SpawnPoint> walker;
    /// The renderer's GPU memory when the view came back: the collection arrays' bytes, and every kind's.
    std::uint64_t arrayBytes = 0;
    std::uint64_t gpuBytes = 0;
    /// The same moment's bytes of each kind, in gl::kGpuMemoryKinds order (the report's "gpuKinds", by kind name): what
    /// the destination holds besides its arrays (its buffers, its scene's textures and procedural surfaces).
    std::array<std::uint64_t, gl::kGpuMemoryKindCount> kindBytes{};

    [[nodiscard]] std::uint64_t bytesOf(gl::GpuMemoryKind kind) const noexcept {
        return kindBytes[static_cast<std::size_t>(kind)];
    }
    bool operator==(const RideRecord&) const = default;
};

/// A teleport on the floor shown (I4 step 7b: world.teleports): the directory's room on the walker's floor (to its
/// doorway), the walker's own floor (to its elevator), "Mostra nella stanza" of a game in another room of the floor (to
/// that room's doorway, then the ease to the Focus pose).
struct TeleportRecord {
    std::uint64_t frame = 0;
    /// The room's id, or "elevator".
    std::string to;
    /// "floor", "room", "showInRoom" (as RideRecord::reason).
    std::string reason;
    SpawnPoint walker;

    bool operator==(const TeleportRecord&) const = default;
};

/// The building job's phases, in milliseconds on its worker (I4 step 6b: WorldBuild's times): the world's lock and the
/// plan read under it, the selections, the rooms' data, the generation or update with its validation, the plan's
/// write, the rooms' items files, and the whole job (the floor's build included); `skipped`: the job found the
/// catalog and the world's inputs as the stored plan was made from and planned nothing (world_build.hpp).
struct WorldJobRecord {
    double lockMs = 0.0;
    double selectMs = 0.0;
    double dataMs = 0.0;
    double planMs = 0.0;
    double saveMs = 0.0;
    double itemsMs = 0.0;
    double floorMs = 0.0;
    double totalMs = 0.0;
    bool skipped = false;

    bool operator==(const WorldJobRecord&) const = default;
};

/// A room the run's update migrated to its family's successor (I6 step 4: world::MigratedRoom).
struct MigratedRoomRecord {
    std::string room;
    std::string from;
    std::string to;
    /// Its titles its new room holds no slot for (unplaced for capacity).
    std::uint64_t unplacedForCapacity = 0;

    bool operator==(const MigratedRoomRecord&) const = default;
};

/// A room the run's update re-placed onto the hub (I6 step 5: world::ReplacedRoom), its place on the ground floor before
/// and after (millimetres).
struct ReplacedRoomRecord {
    std::string room;
    std::int64_t fromXMm = 0;
    std::int64_t fromZMm = 0;
    std::int64_t toXMm = 0;
    std::int64_t toZMm = 0;

    bool operator==(const ReplacedRoomRecord&) const = default;
};

/// A drawing of the hub's showcase (I6 step 5's review): the scene's mesh and material registries' sizes after it, and
/// whether its collection was updated in place (every drawing after the first of a floor).
struct ShowcaseDrawRecord {
    std::uint64_t meshes = 0;
    std::uint64_t materials = 0;
    bool inPlace = false;

    bool operator==(const ShowcaseDrawRecord&) const = default;
};

/// The hub's favourites showcase of the run (I6 step 5): its proxies' games (GameIds, in slot order) as shown last, how
/// many times its content was made, of them how many equalled the content shown (nothing drawn again), each drawing,
/// the cards it opened, the proxy hovered at the end, its layers' residency.
struct ShowcaseRecord {
    bool bound = false;
    std::vector<std::string> games;
    std::uint64_t builds = 0;
    std::uint64_t unchanged = 0;
    std::vector<ShowcaseDrawRecord> draws;
    std::uint64_t cardsOpened = 0;
    std::string lastCard;
    std::string hovered;
    std::uint64_t frontsResident = 0;

    bool operator==(const ShowcaseRecord&) const = default;
};

/// The generated building of the run (I3 step 10b's room, the floor shown since I4 step 5a): what showed, from which
/// plan, how it came to be and when. Empty ({}) in runs that ask for no room (--scene, --model, --mock-catalog, the
/// asset preview).
/// I4b step 3: one operation of an edit session (ADR-0015 §4), as the editor asked it.
struct EditOperationRecord {
    /// The frame the player asked it in, and the frame it showed in (none: refused, or not shown by the run's end).
    std::uint64_t frame = 0;
    std::optional<std::uint64_t> shownFrame;
    /// world::toString(EditKind) and world::editText().
    std::string op;
    std::string operation;
    /// The refusal's rule ("" applied), and "" or the edit's rebuild level ("no-rebuild", "local-reflow").
    std::string refused;
    std::string rebuild;
    /// How it showed: "in-place" (the room's collection laid out again and applied in place), "room" (I4b step 4: its
    /// static part changed, rebuilt alone, then its collection applied in place), "floor" (the floor built again from
    /// the draft), "none" (pins only, or refused).
    std::string path;
    /// Milliseconds: the draft's applyEdit() and the room's layout on the pool; from the request to the frame it
    /// showed (ADR-0015 §9's 0.25 s).
    std::optional<double> editMs;
    std::optional<double> shownMs;

    bool operator==(const EditOperationRecord&) const = default;
};

/// I4b step 3: one edit session (ADR-0015 §4).
struct WorldEditRecord {
    /// The room edited, how the session was entered ("directory", "card") and on which frame; the game "Sposta" held.
    std::string room;
    std::string via;
    std::uint64_t enterFrame = 0;
    std::string heldAtStart;
    /// The room's data read and the session ready (milliseconds after the request), or why it could not start.
    std::optional<std::uint64_t> readyFrame;
    std::optional<double> prepareMs;
    std::string failed;
    std::vector<EditOperationRecord> operations;
    /// "Annulla ultima azione" taken, and the menu's openings; I4b step 4: the held fixtures' places judged (probes).
    std::uint64_t undone = 0;
    std::uint64_t menus = 0;
    std::uint64_t probes = 0;
    /// How it ended ("saved", "discarded", "exited": nothing pending; "" still open at the run's end) and on which frame;
    /// the save: written, replayed over a changed plan, refused (its rule), the snapshot, the digest of the plan shown
    /// at the end (the saved one after Salva, the stored one after Esci senza salvare), its time and the time until the
    /// room showed again (ADR-0015 §9's 3 s).
    std::string end;
    std::optional<std::uint64_t> endFrame;
    bool written = false;
    bool replayed = false;
    std::string saveRefused;
    std::string snapshot;
    std::string planDigest;
    std::optional<double> saveMs;
    std::optional<double> backMs;
    /// A live update (a refreshed catalog's building job) waited for the session's end (it starts then).
    bool liveUpdateDeferred = false;
    /// I4b step 8: the rooms of its chain the session edited, in the order the walker entered them (its room first).
    std::vector<std::string> rooms;

    bool operator==(const WorldEditRecord&) const = default;
};

/// I4b step 6: one regeneration or undo asked in the app (ADR-0015 §5, §6): its preview, the choice, the commit.
struct WorldRegenerationRecord {
    /// "room", "floor", "world", "undo"; the scope (a room's id, "floor:<id>", "world"; an undo's: the snapshot's
    /// scope once known); how it was asked ("directory", "notice"), on which frame.
    std::string kind;
    std::string scope;
    std::string via;
    std::uint64_t requestFrame = 0;
    /// The preview shown: its frame, the time from the request (ADR-0015 §9: a room 2 s, a floor or the world 15 s),
    /// its job's, the catalog's inputs (taken by the preview: `inputsTaken`, else made before) and the dry run's own.
    std::optional<std::uint64_t> previewFrame;
    std::optional<double> previewMs;
    std::optional<double> previewJobMs;
    std::optional<double> inputsMs;
    bool inputsTaken = false;
    std::optional<double> regenerateMs;
    /// Why there was no preview or no commit (ui::regenerate_preview::kFailures; "" none).
    std::string failure;
    /// The preview's counts (ui::RegeneratePreviewContent), whether "Rigenera" would change anything, the overrides the
    /// drop would remove; an undo's snapshot.
    std::uint64_t roomsRegenerated = 0;
    std::uint64_t roomsAdded = 0;
    std::uint64_t roomsRemoved = 0;
    std::uint64_t roomsMoved = 0;
    std::uint64_t gamesMoved = 0;
    std::uint64_t faceOutEntering = 0;
    std::uint64_t faceOutLeaving = 0;
    std::uint64_t newlyPlaced = 0;
    std::uint64_t newlyUnplaced = 0;
    std::uint64_t fixturesAdded = 0;
    std::uint64_t fixturesRemoved = 0;
    std::uint64_t fixturesMoved = 0;
    std::uint64_t pinsKept = 0;
    std::uint64_t conflicts = 0;
    std::uint64_t overrides = 0;
    bool changed = false;
    std::string undoSnapshot;
    /// The second confirmations of "Rigenera e scarta le personalizzazioni" shown.
    std::uint64_t confirmations = 0;
    /// The choice ("regenerate", "regenerate-drop", "undo", "cancel"; "" none by the run's end) and its frame.
    std::string choice;
    std::optional<std::uint64_t> choiceFrame;
    /// The commit: written (behind the snapshot named), the preview's own result committed (`reused`), the digest of
    /// the plan stored after it, its job's time; the room shown again (ADR-0015 §9: 3 s warm) from the choice to the
    /// frame that shows it, how ("in-place": the floor's rooms applied in place, "floor": the floor committed again,
    /// "none"), where the walker went ("doorway:<room>", "elevator", "kept").
    bool written = false;
    bool reused = false;
    std::string snapshot;
    std::string planDigest;
    std::optional<double> commitMs;
    std::optional<std::uint64_t> backFrame;
    std::optional<double> backMs;
    std::string path;
    std::string walker;
    /// How it ended: "committed", "cancelled", "failed" ("" still open at the run's end).
    std::string end;

    bool operator==(const WorldRegenerationRecord&) const = default;
};

struct WorldRecord {
    /// The run asked for the generated room (retroverse.exe without --scene, --model or --mock-catalog).
    bool bound = false;
    /// What showed at the end: "room" (the plan's room), "shell" (the empty room: no plan to show), "test-room" (the
    /// fallback: no catalog, no games of the room's system), "generating" (still being made when the run ended).
    std::string state;
    /// The world notice shown last (world_notice_ui.hpp's keys; "" none) and why, in English; and the start room it
    /// names (the notice's "room": --start-room's id, "" without one).
    std::string notice;
    std::string reason;
    std::string noticeRoom;
    /// The catalog's state when the test room showed for want of a catalog ("no-library", ...).
    std::string catalogState;
    /// The room that held the walker's eye at the end ("room:snes"; "" in a hall, or before a floor showed: I4 step 5a),
    /// the room whose CRT was the hero CRT at the end (I4 step 6b: the eye's room after the dwell; "" none), and the
    /// library.
    std::string room;
    std::string screenRoom;
    std::string library;
    /// The floor shown ("ground", "gen4", ...; "" before one showed) and its rooms (I4 step 5a).
    std::string floor;
    std::vector<WorldRoomRecord> rooms;
    /// app::toString(WorldOutcome): "generated", "updated", "unchanged", "kept", "no-plan"; and the change report's
    /// outcome and rebuild level when a generation or update ran.
    std::string outcome;
    std::string changeOutcome;
    std::string rebuild;
    /// The SHA-256 of the plan shown, and whether this run wrote it (world-plan.json).
    std::string planDigest;
    bool planWritten = false;
    std::string saveError;
    /// Placements, per section, and the unplaced games, per reason.
    std::uint64_t placements = 0;
    std::uint64_t featured = 0;
    std::uint64_t band = 0;
    std::uint64_t spine = 0;
    std::uint64_t unplaced = 0;
    std::uint64_t duplicateTitle = 0;
    std::uint64_t noCover = 0;
    std::uint64_t unavailable = 0;
    std::uint64_t capacity = 0;
    /// In a room an update keeps as it is until a regeneration (ADR-0013 §5).
    std::uint64_t roomKept = 0;
    /// The change report's counts: games added, placements kept.
    std::uint64_t added = 0;
    std::uint64_t kept = 0;
    /// The rooms the update migrated to their families' successors (I6 step 4), by room.
    std::vector<MigratedRoomRecord> migratedRooms;
    /// The rooms the update re-placed onto the hub (I6 step 5), by room.
    std::vector<ReplacedRoomRecord> replacedRooms;
    /// What the update left undone because it would have moved something pinned (world::ChangeReport's
    /// needsConfirmation: an unhonoured pin, a pinned room not migrated), as the report words it.
    std::vector<std::string> needsConfirmation;
    /// I4b step 8 (ADR-0015 §7): the expansion rooms the update that saved the building added (the change report's
    /// roomsAdded of a chain), which its notice names.
    std::vector<std::string> roomsAdded;
    /// The validation's findings of the plan made (a hard one keeps the plan from showing).
    std::uint64_t hardFindings = 0;
    std::uint64_t softFindings = 0;
    /// The scene: its objects and lights, the dressed spans it left bare; the collection: items, clusters, placements
    /// it could not draw, the static group's records (three per item: shell, front, spine) and pick targets.
    std::uint64_t objects = 0;
    std::uint64_t lights = 0;
    std::uint64_t undressed = 0;
    std::uint64_t items = 0;
    std::uint64_t clusters = 0;
    std::uint64_t skipped = 0;
    std::uint64_t staticRecords = 0;
    std::uint64_t pickTargets = 0;
    /// Times in milliseconds: from the process's creation to the catalog's snapshot done on its worker and to the
    /// room's commit (after a preview, its reconciliation's); the snapshot on the worker, the room's job on the pool,
    /// the commit on the main thread; from the snapshot's answer to the commit.
    std::optional<double> snapshotAtMs;
    std::optional<double> committedAtMs;
    std::optional<double> snapshotMs;
    std::optional<double> buildMs;
    std::optional<double> commitMs;
    std::optional<double> snapshotToCommitMs;
    /// The building job's phases (I4 step 6b), the last one committed; none before one answered.
    std::optional<WorldJobRecord> job;
    /// The frame the room first showed in (its preview's, after a persisted start; none before it showed).
    std::optional<std::uint64_t> firstFrame;
    /// The hover (I3 step 10b's outline on the picked item): changes of the hovered item, and the one hovered at the
    /// end (its pick id in hexadecimal, its GameId), if any.
    std::uint64_t hoverChanges = 0;
    std::string hoveredPickId;
    std::string hoveredGame;
    /// The interaction with the room's items (I3 step 13a).
    InteractionRecord interaction;
    /// The card beside the inspected item (I3 step 14a).
    CardRecord card;
    /// The launches from the room and "Mostra nella stanza" (I3 step 15a).
    RoomLaunchRecord launch;
    /// I3 step 10c: the persisted start's preview and its reconciliation (none without a stored plan shown); the room's
    /// items written for the next start; the games given to the hero CRT; the room's object fallbacks.
    std::optional<WorldPreviewRecord> preview;
    std::optional<WorldReconcileRecord> reconcile;
    bool itemsWritten = false;
    std::uint64_t screenGames = 0;
    std::vector<ObjectFallbackRecord> objectFallbacks;
    /// I3 step 15b: the live updates applied to the room shown, in order (after a reconciliation of the start that
    /// applied its items in place, `reconcile.kind` is "items").
    std::vector<WorldLiveUpdateRecord> liveUpdates;
    /// I4 step 7b: the plan's floors in the elevator's order, the rides and teleports in order, the frames the
    /// elevator's prompt showed.
    std::vector<WorldFloorRecord> floors;
    std::vector<RideRecord> rides;
    std::vector<TeleportRecord> teleports;
    std::uint64_t elevatorPromptFrames = 0;
    /// I6 step 5: the frames the hub's directory totem's prompt showed, its favourites showcase.
    std::uint64_t totemPromptFrames = 0;
    ShowcaseRecord showcase;
    /// I4b step 3: the editor's sessions, in order.
    std::vector<WorldEditRecord> edits;
    /// I4b step 6: the regenerations and undos asked, in order.
    std::vector<WorldRegenerationRecord> regenerations;
    /// I4b step 6's review: the times the regenerations' inputs were let go of while nothing needed them (the
    /// directory closed, no notice offering a preview, no preview or commit asked or running).
    std::uint64_t regenerationInputsReleased = 0;
    /// I5 step 4's review: the frames that drew a cabinet's clip on the dynamic path, and those among them in which
    /// such a cabinet's static front (its cover) was drawn too, or its item was gone: 0.
    std::uint64_t cabinetClipFrames = 0;
    std::uint64_t cabinetCoverBeneathFrames = 0;

    bool operator==(const WorldRecord&) const = default;
};

/// The hero textures of the run (I3 step 12, ADR-0011 §8: the focused item's cover at up to 1,024 px, 8 kept):
/// textures asked for, made, given up (no readable cover), evicted by the least-recently-used set, resident at the
/// end and their bytes (now and at most), the longest upload (milliseconds), the game whose hero was shown at the end.
struct HeroRecord {
    std::uint64_t requests = 0;
    std::uint64_t loaded = 0;
    std::uint64_t failed = 0;
    std::uint64_t evicted = 0;
    std::uint64_t resident = 0;
    std::uint64_t bytes = 0;
    std::uint64_t peakBytes = 0;
    double maxUploadMs = 0.0;
    std::string shownGame;

    bool operator==(const HeroRecord&) const = default;
};

/// One collection room's layers on the floor shown (I4 step 5a): its items, the layers of each class resident and
/// pending, its arrays' bytes, its arrays and how many of them its layered materials sample (all of them: its own
/// arrays, never another room's, ADR-0013 §9's registry keys carrying the room id), and its items in the loader's first
/// view (ResidencyRecord::viewItems' share: none for a room that neither holds the eye nor has its doorway in view).
struct RoomResidencyRecord {
    std::string room;
    std::uint64_t items = 0;
    std::uint64_t viewItems = 0;
    std::uint64_t frontsResident = 0;
    std::uint64_t spinesResident = 0;
    std::uint64_t pending = 0;
    std::uint64_t arrayBytes = 0;
    std::uint64_t arrays = 0;
    std::uint64_t ownArrays = 0;
    /// Layers uploaded into the room's collection (the one that shows at the end: kept by a per-room reconcile, I4 step
    /// 6b), and of them those over a layer its item already showed: a picture uploaded again (0 for a room unchanged).
    std::uint64_t uploads = 0;
    std::uint64_t reuploads = 0;

    bool operator==(const RoomResidencyRecord&) const = default;
};

/// The collection's textures (I3 step 10b's loader; step 12 orders it): layers resident and pending, the cache's hits
/// and misses, when the room was shown and when its last layer became resident. Empty ({}) without a room. Since I4
/// step 5a the floor's: its counts the totals of its collection rooms, each room's in `rooms`.
struct ResidencyRecord {
    bool bound = false;
    /// The arrays' format ("bc1-srgb", "rgba8-srgb") and their bytes.
    std::string format;
    std::uint64_t arrayBytes = 0;
    /// Items, and the layers of each class resident and still pending at the end.
    std::uint64_t items = 0;
    std::uint64_t frontsResident = 0;
    std::uint64_t spinesResident = 0;
    std::uint64_t pending = 0;
    /// Fronts from covers (read now or from the cache), from the cache with their cover out of reach, generated labels
    /// (no cover; a cover that could not be used), and layers that could not be made.
    std::uint64_t covers = 0;
    std::uint64_t cachedCovers = 0;
    std::uint64_t labels = 0;
    std::uint64_t coverFailures = 0;
    std::uint64_t failed = 0;
    /// The derived-texture cache's counters (assets::CacheStats).
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheHitsWithoutSource = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t cacheStale = 0;
    std::uint64_t cacheCorrupt = 0;
    std::uint64_t cacheStored = 0;
    std::uint64_t cacheStoreFailures = 0;
    /// Milliseconds from the process's creation to the room shown and to its last layer resident, and between them.
    std::optional<double> shownAtMs;
    std::optional<double> allResidentAtMs;
    std::optional<double> residentAfterShownMs;
    /// The frame whose uploads made the last layer resident.
    std::optional<std::uint64_t> allResidentFrame;
    /// Frames (and idle wakes) that uploaded layers, and the most layers one frame uploaded (idle wakes of a lockstep
    /// run, which have a budget of their own, are not counted).
    std::uint64_t uploadRounds = 0;
    std::uint64_t maxLayersPerRound = 0;

    /// The loader's order (I3 step 12, ADR-0011 §6: the focused item, the view by projected size, the rest by
    /// distance): the items in view when the room showed; when every item in view was first resident (from the room
    /// shown) and how many items were resident then; how many of the first `viewItems` arrivals were in view; the mean
    /// distance (meters, from the eye when each arrived) of each quarter of the arrivals that were out of view, in
    /// arrival order (0 for an empty quarter).
    std::uint64_t viewItems = 0;
    std::optional<double> viewResidentAfterShownMs;
    std::uint64_t residentAtViewResident = 0;
    std::uint64_t inViewAmongFirst = 0;
    std::array<double, 4> arrivalDistanceQuarters{};
    /// Frame rounds whose uploads took longer than the frame's budget (1 ms), and the longest round (milliseconds; the
    /// layers and the hero texture together). Idle wakes of a lockstep run, which have a budget of their own, are not
    /// counted.
    std::uint64_t overBudgetRounds = 0;
    double maxRoundMs = 0.0;
    /// Lockstep runs: frames whose update waited for their view (the camera brought items into view that were not
    /// resident yet), and the time they waited in all.
    std::uint64_t lockstepWaits = 0;
    double lockstepWaitMs = 0.0;
    /// Rounds that made and uploaded nothing because a launched game ran (ADR-0012 §6: the loader paused).
    std::uint64_t pausedRounds = 0;
    /// The loader's retries after the catalog's refreshes (ADR-0011 §6: the media root back): rounds, items queued
    /// again (fronts not from their cover), and items whose pictures changed (uploaded again).
    std::uint64_t retryRounds = 0;
    std::uint64_t retriedItems = 0;
    std::uint64_t retriedChanged = 0;
    /// Items a new loader of the same collection took over from the one it replaced (I3 step 10c's textures
    /// reconciliation), and those whose pictures changed (uploaded again; the others were not); of the items taken
    /// over, those whose pictures showed as they are asked now (I4 step 6b: done at once, nothing served again).
    std::uint64_t adoptedItems = 0;
    std::uint64_t adoptedChanged = 0;
    std::uint64_t adoptedResident = 0;
    /// The hero textures (ADR-0011 §8).
    HeroRecord hero;
    /// Each collection room's (I4 step 5a), in the floor's room order.
    std::vector<RoomResidencyRecord> rooms;

    bool operator==(const ResidencyRecord&) const = default;
};

/// A picture the hero CRT could not show (I3 step 11, ADR-0012 §7: the chain then shows the next one).
struct VideoFallbackRecord {
    /// The game's catalog id.
    std::string gameId;
    /// What could not be shown (app::toString(ScreenPicture): "video", "screenshot", "title-screen", "cover").
    std::string picture;
    /// Why: for the video its media::DecodeFailure, hyphenated ("file-not-found", "invalid-file", "corrupt-data",
    /// "truncated", "unsupported-codec", ...), or "not-playable" (a file ES-DE would not read, never opened); for a
    /// still, or a decoder or texture that could not start, the error's code (toString(ErrorCode): "not-found",
    /// "parse-error", "unsupported").
    std::string reason;

    bool operator==(const VideoFallbackRecord&) const = default;
};

/// What a launch did to the hero CRT's decoder (ADR-0012 §6): stopped and its thread waited for, at most
/// kScreenStopBoundSeconds, before the hand-over; opened again at its position after the return.
struct VideoLaunchRecord {
    /// A decoder ran (or was still ending) when the launch started.
    bool decoderRan = false;
    /// Milliseconds from the stop to the end of its thread (to the bound when it had not ended).
    double stopMs = 0.0;
    /// Decoder threads still running when the hand-over began: 0 unless the bound passed.
    std::uint64_t threadsRunning = 0;
    /// Seconds into the clip shown when it stopped (absent when no frame of a video showed).
    std::optional<double> positionSeconds;
    /// The decoder opened again after the launch ended, seeking to this position (absent: not reopened, or from the
    /// start of the clip).
    bool reopened = false;
    std::optional<double> reopenedAtSeconds;

    bool operator==(const VideoLaunchRecord&) const = default;
};

/// The hero CRT bound to a screen after the run's first (I4 step 6b, ADR-0013 §9: the CRT of the room that holds the
/// eye, after the dwell; a scene shown in place of another, such as a floor committed again, binds its CRT again; the
/// run's first screen, at the start or the first room the eye stays in, is not one): the frame, the screen's object id,
/// the room's games, the milliseconds the bind took on the main thread, the decoder threads still running as it
/// returned, and whether it stopped a clip open on the previous screen (playing, or paused out of view: its thread
/// ends on its own, never joined there).
struct VideoBindRecord {
    std::uint64_t frame = 0;
    std::string screen;
    std::uint64_t games = 0;
    double bindMs = 0.0;
    std::uint64_t threadsRunning = 0;
    bool clipStopped = false;

    bool operator==(const VideoBindRecord&) const = default;
};

/// A game the hero CRT played (I5 step 1): when its turn began (the run's simulated seconds), its catalog id and why it
/// played (app::toString(ScreenOrigin): "start", "playlist", "selection").
struct VideoPlayedRecord {
    double atSeconds = 0.0;
    std::string gameId;
    std::string origin;

    bool operator==(const VideoPlayedRecord&) const = default;
};

/// A picture the hero CRT began to show (I5 step 1): when, the game and the picture (app::toString(ScreenPicture)).
struct VideoShownRecord {
    double atSeconds = 0.0;
    std::string gameId;
    std::string picture;

    bool operator==(const VideoShownRecord&) const = default;
};

/// A screen of the service at the end of the run (I5 step 2, ADR-0016 §2): its object id, its kind
/// (app::toString(ScreenKind)) and class (app::toString(ScreenClass)), the game and the picture it showed, the decoder
/// slot showing its clip or reserved for it (none without), whether that slot played its clip, the GL texture its
/// material showed (0: its own pattern; screens sharing a slot show one texture) and whether it was in view. Since I5
/// step 4: the arcade cabinets asking for their clips at the end too (kind "cabinet"; picture "video" with the texture
/// their pooled video material showed while their clip shows, else "cover" with 0: their item's own front); the
/// rectangle of its screen the picture filled (x, y, width, height in its texture coordinates: ADR-0016 §7's letterbox
/// or pillarbox; empty for the whole screen); whether it showed the frame it kept when its slot was taken (ADR-0016 §6:
/// with one decoder, the hero cabinet's paused frame while a focused cabinet plays; a cabinet never keeps one).
struct VideoScreenRecord {
    std::string screen;
    std::string kind;
    std::string screenClass;
    std::string gameId;
    std::string picture;
    std::optional<std::uint64_t> slot;
    bool playing = false;
    std::uint64_t textureId = 0;
    bool visible = false;
    std::vector<double> pictureRect;
    bool heldFrame = false;
    /// I5 step 3's review: the box its distance and view test read (its object's bounds in the scene: min x, y, z, max
    /// x, y, z; a cabinet's item box), as bound or rebound last.
    std::vector<double> bounds;

    bool operator==(const VideoScreenRecord&) const = default;
};

/// An arcade cabinet's screen that began or ended to show its clip (I5 step 4, ADR-0016 §6): the frame and the run's
/// simulated seconds, the cabinet's screen (app::cabinetScreenId()), its game, its class then ("focus", "direct",
/// "ambient") and what it shows from that frame: "video" (its clip, drawn on the dynamic path with a pooled video
/// material) or "cover" (its item's own front again).
struct VideoCabinetEventRecord {
    std::uint64_t frame = 0;
    double atSeconds = 0.0;
    std::string screen;
    std::string gameId;
    std::string screenClass;
    std::string picture;

    bool operator==(const VideoCabinetEventRecord&) const = default;
};

/// What the scheduler did to a decoder slot (I5 step 2): the frame and the run's simulated seconds, the slot, the
/// action (app::toString(SlotAction): "open", "stop", "pause", "resume"), the game whose clip it was, why (a
/// SlotCommand's reason: "wanted", "unwanted", "needed", "out-of-view", "in-view", "budget", "launch", "unbind",
/// "shutdown") and the screens showing it then; since I5 step 4's review an open's position in its clip (seconds: where
/// it stopped, a launch's position, --video-freeze-pts; absent from its start).
struct VideoSlotEventRecord {
    std::uint64_t frame = 0;
    double atSeconds = 0.0;
    std::uint64_t slot = 0;
    std::string action;
    std::string gameId;
    std::string reason;
    std::vector<std::string> screens;
    std::optional<double> fromSeconds;

    bool operator==(const VideoSlotEventRecord&) const = default;
};

/// A late-frame drop to one decoder (I5 step 2, app::BudgetDrop): when, the slot and game whose frames came late, the
/// ratio of late and dropped frames over its window, and when the budget came back.
struct VideoBudgetDropRecord {
    double atSeconds = 0.0;
    std::uint64_t slot = 0;
    std::string gameId;
    double ratio = 0.0;
    double untilSeconds = 0.0;

    bool operator==(const VideoBudgetDropRecord&) const = default;
};

/// A clip's verdict in the run (I5 step 7, ADR-0016 §8): its game and the verdict (media::verdictName: "playable",
/// "corrupt-data", ...).
struct VideoVerdictRecord {
    std::string gameId;
    std::string verdict;

    bool operator==(const VideoVerdictRecord&) const = default;
};

/// The clips' verdicts over the run (I5 step 7, ADR-0016 §8: <user data>/cache/media/videos.json): whether the file was
/// read and its verdicts, the games whose clips were left out as known to fail (never opened: their screens show their
/// stills), the verdicts the run gave (a clip that failed, or played through once clean) and how many of them reached
/// the file, and the writes that failed.
struct VideoVerdictsRecord {
    bool loaded = false;
    std::uint64_t entries = 0;
    std::vector<VideoVerdictRecord> knownBad;
    std::vector<VideoVerdictRecord> given;
    std::uint64_t written = 0;
    std::uint64_t writeFailures = 0;

    bool operator==(const VideoVerdictsRecord&) const = default;
};

/// The hero CRT's video (I3 step 11, ADR-0012 §7: app::ScreenService): what it showed at the end of the run, the one
/// decoder, the current clip's first frame and uploads, the fallbacks and the launches. Since I5 step 2 the service
/// holds several screens (the hero first) and slots: the fields below are the hero screen's, the decoder and upload
/// counts the whole service's, and `screens`, `slotEvents` and `budgetDrops` the scheduler's.
struct VideoRecord {
    /// A hero CRT was bound (the scene's first `crt` object with a picture on its screen) and its object id; without
    /// one nothing else is recorded.
    bool bound = false;
    std::string screen;
    /// At the end of the run: the game shown (its catalog id, empty for none), the picture (toString(ScreenPicture)),
    /// the decoder's life (toString(ScreenPhase): "launched" when Retroverse quit during a game) and its state
    /// (media::toString(DecoderState): "playing", "paused", ...; "none" without one).
    std::string gameId;
    std::string picture;
    std::string phase;
    std::string decoderState;
    /// --video-freeze-pts, and whether the frame at that time was shown.
    std::optional<double> freezePtsSeconds;
    bool frozenFrameShown = false;
    /// Selections that became the target after their dwell; targets the screen had (the start choice included).
    std::uint64_t selections = 0;
    std::uint64_t targets = 0;
    /// Decoders started in the run, and the most decoder threads alive at once (never above 1: NFR-07's single
    /// decoder of I3, asserted).
    std::uint64_t decodersStarted = 0;
    std::uint64_t maxDecoderThreads = 0;
    /// Frames uploaded over the run; uploads skipped with the pixel buffer ring busy; pauses out of the view.
    std::uint64_t framesUploaded = 0;
    std::uint64_t ringBusy = 0;
    std::uint64_t pauses = 0;
    /// The last clip opened: wall-clock milliseconds from its decoder's start (its selection after the dwell, the
    /// start choice, the reopening after a launch) to its first frame uploaded (NFR-06: within 2 s of the selection);
    /// the frames of it uploaded, their first and last timestamps and the simulated seconds between their uploads.
    std::optional<double> firstFrameMs;
    std::uint64_t clipFramesUploaded = 0;
    std::optional<double> firstPtsSeconds;
    std::optional<double> lastPtsSeconds;
    double clipUploadSeconds = 0.0;
    /// GPU bytes of the video's texture and pixel buffer ring (gpuMemory's video) and of the still picture shown
    /// (gpuMemory's heroTextures) at the end of the run.
    std::uint64_t videoBytes = 0;
    std::uint64_t stillBytes = 0;
    /// The decoder threads had all ended once the service shut down (media::waitForStoppedDecoders()).
    bool joinedAtExit = false;
    /// Waits for every stopped decoder's thread (media::stoppedDecoderWaits()) made before the service's shutdown at
    /// the run's end: 0, none from the render loop (ADR-0004; I4 step 6b).
    std::uint64_t waitsBeforeExit = 0;
    /// The pictures that could not be shown and why, in order (the first 512: a long run's failing clip falls back each
    /// time it plays).
    std::vector<VideoFallbackRecord> fallbacks;
    std::vector<VideoLaunchRecord> launches;
    /// The screens bound after the first (I4 step 6b), in order.
    std::vector<VideoBindRecord> binds;
    /// The bound screens moved onto their objects replaced in place, their state kept (I5 step 3,
    /// ScreenService::rebind(): the room editor rebuilt the room's static part).
    std::uint64_t rebinds = 0;
    /// The playlist at the end of the run (I5 step 1, ADR-0016 §3): its mode as played (toString(PlaylistMode)), its
    /// room, the seconds a game plays, its current cycle's order, and why the last target played.
    std::string playlistMode;
    std::string playlistRoom;
    double previewSeconds = 0.0;
    std::vector<std::string> playlistOrder;
    std::string origin;
    /// Each game played, and each picture shown, in order (the first 512 of each).
    std::vector<VideoPlayedRecord> played;
    std::vector<VideoShownRecord> shown;
    /// Frames a switch kept the previous picture up while the next was not ready, and frames the screen's own pattern
    /// showed although it had a game (before its first picture since it was bound, or once a switch's hold ran out:
    /// app::kMaxHeldPictureSeconds, or nothing left that could show).
    std::uint64_t heldFrames = 0;
    std::uint64_t idleFramesWithGame = 0;
    /// The scheduler (I5 step 2, ADR-0016 §2): the budget (video.json's maxDecoders, or --max-decoders), the most
    /// decoders open at once (never above the budget: NFR-07, asserted), the slots taken from a clip still asked for;
    /// the most video bytes uploaded in one render frame (at most app::kMaxVideoUploadBytesPerFrame unless one frame
    /// alone is larger), the uploads a frame's budget put off to the next frame, and the frames whose first upload alone
    /// was over the budget.
    std::uint64_t maxDecoders = 0;
    std::uint64_t maxOpenDecoders = 0;
    std::uint64_t handOvers = 0;
    std::uint64_t maxUploadBytes = 0;
    std::uint64_t deferredUploads = 0;
    std::uint64_t oversizeUploads = 0;
    /// Every screen bound at the end, the hero first; the slots' events in order (the first 512); the late-frame drops.
    std::vector<VideoScreenRecord> screens;
    std::vector<VideoSlotEventRecord> slotEvents;
    std::vector<VideoBudgetDropRecord> budgetDrops;
    /// The slots' events after the first 512, not listed (I5 step 2's review).
    std::uint64_t slotEventsDropped = 0;
    /// The arcade cabinets' screens (I5 step 4): each start and end of a clip shown, in order (the first 512), those
    /// after them (not listed), and the frames a bound screen showed the frame it kept when its slot was taken.
    std::vector<VideoCabinetEventRecord> cabinetEvents;
    std::uint64_t cabinetEventsDropped = 0;
    std::uint64_t heldFrameShows = 0;
    /// Lockstep runs (I5 step 4's review): the waits for the first frame of a clip a slot opened for a screen in view
    /// (at most 2 s, NFR-06's), the longest of them (wall-clock milliseconds) and those that ran out.
    std::uint64_t firstFrameWaits = 0;
    double firstFrameWaitMaxMs = 0.0;
    std::uint64_t firstFrameWaitTimeouts = 0;
    /// The clips' verdicts (I5 step 7).
    VideoVerdictsRecord verdicts;

    bool operator==(const VideoRecord&) const = default;
};

/// A change of the process's priority class (I5 step 2, ADR-0016 §2): the frame, the priority read back from the
/// system afterwards (platform_win::toString(ProcessPriority): "above-normal", "normal", "other"), why ("rendering":
/// the 3D view renders; "minimized"; "launch": before an emulator starts; "exit") and whether the system took it.
struct PriorityChangeRecord {
    std::uint64_t frame = 0;
    std::string priority;
    std::string reason;
    bool applied = false;

    bool operator==(const PriorityChangeRecord&) const = default;
};

/// The process priority over the run (retroverse.exe): each change in order.
struct ProcessPriorityRecord {
    std::vector<PriorityChangeRecord> changes;

    bool operator==(const ProcessPriorityRecord&) const = default;
};

/// A screen's CRT look (I5 step 6, ADR-0016 §7, app/crt_looks.hpp) as its material was registered: the screen (its
/// object's id, or a room's cabinets' "collection/<room id>/<family>"), its room, its kind ("hero", "secondary",
/// "cabinet"), the preset it draws with and the one the rules chose before the user's crtEffect
/// (assets::toString(CrtPreset): "neutral", "consumer", "arcade", "monitor"), and the rule ("video.json", "pack",
/// "kind").
struct CrtLookRecord {
    std::string screen;
    std::string room;
    std::string kind;
    std::string preset;
    std::string chosen;
    std::string rule;

    bool operator==(const CrtLookRecord&) const = default;
};

/// The screens' CRT looks of the scene shown at the end (retroverse.exe): video.json's crtEffect ("off", "light",
/// "vintage"), where the presets' values came from (assets::LoadedCrtPresets::source: "shipped", "user", "built-in")
/// and each screen's look, by screen.
struct CrtLooksRecord {
    std::string crtEffect;
    std::string presetsSource;
    std::vector<CrtLookRecord> screens;

    bool operator==(const CrtLooksRecord&) const = default;
};

/// A change of the audible screen (I5 step 5, ADR-0016 §9): the frame, the run's simulated time, the screen (""
/// none: silence) and the game it plays.
struct AudioAudibleRecord {
    std::uint64_t frame = 0;
    double atSeconds = 0.0;
    std::string screen;
    std::string gameId;

    bool operator==(const AudioAudibleRecord&) const = default;
};

/// The audible screen's sound, sampled every kAudioLevelSampleSeconds of the run: its distance, its spatial gain and pan
/// (audio::spatialGain, panForAzimuth) and the source's whole gain (its volume and bus times the spatial gain).
struct AudioLevelRecord {
    double atSeconds = 0.0;
    std::string screen;
    double distance = 0.0;
    double spatialGain = 0.0;
    double pan = 0.0;
    double gain = 0.0;

    bool operator==(const AudioLevelRecord&) const = default;
};

/// A frame of the audible clip uploaded, in runs that capture the mix (--audio-capture): the run's simulated time it
/// showed at and its time in the clip, so that the capture's sound can be set against the picture.
struct AudioPresentedRecord {
    double atSeconds = 0.0;
    double ptsSeconds = 0.0;

    bool operator==(const AudioPresentedRecord&) const = default;
};

/// A clip whose sound was decoded: its game and its audio stream (codec, rate, channels; 0 without one).
struct AudioClipRecord {
    std::string gameId;
    std::string codec;
    std::uint64_t sampleRate = 0;
    std::uint64_t channels = 0;

    bool operator==(const AudioClipRecord&) const = default;
};

/// The clips' sound over the run (I5 step 5, ADR-0016 §9; retroverse.exe). `device`: the output used ("device", "null",
/// "off": every smoke test asserts null or off), `reason`: why (app::resolveAudioOutput's: "RV_AUDIO", "--audio",
/// "frame-limited", "interactive"; "device-failed" when the device did not open and the null output took over).
struct AudioRecord {
    std::string device;
    std::string reason;
    /// The endpoint's name (a device only).
    std::string deviceName;
    std::uint64_t sampleRate = 0;
    bool mutedAtStart = false;
    bool muted = false;
    std::uint64_t muteToggles = 0;
    /// Frames rendered while the sound was ducked (the card, the directory, the quick catalog), and the duck's changes.
    std::uint64_t duckedFrames = 0;
    std::uint64_t duckChanges = 0;
    /// The mix: blocks (pulls or callbacks) and frames rendered.
    std::uint64_t blocks = 0;
    std::uint64_t framesMixed = 0;
    /// The null output's capture (--audio-capture) and the run's time of its first frame.
    std::optional<std::filesystem::path> capture;
    std::optional<double> captureStartSeconds;
    /// The sync (audio::SyncSummary): blocks measured while a source was audible, resyncs in play, blocks corrected,
    /// frames the rings could not give, frames they could not keep (overflow), the error's p50, p95 and largest.
    std::uint64_t syncBlocks = 0;
    std::uint64_t resyncs = 0;
    std::uint64_t corrections = 0;
    std::uint64_t underrunFrames = 0;
    std::uint64_t overflowFrames = 0;
    double syncP50Milliseconds = 0.0;
    double syncP95Milliseconds = 0.0;
    double syncMaxMilliseconds = 0.0;
    /// Launches: the output suspended and resumed around each, and the device's stops and starts.
    std::uint64_t suspends = 0;
    std::uint64_t deviceStops = 0;
    std::uint64_t deviceStarts = 0;
    /// The device's trouble (audio::AudioEngineStats): starts that failed (after a launch, or a retry), stops it made
    /// by itself, and retries that brought it back (every 2 s while not suspended).
    std::uint64_t deviceFailures = 0;
    std::uint64_t deviceLosses = 0;
    std::uint64_t deviceRecoveries = 0;
    /// Each change of the audible screen, the levels sampled, the audible clip's frames (with a capture), the clips
    /// whose sound was decoded: the first kMaxAudioTimelineEntries of each.
    std::vector<AudioAudibleRecord> audible;
    std::vector<AudioLevelRecord> levels;
    std::vector<AudioPresentedRecord> presented;
    std::vector<AudioClipRecord> clips;

    bool operator==(const AudioRecord&) const = default;
};

/// The GPU time of one pass of the frames' views (I3 step 17b, FrameTiming::recordGpuPasses): the timer query results
/// each frame's last view reported (gl::FrameStats; they arrive a frame or two late), over the frames past the run's
/// warm-up that had one. Names, in this order: "shadow", "depth", "ssao", "scene", "post", "outline" and "total" (the
/// sum of a frame's passes, over the frames that reported any).
struct GpuPassRecord {
    std::string name;
    TimeSummary times;

    bool operator==(const GpuPassRecord&) const = default;
};

/// Frame times (I3 step 0d, FrameTiming): the wall-clock time of each frame, from the end of the previous frame (its
/// buffer swap) to the end of this one, the frames of the run's first warmUpFrames left out (the first frame has no
/// previous one anyway). The measured time, also with --fixed-dt, which replaces only the simulation's step.
struct FrameTimesRecord {
    /// Frames left out at the start of the run (kFrameTimeWarmUpFrames).
    std::uint64_t warmUpFrames = 0;
    /// The frames measured after them: their count and percentiles; absent when there was none.
    std::optional<TimeSummary> times;
    /// The GPU times per pass over the same frames (I3 step 17b): only the passes that reported a time, in
    /// GpuPassRecord's order; empty in runs that drew no view.
    std::vector<GpuPassRecord> gpuPasses;

    bool operator==(const FrameTimesRecord&) const = default;
};

/// One named phase of the run, opened and closed by the code that runs it (AppContext::frameTiming): a refresh, a
/// cover load, an animation, the benchmark path, or a step of an --input-script with "phase". The frames of every
/// opening of the name together.
struct PhaseRecord {
    std::string name;
    /// Times it was opened, and whether it was still open when the run ended.
    std::uint64_t opened = 0;
    bool open = false;
    /// Frames that belonged to it (open when the frame ended, or opened and closed during it), those of the run's
    /// warm-up included.
    std::uint64_t frames = 0;
    /// The times of those frames past the run's warm-up (FrameTimesRecord); absent when there was none.
    std::optional<TimeSummary> times;
    /// The GPU times per pass of those frames (FrameTimesRecord::gpuPasses).
    std::vector<GpuPassRecord> gpuPasses;

    bool operator==(const PhaseRecord&) const = default;
};

/// One CPU section of the main thread (I3 step 0d, core::mainThreadCpuTimers(): core::cpu_section names them): its time
/// per frame, over the frames during which it ran (times.count; every frame of the run, the warm-up included).
struct CpuSectionRecord {
    std::string name;
    TimeSummary times;

    bool operator==(const CpuSectionRecord&) const = default;
};

/// The GPU memory the renderer allocated, by its own accounting per kind (I3 step 0d, gl::GpuMemoryLedger: procedural
/// surfaces, collection arrays, hero textures, video, targets, other textures, buffers): what was allocated when the
/// main loop ended (when the report was written, for a run that ended before), and the peaks of the run.
struct GpuMemoryRecord {
    std::uint64_t totalBytes = 0;
    std::uint64_t peakTotalBytes = 0;
    /// Every kind, in gl::kGpuMemoryKinds order.
    std::array<gl::GpuMemoryUsage, gl::kGpuMemoryKindCount> kinds{};

    [[nodiscard]] const gl::GpuMemoryUsage& usage(gl::GpuMemoryKind kind) const noexcept {
        return kinds[static_cast<std::size_t>(kind)];
    }
    bool operator==(const GpuMemoryRecord&) const = default;
};

/// What `ledger` holds now.
[[nodiscard]] GpuMemoryRecord gpuMemoryRecord(const gl::GpuMemoryLedger& ledger);

/// The sections that ran, from `timers` (core::CpuTimers::summaries(): by name).
[[nodiscard]] std::vector<CpuSectionRecord> cpuSectionRecords(const core::CpuTimers& timers);

/// When the eye entered an area of a scene with areas (SceneAreasRecord::changes).
struct SceneAreaChange {
    /// The frame whose view first packed the area's lights.
    std::uint64_t frame = 0;
    std::string area;

    bool operator==(const SceneAreaChange&) const = default;
};

/// The most SceneAreasRecord::samples a run records.
inline constexpr std::size_t kMaxSceneLightingSamples = 2000;

/// A point or spot light of the lights block at the last view of a scene with areas (I6 step 1).
struct PackedLightRecord {
    /// The scene light's name ("#<index>" for one without).
    std::string name;
    /// Its weight: 0 to 1 while it fades in or out, 1 once in.
    double weight = 1.0;
    /// A portal clips it (a neighbour's light, or a room's own).
    bool clipped = false;

    bool operator==(const PackedLightRecord&) const = default;
};

/// The light of a frame in a scene with areas (I6 step 1): the environment the view used, as eased so far, and its
/// lights block.
struct SceneLightingSample {
    std::uint64_t frame = 0;
    std::string area;
    double exposure = 0.0;
    double ambientIntensity = 0.0;
    double fillIntensity = 0.0;
    /// The point and spot lights packed, and how many of them fade (a weight below 1).
    std::size_t lights = 0;
    std::size_t fading = 0;
    /// The sum of their weights.
    double weight = 0.0;

    bool operator==(const SceneLightingSample&) const = default;
};

/// The areas of a scene with areas (I4 step 3, ADR-0013 §9: a floor's rooms and hall): the area that held the eye at
/// the last view and the lights packed for it, and every change of area during the run. Absent for a scene without
/// areas.
struct SceneAreasRecord {
    /// The scene's areas.
    std::size_t areas = 0;
    /// The area that held the eye at the run's end.
    std::string area;
    /// The scene lights packed into the lights block for it (their names, "#<index>" for one without), in the
    /// scene's order, and the one casting the shadows (empty for none).
    std::vector<std::string> lights;
    std::string keyLight;
    /// The area at the first view, then each change, in order.
    std::vector<SceneAreaChange> changes;
    /// The lights block at the last view (I6 step 1): its point and spot lights in its order, with their weights.
    std::vector<PackedLightRecord> packed;
    /// The light of the last view, and of the first view and of every frame while the environment eased or a light
    /// faded (at most kMaxSceneLightingSamples, the first ones).
    SceneLightingSample last;
    std::vector<SceneLightingSample> samples;

    bool operator==(const SceneAreasRecord&) const = default;
};

/// What the scene shown holds at the run's end (I4b step 4's review): its objects, the boxes of its collision world and
/// its lights (SceneInstance's): a room's static part replaced in place holds what a fresh start on the same plan holds.
struct SceneContentsRecord {
    std::size_t objects = 0;
    std::size_t colliders = 0;
    std::size_t lights = 0;

    bool operator==(const SceneContentsRecord&) const = default;
};

/// What a run of an application built on AppHost did (retroverse.exe, retroverse-asset-preview), written as JSON by
/// --run-report <file.json> when the run ends, whether it succeeded or failed (formatRunReport). Only the host writes
/// it: a command line that does not parse, --help, --version and --write-fixture end before the host starts and write
/// none. Automated checks (the GPU smoke tests of tests/smoke, scripts/gate.ps1) read it back with readRunReport
/// instead of scraping the log.
///
/// The host fills the executable, the directories, the outcome, the OpenGL context, the debug message counts, the
/// --gl-selftest outcome and the frames (AppHost); the application logic adds what it rendered through
/// AppContext::report() (SceneGraphics: the last view's statistics, the instance buffer, the screenshot; RetroverseApp:
/// the walker's final pose, the self-check, the quick catalog and its launches; from I3 the world, the textures'
/// residency and the video, always written and empty until their steps fill them). The host also fills the frame
/// times, the phases, the CPU sections and the GPU memory (I3 step 0d).
struct RunReport {
    /// AppIdentity::name, e.g. "Retroverse".
    std::string application;
    /// core::kVersion and core::kBuildType of the executable.
    std::string version;
    std::string buildType;
    /// Built with the developer overlay (RV_DEV_UI).
    bool developerOverlay = false;
    /// Built to route OpenGL debug output to the log (RV_GL_DEBUG); without it, debugMessages stays empty.
    bool glDebugOutput = false;

    /// The asset root and where it came from (core::toString(core::AssetRootSource): "command line", "RV_ASSET_ROOT",
    /// "source tree", "executable directory"); empty when the directories could not be resolved.
    std::filesystem::path assetRoot;
    std::string assetRootSource;

    /// The process exit code (ExitCode) and its name (toString(ExitCode)).
    int exitCode = 0;
    std::string outcome;
    /// What failed, when the run failed: the initialization error, the first failure the logic reported
    /// (AppContext::failRun), the smoke check or an unexpected error.
    std::optional<std::string> failure;
    /// The initialization failed because no OpenGL 4.5 core profile context could be created (the window's context, or
    /// the GL device that requires 4.5): the machine cannot run Retroverse, which tests may treat as a skip.
    bool openGlUnavailable = false;

    /// The OpenGL context; absent when none was created.
    std::optional<RunReportContext> context;
    /// Debug messages over the whole run; absent without debug output (glDebugOutput off).
    std::optional<DebugMessageCounts> debugMessages;
    /// The outcome of --gl-selftest; absent without it. The messages it provoked are counted in debugMessages as well,
    /// so the run had no unexpected error when debugMessages.high equals glSelfTest.highSeverityCaptured (the rule
    /// exitCodeForCompletedRun applies to --smoke runs).
    std::optional<SelfTestOutcome> glSelfTest;

    /// CommandLineOptions::frameLimit, and the frames presented.
    std::optional<std::uint32_t> frameLimit;
    std::uint64_t framesRendered = 0;

    /// What the last view of the run drew (the window's, or the screenshot's when no window view was drawn); absent
    /// when no view was rendered. Its GPU times are those FrameStats reported for that view.
    std::optional<gl::FrameStats> lastView;
    /// Instance matrices per region of the renderer's instance buffer when the run ended: it starts at
    /// gl::SceneRenderer::kInitialInstanceCapacity and grows when a view needs more. 0 without a renderer.
    std::size_t instanceCapacity = 0;
    /// The --screenshot of the last frame, when it was written.
    std::optional<ScreenshotRecord> screenshot;
    /// The window; absent when none was created.
    std::optional<WindowRecord> window;
    /// Where the walker of retroverse.exe ended, as --spawn takes it (feet, yaw and pitch): a run started with that
    /// --spawn and no input sees the same view up to float rounding (the yaw goes through radians and back, so a few
    /// edges may move by a fraction of a pixel).
    std::optional<SpawnPoint> finalCamera;
    /// The --self-check of the run.
    std::optional<SelfCheckRecord> selfCheck;
    /// The areas of the scene shown (retroverse.exe; absent for a scene without areas).
    std::optional<SceneAreasRecord> sceneAreas;
    /// What the scene shown holds at the end (retroverse.exe; absent without a scene).
    std::optional<SceneContentsRecord> sceneContents;
    /// The quick catalog; absent in runs without the product UI (the model viewer).
    std::optional<CatalogRecord> catalog;
    /// The launches from the quick catalog, in order.
    std::vector<LaunchRecord> launches;
    /// The sections of I3 (above), always written: world and residency filled by RetroverseApp's generated room (I3
    /// step 10b), empty in runs that ask for none; video filled by RetroverseApp (I3 step 11), empty in runs without
    /// the hero CRT's service (the model viewer, the asset preview).
    WorldRecord world;
    ResidencyRecord residency;
    VideoRecord video;
    /// The process priority's changes (I5 step 2); absent in runs that change none (the asset preview).
    std::optional<ProcessPriorityRecord> processPriority;
    /// The screens' CRT looks (I5 step 6); absent in runs that record none (the asset preview).
    std::optional<CrtLooksRecord> crtLooks;
    /// The media viewer and Informazioni (I3 step 14b; the product UI), always written ("screens").
    GameScreensRecord screens;
    /// The directory (I4 step 7b; the product UI), always written ("directory").
    DirectoryRecord directory;
    /// Frame times over the run and per phase, the main thread's CPU sections, the GPU memory (I3 step 0d; the host:
    /// AppHost).
    FrameTimesRecord frameTimes;
    std::vector<PhaseRecord> phases;
    std::vector<CpuSectionRecord> cpuSections;
    GpuMemoryRecord gpuMemory;
    /// The clips' sound (I5 step 5, ADR-0016 §9; "audio", written last): absent in runs without the application's
    /// sound (the asset preview).
    std::optional<AudioRecord> audio;
};

/// The report as JSON text (two-space indented, keys in the order of RunReport, absent optionals left out, paths
/// UTF-8 with '/' separators, floating point values that read back exactly).
[[nodiscard]] std::string formatRunReport(const RunReport& report);

/// Parses formatRunReport() output. Keys it does not know are ignored (a newer writer may add some). Errors
/// (ValidationFailed, context "<context>:<JSON pointer>"): another schemaVersion, a missing required key (every
/// non-optional field but failure), a value of the wrong type or out of range; ParseError for malformed JSON.
[[nodiscard]] Expected<RunReport> parseRunReport(std::string_view json, std::string_view context);

/// Writes formatRunReport() atomically (core::writeTextFileAtomic: parent directories created).
[[nodiscard]] Status writeRunReport(const std::filesystem::path& path, const RunReport& report);

/// Reads and parses a report file (parseRunReport, with the path as context).
[[nodiscard]] Expected<RunReport> readRunReport(const std::filesystem::path& path);

} // namespace rv::app
