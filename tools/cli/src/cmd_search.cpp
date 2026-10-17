// retroverse-cli search: searches games through the FTS5 index (I2 step 9, ADR-0008 §5): `search <text>` with the
// options of `games` (runGameList(), cmd_games.cpp). The text is split into words by the index's own tokenizer; every
// word must match a whole word of the title, sort title, developer, publisher, genre, year or system name, the last one
// a prefix, case and diacritics ignored; one character finds the titles that start with it; text is never FTS5 syntax.

#include "catalog_text.hpp"
#include "commands.hpp"

namespace rv::cli {

ExitCode runSearch(CommandContext& context) {
    return runGameList(context, true);
}

} // namespace rv::cli
