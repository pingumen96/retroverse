#include <rv/app/app.hpp>

#include <cstdio>

#include <rv/core/arg_parser.hpp>
#include <rv/core/log.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/version.hpp>

namespace rv::app {

int run(int argc, char** argv) {
    core::markMainThread();

    core::ArgParser parser("retroverse", "Explorable 3D frontend for ES-DE game collections.");
    parser.flag("version", "Print the version and exit.");
    const auto args = parser.parse(argc, argv);
    if (!args) {
        std::fprintf(stderr, "%s\n\n%s", args.error().toString().c_str(), parser.helpText().c_str());
        return 2;
    }
    if (args->has("help")) {
        std::printf("%s", parser.helpText().c_str());
        return 0;
    }

    const std::string banner = core::versionBanner();
    std::printf("%s\n", banner.c_str());
    if (args->has("version")) {
        return 0;
    }

    core::LogConfig logConfig;
    core::initLogging(logConfig);
    spdlog::info("{} starting (renderer not implemented yet)", banner);
    core::shutdownLogging();
    return 0;
}

} // namespace rv::app
