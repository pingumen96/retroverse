// The navigation grid of the validation (rv/world/nav_grid.hpp, ADR-0010 §11): 0.1 m cells, obstacles and walls
// dilated by the clearance with Euclidean distances (rounded corners), a breadth-first search through edge neighbours.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <rv/world/nav_grid.hpp>

using rv::world::Footprint;
using rv::world::NavCell;
using rv::world::NavGrid;
using rv::world::NavReach;

TEST_CASE("world/nav_grid: cells of 0.1 m over the area, their centres, the cell of a point", "[world][nav_grid]") {
    const NavGrid grid(Footprint{-500, 1000, 500, 1550}, {}, 0);
    CHECK(grid.columns() == 10);
    CHECK(grid.rows() == 5); // the last 50 mm are not a whole cell
    CHECK(grid.freeCount() == 50);
    CHECK(grid.centreXMm({0, 0}) == -450);
    CHECK(grid.centreZMm({0, 0}) == 1050);
    CHECK(grid.centreXMm({9, 4}) == 450);
    CHECK(grid.centreZMm({9, 4}) == 1450);
    CHECK(grid.cellAt(-500, 1000) == NavCell{0, 0});
    CHECK(grid.cellAt(-401, 1099) == NavCell{0, 0});
    CHECK(grid.cellAt(-400, 1100) == NavCell{1, 1}); // a point on an edge is in the cell after it
    CHECK(grid.cellAt(499, 1499) == NavCell{9, 4});
    CHECK_FALSE(grid.cellAt(500, 1200).has_value());
    CHECK_FALSE(grid.cellAt(0, 1500).has_value());
    CHECK_FALSE(grid.cellAt(-501, 1200).has_value());
    CHECK(grid.contains({9, 4}));
    CHECK_FALSE(grid.contains({10, 0}));
    CHECK_FALSE(grid.contains({0, -1}));
    CHECK_FALSE(grid.isFree({-1, 0}));

    // An empty or inverted area has no cells, and nothing is reached in it.
    const NavGrid empty(Footprint{0, 0, 0, 1000}, {}, 0);
    CHECK(empty.columns() == 0);
    CHECK(empty.freeCount() == 0);
    CHECK_FALSE(empty.cellAt(0, 0).has_value());
    CHECK(NavReach(empty, {0, 0}).reachedCount() == 0);
    CHECK(NavGrid(Footprint{100, 0, 0, 100}, {}, 0).columns() == 0);
}

TEST_CASE("world/nav_grid: walls and obstacles block the cells closer than the clearance", "[world][nav_grid]") {
    // 1 m x 1 m, clearance 150 mm: the centres at 50 mm from a wall are blocked, those at 150 mm free.
    const NavGrid walls(Footprint{0, 0, 1000, 1000}, {}, 150);
    CHECK_FALSE(walls.isFree({0, 5}));
    CHECK(walls.isFree({1, 5}));
    CHECK(walls.isFree({8, 5}));
    CHECK_FALSE(walls.isFree({9, 5}));
    CHECK(walls.freeCount() == 64);

    // An obstacle from (400, 400) to (600, 500), clearance 200: blocked along its sides within 200 mm, and at its
    // corners within 200 mm of the corner point (rounded, not a square dilation).
    const std::vector<Footprint> obstacles{{400, 400, 600, 500}};
    const NavGrid grid(Footprint{0, 0, 1000, 1000}, obstacles, 200);
    CHECK_FALSE(grid.isFree({5, 4})); // inside
    CHECK_FALSE(grid.isFree({2, 4})); // centre x 250: 150 mm from its west side
    CHECK(grid.isFree({2, 2}));       // centre (250, 250): 212 mm from the corner (400, 400)
    CHECK_FALSE(grid.isFree({3, 2})); // centre (350, 250): 158 mm from that corner
    CHECK_FALSE(grid.isFree({3, 3})); // centre (350, 350): 71 mm from it
    CHECK(grid.isFree({2, 7}));       // centre (250, 750): 292 mm from the corner (400, 500)
    CHECK_FALSE(grid.isFree({3, 6})); // centre (350, 650): 158 mm from that corner
    CHECK(grid.isFree({5, 7}));       // centre (550, 750): 250 mm south of it
    CHECK_FALSE(grid.isFree({5, 6})); // centre (550, 650): 150 mm south of it
    // A centre exactly at the clearance is free: (550, 650) lies 200 mm from an obstacle ending at z 450.
    const std::vector<Footprint> low{{400, 400, 600, 450}};
    const NavGrid exact(Footprint{0, 0, 1000, 1000}, low, 200);
    CHECK(exact.isFree({5, 6}));
    CHECK_FALSE(exact.isFree({5, 5}));

    // An obstacle reaching outside the area blocks the cells inside it; a clearance of 0 blocks only the cells whose
    // centres lie in an obstacle (its edges included).
    const std::vector<Footprint> outside{{-500, -500, 150, 150}};
    const NavGrid touching(Footprint{0, 0, 1000, 1000}, outside, 0);
    CHECK_FALSE(touching.isFree({0, 0}));
    CHECK_FALSE(touching.isFree({1, 1})); // centre (150, 150) on its corner
    CHECK(touching.isFree({2, 1}));
    CHECK(touching.freeCount() == 96);
}

