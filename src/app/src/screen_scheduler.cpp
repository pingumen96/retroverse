#include <rv/app/screen_scheduler.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

namespace rv::app {
namespace {

/// Seconds a wait may fall short by and still count: fixed steps of 1/60 s add up only up to rounding.
constexpr double kTimeTolerance = 1e-6;

/// The clips asked for on a frame, with what ranks them.
struct Demand {
    std::string clip;
    bool visible = false;
    ScreenClass screenClass = ScreenClass::Ambient;
    double distance = 0.0;
    std::string screenId;
    std::vector<std::string> screens;

    /// Visible first, then the class, the distance and the screen id.
    [[nodiscard]] auto key() const { return std::tie(screenClass, distance, screenId); }
    [[nodiscard]] bool before(const Demand& other) const {
        if (visible != other.visible) {
            return visible;
        }
        return key() < other.key();
    }
};

/// A request's own rank, as a Demand made of it alone.
Demand demandOf(const ScreenRequest& request) {
    Demand demand;
    demand.clip = request.clip;
    demand.visible = request.visible;
    demand.screenClass = request.screenClass;
    demand.distance = request.distanceMeters;
    demand.screenId = request.screenId;
    return demand;
}

} // namespace

std::string_view toString(ScreenClass screenClass) noexcept {
    switch (screenClass) {
    case ScreenClass::Direct:
        return "direct";
    case ScreenClass::Focus:
        return "focus";
    case ScreenClass::Room:
        return "room";
    case ScreenClass::Ambient:
        return "ambient";
    }
    return "unknown";
}

std::string_view toString(SlotState state) noexcept {
    switch (state) {
    case SlotState::Free:
        return "free";
    case SlotState::Open:
        return "open";
    case SlotState::Draining:
        return "draining";
    }
    return "unknown";
}

std::string_view toString(SlotAction action) noexcept {
    switch (action) {
    case SlotAction::Open:
        return "open";
    case SlotAction::Stop:
        return "stop";
    case SlotAction::Pause:
        return "pause";
    case SlotAction::Resume:
        return "resume";
    }
    return "unknown";
}

ScreenScheduler::ScreenScheduler(ScreenSchedulerSettings settings) : m_settings(settings) {
    m_settings.maxDecoders = std::clamp(m_settings.maxDecoders, 1, kMaxScreenDecoders);
    m_slots.resize(static_cast<std::size_t>(m_settings.maxDecoders));
    m_stats.resize(m_slots.size());
}

int ScreenScheduler::budget(double nowSeconds) const noexcept {
    if (m_dropUntil && nowSeconds + kTimeTolerance < *m_dropUntil) {
        return 1;
    }
    return m_settings.maxDecoders;
}

std::size_t ScreenScheduler::openSlots() const noexcept {
    return static_cast<std::size_t>(std::count_if(m_slots.begin(), m_slots.end(), [](const ScreenSlot& slot) {
        return slot.state == SlotState::Open;
    }));
}

void ScreenScheduler::stop(std::size_t index, std::string_view reason, std::vector<SlotCommand>& stops) {
    ScreenSlot& slot = m_slots[index];
    if (slot.state != SlotState::Open) {
        return;
    }
    stops.push_back(SlotCommand{SlotAction::Stop, index, slot.clip, std::string(reason)});
    slot.state = SlotState::Draining;
    slot.paused = false;
    slot.hiddenSince.reset();
    slot.previousScreens = std::move(slot.screens);
    slot.screens.clear();
    m_stats[index] = SlotStats{};
}

std::vector<SlotCommand> ScreenScheduler::stopAll(std::string_view reason) {
    std::vector<SlotCommand> stops;
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        stop(index, reason, stops);
        m_slots[index].next.clear();
    }
    return stops;
}

void ScreenScheduler::openFailed(std::size_t index) {
    if (index >= m_slots.size()) {
        return;
    }
    ScreenSlot& slot = m_slots[index];
    slot = ScreenSlot{};
    m_stats[index] = SlotStats{};
}

