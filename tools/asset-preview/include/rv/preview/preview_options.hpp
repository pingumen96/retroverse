#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/app/command_line.hpp>
#include <rv/core/expected.hpp>
#include <rv/preview/view_mode.hpp>

namespace rv::preview {

/// Prefix of the procedural prefab targets: "prefab:<kind>".
inline constexpr std::string_view kPrefabPrefix = "prefab:";

/// Frames the panels take to settle: the developer overlay docks them in its first frame, brings the first tab of each
/// dock node forward in the second and the tabs asked for (--panel, Manifest) in the third, and the camera frames the
/// subject in the area they leave from the second. A --screenshot of an earlier frame shows them unfinished.
inline constexpr std::uint32_t kPanelsSettledFrames = 3;

/// What the asset preview shows: a glTF model file, a prefab manifest (style §11) or a procedural prefab of the
/// geometry kit.
struct PreviewTarget {
    enum class Kind : std::uint8_t { Model, Manifest, Prefab };

    Kind kind = Kind::Model;
    /// Kind::Model and Kind::Manifest: the file.
    std::filesystem::path file;
    /// Kind::Prefab: a kind of geometry::prefabKinds().
    std::string prefabKind;
    /// Kind::Prefab: the builder parameters, an object (--params; empty: the kind's defaults).
    nlohmann::json params = nlohmann::json::object();

    /// "<file>" for files (with '/' separators), "prefab:<kind>" for prefabs.
    [[nodiscard]] std::string label() const;
};

/// The target a command line argument or a file dropped on the window names: "prefab:<kind>" with a kind of
/// geometry::prefabKinds(), a .glb or .gltf file (a model) or a .json file (a manifest), extensions in any letter
/// case. Files are not opened here. Errors (InvalidArgument, context the text): an empty text, an unknown prefab kind,
/// another extension.
[[nodiscard]] Expected<PreviewTarget> parsePreviewTarget(std::string_view text);

/// The validated command line of retroverse-asset-preview.
struct PreviewOptions {
    /// The options of the host (app::addHostOptions: window, run length, directories, logging, screenshots). The tool
    /// always shows its panels, also in screenshots (devUiOpen and screenshotIncludeUi are set).
    app::CommandLineOptions run;
    /// What to show at start; none: an empty preview waiting for a dropped file.
    std::optional<PreviewTarget> target;
    /// --write-fixture <file.glb>: write the CRT-like test model there (makeCrtFixture) and exit, without a window.
    std::optional<std::filesystem::path> writeFixture;
    /// --view <mode>: the view mode at start (viewModeFromName), for screenshots of the inspection views.
    ViewMode view = ViewMode::Lit;
    /// --panel <title>: a panel brought to the front of its dock node at start, for screenshots of the tabs behind the
    /// first ones (Materials, Manifest, Stats, Log).
    std::optional<std::string> panel;
};

/// Parses the arguments of retroverse-asset-preview (UTF-8, program name excluded): an optional target
/// (parsePreviewTarget), --params '<json object>' for a prefab target, --view <mode>, --panel <title>,
/// --write-fixture <file.glb>, and the host options (with the errors of app::applyHostOptions). Errors name the
/// offending option: an unknown option or
/// a second target, a target parsePreviewTarget refuses, --params that is not a JSON object or without a prefab target,
/// an unknown --view, --write-fixture not ending in ".glb" or together with a target or a frame limit. With --help or
/// --version the remaining options are not validated.
[[nodiscard]] Expected<PreviewOptions> parsePreviewCommandLine(std::span<const std::string> arguments);
/// Convenience overload for main(): skips argv[0].
[[nodiscard]] Expected<PreviewOptions> parsePreviewCommandLine(int argc, const char* const* argv);

/// Usage text printed by --help: the target forms, the controls, every option and the exit codes.
[[nodiscard]] std::string previewHelp();

} // namespace rv::preview
