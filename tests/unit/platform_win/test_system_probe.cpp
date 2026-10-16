// The Win32 system probe on what every Windows installation has: registry values of Windows itself, System32, the
// environment. Read-only: nothing is written to the registry or the file system (the PATH test changes this process's
// environment only, and puts it back).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include <windows.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/core/utf8.hpp>
#include <rv/platform_win/system_probe.hpp>

namespace fs = std::filesystem;
using rv::core::RegistryRoot;
using rv::platform_win::Win32SystemProbe;

namespace {

constexpr std::string_view kCurrentVersion = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

bool samePath(const fs::path& a, const fs::path& b) {
    const auto upperA = rv::core::fileSystemUpperCase(std::wstring_view(a.lexically_normal().native()));
    const auto upperB = rv::core::fileSystemUpperCase(std::wstring_view(b.lexically_normal().native()));
    return upperA && upperB && *upperA == *upperB;
}

std::optional<std::wstring> environmentVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return value;
}

/// Sets an environment variable of this process for the scope, then puts the old value back.
class ScopedVariable {
public:
    ScopedVariable(const wchar_t* name, const std::wstring& value) : m_name(name), m_old(environmentVariable(name)) {
        SetEnvironmentVariableW(name, value.c_str());
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ~ScopedVariable() { SetEnvironmentVariableW(m_name, m_old ? m_old->c_str() : nullptr); }

private:
    const wchar_t* m_name;
    std::optional<std::wstring> m_old;
};

} // namespace

TEST_CASE("platform_win/system_probe: registry strings", "[platform_win][system_probe]") {
    const Win32SystemProbe probe;
    // REG_SZ values Windows always has.
    const auto product = probe.registryString(RegistryRoot::LocalMachine, kCurrentVersion, "ProductName");
    REQUIRE(product.has_value());
    CHECK(product->starts_with("Windows"));
    const auto systemRoot = probe.registryString(RegistryRoot::LocalMachine, kCurrentVersion, "SystemRoot");
    REQUIRE(systemRoot.has_value());
    CHECK(samePath(rv::core::tryPathFromUtf8(*systemRoot).value(), probe.windowsFolder()));
    // Key and value names ignore case.
    CHECK(probe.registryString(RegistryRoot::LocalMachine, "software\\microsoft\\windows nt\\currentversion",
                               "productname") == product);

    // REG_EXPAND_SZ comes back expanded ("%SystemRoot%\TEMP").
    const auto temp = probe.registryString(RegistryRoot::LocalMachine,
                                           "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", "TEMP");
    REQUIRE(temp.has_value());
    CHECK(temp->find('%') == std::string::npos);
    CHECK_FALSE(temp->empty());

    // Not strings, or not there.
    CHECK_FALSE(probe.registryString(RegistryRoot::LocalMachine, kCurrentVersion, "InstallDate").has_value()); // DWORD
    CHECK_FALSE(probe.registryString(RegistryRoot::LocalMachine, kCurrentVersion, "No Such Value 7c1e").has_value());
    CHECK_FALSE(
        probe.registryString(RegistryRoot::LocalMachine, "SOFTWARE\\No Such Key 7c1e", "ProductName").has_value());
    CHECK_FALSE(probe.registryString(RegistryRoot::CurrentUser, "", "x").has_value());
}

