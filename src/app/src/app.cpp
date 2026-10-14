#include <rv/app/app.hpp>

#include <cstdio>
#include <exception>

#include <rv/app/app_host.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/app/retroverse_app.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/version.hpp>

namespace rv::app {

int run(int argc, char** argv) {
    try {
        core::markMainThread();
        const auto options = parseCommandLine(argc, argv);
        if (!options) {
            std::fprintf(stderr, "%s\n\n%s", options.error().toString().c_str(), commandLineHelp().c_str());
            return toInt(ExitCode::InitializationFailed);
        }
        if (options->showHelp) {
            std::printf("%s", commandLineHelp().c_str());
            return toInt(ExitCode::Ok);
        }
        if (options->showVersion) {
            std::printf("%s\n", core::versionBanner().c_str());
            return toInt(ExitCode::Ok);
        }
        RetroverseApp app;
        AppHost host(*options);
        return host.run(app);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Unexpected failure: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Unexpected failure: unknown exception\n");
    }
    return toInt(ExitCode::RuntimeFailure);
}

} // namespace rv::app
