// Schema area "search": filled by I2 step 9 (the FTS5 index over idx_games and what the queries need, ADR-0008 §5). It
// has no steps until then; step 9 adds its tables to schema v1 here (schema.hpp), without editing the other areas.

#include "schema.hpp"

namespace rv::catalog_db::schema {

Area searchArea() {
    return Area{"search", {}};
}

} // namespace rv::catalog_db::schema
