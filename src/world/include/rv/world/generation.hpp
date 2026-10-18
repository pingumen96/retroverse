#pragma once

// GenerationResult (ADR-0010 §2): what world::generate (a fresh plan, I3 step 3) and the incremental update (step 9)
// return, an immutable value posted to the main thread (§12): the plan, the change report and the validation report.
// The types are I3 step 1a's; generate() and the data it reads (WorldData, loadWorldData()) are step 3's.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/capacity.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/platform_manifest.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// How a plan came to be.
enum class GenerationOutcome : std::uint8_t {
    Generated,                    ///< a fresh plan: no previous one, or an explicit regeneration
    Updated,                      ///< the previous plan updated by the incremental rules (ADR-0010 §10)
    RequiresExplicitRegeneration, ///< another seed, preset, template version or generator major version: the previous
                                  ///< plan is kept as it is, and only an explicit regeneration replaces it
};

/// "generated", "updated", "requires-explicit-regeneration".
[[nodiscard]] constexpr std::string_view toString(GenerationOutcome outcome) noexcept {
    switch (outcome) {
    case GenerationOutcome::Generated:
        return "generated";
    case GenerationOutcome::Updated:
        return "updated";
    case GenerationOutcome::RequiresExplicitRegeneration:
        return "requires-explicit-regeneration";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<GenerationOutcome> parseGenerationOutcome(std::string_view text) noexcept {
    return detail::enumFromString(text, GenerationOutcome::RequiresExplicitRegeneration);
}

/// What the room has to rebuild for a change (style §13.6, ADR-0010 §10), the least first.
enum class RebuildLevel : std::uint8_t {
    NoRebuild,     ///< media or metadata changed: textures and spine text swapped, no geometry
    LocalReflow,   ///< games took free slots or dressed levels, or a side-wall module replaced decoration: the static
                   ///< part rebuilt
    RoomExpansion, ///< the room grew south
    Full,          ///< a fresh plan: everything built anew
};

/// "no-rebuild", "local-reflow", "room-expansion", "full".
[[nodiscard]] constexpr std::string_view toString(RebuildLevel level) noexcept {
    switch (level) {
    case RebuildLevel::NoRebuild:
        return "no-rebuild";
    case RebuildLevel::LocalReflow:
        return "local-reflow";
    case RebuildLevel::RoomExpansion:
        return "room-expansion";
    case RebuildLevel::Full:
        return "full";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<RebuildLevel> parseRebuildLevel(std::string_view text) noexcept {
    return detail::enumFromString(text, RebuildLevel::Full);
}

/// What changed from the previous plan (ADR-0010 §10). Lists are sorted (GameIds, ids, text) so that two runs report
/// alike.
struct ChangeReport {
    GenerationOutcome outcome = GenerationOutcome::Generated;
    RebuildLevel rebuild = RebuildLevel::Full;
    /// Games placed by this change, and how many placements of the previous plan were kept as they were.
    std::vector<catalog::GameId> added;
    std::uint64_t kept = 0;
    /// Games newly listed as unplaced (with their reasons in the plan).
    std::vector<catalog::GameId> unplaced;
    /// Fixtures this change added (side-wall modules in place of decoration, the fixtures of a growth step).
    std::vector<FixtureId> fixturesAdded;
    /// Fixtures it removed (the decoration a new side-wall module replaced) and moved (what stands on the south wall,
    /// moved by a growth step), by id (I3 step 9).
    std::vector<FixtureId> fixturesRemoved;
    std::vector<FixtureId> fixturesMoved;
    /// Levels whose shelf dressing gave way to a title, and slots an alias freed (ADR-0010 §10), by id (I3 step 9).
    std::vector<LevelId> dressingRemoved;
    std::vector<SlotId> slotsFreed;
    /// Pins of overrides.json that name a game or slot that no longer exists (never moved), and changes that would
    /// have moved a pinned object, not applied (`needsConfirmation`, the editor's dialog in I4): each as the pin's
    /// text (overrides.hpp's pinText()), or for a refused change what it would have done.
    std::vector<std::string> orphanedPins;
    std::vector<std::string> needsConfirmation;
    /// Why an explicit regeneration is needed (RequiresExplicitRegeneration): which version, seed or preset differs.
    std::string regenerationReason;
    /// The plan digests before (empty without a previous plan) and after.
    std::string digestBefore;
    std::string digestAfter;

    bool operator==(const ChangeReport&) const = default;
};

/// A hard rule makes a plan unusable (never saved or shown); a soft one is reported and the plan kept (ADR-0010 §11).
enum class Severity : std::uint8_t {
    Hard,
    Soft,
};

/// "hard", "soft".
[[nodiscard]] constexpr std::string_view toString(Severity severity) noexcept {
    switch (severity) {
    case Severity::Hard:
        return "hard";
    case Severity::Soft:
        return "soft";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<Severity> parseSeverity(std::string_view text) noexcept {
    return detail::enumFromString(text, Severity::Soft);
}

/// One broken rule.
struct ValidationFinding {
    /// The rule's name ("footprints-overlap", "doorway-blocked", "unreachable", ...: validation.hpp, I3 step 4).
    std::string rule;
    Severity severity = Severity::Hard;
    /// The id it is about (a fixture, a slot, a GameId), empty for the plan as a whole.
    std::string subject;
    std::string message;

    bool operator==(const ValidationFinding&) const = default;
};

struct ValidationReport {
    /// In the order the rules ran, each rule's findings sorted by subject.
    std::vector<ValidationFinding> findings;
    /// Style §13.5's LayoutScore in permille, a diagnostic that chooses nothing in I3; nothing until computed.
    std::optional<std::int64_t> layoutScorePermille;

    /// No hard finding.
    [[nodiscard]] bool passed() const noexcept {
        for (const ValidationFinding& finding : findings) {
            if (finding.severity == Severity::Hard) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const ValidationReport&) const = default;
};

/// The generator's answer: the plan (the previous one, unchanged, for RequiresExplicitRegeneration), what changed and
/// what the validation found.
struct GenerationResult {
    WorldPlan plan;
    ChangeReport changes;
    ValidationReport validation;

    bool operator==(const GenerationResult&) const = default;
};

/// The generator's version (ADR-0010 §8, §10): 1.0 is I3 step 3's fresh generation with step 9's incremental rules
/// (incremental.hpp), both first released in I3 (schema 1 stays open until Gate B). A change that places any game
/// differently is a new major version; one that only adds rules keeping every placement a new minor one.
inline constexpr GeneratorVersion kGeneratorVersion{1, 0};

/// The one room template of I3 (ADR-0010 §4).
inline constexpr std::string_view kDefaultRoomTemplate = "console_room_9x10";

/// What a generation reads from the asset root for one system's room: the template, the system's platform manifest,
/// its media family and every prefab they name, measured (capacity.hpp).
struct WorldData {
    RoomTemplate roomTemplate;
    PlatformManifest platform;
    MediaFamily mediaFamily;
    PrefabCatalog prefabs;
};

/// Reads the world's data for `system`'s room: <assetRoot>/world/templates/<templateId>.json, platforms/<system>.json,
/// media_families.json (the platform's family, its item checked against its manifest with checkItemManifest()) and,
/// through the prefab families (assets/manifests/families.json), every prefab they name (a missing or broken manifest
/// falls back to its family's prefab, logged once). Errors: those of the readers (NotFound for a system without a
/// platform manifest), NotFound for a media family the registry lacks, ValidationFailed for an item that disagrees with
/// its manifest.
[[nodiscard]] Expected<WorldData> loadWorldData(const std::filesystem::path& assetRoot,
                                                const catalog::SystemName& system,
                                                std::string_view templateId = kDefaultRoomTemplate);

/// What a fresh generation is asked for: the library, its room's selection (world::selectRoom, I3 step 2), the
/// configuration (world_config.hpp) and the world's overrides (overrides.hpp, I3 step 9: a fresh plan applies no pin,
/// and one that does not honour a pin breaks the validation's pin-broken rule, so it is never saved).
struct GenerationInput {
    catalog::LibraryId library;
    RoomSelection selection;
    WorldConfig config;
    WorldOverrides overrides;
};

/// The plan's inputDigest for these inputs (ADR-0010 §8): the SHA-256 of the canonical document of the configuration
/// as it shapes the room (reduceMotion only shows it), the world seed, the selection, the template by id and version,
/// what the plan takes from the platform manifest and the media family, every prefab as measured, and the generator's
/// version. The overrides are not part of it (they are protected, never applied, in I3). The incremental update
/// (incremental.hpp) records the digest of the inputs it read the same way, so a plan's inputDigest always names the
/// inputs of its last generation or update. Errors: InvalidArgument for a configuration or selection their writers
/// refuse.
[[nodiscard]] Expected<std::string> generationInputDigest(const GenerationInput& input, const WorldData& data);

/// A fresh plan for the selection's room (ADR-0010 §3 to §8), a pure function of its inputs: two runs, and Debug and
/// Release, give one canonical text. The display rule: the first min(T, D + B) titles in priority order stand face-out
/// (the first D on the display fixtures, the next B in the hero band), the others spine-out in the hero-wall modules
/// and in as few side-wall modules as they and the reserve need, in the smallest room that holds them (growth); at the
/// template's limit the titles last in priority order are unplaced with reason capacity. Every side-wall socket no
/// module needs holds decoration, the decoration sockets within the density, every level no title needs shelf dressing;
/// the lights stay within the template's limit. The selection's unplaced games are copied into the plan. The result's
/// changes are a fresh plan's (Generated, Full, every placement added); its validation is validatePlan()'s
/// (validation.hpp) against the template, the configuration, the selection, the prefabs' slots as measured here and the
/// plan's lights: a plan with a hard finding is returned all the same, and its caller never saves or shows it
/// (ADR-0010 §11). Errors: InvalidArgument for a selection of another room than the data's system, a
/// selection roomSelectionToJson() refuses, a game both a title and unplaced, a configuration checkWorldConfig()
/// refuses, a media family other than the platform's; those of the layout (a module or display prefab that does not
/// fit its socket: ValidationFailed).
[[nodiscard]] Expected<GenerationResult> generate(const GenerationInput& input, const WorldData& data);

} // namespace rv::world
