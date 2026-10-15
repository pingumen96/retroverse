#include <rv/preview/preview_options.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/core/arg_parser.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>

namespace rv::preview {
namespace {

constexpr std::string_view kDescription =
    "Asset preview of Retroverse (style guide 19.3): shows a glTF model, a prefab manifest or a procedural prefab on\n"
    "a neutral studio floor, with its bounds in centimeters, sockets, statistics, materials and manifest checks.\n"
    "\n"
    "Target: a .glb or .gltf file, a manifest .json file, or prefab:<kind> (one of room_shell, shelf, cartridge,\n"
    "        crt, tv_cabinet, console_cartridge, poster_frame, rug, floor_lamp, armchair) with --params.\n"
    "        A manifest next to a model (<model>.manifest.json or <model>.json) is checked with it.\n"
    "Controls: left drag orbits, middle or right drag pans, the wheel zooms; gamepad: right stick orbits, left stick\n"
    "          pans, the triggers zoom. F frames the subject (pressed again before the camera moves: with its sockets\n"
    "          and scale references), R toggles the auto-rotation, 1 to 5 pick the view (lit, base color, normals,\n"
    "          uv checker, wireframe). Drop a file on the window to preview it; the file is reloaded when it or a\n"
    "          file it references changes on disk. F1 hides the panels, F5 reloads the shaders, F12 saves a\n"
    "          screenshot to <user data>/screenshots; Esc closes a popup, else hides the panels, else quits.\n"
    "Screenshots: the panels settle over the first frames; --screenshot shows them laid out from --frames 3 on.\n"
    "Exit codes: 0 success; 1 --smoke run that stopped early, hit unexpected OpenGL errors or failed --gl-selftest;\n"
    "            2 initialization failure (command line, paths, SDL, window, OpenGL 4.5, --write-fixture not\n"
    "              written); 3 runtime failure (unexpected error, --screenshot not written, shader never built,\n"
    "              --run-report not written).";

core::ArgParser makeParser() {
    core::ArgParser parser("retroverse-asset-preview", std::string(kDescription));
    app::addHostOptions(parser);
    parser.positional("target", "file.glb | file.gltf | manifest.json | prefab:<kind>")
        .option("params", "json",
                "Builder parameters of a prefab: target, a JSON object such as {\"width\": 0.6} (the kind's "
                "parameters, see rv/geometry/prefabs.hpp).")
        .option("view", "mode",
                "View mode at start: lit (default), base-color, normals, uv-checker or wireframe (keys 1 to 5).")
        .option("panel", "title",
                "Bring the panel with this title to the front of its tabs at start, e.g. Materials (for screenshots; "
                "unknown titles bring nothing forward).")
        .option("write-fixture", "file.glb",
                "Write a small CRT-like test model (Body, ScreenSurface, Anchor_TV_Stand, Anchor_ScreenLook, "
                "ColliderProxy) to the file and exit.");
    return parser;
}

bool hasExtension(const std::filesystem::path& path, std::string_view extension) {
    return core::equalsIgnoreCaseAscii(core::pathToUtf8(path.extension()), extension);
}

/// --params: a JSON object (comments allowed), for a prefab target only.
Status applyParams(const core::ParsedArgs& args, PreviewOptions& options) {
    if (!args.has("params")) {
        return ok();
    }
    const std::string text = args.value("params").value_or("");
    if (!options.target || options.target->kind != PreviewTarget::Kind::Prefab) {
        return makeError(ErrorCode::InvalidArgument, "only valid with a prefab:<kind> target", "--params");
    }
    auto parsed = core::parseJson(text, "--params");
    if (!parsed) {
        return propagate(parsed.error());
    }
    if (!parsed->is_object()) {
        return makeError(ErrorCode::InvalidArgument, "expected a JSON object such as {\"width\": 0.6}",
                         "--params=" + text);
    }
    options.target->params = std::move(*parsed);
    return ok();
}

/// --write-fixture <file.glb>: written without a window, so neither a target nor a run of frames goes with it.
Status applyWriteFixture(const core::ParsedArgs& args, PreviewOptions& options) {
    if (!args.has("write-fixture")) {
        return ok();
    }
    const std::string text = args.value("write-fixture").value_or("");
    if (text.empty()) {
        return makeError(ErrorCode::InvalidArgument, "value must not be empty", "--write-fixture");
    }
    const std::filesystem::path path = core::pathFromUtf8(text);
    if (!hasExtension(path, ".glb")) {
        return makeError(ErrorCode::InvalidArgument, "the fixture is a binary glTF: use the .glb extension",
                         "--write-fixture=" + text);
    }
    if (options.target || options.run.frameLimit) {
        return makeError(ErrorCode::InvalidArgument,
                         "writes the fixture and exits: give neither a target nor a frame limit with it",
                         "--write-fixture");
    }
    options.writeFixture = path;
    return ok();
}

} // namespace

std::string PreviewTarget::label() const {
    return kind == Kind::Prefab ? std::string(kPrefabPrefix) + prefabKind : core::pathToGenericUtf8(file);
}

Expected<PreviewTarget> parsePreviewTarget(std::string_view text) {
    if (text.empty()) {
        return makeError(ErrorCode::InvalidArgument, "expected a file or prefab:<kind>", "target");
    }
    PreviewTarget target;
    if (text.starts_with(kPrefabPrefix)) {
        const std::string_view kind = text.substr(kPrefabPrefix.size());
        const auto kinds = geometry::prefabKinds();
        if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) {
            std::string known;
            for (const std::string_view name : kinds) {
                known += known.empty() ? "" : ", ";
                known += name;
            }
            return makeError(ErrorCode::InvalidArgument, "unknown prefab kind (expected one of " + known + ")",
                             std::string(text));
        }
        target.kind = PreviewTarget::Kind::Prefab;
        target.prefabKind = std::string(kind);
        return target;
    }
    target.file = core::pathFromUtf8(text);
    if (hasExtension(target.file, ".glb") || hasExtension(target.file, ".gltf")) {
        target.kind = PreviewTarget::Kind::Model;
    } else if (hasExtension(target.file, ".json")) {
        target.kind = PreviewTarget::Kind::Manifest;
    } else {
        return makeError(ErrorCode::InvalidArgument,
                         "expected a .glb or .gltf model, a manifest .json file or prefab:<kind>", std::string(text));
    }
    return target;
}

