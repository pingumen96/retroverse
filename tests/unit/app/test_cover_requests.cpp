// The cover loader's order and the hero textures' set (I3 step 12, rv/app/cover_requests.hpp): an item's priority (the
// focused item first, the items in view by projected size, the rest by distance), the schedule of every item's stages
// (made and uploaded best first, ranked again when the view or the focus changes, nothing handed out while a launched
// game runs, the arrival order recorded, retries), the least-recently-used set of 8 with the shown texture pinned, and
// the hero picture's size and making.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/cover_requests.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/camera.hpp>

using rv::app::CoverPriority;
using rv::app::CoverRank;
using rv::app::CoverSchedule;
using rv::app::CoverView;
using rv::app::HeroLru;

namespace {

/// A cartridge-sized box (12.5 x 8.5 x 2 cm) centred at `center`.
rv::core::Aabb box(glm::vec3 center, float scale = 1.0f) {
    return rv::core::Aabb::fromCenterHalfExtents(center, glm::vec3(0.0625f, 0.0425f, 0.01f) * scale);
}

/// The default camera at the origin looking at -Z (16:9), turned by `yawDegrees`.
CoverView viewFrom(glm::vec3 eye = glm::vec3(0.0f), float yawDegrees = 0.0f) {
    rv::scene::Camera camera;
    camera.setPose(eye, glm::radians(yawDegrees), 0.0f);
    return CoverView::of(camera);
}

/// Every item taken to make, in the order the schedule hands them out.
std::vector<std::size_t> makeOrder(CoverSchedule& schedule) {
    std::vector<std::size_t> order;
    for (std::vector<std::size_t> taken = schedule.takeToMake(1); !taken.empty(); taken = schedule.takeToMake(1)) {
        order.push_back(taken.front());
    }
    return order;
}

/// Makes and uploads `item` whole (`layers` layers).
void arrive(CoverSchedule& schedule, std::size_t item, unsigned layers = 2) {
    REQUIRE(schedule.made(item, layers));
    const std::vector<std::size_t> uploaded = schedule.takeToUpload(64);
    REQUIRE(std::find(uploaded.begin(), uploaded.end(), item) != uploaded.end());
    for (unsigned layer = 0; layer < layers; ++layer) {
        schedule.layerDone(item);
    }
}

} // namespace

TEST_CASE("app/cover_requests: the focused item first, the view by projected size, the rest by distance",
          "[app][cover_requests]") {
    const CoverView view = viewFrom();
    // In front at 1 m and 4 m, a larger box at 4 m, behind at 2 m and 6 m.
    const rv::core::Aabb near = box({0.0f, 0.0f, -1.0f});
    const rv::core::Aabb far = box({0.2f, 0.0f, -4.0f});
    const rv::core::Aabb farLarge = box({-0.2f, 0.0f, -4.0f}, 3.0f);
    const rv::core::Aabb behindNear = box({0.0f, 0.0f, 2.0f});
    const rv::core::Aabb behindFar = box({0.0f, 0.0f, 6.0f});

    CHECK(rv::app::coverInView(near, view));
    CHECK(rv::app::coverInView(far, view));
    CHECK_FALSE(rv::app::coverInView(behindNear, view));
    CHECK_FALSE(rv::app::coverInView(rv::core::Aabb{}, view));
    CHECK(rv::app::coverDistance(box({0.0f, 0.0f, -1.0f}), glm::vec3(0.0f)) == Catch::Approx(0.99f));
    CHECK(rv::app::coverDistance(box({0.0f, 0.0f, 0.0f}), glm::vec3(0.0f)) == 0.0f);

    const CoverPriority pNear = rv::app::coverPriority(0, near, view, false);
    const CoverPriority pFar = rv::app::coverPriority(1, far, view, false);
    const CoverPriority pFarLarge = rv::app::coverPriority(2, farLarge, view, false);
    const CoverPriority pBehindNear = rv::app::coverPriority(3, behindNear, view, false);
    const CoverPriority pBehindFar = rv::app::coverPriority(4, behindFar, view, false);
    const CoverPriority pFocused = rv::app::coverPriority(5, behindFar, view, true);
    CHECK(pNear.rank == CoverRank::InView);
    CHECK(pBehindNear.rank == CoverRank::Rest);
    CHECK(pFocused.rank == CoverRank::Focused);
    CHECK(pFocused < pNear);
    CHECK(pNear < pFarLarge);
    CHECK(pFarLarge < pFar);         // the larger box at the same distance projects larger
    CHECK(pFar < pBehindNear);       // anything in view before anything out of it
    CHECK(pBehindNear < pBehindFar); // out of view: the nearest first
    // Equal priorities fall back on the item's index.
    CHECK(rv::app::coverPriority(7, near, view, false) < rv::app::coverPriority(8, near, view, false));
    CHECK(rv::app::toString(CoverRank::InView) == "in-view");
}

