// I2 step 0a: rv_esde is linked into rv_tests and every planned public header is reachable (pugixml stays private to
// the module). The steps that fill the headers add their tests beside this one.

#include <catch2/catch_test_macros.hpp>

#include <rv/esde/command_template.hpp>
#include <rv/esde/disc_groups.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/mame_sets.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/rom_scanner.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

TEST_CASE("esde/module: the skeleton links into rv_tests with every planned header", "[esde]") {
    SUCCEED("rv_esde is reachable");
}
