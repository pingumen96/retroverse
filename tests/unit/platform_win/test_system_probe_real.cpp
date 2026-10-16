// The Win32 system probe against the real machine's ES-DE installation and emulators (ADR-0008 §10). Hidden
// ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the
// library before and after, and the paths in environment variables:
//   RV_REAL_ESDE_INSTALL   the ES-DE install folder: the uninstall entry must lead there
//   RV_REAL_RETROARCH_EXE  retroarch.exe: its App Paths entry must name it (optional)
// A variable that is not set skips its part. Nothing is written anywhere.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <windows.h>

#include <rv/core/paths.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/core/utf8.hpp>
#include <rv/platform_win/system_probe.hpp>

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// The same folder or file, compared as the file system compares names.
bool samePlace(const fs::path& a, const fs::path& b) {
    return rv::core::pathIsWithin(a, b) && rv::core::pathIsWithin(b, a);
}

} // namespace

TEST_CASE("platform_win/system_probe: the real ES-DE uninstall entry and RetroArch's App Paths",
          "[platform_win][system_probe][.real-library]") {
    const rv::platform_win::Win32SystemProbe probe;
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto retroArch = pathVariable(L"RV_REAL_RETROARCH_EXE");
    if (!install && !retroArch) {
        SKIP("set RV_REAL_ESDE_INSTALL and RV_REAL_RETROARCH_EXE (scripts/real-library.ps1 does)");
    }

    if (install) {
        const auto entry = rv::core::findUninstallEntry(probe, "ES-DE");
        REQUIRE(entry.has_value());
        INFO(rv::core::toString(entry->root) << "\\" << entry->keyPath << ": " << entry->displayName);
        CHECK_FALSE(entry->displayVersion.empty());
        const auto folder = entry->installFolder();
        REQUIRE(folder.has_value());
        CHECK(samePlace(*folder, *install));
        CHECK(probe.fileExists(*install / "ES-DE.exe"));
        // The bundled systems and find rules the adapter reads (I2 step 2).
        CHECK(probe.fileExists(*install / "resources" / "systems" / "windows" / "es_systems.xml"));
        CHECK(probe.fileExists(*install / "resources" / "systems" / "windows" / "es_find_rules.xml"));
    }
    if (retroArch) {
        const auto registered = rv::core::appPath(probe, "retroarch.exe");
        REQUIRE(registered.has_value());
        CHECK(samePlace(*registered, *retroArch));
        CHECK(probe.fileExists(*registered));
    }
}
