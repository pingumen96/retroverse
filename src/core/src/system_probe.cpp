#include <rv/core/system_probe.hpp>

#include <array>

#include <rv/core/case_fold.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

constexpr std::string_view kAppPathsKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
/// The uninstall keys of the native and the 32-bit view.
constexpr std::array<std::string_view, 2> kUninstallKeys{
    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
};

/// `text` without surrounding blanks and one pair of surrounding double quotes.
std::string_view unquoted(std::string_view text) {
    text = trim(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = trim(text.substr(1, text.size() - 2));
    }
    return text;
}

std::optional<std::filesystem::path> pathOf(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    auto path = tryPathFromUtf8(text);
    if (!path) {
        return std::nullopt;
    }
    return std::move(*path);
}

/// The program of a DisplayIcon value: `"C:\dir\a.exe",0`, `C:\dir\a.exe,-101` or `C:\dir\a.exe`.
std::optional<std::filesystem::path> iconProgram(std::string_view value) {
    std::string_view text = trim(value);
    if (text.starts_with('"')) {
        const std::size_t closing = text.find('"', 1);
        return pathOf(closing == std::string_view::npos ? text.substr(1) : text.substr(1, closing - 1));
    }
    if (const std::size_t comma = text.rfind(','); comma != std::string_view::npos) {
        std::string_view index = trim(text.substr(comma + 1));
        if (index.starts_with('-')) {
            index.remove_prefix(1);
        }
        const bool numeric =
            !index.empty() && index.find_first_not_of("0123456789") == std::string_view::npos;
        if (numeric) {
            text = trim(text.substr(0, comma));
        }
    }
    return pathOf(text);
}

/// The code points of `text` upper-cased with the file-system casing; nothing for malformed UTF-8.
std::optional<std::u32string> foldedCodePoints(std::string_view text) {
    const auto upper = fileSystemUpperCase(text);
    if (!upper) {
        return std::nullopt;
    }
    std::u32string codePoints;
    const std::string& utf8 = *upper;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        char32_t codePoint = length == 1 ? lead : lead & (length == 2 ? 0x1Fu : length == 3 ? 0x0Fu : 0x07u);
        for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        codePoints.push_back(codePoint);
        i += length;
    }
    return codePoints;
}

} // namespace

std::string_view toString(RegistryRoot root) noexcept {
    switch (root) {
    case RegistryRoot::CurrentUser:
        return "HKCU";
    case RegistryRoot::LocalMachine:
        return "HKLM";
    }
    return "?";
}

std::optional<std::filesystem::path> appPath(const ISystemProbe& probe, std::string_view executableName) {
    if (executableName.empty() || executableName.find_first_of("\\/") != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string key = std::string(kAppPathsKey) + std::string(executableName);
    for (const RegistryRoot root : {RegistryRoot::CurrentUser, RegistryRoot::LocalMachine}) {
        if (!probe.registryKeyExists(root, key)) {
            continue;
        }
        // The first hive holding the key decides, even without a value (ES-DE's rule).
        const auto value = probe.registryString(root, key, "");
        return value ? pathOf(unquoted(*value)) : std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> UninstallEntry::installFolder() const {
    if (installLocation) {
        return installLocation;
    }
    if (displayIcon && displayIcon->has_parent_path()) {
        return displayIcon->parent_path();
    }
    return std::nullopt;
}

std::optional<UninstallEntry> findUninstallEntry(const ISystemProbe& probe, std::string_view displayName) {
    if (displayName.empty()) {
        return std::nullopt;
    }
    for (const RegistryRoot root : {RegistryRoot::CurrentUser, RegistryRoot::LocalMachine}) {
        for (const std::string_view base : kUninstallKeys) {
            for (const std::string& subKey : probe.registrySubKeys(root, base)) {
                const std::string key = std::string(base) + "\\" + subKey;
                const auto name = probe.registryString(root, key, "DisplayName");
                if (!name) {
                    continue;
                }
                const std::string version = std::string(trim(probe.registryString(root, key, "DisplayVersion")
                                                                  .value_or(std::string())));
                const std::string_view shown = trim(*name);
                const bool matches = equalsIgnoreCaseAscii(shown, displayName) ||
                                     (!version.empty() &&
                                      equalsIgnoreCaseAscii(shown, std::string(displayName) + " " + version));
                if (!matches) {
                    continue;
                }
                UninstallEntry entry;
                entry.root = root;
                entry.keyPath = key;
                entry.displayName = std::string(shown);
                entry.displayVersion = version;
                if (const auto location = probe.registryString(root, key, "InstallLocation")) {
                    entry.installLocation = pathOf(unquoted(*location));
                }
                if (const auto icon = probe.registryString(root, key, "DisplayIcon")) {
                    entry.displayIcon = iconProgram(*icon);
                }
                return entry;
            }
        }
    }
    return std::nullopt;
}

Expected<ShellShortcut> ISystemProbe::readShortcut(const std::filesystem::path& path) const {
    return makeError(ErrorCode::Unsupported, "this machine's description reads no shortcuts", pathToUtf8Lossy(path));
}

bool matchesWildcard(std::string_view name, std::string_view pattern) {
    const auto text = foldedCodePoints(name);
    const auto glob = foldedCodePoints(pattern);
    if (!text || !glob) {
        return false;
    }
    // Greedy matching with one backtrack point: the last '*' seen and where its run ends in the text.
    std::size_t t = 0;
    std::size_t g = 0;
    std::size_t starAt = std::u32string::npos;
    std::size_t resumeAt = 0;
    while (t < text->size()) {
        if (g < glob->size() && ((*glob)[g] == U'?' || (*glob)[g] == (*text)[t])) {
            ++t;
            ++g;
        } else if (g < glob->size() && (*glob)[g] == U'*') {
            starAt = g++;
            resumeAt = t;
        } else if (starAt != std::u32string::npos) {
            g = starAt + 1;
            t = ++resumeAt;
        } else {
            return false;
        }
    }
    while (g < glob->size() && (*glob)[g] == U'*') {
        ++g;
    }
    return g == glob->size();
}

} // namespace rv::core
