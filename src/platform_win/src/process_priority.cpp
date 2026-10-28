#include <rv/platform_win/process_priority.hpp>
#include <rv/platform_win/win32_util.hpp>

namespace rv::platform_win {

std::string_view toString(ProcessPriority priority) noexcept {
    switch (priority) {
    case ProcessPriority::Normal:
        return "normal";
    case ProcessPriority::AboveNormal:
        return "above-normal";
    case ProcessPriority::Other:
        return "other";
    }
    return "other";
}

ProcessPriority currentProcessPriority() noexcept {
    switch (GetPriorityClass(GetCurrentProcess())) {
    case NORMAL_PRIORITY_CLASS:
        return ProcessPriority::Normal;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return ProcessPriority::AboveNormal;
    default:
        return ProcessPriority::Other;
    }
}

Status setProcessPriority(ProcessPriority priority) {
    if (priority == ProcessPriority::Other) {
        return makeError(ErrorCode::InvalidArgument, "only normal or above-normal can be set", "process priority");
    }
    const DWORD priorityClass =
        priority == ProcessPriority::AboveNormal ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    if (SetPriorityClass(GetCurrentProcess(), priorityClass) == FALSE) {
        return makeError(ErrorCode::PlatformError, lastErrorMessage(), "process priority");
    }
    return ok();
}

} // namespace rv::platform_win
