#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>

#include "support/temp_dir.hpp"

using rv::cli::Output;

TEST_CASE("cli/output: results are UTF-8 text with '\\n' line ends; a file output is written by finish()", "[cli]") {
    Output memory = Output::memory();
    CHECK(memory.target() == Output::Target::Memory);
    memory.write("Pok\xC3\xA9mon ");
    memory.writeLine("\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3"); // katakana, outside the ANSI code pages
    memory.writeLine();
    CHECK(memory.text() == "Pok\xC3\xA9mon \xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3\n\n");
    CHECK(memory.finish().has_value());

    const rv::test::TempDir dir("rv-cli-output");
    const std::filesystem::path path = dir.file("results/games.txt");
    Output file = Output::file(path);
    CHECK(file.target() == Output::Target::File);
    CHECK(file.path() == path);
    file.writeLine("Pok\xC3\xA9mon");
    file.writeLine("second line");
    // Nothing reaches the disk before finish(): a failed command leaves no partial file.
    CHECK_FALSE(std::filesystem::exists(path));
    REQUIRE(file.finish().has_value());
    const auto bytes = rv::core::readTextFile(path);
    REQUIRE(bytes.has_value());
    CHECK(*bytes == "Pok\xC3\xA9mon\nsecond line\n");

    // The standard streams keep nothing.
    CHECK(Output::standardOutput().target() == Output::Target::Stdout);
    CHECK(Output::standardError().target() == Output::Target::Stderr);
    CHECK(Output::standardOutput().text().empty());
}

TEST_CASE("cli/output: --output names a file output; without it the results go to stdout", "[cli]") {
    rv::core::ArgParser parser("retroverse-cli test");
    rv::cli::addOutputOption(parser);

    const auto given = parser.parse(std::vector<std::string>{"--output", "results/out.txt"});
    REQUIRE(given.has_value());
    auto file = rv::cli::outputFileOption(*given);
    REQUIRE(file.has_value());
    REQUIRE(file->has_value());
    CHECK((*file)->target() == Output::Target::File);
    CHECK((*file)->path() == std::filesystem::path("results/out.txt"));

    const auto absent = parser.parse(std::vector<std::string>{});
    REQUIRE(absent.has_value());
    const auto none = rv::cli::outputFileOption(*absent);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->has_value());

    const auto empty = parser.parse(std::vector<std::string>{"--output="});
    REQUIRE(empty.has_value());
    const auto refused = rv::cli::outputFileOption(*empty);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(refused.error().context == "--output");
}

TEST_CASE("cli/output: a console session gives the console its code page back", "[cli]") {
    // Under ctest stdout and stderr are pipes and nothing changes; run from a console, the session switches to UTF-8.
    const UINT before = GetConsoleOutputCP();
    {
        const rv::cli::ConsoleSession session;
        if (session.switchedCodePage()) {
            CHECK(GetConsoleOutputCP() == CP_UTF8);
        }
    }
    CHECK(GetConsoleOutputCP() == before);
}
