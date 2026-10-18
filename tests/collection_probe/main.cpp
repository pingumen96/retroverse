// rv_collection_probe: the GPU smoke tests' driver of the collection's texture arrays (I3 step 6, ADR-0011 §4 and §6).
// It runs a wall of test items (probe_layout.hpp) through gl::CollectionArrays with the host and renderer of rv_app, so
// that tests/smoke/test_collection_arrays.cpp can probe its screenshots and read its run report. Its own options come
// first and are taken out; the rest is retroverse.exe's command line (--hidden, --exit-after-frames, --frames,
// --screenshot, --run-report, --data-dir, --no-texture-compression, ...):
//   rv_collection_probe [--probe-scenario chunks|growth] [--probe-view lit|base-color] <retroverse.exe options>
// SDL_main.h provides the real entry point and converts the Windows command line to UTF-8.
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/app_host.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/core/main_thread.hpp>

#include "probe_app.hpp"

extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

constexpr rv::app::AppIdentity kIdentity{"Retroverse Collection Probe",
                                         "io.github.pingumen96.retroverse.collection-probe", "collection-probe.log"};

constexpr const char* kUsage =
    "rv_collection_probe [--probe-scenario chunks|growth] [--probe-view lit|base-color] <retroverse.exe options>\n"
    "  chunks: 2,049 items, two arrays per class; growth: 63 items, then 191 from frame 20 (arrays grown by a GPU\n"
    "  copy). The view: the lit shading (default) or the base color inspection view.\n\n";

/// Takes the probe's options out of `arguments`; false (with a message) for a bad one.
bool takeProbeOptions(std::vector<std::string>& arguments, rv::probe::ProbeOptions& options) {
    std::vector<std::string> rest;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument != "--probe-scenario" && argument != "--probe-view") {
            rest.push_back(argument);
            continue;
        }
        if (index + 1 >= arguments.size()) {
            std::fprintf(stderr, "%s needs a value\n\n%s", argument.c_str(), kUsage);
            return false;
        }
        const std::string_view value = arguments[++index];
        if (argument == "--probe-scenario") {
            const std::optional<rv::probe::Scenario> scenario = rv::probe::scenarioFromString(value);
            if (!scenario) {
                std::fprintf(stderr, "unknown scenario '%s'\n\n%s", std::string(value).c_str(), kUsage);
                return false;
            }
            options.scenario = *scenario;
        } else if (value == "lit" || value == "base-color") {
            options.view = value == "lit" ? rv::gl::SurfaceView::Lit : rv::gl::SurfaceView::BaseColor;
        } else {
            std::fprintf(stderr, "unknown view '%s'\n\n%s", std::string(value).c_str(), kUsage);
            return false;
        }
    }
    arguments = std::move(rest);
    return true;
}

int run(int argc, char** argv) {
    using rv::app::ExitCode;
    rv::core::markMainThread();
    std::vector<std::string> arguments(argv + (argc > 0 ? 1 : 0), argv + argc);
    rv::probe::ProbeOptions probeOptions;
    if (!takeProbeOptions(arguments, probeOptions)) {
        return rv::app::toInt(ExitCode::InitializationFailed);
    }
    const auto options = rv::app::parseCommandLine(arguments);
    if (!options) {
        std::fprintf(stderr, "%s\n\n%s", options.error().toString().c_str(), kUsage);
        return rv::app::toInt(ExitCode::InitializationFailed);
    }
    if (options->showHelp || options->showVersion) {
        std::printf("%s%s", kUsage, rv::app::commandLineHelp().c_str());
        return rv::app::toInt(ExitCode::Ok);
    }
    rv::probe::ProbeApp app(probeOptions);
    rv::app::AppHost host(*options, kIdentity);
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
