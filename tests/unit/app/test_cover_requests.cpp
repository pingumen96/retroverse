// The cover loader's order and the hero textures' set (I3 step 12, rv/app/cover_requests.hpp): an item's priority (the
// focused item first, the items in view by projected size, the rest by distance), the schedule of every item's stages
// (made and uploaded best first, ranked again when the view or the focus changes, nothing handed out while a launched
// game runs, the arrival order recorded, retries), the least-recently-used set of 8 with the shown texture pinned, and
// the hero picture's size and making.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/cover_requests.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/text.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/texture_memory.hpp>
#include <rv/scene/camera.hpp>
#include <rv/world/media_family.hpp>

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

TEST_CASE("app/cover_requests: on a floor the view is the eye's room and the rooms whose doorway it sees; the rest "
          "comes by distance",
          "[app][cover_requests]") {
    // I4 step 5a, ADR-0013 §9: one group per room. The camera at the origin looks at -Z.
    const CoverView plain = viewFrom();
    const rv::core::Aabb ahead = box({0.0f, 0.0f, -2.0f});
    // An empty list of groups sees every group; a list sees the groups it flags, none past its end.
    CHECK(plain.seesGroup(0));
    CHECK(plain.seesGroup(7));
    CoverView seen = plain;
    seen.groups = {1, 0};
    CHECK(seen.seesGroup(0));
    CHECK_FALSE(seen.seesGroup(1));
    CHECK_FALSE(seen.seesGroup(2));
    // An item in a frustum is in view only for a group the view sees; out of it, it ranks by its distance.
    CHECK(rv::app::coverInView(ahead, seen, 0));
    CHECK_FALSE(rv::app::coverInView(ahead, seen, 1));
    const CoverPriority unseen = rv::app::coverPriority(3, ahead, seen, false, 1);
    CHECK(unseen.rank == CoverRank::Rest);
    CHECK(unseen.order == Catch::Approx(rv::app::coverDistance(ahead, seen.eye)));
    CHECK(rv::app::coverPriority(3, ahead, seen, false, 0).rank == CoverRank::InView);
    // The focused item comes first whatever its group.
    CHECK(rv::app::coverPriority(3, ahead, seen, true, 1).rank == CoverRank::Focused);

    // The groups of a floor of three rooms: room 0 holds the eye, room 1's doorway is ahead (in the frustum), room 2's
    // behind the camera; a room without a doorway (an empty box) is seen only when it holds the eye.
    const std::vector<rv::core::Aabb> doorways{
        rv::core::Aabb{glm::vec3(-0.6f, 0.0f, 0.5f), glm::vec3(0.6f, 2.2f, 0.7f)},
        rv::core::Aabb{glm::vec3(-0.6f, 0.0f, -8.2f), glm::vec3(0.6f, 2.2f, -8.0f)},
        rv::core::Aabb{glm::vec3(-0.6f, 0.0f, 6.0f), glm::vec3(0.6f, 2.2f, 6.2f)}};
    CHECK(rv::app::coverViewGroups(plain, doorways, 0) == std::vector<std::uint8_t>{1, 1, 0});
    // In a hall (no room holds the eye) only the doorways count; turned around, room 2's is seen and room 1's is not.
    CHECK(rv::app::coverViewGroups(plain, doorways, std::nullopt) == std::vector<std::uint8_t>{0, 1, 0});
    CHECK(rv::app::coverViewGroups(viewFrom(glm::vec3(0.0f), 180.0f), doorways, std::nullopt) ==
          std::vector<std::uint8_t>{1, 0, 1});
    CHECK(rv::app::coverViewGroups(plain, doorways, 2) == std::vector<std::uint8_t>{0, 1, 1});
    const std::vector<rv::core::Aabb> withoutDoorway{rv::core::Aabb{}, doorways[1]};
    CHECK(rv::app::coverViewGroups(plain, withoutDoorway, std::nullopt) == std::vector<std::uint8_t>{0, 1});
    CHECK(rv::app::coverViewGroups(plain, withoutDoorway, 0) == std::vector<std::uint8_t>{1, 1});
    // The view's own groups do not hide a doorway; a floor of one room (or none) sees it whole.
    CHECK(rv::app::coverViewGroups(seen, doorways, 0) == std::vector<std::uint8_t>{1, 1, 0});
    CHECK(rv::app::coverViewGroups(plain, std::vector<rv::core::Aabb>{doorways[2]}, std::nullopt).empty());
    CHECK(rv::app::coverViewGroups(plain, {}, std::nullopt).empty());

    // The schedule over three rooms seen from room 0: an item of room 2 in the frustum (beyond a wall, its doorway out
    // of view) is not in view and comes by its distance after every item in view, before a farther one.
    const std::vector<rv::core::Aabb> bounds{
        box({0.0f, 0.0f, -4.0f}),  // 0: room 0, in view
        box({1.5f, 0.0f, -2.0f}),  // 1: room 2, in the frustum, unseen
        box({0.0f, 0.0f, -6.0f}),  // 2: room 1, in view through its doorway
        box({0.0f, 0.0f, 9.0f}),   // 3: room 2, behind
        box({0.0f, 0.0f, 1.5f})};  // 4: room 0, behind (its group seen, out of the frustum)
    CoverSchedule schedule(bounds, {0, 2, 1, 2, 0});
    CoverView floorView = plain;
    floorView.groups = rv::app::coverViewGroups(plain, doorways, 0);
    schedule.setView(floorView);
    CHECK(schedule.inViewCount() == 2);
    CHECK(schedule.inView(0));
    CHECK(schedule.inView(2));
    CHECK_FALSE(schedule.inView(1));
    CHECK(schedule.rank(1) == CoverRank::Rest);
    CHECK(makeOrder(schedule) == std::vector<std::size_t>{0, 2, 4, 1, 3});
    // Every item in view resident is the view done, the unseen room's item still waiting.
    CoverSchedule settled(bounds, {0, 2, 1, 2, 0});
    settled.setView(floorView);
    CHECK(settled.takeToMake(2) == std::vector<std::size_t>{0, 2});
    CHECK_FALSE(settled.viewDone());
    arrive(settled, 0);
    arrive(settled, 2);
    CHECK(settled.viewDone());
    CHECK(settled.inViewPending() == 0);
    CHECK_FALSE(settled.done());
    // Without groups (the floor of one room, I3's) the same item in the frustum is in view.
    CoverSchedule whole(bounds);
    whole.setView(plain);
    CHECK(whole.inView(1));
    CHECK(whole.inViewCount() == 3);
}

