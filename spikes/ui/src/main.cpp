// spike_ui (I1 spike d, docs/spikes/i1-ui.md, ADR-0005): the quick catalog mock screen in RmlUi over the test room,
// navigated with the gamepad alone, measured. SDL_main.h provides the real entry point (main/WinMain), converts the
// Windows command line to UTF-8 and calls the main() below, which it renames to SDL_main.
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>
#include <utility>

#include <rv/app/app_host.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/version.hpp>

#include "spike_app.hpp"
#include "spike_options.hpp"

// Hybrid-GPU laptops: ask the NVIDIA Optimus and AMD PowerXpress drivers to run the spike on the discrete GPU.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

/// The spike's window title, SDL identity and log file (<user data>/logs/spike-ui.log).
constexpr rv::app::AppIdentity kIdentity{"Retroverse UI Spike", "io.github.pingumen96.retroverse.spike-ui",
                                         "spike-ui.log"};

int run(int argc, char** argv) {
    using rv::app::ExitCode;
    rv::core::markMainThread();
    auto options = rv::spike_ui::parseSpikeOptions(argc, argv);
    if (!options) {
        std::fprintf(stderr, "%s\n\n%s", options.error().toString().c_str(), rv::spike_ui::spikeHelp().c_str());
        return rv::app::toInt(ExitCode::InitializationFailed);
    }
    if (options->run.showHelp) {
        std::printf("%s", rv::spike_ui::spikeHelp().c_str());
        return rv::app::toInt(ExitCode::Ok);
    }
    if (options->run.showVersion) {
        std::printf("%s\n", rv::core::versionBanner().c_str());
        return rv::app::toInt(ExitCode::Ok);
    }
    rv::app::AppHost host(options->run, kIdentity);
    rv::spike_ui::SpikeApp app(std::move(*options));
    return host.run(app);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Unexpected failure: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Unexpected failure: unknown exception\n");
    }
    return rv::app::toInt(rv::app::ExitCode::RuntimeFailure);
}
