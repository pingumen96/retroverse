// Schema area "launch": filled by I2 step 12 (per-game launch choices, launch verifications and play sessions, ADR-0008
// §5, §9). It has no steps until then; step 12 adds its tables to schema v1 here (schema.hpp), without editing the
// other areas.

#include "schema.hpp"

namespace rv::catalog_db::schema {

Area launchArea() {
    return Area{"launch", {}};
}

} // namespace rv::catalog_db::schema
