#pragma once

// The live update (I3 step 15b; ADR-0010 §10, §12): after a refresh of the catalog (step 13r) the room's job runs again
// while the room shows (WorldFlow without its preview: the snapshot, world::update with the incremental rules, the plan
// saved under the world's lock) and its result is applied at a frame boundary with as little rebuilt as it needs.
// world_live_update.cpp implements WorldSession's part of it (WorldSession::requestLiveUpdate, startLiveFlow,
// applyLive, applyItems); the decision is rv/app/world_live_plan.hpp's, the collection's items applied in place
// CollectionInstance::applyLayout, the covers of the items that stood adopted by CoverLoader::adoptShown (only the
// changed pictures uploaded again).

#include "world_session.hpp"
