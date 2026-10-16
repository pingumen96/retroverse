// I2 step 0a: rv_catalog_db is linked into rv_tests and every planned public header is reachable (SQLite stays private
// to the module). The steps that fill the headers add their tests beside this one.

#include <catch2/catch_test_macros.hpp>

#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/session_store.hpp>

TEST_CASE("catalog_db/module: the skeleton links into rv_tests with every planned header", "[catalog_db]") {
    SUCCEED("rv_catalog_db is reachable");
}
