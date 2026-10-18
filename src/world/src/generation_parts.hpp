#pragma once

// Private to rv_world (I3 step 9): what the fresh generation (generation.cpp) and the incremental update
// (incremental.cpp) share: the checks of their inputs and the context the validation judges their plans in.

#include <rv/core/expected.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world::detail {

/// The prefabs' slot levels as the generator measured them, for the validation (ValidationContext::prefabSlots). The
/// source refers to `prefabs`, which must outlive it.
[[nodiscard]] PrefabSlotSource catalogSlots(const PrefabCatalog& prefabs);

/// The selection of the data's room, of the platform's media family, a configuration checkWorldConfig() accepts, no
/// game both a title and unplaced. Errors: InvalidArgument naming the problem (the configuration's own error code
/// otherwise).
[[nodiscard]] Status checkGenerationInput(const GenerationInput& input, const WorldData& data);

/// generationInputDigest() of an input whose selection the caller has checked (the update: checkSelectionForm() in
/// incremental.cpp), without roomSelectionToJson()'s check by the reader; the same digest.
[[nodiscard]] Expected<std::string> uncheckedInputDigest(const GenerationInput& input, const WorldData& data);

/// What a plan generated or updated from `input` is judged against: the data's template, the configuration, the
/// selection, the prefabs' slots as measured, the plan's lights per room and the pins given.
[[nodiscard]] ValidationContext validationContext(const WorldPlan& plan, const GenerationInput& input,
                                                  const WorldData& data, const WorldOverrides& pins);

} // namespace rv::world::detail
