#pragma once

// Private to rv_world (I4b step 2): the validation's geometry of fixtures (validation.hpp's header: a fixture's box,
// rounded outwards to half millimetres, floor coverings walked over), shared with the incremental update, which skips a
// socket whose new fixture would overlap a fixture the user moved there (ADR-0015 §3). Defined in validation.cpp.

#include <rv/world/world_plan.hpp>

namespace rv::world::detail {

/// Whether the boxes of `a` and `b` overlap as the validation's footprints-overlap rule judges them (boxes that touch do
/// not; a floor covering overlaps nothing).
[[nodiscard]] bool fixturesOverlap(const Fixture& a, const Fixture& b) noexcept;

} // namespace rv::world::detail
