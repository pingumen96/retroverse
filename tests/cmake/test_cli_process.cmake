# ctest unit.cmake/cli_process: retroverse-cli run as a process, where its output contract (ADR-0008 §2) meets wmain,
# the ConsoleSession and the exit codes, which the unit tests of rv_cli_core never see: results on stdout as UTF-8 with
# '\n' line ends, usage errors and logs on stderr and never on stdout, the log file in <user data>/logs. Only what every
# command keeps once its I2 step implements it is checked (an unknown option is a usage error), so this test stays as it
# is while the commands are filled in.
#
#   cmake -DCLI=<retroverse-cli.exe> -DWORK_DIR=<scratch directory, emptied> -P test_cli_process.cmake

cmake_minimum_required(VERSION 3.28)

foreach(variable IN ITEMS CLI WORK_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "test_cli_process.cmake requires -D${variable}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Fails, showing what the last run wrote.
function(fail what)
    message(FATAL_ERROR "${ran}: ${what}\n--- exit code: ${code}\n--- stdout:\n${out}\n--- stderr:\n${err}")
endfunction()

# Runs retroverse-cli with the arguments given. Sets, in the caller's scope, `code`, `out` and `err` (stdout and stderr
# apart) and `ran` (the command line, for messages). The streams go through files: OUTPUT_VARIABLE, like file(READ)
# without HEX on Windows, loses the carriage returns, so only the bytes show the line ends stdout really has.
function(run_cli)
    set(stdout_file "${WORK_DIR}/stdout.bin")
    set(stderr_file "${WORK_DIR}/stderr.bin")
    execute_process(COMMAND "${CLI}" ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE code OUTPUT_FILE "${stdout_file}" ERROR_FILE "${stderr_file}")
    file(READ "${stdout_file}" out)
    file(READ "${stderr_file}" err)
    list(JOIN ARGN " " arguments)
    set(ran "retroverse-cli ${arguments}")
    foreach(variable IN ITEMS code out err ran)
        set(${variable} "${${variable}}" PARENT_SCOPE)
    endforeach()
    # Results keep '\n' line ends (stdout is binary while the program runs): no byte 0D on stdout.
    file(READ "${stdout_file}" stdout_hex HEX)
    string(REGEX MATCHALL ".." stdout_bytes "${stdout_hex}")
    if("0d" IN_LIST stdout_bytes)
        fail("stdout has a carriage return: results must keep '\\n' line ends")
    endif()
endfunction()

function(expect_code expected)
    if(NOT code STREQUAL expected)
        fail("exit code ${code}, expected ${expected}")
    endif()
endfunction()

# `stream` is out or err.
function(expect_empty stream)
    if(NOT "${${stream}}" STREQUAL "")
        fail("${stream} is not empty")
    endif()
endfunction()

function(expect_contains stream text)
    string(FIND "${${stream}}" "${text}" at)
    if(at EQUAL -1)
        fail("${stream} lacks '${text}'")
    endif()
endfunction()

# --version: the banner alone on stdout, ended by '\n'.
run_cli(--version)
expect_code(0)
expect_empty(err)
if(NOT out MATCHES "^Retroverse [0-9]+\\.[0-9]+\\.[0-9]+ \\([^\n]+\\)\n$")
    fail("stdout is not the version banner followed by '\\n'")
endif()

# --help: the help text on stdout.
run_cli(--help)
expect_code(0)
expect_empty(err)
if(NOT out MATCHES "^Usage: retroverse-cli ")
    fail("stdout does not start with the usage line")
endif()
expect_contains(out "\nCommands:\n")

# The commands as --help lists them: "  <name>  <summary>" lines from "Commands:" to the next blank line.
string(FIND "${out}" "\nCommands:\n" start)
string(SUBSTRING "${out}" ${start} -1 table)
string(FIND "${table}" "\n\n" end)
string(SUBSTRING "${table}" 0 ${end} table)
string(REGEX MATCHALL "\n  [a-z][a-z-]*" commands "${table}")
list(TRANSFORM commands STRIP)
if(NOT "discover" IN_LIST commands OR NOT "sessions" IN_LIST commands)
    fail("the command table could not be read from stdout (found '${commands}')")
endif()

# No command: the help text on stderr.
run_cli()
expect_code(2)
expect_empty(out)
if(NOT err MATCHES "^Usage: retroverse-cli ")
    fail("stderr does not start with the usage line")
endif()

# An unknown command named outside ASCII: wmain takes the arguments as UTF-16, stderr names it in UTF-8.
run_cli(frobnicaté --now)
expect_code(2)
expect_empty(out)
expect_contains(err "retroverse-cli: unknown command 'frobnicaté'")

# Every command, stub or implemented, given an unknown option with every log level shown on the console: exit 2,
# nothing on stdout, the dispatcher's log lines on stderr and in the log file of a user data directory named outside
# ASCII.
set(data "${WORK_DIR}/dàta")
foreach(command IN LISTS commands)
    run_cli(--data-dir "${data}" --log-level trace ${command} --no-such-option)
    expect_code(2)
    expect_empty(out)
    expect_contains(err "retroverse-cli ${command}: exit code 2")
    if(NOT EXISTS "${data}/logs/retroverse-cli.log")
        fail("no log file in ${data}/logs")
    endif()
    file(READ "${data}/logs/retroverse-cli.log" log)
    string(FIND "${log}" "retroverse-cli ${command}: exit code 2" at)
    if(at EQUAL -1)
        fail("the log file lacks the exit code of '${command}'")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
list(LENGTH commands count)
message(STATUS "retroverse-cli keeps its output contract as a process (${count} commands)")
