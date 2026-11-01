#pragma once

// Private to rv_cli_core (I2 step 10): what `profiles`, `launch` and `assign` share: the library they translate commands
// for (library::LaunchLibrary, since I7 step 2 the app's too), and how choices, translations and blockers read as text
// and as JSON.

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/paths.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/library/launch_library.hpp>
#include <rv/platform_win/system_probe.hpp>

#include "esde_text.hpp"

namespace rv::cli {

using library::LaunchLibrary;
using library::kAssignedOrigin;
using library::kEmulatorsJsonOrigin;
using library::loadAssignedChoices;
using library::readSystemGamelist;
using library::systemChoiceOrigin;
using library::systemChoices;

/// Reads the library <user data>/config/library.json records and the emulator profiles. On failure the message goes to
/// the context's stderr and `failure` says how the command exits: UsageError without a library.json (or with one that
/// is not valid: library create writes it), RuntimeFailure when ES-DE's settings or systems cannot be read.
inline std::unique_ptr<LaunchLibrary> loadLaunchLibrary(CommandContext& context, std::string_view name,
                                                        ExitCode& failure) {
    const std::filesystem::path file = context.paths.configDir() / esde::kLibraryFileName;
    auto profile = esde::readLibraryFile(file);
    if (!profile) {
        context.err.writeLine(fmt::format("{}: no library: {} (run 'retroverse-cli library create' first)", name,
                                          profile.error().toString()));
        failure = ExitCode::UsageError;
        return nullptr;
    }
    auto library = library::loadLaunchLibrary(std::move(*profile), context.paths);
    if (!library) {
        context.err.writeLine(fmt::format("{}: {}", name, library.error().message));
        failure = ExitCode::RuntimeFailure;
        return nullptr;
    }
    return std::move(*library);
}

/// "command \"bsnes\"", "profile 'retroarch'", "the command" (a single unlabelled one).
inline std::string choiceText(const launcher::LaunchChoice& choice) {
    if (choice.kind == launcher::LaunchChoiceKind::Profile) {
        return fmt::format("profile '{}'", choice.value);
    }
    return choice.value.empty() ? std::string("the command") : fmt::format("command \"{}\"", choice.value);
}

inline Json choiceJson(const launcher::LaunchChoice& choice) {
    return Json{{"kind", std::string(launcher::toString(choice.kind))}, {"value", choice.value}};
}

/// Why a choice was made, for people: the source, with where Retroverse's choice came from ("Retroverse's choice for the
/// system (assign)") when `origin` says it.
inline std::string whyText(const catalog::ChosenLaunch& chosen, std::string_view origin = {}) {
    const bool retroverse = chosen.source == catalog::ChoiceSource::RetroverseGame ||
                            chosen.source == catalog::ChoiceSource::RetroverseSystem;
    if (!retroverse || origin.empty()) {
        return std::string(catalog::describe(chosen.source));
    }
    return fmt::format("{} ({})", catalog::describe(chosen.source), origin);
}

/// `origin`: where Retroverse's choice came from (kAssignedOrigin, kEmulatorsJsonOrigin), for a Retroverse choice; it
/// becomes "origin" (null otherwise) and part of "why".
inline Json chosenJson(const catalog::ChosenLaunch& chosen, std::string_view origin = {}) {
    Json json = choiceJson(chosen.choice);
    json["source"] = std::string(catalog::toString(chosen.source));
    const bool retroverse = chosen.source == catalog::ChoiceSource::RetroverseGame ||
                            chosen.source == catalog::ChoiceSource::RetroverseSystem;
    json["origin"] = retroverse && !origin.empty() ? Json(std::string(origin)) : Json(nullptr);
    json["why"] = whyText(chosen, origin);
    json["warnings"] = chosen.warnings;
    return json;
}

/// "<path> (winregistrypath \"retroarch.exe\", HKLM)".
inline std::string foundText(const catalog::FoundLocation& found) {
    return fmt::format("{} ({} \"{}\"{})", pathText(found.path), found.rule, found.entry,
                       found.detail.empty() ? std::string() : ", " + found.detail);
}

inline Json foundJson(const std::optional<catalog::FoundLocation>& found) {
    if (!found) {
        return nullptr;
    }
    return Json{
        {"path", pathText(found->path)}, {"rule", found->rule}, {"entry", found->entry}, {"detail", found->detail}};
}

inline Json blockersJson(const std::vector<catalog::ReadinessBlocker>& blockers) {
    Json json = Json::array();
    for (const catalog::ReadinessBlocker& blocker : blockers) {
        json.push_back(Json{{"readiness", std::string(catalog::toString(blocker.readiness))},
                            {"code", blocker.code},
                            {"message", blocker.message}});
    }
    return json;
}

/// A translation's readiness, program, core, blockers, attempts and notes as JSON.
inline Json translationJson(const catalog::LaunchTranslation& translation) {
    return Json{{"readiness", std::string(catalog::toString(translation.readiness()))},
                {"program", foundJson(translation.program)},
                {"core", foundJson(translation.core)},
                {"blockers", blockersJson(translation.blockers)},
                {"attempts", translation.attempts},
                {"notes", translation.notes}};
}

/// The lines of a translation's details, "<label><text>", the labels `width` wide after `indent`; `capitalized`: the
/// labels begin with a capital ("Program"), as the lines around them do.
inline void writeTranslationDetails(Output& out, const catalog::LaunchTranslation& translation,
                                    std::string_view indent, std::size_t width, bool capitalized) {
    const auto line = [&](std::string_view label, const std::string& text) {
        std::string name(label);
        if (capitalized && !name.empty()) {
            name.front() = static_cast<char>(name.front() - 'a' + 'A');
        }
        out.writeLine(fmt::format("{}{:<{}}{}", indent, name, width, text));
    };
    if (translation.program) {
        line("program", foundText(*translation.program));
    }
    if (translation.core) {
        line("core", foundText(*translation.core));
    }
    for (const catalog::ReadinessBlocker& blocker : translation.blockers) {
        line("blocked", fmt::format("{} ({}): {}", catalog::describe(blocker.readiness), blocker.code,
                                    blocker.message));
    }
    if (translation.readiness() == catalog::LaunchReadiness::EmulatorNotFound ||
        translation.readiness() == catalog::LaunchReadiness::CoreNotFound) {
        for (const std::string& attempt : translation.attempts) {
            line("tried", attempt);
        }
    }
    for (const std::string& note : translation.notes) {
        line("note", note);
    }
}

/// Writes `text` or `json` where the command's --output says, then finishes the file. False when it cannot be written
/// (the message on stderr).
inline bool finishOutput(CommandContext& context, std::optional<Output>& file, std::string_view name) {
    if (file) {
        if (const Status written = file->finish(); !written) {
            context.err.writeLine(fmt::format("{}: {}", name, written.error().toString()));
            return false;
        }
    }
    return true;
}

} // namespace rv::cli