void ScreenScheduler::watchLateFrames(double nowSeconds, std::span<const SlotFeedback> feedback) {
    if (m_dropUntil && nowSeconds + kTimeTolerance >= *m_dropUntil) {
        m_dropUntil.reset();
    }
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        const ScreenSlot& slot = m_slots[index];
        SlotStats& stats = m_stats[index];
        if (slot.state != SlotState::Open) {
            stats = SlotStats{};
            continue;
        }
        const SlotFeedback counts = index < feedback.size() ? feedback[index] : SlotFeedback{};
        const std::uint64_t frames = counts.presented + counts.dropped;
        const std::uint64_t late = counts.late + counts.dropped;
        const std::uint64_t previousFrames = stats.presented + stats.dropped;
        const std::uint64_t previousLate = stats.late + stats.dropped;
        const std::uint64_t newFrames = frames >= previousFrames ? frames - previousFrames : frames;
        const std::uint64_t newLate = late >= previousLate ? late - previousLate : late;
        stats.presented = counts.presented;
        stats.late = counts.late;
        stats.dropped = counts.dropped;
        if (slot.paused) {
            stats.window.clear(); // the window covers playing time only
            continue;
        }
        stats.window.push_back(LateSample{nowSeconds, newFrames, newLate});
        // The window: the samples of the last kLateWindowSeconds, its first sample the start of its span.
        while (stats.window.size() > 1 && stats.window[1].atSeconds <= nowSeconds - kLateWindowSeconds + kTimeTolerance) {
            stats.window.erase(stats.window.begin());
        }
        if (m_dropUntil || m_settings.maxDecoders < 2 ||
            nowSeconds - stats.window.front().atSeconds + kTimeTolerance < kLateWindowSeconds) {
            continue;
        }
        std::uint64_t windowFrames = 0;
        std::uint64_t windowLate = 0;
        for (std::size_t sample = 1; sample < stats.window.size(); ++sample) {
            windowFrames += stats.window[sample].frames;
            windowLate += stats.window[sample].late;
        }
        if (windowFrames < kLateMinFrames) {
            continue;
        }
        const double ratio = static_cast<double>(windowLate) / static_cast<double>(windowFrames);
        if (ratio <= kLateFrameRatio) {
            continue;
        }
        m_dropUntil = nowSeconds + kLateDropSeconds;
        m_drops.push_back(BudgetDrop{nowSeconds, index, slot.clip, ratio, *m_dropUntil});
        for (SlotStats& other : m_stats) {
            other.window.clear();
        }
    }
}

