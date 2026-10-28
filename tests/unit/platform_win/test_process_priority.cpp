// rv_platform_win's process priority (I5 step 2, ADR-0016 §2): the classes set and read back, the test process left at
// the class it had on every path (a guard sets it again, whatever class it was and however the case ends).

#include <rv/platform_win/process_priority.hpp>

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using rv::platform_win::ProcessPriority;

namespace {

/// The test process's priority class when made, set again when destroyed (any class: through the system directly).
class PriorityClassGuard {
public:
    PriorityClassGuard() noexcept : m_class(GetPriorityClass(GetCurrentProcess())) {}
    ~PriorityClassGuard() {
        if (m_class != 0) {
            SetPriorityClass(GetCurrentProcess(), m_class);
        }
    }
    PriorityClassGuard(const PriorityClassGuard&) = delete;
    PriorityClassGuard& operator=(const PriorityClassGuard&) = delete;

    [[nodiscard]] DWORD priorityClass() const noexcept { return m_class; }

private:
    DWORD m_class;
};

} // namespace

TEST_CASE("platform_win/process_priority: above normal and normal are set and read back", "[platform_win]") {
    const PriorityClassGuard guard;
    REQUIRE(guard.priorityClass() != 0);
    REQUIRE(rv::platform_win::setProcessPriority(ProcessPriority::AboveNormal).has_value());
    CHECK(rv::platform_win::currentProcessPriority() == ProcessPriority::AboveNormal);
    REQUIRE(rv::platform_win::setProcessPriority(ProcessPriority::Normal).has_value());
    CHECK(rv::platform_win::currentProcessPriority() == ProcessPriority::Normal);
    const auto other = rv::platform_win::setProcessPriority(ProcessPriority::Other);
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(rv::platform_win::toString(ProcessPriority::AboveNormal) == "above-normal");
    CHECK(rv::platform_win::toString(ProcessPriority::Normal) == "normal");
}
