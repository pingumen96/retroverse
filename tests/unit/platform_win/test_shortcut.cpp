// I11 step 13 (ADR-0020 §3): Win32SystemProbe::readShortcut on shortcuts made at test time.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <rv/core/system_probe.hpp>
#include <rv/platform_win/system_probe.hpp>

#include "support/program_stub.hpp"
#include "support/shortcut_file.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::platform_win::Win32SystemProbe;

namespace {

/// Writes a shortcut, its error shown when it cannot be written.
void shortcut(const fs::path& file, const fs::path& target, const std::string& arguments = {},
              const fs::path& folder = {}, bool administrator = false) {
    const rv::Status written = rv::test::writeShortcut(file, target, arguments, folder, administrator);
    INFO((written ? std::string() : written.error().toString()));
    REQUIRE(written);
}

} // namespace

TEST_CASE("platform_win/shortcut: a shortcut read as it says, never resolved", "[platform_win][shortcut]") {
    rv::test::TempDir dir("rv-shortcut");
    const Win32SystemProbe probe;
    // The shell refuses to save a shortcut to a program stub (a PE of headers alone): the shortcuts are written before
    // their target exists, as a shortcut to a program installed later is.
    const fs::path program = dir.file("Games/Kettle Kit/kettle.exe");
    const fs::path folder = program.parent_path() / "data";
    fs::create_directories(folder);

    SECTION("its target, arguments, folder and administrator flag") {
        const fs::path file = dir.file("Kettle Kit.lnk");
        shortcut(file, program, "-w \"two words\" --última", folder, true);
        rv::test::writeProgramStub(program);
        const auto shortcut = probe.readShortcut(file);
        REQUIRE(shortcut);
        CHECK(fs::equivalent(shortcut->target, program));
        CHECK(shortcut->arguments == "-w \"two words\" --última");
        CHECK(shortcut->workingDirectory == folder);
        CHECK(shortcut->runAsAdministrator);
    }
    SECTION("a target that does not exist keeps its path; the shortcut is not written") {
        const fs::path file = dir.file("Gone.lnk");
        shortcut(file, program);
        const auto written = fs::last_write_time(file);
        const auto size = fs::file_size(file);
        const auto shortcut = probe.readShortcut(file);
        REQUIRE(shortcut);
        CHECK(shortcut->target.filename() == "kettle.exe");
        CHECK(shortcut->arguments.empty());
        CHECK(shortcut->workingDirectory.empty());
        CHECK_FALSE(shortcut->runAsAdministrator);
        CHECK(fs::last_write_time(file) == written);
        CHECK(fs::file_size(file) == size);
    }
    SECTION("what is no shortcut") {
        CHECK(probe.readShortcut(dir.file("missing.lnk")).error().code == rv::ErrorCode::NotFound);
        CHECK(probe.readShortcut(fs::path("relative.lnk")).error().code == rv::ErrorCode::NotFound);
        const fs::path text = dir.file("text.lnk");
        std::ofstream(text) << "not a shortcut";
        CHECK(probe.readShortcut(text).error().code == rv::ErrorCode::ValidationFailed);
    }
    SECTION("from a thread without COM") {
        const fs::path file = dir.file("Kettle Kit.lnk");
        shortcut(file, program);
        bool read = false;
        std::thread([&] { read = probe.readShortcut(file).has_value(); }).join();
        CHECK(read);
    }
}

TEST_CASE("core/system_probe: a probe that reads no shortcuts says so", "[core][shortcut]") {
    struct Bare final : rv::core::ISystemProbe {
        std::optional<std::string> registryString(rv::core::RegistryRoot, std::string_view,
                                                  std::string_view) const override {
            return std::nullopt;
        }
        bool registryKeyExists(rv::core::RegistryRoot, std::string_view) const override { return false; }
        std::vector<std::string> registrySubKeys(rv::core::RegistryRoot, std::string_view) const override { return {}; }
        bool fileExists(const fs::path&) const override { return false; }
        std::vector<fs::path> filesMatching(const fs::path&, std::string_view) const override { return {}; }
        std::optional<fs::path> homeFolder() const override { return std::nullopt; }
        fs::path system32Folder() const override { return {}; }
        fs::path windowsFolder() const override { return {}; }
        std::vector<fs::path> pathEntries() const override { return {}; }
    };
    CHECK(Bare{}.readShortcut("C:\\Games\\x.lnk").error().code == rv::ErrorCode::Unsupported);
}
