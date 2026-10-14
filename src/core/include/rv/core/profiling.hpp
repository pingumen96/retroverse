#pragma once

// Thin wrappers over Tracy so call sites compile to nothing unless RV_ENABLE_TRACY is set
// (preset msvc-x64-profile).

#if defined(RV_ENABLE_TRACY)
#include <tracy/Tracy.hpp>
#define RV_PROFILE_ZONE() ZoneScoped
#define RV_PROFILE_ZONE_NAMED(name) ZoneScopedN(name)
#define RV_PROFILE_FRAME() FrameMark
#else
#define RV_PROFILE_ZONE() ((void)0)
#define RV_PROFILE_ZONE_NAMED(name) ((void)0)
#define RV_PROFILE_FRAME() ((void)0)
#endif