ScheduleResult ScreenScheduler::update(double nowSeconds, std::span<const ScreenRequest> requests,
                                       std::span<const SlotFeedback> feedback) {
    ScheduleResult result;
    std::vector<SlotCommand> stops;
    std::vector<SlotCommand> opens;
    std::vector<SlotCommand> others;

    // A stopped slot is free once its thread ended (never before: one thread per slot).
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        ScreenSlot& slot = m_slots[index];
        const bool running = index < feedback.size() && feedback[index].threadRunning;
        if (slot.state == SlotState::Draining && !running) {
            slot.state = SlotState::Free;
            slot.clip.clear();
        }
    }
    watchLateFrames(nowSeconds, feedback);
    const auto budget = static_cast<std::size_t>(this->budget(nowSeconds));

    // The clips asked for, each with its screens; ambient requests within their proximity (the hysteresis).
    std::vector<std::string> ambientIn;
    std::vector<bool> counted(requests.size(), false);
    std::vector<Demand> demands;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const ScreenRequest& request = requests[index];
        if (request.clip.empty()) {
            continue;
        }
        if (request.screenClass == ScreenClass::Ambient) {
            const bool wasIn = std::binary_search(m_ambientIn.begin(), m_ambientIn.end(), request.screenId);
            const double reach = m_settings.proximityMeters + (wasIn ? kAmbientHysteresisMeters : 0.0);
            if (request.distanceMeters > reach) {
                continue;
            }
            ambientIn.push_back(request.screenId);
        }
        counted[index] = true;
        const Demand own = demandOf(request);
        const auto found = std::find_if(demands.begin(), demands.end(),
                                        [&](const Demand& demand) { return demand.clip == request.clip; });
        if (found == demands.end()) {
            demands.push_back(own);
            demands.back().screens.push_back(request.screenId);
            continue;
        }
        found->screens.push_back(request.screenId);
        const bool visible = found->visible || own.visible;
        if (own.before(*found)) {
            std::vector<std::string> screens = std::move(found->screens);
            *found = own;
            found->screens = std::move(screens);
        }
        found->visible = visible;
    }
    std::sort(ambientIn.begin(), ambientIn.end());
    m_ambientIn = std::move(ambientIn);
    std::sort(demands.begin(), demands.end(), [](const Demand& a, const Demand& b) { return a.before(b); });
    for (Demand& demand : demands) {
        std::sort(demand.screens.begin(), demand.screens.end());
    }

    // Since when each clip has been out of view (its stop after kScreenStopAfterSeconds counts from there).
    std::map<std::string, double, std::less<>> hiddenSince;
    for (const Demand& demand : demands) {
        if (!demand.visible) {
            const auto previous = m_hiddenSince.find(demand.clip);
            hiddenSince.emplace(demand.clip, previous != m_hiddenSince.end() ? previous->second : nowSeconds);
        }
    }
    m_hiddenSince = std::move(hiddenSince);
    const auto hiddenTooLong = [&](const std::string& clip) {
        const auto hidden = m_hiddenSince.find(clip);
        return hidden != m_hiddenSince.end() &&
               nowSeconds - hidden->second + kTimeTolerance >= kScreenStopAfterSeconds;
    };

    const auto demandIndex = [&](const std::string& clip) -> std::optional<std::size_t> {
        if (clip.empty()) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < demands.size(); ++index) {
            if (demands[index].clip == clip) {
                return index;
            }
        }
        return std::nullopt;
    };
    const auto holder = [&](const std::string& clip) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < m_slots.size(); ++index) {
            const ScreenSlot& slot = m_slots[index];
            if ((slot.state == SlotState::Open && slot.clip == clip) || slot.next == clip) {
                return index;
            }
        }
        return std::nullopt;
    };

    // A clip no screen asks for any more stops at once; a reservation nobody asks for goes.
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        ScreenSlot& slot = m_slots[index];
        if (slot.state == SlotState::Open && !demandIndex(slot.clip)) {
            stop(index, "unwanted", stops);
        }
        if (!slot.next.empty() && !demandIndex(slot.next)) {
            slot.next.clear();
        }
    }

    // The budget's best clips. Out of view, a clip keeps its slot or takes a free one (below), until
    // kScreenStopAfterSeconds.
    std::vector<std::size_t> desired;
    for (std::size_t index = 0; index < demands.size() && desired.size() < budget; ++index) {
        const Demand& demand = demands[index];
        if (!demand.visible && hiddenTooLong(demand.clip)) {
            if (const std::optional<std::size_t> held = holder(demand.clip); held && m_slots[*held].next == demand.clip) {
                m_slots[*held].next.clear(); // a reservation out of view too long: given up
            }
            continue;
        }
        desired.push_back(index);
    }
    const auto isDesired = [&](const std::string& clip) {
        const std::optional<std::size_t> index = demandIndex(clip);
        return index && std::find(desired.begin(), desired.end(), *index) != desired.end();
    };

    // Out of view for kScreenStopAfterSeconds: stopped, the screens on their stills.
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        const ScreenSlot& slot = m_slots[index];
        if (slot.state == SlotState::Open && hiddenTooLong(slot.clip)) {
            stop(index, "out-of-view", stops);
        }
    }
    // Over the budget (the late-frame drop): the lowest-ranked open clips lose their slots.
    while (openSlots() > budget) {
        std::optional<std::size_t> worst;
        std::size_t worstRank = 0;
        for (std::size_t index = 0; index < m_slots.size(); ++index) {
            const ScreenSlot& slot = m_slots[index];
            if (slot.state != SlotState::Open) {
                continue;
            }
            const std::size_t rank = isDesired(slot.clip) ? demandIndex(slot.clip).value_or(0) : demands.size() + 1;
            if (!worst || rank >= worstRank) {
                worst = index;
                worstRank = rank;
            }
        }
        stop(*worst, "budget", stops);
    }

    // A slot for each best clip without one: the one a screen of it showed before (free, then still draining), then a
    // free one, then a draining one, then one taken from a clip not among the best (the lowest-ranked), at most once a
    // second unless the request is direct or focus.
    for (const std::size_t index : desired) {
        const Demand& demand = demands[index];
        if (holder(demand.clip)) {
            continue;
        }
        const auto showedBefore = [&](const ScreenSlot& slot) {
            return std::any_of(slot.previousScreens.begin(), slot.previousScreens.end(), [&](const std::string& id) {
                return std::binary_search(demand.screens.begin(), demand.screens.end(), id);
            });
        };
        std::optional<std::size_t> pick;
        for (const bool own : {true, false}) {
            for (const SlotState state : {SlotState::Free, SlotState::Draining}) {
                for (std::size_t slot = 0; slot < m_slots.size() && !pick; ++slot) {
                    const ScreenSlot& candidate = m_slots[slot];
                    if (candidate.state == state && candidate.next.empty() && (!own || showedBefore(candidate))) {
                        pick = slot;
                    }
                }
            }
        }
        if (!pick) {
            std::optional<std::size_t> victim;
            std::size_t victimRank = 0;
            for (std::size_t slot = 0; slot < m_slots.size(); ++slot) {
                const ScreenSlot& candidate = m_slots[slot];
                const std::string& held = !candidate.next.empty() ? candidate.next : candidate.clip;
                if (held.empty() || isDesired(held)) {
                    continue;
                }
                const std::size_t rank = demandIndex(held).value_or(demands.size());
                if (!victim || rank > victimRank) {
                    victim = slot;
                    victimRank = rank;
                }
            }
            if (!victim || !demand.visible) {
                continue; // out of view, a clip never takes a slot another clip holds
            }
            const bool urgent = demand.screenClass == ScreenClass::Direct || demand.screenClass == ScreenClass::Focus;
            if (!urgent && nowSeconds - m_slots[*victim].since + kTimeTolerance < kSlotHandOverSeconds) {
                continue; // the slot changed hands less than a second ago
            }
            ++m_handOvers;
            stop(*victim, "needed", stops);
            pick = victim;
        }
        m_slots[*pick].next = demand.clip;
        m_slots[*pick].since = nowSeconds;
    }

    // Free slots open what is reserved for them.
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        ScreenSlot& slot = m_slots[index];
        if (slot.state != SlotState::Free || slot.next.empty()) {
            continue;
        }
        slot.state = SlotState::Open;
        slot.clip = std::exchange(slot.next, std::string());
        slot.paused = false;
        slot.hiddenSince.reset();
        slot.previousScreens.clear();
        m_stats[index] = SlotStats{};
        opens.push_back(SlotCommand{SlotAction::Open, index, slot.clip, "wanted"});
    }

    // The open slots' screens, and their pause while every one of them is out of view.
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        ScreenSlot& slot = m_slots[index];
        if (slot.state != SlotState::Open) {
            continue;
        }
        const std::optional<std::size_t> demand = demandIndex(slot.clip);
        slot.screens = demand ? demands[*demand].screens : std::vector<std::string>{};
        const bool visible = demand && demands[*demand].visible;
        if (!visible && !slot.paused) {
            slot.paused = true;
            const auto hidden = m_hiddenSince.find(slot.clip);
            slot.hiddenSince = hidden != m_hiddenSince.end() ? hidden->second : nowSeconds;
            others.push_back(SlotCommand{SlotAction::Pause, index, slot.clip, "out-of-view"});
        } else if (visible && slot.paused) {
            slot.paused = false;
            slot.hiddenSince.reset();
            others.push_back(SlotCommand{SlotAction::Resume, index, slot.clip, "in-view"});
        }
    }

    result.screens.resize(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (!counted[index]) {
            continue;
        }
        ScreenAssignment& assignment = result.screens[index];
        for (std::size_t slot = 0; slot < m_slots.size(); ++slot) {
            if (m_slots[slot].state == SlotState::Open && m_slots[slot].clip == requests[index].clip) {
                assignment.slot = slot;
                assignment.playing = true;
            } else if (m_slots[slot].next == requests[index].clip) {
                assignment.slot = slot;
                assignment.waiting = true;
            }
        }
    }
    result.commands = std::move(stops);
    result.commands.insert(result.commands.end(), opens.begin(), opens.end());
    result.commands.insert(result.commands.end(), others.begin(), others.end());
    return result;
}

} // namespace rv::app
