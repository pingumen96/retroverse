#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <rv/launcher/launch_request.hpp>

#include "program_stub.hpp"
#include "support/temp_dir.hpp"

using rv::ErrorCode;
using rv::launcher::effectiveStrategy;
using rv::launcher::LaunchRequest;
using rv::launcher::parseWaitStrategy;
using rv::launcher::validateLaunchRequest;
using rv::launcher::WaitStrategy;
using rv::launcher::workingDirectoryOf;

namespace {

/// A file with `text` in it.
std::filesystem::path touch(const std::filesystem::path& path, const char* text = "") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
    return path;
}

using rv::test::writeProgramStub;

LaunchRequest requestFor(const std::filesystem::path& executable) {
    LaunchRequest request;
    request.executable = executable;
    request.arguments = {"-L", "core.dll", "I:\\Games\\Some Game (USA).gb"};
    return request;
}

} // namespace

TEST_CASE("launcher/launch_request: a valid request", "[launcher][launch_request]") {
    rv::test::TempDir dir("rv-launch-request");
    const auto emulator = writeProgramStub(dir.file("Emu Folder/emulator.exe"));
    LaunchRequest request = requestFor(emulator);
    CHECK(validateLaunchRequest(request).has_value());
    CHECK(workingDirectoryOf(request) == emulator.parent_path());

    request.workingDirectory = dir.path();
    request.environment = {{"EMU_HOME", "x"}, {"REMOVED", std::nullopt}};
    request.outputLog = dir.file("logs/launch.log");
    CHECK(validateLaunchRequest(request).has_value());
    CHECK(workingDirectoryOf(request) == dir.path());

    request.executable = writeProgramStub(dir.file("tool.COM"), 0x014C); // a 32-bit program
    CHECK(validateLaunchRequest(request).has_value());
}

TEST_CASE("launcher/launch_request: refused executables", "[launcher][launch_request]") {
    rv::test::TempDir dir("rv-launch-request");
    const auto check = [&](const std::filesystem::path& executable, ErrorCode code) {
        INFO(executable.string());
        const auto result = validateLaunchRequest(requestFor(executable));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == code);
        CHECK(result.error().context.starts_with("executable"));
    };
    check(dir.file("missing.exe"), ErrorCode::NotFound);
    check(touch(dir.file("wrapper.bat")), ErrorCode::Unsupported); // cmd.exe would re-parse the arguments
    check(touch(dir.file("wrapper.CMD")), ErrorCode::Unsupported);
    check(touch(dir.file("shortcut.lnk")), ErrorCode::Unsupported);
    check(touch(dir.file("noextension")), ErrorCode::Unsupported);
    // Files named like programs that Windows cannot run as such.
    check(touch(dir.file("text.exe"), "not a program"), ErrorCode::Unsupported);
    check(touch(dir.file("empty.exe")), ErrorCode::Unsupported);
    check(touch(dir.file("dos.com"), "MZ only a DOS header, no PE signature................................"),
          ErrorCode::Unsupported);
    check(writeProgramStub(dir.file("arm32.exe"), 0x01C4), ErrorCode::Unsupported);
    std::filesystem::create_directories(dir.file("folder.exe"));
    check(dir.file("folder.exe"), ErrorCode::ValidationFailed);
    check(std::filesystem::path("relative\\emulator.exe"), ErrorCode::ValidationFailed);
    check(std::filesystem::path(), ErrorCode::ValidationFailed);
}

TEST_CASE("launcher/launch_request: refused arguments, directories and environment", "[launcher][launch_request]") {
    rv::test::TempDir dir("rv-launch-request");
    const auto emulator = writeProgramStub(dir.file("emulator.exe"));

    LaunchRequest request = requestFor(emulator);
    CHECK(validateLaunchRequest(request).has_value()); // only the change below makes it invalid
    request.arguments.push_back(std::string("nul\0inside", 10));
    CHECK_FALSE(validateLaunchRequest(request).has_value());

    request = requestFor(emulator);
    request.arguments.push_back("\xC3\x28");
    CHECK_FALSE(validateLaunchRequest(request).has_value());

    request = requestFor(emulator);
    request.workingDirectory = dir.file("missing");
    const auto missingDirectory = validateLaunchRequest(request);
    REQUIRE_FALSE(missingDirectory.has_value());
    CHECK(missingDirectory.error().code == ErrorCode::NotFound);

    request = requestFor(emulator);
    request.environment = {{"BAD=NAME", "x"}};
    CHECK_FALSE(validateLaunchRequest(request).has_value());

    request = requestFor(emulator);
    request.outputLog = "relative.log";
    CHECK_FALSE(validateLaunchRequest(request).has_value());
}

TEST_CASE("launcher/launch_request: wait strategies", "[launcher][launch_request]") {
    CHECK(parseWaitStrategy("direct") == WaitStrategy::DirectProcess);
    CHECK(parseWaitStrategy("TREE") == WaitStrategy::ProcessTree);
    CHECK(parseWaitStrategy("confirm") == WaitStrategy::UserConfirmReturn);
    CHECK(parseWaitStrategy("ProcessTree") == WaitStrategy::ProcessTree);
    CHECK_FALSE(parseWaitStrategy("shell").has_value());
    CHECK(parseWaitStrategy(rv::launcher::toString(WaitStrategy::UserConfirmReturn)) ==
          WaitStrategy::UserConfirmReturn);

    // Without its tree followed (no job object), ProcessTree falls back to the user's confirmation.
    CHECK(effectiveStrategy(WaitStrategy::ProcessTree, true) == WaitStrategy::ProcessTree);
    CHECK(effectiveStrategy(WaitStrategy::ProcessTree, false) == WaitStrategy::UserConfirmReturn);
    CHECK(effectiveStrategy(WaitStrategy::DirectProcess, false) == WaitStrategy::DirectProcess);
    CHECK(effectiveStrategy(WaitStrategy::UserConfirmReturn, true) == WaitStrategy::UserConfirmReturn);
}