Expected<PreviewOptions> parsePreviewCommandLine(std::span<const std::string> arguments) {
    const core::ArgParser parser = makeParser();
    const auto args = parser.parse(arguments);
    if (!args) {
        return propagate(args.error());
    }
    PreviewOptions options;
    options.run.showHelp = args->has("help");
    options.run.showVersion = args->has("version");
    if (options.run.showHelp || options.run.showVersion) {
        return options;
    }
    if (auto host = app::applyHostOptions(*args, options.run); !host) {
        return propagate(host.error());
    }
    // The panels are the tool: always shown, in screenshots too.
    options.run.devUiOpen = true;
    options.run.screenshotIncludeUi = true;
    if (const std::optional<std::string> text = args->positional("target")) {
        auto target = parsePreviewTarget(*text);
        if (!target) {
            return propagate(target.error());
        }
        options.target = std::move(*target);
    }
    if (auto params = applyParams(*args, options); !params) {
        return propagate(params.error());
    }
    if (const std::optional<std::string> view = args->value("view")) {
        const std::optional<ViewMode> mode = viewModeFromName(*view);
        if (!mode) {
            return makeError(ErrorCode::InvalidArgument, "expected lit, base-color, normals, uv-checker or wireframe",
                             "--view=" + *view);
        }
        options.view = *mode;
    }
    if (const std::optional<std::string> panel = args->value("panel")) {
        if (panel->empty()) {
            return makeError(ErrorCode::InvalidArgument, "value must not be empty", "--panel");
        }
        options.panel = *panel;
    }
    if (auto fixture = applyWriteFixture(*args, options); !fixture) {
        return propagate(fixture.error());
    }
    return options;
}

Expected<PreviewOptions> parsePreviewCommandLine(int argc, const char* const* argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    return parsePreviewCommandLine(arguments);
}

std::string previewHelp() {
    return makeParser().helpText();
}

} // namespace rv::preview
