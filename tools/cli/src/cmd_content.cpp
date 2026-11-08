// retroverse-cli content (I11 step 7, ADR-0019 §7): the user's console models in <user data>/content/consoles. The one
// command that writes there: add-console copies a model the user downloaded or authored (normalized by
// retroverse-content's Blender script) into its system's folder with its console.json; check and list read. The
// models are the user's own, for personal use: nothing here ever leaves the data directory, nothing is downloaded.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/assets/console_content.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli content";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] content add-console --system <name> --model <file.glb> --source <url>\n"
    "                                               --author <name> --license <id> [--kind home|handheld|computer]\n"
    "                                               [--front +z|-z|+x|-x] [--scale-cm <w,h,d>] [--date <YYYY-MM-DD>]\n"
    "                                               [--modifications <text>] [--replace]\n"
    "       retroverse-cli [global options] content check [--system <name>] [--json]\n"
    "       retroverse-cli [global options] content list [--json]\n"
    "\n"
    "The console models of <user data>/content/consoles (ADR-0019): a system's model stands in its console room in\n"
    "place of the procedural console when it passes the checks: each side 5 to 45 cm, a footprint of at most\n"
    "42.5 x 40 cm, at most 60,000 triangles, at most 2 MiB of textures once compressed. add-console copies a .glb\n"
    "into <system>/console.glb and writes console.json with its source, author and licence (the model is for personal\n"
    "use: never packaged, never shared); it refuses a model that fails the checks, and a folder that has one unless\n"
    "--replace. check reads every folder (or --system's) and says what fails; list shows them. Exit codes: 0, 1\n"
    "when a check fails, 2 for invalid arguments, 3 when files cannot be read or written.\n";

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = text.find(separator, start);
        parts.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

struct Checked {
    std::string system;
    std::optional<assets::ConsoleManifest> manifest;
    assets::ConsoleCheck check;
    std::string error;

    [[nodiscard]] bool ok() const { return error.empty() && check.ok(); }
};

Checked checkFolder(const std::filesystem::path& folder, std::string system) {
    Checked checked;
    checked.system = std::move(system);
    auto content = assets::loadConsoleContent(folder);
    if (!content) {
        checked.error = content.error().toString();
        return checked;
    }
    checked.manifest = content->manifest;
    checked.check = assets::checkConsoleModel(content->model, content->manifest);
    return checked;
}

std::string describe(const Checked& checked) {
    if (!checked.error.empty()) {
        return fmt::format("{}: unreadable: {}", checked.system, checked.error);
    }
    const assets::ConsoleCheck& check = checked.check;
    std::string line = fmt::format("{}: {} ({}): {:.1f} x {:.1f} x {:.1f} cm, {} triangle(s), {:.2f} MiB of textures",
                                   checked.system, check.ok() ? "ok" : "refused",
                                   assets::toString(checked.manifest->kind), check.size.x * 100.0F,
                                   check.size.y * 100.0F, check.size.z * 100.0F, check.triangles,
                                   static_cast<double>(check.textureBytes) / (1024.0 * 1024.0));
    const assets::ConsoleSourceInfo& source = checked.manifest->source;
    if (!source.author.empty() || !source.license.empty()) {
        line += fmt::format("; by {}, {}", source.author.empty() ? "?" : source.author,
                            source.license.empty() ? "?" : source.license);
    }
    for (const std::string& problem : check.problems) {
        line += "\n  - " + problem;
    }
    return line;
}

nlohmann::json toJson(const Checked& checked) {
    nlohmann::json entry{{"system", checked.system}, {"ok", checked.ok()}};
    if (!checked.error.empty()) {
        entry["error"] = checked.error;
        return entry;
    }
    entry["kind"] = std::string(assets::toString(checked.manifest->kind));
    entry["sizeCm"] = nlohmann::json::array(
        {checked.check.size.x * 100.0F, checked.check.size.y * 100.0F, checked.check.size.z * 100.0F});
    entry["triangles"] = checked.check.triangles;
    entry["textureBytes"] = checked.check.textureBytes;
    entry["problems"] = checked.check.problems;
    entry["source"] = nlohmann::json{{"url", checked.manifest->source.url},
                                     {"author", checked.manifest->source.author},
                                     {"license", checked.manifest->source.license}};
    return entry;
}

