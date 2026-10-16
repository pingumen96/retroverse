#include <rv/core/tree_walk.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <ratio>
#include <type_traits>
#include <utility>

#include <windows.h>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

static_assert(std::is_same_v<std::filesystem::file_time_type::period, std::ratio<1, 10'000'000>>,
              "std::filesystem::file_time_type counts FILETIME ticks (MSVC)");

std::filesystem::file_time_type toFileTime(const FILETIME& time) noexcept {
    const std::uint64_t ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return std::filesystem::file_time_type(
        std::filesystem::file_time_type::duration(static_cast<std::int64_t>(ticks)));
}

/// A folder's identity: the volume serial number and the file id, which a link reaching the folder again shares.
struct DirectoryId {
    std::uint64_t volume = 0;
    std::array<std::uint8_t, 16> file{};

    bool operator==(const DirectoryId&) const = default;
};

/// Closes a handle on scope exit.
class Handle {
public:
    explicit Handle(HANDLE handle) noexcept : m_handle(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (valid()) {
            CloseHandle(m_handle);
        }
    }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }

private:
    HANDLE m_handle;
};

/// `folder` (\\?\ form, no trailing separator) as CreateFileW opens a folder: "\\?\C:" names the volume, "\\?\C:\" its
/// root folder.
std::wstring openableFolder(const std::wstring& folder) {
    return folder.ends_with(L':') ? folder + L'\\' : folder;
}

/// Opens what `path` names, links followed, for its attributes only (nothing is read, nothing is changed).
HANDLE openForAttributes(const std::wstring& path) {
    return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

Expected<DirectoryId> directoryIdOf(const std::wstring& folder) {
    const Handle handle(openForAttributes(openableFolder(folder)));
    if (!handle.valid()) {
        const DWORD error = GetLastError();
        return makeError(fileErrorCode(error), "cannot open the folder: " + systemErrorMessage(error));
    }
    DirectoryId id;
    FILE_ID_INFO info{};
    if (GetFileInformationByHandleEx(handle.get(), FileIdInfo, &info, sizeof(info))) {
        id.volume = info.VolumeSerialNumber;
        std::memcpy(id.file.data(), info.FileId.Identifier, id.file.size());
        return id;
    }
    // File systems without 128-bit ids (FAT, exFAT): the 64-bit index.
    BY_HANDLE_FILE_INFORMATION basic{};
    if (!GetFileInformationByHandle(handle.get(), &basic)) {
        return makeError(ErrorCode::IoError, "cannot read the folder's id: " + lastSystemErrorMessage());
    }
    id.volume = basic.dwVolumeSerialNumber;
    const std::uint64_t index = (static_cast<std::uint64_t>(basic.nFileIndexHigh) << 32) | basic.nFileIndexLow;
    std::memcpy(id.file.data(), &index, sizeof(index));
    return id;
}

/// An entry of a folder as listed.
struct Child {
    std::string name; ///< UTF-8; unpaired surrogates as U+FFFD when nameConverted is false
    std::wstring wideName;
    bool nameConverted = true;
    DWORD attributes = 0;
    std::uint64_t size = 0;
    FILETIME lastWrite{};
    DWORD reparseTag = 0;

    [[nodiscard]] bool isDirectory() const noexcept { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    [[nodiscard]] bool isLink() const noexcept {
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(reparseTag);
    }
};

/// The entries of `folder` (\\?\ form, no trailing separator), "." and ".." left out, sorted by their UTF-8 names. A
/// folder that cannot be listed, whatever the reason (access denied, a drive gone, the folder removed meanwhile), is
/// Unavailable: its content is unknown, not gone (ADR-0008 §4).
Expected<std::vector<Child>> listFolder(const std::wstring& folder) {
    const std::wstring pattern = folder + L"\\*";
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                         FIND_FIRST_EX_LARGE_FETCH);
    std::vector<Child> children;
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return children; // an empty drive root has not even "." and ".."
        }
        return makeError(ErrorCode::Unavailable, "cannot list the folder: " + systemErrorMessage(error));
    }
    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        Child child;
        child.wideName = std::wstring(name);
        if (auto utf8 = wideToUtf8(name)) {
            child.name = std::move(*utf8);
        } else {
            child.name = wideToUtf8Lossy(name);
            child.nameConverted = false;
        }
        child.attributes = data.dwFileAttributes;
        child.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        child.lastWrite = data.ftLastWriteTime;
        child.reparseTag = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? data.dwReserved0 : 0;
        children.push_back(std::move(child));
    } while (FindNextFileW(find, &data));
    const DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) {
        return makeError(ErrorCode::Unavailable, "the folder's listing broke off: " + systemErrorMessage(error));
    }
    std::sort(children.begin(), children.end(), [](const Child& a, const Child& b) {
        // Ordinal UTF-8 (std::string compares bytes as unsigned char); names whose lossy forms clash by their UTF-16.
        return a.name != b.name ? a.name < b.name : a.wideName < b.wideName;
    });
    return children;
}

