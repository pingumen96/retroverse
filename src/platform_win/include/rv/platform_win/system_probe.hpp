#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/system_probe.hpp>

namespace rv::platform_win {

/// rv::core::ISystemProbe on the machine it runs on, read-only: registry keys are opened with read access only
/// (RegOpenKeyExW; the native 64-bit view, the 32-bit one through the paths that name WOW6432Node), files and folders
/// are queried and listed through their \\?\ form (any length), never opened for reading, and nothing is ever written.
/// Every call reads the machine again (no cache): it may run on any thread.
class Win32SystemProbe final : public core::ISystemProbe {
public:
    [[nodiscard]] std::optional<std::string> registryString(core::RegistryRoot root, std::string_view keyPath,
                                                            std::string_view valueName) const override;
    [[nodiscard]] bool registryKeyExists(core::RegistryRoot root, std::string_view keyPath) const override;
    [[nodiscard]] std::vector<std::string> registrySubKeys(core::RegistryRoot root,
                                                           std::string_view keyPath) const override;
    [[nodiscard]] bool fileExists(const std::filesystem::path& path) const override;
    [[nodiscard]] std::vector<std::filesystem::path> filesMatching(const std::filesystem::path& folder,
                                                                   std::string_view pattern) const override;
    [[nodiscard]] std::optional<std::filesystem::path> homeFolder() const override;
    [[nodiscard]] std::filesystem::path system32Folder() const override;
    [[nodiscard]] std::filesystem::path windowsFolder() const override;
    [[nodiscard]] std::vector<std::filesystem::path> pathEntries() const override;
    /// IShellLinkW and IPersistFile in a COM apartment of the calling thread's (made for the call when it has none):
    /// loaded for reading (STGM_READ), never resolved (no search, no tracking, no user interface) and never saved.
    /// The target as IShellLinkW::GetPath gives it, environment variables expanded (shortcut.cpp, I11 step 13).
    [[nodiscard]] Expected<core::ShellShortcut> readShortcut(const std::filesystem::path& path) const override;
};

} // namespace rv::platform_win
