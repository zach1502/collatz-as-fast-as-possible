# Test of collatz_git_commit_stamp() (cmake/GitCommitStamp.cmake) through real
# builds of a small project in a scratch git repository, with its build directory
# outside the source directory. Invoked by ctest as
#   cmake -DGIT_EXECUTABLE=<git> -DMODULE_DIR=<cmake dir> -DWORK=<dir>
#         -DGENERATOR=<generator> -DMAKE_PROGRAM=<make> -DCXX=<compiler> -P git_commit_stamp_build.cmake
#
# The program prints COLLATZ_GIT_COMMIT. Without a reconfigure, each build must
# print the commit of the tree it was built from: <sha> when clean, <sha>-dirty
# after an edit, <sha> again after `git checkout`, <sha> after a touch, and the new
# commit after a commit. A build that leaves the value unchanged recompiles nothing.

cmake_minimum_required(VERSION 3.25)
foreach(_v MODULE_DIR WORK GENERATOR CXX)
    if(NOT ${_v})
        message(FATAL_ERROR "need -D${_v}=...")
    endif()
endforeach()
if(NOT GIT_EXECUTABLE)
    message("git is not available; skipped")
    return()
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/no-hooks")
set(P "${WORK}/project")
set(BIN "${WORK}/build")

# Runs git in the project without the user's hooks, signing or identity.
function(g)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${P}"
            -c "core.hooksPath=${WORK}/no-hooks" -c commit.gpgsign=false -c core.autocrlf=false
            -c user.name=stamp-test -c user.email=stamp-test@example.invalid ${ARGN}
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed (${_rc}): ${_err}")
    endif()
    set(GIT_OUT "${_out}" PARENT_SCOPE)
endfunction()

# Builds the project and runs the program; fails unless it prints <expected>.
# Sets BUILD_LOG to the build's output.
function(build_expect what expected)
    # Some make programs compare whole seconds, so a header
    # rewritten in the second of the previous build would look up to date to them.
    if(GENERATOR MATCHES "Makefiles")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BIN}" --config Release
        OUTPUT_VARIABLE _log ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${what}: build failed (${_rc}):\n${_log}${_err}")
    endif()
    set(_exe "")
    foreach(_c "${WORK}/bin/probe.exe" "${WORK}/bin/Release/probe.exe")
        if(EXISTS "${_c}" AND NOT _exe)
            set(_exe "${_c}")
        endif()
    endforeach()
    execute_process(COMMAND "${_exe}" OUTPUT_VARIABLE _got RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0 OR NOT _got STREQUAL expected)
        message(FATAL_ERROR "${what}: expected '${expected}', the program printed '${_got}' (rc ${_rc})")
    endif()
    set(BUILD_LOG "${_log}" PARENT_SCOPE)
    message(STATUS "ok: ${what}: ${_got}")
endfunction()

file(WRITE "${P}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(stamp_probe LANGUAGES CXX)\n"
    "list(APPEND CMAKE_MODULE_PATH \"${MODULE_DIR}\")\n"
    "include(GitCommitStamp)\n"
    "add_executable(probe src/main.cpp src/other.cpp)\n"
    "collatz_git_commit_stamp(probe)\n")
file(WRITE "${P}/src/main.cpp"
    "#include <cstdio>\nint other();\nint main() { std::puts(COLLATZ_GIT_COMMIT); return other(); }\n")
file(WRITE "${P}/src/other.cpp" "int other() { return 0; }\n")
g(init -q -b main)
g(add -A)
g(commit -q --no-verify -m one)
g(rev-parse HEAD)
set(SHA "${GIT_OUT}")

set(_args -S "${P}" -B "${BIN}" -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX}"
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${WORK}/bin")
if(MAKE_PROGRAM)
    list(APPEND _args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${_args}
    OUTPUT_VARIABLE _log ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "configure failed (${_rc}):\n${_log}${_err}")
endif()

build_expect("first build" "${SHA}")
build_expect("no-op build" "${SHA}")
if(BUILD_LOG MATCHES "Building CXX")
    message(FATAL_ERROR "a build with an unchanged value recompiled:\n${BUILD_LOG}")
endif()
file(APPEND "${P}/src/other.cpp" "// edit\n")
build_expect("edited source" "${SHA}-dirty")
g(checkout -q -- src/other.cpp)
build_expect("after git checkout" "${SHA}")
file(TOUCH "${P}/src/other.cpp")
build_expect("after a touch" "${SHA}")
if(BUILD_LOG MATCHES "main\\.cpp")
    message(FATAL_ERROR "a touch of other.cpp changed the value and recompiled main.cpp:\n${BUILD_LOG}")
endif()
file(APPEND "${P}/src/other.cpp" "// second\n")
g(commit -q --no-verify -a -m two)
g(rev-parse HEAD)
build_expect("after a commit" "${GIT_OUT}")

file(REMOVE_RECURSE "${WORK}")
message(STATUS "all git commit stamp build cases passed")
