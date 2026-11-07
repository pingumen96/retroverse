#include <rv/launcher/process_runner.hpp>

namespace rv::launcher {

IRunningProcess::~IRunningProcess() = default;
IProcessRunner::~IProcessRunner() = default;

std::string_view toString(ProcessEventKind kind) noexcept {
    switch (kind) {
    case ProcessEventKind::MainExited:
        return "main process exited";
    case ProcessEventKind::ChildStarted:
        return "child started";
    case ProcessEventKind::ChildExited:
        return "child exited";
    case ProcessEventKind::TreeEmpty:
        return "process tree empty";
    case ProcessEventKind::WindowShown:
        return "window shown";
    case ProcessEventKind::WindowsGone:
        return "windows gone";
    case ProcessEventKind::WindowChanged:
        return "window changed";
    case ProcessEventKind::WatchFailed:
        return "watch failed";
    }
    return "unknown";
}

} // namespace rv::launcher
