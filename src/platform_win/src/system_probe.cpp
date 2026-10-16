#include <rv/platform_win/system_probe.hpp>

#include <algorithm>
#include <utility>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/platform_win/win32_util.hpp>

namespace rv::platform_win {
namespace {

/// Closes a registry key on scope exit.
class RegistryKey {
public:
    RegistryKey() noexcept = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    ~RegistryKey() {
        if (m_key != nullptr) {
            RegCloseKey(m_key);
        }
    }
    [[nodiscard]] HKEY get() const noexcept { return m_key; }
    [[nodiscard]] HKEY* out() noexcept { return &m_key; }

private:
    HKEY m_key = nullptr;
};

HKEY hiveOf(core::RegistryRoot root) noexcept {
    return root == core::RegistryRoot::CurrentUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

/// Opens `keyPath` under `root` with `access` (read rights only); the status of RegOpenKeyExW.
LSTATUS openKey(core::RegistryRoot root, std::string_view keyPath, REGSAM access, RegistryKey& key) {
    const auto wide = core::utf8ToWide(keyPath);
    if (!wide || wide->empty()) {
        return ERROR_INVALID_PARAMETER;
    }
    return RegOpenKeyExW(hiveOf(root), wide->c_str(), 0, access | KEY_WOW64_64KEY, key.out());
}

/// The value of an environment variable; nothing when it is not set or empty.
std::optional<std::wstring> environmentVariable(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required <= 1) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

/// A folder the system reports through a (buffer, size) function such as GetSystemDirectoryW.
template <class Function>
std::filesystem::path systemFolder(Function function) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const UINT length = function(buffer.data(), static_cast<UINT>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(length + 1);
    }
    return {};
}

} // namespace

std::optional<std::string> Win32SystemProbe::registryString(core::RegistryRoot root, std::string_view keyPath,
                                                            std::string_view valueName) const {
    RegistryKey key;
    if (openKey(root, keyPath, KEY_QUERY_VALUE, key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    const auto name = core::utf8ToWide(valueName);
    if (!name) {
        return std::nullopt;
    }
    const wchar_t* valueNamePointer = name->empty() ? nullptr : name->c_str(); // the default value
    std::wstring data;
    DWORD type = 0;
    bool read = false;
    for (int attempt = 0; attempt < 4 && !read; ++attempt) {
        DWORD bytes = 0;
        if (RegQueryValueExW(key.get(), valueNamePointer, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ)) {
            return std::nullopt;
        }
        // Room for a terminating NUL the stored data may lack.
        data.assign(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD size = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        const LSTATUS status =
            RegQueryValueExW(key.get(), valueNamePointer, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size);
        if (status == ERROR_MORE_DATA) {
            continue; // the value grew meanwhile
        }
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
            return std::nullopt;
        }
        data.resize(size / sizeof(wchar_t));
        read = true;
    }
    if (!read) {
        return std::nullopt; // the value kept growing while it was read
    }
    while (!data.empty() && data.back() == L'\0') {
        data.pop_back();
    }
    if (type == REG_EXPAND_SZ && !data.empty()) {
        const DWORD needed = ExpandEnvironmentStringsW(data.c_str(), nullptr, 0);
        if (needed == 0) {
            return std::nullopt;
        }
        std::wstring expanded(needed, L'\0');
        const DWORD written = ExpandEnvironmentStringsW(data.c_str(), expanded.data(), needed);
        if (written == 0 || written > needed) {
            return std::nullopt;
        }
        expanded.resize(written - 1); // without the terminating NUL
        data = std::move(expanded);
    }
    auto utf8 = core::wideToUtf8(data);
    if (!utf8) {
        return std::nullopt;
    }
    return std::move(*utf8);
}

bool Win32SystemProbe::registryKeyExists(core::RegistryRoot root, std::string_view keyPath) const {
    RegistryKey key;
    const LSTATUS status = openKey(root, keyPath, KEY_QUERY_VALUE, key);
    // A key this user may not read still exists.
    return status == ERROR_SUCCESS || status == ERROR_ACCESS_DENIED;
}

std::vector<std::string> Win32SystemProbe::registrySubKeys(core::RegistryRoot root, std::string_view keyPath) const {
    std::vector<std::string> names;
    RegistryKey key;
    if (openKey(root, keyPath, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, key) != ERROR_SUCCESS) {
        return names;
    }
    DWORD longest = 0;
    if (RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, &longest, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr) != ERROR_SUCCESS) {
        return names;
    }
    std::wstring buffer(static_cast<std::size_t>(longest) + 1, L'\0');
    for (DWORD index = 0;; ++index) {
        DWORD length = static_cast<DWORD>(buffer.size());
        const LSTATUS status =
            RegEnumKeyExW(key.get(), index, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status == ERROR_MORE_DATA) {
            // A longer name was added meanwhile: grow and read the same index again.
            buffer.assign(buffer.size() * 2, L'\0');
            --index;
            continue;
        }
        if (status != ERROR_SUCCESS) {
            break;
        }
        if (auto name = core::wideToUtf8(std::wstring_view(buffer.data(), length))) {
            names.push_back(std::move(*name));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool Win32SystemProbe::fileExists(const std::filesystem::path& path) const {
    const std::wstring target = core::extendedLengthPath(path);
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    // A link: what it leads to, if anything.
    const UniqueHandle handle(CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    return handle.valid() && GetFileInformationByHandle(handle.get(), &info) &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::vector<std::filesystem::path> Win32SystemProbe::filesMatching(const std::filesystem::path& folder,
                                                                   std::string_view pattern) const {
    std::vector<std::pair<std::string, std::wstring>> matches; // UTF-8 name (the sort key), UTF-16 name
    // A \\?\ path is never normalized again: one separator before the pattern, also after a drive's root ("C:\").
    std::wstring listing = core::extendedLengthPath(folder);
    while (listing.size() > 4 && listing.ends_with(L'\\')) {
        listing.pop_back();
    }
    listing += L"\\*";
    WIN32_FIND_DATAW data{};
    const HANDLE find =
        FindFirstFileExW(listing.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        // Matched here rather than by FindFirstFileExW's own pattern, which also matches 8.3 short names.
        auto name = core::wideToUtf8(data.cFileName);
        if (name && core::matchesWildcard(*name, pattern)) {
            matches.emplace_back(std::move(*name), std::wstring(data.cFileName));
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(matches.begin(), matches.end());
    std::vector<std::filesystem::path> paths;
    paths.reserve(matches.size());
    for (const auto& [utf8, wide] : matches) {
        paths.push_back(folder / wide);
    }
    return paths;
}

std::optional<std::filesystem::path> Win32SystemProbe::homeFolder() const {
    const auto drive = environmentVariable(L"HOMEDRIVE");
    const auto path = environmentVariable(L"HOMEPATH");
    if (drive && path) {
        return std::filesystem::path(*drive + *path);
    }
    if (const auto profile = environmentVariable(L"USERPROFILE")) {
        return std::filesystem::path(*profile);
    }
    return std::nullopt;
}

std::filesystem::path Win32SystemProbe::system32Folder() const {
    return systemFolder([](wchar_t* buffer, UINT size) { return GetSystemDirectoryW(buffer, size); });
}

std::filesystem::path Win32SystemProbe::windowsFolder() const {
    return systemFolder([](wchar_t* buffer, UINT size) { return GetWindowsDirectoryW(buffer, size); });
}

std::vector<std::filesystem::path> Win32SystemProbe::pathEntries() const {
    std::vector<std::filesystem::path> entries;
    const auto value = environmentVariable(L"PATH");
    if (!value) {
        return entries;
    }
    std::size_t start = 0;
    while (start <= value->size()) {
        std::size_t end = value->find(L';', start);
        if (end == std::wstring::npos) {
            end = value->size();
        }
        std::wstring_view entry = std::wstring_view(*value).substr(start, end - start);
        while (!entry.empty() && (entry.front() == L' ' || entry.front() == L'\t')) {
            entry.remove_prefix(1);
        }
        while (!entry.empty() && (entry.back() == L' ' || entry.back() == L'\t')) {
            entry.remove_suffix(1);
        }
        if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        if (!entry.empty()) {
            std::filesystem::path path{std::wstring(entry)};
            // A relative entry would depend on the current directory: never searched.
            if (path.is_absolute()) {
                entries.push_back(std::move(path));
            }
        }
        start = end + 1;
    }
    return entries;
}

} // namespace rv::platform_win
