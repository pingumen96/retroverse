#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// The decoder scheduler (I5 step 2, ADR-0016 §2): which clips get one of the few decoder slots, frame by frame, from the
// screens' requests. The screen service of rv_app (screen_service.hpp) asks it every frame with every screen of the
// room that holds the eye, carries its commands out (a decoder started, stopped, paused or resumed) and tells it, per
// slot, whether the stopped decoder's thread is still running and how its frames were presented.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rv::app {

/// Decoders at most, in any profile (NFR-07, ADR-0016 §2: 2 in Balanced, 1 in Low; no High profile with more).
inline constexpr int kMaxScreenDecoders = 2;
/// A slot whose screens are all out of view pauses at once and stops after this long (simulated seconds).
inline constexpr double kScreenStopAfterSeconds = 10.0;
/// A slot changes hands at most this often, unless a `direct` or `focus` request asks for it (simulated seconds).
inline constexpr double kSlotHandOverSeconds = 1.0;
/// Ambient requests enter within proximityMeters and leave beyond it plus this margin.
inline constexpr double kAmbientHysteresisMeters = 0.5;
/// The late-frame drop (ADR-0004's open risk): a slot whose late plus dropped frames exceed kLateFrameRatio of its
/// frames over kLateWindowSeconds drops the budget to one decoder for kLateDropSeconds. A window needs at least
/// kLateMinFrames frames to count.
inline constexpr double kLateWindowSeconds = 4.0;
inline constexpr double kLateFrameRatio = 0.05;
inline constexpr double kLateDropSeconds = 30.0;
inline constexpr std::uint64_t kLateMinFrames = 30;
/// The video uploads of one render frame carry at most this many bytes (ADR-0016 §2: one 720p frame or three 480p
/// ones); a frame over it stays pending one render frame, oldest first. The first upload of a frame always goes.
inline constexpr std::size_t kMaxVideoUploadBytesPerFrame = 4u * 1024u * 1024u;

/// Priority classes (specification §9.3, ADR-0016 §2), the most urgent first.
enum class ScreenClass : std::uint8_t {
    Direct,  ///< the viewer's video, or an inspected cabinet
    Focus,   ///< the screen showing the focused or inspected game
    Room,    ///< the eye's room's hero screen playing its playlist
    Ambient, ///< secondary TVs and attract cabinets, within proximityMeters
};
/// "direct", "focus", "room", "ambient".
[[nodiscard]] std::string_view toString(ScreenClass screenClass) noexcept;

struct ScreenSchedulerSettings {
    /// The budget: 1 or 2 decoders (clamped to that range); also the number of slots.
    int maxDecoders = kMaxScreenDecoders;
    /// Ambient requests count within this distance of the eye (video.json's proximityMeters).
    double proximityMeters = 12.0;
};

/// One screen's request on one frame.
struct ScreenRequest {
    /// The screen (its object id): ties between equal requests go to the smaller id.
    std::string screenId;
    ScreenClass screenClass = ScreenClass::Room;
    /// The clip it wants played (any key naming the file: requests for the same clip share one slot); empty for none.
    std::string clip;
    /// From the eye to the screen, meters: ties within a class go to the nearer screen.
    double distanceMeters = 0.0;
    /// In the view frustum (the screens asked about are those of the area that holds the eye).
    bool visible = true;
};

/// What the service tells about a slot on each frame, before update(): its decoder's thread, and its clock's
/// cumulative counts since the slot opened (media::PresentationStats).
struct SlotFeedback {
    bool threadRunning = false;
    std::uint64_t presented = 0;
    std::uint64_t late = 0;
    std::uint64_t dropped = 0;
};

enum class SlotState : std::uint8_t {
    Free,     ///< no decoder
    Open,     ///< a decoder plays (or pauses on) its clip
    Draining, ///< its decoder was stopped and its thread still runs: the slot opens again only once it ended
};
/// "free", "open", "draining".
[[nodiscard]] std::string_view toString(SlotState state) noexcept;

/// One decoder slot as the scheduler sees it.
struct ScreenSlot {
    SlotState state = SlotState::Free;
    /// Open: its clip. Draining: the clip that stopped.
    std::string clip;
    /// The clip reserved for it, opened once the slot is free (Draining, or Free until the frame's commands).
    std::string next;
    /// Open: every screen showing it is out of view.
    bool paused = false;
    /// When it took its current (or reserved) clip: the hand-over rule's clock.
    double since = 0.0;
    /// Open: since when every screen showing it is out of view.
    std::optional<double> hiddenSince;
    /// Open: the screens that show it, by id.
    std::vector<std::string> screens;
    /// Draining or free: the screens it showed when it stopped. A screen switching clips gets its own slot back (one
    /// thread for it, as with one decoder), not a free one.
    std::vector<std::string> previousScreens;
};

enum class SlotAction : std::uint8_t {
    Open,   ///< start a decoder on `clip` (the slot is free: its previous thread ended)
    Stop,   ///< stop the decoder (its thread ends on its own; the slot drains)
    Pause,  ///< every screen showing it left the view: pause, the frame kept
    Resume, ///< a screen showing it is in view again
};
/// "open", "stop", "pause", "resume".
[[nodiscard]] std::string_view toString(SlotAction action) noexcept;

