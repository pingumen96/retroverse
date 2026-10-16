#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rv/launcher/environment.hpp>
#include <rv/launcher/exit_status.hpp>

using rv::launcher::classifyExitCode;
using rv::launcher::describeExitCode;
using rv::launcher::EnvironmentOverride;
using rv::launcher::EnvironmentVariable;
using rv::launcher::ExitKind;
using rv::launcher::mergeEnvironment;
using rv::launcher::sameVariableName;
using rv::launcher::validateOverride;

TEST_CASE("launcher/environment: overrides set, replace and remove variables", "[launcher][environment]") {
    const std::vector<EnvironmentVariable> inherited{
        {"Path", R"(C:\Windows)"}, {"=C:", R"(C:\Games)"}, {"TEMP", R"(C:\Temp)"}, {"emu_home", "old"}};
    const std::vector<EnvironmentOverride> overrides{
        {"EMU_HOME", "new"},           // same name in another case: the value changes, the spelling stays
        {"TEMP", std::nullopt},        // removed
        {"ADDED", "\xC3\xA9"},         // added
        {"later", "first"},
        {"LATER", "second"},           // a later override of the same name wins
        {"MISSING", std::nullopt},     // removing what is not there changes nothing
        {"EMPTY", ""},                 // an empty value is a value
    };
    const std::vector<EnvironmentVariable> merged = mergeEnvironment(inherited, overrides);
    const std::vector<EnvironmentVariable> expected{
        {"=C:", R"(C:\Games)"}, {"ADDED", "\xC3\xA9"}, {"EMPTY", ""},
        {"emu_home", "new"},    {"later", "second"},   {"Path", R"(C:\Windows)"},
    };
    CHECK(merged == expected); // sorted by name without regard to case, as CreateProcessW asks
}

TEST_CASE("launcher/environment: names compare without regard to ASCII case", "[launcher][environment]") {
    CHECK(sameVariableName("Path", "PATH"));
    CHECK(sameVariableName("rv_x", "RV_X"));
    CHECK_FALSE(sameVariableName("PATH", "PATH2"));
    CHECK_FALSE(sameVariableName("\xC3\xA9", "\xC3\x89")); // only ASCII letters fold
}

TEST_CASE("launcher/environment: invalid overrides", "[launcher][environment]") {
    CHECK(validateOverride({"NAME", "value"}).has_value());
    CHECK(validateOverride({"NAME", std::nullopt}).has_value());
    CHECK_FALSE(validateOverride({"", "value"}).has_value());
    CHECK_FALSE(validateOverride({"A=B", "value"}).has_value());
    CHECK_FALSE(validateOverride({"=C:", "C:\\"}).has_value());
    CHECK_FALSE(validateOverride({std::string("A\0B", 3), "value"}).has_value());
    CHECK_FALSE(validateOverride({"NAME", std::string("a\0b", 3)}).has_value());
    CHECK_FALSE(validateOverride({"NAME", "\xC3\x28"}).has_value());
}

TEST_CASE("launcher/exit_status: exit codes are classified", "[launcher][exit_status]") {
    struct Case {
        std::uint32_t code;
        ExitKind kind;
    };
    const Case cases[] = {
        {0, ExitKind::Success},
        {1, ExitKind::ErrorCode},
        {3, ExitKind::ErrorCode},           // abort()
        {0xFFFFFFFFu, ExitKind::ErrorCode}, // exit(-1)
        {0xDEADu, ExitKind::ErrorCode},
        {0xC0000005u, ExitKind::Crash},     // access violation
        {0xC0000409u, ExitKind::Crash},     // fail fast, /GS
        {0xC00000FDu, ExitKind::Crash},     // stack overflow
        {0xC0000374u, ExitKind::Crash},     // heap corruption
        {0x80000003u, ExitKind::Crash},     // breakpoint without a debugger
        {0xE06D7363u, ExitKind::Crash},     // unhandled C++ exception
        {0xC0001234u, ExitKind::Crash},     // another system status error
        {0xC0020001u, ExitKind::ErrorCode}, // another facility: a program's own code
        {0xC0000135u, ExitKind::LoaderError},
        {0xC000007Bu, ExitKind::LoaderError},
        {0xC0000142u, ExitKind::LoaderError},
        {0xC000013Au, ExitKind::Interrupted},
        {0x40010004u, ExitKind::Interrupted},
    };
    for (const Case& c : cases) {
        INFO("code " << c.code);
        CHECK(classifyExitCode(c.code).kind == c.kind);
    }
    CHECK(classifyExitCode(0xC0000005u).name == "STATUS_ACCESS_VIOLATION");
    CHECK(classifyExitCode(7).name.empty());
}

TEST_CASE("launcher/exit_status: exit codes are described for people", "[launcher][exit_status]") {
    CHECK(describeExitCode(0) == "0 (success)");
    CHECK(describeExitCode(1) == "1 (error code)");
    CHECK(describeExitCode(0xC0000005u) == "0xC0000005 STATUS_ACCESS_VIOLATION (crash)");
    CHECK(describeExitCode(0xC0000135u) == "0xC0000135 STATUS_DLL_NOT_FOUND (loader error)");
    CHECK(describeExitCode(0xFFFFFFFFu) == "0xFFFFFFFF (error code)");
}
