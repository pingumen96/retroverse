#include <rv/core/version.hpp>

namespace rv::core {

std::string versionBanner() {
    std::string banner;
    banner += kProductName;
    banner += ' ';
    banner += kVersion;
    banner += " (";
    banner += kBuildType.empty() ? std::string_view("unknown build") : kBuildType;
#if defined(_MSC_VER)
    banner += ", MSVC ";
    banner += std::to_string(_MSC_VER);
#elif defined(__clang__)
    banner += ", clang ";
    banner += __clang_version__;
#endif
    banner += ')';
    return banner;
}

} // namespace rv::core