/// Why a slot stopped (SlotCommand::reason): "unwanted" (no screen asks for its clip), "needed" (another request took
/// it), "out-of-view" (kScreenStopAfterSeconds out of view), "budget" (the late-frame drop), and the service's own:
/// "launch", "unbind", "shutdown".
struct SlotCommand {
    SlotAction action = SlotAction::Open;
    std::size_t slot = 0;
    std::string clip;
    std::string reason;

    bool operator==(const SlotCommand&) const = default;
};

/// Where a screen's request stands after update().
struct ScreenAssignment {
    /// The slot showing its clip (Open), or reserved for it (Draining or about to open); none without a slot.
    std::optional<std::size_t> slot;
    /// The slot is open on its clip: its texture is the screen's picture once a frame came.
    bool playing = false;
    /// The slot is reserved for its clip and opens it once the slot's previous thread ended.
    bool waiting = false;

    bool operator==(const ScreenAssignment&) const = default;
};

struct ScheduleResult {
    /// The frame's commands, in the order to carry them out (stops first, then opens, pauses and resumes).
    std::vector<SlotCommand> commands;
    /// One per request, in the requests' order.
    std::vector<ScreenAssignment> screens;
};

/// A late-frame drop to one decoder (the run report's).
struct BudgetDrop {
    double atSeconds = 0.0;
    std::size_t slot = 0;
    std::string clip;
    /// Late plus dropped frames over the window's frames.
    double ratio = 0.0;
    /// The budget comes back at this time.
    double untilSeconds = 0.0;

    bool operator==(const BudgetDrop&) const = default;
};

/// The decoder scheduler (ADR-0016 §2), deterministic for the same requests, feedback and times:
///  - Requests for the same clip share one slot. A clip's rank: visible before out of view, then its best class, then
///    its nearest screen, then the smallest screen id.
///  - Ambient requests count within proximityMeters of the eye, and stop counting only beyond it plus
///    kAmbientHysteresisMeters.
///  - The budget's best-ranked clips have slots. A clip out of view keeps a slot it has, paused, and takes only a free
///    one (never one another clip holds), until kScreenStopAfterSeconds out of view (counted from when its last screen
///    left the view) or until a request in view needs it.
///  - A slot changes hands (a request takes it from a clip still asked for) at most once every kSlotHandOverSeconds
///    unless the request taking it is `direct` or `focus`. A clip no screen asks for any more stops at once.
///  - A stopped slot drains: it opens its next clip only once the service reports its thread ended, so that there are
///    never more decoder threads than slots, nor more open decoders than the budget. A clip takes the slot one of its
///    screens showed before (a screen switching clips waits for its own slot), else a free one, else a draining one.
///  - The late-frame drop: a slot over kLateFrameRatio over kLateWindowSeconds lowers the budget to 1 for
///    kLateDropSeconds; the lowest-ranked clip loses its slot.
class ScreenScheduler {
public:
    explicit ScreenScheduler(ScreenSchedulerSettings settings = {});

    /// One frame at `nowSeconds` (the run's simulated time): `feedback` holds one entry per slot (fewer: the rest
    /// count as no thread and no frames).
    [[nodiscard]] ScheduleResult update(double nowSeconds, std::span<const ScreenRequest> requests,
                                        std::span<const SlotFeedback> feedback);

    /// Every open slot stops at once, `reason` given (a launch, an unbind, the end), and every reservation is dropped.
    [[nodiscard]] std::vector<SlotCommand> stopAll(std::string_view reason);
    /// The decoder of `slot` could not start (an Open command failed): the slot is free again at once.
    void openFailed(std::size_t slot);

    /// The budget at `nowSeconds`: maxDecoders, or 1 during a late-frame drop.
    [[nodiscard]] int budget(double nowSeconds) const noexcept;
    [[nodiscard]] std::span<const ScreenSlot> slots() const noexcept { return m_slots; }
    [[nodiscard]] const std::vector<BudgetDrop>& drops() const noexcept { return m_drops; }
    [[nodiscard]] const ScreenSchedulerSettings& settings() const noexcept { return m_settings; }
    /// Slots open now.
    [[nodiscard]] std::size_t openSlots() const noexcept;
    /// Times a slot was taken from a clip still asked for.
    [[nodiscard]] std::uint64_t handOvers() const noexcept { return m_handOvers; }

private:
    struct LateSample {
        double atSeconds = 0.0;
        std::uint64_t frames = 0;
        std::uint64_t late = 0;
    };
    struct SlotStats {
        std::uint64_t presented = 0;
        std::uint64_t late = 0;
        std::uint64_t dropped = 0;
        std::vector<LateSample> window;
    };

    void stop(std::size_t slot, std::string_view reason, std::vector<SlotCommand>& stops);
    void watchLateFrames(double nowSeconds, std::span<const SlotFeedback> feedback);

    ScreenSchedulerSettings m_settings;
    std::vector<ScreenSlot> m_slots;
    std::vector<SlotStats> m_stats;
    /// The ambient screens counting now (the hysteresis), by id, sorted.
    std::vector<std::string> m_ambientIn;
    /// Since when each clip asked for has had every screen out of view (none for a clip in view).
    std::map<std::string, double, std::less<>> m_hiddenSince;
    std::optional<double> m_dropUntil;
    std::vector<BudgetDrop> m_drops;
    std::uint64_t m_handOvers = 0;
};

} // namespace rv::app
