#pragma once

// The persisted start (I3 step 10c; ADR-0010 §12): the plan read at start and shown before the catalog's snapshot
// answers, then reconciled with the incremental rules. world_start.cpp implements WorldSession's part of it
// (WorldSession::commitPreview, WorldSession::reconcile); the flow's part is WorldFlow's preview phase
// (rv/app/world_build.hpp: previewWorld, WorldFlow::takePreview), the decision rv/app/world_reconcile.hpp's, the room's
// items kept for it rv/app/room_items.hpp's.

#include "world_session.hpp"
