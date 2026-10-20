#include <rv/world/platform_manifest.hpp>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;

constexpr std::string_view kColourForm = "an sRGB colour #RRGGBB in uppercase hexadecimal";

bool isSignText(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxSignTextLength || text.front() == ' ' || text.back() == ' ') {
        return false;
    }
    for (const char c : text) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

} // namespace

bool isSrgbHexColor(std::string_view text) noexcept {
    if (text.size() != 7 || text.front() != '#') {
        return false;
    }
    for (const char c : text.substr(1)) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

Expected<PlatformManifest> parsePlatformManifest(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "",
                              {"console", "mediaFamily", "plastic", "schemaVersion", "sign", "system", "themePack"});
    PlatformManifest manifest;
    (void)reader.integer("schemaVersion", 1, 1);
    manifest.system = reader.parsed<catalog::SystemName>("system", catalog::SystemName::parse, "an ES-DE system name");
    manifest.mediaFamily = reader.dataId("mediaFamily");
    manifest.themePack = reader.dataId("themePack");
    const ObjectReader plastic = reader.object("plastic", {"accent", "cartridge", "console"});
    manifest.cartridgePlastic = plastic.checkedString("cartridge", isSrgbHexColor, kColourForm);
    manifest.consolePlastic = plastic.checkedString("console", isSrgbHexColor, kColourForm);
    manifest.accentPlastic = plastic.checkedString("accent", isSrgbHexColor, kColourForm);
    if (!document.failed() && !reader.value("console").is_null()) {
        manifest.console = reader.prefab("console");
    }
    const ObjectReader sign = reader.object("sign", {"text"});
    manifest.signText =
        sign.checkedString("text", isSignText, "1 to 32 printable ASCII characters without a space at either end");
    if (document.failed()) {
        return propagate(document.error());
    }
    return manifest;
}

Expected<PlatformManifest> loadPlatformManifest(const std::filesystem::path& assetRoot,
                                                const catalog::SystemName& system) {
    if (system.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a platform manifest needs a system");
    }
    const std::filesystem::path path = assetRoot / "world" / "platforms" / core::pathFromUtf8(system.str() + ".json");
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    const std::string context = core::pathToUtf8Lossy(path);
    auto manifest = parsePlatformManifest(*text, context);
    if (manifest && manifest->system != system) {
        return makeError(ErrorCode::ValidationFailed,
                         "/system: the manifest is of \"" + manifest->system.str() + "\", its file of \"" +
                             system.str() + "\"",
                         context);
    }
    return manifest;
}

} // namespace rv::world