TEST_CASE("world/nav_grid: the search reaches through free cells by their edges only", "[world][nav_grid]") {
    // A wall across the room with a 300 mm gap, clearance 0.
    const std::vector<Footprint> wall{{0, 450, 400, 550}, {700, 450, 1000, 550}};
    const NavGrid grid(Footprint{0, 0, 1000, 1000}, wall, 0);
    const NavReach reach(grid, {0, 0});
    CHECK(reach.reached({0, 0}));
    CHECK(reach.reached({9, 9})); // through the gap at x 400 to 700
    CHECK_FALSE(reach.reached({0, 5}));
    CHECK(reach.reachedCount() == grid.freeCount());
    // A clearance of 200 mm closes the gap: the north half is not reached.
    const NavGrid narrow(Footprint{0, 0, 1000, 1000}, wall, 200);
    const NavReach blocked(narrow, *narrow.cellAt(500, 200));
    CHECK(blocked.reached(*narrow.cellAt(500, 200))); // centre (550, 250): 200 mm from the wall
    CHECK_FALSE(blocked.reached(*narrow.cellAt(500, 300)));
    CHECK_FALSE(blocked.reached(*narrow.cellAt(500, 800)));
    CHECK_FALSE(blocked.reachesNear(500, 340, 0));   // (500, 340) is no cell centre
    CHECK_FALSE(blocked.reachesNear(500, 340, 100)); // the centres within 100 mm are blocked
    CHECK(blocked.reachesNear(500, 340, 120));       // (550, 250) is 103 mm away
    CHECK_FALSE(blocked.reachesNear(500, 800, 250));

    // Two obstacles touching at a corner leave only a diagonal step, which the search never takes.
    const std::vector<Footprint> corner{{100, 0, 200, 100}, {0, 100, 100, 200}};
    const NavGrid diagonal(Footprint{0, 0, 200, 200}, corner, 0);
    CHECK(diagonal.isFree({0, 0}));
    CHECK(diagonal.isFree({1, 1}));
    const NavReach stuck(diagonal, {0, 0});
    CHECK(stuck.reachedCount() == 1);
    CHECK_FALSE(stuck.reached({1, 1}));

    // From a blocked or outside cell nothing is reached.
    CHECK(NavReach(diagonal, {1, 0}).reachedCount() == 0);
    CHECK(NavReach(diagonal, {5, 5}).reachedCount() == 0);
    CHECK_FALSE(NavReach().reachesNear(0, 0, 1000));
}