/// A folder being walked: where it is, its identity once known, its entries and the next one to visit.
struct Frame {
    std::string relativePath; ///< "" for the root
    std::wstring folder;      ///< \\?\ form through the links walked, no trailing separator
    std::optional<DirectoryId> id;
    std::vector<Child> children;
    std::size_t next = 0;
};

TreeEntry entryOf(const Child& child, std::string relativePath) {
    TreeEntry entry;
    entry.kind = child.isDirectory() ? TreeEntryKind::Directory : TreeEntryKind::File;
    entry.relativePath = std::move(relativePath);
    entry.size = child.isDirectory() ? 0 : child.size;
    entry.lastWriteTime = toFileTime(child.lastWrite);
    entry.hidden = (child.attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    entry.system = (child.attributes & FILE_ATTRIBUTE_SYSTEM) != 0;
    entry.reparseTag = child.reparseTag;
    return entry;
}

TreeEntry errorEntry(std::string relativePath, ErrorCode code, std::string message) {
    TreeEntry entry;
    entry.kind = TreeEntryKind::Error;
    entry.error = Error{code, std::move(message), relativePath};
    entry.relativePath = std::move(relativePath);
    return entry;
}

/// The root's error when its attributes cannot be read: NotFound when it does not exist, Unavailable otherwise (a
/// missing drive or share is unavailable, not missing, and so is a folder that cannot be reached).
Error rootError(const std::filesystem::path& root, DWORD error, std::string context) {
    const ErrorCode code = fileErrorCode(error, root);
    if (code == ErrorCode::NotFound) {
        return Error{code, "the folder does not exist: " + systemErrorMessage(error), std::move(context)};
    }
    if (code == ErrorCode::Unavailable && fileErrorCode(error) == ErrorCode::NotFound) {
        return Error{code, "its drive or share is not available", std::move(context)};
    }
    return Error{ErrorCode::Unavailable, "the folder cannot be opened: " + systemErrorMessage(error),
                 std::move(context)};
}

} // namespace

