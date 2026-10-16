// spike_video: the measurement executable of I1 spike (c) (docs/spikes/i1-video.md). SDL_main.h provides the real entry
// point (main/WinMain), converts the Windows command line to UTF-8 and calls the main() below, which it renames to
// SDL_main.
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <rv/app/app_host.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/text.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_decoder.hpp>

#include "spike_fixtures.hpp"
#include "spike_options.hpp"
#include "video_spike_app.hpp"

// Hybrid-GPU laptops: ask the NVIDIA Optimus and AMD PowerXpress drivers to run the spike on the discrete GPU.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

constexpr rv::app::AppIdentity kIdentity{"Retroverse video spike", "io.github.pingumen96.retroverse.spike-video",
                                         "spike-video.log"};

int run(int argc, char** argv) {
    using rv::app::ExitCode;
    rv::core::markMainThread();
    const std::vector<std::string> arguments(argv + std::min(argc, 1), argv + argc);
    auto options = rv::spike::parseSpikeOptions(arguments);
    if (!options) {
        std::fprintf(stderr, "%s\n\n%s", options.error().toString().c_str(), rv::spike::spikeHelp().c_str());
        return rv::app::toInt(ExitCode::InitializationFailed);
    }
    if (options->showHelp) {
        std::printf("%s", rv::spike::spikeHelp().c_str());
        return rv::app::toInt(ExitCode::Ok);
    }
    if (options->listEncoders) {
        std::printf("%s\nVideo encoders:", rv::media::ffmpegVersionInfo().c_str());
        for (const std::string& name : rv::media::availableVideoEncoders()) {
            std::printf(" %s", name.c_str());
        }
        std::printf("\n");
        return rv::app::toInt(ExitCode::Ok);
    }

    std::vector<std::filesystem::path> videos = options->videos;
    if (options->fixtureDirectory) {
        // Written before the window opens, logged to the console.
        auto fixtures =
            rv::spike::prepareFixtures(*options->fixtureDirectory, options->fixture, options->fixtureExtension,
                                       options->count, options->errorCases, options->regenerate);
        if (!fixtures) {
            std::fprintf(stderr, "--generate-fixtures failed: %s\n", fixtures.error().toString().c_str());
            return rv::app::toInt(ExitCode::InitializationFailed);
        }
        if (videos.empty()) {
            videos = options->errorCases ? fixtures->errorCases : fixtures->clips;
        }
    }
    if (!options->errorCases && options->countGiven && !videos.empty()) {
        // --count with --videos: the list repeated or cut to that many decoders.
        std::vector<std::filesystem::path> repeated;
        for (int index = 0; index < options->count; ++index) {
            repeated.push_back(videos[static_cast<std::size_t>(index) % videos.size()]);
        }
        videos = std::move(repeated);
    }

    rv::spike::VideoSpikeApp app(*options, std::move(videos));
    rv::app::AppHost host(options->host, kIdentity);
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
