#include <rv/platform_win/drives.hpp>

#include <string>

#include <windows.h>

namespace rv::platform_win {

std::vector<std::filesystem::path> driveRoots() {
    std::vector<std::filesystem::path> roots;
    const DWORD letters = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if ((letters & (DWORD{1} << index)) != 0) {
            std::wstring root;
            root.push_back(static_cast<wchar_t>(L'A' + index));
            root += L":\\";
            roots.emplace_back(root);
        }
    }
    return roots;
}

} // namespace rv::platform_win
