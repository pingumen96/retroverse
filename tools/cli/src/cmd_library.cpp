// retroverse-cli library (I2 step 2): the library profile, <user data>/config/library.json (ADR-0008 §4, §6).
//  - library [show] [--json] [--output <file>]: prints the profile (its id, adapter, ES-DE home and install folder,
//    the overrides) and the ROM and media roots it gives with ES-DE's settings. Exit codes: 0, 1 when there is no
//    library.json, 2 when the user data directory lies inside the library's folders, 3 when library.json cannot be
//    read or is invalid.
//  - library create [--id <slug>] [--home <dir>] [--install <dir>] [--rom-root <dir>] [--media-root <dir>] [--force]
//    [--json]: discovers ES-DE (--home and --install as discover takes them), resolves the ROM and media roots (from
//    the settings, else the options must give them), refuses a user data directory inside the library's folders
//    (checkDataDirectory with esde::readOnlyFolders: the ES-DE home and install folder, the ROM and media roots, the
//    ROM folders of systems outside them) and writes library.json atomically; an existing library.json, also one
//    created meanwhile, is replaced only with --force. Exit codes: 0, 2 for invalid options and what they must change
//    (no ES-DE home, a root that cannot be resolved, a data directory inside the library, library.json already there),
//    3 when it cannot be written.
// Both start the log file (startFileLog) only once the user data directory has passed that check: until then, and when
// it fails, nothing is written.
// The catalog database, the authority on the id, does not exist before I2 step 4: from then on the import refuses a
// library.json whose id differs from the database's.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/platform_win/system_probe.hpp>

#include "commands.hpp"
#include "esde_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli library";

std::string text(const std::filesystem::path& path) {
    return pathText(path);
}

/// The roots a profile gives: its overrides, else ES-DE's settings (nothing when those cannot be read).
struct Roots {
    std::optional<esde::ResolvedRoot> rom;
    std::optional<esde::ResolvedRoot> media;
    /// Why the settings could not be read, when they could not.
    std::string settingsError;

    /// "unknown (...)": what a root without settings reads as.
    [[nodiscard]] std::string unknown() const {
        return fmt::format("unknown (the settings cannot be read: {})", settingsError);
    }
};

Roots rootsOf(const catalog::LibraryProfile& profile) {
    Roots roots;
    const auto settings = esde::readSettings(profile.esdeHome);
    if (settings) {
        roots.rom = esde::resolveRomRoot(*settings, profile);
        roots.media = esde::resolveMediaRoot(*settings, profile);
        return roots;
    }
    // Overrides need no settings.
    roots.settingsError = settings.error().toString();
    const esde::EsdeSettings none;
    if (profile.romRoot) {
        roots.rom = esde::resolveRomRoot(none, profile);
    }
    if (profile.mediaRoot) {
        roots.media = esde::resolveMediaRoot(none, profile);
    }
    return roots;
}

/// The folders of the library a profile describes, which Retroverse only reads (esde::readOnlyFolders()): its ES-DE home
/// and install folder, the roots, the ROM folders of its systems outside them (when the systems can be read).
std::vector<std::filesystem::path> foldersOf(const catalog::LibraryProfile& profile, const Roots& roots) {
    const auto systems = esde::loadSystems(profile);
    return esde::readOnlyFolders(profile, roots.rom, roots.media, systems ? &*systems : nullptr);
}

/// Why checkDataDirectory() refused the user data directory.
std::string insideMessage(const Error& error) {
    return fmt::format("the user data directory may not lie inside the library's folders: {}", error.toString());
}