TEST_CASE("platform_win/system_probe: registry keys and sub-keys", "[platform_win][system_probe]") {
    const Win32SystemProbe probe;
    CHECK(probe.registryKeyExists(RegistryRoot::LocalMachine, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion"));
    CHECK(probe.registryKeyExists(RegistryRoot::CurrentUser, "Software\\Microsoft"));
    CHECK_FALSE(probe.registryKeyExists(RegistryRoot::LocalMachine, "SOFTWARE\\No Such Key 7c1e"));
    const auto subKeys = probe.registrySubKeys(RegistryRoot::LocalMachine, "SOFTWARE\\Microsoft\\Windows");
    CHECK(std::is_sorted(subKeys.begin(), subKeys.end()));
    CHECK(std::find(subKeys.begin(), subKeys.end(), "CurrentVersion") != subKeys.end());
    CHECK(probe.registrySubKeys(RegistryRoot::LocalMachine, "SOFTWARE\\No Such Key 7c1e").empty());
}

TEST_CASE("platform_win/system_probe: files and wildcards", "[platform_win][system_probe]") {
    const Win32SystemProbe probe;
    const fs::path system32 = probe.system32Folder();
    REQUIRE_FALSE(system32.empty());
    CHECK(probe.fileExists(system32 / "kernel32.dll"));
    CHECK(probe.fileExists(system32 / "KERNEL32.DLL"));
    CHECK_FALSE(probe.fileExists(system32));
    CHECK_FALSE(probe.fileExists(system32 / "no such file 7c1e.dll"));

    const auto kernel = probe.filesMatching(system32, "KERNEL*.DLL");
    CHECK(std::find(kernel.begin(), kernel.end(), system32 / "kernel32.dll") != kernel.end());
    CHECK(std::is_sorted(kernel.begin(), kernel.end(), [](const fs::path& a, const fs::path& b) {
        return rv::core::pathToUtf8Lossy(a.filename()) < rv::core::pathToUtf8Lossy(b.filename());
    }));
    for (const fs::path& file : kernel) {
        CHECK(file.parent_path() == system32);
    }
    CHECK(probe.filesMatching(system32, "kernel32.dll").size() == 1);
    CHECK(probe.filesMatching(fs::path(system32.native() + L"\\"), "kernel32.dll").size() == 1); // a trailing separator
    CHECK(probe.filesMatching(system32 / "No Such Folder 7c1e", "*").empty());
    CHECK(probe.filesMatching(probe.windowsFolder(), "System32").empty()); // folders are not files
}

TEST_CASE("platform_win/system_probe: folders of the machine and the user", "[platform_win][system_probe]") {
    const Win32SystemProbe probe;
    const fs::path windows = probe.windowsFolder();
    REQUIRE_FALSE(windows.empty());
    CHECK(fs::is_directory(windows));
    CHECK(samePath(probe.system32Folder(), windows / "System32"));

    const auto home = probe.homeFolder();
    REQUIRE(home.has_value());
    const auto drive = environmentVariable(L"HOMEDRIVE");
    const auto path = environmentVariable(L"HOMEPATH");
    if (drive && path) {
        CHECK(home->native() == *drive + *path);
    }
    {
        const ScopedVariable noDrive(L"HOMEDRIVE", L"");
        const auto profile = environmentVariable(L"USERPROFILE");
        REQUIRE(profile.has_value());
        CHECK(probe.homeFolder() == fs::path(*profile));
    }
}

TEST_CASE("platform_win/system_probe: PATH keeps its absolute entries", "[platform_win][system_probe]") {
    const Win32SystemProbe probe;
    const ScopedVariable path(L"PATH", L"C:\\Tools;;relative\\bin; \"C:\\Program Files\\Emu\" ;.;\\rooted;D:drive;"
                                       L"\\\\nas\\share\\bin;C:\\Tools\\");
    CHECK(probe.pathEntries() == std::vector<fs::path>{fs::path(L"C:\\Tools"), fs::path(L"C:\\Program Files\\Emu"),
                                                       fs::path(L"\\\\nas\\share\\bin"), fs::path(L"C:\\Tools\\")});
}

TEST_CASE("platform_win/system_probe: App Paths and the uninstall key on the real registry",
          "[platform_win][system_probe]") {
    // Read-only lookups that hold on any machine: an executable nobody registers, a program nobody installs.
    const Win32SystemProbe probe;
    CHECK_FALSE(rv::core::appPath(probe, "no-such-program-7c1e.exe").has_value());
    CHECK_FALSE(rv::core::findUninstallEntry(probe, "No Such Program 7c1e").has_value());
}
