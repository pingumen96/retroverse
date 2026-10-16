#include <rv/launcher/exit_status.hpp>

#include <array>

#include <fmt/format.h>

namespace rv::launcher {
namespace {

struct KnownCode {
    std::uint32_t code;
    ExitKind kind;
    std::string_view name;
};

// Exit codes of processes ended by the system rather than by the program (ntstatus.h, winnt.h).
constexpr std::array kKnownCodes{
    KnownCode{0x80000002u, ExitKind::Crash, "STATUS_DATATYPE_MISALIGNMENT"},
    KnownCode{0x80000003u, ExitKind::Crash, "STATUS_BREAKPOINT"},
    KnownCode{0x80000004u, ExitKind::Crash, "STATUS_SINGLE_STEP"},
    KnownCode{0xC0000005u, ExitKind::Crash, "STATUS_ACCESS_VIOLATION"},
    KnownCode{0xC0000006u, ExitKind::Crash, "STATUS_IN_PAGE_ERROR"},
    KnownCode{0xC0000008u, ExitKind::Crash, "STATUS_INVALID_HANDLE"},
    KnownCode{0xC000001Du, ExitKind::Crash, "STATUS_ILLEGAL_INSTRUCTION"},
    KnownCode{0xC0000025u, ExitKind::Crash, "STATUS_NONCONTINUABLE_EXCEPTION"},
    KnownCode{0xC0000026u, ExitKind::Crash, "STATUS_INVALID_DISPOSITION"},
    KnownCode{0xC000008Cu, ExitKind::Crash, "STATUS_ARRAY_BOUNDS_EXCEEDED"},
    KnownCode{0xC000008Du, ExitKind::Crash, "STATUS_FLOAT_DENORMAL_OPERAND"},
    KnownCode{0xC000008Eu, ExitKind::Crash, "STATUS_FLOAT_DIVIDE_BY_ZERO"},
    KnownCode{0xC000008Fu, ExitKind::Crash, "STATUS_FLOAT_INEXACT_RESULT"},
    KnownCode{0xC0000090u, ExitKind::Crash, "STATUS_FLOAT_INVALID_OPERATION"},
    KnownCode{0xC0000091u, ExitKind::Crash, "STATUS_FLOAT_OVERFLOW"},
    KnownCode{0xC0000092u, ExitKind::Crash, "STATUS_FLOAT_STACK_CHECK"},
    KnownCode{0xC0000093u, ExitKind::Crash, "STATUS_FLOAT_UNDERFLOW"},
    KnownCode{0xC0000094u, ExitKind::Crash, "STATUS_INTEGER_DIVIDE_BY_ZERO"},
    KnownCode{0xC0000095u, ExitKind::Crash, "STATUS_INTEGER_OVERFLOW"},
    KnownCode{0xC0000096u, ExitKind::Crash, "STATUS_PRIVILEGED_INSTRUCTION"},
    KnownCode{0xC00000FDu, ExitKind::Crash, "STATUS_STACK_OVERFLOW"},
    KnownCode{0xC0000374u, ExitKind::Crash, "STATUS_HEAP_CORRUPTION"},
    KnownCode{0xC0000409u, ExitKind::Crash, "STATUS_STACK_BUFFER_OVERRUN"},
    KnownCode{0xC0000417u, ExitKind::Crash, "STATUS_INVALID_CRUNTIME_PARAMETER"},
    KnownCode{0xC000041Du, ExitKind::Crash, "STATUS_FATAL_USER_CALLBACK_EXCEPTION"},
    KnownCode{0xC0000420u, ExitKind::Crash, "STATUS_ASSERTION_FAILURE"},
    KnownCode{0xE06D7363u, ExitKind::Crash, "unhandled C++ exception"},
    KnownCode{0xE0434352u, ExitKind::Crash, "unhandled .NET exception"},
    KnownCode{0xC0000135u, ExitKind::LoaderError, "STATUS_DLL_NOT_FOUND"},
    KnownCode{0xC0000138u, ExitKind::LoaderError, "STATUS_ORDINAL_NOT_FOUND"},
    KnownCode{0xC0000139u, ExitKind::LoaderError, "STATUS_ENTRYPOINT_NOT_FOUND"},
    KnownCode{0xC000007Bu, ExitKind::LoaderError, "STATUS_INVALID_IMAGE_FORMAT"},
    KnownCode{0xC0000142u, ExitKind::LoaderError, "STATUS_DLL_INIT_FAILED"},
    KnownCode{0xC0000017u, ExitKind::Crash, "STATUS_NO_MEMORY"},
    KnownCode{0xC000013Au, ExitKind::Interrupted, "STATUS_CONTROL_C_EXIT"},
    KnownCode{0x40010004u, ExitKind::Interrupted, "DBG_TERMINATE_PROCESS"},
};

} // namespace

std::string_view toString(ExitKind kind) noexcept {
    switch (kind) {
    case ExitKind::Success:
        return "success";
    case ExitKind::ErrorCode:
        return "error code";
    case ExitKind::Crash:
        return "crash";
    case ExitKind::LoaderError:
        return "loader error";
    case ExitKind::Interrupted:
        return "interrupted";
    }
    return "unknown";
}

ExitStatus classifyExitCode(std::uint32_t code) noexcept {
    if (code == 0) {
        return ExitStatus{code, ExitKind::Success, {}};
    }
    for (const KnownCode& known : kKnownCodes) {
        if (known.code == code) {
            return ExitStatus{code, known.kind, known.name};
        }
    }
    // Severity "error" (top bits 11), customer bit clear, facility 0: the system's own status codes.
    if ((code & 0xFFFF0000u) == 0xC0000000u) {
        return ExitStatus{code, ExitKind::Crash, {}};
    }
    return ExitStatus{code, ExitKind::ErrorCode, {}};
}

std::string describeExitCode(std::uint32_t code) {
    const ExitStatus status = classifyExitCode(code);
    const std::string number = code >= 0x10000u ? fmt::format("0x{:08X}", code) : fmt::format("{}", code);
    if (status.name.empty()) {
        return fmt::format("{} ({})", number, toString(status.kind));
    }
    return fmt::format("{} {} ({})", number, status.name, toString(status.kind));
}

} // namespace rv::launcher
