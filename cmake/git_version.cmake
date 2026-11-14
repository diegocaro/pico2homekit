# Runs on every build: writes ${OUT} with GIT_VERSION, the short commit id, plus
# "-dirty" if there were uncommitted changes. The file is only rewritten when the
# value changes, so an unchanged tree does not recompile anything.
execute_process(
    COMMAND "${GIT}" describe --always --dirty --abbrev=7
    WORKING_DIRECTORY "${SRC}"
    OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if (NOT version)
    set(version "unknown")
endif()
set(content "#define GIT_VERSION \"${version}\"\n")
if (EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if (NOT "${old}" STREQUAL "${content}")
    file(WRITE "${OUT}" "${content}")
endif()
