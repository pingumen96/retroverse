#include <rv/ui/catalog_source.hpp>

#include <algorithm>

namespace rv::ui {
namespace {

constexpr std::size_t kMaxEntryIdLength = 64;

} // namespace

bool isValidEntryId(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxEntryIdLength) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

} // namespace rv::ui
