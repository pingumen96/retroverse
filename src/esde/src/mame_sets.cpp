#include <rv/esde/mame_sets.hpp>

#include <algorithm>
#include <new>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>

#include "xml.hpp"

namespace rv::esde {
namespace {

catalog::Issue issueOf(std::string_view code, const std::filesystem::path& file, std::string reason) {
    return catalog::Issue{
        catalog::IssueSeverity::Warning, std::string(code), {}, core::pathToUtf8Lossy(file), std::move(reason)};
}

bool sortedContains(const std::vector<std::string>& sorted, std::string_view value) noexcept {
    const auto found = std::lower_bound(sorted.begin(), sorted.end(), value,
                                        [](const std::string& a, std::string_view b) { return a < b; });
    return found != sorted.end() && *found == value;
}

/// One list read with rv_esde's XML loader (shared reading, ES-DE's decoding); what was read leniently is logged at
/// debug level only, the path being the user's.
Expected<detail::XmlDocument> readList(const std::filesystem::path& file) {
    const std::string source = core::pathToUtf8Lossy(file);
    auto bytes = core::readFileShared(file, kMaxMameListBytes);
    if (!bytes) {
        return propagate(std::move(bytes.error()));
    }
    auto document = detail::parseXml(std::move(*bytes), source);
    if (!document) {
        return propagate(std::move(document.error()));
    }
    for (const std::string& warning : document->warnings()) {
        spdlog::debug("{}: {}", source, warning);
    }
    return document;
}

/// The text of the top-level elements named `name`, as ES-DE reads them (text().get(): the first text child, as
/// written).
std::vector<std::string> topLevelTexts(const detail::XmlDocument& document, const char* name) {
    std::vector<std::string> texts;
    for (pugi::xml_node node = document.document().child(name); node; node = node.next_sibling(name)) {
        texts.emplace_back(node.text().get());
    }
    return texts;
}

} // namespace

MameSets::MameSets(std::vector<std::pair<std::string, std::string>> names, std::vector<std::string> bioses,
                   std::vector<std::string> devices)
    : m_names(std::move(names)), m_bioses(std::move(bioses)), m_devices(std::move(devices)) {
    // The last pair of a mamename counts, as ES-DE's map assignment keeps it: a stable sort keeps the file order among
    // equal names, then each run of them keeps its last.
    std::stable_sort(m_names.begin(), m_names.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<std::string, std::string>> unique;
    unique.reserve(m_names.size());
    for (auto& pair : m_names) {
        if (!unique.empty() && unique.back().first == pair.first) {
            unique.back().second = std::move(pair.second);
        } else {
            unique.push_back(std::move(pair));
        }
    }
    m_names = std::move(unique);
    std::sort(m_bioses.begin(), m_bioses.end());
    std::sort(m_devices.begin(), m_devices.end());
}

bool MameSets::isBios(std::string_view stem) const noexcept {
    return sortedContains(m_bioses, stem);
}

bool MameSets::isDevice(std::string_view stem) const noexcept {
    return sortedContains(m_devices, stem);
}

std::string MameSets::realName(std::string_view mameName) const {
    const auto found = std::lower_bound(m_names.begin(), m_names.end(), mameName,
                                        [](const auto& pair, std::string_view name) { return pair.first < name; });
    if (found != m_names.end() && found->first == mameName && !found->second.empty()) {
        return found->second;
    }
    return std::string(mameName);
}

MameLists readMameListFiles(const std::optional<std::filesystem::path>& namesFile,
                            const std::optional<std::filesystem::path>& biosesFile,
                            const std::optional<std::filesystem::path>& devicesFile) {
    MameLists lists;
    std::vector<std::pair<std::string, std::string>> names;
    std::vector<std::string> bioses;
    std::vector<std::string> devices;
    const auto read = [&](const std::optional<std::filesystem::path>& file, std::string_view resource,
                          const auto& take) {
        if (!file) {
            lists.issues.push_back(catalog::Issue{
                catalog::IssueSeverity::Warning,
                std::string(kMameIssueListMissing),
                {},
                {},
                fmt::format("ES-DE's resources/{} is not there: ES-DE reads no MAME list after it", resource)});
            return false;
        }
        try {
            auto document = readList(*file);
            if (!document) {
                lists.issues.push_back(issueOf(
                    kMameIssueListUnreadable, *file,
                    fmt::format("cannot be read ({}): ES-DE reads no MAME list after it", document.error().message)));
                return false;
            }
            take(*document);
        } catch (const std::bad_alloc&) {
            lists.issues.push_back(
                issueOf(kMameIssueListUnreadable, *file, "out of memory while reading it: no MAME list read after it"));
            return false;
        }
        lists.files.push_back(*file);
        return true;
    };
    // ES-DE's order: the names, then the BIOS sets, then the devices; the first list that fails ends the reading.
    const bool namesRead = read(namesFile, kMameNamesResource, [&](const detail::XmlDocument& document) {
        for (pugi::xml_node game = document.document().child("game"); game; game = game.next_sibling("game")) {
            names.emplace_back(game.child("mamename").text().get(), game.child("realname").text().get());
        }
    });
    const bool biosesRead = namesRead && read(biosesFile, kMameBiosesResource, [&](const detail::XmlDocument& doc) {
                                bioses = topLevelTexts(doc, "bios");
                            });
    if (biosesRead) {
        read(devicesFile, kMameDevicesResource,
             [&](const detail::XmlDocument& document) { devices = topLevelTexts(document, "device"); });
    }
    lists.sets = MameSets(std::move(names), std::move(bioses), std::move(devices));
    return lists;
}

MameLists loadMameLists(const catalog::LibraryProfile& profile) {
    MameLists lists =
        readMameListFiles(bundledResource(profile, kMameNamesResource), bundledResource(profile, kMameBiosesResource),
                          bundledResource(profile, kMameDevicesResource));
    spdlog::debug("MAME lists: {} name(s), {} BIOS set(s), {} device(s), {} issue(s)", lists.sets.nameCount(),
                  lists.sets.biosCount(), lists.sets.deviceCount(), lists.issues.size());
    return lists;
}

bool isArcadePlatform(std::span<const std::string> platforms) noexcept {
    bool arcade = false;
    for (const std::string& platform : platforms) {
        if (platform == "ignore") {
            return false;
        }
        arcade = arcade || platform == "arcade" || platform == "neogeo";
    }
    return arcade;
}

std::string esdeRemoveParenthesis(std::string_view text) {
    static constexpr std::string_view kBrackets = "()[]";
    std::string result(text);
    bool done = false;
    while (!done) {
        done = true;
        for (std::size_t i = 0; i < kBrackets.size(); i += 2) {
            const std::size_t end = result.find_first_of(kBrackets[i + 1]);
            const std::size_t start = result.find_last_of(kBrackets[i], end);
            if (start != std::string::npos && end != std::string::npos) {
                result.erase(start, end - start + 1);
                done = false;
            }
        }
    }
    // Utils::String::trim: what C's isspace calls blank, in the default locale.
    static constexpr std::string_view kBlanks = " \t\n\v\f\r";
    const std::size_t first = result.find_first_not_of(kBlanks);
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = result.find_last_not_of(kBlanks);
    return result.substr(first, last - first + 1);
}

std::string mameName(const MameSets& sets, std::string_view stem, bool stripExtraInfo) {
    const std::string realName = sets.realName(esdeRemoveParenthesis(stem));
    return stripExtraInfo ? esdeRemoveParenthesis(realName) : realName;
}

} // namespace rv::esde
