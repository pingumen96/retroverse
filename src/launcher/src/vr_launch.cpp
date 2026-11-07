#include <rv/launcher/vr_launch.hpp>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::launcher {
namespace {

/// The largest append-file read back to compare (the shipped one is a few hundred bytes).
constexpr std::uintmax_t kMaxAppendFileBytes = 1024 * 1024;

/// An argument's file name: the text after its last '\' or '/' (a core's path, a ROM's).
[[nodiscard]] std::string_view fileNameOf(std::string_view argument) noexcept {
    const std::size_t separator = argument.find_last_of("\\/");
    return separator == std::string_view::npos ? argument : argument.substr(separator + 1);
}

[[nodiscard]] bool namesHeavyFile(const std::vector<std::string>& arguments, const std::vector<std::string>& patterns) {
    return std::any_of(arguments.begin(), arguments.end(), [&](const std::string& argument) {
        const std::string_view name = fileNameOf(argument);
        return !name.empty() && std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
            return core::matchesWildcard(name, pattern);
        });
    });
}

} // namespace

std::string appendFileBytes(const AppendConfig& config) {
    std::string bytes;
    for (const std::string& line : config.lines) {
        bytes += line;
        bytes += '\n';
    }
    return bytes;
}

AdaptedLaunch adaptLaunch(LaunchRequest request, const ProgramProfile* program, bool vr,
                          const std::filesystem::path& vrFolder) {
    AdaptedLaunch adapted;
    adapted.vr = vr;
    if (program == nullptr) {
        adapted.request = std::move(request);
        return adapted;
    }
    adapted.programId = program->id;
    adapted.programName = program->displayName;
    request.windowRules = program->window;
    if (!vr) {
        adapted.request = std::move(request);
        return adapted;
    }
    const VrSettings& settings = program->vr;
    LaunchRequest changed = request;

    // 1. The arguments dropped.
    std::vector<std::string> kept;
    for (std::string& argument : changed.arguments) {
        if (std::find(settings.dropArguments.begin(), settings.dropArguments.end(), argument) !=
            settings.dropArguments.end()) {
            adapted.dropped.push_back(std::move(argument));
        } else {
            kept.push_back(std::move(argument));
        }
    }

    // 2. The append-file: joined to the option's value the launch already gives, or in front.
    std::vector<std::string> front = settings.addArguments;
    if (settings.appendConfig) {
        const AppendConfig& config = *settings.appendConfig;
        std::filesystem::path path = (vrFolder / core::pathFromUtf8(config.fileName)).lexically_normal();
        path.make_preferred();
        const auto text = core::tryPathToUtf8(path);
        if (!text) {
            adapted.refused = fmt::format("the append-file's path is not UTF-8 text: {}", core::pathToUtf8Lossy(path));
            adapted.request = std::move(request);
            return adapted;
        }
        const auto given = std::find(kept.begin(), kept.end(), config.option);
        if (!config.joinWith.empty() && given != kept.end() && std::next(given) != kept.end()) {
            std::string& value = *std::next(given);
            value += config.joinWith;
            value += *text;
            adapted.added.push_back(config.joinWith + *text);
        } else {
            front.push_back(config.option);
            front.push_back(*text);
        }
        adapted.appendFile = AppendFile{std::move(path), appendFileBytes(config)};
    }

    // 3. The arguments added, first.
    adapted.added.insert(adapted.added.begin(), front.begin(), front.end());
    changed.arguments = std::move(front);
    changed.arguments.insert(changed.arguments.end(), std::make_move_iterator(kept.begin()),
                             std::make_move_iterator(kept.end()));

    // 4. The load.
    adapted.inGame = settings.inGame == InGameLoad::Heavy || namesHeavyFile(changed.arguments, settings.heavyWith)
                         ? InGameLoad::Heavy
                         : InGameLoad::Light;

    // 5. Still a request Windows can start (a command line not too long).
    if (auto valid = validateLaunchRequest(changed); !valid) {
        adapted.refused = valid.error().toString();
        adapted.added.clear();
        adapted.dropped.clear();
        adapted.appendFile.reset();
        adapted.request = std::move(request);
        return adapted;
    }
    adapted.request = std::move(changed);
    return adapted;
}

Status writeAppendFile(const AppendFile& file) {
    if (auto current = core::readTextFile(file.path, kMaxAppendFileBytes); current && *current == file.bytes) {
        return ok();
    }
    if (auto written = core::writeTextFileAtomic(file.path, file.bytes); !written) {
        Error error = written.error();
        error.message = fmt::format("the VR append-file could not be written: {}", error.message);
        if (error.context.empty()) {
            error.context = core::pathToUtf8Lossy(file.path);
        }
        return propagate(std::move(error));
    }
    return ok();
}

} // namespace rv::launcher
