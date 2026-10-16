#pragma once

// A machine described by maps, for the code that asks rv::core::ISystemProbe (ES-DE discovery, find rules, emulator
// executables): registry keys and string values, files, the home, System32 and Windows folders, PATH.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/core/system_probe.hpp>

namespace rv::test {

class FakeSystemProbe final : public core::ISystemProbe {
public:
    /// Adds a key (and the keys above it, each listed as a sub-key of its parent). Key paths are matched ignoring case,
    /// as the registry does, and listed with the spelling first given.
    FakeSystemProbe& addRegistryKey(core::RegistryRoot root, std::string_view keyPath);
    /// Sets a string value, adding its key; an empty name is the key's default value. Returned as given: write the
    /// value of a REG_EXPAND_SZ already expanded.
    FakeSystemProbe& setRegistryString(core::RegistryRoot root, std::string_view keyPath, std::string_view valueName,
                                       std::string value);
    /// Adds a file (absolute path). fileExists() and filesMatching() see it; folders are implied.
    FakeSystemProbe& addFile(const std::filesystem::path& path);
    FakeSystemProbe& setHomeFolder(std::optional<std::filesystem::path> folder);
    FakeSystemProbe& setSystem32Folder(std::filesystem::path folder);
    FakeSystemProbe& setWindowsFolder(std::filesystem::path folder);
    /// The PATH entries as pathEntries() returns them (already absolute).
    FakeSystemProbe& setPathEntries(std::vector<std::filesystem::path> entries);

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

private:
    struct Key {
        /// Folded sub-key name -> its spelling.
        std::map<std::string, std::string> subKeys;
        /// Folded value name -> value.
        std::map<std::string, std::string> values;
    };
    using KeyId = std::pair<core::RegistryRoot, std::string>; ///< the folded key path

    [[nodiscard]] const Key* findKey(core::RegistryRoot root, std::string_view keyPath) const;

    std::map<KeyId, Key> m_keys;
    /// Folded parent folder -> the files in it (their paths as given).
    std::map<std::wstring, std::vector<std::filesystem::path>> m_files;
    std::optional<std::filesystem::path> m_home;
    std::filesystem::path m_system32{L"C:\\Windows\\System32"};
    std::filesystem::path m_windows{L"C:\\Windows"};
    std::vector<std::filesystem::path> m_pathEntries;
};

} // namespace rv::test
