#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// Which room's CRT is the hero CRT on a floor (I4 step 6b; ADR-0013 §9, ADR-0012 §7): the CRT of the room that holds
// the eye, with one decoder. The session asks every frame with the area that holds the eye; once the eye has stayed in
// another collection room for kScreenDwellSeconds of the run's time, that room's CRT is bound (the clip of the previous
// one stops as a clip replacement does, its picture override clears, the new CRT plays that room's games). The dwell
// is also the hysteresis at doorways: an eye that crosses a doorway's edge back and forth starts it again each time,
// so nothing is rebound until it stays. In a hall (any area that is no collection room) the last room's CRT stays
// bound, paused while out of view (ScreenPolicy::setVisible). A run that starts in a room (--start-room) has its CRT
// bound from the start; one that starts in the lobby has none until the eye stays in a room.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rv::app {

class ScreenRoomTracker {
public:
    /// `bound`: the room whose CRT is bound from the start (--start-room's), none (the lobby).
    explicit ScreenRoomTracker(std::optional<std::string> bound = std::nullopt);

    /// One frame: the collection room that holds the eye (its id; none in a hall), at `nowSeconds` (the run's
    /// simulated time). Returns the room to bind now, once, when the eye has stayed in it for kScreenDwellSeconds.
    [[nodiscard]] std::optional<std::string> update(std::optional<std::string_view> eyeRoom, double nowSeconds);

    /// The room whose CRT is bound (none yet).
    [[nodiscard]] const std::optional<std::string>& bound() const noexcept { return m_bound; }
    /// The room the eye is in and waits to be bound (its dwell running), if any.
    [[nodiscard]] const std::optional<std::string>& candidate() const noexcept { return m_candidate; }

    /// The CRT of `room` is bound from now on (a floor committed again: its bound room's; none: the lobby's), any dwell
    /// forgotten.
    void reset(std::optional<std::string> room);

private:
    std::optional<std::string> m_bound;
    std::optional<std::string> m_candidate;
    double m_since = 0.0;
};

/// The bound screens found again after the room editor replaced their objects in place (I5 step 3's review,
/// ScreenService::rebind()): for each bound screen (`bound`, by object id, in its order), the index in `found` of the
/// screen with its id, whatever their order. None when the two are not the same screens: another count, an id of one
/// missing from the other, or an id twice.
[[nodiscard]] std::optional<std::vector<std::size_t>> rebindOrder(std::span<const std::string> bound,
                                                                  std::span<const std::string> found);

} // namespace rv::app
