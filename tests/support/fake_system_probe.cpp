#include "support/fake_system_probe.hpp"

#include <algorithm>

#include <rv/core/case_fold.hpp>
#include <rv/core/utf8.hpp>

namespace rv::test {
namespace {

std::string folded(std::string_view text) {
    auto upper = core::fileSystemUpperCase(text);
    return upper ? std::move(*upper) : std::string(text);
}

/// A folder's path as the fake compares them: normalized, '\' separators, no trailing one, upper-cased.
std::wstring foldedFolder(const std::filesystem::path& folder) {
    std::wstring text = folder.lexically_normal().native();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    while (text.size() > 3 && text.ends_with(L'\\')) {
        text.pop_back();
    }
    auto upper = core::fileSystemUpperCase(std::wstring_view(text));
    return upper ? std::move(*upper) : text;
}

std::vector<std::string_view> keyElements(std::string_view keyPath) {
    std::vector<std::string_view> elements;
    std::size_t start = 0;
    while (start <= keyPath.size()) {
        std::size_t end = keyPath.find('\\', start);
        if (end == std::string_view::npos) {
            end = keyPath.size();
        }
        if (end > start) {
            elements.push_back(keyPath.substr(start, end - start));
        }
        start = end + 1;
    }
    return elements;
}

std::string foldedKey(std::string_view keyPath) {
    std::string key;
    for (const std::string_view element : keyElements(keyPath)) {
        if (!key.empty()) {
            key += '\\';
        }
        key += folded(element);
    }
    return key;
}

} // namespace

FakeSystemProbe& FakeSystemProbe::addRegistryKey(core::RegistryRoot root, std::string_view keyPath) {
    std::string parent;
    m_keys[{root, parent}]; // the hive itself
    for (const std::string_view element : keyElements(keyPath)) {
        const std::string name = folded(element);
        m_keys[{root, parent}].subKeys.emplace(name, std::string(element)); // the first spelling stays
        parent += parent.empty() ? name : "\\" + name;
        m_keys[{root, parent}];
    }
    return *this;
}

FakeSystemProbe& FakeSystemProbe::setRegistryString(core::RegistryRoot root, std::string_view keyPath,
                                                    std::string_view valueName, std::string value) {
    addRegistryKey(root, keyPath);
    m_keys[{root, foldedKey(keyPath)}].values[folded(valueName)] = std::move(value);
    return *this;
}

FakeSystemProbe& FakeSystemProbe::addFile(const std::filesystem::path& path) {
    std::vector<std::filesystem::path>& files = m_files[foldedFolder(path.parent_path())];
    if (!fileExists(path)) {
        files.push_back(path);
    }
    return *this;
}

FakeSystemProbe& FakeSystemProbe::setHomeFolder(std::optional<std::filesystem::path> folder) {
    m_home = std::move(folder);
    return *this;
}

FakeSystemProbe& FakeSystemProbe::setSystem32Folder(std::filesystem::path folder) {
    m_system32 = std::move(folder);
    return *this;
}

FakeSystemProbe& FakeSystemProbe::setWindowsFolder(std::filesystem::path folder) {
    m_windows = std::move(folder);
    return *this;
}

FakeSystemProbe& FakeSystemProbe::setPathEntries(std::vector<std::filesystem::path> entries) {
    m_pathEntries = std::move(entries);
    return *this;
}

const FakeSystemProbe::Key* FakeSystemProbe::findKey(core::RegistryRoot root, std::string_view keyPath) const {
    const auto found = m_keys.find({root, foldedKey(keyPath)});
    return found == m_keys.end() ? nullptr : &found->second;
}

std::optional<std::string> FakeSystemProbe::registryString(core::RegistryRoot root, std::string_view keyPath,
                                                           std::string_view valueName) const {
    const Key* key = findKey(root, keyPath);
    if (key == nullptr) {
        return std::nullopt;
    }
    const auto value = key->values.find(folded(valueName));
    if (value == key->values.end()) {
        return std::nullopt;
    }
    return value->second;
}

bool FakeSystemProbe::registryKeyExists(core::RegistryRoot root, std::string_view keyPath) const {
    return !keyElements(keyPath).empty() && findKey(root, keyPath) != nullptr;
}

std::vector<std::string> FakeSystemProbe::registrySubKeys(core::RegistryRoot root, std::string_view keyPath) const {
    std::vector<std::string> names;
    if (const Key* key = findKey(root, keyPath)) {
        for (const auto& [foldedName, spelling] : key->subKeys) {
            names.push_back(spelling);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool FakeSystemProbe::fileExists(const std::filesystem::path& path) const {
    const auto folder = m_files.find(foldedFolder(path.parent_path()));
    if (folder == m_files.end()) {
        return false;
    }
    const std::wstring name = foldedFolder(path.filename());
    return std::any_of(folder->second.begin(), folder->second.end(), [&](const std::filesystem::path& file) {
        return foldedFolder(file.filename()) == name;
    });
}

std::vector<std::filesystem::path> FakeSystemProbe::filesMatching(const std::filesystem::path& folder,
                                                                  std::string_view pattern) const {
    std::vector<std::pair<std::string, std::filesystem::path>> matches;
    if (const auto files = m_files.find(foldedFolder(folder)); files != m_files.end()) {
        for (const std::filesystem::path& file : files->second) {
            const std::string name = core::pathToUtf8Lossy(file.filename());
            if (core::matchesWildcard(name, pattern)) {
                matches.emplace_back(name, folder / file.filename());
            }
        }
    }
    std::sort(matches.begin(), matches.end());
    std::vector<std::filesystem::path> paths;
    for (auto& [name, path] : matches) {
        paths.push_back(std::move(path));
    }
    return paths;
}

std::optional<std::filesystem::path> FakeSystemProbe::homeFolder() const {
    return m_home;
}

std::filesystem::path FakeSystemProbe::system32Folder() const {
    return m_system32;
}

std::filesystem::path FakeSystemProbe::windowsFolder() const {
    return m_windows;
}

std::vector<std::filesystem::path> FakeSystemProbe::pathEntries() const {
    return m_pathEntries;
}

} // namespace rv::test