Expected<TreeWalkSummary> walkTree(const std::filesystem::path& root, const TreeVisitor& visit,
                                   const TreeWalkOptions& options) {
    const std::string context = pathToUtf8Lossy(root);
    std::wstring rootFolder = extendedLengthPath(root);
    while (rootFolder.size() > 4 && rootFolder.ends_with(L'\\')) {
        rootFolder.pop_back();
    }
    const DWORD attributes = GetFileAttributesW(openableFolder(rootFolder).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return tl::unexpected<Error>(rootError(root, GetLastError(), context));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return makeError(ErrorCode::InvalidArgument, "not a folder", context);
    }
    auto rootChildren = listFolder(rootFolder);
    if (!rootChildren) {
        Error error = std::move(rootChildren.error());
        error.context = context;
        return tl::unexpected<Error>(std::move(error));
    }

    TreeWalkSummary summary;
    std::vector<Frame> stack;
    stack.push_back(Frame{std::string(), rootFolder, std::nullopt, std::move(*rootChildren), 0});
    const auto emit = [&](const TreeEntry& entry) {
        switch (entry.kind) {
        case TreeEntryKind::File:
            ++summary.files;
            break;
        case TreeEntryKind::Directory:
            ++summary.directories;
            break;
        case TreeEntryKind::Error:
            ++summary.errors;
            break;
        }
        const WalkAction action = visit(entry);
        if (action == WalkAction::Stop) {
            summary.stopped = true;
        }
        return action;
    };

    while (!stack.empty()) {
        if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) {
            summary.cancelled = true;
            break;
        }
        Frame& top = stack.back();
        if (top.next == top.children.size()) {
            stack.pop_back();
            continue;
        }
        const Child& child = top.children[top.next++];
        std::string relative = top.relativePath.empty() ? child.name : top.relativePath + "/" + child.name;
        const std::wstring childPath = top.folder + L"\\" + child.wideName;

        if (!child.nameConverted) {
            if (emit(errorEntry(std::move(relative), ErrorCode::InvalidArgument,
                                "the name holds an unpaired UTF-16 surrogate and has no UTF-8 form")) ==
                WalkAction::Stop) {
                break;
            }
            continue;
        }

        if (!child.isDirectory()) {
            TreeEntry entry = entryOf(child, std::move(relative));
            if (child.isLink() && options.followLinks) {
                // A symbolic link to a file: the size and time of the file it leads to.
                const Handle target(openForAttributes(childPath));
                BY_HANDLE_FILE_INFORMATION info{};
                if (!target.valid() || !GetFileInformationByHandle(target.get(), &info)) {
                    const DWORD error = GetLastError();
                    if (emit(errorEntry(std::move(entry.relativePath), fileErrorCode(error),
                                        "the link's target cannot be opened: " + systemErrorMessage(error))) ==
                        WalkAction::Stop) {
                        break;
                    }
                    continue;
                }
                entry.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                entry.lastWriteTime = toFileTime(info.ftLastWriteTime);
            }
            if (emit(entry) == WalkAction::Stop) {
                break;
            }
            continue;
        }

        const WalkAction action = emit(entryOf(child, relative));
        if (action == WalkAction::Stop) {
            break;
        }
        if (action == WalkAction::SkipChildren || (child.isLink() && !options.followLinks)) {
            continue;
        }
        std::optional<DirectoryId> id;
        if (child.isLink()) {
            // A folder reached through a link may be one the walk is inside already: compare identities with every
            // folder of the path walked down (theirs computed the first time a link needs them).
            auto linked = directoryIdOf(childPath);
            if (!linked) {
                if (emit(errorEntry(relative, linked.error().code,
                                    "the link's target cannot be opened: " + linked.error().message)) ==
                    WalkAction::Stop) {
                    break;
                }
                continue;
            }
            std::optional<std::string> loopsTo;
            for (Frame& frame : stack) {
                if (!frame.id) {
                    if (auto frameId = directoryIdOf(frame.folder)) {
                        frame.id = *frameId;
                    } else {
                        continue;
                    }
                }
                if (*frame.id == *linked) {
                    loopsTo = frame.relativePath;
                    break;
                }
            }
            if (loopsTo) {
                const std::string target = loopsTo->empty() ? std::string("the root") : "\"" + *loopsTo + "\"";
                if (emit(errorEntry(relative, ErrorCode::Unsupported,
                                    "not followed: the link leads back to " + target + " (a loop)")) ==
                    WalkAction::Stop) {
                    break;
                }
                continue;
            }
            id = *linked;
        }
        auto children = listFolder(childPath);
        if (!children) {
            if (emit(errorEntry(relative, children.error().code, children.error().message)) == WalkAction::Stop) {
                break;
            }
            continue;
        }
        stack.push_back(Frame{std::move(relative), childPath, id, std::move(*children), 0});
    }
    return summary;
}

Expected<std::vector<TreeEntry>> listTree(const std::filesystem::path& root, const TreeWalkOptions& options) {
    std::vector<TreeEntry> entries;
    auto walked = walkTree(
        root,
        [&](const TreeEntry& entry) {
            entries.push_back(entry);
            return WalkAction::Continue;
        },
        options);
    if (!walked) {
        return propagate(std::move(walked.error()));
    }
    return entries;
}

} // namespace rv::core
