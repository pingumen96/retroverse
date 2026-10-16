# retroverse-cli (ADR-0008 §2, roadmap I2): the ES-DE catalog of milestone M1 from the command line, without a window:
# discovery, import, queries, media, emulator profiles, launches and play sessions. Results go to stdout, or to the file
# a command's --output names, as UTF-8; logs go to stderr and to <user data>/logs/retroverse-cli.log, never to stdout.

# GPU-free core, built in every configuration and unit-tested by tests/unit/cli: the command table and the dispatcher
# (global options, help, exit codes), the output contract (Output, ConsoleSession) and one source per command
# (cmd_<name>.cpp; each names the I2 step that implements it and is a stub until then).
add_library(rv_cli_core STATIC
    src/cmd_assign.cpp
    src/cmd_db.cpp
    src/cmd_discover.cpp
    src/cmd_games.cpp
    src/cmd_identities.cpp
    src/cmd_import.cpp
    src/cmd_launch.cpp
    src/cmd_library.cpp
    src/cmd_media.cpp
    src/cmd_profiles.cpp
    src/cmd_search.cpp
    src/cmd_sessions.cpp
    src/cmd_show.cpp
    src/cmd_systems.cpp
    src/command.cpp
    src/output.cpp
)
add_library(rv::cli_core ALIAS rv_cli_core)
rv_set_target_defaults(rv_cli_core)
target_include_directories(rv_cli_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The public headers need rv_core only; the commands use the catalog, its adapters and the Windows services.
target_link_libraries(rv_cli_core
    PUBLIC rv_core
    PRIVATE rv_catalog rv_esde rv_catalog_db rv_platform_win rv_launcher)
rv_forbid_link(rv_cli_core)

add_executable(retroverse-cli src/main.cpp)
rv_set_target_defaults(retroverse-cli)
rv_enable_long_paths(retroverse-cli)
target_link_libraries(retroverse-cli PRIVATE rv_cli_core)
# The shipped defaults (assets/launcher) are read from the asset root, deployed next to the executable in Release.
rv_deploy_assets(retroverse-cli)