/// Every system folder of content/consoles, sorted by name (none without the folder).
std::vector<std::string> consoleSystems(const std::filesystem::path& contentDir) {
    std::vector<std::string> systems;
    std::error_code error;
    const std::filesystem::path root = contentDir / core::pathFromUtf8(assets::kConsolesFolder);
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.is_directory(error)) {
            systems.push_back(core::pathToUtf8Lossy(entry.path().filename()));
        }
    }
    std::sort(systems.begin(), systems.end());
    return systems;
}

ExitCode runReport(CommandContext& context, std::span<const std::string> arguments, bool check) {
    core::ArgParser parser{fmt::format("{} {}", kName, check ? "check" : "list")};
    if (check) {
        parser.option("system", "name", "Only this system's folder.");
    }
    parser.flag("json", "Write the results as JSON.");
    auto args = parser.parse(arguments);
    if (!args) {
        return usageError(context, args.error().toString());
    }
    if (args->has("help")) {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    std::vector<std::string> systems = consoleSystems(context.paths.contentDir());
    if (const std::optional<std::string> only = check ? args->value("system") : std::nullopt) {
        if (std::find(systems.begin(), systems.end(), *only) == systems.end()) {
            return usageError(context, fmt::format("content/consoles has no folder \"{}\"", *only));
        }
        systems = {*only};
    }
    std::vector<Checked> results;
    for (const std::string& system : systems) {
        results.push_back(checkFolder(assets::consoleFolder(context.paths.contentDir(), system), system));
    }
    const bool failed = std::any_of(results.begin(), results.end(), [](const Checked& c) { return !c.ok(); });
    if (args->has("json")) {
        nlohmann::json list = nlohmann::json::array();
        for (const Checked& checked : results) {
            list.push_back(toJson(checked));
        }
        context.out.writeLine(nlohmann::json{{"consoles", list}}.dump(2));
    } else if (results.empty()) {
        context.out.writeLine(fmt::format("No console models in {}", core::pathToUtf8Lossy(
                                                                         context.paths.contentDir() /
                                                                         core::pathFromUtf8(assets::kConsolesFolder))));
    } else {
        for (const Checked& checked : results) {
            context.out.writeLine(describe(checked));
        }
    }
    return check && failed ? ExitCode::ProblemsFound : ExitCode::Ok;
}

ExitCode runAddConsole(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser{fmt::format("{} add-console", kName)};
    parser.option("system", "name", "The ES-DE system whose console room shows it (snes, megadrive, ...).")
        .option("model", "file", "The console's model, a binary glTF (.glb), normalized: metres, front +Z, origin at "
                                 "the bottom's middle.")
        .option("source", "url", "Where the model comes from.")
        .option("author", "name", "Its author.")
        .option("license", "id", "Its licence (it only has to allow personal use).")
        .option("kind", "kind", "home, handheld or computer.", "home")
        .option("front", "axis", "The axis the console's front faces in the model: +z, -z, +x or -x.", "+z")
        .option("scale-cm", "w,h,d", "The real console's width, height and depth in centimetres, checked.")
        .option("date", "YYYY-MM-DD", "When it was taken.")
        .option("modifications", "text", "What was changed (normalized, decimated, retextured, ...).")
        .flag("replace", "Replace the system's console model.");
    auto args = parser.parse(arguments);
    if (!args) {
        return usageError(context, args.error().toString());
    }
    if (args->has("help")) {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    for (const char* required : {"system", "model", "source", "author", "license"}) {
        if (!args->value(required)) {
            return usageError(context, fmt::format("--{} is required", required));
        }
    }
    const std::string system = *args->value("system");
    if (!catalog::SystemName::isValid(system)) {
        return usageError(context, fmt::format("\"{}\" is no system name", system));
    }
    const std::filesystem::path model = core::pathFromUtf8(*args->value("model"));
    if (core::toLowerAscii(core::pathToUtf8Lossy(model.extension())) != ".glb") {
        return usageError(context, "--model must be a binary glTF (.glb): one file, its textures inside");
    }
    assets::ConsoleManifest manifest;
    const auto kind = assets::consoleKindFromString(*args->value("kind"));
    const auto front = assets::consoleFrontFromString(*args->value("front"));
    if (!kind || !front) {
        return usageError(context, !kind ? "--kind must be home, handheld or computer"
                                         : "--front must be +z, -z, +x or -x");
    }
    manifest.kind = *kind;
    manifest.front = *front;
    if (const auto scale = args->value("scale-cm")) {
        glm::vec3 cm(0.0F);
        const std::vector<std::string> parts = split(*scale, ',');
        bool valid = parts.size() == 3;
        for (std::size_t axis = 0; valid && axis < 3; ++axis) {
            try {
                std::size_t used = 0;
                cm[static_cast<int>(axis)] = std::stof(parts[axis], &used);
                valid = used == parts[axis].size() && cm[static_cast<int>(axis)] > 0.0F;
            } catch (const std::exception&) {
                valid = false;
            }
        }
        if (!valid) {
            return usageError(context, "--scale-cm must be three positive numbers: width,height,depth");
        }
        manifest.scaleCm = cm;
    }
    manifest.source = assets::ConsoleSourceInfo{*args->value("source"), *args->value("author"),
                                                *args->value("license"), args->value("date").value_or(""),
                                                args->value("modifications").value_or("")};

    assets::GltfLoadOptions options;
    options.maxFileBytes = 64ull << 20;
    auto loaded = assets::loadGltf(model, options);
    if (!loaded) {
        context.err.writeLine(fmt::format("{}: the model cannot be read: {}", kName, loaded.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    const assets::ConsoleCheck check = assets::checkConsoleModel(*loaded, manifest);
    Checked checked{system, manifest, check, {}};
    if (!check.ok()) {
        context.out.writeLine(describe(checked));
        context.err.writeLine(fmt::format("{}: the model is refused: nothing was written", kName));
        return ExitCode::ProblemsFound;
    }
    const std::filesystem::path folder = assets::consoleFolder(context.paths.contentDir(), system);
    std::error_code error;
    if (std::filesystem::exists(folder / core::pathFromUtf8(assets::kConsoleManifestName), error) &&
        !args->has("replace")) {
        return usageError(context, fmt::format("{} has a console model already: --replace replaces it", system));
    }
    std::filesystem::create_directories(folder, error);
    if (error) {
        context.err.writeLine(fmt::format("{}: {} cannot be made: {}", kName, core::pathToUtf8Lossy(folder),
                                          error.message()));
        return ExitCode::RuntimeFailure;
    }
    std::filesystem::copy_file(model, folder / core::pathFromUtf8(assets::kConsoleModelName),
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        context.err.writeLine(fmt::format("{}: the model cannot be copied: {}", kName, error.message()));
        return ExitCode::RuntimeFailure;
    }
    if (auto written = core::writeTextFileAtomic(folder / core::pathFromUtf8(assets::kConsoleManifestName),
                                                 assets::writeConsoleManifest(manifest));
        !written) {
        context.err.writeLine(fmt::format("{}: console.json cannot be written: {}", kName, written.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    context.out.writeLine(describe(checked));
    context.out.writeLine(fmt::format("Written to {}", core::pathToUtf8Lossy(folder)));
    return ExitCode::Ok;
}

} // namespace

ExitCode runContent(CommandContext& context) {
    if (context.arguments.empty()) {
        return usageError(context, "a subcommand is required: add-console, check or list");
    }
    const std::string& subcommand = context.arguments.front();
    if (subcommand == "--help" || subcommand == "-h") {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    const std::span<const std::string> rest = context.arguments.subspan(1);
    if (subcommand == "add-console") {
        return runAddConsole(context, rest);
    }
    if (subcommand == "check") {
        return runReport(context, rest, true);
    }
    if (subcommand == "list") {
        return runReport(context, rest, false);
    }
    return usageError(context,
                      fmt::format("unknown subcommand \"{}\" (there are: add-console, check, list)", subcommand));
}

} // namespace rv::cli