TEST_CASE("app/cover_requests: a view of another aspect adds its frustum; views compare by eye and planes",
          "[app][cover_requests]") {
    rv::scene::Camera camera;
    camera.setAspect(16.0f / 9.0f);
    CHECK(CoverView::of(camera).frusta.size() == 1);
    CHECK(CoverView::of(camera, 16.0f / 9.0f).frusta.size() == 1);
    // The lens's field of view is horizontal: a squarer picture sees more above and below.
    const CoverView square = CoverView::of(camera, 1.0f);
    REQUIRE(square.frusta.size() == 2);
    // A box above the 16:9 frustum (its half height 0.43 m at 1 m), inside the 1:1 one (0.77 m).
    const rv::core::Aabb above = box({0.0f, 0.6f, -1.0f});
    CHECK_FALSE(rv::app::coverInView(above, CoverView::of(camera)));
    CHECK(rv::app::coverInView(above, square));
    CHECK(CoverView::of(camera) == CoverView::of(camera));
    rv::scene::Camera moved = camera;
    moved.setPose(glm::vec3(0.0f, 0.0f, 0.01f), 0.0f, 0.0f);
    CHECK_FALSE(CoverView::of(camera) == CoverView::of(moved));
}

TEST_CASE("app/cover_requests: the schedule hands out the layout's order without a view, then the view's",
          "[app][cover_requests]") {
    // Items 0..3 behind the camera (at 1, 2, 3 and 4 m), 4..6 in front (at 5, 2 and 1 m).
    std::vector<rv::core::Aabb> bounds{box({0, 0, 1}),  box({0, 0, 2}),  box({0, 0, 3}), box({0, 0, 4}),
                                       box({0, 0, -5}), box({0, 0, -2}), box({0, 0, -1})};
    {
        CoverSchedule schedule(bounds);
        CHECK(makeOrder(schedule) == std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6});
    }
    CoverSchedule schedule(bounds);
    schedule.setView(viewFrom());
    CHECK(schedule.inViewCount() == 3);
    CHECK(schedule.inView(6));
    CHECK_FALSE(schedule.inView(0));
    CHECK(makeOrder(schedule) == std::vector<std::size_t>{6, 5, 4, 0, 1, 2, 3});
    CHECK(schedule.makingCount() == 7);

    // The focused item comes first, whatever the view; a new view ranks what is still waiting again.
    CoverSchedule focused(bounds);
    focused.setView(viewFrom());
    focused.setFocused(3);
    CHECK(focused.takeToMake(2) == std::vector<std::size_t>{3, 6});
    focused.setView(viewFrom(glm::vec3(0.0f), 180.0f)); // now facing +Z: items 0..2 in view
    CHECK(focused.takeToMake(3) == std::vector<std::size_t>{0, 1, 2});
    CHECK(focused.takeToMake(10) == std::vector<std::size_t>{5, 4});
    // An index outside the room focuses nothing.
    focused.setFocused(99);
    CHECK_FALSE(focused.focused().has_value());
}

TEST_CASE("app/cover_requests: made layers go up best first within the budget; arrivals are recorded in order",
          "[app][cover_requests]") {
    std::vector<rv::core::Aabb> bounds{box({0, 0, 3}), box({0, 0, -3}), box({0, 0, -1}), box({0, 0, 8})};
    CoverSchedule schedule(bounds);
    schedule.setView(viewFrom());
    REQUIRE(schedule.takeToMake(4).size() == 4);
    // Finished in another order than asked: the far ones first.
    CHECK(schedule.made(3, 2));
    CHECK(schedule.made(0, 1));
    CHECK(schedule.made(1, 2));
    CHECK(schedule.made(2, 2));
    CHECK_FALSE(schedule.made(2, 2)); // not Making any more
    CHECK(schedule.madeCount() == 4);
    CHECK(schedule.madeLayers() == 7);
    // 3 layers: item 2 (in view, nearest), then item 1 would pass the bound.
    CHECK(schedule.takeToUpload(3) == std::vector<std::size_t>{2});
    CHECK(schedule.takeToUpload(2) == std::vector<std::size_t>{1});
    // Always one, even when its layers pass the bound.
    CHECK(schedule.takeToUpload(1) == std::vector<std::size_t>{0});
    CHECK(schedule.takeToUpload(0).empty());
    CHECK(schedule.stage(1) == CoverSchedule::Stage::Uploading);
    CHECK_FALSE(schedule.viewDone());
    schedule.layerDone(1);
    schedule.layerDone(1);
    CHECK(schedule.stage(1) == CoverSchedule::Stage::Done);
    CHECK_FALSE(schedule.viewDone()); // item 2 in view still uploading
    schedule.layerDone(2);
    schedule.layerDone(2);
    CHECK(schedule.viewDone());
    CHECK(schedule.inViewPending() == 0);
    schedule.layerDone(0);
    CHECK_FALSE(schedule.done());
    REQUIRE(schedule.takeToUpload(64) == std::vector<std::size_t>{3});
    schedule.layerDone(3);
    schedule.layerDone(3);
    CHECK(schedule.done());
    const auto arrivals = schedule.arrivals();
    REQUIRE(arrivals.size() == 4);
    CHECK(arrivals[0].item == 1);
    CHECK(arrivals[0].rank == CoverRank::InView);
    CHECK(arrivals[1].item == 2);
    CHECK(arrivals[2].item == 0);
    CHECK(arrivals[2].rank == CoverRank::Rest);
    CHECK(arrivals[3].item == 3);
    CHECK(arrivals[3].distance > arrivals[2].distance);

    // A job with nothing to upload is done at once; a layer done for an item not uploading changes nothing.
    CoverSchedule empty({box({0, 0, -1})});
    REQUIRE(empty.takeToMake(1).size() == 1);
    empty.layerDone(0);
    CHECK(empty.stage(0) == CoverSchedule::Stage::Making);
    CHECK(empty.made(0, 0));
    CHECK(empty.done());
}

