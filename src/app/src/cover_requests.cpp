#include <rv/app/cover_requests.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/geometric.hpp>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/rvtex.hpp>

namespace rv::app {
namespace {

/// The smallest distance a projected size divides by: an item the eye stands in still ranks by its size.
constexpr float kMinimumDistance = 0.01f;

[[nodiscard]] bool samePlanes(const scene::Frustum& a, const scene::Frustum& b) noexcept {
    for (std::size_t index = 0; index < a.planes.size(); ++index) {
        if (a.planes[index].normal != b.planes[index].normal || a.planes[index].d != b.planes[index].d) {
            return false;
        }
    }
    return true;
}

} // namespace

CoverView CoverView::of(const scene::Camera& camera, std::optional<float> otherAspect) {
    CoverView view;
    view.eye = camera.position();
    view.frusta.push_back(camera.frustum());
    if (otherAspect && std::isfinite(*otherAspect) && *otherAspect > 0.0f &&
        std::abs(*otherAspect - camera.aspect()) > 1e-4f) {
        scene::Camera other = camera;
        other.setAspect(*otherAspect);
        view.frusta.push_back(other.frustum());
    }
    return view;
}

bool CoverView::operator==(const CoverView& other) const noexcept {
    if (eye != other.eye || frusta.size() != other.frusta.size()) {
        return false;
    }
    for (std::size_t index = 0; index < frusta.size(); ++index) {
        if (!samePlanes(frusta[index], other.frusta[index])) {
            return false;
        }
    }
    return true;
}

std::string_view toString(CoverRank rank) noexcept {
    switch (rank) {
    case CoverRank::Focused:
        return "focused";
    case CoverRank::InView:
        return "in-view";
    case CoverRank::Rest:
        return "rest";
    }
    return "unknown";
}

bool CoverPriority::operator<(const CoverPriority& other) const noexcept {
    if (rank != other.rank) {
        return rank < other.rank;
    }
    if (order != other.order) {
        return order < other.order;
    }
    return item < other.item;
}

bool coverInView(const core::Aabb& bounds, const CoverView& view) noexcept {
    if (bounds.isEmpty()) {
        return false;
    }
    return std::any_of(view.frusta.begin(), view.frusta.end(),
                       [&](const scene::Frustum& frustum) { return frustum.intersects(bounds); });
}

float coverDistance(const core::Aabb& bounds, const glm::vec3& eye) noexcept {
    if (bounds.isEmpty()) {
        return 0.0f;
    }
    const glm::vec3 nearest = glm::clamp(eye, bounds.min, bounds.max);
    return glm::length(nearest - eye);
}

CoverPriority coverPriority(std::size_t item, const core::Aabb& bounds, const CoverView& view, bool focused) noexcept {
    CoverPriority priority;
    priority.item = item;
    const float distance = coverDistance(bounds, view.eye);
    if (focused) {
        priority.rank = CoverRank::Focused;
        priority.order = distance;
        return priority;
    }
    if (coverInView(bounds, view)) {
        priority.rank = CoverRank::InView;
        const float radius = bounds.isEmpty() ? 0.0f : glm::length(bounds.halfExtents());
        priority.order = -radius / std::max(distance, kMinimumDistance);
        return priority;
    }
    priority.rank = CoverRank::Rest;
    priority.order = distance;
    return priority;
}

// --- CoverSchedule ---------------------------------------------------------------------------------------------------

CoverSchedule::CoverSchedule(std::vector<core::Aabb> bounds)
    : m_bounds(std::move(bounds)), m_stage(m_bounds.size(), Stage::Waiting), m_layersLeft(m_bounds.size(), 0),
      m_inView(m_bounds.size(), 0), m_arrived(m_bounds.size(), 0), m_priority(m_bounds.size()) {
    m_waiting.reserve(m_bounds.size());
    for (std::size_t item = 0; item < m_bounds.size(); ++item) {
        m_priority[item] = priorityOf(item);
    }
    // Without a view the items keep their order (the layout's) until the first setView(): best last.
    for (std::size_t item = m_bounds.size(); item > 0; --item) {
        m_waiting.push_back(item - 1);
    }
    m_waitingSorted = true;
    m_madeSorted = true;
}

CoverPriority CoverSchedule::priorityOf(std::size_t item) const {
    if (!m_view) {
        CoverPriority priority;
        priority.rank = m_focused == item ? CoverRank::Focused : CoverRank::Rest;
        priority.order = static_cast<float>(item);
        priority.item = item;
        return priority;
    }
    return coverPriority(item, m_bounds[item], *m_view, m_focused == item);
}

void CoverSchedule::rank() {
    m_inViewCount = 0;
    for (std::size_t item = 0; item < m_bounds.size(); ++item) {
        m_priority[item] = priorityOf(item);
        const bool shown = m_view && coverInView(m_bounds[item], *m_view);
        m_inView[item] = shown ? 1 : 0;
        m_inViewCount += shown ? 1 : 0;
    }
    m_waitingSorted = false;
    m_madeSorted = false;
}

void CoverSchedule::setView(const CoverView& view) {
    if (m_view && *m_view == view) {
        return;
    }
    m_view = view;
    rank();
}

void CoverSchedule::setFocused(std::optional<std::size_t> item) {
    if (item && *item >= m_bounds.size()) {
        item.reset();
    }
    if (item == m_focused) {
        return;
    }
    const std::optional<std::size_t> previous = std::exchange(m_focused, item);
    for (const std::optional<std::size_t>& changed : {previous, item}) {
        if (changed) {
            m_priority[*changed] = priorityOf(*changed);
        }
    }
    m_waitingSorted = false;
    m_madeSorted = false;
}

std::vector<std::size_t> CoverSchedule::takeToMake(std::size_t count) {
    std::vector<std::size_t> taken;
    if (m_paused || count == 0 || m_waiting.empty()) {
        return taken;
    }
    if (!m_waitingSorted) {
        // Best last, so that the best are taken from the back.
        std::sort(m_waiting.begin(), m_waiting.end(),
                  [&](std::size_t a, std::size_t b) { return m_priority[b] < m_priority[a]; });
        m_waitingSorted = true;
    }
    while (taken.size() < count && !m_waiting.empty()) {
        const std::size_t item = m_waiting.back();
        m_waiting.pop_back();
        m_stage[item] = Stage::Making;
        ++m_makingCount;
        taken.push_back(item);
    }
    return taken;
}

bool CoverSchedule::made(std::size_t item, unsigned layers) {
    if (item >= m_bounds.size() || m_stage[item] != Stage::Making) {
        return false;
    }
    --m_makingCount;
    if (layers == 0) {
        finish(item);
        return true;
    }
    m_stage[item] = Stage::Made;
    m_layersLeft[item] = layers;
    m_madeLayers += layers;
    m_made.push_back(item);
    m_madeSorted = false;
    return true;
}

std::vector<std::size_t> CoverSchedule::takeToUpload(std::size_t maxLayers) {
    std::vector<std::size_t> taken;
    if (m_paused || maxLayers == 0 || m_made.empty()) {
        return taken;
    }
    if (!m_madeSorted) {
        std::sort(m_made.begin(), m_made.end(),
                  [&](std::size_t a, std::size_t b) { return m_priority[b] < m_priority[a]; });
        m_madeSorted = true;
    }
    std::size_t layers = 0;
    while (!m_made.empty()) {
        const std::size_t item = m_made.back();
        if (!taken.empty() && layers + m_layersLeft[item] > maxLayers) {
            break;
        }
        m_made.pop_back();
        layers += m_layersLeft[item];
        m_madeLayers -= m_layersLeft[item];
        m_stage[item] = Stage::Uploading;
        taken.push_back(item);
        if (layers >= maxLayers) {
            break;
        }
    }
    return taken;
}

void CoverSchedule::layerDone(std::size_t item) {
    if (item >= m_bounds.size() || m_stage[item] != Stage::Uploading) {
        return;
    }
    if (m_layersLeft[item] > 0) {
        --m_layersLeft[item];
    }
    if (m_layersLeft[item] == 0) {
        finish(item);
    }
}

void CoverSchedule::finish(std::size_t item) {
    m_stage[item] = Stage::Done;
    m_layersLeft[item] = 0;
    ++m_doneCount;
    if (m_arrived[item] == 0) {
        m_arrived[item] = 1;
        CoverArrival arrival;
        arrival.item = item;
        arrival.rank = m_priority[item].rank;
        arrival.distance = m_view ? coverDistance(m_bounds[item], m_view->eye) : 0.0f;
        m_arrivals.push_back(arrival);
    }
}

void CoverSchedule::giveBack(std::size_t item) {
    if (item >= m_bounds.size() || m_stage[item] != Stage::Making) {
        return;
    }
    --m_makingCount;
    m_stage[item] = Stage::Waiting;
    m_waiting.push_back(item);
    m_waitingSorted = false;
}

bool CoverSchedule::requeue(std::size_t item) {
    if (item >= m_bounds.size() || m_stage[item] != Stage::Done) {
        return false;
    }
    m_stage[item] = Stage::Waiting;
    --m_doneCount;
    m_waiting.push_back(item);
    m_waitingSorted = false;
    return true;
}

bool CoverSchedule::viewDone() const noexcept {
    if (m_focused && m_stage[*m_focused] != Stage::Done) {
        return false;
    }
    return inViewPending() == 0;
}

std::size_t CoverSchedule::inViewPending() const noexcept {
    std::size_t pending = 0;
    for (std::size_t item = 0; item < m_bounds.size(); ++item) {
        pending += m_inView[item] != 0 && m_stage[item] != Stage::Done ? 1 : 0;
    }
    return pending;
}

// --- Hero pictures ---------------------------------------------------------------------------------------------------

glm::ivec2 heroPictureSize(int sourceWidth, int sourceHeight, int maxSide, const assets::LabelAspect& label) noexcept {
    const assets::LayerSize front = assets::layerSize(assets::TextureClass::Front, assets::TextureFormat::Bc1Srgb);
    const double aspect = label.isLayer() ? static_cast<double>(front.width) / static_cast<double>(front.height)
                                          : static_cast<double>(label.width) / static_cast<double>(label.height);
    const int cap = std::max(16, maxSide);
    // As wide as the cover needs to keep its pixels: its width, or its height at the label's aspect.
    const double wanted = std::max(static_cast<double>(std::max(1, sourceWidth)),
                                   static_cast<double>(std::max(1, sourceHeight)) * aspect);
    const int width = std::clamp(static_cast<int>(std::lround(wanted)), 16, cap);
    const int height = std::max(12, static_cast<int>(std::lround(width / aspect)));
    return {width, height};
}

Expected<assets::ImageData> makeHeroPicture(std::span<const std::byte> bytes, std::string_view context, int maxSide,
                                            const assets::LabelAspect& label) {
    auto image = assets::decodeImage(bytes, 0, context);
    if (!image) {
        return propagate(image.error());
    }
    // At the label's shape the picture is fitted to its own size: the quad shows it unstretched.
    const glm::ivec2 size = heroPictureSize(image->width, image->height, maxSide, label);
    auto fitted = assets::fitCover(*image, size.x, size.y);
    if (!fitted) {
        return propagate(fitted.error());
    }
    return std::move(fitted->picture);
}

// --- HeroLru ---------------------------------------------------------------------------------------------------------

HeroLru::HeroLru(std::size_t capacity) : m_capacity(std::max<std::size_t>(1, capacity)) {}

bool HeroLru::contains(std::string_view key) const noexcept {
    return std::find(m_keys.begin(), m_keys.end(), key) != m_keys.end();
}

bool HeroLru::touch(std::string_view key) {
    const auto found = std::find(m_keys.begin(), m_keys.end(), key);
    if (found == m_keys.end()) {
        return false;
    }
    std::rotate(found, found + 1, m_keys.end());
    return true;
}

std::optional<std::string> HeroLru::insert(std::string key, std::span<const std::string> pinned) {
    if (touch(key)) {
        return std::nullopt;
    }
    std::optional<std::string> evicted;
    if (m_keys.size() >= m_capacity) {
        const auto victim = std::find_if(m_keys.begin(), m_keys.end(), [&](const std::string& held) {
            return std::find(pinned.begin(), pinned.end(), held) == pinned.end();
        });
        if (victim == m_keys.end()) {
            return key; // every key is pinned: nothing makes room
        }
        evicted = std::move(*victim);
        m_keys.erase(victim);
    }
    m_keys.push_back(std::move(key));
    return evicted;
}

bool HeroLru::erase(std::string_view key) {
    const auto found = std::find(m_keys.begin(), m_keys.end(), key);
    if (found == m_keys.end()) {
        return false;
    }
    m_keys.erase(found);
    return true;
}

} // namespace rv::app
