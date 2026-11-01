#include <rv/library/folder_listing.hpp>

#include <algorithm>
#include <cwchar>
#include <system_error>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/platform_win/drives.hpp>

namespace rv::library {
namespace {

[[nodiscard]] bool bookkeeping(const std::wstring& name) {
    return _wcsicmp(name.c_str(), L"$RECYCLE.BIN") == 0 || _wcsicmp(name.c_str(), L"System Volume Information") == 0;
}

[[nodiscard]] Error listingError(const std::filesystem::path& folder, const esde::FolderCheck& check,
                                 const std::string& detail) {
    const std::string name = core::pathToUtf8Lossy(folder);
    if (check.availability == catalog::Availability::Available) {
        return Error{ErrorCode::IoError, "the folder cannot be listed: " + detail, name};
    }
    if (check.reason == catalog::SourceUnavailableReason::FolderMissing) {
        return Error{ErrorCode::NotFound, "the folder does not exist", name};
    }
    if (check.reason == catalog::SourceUnavailableReason::Unreachable) {
        return Error{ErrorCode::Unavailable, "the drive cannot be reached: " + check.detail, name};
    }
    return Error{ErrorCode::IoError, "the folder cannot be listed: " + check.detail, name};
}

} // namespace

std::vector<ListedFolder> listDrives() {
    std::vector<ListedFolder> drives;
    for (const std::filesystem::path& root : platform_win::driveRoots()) {
        drives.push_back(ListedFolder{root, core::pathToUtf8Lossy(root)});
    }
    return drives;
}

Expected<std::vector<ListedFolder>> listFolders(const std::filesystem::path& folder) {
    const esde::FolderCheck check = esde::checkFolder(folder);
    if (check.availability != catalog::Availability::Available) {
        return propagate(listingError(folder, check, {}));
    }
    std::vector<ListedFolder> folders;
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, std::filesystem::directory_options::skip_permission_denied,
                                                error);
    if (error) {
        return propagate(listingError(folder, check, error.message()));
    }
    const std::filesystem::directory_iterator end;
    while (entries != end && folders.size() < kMaxListedFolders) {
        std::error_code entryError;
        if (entries->is_directory(entryError) && !bookkeeping(entries->path().filename().native())) {
            folders.push_back(ListedFolder{entries->path(), core::pathToUtf8Lossy(entries->path().filename())});
        }
        entries.increment(error);
        if (error) {
            return propagate(listingError(folder, check, error.message()));
        }
    }
    std::sort(folders.begin(), folders.end(), [](const ListedFolder& left, const ListedFolder& right) {
        const std::string a = core::toLowerAscii(left.name);
        const std::string b = core::toLowerAscii(right.name);
        return a != b ? a < b : left.name < right.name;
    });
    return folders;
}

esde::FolderCheck checkAccess(const std::filesystem::path& folder) {
    esde::FolderCheck check = esde::checkFolder(folder);
    if (check.availability != catalog::Availability::Available) {
        return check;
    }
    std::error_code error;
    const std::filesystem::directory_iterator entries(folder, error);
    if (error) {
        check.availability = catalog::Availability::SourceUnavailable;
        check.reason = catalog::SourceUnavailableReason::NotListable;
        check.detail = error.message();
    }
    return check;
}

} // namespace rv::library