/// Prints a profile and the roots it gives (rootsOf()), as text or JSON.
void writeProfile(Output& out, const std::filesystem::path& file, const catalog::LibraryProfile& profile,
                  const Roots& roots, bool json) {
    if (json) {
        const Json report{{"file", text(file)},
                          {"id", profile.id.str()},
                          {"adapter", std::string(catalog::toString(profile.adapter))},
                          {"esdeHome", text(profile.esdeHome)},
                          {"esdeInstall", pathJson(profile.installFolder)},
                          {"romRoot", pathJson(profile.romRoot)},
                          {"mediaRoot", pathJson(profile.mediaRoot)},
                          {"effectiveRomRoot", rootJson(roots.rom)},
                          {"effectiveMediaRoot", rootJson(roots.media)}};
        out.write(report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
        return;
    }
    const auto line = [&](std::string_view label, const std::string& value) {
        out.writeLine(fmt::format("{:<18}{}", label, value));
    };
    line("Library", fmt::format("{} ({})", profile.id.str(), text(file)));
    line("Adapter", std::string(catalog::toString(profile.adapter)));
    line("ES-DE home", text(profile.esdeHome));
    line("Install folder", profile.installFolder ? text(*profile.installFolder) : std::string("not known"));
    line("ROM root", rootText(roots.rom, "ROMDirectory", roots.unknown()));
    line("Media root", rootText(roots.media, "MediaDirectory", roots.unknown()));
}

/// The output a command's --output option names, else the context's.
struct Destination {
    std::optional<Output> file;
    Output* out = nullptr;
};

ExitCode finish(Destination& destination, CommandContext& context, ExitCode code) {
    if (destination.file) {
        if (const Status written = destination.file->finish(); !written) {
            context.err.writeLine(fmt::format("{}: {}", kName, written.error().toString()));
            return ExitCode::RuntimeFailure;
        }
    }
    return code;
}

ExitCode show(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser(std::string(kName) + " show", "Prints the library profile and the roots it gives.");
    parser.flag("json", "Print the results as JSON.");
    addOutputOption(parser);
    const auto args = parser.parse(arguments);
    if (!args) {
        context.err.writeLine(fmt::format("{} show: {}", kName, args.error().toString()));
        return ExitCode::UsageError;
    }
    if (args->has("help")) {
        context.out.write(parser.helpText());
        return ExitCode::Ok;
    }
    auto output = outputFileOption(*args);
    if (!output) {
        context.err.writeLine(fmt::format("{} show: {}", kName, output.error().toString()));
        return ExitCode::UsageError;
    }
    Destination destination{std::move(*output), nullptr};
    destination.out = destination.file ? &*destination.file : &context.out;

    const std::filesystem::path file = context.paths.configDir() / esde::kLibraryFileName;
    const auto profile = esde::readLibraryFile(file);
    if (!profile) {
        startFileLog(context); // no library: no folders to keep out of
        if (profile.error().code == ErrorCode::NotFound) {
            context.err.writeLine(fmt::format("{}: no library yet ({} does not exist): run 'retroverse-cli library "
                                              "create'",
                                              kName, text(file)));
            return ExitCode::ProblemsFound;
        }
        context.err.writeLine(fmt::format("{}: {}", kName, profile.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    const Roots roots = rootsOf(*profile);
    if (const Status allowed = checkDataDirectory(context, foldersOf(*profile, roots)); !allowed) {
        context.err.writeLine(fmt::format("{} show: {}", kName, insideMessage(allowed.error())));
        return ExitCode::UsageError;
    }
    startFileLog(context);
    writeProfile(*destination.out, file, *profile, roots, args->has("json"));
    return finish(destination, context, ExitCode::Ok);
}

ExitCode create(CommandContext& context, std::span<const std::string> arguments) {
    core::ArgParser parser(std::string(kName) + " create",
                           "Discovers ES-DE and records the library in <user data>/config/library.json.");
    parser.option("id", "slug", "The library's id: 1 to 32 characters among a-z, 0-9 and '-'.", "esde")
        .option("home", "dir", "ES-DE's home folder, as ES-DE's own --home takes it: the ES-DE home is <dir>\\ES-DE.")
        .option("install", "dir", "ES-DE's install folder, the folder of ES-DE.exe (default: its uninstall entry).")
        .option("rom-root", "dir", "Replaces ES-DE's ROMDirectory.")
        .option("media-root", "dir", "Replaces ES-DE's MediaDirectory.")
        .flag("force", "Replace an existing library.json.")
        .flag("json", "Print the library as JSON.");
    addOutputOption(parser);
    const auto args = parser.parse(arguments);
    if (!args) {
        context.err.writeLine(fmt::format("{} create: {}", kName, args.error().toString()));
        return ExitCode::UsageError;
    }
    if (args->has("help")) {
        context.out.write(parser.helpText());
        return ExitCode::Ok;
    }
    const auto usage = [&](const std::string& message) {
        context.err.writeLine(fmt::format("{} create: {}", kName, message));
        return ExitCode::UsageError;
    };
    auto output = outputFileOption(*args);
    auto home = folderOption(*args, "home");
    auto install = folderOption(*args, "install");
    auto romRoot = folderOption(*args, "rom-root");
    auto mediaRoot = folderOption(*args, "media-root");
    for (const Error* error :
         {output ? nullptr : &output.error(), home ? nullptr : &home.error(), install ? nullptr : &install.error(),
          romRoot ? nullptr : &romRoot.error(), mediaRoot ? nullptr : &mediaRoot.error()}) {
        if (error != nullptr) {
            return usage(error->toString());
        }
    }
    auto id = catalog::LibraryId::parse(args->value("id").value_or(std::string(catalog::LibraryId::kDefault)));
    if (!id) {
        return usage(id.error().toString());
    }

    esde::DiscoveryOptions options;
    options.homeFolder = *home;
    options.installFolder = *install;
    const platform_win::Win32SystemProbe probe;
    const auto discovery = esde::discoverEsde(options, probe);
    if (!discovery) {
        return usage(discovery.error().toString());
    }
    if (!discovery->esdeHome || !discovery->esdeHomeExists) {
        std::string reason = "no ES-DE home was found";
        for (const catalog::Issue& issue : discovery->issues) {
            if (issue.code == "home-missing") {
                reason = fmt::format("the ES-DE home {} does not exist ({})", text(*discovery->esdeHome),
                                     issue.reason);
            }
        }
        return usage(reason + ": give ES-DE's home folder with --home");
    }
    // What the library is created without (an install folder) or despite (an ignored portable.txt).
    for (const catalog::Issue& issue : discovery->issues) {
        context.err.writeLine(fmt::format("{} create: {}: {}", kName, catalog::toString(issue.severity), issue.reason));
    }
    auto profile = esde::profileOf(*discovery, std::move(*id));
    if (!profile) {
        return usage(profile.error().toString());
    }
    profile->romRoot = *romRoot;
    profile->mediaRoot = *mediaRoot;

    // Retroverse writes nowhere in what it reads (ADR-0008 §6, §7): the roots must be known to be kept out of.
    const Roots roots = rootsOf(*profile);
    for (const auto& [root, kind, setting, option] :
         {std::tuple{&roots.rom, "ROM", "ROMDirectory", "--rom-root"},
          std::tuple{&roots.media, "media", "MediaDirectory", "--media-root"}}) {
        if (!*root || !(*root)->path) {
            return usage(fmt::format("the library's {} root is {}: give it with {}", kind,
                                     rootText(*root, setting, roots.unknown()), option));
        }
    }
    if (const Status allowed = checkDataDirectory(context, foldersOf(*profile, roots)); !allowed) {
        return usage(insideMessage(allowed.error()));
    }
    startFileLog(context);

    const std::filesystem::path file = context.paths.configDir() / esde::kLibraryFileName;
    const bool force = args->has("force");
    std::error_code error;
    const bool present = std::filesystem::exists(file, error);
    if (error) {
        context.err.writeLine(fmt::format("{} create: cannot tell whether {} exists: {}", kName, text(file),
                                          error.message()));
        return ExitCode::RuntimeFailure;
    }
    const std::string exists = fmt::format("{} exists already: --force replaces it", text(file));
    if (present && !force) {
        return usage(exists);
    }
    Destination destination{std::move(*output), nullptr};
    destination.out = destination.file ? &*destination.file : &context.out;
    // Without --force, a library.json created meanwhile (another library create) is kept too.
    if (const Status written =
            esde::writeLibraryFile(file, *profile, force ? esde::ExistingFile::Replace : esde::ExistingFile::Keep);
        !written) {
        if (written.error().code == ErrorCode::Conflict) {
            return usage(exists);
        }
        context.err.writeLine(fmt::format("{} create: {}", kName, written.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    spdlog::info("library create: library '{}' written", profile->id.str());
    writeProfile(*destination.out, file, *profile, roots, args->has("json"));
    return finish(destination, context, ExitCode::Ok);
}

} // namespace

ExitCode runLibrary(CommandContext& context) {
    const std::span<const std::string> arguments = context.arguments;
    // The action comes first; without one (or before an option) the library is shown.
    if (arguments.empty() || arguments.front().starts_with('-')) {
        return show(context, arguments);
    }
    if (arguments.front() == "show") {
        return show(context, arguments.subspan(1));
    }
    if (arguments.front() == "create") {
        return create(context, arguments.subspan(1));
    }
    context.err.writeLine(fmt::format("{}: unknown action '{}': use show or create", kName, arguments.front()));
    return ExitCode::UsageError;
}

} // namespace rv::cli
