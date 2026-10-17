#include <rv/esde/disc_groups.hpp>

#include <algorithm>
#include <cstddef>
#include <map>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::esde {
namespace {

constexpr std::string_view kBlanks = " \t";

bool isBlank(char c) noexcept {
    return c == ' ' || c == '\t';
}

std::string_view trimBlanks(std::string_view text) noexcept {
    const std::size_t first = text.find_first_not_of(kBlanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

/// The lines of a text, without their CR LF or LF ends.
std::vector<std::string_view> linesOf(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

bool isDriveLetter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// A path for messages: the system folder joined with a relative path, '\' between the elements.
std::string fileText(const std::filesystem::path& systemFolder, std::string_view relativePath) {
    std::string text = core::pathToUtf8Lossy(systemFolder) + "\\" + std::string(relativePath);
    std::replace(text.end() - static_cast<std::ptrdiff_t>(relativePath.size()), text.end(), '/', '\\');
    return text;
}

} // namespace

std::optional<catalog::DiscGroupKind> discSheetKind(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view extension = name.substr(dot);
    if (core::equalsIgnoreCaseAscii(extension, ".cue")) {
        return catalog::DiscGroupKind::Cue;
    }
    if (core::equalsIgnoreCaseAscii(extension, ".m3u")) {
        return catalog::DiscGroupKind::M3u;
    }
    return std::nullopt;
}

SheetText decodeSheet(std::string_view bytes) {
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
    }
    SheetText result;
    std::size_t fallbackBytes = 0;
    result.text = core::utf8WithWindows1252Fallback(bytes, &fallbackBytes);
    result.windows1252 = fallbackBytes > 0;
    return result;
}

std::vector<std::string> cueSheetFiles(std::string_view text) {
    std::vector<std::string> files;
    for (const std::string_view rawLine : linesOf(text)) {
        const std::string_view line = trimBlanks(rawLine);
        if (line.size() < 4 || !core::equalsIgnoreCaseAscii(line.substr(0, 4), "FILE") ||
            (line.size() > 4 && !isBlank(line[4]))) {
            continue;
        }
        const std::string_view rest = trimBlanks(line.substr(4));
        std::string_view name;
        if (rest.starts_with('"')) {
            const std::size_t close = rest.find('"', 1);
            name = rest.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1);
        } else {
            // FILE <name> <type>: the type is the last word; a name alone (no type) is taken whole.
            const std::size_t lastBlank = rest.find_last_of(kBlanks);
            name = lastBlank == std::string_view::npos ? rest : trimBlanks(rest.substr(0, lastBlank));
        }
        if (!name.empty()) {
            files.emplace_back(name);
        }
    }
    return files;
}

std::vector<std::string> m3uFiles(std::string_view text) {
    std::vector<std::string> files;
    for (const std::string_view rawLine : linesOf(text)) {
        const std::string_view line = trimBlanks(rawLine);
        if (!line.empty() && !line.starts_with('#')) {
            files.emplace_back(line);
        }
    }
    return files;
}

Expected<std::string> resolveSheetReference(std::string_view reference, std::string_view sheetRelativePath,
                                            const std::filesystem::path& systemFolder) {
    const std::string context(reference);
    if (reference.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an empty reference", context);
    }
    for (const char c : reference) {
        if (static_cast<unsigned char>(c) < 0x20) {
            return makeError(ErrorCode::InvalidArgument, "a control character in the reference", context);
        }
    }
    const bool drive = reference.size() >= 2 && isDriveLetter(reference[0]) && reference[1] == ':';
    const bool rooted = reference.starts_with('/') || reference.starts_with('\\');
    if (drive || rooted) {
        // An absolute path (a drive's or a share's) counts when it lies inside the system folder.
        const bool absolute = (drive && reference.size() >= 3 && (reference[2] == '\\' || reference[2] == '/')) ||
                              reference.starts_with("\\\\") || reference.starts_with("//");
        if (!absolute) {
            return makeError(ErrorCode::InvalidArgument, "a drive-relative or rooted path", context);
        }
        return core::resolveRelativePath(reference, systemFolder);
    }
    if (reference.find(':') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "':' in a name", context);
    }
    // From the sheet's folder, element by element.
    std::vector<std::string_view> elements = core::split(sheetRelativePath, '/');
    elements.pop_back(); // the sheet's own name
    std::size_t start = 0;
    while (start <= reference.size()) {
        const std::size_t end = reference.find_first_of("/\\", start);
        const std::string_view element =
            reference.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (element == "..") {
            if (elements.empty()) {
                return makeError(ErrorCode::OutOfRange, "the reference leads out of the system folder", context);
            }
            elements.pop_back();
        } else if (!element.empty() && element != ".") {
            elements.push_back(element);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (elements.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the reference names the system folder itself", context);
    }
    std::string path;
    for (const std::string_view element : elements) {
        if (!path.empty()) {
            path += '/';
        }
        path += element;
    }
    return path;
}

Expected<DiscSheet> readDiscSheet(const std::filesystem::path& systemFolder, const catalog::LookupKey& sheet,
                                  catalog::DiscGroupKind kind) {
    const std::string context = fileText(systemFolder, sheet.relativePath());
    auto path = core::extendedLengthPathBelow(systemFolder, sheet.relativePath());
    if (!path) {
        return makeError(path.error().code, path.error().message, context);
    }
    auto bytes = core::readFileShared(*path, kMaxDiscSheetBytes);
    if (!bytes) {
        return makeError(bytes.error().code, bytes.error().message, context);
    }
    const SheetText text = decodeSheet(*bytes);
    DiscSheet result;
    result.kind = kind;
    result.windows1252 = text.windows1252;
    result.references = kind == catalog::DiscGroupKind::Cue ? cueSheetFiles(text.text) : m3uFiles(text.text);
    return result;
}

const catalog::LookupKey* DiscGrouping::headOf(const catalog::LookupKey& member) const noexcept {
    const auto found = std::lower_bound(memberHeads.begin(), memberHeads.end(), member,
                                        [](const auto& pair, const catalog::LookupKey& key) { return pair.first < key; });
    return found != memberHeads.end() && found->first == member ? &found->second : nullptr;
}

DiscGrouping groupDiscs(const catalog::SystemName& system, std::vector<DiscHeadLinks> links,
                        const std::filesystem::path& systemFolder) {
    std::sort(links.begin(), links.end(), [](const DiscHeadLinks& a, const DiscHeadLinks& b) { return a.head < b.head; });
    std::map<catalog::LookupKey, std::size_t> headIndex;
    for (std::size_t i = 0; i < links.size(); ++i) {
        headIndex.emplace(links[i].head, i);
    }
    DiscGrouping grouping;

    // Depth first over the heads in key order; a reference to a head on the current path closes a cycle and is
    // dropped. A head is finished after every head it reaches: the finishing order puts members first.
    enum class Mark : std::uint8_t { Unvisited, OnPath, Finished };
    std::vector<Mark> marks(links.size(), Mark::Unvisited);
    std::vector<std::vector<bool>> kept(links.size());
    for (std::size_t i = 0; i < links.size(); ++i) {
        kept[i].assign(links[i].members.size(), true);
    }
    std::vector<std::size_t> finished;
    struct Frame {
        std::size_t head;
        std::size_t next = 0;
    };
    for (std::size_t root = 0; root < links.size(); ++root) {
        if (marks[root] != Mark::Unvisited) {
            continue;
        }
        std::vector<Frame> path{Frame{root}};
        marks[root] = Mark::OnPath;
        while (!path.empty()) {
            Frame& frame = path.back();
            const DiscHeadLinks& link = links[frame.head];
            if (frame.next == link.members.size()) {
                marks[frame.head] = Mark::Finished;
                finished.push_back(frame.head);
                path.pop_back();
                continue;
            }
            const std::size_t m = frame.next++;
            const auto target = headIndex.find(link.members[m]);
            if (target == headIndex.end()) {
                continue; // a track, not a head
            }
            if (marks[target->second] == Mark::OnPath) {
                kept[frame.head][m] = false;
                grouping.issues.push_back(catalog::Issue{
                    catalog::IssueSeverity::Warning, std::string(kDiscIssueCycle), system.str(),
                    fileText(systemFolder, link.head.relativePath()),
                    fmt::format("references \"{}\", which leads back to it: that reference is dropped, so that the "
                                "cycle keeps a game",
                                link.members[m].relativePath())});
            } else if (marks[target->second] == Mark::Unvisited) {
                marks[target->second] = Mark::OnPath;
                path.push_back(Frame{target->second});
            }
        }
    }

    std::vector<std::size_t> groupOfHead(links.size(), links.size());
    for (std::size_t i = 0; i < links.size(); ++i) {
        catalog::DiscGroup group;
        group.system = system;
        group.head = links[i].head;
        group.kind = links[i].kind;
        for (std::size_t m = 0; m < links[i].members.size(); ++m) {
            if (kept[i][m]) {
                group.members.push_back(links[i].members[m]);
                grouping.memberHeads.emplace_back(links[i].members[m], links[i].head);
            }
        }
        if (!group.members.empty()) {
            groupOfHead[i] = grouping.groups.size();
            grouping.groups.push_back(std::move(group));
        }
    }
    // Of several heads of one member, the first in key order: pairs sorted by member, then head.
    std::sort(grouping.memberHeads.begin(), grouping.memberHeads.end());
    grouping.memberHeads.erase(std::unique(grouping.memberHeads.begin(), grouping.memberHeads.end(),
                                           [](const auto& a, const auto& b) { return a.first == b.first; }),
                               grouping.memberHeads.end());
    for (const std::size_t head : finished) {
        if (groupOfHead[head] != links.size()) {
            grouping.membersFirst.push_back(groupOfHead[head]);
        }
    }
    return grouping;
}

} // namespace rv::esde
