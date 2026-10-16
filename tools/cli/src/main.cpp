// retroverse-cli: the ES-DE catalog of milestone M1 from the command line (ADR-0008 §2). The dispatcher, the commands
// and the output contract live in rv_cli_core (rv/cli/command.hpp, rv/cli/output.hpp).

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

namespace {

int runCli(const std::vector<std::string>& arguments) {
    rv::cli::Output out = rv::cli::Output::standardOutput();
    rv::cli::Output err = rv::cli::Output::standardError();
    rv::cli::ExitCode code = rv::cli::run(arguments, out, err, rv::core::PathEnvironment::fromProcess());
    // Results that could not be written (a closed pipe, a full disk) fail a run that would otherwise succeed.
    if (const auto finished = out.finish(); !finished) {
        std::fprintf(stderr, "retroverse-cli: %s\n", finished.error().toString().c_str());
        if (code == rv::cli::ExitCode::Ok) {
            code = rv::cli::ExitCode::RuntimeFailure;
        }
    }
    static_cast<void>(err.finish());
    return rv::cli::toInt(code);
}

} // namespace

// wmain: the arguments arrive as UTF-16 and are converted to UTF-8, so paths outside the ANSI code page work.
int wmain(int argc, wchar_t* argv[]) {
    const rv::cli::ConsoleSession console;
    rv::core::markMainThread();
    std::vector<std::string> arguments;
    try {
        for (int i = 1; i < argc; ++i) {
            arguments.push_back(rv::core::pathToUtf8(std::filesystem::path(argv[i])));
        }
    } catch (const std::exception&) {
        // pathToUtf8 throws on unpaired UTF-16 surrogates.
        std::fprintf(stderr, "retroverse-cli: an argument is not valid Unicode text\n");
        return rv::cli::toInt(rv::cli::ExitCode::UsageError);
    }
    try {
        return runCli(arguments);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "retroverse-cli: unexpected failure: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "retroverse-cli: unexpected failure: unknown exception\n");
    }
    return rv::cli::toInt(rv::cli::ExitCode::RuntimeFailure);
}
