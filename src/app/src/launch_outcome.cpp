#include <rv/app/launch_outcome.hpp>

#include <fmt/format.h>

#include <rv/core/utf8.hpp>
#include <rv/launcher/exit_status.hpp>

namespace rv::app {

namespace lo = launch_outcome;

std::string exitCodeName(std::uint32_t code) {
    const launcher::ExitStatus status = launcher::classifyExitCode(code);
    return status.name.empty() ? fmt::format("0x{:08X}", code) : fmt::format("0x{:08X} {}", code, status.name);
}

std::string_view firstToken(std::string_view text) noexcept {
    const std::size_t open = text.find('{');
    if (open == std::string_view::npos) {
        return {};
    }
    const std::size_t close = text.find('}', open + 1);
    if (close == std::string_view::npos) {
        return {};
    }
    return text.substr(open, close - open + 1);
}

LaunchMessage messageForOutcome(const launcher::LaunchOutcome& outcome) {
    LaunchMessage message;
    switch (outcome.state) {
    case launcher::SessionState::Crashed:
        message.code = std::string(lo::kCrashed);
        message.detail = outcome.exitCode ? exitCodeName(*outcome.exitCode) : std::string();
        break;
    case launcher::SessionState::FailedToStart:
        message.code = std::string(lo::kFailedToStart);
        switch (outcome.failure) {
        case launcher::FailureReason::LoaderError:
            message.reason = std::string(lo::kReasonLoaderError);
            message.detail = outcome.exitCode ? exitCodeName(*outcome.exitCode) : outcome.message;
            break;
        case launcher::FailureReason::EarlyExit:
            message.reason = std::string(lo::kReasonEarlyExit);
            message.detail = outcome.exitCode ? launcher::describeExitCode(*outcome.exitCode) : outcome.message;
            break;
        case launcher::FailureReason::InvalidRequest:
            message.reason = std::string(lo::kReasonInvalidRequest);
            message.detail = outcome.message;
            break;
        case launcher::FailureReason::StartFailed:
        case launcher::FailureReason::None:
            message.reason = std::string(lo::kReasonStartFailed);
            message.detail = outcome.message;
            break;
        }
        break;
    default:
        if (outcome.confirmedByUser && outcome.processesRemained) {
            message.code = std::string(lo::kLeftRunning);
            message.reason = std::string(lo::kReasonConfirmed);
        } else if (outcome.quickExit) {
            message.code = std::string(lo::kQuickExit);
        } else {
            message.code = std::string(lo::kReturned);
        }
        message.detail = outcome.exitCode ? launcher::describeExitCode(*outcome.exitCode) : std::string();
        break;
    }
    return message;
}

LaunchMessage messageForBlocker(const launcher::LaunchBlocker& blocker, const launcher::LaunchTarget& target,
                                std::string_view profileName) {
    LaunchMessage message;
    switch (blocker.kind) {
    case launcher::BlockerKind::RomMissing:
        message.code = std::string(lo::kRomMissing);
        message.detail = core::pathToUtf8Lossy(target.rom.filename());
        break;
    case launcher::BlockerKind::TemplateError:
    case launcher::BlockerKind::VariableMissing: {
        message.code = std::string(lo::kNotConfigured);
        message.reason = std::string(lo::kReasonTemplate);
        const std::string_view token = firstToken(blocker.message);
        message.detail = token.empty() ? blocker.message : std::string(token);
        break;
    }
    case launcher::BlockerKind::ProgramRefused:
        message.code = std::string(lo::kNotConfigured);
        message.reason = std::string(lo::kReasonProgramRefused);
        message.detail = blocker.message;
        break;
    case launcher::BlockerKind::InvalidRequest:
        message.code = std::string(lo::kNotConfigured);
        message.reason = std::string(lo::kReasonInvalidRequest);
        message.detail = blocker.message;
        break;
    case launcher::BlockerKind::ExecutableNotFound:
        message.code = std::string(lo::kEmulatorMissing);
        message.detail = std::string(profileName);
        break;
    case launcher::BlockerKind::RequiredFileMissing:
        message.code = std::string(lo::kCoreMissing);
        message.detail = blocker.message;
        break;
    }
    return message;
}

bool showsMessage(const LaunchMessage& message) noexcept {
    if (message.code == lo::kReturned) {
        return false;
    }
    return !(message.code == lo::kLeftRunning && message.reason == lo::kReasonConfirmed);
}

} // namespace rv::app
