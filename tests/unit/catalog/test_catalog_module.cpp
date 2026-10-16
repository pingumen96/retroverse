// I2 step 0a: rv_catalog is linked into rv_tests and every planned public header is reachable. The steps that fill the
// headers add their tests beside this one.

#include <catch2/catch_test_macros.hpp>

#include <rv/catalog/catalog_query.hpp>
#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/change_set.hpp>
#include <rv/catalog/coverage.hpp>
#include <rv/catalog/disc_group.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/fingerprint.hpp>
#include <rv/catalog/identity.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/system_probe.hpp>

TEST_CASE("catalog/module: the skeleton links into rv_tests with every planned header", "[catalog]") {
    SUCCEED("rv_catalog and the system probe port of rv_core are reachable");
}
