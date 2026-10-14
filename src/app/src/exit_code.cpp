#include <rv/app/exit_code.hpp>

namespace rv::app {

std::string_view toString(ExitCode code) noexcept {
    switch (code) {
    case ExitCode::Ok:
        return "ok";
    case ExitCode::SmokeCheckFailed:
        return "smoke check failed";
    case ExitCode::InitializationFailed:
        return "initialization failed";
    case ExitCode::RuntimeFailure:
        return "unexpected runtime failure";
    }
    return "unknown";
}

bool SelfTestOutcome::passed() const noexcept {
    return errorFlagRaised && (!debugOutputActive || messagesCaptured > 0);
}

ExitCode exitCodeForCompletedRun(const RunSummary& summary) noexcept {
    if (!summary.smoke) {
        return ExitCode::Ok;
    }
    std::uint64_t provoked = 0;
    if (summary.selfTest) {
        if (!summary.selfTest->passed()) {
            return ExitCode::SmokeCheckFailed;
        }
        provoked = summary.selfTest->highSeverityCaptured;
    }
    return summary.highSeverityGlMessages > provoked ? ExitCode::SmokeCheckFailed : ExitCode::Ok;
}

} // namespace rv::app
