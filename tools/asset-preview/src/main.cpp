// retroverse-asset-preview (style guide §19.3): previews a glTF model, a prefab manifest or a procedural prefab with
// the renderer and developer overlay of rv_app. SDL_main.h provides the real entry point (main/WinMain), converts the
// Windows command line to UTF-8 and calls the main() below, which it renames to SDL_main.
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>

#include <rv/app/app_host.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/assets/glb_writer.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/preview/crt_fixture.hpp>
#include <rv/preview/preview_options.hpp>

#include "preview_app.hpp"

// Hybrid-GPU laptops: ask the NVIDIA Optimus and AMD PowerXpress drivers to run the tool on the discrete GPU. The
// drivers only look for these symbols in the executable's export table.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

/// The tool's window title, SDL identity and log file (<user data>/logs/asset-preview.log).
constexpr rv::app::AppIdentity kIdentity{"Retroverse Asset Preview", "io.github.pingumen96.retroverse.asset-preview",
                                         "asset-preview.log"};

int run(int argc, char** argv) {
    using rv::app::ExitCode;
    rv::core::markMainThread();
    const auto options = rv::preview::parsePreviewCommandLine(argc, argv);
    if (!options) {
        std::fprintf(stderr, "%s\n\n%s", options.error().toString().c_str(), rv::preview::previewHelp().c_str());
        return rv::app::toInt(ExitCode::InitializationFailed);
    }
    if (options->run.showHelp) {
        std::printf("%s", rv::preview::previewHelp().c_str());
        return rv::app::toInt(ExitCode::Ok);
    }
    if (options->run.showVersion) {
        std::printf("%s\n", rv::core::versionBanner().c_str());
        return rv::app::toInt(ExitCode::Ok);
    }
    if (options->writeFixture) {
        // Written without a window: the tool makes its own sample.
        if (auto written = rv::assets::writeGlb(*options->writeFixture, rv::preview::makeCrtFixture()); !written) {
            std::fprintf(stderr, "--write-fixture failed: %s\n", written.error().toString().c_str());
            return rv::app::toInt(ExitCode::InitializationFailed);
        }
        std::printf("CRT-like fixture written to %s\n", rv::core::pathToUtf8(*options->writeFixture).c_str());
        return rv::app::toInt(ExitCode::Ok);
    }
    rv::preview::PreviewApp app(*options);
    rv::app::AppHost host(options->run, kIdentity);
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
