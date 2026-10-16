#include "support/file_system.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <aclapi.h>
#include <winioctl.h>

#include <rv/core/file_io.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/utf8.hpp>

namespace rv::test {
namespace {

/// The layout of a mount point's REPARSE_DATA_BUFFER (ntifs.h, not in the user-mode headers), before its path buffer.
struct MountPointHeader {
    DWORD reparseTag;
    WORD reparseDataLength; ///< bytes after `reserved`
    WORD reserved;
    WORD substituteNameOffset;
    WORD substituteNameLength;
    WORD printNameOffset;
    WORD printNameLength;
};
static_assert(sizeof(MountPointHeader) == 16);

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

tl::unexpected<Error> lastError(std::string_view what, const std::filesystem::path& path) {
    return makeError(ErrorCode::IoError, std::string(what) + ": " + core::lastSystemErrorMessage(),
                     core::pathToUtf8Lossy(path));
}

std::filesystem::path absoluteNormal(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

} // namespace

Expected<Junction> Junction::create(const std::filesystem::path& link, const std::filesystem::path& target) {
    const std::wstring linkPath = core::extendedLengthPath(link);
    if (!CreateDirectoryW(linkPath.c_str(), nullptr)) {
        return lastError("cannot create the junction's folder", link);
    }
    Junction junction(link); // from here on its destructor removes the folder
    const Handle handle(CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        return lastError("cannot open the junction's folder", link);
    }
    std::wstring printName = absoluteNormal(target).native();
    while (printName.size() > 3 && printName.ends_with(L'\\')) {
        printName.pop_back();
    }
    const std::wstring substituteName = L"\\??\\" + printName; // the NT form of the target
    const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBufferBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(MountPointHeader) + pathBufferBytes);
    MountPointHeader header{};
    header.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    header.reparseDataLength = static_cast<WORD>(8 + pathBufferBytes);
    header.substituteNameOffset = 0;
    header.substituteNameLength = static_cast<WORD>(substituteBytes);
    header.printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    header.printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::byte* names = buffer.data() + sizeof(header);
    std::memcpy(names, substituteName.data(), substituteBytes); // a NUL follows: the buffer is zeroed
    std::memcpy(names + header.printNameOffset, printName.data(), printBytes);
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                         nullptr, 0, &returned, nullptr)) {
        return lastError("cannot make the folder a junction", link);
    }
    return junction;
}

Junction::Junction(Junction&& other) noexcept : m_link(std::move(other.m_link)) {
    other.m_link.clear();
}

Junction::~Junction() {
    if (!m_link.empty()) {
        // The link only: RemoveDirectoryW never follows a reparse point.
        RemoveDirectoryW(core::extendedLengthPath(m_link).c_str());
    }
}

Expected<ListingDenied> ListingDenied::apply(const std::filesystem::path& folder) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return lastError("cannot open the process token", folder);
    }
    const Handle token(rawToken);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> userBuffer(size);
    if (!GetTokenInformation(token.get(), TokenUser, userBuffer.data(), size, &size)) {
        return lastError("cannot read the user of the process token", folder);
    }
    const PSID user = reinterpret_cast<const TOKEN_USER*>(userBuffer.data())->User.Sid;
    const DWORD aclSize = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(user);
    std::vector<std::byte> aclBuffer(aclSize);
    const auto acl = reinterpret_cast<PACL>(aclBuffer.data());
    if (!InitializeAcl(acl, aclSize, ACL_REVISION) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS & ~FILE_LIST_DIRECTORY, user)) {
        return lastError("cannot build the access control list", folder);
    }
    std::wstring path = folder.native();
    const DWORD status = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, acl, nullptr);
    if (status != ERROR_SUCCESS) {
        return makeError(ErrorCode::IoError, "cannot deny the listing: " + core::systemErrorMessage(status),
                         core::pathToUtf8Lossy(folder));
    }
    return ListingDenied(folder);
}

ListingDenied::ListingDenied(ListingDenied&& other) noexcept : m_folder(std::move(other.m_folder)) {
    other.m_folder.clear();
}

ListingDenied::~ListingDenied() {
    if (m_folder.empty()) {
        return;
    }
    // An empty, unprotected DACL: the folder inherits its parent's permissions again, as when it was created.
    ACL empty{};
    if (InitializeAcl(&empty, sizeof(ACL), ACL_REVISION)) {
        std::wstring path = m_folder.native();
        SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &empty, nullptr);
    }
}

Status setHidden(const std::filesystem::path& path, bool hidden) {
    const std::wstring target = core::extendedLengthPath(path);
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return lastError("cannot read the attributes", path);
    }
    constexpr DWORD kSettable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    DWORD updated = attributes & kSettable;
    updated = hidden ? (updated | FILE_ATTRIBUTE_HIDDEN) : (updated & ~static_cast<DWORD>(FILE_ATTRIBUTE_HIDDEN));
    if (!SetFileAttributesW(target.c_str(), updated == 0 ? FILE_ATTRIBUTE_NORMAL : updated)) {
        return lastError("cannot set the attributes", path);
    }
    return ok();
}

Status createFoldersAnyLength(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> folders;
    for (std::filesystem::path folder = absoluteNormal(path); folder.has_relative_path();
         folder = folder.parent_path()) {
        folders.push_back(folder);
    }
    for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
        if (!CreateDirectoryW(core::extendedLengthPath(*folder).c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return lastError("cannot create the folder", *folder);
        }
    }
    return ok();
}

Status setLastWriteTimeAnyLength(const std::filesystem::path& path, std::filesystem::file_time_type time) {
    const Handle file(CreateFileW(core::extendedLengthPath(path).c_str(), FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file.valid()) {
        return lastError("cannot open the file to set its time", path);
    }
    // MSVC's file_time_type counts FILETIME ticks (100 ns since 1601).
    const auto ticks = static_cast<std::uint64_t>(time.time_since_epoch().count());
    FILETIME written{static_cast<DWORD>(ticks & 0xFFFFFFFFu), static_cast<DWORD>(ticks >> 32)};
    if (!SetFileTime(file.get(), nullptr, nullptr, &written)) {
        return lastError("cannot set the last write time", path);
    }
    return ok();
}

Status writeFileAnyLength(const std::filesystem::path& path, std::string_view content) {
    const std::filesystem::path absolute = absoluteNormal(path);
    if (auto folders = createFoldersAnyLength(absolute.parent_path()); !folders) {
        return folders;
    }
    const Handle file(CreateFileW(core::extendedLengthPath(absolute).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return lastError("cannot create the file", path);
    }
    DWORD written = 0;
    if (!content.empty() &&
        (!WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
         written != content.size())) {
        return lastError("cannot write the file", path);
    }
    return ok();
}

void removeTreeAnyLength(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(core::extendedLengthPath(path)), ec);
}

} // namespace rv::test