TEST_CASE("app/cover_requests: nothing is handed out while a launched game runs", "[app][cover_requests]") {
    CoverSchedule schedule({box({0, 0, -1}), box({0, 0, -2}), box({0, 0, -3})});
    schedule.setView(viewFrom());
    REQUIRE(schedule.takeToMake(1) == std::vector<std::size_t>{0});
    schedule.setPaused(true);
    CHECK(schedule.paused());
    CHECK(schedule.takeToMake(3).empty());
    // A job that was running finishes; its layers wait.
    CHECK(schedule.made(0, 2));
    CHECK(schedule.takeToUpload(64).empty());
    CHECK(schedule.madeCount() == 1);
    schedule.setPaused(false);
    CHECK(schedule.takeToUpload(64) == std::vector<std::size_t>{0});
    CHECK(schedule.takeToMake(3) == std::vector<std::size_t>{1, 2});
}

TEST_CASE("app/cover_requests: a done item can be queued again, its first arrival kept; a job given back waits",
          "[app][cover_requests]") {
    CoverSchedule schedule({box({0, 0, -1}), box({0, 0, -2})});
    schedule.setView(viewFrom());
    REQUIRE(schedule.takeToMake(2).size() == 2);
    CHECK_FALSE(schedule.requeue(0)); // on its way
    arrive(schedule, 0);
    arrive(schedule, 1);
    CHECK(schedule.done());
    CHECK(schedule.requeue(1));
    CHECK_FALSE(schedule.requeue(1));
    CHECK_FALSE(schedule.done());
    CHECK(schedule.doneCount() == 1);
    CHECK(schedule.takeToMake(2) == std::vector<std::size_t>{1});
    // A retry that changed nothing: done at once, no second arrival.
    CHECK(schedule.made(1, 0));
    CHECK(schedule.done());
    CHECK(schedule.arrivals().size() == 2);

    CoverSchedule givenBack({box({0, 0, -1})});
    REQUIRE(givenBack.takeToMake(1) == std::vector<std::size_t>{0});
    givenBack.giveBack(0);
    CHECK(givenBack.makingCount() == 0);
    CHECK(givenBack.stage(0) == CoverSchedule::Stage::Waiting);
    CHECK(givenBack.takeToMake(1) == std::vector<std::size_t>{0});
    CHECK(givenBack.arrivals().empty());
}

