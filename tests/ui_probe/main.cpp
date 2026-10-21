// rv_ui_probe: the GPU smoke tests' driver of the product UI's screens from fixed models (I4 step 7a): the directory
// (rv::ui::DirectoryScreen) over the test room, from directory_fixture.hpp's model, driven by an input script's UI
// actions as retroverse.exe reads them, its checkpoints recorded and screenshotted, so that
// tests/smoke/test_directory_panel.cpp can read its report and look at its screenshots. Its own options are taken out;
// the rest is retroverse.exe's command line (--hidden, --exit-after-frames, --frames, --fixed-dt, --input-script,
// --width, --height, --data-dir, --run-report, ...):
//   rv_ui_probe [--probe-shots <dir>] [--probe-report <json>] <retroverse.exe options>
// SDL_main.h provides the real entry point and converts the Windows command line to UTF-8.
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <rv/app/app_host.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/text.hpp>

#include "probe_app.hpp"

extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

constexpr rv::app::AppIdentity kIdentity{"Retroverse UI Probe", "io.github.pingumen96.retroverse.ui-probe",
                                         "ui-probe.log"};

constexpr const char* kUsage =
    "rv_ui_probe [--probe-shots <dir>] [--probe-report <json>] <retroverse.exe options>\n"
    "  The directory over the test room from a fixed model, opened on its floors at frame 0 and driven by\n"
    "  --input-script (tests/input_scripts/directory_panel.json); --probe-shots writes its checkpoints' screenshots\n"
    "  into a folder, --probe-report its checkpoints, commands and RmlUi's log counts as JSON.\n\n";

/// Takes the probe's options out of `arguments`; false (with a message) for a bad one.
bool takeProbeOptions(std::vector<std::string>& arguments, rv::probe::UiProbeOptions& options) {
    std::vector<std::string> rest;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument != "--probe-shots" && argument != "--probe-report") {
            rest.push_back(argument);
            continue;
        }
        if (index + 1 >= arguments.size()) {
            std::fprintf(stderr, "%s needs a value\n\n%s", argument.c_str(), kUsage);
            return false;
        }
        const std::filesystem::path value = rv::core::pathFromUtf8(arguments[++index]);
        (argument == "--probe-shots" ? options.shots : options.report) = value;
    }
    arguments = std::move(rest);
    return true;
}

int run(int argc, char** argv) {
    using rv::app::ExitCode;
    rv::core::markMainThread();
    std::vector<std::string> arguments(argv + (argc > 0 ? 1 : 0), argv + argc);
    rv::probe::UiProbeOptions probeOptions;
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
    rv::probe::UiProbeApp app(probeOptions);
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
