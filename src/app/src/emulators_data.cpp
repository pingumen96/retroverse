#include <rv/app/emulators_data.hpp>

#include <utility>

#include <rv/core/utf8.hpp>

namespace rv::app {
namespace {

/// The first blocker of the readiness that decides, as the card shows it.
[[nodiscard]] std::string firstBlocker(const catalog::LaunchTranslation& translation) {
    const catalog::LaunchReadiness readiness = translation.readiness();
    for (const catalog::ReadinessBlocker& blocker : translation.blockers) {
        if (blocker.readiness == readiness) {
            return blocker.code;
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path normalized(std::filesystem::path path) {
    path.make_preferred();
    return path.lexically_normal();
}

[[nodiscard]] ui::EmulatorOption optionOf(const library::LaunchOption& source, const catalog::SystemName& system,
                                          const catalog::LaunchResolution& resolution,
                                          const VerifiedPrograms& verified) {
    ui::EmulatorOption option;
    const catalog::LaunchTranslation& translation = source.translation;
    option.profile = source.choice.kind == launcher::LaunchChoiceKind::Profile;
    option.value = source.choice.value;
    option.name = option.profile ? source.text : source.choice.value;
    option.readiness = std::string(catalog::toString(translation.readiness()));
    option.blocker = firstBlocker(translation);
    option.emulator = translation.emulator;
    if (translation.program) {
        option.program = core::pathToUtf8Lossy(translation.program->path);
        option.programChosen = translation.program->rule == "retroverse";
    }
    option.chosen = resolution.chosen.choice == source.choice && !resolution.chosen.blocker;
    option.verified = translation.blockers.empty() && verifiedWith(verified, system, source.choice, translation);
    return option;
}

} // namespace

std::tuple<std::string, int, std::string> verificationKey(const catalog::SystemName& system,
                                                          const launcher::LaunchChoice& choice) {
    return {system.str(), static_cast<int>(choice.kind), choice.value};
}

bool verifiedWith(const VerifiedPrograms& verified, const catalog::SystemName& system,
                  const launcher::LaunchChoice& choice, const catalog::LaunchTranslation& translation) {
    const auto found = verified.find(verificationKey(system, choice));
    return found != verified.end() && translation.program && sameProgram(found->second, translation.program->path);
}

bool sameProgram(const std::filesystem::path& left, const std::filesystem::path& right) {
    return normalized(left) == normalized(right);
}

ui::EmulatorSystem emulatorSystemOf(const library::SystemLaunchRow& row, std::size_t games,
                                    const VerifiedPrograms& verified, bool stored) {
    ui::EmulatorSystem system;
    system.name = row.system.name.str();
    system.fullName = row.system.fullName.empty() ? system.name : row.system.fullName;
    system.games = games;
    const catalog::LaunchResolution& resolution = row.resolution;
    system.readiness = std::string(catalog::toString(resolution.readiness()));
    system.blocker = firstBlocker(resolution.translation);
    system.source = std::string(catalog::toString(resolution.chosen.source));
    system.stored = stored;
    system.verified = resolution.translation.blockers.empty() &&
                      verifiedWith(verified, row.system.name, resolution.chosen.choice, resolution.translation);
    for (const library::LaunchOption& option : library::suggestedOptions(row)) {
        system.options.push_back(optionOf(option, row.system.name, resolution, verified));
        if (system.options.back().chosen) {
            system.choice = system.options.back().name;
        }
    }
    if (system.choice.empty()) {
        system.choice = resolution.chosen.choice.value; // a stored choice no option names: it says so itself
    }
    return system;
}

std::string testVerdictOf(const catalog::PlaySession& session, bool stored) {
    if (catalog::returnedNormally(session)) {
        return stored ? "verified" : "error";
    }
    switch (session.outcome) {
    case catalog::PlaySessionOutcome::Crashed:
        return "crashed";
    case catalog::PlaySessionOutcome::Blocked:
        return "blocked";
    case catalog::PlaySessionOutcome::LeftRunning:
        return "left-running";
    case catalog::PlaySessionOutcome::FailedToStart:
        return session.refusedByGuard ? "guard" : "failed";
    case catalog::PlaySessionOutcome::Returned:
        return session.quickExit ? "quick-exit" : "failed";
    default:
        return "error";
    }
}

} // namespace rv::app