TEST_CASE("app/cover_requests: a room's items in view and those pending, what a ride's view waits for (I4 step 8)",
          "[app][cover_requests]") {
    // Room 0 holds the eye: two items in view (ahead), one behind; room 1's item in view through its doorway.
    const CoverView plain = viewFrom();
    const std::vector<rv::core::Aabb> doorways{
        rv::core::Aabb{glm::vec3(-0.6f, 0.0f, 0.5f), glm::vec3(0.6f, 2.2f, 0.7f)},
        rv::core::Aabb{glm::vec3(-0.6f, 0.0f, -8.2f), glm::vec3(0.6f, 2.2f, -8.0f)}};
    const std::vector<rv::core::Aabb> bounds{box({0.0f, 0.0f, -2.0f}), box({0.3f, 0.0f, -3.0f}),
                                             box({0.0f, 0.0f, 2.0f}), box({0.0f, 0.0f, -9.0f})};
    CoverSchedule schedule(bounds, {0, 0, 0, 1});
    // Before any view nothing is in view (the session checks the view only from the frame after its floor showed).
    CHECK(schedule.groupView(0) == rv::app::CoverGroupView{0, 0});
    CoverView view = plain;
    view.groups = rv::app::coverViewGroups(plain, doorways, 0);
    schedule.setView(view);
    CHECK(schedule.groupView(0) == rv::app::CoverGroupView{2, 2});
    CHECK(schedule.groupView(1) == rv::app::CoverGroupView{1, 1});
    CHECK(schedule.groupView(7) == rv::app::CoverGroupView{0, 0});
    // The covers in view of the eye's room resident: none pending, while room 0's item behind and room 1's wait.
    REQUIRE(schedule.takeToMake(2) == std::vector<std::size_t>{0, 1});
    arrive(schedule, 0);
    CHECK(schedule.groupView(0) == rv::app::CoverGroupView{2, 1});
    arrive(schedule, 1);
    CHECK(schedule.groupView(0) == rv::app::CoverGroupView{2, 0});
    CHECK(schedule.groupView(1).pending == 1);
    CHECK_FALSE(schedule.viewDone()); // the whole view holds room 1's item too
    CHECK(schedule.stage(2) == CoverSchedule::Stage::Waiting);
    // Turned around, the room's item behind is in view and pending.
    CoverView back = viewFrom(glm::vec3(0.0f), 180.0f);
    back.groups = rv::app::coverViewGroups(back, doorways, 0);
    schedule.setView(back);
    CHECK(schedule.groupView(0) == rv::app::CoverGroupView{1, 1});
    // Without groups every item is group 0's.
    CoverSchedule single(bounds);
    single.setView(plain);
    CHECK(single.groupView(0) == rv::app::CoverGroupView{3, 3});
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

TEST_CASE("app/cover_requests: items taken over as they show are done at once, without an arrival; the others are "
          "handed out as before",
          "[app][cover_requests]") {
    // I4 step 6b: a cover loader that replaces another (a floor committed again, a live update) adopts the items whose
    // pictures show already in their collection: CoverSchedule::adoptDone(), nothing made or uploaded again.
    CoverSchedule schedule({box({0, 0, -1}), box({0, 0, -2}), box({0, 0, -3}), box({0, 0, -4})});
    schedule.setView(viewFrom());
    REQUIRE(schedule.takeToMake(1) == std::vector<std::size_t>{0}); // item 0 is Making: not adopted
    schedule.adoptDone(std::vector<std::size_t>{0, 1, 3, 3, 9}); // a repeat and an index past the items: ignored
    CHECK(schedule.stage(0) == CoverSchedule::Stage::Making);
    CHECK(schedule.stage(1) == CoverSchedule::Stage::Done);
    CHECK(schedule.stage(2) == CoverSchedule::Stage::Waiting);
    CHECK(schedule.stage(3) == CoverSchedule::Stage::Done);
    CHECK(schedule.doneCount() == 2);
    CHECK(schedule.arrivals().empty()); // shown before: no arrival of this schedule
    CHECK_FALSE(schedule.requeue(0));
    // Only the others are handed out; an adopted item can be queued again (a retry after a refresh).
    CHECK(schedule.takeToMake(4) == std::vector<std::size_t>{2});
    arrive(schedule, 0);
    arrive(schedule, 2);
    CHECK(schedule.done());
    CHECK(schedule.viewDone());
    CHECK(schedule.arrivals().size() == 2);
    CHECK(schedule.requeue(1));
    CHECK(schedule.takeToMake(4) == std::vector<std::size_t>{1});
    CHECK(schedule.made(1, 0));
    CHECK(schedule.done());
    CHECK(schedule.arrivals().size() == 2); // shown before the schedule: never an arrival of it
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
    // ADR-0011 §8's bounds whatever the shape: the long side at most 1,024 px, at most 1,024 x 752 pixels.
    CHECK(rv::app::heroPictureSize(2000, 3000, rv::app::kHeroMaxSide, keepCase) == glm::ivec2(707, 1023));
    CHECK(rv::app::heroPictureSize(2000, 2000, rv::app::kHeroMaxSide, square) == glm::ivec2(877, 877));
    CHECK(rv::app::heroPictureSize(3000, 2000, rv::app::kHeroMaxSide, {96, 60}) == glm::ivec2(1024, 640));
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

TEST_CASE("app/cover_requests: every family's hero pictures keep ADR-0011's hero bounds, 8 of them within 32 MiB",
          "[app][cover_requests]") {
    const auto registry = rv::world::loadMediaFamilies(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR));
    REQUIRE(registry.has_value());
    REQUIRE(registry->families.size() >= 9);
    constexpr std::size_t kHeroBudgetBytes = 32ull * 1024ull * 1024ull;
    const std::array<glm::ivec2, 5> sources{glm::ivec2(4000, 4000), glm::ivec2(3000, 4500), glm::ivec2(4500, 3000),
                                            glm::ivec2(1500, 2100), glm::ivec2(2050, 1440)};
    for (const rv::world::MediaFamily& family : registry->families) {
        for (const glm::ivec2 source : sources) {
            const glm::ivec2 size =
                rv::app::heroPictureSize(source.x, source.y, rv::app::kHeroMaxSide, family.coverAspect());
            INFO(family.id << ": " << source.x << " x " << source.y << " gives " << size.x << " x " << size.y);
            CHECK(std::max(size.x, size.y) <= rv::app::kHeroMaxSide);
            CHECK(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) <= 1024u * 752u);
            // The texture as HeroTextures makes it: RGBA8 sRGB with its whole mip chain.
            const std::size_t bytes =
                rv::gl::textureBytes(rv::gl::GpuFormat::Srgb8Alpha8, size, rv::gl::fullMipLevelCount(size));
            CHECK(bytes * rv::app::HeroLru::kDefaultCapacity <= kHeroBudgetBytes);
            // At the label's shape (its aspect within the rounding of a side).
            const rv::assets::LabelAspect label = family.coverAspect();
            if (!label.isLayer()) {
                const double aspect = static_cast<double>(label.width) / static_cast<double>(label.height);
                CHECK(std::abs(static_cast<double>(size.x) / static_cast<double>(size.y) - aspect) <=
                      aspect / static_cast<double>(std::min(size.x, size.y)));
            }
        }
    }
}
