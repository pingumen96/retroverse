#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <rv/core/arg_parser.hpp>

using rv::core::ArgParser;

namespace {

ArgParser makeParser() {
    ArgParser parser("retroverse", "test parser");
    parser.flag("smoke", "Render a few frames and exit")
        .option("frames", "count", "Frames to render", "3")
        .option("scene", "name", "Scene to load")
        .option("scale", "factor", "Resolution scale")
        .positional("model", "Model file to preview");
    return parser;
}

rv::Expected<rv::core::ParsedArgs> parse(const ArgParser& parser, std::vector<std::string> arguments) {
    return parser.parse(arguments);
}

} // namespace

TEST_CASE("core/arg_parser: flags, options and defaults", "[core][arg_parser]") {
    const ArgParser parser = makeParser();

    const auto args = parse(parser, {"--smoke", "--scene", "m0_testroom", "--scale=1.5"});
    REQUIRE(args.has_value());
    CHECK(args->has("smoke"));
    CHECK(args->value("scene") == "m0_testroom");
    CHECK(args->doubleValue("scale", 1.0).value() == 1.5);
    CHECK_FALSE(args->has("frames"));
    CHECK(args->value("frames") == "3"); // default value
    CHECK(args->intValue("frames", 0).value() == 3);
    CHECK_FALSE(args->positional("model").has_value());
}

TEST_CASE("core/arg_parser: positionals and the -- separator", "[core][arg_parser]") {
    const ArgParser parser = makeParser();
    const auto args = parse(parser, {"--frames", "10", "crt.glb"});
    REQUIRE(args.has_value());
    CHECK(args->intValue("frames", 0).value() == 10);
    CHECK(args->positional("model") == "crt.glb");

    const auto dashed = parse(parser, {"--", "--weird-name.glb"});
    REQUIRE(dashed.has_value());
    CHECK(dashed->positional("model") == "--weird-name.glb");
}

TEST_CASE("core/arg_parser: errors", "[core][arg_parser]") {
    const ArgParser parser = makeParser();
    CHECK_FALSE(parse(parser, {"--unknown"}).has_value());
    CHECK_FALSE(parse(parser, {"--scene"}).has_value());          // missing value
    CHECK_FALSE(parse(parser, {"--smoke=yes"}).has_value());      // flags take no value
    CHECK_FALSE(parse(parser, {"a.glb", "b.glb"}).has_value());   // surplus positional
    CHECK_FALSE(parse(parser, {"-x"}).has_value());               // single-dash options are not supported

    const auto badInt = parse(parser, {"--frames", "ten"});
    REQUIRE(badInt.has_value());
    CHECK_FALSE(badInt->intValue("frames", 0).has_value());
}

TEST_CASE("core/arg_parser: help", "[core][arg_parser]") {
    ArgParser parser = makeParser();
    parser.positional("required-thing", "must be given", true);
    const auto args = parse(parser, {"--help"});
    REQUIRE(args.has_value()); // --help suppresses the required-positional check
    CHECK(args->has("help"));
    CHECK_FALSE(parse(parser, {}).has_value());

    const std::string help = parser.helpText();
    CHECK(help.find("--frames <count>") != std::string::npos);
    CHECK(help.find("(default: 3)") != std::string::npos);
}