TEST_CASE("app/cover_requests: the hero set keeps 8, the least recently used out first, the shown one pinned",
          "[app][cover_requests]") {
    HeroLru lru;
    CHECK(lru.capacity() == 8);
    for (int index = 0; index < 8; ++index) {
        CHECK_FALSE(lru.insert("g" + std::to_string(index)).has_value());
    }
    CHECK(lru.size() == 8);
    // g0 used again: g1 is now the least recently used.
    CHECK(lru.touch("g0"));
    CHECK_FALSE(lru.touch("missing"));
    CHECK(lru.insert("g8") == std::optional<std::string>("g1"));
    CHECK_FALSE(lru.contains("g1"));
    // A pinned key is never evicted: g2 is pinned, g3 goes.
    const std::array<std::string, 1> pinned{"g2"};
    CHECK(lru.insert("g9", pinned) == std::optional<std::string>("g3"));
    CHECK(lru.contains("g2"));
    // Inserting a held key touches it, evicts nothing.
    CHECK_FALSE(lru.insert("g4").has_value());
    CHECK(lru.keys().back() == "g4");
    CHECK(lru.keys().front() == "g2");
    CHECK(lru.erase("g4"));
    CHECK_FALSE(lru.erase("g4"));
    CHECK(lru.size() == 7);

    // Every key pinned: nothing makes room, the new key is not held.
    HeroLru small(2);
    CHECK_FALSE(small.insert("a").has_value());
    CHECK_FALSE(small.insert("b").has_value());
    const std::array<std::string, 2> both{"a", "b"};
    CHECK(small.insert("c", both) == std::optional<std::string>("c"));
    CHECK_FALSE(small.contains("c"));
    CHECK(small.size() == 2);
    CHECK(HeroLru(0).capacity() == 1);
}

TEST_CASE("app/cover_requests: the hero picture keeps the cover's pixels at the label's aspect, up to 1,024 px",
          "[app][cover_requests]") {
    CHECK(rv::app::heroPictureSize(680, 497) == glm::ivec2(680, 499));
    CHECK(rv::app::heroPictureSize(476, 864) == glm::ivec2(1024, 752));
    CHECK(rv::app::heroPictureSize(2050, 1440) == glm::ivec2(1024, 752));
    CHECK(rv::app::heroPictureSize(256, 188) == glm::ivec2(256, 188));
    CHECK(rv::app::heroPictureSize(4, 3) == glm::ivec2(16, 12));
    CHECK(rv::app::heroPictureSize(0, 0) == glm::ivec2(16, 12));
    CHECK(rv::app::heroPictureSize(680, 497, 512) == glm::ivec2(512, 376));

    // A landscape and a portrait picture made from PNG bytes; bytes that are no image refused.
    const rv::assets::ImageData landscape =
        rv::assets::makeCheckerboard(680, 497, 17, {200, 40, 40, 255}, {20, 20, 160, 255});
    const auto landscapePng = rv::assets::encodePng(landscape);
    REQUIRE(landscapePng.has_value());
    const auto picture = rv::app::makeHeroPicture(*landscapePng, "landscape");
    REQUIRE(picture.has_value());
    CHECK(picture->width == 680);
    CHECK(picture->height == 499);
    CHECK(picture->channels == 4);
    const rv::assets::ImageData portrait =
        rv::assets::makeCheckerboard(476, 864, 17, {200, 40, 40, 255}, {20, 20, 160, 255});
    const auto portraitPng = rv::assets::encodePng(portrait);
    REQUIRE(portraitPng.has_value());
    const auto tall = rv::app::makeHeroPicture(*portraitPng, "portrait");
    REQUIRE(tall.has_value());
    CHECK(tall->width == 1024);
    CHECK(tall->height == 752);
    const std::array<std::byte, 4> junk{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    CHECK_FALSE(rv::app::makeHeroPicture(junk, "junk").has_value());
}

TEST_CASE("app/cover_requests: a family's hero picture is made at its label's shape (I4 step 2)",
          "[app][cover_requests]") {
    // ADR-0013 §4: the hero picture of a jewel case is square, of a keep case portrait, as wide as the cover needs.
    const rv::assets::LabelAspect square{120, 120};
    const rv::assets::LabelAspect keepCase{123, 178};
    CHECK(rv::app::heroPictureSize(680, 497, rv::app::kHeroMaxSide, square) == glm::ivec2(680, 680));
    CHECK(rv::app::heroPictureSize(600, 600, rv::app::kHeroMaxSide, square) == glm::ivec2(600, 600));
    CHECK(rv::app::heroPictureSize(476, 864, rv::app::kHeroMaxSide, keepCase) == glm::ivec2(597, 864));
    CHECK(rv::app::heroPictureSize(2000, 3000, rv::app::kHeroMaxSide, keepCase) == glm::ivec2(1024, 1482));
    // The layer's own shape is I3's.
    CHECK(rv::app::heroPictureSize(680, 497, rv::app::kHeroMaxSide, {}) == rv::app::heroPictureSize(680, 497));
    const rv::assets::ImageData portrait =
        rv::assets::makeCheckerboard(476, 864, 17, {200, 40, 40, 255}, {20, 20, 160, 255});
    const auto portraitPng = rv::assets::encodePng(portrait);
    REQUIRE(portraitPng.has_value());
    const auto picture = rv::app::makeHeroPicture(*portraitPng, "portrait", rv::app::kHeroMaxSide, keepCase);
    REQUIRE(picture.has_value());
    CHECK(picture->width == 597);
    CHECK(picture->height == 864);
}
