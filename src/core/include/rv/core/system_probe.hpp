#pragma once

// The system probe (ADR-0008 §2): what the catalog asks of the machine it runs on, behind a port so that tests describe
// a machine with maps (tests/support/fake_system_probe.hpp) and the product asks Windows (rv::platform_win::
// Win32SystemProbe). Read-only: nothing here writes to the registry or the file system. The rules ES-DE applies on top
// of the raw values (App Paths precedence, the uninstall entry) are free functions over the port, so that they are the
// same on every probe; the executable search order is composed by the launch translation (I2 step 10).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::core {

/// What a Windows shortcut (a .lnk file) says, read without being resolved (I11 step 13, ADR-0020 §3).
struct ShellShortcut {
    /// The file it opens, absolute, environment variables expanded (as the shell opens it).
    std::filesystem::path target;
    /// Its argument string, as the shell passes it on: split as CommandLineToArgvW splits it.
    std::string arguments;
    /// Its working folder; empty when it names none.
    std::filesystem::path workingDirectory;
    /// It asks to run as an administrator (the shortcut's "Run as administrator").
    bool runAsAdministrator = false;
};

/// The registry hives the probe reads.
enum class RegistryRoot {
    CurrentUser,  ///< HKEY_CURRENT_USER
    LocalMachine, ///< HKEY_LOCAL_MACHINE
};

/// "HKCU" or "HKLM".
[[nodiscard]] std::string_view toString(RegistryRoot root) noexcept;

/// The port. Key paths are relative to the hive, '\' between the names ("SOFTWARE\Microsoft\Windows"), compared
/// ignoring case as the registry does; 64-bit processes see the native view, the 32-bit one through "WOW6432Node".
class ISystemProbe {
public:
    virtual ~ISystemProbe() = default;

    /// A string value: REG_SZ as it is, REG_EXPAND_SZ with its environment variables expanded. An empty `valueName`
    /// reads the key's default value. Nothing when the key or the value is missing or the value is not a string.
    [[nodiscard]] virtual std::optional<std::string> registryString(RegistryRoot root, std::string_view keyPath,
                                                                    std::string_view valueName) const = 0;
    /// True when the key exists (with or without values).
    [[nodiscard]] virtual bool registryKeyExists(RegistryRoot root, std::string_view keyPath) const = 0;
    /// The names of the key's sub-keys, sorted by their UTF-8 bytes; empty when the key is missing.
    [[nodiscard]] virtual std::vector<std::string> registrySubKeys(RegistryRoot root,
                                                                   std::string_view keyPath) const = 0;

    /// True when `path` names an existing file (not a folder), links followed.
    [[nodiscard]] virtual bool fileExists(const std::filesystem::path& path) const = 0;
    /// The files of `folder` (not its sub-folders) whose names match `pattern`, where '*' stands for any run of
    /// characters and '?' for one, compared ignoring case as the file system does; full paths sorted by the UTF-8
    /// bytes of their names. Empty when the folder is missing.
    [[nodiscard]] virtual std::vector<std::filesystem::path> filesMatching(const std::filesystem::path& folder,
                                                                           std::string_view pattern) const = 0;

    /// The user's home folder as ES-DE finds it on Windows (HOMEDRIVE and HOMEPATH, else USERPROFILE), the default
    /// parent of the ES-DE home; nothing when neither is set.
    [[nodiscard]] virtual std::optional<std::filesystem::path> homeFolder() const = 0;
    /// The System32 folder (GetSystemDirectoryW).
    [[nodiscard]] virtual std::filesystem::path system32Folder() const = 0;
    /// The Windows folder (GetWindowsDirectoryW).
    [[nodiscard]] virtual std::filesystem::path windowsFolder() const = 0;
    /// The entries of the PATH environment variable in their order, surrounding quotes removed; empty and relative
    /// entries (which would depend on the current directory) are left out.
    [[nodiscard]] virtual std::vector<std::filesystem::path> pathEntries() const = 0;

    /// The shortcut file `path` (a .lnk), read only: nothing is resolved, searched for or written (I11 step 13).
    /// Errors: NotFound (no such file), ValidationFailed (not a shortcut, or one whose target is no file: a shell
    /// folder, a store app), Unsupported (a probe that reads no shortcuts: the default), PlatformError.
    [[nodiscard]] virtual Expected<ShellShortcut> readShortcut(const std::filesystem::path& path) const;
};

/// The App Paths entry of an executable file name ("retroarch.exe"), as ES-DE's find rules read it: the default value
/// of SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\<name> under HKCU, and under HKLM only when the HKCU key does
/// not exist (a HKCU key without a default value hides HKLM's). Surrounding quotes are removed. Nothing when not
/// registered, or registered without a value.
[[nodiscard]] std::optional<std::filesystem::path> appPath(const ISystemProbe& probe, std::string_view executableName);

/// An installed program's uninstall entry (SOFTWARE\[WOW6432Node\]Microsoft\Windows\CurrentVersion\Uninstall\<key>).
struct UninstallEntry {
    RegistryRoot root = RegistryRoot::LocalMachine;
    std::string keyPath;        ///< relative to the hive
    std::string displayName;    ///< DisplayName ("ES-DE 3.4.1")
    std::string displayVersion; ///< DisplayVersion ("3.4.1"), empty when missing
    /// InstallLocation, when set and not empty.
    std::optional<std::filesystem::path> installLocation;
    /// The program of DisplayIcon, its quotes and icon index (",0") removed, when set.
    std::optional<std::filesystem::path> displayIcon;

    /// Where the program is installed: InstallLocation, else the folder of DisplayIcon's program.
    [[nodiscard]] std::optional<std::filesystem::path> installFolder() const;
};

/// The uninstall entry of the program named `displayName` ("ES-DE"): an entry whose DisplayName is that name, or that
/// name followed by a space and the entry's DisplayVersion ("ES-DE 3.4.1"; the ES-DE installer writes it so), compared
/// ignoring ASCII case. Searched under HKCU, then HKLM, each in the native view and then under WOW6432Node, the
/// sub-keys in their sorted order; the first match wins.
[[nodiscard]] std::optional<UninstallEntry> findUninstallEntry(const ISystemProbe& probe,
                                                               std::string_view displayName);

/// True when `name` (a file name, no folder) matches `pattern` as ISystemProbe::filesMatching matches: '*' any run of
/// characters, '?' exactly one, the rest compared with the file-system casing. Malformed UTF-8 matches nothing.
[[nodiscard]] bool matchesWildcard(std::string_view name, std::string_view pattern);

} // namespace rv::core
