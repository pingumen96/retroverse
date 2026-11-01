#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The settings screen's rows (ADR-0018 §3): a table of what each row sets (its section of settings.json and its dotted
// key), its kind, range, whether it applies at once. The Italian label of each is the document's (assets/ui/settings.rml,
// keyed by id()). No key that shapes a plan is here (ADR-0018 §2); the bindings of the Controlli group are step 3's.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace rv::app {

enum class SettingGroup : std::uint8_t { Display, Audio, World, Controls, Vr };
/// "display", "audio", "world", "controls", "vr" (the document's tabs).
[[nodiscard]] std::string_view toString(SettingGroup group) noexcept;

enum class SettingKind : std::uint8_t { Toggle, Choice, Slider };

/// How a row's change reaches the run.
enum class SettingApply : std::uint8_t {
    AtOnce,      ///< applied at once
    CommitFloor, ///< the floor shown is committed again (the lighting profile, the CRT effect)
    NextSession, ///< at the next VR session, as the row says
};

struct SettingRow {
    std::string_view section;
    std::string_view key;
    SettingGroup group = SettingGroup::Display;
    SettingKind kind = SettingKind::Toggle;
    /// A slider's range and step.
    double min = 0.0;
    double max = 1.0;
    double step = 0.1;
    /// A slider's value below `min`: null (the FOV's "della scena").
    bool nullable = false;
    /// A choice's values, in order.
    std::vector<nlohmann::json> choices;
    SettingApply apply = SettingApply::AtOnce;
    /// The runtime decides it in a headset (ADR-0018 §12).
    bool hiddenInVr = false;

    /// "<section>.<key>": the document's key for the row.
    [[nodiscard]] std::string id() const;
    /// The value is one the row can hold.
    [[nodiscard]] bool takes(const nlohmann::json& value) const;
};

/// Every row, in the screen's order; `lightingProfiles`: the profiles' ids (world::lightingProfileIds()).
[[nodiscard]] std::vector<SettingRow> settingRows(std::span<const std::string> lightingProfiles);

} // namespace rv::app
